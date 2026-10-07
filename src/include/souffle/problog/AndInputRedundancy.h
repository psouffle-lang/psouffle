#pragma once

#include "souffle/problog/DerivationGraph.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <ostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace souffle::problog {

struct AndInputRedundancyWitness {
    NodePtr premise;
    NodePtr provider;
    // Empty for a direct input; otherwise every active derivation of provider.
    std::vector<EdgePtr> sourceEdges;
};

struct AndInputRedundancyProof {
    EdgePtr edge;
    std::size_t inputIndex = 0;
    NodePtr redundant;
    EdgePtr definition;
    std::vector<AndInputRedundancyWitness> witnesses;
};

struct AndInputRedundancyStats {
    std::size_t nodes = 0;
    std::size_t edges = 0;
    std::size_t inputAssociations = 0;
    std::size_t eligibleDefinitions = 0;
    std::size_t provenInputAssociations = 0;
    std::size_t affectedEdges = 0;
    std::size_t distinctRedundantNodes = 0;
    std::size_t recursiveNodes = 0;
    double analysisMs = 0.0;
};

struct AndInputRedundancyReport {
    bool completeDerivations = false;
    AndInputRedundancyStats stats;
    // Each proof permits ONE removal from this snapshot. Proofs are not a batch plan.
    std::vector<AndInputRedundancyProof> proofs;
};

// Optional, atomic cleanup proposal for a freshly query/evidence-pruned view.
// Applying it is the caller's responsibility; the core pass only edits bodies.
struct AndInputRedundancyCleanupPlan {
    std::vector<NodePtr> nodes;
    std::vector<EdgePtr> edges;
    bool requiresFullPrune = false;
    // Exact shape of the retained view, computed from the existing indexes.
    bool hasSummary = false;
    std::size_t finalInputAssociations = 0;
    std::size_t removedDisjunctionNodes = 0;
    std::size_t maxInDegree = 0;
    std::size_t maxOutDegree = 0;
    std::size_t maxHyperedgeInputs = 0;
};

namespace detail {

struct AndInputRedundancySnapshot {
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    struct Range {
        std::size_t begin = 0;
        std::size_t end = 0;
    };
    struct EdgeInfo {
        EdgePtr edge;
        std::size_t head;
        Range body;
        bool safe = false;
    };
    struct Candidate {
        std::size_t edge;
        std::size_t inputIndex;
        std::size_t node;
    };

    bool complete = false;
    AndInputRedundancyStats baseStats;
    std::vector<NodePtr> nodes;
    // Ordinary graph IDs are dense enough for direct lookup. Sparse IDs or
    // colliding IDs from different owners fall back to actual pointer identity.
    std::vector<std::size_t> idIndex;
    std::unordered_map<const Node*, std::size_t> pointerIndex;
    std::vector<EdgeInfo> edges;
    std::vector<std::size_t> bodies, incomingOffsets, incomingEdges, definition, targets;
    std::vector<unsigned char> safe, fact;
    // Only requested providers acquire a Must_1 entry. All entries share one
    // flat storage array; a deletion shortens its own segment without moving
    // subsequent entries.
    std::vector<std::size_t> mustSlots, mustBodies, bodyMarks, coverageMarks, providers;
    std::vector<Range> mustEntries;
    std::vector<std::size_t> mustScratch;
    std::vector<std::size_t> outgoingOccurrences, cleanupSeeds;
    std::size_t bodyEpoch = 0, coverageEpoch = 0;

    std::size_t indexOf(const NodePtr& node) const {
        if (!node) return none;
        if (!idIndex.empty()) {
            const auto id = node->getId();
            if (id >= idIndex.size()) return none;
            const auto index = idIndex[id];
            return index != none && nodes[index] == node ? index : none;
        }
        const auto it = pointerIndex.find(node.get());
        return it == pointerIndex.end() ? none : it->second;
    }

    static std::size_t nextEpoch(std::size_t& epoch, std::vector<std::size_t>& marks) {
        if (++epoch == 0) {
            std::fill(marks.begin(), marks.end(), 0);
            ++epoch;
        }
        return epoch;
    }

