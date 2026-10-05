/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file RemoveRedundantRelations.h
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

/**
 * Transformation pass to remove relations which are redundant (do not contribute to output).
 */
class RemoveRedundantRelationsTransformer : public Transformer {
public:
    std::string getName() const override {
        return "RemoveRedundantRelationsTransformer";
    }

private:
    RemoveRedundantRelationsTransformer* cloning() const override {
        return new RemoveRedundantRelationsTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
