# Framework API

**Header:** [`runtime/include/filc_async.h`](../runtime/include/filc_async.h)
**Implementation:** [`runtime/framework/filc_async.c`](../runtime/framework/filc_async.c), in `libpizlo.a`

The framework sits between annotated calls and the runtime. It owns tasks,
pending buffer marks, and dependency locks. It never interprets `op=`. This
page covers the functions programs call, the descriptor the compiler emits,
and the entry points the compiler's stubs call. The functions a runtime
implements or uses are in the [Runtime API](Runtime-API.md).

Every function here may be called from any thread.

## Program API

### Results

```c
struct filc_async_result_s {
    const void* pending;   /* the task an annotated call returned */
    long result;           /* the runtime's result, or -errno */
    unsigned char state;   /* 0 done, 1 pending, 2 failed */
};

bool filc_async_poll(struct filc_async_result_s* out);
void filc_async_wait(struct filc_async_result_s* out);
```

Set `out->pending` to the handle an annotated call returned.

| Function | Behavior |
|---|---|
| `filc_async_poll` | Asks the runtime to make progress (`FILC_ASYNC_POLL_PROGRESS`) without blocking. Returns `true` and fills `result` and `state` once the call has completed. |
| `filc_async_wait` | Blocks until the call completes (`FILC_ASYNC_POLL_BLOCK`), then fills `result` and `state`. |

**One delivery per handle.** Delivering a completion retires the handle.
`out` keeps the result, but a later `poll` or `wait` with the same handle
finds nothing and leaves `out` unchanged (`poll` returns `false`).

**Results you never collect.** You never have to collect a result. A call
completes, and its buffers resolve, whether or not anyone waits on it.

### Pending buffers

```c
bool filc_async_is_pending(const void* buf);
void filc_async_mark_resolved(void* buf);
```

| Function | Behavior |
|---|---|
| `filc_async_is_pending` | Whether any call is still producing the object `buf` points into (holds a write mark on it). Calls only reading it do not count. Does not wait. |
| `filc_async_mark_resolved` | Removes one write mark on the object without waiting for its owner; read marks stay. The call's completion does not restore the mark. Meant for tests and for marks made with a `NULL` task. |

To wait for a buffer, simply access it. Code built by the patched compiler
waits on the first access.

### Joining pending buffers

```c
typedef void* prov_tag;
prov_tag prov_alloc(void);
prov_tag filc_async_wait_all(const prov_tag* tags, size_t count);
```

The function returns a new completion object immediately. It stays pending
until all the input marks captured by the call have resolved. The array
contents are copied; the array itself can be reused or go out of scope.

This orders operations that have no shared data. Give each producer a separate
completion object, then pass the group to the consumer as an unannotated
`prov_tag` (or any pointer to non-`const`) or a `bout=` / `buf=` argument. Its
stub waits for the group before handing the consumer to its runtime. Do not
mark that consumer argument `bin=` or declare it as a pointer to `const`: an
input alone does not make the stub wait.

```c
prov_tag first = prov_alloc();
prov_tag second = prov_alloc();
prov_tag tags[] = { first, second };

read_work(first);
read_work(second);
write_work(filc_async_wait_all(tags, 2));
```

