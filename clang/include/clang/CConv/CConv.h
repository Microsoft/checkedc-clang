//=--CConv.h------------------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The main interface for invoking cconv or checked-c-convert tool.
// This provides various methods that can be used to access different
// aspects of the cconv tool.
//
//===----------------------------------------------------------------------===//

#ifndef _CCONV_H
#define _CCONV_H

#include "ConstraintVariables.h"
#include "PersistentSourceLoc.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "CConvInteractiveData.h"
#include "ProgramInfo.h"
#include <mutex>

// Options used to initialize CConv tool.
struct CConvertOptions {
  bool DumpIntermediate;

  bool Verbose;

  bool SeperateMultipleFuncDecls;

  std::string OutputPostfix;

  std::string ConstraintOutputJson;

  bool DumpStats;

  bool HandleVARARGS;

  bool EnablePropThruIType;

  bool ConsiderAllocUnsafe;

  std::string BaseDir;

  bool EnableAllTypes;

  bool AddCheckedRegions;
};

// The main interface exposed by the CConv to interact with the tool.
class CConvInterface {

public:
  ProgramInfo GlobalProgramInfo;
  // Mutex for this interface.
  std::mutex InterfaceMutex;

  CConvInterface(const struct CConvertOptions &CCopt,
                 const std::vector<std::string> &SourceFileList,
                 clang::tooling::CompilationDatabase *CompDB);

  // Constraint Building.

  // Build initial constraints.
  bool BuildInitialConstraints();

  // Constraint Solving.
  bool SolveConstraints();

  // Interactivity.

  // Get all the WILD pointers and corresponding reason why they became WILD.
  DisjointSet &GetWILDPtrsInfo();

  // Given a constraint key make the corresponding constraint var
  // to be non-WILD.
  bool MakeSinglePtrNonWild(ConstraintKey targetPtr);

  // Make the provided pointer non-WILD and also make all the
  // pointers, which are wild because of the same reason, as non-wild
  // as well.
  bool InvalidateWildReasonGlobally(ConstraintKey PtrKey);

  // Rewriting.

  // Write all converted versions of the files in the source file list
  // to disk
  bool WriteAllConvertedFilesToDisk();
  // Write the current converted state of the provided file.
  bool WriteConvertedFileToDisk(const std::string &FilePath);

private:
  // Are constraints already built?
  bool ConstraintsBuilt;
  void ResetAllPointerConstraints();
  void InvalidateAllConstraintsWithReason(Constraint *ConstraintToRemove);

};

#endif // _CCONV_H
