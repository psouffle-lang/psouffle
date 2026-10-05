/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file TupleId.h
 *
 ***********************************************************************/

#pragma once

#include "ram/Program.h"
#include "ram/TranslationUnit.h"
#include "ram/transform/Transformer.h"
#include <string>

namespace souffle::ram::transform {

/**
 * @class TupleIdTransformer
 * @brief Ordering tupleIds in TupleOperation operations correctly
 *
 * Transformations, like MakeIndex and IfConversion do not
 * ensure that TupleOperations maintain an appropriate order
 * with respect to their tupleId's
 *
 * For example:
 * SEARCH ... (tupleId = 2)
 * ...
 * 		SEARCH ... (tupleId = 1)
 * 			...
 *
 * Will be converted to
 * SEARCH ... (tupleId = 0)
 * ...
 * 		SEARCH ... (tupleId = 1)
 * 			...
 *
 */
class TupleIdTransformer : public Transformer {
public:
    std::string getName() const override {
        return "TupleIdTransformer";
    }

    /**
     * @brief Apply tupleId reordering to the whole program
     * @param RAM program
     * @result A flag indicating whether the RAM program has been changed.
     *
     * Search for TupleOperations and TupleElements and rewrite their tupleIds
     */
    bool reorderOperations(Program& program);

protected:
    bool transform(TranslationUnit& translationUnit) override {
        return reorderOperations(translationUnit.getProgram());
    }
};

}  // namespace souffle::ram::transform
