//===- FilAsync.cpp - filc_async annotation lowering ----------------------===//
//
// Sibling pass of FilPizlonator. Reads the `filc_async` annotations a stock
// clang records for `#pragma clang attribute`, renames each enrolled body to
// `__filc_async_<name>`, emits a `filc_async_meta` global `@__filc_meta_<name>`
// that points at the runtime its runtime=<name> option names, an options array `@__filc_opts_<name>`, a per-TU `@__filc_async_meta_table`,
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

#include "llvm/Transforms/Instrumentation/FilAsync.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
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
static const unsigned ARG_PENDING = 5;
static const unsigned DEP_NONE = 0;
static const unsigned DEP_READ = 1;
static const unsigned DEP_WRITE = 2;
static const unsigned DEP_POINTER = 4;
// A dependency's space, from r_dep=<param>:<ns> or w_dep=<param>:<ns>, goes in
// bits 8..31 as a 24-bit hash of "<param>:<ns>".
static const unsigned DEP_NAMESPACE_SHIFT = 8;
static const unsigned DEP_NAMESPACE_MASK = 0x00FFFFFF;

// Result constants; mirror FILC_ASYNC_RESULT_* in the runtime header.
static const unsigned RESULT_WORD = 1;
static const unsigned RESULT_PTR = 2;

// Read a global string constant (the frontend's .str globals); empty
// StringRef when C does not have that shape.
// FNV-1a folded to 24 bits, so every translation unit derives the same value
// for a name. A hash of 0 becomes 1, so a dependency's space is never 0.
static unsigned hashNamespace(StringRef Name) {
  uint32_t H = 2166136261u;
  for (unsigned char C : Name.bytes()) {
    H ^= C;
    H *= 16777619u;
  }
  H &= DEP_NAMESPACE_MASK;
  return H ? H : 1;
}

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

