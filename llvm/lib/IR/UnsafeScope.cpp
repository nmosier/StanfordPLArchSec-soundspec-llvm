#include "llvm/IR/UnsafeScope.h"

#include "llvm-c/Types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"

using namespace llvm;

/// Decode the `i32` scope ID stored as the single operand of an
/// `!unsafe !N = !{i32 ID}` MDNode (created in
/// `rustc_codegen_llvm::context::CodegenCx::soundspec_unsafe_md_node`).
/// Returns `std::nullopt` if `MD` doesn't have the expected shape.
std::optional<uint64_t> llvm::getUnsafeScopeID(const MDNode *MD) {
  if (!MD || MD->getNumOperands() != 1)
    return std::nullopt;
  auto *ValueAsMD = dyn_cast<ValueAsMetadata>(MD->getOperand(0).get());
  if (!ValueAsMD)
    return std::nullopt;
  auto *CI = dyn_cast<ConstantInt>(ValueAsMD->getValue());
  if (!CI)
    return std::nullopt;
  return CI->getZExtValue();
}

std::optional<uint64_t> llvm::getUnsafeScopeID(const Instruction &I) {
  if (const IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I)) {
    switch (II->getIntrinsicID()) {
    case Intrinsic::unsafe_in:
    case Intrinsic::unsafe_out:
      assert(!I.getMetadata(LLVMContext::MD_unsafe));
      return cast<ConstantInt>(II->getArgOperand(1))->getLimitedValue();
    case Intrinsic::unsafe_enter:
    case Intrinsic::unsafe_exit:
      assert(!I.getMetadata(LLVMContext::MD_unsafe));
      return cast<ConstantInt>(II->getArgOperand(0))->getLimitedValue();
    }
  }
  return getUnsafeScopeID(I.getMetadata(LLVMContext::MD_unsafe));
}

bool llvm::isUnsafeScopeIntr(const Instruction &I, uint64_t *ID,
                                   Intrinsic::ID IID) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;
  if (II->getIntrinsicID() != IID)
    return false;
  if (ID) {
    auto Idx = II->arg_size() - 1;
    auto *Val = II->getOperand(Idx);
    *ID = cast<ConstantInt>(Val)->getLimitedValue();
  }
  return true;  
}

bool llvm::isUnsafeScopeEnter(const Instruction &I, uint64_t *ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_enter);
}

bool llvm::isUnsafeScopeIntr(const Instruction &I, uint64_t UnsafeScopeID,
                                   Intrinsic::ID IID) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II || II->getIntrinsicID() != IID)
    return false;
  auto ScopeIdx = II->arg_size() - 1;
  return cast<ConstantInt>(II->getArgOperand(ScopeIdx))->getLimitedValue() == UnsafeScopeID;
}

bool llvm::isUnsafeScopeEnter(const Instruction &I, uint64_t ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_enter);
}

bool llvm::isUnsafeScopeExit(const Instruction &I, uint64_t *ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_exit);
}

bool llvm::isUnsafeScopeExit(const Instruction &I, uint64_t ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_exit);
}

bool llvm::isUnsafeScopeEnterOrExit(const Instruction &I, uint64_t *ID) {
  auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;
  switch (II->getIntrinsicID()) {
  default:
    return false;
  case Intrinsic::unsafe_enter:
  case Intrinsic::unsafe_exit:
    if (ID)
      *ID = cast<ConstantInt>(II->getArgOperand(0))->getLimitedValue();
    return true;
  }
}

bool llvm::isUnsafeScopeIn(const Instruction &I, uint64_t *ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_in);
}

bool llvm::isUnsafeScopeOut(const Instruction &I, uint64_t *ID) {
  return isUnsafeScopeIntr(I, ID, Intrinsic::unsafe_out);
}

bool llvm::isUnsafeScopeIntr(const Instruction &I,
                             ArrayRef<Intrinsic::ID> IIDs) {
  auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;
  return llvm::is_contained(IIDs, II->getIntrinsicID());
}