    void initialize(const DerivationGraphViewInterface& view, bool completeDerivations,
            bool collectCycleStats, bool trackCleanup = false) {
        baseStats.nodes = view.getNodes().size();
        baseStats.edges = view.getEdges().size();
        for (const auto& edge : view.getEdges()) {
            if (edge) baseStats.inputAssociations += edge->getInputs().size();
        }
        if (!completeDerivations) return;
        nodes.assign(view.getNodes().begin(), view.getNodes().end());
        safe.resize(nodes.size());
        fact.resize(nodes.size());
        std::size_t maxId = 0;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto& node = nodes[i];
            if (!node) return;
            maxId = std::max(maxId, node->getId());
            safe[i] = !node->isShadow;
            fact[i] = node->isFact || node->isOriginalFactNode();
        }
        if (!nodes.empty() && maxId != none && maxId < nodes.size() * 4 + 4096) {
            idIndex.assign(maxId + 1, none);
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                auto& slot = idIndex[nodes[i]->getId()];
                if (slot != none) {
                    idIndex.clear();
                    break;
                }
                slot = i;
            }
        }
        if (idIndex.empty()) {
            pointerIndex.reserve(nodes.size());
            for (std::size_t i = 0; i < nodes.size(); ++i) pointerIndex.emplace(nodes[i].get(), i);
        }

        incomingOffsets.assign(nodes.size() + 1, 0);
        edges.reserve(baseStats.edges);
        bodies.reserve(baseStats.inputAssociations);
        // Validate EVERY endpoint before any early return or eligibility shortcut.
        for (const auto& edge : view.getEdges()) {
            if (!edge) return;
            const auto head = indexOf(edge->getOutput());
            if (head == none) return;
            const auto& inputs = edge->getInputs();
            const auto& negations = edge->getBodyNegations();
            bool supported = safe[head] && inputs.size() == negations.size() &&
                    std::none_of(negations.begin(), negations.end(), [](bool neg) { return neg; });
            const auto begin = bodies.size();
            for (const auto& input : inputs) {
                const auto index = indexOf(input);
                if (index == none) return;
                bodies.push_back(index);
                supported = supported && safe[index];
            }
            edges.push_back({edge, head, {begin, bodies.size()}, supported});
            ++incomingOffsets[head + 1];
        }
        for (std::size_t i = 1; i < incomingOffsets.size(); ++i) incomingOffsets[i] += incomingOffsets[i - 1];
        auto cursor = incomingOffsets;
        incomingEdges.resize(edges.size());
        for (std::size_t i = 0; i < edges.size(); ++i) incomingEdges[cursor[edges[i].head]++] = i;
        complete = true;

        // Preselect structural definitions and targets before SCC/Must_1 work.
        // Bodies only shrink, so mutation cannot add a definition or target to
        // this pool: unsupported edges are never edited and sources stay fixed.
        definition.assign(nodes.size(), none);
        for (std::size_t node = 0; node < nodes.size(); ++node) {
            if (!safe[node] || fact[node] || incomingOffsets[node + 1] - incomingOffsets[node] != 1) continue;
            const auto edgeId = incomingEdges[incomingOffsets[node]];
            const auto& edge = edges[edgeId];
            if (edge.safe && edge.body.begin != edge.body.end && edge.edge->isDeterministic() &&
                    edge.edge->getProbabilisticSupportTokens().empty()) definition[node] = edgeId;
        }
        auto hasCandidate = [&](const EdgeInfo& edge) {
            for (auto i = edge.body.begin; i < edge.body.end; ++i) {
                if (definition[bodies[i]] != none) return true;
            }
            return false;
        };
        for (std::size_t i = 0; i < edges.size(); ++i) {
            const auto& edge = edges[i];
            if (edge.safe && edge.body.end - edge.body.begin >= 2 && hasCandidate(edge)) targets.push_back(i);
        }
        // Mutation needs no SCC if there can be no deletion. Read-only reports
        // still compute the complete recursive-node statistic.
        if (targets.empty() && !collectCycleStats) return;