// Reuse an existing constant .str global matching string S; make a private
// copy only when none exists (normally never hit). A mutable global with the
// same bytes (`char buf[] = "op=pread"`) must not be reused: the program could
// rewrite the options the runtime reads.
static Constant *findOrCreateStringGlobal(Module &M, StringRef S) {
  for (GlobalVariable &GV : M.globals()) {
    if (!GV.isConstant() || !GV.hasInitializer())
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

// The name of each of F's parameters: from the !filc_async.params metadata
// the patched clang attaches, else from the IR argument names (IR from a
// clang that keeps value names). A parameter with no name is "".
static SmallVector<StringRef, 8> paramNames(const Function &F) {
  SmallVector<StringRef, 8> Names;
  if (MDNode *MD = F.getMetadata("filc_async.params")) {
    for (const MDOperand &Op : MD->operands()) {
      auto *S = dyn_cast_or_null<MDString>(Op.get());
      Names.push_back(S ? S->getString() : StringRef());
    }
    Names.resize(F.arg_size());
    return Names;
  }
  for (const Argument &A : F.args())
    Names.push_back(A.getName());
  return Names;
}

// Reads the argument option tokens into Kinds and Deps and counts the buffer
// options in Noped. Each option names a parameter: bin=<p> -> ARG_BUFFER_IN;
// bout=<p> -> ARG_BUFFER_OUT; buf=<p> -> ARG_PENDING (direction decided at use
// time by the runtime); r_dep=<p>:<ns> and w_dep=<p>:<ns> lock the argument's
// value in a space hashed from "<p>:<ns>". op= never decides a kind.
// Unannotated pointer args default to ARG_PENDING (the pessimistic "undecided
// direction" case); unannotated non-pointers stay ARG_IGNORED. An option that
// names no parameter is a compile-time fatal.
static void parseKinds(StringRef OrigName, const Function &F,
                       const FilAsyncPass::AnnotInfo &Info,
                       SmallVectorImpl<unsigned> &Kinds,
                       SmallVectorImpl<unsigned> &Deps, unsigned &Noped) {
  FunctionType *FTy = F.getFunctionType();
  unsigned NArgs = FTy->getNumParams();
  SmallVector<StringRef, 8> Names = paramNames(F);
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
      errs() << "FilAsync: '" << Opt << "' on " << OrigName
             << " is no longer an option; the runtime finds its descriptor "
             << "itself\n";
      report_fatal_error("FilAsync: malformed filc_async option");
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
    StringRef Value = Opt.drop_front(PrefixLen);
    // A dependency is <param>:<namespace>; on anything else a colon makes
    // the parameter name unknown below.
    StringRef Param = Value;
    if (Dep != DEP_NONE) {
      size_t Colon = Value.find(':');
      if (Colon == StringRef::npos) {
        errs() << "FilAsync: '" << Opt << "' on " << OrigName
               << " needs a namespace: <param>:<namespace>\n";
        report_fatal_error("FilAsync: malformed filc_async option");
      }
      Param = Value.take_front(Colon);
      if (Value.drop_front(Colon + 1).empty()) {
        errs() << "FilAsync: '" << Opt << "' has an empty namespace name\n";
        report_fatal_error("FilAsync: malformed filc_async option");
      }
    }
    const StringRef *Found = Param.empty() ? Names.end() : llvm::find(Names, Param);
    if (Found == Names.end()) {
      errs() << "FilAsync: '" << Opt << "' on " << OrigName
             << " names no parameter; use a parameter name";
      if (llvm::all_of(Names, [](StringRef N) { return N.empty(); }))
        errs() << " (" << OrigName << " has no parameter names here)";
      errs() << "\n";
      report_fatal_error("FilAsync: malformed filc_async option");
    }
    unsigned Idx = Found - Names.begin();
    if (Dep != DEP_NONE) {
      Type *ArgTy = FTy->getParamType(Idx);
      if (!ArgTy->isPointerTy() &&
          (!ArgTy->isIntegerTy() || ArgTy->getIntegerBitWidth() > 64)) {
        errs() << "FilAsync: dependency parameter " << Param << " of "
               << OrigName << " must be a pointer or an integer up to 64 bits\n";
        report_fatal_error("FilAsync: malformed filc_async option");
      }
      // The space hashes the parameter's name with the namespace; the lock
      // key adds the argument's value.
      unsigned Word = Dep | (ArgTy->isPointerTy() ? DEP_POINTER : 0) |
                      hashNamespace(Value) << DEP_NAMESPACE_SHIFT;
      // Repeating an option is harmless; a different mode or namespace for
      // the same parameter is a contradiction.
      if (Deps[Idx] != DEP_NONE && Deps[Idx] != Word) {
        errs() << "FilAsync: conflicting dependencies on parameter " << Param
               << " of " << OrigName << "\n";
        report_fatal_error("FilAsync: malformed filc_async option");
      }
      Deps[Idx] = Word;
    } else {
      // The runtime marks buffer args pending from their kind alone, so a
      // buffer option must name a pointer.
      if (!FTy->getParamType(Idx)->isPointerTy()) {
        errs() << "FilAsync: '" << Opt << "' names " << Param << " of "
               << OrigName << ", which is not a pointer\n";
        report_fatal_error("FilAsync: malformed filc_async option");
      }
      ++Noped;
      Kinds[Idx] = Kind;
    }
  }
}

// The runtime named by the one runtime=<name> option. Every annotated
// function names its runtime; the descriptor points at the runtime's
// `filc_async_runtime_<name>`, so the name must be a C identifier.
static StringRef parseRuntime(StringRef OrigName,
                              const FilAsyncPass::AnnotInfo &Info) {
  StringRef Name;
  for (StringRef Opt : Info.opts) {
    if (!Opt.starts_with("runtime="))
      continue;
    StringRef N = Opt.drop_front(8);
    bool Ident = !N.empty() && !isDigit(N.front()) &&
                 llvm::all_of(N, [](char C) { return isAlnum(C) || C == '_'; });
    if (!Ident) {
      errs() << "FilAsync: '" << Opt << "' on " << OrigName
             << " does not name a runtime; use runtime=<C identifier>\n";
      report_fatal_error("FilAsync: malformed filc_async option");
    }
    if (!Name.empty() && Name != N) {
      errs() << "FilAsync: " << OrigName << " names two runtimes, " << Name
             << " and " << N << "\n";
      report_fatal_error("FilAsync: malformed filc_async option");
    }
    Name = N;
  }
  if (Name.empty()) {
    errs() << "FilAsync: " << OrigName
           << " names no runtime; add runtime=<name>\n";
    report_fatal_error("FilAsync: malformed filc_async option");
  }
  return Name;
}

static bool isFilcAsyncEntry(Value *E) {
  auto *Entry = dyn_cast<ConstantStruct>(E);
  return Entry && Entry->getNumOperands() == 5 &&
         underlyingString(cast<Constant>(Entry->getOperand(1))) == "filc_async";
}

} // anonymous namespace

