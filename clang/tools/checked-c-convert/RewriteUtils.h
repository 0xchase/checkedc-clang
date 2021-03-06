//=--RewriteUtils.h-----------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This class contains functions and classes that deal with
// rewriting the source file after converting to CheckedC format.
//===----------------------------------------------------------------------===//

#ifndef _REWRITEUTILS_H
#define _REWRITEUTILS_H

#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/ASTContext.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include "ProgramInfo.h"

using namespace clang;

// A Declaration, optional DeclStmt, and a replacement string
// for that Declaration.
struct DAndReplace
{
    Decl        *Declaration; // The declaration to replace.
    Stmt        *Statement;   // The Stmt, if it exists.
    std::string Replacement;  // The string to replace the declaration with.
    bool        fullDecl;     // If the declaration is a function, true if
    // Replace the entire declaration or just the
    // return declaration.
    DAndReplace() : Declaration(nullptr),
                    Statement(nullptr),
                    Replacement(""),
                    fullDecl(false) { }

    DAndReplace(Decl *D, std::string R) : Declaration(D),
                                          Statement(nullptr),
                                          Replacement(R),
                                          fullDecl(false) {}

    DAndReplace(Decl *D, std::string R, bool F) : Declaration(D),
                                                  Statement(nullptr),
                                                  Replacement(R),
                                                  fullDecl(F) {}


    DAndReplace(Decl *D, Stmt *S, std::string R) :  Declaration(D),
                                                    Statement(S),
                                                    Replacement(R),
                                                    fullDecl(false) { }
};

// Compare two DAndReplace values. The algorithm for comparing them relates
// their source positions. If two DAndReplace values refer to overlapping
// source positions, then they are the same. Otherwise, they are ordered
// by their placement in the input file.
//
// There are two special cases: Function declarations, and DeclStmts. In turn:
//
//  - Function declarations might either be a DAndReplace describing the entire
//    declaration, i.e. replacing "int *foo(void)"
//    with "int *foo(void) : itype(_Ptr<int>)". Or, it might describe just
//    replacing only the return type, i.e. "_Ptr<int> foo(void)". This is
//    discriminated against with the 'fullDecl' field of the DAndReplace type
//    and the comparison function first checks if the operands are
//    FunctionDecls and if the 'fullDecl' field is set.
//  - A DeclStmt of mupltiple Decls, i.e. 'int *a = 0, *b = 0'. In this case,
//    we want the DAndReplace to refer only to the specific sub-region that
//    would be replaced, i.e. '*a = 0' and '*b = 0'. To do that, we traverse
//    the Decls contained in a DeclStmt and figure out what the appropriate
//    source locations are to describe the positions of the independent
//    declarations.
struct DComp
{
    SourceManager &SM;
    DComp(SourceManager &S) : SM(S) { }

    SourceRange getWholeSR(SourceRange Orig, DAndReplace Dr) const;

    bool operator()(const DAndReplace Lhs, const DAndReplace Rhs) const;
};

typedef std::set<DAndReplace, DComp> RSet;

void rewrite(ParmVarDecl *PV, Rewriter &R, std::string SRewrite);

void rewrite( VarDecl               *VD,
              Rewriter              &R,
              std::string SRewrite,
              Stmt                  *WhereStmt,
              RSet                  &skip,
              const DAndReplace     &N,
              RSet                  &ToRewrite,
              ASTContext            &A);

// Visit each Decl in toRewrite and apply the appropriate pointer type
// to that Decl. The state of the rewrite is contained within R, which
// is both input and output. R is initialized to point to the 'main'
// source file for this transformation. toRewrite contains the set of
// declarations to rewrite. S is passed for source-level information
// about the current compilation unit. skip indicates some rewrites that
// we should skip because we already applied them, for example, as part
// of turning a single line declaration into a multi-line declaration.
void rewrite( Rewriter              &R,
              RSet                  &ToRewrite,
              RSet                  &Skip,
              SourceManager         &S,
              ASTContext            &A,
              std::set<FileID>      &Files);

// Class that handles rewriting bounds information for all the
// detected array variables.
class ArrayBoundsRewriter {
public:
  ArrayBoundsRewriter(ASTContext *C, ProgramInfo &I): Context(C), Info(I) {}
  // Compute the possible bounds for all the array variables.
  void computeArrayBounds();
  // Get the string representation of the bounds for the given variable.
  std::string getBoundsString(Decl *D, bool Isitype = false);
private:
  ASTContext *Context;
  ProgramInfo &Info;
};

// Class for visiting declarations during re-writing to find locations to
// insert casts. Right now, it looks specifically for 'free'.
class CastPlacementVisitor : public RecursiveASTVisitor<CastPlacementVisitor> {
public:
  explicit CastPlacementVisitor(ASTContext *C, ProgramInfo &I,
                                RSet &DR, std::set<std::string> &V,
                                std::map<std::string, std::string> &newFuncSig,
                                ArrayBoundsRewriter &ArrRewriter)
          : Context(C), Info(I), rewriteThese(DR), VisitedSet(V),
            ModifiedFuncSignatures(newFuncSig), ABRewriter(ArrRewriter) {}

  bool VisitCallExpr(CallExpr *);
  bool VisitFunctionDecl(FunctionDecl *);
  bool isFunctionVisited(std::string FName);
private:
  std::set<unsigned int> getParamsForExtern(std::string);
  // Get existing itype string from constraint variables.
  // If tries to get the string from declaration, however,
  // if there is no declaration of the function,
  // it will try to get it from the definition.
  std::string getExistingIType(ConstraintVariable *Declc,
                               ConstraintVariable *Defc,
                               FunctionDecl *FuncDecl);
  bool anyTop(std::set<ConstraintVariable *>);
  ASTContext            *Context;
  ProgramInfo           &Info;
  RSet                  &rewriteThese;
  std::set<std::string> &VisitedSet;
  std::map<std::string, std::string> &ModifiedFuncSignatures;
  ArrayBoundsRewriter   &ABRewriter;
};


class RewriteConsumer : public ASTConsumer {
public:
  explicit RewriteConsumer(ProgramInfo &I,
                           std::set<std::string> &F, ASTContext *Context,
                           std::string &OPostfix, std::string &bDir) :
                           Info(I), InOutFiles(F), OutputPostfix(OPostfix),
                           BaseDir(bDir) {}

  virtual void HandleTranslationUnit(ASTContext &Context);

private:
  // Functions to handle modified signatures and ensuring that we always use
  // the latest signature.
  static std::string getModifiedFuncSignature(std::string FuncName);
  static bool hasModifiedSignature(std::string FuncName);
  ProgramInfo &Info;
  std::set<std::string> &InOutFiles;
  static std::map<std::string, std::string> ModifiedFuncSignatures;
  std::string &OutputPostfix;
  std::string &BaseDir;
};

#endif //_REWRITEUTILS_H
