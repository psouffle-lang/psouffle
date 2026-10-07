#pragma once

#include "souffle/problog/AndInputRedundancy.h"
#include "souffle/problog/DerivationGraph.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace souffle::problog {

struct DeterministicEventAliasStats {
    bool reusedPruneIndexes = false;
    std::size_t provenTrueNodes = 0;
    std::size_t provenDerivedTrueNodes = 0;
    std::size_t deterministicProofEdges = 0;
    std::size_t candidates = 0;
    std::size_t mergedAliases = 0;
    std::size_t queryAliases = 0;
    // Replacements and duplicate removals include inactive raw consumers too.
    std::size_t inputReplacements = 0;
    std::size_t duplicateInputsRemoved = 0;
    std::size_t activeInputReplacements = 0;
    std::size_t activeDuplicateInputsRemoved = 0;
    std::size_t removedNodes = 0;
    std::size_t removedEdges = 0;
    std::size_t removedOwnerEdges = 0;
    std::size_t skippedCycleAliases = 0;
    std::size_t evidenceConflictClasses = 0;
    double analysisMs = 0.0;
    double mutationMs = 0.0;
    double totalMs = 0.0;
};

struct DeterministicEventAliasResult {
    // Removed nodes retain their original tuples and flags for output binding.
    std::vector<std::pair<NodePtr, NodePtr>> aliases;
    std::vector<std::pair<NodePtr, NodePtr>> outputAliases;
    // These roots became outputs solely to supply the aliases' probabilities.
    std::vector<NodePtr> promotedOutputRoots;
    DeterministicEventAliasStats stats;
};

