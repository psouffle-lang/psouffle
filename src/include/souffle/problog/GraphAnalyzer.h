#ifndef GRAPHANALYZER_H
#define GRAPHANALYZER_H

#include "souffle/problog/DerivationGraph.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>

enum class SISORegionKind {
    Unknown = 0,
    General,
    SingleHyperedge,  // single hyperedge from SI to SO
    LinearTwoEdge,    // SI -> mid -> SO (two edges chain)
    ParallelEdge,  // >=2 parallel single-input edges SI -> SO
    AllFactsToSO,     // single edge, inputs all facts (no incoming edges or evidence/output)
    FanOutConverge,   // SI fan-out to xi, then single AND edge xi... -> SO (polarity-aware)
};

struct SISORegionInfo {
    std::vector<NodePtr> internalNodes;      // All nodes in the region (including SI / SO)
    std::vector<EdgePtr> internalEdges;      // All edges in the region (edges on SI->SO paths)
    NodePtr entry = nullptr;                 // SI node (entry)
    std::vector<NodePtr> entryPreds;         // External predecessors of SI (may be empty)
    NodePtr exit = nullptr;                  // SO node (exit)
    bool valid = false;
    bool prefixAllFactsRequired = false;     // Keep this flag for now; may use support for finer checks later
    SISORegionKind kind = SISORegionKind::Unknown;  // SISO kind tag for fast-path handling
};

class GraphAnalyzer {
private:
    using NodeSet     = std::unordered_set<NodePtr>;
    using EdgeSet     = std::unordered_set<EdgePtr>;

    static std::string escapeDot(const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (c == '"') r.push_back('\'');
            else if (c == '\\') {
                r.push_back('\\');
                r.push_back('\\');
            } else {
                r.push_back(c);
            }
        }
        return r;
    }

    static SISORegionInfo makeRegion(NodePtr entry, NodePtr exit, const std::vector<EdgePtr>& edges,
            SISORegionKind kind) {
        SISORegionInfo info;
        if (!entry || !exit) return info;
        info.entry = entry;
        info.exit = exit;
        info.entryPreds = {};  // not used in fast path
        info.internalEdges.assign(edges.begin(), edges.end());
        // collect nodes from edges plus entry/exit
        NodeSet nodeSet;
        nodeSet.insert(entry);
        nodeSet.insert(exit);
        for (auto e : edges) {
            if (!e) continue;
            nodeSet.insert(e->getInputs().begin(), e->getInputs().end());
            NodePtr out = e->getOutput();
            if (out) nodeSet.insert(out);
        }
        info.internalNodes.assign(nodeSet.begin(), nodeSet.end());
        info.valid = true;
        info.kind = kind;
        return info;
    }

    struct FastPathDetectStats {
        long singleHyperedgeMs = 0;
        long linearTwoEdgeMs   = 0;
        long parallelTwoEdgeMs = 0;
        long allFactsToSOMs    = 0;
        long fanOutConvergeMs  = 0;
        long generalBoundedMs  = 0;
        long semanticFactStatsMs = 0;
        size_t singleHyperedgeCount = 0;
        size_t linearTwoEdgeCount   = 0;
        size_t parallelTwoEdgeCount = 0;
        size_t allFactsToSOCount    = 0;
        size_t fanOutConvergeCount  = 0;
        size_t generalBoundedCount  = 0;
        size_t generalBoundedCandidates = 0;
    };

public:
    struct FastPathDetectOptions {
        bool enableSingleHyperedge = true;
        bool enableLinearTwoEdge = true;
        bool enableParallelEdge = true;
        bool enableAllFactsToSO = true;
        bool enableFanOutConverge = true;
        bool enableGeneral = false;
        size_t maxGeneralNodes = 8;
        size_t maxGeneralEdges = 5;
        size_t maxGeneralExitIncoming = 4;
        size_t maxGeneralRegionsPerDetect = 256;
        size_t maxGeneralCandidateEntries = 16;
        bool requireGeneralRandomVariable = true;
    };

