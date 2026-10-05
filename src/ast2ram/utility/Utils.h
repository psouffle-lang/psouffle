/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Utils.h
 *
 * A collection of utilities used in translation
 *
 ***********************************************************************/

#pragma once

#include "ast2ram/ClauseTranslator.h"
#include "souffle/utility/ContainerUtil.h"
#include <string>

namespace souffle::ast {
class Atom;
class Clause;
class QualifiedName;
class Relation;
}  // namespace souffle::ast

namespace souffle::ram {
class Clear;
class Condition;
class Statement;
class TupleElement;
}  // namespace souffle::ram

namespace souffle::ast2ram {

struct Location;

/** Get the corresponding atom name given the clause and other state */
std::string getAtomName(const ast::Clause& clause, const ast::Atom* atom,
        const std::vector<ast::Atom*>& sccAtoms, std::size_t version, bool isRecursive, TranslationMode mode, bool isIncremental = false, bool isDelete = false);

/** Get the corresponding concretised RAM relation name for the relation */
std::string getConcreteRelationName(const ast::QualifiedName& name, std::string prefix = "");

/** converts the given relation identifier into a relation name */
const std::string& getRelationName(const ast::QualifiedName& name);

/** Get the corresponding RAM delta relation name for the relation */
std::string getOldRelationName(const ast::QualifiedName& name);

/** Get the corresponding RAM post-delete relation name for the relation */
std::string getPostDeleteRelationName(const ast::QualifiedName& name);

/** Get the corresponding RAM delta relation name for the relation */
std::string getDeltaRelationName(const ast::QualifiedName& name);

/** Get the corresponding RAM 'new' relation name for the relation */
std::string getNewRelationName(const ast::QualifiedName& name);

/** Get the corresponding RAM 'lub' relation name for the relation */
std::string getLubRelationName(const ast::QualifiedName& name);

/** Get the corresponding RAM 'reject' relation name for the relation */
std::string getRejectRelationName(const ast::QualifiedName& name);

/** Get the corresponding RAM 'delete' relation name for the relation */
std::string getDeleteRelationName(const ast::QualifiedName& name);

/** INC */
std::string getIncDeltaDervInsertRelationName(const ast::QualifiedName& name);

std::string getIncDeltaDervDeleteRelationName(const ast::QualifiedName& name);

std::string getIncDeltaTupleInsertRelationName(const ast::QualifiedName& name);

std::string getIncDeltaTupleDeleteRelationName(const ast::QualifiedName& name);

std::string getTmpRelationName(const ast::QualifiedName& name);
std::string getTmp2RelationName(const ast::QualifiedName& name);
std::string getTmp3RelationName(const ast::QualifiedName& name);
std::string getTmp4RelationName(const ast::QualifiedName& name);

/**
 * For inc + recursion
 */
std::string getDeltaDeletionRelationName(const ast::QualifiedName& name);
std::string getDeltaInsertionRelationName(const ast::QualifiedName& name);
std::string getNewDeletionRelationName(const ast::QualifiedName& name);
std::string getNewInsertionRelationName(const ast::QualifiedName& name);

// rederivation
std::string getIncTupleOverDeleteRelationName(const ast::QualifiedName& name);
std::string getIncDervOverDeleteRelationName(const ast::QualifiedName& name);
std::string getIncNewDervRederiveRelationName(const ast::QualifiedName& name);
std::string getIncDeltaTupleRederiveRelationName(const ast::QualifiedName& name);

/** Get base relation name, strip off any possible prefix */
std::string getBaseRelationName(const ast::QualifiedName& name);

/** Append statement to a list of statements */
void appendStmt(VecOwn<ram::Statement>& stmtList, Own<ram::Statement> stmt);

/** Assign names to unnamed variables */
void nameUnnamedVariables(ast::Clause* clause);

/** Create a RAM element access node */
Own<ram::TupleElement> makeRamTupleElement(const Location& loc);

/** Add a term to a conjunction */
Own<ram::Condition> addConjunctiveTerm(Own<ram::Condition> curCondition, Own<ram::Condition> newTerm);

}  // namespace souffle::ast2ram
