/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file UnitTranslator.h
 *
 ***********************************************************************/

#pragma once

#include "ast/Relation.h"
#include "ast2ram/UnitTranslator.h"
#include "ram/Expression.h"
#include "ast/ProbQuery.h"
#include "souffle/utility/ContainerUtil.h"
#include <map>
#include "ast/Evidence.h"
#include "ram/Program.h"
#include <set>
#include <string>
#include <vector>

namespace souffle {
class Global;
}

namespace souffle::ast {
class Clause;
class Relation;
class TranslationUnit;
class Atom;
}  // namespace souffle::ast

namespace souffle::ram {
class Relation;
class Sequence;
class Statement;
class TranslationUnit;
}  // namespace souffle::ram

namespace souffle::ast2ram::online {

class UnitTranslator : public ast2ram::UnitTranslator {
public:
    UnitTranslator();
    ~UnitTranslator();

    souffle::ram::Program* ramProgram = nullptr;
    Own<ram::TranslationUnit> translateUnit(ast::TranslationUnit& tu) override;
inline  void setRamProgram(souffle::ram::Program* program) {
        ramProgram = program;
    };
protected:
    void addRamSubroutine(std::string subroutineID, Own<ram::Statement> subroutine);
    virtual Own<ram::Relation> createRamRelation(
            const ast::Relation* baseRelation, std::string ramRelationName) const;
    virtual VecOwn<ram::Relation> createRamRelations(const std::vector<std::size_t>& sccOrdering) const;
    Own<ram::Statement> translateRecursiveClauses(
            const ast::RelationSet& scc, const ast::Relation* rel) const;
    Own<ram::Statement> translateSubsumptiveRecursiveClauses(
            const ast::RelationSet& scc, const ast::Relation* rel) const;
    VecOwn<ram::Statement> generateClauseVersions(
            const ast::Clause* clause, const ast::RelationSet& scc) const;
    std::vector<ast::Atom*> getSccAtoms(const ast::Clause* clause, const ast::RelationSet& scc) const;

    virtual void addAuxiliaryArity(
            const ast::Relation* relation, std::map<std::string, std::string>& directives) const;

    /** -- Generation methods -- */
    VecOwn<ram::Statement> generateClauseVersionsInc(
        const ast::Clause* clause, const ast::RelationSet& scc, bool isDelete = false) const;
    VecOwn<ram::Statement> generateClauseVersionsIncRederive(
            const ast::Clause* clause, const ast::RelationSet& scc, std::size_t sccNumber) const;
    VecOwn<ram::Statement> generateClauseVersionsPrefill(
        const ast::Clause* clause, const ast::RelationSet& scc, bool isDelete) const;
    Own<ram::Statement> generateStratumTableUpdatesInc(const ast::RelationSet& scc, bool isDelete = false) const;
    Own<ram::Statement> generateStratumTableUpdatesIncRederive(const ast::RelationSet& scc) const;
    Own<ram::Statement> generateStratumLoopBodyInc(const ast::RelationSet& scc, bool isDelete = false) const;
    Own<ram::Statement> generateStratumLoopBodyIncRederive(
            const ast::RelationSet& scc, std::size_t sccNumber) const;

    Own<ram::Statement> generateStratumExitSequenceInc(const ast::RelationSet& scc, bool isDelete = false) const;
    Own<ram::Statement> generateStratumExitSequenceIncRederive(const ast::RelationSet& scc) const;
    Own<ram::Statement> translateRecursiveClausesInc(
        const ast::RelationSet& scc, const ast::Relation* rel, bool isDelete = false, bool isPrefill = false) const;
    Own<ram::Statement> translateRecursiveClausesIncRederive(
            const ast::RelationSet& scc, const ast::Relation* rel, std::size_t sccNumber) const;
    Own<ram::Statement> generateStratumNonSccPreFill(const ast::RelationSet& scc, bool isDelete = false) const;
    Own<ram::Statement> generateNonRecursiveRelationIns(const ast::Relation& rel) const;
    Own<ram::Statement> generateNonRecursiveRelationDel(const ast::Relation& rel) const;
    /** High-level relation translation */
    virtual Own<ram::Sequence> generateProgram(const ast::TranslationUnit& translationUnit);
    virtual Own<ram::Sequence> generateProgramInc(const ast::TranslationUnit& translationUnit);

