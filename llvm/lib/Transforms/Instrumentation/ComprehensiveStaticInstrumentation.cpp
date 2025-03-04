//===- ComprehensiveStaticInstrumentation.cpp - CSI compiler pass ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is part of CSI, a framework that provides comprehensive static
// instrumentation.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/ComprehensiveStaticInstrumentation.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/EHPersonalities.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TapirTaskInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Instrumentation/CSI.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/EscapeEnumerator.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Utils/TapirUtils.h"

using namespace llvm;

#define DEBUG_TYPE "csi"

static cl::opt<bool>
    ClInstrumentFuncEntryExit("csi-instrument-func-entry-exit", cl::init(true),
                              cl::desc("Instrument function entry and exit"),
                              cl::Hidden);
static cl::opt<bool>
    ClInstrumentLoops("csi-instrument-loops", cl::init(true),
                      cl::desc("Instrument loops"), cl::Hidden);
static cl::opt<bool>
    ClInstrumentBasicBlocks("csi-instrument-basic-blocks", cl::init(true),
                            cl::desc("Instrument basic blocks"), cl::Hidden);
static cl::opt<bool>
    ClInstrumentMemoryAccesses("csi-instrument-memory-accesses", cl::init(true),
                               cl::desc("Instrument memory accesses"),
                               cl::Hidden);
static cl::opt<bool> ClInstrumentCalls("csi-instrument-function-calls",
                                       cl::init(true),
                                       cl::desc("Instrument function calls"),
                                       cl::Hidden);
static cl::opt<bool> ClInstrumentAtomics("csi-instrument-atomics",
                                         cl::init(true),
                                         cl::desc("Instrument atomics"),
                                         cl::Hidden);
static cl::opt<bool> ClInstrumentMemIntrinsics(
    "csi-instrument-memintrinsics", cl::init(true),
    cl::desc("Instrument memintrinsics (memset/memcpy/memmove)"), cl::Hidden);
static cl::opt<bool> ClInstrumentTapir("csi-instrument-tapir", cl::init(true),
                                       cl::desc("Instrument tapir constructs"),
                                       cl::Hidden);
static cl::opt<bool> ClInstrumentAllocas("csi-instrument-alloca",
                                         cl::init(true),
                                         cl::desc("Instrument allocas"),
                                         cl::Hidden);
static cl::opt<bool>
    ClInstrumentAllocFns("csi-instrument-allocfn", cl::init(true),
                         cl::desc("Instrument allocation functions"),
                         cl::Hidden);

static cl::opt<bool> ClInterpose("csi-interpose", cl::init(true),
                                 cl::desc("Enable function interpositioning"),
                                 cl::Hidden);

static cl::opt<std::string> ClToolBitcode(
    "csi-tool-bitcode", cl::init(""),
    cl::desc("Path to the tool bitcode file for compile-time instrumentation"),
    cl::Hidden);

static cl::opt<std::string>
    ClRuntimeBitcode("csi-runtime-bitcode", cl::init(""),
                     cl::desc("Path to the CSI runtime bitcode file for "
                              "optimized compile-time instrumentation"),
                     cl::Hidden);

static cl::opt<std::string> ClToolLibrary(
    "csi-tool-library", cl::init(""),
    cl::desc("Path to the tool library file for compile-time instrumentation"),
    cl::Hidden);

static cl::opt<std::string> ClConfigurationFilename(
    "csi-config-filename", cl::init(""),
    cl::desc("Path to the configuration file for surgical instrumentation"),
    cl::Hidden);

static cl::opt<InstrumentationConfigMode> ClConfigurationMode(
    "csi-config-mode", cl::init(InstrumentationConfigMode::WHITELIST),
    cl::values(clEnumValN(InstrumentationConfigMode::WHITELIST, "whitelist",
                          "Use configuration file as a whitelist"),
               clEnumValN(InstrumentationConfigMode::BLACKLIST, "blacklist",
                          "Use configuration file as a blacklist")),
    cl::desc("Specifies how to interpret the configuration file"), cl::Hidden);

static cl::opt<bool>
    AssumeNoExceptions(
        "csi-assume-no-exceptions", cl::init(false), cl::Hidden,
        cl::desc("Assume that ordinary calls cannot throw exceptions."));

static cl::opt<bool>
    SplitBlocksAtCalls(
        "csi-split-blocks-at-calls", cl::init(true), cl::Hidden,
        cl::desc("Split basic blocks at function calls."));

static size_t numPassRuns = 0;
bool IsFirstRun() { return numPassRuns == 0; }

namespace {

static CSIOptions OverrideFromCL(CSIOptions Options) {
  Options.InstrumentFuncEntryExit = ClInstrumentFuncEntryExit;
  Options.InstrumentLoops = ClInstrumentLoops;
  Options.InstrumentBasicBlocks = ClInstrumentBasicBlocks;
  Options.InstrumentMemoryAccesses = ClInstrumentMemoryAccesses;
  Options.InstrumentCalls = ClInstrumentCalls;
  Options.InstrumentAtomics = ClInstrumentAtomics;
  Options.InstrumentMemIntrinsics = ClInstrumentMemIntrinsics;
  Options.InstrumentTapir = ClInstrumentTapir;
  Options.InstrumentAllocas = ClInstrumentAllocas;
  Options.InstrumentAllocFns = ClInstrumentAllocFns;
  Options.CallsMayThrow = !AssumeNoExceptions;
  Options.CallsTerminateBlocks = SplitBlocksAtCalls;
  return Options;
}

/// The Comprehensive Static Instrumentation pass.
/// Inserts calls to user-defined hooks at predefined points in the IR.
struct ComprehensiveStaticInstrumentationLegacyPass : public ModulePass {
  static char ID; // Pass identification, replacement for typeid.

  ComprehensiveStaticInstrumentationLegacyPass(
      const CSIOptions &Options = OverrideFromCL(CSIOptions()))
      : ModulePass(ID), Options(Options) {
    initializeComprehensiveStaticInstrumentationLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }
  StringRef getPassName() const override {
    return "ComprehensiveStaticInstrumentation";
  }
  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  CSIOptions Options;
}; // struct ComprehensiveStaticInstrumentation
} // anonymous namespace

char ComprehensiveStaticInstrumentationLegacyPass::ID = 0;

INITIALIZE_PASS_BEGIN(ComprehensiveStaticInstrumentationLegacyPass, "csi",
                      "ComprehensiveStaticInstrumentation pass", false, false)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TaskInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(ComprehensiveStaticInstrumentationLegacyPass, "csi",
                    "ComprehensiveStaticInstrumentation pass", false, false)

ModulePass *llvm::createComprehensiveStaticInstrumentationLegacyPass() {
  return new ComprehensiveStaticInstrumentationLegacyPass();
}
ModulePass *llvm::createComprehensiveStaticInstrumentationLegacyPass(
    const CSIOptions &Options) {
  return new ComprehensiveStaticInstrumentationLegacyPass(Options);
}

/// Return the first DILocation in the given basic block, or nullptr
/// if none exists.
static const DILocation *getFirstDebugLoc(const BasicBlock &BB) {
  for (const Instruction &Inst : BB)
    if (const DILocation *Loc = Inst.getDebugLoc())
      return Loc;

  return nullptr;
}

/// Set DebugLoc on the call instruction to a CSI hook, based on the
/// debug information of the instrumented instruction.
static void setInstrumentationDebugLoc(Instruction *Instrumented,
                                       Instruction *Call) {
  DISubprogram *Subprog = Instrumented->getFunction()->getSubprogram();
  if (Subprog) {
    if (Instrumented->getDebugLoc()) {
      Call->setDebugLoc(Instrumented->getDebugLoc());
    } else {
      LLVMContext &C = Instrumented->getContext();
      Call->setDebugLoc(DILocation::get(C, 0, 0, Subprog));
    }
  }
}

/// Set DebugLoc on the call instruction to a CSI hook, based on the
/// debug information of the instrumented instruction.
static void setInstrumentationDebugLoc(BasicBlock &Instrumented,
                                       Instruction *Call) {
  DISubprogram *Subprog = Instrumented.getParent()->getSubprogram();
  if (Subprog) {
    if (const DILocation *FirstDebugLoc = getFirstDebugLoc(Instrumented))
      Call->setDebugLoc(FirstDebugLoc);
    else {
      LLVMContext &C = Instrumented.getContext();
      Call->setDebugLoc(DILocation::get(C, 0, 0, Subprog));
    }
  }
}

bool CSISetupImpl::run() {
  bool Changed = false;
  for (Function &F : M)
    Changed |= setupFunction(F);
  return Changed;
}

bool CSISetupImpl::setupFunction(Function &F) {
  if (F.empty() || CSIImpl::shouldNotInstrumentFunction(F))
    return false;

  if (Options.CallsMayThrow)
    // Promote calls to invokes to insert CSI instrumentation in
    // exception-handling code.
    CSIImpl::setupCalls(F);

  // If we do not assume that calls terminate blocks, or if we're not
  // instrumenting basic blocks, then we're done.
  if (Options.InstrumentBasicBlocks && Options.CallsTerminateBlocks)
    CSIImpl::splitBlocksAtCalls(F);

  LLVM_DEBUG(dbgs() << "Setup function:\n" << F);

  return true;
}

bool CSIImpl::callsPlaceholderFunction(const Instruction &I) {
  if (isa<DbgInfoIntrinsic>(I))
    return true;

  if (isDetachedRethrow(&I) || isTaskFrameResume(&I) || isSyncUnwind(&I))
    return true;

  if (const IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I))
    switch (II->getIntrinsicID()) {
    default: break;
      // FIXME: This list is repeated from NoTTI::getIntrinsicCost.
    case Intrinsic::annotation:
    case Intrinsic::assume:
    case Intrinsic::sideeffect:
    case Intrinsic::invariant_start:
    case Intrinsic::invariant_end:
    case Intrinsic::launder_invariant_group:
    case Intrinsic::strip_invariant_group:
    case Intrinsic::is_constant:
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::objectsize:
    case Intrinsic::ptr_annotation:
    case Intrinsic::var_annotation:
    case Intrinsic::experimental_gc_result:
    case Intrinsic::experimental_gc_relocate:
    case Intrinsic::experimental_noalias_scope_decl:
    case Intrinsic::coro_alloc:
    case Intrinsic::coro_begin:
    case Intrinsic::coro_free:
    case Intrinsic::coro_end:
    case Intrinsic::coro_frame:
    case Intrinsic::coro_size:
    case Intrinsic::coro_suspend:
    case Intrinsic::coro_subfn_addr:
    case Intrinsic::syncregion_start:
    case Intrinsic::taskframe_create:
    case Intrinsic::taskframe_use:
    case Intrinsic::taskframe_end:
    case Intrinsic::taskframe_load_guard:
      // These intrinsics don't actually represent code after lowering.
      return true;
    }

  return false;
}

bool CSIImpl::spawnsTapirLoopBody(DetachInst *DI, LoopInfo &LI, TaskInfo &TI) {
  Loop *L = LI.getLoopFor(DI->getParent());
  return (TI.getTaskFor(DI->getDetached()) == getTaskIfTapirLoop(L, &TI));
}

bool CSIImpl::run() {
  // Link the tool bitcode once initially, to get type definitions.
  linkInToolFromBitcode(ClToolBitcode);
  initializeCsi();

  for (Function &F : M)
    instrumentFunction(F);

  collectUnitFEDTables();
  collectUnitSizeTables();

  finalizeCsi();

  if (IsFirstRun() && Options.jitMode) {
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(ClToolLibrary.c_str());
  }
  // Link the tool bitcode a second time, for definitions of used functions.
  linkInToolFromBitcode(ClToolBitcode);
  linkInToolFromBitcode(ClRuntimeBitcode);

  return true; // We always insert the unit constructor.
}

Constant *ForensicTable::getObjectStrGV(Module &M, StringRef Str,
                                        const Twine GVName) {
  LLVMContext &C = M.getContext();
  IntegerType *Int32Ty = IntegerType::get(C, 32);
  Constant *Zero = ConstantInt::get(Int32Ty, 0);
  Value *GepArgs[] = {Zero, Zero};
  if (Str.empty())
    return ConstantPointerNull::get(
        PointerType::get(IntegerType::get(C, 8), 0));

  Constant *NameStrConstant = ConstantDataArray::getString(C, Str);
  GlobalVariable *GV = M.getGlobalVariable((GVName + Str).str(), true);
  if (GV == NULL) {
    GV = new GlobalVariable(M, NameStrConstant->getType(), true,
                            GlobalValue::PrivateLinkage, NameStrConstant,
                            GVName + Str, nullptr,
                            GlobalVariable::NotThreadLocal, 0);
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  }
  assert(GV);
  return ConstantExpr::getGetElementPtr(GV->getValueType(), GV, GepArgs);
}

ForensicTable::ForensicTable(Module &M, StringRef BaseIdName,
                             StringRef TableName, bool UseExistingBaseId)
    : TableName(TableName) {
  LLVMContext &C = M.getContext();
  IntegerType *Int64Ty = IntegerType::get(C, 64);
  IdCounter = 0;

  if (UseExistingBaseId)
    // Try to look up an existing BaseId to use.
    BaseId = M.getGlobalVariable(BaseIdName, true);
  if (nullptr == BaseId)
    BaseId = new GlobalVariable(M, Int64Ty, false, GlobalValue::InternalLinkage,
                                ConstantInt::get(Int64Ty, 0), BaseIdName);
  assert(BaseId);
}

uint64_t ForensicTable::getId(const Value *V) {
  if (!ValueToLocalIdMap.count(V))
    ValueToLocalIdMap[V] = IdCounter++;
  assert(ValueToLocalIdMap.count(V) && "Value not in ID map.");
  return ValueToLocalIdMap[V];
}

Value *ForensicTable::localToGlobalId(uint64_t LocalId,
                                      IRBuilder<> &IRB) const {
  assert(BaseId);
  LLVMContext &C = IRB.getContext();
  Type *BaseIdTy = IRB.getInt64Ty();
  LoadInst *Base = IRB.CreateLoad(BaseIdTy, BaseId);
  MDNode *MD = llvm::MDNode::get(C, None);
  Base->setMetadata(LLVMContext::MD_invariant_load, MD);
  Value *Offset = IRB.getInt64(LocalId);
  return IRB.CreateAdd(Base, Offset);
}

uint64_t SizeTable::add(const BasicBlock &BB, TargetTransformInfo *TTI) {
  uint64_t ID = getId(&BB);
  // Count the LLVM IR instructions
  int32_t IRCost = 0;
  for (const Instruction &I : BB) {
    if (TTI) {
      InstructionCost ICost =
          TTI->getInstructionCost(&I, TargetTransformInfo::TCK_Latency);
      if (!ICost.isValid())
        IRCost += static_cast<int>(TargetTransformInfo::TCC_Basic);
      else
        IRCost += *(ICost.getValue());
    } else {
      if (isa<PHINode>(I))
        continue;
      if (CSIImpl::callsPlaceholderFunction(I))
        continue;
      IRCost++;
    }
  }
  add(ID, BB.size(), IRCost);
  return ID;
}

