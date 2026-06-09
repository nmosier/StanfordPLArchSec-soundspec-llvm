//===- SoundSpecPreprocess.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SoundSpec preprocessing pass. Runs very early (before SROA / mem2reg) so it
// can see allocas and pre-SSA memory structure. Husk for now — fill in.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/SoundSpecPreprocess.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Analysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/SoundSpec.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/ADT/SmallSet.h"

using namespace llvm;

#define DEBUG_TYPE "soundspec-preprocess"

static Value *isSimpleTaintedDeref(Instruction *I) {
  using namespace llvm::PatternMatch;

  // The instruction doesn't have a pointer.
  auto *Ptr = getLoadStorePointerOperand(I);
  if (!Ptr)
    return nullptr;

  // The pointer isn't a simple @llvm.soundspec.taint() intrinsic.
  Value *WrappedPtr;
  if (!match(Ptr, m_Intrinsic<Intrinsic::soundspec_taint>(m_Value(WrappedPtr))))
    return nullptr;

  // The pointer doesn't have !soundspec.public metadata. The marker lives
  // on the `llvm.soundspec.taint` *call* (rustc-side `soundspec_taint_public`
  // attaches it there), not on the load/store consuming the result.
  if (!cast<Instruction>(Ptr)->getMetadata("soundspec.public"))
    return nullptr;
  
  // The pointer is defined in a different block.
  if (cast<Instruction>(Ptr)->getParent() != I->getParent())
    return nullptr;

  return WrappedPtr;
}

static void optimizeLoadStoreInstSimple(Instruction *I) {
  auto *Ptr = isSimpleTaintedDeref(I);
  if (!Ptr)
    return;

  // Handle store.
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    IRBuilder<> IRB(SI);
    auto *Val = IRB.CreateUnaryIntrinsic(Intrinsic::soundspec_taint,
                                        SI->getValueOperand());
    SI->setOperand(0, Val); // Set store value operand.
    SI->setOperand(1, Ptr); // Unwrap store pointer operand.
    return;
  }

  // Handle load.
  if (auto *LI = dyn_cast<LoadInst>(I)) {
    IRBuilder<> IRB(LI->getNextNode());
    auto *Val = IRB.CreateUnaryIntrinsic(Intrinsic::soundspec_taint, LI);
    LI->replaceAllUsesWith(Val);
    // RAUW above also rewrote Val's own argument (a use of LI) to Val,
    // leaving the self-referential `Val = taint(Val)`. Restore it so Val
    // wraps the load result.
    cast<CallInst>(Val)->setArgOperand(0, LI);
    LI->setOperand(0, Ptr); // Unwrap load pointer operand.
    return;
  }
}

PreservedAnalyses SoundSpecPreprocessPass::run(Function &F,
                                               FunctionAnalysisManager &AM) {
  using namespace llvm::PatternMatch;

  if (!soundspec::EnableSoundSpec || F.isDeclaration())
    return PreservedAnalyses::all();

  // Do simple load/store taint optimizations.
  SmallVector<Instruction *> Worklist;
  for (auto &I : instructions(F))
    if (isa<LoadInst, StoreInst>(&I))
      Worklist.push_back(&I);
  for (auto *I : Worklist)
    optimizeLoadStoreInstSimple(I);

  // Remove dead @llvm.soundspec.taint()'s.
  for (auto &I : make_early_inc_range(instructions(F)))
    if (match(&I, m_Intrinsic<Intrinsic::soundspec_taint>()) && I.uses().empty())
      I.eraseFromParent();

  return PreservedAnalyses::none();
}
