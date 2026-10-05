/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file NameUnnamedVariables.h
 *
 * Transformation pass to replace unnamed variables
 * with singletons.
 * E.g.: a() :- b(_). -> a() :- b(x).
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

/**
 * Transformation pass to replace unnamed variables
 * with singletons.
 * E.g.: a() :- b(_). -> a() :- b(x).
 */
class NameUnnamedVariablesTransformer : public Transformer {
public:
    std::string getName() const override {
        return "NameUnnamedVariablesTransformer";
    }

private:
    NameUnnamedVariablesTransformer* cloning() const override {
        return new NameUnnamedVariablesTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
