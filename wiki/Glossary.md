# Glossary

- **Access hook**: The pending-flag test FilPizlonator inserts before an access through a pointer. If the flag is set, it calls `filc_resolve_pending`.
- **Annotated function**: A function under a `filc_async` annotation pragma. Direct calls to it are redirected to its stub.
- **Capability**: Fil-C's bounds and type information carried with every pointer. The object it names has a header whose `aux` word holds the pending flag.
- **Descriptor**: `filc_async_meta`, emitted by the pass for each annotated function. It holds the argument kinds, the dependency bits, the option strings and a pointer to the function's runtime. A runtime's own descriptor is a `filc_async_runtime`.
- **Dependency lock**: A reader/writer lock on an argument's value (or object), its parameter name and a namespace, taken by the stub for each `r_dep=`/`w_dep=` and released when the call completes.
- **Deferred submission**: The io_uring runtime's policy: queue requests and send them to the kernel only when something needs a result.
- **Framework**: The runtime-agnostic layer in `libpizlo.a` (`filc_async.c`). It holds tasks, marks and locks.
- **Mark**: A record that a task owns an object. While an object has one, it is *pending*. Stub marks are exclusive; runtime marks made with `filc_async_mark_shared` may overlap.
- **Namespace**: The required `:<namespace>` part of a dependency option, `r_dep=<param>:<namespace>`. With the parameter name, it says which resource a value refers to.
- **Pending**: A buffer owned by a call that has not completed. The first access waits.
- **Run thunk**: `__filc_async_run_<name>`, emitted by the pass. It unpacks the staged arguments and calls the function's body. Runtimes call it through `filc_async_run`.
- **Runtime**: The code that executes calls, exported as a `filc_async_runtime` descriptor named `filc_async_runtime_<name>`. Each annotated function names one with `runtime=<name>`, and a program may link several. This repository ships `runtime=io_uring` and `runtime=rpc`.
- **Staged arguments**: The call's arguments, copied by the stub into 16-byte cells.
- **Stub**: `__filc_async_stub_<name>`, emitted by the pass. It stages arguments, starts the task, takes locks, marks buffers, and submits to the runtime.
- **Task**: The framework's record of one call. An annotated call returns it as a handle for `filc_async_poll`/`filc_async_wait`.
