/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file MaterializeAggregationQueries.h
 *
 * Transformation pass to create artificial relations for bodies of
 * aggregation functions consisting of more than a single atom.
 *
 ***********************************************************************/

#pragma once

#include "ast/Aggregator.h"
#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <set>
#include <string>

namespace souffle::ast {
class Clause;
}

namespace souffle::ast::transform {
/**
 * Transformation pass to create artificial relations for bodies of
 * aggregation functions consisting of more than a single atom.
 */
class MaterializeAggregationQueriesTransformer : public Transformer {
public:
    std::string getName() const override {
        return "MaterializeAggregationQueriesTransformer";
    }

    /**
     * Creates artificial relations for bodies of aggregation functions
     * consisting of more than a single atom, in the given program.
     *
     * @param program the program to be processed
     * @return whether the program was modified
     */
    static bool materializeAggregationQueries(TranslationUnit& translationUnit);

private:
    MaterializeAggregationQueriesTransformer* cloning() const override {
        return new MaterializeAggregationQueriesTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override {
        return materializeAggregationQueries(translationUnit);
    }
    /**
     *  Modify the aggClause by adding in grounding literals for every
     *  variable that appears in the clause ungrounded. The source of literals
     *  to copy from is the originalClause.
     **/
    static void groundInjectedParameters(const TranslationUnit& translationUnit, Clause& aggClause,
            const Clause& originalClause, const Aggregator& aggregate);

    /**
     * A test determining whether the body of a given aggregation needs to be
     * 'outlined' into an independent relation or can be kept inline.
     */
    static bool needsMaterializedRelation(const Aggregator& agg);
    /**
     *  Whatever variables have been left unnamed
     *  have significance for a count aggregate. They NEED to
     *  be in the head of the materialized atom that will replace
     *  this aggregate subclause. Therefore, we give them
     *  dummy names (_n rather than just _) so that the count
     *  will be correct.
     **/
    static void instantiateUnnamedVariables(Clause& aggClause);
    /**
     * When we materialise an aggregate subclause,
     * it's a good question which variables belong in the
     * head of that materialised clause and which don't.
     *
     * For a count aggregate for example, it's important that
     * all variables (even unnamed ones) appearing in the subclause are given a space
     * in the head. For min and max aggregates, we know that the entire subclause
     * (as is the case for any aggregate) will need to appear as the body of the materialised rule,
     * but what goes in the head is a bit less straightforward. We could get away with only
     * exporting the column that we are taking the maximum over, because this is the only value
     * that we need to retrieve. The same can be said for sum and mean.
     *
     * BUT there is a caveat: We support the construct of witnesses to an aggregate calculation.
     * A witness is some variable present in the aggregate subclause that is exported
     * *ungrounded* to the base scope. If we do save all configurations of the variables
     * in the aggregate subclause (as we are doing when we materialise it), then we can
     * reuse this materialised subclause to retrieve the witness almost instantly.
     *
     * (This is assuming that we have a good enough machinery to recognise when
     * rules are exactly the same, because the witness transformation actually has to take place BEFORE
     * the materialisation, meaning that there will be two separate relations that actually will represent
     * exactly the same relation semantically, and we would want those to be recognised as a single relation)
     *
     * So not only then (assuming we have good "equality of rules" machinery) does using the "count"
     * aggregate technique (just giving each variable a place in the head) allow us to save and retrieve
     * witnesses easily, but it also just makes the treatment of aggregates uniform, which I suppose is
     * simpler, nicer, more aesthetic maybe.
     *
     * Giving min/max/mean/sum aggregate materialised relations only a single column would be far more
     * efficient, but then we would lose the ability to retrieve witnesses. So this was the tradeoff, and
     * losing witnesses is just not an option. We NEED them to be able to give the user witness functionality.
     *
     * To clarify, we don't want to include local variables of any inner aggregates appearing in this
     * aggregate. My reasoning for deciding this was really just a hunch that I felt like local variables of
     *an inner aggregate should be totally irrelevant. So we include "immediate" local variables, not local
     * variables of any inner aggregate.
     *
     * The other thing we must include is any injected variables. Depending on the assignment of the injected
     * variable, the aggregate's value may change, but so, still, why would it be important to include the
     * injected variable in the head then?
     *
     *       A(x, n) :- B(x), n = sum y : { C(y, x) }.
     *
     * Because we need the assignment of x in the outer scope to COINCIDE with the assignment of x in the
     * inner scope. The only thing visible to the outer scope is whatever is in the head. That is good enough
     * justification. And this applies to any type of aggregate.
     *
     *  So the overall conclusion is that what we should include are:
     *      * injected variables
     *      * *immediate* local variables
     **/
    static std::set<std::string> distinguishHeadArguments(
            const TranslationUnit& tu, const Clause& clause, const Aggregator& aggregate);
};

}  // namespace souffle::ast::transform
