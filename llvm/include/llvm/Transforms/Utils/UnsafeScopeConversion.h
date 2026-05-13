#ifndef LLVM_TRANSFORMS_UTILS_H
#define LLVM_TRANSFORMS_UTILS_H

#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

class Function;
class Instruction;

void convertUnsafeMetadataToIntrinsics(Function &F);
void addMissingUnsafeInOutIntrinsics(Function &F);

void convertUnsafeMetadataToIntrinsicsNew(Function &F);

void repairUnsafeScope(Function &F, uint64_t ID,
                       const SmallSet<Instruction *, 4> &Insts);
void repairUnsafeScopes(Function &F);

/// New-pass-manager wrapper around \c repairUnsafeScopes. Lets the pass be
/// driven from the pipeline parser via `-passes=repair-unsafe-scopes`.
class RepairUnsafeScopesPass
    : public PassInfoMixin<RepairUnsafeScopesPass> {
public:
  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }
};

}

#endif