// shvi build | test | sim | package | clean
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, join, relative, sep } from "node:path";
import type { Args } from "./args";
import { type Project, type Target, findProject } from "./project";
import { requireSdk } from "./sdk";
import { hasWsl, isWindows, requireCmake, run, toWslPath } from "./toolchain";
import { CliError, c, log } from "./ui";
import { makeZip, type ZipEntry } from "./zip";

export type BuildTarget = Target | "sim";

interface Ctx {
  project: Project;
  sdk: string;
  config: string;
}

async function context(a: Args): Promise<Ctx> {
  const project = findProject(a.opt("project") ?? process.cwd());
  const sdk = await requireSdk({
    flag: a.opt("sdk"),
    projectDir: project.dir,
    manifestPath: project.manifest.sdk.path,
    version: project.manifest.sdk.version,
  });
  return { project, sdk, config: a.flag("debug") ? "Debug" : "Release" };
}

// Which targets a command acts on: --target x[,y], or "all" / none = every target in shvi.json.
function targetsOf(a: Args, p: Project, allowSim: boolean): BuildTarget[] {
  const raw = a.opt("target");
  if (!raw || raw === "all") return [...p.manifest.targets];
  const out: BuildTarget[] = [];
  for (const t of raw.split(",").map((s) => s.trim())) {
    if (t === "sim" && allowSim) out.push("sim");
    else if (t === "pc" || t === "ps5") out.push(t);
    else throw new CliError(`unknown target "${t}" (use pc, ps5${allowSim ? ", sim" : ""})`);
  }
  return out;
}

// On Windows the simulator runs the PC build itself (an .asi is a DLL), so "sim" builds "pc".
function buildDirFor(ctx: Ctx, t: BuildTarget): string {
  const dir = t === "sim" && isWindows ? "pc" : t;
  return join(ctx.project.dir, "build", dir);
}

export function artifacts(ctx: Ctx, t: BuildTarget) {
  const id = ctx.project.manifest.id;
  const b = buildDirFor(ctx, t);
  if (t === "ps5") return { mod: join(b, `${id}.elf`), loader: join(b, "shvi_loader.elf") };
  // pc, and sim on Windows, are the same MSVC build (.asi); sim elsewhere is a .so.
  const ext = t === "sim" && !isWindows ? ".so" : ".asi";
  const sim = isWindows ? join(b, ctx.config, "shvi_sim.exe") : join(b, "shvi_sim");
  return { mod: join(b, "shvi", "plugins", id + ext), sim };
}

async function buildOne(ctx: Ctx, t: BuildTarget): Promise<void> {
  const src = ctx.project.dir;
  const bdir = buildDirFor(ctx, t);
  log.step(`${c.bold(t)}: building ${ctx.project.manifest.id} (${ctx.config})`);

  if (t === "pc" || (t === "sim" && isWindows)) {
    if (!isWindows) throw new CliError("the pc target builds on Windows with Visual Studio (MSVC); use --target sim or ps5 here");
    const cmake = await requireCmake();
    await run(cmake, ["-S", src, "-B", bdir, "-A", "x64", "-DSHVI_PLATFORM=win64", `-DSHVI_SDK_DIR=${ctx.sdk}`]);
    await run(cmake, ["--build", bdir, "--config", ctx.config, "--parallel"]);
  } else if (t === "sim") {
    const cmake = await requireCmake();
    await run(cmake, ["-S", src, "-B", bdir, "-DSHVI_PLATFORM=sim", `-DSHVI_SDK_DIR=${ctx.sdk}`, `-DCMAKE_BUILD_TYPE=${ctx.config}`]);
    await run(cmake, ["--build", bdir, "--parallel"]);
  } else {
    const toolchain = (sdk: string) => `${sdk}/sdk/cmake/toolchains/ps5.cmake`;
    if (isWindows) {
      // ps5-payload-sdk is a Linux toolchain: build through WSL on the same files.
      if (!(await hasWsl())) throw new CliError("PS5 builds on Windows need WSL (wsl --install -d Ubuntu-24.04), then: shvi doctor");
      const w = { src: toWslPath(src), bdir: toWslPath(bdir), sdk: toWslPath(ctx.sdk) };
      const script = [
        'for d in "$PS5_PAYLOAD_SDK" "$HOME/.shvi/ps5-payload-sdk" /opt/ps5-payload-sdk; do [ -n "$d" ] && [ -d "$d/bin" ] && export PS5_PAYLOAD_SDK="$d" && break; done',
        `cmake -S '${w.src}' -B '${w.bdir}' -DCMAKE_TOOLCHAIN_FILE='${toolchain(w.sdk)}' -DSHVI_SDK_DIR='${w.sdk}' -DCMAKE_BUILD_TYPE=${ctx.config}`,
        `cmake --build '${w.bdir}' --parallel`,
      ].join(" && ");
      await run("wsl.exe", ["-e", "bash", "-lc", script]);
    } else {
      const cmake = await requireCmake();
      await run(cmake, ["-S", src, "-B", bdir, `-DCMAKE_TOOLCHAIN_FILE=${toolchain(ctx.sdk)}`, `-DSHVI_SDK_DIR=${ctx.sdk}`, `-DCMAKE_BUILD_TYPE=${ctx.config}`]);
      await run(cmake, ["--build", bdir, "--parallel"]);
    }
  }
  const out = artifacts(ctx, t);
  for (const f of Object.values(out)) {
    if (!existsSync(f)) throw new CliError(`build finished but ${f} is missing`);
  }
  log.ok(`${t}: ${Object.values(out).map((f) => c.bold(f)).join(", ")}`);
}

