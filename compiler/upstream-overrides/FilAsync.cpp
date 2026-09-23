//===- FilAsync.cpp - reader for filc_async annotations ------------------===//
//
// FilAsync is the sibling pass of FilPizlonator: it reads the annotations a
// stock clang records for `#pragma clang attribute` +
// `__attribute__((annotate("filc_async", ...)))` and turns them into the
// per-function options a later task's call-site rewriting consumes. This file
// is Task 2's scope: the reader and option parser only, wired up so `opt` can
// load it as a pass plugin and run it under `-passes="filc-async"`.
//
// The annotation entry layout (recorded by Task 1, pre-pizlonation only):
//
//   @.str   = private unnamed_addr constant [11 x i8] c"filc_async\00",
//             section "llvm.metadata"
//   @.str.1 = private unnamed_addr constant [25 x i8] c"tests/....c\00",
//             section "llvm.metadata"
//   @.str.2 = private unnamed_addr constant ... c"op=pread\00"
//   @.args  = private unnamed_addr constant { ptr, ptr, ptr }
//             { ptr @.str.2, ptr @.str.3, ptr @.str.4 }
//   @llvm.global.annotations = appending global
//     [1 x { ptr, ptr, ptr, i32, ptr }]
//     [{ ptr, ptr, ptr, i32, ptr }
//       { ptr @procread, ptr @.str, ptr @.str.1, i32 4, ptr @.args }]
//
// element fields: {target, anno-string, unit-string, line, args}
//
//===----------------------------------------------------------------------===//

#include "FilAsync.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

using namespace llvm;

namespace {

// The compile-time known `op=` set, fixed by Ruling-4: the five real ops plus
// the synthetic `ignore` family (valid but never executed; the backend
// rejects it at the emission site). Any other value, or an empty `op=`, is a
// compile-time fatal naming the function.
static const StringRef KnownOpcodes[] = {
    "pread", "pwrite", "openat", "fsync", "close", "ignore",
};

bool isKnownOpcode(StringRef Op) { return is_contained(KnownOpcodes, Op); }

// Resolve Constant C to a C string, if the frontend emitted it as a global
// string constant (the `.str` globals above). Returns an empty StringRef when
// C does not have that shape.
static StringRef underlyingString(Constant *C) {
  C = C->stripPointerCasts();
  auto *GV = dyn_cast<GlobalVariable>(C);
  if (!GV || !GV->hasInitializer())
    return StringRef();
  auto *CDS = dyn_cast<ConstantDataSequential>(GV->getInitializer());
  if (!CDS || !CDS->isString())
    return StringRef();
  // Full-width constants carry a trailing NUL; drop it so the value reads the
  // way the programmer wrote it ("op=pread", not "op=pread\0").
  StringRef S = CDS->getAsString();
  if (S.size() && S.back() == '\0')
    return S.drop_back();
  return S;
}

// The target field does not carry a bitcast on the opaque-pointer clang, but
// strip defensively anyway; callers report the diagnostic when this fails.
static Function *extractAnnotatedFunction(Constant *C) {
  Value *V = C->stripPointerCasts();
  return dyn_cast<Function>(V);
}

} // anonymous namespace

const FilAsyncPass::AnnotInfo *
FilAsyncPass::getAnnotInfo(const Function *F) const {
  auto It = Annotated.find(F);
  return It == Annotated.end() ? nullptr : &It->second;
}

bool FilAsyncPass::enrollAnnotatedFunctions(Module &M) {
  GlobalVariable *GA = M.getNamedGlobal("llvm.global.annotations");
  if (!GA || !GA->hasInitializer()) {
    // Nothing annotated is not an error; the backend simply never sees a
    // filc_async function on this module.
    return true;
  }

  auto *Array = dyn_cast<ConstantArray>(GA->getInitializer()->stripPointerCasts());
  if (!Array)
    return true;

  for (Value *E : Array->operand_values()) {
    auto *Entry = dyn_cast<ConstantStruct>(E);
    if (!Entry || Entry->getNumOperands() != 5)
      continue;

    if (underlyingString(cast<Constant>(Entry->getOperand(1))) !=
        "filc_async")
      continue;

    Function *F = extractAnnotatedFunction(cast<Constant>(Entry->getOperand(0)));
    if (!F) {
      errs() << "FilAsync: filc_async annotation did not name a function\n";
      return false;
    }

    AnnotInfo Info;
    Value *ArgsTarget = Entry->getOperand(4)->stripPointerCasts();
    if (auto *ArgsGV = dyn_cast<GlobalVariable>(ArgsTarget)) {
      if (ArgsGV->hasInitializer()) {
        if (auto *Args = dyn_cast<ConstantStruct>(ArgsGV->getInitializer())) {
          for (Value *Field : Args->operand_values())
            Info.opts.push_back(underlyingString(cast<Constant>(Field)));
        }
      }
    }

    // Validate (Ruling-4): every `op=` option must be non-empty and one of the
    // compile-time known opcodes; anything else is fatal naming the function.
    for (StringRef Opt : Info.opts) {
      if (!Opt.starts_with("op="))
        continue;
      StringRef OpName = Opt.drop_front(3);
      if (OpName.empty() || !isKnownOpcode(OpName)) {
        errs() << "FilAsync: '" << Opt << "' is not a known filc_async op for "
               << F->getName() << "\n";
        errs() << "FilAsync: known ops are "
                  "pread, pwrite, openat, fsync, close, ignore\n";
        return false;
      }
    }

    if (Debug) {
      errs() << "enrolled " << F->getName() << "\n";
      for (StringRef Opt : Info.opts)
        errs() << "  " << Opt << "\n";
    }
    Annotated[F] = std::move(Info);
  }

  return true;
}

PreservedAnalyses FilAsyncPass::run(Module &M, ModuleAnalysisManager &) {
  if (!enrollAnnotatedFunctions(M))
    report_fatal_error(
        "FilAsync: malformed filc_async annotation; see diagnostics above");
  return PreservedAnalyses::all();
}

// Plugin self-registration (LLVM 20 PassBuilder route) so that
// `opt -load-pass-plugin=libFilAsync.so -passes="filc-async"` runs the pass.
static llvm::PassPluginLibraryInfo getFilAsyncPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "FilAsync", LLVM_VERSION_STRING,
          [](llvm::PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](llvm::StringRef Name, llvm::ModulePassManager &MPM,
                   llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
                  if (Name == "filc-async") {
                    MPM.addPass(FilAsyncPass());
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getFilAsyncPluginInfo();
}