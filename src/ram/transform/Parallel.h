/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Parallel.h
 *
 ***********************************************************************/

#pragma once

#include "ram/Program.h"
#include "ram/TranslationUnit.h"
#include "ram/analysis/Relation.h"
#include "ram/transform/Transformer.h"
#include <string>

namespace souffle::ram::transform {

/**
 * @class ParallelTransformer
 * @brief Transforms IfExists/IndexIfExists/IndexScan/Scan into parallel versions.
 *
 * For example ..
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  QUERY
 *    FOR t0 in A
 *     ...
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * will be rewritten to
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  QUERY
 *    PARALLEL FOR t0 in A
 *     ...
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 */
class ParallelTransformer : public Transformer {
public:
    std::string getName() const override {
        return "ParallelTransformer";
    }

    /**
     * @brief Parallelize operations
     * @param program Program that is transformed
     * @return Flag showing whether the program has been changed by the transformation
     */
    bool parallelizeOperations(Program& program);

protected:
    bool transform(TranslationUnit& translationUnit) override {
        relAnalysis = &translationUnit.getAnalysis<analysis::RelationAnalysis>();
        return parallelizeOperations(translationUnit.getProgram());
    }
    analysis::RelationAnalysis* relAnalysis{nullptr};
};

}  // namespace souffle::ram::transform
