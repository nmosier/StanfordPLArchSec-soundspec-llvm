//===- LowerUnsafeInit.h - Lower !unsafe.init to !unsafe scope --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// [soundspec] Convert `!unsafe.init` metadata on allocas into `!unsafe`
// metadata on the alloca and on its initialization stores. Intended to
// run after SROA so that split allocas have already inherited the
// `!unsafe.init` marker from their parent aggregate.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_LOWERUNSAFEINIT_H
#define LLVM_TRANSFORMS_SCALAR_LOWERUNSAFEINIT_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Function;

class LowerUnsafeInitPass : public PassInfoMixin<LowerUnsafeInitPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_LOWERUNSAFEINIT_H
