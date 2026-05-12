#ifndef LLVM_TRANSFORMS_UTILS_H
#define LLVM_TRANSFORMS_UTILS_H

#include "llvm/ADT/SmallSet.h"

namespace llvm {

class Function;
class Instruction;

void convertUnsafeMetadataToIntrinsics(Function &F);
void addMissingUnsafeInOutIntrinsics(Function &F);

void convertUnsafeMetadataToIntrinsicsNew(Function &F);

void repairUnsafeScope(Function &F, uint64_t ID,
                       const SmallSet<Instruction *, 4> &Insts);
void repairUnsafeScopes(Function &F);

}

#endif