PointerType *SizeTable::getPointerType(LLVMContext &C) {
  return PointerType::get(getSizeStructType(C), 0);
}

StructType *SizeTable::getSizeStructType(LLVMContext &C) {
  return StructType::get(
      /* FullIRSize */ IntegerType::get(C, 32),
      /* NonEmptyIRSize */ IntegerType::get(C, 32));
}

void SizeTable::add(uint64_t ID, int32_t FullIRSize, int32_t NonEmptyIRSize) {
  assert(LocalIdToSizeMap.find(ID) == LocalIdToSizeMap.end() &&
         "ID already exists in FED table.");
  LocalIdToSizeMap[ID] = {FullIRSize, NonEmptyIRSize};
}

Constant *SizeTable::insertIntoModule(Module &M) const {
  LLVMContext &C = M.getContext();
  StructType *TableType = getSizeStructType(C);
  IntegerType *Int32Ty = IntegerType::get(C, 32);
  Constant *Zero = ConstantInt::get(Int32Ty, 0);
  Value *GepArgs[] = {Zero, Zero};
  SmallVector<Constant *, 1> TableEntries;

  for (uint64_t LocalID = 0; LocalID < IdCounter; ++LocalID) {
    const SizeInformation &E = LocalIdToSizeMap.find(LocalID)->second;
    Constant *FullIRSize = ConstantInt::get(Int32Ty, E.FullIRSize);
    Constant *NonEmptyIRSize = ConstantInt::get(Int32Ty, E.NonEmptyIRSize);
    // The order of arguments to ConstantStruct::get() must match the
    // sizeinfo_t type in csi.h.
    TableEntries.push_back(
        ConstantStruct::get(TableType, FullIRSize, NonEmptyIRSize));
  }

  ArrayType *TableArrayType = ArrayType::get(TableType, TableEntries.size());
  Constant *Table = ConstantArray::get(TableArrayType, TableEntries);
  GlobalVariable *GV =
      new GlobalVariable(M, TableArrayType, false, GlobalValue::InternalLinkage,
                         Table, CsiUnitSizeTableName);
  return ConstantExpr::getGetElementPtr(GV->getValueType(), GV, GepArgs);
}

uint64_t FrontEndDataTable::add(const Function &F) {
  uint64_t ID = getId(&F);
  if (F.getSubprogram())
    add(ID, F.getSubprogram());
  else
    add(ID, -1, -1, F.getParent()->getName(), "", F.getName());
  return ID;
}

uint64_t FrontEndDataTable::add(const BasicBlock &BB) {
  uint64_t ID = getId(&BB);
  add(ID, getFirstDebugLoc(BB));
  return ID;
}

uint64_t FrontEndDataTable::add(const Instruction &I,
                                const StringRef &RealName) {
  uint64_t ID = getId(&I);
  if (auto DL = I.getDebugLoc())
    add(ID, DL, RealName);
  else {
    if (const DISubprogram *Subprog = I.getFunction()->getSubprogram())
      add(ID, (int32_t)Subprog->getLine(), -1, Subprog->getFilename(),
          Subprog->getDirectory(),
          RealName == "" ? Subprog->getName() : RealName);
    else
      add(ID, -1, -1, I.getModule()->getName(), "",
          RealName == "" ? I.getFunction()->getName() : RealName);
  }
  return ID;
}

PointerType *FrontEndDataTable::getPointerType(LLVMContext &C) {
  return PointerType::get(getSourceLocStructType(C), 0);
}

StructType *FrontEndDataTable::getSourceLocStructType(LLVMContext &C) {
  return StructType::get(
      /* Name */ PointerType::get(IntegerType::get(C, 8), 0),
      /* Line */ IntegerType::get(C, 32),
      /* Column */ IntegerType::get(C, 32),
      /* File */ PointerType::get(IntegerType::get(C, 8), 0));
}

void FrontEndDataTable::add(uint64_t ID, const DILocation *Loc,
                            const StringRef &RealName) {
  if (Loc) {
    // TODO: Add location information for inlining
    const DISubprogram *Subprog = Loc->getScope()->getSubprogram();
    add(ID, (int32_t)Loc->getLine(), (int32_t)Loc->getColumn(),
        Loc->getFilename(), Loc->getDirectory(),
        RealName == "" ? Subprog->getName() : RealName);
  } else
    add(ID);
}

void FrontEndDataTable::add(uint64_t ID, const DISubprogram *Subprog) {
  if (Subprog)
    add(ID, (int32_t)Subprog->getLine(), -1, Subprog->getFilename(),
        Subprog->getDirectory(), Subprog->getName());
  else
    add(ID);
}

void FrontEndDataTable::add(uint64_t ID, int32_t Line, int32_t Column,
                            StringRef Filename, StringRef Directory,
                            StringRef Name) {
  // TODO: This assert is too strong for unwind basic blocks' FED.
  /*assert(LocalIdToSourceLocationMap.find(ID) ==
             LocalIdToSourceLocationMap.end() &&
         "Id already exists in FED table."); */
  LocalIdToSourceLocationMap[ID] = {Name, Line, Column, Filename, Directory};
}

// The order of arguments to ConstantStruct::get() must match the source_loc_t
// type in csi.h.
static void addFEDTableEntries(SmallVectorImpl<Constant *> &FEDEntries,
                               StructType *FedType, Constant *Name,
                               Constant *Line, Constant *Column,
                               Constant *File) {
  FEDEntries.push_back(ConstantStruct::get(FedType, Name, Line, Column, File));
}

Constant *FrontEndDataTable::insertIntoModule(Module &M) const {
  LLVMContext &C = M.getContext();
  StructType *FedType = getSourceLocStructType(C);
  IntegerType *Int32Ty = IntegerType::get(C, 32);
  Constant *Zero = ConstantInt::get(Int32Ty, 0);
  Value *GepArgs[] = {Zero, Zero};
  SmallVector<Constant *, 11> FEDEntries;

  for (uint64_t LocalID = 0; LocalID < IdCounter; ++LocalID) {
    const SourceLocation &E = LocalIdToSourceLocationMap.find(LocalID)->second;
    Constant *Line = ConstantInt::get(Int32Ty, E.Line);
    Constant *Column = ConstantInt::get(Int32Ty, E.Column);
    Constant *File;
    {
      std::string Filename = E.Filename.str();
      if (!E.Directory.empty())
        Filename = E.Directory.str() + "/" + Filename;
      File = getObjectStrGV(M, Filename, "__csi_unit_filename_");
    }
    Constant *Name = getObjectStrGV(M, E.Name, "__csi_unit_function_name_");
    addFEDTableEntries(FEDEntries, FedType, Name, Line, Column, File);
  }

  ArrayType *FedArrayType = ArrayType::get(FedType, FEDEntries.size());
  Constant *Table = ConstantArray::get(FedArrayType, FEDEntries);
  GlobalVariable *GV =
      new GlobalVariable(M, FedArrayType, false, GlobalValue::InternalLinkage,
                         Table, CsiUnitFedTableName + BaseId->getName());
  return ConstantExpr::getGetElementPtr(GV->getValueType(), GV, GepArgs);
}

/// Function entry and exit hook initialization
void CSIImpl::initializeFuncHooks() {
  LLVMContext &C = M.getContext();
  IRBuilder<> IRB(C);
  // Initialize function entry hook
  Type *FuncPropertyTy = CsiFuncProperty::getType(C);
  CsiFuncEntry = M.getOrInsertFunction("__csi_func_entry", IRB.getVoidTy(),
                                       IRB.getInt64Ty(), FuncPropertyTy);
  // Initialize function exit hook
  Type *FuncExitPropertyTy = CsiFuncExitProperty::getType(C);
  CsiFuncExit = M.getOrInsertFunction("__csi_func_exit", IRB.getVoidTy(),
                                      IRB.getInt64Ty(), IRB.getInt64Ty(),
                                      FuncExitPropertyTy);
}

/// Basic-block hook initialization
void CSIImpl::initializeBasicBlockHooks() {
  LLVMContext &C = M.getContext();
  IRBuilder<> IRB(C);
  Type *PropertyTy = CsiBBProperty::getType(C);
  CsiBBEntry = M.getOrInsertFunction("__csi_bb_entry", IRB.getVoidTy(),
                                     IRB.getInt64Ty(), PropertyTy);
  CsiBBExit = M.getOrInsertFunction("__csi_bb_exit", IRB.getVoidTy(),
                                    IRB.getInt64Ty(), PropertyTy);
}

/// Loop hook initialization
void CSIImpl::initializeLoopHooks() {
  LLVMContext &C = M.getContext();
  IRBuilder<> IRB(C);
  Type *IDType = IRB.getInt64Ty();
  Type *LoopPropertyTy = CsiLoopProperty::getType(C);
  Type *LoopExitPropertyTy = CsiLoopExitProperty::getType(C);

  CsiBeforeLoop = M.getOrInsertFunction("__csi_before_loop", IRB.getVoidTy(),
                                        IDType, IRB.getInt64Ty(),
                                        LoopPropertyTy);
  CsiAfterLoop = M.getOrInsertFunction("__csi_after_loop", IRB.getVoidTy(),
                                       IDType, LoopPropertyTy);

  CsiLoopBodyEntry = M.getOrInsertFunction("__csi_loopbody_entry",
                                           IRB.getVoidTy(), IDType,
                                           LoopPropertyTy);
  CsiLoopBodyExit = M.getOrInsertFunction("__csi_loopbody_exit",
                                          IRB.getVoidTy(), IDType, IDType,
                                          LoopExitPropertyTy);
}

// Call-site hook initialization
void CSIImpl::initializeCallsiteHooks() {
  LLVMContext &C = M.getContext();
  IRBuilder<> IRB(C);
  Type *PropertyTy = CsiCallProperty::getType(C);
  CsiBeforeCallsite = M.getOrInsertFunction("__csi_before_call",
                                            IRB.getVoidTy(), IRB.getInt64Ty(),
                                            IRB.getInt64Ty(), PropertyTy);
  CsiAfterCallsite = M.getOrInsertFunction("__csi_after_call", IRB.getVoidTy(),
                                           IRB.getInt64Ty(), IRB.getInt64Ty(),
                                           PropertyTy);
}

// Alloca (local variable) hook initialization
void CSIImpl::initializeAllocaHooks() {
  LLVMContext &C = M.getContext();
  IRBuilder<> IRB(C);
  Type *IDType = IRB.getInt64Ty();
  Type *AddrType = IRB.getInt8PtrTy();
  Type *PropType = CsiAllocaProperty::getType(C);

  CsiBeforeAlloca = M.getOrInsertFunction("__csi_before_alloca",
                                          IRB.getVoidTy(), IDType, IntptrTy,
                                          PropType);
  CsiAfterAlloca = M.getOrInsertFunction("__csi_after_alloca", IRB.getVoidTy(),
                                         IDType, AddrType, IntptrTy, PropType);
}

// Non-local-variable allocation/free hook initialization
void CSIImpl::initializeAllocFnHooks() {
  LLVMContext &C = M.getContext();
  IRBuilder<> IRB(C);
  Type *RetType = IRB.getVoidTy();
  Type *IDType = IRB.getInt64Ty();
  Type *AddrType = IRB.getInt8PtrTy();
  Type *LargeNumBytesType = IntptrTy;
  Type *AllocFnPropType = CsiAllocFnProperty::getType(C);
  Type *FreePropType = CsiFreeProperty::getType(C);

  CsiBeforeAllocFn = M.getOrInsertFunction("__csi_before_allocfn", RetType,
                                           IDType, LargeNumBytesType,
                                           LargeNumBytesType, LargeNumBytesType,
                                           AddrType, AllocFnPropType);
  CsiAfterAllocFn = M.getOrInsertFunction("__csi_after_allocfn", RetType,
                                          IDType, /* new ptr */ AddrType,
                                          /* size */ LargeNumBytesType,
                                          /* num elements */ LargeNumBytesType,
                                          /* alignment */ LargeNumBytesType,
                                          /* old ptr */ AddrType,
                                          /* property */ AllocFnPropType);

  CsiBeforeFree = M.getOrInsertFunction("__csi_before_free", RetType, IDType,
                                        AddrType, FreePropType);
  CsiAfterFree = M.getOrInsertFunction("__csi_after_free", RetType, IDType,
                                       AddrType, FreePropType);
}

// Load and store hook initialization
void CSIImpl::initializeLoadStoreHooks() {
  LLVMContext &C = M.getContext();
  IRBuilder<> IRB(C);
  Type *LoadPropertyTy = CsiLoadStoreProperty::getType(C);
  Type *StorePropertyTy = CsiLoadStoreProperty::getType(C);
  Type *RetType = IRB.getVoidTy();
  Type *AddrType = IRB.getInt8PtrTy();
  Type *NumBytesType = IRB.getInt32Ty();

  CsiBeforeRead = M.getOrInsertFunction("__csi_before_load", RetType,
                                        IRB.getInt64Ty(), AddrType,
                                        NumBytesType, LoadPropertyTy);
  CsiAfterRead = M.getOrInsertFunction("__csi_after_load", RetType,
                                       IRB.getInt64Ty(), AddrType, NumBytesType,
                                       LoadPropertyTy);

  CsiBeforeWrite = M.getOrInsertFunction("__csi_before_store", RetType,
                                         IRB.getInt64Ty(), AddrType,
                                         NumBytesType, StorePropertyTy);
  CsiAfterWrite = M.getOrInsertFunction("__csi_after_store", RetType,
                                        IRB.getInt64Ty(), AddrType,
                                        NumBytesType, StorePropertyTy);
}

// Initialization of hooks for LLVM memory intrinsics
void CSIImpl::initializeMemIntrinsicsHooks() {
  LLVMContext &C = M.getContext();
  IRBuilder<> IRB(C);

  MemmoveFn = M.getOrInsertFunction("memmove", IRB.getInt8PtrTy(),
                                    IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
                                    IntptrTy);
  MemcpyFn = M.getOrInsertFunction("memcpy", IRB.getInt8PtrTy(),
                                   IRB.getInt8PtrTy(), IRB.getInt8PtrTy(),
                                   IntptrTy);
  MemsetFn = M.getOrInsertFunction("memset", IRB.getInt8PtrTy(),
                                   IRB.getInt8PtrTy(), IRB.getInt32Ty(),
                                   IntptrTy);
}

