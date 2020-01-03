//===--------- BoundsAnalysis.cpp - Bounds Widening Analysis --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements a dataflow analysis for bounds widening.
//
//===----------------------------------------------------------------------===//

#include "clang/Sema/BoundsAnalysis.h"

namespace clang {

void BoundsAnalysis::WidenBounds() {
  assert(Cfg && "expected CFG to exist");

  WorkListTy WorkList;
  BlockMapTy BlockMap;

  // Add each block to WorkList and create a mapping from Block to
  // ElevatedCFGBlock.
  // Note: By default, PostOrderCFGView iterates in reverse order. So we always
  // get a reverse post order when we iterate PostOrderCFGView.
  for (const auto *B : PostOrderCFGView(Cfg)) {
    // SkipBlock will skip all null, entry and exit blocks. PostOrderCFGView
    // does not contain any unreachable blocks. So at the end of this loop
    // BlockMap only contains reachable blocks.
    if (SkipBlock(B))
      continue;

    auto EB = new ElevatedCFGBlock(B);
    // Note: WorkList is a queue. So we maintain the reverse post order when we
    // iterate WorkList.
    WorkList.append(EB);
    BlockMap[B] = EB;
  }

  // At this time, BlockMap only contains reachable blocks. We iterate through
  // all blocks in the CFG and append all unreachable blocks to the WorkList.
  for (auto I = Cfg->begin(), E = Cfg->end(); I != E; ++I) {
    const auto *B = *I;
    if (!SkipBlock(B) && !BlockMap.count(B)) {
      auto EB = new ElevatedCFGBlock(B);
      WorkList.append(EB);
      BlockMap[B] = EB;
    }
  }

  // Compute Gen and Kill sets.
  ComputeGenSets(BlockMap);
  ComputeKillSets(BlockMap);

  // Compute In and Out sets.
  while (!WorkList.empty()) {
    auto *EB = WorkList.next();
    WorkList.remove(EB);

    ComputeInSets(EB, BlockMap);
    ComputeOutSets(EB, BlockMap, WorkList);
  }

  CollectWidenedBounds(BlockMap);
}

void BoundsAnalysis::ComputeGenSets(BlockMapTy BlockMap) {
  // If there is an edge B1->B2 and the edge condition is of the form
  // "if (*(p + i))" then Gen[B1] = {B2, p:i} .

  for (const auto B : BlockMap) {
    auto EB = B.second;

    for (const CFGBlock *pred : EB->Block->preds()) {
      if (SkipBlock(pred))
        continue;

      // We can add "p:i" only on the true edge.
      // For example,
      // B1: if (*(p + i))
      // B2:   foo();
      // B3: else bar();

      // Here we have the edges (B1->B2) and (B1->B3). We can add "p:i" only
      // on the true edge. Which means we will add the following entry to
      // Gen[B1]: {B2, p:i}
      if (const auto *I = pred->succs().begin())
        if (*I != EB->Block)
          continue;

      // Get the edge condition and fill the Gen set.
      if (Expr *E = GetTerminatorCondition(pred))
        FillGenSet(E, BlockMap[pred], EB);
    }
  }
}

void BoundsAnalysis::CollectBoundsVars(const Expr *E, DeclSetTy &BoundsVars) {
  if (!E)
    return;

  E = IgnoreCasts(const_cast<Expr *>(E));

  // Collect bounds vars for the lower and upper bounds exprs.
  // Example:
  // _Nt_array_ptr<char> p : bounds(p + i, p + j);
  // LowerExpr: p + i.
  // UpperExpr: p + j.
  if (const auto *RBE = dyn_cast<RangeBoundsExpr>(E)) {
    CollectBoundsVars(RBE->getLowerExpr(), BoundsVars);
    CollectBoundsVars(RBE->getUpperExpr(), BoundsVars);
  }

  // Collect bounds vars for the LHS and RHS of binary expressions.
  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    CollectBoundsVars(BO->getLHS(), BoundsVars);
    CollectBoundsVars(BO->getRHS(), BoundsVars);
  }

  if (const auto *D = dyn_cast<DeclRefExpr>(E)) {
    if (const auto *V = dyn_cast<VarDecl>(D->getDecl()))
      BoundsVars.insert(V);
  }
}

