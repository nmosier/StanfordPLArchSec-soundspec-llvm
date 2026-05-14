//===- LowerUnsafeInit.cpp - Lower !unsafe.init to !unsafe scope ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// [soundspec] For each alloca carrying `!unsafe.init`, identify its
// initialization stores and tag them (and the alloca itself) with the
// equivalent `!unsafe` scope metadata. Runs after SROA so that
// per-field allocas of a split aggregate have already inherited the
// `!unsafe.init` marker.
//
//===----------------------------------------------------------------------===//

// TODO: Should check every use of alloca or alloca-derived pointers
// to enusre that they are properly initialized or marked !unsafe.

#include "llvm/Transforms/Scalar/LowerUnsafeInit.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/UnsafeScope.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Support/ModRef.h"
#include "llvm/ADT/DenseMap.h"

#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "lower-unsafe-init"

STATISTIC(NumAllocasLowered,
          "[soundspec] number of allocas whose !unsafe.init was lowered");
STATISTIC(NumStoresTagged,
          "[soundspec] number of initialization stores tagged with !unsafe");
STATISTIC(NumInstsTagged,
          "[soundspec] number of initialization accesses tagged with !unsafe");
STATISTIC(NumAllocasEverFullyInitialized,
          "[soundspec] number of allocas that are ever fully initialized at "
          "any program point");
STATISTIC(NumCallBasesTagged, "[soundspec] number of CallBase tagged with !unsafe");
STATISTIC(NumIntrinsicsTagged,
          "[soundspec] number of IntrinsicInst tagged with !unsafe");
STATISTIC(NumInstsTaggedFullyInit,
          "[soundspec] number of instructions tagged with !unsafe for allocas "
          "that are fully initialized at some point");


namespace {

struct P {
  using T = SmallBitVector;

  T In;
  T Out;

