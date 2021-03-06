//===--- OwnershipUtils.h ------------------------------------*- C++ -*----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_OWNERSHIPUTILS_H
#define SWIFT_SIL_OWNERSHIPUTILS_H

#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class SILBasicBlock;
class SILInstruction;
class SILModule;
class SILValue;
class DeadEndBlocks;

/// Returns true if v is an address or trivial.
bool isValueAddressOrTrivial(SILValue v);

/// These operations forward both owned and guaranteed ownership.
bool isOwnershipForwardingValueKind(SILNodeKind kind);

/// Is this an instruction that can forward both owned and guaranteed ownership
/// kinds.
bool isOwnershipForwardingInst(SILInstruction *i);

/// Is this an instruction that can forward guaranteed ownership.
bool isGuaranteedForwardingInst(SILInstruction *i);

/// These operations forward guaranteed ownership, but don't necessarily forward
/// owned values.
bool isGuaranteedForwardingValueKind(SILNodeKind kind);

/// Is this a value that is the result of an operation that forwards owned
/// ownership.
bool isGuaranteedForwardingValue(SILValue value);

/// Is this a node kind that can forward owned ownership, but may not be able to
/// forward guaranteed ownership.
bool isOwnedForwardingValueKind(SILNodeKind kind);

/// Does this SILInstruction 'forward' owned ownership, but may not be able to
/// forward guaranteed ownership.
bool isOwnedForwardingInstruction(SILInstruction *inst);

struct BorrowScopeOperandKind {
  enum Kind {
    BeginBorrow,
    BeginApply,
    Branch,
  };

  Kind value;

  BorrowScopeOperandKind(Kind newValue) : value(newValue) {}
  BorrowScopeOperandKind(const BorrowScopeOperandKind &other)
      : value(other.value) {}
  operator Kind() const { return value; }

  static Optional<BorrowScopeOperandKind> get(SILInstructionKind kind) {
    switch (kind) {
    default:
      return None;
    case SILInstructionKind::BeginBorrowInst:
      return BorrowScopeOperandKind(BeginBorrow);
    case SILInstructionKind::BeginApplyInst:
      return BorrowScopeOperandKind(BeginApply);
    case SILInstructionKind::BranchInst:
      return BorrowScopeOperandKind(Branch);
    }
  }

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              BorrowScopeOperandKind kind);

struct BorrowScopeIntroducingValue;

/// An operand whose user instruction introduces a new borrow scope for the
/// operand's value. The value of the operand must be considered as implicitly
/// borrowed until the user's corresponding end scope instruction.
///
/// NOTE: We do not require that the guaranteed scope be represented by a
/// guaranteed value in the same function: see begin_apply. In such cases, we
/// require instead an end_* instruction to mark the end of the scope's region.
struct BorrowScopeOperand {
  BorrowScopeOperandKind kind;
  Operand *op;

  BorrowScopeOperand(Operand *op)
      : kind(*BorrowScopeOperandKind::get(op->getUser()->getKind())), op(op) {}
  BorrowScopeOperand(const BorrowScopeOperand &other)
      : kind(other.kind), op(other.op) {}
  BorrowScopeOperand &operator=(const BorrowScopeOperand &other) {
    kind = other.kind;
    op = other.op;
    return *this;
  }

  /// If value is a borrow introducer return it after doing some checks.
  static Optional<BorrowScopeOperand> get(Operand *op) {
    auto *user = op->getUser();
    auto kind = BorrowScopeOperandKind::get(user->getKind());
    if (!kind)
      return None;
    return BorrowScopeOperand(*kind, op);
  }

  void visitEndScopeInstructions(function_ref<void(Operand *)> func) const;

