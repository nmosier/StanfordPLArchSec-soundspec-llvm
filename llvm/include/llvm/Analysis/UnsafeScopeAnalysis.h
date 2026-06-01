//===- UnsafeScopeAnalysis.h - SoundSpec unsafe-scope info ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides a per-function analysis that summarizes the
// SoundSpec unsafe scopes present in the IR. Each scope is identified by
// an i32 scope id carried on the SoundSpec unsafe intrinsics. The analysis
// result lets clients ask whether an instruction is inside a given scope,
// enumerate all scopes in the function, etc.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_UNSAFESCOPEANALYSIS_H
#define LLVM_ANALYSIS_UNSAFESCOPEANALYSIS_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"

namespace llvm {

class Function;
class Instruction;
class IntrinsicInst;
class raw_ostream;

/// Summary of the SoundSpec unsafe scopes within a single function.
///
/// Currently a thin skeleton: tracks the enter/exit intrinsic calls per
/// scope id. Build out as the analysis evolves.
class UnsafeScopeInfo {
public:
  using ScopeID = uint32_t;

  UnsafeScopeInfo() = default;
  UnsafeScopeInfo(UnsafeScopeInfo &&) = default;
  UnsafeScopeInfo &operator=(UnsafeScopeInfo &&) = default;
  UnsafeScopeInfo(const UnsafeScopeInfo &) = delete;
  UnsafeScopeInfo &operator=(const UnsafeScopeInfo &) = delete;

  std::optional<ScopeID> lookup(const Instruction *I) const;

  /// Handle invalidation. The analysis is preserved if and only if all
  /// analyses we depend on are preserved.
  LLVM_ABI bool invalidate(Function &F, const PreservedAnalyses &PA,
                           FunctionAnalysisManager::Invalidator &Inv);

  /// Debug print.
  LLVM_ABI void print(raw_ostream &OS) const;

private:
  friend class UnsafeScopeAnalysis;

  DenseMap<const Instruction *, ScopeID> InstToScopeID;

  template <typename Range>
  void populateScopeFromEnter(ScopeID ID, const Range &EnterInsts);

  template <typename Range>
  void populateScopeFromIn(ScopeID ID, const Range &InInsts);
};

/// New-pass-manager analysis: computes \c UnsafeScopeInfo for a function.
class UnsafeScopeAnalysis : public AnalysisInfoMixin<UnsafeScopeAnalysis> {
  friend AnalysisInfoMixin<UnsafeScopeAnalysis>;
  LLVM_ABI static AnalysisKey Key;

public:
  using Result = UnsafeScopeInfo;

  LLVM_ABI UnsafeScopeInfo run(Function &F, FunctionAnalysisManager &AM);
};

/// Printer pass for \c UnsafeScopeAnalysis.
class UnsafeScopePrinterPass : public PassInfoMixin<UnsafeScopePrinterPass> {
  raw_ostream &OS;

public:
  LLVM_ABI explicit UnsafeScopePrinterPass(raw_ostream &OS) : OS(OS) {}

  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // end namespace llvm

#endif // LLVM_ANALYSIS_UNSAFESCOPEANALYSIS_H
