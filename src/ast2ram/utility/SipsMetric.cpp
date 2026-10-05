/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file SipsMetric.cpp
 *
 * Defines the SipsMetric class, which specifies cost functions for atom orderings in a clause.
 *
 ***********************************************************************/

#include "ast2ram/utility/SipsMetric.h"
#include "Global.h"
#include "ast/Clause.h"
#include "ast/TranslationUnit.h"
#include "ast/Variable.h"
#include "ast/analysis/IOType.h"
#include "ast/analysis/SCCGraph.h"
#include "ast/utility/BindingStore.h"
#include "ast/utility/Utils.h"
#include "ast/utility/Visitor.h"
#include "ast2ram/utility/Utils.h"
#include "souffle/utility/StringUtil.h"
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

namespace souffle::ast {

SipsMetric::SipsMetric(const TranslationUnit& tu) : program(tu.getProgram()) {
    sccGraph = &tu.getAnalysis<ast::analysis::SCCGraphAnalysis>();
}

std::vector<std::size_t> SipsMetric::getReorderingWithInitialBindings(const Clause* clause,
        const std::vector<std::string>& atomNames,
        const std::vector<std::string>& initialBoundVars) const {
    (void)initialBoundVars;
    return getReordering(clause, atomNames);
}

std::vector<std::size_t> StaticSipsMetric::getReordering(
        const Clause* clause, const std::vector<std::string>& atomNames) const {
    return getReorderingWithInitialBindings(clause, atomNames, {});
}

std::vector<std::size_t> StaticSipsMetric::getReorderingWithInitialBindings(
        const Clause* clause, const std::vector<std::string>& atomNames,
        const std::vector<std::string>& initialBoundVars) const {
    std::size_t relStratum = sccGraph->getSCC(program.getRelation(*clause));
    auto sccRelations = sccGraph->getInternalRelations(relStratum);

    auto sccAtoms = filter(ast::getBodyLiterals<ast::Atom>(*clause),
            [&](auto* atom) { return contains(sccRelations, program.getRelation(*atom)); });

    BindingStore bindingStore(clause);
    for (const auto& varName : initialBoundVars) {
        bindingStore.bindVariableStrongly(varName);
    }
    auto atoms = getBodyLiterals<Atom>(*clause);
    std::vector<std::size_t> newOrder(atoms.size());

    std::size_t numAdded = 0;
    while (numAdded < atoms.size()) {
        // grab the index of the next atom, based on the SIPS function
        const auto& costs = evaluateCosts(atoms, bindingStore, atomNames);
        assert(atoms.size() == costs.size() && "each atom should have exactly one cost");
        std::size_t minIdx = static_cast<std::size_t>(
                std::distance(costs.begin(), std::min_element(costs.begin(), costs.end())));
        const auto* nextAtom = atoms[minIdx];
        assert(nextAtom != nullptr && "nullptr atoms should have maximal cost");

        // set all arguments that are variables as bound
        for (const auto* arg : nextAtom->getArguments()) {
            if (const auto* var = as<Variable>(arg)) {
                bindingStore.bindVariableStrongly(var->getName());
            }
        }

        newOrder[numAdded] = minIdx;  // add to the ordering
        atoms[minIdx] = nullptr;      // mark as done
        numAdded++;                   // move on
    }

    return newOrder;
}

/** Create a SIPS metric based on a given heuristic. */
std::unique_ptr<SipsMetric> SipsMetric::create(const std::string& heuristic, const TranslationUnit& tu) {
    if (heuristic == "strict")
        return mk<StrictSips>(tu);
    else if (heuristic == "all-bound")
        return mk<AllBoundSips>(tu);
    else if (heuristic == "naive")
        return mk<NaiveSips>(tu);
    else if (heuristic == "max-bound")
        return mk<MaxBoundSips>(tu);
    else if (heuristic == "delta-max-bound")
        return mk<DeltaMaxBoundSips>(tu);
    else if (heuristic == "max-ratio")
        return mk<MaxRatioSips>(tu);
    else if (heuristic == "least-free")
        return mk<LeastFreeSips>(tu);
    else if (heuristic == "least-free-vars")
        return mk<LeastFreeVarsSips>(tu);
    else if (heuristic == "input")
        return mk<InputSips>(tu);

    // default is all-bound
    return create("all-bound", tu);
}

std::vector<double> StrictSips::evaluateCosts(const std::vector<Atom*> atoms,
        const BindingStore& /* bindingStore */, const std::vector<std::string>& /*atomNames*/) const {
    // Goal: Always choose the left-most atom
    std::vector<double> cost;
    for (const auto* atom : atoms) {
        cost.push_back(atom == nullptr ? std::numeric_limits<double>::max() : 0);
    }
    assert(atoms.size() == cost.size() && "each atom should have exactly one cost");
    return cost;
}

std::vector<double> AllBoundSips::evaluateCosts(const std::vector<Atom*> atoms,
        const BindingStore& bindingStore, const std::vector<std::string>& /*atomNames*/) const {
    // Goal: Prioritise atoms with all arguments bound
    std::vector<double> cost;
    for (const auto* atom : atoms) {
        if (atom == nullptr) {
            cost.push_back(std::numeric_limits<double>::max());
            continue;
        }

        std::size_t arity = atom->getArity();
        std::size_t numBound = bindingStore.numBoundArguments(atom);
        cost.push_back(arity == numBound ? 0 : 1);
    }
    assert(atoms.size() == cost.size() && "each atom should have exactly one cost");
    return cost;
}

std::vector<double> NaiveSips::evaluateCosts(const std::vector<Atom*> atoms, const BindingStore& bindingStore,
        const std::vector<std::string>& /*atomNames*/) const {
    // Goal: Prioritise (1) all bound, then (2) atoms with at least one bound argument, then (3) left-most
    std::vector<double> cost;
    for (const auto* atom : atoms) {
        if (atom == nullptr) {
            cost.push_back(std::numeric_limits<double>::max());
            continue;
        }

        std::size_t arity = atom->getArity();
        std::size_t numBound = bindingStore.numBoundArguments(atom);
        if (arity == numBound) {
            cost.push_back(0);
        } else if (numBound >= 1) {
            cost.push_back(1);
        } else {
            cost.push_back(2);
        }
    }
    assert(atoms.size() == cost.size() && "each atom should have exactly one cost");
    return cost;
}

std::vector<double> MaxBoundSips::evaluateCosts(const std::vector<Atom*> atoms,
        const BindingStore& bindingStore, const std::vector<std::string>& /*atomNames*/) const {
    // Goal: prioritise (1) all-bound, then (2) max number of bound vars, then (3) left-most
    std::vector<double> cost;
    for (const auto* atom : atoms) {
        if (atom == nullptr) {
            cost.push_back(std::numeric_limits<double>::max());
            continue;
        }

        std::size_t arity = atom->getArity();
        std::size_t numBound = bindingStore.numBoundArguments(atom);
        if (arity == numBound) {
            // Always better than anything else
            cost.push_back(0);
        } else if (numBound == 0) {
            // Always worse than any number of bound vars
            cost.push_back(2);
        } else {
            // Between 0 and 1, decreasing with more num bound
            cost.push_back(1.0 / numBound);
        }
    }
    assert(atoms.size() == cost.size() && "each atom should have exactly one cost");
    return cost;
}

std::vector<double> MaxRatioSips::evaluateCosts(const std::vector<Atom*> atoms,
        const BindingStore& bindingStore, const std::vector<std::string>& /*atomNames*/) const {
    // Goal: prioritise max ratio of bound args
    std::vector<double> cost;
    for (const auto* atom : atoms) {
        if (atom == nullptr) {
            cost.push_back(std::numeric_limits<double>::max());
            continue;
        }

        std::size_t arity = atom->getArity();
        std::size_t numBound = bindingStore.numBoundArguments(atom);
        if (arity == 0) {
            // Always better than anything else
            cost.push_back(0);
        } else if (numBound == 0) {
            // Always worse than anything else
            cost.push_back(2);
        } else {
            // Between 0 and 1, decreasing as the ratio increases
            cost.push_back(1.0 - numBound / arity);
        }
    }
    assert(atoms.size() == cost.size() && "each atom should have exactly one cost");
    return cost;
}

std::vector<double> LeastFreeSips::evaluateCosts(const std::vector<Atom*> atoms,
        const BindingStore& bindingStore, const std::vector<std::string>& /*atomNames*/) const {
    // Goal: choose the atom with the least number of unbound arguments
    std::vector<double> cost;
    for (const auto* atom : atoms) {
        if (atom == nullptr) {
            cost.push_back(std::numeric_limits<double>::max());
            continue;
        }

        cost.push_back((double)(atom->getArity() - bindingStore.numBoundArguments(atom)));
    }
    return cost;
}

std::vector<double> LeastFreeVarsSips::evaluateCosts(const std::vector<Atom*> atoms,
        const BindingStore& bindingStore, const std::vector<std::string>& /*atomNames*/) const {
    // Goal: choose the atom with the least amount of unbound variables
    std::vector<double> cost;
    for (const auto* atom : atoms) {
        if (atom == nullptr) {
            cost.push_back(std::numeric_limits<double>::max());
            continue;
        }

        // use a set to hold all free variables to avoid double-counting
        std::set<std::string> freeVars;
        visit(*atom, [&](const Variable& var) {
            if (bindingStore.isBound(var.getName())) {
                freeVars.insert(var.getName());
            }
        });
        cost.push_back((double)freeVars.size());
    }
    return cost;
}

InputSips::InputSips(const TranslationUnit& tu)
        : StaticSipsMetric(tu), ioTypes(tu.getAnalysis<analysis::IOTypeAnalysis>()) {}

std::vector<double> InputSips::evaluateCosts(const std::vector<Atom*> atoms, const BindingStore& bindingStore,
        const std::vector<std::string>& /*atomNames*/) const {
    // Goal: prioritise (1) all-bound, (2) input, then (3) rest
    std::vector<double> cost;
    for (const auto* atom : atoms) {
        if (atom == nullptr) {
            cost.push_back(std::numeric_limits<double>::max());
            continue;
        }

        const auto& relName = atom->getQualifiedName();
        std::size_t arity = atom->getArity();
        std::size_t numBound = bindingStore.numBoundArguments(atom);
        if (arity == numBound) {
            // prioritise all-bound
            cost.push_back(0);
        } else if (ioTypes.isInput(program.getRelation(relName))) {
            // then input
            cost.push_back(1);
        } else {
            cost.push_back(2);
        }
    }
    return cost;
}

std::vector<double> DeltaMaxBoundSips::evaluateCosts(const std::vector<Atom*> atoms,
        const BindingStore& bindingStore, const std::vector<std::string>& atomNames) const {
    std::vector<double> cost;
    for (std::size_t i = 0; i < atoms.size(); ++i) {
        const auto* atom = atoms[i];
        if (atom == nullptr) {
            cost.push_back(std::numeric_limits<double>::max());
            continue;
        }

        std::size_t arity = atom->getArity();
        std::size_t numBound = bindingStore.numBoundArguments(atom);
        if (arity == numBound) {
            // Always better than anything else
            cost.push_back(0.0);
        } else if (isPrefix("@delta_", atomNames[i])) {
            // Better than any other atom that is not fully bounded
            cost.push_back(1.0);
        } else if (numBound == 0) {
            // Always worse than any number of bound vars
            cost.push_back(4.0);
        } else {
            // Between 2 and 3, decreasing with more num bound
            cost.push_back(2.0 + (1.0 / (double)numBound));
        }
    }
    assert(atoms.size() == cost.size() && "each atom should have exactly one cost");
    return cost;
}

}  // namespace souffle::ast
