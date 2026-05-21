#ifndef LLVM_IR_UNSAFESCOPE_H
#define LLVM_IR_UNSAFESCOPE_H

#include "llvm/IR/CFG.h"
#include "llvm/IR/Metadata.h"
#include "llvm/ADT/SmallSet.h"

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

MDNode *enumerateArgumentDeps(const CallBase &I, unsigned ArgNo);

MDNode *enumerateDeps(const Value *V);

MDNode *loopMetadata(const Loop &L);

bool isRelevantCall(const Instruction &I);

unsigned getNumDepOperands(const Instruction &I);
Value *getDepOperand(Instruction &I, unsigned Idx);

uint32_t getScopeFromMetadata(const Metadata *MD);

MDNode *addIDToSet(MDNode *Set, Metadata *ID);

void addIDToMetadataSetInPlace(Instruction &I, StringRef Key, Metadata *ID);

uint32_t getMaxScopeID(const Function &F);

bool hasID(const Instruction &I, Metadata *ID);

/// Call this before inlining a !safe-metadata call to an unsafe-attr function.
/// It inserts and removes some intrinsics directly before the call,
/// and removes some intrinsics right after the call.
/// It allocates a new unsafe scope and returns it.
/// All transmitters (isTransmitter), speculation primitives
/// (isSpeculationPrimitive), and relevant calls (isRelevantCall)
/// should be then tagged with the returned unsafe scope.
MDNode *prepareCallSiteToUnsafeFnForInlining(CallBase *C);

/// Call this after inlining a non-unsafe-attr function into an unsafe-attr function.
/// It removes llvm.unsafe.shadow.* intrinsics from the callee.
void postProcessUnsafeFnAfterInlining(Function &F);

void setUnsafeScope(Instruction &I, MDNode *ID);

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

  void print(raw_ostream &os);
  void dump();
};

// TODO: Might be able to remove.
void replaceUnsafeScopeWith(Function &F, MDNode *OldID, MDNode *NewID);

} // namespace llvm

#endif