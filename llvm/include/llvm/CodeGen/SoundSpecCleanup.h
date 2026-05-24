//===- llvm/CodeGen/SoundSpecCleanup.h --------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SoundSpec cleanup pass. Runs in the codegen pipeline (alongside
// SafeStack / StackProtector), after all IR optimizations and before
// ISel. Lowers `!unsafe`/`!safe` metadata and the SoundSpec shadow
// intrinsics into the hardware-shaped IR that the backend lowers to
// the SoundSpec ISA.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_SOUNDSPECCLEANUP_H
#define LLVM_CODEGEN_SOUNDSPECCLEANUP_H

#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

class SoundSpecCleanupPass : public PassInfoMixin<SoundSpecCleanupPass> {
public:
  SoundSpecCleanupPass() = default;
  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm

#endif // LLVM_CODEGEN_SOUNDSPECCLEANUP_H
