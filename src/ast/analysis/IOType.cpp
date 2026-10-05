/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2019, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file IOTypeAnalysis.cpp
 *
 * Implements methods to identify a relation as input, output, or printsize.
 *
 ***********************************************************************/

#include "ast/analysis/IOType.h"
#include "ast/Directive.h"
#include "ast/Program.h"
#include "ast/QualifiedName.h"
#include "ast/Relation.h"
#include "ast/TranslationUnit.h"
#include "ast/utility/Utils.h"
#include "ast/utility/Visitor.h"
#include "souffle/utility/StreamUtil.h"
#include <cassert>
#include <ostream>
#include <vector>

namespace souffle::ast::analysis {

void IOTypeAnalysis::run(const TranslationUnit& translationUnit) {
    const Program& program = translationUnit.getProgram();
    visit(program, [&](const Directive& directive) {
        auto* relation = program.getRelation(directive);
        if (relation == nullptr) {
            return;
        }
        switch (directive.getType()) {
            case ast::DirectiveType::input: inputRelations.insert(relation); break;
            case ast::DirectiveType::output: outputRelations.insert(relation); break;
            case ast::DirectiveType::printsize:
                printSizeRelations.insert(relation);
                outputRelations.insert(relation);
                break;
            case ast::DirectiveType::limitsize:
                limitSizeRelations.insert(relation);
                assert(directive.hasParameter("n") && "limitsize has no n directive");
                limitSize[relation] = stoi(directive.getParameter("n"));
                break;
        }
    });
    for (auto& probquery : program.getProbQueries()) {
        auto* relation = program.getRelation(probquery->getAtom());
        outputRelations.insert(relation);
    }
}

void IOTypeAnalysis::print(std::ostream& os) const {
    auto show = [](std::ostream& os, const Relation* r) { os << r->getQualifiedName(); };
    os << "input relations: {" << join(inputRelations, ", ", show) << "}\n";
    os << "output relations: {" << join(outputRelations, ", ", show) << "}\n";
    os << "printsize relations: {" << join(printSizeRelations, ", ", show) << "}\n";
    os << "limitsize relations: {" << join(limitSizeRelations, ", ", show) << "}\n";
}

}  // namespace souffle::ast::analysis
