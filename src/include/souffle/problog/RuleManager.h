#ifndef RULEMANAGER_H
#define RULEMANAGER_H

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <cstddef>
#include "souffle/problog/Rule.h"

class RuleManager {
public:
    RuleManager(std::vector<Rule> rules, std::vector<std::string> eqrelRelations = {});
    void addRule(Rule rule);
    const Rule* getRule(std::size_t ruleId) const;
    std::vector<const Rule*> getRulesForPredicate(const std::string& predicate) const;
    std::vector<const Rule*> getRulesDependingOn(const std::string& predicate) const;
    std::vector<const Rule*> getAllRules() const;
    bool removeRule(std::size_t ruleId);
    bool hasRule(std::size_t ruleId) const;
    std::size_t size() const;
    std::string toString() const;
    bool isRecursive(std::size_t ruleId) const {
        return getRule(ruleId)->isRecursive();
    }
    bool isInRecursiveStratum(std::size_t ruleId) const {
        return getRule(ruleId)->isInRecursiveStratum();
    }
    bool isEqrelRelation(const std::string& predicate) const {
        return eqrelRelations.count(predicate) != 0;
     }
    void addEqrelRelation(const std::string& predicate) {
        eqrelRelations.insert(predicate);
    }

private:
    std::unordered_map<std::size_t, Rule> rules;
    std::unordered_map<std::string, std::unordered_set<std::size_t>> predicateToRules;
    std::unordered_set<std::string> eqrelRelations;
};

extern RuleManager ruleManager;

#endif //RULEMANAGER_H
