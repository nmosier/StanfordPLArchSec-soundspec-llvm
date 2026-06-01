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
#include "llvm/ADT/SetVector.h"
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
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/Transforms/Utils/SoundSpecOptimizer.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <algorithm>

#define DEBUG_TYPE "soundspec-cleanup"

using namespace llvm;

static cl::opt<bool> SoundSpecStrip("soundspec-strip",
                                    cl::init(false), cl::Hidden);

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

void createUnsafeBegin(IRBuilderBase &IRB, Metadata *ID, Value *Dep) {
  auto &Ctx = IRB.getContext();
  auto *Begin = IRB.CreateIntrinsic(Intrinsic::unsafe_begin, {IRB.getInt32(0)});
  Begin->setMetadata(LLVMContext::MD_unsafe, unsafe::getScopeSet(Ctx, {ID}));
}

void createUnsafeBegin(Instruction *I, Metadata *ID, Value *Dep) {
  IRBuilder<> IRB(I);
  createUnsafeBegin(IRB, ID, Dep);
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
  // For any terminator with *multiple distinct edges* from `From` to
  // `To`, `SplitEdge` only redirects ONE such edge through the new
  // block, leaving the others direct. That leaves the SoundSpec scope
  // unbalanced (the un-split edges bypass the `unsafe_end` /
  // `unsafe_begin` we want to insert at the split).
  //
  // Multi-edge cases include:
  //   * SwitchInst with several cases targeting `To` (covered before).
  //   * IndirectBrInst with `To` listed more than once.
  //   * `br i1 cond, label %To, label %To` — a degenerate same-target
  //     conditional branch. Looks weird but appears in unoptimized
  //     IR (e.g. when an earlier transform collapses both targets but
  //     hasn't canonicalized to `br label %To` yet). This was the
  //     surprise case: we previously gated the SplitBlockPredecessors
  //     path on switch/indirectbr, so `br i1 c, X, X` fell through to
  //     plain SplitEdge and only one of the two X-edges got the end.
  //
  // `SplitBlockPredecessors(To, {From}, ...)` redirects *every* edge
  // from `From` to `To` through one new block — what SoundSpec needs.
  auto *Term = From->getTerminator();
  unsigned NumEdgesToTo = 0;
  for (unsigned I = 0, E = Term->getNumSuccessors(); I != E; ++I)
    if (Term->getSuccessor(I) == To)
      ++NumEdgesToTo;
  if (NumEdgesToTo > 1) {
    if (auto *New = SplitBlockPredecessors(To, {From}, ".soundspec.split"))
      return New;
  }

  if (auto *New = SplitEdge(From, To))
    return New;

  if (auto *Invoke = dyn_cast<InvokeInst>(Term);
      Invoke && Invoke->getUnwindDest() == To) {
    return robustSplitEdgeInvoke(From, To, Invoke);
  }

  report_fatal_error("Failed to split for unknown reason");
}

using InstructionPred = function_ref<bool(Instruction *)>;

