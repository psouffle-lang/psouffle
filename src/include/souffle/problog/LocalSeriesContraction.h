#pragma once

#include "souffle/problog/AndInputRedundancy.h"

#include <cmath>
#include <chrono>
#include <limits>
#include <unordered_map>
#include <vector>

namespace souffle::problog {

struct LocalSeriesContractionStats {
    std::size_t nodesBefore = 0, edgesBefore = 0;
    std::size_t inputAssociationsBefore = 0, inputAssociationsAfter = 0;
    std::size_t candidates = 0, contractions = 0;
    std::size_t removedNodes = 0, removedEdges = 0, addedEdges = 0;
    std::size_t duplicateInputsRemoved = 0, ruleVariablesRemoved = 0;
    std::size_t supportOwners = 0, supportTokens = 0, unknownRandomEvents = 0;
    std::size_t recursiveNodes = 0, rejectedPrivateSupport = 0, rejectedOwnerUses = 0;
    std::size_t bodyOccurrencesAnalyzed = 0, rejectedWorkBudget = 0;
    double analysisMs = 0.0, mutationMs = 0.0, totalMs = 0.0;
};

struct LocalSeriesContractionResult {
    bool completeDerivations = false;
    LocalSeriesContractionStats stats;
    std::vector<NodePtr> removedNodes;
    // Only surviving compound edges are materialized. Their support records
    // identify the original factors, including factors contracted along chains.
    std::vector<EdgePtr> compoundEdges;
};

// Substitute a sole, positive use of a single-source intermediate event. Only
// private rule factors are multiplied; all external signed events are retained.
// The owner must be synchronized with the complete query/evidence view first.
inline LocalSeriesContractionResult contractLocalSeries(WorkingDerivationGraph& graph,
        WorkingSubgraphView& view, bool completeDerivations = false) {
    using Snapshot = detail::AndInputRedundancySnapshot;
    constexpr auto none = Snapshot::none;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    LocalSeriesContractionResult result;
    auto& stats = result.stats;
    stats.nodesBefore = view.getNodes().size();
    stats.edgesBefore = view.getEdges().size();
    if (!completeDerivations) return result;

    // Reuse the source/body CSR and recursive-node analysis used by AND input
    // elimination, without computing its Must sets or redundancy candidates.
    Snapshot snapshot;
    snapshot.begin(stats.nodesBefore, stats.edgesBefore, true);
    for (const auto& node : view.getNodes()) snapshot.addNode(node);
    snapshot.finishNodes();
    for (const auto& edge : view.getEdges()) snapshot.addEdge(edge);
    snapshot.finishSources();
    stats.inputAssociationsBefore = stats.inputAssociationsAfter = snapshot.bodies.size();
    if (!snapshot.complete) {
        stats.analysisMs = stats.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        return result;
    }
    result.completeDerivations = true;
    // Unlike AND redundancy, substitution permits signed external literals.
    // Arity and endpoint validity still apply to every retained definition.
    for (auto& edge : snapshot.edges) {
        if (edge.edge->getInputs().size() != edge.edge->getBodyNegations().size()) {
            result.completeDerivations = false;
            stats.analysisMs = stats.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            return result;
        }
        edge.safe = snapshot.safe[edge.head];
        for (auto i = edge.body.begin; i < edge.body.end; ++i) edge.safe = edge.safe && snapshot.safe[snapshot.bodies[i]];
    }
    snapshot.excludeRecursiveNodes();
    stats.recursiveNodes = snapshot.baseStats.recursiveNodes;

    // Provenance tokens need one global incidence calculation, rather than a
    // dependency traversal for each candidate. Unknown random provenance fails
    // closed: an empty token list does not certify independence.
    std::unordered_map<SupportToken, unsigned char> owners;
    owners.reserve(stats.nodesBefore + stats.edgesBefore);
    bool valid = true;
    auto addSupports = [&](double probability, const std::vector<SupportToken>& tokens) {
        if (!std::isfinite(probability) || probability < 0 || probability > 1) valid = false;
        if (probability > 0 && probability < 1 && tokens.empty()) ++stats.unknownRandomEvents;
        if (!tokens.empty()) ++stats.supportOwners;
        for (const auto token : tokens) {
            auto& count = owners[token];
            if (count < 2) ++count;
        }
    };
    for (const auto& node : snapshot.nodes) {
        if (!graph.getNodes().count(node)) valid = false;
        addSupports(node->isFact ? node->getProbability() : 1.0, node->getProbabilisticSupportTokens());
    }
    for (const auto& edge : snapshot.edges) {
        if (!graph.getEdges().count(edge.edge)) valid = false;
        addSupports(edge.edge->getProbability(), edge.edge->getProbabilisticSupportTokens());
    }
    stats.supportTokens = owners.size();
    if (!valid || stats.unknownRandomEvents) {
        stats.analysisMs = stats.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        return result;
    }

    struct VirtualEdge {
        EdgePtr original;
        std::size_t head;
        Snapshot::Range body;
        double probability;
        bool safe, active = true;
        std::vector<bool> signs;
        bool privateFactors = false;
        std::size_t leftFactors = Snapshot::none, rightFactors = Snapshot::none;
        bool negative(std::size_t position) const {
            return original ? original->getBodyNegations()[position] : signs[position];
        }
    };
    std::vector<VirtualEdge> edges;
    edges.reserve(snapshot.edges.size() + snapshot.nodes.size());
    for (const auto& edge : snapshot.edges) {
        edges.push_back({edge.edge, edge.head, edge.body, edge.edge->getProbability(), edge.safe});
        auto& record = edges.back();
        record.privateFactors = std::all_of(edge.edge->getProbabilisticSupportTokens().begin(),
                edge.edge->getProbabilisticSupportTokens().end(), [&](SupportToken token) { return owners.at(token) == 1; });
    }
    const auto initialEdges = edges.size();
    std::vector<std::size_t> source(snapshot.nodes.size(), none), outgoing(snapshot.nodes.size(), 0);
    std::vector<std::size_t> forwardOffsets(snapshot.nodes.size() + 1, 0);
    for (std::size_t node = 0; node < snapshot.nodes.size(); ++node) {
        if (snapshot.incomingOffsets[node + 1] - snapshot.incomingOffsets[node] == 1) {
            source[node] = snapshot.incomingEdges[snapshot.incomingOffsets[node]];
        }
    }
    for (const auto& edge : edges) {
        for (auto position = edge.body.begin; position < edge.body.end; ++position) {
            ++forwardOffsets[snapshot.bodies[position] + 1];
        }
    }
    for (std::size_t i = 1; i < forwardOffsets.size(); ++i) forwardOffsets[i] += forwardOffsets[i - 1];
    auto cursor = forwardOffsets;
    std::vector<std::size_t> forward(snapshot.bodies.size());
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto& edge = edges[index];
        for (auto position = edge.body.begin; position < edge.body.end; ++position) {
            const auto node = snapshot.bodies[position];
            forward[cursor[node]++] = index;
            ++outgoing[node];
        }
    }
    // New edge occurrences use a flat linked pool. Original outgoing CSR stays
    // immutable, and only nodes whose degree changed need reconsideration.
    struct Occurrence { std::size_t edge, next; };
    std::vector<Occurrence> newOccurrences;
    std::vector<std::size_t> newHeads(snapshot.nodes.size(), none);
    std::vector<unsigned char> live(snapshot.nodes.size(), 1), queued(snapshot.nodes.size(), 0);
    std::vector<unsigned char> seenCandidate(snapshot.nodes.size(), 0), ownerClosed(snapshot.nodes.size(), 0);
    std::vector<std::size_t> queue, touched, marks(snapshot.nodes.size(), 0);
    std::size_t markEpoch = 0;
    auto eligible = [&](std::size_t node) {
        const auto& value = snapshot.nodes[node];
        return live[node] && source[node] != none && outgoing[node] == 1 && snapshot.safe[node] &&
                !value->isFact && !value->isOriginalFactNode() && !value->isShadow &&
                !value->needOutput && !value->isQuery && !value->hasEvidence();
    };
    auto enqueue = [&](std::size_t node) {
        if (!queued[node] && eligible(node)) { queued[node] = 1; queue.push_back(node); }
    };
    for (std::size_t node = 0; node < snapshot.nodes.size(); ++node) enqueue(node);
    std::sort(queue.begin(), queue.end(), [&](std::size_t a, std::size_t b) {
        if (snapshot.nodes[a]->getId() != snapshot.nodes[b]->getId()) {
            return snapshot.nodes[a]->getId() < snapshot.nodes[b]->getId();
        }
        return std::less<const Node*>{}(snapshot.nodes[a].get(), snapshot.nodes[b].get());
    });
    auto soleConsumer = [&](std::size_t node) {
        for (auto occurrence = newHeads[node]; occurrence != none; occurrence = newOccurrences[occurrence].next) {
            const auto edge = newOccurrences[occurrence].edge;
            if (edges[edge].active) return edge;
        }
        for (auto position = forwardOffsets[node]; position < forwardOffsets[node + 1]; ++position) {
            if (edges[forward[position]].active) return forward[position];
        }
        return none;
    };
    auto closedOwner = [&](std::size_t node) {
        if (ownerClosed[node]) return ownerClosed[node] == 1;
        ownerClosed[node] = 1;
        const auto check = [&](const std::vector<EdgePtr>& incident) {
            for (const auto& edge : incident) {
                if (graph.getEdges().count(edge) && !view.getEdges().count(edge)) return false;
            }
            return true;
        };
        if (!check(snapshot.nodes[node]->getIncomingEdges()) || !check(snapshot.nodes[node]->getOutgoingEdges())) {
            ownerClosed[node] = 2;
        }
        return ownerClosed[node] == 1;
    };
    std::unordered_map<std::size_t, unsigned char> longBodySigns;
    // A chain accumulating external inputs can otherwise repeatedly copy an
    // ever-growing body. Bound that work relative to the existing graph; a
    // skipped contraction leaves its exact original event expression intact.
    const auto bodyBudget = stats.inputAssociationsBefore > (none - 4096) / 4
            ? none : stats.inputAssociationsBefore * 4 + 4096;
    for (std::size_t current = 0; current < queue.size(); ++current) {
        const auto middle = queue[current];
        queued[middle] = 0;
        if (!eligible(middle)) continue;
        if (!seenCandidate[middle]) { seenCandidate[middle] = 1; ++stats.candidates; }
        const auto into = source[middle], out = soleConsumer(middle);
        if (out == none || into == out || !edges[into].active || !edges[into].safe || !edges[out].safe) continue;
        const auto& first = edges[into];
        const auto& second = edges[out];
        // Charge before searching the consumer body: many private middles can
        // share a huge AND edge, including after the work budget is exhausted.
        const auto bodyWork = first.body.end - first.body.begin + 2 * (second.body.end - second.body.begin);
        if (bodyWork > bodyBudget - stats.bodyOccurrencesAnalyzed) {
            ++stats.rejectedWorkBudget;
            continue;
        }
        stats.bodyOccurrencesAnalyzed += bodyWork;
        std::size_t middlePosition = none;
        for (auto position = second.body.begin; position < second.body.end; ++position) {
            if (snapshot.bodies[position] == middle) middlePosition = position - second.body.begin;
        }
        if (middlePosition == none || second.negative(middlePosition)) continue;
        if (!closedOwner(middle)) { ++stats.rejectedOwnerUses; continue; }
        // Initially private token sets are disjoint globally. Each contraction
        // consumes both edges, so active private sets remain disjoint without
        // repeatedly copying or intersecting growing chain support unions.
        if (!first.privateFactors || !second.privateFactors) {
            ++stats.rejectedPrivateSupport;
            continue;
        }
        const double probability = first.probability * second.probability;
        if (probability == 0 && first.probability > 0 && second.probability > 0) continue;
        const auto head = second.head;
        const auto bodyBegin = snapshot.bodies.size();
        std::vector<bool> signs;
        const auto maximumBody = first.body.end - first.body.begin + second.body.end - second.body.begin - 1;
        signs.reserve(maximumBody);
        if (maximumBody > 8) longBodySigns.clear();
        auto append = [&](std::size_t node, bool negative) {
            bool duplicate = false;
            if (maximumBody <= 8) {
                for (auto i = bodyBegin; i < snapshot.bodies.size(); ++i) {
                    if (snapshot.bodies[i] == node && signs[i - bodyBegin] == negative) { duplicate = true; break; }
                }
            } else {
                auto& seen = longBodySigns[node];
                const unsigned char sign = negative ? 2 : 1;
                duplicate = (seen & sign) != 0;
                seen |= sign;
            }
            if (duplicate) ++stats.duplicateInputsRemoved;
            else { snapshot.bodies.push_back(node); signs.push_back(negative); }
        };
        for (auto i = second.body.begin; i < second.body.end; ++i) {
            if (i - second.body.begin != middlePosition) append(snapshot.bodies[i], second.negative(i - second.body.begin));
            else for (auto j = first.body.begin; j < first.body.end; ++j) {
                append(snapshot.bodies[j], first.negative(j - first.body.begin));
            }
        }
        const auto mergedBody = Snapshot::Range{bodyBegin, snapshot.bodies.size()};
        ++stats.contractions;
        const auto random = [](double p) { return p > 0 && p < 1 ? 1U : 0U; };
        stats.ruleVariablesRemoved += random(first.probability) + random(second.probability) - random(probability);
        stats.inputAssociationsAfter -= first.body.end - first.body.begin + second.body.end - second.body.begin;
        stats.inputAssociationsAfter += mergedBody.end - mergedBody.begin;
        // Defer queue updates until both old bodies and the new body have been
        // accounted for, avoiding transient-degree rescans of large fanouts.
        touched.clear();
        Snapshot::nextEpoch(markEpoch, marks);
        auto touch = [&](std::size_t node) {
            if (marks[node] != markEpoch) { marks[node] = markEpoch; touched.push_back(node); }
        };
        for (const auto index : {into, out}) {
            const auto body = edges[index].body;
            for (auto i = body.begin; i < body.end; ++i) { --outgoing[snapshot.bodies[i]]; touch(snapshot.bodies[i]); }
            edges[index].active = false;
        }
        const auto replacement = edges.size();
        edges.push_back({nullptr, head, mergedBody, probability, true, true, std::move(signs), true, into, out});
        for (auto i = mergedBody.begin; i < mergedBody.end; ++i) {
            const auto node = snapshot.bodies[i];
            ++outgoing[node];
            newOccurrences.push_back({replacement, newHeads[node]});
            newHeads[node] = newOccurrences.size() - 1;
            touch(node);
        }
        live[middle] = 0;
        source[middle] = none;
        result.removedNodes.push_back(snapshot.nodes[middle]);
        if (source[head] != none) source[head] = replacement;
        touch(head);
        for (const auto node : touched) enqueue(node);
    }
    stats.analysisMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    stats.totalMs = stats.analysisMs;
    if (!stats.contractions) return result;

