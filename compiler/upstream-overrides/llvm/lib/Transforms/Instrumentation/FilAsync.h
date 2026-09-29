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

// FilAsync, the sibling pass of FilPizlonator. Rewrites the call sites of
// functions annotated with `filc_async` into filc_async_submit, and erases the
// consumed annotations. See
// wiki/2026-09-29-pragma-async-runtime-interface.md for what it emits.
class FilAsyncPass : public PassInfoMixin<FilAsyncPass> {
public:
  struct AnnotInfo {
    SmallVector<StringRef, 4> opts;
  };

  // False only when an annotation does not name a function; the caller turns
  // that into a compile-time fatal. The op= set is not validated here: the
  // linked runtime is the authority.
  bool enrollAnnotatedFunctions(Module &M);

  const AnnotInfo *getAnnotInfo(const Function *F) const;

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
  static bool isRequired() { return true; }

  GlobalVariable *emitOpts(Module &M, StringRef OrigName, const AnnotInfo &Info);
  GlobalVariable *emitMeta(Function *F, StringRef OrigName, const AnnotInfo &Info,
                           GlobalVariable *Opts);
  // Definitions become __filc_async_<name>; declarations keep their name.
  void renameBody(Function *F, StringRef OrigName);
  void emitMetaTableAndCtor(Module &M, SmallVectorImpl<GlobalVariable *> &Metas);

  // Direct call bases only. Pointer args are stored as plain pointers for
  // FilPizlonator to widen, 16 bytes per cell.
  void rewriteCallSites(Module &M);
  void eraseAnnotations(Module &M);

private:
  std::map<const Function *, AnnotInfo> Annotated;

  // Kept so rewriteCallSites can reference the emitted globals directly.
  struct Descriptors {
    GlobalVariable *Opts;
    GlobalVariable *Meta;
    std::string OrigName;
  };
  std::map<const Function *, Descriptors> Emitted;
};

} // namespace llvm

#endif // FILASYNC_H