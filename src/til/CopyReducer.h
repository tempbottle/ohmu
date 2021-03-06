//===- CopyReducer.h -------------------------------------------*- C++ --*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License.  See LICENSE.TXT in the LLVM repository for details.
//
//===----------------------------------------------------------------------===//
//
// CopyReducer extends AttributeGrammar, and implements the reducer interface
// to make a deep copy of a term.
//
// It is also useful as a base class for more complex non-destructive term
// rewriting operations.
//
//===----------------------------------------------------------------------===//

#ifndef OHMU_TIL_COPYREDUCER_H
#define OHMU_TIL_COPYREDUCER_H

#include "AttributeGrammar.h"
#include "CFGBuilder.h"

#include <cstddef>
#include <memory>
#include <queue>
#include <vector>


namespace ohmu {
namespace til  {



/// A CopyScope maintains a map from blocks to rewritten blocks in
/// addition to the variable maps maintained by ScopeFrame.
template<class Attr, typename LocStateT=int>
class CopyScope : public ScopeFrame<Attr, LocStateT> {
public:
  typedef ScopeFrame<Attr, LocStateT> Super;

  /// Return the block that Orig maps to in CFG rewriting.
  BasicBlock* lookupBlock(BasicBlock *Orig) {
    return BlockMap[Orig->blockID()];
  }

  /// Enter a new CFG, mapping blocks from Orig to blocks in S.
  void enterCFG(SCFG *Orig, SCFG *S) {
    Super::enterCFG(Orig);

    BlockMap.resize(Orig->numBlocks(), nullptr);
    insertBlockMap(Orig->entry(), S->entry());
    insertBlockMap(Orig->exit(),  S->exit());
  }

  void exitCFG() {
    Super::exitCFG();
    BlockMap.clear();
  }

  // Add B to BlockMap, and add its arguments to the instruction map
  void insertBlockMap(BasicBlock *Orig, BasicBlock *B) {
    this->BlockMap[Orig->blockID()] = B;

    // Map the arguments of Orig to the arguments of B.
    unsigned Nargs = Orig->arguments().size();
    assert(Nargs == B->arguments().size() && "Block arguments don't match.");

    for (unsigned i = 0; i < Nargs; ++i) {
      Phi *Ph = Orig->arguments()[i];
      if (Ph && Ph->instrID() > 0)
        this->insertInstructionMap(Ph, Attr( B->arguments()[i] ));
    }
  }

  /// Create a copy of this scope.  (Used for lazy rewriting)
  CopyScope* clone() { return new CopyScope(*this); }

  CopyScope() { }

  CopyScope(Substitution<Attr> &&Subst) : Super(std::move(Subst)) { }

protected:
  CopyScope(const CopyScope& S) = default;

  std::vector<BasicBlock*> BlockMap;    // map blocks to rewritten blocks.
};



/// CopyReducer implements the reducer interface to build a new SExpr.
/// In other words, it makes a deep copy of a term.
/// It also useful as a base class for non-destructive rewrites.
/// It automatically performs variable substitution during the copy.
template<class Attr = CopyAttr, class ScopeT = CopyScope<Attr>>
class CopyReducer : public AttributeGrammar<Attr, ScopeT> {
public:
  MemRegionRef& arena() { return Builder.arena(); }

  void enterScope(VarDecl *Vd) {
    // enterScope must be called immediately after reduceVarDecl()
    auto* Nvd = cast<VarDecl>( this->lastAttr().Exp );
    auto* Nv  = Builder.newVariable(Nvd);

    // Variables that point to Orig will be replaced with Nv.
    Builder.enterScope(Nvd);
    this->scope()->enterScope(Vd, Attr(Nv));
  }

  void exitScope(VarDecl *Vd) {
    Builder.exitScope();
    this->scope()->exitScope();
  }

