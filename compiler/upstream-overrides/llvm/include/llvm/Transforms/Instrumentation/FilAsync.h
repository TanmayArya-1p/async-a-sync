#ifndef FILASYNC_H
#define FILASYNC_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"

#include <map>
#include <string>

namespace llvm {

class Function;
class GlobalVariable;
class Module;

// FilAsync, the sibling pass of FilPizlonator. Reads the `filc_async`
// annotations (`#pragma clang attribute` + `annotate("filc_async", ...)`) a
// stock clang records on a pre-pizlonated module, renames each enrolled body
// to `__filc_async_<name>`, emits a `filc_async_meta` global
// `@__filc_meta_<name>` and an options array `@__filc_opts_<name>`, and a
// per-TU meta table plus a startup constructor feeding it to
// `filc_async_validate_table`. Direct calls of an enrolled function are then
// rewritten into the staging alloc + intval/capability stores +
// `filc_async_mark_pending` (per producing buffer arg) + `filc_async_submit`
// sequence, and the consumed `llvm.global.annotations`
// (plus its now use-empty `.args`/`.str` globals) are erased so FilPizlonator
// never sees them.
//
// Annotation entry layout:
//   llvm.global.annotations = appending global
//     [1 x { ptr, ptr, ptr, i32, ptr }]
//     [{ ptr, ptr, ptr, i32, ptr }
//       { ptr @procread, ptr @.str, ptr @.str.1, i32 4, ptr @.args }]
//   @.args = { ptr @.str.2, ptr @.str.3, ptr @.str.4 }
// element fields: {target, anno-string, unit-string, line, args}
class FilAsyncPass : public PassInfoMixin<FilAsyncPass> {
public:
  struct AnnotInfo {
    SmallVector<StringRef, 4> opts;
  };

  // Fills Annotated from llvm.global.annotations. Returns true on success
  // (including the not-annotated case); returns false only when an entry does
  // not name a function, naming it on errs() (the caller turns that into a
  // compile-time fatal). The op= set is NOT validated here: the runtime is the
  // authority, enforced by its startup validator (filc_async_validate_table).
  bool enrollAnnotatedFunctions(Module &M);

  const AnnotInfo *getAnnotInfo(const Function *F) const;

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
  static bool isRequired() { return true; }

  // Def-site emission:
  // Opts: `@__filc_opts_<name>` = internal [<nopts+1> x ptr] into the
  // annotation's existing .str globals (no duplicate strings), trailing null.
  GlobalVariable *emitOpts(Module &M, StringRef OrigName, const AnnotInfo &Info);
  // Meta: `@__filc_meta_<name>` = internal constant typed exactly like the C
  // `filc_async_meta` {name, nargs, noped_args, flags, result, opts, args[]},
  // pointer fields as real pointers.
  GlobalVariable *emitMeta(Function *F, StringRef OrigName, const AnnotInfo &Info,
                           GlobalVariable *Opts);
  // Renames a defined F to `__filc_async_<OrigName>` and, unless F is local,
  // keeps `<OrigName>` as an alias of it for callers in other TUs.
  // Declarations retain the ordinary linker name so the implementation can
  // live in another TU.
  void renameBody(Function *F, StringRef OrigName);
  // `@__filc_async_meta_table` (internal [<n+1> x ptr] of metas + null) plus
  // an internal `void()` ctor calling filc_async_validate_table, appended to
  // llvm.global_ctors at priority 65535 (last).
  void emitMetaTableAndCtor(Module &M, SmallVectorImpl<GlobalVariable *> &Metas);

  // Call-site rewriting + annotation erasure: for every enrolled function,
  // each DIRECT CallBase user (callee operand IS the function) is replaced by
  // `@filc_async_alloc(<nargs*16>, 16)` + one {intval, capability-zero} i64
  // pair per parameter + `@filc_async_submit(meta, impl, opts, staging,
  // nargs)`. Non-direct users (address-taken, blockaddress, ...) are skipped
  // silently. eraseAnnotations drops llvm.global.annotations and every global
  // that becomes use-empty as a result (.args, annotation-marker/source
  // .str), never a global with other users, so the opts arrays keep the
  // option .str globals alive.
  void rewriteCallSites(Module &M);
  void eraseAnnotations(Module &M);

private:
  std::map<const Function *, AnnotInfo> Annotated;

  // Per-enrolled-function emission results, so rewriteCallSites can reference
  // the exact metas/opts globals and re-derive kinds without re-parsing.
  struct Descriptors {
    GlobalVariable *Opts;
    GlobalVariable *Meta;
    std::string OrigName;
    SmallVector<unsigned, 8> Kinds;
  };
  std::map<const Function *, Descriptors> Emitted;
};

} // namespace llvm

#endif // FILASYNC_H
