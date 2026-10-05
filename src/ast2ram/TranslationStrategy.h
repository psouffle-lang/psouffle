/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file TranslationStrategy.h
 *
 * Abstract class representing an AST->RAM translation strategy.
 *
 ***********************************************************************/

#pragma once

#include "ast2ram/ClauseTranslator.h"
#include "souffle/utility/ContainerUtil.h"

namespace souffle::ast2ram {

class ClauseTranslator;
class ConstraintTranslator;
class UnitTranslator;
class TranslatorContext;
class ValueIndex;
class ValueTranslator;

class TranslationStrategy {
public:
    virtual ~TranslationStrategy() = default;

    /** Translation strategy name */
    virtual std::string getName() const = 0;

    /** AST translation unit -> RAM translation unit translator */
    virtual UnitTranslator* createUnitTranslator() const = 0;

    /** AST clause -> RAM statement translator */
    virtual ClauseTranslator* createClauseTranslator(
            const TranslatorContext& context, TranslationMode mode) const = 0;

    /** AST clause -> RAM statement translator */
    virtual ClauseTranslator* createClauseTranslatorInc(
                const TranslatorContext& context, TranslationMode mode) const { return nullptr; };

    /** AST literal -> RAM condition translator */
    virtual ConstraintTranslator* createConstraintTranslator(
            const TranslatorContext& context, const ValueIndex& index) const = 0;

    /** AST argument -> RAM expression translator */
    virtual ValueTranslator* createValueTranslator(
            const TranslatorContext& context, const ValueIndex& index) const = 0;

};

}  // namespace souffle::ast2ram
