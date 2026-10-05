//
// Created by 17308 on 2025/6/26.
//

#pragma once

#include "ram/Statement.h"
#include "souffle/utility/StreamUtil.h"
#include <string>

namespace souffle::ram {

/**
 * @class Evidence
 * @brief Represents an evidence directive, e.g., evidence(x(a), true)
 */
class Evidence : public Statement {
public:
    Evidence(std::string rel, std::string tuple, bool val)
        : Statement(NK_Evidence), relation(std::move(rel)), tupleStr(std::move(tuple)), value(val) {}

    const std::string& getRelation() const { return relation; }

    const std::string& getTupleString() const { return tupleStr; }

    bool getValue() const { return value; }

    Evidence* cloning() const override {
        return new Evidence(relation, tupleStr, value);
    }

    void apply(const NodeMapper&) override {
        // no children to map
    }

    static bool classof(const Node* n) {
        return n->getKind() == NK_Evidence;
    }

protected:
    void print(std::ostream& os, int tabpos) const override {
        os << times(" ", tabpos) << "EVIDENCE " << relation << "(" << tupleStr << "), value=" << value << "\n";
    }

    bool equal(const Node& node) const override {
        const auto& other = asAssert<Evidence>(node);
        return relation == other.relation && tupleStr == other.tupleStr && value == other.value;
    }

    NodeVec getChildren() const override {
        return {};
    }

private:
    std::string relation;
    std::string tupleStr;
    bool value;
};

}
