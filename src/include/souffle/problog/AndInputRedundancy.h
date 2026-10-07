#pragma once

#include "souffle/problog/DerivationGraph.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
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

namespace detail {

struct AndInputRedundancySnapshot {
    struct EdgeInfo {
        EdgePtr edge;
        std::size_t head;
        std::vector<std::size_t> inputs;
        bool positive = false;
    };
    std::vector<NodePtr> nodes;
    std::unordered_map<NodePtr, std::size_t> index;
    std::vector<EdgeInfo> edges;
    std::vector<std::vector<std::size_t>> incoming;
    std::vector<bool> recursive;
    std::vector<std::unordered_set<std::size_t>> must;
};

// Mutation rounds reuse the indexed analysis and omit expensive certificate
// witnesses. The ordinary read-only diagnostic still records complete proofs.
inline AndInputRedundancyReport detectAndInputRedundancySnapshot(
        const DerivationGraphViewInterface& view, bool completeDerivations,
        AndInputRedundancySnapshot* snapshot, bool collectWitnesses) {
    const auto start = std::chrono::steady_clock::now();
    AndInputRedundancyReport report;
    report.completeDerivations = completeDerivations;
    auto finish = [&]() {
        report.stats.analysisMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
    };
    report.stats.nodes = view.getNodes().size();
    report.stats.edges = view.getEdges().size();
    for (const auto& edge : view.getEdges()) {
        if (edge) report.stats.inputAssociations += edge->getInputs().size();
    }
    if (!completeDerivations) {
        finish();
        return report;
    }

    std::vector<NodePtr> nodes(view.getNodes().begin(), view.getNodes().end());
    std::unordered_map<NodePtr, std::size_t> index;
    index.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i]) {
            report.completeDerivations = false;
            finish();
            return report;
        }
        index.emplace(nodes[i], i);
    }
    using EdgeInfo = AndInputRedundancySnapshot::EdgeInfo;
    std::vector<EdgeInfo> edges;
    edges.reserve(view.getEdges().size());
    std::vector<std::vector<std::size_t>> incoming(nodes.size()), next(nodes.size()), prev(nodes.size());
    for (const auto& edge : view.getEdges()) {
        if (!edge || !index.count(edge->getOutput())) {
            report.completeDerivations = false;
            break;
        }
        EdgeInfo info{edge, index.at(edge->getOutput()), {}, false};
        const auto& inputs = edge->getInputs();
        const auto& negations = edge->getBodyNegations();
        info.positive = inputs.size() == negations.size() &&
                std::none_of(negations.begin(), negations.end(), [](bool neg) { return neg; });
        for (const auto& input : inputs) {
            auto it = index.find(input);
            if (it == index.end()) {
                report.completeDerivations = false;
                break;
            }
            info.inputs.push_back(it->second);
            next[it->second].push_back(info.head);
            prev[info.head].push_back(it->second);
        }
        if (!report.completeDerivations) break;
        incoming[info.head].push_back(edges.size());
        edges.push_back(std::move(info));
    }
    if (!report.completeDerivations) {
        finish();
        return report;
    }

    // Iterative Kosaraju SCCs over all signed body-to-head arcs. No recursive
    // DFS and no full dependency/component/depth analysis or cache reuse.
    std::vector<bool> visited(nodes.size(), false), recursive(nodes.size(), false);
    std::vector<std::size_t> order;
    order.reserve(nodes.size());
    for (std::size_t root = 0; root < nodes.size(); ++root) {
        if (visited[root]) continue;
        std::vector<std::pair<std::size_t, std::size_t>> stack{{root, 0}};
        visited[root] = true;
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.second == next[frame.first].size()) {
                order.push_back(frame.first);
                stack.pop_back();
            } else {
                const auto child = next[frame.first][frame.second++];
                if (!visited[child]) {
                    visited[child] = true;
                    stack.emplace_back(child, 0);
                }
            }
        }
    }
    std::fill(visited.begin(), visited.end(), false);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (visited[*it]) continue;
        std::vector<std::size_t> stack{*it}, component;
        visited[*it] = true;
        while (!stack.empty()) {
            const auto node = stack.back();
            stack.pop_back();
            component.push_back(node);
            for (auto parent : prev[node]) {
                if (!visited[parent]) {
                    visited[parent] = true;
                    stack.push_back(parent);
                }
            }
        }
        if (component.size() > 1 ||
                std::find(next[*it].begin(), next[*it].end(), *it) != next[*it].end()) {
            for (auto node : component) recursive[node] = true;
            report.stats.recursiveNodes += component.size();
        }
    }
    auto safeNode = [&](std::size_t node) { return !nodes[node]->isShadow && !recursive[node]; };
    auto safeEdge = [&](const EdgeInfo& edge) {
        return edge.positive && safeNode(edge.head) &&
                std::all_of(edge.inputs.begin(), edge.inputs.end(), safeNode);
    };

    // Must_1 is an intersection, including every alternative rule event.
    // Facts, empty bodies, aliases and unsupported local sources contribute nothing.
    std::vector<std::unordered_set<std::size_t>> must(nodes.size());
    std::vector<std::size_t> definition(nodes.size(), edges.size());
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        if (!safeNode(node) || nodes[node]->isFact || nodes[node]->isOriginalFactNode() ||
                incoming[node].empty()) continue;
        bool first = true;
        for (auto edgeId : incoming[node]) {
            const auto& edge = edges[edgeId];
            if (!safeEdge(edge) || edge.inputs.empty()) {
                must[node].clear();
                break;
            }
            std::unordered_set<std::size_t> body(edge.inputs.begin(), edge.inputs.end());
            if (first) {
                must[node] = std::move(body);
                first = false;
            } else {
                for (auto it = must[node].begin(); it != must[node].end();) {
                    if (!body.count(*it)) it = must[node].erase(it);
                    else ++it;
                }
            }
        }
        if (incoming[node].size() == 1) {
            const auto edgeId = incoming[node][0];
            const auto& edge = edges[edgeId];
            if (safeEdge(edge) && !edge.inputs.empty() && edge.edge->isDeterministic() &&
                    edge.edge->getProbabilisticSupportTokens().empty()) {
                definition[node] = edgeId;
                ++report.stats.eligibleDefinitions;
            }
        }
    }

    std::unordered_set<EdgePtr> affected;
    std::unordered_set<NodePtr> redundant;
    for (const auto& target : edges) {
        if (!safeEdge(target) || target.inputs.size() < 2) continue;
        for (std::size_t inputIndex = 0; inputIndex < target.inputs.size(); ++inputIndex) {
            const auto candidate = target.inputs[inputIndex];
            if (definition[candidate] == edges.size()) continue;
            const auto& def = edges[definition[candidate]];
            // A provider map constructs the union of {x_i} and Must_1(x_i).
            std::unordered_map<std::size_t, std::size_t> providers;
            for (std::size_t i = 0; i < target.inputs.size(); ++i) {
                if (i == inputIndex) continue;
                const auto other = target.inputs[i];
                providers.emplace(other, other);
                for (auto premise : must[other]) providers.emplace(premise, other);
            }
            if (!std::all_of(def.inputs.begin(), def.inputs.end(),
                        [&](auto premise) { return providers.count(premise) != 0; })) continue;
            AndInputRedundancyProof proof{target.edge, inputIndex, nodes[candidate], def.edge, {}};
            if (collectWitnesses) {
                std::unordered_set<std::size_t> seenPremises;
                for (auto premise : def.inputs) {
                    if (!seenPremises.insert(premise).second) continue;
                    const auto provider = providers.at(premise);
                    AndInputRedundancyWitness witness{nodes[premise], nodes[provider], {}};
                    if (premise != provider) {
                        for (auto source : incoming[provider]) witness.sourceEdges.push_back(edges[source].edge);
                        std::sort(witness.sourceEdges.begin(), witness.sourceEdges.end(),
                                [](const auto& a, const auto& b) { return a->getId() < b->getId(); });
                    }
                    proof.witnesses.push_back(std::move(witness));
                }
                std::sort(proof.witnesses.begin(), proof.witnesses.end(),
                        [](const auto& a, const auto& b) { return a.premise->getId() < b.premise->getId(); });
            }
            report.proofs.push_back(std::move(proof));
            affected.insert(target.edge);
            redundant.insert(nodes[candidate]);
        }
    }
    std::sort(report.proofs.begin(), report.proofs.end(), [](const auto& a, const auto& b) {
        if (a.edge->getId() != b.edge->getId()) return a.edge->getId() < b.edge->getId();
        return a.inputIndex < b.inputIndex;
    });
    report.stats.provenInputAssociations = report.proofs.size();
    report.stats.affectedEdges = affected.size();
    report.stats.distinctRedundantNodes = redundant.size();
    if (snapshot) {
        snapshot->nodes = std::move(nodes);
        snapshot->index = std::move(index);
        snapshot->edges = std::move(edges);
        snapshot->incoming = std::move(incoming);
        snapshot->recursive = std::move(recursive);
        snapshot->must = std::move(must);
    }
    finish();
    return report;
}

}  // namespace detail

