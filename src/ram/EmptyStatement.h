//
// Created by lxy10 on 12/25/2024.
//

/************************************************************************
 *
 * @file EmptyStatement.h
 *
 ***********************************************************************/

#pragma once

#include "ram/Node.h"
#include "ram/Statement.h"
#include "souffle/utility/MiscUtil.h"
#include "souffle/utility/StreamUtil.h"
#include <ostream>
#include <string>
#include <utility>

namespace souffle::ram {

/**
 * @class EmptyStatement
 */

class EmptyStatement : public Statement {
public:
    EmptyStatement() : Statement(NK_EmptyStatement) {}


    EmptyStatement* cloning() const override {
        return new EmptyStatement();
    }

    static bool classof(const Node* n) {
        return n->getKind() == NK_EmptyStatement;
    }

protected:
    void print(std::ostream& os, int tabpos) const override {
        os << times(" ", tabpos) << "EMPTY_STMT" << std::endl;
    }

    bool equal(const Node& node) const override {
        const auto& other = asAssert<EmptyStatement>(node);
        return true;
    }
};

}  // namespace souffle::ram