const FilAsyncPass::AnnotInfo *
FilAsyncPass::getAnnotInfo(const Function *F) const {
  auto It = Annotated.find(F);
  return It == Annotated.end() ? nullptr : &It->second;
}

void FilAsyncPass::renameBody(Function *F, StringRef OrigName) {
  // A declaration may be backed by an ordinary definition in another TU.
  // Keep its linker name; the annotated call still passes it as impl. An
  // available_externally body is only a copy of such a definition, so it is
  // treated the same way.
  if (F->isDeclaration() || F->hasAvailableExternallyLinkage())
    return;
  F->setName("__filc_async_" + OrigName.str());
  // Callers in other TUs still refer to the original name: typically the
  // annotation sits on a header declaration that both the caller and this
  // definition see. Keep that name bound to the body with an alias.
  if (!F->hasLocalLinkage()) {
    GlobalAlias *GA = GlobalAlias::create(F->getValueType(),
                                          F->getAddressSpace(), F->getLinkage(),
                                          OrigName, F, F->getParent());
    GA->setVisibility(F->getVisibility());
  }
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

  // Kinds come from the argument options ONLY -- op= never decides a
  // kind (see parseKinds). noped_args counts the bin=/bout=/buf= options.
  SmallVector<unsigned, 8> Kinds;
  SmallVector<unsigned, 8> Deps;
  unsigned Noped;
  parseKinds(OrigName, *F, Info, Kinds, Deps, Noped);

  // The `name` field holds the ORIGINAL name, captured before renameBody in run().
  Constant *NameInit =
      ConstantDataArray::getString(Ctx, OrigName, /*AddNull=*/true);
  auto *NameGV = new GlobalVariable(M, NameInit->getType(),
                                    /*isConstant=*/true,
                                    GlobalValue::PrivateLinkage, NameInit,
                                    "__filc_async_name_" + OrigName.str());

  // The runtime's descriptor, defined by the runtime the program links; a
  // runtime that is not linked leaves filc_async_runtime_<name> undefined.
  Constant *Runtime = M.getOrInsertGlobal(
      ("filc_async_runtime_" + parseRuntime(OrigName, Info)).str(), PtrTy);

  // args[] tail: exactly nargs {i32,i32} pairs. Field order matches the C header
  // {name, nargs, noped_args, flags, result, opts, runtime, args[]}; the
  // 16-byte pointers and the offsets they imply materialize under
  // FilPizlonator.
  SmallVector<Constant *, 8> ArgCs;
  for (unsigned I = 0; I < NArgs; ++I)
    ArgCs.push_back(ConstantStruct::get(
        PairTy, {ConstantInt::get(I32Ty, Kinds[I]),
                 ConstantInt::get(I32Ty, Deps[I])}));
  ArrayType *ArgsTy = ArrayType::get(PairTy, NArgs);
  Constant *ArgsInit = ConstantArray::get(ArgsTy, ArgCs);

  StructType *MetaTy = StructType::get(
      Ctx, {PtrTy, I32Ty, I32Ty, I32Ty, I32Ty, PtrTy, PtrTy, ArgsTy},
      /*isPacked=*/false);
  // Real pointer-typed constants (no ptrtoint i64): InvisiCap relocation
  // requires pointer fields to be addressable.
  Constant *Init = ConstantStruct::get(
      MetaTy, {NameGV, ConstantInt::get(I32Ty, NArgs),
               ConstantInt::get(I32Ty, Noped), ConstantInt::get(I32Ty, 0),
               ConstantInt::get(I32Ty, Result), Opts, Runtime, ArgsInit});
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

// Stores a call's arguments into 16-byte cells: pointers as pointers, so
// FilPizlonator keeps their capability, and integers in the low word with a
// zero second word. Wider integers and other types become an ignored zero.
static void stageArguments(IRBuilder<> &B, Value *Staging, Function *Stub) {
  Type *Int64Ty = B.getInt64Ty();
  for (unsigned I = 0; I < Stub->arg_size(); ++I) {
    Value *Arg = Stub->getArg(I);
    Type *ArgTy = Arg->getType();
    Value *Slot = B.CreateGEP(Int64Ty, Staging, {B.getInt64(I * 2)});
    if (ArgTy->isPointerTy()) {
      B.CreateStore(Arg, Slot);
      continue;
    }
    Value *IntVal = B.getInt64(0);
    if (ArgTy->isIntegerTy() && ArgTy->getIntegerBitWidth() < 64)
      IntVal = B.CreateZExt(Arg, Int64Ty);
    else if (ArgTy->isIntegerTy() && ArgTy->getIntegerBitWidth() == 64)
      IntVal = Arg;
    B.CreateStore(IntVal, Slot);
    B.CreateStore(B.getInt64(0),
                  B.CreateGEP(Int64Ty, Staging, {B.getInt64(I * 2 + 1)}));
  }
}

// `i64 @__filc_async_run_<name>(ptr staging)`: loads each argument back out
// of its cell and calls F's body, returning its result as a word. Only
// runtimes call it, through filc_async_run.
Function *FilAsyncPass::emitRunThunk(Module &M, Function *F,
                                     const Descriptors &D) {
  LLVMContext &Ctx = M.getContext();
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  PointerType *PtrTy = PointerType::getUnqual(Ctx);
  FunctionType *FTy = F->getFunctionType();

  Function *Run = Function::Create(FunctionType::get(Int64Ty, {PtrTy}, false),
                                   GlobalValue::InternalLinkage,
                                   "__filc_async_run_" + D.OrigName, &M);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", Run));
  Value *Staging = Run->getArg(0);
  SmallVector<Value *, 8> Args;
  for (unsigned I = 0; I < FTy->getNumParams(); ++I) {
    Type *ParamTy = FTy->getParamType(I);
    Value *Slot = B.CreateGEP(Int64Ty, Staging, {B.getInt64(I * 2)});
    if (ParamTy->isPointerTy()) {
      Args.push_back(B.CreateLoad(PtrTy, Slot));
    } else if (ParamTy->isIntegerTy() && ParamTy->getIntegerBitWidth() <= 64) {
      Value *Word = B.CreateLoad(Int64Ty, Slot);
      Args.push_back(ParamTy->getIntegerBitWidth() < 64
                         ? B.CreateTrunc(Word, ParamTy)
                         : Word);
    } else {
      Args.push_back(Constant::getNullValue(ParamTy));
    }
  }
  CallInst *Call = B.CreateCall(FTy, F, Args);
  Type *RetTy = FTy->getReturnType();
  Value *Result = B.getInt64(0);
  if (RetTy->isPointerTy())
    Result = B.CreatePtrToInt(Call, Int64Ty);
  else if (RetTy->isIntegerTy())
    Result = B.CreateSExtOrTrunc(Call, Int64Ty);
  B.CreateRet(Result);
  return Run;
}

// `@__filc_async_stub_<name>`, with F's signature: stages the arguments,
// starts a task, takes the dependency locks and marks the output buffers
// that the annotation names, then hands the call to the runtime. What to
// lock and mark is known here, so the stub carries no descriptor walk.
Function *FilAsyncPass::emitStub(Module &M, Function *F, const Descriptors &D) {
  LLVMContext &Ctx = M.getContext();
  Type *Int64Ty = Type::getInt64Ty(Ctx);
  Type *Int32Ty = Type::getInt32Ty(Ctx);
  Type *VoidTy = Type::getVoidTy(Ctx);
  PointerType *PtrTy = PointerType::getUnqual(Ctx);
  FunctionType *FTy = F->getFunctionType();
  unsigned NArgs = FTy->getNumParams();

  FunctionCallee Alloc = M.getOrInsertFunction(
      "filc_async_alloc", FunctionType::get(PtrTy, {Int64Ty, Int64Ty}, false));
  FunctionCallee Begin = M.getOrInsertFunction(
      "filc_async_begin", FunctionType::get(PtrTy, {PtrTy, PtrTy}, false));
  FunctionCallee LockWord = M.getOrInsertFunction(
      "filc_async_lock_word",
      FunctionType::get(VoidTy, {PtrTy, Int64Ty, Int32Ty, Int32Ty}, false));
  FunctionCallee LockPtr = M.getOrInsertFunction(
      "filc_async_lock_ptr",
      FunctionType::get(VoidTy, {PtrTy, PtrTy, Int32Ty, Int32Ty}, false));
  FunctionCallee Mark = M.getOrInsertFunction(
      "filc_async_mark_pending",
      FunctionType::get(VoidTy, {PtrTy, PtrTy}, false));
  FunctionCallee Submit = M.getOrInsertFunction(
      "filc_async_submit",
      FunctionType::get(VoidTy, {PtrTy, PtrTy, PtrTy, PtrTy, Int64Ty}, false));

  Function *Run = emitRunThunk(M, F, D);
  Function *Stub = Function::Create(FTy, GlobalValue::InternalLinkage,
                                    "__filc_async_stub_" + D.OrigName, &M);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", Stub));

  Value *Staging = B.CreateCall(
      Alloc, {B.getInt64(NArgs * 16), B.getInt64(16)}, "staging");
  stageArguments(B, Staging, Stub);
  Value *Task = B.CreateCall(Begin, {D.Meta, Staging}, "task");

  for (unsigned I = 0; I < NArgs; ++I) {
    unsigned Mode = D.Deps[I] & (DEP_READ | DEP_WRITE);
    if (!Mode)
      continue;
    Value *Arg = Stub->getArg(I);
    Value *Space = ConstantInt::get(Int32Ty, D.Deps[I] & ~(DEP_READ | DEP_WRITE));
    Value *ModeV = ConstantInt::get(Int32Ty, Mode);
    if (Arg->getType()->isPointerTy())
      B.CreateCall(LockPtr, {Task, Arg, Space, ModeV});
    else
      B.CreateCall(LockWord,
                   {Task, B.CreateZExtOrTrunc(Arg, Int64Ty), Space, ModeV});
  }
  // bout=, bare buf= and unannotated pointers are marked; bin= never is. The
  // option parser only gives these kinds to pointer arguments.
  for (unsigned I = 0; I < NArgs; ++I)
    if (D.Kinds[I] == ARG_BUFFER_OUT || D.Kinds[I] == ARG_PENDING)
      B.CreateCall(Mark, {Task, Stub->getArg(I)});

  B.CreateCall(Submit, {Task, D.Meta, Run, Staging, B.getInt64(NArgs)});
  if (FTy->getReturnType()->isVoidTy())
    B.CreateRetVoid();
  else
    B.CreateRet(Task);
  return Stub;
}

void FilAsyncPass::rewriteCallSites(Module &M) {
  for (const auto &KV : Annotated) {
    Function *F = const_cast<Function *>(KV.first);
    auto DesIt = Emitted.find(F);
    if (DesIt == Emitted.end()) {
      // Descriptors were always emitted for enrolled entries; guard anyway so a
      // reader/emitter divergence names the function instead of null.
      errs() << "FilAsync: no descriptor emitted for " << F->getName() << "\n";
      report_fatal_error("FilAsync: internal error: missing descriptor");
    }

    SmallVector<CallInst *, 8> Calls;
    for (User *U : F->users()) {
      auto *CB = dyn_cast<CallBase>(U);
      if (!CB || CB->getCalledOperand() != F)
        continue;
      // Only a plain call through F's own prototype can be redirected: an
      // invoke or callbr has unwind edges the stub does not model, and a call
      // through an unprototyped declaration may pass fewer operands.
      if (!isa<CallInst>(CB) || CB->getFunctionType() != F->getFunctionType()) {
        errs() << "FilAsync: call to " << F->getName()
               << " is not a plain call matching its prototype; call left in "
                  "place\n";
        continue;
      }
      // The stub returns the task, a pointer, so a call that expects another
      // non-void type is left alone rather than given the wrong value.
      Type *RetTy = CB->getType();
      if (!RetTy->isVoidTy() && !RetTy->isPointerTy()) {
        errs() << "FilAsync: call to " << F->getName() << " returns "
               << *RetTy << " but the stub returns ptr; call left in place\n";
        continue;
      }
      Calls.push_back(cast<CallInst>(CB));
    }
    if (Calls.empty())
      continue;

    Function *Stub = emitStub(M, F, DesIt->second);
    for (CallInst *CI : Calls)
      CI->setCalledFunction(Stub);
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
  //
  // Entries that are not filc_async belong to other consumers: they move to a
  // fresh llvm.global.annotations, whose uses keep them out of the teardown.
  GlobalVariable *GA = M.getNamedGlobal("llvm.global.annotations");
  if (!GA || !GA->hasInitializer())
    return;

  Constant *Root = GA->getInitializer();
  SmallVector<Constant *, 8> Foreign;
  if (auto *Array = dyn_cast<ConstantArray>(Root->stripPointerCasts()))
    for (Value *E : Array->operand_values())
      if (!isFilcAsyncEntry(E))
        Foreign.push_back(cast<Constant>(E));

  GlobalValue::LinkageTypes Linkage = GA->getLinkage();
  std::string Section = GA->getSection().str();
  GA->setInitializer(nullptr);
  GA->eraseFromParent();

  if (!Foreign.empty()) {
    ArrayType *Ty = ArrayType::get(Foreign.front()->getType(), Foreign.size());
    auto *Kept = new GlobalVariable(M, Ty, /*isConstant=*/false, Linkage,
                                    ConstantArray::get(Ty, Foreign),
                                    "llvm.global.annotations");
    Kept->setSection(Section);
  }

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
    if (!isFilcAsyncEntry(E))
      continue;
    auto *Entry = cast<ConstantStruct>(E);

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
  // A pass instance may be run on more than one module.
  Annotated.clear();
  Emitted.clear();

  if (!enrollAnnotatedFunctions(M))
    report_fatal_error(
        "FilAsync: malformed filc_async annotation; see diagnostics above");
  if (Annotated.empty())
    return PreservedAnalyses::all();

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
    SmallVector<unsigned, 8> Deps;
    unsigned Noped;
    parseKinds(OrigName, *F, *KV.second, Kinds, Deps, Noped);
    renameBody(F, OrigName);
    GlobalVariable *Opts = emitOpts(M, OrigName, *KV.second);
    GlobalVariable *Meta = emitMeta(F, OrigName, *KV.second, Opts);
    Emitted[F] = {Opts, Meta, OrigName, std::move(Kinds), std::move(Deps)};
    Metas.push_back(Meta);
  }
  emitMetaTableAndCtor(M, Metas);

  // Rewrite every direct call site, then drop the consumed annotations so
  // FilPizlonator never sees them.
  rewriteCallSites(M);
  eraseAnnotations(M);

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
