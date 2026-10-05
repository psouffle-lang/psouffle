/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ExpandEqrels.h
 *
 * Transformation pass to explicitly define eqrel relations.
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

class ExpandEqrelsTransformer : public Transformer {
public:
    std::string getName() const override {
        return "ExpandEqrelsTransformer";
    }

private:
    ExpandEqrelsTransformer* cloning() const override {
        return new ExpandEqrelsTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
