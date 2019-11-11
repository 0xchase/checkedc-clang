//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//
#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"

#include <algorithm>

#include "Constraints.h"

#include "ConstraintBuilder.h"
#include "ProgramInfo.h"
#include "MappingVisitor.h"
#include "RewriteUtils.h"
#include "ArrayBoundsInferenceConsumer.h"
#include "IterativeItypeHelper.h"
#include "CConvInteractive.h"

using namespace clang::driver;
using namespace clang::tooling;
using namespace clang;
using namespace llvm;

static cl::OptionCategory ConvertCategory("checked-c-convert options");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp("");

bool DumpIntermediate;

bool Verbose;

bool mergeMultipleFuncDecls;

std::string OutputPostfix;

std::string ConstraintOutputJson;

bool DumpStats;

bool handleVARARGS;

bool enablePropThruIType;

bool considerAllocUnsafe;

std::string BaseDir;


template <typename T, typename V>
class GenericAction : public ASTFrontendAction {
public:
  GenericAction(V &I) : Info(I) {}

  virtual std::unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &Compiler, StringRef InFile) {
    return std::unique_ptr<ASTConsumer>(new T(Info, &Compiler.getASTContext()));
  }

private:
  V &Info;
};

template <typename T, typename V, typename U>
class RewriteAction : public ASTFrontendAction {
public:
  RewriteAction(V &I, U &P) : Info(I),Files(P) {}

  virtual std::unique_ptr<ASTConsumer>
    CreateASTConsumer(CompilerInstance &Compiler, StringRef InFile) {
    return std::unique_ptr<ASTConsumer>
      (new T(Info, Files, &Compiler.getASTContext(), OutputPostfix, BaseDir));
  }

private:
  V &Info;
  U &Files;
};

template <typename T>
std::unique_ptr<FrontendActionFactory>
newFrontendActionFactoryA(ProgramInfo &I) {
  class ArgFrontendActionFactory : public FrontendActionFactory {
  public:
    explicit ArgFrontendActionFactory(ProgramInfo &I) : Info(I) {}

    FrontendAction *create() override { return new T(Info); }

  private:
    ProgramInfo &Info;
  };

  return std::unique_ptr<FrontendActionFactory>(
      new ArgFrontendActionFactory(I));
}

template <typename T>
std::unique_ptr<FrontendActionFactory>
newFrontendActionFactoryB(ProgramInfo &I, std::set<std::string> &PS) {
  class ArgFrontendActionFactory : public FrontendActionFactory {
  public:
    explicit ArgFrontendActionFactory(ProgramInfo &I,
      std::set<std::string> &PS) : Info(I),Files(PS) {}

    FrontendAction *create() override { return new T(Info, Files); }

  private:
    ProgramInfo &Info;
    std::set<std::string> &Files;
  };

  return std::unique_ptr<FrontendActionFactory>(
    new ArgFrontendActionFactory(I, PS));
}


std::pair<Constraints::ConstraintSet, bool> solveConstraintsWithFunctionSubTyping(ProgramInfo &Info) {
// solve the constrains by handling function sub-typing.
  Constraints &CS = Info.getConstraints();
  unsigned numIterations = 0;
  std::pair<Constraints::ConstraintSet, bool> toRet;
  bool fixed = false;
  while (!fixed) {
    toRet = CS.solve(numIterations);
    if (numIterations > 1)
      // this means we have made some changes to the environment
      // see if the function subtype handling causes any changes?
      fixed = !Info.handleFunctionSubtyping();
    else
      // we reached a fixed point.
      fixed = true;
  }
  return toRet;
}