  void enterCFG(SCFG *Cfg) {
    if (Cfg) {
      // We are rewriting a CFG to a CFG.
      Builder.beginCFG(nullptr);
      this->scope()->enterCFG(Cfg, Builder.currentCFG());
    }
    else {
      // We are converting an ordinary term to a CFG.
      Builder.beginCFG(nullptr);
    }
  }

  void exitCFG(SCFG *Cfg) {
    Builder.endCFG();
    this->scope()->exitCFG();
  }

  void enterBlock(BasicBlock *B) {
    Builder.beginBlock( lookupBlock(B) );
  }

  void exitBlock(BasicBlock *B) {
    // Sanity check; the terminator should end the block.
    if (Builder.currentBB())
      Builder.endBlock(nullptr);
  }

  /// Find the basic block that Orig maps to, or create a new one.
  BasicBlock* lookupBlock(BasicBlock *Orig) {
    if (!Orig)
      return nullptr;

    auto *B2 = this->scope()->lookupBlock(Orig);
    if (!B2) {
      // Create new blocks on demand.
      unsigned Nargs = Orig->arguments().size();
      B2 = Builder.newBlock(Nargs, Orig->numPredecessors());
      this->scope()->insertBlockMap(Orig, B2);
    }
    return B2;
  }

  /*--- Reduce Methods ---*/

  /// Reduce a null pointer.
  void reduceNull() {
    this->resultAttr().Exp = nullptr;
  }

  void reduceWeak(Instruction *Orig) {
    unsigned Idx = Orig->instrID();
    this->resultAttr() = this->scope()->instr(Idx);
  }

  void reduceBBArgument(Phi *Ph) { }  // Arguments are handled in lookupBlock

  void reduceBBInstruction(Instruction *I) {
    this->scope()->insertInstructionMap(I, std::move(this->lastAttr()));
  }

  void reduceVarDecl(VarDecl *Orig) {
    auto *E = this->attr(0).Exp;
    VarDecl *Nvd = Builder.newVarDecl(Orig->kind(), Orig->varName(), E);
    this->resultAttr().Exp = Nvd;
  }

  void reduceFunction(Function *Orig) {
    VarDecl *Nvd = cast<VarDecl>(this->attr(0).Exp);
    auto *E0 = this->attr(1).Exp;
    this->resultAttr().Exp = Builder.newFunction(Nvd, E0);
  }

  void reduceCode(Code *Orig) {
    auto *E0 = this->attr(0).Exp;
    auto *E1 = this->attr(1).Exp;
    auto *Res = Builder.newCode(E0, E1);
    Res->setCallingConvention(Orig->callingConvention());
    this->resultAttr().Exp = Res;
  }

  void reduceField(Field *Orig) {
    auto *E0 = this->attr(0).Exp;
    auto *E1 = this->attr(1).Exp;
    this->resultAttr().Exp = Builder.newField(E0, E1);
  }

  void reduceSlot(Slot *Orig) {
    auto *E0 = this->attr(0).Exp;
    auto *Res = Builder.newSlot(Orig->slotName(), E0);
    Res->setModifiers(Orig->modifiers());
    this->resultAttr().Exp = Res;
  }

  void reduceRecord(Record *Orig) {
    unsigned Ns = this->numAttrs() - 1;
    assert(Ns == Orig->slots().size());
    auto *Res = Builder.newRecord(Ns, this->attr(0).Exp);
    for (unsigned i = 0; i < Ns; ++i) {
      Slot *S = cast<Slot>( this->attr(i+1).Exp );
      Res->addSlot(arena(), S);
    }
    this->resultAttr().Exp = Res;
  }

  void reduceArray(Array *Orig) {
    auto *E0 = this->attr(0).Exp;
    auto *E1 = this->attr(1).Exp;
    Array* Arr;

    if (Orig->concrete()) {
      uint64_t Nelems = Orig->numElements();
      Arr = Builder.newArray(E0, Nelems);
      for (uint64_t i=0; i < Nelems; ++i) {
        Arr->elements()[i].reset( this->attr(i+2).Exp );
      }
    }
    else {
      Arr = Builder.newArray(E0, E1);
    }
    this->resultAttr().Exp = Arr;
  }

