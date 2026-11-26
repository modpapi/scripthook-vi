import { describe, expect, test } from "bun:test";
import { inflateRawSync } from "node:zlib";
import { Args, BOOLEAN_FLAGS } from "../src/args";
import { type Manifest, TEMPLATES, toId, validateId, validateManifest, validateVersion } from "../src/project";
import { TEMPLATE_INFO, render } from "../src/templates";
import { toWslPath } from "../src/toolchain";
import { crc32, makeZip } from "../src/zip";

describe("project manifest", () => {
  test("ids from names", () => {
    expect(toId("My Cool Mod!")).toBe("my_cool_mod");
    expect(toId("  ")).toBe("my_mod");
    expect(toId("2fast")).toBe("mod_2fast");
    expect(validateId("ok_id")).toBeNull();
    expect(validateId("Bad-Id")).not.toBeNull();
  });
  test("versions", () => {
    expect(validateVersion("0.1.0")).toBeNull();
    expect(validateVersion("1.2.3-beta.1")).toBeNull();
    expect(validateVersion("1.2")).not.toBeNull();
  });
  test("validation rejects bad manifests", () => {
    const good: Manifest = { name: "A", id: "a", version: "0.1.0", language: "cpp", template: "menu", targets: ["pc"], sdk: { version: "0.2.0" } };
    expect(validateManifest(good)).toEqual(good);
    expect(() => validateManifest({ ...good, targets: ["xbox"] })).toThrow();
    expect(() => validateManifest({ ...good, id: "A B" })).toThrow();
    expect(() => validateManifest({ ...good, language: "lua" })).toThrow();
  });
});

describe("args", () => {
  test("options, flags, positionals, passthrough", () => {
    const a = new Args(["new", "--yes", "My Mod", "--targets=pc,ps5", "--lang", "c", "--", "--frames", "10"], BOOLEAN_FLAGS);
    expect(a.positional).toEqual(["new", "My Mod"]);
    expect(a.flag("yes")).toBe(true);
    expect(a.opt("targets")).toBe("pc,ps5");
    expect(a.opt("lang")).toBe("c");
    expect(a.passthrough).toEqual(["--frames", "10"]);
  });
});

describe("paths", () => {
  test("windows paths map to WSL mounts", () => {
    expect(toWslPath("C:\\Users\\me\\my mod")).toBe("/mnt/c/Users/me/my mod");
    expect(toWslPath("D:/x/y")).toBe("/mnt/d/x/y");
    expect(toWslPath("/already/posix")).toBe("/already/posix");
  });
});

// Reads back what makeZip wrote (central directory -> local entries), independent of the writer's code.
function readZip(z: Uint8Array): Record<string, Uint8Array> {
  const dv = new DataView(z.buffer, z.byteOffset, z.byteLength);
  const end = z.byteLength - 22;
  expect(dv.getUint32(end, true)).toBe(0x06054b50);
  const count = dv.getUint16(end + 10, true);
  let p = dv.getUint32(end + 16, true);
  const out: Record<string, Uint8Array> = {};
  for (let i = 0; i < count; i++) {
    expect(dv.getUint32(p, true)).toBe(0x02014b50);
    const method = dv.getUint16(p + 10, true);
    const crc = dv.getUint32(p + 16, true);
    const csize = dv.getUint32(p + 20, true);
    const nlen = dv.getUint16(p + 28, true);
    const local = dv.getUint32(p + 42, true);
    const name = new TextDecoder().decode(z.subarray(p + 46, p + 46 + nlen));
    const lnlen = dv.getUint16(local + 26, true);
    const body = z.subarray(local + 30 + lnlen, local + 30 + lnlen + csize);
    const data = method === 8 ? new Uint8Array(inflateRawSync(body)) : body;
    expect(crc32(data)).toBe(crc);
    out[name] = data;
    p += 46 + nlen;
  }
  return out;
}

describe("zip", () => {
  test("crc32 known value", () => {
    expect(crc32(new TextEncoder().encode("123456789"))).toBe(0xcbf43926);
  });
  test("round trip, stored and deflated", () => {
    const big = new TextEncoder().encode("ScriptHook VI ".repeat(500));
    const rnd = crypto.getRandomValues(new Uint8Array(300));
    const z = makeZip([
      { name: "shvi/plugins/x.asi", data: rnd },
      { name: "README.txt", data: big },
    ]);
    const back = readZip(z);
    expect(Object.keys(back)).toEqual(["shvi/plugins/x.asi", "README.txt"]);
    expect(back["shvi/plugins/x.asi"]).toEqual(rnd);
    expect(back["README.txt"]).toEqual(big);
  });
});

describe("templates", () => {
  const combos = TEMPLATE_INFO.flatMap((t) => t.languages.map((l) => [t.id, l] as const));
  test("every template id has info", () => {
    expect(TEMPLATE_INFO.map((t) => t.id).sort()).toEqual([...TEMPLATES].sort());
  });
  for (const [tpl, lang] of combos) {
    test(`${tpl} / ${lang} renders a complete project`, () => {
      const manifest: Manifest = { name: "Test Mod", id: "test_mod", version: "1.2.3", language: lang, template: tpl, targets: ["pc", "ps5"], sdk: { version: "0.2.0" } };
      const files = render({ manifest, ci: true, repo: "owner/repo" });
      const src = `src/main.${lang === "c" ? "c" : "cpp"}`;
      for (const f of ["shvi.json", "CMakeLists.txt", "README.md", ".gitignore", ".github/workflows/build.yml", src])
        expect(files[f]).toBeDefined();
      expect(JSON.parse(files["shvi.json"])).toEqual(manifest);
      expect(files["CMakeLists.txt"]).toContain("shvi_add_mod(test_mod SOURCES " + src + ")");
      expect(files[src]).toContain("shvi_plugin_load");
      expect(files[src]).toContain("SHVI_THIS_MODULE");
      expect(files[".github/workflows/build.yml"]).toContain("owner/repo");
      expect(files[".github/workflows/build.yml"]).toContain("shvi build --target ps5");
      if (tpl === "trainer") expect(files["src/features.hpp"]).toContain("native_exists");
    });
  }
  test("pc-only project has no ps5 job", () => {
    const manifest: Manifest = { name: "P", id: "p", version: "0.1.0", language: "c", template: "blank", targets: ["pc"], sdk: { version: "0.2.0" } };
    const wf = render({ manifest, ci: true, repo: "o/r" })[".github/workflows/build.yml"];
    expect(wf).not.toContain("--target ps5");
    expect(wf).toContain("needs: [simulate, pc]");
  });
});
