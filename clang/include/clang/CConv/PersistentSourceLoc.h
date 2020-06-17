//=--PersistentSourceLoc.h----------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This class specifies a location in a source file that persists across
// invocations of the frontend. Given a Decl/Stmt/Expr, the FullSourceLoc
// of that value can be compared with an instance of this class for
// equality. If they are equal, then you can substitute the Decl/Stmt/Expr
// for the instance of this class.
//===----------------------------------------------------------------------===//

#ifndef _PERSISTENT_SOURCE_LOC_H
#define _PERSISTENT_SOURCE_LOC_H
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"

class PersistentSourceLoc {
protected:
  PersistentSourceLoc(std::string f, uint32_t l, uint32_t c) : FileName(f), LineNo(l), ColNo(c),
                                                               isValid(true) {}
  
public:
  PersistentSourceLoc() : FileName(""), LineNo(0), ColNo(0),
                          isValid(false) {}
  std::string getFileName() const { return FileName; }
  uint32_t getLineNo() const { return LineNo; }
  uint32_t getColNo() const { return ColNo; }
  bool valid() { return isValid; }

  bool operator<(const PersistentSourceLoc &o) const {
    if (FileName == o.FileName)
      if (LineNo == o.LineNo)
        if (ColNo == o.ColNo)
          return false;
        else
          return ColNo < o.ColNo;
      else
        return LineNo < o.LineNo;
    else
      return FileName < o.FileName;
  }

  void print(llvm::raw_ostream &O) const {
    O << FileName << ":" << LineNo << ":" << ColNo;
  }

  void dump() const { print(llvm::errs()); }

  static
  PersistentSourceLoc mkPSL(const clang::Decl *D, clang::ASTContext &Context);

  static
  PersistentSourceLoc mkPSL(const clang::Stmt *S, clang::ASTContext &Context);

private:
  // Create a PersistentSourceLoc based on absolute file path
  // from the given SourceRange and SourceLocation.
  static
  PersistentSourceLoc mkPSL(clang::SourceRange SR,
                            clang::SourceLocation SL,
                            clang::ASTContext &Context);
  std::string FileName;
  uint32_t LineNo;
  uint32_t ColNo;
  bool isValid;
};

typedef std::pair<PersistentSourceLoc, PersistentSourceLoc>
PersistentSourceRange;

#endif
