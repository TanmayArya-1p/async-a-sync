#ifndef FILASYNC_H
#define FILASYNC_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"

#include <map>

namespace llvm {

class Function;
class Module;

// FilAsync, the sibling pass of FilPizlonator: reads stock-clang
// `#pragma clang attribute` + `__attribute__((annotate("filc_async", ...)))`
// annotations on a pre-pizlonated module and records the per-function options
// they carry. Task 2 implements only the reader; call-site rewriting is added
// by later tasks.
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

  // Dev-phase switch: when true, prints "enrolled <name>" and each parsed
  // option to errs() so the reader is observable under `opt`.
  bool Debug = true;

private:
  std::map<const Function *, AnnotInfo> Annotated;
};

} // namespace llvm

#endif // FILASYNC_H