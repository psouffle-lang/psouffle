/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file While.h
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/DebugReporter.h"
#include "ast/transform/Meta.h"
#include "ast/transform/Null.h"
#include "ast/transform/Transformer.h"
#include "souffle/utility/MiscUtil.h"
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace souffle::ast::transform {

/**
 * Transformer that repeatedly executes a sub-transformer while a condition is met
 */
class WhileTransformer : public MetaTransformer {
public:
    WhileTransformer(std::function<bool()> cond, Own<Transformer> transformer)
            : condition(std::move(cond)), transformer(std::move(transformer)) {}

    WhileTransformer(bool cond, Own<Transformer> transformer)
            : condition([=]() { return cond; }), transformer(std::move(transformer)) {}

    std::vector<Transformer*> getSubtransformers() const override {
        return {transformer.get()};
    }
    void setDebugReport() override {
        if (auto* mt = as<MetaTransformer>(transformer)) {
            mt->setDebugReport();
        } else {
            transformer = mk<DebugReporter>(std::move(transformer));
        }
    }

    void setVerbosity(bool verbose) override {
        this->verbose = verbose;
        if (auto* mt = as<MetaTransformer>(transformer)) {
            mt->setVerbosity(verbose);
        }
    }

    void disableTransformers(const std::set<std::string>& transforms) override {
        if (auto* mt = as<MetaTransformer>(transformer)) {
            mt->disableTransformers(transforms);
        } else if (transforms.find(transformer->getName()) != transforms.end()) {
            transformer = mk<NullTransformer>();
        }
    }

    std::string getName() const override {
        return "WhileTransformer";
    }

private:
    WhileTransformer* cloning() const override {
        return new WhileTransformer(condition, clone(transformer));
    }

    bool transform(TranslationUnit& translationUnit) override {
        bool changed = false;
        while (condition()) {
            changed |= applySubtransformer(translationUnit, transformer.get());
        }
        return changed;
    }

private:
    std::function<bool()> condition;
    Own<Transformer> transformer;
};

}  // namespace souffle::ast::transform
