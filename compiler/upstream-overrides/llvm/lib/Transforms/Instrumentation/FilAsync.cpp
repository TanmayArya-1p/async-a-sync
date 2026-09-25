//===- FilAsync.cpp - filc_async annotation lowering ----------------------===//
//
// Sibling pass of FilPizlonator. Reads the `filc_async` annotations a stock
// clang records for `#pragma clang attribute`, renames each enrolled body to
// `__filc_async_<name>`, emits a `filc_async_meta` global `@__filc_meta_<name>`,
// an options array `@__filc_opts_<name>`, a per-TU `@__filc_async_meta_table`,
// and a constructor calling `filc_async_validate_table` at startup; then
// rewrites every direct call site into the staging alloc + submit sequence and
// erases the consumed annotations. Loadable via
// `opt -load-pass-plugin=libFilAsync.so -passes="filc-async"`.
//
// Annotation entry layout (pre-pizlonation, opaque pointers):
//   @llvm.global.annotations = appending global
//     [1 x { ptr, ptr, ptr, i32, ptr }]
//     [{ ptr, ptr, ptr, i32, ptr }
//       { ptr @procread, ptr @.str, ptr @.str.1, i32 4, ptr @.args }]
//   @.args = { ptr @.str.2, ptr @.str.3, ptr @.str.4 }  (one ptr per option)
// element fields: {target, anno-string, unit-string, line, args}
//
//===----------------------------------------------------------------------===//

#include "FilAsync.h"

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

static cl::opt<bool> FilAsyncDebug(
    "filc-async-debug", cl::init(false),
    cl::desc("Print enrolled filc_async functions and their options"));

// Arg-kind constants; mirror FILC_ASYNC_ARG_* in the runtime header.
static const unsigned ARG_IGNORED = 0;
static const unsigned ARG_BUFFER_IN = 2;
static const unsigned ARG_BUFFER_OUT = 3;
static const unsigned ARG_FD = 4;
static const unsigned ARG_PENDING = 5;

// Result constants; mirror FILC_ASYNC_RESULT_* in the runtime header.
static const unsigned RESULT_WORD = 1;
static const unsigned RESULT_PTR = 2;

// Read a global string constant (the frontend's .str globals); empty
// StringRef when C does not have that shape.
static StringRef underlyingString(Constant *C) {
  C = C->stripPointerCasts();
  auto *GV = dyn_cast<GlobalVariable>(C);
  if (!GV || !GV->hasInitializer())
    return StringRef();
  auto *CDS = dyn_cast<ConstantDataSequential>(GV->getInitializer());
  if (!CDS || !CDS->isString())
    return StringRef();
  // Drop the trailing NUL so the value reads as written ("op=pread").
  StringRef S = CDS->getAsString();
  if (S.size() && S.back() == '\0')
    return S.drop_back();
  return S;
}

// Opaque-pointer clang emits no bitcast on the target; strip defensively.
static Function *extractAnnotatedFunction(Constant *C) {
  Value *V = C->stripPointerCasts();
  return dyn_cast<Function>(V);
}

// Reuse an existing .str global matching string S; make a private copy only
// when none exists (normally never hit).
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

// Only container constants are destroyed downstream: pooled leaves (constant
// ints, string/inert arrays) must never reach destroyConstant/deleteConstant.
static bool isPoolConstant(Constant *C) {
  return isa<ConstantArray, ConstantStruct, ConstantVector, ConstantExpr>(C);
}

