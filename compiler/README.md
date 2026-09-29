# compiler

Overrides and patches that turn a Fil-C source checkout into the compiler
async-a-sync needs.

| Path | What it is |
|---|---|
| `build.sh` | installs the overrides, applies the patches, builds `vendor/fil-c-src/build/bin/filcc` |
| `upstream-overrides/llvm/.../FilAsync.cpp` | the FilAsync pass: descriptors, stubs, run thunks, call rewriting |
| `upstream-overrides/llvm/.../FilPizlonator.cpp` | Fil-C's pointer pass with the pending-flag test at access sites |
| `upstream-overrides/clang/lib/CodeGen/BackendUtil.cpp` | runs FilAsync first in Fil-C's pipeline |
| `upstream-patches/sroa-release-verbose.patch` | fixes a Release-build error in the pinned revision |
| `upstream-patches/filc-async-param-names.patch` | clang records each `filc_async` function's parameter names for the pass |

See the wiki:

- [Build and link](../wiki/Building-and-Linking.md): requirements, the pinned
  revision, link flags;
- [Architecture](../wiki/Architecture.md#compiler): what the passes emit;
- [Troubleshooting](../wiki/Troubleshooting.md).
