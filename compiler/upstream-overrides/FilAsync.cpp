//===- FilAsync.cpp - reader for filc_async annotations ------------------===//
//
// FilAsync is the sibling pass of FilPizlonator: it reads the annotations a
// stock clang records for `#pragma clang attribute` +
// `__attribute__((annotate("filc_async", ...)))` and turns them into the
// per-function options a later task's call-site rewriting consumes. Task 2
// built the reader; Task 3 adds the def-site emission: each enrolled body is
// renamed to `__filc_async_<name>`, and the pass emits a `filc_async_meta`
// global `@__filc_meta_<name>`, an options array `@__filc_opts_<name>`, and a
// per-TU `@__filc_async_meta_table` plus a generated constructor that passes
// the table to `filc_async_validate_table` at startup. This file is wired up
// so `opt` can load it as a pass plugin and run it under
// `-passes="filc-async"`.
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
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

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

// Ruling-2 (Task 4): the reader's "enrolled ..." lines print only under
// `-filc-async-debug`, default OFF, so the future real clang driver (Task 5)
// stays quiet. opt dlopens the plugin mid-parse (the `-load-pass-plugin`
// callback fires while the command line is still being consumed), so this
// file-local option is registered by the time a trailing `-filc-async-debug`
// token is looked up.
static cl::opt<bool> FilAsyncDebug(
    "filc-async-debug", cl::init(false),
    cl::desc("Print enrolled filc_async functions and their options"));

// Ruling-4 kind constants (mirror FILC_ASYNC_ARG_* in the plan's runtime
// header).
static const unsigned ARG_IGNORED = 0;
static const unsigned ARG_BUFFER_IN = 2;
static const unsigned ARG_BUFFER_OUT = 3;
static const unsigned ARG_FD = 4;

// Ruling-2 result constants.
static const unsigned RESULT_WORD = 1;
static const unsigned RESULT_PTR = 2;

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

// Returns the existing global that holds the bytes of string constant S, if
// one exists. Ruling-6: the opts array reuses the annotation's .str globals
// instead of duplicating their contents; only when no such global is found
// does this create a private copy (defensive; the reader's StringRefs always
// originate from a module global, so this fallback is normally never hit).
static Constant *findOrCreateStringGlobal(Module &M, StringRef S) {
  for (GlobalVariable &GV : M.globals()) {
    if (!GV.hasInitializer())
      continue;
    auto *CDS = dyn_cast<ConstantDataSequential>(GV.getInitializer());
    if (!CDS || !CDS->isString())
      continue;
    StringRef GS = CDS->getAsString();
    if (!GS.empty() && GS.back() == '\0')
      GS = GS.drop_back();
    if (GS == S)
      return &GV;
  }
  LLVMContext &Ctx = M.getContext();
  Constant *Init = ConstantDataArray::getString(Ctx, S, /*AddNull=*/true);
  return new GlobalVariable(M, Init->getType(), /*isConstant=*/true,
                            GlobalValue::PrivateLinkage, Init,
                            ".filc_async.opt");
}

// The annotation graph's leaf scalars/int-pool constants (ConstantInt etc.) are
// value-cached and must NOT be destroyed via Constant::destroyConstant (their
// impls abort). Pool-owned ConstantDataSequential (string bytes) and
// ConstantAggregateZero are likewise non-destroyable: destroyConstantImpl
// erases the owning unique_ptr, so deleteConstant afterwards reads freed
// memory. Only the container constants -- whose *operand-uses* pin orphaned
// globals alive -- are destroyed; they are unlinked from FoldingSet pools
// without being deleted, so the follow-up deleteConstant is safe.
static bool isPoolConstant(Constant *C) {
  return isa<ConstantArray, ConstantStruct, ConstantVector, ConstantExpr>(C);
}

} // anonymous namespace

const FilAsyncPass::AnnotInfo *
FilAsyncPass::getAnnotInfo(const Function *F) const {
  auto It = Annotated.find(F);
  return It == Annotated.end() ? nullptr : &It->second;
}

