/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file IfExistsConversion.h
 *
 ***********************************************************************/

#pragma once

#include "ram/IndexScan.h"
#include "ram/Operation.h"
#include "ram/Program.h"
#include "ram/Scan.h"
#include "ram/TranslationUnit.h"
#include "ram/analysis/Level.h"
#include "ram/transform/Transformer.h"
#include <memory>
#include <string>

namespace souffle::ram::transform {

/**
 * @class IfExistsConversionTransformer
 * @brief Convert (Scan/If)/(IndexScan/If) operaitons to
 * (IfExists)/(IndexIfExists) operations

 * If there exists Scan/IndexScan operations in the RAM, and the
 * variables are used in a subsequent Filter operation but no
 * subsequent operation in the tree (up until and including
 * Insert), the operations are rewritten to IfExists/IndexIfExists
 * operations.
 *
 * For example,
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  QUERY
 *   ...
 *    FOR t1 IN A ON INDEX t1.x=10 AND t1.y = 20
 *    	IF (t1.x, t1.y) NOT IN A
 *          ... // no occurrence of t1
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * will be rewritten to
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  QUERY
 *   ...
 *    IF ∃t1 IN A ON INDEX t1.x=10 AND t1.y = 20
 *    WHERE (t1.x, t1.y) NOT IN A
 *      ...
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 */
class IfExistsConversionTransformer : public Transformer {
public:
    std::string getName() const override {
        return "IfExistsConversionTransformer";
    }

    /**
     * @brief Rewrite Scan operations
     * @param A scan operation
     * @result The old operation if the if-conversion fails; otherwise the IfExists operation
     *
     * Rewrites Scan/If pair to a IfExists operation if value
     * is not used in a consecutive RAM operation
     */
    Own<Operation> rewriteScan(const Scan* scan);

    /**
     * @brief Rewrite IndexScan operations
     * @param An index operation
     * @result The old operation if the if-conversion fails; otherwise the IndexIfExists operation
     *
     * Rewrites IndexScan/If pair to an IndexIfExists operation if value
     * is not used in a consecutive RAM operation
     */
    Own<Operation> rewriteIndexScan(const IndexScan* indexScan);

    /**
     * @brief Apply if-exists conversion to the whole program
     * @param RAM program
     * @result A flag indicating whether the RAM program has been changed.
     *
     * Search for queries and rewrite their Scan/IndexScan and If operations if possible.
     */
    bool convertScans(Program& program);

protected:
    analysis::LevelAnalysis* rla{nullptr};
    bool transform(TranslationUnit& translationUnit) override {
        rla = &translationUnit.getAnalysis<analysis::LevelAnalysis>();
        return convertScans(translationUnit.getProgram());
    }
};

}  // namespace souffle::ram::transform
