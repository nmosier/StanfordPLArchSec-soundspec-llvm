#include "llvm/IR/UnsafeScope.h"
#include "LLVMContextImpl.h"

#include "llvm/ADT/STLExtras.h"
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
#include <iterator>
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

/// Decode the `i32` scope ID stored as the single operand of an
/// `!unsafe !N = !{i32 ID}` MDNode (created in
/// `rustc_codegen_llvm::context::CodegenCx::soundspec_unsafe_md_node`).
/// Returns `std::nullopt` if `MD` doesn't have the expected shape.
std::optional<uint64_t> llvm::getUnsafeScopeID(const MDNode *MD) {
  if (!MD || MD->getNumOperands() != 1)
    return std::nullopt;
  auto *ValueAsMD = dyn_cast<ValueAsMetadata>(MD->getOperand(0).get());
  if (!ValueAsMD)
    return std::nullopt;
  auto *CI = dyn_cast<ConstantInt>(ValueAsMD->getValue());
  if (!CI)
    return std::nullopt;
  return CI->getZExtValue();
}

std::optional<uint64_t> llvm::getUnsafeScopeID(const Instruction &I) {
  if (const IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I)) {
    switch (II->getIntrinsicID()) {
    case Intrinsic::unsafe_in:
    case Intrinsic::unsafe_out:
      assert(!I.getMetadata(LLVMContext::MD_unsafe));
      return cast<ConstantInt>(II->getArgOperand(1))->getLimitedValue();
    case Intrinsic::unsafe_enter:
    case Intrinsic::unsafe_exit:
      assert(!I.getMetadata(LLVMContext::MD_unsafe));
      return cast<ConstantInt>(II->getArgOperand(0))->getLimitedValue();
    }
  }
  return getUnsafeScopeID(I.getMetadata(LLVMContext::MD_unsafe));
}

bool llvm::isUnsafeScopeIntr(const Instruction &I, uint64_t *ID,
                                   Intrinsic::ID IID) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;
  if (II->getIntrinsicID() != IID)
    return false;
  if (ID) {
    auto Idx = II->arg_size() - 1;
    auto *Val = II->getOperand(Idx);
    *ID = cast<ConstantInt>(Val)->getLimitedValue();
  }
  return true;  
}

bool llvm::isUnsafeScopeEnter(const Instruction &I, uint64_t *ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_enter);
}

bool llvm::isUnsafeScopeIntr(const Instruction &I, uint64_t UnsafeScopeID,
                                   Intrinsic::ID IID) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II || II->getIntrinsicID() != IID)
    return false;
  auto ScopeIdx = II->arg_size() - 1;
  return cast<ConstantInt>(II->getArgOperand(ScopeIdx))->getLimitedValue() == UnsafeScopeID;
}

bool llvm::isUnsafeScopeEnter(const Instruction &I, uint64_t ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_enter);
}

bool llvm::isUnsafeScopeExit(const Instruction &I, uint64_t *ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_exit);
}

bool llvm::isUnsafeScopeExit(const Instruction &I, uint64_t ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_exit);
}

bool llvm::isUnsafeScopeEnterOrExit(const Instruction &I, uint64_t *ID) {
  auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;
  switch (II->getIntrinsicID()) {
  default:
    return false;
  case Intrinsic::unsafe_enter:
  case Intrinsic::unsafe_exit:
    if (ID)
      *ID = cast<ConstantInt>(II->getArgOperand(0))->getLimitedValue();
    return true;
  }
}

bool llvm::isUnsafeScopeIn(const Instruction &I, uint64_t *ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_in);
}

bool llvm::isUnsafeScopeOut(const Instruction &I, uint64_t *ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_out);
}

