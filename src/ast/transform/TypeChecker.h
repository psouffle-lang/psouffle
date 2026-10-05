/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file TypeChecker.h
 *
 * Defines the type checker pass (part of semantic checker)
 * The type checker checks type declarations
 * and if the declarations are valid it checks typing in clauses
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

class TypeChecker : public Transformer {
public:
    ~TypeChecker() override = default;

    std::string getName() const override {
        return "TypeChecker";
    }

    // `apply` but doesn't immediately bail if any errors are found.
    void verify(TranslationUnit& translationUnit);

private:
    TypeChecker* cloning() const override {
        return new TypeChecker();
    }

    bool transform(TranslationUnit& translationUnit) override {
        verify(translationUnit);
        return false;
    }
};
}  // namespace souffle::ast::transform
