/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ReportIndex.h
 *
 ***********************************************************************/

#pragma once

#include "ram/Condition.h"
#include "ram/Expression.h"
#include "ram/Operation.h"
#include "ram/Program.h"
#include "ram/TranslationUnit.h"
#include "ram/analysis/Complexity.h"
#include "ram/analysis/Index.h"
#include "ram/analysis/Level.h"
#include "ram/transform/Transformer.h"
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace souffle::ram::transform {

/**
 * @class ReportIndexSetsTransformer
 * @brief does not transform the program but reports on the index sets
 *        if the debug-report flag is enabled.
 *
 */
class ReportIndexTransformer : public Transformer {
public:
    std::string getName() const override {
        return "ReportIndexTransformer";
    }

protected:
    bool transform(TranslationUnit& translationUnit) override {
        translationUnit.getAnalysis<analysis::IndexAnalysis>();
        return false;
    }
};

}  // namespace souffle::ram::transform
