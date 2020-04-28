//=--IterativeItypeHelper.cpp-------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Implementation of IterativeItypeHelper methods.
//===----------------------------------------------------------------------===//

#include "IterativeItypeHelper.h"

// Map that stored the newly detected itype parameters and
// returns that are detected in this iteration.
static Constraints::EnvironmentMap currIterationItypeMap;
// Map that contains the constraint variables of parameters
// and its return for all functions (including their declarations
// and definitions).
// This map is used to determine new detection of itypes.
static std::map<std::string, std::map<VarAtom*, ConstAtom*>>
    funcParamsReturnSavedValues;

// This method saves the constraint vars of parameters and return of all the
// provided FVConstraint vars with default value as null.
// These are used to later check if anything has changed, in which case the
// corresponding function will be considered as modified.
static void
updateFunctionConstraintVars(std::string funcUniqKey,
                             Constraints &CS,
                             std::set<ConstraintVariable*> &fvconstraintVars) {
  for (auto topVar: fvconstraintVars) {
    // If this is a function constraint?
    if (FVConstraint *fvCons = dyn_cast<FVConstraint>(topVar)) {
      // Update the variables of function parameters.
      for (unsigned i = 0; i < fvCons->numParams(); i++) {
        for (ConstraintVariable *paramVar: fvCons->getParamVar(i)) {
          assert(dyn_cast<PVConstraint>(paramVar) && "Expected a pointer "
                                                     "variable constraint.");
          PVConstraint *pvConst = dyn_cast<PVConstraint>(paramVar);
          for (auto cVar: pvConst->getCvars()) {
            VarAtom *currVarAtom = CS.getVar(cVar);
            // Default value is null.
            funcParamsReturnSavedValues[funcUniqKey][currVarAtom] = nullptr;
          }
        }
      }
      // Now, update the variables of function return vars.
      for (ConstraintVariable *returnVar: fvCons->getReturnVars()) {
        assert(dyn_cast<PVConstraint>(returnVar) && "Expected a pointer "
                                                    "variable constraint.");
        PVConstraint *retVarConst = dyn_cast<PVConstraint>(returnVar);
        for (auto cVar: retVarConst->getCvars()) {
          VarAtom *currVarAtom = CS.getVar(cVar);
          // The default value is also null here.
          funcParamsReturnSavedValues[funcUniqKey][currVarAtom] = nullptr;
        }
      }
    }
  }
}

bool identifyModifiedFunctions(Constraints &CS,
                               std::set<std::string> &modifiedFunctions) {
  modifiedFunctions.clear();
  // Get the current values.
  Constraints::EnvironmentMap &currEnvMap = CS.getVariables();
  // Check to see if they differ from previous values.
  for (auto &prevFuncVals: funcParamsReturnSavedValues) {
    std::string funcDefKey = prevFuncVals.first;
    for (auto &currVar: prevFuncVals.second) {
      // Check if the value of the constraint variable changed?
      // Then we consider the corresponding function as modified.
      if (currEnvMap[currVar.first] != currVar.second) {
        currVar.second = currEnvMap[currVar.first];
        modifiedFunctions.insert(funcDefKey);
      }
    }
  }
  return !modifiedFunctions.empty();
}

unsigned long resetWithitypeConstraints(Constraints &CS) {
  Constraints::EnvironmentMap backupDeclConstraints;
  backupDeclConstraints.clear();
  Constraints::EnvironmentMap &currEnvMap = CS.getVariables();
  unsigned long numConstraintsRemoved = 0;

  Constraints::EnvironmentMap toRemoveVAtoms;

  // Restore the erased constraints and try to remove constraints that
  // depend on ityped constraint variables.

  // Make a map of constraints to remove.
  for (auto &currITypeVar: currIterationItypeMap) {
    ConstAtom *targetCons = currITypeVar.second;
    if (!dyn_cast<NTArrAtom>(currITypeVar.second)) {
      targetCons = nullptr;
    }
    toRemoveVAtoms[currITypeVar.first] = targetCons;
  }

  // Now try to remove the constraints.
  for (auto &currE: currEnvMap) {
    currE.first->resetErasedConstraints();
    numConstraintsRemoved +=
        currE.first->replaceEqConstraints(toRemoveVAtoms, CS);
  }

  // Check if we removed any constraints?
  if (numConstraintsRemoved > 0) {
    // We removed constraints; reset everything.

    // Backup the computed results of declaration parameters and returns.
    for (auto &currITypeVar: CS.getitypeVarMap()) {
      backupDeclConstraints[currITypeVar.first] =
          currEnvMap[CS.getVar(currITypeVar.first->getLoc())];
    }

    // Reset all constraints to Ptrs.
    CS.resetConstraints();

    // Restore the precomputed constraints for declarations.
    for (auto &currITypeVar: backupDeclConstraints) {
      currEnvMap[CS.getVar(currITypeVar.first->getLoc())] = currITypeVar.second;
    }
  }

  return numConstraintsRemoved;

}

