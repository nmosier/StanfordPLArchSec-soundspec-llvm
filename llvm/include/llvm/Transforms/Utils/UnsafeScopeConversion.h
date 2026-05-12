#ifndef LLVM_TRANSFORMS_UTILS_H
#define LLVM_TRANSFORMS_UTILS_H

namespace llvm {

class Function;

void convertUnsafeMetadataToIntrinsics(Function &F);
void addMissingUnsafeInOutIntrinsics(Function &F);

}

#endif