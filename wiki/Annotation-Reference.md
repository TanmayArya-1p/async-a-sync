# Annotation reference

This page lists the `filc_async` annotation options and how the FilAsync pass
encodes each one in a function's descriptor (`filc_async_meta`, see the
[Framework API](Framework-API.md#descriptor)). For a task-oriented introduction,
see [Annotate a function](Annotating-Functions.md).

## Syntax

```c
#pragma clang attribute push(__attribute__((annotate("filc_async", "<option>", ...))), apply_to=function)
<function declarations or definitions>
#pragma clang attribute pop
```

- **Options.** Each option is a separate string literal. `runtime=` is
  required; the others are optional.
- **Argument indices.** They are zero-based positions in the function's
  parameter list.
- **Placement.** The annotation can be on a declaration, a definition, or both.
  When both carry it, the definition's options apply.

## Options

| Option | Argument type | Arg kind recorded | Effect |
|---|---|---|---|
| `runtime=<name>` | none | none | **Required.** The runtime that runs the call: `meta->runtime` points to `filc_async_runtime_<name>`, which the program must link. `<name>` is a C identifier. The io_uring runtime is `runtime=io_uring`. |
| `op=<name>` | none | none | Names the operation. Passed to the runtime in `meta->opts`, and never read by the compiler or the framework. |
| `bin=<i>` | pointer | `FILC_ASYNC_ARG_BUFFER_IN` | The call reads argument *i*. Not marked pending. |
| `bout=<i>` | pointer | `FILC_ASYNC_ARG_BUFFER_OUT` | The call writes argument *i*. Marked pending until the call completes. |
| `buf=<i>` | pointer | `FILC_ASYNC_ARG_PENDING` | Direction unknown; treated as written. Marked pending. |
| `r_dep=<i>[:<name>]` | pointer, or integer ≤ 64 bits | none (dependency bits) | The call reads the resource keyed by argument *i*. |
| `w_dep=<i>[:<name>]` | pointer, or integer ≤ 64 bits | none (dependency bits) | The call writes the resource keyed by argument *i*. |
| anything else | none | none | Copied into `meta->opts` for the runtime. |

### Arguments without an option

These kinds only record what an annotation says. `op=` never changes them.

| Argument | Kind recorded | Marked pending? |
|---|---|---|
| unannotated pointer | `FILC_ASYNC_ARG_PENDING` | yes (the pessimistic default) |
| unannotated non-pointer | `FILC_ASYNC_ARG_IGNORED` | no |

## Dependencies

Each dependency argument's value is a key, and the stub takes a lock on it.
Read locks are shared and write locks are exclusive. Requests for one lock
are granted in the order they were made.

| Key type | Compared by |
|---|---|
| integer | its value, zero-extended to 64 bits |
| pointer | the base (lower bound) of the Fil-C object it points into |

A pointer key never matches an integer key.

**Namespaces.** A `:<name>` suffix gives the key a namespace. Equal values in
different namespaces are different keys. A key without a name is in the
default namespace, which differs from every named one.

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
| 8–31 | namespace: 24-bit FNV-1a hash of `<name>` (never 0), or 0 for none |

The namespace hash is 32-bit FNV-1a over the name's bytes, masked to 24 bits,
with 0 mapped to 1.

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
| `'<opt>' does not index an argument of <f>` | index missing, not a number, or ≥ the parameter count |
| `'<opt>' names argument <i> of <f>, which is not a pointer` | `bin=`, `bout=` or `buf=` on a non-pointer |
| `dependency argument <i> of <f> must be a pointer or an integer up to 64 bits` | `r_dep=`/`w_dep=` on a float, struct, or wide integer |
| `conflicting dependencies on argument <i> of <f>` | two dependency options on one argument disagree in mode or namespace |
| `'<opt>' has an empty namespace name` | `r_dep=0:` |
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
