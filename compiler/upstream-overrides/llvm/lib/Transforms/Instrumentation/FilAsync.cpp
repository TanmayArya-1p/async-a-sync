//===- FilAsync.cpp - filc_async annotation lowering ----------------------===//
//
// Sibling pass of FilPizlonator. Lowers the `filc_async` annotations stock
// clang records for `#pragma clang attribute` into meta globals, and rewrites
// every direct call site into a staging alloc plus filc_async_submit.
// Loadable via `opt -load-pass-plugin=libFilAsync.so -passes="filc-async"`.
// See wiki/2026-09-29-pragma-async-runtime-interface.md.
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

#include <cstdint>
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

// Dependency bits: read/write/pointer in the low byte, 24-bit namespace hash
// in bits 8..31, 0 = unnamed.
static const unsigned DEP_NONE = 0;
static const unsigned DEP_READ = 1;
static const unsigned DEP_WRITE = 2;
static const unsigned DEP_POINTER = 4;
static const unsigned DEP_NAMESPACE_SHIFT = 8;
static const unsigned DEP_NAMESPACE_MASK = 0x00FFFFFF;

// Result constants; mirror FILC_ASYNC_RESULT_* in the runtime header.
static const unsigned RESULT_WORD = 1;
static const unsigned RESULT_PTR = 2;

// FNV-1a 24-bit, stable across TUs. A zero hash is forced to 1 so a named key
// never aliases an unnamed one.
static unsigned hashNamespaceName(StringRef Name) {
  uint32_t H = 2166136261u;
  for (unsigned char C : Name.bytes()) {
    H ^= C;
    H *= 16777619u;
  }
  H &= DEP_NAMESPACE_MASK;
  if (H == 0)
    H = 1;
  return H;
}

// Reads the frontend's .str globals; empty when C has that shape.
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

// Reuse a matching .str global rather than duplicating the string.
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

// Container constants only: pooled leaves must never be destroyed.
static bool isPoolConstant(Constant *C) {
  return isa<ConstantArray, ConstantStruct, ConstantVector, ConstantExpr>(C);
}

// Parses the positional tokens into Kinds/Deps and counts kinds in Noped.
// op= never decides a kind; unannotated pointers are ARG_PENDING and
// unannotated non-pointers are ARG_IGNORED. A token that does not index an
// argument is a compile-time fatal.
static void parseKinds(StringRef OrigName, FunctionType *FTy,
                       const FilAsyncPass::AnnotInfo &Info,
                       SmallVectorImpl<unsigned> &Kinds,
                       SmallVectorImpl<unsigned> &Deps, unsigned &Noped) {
  unsigned NArgs = FTy->getNumParams();
  Kinds.assign(NArgs, ARG_IGNORED);
  Deps.assign(NArgs, DEP_NONE);
  for (unsigned I = 0; I < NArgs; ++I)
    if (FTy->getParamType(I)->isPointerTy())
      Kinds[I] = ARG_PENDING;
  Noped = 0;
  for (StringRef Opt : Info.opts) {
    unsigned Kind = 0;
    unsigned Dep = DEP_NONE;
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
    } else if (Opt.starts_with("r_dep=")) {
      Dep = DEP_READ;
      PrefixLen = 6;
    } else if (Opt.starts_with("w_dep=")) {
      Dep = DEP_WRITE;
      PrefixLen = 6;
    } else if (Opt.starts_with("read_dep=") ||
               Opt.starts_with("write_dep=")) {
      errs() << "FilAsync: '" << Opt << "' uses an obsolete dependency name; "
             << "use r_dep= or w_dep=\n";
      report_fatal_error("FilAsync: malformed filc_async option");
    } else {
      continue;
    }

    unsigned Idx = 0;
    StringRef Num = Opt.drop_front(PrefixLen);
    StringRef Namespace;
    size_t Colon = Num.find(':');
    if (Colon != StringRef::npos) {
      Namespace = Num.substr(Colon + 1);
      Num = Num.substr(0, Colon);
    }
    if (Num.empty() || Num.getAsInteger(10, Idx) || Idx >= NArgs) {
      errs() << "FilAsync: '" << Opt
             << "' does not index an argument of " << OrigName << "\n";
      report_fatal_error("FilAsync: malformed filc_async option");
    }

    if (Dep != DEP_NONE) {
      Type *ArgTy = FTy->getParamType(Idx);
      if (!ArgTy->isPointerTy() &&
          (!ArgTy->isIntegerTy() || ArgTy->getIntegerBitWidth() > 64)) {
        errs() << "FilAsync: dependency argument " << Idx << " of "
               << OrigName << " must be a pointer or an integer up to 64 bits\n";
        report_fatal_error("FilAsync: malformed filc_async option");
      }
      unsigned DepVal = Dep | (ArgTy->isPointerTy() ? DEP_POINTER : 0);
      if (!Namespace.empty())
        DepVal |= hashNamespaceName(Namespace) << DEP_NAMESPACE_SHIFT;
      if (Deps[Idx] != DEP_NONE) {
        unsigned Existing = Deps[Idx];
        if ((Existing & (DEP_READ | DEP_WRITE)) != (DepVal & (DEP_READ | DEP_WRITE)) ||
            (Existing >> DEP_NAMESPACE_SHIFT) !=
                (DepVal >> DEP_NAMESPACE_SHIFT)) {
          errs() << "FilAsync: conflicting dependencies on argument " << Idx
                 << " of " << OrigName << "\n";
          report_fatal_error("FilAsync: malformed filc_async option");
        }
      }
      Deps[Idx] = DepVal;
    } else {
      ++Noped;
      Kinds[Idx] = Kind;
    }
  }
}

