/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file UnitTranslator.cpp
 *
 ***********************************************************************/

#include "ast2ram/online/UnitTranslator.h"
#include "Global.h"
#include "LogStatement.h"
#include "MainDriver.h"
#include "ast/Clause.h"
#include "ast/Directive.h"
#include "ast/Evidence.h"
#include "ast/Relation.h"
#include "ast/SubsumptiveClause.h"
#include "ast/ProbQuery.h"
#include "ast/TranslationUnit.h"
#include "ast/UserDefinedFunctor.h"
#include "ast/analysis/TopologicallySortedSCCGraph.h"
#include "ast/utility/Utils.h"
#include "ast/utility/Visitor.h"
#include "ast2ram/ClauseTranslator.h"
#include "ast2ram/utility/TranslatorContext.h"
#include "ast2ram/utility/Utils.h"
#include "ram/Aggregate.h"
#include "ram/Assign.h"
#include "ram/Call.h"
#include "ram/Clear.h"
#include "ram/Condition.h"
#include "ram/Conjunction.h"
#include "ram/Constraint.h"
#include "ram/DebugInfo.h"
#include "ram/DeltaUnion.h"
#include "ram/EmptinessCheck.h"
#include "ram/Erase.h"
#include "ram/Evidence.h"
#include "ram/ExistenceCheck.h"
#include "ram/Exit.h"
#include "ram/Expression.h"
#include "ram/Filter.h"
#include "ram/IO.h"
#include "ram/Insert.h"
#include "ram/IntrinsicOperator.h"
#include "ram/LogRelationTimer.h"
#include "ram/LogSize.h"
#include "ram/LogTimer.h"
#include "ram/Loop.h"
#include "ram/Negation.h"
#include "ram/Parallel.h"
#include "ram/Program.h"
#include "ram/ProbQuery.h"
#include "ram/Query.h"
#include "ram/Relation.h"
#include "ram/RelationSize.h"
#include "ram/Scan.h"
#include "ram/Sequence.h"
#include "ram/SignedConstant.h"
#include "ram/Statement.h"
#include "ram/Swap.h"
#include "ram/TranslationUnit.h"
#include "ram/TupleElement.h"
#include "ram/UndefValue.h"
#include "ram/UnsignedConstant.h"
#include "ram/UserDefinedAggregator.h"
#include "ram/UserDefinedOperator.h"
#include "ram/Variable.h"
#include "ram/utility/Utils.h"
#include "reports/DebugReport.h"
#include "reports/ErrorReport.h"
#include "souffle/BinaryConstraintOps.h"
#include "souffle/TypeAttribute.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/FunctionalUtil.h"
#include "souffle/utility/MiscUtil.h"
#include "souffle/utility/StringUtil.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <ram/ExactClear.h>
#include <ram/False.h>