bool BoundsAnalysis::AreDeclaredBoundsZero(const Expr *E, const Expr *V) {
  if (!E)
    return !V;

  E = IgnoreCasts(const_cast<Expr *>(E));

  // Check if the upper bound of V is equal to V.
  // To do this, we check that the LHS of the bounds expr is V and the RHS is
  // 0.
  if (const auto *RBE = dyn_cast<RangeBoundsExpr>(E)) {
    if (const auto *BO = dyn_cast<BinaryOperator>(RBE->getUpperExpr())) {
      auto *RHS = IgnoreCasts(BO->getRHS());
      if (const auto *Lit = dyn_cast<IntegerLiteral>(RHS)) {
        auto *LHS = IgnoreCasts(BO->getLHS());
        return Lit->getValue().getLimitedValue() == 0 &&
               Lexicographic(Ctx, nullptr).CompareExpr(LHS, V) ==
               Lexicographic::Result::Equal;
      }
    }
  }
  return false;
}

void BoundsAnalysis::FillGenSet(Expr *E,
                                ElevatedCFGBlock *EB,
                                ElevatedCFGBlock *SuccEB) {

  // Handle if conditions of the form "if (*e1 && *e2)".
  if (const auto *BO = dyn_cast<const BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_LAnd) {
      FillGenSet(BO->getLHS(), EB, SuccEB);
      FillGenSet(BO->getRHS(), EB, SuccEB);
    }
  }

  // Check if the edge condition contains a pointer deref.
  if (!ContainsPointerDeref(E))
    return;

  E = IgnoreCasts(E);

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    const auto *Exp = IgnoreCasts(UO->getSubExpr());
    if (!Exp)
      return;

    // TODO: Handle accesses of the form:
    // "if (*(p + i) && *(p + j) && *(p + k))"

    // For conditions of the form "if (*p)".
    if (const auto *D = dyn_cast<DeclRefExpr>(Exp)) {
      // TODO: Remove this check. Currently, for the first version of this
      // algorithm, we are enabling bounds widening only when the declared
      // bounds are bounds(p, p) or count(0). We need to generalize this to
      // widen bounds for dereferences involving constant offsets from the
      // declared upper bound of a variable.
      if (!AreDeclaredBoundsZero(UO->getBoundsExpr(), D))
        return;

      if (const auto *V = dyn_cast<VarDecl>(D->getDecl())) {
        if (V->getType()->isCheckedPointerNtArrayType()) {
          EB->Gen[SuccEB->Block].insert(std::make_pair(V, 0));
          if (!SuccEB->BoundsVars.count(V)) {
            DeclSetTy BoundsVars;
            CollectBoundsVars(UO->getBoundsExpr(), BoundsVars);
            SuccEB->BoundsVars[V] = BoundsVars;
          }
        }
      }

    // For conditions of the form "if (*(p + i))"
    } else if (const auto *BO = dyn_cast<BinaryOperator>(Exp)) {
      // Currently we only handle additive offsets.
      if (BO->getOpcode() != BO_Add)
        return;

      Expr *LHS = IgnoreCasts(BO->getLHS());
      Expr *RHS = IgnoreCasts(BO->getRHS());
      DeclRefExpr *D = nullptr;
      IntegerLiteral *Lit = nullptr;

      // Handle *(p + i).
      if (isa<DeclRefExpr>(LHS) && isa<IntegerLiteral>(RHS)) {
        D = dyn_cast<DeclRefExpr>(LHS);
        Lit = dyn_cast<IntegerLiteral>(RHS);

      // Handle *(i + p).
      } else if (isa<DeclRefExpr>(RHS) && isa<IntegerLiteral>(LHS)) {
        D = dyn_cast<DeclRefExpr>(RHS);
        Lit = dyn_cast<IntegerLiteral>(LHS);
      }

      if (!D || !Lit)
        return;

      // TODO: Remove this check. Currently, for the first version of this
      // algorithm, we are enabling bounds widening only when the declared
      // bounds are bounds(p, p) or count(0). We need to generalize this to
      // widen bounds for dereferences involving constant offsets from the
      // declared upper bound of a variable.
      if (!AreDeclaredBoundsZero(UO->getBoundsExpr(), D))
        return;

      if (const auto *V = dyn_cast<VarDecl>(D->getDecl())) {
        if (V->getType()->isCheckedPointerNtArrayType()) {
          // We update the bounds of p on the edge EB->SuccEB only if this is
          // the first time we encounter "if (*(p + i)" on that edge.
          if (!EB->Gen[SuccEB->Block].count(V)) {
            EB->Gen[SuccEB->Block][V] = Lit->getValue().getLimitedValue();
            if (!SuccEB->BoundsVars.count(V)) {
              DeclSetTy BoundsVars;
              CollectBoundsVars(UO->getBoundsExpr(), BoundsVars);
              SuccEB->BoundsVars[V] = BoundsVars;
            }
          }
        }
      }
    }
  }
}

