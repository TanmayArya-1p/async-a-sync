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
- `tests/probe_io_uring.c` finds io_uring unavailable.

Read the skip count as well as the failure count.

**Not covered.** `run.sh` does not run the Makefile demo targets,
`demos/wordcount/run_wordcount_3way.sh`, or
`demos/wordcount/inspect_disasm_cfg.sh`. After
changing the runtime or the link line, run `make demo-pragma`,
`make all-demos` and `make disasm` by hand.

## What each check proves

### Compiler

| Check | Proves |
|---|---|
| `check_dependency_options.sh` | dependency metadata and namespace hashes, the runtime each descriptor points to, option precedence between declaration and definition, rejection of conflicting, empty-name and obsolete options and of a missing, malformed or doubled `runtime=` |
| `check_callsite_pragma.sh` | a pragma around a call is an error with `-Werror=pragma-clang-attribute` |
| `t_annotate_smoke.c` | Clang emits `llvm.global.annotations` for the pragma |
| `stage4_compiler_hook.c` | the patched compiler inserts the pending-flag test and resolves buffers on access |
| `t_pragma_same_tu_lazy.c`, `t_pragma_repeat_read.c` | calls in the defining translation unit are redirected before inlining; identical call sites each submit |

### Framework (no io_uring)

| Check | Proves |
|---|---|
| `check_dependencies.sh`, `t_dependency_mock.c` | dependency locks order calls against a deterministic mock runtime |
| `t_mock_runtime.c` with `mock_runtime.c` | annotated calls work with another runtime linked and no io_uring symbol present |
| `t_two_runtimes.c` with `mock_runtime.c` and io_uring | two runtimes in one program: a lock and a pending buffer order calls across them |
| `t_unlinked_runtime.c` | naming a runtime the program does not link fails at link time |
| `t_pending_registry.c`, `t_pragma_markpending.c` | the pending-mark contract: which arguments are marked, aliasing, ownerless marks |
| `t_pragma_alloc.c` | the allocator interface |
| `t_pragma_custom_validator.c`, `t_pragma_unknownop.c`, `t_pragma_ignore.c` | the runtime owns the op set, and a program validator replaces it |

### io_uring runtime

| Check | Proves |
|---|---|
| `check_forwarders.sh` | the native forwarders for the io_uring syscalls are generated (needs only Ruby) |
| `check_linkage.sh` with `t_linked_async_main.c`/`_def.c` | the archives hold what they should; the framework refers to no runtime; a declaration-only annotation links an implementation in another file |
| `t_backend_io_uring.c` | hand-built descriptors naming the io_uring runtime reach every supported operation through `filc_async_submit` |
| `t_pragma_io_uring.c`, `t_pragma_dependencies.c` | annotated calls dispatch end to end and honor dependency order |
| `t_pragma_lazy_many.c` | more never-polled calls than the request table holds complete (slot reclaiming) |
| `t_pragma_many_calls.c`, `t_pragma_reuse_lazy.c` | handles retire once delivered; reusing touched buffers does not flush the queue |
| `t_pragma_error_path.c` | calls on a bad fd fail with `-EBADF` instead of hanging |
| `t_openat_pending_path.c`, `t_pending_open_failure.c` | an open waits for a path still being read; a failed pending open frees its handle |

### Threads

| Check | Proves |
|---|---|
| `t_threads.c` | two threads issue annotated reads and read each other's buffers |
| `t_thread_compute.c` | a thread that only computes never enters the framework |

### Explicit API and measurements

The `stage*.c` programs cover the following:

- lazy submission (`stage2_*`);
- the effect-set DAG (`stage3`) and tokens (`stage5`, `stage_token_ordering`);
- descriptor provenance (`stage6`);
- throughput (`stage7`);
- latency (`stage8`).

The `*_probe.c` programs establish kernel and Fil-C properties the design
relies on. `stage9_device_parallelism.c` measures the device's own
parallelism. `t_dag_submit_failure.c` covers DAG submission errors.

## Adding a test

- **A C test** prints a `PASS`/`FAIL` line and exits non-zero on failure.
  Exit 77 when a prerequisite is missing, so the test counts as skipped.
- **Registering it.** Add it to the matching list in `tests/run.sh`: stock
  Fil-C, patched compiler, or a custom build function like
  `run_mock_runtime`.
