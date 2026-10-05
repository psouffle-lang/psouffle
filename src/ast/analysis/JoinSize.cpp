/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2022, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file JoinSize.cpp
 *
 * EstimateJoinSize statements accumulate relation selectivity statistics.
 * This analysis determines which EstimateJoinSize statements to emit in the RAM
 *
 ***********************************************************************/

#include "ast/analysis/JoinSize.h"
#include "Global.h"
#include "GraphUtils.h"
#include "ast/BinaryConstraint.h"
#include "ast/Constant.h"
#include "ast/NilConstant.h"
#include "ast/QualifiedName.h"
#include "ast/Relation.h"
#include "ast/StringConstant.h"
#include "ast/SubsumptiveClause.h"
#include "ast/UnnamedVariable.h"
#include "ast2ram/utility/SipGraph.h"
#include "ast2ram/utility/Utils.h"
#include "ram/FloatConstant.h"
#include "ram/SignedConstant.h"
#include "ram/StringConstant.h"
#include "ram/UnsignedConstant.h"
#include "souffle/BinaryConstraintOps.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/SubsetCache.h"
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <unordered_map>

namespace souffle::ast::analysis {

analysis::StratumJoinSizeEstimates JoinSizeAnalysis::computeRuleVersionStatements(const RelationSet& scc,
        const ast::Clause& clause, std::size_t version, ast2ram::TranslationMode mode) {
    auto* prog = program;
    auto sccAtoms = filter(ast::getBodyLiterals<ast::Atom>(clause),
            [&](auto* atom) { return contains(scc, prog->getRelation(*atom)); });

    auto* poly = polyAnalysis;
    auto astConstantTranslator = [poly](const ast::Constant& constant) -> Own<ram::Expression> {
        if (auto strConstant = as<ast::StringConstant>(constant)) {
            return mk<ram::StringConstant>(strConstant->getConstant());
        } else if (isA<ast::NilConstant>(&constant)) {
            return mk<ram::SignedConstant>(0);
        } else if (auto* numConstant = as<ast::NumericConstant>(constant)) {
            switch (poly->getInferredType(*numConstant)) {
                case ast::NumericConstant::Type::Int:
                    return mk<ram::SignedConstant>(
                            RamSignedFromString(numConstant->getConstant(), nullptr, 0));
                case ast::NumericConstant::Type::Uint:
                    return mk<ram::UnsignedConstant>(
                            RamUnsignedFromString(numConstant->getConstant(), nullptr, 0));
                case ast::NumericConstant::Type::Float:
                    return mk<ram::FloatConstant>(RamFloatFromString(numConstant->getConstant()));
            }
        }
        fatal("unaccounted-for constant");
        return nullptr;
    };

    analysis::StratumJoinSizeEstimates statements;

    auto getClauseAtomName = [&sccAtoms, version, mode](
                                     const ast::Clause& clause, const ast::Atom* atom, bool isRecursive) {
        return getAtomName(clause, atom, sccAtoms, version, isRecursive, mode);
    };

    std::set<const Atom*> recursiveInCurrentStratum;
    auto atoms = ast::getBodyLiterals<ast::Atom>(clause);
    auto constraints = ast::getBodyLiterals<ast::BinaryConstraint>(clause);

    SipGraph sipGraph(&clause, astConstantTranslator);

    for (Atom* atom : atoms) {
        auto constantMap = sipGraph.getConstantsMap(atom);
        auto unnamedVariables = sipGraph.getUnnamedIndices(atom);
        for (auto joinColumns : sipGraph.getPossibleBoundIndices(atom)) {
            for (auto [k, _] : constantMap) {
                joinColumns.insert(k);
            }

            // construct a EstimateJoinSize ram node
            bool isRecursive = contains(sccAtoms, atom);
            auto relation = getClauseAtomName(clause, atom, isRecursive);

            std::stringstream ss;
            ss << relation << " " << joinColumns << " ";
            for (auto& p : constantMap) {
                ss << "(" << p.first << ", " << p.second << ") ";
            }
            ss << isRecursive;

            if (!contains(seenNodes, ss.str())) {
                auto node =
                        mk<souffle::ram::EstimateJoinSize>(relation, joinColumns, constantMap, isRecursive);
                seenNodes.insert(ss.str());

                if (!joinColumns.empty() || isRecursive) {
                    statements.push_back(std::move(node));
                }
            }
        }
    }
    return statements;
}

std::vector<analysis::StratumJoinSizeEstimates> JoinSizeAnalysis::computeJoinSizeStatements(
        const bool emitStatistics) {
    auto* prog = program;
    auto getSccAtoms = [prog](const ast::Clause* clause, const ast::RelationSet& scc) {
        const auto& sccAtoms = filter(ast::getBodyLiterals<ast::Atom>(*clause),
                [&](const ast::Atom* atom) { return contains(scc, prog->getRelation(*atom)); });
        return sccAtoms;
    };

    const auto& sccOrdering = topsortSCCGraphAnalysis->order();

    std::vector<analysis::StratumJoinSizeEstimates> joinSizeStatements;
    joinSizeStatements.resize(sccOrdering.size());

    if (!emitStatistics) {
        return joinSizeStatements;
    }

    // for each stratum (formed from scc ordering)
    for (std::size_t i = 0; i < sccOrdering.size(); i++) {
        analysis::StratumJoinSizeEstimates stratumNodes;

        auto scc = sccOrdering[i];
        const ast::RelationSet sccRelations = sccGraph->getInternalRelations(scc);
        for (const auto* rel : sccRelations) {
            // Translate each recursive clasue
            for (auto&& clause : program->getClauses(*rel)) {
                auto sccAtoms = getSccAtoms(clause, sccRelations);
                if (recursiveClauses->recursive(clause)) {
                    // for each rule version
                    for (std::size_t version = 0; version < sccAtoms.size(); version++) {
                        if (isA<ast::SubsumptiveClause>(clause)) {
                            using namespace souffle::ast2ram;
                            auto rejectNew = computeRuleVersionStatements(
                                    sccRelations, *clause, version, TranslationMode::SubsumeRejectNewNew);
                            auto rejectNewCurrent = computeRuleVersionStatements(
                                    sccRelations, *clause, version, TranslationMode::SubsumeRejectNewCurrent);
                            auto mode = (sccAtoms.size() > 1) ? TranslationMode::SubsumeDeleteCurrentCurrent
                                                              : TranslationMode::SubsumeDeleteCurrentDelta;
                            auto deleteCurrent =
                                    computeRuleVersionStatements(sccRelations, *clause, version, mode);

                            for (auto& s : rejectNew) {
                                stratumNodes.push_back(std::move(s));
                            }

                            for (auto& s : rejectNewCurrent) {
                                stratumNodes.push_back(std::move(s));
                            }

                            for (auto& s : deleteCurrent) {
                                stratumNodes.push_back(std::move(s));
                            }

                        } else {
                            auto res = computeRuleVersionStatements(sccRelations, *clause, version);
                            for (auto& s : res) {
                                stratumNodes.push_back(std::move(s));
                            }
                        }
                    }
                } else {
                    auto res = computeRuleVersionStatements(sccRelations, *clause, 0);
                    for (auto& s : res) {
                        stratumNodes.push_back(std::move(s));
                    }
                }
            }
        }
        joinSizeStatements[scc] = std::move(stratumNodes);
    }

    std::map<std::string, std::size_t> relationToCompletedStratum;

    // first step is to compute the earliest stratum that a non-recursive relation completes
    for (std::size_t i = 0; i < sccOrdering.size(); ++i) {
        auto scc = sccOrdering[i];
        for (const auto& statement : joinSizeStatements[scc]) {
            const auto& rel = statement->getRelation();

            if (statement->isRecursiveRelation()) {
                continue;
            }

            if (!contains(relationToCompletedStratum, rel)) {
                assert(i > 0 && "Can't access non-recursive relation on stratum 0");
                relationToCompletedStratum[rel] = sccOrdering[i - 1];
            }
        }
    }

    for (std::size_t i = 0; i < sccOrdering.size(); ++i) {
        auto scc = sccOrdering[i];
        for (auto& statement : joinSizeStatements[scc]) {
            const auto& rel = statement->getRelation();
            if (statement->isRecursiveRelation()) {
                continue;
            }
            // sanity check that we have an earliest stratum
            assert(contains(relationToCompletedStratum, rel) &&
                    "Must have earliest stratum where relation is fully computed!");
            std::size_t newStratum = relationToCompletedStratum.at(rel);

            // move the node into the new stratum
            joinSizeStatements[newStratum].push_back(std::move(statement));
        }

        // erase remove all nullptr from the vector since moved from unique_ptr are guaranteed to be nullptr
        auto& v = joinSizeStatements[scc];
        v.erase(std::remove(v.begin(), v.end(), nullptr), v.end());
    }
    return joinSizeStatements;
}

void JoinSizeAnalysis::run(const TranslationUnit& translationUnit) {
    program = &translationUnit.getProgram();
    sccGraph = &translationUnit.getAnalysis<SCCGraphAnalysis>();
    topsortSCCGraphAnalysis = &translationUnit.getAnalysis<TopologicallySortedSCCGraphAnalysis>();
    recursiveClauses = &translationUnit.getAnalysis<RecursiveClausesAnalysis>();
    polyAnalysis = &translationUnit.getAnalysis<ast::analysis::PolymorphicObjectsAnalysis>();
    joinSizeStatements = computeJoinSizeStatements(translationUnit.global().config().has("emit-statistics"));
}

void JoinSizeAnalysis::print(std::ostream& os) const {
    os << "Begin JoinSizeStatements\n";
    for (std::size_t i = 0; i < joinSizeStatements.size(); ++i) {
        os << "Stratum: " << i << "\n";
        for (auto& s : joinSizeStatements[i]) {
            os << *s << "\n";
        }
    }
    os << "End JoinSizeStatements\n";
}

}  // namespace souffle::ast::analysis