// Initialization of Tapir hooks
void CSIImpl::initializeTapirHooks() {
  LLVMContext &C = M.getContext();
  IRBuilder<> IRB(C);
  Type *IDType = IRB.getInt64Ty();
  Type *RetType = IRB.getVoidTy();
  Type *TaskPropertyTy = CsiTaskProperty::getType(C);
  Type *TaskExitPropertyTy = CsiTaskExitProperty::getType(C);
  Type *DetContPropertyTy = CsiDetachContinueProperty::getType(C);

  CsiDetach = M.getOrInsertFunction("__csi_detach", RetType,
                                    /* detach_id */ IDType,
                                    IntegerType::getInt32Ty(C)->getPointerTo());
  CsiTaskEntry = M.getOrInsertFunction("__csi_task", RetType,
                                       /* task_id */ IDType,
                                       /* detach_id */ IDType,
                                       TaskPropertyTy);
  CsiTaskExit = M.getOrInsertFunction("__csi_task_exit", RetType,
                                      /* task_exit_id */ IDType,
                                      /* task_id */ IDType,
                                      /* detach_id */ IDType,
                                      TaskExitPropertyTy);
  CsiDetachContinue = M.getOrInsertFunction("__csi_detach_continue", RetType,
                                            /* detach_continue_id */ IDType,
                                            /* detach_id */ IDType,
                                            DetContPropertyTy);
  CsiBeforeSync = M.getOrInsertFunction(
      "__csi_before_sync", RetType, IDType,
      IntegerType::getInt32Ty(C)->getPointerTo());
  CsiAfterSync = M.getOrInsertFunction(
      "__csi_after_sync", RetType, IDType,
      IntegerType::getInt32Ty(C)->getPointerTo());
}

// Prepare any calls in the CFG for instrumentation, e.g., by making sure any
// call that can throw is modeled with an invoke.
void CSIImpl::setupCalls(Function &F) {
  // If F does not throw, then no need to promote calls to invokes.
  if (F.doesNotThrow())
    return;

  promoteCallsInTasksToInvokes(F, "csi.cleanup");
}

static BasicBlock *SplitOffPreds(BasicBlock *BB,
                                 SmallVectorImpl<BasicBlock *> &Preds,
                                 DominatorTree *DT, LoopInfo *LI) {
  if (BB->isLandingPad()) {
    SmallVector<BasicBlock *, 2> NewBBs;
    SplitLandingPadPredecessors(BB, Preds, ".csi-split-lp", ".csi-split",
                                NewBBs, DT, LI);
    return NewBBs[1];
  }

  BasicBlock *NewBB = SplitBlockPredecessors(BB, Preds, ".csi-split", DT, LI);
  if (isa<UnreachableInst>(BB->getFirstNonPHIOrDbg()))
    // If the block being split is simply contains an unreachable, then replace
    // the terminator of the new block with an unreachable.  This helps preserve
    // invariants on the CFG structure for Tapir placeholder blocks following
    // detached.rethrow and taskframe.resume terminators.
    ReplaceInstWithInst(NewBB->getTerminator(),
                        new UnreachableInst(BB->getContext()));
  return BB;
}

// Setup each block such that all of its predecessors belong to the same CSI ID
// space.
static void setupBlock(BasicBlock *BB, const TargetLibraryInfo *TLI,
                       DominatorTree *DT, LoopInfo *LI) {
  if (BB->getUniquePredecessor())
    return;

  SmallVector<BasicBlock *, 4> DetachPreds;
  SmallVector<BasicBlock *, 4> TFResumePreds;
  SmallVector<BasicBlock *, 4> SyncPreds;
  SmallVector<BasicBlock *, 4> SyncUnwindPreds;
  SmallVector<BasicBlock *, 4> AllocFnPreds;
  SmallVector<BasicBlock *, 4> InvokePreds;
  bool HasOtherPredTypes = false;
  unsigned NumPredTypes = 0;

  // Partition the predecessors of the landing pad.
  for (BasicBlock *Pred : predecessors(BB)) {
    if (isa<DetachInst>(Pred->getTerminator()) ||
        isa<ReattachInst>(Pred->getTerminator()) ||
        isDetachedRethrow(Pred->getTerminator()))
      DetachPreds.push_back(Pred);
    else if (isTaskFrameResume(Pred->getTerminator()))
      TFResumePreds.push_back(Pred);
    else if (isa<SyncInst>(Pred->getTerminator()))
      SyncPreds.push_back(Pred);
    else if (isSyncUnwind(Pred->getTerminator()))
      SyncUnwindPreds.push_back(Pred);
    else if (isAllocationFn(Pred->getTerminator(), TLI))
      AllocFnPreds.push_back(Pred);
    else if (isa<InvokeInst>(Pred->getTerminator()))
      InvokePreds.push_back(Pred);
    else
      HasOtherPredTypes = true;
  }

  NumPredTypes = static_cast<unsigned>(!DetachPreds.empty()) +
                 static_cast<unsigned>(!TFResumePreds.empty()) +
                 static_cast<unsigned>(!SyncPreds.empty()) +
                 static_cast<unsigned>(!SyncUnwindPreds.empty()) +
                 static_cast<unsigned>(!AllocFnPreds.empty()) +
                 static_cast<unsigned>(!InvokePreds.empty()) +
                 static_cast<unsigned>(HasOtherPredTypes);

  BasicBlock *BBToSplit = BB;
  // Split off the predecessors of each type.
  if (!SyncPreds.empty() && NumPredTypes > 1) {
    BBToSplit = SplitOffPreds(BBToSplit, SyncPreds, DT, LI);
    NumPredTypes--;
  }
  if (!SyncUnwindPreds.empty() && NumPredTypes > 1) {
    BBToSplit = SplitOffPreds(BBToSplit, SyncUnwindPreds, DT, LI);
    NumPredTypes--;
  }
  if (!AllocFnPreds.empty() && NumPredTypes > 1) {
    BBToSplit = SplitOffPreds(BBToSplit, AllocFnPreds, DT, LI);
    NumPredTypes--;
  }
  if (!InvokePreds.empty() && NumPredTypes > 1) {
    BBToSplit = SplitOffPreds(BBToSplit, InvokePreds, DT, LI);
    NumPredTypes--;
  }
  if (!TFResumePreds.empty() && NumPredTypes > 1) {
    BBToSplit = SplitOffPreds(BBToSplit, TFResumePreds, DT, LI);
    NumPredTypes--;
  }
  // We handle detach and detached.rethrow predecessors at the end to preserve
  // invariants on the CFG structure about the deadness of basic blocks after
  // detached-rethrows.
  if (!DetachPreds.empty() && NumPredTypes > 1) {
    BBToSplit = SplitOffPreds(BBToSplit, DetachPreds, DT, LI);
    NumPredTypes--;
  }
}

// Setup all basic blocks such that each block's predecessors belong entirely to
// one CSI ID space.
void CSIImpl::setupBlocks(Function &F, const TargetLibraryInfo *TLI,
                          DominatorTree *DT, LoopInfo *LI) {
  SmallPtrSet<BasicBlock *, 8> BlocksToSetup;
  for (BasicBlock &BB : F) {
    if (BB.isLandingPad())
      BlocksToSetup.insert(&BB);

    if (InvokeInst *II = dyn_cast<InvokeInst>(BB.getTerminator())) {
      if (!isTapirPlaceholderSuccessor(II->getNormalDest()))
        BlocksToSetup.insert(II->getNormalDest());
    } else if (SyncInst *SI = dyn_cast<SyncInst>(BB.getTerminator()))
      BlocksToSetup.insert(SI->getSuccessor(0));
  }

  for (BasicBlock *BB : BlocksToSetup)
    setupBlock(BB, TLI, DT, LI);
}

// Split basic blocks so that ordinary call instructions terminate basic blocks.
void CSIImpl::splitBlocksAtCalls(Function &F, DominatorTree *DT, LoopInfo *LI) {
  // Split basic blocks after call instructions.
  SmallVector<Instruction *, 32> CallsToSplit;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (isa<CallInst>(I) &&
          // Skip placeholder call instructions
          !callsPlaceholderFunction(I) &&
          // Skip a call instruction if it is immediately followed by a
          // terminator
          !I.getNextNode()->isTerminator() &&
          // If the call does not return, don't bother splitting
          !cast<CallInst>(&I)->doesNotReturn())
        CallsToSplit.push_back(&I);

  for (Instruction *Call : CallsToSplit)
    SplitBlock(Call->getParent(), Call->getNextNode(), DT, LI);
}

int CSIImpl::getNumBytesAccessed(Value *Addr, Type *OrigTy,
                                 const DataLayout &DL) {
  assert(OrigTy->isSized());
  uint32_t TypeSize = DL.getTypeStoreSizeInBits(OrigTy);
  if (TypeSize % 8 != 0)
    return -1;
  return TypeSize / 8;
}

void CSIImpl::addLoadStoreInstrumentation(Instruction *I,
                                          FunctionCallee BeforeFn,
                                          FunctionCallee AfterFn, Value *CsiId,
                                          Type *AddrType, Value *Addr,
                                          int NumBytes,
                                          CsiLoadStoreProperty &Prop) {
  IRBuilder<> IRB(I);
  Value *PropVal = Prop.getValue(IRB);
  insertHookCall(I, BeforeFn,
                 {CsiId, IRB.CreatePointerCast(Addr, AddrType),
                  IRB.getInt32(NumBytes), PropVal});

  BasicBlock::iterator Iter = ++I->getIterator();
  IRB.SetInsertPoint(&*Iter);
  insertHookCall(&*Iter, AfterFn,
                 {CsiId, IRB.CreatePointerCast(Addr, AddrType),
                  IRB.getInt32(NumBytes), PropVal});
}

void CSIImpl::instrumentLoadOrStore(Instruction *I,
                                    CsiLoadStoreProperty &Prop) {
  IRBuilder<> IRB(I);
  bool IsWrite = isa<StoreInst>(I);
  Value *Addr = IsWrite ? cast<StoreInst>(I)->getPointerOperand()
                        : cast<LoadInst>(I)->getPointerOperand();
  Type *Ty =
      IsWrite ? cast<StoreInst>(I)->getValueOperand()->getType() : I->getType();
  int NumBytes = getNumBytesAccessed(Addr, Ty, DL);
  Type *AddrType = IRB.getInt8PtrTy();

  if (NumBytes == -1)
    return; // size that we don't recognize

  if (IsWrite) {
    uint64_t LocalId = StoreFED.add(*I);
    Value *CsiId = StoreFED.localToGlobalId(LocalId, IRB);
    addLoadStoreInstrumentation(I, CsiBeforeWrite, CsiAfterWrite, CsiId,
                                AddrType, Addr, NumBytes, Prop);
  } else { // is read
    uint64_t LocalId = LoadFED.add(*I);
    Value *CsiId = LoadFED.localToGlobalId(LocalId, IRB);
    addLoadStoreInstrumentation(I, CsiBeforeRead, CsiAfterRead, CsiId, AddrType,
                                Addr, NumBytes, Prop);
  }
}

void CSIImpl::instrumentAtomic(Instruction *I) {
  // For now, print a message that this code contains atomics.
  dbgs()
      << "WARNING: Uninstrumented atomic operations in program-under-test!\n";
}

// TODO: This code for instrumenting memory intrinsics was borrowed
// from TSan.  Different tools might have better ways to handle these
// function calls.  Replace this logic with a more flexible solution,
// possibly one based on interpositioning.
//
// If a memset intrinsic gets inlined by the code gen, we will miss it.
// So, we either need to ensure the intrinsic is not inlined, or instrument it.
// We do not instrument memset/memmove/memcpy intrinsics (too complicated),
// instead we simply replace them with regular function calls, which are then
// intercepted by the run-time.
// Since our pass runs after everyone else, the calls should not be
// replaced back with intrinsics. If that becomes wrong at some point,
// we will need to call e.g. __csi_memset to avoid the intrinsics.
bool CSIImpl::instrumentMemIntrinsic(Instruction *I) {
  IRBuilder<> IRB(I);
  if (MemSetInst *M = dyn_cast<MemSetInst>(I)) {
    Instruction *Call = IRB.CreateCall(
        MemsetFn,
        {IRB.CreatePointerCast(M->getArgOperand(0), IRB.getInt8PtrTy()),
         IRB.CreateIntCast(M->getArgOperand(1), IRB.getInt32Ty(), false),
         IRB.CreateIntCast(M->getArgOperand(2), IntptrTy, false)});
    setInstrumentationDebugLoc(I, Call);
    I->eraseFromParent();
    return true;
  } else if (MemTransferInst *M = dyn_cast<MemTransferInst>(I)) {
    Instruction *Call = IRB.CreateCall(
        isa<MemCpyInst>(M) ? MemcpyFn : MemmoveFn,
        {IRB.CreatePointerCast(M->getArgOperand(0), IRB.getInt8PtrTy()),
         IRB.CreatePointerCast(M->getArgOperand(1), IRB.getInt8PtrTy()),
         IRB.CreateIntCast(M->getArgOperand(2), IntptrTy, false)});
    setInstrumentationDebugLoc(I, Call);
    I->eraseFromParent();
    return true;
  }
  return false;
}

void CSIImpl::instrumentBasicBlock(BasicBlock &BB) {
  IRBuilder<> IRB(&*BB.getFirstInsertionPt());
  uint64_t LocalId = BasicBlockFED.add(BB);
  uint64_t BBSizeId = BBSize.add(BB, GetTTI ?
                                 &(*GetTTI)(*BB.getParent()) : nullptr);
  assert(LocalId == BBSizeId &&
         "BB recieved different ID's in FED and sizeinfo tables.");
  Value *CsiId = BasicBlockFED.localToGlobalId(LocalId, IRB);
  CsiBBProperty Prop;
  Prop.setIsLandingPad(BB.isLandingPad());
  Prop.setIsEHPad(BB.isEHPad());
  Instruction *TI = BB.getTerminator();
  Value *PropVal = Prop.getValue(IRB);
  insertHookCall(&*IRB.GetInsertPoint(), CsiBBEntry, {CsiId, PropVal});
  IRB.SetInsertPoint(TI);
  insertHookCall(TI, CsiBBExit, {CsiId, PropVal});
}

// Helper function to get a value for the runtime trip count of the given loop.
static const SCEV *getRuntimeTripCount(Loop &L, ScalarEvolution *SE) {
  BasicBlock *Latch = L.getLoopLatch();

  const SCEV *BECountSC = SE->getExitCount(&L, Latch);
  if (isa<SCEVCouldNotCompute>(BECountSC) ||
      !BECountSC->getType()->isIntegerTy()) {
    LLVM_DEBUG(dbgs() << "Could not compute exit block SCEV\n");
    return SE->getCouldNotCompute();
  }

  // Add 1 since the backedge count doesn't include the first loop iteration.
  const SCEV *TripCountSC =
      SE->getAddExpr(BECountSC, SE->getConstant(BECountSC->getType(), 1));
  if (isa<SCEVCouldNotCompute>(TripCountSC)) {
    LLVM_DEBUG(dbgs() << "Could not compute trip count SCEV.\n");
    return SE->getCouldNotCompute();
  }

  return TripCountSC;
}