// Buffer direction is decided at dispatch, not here: the submit stub marks the
// producing args pending, and bin= inputs are never marked.

} // anonymous namespace

const FilAsyncPass::AnnotInfo *
FilAsyncPass::getAnnotInfo(const Function *F) const {
  auto It = Annotated.find(F);
  return It == Annotated.end() ? nullptr : &It->second;
}

void FilAsyncPass::renameBody(Function *F, StringRef OrigName) {
  // Declarations keep their name: the definition may live in another TU.
  if (!F->isDeclaration())
    F->setName("__filc_async_" + OrigName.str());
}

GlobalVariable *
FilAsyncPass::emitOpts(Module &M, StringRef OrigName, const AnnotInfo &Info) {
  LLVMContext &Ctx = M.getContext();
  PointerType *PtrTy = PointerType::getUnqual(Ctx);

  // One i8* per option, trailing null i8*.
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

  // Pointer return -> FILC_ASYNC_RESULT_PTR, anything else WORD.
  unsigned Result = F->getReturnType()->isPointerTy() ? RESULT_PTR : RESULT_WORD;

  SmallVector<unsigned, 8> Kinds;
  SmallVector<unsigned, 8> Deps;
  unsigned Noped;
  parseKinds(OrigName, F->getFunctionType(), Info, Kinds, Deps, Noped);

  // The name field holds the ORIGINAL name; run() renames the body after this.
  Constant *NameInit =
      ConstantDataArray::getString(Ctx, OrigName, /*AddNull=*/true);
  auto *NameGV = new GlobalVariable(M, NameInit->getType(),
                                    /*isConstant=*/true,
                                    GlobalValue::PrivateLinkage, NameInit,
                                    "__filc_async_name_" + OrigName.str());

  // args[] tail: nargs {kind, dependency} pairs, in the C header's field order.
  SmallVector<Constant *, 8> ArgCs;
  for (unsigned I = 0; I < NArgs; ++I)
    ArgCs.push_back(ConstantStruct::get(
        PairTy, {ConstantInt::get(I32Ty, Kinds[I]),
                 ConstantInt::get(I32Ty, Deps[I])}));
  ArrayType *ArgsTy = ArrayType::get(PairTy, NArgs);
  Constant *ArgsInit = ConstantArray::get(ArgsTy, ArgCs);

  StructType *MetaTy = StructType::get(
      Ctx, {PtrTy, I32Ty, I32Ty, I32Ty, I32Ty, PtrTy, ArgsTy},
      /*isPacked=*/false);
  // Real pointer-typed constants: InvisiCap relocation needs addressable
  // pointer fields.
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

  // Last ctor, so a user validator is already installed when this runs.
  appendToGlobalCtors(M, Ctor, /*Priority=*/65535);
}

