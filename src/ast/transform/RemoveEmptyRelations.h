/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file RemoveEmptyRelations.h
 *
 ***********************************************************************/

#pragma once

#include "ast/QualifiedName.h"
#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

/**
 * Transformation pass to remove all empty relations and rules that use empty relations.
 */
class RemoveEmptyRelationsTransformer : public Transformer {
public:
    std::string getName() const override {
        return "RemoveEmptyRelationsTransformer";
    }

    /**
     * Eliminate all empty relations (and their uses) in the given program.
     *
     * @param translationUnit the program to be processed
     * @return whether the program was modified
     */
    static bool removeEmptyRelations(TranslationUnit& translationUnit);

private:
    RemoveEmptyRelationsTransformer* cloning() const override {
        return new RemoveEmptyRelationsTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override {
        return removeEmptyRelations(translationUnit);
    }

    /**
     * Eliminate rules that contain empty relations and/or rewrite them.
     *
     * @param translationUnit the program to be processed
     * @param emptyRelation relation that is empty
     * @return whether the program was modified
     */
    static bool removeEmptyRelationUses(TranslationUnit& translationUnit, const QualifiedName& emptyRelation);
};

}  // namespace souffle::ast::transform
