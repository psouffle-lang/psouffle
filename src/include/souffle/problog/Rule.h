#ifndef RULE_H
#define RULE_H

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <variant>
#include <memory>
#include <sstream>
#include <map>
#include "souffle/RamTypes.h"
#include "souffle/problog/Atom.h"

struct AggregateSpec {
    std::string op = "sum";
    std::string resultVar;
    Atom witnessAtom;
    SymbolicField weightExpr;

    AggregateSpec(std::string op, std::string resultVar, Atom witnessAtom, SymbolicField weightExpr)
            : op(op),
              resultVar(std::move(resultVar)),
              witnessAtom(std::move(witnessAtom)),
              weightExpr(std::move(weightExpr)) {}
};

class Rule {
public:
    Rule(std::size_t ruleId, Atom head, std::vector<Atom> bodyAtoms = {},
            std::vector<std::string> vars = {}, double probability = 1.0,
            bool recursive = false, bool recursiveStratum = false,
            bool isEqrelHead = false, std::vector<AggregateSpec> aggregates = {});

    const Atom& getHead() const;
    const std::vector<Atom>& getBodyAtoms() const;
    const std::vector<AggregateSpec>& getAggregates() const {
        return aggregates;
    }
    std::size_t getRuleId() const;
    double getProbability() const;
    bool isFact() const;
    std::string toString() const;
    std::vector<std::string> getVars() const {
        return vars;
    }
    bool isDeterminstic() const {
        return probability == 1.0;
    }
    bool isRecursive() const {
        return recursive;
    }
    bool isInRecursiveStratum() const {
        return recursiveStratum;
    }
    bool isEqrel() const {
        return isEqrelRelation;
    }
private:
    std::size_t ruleId;
    Atom head;
    std::vector<Atom> bodyAtoms;
    std::vector<std::string> vars;
    double probability;
    bool recursive;
    bool recursiveStratum;
    bool isEqrelRelation;
    std::vector<AggregateSpec> aggregates;
    void addBodyAtom(Atom atom);
};

#endif //RULE_H
