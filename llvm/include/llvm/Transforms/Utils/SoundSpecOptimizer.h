#pragma once

namespace llvm {

// Forward declarations
class Function;

void optimizeUnsafeScopes(Function &F);

}