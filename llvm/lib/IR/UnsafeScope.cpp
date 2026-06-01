#include "llvm/IR/UnsafeScope.h"
#include "LLVMContextImpl.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/PatternMatch.h"
#include <iterator>
#include <cassert>
#include <optional>

using namespace llvm;

namespace {

struct MyBasicBlockEdge {
  BasicBlock *Src = nullptr;
  BasicBlock *Dest = nullptr;

  MyBasicBlockEdge(BasicBlock *Src, BasicBlock *Dest) : Src(Src), Dest(Dest) {}

  BasicBlock *getStart() const { return Src; }
  BasicBlock *getEnd() const { return Dest; }
};

} // namespace

UnsafeBuilderScope::UnsafeBuilderScope(IRBuilderBase &Builder, MDNode *MD)
    : Builder(Builder) {
  Builder.SetUnsafeMetadata(MD);
}

UnsafeBuilderScope::~UnsafeBuilderScope() {
  Builder.SetUnsafeMetadata(nullptr);
}

static void appendSuccessors(Instruction *I,
                            SmallVectorImpl<Instruction *> &Out) {
  if (Instruction *Succ = I->getNextNode()) {
    Out.push_back(Succ);
  } else {
    for (auto *Succ : llvm::successors(I->getParent()))
      Out.push_back(&Succ->front());
  }
}

static void appendPredecessors(Instruction *I,
                               SmallVectorImpl<Instruction *> &Out) {
  if (Instruction *Pred = I->getPrevNode()) {
    Out.push_back(Pred);
  } else {
    for (auto *Pred : llvm::predecessors(I->getParent()))
      Out.push_back(&Pred->back());
  }
}

static void sortUniqueMetadata(SmallVectorImpl<Metadata *> &MDs) {
  llvm::sort(MDs, [&](auto *A, auto *B) { return A->getMetadataID() < B->getMetadataID(); });
  MDs.erase(llvm::unique(MDs), MDs.end());
}

bool unsafe::isValidScope(Metadata *ID) {
  auto *IDT = dyn_cast<MDTuple>(ID);
  if (!IDT)
    return false;
  if (IDT->getNumOperands() != 1)
    return false;
  Metadata *Child = IDT->getOperand(0);
  if (IDT != Child)
    return false;
  return true;
}

MDNode *unsafe::mergeMetadata(MDNode *A, MDNode *B) {
  if (!A)
    return B;
  if (!B)
    return A;
  SmallVector<Metadata *> MDs;
  llvm::copy(A->operands(), std::back_inserter(MDs));
  llvm::copy(B->operands(), std::back_inserter(MDs));
  sortUniqueMetadata(MDs);
  auto &Ctx = A ? A->getContext() : B->getContext();
  return MDTuple::get(Ctx, MDs);
}

MDNode *unsafe::removeID(MDNode *MD, uint64_t ID) {
  if (!MD || !MD->getNumOperands())
    return nullptr;
  SmallVector<Metadata *> MDs;
  llvm::copy(MD->operands(), std::back_inserter(MDs));
  sortUniqueMetadata(MDs);
  auto &Ctx = MD->getContext();
  return MDTuple::get(Ctx, MDs);
}

bool llvm::isSpeculationPrimitive(const Instruction &I) {
  // If register-only, never speculation primitive.
  if (isa<BinaryOperator, UnaryOperator, CastInst, CmpInst>(&I))
    return false;

  // Check if it's an intrinsic that may write memory.
  if (auto *II = dyn_cast<IntrinsicInst>(&I);
      II && II->mayWriteToMemory() && !II->isDebugOrPseudoInst() &&
      !II->isAssumeLikeIntrinsic() && !II->onlyAccessesInaccessibleMemory()) {
    return true;
  }
  
  switch (I.getOpcode()) {
  case Instruction::Br:
    return cast<BranchInst>(I).isConditional();

  case Instruction::Switch:
  case Instruction::IndirectBr:
  case Instruction::Store:
    return true;

  case Instruction::CallBr:
    return cast<CallBrInst>(I).getNumIndirectDests() > 0;

  case Instruction::Call:
  case Instruction::Invoke:
    return cast<CallBase>(&I)->isIndirectCall();

  case Instruction::Unreachable:
  case Instruction::Ret:
  case Instruction::Load:
  case Instruction::GetElementPtr:
  case Instruction::Alloca:
  case Instruction::Freeze:
  case Instruction::ExtractValue:
  case Instruction::VAArg:
  case Instruction::LandingPad:
  case Instruction::InsertElement:
  case Instruction::InsertValue:
  case Instruction::PHI:
  case Instruction::Resume: // TODO: Is this actually possibly a speculation
                            // primitive, since it might compile down to an
                            // indirect branch? Could always disable speculation
                            // for the branch, since this should be very rare.
  case Instruction::AtomicCmpXchg:
  case Instruction::AtomicRMW:
  case Instruction::ExtractElement:
  case Instruction::Select:
  case Instruction::Fence:
  case Instruction::ShuffleVector:
    return false;

  default: {
    auto Msg = formatv("{}: unhandled instruction: {}", __func__, I);
    report_fatal_error(Msg);
  }
  }
}

