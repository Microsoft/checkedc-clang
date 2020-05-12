//=--Constraints.h------------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This implements a simple constraint solver for expressions of the form:
//  a = b
//  not a
//  a implies b
//
// The Checked C converter tool performs type inference to identify locations
// where a C type might be replaced with a Checked C type. This interface
// does the solving to figure out where those substitions might happen.
//===----------------------------------------------------------------------===//

#ifndef _CONSTRAINTS_H
#define _CONSTRAINTS_H
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <map>

class Constraint;
class ConstraintVariable;
class Constraints;

template<typename T>
struct PComp
{
  bool operator()(const T Lhs, const T Rhs) const {
    return *Lhs < *Rhs;
  }
};

class VarAtom;

// Represents atomic values that can occur at positions in constraints.
class Atom {
public:
  enum AtomKind {
    A_Var,
    A_Ptr,
    A_Arr,
    A_NTArr,
    A_Wild,
    A_Const
  };
private:
  const AtomKind Kind;
public:
  Atom(AtomKind K) : Kind(K) {}
  virtual ~Atom() {}

  AtomKind getKind() const { return Kind; }

  virtual void print(llvm::raw_ostream &) const = 0;
  virtual void dump(void) const = 0;
  virtual void dump_json(llvm::raw_ostream &) const = 0;
  virtual bool operator==(const Atom &) const = 0;
  virtual bool operator!=(const Atom &) const = 0;
  virtual bool operator<(const Atom &) const = 0;
  // Check if this atom contains the provided atom
  virtual bool containsConstraint(VarAtom *) = 0;
};

class ConstAtom : public Atom {
public:
  ConstAtom() : Atom(A_Const) {}
  ConstAtom(AtomKind K) : Atom(K) {}

  static bool classof(const Atom *A) {
    // Something is a class of ConstAtom if it ISN'T a Var.
    return A->getKind() != A_Var;
  }

  virtual bool containsConstraint(VarAtom *) {
    // Constant atom can never contain a VarAtom
    return false;
  }
};

// This refers to a location that we are trying to solve for.
class VarAtom : public Atom {
  friend class Constraints;
public:
  VarAtom(uint32_t D) : Atom(A_Var), Loc(D) {
    IfArrThenNtArray = false;
    ShouldBeArr = false;
    ShouldBeNtArr = false;
  }

  static bool classof(const Atom *S) {
    return S->getKind() == A_Var;
  }

  void print(llvm::raw_ostream &O) const {
    O << "q_" << Loc;
  }

  void dump(void) const {
    print(llvm::errs());
  }

  void dump_json(llvm::raw_ostream &O) const {
    O << "\"q_" << Loc << "\"";
  }

  bool operator==(const Atom &Other) const {
    if (const VarAtom *V = llvm::dyn_cast<VarAtom>(&Other))
      return V->Loc == Loc;
    else 
      return false;
  }

  bool operator!=(const Atom &Other) const {
    return !(*this == Other);
  }

  bool operator<(const Atom &Other) const {
    if (const VarAtom *V = llvm::dyn_cast<VarAtom>(&Other))
      return Loc < V->Loc;
    else
      return false;
  }

  uint32_t getLoc() const {
    return Loc;
  }

  void eraseConstraint(Constraint *Todel) {
    // Remove the constraint
    Constraints.erase(Todel);
    // Add the constraint into another set so that
    // we can restore in future.
    ErasedConstraints.insert(Todel);
  }

  // Replace the equality constraints that contains the provided
  // constraint variable with the constant atom
  unsigned replaceEqConstraints(std::map<VarAtom *, ConstAtom *,
                                PComp<VarAtom *>> &VAtoms,
                                class Constraints &CS);

  // Restore the erased constraints into the regular constraints.
  bool resetErasedConstraints() {
    bool Added = false;
    // Insert the erased constraints into the original constraints.
    for (auto c : ErasedConstraints) {
      Added = Constraints.insert(c).second || Added;
    }
    // Remove all the erased constraints.
    ErasedConstraints.clear();
    return Added;
  }

