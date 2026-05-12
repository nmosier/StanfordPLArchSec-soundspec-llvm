#include "llvm/Transforms/Utils/UnsafeScopeConversion.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/UnsafeScope.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/PatternMatch.h"

using namespace llvm;

namespace {

struct MyBasicBlockEdge {
  BasicBlock *Src = nullptr;
  BasicBlock *Dest = nullptr;

  MyBasicBlockEdge(BasicBlock *Src, BasicBlock *Dest) : Src(Src), Dest(Dest) {}

  BasicBlock *getStart() const { return Src; }
  BasicBlock *getEnd() const { return Dest; }
};

}

static void collectScopeEnters(Function &F, int64_t Scope,
                               SmallVectorImpl<MyBasicBlockEdge> &BlockEdges,
                               SmallVectorImpl<Instruction *> &InstEdges) {
  SmallVector<Instruction *> Todo;
  SmallSet<Instruction *, 4> Seen;
  Todo.push_back(&F.getEntryBlock().front());
  while (!Todo.empty()) {
    Instruction *I = Todo.pop_back_val();

    // If we found unsafe metadata, stop.
    if (auto ID = getUnsafeScopeID(I->getMetadata(LLVMContext::MD_unsafe));
        ID && *ID == Scope)
      continue;

    // Skip if already seen.
    if (!Seen.insert(I).second)
      continue;

    // Add successors.
    if (auto *Succ = I->getNextNode()) {
      Todo.push_back(Succ);
    } else {
      for (auto *Succ : llvm::successors(I->getParent()))
        Todo.push_back(&Succ->front());
    }
  }

  for (Instruction *I : Seen) {
    // Intra-block.
    if (Instruction *Succ = I->getNextNode()) {
      if (!Seen.contains(Succ))
        InstEdges.push_back(I);
      continue;
    }

    // Inter-block.
    for (BasicBlock *Succ : llvm::successors(I->getParent())) {
      if (!Seen.contains(&Succ->front()))
        BlockEdges.emplace_back(I->getParent(), Succ);
    }
  }
}

static void collectScopeExits(Function &F, int64_t Scope,
                              SmallVectorImpl<MyBasicBlockEdge> &BlockEdges,
                              SmallVectorImpl<Instruction *> &InstEdges) {
  SmallVector<Instruction *> Todo;
  SmallSet<Instruction *, 4> Seen;
  for (Instruction &I : llvm::instructions(F))
    if (isa<ReturnInst>(&I))
      Todo.push_back(&I);
  while (!Todo.empty()) {
    Instruction *I = Todo.pop_back_val();

    // If the instruction has the scope, stop exploring.
    if (getUnsafeScopeID(I->getMetadata(LLVMContext::MD_unsafe)) == Scope)
      continue;

    // Skip if already seen.
    if (!Seen.insert(I).second)
      continue;

    // Add successors.
    if (Instruction *Pred = I->getPrevNode()) {
      Todo.push_back(Pred);
      continue;
    }
    for (BasicBlock *Pred : llvm::predecessors(I->getParent()))
      Todo.push_back(&Pred->back());
  }

  for (Instruction *I : Seen) {
    // Intra-block.
    if (Instruction *Pred = I->getPrevNode()) {
      if (!Seen.contains(Pred))
        InstEdges.push_back(Pred);
      continue;
    }

    // Inter-block.
    for (BasicBlock *Pred : llvm::predecessors(I->getParent()))
      if (!Seen.contains(&Pred->back()))
        BlockEdges.emplace_back(Pred, I->getParent());
  }
}

template <typename Func>
static void insertEnterExitIntrinsicAlongBlockEdges(
    Function &F, ArrayRef<MyBasicBlockEdge> Edges, Func CreateIntrinsic) {
  IRBuilder<> IRB(F.getContext());
  for (MyBasicBlockEdge Edge : Edges) {
    BasicBlock *InsBB = Edge.Dest;

    // If the destination has multiple predecessors,
    // then split the edge.
    if (llvm::pred_size(Edge.Dest) > 1) {
      InsBB = SplitEdge(Edge.Src, Edge.Dest);
      assert(InsBB && "Failed to split edge!");
    }

    // Set the insertion point.
    IRB.SetInsertPoint(InsBB, InsBB->getFirstNonPHIOrDbgOrLifetime());

    // Insert the intrinsic.
    CreateIntrinsic(IRB);
  }
}

template <typename Func>
static void insertEnterExitIntrinsicAlongInstEdges(
    Function &F, ArrayRef<Instruction *> Edges, Func CreateIntrinsic) {
  for (Instruction *Src : Edges) {
    Instruction *Dest = Src->getNextNode();
    assert(Dest);
    IRBuilder<> IRB(Dest);
    CreateIntrinsic(IRB);
  }
}

template <typename Func>
static void insertUnsafeEnterExitIntrinsicsForUnsafeScope(
    Function &F, int64_t Scope, Func Collect, Intrinsic::ID Intrin) {
  SmallVector<MyBasicBlockEdge> BlockEdges;
  SmallVector < Instruction *> InstEdges;
  Collect(F, Scope, BlockEdges, InstEdges);
  auto CreateIntrinsic = [&](IRBuilder<> &IRB) {
    IRB.CreateIntrinsic(Intrin, {IRB.getInt32(Scope)});
  };
  insertEnterExitIntrinsicAlongBlockEdges(F, BlockEdges, CreateIntrinsic);
  insertEnterExitIntrinsicAlongInstEdges(F, InstEdges, CreateIntrinsic);
}