private:
    struct DetectionFrontier {
        NodeSet nodes;
        EdgeSet edges;
    };

    static bool isNodeInView(const DerivationGraphViewInterface& g, NodePtr n) {
        return n && g.getNodes().count(n) > 0;
    }

    static bool isEdgeInView(const DerivationGraphViewInterface& g, EdgePtr e) {
        return e && g.getEdges().count(e) > 0;
    }

    static void addEdgeEndpoints(const DerivationGraphViewInterface& g, EdgePtr e, NodeSet& nodes) {
        if (!isEdgeInView(g, e)) return;
        NodePtr out = g.getOutput(e);
        if (isNodeInView(g, out)) {
            nodes.insert(out);
        }
        for (auto in : e->getInputs()) {
            if (isNodeInView(g, in)) {
                nodes.insert(in);
            }
        }
    }

    static DetectionFrontier buildDetectionFrontier(
            const DerivationGraphViewInterface& g,
            const NodeSet& dirtyNodes,
            const EdgeSet& dirtyEdges) {
        DetectionFrontier frontier;

        for (auto n : dirtyNodes) {
            if (isNodeInView(g, n)) {
                frontier.nodes.insert(n);
            }
        }
        for (auto e : dirtyEdges) {
            if (!isEdgeInView(g, e)) continue;
            frontier.edges.insert(e);
        }
        for (auto e : frontier.edges) {
            addEdgeEndpoints(g, e, frontier.nodes);
        }

        // One-hop expansion around dirty seeds is enough for the current fast
        // paths: we seed rewritten edge endpoints explicitly, so a single local
        // expansion already reaches the adjacent chain/fan-out structure needed
        // to re-detect follow-on opportunities. A second round makes large
        // all-facts waves in DDisasm-like hosts balloon back toward full-graph
        // scans, dominating rewrite time without exposing additional regions in
        // the maintained workloads we care about.
        constexpr int kExpandRounds = 1;
        for (int round = 0; round < kExpandRounds; ++round) {
            std::vector<NodePtr> nodeSnapshot(frontier.nodes.begin(), frontier.nodes.end());
            for (auto n : nodeSnapshot) {
                if (!isNodeInView(g, n)) continue;
                for (auto inEdge : g.getIncomingEdges(n)) {
                    if (!isEdgeInView(g, inEdge)) continue;
                    frontier.edges.insert(inEdge);
                    addEdgeEndpoints(g, inEdge, frontier.nodes);
                }
                for (auto outEdge : g.getOutgoingEdges(n)) {
                    if (!isEdgeInView(g, outEdge)) continue;
                    frontier.edges.insert(outEdge);
                    addEdgeEndpoints(g, outEdge, frontier.nodes);
                }
            }
        }

        return frontier;
    }

    struct SemanticFactUseStats {
        std::unordered_map<std::size_t, std::size_t> inputOccurrences;
        std::unordered_map<std::size_t, std::size_t> pinnedNodes;
    };

    static bool isProbabilisticFact(const NodePtr& node) {
        return node && node->isFact && node->getProbability() > 0.0 && node->getProbability() < 1.0;
    }

    static bool isProbabilisticEdge(const EdgePtr& edge) {
        return edge && edge->getProbability() > 0.0 && edge->getProbability() < 1.0;
    }

    static bool hasInternalRandomVariable(
            const NodeSet& nodes, const EdgeSet& edges, NodePtr entry, NodePtr exit) {
        for (EdgePtr edge : edges) {
            if (isProbabilisticEdge(edge)) {
                return true;
            }
        }
        for (NodePtr node : nodes) {
            if (node == entry || node == exit) {
                continue;
            }
            if (isProbabilisticFact(node)) {
                return true;
            }
        }
        return false;
    }

    static SemanticFactUseStats buildSemanticFactUseStats(const DerivationGraphViewInterface& g) {
        SemanticFactUseStats stats;
        for (auto node : g.getNodes()) {
            if (!isProbabilisticFact(node)) continue;
            if (node->needOutput || node->hasEvidence()) {
                ++stats.pinnedNodes[node->getSemanticFactId()];
            }
        }
        for (auto edge : g.getEdges()) {
            if (!edge) continue;
            for (auto input : edge->getInputs()) {
                if (!isProbabilisticFact(input)) continue;
                ++stats.inputOccurrences[input->getSemanticFactId()];
            }
        }
        return stats;
    }

    static bool canAbsorbFactLiteral(const DerivationGraphViewInterface& g, NodePtr node,
            const SemanticFactUseStats& semanticStats, std::size_t localOccurrences = 1) {
        if (!node || !node->isFact || node->hasEvidence() || node->needOutput) {
            return false;
        }
        if (!g.getIncomingEdges(node).empty()) {
            return false;
        }
        if (!isProbabilisticFact(node)) {
            return true;
        }
        if (!node->isOriginalFactNode()) {
            return false;
        }
        const auto semanticId = node->getSemanticFactId();
        auto pinnedIt = semanticStats.pinnedNodes.find(semanticId);
        if (pinnedIt != semanticStats.pinnedNodes.end() && pinnedIt->second > 0) {
            return false;
        }
        auto occIt = semanticStats.inputOccurrences.find(semanticId);
        return occIt != semanticStats.inputOccurrences.end() && occIt->second == localOccurrences;
    }

    static std::unordered_map<NodePtr, std::size_t> countLocalFactOccurrences(const EdgeSet& edges) {
        std::unordered_map<NodePtr, std::size_t> occurrences;
        for (EdgePtr edge : edges) {
            if (!edge) continue;
            for (NodePtr input : edge->getInputs()) {
                if (input && input->isFact) {
                    ++occurrences[input];
                }
            }
        }
        return occurrences;
    }

    static bool canAbsorbSourceFact(const DerivationGraphViewInterface& g, NodePtr node,
            const SemanticFactUseStats& semanticStats,
            const std::unordered_map<NodePtr, std::size_t>& localOccurrences) {
        auto it = localOccurrences.find(node);
        std::size_t occurrences = it == localOccurrences.end() ? 0 : it->second;
        return occurrences > 0 && canAbsorbFactLiteral(g, node, semanticStats, occurrences);
    }

    static std::vector<NodePtr> collectBoundedGeneralEntryCandidates(
            const DerivationGraphViewInterface& g, NodePtr exit, const FastPathDetectOptions& opts) {
        std::vector<NodePtr> candidates;
        NodeSet seenCandidates;
        NodeSet seenNodes;
        EdgeSet seenEdges;
        std::queue<NodePtr> worklist;
        auto addCandidate = [&](NodePtr node) {
            if (!node || node == exit) return;
            if (seenCandidates.insert(node).second) {
                candidates.push_back(node);
            }
        };

        seenNodes.insert(exit);
        worklist.push(exit);

        while (!worklist.empty() && candidates.size() < opts.maxGeneralCandidateEntries) {
            NodePtr node = worklist.front();
            worklist.pop();
            if (!isNodeInView(g, node)) continue;
            for (EdgePtr edge : g.getIncomingEdges(node)) {
                if (!isEdgeInView(g, edge)) continue;
                if (seenEdges.insert(edge).second && seenEdges.size() > opts.maxGeneralEdges * 2) {
                    break;
                }
                for (NodePtr input : edge->getInputs()) {
                    if (!isNodeInView(g, input)) continue;
                    addCandidate(input);
                    if (seenNodes.insert(input).second && seenNodes.size() <= opts.maxGeneralNodes * 2) {
                        worklist.push(input);
                    }
                }
            }
        }

        std::stable_sort(candidates.begin(), candidates.end(), [](NodePtr a, NodePtr b) {
            const bool aFact = a && a->isFact;
            const bool bFact = b && b->isFact;
            return aFact < bFact;
        });
        return candidates;
    }

    static SISORegionInfo tryBuildBoundedGeneralRegionForEntry(const DerivationGraphViewInterface& g,
            NodePtr exit, NodePtr entry, const FastPathDetectOptions& opts,
            const SemanticFactUseStats& semanticStats) {
        SISORegionInfo invalid;
        if (!isNodeInView(g, exit) || !isNodeInView(g, entry) || entry == exit) {
            return invalid;
        }

        NodeSet regionNodes;
        EdgeSet regionEdges;
        std::queue<NodePtr> worklist;
        regionNodes.insert(exit);
        worklist.push(exit);

        bool aborted = false;
        bool reachedEntry = false;
        while (!worklist.empty() && !aborted) {
            NodePtr node = worklist.front();
            worklist.pop();
            if (!isNodeInView(g, node)) {
                aborted = true;
                break;
            }
            if (node == entry) {
                reachedEntry = true;
                continue;
            }
            for (EdgePtr edge : g.getIncomingEdges(node)) {
                if (!isEdgeInView(g, edge)) continue;
                if (regionEdges.insert(edge).second && regionEdges.size() > opts.maxGeneralEdges) {
                    aborted = true;
                    break;
                }
                for (NodePtr input : edge->getInputs()) {
                    if (!isNodeInView(g, input)) {
                        aborted = true;
                        break;
                    }
                    if (regionNodes.insert(input).second) {
                        if (regionNodes.size() > opts.maxGeneralNodes) {
                            aborted = true;
                            break;
                        }
                        worklist.push(input);
                    }
                }
                if (aborted) break;
            }
        }
        if (aborted || !reachedEntry || regionEdges.empty() || regionNodes.size() <= 2) {
            return invalid;
        }

        NodeSet hasInternalIncoming;
        for (EdgePtr edge : regionEdges) {
            NodePtr out = g.getOutput(edge);
            if (out) {
                hasInternalIncoming.insert(out);
            }
        }
        auto localOccurrences = countLocalFactOccurrences(regionEdges);

        for (NodePtr node : regionNodes) {
            if (!node) {
                return invalid;
            }
            if (!hasInternalIncoming.count(node)) {
                if (node == entry) {
                    continue;
                }
                // A bounded general SISO may have several source facts. They
                // are local support for the single entry, not additional SISO
                // inputs, so they can be folded only when they are not pinned
                // and their semantic fact is not used outside this region.
                if (!canAbsorbSourceFact(g, node, semanticStats, localOccurrences)) {
                    return invalid;
                }
            }
        }
        if (opts.requireGeneralRandomVariable &&
                !hasInternalRandomVariable(regionNodes, regionEdges, entry, exit)) {
            return invalid;
        }

        for (NodePtr node : regionNodes) {
            if (!node) {
                return invalid;
            }
            if (node != entry) {
                for (EdgePtr incoming : g.getIncomingEdges(node)) {
                    if (isEdgeInView(g, incoming) && !regionEdges.count(incoming)) {
                        return invalid;
                    }
                }
            }
            if (node != entry && node != exit) {
                if (node->needOutput || node->hasEvidence()) {
                    return invalid;
                }
                for (EdgePtr outgoing : g.getOutgoingEdges(node)) {
                    if (isEdgeInView(g, outgoing) && !regionEdges.count(outgoing)) {
                        return invalid;
                    }
                }
            }
        }

        std::vector<EdgePtr> edges(regionEdges.begin(), regionEdges.end());
        return makeRegion(entry, exit, edges, SISORegionKind::General);
    }

    static SISORegionInfo tryBuildBoundedGeneralRegion(const DerivationGraphViewInterface& g,
            NodePtr exit, const FastPathDetectOptions& opts, const SemanticFactUseStats& semanticStats) {
        SISORegionInfo invalid;
        if (!isNodeInView(g, exit)) {
            return invalid;
        }
        const auto& exitIncoming = g.getIncomingEdges(exit);
        if (exitIncoming.empty() || exitIncoming.size() > opts.maxGeneralExitIncoming) {
            return invalid;
        }
        if (exitIncoming.size() == 1 && exitIncoming.front() &&
                exitIncoming.front()->getInputs().size() <= 1) {
            return invalid;
        }

        for (NodePtr entry : collectBoundedGeneralEntryCandidates(g, exit, opts)) {
            auto region = tryBuildBoundedGeneralRegionForEntry(g, exit, entry, opts, semanticStats);
            if (region.valid) {
                return region;
            }
        }
        return invalid;
    }

    // Fast-path detectors (<=2 edges).
    // If candidate sets are provided, scanning is restricted to that local frontier.
    static std::vector<SISORegionInfo> detectFastPathRegions(
            const DerivationGraphViewInterface& g,
            FastPathDetectStats* stats = nullptr,
            const EdgeSet* candidateEdges = nullptr,
            const NodeSet* candidateNodes = nullptr,
            const FastPathDetectOptions* options = nullptr) {
        FastPathDetectOptions defaultOptions;
        const FastPathDetectOptions& opts = options ? *options : defaultOptions;
        std::vector<SISORegionInfo> regions;
        std::vector<EdgePtr> edgeScan;
        if (candidateEdges) {
            edgeScan.reserve(candidateEdges->size());
            for (auto e : *candidateEdges) {
                if (isEdgeInView(g, e)) edgeScan.push_back(e);
            }
        } else {
            edgeScan.reserve(g.getEdges().size());
            for (auto e : g.getEdges()) {
                if (e) edgeScan.push_back(e);
            }
        }
        std::vector<NodePtr> nodeScan;
        if (candidateNodes) {
            nodeScan.reserve(candidateNodes->size());
            for (auto n : *candidateNodes) {
                if (isNodeInView(g, n)) nodeScan.push_back(n);
            }
        } else {
            nodeScan.reserve(g.getNodes().size());
            for (auto n : g.getNodes()) {
                if (n) nodeScan.push_back(n);
            }
        }
        auto tSemanticStart = std::chrono::steady_clock::now();
        SemanticFactUseStats semanticFactStats;
        if (opts.enableSingleHyperedge || opts.enableAllFactsToSO || opts.enableFanOutConverge ||
                opts.enableGeneral) {
            semanticFactStats = buildSemanticFactUseStats(g);
        }
        auto tSemanticEnd = std::chrono::steady_clock::now();
        if (stats) {
            stats->semanticFactStatsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    tSemanticEnd - tSemanticStart)
                                                 .count();
        }

        // 1) Single hyperedge: one edge exit, inputs.size()>=1, exactly one non-fact (SI), others are input facts
        auto tSingleStart = std::chrono::steady_clock::now();
        if (opts.enableSingleHyperedge) for (auto e : edgeScan) {
            if (!e) continue;
            const auto& inputs = e->getInputs();
            if (inputs.size() <= 1) {
                continue;
            }
            NodePtr exit = g.getOutput(e);
            if (!exit) continue;
            size_t factInputs = 0;
            NodePtr si = nullptr;
            bool invalid = false;
            for (size_t idx = 0; idx < inputs.size(); ++idx) {
                auto n = inputs[idx];
                if (!n) {
                    invalid = true;
                    break;
                }
                if (n->isFact) {
                    // Single-hyperedge rewrite collapses absorbed literals into a
                    // synthetic edge weight. This is semantics-preserving for deterministic
                    // literals, but not for probabilistic literals under the current
                    // edge-variable FC/WMC encoding, because shared support would be folded
                    // inside an edge weight rather than represented explicitly in the graph.
                    if (isProbabilisticFact(n)) {
                        invalid = true;
                        break;
                    }
                    if (!canAbsorbFactLiteral(g, n, semanticFactStats, 1)) {
                        invalid = true;
                        break;
                    }
                    const auto& support = n->getProbabilisticSupportTokens();
                    if (!support.empty() && edgeInputHasSupportOverlap(g, e, idx, support)) {
                        invalid = true;
                        break;
                    }
                    ++factInputs;
                } else if (!si) {
                    si = n;
                } else {
                    invalid = true;
                    break;  // more than one non-fact
                }
            }
            if (invalid) continue;
            if (!si) {
                continue;  // all facts handled elsewhere
            }
            if (factInputs == 0) {
                continue;  // nothing to absorb; would loop after rewrite
            }
            if (si->isFact) {
                continue;  // force SI to be non-fact to avoid overlap with all-facts
            }
            if (factInputs + 1 != inputs.size()) {
                continue;
            }
            if (si == exit) {
                continue;
            }
            auto region = makeRegion(si, exit, {e}, SISORegionKind::SingleHyperedge);
            regions.push_back(std::move(region));
            if (stats) stats->singleHyperedgeCount++;
            continue;
        }
        auto tSingleEnd = std::chrono::steady_clock::now();
        if (stats) {
            stats->singleHyperedgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    tSingleEnd - tSingleStart)
                                               .count();
        }

        // 3) Linear two-edge: entry->mid->exit, each edge single input
        auto tLinearStart = std::chrono::steady_clock::now();
        if (opts.enableLinearTwoEdge) for (auto e1 : edgeScan) {
            if (!e1) continue;
            const auto& in1 = e1->getInputs();
            if (in1.size() != 1) continue;
            const auto& neg1 = e1->getBodyNegationsStable();
            if (neg1.size() > 1) continue;  // expect single-input edge
            NodePtr entry = in1[0];
            NodePtr mid = g.getOutput(e1);
            if (!entry || !mid) continue;
            if (entry == mid) continue;
            if (mid->hasEvidence() || mid->needOutput) continue;  // mid cannot be query/evidence
            // Linear contraction deletes `mid`, so `mid` must be single-source from this edge.
            auto midIn = g.getIncomingEdges(mid);
            if (midIn.size() != 1 || midIn[0] != e1) continue;
            // mid should have exactly one outgoing edge for the chain
            const auto& midOut = g.getOutgoingEdges(mid);
            if (midOut.size() != 1) continue;
            EdgePtr e2 = midOut[0];
            if (!e2) continue;
            const auto& in2 = e2->getInputs();
            if (in2.size() != 1 || in2[0] != mid) continue;
            const auto& neg2 = e2->getBodyNegationsStable();
            if (neg2.size() > 1) continue;  // expect single-input edge
            if (!neg2.empty() && neg2[0]) continue;  // do not fast-path if mid->exit is negated
            NodePtr exit = g.getOutput(e2);
            if (!exit || exit == entry || exit == mid) continue;
            regions.push_back(makeRegion(entry, exit, {e1, e2}, SISORegionKind::LinearTwoEdge));
            if (stats) stats->linearTwoEdgeCount++;
        }
        auto tLinearEnd = std::chrono::steady_clock::now();
        if (stats) {
            stats->linearTwoEdgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    tLinearEnd - tLinearStart)
                                            .count();
        }

        // 4) Parallel edges: same SI -> same SO, each edge has exactly one input.
        // Linear-time grouping by SO and then SI to avoid O(m^2).
        auto tParallelStart = std::chrono::steady_clock::now();
        if (opts.enableParallelEdge) for (NodePtr so : nodeScan) {
            if (!so) continue;
            const auto& incoming = g.getIncomingEdges(so);
            if (incoming.size() < 2) continue;  // need at least two edges to form parallel region
            // group single-input edges by their sole input (SI) and negation flag
            std::unordered_map<NodePtr, std::array<std::vector<EdgePtr>, 2>> bySiNeg;
            for (EdgePtr e : incoming) {
                if (!e) continue;
                const auto& ins = e->getInputs();
                if (ins.size() != 1) continue;
                const auto& negs = e->getBodyNegationsStable();
                if (!negs.empty() && negs.size() != 1) continue;  // keep only single-input with aligned neg flag
                bool isNeg = (!negs.empty() && negs[0]);
                NodePtr si = ins[0];
                if (!si) continue;
                bySiNeg[si][isNeg ? 1 : 0].push_back(e);
            }
            for (auto& kv : bySiNeg) {
                NodePtr si = kv.first;
                if (si == so) continue;
                auto& groups = kv.second;
                for (int idx = 0; idx < 2; ++idx) {
                    auto& edges = groups[idx];
                    if (edges.size() < 2) continue;
                    regions.push_back(makeRegion(si, so, edges, SISORegionKind::ParallelEdge));
                    if (stats) stats->parallelTwoEdgeCount++;
                }
            }
        }
        auto tParallelEnd = std::chrono::steady_clock::now();
        if (stats) {
            stats->parallelTwoEdgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    tParallelEnd - tParallelStart)
                                              .count();
        }

        // 2) Fan-out converge (SI fact fan-out to xi, xi converge to SO via one multi-input edge)
        auto tFanStart = std::chrono::steady_clock::now();
        if (opts.enableFanOutConverge) for (NodePtr si : nodeScan) {
            if (!si) continue;
            if (!si->isFact || si->hasEvidence() || si->needOutput) continue;
            const auto& outsSi = g.getOutgoingEdges(si);
            if (outsSi.size() < 2) continue;  // need fan-out
            bool bad = false;
            std::vector<EdgePtr> fanEdges;
            std::vector<NodePtr> xiNodes;
            for (EdgePtr e : outsSi) {
                if (!e) continue;
                const auto& ins = e->getInputs();
                if (ins.size() != 1 || ins[0] != si) {
                    bad = true; break;
                }
                fanEdges.push_back(e);
                xiNodes.push_back(g.getOutput(e));
            }
            if (bad || fanEdges.size() < 2) continue;
            // ensure xi nodes are unique and only used here
            std::unordered_set<NodePtr> xiSet;
            for (NodePtr x : xiNodes) {
                if (!x) { bad = true; break; }
                if (!xiSet.insert(x).second) { bad = true; break; }
                const auto& inX = g.getIncomingEdges(x);
                const auto& outX = g.getOutgoingEdges(x);
                if (inX.size() != 1 || outX.size() != 1) { bad = true; break; }
            }
            if (bad) continue;
            // all xi must share the same convergence edge
            EdgePtr conv = nullptr;
            for (NodePtr x : xiSet) {
                const auto& outX = g.getOutgoingEdges(x);
                if (outX.empty()) { bad = true; break; }
                if (!conv) {
                    conv = outX[0];
                } else if (conv != outX[0]) {
                    bad = true; break;
                }
            }
            if (bad || !conv) continue;
            const auto& convInputs = conv->getInputs();
            if (convInputs.size() != xiSet.size()) continue;
            // inputs of conv must be exactly xi and all positive
            const auto& convNeg = conv->getBodyNegationsStable();
            if (!convNeg.empty()) {
                bool allFalse = std::all_of(convNeg.begin(), convNeg.end(), [](bool b){return !b;});
                if (!allFalse) continue;
            }
            std::unordered_set<NodePtr> convInSet(convInputs.begin(), convInputs.end());
            if (convInSet != xiSet) continue;
            if (!canAbsorbFactLiteral(g, si, semanticFactStats, fanEdges.size())) continue;
            NodePtr so = g.getOutput(conv);
            if (!so || so == si) continue;
            // build region: all fan edges + conv edge
            std::vector<EdgePtr> regEdges = fanEdges;
            regEdges.push_back(conv);
            auto region = makeRegion(si, so, regEdges, SISORegionKind::FanOutConverge);
            regions.push_back(std::move(region));
            if (stats) stats->fanOutConvergeCount++;
        }
        auto tFanEnd = std::chrono::steady_clock::now();
        if (stats) {
            stats->fanOutConvergeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    tFanEnd - tFanStart)
                                              .count();
        }

        // 2) All-facts single hyperedge: single edge with all fact inputs and inputs have no incoming edges.
        auto tAllFactsStart = std::chrono::steady_clock::now();
        if (opts.enableAllFactsToSO) for (auto e : edgeScan) {
            if (!e) continue;
            const auto& inputs = e->getInputs();
            if (inputs.empty()) continue;
            bool allFacts = true;
            for (size_t idx = 0; idx < inputs.size(); ++idx) {
                auto n = inputs[idx];
                if (!n || !n->isFact) {
                    allFacts = false;
                    break;
                }
                if (!canAbsorbFactLiteral(g, n, semanticFactStats, 1)) {
                    allFacts = false;
                    break;
                }
                const auto& support = n->getProbabilisticSupportTokens();
                if (!support.empty() && edgeInputHasSupportOverlap(g, e, idx, support)) {
                    allFacts = false;
                    break;
                }
            }
            if (!allFacts) continue;
            NodePtr exit = g.getOutput(e);
            if (!exit) continue;
            auto exitIns = g.getIncomingEdges(exit);
            if (exitIns.size() != 1 || exitIns[0] != e) {
                continue;
            }
            NodePtr entry = inputs[0];
            auto region = makeRegion(entry, exit, {e}, SISORegionKind::AllFactsToSO);
            regions.push_back(std::move(region));
            if (stats) stats->allFactsToSOCount++;
        }
        auto tAllFactsEnd = std::chrono::steady_clock::now();
        if (stats) {
            stats->allFactsToSOMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    tAllFactsEnd - tAllFactsStart)
                                           .count();
        }

        auto tGeneralStart = std::chrono::steady_clock::now();
        if (opts.enableGeneral) {
            size_t acceptedGeneralRegions = 0;
            for (NodePtr exit : nodeScan) {
                if (!exit || !isNodeInView(g, exit)) {
                    continue;
                }
                const auto& incoming = g.getIncomingEdges(exit);
                if (incoming.empty() || incoming.size() > opts.maxGeneralExitIncoming) {
                    continue;
                }
                if (stats) {
                    ++stats->generalBoundedCandidates;
                }
                auto region = tryBuildBoundedGeneralRegion(g, exit, opts, semanticFactStats);
                if (!region.valid) {
                    continue;
                }
                regions.push_back(std::move(region));
                ++acceptedGeneralRegions;
                if (stats) {
                    ++stats->generalBoundedCount;
                }
                if (acceptedGeneralRegions >= opts.maxGeneralRegionsPerDetect) {
                    break;
                }
            }
        }
        auto tGeneralEnd = std::chrono::steady_clock::now();
        if (stats) {
            stats->generalBoundedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    tGeneralEnd - tGeneralStart)
                                             .count();
        }

        return regions;
    }