  bool containsConstraint(VarAtom *ToFind) {
    // This is a VarAtom and contains is same as equality.
    return (*this == *ToFind);
  }

  // Returns the constraints associated with this atom.
  std::set<Constraint *, PComp<Constraint *>> &getAllConstraints() {
    return Constraints;
  }

  // Check if we can assign the provided const atom to this VarAtom
  // this is to implement a Band Pass filter mechanism.
  // i.e., this VarAtom cannot be assigned or involved in propagating
  // some ConstAtom.
  // for example: a static array i.e., int arr[10] can never be WILD.
  bool inline canAssign(ConstAtom *ToAssign) {
    return ImpossibleVals.find(ToAssign->getKind()) == ImpossibleVals.end();
  }

  // Set the provided constant atom as being impossible for this VarAtom.
  void setConstImpossible(ConstAtom *ImpossibleConst) {
    ImpossibleVals.insert(ImpossibleConst->getKind());
  }

  void setNtArrayIfArray() { IfArrThenNtArray = true; }

  void setShouldBeArr() { ShouldBeArr = true; }

  void setShouldBeNtArr() { ShouldBeNtArr = true; }

  bool getShouldBeArr() { return ShouldBeArr; }

  bool getShouldBeNtArr() { return ShouldBeNtArr; }

  bool couldBeNtArr(ConstAtom *cVal) {
    return IfArrThenNtArray && cVal->getKind() == A_Arr;
  }

private:
  std::set<ConstAtom::AtomKind> ImpossibleVals;
  // Flag that indicates that if this atom is an array then should be tried
  // to promote to NtArr.
  bool IfArrThenNtArray;
  bool ShouldBeArr;
  bool ShouldBeNtArr;
  uint32_t  Loc;
  // These are the constraints erased during constraint solving.
  std::set<Constraint *, PComp<Constraint *>> ErasedConstraints;
  // The constraint expressions where this variable is mentioned on the 
  // LHS of an equality.
  std::set<Constraint *, PComp<Constraint *>> Constraints;
};

// This refers to the constant PTR.
class PtrAtom : public ConstAtom {
public:
  PtrAtom() : ConstAtom(A_Ptr) {}

  static bool classof(const Atom *S) {
    return S->getKind() == A_Ptr;
  }

  void print(llvm::raw_ostream &O) const {
    O << "PTR";
  }

  void dump(void) const {
    print(llvm::errs());
  }

  void dump_json(llvm::raw_ostream &O) const {
    O << "\"PTR\"";
  }

  bool operator==(const Atom &Other) const {
    return llvm::isa <PtrAtom>(&Other);
  }

  bool operator!=(const Atom &Other) const {
    return !(*this == Other);
  }

  bool operator<(const Atom &Other) const {
    return *this != Other;
  }
};

// This refers to the constant ARR.
class ArrAtom : public ConstAtom {
public:
  ArrAtom() : ConstAtom(A_Arr) {}

  static bool classof(const Atom *S) {
    return S->getKind() == A_Arr;
  }

  void print(llvm::raw_ostream &O) const {
    O << "ARR";
  }

  void dump(void) const {
    print(llvm::errs());
  }

  void dump_json(llvm::raw_ostream &O) const {
    O << "\"ARR\"";
  }

  bool operator==(const Atom &Other) const {
    return llvm::isa<ArrAtom>(&Other);
  }

  bool operator!=(const Atom &Other) const {
    return !(*this == Other);
  }

  bool operator<(const Atom &Other) const {
    if (llvm::isa<PtrAtom>(&Other) || *this == Other)
      return false;
    else
      return true;
  }
};

// This refers to the constant NTARR.
class NTArrAtom : public ConstAtom {
public:
  NTArrAtom() : ConstAtom(A_NTArr) {}

  static bool classof(const Atom *S) {
    return S->getKind() == A_NTArr;
  }

  void print(llvm::raw_ostream &O) const {
    O << "NTARR";
  }

  void dump(void) const {
    print(llvm::errs());
  }

