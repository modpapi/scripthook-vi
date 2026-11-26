// Terminal output and line-based prompts. Line-based on purpose: works the same in Windows Terminal,
// cmd, PowerShell, WSL and CI logs, with no raw-mode key handling.
import { createInterface } from "node:readline/promises";

const useColor = process.stdout.isTTY && !process.env.NO_COLOR;
const wrap = (code: string) => (s: string) => (useColor ? `\x1b[${code}m${s}\x1b[0m` : s);
export const c = {
  bold: wrap("1"),
  dim: wrap("2"),
  red: wrap("31"),
  green: wrap("32"),
  yellow: wrap("33"),
  cyan: wrap("36"),
  magenta: wrap("35"),
};

export const log = {
  info: (s: string) => console.log(s),
  step: (s: string) => console.log(`${c.cyan("›")} ${s}`),
  ok: (s: string) => console.log(`${c.green("✓")} ${s}`),
  warn: (s: string) => console.log(`${c.yellow("!")} ${s}`),
  err: (s: string) => console.error(`${c.red("✗")} ${s}`),
};

export class CliError extends Error {
  constructor(message: string, public exitCode = 1) {
    super(message);
  }
}

export interface Choice<T extends string> {
  value: T;
  label: string;
  hint?: string;
}

// Answers can be fed from a non-TTY stdin (tests, scripts): one answer per line.
let rl: ReturnType<typeof createInterface> | null = null;
let piped: string[] | null = null;
async function ask(q: string): Promise<string> {
  if (!process.stdin.isTTY) {
    if (piped === null) piped = (await new Response(Bun.stdin.stream()).text()).split(/\r?\n/);
    const a = piped.shift() ?? "";
    console.log(`${q}${a}`);
    return a;
  }
  rl ??= createInterface({ input: process.stdin, output: process.stdout });
  return rl.question(q);
}
export function closePrompts() {
  rl?.close();
  rl = null;
}

export async function text(question: string, def = "", validate?: (v: string) => string | null): Promise<string> {
  for (;;) {
    const a = (await ask(`${c.bold("?")} ${question}${def ? c.dim(` (${def})`) : ""}: `)).trim() || def;
    const problem = validate?.(a) ?? null;
    if (!problem) return a;
    log.warn(problem);
    if (!process.stdin.isTTY) throw new CliError(`invalid answer for "${question}": ${problem}`);
  }
}

export async function confirm(question: string, def = true): Promise<boolean> {
  const a = (await ask(`${c.bold("?")} ${question} ${c.dim(def ? "[Y/n]" : "[y/N]")} `)).trim().toLowerCase();
  if (!a) return def;
  return a === "y" || a === "yes" || a === "s" || a === "si";
}

function printChoices<T extends string>(choices: Choice<T>[], marked: Set<T>) {
  choices.forEach((ch, i) => {
    const mark = marked.has(ch.value) ? c.green("●") : c.dim("○");
    console.log(`  ${mark} ${c.bold(String(i + 1))}) ${ch.label}${ch.hint ? c.dim(`  ${ch.hint}`) : ""}`);
  });
}

function pick<T extends string>(choices: Choice<T>[], token: string): T | null {
  const t = token.trim().toLowerCase();
  const n = Number(t);
  if (Number.isInteger(n) && n >= 1 && n <= choices.length) return choices[n - 1].value;
  return choices.find((ch) => ch.value.toLowerCase() === t)?.value ?? null;
}

export async function select<T extends string>(question: string, choices: Choice<T>[], def: T): Promise<T> {
  console.log(`${c.bold("?")} ${question}`);
  printChoices(choices, new Set([def]));
  for (;;) {
    const a = (await ask(`  ${c.dim("number or name")} ${c.dim(`(${def})`)}: `)).trim();
    if (!a) return def;
    const v = pick(choices, a);
    if (v) return v;
    log.warn(`pick 1-${choices.length}`);
    if (!process.stdin.isTTY) throw new CliError(`invalid choice "${a}" for "${question}"`);
  }
}

export async function multiSelect<T extends string>(question: string, choices: Choice<T>[], def: T[]): Promise<T[]> {
  console.log(`${c.bold("?")} ${question} ${c.dim("(comma separated)")}`);
  printChoices(choices, new Set(def));
  for (;;) {
    const a = (await ask(`  ${c.dim("numbers or names")} ${c.dim(`(${def.join(",")})`)}: `)).trim();
    if (!a) return def;
    const vals = a.split(/[,\s]+/).filter(Boolean).map((t) => pick(choices, t));
    if (vals.length && vals.every((v) => v !== null)) return [...new Set(vals as T[])];
    log.warn(`pick from 1-${choices.length}, e.g. 1,2`);
    if (!process.stdin.isTTY) throw new CliError(`invalid choice "${a}" for "${question}"`);
  }
}

export function banner(version: string) {
  console.log(c.magenta(c.bold("  ScriptHook VI")) + c.dim(`  shvi ${version}`));
  console.log(c.dim("  one mod source → PC (.asi) · PS5 (.elf + loader) · simulator\n"));
}
