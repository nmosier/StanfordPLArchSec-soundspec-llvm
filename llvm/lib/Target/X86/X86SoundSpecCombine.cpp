//===- X86SoundSpecCombine.cpp - Late SoundSpec MI combining --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
///
/// Late-stage machine-IR pass that performs basic SoundSpec instruction
/// combining right before emission. It is scheduled at the very end of the X86
/// pipeline (addPreEmitPass2), after all CFG- and instruction-modifying passes
/// and after register allocation, so it sees the final physical-register
/// instruction stream that the AsmPrinter is about to emit.
///
/// Currently folds a standalone TAINT into its producer: when the register a
/// TAINT marks is the sole live def of the nearest preceding same-block
/// instruction, the TAINT is removed and that producer gets the Taint MIFlag
/// (lowered to the taint prefix at emission).
///
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/SoundSpec.h"
#include "llvm/Support/Debug.h"
#include "X86InstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

#define PASS_KEY "x86-soundspec-combine"
#define DEBUG_TYPE PASS_KEY

namespace {
struct X86SoundSpecCombine final : public MachineFunctionPass {
  static char ID;
  X86SoundSpecCombine() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return "X86 SoundSpec Combine"; }
  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // namespace

char X86SoundSpecCombine::ID = 0;

bool X86SoundSpecCombine::runOnMachineFunction(MachineFunction &MF) {
  if (!soundspec::EnableSoundSpec)
    return false;

  LLVM_DEBUG(dbgs() << getPassName() << "\n");

  bool Changed = false;

  const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();

  // Fold a standalone TAINT into its producer: if the register a TAINT marks is
  // the sole live def of an immediately-preceding (same-block) producer, drop
  // the TAINT and set the Taint MIFlag on the producer instead (emission turns
  // that flag into the taint prefix).
  for (auto &MBB : MF) {
    for (auto &MI : make_early_inc_range(MBB)) {
      switch (MI.getOpcode()) {
      default:
        continue;
      case X86::TAINT32r:
      case X86::TAINT64r:
      case X86::TAINT128r:
        break;
      }

      // Operand 0 is the tied def: the register being tainted.
      const MachineOperand &MO = MI.getOperand(0);
      assert(MO.isReg() && MO.isDef() && "unexpected TAINT operand 0");
      Register Reg = MO.getReg();

      // Walk back to the NEAREST instruction that defines/clobbers Reg — that
      // is the producer of the value this TAINT marks. We must stop there: an
      // earlier def is a different value, and a redefinition or clobber in
      // between (another def, a call's regmask, ...) means the taint can't be
      // moved. modifiesRegister() also catches regmask clobbers.
      for (auto *PrevMI = MI.getPrevNode(); PrevMI;
           PrevMI = PrevMI->getPrevNode()) {
        if (!PrevMI->modifiesRegister(Reg, TRI)) {
          // If Reg is read before we reach its producer, moving the taint back
          // onto the producer would over-taint that intervening use — bail.
          // (Checked after modifiesRegister so a read-modify-write producer is
          // still handled as the producer below.)
          if (PrevMI->readsRegister(Reg, TRI))
            break;
          continue;
        }

        // Fold only when Reg is the producer's single live def, so the taint
        // prefix applies unambiguously. Otherwise leave the TAINT in place.
        // Either way, do not look past this producer.
        SmallVector<Register, 2> LiveDefs;
        for (const MachineOperand &Def : PrevMI->defs())
          if (Def.isReg() && !Def.isDead())
            LiveDefs.push_back(Def.getReg());
        if (LiveDefs.size() == 1 && TRI->regsOverlap(Reg, LiveDefs.front())) {
          PrevMI->setFlag(MachineInstr::Taint);
          MI.eraseFromParent();
          Changed = true;
        }
        break;
      }
    }
  }

  return Changed;
}

INITIALIZE_PASS(X86SoundSpecCombine, PASS_KEY, "X86 SoundSpec Combine", false,
                false)

FunctionPass *llvm::createX86SoundSpecCombinePass() {
  return new X86SoundSpecCombine();
}
