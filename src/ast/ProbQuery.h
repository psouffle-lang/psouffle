#pragma once

#include "ast/Atom.h"
#include "ast/Directive.h"
#include "ast/Node.h"
#include "ast/Argument.h"

namespace souffle::ast {

/**
 * @class Query
 * @brief query(a(1)).
 */
class ProbQuery : public Node {
public:
    ProbQuery(Own<Atom> atom, SrcLocation loc);

    const Atom& getAtom() const;
    Atom& getAtom() { return *atom; }
    const QualifiedName& getAtomName() const;
    const std::vector<Argument*> getArguments() const;
    void print(std::ostream& os) const override;
    bool equal(const Node& other) const override;
    ProbQuery* cloning() const override;
    std::string toString() const;




private:
    Own<Atom> atom;
    bool value;
};

}