Value *llvm::getTransmittedOperand(Instruction &I) {
  // TODO: Should do switch on opcode?
  if (auto *BI = dyn_cast<BranchInst>(&I))
    return BI->isConditional() ? BI->getCondition() : nullptr;

  if (auto *Indbr = dyn_cast<IndirectBrInst>(&I))
    return Indbr->getAddress();

  if (auto *CB = dyn_cast<CallBase>(&I))
    return CB->getCalledOperand();

  if (auto *Ptr = getLoadStorePointerOperand(&I))
    return Ptr;

  return nullptr;
}

const Value *llvm::getTransmittedOperand(const Instruction &I) {
  return const_cast<const Value *>(getTransmittedOperand(const_cast<Instruction &>(I)));
}

static bool isValueConstantPHI(const Value *V) {
  SmallVector<const Value *> Todo;
  SmallSet<const Value *, 4> Seen;
  Todo.push_back(V);
  while (!Todo.empty()) {
    auto *V = Todo.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    if (isa<Constant, BasicBlock, InlineAsm>(V))
      continue;
    auto *Phi = dyn_cast<PHINode>(V);
    if (!Phi)
      return false;
    llvm::copy(Phi->incoming_values(), std::back_inserter(Todo));
  }
  return true;
}

bool llvm::isTransmitter(const Instruction &I) {
#if 0
  auto *V = getTransmittedOperand(I);
  return V && !isValueConstantPHI(V);
#else
  auto *V = getTransmittedOperand(I);
  return isa_and_nonnull<Argument, Instruction>(V);
#endif
}

static Metadata *getUnsafeMDElt(int ID, LLVMContext &Ctx) {
  auto *Int32Ty = IntegerType::getInt32Ty(Ctx);
  auto *C = ConstantInt::get(Int32Ty, ID);
  auto *CAM = ConstantAsMetadata::get(C);
  return CAM;
}

// TODO: This and its users are ridiculously redundant. Refactor.
static void enumerateDepsImpl(SmallVectorImpl<const Value *> &Todo, LLVMContext &Ctx, SmallVectorImpl<Metadata *> &IDs) {
  SmallSet<const Value *, 4> Seen;
  while (!Todo.empty()) {
    auto *V = Todo.pop_back_val();

    if (!Seen.insert(V).second)
      continue;

    // Handle arguments by just returning an abstract unsafe scope
    // with an ID that's just its negative index.
    if (auto *A = dyn_cast<Argument>(V)) {
      auto ID = -static_cast<int>(A->getArgNo());
      IDs.push_back(getUnsafeMDElt(ID, Ctx));
      continue;
    }

    // Handle instructions.
    if (auto *I = dyn_cast<Instruction>(V)) {
      // If the instruction is part of an unsafe scope, then add that and stop
      // exploring.
      if (auto *MD = I->getMetadata(LLVMContext::MD_unsafe)) {
        llvm::copy(MD->operands(), std::back_inserter(IDs));
        continue;
      }

      // Otherwise, add all operands.
      llvm::copy(I->operands(), std::back_inserter(Todo));
      continue;
    }
  }
}

