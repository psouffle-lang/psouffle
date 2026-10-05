/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

#include "ast/Clause.h"
#include "ast/Aggregator.h"
#include "ast/Variable.h"
#include "ast/utility/Visitor.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/MiscUtil.h"
#include "souffle/utility/NodeMapperFwd.h"
#include "souffle/utility/StreamUtil.h"
#include <cassert>
#include <ostream>
#include <utility>

namespace souffle::ast {

namespace {
struct ClauseVariableSummary {
    std::vector<std::string> variables;
    bool hasRederive = false;
};

ClauseVariableSummary collectClauseVariables(const VecOwn<Literal>& bodyLiterals) {
    ClauseVariableSummary summary;
    for (const auto& lit : bodyLiterals) {
        Atom* atom = nullptr;
        if (isA<Atom>(lit)) {
            atom = as<Atom>(lit);
        } else if (isA<Negation>(lit)) {
            atom = as<Negation>(lit)->getAtom();
        }
        if (atom != nullptr && atom->isRederive) {
            summary.hasRederive = true;
            continue;
        }
        visitFrontier(*lit, [&](const Node& node) {
            if (isA<Aggregator>(node)) {
                return true;
            }
            if (const auto* var = as<Variable>(&node)) {
                const auto& varName = var->getName();
                if (std::find(summary.variables.begin(), summary.variables.end(), varName) == summary.variables.end()) {
                    summary.variables.emplace_back(varName);
                }
            }
            return false;
        });
    }
    return summary;
}
}  // namespace

Clause::Clause(
        NodeKind kind, Own<Atom> head, VecOwn<Literal> bodyLiterals, Own<ExecutionPlan> plan, SrcLocation loc)
        : Node(kind, std::move(loc)), head(std::move(head)), bodyLiterals(std::move(bodyLiterals)),
          plan(std::move(plan)) {
    assert(this->head != nullptr);
    assert(allValidPtrs(this->bodyLiterals));
    assert(kind >= NK_Clause && kind < NK_LastClause);
    // Execution plan can be null
    auto summary = collectClauseVariables(this->bodyLiterals);
    variables = std::move(summary.variables);
    isRederive = summary.hasRederive;
}

Clause::Clause(Own<Atom> head, VecOwn<Literal> bodyLiterals, Own<ExecutionPlan> plan, SrcLocation loc)
        : Clause(NK_Clause, std::move(head), std::move(bodyLiterals), std::move(plan), std::move(loc)) {
}

Clause::Clause(Own<Atom> head, SrcLocation loc) : Clause(std::move(head), {}, {}, std::move(loc)) {
}

Clause::Clause(QualifiedName name, SrcLocation loc) : Clause(mk<Atom>(name), std::move(loc)) {}

void Clause::addToBody(Own<Literal> literal) {
    assert(literal != nullptr);
    bodyLiterals.push_back(std::move(literal));
    auto summary = collectClauseVariables(bodyLiterals);
    variables = std::move(summary.variables);
    isRederive = summary.hasRederive;
}
void Clause::addToBody(VecOwn<Literal>&& literals) {
    assert(allValidPtrs(literals));
    for (auto& lit : literals) {
        bodyLiterals.push_back(std::move(lit));
    }
    auto summary = collectClauseVariables(bodyLiterals);
    variables = std::move(summary.variables);
    isRederive = summary.hasRederive;
}

void Clause::setHead(Own<Atom> h) {
    assert(h != nullptr);
    head = std::move(h);
}

void Clause::setBodyLiterals(VecOwn<Literal> body) {
    assert(allValidPtrs(body));
    bodyLiterals = std::move(body);
    auto summary = collectClauseVariables(bodyLiterals);
    variables = std::move(summary.variables);
    isRederive = summary.hasRederive;
}

std::vector<std::string> Clause::getVariables() const {
    return collectClauseVariables(bodyLiterals).variables;
}

std::vector<Literal*> Clause::getBodyLiterals() const {
    return toPtrVector(bodyLiterals);
}

void Clause::setExecutionPlan(Own<ExecutionPlan> plan) {
    this->plan = std::move(plan);
}

void Clause::apply(const NodeMapper& map) {
    head = map(std::move(head));
    mapAll(bodyLiterals, map);
}

Node::NodeVec Clause::getChildren() const {
    std::vector<const Node*> res = {head.get()};
    append(res, makePtrRange(bodyLiterals));
    return res;
}

void Clause::print(std::ostream& os) const {
    printAnnotations(os);
    os << *head;
    if (!bodyLiterals.empty()) {
        os << " :- \n   " << join(bodyLiterals, ",\n   ");
    }
    os << ".";
    if (plan != nullptr) {
        os << *plan;
    }
}

std::string Clause::toString() const {
    std::stringstream resultStream;
    resultStream << *head;
    if (!bodyLiterals.empty()) {
        resultStream << " :-  " << join(bodyLiterals, ",  ");
    }
    resultStream << ".";
    if (plan != nullptr) {
        resultStream << *plan;
    }
    return resultStream.str();
}

void Clause::printForDebugInfo(std::ostream& os) const {
    os << *head;
    if (!bodyLiterals.empty()) {
        os << " :- \n   " << join(bodyLiterals, ",\n   ");
    }
    os << ".";
}

bool Clause::equal(const Node& node) const {
    const auto& other = asAssert<Clause>(node);
    return equal_ptr(head, other.head) && equal_targets(bodyLiterals, other.bodyLiterals) &&
           equal_ptr(plan, other.plan);
}

Clause* Clause::cloning() const {
    auto* cl = new Clause(clone(head), clone(bodyLiterals), clone(plan), getSrcLoc());
    cl->setClauseId(clauseId);
    cl->setProbability(probability);
    cl->setVariables(getVariables());
    cl->isRederive = isRederive;
    cl->setRecursive(recursive);
    cl->setInRecursiveStratum(inRecursiveStratum);
    return cl;
}

Clause* Clause::cloneHead() const {
    Clause* myClone = new Clause(clone(head), getSrcLoc());
    if (getExecutionPlan() != nullptr) {
        myClone->setExecutionPlan(clone(getExecutionPlan()));
    }
    myClone->setAnnotationsFrom(*this);
    myClone->setClauseId(getClauseId());
    myClone->setProbability(getProbability());
    myClone->setVariables(getVariables());
    return myClone;
}

bool Clause::classof(const Node* n) {
    const NodeKind kind = n->getKind();
    return (kind >= NK_Clause && kind < NK_LastClause);
}

}  // namespace souffle::ast
