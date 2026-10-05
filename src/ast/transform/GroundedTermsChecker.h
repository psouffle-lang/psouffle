/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file GroundedTermsChecker.h
 *
 * Defines the grounded terms checker pass.
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

class GroundedTermsChecker : public Transformer {
public:
    std::string getName() const override {
        return "GroundedTermsChecker";
    }

    // `apply` but doesn't immediately bail if any errors are found.
    void verify(TranslationUnit& translationUnit);

private:
    GroundedTermsChecker* cloning() const override {
        return new GroundedTermsChecker();
    }

    bool transform(TranslationUnit& translationUnit) override {
        verify(translationUnit);
        return false;
    }
};

}  // namespace souffle::ast::transform
