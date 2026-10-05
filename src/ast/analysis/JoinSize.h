/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2022, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file JoinSize.h

 * Computes for every stratum, which EstimateJoinSize nodes to emit in the RAM
 * This is useful for accumulating selectivity statistics
 *
 ***********************************************************************/

#pragma once

#include "ast/Program.h"
#include "ast/Relation.h"
#include "ast/TranslationUnit.h"
#include "ast/analysis/RecursiveClauses.h"
#include "ast/analysis/SCCGraph.h"
#include "ast/analysis/TopologicallySortedSCCGraph.h"
#include "ast/analysis/typesystem/PolymorphicObjects.h"
#include "ast/utility/Visitor.h"
#include "ast2ram/ClauseTranslator.h"
#include "ast2ram/utility/Utils.h"
#include "ram/EstimateJoinSize.h"
#include "ram/Expression.h"
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace souffle::ast::analysis {

/**
 * Analysis pass computing a schedule for computing relations.
 */
using StratumJoinSizeEstimates = std::vector<Own<ram::EstimateJoinSize>>;

class JoinSizeAnalysis : public Analysis {
public:
    static constexpr const char* name = "join-size";

    JoinSizeAnalysis() : Analysis(name) {}

    void run(const TranslationUnit& translationUnit) override;

    /** Dump this relation schedule to standard error. */
    void print(std::ostream& os) const override;

    const StratumJoinSizeEstimates& getJoinSizeStatementsInSCC(std::size_t scc) const {
        return joinSizeStatements[scc];
    }

private:
    std::vector<StratumJoinSizeEstimates> joinSizeStatements;

    std::set<std::string> seenNodes;
    ast::Program* program = nullptr;
    SCCGraphAnalysis* sccGraph = nullptr;
    TopologicallySortedSCCGraphAnalysis* topsortSCCGraphAnalysis = nullptr;
    RecursiveClausesAnalysis* recursiveClauses = nullptr;
    PolymorphicObjectsAnalysis* polyAnalysis = nullptr;

    // for each stratum compute the EstimateJoinSize nodes to emit
    std::vector<StratumJoinSizeEstimates> computeJoinSizeStatements(bool emitStatistics);
    StratumJoinSizeEstimates computeRuleVersionStatements(const RelationSet& sccRelations,
            const ast::Clause& clause, std::size_t version,
            ast2ram::TranslationMode mode = ast2ram::TranslationMode::DEFAULT);
};

}  // namespace souffle::ast::analysis