bool performIterativeItypeRefinement(Constraints &CS, ProgramInfo &Info,
                                     ClangTool &Tool, std::set<std::string> &inoutPaths) {
  bool fixedPointReached = false;
  unsigned long iterationNum = 1;
  unsigned long numberOfEdgesRemoved = 0;
  unsigned long numItypeVars = 0;
  std::set<std::string> modifiedFunctions;
  if(Verbose) {
    errs() << "Trying to capture Constraint Variables for all functions\n";
  }
  // first capture itype parameters and return values for all functions
  performConstraintSetup(Info);

  // sanity check
  assert(CS.checkInitialEnvSanity() && "Invalid initial environment. We expect all pointers to be "
                                       "initialized with Ptr to begin with.");

  while(!fixedPointReached) {
    clock_t startTime = clock();
    errs() << "****Iteration " << iterationNum << " starts.****\n";
    if(Verbose) {
      errs() << "Iterative Itype refinement, Round:" << iterationNum << "\n";
    }

    std::pair<Constraints::ConstraintSet, bool> R = solveConstraintsWithFunctionSubTyping(Info);

    errs() << "Iteration:" << iterationNum << ", Constraint solve time:" << getTimeSpentInSeconds(startTime) << "\n";

    if(R.second) {
      errs() << "Constraints solved for iteration:" << iterationNum << "\n";
    }

    if(DumpStats) {
      Info.print_stats(inoutPaths, llvm::errs(), true);
    }

    // get all the functions whose constraints have been modified.
    identifyModifiedFunctions(CS, modifiedFunctions);

    startTime = clock();
    // detect and update new found itype vars.
    numItypeVars = detectAndUpdateITypeVars(Info, modifiedFunctions);

    errs() << "Iteration:" << iterationNum << ", Number of detected itype vars:" << numItypeVars
           << ", detection time:" << getTimeSpentInSeconds(startTime) << "\n";

    startTime = clock();
    // update the constraint graph by removing edges from/to iype parameters and returns.
    numberOfEdgesRemoved = resetWithitypeConstraints(CS);

    errs() << "Iteration:" << iterationNum << ", Number of edges removed:" << numberOfEdgesRemoved << "\n";

    errs() << "Iteration:" << iterationNum << ", Refinement Time:" << getTimeSpentInSeconds(startTime) << "\n";

    // if we removed any edges, that means we did not reach fix point.
    // In other words, we reach fixed point when no edges are removed from the constraint graph.
    fixedPointReached = !(numberOfEdgesRemoved > 0);
    errs() << "****Iteration " << iterationNum << " ends****\n";
    iterationNum++;
  }

  errs() << "Fixed point reached after " << (iterationNum-1) << " iterations.\n";

  return fixedPointReached;
}

static std::set<std::string> inoutPaths;

static CompilationDatabase *CurrCompDB = nullptr;

static ProgramInfo Info;

static tooling::CommandLineArguments sourceFiles;

bool initializeCConvert(CommonOptionsParser &OptionsParser, struct CConvertOptions &options) {

  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();

  DumpIntermediate = options.DumpIntermediate;

  Verbose = options.Verbose;

  mergeMultipleFuncDecls = options.mergeMultipleFuncDecls;

  OutputPostfix = options.OutputPostfix;

  ConstraintOutputJson = options.ConstraintOutputJson;

  DumpStats = options.DumpStats;

  handleVARARGS = options.handleVARARGS;

  enablePropThruIType = options.enablePropThruIType;

  considerAllocUnsafe = options.considerAllocUnsafe;

  BaseDir = options.BaseDir;

  if (BaseDir.empty()) {
    SmallString<256>  cp;
    if (std::error_code ec = sys::fs::current_path(cp)) {
      errs() << "could not get current working dir\n";
      return false;
    }

    BaseDir = cp.str();
  }

  sourceFiles = OptionsParser.getSourcePathList();

  for (const auto &S : sourceFiles) {
    std::string abs_path;
    if (getAbsoluteFilePath(S, abs_path))
      inoutPaths.insert(abs_path);
  }

  CurrCompDB = &(OptionsParser.getCompilations());

  if (OutputPostfix == "-" && inoutPaths.size() > 1) {
    errs() << "If rewriting more than one , can't output to stdout\n";
    return false;
  }
  return true;
}

bool buildInitialConstraints() {
  ClangTool Tool(*CurrCompDB, sourceFiles);

  // 1. Gather constraints.
  std::unique_ptr<ToolAction> ConstraintTool = newFrontendActionFactoryA<
      GenericAction<ConstraintBuilderConsumer, ProgramInfo>>(Info);

  if (ConstraintTool)
    Tool.run(ConstraintTool.get());
  else
    llvm_unreachable("No action");

  if (!Info.link()) {
    errs() << "Linking failed!\n";
    return false;
  }

  // 2. Solve constraints.
  if (Verbose)
    outs() << "Solving constraints\n";

  Constraints &CS = Info.getConstraints();

  // perform constraint solving by iteratively refining based on itypes.
  bool fPointReached = performIterativeItypeRefinement(CS, Info, Tool, inoutPaths);

  assert(fPointReached);
  if (Verbose)
    outs() << "Constraints solved\n";
  if (DumpIntermediate) {
    Info.dump();
    Info.computePointerDisjointSet();
    outs() << "Writing json output to:" << ConstraintOutputJson << "\n";
    std::error_code ec;
    llvm::raw_fd_ostream output_json(ConstraintOutputJson, ec);
    if (!output_json.has_error()) {
      Info.dump_json(output_json);
      output_json.close();
    } else {
      Info.dump_json(llvm::errs());
    }
  }
  return true;
}

DisjointSet& getWILDPtrsInfo() {
  return Info.getPointerConstraintDisjointSet();
}

static void resetAllPointerConstraints() {
  Constraints::EnvironmentMap &currEnvMap = Info.getConstraints().getVariables();
  for (auto &CV : currEnvMap) {
    CV.first->resetErasedConstraints();
  }
}