void instrumentUnsafeBegin(Instruction *I, Metadata *ID,
                           InstructionPred InScope) {
  auto &Ctx = I->getContext();
  auto *Zero = ConstantInt::get(IntegerType::get(Ctx, 32), 0);
  
  if (auto *Pred = I->getPrevNode()) {
    if (!InScope(Pred))
      createUnsafeBegin(I, ID, Zero);
    return;
  }

  auto *B = I->getParent();
  auto InScopeB = [&](BasicBlock *B) { return InScope(B->getTerminator()); };

  if (none_of(predecessors(B), InScopeB)) {
    createUnsafeBegin(I, ID, Zero);
    return;
  }

  // Dedupe — `predecessors(B)` yields one entry per *edge*, so a
  // SwitchInst whose three cases all target `B` would have us split
  // that source three times.
  SmallSetVector<BasicBlock *, 4> Preds;
  Preds.insert_range(predecessors(B));
  for (auto *PredB : Preds) {
    if (InScopeB(PredB))
      continue;
    auto *NewB = robustSplitEdge(PredB, B);
    assert(NewB);
    createUnsafeBegin(NewB->getTerminator(), ID, Zero);
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
  // Dedupe successors. `successors(B)` walks the terminator's successor
  // list, which has one entry per *edge* — a SwitchInst with three
  // cases targeting the same block returns that block three times.
  // Without deduping we'd insert one `unsafe_end` per duplicate at the
  // target's first insertion point, leaving multiple ends matched to a
  // single begin (and tripping checkUnsafeBeginEnd).
  SmallSetVector<BasicBlock *, 4> Succs;
  Succs.insert_range(successors(B));
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
  auto &Ctx = F.getContext();
  auto *ID = unsafe::createNewScope(Ctx);
  auto *IDs = unsafe::getScopeSet(Ctx, {ID});
  {
    // Create deps for args.
    IRBuilder<> IRB(&*F.getEntryBlock().getFirstInsertionPt());
    Value *Dep = IRB.getInt32(0);
    for (auto &A : F.args()) {
      auto *ArgDep = IRB.CreateUnaryIntrinsic(Intrinsic::unsafe_shadow_arg, &A);
      Dep = getUnsafeShadowDep(IRB, Dep, ArgDep);
    }
    createUnsafeBegin(IRB, ID, Dep);
  }

  for (auto &B : F) {
    if (succ_size(&B) > 0)
      continue;
    Value *RetVal = nullptr;
    auto &I = *B.getTerminator();
    if (auto *Ret = dyn_cast<ReturnInst>(&I)) {
      RetVal = Ret->getReturnValue();
    } else if (auto *Resume = dyn_cast<ResumeInst>(&I)) {
      RetVal = Resume->getValue();
    } else if (isa<UnreachableInst>(&I)) {
      RetVal = nullptr;
    } else {
      errs() << "[soundspec] unhandled terminator: " << I << "\n";
      std::abort();
    }

    IRBuilder<> IRB(&I);
    auto *End = IRB.CreateIntrinsic(Intrinsic::unsafe_end, {});
    if (!RetVal)
      continue;
    auto *NewRetVal = IRB.CreateBinaryIntrinsic(Intrinsic::unsafe_shadow_embed,
                                                RetVal, End);
    I.replaceUsesOfWith(RetVal, NewRetVal);
  }

  F.removeFnAttr(Attribute::Unsafe);

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

static void removeLoadTies(Function &F) {
  using namespace llvm::PatternMatch;
  for (auto &I : make_early_inc_range(instructions(F))) {
    Value *V;
    if (!match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_tie>(
                       m_Value(),
                       m_Intrinsic<Intrinsic::unsafe_shadow_arg>(m_Value(V)))))
      continue;
    LoadInst *Load = dyn_cast<LoadInst>(V);
    if (!Load)
      continue;
    if (Load->getMetadata(LLVMContext::MD_unsafe) !=
        I.getMetadata(LLVMContext::MD_unsafe))
      continue;
    I.eraseFromParent();
  }
}

static void
promoteShadowTiesToBeginTransferPHIs(BasicBlock &B, SmallSet<Value *, 4> &X,
                                     BasicBlock *Pred,
                                     ArrayRef<Instruction *> Scope) {
  for (auto &I : reverse(make_range(B.begin(), B.getFirstNonPHIIt()))) {
    if (!is_contained(Scope, &I)) {
      X.clear();
      continue;
    }
    auto *Phi = cast<PHINode>(&I);
    if (!X.erase(Phi))
      continue;
    auto *V = Phi->getIncomingValueForBlock(Pred);
    assert(V);
    X.insert(V);
  }
}

static void promoteShadowTiesToBeginTransferBlock(
    BasicBlock &B, SmallSet<Value *, 4> &X,
    SmallMapVector<Instruction *, SmallSet<Value *, 4>, 4> &Begins,
    ArrayRef<Instruction *> Scope) {
  using namespace llvm::PatternMatch;
  auto Range = make_range(B.getFirstNonPHIIt(), B.end());
  for (auto &I : reverse(Range)) {
    if (!is_contained(Scope, &I)) {
      X.clear();
      continue;
    }

    // Is this @llvm.unsafe.shadow.tie()?
    // If so, add the tied dep.
    Value *Dep;
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_tie>(m_Value(),
                                                            m_Value(Dep)))) {
      X.insert(Dep);
      continue;
    }

    // Is this @llvm.unsafe.shadow.begin()?
    // If so, clear the deps and skip.
    // Also, copy out the results.
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_begin>())) {
      Begins[&I] = X;
      X.clear();
      continue;
    }

    // Otherwise, is the result not a tied dep? If not, skip.
    if (!X.erase(&I))
      continue;

    // If it was @llvm.unsafe.shadow.arg(), then add the input I guess.
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_arg>(m_Value(Dep)))) {
      X.insert(Dep);
      continue;
    }

    // Handle on a case-by-case basis.
    if (isa<CallBase, LoadInst>(&I))
      continue;
