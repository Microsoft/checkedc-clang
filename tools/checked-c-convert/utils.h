//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// Type declarations for map data structures.
//===----------------------------------------------------------------------===//
#ifndef _UTILS_H
#define _UTILS_H
#include "llvm/Support/CommandLine.h"
#include "PersistentSourceLoc.h"

class ConstraintVariable;

// Maps a Decl to the first constraint variable for the variable defined by 
// that Decl.
//typedef std::map<clang::Decl*, uint32_t> VariableMap;
typedef std::map<PersistentSourceLoc, 
  std::set<ConstraintVariable*>> VariableMap;
// Maps a constraint variable to the Decl that defines the variable the 
// constraint variable refers to.
//typedef std::map<uint32_t, clang::Decl*> ReverseVariableMap;
//typedef std::map<clang::Decl*, uint32_t> DeclMap;
// Maps a Decl to the DeclStmt that defines the Decl.
typedef std::map<clang::Decl*, clang::DeclStmt*> VariableDecltoStmtMap;

extern llvm::cl::opt<bool> Verbose;
extern llvm::cl::opt<bool> DumpIntermediate;

const clang::Type *getNextTy(const clang::Type *Ty);
#endif
