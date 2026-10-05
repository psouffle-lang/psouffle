/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ResolveAliases.h
 *
 ***********************************************************************/

#pragma once

#include "ast/Clause.h"
#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include "souffle/utility/ContainerUtil.h"
#include <memory>
#include <string>

namespace souffle::ast::transform {

/**
 * Transformation pass to eliminate grounded aliases.
 * e.g. resolve: a(r) , r = [x,y]       => a(x,y)
 * e.g. resolve: a(x) , !b(y) , y = x   => a(x) , !b(x)
 */
class ResolveAliasesTransformer : public Transformer {
public:
    std::string getName() const override {
        return "ResolveAliasesTransformer";
    }

    /**
     * ResolveAliasesTransformer cannot be disabled.
     */
    bool isSwitchable() override {
        return false;
    }

    /**
     * Converts the given clause into a version without variables aliasing
     * grounded variables.
     *
     * @param clause the clause to be processed
     * @return a modified clone of the processed clause
     */
    static Own<Clause> resolveAliases(const Clause& clause);

    /**
     * Removes trivial equalities of the form t = t from the given clause.
     *
     * @param clause the clause to be processed
     * @return a modified clone of the given clause
     */
    static Own<Clause> removeTrivialEquality(const Clause& clause);

    /**
     * Removes complex terms in atoms, replacing them with constrained variables.
     *
     * @param clause the clause to be processed
     * @return a modified clone of the processed clause
     */
    static Own<Clause> removeComplexTermsInAtoms(const Clause& clause);

private:
    ResolveAliasesTransformer* cloning() const override {
        return new ResolveAliasesTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
