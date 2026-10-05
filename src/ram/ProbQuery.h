#pragma once

#include "ram/Statement.h"
#include "souffle/profile/EventProcessor.h"
#include "ram/Relation.h"
#include <string>

namespace souffle::ram {

/**
 * @class ProbQuery
 * @brief Represents an query, e.g., query(a(1)).
 **/

class ProbQuery : public Statement {
public:
    ProbQuery(std::string rel, std::string tuple)
        : Statement(NK_ProbQuery), relation(std::move(rel)), tupleStr(std::move(tuple)) {}

    const std::string& getRelation() const { return relation; }

    const std::string& getTupleString() const {return tupleStr;}

    ProbQuery* cloning() const override {
        return new ProbQuery(relation, tupleStr);
    }

    void apply(const NodeMapper&) override {};

    static bool classof(const Node* n) {
        return n->getKind() == NK_ProbQuery;
    }
protected:
    void print(std::ostream& os, int tabpos) const override {
        os << times(" ", tabpos) << "QUERY " << relation << "(" << tupleStr << ")" << "\n";
    }

private:
    std::string relation;
    std::string tupleStr;
};

}