  void dump_json(llvm::raw_ostream &O) const {
    O << "\"NTARR\"";
  }

  bool operator==(const Atom &Other) const {
    return llvm::isa<NTArrAtom>(&Other);
  }

  bool operator!=(const Atom &Other) const {
    return !(*this == Other);
  }

  bool operator<(const Atom &Other) const {
    if (llvm::isa<PtrAtom>(&Other) || llvm::isa<ArrAtom>(&Other) ||
        *this == Other)
      return false;
    else
      return true;
  }
};

// This refers to the constant WILD.
class WildAtom : public ConstAtom {
public:
  WildAtom() : ConstAtom(A_Wild) {}

  static bool classof(const Atom *S) {
    return S->getKind() == A_Wild;
  }

  void print(llvm::raw_ostream &O) const {
    O << "WILD";
  }

  void dump(void) const {
    print(llvm::errs());
  }

  void dump_json(llvm::raw_ostream &O) const {
    O << "\"WILD\"";
  }

  bool operator==(const Atom &Other) const {
    if (llvm::isa<WildAtom>(&Other))
      return true;
    else
      return false;
  }

  bool operator!=(const Atom &Other) const {
    return !(*this == Other);
  }

  bool operator<(const Atom &Other) const {
    if (llvm::isa<ArrAtom>(&Other) || llvm::isa<NTArrAtom>(&Other) ||
        llvm::isa<PtrAtom>(&Other) || *this == Other)
      return false;
    else
      return true;
  }
};

// Represents constraints of the form:
//  - a = b
//  - not a
//  - a => b
class Constraint {
public:
  enum ConstraintKind {
    C_Eq,
    C_Not,
    C_Imp
  };
private:
  const ConstraintKind Kind;
public:
  std::string REASON = "DEFAULT";
  Constraint(ConstraintKind K) : Kind(K) { }
  Constraint(ConstraintKind K, std::string &rsn) : Kind(K) {
    REASON = rsn;
  }
  virtual ~Constraint() {}

  ConstraintKind getKind() const { return Kind; }

  virtual void print(llvm::raw_ostream &) const = 0;
  virtual void dump(void) const = 0;
  virtual void dump_json(llvm::raw_ostream &) const = 0;
  virtual bool operator==(const Constraint &) const = 0;
  virtual bool operator!=(const Constraint &) const = 0;
  virtual bool operator<(const Constraint &) const = 0;
  virtual std::string getReason() {
    return REASON;
  }
  // Check if the provided constraint contains the provided VarAtom.
  virtual bool containsConstraint(VarAtom *) = 0;
};

// a = b
class Eq : public Constraint {
  friend class VarAtom;
public:

  Eq(Atom *Lhs, Atom *Rhs)
    : Constraint(C_Eq), lhs(Lhs), rhs(Rhs) {}

  Eq(Atom *Lhs, Atom *Rhs, std::string &Rsn)
      : Constraint(C_Eq, Rsn), lhs(Lhs), rhs(Rhs) {}

  static bool classof(const Constraint *C) {
    return C->getKind() == C_Eq;
  }

  void print(llvm::raw_ostream &O) const {
    lhs->print(O);
    O << " == ";
    rhs->print(O);
  }

  void dump(void) const {
    print(llvm::errs());
  }

  void dump_json(llvm::raw_ostream &O) const {
    O << "{\"Eq\":{\"Atom1\":";
    lhs->dump_json(O);
    O << ", \"Atom2\":";
    rhs->dump_json(O);
    O << "}}";
  }

  Atom *getLHS(void) const { return lhs; }
  Atom *getRHS(void) const { return rhs; }

  bool operator==(const Constraint &Other) const {
    if (const Eq *E = llvm::dyn_cast<Eq>(&Other))
      return *lhs == *E->lhs && *rhs == *E->rhs;
    else
      return false;
  }

  bool operator!=(const Constraint &Other) const {
    return !(*this == Other);
  }

