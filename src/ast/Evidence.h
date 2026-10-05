#pragma once

#include "ast/Atom.h"
#include "ast/Directive.h"
#include "ast/Node.h"
#include "ast/Argument.h"

namespace souffle::ast {

/**
 * @class Evidence
 * @brief Represents an evidence directive: e.g., evidence(a(1), true)
 */
class Evidence : public Node {
public:
    Evidence(Own<Atom> atom, bool value, SrcLocation loc);

    const Atom& getAtom() const;
    const QualifiedName& getAtomName() const;
    const std::vector<Argument*> getArguments() const;
    bool getEvidenceValue() const;
    void print(std::ostream& os) const override;
    bool equal(const Node& other) const override;
    Evidence* cloning() const override;




private:
    Own<Atom> atom;
    bool value;
};

}