bool llvm::isUnsafeScopeIntr(const Instruction &I,
                             ArrayRef<Intrinsic::ID> IIDs) {
  auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;
  return llvm::is_contained(IIDs, II->getIntrinsicID());
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

bool unsafe::isReturnLike(const Instruction &I) {
  if (!I.isTerminator())
    return false;
  if (isa<ReturnInst, ResumeInst>(&I))
    return true;
  auto *B = I.getParent();
  return llvm::succ_empty(B);
}

static auto computeControlScope(Function &F, uint64_t ID) {
  SmallSet<Instruction *, 4> Insts;
  for (Instruction &I : instructions(F))
    if (auto MyID = getUnsafeScopeID(I.getMetadata(LLVMContext::MD_unsafe));
        MyID == ID)
      Insts.insert(&I);
  return Insts;
}

static auto computeDataScope(Function &F, uint64_t ID) {
  using namespace llvm::PatternMatch;

  SmallVector<Instruction *> Todo;
  SmallSet<Instruction *, 4> Seen;

  // Forward.
  for (Instruction &I : llvm::instructions(F))
    if (match(&I,
              m_Intrinsic<Intrinsic::unsafe_in>(m_Value(), m_SpecificInt(ID))))
      Todo.push_back(&I);
  while (!Todo.empty()) {
    auto *I = Todo.pop_back_val();

    // If this has !unsafe metadata, skip.
    if (auto MyID = getUnsafeScopeID(I->getMetadata(LLVMContext::MD_unsafe));
        MyID == ID)
      continue;

    // If we've seen it already, skip.
    if (!Seen.insert(I).second)
      continue;

    // If this is llvm.unsafe.out, we're done.
    if (match(I,
              m_Intrinsic<Intrinsic::unsafe_out>(m_Value(), m_SpecificInt(ID))))
      continue;

    // Add consumers.
    for (User *U : I->users())
      Todo.push_back(cast<Instruction>(U));
  }

  // Backward.
  for (Instruction &I : llvm::instructions(F))
    if (match(&I,
              m_Intrinsic<Intrinsic::unsafe_out>(m_Value(), m_SpecificInt(ID))))
      Todo.push_back(&I);
  while (!Todo.empty()) {
    auto *I = Todo.pop_back_val();

    // If this has !unsafe metadata, skip.
    if (auto MyID = getUnsafeScopeID(I->getMetadata(LLVMContext::MD_unsafe));
        MyID == ID)
      continue;

    // If we've seen it already, skip.
    if (!Seen.insert(I).second)
      continue;

    // If this is llvm.unsafe.in, we're done.
    if (match(I,
              m_Intrinsic<Intrinsic::unsafe_in>(m_Value(), m_SpecificInt(ID))))
      continue;

    // Add producers.
    for (Value *V : I->operands())
      if (auto *I = dyn_cast<Instruction>(V))
        Todo.push_back(I);
  }

  return Seen;
}

static bool isValidUnsafeScopeInst(const Instruction *I) {
  auto *II = dyn_cast<IntrinsicInst>(I);
  if (II && II->isAssumeLikeIntrinsic())
    return false;
  if (isa<AllocaInst>(I))
    return false;
  return true;
}

SmallSet<Instruction *, 4> llvm::getUnsafeScopeInsts(Function &F, uint64_t ID) {
  auto ControlScope = computeControlScope(F, ID);
  auto DataScope = computeDataScope(F, ID);
  SmallSet<Instruction *, 4> Result;

  llvm::copy_if(ControlScope, std::inserter(Result, Result.end()), isValidUnsafeScopeInst);
  llvm::copy_if(DataScope, std::inserter(Result, Result.end()), isValidUnsafeScopeInst);

  return Result;
}

void llvm::dumpUnsafeScope(const Function &F, uint64_t ID) {
  auto Insts = getUnsafeScopeInsts(const_cast<Function &>(F), ID);
  errs() << "unsafe scope for " << ID << ": [\n";
  for (auto *I : Insts)
    errs() << "    " << *I << "\n";
  errs() << "]\n";
}

static std::optional<uint64_t>
getValueUnsafeScopeRec(const Value *V, SmallSet<const Value *, 4> &Seen) {
  // If already seen, skip.
  if (!Seen.insert(V).second)
    return std::nullopt;

  // Handle if it's an instruction with !unsafe metadata.
  if (auto *I = dyn_cast<Instruction>(V)) {
    if (auto ID = getUnsafeScopeID(I->getMetadata(LLVMContext::MD_unsafe)))
      return ID;
  }

  // Otherwise, follow its uses.
  if (auto *U = dyn_cast<User>(V)) {
    for (Value *X : U->operands()) {
      if (auto ID = getValueUnsafeScopeRec(X, Seen))
        return ID;
    }
  }

  return std::nullopt;
}

std::optional<uint64_t> llvm::getValueUnsafeScope(const Value *V) {
  SmallSet<const Value *, 4> Seen;
  return getValueUnsafeScopeRec(V, Seen);
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
  assert(!MDs.empty());
  sortUniqueMetadata(MDs);
  auto &Ctx = cast<ConstantAsMetadata>(MDs.front())->getContext();
  return MDTuple::get(Ctx, MDs);
}

MDNode *unsafe::removeID(MDNode *MD, uint64_t ID) {
  if (!MD || !MD->getNumOperands())
    return nullptr;
  SmallVector<Metadata *> MDs;
  llvm::copy(MD->operands(), std::back_inserter(MDs));
  sortUniqueMetadata(MDs);
  auto &Ctx = cast<ConstantAsMetadata>(MDs.front())->getContext();
  return MDTuple::get(Ctx, MDs);
}

bool llvm::isSpeculationPrimitive(const Instruction &I) {
  // Handle conditional branches.
  if (auto *BI = dyn_cast<BranchInst>(&I))
    return BI->isConditional();

  // Handle indirect branches.
  if (auto *CB = dyn_cast<CallBase>(&I))
    return CB->isIndirectCall();

  // Handle stores.
  // TODO: Handle other types of stores?
  if (isa<StoreInst>(&I))
    return true;

  return false;
}

Value *llvm::isTransmitter(Instruction &I) {
  if (auto *BI = dyn_cast<BranchInst>(&I))
    return BI->isConditional() ? BI->getCondition() : nullptr;

  if (auto *CB = dyn_cast<CallBase>(&I))
    return CB->getCalledOperand();

  if (auto *Ptr = getLoadStorePointerOperand(&I))
    return Ptr;

  return nullptr;
}

const Value *llvm::isTransmitter(const Instruction &I) {
  return const_cast<const Value *>(isTransmitter(const_cast<Instruction &>(I)));
}

static Metadata *getUnsafeMDElt(int ID, LLVMContext &Ctx) {
  auto *Int32Ty = IntegerType::getInt32Ty(Ctx);
  auto *C = ConstantInt::get(Int32Ty, ID);
  auto *CAM = ConstantAsMetadata::get(C);
  return CAM;
}

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
