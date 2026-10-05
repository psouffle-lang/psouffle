#include "souffle/problog/Rule.h"

Rule::Rule(std::size_t ruleId, Atom head, std::vector<Atom> bodyAtoms, std::vector<std::string> vars,
        double probability, bool recursive, bool recursiveStratum, bool isEqrelHead,
        std::vector<AggregateSpec> aggregates)
        : ruleId(ruleId),
          head(std::move(head)),
          bodyAtoms(std::move(bodyAtoms)),
          vars(std::move(vars)),
          probability(probability),
          recursive(recursive),
          recursiveStratum(recursiveStratum),
          isEqrelRelation(isEqrelHead),
          aggregates(std::move(aggregates)) {}

const Atom& Rule::getHead() const {
    return head;
}

const std::vector<Atom>& Rule::getBodyAtoms() const {
    return bodyAtoms;
}

std::size_t Rule::getRuleId() const {
    return ruleId;
}

double Rule::getProbability() const {
    return probability;
}

bool Rule::isFact() const {
    return bodyAtoms.empty();
}

void Rule::addBodyAtom(Atom atom) {
    bodyAtoms.push_back(std::move(atom));
}

std::string Rule::toString() const {
    std::ostringstream oss;
    oss << "[Rule " << ruleId;
    if (probability != 1.0) {
        oss << ", prob=" << probability;
    }
    if (recursive) {
        oss << ", recursive";
    }
    oss << "] " << head.toString();

    if (!bodyAtoms.empty()) {
        oss << " :- ";
        for (size_t i = 0; i < bodyAtoms.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << bodyAtoms[i].toString();
        }
    }
    oss << ". <";
    for (const auto& var : vars) {
        oss << " " << var;
    }
    oss << " >";
    return oss.str();
}
