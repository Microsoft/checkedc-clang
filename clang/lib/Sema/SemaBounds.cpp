//===---------- SemaBounds.cpp - Operations On Bounds Expressions --------===//
//
//                     The LLVM Compiler Infrastructure
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements operations on bounds expressions for semantic analysis.
//  The operations include:
//  * Abstracting bounds expressions so that they can be used in function types.
//    This also checks that requirements on variable references are met and
//    emit diagnostics if they are not.
//
//    The abstraction also removes extraneous details:
//    - References to ParamVarDecl's are abstracted to positional index numbers
//      in argument lists.
//    - References to other VarDecls's are changed to use canonical
//      declarations.
//
//    Line number information is left in place for expressions, though.  It
//    would be a lot of work to write functions to change the line numbers to
//    the invalid line number. The canonicalization of types ignores line number
//    information in determining if two expressions are the same.  Users of bounds
//    expressions that have been abstracted need to be aware that line number
//    information may be inaccurate.
//  * Concretizing bounds expressions from function types.  This undoes the
//    abstraction by substituting parameter varaibles for the positional index
//    numbers.
//
//  Debugging pre-processor flags:
//    - TRACE_CFG:
//      Dumps AST and CFG of the visited nodes when traversing the CFG.
//    - TRACE_RANGE:
//      Dumps the valid bounds ranges, memory access ranges and memory
//      access expressions.
//===----------------------------------------------------------------------===//

#include "clang/Analysis/CFG.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/AST/CanonBounds.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Sema/AvailableFactsAnalysis.h"
#include "clang/Sema/BoundsAnalysis.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "TreeTransform.h"
#include <queue>

// #define TRACE_CFG 1
// #define TRACE_RANGE 1

using namespace clang;
using namespace sema;

namespace {
class BoundsUtil {
public:
  static bool IsStandardForm(const BoundsExpr *BE) {
    BoundsExpr::Kind K = BE->getKind();
    return (K == BoundsExpr::Kind::Any || K == BoundsExpr::Kind::Unknown ||
      K == BoundsExpr::Kind::Range || K == BoundsExpr::Kind::Invalid);
  }

  static Expr *IgnoreRedundantCast(ASTContext &Ctx, CastKind NewCK, Expr *E) {
    CastExpr *P = dyn_cast<CastExpr>(E);
    if (!P)
      return E;

    CastKind ExistingCK = P->getCastKind();
    Expr *SE = P->getSubExpr();
    if (NewCK == CK_BitCast && ExistingCK == CK_BitCast)
      return SE;

    return E;
  }

  static bool getReferentSizeInChars(ASTContext &Ctx, QualType Ty, llvm::APSInt &Size) {
    assert(Ty->isPointerType());
    const Type *Pointee = Ty->getPointeeOrArrayElementType();
    if (Pointee->isIncompleteType())
      return false;
    uint64_t ElemBitSize = Ctx.getTypeSize(Pointee);
    uint64_t ElemSize = Ctx.toCharUnitsFromBits(ElemBitSize).getQuantity();
    Size = llvm::APSInt(llvm::APInt(Ctx.getTargetInfo().getPointerWidth(0), ElemSize), false);
    return true;
  }

  // Convert I to a signed integer with Ctx.PointerWidth.
  static llvm::APSInt ConvertToSignedPointerWidth(ASTContext &Ctx, llvm::APSInt I, bool &Overflow) {
    uint64_t PointerWidth = Ctx.getTargetInfo().getPointerWidth(0);
    Overflow = false;
    if (I.getBitWidth() > PointerWidth) {
      Overflow = true;
      goto exit;
    }
    if (I.getBitWidth() < PointerWidth)
      I = I.extend(PointerWidth);
    if (I.isUnsigned()) {
      if (I > llvm::APSInt(I.getSignedMaxValue(PointerWidth))) {
        Overflow = true;
        goto exit;
      }
      I = llvm::APSInt(I, false);
    }
    exit:
      return I;
  }
};
}

namespace {
  class ExprCreatorUtil {
    public:
      // If Op is not a compound operator, CreateBinaryOperator returns a
      // binary operator LHS Op RHS.  If Op is a compound operator @=,
      // CreateBinaryOperator returns a binary operator LHS @ RHS.
      // LHS and RHS are cast to rvalues if necessary.
      static BinaryOperator *CreateBinaryOperator(Sema &SemaRef,
                                                  Expr *LHS, Expr *RHS,
                                                  BinaryOperatorKind Op) {
        assert(LHS && "expected LHS to exist");
        assert(RHS && "expected RHS to exist");
        LHS = EnsureRValue(SemaRef, LHS);
        RHS = EnsureRValue(SemaRef, RHS);
        if (BinaryOperator::isCompoundAssignmentOp(Op))
          Op = BinaryOperator::getOpForCompoundAssignment(Op);
        return new (SemaRef.Context) BinaryOperator(LHS, RHS, Op,
                                                    LHS->getType(),
                                                    LHS->getValueKind(),
                                                    LHS->getObjectKind(),
                                                    SourceLocation(),
                                                    FPOptions());
      }

      // Create an unsigned integer literal.
      static IntegerLiteral *CreateUnsignedInt(Sema &SemaRef, unsigned Value) {
        QualType T = SemaRef.Context.UnsignedIntTy;
        llvm::APInt Val(SemaRef.Context.getIntWidth(T), Value);
        return IntegerLiteral::Create(SemaRef.Context, Val,
                                      T, SourceLocation());
      }

      // Create an implicit cast expression.
      static ImplicitCastExpr *CreateImplicitCast(Sema &SemaRef, Expr *E,
                                                  CastKind CK, QualType T) {
        return ImplicitCastExpr::Create(SemaRef.Context, T,
                                        CK, E, nullptr,
                                        ExprValueKind::VK_RValue);
      }

      // If e is an rvalue, EnsureRValue returns e.  Otherwise, EnsureRValue
      // returns a cast of e to an rvalue, based on the type of e.
      static Expr *EnsureRValue(Sema &SemaRef, Expr *E) {
        if (E->isRValue())
          return E;

        CastKind Kind;
        QualType TargetTy;
        if (E->getType()->isArrayType()) {
          Kind = CK_ArrayToPointerDecay;
          TargetTy = SemaRef.getASTContext().getArrayDecayedType(E->getType());
        } else {
          Kind = CK_LValueToRValue;
          TargetTy = E->getType();
        }
        return CreateImplicitCast(SemaRef, E, Kind, TargetTy);
      }
  };
}

namespace {
  class AbstractBoundsExpr : public TreeTransform<AbstractBoundsExpr> {
    typedef TreeTransform<AbstractBoundsExpr> BaseTransform;
    typedef ArrayRef<DeclaratorChunk::ParamInfo> ParamsInfo;

  private:
    const ParamsInfo Params;

    // TODO: change this constant when we want to error on global variables
    // in parameter bounds declarations.
    const bool errorOnGlobals = false;

  public:
    AbstractBoundsExpr(Sema &SemaRef, ParamsInfo Params) :
      BaseTransform(SemaRef), Params(Params) {}

    Decl *TransformDecl(SourceLocation Loc, Decl *D) {
      return D->getCanonicalDecl();
    }

    ExprResult TransformDeclRefExpr(DeclRefExpr *E) {
      ValueDecl *D = E->getDecl();
      if (VarDecl *V = dyn_cast<VarDecl>(D)) {
        if (V->isLocalVarDecl())
          // Parameter bounds may not be in terms of local variables
          SemaRef.Diag(E->getLocation(),
                       diag::err_out_of_scope_function_type_local);
        else if (V->isFileVarDecl() || V->isExternC()) {
          // Parameter bounds may not be in terms of "global" variables
          // TODO: This is guarded by a flag right now, as we don't yet
          // want to error everywhere.
          if (errorOnGlobals) {
            SemaRef.Diag(E->getLocation(),
                          diag::err_out_of_scope_function_type_global);
          }
        }
        else if (ParmVarDecl *PD = dyn_cast<ParmVarDecl>(D)) {
          // Parameter bounds may be in terms of other parameters,
          // in which case we'll convert to a position-based representation.
          for (auto &ParamInfo : Params)
            if (PD == ParamInfo.Param) {
              return SemaRef.CreatePositionalParameterExpr(
                PD->getFunctionScopeIndex(),
                PD->getType());
            }
          SemaRef.Diag(E->getLocation(),
                       diag::err_out_of_scope_function_type_parameter);
        }
      }

      ValueDecl *ND =
        dyn_cast_or_null<ValueDecl>(BaseTransform::TransformDecl(
          SourceLocation(), D));
      if (D == ND || ND == nullptr)
        return E;
      else {
        clang::NestedNameSpecifierLoc QualifierLoc  = E->getQualifierLoc();
        clang::DeclarationNameInfo NameInfo = E->getNameInfo();
        return getDerived().RebuildDeclRefExpr(QualifierLoc, ND, NameInfo,
                                                nullptr);
      }
    }
  };
}

bool Sema::AbstractForFunctionType(
  BoundsAnnotations &Annots,
  ArrayRef<DeclaratorChunk::ParamInfo> Params) {  

  BoundsExpr *Expr = Annots.getBoundsExpr();
  // If there is no bounds expression, the itype does not change
  // as  aresult of abstraction.  Just return the original annotation.
  if (!Expr)
    return false;

  BoundsExpr *Result = nullptr;
  ExprResult AbstractedBounds =
    AbstractBoundsExpr(*this, Params).TransformExpr(Expr);
  if (AbstractedBounds.isInvalid()) {
    llvm_unreachable("unexpected failure to abstract bounds");
    Result = nullptr;
  } else {
    Result = dyn_cast<BoundsExpr>(AbstractedBounds.get());
    assert(Result && "unexpected dyn_cast failure");
  }

  if (Result == Expr)
    return false;

  Annots.setBoundsExpr(Result);
  return true;
}

namespace {
  class ConcretizeBoundsExpr : public TreeTransform<ConcretizeBoundsExpr> {
    typedef TreeTransform<ConcretizeBoundsExpr> BaseTransform;

  private:
    ArrayRef<ParmVarDecl *> Parameters;

  public:
    ConcretizeBoundsExpr(Sema &SemaRef, ArrayRef<ParmVarDecl *> Params) :
      BaseTransform(SemaRef),
      Parameters(Params) { }

    ExprResult TransformPositionalParameterExpr(PositionalParameterExpr *E) {
      unsigned index = E->getIndex();
      if (index < Parameters.size()) {
        ParmVarDecl *PD = Parameters[index];
        return SemaRef.BuildDeclRefExpr(PD, E->getType(),
          clang::ExprValueKind::VK_LValue, SourceLocation());
      } else {
        llvm_unreachable("out of range index for positional parameter");
        return ExprError();
      }
    }
  };
}

BoundsExpr *Sema::ConcretizeFromFunctionType(BoundsExpr *Expr,
                                             ArrayRef<ParmVarDecl *> Params) {
  if (!Expr)
    return Expr;

  BoundsExpr *Result;
  ExprSubstitutionScope Scope(*this); // suppress diagnostics

  ExprResult ConcreteBounds = ConcretizeBoundsExpr(*this, Params).TransformExpr(Expr);
  if (ConcreteBounds.isInvalid()) {
    llvm_unreachable("unexpected failure in making bounds concrete");
    return nullptr;
  }
  else {
    Result = dyn_cast<BoundsExpr>(ConcreteBounds.get());
    assert(Result && "unexpected dyn_cast failure");
    return Result;
  }
}

namespace {
  class CheckForModifyingArgs : public RecursiveASTVisitor<CheckForModifyingArgs> {
  private:
    Sema &SemaRef;
    const ArrayRef<Expr *> Arguments;
    llvm::SmallBitVector VisitedArgs;
    Sema::NonModifyingContext ErrorKind;
    bool ModifyingArg;
  public:
    CheckForModifyingArgs(Sema &SemaRef, ArrayRef<Expr *> Args,
                          Sema::NonModifyingContext ErrorKind) :
      SemaRef(SemaRef),
      Arguments(Args),
      VisitedArgs(Args.size()),
      ErrorKind(ErrorKind),
      ModifyingArg(false) {}

    bool FoundModifyingArg() {
      return ModifyingArg;
    }

    bool VisitPositionalParameterExpr(PositionalParameterExpr *E) {
      unsigned index = E->getIndex();
      if (index < Arguments.size() && !VisitedArgs[index]) {
        VisitedArgs.set(index);
        if (!SemaRef.CheckIsNonModifying(Arguments[index], ErrorKind,
                                         Sema::NonModifyingMessage::NMM_Error)) {
          ModifyingArg = true;
        }
      }
      return true;
    }
  };
}

namespace {
  class ConcretizeBoundsExprWithArgs : public TreeTransform<ConcretizeBoundsExprWithArgs> {
    typedef TreeTransform<ConcretizeBoundsExprWithArgs> BaseTransform;

  private:
    ArrayRef<Expr *> Args;

  public:
    ConcretizeBoundsExprWithArgs(Sema &SemaRef, ArrayRef<Expr *> Args) :
      BaseTransform(SemaRef),
      Args(Args) { }

    ExprResult TransformPositionalParameterExpr(PositionalParameterExpr *E) {
      unsigned index = E->getIndex();
      if (index < Args.size()) {
        return SemaRef.MakeAssignmentImplicitCastExplicit(Args[index]);
      } else {
        llvm_unreachable("out of range index for positional parameter");
        return ExprError();
      }
    }
  };
}

BoundsExpr *Sema::ConcretizeFromFunctionTypeWithArgs(
  BoundsExpr *Bounds, ArrayRef<Expr *> Args,
  NonModifyingContext ErrorKind) {
  if (!Bounds || Bounds->isInvalid())
    return Bounds;

  auto CheckArgs = CheckForModifyingArgs(*this, Args, ErrorKind);
  CheckArgs.TraverseStmt(Bounds);
  if (CheckArgs.FoundModifyingArg())
    return nullptr;

  ExprSubstitutionScope Scope(*this); // suppress diagnostics
  auto Concretizer = ConcretizeBoundsExprWithArgs(*this, Args);
  ExprResult ConcreteBounds = Concretizer.TransformExpr(Bounds);
  if (ConcreteBounds.isInvalid()) {
#ifndef NDEBUG
    llvm::outs() << "Failed concretizing\n";
    llvm::outs() << "Bounds:\n";
    Bounds->dump(llvm::outs());
    int count = Args.size();
    for (int i = 0; i < count; i++) {
      llvm::outs() << "Dumping arg " << i << "\n";
      Args[i]->dump(llvm::outs());
    }
    llvm::outs().flush();
#endif
    llvm_unreachable("unexpected failure in making function bounds concrete with arguments");
    return nullptr;
  }
  else {
    BoundsExpr *Result = dyn_cast<BoundsExpr>(ConcreteBounds.get());
    assert(Result && "unexpected dyn_cast failure");
    return Result;
  }
}

namespace {
  class ConcretizeMemberBounds : public TreeTransform<ConcretizeMemberBounds> {
    typedef TreeTransform<ConcretizeMemberBounds> BaseTransform;

  private:
    Expr *Base;
    bool IsArrow;

  public:
    ConcretizeMemberBounds(Sema &SemaRef, Expr *MemberBaseExpr, bool IsArrow) :
      BaseTransform(SemaRef), Base(MemberBaseExpr), IsArrow(IsArrow) { }

    // TODO: handle the situation where the base expression is an rvalue.
    // By C semantics, the result is an rvalue.  We are setting fields used in
    // bounds expressions to be lvalues, so we end up with a problems when
    // we expand the occurrences of the fields to be expressions that are
    //  rvalues.
    //
    // There are two problematic cases:
    // - We assume field expressions are lvalues, so we will have lvalue-to-rvalue
    //   conversions applied to rvalues.  We need to remove these conversions.
    // - The address of a field is taken.  It is illegal to take the address of
    //   an rvalue.
    //
    // rVvalue structs can arise from function returns of struct values.
    ExprResult TransformDeclRefExpr(DeclRefExpr *E) {
      if (FieldDecl *FD = dyn_cast<FieldDecl>(E->getDecl())) {
        if (Base->isRValue() && !IsArrow)
          // For now, return an error if we see an rvalue base.
          return ExprError();
        ASTContext &Context = SemaRef.getASTContext();
        ExprValueKind ResultKind;
        if (IsArrow)
          ResultKind = VK_LValue;
        else
          ResultKind = Base->isLValue() ? VK_LValue : VK_RValue;
        return
          MemberExpr::CreateImplicit(Context, Base, IsArrow, FD,
                                     E->getType(), ResultKind, OK_Ordinary);
      }
      return E;
    }
  };
}

BoundsExpr *Sema::MakeMemberBoundsConcrete(
  Expr *Base,
  bool IsArrow,
  BoundsExpr *Bounds) {
  ExprSubstitutionScope Scope(*this); // suppress diagnostics
  ExprResult ConcreteBounds =
    ConcretizeMemberBounds(*this, Base, IsArrow).TransformExpr(Bounds);
  if (ConcreteBounds.isInvalid())
    return nullptr;
  else {
    BoundsExpr *Result = dyn_cast<BoundsExpr>(ConcreteBounds.get());
    return Result;
  }
}

#if 0
namespace {
  // Convert occurrences of _Return_value in a return bounds expression
  // to _Current_expr_value.  Don't recurse into any return bounds
  // expressions nested in function types.  Occurrences of _Return_value
  // in those shouldn't be changed to _Current_value.
  class ReplaceReturnValue : public TreeTransform<ReplaceReturnValue> {
    typedef TreeTransform<ReplaceReturnValue> BaseTransform;

  public:
    ReplaceReturnValue(Sema &SemaRef) :BaseTransform(SemaRef) { }

    // Avoid transforming nested return bounds expressions.
    bool TransformReturnBoundsAnnotations(BoundsAnnotations &Annot,
                                          bool &Changed) {
      return false;
    }

    ExprResult TransformBoundsValueExpr(BoundsValueExpr *E) {
      BoundsValueExpr::Kind K = E->getKind();
      bool KindChanged = false;
      if (K == BoundsValueExpr::Kind::Return) {
        K = BoundsValueExpr::Kind::Current;
        KindChanged = true;
      }
      QualType QT = getDerived().TransformType(E->getType());
      if (!getDerived().AlwaysRebuild() && QT == E->getType() &&
          !KindChanged)
        return E;

      return getDerived().
        RebuildBoundsValueExpr(E->getLocation(), QT,K);
    }
   };
}
#endif

// Convert all temporary bindings in an expression to uses of the values	
// produced by a binding.   This should be done for bounds expressions that	
// are used in runtime checks.  That way we don't try to recompute a	
// temporary multiple times in an expression.	
namespace {	
  class PruneTemporaryHelper : public TreeTransform<PruneTemporaryHelper> {	
    typedef TreeTransform<PruneTemporaryHelper> BaseTransform;	


  public:	
    PruneTemporaryHelper(Sema &SemaRef) :	
      BaseTransform(SemaRef) { }	

    ExprResult TransformCHKCBindTemporaryExpr(CHKCBindTemporaryExpr *E) {	
      return new (SemaRef.Context) BoundsValueExpr(SourceLocation(), E);	
    }	
  };	

  Expr *PruneTemporaryBindings(Sema &SemaRef, Expr *E, CheckedScopeSpecifier CSS) {	
    // Account for checked scope information when transforming the expression.
    Sema::CheckedScopeRAII CheckedScope(SemaRef, CSS);

    Sema::ExprSubstitutionScope Scope(SemaRef); // suppress diagnostics	
    ExprResult R = PruneTemporaryHelper(SemaRef).TransformExpr(E);	
    if (R.isInvalid())
      return SemaRef.Context.getPrebuiltBoundsUnknown();
    else
      return R.get();
  }	
}

namespace {
  class VariableCountHelper : public RecursiveASTVisitor<VariableCountHelper> {
    private:
      Sema &SemaRef;
      DeclRefExpr *V;
      int Count;

    public:
      VariableCountHelper(Sema &SemaRef, DeclRefExpr *V) :
        SemaRef(SemaRef),
        V(V),
        Count(0) {}

      int GetCount() { return Count; }

      bool VisitDeclRefExpr(DeclRefExpr *E) {
        Lexicographic Lex(SemaRef.Context, nullptr);
        if (Lex.CompareExpr(E, V) == Lexicographic::Result::Equal)
          ++Count;
        return true;
      }
  };

  // VariableOccurrenceCount returns the number of occurrences of V in E.
  int VariableOccurrenceCount(Sema &SemaRef, DeclRefExpr *V, Expr *E) {
    VariableCountHelper Counter(SemaRef, V);
    Counter.TraverseStmt(E);
    return Counter.GetCount();
  }
}

namespace {
  class ReplaceVariableHelper : public TreeTransform<ReplaceVariableHelper> {
    typedef TreeTransform<ReplaceVariableHelper> BaseTransform;
    private:
      // The variable whose uses should be replaced in an expression.
      DeclRefExpr *Variable;

      // The original value (if any) to replace uses of the variable with.
      // If no original value is provided, an expression using the variable
      // will be transformed into an invalid result.
      Expr *OriginalValue;

    public:
      ReplaceVariableHelper(Sema &SemaRef, DeclRefExpr *V, Expr *OV) :
        BaseTransform(SemaRef),
        Variable(V),
        OriginalValue(OV) { }

      ExprResult TransformDeclRefExpr(DeclRefExpr *E) {
        Lexicographic Lex(SemaRef.Context, nullptr);
        if (Lex.CompareExpr(Variable, E) == Lexicographic::Result::Equal) {
          if (OriginalValue)
            return OriginalValue;
          else
            return ExprError();
        } else
          return E;
      }

      // Overriding TransformImplicitCastExpr is necessary since TreeTransform
      // does not preserve implicit casts.
      ExprResult TransformImplicitCastExpr(ImplicitCastExpr *E) {
        // Replace V with OV (if applicable) in the subexpression of E.
        ExprResult ChildResult = TransformExpr(E->getSubExpr());
        if (ChildResult.isInvalid())
          return ChildResult;

        Expr *Child = ChildResult.get();
        CastKind CK = E->getCastKind();

        if (CK == CastKind::CK_LValueToRValue ||
            CK == CastKind::CK_ArrayToPointerDecay)
          // Only cast children of lvalue to rvalue casts to an rvalue if
          // necessary.  The transformed child expression may no longer be
          // an lvalue, depending on the original value.  For example, if x
          // is transformed to the original value x + 1, it does not need to
          // be cast to an rvalue.
          return ExprCreatorUtil::EnsureRValue(SemaRef, Child);
        else
          return ExprCreatorUtil::CreateImplicitCast(SemaRef, Child,
                                                     CK, E->getType());
      }
  };

  // If an original value OV is provided, ReplaceVariableReferences returns
  // an expression that replaces all uses of the variable V in E with OV.
  // If no original value is provided and E uses V, ReplaceVariableReferences
  // returns nullptr.
  Expr *ReplaceVariableReferences(Sema &SemaRef, Expr *E, DeclRefExpr *V,
                                  Expr *OV, CheckedScopeSpecifier CSS) {
    // Don't transform e if it does not use the value of v.
    if (!VariableOccurrenceCount(SemaRef, V, E))
      return E;

    // Account for checked scope information when transforming the expression.
    Sema::CheckedScopeRAII CheckedScope(SemaRef, CSS);

    Sema::ExprSubstitutionScope Scope(SemaRef); // suppress diagnostics
    ExprResult R = ReplaceVariableHelper(SemaRef, V, OV).TransformExpr(E);
    if (R.isInvalid())
      return nullptr;
    else
      return R.get();
  }
}

namespace {
  // BoundsContextTy denotes a map of a variable declaration to the bounds
  // that are currently known to be valid for the variable.
  using BoundsContextTy = llvm::DenseMap<const VarDecl *, BoundsExpr *>;

  // EqualExprTy denotes a set of expressions that produce the same value
  // as an expression e.
  using EqualExprTy = SmallVector<Expr *, 4>;

  // ExprEqualMapTy denotes a map of an expression e to the set of
  // expressions that produce the same value as e.
  using ExprEqualMapTy = llvm::DenseMap<Expr *, EqualExprTy>;

  // CheckingState stores the outputs of bounds checking methods.
  // These members represent the state during bounds checking
  // and are updated while checking individual expressions.
  class CheckingState {
    public:
      // ObservedBounds maps variables to their current known bounds as
      // inferred by bounds checking.  These bounds are updated after
      // assignments to variables.
      //
      // The bounds in the ObservedBounds context should always be normalized
      // to range bounds if possible.  This allows updates to variables that
      // are implicitly used in bounds declarations to update the observed
      // bounds.  For example, an assignment to the variable p where p has
      // declared bounds count(i) should update the bounds of p, which
      // normalize to bounds(p, p + i).
      BoundsContextTy ObservedBounds;

      // UEQ stores sets of expressions that are equivalent to each other
      // after checking an expression e.
      EquivExprSets UEQ;

      // G is a set of expressions that produce the same value as an
      // expression e once checking of e is complete.
      EqualExprTy G;
  };
}

namespace {
  class DeclaredBoundsHelper : public RecursiveASTVisitor<DeclaredBoundsHelper> {
    private:
      Sema &SemaRef;
      BoundsContextTy &BoundsContextRef;

    public:
      DeclaredBoundsHelper(Sema &SemaRef, BoundsContextTy &Context) :
        SemaRef(SemaRef),
        BoundsContextRef(Context) {}

      // If a variable declaration has declared bounds, modify BoundsContextRef
      // to map the variable declaration to the normalized declared bounds.
      // 
      // Returns true if visiting the variable declaration did not terminate
      // early.  Visiting variable declarations in DeclaredBoundsHelper should
      // never terminate early.
      bool VisitVarDecl(const VarDecl *D) {
        if (!D)
          return true;
        if (D->isInvalidDecl())
          return true;
        // The bounds expressions in the bounds context should be normalized
        // to range bounds.
        if (const BoundsExpr *Bounds = D->getBoundsExpr())
          BoundsContextRef[D] = SemaRef.ExpandBoundsToRange(D, Bounds);
        return true;
      }
  };

  // GetDeclaredBounds modifies the bounds context to map any variables
  // declared in S to their declared bounds (if any).
  void GetDeclaredBounds(Sema &SemaRef, BoundsContextTy &Context, Stmt *S) {
    DeclaredBoundsHelper Declared(SemaRef, Context);
    Declared.TraverseStmt(S);
  }
}

namespace {
  class CheckBoundsDeclarations {
  private:
    Sema &S;
    bool DumpBounds;
    bool DumpState;
    uint64_t PointerWidth;
    Stmt *Body;
    CFG *Cfg;
    BoundsExpr *ReturnBounds; // return bounds expression for enclosing
                              // function, if any.
    ASTContext &Context;
    std::pair<ComparisonSet, ComparisonSet> &Facts;

    // Having a BoundsAnalysis object here allows us to easily invoke methods
    // for bounds-widening and get back the bounds-widening info needed for
    // bounds inference/checking.
    BoundsAnalysis BoundsAnalyzer;

    // When this flag is set to true, include the null terminator in the
    // bounds of a null-terminated array.  This is used when calculating
    // physical sizes during casts to pointers to null-terminated arrays.
    bool IncludeNullTerminator;

    void DumpAssignmentBounds(raw_ostream &OS, BinaryOperator *E,
                              BoundsExpr *LValueTargetBounds,
                              BoundsExpr *RHSBounds) {
      OS << "\n";
      E->dump(OS);
      if (LValueTargetBounds) {
        OS << "Target Bounds:\n";
        LValueTargetBounds->dump(OS);
      }
      if (RHSBounds) {
        OS << "RHS Bounds:\n ";
        RHSBounds->dump(OS);
      }
    }

    void DumpBoundsCastBounds(raw_ostream &OS, CastExpr *E,
                              BoundsExpr *Declared, BoundsExpr *NormalizedDeclared,
                              BoundsExpr *SubExprBounds) {
      OS << "\n";
      E->dump(OS);
      if (Declared) {
        OS << "Declared Bounds:\n";
        Declared->dump(OS);
      }
      if (NormalizedDeclared) {
        OS << "Normalized Declared Bounds:\n ";
        NormalizedDeclared->dump(OS);
      }
      if (SubExprBounds) {
        OS << "Inferred Subexpression Bounds:\n ";
        SubExprBounds->dump(OS);
      }
    }