        // Forward CSR over ALL signed body-to-head arcs. The existing incoming
        // source/body index already represents the reverse adjacency exactly.
        // Reuse DFS storage across roots instead of allocating per-node vectors.
        std::vector<std::size_t> nextOffsets(nodes.size() + 1, 0);
        std::vector<std::size_t> incomingOccurrences(nodes.size(), 0);
        std::vector<unsigned char> selfLoop(nodes.size(), 0);
        for (const auto& edge : edges) {
            incomingOccurrences[edge.head] += edge.body.end - edge.body.begin;
            for (auto i = edge.body.begin; i < edge.body.end; ++i) {
                ++nextOffsets[bodies[i] + 1];
                if (bodies[i] == edge.head) selfLoop[edge.head] = 1;
            }
        }
        // Reuse the outgoing degrees already counted for SCC construction.
        if (trackCleanup) outgoingOccurrences.assign(nextOffsets.begin() + 1, nextOffsets.end());
        for (std::size_t i = 1; i < nextOffsets.size(); ++i) {
            nextOffsets[i] += nextOffsets[i - 1];
        }
        std::vector<std::size_t> next(bodies.size());
        cursor = nextOffsets;
        for (const auto& edge : edges) {
            for (auto i = edge.body.begin; i < edge.body.end; ++i) next[cursor[bodies[i]]++] = edge.head;
        }
        std::vector<unsigned char> visited(nodes.size(), 0);
        std::vector<std::size_t> order, stack, component;
        std::vector<std::pair<std::size_t, std::size_t>> dfs;
        order.reserve(nodes.size());
        // Peel acyclic sources using the forward CSR, counting every signed
        // occurrence (including duplicates). A cycle cannot lose its internal
        // predecessors. Acyclic descendants of cycles may remain, so only the
        // residual graph needs the exact SCC analysis below.
        for (std::size_t node = 0; node < nodes.size(); ++node) {
            if (incomingOccurrences[node] == 0) {
                visited[node] = 1;
                order.push_back(node);
            }
        }
        for (std::size_t current = 0; current < order.size(); ++current) {
            const auto node = order[current];
            for (auto i = nextOffsets[node]; i < nextOffsets[node + 1]; ++i) {
                const auto child = next[i];
                if (--incomingOccurrences[child] == 0) {
                    visited[child] = 1;
                    order.push_back(child);
                }
            }
        }
        const bool hasResidual = order.size() != nodes.size();
        order.clear();
        for (std::size_t root = 0; hasResidual && root < nodes.size(); ++root) {
            if (visited[root]) continue;
            visited[root] = 1;
            dfs.emplace_back(root, nextOffsets[root]);
            while (!dfs.empty()) {
                auto& frame = dfs.back();
                if (frame.second == nextOffsets[frame.first + 1]) {
                    order.push_back(frame.first);
                    dfs.pop_back();
                } else {
                    const auto child = next[frame.second++];
                    if (!visited[child]) {
                        visited[child] = 1;
                        dfs.emplace_back(child, nextOffsets[child]);
                    }
                }
            }
        }
        if (hasResidual) {
            for (std::size_t node = 0; node < nodes.size(); ++node) {
                visited[node] = incomingOccurrences[node] == 0;
            }
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            if (visited[*it]) continue;
            component.clear();
            stack.push_back(*it);
            visited[*it] = 1;
            while (!stack.empty()) {
                const auto node = stack.back();
                stack.pop_back();
                component.push_back(node);
                for (auto i = incomingOffsets[node]; i < incomingOffsets[node + 1]; ++i) {
                    const auto body = edges[incomingEdges[i]].body;
                    for (auto j = body.begin; j < body.end; ++j) {
                        const auto parent = bodies[j];
                        if (!visited[parent]) {
                            visited[parent] = 1;
                            stack.push_back(parent);
                        }
                    }
                }
            }
            if (component.size() > 1 || selfLoop[*it]) {
                for (auto node : component) safe[node] = 0;
                baseStats.recursiveNodes += component.size();
            }
        }
        for (auto& edge : edges) {
            edge.safe = edge.safe && safe[edge.head];
            for (auto i = edge.body.begin; edge.safe && i < edge.body.end; ++i) edge.safe = safe[bodies[i]];
        }
        for (std::size_t node = 0; node < nodes.size(); ++node) {
            if (definition[node] != none && !edges[definition[node]].safe) definition[node] = none;
            if (definition[node] != none) ++baseStats.eligibleDefinitions;
        }
        targets.erase(std::remove_if(targets.begin(), targets.end(),
                              [&](auto edge) { return !edges[edge].safe || !hasCandidate(edges[edge]); }),
                targets.end());
        std::sort(targets.begin(), targets.end(), [&](auto a, auto b) {
            const auto left = edges[a].edge->getId(), right = edges[b].edge->getId();
            return left != right ? left < right : a < b;
        });
        if (targets.empty()) return;
        mustSlots.assign(nodes.size(), none);
        bodyMarks.assign(nodes.size(), 0);
        coverageMarks.assign(nodes.size(), 0);
    }

    Range mustFor(std::size_t node) {
        if (!safe[node] || fact[node] || incomingOffsets[node] == incomingOffsets[node + 1]) return {};
        if (mustSlots[node] != none) return mustEntries[mustSlots[node]];
        const auto cacheBegin = mustBodies.size();
        auto remember = [&]() {
            const Range range{cacheBegin, mustBodies.size()};
            mustSlots[node] = mustEntries.size();
            mustEntries.push_back(range);
            return range;
        };
        auto shortest = none;
        // Check all sources before deriving anything, and start with the
        // shortest CURRENT body to keep subsequent intersections small.
        for (auto i = incomingOffsets[node]; i < incomingOffsets[node + 1]; ++i) {
            const auto source = incomingEdges[i];
            const auto& edge = edges[source];
            if (!edge.safe || edge.body.begin == edge.body.end) return remember();
            if (shortest == none || edge.body.end - edge.body.begin <
                                           edges[shortest].body.end - edges[shortest].body.begin) shortest = source;
        }
        mustScratch.clear();
        const auto firstEpoch = nextEpoch(bodyEpoch, bodyMarks);
        const auto first = edges[shortest].body;
        for (auto i = first.begin; i < first.end; ++i) {
            const auto premise = bodies[i];
            if (bodyMarks[premise] != firstEpoch) {
                bodyMarks[premise] = firstEpoch;
                mustScratch.push_back(premise);
            }
        }
        for (auto i = incomingOffsets[node]; i < incomingOffsets[node + 1] && !mustScratch.empty(); ++i) {
            const auto source = incomingEdges[i];
            if (source == shortest) continue;
            const auto epoch = nextEpoch(bodyEpoch, bodyMarks);
            const auto body = edges[source].body;
            for (auto j = body.begin; j < body.end; ++j) bodyMarks[bodies[j]] = epoch;
            mustScratch.erase(std::remove_if(mustScratch.begin(), mustScratch.end(),
                                      [&](auto premise) { return bodyMarks[premise] != epoch; }),
                    mustScratch.end());
        }
        mustBodies.insert(mustBodies.end(), mustScratch.begin(), mustScratch.end());
        return remember();
    }

    bool prove(std::size_t target, std::size_t inputIndex, bool collectProviders) {
        const auto& edge = edges[target];
        if (!edge.safe || edge.body.end - edge.body.begin < 2 ||
                inputIndex >= edge.body.end - edge.body.begin) return false;
        const auto candidate = bodies[edge.body.begin + inputIndex];
        if (definition[candidate] == none) return false;
        const auto premises = edges[definition[candidate]].body;
        if (premises.begin == premises.end) return false;
        const auto epoch = nextEpoch(coverageEpoch, coverageMarks);
        if (collectProviders && providers.empty()) providers.resize(nodes.size());
        auto cover = [&](std::size_t premise, std::size_t provider) {
            if (coverageMarks[premise] == epoch) return;
            coverageMarks[premise] = epoch;
            if (collectProviders) providers[premise] = provider;
        };
        for (auto i = edge.body.begin; i < edge.body.end; ++i) {
            if (i == edge.body.begin + inputIndex) continue;
            const auto other = bodies[i];
            cover(other, other);
            const auto must = mustFor(other);
            for (auto j = must.begin; j < must.end; ++j) cover(mustBodies[j], other);
        }
        for (auto i = premises.begin; i < premises.end; ++i) {
            if (coverageMarks[bodies[i]] != epoch) return false;
        }
        return true;
    }

    AndInputRedundancyReport detect(bool collectWitnesses, std::vector<Candidate>* compact = nullptr) {
        AndInputRedundancyReport report;
        report.completeDerivations = complete;
        report.stats = baseStats;
        if (!complete || targets.empty()) return report;
        std::vector<unsigned char> redundant;
        if (collectWitnesses) redundant.assign(nodes.size(), 0);
        for (auto target : targets) {
            const auto& edge = edges[target];
            bool affected = false;
            for (std::size_t occurrence = 0; occurrence < edge.body.end - edge.body.begin; ++occurrence) {
                const auto candidate = bodies[edge.body.begin + occurrence];
                if (definition[candidate] == none || !prove(target, occurrence, collectWitnesses)) continue;
                ++report.stats.provenInputAssociations;
                affected = true;
                if (compact) {
                    compact->push_back({target, occurrence, candidate});
                    continue;
                }
                if (!collectWitnesses) continue;
                if (!redundant[candidate]) {
                    redundant[candidate] = 1;
                    ++report.stats.distinctRedundantNodes;
                }
                const auto& def = edges[definition[candidate]];
                AndInputRedundancyProof proof{edge.edge, occurrence, nodes[candidate], def.edge, {}};
                std::vector<std::size_t> premises(bodies.begin() + def.body.begin, bodies.begin() + def.body.end);
                std::sort(premises.begin(), premises.end());
                premises.erase(std::unique(premises.begin(), premises.end()), premises.end());
                for (auto premise : premises) {
                    const auto provider = providers[premise];
                    AndInputRedundancyWitness witness{nodes[premise], nodes[provider], {}};
                    if (premise != provider) {
                        for (auto i = incomingOffsets[provider]; i < incomingOffsets[provider + 1]; ++i) {
                            witness.sourceEdges.push_back(edges[incomingEdges[i]].edge);
                        }
                        std::sort(witness.sourceEdges.begin(), witness.sourceEdges.end(),
                                [](const auto& a, const auto& b) { return a->getId() < b->getId(); });
                    }
                    proof.witnesses.push_back(std::move(witness));
                }
                std::sort(proof.witnesses.begin(), proof.witnesses.end(),
                        [](const auto& a, const auto& b) { return a.premise->getId() < b.premise->getId(); });
                report.proofs.push_back(std::move(proof));
            }
            if (affected) ++report.stats.affectedEdges;
        }
        return report;
    }

    bool erase(const Candidate& candidate) {
        auto& edge = edges[candidate.edge];
        if (candidate.inputIndex >= edge.body.end - edge.body.begin) return false;
        const auto position = edge.body.begin + candidate.inputIndex;
        if (position >= edge.body.end || bodies[position] != candidate.node ||
                !prove(candidate.edge, candidate.inputIndex, false) ||
                !edge.edge->eraseInputOccurrence(candidate.inputIndex)) return false;
        // Mirror the successful in-place erasure in this edge's flat segment.
        // Must_1 first requested later therefore reads the current body too.
        for (auto i = position + 1; i < edge.body.end; ++i) bodies[i - 1] = bodies[i];
        --edge.body.end;
        --baseStats.inputAssociations;
        if (!outgoingOccurrences.empty() && --outgoingOccurrences[candidate.node] == 0) {
            cleanupSeeds.push_back(candidate.node);
        }
        const auto slot = mustSlots[edge.head];
        if (slot != none && std::find(bodies.begin() + edge.body.begin,
                                   bodies.begin() + edge.body.end, candidate.node) == bodies.begin() + edge.body.end) {
            auto& must = mustEntries[slot];
            const auto newEnd = std::remove(mustBodies.begin() + must.begin,
                    mustBodies.begin() + must.end, candidate.node);
            must.end = static_cast<std::size_t>(newEnd - mustBodies.begin());
        }
        return true;
    }

    void prepareCleanup(AndInputRedundancyCleanupPlan& plan) {
        plan = {};
        std::vector<unsigned char> removed(nodes.size(), 0);
        plan.finalInputAssociations = baseStats.inputAssociations;
        // All roots and active sources come from the initial pruning. Only
        // deletion can change reachability, so start where an erased input
        // loses its final active outgoing occurrence; reuse our source index.
        for (std::size_t current = 0; current < cleanupSeeds.size(); ++current) {
            const auto node = cleanupSeeds[current];
            if (nodes[node]->needOutput || nodes[node]->hasEvidence()) continue;
            if (!safe[node]) {
                plan = {{}, {}, true};
                return;
            }
            plan.nodes.push_back(nodes[node]);
            removed[node] = 1;
            const auto incoming = incomingOffsets[node + 1] - incomingOffsets[node];
            if ((!nodes[node]->isFact && incoming > 1) || (nodes[node]->isFact && incoming > 0)) {
                ++plan.removedDisjunctionNodes;
            }
            for (auto i = incomingOffsets[node]; i < incomingOffsets[node + 1]; ++i) {
                const auto& edge = edges[incomingEdges[i]];
                if (!edge.safe) {
                    plan = {{}, {}, true};
                    return;
                }
                plan.edges.push_back(edge.edge);
                plan.finalInputAssociations -= edge.body.end - edge.body.begin;
                for (auto j = edge.body.begin; j < edge.body.end; ++j) {
                    const auto input = bodies[j];
                    // A detached recursive component can retain its internal
                    // references forever. Fall back upon touching it, even
                    // before its outgoing count reaches zero.
                    if (!safe[input] || outgoingOccurrences[input] == 0) {
                        plan = {{}, {}, true};
                        return;
                    }
                    if (--outgoingOccurrences[input] == 0) cleanupSeeds.push_back(input);
                }
            }
        }
        // Removing a node removes all its sources, so retained heads keep their
        // original incoming degree. Outgoing counts already reflect both body
        // erasures and the cleanup closure. Scan flat indexes, not graph bodies
        // and a newly allocated pointer-to-degree map.
        for (std::size_t node = 0; node < nodes.size(); ++node) {
            if (removed[node]) continue;
            plan.maxInDegree = std::max(plan.maxInDegree, incomingOffsets[node + 1] - incomingOffsets[node]);
            plan.maxOutDegree = std::max(plan.maxOutDegree, outgoingOccurrences[node]);
        }
        for (const auto& edge : edges) {
            if (!removed[edge.head]) {
                plan.maxHyperedgeInputs = std::max(plan.maxHyperedgeInputs, edge.body.end - edge.body.begin);
            }
        }
        plan.hasSummary = true;
    }
};

}  // namespace detail