namespace souffle::ast2ram::online {

UnitTranslator::UnitTranslator() : ast2ram::UnitTranslator() {}

UnitTranslator::~UnitTranslator() = default;


void UnitTranslator::addRamSubroutine(std::string subroutineID, Own<ram::Statement> subroutine) {
    assert(!contains(ramSubroutines, subroutineID) && "subroutine ID should not already exist");
    ramSubroutines[subroutineID] = std::move(subroutine);
}

Own<ram::Statement> UnitTranslator::generateClearRelation(const ast::Relation* relation) const {
    return mk<ram::Clear>(getConcreteRelationName(relation->getQualifiedName()));
}

Own<ram::Statement> UnitTranslator::generateNonRecursiveRelation(const ast::Relation& rel) const {
    VecOwn<ram::Statement> result;

    // Get relation names
    std::string mainRelation = getConcreteRelationName(rel.getQualifiedName());

    // Iterate over all non-recursive clauses that belong to the relation
    for (auto&& clause : context->getProgram()->getClauses(rel)) {
        // Skip recursive and subsumptive clauses
        if (context->isRecursiveClause(clause) || isA<ast::SubsumptiveClause>(clause)) {
            continue;
        }

        // Translate clause
        TranslationMode mode = rel.getAuxiliaryArity() > 0 ? Auxiliary : DEFAULT;
        Own<ram::Statement> rule = context->translateNonRecursiveClause(*clause, mode);

        // Add logging
        if (glb->config().has("profile")) {
            const std::string& relationName = toString(rel.getQualifiedName());
            const auto& srcLocation = clause->getSrcLoc();
            const std::string clauseText = stringify(toString(*clause));
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRule(relationName, srcLocation, clauseText);
            rule = mk<ram::LogRelationTimer>(std::move(rule), logTimerStatement, mainRelation);
        }

        // Add debug info
        std::ostringstream ds;
        clause->printForDebugInfo(ds);
        ds << "\nin file ";
        ds << clause->getSrcLoc();
        rule = mk<ram::DebugInfo>(std::move(rule), ds.str());

        // Add rule to result
        appendStmt(result, std::move(rule));
    }

    // Add logging for entire relation
    if (glb->config().has("profile")) {
        const std::string& relationName = toString(rel.getQualifiedName());
        const auto& srcLocation = rel.getSrcLoc();
        const std::string logSizeStatement = LogStatement::nNonrecursiveRelation(relationName, srcLocation);

        // Add timer if we did any work
        if (!result.empty()) {
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRelation(relationName, srcLocation);
            auto newStmt = mk<ram::LogRelationTimer>(
                    mk<ram::Sequence>(std::move(result)), logTimerStatement, mainRelation);
            result.clear();
            appendStmt(result, std::move(newStmt));
        } else {
            // Add table size printer
            appendStmt(result, mk<ram::LogSize>(mainRelation, logSizeStatement));
        }
    }

    return mk<ram::Sequence>(std::move(result));
}


Own<ram::Statement> UnitTranslator::generateNonRecursiveDelete(const ast::Relation& rel) const {
    VecOwn<ram::Statement> code;

    // Generate code for non-recursive subsumption
    if (!context->hasSubsumptiveClause(rel.getQualifiedName())) {
        return mk<ram::Sequence>(std::move(code));
    }

    std::string mainRelation = getConcreteRelationName(rel.getQualifiedName());
    std::string deleteRelation = getDeleteRelationName(rel.getQualifiedName());

    // Compute subsumptive deletions for non-recursive rules
    for (auto clause : context->getProgram()->getClauses(rel)) {
        if (!isA<ast::SubsumptiveClause>(clause)) {
            continue;
        }
        // Translate subsumptive clause
        Own<ram::Statement> rule = context->translateNonRecursiveClause(*clause, SubsumeDeleteCurrentCurrent);

        // Add logging for subsumptive clause
        if (glb->config().has("profile")) {
            const std::string& relationName = toString(rel.getQualifiedName());
            const auto& srcLocation = clause->getSrcLoc();
            const std::string clauseText = stringify(toString(*clause));
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRule(relationName, srcLocation, clauseText);
            rule = mk<ram::LogRelationTimer>(std::move(rule), logTimerStatement, mainRelation);
        }

        // Add debug info for subsumptive clause
        std::ostringstream ds;
        ds << toString(*clause) << "\nin file ";
        ds << clause->getSrcLoc();
        rule = mk<ram::DebugInfo>(std::move(rule), ds.str());

        // Add subsumptive rule to result
        appendStmt(code, std::move(rule));
    }
    appendStmt(code, mk<ram::Sequence>(generateEraseTuples(&rel, mainRelation, deleteRelation),
                             mk<ram::Clear>(deleteRelation)));
    return mk<ram::Sequence>(std::move(code));
}

Own<ram::Statement> UnitTranslator::generateStratum(std::size_t scc) const {
    // Make a new ram statement for the current SCC
    VecOwn<ram::Statement> current;

    // Load all internal input relations from the facts dir with a .facts extension
    for (const auto& relation : context->getInputRelationsInSCC(scc)) {
        appendStmt(current, generateLoadRelation(relation));
    }

    // Compute the current stratum
    const auto& sccRelations = context->getRelationsInSCC(scc);
    if (context->isRecursiveSCC(scc)) {
        appendStmt(current, generateRecursiveStratum(sccRelations, scc));
    } else {
        assert(sccRelations.size() == 1 && "only one relation should exist in non-recursive stratum");
        const auto* rel = *sccRelations.begin();
        appendStmt(current, generateNonRecursiveRelation(*rel));

        // lub auxiliary arities using the @lub relation
        if (rel->getAuxiliaryArity() > 0) {
            std::string mainRelation = getConcreteRelationName(rel->getQualifiedName());
            std::string newRelation = getNewRelationName(rel->getQualifiedName());
            std::string deltaRelation = getDeltaRelationName(rel->getQualifiedName());
            appendStmt(current, generateStratumLubSequence(*rel, false));
            std::map<std::string, std::string> directives;
            appendStmt(current, mk<ram::Clear>(newRelation));
        }

        // issue delete sequence for non-recursive subsumptions
        // appendStmt(current, generateNonRecursiveDelete(*rel));
    }

    // Get all non-recursive relation statements
    auto nonRecursiveJoinSizeStatements = context->getNonRecursiveJoinSizeStatementsInSCC(scc);
    auto joinSizeSequence = mk<ram::Sequence>(std::move(nonRecursiveJoinSizeStatements));
    appendStmt(current, std::move(joinSizeSequence));

    // Store all internal output relations to the output dir with a .csv extension
    for (const auto& relation : context->getOutputRelationsInSCC(scc)) {
        appendStmt(current, generateStoreRelation(relation));
    }

    return mk<ram::Sequence>(std::move(current));
}

Own<ram::Statement> UnitTranslator::generateNonRecursiveRelationInc(const ast::Relation& rel) const {
    VecOwn<ram::Statement> result;
    const auto& clauses = context->getProgram()->getClauses(rel);

    // Pure EDB relations already maintain old/post-delete/current in generateLoadRelationInc().
    // Running the generic non-recursive phase again would overwrite post_delete from final current.
    // Mixed EDB+IDB relations use the relation-level delete/insert phases below.
    if (clauses.empty()) {
        return mk<ram::Sequence>(std::move(result));
    }

    std::string mainRelation = getConcreteRelationName(rel.getQualifiedName());
    std::string postDeleteRelation = getPostDeleteRelationName(rel.getQualifiedName());

    appendStmt(result, generateNonRecursiveRelationDel(rel));
    appendStmt(result, mk<ram::Clear>(postDeleteRelation));
    appendStmt(result, generateMergeRelations(&rel, postDeleteRelation, mainRelation));
    appendStmt(result, generateNonRecursiveRelationIns(rel));

    // Add logging for entire relation
    if (glb->config().has("profile")) {
        const std::string& relationName = toString(rel.getQualifiedName());
        const auto& srcLocation = rel.getSrcLoc();
        const std::string logSizeStatement = LogStatement::nNonrecursiveRelation(relationName, srcLocation);

        // Add timer if we did any work
        if (!result.empty()) {
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRelation(relationName, srcLocation);
            auto newStmt = mk<ram::LogRelationTimer>(
                    mk<ram::Sequence>(std::move(result)), logTimerStatement, mainRelation);
            result.clear();
            appendStmt(result, std::move(newStmt));
        } else {
            // Add table size printer
            appendStmt(result, mk<ram::LogSize>(mainRelation, logSizeStatement));
        }
    }

    return mk<ram::Sequence>(std::move(result));
}

Own<ram::Statement> UnitTranslator::generateStratumInc(std::size_t scc) const {
    // Make a new ram statement for the current SCC
    VecOwn<ram::Statement> current;

    for (const auto& relation : context->getInputRelationsInSCC(scc)) {
        appendStmt(current, generateLoadRelationInc(relation));
    }

    // Compute the current stratum
    const auto& sccRelations = context->getRelationsInSCC(scc);
    if (context->isRecursiveSCC(scc)) {
        appendStmt(current, generateRecursiveStratumInc(sccRelations, scc));
        // assert(false && "recursion not supported");
    } else {
        assert(sccRelations.size() == 1 && "only one relation should exist in non-recursive stratum");
        const auto* rel = *sccRelations.begin();
        appendStmt(current, generateNonRecursiveRelationInc(*rel));

        // lub auxiliary arities using the @lub relation
        if (rel->getAuxiliaryArity() > 0) {
            assert (false && "lub auxiliary not supported");
        }

        if (context->hasSubsumptiveClause(rel->getQualifiedName())) {
            assert (false && "subsumption clause not supported");
        }
    }

    // Join-size statements are not part of the incremental RAM protocol.
    const auto nonRecursiveJoinSizeStatements = context->getNonRecursiveJoinSizeStatementsInSCC(scc);
    assert(nonRecursiveJoinSizeStatements.empty() && "does not support join size statement currently");

    // Store all internal output relations to the output dir with a .csv extension
    // Note: delta derivations will be dumped after all calculations
    // Note: do we also need to provide delta EDB? It seems this is already enough
    for (const auto& relation : context->getOutputRelationsInSCC(scc)) {
        appendStmt(current, generateStoreRelation(relation));
    }

    return mk<ram::Sequence>(std::move(current));
}


Own<ram::Statement> UnitTranslator::generateClearExpiredRelations(
        const ast::RelationSet& expiredRelations) const {
    VecOwn<ram::Statement> stmts;
    for (const auto& relation : expiredRelations) {
        appendStmt(stmts, generateClearRelation(relation));
    }
    return mk<ram::Sequence>(std::move(stmts));
}

Own<ram::Statement> UnitTranslator::generateEraseTuples(
        const ast::Relation* rel, const std::string& destRelation, const std::string& srcRelation) const {
    VecOwn<ram::Expression> values;
    for (std::size_t i = 0; i < rel->getArity(); i++) {
        values.push_back(mk<ram::TupleElement>(0, i));
    }
    auto insertion = mk<ram::Erase>(destRelation, std::move(values));
    return mk<ram::Query>(mk<ram::Scan>(srcRelation, 0, std::move(insertion)));
}

Own<ram::Statement> UnitTranslator::generateMergeRelationsWithFilter(const ast::Relation* rel,
        const std::string& destRelation, const std::string& srcRelation,
        const std::string& filterRelation) const {
    VecOwn<ram::Expression> values;
    VecOwn<ram::Expression> values2;

    // Proposition - insert if not empty
    if (rel->getArity() == 0) {
        auto insertion = mk<ram::Insert>(destRelation, std::move(values));
        return mk<ram::Query>(mk<ram::Filter>(
                mk<ram::Negation>(mk<ram::EmptinessCheck>(srcRelation)), std::move(insertion)));
    }

    // Predicate - insert all values
    for (std::size_t i = 0; i < rel->getArity(); i++) {
        values.push_back(mk<ram::TupleElement>(0, i));
        values2.push_back(mk<ram::TupleElement>(0, i));
    }
    auto insertion = mk<ram::Insert>(destRelation, std::move(values));
    auto filtered =
            mk<ram::Filter>(mk<ram::Negation>(mk<ram::ExistenceCheck>(filterRelation, std::move(values2))),
                    std::move(insertion));
    auto stmt = mk<ram::Query>(mk<ram::Scan>(srcRelation, 0, std::move(filtered)));

    return stmt;
}


Own<ram::Statement> UnitTranslator::generateMergeRelationsWithNegativeFilter(const ast::Relation* rel,
        const std::string& destRelation, const std::string& srcRelation,
        const std::string& filterRelation) const {
    VecOwn<ram::Expression> values;
    VecOwn<ram::Expression> values2;

    // Proposition - insert if not empty
    if (rel->getArity() == 0) {
        auto insertion = mk<ram::Insert>(destRelation, std::move(values));
        return mk<ram::Query>(mk<ram::Filter>(
                (mk<ram::EmptinessCheck>(srcRelation)), std::move(insertion)));
    }

    // Predicate - insert all values
    for (std::size_t i = 0; i < rel->getArity(); i++) {
        values.push_back(mk<ram::TupleElement>(0, i));
        values2.push_back(mk<ram::TupleElement>(0, i));
    }
    auto insertion = mk<ram::Insert>(destRelation, std::move(values));
    auto filtered =
            mk<ram::Filter>(mk<ram::ExistenceCheck>(filterRelation, std::move(values2)),
                    std::move(insertion));
    auto stmt = mk<ram::Query>(mk<ram::Scan>(srcRelation, 0, std::move(filtered)));

    return stmt;
}

Own<ram::Statement> UnitTranslator::generateMergeRelations(
        const ast::Relation* rel, const std::string& destRelation, const std::string& srcRelation) const {
    VecOwn<ram::Expression> values;

    // Proposition - insert if not empty
    if (rel->getArity() == 0) {
        auto insertion = mk<ram::Insert>(destRelation, std::move(values));
        return mk<ram::Query>(mk<ram::Filter>(
                mk<ram::Negation>(mk<ram::EmptinessCheck>(srcRelation)), std::move(insertion)));
    }

    // Predicate - insert all values
    for (std::size_t i = 0; i < rel->getArity(); i++) {
        values.push_back(mk<ram::TupleElement>(0, i));  // TupleElement refers to a tuple field. This looks a bit redundant, essentially deconstructing and rebuilding a tuple; it is tied to RAM design.
    }
    auto insertion = mk<ram::Insert>(destRelation, std::move(values));
    auto stmt = mk<ram::Query>(mk<ram::Scan>(srcRelation, 0, std::move(insertion)));
    return stmt;
}

Own<ram::Statement> UnitTranslator::generateDebugRelation(const ast::Relation* rel,
        const std::string& destRelation, const std::string& srcRelation,
        Own<ram::Expression> iteration) const {
    VecOwn<ram::Expression> values;

    for (std::size_t i = 0; i < rel->getArity(); i++) {
        values.push_back(mk<ram::TupleElement>(0, i));
    }

    values.push_back(std::move(iteration));

    // Proposition - insert if not empty
    if (rel->getArity() == 0) {
        auto insertion = mk<ram::Insert>(destRelation, std::move(values));
        return mk<ram::Query>(mk<ram::Filter>(
                mk<ram::Negation>(mk<ram::EmptinessCheck>(srcRelation)), std::move(insertion)));
    }

    auto insertion = mk<ram::Insert>(destRelation, std::move(values));
    auto stmt = mk<ram::Query>(mk<ram::Scan>(srcRelation, 0, std::move(insertion)));
    return stmt;
}

Own<ram::Statement> UnitTranslator::translateRecursiveClauses(
        const ast::RelationSet& scc, const ast::Relation* rel) const {
    assert(contains(scc, rel) && "relation should belong to scc");
    VecOwn<ram::Statement> code;

    // Translate each recursive clasue
    for (auto&& clause : context->getProgram()->getClauses(*rel)) {
        // Skip non-recursive and subsumptive clauses
        if (!context->isRecursiveClause(clause) || isA<ast::SubsumptiveClause>(clause)) {
            continue;
        }

        // generate all delta versions of a recursive clause
        auto clauseVersions = generateClauseVersions(clause, scc);
        for (auto& clauseVersion : clauseVersions) {
            appendStmt(code, std::move(clauseVersion));
        }
    }

    return mk<ram::Sequence>(std::move(code));
}

Own<ram::Statement> UnitTranslator::translateSubsumptiveRecursiveClauses(
        const ast::RelationSet& scc, const ast::Relation* rel) const {
    assert(contains(scc, rel) && "relation should belong to scc");

    VecOwn<ram::Statement> code;
    if (!context->hasSubsumptiveClause(rel->getQualifiedName())) {
        return mk<ram::Sequence>(std::move(code));
    }

    std::string mainRelation = getConcreteRelationName(rel->getQualifiedName());
    std::string newRelation = getNewRelationName(rel->getQualifiedName());
    std::string deltaRelation = getDeltaRelationName(rel->getQualifiedName());
    std::string rejectRelation = getRejectRelationName(rel->getQualifiedName());
    std::string deleteRelation = getDeleteRelationName(rel->getQualifiedName());

    // old delta relation can be cleared
    appendStmt(code, mk<ram::Clear>(deltaRelation));

    // compute reject set using the subsumptive clauses
    for (const auto* clause : context->getProgram()->getClauses(*rel)) {
        // Skip non-subsumptive clauses
        if (!isA<ast::SubsumptiveClause>(clause)) {
            continue;
        }

        const std::size_t version = 0;
        // find dominated tuples in the newR by tuples in newR  and store them in rejectR
        appendStmt(code, context->translateRecursiveClause(*clause, scc, version, SubsumeRejectNewNew));
        // find dominated tuples in the newR by tuples in R and store them in rejectR
        appendStmt(code, context->translateRecursiveClause(*clause, scc, version, SubsumeRejectNewCurrent));
    }

    // compute new delta set, i.e., deltaR = newR \ rejectR
    appendStmt(code, generateMergeRelationsWithFilter(rel, deltaRelation, newRelation, rejectRelation));
    appendStmt(code, mk<ram::Clear>(rejectRelation));
    appendStmt(code, mk<ram::Clear>(newRelation));

    // compute delete set,  remove tuples from R, and clear delete set
    for (const auto* clause : context->getProgram()->getClauses(*rel)) {
        // Skip non-subsumptive clauses
        if (!isA<ast::SubsumptiveClause>(clause)) {
            continue;
        }

        const auto& sccAtoms = getSccAtoms(clause, scc);
        std::size_t sz = sccAtoms.size();
        for (std::size_t version = 0; version < sz; version++) {
            appendStmt(
                    code, context->translateRecursiveClause(*clause, scc, version,
                                  (version >= 1) ? SubsumeDeleteCurrentCurrent : SubsumeDeleteCurrentDelta));
        }
        appendStmt(code, generateEraseTuples(rel, mainRelation, deleteRelation));
        appendStmt(code, mk<ram::Clear>(deleteRelation));
    }

    return mk<ram::Sequence>(std::move(code));
}

std::vector<ast::Atom*> UnitTranslator::getSccAtoms(
        const ast::Clause* clause, const ast::RelationSet& scc) const {
    const auto& sccAtoms = filter(ast::getBodyLiterals<ast::Atom>(*clause), [&](const ast::Atom* atom) {
        if (isA<ast::SubsumptiveClause>(clause)) {
            const auto& body = clause->getBodyLiterals();
            // skip dominated head
            auto dominatedHeadAtom = dynamic_cast<const ast::Atom*>(body[0]);
            if (atom == dominatedHeadAtom) return false;
        }
        return contains(scc, context->getProgram()->getRelation(*atom));
    });
    return sccAtoms;
}

VecOwn<ram::Statement> UnitTranslator::generateClauseVersions(
        const ast::Clause* clause, const ast::RelationSet& scc) const {
    const auto& sccAtoms = getSccAtoms(clause, scc);

    // Create each version
    VecOwn<ram::Statement> clauseVersions;
    for (std::size_t version = 0; version < sccAtoms.size(); version++) {
        appendStmt(clauseVersions, context->translateRecursiveClause(*clause, scc, version));
    }

    // Check that the correct number of versions have been created
    if (clause->getExecutionPlan() != nullptr) {
        std::optional<std::size_t> maxVersion;
        for (const auto& cur : clause->getExecutionPlan()->getOrders()) {
            maxVersion = std::max(cur.first, maxVersion.value_or(cur.first));
        }
        assert(sccAtoms.size() > *maxVersion && "missing clause versions");
    }

    return clauseVersions;
}

Own<ram::Statement> UnitTranslator::generateNonRecursiveRelationDel(const ast::Relation& rel) const {
    VecOwn<ram::Statement> result;

    // Get relation names
    std::string mainRelation = getConcreteRelationName(rel.getQualifiedName());

    // Iterate over all non-recursive clauses that belong to the relation
    for (auto&& clause : context->getProgram()->getClauses(rel)) {
        // Skip recursive and subsumptive clauses
        if (context->isRecursiveClause(clause) || isA<ast::SubsumptiveClause>(clause)) {
            continue;
        }

        // Translate clause
        TranslationMode mode = rel.getAuxiliaryArity() > 0 ? Auxiliary : Incremental;
        Own<ram::Statement> rule = context->translateNonRecursiveClauseDelInc(*clause, mode);

        // Add logging
        if (glb->config().has("profile")) {
            const std::string& relationName = toString(rel.getQualifiedName());
            const auto& srcLocation = clause->getSrcLoc();
            const std::string clauseText = stringify(toString(*clause));
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRule(relationName, srcLocation, clauseText);
            rule = mk<ram::LogRelationTimer>(std::move(rule), logTimerStatement, mainRelation);
        }

        // Add debug info
        std::ostringstream ds;
        clause->printForDebugInfo(ds);
        ds << "\nin file ";
        ds << clause->getSrcLoc();
        rule = mk<ram::DebugInfo>(std::move(rule), ds.str());

        // Add rule to result
        appendStmt(result, std::move(rule));
    }

    if (context->getProgram()->getClauses(rel).size() > 0) {
        auto headRelationName = getConcreteRelationName(rel.getQualifiedName());
        auto headOldRelationName = getOldRelationName(rel.getQualifiedName());
        auto headDeltaDervDeleteRelationName = getIncDeltaDervDeleteRelationName(rel.getQualifiedName());
        auto headDeltaTupleDeleteRelationName = getIncDeltaTupleDeleteRelationName(rel.getQualifiedName());

        appendStmt(result, mk<ram::DeltaUnion>(headRelationName,
            "", headRelationName,
            "", headDeltaDervDeleteRelationName,
            "", headDeltaTupleDeleteRelationName));
    }

    // Add logging for entire relation
    if (glb->config().has("profile")) {
        const std::string& relationName = toString(rel.getQualifiedName());
        const auto& srcLocation = rel.getSrcLoc();
        const std::string logSizeStatement = LogStatement::nNonrecursiveRelation(relationName, srcLocation);

        // Add timer if we did any work
        if (!result.empty()) {
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRelation(relationName, srcLocation);
            auto newStmt = mk<ram::LogRelationTimer>(
                    mk<ram::Sequence>(std::move(result)), logTimerStatement, mainRelation);
            result.clear();
            appendStmt(result, std::move(newStmt));
        } else {
            // Add table size printer
            appendStmt(result, mk<ram::LogSize>(mainRelation, logSizeStatement));
        }
    }

    return mk<ram::Sequence>(std::move(result));
}


Own<ram::Statement> UnitTranslator::generateNonRecursiveRelationIns(const ast::Relation& rel) const {
    VecOwn<ram::Statement> result;

    // Get relation names
    std::string mainRelation = getConcreteRelationName(rel.getQualifiedName());

    // Iterate over all non-recursive clauses that belong to the relation
    for (auto&& clause : context->getProgram()->getClauses(rel)) {
        // Skip recursive and subsumptive clauses
        if (context->isRecursiveClause(clause) || isA<ast::SubsumptiveClause>(clause)) {
            continue;
        }

        // Translate clause
        TranslationMode mode = rel.getAuxiliaryArity() > 0 ? Auxiliary : Incremental;
        Own<ram::Statement> rule = context->translateNonRecursiveClauseInsInc(*clause, mode);

        // Add logging
        if (glb->config().has("profile")) {
            const std::string& relationName = toString(rel.getQualifiedName());
            const auto& srcLocation = clause->getSrcLoc();
            const std::string clauseText = stringify(toString(*clause));
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRule(relationName, srcLocation, clauseText);
            rule = mk<ram::LogRelationTimer>(std::move(rule), logTimerStatement, mainRelation);
        }

        // Add debug info
        std::ostringstream ds;
        clause->printForDebugInfo(ds);
        ds << "\nin file ";
        ds << clause->getSrcLoc();
        rule = mk<ram::DebugInfo>(std::move(rule), ds.str());

        // Add rule to result
        appendStmt(result, std::move(rule));
    }

    // Merge insertion derivation deltas into the concrete tuple relation.
    if (context->getProgram()->getClauses(rel).size() > 0) {
        auto headRelationName = getConcreteRelationName(rel.getQualifiedName());
        auto headOldRelationName = getOldRelationName(rel.getQualifiedName());
        auto headDeltaDervInsertRelationName = getIncDeltaDervInsertRelationName(rel.getQualifiedName());
        auto headDeltaTupleInsertRelationName = getIncDeltaTupleInsertRelationName(rel.getQualifiedName());

        appendStmt(result, mk<ram::DeltaUnion>(headRelationName,
            "", headRelationName,
            headDeltaDervInsertRelationName, "",
            headDeltaTupleInsertRelationName, ""));
    }

    // Add logging for entire relation
    if (glb->config().has("profile")) {
        const std::string& relationName = toString(rel.getQualifiedName());
        const auto& srcLocation = rel.getSrcLoc();
        const std::string logSizeStatement = LogStatement::nNonrecursiveRelation(relationName, srcLocation);

        // Add timer if we did any work
        if (!result.empty()) {
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRelation(relationName, srcLocation);
            auto newStmt = mk<ram::LogRelationTimer>(
                    mk<ram::Sequence>(std::move(result)), logTimerStatement, mainRelation);
            result.clear();
            appendStmt(result, std::move(newStmt));
        } else {
            // Add table size printer
            appendStmt(result, mk<ram::LogSize>(mainRelation, logSizeStatement));
        }
    }

    return mk<ram::Sequence>(std::move(result));
}

Own<ram::Statement> UnitTranslator::generateStratumPreamble(const ast::RelationSet& scc) const {
    VecOwn<ram::Statement> preamble;

    // Generate code for non-recursive rules
    // Process non-recursive rules for each relation.
    for (const ast::Relation* rel : scc) {
        std::string deltaRelation = getDeltaRelationName(rel->getQualifiedName());
        std::string mainRelation = getConcreteRelationName(rel->getQualifiedName());
        appendStmt(preamble, generateNonRecursiveRelation(*rel));
        // lub tuples using the @lub relation
        if (rel->getAuxiliaryArity() > 0) {
            std::string newRelation = getNewRelationName(rel->getQualifiedName());
            appendStmt(preamble, generateStratumLubSequence(*rel, false));
        }
        // Generate non recursive delete sequences for subsumptive rules
        appendStmt(preamble, generateNonRecursiveDelete(*rel));
    }

    // Generate code for priming relation
    // Initialize delta by copying existing relation contents into delta. They may be non-empty due to non-recursive rules, handled earlier.
    for (const ast::Relation* rel : scc) {
        std::string deltaRelation = getDeltaRelationName(rel->getQualifiedName());
        std::string mainRelation = getConcreteRelationName(rel->getQualifiedName());
        appendStmt(preamble, generateMergeRelations(rel, deltaRelation, mainRelation));
    }

    // Maintain debug delta relations when the analysis requested them.
    for (const ast::Relation* rel : scc) {
        if (const auto* debugRel = context->getDeltaDebugRelation(rel)) {
            const std::string debugRelation = getConcreteRelationName(debugRel->getQualifiedName());
            std::string deltaRelation = getDeltaRelationName(rel->getQualifiedName());
            appendStmt(preamble,
                    generateDebugRelation(rel, debugRelation, deltaRelation, mk<ram::UnsignedConstant>(0)));
        }
    }
    return mk<ram::Sequence>(std::move(preamble));
}


Own<ram::Statement> UnitTranslator::generateStratumPreambleInc(const ast::RelationSet& scc, bool isDelete) const {
    VecOwn<ram::Statement> preamble;


    for (const ast::Relation* rel : scc) {
        if (isDelete) {
            appendStmt(preamble, generateNonRecursiveRelationDel(*rel));
        }
        else {
            appendStmt(preamble, generateNonRecursiveRelationIns(*rel));
        }
    }

    // Generate code for priming relation
    // Initialize delta by copying existing relation contents into delta. They may be non-empty due to non-recursive rules, handled earlier.
    // INC: initialize delta_deletion, delta_insertion
    for (const ast::Relation* rel : scc) {
        std::string deltaDelRelation = getDeltaDeletionRelationName(rel->getQualifiedName());
        std::string deltaInsRelation = getDeltaInsertionRelationName(rel->getQualifiedName());
        std::string deltaTupleDelRelation = getIncDeltaTupleDeleteRelationName(rel->getQualifiedName());
        std::string deltaTupleInsRelation = getIncDeltaTupleInsertRelationName(rel->getQualifiedName());
        if (isDelete) {
            appendStmt(preamble, generateMergeRelations(rel, deltaDelRelation, deltaTupleDelRelation));
            // recursion treat deletions in over-delete relations
            appendStmt(preamble, generateMergeRelations(rel, getIncTupleOverDeleteRelationName(rel->getQualifiedName()), getIncDeltaTupleDeleteRelationName(rel->getQualifiedName())))
;        } else {
            appendStmt(preamble, generateMergeRelations(rel, deltaInsRelation, deltaTupleInsRelation));
        }
    }

    // Maintain debug delta relations when the analysis requested them.
    for (const ast::Relation* rel : scc) {
        if (const auto* debugRel = context->getDeltaDebugRelation(rel)) {
            const std::string debugRelation = getConcreteRelationName(debugRel->getQualifiedName());
            std::string deltaRelation = getDeltaRelationName(rel->getQualifiedName());
            appendStmt(preamble,
                    generateDebugRelation(rel, debugRelation, deltaRelation, mk<ram::UnsignedConstant>(0)));
        }
    }
    return mk<ram::Sequence>(std::move(preamble));
}

Own<ram::Statement> UnitTranslator::generateStratumPostamble(const ast::RelationSet& scc) const {
    VecOwn<ram::Statement> postamble;
    for (const ast::Relation* rel : scc) {
        // Drop temporary tables after recursion
        appendStmt(postamble, mk<ram::Clear>(getDeltaRelationName(rel->getQualifiedName())));
        appendStmt(postamble, mk<ram::Clear>(getNewRelationName(rel->getQualifiedName())));
    }
    return mk<ram::Sequence>(std::move(postamble));
}

Own<ram::Statement> UnitTranslator::generateStratumTableUpdates(const ast::RelationSet& scc) const {
    VecOwn<ram::Statement> updateTable;

    for (const ast::Relation* rel : scc) {
        // Copy @new into main relation, @delta := @new, and empty out @new
        std::string mainRelation = getConcreteRelationName(rel->getQualifiedName());
        std::string newRelation = getNewRelationName(rel->getQualifiedName());
        std::string deltaRelation = getDeltaRelationName(rel->getQualifiedName());

        // swap new and and delta relation and clear new relation afterwards (if not a subsumptive relation)
        Own<ram::Statement> updateRelTable;
        if (rel->getAuxiliaryArity() > 0) {
            updateRelTable =
                    mk<ram::Sequence>(mk<ram::Clear>(deltaRelation), generateStratumLubSequence(*rel, true),
                            generateMergeRelations(rel, mainRelation, deltaRelation));
        } else if (!context->hasSubsumptiveClause(rel->getQualifiedName())) {
            updateRelTable = mk<ram::Sequence>(generateMergeRelations(rel, mainRelation, newRelation),
                    mk<ram::Swap>(deltaRelation, newRelation), mk<ram::Clear>(newRelation));
        } else {
            updateRelTable = generateMergeRelations(rel, mainRelation, deltaRelation);
        }

        // Measure update time
        if (glb->config().has("profile")) {
            updateRelTable = mk<ram::LogRelationTimer>(std::move(updateRelTable),
                    LogStatement::cRecursiveRelation(toString(rel->getQualifiedName()), rel->getSrcLoc()),
                    newRelation);
        }

        appendStmt(updateTable, std::move(updateRelTable));
        if (const auto* debugRel = context->getDeltaDebugRelation(rel)) {
            const std::string debugRelation = getConcreteRelationName(debugRel->getQualifiedName());
            appendStmt(updateTable, generateDebugRelation(rel, debugRelation, deltaRelation,
                                            mk<ram::Variable>("loop_counter")));
        }
    }
    return mk<ram::Sequence>(std::move(updateTable));
}

Own<ram::Statement> UnitTranslator::generateStratumLoopBody(const ast::RelationSet& scc) const {
    VecOwn<ram::Statement> loopBody;

    const bool hasProfile = glb->config().has("profile");

    auto addProfiling = [hasProfile](
                                const ast::Relation* rel, Own<ram::Statement> stmt) -> Own<ram::Statement> {
        if (hasProfile) {
            const std::string& relationName = toString(rel->getQualifiedName());
            const auto& srcLocation = rel->getSrcLoc();
            const std::string logTimerStatement = LogStatement::tRecursiveRelation(relationName, srcLocation);
            return mk<ram::LogRelationTimer>(mk<ram::Sequence>(std::move(stmt)), logTimerStatement,
                    getNewRelationName(rel->getQualifiedName()));
        }
        return stmt;
    };

    // first translate regular recursive clauses
    for (const ast::Relation* rel : scc) {
        auto relClauses = translateRecursiveClauses(scc, rel);
        // add profiling information
        relClauses = addProfiling(rel, std::move(relClauses));
        appendStmt(loopBody, mk<ram::Sequence>(std::move(relClauses)));
    }

    // translating subsumptive clauses
    for (const ast::Relation* rel : scc) {
        auto relClauses = translateSubsumptiveRecursiveClauses(scc, rel);
        // add profiling information
        relClauses = addProfiling(rel, std::move(relClauses));
        appendStmt(loopBody, mk<ram::Sequence>(std::move(relClauses)));
    }

    return mk<ram::Sequence>(std::move(loopBody));
}

/// assuming the @new() relation is populated with new tuples, generate RAM code
/// to populate the @delta() relation with the lubbed elements from @new()
Own<ram::Statement> UnitTranslator::generateStratumLubSequence(
        const ast::Relation& rel, bool inRecursiveLoop) const {
    VecOwn<ram::Statement> stmts;
    assert(rel.getAuxiliaryArity() > 0);

    auto attributes = rel.getAttributes();
    std::string name = getConcreteRelationName(rel.getQualifiedName());
    std::string lubName = getLubRelationName(rel.getQualifiedName());
    std::string newName = getNewRelationName(rel.getQualifiedName());

    const std::size_t arity = rel.getArity();
    const std::size_t auxiliaryArity = rel.getAuxiliaryArity();

    // Step 1 : populate @lub() from @new()
    VecOwn<ram::Expression> values;

    // index of the first auxiliary element of the relation
    std::size_t firstAuxiliary = arity - auxiliaryArity;

    for (std::size_t i = 0; i < arity; i++) {
        if (i >= firstAuxiliary) {
            values.push_back(mk<ram::TupleElement>(i - firstAuxiliary + 1, 0));
        } else {
            values.push_back(mk<ram::TupleElement>(0, i));
        }
    }
    Own<ram::Operation> op = mk<ram::Insert>(lubName, std::move(values));

    for (std::size_t i = arity; i >= firstAuxiliary + 1; i--) {
        const auto type = attributes[i - 1]->getTypeName();
        std::size_t level = i - firstAuxiliary;
        auto aggregator = context->getLatticeTypeLubAggregator(type, mk<ram::TupleElement>(0, i - 1));
        Own<ram::Condition> condition = mk<ram::Constraint>(
                BinaryConstraintOp::NE, mk<ram::TupleElement>(level, i - 1), mk<ram::TupleElement>(0, i - 1));
        for (std::size_t j = 0; j < attributes.size(); j++) {
            if (attributes[j]->getIsLattice()) break;
            condition = mk<ram::Conjunction>(std::move(condition),
                    mk<ram::Constraint>(BinaryConstraintOp::EQ, mk<ram::TupleElement>(level, j),
                            mk<ram::TupleElement>(0, j)));
        }
        op = mk<ram::Aggregate>(std::move(op), std::move(aggregator), newName,
                mk<ram::TupleElement>(level, i - 1), std::move(condition), level);
    }

    op = mk<ram::Scan>(newName, 0, std::move(op));
    appendStmt(stmts, mk<ram::Query>(std::move(op)));

    // clear @new() now that we no longer need it
    appendStmt(stmts, mk<ram::Clear>(newName));

    if (inRecursiveLoop) {
        // Step 2 : populate @delta() from @lub() for tuples that have to be lubbed with @concrete
        std::string deltaName = getDeltaRelationName(rel.getQualifiedName());

        Own<ram::Condition> condition;
        for (std::size_t i = 0; i < arity; i++) {
            if (i < firstAuxiliary) {
                values.push_back(mk<ram::TupleElement>(0, i));
            } else {
                assert(attributes[i]->getIsLattice());
                const auto type = attributes[i]->getTypeName();
                VecOwn<ram::Expression> args;
                args.push_back(mk<ram::TupleElement>(0, i));
                args.push_back(mk<ram::TupleElement>(1, i));
                auto lub = context->getLatticeTypeLubFunctor(type, std::move(args));
                auto cst =
                        mk<ram::Constraint>(BinaryConstraintOp::EQ, mk<ram::TupleElement>(1, i), clone(lub));
                if (condition) {
                    condition = mk<ram::Conjunction>(std::move(condition), std::move(cst));
                } else {
                    condition = std::move(cst);
                }
                values.push_back(std::move(lub));
            }
        }
        op = mk<ram::Insert>(deltaName, std::move(values));
        op = mk<ram::Filter>(mk<ram::Negation>(std::move(condition)), std::move(op));

        for (std::size_t i = 0; i < arity - auxiliaryArity; i++) {
            auto cst = mk<ram::Constraint>(
                    BinaryConstraintOp::EQ, mk<ram::TupleElement>(0, i), mk<ram::TupleElement>(1, i));
            if (condition) {
                condition = mk<ram::Conjunction>(std::move(condition), std::move(cst));
            } else {
                condition = std::move(cst);
            }
        }
        if (condition) {
            op = mk<ram::Filter>(std::move(condition), std::move(op));
        }
        op = mk<ram::Scan>(name, 1, std::move(op));
        op = mk<ram::Scan>(lubName, 0, std::move(op));
        appendStmt(stmts, mk<ram::Query>(std::move(op)));

        // Step 3 : populate @delta() from @lub() for tuples that have nothing to lub in @concrete
        for (std::size_t i = 0; i < arity; i++) {
            values.push_back(mk<ram::TupleElement>(0, i));
        }
        op = mk<ram::Insert>(deltaName, std::move(values));
        for (std::size_t i = 0; i < arity; i++) {
            if (i < firstAuxiliary) {
                values.push_back(mk<ram::TupleElement>(0, i));
            } else {
                values.push_back(mk<ram::UndefValue>());
            }
        }
        op = mk<ram::Filter>(
                mk<ram::Negation>(mk<ram::ExistenceCheck>(name, std::move(values))), std::move(op));
        op = mk<ram::Scan>(lubName, 0, std::move(op));
        appendStmt(stmts, mk<ram::Query>(std::move(op)));
    } else {
        // we are not in the recursive loop, so the concrete relation is empty for now
        // we can just insert the content of the @lub relation into the concrete one
        for (std::size_t i = 0; i < arity; i++) {
            values.push_back(mk<ram::TupleElement>(0, i));
        }
        op = mk<ram::Insert>(name, std::move(values));
        op = mk<ram::Scan>(lubName, 0, std::move(op));
        appendStmt(stmts, mk<ram::Query>(std::move(op)));
    }

    appendStmt(stmts, mk<ram::Clear>(lubName));

    return mk<ram::Sequence>(std::move(stmts));
}

Own<ram::Statement> UnitTranslator::generateStratumExitSequence(const ast::RelationSet& scc) const {
    // Helper function to add a new term to a conjunctive condition
    auto addCondition = [&](Own<ram::Condition>& cond, Own<ram::Condition> term) {
        cond = (cond == nullptr) ? std::move(term) : mk<ram::Conjunction>(std::move(cond), std::move(term));
    };

    VecOwn<ram::Statement> exitConditions;

    // (1) if all relations in the scc are empty
    Own<ram::Condition> emptinessCheck;
    for (const ast::Relation* rel : scc) {
        if (!context->hasSubsumptiveClause(rel->getQualifiedName())) {
            addCondition(
                    emptinessCheck, mk<ram::EmptinessCheck>(getNewRelationName(rel->getQualifiedName())));
        } else {
            addCondition(
                    emptinessCheck, mk<ram::EmptinessCheck>(getDeltaRelationName(rel->getQualifiedName())));
        }
    }
    appendStmt(exitConditions, mk<ram::Exit>(std::move(emptinessCheck)));

    // (2) if the size limit has been reached for any limitsize relations
    for (const ast::Relation* rel : scc) {
        if (context->hasSizeLimit(rel)) {
            Own<ram::Condition> limit = mk<ram::Constraint>(BinaryConstraintOp::GE,
                    mk<ram::RelationSize>(getConcreteRelationName(rel->getQualifiedName())),
                    mk<ram::SignedConstant>(context->getSizeLimit(rel)));
            appendStmt(exitConditions, mk<ram::Exit>(std::move(limit)));
        }
    }

    return mk<ram::Sequence>(std::move(exitConditions));
}

/** generate RAM code for recursive relations in a strongly-connected component */
Own<ram::Statement> UnitTranslator::generateRecursiveStratum(
        const ast::RelationSet& scc, std::size_t sccNumber) const {
    assert(!scc.empty() && "scc set should not be empty");
    VecOwn<ram::Statement> result;

    // Add in the preamble
    appendStmt(result, generateStratumPreamble(scc));

    // Get all recursive join-size instrumentation statements.
    auto recursiveJoinSizeStatements = context->getRecursiveJoinSizeStatementsInSCC(sccNumber);
    auto joinSizeSequence = mk<ram::Sequence>(std::move(recursiveJoinSizeStatements));

    const std::string loop_counter = "loop_counter";
    VecOwn<ram::Expression> inc;
    inc.push_back(mk<ram::Variable>(loop_counter));
    inc.push_back(mk<ram::UnsignedConstant>(1));
    auto increment_counter = mk<ram::Assign>(mk<ram::Variable>(loop_counter),
            mk<ram::IntrinsicOperator>(FunctorOp::UADD, std::move(inc)), false);  // Counter increment each iteration
    // Add in the main fixpoint loop
    auto loopBody = generateStratumLoopBody(scc);
    auto exitSequence = generateStratumExitSequence(scc);  // If new is empty after an iteration, exit; otherwise update tables and continue.
    auto updateSequence = generateStratumTableUpdates(scc); // After each iteration, move new->table, new->delta, and clear new.
    auto fixpointLoop = mk<ram::Loop>(mk<ram::Sequence>(std::move(loopBody), std::move(joinSizeSequence),
            std::move(exitSequence), std::move(updateSequence), std::move(increment_counter)));

    appendStmt(result, mk<ram::Assign>(mk<ram::Variable>(loop_counter), mk<ram::UnsignedConstant>(1), true));  // Initial counter assignment to 1
    appendStmt(result, std::move(fixpointLoop)); // Semi-naive loop body

    // Add in the postamble
    appendStmt(result, generateStratumPostamble(scc)); // Finally clear all temporary variables
    return mk<ram::Sequence>(std::move(result));
}


VecOwn<ram::Statement> UnitTranslator::generateClauseVersionsPrefill(
        const ast::Clause* clause, const ast::RelationSet& scc, bool isDelete) const {
    const auto& sccAtoms = getSccAtoms(clause, scc);

    // Create each version
    VecOwn<ram::Statement> clauseVersions;
    for (std::size_t version = 0; version < sccAtoms.size(); version++) {
        // Deletion and insertion are translated as separate recursive clause versions.
        appendStmt(clauseVersions, context->translateRecursiveClauseInc(*clause, scc, version, Incremental, isDelete, true));
    }

    // Check that the correct number of versions have been created
    if (clause->getExecutionPlan() != nullptr) {
        std::optional<std::size_t> maxVersion;
        for (const auto& cur : clause->getExecutionPlan()->getOrders()) {
            maxVersion = std::max(cur.first, maxVersion.value_or(cur.first));
        }
        assert(sccAtoms.size() > *maxVersion && "missing clause versions");
    }

    return clauseVersions;
}


VecOwn<ram::Statement> UnitTranslator::generateClauseVersionsInc(
        const ast::Clause* clause, const ast::RelationSet& scc, bool isDelete) const {
    const auto& sccAtoms = getSccAtoms(clause, scc);

    // Create each version
    VecOwn<ram::Statement> clauseVersions;
    for (std::size_t version = 0; version < sccAtoms.size(); version++) {
        // Deletion and insertion are translated as separate recursive clause versions.
        appendStmt(clauseVersions, context->translateRecursiveClauseInc(*clause, scc, version, Incremental, isDelete));
    }

    // Check that the correct number of versions have been created
    if (clause->getExecutionPlan() != nullptr) {
        std::optional<std::size_t> maxVersion;
        for (const auto& cur : clause->getExecutionPlan()->getOrders()) {
            maxVersion = std::max(cur.first, maxVersion.value_or(cur.first));
        }
        assert(sccAtoms.size() > *maxVersion && "missing clause versions");
    }

    return clauseVersions;
}


VecOwn<ram::Statement> UnitTranslator::generateClauseVersionsIncRederive(
        const ast::Clause* clause, const ast::RelationSet& scc, std::size_t sccNumber) const {
    const auto& sccAtoms = getSccAtoms(clause, scc);

    // Create each version
    VecOwn<ram::Statement> clauseVersions;
    if (sccAtoms.empty()) {
        auto clauseVersion = context->translateRecursiveClauseIncRederive(*clause, scc, 0);
        if (glb->config().has("profile") && glb->config().has("dred-profile")) {
            const std::string phaseName = "__inc_dred_rederive_rule_" +
                                          std::to_string(context->getClauseNum(clause)) +
                                          "_v0_scc" + std::to_string(sccNumber);
            const std::string logStmt = LogStatement::tRecursiveRelation(phaseName, clause->getSrcLoc());
            clauseVersion = mk<ram::LogTimer>(std::move(clauseVersion), logStmt);
        }
        appendStmt(clauseVersions, std::move(clauseVersion));
        assert(clause->getExecutionPlan() == nullptr && "execution plan not supported");
        return clauseVersions;
    }
    for (std::size_t version = 0; version < sccAtoms.size(); version++) {
        auto clauseVersion = context->translateRecursiveClauseIncRederive(*clause, scc, version);
        if (glb->config().has("profile") && glb->config().has("dred-profile")) {
            const std::string phaseName = "__inc_dred_rederive_rule_" +
                                          std::to_string(context->getClauseNum(clause)) +
                                          "_v" + std::to_string(version) +
                                          "_scc" + std::to_string(sccNumber);
            const std::string logStmt = LogStatement::tRecursiveRelation(phaseName, clause->getSrcLoc());
            clauseVersion = mk<ram::LogTimer>(std::move(clauseVersion), logStmt);
        }
        appendStmt(clauseVersions, std::move(clauseVersion));
    }

    // Check that the correct number of versions have been created
    assert (clause->getExecutionPlan() == nullptr && "execution plan not supported");
    return clauseVersions;
}

Own<ram::Statement> UnitTranslator::translateRecursiveClausesInc(
        const ast::RelationSet& scc, const ast::Relation* rel, bool isDelete, bool isPrefill) const {
    assert(contains(scc, rel) && "relation should belong to scc");
    VecOwn<ram::Statement> code;

    // Translate each recursive clasue
    for (auto&& clause : context->getProgram()->getClauses(*rel)) {
        // Skip non-recursive and subsumptive clauses
        if (!context->isRecursiveClause(clause) || isA<ast::SubsumptiveClause>(clause)) {
            continue;
        }

        // generate all delta versions of a recursive clause
        auto clauseVersions =
            isPrefill?
            generateClauseVersionsPrefill(clause, scc, isDelete)
            :generateClauseVersionsInc(clause, scc, isDelete);
        for (auto& clauseVersion : clauseVersions) {
            appendStmt(code, std::move(clauseVersion));
        }
    }
    return mk<ram::Sequence>(std::move(code));
}

Own<ram::Statement> UnitTranslator::translateRecursiveClausesIncRederive(
        const ast::RelationSet& scc, const ast::Relation* rel, std::size_t sccNumber) const {
    assert(contains(scc, rel) && "relation should belong to scc");
    VecOwn<ram::Statement> code;

    // Translate each recursive clasue
    for (auto&& clause : context->getProgram()->getClauses(*rel)) {
        // Skip subsumptive clauses
        if (isA<ast::SubsumptiveClause>(clause)) {
            continue;
        }

        // generate all delta versions of a recursive clause
        auto clauseVersions = generateClauseVersionsIncRederive(clause, scc, sccNumber);
        for (auto& clauseVersion : clauseVersions) {
            appendStmt(code, std::move(clauseVersion));
        }
    }
    return mk<ram::Sequence>(std::move(code));
}

Own<ram::Statement> UnitTranslator::generateStratumNonSccPreFill(const ast::RelationSet& scc, bool isDelete) const {
    VecOwn<ram::Statement> prefill;

    // first translate regular recursive clauses
    // INC: deletion
    if (isDelete) {
        for (const ast::Relation* rel : scc) {
            auto relClauses = translateRecursiveClausesInc(scc, rel, true, true);
            appendStmt(prefill, mk<ram::Sequence>(std::move(relClauses)));
        }
        for (const ast::Relation* rel : scc) {
            appendStmt(prefill, mk<ram::Sequence>(
                generateMergeRelations(rel, getIncDervOverDeleteRelationName(rel->getQualifiedName()), getIncDeltaDervDeleteRelationName(rel->getQualifiedName()))
            ));
        }
        // non-recursive rules in scc could result in over-deletion
    } else {
        for (const ast::Relation* rel : scc) {
            auto relClauses = translateRecursiveClausesInc(scc, rel, false, true);
            appendStmt(prefill, mk<ram::Sequence>(std::move(relClauses)));
        }
    }
    return mk<ram::Sequence>(std::move(prefill));
    // INC: should derv info -> tuple info, delta union
    // INC: insertion

}


Own<ram::Statement> UnitTranslator::generateStratumTableUpdatesInc(const ast::RelationSet& scc, bool isDelete) const {
    VecOwn<ram::Statement> updateTable;

    for (const ast::Relation* rel : scc) {
        // Copy @new into main relation, @delta := @new, and empty out @new
        std::string mainRelation = getConcreteRelationName(rel->getQualifiedName());
        std::string newRelation = getNewRelationName(rel->getQualifiedName());
        // std::string deltaRelation = getDeltaRelationName(rel->getQualifiedName());
        // relation, oldRel, newRel,
                    // deltaDervInsertRel, deltaDervDeleteRel, deltaTupleInsertRel, deltaTupleDeleteRel
        // swap new and and delta relation and clear new relation afterwards (if not a subsumptive relation)
        Own<ram::Statement> updateRelTable;
        if (rel->getAuxiliaryArity() > 0) {
            assert ( false && "no auxiliary arity");
        } else if (!context->hasSubsumptiveClause(rel->getQualifiedName())) {
            if (isDelete) {
                updateRelTable = mk<ram::Sequence>(
                mk<ram::Clear>(getDeltaDeletionRelationName(rel->getQualifiedName())),
                    mk<ram::DeltaUnion>(mainRelation, "", mainRelation,
                        "", getNewDeletionRelationName(rel->getQualifiedName()), "", getDeltaDeletionRelationName(rel->getQualifiedName())),
                        // Recursive deletion updates both semantic delta and tuple delta relations.
                        generateMergeRelations(rel, getIncDeltaTupleDeleteRelationName(rel->getQualifiedName()), getDeltaDeletionRelationName(rel->getQualifiedName())),
                        // possibly tuples with over-deleted derivations, will be rederived
                        generateMergeRelations(rel, getIncDervOverDeleteRelationName(rel->getQualifiedName()), getNewDeletionRelationName(rel->getQualifiedName())),
                        generateMergeRelations(rel, getIncTupleOverDeleteRelationName(rel->getQualifiedName()), getDeltaDeletionRelationName(rel->getQualifiedName())),
                    mk<ram::Clear>(getNewDeletionRelationName(rel->getQualifiedName())));
            } else {
                updateRelTable = mk<ram::Sequence>(
                    mk<ram::Clear>(getDeltaInsertionRelationName(rel->getQualifiedName())), // clear old delta, use delta union to update it with new
                    mk<ram::DeltaUnion>(mainRelation, "", mainRelation,
                        getNewInsertionRelationName(rel->getQualifiedName()), "", getDeltaInsertionRelationName(rel->getQualifiedName()), ""),
                        generateMergeRelations(rel, getIncDeltaTupleInsertRelationName(rel->getQualifiedName()), getDeltaInsertionRelationName(rel->getQualifiedName())),
                        mk<ram::Clear>(getNewInsertionRelationName(rel->getQualifiedName())));
            }
        } else {
            assert (false && "no subsumptive clause");
        }

        // Measure update time
        if (glb->config().has("profile")) {
            updateRelTable = mk<ram::LogRelationTimer>(std::move(updateRelTable),
                    LogStatement::cRecursiveRelation(toString(rel->getQualifiedName()), rel->getSrcLoc()),
                    newRelation);
        }

        appendStmt(updateTable, std::move(updateRelTable));
    }
    return mk<ram::Sequence>(std::move(updateTable));
}


Own<ram::Statement> UnitTranslator::generateStratumLoopBodyInc(const ast::RelationSet& scc, bool isDelete) const {
    VecOwn<ram::Statement> loopBody;

    // first translate regular recursive clauses
    // INC: deletion
    if (isDelete) {
        for (const ast::Relation* rel : scc) {
            auto relClauses = translateRecursiveClausesInc(scc, rel, true);
            appendStmt(loopBody, mk<ram::Sequence>(std::move(relClauses)));
        }
    } else {
        for (const ast::Relation* rel : scc) {
            auto relClauses = translateRecursiveClausesInc(scc, rel, false);
            appendStmt(loopBody, mk<ram::Sequence>(std::move(relClauses)));
        }
    }
    return mk<ram::Sequence>(std::move(loopBody));
    // INC: should derv info -> tuple info, delta union
    // INC: insertion

}


Own<ram::Statement> UnitTranslator::generateStratumPostambleInc(const ast::RelationSet& scc, bool isDelete) const {
    VecOwn<ram::Statement> postamble;

    for (const ast::Relation* rel : scc) {
        // Drop temporary tables after recursion
        if (isDelete) {
            appendStmt(postamble, mk<ram::Clear>(getDeltaDeletionRelationName(rel->getQualifiedName())));
            appendStmt(postamble, mk<ram::Clear>(getNewDeletionRelationName(rel->getQualifiedName())));
        } else {
            appendStmt(postamble, mk<ram::Clear>(getDeltaInsertionRelationName(rel->getQualifiedName())));
            appendStmt(postamble, mk<ram::Clear>(getNewInsertionRelationName(rel->getQualifiedName())));
        }
    }
    return mk<ram::Sequence>(std::move(postamble));
}


Own<ram::Statement> UnitTranslator::generateStratumExitSequenceInc(const ast::RelationSet& scc, bool isDelete) const {
    // Helper function to add a new term to a conjunctive condition
    auto addCondition = [&](Own<ram::Condition>& cond, Own<ram::Condition> term) {
        cond = (cond == nullptr) ? std::move(term) : mk<ram::Conjunction>(std::move(cond), std::move(term));
    };

    VecOwn<ram::Statement> exitConditions;

    // (1) if all relations in the scc are empty
    Own<ram::Condition> emptinessCheck;
    for (const ast::Relation* rel : scc) {
        if (!context->hasSubsumptiveClause(rel->getQualifiedName())) {
            if (isDelete) {
                addCondition(
                        emptinessCheck, mk<ram::EmptinessCheck>(getNewDeletionRelationName(rel->getQualifiedName())));
            } else {
                addCondition(
                        emptinessCheck, mk<ram::EmptinessCheck>(getNewInsertionRelationName(rel->getQualifiedName())));
            }
        } else {
            assert (false && "non subsumptive clause");
        }
    }
    appendStmt(exitConditions, mk<ram::Exit>(std::move(emptinessCheck)));

    // (2) if the size limit has been reached for any limitsize relations
    for (const ast::Relation* rel : scc) {
        if (context->hasSizeLimit(rel)) {
            Own<ram::Condition> limit = mk<ram::Constraint>(BinaryConstraintOp::GE,
                    mk<ram::RelationSize>(getConcreteRelationName(rel->getQualifiedName())),
                    mk<ram::SignedConstant>(context->getSizeLimit(rel)));
            appendStmt(exitConditions, mk<ram::Exit>(std::move(limit)));
        }
    }

    return mk<ram::Sequence>(std::move(exitConditions));
}

/**
 * for Dred, over-deletion and rederive
 * - before rederive, move tuples with over-deleted derivations to @inc_delta_derv_overdelete
 * - during rederive, rederive derivations to @inc_delta_derv_rederive
 * - after each iteration of rederive, delta union tuples (can be considered as insertion )
     and find real deleted tuples @inc_delta_tuple_rederive, update original relation at the same time
 * - and @inc_delta_tuple_overdelete will minus @inc_delta_tuple_rederive
 * if inc_delta_derv_overdelete, the rederivation finishes.
 * @inc_delta_tuple_overdelete will be $inc_delta_tuple_delete (swap and clear)
 */
// Need to scan delta derivations separately; other parts follow the full algorithm.
// Probably need new relations.
Own<ram::Statement> UnitTranslator::generateStratumLoopBodyIncRederive(
        const ast::RelationSet& scc, std::size_t sccNumber) const {
    VecOwn<ram::Statement> loopBody;
    for (const ast::Relation* rel : scc) {
        auto relClauses = translateRecursiveClausesIncRederive(scc, rel, sccNumber);
        appendStmt(loopBody, mk<ram::Sequence>(std::move(relClauses)));
    }
    return mk<ram::Sequence>(std::move(loopBody));
}
// Can largely reuse DeltaUnion functionality for insert.
// Also need to update delete tuple/derivation.
Own<ram::Statement> UnitTranslator::generateStratumTableUpdatesIncRederive(const ast::RelationSet& scc) const {
    VecOwn<ram::Statement> updateTable;
    for (const ast::Relation* rel : scc) {
        assert (rel->getAuxiliaryArity() <= 0 && "no support for auxiliary arity");
        assert (!context->hasSubsumptiveClause(rel->getQualifiedName()) && "no support for subsumptive clause");
        std::string mainRelation = getConcreteRelationName(rel->getQualifiedName());
        Own<ram::Statement> updateRelTable;
        updateRelTable = mk<ram::Sequence>(
            // clear old delta, use delta union to update it with new
            mk<ram::Clear>(getIncDeltaTupleRederiveRelationName(rel->getQualifiedName())),
            // rederived derivations, if rederive a tuple, will be added to main relation
            mk<ram::DeltaUnion>(mainRelation, "", mainRelation,
                                    getIncNewDervRederiveRelationName(rel->getQualifiedName()), "", getIncDeltaTupleRederiveRelationName(rel->getQualifiedName()), ""),
            // should also update overdelete info: overdelete derv, tuple
            generateEraseTuples(rel, getIncTupleOverDeleteRelationName(rel->getQualifiedName()), getIncDeltaTupleRederiveRelationName(rel->getQualifiedName())),
            // clear the new relation
            mk<ram::Clear>(getIncNewDervRederiveRelationName(rel->getQualifiedName()))
        );
        appendStmt(updateTable, std::move(updateRelTable));
    }
    return mk<ram::Sequence>(std::move(updateTable));
}
Own<ram::Statement> UnitTranslator::generateStratumExitSequenceIncRederive(const ast::RelationSet& scc) const {
    // Helper function to add a new term to a conjunctive condition
    auto addCondition = [&](Own<ram::Condition>& cond, Own<ram::Condition> term) {
        cond = (cond == nullptr) ? std::move(term) : mk<ram::Conjunction>(std::move(cond), std::move(term));
    };

    VecOwn<ram::Statement> exitConditions;

    // (1) if all relations in the scc are empty
    Own<ram::Condition> emptinessCheck;
    for (const ast::Relation* rel : scc) {
        if (!context->hasSubsumptiveClause(rel->getQualifiedName())) {
            addCondition(
                    emptinessCheck, mk<ram::EmptinessCheck>(getIncNewDervRederiveRelationName(rel->getQualifiedName())));
        } else {
            assert (false && "non subsumptive clause");
        }
    }
    appendStmt(exitConditions, mk<ram::Exit>(std::move(emptinessCheck)));

    // (2) if the size limit has been reached for any limitsize relations
    for (const ast::Relation* rel : scc) {
        if (context->hasSizeLimit(rel)) {
            Own<ram::Condition> limit = mk<ram::Constraint>(BinaryConstraintOp::GE,
                    mk<ram::RelationSize>(getConcreteRelationName(rel->getQualifiedName())),
                    mk<ram::SignedConstant>(context->getSizeLimit(rel)));
            appendStmt(exitConditions, mk<ram::Exit>(std::move(limit)));
        }
    }

    return mk<ram::Sequence>(std::move(exitConditions));
}
Own<ram::Statement> UnitTranslator::generateStratumPostambleIncRederive(const ast::RelationSet& scc) const {
    // getIncTupleOverDeleteRelationName
    // getIncDeltaTupleDeleteRelationName
    VecOwn<ram::Statement> postamble;
    for (const ast::Relation* rel : scc) {
        // swap, get a correct delta delete; but cannot just use swap for it will be reference-based
        appendStmt(postamble,
        mk<ram::Sequence>(
                mk<ram::ExactClear>(getIncDeltaTupleDeleteRelationName(rel->getQualifiedName())),
                generateMergeRelations(rel, getIncDeltaTupleDeleteRelationName(rel->getQualifiedName()), getIncTupleOverDeleteRelationName(rel->getQualifiedName()))
            )
        );
        appendStmt(postamble, mk<ram::Clear>(getIncNewDervRederiveRelationName(rel->getQualifiedName())));
        appendStmt(postamble, mk<ram::Clear>(getIncDeltaTupleRederiveRelationName(rel->getQualifiedName())));
        appendStmt(postamble, mk<ram::Clear>(getIncTupleOverDeleteRelationName(rel->getQualifiedName())));
        appendStmt(postamble, mk<ram::Clear>(getIncDervOverDeleteRelationName(rel->getQualifiedName())));
    }
    return mk<ram::Sequence>(std::move(postamble));
}

Own<ram::Statement> UnitTranslator::generateStratumRederive(
        const ast::RelationSet& scc, std::size_t sccNumber) const {
    VecOwn<ram::Statement> result;
    const ast::Relation* phaseRel = *scc.begin();
    const auto phaseLoc = phaseRel->getSrcLoc();
    auto makeLoopTrace = [&]() -> Own<ram::Statement> {
        VecOwn<ram::Statement> logs;
        auto addLog = [&](const ast::Relation* rel, const std::string& relName, const std::string& kind) {
            std::ostringstream msg;
            msg << "@dred-loop;phase=rederive"
                << ";scc=" << sccNumber
                << ";rel=" << toString(rel->getQualifiedName())
                << ";kind=" << kind;
            appendStmt(logs, mk<ram::LogSize>(relName, msg.str()));
        };
        for (const ast::Relation* rel : scc) {
            addLog(rel, getIncNewDervRederiveRelationName(rel->getQualifiedName()), "new_rederive");
            addLog(rel, getIncDeltaTupleRederiveRelationName(rel->getQualifiedName()), "delta_tuple_rederive");
            addLog(rel, getIncTupleOverDeleteRelationName(rel->getQualifiedName()), "tuple_overdelete");
            addLog(rel, getIncDervOverDeleteRelationName(rel->getQualifiedName()), "derv_overdelete");
        }
        if (logs.empty()) {
            return mk<ram::EmptyStatement>();
        }
        return mk<ram::Sequence>(std::move(logs));
    };
    auto wrapSubTimer = [&](const std::string& label, Own<ram::Statement> stmt) -> Own<ram::Statement> {
        if (!glb->config().has("profile") || !glb->config().has("dred-profile")) {
            return stmt;
        }
        const std::string phaseName = "__inc_dred_rederive_" + label + "_scc" +
                                      std::to_string(static_cast<unsigned long long>(sccNumber));
        const std::string logStmt = LogStatement::tRecursiveRelation(phaseName, phaseLoc);
        return mk<ram::LogTimer>(std::move(stmt), logStmt);
    };

    const std::string loop_counter = "loop_counter_rederive";
    VecOwn<ram::Expression> inc;
    inc.push_back(mk<ram::Variable>(loop_counter));
    inc.push_back(mk<ram::UnsignedConstant>(1));
    auto increment_counter = mk<ram::Assign>(mk<ram::Variable>(loop_counter),
            mk<ram::IntrinsicOperator>(FunctorOp::UADD, std::move(inc)), false);  // Counter increment each iteration
    // Add in the main fixpoint loop
    auto loopBody = wrapSubTimer("loop_body", generateStratumLoopBodyIncRederive(scc, sccNumber));
    auto loopTrace = makeLoopTrace();
    auto exitSequence = wrapSubTimer("loop_exit",
            generateStratumExitSequenceIncRederive(scc));  // If new is empty after an iteration, exit; otherwise update tables and continue.
    auto updateSequence = wrapSubTimer("loop_update",
            generateStratumTableUpdatesIncRederive(scc)); // After each iteration, move new->table, new->delta, and clear new.
    auto fixpointLoop = mk<ram::Loop>(mk<ram::Sequence>(std::move(loopBody),
            std::move(loopTrace), std::move(exitSequence),
            std::move(updateSequence), std::move(increment_counter)));

    VecOwn<ram::Statement> prefill;
    for (const ast::Relation* rel : scc) {
        auto deltaRederive = getIncDeltaTupleRederiveRelationName(rel->getQualifiedName());
        auto tupleOverdelete = getIncTupleOverDeleteRelationName(rel->getQualifiedName());
        appendStmt(prefill, mk<ram::Clear>(deltaRederive));
        appendStmt(prefill, generateMergeRelations(rel, deltaRederive, tupleOverdelete));
    }

    appendStmt(result, wrapSubTimer("prefill", mk<ram::Sequence>(std::move(prefill))));
    appendStmt(result, mk<ram::Assign>(mk<ram::Variable>(loop_counter), mk<ram::UnsignedConstant>(1), true));  // Initial counter assignment to 1
    appendStmt(result, std::move(fixpointLoop)); // Semi-naive loop body
    // Add in the postamble
    appendStmt(result, wrapSubTimer("postamble", generateStratumPostambleIncRederive(scc))); // Finally clear all temporary variables

    return mk<ram::Sequence>(std::move(result));
}

/** generate RAM code for recursive relations in a strongly-connected component */
Own<ram::Statement> UnitTranslator::generateRecursiveStratumInc(
        const ast::RelationSet& scc, std::size_t sccNumber) const {
    assert(!scc.empty() && "scc set should not be empty");
    VecOwn<ram::Statement> result;
    const ast::Relation* phaseRel = *scc.begin();
    const auto phaseLoc = phaseRel->getSrcLoc();
    auto makeLoopTrace = [&](const std::string& phase) -> Own<ram::Statement> {
        VecOwn<ram::Statement> logs;
        auto addLog = [&](const ast::Relation* rel, const std::string& relName, const std::string& kind) {
            std::ostringstream msg;
            msg << "@dred-loop;phase=" << phase
                << ";scc=" << sccNumber
                << ";rel=" << toString(rel->getQualifiedName())
                << ";kind=" << kind;
            appendStmt(logs, mk<ram::LogSize>(relName, msg.str()));
        };
        for (const ast::Relation* rel : scc) {
            if (phase == "delete") {
                addLog(rel, getNewDeletionRelationName(rel->getQualifiedName()), "new_delete");
                addLog(rel, getDeltaDeletionRelationName(rel->getQualifiedName()), "delta_delete");
                addLog(rel, getIncDeltaTupleDeleteRelationName(rel->getQualifiedName()), "inc_delta_tuple_delete");
                addLog(rel, getIncTupleOverDeleteRelationName(rel->getQualifiedName()), "inc_tuple_overdelete");
                addLog(rel, getIncDervOverDeleteRelationName(rel->getQualifiedName()), "inc_derv_overdelete");
            } else if (phase == "insert") {
                addLog(rel, getNewInsertionRelationName(rel->getQualifiedName()), "new_insert");
                addLog(rel, getDeltaInsertionRelationName(rel->getQualifiedName()), "delta_insert");
                addLog(rel, getIncDeltaTupleInsertRelationName(rel->getQualifiedName()), "inc_delta_tuple_insert");
            }
        }
        if (logs.empty()) {
            return mk<ram::EmptyStatement>();
        }
        return mk<ram::Sequence>(std::move(logs));
    };
    auto wrapPhaseTimer = [&](const std::string& phase, Own<ram::Statement> stmt) -> Own<ram::Statement> {
        if (!glb->config().has("profile") || !glb->config().has("dred-profile")) {
            return stmt;
        }
        const std::string phaseName =
                "__inc_dred_" + phase + "_scc" + std::to_string(static_cast<unsigned long long>(sccNumber));
        const std::string logStmt = LogStatement::tRecursiveRelation(phaseName, phaseLoc);
        return mk<ram::LogTimer>(std::move(stmt), logStmt);
    };
    auto wrapPhaseSubTimer = [&](const std::string& phase, const std::string& label,
                                     Own<ram::Statement> stmt) -> Own<ram::Statement> {
        if (!glb->config().has("profile") || !glb->config().has("dred-profile")) {
            return stmt;
        }
        const std::string phaseName = "__inc_dred_" + phase + "_" + label + "_scc" +
                                      std::to_string(static_cast<unsigned long long>(sccNumber));
        const std::string logStmt = LogStatement::tRecursiveRelation(phaseName, phaseLoc);
        return mk<ram::LogTimer>(std::move(stmt), logStmt);
    };

    // delete phase
    VecOwn<ram::Statement> deletePhase;
    // explicit copy old to new for recursive case... delta union do not copy old to new in recursive case
    {
        VecOwn<ram::Statement> copyOld;
        for (const ast::Relation* rel : scc) {
            appendStmt(copyOld, generateMergeRelations(rel, getConcreteRelationName(rel->getQualifiedName()),
                                          getOldRelationName(rel->getQualifiedName())));
        }
        appendStmt(deletePhase, wrapPhaseSubTimer("delete", "copy_old",
                                      mk<ram::Sequence>(std::move(copyOld))));
    }
    appendStmt(deletePhase, wrapPhaseSubTimer("delete", "preamble", generateStratumPreambleInc(scc, true)));
    {
        auto prefill = generateStratumNonSccPreFill(scc, true);
        auto updateSequence = generateStratumTableUpdatesInc(scc, true);
        appendStmt(deletePhase, wrapPhaseSubTimer("delete", "prefill", std::move(prefill)));
        appendStmt(deletePhase, wrapPhaseSubTimer("delete", "prefill_update", std::move(updateSequence)));
    }
    {
        const std::string loop_counter = "loop_counter1";
        VecOwn<ram::Expression> inc;
        inc.push_back(mk<ram::Variable>(loop_counter));
        inc.push_back(mk<ram::UnsignedConstant>(1));
        auto increment_counter = mk<ram::Assign>(mk<ram::Variable>(loop_counter),
                mk<ram::IntrinsicOperator>(FunctorOp::UADD, std::move(inc)), false);
        auto loopBody = wrapPhaseSubTimer("delete", "loop_body", generateStratumLoopBodyInc(scc, true));
        auto loopTrace = makeLoopTrace("delete");
        auto exitSequence = wrapPhaseSubTimer("delete", "loop_exit",
                generateStratumExitSequenceInc(scc, true));
        auto updateSequence = wrapPhaseSubTimer("delete", "loop_update",
                generateStratumTableUpdatesInc(scc, true));
        auto fixpointLoop = mk<ram::Loop>(mk<ram::Sequence>(std::move(loopBody),
                std::move(loopTrace), std::move(exitSequence),
                std::move(updateSequence), std::move(increment_counter)));

        appendStmt(deletePhase, mk<ram::Assign>(mk<ram::Variable>(loop_counter),
                                      mk<ram::UnsignedConstant>(1), true));
        appendStmt(deletePhase, std::move(fixpointLoop));
        appendStmt(deletePhase, wrapPhaseSubTimer("delete", "postamble",
                                      generateStratumPostambleInc(scc, true)));
    }
    appendStmt(result, wrapPhaseTimer("delete", mk<ram::Sequence>(std::move(deletePhase))));

    // rederive phase
    appendStmt(result, wrapPhaseTimer("rederive", generateStratumRederive(scc, sccNumber)));

    {
        VecOwn<ram::Statement> snapshotPostDelete;
        for (const ast::Relation* rel : scc) {
            const std::string postDeleteRelation = getPostDeleteRelationName(rel->getQualifiedName());
            appendStmt(snapshotPostDelete, mk<ram::Clear>(postDeleteRelation));
            appendStmt(snapshotPostDelete, generateMergeRelations(rel, postDeleteRelation,
                    getConcreteRelationName(rel->getQualifiedName())));
        }
        appendStmt(result, wrapPhaseTimer("post_delete_snapshot",
                mk<ram::Sequence>(std::move(snapshotPostDelete))));
    }

    // insert phase
    VecOwn<ram::Statement> insertPhase;
    appendStmt(insertPhase, wrapPhaseSubTimer("insert", "preamble", generateStratumPreambleInc(scc, false)));
    {
        auto prefill = generateStratumNonSccPreFill(scc, false);
        auto updateSequence = generateStratumTableUpdatesInc(scc, false);
        appendStmt(insertPhase, wrapPhaseSubTimer("insert", "prefill", std::move(prefill)));
        appendStmt(insertPhase, wrapPhaseSubTimer("insert", "prefill_update", std::move(updateSequence)));
    }
    {
        const std::string loop_counter = "loop_counter2";
        VecOwn<ram::Expression> inc;
        inc.push_back(mk<ram::Variable>(loop_counter));
        inc.push_back(mk<ram::UnsignedConstant>(1));
        auto increment_counter = mk<ram::Assign>(mk<ram::Variable>(loop_counter),
                mk<ram::IntrinsicOperator>(FunctorOp::UADD, std::move(inc)), false);
        auto loopBody = wrapPhaseSubTimer("insert", "loop_body", generateStratumLoopBodyInc(scc, false));
        auto loopTrace = makeLoopTrace("insert");
        auto exitSequence = wrapPhaseSubTimer("insert", "loop_exit",
                generateStratumExitSequenceInc(scc, false));
        auto updateSequence = wrapPhaseSubTimer("insert", "loop_update",
                generateStratumTableUpdatesInc(scc, false));
        auto fixpointLoop = mk<ram::Loop>(mk<ram::Sequence>(std::move(loopBody),
                std::move(loopTrace), std::move(exitSequence),
                std::move(updateSequence), std::move(increment_counter)));

        appendStmt(insertPhase, mk<ram::Assign>(mk<ram::Variable>(loop_counter),
                                      mk<ram::UnsignedConstant>(1), true));
        appendStmt(insertPhase, std::move(fixpointLoop));
        appendStmt(insertPhase, wrapPhaseSubTimer("insert", "postamble",
                                      generateStratumPostambleInc(scc, false)));
    }
    appendStmt(result, wrapPhaseTimer("insert", mk<ram::Sequence>(std::move(insertPhase))));

    return mk<ram::Sequence>(std::move(result));
}


void UnitTranslator::addAuxiliaryArity(
        const ast::Relation* /* relation */, std::map<std::string, std::string>& directives) const {
    directives.insert(std::make_pair("auxArity", "0"));
}

Own<ram::Statement> UnitTranslator::generateLoadRelation(const ast::Relation* relation) const {
    VecOwn<ram::Statement> loadStmts;
    for (const auto* load : context->getLoadDirectives(relation->getQualifiedName())) {
        // Set up the corresponding directive map
        std::map<std::string, std::string> directives;
        for (const auto& [key, value] : load->getParameters()) {
            directives.insert(std::make_pair(key, unescape(value)));
        }
        directives.insert(std::make_pair("incDelta", "false"));
        directives.insert(std::make_pair("inc-insert", "false"));
        directives.insert(std::make_pair("inc-delete", "false"));
        directives.insert(std::make_pair("cache", "true"));  // maintain a copy of input facts

        if (glb->config().has("no-warn")) {
            directives.insert(std::make_pair("no-warn", "true"));
        }
        addAuxiliaryArity(relation, directives);

        // Create the resultant load statement, with profile information
        std::string ramRelationName = getConcreteRelationName(relation->getQualifiedName());
        Own<ram::Statement> loadStmt = mk<ram::IO>(ramRelationName, directives);
        if (glb->config().has("profile")) {
            const std::string logTimerStatement =
                    LogStatement::tRelationLoadTime(ramRelationName, relation->getSrcLoc());
            loadStmt = mk<ram::LogRelationTimer>(std::move(loadStmt), logTimerStatement, ramRelationName);
        }
        appendStmt(loadStmts, std::move(loadStmt));
    }
    return mk<ram::Sequence>(std::move(loadStmts));
}

// Incremental loads seed old/current snapshots and per-turn insert/delete deltas.
Own<ram::Statement> UnitTranslator::generateLoadRelationInc(const ast::Relation* relation) const {
    VecOwn<ram::Statement> loadStmts;
    for (const auto* load : context->getLoadDirectives(relation->getQualifiedName())) {
        // Set up the corresponding directive map
        std::map<std::string, std::string> directives;
        for (const auto& [key, value] : load->getParameters()) {
            directives.insert(std::make_pair(key, unescape(value)));
        }
        if (glb->config().has("no-warn")) {
            directives.insert(std::make_pair("no-warn", "true"));
        }
        addAuxiliaryArity(relation, directives);
        directives.insert(std::make_pair("incDelta", "false"));
        directives.insert(std::make_pair("inc-insert", "false"));
        directives.insert(std::make_pair("inc-delete", "false"));

        // base relation
        std::string ramRelationName = getRelationName(relation->getQualifiedName());
        std::string ramOldRelationName = getOldRelationName(relation->getQualifiedName());
        std::string ramPostDeleteRelationName = getPostDeleteRelationName(relation->getQualifiedName());

        auto rebuildInputViewsStmt =
            mk<ram::Sequence>(
                mk<ram::ExactClear>(ramPostDeleteRelationName),
                generateMergeRelationsWithFilter(relation,
                    ramPostDeleteRelationName,
                    ramOldRelationName,
                    getIncDeltaTupleDeleteRelationName(relation->getQualifiedName())),
                mk<ram::ExactClear>(ramRelationName),
                generateMergeRelations(relation,
                    getConcreteRelationName(relation->getQualifiedName()),
                    ramPostDeleteRelationName),
                generateMergeRelations(relation,
                    getConcreteRelationName(relation->getQualifiedName()),
                    getIncDeltaTupleInsertRelationName(relation->getQualifiedName()))
            );

        Own<ram::Statement> stmts = mk<ram::Sequence>(std::move(rebuildInputViewsStmt));
        if (glb->config().has("profile")) {
            const std::string logTimerStatement =
                    LogStatement::tRelationLoadTime(ramRelationName, relation->getSrcLoc());
            stmts = mk<ram::LogRelationTimer>(std::move(stmts), logTimerStatement, ramRelationName);
        }
        appendStmt(loadStmts, std::move(stmts));
    }
    return mk<ram::Sequence>(std::move(loadStmts));
}

Own<ram::Statement> UnitTranslator::generateStoreRelation(const ast::Relation* relation) const {
    VecOwn<ram::Statement> storeStmts;
    for (const auto* store : context->getStoreDirectives(relation->getQualifiedName())) {
        // Set up the corresponding directive map
        std::map<std::string, std::string> directives;
        for (const auto& [key, value] : store->getParameters()) {
            directives.insert(std::make_pair(key, unescape(value)));
        }
        directives.insert(std::make_pair("incDelta", "false"));
        directives.insert(std::make_pair("inc-insert", "false"));
        directives.insert(std::make_pair("inc-delete", "false"));
        addAuxiliaryArity(relation, directives);

        // Create the resultant store statement, with profile information
        std::string ramRelationName = getConcreteRelationName(relation->getQualifiedName());
        Own<ram::Statement> storeStmt = mk<ram::IO>(ramRelationName, directives);
        if (glb->config().has("profile")) {
            const std::string logTimerStatement =
                    LogStatement::tRelationSaveTime(ramRelationName, relation->getSrcLoc());
            storeStmt = mk<ram::LogRelationTimer>(std::move(storeStmt), logTimerStatement, ramRelationName);
        }
        appendStmt(storeStmts, std::move(storeStmt));
    }
    return mk<ram::Sequence>(std::move(storeStmts));
}

Own<ram::Relation> UnitTranslator::createRamRelation(
        const ast::Relation* baseRelation, std::string ramRelationName) const {
    auto arity = baseRelation->getArity();

    bool mergeAuxiliary = (ramRelationName != getNewRelationName(baseRelation->getQualifiedName()));

    auto auxArity = mergeAuxiliary ? baseRelation->getAuxiliaryArity() : 0;
    auto representation = RelationRepresentation::BTREE_DELETE;
    if (representation == RelationRepresentation::BTREE_DELETE
        && (ramRelationName[0] == '@' || ramRelationName[0] == '$')) {
        representation = RelationRepresentation::DEFAULT;
    }
    if (ramRelationName.find("tuple_overdelete") != std::string::npos) {
        representation = RelationRepresentation::BTREE_DELETE;
    }

    std::vector<std::string> attributeNames;
    std::vector<std::string> attributeTypeQualifiers;
    for (const auto& attribute : baseRelation->getAttributes()) {
        attributeNames.push_back(attribute->getName());
        attributeTypeQualifiers.push_back(context->getAttributeTypeQualifier(attribute->getTypeName()));
    }

    return mk<ram::Relation>(
            ramRelationName, arity, auxArity, attributeNames, attributeTypeQualifiers, representation);
}

VecOwn<ram::Relation> UnitTranslator::createRamRelations(const std::vector<std::size_t>& sccOrdering) const {
     VecOwn<ram::Relation> ramRelations;
    for (const auto& scc : sccOrdering) {
        bool isRecursive = context->isRecursiveSCC(scc);
        for (const auto& rel : context->getRelationsInSCC(scc)) {
            // Add main relation
            std::string mainName = getConcreteRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, mainName));

            if (isRecursive || rel->getAuxiliaryArity() > 0) {
                // Add new relation
                std::string newName = getNewRelationName(rel->getQualifiedName());
                ramRelations.push_back(createRamRelation(rel, newName));
            }

            // Recursive relations also require @delta and @new variants, with the same signature
            if (isRecursive) {
                // Add delta relation
                std::string deltaName = getDeltaRelationName(rel->getQualifiedName());
                ramRelations.push_back(createRamRelation(rel, deltaName));
            }

            // Add relation that cache old result
            if (glb->config().has("full-only")) {
                continue;
            }
            std::string oldName = getOldRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, oldName));

            // Snapshot used by mixed-update non-recursive clauses after delete
            // processing and, for recursive SCCs, after rederive.
            std::string postDeleteName = getPostDeleteRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, postDeleteName));

            // INC: Add delta relation for derivation-changing tuples in incremental computation
            // We will have @inc_delta_derv_[insert|delete]_ and @inc_delta_tuple_[insert|delete}_ relations
            // The former one tracks those tuples that change its derivations
            // The later one tracks the inserted and deleted tuples for real
            // i.e., newly inserted tuples and newly deleted tuples
            // Only those tuples cause further derivation info (rule application) changes in our setting

            std::string incDeltaDervInsertName = getIncDeltaDervInsertRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, incDeltaDervInsertName));

            std::string incDeltaDervDeleteName = getIncDeltaDervDeleteRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, incDeltaDervDeleteName));

            std::string incDeltaTupleInsertName = getIncDeltaTupleInsertRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, incDeltaTupleInsertName));

            std::string incDeltaTupleDeleteName = getIncDeltaTupleDeleteRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, incDeltaTupleDeleteName));

            std::string tmpName = getTmpRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, tmpName));

            std::string tmp2Name = getTmp2RelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, tmp2Name));

            std::string tmp3Name = getTmp3RelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, tmp3Name));

            std::string tmp4Name = getTmp4RelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, tmp4Name));

            std::string incTupleOverdelete = getIncTupleOverDeleteRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, incTupleOverdelete));

            std::string incDervOverdelete = getIncDervOverDeleteRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, incDervOverdelete));

            std::string incNewTupleRederive = getIncNewDervRederiveRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, incNewTupleRederive));

            std::string incDeltaDervRederive = getIncDeltaTupleRederiveRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, incDeltaDervRederive));


            // for recursion: delta and new, now have deletion and insertion version
            // delta - real tuple change, new - derivation change & record derivation

            if (rel->getAuxiliaryArity() > 0) {
                assert(false && "does not support lub relation");
                // Add lub relation
                std::string lubName = getLubRelationName(rel->getQualifiedName());
                ramRelations.push_back(createRamRelation(rel, lubName));
            }

            if (rel->getAuxiliaryArity() > 0) {
                assert(false && "does not support AUXILIARY ARITY");
            }

            // Incremental recursive delta/new relations carry deletion and insertion phases separately.
            std::string deltaDelName = getDeltaDeletionRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, deltaDelName));
            std::string deltaInsertName = getDeltaInsertionRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, deltaInsertName));
            std::string newDelName = getNewDeletionRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, newDelName));
            std::string newInsertName = getNewInsertionRelationName(rel->getQualifiedName());
            ramRelations.push_back(createRamRelation(rel, newInsertName));
        }
    }
    return ramRelations;
}

