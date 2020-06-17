//=--ArrayBoundsInformation.cpp-----------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This contains the implementation of the methods in ArrayBoundsInformation.
//===----------------------------------------------------------------------===//

#include "clang/CConv/ArrayBoundsInformation.h"
#include "clang/CConv/ProgramInfo.h"
#include "clang/CConv/Utils.h"

ConstraintKey ArrayBoundsInformation::getTopLevelConstraintVar(Decl *D) {
  std::set<ConstraintVariable *> DefCVars =
      Info.getVariable(D, &(D->getASTContext()), true);
  for (auto ConsVar : DefCVars) {
    if (PVConstraint *PV = dyn_cast<PVConstraint>(ConsVar)) {
      auto &Cvars = PV->getCvars();
      if (Cvars.size() > 0) {
        return *(Cvars.begin());
      }
    }
  }
  assert (false && "Invalid declaration variable requested.");
}

bool ArrayBoundsInformation::addBoundsInformation(FieldDecl *ArrFD,
                                                  FieldDecl *LenFD) {
  ConstraintKey ArrCKey = getTopLevelConstraintVar(ArrFD);
  std::string BString = LenFD->getNameAsString();
  auto BPair = std::make_pair(BoundsKind::LocalFieldBound, BString);
  return BoundsInfo[ArrCKey].insert(BPair).second;
}

bool ArrayBoundsInformation::addBoundsInformation(FieldDecl *ArrFD,
                                                  Expr *E) {
  ConstraintKey ArrCKey = getTopLevelConstraintVar(ArrFD);
  auto BInfo = getExprBoundsInfo(ArrFD, E);
  if (BInfo.first != ArrayBoundsInformation::BoundsKind::InvalidKind)
    return BoundsInfo[ArrCKey].insert(BInfo).second;
  return false;
}

bool ArrayBoundsInformation::addBoundsInformation(FieldDecl *ArrFD,
                                                  BOUNDSINFOTYPE Binfo) {
  ConstraintKey ArrCKey = getTopLevelConstraintVar(ArrFD);
  return BoundsInfo[ArrCKey].insert(Binfo).second;
}

bool ArrayBoundsInformation::addBoundsInformation(ParmVarDecl *ArrFD,
                                                  ParmVarDecl *LenFD) {
  ConstraintKey ArrCKey = getTopLevelConstraintVar(ArrFD);
  std::string BString = LenFD->getNameAsString();
  auto BPair = std::make_pair(BoundsKind::LocalParamBound, BString);
  return BoundsInfo[ArrCKey].insert(BPair).second;
}

bool ArrayBoundsInformation::addBoundsInformation(ParmVarDecl *ArrFD,
                                                  BOUNDSINFOTYPE Binfo) {
  ConstraintKey ArrCKey = getTopLevelConstraintVar(ArrFD);
  return BoundsInfo[ArrCKey].insert(Binfo).second;
}

bool ArrayBoundsInformation::addBoundsInformation(VarDecl *ArrFD,
                                                  VarDecl *LenFD) {
  ConstraintKey ArrCKey = getTopLevelConstraintVar(ArrFD);
  std::string BString = LenFD->getNameAsString();
  auto BPair = std::make_pair(BoundsKind::LocalVarBound, BString);
  return BoundsInfo[ArrCKey].insert(BPair).second;
}

bool ArrayBoundsInformation::addBoundsInformation(VarDecl *ArrFD,
                                                  BOUNDSINFOTYPE Binfo) {
  ConstraintKey ArrCKey = getTopLevelConstraintVar(ArrFD);
  return BoundsInfo[ArrCKey].insert(Binfo).second;
}

bool ArrayBoundsInformation::addBoundsInformation(VarDecl *ArrFD, Expr *E) {
  ConstraintKey ArrCKey = getTopLevelConstraintVar(ArrFD);
  auto Binfo = getExprBoundsInfo(nullptr, E);
  if (Binfo.first != ArrayBoundsInformation::BoundsKind::InvalidKind)
    return BoundsInfo[ArrCKey].insert(Binfo).second;
  return false;
}

bool ArrayBoundsInformation::removeBoundsInformation(Decl *D) {
  ConstraintKey ArrCKey = getTopLevelConstraintVar(D);
  if (BoundsInfo.find(ArrCKey) != BoundsInfo.end()) {
    BoundsInfo.erase(ArrCKey);
    return true;
  }
  return false;
}

bool ArrayBoundsInformation::hasBoundsInformation(Decl *D) {
  ConstraintKey ArrCKey = getTopLevelConstraintVar(D);
  return BoundsInfo.find(ArrCKey) != BoundsInfo.end();
}

ArrayBoundsInformation::BOUNDSINFOTYPE
ArrayBoundsInformation::getBoundsInformation(Decl *D) {
  assert(hasBoundsInformation(D) && "Has no bounds information "
                                       "for the decl");
  ConstraintKey ArrCKey = getTopLevelConstraintVar(D);
  return *(BoundsInfo[ArrCKey].begin());
}

bool ArrayBoundsInformation::isValidBoundKindForField(
    ArrayBoundsInformation::BoundsKind BoundsKind) {
  return BoundsKind != ArrayBoundsInformation::BoundsKind::
                                LocalParamBound &&
         BoundsKind != ArrayBoundsInformation::BoundsKind::
                                LocalVarBound &&
         BoundsKind != ArrayBoundsInformation::BoundsKind::InvalidKind;
}