  auto sig() const {
    return std::make_pair(In.count(), Out.count());
  }
};

static bool accumulateConstantOffset(const Value *Ptr, const AllocaInst *AI,
                                     const DataLayout &DL, uint64_t &Offset) {
  if (Ptr == AI) {
    Offset = 0;
    return true;
  }

  auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
  if (!GEP)
    return false;

  if (!accumulateConstantOffset(GEP->getPointerOperand(), AI, DL, Offset))
    return false;

  APInt MyOffset(DL.getIndexSizeInBits(GEP->getPointerAddressSpace()), 0);
  if (!GEP->accumulateConstantOffset(DL, MyOffset))
    return false;

  Offset += MyOffset.getLimitedValue();
  return true;
}

template <typename OutputIt>
static OutputIt getInitPointerAndSize(const Instruction &I, const DataLayout &DL,
                                  OutputIt Out) {
  // Handle stores.
  if (auto *SI = dyn_cast<StoreInst>(&I)) {
    auto Size = DL.getTypeStoreSizeInBits(SI->getValueOperand()->getType());
    auto *Ptr = SI->getPointerOperand();
    *Out++ = std::make_pair(Ptr, Size);
  }

  // Handle memset, memcpy, memmove.
  if (auto *MI = dyn_cast<MemIntrinsic>(&I)) {
    if (auto Len = MI->getLengthInBytes()) {
      auto Size = Len->getLimitedValue() * 8;
      auto *Ptr = MI->getDest();
      *Out++ = std::make_pair(Ptr, Size);
    }
  }

  // Handle sret calls.
  if (auto *C = dyn_cast<CallBase>(&I)) {
    for (unsigned I = 0; I < C->arg_size(); ++I) {
      auto *Ptr = C->getArgOperand(I);
      Attribute Attr = C->getParamAttr(I, Attribute::StructRet);
      if (!Attr.isValid())
        continue;
      assert(Ptr->getType()->isPointerTy());
      auto *T = Attr.getValueAsType();
      auto Size = DL.getTypeStoreSizeInBits(T);
      *Out++ = std::make_pair(Ptr, Size);
    }
  }

  return Out;
}

template <typename OutputIt>
static OutputIt getInitRanges(const Instruction &I, const AllocaInst *AI,
                         const DataLayout &DL, OutputIt Out) {
  SmallVector<std::pair<const Value *, uint64_t>> PtrSizes;
  getInitPointerAndSize(I, DL, std::back_inserter(PtrSizes));

  for (auto [Ptr, Size] : PtrSizes) {
    uint64_t Offset;
    if (!accumulateConstantOffset(Ptr, AI, DL, Offset))
      continue;
    auto First = Offset;
    auto Last = First + Size;
    *Out++ = std::make_pair(First, Last);
  }

  return Out;
}

static void transferInstruction(const Instruction &I, const AllocaInst *AI,
                                const DataLayout &DL, SmallBitVector &BV) {
  SmallVector<std::pair<uint64_t, uint64_t>> Ranges;
  getInitRanges(I, AI, DL, std::back_inserter(Ranges));
  for (auto [First, Last] : Ranges)
    BV.set(First, Last);
}

static void transferBlock(const BasicBlock &B, const AllocaInst *AI,
                          const DataLayout &DL, SmallBitVector &BV) {
  for (auto &I : B)
    transferInstruction(I, AI, DL, BV);
}

static MemoryLocation getAllocaMemLoc(const AllocaInst *AI,
                                      const DataLayout &DL) {
  auto TS = AI->getAllocationSize(DL);
  auto LS =
      TS ? LocationSize::precise(*TS) : LocationSize::beforeOrAfterPointer();
  return MemoryLocation(AI, LS);
}

static ModRefInfo getAllocaModRef(AAResults &AA, const Instruction &I,
                                  const AllocaInst *AI, const DataLayout &DL) {
  return AA.getModRefInfo(&I, getAllocaMemLoc(AI, DL));
}

// Returns whether uninitialized alloca was accessed.
static bool checkAccess(const Instruction &I, AAResults &AA,
                        const AllocaInst *AI, const DataLayout &DL,
                        const SmallBitVector &BV) {
  // If the allocation was fully initialized, then all accesses will always be
  // fully initialized.
  if (BV.all())
    return false;

  // Otherwise, this instruction accessed the uninitialized alloca if
  // it aliases with it per AA.
  return isModOrRefSet(getAllocaModRef(AA, I, AI, DL));
}

static void identifyInitializationStores(Function &F, AllocaInst *AI, AAResults &AA) {
  auto &DL = F.getDataLayout();
  auto Size = AI->getAllocationSizeInBits(DL);
  assert(Size && "VLAs are not allowed to have !unsafe.init metadata!");
  using T = P::T;

  auto Top = T(*Size, true);
  auto Bot = T(*Size, false);
  DenseMap<BasicBlock *, P> Map;
  for (auto &B : F)
    Map[&B].In = Top;
  Map[&F.getEntryBlock()].In = Bot;

  // Compute the initialization at each program point.
  bool Changed;
  do {
    Changed = false;

    for (auto &B : F) {
      auto &P = Map[&B];
      auto OldSig = P.sig();

      // Compute in from outs of previous (intersection).
      for (auto *Pred : llvm::predecessors(&B))
        P.In &= Map[Pred].Out;

      // Compute out from in.
      P.Out = P.In;
      transferBlock(B, AI, DL, P.Out);

      // Check if changed.
      Changed |= (P.sig() != OldSig);
    }
  } while (Changed);

  // STATS: Was this alloca ever fully initialized?
  bool EverFullyInit = llvm::any_of(F, [&Map](auto &B) -> bool {
    return Map[&B].Out.all();
  });
  if (EverFullyInit)
    ++NumAllocasEverFullyInitialized;

  for (BasicBlock &B : F) {
    auto BV = Map[&B].In;

    for (Instruction &I : B) {
      // Check if this instruction accessed uninitialized memory.
      if (checkAccess(I, AA, AI, DL, BV)) {
        // Mark this access as !unsafe.
        auto *MD =
            unsafe::mergeMetadata(I.getMetadata(LLVMContext::MD_unsafe),
                                  AI->getMetadata(LLVMContext::MD_unsafe));
        assert(MD);
        I.setMetadata(LLVMContext::MD_unsafe, MD);

        ++NumInstsTagged;
        if (I.mayWriteToMemory())
          ++NumStoresTagged;
        if (isa<CallBase>(&I))
          ++NumCallBasesTagged;
        if (isa<IntrinsicInst>(&I))
          ++NumIntrinsicsTagged;
        if (EverFullyInit)
          ++NumInstsTaggedFullyInit;
      }

      // Step forward over the instruction.
      transferInstruction(I, AI, DL, BV);
    }
  }
}

/// Process a single function: find allocas with `!unsafe.init`, tag their
/// init stores, propagate the scope onto the alloca itself, and drop the
/// `!unsafe.init` marker.
static bool lowerUnsafeInit(Function &F, AAResults &AA) {
  for (Instruction &I : llvm::instructions(F)) {
    auto *AI = dyn_cast<AllocaInst>(&I);
    if (!AI)
      continue;
    auto *MD = AI->getMetadata(LLVMContext::MD_unsafe_init);
    if (!MD)
      continue;
    assert(MD->getNumOperands() &&
           "Expected !unsafe.init metadata to have >0 operands!");
    AI->setMetadata(LLVMContext::MD_unsafe_init, nullptr);
    assert(!AI->getMetadata(LLVMContext::MD_unsafe) &&
           "Expected !unsafe.init alloca to not have !unsafe metadata!");
    AI->setMetadata(LLVMContext::MD_unsafe, MD);

    // Identify any initialization stores.
    identifyInitializationStores(F, AI, AA);

    ++NumAllocasLowered;
  }

  return true;
}

} // end anonymous namespace

PreservedAnalyses LowerUnsafeInitPass::run(Function &F,
                                           FunctionAnalysisManager &AM) {
  AAResults &AA = AM.getResult<AAManager>(F);
  if (!lowerUnsafeInit(F, AA))
    return PreservedAnalyses::all();

  // Only metadata changes; CFG is untouched.
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
