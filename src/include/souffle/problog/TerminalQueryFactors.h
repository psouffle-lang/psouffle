#pragma once

#include "souffle/problog/PrivateFactorRewrite.h"

#include <stdexcept>
#include <unordered_map>

namespace souffle::problog {

// Compatibility entry point for marginal-only output calculations. Online and
// joint-query APIs must retain the original query event expressions instead.
inline TerminalQueryFactorResult factorTerminalQueries(WorkingDerivationGraph& graph,
        WorkingSubgraphView& view, bool completeDerivations = false) {
    auto result = rewritePrivateFactors(graph, view, false, true, completeDerivations);
    auto& stats = result.terminal.stats;
    stats.analysisMs = result.stats.preparationMs + result.stats.terminalPlanMs;
    stats.mutationMs = result.stats.mutationMs;
    stats.totalMs = result.stats.totalMs;
    return std::move(result.terminal);
}

// An implicit rewrite can replace the owner. Resolve only the surviving input
// of an output calculation; Q itself must never be bound to this event.
inline void restoreTerminalQueryFactorParents(
        WorkingDerivationGraph& graph, TerminalQueryFactorResult& result) {
    for (auto& record : result.records) {
        if (auto current = graph.findNode(record.parent->getTuple())) record.parent = current;
    }
    for (auto& parent : result.promotedOutputRoots) {
        if (auto current = graph.findNode(parent->getTuple())) parent = current;
    }
}

// Call after conditional inference, before ordinary output/alias rendering.
// Parent probabilities may have been precomputed by another exact rewrite.
inline void evaluateTerminalQueryFactors(const TerminalQueryFactorResult& result,
        std::unordered_map<NodePtr, double>& probabilities) {
    for (const auto& record : result.records) {
        double parentProbability;
        const auto current = probabilities.find(record.parent);
        const auto precomputed = precomputedProbResult.find(record.parent);
        if (current != probabilities.end()) parentProbability = current->second;
        else if (precomputed != precomputedProbResult.end()) parentProbability = precomputed->second;
        else {
            const auto tuple = precomputedTupleProbResult.find(record.parent->getTuple().toString());
            if (tuple == precomputedTupleProbResult.end()) {
                throw std::runtime_error("Missing terminal-query parent probability for " +
                        record.query->getTuple().toString());
            }
            parentProbability = tuple->second;
        }
        probabilities[record.query] = record.factor * parentProbability;
    }
}

}  // namespace souffle::problog
