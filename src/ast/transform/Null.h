/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Null.h
 *
 * Defines the interface for Null transformation passes.
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Meta.h"
#include "ast/transform/Transformer.h"
#include <set>
#include <string>
#include <vector>

namespace souffle::ast::transform {

/**
 * Transformer that does absolutely nothing
 */
class NullTransformer : public MetaTransformer {
private:
    bool transform(TranslationUnit& /* translationUnit */) override {
        return false;
    }

public:
    std::vector<Transformer*> getSubtransformers() const override {
        return {};
    }

    void setDebugReport() override {}

    void setVerbosity(bool /* verbose */) override {}

    void disableTransformers(const std::set<std::string>& /* transforms */) override {}

    std::string getName() const override {
        return "NullTransformer";
    }

    NullTransformer* cloning() const override {
        return new NullTransformer();
    }
};

}  // namespace souffle::ast::transform
