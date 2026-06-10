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
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/Verifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

#define DEBUG_TYPE "x86-lower-soundspec-intrinsics"

namespace llvm {
void initializeX86LowerSoundSpecIntrinsicsLegacyPassPass(PassRegistry &);
}

namespace {

constexpr unsigned TaintAddrSpace = 259; // From X86.h

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
static cl::opt<bool> StripTaint(DEBUG_TYPE "-strip", cl::init(false),
                                cl::Hidden);
static cl::opt<bool> InsertChecks(DEBUG_TYPE "-check", cl::init(false), cl::Hidden);

static Value *CreateTaint(IRBuilderBase &IRB, Value *Op,
                          const Instruction *Src = nullptr) {
  auto *T = IRB.CreateUnaryIntrinsic(Intrinsic::soundspec_taint, Op);
  // Inherit the SoundSpec provenance comment from the taint being lowered so
  // decomposed sub-taints aren't left comment-less.
  if (Src)
    if (MDNode *C = Src->getMetadata(LLVMContext::MD_soundspec_comment))
      T->setMetadata(LLVMContext::MD_soundspec_comment, C);
  return T;
}

// Taint a non-pointer, non-aggregate scalar/vector value (FP scalar, integer
// vector, ...) by bitcasting the whole thing to a same-width integer, tainting
// that GPR-class scalar (matches TAINT32r / TAINT64r), and bitcasting back.
// Sub-32-bit widths get a zext/trunc round trip to reach i32. `Bits` must be in
// (0, 64] and `T` must be bitcast-compatible with i`Bits`.
static Value *taintViaIntBitcast(IRBuilderBase &IRB, Value *Op, Type *T,
                                 unsigned Bits, const Instruction *Src) {
  auto *IntTy = IRB.getIntNTy(Bits);
  Value *V = IRB.CreateBitCast(Op, IntTy);
  if (Bits < 32) {
    V = IRB.CreateZExt(V, IRB.getInt32Ty());
    V = CreateTaint(IRB, V, Src);
    V = IRB.CreateTrunc(V, IntTy);
  } else {
    V = CreateTaint(IRB, V, Src);
  }
  return IRB.CreateBitCast(V, T);
}

static bool rewriteLoadTaintOut(LoadInst *LI) {
  using namespace llvm::PatternMatch;

  // Only rewrite loads with a single use, which is a taint.
  if (!LI->hasOneUser())
    return false;

  // Is the single consumer taint(LI)?
  User *Consumer = *LI->user_begin();
  if (!match(Consumer, m_Intrinsic<Intrinsic::soundspec_taint>()))
    return false;

  // Rewrite the load's pointer with an addrspacecast.
  IRBuilder<> IRB(LI);
  auto *Ptr = IRB.CreateAddrSpaceCast(LI->getPointerOperand(),
                                      IRB.getPtrTy(TaintAddrSpace));
  LI->setOperand(LI->getPointerOperandIndex(), Ptr);

  // Make the consumer dead. Delete later, though.
  Consumer->replaceAllUsesWith(LI);

  return true;
}

static bool rewriteStoreTaintOut(StoreInst *SI) {
  using namespace llvm::PatternMatch;

  // Is the data operand a taint?
  auto *TaintedData = SI->getValueOperand();
  Value *WrappedData;
  if (!match(TaintedData,
             m_Intrinsic<Intrinsic::soundspec_taint>(m_Value(WrappedData))))
    return false;

  // Rewrite the store's pointer with an addrspacecast.
  IRBuilder<> IRB(SI);
  auto *Ptr = IRB.CreateAddrSpaceCast(SI->getPointerOperand(),
                                      IRB.getPtrTy(TaintAddrSpace));
  SI->setOperand(SI->getPointerOperandIndex(), Ptr);

  // Remove the tainted data wrap.
  SI->setOperand(0, WrappedData);

  return true;
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
      Op = CreateTaint(IRB, Op, &I);
      Op = IRB.CreateTrunc(Op, T);
      return Op;
    }
  }

  // Handle scalar floating-point (half/bfloat=16, float=32, double=64): bitcast
  // to a same-width integer, taint, bitcast back. fp128 / x86_fp80 (> 64 bits)
  // fall through to the LFENCE fallback.
  if (T->isFloatingPointTy()) {
    unsigned Bits = T->getPrimitiveSizeInBits().getFixedValue();
    if (Bits != 0 && Bits <= 64)
      return taintViaIntBitcast(IRB, Op, T, Bits, &I);
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
      V = CreateTaint(IRB, V, &I);
      NewOp = IRB.CreateInsertValue(NewOp, V, {Idx});
    }
    return NewOp;
  }

  // Handle fixed-width vector types.
  if (auto *VT = dyn_cast<FixedVectorType>(T)) {
    unsigned Bits = VT->getPrimitiveSizeInBits().getFixedValue();

    // 128-bit vectors are matched directly by the TAINT128r td patterns.
    if (Bits == 128)
      return &I;

    // GPR-sized vectors (e.g. <4 x i8>=32, <8 x i8>=64, <2 x i32>=64): you
    // can't zext a vector (that re-lays-out the lanes), so bitcast the whole
    // value to a same-width integer, taint that scalar (GPR-class, matches
    // TAINT32r / TAINT64r), then bitcast back. Sub-32-bit widths still need the
    // zext/trunc round trip to reach i32.
    //
    // Exclude pointer-element vectors (gather/scatter addresses, `<N x ptr>`):
    // getPrimitiveSizeInBits() reports 0 for them (so `Bits` would be 0 and
    // getIntNTy(0) asserts), and `bitcast <N x ptr>` to an integer is illegal
    // anyway. They fall through to the LFENCE fallback.
    if (Bits != 0 && Bits <= 64 && !VT->getElementType()->isPointerTy())
      return taintViaIntBitcast(IRB, Op, T, Bits, &I);

    // TODO: 256/512-bit (YMM/ZMM) -> split into 128-bit halves + TAINT128r each
    // (no TAINT256r/TAINT512r form yet). Falls through to LFENCE below.
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

static bool insertChecks(Function &F) {
  using namespace llvm::PatternMatch;

  bool Changed = true;
  auto &Ctx = F.getContext();

  // Create a fail block.
  auto *FailB = BasicBlock::Create(Ctx, "soundspec.check.fail", &F);
  IRBuilder<> IRB(FailB);
  IRB.CreateIntrinsic(Intrinsic::trap, {});
  IRB.CreateUnreachable();

  SmallVector<Instruction *> Todo;
  for (auto &I : instructions(F)) {
    if (!match(&I, m_Intrinsic<Intrinsic::soundspec_delay_if_zero>(m_Value())))
      continue;
    Todo.push_back(&I);
  }

  // For each llvm.soundspec.delay_if_zero intrinsic, split the block at the
  // intrinsic + add a check. Re-read the operand each iteration: a prior
  // iteration's RAUW may have rewritten this call's operand to bypass an
  // already-erased earlier `delay_if_zero` in a chain. Caching the operand
  // at gather time would leave it dangling and crash the verifier.
  for (Instruction *I : Todo) {
    Value *Op = I->getOperand(0);
    assert(I != Op);
    LLVM_DEBUG(dbgs() << "before check:\n" << *I->getParent() << "\n");
    auto *B = I->getParent();
    auto *NewB = B->splitBasicBlock(I);
    I->replaceAllUsesWith(Op);
    I->eraseFromParent();
    B->getTerminator()->eraseFromParent();
    IRBuilder<> IRB(B);
    assert(!B->getTerminator());
    auto *IsZero = IRB.CreateICmpEQ(Op, Constant::getNullValue(Op->getType()));
    IRB.CreateCondBr(IsZero, FailB, NewB);
    LLVM_DEBUG(dbgs() << "after check:\n" << *B << "\n" << *NewB << "\n");
  }

  verifyFunction(F, &errs());

  return Changed;
}

bool X86LowerSoundSpecIntrinsicsLegacyPass::runOnFunction(Function &F) {
  bool ChangedOverall = false;

  if (StripTaint) {
    return stripTaint(F);
  }

  if (InsertChecks) {
    ChangedOverall |= insertChecks(F);
  }

  using namespace llvm::PatternMatch;
  bool Changed;
  do {
    Changed = false;
    for (auto &I : make_early_inc_range(instructions(F))) {
      // Rewrite loads.
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        Changed |= rewriteLoadTaintOut(LI);
        continue;
      }

      // Rewrite stores.
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        Changed |= rewriteStoreTaintOut(SI);
        continue;
      }

      // Rewrite taint intrinsics.
      Value *Op;
      if (match(&I, m_Intrinsic<Intrinsic::soundspec_taint>(m_Value(Op)))) {
        // Remove if dead.
        if (I.uses().empty()) {
          I.eraseFromParent();
          Changed = true;
          continue;
        }

        // Rewrite non-dead taint.
        if (auto *NewI = rewriteTaintInst(I, Op); NewI != &I) {
          I.replaceAllUsesWith(NewI);
          I.eraseFromParent();
          Changed = true;
          continue;
        }
      }
    }
    ChangedOverall |= Changed;
  } while (Changed); 

  return ChangedOverall;
}