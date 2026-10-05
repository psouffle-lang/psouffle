/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file AddNullariesToAtomlessAggregates.h
 *
 * Transformation pass to create artificial relations for bodies of
 * aggregation functions consisting of more than a single atom.
 *
 * Transformation pass to add artificial nullary atom (+Tautology())
 * to aggregate bodies that have no atoms. This is because the RAM expects
 * all aggregates to refer to a relation.
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

/**
 * Transformation pass to add artificial nullary atom (+Tautology())
 * to aggregate bodies that have no atoms.
 */
class AddNullariesToAtomlessAggregatesTransformer : public Transformer {
public:
    std::string getName() const override {
        return "AddNullariesToAtomlessAggregatesTransformer";
    }

private:
    AddNullariesToAtomlessAggregatesTransformer* cloning() const override {
        return new AddNullariesToAtomlessAggregatesTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
