/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ResolveAnonymousRecordAliases.h
 *
 ***********************************************************************/

#pragma once

#include "ast/Clause.h"
#include "ast/RecordInit.h"
#include "ast/TranslationUnit.h"
#include "ast/transform/Transformer.h"
#include <map>
#include <string>

namespace souffle::ast::transform {

/**
 * Transformer resolving aliases for anonymous records.
 *
 * The transformer works by searching the clause for equalities
 * of the form a = [...], where a is an anonymous record, and replacing
 * all occurrences of a with the RHS.
 *
 * The transformer is to be called in conjunction with FoldAnonymousRecords.
 **/
class ResolveAnonymousRecordAliasesTransformer : public Transformer {
public:
    std::string getName() const override {
        return "ResolveAnonymousRecordAliases";
    }

private:
    ResolveAnonymousRecordAliasesTransformer* cloning() const override {
        return new ResolveAnonymousRecordAliasesTransformer();
    }

    bool transform(TranslationUnit& translationUnit) override;

    /**
     * Use mapping found by findVariablesRecordMapping to substitute
     * a records for each variable that operates on records.
     **/
    bool replaceNamedVariables(TranslationUnit&, Clause&);

    /**
     * For each variable equal to some anonymous record,
     * assign a value of that record.
     **/
    std::map<std::string, const RecordInit*> findVariablesRecordMapping(TranslationUnit&, const Clause&);

    /**
     * For unnamed variables, replace each equation _ op record with true.
     **/
    bool replaceUnnamedVariable(Clause&);
};

}  // namespace souffle::ast::transform
