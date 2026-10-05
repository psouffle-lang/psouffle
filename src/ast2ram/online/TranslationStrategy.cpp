/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file TranslationStrategy.cpp
 *
 ***********************************************************************/

#include "ast2ram/online/TranslationStrategy.h"
#include "ast2ram/online/ClauseTranslator.h"
#include "ast2ram/online/IncClauseTranslator.h"
#include "ast2ram/online/ConstraintTranslator.h"
#include "ast2ram/online/IncConstraintTranslator.h"
#include "ast2ram/online/UnitTranslator.h"
#include "ast2ram/online/ValueTranslator.h"
#include "ast2ram/online/IncValueTranslator.h"
#include "ast2ram/utility/TranslatorContext.h"
#include "ram/Condition.h"
#include "ram/Expression.h"
#include "ram/Program.h"

namespace souffle::ast2ram::online {

ast2ram::UnitTranslator* TranslationStrategy::createUnitTranslator() const {
    return new UnitTranslator();
}

ast2ram::ClauseTranslator* TranslationStrategy::createClauseTranslator(
        const TranslatorContext& context, TranslationMode mode) const {
    return new ClauseTranslator(context, mode);
}

ast2ram::ClauseTranslator* TranslationStrategy::createClauseTranslatorInc(
        const TranslatorContext& context, TranslationMode mode) const {
    return new IncClauseTranslator(context, mode);
}

ast2ram::ConstraintTranslator* TranslationStrategy::createConstraintTranslator(
        const TranslatorContext& context, const ValueIndex& index) const {
    return new ConstraintTranslator(context, index);
}

ast2ram::ConstraintTranslator* TranslationStrategy::createConstraintTranslatorInc(
        const TranslatorContext& context, const ValueIndex& index) const {
    return new IncConstraintTranslator(context, index);
}

ast2ram::ValueTranslator* TranslationStrategy::createValueTranslator(
        const TranslatorContext& context, const ValueIndex& index) const {
    return new ValueTranslator(context, index);
}

ast2ram::ValueTranslator* TranslationStrategy::createValueTranslatorInc(
        const TranslatorContext& context, const ValueIndex& index) const {
    return new IncValueTranslator(context, index);
}

}  // namespace souffle::ast2ram::online
