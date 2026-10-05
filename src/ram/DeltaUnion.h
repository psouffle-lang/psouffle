/************************************************************************
 *
 * @file DeltaUnion.h
 *
 ***********************************************************************/

#pragma once

#include "ram/Node.h"
#include "ram/Relation.h"
#include "ram/RelationStatement.h"
#include "souffle/utility/StreamUtil.h"
#include <memory>
#include <ostream>
#include <string>

namespace souffle::ram {

/**
 * @class DeltaUnion
 * @brief calculate new relation and real deltas from old relation and derivation deltas
 *
 * Rnew, delta_Rreal <= Rold, delta_Rder
 */
class DeltaUnion : public RelationStatement {
public:
    DeltaUnion(std::string rel,
        std::string oldRel, std::string newRel,
        std::string deltaDervInsertRel, std::string deltaDervDeleteRel,
        std::string deltaTupleInsertRel, std::string deltaTupleDeleteRel)
            : RelationStatement(NK_DeltaUnion, rel),
    oldRel(oldRel), newRel(newRel),
    deltaDervInsertRel(deltaDervInsertRel), deltaDervDeleteRel(deltaDervDeleteRel),
    deltaTupleInsertRel(deltaTupleInsertRel), deltaTupleDeleteRel(deltaTupleDeleteRel)
    {}

    DeltaUnion* cloning() const override {
        return new DeltaUnion(relation, oldRel, newRel,
            deltaDervInsertRel, deltaDervDeleteRel, deltaTupleInsertRel, deltaTupleDeleteRel);
    }

    static bool classof(const Node* n) {
        return n->getKind() == NK_DeltaUnion;
    }

    std::string getOldRel() const { return oldRel; }
    std::string getNewRel() const { return newRel; }
    std::string getDeltaDervInsertRel() const { return deltaDervInsertRel; }
    std::string getDeltaDervDeleteRel() const { return deltaDervDeleteRel; }
    std::string getDeltaTupleInsertRel() const { return deltaTupleInsertRel; }
    std::string getDeltaTupleDeleteRel() const { return deltaTupleDeleteRel; }
    // bool isOverdelete() const { return overdelete; }
protected:
    bool overdelete;
    void print(std::ostream& os, int tabpos) const override {
        os << times(" ", tabpos);
        os << "DELTA UNION " << relation << std::endl;
    };

    bool equal(const Node& node) const override {
        const auto& other = asAssert<DeltaUnion>(node);
        return RelationStatement::equal(other);
    }

    std::string oldRel;
    std::string newRel;
    std::string deltaDervInsertRel;
    std::string deltaDervDeleteRel;
    std::string deltaTupleInsertRel;
    std::string deltaTupleDeleteRel;
};

}  // namespace souffle::ram
