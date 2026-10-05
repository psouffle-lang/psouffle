/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ConstraintTranslator.cpp
 *
 ***********************************************************************/

#include "ast2ram/online/ConstraintTranslator.h"
#include "ast/BinaryConstraint.h"
#include "ast/TranslationUnit.h"
#include "ast2ram/ValueTranslator.h"
#include "ast2ram/utility/TranslatorContext.h"
#include "ast2ram/utility/Utils.h"
#include "ast2ram/utility/ValueIndex.h"
#include "ram/Constraint.h"
#include "ram/EmptinessCheck.h"
#include "ram/ExistenceCheck.h"
#include "ram/Negation.h"

namespace souffle::ast2ram::online {

Own<ram::Condition> ConstraintTranslator::translateConstraint(const ast::Literal* lit) {
    assert(lit != nullptr && "literal should be defined");
    return ConstraintTranslator(context, index)(*lit);
}

Own<ram::Condition> ConstraintTranslator::visit_(type_identity<ast::Atom>, const ast::Atom&) {
    return nullptr;  // covered already within the scan/lookup generation step
}

Own<ram::Condition> ConstraintTranslator::visit_(
        type_identity<ast::BinaryConstraint>, const ast::BinaryConstraint& binRel) {
    auto valLHS = context.translateValue(index, binRel.getLHS());
    auto valRHS = context.translateValue(index, binRel.getRHS());
    return mk<ram::Constraint>(
            context.getOverloadedBinaryConstraintOperator(binRel), std::move(valLHS), std::move(valRHS));
}

Own<ram::Condition> ConstraintTranslator::visit_(type_identity<ast::Negation>, const ast::Negation& neg) {
    const auto* atom = neg.getAtom();
    std::size_t arity = atom->getArity();

    if (arity == 0) {
        // for a nullary, negation is a simple emptiness check
        return nullptr;
        // return mk<ram::EmptinessCheck>(getConcreteRelationName(atom->getQualifiedName()));
    }

    // else, we construct the atom and create a negation
    VecOwn<ram::Expression> values;
    for (const auto* arg : atom->getArguments()) {
        values.push_back(context.translateValue(index, arg));
    }
    // return mk<ram::Negation>(
    //         mk<ram::ExistenceCheck>(getConcreteRelationName(atom->getQualifiedName()), std::move(values)));
    return nullptr;
}
}  // namespace souffle::ast2ram::online
