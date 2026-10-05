/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file SequentialOperation.h
 *
 ***********************************************************************/

#pragma once

#include "ram/Node.h"
#include "ram/Operation.h"
#include "souffle/utility/MiscUtil.h"
#include <cassert>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <souffle/utility/ContainerUtil.h>

namespace souffle::ram {

/**
 * @class SequentialOperation
 * @brief class for a sequential operations in a loop-nest
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  QUERY
 *   ...
 *    IF C1
 *     ...
 *      INSERT
 *      RECORD DERIVATION
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 */
class SequentialOperation : public Operation {
public:
    SequentialOperation()
            : Operation(NK_SequentialOperation), operations() {
    }

    SequentialOperation(VecOwn<Operation> operations)
        : Operation(NK_SequentialOperation), operations(std::move(operations)) {}

    template <typename... Ops>
    SequentialOperation(Own<Ops>&&... ops) : Operation(NK_SequentialOperation) {
        Own<Operation> tmp[] = {std::move(ops)...};
        for (auto& cur : tmp) {
            assert(cur.get() != nullptr && "operation is a null-pointer");
            operations.emplace_back(std::move(cur));
        }
    }

    SequentialOperation* cloning() const {
        VecOwn<Operation> newOps;
        for (auto& cur : operations) {
            newOps.emplace_back(cur->cloning());
        }
        return new SequentialOperation(std::move(newOps));
        // return new SequentialOperation();
    }

    /** @brief Get sequantial operations */
    std::vector<Operation*> getOperations() const {
        return toPtrVector(operations);
    }

    NodeVec getChildren() const override {
        return toPtrVector<Node const>(operations);;
    }

    void apply(const NodeMapper& map) override {
        for (auto&& op : operations) {
            op = map(std::move(op));
        }
    }

    static bool classof(const Node* n) {
        const NodeKind kind = n->getKind();
        return (kind == NK_SequentialOperation);
    }

protected:
    void print(std::ostream& os, int tabpos) const override {
        for (const auto& cur : operations) {
            Operation::print(cur.get(), os, tabpos);
        }
    }

    bool equal(const Node& node) const override {
        const auto& other = asAssert<SequentialOperation>(node);
        if (operations.size() != other.operations.size()) {
            return false;
        }
        for (long unsigned int i=0; i < operations.size(); i++) {
            if (!equal_ptr(operations[i], other.operations[i])) {
                return false;
            }
        }
        return true;
    }

    /** Nested operation */
    VecOwn<Operation> operations;
};

}  // namespace souffle::ram
