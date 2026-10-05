/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2022, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file SipGraph.h
 *
 * Defines the sideways information passing (SIP) graph class
 *
 ***********************************************************************/

#pragma once

#include "ast/Clause.h"
#include "ast/Constant.h"
#include "ram/Expression.h"
#include "souffle/utility/StreamUtil.h"
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace souffle::ast {

class SipGraph {
public:
    using AstConstantTranslator = std::function<Own<ram::Expression>(const ast::Constant& constant)>;

    SipGraph(const Clause* clause, AstConstantTranslator translateConstant)
            : clause(clause), translateConstant(translateConstant) {
        computeBindings();
    }

    using VarName = std::string;
    using VarSet = std::set<VarName>;
    using ArgIdx = std::size_t;
    using AtomIdx = std::size_t;
    using AtomSet = std::set<std::size_t>;

    void computeBindings();

    std::set<std::size_t> getBoundIndices(const std::set<const Atom*>& from, const Atom* to) const;

    std::set<std::set<std::size_t>> getPossibleBoundIndices(const Atom* to) const;

    std::map<std::size_t, const ram::Expression*> getConstantsMap(const Atom* atom) const {
        return atomConstantsMap.at(atom);
    }

    std::set<std::size_t> getUnnamedIndices(const Atom* atom) const {
        return unnamedMap.at(atom);
    }

private:
    // the clause to compute the SIP graph for
    const Clause* clause;

    // user provided lambda function to convert AST constant to RAM expression
    AstConstantTranslator translateConstant;

    // map atom to the variables it grounds
    std::unordered_map<const Atom*, VarSet> atomToGroundedVars;

    // map variables to necessary variables on other side of the equality
    // i.e. x = y + z we should map x -> { y, z }
    std::unordered_map<VarName, VarSet> varToOtherVars;

    // map atom to the constants bound at each index
    std::map<const Atom*, std::map<std::size_t, const ram::Expression*>> atomConstantsMap;

    // hold the memory for each translated constant
    VecOwn<ram::Expression> constants;

    // map atom to the number of unnamed arguments
    std::map<const Atom*, std::set<std::size_t>> unnamedMap;
};

}  // namespace souffle::ast
