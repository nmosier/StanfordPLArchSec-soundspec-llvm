//===- StripSoundSpecTaint.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Removes every `llvm.soundspec.taint` intrinsic call by RAUW'ing its result
// with its argument. Bisection tool — drop in at different pipeline points
// to localize where the taint-related overhead originates.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/StripSoundSpecTaint.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace llvm {
void initializeStripSoundSpecTaintLegacyPass(PassRegistry &);
}

static bool stripSoundSpecTaint(Function &F) {
  if (F.isDeclaration())
    return false;

  SmallVector<IntrinsicInst *, 16> Taints;
  for (Instruction &I : instructions(F))
    if (auto *II = dyn_cast<IntrinsicInst>(&I))
      if (II->getIntrinsicID() == Intrinsic::soundspec_taint)
        Taints.push_back(II);

  if (Taints.empty())
    return false;

  for (IntrinsicInst *II : Taints) {
    II->replaceAllUsesWith(II->getArgOperand(0));
    II->eraseFromParent();
  }
  return true;
}

PreservedAnalyses StripSoundSpecTaintPass::run(Function &F,
                                                FunctionAnalysisManager &AM) {
  if (!stripSoundSpecTaint(F))
    return PreservedAnalyses::all();

  // No CFG changes; analyses depending on value identity (use-lists, AA
  // result caches) may be invalidated, so be conservative on those.
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

namespace {
class StripSoundSpecTaintLegacy : public FunctionPass {
public:
  static char ID;
  StripSoundSpecTaintLegacy() : FunctionPass(ID) {}
  bool runOnFunction(Function &F) override {
    return stripSoundSpecTaint(F);
  }
  StringRef getPassName() const override {
    return "Strip SoundSpec taint intrinsics";
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // The strip is semantically identity (RAUW of intrinsic call with its
    // argument), and the intrinsic was IntrNoMem/IntrSpeculatable so no
    // memory/control-flow analyses can change. Preserve everything.
    AU.setPreservesAll();
  }
};
} // end anonymous namespace

char StripSoundSpecTaintLegacy::ID = 0;
INITIALIZE_PASS(StripSoundSpecTaintLegacy, "strip-soundspec-taint-legacy",
                "Strip SoundSpec taint intrinsics", false, false)

FunctionPass *llvm::createStripSoundSpecTaintLegacyPass() {
  return new StripSoundSpecTaintLegacy();
}