  void reduceScalarType(ScalarType *Orig) {
    // Scalar types are globally defined; we share pointers.
    this->resultAttr().Exp = Orig;
  }

  void reduceLiteral(Literal *Orig) {
    this->resultAttr().Exp = new (arena()) Literal(*Orig);
  }

  template<class T>
  void reduceLiteralT(LiteralT<T> *Orig) {
    this->resultAttr().Exp = Builder.newLiteralT<T>(Orig->value());
  }

  void reduceVariable(Variable *Orig) {
    unsigned Idx = Orig->variableDecl()->varIndex();
    if (this->scope()->isNull(Idx)) {
      // Null substitution: just copy the variable.
      this->resultAttr() = Attr( Builder.newVariable(Orig->variableDecl()) );
    }
    else {
      // Substitute for variable.
      this->resultAttr() = this->scope()->var(Idx);
    }
  }

  void reduceApply(Apply *Orig) {
    auto *E0 = this->attr(0).Exp;
    auto *E1 = this->attr(1).Exp;
    this->resultAttr().Exp = Builder.newApply(E0, E1, Orig->applyKind());
  }

  void reduceProject(Project *Orig) {
    auto *E0  = this->attr(0).Exp;
    auto *Res = Builder.newProject(E0, Orig->slotName());
    Res->setArrow(Orig->isArrow());
    this->resultAttr().Exp = Res;
  }

  void reduceCall(Call *Orig) {
    auto *E0  = this->attr(0).Exp;
    auto *Res = Builder.newCall(E0);
    Res->setCallingConvention(Res->callingConvention());
    this->resultAttr().Exp = Res;
  }

  void reduceAlloc(Alloc *Orig) {
    auto *E0 = this->attr(0).Exp;
    this->resultAttr().Exp = Builder.newAlloc(E0, Orig->allocKind());
  }

  void reduceLoad(Load *Orig) {
    auto *E0 = this->attr(0).Exp;
    this->resultAttr().Exp = Builder.newLoad(E0);
  }

  void reduceStore(Store *Orig) {
    auto *E0 = this->attr(0).Exp;
    auto *E1 = this->attr(1).Exp;
    this->resultAttr().Exp = Builder.newStore(E0, E1);
  }

  void reduceArrayIndex(ArrayIndex *Orig) {
    auto *E0 = this->attr(0).Exp;
    auto *E1 = this->attr(1).Exp;
    this->resultAttr().Exp = Builder.newArrayIndex(E0, E1);
  }

  void reduceArrayAdd(ArrayAdd *Orig) {
    auto *E0 = this->attr(0).Exp;
    auto *E1 = this->attr(1).Exp;
    this->resultAttr().Exp = Builder.newArrayAdd(E0, E1);
  }

  void reduceUnaryOp(UnaryOp *Orig) {
    auto *E0 = this->attr(0).Exp;
    this->resultAttr().Exp = Builder.newUnaryOp(Orig->unaryOpcode(), E0);
  }

  void reduceBinaryOp(BinaryOp *Orig) {
    auto *E0 = this->attr(0).Exp;
    auto *E1 = this->attr(1).Exp;
    this->resultAttr().Exp = Builder.newBinaryOp(Orig->binaryOpcode(), E0, E1);
  }

  void reduceCast(Cast *Orig) {
    auto *E0 = this->attr(0).Exp;
    this->resultAttr().Exp = Builder.newCast(Orig->castOpcode(), E0);
  }

  // Phi nodes are created in lookupBlock().
  void reducePhi(Phi *Orig) { }

