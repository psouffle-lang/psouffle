/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file GroundedTermsChecker.cpp
 *
 * Implementation of the grounded terms checker pass.
 *
 ***********************************************************************/

#include "ast/transform/GroundedTermsChecker.h"
#include "ast/BranchInit.h"
#include "ast/Clause.h"
#include "ast/Program.h"
#include "ast/RecordInit.h"
#include "ast/TranslationUnit.h"
#include "ast/Variable.h"
#include "ast/analysis/Ground.h"
#include "ast/utility/Utils.h"
#include "ast/utility/Visitor.h"
#include "reports/ErrorReport.h"
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace souffle::ast::transform {

void GroundedTermsChecker::verify(TranslationUnit& translationUnit) {
    auto&& program = translationUnit.getProgram();
    auto&& report = translationUnit.getErrorReport();

    // -- check grounded variables and records --
    visit(program, [&](const Clause& clause) {
        if (isFact(clause)) return;  // only interested in rules

        auto isGrounded = analysis::getGroundedTerms(translationUnit, clause);

        std::set<std::string> reportedVars;
        // all terms in head need to be grounded
        visit(clause, [&](const Variable& cur) {
            if (!isGrounded[&cur] && reportedVars.insert(cur.getName()).second) {
                report.addError("Ungrounded variable " + cur.getName(), cur.getSrcLoc());
            }
        });

        // all records need to be grounded
        visit(clause, [&](const RecordInit& record) {
            if (!isGrounded[&record]) {
                report.addError("Ungrounded record", record.getSrcLoc());
            }
        });

        // All sums need to be grounded
        visit(clause, [&](const BranchInit& adt) {
            if (!isGrounded[&adt]) {
                report.addError("Ungrounded ADT branch", adt.getSrcLoc());
            }
        });
    });
}

}  // namespace souffle::ast::transform
