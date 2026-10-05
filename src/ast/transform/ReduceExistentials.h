/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ReduceExistentials.h
 *
 * Transformation pass to reduce unnecessary computation for
 * relations that only appear in the form A(_,...,_).
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

/**
 * Transformation pass to reduce unnecessary computation for
 * relations that only appear in the form A(_,...,_).
 */
class ReduceExistentialsTransformer : public Transformer {
public:
    std::string getName() const override {
        return "ReduceExistentialsTransformer";
    }

private:
    ReduceExistentialsTransformer* cloning() const override {
        return new ReduceExistentialsTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