  bool operator<(const Constraint &Other) const {
    ConstraintKind K = Other.getKind();
    if (K == C_Eq) {
      const Eq *E = llvm::dyn_cast<Eq>(&Other);
      assert(E != nullptr && "Invalid type.");

      if (*lhs == *E->lhs && *rhs == *E->rhs)
        return false;
      else if (*lhs == *E->lhs && *rhs != *E->rhs)
        return *rhs < *E->rhs;
      else
        return *lhs < *E->lhs;
    }
    else 
      return C_Eq < K;
  }

  bool containsConstraint(VarAtom *ToFind) {
    return lhs->containsConstraint(ToFind) || rhs->containsConstraint(ToFind);
  }

private:
  Atom *lhs;
  Atom *rhs;
};

// Not a
class Not : public Constraint {
public:
  Not(Constraint *b)
    : Constraint(C_Not), body(b) {}

  static bool classof(const Constraint *C) {
    return C->getKind() == C_Not;
  }

  void print(llvm::raw_ostream &O) const {
    O << "~(";
    body->print(O);
    O << ")";
  }

  void dump(void) const {
    print(llvm::errs());
  }

  void dump_json(llvm::raw_ostream &O) const {
    O << "{\"Not\":";
    body->dump_json(O);
    O << "}";
  }

  bool operator==(const Constraint &Other) const {
    if (const Not *N = llvm::dyn_cast<Not>(&Other))
      return *body == *N->body;
    else
      return false;
  }

  bool operator!=(const Constraint &Other) const {
    return !(*this == Other);
  }

  bool operator<(const Constraint &Other) const {
    ConstraintKind K = Other.getKind();
    if (K == C_Not) {
      const Not *N = llvm::dyn_cast<Not>(&Other);
      assert(N != nullptr && "Invalid type");

      return *body < *N->body;
    }
    else 
      return C_Not < K;
  }

  Constraint *getBody() const {
    return body;
  }

  bool containsConstraint(VarAtom *ToFind) {
    return body->containsConstraint(ToFind);
  }

private:
  Constraint *body;
};

// a => b
class Implies : public Constraint {
public:

  Implies(Constraint *Premise, Constraint *Conclusion)
    : Constraint(C_Imp), premise(Premise), conclusion(Conclusion) {}

  static bool classof(const Constraint *C) {
    return C->getKind() == C_Imp;
  }

  Constraint *getPremise() const { return premise; }
  Constraint *getConclusion() const { return conclusion; }

  void print(llvm::raw_ostream &O) const {
    premise->print(O);
    O << " => ";
    conclusion->print(O);
  }

  void dump(void) const {
    print(llvm::errs());
  }

  void dump_json(llvm::raw_ostream &O) const {
    O << "{\"Implies\":{\"Premise\":";
    premise->dump_json(O);
    O << ", \"Conclusion\":";
    conclusion->dump_json(O);
    O << "}}";
  }

  bool operator==(const Constraint &Other) const {
    if (const Implies *I = llvm::dyn_cast<Implies>(&Other))
      return *premise == *I->premise && *conclusion == *I->conclusion;
    else 
      return false;
  }

  bool operator!=(const Constraint &Other) const {
    return !(*this == Other);
  }
 
  bool operator<(const Constraint &Other) const {
    ConstraintKind K = Other.getKind();
    if (K == C_Imp) {
      const Implies *I = llvm::dyn_cast<Implies>(&Other);
      assert(I != nullptr && "Invalid type.");

      if (*premise == *I->premise && *conclusion == *I->conclusion)
        return false;
      else if (*premise == *I->premise && *conclusion != *I->conclusion)
        return *conclusion < *I->conclusion;
      else
        return *premise < *I->premise;
    }
    else
      return C_Imp < K;
  }

  bool containsConstraint(VarAtom *ToFind) {
    return premise->containsConstraint(ToFind) ||
           conclusion->containsConstraint(ToFind);
  }

private:
  Constraint *premise;
  Constraint *conclusion;
};

class ConstraintVariable;

