#include "llvm/IR/SoundSpec.h"

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/ADT/STLExtras.h"

using namespace llvm;

cl::opt<bool> soundspec::EnableSoundSpec("soundspec", cl::init(true));

bool soundspec::isSafe(const Instruction &I) {
  // Store instructions are only safe if their !safe metadata wasn't tampered
  // with.
  if (auto *SI = dyn_cast<StoreInst>(&I))
    return I.getMetadata(LLVMContext::MD_safe) != nullptr;

  // Any instruction that may write to memory that isn't a non-intrinsic
  // call is treated as unsafe.
  if (I.mayWriteToMemory() && (!isa<CallBase>(&I) || isa<IntrinsicInst>(&I)))
    return false;

  // Otherwise, believe the !safe metadata.
  return I.getMetadata(LLVMContext::MD_safe) != nullptr;
}

bool soundspec::isUnsafeFunction(const Function &F) {
  // If no speculation primitives are !safe, then consider it safe.
  return none_of(instructions(F), soundspec::isSafe);
}