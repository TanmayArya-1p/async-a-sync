# compiler

The FilAsync pass, and the patches that put it and the access hook into a
Fil-C source checkout.

| Path | What it is |
|---|---|
| `build.sh` | copies `pass/` into the checkout, applies `patches/`, builds `vendor/fil-c-src/build/bin/filcc` |
| `pass/FilAsync.cpp`, `pass/FilAsync.h` | the FilAsync pass: descriptors, stubs, run thunks, call rewriting |
| `patches/backend-util-run-filasync.patch` | runs FilAsync first in Fil-C's pipeline |
| `patches/instrumentation-cmake-filasync.patch` | builds `FilAsync.cpp` into LLVM |
| `patches/filpizlonator-pending-hook.patch` | the pending-flag test FilPizlonator puts before each access |
| `patches/filc-async-param-names.patch` | clang records each `filc_async` function's parameter names, and which parameters point to `const`, for the pass |
| `patches/sroa-release-verbose.patch` | fixes a Release-build error in the pinned revision |
| `plugin/CMakeLists.txt` | builds the pass as an `opt` plugin, for the tests that run it on its own |

Each patch is a diff against the pinned Fil-C revision, and holds only our
change. `build.sh` applies it once, through `scripts/apply_filc_patches.sh`.

See the wiki:

- [Build and link](../wiki/Building-and-Linking.md): requirements, the pinned
  revision, link flags;
- [Architecture](../wiki/Architecture.md#compiler): what the passes emit;
- [Troubleshooting](../wiki/Troubleshooting.md).
