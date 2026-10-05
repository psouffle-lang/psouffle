/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file BindingStore.cpp
 *
 * Defines the BindingStore class, which can be used to dynamically
 * determine the set of bound variables within a given clause.
 *
 ***********************************************************************/

#include "ast/utility/BindingStore.h"
#include "ast/Aggregator.h"
#include "ast/Atom.h"
#include "ast/BinaryConstraint.h"
#include "ast/Clause.h"
#include "ast/Constant.h"
#include "ast/RecordInit.h"
#include "ast/Term.h"
#include "ast/Variable.h"
#include "ast/utility/Visitor.h"
#include "souffle/BinaryConstraintOps.h"
#include <algorithm>
#include <cassert>
#include <vector>

namespace souffle::ast {

BindingStore::BindingStore(const Clause* clause) {
    generateBindingDependencies(clause);
    reduceDependencies();
}

void BindingStore::generateBindingDependencies(const Clause* clause) {
    // Grab all relevant constraints (i.e. eq. constrs not involving aggregators)
    std::set<const BinaryConstraint*> relevantEqConstraints;
    visit(*clause, [&](const BinaryConstraint& bc) {
        bool containsAggregators = false;
        visit(bc, [&](const Aggregator& /* aggr */) { containsAggregators = true; });
        if (!containsAggregators && isEqConstraint(bc.getBaseOperator())) {
            relevantEqConstraints.insert(&bc);
        }
    });

    // Add variable binding dependencies implied by the constraint
    for (const auto* eqConstraint : relevantEqConstraints) {
        processEqualityBindings(eqConstraint->getLHS(), eqConstraint->getRHS());
        processEqualityBindings(eqConstraint->getRHS(), eqConstraint->getLHS());
    }
}

void BindingStore::processEqualityBindings(const Argument* lhs, const Argument* rhs) {
    // Only care about equalities affecting the bound status of variables
    const auto* var = as<Variable>(lhs);
    if (var == nullptr) return;

    // If all variables on the rhs are bound, then lhs is also bound
    BindingStore::ConjBindingSet depSet;
    visit(*rhs, [&](const Variable& subVar) { depSet.insert(subVar.getName()); });
    addBindingDependency(var->getName(), depSet);

    // If the lhs is bound, then all args in the rec on the rhs are also bound
    if (const auto* rec = as<RecordInit>(rhs)) {
        for (const auto* arg : rec->getArguments()) {
            const auto* subVar = as<Variable>(arg);
            assert(subVar != nullptr && "expected args to be variables");
            addBindingDependency(subVar->getName(), BindingStore::ConjBindingSet({var->getName()}));
        }
    }
}

BindingStore::ConjBindingSet BindingStore::reduceDependency(
        const BindingStore::ConjBindingSet& origDependency) {
    BindingStore::ConjBindingSet newDependency;
    for (const auto& var : origDependency) {
        // Only keep unbound variables in the dependency
        if (!contains(stronglyBoundVariables, var)) {
            newDependency.insert(var);
        }
    }
    return newDependency;
}

BindingStore::DisjBindingSet BindingStore::reduceDependency(
        const BindingStore::DisjBindingSet& origDependency) {
    BindingStore::DisjBindingSet newDependencies;
    for (const auto& dep : origDependency) {
        auto newDep = reduceDependency(dep);
        if (!newDep.empty()) {
            newDependencies.insert(newDep);
        }
    }
    return newDependencies;
}

bool BindingStore::reduceDependencies() {
    bool changed = false;
    std::map<std::string, BindingStore::DisjBindingSet> newVariableDependencies;
    std::set<std::string> variablesToBind;

    // Reduce each variable's set of dependencies one by one
    for (const auto& [headVar, dependencies] : variableDependencies) {
        // No need to track the dependencies of already-bound variables
        if (contains(stronglyBoundVariables, headVar)) {
            changed = true;
            continue;
        }

        // Reduce the dependency set based on bound variables
        auto newDependencies = reduceDependency(dependencies);
        if (newDependencies.empty() || newDependencies.size() < dependencies.size()) {
            // At least one dependency has been satisfied, so variable is now bound
            changed = true;
            variablesToBind.insert(headVar);
            continue;
        }
        newVariableDependencies[headVar] = newDependencies;
        changed |= (newDependencies != dependencies);
    }

    // Bind variables that need to be bound
    for (auto var : variablesToBind) {
        stronglyBoundVariables.insert(var);
    }

    // Repeat it recursively if any changes happened, until we reach a fixpoint
    if (changed) {
        variableDependencies = newVariableDependencies;
        reduceDependencies();
        return true;
    }
    assert(variableDependencies == newVariableDependencies && "unexpected change");
    return false;
}

bool BindingStore::isBound(const Argument* arg) const {
    if (const auto* var = as<Variable>(arg)) {
        return isBound(var->getName());
    } else if (const auto* term = as<Term>(arg)) {
        for (const auto* subArg : term->getArguments()) {
            if (!isBound(subArg)) {
                return false;
            }
        }
        return true;
    } else if (isA<Constant>(arg)) {
        return true;
    } else {
        return false;
    }
}

std::size_t BindingStore::numBoundArguments(const Atom* atom) const {
    std::size_t count = 0;
    for (const auto* arg : atom->getArguments()) {
        if (isBound(arg)) {
            count++;
        }
    }
    return count;
}
}  // namespace souffle::ast
