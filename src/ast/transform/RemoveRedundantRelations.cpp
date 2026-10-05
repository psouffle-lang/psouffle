/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file RemoveRedundantRelations.cpp
 *
 ***********************************************************************/

#include "ast/transform/RemoveRedundantRelations.h"
#include "ast/Program.h"
#include "ast/TranslationUnit.h"
#include "ast/analysis/RedundantRelations.h"
#include "ast/utility/Utils.h"

namespace souffle::ast::transform {

bool RemoveRedundantRelationsTransformer::transform(TranslationUnit& translationUnit) {
    bool changed = false;
    auto& redundantRelationsAnalysis = translationUnit.getAnalysis<analysis::RedundantRelationsAnalysis>();
    for (auto&& name : redundantRelationsAnalysis.getRedundantRelations()) {
        changed |= translationUnit.getProgram().removeRelation(name);
    }
    return changed;
}

}  // namespace souffle::ast::transform