// TODO: Is this dead code?
MDNode *unsafe::enumerateTransmitterDeps(const Instruction &I) {
  LLVMContext &Ctx = I.getContext();
  SmallVector<Metadata *> IDs;

  SmallVector<const Value *> Todo;
  llvm::copy(I.operands(), std::back_inserter(Todo));
  enumerateDepsImpl(Todo, Ctx, IDs);

  if (auto *MD = I.getMetadata(LLVMContext::MD_unsafe))
    llvm::copy(MD->operands(), std::back_inserter(IDs));

  sortUniqueMetadata(IDs);
  return MDNode::get(Ctx, IDs);
}

MDNode *unsafe::enumerateDeps(const Value *V) {
  LLVMContext &Ctx = V->getContext();
  SmallVector<Metadata *> IDs;
  SmallVector<const Value *> Todo;
  Todo.push_back(V);
  enumerateDepsImpl(Todo, Ctx, IDs);
  sortUniqueMetadata(IDs);
  return MDNode::get(Ctx, IDs);
}

MDNode *unsafe::enumerateScopeDeps(int ID, const Function &F) {
  LLVMContext &Ctx = F.getContext();

  // Get a `Metadata *` for the ID, so we can do quick pointer comparisons.
  Metadata *IDMD = getUnsafeMDElt(ID, Ctx);

  // Collect all instructions with !unsafe tag containing ID.
  SmallVector<const Value *> Todo;
  SmallVector<Metadata *> IDs;
  for (const Instruction &I : llvm::instructions(F)) {
    auto *MD = I.getMetadata(LLVMContext::MD_unsafe);
    if (!MD || !llvm::is_contained(MD->operands(), IDMD))
      continue;
    llvm::copy(I.operands(), std::back_inserter(Todo));
  }
  enumerateDepsImpl(Todo, Ctx, IDs);

  sortUniqueMetadata(IDs);
  return MDNode::get(Ctx, IDs);
}

static MDNode *blockMetadata(const BasicBlock &B) {
  auto &Ctx = B.getContext();
  MDNode *MD = MDNode::get(Ctx, {});
  for (const Instruction &I : B)
    MD = unsafe::mergeMetadata(MD, I.getMetadata(LLVMContext::MD_unsafe));
  return MD->getNumOperands() ? MD : nullptr;
}

MDNode *unsafe::loopMetadata(const Loop &L) {
  auto &Ctx = (*L.block_begin())->getContext();
  MDNode *MD = MDNode::get(Ctx, {});
  for (auto *B : L.getBlocks())
    MD = mergeMetadata(MD, blockMetadata(*B));
  return MD->getNumOperands() ? MD : nullptr;
}

bool unsafe::isRelevantCall(const Instruction &I) {
  if (!isa<CallBase>(&I))
    return false;
  auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return true;
  if (II->isDebugOrPseudoInst() || II->isAssumeLikeIntrinsic())
    return false;
  switch (II->getIntrinsicID()) {
  case Intrinsic::unsafe_shadow_dep:
  case Intrinsic::unsafe_shadow_xmit:
  case Intrinsic::unsafe_shadow_arg:
  case Intrinsic::unsafe_shadow_embed:
  case Intrinsic::unsafe_begin:
  case Intrinsic::unsafe_end:
    return false;
    // We should mark llvm.unsafe.shadow.gen with the !unsafe scope it belongs to.
  case Intrinsic::unsafe_shadow_gen:
  case Intrinsic::unsafe_shadow_tie:
    return true;
  }
  if (!II->mayHaveSideEffects() && !II->mayReadOrWriteMemory())
    return false;
  return true;
}

unsigned unsafe::getNumDepOperands(const Instruction &I) {
  assert(isTransmitter(I) || isRelevantCall(I));

  // Handle calls.
  if (auto *C = dyn_cast<CallBase>(&I))
    return C->arg_size() + 1;

  return I.getNumOperands();
}

Value *unsafe::getDepOperand(Instruction &I, unsigned Idx) {
  assert(isTransmitter(I) || isRelevantCall(I));
  assert(Idx < getNumDepOperands(I));

  // Handle calls.
  if (auto *C = dyn_cast<CallBase>(&I)) {
    // First operand is the called operand.
    if (Idx == 0)
      return C->getCalledOperand();
    // Followed by arguments.
    return C->getArgOperand(Idx - 1);
  }

  return I.getOperand(Idx);
}

MDNode *unsafe::addIDToSet(MDNode *Set, Metadata *ID) {
  auto &Ctx = Set->getContext();
  return mergeMetadata(Set, MDNode::get(Ctx, {ID}));
}

