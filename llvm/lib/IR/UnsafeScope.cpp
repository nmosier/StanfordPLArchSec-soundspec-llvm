#include "llvm/IR/UnsafeScope.h"
#include "LLVMContextImpl.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
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
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Analysis/LoopInfo.h"
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

static uint32_t getInt32(Metadata *MD) {
  auto *CAM = cast<ConstantAsMetadata>(MD);
  auto *C = cast<ConstantInt>(CAM->getValue());
  return C->getLimitedValue();
}

static void sortUniqueMetadata(SmallVectorImpl<Metadata *> &MDs) {
  llvm::sort(MDs, [&](auto *A, auto *B) { return getInt32(A) < getInt32(B); });
  MDs.erase(llvm::unique(MDs), MDs.end());
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
  if (isa<BinaryOperator, UnaryOperator, CastInst, CmpInst>(&I))
    return false;

  switch (I.getOpcode()) {
  case Instruction::Br:
    return cast<BranchInst>(I).isConditional();

  case Instruction::Switch:
  case Instruction::IndirectBr:
  case Instruction::Store:
    return true;

  case Instruction::CallBr:
    return cast<CallBrInst>(I).getNumIndirectDests() > 0;

  case Instruction::Invoke:
  case Instruction::Call:
    return cast<CallBase>(I).isIndirectCall();

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
  auto *V = getTransmittedOperand(I);
  return V && !isValueConstantPHI(V);
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

MDNode *unsafe::enumerateArgumentDeps(const CallBase &I, unsigned ArgNo) {
  LLVMContext &Ctx = I.getContext();
  SmallVector<Metadata *> IDs;
  SmallVector<const Value *> Todo;
  Todo.push_back(I.getArgOperand(ArgNo));
  enumerateDepsImpl(Todo, Ctx, IDs);
  return MDNode::get(Ctx, IDs);
}

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
    return false;
    // We should mark llvm.unsafe.shadow.gen with the !unsafe scope it belongs to.
  case Intrinsic::unsafe_shadow_gen:
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

uint32_t unsafe::getScopeFromMetadata(const Metadata *MD) {
  auto *CAM = cast<ConstantAsMetadata>(MD);
  auto *C = CAM->getValue();
  auto *CI = cast<ConstantInt>(C);
  return CI->getZExtValue();
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

uint32_t unsafe::getMaxScopeID(const Function &F) {
  uint32_t Best = 0;
  for (const Instruction &I : instructions(F)) {
    auto *IDs = I.getMetadata(LLVMContext::MD_unsafe);
    if (!IDs)
      continue;
    for (Metadata *ID : IDs->operands()) {
      auto IntID = getInt32(ID);
      Best = std::max<uint32_t>(Best, IntID);
    }
  }
  return Best;
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