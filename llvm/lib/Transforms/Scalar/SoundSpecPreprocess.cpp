//===- SoundSpecPreprocess.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SoundSpec preprocessing pass. Runs very early (before SROA / mem2reg) so it
// can see allocas and pre-SSA memory structure.
//
// Husk for now. The SoundSpec taint canonicalizations that used to live here
// now run in InstCombine, which is their proper home — they are peephole folds
// of the soundspec_taint intrinsic, they get consistent coverage at every opt
// level and LTO phase, and the worklist drives them to a fixpoint:
//   - load(taint(p)) => taint(load(p)) and the store dual, plus dead-taint
//     cleanup: InstCombinerImpl::visitLoadInst / visitStoreInst and the
//     soundspec_taint case in InstCombineCalls.cpp.
//   - intra-block transitive-taint removal (drop taint(P) when P already
//     carries taint along a same-block GEP address chain):
//     hasIntraBlockTaintedProducer in InstCombineCalls.cpp.
// Kept as a placeholder for future preprocessing that genuinely needs the
// pre-SROA / pre-mem2reg view.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/SoundSpecPreprocess.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/SoundSpec.h"

using namespace llvm;

#define DEBUG_TYPE "soundspec-preprocess"

PreservedAnalyses SoundSpecPreprocessPass::run(Function &F,
                                               FunctionAnalysisManager &AM) {
  if (!soundspec::EnableSoundSpec || F.isDeclaration())
    return PreservedAnalyses::all();

  // Pattern:
  // q = taint(p)
  // r = gep(q)
  // --->

  // Husk: nothing to do. See file comment.
  return PreservedAnalyses::all();
}