// This method updates the pointer type of the declaration constraint variable
// with the type of the definition constraint variable.
static bool updateDeclWithDefnType(ConstraintVariable *decl,
                                   ConstraintVariable *defn,
                                   ProgramInfo &Info) {
  Constraints &CS = Info.getConstraints();
  bool changesHappened = false;
  // Get the itype map where we store the pointer type of the
  // declaration constraint variables.
  Constraints::EnvironmentMap &itypeMap = CS.getitypeVarMap();
  PVConstraint *PVDeclCons = dyn_cast<PVConstraint>(decl);
  PVConstraint *PVDefnCons = dyn_cast<PVConstraint>(defn);

  // These has to be pointer constraint variables.
  assert(PVDeclCons != nullptr && PVDefnCons != nullptr &&
         "Expected a pointer variable constraint for function "
         "parameter but got nullptr");

  ConstAtom *itypeAtom = nullptr;

  // Get the pointer type of the definition constraint variable.
  for (ConstraintKey k: PVDefnCons->getCvars()) {
    itypeAtom = CS.getVariables()[CS.getVar(k)];
  }
  assert(itypeAtom != nullptr && "Unable to find assignment for "
                                 "definition constraint variable.");

  // Check if this is already identified itype.
  for (ConstraintKey k: PVDeclCons->getCvars()) {
    VarAtom *cK = CS.getVar(k);
    if (itypeMap.find(cK) != itypeMap.end() && itypeMap[cK] == itypeAtom) {
        //Yes, then no need to do anything.
        return changesHappened;
    }
  }

  // Update the type of the declaration constraint variable.
  for (ConstraintKey k: PVDeclCons->getCvars()) {
    VarAtom *cK = CS.getVar(k);
    itypeMap[cK] = itypeAtom;
    currIterationItypeMap[cK] = itypeAtom;
    changesHappened = true;
  }
  return changesHappened;
}

unsigned long
detectAndUpdateITypeVars(ProgramInfo &Info,
                         std::set<std::string> &modifiedFunctions) {
  Constraints &CS = Info.getConstraints();
  unsigned long numITypeVars = 0;
  // Clear the current iteration itype vars.
  currIterationItypeMap.clear();
  for (auto funcDefKey: modifiedFunctions) {
    FVConstraint *cDefn =
        dyn_cast<FVConstraint>(getHighest(CS.getFuncDefnVarMap()[funcDefKey],
                                          Info));

    auto declConstraintsPtr = Info.getFuncDeclConstraintSet(funcDefKey);
    assert(declConstraintsPtr != nullptr && "This cannot be nullptr, if it was "
                                            "null, we would never have "
                                            "inserted this info modified "
                                            "functions.");
    FVConstraint *cDecl =
        dyn_cast<FVConstraint>(getHighest(*declConstraintsPtr, Info));

    assert(cDecl != nullptr);
    assert(cDefn != nullptr);

    if (cDecl->numParams() == cDefn->numParams()) {
      // Compare parameters.
      for (unsigned i = 0; i < cDecl->numParams(); ++i) {
        auto Decl = getHighest(cDecl->getParamVar(i), Info);
        auto Defn = getHighest(cDefn->getParamVar(i), Info);
        assert(Decl);
        assert(Defn);

        // If this holds, then we want to insert a bounds safe interface.
        bool anyConstrained =
            Defn->anyChanges(Info.getConstraints().getVariables());
        // Definition is more precise than declaration so declaration
        // has to be WILD.
        if (anyConstrained && Decl->hasWild(CS.getVariables()) &&
            updateDeclWithDefnType(Decl, Defn, Info)) {
          numITypeVars++;
        }
      }

    }


    // Compare returns.
    auto Decl = getHighest(cDecl->getReturnVars(), Info);
    auto Defn = getHighest(cDefn->getReturnVars(), Info);


    bool anyConstrained =
        Defn->anyChanges(Info.getConstraints().getVariables());
    if (anyConstrained) {
      // Definition is more precise than declaration so declaration
      // has to be WILD.
      if (Decl->hasWild(CS.getVariables()) &&
          updateDeclWithDefnType(Decl, Defn, Info)) {
        numITypeVars++;
      }
    }
  }
  return numITypeVars;
}

bool performConstraintSetup(ProgramInfo &Info) {
  bool hasSome = false;
  Constraints &CS = Info.getConstraints();
  for (auto &currFDef: CS.getFuncDefnVarMap()) {
    // Get the key for the function definition.
    auto funcDefKey = currFDef.first;
    std::set<ConstraintVariable *> &defCVars = currFDef.second;

    std::set<ConstraintVariable *> *declCVarsPtr =
        Info.getFuncDeclConstraintSet(funcDefKey);

    if (declCVarsPtr != nullptr) {
      // Okay, we have constraint variables for declaration there could be a
      // possibility of itypes. Lets save the var atoms.
      updateFunctionConstraintVars(funcDefKey, CS, defCVars);
      hasSome = true;
    }
  }
  return hasSome;
}