    void DumpInitializerBounds(raw_ostream &OS, VarDecl *D,
                               BoundsExpr *Target, BoundsExpr *B) {
      OS << "\n";
      D->dump(OS);
      OS << "Declared Bounds:\n";
      Target->dump(OS);
      OS << "Initializer Bounds:\n ";
      B->dump(OS);
    }

    void DumpExpression(raw_ostream &OS, Expr *E) {
      OS << "\n";
      E->dump(OS);
    }

    void DumpCallArgumentBounds(raw_ostream &OS, BoundsExpr *Param,
                                Expr *Arg,
                                BoundsExpr *ParamBounds,
                                BoundsExpr *ArgBounds) {
      OS << "\n";
      if (Param) {
        OS << "Original parameter bounds\n";
        Param->dump(OS);
      }
      if (Arg) {
        OS << "Argument:\n";
        Arg->dump(OS);
      }
      if (ParamBounds) {
        OS << "Parameter Bounds:\n";
        ParamBounds->dump(OS);
      }
      if (ArgBounds) {
        OS << "Argument Bounds:\n ";
        ArgBounds->dump(OS);
      }
    }

    void DumpCheckingState(raw_ostream &OS, Stmt *S, CheckingState &State) {
      OS << "\nStatement S:\n";
      S->dump(OS);

      OS << "Observed bounds context after checking S:\n";
      DumpBoundsContext(OS, State.ObservedBounds);

      OS << "Sets of equivalent expressions after checking S:\n";
      if (State.UEQ.size() == 0)
        OS << "{ }\n";
      else {
        OS << "{\n";
        for (auto OuterList = State.UEQ.begin(); OuterList != State.UEQ.end(); ++OuterList) {
          auto ExprList = *OuterList;
          DumpEqualExpr(OS, ExprList);
        }
        OS << "}\n";
      }

      OS << "Expressions that produce the same value as S:\n";
      DumpEqualExpr(OS, State.G);
    }

    void DumpBoundsContext(raw_ostream &OS, BoundsContextTy &Context) {
      if (Context.empty())
        OS << "{ }\n";
      else {
        // The keys in an llvm::DenseMap are unordered.  Create a set of
        // variable declarations in the context ordered first by name,
        // then by location in order to guarantee a deterministic output
        // so that printing the bounds context can be tested.
        std::vector<const VarDecl *> OrderedDecls;
        for (auto Pair : Context)
          OrderedDecls.push_back(Pair.first);
        llvm::sort(OrderedDecls.begin(), OrderedDecls.end(),
             [] (const VarDecl *A, const VarDecl *B) {
               if (A->getNameAsString() == B->getNameAsString())
                 return A->getLocation() < B->getLocation();
               else
                 return A->getNameAsString() < B->getNameAsString();
             });

        OS << "{\n";
        for (auto I = OrderedDecls.begin(); I != OrderedDecls.end(); ++I) {
          const VarDecl *Variable = *I;
          auto It = Context.find(Variable);
          if (It == Context.end())
            continue;
          OS << "Variable:\n";
          Variable->dump(OS);
          OS << "Bounds:\n";
          It->second->dump(OS);
        }
        OS << "}\n";
      }
    }

    void DumpEqualExpr(raw_ostream &OS, EqualExprTy G) {
      if (G.size() == 0)
        OS << "{ }\n";
      else {
        OS << "{\n";
        for (auto I = G.begin(); I != G.end(); ++I) {
          Expr *E = *I;
          E->dump(OS);
        }
        OS << "}\n";
      }
    }

    // Add bounds check to an lvalue expression, if it is an Array_ptr
    // dereference.  The caller has determined that the lvalue is being
    // used in a way that requies a bounds check if the lvalue is an
    // _Array_ptr or _Nt_array_ptr dereference.  The lvalue uses are to read
    // or write memory or as the base expression of a member reference.
    //
    // If the Array_ptr has unknown bounds, this is a compile-time error.
    // Generate an error message and set the bounds to an invalid bounds
    // expression.
    enum class OperationKind {
      Read,   // just reads memory
      Assign, // simple assignment to memory
      Other   // reads and writes memory, struct base check
    };

    bool AddBoundsCheck(Expr *E, OperationKind OpKind, CheckedScopeSpecifier CSS,
                        BoundsExpr *LValueBounds) {
      assert(E->isLValue());
      bool NeedsBoundsCheck = false;
      QualType PtrType;
      if (Expr *Deref = S.GetArrayPtrDereference(E, PtrType)) {
        NeedsBoundsCheck = true;
        LValueBounds = S.CheckNonModifyingBounds(LValueBounds, E);
        BoundsCheckKind Kind = BCK_Normal;
        // Null-terminated array pointers have special semantics for
        // bounds checks.
        if (PtrType->isCheckedPointerNtArrayType()) {
          if (OpKind == OperationKind::Read)
            Kind = BCK_NullTermRead;
          else if (OpKind == OperationKind::Assign)
            Kind = BCK_NullTermWriteAssign;
          // Otherwise, use the default range check for bounds.
        }
        if (LValueBounds->isUnknown()) {
          S.Diag(E->getBeginLoc(), diag::err_expected_bounds) << E->getSourceRange();
          LValueBounds = S.CreateInvalidBoundsExpr();
        } else {
          CheckBoundsAtMemoryAccess(Deref, LValueBounds, Kind, CSS);
        }
        if (UnaryOperator *UO = dyn_cast<UnaryOperator>(Deref)) {
          assert(!UO->hasBoundsExpr());
          UO->setBoundsExpr(LValueBounds);
          UO->setBoundsCheckKind(Kind);

        } else if (ArraySubscriptExpr *AS = dyn_cast<ArraySubscriptExpr>(Deref)) {
          assert(!AS->hasBoundsExpr());
          AS->setBoundsExpr(LValueBounds);
          AS->setBoundsCheckKind(Kind);
        } else
          llvm_unreachable("unexpected expression kind");
      }
      return NeedsBoundsCheck;
    }

    // Add bounds check to the base expression of a member reference, if the
    // base expression is an Array_ptr dereference.  Such base expressions
    // always need bounds checks, even though their lvalues are only used for an
    // address computation.
    bool AddMemberBaseBoundsCheck(MemberExpr *E, CheckedScopeSpecifier CSS,
                                  BoundsExpr *BaseLValueBounds,
                                  BoundsExpr *BaseBounds) {
      Expr *Base = E->getBase();
      // E.F
      if (!E->isArrow()) {
        // The base expression only needs a bounds check if it is an lvalue.
        if (Base->isLValue())
          return AddBoundsCheck(Base, OperationKind::Other, CSS,
                                BaseLValueBounds);
        return false;
      }

      // E->F.  This is equivalent to (*E).F.
      if (Base->getType()->isCheckedPointerArrayType()) {
        BoundsExpr *Bounds = S.CheckNonModifyingBounds(BaseBounds, Base);
        if (Bounds->isUnknown()) {
          S.Diag(Base->getBeginLoc(), diag::err_expected_bounds) << Base->getSourceRange();
          Bounds = S.CreateInvalidBoundsExpr();
        } else {
          CheckBoundsAtMemoryAccess(E, Bounds, BCK_Normal, CSS);
        }
        E->setBoundsExpr(Bounds);
        return true;
      }

      return false;
    }


    // The result of trying to prove a statement about bounds declarations.
    // The proof system is incomplete, so there are will be statements that
    // cannot be proved true or false.  That's why "maybe" is a result.
    enum class ProofResult {
      True,  // Definitely provable.
      False, // Definitely false (an error)
      Maybe  // We're not sure yet.
    };

    // The kind of statement that we are trying to prove true or false.
    //
    // This enum is used in generating diagnostic messages. If you change the order,
    // update the messages in DiagnosticSemaKinds.td used in
    // ExplainProofFailure
    enum class ProofStmtKind : unsigned {
      BoundsDeclaration,
      StaticBoundsCast,
      MemoryAccess,
      MemberArrowBase
    };

    // ProofFailure: codes that explain why a statement is false.  This is a
    // bitmask because there may be multiple reasons why a statement false.
    enum class ProofFailure : unsigned {
      None = 0x0,
      LowerBound = 0x1,     // The destination lower bound is below the source lower bound.
      UpperBound = 0x2,     // The destination upper bound is above the source upper bound.
      SrcEmpty = 0x4,       // The source bounds are empty (LB == UB)
      SrcInvalid = 0x8,     // The source bounds are invalid (LB > UB).
      DstEmpty = 0x10,      // The destination bounds are empty (LB == UB).
      DstInvalid = 0x20,    // The destination bounds are invalid (LB > UB).
      Width = 0x40,         // The source bounds are narrower than the destination bounds.
      PartialOverlap = 0x80 // There was only partial overlap of the destination bounds with
                            // the source bounds.
    };

    enum class DiagnosticNameForTarget {
      Destination = 0x0,
      Target = 0x1
    };

    // Combine proof failure codes.
    static constexpr ProofFailure CombineFailures(ProofFailure A,
                                                  ProofFailure B) {
      return static_cast<ProofFailure>(static_cast<unsigned>(A) |
                                       static_cast<unsigned>(B));
    }

    // Check that all the conditions in "Test" are in the failure code.
    static constexpr bool TestFailure(ProofFailure A, ProofFailure Test) {
      return ((static_cast<unsigned>(A) &  static_cast<unsigned>(Test)) ==
              static_cast<unsigned>(Test));
    }

    static void DumpFailure(raw_ostream &OS, ProofFailure A) {
      OS << "[ ";
      if (TestFailure(A, ProofFailure::LowerBound)) OS << "LowerBound ";
      if (TestFailure(A, ProofFailure::UpperBound)) OS << "UpperBound ";
      if (TestFailure(A, ProofFailure::SrcEmpty)) OS << "SrcEmpty ";
      if (TestFailure(A, ProofFailure::SrcInvalid)) OS << "SrcInvalid ";
      if (TestFailure(A, ProofFailure::DstEmpty)) OS << "DstEmpty ";
      if (TestFailure(A, ProofFailure::DstInvalid)) OS << "DstInvalid ";
      if (TestFailure(A, ProofFailure::Width)) OS << "Width ";
      if (TestFailure(A, ProofFailure::PartialOverlap)) OS << "PartialOverlap ";
      OS << "]";
    }

    // Representation and operations on ranges.
    // A range has the form (e1 + e2, e1 + e3) where e1 is an expression.
    // A range can be either Constant- or Variable-sized.
    //
    // - If e2 and e3 are both constant integer expressions, the range is Constant-sized.
    //   For now, in this case, we represent e2 and e3 as signed (APSInt) integers.
    //   They must have the same bitsize.
    //   More specifically: (UpperOffsetVariable == nullptr && LowerOffsetVariable == nullptr)
    // - If one or both of e2 and e3 are non-constant expressions, the range is Variable-sized.
    //   More specifically: (UpperOffsetVariable != nullptr || LowerOffsetVariable != nullptr)
    class BaseRange {
    public:
      enum Kind {
        ConstantSized,
        VariableSized,
        Invalid
      };

    private:
      Sema &S;
      Expr *Base;
      llvm::APSInt LowerOffsetConstant;
      llvm::APSInt UpperOffsetConstant;
      Expr *LowerOffsetVariable;
      Expr *UpperOffsetVariable;

    public:
      BaseRange(Sema &S) : S(S), Base(nullptr), LowerOffsetConstant(1, true),
        UpperOffsetConstant(1, true), LowerOffsetVariable(nullptr), UpperOffsetVariable(nullptr) {
      }

      BaseRange(Sema &S, Expr *Base,
                         llvm::APSInt &LowerOffsetConstant,
                         llvm::APSInt &UpperOffsetConstant) :
        S(S), Base(Base), LowerOffsetConstant(LowerOffsetConstant), UpperOffsetConstant(UpperOffsetConstant),
        LowerOffsetVariable(nullptr), UpperOffsetVariable(nullptr) {
      }

      BaseRange(Sema &S, Expr *Base,
                         Expr *LowerOffsetVariable,
                         Expr *UpperOffsetVariable) :
        S(S), Base(Base), LowerOffsetConstant(1, true), UpperOffsetConstant(1, true),
        LowerOffsetVariable(LowerOffsetVariable), UpperOffsetVariable(UpperOffsetVariable) {
      }

      // Is R a subrange of this range?
      ProofResult InRange(BaseRange &R, ProofFailure &Cause, EquivExprSets *EquivExprs,
                          std::pair<ComparisonSet, ComparisonSet>& Facts) {

        // We will warn on declaration of Invalid ranges (upperBound < lowerBound).
        // The following cases are handled by the callers of this function:
        // - Error on memory access to Invalid and Empty ranges
        if (R.IsInvalid()) {
          Cause = CombineFailures(Cause, ProofFailure::DstInvalid);
          return ProofResult::Maybe;
        }

        if (EqualValue(S.Context, Base, R.Base, EquivExprs)) {
          ProofResult LowerBoundsResult = CompareLowerOffsets(R, Cause, EquivExprs, Facts);
          ProofResult UpperBoundsResult = CompareUpperOffsets(R, Cause, EquivExprs, Facts);

          if (LowerBoundsResult == ProofResult::True &&
              UpperBoundsResult == ProofResult::True)
            return ProofResult::True;
          if (LowerBoundsResult == ProofResult::False ||
              UpperBoundsResult == ProofResult::False)
            return ProofResult::False;
        }
        return ProofResult::Maybe;
      }

      // This function proves whether this.LowerOffset <= R.LowerOffset.
      // Depending on whether these lower offsets are ConstantSized or VariableSized, various cases should be checked:
      // - If `this` and `R` both have constant lower offsets (i.e., if the following condition holds:
      //   `IsLowerOffsetConstant() && R.IsLowerOffsetConstant()`), the function returns
      //   true only if `LowerOffsetConstant <= R.LowerOffsetConstant`. Otherwise, it should return false.
      // - If `this` and `R` both have variable lower offsets (i.e., if the following condition holds:
      //   `IsLowerOffsetVariable() && R.IsLowerOffsetVariable()`), the function returns true if
      //   `EqualValue()` determines that `LowerOffsetVariable` and `R.LowerOffsetVariable` are equal.
      // - If `this` has a constant lower offset (i.e., `IsLowerOffsetConstant()` is true),
      //   but `R` has a variable lower offset (i.e., `R.IsLowerOffsetVariable()` is true), the function
      //   returns true only if `R.LowerOffsetVariable` has unsgined integer type and `this.LowerOffsetConstant`
      //   has value 0 when it is extended to int64_t.
      // - If none of the above cases happen, it means that the function has not been able to prove
      //   whether this.LowerOffset is less than or equal to R.LowerOffset, or not. Therefore,
      //   it returns maybe as the result.
      ProofResult CompareLowerOffsets(BaseRange &R, ProofFailure &Cause, EquivExprSets *EquivExprs,
                                      std::pair<ComparisonSet, ComparisonSet>& Facts) {
        if (IsLowerOffsetConstant() && R.IsLowerOffsetConstant()) {
          if (LowerOffsetConstant <= R.LowerOffsetConstant)
            return ProofResult::True;
          Cause = CombineFailures(Cause, ProofFailure::LowerBound);
          return ProofResult::False;
        }
        if (IsLowerOffsetVariable() && R.IsLowerOffsetVariable())
          if (LessThanOrEqualExtended(S.Context, Base, R.Base, LowerOffsetVariable, R.LowerOffsetVariable, EquivExprs, Facts))
            return ProofResult::True;
        if (R.IsLowerOffsetVariable() && IsLowerOffsetConstant() &&
            R.LowerOffsetVariable->getType()->isUnsignedIntegerType() && LowerOffsetConstant.getExtValue() == 0)
          return ProofResult::True;

        return ProofResult::Maybe;
      }

      // This function proves whether R.UpperOffset <= this.UpperOffset.
      // Depending on whether these upper offsets are ConstantSized or VariableSized, various cases should be checked:
      // - If `this` and `R` both have constant upper offsets (i.e., if the following condition holds:
      //   `IsUpperOffsetConstant() && R.IsUpperOffsetConstant()`), the function returns
      //   true only if `R.UpperOffsetConstant <= UpperOffsetConstant`. Otherwise, it should return false.
      // - If `this` and `R` both have variable upper offsets (i.e., if the following condition holds:
      //   `IsUpperOffsetVariable() && R.IsUpperOffsetVariable()`), the function returns true if
      //   `EqualValue()` determines that `UpperOffsetVariable` and `R.UpperOffsetVariable` are equal.
      // - If `R` has a constant upper offset (i.e., `R.IsUpperOffsetConstant()` is true),
      //   but `this` has a variable upper offset (i.e., `IsUpperOffsetVariable()` is true), the function
      //   returns true only if `UpperOffsetVariable` has unsgined integer type and `R.UpperOffsetConstant`
      //   has value 0 when it is extended to int64_t.
      // - If none of the above cases happen, it means that the function has not been able to prove
      //   whether R.UpperOffset is less than or equal to this.UpperOffset, or not. Therefore,
      //   it returns maybe as the result.
      ProofResult CompareUpperOffsets(BaseRange &R, ProofFailure &Cause, EquivExprSets *EquivExprs,
                                      std::pair<ComparisonSet, ComparisonSet>& Facts) {
        if (IsUpperOffsetConstant() && R.IsUpperOffsetConstant()) {
          if (R.UpperOffsetConstant <= UpperOffsetConstant)
            return ProofResult::True;
          Cause = CombineFailures(Cause, ProofFailure::UpperBound);
          return ProofResult::False;
        }
        if (IsUpperOffsetVariable() && R.IsUpperOffsetVariable())
          if (LessThanOrEqualExtended(S.Context, R.Base, Base, R.UpperOffsetVariable, UpperOffsetVariable, EquivExprs, Facts))
            return ProofResult::True;
        if (IsUpperOffsetVariable() && R.IsUpperOffsetConstant() &&
            UpperOffsetVariable->getType()->isUnsignedIntegerType() && R.UpperOffsetConstant.getExtValue() == 0)
          return ProofResult::True;

        return ProofResult::Maybe;
      }

      bool IsConstantSizedRange() {
        return IsLowerOffsetConstant() && IsUpperOffsetConstant();
      }

      bool IsVariableSizedRange() {
        return IsLowerOffsetVariable() || IsUpperOffsetVariable();
      }

      bool IsLowerOffsetConstant() {
        return !LowerOffsetVariable;
      }

      bool IsLowerOffsetVariable() {
        return LowerOffsetVariable;
      }

      bool IsUpperOffsetConstant() {
        return !UpperOffsetVariable;
      }

      bool IsUpperOffsetVariable() {
        return UpperOffsetVariable;
      }

      // This function returns true if, when the range is ConstantSized,
      // `UpperOffsetConstant == LowerOffsetConstant`.
      // Currently, it returns false when the range is not ConstantSized.
      // However, this should be generalized in the future.
      bool IsEmpty() {
        if (IsConstantSizedRange())
          return UpperOffsetConstant == LowerOffsetConstant;
        // TODO: can we generalize IsEmpty to non-constant ranges?
        return false;
      }

      // This function returns true if, when the range is ConstantSized,
      // `UpperOffsetConstant < LowerOffsetConstant`.
      // Currently, it returns false when the range is not ConstantSized.
      // However, this should be generalized in the future.
      bool IsInvalid() {
        if (IsConstantSizedRange())
          return UpperOffsetConstant < LowerOffsetConstant;
        // TODO: can we generalize IsInvalid to non-constant ranges?
        return false;
      }

      // Does R partially overlap this range?
      ProofResult PartialOverlap(BaseRange &R) {
        if (Lexicographic(S.Context, nullptr).CompareExpr(Base, R.Base) ==
            Lexicographic::Result::Equal) {
          // TODO: can we generalize this function to non-constant ranges?
          if (IsConstantSizedRange() && R.IsConstantSizedRange()) {
            if (!IsEmpty() && !R.IsEmpty() && !IsInvalid() && !R.IsInvalid()) {
              // R.LowerOffset is within this range, but R.UpperOffset is above the range
              if (LowerOffsetConstant <= R.LowerOffsetConstant && R.LowerOffsetConstant < UpperOffsetConstant &&
                  UpperOffsetConstant < R.UpperOffsetConstant)
                return ProofResult::True;
              // Or R.UpperOffset is within this range, but R.LowerOffset is below the range.
              if (LowerOffsetConstant < R.UpperOffsetConstant && R.UpperOffsetConstant <= UpperOffsetConstant &&
                  R.LowerOffsetConstant < LowerOffsetConstant)
                return ProofResult::True;
            }
          }
          return ProofResult::False;
        }
        return ProofResult::Maybe;
      }

      bool AddToUpper(llvm::APSInt &Num) {
        bool Overflow;
        UpperOffsetConstant = UpperOffsetConstant.sadd_ov(Num, Overflow);
        return Overflow;
      }

      llvm::APSInt GetWidth() {
        return UpperOffsetConstant - LowerOffsetConstant;
      }

      void SetBase(Expr *B) {
        Base = B;
      }

      void SetLowerConstant(llvm::APSInt &Lower) {
        LowerOffsetConstant = Lower;
      }

      void SetUpperConstant(llvm::APSInt &Upper) {
        UpperOffsetConstant = Upper;
      }

      void SetLowerVariable(Expr *Lower) {
        LowerOffsetVariable = Lower;
      }

      void SetUpperVariable(Expr *Upper) {
        UpperOffsetVariable = Upper;
      }

      void Dump(raw_ostream &OS) {
        OS << "Range:\n";
        OS << "Base: ";
        if (Base)
          Base->dump(OS);
        else
          OS << "nullptr\n";
        if (IsLowerOffsetConstant()) {
          SmallString<12> Str;
          LowerOffsetConstant.toString(Str);
          OS << "Lower offset:" << Str << "\n";
        }
        if (IsUpperOffsetConstant()) {
          SmallString<12> Str;
          UpperOffsetConstant.toString(Str);
          OS << "Upper offset:" << Str << "\n";
        }
        if (IsLowerOffsetVariable()) {
          OS << "Lower offset:\n";
          LowerOffsetVariable->dump(OS);
        }
        if (IsUpperOffsetVariable()) {
          OS << "Upper offset:\n";
          UpperOffsetVariable->dump(OS);
        }
      }
    };



    // This function splits the expression `E` into an expression `Base`, and an offset.
    // The offset can be an integer constant or not. If it is an integer constant, the
    // extracted offset can be found in `OffsetConstant`, and `OffsetVariable` will be nullptr.
    // In this case, the return value is `BaseRange::Kind::ConstantSized`.
    // Otherwise, the extracted offset can be found in `OffsetVariable`, and `OffsetConstant`
    // will not be updated. In this case, the return value is `BaseRange::Kind::VariableSized`.
    //
    // Implementation details:
    // - If `E` is a BinaryOperator with an additive opcode, depending on whether the LHS or RHS
    //   is a pointer, Base and offset can get different values in different cases:
    //
    //    First, for extracting the `Base`,
    //     1a. if E.LHS is a pointer, Base = E.LHS.
    //     2a. if E.RHS is a pointer, Base = E.RHS.
    //     If (1a) and (2a) do not hold, Base = E and OffsetConstant = 0 and OffsetVariable = nullptr. Also,
    //     `BaseRange::Kind::ConstantSized` will be returned.
    //
    //    Next, for extracting the offset,
    //     1b. if E.LHS is a pointer and E.RHS is a constant integer,
    //         or, if E.RHS is a pointer and E.LHS is a constant integer, the function will set
    //        `OffsetConstant` to the constant integer and widen and/or normalize it if needed.
    //        Then, it returns `BaseRange::Kind::ConstantSized`. When manipulating the extracted
    //        constant integer, if an overflow occurres in any of the steps, `OffsetConstant = 0`
    //        and `OffsetVariable = nullptr`. Also, `BaseRange::Kind::ConstantSized` will be returned.
    //     If (1b) does not hold, we define the offset to be VariableSized. Therefore,
    //     `OffsetVariable = E.RHS` if E.LHS is a pointer, and `OffsetVariable = E.LHS` if E.RHS is
    //     a pointer. In this case, `BaseRange::Kind::VariableSized` will be returned.
    //
    // TODO: we use signed integers to represent the result of the OffsetConstant.
    // We can't represent unsigned offsets larger the the maximum signed
    // integer that will fit pointer width.
    BaseRange::Kind SplitIntoBaseAndOffset(Expr *E, Expr *&Base, llvm::APSInt &OffsetConstant,
                                Expr *&OffsetVariable) {
      if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(E->IgnoreParens())) {
        if (BO->isAdditiveOp()) {
          Expr *Other = nullptr;
          if (BO->getLHS()->getType()->isPointerType()) {
            Base = BO->getLHS();
            Other = BO->getRHS();
          } else if (BO->getRHS()->getType()->isPointerType()) {
            Base = BO->getRHS();
            Other = BO->getLHS();
          } else
            goto exit;
          assert(Other->getType()->isIntegerType());
          if (Other->isIntegerConstantExpr(OffsetConstant, S.Context)) {
            // Widen the integer to the number of bits in a pointer.
            bool Overflow;
            OffsetConstant = BoundsUtil::ConvertToSignedPointerWidth(S.Context, OffsetConstant, Overflow);
            if (Overflow)
              goto exit;
            // Normalize the operation by negating the offset if necessary.
            if (BO->getOpcode() == BO_Sub) {
              OffsetConstant = llvm::APSInt(PointerWidth, false).ssub_ov(OffsetConstant, Overflow);
              if (Overflow)
                goto exit;
            }
            llvm::APSInt ElemSize;
            if (!BoundsUtil::getReferentSizeInChars(S.Context, Base->getType(), ElemSize))
                goto exit;
            OffsetConstant = OffsetConstant.smul_ov(ElemSize, Overflow);
            if (Overflow)
              goto exit;
            OffsetVariable = nullptr;
            return BaseRange::Kind::ConstantSized;
          } else {
            OffsetVariable = Other;
            return BaseRange::Kind::VariableSized;
          }
        }
      }

    exit:
      // Return (E, 0).
      Base = E->IgnoreParens();
      OffsetConstant = llvm::APSInt(PointerWidth, false);
      OffsetVariable = nullptr;
      return BaseRange::Kind::ConstantSized;
    }

    // Given a `Base` and `Offset`, this function tries to convert it to a standard form `Base + (ConstantPart OP VariablePart)`.
    // The OP's signedness is stored in IsOpSigned. If the function fails to create the standard form, it returns false. Otherwise,
    // it returns true to indicate success, and stores each part of the standard form in a separate argument as follows:
    // `ConstantPart`: a signed integer
    // `IsOpSigned`: a boolean which is true if VariablePart is signed, and false otherwise
    // `VariablePart`: an integer expression that can be a either signed or unsigned integer
    //
    // Given array_ptr<T> p:
    // 1. For (char *)p + e1 or (unsigned char *)p + e1, ConstantPart = 1, VariablePart = e1.
    // 2. For p + e1, ConstantPart = sizeof(T), VariablePart = e1.
    //
    // Note that another way to interpret the functionality of this function is that it expands
    // pointer arithmetic to bytewise arithmetic.
    static bool CreateStandardForm(ASTContext &Ctx, Expr *Base, Expr *Offset, llvm::APSInt &ConstantPart, bool &IsOpSigned, Expr *&VariablePart) {
      bool Overflow;
      uint64_t PointerWidth = Ctx.getTargetInfo().getPointerWidth(0);
      if (!Base->getType()->isPointerType())
        return false;
      if (Base->getType()->getPointeeOrArrayElementType()->isCharType()) {
        if (BinaryOperator *BO = dyn_cast<BinaryOperator>(Offset)) {
          if (BO->getRHS()->isIntegerConstantExpr(ConstantPart, Ctx))
            VariablePart = BO->getLHS();
          else if (BO->getLHS()->isIntegerConstantExpr(ConstantPart, Ctx))
            VariablePart = BO->getRHS();
          else
            goto exit;
          IsOpSigned = VariablePart->getType()->isSignedIntegerType();
          ConstantPart = BoundsUtil::ConvertToSignedPointerWidth(Ctx, ConstantPart, Overflow);
          if (Overflow)
            goto exit;
        } else
          goto exit;
        return true;

      exit:
        VariablePart = Offset;
        ConstantPart = llvm::APSInt(llvm::APInt(PointerWidth, 1), false);
        IsOpSigned = VariablePart->getType()->isSignedIntegerType();
        return true;
      } else {
        VariablePart = Offset;
        IsOpSigned = VariablePart->getType()->isSignedIntegerType();
        if (!BoundsUtil::getReferentSizeInChars(Ctx, Base->getType(), ConstantPart))
          return false;
        ConstantPart = BoundsUtil::ConvertToSignedPointerWidth(Ctx, ConstantPart, Overflow);
        if (Overflow)
          return false;
        return true;
      }
    }

