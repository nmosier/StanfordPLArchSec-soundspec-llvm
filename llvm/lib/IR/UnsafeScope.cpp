#include "llvm/IR/UnsafeScope.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PatternMatch.h"

using namespace llvm;

namespace {

struct MyBasicBlockEdge {
  BasicBlock *Src = nullptr;
  BasicBlock *Dest = nullptr;

  MyBasicBlockEdge(BasicBlock *Src, BasicBlock *Dest) : Src(Src), Dest(Dest) {}

  BasicBlock *getStart() const { return Src; }
  BasicBlock *getEnd() const { return Dest; }
};

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

static auto computeControlScope(Function &F, uint64_t ID) {
  SmallVector<Instruction *> Todo;
  SmallSet<Instruction *, 4> Seen;

  auto dfs = [&](auto AddNext) {
    while (!Todo.empty()) {
      auto *I = Todo.pop_back_val();

      // If this has !unsafe metadata, skip.
      if (auto MyID = getUnsafeScopeID(I->getMetadata(LLVMContext::MD_unsafe));
          MyID == ID)
        continue;

      // If we've seen it already, skip.
      if (!Seen.insert(I).second)
        continue;

      // Add successors/predecessors.
      AddNext(I, Todo);
    }
  };

  // Forward pass.
  Todo.push_back(&F.getEntryBlock().front());
  dfs(appendSuccessors);

  // Backward pass.
  for (Instruction &I : llvm::instructions(F))
    if (isa<ReturnInst>(&I))
      Todo.push_back(&I);
  dfs(appendPredecessors);

  // Compute the instructions in the scope.
  SmallSet<Instruction *, 4> Insts;
  for (Instruction &I : llvm::instructions(F))
    if (!Seen.contains(&I))
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

SmallSet<Instruction *, 4> llvm::getUnsafeScopeInsts(Function &F, uint64_t ID) {
  auto ControlScope = computeControlScope(F, ID);
  auto DataScope = computeDataScope(F, ID);
  SmallSet<Instruction *, 4> Result;
  llvm::copy(ControlScope, std::inserter(Result, Result.end()));
  llvm::copy(DataScope, std::inserter(Result, Result.end()));
  return Result;
}