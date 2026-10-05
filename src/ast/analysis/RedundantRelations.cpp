/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file RedundantRelations.cpp
 *
 * Implements method of precedence graph to build the precedence graph,
 * compute strongly connected components of the precedence graph, and
 * build the strongly connected component graph.
 *
 ***********************************************************************/

#include "ast/analysis/RedundantRelations.h"
#include "GraphUtils.h"
#include "ast/Node.h"
#include "ast/Program.h"
#include "ast/Relation.h"
#include "ast/TranslationUnit.h"
#include "ast/analysis/IOType.h"
#include "ast/analysis/PrecedenceGraph.h"
#include "souffle/utility/StreamUtil.h"
#include <set>
#include <vector>

namespace souffle::ast::analysis {

void RedundantRelationsAnalysis::run(const TranslationUnit& translationUnit) {
    precedenceGraph = &translationUnit.getAnalysis<PrecedenceGraphAnalysis>();

    RelationSet work;
    RelationSet notRedundant;
    auto& ioType = translationUnit.getAnalysis<IOTypeAnalysis>();
    Program& program = translationUnit.getProgram();

    const std::vector<Relation*>& relations = program.getRelations();
    /* Add all output relations to the work set */
    for (const Relation* r : relations) {
        if (ioType.isOutput(r)) {
            work.insert(r);
        }
    }

    /* Find all relations which are not redundant for the computations of the
       output relations. */
    while (!work.empty()) {
        /* Chose one element in the work set and add it to notRedundant */
        const Relation* u = *(work.begin());
        work.erase(work.begin());
        notRedundant.insert(u);

        /* Find all predecessors of u and add them to the worklist
            if they are not in the set notRedundant */
        for (const Relation* predecessor : precedenceGraph->graph().predecessors(u)) {
            if (notRedundant.count(predecessor) == 0u) {
                work.insert(predecessor);
            }
        }
    }

    /* All remaining relations are redundant. */
    redundantRelations.clear();
    for (const Relation* r : relations) {
        if (notRedundant.count(r) == 0u) {
            redundantRelations.insert(r->getQualifiedName());
        }
    }
}

void RedundantRelationsAnalysis::print(std::ostream& os) const {
    os << redundantRelations << std::endl;
}

}  // namespace souffle::ast::analysis
