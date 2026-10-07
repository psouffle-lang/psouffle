#pragma once

#include "souffle/problog/AndInputRedundancy.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace souffle::problog {

struct TerminalQueryFactorRecord {
    // This is an output calculation, not an event-alias binding. The original
    // query's tuple and flags remain available after its graph node is retired.
    NodePtr query;
    NodePtr parent;
    double factor = 1.0;
    // Local retired-edge provenance. A flattened chain's other factors each
    // retain their own record; copying their full token union would be quadratic.
    std::vector<SupportToken> supportTokens;
};

struct TerminalQueryFactorStats {
    std::size_t nodes = 0;
    std::size_t edges = 0;
    std::size_t candidateQueries = 0;
    std::size_t factoredQueries = 0;
    std::size_t factoredOutputQueries = 0;
    std::size_t hiddenChainSteps = 0;
    std::size_t removedNodes = 0;
    std::size_t removedEdges = 0;
    std::size_t promotedRoots = 0;
    std::size_t skippedSupportOverlap = 0;
    std::size_t skippedMissingSupport = 0;
    std::size_t skippedRecursive = 0;
    std::size_t skippedOwnerHistory = 0;
    double analysisMs = 0.0;
    double mutationMs = 0.0;
    double totalMs = 0.0;
};

struct TerminalQueryFactorResult {
    // Records are flattened to a surviving parent. Multiple output marginals
    // may share that parent; their factors must never become independent facts.
    std::vector<TerminalQueryFactorRecord> records;
    // Include intermediate promoted nodes too: their calculation records are
    // hidden, while every original output/alias name remains visible.
    std::vector<NodePtr> promotedOutputRoots;
    TerminalQueryFactorStats stats;
};

