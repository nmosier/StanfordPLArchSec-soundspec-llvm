#include "llvm/Transforms/Utils/SoundSpecOptimizer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/UnsafeScope.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/PatternMatch.h"
#include <algorithm>

using namespace llvm;

static SmallSet<Metadata *, 16> getUnsafeScopeIDs(Function &F) {
  SmallSet<Metadata *, 16> IDSet;
  for (auto *B : post_order(&F)) {
    for (auto &I : *B) {
      if (auto *IDs = I.getMetadata(LLVMContext::MD_unsafe)) {
        for (Metadata *ID : IDs->operands()) {
          IDSet.insert(ID);
        }
      }
    }
  }
  return IDSet;
}

#if 0
static void optimizeUnsafeScopeIntraBlockPure(
    BasicBlock &B, iterator_range<BasicBlock::iterator> Range, Metadata *ID) {
  // The only instructions that should block this optimization are
  // transmitters and speuclation primitives.
  using namespace llvm::PatternMatch;
  auto &Ctx = B.getContext();
  assert(!Range.empty());
  // Does it have a leading gen?
  auto It = Range.begin();
  auto &Gen = *It++;
  auto *IDVal = MetadataAsValue::get(Ctx, ID);
  if (!match(&Gen, m_Intrinsic<Intrinsic::unsafe_shadow_gen>(m_Specific(IDVal))))
    return;
  SmallVector<Value *> TiedDeps;
  while (It != Range.end()) {
    auto &Tie = *It++;
    Value *TiedDep = nullptr;
    if (!match(&Tie, m_Intrinsic<Intrinsic::unsafe_shadow_tie>(
                        m_Specific(&Gen), m_Value(TiedDep))))
      return;
    TiedDeps.push_back(TiedDep);
  }

  // Mask in tied deps.
  IRBuilder<> IRB(&Gen);
  Value *Acc = IRB.getInt32(0);
  for (auto *TiedDep : TiedDeps)
    Acc = getUnsafeShadowDep(IRB, Acc, TiedDep);

  // Replace all uses of the `gen` with `Acc`.
  Gen.replaceAllUsesWith(Acc);
  B.erase(Range.begin(), Range.end());
}
#else
static void optimizeUnsafeScopeIntraBlockPure(
    BasicBlock &B, iterator_range<BasicBlock::iterator> Range, Metadata *ID) {
  assert(!Range.empty());
  using namespace llvm::PatternMatch;
  auto &Ctx = B.getContext();
  auto *IDVal = MetadataAsValue::get(Ctx, ID);

  // Bail if the scope contains a transmitter, speculative primitive, or call.
  for (auto &I : Range) {
    auto *II = dyn_cast<IntrinsicInst>(&I);
    if (II) {
      switch (II->getIntrinsicID()) {
      case Intrinsic::unsafe_shadow_gen:
      case Intrinsic::unsafe_shadow_tie:
        continue;
      }
    }
    if (isTransmitter(I) || isSpeculationPrimitive(I) ||
        unsafe::isRelevantCall(I))
      return;
  }

  // Fold each instruction.
  SmallVector<Instruction *> Ties;
  SmallSet<Instruction *, 4> Gens;
  for (auto &I : make_early_inc_range(Range)) {
    // Recognize @llvm.unsafe.shadow.gen(%id).
    if (match(&I,
              m_Intrinsic<Intrinsic::unsafe_shadow_gen>(m_Specific(IDVal)))) {
      Gens.insert(&I);
      continue;
    }

    // Recognize @llvm.unsafe.shadow.tie(%id, %dep).
    Instruction *TieGen;
    Value *TieDep;
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_tie>(
                      m_Instruction(TieGen), m_Value(TieDep))) &&
        Gens.contains(TieGen)) {
      Ties.push_back(&I);
      continue;
    }

    // If this is a transmitter, speculation primitive, or call, bail.
    if (isSpeculationPrimitive(I) || isTransmitter(I) ||
        unsafe::isRelevantCall(I) || I.isTerminator())
      return;

    // If this uses any gen, then bail.
    for (auto *Gen : Gens)
      if (is_contained(I.operands(), Gen))
        return;
  }

  // Insert @gen replacement at end of range.
  IRBuilder<> IRB(&B, Range.end());
  Value *Acc = IRB.getInt32(0);
  for (auto *Tie : Ties) {
    auto *Dep = cast<IntrinsicInst>(Tie)->getArgOperand(1);
    Acc = getUnsafeShadowDep(IRB, Acc, Dep);
  }

  // Remove all ties.
  for (auto *Tie : Ties)
    Tie->eraseFromParent();

  // Replace all gen uses with `Acc`.
  for (auto *Gen : Gens) {
    Gen->replaceAllUsesWith(Acc);
    Gen->eraseFromParent();
  }
}
#endif

