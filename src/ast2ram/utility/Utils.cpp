/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Utils.cpp
 *
 * A collection of utilities used in translation
 *
 ***********************************************************************/

#include "ast2ram/utility/Utils.h"
#include "ast/Atom.h"
#include "ast/Clause.h"
#include "ast/QualifiedName.h"
#include "ast/Relation.h"
#include "ast/SubsumptiveClause.h"
#include "ast/UnnamedVariable.h"
#include "ast/Variable.h"
#include "ast/utility/Utils.h"
#include "ast2ram/ClauseTranslator.h"
#include "ast2ram/utility/Location.h"
#include "ram/Clear.h"
#include "ram/Condition.h"
#include "ram/Conjunction.h"
#include "ram/TupleElement.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/StringUtil.h"
#include <string>
#include <vector>

namespace souffle::ast2ram {

std::string getAtomName(const ast::Clause& clause, const ast::Atom* atom,
        const std::vector<ast::Atom*>& sccAtoms, std::size_t version, bool isRecursive,
        TranslationMode mode, bool isIncremental, bool isDelete) {
    if (isA<ast::SubsumptiveClause>(clause)) {
        assert(false && "subsumptive clause not supported");
    }
    if (atom->isRederive) {
        // allow rederive atoms even when the clause itself is non-recursive
        if (!isRecursive) {
            return getIncTupleOverDeleteRelationName(atom->getQualifiedName());
        }
        return getIncDervOverDeleteRelationName(atom->getQualifiedName());
    }
    if (clause.isRederive) {
        if (clause.getHead() == atom) {
            return getIncNewDervRederiveRelationName(atom->getQualifiedName());
        }
        if (!sccAtoms.empty() && sccAtoms.at(version) == atom) {
            return getIncDeltaTupleRederiveRelationName(atom->getQualifiedName());
        }
        return getConcreteRelationName(atom->getQualifiedName());
    }
    if (!isRecursive) {
        if (mode == Auxiliary && clause.getHead() == atom) {
            assert (false && "auxiliary mode not supported");
            return getNewRelationName(atom->getQualifiedName());
        }
        return getConcreteRelationName(atom->getQualifiedName());
    }
    if (!isIncremental) {
        if (clause.getHead() == atom) {
            return getNewRelationName(atom->getQualifiedName());
        }
        if (sccAtoms.at(version) == atom) {
            return getDeltaRelationName(atom->getQualifiedName());
        }
        return getConcreteRelationName(atom->getQualifiedName());
    } else {
        // TODO: should be, return DervDel (new) for head
        // return tuple delta delete for sccAtom (if is deletion)
        // return tuple delete for non sccAtoms
        if (isDelete) {
            if (clause.getHead() == atom) {
                return getNewDeletionRelationName(atom->getQualifiedName());
            }
            if (sccAtoms.at(version) == atom) {
                return getDeltaDeletionRelationName(atom->getQualifiedName());
            }
            return getIncDeltaDervDeleteRelationName(atom->getQualifiedName());
        } else {
            if (clause.getHead() == atom) {
                return getNewInsertionRelationName(atom->getQualifiedName());
            }
            if (sccAtoms.at(version) == atom) {
                return getDeltaInsertionRelationName(atom->getQualifiedName());
            }
            return getIncDeltaDervInsertRelationName(atom->getQualifiedName());
        }
    }
}

std::string getConcreteRelationName(const ast::QualifiedName& name, const std::string prefix) {
    return prefix + getRelationName(name);
}

std::string getOldRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@old_");
}

std::string getPostDeleteRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@post_delete_");
}

std::string getDeltaRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@delta_");
}

std::string getNewRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@new_");
}

/**
 * For inc + recursion; delta_delta
 */
std::string getDeltaDeletionRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@delta_tuple_delete_");
}

std::string getDeltaInsertionRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@delta_tuple_insert_");
}

std::string getNewDeletionRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@new_derv_delete_");
}

std::string getNewInsertionRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@new_derv_insert_");
}

std::string getLubRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@lub_");
}

