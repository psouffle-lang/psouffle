/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file PolymorphicObjects.cpp
 *
 ***********************************************************************/

#include "ast/analysis/typesystem/PolymorphicObjects.h"
#include "ast/Aggregator.h"
#include "ast/BinaryConstraint.h"
#include "ast/IntrinsicFunctor.h"
#include "ast/NumericConstant.h"
#include "ast/TranslationUnit.h"
#include "ast/analysis/typesystem/Type.h"
#include "ast/utility/Visitor.h"
#include "souffle/utility/ContainerUtil.h"

namespace souffle::ast::analysis {

void PolymorphicObjectsAnalysis::run(const TranslationUnit& translationUnit) {
    typeAnalysis = &translationUnit.getAnalysis<analysis::TypeAnalysis>();
}

void PolymorphicObjectsAnalysis::print(std::ostream& /* os */) const {}

FunctorOp PolymorphicObjectsAnalysis::getOverloadedFunctionOp(const IntrinsicFunctor& inf) const {
    return typeAnalysis->getPolymorphicOperator(inf);
}

NumericConstant::Type PolymorphicObjectsAnalysis::getInferredType(const NumericConstant& nc) const {
    return typeAnalysis->getPolymorphicNumericConstantType(nc);
}

bool PolymorphicObjectsAnalysis::hasInvalidType(const NumericConstant& nc) const {
    return !typeAnalysis->hasValidTypeInfo(nc);
}

BinaryConstraintOp PolymorphicObjectsAnalysis::getOverloadedOperator(const BinaryConstraint& bc) const {
    return typeAnalysis->getPolymorphicOperator(bc);
}

AggregateOp PolymorphicObjectsAnalysis::getOverloadedOperator(const IntrinsicAggregator& agg) const {
    return typeAnalysis->getPolymorphicOperator(agg);
}

}  // namespace souffle::ast::analysis