    // In this function, the goal is to compare two expressions: `Base1 + Offset1` and `Base2 + Offset2`.
    // The function returns true if they are equal, and false otherwise. Note that before checking equivalence
    // of expressions, the function expands pointer arithmetic to bytewise arithmetic.
    //
    // Steps in checking the equivalence:
    // 0. If `Offset1` or `Offset2` is null, return false.
    // 1. If `Base1` and `Base2` are not lexicographically equal, return false.
    // 2. Next, both bounds are converted into standard forms `Base + ConstantPart * VariablePart` as explained in `CreateStandardForm()`
    // 3. If any of the expressions cannot be converted successfully, return false.
    // 4. If VariableParts are not lexicographically equal, return false.
    // 5. If OP signs are not equivalent in both, return false.
    // 6. If ConstantParts are not equal, return false.
    // If the expressions pass all the above tests, then return true.
    //
    // Note that in all steps involving in checking the equality of the types or values of offsets, parentheses and
    // casts are ignored.
    static bool EqualExtended(ASTContext &Ctx, Expr *Base1, Expr *Base2, Expr *Offset1, Expr *Offset2, EquivExprSets *EquivExprs) {
      if (!Offset1 && !Offset2)
        return false;

      if (!EqualValue(Ctx, Base1, Base2, EquivExprs))
        return false;

      llvm::APSInt ConstantPart1, ConstantPart2;
      bool IsOpSigned1, IsOpSigned2;
      Expr *VariablePart1, *VariablePart2;

      bool CreatedStdForm1 = CreateStandardForm(Ctx, Base1, Offset1, ConstantPart1, IsOpSigned1, VariablePart1);
      bool CreatedStdForm2 = CreateStandardForm(Ctx, Base2, Offset2, ConstantPart2, IsOpSigned2, VariablePart2);
      
      if (!CreatedStdForm1 || !CreatedStdForm2)
        return false;
      if (!EqualValue(Ctx, VariablePart1, VariablePart2, EquivExprs))
        return false;
      if (IsOpSigned1 != IsOpSigned2)
        return false;
      if (ConstantPart1 != ConstantPart2)
        return false;

      return true;
    }

    // This function is an extension of EqualExtended. It looks into the provided `Facts`
    // in order to prove `Base1 + Offset1 <= Base2 + Offset2`. Note that in order to prove this,
    // Base1 must equal Base2 (as in EqualExtended), and the fact that
    // "Offset1 <= Offset2" must exist in `Facts`.
    //
    // TODO: we are ignoring the possibility of overflow in the addition.
    static bool LessThanOrEqualExtended(ASTContext &Ctx, Expr *Base1, Expr *Base2, Expr *Offset1, Expr *Offset2,
                                        EquivExprSets *EquivExprs, std::pair<ComparisonSet, ComparisonSet>& Facts) {
      if (!Offset1 && !Offset2)
        return false;

      if (!EqualValue(Ctx, Base1, Base2, EquivExprs))
        return false;

      llvm::APSInt ConstantPart1, ConstantPart2;
      bool IsOpSigned1, IsOpSigned2;
      Expr *VariablePart1, *VariablePart2;

      bool CreatedStdForm1 = CreateStandardForm(Ctx, Base1, Offset1, ConstantPart1, IsOpSigned1, VariablePart1);
      bool CreatedStdForm2 = CreateStandardForm(Ctx, Base2, Offset2, ConstantPart2, IsOpSigned2, VariablePart2);

      if (!CreatedStdForm1 || !CreatedStdForm2)
        return false;
      if (IsOpSigned1 != IsOpSigned2)
        return false;
      if (ConstantPart1 != ConstantPart2)
        return false;

      if (EqualValue(Ctx, VariablePart1, VariablePart2, EquivExprs))
        return true;
      if (FactExists(Ctx, VariablePart1, VariablePart2, EquivExprs, Facts))
        return true;

      return false;
    }

    // Given `Facts`, `E1`, and `E2`, this function looks for the fact `E1 <= E2` inside
    // the fatcs and returns true if it is able to find it. Otherwise, it returns false.
    static bool FactExists(ASTContext &Ctx, Expr *E1, Expr *E2, EquivExprSets *EquivExprs,
                           std::pair<ComparisonSet, ComparisonSet>& Facts) {
      bool ExistsIn = false, ExistsKill = false;
      for (auto InFact : Facts.first) {
        if (Lexicographic(Ctx, EquivExprs).CompareExpr(E1, InFact.first) == Lexicographic::Result::Equal &&
            Lexicographic(Ctx, EquivExprs).CompareExpr(E2, InFact.second) == Lexicographic::Result::Equal) {
          ExistsIn = true;
          break;
        }
      }
      for (auto KillFact : Facts.second) {
        if (Lexicographic(Ctx, EquivExprs).CompareExpr(E1, KillFact.first) == Lexicographic::Result::Equal &&
            Lexicographic(Ctx, EquivExprs).CompareExpr(E2, KillFact.second) == Lexicographic::Result::Equal) {
          ExistsKill = true;
          break;
        }
      }
      return ExistsIn && !ExistsKill;
    }

    static bool EqualValue(ASTContext &Ctx, Expr *E1, Expr *E2, EquivExprSets *EquivExprs) {
      Lexicographic::Result R = Lexicographic(Ctx, EquivExprs).CompareExpr(E1, E2);
      return R == Lexicographic::Result::Equal;
    }

    // Convert the bounds expression `Bounds` to a range `R`. This function returns true
    // if the conversion is successful, and false otherwise.
    // Currently, this function only performs the conversion for bounds expression of
    // kind Range and returns false for other kinds.
    //
    // Implementation details:
    // - First, SplitIntoBaseAndOffset is called on lower and upper fields in BoundsExpr to extract
    //   the bases and offsets. Note that offsets can be either ConstantSized or VariablesSized.
    // - Next, if the extracted lower base and upper base are equal, the function sets the base and
    //   the offsets of `R` based on the extracted values. Finally, it returns true to indicate success.
    //   If bases are not equal, R's fields will not be updated and the function returns false.
    bool CreateBaseRange(const BoundsExpr *Bounds, BaseRange *R,
                             EquivExprSets *EquivExprs) {
      switch (Bounds->getKind()) {
        case BoundsExpr::Kind::Invalid:
        case BoundsExpr::Kind::Unknown:
        case BoundsExpr::Kind::Any:
          return false;
        case BoundsExpr::Kind::ByteCount:
        case BoundsExpr::Kind::ElementCount:
          // TODO: fill these cases in.
          return false;
        case BoundsExpr::Kind::Range: {
          const RangeBoundsExpr *RB = cast<RangeBoundsExpr>(Bounds);
          Expr *Lower = RB->getLowerExpr();
          Expr *Upper = RB->getUpperExpr();
          Expr *LowerBase, *UpperBase;
          llvm::APSInt LowerOffsetConstant(1, true);
          llvm::APSInt  UpperOffsetConstant(1, true);
          Expr *LowerOffsetVariable = nullptr;
          Expr *UpperOffsetVariable = nullptr;
          SplitIntoBaseAndOffset(Lower, LowerBase, LowerOffsetConstant, LowerOffsetVariable);
          SplitIntoBaseAndOffset(Upper, UpperBase, UpperOffsetConstant, UpperOffsetVariable);

          // If both of the offsets are constants, the range is considered constant-sized.
          // Otherwise, it is a variable-sized range.
          if (EqualValue(S.Context, LowerBase, UpperBase, EquivExprs)) {
            R->SetBase(LowerBase);
            R->SetLowerConstant(LowerOffsetConstant);
            R->SetLowerVariable(LowerOffsetVariable);
            R->SetUpperConstant(UpperOffsetConstant);
            R->SetUpperVariable(UpperOffsetVariable);
            return true;
          }
        }
      }
      return false;
    }

    // Try to prove that SrcBounds implies the validity of DeclaredBounds.
    //
    // If Kind is StaticBoundsCast, check whether a static cast between Ptr
    // types from SrcBounds to DestBounds is legal.
    ProofResult ProveBoundsDeclValidity(const BoundsExpr *DeclaredBounds,
                                        const BoundsExpr *SrcBounds,
                                        ProofFailure &Cause,
                                        EquivExprSets *EquivExprs,
                                        ProofStmtKind Kind =
                                          ProofStmtKind::BoundsDeclaration) {
      assert(BoundsUtil::IsStandardForm(DeclaredBounds) &&
        "declared bounds not in standard form");
      assert(BoundsUtil::IsStandardForm(SrcBounds) &&
        "src bounds not in standard form");
      Cause = ProofFailure::None;

      // Ignore invalid bounds.
      if (SrcBounds->isInvalid() || DeclaredBounds->isInvalid())
        return ProofResult::True;

     // source bounds(any) implies that any other bounds is valid.
      if (SrcBounds->isAny())
        return ProofResult::True;

      // target bounds(unknown) implied by any other bounds.
      if (DeclaredBounds->isUnknown())
        return ProofResult::True;

      if (S.Context.EquivalentBounds(DeclaredBounds, SrcBounds, EquivExprs))
        return ProofResult::True;

      BaseRange DeclaredRange(S);
      BaseRange SrcRange(S);

      if (CreateBaseRange(DeclaredBounds, &DeclaredRange, EquivExprs) &&
          CreateBaseRange(SrcBounds, &SrcRange, EquivExprs)) {

#ifdef TRACE_RANGE
        llvm::outs() << "Found constant ranges:\n";
        llvm::outs() << "Declared bounds";
        DeclaredBounds->dump(llvm::outs());
        llvm::outs() << "\nSource bounds";
        SrcBounds->dump(llvm::outs());
        llvm::outs() << "\nDeclared range:";
        DeclaredRange.Dump(llvm::outs());
        llvm::outs() << "\nSource range:";
        SrcRange.Dump(llvm::outs());
#endif
        ProofResult R = SrcRange.InRange(DeclaredRange, Cause, EquivExprs, Facts);
        if (R == ProofResult::True)
          return R;
        if (R == ProofResult::False || R == ProofResult::Maybe) {
          if (R == ProofResult::False && SrcRange.IsEmpty())
            Cause = CombineFailures(Cause, ProofFailure::SrcEmpty);
          if (SrcRange.IsInvalid())
            Cause = CombineFailures(Cause, ProofFailure::SrcInvalid);
          if (DeclaredRange.IsConstantSizedRange() && SrcRange.IsConstantSizedRange()) {
            if (DeclaredRange.GetWidth() > SrcRange.GetWidth()) {
              Cause = CombineFailures(Cause, ProofFailure::Width);
              R = ProofResult::False;
            } else if (Kind == ProofStmtKind::StaticBoundsCast) {
              // For checking static casts between Ptr types, we only need to
              // prove that the declared width <= the source width.
              return ProofResult::True;
            }
          }
        }
        return R;
      }
      return ProofResult::Maybe;
    }

    // Try to prove that PtrBase + Offset is within Bounds, where PtrBase has pointer type.
    // Offset is optional and may be a nullptr.
    ProofResult ProveMemoryAccessInRange(Expr *PtrBase, Expr *Offset, BoundsExpr *Bounds,
                                         BoundsCheckKind Kind, ProofFailure &Cause) {
#ifdef TRACE_RANGE
      llvm::outs() << "Examining:\nPtrBase\n";
      PtrBase->dump(llvm::outs());
      llvm::outs() << "Offset = ";
      if (Offset != nullptr) {
        Offset->dump(llvm::outs());
      } else
        llvm::outs() << "nullptr\n";
      llvm::outs() << "Bounds\n";
      Bounds->dump(llvm::outs());
#endif
      assert(BoundsUtil::IsStandardForm(Bounds) &&
             "bounds not in standard form");
      Cause = ProofFailure::None;
      BaseRange ValidRange(S);

      // Currently, we do not try to prove whether the memory access is in range for non-constant ranges
      // TODO: generalize memory access range check to non-constants
      if (!CreateBaseRange(Bounds, &ValidRange, nullptr))
        return ProofResult::Maybe;
      if (ValidRange.IsVariableSizedRange())
        return ProofResult::Maybe;

      bool Overflow;
      llvm::APSInt ElementSize;
      if (!BoundsUtil::getReferentSizeInChars(S.Context, PtrBase->getType(), ElementSize))
          return ProofResult::Maybe;
      if (Kind == BoundsCheckKind::BCK_NullTermRead || Kind == BoundsCheckKind::BCK_NullTermWriteAssign) {
        Overflow = ValidRange.AddToUpper(ElementSize);
        if (Overflow)
          return ProofResult::Maybe;
      }

      Expr *AccessBase;
      llvm::APSInt AccessStartOffset;
      Expr *DummyOffset;
      // Currently, we do not try to prove whether the memory access is in range for non-constant ranges
      // TODO: generalize memory access range check to non-constants
      if (SplitIntoBaseAndOffset(PtrBase, AccessBase, AccessStartOffset, DummyOffset) != BaseRange::Kind::ConstantSized)
        return ProofResult::Maybe;

      // The access base for bounds_cast(e) should be a temporary binding of e.
      if (isa<BoundsCastExpr>(AccessBase))
        AccessBase = GetTempBinding(AccessBase);

      if (Offset) {
        llvm::APSInt IntVal;
        if (!Offset->isIntegerConstantExpr(IntVal, S.Context))
          return ProofResult::Maybe;
        IntVal = BoundsUtil::ConvertToSignedPointerWidth(S.Context, IntVal, Overflow);
        if (Overflow)
          return ProofResult::Maybe;
        IntVal = IntVal.smul_ov(ElementSize, Overflow);
        if (Overflow)
          return ProofResult::Maybe;
        AccessStartOffset = AccessStartOffset.sadd_ov(IntVal, Overflow);
        if (Overflow)
          return ProofResult::Maybe;
      }
      BaseRange MemoryAccessRange(S, AccessBase, AccessStartOffset,
                                           AccessStartOffset);
      Overflow = MemoryAccessRange.AddToUpper(ElementSize);
      if (Overflow)
        return ProofResult::Maybe;
#ifdef TRACE_RANGE
      llvm::outs() << "Memory access range:\n";
      MemoryAccessRange.Dump(llvm::outs());
      llvm::outs() << "Valid range:\n";
      ValidRange.Dump(llvm::outs());
#endif
      if (MemoryAccessRange.IsEmpty()) {
        Cause = CombineFailures(Cause, ProofFailure::DstEmpty);
        return ProofResult::False;
      }
      else if (MemoryAccessRange.IsInvalid()) {
        Cause = CombineFailures(Cause, ProofFailure::DstInvalid);
        return ProofResult::False;
      }
      std::pair<ComparisonSet, ComparisonSet> EmptyFacts;
      ProofResult R = ValidRange.InRange(MemoryAccessRange, Cause, nullptr, EmptyFacts);
      if (R == ProofResult::True)
        return R;
      if (R == ProofResult::False || R == ProofResult::Maybe) {
        if (R == ProofResult::False &&
            ValidRange.PartialOverlap(MemoryAccessRange) == ProofResult::True)
          Cause = CombineFailures(Cause, ProofFailure::PartialOverlap);
        if (ValidRange.IsEmpty()) {
          Cause = CombineFailures(Cause, ProofFailure::SrcEmpty);
          R = ProofResult::False;
        }
        if (ValidRange.IsInvalid()) {
          Cause = CombineFailures(Cause, ProofFailure::SrcInvalid);
          R = ProofResult::False;
        }
        if (MemoryAccessRange.GetWidth() > ValidRange.GetWidth()) {
          Cause = CombineFailures(Cause, ProofFailure::Width);
          R = ProofResult::False;
        }
      }
      return R;
    }

    // Convert ProofFailure codes into diagnostic notes explaining why the
    // statement involving bounds is false.
    void ExplainProofFailure(SourceLocation Loc, ProofFailure Cause,
                             ProofStmtKind Kind) {
      // Prefer diagnosis of empty bounds over bounds being too narrow.
      if (TestFailure(Cause, ProofFailure::SrcEmpty))
        S.Diag(Loc, diag::note_source_bounds_empty);
      else if (TestFailure(Cause, ProofFailure::DstEmpty))
        S.Diag(Loc, diag::note_destination_bounds_empty);
      else if (TestFailure(Cause, ProofFailure::SrcInvalid))
        S.Diag(Loc, diag::note_source_bounds_invalid);
      else if (TestFailure(Cause, ProofFailure::DstInvalid))
        S.Diag(Loc, diag::note_destination_bounds_invalid);
      else if (Kind != ProofStmtKind::StaticBoundsCast &&
               TestFailure(Cause, ProofFailure::Width))
        S.Diag(Loc, diag::note_bounds_too_narrow) << (unsigned)Kind;

      // Memory access/struct base error message.
      if (Kind == ProofStmtKind::MemoryAccess || Kind == ProofStmtKind::MemberArrowBase) {
        if (TestFailure(Cause, ProofFailure::PartialOverlap)) {
          S.Diag(Loc, diag::note_bounds_partially_overlap);
        }
      }

      if (TestFailure(Cause, ProofFailure::LowerBound))
        S.Diag(Loc, diag::note_lower_out_of_bounds) << (unsigned) Kind;
      if (TestFailure(Cause, ProofFailure::UpperBound))
        S.Diag(Loc, diag::note_upper_out_of_bounds) << (unsigned) Kind;
    }

    CHKCBindTemporaryExpr *GetTempBinding(Expr *E) {
      // Bounds casts should always have a temporary binding.
      if (BoundsCastExpr *BCE = dyn_cast<BoundsCastExpr>(E)) {
        CHKCBindTemporaryExpr *Result = dyn_cast<CHKCBindTemporaryExpr>(BCE->getSubExpr());
        return Result;
      }

      CHKCBindTemporaryExpr *Result =
        dyn_cast<CHKCBindTemporaryExpr>(E->IgnoreParenNoopCasts(S.getASTContext()));
      return Result;
    }

    // Given an assignment target = e, where target has declared bounds
    // DeclaredBounds and and e has inferred bounds SrcBounds, make sure
    // that SrcBounds implies that DeclaredBounds are provably true.
    void CheckBoundsDeclAtAssignment(SourceLocation ExprLoc, Expr *Target,
                                     BoundsExpr *DeclaredBounds, Expr *Src,
                                     BoundsExpr *SrcBounds,
                                     CheckedScopeSpecifier CSS) {
      // Record expression equality implied by assignment.
      SmallVector<SmallVector <Expr *, 4>, 4> EquivExprs;
      SmallVector<Expr *, 4> EqualExpr;

      if (S.CheckIsNonModifying(Target, Sema::NonModifyingContext::NMC_Unknown,
                                Sema::NonModifyingMessage::NMM_None)) {
         CHKCBindTemporaryExpr *Temp = GetTempBinding(Src);
         // TODO: make sure assignment to lvalue doesn't modify value used in Src.
         bool SrcIsNonModifying =
           S.CheckIsNonModifying(Src, Sema::NonModifyingContext::NMC_Unknown,
                                 Sema::NonModifyingMessage::NMM_None);
         if (Temp || SrcIsNonModifying) {
           Expr *TargetExpr =
             CreateImplicitCast(Target->getType(), CK_LValueToRValue, Target);
           EqualExpr.push_back(TargetExpr);
           if (Temp)
             EqualExpr.push_back(CreateTemporaryUse(Temp));
           else
             EqualExpr.push_back(Src);
           EquivExprs.push_back(EqualExpr);
         }
      }

      ProofFailure Cause;
      ProofResult Result = ProveBoundsDeclValidity(DeclaredBounds, SrcBounds,
                                                   Cause, &EquivExprs);
      if (Result != ProofResult::True) {
        unsigned DiagId = (Result == ProofResult::False) ?
          diag::error_bounds_declaration_invalid :
          (CSS != CheckedScopeSpecifier::CSS_Unchecked?
           diag::warn_checked_scope_bounds_declaration_invalid :
           diag::warn_bounds_declaration_invalid);
        S.Diag(ExprLoc, DiagId)
          << Sema::BoundsDeclarationCheck::BDC_Assignment << Target
          << Target->getSourceRange() << Src->getSourceRange();
        if (Result == ProofResult::False)
          ExplainProofFailure(ExprLoc, Cause, ProofStmtKind::BoundsDeclaration);
        S.Diag(Target->getExprLoc(), diag::note_declared_bounds)
          << DeclaredBounds << DeclaredBounds->getSourceRange();
        S.Diag(Src->getExprLoc(), diag::note_expanded_inferred_bounds)
          << SrcBounds << Src->getSourceRange();
      }
    }

    // Check that the bounds for an argument imply the expected
    // bounds for the argument.   The expected bounds are computed
    // by substituting the arguments into the bounds expression for
    // the corresponding parameter.
    void CheckBoundsDeclAtCallArg(unsigned ParamNum,
                                  BoundsExpr *ExpectedArgBounds, Expr *Arg,
                                  BoundsExpr *ArgBounds,
                                  CheckedScopeSpecifier CSS,
                                  SmallVector<SmallVector <Expr *, 4>, 4> *EquivExprs) {
      SourceLocation ArgLoc = Arg->getBeginLoc();
      ProofFailure Cause;
      ProofResult Result = ProveBoundsDeclValidity(ExpectedArgBounds,
                                                   ArgBounds, Cause, EquivExprs);
      if (Result != ProofResult::True) {
        unsigned DiagId = (Result == ProofResult::False) ?
          diag::error_argument_bounds_invalid :
          (CSS != CheckedScopeSpecifier::CSS_Unchecked ?
           diag::warn_checked_scope_argument_bounds_invalid :
           diag::warn_argument_bounds_invalid);
        S.Diag(ArgLoc, DiagId) << (ParamNum + 1) << Arg->getSourceRange();
        if (Result == ProofResult::False)
          ExplainProofFailure(ArgLoc, Cause, ProofStmtKind::BoundsDeclaration);
        S.Diag(ArgLoc, diag::note_expected_argument_bounds) << ExpectedArgBounds;
        S.Diag(Arg->getExprLoc(), diag::note_expanded_inferred_bounds)
          << ArgBounds << Arg->getSourceRange();
      }
    }

    // Given an initializer v = e, where v is a variable that has declared
    // bounds DeclaredBounds and and e has inferred bounds SrcBounds, make sure
    // that SrcBounds implies that DeclaredBounds are provably true.
    void CheckBoundsDeclAtInitializer(SourceLocation ExprLoc, VarDecl *D,
                                      BoundsExpr *DeclaredBounds, Expr *Src,
                                      BoundsExpr *SrcBounds,
                                      CheckedScopeSpecifier CSS) {
      // Record expression equality implied by initialization.
      SmallVector<SmallVector <Expr *, 4>, 4> EquivExprs;
      SmallVector<Expr *, 4> EqualExpr;
      // Record equivalence between expressions implied by initializion.
      // If D declares a variable V, and
      // 1. Src binds a temporary variable T, record equivalence
      //    beteween V and T.
      // 2. Otherwise, if Src is a non-modifying expression, record
      //    equivalence between V and Src.
      CHKCBindTemporaryExpr *Temp = GetTempBinding(Src);
      if (Temp ||  S.CheckIsNonModifying(Src, Sema::NonModifyingContext::NMC_Unknown,
                                         Sema::NonModifyingMessage::NMM_None)) {
        // TODO: make sure variable being initialized isn't read by Src.
        DeclRefExpr *TargetDeclRef =
          DeclRefExpr::Create(S.getASTContext(), NestedNameSpecifierLoc(),
                              SourceLocation(), D, false, SourceLocation(),
                              D->getType(), ExprValueKind::VK_LValue);
        CastKind Kind;
        QualType TargetTy;
        if (D->getType()->isArrayType()) {
          Kind = CK_ArrayToPointerDecay;
          TargetTy = S.getASTContext().getArrayDecayedType(D->getType());
        } else {
          Kind = CK_LValueToRValue;
          TargetTy = D->getType();
        }
        Expr *TargetExpr = CreateImplicitCast(TargetTy, Kind, TargetDeclRef);
        EqualExpr.push_back(TargetExpr);
        if (Temp)
          EqualExpr.push_back(CreateTemporaryUse(Temp));
        else
          EqualExpr.push_back(Src);
        EquivExprs.push_back(EqualExpr);
        /*
        llvm::outs() << "Dumping target/src equality relation\n";
        for (Expr *E : EqualExpr)
          E->dump(llvm::outs());
        */
      }
      ProofFailure Cause;
      ProofResult Result = ProveBoundsDeclValidity(DeclaredBounds,
                                                   SrcBounds, Cause, &EquivExprs);
      if (Result != ProofResult::True) {
        unsigned DiagId = (Result == ProofResult::False) ?
          diag::error_bounds_declaration_invalid :
          (CSS != CheckedScopeSpecifier::CSS_Unchecked ?
           diag::warn_checked_scope_bounds_declaration_invalid :
           diag::warn_bounds_declaration_invalid);
        S.Diag(ExprLoc, DiagId)
          << Sema::BoundsDeclarationCheck::BDC_Initialization << D
          << D->getLocation() << Src->getSourceRange();
        if (Result == ProofResult::False)
          ExplainProofFailure(ExprLoc, Cause, ProofStmtKind::BoundsDeclaration);
        S.Diag(D->getLocation(), diag::note_declared_bounds)
          << DeclaredBounds << D->getLocation();
        S.Diag(Src->getExprLoc(), diag::note_expanded_inferred_bounds)
          << SrcBounds << Src->getSourceRange();
      }
    }

    // Given a static cast to a Ptr type, where the Ptr type has
    // TargetBounds and the source has SrcBounds, make sure that (1) SrcBounds
    // implies Targetbounds or (2) the SrcBounds is at least as wide as
    // the TargetBounds.
    void CheckBoundsDeclAtStaticPtrCast(CastExpr *Cast,
                                        BoundsExpr *TargetBounds,
                                        Expr *Src,
                                        BoundsExpr *SrcBounds,
                                        CheckedScopeSpecifier CSS) {
      ProofFailure Cause;
      bool IsStaticPtrCast = (Src->getType()->isCheckedPointerPtrType() &&
                              Cast->getType()->isCheckedPointerPtrType());
      ProofStmtKind Kind = IsStaticPtrCast ? ProofStmtKind::StaticBoundsCast :
                             ProofStmtKind::BoundsDeclaration;
      ProofResult Result =
        ProveBoundsDeclValidity(TargetBounds, SrcBounds, Cause, nullptr, Kind);
      if (Result != ProofResult::True) {
        unsigned DiagId = (Result == ProofResult::False) ?
          diag::error_static_cast_bounds_invalid :
          (CSS != CheckedScopeSpecifier::CSS_Unchecked ?
           diag::warn_checked_scopestatic_cast_bounds_invalid :
           diag::warn_static_cast_bounds_invalid);
        SourceLocation ExprLoc = Cast->getExprLoc();
        S.Diag(ExprLoc, DiagId) << Cast->getType() << Cast->getSourceRange();
        if (Result == ProofResult::False)
          ExplainProofFailure(ExprLoc, Cause,
                              ProofStmtKind::StaticBoundsCast);
        S.Diag(ExprLoc, diag::note_required_bounds) << TargetBounds;
        S.Diag(ExprLoc, diag::note_expanded_inferred_bounds) << SrcBounds;
      }
    }

    void CheckBoundsAtMemoryAccess(Expr *Deref, BoundsExpr *ValidRange,
                                   BoundsCheckKind CheckKind,
                                   CheckedScopeSpecifier CSS) {
      ProofFailure Cause;
      ProofResult Result;
      ProofStmtKind ProofKind;
      #ifdef TRACE_RANGE
      llvm::outs() << "CheckBoundsMemAccess: Deref Expr: ";
      Deref->dumpPretty(S.Context);
      llvm::outs() << "\n";
      #endif
      if (UnaryOperator *UO = dyn_cast<UnaryOperator>(Deref)) {
        ProofKind = ProofStmtKind::MemoryAccess;
        Result = ProveMemoryAccessInRange(UO->getSubExpr(), nullptr, ValidRange,
                                          CheckKind, Cause);
      } else if (ArraySubscriptExpr *AS = dyn_cast<ArraySubscriptExpr>(Deref)) {
        ProofKind = ProofStmtKind::MemoryAccess;
        Result = ProveMemoryAccessInRange(AS->getBase(), AS->getIdx(),
                                          ValidRange, CheckKind, Cause);
      } else if (MemberExpr *ME = dyn_cast<MemberExpr>(Deref)) {
        assert(ME->isArrow());
        ProofKind = ProofStmtKind::MemberArrowBase;
        Result = ProveMemoryAccessInRange(ME->getBase(), nullptr, ValidRange, CheckKind, Cause);
      } else {
        llvm_unreachable("unexpected expression kind");
      }

      if (Result == ProofResult::False) {
        #ifdef TRACE_RANGE
        llvm::outs() << "Memory access Failure Causes:";
        DumpFailure(llvm::outs(), Cause);
        llvm::outs() << "\n";
        #endif
        unsigned DiagId = diag::error_out_of_bounds_access;
        SourceLocation ExprLoc = Deref->getExprLoc();
        S.Diag(ExprLoc, DiagId) << (unsigned) ProofKind << Deref->getSourceRange();
        ExplainProofFailure(ExprLoc, Cause, ProofKind);
        S.Diag(ExprLoc, diag::note_expanded_inferred_bounds) << ValidRange;
      }
    }


