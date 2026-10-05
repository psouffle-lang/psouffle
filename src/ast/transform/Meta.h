/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Meta.h
 *
 * Defines the interface for AST meta-transformation passes.
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <set>
#include <string>
#include <vector>

namespace souffle::ast::transform {

/**
 * Transformer that coordinates other sub-transformations
 */
class MetaTransformer : public Transformer {
protected:
    bool verbose = false;

public:
    /* Get subtransformers */
    virtual std::vector<Transformer*> getSubtransformers() const = 0;

    /* Enable the debug-report for all sub-transformations */
    virtual void setDebugReport() = 0;

    /* Enable high verbosity */
    virtual void setVerbosity(bool verbose) = 0;

    /* Disable subtransformers */
    virtual void disableTransformers(const std::set<std::string>& transforms) = 0;

    /* Apply a nested transformer */
    bool applySubtransformer(TranslationUnit& translationUnit, Transformer* transformer);
};

}  // namespace souffle::ast::transform
