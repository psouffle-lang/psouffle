/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file SCCGraph.cpp
 *
 * Implements method of precedence graph to build the precedence graph,
 * compute strongly connected components of the precedence graph, and
 * build the strongly connected component graph.
 *
 ***********************************************************************/

#include "ast/analysis/SCCGraph.h"
#include "Global.h"
#include "GraphUtils.h"
#include "ast/Program.h"
#include "ast/QualifiedName.h"
#include "ast/Relation.h"
#include "ast/TranslationUnit.h"
#include "ast/analysis/IOType.h"
#include "souffle/utility/StreamUtil.h"
#include <algorithm>
#include <cassert>
#include <memory>
#include <set>
#include <string>

namespace souffle::ast::analysis {

void SCCGraphAnalysis::run(const TranslationUnit& translationUnit) {
    precedenceGraph = &translationUnit.getAnalysis<PrecedenceGraphAnalysis>();
    ioType = &translationUnit.getAnalysis<IOTypeAnalysis>();
    programName = translationUnit.global().config().get("name");
    sccToRelation.clear();
    relationToScc.clear();
    predecessors.clear();
    successors.clear();

    /* Compute SCC */
    Program& program = translationUnit.getProgram();
    std::vector<Relation*> relations = program.getRelations();
    std::size_t counter = 0;
    std::size_t numSCCs = 0;
    std::stack<const Relation*> S;
    std::stack<const Relation*> P;
    std::map<const Relation*, std::size_t> preOrder;  // Pre-order number of a node (for Gabow's Algo)
    for (const Relation* relation : relations) {
        relationToScc[relation] = preOrder[relation] = (std::size_t)-1;
    }
    for (const Relation* relation : relations) {
        if (preOrder[relation] == (std::size_t)-1) {
            scR(relation, preOrder, counter, S, P, numSCCs);
        }
    }

    /* Build SCC graph */
    successors.resize(numSCCs);
    predecessors.resize(numSCCs);
    for (const Relation* u : relations) {
        for (const Relation* v : precedenceGraph->graph().predecessors(u)) {
            auto scc_u = relationToScc[u];
            auto scc_v = relationToScc[v];
            assert(scc_u < numSCCs && "Wrong range");
            assert(scc_v < numSCCs && "Wrong range");
            if (scc_u != scc_v) {
                predecessors[scc_u].insert(scc_v);
                successors[scc_v].insert(scc_u);
            }
        }
    }

    /* Store the relations for each SCC */
    sccToRelation.resize(numSCCs);
    for (const Relation* relation : relations) {
        sccToRelation[relationToScc[relation]].insert(relation);
    }
}

/* Compute strongly connected components using Gabow's algorithm (cf. Algorithms in
 * Java by Robert Sedgewick / Part 5 / Graph *  algorithms). The algorithm has linear
 * runtime. */
void SCCGraphAnalysis::scR(const Relation* w, std::map<const Relation*, std::size_t>& preOrder,
        std::size_t& counter, std::stack<const Relation*>& S, std::stack<const Relation*>& P,
        std::size_t& numSCCs) {
    preOrder[w] = counter++;
    S.push(w);
    P.push(w);
    for (const Relation* t : precedenceGraph->graph().predecessors(w)) {
        if (preOrder[t] == (std::size_t)-1) {
            scR(t, preOrder, counter, S, P, numSCCs);
        } else if (relationToScc[t] == (std::size_t)-1) {
            while (preOrder[P.top()] > preOrder[t]) {
                P.pop();
            }
        }
    }
    if (P.top() == w) {
        P.pop();
    } else {
        return;
    }

    const Relation* v;
    do {
        v = S.top();
        S.pop();
        relationToScc[v] = numSCCs;
    } while (v != w);
    numSCCs++;
}

void SCCGraphAnalysis::printRaw(std::stringstream& ss) const {
    const std::string& name = programName;
    /* Print SCC graph */
    ss << "digraph {" << std::endl;
    /* Print nodes of SCC graph */
    for (std::size_t scc = 0; scc < getNumberOfSCCs(); scc++) {
        ss << "\t" << name << "_" << scc << "[label = \"";
        ss << join(getInternalRelations(scc), ",\\n",
                [](std::ostream& out, const Relation* rel) { out << rel->getQualifiedName(); });
        ss << "\" ];" << std::endl;
    }
    for (std::size_t scc = 0; scc < getNumberOfSCCs(); scc++) {
        for (auto succ : getSuccessorSCCs(scc)) {
            ss << "\t" << name << "_" << scc << " -> " << name << "_" << succ << ";" << std::endl;
        }
    }
    ss << "}";
}

void SCCGraphAnalysis::print(std::ostream& os) const {
    std::stringstream ss;
    printRaw(ss);
    os << ss.str();
}

void SCCGraphAnalysis::printHTML(std::ostream& os) const {
    std::stringstream ss;
    printRaw(ss);
    printHTMLGraph(os, ss.str(), getName());
}

}  // namespace souffle::ast::analysis