void CSIImpl::instrumentLoop(Loop &L, TaskInfo &TI, ScalarEvolution *SE) {
  assert(L.isLoopSimplifyForm() && "CSI assumes loops are in simplified form.");
  BasicBlock *Preheader = L.getLoopPreheader();
  BasicBlock *Header = L.getHeader();
  SmallVector<BasicBlock *, 4> ExitingBlocks, ExitBlocks;
  L.getExitingBlocks(ExitingBlocks);
  L.getUniqueExitBlocks(ExitBlocks);

  // We assign a local ID for this loop here, so that IDs for loops follow a
  // depth-first ordering.
  csi_id_t LocalId = LoopFED.add(*Header);

  // Recursively instrument each subloop.
  for (Loop *SubL : L)
    instrumentLoop(*SubL, TI, SE);

  // Record properties of this loop.
  CsiLoopProperty LoopProp;
  LoopProp.setIsTapirLoop(static_cast<bool>(getTaskIfTapirLoop(&L, &TI)));
  LoopProp.setHasUniqueExitingBlock((ExitingBlocks.size() == 1));

  IRBuilder<> IRB(Preheader->getTerminator());
  Value *LoopCsiId = LoopFED.localToGlobalId(LocalId, IRB);
  Value *LoopPropVal = LoopProp.getValue(IRB);

  // Try to evaluate the runtime trip count for this loop.  Default to a count
  // of -1 for unknown trip counts.
  Value *TripCount = IRB.getInt64(-1);
  if (SE) {
    const SCEV *TripCountSC = getRuntimeTripCount(L, SE);
    if (!isa<SCEVCouldNotCompute>(TripCountSC)) {
      // Extend the TripCount type if necessary.
      if (TripCountSC->getType() != IRB.getInt64Ty())
        TripCountSC = SE->getZeroExtendExpr(TripCountSC, IRB.getInt64Ty());
      // Compute the trip count to pass to the CSI hook.
      SCEVExpander Expander(*SE, DL, "csi");
      TripCount = Expander.expandCodeFor(TripCountSC, IRB.getInt64Ty(),
                                         &*IRB.GetInsertPoint());
    }
  }

  // Insert before-loop hook.
  insertHookCall(&*IRB.GetInsertPoint(), CsiBeforeLoop, {LoopCsiId, TripCount,
                                                         LoopPropVal});

  // Insert loop-body-entry hook.
  IRB.SetInsertPoint(&*Header->getFirstInsertionPt());
  // TODO: Pass IVs to hook?
  insertHookCall(&*IRB.GetInsertPoint(), CsiLoopBodyEntry, {LoopCsiId,
                                                            LoopPropVal});

  // Insert hooks at the ends of the exiting blocks.
  for (BasicBlock *BB : ExitingBlocks) {
    // Record properties of this loop exit
    CsiLoopExitProperty LoopExitProp;
    LoopExitProp.setIsLatch(L.isLoopLatch(BB));

    // Insert the loop-exit hook
    IRB.SetInsertPoint(BB->getTerminator());
    csi_id_t LocalExitId = LoopExitFED.add(*BB);
    Value *ExitCsiId = LoopFED.localToGlobalId(LocalExitId, IRB);
    Value *LoopExitPropVal = LoopExitProp.getValue(IRB);
    // TODO: For latches, record whether the loop will repeat.
    insertHookCall(&*IRB.GetInsertPoint(), CsiLoopBodyExit,
                   {ExitCsiId, LoopCsiId, LoopExitPropVal});
  }
  // Insert after-loop hooks.
  for (BasicBlock *BB : ExitBlocks) {
    IRB.SetInsertPoint(&*BB->getFirstInsertionPt());
    insertHookCall(&*IRB.GetInsertPoint(), CsiAfterLoop, {LoopCsiId,
                                                          LoopPropVal});
  }
}

void CSIImpl::instrumentCallsite(Instruction *I, DominatorTree *DT) {
  if (callsPlaceholderFunction(*I))
    return;

  bool IsInvoke = isa<InvokeInst>(I);
  Function *Called = nullptr;
  if (CallInst *CI = dyn_cast<CallInst>(I))
    Called = CI->getCalledFunction();
  else if (InvokeInst *II = dyn_cast<InvokeInst>(I))
    Called = II->getCalledFunction();

  bool shouldInstrumentBefore = true;
  bool shouldInstrumentAfter = true;

  // Does this call require instrumentation before or after?
  if (Called) {
    shouldInstrumentBefore = Config->DoesFunctionRequireInstrumentationForPoint(
        Called->getName(), InstrumentationPoint::INSTR_BEFORE_CALL);
    shouldInstrumentAfter = Config->DoesFunctionRequireInstrumentationForPoint(
        Called->getName(), InstrumentationPoint::INSTR_AFTER_CALL);
  }

  if (!shouldInstrumentAfter && !shouldInstrumentBefore)
    return;

  IRBuilder<> IRB(I);
  Value *DefaultID = getDefaultID(IRB);
  uint64_t LocalId = CallsiteFED.add(*I, Called ? Called->getName() : "");
  Value *CallsiteId = CallsiteFED.localToGlobalId(LocalId, IRB);
  Value *FuncId = nullptr;
  GlobalVariable *FuncIdGV = nullptr;
  if (Called) {
    std::string GVName = CsiFuncIdVariablePrefix + Called->getName().str();
    Type *FuncIdGVTy = IRB.getInt64Ty();
    FuncIdGV = dyn_cast<GlobalVariable>(
        M.getOrInsertGlobal(GVName, FuncIdGVTy));
    assert(FuncIdGV);
    FuncIdGV->setConstant(false);
    if (Options.jitMode && !Called->empty())
      FuncIdGV->setLinkage(Called->getLinkage());
    else
      FuncIdGV->setLinkage(GlobalValue::WeakAnyLinkage);
    FuncIdGV->setInitializer(IRB.getInt64(CsiCallsiteUnknownTargetId));
    FuncId = IRB.CreateLoad(FuncIdGVTy, FuncIdGV);
  } else {
    // Unknown targets (i.e. indirect calls) are always unknown.
    FuncId = IRB.getInt64(CsiCallsiteUnknownTargetId);
  }
  assert(FuncId != NULL);
  CsiCallProperty Prop;
  Value *DefaultPropVal = Prop.getValue(IRB);
  Prop.setIsIndirect(!Called);
  Value *PropVal = Prop.getValue(IRB);
  if (shouldInstrumentBefore)
    insertHookCall(I, CsiBeforeCallsite, {CallsiteId, FuncId, PropVal});

  BasicBlock::iterator Iter(I);
  if (shouldInstrumentAfter) {
    if (IsInvoke) {
      // There are two "after" positions for invokes: the normal block and the
      // exception block.
      InvokeInst *II = cast<InvokeInst>(I);
      insertHookCallInSuccessorBB(II->getNormalDest(), II->getParent(),
                                  CsiAfterCallsite,
                                  {CallsiteId, FuncId, PropVal},
                                  {DefaultID, DefaultID, DefaultPropVal});
      insertHookCallInSuccessorBB(II->getUnwindDest(), II->getParent(),
                                  CsiAfterCallsite,
                                  {CallsiteId, FuncId, PropVal},
                                  {DefaultID, DefaultID, DefaultPropVal});
    } else {
      // Simple call instruction; there is only one "after" position.
      Iter++;
      IRB.SetInsertPoint(&*Iter);
      PropVal = Prop.getValue(IRB);
      insertHookCall(&*Iter, CsiAfterCallsite, {CallsiteId, FuncId, PropVal});
    }
  }
}

void CSIImpl::interposeCall(Instruction *I) {
  CallBase *CB = dyn_cast<CallBase>(I);
  if (!CB)
    return;

  Function *Called = CB->getCalledFunction();

  // Should we interpose this call?
  if (Called && Called->getName().size() > 0) {
    bool shouldInterpose =
        Config->DoesFunctionRequireInterposition(Called->getName());

    if (shouldInterpose) {
      Function *interpositionFunction = getInterpositionFunction(Called);
      assert(interpositionFunction != nullptr);
      CB->setCalledFunction(interpositionFunction);
    }
  }
}

static void getTaskExits(DetachInst *DI,
                         SmallVectorImpl<BasicBlock *> &TaskReturns,
                         SmallVectorImpl<BasicBlock *> &TaskResumes,
                         SmallVectorImpl<Spindle *> &SharedEHExits,
                         TaskInfo &TI) {
  BasicBlock *DetachedBlock = DI->getDetached();
  Task *T = TI.getTaskFor(DetachedBlock);
  BasicBlock *ContinueBlock = DI->getContinue();

  // Examine the predecessors of the continue block and save any predecessors in
  // the task as a task return.
  for (BasicBlock *Pred : predecessors(ContinueBlock)) {
    if (T->simplyEncloses(Pred)) {
      assert(isa<ReattachInst>(Pred->getTerminator()));
      TaskReturns.push_back(Pred);
    }
  }

  // If the detach cannot throw, we're done.
  if (!DI->hasUnwindDest())
    return;

  // Detached-rethrow exits can appear in strange places within a task-exiting
  // spindle.  Hence we loop over all blocks in the spindle to find
  // detached rethrows.
  for (Spindle *S : depth_first<InTask<Spindle *>>(T->getEntrySpindle())) {
    if (S->isSharedEH()) {
      if (llvm::any_of(predecessors(S),
                       [](const Spindle *Pred) { return !Pred->isSharedEH(); }))
        SharedEHExits.push_back(S);
      continue;
    }

    for (BasicBlock *B : S->blocks())
      if (isDetachedRethrow(B->getTerminator()))
        TaskResumes.push_back(B);
  }
}

BasicBlock::iterator
CSIImpl::getFirstInsertionPtInDetachedBlock(BasicBlock *Detached) {
  for (Instruction &I : *Detached)
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I))
      if (Intrinsic::taskframe_use == II->getIntrinsicID())
        return ++(II->getIterator());
  return Detached->getFirstInsertionPt();
}

void CSIImpl::instrumentDetach(DetachInst *DI, DominatorTree *DT, TaskInfo &TI,
                               LoopInfo &LI,
                               const DenseMap<Value *, Value *> &TrackVars) {
  LLVMContext &Ctx = DI->getContext();
  BasicBlock *TaskEntryBlock = TI.getTaskFor(DI->getParent())->getEntry();
  IRBuilder<> IDBuilder(&*TaskEntryBlock->getFirstInsertionPt());
  bool TapirLoopBody = spawnsTapirLoopBody(DI, LI, TI);
  // Instrument the detach instruction itself
  Value *DetachID;
  {
    IRBuilder<> IRB(DI);
    uint64_t LocalID = DetachFED.add(*DI);
    DetachID = DetachFED.localToGlobalId(LocalID, IDBuilder);
    Value *TrackVar = TrackVars.lookup(DI->getSyncRegion());
    IRB.CreateStore(
        Constant::getIntegerValue(IntegerType::getInt32Ty(Ctx), APInt(32, 1)),
        TrackVar);
    insertHookCall(DI, CsiDetach, {DetachID, TrackVar});
  }

  // Find the detached block, continuation, and associated reattaches.
  BasicBlock *DetachedBlock = DI->getDetached();
  BasicBlock *ContinueBlock = DI->getContinue();
  Task *T = TI.getTaskFor(DetachedBlock);
  SmallVector<BasicBlock *, 8> TaskExits, TaskResumes;
  SmallVector<Spindle *, 2> SharedEHExits;
  getTaskExits(DI, TaskExits, TaskResumes, SharedEHExits, TI);

  // Instrument the entry and exit points of the detached task.
  {
    // Instrument the entry point of the detached task.
    IRBuilder<> IRB(&*DetachedBlock->getFirstInsertionPt());
    uint64_t LocalID = TaskFED.add(*DetachedBlock);
    Value *TaskID = TaskFED.localToGlobalId(LocalID, IDBuilder);
    CsiTaskProperty Prop;
    Prop.setIsTapirLoopBody(TapirLoopBody);
    Instruction *Call = IRB.CreateCall(CsiTaskEntry, {TaskID, DetachID,
                                                      Prop.getValue(IRB)});
    setInstrumentationDebugLoc(*DetachedBlock, Call);

    // Instrument the exit points of the detached tasks.
    for (BasicBlock *Exit : TaskExits) {
      IRBuilder<> IRB(Exit->getTerminator());
      uint64_t LocalID = TaskExitFED.add(*Exit->getTerminator());
      Value *ExitID = TaskExitFED.localToGlobalId(LocalID, IDBuilder);
      CsiTaskExitProperty ExitProp;
      ExitProp.setIsTapirLoopBody(TapirLoopBody);
      insertHookCall(Exit->getTerminator(), CsiTaskExit,
                     {ExitID, TaskID, DetachID, ExitProp.getValue(IRB)});
    }
    // Instrument the EH exits of the detached task.
    for (BasicBlock *Exit : TaskResumes) {
      IRBuilder<> IRB(Exit->getTerminator());
      uint64_t LocalID = TaskExitFED.add(*Exit->getTerminator());
      Value *ExitID = TaskExitFED.localToGlobalId(LocalID, IDBuilder);
      CsiTaskExitProperty ExitProp;
      ExitProp.setIsTapirLoopBody(TapirLoopBody);
      insertHookCall(Exit->getTerminator(), CsiTaskExit,
                     {ExitID, TaskID, DetachID, ExitProp.getValue(IRB)});
    }

    Value *DefaultID = getDefaultID(IDBuilder);
    for (Spindle *SharedEH : SharedEHExits) {
      // Skip shared-eh spindle exits that are placeholder unreachable blocks.
      if (isa<UnreachableInst>(
              SharedEH->getEntry()->getFirstNonPHIOrDbgOrLifetime()))
        continue;
      CsiTaskExitProperty ExitProp;
      ExitProp.setIsTapirLoopBody(TapirLoopBody);
      insertHookCallAtSharedEHSpindleExits(
          SharedEH, T, CsiTaskExit, TaskExitFED,
          {TaskID, DetachID, ExitProp.getValueImpl(Ctx)},
          {DefaultID, DefaultID,
           CsiTaskExitProperty::getDefaultValueImpl(Ctx)});
    }
  }

  // Instrument the continuation of the detach.
  {
    if (isCriticalContinueEdge(DI, 1))
      ContinueBlock = SplitCriticalEdge(
          DI, 1,
          CriticalEdgeSplittingOptions(DT, &LI).setSplitDetachContinue());

    IRBuilder<> IRB(&*ContinueBlock->getFirstInsertionPt());
    uint64_t LocalID = DetachContinueFED.add(*ContinueBlock);
    Value *ContinueID = DetachContinueFED.localToGlobalId(LocalID, IDBuilder);
    CsiDetachContinueProperty ContProp;
    Instruction *Call = IRB.CreateCall(
        CsiDetachContinue, {ContinueID, DetachID, ContProp.getValue(IRB)});
    setInstrumentationDebugLoc(*ContinueBlock, Call);
  }
  // Instrument the unwind of the detach, if it exists.
  if (DI->hasUnwindDest()) {
    BasicBlock *UnwindBlock = DI->getUnwindDest();
    BasicBlock *PredBlock = DI->getParent();
    if (Value *TF = T->getTaskFrameUsed()) {
      // If the detached task uses a taskframe, then we want to insert the
      // detach_continue instrumentation for the unwind destination after the
      // taskframe.resume.
      UnwindBlock = getTaskFrameResumeDest(TF);
      assert(UnwindBlock &&
             "Detach with unwind uses a taskframe with no resume");
      PredBlock = getTaskFrameResume(TF)->getParent();
    }
    Value *DefaultID = getDefaultID(IDBuilder);
    uint64_t LocalID = DetachContinueFED.add(*UnwindBlock);
    Value *ContinueID = DetachContinueFED.localToGlobalId(LocalID, IDBuilder);
    CsiDetachContinueProperty ContProp;
    Value *DefaultPropVal = ContProp.getValueImpl(Ctx);
    ContProp.setIsUnwind();
    insertHookCallInSuccessorBB(UnwindBlock, PredBlock, CsiDetachContinue,
                                {ContinueID, DetachID, ContProp.getValue(Ctx)},
                                {DefaultID, DefaultID, DefaultPropVal});
    for (BasicBlock *DRPred : predecessors(UnwindBlock))
      if (isDetachedRethrow(DRPred->getTerminator(), DI->getSyncRegion()))
        insertHookCallInSuccessorBB(
            UnwindBlock, DRPred, CsiDetachContinue,
            {ContinueID, DetachID, ContProp.getValue(Ctx)},
            {DefaultID, DefaultID, DefaultPropVal});
  }
}

