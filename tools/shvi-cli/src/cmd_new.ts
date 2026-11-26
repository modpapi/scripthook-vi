// shvi new: create a mod project, guided (interactive) or from flags (--yes for defaults).
import { existsSync, mkdirSync, readdirSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import type { Args } from "./args";
import {
  LANGUAGES,
  type Language,
  type Manifest,
  TARGETS,
  type Target,
  type TemplateId,
  TEMPLATES,
  toId,
  validateId,
  validateVersion,
} from "./project";
import { SDK_REPO } from "./sdk";
import { TEMPLATE_INFO, render } from "./templates";
import { run } from "./toolchain";
import { CliError, c, confirm, log, multiSelect, select, text } from "./ui";
import { VERSION } from "./version";

const TARGET_CHOICES = [
  { value: "pc" as Target, label: "PC", hint: "Windows .asi, loaded by ScriptHook VI" },
  { value: "ps5" as Target, label: "PS5", hint: "self-contained .elf mod image + shvi_loader.elf (ELF loader payload)" },
];
const LANGUAGE_CHOICES = [
  { value: "cpp" as Language, label: "C++20", hint: "menus, typed natives, the full SDK (recommended)" },
  { value: "c" as Language, label: "C11", hint: "the stable C ABI only (shvi.h)" },
];

function parseList<T extends string>(raw: string | undefined, allowed: readonly T[], what: string): T[] | undefined {
  if (raw === undefined) return undefined;
  const vals = raw.split(",").map((s) => s.trim().toLowerCase()).filter(Boolean);
  for (const v of vals) if (!allowed.includes(v as T)) throw new CliError(`unknown ${what} "${v}" (use ${allowed.join(", ")})`);
  return vals as T[];
}

export async function cmdNew(a: Args): Promise<void> {
  const yes = a.flag("yes");
  const interactive = !yes;

  let name = a.opt("name") ?? a.positional[0];
  if (!name) name = interactive ? await text("Mod name", "My Mod") : "My Mod";
  let id = a.opt("id") ?? toId(name);
  if (interactive && !a.opt("id")) id = await text("Identifier (file name)", id, validateId);
  if (validateId(id)) throw new CliError(`--id: ${validateId(id)}`);

  let targets = parseList(a.opt("targets"), TARGETS, "target");
  if (!targets) targets = interactive ? await multiSelect("Which platforms?", TARGET_CHOICES, ["pc", "ps5"]) : ["pc", "ps5"];

  let language = parseList(a.opt("lang"), LANGUAGES, "language")?.[0];
  if (!language) language = interactive ? await select("Language", LANGUAGE_CHOICES, "cpp") : "cpp";

  const fit = TEMPLATE_INFO.filter((t) => t.languages.includes(language!));
  let template = parseList(a.opt("template"), TEMPLATES, "template")?.[0] as TemplateId | undefined;
  if (template && !fit.some((t) => t.id === template))
    throw new CliError(`template "${template}" needs C++ (--lang cpp); C mods use the blank template`);
  if (!template)
    template = interactive && fit.length > 1
      ? await select("Template", fit.map((t) => ({ value: t.id, label: t.label, hint: t.hint })), language === "cpp" ? "menu" : "blank")
      : language === "cpp" ? "menu" : "blank";

  let version = a.opt("version") ?? "0.1.0";
  if (interactive && !a.opt("version")) version = await text("Version", version, validateVersion);
  if (validateVersion(version)) throw new CliError(`--version: ${validateVersion(version)}`);
  const author = a.opt("author") ?? (interactive ? await text("Author", "") : "");

  const ci = a.flag("no-ci") ? false : a.flag("ci") || !interactive ? true : await confirm("Add a GitHub Actions workflow (build + release)?", true);
  const git = a.flag("no-git") ? false : a.flag("git") ? true : interactive ? await confirm("Initialise a git repository?", true) : false;

  const dir = resolve(a.opt("dir") ?? a.positional[1] ?? id.replace(/_/g, "-"));
  if (existsSync(dir) && readdirSync(dir).length && !a.flag("force"))
    throw new CliError(`${dir} exists and is not empty (use --force to write into it)`);

  const manifest: Manifest = {
    name,
    id,
    version,
    ...(author ? { author } : {}),
    language,
    template,
    targets,
    sdk: { version: a.opt("sdk-version") ?? VERSION, ...(a.opt("sdk-path") ? { path: a.opt("sdk-path") } : {}) },
  };
  const files = render({ manifest, ci, repo: SDK_REPO });
  for (const [rel, body] of Object.entries(files)) {
    const p = join(dir, rel);
    mkdirSync(dirname(p), { recursive: true });
    writeFileSync(p, body);
  }
  log.ok(`created ${c.bold(name)} in ${dir}`);
  for (const rel of Object.keys(files).sort()) log.info(c.dim(`    ${rel}`));

  if (git && Bun.which("git")) {
    await run("git", ["init", "-q"], { cwd: dir, quiet: true, allowFail: true });
    log.ok("git repository initialised");
  }

  const rel = relative(process.cwd(), dir) || ".";
  log.info(`
Next:
  cd ${rel}
  shvi build${targets.length > 1 ? "              # " + targets.join(" + ") : ""}
  shvi test               # run it in the simulator, no game needed
  shvi package            # zip(s) in dist/, ready to share
`);
}
