#ifndef LLVM_IR_UNSAFESCOPE_H
#define LLVM_IR_UNSAFESCOPE_H

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include <optional>

namespace llvm {

class IntrinsicInst;

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
  return I;
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

std::optional<uint64_t> getUnsafeScopeID(const MDNode *MD);
std::optional<uint64_t> getUnsafeScopeID(const Instruction &I);

bool isUnsafeScopeEnter(const Instruction &I);
bool isUnsafeScopeEnter(const Instruction &I, uint64_t ID);
bool isUnsafeScopeExit(const Instruction &I, uint64_t ID);

}

#endif