  void reduceGoto(Goto *Orig) {
    BasicBlock *B = lookupBlock(Orig->targetBlock());
    unsigned Idx = B->addPredecessor(Builder.currentBB());
    Goto *Res = new (arena()) Goto(B, Idx);

    // All "arguments" to the Goto should have been pushed onto the stack.
    // Write them to their appropriate Phi nodes.
    assert(B->arguments().size() == this->numAttrs());
    unsigned i = 0;
    for (Phi *Ph : B->arguments()) {
      Builder.setPhiArgument(Ph, this->attr(i).Exp, Idx);
      ++i;
    }

    Builder.endBlock(Res);
    this->resultAttr().Exp = Res;
  }

  void reduceBranch(Branch *Orig) {
    auto *C = this->attr(0).Exp;
    BasicBlock *B0 = lookupBlock(Orig->thenBlock());
    BasicBlock *B1 = lookupBlock(Orig->elseBlock());
    this->resultAttr().Exp = Builder.newBranch(C, B0, B1);
  }

  void reduceSwitch(Switch *Orig) {
    int Nc = Orig->numCases();
    auto *C = this->attr(0).Exp;
    auto *E = Builder.newSwitch(C, Nc);
    for (int i=0; i < Nc; ++i) {
      auto *L = this->attr(i+1).Exp;
      Builder.addSwitchCase(E, L, lookupBlock(Orig->caseBlock(i)));
    }
    this->resultAttr().Exp = E;
  }

  void reduceReturn(Return *Orig) {
    auto *E = this->attr(0).Exp;
    this->resultAttr().Exp = Builder.newReturn(E);
  }

  void reduceBasicBlock(BasicBlock *Orig) {
    // We don't return a result, because the basic block should have ended
    // with the terminator.
    this->resultAttr().Exp = nullptr;
  }

  void reduceSCFG(SCFG *Orig) {
    this->resultAttr().Exp = Builder.currentCFG();;
  }

  void reduceUndefined(Undefined *Orig) {
    this->resultAttr().Exp = Builder.newUndefined();
  }

  void reduceWildcard(Wildcard *Orig) {
    this->resultAttr().Exp = Builder.newWildcard();
  }

  void reduceIdentifier(Identifier *Orig) {
    this->resultAttr().Exp =
      new (arena()) Identifier(Orig->idString());
  }

  void reduceLet(Let *Orig) {
    VarDecl *Nvd = cast<VarDecl>( this->attr(0).Exp );
    auto    *E   = this->attr(1).Exp;
    this->resultAttr().Exp = Builder.newLet(Nvd, E);
  }

  void reduceIfThenElse(IfThenElse *Orig) {
    auto *C = this->attr(0).Exp;
    auto *T = this->attr(1).Exp;
    auto *E = this->attr(2).Exp;
    this->resultAttr().Exp = Builder.newIfThenElse(C, T, E);
  }

  template <class T>
  void reduceAnnotationT(T *A) {
    this->ResultAnn = A->copy(this->Builder, this->attributesAsSExpr());
  }

  std::vector<SExpr*> attributesAsSExpr() {
    std::vector<SExpr*> Res;
    for (unsigned int i = 0; i < this->numAttrs(); ++i) {
      Res.push_back(this->attr(i).Exp);
    }
    return Res;
  }

public:
  CopyReducer()
    : AttributeGrammar<Attr, ScopeT>(new ScopeT()), ResultAnn(nullptr)
  { }
  CopyReducer(MemRegionRef A)
    : AttributeGrammar<Attr, ScopeT>(new ScopeT()), Builder(A),
      ResultAnn(nullptr)
  { }
  ~CopyReducer() { }

public:
  CFGBuilder Builder;
  Annotation* ResultAnn;
};



/// An implementation of Future for lazy, non-destructive traversals.
/// Visitor extends CopyReducer.
template<class Visitor, class ScopeT>
class LazyCopyFuture : public Future {
public:
  typedef CFGBuilder::BuilderState BuilderState;

  LazyCopyFuture(Visitor* R, SExpr* E, ScopeT* S, const BuilderState& Bs,
                 bool NewCfg = false)
    : Reducer(R), PendingExpr(E), ScopePtr(S), BState(Bs), CreateCfg(NewCfg)
  { }
  virtual ~LazyCopyFuture() { }

