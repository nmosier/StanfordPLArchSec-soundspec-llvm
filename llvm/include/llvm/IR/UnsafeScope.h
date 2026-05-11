#ifndef LLVM_IR_UNSAFESCOPE_H
#define LLVM_IR_UNSAFESCOPE_H

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include <functional>

namespace llvm {

class UnsafeBuilderScope {
  IRBuilderBase &Builder;

public:
  UnsafeBuilderScope(IRBuilderBase &Builder, MDNode *UnsafeScopeMD)
      : Builder(Builder) {
    Builder.SetUnsafeMetadata(UnsafeScopeMD);
  }
  UnsafeBuilderScope(IRBuilderBase &Builder, const Instruction &I)
      : UnsafeBuilderScope(Builder, I.getMetadata(LLVMContext::MD_unsafe)) {}

  ~UnsafeBuilderScope() {
    Builder.SetUnsafeMetadata(nullptr);
  }
};

inline Instruction &removeUnsafeScope(Instruction &I) {
  I.setMetadata(LLVMContext::MD_unsafe, nullptr);
}

inline Instruction *removeUnsafeScope(Instruction *I) {
  return &removeUnsafeScope(*I);
}

inline MDNode *takeUnsafeScope(Instruction &I){
  return I.takeMetadata(LLVMContext::MD_unsafe);
}

inline MDNode *stashUnsafeScope(Instruction &I) {
  MDNode *MD = I.takeMetadata(LLVMContext::MD_unsafe);
  I.setMetadata("unsafe.bak", MD);
  return MD;
}

inline MDNode *restoreUnsafeScope(Instruction &I) {
  MDNode *MD = I.takeMetadata("unsafe.bak");
  I.setMetadata(LLVMContext::MD_unsafe, MD);
  return MD;
}

inline MDNode *takeUnsafeScopeStashed(Instruction &I) {
  return I.takeMetadata("unsafe.bak");
}

inline MDNode *getUnsafeScope(const Instruction &I) {
  return I.getMetadata(LLVMContext::MD_unsafe);
}

}

#endif