export async function cmdBuild(a: Args): Promise<void> {
  const ctx = await context(a);
  for (const t of targetsOf(a, ctx.project, true)) await buildOne(ctx, t);
}

// shvi test: build (pc or sim) and run the mod's simulator test plus any tests the project adds.
export async function cmdTest(a: Args): Promise<void> {
  const ctx = await context(a);
  let ts = targetsOf(a, ctx.project, true).filter((t) => t !== "ps5");
  if (!a.opt("target")) ts = [isWindows && ctx.project.manifest.targets.includes("pc") ? "pc" : "sim"];
  if (!ts.length) throw new CliError("PS5 images are tested on the console (shvi deploy --dry-run); here use --target sim");
  for (const t of ts) {
    if (t === "pc" && !isWindows) throw new CliError("--target pc tests on Windows; use --target sim");
    await buildOne(ctx, t);
    const cmake = await requireCmake();
    const ctest = join(dirname(cmake), isWindows ? "ctest.exe" : "ctest");
    await run(existsSync(ctest) ? ctest : "ctest", ["--test-dir", buildDirFor(ctx, t), "-C", ctx.config, "--output-on-failure"]);
    log.ok(`${t}: tests passed`);
  }
}

// shvi sim [-- shvi_sim args]: run the built mod in the simulator.
export async function cmdSim(a: Args): Promise<void> {
  const ctx = await context(a);
  const t: BuildTarget = isWindows ? "pc" : "sim";
  const out = artifacts(ctx, t);
  if (!existsSync(out.mod!) || !existsSync(out.sim!) || a.flag("force")) await buildOne(ctx, t);
  const args = a.passthrough.length ? a.passthrough : ["--frames", "240", "--bind-all", "--draw", "--key", "30:F4"];
  await run(out.sim!, [out.mod!, ...args]);
}

const PC_README = (id: string, v: string) => `${id} ${v} for GTA VI (PC), a ScriptHook VI mod

Install: copy the "shvi" folder of this zip into your game folder, so that
  <game>/shvi/plugins/${id}.asi
exists. ScriptHook VI itself must be installed (dinput8.dll + shvi/shvi_core.dll).
Story mode / offline only. F4 opens the menu (if the mod has one).
`;
const PS5_README = (id: string, v: string) => `${id} ${v} for GTA VI (PS5), a ScriptHook VI mod

Needs a console that runs homebrew with an ELF loader (elfldr, port 9021) and an FTP server.
1. Copy data/shvi/mods/${id}.elf to /data/shvi/mods/ on the console (FTP).
2. Start GTA VI in story mode, offline.
3. Send shvi_loader.elf to the ELF loader (port 9021), e.g.  nc -q0 <console ip> 9021 < shvi_loader.elf
   or let the CLI do 1-3:  shvi deploy --host <console ip>
Dry run (no game): send ${id}.elf itself to the ELF loader; it runs a few seconds and reports.
Controls: L1 + R3 opens the menu; d-pad moves, Cross selects, Circle goes back.
Keep the console off PSN while modded. Story mode / offline only.
`;

export async function cmdPackage(a: Args): Promise<void> {
  const ctx = await context(a);
  const m = ctx.project.manifest;
  const dist = join(ctx.project.dir, "dist");
  mkdirSync(dist, { recursive: true });
  for (const t of targetsOf(a, ctx.project, false)) {
    const out = artifacts(ctx, t);
    if (Object.values(out).some((f) => f && !existsSync(f)) || a.flag("force")) await buildOne(ctx, t);
    const enc = new TextEncoder();
    const entries: ZipEntry[] = [];
    if (t === "pc") {
      entries.push({ name: `shvi/plugins/${m.id}.asi`, data: readFileSync(out.mod!) });
      if (a.flag("with-runtime")) {
        // The ScriptHook VI core for players who do not have it: dinput8.dll, shvi/shvi_core.dll, shvi/natives/.
        const rt = join(ctx.sdk, "runtime", "win64");
        if (!existsSync(join(rt, "shvi", "shvi_core.dll"))) log.warn("--with-runtime: this SDK has no runtime/win64 (release SDKs carry it)");
        else for (const f of walk(rt)) entries.push({ name: relative(rt, f).split(sep).join("/"), data: readFileSync(f) });
      }
      entries.push({ name: "README.txt", data: enc.encode(PC_README(m.id, m.version)) });
    } else if (t === "ps5") {
      entries.push({ name: "shvi_loader.elf", data: readFileSync(out.loader!) });
      entries.push({ name: `data/shvi/mods/${m.id}.elf`, data: readFileSync(out.mod!) });
      entries.push({ name: "README.txt", data: enc.encode(PS5_README(m.id, m.version)) });
    }
    const zip = join(dist, `${m.id}-${m.version}-${t}.zip`);
    writeFileSync(zip, makeZip(entries));
    log.ok(`${t}: ${c.bold(zip)} (${entries.map((e) => basename(e.name)).join(", ")})`);
  }
}

function walk(dir: string): string[] {
  return readdirSync(dir).sort().flatMap((n) => {
    const p = join(dir, n);
    return statSync(p).isDirectory() ? walk(p) : [p];
  });
}

export async function cmdClean(a: Args): Promise<void> {
  const p = findProject(a.opt("project") ?? process.cwd());
  for (const d of ["build", "dist"]) rmSync(join(p.dir, d), { recursive: true, force: true });
  log.ok("removed build/ and dist/");
}
