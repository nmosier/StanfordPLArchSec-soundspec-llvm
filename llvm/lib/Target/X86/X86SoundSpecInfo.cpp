#include "X86SoundSpecInfo.h"

#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/ErrorHandling.h"
#include "X86.h"
#include "X86InstrInfo.h"

using namespace llvm;

bool llvm::X86::soundspec::maybeSpeculationPrimitive(const MachineInstr &MI) {
  auto &MF = *MI.getMF();
  auto &MBB = *MI.getParent();

  // Does this have an address operand that it may store to?
  // If so, treat as store speculation primitive.
  auto MemIdx = X86::getFirstAddrOperandIdx(MI);
  if (MemIdx >= 0 && MI.mayStore())
    return true;

  // Is this a terminator with multiple successors?
  if (MI.isTerminator() && MBB.succ_size() > 1)
    return true;

  // Indirect branches are speculation primitives.
  if (MI.isIndirectBranch())
    return true;

  // If this is a call to a non-constant callee, consider speculative.
  if (MI.isCall()) {
    auto &MO = MI.getOperand(0);
    if (!MO.isGlobal() && !MO.isSymbol() && !MO.isMCSymbol())
      return true;
  }

  // Otherwise, not a speculation primitive.
  return false;
}