void FilAsyncPass::rewriteCallSites(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  PointerType *PtrTy = PointerType::getUnqual(Ctx);

  // alloc(size: i64, align: i64) -> ptr; submit(meta, impl, opts, staging,
  // nargs) -> ptr.
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
      // Name the function rather than dereferencing a null descriptor.
      errs() << "FilAsync: no descriptor emitted for " << F->getName() << "\n";
      report_fatal_error("FilAsync: internal error: missing descriptor");
    }
    GlobalVariable *Meta = DesIt->second.Meta;
    GlobalVariable *Opts = DesIt->second.Opts;

    // Snapshot the direct call sites; F survives the rewrite.
    SmallVector<CallBase *, 8> DirectCalls;
    for (User *U : F->users()) {
      if (auto *CB = dyn_cast<CallBase>(U))
        if (CB->getCalledOperand() == F)
          DirectCalls.push_back(CB);
    }

    unsigned NArgs = F->getFunctionType()->getNumParams();
    for (CallBase *CB : DirectCalls) {
      // Check the result type BEFORE inserting: a non-void non-ptr return
      // cannot take submit's ptr result, and leaving the old call in place
      // would run the async op alongside the synchronous one.
      bool IsVoid = CB->getType()->isVoidTy();
      if (!IsVoid && !CB->getType()->isPointerTy()) {
        errs() << "FilAsync: call to " << F->getName() << " returns "
               << *CB->getType()
               << " but filc_async_submit returns ptr; call left in place\n";
        continue;
      }

      IRBuilder<> Builder(CB);
      // nargs cells of 16 bytes, 16-byte aligned. Opaque here; FilPizlonator
      // widens the pointer stores.
      Value *Staging = Builder.CreateCall(
          AllocCallee,
          {ConstantInt::get(Int64Ty, NArgs * 16), ConstantInt::get(Int64Ty, 16)},
          "staging");

      // Store pointer operands AS POINTERS so FilPizlonator preserves their
      // capability; as integers the backend could not retain the buffer.
      // Scalars take the low word, wider ints become an ignored zero, and the
      // capability word is always zero.
      for (unsigned I = 0; I < NArgs; ++I) {
        Value *Arg = CB->getArgOperand(I);
        Type *ArgTy = Arg->getType();
        Value *Slot = Builder.CreateGEP(
            Int64Ty, Staging, {ConstantInt::get(Int64Ty, I * 2)});
        if (ArgTy->isPointerTy()) {
          Builder.CreateStore(Arg, Slot);
        } else {
          Value *IntVal = ConstantInt::get(Int64Ty, 0);
          if (ArgTy->isIntegerTy() && ArgTy->getIntegerBitWidth() < 64)
            IntVal = Builder.CreateZExt(Arg, Int64Ty);
          else if (ArgTy->isIntegerTy() &&
                   ArgTy->getIntegerBitWidth() == 64)
            IntVal = Arg;
          Builder.CreateStore(IntVal, Slot);
        }
        Value *CapGEP = Builder.CreateGEP(
            Int64Ty, Staging, {ConstantInt::get(Int64Ty, I * 2 + 1)});
        Builder.CreateStore(ConstantInt::get(Int64Ty, 0), CapGEP);
      }

      // The submit stub marks the producing args pending, so the call site
      // needs no mark_pending of its own. The task pointer it returns is the
      // pending result whose deref triggers the filc_resolve_pending hook.
      CallInst *Submit = Builder.CreateCall(
          SubmitCallee,
          {Meta, F, Opts, Staging, ConstantInt::get(Int64Ty, NArgs)},
          "async_result");

      if (IsVoid) {
        CB->eraseFromParent();
      } else {
        // Pure swap: both sides are opaque ptr. Non-ptr non-void was rejected
        // above, before anything was inserted.
        CB->replaceAllUsesWith(Submit);
        CB->eraseFromParent();
      }
    }
  }
}

void FilAsyncPass::eraseAnnotations(Module &M) {
  // Erasing llvm.global.annotations alone leaves the .args/.str globals alive:
  // its pool-uniqued ConstantArray holds their uses. So sever GA from its
  // initializer, then destroy the graph parent-before-child, freeing a node
  // only once use_empty. Shared leaves survive, so the option .str globals the
  // opts arrays still use stay. Only GlobalVariables are erased.
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
      // GlobalVariables go use-empty once the surrounding constants are gone.
      // Functions and aliases never do.
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
    // Not an error: a module with nothing annotated for us never sees a
    // filc_async function.
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

    // The op set is not validated here; the linked runtime's startup validator
    // owns it.

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

  // Sort by name so the emitted globals and table are deterministic.
  SmallVector<std::pair<Function *, const AnnotInfo *>, 8> Work;
  for (const auto &KV : Annotated)
    Work.emplace_back(const_cast<Function *>(KV.first), &KV.second);
  llvm::sort(Work, [](const auto &A, const auto &B) {
    return A.first->getName() < B.first->getName();
  });

  SmallVector<GlobalVariable *, 8> Metas;
  for (auto &KV : Work) {
    Function *F = KV.first;
    // Copy the name before the rename invalidates F's name storage.
    std::string OrigName = F->getName().str();
    renameBody(F, OrigName);
    GlobalVariable *Opts = emitOpts(M, OrigName, *KV.second);
    GlobalVariable *Meta = emitMeta(F, OrigName, *KV.second, Opts);
    Emitted[F] = {Opts, Meta, OrigName};
    Metas.push_back(Meta);
  }
  if (!Metas.empty())
    emitMetaTableAndCtor(M, Metas);

  // Drop the annotations only if we consumed some, so a module that holds
  // nothing for us keeps them for other users.
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