  /// Returns true if this borrow scope operand consumes guaranteed
  /// values and produces a new scope afterwards.
  bool consumesGuaranteedValues() const {
    switch (kind) {
    case BorrowScopeOperandKind::BeginBorrow:
    case BorrowScopeOperandKind::BeginApply:
      return false;
    case BorrowScopeOperandKind::Branch:
      return true;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Is this a borrow scope operand that can open new borrow scopes
  /// for owned values.
  bool canAcceptOwnedValues() const {
    switch (kind) {
    case BorrowScopeOperandKind::BeginBorrow:
    case BorrowScopeOperandKind::BeginApply:
      return true;
    case BorrowScopeOperandKind::Branch:
      return false;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Is the result of this instruction also a borrow introducer?
  ///
  /// TODO: This needs a better name.
  bool areAnyUserResultsBorrowIntroducers() const {
    // TODO: Can we derive this by running a borrow introducer check ourselves?
    switch (kind) {
    case BorrowScopeOperandKind::BeginBorrow:
    case BorrowScopeOperandKind::Branch:
      return true;
    case BorrowScopeOperandKind::BeginApply:
      return false;
    }
    llvm_unreachable("Covered switch isn't covered?!");
  }

  /// Visit all of the results of the operand's user instruction that are
  /// consuming uses.
  void visitUserResultConsumingUses(function_ref<void(Operand *)> visitor);

  /// Visit all of the "results" of the user of this operand that are borrow
  /// scope introducers for the specific scope that this borrow scope operand
  /// summarizes.
  void visitBorrowIntroducingUserResults(
      function_ref<void(BorrowScopeIntroducingValue)> visitor);

  /// Passes to visitor all of the consuming uses of this use's using
  /// instruction.
  ///
  /// This enables one to walk the def-use chain of guaranteed phis for a single
  /// guaranteed scope by using a worklist and checking if any of the operands
  /// are BorrowScopeOperands.
  void visitConsumingUsesOfBorrowIntroducingUserResults(
      function_ref<void(Operand *)> visitor);

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

private:
  /// Internal constructor for failable static constructor. Please do not expand
  /// its usage since it assumes the code passed in is well formed.
  BorrowScopeOperand(BorrowScopeOperandKind kind, Operand *op)
      : kind(kind), op(op) {}
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const BorrowScopeOperand &operand);

struct BorrowScopeIntroducingValueKind {
  /// Enum we use for exhaustive pattern matching over borrow scope introducers.
  enum Kind {
    LoadBorrow,
    BeginBorrow,
    SILFunctionArgument,
    Phi,
  };

  static Optional<BorrowScopeIntroducingValueKind> get(SILValue value) {
    if (value.getOwnershipKind() != ValueOwnershipKind::Guaranteed)
      return None;
    switch (value->getKind()) {
    default:
      return None;
    case ValueKind::LoadBorrowInst:
      return BorrowScopeIntroducingValueKind(LoadBorrow);
    case ValueKind::BeginBorrowInst:
      return BorrowScopeIntroducingValueKind(BeginBorrow);
    case ValueKind::SILFunctionArgument:
      return BorrowScopeIntroducingValueKind(SILFunctionArgument);
    case ValueKind::SILPhiArgument: {
      if (llvm::any_of(value->getParentBlock()->getPredecessorBlocks(),
                       [](SILBasicBlock *block) {
                         return !isa<BranchInst>(block->getTerminator());
                       })) {
        return None;
      }
      return BorrowScopeIntroducingValueKind(Phi);
    }
    }
  }

  Kind value;

  BorrowScopeIntroducingValueKind(Kind newValue) : value(newValue) {}
  BorrowScopeIntroducingValueKind(const BorrowScopeIntroducingValueKind &other)
      : value(other.value) {}
  operator Kind() const { return value; }

  /// Is this a borrow scope that begins and ends within the same function and
  /// thus is guaranteed to have an "end_scope" instruction.
  ///
  /// In contrast, borrow scopes that are non-local (e.x. from
  /// SILFunctionArguments) rely a construct like a SILFunction as the begin/end
  /// of the scope.
  bool isLocalScope() const {
    switch (value) {
    case BorrowScopeIntroducingValueKind::BeginBorrow:
    case BorrowScopeIntroducingValueKind::LoadBorrow:
    case BorrowScopeIntroducingValueKind::Phi:
      return true;
    case BorrowScopeIntroducingValueKind::SILFunctionArgument:
      return false;
    }
    llvm_unreachable("Covered switch isnt covered?!");
  }

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              BorrowScopeIntroducingValueKind kind);

/// A higher level construct for working with values that represent the
/// introduction of a new borrow scope.
///
/// DISCUSSION: A "borrow introducer" is a SILValue that represents the
/// beginning of a borrow scope that the ownership verifier validates. The idea
/// is this API allows one to work in a generic way with all of the various
/// introducers.
///
/// Some examples of borrow introducers: guaranteed SILFunctionArgument,
/// LoadBorrow, BeginBorrow, guaranteed BeginApply results.
///
/// NOTE: It is assumed that if a borrow introducer is a value of a
/// SILInstruction with multiple results, that the all of the SILInstruction's
/// guaranteed results are borrow introducers. In practice this means that
/// borrow introducers can not have guaranteed results that are not creating a
/// new borrow scope. No such instructions exist today.
struct BorrowScopeIntroducingValue {
  BorrowScopeIntroducingValueKind kind;
  SILValue value;

  /// If value is a borrow introducer return it after doing some checks.
  ///
  /// This is the only way to construct a BorrowScopeIntroducingValue. We make
  /// the primary constructor private for this reason.
  static Optional<BorrowScopeIntroducingValue> get(SILValue value) {
    auto kind = BorrowScopeIntroducingValueKind::get(value);
    if (!kind)
      return None;
    return BorrowScopeIntroducingValue(*kind, value);
  }

  /// If this value is introducing a local scope, gather all local end scope
  /// instructions and append them to \p scopeEndingInsts. Asserts if this is
  /// called with a scope that is not local.
  ///
  /// NOTE: To determine if a scope is a local scope, call
  /// BorrowScopeIntoducingValue::isLocalScope().
  void getLocalScopeEndingInstructions(
      SmallVectorImpl<SILInstruction *> &scopeEndingInsts) const;

  /// If this value is introducing a local scope, gather all local end scope
  /// instructions and pass them individually to visitor. Asserts if this is
  /// called with a scope that is not local.
  ///
  /// The intention is that this method can be used instead of
  /// BorrowScopeIntroducingValue::getLocalScopeEndingUses() to avoid
  /// introducing an intermediate array when one needs to transform the
  /// instructions before storing them.
  ///
  /// NOTE: To determine if a scope is a local scope, call
  /// BorrowScopeIntoducingValue::isLocalScope().
  void visitLocalScopeEndingUses(function_ref<void(Operand *)> visitor) const;

  bool isLocalScope() const { return kind.isLocalScope(); }

  /// Returns true if the passed in set of instructions is completely within the
  /// lifetime of this borrow introducer.
  ///
  /// NOTE: Scratch space is used internally to this method to store the end
  /// borrow scopes if needed.
  bool
  areInstructionsWithinScope(ArrayRef<SILInstruction *> instructions,
                             SmallVectorImpl<SILInstruction *> &scratchSpace,
                             SmallPtrSetImpl<SILBasicBlock *> &visitedBlocks,
                             DeadEndBlocks &deadEndBlocks) const;

  /// Given a local borrow scope introducer, visit all non-forwarding consuming
  /// users. This means that this looks through guaranteed block arguments.
  bool visitLocalScopeTransitiveEndingUses(
      function_ref<void(Operand *)> visitor) const;

  void print(llvm::raw_ostream &os) const;
  SWIFT_DEBUG_DUMP { print(llvm::dbgs()); }

private:
  /// Internal constructor for failable static constructor. Please do not expand
  /// its usage since it assumes the code passed in is well formed.
  BorrowScopeIntroducingValue(BorrowScopeIntroducingValueKind kind,
                              SILValue value)
      : kind(kind), value(value) {}
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const BorrowScopeIntroducingValue &value);

/// Look up the def-use graph starting at use \p inputOperand, recording any
/// "borrow" introducing values that we find into \p out. If at any point, we
/// find a point in the chain we do not understand, we bail and return false. If
/// we are able to understand all of the def-use graph, we know that we have
/// found all of the borrow introducing values, we return true.
///
/// NOTE: This may return multiple borrow introducing values in cases where
/// there are phi-like nodes in the IR like any true phi block arguments or
/// aggregate literal instructions (struct, tuple, enum, etc.).
bool getAllBorrowIntroducingValues(
    SILValue value, SmallVectorImpl<BorrowScopeIntroducingValue> &out);

/// Look up the def-use graph starting at \p inputOperand and see if
/// we can find a single BorrowScopeIntroducingValue for \p
/// inputOperand. Returns None if there are multiple such introducers
/// or if while processing we find a user we do not understand.
Optional<BorrowScopeIntroducingValue>
getSingleBorrowIntroducingValue(SILValue value);

/// Look up through the def-use chain of \p inputValue, looking for an initial
/// "borrow" introducing value. If at any point, we find two introducers or we
/// find a point in the chain we do not understand, we bail and return false. If
/// we are able to understand all of the def-use graph and only find a single
/// introducer, then we return a .some(BorrowScopeIntroducingValue).
Optional<BorrowScopeIntroducingValue>
getSingleBorrowIntroducingValue(SILValue inputValue);

} // namespace swift

#endif
