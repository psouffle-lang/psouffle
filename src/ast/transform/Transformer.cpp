/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Transformer.cpp
 *
 * Defines the interface for AST transformation passes.
 *
 ***********************************************************************/

#include "ast/transform/Transformer.h"
#include "ast/TranslationUnit.h"
#include "reports/ErrorReport.h"

namespace souffle::ast::transform {

bool Transformer::apply(TranslationUnit& translationUnit) {
    // invoke the transformation
    bool changed = transform(translationUnit);

    if (changed) {
        translationUnit.invalidateAnalyses();
    }

    /* Abort evaluation of the program if errors were encountered */
    translationUnit.getErrorReport().exitIfErrors();

    return changed;
}

}  // namespace souffle::ast::transform