void CSIImpl::instrumentSync(SyncInst *SI,
                             const DenseMap<Value *, Value *> &TrackVars) {
  IRBuilder<> IRB(SI);
  Value *DefaultID = getDefaultID(IRB);
  // Get the ID of this sync.
  uint64_t LocalID = SyncFED.add(*SI);
  Value *SyncID = SyncFED.localToGlobalId(LocalID, IRB);

  Value *TrackVar = TrackVars.lookup(SI->getSyncRegion());

  // Insert instrumentation before the sync.
  insertHookCall(SI, CsiBeforeSync, {SyncID, TrackVar});
  BasicBlock *SyncBB = SI->getParent();
  BasicBlock *SyncCont = SI->getSuccessor(0);
  BasicBlock *SyncUnwind = nullptr;
  if (SyncsWithUnwinds.count(SI)) {
    InvokeInst *II = dyn_cast<InvokeInst>(SyncCont->getTerminator());
    SyncBB = SyncCont;
    SyncUnwind = II->getUnwindDest();
    SyncCont = II->getNormalDest();
  }

  CallInst *Call = insertHookCallInSuccessorBB(
      SyncCont, SyncBB, CsiAfterSync, {SyncID, TrackVar},
      {DefaultID,
       ConstantPointerNull::get(
           IntegerType::getInt32Ty(SI->getContext())->getPointerTo())});
  // Reset the tracking variable to 0.
  if (Call != nullptr) {
    callsAfterSync.insert({SyncCont, Call});
    IRB.SetInsertPoint(Call->getNextNode());
    IRB.CreateStore(
        Constant::getIntegerValue(IntegerType::getInt32Ty(SI->getContext()),
                                  APInt(32, 0)),
        TrackVar);
  } else {
    assert(callsAfterSync.find(SyncCont) != callsAfterSync.end());
  }

  // If we have no unwind for the sync, then we're done.
  if (!SyncUnwind)
    return;

  Call = insertHookCallInSuccessorBB(
      SyncUnwind, SyncBB, CsiAfterSync, {SyncID, TrackVar},
      {DefaultID,
       ConstantPointerNull::get(
           IntegerType::getInt32Ty(SI->getContext())->getPointerTo())});
  // Reset the tracking variable to 0.
  if (Call != nullptr) {
    callsAfterSync.insert({SyncUnwind, Call});
    IRB.SetInsertPoint(Call->getNextNode());
    IRB.CreateStore(
        Constant::getIntegerValue(IntegerType::getInt32Ty(SI->getContext()),
                                  APInt(32, 0)),
        TrackVar);
  } else {
    assert(callsAfterSync.find(SyncUnwind) != callsAfterSync.end());
  }
}

void CSIImpl::instrumentAlloca(Instruction *I) {
  IRBuilder<> IRB(I);
  AllocaInst *AI = cast<AllocaInst>(I);

  uint64_t LocalId = AllocaFED.add(*I);
  Value *CsiId = AllocaFED.localToGlobalId(LocalId, IRB);

  CsiAllocaProperty Prop;
  Prop.setIsStatic(AI->isStaticAlloca());
  Value *PropVal = Prop.getValue(IRB);

  // Get size of allocation.
  uint64_t Size = DL.getTypeAllocSize(AI->getAllocatedType());
  Value *SizeVal = IRB.getInt64(Size);
  if (AI->isArrayAllocation())
    SizeVal = IRB.CreateMul(SizeVal,
                            IRB.CreateZExtOrBitCast(AI->getArraySize(),
                                                    IRB.getInt64Ty()));

  insertHookCall(I, CsiBeforeAlloca, {CsiId, SizeVal, PropVal});
  BasicBlock::iterator Iter(I);
  Iter++;
  IRB.SetInsertPoint(&*Iter);

  Type *AddrType = IRB.getInt8PtrTy();
  Value *Addr = IRB.CreatePointerCast(I, AddrType);
  insertHookCall(&*Iter, CsiAfterAlloca, {CsiId, Addr, SizeVal, PropVal});
}

bool CSIImpl::getAllocFnArgs(const Instruction *I,
                             SmallVectorImpl<Value *> &AllocFnArgs,
                             Type *SizeTy, Type *AddrTy,
                             const TargetLibraryInfo &TLI) {
  const CallBase *CB = dyn_cast<CallBase>(I);

  std::pair<Value *, Value *> SizeArgs =
      getAllocSizeArgs(CB, &TLI, /*IgnoreBuiltinAttr=*/true);
  // If the first size argument is null, then we failed to get size arguments
  // for this call.
  if (!SizeArgs.first)
    return false;

  Value *AlignmentArg = getAllocAlignment(CB, &TLI, /*IgnoreBuiltinAttr=*/true);

  // Push the size arguments.
  AllocFnArgs.push_back(SizeArgs.first);
  // The second size argument is the number of elements allocated (i.e., for
  // calloc-like functions).
  if (SizeArgs.second)
    AllocFnArgs.push_back(SizeArgs.second);
  else
    // Report number of elements == 1.
    AllocFnArgs.push_back(ConstantInt::get(SizeTy, 1));

  // Push the alignment argument or 0 if there is no alignment argument.
  if (AlignmentArg)
    AllocFnArgs.push_back(AlignmentArg);
  else
    AllocFnArgs.push_back(ConstantInt::get(SizeTy, 0));

  // Return the old pointer argument for realloc-like functions or nullptr for
  // other allocation functions.
  if (isReallocLikeFn(CB, &TLI))
    AllocFnArgs.push_back(CB->getArgOperand(0));
  else
    AllocFnArgs.push_back(Constant::getNullValue(AddrTy));

  return true;
}

void CSIImpl::instrumentAllocFn(Instruction *I, DominatorTree *DT,
                                const TargetLibraryInfo *TLI) {
  bool IsInvoke = isa<InvokeInst>(I);
  Function *Called = nullptr;
  if (CallInst *CI = dyn_cast<CallInst>(I))
    Called = CI->getCalledFunction();
  else if (InvokeInst *II = dyn_cast<InvokeInst>(I))
    Called = II->getCalledFunction();

  assert(Called && "Could not get called function for allocation fn.");

  IRBuilder<> IRB(I);
  Value *DefaultID = getDefaultID(IRB);
  uint64_t LocalId = AllocFnFED.add(*I);
  Value *AllocFnId = AllocFnFED.localToGlobalId(LocalId, IRB);

  SmallVector<Value *, 4> AllocFnArgs;
  getAllocFnArgs(I, AllocFnArgs, IntptrTy, IRB.getInt8PtrTy(), *TLI);
  SmallVector<Value *, 4> DefaultAllocFnArgs({
      /* Allocated size */ Constant::getNullValue(IntptrTy),
      /* Number of elements */ Constant::getNullValue(IntptrTy),
      /* Alignment */ Constant::getNullValue(IntptrTy),
      /* Old pointer */ Constant::getNullValue(IRB.getInt8PtrTy()),
  });

  CsiAllocFnProperty Prop;
  Value *DefaultPropVal = Prop.getValue(IRB);
  LibFunc AllocLibF;
  TLI->getLibFunc(*Called, AllocLibF);
  Prop.setAllocFnTy(static_cast<unsigned>(getAllocFnTy(AllocLibF)));
  AllocFnArgs.push_back(Prop.getValue(IRB));
  DefaultAllocFnArgs.push_back(DefaultPropVal);

  BasicBlock::iterator Iter(I);
  if (IsInvoke) {
    // There are two "after" positions for invokes: the normal block and the
    // exception block.
    InvokeInst *II = cast<InvokeInst>(I);

    BasicBlock *NormalBB = II->getNormalDest();
    unsigned SuccNum = GetSuccessorNumber(II->getParent(), NormalBB);
    if (isCriticalEdge(II, SuccNum))
      NormalBB =
          SplitCriticalEdge(II, SuccNum, CriticalEdgeSplittingOptions(DT));
    // Insert hook into normal destination.
    {
      IRB.SetInsertPoint(&*NormalBB->getFirstInsertionPt());
      SmallVector<Value *, 4> AfterAllocFnArgs;
      AfterAllocFnArgs.push_back(AllocFnId);
      AfterAllocFnArgs.push_back(IRB.CreatePointerCast(I, IRB.getInt8PtrTy()));
      AfterAllocFnArgs.append(AllocFnArgs.begin(), AllocFnArgs.end());
      insertHookCall(&*IRB.GetInsertPoint(), CsiAfterAllocFn, AfterAllocFnArgs);
    }
    // Insert hook into unwind destination.
    {
      // The return value of the allocation function is not valid in the unwind
      // destination.
      SmallVector<Value *, 4> AfterAllocFnArgs, DefaultAfterAllocFnArgs;
      AfterAllocFnArgs.push_back(AllocFnId);
      AfterAllocFnArgs.push_back(Constant::getNullValue(IRB.getInt8PtrTy()));
      AfterAllocFnArgs.append(AllocFnArgs.begin(), AllocFnArgs.end());
      DefaultAfterAllocFnArgs.push_back(DefaultID);
      DefaultAfterAllocFnArgs.push_back(
          Constant::getNullValue(IRB.getInt8PtrTy()));
      DefaultAfterAllocFnArgs.append(DefaultAllocFnArgs.begin(),
                                     DefaultAllocFnArgs.end());
      insertHookCallInSuccessorBB(II->getUnwindDest(), II->getParent(),
                                  CsiAfterAllocFn, AfterAllocFnArgs,
                                  DefaultAfterAllocFnArgs);
    }
  } else {
    // Simple call instruction; there is only one "after" position.
    Iter++;
    IRB.SetInsertPoint(&*Iter);
    SmallVector<Value *, 4> AfterAllocFnArgs;
    AfterAllocFnArgs.push_back(AllocFnId);
    AfterAllocFnArgs.push_back(IRB.CreatePointerCast(I, IRB.getInt8PtrTy()));
    AfterAllocFnArgs.append(AllocFnArgs.begin(), AllocFnArgs.end());
    insertHookCall(&*Iter, CsiAfterAllocFn, AfterAllocFnArgs);
  }
}

void CSIImpl::instrumentFree(Instruction *I, const TargetLibraryInfo *TLI) {
  // It appears that frees (and deletes) never throw.
  assert(isa<CallInst>(I) && "Free call is not a call instruction");

  CallInst *FC = cast<CallInst>(I);
  Function *Called = FC->getCalledFunction();
  assert(Called && "Could not get called function for free.");

  IRBuilder<> IRB(I);
  uint64_t LocalId = FreeFED.add(*I);
  Value *FreeId = FreeFED.localToGlobalId(LocalId, IRB);

  Value *Addr = FC->getArgOperand(0);
  CsiFreeProperty Prop;
  LibFunc FreeLibF;
  TLI->getLibFunc(*Called, FreeLibF);
  Prop.setFreeTy(static_cast<unsigned>(getFreeTy(FreeLibF)));

  insertHookCall(I, CsiBeforeFree, {FreeId, Addr, Prop.getValue(IRB)});
  BasicBlock::iterator Iter(I);
  Iter++;
  insertHookCall(&*Iter, CsiAfterFree, {FreeId, Addr, Prop.getValue(IRB)});
}

CallInst *CSIImpl::insertHookCall(Instruction *I, FunctionCallee HookFunction,
                                  ArrayRef<Value *> HookArgs) {
  IRBuilder<> IRB(I);
  CallInst *Call = IRB.CreateCall(HookFunction, HookArgs);
  setInstrumentationDebugLoc(I, (Instruction *)Call);
  return Call;
}

bool CSIImpl::updateArgPHIs(BasicBlock *Succ, BasicBlock *BB,
                            FunctionCallee HookFunction,
                            ArrayRef<Value *> HookArgs,
                            ArrayRef<Value *> DefaultArgs) {
  // If we've already created a PHI node in this block for the hook arguments,
  // just add the incoming arguments to the PHIs.
  auto Key = std::make_pair(Succ, cast<Function>(HookFunction.getCallee()));
  if (ArgPHIs.count(Key)) {
    unsigned HookArgNum = 0;
    for (PHINode *ArgPHI : ArgPHIs[Key]) {
      ArgPHI->setIncomingValue(ArgPHI->getBasicBlockIndex(BB),
                               HookArgs[HookArgNum]);
      ++HookArgNum;
    }
    return true;
  }

  // Create PHI nodes in this block for each hook argument.
  IRBuilder<> IRB(&Succ->front());
  unsigned HookArgNum = 0;
  for (Value *Arg : HookArgs) {
    PHINode *ArgPHI = IRB.CreatePHI(Arg->getType(), 2);
    for (BasicBlock *Pred : predecessors(Succ)) {
      if (Pred == BB)
        ArgPHI->addIncoming(Arg, BB);
      else
        ArgPHI->addIncoming(DefaultArgs[HookArgNum], Pred);
    }
    ArgPHIs[Key].push_back(ArgPHI);
    ++HookArgNum;
  }
  return false;
}

CallInst *CSIImpl::insertHookCallInSuccessorBB(BasicBlock *Succ, BasicBlock *BB,
                                               FunctionCallee HookFunction,
                                               ArrayRef<Value *> HookArgs,
                                               ArrayRef<Value *> DefaultArgs) {
  assert(HookFunction && "No hook function given.");
  // If this successor block has a unique predecessor, just insert the hook call
  // as normal.
  if (Succ->getUniquePredecessor()) {
    assert(Succ->getUniquePredecessor() == BB &&
           "BB is not unique predecessor of successor block");
    return insertHookCall(&*Succ->getFirstInsertionPt(), HookFunction,
                          HookArgs);
  }

  if (updateArgPHIs(Succ, BB, HookFunction, HookArgs, DefaultArgs))
    return nullptr;

  auto Key = std::make_pair(Succ, cast<Function>(HookFunction.getCallee()));
  SmallVector<Value *, 2> SuccessorHookArgs;
  for (PHINode *ArgPHI : ArgPHIs[Key])
    SuccessorHookArgs.push_back(ArgPHI);

  IRBuilder<> IRB(&*Succ->getFirstInsertionPt());
  // Insert the hook call, using the PHI as the CSI ID.
  CallInst *Call = IRB.CreateCall(HookFunction, SuccessorHookArgs);
  setInstrumentationDebugLoc(*Succ, (Instruction *)Call);

  return Call;
}