  public:
    CheckBoundsDeclarations(Sema &SemaRef, Stmt *Body, CFG *Cfg, BoundsExpr *ReturnBounds, std::pair<ComparisonSet, ComparisonSet> &Facts) : S(SemaRef),
      DumpBounds(SemaRef.getLangOpts().DumpInferredBounds),
      DumpState(SemaRef.getLangOpts().DumpCheckingState),
      PointerWidth(SemaRef.Context.getTargetInfo().getPointerWidth(0)),
      Body(Body),
      Cfg(Cfg),
      ReturnBounds(ReturnBounds),
      Context(SemaRef.Context),
      Facts(Facts),
      BoundsAnalyzer(BoundsAnalysis(SemaRef, Cfg)),
      IncludeNullTerminator(false) {}

    CheckBoundsDeclarations(Sema &SemaRef, std::pair<ComparisonSet, ComparisonSet> &Facts) : S(SemaRef),
      DumpBounds(SemaRef.getLangOpts().DumpInferredBounds),
      DumpState(SemaRef.getLangOpts().DumpCheckingState),
      PointerWidth(SemaRef.Context.getTargetInfo().getPointerWidth(0)),
      Body(nullptr),
      Cfg(nullptr),
      ReturnBounds(nullptr),
      Context(SemaRef.Context),
      Facts(Facts),
      BoundsAnalyzer(BoundsAnalysis(SemaRef, nullptr)),
      IncludeNullTerminator(false) {}

    typedef llvm::SmallPtrSet<const Stmt *, 16> StmtSet;

    void IdentifyChecked(Stmt *S, StmtSet &MemoryCheckedStmts, StmtSet &BoundsCheckedStmts, CheckedScopeSpecifier CSS) {
      if (!S)
        return;

      if (CSS == CheckedScopeSpecifier::CSS_Memory)
        if (isa<Expr>(S) || isa<DeclStmt>(S) || isa<ReturnStmt>(S))
          MemoryCheckedStmts.insert(S);

      if (CSS == CheckedScopeSpecifier::CSS_Bounds)
        if (isa<Expr>(S) || isa<DeclStmt>(S) || isa<ReturnStmt>(S))
          BoundsCheckedStmts.insert(S);

      if (const CompoundStmt *CS = dyn_cast<CompoundStmt>(S))
        CSS = CS->getCheckedSpecifier();

      auto Begin = S->child_begin(), End = S->child_end();
      for (auto I = Begin; I != End; ++I)
        IdentifyChecked(*I, MemoryCheckedStmts, BoundsCheckedStmts, CSS);
    }

    // Add any subexpressions of S that occur in TopLevelElems to NestedExprs.
    void MarkNested(const Stmt *S, StmtSet &NestedExprs, StmtSet &TopLevelElems) {
      auto Begin = S->child_begin(), End = S->child_end();
      for (auto I = Begin; I != End; ++I) {
        const Stmt *Child = *I;
        if (!Child)
          continue;
        if (TopLevelElems.find(Child) != TopLevelElems.end())
          NestedExprs.insert(Child);
        MarkNested(Child, NestedExprs, TopLevelElems);
      }
   }

  // Identify CFG elements that are statements that are substatements of other
  // CFG elements.  (CFG elements are the components of basic blocks).  When a
  // CFG is constructed, subexpressions of top-level expressions may be placed
  // in separate CFG elements.  This is done for subexpressions of expressions
  // with control-flow, for example. When checking bounds declarations, we want
  // to process a subexpression with its enclosing expression. We want to
  // ignore CFG elements that are substatements of other CFG elements.
  //
  // As an example, given a conditional expression, all subexpressions will
  // be made into separate CFG elements.  The expression
  //    x = (cond == 0) ? f1() : f2(),
  // has a CFG of the form:
  //    B1:
  //     1: cond == 0
  //     branch cond == 0 B2, B3
  //   B2:
  //     1: f1();
  //     jump B4
  //   B3:
  //     1: f2();
  //     jump B4
  //   B4:
  //     1: x = (cond == 0) ? f1 : f2();
  //
  // For now, we want to skip B1.1, B2.1, and B3.1 because they will be processed
  // as part of B4.1.
   void FindNestedElements(StmtSet &NestedStmts) {
      // Create the set of top-level CFG elements.
      StmtSet TopLevelElems;
      for (const CFGBlock *Block : *Cfg) {
        for (CFGElement Elem : *Block) {
          if (Elem.getKind() == CFGElement::Statement) {
            CFGStmt CS = Elem.castAs<CFGStmt>();
            const Stmt *S = CS.getStmt();
            TopLevelElems.insert(S);
          }
        }
      }

      // Create the set of top-level elements that are subexpressions
      // of other top-level elements.
      for (const CFGBlock *Block : *Cfg) {
        for (CFGElement Elem : *Block) {
          if (Elem.getKind() == CFGElement::Statement) {
            CFGStmt CS = Elem.castAs<CFGStmt>();
            const Stmt *S = CS.getStmt();
            MarkNested(S, NestedStmts, TopLevelElems);
          }
        }
      }
   }

   void ResetKilledBounds(StmtDeclSetTy &KilledBounds, Stmt *St,
                          BoundsContextTy &ObservedBounds) {
     auto I = KilledBounds.find(St);
     if (I == KilledBounds.end())
       return;

     // KilledBounds stores a mapping of statements to all variables whose
     // bounds are killed by each statement. Here we reset the bounds of all
     // variables killed by the statement S to the declared bounds.
     for (const VarDecl *V : I->second) {
       if (const BoundsExpr *Bounds = V->getBoundsExpr())

         // TODO: Throughout clang in general (and inside dataflow analysis in
         // particular) we repeatedly invoke ExpandBoundsToRange in order to
         // canonicalize the bounds of a variable to RangeBoundsExpr. Sometimes
         // we do this multiple times for the same variable. This is very
         // inefficient because ExpandBoundsToRange can allocate AST data
         // structures that are permanently allocated and increase the memory
         // usage of the compiler. The solution is to canonicalize the bounds
         // once and attach it to the VarDecl. See issue
         // https://github.com/microsoft/checkedc-clang/issues/830.

         ObservedBounds[V] = S.ExpandBoundsToRange(V, Bounds);
     }
   }

   void UpdateCtxWithWidenedBounds(BoundsMapTy &WidenedBounds,
                                   BoundsContextTy &ObservedBounds) {
     // WidenedBounds contains the mapping from _Nt_array_ptr to the offset by
     // which its declared bounds should be widened. In this function we apply
     // the offset to the declared bounds of the _Nt_array_ptr and update its
     // bounds in ObservedBounds.

     for (const auto item : WidenedBounds) {
       const VarDecl *V = item.first;
       unsigned Offset = item.second;

       // We normalize the declared bounds to RangBoundsExpr here so that we
       // can easily apply the offset to the upper bound.

       // TODO: Throughout clang in general (and inside dataflow analysis in
       // particular) we repeatedly invoke ExpandBoundsToRange in order to
       // canonicalize the bounds of a variable to RangeBoundsExpr. Sometimes
       // we do this multiple times for the same variable. This is very
       // inefficient because ExpandBoundsToRange can allocate AST data
       // structures that are permanently allocated and increase the memory
       // usage of the compiler. The solution is to canonicalize the bounds
       // once and attach it to the VarDecl. See issue
       // https://github.com/microsoft/checkedc-clang/issues/830.

       BoundsExpr *Bounds = S.ExpandBoundsToRange(V, V->getBoundsExpr());
       if (RangeBoundsExpr *RBE = dyn_cast<RangeBoundsExpr>(Bounds)) {
         const llvm::APInt
           APIntOff(Context.getTargetInfo().getPointerWidth(0), Offset);
         IntegerLiteral *WidenedOffset = CreateIntegerLiteral(APIntOff);

         Expr *Lower = RBE->getLowerExpr();
         Expr *Upper = RBE->getUpperExpr();

         // WidenedUpperBound = UpperBound + WidenedOffset.
         Expr *WidenedUpper = ExprCreatorUtil::CreateBinaryOperator(
                                S, Upper, WidenedOffset,
                                BinaryOperatorKind::BO_Add);

         RangeBoundsExpr *R =
           new (Context) RangeBoundsExpr(Lower, WidenedUpper,
                                         SourceLocation(), SourceLocation());
         ObservedBounds[V] = R;
       }
     }
   }

   // Walk the CFG, traversing basic blocks in reverse post-oder.
   // For each element of a block, check bounds declarations.  Skip
   // CFG elements that are subexpressions of other CFG elements.
   void TraverseCFG(AvailableFactsAnalysis& AFA, FunctionDecl *FD) {
     assert(Cfg && "expected CFG to exist");
#if TRACE_CFG
     llvm::outs() << "Dumping AST";
     Body->dump(llvm::outs());
     llvm::outs() << "Dumping CFG:\n";
     Cfg->print(llvm::outs(), S.getLangOpts(), true);
     llvm::outs() << "Traversing CFG:\n";
#endif

     // Map each function parameter to its declared bounds (if any),
     // normalized to range bounds, before checking the body of the function.
     // The context formed by the declared parameter bounds is the initial
     // observed bounds context for checking the function body.
     CheckingState ParamsState;
     for (auto I = FD->param_begin(); I != FD->param_end(); ++I) {
       ParmVarDecl *Param = *I;
       BoundsExpr *Bounds = Param->getBoundsExpr();
       if (Bounds)
         ParamsState.ObservedBounds[Param] = ExpandToRange(Param, Bounds);
     }

     // Store a checking state for each CFG block in order to track
     // the variables with bounds declarations that are in scope.
     llvm::DenseMap<unsigned int, CheckingState> BlockStates;
     BlockStates[Cfg->getEntry().getBlockID()] = ParamsState;

     StmtSet NestedElements;
     FindNestedElements(NestedElements);
     StmtSet MemoryCheckedStmts;
     StmtSet BoundsCheckedStmts;
     IdentifyChecked(Body, MemoryCheckedStmts, BoundsCheckedStmts, CheckedScopeSpecifier::CSS_Unchecked);

     // Run the bounds widening analysis on this function.
     BoundsAnalysis BA = getBoundsAnalyzer();
     BA.WidenBounds(FD);
     if (S.getLangOpts().DumpWidenedBounds)
       BA.DumpWidenedBounds(FD);

     PostOrderCFGView POView = PostOrderCFGView(Cfg);
     ResetFacts();
     for (const CFGBlock *Block : POView) {
       AFA.GetFacts(Facts);
       CheckingState BlockState = GetIncomingBlockState(Block, BlockStates);

       // Get the widened bounds for the current block as computed by the
       // bounds widening analysis invoked by WidenBounds above.
       BoundsMapTy WidenedBounds = BA.GetWidenedBounds(Block);
       // Also get the bounds killed (if any) by each statement in the current
       // block.
       StmtDeclSetTy KilledBounds = BA.GetKilledBounds(Block);
       // Update the Observed bounds with the widened bounds calculated above.
       UpdateCtxWithWidenedBounds(WidenedBounds, BlockState.ObservedBounds);

       for (CFGElement Elem : *Block) {
         if (Elem.getKind() == CFGElement::Statement) {
           CFGStmt CS = Elem.castAs<CFGStmt>();
           // We may attach a bounds expression to Stmt, so drop the const
           // modifier.
           Stmt *S = const_cast<Stmt *>(CS.getStmt());

           // Skip top-level elements that are nested in
           // another top-level element.
           if (NestedElements.find(S) != NestedElements.end())
             continue;

           CheckedScopeSpecifier CSS = CheckedScopeSpecifier::CSS_Unchecked;
           const Stmt *Statement = S;
           if (DeclStmt *DS = dyn_cast<DeclStmt>(S))
             // CFG construction will synthesize decl statements so that
             // each declarator is a separate CFGElem.  To see if we are in
             // a checked scope, look at the original decl statement.
             Statement = Cfg->getSourceDeclStmt(DS);
           if (MemoryCheckedStmts.find(Statement) != MemoryCheckedStmts.end())
             CSS = CheckedScopeSpecifier::CSS_Memory;
           else if (BoundsCheckedStmts.find(Statement) != BoundsCheckedStmts.end())
             CSS = CheckedScopeSpecifier::CSS_Bounds;

#if TRACE_CFG
            llvm::outs() << "Visiting ";
            S->dump(llvm::outs());
            llvm::outs().flush();
#endif
            // Modify the ObservedBounds context to include any variables with
            // bounds that are declared in S.  Before checking S, the observed
            // bounds for each variable v that is in scope are the widened
            // bounds for v (if any), or the declared bounds for v (if any).
            GetDeclaredBounds(this->S, BlockState.ObservedBounds, S);

            // If any bounds are killed by statement S, reset their bounds
            // to their declared bounds.
            ResetKilledBounds(KilledBounds, S, BlockState.ObservedBounds);

            BoundsContextTy InitialObservedBounds = BlockState.ObservedBounds;
            BlockState.G.clear();

            Check(S, CSS, BlockState);

            if (DumpState)
              DumpCheckingState(llvm::outs(), S, BlockState);

            // TODO: for each variable v in ObservedBounds, check that the
            // observed bounds of v imply the declared bounds of v.

            // The observed bounds that were updated after checking S should
            // only be used to check that the updated observed bounds imply
            // the declared variable bounds.  After checking the observed and
            // declared bounds, the observed bounds for each variable should
            // be reset to their observed bounds from before checking S.
            BlockState.ObservedBounds = InitialObservedBounds;
         }
       }
       if (Block->getBlockID() != Cfg->getEntry().getBlockID())
         BlockStates[Block->getBlockID()] = BlockState;
       AFA.Next();
     }
    }

  // Methods for inferring bounds expressions for C expressions.

  // C has an interesting semantics for expressions that differentiates between
  // lvalue and value expressions and inserts implicit conversions from lvalues
  // to values.  Value expressions are usually called rvalue expressions.  This
  // semantics is represented directly in the clang IR by having some
  // expressions evaluate to lvalues and having implicit conversions that convert
  // those lvalues to rvalues.
  //
  // Using ths representation directly would make it clumsy to compute bounds
  // expressions.  For an expression that evaluates to an lvalue, we would have
  // to compute and carry along two bounds expressions: the bounds expression
  // for the lvalue and the bounds expression for the value at which the lvalue
  // points.
  //
  // We address this by having two methods for computing bounds.  One method
  // (Check) computes the bounds for an rvalue expression. For lvalue
  // expressions, we have one method that compute two kinds of bounds.
  // CheckLValue computes the bounds for the lvalue produced by an expression
  // and the bounds for the target of the lvalue produced by the expression.
  //
  // There are only a few contexts where an lvalue expression can occur, so it
  // is straightforward to determine which method to use. Also, the clang IR
  // makes it explicit when an lvalue is converted to an rvalue by an lvalue
  // cast operation.
  //
  // An expression denotes an lvalue if it occurs in the following contexts:
  // 1. As the left-hand side of an assignment operator.
  // 2. As the operand to a postfix or prefix incrementation operators (which
  //    implicitly do assignment).
  // 3. As the operand of the address-of (&) operator.
  // 4. If a member access operation e1.f denotes an lvalue, e1 denotes an
  //    lvalue.
  // 5. In clang IR, as an operand to an LValueToRValue cast operation.
  // Otherwise an expression denotes an rvalue.

  public:
    BoundsExpr *Check(Stmt *S, CheckedScopeSpecifier CSS) {
      CheckingState State;
      BoundsExpr *Bounds = Check(S, CSS, State);
      if (DumpState)
        DumpCheckingState(llvm::outs(), S, State);
      return Bounds;
    }

    // If e is an rvalue, Check checks e and its children, performing any
    // necessary side effects, and returns the bounds for the value
    // produced by e.
    // If e is an lvalue, Check checks e and its children, performing any
    // necessary side effects, and returns unknown bounds.
    //
    // The returned bounds expression may contain a modifying expression within
    // it. It is the caller's responsibility to validate that the bounds
    // expression is non-modifying.
    //
    // Check recursively checks the children of e and performs any
    // necessary side effects on e.  Check and CheckLValue work together
    // to traverse each expression in a CFG exactly once.
    //
    // State is an out parameter that holds the result of Check.
    BoundsExpr *Check(Stmt *S, CheckedScopeSpecifier CSS, CheckingState &State) {
      if (!S)
        return CreateBoundsEmpty();

      if (Expr *E = dyn_cast<Expr>(S)) {
        E = E->IgnoreParens();
        S = E;
        if (E->isLValue()) {
          BoundsExpr *TargetBounds = nullptr;
          CheckLValue(E, CSS, TargetBounds, State);
          return CreateBoundsAlwaysUnknown();
        }
      }

      BoundsExpr *ResultBounds = CreateBoundsAlwaysUnknown();

      switch (S->getStmtClass()) {
        case Expr::UnaryOperatorClass:
          ResultBounds = CheckUnaryOperator(cast<UnaryOperator>(S),
                                            CSS, State);
          break;
        case Expr::CallExprClass:
          ResultBounds = CheckCallExpr(cast<CallExpr>(S), CSS, State);
          break;
        case Expr::ImplicitCastExprClass:
        case Expr::CStyleCastExprClass:
        case Expr::BoundsCastExprClass:
          ResultBounds = CheckCastExpr(cast<CastExpr>(S), CSS, State);
          break;
        case Expr::BinaryOperatorClass:
        case Expr::CompoundAssignOperatorClass:
          ResultBounds = CheckBinaryOperator(cast<BinaryOperator>(S),
                                             CSS, State);
          break;
        case Stmt::CompoundStmtClass: {
          CompoundStmt *CS = cast<CompoundStmt>(S);
          CSS = CS->getCheckedSpecifier();
          // Check may be called on a CompoundStmt if a CFG could not be
          // constructed, so check the children of a CompoundStmt.
          CheckChildren(CS, CSS, State);
          break;
        }
        case Stmt::DeclStmtClass: {
          DeclStmt *DS = cast<DeclStmt>(S);
          auto BeginDecls = DS->decl_begin(), EndDecls = DS->decl_end();
          for (auto I = BeginDecls; I != EndDecls; ++I) {
            Decl *D = *I;
            // If an initializer expression is present, it is visited
            // during the traversal of the variable declaration.
            if (VarDecl *VD = dyn_cast<VarDecl>(D))
              ResultBounds = CheckVarDecl(VD, CSS, State);
          }
          break;
        }
        case Stmt::ReturnStmtClass:
          ResultBounds = CheckReturnStmt(cast<ReturnStmt>(S), CSS, State);
          break;
        case Stmt::CHKCBindTemporaryExprClass: {
          CHKCBindTemporaryExpr *Binding = cast<CHKCBindTemporaryExpr>(S);
          ResultBounds = CheckTemporaryBinding(Binding, CSS, State);
          break;
        }
        case Expr::ConditionalOperatorClass:
        case Expr::BinaryConditionalOperatorClass: {
          AbstractConditionalOperator *ACO = cast<AbstractConditionalOperator>(S);
          ResultBounds = CheckConditionalOperator(ACO, CSS, State);
          break;
        }
        case Expr::BoundsValueExprClass:
          ResultBounds = CheckBoundsValueExpr(cast<BoundsValueExpr>(S),
                                              CSS, State);
          break;
        default:
          CheckChildren(S, CSS, State);
          break;
      }

      if (Expr *E = dyn_cast<Expr>(S)) {
        // Bounds expressions are not null ptrs.
        if (isa<BoundsExpr>(E))
          return ResultBounds;

        // Temporary bindings are not null ptrs.
        if (isa<CHKCBindTemporaryExpr>(E))
          return ResultBounds;

        // Null ptrs always have bounds(any).
        // This is the correct way to detect all the different ways that
        // C can make a null ptr.
        if (E->isNullPointerConstant(Context, Expr::NPC_NeverValueDependent))
          return CreateBoundsAny();
      }

      return ResultBounds;
    }

    // Infer the bounds for an lvalue and the bounds for the target
    // of the lvalue.
    //
    // The lvalue bounds determine whether it is valid to access memory
    // using the lvalue.  The bounds should be the range of an object in
    // memory or a subrange of an object.
    // Values assigned through the lvalue must satisfy the target bounds.
    // Values read through the lvalue will meet the target bounds.
    //
    // The returned bounds expressions may contain a modifying expression within
    // them. It is the caller's responsibility to validate that the bounds
    // expressions are non-modifying.
    //
    // CheckLValue recursively checks the children of e and performs any
    // necessary side effects on e.  Check and CheckLValue work together
    // to traverse each expression in a CFG exactly once.
    //
    // State is an out parameter that holds the result of Check.
    BoundsExpr *CheckLValue(Expr *E, CheckedScopeSpecifier CSS,
                            BoundsExpr *&OutTargetBounds,
                            CheckingState &State) {
      if (!E->isLValue())
        return CreateBoundsInferenceError();

      E = E->IgnoreParens();

      OutTargetBounds = CreateBoundsAlwaysUnknown();
      BoundsExpr *Bounds = CreateBoundsAlwaysUnknown();

      switch (E->getStmtClass()) {
        case Expr::DeclRefExprClass:
          Bounds = CheckDeclRefExpr(cast<DeclRefExpr>(E),
                                    CSS, OutTargetBounds, State);
          break;
        case Expr::UnaryOperatorClass:
          Bounds = CheckUnaryLValue(cast<UnaryOperator>(E),
                                    CSS, OutTargetBounds, State);
          break;
        case Expr::ArraySubscriptExprClass:
          Bounds = CheckArraySubscriptExpr(cast<ArraySubscriptExpr>(E),
                                           CSS, OutTargetBounds, State);
          break;
        case Expr::MemberExprClass:
          Bounds = CheckMemberExpr(cast<MemberExpr>(E),
                                   CSS, OutTargetBounds, State);
          break;
        case Expr::ImplicitCastExprClass:
          Bounds = CheckCastLValue(cast<CastExpr>(E),
                                   CSS, OutTargetBounds, State);
          break;
        case Expr::CHKCBindTemporaryExprClass:
          Bounds = CheckTempBindingLValue(cast<CHKCBindTemporaryExpr>(E),
                                          CSS, OutTargetBounds, State);
          break;
        default:
          CheckChildren(E, CSS, State);
          break;
      }

      // The type for inferring the target bounds cannot ever be an array
      // type, as these are dealt with by an array conversion, not an lvalue
      // conversion. The bounds for an array conversion are the same as the
      // lvalue bounds of the array-typed expression.
      if (E->getType()->isArrayType())
        OutTargetBounds = CreateBoundsInferenceError();

      return Bounds;
    }

    // CheckChildren recursively checks and performs any side effects on the
    // children of a statement or expression, throwing away the resulting
    // bounds.
    void CheckChildren(Stmt *S, CheckedScopeSpecifier CSS,
                       CheckingState &State) {
      ExprEqualMapTy SubExprGs;
      auto Begin = S->child_begin(), End = S->child_end();

      for (auto I = Begin; I != End; ++I) {
        Stmt *Child = *I;
        if (!Child) continue;
        // Accumulate the UEQ from checking each child into the UEQ for S.
        Check(Child, CSS, State);

        // Store the set Gi for each subexpression Si.
        if (Expr *SubExpr = dyn_cast<Expr>(Child))
          SubExprGs[SubExpr] = State.G;
      }

      // Use the stored sets Gi for each subexpression Si
      // to update the set G for the expression S.
      if (Expr *E = dyn_cast<Expr>(S))
        UpdateG(E, SubExprGs, State.G);
    }

    // Traverse a top-level variable declaration.  If there is an
    // initializer, it will be traversed in CheckVarDecl.
    void TraverseTopLevelVarDecl(VarDecl *VD, CheckedScopeSpecifier CSS) {
      ResetFacts();
      CheckingState State;
      CheckVarDecl(VD, CSS, State);
    }

    void ResetFacts() {
      std::pair<ComparisonSet, ComparisonSet> EmptyFacts;
      Facts = EmptyFacts;
    }

