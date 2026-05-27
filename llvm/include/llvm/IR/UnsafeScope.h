#ifndef LLVM_IR_UNSAFESCOPE_H
#define LLVM_IR_UNSAFESCOPE_H

#include "llvm/IR/CFG.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/MapVector.h"

namespace llvm {

class IntrinsicInst;
class IRBuilderBase;
class CallBase;
class Loop;

class UnsafeBuilderScope {
  IRBuilderBase &Builder;

public:
  UnsafeBuilderScope(IRBuilderBase &Builder, MDNode *UnsafeScopeMD);
  UnsafeBuilderScope(IRBuilderBase &Builder, const Instruction &I)
      : UnsafeBuilderScope(Builder, I.getMetadata(LLVMContext::MD_unsafe)) {}

  ~UnsafeBuilderScope();
};

inline MDNode *getUnsafeScope(const Instruction &I) {
  return I.getMetadata(LLVMContext::MD_unsafe);
}

namespace unsafe {

MDNode *mergeMetadata(MDNode *A, MDNode *B);

// Removes the given unsafe scope ID from the list of unsafe scopes `MD`.
// MD can be null, which denotes an empty list.
// This function returns null if the resulting unsafe scope ID set is empty.
MDNode *removeID(MDNode *MD, uint64_t ID);

// Enumerate direct dependencies of a specific value.
// If the value is an instruction with !unsafe metadata,
// then we return simply that metadata.
MDNode *enumerateTransmitterDeps(const Instruction &I);

// Enumerate direct dependencies of an unsafe scope.
MDNode *enumerateScopeDeps(int ID, const Function &F);

MDNode *enumerateDeps(const Value *V);

MDNode *loopMetadata(const Loop &L);

bool isRelevantCall(const Instruction &I);

unsigned getNumDepOperands(const Instruction &I);
Value *getDepOperand(Instruction &I, unsigned Idx);

MDNode *addIDToSet(MDNode *Set, Metadata *ID);

void addIDToMetadataSetInPlace(Instruction &I, StringRef Key, Metadata *ID);

bool hasID(const Instruction &I, Metadata *ID);

/// Call this after inlining a non-unsafe-attr function into an unsafe-attr function.
/// It removes llvm.unsafe.shadow.* intrinsics from the callee.
void postProcessAfterInliningIntoUnsafeFn(Function &F);

void setUnsafeScope(Instruction &I, MDNode *ID);

MDNode *createNewScope(LLVMContext &Ctx);

MDNode *getScopeSet(LLVMContext &Ctx, ArrayRef<Metadata *> IDs);

MDNode *createNewXmit(LLVMContext &Ctx);

MDNode *createNewXmitSet(LLVMContext &Ctx, ArrayRef<Metadata *> Xmits);

MDNode *concatMetadataNodes(MDNode *A, MDNode *B);

CallInst *createShadowTie(Instruction *At, Value *Gen, Value *Dep, MDNode *UnsafeIDs);
CallInst *createShadowTie(IRBuilderBase &IRB, Value *Gen, Value *Dep, MDNode *UnsafeIDs);

struct InliningCookie {
  /// The call's !unsafe scope ID, if any.
  /// nullptr = the call was safe.
  Metadata *ID = nullptr;
  /// If the call was !unsafe, then this is the producer
  /// of the scope's gen.
  Instruction *Gen = nullptr;
};

InliningCookie prepareCallSiteForInlining(CallBase &C);
void postProcessInlinedBlocks(iterator_range<Function::iterator> Blocks, InliningCookie Info);

Instruction *normalizeCallSiteBeforeInliningIntoSafeFn(CallBase &C);

/// Call this on each inlined basic block of an inlined !unsafe call.
/// IDs should be the !unsafe scope set on the call.
void postProcessBlockInlinedFromUnsafeCall(BasicBlock &B, MDNode *IDs, Instruction *Gen);

void tieLoadResult(Instruction &I, Value *Gen, MDNode *UnsafeIDs);

bool isValidScope(Metadata *ID);

Metadata *getUniqueScope(Instruction &I);

CallInst *createGenIntrinsic(IRBuilderBase &IRB, Metadata *ID);

}

bool isSpeculationPrimitive(const Instruction &I);
Value *getTransmittedOperand(Instruction &I);
const Value *getTransmittedOperand(const Instruction &I);
bool isTransmitter(const Instruction &I);

class UnsafeScope {
  using DomSetType = SmallSet<Instruction *, 2>;

  Function &F;

public:
  Metadata *ID;

private:
  DomSetType DomSet;
  SmallVector<Instruction *> Scope;

  template <typename OutputIt>
  void getScopeDomSet(Function &F, Metadata *ID, OutputIt Out);

  void computeScopeSet(Function &F, Metadata *ID);

public:
  UnsafeScope(Function &F, Metadata *ID) : F(F), ID(ID) { computeScopeSet(F, ID); }

  /// Returns the list of instructions in the scope, in reverse postorder.
  ArrayRef<Instruction *> instructions() const { return Scope; }

  const DomSetType &dominanceSet() const { return DomSet; }

  // Get the values consumed by each instruction in the dominance set.
  DenseMap<Instruction *, SmallSet<Value *, 4>> computeConsumptionFrontier();

  void print(raw_ostream &OS);
  void dump();
};

void replaceUnsafeScopeWith(Function &F, Metadata *Old, Metadata *New);

class GenAnalysisImpl {
  Function &F;
  LLVMContext &Ctx;
  Metadata *ID;
  Value *IDVal;
  MapVector<PHINode *, bool> Cache;

public:
  GenAnalysisImpl(Function &F, Metadata *ID)
      : F(F), Ctx(F.getContext()), ID(ID), IDVal(MetadataAsValue::get(Ctx, ID)) {}

  bool queryGen(Value *V);
};

void sortInstructionsInRPO(MutableArrayRef<Instruction *> Insts);

Value *getUnsafeShadowDep(IRBuilderBase &IRB, Value *A, Value *B);

} // namespace llvm

#endif