/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file GroundWitnesses.h
 *
 * Transformation pass to ground witnesses of an aggregate so that
 * they can be transferred to the head.
 *
 ***********************************************************************/

#pragma once

#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"

#include <string>

namespace souffle::ast::transform {

/**
 * Apply a grounding so that the witness of a selection aggregate (min/max)
 * can be transferred to the outer scope. Here is an example:
 *
 * Tallest(student) :- _ = max height : { Student(student, height) }.
 *
 * student occurs ungrounded in the outer scope, but we can fix this by using the
 * aggregate result to figure out which students satisfy this aggregate.
 *
 * Tallest(student) :- n = max height : { Student(student0, height) },
 *                      Student(student, n).
 *
 * This transformation is really just syntactic sugar.
 *
 */
class GroundWitnessesTransformer : public Transformer {
public:
    std::string getName() const override {
        return "GroundWitnessesTransformer";
    }

private:
    GroundWitnessesTransformer* cloning() const override {
        return new GroundWitnessesTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;
};

}  // namespace souffle::ast::transform