    const auto mutationStart = Clock::now();
    std::vector<EdgePtr> retiredEdges;
    std::vector<std::size_t> factorStack;
    for (std::size_t index = 0; index < initialEdges; ++index) {
        if (!edges[index].active) retiredEdges.push_back(edges[index].original);
    }
    for (auto index = initialEdges; index < edges.size(); ++index) {
        const auto& edge = edges[index];
        if (!edge.active) continue;
        std::vector<NodePtr> body;
        body.reserve(edge.body.end - edge.body.begin);
        for (auto i = edge.body.begin; i < edge.body.end; ++i) body.push_back(snapshot.nodes[snapshot.bodies[i]]);
        auto compound = body.empty() ? graph.createHyperedge(body, snapshot.nodes[edge.head])
                                    : graph.createHyperedge(body, snapshot.nodes[edge.head], nullptr, edge.signs);
        compound->setProbability(edge.probability);
        std::vector<SupportToken> supports;
        factorStack.push_back(index);
        while (!factorStack.empty()) {
            const auto factor = factorStack.back();
            factorStack.pop_back();
            const auto& record = edges[factor];
            if (record.original) {
                const auto& tokens = record.original->getProbabilisticSupportTokens();
                supports.insert(supports.end(), tokens.begin(), tokens.end());
            } else {
                factorStack.push_back(record.leftFactors);
                factorStack.push_back(record.rightFactors);
            }
        }
        compound->setProbabilisticSupportTokens(std::move(supports));
        view.mutableEdges().insert(compound);
        result.compoundEdges.push_back(std::move(compound));
    }
    const auto retired = graph.retireRewriteObjects(view, result.removedNodes, retiredEdges);
    stats.removedNodes = retired.removedNodes;
    stats.removedEdges = retired.removedEdges;
    stats.addedEdges = result.compoundEdges.size();
    stats.mutationMs = std::chrono::duration<double, std::milli>(Clock::now() - mutationStart).count();
    stats.totalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return result;
}

}  // namespace souffle::problog