void CSIImpl::insertHookCallAtSharedEHSpindleExits(
    Spindle *SharedEHSpindle, Task *T, FunctionCallee HookFunction,
    FrontEndDataTable &FED, ArrayRef<Value *> HookArgs,
    ArrayRef<Value *> DefaultArgs) {
  // Get the set of shared EH spindles to examine.  Store them in post order, so
  // they can be evaluated in reverse post order.
  SmallVector<Spindle *, 2> WorkList;
  for (Spindle *S : post_order<InTask<Spindle *>>(SharedEHSpindle))
    WorkList.push_back(S);

  // Traverse the shared-EH spindles in reverse post order, updating the
  // hook-argument PHI's along the way.
  SmallPtrSet<Spindle *, 2> Visited;
  for (Spindle *S : llvm::reverse(WorkList)) {
    bool NoNewPHINode = true;
    // If this spindle is the first shared-EH spindle in the traversal, use the
    // given hook arguments to update the PHI node.
    if (S == SharedEHSpindle) {
      for (Spindle::SpindleEdge &InEdge : S->in_edges()) {
        Spindle *SPred = InEdge.first;
        BasicBlock *Pred = InEdge.second;
        if (T->contains(SPred))
          NoNewPHINode &=
            updateArgPHIs(S->getEntry(), Pred, HookFunction, HookArgs,
                          DefaultArgs);
      }
    } else {
      // Otherwise update the PHI node based on the predecessor shared-eh
      // spindles in this RPO traversal.
      for (Spindle::SpindleEdge &InEdge : S->in_edges()) {
        Spindle *SPred = InEdge.first;
        BasicBlock *Pred = InEdge.second;
        if (Visited.count(SPred)) {
          auto Key = std::make_pair(SPred->getEntry(),
                                    cast<Function>(HookFunction.getCallee()));
          SmallVector<Value *, 4> NewHookArgs(
              ArgPHIs[Key].begin(), ArgPHIs[Key].end());
          NoNewPHINode &=
            updateArgPHIs(S->getEntry(), Pred, HookFunction, NewHookArgs,
                          DefaultArgs);
        }
      }
    }
    Visited.insert(S);

    if (NoNewPHINode)
      continue;

    // Detached-rethrow exits can appear in strange places within a task-exiting
    // spindle.  Hence we loop over all blocks in the spindle to find detached
    // rethrows.
    auto Key = std::make_pair(S->getEntry(),
                              cast<Function>(HookFunction.getCallee()));
    for (BasicBlock *B : S->blocks()) {
      if (isDetachedRethrow(B->getTerminator())) {
        IRBuilder<> IRB(B->getTerminator());
        uint64_t LocalID = FED.add(*B->getTerminator());
        Value *HookID = FED.localToGlobalId(LocalID, IRB);
        SmallVector<Value *, 4> Args({HookID});
        Args.append(ArgPHIs[Key].begin(), ArgPHIs[Key].end());
        Instruction *Call = IRB.CreateCall(HookFunction, Args);
        setInstrumentationDebugLoc(*B, Call);
      }
    }
  }
}

void CSIImpl::initializeFEDTables() {
  FunctionFED = FrontEndDataTable(M, CsiFunctionBaseIdName,
                                  "__csi_unit_fed_table_function",
                                  "__csi_unit_function_name_",
                                  /*UseExistingBaseId=*/false);
  FunctionExitFED = FrontEndDataTable(M, CsiFunctionExitBaseIdName,
                                      "__csi_unit_fed_table_function_exit",
                                      "__csi_unit_function_name_");
  LoopFED = FrontEndDataTable(M, CsiLoopBaseIdName,
                              "__csi_unit_fed_table_loop");
  LoopExitFED = FrontEndDataTable(M, CsiLoopExitBaseIdName,
                                  "__csi_unit_fed_table_loop");
  BasicBlockFED = FrontEndDataTable(M, CsiBasicBlockBaseIdName,
                                    "__csi_unit_fed_table_basic_block");
  CallsiteFED = FrontEndDataTable(M, CsiCallsiteBaseIdName,
                                  "__csi_unit_fed_table_callsite",
                                  "__csi_unit_function_name_");
  LoadFED = FrontEndDataTable(M, CsiLoadBaseIdName,
                              "__csi_unit_fed_table_load");
  StoreFED = FrontEndDataTable(M, CsiStoreBaseIdName,
                               "__csi_unit_fed_table_store");
  AllocaFED = FrontEndDataTable(M, CsiAllocaBaseIdName,
                                "__csi_unit_fed_table_alloca",
                                "__csi_unit_variable_name_");
  DetachFED = FrontEndDataTable(M, CsiDetachBaseIdName,
                                "__csi_unit_fed_table_detach");
  TaskFED = FrontEndDataTable(M, CsiTaskBaseIdName,
                              "__csi_unit_fed_table_task");
  TaskExitFED = FrontEndDataTable(M, CsiTaskExitBaseIdName,
                                  "__csi_unit_fed_table_task_exit");
  DetachContinueFED = FrontEndDataTable(M, CsiDetachContinueBaseIdName,
                                        "__csi_unit_fed_table_detach_continue");
  SyncFED = FrontEndDataTable(M, CsiSyncBaseIdName,
                              "__csi_unit_fed_table_sync");
  AllocFnFED = FrontEndDataTable(M, CsiAllocFnBaseIdName,
                                 "__csi_unit_fed_table_allocfn",
                                 "__csi_unit_variable_name_");
  FreeFED = FrontEndDataTable(M, CsiFreeBaseIdName,
                              "__csi_unit_fed_free");
}

void CSIImpl::initializeSizeTables() {
  BBSize = SizeTable(M, CsiBasicBlockBaseIdName);
}

uint64_t CSIImpl::getLocalFunctionID(Function &F) {
  uint64_t LocalId = FunctionFED.add(F);
  FuncOffsetMap[F.getName()] = LocalId;
  return LocalId;
}

void CSIImpl::generateInitCallsiteToFunction() {
  LLVMContext &C = M.getContext();
  BasicBlock *EntryBB = BasicBlock::Create(C, "", InitCallsiteToFunction);
  IRBuilder<> IRB(ReturnInst::Create(C, EntryBB));

  GlobalVariable *Base = FunctionFED.baseId();
  Type *BaseTy = IRB.getInt64Ty();
  LoadInst *LI = IRB.CreateLoad(BaseTy, Base);
  // Traverse the map of function name -> function local id. Generate
  // a store of each function's global ID to the corresponding weak
  // global variable.
  for (const auto &it : FuncOffsetMap) {
    std::string GVName = CsiFuncIdVariablePrefix + it.first.str();
    GlobalVariable *GV = nullptr;
    if ((GV = M.getGlobalVariable(GVName)) == nullptr) {
      GV = new GlobalVariable(M, IRB.getInt64Ty(), false,
                              (Options.jitMode ? GlobalValue::ExternalLinkage :
                               GlobalValue::WeakAnyLinkage),
                              IRB.getInt64(CsiCallsiteUnknownTargetId), GVName);
    }
    assert(GV);
    IRB.CreateStore(IRB.CreateAdd(LI, IRB.getInt64(it.second)), GV);
  }
}

void CSIImpl::initializeCsi() {
  IntptrTy = DL.getIntPtrType(M.getContext());

  initializeFEDTables();
  initializeSizeTables();
  if (Options.InstrumentFuncEntryExit)
    initializeFuncHooks();
  if (Options.InstrumentMemoryAccesses)
    initializeLoadStoreHooks();
  if (Options.InstrumentLoops)
    initializeLoopHooks();
  if (Options.InstrumentBasicBlocks)
    initializeBasicBlockHooks();
  if (Options.InstrumentCalls)
    initializeCallsiteHooks();
  if (Options.InstrumentMemIntrinsics)
    initializeMemIntrinsicsHooks();
  if (Options.InstrumentTapir)
    initializeTapirHooks();
  if (Options.InstrumentAllocas)
    initializeAllocaHooks();
  if (Options.InstrumentAllocFns)
    initializeAllocFnHooks();

  FunctionType *FnType =
      FunctionType::get(Type::getVoidTy(M.getContext()), {}, false);
  InitCallsiteToFunction = cast<Function>(M.getOrInsertFunction(
                                              CsiInitCallsiteToFunctionName,
                                              FnType)
                                          .getCallee());
  assert(InitCallsiteToFunction);

  InitCallsiteToFunction->setLinkage(GlobalValue::InternalLinkage);

  /*
  The runtime declares this as a __thread var --- need to change this decl
  generation or the tool won't compile DisableInstrGV = new GlobalVariable(M,
  IntegerType::get(M.getContext(), 1), false, GlobalValue::ExternalLinkage,
  nullptr, CsiDisableInstrumentationName, nullptr,
                                      GlobalValue::GeneralDynamicTLSModel, 0,
  true);
  */
}

// Create a struct type to match the unit_fed_entry_t type in csirt.c.
StructType *CSIImpl::getUnitFedTableType(LLVMContext &C,
                                         PointerType *EntryPointerType) {
  return StructType::get(IntegerType::get(C, 64), Type::getInt8PtrTy(C, 0),
                         EntryPointerType);
}

Constant *CSIImpl::fedTableToUnitFedTable(Module &M,
                                          StructType *UnitFedTableType,
                                          FrontEndDataTable &FedTable) {
  Constant *NumEntries =
      ConstantInt::get(IntegerType::get(M.getContext(), 64), FedTable.size());
  Constant *BaseIdPtr = ConstantExpr::getPointerCast(
      FedTable.baseId(), Type::getInt8PtrTy(M.getContext(), 0));
  Constant *InsertedTable = FedTable.insertIntoModule(M);
  return ConstantStruct::get(UnitFedTableType, NumEntries, BaseIdPtr,
                             InsertedTable);
}

void CSIImpl::collectUnitFEDTables() {
  LLVMContext &C = M.getContext();
  StructType *UnitFedTableType =
      getUnitFedTableType(C, FrontEndDataTable::getPointerType(C));

  // The order of the FED tables here must match the enum in csirt.c and the
  // instrumentation_counts_t in csi.h.
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, FunctionFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, FunctionExitFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, LoopFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, LoopExitFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, BasicBlockFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, CallsiteFED));
  UnitFedTables.push_back(fedTableToUnitFedTable(M, UnitFedTableType, LoadFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, StoreFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, DetachFED));
  UnitFedTables.push_back(fedTableToUnitFedTable(M, UnitFedTableType, TaskFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, TaskExitFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, DetachContinueFED));
  UnitFedTables.push_back(fedTableToUnitFedTable(M, UnitFedTableType, SyncFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, AllocaFED));
  UnitFedTables.push_back(
      fedTableToUnitFedTable(M, UnitFedTableType, AllocFnFED));
  UnitFedTables.push_back(fedTableToUnitFedTable(M, UnitFedTableType, FreeFED));
}

// Create a struct type to match the unit_obj_entry_t type in csirt.c.
StructType *CSIImpl::getUnitSizeTableType(LLVMContext &C,
                                          PointerType *EntryPointerType) {
  return StructType::get(IntegerType::get(C, 64), EntryPointerType);
}

Constant *CSIImpl::sizeTableToUnitSizeTable(Module &M,
                                            StructType *UnitSizeTableType,
                                            SizeTable &SzTable) {
  Constant *NumEntries =
      ConstantInt::get(IntegerType::get(M.getContext(), 64), SzTable.size());
  // Constant *BaseIdPtr =
  //   ConstantExpr::getPointerCast(FedTable.baseId(),
  //                                Type::getInt8PtrTy(M.getContext(), 0));
  Constant *InsertedTable = SzTable.insertIntoModule(M);
  return ConstantStruct::get(UnitSizeTableType, NumEntries, InsertedTable);
}

void CSIImpl::collectUnitSizeTables() {
  LLVMContext &C = M.getContext();
  StructType *UnitSizeTableType =
      getUnitSizeTableType(C, SizeTable::getPointerType(C));

  UnitSizeTables.push_back(
      sizeTableToUnitSizeTable(M, UnitSizeTableType, BBSize));
}

CallInst *CSIImpl::createRTUnitInitCall(IRBuilder<> &IRB) {
  LLVMContext &C = M.getContext();

  StructType *UnitFedTableType =
      getUnitFedTableType(C, FrontEndDataTable::getPointerType(C));
  StructType *UnitSizeTableType =
      getUnitSizeTableType(C, SizeTable::getPointerType(C));

  // Lookup __csirt_unit_init
  SmallVector<Type *, 4> InitArgTypes({IRB.getInt8PtrTy(),
                                       PointerType::get(UnitFedTableType, 0),
                                       PointerType::get(UnitSizeTableType, 0),
                                       InitCallsiteToFunction->getType()});
  FunctionType *InitFunctionTy =
      FunctionType::get(IRB.getVoidTy(), InitArgTypes, false);
  RTUnitInit = M.getOrInsertFunction(CsiRtUnitInitName, InitFunctionTy);
  assert(isa<Function>(RTUnitInit.getCallee()) &&
         "Failed to get or insert __csirt_unit_init function");

  ArrayType *UnitFedTableArrayType =
      ArrayType::get(UnitFedTableType, UnitFedTables.size());
  Constant *FEDTable = ConstantArray::get(UnitFedTableArrayType, UnitFedTables);
  GlobalVariable *FEDGV = new GlobalVariable(
      M, UnitFedTableArrayType, false, GlobalValue::InternalLinkage, FEDTable,
      CsiUnitFedTableArrayName);
  ArrayType *UnitSizeTableArrayType =
      ArrayType::get(UnitSizeTableType, UnitSizeTables.size());
  Constant *SzTable =
      ConstantArray::get(UnitSizeTableArrayType, UnitSizeTables);
  GlobalVariable *SizeGV = new GlobalVariable(
      M, UnitSizeTableArrayType, false, GlobalValue::InternalLinkage, SzTable,
      CsiUnitSizeTableArrayName);

  Constant *Zero = ConstantInt::get(IRB.getInt32Ty(), 0);
  Value *GepArgs[] = {Zero, Zero};

  // Insert call to __csirt_unit_init
  return IRB.CreateCall(
      RTUnitInit,
      {IRB.CreateGlobalStringPtr(M.getName()),
       ConstantExpr::getGetElementPtr(FEDGV->getValueType(), FEDGV, GepArgs),
       ConstantExpr::getGetElementPtr(SizeGV->getValueType(), SizeGV, GepArgs),
       InitCallsiteToFunction});
}