// Reads the positional option tokens into Kinds and counts them in Noped.
// op= never decides a kind: fd=<i> -> ARG_FD; bin=<i> -> ARG_BUFFER_IN;
// bout=<i> -> ARG_BUFFER_OUT; buf=<i> -> ARG_PENDING (direction decided at use
// time by the runtime). Unannotated pointer args default to ARG_PENDING (the
// pessimistic "undecided direction" case); unannotated non-pointers stay
// ARG_IGNORED. A token that does not index an argument is a compile-time fatal.
static void parseKinds(StringRef OrigName, FunctionType *FTy,
                       const FilAsyncPass::AnnotInfo &Info,
                       SmallVectorImpl<unsigned> &Kinds, unsigned &Noped) {
  unsigned NArgs = FTy->getNumParams();
  Kinds.assign(NArgs, ARG_IGNORED);
  for (unsigned I = 0; I < NArgs; ++I)
    if (FTy->getParamType(I)->isPointerTy())
      Kinds[I] = ARG_PENDING;
  Noped = 0;
  for (StringRef Opt : Info.opts) {
    unsigned Kind = 0;
    unsigned PrefixLen = 0;
    if (Opt.starts_with("fd=")) {
      Kind = ARG_FD;
      PrefixLen = 3;
    } else if (Opt.starts_with("bin=")) {
      Kind = ARG_BUFFER_IN;
      PrefixLen = 4;
    } else if (Opt.starts_with("bout=")) {
      Kind = ARG_BUFFER_OUT;
      PrefixLen = 5;
    } else if (Opt.starts_with("buf=")) {
      Kind = ARG_PENDING;
      PrefixLen = 4;
    } else {
      continue;
    }
    ++Noped;
    unsigned Idx = 0;
    StringRef Num = Opt.drop_front(PrefixLen);
    if (Num.empty() || Num.getAsInteger(10, Idx) || Idx >= NArgs) {
      errs() << "FilAsync: '" << Opt
             << "' does not index an argument of " << OrigName << "\n";
      report_fatal_error("FilAsync: malformed filc_async option");
    }
    Kinds[Idx] = Kind;
  }
}

// Kinds the pass marks pending before submit: the op produces bytes into them
// (bout=) or their direction is undecided (bare buf= and unannotated pointer
// args, both ARG_PENDING, out by default). bin= const inputs are never marked.
static bool isBufferKind(unsigned Kind) {
  return Kind == ARG_BUFFER_OUT || Kind == ARG_PENDING;
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

  // One i8* per option into the existing .str globals; trailing null i8*.
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

  // Pointer return -> FILC_ASYNC_RESULT_PTR (2); anything else WORD (1).
  unsigned Result = F->getReturnType()->isPointerTy() ? RESULT_PTR : RESULT_WORD;

  // Kinds come from the positional option tokens ONLY -- op= never decides a
  // kind (see parseKinds). noped_args counts the fd=/bin=/bout=/buf= options.
  SmallVector<unsigned, 8> Kinds;
  unsigned Noped;
  parseKinds(OrigName, F->getFunctionType(), Info, Kinds, Noped);

  // The `name` field holds the ORIGINAL name, captured before renameBody in run().
  Constant *NameInit =
      ConstantDataArray::getString(Ctx, OrigName, /*AddNull=*/true);
  auto *NameGV = new GlobalVariable(M, NameInit->getType(),
                                    /*isConstant=*/true,
                                    GlobalValue::PrivateLinkage, NameInit,
                                    "__filc_async_name_" + OrigName.str());

  // args[] tail: exactly nargs {i32,i32} pairs. Field order matches the C header
  // {name, nargs, noped_args, flags, result, opts, args[]}; the 16-byte
  // pointers and the offsets they imply materialize under FilPizlonator.
  SmallVector<Constant *, 8> ArgCs;
  for (unsigned Kind : Kinds)
    ArgCs.push_back(ConstantStruct::get(
        PairTy, {ConstantInt::get(I32Ty, Kind), ConstantInt::get(I32Ty, 0)}));
  ArrayType *ArgsTy = ArrayType::get(PairTy, NArgs);
  Constant *ArgsInit = ConstantArray::get(ArgsTy, ArgCs);

  StructType *MetaTy = StructType::get(
      Ctx, {PtrTy, I32Ty, I32Ty, I32Ty, I32Ty, PtrTy, ArgsTy},
      /*isPacked=*/false);
  // Real pointer-typed constants (no ptrtoint i64): InvisiCap relocation
  // requires pointer fields to be addressable.
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

  // Fixed internal name (legal across TUs); pointer array terminated by null.
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

  // User ctors (validator installation) run first; this ctor is deliberately
  // LAST.
  appendToGlobalCtors(M, Ctor, /*Priority=*/65535);
}