    bool IsBoundsSafeInterfaceAssignment(QualType DestTy, Expr *E) {
      if (DestTy->isUncheckedPointerType()) {
        ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E);
        if (ICE)
          return ICE && ICE->getCastKind() == CK_BitCast &&
                 ICE->getSubExpr()->getType()->isCheckedPointerType();
      }
      return false;
    }

  // Methods to infer bounds for an expression that produces an rvalue.

  private:
    // CheckBinaryOperator returns the bounds for the value produced by e.
    // e is an rvalue.
    BoundsExpr *CheckBinaryOperator(BinaryOperator *E,
                                    CheckedScopeSpecifier CSS,
                                    CheckingState &State) {
      Expr *LHS = E->getLHS();
      Expr *RHS = E->getRHS();
      ExprEqualMapTy SubExprGs;

      // Infer the lvalue or rvalue bounds of the LHS, saving the set G 
      // of expressions that produce the same value as the LHS.
      BoundsExpr *LHSTargetBounds, *LHSLValueBounds, *LHSBounds;
      InferBounds(LHS, CSS, LHSTargetBounds,
                  LHSLValueBounds, LHSBounds, State);
      SubExprGs[LHS] = State.G;

      // Infer the rvalue bounds of the RHS, saving the set G
      // of expressions that produce the same value as the RHS.
      BoundsExpr *RHSBounds = Check(RHS, CSS, State);
      SubExprGs[RHS] = State.G;

      BinaryOperatorKind Op = E->getOpcode();

      // Bounds of the binary operator.
      BoundsExpr *ResultBounds = CreateBoundsEmpty();

      // Floating point expressions have empty bounds.
      if (E->getType()->isFloatingType())
        ResultBounds = CreateBoundsEmpty();

      // `e1 = e2` has the bounds of `e2`. `e2` is an RValue.
      else if (Op == BinaryOperatorKind::BO_Assign)
        ResultBounds = RHSBounds;

      // `e1, e2` has the bounds of `e2`. Both `e1` and `e2`
      // are RValues.
      else if (Op == BinaryOperatorKind::BO_Comma)
        ResultBounds = RHSBounds;
      
      else {
        // Compound Assignments function like assignments mostly,
        // except the LHS is an L-Value, so we'll use its lvalue target bounds
        bool IsCompoundAssignment = false;
        if (BinaryOperator::isCompoundAssignmentOp(Op)) {
          Op = BinaryOperator::getOpForCompoundAssignment(Op);
          IsCompoundAssignment = true;
        }

        // Pointer arithmetic.
        //
        // `p + i` has the bounds of `p`. `p` is an RValue.
        // `p += i` has the lvalue target bounds of `p`. `p` is an LValue. `p += i` is an RValue
        // same applies for `-` and `-=` respectively
        if (LHS->getType()->isPointerType() &&
            RHS->getType()->isIntegerType() &&
            BinaryOperator::isAdditiveOp(Op)) {
          ResultBounds = IsCompoundAssignment ?
            LHSTargetBounds : LHSBounds;
        }
        // `i + p` has the bounds of `p`. `p` is an RValue.
        // `i += p` has the bounds of `p`. `p` is an RValue.
        else if (LHS->getType()->isIntegerType() &&
            RHS->getType()->isPointerType() &&
            Op == BinaryOperatorKind::BO_Add) {
          ResultBounds = RHSBounds;
        }
        // `e - p` has empty bounds, regardless of the bounds of p.
        // `e -= p` has empty bounds, regardless of the bounds of p.
        else if (RHS->getType()->isPointerType() &&
            Op == BinaryOperatorKind::BO_Sub) {
          ResultBounds = CreateBoundsEmpty();
        }

        // Arithmetic on integers with bounds.
        //
        // `e1 @ e2` has the bounds of whichever of `e1` or `e2` has bounds.
        // if both `e1` and `e2` have bounds, then they must be equal.
        // Both `e1` and `e2` are RValues
        //
        // `e1 @= e2` has the bounds of whichever of `e1` or `e2` has bounds.
        // if both `e1` and `e2` have bounds, then they must be equal.
        // `e1` is an LValue, its bounds are the lvalue target bounds.
        // `e2` is an RValue
        //
        // @ can stand for: +, -, *, /, %, &, |, ^, >>, <<
        else if (LHS->getType()->isIntegerType() &&
            RHS->getType()->isIntegerType() &&
            (BinaryOperator::isAdditiveOp(Op) ||
              BinaryOperator::isMultiplicativeOp(Op) ||
              BinaryOperator::isBitwiseOp(Op) ||
              BinaryOperator::isShiftOp(Op))) {
          BoundsExpr *LeftBounds = IsCompoundAssignment ?
            LHSTargetBounds : LHSBounds;
          if (LeftBounds->isUnknown() && !RHSBounds->isUnknown())
            ResultBounds = RHSBounds;
          else if (!LeftBounds->isUnknown() && RHSBounds->isUnknown())
            ResultBounds = LeftBounds;
          else if (!LeftBounds->isUnknown() && !RHSBounds->isUnknown()) {
            // TODO: Check if LeftBounds and RHSBounds are equal.
            // if so, return one of them. If not, return bounds(unknown)
            ResultBounds = CreateBoundsAlwaysUnknown();
          }
          else if (LeftBounds->isUnknown() && RHSBounds->isUnknown())
            ResultBounds = CreateBoundsEmpty();
        }
      }

      // Update State.UEQ and State.G.
      if (E->isAssignmentOp()) {
        Expr *Target =
             CreateImplicitCast(LHS->getType(), CK_LValueToRValue, LHS);
        Expr *Src = RHS;

        // A compound assignment `e1 @= e2` implies an assignment `e1 = e1 @ e2`.
        if (E->isCompoundAssignmentOp()) {
          // Create the RHS of the implied assignment `e1 = e1 @ e2`.
          Src = ExprCreatorUtil::CreateBinaryOperator(S, Target, RHS, Op);

          // Update State.G to be the set of expressions that produce the same
          // value as the source `e1 @ e2` of the assignment `e1 = e1 @ e2`.
          UpdateG(Src, SubExprGs, State.G);
        }

        // Update UEQ and G for assignments to `e1` where `e1` is a variable.
        if (DeclRefExpr *V = GetLValueVariable(LHS)) {
          bool OVUsesV = false;
          Expr *OV = GetOriginalValue(V, Target, Src, State.UEQ, OVUsesV);
          UpdateAfterAssignment(V, Target, OV, OVUsesV, CSS, State, State);
        }
        // Update UEQ and G for assignments where `e1` is not a variable.
        else {
          // G is empty for assignments to a non-variable.  This conservative
          // approach avoids recording false equality facts for assignments
          // where the LHS appears on the RHS, e.g. *p = *p + 1.
          State.G.clear();
        }
      } else if (BinaryOperator::isLogicalOp(Op)) {
        // TODO: update State for logical operators `e1 && e2` and `e1 || e2`.
      } else if (Op == BinaryOperatorKind::BO_Comma) {
        // Do nothing for comma operators `e1, e2`. State already contains
        // the correct UEQ and G sets as a result of checking `e1` and `e2`.
      } else {
        // For all other binary operators `e1 @ e2`, use the G sets for
        // `e1` and `e2` stored in SubExprGs to update State.G for `e1 @ e2`.
        UpdateG(E, SubExprGs, State.G);
      }

      if (E->isAssignmentOp()) {
        QualType LHSType = LHS->getType();
        // Bounds of the right-hand side of the assignment
        BoundsExpr *RightBounds = nullptr;

        if (!E->isCompoundAssignmentOp() &&
            LHSType->isCheckedPointerPtrType() &&
            RHS->getType()->isCheckedPointerPtrType()) {
          // ptr<T> to ptr<T> assignment, no obligation to check assignment bounds
        }
        else if (LHSType->isCheckedPointerType() ||
                  LHSType->isIntegerType() ||
                  IsBoundsSafeInterfaceAssignment(LHSType, RHS)) {
          // Check that the value being assigned has bounds if the
          // target of the LHS lvalue has bounds.
          LHSTargetBounds = S.CheckNonModifyingBounds(LHSTargetBounds, LHS);
          if (!LHSTargetBounds->isUnknown()) {
            if (E->isCompoundAssignmentOp())
              RightBounds = S.CheckNonModifyingBounds(ResultBounds, E);
            else
              RightBounds = S.CheckNonModifyingBounds(ResultBounds, RHS);

            if (RightBounds->isUnknown()) {
                S.Diag(RHS->getBeginLoc(),
                      diag::err_expected_bounds_for_assignment)
                      << RHS->getSourceRange();
                RightBounds = S.CreateInvalidBoundsExpr();
            }

            CheckBoundsDeclAtAssignment(E->getExprLoc(), LHS, LHSTargetBounds,
                                        RHS, RightBounds, CSS);
          }
        }

        // Check that the LHS lvalue of the assignment has bounds, if it is an
        // lvalue that was produced by dereferencing an _Array_ptr.
        bool LHSNeedsBoundsCheck = false;
        OperationKind OpKind = (E->getOpcode() == BO_Assign) ?
          OperationKind::Assign : OperationKind::Other;
        LHSNeedsBoundsCheck = AddBoundsCheck(LHS, OpKind, CSS, LHSLValueBounds);
        if (DumpBounds && (LHSNeedsBoundsCheck ||
                            (LHSTargetBounds && !LHSTargetBounds->isUnknown())))
          DumpAssignmentBounds(llvm::outs(), E, LHSTargetBounds, RightBounds);
      }

      return ResultBounds;
    }

    // CheckCallExpr returns the bounds for the value produced by e.
    // e is an rvalue.
    BoundsExpr *CheckCallExpr(CallExpr *E, CheckedScopeSpecifier CSS,
                              CheckingState &State,
                              CHKCBindTemporaryExpr *Binding = nullptr) {
      BoundsExpr *ResultBounds = CallExprBounds(E, Binding);

      QualType CalleeType = E->getCallee()->getType();
      // Extract the pointee type.  The caller type could be a regular pointer
      // type or a block pointer type.
      QualType PointeeType;
      if (const PointerType *FuncPtrTy = CalleeType->getAs<PointerType>())
        PointeeType = FuncPtrTy->getPointeeType();
      else if (const BlockPointerType *BlockPtrTy = CalleeType->getAs<BlockPointerType>())
        PointeeType = BlockPtrTy->getPointeeType();
      else {
        llvm_unreachable("Unexpected callee type");
        return CreateBoundsInferenceError();
      }

      const FunctionType *FuncTy = PointeeType->getAs<FunctionType>();
      assert(FuncTy);
      const FunctionProtoType *FuncProtoTy = FuncTy->getAs<FunctionProtoType>();

      // If the callee and arguments will not be checked during
      // the bounds declaration checking below, check them here.
      if (!FuncProtoTy) {
        CheckChildren(E, CSS, State);
        return ResultBounds;
      }
      if (!FuncProtoTy->hasParamAnnots()) {
        CheckChildren(E, CSS, State);
        return ResultBounds;
      }

      // Check the callee since CheckCallExpr should check
      // all its children.  The arguments will be checked below.
      Check(E->getCallee(), CSS, State);

      unsigned NumParams = FuncProtoTy->getNumParams();
      unsigned NumArgs = E->getNumArgs();
      unsigned Count = (NumParams < NumArgs) ? NumParams : NumArgs;
      ArrayRef<Expr *> ArgExprs = llvm::makeArrayRef(const_cast<Expr**>(E->getArgs()), E->getNumArgs());

      for (unsigned i = 0; i < Count; i++) {
        // Check each argument.
        Expr *Arg = E->getArg(i);
        BoundsExpr *ArgBounds = Check(Arg, CSS, State);

        QualType ParamType = FuncProtoTy->getParamType(i);
        // Skip checking bounds for unchecked pointer parameters, unless
        // the argument was subject to a bounds-safe interface cast.
        if (ParamType->isUncheckedPointerType() && !IsBoundsSafeInterfaceAssignment(ParamType, E->getArg(i))) {
          continue;
        }
        // We want to check the argument expression implies the desired parameter bounds.
        // To compute the desired parameter bounds, we substitute the arguments for
        // parameters in the parameter bounds expression.
        const BoundsAnnotations ParamAnnots = FuncProtoTy->getParamAnnots(i);
        const BoundsExpr *ParamBounds = ParamAnnots.getBoundsExpr();
        const InteropTypeExpr *ParamIType = ParamAnnots.getInteropTypeExpr();
        if (!ParamBounds && !ParamIType)
          continue;

        bool UsedIType = false;
        if (!ParamBounds && ParamIType) {
          ParamBounds = CreateTypeBasedBounds(nullptr, ParamIType->getType(),
                                                true, true);
          UsedIType = true;
        }

        // Check after handling the interop type annotation, not before, because
        // handling the interop type annotation could make the bounds known.
        if (ParamBounds->isUnknown())
          continue;

        ArgBounds = S.CheckNonModifyingBounds(ArgBounds, Arg);
        if (ArgBounds->isUnknown()) {
          S.Diag(Arg->getBeginLoc(),
                  diag::err_expected_bounds_for_argument) << (i + 1) <<
            Arg->getSourceRange();
          ArgBounds = S.CreateInvalidBoundsExpr();
          continue;
        } else if (ArgBounds->isInvalid())
          continue;

        // Concretize parameter bounds with argument expressions. This fails
        // and returns null if an argument expression is a modifying
        // expression,  We issue an error during concretization about that.
        BoundsExpr *SubstParamBounds =
          S.ConcretizeFromFunctionTypeWithArgs(
            const_cast<BoundsExpr *>(ParamBounds),
            ArgExprs,
            Sema::NonModifyingContext::NMC_Function_Parameter);

        if (!SubstParamBounds)
          continue;

        // Put the parameter bounds in a standard form if necessary.
        if (SubstParamBounds->isElementCount() ||
            SubstParamBounds->isByteCount()) {
          // TODO: turn this check on as part of adding temporary variables for
          // calls.
          // Turning it on now would cause errors to be issued for arguments
          // that are calls.
          if (true /* S.CheckIsNonModifying(Arg,
                              Sema::NonModifyingContext::NMC_Function_Parameter,
                                    Sema::NonModifyingMessage::NMM_Error) */) {
            Expr *TypedArg = Arg;
            // The bounds expression is for an interface type. Retype the
            // argument to the interface type.
            if (UsedIType) {
              TypedArg = CreateExplicitCast(
                ParamIType->getType(), CK_BitCast, Arg, true);
            }
            SubstParamBounds = ExpandToRange(TypedArg,
                                    const_cast<BoundsExpr *>(SubstParamBounds));
            } else
              continue;
        }

        if (DumpBounds) {
          DumpCallArgumentBounds(llvm::outs(), FuncProtoTy->getParamAnnots(i).getBoundsExpr(), Arg, SubstParamBounds, ArgBounds);
        }

        CheckBoundsDeclAtCallArg(i, SubstParamBounds, Arg, ArgBounds, CSS, nullptr);
      }

      // Check any arguments that are beyond
      // the number of function parameters.
      for (unsigned i = Count; i < NumArgs; i++) {
        Expr *Arg = E->getArg(i);
        Check(Arg, CSS, State);
      }

      // State.G is empty for call expressions.
      State.G.clear();

      return ResultBounds;
    }

    // If e is an rvalue, CheckCastExpr returns the bounds for
    // the value produced by e.
    // If e is an lvalue, it returns unknown bounds (CheckCastLValue
    // should be called instead).
    // This includes both ImplicitCastExprs and CStyleCastExprs.
    BoundsExpr *CheckCastExpr(CastExpr *E, CheckedScopeSpecifier CSS,
                              CheckingState &State) {
      // If the rvalue bounds for e cannot be determined,
      // e may be an lvalue (or may have unknown rvalue bounds).
      BoundsExpr *ResultBounds = CreateBoundsUnknown();

      Expr *SubExpr = E->getSubExpr();
      CastKind CK = E->getCastKind();

      bool IncludeNullTerm =
          E->getType()->getPointeeOrArrayElementType()->isNtCheckedArrayType();
      bool PreviousIncludeNullTerminator = IncludeNullTerminator;
      IncludeNullTerminator = IncludeNullTerm;

      // Infer the lvalue or rvalue bounds of the subexpression e1,
      // setting State to contain the results for e1.
      BoundsExpr *SubExprTargetBounds, *SubExprLValueBounds, *SubExprBounds;
      InferBounds(SubExpr, CSS, SubExprTargetBounds,
                  SubExprLValueBounds, SubExprBounds, State);

      IncludeNullTerminator = PreviousIncludeNullTerminator;

      // Update the set State.G of expressions that produce the
      // same value as e.
      if (CK == CastKind::CK_ArrayToPointerDecay) {
        // State.G = { e } for lvalues with array type.
        if (!CreatesNewObject(E) && CheckIsNonModifying(E))
          State.G = { E };
      } else if (CK == CastKind::CK_LValueToRValue) {
        if (E->getType()->isArrayType()) {
          // State.G = { e } for lvalues with array type.
          if (!CreatesNewObject(E) && CheckIsNonModifying(E))
            State.G = { E };
        }
        else {
          // If e appears in some set F in State.UEQ, State.G = F.
          State.G = GetEqualExprSetContainingExpr(E, State.UEQ);
          if (State.G.size() == 0) {
            // Otherwise, if e is nonmodifying and does not read memory
            // via a pointer, State.G = { e }.  Otherwise, State.G is empty.
            if (CheckIsNonModifying(E) && !ReadsMemoryViaPointer(E) &&
                !CreatesNewObject(E))
              State.G.push_back(E);
          }
        }
      } else
        // Use the default rules to update State.G for e using
        // the current State.G for the subexpression e1.
        UpdateG(E, State.G, State.G);

      // Casts to _Ptr narrow the bounds.  If the cast to
      // _Ptr is invalid, that will be diagnosed separately.
      if (E->getStmtClass() == Stmt::ImplicitCastExprClass ||
          E->getStmtClass() == Stmt::CStyleCastExprClass) {
        if (E->getType()->isCheckedPointerPtrType())
          ResultBounds = CreateTypeBasedBounds(E, E->getType(), false, false);
        else
          ResultBounds = RValueCastBounds(E, SubExprTargetBounds,
                                          SubExprLValueBounds,
                                          SubExprBounds, State);
      }

      CheckDisallowedFunctionPtrCasts(E);

      if (CK == CK_LValueToRValue && !E->getType()->isArrayType()) {
        bool NeedsBoundsCheck = AddBoundsCheck(SubExpr, OperationKind::Read,
                                               CSS, SubExprLValueBounds);
        if (NeedsBoundsCheck && DumpBounds)
          DumpExpression(llvm::outs(), E);
        return ResultBounds;
      }

      // Handle dynamic_bounds_casts.
      //
      // If the inferred bounds of the subexpression are:
      // - bounds(unknown), this is a compile-time error.
      // - bounds(any), there is no runtime checks.
      // - bounds(lb, ub):  If the declared bounds of the cast operation are
      // (e2, e3),  a runtime check that lb <= e2 && e3 <= ub is inserted
      // during code generation.
      if (CK == CK_DynamicPtrBounds || CK == CK_AssumePtrBounds) {
        CHKCBindTemporaryExpr *TempExpr = dyn_cast<CHKCBindTemporaryExpr>(SubExpr);
        assert(TempExpr);

        // These bounds may be computed and tested at runtime.  Don't
        // recompute any expressions computed to temporaries already.
        Expr *TempUse = CreateTemporaryUse(TempExpr);

        Expr *SubExprAtNewType = CreateExplicitCast(E->getType(),
                                                CastKind::CK_BitCast,
                                                TempUse, true);

        if (CK == CK_AssumePtrBounds)
          return ExpandToRange(SubExprAtNewType, E->getBoundsExpr());

        BoundsExpr *DeclaredBounds = E->getBoundsExpr();
        BoundsExpr *NormalizedBounds = ExpandToRange(SubExprAtNewType,
                                                      DeclaredBounds);

        SubExprBounds = S.CheckNonModifyingBounds(SubExprBounds, SubExpr);
        if (SubExprBounds->isUnknown()) {
          S.Diag(SubExpr->getBeginLoc(), diag::err_expected_bounds);
        }

        assert(NormalizedBounds);

        E->setNormalizedBoundsExpr(NormalizedBounds);
        E->setSubExprBoundsExpr(SubExprBounds);

        if (DumpBounds)
          DumpBoundsCastBounds(llvm::outs(), E, DeclaredBounds, NormalizedBounds, SubExprBounds);
        
        return ExpandToRange(SubExprAtNewType, E->getBoundsExpr());
      }

      // Casts to _Ptr type must have a source for which we can infer bounds.
      if ((CK == CK_BitCast || CK == CK_IntegralToPointer) &&
          E->getType()->isCheckedPointerPtrType() &&
          !E->getType()->isFunctionPointerType()) {
        SubExprBounds = S.CheckNonModifyingBounds(SubExprBounds, SubExpr);
        if (SubExprBounds->isUnknown()) {
          S.Diag(SubExpr->getBeginLoc(),
                  diag::err_expected_bounds_for_ptr_cast)
                  << SubExpr->getSourceRange();
          SubExprBounds = S.CreateInvalidBoundsExpr();
        } else {
          BoundsExpr *TargetBounds =
            CreateTypeBasedBounds(E, E->getType(), false, false);
          CheckBoundsDeclAtStaticPtrCast(E, TargetBounds, SubExpr,
                                         SubExprBounds, CSS);
        }
        assert(SubExprBounds);
        assert(!E->getSubExprBoundsExpr());
        E->setSubExprBoundsExpr(SubExprBounds);
        if (DumpBounds)
          DumpExpression(llvm::outs(), E);
      }

      return ResultBounds;
    }

    // If e is an rvalue, CheckUnaryOperator returns the bounds for
    // the value produced by e.
    // If e is an lvalue, CheckUnaryLValue should be called instead.
    BoundsExpr *CheckUnaryOperator(UnaryOperator *E, CheckedScopeSpecifier CSS,
                                   CheckingState &State) {
      UnaryOperatorKind Op = E->getOpcode();
      Expr *SubExpr = E->getSubExpr();

      // Infer the lvalue or rvalue bounds of the subexpression e1,
      // setting State to contain the results for e1.
      BoundsExpr *SubExprTargetBounds, *SubExprLValueBounds, *SubExprBounds;
      InferBounds(SubExpr, CSS, SubExprTargetBounds,
                  SubExprLValueBounds, SubExprBounds, State);

      if (Op == UO_AddrOf)
        S.CheckAddressTakenMembers(E);

      if (E->isIncrementDecrementOp()) {
        bool NeedsBoundsCheck = AddBoundsCheck(SubExpr, OperationKind::Other,
                                               CSS, SubExprLValueBounds);
        if (NeedsBoundsCheck && DumpBounds)
          DumpExpression(llvm::outs(), E);
      }

      // `*e` is not an rvalue.
      if (Op == UnaryOperatorKind::UO_Deref)
        return CreateBoundsInferenceError();

      // Update UEQ and G for inc/dec operators `++e1`, `e1++`, `--e1`, `e1--`.
      // At this point, State contains UEQ and G for `e1`.
      if (UnaryOperator::isIncrementDecrementOp(Op)) {
        // Create the target of the implied assignment `e1 = e1 +/- 1`.
        Expr *Target = CreateImplicitCast(SubExpr->getType(),
                                            CK_LValueToRValue, SubExpr);

        // Only use the RHS `e1 +/1 ` of the implied assignment to update
        // UEQ and G if the integer constant 1 can be created, which is
        // only true if `e1` has integer type or integer pointer type.
        IntegerLiteral *One = CreateIntegerLiteral(1, SubExpr->getType());
        Expr *RHS = nullptr;
        if (One) {
          BinaryOperatorKind RHSOp = UnaryOperator::isIncrementOp(Op) ?
                                      BinaryOperatorKind::BO_Add :
                                      BinaryOperatorKind::BO_Sub;
          RHS = ExprCreatorUtil::CreateBinaryOperator(S, SubExpr, One, RHSOp);
        }

        // Update UEQ for inc/dec operators where `e1` is a variable.  Any
        // expressions in UEQ that use the value of `e1` need to be adjusted
        // using the original value of `e1`, since `e1` has been updated.
        if (DeclRefExpr *V = GetLValueVariable(SubExpr)) {
          // Update G to be the set of expressions that produce the same
          // value as the RHS `e1 +/- 1` (if the RHS could be created).
          UpdateG(E, State.G, State.G, RHS);
          bool OVUsesV = false;
          Expr *OV = GetOriginalValue(V, Target, RHS, State.UEQ, OVUsesV);
          UpdateAfterAssignment(V, Target, OV, OVUsesV, CSS, State, State);
        }

        // Update the set G of expressions that produce the same value as `e`.
        if (One) {
          // For integer or integer pointer-typed expressions, create the
          // expression Val that is equivalent to `e` in the program state
          // after the increment/decrement expression `e` has executed.
          // (The call to UpdateG will only add Val to G if Val is a
          // non-modifying expression).

          // `++e1` and `--e1` produce the same value as the rvalue cast of
          // `e1` after executing `++e1` or `--e1`.
          Expr *Val = Target;
          // `e1++` produces the same value as `e1 - 1` after executing `e1++`.
          if (Op == UnaryOperatorKind::UO_PostInc)
            Val = ExprCreatorUtil::CreateBinaryOperator(S, SubExpr, One,
                                    BinaryOperatorKind::BO_Sub);
          // `e1--` produces the same value as `e1 + 1` after executing `e1--`.
          else if (Op == UnaryOperatorKind::UO_PostDec)
            Val = ExprCreatorUtil::CreateBinaryOperator(S, SubExpr, One,
                                    BinaryOperatorKind::BO_Add);
          UpdateG(E, State.G, State.G, Val);
        } else {
          // G is empty for expressions where the integer constant 1
          // could not be constructed (e.g. floating point expressions).
          State.G.clear();
        }
      }

      // `&e` has the bounds of `e`.
      // `e` is an lvalue, so its bounds are its lvalue bounds.
      // State.G for `&e` remains the same as State.G for `e`.
      if (Op == UnaryOperatorKind::UO_AddrOf) {

        // Functions have bounds corresponding to the empty range.
        if (SubExpr->getType()->isFunctionType())
          return CreateBoundsEmpty();

        return SubExprLValueBounds;
      }

      // `++e`, `e++`, `--e`, `e--` all have bounds of `e`.
      // `e` is an lvalue, so its bounds are its lvalue target bounds.
      if (UnaryOperator::isIncrementDecrementOp(Op))
        return SubExprTargetBounds;

      // Update State.G for `!e`, `+e`, `-e`, and `~e`
      // using the current State.G for `e`.
      UpdateG(E, State.G, State.G);

      // `!e` has empty bounds.
      if (Op == UnaryOperatorKind::UO_LNot)
        return CreateBoundsEmpty();

      // `+e`, `-e`, `~e` all have bounds of `e`. `e` is an rvalue.
      if (Op == UnaryOperatorKind::UO_Plus ||
          Op == UnaryOperatorKind::UO_Minus ||
          Op == UnaryOperatorKind::UO_Not)
        return SubExprBounds;

      // We cannot infer the bounds of other unary operators.
      return CreateBoundsAlwaysUnknown();
    }

    // CheckVarDecl returns empty bounds.
    BoundsExpr *CheckVarDecl(VarDecl *D, CheckedScopeSpecifier CSS,
                             CheckingState &State) {
      BoundsExpr *ResultBounds = CreateBoundsEmpty();

      Expr *Init = D->getInit();
      BoundsExpr *InitBounds = nullptr;
      // If there is an initializer, check it, and update the state to record
      // expression equality implied by initialization. After checking Init,
      // State.G will contain non-modifying expressions that produce values
      // equivalent to the value produced by Init.
      if (Init) {
        InitBounds = Check(Init, CSS, State);

        // Create an rvalue expression for v. v could be an array or
        // non-array variable.
        DeclRefExpr *TargetDeclRef =
          DeclRefExpr::Create(S.getASTContext(), NestedNameSpecifierLoc(),
                              SourceLocation(), D, false, SourceLocation(),
                              D->getType(), ExprValueKind::VK_LValue);
        CastKind Kind;
        QualType TargetTy;
        if (D->getType()->isArrayType()) {
          Kind = CK_ArrayToPointerDecay;
          TargetTy = S.getASTContext().getArrayDecayedType(D->getType());
        } else {
          Kind = CK_LValueToRValue;
          TargetTy = D->getType();
        }
        Expr *TargetExpr = CreateImplicitCast(TargetTy, Kind, TargetDeclRef);
        
        // Record equality between the target and initializer.
        RecordEqualityWithTarget(TargetExpr, State);
      }

      if (D->isInvalidDecl())
        return ResultBounds;

      if (isa<ParmVarDecl>(D))
        return ResultBounds;

      VarDecl::DefinitionKind defKind = D->isThisDeclarationADefinition();
      if (defKind == VarDecl::DefinitionKind::DeclarationOnly)
        return ResultBounds;

      // Handle variables with bounds declarations
      BoundsExpr *DeclaredBounds = D->getBoundsExpr();
      if (!DeclaredBounds || DeclaredBounds->isInvalid() ||
          DeclaredBounds->isUnknown())
        return ResultBounds;

      // TODO: for array types, check that any declared bounds at the point
      // of initialization are true based on the array size.

      // If there is a scalar initializer, check that the initializer meets the bounds
      // requirements for the variable.  For non-scalar types (arrays, structs, and
      // unions), the amount of storage allocated depends on the type, so we don't
      // to check the initializer bounds.
      if (Init && D->getType()->isScalarType()) {
        assert(D->getInitStyle() == VarDecl::InitializationStyle::CInit);
        InitBounds = S.CheckNonModifyingBounds(InitBounds, Init);
        if (InitBounds->isUnknown()) {
          // TODO: need some place to record the initializer bounds
          S.Diag(Init->getBeginLoc(), diag::err_expected_bounds_for_initializer)
              << Init->getSourceRange();
          InitBounds = S.CreateInvalidBoundsExpr();
        } else {
          BoundsExpr *NormalizedDeclaredBounds = ExpandToRange(D, DeclaredBounds);
          CheckBoundsDeclAtInitializer(D->getLocation(), D, NormalizedDeclaredBounds,
            Init, InitBounds, CSS);
        }
        if (DumpBounds)
          DumpInitializerBounds(llvm::outs(), D, DeclaredBounds, InitBounds);
      }

      return ResultBounds;
    }

    // CheckReturnStmt returns empty bounds.
    BoundsExpr *CheckReturnStmt(ReturnStmt *RS, CheckedScopeSpecifier CSS,
                                CheckingState &State) {
      BoundsExpr *ResultBounds = CreateBoundsEmpty();

      Expr *RetValue = RS->getRetValue();

      if (!RetValue)
        // We already issued an error message for this case.
        return ResultBounds;

      // Check the return value if it exists.
      Check(RetValue, CSS, State);

      if (!ReturnBounds)
        return ResultBounds;

      // TODO: Actually check that the return expression bounds imply the 
      // return bounds.
      // TODO: Also check that any parameters used in the return bounds are
      // unmodified.
      return ResultBounds;
    }

    // If e is an rvalue, CheckTemporaryBinding returns the bounds for
    // the value produced by e.
    // If e is an lvalue, CheckTempBindingLValue should be called instead.
    BoundsExpr *CheckTemporaryBinding(CHKCBindTemporaryExpr *E,
                                      CheckedScopeSpecifier CSS,
                                      CheckingState &State) {
      Expr *Child = E->getSubExpr();

      BoundsExpr *SubExprBounds = nullptr;
      if (CallExpr *CE = dyn_cast<CallExpr>(Child))
        SubExprBounds = CheckCallExpr(CE, CSS, State, E);
      else
        SubExprBounds = Check(Child, CSS, State);

      UpdateG(E, State.G, State.G);
      return SubExprBounds;
    }

    // CheckBoundsValueExpr returns the bounds for the value produced by e.
    // e is an rvalue.
    BoundsExpr *CheckBoundsValueExpr(BoundsValueExpr *E,
                                     CheckedScopeSpecifier CSS,
                                     CheckingState &State) {
      Expr *Binding = E->getTemporaryBinding();
      return Check(Binding, CSS, State);
    }

    // CheckConditionalOperator returns the bounds for the value produced by e.
    // e is an rvalue.
    BoundsExpr *CheckConditionalOperator(AbstractConditionalOperator *E,
                                         CheckedScopeSpecifier CSS,
                                         CheckingState &State) {
      CheckChildren(E, CSS, State);
      // TODO: infer correct bounds for conditional operators
      return CreateBoundsAllowedButNotComputed();
    }

  // Methods to infer both:
  // 1. Bounds for an expression that produces an lvalue, and
  // 2. Bounds for the target of an expression that produces an lvalue.

  private:

    // CheckDeclRefExpr returns the lvalue and target bounds of e.
    // e is an lvalue.
    BoundsExpr *CheckDeclRefExpr(DeclRefExpr *E, CheckedScopeSpecifier CSS,
                                 BoundsExpr *&OutTargetBounds,
                                 CheckingState &State) {
      CheckChildren(E, CSS, State);
      State.G.clear();

      VarDecl *VD = dyn_cast<VarDecl>(E->getDecl());
      BoundsExpr *B = nullptr;
      InteropTypeExpr *IT = nullptr;
      if (VD) {
        B = VD->getBoundsExpr();
        IT = VD->getInteropTypeExpr();
      }

      if (E->getType()->isArrayType()) {
        // Variables with array type do not have target bounds.
        OutTargetBounds = CreateBoundsAlwaysUnknown();

        if (!VD) {
          llvm_unreachable("declref with array type not a vardecl");
          return CreateBoundsInferenceError();
        }

        // Update G for variables with array type.
        const ConstantArrayType *CAT = Context.getAsConstantArrayType(E->getType());
        if (CAT) {
          if (E->getType()->isCheckedArrayType())
            State.G.push_back(E);
          else if (VD->hasLocalStorage() || VD->hasExternalStorage())
            State.G.push_back(E);
        }

        // Declared bounds override the bounds based on the array type.
        if (B) {
          Expr *Base = CreateImplicitCast(Context.getDecayedType(E->getType()),
                                          CastKind::CK_ArrayToPointerDecay, E);
          return ExpandToRange(Base, B);
        }

        // If B is an interop type annotation, the type must be identical
        // to the declared type, modulo checkedness.  So it is OK to
        // compute the array bounds based on the original type.
        return ArrayExprBounds(E);
      }

      // Infer the target bounds of e.
      // e only has target bounds if e does not have array type.
      bool IsParam = isa<ParmVarDecl>(E->getDecl());
      if (E->getType()->isCheckedPointerPtrType())
        OutTargetBounds = CreateTypeBasedBounds(E, E->getType(),
                                                IsParam, false);
      else if (!VD)
        OutTargetBounds = CreateBoundsInferenceError();
      else if (!B && IT)
        OutTargetBounds = CreateTypeBasedBounds(E, IT->getType(),
                                                IsParam, true);
      else if (!B || B->isUnknown())
        OutTargetBounds = CreateBoundsAlwaysUnknown();
      else {
        Expr *Base = CreateImplicitCast(E->getType(),
                                        CastKind::CK_LValueToRValue, E);
        OutTargetBounds = ExpandToRange(Base, B);
      }

      if (E->getType()->isFunctionType()) {
        // Only function decl refs should have function type.
        assert(isa<FunctionDecl>(E->getDecl()));
        return CreateBoundsEmpty();
      }

      Expr *AddrOf = CreateAddressOfOperator(E);
      // G is { &v } for variables v that do not have array type.
      State.G.push_back(AddrOf);
      return CreateSingleElementBounds(AddrOf);
    }

    // If e is an lvalue, CheckUnaryLValue returns the
    // lvalue and target bounds of e.
    // If e is an rvalue, CheckUnaryOperator should be called instead.
    BoundsExpr *CheckUnaryLValue(UnaryOperator *E, CheckedScopeSpecifier CSS,
                                 BoundsExpr *&OutTargetBounds,
                                 CheckingState &State) {
      BoundsExpr *SubExprBounds = Check(E->getSubExpr(), CSS, State);

      if (E->getOpcode() == UnaryOperatorKind::UO_Deref) {
        // Currently, we don't know the target bounds of a pointer stored in a
        // pointer dereference, unless it is a _Ptr type or an _Nt_array_ptr.
        if (E->getType()->isCheckedPointerPtrType() ||
            E->getType()->isCheckedPointerNtArrayType())
          OutTargetBounds = CreateTypeBasedBounds(E, E->getType(),
                                                  false, false);
        else
          OutTargetBounds = CreateBoundsUnknown();

        // G is empty for pointer dereferences.
        State.G.clear();

        // The lvalue bounds of *e are the rvalue bounds of e.
        return SubExprBounds;
      }

      OutTargetBounds = CreateBoundsInferenceError();
      return CreateBoundsInferenceError();
    }

    // CheckArraySubscriptExpr returns the lvalue and target bounds of e.
    // e is an lvalue.
    BoundsExpr *CheckArraySubscriptExpr(ArraySubscriptExpr *E,
                                        CheckedScopeSpecifier CSS,
                                        BoundsExpr *&OutTargetBounds,
                                        CheckingState &State) {
      // Currently, we don't know the target bounds of a pointer returned by a
      // subscripting operation, unless it is a _Ptr type or an _Nt_array_ptr.
      if (E->getType()->isCheckedPointerPtrType() ||
          E->getType()->isCheckedPointerNtArrayType())
        OutTargetBounds = CreateTypeBasedBounds(E, E->getType(), false, false);
      else
        OutTargetBounds = CreateBoundsAlwaysUnknown();

      // e1[e2] is a synonym for *(e1 + e2).  The bounds are
      // the bounds of e1 + e2, which reduces to the bounds
      // of whichever subexpression has pointer type.
      // getBase returns the pointer-typed expression.
      BoundsExpr *Bounds = Check(E->getBase(), CSS, State);
      Check(E->getIdx(), CSS, State);

      // G is empty for array subscript expressions.
      State.G.clear();

      return Bounds;
    }

    // CheckMemberExpr returns the lvalue and target bounds of e.
    // e is an lvalue.
    //
    // A member expression is a narrowing operator that shrinks the range of
    // memory to which the base refers to a specific member.  We always bounds
    // check the base.  That way we know that the lvalue produced by the
    // member points to a valid range of memory given by
    // (lvalue, lvalue + 1).   The lvalue is interpreted as a pointer to T,
    // where T is the type of the member.
    BoundsExpr *CheckMemberExpr(MemberExpr *E, CheckedScopeSpecifier CSS,
                                BoundsExpr *&OutTargetBounds,
                                CheckingState &State) {
      // The lvalue and target bounds must be inferred before
      // performing any side effects on the base, since
      // inferring these bounds may call PruneTemporaryBindings.
      OutTargetBounds = MemberExprTargetBounds(E, CSS);
      BoundsExpr *Bounds = MemberExprBounds(E, CSS);

      // Infer the lvalue or rvalue bounds of the base.
      Expr *Base = E->getBase();
      BoundsExpr *BaseTargetBounds, *BaseLValueBounds, *BaseBounds;
      InferBounds(Base, CSS, BaseTargetBounds,
                  BaseLValueBounds, BaseBounds, State);

      // Clear State.G to avoid adding false equality information.
      // TODO: implement updating state for member expressions.
      State.G.clear();

      bool NeedsBoundsCheck = AddMemberBaseBoundsCheck(E, CSS,
                                                       BaseLValueBounds,
                                                       BaseBounds);
      if (NeedsBoundsCheck && DumpBounds)
        DumpExpression(llvm::outs(), E);
      return Bounds;
    }

    // If e is an lvalue, CheckCastLValue returns the
    // lvalue and target bounds of e.
    // If e is an rvalue, CheckCastExpr should be called instead.
    BoundsExpr *CheckCastLValue(CastExpr *E, CheckedScopeSpecifier CSS,
                                BoundsExpr *&OutTargetBounds,
                                CheckingState &State) {
      // An LValueBitCast adjusts the type of the lvalue.  The bounds are not
      // changed, except that their relative alignment may change (the bounds 
      // may only cover a partial object).  TODO: When we add relative
      // alignment support to the compiler, adjust the relative alignment.
      if (E->getCastKind() == CastKind::CK_LValueBitCast)
        return CheckLValue(E->getSubExpr(), CSS, OutTargetBounds, State);

      CheckChildren(E, CSS, State);

      // Cast kinds other than LValueBitCast
      // do not have lvalue or target bounds.
      OutTargetBounds = CreateBoundsAlwaysUnknown();
      return CreateBoundsAlwaysUnknown();
    }

    // If e is an lvalue, CheckTempBindingLValue returns the
    // lvalue and target bounds of e.
    // If e is an rvalue, CheckTemporaryBinding should be called instead.
    BoundsExpr *CheckTempBindingLValue(CHKCBindTemporaryExpr *E,
                                       CheckedScopeSpecifier CSS,
                                       BoundsExpr *&OutTargetBounds,
                                       CheckingState &State) {
      OutTargetBounds = CreateBoundsAlwaysUnknown();

      CheckChildren(E, CSS, State);

      Expr *SubExpr = E->getSubExpr()->IgnoreParens();

      if (isa<CompoundLiteralExpr>(SubExpr)) {
        BoundsExpr *BE = CreateBoundsForArrayType(E->getType());
        QualType PtrType = Context.getDecayedType(E->getType());
        Expr *ArrLValue = CreateTemporaryUse(E);
        Expr *Base = CreateImplicitCast(PtrType,
                                        CastKind::CK_ArrayToPointerDecay,
                                        ArrLValue);
        return ExpandToRange(Base, BE);
      }

      if (auto *SL = dyn_cast<StringLiteral>(SubExpr))
        return InferBoundsForStringLiteral(E, SL, E);

      if (auto *PE = dyn_cast<PredefinedExpr>(SubExpr)) {
        auto *SL = PE->getFunctionName();
        return InferBoundsForStringLiteral(E, SL, E);
      }

      return CreateBoundsAlwaysUnknown();
    }

  public:

    // Given an array type with constant dimension size, produce a count
    // expression with that size.
    BoundsExpr *CreateBoundsForArrayType(QualType QT) {
      const IncompleteArrayType *IAT = Context.getAsIncompleteArrayType(QT);
      if (IAT) {
        if (IAT->getKind() == CheckedArrayKind::NtChecked)
          return Context.getPrebuiltCountZero();
        else
          return CreateBoundsAlwaysUnknown();
      }
      const ConstantArrayType *CAT = Context.getAsConstantArrayType(QT);
      if (!CAT)
        return CreateBoundsAlwaysUnknown();

      llvm::APInt size = CAT->getSize();
      // Null-terminated arrays of size n have bounds of count(n - 1).
      // The null terminator is excluded from the count.
      if (!IncludeNullTerminator &&
          CAT->getKind() == CheckedArrayKind::NtChecked) {
        assert(size.uge(1) && "must have at least one element");
        size = size - 1;
      }
      IntegerLiteral *Size = CreateIntegerLiteral(size);
      CountBoundsExpr *CBE =
          new (Context) CountBoundsExpr(BoundsExpr::Kind::ElementCount,
                                        Size, SourceLocation(),
                                        SourceLocation());
      return CBE;
    }

    Expr *CreateExplicitCast(QualType Target, CastKind CK, Expr *E,
                               bool isBoundsSafeInterface) {
      // Avoid building up nested chains of no-op casts.
      E = BoundsUtil::IgnoreRedundantCast(Context, CK, E);

      // Synthesize some dummy type source source information.
      TypeSourceInfo *DI = Context.getTrivialTypeSourceInfo(Target);
      CStyleCastExpr *CE = CStyleCastExpr::Create(Context, Target,
        ExprValueKind::VK_RValue, CK, E, nullptr, DI, SourceLocation(),
        SourceLocation());
      CE->setBoundsSafeInterface(isBoundsSafeInterface);
      return CE;
    }

    ImplicitCastExpr *CreateImplicitCast(QualType Target, CastKind CK,
                                         Expr *E) {
      return ImplicitCastExpr::Create(Context, Target, CK, E, nullptr,
                                       ExprValueKind::VK_RValue);
    }

    // Given a byte_count or count bounds expression for the expression Base,
    // expand it to a range bounds expression:
    //  E : Count(C) expands to Bounds(E, E + C)
    //  E : ByteCount(C)  expands to Bounds((array_ptr<char>) E,
    //                                      (array_ptr<char>) E + C)
    BoundsExpr *ExpandToRange(Expr *Base, BoundsExpr *B) {
      assert(Base->isRValue() && "expected rvalue expression");
      BoundsExpr::Kind K = B->getKind();
      switch (K) {
        case BoundsExpr::Kind::ByteCount:
        case BoundsExpr::Kind::ElementCount: {
          CountBoundsExpr *BC = dyn_cast<CountBoundsExpr>(B);
          if (!BC) {
            llvm_unreachable("unexpected cast failure");
            return CreateBoundsInferenceError();
          }
          Expr *Count = BC->getCountExpr();
          QualType ResultTy;
          Expr *LowerBound;
          Base = S.MakeAssignmentImplicitCastExplicit(Base);
          if (K == BoundsExpr::ByteCount) {
            ResultTy = Context.getPointerType(Context.CharTy,
                                              CheckedPointerKind::Array);
            // When bounds are pretty-printed as source code, the cast needs
            // to appear in the source code for the code to be correct, so
            // use an explicit cast operation.
            //
            // The bounds-safe interface argument is false because casts
            // to checked pointer types are always allowed by type checking.
            LowerBound =
              CreateExplicitCast(ResultTy, CastKind::CK_BitCast, Base, false);
          } else {
            ResultTy = Base->getType();
            LowerBound = Base;
            if (ResultTy->isCheckedPointerPtrType()) {
              ResultTy = Context.getPointerType(ResultTy->getPointeeType(),
                CheckedPointerKind::Array);
              // The bounds-safe interface argument is false because casts
              // between checked pointer types are always allowed by type
              // checking.
              LowerBound =
                CreateExplicitCast(ResultTy, CastKind::CK_BitCast, Base, false);
            }
          }
          Expr *UpperBound =
            new (Context) BinaryOperator(LowerBound, Count,
                                          BinaryOperatorKind::BO_Add,
                                          ResultTy,
                                          ExprValueKind::VK_RValue,
                                          ExprObjectKind::OK_Ordinary,
                                          SourceLocation(),
                                          FPOptions());
          RangeBoundsExpr *R = new (Context) RangeBoundsExpr(LowerBound, UpperBound,
                                               SourceLocation(),
                                               SourceLocation());
          return R;
        }
        default:
          return B;
      }
    }

    BoundsExpr *ExpandToRange(VarDecl *D, BoundsExpr *B) {
      QualType QT = D->getType();
      ExprResult ER = S.BuildDeclRefExpr(D, QT,
                                       clang::ExprValueKind::VK_LValue, SourceLocation());
      if (ER.isInvalid())
        return nullptr;
      Expr *Base = ER.get();
      if (!QT->isArrayType())
        Base = CreateImplicitCast(QT, CastKind::CK_LValueToRValue, Base);
      return ExpandToRange(Base, B);
    }

    // Compute bounds for a variable expression or member reference expression
    // with an array type.
    BoundsExpr *ArrayExprBounds(Expr *E) {
      DeclRefExpr *DR = dyn_cast<DeclRefExpr>(E);
      assert((DR && dyn_cast<VarDecl>(DR->getDecl())) || isa<MemberExpr>(E));
      BoundsExpr *BE = CreateBoundsForArrayType(E->getType());
      if (BE->isUnknown())
        return BE;

      Expr *Base = CreateImplicitCast(Context.getDecayedType(E->getType()),
                                      CastKind::CK_ArrayToPointerDecay,
                                      E);
      return ExpandToRange(Base, BE);
    }

    BoundsAnalysis getBoundsAnalyzer() { return BoundsAnalyzer; }

  private:
    // Sets the bounds expressions based on
    // whether e is an lvalue or an rvalue.
    void InferBounds(Expr *E, CheckedScopeSpecifier CSS,
                     BoundsExpr *&TargetBounds, BoundsExpr *&LValueBounds,
                     BoundsExpr *&RValueBounds, CheckingState &State) {
      TargetBounds = CreateBoundsUnknown();
      LValueBounds = CreateBoundsUnknown();
      RValueBounds = CreateBoundsUnknown();
      if (E->isLValue())
        LValueBounds = CheckLValue(E, CSS, TargetBounds, State);
      else if (E->isRValue())
        RValueBounds = Check(E, CSS, State);
    }

    // Methods to update sets of equivalent expressions.

    // UpdateAfterAssignment updates the checking state after a variable V
    // is assigned to, based on the state before the assignment.
    //
    // Target is the target expression of the assignment (that accounts for
    // any necessary casts of V).
    //
    // OV is the original value (if any) for V before the assignment.
    // If OV is non-null, it is substituted for any uses of the value of V
    // in the expressions in UEQ and G.
    // If OV is null, any expressions in UEQ and G that use the value of V
    // are removed from UEQ and G.
    //
    // OVUsesV is true if the original value (if any) uses the value of V.
    // It is used to prevent the UEQ and G sets from recording equality
    // between two mathematically equivalent expressions, which can occur
    // for assignments where the variable appears on the right-hand side,
    // e.g. i = i + 2.
    //
    // PrevState is the checking state that was true before the assignment.
    void UpdateAfterAssignment(DeclRefExpr *V, Expr *Target,
                               Expr *OV, bool OVUsesV,
                               CheckedScopeSpecifier CSS,
                               const CheckingState PrevState,
                               CheckingState &State) {
      // Adjust UEQ to account for any uses of V in PrevState.UEQ.
      State.UEQ.clear();
      for (auto I = PrevState.UEQ.begin(); I != PrevState.UEQ.end(); ++I) {
        EqualExprTy ExprList;
        for (auto InnerList = (*I).begin(); InnerList != (*I).end(); ++InnerList) {
          Expr *E = *InnerList;
          Expr *AdjustedE = ReplaceVariableReferences(S, E, V, OV, CSS);
          // Don't add duplicate expressions to any set in UEQ.
          if (AdjustedE && !EqualExprsContainsExpr(ExprList, AdjustedE))
            ExprList.push_back(AdjustedE);
        }
        if (ExprList.size() > 1)
          State.UEQ.push_back(ExprList);
      }

      // Adjust G to account for any uses of V in PrevState.G.
      // If the original value uses the value of V, then any expressions that
      // use the value of V should be removed from G.  For example, in the
      // assignment i = i + 2, where the original value is i - 2, the
      // expression i + 2 in G should be removed rather than replaced with
      // (i - 2) + 2.  Otherwise, G would contain (i - 2) + 2 and i, and UEQ
      // would record equality between (i - 2) + 2 and i, which is a tautology.
      State.G.clear();
      Expr *OriginalGVal = OVUsesV ? nullptr : OV;
      for (auto I = PrevState.G.begin(); I != PrevState.G.end(); ++I) {
        Expr *E = *I;
        Expr *AdjustedE = ReplaceVariableReferences(S, E, V, OriginalGVal, CSS);
        // Don't add duplicate expressions to G.
        if (AdjustedE && !EqualExprsContainsExpr(State.G, AdjustedE))
          State.G.push_back(AdjustedE);
      }

      RecordEqualityWithTarget(Target, State);
    }

    // RecordEqualityWithTarget updates the checking state to record equality
    // between the target expression of an assignment and the source of the
    // assignment.
    //
    // State.G is assumed to contain expressions that produce the same value
    // as the source of the assignment.
    void RecordEqualityWithTarget(Expr *Target, CheckingState &State) {
      // If UEQ contains a set F of expressions that produce the same value
      // as the source, add the target to F.  This prevents UEQ from growing
      // too large and containing redundant equality information.  For example,
      // for the assignments x = 1; y = x; where the target is y, G = { 1, x },
      // and UEQ contains F = { 1, x }, UEQ should contain { 1, x, y } rather
      // than { 1, x } and { 1, x, y }.
      if (State.G.size() > 0) {
        for (auto I = State.UEQ.begin(); I != State.UEQ.end(); ++I) {
          if (IsEqualExprsSubset(State.G, *I)) {
            I->push_back(Target);
            // Add the target to G if G does not already contain the target.
            if (!EqualExprsContainsExpr(State.G, Target))
              State.G.push_back(Target);
            return;
          }
        }
      }

      // Avoid adding sets with duplicate expressions such as { e, e }
      // and singleton sets such as { e } to UEQ.
      if (!EqualExprsContainsExpr(State.G, Target))
        State.G.push_back(Target);
      if (State.G.size() > 1)
        State.UEQ.push_back(State.G);
    }

    // UpdateG updates the set G of expressions that produce
    // the same value as e.
    // e is an expression with exactly one subexpression.
    //
    // SubExprG is the set of expressions that produce the same
    // value as the only subexpression of e.
    //
    // Val is an optional expression that may be contained in the updated G.
    // If Val is not provided, e is used instead.  If Val and e are null,
    // G is not updated.
    void UpdateG(Expr *E, const EqualExprTy SubExprG,
                 EqualExprTy &G, Expr *Val = nullptr) {
      Expr *SubExpr = dyn_cast<Expr>(*(E->child_begin()));
      assert(SubExpr);
      ExprEqualMapTy SubExprGs;
      SubExprGs[SubExpr] = SubExprG;
      UpdateG(E, SubExprGs, G, Val);
    }

    // UpdateG updates the set G of expressions that produce
    // the same value as e.
    // e is an expression with n subexpressions, where n >= 0.
    //
    // Some kinds of expressions (e.g. assignments) have
    // their own rules for how to update the set G.
    // UpdateG is used to update the set G for expressions
    // that do not have their own defined rules for updating G.
    //
    // SubExprGs stores, for each subexpression Si of e, a set Gi
    // of expressions that produce the same value as Si.
    //
    // Val is an optional expression that may be contained in the updated G.
    // If Val is not provided, e is used instead.  If Val and e are null,
    // G is not updated.
    void UpdateG(Expr *E, ExprEqualMapTy SubExprGs,
                 EqualExprTy &G, Expr *Val = nullptr) {
      G.clear();

      if (!Val) Val = E;
      if (!Val)
        return;

      // Expressions that create new objects should not be included in G.
      if (CreatesNewObject(Val))
        return;

      // If Val is a call expression, G does not contain Val.
      if (isa<CallExpr>(Val)) {
      }

      // If Val is a non-modifying expression, G contains Val.
      else if (CheckIsNonModifying(Val))
        G.push_back(Val);

      // If Val is a modifying expression, use the G_i sets of expressions
      // that produce the same value as the subexpressions of e to try to
      // construct a non-modifying expression ValPrime that produces the same
      // value as Val.
      else {
        Expr *ValPrime = nullptr;
        for (llvm::detail::DenseMapPair<Expr *, EqualExprTy> Pair : SubExprGs) {
          Expr *SubExpr_i = Pair.first;
          // For any modifying subexpression SubExpr_i of e, try to set
          // ValPrime to a nonmodifying expression from the set G_i of
          // expressions that produce the same value as SubExpr_i.
          if (!CheckIsNonModifying(SubExpr_i)) {
            EqualExprTy G_i = Pair.second;
            for (auto I = G_i.begin(); I != G_i.end(); ++I) {
              Expr *E_i = *I;
              if (CheckIsNonModifying(E_i)) {
                ValPrime = E_i;
                break;
              }
            }
          }
        }

        if (ValPrime)
          G.push_back(ValPrime);
      }

      // If Val introduces a temporary to hold the value produced by e,
      // add the value of the temporary to G.
      if (CHKCBindTemporaryExpr *Temp = GetTempBinding(Val))
        G.push_back(CreateTemporaryUse(Temp));
    }

    // Methods to get the original value of an expression.

    // GetOriginalValue returns the original value (if it exists) of the
    // expression Src with respect to the variable V in an assignment V = Src.
    //
    // Target is the target expression of the assignment (that accounts for
    // any necessary casts of V).
    //
    // The out parameter OVUsesV will be set to true if the original value
    // uses the value of the variable V.  This prevents callers from having
    // to compute the variable occurrence count of V in the original value,
    // since GetOriginalValue computes this count while trying to construct
    // the inverse expression of the source with respect to V.
    Expr *GetOriginalValue(DeclRefExpr *V, Expr *Target, Expr *Src,
                           const EquivExprSets EQ, bool &OVUsesV) {
      // Check if Src has an inverse expression with respect to v.
      Expr *IV = nullptr;
      if (IsInvertible(V, Src))
        IV = Inverse(V, Target, Src);
      if (IV) {
        // If Src has an inverse with respect to v, then the original
        // value (the inverse) must use the value of v.
        OVUsesV = true;
        return IV;
      }

      // If Src does not have an inverse with respect to v, then the original
      // value is either some variable w != v in EQ, or it is null.  In either
      // case, the original value cannot use the value of v.
      OVUsesV = false;
      
      // Check EQ for a variable w != v that produces the same value as v.
      Expr *ValuePreservingV = nullptr;
      EqualExprTy F = GetEqualExprSetContainingExpr(Target, EQ, ValuePreservingV);
      for (auto I = F.begin(); I != F.end(); ++I) {
        // Account for value-preserving operations on w when searching for
        // a variable w in F. For example, if F contains (T)LValueToRValue(w),
        // where w is a variable != v and (T) is a value-preserving cast, the
        // original value should be (T)LValueToRValue(w).
        Lexicographic Lex(S.Context, nullptr);
        Expr *E = Lex.IgnoreValuePreservingOperations(S.Context, *I);
        DeclRefExpr *W = GetRValueVariable(E);
        if (W != nullptr && !EqualValue(S.Context, V, W, nullptr)) {
          // Expression equality in UEQ does not account for types, so
          // expressions in the same set in UEQ may not have the same type.
          // The original value of Src with respect to v must have a type
          // compatible with the type of v (accounting for value-preserving
          // operations on v). For example, if F contains (T1)LValueToRValue(v)
          // and LValueToRValue(w), where v and w have type T2, (T1) is a value-
          // preserving cast, and T1 and T2 are not compatible types, the
          // original value should be LValueToRValue(w).
          if (S.Context.typesAreCompatible(ValuePreservingV->getType(),
                                            (*I)->getType()))
            return *I;
        }
      }

      return nullptr;
    }

    // IsInvertible returns true if the expression e can be inverted
    // with respect to the variable x.
    bool IsInvertible(DeclRefExpr *X, Expr *E) {
      if (!E)
        return false;

      E = E->IgnoreParens();
      if (IsRValueCastOfVariable(E, X))
        return true;

      switch (E->getStmtClass()) {
        case Expr::UnaryOperatorClass:
          return IsUnaryOperatorInvertible(X, cast<UnaryOperator>(E));
        case Expr::BinaryOperatorClass:
          return IsBinaryOperatorInvertible(X, cast<BinaryOperator>(E));
        case Expr::ImplicitCastExprClass:
        case Expr::CStyleCastExprClass:
          return IsCastExprInvertible(X, cast<CastExpr>(E));
        default:
          return false;
      }
    }

    // Returns true if a unary operator is invertible with respect to x.
    bool IsUnaryOperatorInvertible(DeclRefExpr *X, UnaryOperator *E) {
      UnaryOperatorKind Op = E->getOpcode();
      if (Op != UnaryOperatorKind::UO_Not &&
          Op != UnaryOperatorKind::UO_Minus &&
          Op != UnaryOperatorKind::UO_Plus)
        return false;

      return IsInvertible(X, E->getSubExpr());
    }

    // Returns true if a binary operator is invertible with respect to x.
    bool IsBinaryOperatorInvertible(DeclRefExpr *X, BinaryOperator *E) {
      BinaryOperatorKind Op = E->getOpcode();
      if (Op != BinaryOperatorKind::BO_Add &&
          Op != BinaryOperatorKind::BO_Sub &&
          Op != BinaryOperatorKind::BO_Xor)
        return false;

      Expr *LHS = E->getLHS();
      Expr *RHS = E->getRHS();
      
      // Addition and subtraction operations must be for checked pointer
      // arithmetic or unsigned integer arithmetic.
      if (Op == BinaryOperatorKind::BO_Add || Op == BinaryOperatorKind::BO_Sub) {
        // The operation is checked pointer arithmetic if either the LHS
        // or the RHS have checked pointer type.
        bool IsCheckedPtrArithmetic = LHS->getType()->isCheckedPointerType() ||
                                      RHS->getType()->isCheckedPointerType();
        if (!IsCheckedPtrArithmetic) {
          // The operation is unsigned integer arithmetic if both the LHS
          // and the RHS have unsigned integer type.
          bool IsUnsignedArithmetic = LHS->getType()->isUnsignedIntegerType() &&
                                      RHS->getType()->isUnsignedIntegerType();
          if (!IsUnsignedArithmetic)
            return false;
        }
      }

      // X must appear in exactly one subexpression of E and that
      // subexpression must be invertible with respect to X.
      std::pair<Expr *, Expr*> Pair = SplitByVarCount(X, LHS, RHS);
      if (!Pair.first)
        return false;
      Expr *E_X = Pair.first, *E_NotX = Pair.second;
      if (!IsInvertible(X, E_X))
        return false;

      // The subexpression not containing X must be nonmodifying
      // and cannot be or contain a pointer dereference, member
      // reference, or indirect member reference.
      if (!CheckIsNonModifying(E_NotX) || ReadsMemoryViaPointer(E_NotX, true))
        return false;

      return true;
    }

    // Returns true if a cast expression is invertible with respect to x.
    // A cast expression (T1)e1 is invertible if T1 is a bit-preserving
    // or widening cast and e1 is invertible.
    bool IsCastExprInvertible(DeclRefExpr *X, CastExpr *E) {
      QualType T1 = E->getType();
      QualType T2 = E->getSubExpr()->getType();
      uint64_t Size1 = S.Context.getTypeSize(T1);
      uint64_t Size2 = S.Context.getTypeSize(T2);

      // If T1 is a smaller type than T2, then (T1)e1 is a narrowing cast.
      if (Size1 < Size2)
        return false;

      switch (E->getCastKind()) {
        // Bit-preserving casts
        case CastKind::CK_BitCast:
        case CastKind::CK_LValueBitCast:
        case CastKind::CK_NoOp:
        case CastKind::CK_ArrayToPointerDecay:
        case CastKind::CK_FunctionToPointerDecay:
        case CastKind::CK_NullToPointer:
        // Widening casts
        case CastKind::CK_BooleanToSignedIntegral:
        case CastKind::CK_IntegralToFloating:
          return IsInvertible(X, E->getSubExpr());
        // Potentially non-narrowing casts, depending on type sizes
        case CastKind::CK_IntegralToPointer:
        case CastKind::CK_PointerToIntegral:
        case CastKind::CK_IntegralCast:
          return Size1 >= Size2 && IsInvertible(X, E->getSubExpr());
        // All other casts are considered narrowing
        default:
          return false;
      }
    }

    // Inverse repeatedly applies mathematical rules to the expression e to
    // get the inverse of e with respect to the variable x and expression f.
    // If rules cannot be applied to e, Inverse returns nullptr.
    Expr *Inverse(DeclRefExpr *X, Expr *F, Expr *E) {
      if (!F)
        return nullptr;

      E = E->IgnoreParens();
      if (IsRValueCastOfVariable(E, X))
        return F;

      switch (E->getStmtClass()) {
        case Expr::UnaryOperatorClass:
          return UnaryOperatorInverse(X, F, cast<UnaryOperator>(E));
        case Expr::BinaryOperatorClass:
          return BinaryOperatorInverse(X, F, cast<BinaryOperator>(E));
        case Expr::CStyleCastExprClass:
        case Expr::ImplicitCastExprClass:
          return CastExprInverse(X, F, cast<CastExpr>(E));
        default:
          return nullptr;
      }

      return nullptr;
    }

    // Returns the inverse of a unary operator using the following rule:
    // Inverse(f, @e1) = Inverse(@f, e1) where @ can be ~, -, or +.
    Expr *UnaryOperatorInverse(DeclRefExpr *X, Expr *F, UnaryOperator *E) {
      Expr *SubExpr = E->getSubExpr();
      UnaryOperatorKind Op = E->getOpcode();
      Expr *Child = ExprCreatorUtil::EnsureRValue(S, F);
      Expr *F1 = new (S.Context) UnaryOperator(Child, Op, E->getType(),
                                               E->getValueKind(),
                                               E->getObjectKind(),
                                               SourceLocation(),
                                               E->canOverflow());
      return Inverse(X, F1, SubExpr);
    }

    // Returns the inverse of a binary operator.
    Expr *BinaryOperatorInverse(DeclRefExpr *X, Expr *F, BinaryOperator *E) {
      std::pair<Expr *, Expr*> Pair = SplitByVarCount(X, E->getLHS(), E->getRHS());
      if (!Pair.first)
        return nullptr;

      Expr *E_X = Pair.first, *E_NotX = Pair.second;
      BinaryOperatorKind Op = E->getOpcode();
      Expr *F1 = nullptr;

      switch (Op) {
        case BinaryOperatorKind::BO_Add:
          // Inverse(f, e1 + e2) = Inverse(f - e_notx, e_x)
          F1 = ExprCreatorUtil::CreateBinaryOperator(S, F, E_NotX, BinaryOperatorKind::BO_Sub);
          break;
        case BinaryOperatorKind::BO_Sub: {
          if (E_X == E->getLHS())
            // Inverse(f, e_x - e_notx) = Inverse(f + e_notx, e_x)
            F1 = ExprCreatorUtil::CreateBinaryOperator(S, F, E_NotX, BinaryOperatorKind::BO_Add);
          else
            // Inverse(f, e_notx - e_x) => Inverse(e_notx - f, e_x)
            F1 = ExprCreatorUtil::CreateBinaryOperator(S, E_NotX, F, BinaryOperatorKind::BO_Sub);
          break;
        }
        case BinaryOperatorKind::BO_Xor:
          // Inverse(f, e1 ^ e2) = Inverse(x, f ^ e_notx, e_x)
          F1 = ExprCreatorUtil::CreateBinaryOperator(S, F, E_NotX, BinaryOperatorKind::BO_Xor);
          break;
        default:
          llvm_unreachable("unexpected binary operator kind");
      }

      return Inverse(X, F1, E_X);
    }

    // Returns the inverse of a cast expression.  If e1 has type T2,
    // Inverse(f, (T1)e1) = Inverse((T2)f, e1) (assuming that (T1) is
    // not a narrowing cast).
    Expr *CastExprInverse(DeclRefExpr *X, Expr *F, CastExpr *E) {
      QualType T1 = E->getType();
      QualType T2 = E->getSubExpr()->getType();
      Expr *F1 = nullptr;
      if (isa<ImplicitCastExpr>(E))
        F1 = CreateImplicitCast(T2, E->getCastKind(), F);
      else if (isa<CStyleCastExpr>(E))
        F1 = CreateExplicitCast(T2, E->getCastKind(), F,
                                E->isBoundsSafeInterface());
      if (!F1)
        return nullptr;
      return Inverse(X, F1, E->getSubExpr());
    }

    // GetIncomingBlockState returns the checking state that is true at the
    // beginning of the block by taking the intersection of the observed
    // bounds contexts and UEQ sets of equivalent expressions that were true
    // after each of the block's predecessors.
    //
    // Taking the intersection of the observed bounds contexts of the block's
    // predecessors ensures that, before checking a statement S in the block,
    // the block's observed bounds context contains only variables with bounds
    // that are in scope at S.  At the beginning of the block, each variable in
    // scope is mapped to its normalized declared bounds.
    CheckingState GetIncomingBlockState(const CFGBlock *Block,
                                        llvm::DenseMap<unsigned int, CheckingState> BlockStates) {
      CheckingState BlockState;
      bool IntersectionEmpty = true;
      for (const CFGBlock *PredBlock : Block->preds()) {
        // Prevent null or non-traversed (e.g. unreachable) blocks from causing
        // the incoming bounds context and UEQ set for a block to be empty.
        if (!PredBlock)
          continue;
        if (BlockStates.find(PredBlock->getBlockID()) == BlockStates.end())
          continue;
        CheckingState PredState = BlockStates[PredBlock->getBlockID()];
        if (IntersectionEmpty) {
          BlockState.ObservedBounds = PredState.ObservedBounds;
          BlockState.UEQ = PredState.UEQ;
          IntersectionEmpty = false;
        }
        else {
          BlockState.ObservedBounds =
            IntersectBoundsContexts(PredState.ObservedBounds,
                                    BlockState.ObservedBounds);
          BlockState.UEQ = IntersectUEQ(PredState.UEQ, BlockState.UEQ);
        }
      }
      return BlockState;
    }

    // IntersectBoundsContexts returns a bounds context resulting from taking
    // the intersection of the contexts Context1 and Context2.
    //
    // For each variable declaration v that is in both Context1 and Contex2,
    // the intersected context maps v to its normalized declared bounds.
    // Context1 or Context2 may map v to widened bounds, but those bounds
    // should not persist across CFG blocks.  The observed bounds for each
    // in-scope variable should be reset to its normalized declared bounds
    // at the beginning of a block, before widening the bounds in the block.
    BoundsContextTy IntersectBoundsContexts(BoundsContextTy Context1,
                                            BoundsContextTy Context2) {
      BoundsContextTy IntersectedContext;
      for (auto Pair : Context1) {
        const VarDecl *D = Pair.first;
        if (!Pair.second || !Context2.count(D))
          continue;
        if (const BoundsExpr *B = D->getBoundsExpr())
          IntersectedContext[D] = S.ExpandBoundsToRange(D, B);
      }
      return IntersectedContext;
    }

    // IntersectUEQ returns the intersection of two sets of sets of equivalent
    // expressions, where each set in UEQ1 is intersected with each set in
    // UEQ2 to produce an element of the result.
    EquivExprSets IntersectUEQ(const EquivExprSets UEQ1, const EquivExprSets UEQ2) {
      EquivExprSets IntersectedUEQ;
      for (auto I1 = UEQ1.begin(); I1 != UEQ1.end(); ++I1) {
        EqualExprTy G1 = *I1;
        for (auto I2 = UEQ2.begin(); I2 != UEQ2.end(); ++I2) {
          EqualExprTy G2 = *I2;
          EqualExprTy IntersectedG = IntersectG(G1, G2);
          if (IntersectedG.size() > 1)
            IntersectedUEQ.push_back(IntersectedG);
        }
      }
      return IntersectedUEQ;
    }

    // IntersectG returns the intersection of two sets of equivalent expressions.
    EqualExprTy IntersectG(const EqualExprTy G1, const EqualExprTy G2) {
      EqualExprTy IntersectedG;
      for (auto I = G1.begin(); I != G1.end(); ++I) {
        Expr *E1 = *I;
        if (EqualExprsContainsExpr(G2, E1))
          IntersectedG.push_back(E1);
      }
      return IntersectedG;
    }

    // GetEqualExprSetContainingExpr returns the set F in EQ that contains e
    // if such a set F exists, or an empty set otherwise.
    //
    // If there is a set F in EQ that contains an expression e1 such that
    // e1 is canonically equivalent to e, ValuePreservingE is set to e1.
    // e1 may include value-preserving operations.  For example, if a set F
    // in EQ contains (T)e, where (T) is a value-preserving cast,
    // ValuePreservingE will be set to (T)e.
    EqualExprTy GetEqualExprSetContainingExpr(Expr *E, EquivExprSets EQ,
                                              Expr *&ValuePreservingE) {
      ValuePreservingE = nullptr;
      for (auto OuterList = EQ.begin(); OuterList != EQ.end(); ++OuterList) {
        EqualExprTy F = *OuterList;
        for (auto InnerList = F.begin(); InnerList != F.end(); ++InnerList) {
          Expr *E1 = *InnerList;
          if (EqualValue(S.Context, E, E1, nullptr)) {
            ValuePreservingE = E1;
            return F;
          }
        }
      }
      return { };
    }

    // If e appears in a set F in EQ, GetEqualExprSetContainingExpr
    // returns F.  Otherwise, it returns an empty set.
    EqualExprTy GetEqualExprSetContainingExpr(Expr *E, EquivExprSets EQ) {
      for (auto OuterList = EQ.begin(); OuterList != EQ.end(); ++OuterList) {
        EqualExprTy F = *OuterList;
        if (EqualExprsContainsExpr(F, E))
          return F;
      }
      return { };
    }

    // IsEqualExprsSubset returns true if G1 is a subset of G2.
    bool IsEqualExprsSubset(const EqualExprTy G1, const EqualExprTy G2) {
      for (auto I = G1.begin(); I != G1.end(); ++I) {
        Expr *E = *I;
        if (!EqualExprsContainsExpr(G2, E))
          return false;
      }
      return true;
    }

    // EqualExprsContainsExpr returns true if the set G contains E.
    bool EqualExprsContainsExpr(const EqualExprTy G, Expr *E) {
      for (auto I = G.begin(); I != G.end(); ++I) {
        if (EqualValue(S.Context, E, *I, nullptr))
          return true;
      }
      return false;
    }

    // If E is a (possibly parenthesized) lvalue variable V,
    // GetLValueVariable returns V. Otherwise, it returns nullptr.
    DeclRefExpr *GetLValueVariable(Expr *E) {
      return dyn_cast<DeclRefExpr>(E->IgnoreParens());
    }

    // If E is a possibly parenthesized rvalue cast of a variable V,
    // GetRValueVariable returns V. Otherwise, it returns nullptr.
    //
    // V may have value-preserving operations applied to it.  For example,
    // if E is (LValueToRValue(LValueBitCast(V))), where V is a variable,
    // GetRValueVariable will return V.
    DeclRefExpr *GetRValueVariable(Expr *E) {
      if (!E)
        return nullptr;
      if (CastExpr *CE = dyn_cast<CastExpr>(E->IgnoreParens())) {
        CastKind CK = CE->getCastKind();
        if (CK == CastKind::CK_LValueToRValue ||
            CK == CastKind::CK_ArrayToPointerDecay) {
          Lexicographic Lex(S.Context, nullptr);
          Expr *SubExpr = const_cast<Expr *>(CE->getSubExpr());
          Expr *E1 = Lex.IgnoreValuePreservingOperations(S.Context, SubExpr);
          return dyn_cast<DeclRefExpr>(E1);
        }
      }
      return nullptr;
    }

    // IsRValueCastOfVariable returns true if the expression e is a possibly
    // parenthesized lvalue-to-rvalue cast of the lvalue variable v.
    bool IsRValueCastOfVariable(Expr *E, DeclRefExpr *V) {
      DeclRefExpr *Var = GetRValueVariable(E);
      if (!Var)
        return false;
      return EqualValue(S.Context, V, Var, nullptr);
    }

    // CreatesNewObject returns true if the expression e creates a new object.
    // Expressions that create new objects should not be added to the UEQ or G
    // sets of equivalent expressions in the checking state.
    bool CreatesNewObject(Expr *E) {
      switch (E->getStmtClass()) {
        case Expr::InitListExprClass:
        case Expr::ImplicitValueInitExprClass:
        case Expr::CompoundLiteralExprClass:
        case Expr::ExtVectorElementExprClass:
        case Expr::ExprWithCleanupsClass:
        case Expr::BlockExprClass:
        case Expr::SourceLocExprClass:
        case Expr::PackExprClass:
        case Expr::FixedPointLiteralClass:
        case Expr::StringLiteralClass:
          return true;
        default: {
          for (auto I = E->child_begin(); I != E->child_end(); ++I) {
            if (Expr *SubExpr = dyn_cast<Expr>(*I)) {
              if (CreatesNewObject(SubExpr))
                return true;
            }
          }
          return false;
        }
      }
    }

    // Returns true if the expression e reads memory via a pointer.
    // IncludeAllMemberExprs is used to modify the behavior to return true
    // if e is or contains a pointer dereference, member reference, or
    // indirect member reference (including e1.f which may not read memory
    // via a pointer).
    bool ReadsMemoryViaPointer(Expr *E, bool IncludeAllMemberExprs = false) {
      E = E->IgnoreParens();

      switch (E->getStmtClass()) {
        case Expr::UnaryOperatorClass: {
          UnaryOperator *UO = cast<UnaryOperator>(E);
          // *e reads memory via a pointer.
          return UO->getOpcode() == UnaryOperatorKind::UO_Deref;
        }
        // e1[e2] is a synonym for *(e1 + e2), which reads memory via a pointer.
        case Expr::ArraySubscriptExprClass:
          return true;
        case Expr::MemberExprClass: {
          if (IncludeAllMemberExprs)
            return true;

          MemberExpr *ME = cast<MemberExpr>(E);
          // e1->f reads memory via a pointer.
          if (ME->isArrow())
            return true;
          // e1.f reads memory via a pointer if and only if e1 reads
          // memory via a pointer.
          else
            return ReadsMemoryViaPointer(ME->getBase());
        }
        default: {
          for (auto I = E->child_begin(); I != E->child_end(); ++I) {
            if (Expr *SubExpr = dyn_cast<Expr>(*I)) {
              if (ReadsMemoryViaPointer(SubExpr))
                return true;
            }
          }
          return false;
        }
      }
    }

    // If the variable X appears exactly once in Ei and does not appear in
    // Ej, SplitByVarCount returns the pair (Ei, Ej).  Otherwise, it returns
    // an empty pair.
    std::pair<Expr *, Expr *> SplitByVarCount(DeclRefExpr *X, Expr *E1, Expr *E2) {
      std::pair<Expr *, Expr *> Pair;
      int Count1 = VariableOccurrenceCount(S, X, E1);
      int Count2 = VariableOccurrenceCount(S, X, E2);
      if (Count1 == 1 && Count2 == 0) {
        // X appears once in E1 and does not appear in E2.
        Pair.first = E1;
        Pair.second = E2;
      } else if (Count2 == 1 && Count1 == 0) {
        // X appears once in E2 and does not appear in E1.
        Pair.first = E2;
        Pair.second = E1;
      }
      return Pair;
    }

    // CheckIsNonModifying suppresses diagnostics while checking
    // whether e is a non-modifying expression.
    bool CheckIsNonModifying(Expr *E) {
      return S.CheckIsNonModifying(E, Sema::NonModifyingContext::NMC_Unknown,
                                   Sema::NonModifyingMessage::NMM_None);
    }

    BoundsExpr *CreateBoundsUnknown() {
      return Context.getPrebuiltBoundsUnknown();
    }

    // This describes an empty range. We use this where semantically the value
    // can never point to any range of memory, and statically understanding this
    // is useful.
    // We use this for example for function pointers or float-typed expressions.
    //
    // This is better than represenging the empty range as bounds(e, e), or even
    // bounds(e1, e2), because in these cases we need to do further analysis to
    // understand that the upper and lower bounds of the range are equal.
    BoundsExpr *CreateBoundsEmpty() {
      return CreateBoundsUnknown();
    }

    // This describes that this is an expression we will never
    // be able to infer bounds for.
    BoundsExpr *CreateBoundsAlwaysUnknown() {
      return CreateBoundsUnknown();
    }

    // If we have an error in our bounds inference that we can't
    // recover from, bounds(unknown) is our error value
    BoundsExpr *CreateBoundsInferenceError() {
      return CreateBoundsUnknown();
    }

    // This describes the bounds of null, which is compatible with every
    // other bounds annotation.
    BoundsExpr *CreateBoundsAny() {
      return new (Context) NullaryBoundsExpr(BoundsExpr::Kind::Any,
                                             SourceLocation(),
                                             SourceLocation());
    }

    // Currently our inference algorithm has some limitations,
    // where we cannot express bounds for things that will have bounds
    //
    // This is for the case where we want to allow these today,
    // but we need to re-visit these places and disallow some instances
    // when we can accurately calculate these bounds.
    BoundsExpr *CreateBoundsAllowedButNotComputed() {
      return CreateBoundsAny();
    }
    // This is for the opposite case, where we want to return bounds(unknown)
    // at the moment, but we want to re-visit these parts of inference
    // and in some cases compute bounds.
    BoundsExpr *CreateBoundsNotAllowedYet() {
      return CreateBoundsUnknown();
    }

    BoundsExpr *CreateSingleElementBounds(Expr *LowerBounds) {
      assert(LowerBounds->isRValue());
      return ExpandToRange(LowerBounds, Context.getPrebuiltCountOne());
    }

    Expr *CreateTemporaryUse(CHKCBindTemporaryExpr *Binding) {
      return new (Context) BoundsValueExpr(SourceLocation(), Binding);
    }

    Expr *CreateAddressOfOperator(Expr *E) {
      QualType Ty = Context.getPointerType(E->getType(), CheckedPointerKind::Array);
      return new (Context) UnaryOperator(E, UnaryOperatorKind::UO_AddrOf, Ty,
                                         ExprValueKind::VK_RValue,
                                         ExprObjectKind::OK_Ordinary,
                                         SourceLocation(), false);
    }

    // Determine if the mathemtical value of I (an unsigned integer) fits within
    // the range of Ty, a signed integer type.  APInt requires that bitsizes
    // match exactly, so if I does fit, return an APInt via Result with
    // exactly the bitsize of Ty.
    bool Fits(QualType Ty, const llvm::APInt &I, llvm::APInt &Result) {
      assert(Ty->isSignedIntegerType());
      unsigned bitSize = Context.getTypeSize(Ty);
      if (bitSize < I.getBitWidth()) {
        if (bitSize < I.getActiveBits())
         // Number of bits in use exceeds bitsize
         return false;
        else Result = I.trunc(bitSize);
      } else if (bitSize > I.getBitWidth())
        Result = I.zext(bitSize);
      else
        Result = I;
      return Result.isNonNegative();
    }

    // Create an integer literal from I.  I is interpreted as an
    // unsigned integer.
    IntegerLiteral *CreateIntegerLiteral(const llvm::APInt &I) {
      QualType Ty;
      // Choose the type of an integer constant following the rules in
      // Section 6.4.4 of the C11 specification: the smallest integer
      // type chosen from int, long int, long long int, unsigned long long
      // in which the integer fits.
      llvm::APInt ResultVal;
      if (Fits(Context.IntTy, I, ResultVal))
        Ty = Context.IntTy;
      else if (Fits(Context.LongTy, I, ResultVal))
        Ty = Context.LongTy;
      else if (Fits(Context.LongLongTy, I, ResultVal))
        Ty = Context.LongLongTy;
      else {
        assert(I.getBitWidth() <=
               Context.getIntWidth(Context.UnsignedLongLongTy));
        ResultVal = I;
        Ty = Context.UnsignedLongLongTy;
      }
      IntegerLiteral *Lit = IntegerLiteral::Create(Context, ResultVal, Ty,
                                                   SourceLocation());
      return Lit;
    }

    // If Ty is an integer type (char, unsigned int, int, etc.),
    // CreateIntegerLiteral returns an integer literal with Ty type.
    // If Ty denotes a pointer to an integer type (char *, ptr<int>, etc.),
    // CreateIntegerLiteral returns an integer literal with Ty's pointee type.
    // Otherwise, it returns nullptr.
    IntegerLiteral *CreateIntegerLiteral(int Value, QualType Ty) {
      QualType AdjustedType = Ty;
      if (Ty->isPointerType())
        AdjustedType = Ty->getPointeeType();
      if (!AdjustedType->isIntegerType())
        return nullptr;

      unsigned BitSize = Context.getTypeSize(AdjustedType);
      unsigned IntWidth = Context.getIntWidth(AdjustedType);
      if (BitSize != IntWidth)
        return nullptr;

      llvm::APInt ResultVal(BitSize, Value);
      return IntegerLiteral::Create(Context, ResultVal, AdjustedType,
                                    SourceLocation());
    }

    // Infer bounds for string literals.
    BoundsExpr *InferBoundsForStringLiteral(Expr *E, StringLiteral *SL,
                                            CHKCBindTemporaryExpr *Binding) {
      // Use the number of characters in the string (excluding the null
      // terminator) to calcaulte size.  Don't use the array type of the
      // literal.  In unchecked scopes, the array type is unchecked and its
      // size includes the null terminator.  It converts to an ArrayPtr that
      // could be used to overwrite the null terminator.  We need to prevent
      // this because literal strings may be shared and writeable, depending on
      // the C implementation.
      auto *Size = CreateIntegerLiteral(llvm::APInt(64, SL->getLength()));
      auto *CBE =
        new (Context) CountBoundsExpr(BoundsExpr::Kind::ElementCount,
                                      Size, SourceLocation(),
                                      SourceLocation());

      auto PtrType = Context.getDecayedType(E->getType());

      // For a string literal expression, we always bind the result of the
      // expression to a temporary. We then use this temporary in the bounds
      // expression for the string literal expression. Otherwise, a runtime
      // bounds check based on accessing the predefined expression could be
      // incorrect: the base value could be different for the lower and upper
      // bounds.
      auto *ArrLValue = CreateTemporaryUse(Binding);
      auto *Base = CreateImplicitCast(PtrType,
                                      CastKind::CK_ArrayToPointerDecay,
                                      ArrLValue);
      return ExpandToRange(Base, CBE);
    }

    // Infer the bounds for a member expression.
    // A member expression is an lvalue.
    //
    // MemberExprBounds should only be called on an
    // expression that has not had any side effects performed
    // on it, since PruneTemporaryBindings expects no bounds
    // expressions to have been set.
    BoundsExpr *MemberExprBounds(MemberExpr *ME, CheckedScopeSpecifier CSS) {
      FieldDecl *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
      if (!FD)
        return CreateBoundsInferenceError();

      if (ME->getType()->isArrayType()) {
        // Declared bounds override the bounds based on the array type.
        BoundsExpr *B = FD->getBoundsExpr();
        if (B) {
          B = S.MakeMemberBoundsConcrete(ME->getBase(), ME->isArrow(), B);
          if (!B) {
            assert(ME->getBase()->isRValue());
            // This can happen if the base expression is an rvalue expression.
            // It could be a function call that returns a struct, for example.
            return CreateBoundsNotAllowedYet();
          }
          if (B->isElementCount() || B->isByteCount()) {
            Expr *Base = CreateImplicitCast(Context.getDecayedType(ME->getType()),
                                            CastKind::CK_ArrayToPointerDecay,
                                            ME);
            return cast<BoundsExpr>(PruneTemporaryBindings(S, ExpandToRange(Base, B), CSS));
          } else
            return cast<BoundsExpr>(PruneTemporaryBindings(S, B, CSS));
        }

        // If B is an interop type annotation, the type must be identical
        // to the declared type, modulo checkedness.  So it is OK to
        // compute the array bounds based on the original type.
        return cast<BoundsExpr>(PruneTemporaryBindings(S, ArrayExprBounds(ME), CSS));
      }

      // It is an error for a member to have function type.
      if (ME->getType()->isFunctionType())
        return CreateBoundsInferenceError();

      // If E is an L-value, the ME must be an L-value too.
      if (ME->isRValue()) {
        llvm_unreachable("unexpected MemberExpr r-value");
        return CreateBoundsInferenceError();
      }

      Expr *AddrOf = CreateAddressOfOperator(ME);
      BoundsExpr* Bounds = CreateSingleElementBounds(AddrOf);
      return cast<BoundsExpr>(PruneTemporaryBindings(S, Bounds, CSS));
    }

    // Infer the bounds for the target of a member expression.
    // A member expression is an lvalue.
    //
    // MemberExprTargetBounds should only be called on an
    // expression that has not had any side effects performed
    // on it, since PruneTemporaryBindings expects no bounds
    // expressions to have been set.
    BoundsExpr *MemberExprTargetBounds(MemberExpr *ME,
                                       CheckedScopeSpecifier CSS) {
      FieldDecl *F = dyn_cast<FieldDecl>(ME->getMemberDecl());
      if (!F)
        return CreateBoundsInferenceError();

      BoundsExpr *B = F->getBoundsExpr();
      InteropTypeExpr *IT = F->getInteropTypeExpr();
      if (B && B->isUnknown())
        return CreateBoundsAlwaysUnknown();

      Expr *MemberBaseExpr = ME->getBase();
      if (!B && IT) {
        B = CreateTypeBasedBounds(ME, IT->getType(),
                                      /*IsParam=*/false,
                                      /*IsInteropTypeAnnotation=*/true);
        return cast<BoundsExpr>(PruneTemporaryBindings(S, B, CSS));
      }
            
      if (!B)
        return CreateBoundsAlwaysUnknown();

      B = S.MakeMemberBoundsConcrete(MemberBaseExpr, ME->isArrow(), B);
      if (!B) {
        // This can happen when MemberBaseExpr is an rvalue expression.  An example
        // of this a function call that returns a struct.  MakeMemberBoundsConcrete
        // can't handle this yet.
        return CreateBoundsNotAllowedYet();
      }

      if (B->isElementCount() || B->isByteCount()) {
          Expr *MemberRValue;
        if (ME->isLValue())
          MemberRValue = CreateImplicitCast(ME->getType(),
                                            CastKind::CK_LValueToRValue,
                                            ME);
        else
          MemberRValue = ME;
        B = ExpandToRange(MemberRValue, B);
      }

      return cast<BoundsExpr>(PruneTemporaryBindings(S, B, CSS));
    }

    // Given a Ptr type or a bounds-safe interface type, create the bounds
    // implied by the type.  If E is non-null, place the bounds in standard form
    // (do not use count or byte_count because their meaning changes
    //  when propagated to parent expressions).
    BoundsExpr *CreateTypeBasedBounds(Expr *E, QualType Ty, bool IsParam,
                                      bool IsBoundsSafeInterface) {
      BoundsExpr *BE = nullptr;
      // If the target value v is a Ptr type, it has bounds(v, v + 1), unless
      // it is a function pointer type, in which case it has no required
      // bounds.

      if (Ty->isCheckedPointerPtrType()) {
        if (Ty->isFunctionPointerType())
          BE = CreateBoundsEmpty();
        else if (Ty->isVoidPointerType())
          BE = Context.getPrebuiltByteCountOne();
        else
          BE = Context.getPrebuiltCountOne();
      } else if (Ty->isCheckedArrayType()) {
        assert(IsParam && IsBoundsSafeInterface && "unexpected checked array type");
        BE = CreateBoundsForArrayType(Ty);
      } else if (Ty->isCheckedPointerNtArrayType()) {
        BE = Context.getPrebuiltCountZero();
      }
   
      if (!BE)
        return CreateBoundsEmpty();

      if (!E)
        return BE;

      Expr *Base = E;
      if (Base->isLValue())
        Base = CreateImplicitCast(E->getType(), CastKind::CK_LValueToRValue, Base);

      // If type is a bounds-safe interface type, adjust the type of base to the
      // bounds-safe interface type.
      if (IsBoundsSafeInterface) {
        // Compute the target type.  We could receive an array type for a parameter
        // with a bounds-safe interface.
        QualType TargetTy = Ty;
        if (TargetTy->isArrayType()) {
          assert(IsParam);
          TargetTy = Context.getArrayDecayedType(Ty);
        };

        if (TargetTy != E->getType())
          Base = CreateExplicitCast(TargetTy, CK_BitCast, Base, true);
      } else
        assert(Ty == E->getType());

      return ExpandToRange(Base, BE);
    }

    // Compute the bounds of a cast operation that produces an rvalue.
    BoundsExpr *RValueCastBounds(CastExpr *E,
                                 BoundsExpr *TargetBounds,
                                 BoundsExpr *LValueBounds,
                                 BoundsExpr *RValueBounds,
                                 CheckingState State) {
      switch (E->getCastKind()) {
        case CastKind::CK_BitCast:
        case CastKind::CK_NoOp:
        case CastKind::CK_NullToPointer:
        // Truncation or widening of a value does not affect its bounds.
        case CastKind::CK_IntegralToPointer:
        case CastKind::CK_PointerToIntegral:
        case CastKind::CK_IntegralCast:
        case CastKind::CK_IntegralToBoolean:
        case CastKind::CK_BooleanToSignedIntegral:
          return RValueBounds;
        case CastKind::CK_LValueToRValue: {
          // For an rvalue cast of a variable v, if v has observed bounds,
          // the rvalue bounds of the value of v should be the observed bounds.
          // This also accounts for variables that have widened bounds.
          if (DeclRefExpr *V = GetRValueVariable(E)) {
            if (const VarDecl *D = dyn_cast_or_null<VarDecl>(V->getDecl())) {
              auto It = State.ObservedBounds.find(D);
              if (It != State.ObservedBounds.end())
                return It->second;
            }
          }
          // If an lvalue to rvalue cast e is not the value of a variable
          // with observed bounds, the rvalue bounds of e default to the
          // given target bounds.
          return TargetBounds;
        }
        case CastKind::CK_ArrayToPointerDecay: {
          // For an array to pointer cast of a variable v, if v has observed
          // bounds, the rvalue bounds of the value of v should be the observed
          // bounds. This also accounts for variables with array type that have
          // widened bounds.
          if (DeclRefExpr *V = GetRValueVariable(E)) {
            if (const VarDecl *D = dyn_cast_or_null<VarDecl>(V->getDecl())) {
              auto It = State.ObservedBounds.find(D);
              if (It != State.ObservedBounds.end())
                return It->second;
            }
          }
          // If an array to pointer cast e is not the value of a variable
          // with observed bounds, the rvalue bounds of e default to the
          // given lvalue bounds.
          return LValueBounds;
        }
        case CastKind::CK_DynamicPtrBounds:
        case CastKind::CK_AssumePtrBounds:
          llvm_unreachable("unexpected rvalue bounds cast");
        default:
          return CreateBoundsAlwaysUnknown();
      }
    }

    // Compute the bounds of a call expression.  Call expressions always
    // produce rvalues.
    //
    // If ResultName is non-null, it is a temporary variable where the result
    // of the call expression is stored immediately upon return from the call.
    BoundsExpr *CallExprBounds(const CallExpr *CE,
                               CHKCBindTemporaryExpr *ResultName) {
      BoundsExpr *ReturnBounds = nullptr;
      if (CE->getType()->isCheckedPointerPtrType()) {
        if (CE->getType()->isVoidPointerType())
          ReturnBounds = Context.getPrebuiltByteCountOne();
        else
          ReturnBounds = Context.getPrebuiltCountOne();
      }
      else {
        // Get the function prototype, where the abstract function return
        // bounds are kept.  The callee (if it exists) 
        // is always a function pointer.
        const PointerType *PtrTy =
          CE->getCallee()->getType()->getAs<PointerType>();
        if (PtrTy == nullptr)
          return CreateBoundsInferenceError();
        const FunctionProtoType *CalleeTy =
          PtrTy->getPointeeType()->getAs<FunctionProtoType>();
        if (!CalleeTy)
          // K&R functions have no prototype, and we cannot perform
          // inference on them, so we return bounds(unknown) for their results.
          return CreateBoundsAlwaysUnknown();

        BoundsAnnotations FunReturnAnnots = CalleeTy->getReturnAnnots();
        BoundsExpr *FunBounds = FunReturnAnnots.getBoundsExpr();
        InteropTypeExpr *IType =FunReturnAnnots.getInteropTypeExpr();
        // If there is no return bounds and there is an interop type
        // annotation, use the bounds impied by the interop type
        // annotation.
        if (!FunBounds && IType)
          FunBounds = CreateTypeBasedBounds(nullptr, IType->getType(),
                                            false, true);

        if (!FunBounds)
          // This function has no return bounds
          return CreateBoundsAlwaysUnknown();

        ArrayRef<Expr *> ArgExprs =
          llvm::makeArrayRef(const_cast<Expr**>(CE->getArgs()),
                              CE->getNumArgs());

        // Concretize Call Bounds with argument expressions.
        // We can only do this if the argument expressions are non-modifying
        ReturnBounds =
          S.ConcretizeFromFunctionTypeWithArgs(FunBounds, ArgExprs,
                            Sema::NonModifyingContext::NMC_Function_Return);
        // If concretization failed, this means we tried to substitute with
        // a non-modifying expression, which is not allowed by the
        // specification.
        if (!ReturnBounds)
          return CreateBoundsInferenceError();
      }

      if (ReturnBounds->isElementCount() ||
          ReturnBounds->isByteCount()) {
        if (!ResultName)
          return CreateBoundsInferenceError();
        ReturnBounds = ExpandToRange(CreateTemporaryUse(ResultName), ReturnBounds);
      }
      return ReturnBounds;
    }

    // Check that casts to checked function pointer types produce a valid
    // function pointer.  This implements the checks in Section 3.8 of v0.7
    // of the Checked C specification.
    //
    // The cast expression E has type ToType, a ptr<> to a function p type.  To
    // produce the function pointer,  the program is performing a sequence of
    // casts, both implicit and explicit. This sequence may include uses of
    // addr-of- (&) or deref(*), which act like casts for function pointer
    // types.
    //
    // Start by checking whether E must produce a valid function pointer:
    // - An lvalue-to-rvalue cast,
    // - A bounds-safe interface cast.
    //
    // If E is not guaranteed produce a valid function pointer, check that E
    // is a value-preserving case. Iterate through the chain of subexpressions
    // of E, as long as we see value-preserving casts or a cast-like operator.
    // If a cast is not value-preserving, it is an error because the resulting
    // value may not be valid function pointer.
    //
    // Let Needle be the subexpression the iteration ends at. Check whether
    // Needle is guaranteed to be a valid checked function pointer of type Ty:
    // - It s a null pointer.
    // - It is decl ref to a named function and the pointee type of TyType
    //   matches the function type.
    // - It is a checked function pointer Ty.
    // If is none of those, emit diagnostic about an incompatible type.
    void CheckDisallowedFunctionPtrCasts(CastExpr *E) {
      // The type of the outer value
      QualType ToType = E->getType();

      // We're only looking for casts to checked function ptr<>s.
      if (!ToType->isCheckedPointerPtrType() ||
        !ToType->isFunctionPointerType())
        return;

      // Skip lvalue-to-rvalue casts because they preserve types (except that
      // qualifers are removed).  The lvalue type should be a checked pointer
      // type too.
      if (const ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E))
        if (ICE->getCastKind() == CK_LValueToRValue) {
          assert(ICE->getSubExpr()->getType()->isCheckedPointerType());
          return;
        }

      // Skip bounds-safe interface casts.  They are trusted casts inserted
      // according to bounds-safe interface rules.  The only difference in
      // types is checkedness, which means that this is a trusted cast
      // to the checked function type pointer.
      if (E->isBoundsSafeInterface())
        return;

      if (!CheckValuePreservingCast(E, ToType)) {
        // The top-level cast is not value-preserving
        return;
      }

      // Iterate through chain of subexpressions that are value-preserving
      // casts or cast-like operations.
      const Expr *Needle = E->getSubExpr();
      while (true) {
        Needle = Needle->IgnoreParens();

        // Stop at any cast or cast-like operators that have a checked pointer
        // type.  If they are potential problematic casts, they'll be checked
        // by another call to CheckedDisallowedFunctionPtrCasts.
        if (Needle->getType()->isCheckedPointerType())
          break;

        // If we've found a cast expression...
        if (const CastExpr *NeedleCast = dyn_cast<CastExpr>(Needle)) {
          if (const ImplicitCastExpr *ICE = 
                dyn_cast<ImplicitCastExpr>(NeedleCast))
            // Stop at lvalue-to-ravlue casts.
            if (ICE->getCastKind() == CK_LValueToRValue)
              break;

          if (NeedleCast->isBoundsSafeInterface())
            break;

          if (!CheckValuePreservingCast(NeedleCast, ToType)) {
            // The cast is not value-preserving,
            return;
          }

          Needle = NeedleCast->getSubExpr();
          continue;
        }

        // If we've found a unary operator (such as * or &)...
        if (const UnaryOperator *NeedleOp = dyn_cast<UnaryOperator>(Needle)) {
          // Check if the operator is value-preserving.
          // Only addr-of (&) and deref (*) are with function pointers
          if (!CheckValuePreservingCastLikeOp(NeedleOp, ToType)) {
            return;
          }

          // Keep iterating.
          Needle = NeedleOp->getSubExpr();
          continue;
        }

        // Otherwise we have found an expression that is neither
        // a cast nor a cast-like operator.  Stop iterating.
        break;
      }

      // See if we stopped at a subexpression that must produce a valid checked
      // function pointer.

      // A null pointer.
      if (Needle->isNullPointerConstant(S.Context, Expr::NPC_NeverValueDependent))
        return;

      // A DeclRef to a function declaration matching the desired function type.
      if (const DeclRefExpr *NeedleDeclRef = dyn_cast<DeclRefExpr>(Needle)) {
        if (isa<FunctionDecl>(NeedleDeclRef->getDecl())) {
          // Checked that the function type is compatible with the pointee type
          // of ToType.
          if (S.Context.typesAreCompatible(ToType->getPointeeType(),
                                           Needle->getType(),
                                           /*CompareUnqualified=*/false,
                                           /*IgnoreBounds=*/false))
            return;
        } else {
          S.Diag(Needle->getExprLoc(),
                 diag::err_cast_to_checked_fn_ptr_not_value_preserving)
            << ToType << E->getSourceRange();
          return;
        }
      }

      // An expression with a checked pointer type.
      QualType NeedleTy = Needle->getType();
      if (!S.Context.typesAreCompatible(ToType, NeedleTy,
                                      /* CompareUnqualified=*/false,
                                      /*IgnoreBounds=*/false)) {
        // See if the only difference is that the source is an unchecked pointer type.
        if (NeedleTy->isPointerType()) {
          const PointerType *NeedlePtrType = NeedleTy->getAs<PointerType>();
          const PointerType *ToPtrType = ToType->getAs<PointerType>();
          if (S.Context.typesAreCompatible(NeedlePtrType->getPointeeType(),
                                           ToPtrType->getPointeeType(),
                                           /*CompareUnqualifed=*/false,
                                           /*IgnoreBounds=*/false)) {
            S.Diag(Needle->getExprLoc(), 
                   diag::err_cast_to_checked_fn_ptr_from_unchecked_fn_ptr) <<
              ToType << E->getSourceRange();
            return;
          }
        }

        S.Diag(Needle->getExprLoc(), 
               diag::err_cast_to_checked_fn_ptr_from_incompatible_type)
          << ToType << NeedleTy << NeedleTy->isCheckedPointerPtrType()
          << E->getSourceRange();
      }

      return;
    }

    // See if a cast is value-preserving for a function-pointer casts.   Other
    // operations might also be, but this algorithm is currently conservative.
    //
    // This will add the required error messages.
    bool CheckValuePreservingCast(const CastExpr *E, const QualType ToType) {
      switch (E->getCastKind())
      {
      case CK_NoOp:
      case CK_NullToPointer:
      case CK_FunctionToPointerDecay:
      case CK_BitCast:
      case CK_LValueBitCast:
        return true;
      default:
        S.Diag(E->getExprLoc(), diag::err_cast_to_checked_fn_ptr_not_value_preserving)
          << ToType << E->getSourceRange();

        return false;
      }
    }

    // See if an operationg is a value-preserving deref (*) or/ addr-of (&)
    // operator on a function pointer type.  Other operations might also be,
    // but this algorithm is currently conservative.
    //
    // This will add the required error messages
    bool CheckValuePreservingCastLikeOp(const UnaryOperator *E, const QualType ToType) {
      QualType ETy = E->getType();
      QualType SETy = E->getSubExpr()->getType();

      switch (E->getOpcode()) {
      case UO_Deref: {
        // This may be more conservative than necessary.
        bool between_functions = ETy->isFunctionType() && SETy->isFunctionPointerType();

        if (!between_functions) {
          // Add Error Message
          S.Diag(E->getExprLoc(), diag::err_cast_to_checked_fn_ptr_can_only_ref_deref_functions)
            << ToType << 0 << E->getSourceRange();
        }

        return between_functions;
      }
      case UO_AddrOf: {
        // This may be more conservative than necessary.
        bool between_functions = ETy->isFunctionPointerType() && SETy->isFunctionType();
        if (!between_functions) {
          // Add Error Message
          S.Diag(E->getExprLoc(), diag::err_cast_to_checked_fn_ptr_can_only_ref_deref_functions)
            << ToType << 1 << E->getSourceRange();
        }

        return between_functions;
      }
      default:
        S.Diag(E->getExprLoc(), diag::err_cast_to_checked_fn_ptr_not_value_preserving)
          << ToType << E->getSourceRange();

        return false;
      }
    }
  };
}

