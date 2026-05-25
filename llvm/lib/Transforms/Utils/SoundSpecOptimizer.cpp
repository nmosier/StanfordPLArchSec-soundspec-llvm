#include "llvm/Transforms/Utils/SoundSpecOptimizer.h"

#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Function.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/UnsafeScope.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;

static SmallSet<Metadata *, 16> getUnsafeScopeIDs(Function &F) {
  SmallSet<Metadata *, 16> IDSet;
  for (auto *B : post_order(&F)) {
    for (auto &I : *B) {
      if (auto *IDs = I.getMetadata(LLVMContext::MD_unsafe)) {
        assert(IDs->getNumOperands() == 1);
        IDSet.insert(IDs->getOperand(0));
      }
    }
  }
  return IDSet;
}

static void optimizeNonSpeculativeScopes(Function &F) {
  auto &Ctx = F.getContext();

  // Unsafe scopes with no speculation primitives can always be optimized away.
  auto IDSet = getUnsafeScopeIDs(F);

  for (auto *ID : IDSet) {
    using namespace llvm::PatternMatch;

    UnsafeScope Scope(F, ID);

    // Check if it contains any speculation primitive.
    auto HasSpecPrim = any_of(Scope.instructions(), [](const Instruction *I) {
      return isSpeculationPrimitive(*I);
    });
    if (HasSpecPrim)
      continue;
    auto HasTransmitter =
        any_of(Scope.instructions(),
               [](const Instruction *I) { return isTransmitter(*I); });
    if (HasTransmitter)
      continue;

    // Eliminate each tie.
    for (auto *I : Scope.instructions()) {
      Value *Gen = nullptr;
      Value *Dep = nullptr;
      if (!match(I, m_Intrinsic<Intrinsic::unsafe_shadow_tie>(m_Value(Gen),
                                                              m_Value(Dep))))
        continue;

      // Remove the @llvm.unsafe.shadow.tie(Gen, Dep) call.
      // Replace it with @llvm.unsafe.shadow.dep(Gen, Dep).
      IRBuilder<> IRB(I);
      auto *ReplGen = IRB.CreateIntrinsic(Intrinsic::unsafe_shadow_dep, {Gen, Dep});

      // Rewrite uses of gen.
      SSAUpdater SSAU;
      SSAU.Initialize(IntegerType::get(Ctx, 32), "tie.elim");
      SSAU.AddAvailableValue(cast<Instruction>(Gen)->getParent(), Gen);
      SSAU.AddAvailableValue(I->getParent(), ReplGen);

      // Remove the @tie.
      I->eraseFromParent();

      for (auto &U : Gen->uses())
        SSAU.RewriteUseAfterInsertions(U);
    }

    // Replace @llvm.unsafe.shadow.gen() with zero.
    for (auto *I : Scope.instructions()) {
      if (!match(I, m_Intrinsic<Intrinsic::unsafe_shadow_gen>()))
        continue;
      I->replaceAllUsesWith(ConstantInt::get(IntegerType::get(Ctx, 32), 0));
    }
  }

  // Positively identify scopes that *do* have speculation primitives.
  SmallSet<Metadata *, 16> ScopesWithSpecPrims;
  for (auto &I : instructions(F)) {
    if (!isSpeculationPrimitive(I))
      continue;
    auto *IDs = I.getMetadata(LLVMContext::MD_unsafe);
    if (!IDs)
      continue;
    for (Metadata *ID : IDs->operands())
      ScopesWithSpecPrims.insert(ID);
  }

  // If we see a @llvm.unsafe.shadow.gen() on a scope *without* a speculation
  // primitive, then simply accumulate the actual @llvm.unsafe.shadow.tie()'s
  // into deps.
}
 
void llvm::optimizeUnsafeScopes(Function &F) {
    optimizeNonSpeculativeScopes(F);
}