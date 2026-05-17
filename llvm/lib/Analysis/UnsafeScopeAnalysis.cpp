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

//===----------------------------------------------------------------------===//
// UnsafeScopeAnalysis (new pass manager)
//===----------------------------------------------------------------------===//

AnalysisKey UnsafeScopeAnalysis::Key;

UnsafeScopeInfo UnsafeScopeAnalysis::run(Function &F,
                                         FunctionAnalysisManager &AM) {
  std::abort();
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
