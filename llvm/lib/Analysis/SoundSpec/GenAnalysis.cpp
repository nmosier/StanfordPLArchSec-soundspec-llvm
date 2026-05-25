//===- GenAnalysis.cpp - SoundSpec gen-token analysis ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Skeleton SoundSpec analysis pass. Currently a no-op — the `run` method
// returns an empty Result. Real analysis logic will be filled in here.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/SoundSpec/GenAnalysis.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"

using namespace llvm;
using namespace llvm::soundspec;

AnalysisKey GenAnalysis::Key;

#if 0
namespace {

class GenAnalysisImpl {
  Function &F;
  LLVMContext &Ctx;
  Metadata *ID;
  Value *IDVal;
  MapVector<PHINode *, bool> Cache;
  SmallVector<Instruction *> Gens;

  bool getGen(Value *V);

public:
  GenAnalysisImpl(Function &F, Metadata *ID)
      : F(F), Ctx(F.getContext()), ID(ID), IDVal(MetadataAsValue::get(Ctx, ID)) {}

  void run();

  auto begin() { return Gens.begin(); }
  auto end() { return Gens.end(); }
};

} // namespace

void GenAnalysisImpl::run() {
  for (auto &I : instructions(F))
    if (getGen(&I))
      Gens.push_back(&I);
}

bool GenAnalysisImpl::getGen(Value *V) {
  using namespace llvm::PatternMatch;

  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return false;

  if (match(I, m_Intrinsic<Intrinsic::unsafe_shadow_gen>(m_Specific(IDVal))))
    return true;

  auto *PHI = dyn_cast<PHINode>(I);
  if (!PHI)
    return false;

  // Have we cached this PHI node?
  auto Result = Cache.insert({PHI, true});
  bool &IsPHIGen = Result.first->second;
  if (!Result.second)
    return IsPHIGen;

  // If not, recompute.
  IsPHIGen = all_of(PHI->incoming_values(), [&](auto *Incoming) {
    return getValue(Incoming);
  });

  return IsPHIGen;
}

GenAnalysis::Result GenAnalysis::run(Function &F, FunctionAnalysisManager &AM) {
  // Enumerate all scope IDs.
  SmallSet<Metadata *, 4> IDSet;
  for (auto &I : instructions(F))
    if (auto *IDs = I.getMetadata(LLVMContext::MD_unsafe))
      copy(IDs->operands(), std::inserter(IDSet, IDSet.end()));

  // Get the gens for each scope ID.
  for (auto *ID : IDSet) {
    GenAnalysisImpl Impl(F, ID);
    Impl.run();
    
  }
}
#endif