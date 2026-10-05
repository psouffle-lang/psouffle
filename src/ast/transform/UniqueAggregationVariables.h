/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file UniqueAggregationVariables.h
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

/**
 * Transformation pass to rename aggregation variables to make them unique.
 */
class UniqueAggregationVariablesTransformer : public Transformer {
public:
    std::string getName() const override {
        return "UniqueAggregationVariablesTransformer";
    }

private:
    UniqueAggregationVariablesTransformer* cloning() const override {
        return new UniqueAggregationVariablesTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