class Constraints {
public:
  Constraints();
  ~Constraints();
  // Remove the copy constructor, so we never accidentally copy this thing.
  Constraints(const Constraints& O) = delete;

  typedef std::set<Constraint *, PComp<Constraint *>> ConstraintSet;
  // The environment maps from Vars to Consts (one of Ptr, Arr, Wild).
  typedef std::map<VarAtom *, ConstAtom*, PComp<VarAtom *>> EnvironmentMap;

  // Map from a unique key of a function to its constraint variables.
  typedef std::map<std::string, std::set<ConstraintVariable *>> FuncKeyToConsMap;

  typedef std::map<std::string, std::set<ConstraintVariable *>> NameToConsMap;

  bool addConstraint(Constraint *c);
  // It's important to return these by reference. Programs can have 
  // 10-100-100000 constraints and variables, and copying them each time
  // a client wants to examine the environment is untenable.
  ConstraintSet &getConstraints() { return constraints; }
  EnvironmentMap &getVariables() { return environment; }

  FuncKeyToConsMap &getFuncDeclVarMap() { return FuncDeclConstraints; }
  FuncKeyToConsMap &getFuncDefnVarMap() { return FuncDefnConstraints; }
  std::map<std::string, std::string> &getFuncDefnDeclMap() {
    return FuncDefnDeclKeyMap;
  }
  
  EnvironmentMap &getitypeVarMap() { return itypeConstraintVars; }
  // Solve the system of constraints. Return true in the second position if
  // the system is solved. If the system is solved, the first position is 
  // an empty. If the system could not be solved, the constraints in conflict
  // are returned in the first position.
  // TODO: this functionality is not implemented yet.
  std::pair<ConstraintSet, bool> solve(unsigned &NumOfIter);
  void dump() const;
  void print(llvm::raw_ostream &) const;
  void dump_json(llvm::raw_ostream &) const;

  Eq *createEq(Atom *Lhs, Atom *Rhs);
  Eq *createEq(Atom *Lhs, Atom *Rhs, std::string &Rsn);
  Not *createNot(Constraint *Body);
  Implies *createImplies(Constraint *Premise, Constraint *Conclusion);

  VarAtom *getOrCreateVar(uint32_t V);
  VarAtom *getVar(uint32_t V) const;
  PtrAtom *getPtr() const;
  ArrAtom *getArr() const;
  NTArrAtom *getNTArr() const;
  WildAtom *getWild() const;

  // Reset all constraint variables to Ptrs.
  void resetConstraints();

  // Check the sanity of environment map before solving the constraints.
  bool checkInitialEnvSanity();

private:
  ConstraintSet constraints;
  EnvironmentMap environment;
  // Map of constraint variables, which are identified as itype pointers.
  // These should be the constraint variables of only function parameters
  // or returns.
  EnvironmentMap itypeConstraintVars;

  // Map of function unique key to it declaration FVConstraintVariable.
  FuncKeyToConsMap FuncDeclConstraints;
  // Map of function unique key to it definition FVConstraintVariable.
  FuncKeyToConsMap FuncDefnConstraints;

  template <typename T>
  bool canAssignConst(VarAtom *Src);
  bool step_solve(EnvironmentMap &);
  bool check(Constraint *C);

  bool assignConstToVar(EnvironmentMap::iterator &SrcVar, ConstAtom *C);

  template <typename T>
  bool
  propEq(EnvironmentMap &E, Eq *Dyn, T *A, ConstraintSet &R,
      EnvironmentMap::iterator &CurValLHS);

  template <typename T>
  bool
  propImp(Implies *, T*, ConstraintSet &, ConstAtom *);

  // These atoms can be singletons, so we'll store them in the
  // constraints class.
  PtrAtom *PrebuiltPtr;
  ArrAtom *PrebuiltArr;
  NTArrAtom *PrebuiltNTArr;
  WildAtom *PrebuiltWild;

  // Map that contains the mapping between the unique keys of function
  // definition to its declaration.
  std::map<std::string, std::string> FuncDefnDeclKeyMap;
};

typedef uint32_t ConstraintKey;

#endif
