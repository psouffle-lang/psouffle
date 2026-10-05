/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file MaterializeSingletonAggregation.h
 *
 * Replaces literals containing single-valued aggregates with
 * a synthesised relation
 *
 ***********************************************************************/

#pragma once

#include "ast/Aggregator.h"
#include "ast/Clause.h"
#include "ast/Program.h"
#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

/**
 * Replaces literals containing single-valued aggregates with
 * a synthesised relation
 */
class MaterializeSingletonAggregationTransformer : public Transformer {
public:
    std::string getName() const override {
        return "MaterializeSingletonAggregationTransformer";
    }

private:
    MaterializeSingletonAggregationTransformer* cloning() const override {
        return new MaterializeSingletonAggregationTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
    /**
     * Determines whether an aggregate is single-valued,
     * ie the aggregate does not depend on the outer scope.
     */
    static bool isSingleValued(const TranslationUnit& tu, const Aggregator& agg, const Clause& clause);
    /**
     *  Modify the aggClause by adding in grounding literals for every
     *  variable that appears in the clause ungrounded. The source of literals
     *  to copy from is the originalClause.
     **/
    void groundInjectedParameters(
            const TranslationUnit& translationUnit, Clause& aggClause, const Clause& originalClause);
};

}  // namespace souffle::ast::transform