Own<ram::Sequence> UnitTranslator::generateProgram(const ast::TranslationUnit& translationUnit) {
    // Check if trivial program
    if (context->getNumberOfSCCs() == 0) {
        return mk<ram::Sequence>();
    }
    const auto& sccOrdering =
            translationUnit.getAnalysis<ast::analysis::TopologicallySortedSCCGraphAnalysis>().order();
    VecOwn<ram::Statement> res;
    VecOwn<ram::Statement> incRes;

    // Create subroutines for each SCC according to topological order
    for (std::size_t i = 0; i < sccOrdering.size(); i++) {
        // Generate the main stratum code
        auto stratum = generateStratum(sccOrdering.at(i));

        // Clear expired relations
        const auto& expiredRelations = context->getExpiredRelations(i);
        stratum = mk<ram::Sequence>(std::move(stratum), generateClearExpiredRelations(expiredRelations));

        // Add the subroutine
        const ast::Relation* rel = *context->getRelationsInSCC(sccOrdering.at(i)).begin();

        std::string stratumID = rel->getQualifiedName().toString();

        addRamSubroutine(stratumID, std::move(stratum));
        // invoke the strata
        appendStmt(res, mk<ram::Call>("stratum_" + stratumID));
    }

    // Add main timer if profiling
    if (!res.empty() && glb->config().has("profile")) {
        auto newStmt = mk<ram::LogTimer>(mk<ram::Sequence>(std::move(res)), LogStatement::runtime());
        res.clear();
        appendStmt(res, std::move(newStmt));
    }

    // Program translated!
    return mk<ram::Sequence>(std::move(res));
}

