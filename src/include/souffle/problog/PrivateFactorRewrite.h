#pragma once

#include "souffle/problog/AndInputRedundancy.h"

#include <chrono>
#include <cmath>
#include <stdexcept>
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
    // The fused engine materializes only compounds surviving both stages.
    // stats.addedEdges instead counts virtual survivors of the series stage.
    std::vector<EdgePtr> compoundEdges;
};

struct TerminalQueryFactorRecord {
    // An output calculation, never an independent fact or event-alias binding.
    NodePtr query;
    NodePtr parent;
    double factor = 1.0;
    // Local source provenance; parent-chain factors keep their own records.
    std::vector<SupportToken> supportTokens;
};

struct TerminalQueryFactorStats {
    std::size_t nodes = 0, edges = 0;
    std::size_t candidateQueries = 0, factoredQueries = 0, factoredOutputQueries = 0;
    std::size_t hiddenChainSteps = 0, removedNodes = 0, removedEdges = 0, promotedRoots = 0;
    std::size_t skippedSupportOverlap = 0, skippedMissingSupport = 0, skippedRecursive = 0;
    std::size_t skippedOwnerHistory = 0;
    double analysisMs = 0.0, mutationMs = 0.0, totalMs = 0.0;
};

struct TerminalQueryFactorResult {
    std::vector<TerminalQueryFactorRecord> records;
    // Include intermediate promotions too; their output calculations are hidden.
    std::vector<NodePtr> promotedOutputRoots;
    TerminalQueryFactorStats stats;
};

struct PrivateFactorRewriteStats {
    std::size_t collectionPasses = 0, sccPasses = 0, supportPasses = 0, retirementBatches = 0;
    std::size_t nodesBefore = 0, edgesBefore = 0, inputAssociationsBefore = 0;
    std::size_t nodesAfterSeries = 0, edgesAfterSeries = 0, inputAssociationsAfterSeries = 0;
    std::size_t nodesAfter = 0, edgesAfter = 0, inputAssociationsAfter = 0;
    std::size_t virtualCompoundEdges = 0, materializedCompoundEdges = 0, terminalVirtualSources = 0;
    std::size_t retiredActiveNodes = 0, retiredActiveEdges = 0;
    std::size_t removedOwnerNodes = 0, removedOwnerEdges = 0, zeroHitOwnerCommits = 0;
    std::size_t ownerNodesBefore = 0, ownerEdgesBefore = 0, ownerNodesAfter = 0, ownerEdgesAfter = 0;
    std::size_t ownerEdgesExamined = 0, touchedOwnerNodes = 0, terminalViewCommits = 0;
    double preparationMs = 0.0, seriesPlanMs = 0.0, terminalPlanMs = 0.0;
    double mutationMs = 0.0, ownerCommitMs = 0.0, totalMs = 0.0;
};

struct PrivateFactorRewriteResult {
    LocalSeriesContractionResult series;
    TerminalQueryFactorResult terminal;
    PrivateFactorRewriteStats stats;
};