ArrayBoundsInformation::BOUNDSINFOTYPE
ArrayBoundsInformation::combineBoundsInfo(FieldDecl *Field,
                                          ArrayBoundsInformation::
                                              BOUNDSINFOTYPE &B1,
                                          ArrayBoundsInformation::
                                              BOUNDSINFOTYPE &B2,
                                          std::string OpStr) {
  auto InvalidB =
      std::make_pair(ArrayBoundsInformation::BoundsKind::InvalidKind, "");
  ArrayBoundsInformation::BoundsKind BKind = BoundsKind ::InvalidKind;
  if (B1.first != ArrayBoundsInformation::BoundsKind::InvalidKind &&
      B2.first != ArrayBoundsInformation::BoundsKind::InvalidKind) {
    BKind = B1.first;

    if (B1.first != B2.first) {
      BKind = BoundsKind ::InvalidKind;
      if (B1.first == ArrayBoundsInformation::BoundsKind::ConstantBound) {
        BKind = B2.first;
      }
      if (B2.first == ArrayBoundsInformation::BoundsKind::ConstantBound) {
        BKind = B1.first;
      }
    }
  }

  if (BKind != BoundsKind::InvalidKind &&
      (Field == nullptr || isValidBoundKindForField(BKind))) {
    auto BStr = "(" + B1.second + " " + OpStr + " " + B2.second + ")";
    return std::make_pair(BKind, BStr);
  }
  return InvalidB;
}

ArrayBoundsInformation::BOUNDSINFOTYPE
ArrayBoundsInformation::getExprBoundsInfo(
  FieldDecl *Field,
  Expr *E) {
  E = removeAuxillaryCasts(E);
  auto InvalidB =
      std::make_pair(ArrayBoundsInformation::BoundsKind::InvalidKind, "");
  if (BinaryOperator *BO = dyn_cast<BinaryOperator>(E)) {
    Expr *LHS = BO->getLHS();
    Expr *RHS = BO->getRHS();

    auto LInfo = getExprBoundsInfo(Field, LHS);
    auto RInfo = getExprBoundsInfo(Field, RHS);
    return combineBoundsInfo(Field, LInfo, RInfo, BO->getOpcodeStr().str());

  } else if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (DeclaratorDecl *DD = dyn_cast<DeclaratorDecl>(DRE->getDecl())) {
      if (FieldDecl *FD = dyn_cast<FieldDecl>(DD)) {
        if (Field != nullptr && FD->getParent() == Field->getParent())
          return std::make_pair(ArrayBoundsInformation::BoundsKind::
                                    LocalFieldBound,
                                FD->getNameAsString());

        return InvalidB;
      }
      if (ParmVarDecl *PVD = dyn_cast<ParmVarDecl>(DD)) {
        if (Field == nullptr)
          return std::make_pair(ArrayBoundsInformation::BoundsKind::
                                    LocalParamBound,
                                PVD->getNameAsString());

        return InvalidB;
      } else if (VarDecl *VD = dyn_cast<VarDecl>(DD)) {
        if (VD->hasGlobalStorage())
          return std::make_pair(ArrayBoundsInformation::BoundsKind::
                                    ConstantBound,
                                VD->getNameAsString());

        if (!VD->hasGlobalStorage() && Field == nullptr)
          return std::make_pair(ArrayBoundsInformation::BoundsKind::
                                    LocalVarBound,
                                VD->getNameAsString());

        return InvalidB;
      }
    }
  } else if (IntegerLiteral *IL = dyn_cast<IntegerLiteral>(E)) {
    std::string BInfo = "" + std::to_string(IL->getValue().getZExtValue());
    return std::make_pair(ArrayBoundsInformation::BoundsKind::ConstantBound,
                          BInfo);
  } else if (MemberExpr *DRE = dyn_cast<MemberExpr>(E)) {
    if (FieldDecl *FD = dyn_cast<FieldDecl>(DRE->getMemberDecl())) {
      if (Field != nullptr && FD->getParent() == Field->getParent())
        return
            std::make_pair(ArrayBoundsInformation::BoundsKind::
                               LocalFieldBound,
                           FD->getNameAsString());

      return InvalidB;
    }
  } else if (UnaryExprOrTypeTraitExpr *UETE =
                 dyn_cast<UnaryExprOrTypeTraitExpr>(E)) {
    if (UETE->getKind() == UETT_SizeOf) {
      std::string TmpString;
      llvm::raw_string_ostream RawStr(TmpString);
      UETE->printPretty(RawStr, nullptr,
                        PrintingPolicy(LangOptions()));
      return
          std::make_pair(ArrayBoundsInformation::BoundsKind::
                             ConstantBound,
                            RawStr.str());
    }
  } else if (StringLiteral *SL = dyn_cast<StringLiteral>(E)) {
    std::string BInfo = "" + std::to_string(SL->getLength());
    return std::make_pair(ArrayBoundsInformation::
                              BoundsKind::ConstantBound,
                          BInfo);
  }
  E->dump();
  assert(false && "Unable to handle expression type");
}