Expr *Sema::GetArrayPtrDereference(Expr *E, QualType &Result) {
  assert(E->isLValue());
  E = E->IgnoreParens();
  switch (E->getStmtClass()) {
    case Expr::DeclRefExprClass:
    case Expr::MemberExprClass:
    case Expr::CompoundLiteralExprClass:
    case Expr::ExtVectorElementExprClass:
      return nullptr;
    case Expr::UnaryOperatorClass: {
      UnaryOperator *UO = cast<UnaryOperator>(E);
      if (UO->getOpcode() == UnaryOperatorKind::UO_Deref &&
          UO->getSubExpr()->getType()->isCheckedPointerArrayType()) {
        Result = UO->getSubExpr()->getType();
        return E;
      }

      return nullptr;
    }

    case Expr::ArraySubscriptExprClass: {
      // e1[e2] is a synonym for *(e1 + e2).
      ArraySubscriptExpr *AS = cast<ArraySubscriptExpr>(E);
      // An important invariant for array types in Checked C is that all
      // dimensions of a multi-dimensional array are either checked or
      // unchecked.  This ensures that the intermediate values for
      // multi-dimensional array accesses have checked type and preserve
      //  the "checkedness" of the outermost array.

      // getBase returns the pointer-typed expression.
      if (getLangOpts().UncheckedPointersDynamicCheck ||
          AS->getBase()->getType()->isCheckedPointerArrayType()) {
        Result = AS->getBase()->getType();
        return E;
      }

      return nullptr;
    }
    case Expr::ImplicitCastExprClass: {
      ImplicitCastExpr *IC = cast<ImplicitCastExpr>(E);
      if (IC->getCastKind() == CK_LValueBitCast)
        return GetArrayPtrDereference(IC->getSubExpr(), Result);
      return nullptr;
    }
    default: {
      llvm_unreachable("unexpected lvalue expression");
      return nullptr;
    }
  }
}