// A terminal marginal Q = R AND A can be evaluated after core inference as
// P(Q | E) = P(R) P(A | E), provided R occurs nowhere else in the complete active
// graph. The globally private support certificate covers A, every evidence
// dependency, and other outputs without repeated ancestor traversals.
// Online and joint-query APIs must not invoke this output-only optimization.
inline TerminalQueryFactorResult factorTerminalQueries(
        WorkingDerivationGraph& graph, WorkingSubgraphView& view,
        bool completeDerivations = false) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto elapsedMs = [](Clock::time_point from) {
        return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };
    TerminalQueryFactorResult result;
    result.stats.nodes = view.getNodes().size();
    result.stats.edges = view.getEdges().size();
    auto finishAnalysis = [&]() {
        result.stats.analysisMs = elapsedMs(start);
        result.stats.totalMs = result.stats.analysisMs;
    };
    if (!completeDerivations) {
        finishAnalysis();
        return result;
    }

    // Reuse the existing dense source/body index and iterative SCC analysis;
    // no Must_1 computation is needed for this pass.
    detail::AndInputRedundancySnapshot snapshot;
    snapshot.begin(view.getNodes().size(), view.getEdges().size(), true, true,
            0, view.getEdges().size());
    std::unordered_map<SupportToken, unsigned char> tokenOwners;
    tokenOwners.reserve(view.getNodes().size() + view.getEdges().size());
    bool completeSupportMetadata = true;
    auto countSupport = [&](const std::vector<SupportToken>& tokens) {
        for (const auto token : tokens) {
            auto& count = tokenOwners[token];
            if (count < 2) ++count;
        }
    };
    for (const auto& node : view.getNodes()) {
        if (!node || !graph.getNodes().count(node) || !std::isfinite(node->getProbability()) ||
                node->getProbability() < 0 || node->getProbability() > 1) {
            finishAnalysis();
            return result;
        }
        snapshot.addNode(node);
        countSupport(node->getProbabilisticSupportTokens());
        if (node->isFact && node->getProbability() > 0 && node->getProbability() < 1 &&
                node->getProbabilisticSupportTokens().empty()) completeSupportMetadata = false;
    }
    snapshot.finishNodes();
    for (const auto& edge : view.getEdges()) {
        if (!edge || !graph.getEdges().count(edge) ||
                edge->getInputs().size() != edge->getBodyNegations().size() ||
                !std::isfinite(edge->getProbability()) ||
                edge->getProbability() < 0 || edge->getProbability() > 1) {
            finishAnalysis();
            return result;
        }
        snapshot.addEdge(edge);
        countSupport(edge->getProbabilisticSupportTokens());
        if (edge->getProbability() > 0 && edge->getProbability() < 1 &&
                edge->getProbabilisticSupportTokens().empty()) completeSupportMetadata = false;
    }
    snapshot.finishSources();
    if (!snapshot.complete) {
        finishAnalysis();
        return result;
    }
    snapshot.excludeRecursiveNodes();

    std::vector<unsigned char> retired(snapshot.nodes.size(), 0), queued(snapshot.nodes.size(), 0);
    auto remainingConsumers = snapshot.outgoingOccurrences;
    std::vector<std::size_t> queue;
    queue.reserve(snapshot.nodes.size());
    auto enqueue = [&](std::size_t node) {
        if (!retired[node] && !queued[node] && snapshot.nodes[node]->needOutput &&
                remainingConsumers[node] == 0) {
            queue.push_back(node);
            queued[node] = 1;
        }
    };
    for (std::size_t node = 0; node < snapshot.nodes.size(); ++node) enqueue(node);
    std::vector<NodePtr> retiredNodes;
    std::vector<EdgePtr> retiredEdges;
    std::unordered_set<EdgePtr> retiredEdgeSet;
    retiredNodes.reserve(queue.size());
    retiredEdges.reserve(queue.size());
    retiredEdgeSet.reserve(queue.size());
    // Promotions are committed only after the complete retirement plan is
    // proven, so unsupported candidates leave their graph and flags untouched.
    std::vector<unsigned char> promoted(snapshot.nodes.size(), 0);
    for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
        const auto nodeIndex = queue[cursor];
        const auto& query = snapshot.nodes[nodeIndex];
        ++result.stats.candidateQueries;
        if (snapshot.fact[nodeIndex] || query->isShadow || query->hasEvidence() ||
                !query->getProbabilisticSupportTokens().empty() ||
                snapshot.incomingOffsets[nodeIndex + 1] - snapshot.incomingOffsets[nodeIndex] != 1) continue;
        const auto edgeIndex = snapshot.incomingEdges[snapshot.incomingOffsets[nodeIndex]];
        const auto& source = snapshot.edges[edgeIndex];
        if (source.body.end - source.body.begin != 1 ||
                source.edge->getBodyNegations()[0]) continue;
        const auto parentIndex = snapshot.bodies[source.body.begin];
        const auto& parent = snapshot.nodes[parentIndex];
        if (parent->isShadow || parentIndex == nodeIndex || retired[parentIndex]) continue;
        if (!snapshot.safe[nodeIndex] || !snapshot.safe[parentIndex]) {
            ++result.stats.skippedRecursive;
            continue;
        }
        const auto& support = source.edge->getProbabilisticSupportTokens();
        const auto factor = source.edge->getProbability();
        if (!std::isfinite(factor) || factor < 0 || factor > 1) continue;
        if (factor > 0 && factor < 1 && (support.empty() || !completeSupportMetadata)) {
            ++result.stats.skippedMissingSupport;
            continue;
        }
        bool privateSupport = true;
        for (const auto token : support) {
            if (tokenOwners.at(token) != 1) { privateSupport = false; break; }
        }
        if (!privateSupport) {
            ++result.stats.skippedSupportOverlap;
            continue;
        }
        // Constants cannot carry uncertain legacy support in this first pass.
        if ((factor == 0 || factor == 1) && !support.empty()) continue;
        bool completeOwnerIncidents = true;
        for (const auto& edge : query->getIncomingEdges()) {
            if (edge && graph.getEdges().count(edge) && edge != source.edge) {
                completeOwnerIncidents = false;
                break;
            }
        }
        for (const auto& edge : query->getOutgoingEdges()) {
            if (edge && graph.getEdges().count(edge) && !retiredEdgeSet.count(edge)) {
                completeOwnerIncidents = false;
                break;
            }
        }
        if (!completeOwnerIncidents) {
            ++result.stats.skippedOwnerHistory;
            continue;
        }
        retired[nodeIndex] = 1;
        retiredNodes.push_back(query);
        retiredEdges.push_back(source.edge);
        retiredEdgeSet.insert(source.edge);
        result.records.push_back({query, parent, factor, support});
        if (query->needOutput) ++result.stats.factoredOutputQueries;
        else ++result.stats.hiddenChainSteps;
        --remainingConsumers[parentIndex];
        if (!parent->needOutput && !promoted[parentIndex]) {
            promoted[parentIndex] = 1;
            result.promotedOutputRoots.push_back(parent);
        }
        // A newly required hidden parent can itself become a terminal marginal.
        if (!retired[parentIndex] && !queued[parentIndex] &&
                (parent->needOutput || promoted[parentIndex]) && remainingConsumers[parentIndex] == 0) {
            queue.push_back(parentIndex);
            queued[parentIndex] = 1;
        }
    }

    // Parents are retired after their children, so one reverse scan resolves
    // every chain without repeated graph traversal or quadratic chain walks.
    std::unordered_map<NodePtr, std::size_t> recordIndex;
    recordIndex.reserve(result.records.size());
    for (std::size_t i = 0; i < result.records.size(); ++i) recordIndex.emplace(result.records[i].query, i);
    for (std::size_t i = result.records.size(); i-- > 0;) {
        auto& record = result.records[i];
        const auto parentRecord = recordIndex.find(record.parent);
        if (parentRecord != recordIndex.end()) {
            const auto& resolved = result.records[parentRecord->second];
            record.parent = resolved.parent;
            record.factor *= resolved.factor;
        }
    }
    result.stats.analysisMs = elapsedMs(start);
    if (retiredNodes.empty()) {
        result.stats.totalMs = result.stats.analysisMs;
        return result;
    }
    const auto mutationStart = Clock::now();
    graph.retireRewriteObjects(view, retiredNodes, retiredEdges);
    for (const auto& parent : result.promotedOutputRoots) {
        // Preserve isQuery: a hidden inference root is not a new user query.
        if (view.getNodes().count(parent)) parent->needOutput = true;
    }
    result.stats.factoredQueries = result.records.size();
    result.stats.removedNodes = retiredNodes.size();
    result.stats.removedEdges = retiredEdges.size();
    result.stats.promotedRoots = result.promotedOutputRoots.size();
    result.stats.mutationMs = elapsedMs(mutationStart);
    result.stats.totalMs = elapsedMs(start);
    return result;
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