void unsafe::addIDToMetadataSetInPlace(Instruction &I, StringRef Key,
                                       Metadata *ID) {
  auto *Set = I.getMetadata(Key);
  Set = addIDToSet(Set, ID);
  I.setMetadata(Key, Set);
}

bool unsafe::hasID(const Instruction &I, Metadata *ID) {
  auto *IDs = I.getMetadata(LLVMContext::MD_unsafe);
  return IDs && is_contained(IDs->operands(), ID);
}

DenseMap<Instruction *, SmallSet<Value *, 4>> UnsafeScope::computeConsumptionFrontier() {
  using ValSet = SmallSet<Value *, 4>;
  struct InOut {
    ValSet In;
    ValSet Out;
  };
  auto Snapshot = [](const InOut &Set) {
    return Set.In.size() + Set.Out.size();
  };
  DenseMap<Instruction *, InOut> Map;
  bool Changed;
  do {
    Changed = false;

    // Do data-flow in postorder, since this is a backwards
    // data-flow.
    for (Instruction *I : reverse(instructions())) {
      // Ignore PHIs.
      if (isa<PHINode>(I))
        continue;

      // Compute OUT from successors' INs.
      auto &X = Map[I];
      auto Orig = Snapshot(X);
      if (auto *Succ = I->getNextNode()) {
        X.Out = Map[Succ].In;
      } else {
        // This is a termiantor.
        // We need to compute the IN from each block along PHIs.
        auto *B = I->getParent();
        for (auto *Succ : successors(B)) {
          auto Set = Map[&*Succ->getFirstNonPHIIt()].In;
          auto Range = llvm::reverse(
              llvm::make_range(Succ->begin(), Succ->getFirstNonPHIIt()));
          for (auto &SuccI : Range) {
            auto *SuccPHI = cast<PHINode>(&SuccI);
            auto *V = SuccPHI->getIncomingValueForBlock(B);
            if (Set.erase(SuccPHI))
              Set.insert(V);
          }
          llvm::copy(Set, std::inserter(X.Out, X.Out.end()));
        }
      }

      // Transfer back across instruction.
      X.In = X.Out;
      X.In.erase(I); // Remove any def.
                     // Add any uses.
      copy(I->operands(), std::inserter(X.In, X.In.end()));

      auto New = Snapshot(X);
      Changed |= (Orig != New);
    }
  } while (Changed);

  DenseMap<Instruction *, SmallSet<Value *, 4>> Result;
  for (auto &[I, Set] : Map)
    if (DomSet.contains(I))
      Result[I] = Set.In;
  return Result;
}

template <typename OutputIt>
void UnsafeScope::getScopeDomSet(Function &F, Metadata *ID, OutputIt Out) {
  SmallVector<Instruction *> Todo;
  SmallSet<Instruction *, 4> Seen;
  Todo.push_back(&F.getEntryBlock().front());
  auto HasID = [ID](const Instruction *I) -> bool {
    return unsafe::hasID(*I, ID);
  };
  while (!Todo.empty()) {
    auto *I = Todo.pop_back_val();
    if (!Seen.insert(I).second)
      continue;
    if (HasID(I))
      continue;
    if (auto *Succ = I->getNextNode()) {
      Todo.push_back(Succ);
      continue;
    }
    for (auto *Succ : successors(I->getParent()))
      Todo.push_back(&Succ->front());
  }
  copy_if(Seen, Out, HasID);
}

void UnsafeScope::computeScopeSet(Function &F, Metadata *ID) {
  // First, compute whichID-unsafe are reachable from the function entry.
  getScopeDomSet(F, ID, std::inserter(DomSet, DomSet.end()));

  // Now, compute which instructions may reach an interior ID-unsafe
  // instruction.
  SmallVector<Instruction *> Todo;
  SmallSet<Instruction *, 8> Seen;
  for (Instruction &I : llvm::instructions(F))
    if (unsafe::hasID(I, ID))
      Todo.push_back(&I);
  while (!Todo.empty()) {
    auto *I = Todo.pop_back_val();
    if (!Seen.insert(I).second)
      continue;
    if (DomSet.contains(I))
      continue;
    if (auto *Pred = I->getPrevNode()) {
      Todo.push_back(Pred);
      continue;
    }
    for (auto *Pred : predecessors(I->getParent()))
      Todo.push_back(&Pred->back());
  }

  // Copy scope instructions to scope vector in reverse postorder.
  for (auto *B : ReversePostOrderTraversal(&F))
    for (auto &I : *B)
      if (Seen.contains(&I))
        Scope.push_back(&I);
  assert(Seen.size() >= Scope.size()); // RPO doesn't include unreachable blocks...
}

