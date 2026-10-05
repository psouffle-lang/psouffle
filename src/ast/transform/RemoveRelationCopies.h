/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file RemoveRelationCopies.h
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <string>

namespace souffle::ast::transform {

/**
 * Transformation pass to replaces copy of relations by their origin.
 * For instance, if a relation r is defined by
 *
 *      r(X,Y) :- s(X,Y)
 *
 * and no other clause, all occurrences of r will be replaced by s.
 */
class RemoveRelationCopiesTransformer : public Transformer {
public:
    std::string getName() const override {
        return "RemoveRelationCopiesTransformer";
    }

    /**
     * Replaces copies of relations by their origin in the given program.
     *
     * @param program the program to be processed
     * @return whether the program was modified
     */
    static bool removeRelationCopies(TranslationUnit& translationUnit);

private:
    RemoveRelationCopiesTransformer* cloning() const override {
        return new RemoveRelationCopiesTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override {
        return removeRelationCopies(translationUnit);
    }
};

}  // namespace souffle::ast::transform
