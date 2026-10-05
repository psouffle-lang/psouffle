/************************************************************************
 *
 * @file DerivationCheck.h
 *
 * Defines a condition that check whether a specific derivation of a tuple
 * is already derived in the Relational Algebra Machine.
 * For probabilistic setting.
 *
 ***********************************************************************/

#pragma once

#include "Expression.h"
#include "ram/Condition.h"
#include "ram/Node.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/MiscUtil.h"
#include <cassert>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include <souffle/utility/StreamUtil.h>

namespace souffle::ram {

/**
 * @class DerivationCheck
 * @brief Check the existence of a derivation
 *
 * For example:
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * (Derived t0 in A with ruleapp)
 * will be an existence check plus a derivation check in concrete relation with complete derivation set, during recursion;
 * may need further optimization
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */
class DerivationCheck : public Condition {
public:
    // DerivationCheck() : Condition(NK_DerivationCheck) {
    //     // assert(operand != nullptr && "operand of negation is a null-pointer");
    //     assert( false && "not impl");
    // }

    /** @brief Get operand of negation */
    // const Condition& getOperand() const {
    //     return *operand;
    // }

    /** @brief Get relation */
    const std::string& getRelation() const {
        return rel;
    }

    DerivationCheck* cloning() const override {
        VecOwn<Expression> newValues;
        for (auto& expr : expressions) {
            newValues.emplace_back(expr->cloning());
        }
        std::map<std::string, Own<ram::Expression>> newVarExprMap{};
        for (auto& [var, expr] : varExprMap) {
            newVarExprMap.emplace(var, expr->cloning());
        }
        std::vector<Own<ram::Expression>> newVarExprs{};
        for (auto& expr : varExprs) {
            newVarExprs.emplace_back(expr->cloning());
        }
        return new DerivationCheck(rel, std::move(newValues), clauseID, std::move(newVarExprMap), std::move(newVarExprs));
    }

    // TODO: when do we call this function???
    void apply(const NodeMapper& map) override {
        for (auto& expr : expressions) {
            expr = map(std::move(expr));
        }
        // TODO: do we need to map varValueMap?
        for (auto& [var, expr] : varExprMap) {
            expr = map(std::move(expr));
        }

        for (auto& expr : varExprs) {
            expr = map(std::move(expr));
        }
    }

    static bool classof(const Node* n) {
        return n->getKind() == NK_DerivationCheck;
    }

    void outputVarExprMapString(std::ostream& ss, std::function<void(std::ostream&, const Expression*)>& rec) const {
        ss << "{";
        bool first = true;
        for (auto& [var, expr] : varExprMap) {
            if (!first) {ss << ",";}
            ss << "{\"" << var << "\",";
            rec(ss, expr.get());
            ss << "}";
            first = false;
        }
        ss << "}";
    }

    void outputVarExprsString(std::ostream& ss, std::function<void(std::ostream&, const Expression*)>& rec) const {
        ss << "{";
        bool first = true;
        for (auto& expr : varExprs) {
            if (!first) {ss << ",";}
            rec(ss, expr.get());
            first = false;
        }
        ss << "}";
    }

// protected:

    DerivationCheck(std::string rel, VecOwn<Expression> expressions, std::size_t clauseID, /*bool isDelete, bool isComplete,*/ std::map<std::string, Own<ram::Expression>>&& varExprMap, std::vector<Own<ram::Expression>> varExprs) //
        : Condition(NK_DerivationCheck), rel(std::move(rel)), expressions(std::move(expressions)),
        varExprMap(std::move(varExprMap)),
        varExprs(std::move(varExprs)),
        // isComplete(isComplete), isDelete(isDelete),
        clauseID(clauseID)
    {
        assert(allValidPtrs(expressions));
        // TODO
    }

    void print(std::ostream& os) const override {
        os << "(IF "
        << join(expressions, ", ", print_deref<Own<Expression>>())
        << " DERIVED BY RULE" << std::to_string(clauseID)
        << " OF REL " << rel
        << ", WITH MAPPING " << "<placeholder>"
        // << (!isDelete?"INSERT":"DELETE") << ", "
        // << (isComplete?"COMPLETE":"DELTA") << ", "
        << ")";
        for (auto& [var, expr] : varExprMap) {
            os << times(" ", 2) << var << ": " << *expr << ",";
        }
    }

    bool equal(const Node& node) const override {
        const auto& other = asAssert<DerivationCheck>(node);
        return rel == other.rel
        && equal_targets(expressions, other.expressions)
        && clauseID == other.clauseID;
        // TODO: equal mapping?
    }

    NodeVec getChildren() const override {
        NodeVec nodes;
        for (auto& [_, v] : varExprMap) {
            nodes.push_back(v.get());
        }
        auto vec = toPtrVector<Node const>(expressions);
        nodes.insert(nodes.end(), vec.begin(), vec.end());
        return nodes;
        // return toPtrVector<Node const>(expressions); // TODO: varExprMap?
    }

    const std::vector<Expression*> getValues() const {
        return toPtrVector(expressions);
    }

    std::string rel;
    VecOwn<ram::Expression> expressions;
    std::map<std::string, Own<ram::Expression>> varExprMap;  // TODO
    std::vector<Own<ram::Expression>> varExprs;  // TODO

    // bool isComplete;
    // bool isDelete;
    size_t clauseID;
};

}  // namespace souffle::ram
