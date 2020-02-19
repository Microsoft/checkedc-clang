//                     The LLVM Compiler Infrastructure
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Type declarations for map data structures.
//===----------------------------------------------------------------------===//
#ifndef _UTILS_H
#define _UTILS_H
#include <set>
#include "llvm/Support/CommandLine.h"
#include "clang/AST/Type.h"

#include "PersistentSourceLoc.h"

class ConstraintVariable;
class ProgramInfo;

// Maps a Decl to the set of constraint variables for that Decl.
typedef std::map<PersistentSourceLoc, 
  std::set<ConstraintVariable*>> VariableMap;

// Maps a Decl to the DeclStmt that defines the Decl.
typedef std::map<clang::Decl*, clang::DeclStmt*> VariableDecltoStmtMap;

extern llvm::cl::opt<bool> Verbose;
extern llvm::cl::opt<bool> DumpIntermediate;
extern llvm::cl::opt<bool> handleVARARGS;
extern llvm::cl::opt<bool> mergeMultipleFuncDecls;
extern llvm::cl::opt<bool> enablePropThruIType;
extern llvm::cl::opt<bool> considerAllocUnsafe;

const clang::Type *getNextTy(const clang::Type *Ty);

ConstraintVariable *getHighest(std::set<ConstraintVariable*> Vs, ProgramInfo &Info);

clang::FunctionDecl *getDeclaration(clang::FunctionDecl *FD);

clang::FunctionDecl *getDefinition(clang::FunctionDecl *FD);

clang::CheckedPointerKind getCheckedPointerKind(clang::InteropTypeExpr *itypeExpr);

bool isDeclarationParam(clang::Decl *param);

std::string getStorageQualifierString(clang::Decl *D);

bool getAbsoluteFilePath(std::string fileName, std::string &absoluteFP);

// get the time spent in seconds since the provided time stamp.
float getTimeSpentInSeconds(clock_t startTime);

// check if the function has varargs i.e., foo(<named_arg>,...)
bool functionHasVarArgs(clang::FunctionDecl *FD);

// check if the function is a allocator.
bool isFunctionAllocator(std::string funcName);

// Is the given variable built  in type?
bool isPointerType(clang::VarDecl *VD);

// check if the variable is of a structure or union type.
bool isStructOrUnionType(clang::VarDecl *VD);

// Helper method to print a Type in a way that can be represented in the source.
std::string tyToStr(const clang::Type *T);

clang::SourceLocation getFunctionDeclarationEnd(clang::FunctionDecl *FD, clang::SourceManager &S);
#endif