BoundsExpr *Sema::CheckNonModifyingBounds(BoundsExpr *B, Expr *E) {
  if (!CheckIsNonModifying(B, Sema::NonModifyingContext::NMC_Unknown,
                              Sema::NonModifyingMessage::NMM_None)) {
    Diag(E->getBeginLoc(), diag::err_inferred_modifying_bounds) <<
        B << E->getSourceRange();
    CheckIsNonModifying(B, Sema::NonModifyingContext::NMC_Unknown,
                          Sema::NonModifyingMessage::NMM_Note);
    return CreateInvalidBoundsExpr();
  } else
    return B;
}

BoundsExpr *Sema::CreateCountForArrayType(QualType QT) {
  std::pair<ComparisonSet, ComparisonSet> EmptyFacts;
  return CheckBoundsDeclarations(*this, EmptyFacts).CreateBoundsForArrayType(QT);
}

Expr *Sema::MakeAssignmentImplicitCastExplicit(Expr *E) {
  if (!E->isRValue())
    return E;

  ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E);
  if (!ICE)
    return E;

  bool isUsualUnaryConversion = false;
  CastKind CK = ICE->getCastKind();
  Expr *SE = ICE->getSubExpr();
  QualType TargetTy = ICE->getType();
  if (CK == CK_FunctionToPointerDecay || CK == CK_ArrayToPointerDecay ||
      CK == CK_LValueToRValue)
    isUsualUnaryConversion = true;
  else if (CK == CK_IntegralCast) {
    QualType Ty = SE->getType();
    // Half FP have to be promoted to float unless it is natively supported
    if (CK == CK_FloatingCast && TargetTy == Context.FloatTy &&
        Ty->isHalfType() && !getLangOpts().NativeHalfType)
      isUsualUnaryConversion = true;
    else if (CK == CK_IntegralCast &&
             Ty->isIntegralOrUnscopedEnumerationType()) {
      QualType PTy = Context.isPromotableBitField(SE);
      if (!PTy.isNull() && TargetTy == PTy)
        isUsualUnaryConversion = true;
      else if (Ty->isPromotableIntegerType() &&
              TargetTy == Context.getPromotedIntegerType(Ty))
        isUsualUnaryConversion = true;
    }
  }

  if (isUsualUnaryConversion)
    return E;

  std::pair<ComparisonSet, ComparisonSet> EmptyFacts;
  return CheckBoundsDeclarations(*this, EmptyFacts).CreateExplicitCast(TargetTy, CK, SE,
                                                   ICE->isBoundsSafeInterface());
}