// A unique deterministic positive definition C <- X,T1,...,Tn proves C == X
// when each Ti has a positive deterministic proof from structurally true facts.
// This is event equality in every world, not equality of marginal probabilities.
// Only Boolean occurrences are rewritten: grounded rule applications and
// aggregate witness records remain distinct. The complete active view must
// belong to this owner and all graph adjacency must be current; online callers
// must not invoke this pass.
inline DeterministicEventAliasResult eliminateDeterministicEventAliases(
        WorkingDerivationGraph& graph, WorkingSubgraphView& view, bool completeDerivations = false,
        const detail::AndInputRedundancySnapshot* prepared = nullptr) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto elapsedMs = [](Clock::time_point from) {
        return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };
    DeterministicEventAliasResult result;
    auto finishAnalysis = [&]() {
        result.stats.analysisMs = elapsedMs(start);
        result.stats.totalMs = result.stats.analysisMs;
    };
    if (!completeDerivations) {
        finishAnalysis();
        return result;
    }

    constexpr auto none = std::numeric_limits<std::size_t>::max();
    // A pruning collector can supply the existing source/body index. Borrow
    // only its complete snapshot for this exact owner and active view, before
    // any mutation; ordinary callers still validate every owned endpoint.
    const auto* snapshot = prepared && prepared->complete && prepared->sourcesFinalized &&
                    prepared->endpointsValid && prepared->preparedOwner == &graph &&
                    prepared->preparedViewNodes == &view.getNodes() &&
                    prepared->preparedViewEdges == &view.getEdges() &&
                    prepared->nodes.size() == view.getNodes().size() &&
                    prepared->edges.size() == view.getEdges().size() &&
                    prepared->incomingOffsets.size() == prepared->nodes.size() + 1 &&
                    prepared->incomingEdges.size() == prepared->edges.size()
            ? prepared : nullptr;
    result.stats.reusedPruneIndexes = snapshot != nullptr;
    std::vector<NodePtr> localNodes;
    const auto& nodes = snapshot ? snapshot->nodes : localNodes;
    std::size_t maxNodeId = 0;
    if (!snapshot) {
        localNodes.reserve(view.getNodes().size());
        for (const auto& node : view.getNodes()) {
            if (!node || !graph.getNodes().count(node)) {
                finishAnalysis();
                return result;
            }
            localNodes.push_back(node);
            maxNodeId = std::max(maxNodeId, node->getId());
        }
    }

    // Match the existing AND snapshot's guarded dense-ID lookup. IDs accelerate
    // lookup only; sparse/colliding IDs still use actual pointer identity.
    std::vector<std::size_t> idIndex;
    std::unordered_map<const Node*, std::size_t> pointerIndex;
    if (!snapshot && !nodes.empty() && maxNodeId != none && nodes.size() <= (none - 4096) / 4 &&
            maxNodeId < nodes.size() * 4 + 4096) {
        idIndex.assign(maxNodeId + 1, none);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto& index = idIndex[nodes[i]->getId()];
            if (index != none) {
                idIndex.clear();
                break;
            }
            index = i;
        }
    }
    if (!snapshot && idIndex.empty()) {
        pointerIndex.reserve(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) pointerIndex.emplace(nodes[i].get(), i);
    }
    auto indexOf = [&](const NodePtr& node) {
        if (snapshot) return snapshot->indexOf(node);
        if (!node) return none;
        if (!idIndex.empty()) {
            const auto id = node->getId();
            if (id >= idIndex.size()) return none;
            const auto index = idIndex[id];
            return index != none && nodes[index] == node ? index : none;
        }
        const auto found = pointerIndex.find(node.get());
        return found == pointerIndex.end() ? none : found->second;
    };

    struct Sources {
        std::size_t count = 0;
        const Hyperedge* unique = nullptr;
        std::size_t firstProofOccurrence = std::numeric_limits<std::size_t>::max();
        bool provenTrue = false;
        bool copySupported = false;
    };
    struct TrueProof {
        std::size_t head;
        std::size_t remaining;
        bool supported = true;
    };
    struct ProofOccurrence {
        std::size_t proof;
        std::size_t next;
    };
    std::vector<Sources> sources(nodes.size());
    std::vector<TrueProof> trueProofs;
    std::vector<ProofOccurrence> proofOccurrences;
    std::vector<std::size_t> emptyProofs;
    auto addProofOccurrence = [&](std::size_t input, std::size_t proof) {
        auto& source = sources[input];
        proofOccurrences.push_back({proof, source.firstProofOccurrence});
        source.firstProofOccurrence = proofOccurrences.size() - 1;
    };
    if (snapshot) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto& source = sources[i];
            const auto begin = snapshot->incomingOffsets[i];
            source.count = snapshot->incomingOffsets[i + 1] - begin;
            if (source.count == 1) source.unique = snapshot->edges[snapshot->incomingEdges[begin]].edge.get();
        }
        for (const auto& edge : snapshot->edges) {
            const auto& head = nodes[edge.head];
            const bool supported = edge.safe && !head->isFact && !head->isOriginalFactNode() &&
                    edge.edge->isDeterministic() && edge.edge->getProbabilisticSupportTokens().empty();
            if (sources[edge.head].count == 1) sources[edge.head].copySupported = supported;
            if (!supported) continue;
            const auto proofIndex = trueProofs.size();
            trueProofs.push_back({edge.head, edge.body.end - edge.body.begin, true});
            ++result.stats.deterministicProofEdges;
            for (auto i = edge.body.begin; i < edge.body.end; ++i) {
                addProofOccurrence(snapshot->bodies[i], proofIndex);
            }
            if (edge.body.begin == edge.body.end) emptyProofs.push_back(proofIndex);
        }
    } else {
        // Count every active source, including random, negative and unsupported
        // definitions. An unsupported alternative must prevent a unique-copy proof.
        // Build the positive true-proof occurrence index in the same traversal.
        for (const auto& edge : view.getEdges()) {
            if (!edge || !graph.getEdges().count(edge) ||
                    edge->getInputs().size() != edge->getBodyNegations().size()) {
                finishAnalysis();
                return result;
            }
            const auto headIndex = indexOf(edge->getOutputRef());
            if (headIndex == none) {
                finishAnalysis();
                return result;
            }
            const auto& head = nodes[headIndex];
            const bool proofCandidate = !head->isFact && !head->isOriginalFactNode() && !head->isShadow &&
                    edge->isDeterministic() && edge->getProbabilisticSupportTokens().empty();
            const auto proofIndex = trueProofs.size();
            if (proofCandidate) trueProofs.push_back({headIndex, edge->getInputs().size(), true});
            for (std::size_t i = 0; i < edge->getInputs().size(); ++i) {
                const auto& input = edge->getInputs()[i];
                const auto inputIndex = indexOf(input);
                if (inputIndex == none) {
                    finishAnalysis();
                    return result;
                }
                if (proofCandidate) {
                    auto& proof = trueProofs[proofIndex];
                    proof.supported = proof.supported && !input->isShadow && !edge->getBodyNegations()[i];
                    if (proof.supported) addProofOccurrence(inputIndex, proofIndex);
                }
            }
            if (proofCandidate && trueProofs[proofIndex].supported) {
                ++result.stats.deterministicProofEdges;
                if (edge->getInputs().empty()) emptyProofs.push_back(proofIndex);
            }
            auto& incoming = sources[headIndex];
            ++incoming.count;
            incoming.unique = edge.get();
            incoming.copySupported = proofCandidate && trueProofs[proofIndex].supported;
        }
    }
    auto isTrueFact = [&](std::size_t index) {
        const auto& node = nodes[index];
        // Requiring no active rule source also preserves the runtime's existing
        // fact-initialization behavior for nodes with both facts and derivations.
        return !node->isShadow && node->isFact && node->getProbability() == 1.0 &&
                node->getProbabilisticSupportTokens().empty() && sources[index].count == 0;
    };

    std::vector<std::size_t> trueQueue;
    auto proveTrue = [&](std::size_t index) {
        auto& incoming = sources[index];
        if (incoming.provenTrue) return;
        incoming.provenTrue = true;
        trueQueue.push_back(index);
        ++result.stats.provenTrueNodes;
        if (!nodes[index]->isFact) ++result.stats.provenDerivedTrueNodes;
    };
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (isTrueFact(i)) proveTrue(i);
    }
    for (const auto proofIndex : emptyProofs) proveTrue(trueProofs[proofIndex].head);
    // A single top derivation suffices even with alternative random sources.
    // Each occurrence is discharged once, including repeated body inputs.
    // Unseeded positive cycles cannot prove themselves true; evidence is not a seed.
    for (std::size_t current = 0; current < trueQueue.size(); ++current) {
        for (auto occurrence = sources[trueQueue[current]].firstProofOccurrence; occurrence != none;
                occurrence = proofOccurrences[occurrence].next) {
            auto& proof = trueProofs[proofOccurrences[occurrence].proof];
            if (proof.supported && --proof.remaining == 0) proveTrue(proof.head);
        }
    }

    std::vector<std::size_t> providers(nodes.size(), none), candidates;
    std::size_t candidateOutputs = 0;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        const auto& incoming = sources[index];
        if (incoming.count != 1 || !incoming.copySupported) continue;
        const auto& edge = incoming.unique;
        if (edge->getInputs().empty()) continue;
        auto provider = none;
        auto acceptInput = [&](std::size_t inputIndex) {
            if (sources[inputIndex].provenTrue) return true;
            if (provider != none && provider != inputIndex) return false;
            provider = inputIndex;
            return true;
        };
        bool supported = true;
        if (snapshot) {
            const auto& definition = snapshot->edges[snapshot->incomingEdges[snapshot->incomingOffsets[index]]];
            for (auto i = definition.body.begin; i < definition.body.end; ++i) {
                if (!acceptInput(snapshot->bodies[i])) { supported = false; break; }
            }
        } else {
            for (const auto& input : edge->getInputs()) {
                if (!acceptInput(indexOf(input))) { supported = false; break; }
            }
        }
        if (supported && provider != none) {
            providers[index] = provider;
            candidates.push_back(index);
            candidateOutputs += node->needOutput;
        }
    }
    result.stats.candidates = candidates.size();
    std::sort(candidates.begin(), candidates.end(), [&](std::size_t left, std::size_t right) {
        const auto& a = nodes[left];
        const auto& b = nodes[right];
        if (a->getId() != b->getId()) return a->getId() < b->getId();
        return std::less<const Node*>{}(a.get(), b.get());
    });

    // The copy relation is functional. Resolve each chain once, rejecting both
    // copy cycles and chains reaching them; no global implication solver is used.
    std::vector<unsigned char> state(nodes.size(), 0);
    std::vector<std::size_t> roots(nodes.size(), none), path;
    for (const auto& candidate : candidates) {
        if (state[candidate] == 2) continue;
        path.clear();
        auto current = candidate;
        auto root = none;
        while (true) {
            const auto provider = providers[current];
            if (provider == none) {
                root = current;
                break;
            }
            const auto mark = state[current];
            if (mark == 2) {
                root = roots[current];
                break;
            }
            if (mark == 1) break;
            state[current] = 1;
            path.push_back(current);
            current = provider;
        }
        for (const auto& node : path) {
            state[node] = 2;
            roots[node] = root;
            if (root == none) ++result.stats.skippedCycleAliases;
        }
    }

    struct ClassEvidence {
        bool registered = false;
        bool observed = false;
        bool value = false;
        bool conflict = false;
        bool promoted = false;
    };
    std::vector<ClassEvidence> evidence(nodes.size());
    for (const auto& candidate : candidates) {
        const auto root = roots[candidate];
        if (root == none) continue;
        auto& observed = evidence[root];
        auto record = [&](std::size_t index) {
            const auto& node = nodes[index];
            if (!node->hasEvidence()) return;
            if (observed.observed && observed.value != node->getEvidenceValue() && !observed.conflict) {
                observed.conflict = true;
                ++result.stats.evidenceConflictClasses;
            }
            observed.observed = true;
            observed.value = node->getEvidenceValue();
        };
        if (!observed.registered) {
            observed.registered = true;
            record(root);
        }
        record(candidate);
    }
    result.aliases.reserve(candidates.size());
    result.outputAliases.reserve(candidateOutputs);
    for (const auto& candidate : candidates) {
        const auto root = roots[candidate];
        if (root == none || evidence[root].conflict) continue;
        result.aliases.emplace_back(nodes[candidate], nodes[root]);
        if (nodes[candidate]->needOutput) {
            result.outputAliases.emplace_back(nodes[candidate], nodes[root]);
            if (!nodes[root]->needOutput && !evidence[root].promoted) {
                evidence[root].promoted = true;
                result.promotedOutputRoots.push_back(nodes[root]);
            }
        }
    }
    result.stats.analysisMs = elapsedMs(start);
    const auto mutationStart = Clock::now();
    const auto mutation = graph.applyEventAliases(view, result.aliases);
    result.stats.mergedAliases = result.aliases.size();
    result.stats.queryAliases = result.outputAliases.size();
    result.stats.inputReplacements = mutation.inputReplacements;
    result.stats.duplicateInputsRemoved = mutation.duplicateInputsRemoved;
    result.stats.activeInputReplacements = mutation.activeInputReplacements;
    result.stats.activeDuplicateInputsRemoved = mutation.activeDuplicateInputsRemoved;
    result.stats.removedNodes = mutation.removedNodes;
    result.stats.removedEdges = mutation.removedEdges;
    result.stats.removedOwnerEdges = mutation.removedOwnerEdges;
    result.stats.mutationMs = elapsedMs(mutationStart);
    result.stats.totalMs = elapsedMs(start);
    return result;
}

}  // namespace souffle::problog
