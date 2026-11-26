// shvi deploy: put a built mod where it runs.
//   pc:  copy <id>.asi into <game>/shvi/plugins/                      (--game <folder>)
//   ps5: FTP the mod image to /data/shvi/mods/, then send shvi_loader.elf to the ELF loader
//        (--host <console ip>); --dry-run sends the mod image itself instead (runs outside the game).
// Every console action is confirmed first unless --yes.
import { copyFileSync, existsSync, mkdirSync, readFileSync } from "node:fs";
import { Socket, connect } from "node:net";
import { basename, join } from "node:path";
import type { Args } from "./args";
import { findProject } from "./project";
import { artifacts } from "./cmd_build";
import { requireSdk } from "./sdk";
import { CliError, c, confirm, log } from "./ui";

export async function cmdDeploy(a: Args): Promise<void> {
  const project = findProject(a.opt("project") ?? process.cwd());
  const target = a.opt("target") ?? a.positional[0] ?? (a.opt("host") ? "ps5" : a.opt("game") ? "pc" : "");
  const sdk = await requireSdk({ flag: a.opt("sdk"), projectDir: project.dir, manifestPath: project.manifest.sdk.path, version: project.manifest.sdk.version });
  const ctx = { project, sdk, config: a.flag("debug") ? "Debug" : "Release" };

  if (target === "pc") {
    const game = a.opt("game");
    if (!game) throw new CliError("shvi deploy pc --game <game folder>");
    const { mod } = artifacts(ctx, "pc");
    if (!existsSync(mod!)) throw new CliError(`${mod} not built (shvi build --target pc)`);
    const dest = join(game, "shvi", "plugins");
    mkdirSync(dest, { recursive: true });
    copyFileSync(mod!, join(dest, basename(mod!)));
    log.ok(`copied ${basename(mod!)} to ${dest}`);
    return;
  }
  if (target !== "ps5") throw new CliError("shvi deploy pc --game <folder>  |  shvi deploy ps5 --host <console ip>");

  const host = a.opt("host");
  if (!host) throw new CliError("shvi deploy ps5 --host <console ip> [--ftp-port 2121] [--loader-port 9021] [--dry-run]");
  const ftpPort = Number(a.opt("ftp-port") ?? 2121);
  const loaderPort = Number(a.opt("loader-port") ?? 9021);
  const { mod, loader } = artifacts(ctx, "ps5");
  for (const f of [mod!, loader!]) if (!existsSync(f)) throw new CliError(`${f} not built (shvi build --target ps5)`);
  const dry = a.flag("dry-run");
  const send = dry ? mod! : loader!;

  log.info(`${c.bold("PS5 deploy")} to ${host}:
  1. FTP ${basename(mod!)} -> /data/shvi/mods/ (port ${ftpPort})${dry ? c.dim("  (skipped: dry run)") : ""}
  2. send ${basename(send)} to the ELF loader (port ${loaderPort})${dry ? "  — dry run: the mod runs in its own process, no game" : "  — start GTA VI in story mode, offline, first"}`);
  if (!a.flag("yes") && !(await confirm("Run these console actions now?", false))) {
    log.warn("cancelled");
    return;
  }
  if (!dry) {
    await ftpUpload(host, ftpPort, readFileSync(mod!), "/data/shvi/mods", basename(mod!));
    log.ok(`uploaded /data/shvi/mods/${basename(mod!)}`);
  }
  await sendElf(host, loaderPort, readFileSync(send));
  log.ok(`sent ${basename(send)} (${readFileSync(send).length} bytes); watch the console for the shvi notification`);
}

// Sends an ELF to elfldr: it reads the whole stream until the connection closes.
export function sendElf(host: string, port: number, data: Uint8Array): Promise<void> {
  return new Promise((resolve, reject) => {
    const s = connect({ host, port }, () => s.end(data));
    s.setTimeout(15000, () => s.destroy(new Error(`timeout talking to ${host}:${port}`)));
    s.on("close", (hadError) => (hadError ? undefined : resolve()));
    s.on("error", (e) => reject(new CliError(`ELF loader ${host}:${port}: ${e.message}`)));
  });
}

// Minimal passive-mode FTP client: USER/PASS anonymous, MKD (errors ignored), TYPE I, PASV, STOR.
export async function ftpUpload(host: string, port: number, data: Uint8Array, dir: string, name: string): Promise<void> {
  const ctl = await open(host, port);
  const lines = lineReader(ctl);
  const expect = async (ok: (code: number) => boolean, what: string) => {
    const r = await lines.reply();
    if (!ok(r.code)) throw new CliError(`FTP ${what}: ${r.code} ${r.text}`);
    return r;
  };
  const cmd = (s: string) => ctl.write(s + "\r\n");
  try {
    await expect((n) => n === 220, "greeting");
    cmd("USER anonymous");
    const u = await lines.reply();
    if (u.code === 331) {
      cmd("PASS shvi@");
      await expect((n) => n === 230 || n === 202, "login");
    } else if (u.code !== 230) throw new CliError(`FTP login: ${u.code} ${u.text}`);
    cmd("TYPE I");
    await expect((n) => n === 200, "TYPE I");
    let path = "";
    for (const part of dir.split("/").filter(Boolean)) {
      path += "/" + part;
      cmd(`MKD ${path}`);
      await lines.reply(); // 257 created or 550 exists: both fine
    }
    cmd("PASV");
    const p = await expect((n) => n === 227, "PASV");
    const m = /(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)/.exec(p.text);
    if (!m) throw new CliError(`FTP PASV reply not understood: ${p.text}`);
    const dataPort = Number(m[5]) * 256 + Number(m[6]);
    const dsock = await open(host, dataPort); // the console's own address, not the one in the reply (NAT-safe)
    cmd(`STOR ${dir}/${name}`);
    await expect((n) => n === 150 || n === 125, "STOR");
    await new Promise<void>((res, rej) => {
      dsock.on("error", rej);
      dsock.end(data, () => res());
    });
    await expect((n) => n === 226 || n === 250, "transfer");
    cmd("QUIT");
  } finally {
    ctl.destroy();
  }
}

function open(host: string, port: number): Promise<Socket> {
  return new Promise((resolve, reject) => {
    const s = connect({ host, port }, () => resolve(s));
    s.setTimeout(15000, () => s.destroy(new Error("timeout")));
    s.on("error", (e) => reject(new CliError(`connect ${host}:${port}: ${e.message}`)));
  });
}

function lineReader(s: Socket) {
  let buf = "";
  const waiting: ((l: string) => void)[] = [];
  const ready: string[] = [];
  s.on("data", (d) => {
    buf += d.toString("latin1");
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const l = buf.slice(0, i).replace(/\r$/, "");
      buf = buf.slice(i + 1);
      const w = waiting.shift();
      if (w) w(l);
      else ready.push(l);
    }
  });
  const next = () => new Promise<string>((r) => (ready.length ? r(ready.shift()!) : waiting.push(r)));
  return {
    // One reply; multi-line replies ("123-...") end with "123 ...".
    async reply(): Promise<{ code: number; text: string }> {
      let l = await next();
      const code = Number(l.slice(0, 3));
      if (l[3] === "-") while (!(l.startsWith(String(code)) && l[3] === " ")) l = await next();
      return { code, text: l.slice(4) };
    },
  };
}