#if 0
    if (isa<BinaryOperator, CastInst, SelectInst, CmpInst, ExtractElementInst, ExtractValueInst, GetElementPtrInst, FreezeInst, InsertValueInst, InsertElementInst, LandingPadInst, ShuffleVectorInst>(&I)) {
      copy(I.operands(), std::inserter(X, X.end()));
      continue;
    }
    errs() << "internal error: unhandled instruction: " << I << "\n";
    std::abort();
#else
    copy(I.operands(), std::inserter(X, X.end()));
    continue;
#endif
  }
}

static void promoteShadowTiesToBegin(Function &F, UnsafeScope &Extent) {
  using namespace llvm::PatternMatch;

  using T = SmallSet<Value *, 4>;
  struct InOut {
    T In;
    T Out;
  };
  SmallSet<BasicBlock *, 4> Blocks;
  for (auto *I : Extent.instructions())
    Blocks.insert(I->getParent());
  DenseMap<BasicBlock *, InOut> Map;
  bool Changed;
  SmallMapVector<Instruction *, T, 4> Begins;
  auto Snapshot = [] (const InOut &X) {
    return X.In.size() + X.Out.size();
  };
  do {
    Changed = false;
    for (auto *B : post_order(&F)) {
      if (!Blocks.contains(B))
        continue;

      // Recompute OUT into a local. The Map[Succ] lookups below can insert
      // new entries and grow the DenseMap, which would invalidate any
      // reference into it — so don't hold a reference to Map[B] across them.
      T Out = Map[B].Out;
      for (auto *Succ : successors(B)) {
        auto SuccIn = Map[Succ].In;
        promoteShadowTiesToBeginTransferPHIs(*Succ, SuccIn, B,
                                             Extent.instructions());
        copy(SuccIn, std::inserter(Out, Out.end()));
      }

      // Transfer across block.
      T In = Out;
      promoteShadowTiesToBeginTransferBlock(*B, In, Begins,
                                            Extent.instructions());

      // Commit. Safe to take a reference now: no further inserts follow.
      auto &X = Map[B];
      auto OrigSize = Snapshot(X);
      X.Out = std::move(Out);
      X.In = std::move(In);
      Changed |= (OrigSize != Snapshot(X));
    }
  } while (Changed);

  // Instrument @llvm.unsafe.begin()s.
  for (auto &[I, Deps] : Begins) {
    auto *Begin = dyn_cast<IntrinsicInst>(I);
    IRBuilder<> IRB(I);
    Value *Acc = IRB.getInt32(0);
    for (auto *Dep : Deps) {
      auto *DepTy = dyn_cast<IntegerType>(Dep->getType());
      if (!DepTy || DepTy->getIntegerBitWidth() != 32)
        Dep = IRB.CreateUnaryIntrinsic(Intrinsic::unsafe_shadow_arg, Dep);
      Acc = getUnsafeShadowDep(IRB, Acc, Dep);
    }
    Begin->setArgOperand(0, Acc);
  }

  // Remove all @llvm.unsafe.shadow.tie()s.
  for (auto *I : Extent.instructions()) {
    if (match(I, m_Intrinsic<Intrinsic::unsafe_shadow_tie>())) {
      I->eraseFromParent();
    }
  }
}

