# Annotation reference

This page lists the `filc_async` annotation options and how the FilAsync pass
encodes each one in a function's descriptor (`filc_async_meta`, see the
[Framework API](Framework-API.md#descriptor)). For a task-oriented introduction,
see [Annotate a function](Annotating-Functions.md).

## Syntax

```c
FILC_ASYNC(<runtime>, <option macro>, ...)
<function declaration or definition>
```

`FILC_ASYNC` and the option macros are in `filc_async_annotate.h`, which
`filc_async.h` includes. They expand to the attribute

```c
__attribute__((annotate("filc_async", "runtime=<runtime>", "<option>", ...)))
```

and that attribute is all the compiler pass reads. The tables below give each
option as the string it produces and the macro that writes it:

| Macro | String |
|---|---|
| `FILC_ASYNC(runtime, ...)` | `"runtime=<runtime>"`, then the options |
| `FILC_OP(op)` | `"op=<op>"` |
| `FILC_BIN(param)` | `"bin=<param>"` |
| `FILC_BOUT(param)` | `"bout=<param>"` |
| `FILC_BUF(param)` | `"buf=<param>"` |
| `FILC_R_DEP(param, ns)` | `"r_dep=<param>:<ns>"` |
| `FILC_W_DEP(param, ns)` | `"w_dep=<param>:<ns>"` |
| `FILC_OPTION(key, value)` | `"<key>=<value>"`, an option only the runtime reads |

The same annotation can be written by hand, around functions, as

```c
#pragma clang attribute push(__attribute__((annotate("filc_async", "<option>", ...))), apply_to=function)
<function declarations or definitions>
#pragma clang attribute pop
```

- **Options.** `runtime=` is required; the others are optional. The macro
  arguments are names, not strings, and the compiler stringizes them, so a
  name that is also a macro is not expanded. `FILC_ASYNC` needs at least one
  option after the runtime, normally `FILC_OP`.
- **One annotation.** A function carries one `FILC_ASYNC`; put every option in
  it. The compiler keeps one annotation per function.
- **Parameters.** Options refer to arguments by parameter name, never by
  position. The names come from the annotated declaration; an unnamed
  parameter takes its name from another declaration of the function, such
  as the definition.
- **Placement.** The annotation can be on a declaration, a definition, or both.
  When both carry it, the definition's options apply.

## Options

| Option | Argument type | Arg kind recorded | Effect |
|---|---|---|---|
| `runtime=<name>` | none | none | **Required.** The runtime that runs the call: `meta->runtime` points to `filc_async_runtime_<name>`, which the program must link. `<name>` is a C identifier. The io_uring runtime is `runtime=io_uring`. |
| `op=<name>` | none | none | Names the operation. Passed to the runtime in `meta->opts`, and never read by the compiler or the framework. |
| `bin=<param>` | pointer | `FILC_ASYNC_ARG_BUFFER_IN` | The call reads `<param>`. Not marked pending. |
| `bout=<param>` | pointer | `FILC_ASYNC_ARG_BUFFER_OUT` | The call writes `<param>`. Marked pending until the call completes. |
| `buf=<param>` | pointer | `FILC_ASYNC_ARG_PENDING` | Direction unknown; treated as written. Marked pending. |
| `r_dep=<param>:<namespace>` | pointer, or integer ≤ 64 bits | none (dependency bits) | The call reads the resource `<param>` names in `<namespace>`. The namespace is required. |
| `w_dep=<param>:<namespace>` | pointer, or integer ≤ 64 bits | none (dependency bits) | The call writes it. |
| anything else | none | none | Copied into `meta->opts` for the runtime. |

### Arguments without an option

An argument no option names takes its kind from its parameter's type. `op=`
never changes it.

| Argument | Kind recorded | Marked pending? |
|---|---|---|
| unannotated pointer to `const` | `FILC_ASYNC_ARG_BUFFER_IN`, as `bin=` | no |
| any other unannotated pointer | `FILC_ASYNC_ARG_PENDING`, as `buf=` | yes (the pessimistic default) |
| unannotated non-pointer | `FILC_ASYNC_ARG_IGNORED` | no |

- **The pointee's `const`.** Only a `const` on the type the pointer points to
  counts, after typedefs: `const char*`, `char const* const*` and `cbyte*`
  (with `typedef const char cbyte`) are inputs; `char* const` and
  `const char**` are not. Compatible declarations of a function always agree
  on it.
- **Options win.** `buf=`, `bout=` or `bin=` on a parameter replaces the kind
  its type gives. Only options count in `noped_args`.
- **Where it comes from.** The patched clang records which parameters point
  to `const` as `!filc_async.const`; IR pointers carry no qualifiers. IR from
  a stock clang has no such record, and its unannotated pointers are all
  `FILC_ASYNC_ARG_PENDING`.

## Dependencies

The stub takes a lock for each dependency option. Read locks are shared and
write locks are exclusive. Requests for one lock are granted in the order
they were made.

**The key** combines three things:

- the argument's value:

  | Parameter type | Value compared |
  |---|---|
  | integer | the value, zero-extended to 64 bits |
  | pointer | the base (lower bound) of the Fil-C object it points into |

- the parameter's name;
- the namespace.

Two calls conflict only when all three match (and one of them writes):

```c
"r_dep=fd:file"      // pread (int fd, ...)
"w_dep=fd:file"      // pwrite(int fd, ...)    conflicts with the pread on the same fd
"w_dep=fd:meta"      // another namespace: independent
"w_dep=handle:file"  // another parameter name: independent
```

So name a resource the same way, with the same parameter name and
namespace, in every function that touches it. A pointer key never matches
an integer key.

**Integer width.** Integer keys are zero-extended from their declared width.
A negative value declared as `int` in one function and as `long` in another
produces different keys. Declare a resource with one integer type
everywhere.

### Encoding in `args[i].dependency`

| Bits | Meaning |
|---|---|
| 0 | `FILC_ASYNC_DEP_READ` |
| 1 | `FILC_ASYNC_DEP_WRITE` |
| 2 | `FILC_ASYNC_DEP_POINTER`: the key is an object, not an integer |
| 8–31 | space: 24-bit FNV-1a hash of `<param>:<namespace>` (never 0) |

The space hash is 32-bit FNV-1a over the bytes of `<param>:<namespace>` (the
option's value), masked to 24 bits, with 0 mapped to 1. The stub passes it,
with the pointer bit, to the lock along with the argument's value.

**Collisions.** Two different `<param>:<namespace>` strings can hash alike.
That only makes unrelated calls wait for each other; it never lets
conflicting calls run together.

## Call-site rewriting

The pass redirects a call to an annotated function to that function's stub
only when the call:

- is a plain `call` (not `invoke` or `callbr`) through the function's own
  prototype;
- returns `void` or a pointer.

Every other call is left as a direct call to the body, with a
`FilAsync: call to <f> ... call left in place` diagnostic. Indirect calls are
never rewritten.

## Errors

These make the compiler stop with `FilAsync: malformed filc_async option`:

| Message | Cause |
|---|---|
| `'<opt>' on <f> names no parameter; use a parameter name` | an index such as `bout=1`, a name no parameter has, or a parameter left unnamed in every declaration |
| `'<opt>' on <f> needs a namespace: <param>:<namespace>` | `r_dep=`/`w_dep=` without `:<namespace>` |
| `'<opt>' has an empty namespace name` | `r_dep=fd:` |
| `'<opt>' names <param> of <f>, which is not a pointer` | `bin=`, `bout=` or `buf=` on a non-pointer |
| `dependency parameter <param> of <f> must be a pointer or an integer up to 64 bits` | `r_dep=`/`w_dep=` on a float, struct, or wide integer |
| `conflicting dependencies on parameter <param> of <f>` | two dependency options on one parameter disagree in mode or namespace |
| `'<opt>' uses an obsolete dependency name` | `read_dep=` / `write_dep=`; use `r_dep=` / `w_dep=` |
| `'fd=<i>' on <f> is no longer an option` | `fd=` was removed: a runtime finds its descriptor itself, by position |
| `<f> names no runtime; add runtime=<name>` | the annotation has no `runtime=` option |
| `'<opt>' on <f> does not name a runtime` | `runtime=` is empty or not a C identifier |
| `<f> names two runtimes, <a> and <b>` | two `runtime=` options with different names |

The linker reports a runtime the program names but does not link:
`undefined reference to pizlonated_filc_async_runtime_<name>`.

At startup, before `main`, `filc_async_validate_table` checks every
descriptor. It aborts with
`filc_async: fatal: function cannot be registered on this runtime` in either
of these cases:

- the dependency bits are malformed;
- the validator rejects the function. The validator is the `validate` of
  the runtime the function names, unless the program installed its own. A
  rejection usually means the runtime does not know the op, or the argument
  kinds do not fit it.
