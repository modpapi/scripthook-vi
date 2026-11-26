#!/usr/bin/env bun
// shvi: the ScriptHook VI command line. One mod source -> PC (.asi), PS5 (.elf image + ELF loader), simulator.
import { Args, BOOLEAN_FLAGS } from "./args";
import { cmdBuild, cmdClean, cmdPackage, cmdSim, cmdTest } from "./cmd_build";
import { cmdDeploy } from "./cmd_deploy";
import { cmdDoctor, cmdSdk } from "./cmd_doctor";
import { cmdNew } from "./cmd_new";
import { CliError, banner, c, closePrompts, log } from "./ui";
import { VERSION } from "./version";

const HELP = `${c.bold("usage:")} shvi <command> [options]

${c.bold("Create")}
  new [name] [dir]        guided: platforms, language, template, CI workflow
      --name --id --targets pc,ps5 --lang cpp|c --template blank|menu|trainer
      --version --author --dir --ci|--no-ci --git|--no-git --yes (all defaults) --force

${c.bold("Build and test")}                      (run inside a mod folder, or --project <dir>)
  build   [--target pc|ps5|sim|all] [--debug]   cmake configure + build, per target
  test    [--target pc|sim]                     build, then run the mod in the simulator (ctest)
  sim     [-- <shvi_sim args>]                  run the mod in the simulator (menu as text)
  package [--target pc|ps5] [--with-runtime]    dist/<id>-<version>-<target>.zip
  clean                                         remove build/ and dist/

${c.bold("Run it")}
  deploy pc  --game <game folder>               copy the .asi into <game>/shvi/plugins
  deploy ps5 --host <ip> [--dry-run] [--yes]    FTP the image, send shvi_loader.elf to elfldr :9021

${c.bold("Setup")}
  doctor [--install-ps5-sdk]                    check cmake, MSVC, WSL, ps5-payload-sdk, SDK
  sdk path [version] | sdk install [version]    where the ScriptHook VI SDK is / download it

  --sdk <dir>   use this SDK (a checkout or unpacked release); also SHVI_SDK
  --version     print the version
`;

async function main(argv: string[]): Promise<number> {
  const [cmd, ...rest] = argv;
  const a = new Args(rest, BOOLEAN_FLAGS);
  if (!cmd || cmd === "help" || cmd === "--help" || cmd === "-h") {
    banner(VERSION);
    console.log(HELP);
    return 0;
  }
  if (cmd === "--version" || cmd === "-v" || cmd === "version") {
    console.log(VERSION);
    return 0;
  }
  if (a.flag("help")) {
    console.log(HELP);
    return 0;
  }
  const commands: Record<string, (a: Args) => Promise<void>> = {
    new: async (x) => {
      banner(VERSION);
      await cmdNew(x);
    },
    create: cmdNew,
    build: cmdBuild,
    test: cmdTest,
    sim: cmdSim,
    package: cmdPackage,
    pack: cmdPackage,
    clean: cmdClean,
    deploy: cmdDeploy,
    doctor: cmdDoctor,
    sdk: cmdSdk,
  };
  const fn = commands[cmd];
  if (!fn) {
    log.err(`unknown command "${cmd}"`);
    console.log(HELP);
    return 2;
  }
  await fn(a);
  return 0;
}

try {
  process.exitCode = await main(process.argv.slice(2));
} catch (e) {
  if (e instanceof CliError) {
    log.err(e.message);
    process.exitCode = e.exitCode;
  } else {
    log.err((e as Error).stack ?? String(e));
    process.exitCode = 1;
  }
} finally {
  closePrompts();
}
