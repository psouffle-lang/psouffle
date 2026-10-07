#pragma once

#include "souffle/problog/AndInputRedundancy.h"

#include <functional>

namespace souffle::problog {

struct AndInputRedundancyDegree {
    std::size_t incoming = 0;
    // Count body occurrences, including repeated inputs, rather than raw edge-list entries.
    std::size_t outgoing = 0;
};

struct AndInputRedundancyWorkspace {
    using DegreeMap = std::unordered_map<NodePtr, AndInputRedundancyDegree>;
    DegreeMap degrees;
    std::size_t initialInputAssociations = 0;
    std::vector<EdgePtr> potentialTargets;
    std::vector<std::size_t> arityCounts;
    bool completeEndpoints = false;
};

// The caller must attest to a complete, freshly constructed compiler DAG after
// ordinary pruning, before aliases, special pruning policies or other rewrites.
// Its active membership must agree with the original graph's pruned flags, and
// the workspace must come from that view's initial graph summary. All sources
// occur once in their head's raw incoming list. The helper consumes the workspace.
struct AndInputRedundancyFreshDagCertificate {
    bool completeDerivations = false;
    bool compilerAttestedDag = false;
    bool originalGraph = false;
};

inline bool canUseAndInputRedundancyFreshDag(const WorkingSubgraphView& view,
        const AndInputRedundancyWorkspace& workspace,
        const AndInputRedundancyFreshDagCertificate& certificate) {
    return certificate.completeDerivations && certificate.compilerAttestedDag && certificate.originalGraph &&
            workspace.completeEndpoints && workspace.degrees.size() == view.getNodes().size();
}

namespace detail {

struct AndInputRedundancyFreshDag {
    struct Candidate {
        EdgePtr edge;
        std::size_t inputIndex;
        NodePtr node;
    };

    AndInputRedundancyWorkspace& workspace;
    std::vector<EdgePtr> targets;
    std::unordered_map<NodePtr, EdgePtr> definitions;
    std::unordered_map<NodePtr, std::vector<NodePtr>> must;
    std::vector<NodePtr> cleanupSeeds;
    std::size_t inputAssociations;

    explicit AndInputRedundancyFreshDag(AndInputRedundancyWorkspace& workspace)
            : workspace(workspace), inputAssociations(workspace.initialInputAssociations) {
        for (const auto& edge : workspace.potentialTargets) {
            if (!supported(edge) || edge->getInputs().size() < 2) continue;
            for (const auto& node : edge->getInputs()) {
                if (definition(node)) {
                    targets.push_back(edge);
                    break;
                }
            }
        }
        std::sort(targets.begin(), targets.end(), [](const auto& left, const auto& right) {
            if (left->getId() != right->getId()) return left->getId() < right->getId();
            return std::less<const Hyperedge*>{}(left.get(), right.get());
        });
    }

    bool supported(const EdgePtr& edge) const {
        // Initial summary validated every endpoint. Fresh original ownership
        // makes the active raw sources the same edges, without repeating hashes.
        if (!edge || edge->pruned || !edge->getOutput() || edge->getOutput()->isShadow) return false;
        const auto& inputs = edge->getInputs();
        const auto& negations = edge->getBodyNegations();
        if (inputs.size() != negations.size()) return false;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (!inputs[i] || inputs[i]->isShadow || negations[i]) return false;
        }
        return true;
    }

    EdgePtr definition(const NodePtr& node) {
        if (!node || node->isFact || node->isOriginalFactNode() || node->isShadow) return {};
        const auto degree = workspace.degrees.find(node);
        if (degree == workspace.degrees.end() || degree->second.incoming != 1) return {};
        const auto cached = definitions.find(node);
        if (cached != definitions.end()) return cached->second;
        EdgePtr source;
        for (const auto& edge : node->getIncomingEdges()) {
            if (!edge || edge->pruned) continue;
            if (source || edge->getOutput() != node) {
                source.reset();
                break;
            }
            source = edge;
        }
        if (source && (!supported(source) || source->getInputs().empty() || !source->isDeterministic() ||
                              !source->getProbabilisticSupportTokens().empty())) source.reset();
        definitions.emplace(node, source);
        return source;
    }

