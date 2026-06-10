#pragma once

#include "llvm/Support/CommandLine.h"

namespace llvm {

class Instruction;
class Function;
class Value;
class StoreInst;

namespace soundspec {

// SoundSpec's main flag for enabling.
extern cl::opt<bool> EnableSoundSpec;

bool isSafe(const Instruction &I);

bool isUnsafeFunction(const Function &F);

// True iff V is a call to the `llvm.soundspec.taint` intrinsic.
bool isTaint(const Value *V);

} // namespace soundspec

} // namespace llvm