[[nodiscard]] static auto getControlBoundedScope(Function &F, uint64_t Scope) {
  using namespace llvm::PatternMatch;

  SmallSet<Instruction *, 4> Seen;
  SmallVector<Instruction *> Todo;
  for (Instruction &I : llvm::instructions(F))
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_enter>(m_SpecificInt(Scope))))
      Todo.push_back(&I);
  while (!Todo.empty()) {
    Instruction *I = Todo.pop_back_val();

    // Skip if seen.
    if (!Seen.insert(I).second)
      continue;

    // Stop if llvm.unsafe.exit(x).
    if (match(I, m_Intrinsic<Intrinsic::unsafe_exit>(m_SpecificInt(Scope))))
      continue;

    // Add successors.
    insertSuccessors(I, std::back_inserter(Todo));
  }
  return Seen;
}


static void insertUnsafeEnterExitIntrinsics(Function &F, uint64_t Scope) {
  insertUnsafeEnterExitIntrinsicsForUnsafeScope(F, Scope, &collectScopeEnters,
                                                Intrinsic::unsafe_enter);
  insertUnsafeEnterExitIntrinsicsForUnsafeScope(F, Scope, &collectScopeExits,
                                                Intrinsic::unsafe_exit);
}

static void insertUnsafeInOut(Use &Src, Instruction *Dest,
                                std::optional<uint64_t> SrcID,
                                std::optional<uint64_t> DestID) {
  Value *V = Src;
  IRBuilder<> IRB(Dest);

  // Handle PHI nodes specially.
  if (PHINode *Phi = dyn_cast<PHINode>(&Dest)) {
    BasicBlock *Pred = Phi->getIncomingBlock(Src);
    if (llvm::succ_size(Pred) > 1) {
      Pred = SplitEdge(Phi->getIncomingBlock(Src), Dest->getParent());
      assert(Pred && "Failed to split edge!");
    }
    IRB.SetInsertPoint(Pred->getTerminator());
  }

  if (SrcID) {
    V = IRB.CreateBinaryIntrinsic(Intrinsic::unsafe_out, V,
                                  IRB.getInt32(*SrcID));
  }

  if (DestID) {
    V = IRB.CreateBinaryIntrinsic(Intrinsic::unsafe_in, V,
                                  IRB.getInt32(*DestID));
  }

  Src.set(V);
}

template <typename Range>
static void insertUnsafeInOutIntrinsic(Instruction &Dest, uint64_t Scope,
                                       const Range &ScopeInsts) {
  // Skip assume-like intrinsics, since they're not real code.
  if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(&Dest);
      II && II->isAssumeLikeIntrinsic())
    return;

  std::optional<uint64_t> DestID;
  if (ScopeInsts.contains(&Dest))
    DestID = Scope;
  for (Use &SrcU : Dest.operands()) {
    Value *Src = SrcU.get();
    std::optional<uint64_t> SrcID;
    if (auto *SrcI = dyn_cast<Instruction>(Src); SrcI && ScopeInsts.contains(SrcI))
      SrcID = Scope;

    // If the source and destination scopes are identical, nothing to insert.
    if (SrcID == DestID)
      continue;

    insertUnsafeInOut(SrcU, &Dest, SrcID, DestID);
  }
}

template <typename Range>
static void insertUnsafeInOutIntrinsics(Function &F, uint64_t Scope,
                                       const Range &ScopeInsts) {
  for (Instruction &I : llvm::make_early_inc_range(llvm::instructions(F))) {
    insertUnsafeInOutIntrinsic(I, Scope, ScopeInsts);
  }
}

void llvm::convertUnsafeMetadataToIntrinsics(Function &F) {
  // No llvm.unsafe.* intrinsics should be present currently.
  for (Instruction &I : llvm::instructions(F)) {
    auto *II = dyn_cast<IntrinsicInst>(&I);
    if (!II)
      continue;
    switch (II->getIntrinsicID()) {
    case Intrinsic::unsafe_enter:
    case Intrinsic::unsafe_exit:
    case Intrinsic::unsafe_in:
    case Intrinsic::unsafe_out:
      report_fatal_error("found unsafe intrinsic!");
    }
  }

  // Collect all scopes.
  SmallSet<uint64_t, 4> Scopes;
  for (Instruction &I : llvm::instructions(F))
    if (auto Scope = getUnsafeScopeID(I.getMetadata(LLVMContext::MD_unsafe)))
      Scopes.insert(*Scope);

  // Transform each scope.
  for (auto Scope : Scopes) {
    insertUnsafeEnterExitIntrinsics(F, Scope);
    auto ScopeInsts = getControlBoundedScope(F, Scope);
    insertUnsafeInOutIntrinsics(F, Scope, ScopeInsts);
  }

  // Remove metadata.
  for (Instruction &I : llvm::instructions(F))
    I.setMetadata(LLVMContext::MD_unsafe, nullptr);
}

void llvm::addMissingUnsafeInOutIntrinsics(Function &F) {
  using namespace llvm::PatternMatch;
  SmallSet<uint64_t, 4> Scopes;
  for (Instruction &I : llvm::instructions(F)) {
    uint64_t ID;
    if (match(&I, m_Intrinsic<Intrinsic::unsafe_enter>(m_ConstantInt(ID))))
      Scopes.insert(ID);
  }

  // For each scope, fill in missing intrinsics.
  for (auto ID : Scopes) {
    // Collect instructions in the scope.
    auto Scope = getControlBoundedScope(F, ID);
    for (Instruction *Dest : Scope) {
      for (Use &Src : Dest->operands()) {
        
      }
    }
  }
}