std::string getRejectRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@reject_");
}

std::string getDeleteRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@delete_");
}

// std::string getIncDeltaRelationName(const ast::QualifiedName& name) {
//     return getConcreteRelationName(name, "@inc_delta_");
// }
// for inc + non recursion; in recursion, we need a concept of "delta_delta"
std::string getIncDeltaDervInsertRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "$inc_delta_derv_insert_");
}

std::string getIncDeltaDervDeleteRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "$inc_delta_derv_delete_");
}
// for inc, final delta computed
std::string getIncDeltaTupleInsertRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "$inc_delta_tuple_insert_");
}

std::string getIncDeltaTupleDeleteRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "$inc_delta_tuple_delete_");
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
std::string getIncTupleOverDeleteRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@inc_tuple_overdelete_");
}

std::string getIncDervOverDeleteRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@inc_derv_overdelete_");
}

std::string getIncNewDervRederiveRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@inc_new_derv_rederive_");
}

std::string getIncDeltaTupleRederiveRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@inc_delta_tuple_rederive_");
}


std::string getTmpRelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@tmp_");
}

std::string getTmp2RelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@tmp2_");
}

std::string getTmp3RelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@tmp3_");
}

std::string getTmp4RelationName(const ast::QualifiedName& name) {
    return getConcreteRelationName(name, "@tmp4_");
}
const std::string& getRelationName(const ast::QualifiedName& name) {
    return name.toString();
}

std::string getBaseRelationName(const ast::QualifiedName& name) {
    auto str =
    stripPrefix("$inc_delta_tuple_delete_",
    stripPrefix("$inc_delta_tuple_insert_",
    stripPrefix("$inc_delta_derv_delete_",
    stripPrefix("$inc_delta_derv_insert_",
    stripPrefix("@tmp4_",
    stripPrefix("@tmp3_",
    stripPrefix("@tmp2_",
    stripPrefix("@tmp_",
        stripPrefix("@post_delete_",
        stripPrefix("@old_",
            stripPrefix("@delta_tuple_delete_",
                stripPrefix("@delta_tuple_insert_",
                    stripPrefix("@new_derv_delete_",
                        stripPrefix("@new_derv_insert_",
                            stripPrefix("@new_",
                                stripPrefix("@delta_",
                                    stripPrefix("@info_", name.toString())))))))))))))))));

    str = stripPrefix( "@inc_tuple_overdelete_", str);
    str = stripPrefix( "@inc_derv_overdelete_", str);
    str = stripPrefix( "@inc_new_derv_rederive_", str);
    str = stripPrefix( "@inc_delta_tuple_rederive_", str);
    return str;
}

void appendStmt(VecOwn<ram::Statement>& stmtList, Own<ram::Statement> stmt) {
    if (stmt) {
        stmtList.push_back(std::move(stmt));
    }
}

void nameUnnamedVariables(ast::Clause* clause) {
    // the node mapper conducting the actual renaming
    struct Instantiator : public ast::NodeMapper {
        mutable int counter = 0;

        Instantiator() = default;

        Own<ast::Node> operator()(Own<ast::Node> node) const override {
            // apply recursive
            node->apply(*this);

            // replace unknown variables
            if (isA<ast::UnnamedVariable>(node)) {
                auto name = " _unnamed_var" + toString(++counter);
                return mk<ast::Variable>(name);
            }

            // otherwise nothing
            return node;
        }
    };

    // name all variables in the atoms
    Instantiator init;
    for (auto& atom : ast::getBodyLiterals<ast::Atom>(*clause)) {
        atom->apply(init);
    }
}

Own<ram::TupleElement> makeRamTupleElement(const Location& loc) {
    return mk<ram::TupleElement>(loc.identifier, loc.element);
}

Own<ram::Condition> addConjunctiveTerm(Own<ram::Condition> curCondition, Own<ram::Condition> newTerm) {
    return curCondition ? mk<ram::Conjunction>(std::move(curCondition), std::move(newTerm))
                        : std::move(newTerm);
}

}  // namespace souffle::ast2ram
