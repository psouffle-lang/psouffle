/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file PartitionBodyLiterals.h
 *
 * Transformation pass to move literals into new clauses
 * if they are independent of remaining literals.
 * E.g. a(x) :- b(x), c(y), d(y), e(z). is transformed into:
 *      - a(x) :- b(x), newrel1(), newrel2().
 *      - newrel1() :- c(y), d(y).
 *      - newrel2() :- e(z).
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

/**
 * Transformation pass to move literals into new clauses
 * if they are independent of remaining literals.
 * E.g. a(x) :- b(x), c(y), d(y), e(z). is transformed into:
 *      - a(x) :- b(x), newrel1(), newrel2().
 *      - newrel1() :- c(y), d(y).
 *      - newrel2() :- e(z).
 */
class PartitionBodyLiteralsTransformer : public Transformer {
public:
    std::string getName() const override {
        return "PartitionBodyLiteralsTransformer";
    }

private:
    PartitionBodyLiteralsTransformer* cloning() const override {
        return new PartitionBodyLiteralsTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