bool makeSinglePtrNonWild(ConstraintKey targetPtr) {

  std::vector<ConstraintKey> removePtrs;
  removePtrs.clear();
  CVars oldWILDPtrs = Info.getPointerConstraintDisjointSet().allWildPtrs;

  resetAllPointerConstraints();

  VarAtom *VA = Info.getConstraints().getOrCreateVar(targetPtr);
  Constraints::EnvironmentMap toRemoveVAtoms;
  toRemoveVAtoms[VA] = Info.getConstraints().getWild();

  VA->replaceEqConstraints(toRemoveVAtoms, Info.getConstraints());

  ClangTool Tool(*CurrCompDB, sourceFiles);
  performIterativeItypeRefinement(Info.getConstraints(), Info, Tool, inoutPaths);
  Info.computePointerDisjointSet();

  CVars &newWILDPtrs = Info.getPointerConstraintDisjointSet().allWildPtrs;

  std::set_difference(oldWILDPtrs.begin(), oldWILDPtrs.end(),
                      newWILDPtrs.begin(),
                      newWILDPtrs.end(), removePtrs.begin());

  return !removePtrs.empty();
}


bool invalidateWildReasonGlobally(ConstraintKey targetPtr) {

  std::vector<ConstraintKey> removePtrs;
  removePtrs.clear();
  CVars oldWILDPtrs = Info.getPointerConstraintDisjointSet().allWildPtrs;

  resetAllPointerConstraints();

  VarAtom *VA = Info.getConstraints().getOrCreateVar(targetPtr);
  Constraints::EnvironmentMap toRemoveVAtoms;
  toRemoveVAtoms[VA] = Info.getConstraints().getWild();

  VA->replaceEqConstraints(toRemoveVAtoms, Info.getConstraints());

  ClangTool Tool(*CurrCompDB, sourceFiles);
  performIterativeItypeRefinement(Info.getConstraints(), Info, Tool, inoutPaths);
  Info.computePointerDisjointSet();

  CVars &newWILDPtrs = Info.getPointerConstraintDisjointSet().allWildPtrs;

  std::set_difference(oldWILDPtrs.begin(), oldWILDPtrs.end(),
                      newWILDPtrs.begin(),
                      newWILDPtrs.end(), removePtrs.begin());

  return !removePtrs.empty();
}

int originalmain(int argc, const char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);

  // Initialize targets for clang module support.
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  if (BaseDir.size() == 0) {
    SmallString<256>  cp;
    if (std::error_code ec = sys::fs::current_path(cp)) {
      errs() << "could not get current working dir\n";
      return 1;
    }

    BaseDir = cp.str();
  }

  CommonOptionsParser OptionsParser(argc, argv, ConvertCategory);

  tooling::CommandLineArguments args = OptionsParser.getSourcePathList();

  ClangTool Tool(OptionsParser.getCompilations(), args);
  std::set<std::string> inoutPaths;

  for (const auto &S : args) {
    std::string abs_path;
    if (getAbsoluteFilePath(S, abs_path))
      inoutPaths.insert(abs_path);
  }

  if (OutputPostfix == "-" && inoutPaths.size() > 1) {
    errs() << "If rewriting more than one , can't output to stdout\n";
    return 1;
  }

  ProgramInfo Info;

  // 1. Gather constraints.
  std::unique_ptr<ToolAction> ConstraintTool = newFrontendActionFactoryA<
      GenericAction<ConstraintBuilderConsumer, ProgramInfo>>(Info);
  
  if (ConstraintTool)
    Tool.run(ConstraintTool.get());
  else
    llvm_unreachable("No action");

  if (!Info.link()) {
    errs() << "Linking failed!\n";
    return 1;
  }

  // 2. Solve constraints.
  if (Verbose)
    outs() << "Solving constraints\n";

  Constraints &CS = Info.getConstraints();

  // perform constraint solving by iteratively refining based on itypes.
  bool fPointReached = performIterativeItypeRefinement(CS, Info, Tool, inoutPaths);

  assert(fPointReached == true);
  if (Verbose)
    outs() << "Constraints solved\n";
  if (DumpIntermediate) {
    Info.dump();
    outs() << "Writing json output to:" << ConstraintOutputJson << "\n";
    std::error_code ec;
    llvm::raw_fd_ostream output_json(ConstraintOutputJson, ec);
    if (!output_json.has_error()) {
      Info.dump_json(output_json);
      output_json.close();
    } else {
      Info.dump_json(llvm::errs());
    }
  }

  // 3. Re-write based on constraints.
  std::unique_ptr<ToolAction> RewriteTool =
      newFrontendActionFactoryB
      <RewriteAction<RewriteConsumer, ProgramInfo, std::set<std::string>>>(
          Info, inoutPaths);
  
  if (RewriteTool)
    Tool.run(RewriteTool.get());
  else
    llvm_unreachable("No action");

  if (DumpStats)
    Info.dump_stats(inoutPaths);

  return 0;
}