void FilAsyncPass::renameBody(Function *F, StringRef OrigName) {
  F->setName("__filc_async_" + OrigName.str());
}

GlobalVariable *
FilAsyncPass::emitOpts(Module &M, StringRef OrigName, const AnnotInfo &Info) {
  LLVMContext &Ctx = M.getContext();
  PointerType *PtrTy = PointerType::getUnqual(Ctx);

  // Each option is one i8* into the frontend's existing .str global; the
  // array gets a trailing null i8* (Ruling-6, Ruling-9).
  SmallVector<Constant *, 8> Elems;
  for (StringRef Opt : Info.opts)
    Elems.push_back(findOrCreateStringGlobal(M, Opt));
  Elems.push_back(ConstantPointerNull::get(PtrTy));

  ArrayType *OptsTy = ArrayType::get(PtrTy, Elems.size());
  Constant *Init = ConstantArray::get(OptsTy, Elems);
  return new GlobalVariable(M, OptsTy, /*isConstant=*/true,
                            GlobalValue::InternalLinkage, Init,
                            "__filc_opts_" + OrigName.str());
}

GlobalVariable *FilAsyncPass::emitMeta(Function *F, StringRef OrigName,
                                       const AnnotInfo &Info,
                                       GlobalVariable *Opts) {
  Module &M = *F->getParent();
  LLVMContext &Ctx = M.getContext();
  Type *PtrTy = PointerType::getUnqual(Ctx);
  IntegerType *I32Ty = Type::getInt32Ty(Ctx);
  StructType *PairTy = StructType::get(Ctx, {I32Ty, I32Ty}, false);

  unsigned NArgs = F->getFunctionType()->getNumParams();

  // Ruling-2: pointer return -> FILC_ASYNC_RESULT_PTR (2); anything else gets
  // FILC_ASYNC_RESULT_WORD (1) rather than the brief's NONE/0.
  unsigned Result = F->getReturnType()->isPointerTy() ? RESULT_PTR : RESULT_WORD;

  // Ruling-4: kinds come from the option tokens. fd=<i> -> ARG_FD{4,0};
  // buf=<i> -> ARG_BUFFER_OUT{3,0} for op=pread else ARG_BUFFER_IN{2,0};
  // everything else stays ARG_IGNORED{0,0}. noped_args counts fd=/buf= opts.
  StringRef Op;
  for (StringRef Opt : Info.opts)
    if (Opt.starts_with("op="))
      Op = Opt.drop_front(3);

  SmallVector<unsigned, 8> Kinds(NArgs, ARG_IGNORED);
  unsigned Noped = 0;
  for (StringRef Opt : Info.opts) {
    unsigned Kind = 0;
    unsigned PrefixLen = 0;
    if (Opt.starts_with("fd=")) {
      Kind = ARG_FD;
      PrefixLen = 3;
      ++Noped;
    } else if (Opt.starts_with("buf=")) {
      Kind = (Op == "pread") ? ARG_BUFFER_OUT : ARG_BUFFER_IN;
      PrefixLen = 4;
      ++Noped;
    } else {
      continue;
    }
    unsigned Idx = 0;
    StringRef Num = Opt.drop_front(PrefixLen);
    if (Num.empty() || Num.getAsInteger(10, Idx) || Idx >= NArgs) {
      errs() << "FilAsync: '" << Opt
             << "' does not index an argument of " << OrigName << "\n";
      report_fatal_error("FilAsync: malformed filc_async option");
    }
    Kinds[Idx] = Kind;
  }

  // The `name` field is a fresh string holding the ORIGINAL function name
  // (captured before renameBody in run()).
  Constant *NameInit =
      ConstantDataArray::getString(Ctx, OrigName, /*AddNull=*/true);
  auto *NameGV = new GlobalVariable(M, NameInit->getType(),
                                    /*isConstant=*/true,
                                    GlobalValue::PrivateLinkage, NameInit,
                                    "__filc_async_name_" + OrigName.str());

  // args[] flexible tail: exactly nargs {i32,i32} pairs (spec §2). Field order
  // is the C header's {name, nargs, noped_args, flags, result, opts, args[]};
  // the 16-byte pointers and the resulting offsets materialize later under
  // FilPizlonator (Ruling-5), not here.
  SmallVector<Constant *, 8> ArgCs;
  for (unsigned Kind : Kinds)
    ArgCs.push_back(ConstantStruct::get(
        PairTy, {ConstantInt::get(I32Ty, Kind), ConstantInt::get(I32Ty, 0)}));
  ArrayType *ArgsTy = ArrayType::get(PairTy, NArgs);
  Constant *ArgsInit = ConstantArray::get(ArgsTy, ArgCs);

  StructType *MetaTy = StructType::get(
      Ctx, {PtrTy, I32Ty, I32Ty, I32Ty, I32Ty, PtrTy, ArgsTy},
      /*isPacked=*/false);
  // All pointer fields are real pointer-typed constants (no ptrtoint i64): the
  // InvisiCap relocation pass needs them addressable (Ruling-9).
  Constant *Init = ConstantStruct::get(
      MetaTy, {NameGV, ConstantInt::get(I32Ty, NArgs),
               ConstantInt::get(I32Ty, Noped), ConstantInt::get(I32Ty, 0),
               ConstantInt::get(I32Ty, Result), Opts, ArgsInit});
  return new GlobalVariable(M, MetaTy, /*isConstant=*/true,
                            GlobalValue::InternalLinkage, Init,
                            "__filc_meta_" + OrigName.str());
}