static void insertSafeBeginEnd(Function &F) {
  IRBuilder<> IRB(F.getContext());

  // @llvm.safe.begin() at function entry.
  auto &Entry = F.getEntryBlock();
  IRB.SetInsertPoint(&Entry, Entry.getFirstInsertionPt());
  IRB.CreateIntrinsic(Intrinsic::safe_begin, {});

  // @llvm.safe.end() at each function exit.
  for (auto &B : F) {
    if (!succ_empty(&B))
      continue;
    IRB.SetInsertPoint(B.getTerminator());
    IRB.CreateIntrinsic(Intrinsic::safe_end, {});
  }
}

void runOnSafeFunction(Function &F) {
  using namespace llvm::PatternMatch;

  auto &Ctx = F.getContext();

  // Optimize unsafe scopes before merging.
  optimizeUnsafeScopes(F);

  // First, merge overlapping scopes.
  auto IDSet = getIDSet(F);
  combineScopes(F, IDSet);

  // Optimize unsafe scopes after merging.
  optimizeUnsafeScopes(F);

  // Remove @llvm.unsafe.shadow.tie()s that are rooted to loads in the same
  // unsafe scope.
  // We originally added those in case mem2reg ran. Now that we know it won't,
  // just remove the @llvm.unsafe.shadow.tie().
  removeLoadTies(F);
  
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

    // Push @llvm.unsafe.shadow.tie() to @llvm.unsafe.begin() input.
    // Rebuild the scope first: erasing the Gens above invalidated the
    // instruction list cached in `Extent`.
    UnsafeScope FreshExtent(F, ID);
    promoteShadowTiesToBegin(F, FreshExtent);
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


  // Insert @llvm.safe.begin() and @llvm.safe.end().
  insertSafeBeginEnd(F);

  F.setMetadata("soundspec.final", MDNode::get(Ctx, {}));
}

void instrumentUnsafeArgs(Function &F) {
  errs() << "WARNING: not instrumenting dynamic dispatch\n";
  return;


  auto &Ctx = F.getContext();
  auto *EmptyMD = MDNode::get(Ctx, {});

  auto IsUnsafeArg = [](const Argument &A) {
    return A.hasAttribute(Attribute::SoundSpecArgUnsafe);
  };
  // Callee side: if this function declares any `soundspec_arg_unsafe` params,
  // emit an entry `unsafe_end` (the cross-function close of the scope the
  // caller opened before the call) and embed each unsafe param with its token.
  // The end is tagged `!unsafe.end.nocheck` so the intra-function balance
  // walker exempts it — it has no preceding begin in this function by design.
  if (any_of(F.args(), IsUnsafeArg)) {
    auto *B = &F.getEntryBlock();
    IRBuilder<> IRB(B, B->getFirstInsertionPt());
    auto *End = IRB.CreateIntrinsic(Intrinsic::unsafe_end, {});
    End->setMetadata("unsafe.end.nocheck", EmptyMD);
    for (auto &A : F.args()) {
      if (!IsUnsafeArg(A))
        continue;
      auto *NewA =
          IRB.CreateBinaryIntrinsic(Intrinsic::unsafe_shadow_embed, &A, End);
      A.replaceUsesWithIf(NewA, [NewA](Use &U) {
        return U.getUser() != NewA;
      });
    }
  }

  // Caller side: before any call whose arg ops carry `soundspec_arg_unsafe`,
  // open the cross-function scope with an `unsafe_begin` (closed by the
  // callee's entry `unsafe_end`). Tagged `!unsafe.begin.nocheck` so the
  // balance walker exempts it — it has no matching end in this function by
  // design.
  for (auto &I : instructions(F)) {
    auto *C = dyn_cast<CallBase>(&I);
    if (!C)
      continue;
    bool AnyUnsafeArg = false;
    for (unsigned Idx = 0; Idx < C->arg_size(); ++Idx) {
      auto Attr = C->getParamAttr(Idx, Attribute::SoundSpecArgUnsafe);
      AnyUnsafeArg |= Attr.isValid();
    }
    if (AnyUnsafeArg) {
      IRBuilder<> IRB(C);
      auto *Begin =
          IRB.CreateIntrinsic(Intrinsic::unsafe_begin, {IRB.getInt32(0)});
      Begin->setMetadata("unsafe.begin.nocheck", EmptyMD);
    }
  }
}

