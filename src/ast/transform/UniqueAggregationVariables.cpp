/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file UniqueAggregationVariables.cpp
 *
 ***********************************************************************/

#include "ast/transform/UniqueAggregationVariables.h"
#include "ast/Aggregator.h"
#include "ast/Argument.h"
#include "ast/Program.h"
#include "ast/TranslationUnit.h"
#include "ast/Variable.h"
#include "ast/analysis/Aggregate.h"
#include "ast/utility/Visitor.h"
#include "souffle/utility/StringUtil.h"
#include <set>
#include <vector>

namespace souffle::ast::transform {

/**
 * Renames all local variables of the aggregate to something unique, so that
 *  the scope of the local variable is limited to the body of the aggregate subclause.
 *  This assumes that we have simplified the target expression to a target variable.
 **/
bool UniqueAggregationVariablesTransformer::transform(TranslationUnit& translationUnit) {
    bool changed = false;

    // make variables in aggregates unique
    visit(translationUnit.getProgram(), [&](Clause& clause) {
        // find out if the target expression variable occurs elsewhere in the rule. If so, rename it
        // to avoid naming conflicts
        visit(clause, [&](Aggregator& agg) {
            // get the set of local variables in this aggregate and rename
            // those that occur outside the aggregate
            std::set<std::string> localVariables = analysis::getLocalVariables(translationUnit, clause, agg);
            std::set<std::string> variablesOutsideAggregate =
                    analysis::getVariablesOutsideAggregate(clause, agg);
            for (const std::string& name : localVariables) {
                if (variablesOutsideAggregate.find(name) != variablesOutsideAggregate.end()) {
                    // then this MUST be renamed to avoid scoping issues
                    std::string uniqueName = analysis::findUniqueVariableName(clause, name);
                    visit(agg, [&](Variable& var) {
                        if (var.getName() == name) {
                            var.setName(uniqueName);
                            changed = true;
                        }
                    });
                }
            }
        });
    });
    return changed;
}

}  // namespace souffle::ast::transform