static void normalizeIntraBlockUnsafeScope(
    BasicBlock &B, iterator_range<BasicBlock::iterator> Range, Metadata *ID) {
  // The normal form looks like this:
  // @llvm.unsafe.shadow.gen()...
  // TODO!
}

static void optimizeUnsafeScopeIntraBlock(
    BasicBlock &B, iterator_range<BasicBlock::iterator> Range, Metadata *ID) {
  // Try to put the block in formal form.
  normalizeIntraBlockUnsafeScope(B, Range, ID);
  
  // Check if there any speculation primitives or transmitters.
  if (none_of(Range, [](const Instruction &I) -> bool {
        return isSpeculationPrimitive(I) || isTransmitter(I);
      })) {
    optimizeUnsafeScopeIntraBlockPure(B, Range, ID);
  }
}

static void optimizeUnsafeScope(Function &F, Metadata *ID, UnsafeScope &Scope) {
  assert(!Scope.instructions().empty());

  auto *B = Scope.instructions().front()->getParent();
  if (all_of(Scope.instructions(), [B](const Instruction *I) -> bool {
        return B == I->getParent();
      })) {
    // The block is intra-scope.
    // Find the begin/end.
    BasicBlock::iterator It;
    for (It = B->begin(); It != B->end(); ++It) {
      auto *I = &*It;
      if (is_contained(Scope.instructions(), I))
        break;
    }
    auto First = It;
    for (; It != B->end(); ++It) {
      auto *I = &*It;
      if (!is_contained(Scope.instructions(), I))
        break;
    }
    auto Last = It;
    optimizeUnsafeScopeIntraBlock(*B, iterator_range(First, Last), ID);
  }

}

static void optimizeUnsafeInstructions(Function &F) {
  using namespace llvm::PatternMatch;
  for (auto &I : make_early_inc_range(instructions(F))) {
    Value *Dep;
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_xmit>(m_Value(Dep)))) {
      // Can remove constant dependencies, since they're no-ops.
      // This can happen if we had a no-op unsafe scope that was eliminated.
      if (isa<Constant>(&I) || !I.getMetadata(LLVMContext::MD_soundspec_xmit)) {
        I.eraseFromParent();
        continue;
      }
    }

    // @llvm.unsafe.shadow.tie(0, %dep) - hmm actually this shouldn't happen,
    // but if it does, is still safe to delete.
    if (match(&I,
              m_Intrinsic<Intrinsic::unsafe_shadow_tie>(m_Zero(), m_Value())) ||
        match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_tie>(m_Value(), m_Zero()))) {
      I.eraseFromParent();
      continue;
    }

    // %x = ...
    // %y = @llvm.unsafe.shadow.arg(%x)
    // %z = @llvm.unsafe.shadow.embed(%x, %y)
    // Replace %z with %x, remove @llvm.unsafe.shadow.embed().
    // DCE should then take care of @llvm.unsafe.shadow.arg(). 
    Value *Data;
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_embed>(m_Value(Data),
                                                              m_Value(Dep))) &&
        match(Dep,
              m_Intrinsic<Intrinsic::unsafe_shadow_arg>(m_Specific(Data)))) {
      I.replaceAllUsesWith(Data);
      I.eraseFromParent();
    }
  }
}

void llvm::optimizeUnsafeScopes(Function &F) {
  // TODO: Consider running a fixpoint loop.

  optimizeUnsafeInstructions(F);

  auto IDs = getUnsafeScopeIDs(F);
  for (auto *ID : IDs) {
    UnsafeScope Scope(F, ID);
    optimizeUnsafeScope(F, ID, Scope);
  }

  // Simplify instructions that may have been rewritten
  // by unsafe scope optimization.
  for (auto *B : ReversePostOrderTraversal(&F))
    SimplifyInstructionsInBlock(B);

  optimizeUnsafeInstructions(F);
}

PreservedAnalyses SoundSpecOptimizerPass::run(Function &F,
                                              FunctionAnalysisManager &FAM) {
  optimizeUnsafeScopes(F);
  return PreservedAnalyses::none();
}