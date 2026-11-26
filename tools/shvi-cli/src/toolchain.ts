// External tools: cmake, MSVC (Visual Studio), WSL and the ps5-payload-sdk.
import { existsSync } from "node:fs";
import { homedir, platform } from "node:os";
import { join } from "node:path";
import { CliError, c, log } from "./ui";

export interface RunOpts {
  cwd?: string;
  env?: Record<string, string | undefined>;
  quiet?: boolean; // capture output, print it only on failure
  allowFail?: boolean;
}

export interface RunResult {
  code: number;
  out: string;
}

export async function run(cmd: string, args: string[], o: RunOpts = {}): Promise<RunResult> {
  if (!o.quiet) log.info(c.dim(`$ ${[cmd, ...args].map(quote).join(" ")}`));
  let proc;
  try {
    proc = Bun.spawn([cmd, ...args], {
      cwd: o.cwd,
      env: { ...process.env, ...o.env },
      stdout: o.quiet ? "pipe" : "inherit",
      stderr: o.quiet ? "pipe" : "inherit",
      stdin: "ignore",
    });
  } catch (e) {
    if (o.allowFail) return { code: 127, out: String(e) };
    throw new CliError(`cannot run ${cmd}: ${(e as Error).message}`);
  }
  const out = o.quiet ? (await new Response(proc.stdout as ReadableStream).text()) + (await new Response(proc.stderr as ReadableStream).text()) : "";
  const code = await proc.exited;
  if (code !== 0 && !o.allowFail) {
    if (o.quiet && out) console.error(out);
    throw new CliError(`${cmd} failed (exit ${code})`);
  }
  return { code, out };
}

function quote(a: string): string {
  return /[\s"']/.test(a) ? JSON.stringify(a) : a;
}

export const isWindows = platform() === "win32";

// cmake on PATH, else the one Visual Studio ships (found with vswhere).
export async function findCmake(): Promise<string | null> {
  const onPath = Bun.which("cmake");
  if (onPath) return onPath;
  if (!isWindows) return null;
  const vs = await findVisualStudio();
  if (!vs) return null;
  const p = join(vs, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe");
  return existsSync(p) ? p : null;
}

export async function findVisualStudio(): Promise<string | null> {
  if (!isWindows) return null;
  const vswhere = join(process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)", "Microsoft Visual Studio", "Installer", "vswhere.exe");
  if (!existsSync(vswhere)) return null;
  const r = await run(
    vswhere,
    ["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
    { quiet: true, allowFail: true },
  );
  const path = r.out.trim().split(/\r?\n/)[0];
  return r.code === 0 && path ? path : null;
}

export async function requireCmake(): Promise<string> {
  const p = await findCmake();
  if (!p) throw new CliError("cmake not found: install CMake >= 3.24 (or Visual Studio with C++ CMake tools)");
  return p;
}

// ---- PS5 toolchain
export function ps5SdkDir(): string | null {
  const candidates = [process.env.PS5_PAYLOAD_SDK, join(homedir(), ".shvi", "ps5-payload-sdk"), "/opt/ps5-payload-sdk"];
  for (const d of candidates) if (d && existsSync(join(d, "bin"))) return d;
  return null;
}

export const PS5_SDK_VERSION = "v0.50";
export const PS5_SDK_URL = `https://github.com/ps5-payload-dev/sdk/releases/download/${PS5_SDK_VERSION}/ps5-payload-sdk.zip`;

// ---- WSL (PS5 builds from Windows)
export async function hasWsl(): Promise<boolean> {
  if (!isWindows || !Bun.which("wsl.exe")) return false;
  const r = await run("wsl.exe", ["-e", "true"], { quiet: true, allowFail: true });
  return r.code === 0;
}

// C:\Users\me\mod -> /mnt/c/Users/me/mod (WSL's default automount).
export function toWslPath(p: string): string {
  const m = /^([A-Za-z]):[\\/](.*)$/.exec(p);
  if (!m) return p.replace(/\\/g, "/");
  return `/mnt/${m[1].toLowerCase()}/${m[2].replace(/\\/g, "/")}`;
}