Own<ram::Statement> UnitTranslator::translateProbQuery(const ast::ProbQuery& probQuery) {
    return mk<ram::EmptyStatement>();
}
Own<ram::Statement> UnitTranslator::translateEvidence(const ast::Evidence& evidence) {
    return mk<ram::EmptyStatement>();
}


Own<ram::Statement> UnitTranslator::generateIncTableUpdate(const std::vector<std::size_t>& sccOrderings) const {
    VecOwn<ram::Statement> res;
    for (std::size_t i = 0; i < sccOrderings.size(); i++) {
        for (auto& rel : context->getRelationsInSCC(sccOrderings.at(i))) {
            auto oldRelationName = getOldRelationName(rel->getQualifiedName());
            auto relationName = getConcreteRelationName(rel->getQualifiedName());
            appendStmt(res, mk<ram::Clear>(oldRelationName));
            appendStmt(res,
                generateMergeRelations(rel, oldRelationName, relationName));
            appendStmt(res, mk<ram::Clear>(getPostDeleteRelationName(rel->getQualifiedName())));
            auto overdeleteName = getIncTupleOverDeleteRelationName(rel->getQualifiedName());
            auto dervOverdeleteName = getIncDervOverDeleteRelationName(rel->getQualifiedName());
            appendStmt(res, mk<ram::ExactClear>(overdeleteName));
            appendStmt(res, mk<ram::ExactClear>(dervOverdeleteName));
            // also copy getIncDeltaTupleDeleteRelationName to DeltaDervDelete
            appendStmt(res,
                generateMergeRelations(rel,
                    getIncDeltaTupleDeleteRelationName(rel->getQualifiedName()),
                    getIncDeltaDervDeleteRelationName(rel->getQualifiedName()))
                );
        }
    }
    return mk<ram::Sequence>(std::move(res));
}

