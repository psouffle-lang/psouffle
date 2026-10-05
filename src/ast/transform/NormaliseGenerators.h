/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file NormaliseGenerators.h
 *
 * Transform pass to normalise all appearances of generators.
 * Generators include multi-result functors + aggregators.
 *
 ***********************************************************************/

#pragma once

#include "ast/transform/Transformer.h"

namespace souffle::ast::transform {

/** Uniquely names all appearances of generators */
class NormaliseGeneratorsTransformer : public Transformer {
public:
    std::string getName() const override {
        return "NormaliseGeneratorsTransformer";
    }

private:
    bool transform(TranslationUnit& translationUnit) override;

    NormaliseGeneratorsTransformer* cloning() const override {
        return new NormaliseGeneratorsTransformer();
    }
};

}  // namespace souffle::ast::transform
