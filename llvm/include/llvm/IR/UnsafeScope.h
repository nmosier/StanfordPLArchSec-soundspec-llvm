#ifndef LLVM_IR_UNSAFESCOPE_H
#define LLVM_IR_UNSAFESCOPE_H

#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/ADT/SmallSet.h"
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

// TODO: Remove these in favor of `isUnsafeScopeIntr`?
bool isUnsafeScopeEnter(const Instruction &I, uint64_t *ID = nullptr);
bool isUnsafeScopeEnter(const Instruction &I, uint64_t ID);
bool isUnsafeScopeExit(const Instruction &I, uint64_t *ID = nullptr);
bool isUnsafeScopeExit(const Instruction &I, uint64_t ID);
bool isUnsafeScopeEnterOrExit(const Instruction &I, uint64_t *ID = nullptr);

bool isUnsafeScopeIn(const Instruction &I, uint64_t *ID = nullptr);
bool isUnsafeScopeOut(const Instruction &I, uint64_t *ID = nullptr);

// TODO: Should reorder 2nd and 3rd arguments.
bool isUnsafeScopeIntr(const Instruction &I, uint64_t UnsafeScopeID,
                       Intrinsic::ID IID);
bool isUnsafeScopeIntr(const Instruction &I, uint64_t *ID, Intrinsic::ID IID);
bool isUnsafeScopeIntr(const Instruction &I, ArrayRef<Intrinsic::ID> IIDs);

// TODO: Deprecate most of this in favor of match().

// TODO: Remove.
template <typename OutputIt>
OutputIt insertSuccessors(Instruction *I, OutputIt out) {
  if (Instruction *Succ = I->getNextNode()) {
    *out++ = Succ;
  } else {
    for (auto *Succ : llvm::successors(I->getParent()))
      *out++ = &Succ->front();
  }
  return out;
}

template <typename OutputIt>
OutputIt insertPredecessors(Instruction *I, OutputIt Out) {
  if (Instruction *Pred = I->getPrevNode()) {
    *Out++ = Pred;
  } else {
    for (auto *Pred : llvm::predecessors(I->getParent()))
      *Out++ = &Pred->back();
  }
  return Out;
}

SmallSet<Instruction *, 4> getUnsafeScopeInsts(Function &F, uint64_t ID);

void dumpUnsafeScope(const Function &F, uint64_t ID);

std::optional<uint64_t> getValueUnsafeScope(const Value *V);

namespace unsafe {

bool isReturnLike(const Instruction &I);

}

} // namespace llvm

#endif