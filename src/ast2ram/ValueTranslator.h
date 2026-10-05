/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ValueTranslator.h
 *
 * Abstract class providing an interface for translating an
 * ast::Argument into an equivalent ram::Expression.
 *
 ***********************************************************************/

#pragma once

#include "ast/utility/Visitor.h"
#include "souffle/utility/ContainerUtil.h"

namespace souffle::ast {
class Argument;
}

namespace souffle::ram {
class Expression;
}

namespace souffle::ast2ram {

class TranslatorContext;
class ValueIndex;

class ValueTranslator : public ast::Visitor<Own<ram::Expression>> {
public:
    ValueTranslator(const TranslatorContext& context, const ValueIndex& index)
            : context(context), index(index) {}
    virtual ~ValueTranslator() = default;

    virtual Own<ram::Expression> translateValue(const ast::Argument* arg) = 0;

protected:
    const TranslatorContext& context;
    const ValueIndex& index;
};

}  // namespace souffle::ast2ram