static Instruction *promoteSafeCallToUnsafeFnToUnsafeCall(CallBase &C) {
  using namespace unsafe;
  using namespace llvm::PatternMatch;
  
  assert(C.hasFnAttr(Attribute::Unsafe));
  assert(C.getMetadata(LLVMContext::MD_safe));
  assert(!C.getMetadata(LLVMContext::MD_unsafe));

  auto &Ctx = C.getContext();

  // Mint a new unsafe scope.
  auto *ID = createNewScope(Ctx);
  auto *IDs = getScopeSet(Ctx, {ID});

  // Update call's metadata.
  C.setMetadata(LLVMContext::MD_safe, nullptr);
  C.setMetadata(LLVMContext::MD_unsafe, IDs);

  // Insert @llvm.unsafe.shadow.gen before the call.
  IRBuilder<> IRB(&C);
  auto *Gen = createGenIntrinsic(IRB, ID);

  // Insert @llvm.unsafe.shadow.tie for each argument.
  for (Value *ArgV : C.args()) {
    // Does this have a @llvm.unsafe.shadow.embed(%arg, %dep)?
    // If so, convert to tie.
    // If not, then there was no dep.
    auto *ArgI = dyn_cast<Instruction>(ArgV);
    if (!ArgI)
      continue;
    Value *OrigArg = nullptr;
    Value *Dep = nullptr;
    if (!match(ArgI, m_Intrinsic<Intrinsic::unsafe_shadow_embed>(
                         m_Value(OrigArg), m_Value(Dep))))
      continue;
    
    ArgI->replaceAllUsesWith(OrigArg);
    ArgI->eraseFromParent();
    unsafe::createShadowTie(&C, Gen, Dep, IDs);
  }

  // Replace uses of any trailing !@*.arg with Gen,.
  if (!C.getType()->isVoidTy())
    for (User *U : C.users())
      if (match(U, m_Intrinsic<Intrinsic::unsafe_shadow_arg>()))
        U->replaceAllUsesWith(Gen);

  return Gen;
}

bool GenAnalysisImpl::queryGen(Value *V) {
  using namespace llvm::PatternMatch;

  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return false;

  if (match(I, m_Intrinsic<Intrinsic::unsafe_shadow_gen>(m_Specific(IDVal))))
    return true;

  auto *PHI = dyn_cast<PHINode>(I);
  if (!PHI)
    return false;

  // Have we cached this PHI node?
  auto Result = Cache.insert({PHI, true});
  bool &IsPHIGen = Result.first->second;
  if (!Result.second)
    return IsPHIGen;

  // If not, recompute.
  IsPHIGen = all_of(PHI->incoming_values(), [&](Value *Incoming) {
    return queryGen(Incoming);
  });

  return IsPHIGen;
}

Metadata *unsafe::getUniqueScope(Instruction &I) {
  auto *IDs = I.getMetadata(LLVMContext::MD_unsafe);
  assert(IDs->getNumOperands() == 1);
  Metadata *ID = IDs->getOperand(0);
  assert(isValidScope(ID));
  return ID;
}


Instruction *unsafe::normalizeCallSiteBeforeInliningIntoSafeFn(CallBase &C) {
  Function &F = *C.getFunction();
  assert(!F.hasFnAttribute(Attribute::Unsafe));
  auto &Ctx = F.getContext();

  bool SafeCall = C.getMetadata(LLVMContext::MD_safe);
  bool UnsafeCallee = C.hasFnAttr(Attribute::Unsafe);
  if (SafeCall && !UnsafeCallee)
    return nullptr;

  // Promote safe call to unsafe fn to unsafe call.
  if (SafeCall && C.hasFnAttr(Attribute::Unsafe))
    return promoteSafeCallToUnsafeFnToUnsafeCall(C);

  // Find the gen that reaches the unsafe call.
  auto *ID = getUniqueScope(C);
  IRBuilder<> IRB(&C);
  return createGenIntrinsic(IRB, ID);
}