void FilAsyncPass::emitMetaTableAndCtor(Module &M,
                                        SmallVectorImpl<GlobalVariable *> &Metas) {
  LLVMContext &Ctx = M.getContext();
  PointerType *PtrTy = PointerType::getUnqual(Ctx);
  Type *VoidTy = Type::getVoidTy(Ctx);

  // Fixed internal name (Ruling-7): legal across translation units, and Task
  // 9's `nm` substring assertion matches it. Array of i8* terminated by null.
  SmallVector<Constant *, 8> Entries;
  for (GlobalVariable *Meta : Metas)
    Entries.push_back(Meta);
  Entries.push_back(ConstantPointerNull::get(PtrTy));
  ArrayType *TableTy = ArrayType::get(PtrTy, Entries.size());
  Constant *TableInit = ConstantArray::get(TableTy, Entries);
  auto *Table =
      new GlobalVariable(M, TableTy, /*isConstant=*/true,
                         GlobalValue::InternalLinkage, TableInit,
                         "__filc_async_meta_table");

  FunctionCallee Validate = M.getOrInsertFunction(
      "filc_async_validate_table",
      FunctionType::get(VoidTy, {PtrTy}, /*isVarArg=*/false));

  Function *Ctor = Function::Create(FunctionType::get(VoidTy, false),
                                    GlobalValue::InternalLinkage,
                                    "__filc_async_ctor", &M);
  BasicBlock *BB = BasicBlock::Create(Ctx, "entry", Ctor);
  IRBuilder<> Builder(BB);
  Builder.CreateCall(Validate, {Table});
  Builder.CreateRetVoid();

  // Ruling-3: the validator-installing user ctor runs first at a lower
  // priority; the table ctor is deliberately LAST.
  appendToGlobalCtors(M, Ctor, /*Priority=*/65535);
}