void debugStripAllIntrinsics(Function &F) {
#if 0
  auto &Ctx = F.getContext();
  for (auto &I : make_early_inc_range(instructions(F))) {
    using namespace llvm::PatternMatch;
    Value *ReplVal = ConstantInt::get(IntegerType::get(Ctx, 32), 0);
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_begin>()) ||
        match(&I, m_Intrinsic<Intrinsic::unsafe_end>()) ||
        match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_dep>()) ||
        match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_arg>()) ||
        match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_tie>()) ||
        match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_embed>(m_Value(ReplVal), m_Value()))) {
      if (!I.getType()->isVoidTy())
        I.replaceAllUsesWith(ReplVal);
      I.eraseFromParent();
      continue;
    }
  }
#endif
}

static void checkUnsafeBeginEnd(Function &F) {
  using namespace llvm::PatternMatch;

  struct InOut {
    unsigned In = 0;
    unsigned Out = 0;
  };
  DenseMap<BasicBlock *, InOut> Map;
  for (auto &B : F) {
    auto &X = Map[&B];
    X.In = UINT_MAX;
    X.Out = UINT_MAX;
  }
  Map[&F.getEntryBlock()].In =
      0; // NOTE: When we re-enable self hardening, this may need to be 1.
  bool Changed;
  auto Snapshot = [](const InOut &X) -> uint64_t {
    return (uint64_t) X.In + (uint64_t) X.Out;
  };
  do {
    Changed = false;
    for (auto *B : ReversePostOrderTraversal(&F)) {
      auto &X = Map[B];
      auto Old = Snapshot(X);

      // Compute IN (min).
      for (auto *Pred : predecessors(B))
        X.In = std::min(X.In, Map[Pred].Out);

      // Transfer.
      X.Out = X.In;
      for (auto &I : *B) {
        // `!unsafe.begin.nocheck` / `!unsafe.end.nocheck` mark cross-function
        // begins/ends emitted by `instrumentUnsafeArgs` (the caller-side
        // begin before a `soundspec_arg_unsafe` call is closed by the
        // callee's entry `unsafe_end`, and vice versa). They are unbalanced
        // within a single function by design — skip them here.
        if (match(&I, m_Intrinsic<Intrinsic::unsafe_begin>()) &&
            !I.getMetadata("unsafe.begin.nocheck")) {
          assert(X.Out < UINT_MAX && "depth would overflow!");
          ++X.Out;
        } else if (match(&I, m_Intrinsic<Intrinsic::unsafe_end>()) &&
                   !I.getMetadata("unsafe.end.nocheck")) {
          assert(X.Out > 0 && "mismatched unsafe_end!");
          --X.Out;
        }
      }

      auto New = Snapshot(X);
      assert(New <= Old);
      Changed |= Old != New;
    }
  } while (Changed);

  // Make sure that we don't have a mismatched paths.
  for (auto *B : ReversePostOrderTraversal(&F)) {
    for (auto *Pred : predecessors(B)) {
      assert(Map[B].In == Map[Pred].Out && "mismatched paths!");
    }
    if (succ_size(B) == 0)
      assert(Map[B].Out == 0 && "function exit with unclosed unsafe_begin!");
  }
}