CallInst *unsafe::createShadowTie(IRBuilderBase &IRB, Value *Gen, Value *Dep,
                                 MDNode *UnsafeIDs) {
  assert(UnsafeIDs && "UnsafeIDs was null!");
  auto *Tie = IRB.CreateIntrinsic(Intrinsic::unsafe_shadow_tie, {Gen, Dep});
  Tie->setMetadata(LLVMContext::MD_unsafe, UnsafeIDs);
  return Tie;
}

CallInst *unsafe::createShadowTie(Instruction *At, Value *Gen, Value *Dep,
                                 MDNode *UnsafeIDs) {
  IRBuilder<> IRB(At);
  return createShadowTie(IRB, Gen, Dep, UnsafeIDs);
}

void unsafe::tieLoadResult(Instruction &I, Value *Gen, MDNode *UnsafeIDs) {
  assert(isa<LoadInst>(&I));
  IRBuilder<> IRB(I.getNextNode());
  auto *Dep = IRB.CreateUnaryIntrinsic(Intrinsic::unsafe_shadow_arg, &I);
  createShadowTie(IRB, Gen, Dep, UnsafeIDs);
}

/// Returns whether the instruction was erased.
static bool removeAllScopeInfo(Instruction &I) {
  auto &Ctx = I.getContext();

  if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
    switch (II->getIntrinsicID()) {
    case Intrinsic::unsafe_shadow_gen:
    case Intrinsic::unsafe_shadow_dep:
    case Intrinsic::unsafe_shadow_arg:
      I.replaceAllUsesWith(ConstantInt::get(IntegerType::get(Ctx, 32), 0));
      I.eraseFromParent();
      return true;

    case Intrinsic::unsafe_shadow_tie:
    case Intrinsic::unsafe_shadow_xmit:
      I.eraseFromParent();
      return true;

    case Intrinsic::unsafe_shadow_embed:
      I.replaceAllUsesWith(II->getArgOperand(0));
      I.eraseFromParent();
      return true;
    }
  }

  // Strip any !unsafe/!safe metadata.
  I.setMetadata(LLVMContext::MD_unsafe, nullptr);
  I.setMetadata(LLVMContext::MD_safe, nullptr);

  return false;
}

static void postProcessInstructionInlinedFromUnsafeCall(Instruction &I,
                                                        MDNode *IDs,
                                                        Instruction *Gen) {
  auto &Ctx = I.getContext();

  // Strip all scope information.
  if (removeAllScopeInfo(I))
    return;

  // Stamp speculation primitives, calls, and transmitters.
  if (isSpeculationPrimitive(I) || isTransmitter(I) ||
      unsafe::isRelevantCall(I)) {
    I.setMetadata(LLVMContext::MD_unsafe, IDs);
  } else {
    I.setMetadata(LLVMContext::MD_safe, MDNode::get(Ctx, {}));
  }

  // Extract and tag load dependencies.
  if (isa<LoadInst>(&I))
    unsafe::tieLoadResult(I, Gen, IDs);
}

unsafe::InliningCookie unsafe::prepareCallSiteForInlining(CallBase &C) {
  auto &F = *C.getFunction();

  // If the caller is unsafe, then there's nothing to do.
  if (F.hasFnAttribute(Attribute::Unsafe))
    return {};

  InliningCookie Info;
  Info.Gen = normalizeCallSiteBeforeInliningIntoSafeFn(C);
  if (Info.Gen) {
    auto *IDs = C.getMetadata(LLVMContext::MD_unsafe);
    assert(IDs);
    assert(IDs->getNumOperands() == 1);
    Info.ID = IDs->getOperand(0);
  }

  return Info;
}

