#include "ast/Evidence.h"
#include "souffle/utility/StringUtil.h"
#include "souffle/utility/MiscUtil.h"


namespace souffle::ast {

Evidence::Evidence(Own<Atom> atom, bool value, SrcLocation loc)
    : Node(NK_Evidence, loc),
      atom(std::move(atom)), value(value) {}

const Atom& Evidence::getAtom() const {
    return *atom;
}

const QualifiedName& Evidence::getAtomName() const {
    return atom->getQualifiedName();
}

const std::vector<Argument*> Evidence::getArguments() const {
    return atom->getArguments();
}

bool Evidence::getEvidenceValue() const {
    return value;
}

void Evidence::print(std::ostream& os) const {
    os << "evidence";
    if (!value) {
        os << " not";
    }
        os << *atom;
}

bool Evidence::equal(const Node& other) const {
    const auto& otherEvidence = dynamic_cast<const Evidence*>(&other);
    return otherEvidence != nullptr && value == otherEvidence->value && *atom == *(otherEvidence->atom);
}

Evidence *Evidence::cloning() const {
    return new Evidence(souffle::clone(atom), value, getSrcLoc());
}

};