static void doStrip(Function &F) {
  using namespace llvm::PatternMatch;
  auto &Ctx = F.getContext();
  auto *Zero = ConstantInt::get(IntegerType::get(Ctx, 32), 0);
  for (auto &I : make_early_inc_range(instructions(F))) {
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_gen>()) ||
        match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_dep>()) ||
        match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_arg>())) {
      I.replaceAllUsesWith(Zero);
      I.eraseFromParent();
      continue;
    }
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_tie>()) ||
        match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_xmit>())) {
      I.eraseFromParent();
      continue;
    }
    Value *V;
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_embed>(m_Value(V),
                                                              m_Value()))) {
      I.replaceAllUsesWith(V);
      I.eraseFromParent();
      continue;
    }
  }
}

static void checkNoTies(Function &F) {
  using namespace llvm::PatternMatch;
  for (auto &I : instructions(F)) {
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_shadow_tie>())) {
      errs() << "[soundspec] error: found lingering tie: " << I << "\n";
      std::abort();
    }
  }
}

static void optimizeBeginEndScopes(BasicBlock &B) {
  using namespace llvm::PatternMatch;

  // An empty unsafe scope looks like:
  // @llvm.unsafe.begin(%dep_in)
  // <no speculation primitives, period>
  // %dep_out = @llvm.unsafe.end()
  // ... some users of %dep_out ...
  auto It = B.begin();
  auto Last = B.end();
  while (It != Last) {
    // Look for a @llvm.unsafe.begin(%dep_in).
    Value *DepIn = nullptr;
    It = std::find_if(It, Last, [&DepIn](const Instruction &I) -> bool {
      return match(&I, m_Intrinsic<Intrinsic::unsafe_begin>(m_Value(DepIn)));
    });
    if (It == Last)
      break;
    auto BeginIt = It++;

    // Scan until we hit a @llvm.unsafe.end().
    It = std::find_if(It, Last, [](const Instruction &I) -> bool {
      return match(&I, m_Intrinsic<Intrinsic::unsafe_end>());
    });
    if (It == Last)
      break;
    auto EndIt = It++;
    iterator_range InnerRange(std::next(BeginIt), EndIt);

    // If there are any speculation primitives, abort.
    if (any_of(InnerRange, [](const Instruction &I) -> bool {
          return isSpeculationPrimitive(I) || unsafe::isRelevantCall(I);
        }))
      continue;

    // IT'S A GO!
    //
    // Harden all the transmitters.
    for (auto &I : InnerRange) {
      IRBuilder<> IRB(&I);
      // TODO: Refactor this with the code in runOnSafeFunction() - it does the
      // same kinda thing.
      if (!isTransmitter(I))
        continue;
      auto *OldOp = getTransmittedOperand(I);
      auto *NewOp = IRB.CreateBinaryIntrinsic(Intrinsic::unsafe_shadow_embed,
                                              OldOp, DepIn);
      I.replaceUsesOfWith(OldOp, NewOp);
    }

    // Remove llvm.unsafe.begin(%dep_in).
    BeginIt->eraseFromParent();

    // Replace + remove llvm.unsafe.end().
    EndIt->replaceAllUsesWith(DepIn);
    EndIt->eraseFromParent();
  }
}

bool runSoundSpecCleanupOnFunction(Function &F) {
  auto &Ctx = F.getContext();

  if (SoundSpecStrip) {
    doStrip(F);
    F.setMetadata("soundspec.final", MDNode::get(Ctx, {}));
    return true;
  }

  if (F.hasFnAttribute(Attribute::Unsafe)) {
    runOnUnsafeFunction(F);
  } else {
    runOnSafeFunction(F);
  }

  instrumentUnsafeArgs(F);

  // Prune empty unsafe scopes.
  for (auto &B : F)
    optimizeBeginEndScopes(B);

  checkUnsafeBeginEnd(F);

  checkNoTies(F);

  // If we have any soundspec_arg_unsafe, then we need to
  // insert a @llvm.unsafe.end()

#if 0
  // DEBUG: Remove all intrinsics!
  debugStripAllIntrinsics(F);
#endif

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