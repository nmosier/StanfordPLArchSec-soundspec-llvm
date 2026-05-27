#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

class Function;

void optimizeUnsafeScopes(Function &F);

class SoundSpecOptimizerPass : public PassInfoMixin<SoundSpecOptimizerPass> {
public:
  SoundSpecOptimizerPass() = default;
  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

}
