#include "souffle/problog/RuleManager.h"

RuleManager ruleManager({});

RuleManager::RuleManager(std::vector<Rule> rules, std::vector<std::string> eqrelRelations,
        bool compilerRecursionMetadata) {
    for (const auto& rel : eqrelRelations) {
        addEqrelRelation(rel);
    }
    for (auto& rule : rules) {
        addRule(std::move(rule));
    }
    compilerRecursionMetadataTrusted = compilerRecursionMetadata;
}

void RuleManager::addRule(Rule rule) {
    compilerRecursionMetadataTrusted = false;
    auto ruleId = rule.getRuleId();
    const std::string& headPredicate = rule.getHead().getRelation();
    predicateToRules[headPredicate].insert(ruleId);
    rules.emplace(ruleId, std::move(rule));
}

const Rule* RuleManager::getRule(std::size_t ruleId) const {
    auto it = rules.find(ruleId);
    return it != rules.end() ? &(it->second) : nullptr;
}

std::vector<const Rule*> RuleManager::getRulesForPredicate(const std::string& predicate) const {
    std::vector<const Rule*> result;
    auto it = predicateToRules.find(predicate);
    if (it != predicateToRules.end()) {
        for (auto ruleId : it->second) {
            if (auto rule = getRule(ruleId)) {
                result.push_back(rule);
            }
        }
    }
    return result;
}

std::vector<const Rule*> RuleManager::getRulesDependingOn(const std::string& predicate) const {
    std::vector<const Rule*> result;
    for (const auto& [ruleId, rule] : rules) {
        for (const auto& bodyAtom : rule.getBodyAtoms()) {
            if (bodyAtom.getRelation() == predicate) {
                result.push_back(&rule);
                break;
            }
        }
    }
    return result;
}

std::vector<const Rule*> RuleManager::getAllRules() const {
    std::vector<const Rule*> result;
    result.reserve(rules.size());
    for (const auto& [_, rule] : rules) {
        result.push_back(&rule);
    }
    return result;
}

bool RuleManager::removeRule(std::size_t ruleId) {
    compilerRecursionMetadataTrusted = false;
    auto it = rules.find(ruleId);
    if (it == rules.end()) {
        return false;
    }
    const std::string& headPredicate = it->second.getHead().getRelation();
    auto& ruleSet = predicateToRules[headPredicate];
    ruleSet.erase(ruleId);
    if (ruleSet.empty()) {
        predicateToRules.erase(headPredicate);
    }
    rules.erase(it);
    return true;
}

bool RuleManager::hasRule(std::size_t ruleId) const {
    return rules.find(ruleId) != rules.end();
}

std::size_t RuleManager::size() const {
    return rules.size();
}

bool RuleManager::hasCompilerAcyclicityCertificate() const {
    if (!compilerRecursionMetadataTrusted || !eqrelRelations.empty()) return false;
    const auto maxEncodedIndex = static_cast<std::size_t>(souffle::MAX_RAM_SIGNED);
    for (const auto& [ruleId, rule] : rules) {
        if (rule.isRecursive() || rule.isInRecursiveStratum() || rule.isEqrel() ||
                ruleId > maxEncodedIndex) return false;
        const auto aggregateCount = rule.getAggregates().size();
        if (aggregateCount != 0 && aggregateCount - 1 > maxEncodedIndex) return false;
    }
    return true;
}

std::string RuleManager::toString() const {
    std::ostringstream oss;

    oss << "RuleManager contains " << rules.size() << " rules:\n";

    std::map<std::string, std::vector<const Rule*>> rulesByPredicate;
    for (const auto& [ruleId, rule] : rules) {
        const std::string& predicate = rule.getHead().getRelation();
        rulesByPredicate[predicate].push_back(&rule);
    }

    for (const auto& [predicate, predicateRules] : rulesByPredicate) {
        oss << "\nRules defining '" << predicate << "':\n";
        for (const Rule* rule : predicateRules) {
            oss << "  " << rule->toString() << "\n";
        }
    }

    return oss.str();
}