void BoundsAnalysis::ComputeKillSets(BlockMapTy BlockMap) {
  // For a block B, a variable v is added to Kill[B] if v is assigned to in B.

  for (const auto B : BlockMap) {
    auto EB = B.second;
    DeclSetTy DefinedVars;

    for (auto Elem : *(EB->Block))
      if (Elem.getKind() == CFGElement::Statement)
        CollectDefinedVars(Elem.castAs<CFGStmt>().getStmt(), EB, DefinedVars);

    for (const auto V : DefinedVars)
      EB->Kill.insert(V);
  }
}

void BoundsAnalysis::CollectDefinedVars(const Stmt *S, ElevatedCFGBlock *EB,
                                        DeclSetTy &DefinedVars) {
  if (!S)
    return;

  Expr *E = nullptr;
  if (const auto *UO = dyn_cast<const UnaryOperator>(S)) {
    if (UO->isIncrementDecrementOp())
      E = IgnoreCasts(UO->getSubExpr());
  } else if (const auto *BO = dyn_cast<const BinaryOperator>(S)) {
    if (BO->isAssignmentOp())
      E = IgnoreCasts(BO->getLHS());
  }

  if (E) {
    if (const auto *D = dyn_cast<DeclRefExpr>(E)) {
      if (const auto *V = dyn_cast<VarDecl>(D->getDecl())) {
        if (V->getType()->isCheckedPointerNtArrayType())
          DefinedVars.insert(V);
        else {

          // BoundsVars is a mapping from _Nt_array_ptrs to all the variables
          // used in their bounds exprs. For example:

          // _Nt_array_ptr<char> p : bounds(p + i, i + p + j + 10); 
          // _Nt_array_ptr<char> q : bounds(i + q, i + p + q + m);

          // EB->BoundsVars: {p: {p, i, j}, q: {i, q, p, m}}
  
          for (auto item : EB->BoundsVars) {
            if (item.second.count(V))
              DefinedVars.insert(item.first);
          }
        }
      }
    }
  }

  for (const auto I : S->children())
    CollectDefinedVars(I, EB, DefinedVars);
}

void BoundsAnalysis::ComputeInSets(ElevatedCFGBlock *EB, BlockMapTy BlockMap) {
  // In[B1] = n Out[B*->B1], where B* are all preds of B1.

  BoundsMapTy Intersections;
  bool ItersectionEmpty = true;

  for (const CFGBlock *pred : EB->Block->preds()) {
    if (SkipBlock(pred))
      continue;

    auto PredEB = BlockMap[pred];

    if (ItersectionEmpty) {
      Intersections = PredEB->Out[EB->Block];
      ItersectionEmpty = false;
    } else
      Intersections = Intersect(Intersections, PredEB->Out[EB->Block]);
  }

  EB->In = Intersections;
}

void BoundsAnalysis::ComputeOutSets(ElevatedCFGBlock *EB,
                                    BlockMapTy BlockMap,
                                    WorkListTy &WorkList) {
  // Out[B1->B2] = (In[B1] - Kill[B1]) u Gen[B1->B2].

  auto Diff = Difference(EB->In, EB->Kill);
  for (const CFGBlock *succ : EB->Block->succs()) {
    if (SkipBlock(succ))
      continue;

    auto OldOut = EB->Out[succ];

    // Here's how we compute (In - Kill) u Gen:

    // 1. If variable p does not exist in (In - Kill), then
    // (Gen[p] == 0) ==> Out[B1->B2] = {p:1}.
    // In other words, if p does not exist in (In - Kill) it means that p is
    // dereferenced for the first time on the incoming edge to this block, like
    // "if (*p)". So we can initialize the bounds of p to 1. But we may also
    // run into cases like "if (*(p + 100))". In this case, we cannot
    // initialize the bounds of p. So additionally we check if Gen[p] == 0.

    // 2. Else if the bounds of p in (In - Kill) == Gen[V] then widen the
    // bounds of p by 1.
    // Consider this example:
    // B1: if (*p) { // In[B1] = {}, Gen[Entry->B1] = {} ==> bounds(p) = 1.
    // B2:   if (*(p + 1)) { // In[B2] = {p:1}, Gen[B1->B2] = {p:1} ==> bounds(p) = 2.
    // B3:     if (*(p + 2)) { // In[B2] = {p:2}, Gen[B1->B2] = {p:2} ==> bounds(p) = 3.

    EB->Out[succ] = Union(Diff, EB->Gen[succ]);

    if (Differ(OldOut, EB->Out[succ]))
      WorkList.append(BlockMap[succ]);
  }
}

void BoundsAnalysis::CollectWidenedBounds(BlockMapTy BlockMap) {
  for (auto item : BlockMap) {
    const auto *B = item.first;
    auto *EB = item.second;
    WidenedBounds[B] = EB->In;
    delete EB;
  }
}

