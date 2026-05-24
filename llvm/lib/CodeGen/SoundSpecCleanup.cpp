//===- SoundSpecCleanup.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// SoundSpec cleanup pass. See SoundSpecCleanup.h.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/SoundSpecCleanup.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/IRSimilarityIdentifier.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/CodeGen/Passes.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/UnsafeScope.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#define DEBUG_TYPE "soundspec-cleanup"

using namespace llvm;

namespace {

SmallSet<Metadata *, 4> getIDSet(const Function &F) {
  SmallSet<Metadata *, 4> IDSet;
  for (auto &I : instructions(F))
    if (auto *IDs = I.getMetadata(LLVMContext::MD_unsafe))
      copy(IDs->operands(), std::inserter(IDSet, IDSet.end()));
  return IDSet;
}

bool mergeScopes(DenseMap<Instruction *, SmallSet<Metadata *, 2>> &Extents) {
  auto It = find_if(Extents, [](auto &P) { return P.second.size() > 1; });
  if (It == Extents.end())
    return false;
  auto IDsIt = It->second.begin();
  auto Get = [&IDsIt] { return *IDsIt++; };
  auto *A = Get();
  auto *B = Get();
  for (auto &[I, IDs] : Extents)
    if (IDs.erase(B))
      IDs.insert(A);
  return true;
}

void combineScopes(Function &F, const SmallSet<Metadata *, 4> &IDSet) {
  auto &Ctx = F.getContext();

  DenseMap<Instruction *, SmallSet<Metadata *, 2>> Extents;
  for (auto *ID : IDSet) {
    UnsafeScope Scope(F, ID);
    for (auto *I : Scope.instructions())
      Extents[I].insert(ID);
  }

  while (mergeScopes(Extents)) {
  }

  for (auto &I : instructions(F)) {
    if (!I.getMetadata(LLVMContext::MD_unsafe) &&
        !I.getMetadata(LLVMContext::MD_safe))
      continue;
    auto ExtentsIt = Extents.find(&I);
    if (ExtentsIt == Extents.end()) {
      I.setMetadata(LLVMContext::MD_unsafe, nullptr);
      I.setMetadata(LLVMContext::MD_safe, MDNode::get(Ctx, {}));
      continue;
    }
    auto &Set = ExtentsIt->second;
    assert(Set.size() == 1);
    auto *ID = *Set.begin();
    auto *IDs = MDNode::get(Ctx, {ID});
    I.setMetadata(LLVMContext::MD_unsafe, IDs);
    I.setMetadata(LLVMContext::MD_safe, nullptr);

    if (auto *II = dyn_cast<IntrinsicInst>(&I);
        II && II->getIntrinsicID() == Intrinsic::unsafe_shadow_gen)
      II->setArgOperand(0, MetadataAsValue::get(Ctx, ID));
  }
}

void createUnsafeBegin(Instruction *I, Metadata *ID) {
  auto &Ctx = I->getContext();
  assert(I && (!isa<PHINode, AllocaInst>(I)));
  IRBuilder<> IRB(I);
  auto *Begin = IRB.CreateIntrinsic(Intrinsic::unsafe_begin, {});
  Begin->setMetadata(LLVMContext::MD_unsafe, unsafe::getScopeSet(Ctx, {ID}));
}

CallInst *createUnsafeEnd(Instruction *I, Metadata *ID) {
  auto &Ctx = I->getContext();
  assert(I && (!isa<PHINode, AllocaInst>(I)));
  IRBuilder<> IRB(I);
  auto *End = IRB.CreateIntrinsic(Intrinsic::unsafe_end, {});
  End->setMetadata(LLVMContext::MD_unsafe, unsafe::getScopeSet(Ctx, {ID}));
  return End;
}

LandingPadInst *getLPI(BasicBlock *B) {
  for (auto &I : *B)
    if (auto *LPI = dyn_cast<LandingPadInst>(&I))
      return LPI;
  return nullptr;
}

void fixupPHIs(BasicBlock *B) {
  SmallSet<BasicBlock *, 4> Preds;
  copy(predecessors(B), std::inserter(Preds, Preds.end()));
  for (auto &I : *B) {
    auto *PHI = dyn_cast<PHINode>(&I);
    if (!PHI)
      break;
    SmallVector<BasicBlock *> ToRemove;
    for (auto *Block : PHI->blocks())
      if (!Preds.contains(Block))
        ToRemove.push_back(Block);
    for (auto *Block : ToRemove)
      PHI->removeIncomingValue(Block);
  }
}

BasicBlock *robustSplitEdgeInvoke(BasicBlock *From, BasicBlock *OrigTo,
                                  InvokeInst *Invoke) {
  auto *OrigLPI = getLPI(OrigTo);
  assert(OrigLPI);
  auto *F = From->getParent();

  auto *Join = OrigTo->splitBasicBlock(std::next(OrigLPI->getIterator()));
  auto *TheirBlock = OrigTo;
  ValueToValueMapTy VMap;
  auto *OurBlock = CloneBasicBlock(TheirBlock, VMap);
  OurBlock->insertInto(F);
  Invoke->setUnwindDest(OurBlock);

  IRBuilder<> JoinIRB(&Join->front());
  auto NewBlocks = {OurBlock, TheirBlock};
  assert(OurBlock->size() == TheirBlock->size());
  for (auto &TheirI : *TheirBlock) {
    auto *Ty = TheirI.getType();
    if (Ty->isVoidTy())
      continue;
    Value *OurI = VMap[&TheirI];
    auto *PHI = JoinIRB.CreatePHI(Ty, 2);
    TheirI.replaceAllUsesWith(PHI);
    PHI->addIncoming(OurI, OurBlock);
    PHI->addIncoming(&TheirI, TheirBlock);
  }

  for (auto *B : NewBlocks)
    fixupPHIs(B);

  return OurBlock;
}

BasicBlock *robustSplitEdge(BasicBlock *From, BasicBlock *To) {
  if (auto *New = SplitEdge(From, To))
    return New;

  if (auto *Invoke = dyn_cast<InvokeInst>(From->getTerminator());
      Invoke && Invoke->getUnwindDest() == To) {
    return robustSplitEdgeInvoke(From, To, Invoke);
  }

  report_fatal_error("Failed to split for unknown reason");
}

using InstructionPred = function_ref<bool(Instruction *)>;

void instrumentUnsafeBegin(Instruction *I, Metadata *ID,
                           InstructionPred InScope) {
  if (auto *Pred = I->getPrevNode()) {
    if (!InScope(Pred))
      createUnsafeBegin(I, ID);
    return;
  }

  auto *B = I->getParent();
  auto InScopeB = [&](BasicBlock *B) { return InScope(B->getTerminator()); };

  if (none_of(predecessors(B), InScopeB)) {
    createUnsafeBegin(I, ID);
    return;
  }

  SmallVector<BasicBlock *> Preds;
  copy(predecessors(B), std::back_inserter(Preds));
  for (auto *PredB : Preds) {
    if (InScopeB(PredB))
      continue;
    auto *NewB = robustSplitEdge(PredB, B);
    assert(NewB);
    createUnsafeBegin(NewB->getTerminator(), ID);
  }
}

void instrumentUnsafeEnd(Instruction *I, Metadata *ID, InstructionPred InScope,
                         SmallVectorImpl<CallInst *> &Ends) {
  if (auto *Succ = I->getNextNode()) {
    if (!InScope(Succ))
      Ends.push_back(createUnsafeEnd(Succ, ID));
    return;
  }

  auto *B = I->getParent();
  auto InScopeB = [&](BasicBlock *B) { return InScope(&B->front()); };
  SmallVector<BasicBlock *> Succs;
  copy(successors(B), std::back_inserter(Succs));
  for (auto *SuccB : Succs) {
    if (InScopeB(SuccB))
      continue;
    if (SuccB->getUniquePredecessor()) {
      auto *SuccI = &*SuccB->getFirstInsertionPt();
      Ends.push_back(createUnsafeEnd(SuccI, ID));
      continue;
    }
    auto *NewB = robustSplitEdge(B, SuccB);
    Ends.push_back(createUnsafeEnd(&*NewB->getFirstInsertionPt(), ID));
  }
}

// FIXME: This is broken.
void computeGenSet(ArrayRef<Instruction *> Extent,
                   SmallVectorImpl<Instruction *> &Gens) {
  using namespace llvm::PatternMatch;

  SmallVector<Instruction *> Insts(Extent);
  sortInstructionsInRPO(Insts);

  using InstSet = SmallSet<Instruction *, 4>;
  struct InOut {
    InstSet In, Out;
  };
  auto Snapshot = [&](const InOut &X) { return X.In.size() + X.Out.size(); };
  SmallDenseMap<Instruction *, InOut> Map;

  InstSet Top;
  copy(Insts, std::inserter(Top, Top.end()));
  InstSet Bot;

  for (auto *I : Insts) {
    auto &X = Map[I];
    X.In = Top;
    X.Out = Top;
    if (match(I, m_Intrinsic<Intrinsic::unsafe_begin>())) {
      X.In = Bot;
      X.Out = Bot;
    }
  }

  bool Changed;
  do {
    Changed = false;
    for (auto *I : Insts) {
      if (match(I, m_Intrinsic<Intrinsic::unsafe_begin>()))
        continue;

      auto &X = Map.at(I);
      auto OrigSize = Snapshot(X);

      if (auto *Pred = I->getPrevNode()) {
        X.In = Map.at(Pred).Out;
      } else {
        for (auto *Pred : predecessors(I->getParent())) {
          auto &Y = Map.at(Pred->getTerminator()).Out;
          InstSet NewIn;
          for (auto *J : X.In)
            if (Y.contains(J))
              NewIn.insert(J);
          X.In = std::move(NewIn);
        }
      }

      X.Out = X.In;
      X.Out.erase(I);
      if (auto *PHI = dyn_cast<PHINode>(I)) {
        bool OK = true;
        for (unsigned Idx = 0; Idx < PHI->getNumIncomingValues(); ++Idx) {
          auto *PredB = PHI->getIncomingBlock(Idx);
          auto *PredVal = PHI->getIncomingValue(Idx);
          auto *PredValI = dyn_cast<Instruction>(PredVal);
          if (!PredValI) {
            OK = false;
            break;
          }
          if (!Map.at(PredB->getTerminator()).Out.contains(PredValI)) {
            OK = false;
            break;
          }
        }
        if (OK)
          X.Out.insert(I);
      } else if (match(I, m_Intrinsic<Intrinsic::unsafe_shadow_gen>())) {
        X.Out.insert(I);
      }

      Changed |= OrigSize != Snapshot(X);
    }
  } while (Changed);

  copy_if(Insts, std::back_inserter(Gens),
          [&](Instruction *I) { return Map.at(I).Out.contains(I); });
}

bool runOnUnsafeFunction(Function &F) {
  {
    IRBuilder<> IRB(&*F.getEntryBlock().getFirstInsertionPt());
    IRB.CreateIntrinsic(Intrinsic::unsafe_begin, {});
    for (auto &A : F.args()) {
      auto *Dep = IRB.CreateUnaryIntrinsic(Intrinsic::unsafe_shadow_arg, &A);
      // NOTE: Tying to dummy value currently.
      IRB.CreateIntrinsic(Intrinsic::unsafe_shadow_tie,
                          {IRB.getInt32(0), Dep});
    }
  }

  for (auto &I : instructions(F)) {
    Value *RetVal = nullptr;
    if (auto *Ret = dyn_cast<ReturnInst>(&I))
      RetVal = Ret->getReturnValue();
    else if (auto *Resume = dyn_cast<ResumeInst>(&I))
      RetVal = Resume->getValue();
    if (!RetVal)
      continue;
    IRBuilder<> IRB(&I);
    auto *End = IRB.CreateIntrinsic(Intrinsic::unsafe_end, {});
    auto *NewRetVal = IRB.CreateBinaryIntrinsic(Intrinsic::unsafe_shadow_embed,
                                                RetVal, End);
    I.replaceUsesOfWith(RetVal, NewRetVal);
  }

  F.removeFnAttr(Attribute::Unsafe);

  auto &Ctx = F.getContext();
  F.setMetadata("soundspec.final", MDNode::get(Ctx, {}));

  return true;
}

static bool canRematerializeInstruction(Instruction &I) {
  if (auto *C = dyn_cast<CallBase>(&I))
    if (C->cannotDuplicate())
      return false;
  if (isa<PHINode>(&I))
    return false;
  if (!isSafeToSpeculativelyExecute(&I))
    return false;
  return true;
}

static void rematerializeInstruction(Instruction &I, Instruction *Before, ValueToValueMapTy &VMap, ValueMapper &VMapper) {
  IRBuilder<> IRB(Before);
  auto *NewI = IRB.Insert(I.clone());
  VMapper.remapInstruction(*NewI);
  VMap[&I] = NewI;
}

static Value *makeDepAvailableAt(Value *Dep, Instruction *Consumer, DominatorTree &DT) {
  auto IsAvailable = [&](Value *Dep) { return DT.dominates(Dep, Consumer); };

  auto *F = Consumer->getFunction();

  // Find all instructions that we need to hoist.
  SmallSet<Instruction *, 4> Seen;
  SmallVector<Value *> Todo;
  Todo.push_back(Dep);
  while (!Todo.empty()) {
    auto *Dep = Todo.pop_back_val();
    // Skip if it's already available where it needs to be.
    if (IsAvailable(Dep))
      continue;
    auto *DepI = cast<Instruction>(Dep);
    // Skip if we've already seen it.
    if (!Seen.insert(DepI).second)
      continue;
    // Add uses.
    copy(DepI->operand_values(), std::back_inserter(Todo));
  }

  // Now we have a list of instructions that need to duplicate.
  // Walk them in reverse postorder, to make sure we handle defs before uses.
  ValueToValueMapTy VMap;
  ValueMapper VMapper(VMap, llvm::RF_NoModuleLevelChanges | llvm::RF_IgnoreMissingLocals);
  for (auto *B : ReversePostOrderTraversal(F)) {
    for (auto &I : make_early_inc_range(*B)) {
      if (!Seen.contains(&I))
        continue;
      // We have an instruction in need of duplication.
      assert(canRematerializeInstruction(I));
      rematerializeInstruction(I, Consumer, VMap, VMapper);
    }
  }

  return VMapper.mapValue(*Dep);
}

bool runSoundSpecCleanupOnFunction(Function &F) {
  using namespace llvm::PatternMatch;

  if (F.hasFnAttribute(Attribute::Unsafe))
    return runOnUnsafeFunction(F);

  auto &Ctx = F.getContext();

  // First, merge overlapping scopes.
  auto IDSet = getIDSet(F);
  combineScopes(F, IDSet);

  // Insert @llvm.unsafe.begin/llvm.unsafe.end for each scope.
  // Remove @llvm.unsafe.gen, replace with value returned by @llvm.unsafe.end.
  for (Metadata *ID : IDSet) {
    SmallVector<CallInst *> Ends;
    {
      UnsafeScope Extent(F, ID);
      auto ExtentInsts = Extent.instructions();
      auto InScope = [&](Instruction *I) {
        return is_contained(ExtentInsts, I);
      };
      for (auto *I : Extent.instructions()) {
        instrumentUnsafeBegin(I, ID, InScope);
        instrumentUnsafeEnd(I, ID, InScope, Ends);
      }
    }

    UnsafeScope Extent(F, ID);
    auto ExtentInsts = Extent.instructions();
    SmallVector<Instruction *> Gens;
    computeGenSet(ExtentInsts, Gens);

    SSAUpdater GenToEndUpdater;
    GenToEndUpdater.Initialize(IntegerType::get(Ctx, 32), "gen");
    for (auto *End : Ends) {
      GenToEndUpdater.AddAvailableValue(End->getParent(), End);

      Instruction *I = End;
      while ((I = I->getNextNode()))
        for (auto *Gen : Gens)
          I->replaceUsesOfWith(Gen, End);
    }

    SmallSet<Instruction *, 8> ExtentSet;
    copy(ExtentInsts, std::inserter(ExtentSet, ExtentSet.end()));
    for (auto *Gen : Gens) {
      for (Use &GenUse : make_early_inc_range(Gen->uses())) {
        if (ExtentSet.contains(cast<Instruction>(GenUse.getUser())))
          continue;
        GenToEndUpdater.RewriteUse(GenUse);
      }
    }

    auto *DummyVal = ConstantInt::get(IntegerType::get(Ctx, 32), 0);
    for (auto *Gen : Gens) {
      Gen->replaceAllUsesWith(DummyVal);
      Gen->eraseFromParent();
    }
  }

  // Convert @llvm.unsafe.shadow.xmit() into @llvm.unsafe.shadow.embed(%data,
  // %dep).
  SmallDenseMap<Metadata *, Value *> XmitDeps;
  for (auto &I : make_early_inc_range(instructions(F))) {
    auto *II = dyn_cast<IntrinsicInst>(&I);
    if (!II || II->getIntrinsicID() != Intrinsic::unsafe_shadow_xmit)
      continue;
    auto *XmitIDs = I.getMetadata(LLVMContext::MD_soundspec_xmit);
    assert(XmitIDs);
    auto *Dep = II->getArgOperand(0);
    for (Metadata *XmitID : XmitIDs->operands()) {
      if (auto *OtherDep = XmitDeps.lookup(XmitID)) {
        assert(OtherDep == Dep);
        continue;
      }
      XmitDeps[XmitID] = Dep;
    }
  }

  // !safe transmitters.
  DominatorTree DT(F);
  for (auto &I : instructions(F)) {
    if (!I.getMetadata(LLVMContext::MD_safe))
      continue;
    if (!isTransmitter(I))
      continue;
    auto *Op = getTransmittedOperand(I);
    auto *XmitID = I.getMetadata(LLVMContext::MD_soundspec_xmit);
    assert(XmitID->getNumOperands() == 1 && XmitID->getOperand(0) == XmitID);
    if (!XmitDeps.contains(XmitID)) {
      errs() << "No @llvm.unsafe.shadow.xmit() for metadata " << *XmitID
             << " in function:\n"
             << F << "\n";
      errs() << "Instruction: " << I << "\n";
    }
    auto *Dep = XmitDeps.at(XmitID);
    if (!DT.dominates(Dep, &I))
      Dep = makeDepAvailableAt(Dep, &I, DT);
    assert(DT.dominates(Dep, &I));
    IRBuilder<> IRB(&I);
    auto *NewOp =
        IRB.CreateBinaryIntrinsic(Intrinsic::unsafe_shadow_embed, Op, Dep);
    I.replaceUsesOfWith(Op, NewOp);
  }

  // Remove @llvm.unsafe.shadow.xmit()
  for (auto &I : make_early_inc_range(instructions(F)))
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_xmit>()))
      I.eraseFromParent();

  F.setMetadata("soundspec.final", MDNode::get(Ctx, {}));

  return true;
}

class SoundSpecCleanupLegacyPass : public FunctionPass {
public:
  static char ID;

  SoundSpecCleanupLegacyPass() : FunctionPass(ID) {
    initializeSoundSpecCleanupLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // No required analyses — the pass computes its own DominatorTree and
    // uses no external analyses today. Add as needed.
  }

  bool runOnFunction(Function &F) override {
    return runSoundSpecCleanupOnFunction(F);
  }
};

} // namespace

PreservedAnalyses SoundSpecCleanupPass::run(Function &F,
                                            FunctionAnalysisManager &FAM) {
  bool Changed = runSoundSpecCleanupOnFunction(F);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

char SoundSpecCleanupLegacyPass::ID = 0;

INITIALIZE_PASS(SoundSpecCleanupLegacyPass, DEBUG_TYPE,
                "SoundSpec cleanup pass", false, false)

FunctionPass *llvm::createSoundSpecCleanupLegacyPass() {
  return new SoundSpecCleanupLegacyPass();
}