// Build one complete active source/body/support index, plan both rewrites in
// that index, then commit once. A pipeline-certified residual view can retire
// inactive owner history at that final commit. Standalone callers fail closed
// if a retiring node has unknown owner sources or consumers outside the view.
// TerminalView is reserved for the final standalone-full solve: it preserves
// unrelated history and prevents a later owner prune from reviving that history.
inline PrivateFactorRewriteResult rewritePrivateFactors(WorkingDerivationGraph& graph,
        WorkingSubgraphView& view, bool enableSeries, bool enableTerminal,
        bool completeDerivations = false, bool certifiedActiveView = false,
        WorkingDerivationGraph::OwnerCommitMode ownerMode = WorkingDerivationGraph::OwnerCommitMode::CompleteOwner) {
    using Snapshot = detail::AndInputRedundancySnapshot;
    constexpr auto none = Snapshot::none;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    if (ownerMode == WorkingDerivationGraph::OwnerCommitMode::TerminalView && !certifiedActiveView) {
        throw std::logic_error("Terminal-view rewrite requires a certified final full-inference view");
    }
    const auto elapsed = [](Clock::time_point from) {
        return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };
    PrivateFactorRewriteResult result;
    auto& stats = result.stats;
    auto& series = result.series;
    auto& terminal = result.terminal;
    stats.ownerNodesBefore = stats.ownerNodesAfter = graph.getNodes().size();
    stats.ownerEdgesBefore = stats.ownerEdgesAfter = graph.getEdges().size();
    stats.nodesBefore = stats.nodesAfterSeries = stats.nodesAfter = series.stats.nodesBefore = view.getNodes().size();
    stats.edgesBefore = stats.edgesAfterSeries = stats.edgesAfter = series.stats.edgesBefore = view.getEdges().size();
    terminal.stats.nodes = stats.nodesBefore;
    terminal.stats.edges = stats.edgesBefore;
    auto incomplete = [&]() {
        stats.preparationMs = stats.totalMs = elapsed(start);
        return result;
    };
    if (!completeDerivations || (!enableSeries && !enableTerminal)) return incomplete();

    Snapshot snapshot;
    snapshot.begin(stats.nodesBefore, stats.edgesBefore, true, true, 0, stats.edgesBefore);
    std::unordered_map<SupportToken, unsigned char> owners;
    owners.reserve(stats.nodesBefore + stats.edgesBefore);
    stats.collectionPasses = stats.supportPasses = 1;
    auto countSupport = [&](double probability, const std::vector<SupportToken>& tokens) {
        if (probability > 0 && probability < 1 && tokens.empty()) ++series.stats.unknownRandomEvents;
        if (!tokens.empty()) ++series.stats.supportOwners;
        for (const auto token : tokens) {
            auto& count = owners[token];
            if (count < 2) ++count;
        }
    };
    for (const auto& node : view.getNodes()) {
        if (!node || !graph.getNodes().count(node) || !std::isfinite(node->getProbability()) ||
                node->getProbability() < 0 || node->getProbability() > 1) return incomplete();
        snapshot.addNode(node);
        countSupport(node->isFact ? node->getProbability() : 1.0, node->getProbabilisticSupportTokens());
    }
    snapshot.finishNodes();
    for (const auto& edge : view.getEdges()) {
        if (!edge || !graph.getEdges().count(edge) || edge->getInputs().size() != edge->getBodyNegations().size() ||
                !std::isfinite(edge->getProbability()) || edge->getProbability() < 0 || edge->getProbability() > 1) {
            return incomplete();
        }
        snapshot.addEdge(edge);
        countSupport(edge->getProbability(), edge->getProbabilisticSupportTokens());
    }
    snapshot.finishSources();
    stats.inputAssociationsBefore = stats.inputAssociationsAfterSeries = stats.inputAssociationsAfter = snapshot.bodies.size();
    series.stats.inputAssociationsBefore = series.stats.inputAssociationsAfter = snapshot.bodies.size();
    series.stats.supportTokens = owners.size();
    if (!snapshot.complete) return incomplete();
    series.completeDerivations = true;
    const bool completeSupportMetadata = series.stats.unknownRandomEvents == 0;

    // Build consumer occurrence CSR once. SCC and the series work queue share
    // this forward index; incoming source CSR is already in the snapshot.
    std::vector<std::size_t> forwardOffsets(snapshot.nodes.size() + 1, 0);
    for (std::size_t node = 0; node < snapshot.nodes.size(); ++node) {
        forwardOffsets[node + 1] = forwardOffsets[node] + snapshot.outgoingOccurrences[node];
    }
    auto cursor = forwardOffsets;
    std::vector<std::size_t> forward(snapshot.bodies.size());
    for (std::size_t index = 0; index < snapshot.edges.size(); ++index) {
        auto& edge = snapshot.edges[index];
        // Positive-only AND eligibility is narrower than exact substitution.
        edge.safe = snapshot.safe[edge.head];
        for (auto position = edge.body.begin; position < edge.body.end; ++position) {
            const auto node = snapshot.bodies[position];
            edge.safe = edge.safe && snapshot.safe[node];
            forward[cursor[node]++] = index;
        }
    }
    snapshot.excludeRecursiveNodes(&forwardOffsets, &forward);
    stats.sccPasses = 1;
    series.stats.recursiveNodes = snapshot.baseStats.recursiveNodes;

    struct VirtualEdge {
        EdgePtr original;
        std::size_t head;
        Snapshot::Range body;
        double probability;
        bool safe, active = true;
        std::vector<bool> signs;
        bool privateFactors = false, hasSupport = false;
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
        const auto& support = edge.edge->getProbabilisticSupportTokens();
        record.privateFactors = std::all_of(support.begin(), support.end(),
                [&](SupportToken token) { return owners.at(token) == 1; });
        record.hasSupport = !support.empty();
    }
    const auto initialEdges = edges.size();
    std::vector<std::size_t> source(snapshot.nodes.size(), none);
    auto outgoing = snapshot.outgoingOccurrences;
    for (std::size_t node = 0; node < snapshot.nodes.size(); ++node) {
        if (snapshot.incomingOffsets[node + 1] - snapshot.incomingOffsets[node] == 1) {
            source[node] = snapshot.incomingEdges[snapshot.incomingOffsets[node]];
        }
    }
    struct Occurrence { std::size_t edge, next; };
    std::vector<Occurrence> newOccurrences;
    std::vector<std::size_t> newHeads(snapshot.nodes.size(), none);
    std::vector<unsigned char> live(snapshot.nodes.size(), 1), queued(snapshot.nodes.size(), 0);
    std::vector<unsigned char> seenCandidate(snapshot.nodes.size(), 0), ownerClosed(snapshot.nodes.size(), 0);
    std::vector<std::size_t> queue, touched, marks(snapshot.nodes.size(), 0), factorStack;
    std::size_t markEpoch = 0;
    auto closedOwner = [&](std::size_t node) {
        if (certifiedActiveView) return true;
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
    auto collectFactors = [&](std::size_t index, bool normalize) {
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
        if (normalize) sortUniqueSupportTokens(supports);
        return supports;
    };
    stats.preparationMs = elapsed(start);

    const auto seriesStart = Clock::now();
    if (enableSeries && completeSupportMetadata) {
        auto eligible = [&](std::size_t node) {
            const auto& value = snapshot.nodes[node];
            return live[node] && source[node] != none && outgoing[node] == 1 && snapshot.safe[node] &&
                    !snapshot.fact[node] && !value->isShadow && !value->needOutput && !value->isQuery && !value->hasEvidence();
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
        std::unordered_map<std::size_t, unsigned char> longBodySigns;
        const auto bodyBudget = series.stats.inputAssociationsBefore > (none - 4096) / 4
                ? none : series.stats.inputAssociationsBefore * 4 + 4096;
        for (std::size_t current = 0; current < queue.size(); ++current) {
            const auto middle = queue[current];
            queued[middle] = 0;
            if (!eligible(middle)) continue;
            if (!seenCandidate[middle]) { seenCandidate[middle] = 1; ++series.stats.candidates; }
            const auto into = source[middle], out = soleConsumer(middle);
            if (out == none || into == out || !edges[into].active || !edges[into].safe || !edges[out].safe) continue;
            const auto& first = edges[into];
            const auto& second = edges[out];
            const auto bodyWork = first.body.end - first.body.begin + 2 * (second.body.end - second.body.begin);
            if (bodyWork > bodyBudget - series.stats.bodyOccurrencesAnalyzed) {
                ++series.stats.rejectedWorkBudget;
                continue;
            }
            series.stats.bodyOccurrencesAnalyzed += bodyWork;
            std::size_t middlePosition = none;
            for (auto position = second.body.begin; position < second.body.end; ++position) {
                if (snapshot.bodies[position] == middle) middlePosition = position - second.body.begin;
            }
            if (middlePosition == none || second.negative(middlePosition)) continue;
            if (!closedOwner(middle)) { ++series.stats.rejectedOwnerUses; continue; }
            if (!first.privateFactors || !second.privateFactors) { ++series.stats.rejectedPrivateSupport; continue; }
            const double probability = first.probability * second.probability;
            if (probability == 0 && first.probability > 0 && second.probability > 0) continue;
            const auto head = second.head;
            const bool hasSupport = first.hasSupport || second.hasSupport;
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
                if (duplicate) ++series.stats.duplicateInputsRemoved;
                else { snapshot.bodies.push_back(node); signs.push_back(negative); }
            };
            for (auto i = second.body.begin; i < second.body.end; ++i) {
                if (i - second.body.begin != middlePosition) append(snapshot.bodies[i], second.negative(i - second.body.begin));
                else for (auto j = first.body.begin; j < first.body.end; ++j) {
                    append(snapshot.bodies[j], first.negative(j - first.body.begin));
                }
            }
            const auto mergedBody = Snapshot::Range{bodyBegin, snapshot.bodies.size()};
            ++series.stats.contractions;
            const auto random = [](double p) { return p > 0 && p < 1 ? 1U : 0U; };
            series.stats.ruleVariablesRemoved += random(first.probability) + random(second.probability) - random(probability);
            series.stats.inputAssociationsAfter -= first.body.end - first.body.begin + second.body.end - second.body.begin;
            series.stats.inputAssociationsAfter += mergedBody.end - mergedBody.begin;
            touched.clear();
            Snapshot::nextEpoch(markEpoch, marks);
            auto touch = [&](std::size_t node) {
                if (marks[node] != markEpoch) { marks[node] = markEpoch; touched.push_back(node); }
            };
            for (const auto index : {into, out}) {
                const auto body = edges[index].body;
                for (auto i = body.begin; i < body.end; ++i) { --outgoing[snapshot.bodies[i]]; touch(snapshot.bodies[i]); }
                edges[index].active = false;
                if (index < initialEdges) ++series.stats.removedEdges;
                else --series.stats.addedEdges;
            }
            const auto replacement = edges.size();
            edges.push_back({nullptr, head, mergedBody, probability, true, true, std::move(signs), true, hasSupport, into, out});
            ++series.stats.addedEdges;
            for (auto i = mergedBody.begin; i < mergedBody.end; ++i) {
                const auto node = snapshot.bodies[i];
                ++outgoing[node];
                newOccurrences.push_back({replacement, newHeads[node]});
                newHeads[node] = newOccurrences.size() - 1;
                touch(node);
            }
            live[middle] = 0;
            source[middle] = none;
            series.removedNodes.push_back(snapshot.nodes[middle]);
            if (source[head] != none) source[head] = replacement;
            touch(head);
            for (const auto node : touched) enqueue(node);
        }
    }
    stats.seriesPlanMs = series.stats.analysisMs = series.stats.totalMs = elapsed(seriesStart);
    series.stats.removedNodes = series.removedNodes.size();
    stats.virtualCompoundEdges = series.stats.contractions;
    stats.nodesAfterSeries = stats.nodesBefore - series.stats.removedNodes;
    stats.edgesAfterSeries = stats.edgesBefore - series.stats.removedEdges + series.stats.addedEdges;
    stats.inputAssociationsAfterSeries = series.stats.inputAssociationsAfter;
    stats.nodesAfter = stats.nodesAfterSeries;
    stats.edgesAfter = stats.edgesAfterSeries;
    stats.inputAssociationsAfter = stats.inputAssociationsAfterSeries;
    terminal.stats.nodes = stats.nodesAfterSeries;
    terminal.stats.edges = stats.edgesAfterSeries;

    const auto terminalStart = Clock::now();
    std::vector<unsigned char> promoted(snapshot.nodes.size(), 0);
    std::vector<std::size_t> terminalSources;
    if (enableTerminal) {
        queue.clear(); // Series consumed its queue and reset every queued flag.
        // Series no longer needs its epoch marks. Reuse this dense array for
        // node index -> terminal record index, avoiding a second pointer map.
        std::fill(marks.begin(), marks.end(), none);
        auto enqueue = [&](std::size_t node) {
            if (live[node] && !queued[node] && (snapshot.nodes[node]->needOutput || promoted[node]) && outgoing[node] == 0) {
                queue.push_back(node);
                queued[node] = 1;
            }
        };
        for (std::size_t node = 0; node < snapshot.nodes.size(); ++node) enqueue(node);
        for (std::size_t current = 0; current < queue.size(); ++current) {
            const auto nodeIndex = queue[current];
            const auto& query = snapshot.nodes[nodeIndex];
            ++terminal.stats.candidateQueries;
            if (snapshot.fact[nodeIndex] || query->isShadow || query->hasEvidence() ||
                    !query->getProbabilisticSupportTokens().empty() || source[nodeIndex] == none) continue;
            const auto edgeIndex = source[nodeIndex];
            const auto& edge = edges[edgeIndex];
            if (!edge.active || edge.body.end - edge.body.begin != 1 || edge.negative(0)) continue;
            const auto parentIndex = snapshot.bodies[edge.body.begin];
            const auto& parent = snapshot.nodes[parentIndex];
            if (parent->isShadow || parentIndex == nodeIndex || !live[parentIndex]) continue;
            if (!snapshot.safe[nodeIndex] || !snapshot.safe[parentIndex]) { ++terminal.stats.skippedRecursive; continue; }
            const auto factor = edge.probability;
            if (factor > 0 && factor < 1 && (!edge.hasSupport || !completeSupportMetadata)) {
                ++terminal.stats.skippedMissingSupport;
                continue;
            }
            if (!edge.privateFactors) { ++terminal.stats.skippedSupportOverlap; continue; }
            if ((factor == 0 || factor == 1) && edge.hasSupport) continue;
            if (!closedOwner(nodeIndex)) { ++terminal.stats.skippedOwnerHistory; continue; }
            live[nodeIndex] = 0;
            source[nodeIndex] = none;
            edges[edgeIndex].active = false;
            marks[nodeIndex] = terminal.records.size();
            terminal.records.push_back({query, parent, factor, {}});
            terminalSources.push_back(edgeIndex);
            if (edgeIndex >= initialEdges) ++stats.terminalVirtualSources;
            if (query->needOutput) ++terminal.stats.factoredOutputQueries;
            else ++terminal.stats.hiddenChainSteps;
            --outgoing[parentIndex];
            if (!parent->needOutput && !promoted[parentIndex]) {
                promoted[parentIndex] = 1;
                terminal.promotedOutputRoots.push_back(parent);
            }
            enqueue(parentIndex);
        }
        // Parent records occur after children. One reverse scan flattens every
        // output calculation without walking its chain again for each query.
        for (std::size_t i = terminal.records.size(); i-- > 0;) {
            auto& record = terminal.records[i];
            const auto parentIndex = snapshot.bodies[edges[terminalSources[i]].body.begin];
            const auto parentRecord = marks[parentIndex];
            if (parentRecord != none) {
                const auto& resolved = terminal.records[parentRecord];
                record.parent = resolved.parent;
                record.factor *= resolved.factor;
            }
            record.supportTokens = collectFactors(terminalSources[i], true);
        }
    }
    stats.terminalPlanMs = terminal.stats.analysisMs = terminal.stats.totalMs = elapsed(terminalStart);
    terminal.stats.factoredQueries = terminal.stats.removedNodes = terminal.stats.removedEdges = terminal.records.size();
    terminal.stats.promotedRoots = terminal.promotedOutputRoots.size();
    stats.nodesAfter -= terminal.records.size();
    stats.edgesAfter -= terminal.records.size();
    stats.inputAssociationsAfter -= terminal.records.size();
    stats.retiredActiveNodes = series.removedNodes.size() + terminal.records.size();
    if (stats.retiredActiveNodes == 0) { stats.totalMs = elapsed(start); return result; }

    const auto mutationStart = Clock::now();
    std::vector<NodePtr> retiredNodes = series.removedNodes;
    retiredNodes.reserve(stats.retiredActiveNodes);
    for (const auto& record : terminal.records) retiredNodes.push_back(record.query);
    std::vector<EdgePtr> retiredEdges;
    retiredEdges.reserve(series.stats.removedEdges + terminal.records.size());
    for (std::size_t index = 0; index < initialEdges; ++index) {
        if (!edges[index].active) retiredEdges.push_back(edges[index].original);
    }
    std::vector<WorkingDerivationGraph::RewriteEdgeSpec> compoundSpecs;
    compoundSpecs.reserve(series.stats.addedEdges - stats.terminalVirtualSources);
    for (auto index = initialEdges; index < edges.size(); ++index) {
        const auto& edge = edges[index];
        if (!edge.active) continue;
        WorkingDerivationGraph::RewriteEdgeSpec spec;
        spec.inputs.reserve(edge.body.end - edge.body.begin);
        for (auto i = edge.body.begin; i < edge.body.end; ++i) spec.inputs.push_back(snapshot.nodes[snapshot.bodies[i]]);
        spec.output = snapshot.nodes[edge.head];
        spec.probability = edge.probability;
        spec.bodyNegations = edge.signs;
        spec.supportTokens = collectFactors(index, false); // The owner setter normalizes once.
        compoundSpecs.push_back(std::move(spec));
    }
    stats.retiredActiveEdges = retiredEdges.size();
    stats.materializedCompoundEdges = compoundSpecs.size();
    const auto commitStart = Clock::now();
    auto committed = graph.commitRewriteView(view, retiredNodes, retiredEdges,
            compoundSpecs, certifiedActiveView, ownerMode);
    series.compoundEdges = std::move(committed.insertedEdges);
    stats.ownerCommitMs = elapsed(commitStart);
    stats.removedOwnerNodes = committed.stats.removedOwnerNodes;
    stats.removedOwnerEdges = committed.stats.removedOwnerEdges;
    stats.ownerEdgesExamined = committed.stats.ownerEdgesExamined;
    stats.touchedOwnerNodes = committed.stats.touchedOwnerNodes;
    stats.ownerNodesAfter = graph.getNodes().size();
    stats.ownerEdgesAfter = graph.getEdges().size();
    stats.terminalViewCommits = ownerMode == WorkingDerivationGraph::OwnerCommitMode::TerminalView ? 1 : 0;
    stats.retirementBatches = 1;
    for (const auto& parent : terminal.promotedOutputRoots) {
        if (view.getNodes().count(parent)) parent->needOutput = true;
    }
    stats.mutationMs = elapsed(mutationStart);
    stats.totalMs = elapsed(start);
    return result;
}

}  // namespace souffle::problog
