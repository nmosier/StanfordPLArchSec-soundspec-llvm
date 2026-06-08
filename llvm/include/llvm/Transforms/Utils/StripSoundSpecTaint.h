//===- StripSoundSpecTaint.h ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Removes every `llvm.soundspec.taint` intrinsic call by RAUW'ing its result
// with its argument. Useful for binary-searching pipeline placement: insert
// this pass at different stages to find where the taint-related overhead
// blows up.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_STRIPSOUNDSPECTAINT_H
#define LLVM_TRANSFORMS_UTILS_STRIPSOUNDSPECTAINT_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;
class FunctionPass;

class StripSoundSpecTaintPass : public PassInfoMixin<StripSoundSpecTaintPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

FunctionPass *createStripSoundSpecTaintLegacyPass();

} // end namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_STRIPSOUNDSPECTAINT_H
