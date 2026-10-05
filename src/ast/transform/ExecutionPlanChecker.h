/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ExecutionPlanChecker.h
 *
 * Defines the execution plan checker pass.
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

class ExecutionPlanChecker : public Transformer {
public:
    std::string getName() const override {
        return "ExecutionPlanChecker";
    }

private:
    ExecutionPlanChecker* cloning() const override {
        return new ExecutionPlanChecker();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