BoundsMapTy BoundsAnalysis::GetWidenedBounds(const CFGBlock *B) {
  return WidenedBounds[B];
}

Expr *BoundsAnalysis::GetTerminatorCondition(const CFGBlock *B) const {
  if (const Stmt *S = B->getTerminator()) {
    if (const auto *IfS = dyn_cast<IfStmt>(S))
      return const_cast<Expr *>(IfS->getCond());
  }
  return nullptr;
}

Expr *BoundsAnalysis::IgnoreCasts(Expr *E) {
  while (E) {
    E = E->IgnoreParens();

    if (isa<ParenExpr>(E)) {
      E = E->IgnoreParenCasts();
      continue;
    }

    if (isa<ImplicitCastExpr>(E)) {
      E = E->IgnoreImplicit();
      continue;
    }

    if (auto *CE = dyn_cast<CastExpr>(E)) {
      E = CE->getSubExpr();
      continue;
    }
    return E;
  }
  return E;
}


bool BoundsAnalysis::IsPointerDerefLValue(Expr *E) const {
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return UO->getOpcode() == UO_Deref;
  return false;
}

bool BoundsAnalysis::ContainsPointerDeref(Expr *E) const {
  if (auto *CE = dyn_cast<CastExpr>(E)) {
    if (CE->getCastKind() == CastKind::CK_LValueToRValue)
      return IsPointerDerefLValue(CE->getSubExpr());
    return ContainsPointerDeref(CE->getSubExpr());
  }
  return false;
}

template<class T>
T BoundsAnalysis::Intersect(T &A, T &B) const {
  if (!A.size())
    return A;

  auto Ret = A;
  if (!B.size()) {
    Ret.clear();
    return Ret;
  }

  for (auto I = Ret.begin(), E = Ret.end(); I != E;) {
    const auto *V = I->first;

    if (!B.count(V)) {
      auto Next = std::next(I);
      Ret.erase(I);
      I = Next;
    } else {
      Ret[V] = std::min(Ret[V], B[V]);
      ++I;
    }
  }
  return Ret;
}

template<class T>
T BoundsAnalysis::Union(T &A, T &B) const {
  auto Ret = A;
  for (const auto item : B) {
    const auto *V = item.first;
    auto I = item.second;

    if (!Ret.count(V)) {
      if (I == 0)
        Ret[V] = 1;
    } else if (I == Ret[V])
      Ret[V] = I + 1;
  }
  return Ret;
}

template<class T, class U>
T BoundsAnalysis::Difference(T &A, U &B) const {
  if (!A.size() || !B.size())
    return A;

  auto Ret = A;
  for (auto I = Ret.begin(), E = Ret.end(); I != E; ) {
    const auto *V = I->first;
    if (B.count(V)) {
      auto Next = std::next(I);
      Ret.erase(I);
      I = Next;
    } else ++I;
  }
  return Ret;
}

template<class T>
bool BoundsAnalysis::Differ(T &A, T &B) const {
  if (A.size() != B.size())
    return true;
  auto Ret = Intersect(A, B);
  return Ret.size() != A.size();
}

OrderedBlocksTy BoundsAnalysis::GetOrderedBlocks() {
  // WidenedBounds is a DenseMap and hence is not suitable for iteration as its
  // iteration order is non-deterministic. So we first need to order the
  // blocks. The block IDs decrease from entry to exit. So we sort in the
  // reverse order.
  OrderedBlocksTy OrderedBlocks;
  for (auto item : WidenedBounds)
    OrderedBlocks.push_back(item.first);

  llvm::sort(OrderedBlocks.begin(), OrderedBlocks.end(),
             [] (const CFGBlock *A, const CFGBlock *B) {
               return A->getBlockID() > B->getBlockID();
             });
  return OrderedBlocks;
}

bool BoundsAnalysis::SkipBlock(const CFGBlock *B) const {
  return !B || B == &Cfg->getEntry() || B == &Cfg->getExit();
}

void BoundsAnalysis::DumpWidenedBounds(FunctionDecl *FD) {
  llvm::outs() << "--------------------------------------\n";
  llvm::outs() << "In function: " << FD->getName() << "\n";

  for (const auto *B : GetOrderedBlocks()) {
    llvm::outs() << "--------------------------------------";
    B->print(llvm::outs(), Cfg, S.getLangOpts(), /* ShowColors */ true);

    // WidenedBounds[B] is a MapVector whose iteration order is the same as the
    // insertion order. So we can deterministically iterate the VarDecls.
    for (auto item : WidenedBounds[B])
      llvm::outs() << "upper_bound("
                   << item.first->getNameAsString() << ") = "
                   << item.second << "\n";
  }
}

} // end namespace clang
