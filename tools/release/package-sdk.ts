#!/usr/bin/env bun
// Assembles the ScriptHook VI SDK release: dist/shvi-sdk-<version>.tar.gz and .zip.
//
//   bun tools/release/package-sdk.ts --version 0.2.0 [--win64-runtime <cmake install prefix>]
//                                    [--ps5-loader <shvi_loader.elf>] [--out dist]
//
// The SDK is the subset of this repo a mod build needs (headers, CMake, the portable engine, the PS5
// runtime and loader sources, the simulator) plus optional prebuilt runtimes for players:
//   runtime/win64/  dinput8.dll, shvi/shvi_core.dll, shvi/natives/*.json   (from cmake --install, Windows CI)
//   runtime/ps5/    shvi_loader.elf                                        (from the PS5 CI build)
import { cpSync, existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { join, relative, resolve } from "node:path";
import { makeZip, type ZipEntry } from "../shvi-cli/src/zip";

const ROOT = resolve(import.meta.dir, "..", "..");
const args = process.argv.slice(2);
const opt = (k: string) => {
  const i = args.indexOf(`--${k}`);
  return i >= 0 ? args[i + 1] : undefined;
};
const version = opt("version") ?? JSON.parse(readFileSync(join(ROOT, "tools/shvi-cli/package.json"), "utf8")).version;
const outDir = resolve(opt("out") ?? join(ROOT, "dist"));
const name = `shvi-sdk-${version}`;
const stage = join(outDir, name);

// What goes in, relative to the repo root.
const INCLUDE = [
  "sdk",
  "src/engine",
  "tools/sim",
  "platforms/ps5/runtime",
  "platforms/ps5/payload",
  "platforms/ps5/README.md",
  "samples",
  "docs/SDK.md",
  "README.md",
];
const SKIP = /(^|[\\/])(build|__pycache__|\.DS_Store)([\\/]|$)/;

rmSync(stage, { recursive: true, force: true });
mkdirSync(stage, { recursive: true });
for (const rel of INCLUDE) {
  const from = join(ROOT, rel);
  if (!existsSync(from)) throw new Error(`missing ${rel}`);
  cpSync(from, join(stage, rel), { recursive: true, filter: (src) => !SKIP.test(relative(ROOT, src)) });
}

const win64 = opt("win64-runtime");
if (win64) {
  const game = join(resolve(win64), "game");
  if (!existsSync(join(game, "shvi", "shvi_core.dll"))) throw new Error(`${game}/shvi/shvi_core.dll not found (cmake --install first)`);
  const dest = join(stage, "runtime", "win64");
  cpSync(game, dest, { recursive: true, filter: (src) => !src.includes(join("shvi", "plugins")) });
  const tools = join(resolve(win64), "tools");
  if (existsSync(tools)) cpSync(tools, join(stage, "tools", "bin", "win64"), { recursive: true });
}
const loader = opt("ps5-loader");
if (loader) {
  mkdirSync(join(stage, "runtime", "ps5"), { recursive: true });
  cpSync(resolve(loader), join(stage, "runtime", "ps5", "shvi_loader.elf"));
}
writeFileSync(join(stage, "VERSION"), version + "\n");

// Archives: tar.gz (what `shvi sdk install` downloads) and zip (for people unpacking by hand).
function walk(dir: string): string[] {
  return readdirSync(dir).flatMap((n) => {
    const p = join(dir, n);
    return statSync(p).isDirectory() ? walk(p) : [p];
  });
}
const files = walk(stage).sort();
const entries: ZipEntry[] = files.map((f) => ({ name: `${name}/${relative(stage, f).replace(/\\/g, "/")}`, data: readFileSync(f) }));
writeFileSync(join(outDir, `${name}.zip`), makeZip(entries));
const tar = Bun.spawnSync(["tar", "-czf", `${name}.tar.gz`, name], { cwd: outDir, stdout: "inherit", stderr: "inherit" });
if (tar.exitCode !== 0) throw new Error("tar failed");
console.log(`${name}: ${files.length} files${win64 ? ", win64 runtime" : ""}${loader ? ", ps5 loader" : ""}`);
console.log(`  ${join(outDir, name + ".tar.gz")}\n  ${join(outDir, name + ".zip")}`);
