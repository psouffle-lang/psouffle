#ifndef SOUFFLE_CLI_PENDING_OPERATION_STAGER_H
#define SOUFFLE_CLI_PENDING_OPERATION_STAGER_H

#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "souffle/Derivation.h"
#include "souffle/SouffleInterface.h"

namespace souffle::cli {

inline std::string getConcreteRelationName(const std::string& name, const std::string prefix) {
    return prefix + name;
}

inline std::string getIncDeltaTupleDeleteRelationName(const std::string& name) {
    return getConcreteRelationName(name, "$inc_delta_tuple_delete_");
}

inline std::string getIncDeltaTupleInsertRelationName(const std::string& name) {
    return getConcreteRelationName(name, "$inc_delta_tuple_insert_");
}

template <typename OperationT>
class PendingOperationStager {
public:
    using InputRelationMap = std::map<std::string, std::set<UntypedTuple>>;

    PendingOperationStager(
            souffle::SouffleProgram* program,
            InputRelationMap& initialInputRelations,
            std::unordered_map<UntypedTuple, double>& factProb,
            bool verbose = false)
            : program(program), initialInputRelations(initialInputRelations), factProb(factProb), verbose(verbose) {}

    bool stageInsert(OperationT& op) {
        auto* origRel = program->getRelation(op.relationName);
        auto* rel = program->getRelation(getIncDeltaTupleInsertRelationName(op.relationName));
        if (rel == nullptr) {
            std::cout << "Relation not found, omitted: " << op.relationName << std::endl;
            op.valid = false;
            return false;
        }
        if (op.values.size() != rel->getArity()) {
            std::cout << "Relation arity mismatch, omitted: " << op.relationName << std::endl;
            op.valid = false;
            return false;
        }

        souffle::tuple relTuple = souffle::tuple(rel);
        souffle::tuple origTuple = souffle::tuple{origRel};
        appendOperationValuesToTuple(relTuple, op);
        appendOperationValuesToTuple(origTuple, op);

        const auto untypedTuple = UntypedTuple::fromSouffleTuple(origTuple);
        auto& currentInputs = initialInputRelations[op.relationName];
        if (currentInputs.count(untypedTuple)) {
            std::cout << "Relation already contains the tuple to insert, omitted: "
                      << relTuple.toString() << std::endl;
            op.valid = false;
            return false;
        }

        if (verbose) {
            std::cout << "Inserting tuple: " << origTuple.toString() << std::endl;
        }
        rel->insert(relTuple);
        currentInputs.insert(untypedTuple);
        factProb[untypedTuple] = op.probability;
        return true;
    }

    bool stageDelete(OperationT& op) {
        auto* origRel = program->getRelation(op.relationName);
        auto* rel = program->getRelation(getIncDeltaTupleDeleteRelationName(op.relationName));
        if (rel == nullptr) {
            std::cout << "Relation not found, omitted: " << op.relationName << std::endl;
            op.valid = false;
            return false;
        }
        if (op.values.size() != rel->getArity()) {
            std::cout << "Relation arity mismatch, omitted: " << op.relationName << std::endl;
            op.valid = false;
            return false;
        }

        souffle::tuple relTuple = souffle::tuple(rel);
        souffle::tuple origTuple = souffle::tuple{origRel};
        appendOperationValuesToTuple(relTuple, op);
        appendOperationValuesToTuple(origTuple, op);

        const auto untypedTuple = UntypedTuple::fromSouffleTuple(origTuple);
        auto& currentInputs = initialInputRelations[op.relationName];
        if (!currentInputs.count(untypedTuple)) {
            std::cout << "Relation does not contains the tuple to delete, omitted: "
                      << origTuple.toString() << std::endl;
            op.valid = false;
            return false;
        }
        if (rel->contains(relTuple)) {
            std::cout << "Already deleted the tuple, omitted: " << relTuple.toString() << std::endl;
            op.valid = false;
            return false;
        }

        rel->insert(relTuple);
        currentInputs.erase(untypedTuple);
        factProb.erase(untypedTuple);
        auto& deletedFactRuleAppSet =
                DerivationManager::untypedTuple2RuleApplications[UntypedTuple::fromSouffleTuple(origTuple)];
        if (deletedFactRuleAppSet != nullptr && !deletedFactRuleAppSet->empty()) {
            auto& deltaDeletedFactRuleAppSet =
                    DerivationManager::untypedTuple2DeltaDeleteRuleApplications[
                            UntypedTuple::fromSouffleTuple(origTuple)];
            if (deltaDeletedFactRuleAppSet == nullptr) {
                deltaDeletedFactRuleAppSet = new std::unordered_set<RuleApplication>();
            }
            for (auto& ruleApp : *deletedFactRuleAppSet) {
                deltaDeletedFactRuleAppSet->insert(ruleApp);
            }
            deletedFactRuleAppSet->clear();
            delete deletedFactRuleAppSet;
            deletedFactRuleAppSet = nullptr;
            DerivationManager::untypedTuple2RuleApplications.erase(
                    UntypedTuple::fromSouffleTuple(origTuple));
        }
        return true;
    }

    void stageAll(std::vector<OperationT>& operations) {
        purgeAllIncDeltaRelations();
        for (auto& op : operations) {
            if (op.type == OperationT::INSERT) {
                stageInsert(op);
            } else if (op.type == OperationT::DELETE) {
                stageDelete(op);
            }
        }
    }

    void purgeAllIncDeltaRelations() {
        for (auto* rel : program->getAllRelations()) {
            if (rel->getName()[0] == '$') {
                rel->purge();
            }
        }
    }

    void purgeAllNonIncDeltaRelations() {
        for (auto* rel : program->getAllRelations()) {
            if (rel->getName()[0] != '$') {
                rel->purge();
            }
        }
    }

    void purgeAllRelations() {
        for (auto* rel : program->getAllRelations()) {
            rel->purge();
        }
    }

    void loadInitialInputRelations() {
        for (auto* rel : program->getInputRelations()) {
            for (auto& tuple : initialInputRelations[rel->getName()]) {
                souffle::tuple relTuple = souffle::tuple(rel);
                for (const auto& field : tuple.fields) {
                    relTuple << field;
                }
                rel->insert(relTuple);
            }
        }
    }

private:
    void appendOperationValuesToTuple(souffle::tuple& relTuple, const OperationT& op) const {
        for (const auto& value : op.values) {
            relTuple << std::stoi(value);
        }
    }

    souffle::SouffleProgram* program = nullptr;
    InputRelationMap& initialInputRelations;
    std::unordered_map<UntypedTuple, double>& factProb;
    bool verbose = false;
};

}  // namespace souffle::cli

#endif
