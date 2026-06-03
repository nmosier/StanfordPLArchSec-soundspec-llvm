//===-- X86SoundSpecCFI.cpp - SoundSpec CFI for X86 -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Late machine-IR pass that implements the SoundSpec custom CFI scheme,
/// driven by the `soundspec_cfi(id)` function attribute.
///
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/SoundSpec.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/Function.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "X86InstrInfo.h"
#include "llvm/IR/Constants.h"

using namespace llvm;

#define PASS_KEY "x86-soundspec-cfi"
#define DEBUG_TYPE PASS_KEY

namespace {
struct X86SoundSpecCFI final : public MachineFunctionPass {
  static char ID;
  X86SoundSpecCFI() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return "X86 SoundSpec CFI"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace

char X86SoundSpecCFI::ID = 0;

bool X86SoundSpecCFI::runOnMachineFunction(MachineFunction &MF) {
  if (!soundspec::EnableSoundSpec)
    return false;

  LLVM_DEBUG(dbgs() << getPassName() << "\n");

  const Function &F = MF.getFunction();
  auto *TII = MF.getSubtarget().getInstrInfo();

  // Insert CFI dest landingpads.
  // 32-bit.
  // 0f 01 ae deadbeef
  auto *CFI = F.getMetadata(LLVMContext::MD_kcfi_type);
  if (!CFI)
    return false;
  assert(CFI->getNumOperands() == 1);
  const Metadata *MD = CFI->getOperand(0);
  auto *CI = cast<ConstantInt>(cast<ConstantAsMetadata>(MD)->getValue());
  uint32_t DestLabel = CI->getZExtValue();
  auto &MBB = MF.front();
  auto MBBI = MBB.begin();
  BuildMI(MBB, MBBI, DebugLoc(), TII->get(X86::SOUNDSPEC_CFI_DEST))
      .addImm(DestLabel);

  // If this function is unsafe -or- it has a `self` argument, then mask in any
  // branch.
  auto UnsafeArg = [&F](const Argument &A) -> bool {
    return F.hasParamAttribute(A.getArgNo(), Attribute::SoundSpecArgUnsafe);
  };
  if (!F.getMetadata(LLVMContext::MD_safe) || any_of(F.args(), UnsafeArg))
    BuildMI(MBB, MBBI, DebugLoc(), TII->get(X86::UNSAFE_POST));

  // We unconditionally inserted SOUNDSPEC_CFI_DEST above, so MIR changed.
  return true;
}

INITIALIZE_PASS(X86SoundSpecCFI, PASS_KEY, "X86 SoundSpec CFI", false, false)

FunctionPass *llvm::createX86SoundSpecCFIPass() {
  return new X86SoundSpecCFI();
}