    const std::vector<NodePtr>& mustFor(const NodePtr& node) {
        static const std::vector<NodePtr> empty;
        if (!node || node->isFact || node->isOriginalFactNode() || node->isShadow) return empty;
        const auto degree = workspace.degrees.find(node);
        if (degree == workspace.degrees.end() || degree->second.incoming == 0) return empty;
        const auto cached = must.find(node);
        if (cached != must.end()) return cached->second;
        std::vector<NodePtr> premises;
        std::size_t sources = 0;
        for (const auto& edge : node->getIncomingEdges()) {
            if (!edge || edge->pruned) continue;
            if (edge->getOutput() != node || !supported(edge) || edge->getInputs().empty()) {
                premises.clear();
                sources = degree->second.incoming;
                break;
            }
            const auto& inputs = edge->getInputs();
            if (sources++ == 0) {
                for (const auto& input : inputs) {
                    if (std::find(premises.begin(), premises.end(), input) == premises.end()) premises.push_back(input);
                }
            } else {
                premises.erase(std::remove_if(premises.begin(), premises.end(), [&](const auto& premise) {
                    return std::find(inputs.begin(), inputs.end(), premise) == inputs.end();
                }), premises.end());
            }
        }
        if (sources != degree->second.incoming) premises.clear();
        return must.emplace(node, std::move(premises)).first->second;
    }

    bool prove(const EdgePtr& edge, std::size_t inputIndex) {
        const auto& inputs = edge->getInputs();
        if (inputs.size() < 2 || inputIndex >= inputs.size()) return false;
        const auto source = definition(inputs[inputIndex]);
        if (!source) return false;
        for (const auto& premise : source->getInputs()) {
            bool covered = false;
            for (std::size_t other = 0; other < inputs.size(); ++other) {
                if (other == inputIndex) continue;
                if (inputs[other] == premise) {
                    covered = true;
                    break;
                }
                const auto& necessary = mustFor(inputs[other]);
                if (std::find(necessary.begin(), necessary.end(), premise) != necessary.end()) {
                    covered = true;
                    break;
                }
            }
            if (!covered) return false;
        }
        return true;
    }

    void detect(std::vector<Candidate>& candidates) {
        candidates.clear();
        for (const auto& edge : targets) {
            const auto& inputs = edge->getInputs();
            for (std::size_t occurrence = 0; occurrence < inputs.size(); ++occurrence) {
                if (prove(edge, occurrence)) candidates.push_back({edge, occurrence, inputs[occurrence]});
            }
        }
    }

    bool erase(const Candidate& candidate) {
        const auto& inputs = candidate.edge->getInputs();
        const auto degree = workspace.degrees.find(candidate.node);
        if (candidate.inputIndex >= inputs.size() || inputs[candidate.inputIndex] != candidate.node ||
                degree == workspace.degrees.end() || degree->second.outgoing == 0 ||
                !prove(candidate.edge, candidate.inputIndex)) return false;
        const auto oldArity = inputs.size();
        if (!candidate.edge->eraseInputOccurrence(candidate.inputIndex)) return false;
        --inputAssociations;
        --workspace.arityCounts[oldArity];
        ++workspace.arityCounts[oldArity - 1];
        if (--degree->second.outgoing == 0) cleanupSeeds.push_back(candidate.node);
        const auto cached = must.find(candidate.edge->getOutput());
        if (cached != must.end() && std::find(inputs.begin(), inputs.end(), candidate.node) == inputs.end()) {
            auto& premises = cached->second;
            premises.erase(std::remove(premises.begin(), premises.end(), candidate.node), premises.end());
        }
        return true;
    }