void CSIImpl::finalizeCsi() {
  // Insert __csi_func_id_<f> weak symbols for all defined functions and
  // generate the runtime code that stores to all of them.
  generateInitCallsiteToFunction();

  Function *Ctor = Function::Create(
      FunctionType::get(Type::getVoidTy(M.getContext()), false),
      GlobalValue::InternalLinkage, CsiRtUnitCtorName, &M);
  BasicBlock *CtorBB = BasicBlock::Create(M.getContext(), "", Ctor);
  IRBuilder<> IRB(ReturnInst::Create(M.getContext(), CtorBB));
  CallInst *Call = createRTUnitInitCall(IRB);
  // TODO: Add version-check to the cunstructor?  See
  // ModuleUtils::createSanitizerCtorAndInitFunctions for example.

  // Add the ctor to llvm.global_ctors via appendToGlobalCtors() if either
  // llvm.global_ctors does not exist or it exists with an initializer.  One of
  // these two conditions should always hold for modules compiled normally, but
  // appendToGlobalCtors can crash if a tool, such as bugpoint, removes the
  // initializer from llvm.global_ctors.  This change facilitates using bugpoint
  // to debug crashes involving CSI.
  if (GlobalVariable *GVCtor = M.getNamedGlobal("llvm.global_ctors")) {
    if (GVCtor->hasInitializer())
      appendToGlobalCtors(M, Ctor, CsiUnitCtorPriority);
  } else {
    appendToGlobalCtors(M, Ctor, CsiUnitCtorPriority);
  }

  CallGraphNode *CNCtor = CG->getOrInsertFunction(Ctor);
  CallGraphNode *CNFunc =
      CG->getOrInsertFunction(cast<Function>(RTUnitInit.getCallee()));
  CNCtor->addCalledFunction(Call, CNFunc);
}

namespace {
// Custom DiagnosticInfo for linking a tool bitcode file.
class CSILinkDiagnosticInfo : public DiagnosticInfo {
  const Module *SrcM;
  const Twine &Msg;

public:
  CSILinkDiagnosticInfo(DiagnosticSeverity Severity, const Module *SrcM,
                        const Twine &Msg)
      : DiagnosticInfo(DK_Lowering, Severity), SrcM(SrcM), Msg(Msg) {}
  void print(DiagnosticPrinter &DP) const override {
    DP << "linking module '" << SrcM->getModuleIdentifier() << "': " << Msg;
  }
};

// Custom DiagnosticHandler to handle diagnostics arising when linking a tool
// bitcode file.
class CSIDiagnosticHandler final : public DiagnosticHandler {
  const Module *SrcM;
  DiagnosticHandler *OrigHandler;

public:
  CSIDiagnosticHandler(const Module *SrcM, DiagnosticHandler *OrigHandler)
      : SrcM(SrcM), OrigHandler(OrigHandler) {}

  bool handleDiagnostics(const DiagnosticInfo &DI) override {
    if (DI.getKind() != DK_Linker)
      return OrigHandler->handleDiagnostics(DI);

    std::string MsgStorage;
    {
      raw_string_ostream Stream(MsgStorage);
      DiagnosticPrinterRawOStream DP(Stream);
      DI.print(DP);
    }
    return OrigHandler->handleDiagnostics(
        CSILinkDiagnosticInfo(DI.getSeverity(), SrcM, MsgStorage));
  }
};
} // namespace

static GlobalVariable *copyGlobalArray(const char *Array, Module &M) {
  // Get the current set of static global constructors.
  if (GlobalVariable *GVA = M.getNamedGlobal(Array)) {
    if (Constant *Init = GVA->getInitializer()) {
      // Copy the existing global constructors into a new variable.
      GlobalVariable *NGV = new GlobalVariable(
          Init->getType(), GVA->isConstant(), GVA->getLinkage(), Init, "",
          GVA->getThreadLocalMode());
      GVA->getParent()->getGlobalList().insert(GVA->getIterator(), NGV);
      return NGV;
    }
  }
  return nullptr;
}

// Replace the modified global array list with the copy of the old version.
static void replaceGlobalArray(const char *Array, Module &M,
                               GlobalVariable *GVACopy) {
  // Get the current version of the global array.
  GlobalVariable *GVA = M.getNamedGlobal(Array);
  GVACopy->takeName(GVA);

  // Nuke the old list, replacing any uses with the new one.
  if (!GVA->use_empty()) {
    Constant *V = GVACopy;
    if (V->getType() != GVA->getType())
      V = ConstantExpr::getBitCast(V, GVA->getType());
    GVA->replaceAllUsesWith(V);
  }
  GVA->eraseFromParent();
}

// Restore the global array to its copy of its previous value.
static void restoreGlobalArray(const char *Array, Module &M,
                               GlobalVariable *GVACopy, bool GVAModified) {
  if (GVACopy) {
    if (GVAModified) {
      // Replace the new global array with the old copy.
      replaceGlobalArray(Array, M, GVACopy);
    } else {
      // The bitcode file doesn't add to the global array, so just delete the
      // copy.
      assert(GVACopy->use_empty());
      GVACopy->eraseFromParent();
    }
  } else { // No global array was copied.
    if (GVAModified) {
      // Create a zero-initialized version of the global array.
      GlobalVariable *NewGV = M.getNamedGlobal(Array);
      ConstantArray *NewCA = cast<ConstantArray>(NewGV->getInitializer());
      Constant *CARepl = ConstantArray::get(
          ArrayType::get(NewCA->getType()->getElementType(), 0), {});
      GlobalVariable *GVRepl = new GlobalVariable(
          CARepl->getType(), NewGV->isConstant(), NewGV->getLinkage(), CARepl,
          "", NewGV->getThreadLocalMode());
      NewGV->getParent()->getGlobalList().insert(NewGV->getIterator(), GVRepl);

      // Replace the global array with the zero-initialized version.
      replaceGlobalArray(Array, M, GVRepl);
    } else {
      // Nothing to do.
    }
  }
}

void CSIImpl::linkInToolFromBitcode(const std::string &BitcodePath) {
  if (BitcodePath != "") {
    LLVMContext &C = M.getContext();
    LLVM_DEBUG(dbgs() << "Using external bitcode file for CSI: "
                      << BitcodePath << "\n");
    SMDiagnostic SMD;

    std::unique_ptr<Module> ToolModule = parseIRFile(BitcodePath, SMD, C);
    if (!ToolModule) {
      C.emitError("CSI: Failed to parse bitcode file: " + BitcodePath);
      return;
    }

    // Get the original DiagnosticHandler for this context.
    std::unique_ptr<DiagnosticHandler> OrigDiagHandler =
        C.getDiagnosticHandler();

    // Setup a CSIDiagnosticHandler for this context, to handle
    // diagnostics that arise from linking ToolModule.
    C.setDiagnosticHandler(std::make_unique<CSIDiagnosticHandler>(
                               ToolModule.get(), OrigDiagHandler.get()));

    // Get list of functions in ToolModule.
    for (Function &TF : *ToolModule)
      FunctionsInBitcode.insert(std::string(TF.getName()));

    GlobalVariable *GVCtorCopy = copyGlobalArray("llvm.global_ctors", M);
    GlobalVariable *GVDtorCopy = copyGlobalArray("llvm.global_dtors", M);
    bool BitcodeAddsCtors = false, BitcodeAddsDtors = false;

    // Link the external module into the current module, copying over global
    // values.
    bool Fail = Linker::linkModules(
        M, std::move(ToolModule), Linker::Flags::LinkOnlyNeeded,
        [&](Module &M, const StringSet<> &GVS) {
          for (StringRef GVName : GVS.keys()) {
            LLVM_DEBUG(dbgs() << "Linking global value " << GVName << "\n");
            if (GVName == "llvm.global_ctors") {
              BitcodeAddsCtors = true;
              continue;
            } else if (GVName == "llvm.global_dtors") {
              BitcodeAddsDtors = true;
              continue;
            }
            // Record this GlobalValue as linked from the bitcode.
            LinkedFromBitcode.insert(M.getNamedValue(GVName));
            if (Function *Fn = M.getFunction(GVName)) {
              if (!Fn->isDeclaration() && !Fn->hasComdat()) {
                // We set the function's linkage as available_externally, so
                // that subsequent optimizations can remove these definitions
                // from the module.  We don't want this module redefining any of
                // these symbols, even if they aren't inlined, because the
                // OpenCilk runtime library will provide those definitions
                // later.
                Fn->setLinkage(Function::AvailableExternallyLinkage);
              }
            } else if (GlobalVariable *GV = M.getGlobalVariable(GVName)) {
              if (!GV->isDeclaration() && !GV->hasComdat()) {
                GV->setLinkage(Function::AvailableExternallyLinkage);
              }
            }
          }
        });
    if (Fail)
      C.emitError("CSI: Failed to link bitcode file: " + Twine(BitcodePath));

    // Restore the original DiagnosticHandler for this context.
    C.setDiagnosticHandler(std::move(OrigDiagHandler));

    restoreGlobalArray("llvm.global_ctors", M, GVCtorCopy, BitcodeAddsCtors);
    restoreGlobalArray("llvm.global_dtors", M, GVDtorCopy, BitcodeAddsDtors);

    LinkedBitcode = true;
  }
}

void CSIImpl::loadConfiguration() {
  if (ClConfigurationFilename != "")
    Config = InstrumentationConfig::ReadFromConfigurationFile(
        ClConfigurationFilename);
  else
    Config = InstrumentationConfig::GetDefault();

  Config->SetConfigMode(ClConfigurationMode);
}

Value *CSIImpl::lookupUnderlyingObject(Value *Addr) const {
  return getUnderlyingObject(Addr, 0);
  // if (!UnderlyingObject.count(Addr))
  //   UnderlyingObject[Addr] = getUnderlyingObject(Addr, 0);

  // return UnderlyingObject[Addr];
}

bool CSIImpl::shouldNotInstrumentFunction(Function &F) {
  Module &M = *F.getParent();
  // Don't instrument standard library calls.
#ifdef WIN32
  if (F.hasName() && F.getName().find("_") == 0) {
    return true;
  }
#endif

  if (F.hasName() && F.getName().find("__csi") != std::string::npos)
    return true;

  // Never instrument the CSI ctor.
  if (F.hasName() && F.getName() == CsiRtUnitCtorName)
    return true;

  // Don't instrument anything in the startup section or the __StaticInit
  // section (MacOSX).
  if (F.getSection() == ".text.startup" ||
      F.getSection().find("__StaticInit") != std::string::npos)
    return true;

  // Don't instrument functions that will run before or
  // simultaneously with CSI ctors.
  GlobalVariable *GV = M.getGlobalVariable("llvm.global_ctors");
  if (GV == nullptr)
    return false;
  if (!GV->hasInitializer() || GV->getInitializer()->isNullValue())
    return false;

  ConstantArray *CA = cast<ConstantArray>(GV->getInitializer());
  for (Use &OP : CA->operands()) {
    if (isa<ConstantAggregateZero>(OP))
      continue;
    ConstantStruct *CS = cast<ConstantStruct>(OP);

    if (Function *CF = dyn_cast<Function>(CS->getOperand(1))) {
      uint64_t Priority =
          dyn_cast<ConstantInt>(CS->getOperand(0))->getLimitedValue();
      if (Priority <= CsiUnitCtorPriority && CF->getName() == F.getName()) {
        // Do not instrument F.
        return true;
      }
    }
  }
  // false means do instrument it.
  return false;
}

bool CSIImpl::isVtableAccess(Instruction *I) {
  if (MDNode *Tag = I->getMetadata(LLVMContext::MD_tbaa))
    return Tag->isTBAAVtableAccess();
  return false;
}

bool CSIImpl::addrPointsToConstantData(Value *Addr) {
  // If this is a GEP, just analyze its pointer operand.
  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Addr))
    Addr = GEP->getPointerOperand();

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    if (GV->isConstant()) {
      return true;
    }
  } else if (LoadInst *L = dyn_cast<LoadInst>(Addr)) {
    if (isVtableAccess(L)) {
      return true;
    }
  }
  return false;
}

bool CSIImpl::isAtomic(Instruction *I) {
  if (LoadInst *LI = dyn_cast<LoadInst>(I))
    return LI->isAtomic() && LI->getSyncScopeID() != SyncScope::SingleThread;
  if (StoreInst *SI = dyn_cast<StoreInst>(I))
    return SI->isAtomic() && SI->getSyncScopeID() != SyncScope::SingleThread;
  if (isa<AtomicRMWInst>(I))
    return true;
  if (isa<AtomicCmpXchgInst>(I))
    return true;
  if (isa<FenceInst>(I))
    return true;
  return false;
}

bool CSIImpl::isThreadLocalObject(Value *Obj) {
  if (GlobalValue *GV = dyn_cast<GlobalValue>(Obj))
    return GV->isThreadLocal();
  return false;
}

void CSIImpl::computeLoadAndStoreProperties(
    SmallVectorImpl<std::pair<Instruction *, CsiLoadStoreProperty>>
        &LoadAndStoreProperties,
    SmallVectorImpl<Instruction *> &BBLoadsAndStores) {
  SmallSet<Value *, 8> WriteTargets;

  for (SmallVectorImpl<Instruction *>::reverse_iterator
           It = BBLoadsAndStores.rbegin(),
           E = BBLoadsAndStores.rend();
       It != E; ++It) {
    Instruction *I = *It;
    unsigned Alignment;
    if (StoreInst *Store = dyn_cast<StoreInst>(I)) {
      Value *Addr = Store->getPointerOperand();
      WriteTargets.insert(Addr);
      CsiLoadStoreProperty Prop;
      // Update alignment property data
      Alignment = Store->getAlignment();
      Prop.setAlignment(Alignment);
      // Set vtable-access property
      Prop.setIsVtableAccess(isVtableAccess(Store));
      // Set constant-data-access property
      Prop.setIsConstant(addrPointsToConstantData(Addr));
      Value *Obj = lookupUnderlyingObject(Addr);
      // Set is-on-stack property
      Prop.setIsOnStack(isa<AllocaInst>(Obj));
      // Set may-be-captured property
      Prop.setMayBeCaptured(isa<GlobalValue>(Obj) ||
                            PointerMayBeCaptured(Addr, true, true));
      // Set is-thread-local property
      Prop.setIsThreadLocal(isThreadLocalObject(Obj));
      LoadAndStoreProperties.push_back(std::make_pair(I, Prop));
    } else {
      LoadInst *Load = cast<LoadInst>(I);
      Value *Addr = Load->getPointerOperand();
      CsiLoadStoreProperty Prop;
      // Update alignment property data
      Alignment = Load->getAlignment();
      Prop.setAlignment(Alignment);
      // Set vtable-access property
      Prop.setIsVtableAccess(isVtableAccess(Load));
      // Set constant-data-access-property
      Prop.setIsConstant(addrPointsToConstantData(Addr));
      Value *Obj = lookupUnderlyingObject(Addr);
      // Set is-on-stack property
      Prop.setIsOnStack(isa<AllocaInst>(Obj));
      // Set may-be-captured property
      Prop.setMayBeCaptured(isa<GlobalValue>(Obj) ||
                            PointerMayBeCaptured(Addr, true, true));
      // Set is-thread-local property
      Prop.setIsThreadLocal(isThreadLocalObject(Obj));
      // Set load-read-before-write-in-bb property
      bool HasBeenSeen = WriteTargets.count(Addr) > 0;
      Prop.setLoadReadBeforeWriteInBB(HasBeenSeen);
      LoadAndStoreProperties.push_back(std::make_pair(I, Prop));
    }
  }
  BBLoadsAndStores.clear();
}

