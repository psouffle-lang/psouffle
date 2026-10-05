/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file UnitTranslator.h
 *
 * Abstract class providing an interface for translating an
 * ast::TranslationUnit into an equivalent ram::TranslationUnit.
 *
 ***********************************************************************/

#pragma once

#include "souffle/utility/ContainerUtil.h"
#include "ram/Program.h"

namespace souffle::ast {
class TranslationUnit;
}

namespace souffle::ram {
class TranslationUnit;
}

namespace souffle::ast2ram {

class TranslatorContext;

class UnitTranslator {
public:
    UnitTranslator() = default;
    virtual ~UnitTranslator() = default;

    virtual Own<ram::TranslationUnit> translateUnit(ast::TranslationUnit& tu) = 0;

// protected:
    Own<TranslatorContext> context;
};

}  // namespace souffle::ast2ram
