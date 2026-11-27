# NativeDB data

`db/<PE timestamp>/` holds the NativeDB for one game build. Method and confidence levels:
[docs/NATIVEDB.md](../docs/NATIVEDB.md).

## Licence

The NativeDB data in [`db/`](db/) is licensed under
[Creative Commons Attribution 4.0 International (CC BY 4.0)](db/LICENSE).

The licence covers **only** that directory. It does not cover the code or anything else in this
repository, and it grants nothing over GTA VI or other third-party material or trademarks.

When you share or adapt the data, credit it, for example:

> GTA VI NativeDB from [ScriptHook VI](https://github.com/modpapi/scripthook-vi), licensed under
> [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

Say if you changed it.

## Before you reuse it

- Many names (`corroborated`, `guide`, `guide-order-conflict`) started from a third-party decompiled
  script dump, and candidate names come from the GTA V registration list and the GTA V and RDR2
  NativeDBs. The licence covers only the rights the ScriptHook VI contributors hold in this database.
- Keep each native's `confidence` value with its name. `guide` names are unchecked against the binary,
  and `guide-order-conflict` names contradict it.
- The data is specific to its build; `db/1794606036/` is the Xbox 1.0.2.1 executable.
