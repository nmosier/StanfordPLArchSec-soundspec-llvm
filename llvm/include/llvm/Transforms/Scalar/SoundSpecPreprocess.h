//===- SoundSpecPreprocess.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SoundSpec preprocessing pass. Scheduled very early in the pipeline — before
// SROA / mem2reg — so it can observe allocas and pre-SSA memory structure that
// later passes destroy. Currently a husk.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_SOUNDSPECPREPROCESS_H
#define LLVM_TRANSFORMS_SCALAR_SOUNDSPECPREPROCESS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

class SoundSpecPreprocessPass : public PassInfoMixin<SoundSpecPreprocessPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_SOUNDSPECPREPROCESS_H
