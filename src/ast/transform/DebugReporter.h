/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file DebugReporter.h
 *
 * Defines an adaptor transformer to capture debug output from other transformers
 *
 ***********************************************************************/
#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Meta.h"
#include "ast/transform/Null.h"
#include "ast/transform/Transformer.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/MiscUtil.h"
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace souffle::ast::transform {

/**
 * Transformation pass which wraps another transformation pass and generates
 * a debug report section for the stage after applying the wrapped transformer,
 * and adds it to the translation unit's debug report.
 */
class DebugReporter : public MetaTransformer {
public:
    DebugReporter(Own<Transformer> wrappedTransformer) : wrappedTransformer(std::move(wrappedTransformer)) {}

    std::vector<Transformer*> getSubtransformers() const override {
        return {wrappedTransformer.get()};
    }

    void setDebugReport() override {}

    void setVerbosity(bool verbose) override {
        this->verbose = verbose;
        if (auto* mt = as<MetaTransformer>(wrappedTransformer)) {
            mt->setVerbosity(verbose);
        }
    }

    void disableTransformers(const std::set<std::string>& transforms) override {
        if (auto* mt = as<MetaTransformer>(wrappedTransformer)) {
            mt->disableTransformers(transforms);
        } else if (transforms.find(wrappedTransformer->getName()) != transforms.end()) {
            wrappedTransformer = mk<NullTransformer>();
        }
    }

    std::string getName() const override {
        return "DebugReporter";
    }

    DebugReporter* cloning() const override {
        return new DebugReporter(clone(wrappedTransformer));
    }

private:
    Own<Transformer> wrappedTransformer;

    bool transform(TranslationUnit& translationUnit) override;

    void generateDebugReport(TranslationUnit& tu, const std::string& preTransformDatalog);
};

}  // namespace souffle::ast::transform
