#pragma once

#include "llvm/Support/CommandLine.h"

namespace llvm {

class Instruction;
class Function;

namespace soundspec {

// SoundSpec's main flag for enabling.
extern cl::opt<bool> EnableSoundSpec;

bool isSafe(const Instruction &I);

bool isUnsafeFunction(const Function &F);

}

} // namespace llvm