Own<ram::Sequence> UnitTranslator::generateProgramInc(const ast::TranslationUnit& translationUnit) {
    // Check if trivial program
    if (context->getNumberOfSCCs() == 0) {
        return mk<ram::Sequence>();
    }

    VecOwn<ram::Statement> incRes;

    const auto& sccOrdering =
            translationUnit.getAnalysis<ast::analysis::TopologicallySortedSCCGraphAnalysis>().order();

    // Incremental computation refreshes old/post-delete state before each turn.
    std::string stratumIncTableUpdate = "inc_table_update";
    addRamSubroutine(stratumIncTableUpdate, generateIncTableUpdate(sccOrdering));
    appendStmt(incRes, mk<ram::Call>("stratum_" + stratumIncTableUpdate));

    // Create subroutines for each SCC according to topological order
    for (std::size_t i = 0; i < sccOrdering.size(); i++) {
        // Generate the main stratum code
        auto stratumInc = generateStratumInc(sccOrdering.at(i));

        // Clear expired relations
        const auto& expiredRelations = context->getExpiredRelations(i);
        stratumInc = mk<ram::Sequence>(std::move(stratumInc), generateClearExpiredRelations(expiredRelations));

        // Add the subroutine
        const ast::Relation* rel = *context->getRelationsInSCC(sccOrdering.at(i)).begin();

        std::string stratumIDInc = rel->getQualifiedName().toString() + "_inc";

        addRamSubroutine(stratumIDInc, std::move(stratumInc));
        appendStmt(incRes, mk<ram::Call>("stratum_" + stratumIDInc));
    }


    // Program translated!
    return mk<ram::Sequence>(std::move(incRes));
}

