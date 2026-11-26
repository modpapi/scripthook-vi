// End to end: `shvi new` every template, build it for real, run its simulator test, package it.
// Slow (a CMake build per template), so opt-in: SHVI_E2E=1 bun test test/e2e.test.ts
// SHVI_E2E_SDK=<unpacked SDK> tests a packaged SDK instead of this checkout.
//   Windows: target pc (MSVC) + the simulator on the .asi
//   Linux:   target sim; and target ps5 when ps5-payload-sdk is installed (PS5_PAYLOAD_SDK)
import { describe, expect, test } from "bun:test";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { ps5SdkDir } from "../src/toolchain";

const CLI = resolve(import.meta.dir, "..", "src", "cli.ts");
const REPO = resolve(import.meta.dir, "..", "..", "..");
const enabled = process.env.SHVI_E2E === "1";
const isWin = process.platform === "win32";
const hasPs5 = !isWin && !!ps5SdkDir();

async function shvi(cwd: string, ...args: string[]) {
  const p = Bun.spawn(["bun", CLI, ...args], { cwd, stdout: "pipe", stderr: "pipe", stdin: "ignore", env: { ...process.env, SHVI_SDK: process.env.SHVI_E2E_SDK || REPO, NO_COLOR: "1" } });
  const [out, err, code] = [await new Response(p.stdout).text(), await new Response(p.stderr).text(), await p.exited];
  if (code !== 0) console.log(out + err);
  return { code, out: out + err };
}

const combos: [string, string][] = [
  ["blank", "c"],
  ["blank", "cpp"],
  ["menu", "cpp"],
  ["trainer", "cpp"],
];

describe.skipIf(!enabled)("e2e", () => {
  for (const [tpl, lang] of combos) {
    test(
      `${tpl}/${lang}: new -> build -> test -> package`,
      async () => {
        const root = mkdtempSync(join(tmpdir(), "shvi-e2e-"));
        try {
          const id = `e2e_${tpl}_${lang}`;
          const targets = isWin ? "pc" : hasPs5 ? "pc,ps5" : "pc";
          let r = await shvi(root, "new", "--yes", "--name", `E2E ${tpl} ${lang}`, "--id", id, "--lang", lang, "--template", tpl, "--targets", targets, "--dir", id, "--no-git");
          expect(r.code).toBe(0);
          const dir = join(root, id);
          expect(existsSync(join(dir, "shvi.json"))).toBe(true);

          r = await shvi(dir, "test", "--target", isWin ? "pc" : "sim");
          expect(r.code).toBe(0);
          expect(r.out).toContain("tests passed");

          if (isWin) {
            r = await shvi(dir, "package", "--target", "pc");
            expect(r.code).toBe(0);
            const zip = join(dir, "dist", `${id}-0.1.0-pc.zip`);
            expect(existsSync(zip)).toBe(true);
            expect(readFileSync(zip).subarray(0, 4)).toEqual(Buffer.from([0x50, 0x4b, 0x03, 0x04]));
          }
          if (hasPs5) {
            r = await shvi(dir, "package", "--target", "ps5");
            expect(r.code).toBe(0);
            expect(existsSync(join(dir, "dist", `${id}-0.1.0-ps5.zip`))).toBe(true);
            expect(existsSync(join(dir, "build", "ps5", `${id}.elf`))).toBe(true);
            expect(existsSync(join(dir, "build", "ps5", "shvi_loader.elf"))).toBe(true);
          }
        } finally {
          rmSync(root, { recursive: true, force: true });
        }
      },
      600_000,
    );
  }
});