// Analyze a complete semantic working snapshot, never a SISO/component subview.
// The caller must confirm that all active derivation sources are represented.
// Rebuild local indexes on every call; do not consult or mutate graph caches.
inline AndInputRedundancyReport detectAndInputRedundancy(
        const DerivationGraphViewInterface& view, bool completeDerivations = false) {
    const auto start = std::chrono::steady_clock::now();
    detail::AndInputRedundancySnapshot snapshot;
    snapshot.initialize(view, completeDerivations, true);
    auto report = snapshot.detect(true);
    report.stats.analysisMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    return report;
}

struct AndInputRedundancyPassStats {
    std::size_t deletedInputAssociations = 0;
    std::size_t affectedEdges = 0;
    // Includes the final detection round that establishes the fixpoint.
    std::size_t rounds = 0;
    std::size_t initialInputAssociations = 0;
    std::size_t finalInputAssociations = 0;
    // Remaining independently certified opportunities, not total body size.
    std::size_t remainingInputAssociations = 0;
    double detectionMs = 0.0;
    double mutationMs = 0.0;
    double cleanupPlanningMs = 0.0;
    double totalMs = 0.0;
};

// Run before SISO on a complete working view. Only erase positive AND inputs;
// preserve every node, edge object and random event. The caller invalidates the
// underlying graph caches and performs query/evidence-aware pruning afterwards.
// A cleanup plan reuses these indexes only for a freshly pruned complete view;
// special pruning policies must continue to use the ordinary graph pruner.
inline AndInputRedundancyPassStats eliminateAndInputRedundancy(
        WorkingSubgraphView& view, bool completeDerivations = false,
        AndInputRedundancyCleanupPlan* cleanup = nullptr) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto elapsedMs = [](Clock::time_point since) {
        return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    };
    AndInputRedundancyPassStats stats;
    detail::AndInputRedundancySnapshot snapshot;
    if (cleanup) *cleanup = {};
    snapshot.initialize(view, completeDerivations, false, cleanup != nullptr);
    stats.detectionMs = elapsedMs(start);
    stats.initialInputAssociations = snapshot.baseStats.inputAssociations;
    std::vector<unsigned char> affected(snapshot.edges.size(), 0);
    std::vector<detail::AndInputRedundancySnapshot::Candidate> candidates;
    while (true) {
        const auto detectionStart = Clock::now();
        candidates.clear();
        const auto report = snapshot.detect(false, &candidates);
        ++stats.rounds;
        stats.detectionMs += elapsedMs(detectionStart);
        stats.remainingInputAssociations = report.stats.provenInputAssociations;
        if (candidates.empty()) break;
        const auto mutationStart = Clock::now();
        std::size_t deletedThisRound = 0;
        // Generation follows the fixed, ID-sorted target pool. Descend indexes
        // within each edge so a successful erase preserves pending occurrences.
        for (std::size_t begin = 0; begin < candidates.size();) {
            auto end = begin + 1;
            while (end < candidates.size() && candidates[end].edge == candidates[begin].edge) ++end;
            for (auto current = end; current > begin;) {
                const auto& candidate = candidates[--current];
                if (!snapshot.erase(candidate)) continue;
                ++deletedThisRound;
                ++stats.deletedInputAssociations;
                if (!affected[candidate.edge]) {
                    affected[candidate.edge] = 1;
                    ++stats.affectedEdges;
                }
            }
            begin = end;
        }
        if (deletedThisRound != 0) view.invalidateCaches();
        stats.mutationMs += elapsedMs(mutationStart);
        if (deletedThisRound == 0) break;
    }
    stats.finalInputAssociations = snapshot.baseStats.inputAssociations;
    if (cleanup && stats.deletedInputAssociations != 0) {
        const auto cleanupStart = Clock::now();
        snapshot.prepareCleanup(*cleanup);
        stats.cleanupPlanningMs = elapsedMs(cleanupStart);
    }
    stats.totalMs = elapsedMs(start);
    return stats;
}

