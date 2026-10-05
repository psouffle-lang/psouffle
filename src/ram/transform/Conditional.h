/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Conditional.h
 *
 * Defines the interface for RAM transformation passes.
 *
 ***********************************************************************/

#pragma once

#include "ram/Program.h"
#include "ram/TranslationUnit.h"
#include "ram/transform/Meta.h"
#include "ram/transform/Transformer.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace souffle::ram::transform {

/**
 * @Class ConditionalTransformer
 * @Brief Composite conditional transformer
 *
 * A transformation is invoked if a condition holds.
 */
class ConditionalTransformer : public MetaTransformer {
public:
    ConditionalTransformer(std::function<bool()> fn, Own<Transformer> tb)
            : func(std::move(fn)), body(std::move(tb)) {}
    std::string getName() const override {
        return "ConditionalTransformer";
    }
    bool transform(TranslationUnit& tU) override {
        if (func()) {
            return body->apply(tU);
        } else {
            return false;
        }
    }

protected:
    std::function<bool()> func;
    Own<Transformer> body;
};

}  // namespace souffle::ram::transform
