/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Transformer.cpp
 *
 * Defines the interface for RAM transformation passes.
 *
 ***********************************************************************/

#include "ram/transform/Transformer.h"
#include "Global.h"
#include "ram/Node.h"
#include "ram/Program.h"
#include "ram/TranslationUnit.h"
#include "ram/transform/Meta.h"
#include "reports/DebugReport.h"
#include "reports/ErrorReport.h"
#include "souffle/utility/StringUtil.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace souffle::ram::transform {

bool Transformer::apply(TranslationUnit& translationUnit) {
    const bool debug = translationUnit.global().config().has("debug-report");
    const bool verbose = translationUnit.global().config().has("verbose");
    std::string ramProgStrOld = debug ? toString(translationUnit.getProgram()) : "";

    // invoke the transformation
    auto start = std::chrono::high_resolution_clock::now();
    bool changed = transform(translationUnit);
    auto end = std::chrono::high_resolution_clock::now();

    // invalidate analyses in case the program has changed
    if (changed) {
        translationUnit.invalidateAnalyses();
    }

    // print runtime & change info for transformer in verbose mode
    if (verbose && (!isA<MetaTransformer>(this))) {
        std::string changedString = changed ? "changed" : "unchanged";
        std::cout << getName() << " time: " << std::chrono::duration<double>(end - start).count() << "sec ["
                  << changedString << "]" << std::endl;
    }

    // print program after transformation in debug report
    if (debug) {
        translationUnit.getDebugReport().startSection();
        if (changed) {
            translationUnit.getDebugReport().addCodeSection(getName(), "RAM Program after " + getName(),
                    "ram", ramProgStrOld, toString(translationUnit.getProgram()));

            translationUnit.getDebugReport().endSection(getName(), getName());
        } else {
            translationUnit.getDebugReport().endSection(getName(), getName() + " " + " (unchanged)");
        }
    }

    // abort evaluation of the program if errors were encountered
    if (translationUnit.getErrorReport().getNumErrors() != 0) {
        std::cerr << translationUnit.getErrorReport();
        std::cerr << translationUnit.getErrorReport().getNumErrors()
                  << " errors generated, evaluation aborted" << std::endl;
        exit(EXIT_FAILURE);
    }

    return changed;
}

}  // namespace souffle::ram::transform
