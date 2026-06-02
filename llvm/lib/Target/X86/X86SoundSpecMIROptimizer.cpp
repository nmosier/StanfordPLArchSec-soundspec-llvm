//===-- X86SoundSpecMIROptimizer.cpp - SoundSpec MIR optimizer for X86 ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// X86 machine-IR pass that runs late in the codegen pipeline
/// (addPreEmitPass2, post-register-allocation) to lower the SoundSpec meta
/// pseudos emitted by ISel into their final machine form. Operates on
/// physical registers.
///
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86.h"
#include "X86InstrInfo.h"
#include "X86RegisterInfo.h"
#include "X86Subtarget.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define PASS_KEY "x86-soundspec-mir-optimizer"
#define DEBUG_TYPE PASS_KEY

STATISTIC(NumProtectGR32,
          "SoundSpec embeds lowered to SOUNDSPEC_PROTECT_GR32");
STATISTIC(NumProtectGR64,
          "SoundSpec embeds lowered to SOUNDSPEC_PROTECT_GR64");
STATISTIC(NumEmbedLFENCE,
          "SoundSpec embeds lowered to the LFENCE fallback (no PROTECT variant)");
STATISTIC(NumArgLFENCE,
          "SoundSpec shadow.arg fallbacks lowered to an LFENCE + zero token");

namespace {
struct X86SoundSpecMIROptimizer final : public MachineFunctionPass {
  static char ID;
  X86SoundSpecMIROptimizer() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override {
    return "X86 SoundSpec MIR Optimizer";
  }
  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
} // namespace

char X86SoundSpecMIROptimizer::ID = 0;

// Lower one UNSAFE_SHADOW_EMBED meta pseudo to its final machine form.
//
// The pseudo is `$dst = UNSAFE_SHADOW_EMBED $val(tied to $dst), $tok`. It's a
// pass-through on the data register (`$val = $dst`), with `$tok` the i32 dep
// kept as a register use. Post-RA, `$dst` and `$val` are the same physreg, so
// the data already sits where consumers expect it. We pick the concrete form
// from the data register's class:
//   GR32 -> SOUNDSPEC_PROTECT_GR32
//   GR64 -> SOUNDSPEC_PROTECT_GR64 (dep widened to its 64-bit super-reg)
//   else -> no PROTECT variant: plant an LFENCE as a coarse speculation
//           barrier and drop the pseudo (the data needs no move).
static void lowerUnsafeShadowEmbed(MachineInstr &MI, const TargetInstrInfo &TII,
                                   const TargetRegisterInfo &TRI) {
  MachineBasicBlock &MBB = *MI.getParent();
  const DebugLoc &DL = MI.getDebugLoc();
  Register Dst = MI.getOperand(0).getReg();
  Register Val = MI.getOperand(1).getReg();
  Register Dep = MI.getOperand(2).getReg();

  if (X86::GR32RegClass.contains(Dst)) {
    ++NumProtectGR32;
    BuildMI(MBB, MI, DL, TII.get(X86::SOUNDSPEC_PROTECT_GR32), Dst)
        .addReg(Val)
        .addReg(Dep);
  } else if (X86::GR64RegClass.contains(Dst)) {
    // The dep is an i32 GPR; the GR64 form needs a 64-bit dep register. Use
    // its 64-bit super-register — sound because every 32-bit GPR write
    // zero-extends into the parent, so the upper half is already 0.
    Register Dep64 =
        TRI.getMatchingSuperReg(Dep, X86::sub_32bit, &X86::GR64RegClass);
    assert(Dep64 && "no GR64 super-register for SoundSpec dep");
    ++NumProtectGR64;
    BuildMI(MBB, MI, DL, TII.get(X86::SOUNDSPEC_PROTECT_GR64), Dst)
        .addReg(Val)
        .addReg(Dep64);
  } else {
    ++NumEmbedLFENCE;
    BuildMI(MBB, MI, DL, TII.get(X86::LFENCE));
  }

  MI.eraseFromParent();
}

// Lower one UNSAFE_ARG_LFENCE carrier pseudo. It stands in for a
// `llvm.unsafe.shadow.arg` whose source register class has no `unsafe_arg`
// form (XMM, GR8/16, etc.). Emit the real LFENCE barrier and materialize a
// zero shadow token into the pseudo's def — the token carries no real
// dependency; the LFENCE is the protection.
static void lowerUnsafeArgLFence(MachineInstr &MI, const TargetInstrInfo &TII) {
  MachineBasicBlock &MBB = *MI.getParent();
  const DebugLoc &DL = MI.getDebugLoc();
  Register Tok = MI.getOperand(0).getReg();
  ++NumArgLFENCE;
  BuildMI(MBB, MI, DL, TII.get(X86::LFENCE));
  BuildMI(MBB, MI, DL, TII.get(X86::MOV32ri), Tok).addImm(0);
  MI.eraseFromParent();
}

bool X86SoundSpecMIROptimizer::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(dbgs() << getPassName() << "\n");

  const X86Subtarget &ST = MF.getSubtarget<X86Subtarget>();
  const TargetInstrInfo &TII = *ST.getInstrInfo();
  const TargetRegisterInfo &TRI = *ST.getRegisterInfo();

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : make_early_inc_range(MBB)) {
      switch (MI.getOpcode()) {
      case TargetOpcode::UNSAFE_SHADOW_EMBED:
        lowerUnsafeShadowEmbed(MI, TII, TRI);
        Changed = true;
        break;
      case X86::UNSAFE_ARG_LFENCE:
        lowerUnsafeArgLFence(MI, TII);
        Changed = true;
        break;
      default:
        break;
      }
    }
  }

  return Changed;
}

INITIALIZE_PASS(X86SoundSpecMIROptimizer, PASS_KEY,
                "X86 SoundSpec MIR Optimizer", false, false)

FunctionPass *llvm::createX86SoundSpecMIROptimizerPass() {
  return new X86SoundSpecMIROptimizer();
}
