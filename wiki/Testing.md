# Tests

Run the whole suite after building the runtime and the compiler:

```sh
./tests/run.sh
```

The script:

- builds every test;
- runs the host checks, the stock-Fil-C tests, and the patched-compiler
  tests;
- ends with `tests passed: N   failed: N   skipped: N`.

Tests are **skipped**, not failed, in two cases:

- the patched compiler is missing;
- `tests/probes/probe_io_uring.c` finds io_uring unavailable.

Read the skip count as well as the failure count.

## Layout

| Directory | What its tests check |
|---|---|
| `tests/compiler/` | the FilAsync pass and the access hook: options, descriptors, call rewriting, parameter names |
| `tests/framework/` | the runtime-agnostic framework: locks, pending marks, validators, the allocator, runtimes other than io_uring |
| `tests/io_uring/` | the io_uring runtime and its explicit `fasync_*` API, including the archives and native bridges |
| `tests/probes/` | kernel, Fil-C and device facts the design relies on |
| `tests/support/` | shared by the tests: the mock runtime, mock headers, `add_param_names.py` |

**Stock-clang checks.** `check_dependency_options.sh` and the
`opt_annotate*.sh` scripts in `tests/compiler/` run the pass on IR from a
stock clang, which records no parameter names and no `const`.
`tests/support/add_param_names.py` adds both the way the clang patch does.

**Not covered.** `run.sh` runs `demos/wordcount/run_wordcount.sh` and both
rpc demos, but not the other Makefile demo targets or the inspection
scripts (`make disasm`, `make cfg`). After changing the runtime or the link
line, run `make demo-pragma`, `make all-demos` and `make disasm` by hand.

## What each check proves

### Compiler (`tests/compiler/`)

| Check | Proves |
|---|---|
| `check_dependency_options.sh` | dependency metadata and the hashes of `<param>:<namespace>`, the runtime each descriptor points to, option precedence between declaration and definition, rejection of conflicting, empty-namespace, namespace-less and obsolete options, of indices and unknown parameter names, of the removed `fd=`, and of a missing, malformed or doubled `runtime=`; the kinds unannotated pointers take from `const`, and that options override them |
| `t_param_names.c` | the patched clang gives an annotated prototype without parameter names the definition's names; dependencies conflict only when value, parameter name and namespace all match |
| `check_callsite_pragma.sh` | a pragma around a call is an error with `-Werror=pragma-clang-attribute` |
| `check_annotation_macros.sh` | `FILC_ASYNC` and its option macros give the same annotation as the pragma form, and a misspelt option macro is an error |
| `check_wait_all.sh` | completion arguments retain five 16-byte staging cells; read and writer stubs mark tokens before submission; emitted object relocations reference framework services |
| `t_annotate_smoke.c` | Clang emits `llvm.global.annotations` for the pragma |
| `stage4_compiler_hook.c` | the patched compiler inserts the pending-flag test and resolves buffers on access |
| `t_pragma_same_tu_lazy.c`, `t_pragma_repeat_read.c` | calls in the defining translation unit are redirected before inlining; identical call sites each submit |
| `opt_annotate.sh`, `opt_annotate_test.sh` | the pass on its own, as an `opt` plugin: enrollment, descriptor layout, stubs, annotation erasure |

### Framework (`tests/framework/`)