void unsafe::postProcessInlinedBlocks(iterator_range<Function::iterator> Blocks,
                                      InliningCookie Info) {
  using namespace llvm::PatternMatch;
  assert(!Blocks.empty());
  auto &F = *Blocks.begin()->getParent();
  auto &Ctx = F.getContext();

  // If this was an unsafe-attr function, just strip stuff out.
  if (F.hasFnAttribute(Attribute::Unsafe)) {
    for (auto &B : Blocks)
      for (auto &I : make_early_inc_range(B))
        removeAllScopeInfo(I);
    return;
  }

  // Otherwise, it's a safe-attr function.
  // If the call was !unsafe, then strip all scope info and then stamp.
  if (Info.Gen) {
    for (auto &B : Blocks)
      for (auto &I : make_early_inc_range(B))
        postProcessInstructionInlinedFromUnsafeCall(
            I, Info.Gen->getMetadata(LLVMContext::MD_unsafe), Info.Gen);
    return;
  }

  // The call was !safe.
  // We need to duplicate all scopes.
  SmallDenseMap<Metadata *, Metadata *> Map;
  auto GetNewID = [&](Metadata *OldID) {
    auto &NewID = Map[OldID];
    if (!NewID)
      NewID = createNewScope(Ctx);
    return NewID;
  };
  SmallDenseMap<Metadata *, Metadata *> RemapXmits;
  auto GetNewXmit = [&](Metadata *OldID) {
    auto &NewID = RemapXmits[OldID];
    if (!NewID)
      NewID = createNewXmit(Ctx);
    return NewID;
  };
  // TODO: Would be nice to refactor with replacement function.
  for (auto &B : Blocks)
    for (auto &I : B) {
      if (auto *OrigIDs = I.getMetadata(LLVMContext::MD_unsafe)) {
        SmallVector<Metadata *> NewIDVec;
        transform(OrigIDs->operands(), std::back_inserter(NewIDVec), GetNewID);
        I.setMetadata(LLVMContext::MD_unsafe, getScopeSet(Ctx, NewIDVec));
      }

      if (auto *II = dyn_cast<IntrinsicInst>(&I);
          II && II->getIntrinsicID() == Intrinsic::unsafe_shadow_gen) {
        auto *OldArg = II->getArgOperand(0);
        auto *OldID = cast<MetadataAsValue>(OldArg)->getMetadata();
        auto *NewID = GetNewID(OldID);
        auto *NewArg = MetadataAsValue::get(Ctx, NewID);
        II->setArgOperand(0, NewArg);
      }

      if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_xmit>())) {
        // xmit intrinsic: MD_soundspec_xmit is a *tuple* of distinct
        // self-referencing xmit IDs. Remap each operand through
        // GetNewXmit and re-wrap as a tuple via createNewXmitSet.
        if (auto *OrigIDs = I.getMetadata(LLVMContext::MD_soundspec_xmit)) {
          SmallVector<Metadata *> NewIDVec;
          transform(OrigIDs->operands(), std::back_inserter(NewIDVec),
                    GetNewXmit);
          I.setMetadata(LLVMContext::MD_soundspec_xmit,
                        createNewXmitSet(Ctx, NewIDVec));
        }
      } else if (auto *OrigID = I.getMetadata(LLVMContext::MD_soundspec_xmit)) {
        // Transmitter: MD_soundspec_xmit is the single self-referencing
        // xmit ID directly (verifier invariant:
        // XmitID->getOperand(0) == XmitID). Remap and stamp it without
        // wrapping in an outer tuple.
        I.setMetadata(LLVMContext::MD_soundspec_xmit,
                      cast<MDNode>(GetNewXmit(OrigID)));
      }
    }
}

void unsafe::setUnsafeScope(Instruction &I, MDNode *ID) {
  I.setMetadata(LLVMContext::MD_unsafe, ID);
  if (auto *II = dyn_cast<IntrinsicInst>(&I);
      II && II->getIntrinsicID() == Intrinsic::unsafe_shadow_gen)
    II->setArgOperand(
        0, MetadataAsValue::get(I.getContext(), ID->getOperand(0)));
}

void UnsafeScope::print(raw_ostream &OS) {
  OS << "=== Scope " << *ID << " in " << F.getName() << " ===\n";
  for (auto *I : Scope) {
    StringRef Prefix = " ";
    if (DomSet.contains(I))
      Prefix = "D";
    OS << Prefix << " " << *I << "\n";
  }
}

void UnsafeScope::dump() {
  print(errs());
}

MDNode *unsafe::createNewScope(LLVMContext &Ctx) {
  auto Tmp = MDNode::getTemporary(Ctx, {});
  auto *Scope = MDTuple::getDistinct(Ctx, {Tmp.get()});
  Scope->replaceOperandWith(0, Scope);
  return Scope;
}

