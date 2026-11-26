// Tiny argument parser: --key value, --key=value, --flag, -h, positionals, and "--" passthrough.
export class Args {
  positional: string[] = [];
  passthrough: string[] = [];
  private opts = new Map<string, string>();
  private flags = new Set<string>();

  constructor(argv: string[], booleanFlags: string[] = []) {
    const bools = new Set(booleanFlags);
    for (let i = 0; i < argv.length; i++) {
      const a = argv[i];
      if (a === "--") {
        this.passthrough = argv.slice(i + 1);
        break;
      }
      if (a === "-h") {
        this.flags.add("help");
        continue;
      }
      if (a.startsWith("--")) {
        const eq = a.indexOf("=");
        const key = eq > 0 ? a.slice(2, eq) : a.slice(2);
        if (eq > 0) this.opts.set(key, a.slice(eq + 1));
        else if (bools.has(key) || i + 1 >= argv.length || argv[i + 1].startsWith("--")) this.flags.add(key);
        else this.opts.set(key, argv[++i]);
        continue;
      }
      this.positional.push(a);
    }
  }

  opt(name: string): string | undefined {
    return this.opts.get(name);
  }
  flag(name: string): boolean {
    return this.flags.has(name) || this.opts.get(name) === "true";
  }
}

// Every boolean flag any command takes (so "--yes my-mod" does not eat "my-mod").
export const BOOLEAN_FLAGS = [
  "yes", "force", "ci", "no-ci", "git", "no-git", "help", "version", "debug", "dry-run",
  "install-ps5-sdk", "with-runtime", "draw", "verbose",
];