/**
* Will generate RAM for both full compilation and incremental compilation
*/
Own<ram::TranslationUnit> UnitTranslator::translateUnit(ast::TranslationUnit& tu) {
    glb = &tu.global();

    /* -- Set-up -- */
    auto ram_start = std::chrono::high_resolution_clock::now();
    context = mk<TranslatorContext>(tu);

    for (auto* clause: context->getProgram()->getClauses()) {
        if (context->isRecursiveClause(clause)) {
            clause->setRecursive(true);
        } else {
            clause->setRecursive(false);
        }
    }

    /* -- Translation -- */
    // Generate the RAM program code
    auto ramMain = generateProgram(tu);
    auto ramInc = glb->config().has("full-only") ? mk<ram::Sequence>() : generateProgramInc(tu);
    // Create the relevant RAM relations
    const auto& sccOrdering = tu.getAnalysis<ast::analysis::TopologicallySortedSCCGraphAnalysis>().order();

    for (const auto scc: sccOrdering) {
        for (auto* rel: context->getRelationsInSCC(scc)) {
            bool isRecursiveScc = context->isRecursiveSCC(scc);
            for (auto* clause: context->getProgram()->getClauses(*rel)) {
                clause->setInRecursiveStratum(isRecursiveScc);
            }
        }
    }

    auto ramRelations = createRamRelations(sccOrdering);

    // Combine all parts into the final RAM program
    ErrorReport& errReport = tu.getErrorReport();
    DebugReport& debugReport = tu.getDebugReport();
    auto ramProgram =
            mk<ram::Program>(std::move(ramRelations), std::move(ramMain), std::move(ramSubroutines), std::move(ramInc));
    for (const auto& probQuery : tu.getProgram().getProbQueries()) {
        const auto& atom = probQuery->getAtom();
        ramProgram -> addProbQuery(mk<ram::ProbQuery>(atom.getQualifiedName().toString(), toString(atom)));
    }

    for (const auto& evidence : tu.getProgram().getEvidences()) {
        const auto& atom = evidence->getAtom();
        ramProgram->addEvidence(
            mk<ram::Evidence>(
                atom.getQualifiedName().toString(),
                toString(atom),
                evidence->getEvidenceValue()
            )
        );
    }
    // Add the translated program to the debug report
    if (glb->config().has("debug-report")) {
        auto ram_end = std::chrono::high_resolution_clock::now();
        std::string runtimeStr =
                "(" + std::to_string(std::chrono::duration<double>(ram_end - ram_start).count()) + "s)";
        std::stringstream ramProgramStr;
        ramProgramStr << *ramProgram;
        debugReport.addSection("ram-program", "RAM Program " + runtimeStr, ramProgramStr.str());
    }

    // Wrap the program into a translation unit
    return mk<ram::TranslationUnit>(tu.global(), std::move(ramProgram), errReport, debugReport);
}

}  // namespace souffle::ast2ram::online