MDNode *unsafe::getScopeSet(LLVMContext &Ctx, ArrayRef<Metadata *> IDs) {
  return MDNode::get(Ctx, IDs);
}

MDNode *unsafe::createNewXmit(LLVMContext &Ctx) {
  // For now, createNewScope() does exactly what we need.
  return createNewScope(Ctx);
}

MDNode *unsafe::createNewXmitSet(LLVMContext &Ctx, ArrayRef<Metadata *> Xmits) {
  return getScopeSet(Ctx, Xmits);
}


static MDNode *replaceUnsafeScopeInList(LLVMContext &Ctx, MDNode *List,
                                        Metadata *Old, Metadata *New) {
  if (!List || !is_contained(List->operands(), Old))
    return List;
  SmallVector<Metadata *> IDs;
  for (Metadata *ID : List->operands())
    IDs.push_back(ID == Old ? New : Old);
  return MDNode::get(Ctx, IDs);
}

static void replaceUnsafeScopeForShadowIntrinsic(Instruction &I, Metadata *Old,
                                                 Metadata *New) {
  auto &Ctx = I.getContext();
  auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return;
  switch (II->getIntrinsicID()) {
  case Intrinsic::unsafe_shadow_gen: {
    auto *ID = cast<MetadataAsValue>(II->getArgOperand(0))->getMetadata();
    if (ID == Old)
      II->setArgOperand(0, MetadataAsValue::get(Ctx, New));
    return;
  }
  }
}

void llvm::replaceUnsafeScopeWith(Function &F, Metadata *Old, Metadata *New) {
  assert(unsafe::isValidScope(Old));
  assert(unsafe::isValidScope(New));
  auto &Ctx = F.getContext();
  for (auto &I : instructions(F)) {
    auto *OldIDs = I.getMetadata(LLVMContext::MD_unsafe);
    auto *NewIDs = replaceUnsafeScopeInList(Ctx, OldIDs, Old, New);
    I.setMetadata(LLVMContext::MD_unsafe, NewIDs);
    replaceUnsafeScopeForShadowIntrinsic(I, Old, New);
  }
}

CallInst *unsafe::createGenIntrinsic(IRBuilderBase &IRB, Metadata *ID) {
  auto &Ctx = IRB.getContext();
  auto *Gen = IRB.CreateIntrinsic(Intrinsic::unsafe_shadow_gen,
                                  MetadataAsValue::get(Ctx, ID));
  Gen->setMetadata(LLVMContext::MD_unsafe, MDNode::get(Ctx, {ID}));
  return Gen;
}

void llvm::sortInstructionsInRPO(MutableArrayRef<Instruction *> Insts) {
  if (Insts.empty())
    return;
  SmallVector<Instruction *> Result;
  SmallSet<Instruction *, 8> Set;
  copy(Insts, std::inserter(Set, Set.end()));
  auto *F = Insts[0]->getFunction();
  for (auto *B : ReversePostOrderTraversal(F))
    for (auto &I : *B)
      if (Set.contains(&I))
        Result.push_back(&I);
  assert(Insts.size() == Result.size());
  copy(Result, Insts.begin());
}

void llvm::sortInstructionsInPO(MutableArrayRef<Instruction *> Insts) {
  sortInstructionsInRPO(Insts);
  reverse(Insts);
}

MDNode *unsafe::concatMetadataNodes(MDNode *A, MDNode *B) {
  if (!A)
    return B;
  if (!B)
    return A;
  SmallVector<Metadata *> MDs;
  copy(A->operands(), std::back_inserter(MDs));
  copy(B->operands(), std::back_inserter(MDs));
  auto &Ctx = A->getContext();
  return MDNode::get(Ctx, MDs);
}

Value *llvm::getUnsafeShadowDep(IRBuilderBase &IRB, Value *A, Value *B) {
  auto IsZero = [](Value *A) {
    auto *C = dyn_cast<ConstantInt>(A);
    return C && C->isZeroValue();
  };
  if (IsZero(A))
    return B;
  if (IsZero(B))
    return A;
#if 0
  return IRB.CreateIntrinsic(Intrinsic::unsafe_shadow_dep, {A, B})
#else
  return IRB.CreateOr(A, B);
#endif
}