void FilAsyncPass::rewriteCallSites(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  Type *VoidTy = Type::getVoidTy(Ctx);
  PointerType *PtrTy = PointerType::getUnqual(Ctx);

  // Contract: alloc(size: i64, align: i64) -> ptr;
  // submit(meta, impl, opts, staging, nargs) -> ptr.
  FunctionCallee AllocCallee =
      M.getOrInsertFunction("filc_async_alloc",
                            FunctionType::get(PtrTy, {Int64Ty, Int64Ty}, false));
  FunctionCallee SubmitCallee = M.getOrInsertFunction(
      "filc_async_submit", FunctionType::get(
                               PtrTy, {PtrTy, PtrTy, PtrTy, PtrTy, Int64Ty},
                               /*isVarArg=*/false));
  // mark_pending(ptr): buffer args go pending before the runtime takes them.
  FunctionCallee MarkCallee = M.getOrInsertFunction(
      "filc_async_mark_pending",
      FunctionType::get(VoidTy, {PtrTy}, /*isVarArg=*/false));

  for (const auto &KV : Annotated) {
    Function *F = const_cast<Function *>(KV.first);
    auto DesIt = Emitted.find(F);
    if (DesIt == Emitted.end()) {
      // Descriptors were always emitted for enrolled entries; guard anyway so a
      // reader/emitter divergence names the function instead of null.
      errs() << "FilAsync: no descriptor emitted for " << F->getName() << "\n";
      report_fatal_error("FilAsync: internal error: missing descriptor");
    }
    GlobalVariable *Meta = DesIt->second.Meta;
    GlobalVariable *Opts = DesIt->second.Opts;

    // Snapshot the direct CallBase users first; rewriting does not erase F, but
    // transform on a stable set rather than walking F->users().
    SmallVector<CallBase *, 8> DirectCalls;
    for (User *U : F->users()) {
      if (auto *CB = dyn_cast<CallBase>(U))
        if (CB->getCalledOperand() == F)
          DirectCalls.push_back(CB);
    }

    unsigned NArgs = F->getFunctionType()->getNumParams();
    for (CallBase *CB : DirectCalls) {
      // Decide result handling BEFORE inserting anything: a non-void non-ptr
      // return cannot take submit's ptr result, so skip the site entirely
      // (diagnostic only). Inserting first and then leaving the old call
      // would run the async op alongside the synchronous one.
      bool IsVoid = CB->getType()->isVoidTy();
      if (!IsVoid && !CB->getType()->isPointerTy()) {
        errs() << "FilAsync: call to " << F->getName() << " returns "
               << *CB->getType()
               << " but filc_async_submit returns ptr; call left in place\n";
        continue;
      }

      IRBuilder<> Builder(CB);
      // Staging: nargs filc_ptr slots x 16 bytes, 16-byte aligned (two i64
      // words per slot: intval + capability lower word).
      Value *Staging = Builder.CreateCall(
          AllocCallee,
          {ConstantInt::get(Int64Ty, NArgs * 16), ConstantInt::get(Int64Ty, 16)},
          "staging");

      // per-arg: intval at slot i*2, capability zero-word at i*2+1. Pointers
      // become ptrtoint i64; integers up to 64 bits are zero-extended
      // (i32 fd/flags/mode -> i64) or passed through at exactly i64; wider
      // or otherwise unrepresentable ints are recorded as an ignored zero.
      for (unsigned I = 0; I < NArgs; ++I) {
        Value *Arg = CB->getArgOperand(I);
        Type *ArgTy = Arg->getType();
        Value *IntVal;
        if (ArgTy->isPointerTy()) {
          IntVal = Builder.CreatePtrToInt(Arg, Int64Ty);
        } else if (ArgTy->isIntegerTy() && ArgTy->getIntegerBitWidth() < 64) {
          IntVal = Builder.CreateZExt(Arg, Int64Ty);
        } else if (ArgTy->isIntegerTy() && ArgTy->getIntegerBitWidth() == 64) {
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

      // Mark producing buffer args (bout=, bare buf=) pending before the
      // async runtime takes ownership; the resolve wrapper clears them on
      // request. bin= const inputs are never marked.
      SmallVector<unsigned, 8> &Kinds = DesIt->second.Kinds;
      for (unsigned I = 0; I < NArgs; ++I) {
        if (!isBufferKind(Kinds[I]))
          continue;
        Value *Arg = CB->getArgOperand(I);
        if (!Arg->getType()->isPointerTy())
          continue;
        Builder.CreateCall(MarkCallee, {Arg});
      }

      // Opaque-pointer `ptr` operands -- no bitcasts; the globals and the
      // renamed implementation pass straight through. The returned flight pair
      // is the pending result pointer whose deref triggers the existing
      // filc_resolve_pending hook; FilPizlonator pizlonates the return.
      CallInst *Submit = Builder.CreateCall(
          SubmitCallee,
          {Meta, F, Opts, Staging, ConstantInt::get(Int64Ty, NArgs)},
          "async_result");

      if (IsVoid) {
        CB->eraseFromParent();
      } else {
        // Replacement is a pure swap: both the fixture return and submit are
        // opaque `ptr`. Non-ptr non-void was rejected above, pre-insertion.
        CB->replaceAllUsesWith(Submit);
        CB->eraseFromParent();
      }
    }
  }
}

void FilAsyncPass::eraseAnnotations(Module &M) {
  // Erasing llvm.global.annotations alone is not enough: its initializer is a
  // pool-uniqued ConstantArray whose operand uses keep the .args/.str globals
  // alive for as long as the enclosing constants are referenced by the pool.
  // Sever GA from its initializer, then destroy the constant graph top-down
  // (parent-before-child); each node is freed only once genuinely use_empty,
  // which keeps shared leaves alive -- the opts arrays still use the option
  // .str globals, so those stay. Only GlobalVariables are erased from the
  // module; Functions/aliases are never touched.
  GlobalVariable *GA = M.getNamedGlobal("llvm.global.annotations");
  if (!GA || !GA->hasInitializer())
    return;

  Constant *Root = GA->getInitializer();
  GA->setInitializer(nullptr);
  GA->eraseFromParent();

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
      // GlobalVariables in the annotation graph become use-empty once the
      // constants around them are destroyed. Functions/aliases never are.
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
    // Nothing annotated is not an error; the backend never sees a filc_async
    // function on this module.
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

    // No op= validation here: the op set is the runtime's authority, enforced
    // by its startup validator (filc_async_validate_table), not the compiler's.

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

  // Deterministic emission order (table and globals follow name order; the
  // map is keyed on Function*).
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
    SmallVector<unsigned, 8> Kinds;
    unsigned Noped;
    parseKinds(OrigName, F->getFunctionType(), *KV.second, Kinds, Noped);
    renameBody(F, OrigName);
    GlobalVariable *Opts = emitOpts(M, OrigName, *KV.second);
    GlobalVariable *Meta = emitMeta(F, OrigName, *KV.second, Opts);
    Emitted[F] = {Opts, Meta, OrigName, std::move(Kinds)};
    Metas.push_back(Meta);
  }
  if (!Metas.empty())
    emitMetaTableAndCtor(M, Metas);

  // Rewrite every direct call site, then drop the consumed annotations so
  // FilPizlonator never sees them. Only when the pass actually enrolled
  // entries: a module whose llvm.global.annotations holds nothing for us
  // keeps it for other users.
  if (!Annotated.empty()) {
    rewriteCallSites(M);
    eraseAnnotations(M);
  }

  // The pass mutated the module; everything else must be recomputed.
  return PreservedAnalyses::none();
}

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