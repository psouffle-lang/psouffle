#ifndef FORWARDCOMPILATIONSUPPORT_H
#define FORWARDCOMPILATIONSUPPORT_H

#include "souffle/problog/DerivationGraph.h"
#include "souffle/problog/debug/Debugger.h"

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern Debugger& debugger;

void assertProbabilityInRange(double p, const std::string& ctx);

std::unordered_map<UntypedTuple, std::vector<EdgePtr>> buildDeletedOutEdges(
        const std::set<EdgePtr>& deletedEdges);

void collectImpactUnionWithDeletedEdges(
        const IncrementalDerivationGraphViewInterface& view,
        const std::vector<NodePtr>& sources,
        const std::unordered_map<UntypedTuple, std::vector<EdgePtr>>& deletedOutEdges,
        std::unordered_set<NodePtr>& outNodes,
        std::unordered_set<EdgePtr>& outEdges);

#endif
