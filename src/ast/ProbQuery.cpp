#include "ast/ProbQuery.h"
#include "souffle/utility/StringUtil.h"
#include "souffle/utility/MiscUtil.h"


namespace souffle::ast {

ProbQuery::ProbQuery(Own<Atom> atom, SrcLocation loc)
    : Node(NK_ProbQuery, loc),
      atom(std::move(atom)) {}

const Atom& ProbQuery::getAtom() const {
    return *atom;
}

const QualifiedName& ProbQuery::getAtomName() const {
    return atom->getQualifiedName();
}

const std::vector<Argument*> ProbQuery::getArguments() const {
    return atom->getArguments();
}

void ProbQuery::print(std::ostream& os) const {
    os << "query(";
    os << *atom;
    os << ")";
}

bool ProbQuery::equal(const Node& other) const {
    const auto& otherQuery = dynamic_cast<const ProbQuery*>(&other);
    return otherQuery != nullptr &&  *atom == *(otherQuery->atom);
}

ProbQuery *ProbQuery::cloning() const {
    return new ProbQuery(souffle::clone(atom), getSrcLoc());
}

std::string ProbQuery::toString() const {
    std::ostringstream ss;
    ss << "query(" << *atom << ")";
    return ss.str();
}
};