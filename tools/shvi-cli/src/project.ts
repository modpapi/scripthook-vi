// shvi.json: the mod project manifest the CLI reads and writes.
import { existsSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { CliError } from "./ui";

export const TARGETS = ["pc", "ps5"] as const;
export type Target = (typeof TARGETS)[number];
export const LANGUAGES = ["c", "cpp"] as const;
export type Language = (typeof LANGUAGES)[number];
export const TEMPLATES = ["blank", "menu", "trainer"] as const;
export type TemplateId = (typeof TEMPLATES)[number];

export interface Manifest {
  name: string; // display name
  id: string; // C identifier, file name of the binaries
  version: string;
  author?: string;
  description?: string;
  language: Language;
  template: TemplateId;
  targets: Target[];
  sdk: { version: string; path?: string };
}

export const MANIFEST = "shvi.json";

export function toId(name: string): string {
  let id = name
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "_")
    .replace(/^_+|_+$/g, "");
  if (!id) id = "my_mod";
  if (/^[0-9]/.test(id)) id = "mod_" + id;
  return id.slice(0, 48);
}

export function validateId(id: string): string | null {
  if (!/^[a-z_][a-z0-9_]{0,47}$/.test(id)) return "use lowercase letters, digits and _ (start with a letter)";
  return null;
}

export function validateVersion(v: string): string | null {
  return /^\d+\.\d+\.\d+([-+][0-9A-Za-z.-]+)?$/.test(v) ? null : "use semantic versioning, e.g. 0.1.0";
}

export function validateManifest(m: unknown): Manifest {
  const o = m as Partial<Manifest>;
  const bad = (what: string) => {
    throw new CliError(`${MANIFEST}: ${what}`);
  };
  if (!o || typeof o !== "object") bad("not an object");
  if (typeof o.name !== "string" || !o.name) bad('"name" is required');
  if (typeof o.id !== "string" || validateId(o.id)) bad(`"id" ${validateId(String(o.id)) ?? "is required"}`);
  if (typeof o.version !== "string" || validateVersion(o.version)) bad('"version" must be semver');
  if (!LANGUAGES.includes(o.language as Language)) bad(`"language" must be one of ${LANGUAGES.join(", ")}`);
  if (!TEMPLATES.includes(o.template as TemplateId)) bad(`"template" must be one of ${TEMPLATES.join(", ")}`);
  if (!Array.isArray(o.targets) || !o.targets.length || !o.targets.every((t) => TARGETS.includes(t as Target)))
    bad(`"targets" must list some of ${TARGETS.join(", ")}`);
  if (!o.sdk || typeof o.sdk.version !== "string") bad('"sdk.version" is required');
  return o as Manifest;
}

export interface Project {
  dir: string;
  manifest: Manifest;
}

// Finds shvi.json in dir or a parent.
export function findProject(start = process.cwd()): Project {
  let dir = resolve(start);
  for (;;) {
    const p = join(dir, MANIFEST);
    if (existsSync(p)) {
      let json: unknown;
      try {
        json = JSON.parse(readFileSync(p, "utf8"));
      } catch (e) {
        throw new CliError(`${p}: ${(e as Error).message}`);
      }
      return { dir, manifest: validateManifest(json) };
    }
    const up = dirname(dir);
    if (up === dir) throw new CliError(`no ${MANIFEST} here or in a parent folder (create a mod with: shvi new)`);
    dir = up;
  }
}
