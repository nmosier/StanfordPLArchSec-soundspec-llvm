#ifndef LLVM_IR_UNSAFESCOPE_H
#define LLVM_IR_UNSAFESCOPE_H

#include "llvm/IR/CFG.h"
#include "llvm/IR/Metadata.h"
#include <optional>

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

} // namespace unsafe

bool isSpeculationPrimitive(const Instruction &I);
Value *getTransmittedOperand(Instruction &I);
const Value *getTransmittedOperand(const Instruction &I);
bool isTransmitter(const Instruction &I);

} // namespace llvm

#endif