Here `read_work` and `write_work` are annotated functions whose runtime accepts
the extra pointer. The framework joins marks from any runtime. The io_uring
runtime accepts trailing completion pointers; other runtimes decide their own
argument shapes. See [io_uring](io_uring-Runtime.md#completion-pointers) for a
complete declaration example.

- **Separate objects.** Each producer needs a separate allocation. Different
  elements of one byte array share the same pending flag and would serialize
  the producers. An array containing separately allocated pointers is fine.
  `prov_alloc` allocates a separate Fil-C object for each tag. It returns
  `NULL` if allocation fails. `filc_async_alloc` is a framework arena whose
  slices can share one pending flag.
- **Snapshot.** A group captures existing marks, including every shared owner
  of each input object. New marks on an input are not added to that group, even
  when the same task removes and replaces a mark. Duplicates and interior
  pointers do not add a second dependency on the same mark.
- **Empty inputs.** Null pointers and resolved objects add no dependencies.
  `filc_async_wait_all(NULL, 0)` returns a resolved object. A nonzero
  count needs a readable array of that many pointers.
- **Completion.** A failed operation also resolves its marks. Collect each
  operation's result separately when success matters. Early buffer resolution
  releases that dependency without waiting for the rest of the operation.
  An ownerless mark needs an explicit `filc_async_mark_resolved`.
- **Progress.** Construction starts no work and does not wait for producers.
  Waiting or accessing the group progresses its producers; a statistics refresh
  only checks them. The group may still report pending until one of these
  collects its completion. There is no background worker for groups.
- **Lifetime.** The returned object is a separate GC allocation. Keep its
  pointer while using it, and do not free it or use its bytes as data. It is a
  buffer token, not a result handle for `filc_async_poll` / `filc_async_wait`.
  Do not manually resolve the group or make a producer depend on its own group.

### Statistics

```c
typedef struct {
    unsigned long tasks_submitted;  /* annotated calls started */
    unsigned long tasks_completed;  /* annotated calls completed */
    unsigned long tasks_failed;     /* ... with a negative result */
    unsigned long pending_resolves; /* marking a buffer waited for its previous owner */
    unsigned long lock_waits;       /* a dependency lock waited for a conflicting call */
    unsigned long hook_resolves;    /* an access found its object pending */
} filc_async_stats;

void filc_async_get_stats(filc_async_stats* out);
```

**Refreshing the counts.** `filc_async_get_stats` first asks the runtime,
with `FILC_ASYNC_POLL_CHECK`, about every call still running. Calls that
finished without anyone asking are then counted.

**What is counted.** Tasks a runtime starts for itself (for example explicit
`fasync_pread` calls) are not counted.

### Validation

```c
typedef bool (*filc_async_validator_fn)(const filc_async_meta* meta);
void filc_async_set_validator(filc_async_validator_fn fn);
void filc_async_validate_table(const filc_async_meta* const* metas);
void filc_async_fatal(const char* msg) __attribute__((noreturn));
```

**Startup check.** Each translation unit with annotated functions gets a
constructor, at priority 65535, that calls `filc_async_validate_table` on its
descriptors before `main`. Each descriptor must pass two checks:

- a dependency-bits check;
- the validator. That is the `validate` of the runtime the function names,
  unless the program installed one.

**Overriding the validator.** A program can install its own validator from an
earlier constructor with `filc_async_set_validator`, as
`tests/framework/t_pragma_custom_validator.c` does. The program's validator decides
instead of the runtime's. It only suppresses the startup abort: a call the
runtime cannot run still fails when it is submitted.

**`filc_async_fatal`** prints `filc_async: fatal: <msg>` to stderr and aborts.

### Allocator

**Header:** [`runtime/include/filc_async_alloc.h`](../runtime/include/filc_async_alloc.h)

```c
typedef struct {
    void* (*alloc)(size_t size, size_t align); /* returns zeroed memory */
    void  (*free) (void* p, size_t size);
} filc_async_allocator;

void  filc_async_set_allocator(filc_async_allocator a);
filc_async_allocator filc_async_get_allocator(void);
void* filc_async_alloc(size_t size, size_t align);
```

**What uses it.** Staged arguments, tasks, marks, locks, and runtime state
come from `filc_async_alloc`.
Provenance tags and completion objects returned by `wait_all` use separate GC
allocations, so they do not share an arena object's pending flag. `prov_alloc`
calls Fil-C's `zgc_aligned_alloc` through the linked runtime. A custom
`filc_async_allocator` only changes framework bookkeeping allocations.

**The default arena.** A thread-safe, GC-backed bump arena that never frees.
Passing `{0, 0}` to `filc_async_set_allocator` restores it.

**Installing your own.** Install it before the first annotated call. It must
return zeroed memory and be callable from any thread.

## Descriptor

The pass emits one `filc_async_meta` per annotated function, named
`__filc_meta_<name>`:

```c
typedef struct {
    const char* name;          /* the function's source name */
    uint32_t    nargs;         /* parameter count */
    uint32_t    noped_args;    /* how many bin=/bout=/buf= options it has */
    uint32_t    flags;         /* reserved, 0 */
    uint32_t    result;        /* FILC_ASYNC_RESULT_PTR or FILC_ASYNC_RESULT_WORD */
    const char* const* opts;   /* NULL-terminated copy of every option string */
    const struct filc_async_runtime* runtime; /* &filc_async_runtime_<name> from runtime=<name> */
    struct {
        uint32_t kind;         /* FILC_ASYNC_ARG_* */
        uint32_t dependency;   /* FILC_ASYNC_DEP_* bits and space */
    } args[];
} filc_async_meta;
```

**The layout is ABI.** In the Fil-C layout, `name` is at offset 0, `nargs` at
16, `opts` at 32, `runtime` at 48 and `args[0]` at 64. The pass emits the same layout
field for field. When you add a field, change the header, the pass
(`FilAsync.cpp`), and the metadata tests together.

**Argument kinds.** They are listed in the
[annotation reference](Annotation-Reference.md#options).

| Constant | Value | Set by |
|---|---|---|
| `FILC_ASYNC_ARG_IGNORED` | 0 | unannotated non-pointer |
| `FILC_ASYNC_ARG_SCALAR` | 1 | (reserved) |
| `FILC_ASYNC_ARG_BUFFER_IN` | 2 | `bin=`, or an unannotated pointer to `const` |
| `FILC_ASYNC_ARG_BUFFER_OUT` | 3 | `bout=` |
| (none) | 4 | unused; it was the removed `fd=` |
| `FILC_ASYNC_ARG_PENDING` | 5 | `buf=`, or any other unannotated pointer |

**Dependency bits.** `FILC_ASYNC_DEP_READ` (1), `FILC_ASYNC_DEP_WRITE` (2) and
`FILC_ASYNC_DEP_POINTER` (4). The space, a hash of the option's
`<param>:<namespace>`, is in bits `FILC_ASYNC_DEP_NAMESPACE_SHIFT` (8) and
up, masked by `FILC_ASYNC_DEP_NAMESPACE_MASK` (`0x00FFFFFF`). See
[Dependencies](Annotation-Reference.md#dependencies).

## Stub entry points

The pass emits, in each translation unit that calls an annotated function
`F`:

- a stub `__filc_async_stub_F` with `F`'s parameters;
- a run thunk `__filc_async_run_F`.

Every direct call to `F` is redirected to the stub. You do not call these
entry points yourself. They are listed here because they define what a call
does before the runtime sees it.

```c
void* filc_async_begin(const filc_async_meta* meta, void* staged_args);
void  filc_async_lock_word(void* task, uint64_t value, uint32_t space, uint32_t mode);
void  filc_async_lock_ptr(void* task, const void* ptr, uint32_t space, uint32_t mode);
void  filc_async_mark_pending(void* task, void* buf);
void  filc_async_mark_input(void* task, const void* buf);
void  filc_async_submit(void* task, const filc_async_meta* meta,
                        filc_async_run_fn run, void* staged_args, size_t nargs);
```

The stub is equivalent to:

```c
void* __filc_async_stub_F(int fd, void* buf, size_t len, unsigned long off)
{
    void* staged = filc_async_alloc(4 * 16, 16);      /* 16-byte cells */
    /* store fd, buf, len, off into the cells */
    void* task = filc_async_begin(&__filc_meta_F, staged);
    filc_async_lock_word(task, fd, space_fd, FILC_ASYNC_DEP_READ); /* per r_dep/w_dep */
    filc_async_mark_pending(task, buf);                /* per bout=/buf=/unannotated non-const pointer */
    /* filc_async_mark_input(task, src);                  per bin=/unannotated pointer to const */
    filc_async_submit(task, &__filc_meta_F, __filc_async_run_F, staged, 4);
    return task;
}
```

| Function | Behavior |
|---|---|
| `filc_async_begin` | Creates a task in the running state, belonging to `meta->runtime`. A runtime creates tasks of its own with [`filc_async_task_new`](Runtime-API.md#filc_async_task_new) instead. |
| `filc_async_lock_word` / `_ptr` | Queues a read or write lock on a value or object in `space` (the dependency bits minus the mode: the pointer bit and the hash of `<param>:<namespace>`). It waits, polling the holder through the runtime, until the lock is granted. Released when the task completes. |
| `filc_async_mark_pending` | Waits for every other call that owns the object `buf` points into, including calls only reading it, then gives `task` a write mark on it and sets the pending flag in the object's header. A read mark `task` already holds becomes a write mark. With a `NULL` task, the mark has no owner and only `filc_async_mark_resolved` clears it. |
| `filc_async_mark_input` | Gives `task` a read mark on the object `buf` points into, without waiting. The object is not pending (`filc_async_is_pending` says no), but a later `filc_async_mark_pending` on it waits for `task`, and so does the program's first access to it, unless the object is read-only. Does nothing if `task` already has a mark there. Emitted after the output marks, so an argument that is both stays a write. |
| `filc_async_submit` | Hands the call to `meta->runtime->submit`. |