void Sema::CheckFunctionBodyBoundsDecls(FunctionDecl *FD, Stmt *Body) {
  if (Body == nullptr)
    return;
#if TRACE_CFG
  llvm::outs() << "Checking " << FD->getName() << "\n";
#endif
  ModifiedBoundsDependencies Tracker;
  // Compute a mapping from expressions that modify lvalues to in-scope bounds
  // declarations that depend upon those expressions.  We plan to change
  // CheckBoundsDeclaration to traverse a function body in an order determined
  // by control flow.   The modification information depends on lexically-scoped
  // information that can't be computed easily when doing a control-flow
  // based traversal.
  ComputeBoundsDependencies(Tracker, FD, Body);
  std::pair<ComparisonSet, ComparisonSet> EmptyFacts;
  std::unique_ptr<CFG> Cfg = CFG::buildCFG(nullptr, Body, &getASTContext(), CFG::BuildOptions());
  CheckBoundsDeclarations Checker(*this, Body, Cfg.get(), FD->getBoundsExpr(), EmptyFacts);
  if (Cfg != nullptr) {
    AvailableFactsAnalysis Collector(*this, Cfg.get());
    Collector.Analyze();
    if (getLangOpts().DumpExtractedComparisonFacts)
      Collector.DumpComparisonFacts(llvm::outs(), FD->getNameInfo().getName().getAsString());
    Checker.TraverseCFG(Collector, FD);
  }
  else {
    // A CFG couldn't be constructed.  CFG construction doesn't support
    // __finally or may encounter a malformed AST.  Fall back on to non-flow 
    // based analysis.  The CSS parameter is ignored because the checked
    // scope information is obtained from Body, which is a compound statement.
    Checker.Check(Body, CheckedScopeSpecifier::CSS_Unchecked);
  }

#if TRACE_CFG
  llvm::outs() << "Done " << FD->getName() << "\n";
#endif
}

void Sema::CheckTopLevelBoundsDecls(VarDecl *D) {
  if (!D->isLocalVarDeclOrParm()) {
    std::pair<ComparisonSet, ComparisonSet> EmptyFacts;
    CheckBoundsDeclarations Checker(*this, nullptr, nullptr, nullptr, EmptyFacts);
    Checker.TraverseTopLevelVarDecl(D, GetCheckedScopeInfo());
  }
}

namespace {
  class NonModifiyingExprSema : public RecursiveASTVisitor<NonModifiyingExprSema> {

  private:
    // Represents which kind of modifying expression we have found
    enum ModifyingExprKind {
      MEK_Assign,
      MEK_Increment,
      MEK_Decrement,
      MEK_Call,
      MEK_Volatile
    };

  public:
    NonModifiyingExprSema(Sema &S, Sema::NonModifyingContext From,
                          Sema::NonModifyingMessage Message) :
      S(S), FoundModifyingExpr(false), ReqFrom(From),
      Message(Message) {}

    bool isNonModifyingExpr() { return !FoundModifyingExpr; }

    // Assignments are of course modifying
    bool VisitBinAssign(BinaryOperator* E) {
      addError(E, MEK_Assign);
      FoundModifyingExpr = true;

      return true;
    }

    // Assignments are of course modifying
    bool VisitCompoundAssignOperator(CompoundAssignOperator *E) {
      addError(E, MEK_Assign);
      FoundModifyingExpr = true;

      return true;
    }

    // Pre-increment/decrement, Post-increment/decrement
    bool VisitUnaryOperator(UnaryOperator *E) {
      if (E->isIncrementDecrementOp()) {
        addError(E,
          E->isIncrementOp() ? MEK_Increment : MEK_Decrement);
        FoundModifyingExpr = true;
      }

      return true;
    }

    // Dereferences of volatile variables are modifying.
    bool VisitCastExpr(CastExpr *E) {
      CastKind CK = E->getCastKind();
      if (CK == CK_LValueToRValue)
        FindVolatileVariable(E->getSubExpr());

      return true;
    }

    void FindVolatileVariable(Expr *E) {
      E = E->IgnoreParens();
      switch (E->getStmtClass()) {
        case Expr::DeclRefExprClass: {
          QualType RefType = E->getType();
          if (RefType.isVolatileQualified()) {
            addError(E, MEK_Volatile);
            FoundModifyingExpr = true;
          }
          break;
        }
        case Expr::ImplicitCastExprClass: {
          ImplicitCastExpr *ICE = cast<ImplicitCastExpr>(E);
          if (ICE->getCastKind() == CastKind::CK_LValueBitCast)
            return FindVolatileVariable(ICE->getSubExpr());
          break;
        }
        default:
          break;
      }
    }

    // Function Calls are defined as modifying
    bool VisitCallExpr(CallExpr *E) {
      addError(E, MEK_Call);
      FoundModifyingExpr = true;

      return true;
    }


  private:
    Sema &S;
    bool FoundModifyingExpr;
    Sema::NonModifyingContext ReqFrom;
    Sema::NonModifyingMessage Message;
    // Track modifying expressions so that we can suppress duplicate diagnostic
    // messages for the same modifying expression.
    SmallVector<Expr *, 4> ModifyingExprs;

    void addError(Expr *E, ModifyingExprKind Kind) {
      if (Message != Sema::NonModifyingMessage::NMM_None) {
        for (auto Iter = ModifyingExprs.begin(); Iter != ModifyingExprs.end(); Iter++) {
          if (*Iter == E)
            return;
        }
        ModifyingExprs.push_back(E);
        unsigned DiagId = (Message == Sema::NonModifyingMessage::NMM_Error) ?
          diag::err_not_non_modifying_expr : diag::note_modifying_expression;
        S.Diag(E->getBeginLoc(), DiagId)
          << Kind << ReqFrom << E->getSourceRange();
      }
    }
  };
}

bool Sema::CheckIsNonModifying(Expr *E, NonModifyingContext Req,
                               NonModifyingMessage Message) {
  NonModifiyingExprSema Checker(*this, Req, Message);
  Checker.TraverseStmt(E);

  return Checker.isNonModifyingExpr();
}

/* Will uncomment this in a future pull request.
bool Sema::CheckIsNonModifying(BoundsExpr *E, bool ReportError) {
  NonModifyingContext req = NMC_Unknown;
  if (isa<RangeBoundsExpr>(E))
    req = NMC_Range;
  else if (const CountBoundsExpr *CountBounds = dyn_cast<CountBoundsExpr>(E))
    req = CountBounds->isByteCount() ? NMC_Byte_Count : NMC_Count;

  NonModifiyingExprSema Checker(*this, Req, ReportError);
  Checker.TraverseStmt(E);

  return Checker.isNonModifyingExpr();
}
*/

void Sema::WarnDynamicCheckAlwaysFails(const Expr *Condition) {
  bool ConditionConstant;
  if (Condition->EvaluateAsBooleanCondition(ConditionConstant, Context)) {
    if (!ConditionConstant) {
      // Dynamic Check always fails, emit warning
      Diag(Condition->getBeginLoc(), diag::warn_dynamic_check_condition_fail)
        << Condition->getSourceRange();
    }
  }
}

// This is wrapper around CheckBoundsDeclaration::ExpandToRange. This provides
// an easy way to invoke this function from outside the class. Given a
// byte_count or count bounds expression for the VarDecl D, ExpandToRange will
// expand it to a range bounds expression.
BoundsExpr *Sema::ExpandBoundsToRange(const VarDecl *D, const BoundsExpr *B) {
  // If the bounds expr is already a RangeBoundsExpr, simply return it.
  if (B && isa<RangeBoundsExpr>(B))
    return const_cast<BoundsExpr *>(B);

  std::pair<ComparisonSet, ComparisonSet> EmptyFacts;
  CheckBoundsDeclarations CBD = CheckBoundsDeclarations(*this, EmptyFacts);

  if (D->getType()->isArrayType()) {
    ExprResult ER = BuildDeclRefExpr(const_cast<VarDecl *>(D), D->getType(),
                                     clang::ExprValueKind::VK_LValue,
                                     SourceLocation());
    if (ER.isInvalid())
      return nullptr;
    Expr *Base = ER.get();

    // Declared bounds override the bounds based on the array type.
    if (!B)
      return CBD.ArrayExprBounds(Base);
    Base = CBD.CreateImplicitCast(Context.getDecayedType(Base->getType()),
                                  CastKind::CK_ArrayToPointerDecay,
                                  Base);
    return CBD.ExpandToRange(Base, const_cast<BoundsExpr *>(B));
  }
  return CBD.ExpandToRange(const_cast<VarDecl *>(D),
                           const_cast<BoundsExpr *>(B));
}
