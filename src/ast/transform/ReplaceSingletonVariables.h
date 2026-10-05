/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ReplaceSingletonVariables.h
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

/**
 * Transformation pass to replace singleton variables
 * with unnamed variables.
 * E.g.: a() :- b(x). -> a() :- b(_).
 */
class ReplaceSingletonVariablesTransformer : public Transformer {
public:
    std::string getName() const override {
        return "ReplaceSingletonVariablesTransformer";
    }

private:
    ReplaceSingletonVariablesTransformer* cloning() const override {
        return new ReplaceSingletonVariablesTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