inline void writeAndInputRedundancyReport(
        std::ostream& out, const AndInputRedundancyReport& report, const std::string& phase) {
    auto nodeJson = [](const NodePtr& node) {
        return json11::Json::object{{"id", std::to_string(node->getId())},
                {"name", node->getTuple().toString()}, {"tuple", node->getTuple().toJson()},
                {"is_fact", node->isFact}, {"original_fact", node->isOriginalFactNode()},
                {"is_shadow", node->isShadow}};
    };
    auto edgeJson = [](const EdgePtr& edge) {
        json11::Json::array support, inputs, negations;
        for (auto token : edge->getProbabilisticSupportTokens()) support.emplace_back(std::to_string(token));
        for (const auto& node : edge->getInputs()) inputs.emplace_back(std::to_string(node->getId()));
        for (bool neg : edge->getBodyNegations()) negations.emplace_back(neg);
        return json11::Json::object{{"id", std::to_string(edge->getId())},
                {"rule_id", std::to_string(edge->getRuleApp().ruleId)},
                {"synthetic", edge->getRule() == nullptr}, {"probability", edge->getProbability()},
                {"support_tokens", support}, {"head_id", std::to_string(edge->getOutput()->getId())},
                {"input_ids", inputs}, {"negations", negations}};
    };
    const auto& s = report.stats;
    auto metadata = json11::Json(json11::Json::object{{"schema", "and-input-redundancy-v1"}, {"phase", phase},
            {"complete_derivations", report.completeDerivations}, {"read_only", true},
            {"independent_certificates", true},
            {"stats", json11::Json::object{{"nodes", static_cast<double>(s.nodes)},
                    {"edges", static_cast<double>(s.edges)},
                    {"input_associations", static_cast<double>(s.inputAssociations)},
                    {"eligible_definitions", static_cast<double>(s.eligibleDefinitions)},
                    {"proven_input_associations", static_cast<double>(s.provenInputAssociations)},
                    {"affected_edges", static_cast<double>(s.affectedEdges)},
                    {"distinct_redundant_nodes", static_cast<double>(s.distinctRedundantNodes)},
                    {"recursive_nodes", static_cast<double>(s.recursiveNodes)}, {"analysis_ms", s.analysisMs}}}
    }).dump();
    // Stream certificates to avoid a second in-memory copy of the entire report.
    metadata.pop_back();
    out << metadata << ",\"proofs\":[";
    bool first = true;
    for (const auto& proof : report.proofs) {
        json11::Json::array witnesses;
        for (const auto& witness : proof.witnesses) {
            json11::Json::array sources;
            for (const auto& source : witness.sourceEdges) sources.emplace_back(edgeJson(source));
            witnesses.emplace_back(json11::Json::object{{"premise", nodeJson(witness.premise)},
                    {"provider", nodeJson(witness.provider)}, {"source_edges", sources},
                    {"kind", witness.sourceEdges.empty() ? "direct" : "must_1"}});
        }
        if (!first) out << ',';
        first = false;
        out << json11::Json(json11::Json::object{{"edge", edgeJson(proof.edge)},
                {"head", nodeJson(proof.edge->getOutput())},
                {"input_index", static_cast<double>(proof.inputIndex)},
                {"redundant", nodeJson(proof.redundant)}, {"definition", edgeJson(proof.definition)},
                {"witnesses", witnesses}}).dump();
    }
    out << "]}\n";
}

}  // namespace souffle::problog