    Own<ram::Statement> generateNonRecursiveRelation(const ast::Relation& rel) const;
    Own<ram::Statement> generateRecursiveStratum(const ast::RelationSet& scc, std::size_t sccNum) const;
    Own<ram::Statement> generateRecursiveStratumInc(
                const ast::RelationSet& scc, std::size_t sccNumber) const;
    Own<ram::Statement> translateProbQuery(const ast::ProbQuery& probQuery);
    /** IO translation */
    Own<ram::Statement> generateStoreRelation(const ast::Relation* relation) const;
    Own<ram::Statement> generateLoadRelation(const ast::Relation* relation) const;
    Own<ram::Statement> generateLoadRelationInc(const ast::Relation* relation) const;

    /** Low-level stratum translation */
    Own<ram::Statement> translateEvidence(const ast::Evidence& evidence);
    Own<ram::Statement> generateStratum(std::size_t scc) const;
    Own<ram::Statement> generateStratumInc(std::size_t scc) const;
    Own<ram::Statement> generateIncTableUpdate(const std::vector<std::size_t>& sccOrderings) const;
    Own<ram::Statement> generateStratumPreamble(const ast::RelationSet& scc) const;
    Own<ram::Statement> generateStratumPreambleInc(const ast::RelationSet& scc, bool isDelete = false) const;
    Own<ram::Statement> generateStratumRederive(
            const ast::RelationSet& scc, std::size_t sccNumber) const;

    Own<ram::Statement> generateNonRecursiveDelete(const ast::Relation& rel) const;
    Own<ram::Statement> generateStratumPostamble(const ast::RelationSet& scc) const;
    Own<ram::Statement> generateStratumPostambleInc(const ast::RelationSet& scc, bool isDelete) const;
    Own<ram::Statement> generateStratumPostambleIncRederive(const ast::RelationSet& scc) const;

    Own<ram::Statement> generateStratumLoopBody(const ast::RelationSet& scc) const;
    Own<ram::Statement> generateStratumTableUpdates(const ast::RelationSet& scc) const;
    Own<ram::Statement> generateStratumExitSequence(const ast::RelationSet& scc) const;
    Own<ram::Statement> generateStratumLubSequence(const ast::Relation& rel, bool inRecursiveLoop) const;

    Own<ram::Statement> generateNonRecursiveRelationInc(const ast::Relation& rel) const;
    /** Other helper generations */
    virtual Own<ram::Statement> generateClearExpiredRelations(const ast::RelationSet& expiredRelations) const;
    Own<ram::Statement> generateClearRelation(const ast::Relation* relation) const;
    virtual Own<ram::Statement> generateMergeRelations(
            const ast::Relation* rel, const std::string& destRelation, const std::string& srcRelation) const;
    virtual Own<ram::Statement> generateMergeRelationsWithFilter(const ast::Relation* rel,
            const std::string& destRelation, const std::string& srcRelation,
            const std::string& filterRelation) const;
    virtual Own<ram::Statement> generateMergeRelationsWithNegativeFilter(const ast::Relation* rel,
                const std::string& destRelation, const std::string& srcRelation,
                const std::string& filterRelation) const;
    virtual Own<ram::Statement> generateEraseTuples(
            const ast::Relation* rel, const std::string& destRelation, const std::string& srcRelation) const;
    virtual Own<ram::Statement> generateDebugRelation(const ast::Relation* rel,
            const std::string& destRelation, const std::string& srcRelation,
            Own<ram::Expression> iteration) const;

private:
    std::map<std::string, Own<ram::Statement>> ramSubroutines;
    Own<ram::Program> ramProgramHolder;
    Global* glb;
};

}  // namespace souffle::ast2ram::online
