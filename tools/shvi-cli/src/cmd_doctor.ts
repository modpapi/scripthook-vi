// shvi doctor: what can this machine build, and how to fix what it cannot.
// shvi doctor --install-ps5-sdk: fetch ps5-payload-sdk into ~/.shvi/ps5-payload-sdk (Linux, or WSL from Windows).
// shvi sdk [path|install [version]]
import { mkdirSync, rmSync } from "node:fs";
import { join } from "node:path";
import type { Args } from "./args";
import { findProject } from "./project";
import { SHVI_HOME, installSdk, locateSdk } from "./sdk";
import {
  PS5_SDK_URL,
  PS5_SDK_VERSION,
  findCmake,
  findVisualStudio,
  hasWsl,
  isWindows,
  ps5SdkDir,
  run,
  toWslPath,
} from "./toolchain";
import { CliError, c, log } from "./ui";
import { VERSION } from "./version";

type Row = [ok: boolean | null, what: string, detail: string, fix?: string];

async function ps5Probe(): Promise<{ ok: boolean; detail: string }> {
  const probe =
    'for d in "$PS5_PAYLOAD_SDK" "$HOME/.shvi/ps5-payload-sdk" /opt/ps5-payload-sdk; do [ -n "$d" ] && [ -d "$d/bin" ] && echo "$d" && exit 0; done; exit 1';
  if (isWindows) {
    if (!(await hasWsl())) return { ok: false, detail: "no working WSL" };
    const r = await run("wsl.exe", ["-e", "bash", "-lc", probe], { quiet: true, allowFail: true });
    return r.code === 0 ? { ok: true, detail: `WSL: ${r.out.trim()}` } : { ok: false, detail: "not installed in WSL" };
  }
  const d = ps5SdkDir();
  return d ? { ok: true, detail: d } : { ok: false, detail: "not found" };
}

export async function cmdDoctor(a: Args): Promise<void> {
  if (a.flag("install-ps5-sdk")) return installPs5Sdk();
  const rows: Row[] = [];
  rows.push([true, "shvi", `${VERSION} on bun ${Bun.version} (${process.platform})`]);

  const cmake = await findCmake();
  const cv = cmake ? (await run(cmake, ["--version"], { quiet: true, allowFail: true })).out.split(/\r?\n/)[0] : "";
  rows.push([!!cmake, "cmake", cmake ? `${cv} (${cmake})` : "not found", "install CMake >= 3.24 (Visual Studio includes one)"]);

  if (isWindows) {
    const vs = await findVisualStudio();
    rows.push([!!vs, "MSVC (target pc)", vs ?? "Visual Studio with C++ not found", "install Visual Studio 2022+ with 'Desktop development with C++'"]);
  } else {
    const cxx = Bun.which("c++") ?? Bun.which("clang++") ?? Bun.which("g++");
    rows.push([!!cxx, "C++ compiler (target sim)", cxx ?? "not found", "install clang or g++"]);
    rows.push([null, "target pc", "builds on Windows only (MSVC); CI does it on windows-latest"]);
  }

  const ps5 = await ps5Probe();
  rows.push([ps5.ok, `ps5-payload-sdk (target ps5)`, ps5.detail, `shvi doctor --install-ps5-sdk${isWindows ? "  (WSL Ubuntu: wsl --install -d Ubuntu-24.04)" : ""}`]);
  if (!isWindows && ps5.ok) {
    const clang = Bun.which("clang-18") ?? Bun.which("clang");
    rows.push([!!clang, "clang/lld for ps5-payload-sdk", clang ?? "not found", "sudo apt install clang-18 lld-18 cmake make"]);
  }

  let version = VERSION;
  let projectDir: string | undefined;
  let manifestPath: string | undefined;
  try {
    const p = findProject();
    projectDir = p.dir;
    version = p.manifest.sdk.version;
    manifestPath = p.manifest.sdk.path;
    rows.push([true, "project", `${p.manifest.name} (${p.manifest.id} ${p.manifest.version}), targets ${p.manifest.targets.join(", ")}`]);
  } catch {
    rows.push([null, "project", "none here (shvi new creates one)"]);
  }
  const sdk = locateSdk({ flag: a.opt("sdk"), projectDir, manifestPath, version });
  rows.push([!!sdk, `ScriptHook VI SDK ${version}`, sdk ? `${sdk.dir} (${sdk.how})` : "not installed", `shvi sdk install ${version}`]);

  for (const [ok, what, detail, fix] of rows) {
    const mark = ok === null ? c.dim("·") : ok ? c.green("✓") : c.red("✗");
    log.info(`${mark} ${c.bold(what.padEnd(30))} ${detail}`);
    if (ok === false && fix) log.info(`  ${" ".repeat(30)} ${c.yellow("fix:")} ${fix}`);
  }
}

async function installPs5Sdk(): Promise<void> {
  log.step(`downloading ps5-payload-sdk ${PS5_SDK_VERSION} (open source, GPLv3): ${PS5_SDK_URL}`);
  const res = await fetch(PS5_SDK_URL);
  if (!res.ok) throw new CliError(`download failed: HTTP ${res.status}`);
  mkdirSync(SHVI_HOME, { recursive: true });
  const zip = join(SHVI_HOME, "ps5-payload-sdk.zip");
  await Bun.write(zip, res);
  // The zip holds one folder, ps5-payload-sdk/. Unpack where the CLI looks: ~/.shvi/ps5-payload-sdk.
  const unpack = (home: string, zipPath: string) =>
    `mkdir -p "${home}" && cd "${home}" && rm -rf ps5-payload-sdk && ` +
    `(command -v unzip >/dev/null && unzip -oq "${zipPath}" || python3 -m zipfile -e "${zipPath}" .) && ` +
    `chmod +x ps5-payload-sdk/bin/* 2>/dev/null; test -d ps5-payload-sdk/bin && echo "$PWD/ps5-payload-sdk"`;
  let where: string;
  if (isWindows) {
    if (!(await hasWsl())) throw new CliError("PS5 builds on Windows run in WSL: wsl --install -d Ubuntu-24.04, then retry");
    const r = await run("wsl.exe", ["-e", "bash", "-lc", unpack("$HOME/.shvi", toWslPath(zip))], { quiet: true });
    where = `WSL:${r.out.trim()}`;
  } else {
    const r = await run("bash", ["-lc", unpack(SHVI_HOME, zip)], { quiet: true });
    where = r.out.trim();
  }
  rmSync(zip, { force: true });
  log.ok(`ps5-payload-sdk installed: ${where}`);
  log.info(c.dim("  it also needs clang-18 and lld-18 on the Linux side: sudo apt install clang-18 lld-18 cmake make"));
}

export async function cmdSdk(a: Args): Promise<void> {
  const sub = a.positional[0] ?? "path";
  if (sub === "install") {
    await installSdk(a.positional[1] ?? VERSION, a.flag("force"));
    return;
  }
  if (sub === "path") {
    let version = a.positional[1] ?? VERSION;
    let projectDir: string | undefined;
    let manifestPath: string | undefined;
    try {
      const p = findProject();
      projectDir = p.dir;
      manifestPath = p.manifest.sdk.path;
      version = a.positional[1] ?? p.manifest.sdk.version;
    } catch {}
    const hit = locateSdk({ flag: a.opt("sdk"), projectDir, manifestPath, version });
    if (!hit) throw new CliError(`SDK ${version} not found (shvi sdk install ${version})`);
    console.log(hit.dir);
    return;
  }
  throw new CliError("shvi sdk path [version] | shvi sdk install [version] [--force]");
}
