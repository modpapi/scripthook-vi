// Where the ScriptHook VI SDK is, and how to get one.
//
// Resolution order (first hit wins):
//   1. --sdk <path>
//   2. shvi.json "sdk.path" (relative to the project)
//   3. SHVI_SDK environment variable
//   4. a ScriptHook VI checkout above the project (mods developed inside the repo)
//   5. the checkout this CLI runs from (bun tools/shvi-cli/src/cli.ts)
//   6. ~/.shvi/sdk/<version>, downloaded from the GitHub release on first use
import { existsSync, mkdirSync, rmSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, join, relative, resolve } from "node:path";
import { CliError, log } from "./ui";
import { run } from "./toolchain";

export const SDK_REPO = process.env.SHVI_REPO || "modpapi/scripthook-vi";
export const SHVI_HOME = process.env.SHVI_HOME || join(homedir(), ".shvi");

export function isSdk(dir: string): boolean {
  return existsSync(join(dir, "sdk", "cmake", "ShviMod.cmake")) && existsSync(join(dir, "sdk", "include", "shvi", "shvi.h"));
}

function findUp(start: string): string | null {
  let dir = resolve(start);
  for (;;) {
    if (isSdk(dir)) return dir;
    const up = dirname(dir);
    if (up === dir) return null;
    dir = up;
  }
}

export interface SdkLookup {
  flag?: string;
  projectDir?: string;
  manifestPath?: string;
  version: string;
}

export function locateSdk(o: SdkLookup): { dir: string; how: string } | null {
  const tries: [string | undefined | null, string][] = [
    [o.flag && resolve(o.flag), "--sdk"],
    [o.manifestPath && o.projectDir && resolve(o.projectDir, o.manifestPath), "shvi.json sdk.path"],
    [process.env.SHVI_SDK && resolve(process.env.SHVI_SDK), "SHVI_SDK"],
    [o.projectDir && findUp(o.projectDir), "checkout above the project"],
    [findUp(import.meta.dir), "the checkout the CLI runs from"],
    [join(SHVI_HOME, "sdk", o.version), "~/.shvi/sdk"],
  ];
  for (const [dir, how] of tries) if (dir && isSdk(dir)) return { dir, how };
  return null;
}

export function sdkAssetName(version: string): string {
  return `shvi-sdk-${version}.tar.gz`;
}

// Downloads and unpacks the SDK release into ~/.shvi/sdk/<version>.
export async function installSdk(version: string, force = false): Promise<string> {
  const dest = join(SHVI_HOME, "sdk", version);
  if (isSdk(dest) && !force) {
    log.ok(`SDK ${version} already installed: ${dest}`);
    return dest;
  }
  // SHVI_RELEASES_URL: a mirror (or a local server in tests) laid out like GitHub's <base>/v<version>/<asset>.
  const base = process.env.SHVI_RELEASES_URL || `https://github.com/${SDK_REPO}/releases/download`;
  const url = `${base}/v${version}/${sdkAssetName(version)}`;
  log.step(`downloading ${url}`);
  const res = await fetch(url);
  if (!res.ok) throw new CliError(`SDK download failed: HTTP ${res.status} for ${url}\n  (is v${version} released? use --sdk <checkout> meanwhile)`);
  const tmp = join(SHVI_HOME, "tmp");
  mkdirSync(tmp, { recursive: true });
  const archive = join(tmp, sdkAssetName(version));
  await Bun.write(archive, res);
  rmSync(dest, { recursive: true, force: true });
  mkdirSync(dest, { recursive: true });
  // bsdtar (Windows 10+, macOS) and GNU tar both read .tar.gz. On Windows use the system bsdtar: a GNU tar
  // from Git or MSYS on PATH reads "C:\..." as host:path. Relative names (cwd = the archive's folder) avoid
  // drive letters in the arguments entirely.
  const sysTar = join(process.env.SystemRoot || "C:\\Windows", "System32", "tar.exe");
  const tar = process.platform === "win32" && existsSync(sysTar) ? sysTar : "tar";
  await run(tar, ["-xzf", sdkAssetName(version), "-C", relative(tmp, dest), "--strip-components=1"], { cwd: tmp, quiet: true });
  rmSync(archive, { force: true });
  if (!isSdk(dest)) throw new CliError(`unpacked SDK at ${dest} is incomplete`);
  log.ok(`SDK ${version} installed: ${dest}`);
  return dest;
}

export async function requireSdk(o: SdkLookup): Promise<string> {
  const hit = locateSdk(o);
  if (hit) return hit.dir;
  log.warn(`no ScriptHook VI SDK ${o.version} found locally`);
  return installSdk(o.version);
}