void FilAsyncPass::rewriteCallSites(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  PointerType *PtrTy = PointerType::getUnqual(Ctx);

  // Ruling-6 signatures: alloc(size: i64, align: i64) -> ptr;
  // submit(meta, impl, opts, staging, nargs) -> ptr.
  FunctionCallee AllocCallee =
      M.getOrInsertFunction("filc_async_alloc",
                            FunctionType::get(PtrTy, {Int64Ty, Int64Ty}, false));
  FunctionCallee SubmitCallee = M.getOrInsertFunction(
      "filc_async_submit", FunctionType::get(
                               PtrTy, {PtrTy, PtrTy, PtrTy, PtrTy, Int64Ty},
                               /*isVarArg=*/false));

  for (const auto &KV : Annotated) {
    Function *F = const_cast<Function *>(KV.first);
    auto DesIt = Emitted.find(F);
    if (DesIt == Emitted.end()) {
      // Task 3 always emitted descriptors for every enrolled entry, so this
      // cannot happen; guard anyway so a reader/emitter divergence names the
      // function instead of dereferencing a null meta.
      errs() << "FilAsync: no descriptor emitted for " << F->getName() << "\n";
      report_fatal_error("FilAsync: internal error: missing descriptor");
    }
    GlobalVariable *Meta = DesIt->second.Meta;
    GlobalVariable *Opts = DesIt->second.Opts;

    // Ruling-7: snapshot the direct CallBase users first. Rewriting a call
    // does not erase F, but transforming while walking F->users() would be
    // fragile; collect-then-transform keeps every rewrite on the same basis.
    SmallVector<CallBase *, 8> DirectCalls;
    for (User *U : F->users()) {
      if (auto *CB = dyn_cast<CallBase>(U))
        if (CB->getCalledOperand() == F)
          DirectCalls.push_back(CB);
    }

    unsigned NArgs = F->getFunctionType()->getNumParams();
    for (CallBase *CB : DirectCalls) {
      IRBuilder<> Builder(CB);
      // Staging: nargs filc_ptr slots x 16 bytes, 16-byte alignment (two i64
      // words per slot: intval + capability lower word).
      Value *Staging = Builder.CreateCall(
          AllocCallee,
          {ConstantInt::get(Int64Ty, NArgs * 16), ConstantInt::get(Int64Ty, 16)},
          "staging");

      // Ruling-4: per parameter i, store the intval at slot i*2 and the
      // (explicit) capability zero-word at slot i*2+1. Pointers become
      // ptrtoint i64; unsigned integers are zero-extended (i32 fd/flags/mode
      // -> i64); anything unrepresentable is recorded as an ignored zero.
      for (unsigned I = 0; I < NArgs; ++I) {
        Value *Arg = CB->getArgOperand(I);
        Type *ArgTy = Arg->getType();
        Value *IntVal;
        if (ArgTy->isPointerTy()) {
          IntVal = Builder.CreatePtrToInt(Arg, Int64Ty);
        } else if (ArgTy->isIntegerTy() && ArgTy->getIntegerBitWidth() < 64) {
          IntVal = Builder.CreateZExt(Arg, Int64Ty);
        } else if (ArgTy->isIntegerTy()) {
          IntVal = Arg;
        } else {
          IntVal = ConstantInt::get(Int64Ty, 0);
        }
        Value *IntGEP = Builder.CreateGEP(
            Int64Ty, Staging, {ConstantInt::get(Int64Ty, I * 2)});
        Builder.CreateStore(IntVal, IntGEP);
        Value *CapGEP = Builder.CreateGEP(
            Int64Ty, Staging, {ConstantInt::get(Int64Ty, I * 2 + 1)});
        Builder.CreateStore(ConstantInt::get(Int64Ty, 0), CapGEP);
      }

      // R3: opaque-pointer `ptr` operands -- no bitcasts, the globals and the
      // renamed implementation pass straight through. The returned flight pair
      // is the pending result pointer whose deref triggers the existing
      // filc_resolve_pending hook; FilPizlonator pizlonates the return.
      CallInst *Submit = Builder.CreateCall(
          SubmitCallee,
          {Meta, F, Opts, Staging, ConstantInt::get(Int64Ty, NArgs)},
          "async_result");

      if (CB->getType()->isVoidTy()) {
        CB->eraseFromParent();
      } else if (CB->getType() == Submit->getType()) {
        // Ruling-5: the fixture's annotated functions return ptr, so result
        // types match; replacement is a pure swap.
        CB->replaceAllUsesWith(Submit);
        CB->eraseFromParent();
      } else {
        // Non-ptr non-void result: submit's ptr result cannot stand in for it.
        // Prototype scope has no such calls; leave the old call in place so
        // the module stays valid and name the function.
        errs() << "FilAsync: call to " << F->getName() << " returns "
               << *CB->getType()
               << " but filc_async_submit returns ptr; call left in place\n";
      }
    }
  }
}

