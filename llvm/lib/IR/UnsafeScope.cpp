#include "llvm/IR/UnsafeScope.h"

#include "llvm-c/Types.h"
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
      return cast<ConstantInt>(II->getOperand(1))->getLimitedValue();
    case Intrinsic::unsafe_enter:
    case Intrinsic::unsafe_exit:
      assert(!I.getMetadata(LLVMContext::MD_unsafe));
      return cast<ConstantInt>(II->getOperand(0))->getLimitedValue();
    }
  }
  return getUnsafeScopeID(I.getMetadata(LLVMContext::MD_unsafe));
}

bool llvm::isUnsafeScopeEnter(const Instruction &I) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  return II && II->getIntrinsicID() == Intrinsic::unsafe_enter;
}

static bool isUnsafeScopeEnterExit(const Instruction &I, uint64_t UnsafeScopeID,
                                   Intrinsic::ID IID) {
  const auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II || II->getIntrinsicID() != IID)
    return false;
  return cast<ConstantInt>(II->getOperand(1))->getLimitedValue() == UnsafeScopeID;
}

bool llvm::isUnsafeScopeEnter(const Instruction &I, uint64_t ID) {
  return isUnsafeScopeEnterExit(I, ID, Intrinsic::unsafe_enter);
}

bool llvm::isUnsafeScopeExit(const Instruction &I, uint64_t ID) {
  return isUnsafeScopeEnterExit(I, ID, Intrinsic::unsafe_exit);
}