  /// Traverse PendingExpr and return the result.
  virtual SExpr* evaluate() override {
    auto* S  = Reducer->switchScope(ScopePtr);
    auto  Bs = Reducer->Builder.switchState(BState);

    if (CreateCfg) {
      Reducer->enterCFG(nullptr);
    }

    Reducer->traverse(PendingExpr, TRV_Tail);
    SExpr* Res = Reducer->lastAttr().Exp;
    Reducer->popAttr();

    if (CreateCfg) {
      Res = Reducer->Builder.currentCFG();
      Reducer->exitCFG(nullptr);
    }

    Reducer->Builder.restoreState(Bs);
    Reducer->restoreScope(S);
    finish();
    return Res;
  }

protected:
  void finish() {
    if (ScopePtr)
      delete ScopePtr;
    ScopePtr = nullptr;
    PendingExpr = nullptr;
  }

  Visitor*     Reducer;      // The reducer object.
  SExpr*       PendingExpr;  // The expression to be rewritten
  ScopeT*      ScopePtr;     // The scope in which it occurs
  BuilderState BState;       // The builder state.
  bool         CreateCfg;    // Evaluate in a new CFG?
};



/// Base class for non-destructive, lazy traversals.
template<class Self, class ScopeT,
         class FutureType = LazyCopyFuture<Self, ScopeT>>
class LazyCopyTraversal : public AGTraversal<Self> {
public:
  typedef AGTraversal<Self> SuperTv;

  Self* self() { return static_cast<Self*>(this); }

  /// Factory method to create a future in the current context.
  /// Default implementation works for LazyFuture; override for other types.
  FutureType* makeFuture(SExpr *E) {
    CFGBuilder::BuilderState Bs = self()->Builder.currentState();
    // We need to turn off emit in a lazy or type position;
    // it's easiest to do that here.
    Bs.EmitInstrs = false;
    auto *F = new (self()->arena())
      FutureType(self(), E, self()->scope()->clone(), Bs);
    FutureQueue.push(F);
    return F;
  }

  /// Traverse E, returning a future if K == TRV_Lazy.
  template <class T>
  void traverse(T *E, TraversalKind K) {
    if ((K == TRV_Lazy || K == TRV_Type) && !E->isValue()) {
      auto *A = self()->pushAttr();
      A->Exp = self()->makeFuture(E);
      return;
    }
    SuperTv::traverse(E, K);
  }

  void traverseAnnotation(Annotation *A) {
    unsigned Af = self()->pushAttrFrame();
    self()->traverseAnnotationByKind(A);
    self()->restoreAttrFrame(Af);
    self()->resultAttr().Exp->addAnnotation(self()->ResultAnn);
  }

  /// Perform a lazy traversal.
  SExpr* traverseAll(SExpr *E) {
    assert(self()->emptyAttrs() && "In the middle of a traversal.");

    self()->traverse(E, TRV_Tail);
    SExpr *Result = self()->attr(0).Exp;
    self()->popAttr();

    // Force any SExprs that were rewritten lazily.
    while (!FutureQueue.empty()) {
      auto *F = FutureQueue.front();
      FutureQueue.pop();
      F->force();
    }

    self()->clearAttrFrames();
    return Result;
  }

protected:
  std::queue<FutureType*> FutureQueue;
};



typedef CopyScope<CopyAttr> DefaultCopyScope;

/// This class will make a deep copy of a term.
class SExprCopier : public CopyReducer<CopyAttr, DefaultCopyScope>,
                    public LazyCopyTraversal<SExprCopier, DefaultCopyScope> {
public:
  SExprCopier(MemRegionRef A) : CopyReducer(A) { }

  static SExpr* copy(SExpr *E, MemRegionRef A) {
    SExprCopier Copier(A);
    return Copier.traverseAll(E);
  }
};



}  // end namespace til
}  // end namespace ohmu


#endif  // OHMU_TIL_COPYREDUCER_H