void FilAsyncPass::eraseAnnotations(Module &M) {
  // R8: erase the consumed annotation infrastructure and nothing else.
  //
  // Erasing llvm.global.annotations alone is not enough: its initializer is a
  // pool-uniqued ConstantArray whose operand uses (-> the per-entry struct
  // constants -> the .args / .str / .str.1 globals) survive the GlobalVariable
  // for as long as the enclosing constants are referenced by the constant
  // pool, so those globals keep spurious users and would be left behind. We
  // therefore sever GA from its initializer and destroy the whole annotation
  // constant graph top-down (a destroying parent drops its children's use
  // counts; each node is freed only once it is genuinely use_empty -- this
  // keeps shared leaves alive: the opts arrays are live users of the option
  // .str globals, so those stay). Only GlobalVariable leaves are erased from
  // the module; Functions/aliases (including the enrolled impls) are left
  // untouched, and special globals like llvm.global_ctors are unreachable
  // from the annotation graph and are never considered.
  GlobalVariable *GA = M.getNamedGlobal("llvm.global.annotations");
  if (!GA || !GA->hasInitializer())
    return;

  Constant *Root = GA->getInitializer();
  GA->setInitializer(nullptr);
  GA->eraseFromParent();

  // Pre-order worklist gives parents before children.
  SmallVector<Constant *, 16> Worklist;
  SmallPtrSet<Constant *, 16> Seen;
  Seen.insert(Root);
  Worklist.push_back(Root);
  for (size_t I = 0; I < Worklist.size(); ++I) {
    Constant *C = Worklist[I];
    for (Use &U : C->operands()) {
      if (auto *Child = dyn_cast<Constant>(U.get()))
        if (Seen.insert(Child).second)
          Worklist.push_back(Child);
    }
  }

  for (Constant *C : Worklist) {
    if (auto *GV = dyn_cast<GlobalValue>(C)) {
      // GlobalVariables in the annotation graph (.args and the marker/source
      // .str globals) become use-empty once the constants around them are
      // destroyed. Functions (the enrolled impls) and GlobalAliases/IFuncs
      // are never erased.
      if (auto *GVar = dyn_cast<GlobalVariable>(GV))
        if (GVar->use_empty())
          GVar->eraseFromParent();
    } else if (isPoolConstant(C) && C->use_empty()) {
      C->destroyConstant();
    }
  }
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

    if (FilAsyncDebug) {
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

  // Deterministic emission order (table and globals follow source order via
  // name sort; std::map is keyed on Function*).
  SmallVector<std::pair<Function *, const AnnotInfo *>, 8> Work;
  for (const auto &KV : Annotated)
    Work.emplace_back(const_cast<Function *>(KV.first), &KV.second);
  llvm::sort(Work, [](const auto &A, const auto &B) {
    return A.first->getName() < B.first->getName();
  });

  SmallVector<GlobalVariable *, 8> Metas;
  for (auto &KV : Work) {
    Function *F = KV.first;
    // The meta's name field holds the ORIGINAL name, so copy it before the
    // rename invalidates F's name storage.
    std::string OrigName = F->getName().str();
    renameBody(F, OrigName);
    GlobalVariable *Opts = emitOpts(M, OrigName, *KV.second);
    GlobalVariable *Meta = emitMeta(F, OrigName, *KV.second, Opts);
    Emitted[F] = {Opts, Meta};
    Metas.push_back(Meta);
  }
  if (!Metas.empty())
    emitMetaTableAndCtor(M, Metas);

  // Task 4: rewrite every direct call site into the staging+submit sequence,
  // then drop the consumed annotations so FilPizlonator never sees them. Only
  // when the pass actually enrolled filc_async entries: a module whose
  // llvm.global.annotations holds nothing for us keeps it for other users.
  if (!Annotated.empty()) {
    rewriteCallSites(M);
    eraseAnnotations(M);
  }

  // Emission mutated the module; everything else must be recomputed.
  return PreservedAnalyses::none();
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