    void prepareCleanup(AndInputRedundancyCleanupPlan& plan) {
        plan = {};
        plan.finalInputAssociations = inputAssociations;
        for (std::size_t current = 0; current < cleanupSeeds.size(); ++current) {
            const auto node = cleanupSeeds[current];
            if (node->needOutput || node->hasEvidence()) continue;
            const auto degree = workspace.degrees.find(node);
            if (degree == workspace.degrees.end() || node->isShadow || degree->second.outgoing != 0) {
                plan = {{}, {}, true};
                return;
            }
            if ((!node->isFact && degree->second.incoming > 1) || (node->isFact && degree->second.incoming > 0)) {
                ++plan.removedDisjunctionNodes;
            }
            std::size_t sources = 0;
            for (const auto& edge : node->getIncomingEdges()) {
                if (!edge || edge->pruned) continue;
                if (edge->getOutput() != node || !supported(edge)) {
                    plan = {{}, {}, true};
                    return;
                }
                ++sources;
                plan.edges.push_back(edge);
                const auto arity = edge->getInputs().size();
                plan.finalInputAssociations -= arity;
                --workspace.arityCounts[arity];
                for (const auto& input : edge->getInputs()) {
                    const auto inputDegree = workspace.degrees.find(input);
                    if (inputDegree == workspace.degrees.end() || inputDegree->second.outgoing == 0) {
                        plan = {{}, {}, true};
                        return;
                    }
                    if (--inputDegree->second.outgoing == 0) cleanupSeeds.push_back(input);
                }
            }
            if (sources != degree->second.incoming) {
                plan = {{}, {}, true};
                return;
            }
            plan.nodes.push_back(node);
            workspace.degrees.erase(degree);
        }
        for (const auto& [node, degree] : workspace.degrees) {
            (void)node;
            plan.maxInDegree = std::max(plan.maxInDegree, degree.incoming);
            plan.maxOutDegree = std::max(plan.maxOutDegree, degree.outgoing);
        }
        for (auto arity = workspace.arityCounts.size(); arity > 0; --arity) {
            if (workspace.arityCounts[arity - 1] != 0) {
                plan.maxHyperedgeInputs = arity - 1;
                break;
            }
        }
        plan.hasSummary = true;
    }
};

}  // namespace detail

inline AndInputRedundancyPassStats eliminateAndInputRedundancyFreshDag(WorkingSubgraphView& view,
        AndInputRedundancyWorkspace& workspace,
        const AndInputRedundancyFreshDagCertificate& certificate = {},
        AndInputRedundancyCleanupPlan* cleanup = nullptr) {
    if (!canUseAndInputRedundancyFreshDag(view, workspace, certificate)) {
        return eliminateAndInputRedundancy(view, certificate.completeDerivations, cleanup);
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto elapsedMs = [](Clock::time_point since) {
        return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    };
    if (cleanup) *cleanup = {};
    AndInputRedundancyPassStats stats;
    detail::AndInputRedundancyFreshDag analysis(workspace);
    stats.detectionMs = elapsedMs(start);
    stats.initialInputAssociations = workspace.initialInputAssociations;
    std::vector<detail::AndInputRedundancyFreshDag::Candidate> candidates;
    std::unordered_set<EdgePtr> affected;
    while (true) {
        const auto detectionStart = Clock::now();
        analysis.detect(candidates);
        ++stats.rounds;
        stats.detectionMs += elapsedMs(detectionStart);
        stats.remainingInputAssociations = candidates.size();
        if (candidates.empty()) break;
        const auto mutationStart = Clock::now();
        std::size_t deleted = 0;
        for (std::size_t begin = 0; begin < candidates.size();) {
            auto end = begin + 1;
            while (end < candidates.size() && candidates[end].edge == candidates[begin].edge) ++end;
            for (auto current = end; current > begin;) {
                const auto& candidate = candidates[--current];
                if (!analysis.erase(candidate)) continue;
                ++deleted;
                ++stats.deletedInputAssociations;
                affected.insert(candidate.edge);
            }
            begin = end;
        }
        if (deleted != 0) view.invalidateCaches();
        stats.mutationMs += elapsedMs(mutationStart);
        if (deleted == 0) break;
    }
    stats.affectedEdges = affected.size();
    stats.finalInputAssociations = analysis.inputAssociations;
    if (cleanup && stats.deletedInputAssociations != 0) {
        const auto cleanupStart = Clock::now();
        analysis.prepareCleanup(*cleanup);
        stats.cleanupPlanningMs = elapsedMs(cleanupStart);
    }
    stats.totalMs = elapsedMs(start);
    return stats;
}

}  // namespace souffle::problog
