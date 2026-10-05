/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Transformer.h
 *
 * Defines the interface for AST transformation passes.
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "souffle/utility/Types.h"
#include <string>

namespace souffle::ast::transform {

class Transformer {
private:
    virtual bool transform(TranslationUnit& translationUnit) = 0;

public:
    virtual ~Transformer() = default;

    bool apply(TranslationUnit& translationUnit);

    virtual std::string getName() const = 0;

    /**
     * Transformers can be disabled by command line
     * with --disable-transformer. Default behaviour
     * is that all transformers can be disabled.
     */
    virtual bool isSwitchable() {
        return true;
    }

    Own<Transformer> cloneImpl() const {
        return Own<Transformer>(cloning());
    }

private:
    virtual Transformer* cloning() const = 0;
};

}  // namespace souffle::ast::transform
