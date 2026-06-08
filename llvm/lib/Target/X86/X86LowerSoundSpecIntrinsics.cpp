//===-- X86LowerSoundSpecIntrinsics.cpp -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pre-isel IR-level lowering for `llvm.soundspec.taint` calls whose operand
// type isn't directly matched by the td patterns (i32 / i64 / ptr). Two
// categories:
//
//   - Small integer types (i1, i8, i16): zext to i32, taint, trunc back.
//     Keeps the value in a GPR-class register through the taint
//     instruction so isel can match `TAINT32r`.
//   - Vector / FP / other non-GPR types: replace with an
//     `llvm.x86.sse2.lfence` + identity. LFENCE is a coarse speculation
//     barrier — correct but pessimistic for anything not directly
//     taintable in a single GPR.
//
// Runs as the last X86 IR pass before instruction selection so the rewrite
// is invisible to InstCombine / IR-level taint folds (idempotency,
// push-down through non-transmitter ops).
//
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "x86-lower-soundspec-intrinsics"

namespace llvm {
void initializeX86LowerSoundSpecIntrinsicsLegacyPassPass(PassRegistry &);
}

namespace {

class X86LowerSoundSpecIntrinsicsLegacyPass : public FunctionPass {
public:
  static char ID;
  X86LowerSoundSpecIntrinsicsLegacyPass() : FunctionPass(ID) {}
  bool runOnFunction(Function &F) override;
  StringRef getPassName() const override {
    return "X86 lower SoundSpec intrinsics";
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // The rewrite is value-equivalent (zext-taint-trunc round trip on a
    // value semantically equal to the original) and doesn't touch CFG.
    AU.setPreservesAll();
  }
};

} // end anonymous namespace

char X86LowerSoundSpecIntrinsicsLegacyPass::ID = 0;

INITIALIZE_PASS(X86LowerSoundSpecIntrinsicsLegacyPass, DEBUG_TYPE,
                "X86 lower SoundSpec intrinsics", false, false)

FunctionPass *llvm::createX86LowerSoundSpecIntrinsicsPass() {
  return new X86LowerSoundSpecIntrinsicsLegacyPass();
}

static cl::opt<bool> InsertLFENCEs(DEBUG_TYPE "-lfences", cl::init(true),
                                   cl::Hidden);
static cl::opt<bool> StripTaint(DEBUG_TYPE "-strip", cl::init(false), cl::Hidden);

static Value *CreateTaint(IRBuilderBase &IRB, Value *Op) {
  return IRB.CreateUnaryIntrinsic(Intrinsic::soundspec_taint, Op);
}

static Value *rewriteTaintInst(Instruction &I, Value *Op) {
  auto *T = Op->getType();

  IRBuilder<> IRB(&I);

  // Pointers will always be 32-bit or 64-bit.
  if (T->isPointerTy())
    return &I;

  // Handle integers.
  if (T->isIntegerTy()) {
    auto N = T->getIntegerBitWidth();
    switch (N) {
      // Leave these alone.
    case 32:
    case 64:
      return &I;
    }
    if (N < 32) {
      // ZExt + Trunc.
      Op = IRB.CreateZExt(Op, IRB.getInt32Ty());
      Op = CreateTaint(IRB, Op);
      Op = IRB.CreateTrunc(Op, T);
      return Op;
    }
  }

  // Handle aggregate types (structs and arrays).
  if (T->isAggregateType()) {
    // Iterate over aggregates.
    Value *NewOp = PoisonValue::get(T);
    unsigned NumElts = 0;
    if (T->isStructTy()) {
      NumElts = T->getStructNumElements();
    } else if (T->isArrayTy()) {
      NumElts = T->getArrayNumElements();
    }
    for (unsigned Idx = 0; Idx < NumElts; ++Idx) {
      Value *V = IRB.CreateExtractValue(Op, {Idx});
      V = CreateTaint(IRB, V);
      NewOp = IRB.CreateInsertValue(NewOp, V, {Idx});
    }
    return NewOp;
  }

  // Handle fixed-width vector types.
  if (auto *VT = dyn_cast<FixedVectorType>(T)) {
    constexpr unsigned SupportedBitWidths[] = {512, 256, 128};
    unsigned Bits = VT->getPrimitiveSizeInBits().getFixedValue();
    // 128-bit vectors are matched by the TAINT128r td patterns. Wider
    // vectors (YMM/ZMM) have no TAINT256r/TAINT512r form yet, and we
    // don't decompose into 128-bit halves here, so they fall through to
    // the LFENCE fallback below.
    if (Bits == 128)
      return &I;

    

    // TODO: Consider decomposing them into 128-bit chunks?
    if (Bits % 128 == 0) {

    }
  }

  // Otherwise, LFENCE + identity. Return the original input so the caller
  // RAUWs uses of the taint with `Op` and erases the call.
  if (InsertLFENCEs) {
    IRB.CreateIntrinsic(Intrinsic::x86_sse2_lfence, {});
    LLVM_DEBUG(dbgs() << "[soundspec] falling back to LFENCE for " << I << "\n");
  }
  return Op;
}

static bool stripTaint(Function &F) {
  using namespace llvm::PatternMatch;
  bool Changed = false;
  for (auto &I : make_early_inc_range(instructions(F))) {
    Value *Op;
    if (!match(&I, m_Intrinsic<Intrinsic::soundspec_taint>(m_Value(Op))))
      continue;
    I.replaceAllUsesWith(Op);
    I.eraseFromParent();
    Changed = true;
  }
  return Changed;
}

bool X86LowerSoundSpecIntrinsicsLegacyPass::runOnFunction(Function &F) {
  if (StripTaint) {
    return stripTaint(F);
  }

  using namespace llvm::PatternMatch;
  bool ChangedOverall = false;
  bool Changed;
  do {
    Changed = false;
    for (auto &I : make_early_inc_range(instructions(F))) {
      Value *Op;
      if (!match(&I, m_Intrinsic<Intrinsic::soundspec_taint>(m_Value(Op))))
        continue;
      if (auto *NewI = rewriteTaintInst(I, Op); NewI != &I) {
        I.replaceAllUsesWith(NewI);
        I.eraseFromParent();
        Changed = true;
      }
    }
    ChangedOverall |= Changed;
  } while (Changed); 

  return ChangedOverall;
}