public:
    // ===================== Public API =====================

    static inline std::vector<SISORegionInfo> detectAllSISOStrictFromExit(
        const DerivationGraphViewInterface& g,
        const std::unordered_set<NodePtr>* dirtyNodes,
        const std::unordered_set<EdgePtr>* dirtyEdges,
        bool forceCompleteDetect = false,
        const FastPathDetectOptions* options = nullptr)
    {
        if (forceCompleteDetect) {
            dirtyNodes = nullptr;
            dirtyEdges = nullptr;
        }
        auto t0 = std::chrono::steady_clock::now();
        FastPathDetectStats fastStats;
        std::vector<SISORegionInfo> regions;
        bool usedDirtyFrontier = false;
        bool attemptedDirtyFrontier = false;
        bool fallbackToFull = false;
        size_t seedNodes = 0;
        size_t seedEdges = 0;
        size_t frontierNodes = 0;
        size_t frontierEdges = 0;

        if (dirtyNodes && dirtyEdges && (!dirtyNodes->empty() || !dirtyEdges->empty())) {
            attemptedDirtyFrontier = true;
            seedNodes = dirtyNodes->size();
            seedEdges = dirtyEdges->size();
            const double seedNodeRatio = g.getNodes().empty()
                                                 ? 0.0
                                                 : static_cast<double>(seedNodes) /
                                                           static_cast<double>(g.getNodes().size());
            const double seedEdgeRatio = g.getEdges().empty()
                                                 ? 0.0
                                                 : static_cast<double>(seedEdges) /
                                                           static_cast<double>(g.getEdges().size());
            // Dirty detection is only useful when frontier is materially smaller than full graph.
            constexpr double kFallbackRatio = 0.60;
            // If the seeds themselves already cover roughly half the graph,
            // building an expanded frontier is usually pure overhead: the
            // frontier tends to exceed the fallback ratio anyway on dense
            // rewrite waves. Skip frontier construction entirely in that case
            // and go straight to a full detect.
            constexpr double kSkipFrontierSeedRatio = 0.50;
            if (seedNodeRatio > kSkipFrontierSeedRatio || seedEdgeRatio > kSkipFrontierSeedRatio) {
                fallbackToFull = true;
            } else {
                auto frontier = buildDetectionFrontier(g, *dirtyNodes, *dirtyEdges);
                frontierNodes = frontier.nodes.size();
                frontierEdges = frontier.edges.size();

                const double nodeRatio = g.getNodes().empty()
                                                 ? 0.0
                                                 : static_cast<double>(frontierNodes) /
                                                           static_cast<double>(g.getNodes().size());
                const double edgeRatio = g.getEdges().empty()
                                                 ? 0.0
                                                 : static_cast<double>(frontierEdges) /
                                                           static_cast<double>(g.getEdges().size());
                if (frontierNodes == 0 || frontierEdges == 0 ||
                        nodeRatio > kFallbackRatio || edgeRatio > kFallbackRatio) {
                    fallbackToFull = true;
                } else {
                    usedDirtyFrontier = true;
                    regions = detectFastPathRegions(g, &fastStats, &frontier.edges, &frontier.nodes, options);
                }
            }
        }

        if (!usedDirtyFrontier) {
            regions = detectFastPathRegions(g, &fastStats, nullptr, nullptr, options);
        }

        auto t1 = std::chrono::steady_clock::now();
        // Dedup: smaller regions first to avoid overlap.
        auto countKinds = [](const std::vector<SISORegionInfo>& vec) {
            size_t singleHyperedgeCount = 0;
            size_t linearTwoEdgeCount = 0;
            size_t parallelTwoEdgeCount = 0;
            size_t allFactsToSOCount = 0;
            size_t fanOutConvergeCount = 0;
            size_t generalCount = 0;
            size_t unknownCount = 0;
            for (const auto& r : vec) {
                switch (r.kind) {
                case SISORegionKind::SingleHyperedge: ++singleHyperedgeCount; break;
                case SISORegionKind::LinearTwoEdge: ++linearTwoEdgeCount; break;
                case SISORegionKind::ParallelEdge: ++parallelTwoEdgeCount; break;
                case SISORegionKind::AllFactsToSO: ++allFactsToSOCount; break;
                case SISORegionKind::FanOutConverge: ++fanOutConvergeCount; break;
                case SISORegionKind::General: ++generalCount; break;
                default: ++unknownCount; break;
                }
            }
            return std::array<size_t, 7>{
                singleHyperedgeCount, linearTwoEdgeCount, parallelTwoEdgeCount,
                allFactsToSOCount, fanOutConvergeCount, generalCount, unknownCount};
        };
        auto candidatesByKind = countKinds(regions);

        auto tSortStart = std::chrono::steady_clock::now();
        std::sort(regions.begin(), regions.end(),
                [](const SISORegionInfo& a, const SISORegionInfo& b) {
                    return a.internalNodes.size() < b.internalNodes.size();
                });
        auto tSortEnd = std::chrono::steady_clock::now();
        std::vector<SISORegionInfo> result;
        NodeSet usedNodes;
        auto tFilterStart = std::chrono::steady_clock::now();
        for (auto& r : regions) {
            bool overlap = false;
            for (auto n : r.internalNodes) {
                if (!n) continue;
                if (usedNodes.count(n)) {
                    overlap = true;
                    break;
                }
            }
            if (overlap) continue;
            for (auto n : r.internalNodes) {
                if (n) usedNodes.insert(n);
            }
            result.push_back(std::move(r));
        }
        auto tFilterEnd = std::chrono::steady_clock::now();
        auto keptByKind = countKinds(result);

        auto detectMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        auto sortMs = std::chrono::duration_cast<std::chrono::milliseconds>(tSortEnd - tSortStart).count();
        auto filterMs = std::chrono::duration_cast<std::chrono::milliseconds>(tFilterEnd - tFilterStart).count();

        std::cout << "[siso-detect] mode="
                  << (attemptedDirtyFrontier ? "dirty-frontier" : "full")
                  << " fallback=" << (fallbackToFull ? 1 : 0)
                  << " seeds(nodes=" << seedNodes << ",edges=" << seedEdges << ")"
                  << " frontier(nodes=" << frontierNodes << ",edges=" << frontierEdges << ")"
                  << std::endl;
        std::cout << "[siso-detect] fast-path regions " << result.size()
                  << " (candidates=" << regions.size()
                  << ", single-hyperedge=" << keptByKind[0]
                  << ", linear-two-edge=" << keptByKind[1]
                  << ", parallel-two-edge=" << keptByKind[2]
                  << ", all-facts=" << keptByKind[3]
                  << ", fan-out-converge=" << keptByKind[4]
                  << ", general=" << keptByKind[5]
                  << ", unknown=" << keptByKind[6]
                  << ")" << std::endl;
        std::cout << "[siso-prof] fast-detect breakdown: "
                  << "single=" << fastStats.singleHyperedgeMs << " ms (" << fastStats.singleHyperedgeCount << ") "
                  << "linear=" << fastStats.linearTwoEdgeMs << " ms (" << fastStats.linearTwoEdgeCount << ") "
                  << "parallel=" << fastStats.parallelTwoEdgeMs << " ms (" << fastStats.parallelTwoEdgeCount << ") "
                  << "all-facts=" << fastStats.allFactsToSOMs << " ms (" << fastStats.allFactsToSOCount << ") "
                  << "fan-out-conv=" << fastStats.fanOutConvergeMs << " ms (" << fastStats.fanOutConvergeCount << ") "
                  << "general-bounded=" << fastStats.generalBoundedMs << " ms ("
                  << fastStats.generalBoundedCount << "/" << fastStats.generalBoundedCandidates << ") "
                  << "semantic-stats=" << fastStats.semanticFactStatsMs << " ms"
                  << std::endl;
        std::cout << "[siso-prof] detect=" << detectMs << " ms"
                  << " sort=" << sortMs << " ms"
                  << " filter=" << filterMs << " ms"
                  << " candidates(kind:sh/lin/par/all/fan/gen/unk)="
                  << candidatesByKind[0] << "/" << candidatesByKind[1] << "/" << candidatesByKind[2] << "/"
                  << candidatesByKind[3] << "/" << candidatesByKind[4] << "/" << candidatesByKind[5] << "/"
                  << candidatesByKind[6]
                  << " kept=" << result.size()
                  << std::endl;
        return result;
    }

    static inline std::vector<SISORegionInfo> detectAllSISOStrictFromExit(
        const DerivationGraphViewInterface& g,
        bool forceCompleteDetect = false)
    {
        return detectAllSISOStrictFromExit(g, nullptr, nullptr, forceCompleteDetect, nullptr);
    }

    // Output the full graph and color all SISO regions (non-SISO nodes/edges in light gray).
    static inline void dumpAllRegionsAsDot(
        const DerivationGraphViewInterface& g,
        const std::vector<SISORegionInfo>& regions,
        const std::string& filename)
    {
        if (!DerivationGraphViewInterface::isDumpDotEnabled()) {
            return;
        }
        const std::string path = DerivationGraphViewInterface::qualifyDumpPath(filename);
        std::ofstream out(path);
        if (!out.is_open()) {
            std::cerr << "Cannot open dot file: " << path << "\n";
            return;
        }
        std::vector<std::string> palette = {
            "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728",
            "#9467bd", "#8c564b", "#e377c2", "#7f7f7f",
            "#bcbd22", "#17becf"
        };

        // Record node/edge region colors (first match when multiple regions) and entry/exit markers.
        std::unordered_map<NodePtr, std::string> nodeColor;
        std::unordered_map<EdgePtr, std::string> edgeColor;
        std::unordered_map<NodePtr, int> entryCount;
        std::unordered_map<NodePtr, int> exitCount;
        for (size_t i = 0; i < regions.size(); ++i) {
            const auto& r = regions[i];
            const std::string col = palette[i % palette.size()];
            for (NodePtr n : r.internalNodes) {
                if (!n) continue;
                nodeColor.emplace(n, col);
            }
            for (EdgePtr e : r.internalEdges) {
                if (!e) continue;
                edgeColor.emplace(e, col);
            }
            if (r.entry) entryCount[r.entry]++;
            if (r.exit) exitCount[r.exit]++;
        }

        out << "digraph SISO_All {\n";
        out << "  rankdir=LR;\n";
        out << "  node [shape=box, style=filled, fillcolor=lightgray, color=gray];\n";

        // All nodes: color if in region, otherwise light gray.
        for (const auto& n : g.getNodes()) {
            if (!n) continue;
            auto it = nodeColor.find(n);
            const std::string col = (it != nodeColor.end()) ? it->second : "#dddddd";
            int periph = 1;
            bool isEntry = entryCount.count(n);
            bool isExit  = exitCount.count(n);
            if (isEntry && isExit) periph = 3;
            else if (isEntry || isExit) periph = 2;
            std::string role;
            if (isEntry) role += "[SI]";
            if (isExit) role += "[SO]";
            out << "  node" << n->getId()
                << " [label=\"" << escapeDot(n->getTuple().toString())
                << "\\n(id=" << n->getId() << ")" << role << "\", fillcolor=\"" << col
                << "\", color=\"" << col << "\", fontcolor=\"black\", peripheries=" << periph << "];\n";
        }

        // Hyperedges as point nodes.
        out << "  node [shape=point, width=0.2, height=0.2, style=filled];\n";
        for (const auto& e : g.getEdges()) {
            if (!e) continue;
            if (e->pruned) continue;
            NodePtr outNode = g.getOutput(e);
            if (!outNode) continue;

            std::string col = "#cccccc";
            auto itCol = edgeColor.find(e);
            if (itCol != edgeColor.end()) col = itCol->second;

            out << "  edge" << e->getId()
                << " [label=\"\", fillcolor=\"" << col << "\", color=\"" << col << "\"];\n";

            for (NodePtr in : g.getInputs(e)) {
                if (!in) continue;
                out << "  node" << in->getId() << " -> edge" << e->getId()
                    << " [color=\"" << col << "\"];\n";
            }
            if (outNode) {
                out << "  edge" << e->getId() << " -> node" << outNode->getId()
                    << " [color=\"" << col << "\"];\n";
            }
        }

        out << "}\n";
    }
};

#endif // GRAPHANALYZER_H
