/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Functor.cpp
 *
 * Analysis that provides information about functors
 *
 ***********************************************************************/

#include "ast/analysis/Functor.h"
#include "ast/Functor.h"
#include "ast/FunctorDeclaration.h"
#include "ast/IntrinsicFunctor.h"
#include "ast/TranslationUnit.h"
#include "ast/UserDefinedAggregator.h"
#include "ast/UserDefinedFunctor.h"
#include "ast/utility/Visitor.h"

namespace souffle::ast::analysis {

void FunctorAnalysis::run(const TranslationUnit& translationUnit) {
    const Program& program = translationUnit.getProgram();

    visit(program, [&](const FunctorDeclaration& functorDeclaration) {
        functorNameToDeclaration[functorDeclaration.getName()] = &functorDeclaration;
    });
}

bool FunctorAnalysis::isStatefulFunctor(const UserDefinedFunctor& functor) const {
    return getFunctorDeclaration(functor).isStateful();
}

bool FunctorAnalysis::isStatefulFunctor(const UserDefinedAggregator& aggregator) const {
    return getFunctorDeclaration(aggregator).isStateful();
}

QualifiedName const& FunctorAnalysis::getFunctorReturnType(const UserDefinedFunctor& functor) const {
    return getFunctorDeclaration(functor).getReturnType().getTypeName();
}

std::size_t FunctorAnalysis::getFunctorArity(const UserDefinedFunctor& functor) const {
    return getFunctorDeclaration(functor).getArity();
}

FunctorDeclaration const& FunctorAnalysis::getFunctorDeclaration(const UserDefinedFunctor& functor) const {
    return *functorNameToDeclaration.at(functor.getName());
}

FunctorDeclaration const& FunctorAnalysis::getFunctorDeclaration(
        const UserDefinedAggregator& aggregator) const {
    return *functorNameToDeclaration.at(aggregator.getBaseOperatorName());
}

bool FunctorAnalysis::isMultiResult(const Functor& functor) {
    if (isA<UserDefinedFunctor>(functor)) {
        return false;
    } else if (auto* intrinsic = as<IntrinsicFunctor>(functor)) {
        auto candidates = functorBuiltIn(intrinsic->getBaseFunctionOp());
        assert(!candidates.empty() && "at least one op should match");
        return candidates[0].get().multipleResults;
    }
    fatal("Missing functor type.");
}

}  // namespace souffle::ast::analysis
