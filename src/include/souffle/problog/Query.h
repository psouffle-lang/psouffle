#ifndef QUERY_H
#define QUERY_H

#include "Atom.h"
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Query {
public:
    Query(Atom atom)
        : queryAtom(std::move(atom)) {}

    std::string toString() const {
        std::ostringstream ss;
        ss << "query(" << queryAtom.toString() << ")";
        return ss.str();
    }

    bool hasWildcard() const {
        for (const auto& f : queryAtom.getFields()) {
            if (std::holds_alternative<VariableField>(f.field)) {
                const auto& var = std::get<VariableField>(f.field);
                if (var.name == "_") return true;
            }
        }
        return false;
    }

    const std::string& getRelationName() const {
        return queryAtom.getRelation();
    }

    bool matchesTuple(
            const std::string& relation, const std::vector<souffle::RamDomain>& fields) const {
        if (relation != queryAtom.getRelation()) {
            return false;
        }
        const auto& queryFields = queryAtom.getFields();
        if (queryFields.size() != fields.size()) {
            return false;
        }
        static const std::vector<std::string> noVars;
        static const std::vector<souffle::RamDomain> noValues;
        std::unordered_map<std::string, souffle::RamDomain> bindings;
        for (std::size_t i = 0; i < queryFields.size(); ++i) {
            if (std::holds_alternative<VariableField>(queryFields[i].field)) {
                const auto& var = std::get<VariableField>(queryFields[i].field);
                if (var.name == "_") {
                    continue;
                }
                auto [it, inserted] = bindings.emplace(var.name, fields[i]);
                if (!inserted && it->second != fields[i]) {
                    return false;
                }
                continue;
            }
            if (evaluateSymbolicField(queryFields[i], noVars, noValues) != fields[i]) {
                return false;
            }
        }
        return true;
    }

private:
    Atom queryAtom;
};

#endif