// Analyze a complete semantic working snapshot, never a SISO/component subview.
// The caller must confirm that all active derivation sources are represented.
// Rebuild local indexes on every call; do not consult or mutate graph caches.
inline AndInputRedundancyReport detectAndInputRedundancy(
        const DerivationGraphViewInterface& view, bool completeDerivations = false) {
    return detail::detectAndInputRedundancySnapshot(view, completeDerivations, nullptr, true);
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
    double totalMs = 0.0;
};

// Run before SISO on a complete working view. Only erase positive AND inputs;
// preserve every node, edge object and random event. The caller invalidates the
// underlying graph caches and performs query/evidence-aware pruning afterwards.
inline AndInputRedundancyPassStats eliminateAndInputRedundancy(
        WorkingSubgraphView& view, bool completeDerivations = false) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto elapsedMs = [](Clock::time_point since) {
        return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    };
    AndInputRedundancyPassStats stats;
    std::unordered_set<EdgePtr> affected;
    while (true) {
        detail::AndInputRedundancySnapshot snapshot;
        const auto detectionStart = Clock::now();
        const auto report = detail::detectAndInputRedundancySnapshot(
                view, completeDerivations, &snapshot, false);
        ++stats.rounds;
        stats.detectionMs += elapsedMs(detectionStart);
        if (stats.rounds == 1) stats.initialInputAssociations = report.stats.inputAssociations;
        stats.finalInputAssociations = report.stats.inputAssociations;
        stats.remainingInputAssociations = report.stats.provenInputAssociations;
        if (!report.completeDerivations || report.proofs.empty()) break;

        const auto mutationStart = Clock::now();
        // Deletions cannot create a cycle, so the snapshot's cycle guards remain
        // conservative for this round. All dynamic bodies are read from edges.
        auto safeNode = [&](std::size_t node) {
            return !snapshot.nodes[node]->isShadow && !snapshot.recursive[node];
        };
        auto safeEdge = [&](const EdgePtr& edge) {
            if (!edge || !safeNode(snapshot.index.at(edge->getOutput()))) return false;
            const auto& inputs = edge->getInputs();
            const auto& negations = edge->getBodyNegations();
            return inputs.size() == negations.size() &&
                    std::none_of(negations.begin(), negations.end(), [](bool neg) { return neg; }) &&
                    std::all_of(inputs.begin(), inputs.end(),
                            [&](const auto& input) { return safeNode(snapshot.index.at(input)); });
        };
        std::size_t deletedThisRound = 0;
        // The report is sorted by edge ID and ascending occurrence. Descending
        // within each edge keeps pending occurrence indexes stable after erases.
        for (std::size_t begin = 0; begin < report.proofs.size();) {
            std::size_t end = begin + 1;
            while (end < report.proofs.size() && report.proofs[end].edge == report.proofs[begin].edge) ++end;
            for (std::size_t current = end; current > begin;) {
                const auto& proof = report.proofs[--current];
                const auto& edge = proof.edge;
                const auto& inputs = edge->getInputs();
                if (inputs.size() <= 1 || proof.inputIndex >= inputs.size() ||
                        inputs[proof.inputIndex] != proof.redundant || !safeEdge(edge)) continue;
                const auto candidate = snapshot.index.at(proof.redundant);
                const auto& node = proof.redundant;
                if (!safeNode(candidate) || node->isFact || node->isOriginalFactNode() ||
                        snapshot.incoming[candidate].size() != 1) continue;
                const auto& definition = snapshot.edges[snapshot.incoming[candidate][0]].edge;
                if (definition != proof.definition || !safeEdge(definition) || definition->getInputs().empty() ||
                        !definition->isDeterministic() || !definition->getProbabilisticSupportTokens().empty()) continue;

                // Re-prove against CURRENT inputs and refreshed Must_1 sets.
                // Independently valid snapshot proofs may invalidate each other.
                std::unordered_set<std::size_t> guaranteed;
                for (std::size_t i = 0; i < inputs.size(); ++i) {
                    if (i == proof.inputIndex) continue;
                    const auto other = snapshot.index.at(inputs[i]);
                    guaranteed.insert(other);
                    guaranteed.insert(snapshot.must[other].begin(), snapshot.must[other].end());
                }
                if (!std::all_of(definition->getInputs().begin(), definition->getInputs().end(),
                            [&](const auto& premise) { return guaranteed.count(snapshot.index.at(premise)) != 0; })) continue;
                if (!edge->eraseInputOccurrence(proof.inputIndex)) continue;
                ++deletedThisRound;
                ++stats.deletedInputAssociations;
                affected.insert(edge);
                // Incoming source sets are fixed and bodies stay positive and
                // nonempty. This deletion can only remove this one node from
                // Must_1(head), once its last body occurrence disappears. This
                // is the exact updated intersection without rescanning every
                // alternative derivation of a high-fan-in head.
                if (std::find(inputs.begin(), inputs.end(), node) == inputs.end()) {
                    snapshot.must[snapshot.index.at(edge->getOutput())].erase(candidate);
                }
            }
            begin = end;
        }
        if (deletedThisRound != 0) view.invalidateCaches();
        stats.mutationMs += elapsedMs(mutationStart);
        stats.finalInputAssociations -= deletedThisRound;
        if (deletedThisRound == 0) break;
    }
    stats.affectedEdges = affected.size();
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