// Update the attributes on the instrumented function that might be invalidated
// by the inserted instrumentation.
void CSIImpl::updateInstrumentedFnAttrs(Function &F) {
  F.removeFnAttr(Attribute::ReadOnly);
  F.removeFnAttr(Attribute::ReadNone);
  F.removeFnAttr(Attribute::ArgMemOnly);
  F.removeFnAttr(Attribute::InaccessibleMemOnly);
  F.removeFnAttr(Attribute::InaccessibleMemOrArgMemOnly);
}

void CSIImpl::instrumentFunction(Function &F) {
  // This is required to prevent instrumenting the call to
  // __csi_module_init from within the module constructor.

  if (F.empty() || shouldNotInstrumentFunction(F) ||
      LinkedFromBitcode.count(&F))
    return;

  if (Options.CallsMayThrow)
    // Promote calls to invokes to insert CSI instrumentation in
    // exception-handling code.
    setupCalls(F);

  const TargetLibraryInfo *TLI = &GetTLI(F);

  DominatorTree *DT = &GetDomTree(F);
  LoopInfo &LI = GetLoopInfo(F);

  // If we do not assume that calls terminate blocks, or if we're not
  // instrumenting basic blocks, then we're done.
  if (Options.InstrumentBasicBlocks && Options.CallsTerminateBlocks)
    splitBlocksAtCalls(F, DT, &LI);

  if (Options.InstrumentLoops)
    // Simplify loops to prepare for loop instrumentation
    for (Loop *L : LI)
      simplifyLoop(L, DT, &LI, nullptr, nullptr, nullptr,
                   /* PreserveLCSSA */ false);

  // Canonicalize the CFG for CSI instrumentation
  setupBlocks(F, TLI, DT, &LI);

  LLVM_DEBUG(dbgs() << "Canonicalized function:\n" << F);

  SmallVector<std::pair<Instruction *, CsiLoadStoreProperty>, 8>
      LoadAndStoreProperties;
  SmallVector<Instruction *, 8> AllocationFnCalls;
  SmallVector<Instruction *, 8> FreeCalls;
  SmallVector<Instruction *, 8> MemIntrinsics;
  SmallVector<Instruction *, 8> Callsites;
  SmallVector<BasicBlock *, 8> BasicBlocks;
  SmallVector<Instruction *, 8> AtomicAccesses;
  SmallVector<DetachInst *, 8> Detaches;
  SmallVector<SyncInst *, 8> Syncs;
  SmallVector<Instruction *, 8> Allocas;
  SmallVector<Instruction *, 8> AllCalls;
  bool MaySpawn = false;
  SmallPtrSet<BasicBlock *, 4> BBsToIgnore;

  TaskInfo &TI = GetTaskInfo(F);
  ScalarEvolution *SE = nullptr;
  if (GetScalarEvolution)
    SE = &(*GetScalarEvolution)(F);

  // Compile lists of all instrumentation points before anything is modified.
  for (BasicBlock &BB : F) {
    // Ignore Tapir placeholder basic blocks
    if (&F.getEntryBlock() != &BB && isTapirPlaceholderSuccessor(&BB))
      continue;
    if (!DT->isReachableFromEntry(&BB))
      continue;
    SmallVector<Instruction *, 8> BBLoadsAndStores;
    for (Instruction &I : BB) {
      if (isAtomic(&I))
        AtomicAccesses.push_back(&I);
      else if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
        BBLoadsAndStores.push_back(&I);
      } else if (DetachInst *DI = dyn_cast<DetachInst>(&I)) {
        MaySpawn = true;
        Detaches.push_back(DI);
      } else if (SyncInst *SI = dyn_cast<SyncInst>(&I)) {
        Syncs.push_back(SI);
        if (isSyncUnwind(SI->getSuccessor(0)->getFirstNonPHIOrDbgOrLifetime(),
                         /*SyncRegion=*/nullptr, /*CheckForInvoke=*/true)) {
          SyncsWithUnwinds.insert(SI);
          BBsToIgnore.insert(SI->getSuccessor(0));
        }
      } else if (isa<CallBase>(I)) {
        // Record this function call as either an allocation function, a call to
        // free (or delete), a memory intrinsic, or an ordinary real function
        // call.
        if (isAllocationFn(&I, TLI))
          AllocationFnCalls.push_back(&I);
        else if (isFreeCall(&I, TLI))
          FreeCalls.push_back(&I);
        else if (isa<MemIntrinsic>(I))
          MemIntrinsics.push_back(&I);
        else if (!callsPlaceholderFunction(I))
          Callsites.push_back(&I);

        AllCalls.push_back(&I);

        computeLoadAndStoreProperties(LoadAndStoreProperties, BBLoadsAndStores);
      } else if (isa<AllocaInst>(I)) {
        Allocas.push_back(&I);
      }
    }
    computeLoadAndStoreProperties(LoadAndStoreProperties, BBLoadsAndStores);
    if (!BBsToIgnore.count(&BB))
      BasicBlocks.push_back(&BB);
  }

  uint64_t LocalId = getLocalFunctionID(F);

  // Instrument basic blocks.  Note that we do this before other instrumentation
  // so that we put this at the beginning of the basic block, and then the
  // function entry call goes before the call to basic block entry.
  if (Options.InstrumentBasicBlocks)
    for (BasicBlock *BB : BasicBlocks)
      instrumentBasicBlock(*BB);

  // Instrument Tapir constructs.
  if (Options.InstrumentTapir) {
    // Allocate a local variable that will keep track of whether
    // a spawn has occurred before a sync. It will be set to 1 after
    // a spawn and reset to 0 after a sync.
    auto TrackVars = keepTrackOfSpawns(F, Detaches, Syncs);

    if (Config->DoesFunctionRequireInstrumentationForPoint(
            F.getName(), InstrumentationPoint::INSTR_TAPIR_DETACH)) {
      for (DetachInst *DI : Detaches)
        instrumentDetach(DI, DT, TI, LI, TrackVars);
    }
    if (Config->DoesFunctionRequireInstrumentationForPoint(
            F.getName(), InstrumentationPoint::INSTR_TAPIR_SYNC)) {
      for (SyncInst *SI : Syncs)
        instrumentSync(SI, TrackVars);
    }
  }

  if (Options.InstrumentLoops)
    // Recursively instrument all loops
    for (Loop *L : LI)
      instrumentLoop(*L, TI, SE);

  // Do this work in a separate loop after copying the iterators so that we
  // aren't modifying the list as we're iterating.
  if (Options.InstrumentMemoryAccesses)
    for (std::pair<Instruction *, CsiLoadStoreProperty> p :
         LoadAndStoreProperties)
      instrumentLoadOrStore(p.first, p.second);

  // Instrument atomic memory accesses in any case (they can be used to
  // implement synchronization).
  if (Options.InstrumentAtomics)
    for (Instruction *I : AtomicAccesses)
      instrumentAtomic(I);

  if (Options.InstrumentMemIntrinsics)
    for (Instruction *I : MemIntrinsics)
      instrumentMemIntrinsic(I);

  if (Options.InstrumentCalls)
    for (Instruction *I : Callsites)
      instrumentCallsite(I, DT);

  if (Options.InstrumentAllocas)
    for (Instruction *I : Allocas)
      instrumentAlloca(I);

  if (Options.InstrumentAllocFns) {
    for (Instruction *I : AllocationFnCalls)
      instrumentAllocFn(I, DT, TLI);
    for (Instruction *I : FreeCalls)
      instrumentFree(I, TLI);
  }

  if (Options.Interpose && Config->DoesAnyFunctionRequireInterposition()) {
    for (Instruction *I : AllCalls)
      interposeCall(I);
  }

  // Instrument function entry/exit points.
  if (Options.InstrumentFuncEntryExit) {
    IRBuilder<> IRB(&*F.getEntryBlock().getFirstInsertionPt());
    Value *FuncId = FunctionFED.localToGlobalId(LocalId, IRB);
    if (Config->DoesFunctionRequireInstrumentationForPoint(
            F.getName(), InstrumentationPoint::INSTR_FUNCTION_ENTRY)) {
      CsiFuncProperty FuncEntryProp;
      FuncEntryProp.setMaySpawn(MaySpawn);
      Value *PropVal = FuncEntryProp.getValue(IRB);
      insertHookCall(&*IRB.GetInsertPoint(), CsiFuncEntry, {FuncId, PropVal});
    }
    if (Config->DoesFunctionRequireInstrumentationForPoint(
            F.getName(), InstrumentationPoint::INSTR_FUNCTION_EXIT)) {
      EscapeEnumerator EE(F, "csi.cleanup", false);
      while (IRBuilder<> *AtExit = EE.Next()) {
        // uint64_t ExitLocalId = FunctionExitFED.add(F);
        uint64_t ExitLocalId = FunctionExitFED.add(*AtExit->GetInsertPoint());
        Value *ExitCsiId =
            FunctionExitFED.localToGlobalId(ExitLocalId, *AtExit);
        CsiFuncExitProperty FuncExitProp;
        FuncExitProp.setMaySpawn(MaySpawn);
        FuncExitProp.setEHReturn(isa<ResumeInst>(AtExit->GetInsertPoint()));
        Value *PropVal = FuncExitProp.getValue(*AtExit);
        insertHookCall(&*AtExit->GetInsertPoint(), CsiFuncExit,
                       {ExitCsiId, FuncId, PropVal});
      }
    }
  }

  updateInstrumentedFnAttrs(F);
}

DenseMap<Value *, Value *>
llvm::CSIImpl::keepTrackOfSpawns(Function &F,
                                 const SmallVectorImpl<DetachInst *> &Detaches,
                                 const SmallVectorImpl<SyncInst *> &Syncs) {

  DenseMap<Value *, Value *> TrackVars;

  SmallPtrSet<Value *, 8> Regions;
  for (auto &Detach : Detaches) {
    Regions.insert(Detach->getSyncRegion());
  }
  for (auto &Sync : Syncs) {
    Regions.insert(Sync->getSyncRegion());
  }

  LLVMContext &C = F.getContext();

  IRBuilder<> Builder{&F.getEntryBlock(),
                      F.getEntryBlock().getFirstInsertionPt()};

  size_t RegionIndex = 0;
  for (auto Region : Regions) {
    Value *TrackVar = Builder.CreateAlloca(IntegerType::getInt32Ty(C), nullptr,
                                           "has_spawned_region_" +
                                               std::to_string(RegionIndex));
    Builder.CreateStore(
        Constant::getIntegerValue(IntegerType::getInt32Ty(C), APInt(32, 0)),
        TrackVar);

    TrackVars.insert({Region, TrackVar});
    RegionIndex++;
  }

  return TrackVars;
}

Function *CSIImpl::getInterpositionFunction(Function *F) {
  if (InterpositionFunctions.find(F) != InterpositionFunctions.end())
    return InterpositionFunctions.lookup(F);

  std::string InterposedName = "__csi_interpose_" + F->getName().str();
  Function *InterpositionFunction = cast<Function>(
      M.getOrInsertFunction(InterposedName, F->getFunctionType()).getCallee());

  InterpositionFunctions.insert({F, InterpositionFunction});

  return InterpositionFunction;
}

void ComprehensiveStaticInstrumentationLegacyPass::getAnalysisUsage(
    AnalysisUsage &AU) const {
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<TaskInfoWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<TargetTransformInfoWrapperPass>();
}

bool ComprehensiveStaticInstrumentationLegacyPass::runOnModule(Module &M) {
  if (skipModule(M))
    return false;

  CallGraph *CG = &getAnalysis<CallGraphWrapperPass>().getCallGraph();
  auto GetTLI = [this](Function &F) -> TargetLibraryInfo & {
    return this->getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  };
  auto GetDomTree = [this](Function &F) -> DominatorTree & {
    return this->getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
  };
  auto GetLoopInfo = [this](Function &F) -> LoopInfo & {
    return this->getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
  };
  auto GetTTI = [this](Function &F) -> TargetTransformInfo & {
    return this->getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
  };
  auto GetSE = [this](Function &F) -> ScalarEvolution & {
    return this->getAnalysis<ScalarEvolutionWrapperPass>(F).getSE();
  };
  auto GetTaskInfo = [this](Function &F) -> TaskInfo & {
    return this->getAnalysis<TaskInfoWrapperPass>(F).getTaskInfo();
  };

  bool res = CSIImpl(M, CG, GetDomTree, GetLoopInfo, GetTaskInfo, GetTLI, GetSE,
                     GetTTI, Options)
                 .run();

  verifyModule(M, &llvm::errs());

  numPassRuns++;

  return res;
}

CSISetupPass::CSISetupPass() : Options(OverrideFromCL(CSIOptions())) {}

CSISetupPass::CSISetupPass(const CSIOptions &Options) : Options(Options) {}

PreservedAnalyses CSISetupPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (!CSISetupImpl(M, Options).run())
    return PreservedAnalyses::all();

  return PreservedAnalyses::none();
}

ComprehensiveStaticInstrumentationPass::ComprehensiveStaticInstrumentationPass()
    : Options(OverrideFromCL(CSIOptions())) {}

ComprehensiveStaticInstrumentationPass::ComprehensiveStaticInstrumentationPass(
    const CSIOptions &Options)
    : Options(Options) {}

PreservedAnalyses
ComprehensiveStaticInstrumentationPass::run(Module &M,
                                            ModuleAnalysisManager &AM) {
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  auto &CG = AM.getResult<CallGraphAnalysis>(M);
  auto GetDT = [&FAM](Function &F) -> DominatorTree & {
    return FAM.getResult<DominatorTreeAnalysis>(F);
  };
  auto GetLI = [&FAM](Function &F) -> LoopInfo & {
    return FAM.getResult<LoopAnalysis>(F);
  };
  auto GetTTI = [&FAM](Function &F) -> TargetTransformInfo & {
    return FAM.getResult<TargetIRAnalysis>(F);
  };
  auto GetSE = [&FAM](Function &F) -> ScalarEvolution & {
    return FAM.getResult<ScalarEvolutionAnalysis>(F);
  };
  auto GetTI = [&FAM](Function &F) -> TaskInfo & {
    return FAM.getResult<TaskAnalysis>(F);
  };
  auto GetTLI = [&FAM](Function &F) -> TargetLibraryInfo & {
    return FAM.getResult<TargetLibraryAnalysis>(F);
  };

  // Disable additional conversion of calls to invokes.
  Options.CallsMayThrow = false;

  if (!CSIImpl(M, &CG, GetDT, GetLI, GetTI, GetTLI, GetSE, GetTTI, Options)
           .run())
    return PreservedAnalyses::all();

  return PreservedAnalyses::none();
}
