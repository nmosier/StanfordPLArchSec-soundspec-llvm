//===- UnsafeScopeAnalysis.cpp - SoundSpec unsafe-scope info --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-function analysis that summarizes the SoundSpec unsafe scopes
// present in the IR. See UnsafeScopeAnalysis.h for the public interface.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/UnsafeScopeAnalysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/UnsafeScope.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/ADT/SmallSet.h"

using namespace llvm;

#define DEBUG_TYPE "unsafe-scope-analysis"

//===----------------------------------------------------------------------===//
// UnsafeScopeInfo
//===----------------------------------------------------------------------===//

std::optional<UnsafeScopeInfo::ScopeID>
UnsafeScopeInfo::lookup(const Instruction *I) const {
  auto It = InstToScopeID.find(I);
  if (It == InstToScopeID.end())
    return std::nullopt;
  return It->second;
}

bool UnsafeScopeInfo::invalidate(Function &, const PreservedAnalyses &PA,
                                 FunctionAnalysisManager::Invalidator &) {
  // Result is preserved if the function-level analyses are preserved.
  auto PAC = PA.getChecker<UnsafeScopeAnalysis>();
  return !(PAC.preserved() || PAC.preservedSet<AllAnalysesOn<Function>>());
}

void UnsafeScopeInfo::print(raw_ostream &OS) const { errs() << "TODO\n"; }

static std::optional<UnsafeScopeInfo::ScopeID>
getUnsafeIntrinicScopeID(const Instruction &I) {
  const IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return std::nullopt;
  switch (II->getIntrinsicID()) {
  case Intrinsic::unsafe_enter:
  case Intrinsic::unsafe_exit:
  case Intrinsic::unsafe_in:
  case Intrinsic::unsafe_out: {
    auto *ScopeVal = II->getOperand(II->getNumOperands() - 1);
    return cast<ConstantInt>(ScopeVal)->getLimitedValue();
  }
  default:
    return std::nullopt;
  }
}

template <typename Range>
void UnsafeScopeInfo::populateScopeFromEnter(ScopeID ID,
                                             const Range &EnterInsts) {
  SmallVector<Instruction *> Todo;
  SmallSet<Instruction *, 8> Seen;
  llvm::copy(EnterInsts, std::back_inserter(Todo));
  while (!Todo.empty()) {
    Instruction *I = Todo.pop_back_val();

    // Skip instructions we've already seen.
    if (!Seen.insert(I).second) {
      assert(!isUnsafeScopeEnter(*I) && "Should never see a llvm.unsafe.enter more than once!");
      continue;
    }

    // Stop searching along this path if it's an llvm.unsafe.exit.
    [[maybe_unused]] uint64_t ExitID;
    if (isUnsafeScopeExit(*I, &ExitID)) {
      assert(ID == ExitID && "Saw llvm.unsafe.exit for different scope!");
      continue;
    }

    // Add control-flow successors.
    if (Instruction *Succ = I->getNextNode()) {
      Todo.push_back(Succ);
      continue;
    }
    for (BasicBlock *Succ : llvm::successors(I->getParent()))
      Todo.push_back(&Succ->front());
  }

  // Add seen instructions in the scope into the map.
  for (Instruction *I : Seen) {
    [[maybe_unused]] auto Result = InstToScopeID.insert({I, ID});
    assert(Result.first->second == ID && "Instruction already added to another scope!");
  }
}

// TODO: This could be refactored to avoid redundancy with `populateScopeFromEnter`.
template <typename Range>
void UnsafeScopeInfo::populateScopeFromIn(ScopeID ID, const Range &InInsts) {
  SmallVector<Instruction *> Todo;
  SmallSet<Instruction *, 8> Seen;
  llvm::copy(InInsts, std::back_inserter(Todo));
  while (!Todo.empty()) {
    Instruction *I = Todo.pop_back_val();

    // Skip instructions if we've already seen them.
    if (!Seen.insert(I).second) {
      assert(!isUnsafeScopeIn(*I) &&
             "Should never see a llvm.unsafe.in more than once!");
      continue;
    }

    // Stop searching along this data-flow path if it's an llvm.unsafe.out.
    [[maybe_unused]] uint64_t OutID;
    if (isUnsafeScopeOut(*I, &OutID)) {
      assert(ID == OutID && "Saw llvm.unsafe.out for different scope!");
      continue;
    }

    // Add data-flow successors.
    for (Use &U : I->uses()) {
      Instruction *Succ = cast<Instruction>(U.getUser());
      Todo.push_back(Succ);
    }
  }

  // Add seen instructions in the scope to the map.
  for (Instruction *I : Seen) {
    [[maybe_unused]] auto Result = InstToScopeID.insert({I, ID});
    assert(Result.first->second == ID && "Instruction already added to another scope!");
  }
}

//===----------------------------------------------------------------------===//
// UnsafeScopeAnalysis (new pass manager)
//===----------------------------------------------------------------------===//

AnalysisKey UnsafeScopeAnalysis::Key;

UnsafeScopeInfo UnsafeScopeAnalysis::run(Function &F,
                                         FunctionAnalysisManager &AM) {
  UnsafeScopeInfo Info;
  using ID = UnsafeScopeInfo::ScopeID;

  // Identify all llvm.unsafe.enter's.
  DenseMap<ID, SmallSet<Instruction *, 2>> Enters, Ins;
  for (Instruction &I : llvm::instructions(F)) {
    uint64_t ID;
    if (isUnsafeScopeEnter(I, &ID))
      Enters[ID].insert(&I);
    if (isUnsafeScopeIn(I, &ID))
      Ins[ID].insert(&I);
  }

  // Second, find the instructions in each scope
  // (control-flow).
  for (auto &[ID, Enter] : Enters)
    Info.populateScopeFromEnter(ID, Enter);

  // Third, find the instructions in each scope via data-flow.
  for (auto &[ID, In] : Ins)
    Info.populateScopeFromIn(ID, In);

  LLVM_DEBUG(Info.print(dbgs()));
  return Info;
}

//===----------------------------------------------------------------------===//
// UnsafeScopePrinterPass
//===----------------------------------------------------------------------===//

PreservedAnalyses UnsafeScopePrinterPass::run(Function &F,
                                              FunctionAnalysisManager &AM) {
  OS << "UnsafeScopeAnalysis for function '" << F.getName() << "':\n";
  AM.getResult<UnsafeScopeAnalysis>(F).print(OS);
  return PreservedAnalyses::all();
}