| Check | Proves |
|---|---|
| `check_wait_all.sh`, `t_wait_all.c` | 30 producers can make progress before a writer is handed off; out-of-order completion, failed tasks, snapshots across pointer reuse, duplicate/interior pointers, shared owners, nested groups, early resolution, ownerless marks, and concurrent waiters |
| `check_dependencies.sh`, `t_dependency_mock.c` | dependency locks order calls against a deterministic mock runtime |
| `check_write_after_read.sh`, `t_write_after_read.c` | read marks against a deterministic mock runtime: a writer waits for earlier readers, readers never wait, the flag tracks write marks only, a body's own calls do not wait for it, `wait_all` and `is_pending` see producers only |
| `t_mock_runtime.c` with `support/mock_runtime.c` | annotated calls work with another runtime linked and no io_uring symbol present |
| `t_two_runtimes.c` with `support/mock_runtime.c` and io_uring | two runtimes in one program: a lock and a pending buffer order calls across them |
| `t_unlinked_runtime.c` | naming a runtime the program does not link fails at link time |
| `demos/rpc/run_rpc_demo.sh counter` | `runtime=rpc` (`runtime/rpc/rpc_runtime.c`) sends calls to a loopback TCP server; its results and lock ordering come back through the framework |
| `demos/rpc/run_rpc_demo.sh upload` | io_uring reads and rpc uploads in one loop: an upload's payload waits for the read filling it, through the io_uring runtime, and the uploaded bytes are the file's |
| `t_pending_registry.c`, `t_pragma_markpending.c` | the pending-mark contract: which arguments are marked, aliasing, ownerless marks |
| `t_const_inference.c` | built by the patched clang, unannotated pointers to `const` (also through a typedef or a pointer to const pointers) are not marked; `char* const`, `const char**` and plain pointers are; `FILC_BUF` overrides `const` |
| `t_pragma_alloc.c` | the allocator interface |
| `t_pragma_custom_validator.c`, `t_pragma_unknownop.c`, `t_pragma_ignore.c` | the runtime owns the op set, and a program validator replaces it |

### io_uring runtime (`tests/io_uring/`)

| Check | Proves |
|---|---|
| `check_forwarders.sh` | the forwarders patch makes Fil-C's generator emit every native bridge the runtimes call (needs Ruby and the Fil-C checkout) |
| `check_linkage.sh` with `t_linked_async_main.c`/`_def.c` | the archives hold what they should; the framework refers to no runtime; a declaration-only annotation links an implementation in another file |
| `t_backend_io_uring.c` | hand-built descriptors naming the io_uring runtime reach every supported operation through `filc_async_submit` |
| `t_write_after_read_uring.c` | a pread into a buffer a queued pwrite is sending waits for the pwrite, and two pwrites from one buffer do not wait for each other |
| `t_wait_all_uring.c` | 30 separately marked reads queue before the writer; trailing completion arguments validate, each read sees the original data, and the later write reaches disk; stock builds use explicit stubs, patched builds use annotated calls |
| `t_pragma_io_uring.c`, `t_pragma_dependencies.c` | annotated calls dispatch end to end and honor dependency order |
| `t_annotation_macros.c` | calls annotated with `FILC_ASYNC` (one per function, a shared `#define`, an annotated declaration with an unannotated definition) dispatch end to end in dependency order |
| `t_pragma_lazy_many.c` | more never-polled calls than the request table holds complete (slot reclaiming) |
| `t_pragma_many_calls.c`, `t_pragma_reuse_lazy.c` | handles retire once delivered; reusing touched buffers does not flush the queue |
| `t_pragma_error_path.c` | calls on a bad fd fail with `-EBADF` instead of hanging |
| `t_openat_pending_path.c`, `t_pending_open_failure.c` | an open waits for a path still being read; a failed pending open frees its handle |

### Threads

| Check | Proves |
|---|---|
| `io_uring/t_threads.c` | two threads issue annotated reads and read each other's buffers |
| `framework/t_thread_compute.c` | a thread that only computes never enters the framework |

### Explicit API and measurements

The `stage*.c` programs in `tests/io_uring/` cover the following:

- lazy submission (`stage2_*`);
- the effect-set DAG (`stage3`) and tokens (`stage5`, `stage_token_ordering`);
- descriptor provenance (`stage6`);
- throughput (`stage7`);
- latency (`stage8`).

The programs in `tests/probes/` establish kernel and Fil-C properties the
design relies on (`probe_io_uring.c` and the `*_probe.c` programs), and
`stage9_device_parallelism.c` measures the device's own parallelism.
`io_uring/t_dag_submit_failure.c` covers DAG submission errors.

## Adding a test

- **A C test** prints a `PASS`/`FAIL` line and exits non-zero on failure.
  Exit 77 when a prerequisite is missing, so the test counts as skipped.
- **Where it goes.** Put it in the directory for what it checks (see
  [Layout](#layout)).
- **Registering it.** Add it to the matching list in `tests/run.sh`: stock
  Fil-C, patched compiler, or a custom build function like
  `run_mock_runtime`. `run_filc_test` and `run_host_test` take its path under
  `tests/` without the extension (`io_uring/stage7_throughput`);
  `run_patched` takes a name and the source file.
