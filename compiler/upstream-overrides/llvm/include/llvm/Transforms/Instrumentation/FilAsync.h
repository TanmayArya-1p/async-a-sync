#ifndef FILASYNC_H
#define FILASYNC_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"

#include <map>

namespace llvm {

class Function;
class GlobalVariable;
class Module;

// FilAsync, the sibling pass of FilPizlonator: reads stock-clang
// `#pragma clang attribute` + `__attribute__((annotate("filc_async", ...)))`
// annotations on a pre-pizlonated module, records the per-function options,
// and emits the def-site descriptors: renames each enrolled body to
// `__filc_async_<name>`, emits a `filc_async_meta` global `@__filc_meta_<name>`
// and an options array `@__filc_opts_<name>`, and a per-TU meta table plus a
// startup constructor that feeds it to `filc_async_validate_table`. Task 4
// then rewrites every DIRECT call of an enrolled function into the staging
// alloc + intval/capability stores + `@filc_async_submit` sequence and erases
// the consumed `llvm.global.annotations` (and use-empty `.args`/`.str`
// globals) so FilPizlonator never sees them.
//
// The annotation entry layout this parses (fixed by Task 1):
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

  // Reader: fills Annotated from llvm.global.annotations and validates every
  // filc_async annotation it finds. Returns true on success (including the
  // not-annotated case). On a malformed annotation names the offending
  // function on errs() and returns false; the caller turns that into the
  // compile-time fatal.
  bool enrollAnnotatedFunctions(Module &M);

  const AnnotInfo *getAnnotInfo(const Function *F) const;

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
  static bool isRequired() { return true; }

  // Def-site emission (Task 3):
  // Opts: `@__filc_opts_<name>` = internal [<nopts+1> x ptr] of i8* pointers to
  // the annotation's existing .str globals (Ruling-6: no duplicate strings),
  // trailing null.
  GlobalVariable *emitOpts(Module &M, StringRef OrigName, const AnnotInfo &Info);
  // Meta: `@__filc_meta_<name>` = internal constant typed exactly like the C
  // `filc_async_meta` {name, nargs, noped_args, flags, result, opts, args[]}
  // with pointer fields as real pointers (Ruling-8/Ruling-9).
  GlobalVariable *emitMeta(Function *F, StringRef OrigName, const AnnotInfo &Info,
                           GlobalVariable *Opts);
  // Renames F to `__filc_async_<OrigName>` (body stays in place, callers
  // retarget automatically because the same Function is renamed).
  void renameBody(Function *F, StringRef OrigName);
  // `@__filc_async_meta_table` (internal [<n+1> x ptr] of metas + null) plus
  // internal `void()` ctor calling filc_async_validate_table(bitcast table to
  // i8*), appended to llvm.global_ctors at priority 65535 (Ruling-3).
  void emitMetaTableAndCtor(Module &M, SmallVectorImpl<GlobalVariable *> &Metas);

  // Call-site rewriting + annotation erasure (Task 4): for every enrolled
  // function, each DIRECT CallBase user (callee operand IS the function) is
  // replaced by `@filc_async_alloc(<nargs*16>, 16)` + one `{intval,
  // capability-zero}` i64 pair per parameter (Ruling-4, Ruling-3 token map) +
  // `@filc_async_submit(meta, impl, opts, staging, nargs)`. Non-direct users
  // (address-taken, blockaddress, ...) are skipped silently. eraseAnnotations
  // drops llvm.global.annotations and every global that becomes use-empty as
  // a result (.args, annotation-marker/.source .str) -- never a global with
  // other users, so the opts arrays keep the option .str globals alive (R8).
  void rewriteCallSites(Module &M);
  void eraseAnnotations(Module &M);

private:
  std::map<const Function *, AnnotInfo> Annotated;

  // Task 4: per-enrolled-function emission results, so rewriteCallSites can
  // reference the exact metas/opts globals without re-deriving names.
  struct Descriptors {
    GlobalVariable *Opts;
    GlobalVariable *Meta;
  };
  std::map<const Function *, Descriptors> Emitted;
};

} // namespace llvm

#endif // FILASYNC_H