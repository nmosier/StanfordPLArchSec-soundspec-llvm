#include "llvm/Transforms/Utils/UnsafeScopeConversion.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/UnsafeScope.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Metadata.h"

using namespace llvm;

static void maybeInsertUnsafeIn(Function &F, uint64_t ID,
                                const SmallSet<Instruction *, 4> &Insts,
                                Use &SrcU, Instruction *DestI) {
  using namespace llvm::PatternMatch;

  // Already have llvm.unsafe.in.
  if (match(DestI, m_Intrinsic<Intrinsic::unsafe_in>()))
    return;

  // Producer is inside the unsafe scope.
  if (auto *SrcI = dyn_cast<Instruction>(SrcU.get());
      SrcI && Insts.contains(SrcI))
    return;

  // NOTE: Think about this more.
  // For now, don't mask constants, etc.
  if (!isa<Instruction, Argument>(SrcU.get()))
    return;

  PHINode *DestPhi = dyn_cast<PHINode>(DestI);
  Instruction *InsertBeforeInst = DestI;
  if (DestPhi) {
    auto *B = DestPhi->getIncomingBlock(SrcU);
    if (llvm::succ_size(B) > 1) {
      B = SplitEdge(B, DestI->getParent());
      assert(B);
    }
    InsertBeforeInst = B->getTerminator();
  }

  // Insert llvm.unsafe.in.
  IRBuilder<> IRB(InsertBeforeInst);
  Value *V = IRB.CreateBinaryIntrinsic(Intrinsic::unsafe_in, SrcU.get(),
                                       IRB.getInt32(ID));
  SrcU.set(V);
}

static void maybeInsertUnsafeOut(Function &F, uint64_t ID,
                                 const SmallSet<Instruction *, 4> &Insts,
                                 Use &DestU, Instruction *SrcI) {
  using namespace llvm::PatternMatch;

  // Already have llvm.unsafe.out.
  if (match(SrcI, m_Intrinsic<Intrinsic::unsafe_out>()))
    return;

  // Consumer is inside the unsafe scope.
  if (Insts.contains(cast<Instruction>(DestU.get())))
    return;

  // Insert llvm.unsafe.out.
  Instruction *InsertAfterInst = SrcI;
  if (isa<PHINode>(SrcI))
    InsertAfterInst = &*SrcI->getParent()->getFirstInsertionPt();

  IRBuilder<> IRB(InsertAfterInst->getNextNode());
  auto *V =
      IRB.CreateBinaryIntrinsic(Intrinsic::unsafe_out, SrcI, IRB.getInt32(ID));
  DestU.set(V);
}

void llvm::repairUnsafeScope(Function &F, uint64_t ID,
                             const SmallSet<Instruction *, 4> &Insts) {
  using namespace llvm::PatternMatch;

  // Given the scope defined by the set of instructions, insert
  // necessary metadata and llvm.unsafe.in/out.

  LLVMContext &Ctx = F.getContext();
  IRBuilder<> IRB(Ctx);

  // Some sanity checks
  for (Instruction *I : Insts) {
    // Shouldn't have metadata for another scope.
    if (auto MyID = getUnsafeScopeID(I->getMetadata(LLVMContext::MD_unsafe)))
      assert(MyID == ID);

    // Shouldn't have llvm.unsafe.in/out for another scope.
    uint64_t MyID;
    if (match(I, m_Intrinsic<Intrinsic::unsafe_in>(m_Value(),
                                                   m_ConstantInt(MyID))) ||
        match(I, m_Intrinsic<Intrinsic::unsafe_out>(m_Value(),
                                                    m_ConstantInt(MyID))))
      assert(MyID == ID);
  }

  // First, mark all loads, stores, calls, and branches with metadata.
  auto *MD = MDTuple::get(Ctx, {ConstantAsMetadata::get(IRB.getInt32(ID))});
  for (Instruction *I : Insts) {
    if (isa<CallBase, LoadInst, StoreInst, BranchInst>(I)) {
      [[maybe_unused]] auto *OldMD = I->getMetadata(LLVMContext::MD_unsafe);
      assert(OldMD == nullptr || OldMD == MD);
      I->setMetadata(LLVMContext::MD_unsafe, MD);
      continue;
    }

    // Otherwise, strip the metadata.
    I->setMetadata(LLVMContext::MD_unsafe, nullptr);
  }

  // Mark any register data-flow edges with llvm.unsafe.in/out.
  for (Instruction *I : Insts) {
    // If the instruction already has !unsafe metadata, there is no need
    // to mark its inputs and outputs with llvm.unsafe.in/out.
    if (I->getMetadata(LLVMContext::MD_unsafe))
      continue;
    for (Use &SrcU : I->operands())
      maybeInsertUnsafeIn(F, ID, Insts, SrcU, I);
    for (Use &DestU : I->uses())
      maybeInsertUnsafeOut(F, ID, Insts, DestU, I);
  }
}

void llvm::repairUnsafeScopes(Function &F) {
  using namespace llvm::PatternMatch;

  SmallSet<uint64_t, 4> IDs;
  for (Instruction &I : llvm::instructions(F)) {
    uint64_t ID;
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_in>(m_Value(),
                                                    m_ConstantInt(ID))) ||
        match(&I, m_Intrinsic<Intrinsic::unsafe_out>(m_Value(),
                                                     m_ConstantInt(ID)))) {
      IDs.insert(ID);
    }
    if (auto *MD = I.getMetadata(LLVMContext::MD_unsafe))
      IDs.insert(*getUnsafeScopeID(MD));
  }

  for (auto ID : IDs) {
    auto Insts = getUnsafeScopeInsts(F, ID);
    repairUnsafeScope(F, ID, Insts);
  }

  for (Instruction &I : llvm::instructions(F)) {
    if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
      if (II->isAssumeLikeIntrinsic())
        II->setMetadata(LLVMContext::MD_unsafe, nullptr);
    }
  }
}

//===----------------------------------------------------------------------===//
// New-pass-manager wrapper
//===----------------------------------------------------------------------===//

PreservedAnalyses
RepairUnsafeScopesPass::run(Function &F, FunctionAnalysisManager &AM) {
  repairUnsafeScopes(F);
  // Conservative: assume the repair may have created/erased instructions.
  return PreservedAnalyses::none();
}