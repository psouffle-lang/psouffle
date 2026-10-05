#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <iostream>

#include "souffle/problog/DerivationGraph.h"
#include "souffle/problog/ForwardCompilation.h"
#include "souffle/problog/GraphAnalyzer.h"
#include "souffle/problog/formula/CuddManager.h"  // WeightedBDDManager, BddNodeRef

namespace souffle::problog {

inline void assertRewriteProbability(double p, const std::string& ctx) {
    if (p < 0.0 || p > 1.0) {
        std::cerr << "[GraphRewriter] Probability out of [0,1]: " << p
                  << " at " << ctx << std::endl;
        assert(false && "rewrite probability out of range");
    }
}

using SISORegionInfo = ::SISORegionInfo;

/**
 * Statistics collected during SISO-based graph rewriting.
 *
 * The numbers are informational only and do not affect semantics.
 */
struct GraphRewriteStats {
    size_t numIterations = 0;          ///< Number of outer iterations
    size_t numRegionsDetected = 0;     ///< Total detected SISO regions across iterations
    size_t totalDetectedRegionNodes = 0;  ///< Total node count summed over detected SISO regions
    size_t totalDetectedRegionEdges = 0;  ///< Total edge count summed over detected SISO regions
    size_t numRegionsRewritten = 0;    ///< Total SISO regions rewritten
    size_t numGeneralRegionsRewritten = 0;  ///< Regions summarized via general rewrite
    size_t numFastGeneralRegions = 0;       ///< General regions summarized by conjunctive fast path
    size_t numNodesRemoved = 0;        ///< Internal nodes removed from the view
    size_t numEdgesRemoved = 0;        ///< Internal edges removed from the view
    size_t numEdgesAdded = 0;          ///< Synthetic edges added
    size_t totalRandomVars = 0;        ///< Sum of random vars across regions (facts + edges, excl. entry/exit facts)
    size_t maxRandomVars = 0;          ///< Max random vars in a single region
    size_t simpleFactRegions = 0;      ///< Count of simple fact-based regions rewritten
    size_t randomVarsBefore = 0;       ///< Random vars in the view before rewrite
    size_t randomVarsAfter = 0;        ///< Random vars in the view after rewrite
    double totalDetectMs = 0.0;        ///< Total SISO detection time across iterations
    double totalBddManagerInitMs = 0.0;  ///< Total CUDD manager init time attributed to rewrite
    double totalBddBuildMs = 0.0;      ///< Total BDD build/compilation time across rewritten regions
    double totalBddWmcMs = 0.0;        ///< Total BDD WMC time across rewritten regions
    double totalFastGeneralMs = 0.0;   ///< Total time spent in conjunctive general-region summaries
    double totalApplyMs = 0.0;         ///< Total rewrite apply time for general regions
    double totalCompactionMs = 0.0;    ///< Total deterministic edge compaction time
    double totalCleanupMs = 0.0;       ///< Total isolated-node cleanup time
    double initialCountRandomVarsMs = 0.0;   ///< Time spent counting random vars before rewrite
    double initialEvidenceAffectedMs = 0.0;  ///< Time spent building evidence-affected node seed set
};

enum class SplitMode {
    None = 0,
    Naive,
    Complete,
};

/**
 * Feature switches for SISO detection / rewrite passes.
 * Specialized SISO patterns run first. A bounded general SISO pass then scans
 * for remaining local regions without turning rewrite detection into a full
 * graph decomposition algorithm.
 */
struct RewriteFeatureFlags {
    bool enableSingleHyperedge  = true;
    bool enableAllFactsToSO     = true;
    bool enableLinearTwoEdge    = true;
    bool enableParallelEdge     = true;   ///< enable parallel single-input edge detection/rewrite
    bool enableFanOutConverge   = true;
    bool enableGeneral          = true;   ///< bounded general SISO rewrite after specialized patterns
    bool enableCompaction       = true;   ///< edge compaction after each SISO pass
    bool restrictCompactionToDirty = false;  ///< only compact edges adjacent to the last rewrite frontier
    SplitMode splitMode         = SplitMode::Naive;  ///< split disjoint fan-out branches into shadow facts
    size_t splitMaxNewNodesPerPass = 5000;  ///< exhaustive split budget: max new shadow nodes per pass
    size_t splitMaxNewEdgesPerPass = 50000; ///< exhaustive split budget: max rewired edges per pass
    size_t splitMaxGroupsPerNode = 2;       ///< exhaustive split cap: max groups kept per node (incl. original)
    size_t splitMinGroupEdges = 1;          ///< exhaustive split threshold: min edges in a split group
    bool enableCleanupIsolated  = true;   ///< drop isolated fact/shadow nodes at end of iteration
    bool forceFullSisoDetect = false; ///< force full-graph SISO detect (disable dirty-frontier detect)
    bool relaxCompactionDirty   = true;   ///< reseed only the surviving compacted edge endpoints
    size_t maxGeneralPasses     = 1;      ///< bounded general-SISO passes after fast fixpoint
};

/**
 * Implements SISO-based dependency decomposition.
 *
 * Repeatedly finds SISO regions in the working view, summarizes each region
 * into a single probabilistic edge entry->exit with probability Pr(exit|entry),
 * and updates the view in-place.
 */
class GraphRewriter {
public:
    /**
    * Rewrite all non-trivial SISO regions until a fixpoint on the given view.
    *
    * @param graph   Underlying derivation graph. Only extended (new hyperedges).
    * @param view    Working view mutated in-place (nodes/edges removed or added).
    * @param debug   Enables per-region trace messages.
    * @param flags   Feature switches controlling which SISO kinds / passes are enabled.
    */
    GraphRewriteStats rewriteUntilFixpoint(WorkingDerivationGraph& graph,
                                           WorkingSubgraphView& view,
                                           bool debug = false,
                                           const RewriteFeatureFlags& flags = RewriteFeatureFlags{}) const {
        GraphRewriteStats stats;
        view.invalidateCaches();
        precomputedProbResult.clear();
        std::unique_ptr<WeightedBDDManager> bddManager;
        double managerInitMs = 0.0;
        bool managerInitialized = false;

        auto toMs = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };

        const bool dumpStats = DerivationGraphViewInterface::isDumpStatsEnabled();
        const bool dumpDot = DerivationGraphViewInterface::isDumpDotEnabled();
        (void)debug;

        auto initialCountStart = std::chrono::steady_clock::now();
        stats.randomVarsBefore = countRandomVarsInView(view);
        stats.initialCountRandomVarsMs = toMs(std::chrono::steady_clock::now() - initialCountStart);
        stats.randomVarsAfter = stats.randomVarsBefore;
        const bool generalRewriteEnabled = flags.enableGeneral;
        auto initialEvidenceStart = std::chrono::steady_clock::now();
        const auto evidenceAffectedNodes = collectEvidenceAffectedNodes(view);
        stats.initialEvidenceAffectedMs = toMs(std::chrono::steady_clock::now() - initialEvidenceStart);

        // Only the first region should attribute manager init time; subsequent regions reuse the same manager.
        bool firstRegionTiming = true;

        auto rewriteStart = std::chrono::steady_clock::now();
        std::unordered_set<NodePtr> detectDirtyNodes;
        std::unordered_set<EdgePtr> detectDirtyEdges;
        bool hasDetectDirty = false;
        bool previousPassOnlyLinearParallel = false;
        bool previousPassOnlyFactAbsorption = false;
        size_t generalDetectPasses = 0;

        auto runSplitPass = [&](const std::unordered_set<NodePtr>* splitSeedNodes,
                                const std::unordered_set<EdgePtr>* splitSeedEdges,
                                std::unordered_set<NodePtr>* splitDirtyNodes,
                                std::unordered_set<EdgePtr>* splitDirtyEdges) -> bool {
            if (flags.splitMode == SplitMode::None) {
                return false;
            }
            SplitStats splitStats = (flags.splitMode == SplitMode::Naive)
                    ? splitFanoutNaive(graph, view, stats, evidenceAffectedNodes, splitSeedNodes,
                              splitSeedEdges, splitDirtyNodes, splitDirtyEdges)
                    : splitFanoutComplete(
                              graph, view, stats, flags, evidenceAffectedNodes, splitDirtyNodes, splitDirtyEdges);
            std::cout << "[GraphRewriter]   split(" << splitModeToString(flags.splitMode)
                      << "): nodes=" << splitStats.nodesAdded
                      << " edges=" << splitStats.edgesRewritten
                      << " time=" << splitStats.elapsedMs << " ms"
                      << std::endl;
            if (dumpStats && (splitStats.nodesAdded > 0 || splitStats.edgesRewritten > 0)) {
                std::cout << "[GraphRewriter]   Fan-out split(" << splitModeToString(flags.splitMode)
                          << "): newNodes=" << splitStats.nodesAdded
                          << " edgesRewritten=" << splitStats.edgesRewritten
                          << " time=" << splitStats.elapsedMs << " ms"
                          << std::endl;
            }
            if (splitStats.nodesAdded > 0 || splitStats.edgesRewritten > 0) {
                view.invalidateCaches();
                return true;
            }
            return false;
        };

        while (true) {
            const auto& nodeSet = view.getNodes();
            const auto& edgeSet = view.getEdges();
            if (nodeSet.empty() || edgeSet.empty()) {
                if (dumpStats) {
                    std::cout << "[GraphRewriter] Empty view (nodes=" << nodeSet.size()
                              << ", edges=" << edgeSet.size() << "); stop rewrite." << std::endl;
                }
                stats.randomVarsAfter = 0;
                break;
            }
            auto iterStart = std::chrono::steady_clock::now();
            ++stats.numIterations;
            auto countBeforeStart = std::chrono::steady_clock::now();
            size_t iterRandomVarsBefore = countRandomVarsInView(view);
            double countBeforeMs = toMs(std::chrono::steady_clock::now() - countBeforeStart);
            std::unordered_set<NodePtr> iterDirtyNodes;
            std::unordered_set<EdgePtr> iterDirtyEdges;
            auto markDirtyNode = [&](NodePtr n) {
                if (n) iterDirtyNodes.insert(n);
            };
            auto markDirtyEdge = [&](EdgePtr e) {
                if (e) iterDirtyEdges.insert(e);
            };
            auto markDirtyEdgeEndpoints = [&](EdgePtr e) {
                if (!e) return;
                markDirtyEdge(e);
                markDirtyNode(e->getOutput());
                for (auto in : e->getInputs()) {
                    markDirtyNode(in);
                }
            };
            auto markDirtyRegion = [&](const SISORegionInfo& region) {
                markDirtyNode(region.entry);
                markDirtyNode(region.exit);
                for (auto n : region.internalNodes) {
                    markDirtyNode(n);
                }
                for (auto e : region.internalEdges) {
                    markDirtyEdgeEndpoints(e);
                }
            };
            auto markDirtyAllFactsResult = [&](NodePtr exitNode) {
                if (exitNode && view.getNodes().count(exitNode) > 0) {
                    markDirtyNode(exitNode);
                    for (auto inEdge : view.getIncomingEdges(exitNode)) {
                        markDirtyEdgeEndpoints(inEdge);
                    }
                    for (auto outEdge : view.getOutgoingEdges(exitNode)) {
                        markDirtyEdgeEndpoints(outEdge);
                    }
                }
            };

            view.cachedSortedIncomingEdges.clear();
            double dumpBeforeDotMs = 0.0;
            if (dumpDot) {
                std::ostringstream dotBefore;
                dotBefore << "rewrite_iter" << stats.numIterations << "_before.dot";
                auto dotBeforeStart = std::chrono::steady_clock::now();
                view.dumpDot(dotBefore.str());
                dumpBeforeDotMs = toMs(std::chrono::steady_clock::now() - dotBeforeStart);
                if (dumpStats) {
                    std::cout << "[GraphRewriter] dumpDot(before) took "
                              << dumpBeforeDotMs << " ms" << std::endl;
                }
            }
            auto detectStart = std::chrono::steady_clock::now();
            GraphAnalyzer::FastPathDetectOptions fullDetectOptions;
            fullDetectOptions.enableSingleHyperedge = flags.enableSingleHyperedge;
            fullDetectOptions.enableLinearTwoEdge = flags.enableLinearTwoEdge;
            fullDetectOptions.enableParallelEdge = flags.enableParallelEdge;
            fullDetectOptions.enableAllFactsToSO = flags.enableAllFactsToSO;
            fullDetectOptions.enableFanOutConverge = flags.enableFanOutConverge;
            // General SISO detection is intentionally run only after the
            // cheaper pattern detectors reach a fixpoint. Running it in every
            // pass scans many candidates that will be consumed by simple
            // rewrites anyway.
            fullDetectOptions.enableGeneral = false;
            GraphAnalyzer::FastPathDetectOptions detectOptions = fullDetectOptions;
            bool primaryDetectMask = false;
            bool primaryDetectCanFallback = false;
            if (flags.splitMode == SplitMode::None && previousPassOnlyLinearParallel) {
                detectOptions.enableSingleHyperedge = false;
                detectOptions.enableAllFactsToSO = false;
                detectOptions.enableFanOutConverge = false;
                primaryDetectMask = true;
            }
            if (flags.splitMode == SplitMode::None && previousPassOnlyFactAbsorption) {
                detectOptions.enableSingleHyperedge = false;
                detectOptions.enableAllFactsToSO = false;
                detectOptions.enableFanOutConverge = false;
                primaryDetectMask = true;
                primaryDetectCanFallback = true;
            }
            if (flags.splitMode == SplitMode::None && stats.numIterations == 1 && !hasDetectDirty) {
                detectOptions.enableLinearTwoEdge = false;
                detectOptions.enableParallelEdge = false;
                detectOptions.enableFanOutConverge = false;
                primaryDetectMask = detectOptions.enableSingleHyperedge != fullDetectOptions.enableSingleHyperedge ||
                        detectOptions.enableLinearTwoEdge != fullDetectOptions.enableLinearTwoEdge ||
                        detectOptions.enableParallelEdge != fullDetectOptions.enableParallelEdge ||
                        detectOptions.enableAllFactsToSO != fullDetectOptions.enableAllFactsToSO ||
                        detectOptions.enableFanOutConverge != fullDetectOptions.enableFanOutConverge;
                primaryDetectCanFallback = primaryDetectMask;
            }
            auto detectRegions = [&](const GraphAnalyzer::FastPathDetectOptions& opts) {
                return hasDetectDirty
                    ? GraphAnalyzer::detectAllSISOStrictFromExit(
                              view, &detectDirtyNodes, &detectDirtyEdges, flags.forceFullSisoDetect, &opts)
                    : GraphAnalyzer::detectAllSISOStrictFromExit(
                              view, nullptr, nullptr, flags.forceFullSisoDetect, &opts);
            };
            auto regions = detectRegions(detectOptions);
            if (regions.empty() && primaryDetectMask && primaryDetectCanFallback) {
                regions = detectRegions(fullDetectOptions);
            }
            if (regions.empty() && generalRewriteEnabled && generalDetectPasses < flags.maxGeneralPasses) {
                ++generalDetectPasses;
                GraphAnalyzer::FastPathDetectOptions generalDetectOptions;
                generalDetectOptions.enableSingleHyperedge = false;
                generalDetectOptions.enableLinearTwoEdge = false;
                generalDetectOptions.enableParallelEdge = false;
                generalDetectOptions.enableAllFactsToSO = false;
                generalDetectOptions.enableFanOutConverge = false;
                generalDetectOptions.enableGeneral = true;
                regions = GraphAnalyzer::detectAllSISOStrictFromExit(
                        view, nullptr, nullptr, true, &generalDetectOptions);
            }
            // Filter by enabled flags.
            if (!flags.enableSingleHyperedge || !flags.enableAllFactsToSO ||
                    !flags.enableLinearTwoEdge || !flags.enableParallelEdge ||
                    !flags.enableFanOutConverge || !generalRewriteEnabled) {
                std::vector<SISORegionInfo> filtered;
                filtered.reserve(regions.size());
                for (const auto& r : regions) {
                    switch (r.kind) {
                        case SISORegionKind::SingleHyperedge:
                            if (!flags.enableSingleHyperedge) continue;
                            break;
                        case SISORegionKind::AllFactsToSO:
                            if (!flags.enableAllFactsToSO) continue;
                            break;
                        case SISORegionKind::LinearTwoEdge:
                            if (!flags.enableLinearTwoEdge) continue;
                            break;
                        case SISORegionKind::ParallelEdge:
                            if (!flags.enableParallelEdge) continue;
                            break;
                        case SISORegionKind::FanOutConverge:
                            if (!flags.enableFanOutConverge) continue;
                            break;
                        case SISORegionKind::General:
                            if (!generalRewriteEnabled) continue;
                            break;
                        default:
                            break;
                    }
                    filtered.push_back(r);
                }
                regions.swap(filtered);
            }
            double detectMs = toMs(std::chrono::steady_clock::now() - detectStart);
            stats.totalDetectMs += detectMs;
            std::cout << "[GraphRewriter] SISO detection took "
                      << detectMs << " ms" << std::endl;
            size_t detectedSingle = 0, detectedLinear = 0, detectedParallel = 0, detectedAllFacts = 0;
            size_t detectedFan = 0, detectedGeneral = 0;
            for (const auto& r : regions) {
                switch (r.kind) {
                    case SISORegionKind::SingleHyperedge: ++detectedSingle; break;
                    case SISORegionKind::LinearTwoEdge: ++detectedLinear; break;
                    case SISORegionKind::ParallelEdge: ++detectedParallel; break;
                    case SISORegionKind::AllFactsToSO: ++detectedAllFacts; break;
                    case SISORegionKind::FanOutConverge: ++detectedFan; break;
                    case SISORegionKind::General: ++detectedGeneral; break;
                    default: break;
                }
            }
            stats.numRegionsDetected += regions.size();
            for (const auto& r : regions) {
                const size_t nodesInRegion = r.internalNodes.size();
                const size_t edgesInRegion = r.internalEdges.size();
                stats.totalDetectedRegionNodes += nodesInRegion;
                stats.totalDetectedRegionEdges += edgesInRegion;
            }

            if (regions.empty()) {
                size_t precomputedNow = precomputeOutputFacts(view, evidenceAffectedNodes);
                if (dumpStats && precomputedNow > 0) {
                    std::cout << "[GraphRewriter]   Precomputed output facts: "
                              << precomputedNow << std::endl;
                }
                stats.randomVarsAfter = iterRandomVarsBefore;
                long long iterDelta = 0;
                double iterRatio = iterRandomVarsBefore == 0 ? 0.0 : 1.0;
                std::cout << "[GraphRewriter]   Iteration " << stats.numIterations
                          << " random vars: before=" << iterRandomVarsBefore
                          << ", after=" << stats.randomVarsAfter
                          << ", delta=" << iterDelta
                          << ", ratio=" << iterRatio << std::endl;
                if (dumpStats) {
                    std::cout << "[GraphRewriter] No SISO regions found; rewrite fixpoint at iteration "
                              << stats.numIterations << std::endl;
                }
                const auto* splitSeedNodes = hasDetectDirty ? &detectDirtyNodes : nullptr;
                const auto* splitSeedEdges = hasDetectDirty ? &detectDirtyEdges : nullptr;
                if (runSplitPass(splitSeedNodes, splitSeedEdges, &iterDirtyNodes, &iterDirtyEdges)) {
                    previousPassOnlyLinearParallel = false;
                    previousPassOnlyFactAbsorption = false;
                    hasDetectDirty = !iterDirtyNodes.empty() || !iterDirtyEdges.empty();
                    if (hasDetectDirty) {
                        detectDirtyNodes.swap(iterDirtyNodes);
                        detectDirtyEdges.swap(iterDirtyEdges);
                    } else {
                        detectDirtyNodes.clear();
                        detectDirtyEdges.clear();
                    }
                    continue;
                }
                break;
            }

            double dumpRegionsMs = 0.0;
            if (dumpStats) {
                std::cout << "[GraphRewriter] Iteration " << stats.numIterations
                          << " : detected " << regions.size()
                          << " SISO region(s)." << std::endl;
            }
            if (dumpDot) {
                std::ostringstream sisoDot;
                sisoDot << "siso_regions_iter" << stats.numIterations << ".dot";
                auto dotStart = std::chrono::steady_clock::now();
                GraphAnalyzer::dumpAllRegionsAsDot(view, regions, sisoDot.str());
                dumpRegionsMs = toMs(std::chrono::steady_clock::now() - dotStart);
                if (dumpStats) {
                    std::cout << "[GraphRewriter] dumpAllRegionsAsDot took "
                              << dumpRegionsMs << " ms" << std::endl;
                }
            }

            size_t rewrittenThisRound = 0;
            auto loopStart = std::chrono::steady_clock::now();
            double loopRewrittenMs = 0.0;
            double loopCondMs = 0.0;

            // Regions produced by GraphAnalyzer are non-overlapping.
            for (const auto& region : regions) {
                if (!region.valid) {
                    continue;
                }

                // Fast-path by SISO kind.
                switch (region.kind) {
                    case SISORegionKind::AllFactsToSO: {
                        if (region.internalEdges.size() != 1) continue;
                        EdgePtr edge = region.internalEdges.front();
                        if (!edge) continue;
                        NodePtr exit = region.exit;
                        if (!exit) continue;
                        const auto& inputs = edge->getInputs();
                        const auto& negs   = edge->getBodyNegations();
                        double p = edge->getProbability();
                        if (p < 0.0) p = 0.0;
                        if (p > 1.0) p = 1.0;
                        std::vector<SupportToken> exitSupport = edge->getProbabilisticSupportTokens();
                        size_t regionRandomVars = 0;
                        if (p > 0.0 && p < 1.0) ++regionRandomVars;
                        for (size_t i = 0; i < inputs.size(); ++i) {
                            NodePtr n = inputs[i];
                            if (!n) continue;
                            bool isNegated = i < negs.size() ? negs[i] : false;
                            double np = n->getProbability();
                            if (np < 0.0) np = 0.0;
                            if (np > 1.0) np = 1.0;
                            p *= isNegated ? (1.0 - np) : np;
                            exitSupport = mergeSupportTokenLists(
                                    {&exitSupport, &n->getProbabilisticSupportTokens()});
                            if (np > 0.0 && np < 1.0) ++regionRandomVars;
                        }
                        exit->isFact = true;
                        exit->setProbability(p);
                        exit->setProbabilisticSupportTokens(std::move(exitSupport));

                        auto& edges = view.mutableEdges();
                        auto& nodes = view.mutableNodes();
                        size_t removedEdges = edges.erase(edge);
                        view.invalidateCaches();  // ensure degree queries reflect removal
                        size_t removedNodes = 0;
                        for (auto n : inputs) {
                            if (!n) continue;
                            // Drop isolated fact inputs; preserve output/evidence facts via precompute.
                            if (view.getIncomingEdges(n).empty() && view.getOutgoingEdges(n).empty()) {
                                bool removedInput = false;
                                if (canPrecomputeOutputFact(view, n, evidenceAffectedNodes)) {
                                    precomputedProbResult[n] = n->getProbability();
                                    n->needOutput = false;
                                    n->isQuery = false;
                                    removedInput = nodes.erase(n) > 0;
                                } else if (!n->needOutput && !n->hasEvidence()) {
                                    removedInput = nodes.erase(n) > 0;
                                }
                                if (removedInput) {
                                    ++removedNodes;
                                }
                            }
                        }
                        // If exit becomes isolated, precompute it (if eligible) and drop it from the view.
                        if (view.getIncomingEdges(exit).empty() && view.getOutgoingEdges(exit).empty()) {
                            bool removedExit = false;
                            if (canPrecomputeOutputFact(view, exit, evidenceAffectedNodes)) {
                                precomputedProbResult[exit] = exit->getProbability();
                                exit->needOutput = false;
                                exit->isQuery = false;
                                removedExit = nodes.erase(exit) > 0;
                            } else if (!exit->needOutput && !exit->hasEvidence()) {
                                removedExit = nodes.erase(exit) > 0;
                            }
                            if (removedExit) {
                                ++removedNodes;
                            }
                        }
                        stats.numEdgesRemoved += removedEdges;
                        stats.numNodesRemoved += removedNodes;
                        stats.totalRandomVars += regionRandomVars;
                        stats.maxRandomVars = std::max(stats.maxRandomVars, regionRandomVars);
                        view.invalidateCaches();

                        markDirtyAllFactsResult(exit);
                        ++rewrittenThisRound;
                        ++stats.numRegionsRewritten;
                        continue;
                    }
                    case SISORegionKind::SingleHyperedge: {
                        if (region.internalEdges.size() != 1) continue;
                        EdgePtr edge = region.internalEdges.front();
                        if (!edge) continue;
                        if (!region.entry || !region.exit) continue;
                        const auto& inputs = edge->getInputs();
                        const auto& negs   = edge->getBodyNegations();
                        if (inputs.empty()) continue;
                        double p = edge->getProbability();
                        if (p < 0.0) p = 0.0;
                        if (p > 1.0) p = 1.0;
                        std::vector<SupportToken> newEdgeSupport =
                                edge->getProbabilisticSupportTokens();
                        size_t regionRandomVars = 0;
                        if (p > 0.0 && p < 1.0) ++regionRandomVars;
                        for (size_t i = 0; i < inputs.size(); ++i) {
                            NodePtr n = inputs[i];
                            if (!n) continue;
                            bool isNegated = i < negs.size() ? negs[i] : false;
                            if (!n->isFact) continue;
                            double np = n->getProbability();
                            if (np < 0.0) np = 0.0;
                            if (np > 1.0) np = 1.0;
                            p *= isNegated ? (1.0 - np) : np;
                            newEdgeSupport = mergeSupportTokenLists(
                                    {&newEdgeSupport, &n->getProbabilisticSupportTokens()});
                            if (np > 0.0 && np < 1.0) ++regionRandomVars;
                        }
                        std::vector<NodePtr> siInput = {region.entry};
                        // Preserve the negation flag (if any) on the entry input.
                        bool entryNeg = false;
                        for (size_t i = 0; i < inputs.size(); ++i) {
                            if (inputs[i] == region.entry) {
                                entryNeg = i < negs.size() ? negs[i] : false;
                                break;
                            }
                        }
                        std::vector<bool> newNegs = {entryNeg};
                        EdgePtr newEdge = graph.createHyperedge(siInput, region.exit, nullptr, newNegs);
                        if (!newEdge) {
                            continue;
                        }
                        newEdge->setProbability(p);
                        newEdge->setProbabilisticSupportTokens(std::move(newEdgeSupport));

                        auto& edges = view.mutableEdges();
                        auto& nodes = view.mutableNodes();
                        size_t removedEdges = edges.erase(edge);
                        edges.insert(newEdge);
                        view.invalidateCaches();  // update degree queries after edge replacement
                        size_t removedNodes = 0;
                        for (auto n : inputs) {
                            if (!n) continue;
                            if (view.getIncomingEdges(n).empty() && view.getOutgoingEdges(n).empty()) {
                                if (nodes.erase(n) > 0) {
                                    ++removedNodes;
                                }
                            }
                        }
                        stats.totalRandomVars += regionRandomVars;
                        stats.maxRandomVars = std::max(stats.maxRandomVars, regionRandomVars);
                        stats.numEdgesRemoved += removedEdges;
                        stats.numEdgesAdded += 1;
                        stats.numNodesRemoved += removedNodes;
                        view.invalidateCaches();
                        markDirtyRegion(region);
                        markDirtyEdgeEndpoints(newEdge);
                        ++rewrittenThisRound;
                        ++stats.numRegionsRewritten;
                        continue;
                    }
                    case SISORegionKind::LinearTwoEdge: {
                        if (region.internalEdges.size() != 2) continue;
                        EdgePtr e1 = region.internalEdges[0];
                        EdgePtr e2 = region.internalEdges[1];
                        if (!e1 || !e2) continue;
                        NodePtr mid = nullptr;
                        EdgePtr intoMid = nullptr;
                        EdgePtr outMid = nullptr;
                        for (auto e : region.internalEdges) {
                            if (!e) continue;
                            NodePtr out = e->getOutput();
                            if (out == region.entry || out == region.exit) {
                                continue;
                            }
                            mid = out;
                            intoMid = e;
                            break;
                        }
                        if (!mid) continue;
                        for (auto e : region.internalEdges) {
                            if (!e || e == intoMid) continue;
                            const auto& inputs = e->getInputs();
                            if (inputs.size() == 1 && inputs[0] == mid) {
                                outMid = e;
                                break;
                            }
                        }
                        if (!intoMid || !outMid) continue;
                        const auto& negInto = intoMid->getBodyNegations();
                        if (negInto.size() > 1) continue;  // expect single-input edge
                        bool entryNeg = (!negInto.empty() && negInto[0]);
                        const auto& negOut = outMid->getBodyNegations();
                        if (negOut.size() > 1) continue;   // expect single-input edge
                        if (!negOut.empty() && negOut[0]) continue;  // do not fast-path if mid->exit is negated
                        double p1 = intoMid->getProbability();
                        double p2 = outMid->getProbability();
                        if (p1 < 0.0) p1 = 0.0;
                        if (p1 > 1.0) p1 = 1.0;
                        if (p2 < 0.0) p2 = 0.0;
                        if (p2 > 1.0) p2 = 1.0;
                        double p = p1 * p2;
                        std::vector<SupportToken> newEdgeSupport = mergeSupportTokenLists(
                                {&intoMid->getProbabilisticSupportTokens(),
                                        &outMid->getProbabilisticSupportTokens()});
                        size_t regionRandomVars = 0;
                        if (p1 > 0.0 && p1 < 1.0) ++regionRandomVars;
                        if (p2 > 0.0 && p2 < 1.0) ++regionRandomVars;

                        std::vector<NodePtr> inputsNew = {region.entry};
                        std::vector<bool> negsNew = {entryNeg};
                        EdgePtr newEdge = graph.createHyperedge(inputsNew, region.exit, nullptr, negsNew);
                        if (!newEdge) continue;
                        newEdge->setProbability(p);
                        newEdge->setProbabilisticSupportTokens(std::move(newEdgeSupport));

                        auto& edges = view.mutableEdges();
                        auto& nodes = view.mutableNodes();
                        size_t removedEdges = 0;
                        removedEdges += edges.erase(intoMid);
                        removedEdges += edges.erase(outMid);
                        edges.insert(newEdge);
                        size_t removedNodes = 0;
                        if (nodes.erase(mid) > 0) {
                            ++removedNodes;
                        }
                        stats.numEdgesRemoved += removedEdges;
                        stats.numEdgesAdded += 1;
                        stats.numNodesRemoved += removedNodes;
                        stats.totalRandomVars += regionRandomVars;
                        stats.maxRandomVars = std::max(stats.maxRandomVars, regionRandomVars);
                        view.invalidateCaches();

                        markDirtyRegion(region);
                        markDirtyEdgeEndpoints(newEdge);
                        ++rewrittenThisRound;
                        ++stats.numRegionsRewritten;
                        continue;
                    }
                    case SISORegionKind::ParallelEdge: {
                        if (region.internalEdges.size() < 2) continue;
                        if (!region.entry || !region.exit) continue;
                        NodePtr entryNode = region.entry;
                        NodePtr exitNode = region.exit;
                        bool bad = false;
                        bool negInit = false;
                        bool negFlag = false;
                        double prod = 1.0;
                        size_t regionRandomVars = 0;
                        for (auto e : region.internalEdges) {
                            if (!e) {
                                bad = true;
                                break;
                            }
                            const auto& inputs = e->getInputs();
                            if (inputs.size() != 1 || inputs[0] != entryNode) {
                                bad = true;
                                break;
                            }
                            if (view.getOutput(e) != exitNode) {
                                bad = true;
                                break;
                            }
                            const auto& negs = e->getBodyNegations();
                            if (negs.size() > 1) {
                                bad = true;
                                break;
                            }
                            bool isNeg = (!negs.empty() && negs[0]);
                            if (!negInit) {
                                negFlag = isNeg;
                                negInit = true;
                            } else if (negFlag != isNeg) {
                                bad = true;
                                break;
                            }
                            double p = e->getProbability();
                            if (p < 0.0) p = 0.0;
                            if (p > 1.0) p = 1.0;
                            prod *= (1.0 - p);
                            if (p > 0.0 && p < 1.0) ++regionRandomVars;
                        }
                        if (bad) continue;
                        double pEff = 1.0 - prod;
                        if (pEff < 0.0) pEff = 0.0;
                        if (pEff > 1.0) pEff = 1.0;
                        std::vector<SupportToken> newEdgeSupport;
                        for (auto e : region.internalEdges) {
                            if (!e) continue;
                            newEdgeSupport = mergeSupportTokenLists(
                                    {&newEdgeSupport, &e->getProbabilisticSupportTokens()});
                        }

                        std::vector<NodePtr> newInputs = {entryNode};
                        std::vector<bool> newNegs = {negFlag};
                        EdgePtr newEdge = graph.createHyperedge(newInputs, exitNode, nullptr, newNegs);
                        if (!newEdge) continue;
                        newEdge->setProbability(pEff);
                        newEdge->setProbabilisticSupportTokens(std::move(newEdgeSupport));

                        auto& edges = view.mutableEdges();
                        size_t removedEdges = 0;
                        for (auto e : region.internalEdges) {
                            if (!e) continue;
                            removedEdges += edges.erase(e);
                        }
                        edges.insert(newEdge);

                        stats.numEdgesRemoved += removedEdges;
                        stats.numEdgesAdded += 1;
                        stats.totalRandomVars += regionRandomVars;
                        stats.maxRandomVars = std::max(stats.maxRandomVars, regionRandomVars);
                        view.invalidateCaches();
                        markDirtyRegion(region);
                        markDirtyEdgeEndpoints(newEdge);
                        ++rewrittenThisRound;
                        ++stats.numRegionsRewritten;
                        continue;
                    }
                    case SISORegionKind::FanOutConverge: {
                        if (region.internalEdges.size() < 3) continue;
                        if (!region.entry || !region.exit) continue;
                        NodePtr entryNode = region.entry;
                        NodePtr exitNode = region.exit;
                        // Separate fan edges (inputs=entry) and convergence edge
                        std::vector<EdgePtr> fanEdges;
                        EdgePtr convEdge = nullptr;
                        for (auto e : region.internalEdges) {
                            if (!e) continue;
                            const auto& ins = e->getInputs();
                            if (ins.size() == 1 && ins[0] == entryNode) {
                                fanEdges.push_back(e);
                            } else {
                                convEdge = e;
                            }
                        }
                        if (!convEdge || fanEdges.size() < 2) continue;
                        const auto& convInputs = convEdge->getInputs();
                        if (convInputs.size() != fanEdges.size()) continue;
                        std::unordered_set<NodePtr> fanOutputs;
                        for (auto fe : fanEdges) {
                            if (!fe) continue;
                            fanOutputs.insert(view.getOutput(fe));
                        }
                        std::unordered_set<NodePtr> convSet(convInputs.begin(), convInputs.end());
                        if (convSet != fanOutputs) continue;
                        const auto& convNegs = convEdge->getBodyNegations();
                        bool convAllPos = std::all_of(convNegs.begin(), convNegs.end(), [](bool b){return !b;});
                        if (!convNegs.empty() && !convAllPos) continue;
                        bool firstNeg = false;
                        bool hasNegFlag = false;
                        bool mixedPolarity = false;
                        for (auto fe : fanEdges) {
                            if (!fe) continue;
                            const auto& negs = fe->getBodyNegations();
                            bool neg = (!negs.empty() && negs[0]);
                            if (!hasNegFlag) {
                                firstNeg = neg;
                                hasNegFlag = true;
                            } else if (firstNeg != neg) {
                                mixedPolarity = true;
                                break;
                            }
                        }
                        auto& edges = view.mutableEdges();
                        auto& nodes = view.mutableNodes();
                        size_t removedEdges = 0;
                        size_t removedNodes = 0;
                        auto removeFanAndConv = [&]() {
                            for (auto fe : fanEdges) {
                                if (!fe) continue;
                                removedEdges += edges.erase(fe);
                                NodePtr out = view.getOutput(fe);
                                if (out && view.getIncomingEdges(out).size() <= 1 && view.getOutgoingEdges(out).size() <= 1) {
                                    if (nodes.erase(out) > 0) ++removedNodes;
                                }
                            }
                            removedEdges += edges.erase(convEdge);
                        };
                        if (mixedPolarity) {
                            removeFanAndConv();
                            if (view.getIncomingEdges(entryNode).empty() && view.getOutgoingEdges(entryNode).empty()) {
                                if (nodes.erase(entryNode) > 0) ++removedNodes;
                            }
                            stats.numEdgesRemoved += removedEdges;
                            stats.numNodesRemoved += removedNodes;
                            view.invalidateCaches();
                            markDirtyRegion(region);
                            ++rewrittenThisRound;
                            ++stats.numRegionsRewritten;
                            continue;
                        }
                        double pEntry = entryNode->getProbability();
                        if (pEntry < 0.0) pEntry = 0.0;
                        if (pEntry > 1.0) pEntry = 1.0;
                        double p = firstNeg ? (1.0 - pEntry) : pEntry;
                        size_t regionRandomVars = 0;
                        if (pEntry > 0.0 && pEntry < 1.0) ++regionRandomVars;
                        for (auto fe : fanEdges) {
                            if (!fe) continue;
                            double pf = fe->getProbability();
                            if (pf < 0.0) pf = 0.0;
                            if (pf > 1.0) pf = 1.0;
                            p *= pf;
                            if (pf > 0.0 && pf < 1.0) ++regionRandomVars;
                        }
                        double pc = convEdge->getProbability();
                        if (pc < 0.0) pc = 0.0;
                        if (pc > 1.0) pc = 1.0;
                        p *= pc;
                        if (pc > 0.0 && pc < 1.0) ++regionRandomVars;
                        std::vector<SupportToken> entrySupport =
                                entryNode->getProbabilisticSupportTokens();
                        for (auto fe : fanEdges) {
                            if (!fe) continue;
                            entrySupport = mergeSupportTokenLists(
                                    {&entrySupport, &fe->getProbabilisticSupportTokens()});
                        }
                        entrySupport = mergeSupportTokenLists(
                                {&entrySupport, &convEdge->getProbabilisticSupportTokens()});

                        removeFanAndConv();

                        std::vector<NodePtr> newInputs = {entryNode};
                        std::vector<bool> newNeg = {firstNeg};
                        EdgePtr newEdge = graph.createHyperedge(newInputs, exitNode, nullptr, newNeg);
                        if (newEdge) {
                            newEdge->setProbability(1.0);
                            newEdge->clearProbabilisticSupportTokens();
                            edges.insert(newEdge);
                            stats.numEdgesAdded += 1;
                        }
                        entryNode->isFact = true;
                        entryNode->setProbability(p);
                        entryNode->setProbabilisticSupportTokens(std::move(entrySupport));

                        stats.totalRandomVars += regionRandomVars;
                        stats.maxRandomVars = std::max(stats.maxRandomVars, regionRandomVars);
                        stats.numEdgesRemoved += removedEdges;
                        stats.numNodesRemoved += removedNodes;
                        view.invalidateCaches();

                        markDirtyRegion(region);
                        if (newEdge) {
                            markDirtyEdgeEndpoints(newEdge);
                        }
                        ++rewrittenThisRound;
                        ++stats.numRegionsRewritten;
                        continue;
                    }
                    case SISORegionKind::General:
                        if (!generalRewriteEnabled) {
                            continue;
                        }
                        break;
                    default:
                        continue;
                }

                auto regionStart = std::chrono::steady_clock::now();

                if (!isRegionNonTrivial(region)) {
                    if (dumpStats) {
                        std::cout << "[GraphRewriter]   Skip trivial SISO: "
                                  << regionToString(region) << std::endl;
                    }
                    continue;
                }

                size_t regionRandomVars = countRandomVars(region);

                std::unordered_set<NodePtr> regionNodes(region.internalNodes.begin(),
                                                        region.internalNodes.end());
                std::unordered_set<EdgePtr> regionEdges(region.internalEdges.begin(),
                                                        region.internalEdges.end());
                SubgraphView regionView(std::move(regionNodes), std::move(regionEdges));

                RegionTiming timing;
                double condProb = 0.0;
                auto condStart = std::chrono::steady_clock::now();
                bool usedFastGeneralSummary = false;
                bool usedBddSummary = false;

                bool isSimple = isSimpleFactRegion(region);
                EdgePtr oldSimpleEdge = nullptr;
                if (isSimple && region.internalEdges.size() == 1) {
                    oldSimpleEdge = region.internalEdges.front();
                    if (oldSimpleEdge && simpleProcessedEdges_.count(oldSimpleEdge->getId()) > 0) {
                        if (dumpStats) {
                            std::cout << "[GraphRewriter]   Skip already processed simple SISO "
                                      << regionToString(region) << std::endl;
                        }
                        continue;
                    }
                }
                if (isSimple) {
                    // Fast path: entry fact -> single edge -> exit
                    EdgePtr onlyEdge = region.internalEdges.front();
                    double pEntry = region.entry->getProbability();
                    double pEdge = onlyEdge->getProbability();
                    if (pEdge < 0.0) pEdge = 0.0;
                    if (pEdge > 1.0) pEdge = 1.0;
                    condProb = pEntry * pEdge;
                    if (dumpStats) {
                        std::cout << "[GraphRewriter]   Simple fact region "
                                  << regionToString(region)
                                  << " with pEntry=" << pEntry
                                  << ", pEdge=" << pEdge
                                  << " => newPr=" << condProb << std::endl;
                    }
                    timing.mgrInitMs = 0.0;
                    timing.buildMs = 0.0;
                    timing.wmcMs = 0.0;
                    timing.applyMs = 0.0;
                } else {
                    usedFastGeneralSummary = region.kind == SISORegionKind::General &&
                            tryComputeFastConjConditionalProbability(
                                    region, regionView, condProb, dumpStats, &timing);
                    if (!usedFastGeneralSummary) {
                        usedBddSummary = true;
                        // Lazy init BDD manager when first needed.
                        if (!managerInitialized) {
                            auto managerStart = std::chrono::steady_clock::now();
                            auto config = makeLocalRewriteBddConfig();
                            bddManager = std::make_unique<WeightedBDDManager>(config);
                            auto managerEnd = std::chrono::steady_clock::now();
                            managerInitMs = std::chrono::duration<double, std::milli>(
                                                    managerEnd - managerStart)
                                                    .count();
                            managerInitialized = true;
                            if (dumpStats) {
                                std::cout << "[GraphRewriter] CUDD manager init took "
                                          << managerInitMs << " ms" << std::endl;
                            }
                        }
                        double effectiveMgrInitMs = firstRegionTiming ? managerInitMs : 0.0;
                        condProb = computeRegionConditionalProbability(
                            *bddManager, effectiveMgrInitMs, regionView, region.entry, region.exit, dumpStats, &timing);
                    }
                }
                double condMs = toMs(std::chrono::steady_clock::now() - condStart);
                loopCondMs += condMs;
                if (usedBddSummary) {
                    stats.totalBddManagerInitMs += timing.mgrInitMs;
                    stats.totalBddBuildMs += timing.buildMs;
                    stats.totalBddWmcMs += timing.wmcMs;
                    firstRegionTiming = false;
                } else if (usedFastGeneralSummary) {
                    stats.totalFastGeneralMs += timing.buildMs;
                }

                if (condProb <= 0.0) {
                    if (dumpStats) {
                        std::cout << "[GraphRewriter]   Skip SISO with Pr(exit|entry)=0: "
                                  << regionToString(region) << std::endl;
                    }
                    continue;
                }

                auto applyStart = std::chrono::steady_clock::now();
                EdgePtr newEdge = applyRegionRewrite(graph, view, region, condProb, stats, dumpStats, isSimple);
                double applyMs = toMs(std::chrono::steady_clock::now() - applyStart);
                timing.applyMs = applyMs;
                loopRewrittenMs += applyMs;
                if (!isSimple) {
                    stats.totalApplyMs += applyMs;
                    ++stats.numGeneralRegionsRewritten;
                    if (usedFastGeneralSummary) {
                        ++stats.numFastGeneralRegions;
                    }
                }

                if (dumpStats) {
                    auto regionMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - regionStart).count();
                    std::cout << "[GraphRewriter]   Region rewrite time: "
                              << regionMs << " ms for " << regionToString(region)
                              << " (apply=" << applyMs << " ms)"
                              << std::endl;
                    std::cout << "[GraphRewriter]     Steps: mgrInit=" << timing.mgrInitMs
                              << " ms, build=" << timing.buildMs
                              << " ms, WMC=" << timing.wmcMs
                              << " ms, apply=" << timing.applyMs << " ms";
                    if (!timing.roundTimingsMs.empty()) {
                        std::cout << ", rounds(ms)=";
                        for (size_t i = 0; i < timing.roundTimingsMs.size(); ++i) {
                            std::cout << (i == 0 ? "[" : ", ") << timing.roundTimingsMs[i];
                        }
                        std::cout << "]";
                    }
                    std::cout << ", BDD live nodes=" << timing.liveNodes
                              << ", mem=" << timing.memMb << " MB"
                              << ", randomVars=" << regionRandomVars
                              << ", kind=" << (isSimple ? "simple_fact" :
                                      (usedFastGeneralSummary ? "general_fast_conj" : "general_bdd"))
                              << std::endl;
                }
                markDirtyRegion(region);
                if (newEdge) {
                    markDirtyEdgeEndpoints(newEdge);
                }
                ++rewrittenThisRound;
                stats.totalRandomVars += regionRandomVars;
                stats.maxRandomVars = std::max(stats.maxRandomVars, regionRandomVars);
                if (isSimple) {
                    ++stats.simpleFactRegions;
                    if (oldSimpleEdge) {
                        simpleProcessedEdges_.insert(oldSimpleEdge->getId());
                    }
                    if (newEdge) {
                        simpleProcessedEdges_.insert(newEdge->getId());
                    }
                }
            }

            double loopMs = toMs(std::chrono::steady_clock::now() - loopStart);
            double loopSkipMs = loopMs - loopRewrittenMs;
            if (loopSkipMs < 0) loopSkipMs = 0.0;

            // After each SISO pass, compact edges by absorbing pure fact inputs into edge probability.
            double compactMs = 0.0;
            double edgeListMs = 0.0;
            size_t compactedEdges = 0, compactRemovedEdges = 0, compactAddedEdges = 0, compactRemovedNodes = 0;
            if (flags.enableCompaction) {
                auto compactStart = std::chrono::steady_clock::now();
                auto edgeListStart = std::chrono::steady_clock::now();
                std::vector<EdgePtr> edgeList;
                const double dirtyNodeRatio = view.getNodes().empty()
                                                      ? 0.0
                                                      : static_cast<double>(iterDirtyNodes.size()) /
                                                                static_cast<double>(view.getNodes().size());
                const double dirtyEdgeRatio = view.getEdges().empty()
                                                      ? 0.0
                                                      : static_cast<double>(iterDirtyEdges.size()) /
                                                                static_cast<double>(view.getEdges().size());
                const bool useDirtyCompaction = flags.restrictCompactionToDirty &&
                        (!iterDirtyNodes.empty() || !iterDirtyEdges.empty()) &&
                        dirtyNodeRatio <= 0.50 && dirtyEdgeRatio <= 0.50;
                if (useDirtyCompaction) {
                    std::unordered_set<EdgePtr> candidateEdges;
                    for (auto edge : iterDirtyEdges) {
                        if (edge && view.getEdges().count(edge) > 0) {
                            candidateEdges.insert(edge);
                        }
                    }
                    for (auto node : iterDirtyNodes) {
                        if (!node || view.getNodes().count(node) == 0) continue;
                        for (auto edge : view.getIncomingEdges(node)) {
                            if (edge && view.getEdges().count(edge) > 0) {
                                candidateEdges.insert(edge);
                            }
                        }
                        for (auto edge : view.getOutgoingEdges(node)) {
                            if (edge && view.getEdges().count(edge) > 0) {
                                candidateEdges.insert(edge);
                            }
                        }
                    }
                    edgeList.reserve(candidateEdges.size());
                    for (auto edge : candidateEdges) {
                        edgeList.push_back(edge);
                    }
                } else {
                    edgeList.reserve(view.getEdges().size());
                    for (auto edge : view.getEdges()) {
                        edgeList.push_back(edge);
                    }
                }
                edgeListMs = toMs(std::chrono::steady_clock::now() - edgeListStart);
                // Compaction only absorbs deterministic facts. The semantic
                // occurrence map is only consulted for probabilistic facts, so
                // avoid rebuilding it on every pass.
                const SemanticFactUseStats semanticFactStats;
                for (auto edge : edgeList) {
                    if (!edge) continue;
                    const auto& inputs = edge->getInputs();
                    if (inputs.empty()) continue;
                    std::vector<NodePtr> keepInputs;
                    std::vector<bool> keepNegs;
                    const auto& negs = edge->getBodyNegations();
                    double p = edge->getProbability();
                    std::vector<SupportToken> compactSupport =
                            edge->getProbabilisticSupportTokens();
                    assertRewriteProbability(p, "edge compaction base edge id=" + std::to_string(edge->getId()));
                    bool changed = false;
                    for (size_t idx = 0; idx < inputs.size(); ++idx) {
                        auto n = inputs[idx];
                        if (!n) continue;
                        if (n->isFact && !isProbabilisticFactNode(n) &&
                                canAbsorbFactLiteral(view, n, semanticFactStats, 1) &&
                                !edgeInputHasSupportOverlap(view, edge, idx, n->getProbabilisticSupportTokens())) {
                            double np = n->getProbability();
                            assertRewriteProbability(np, "edge compaction input fact id=" + std::to_string(n->getId()));
                            bool isNeg = (idx < negs.size() ? negs[idx] : false);
                            p *= isNeg ? (1.0 - np) : np;
                            compactSupport = mergeSupportTokenLists(
                                    {&compactSupport, &n->getProbabilisticSupportTokens()});
                            changed = true;
                        } else {
                            keepInputs.push_back(n);
                            keepNegs.push_back(idx < negs.size() ? negs[idx] : false);
                        }
                    }
                    // If nothing to absorb or no non-fact inputs remain, skip.
                    if (!changed || keepInputs.empty()) continue;

                    EdgePtr newEdge = graph.createHyperedge(
                            keepInputs, edge->getOutput(), nullptr, keepNegs, edge->getRuleApp());
                    if (!newEdge) continue;
                    assertRewriteProbability(p, "edge compaction new edge id=" + std::to_string(newEdge->getId()));
                    newEdge->setProbability(p);
                    newEdge->setProbabilisticSupportTokens(std::move(compactSupport));

                    auto& edges = view.mutableEdges();
                    if (edges.erase(edge) > 0) {
                        ++compactRemovedEdges;
                    }
                    edges.insert(newEdge);
                    ++compactAddedEdges;
                    if (flags.relaxCompactionDirty) {
                        // Relaxed dirtying keeps the frontier tied to the
                        // surviving compacted edge only. This avoids dragging
                        // the removed edge neighborhood back into the next
                        // detect pass after large deterministic compaction
                        // waves, which is the dominant cost on dense hosts such
                        // as the DDisasm function-inference benchmark.
                        markDirtyEdgeEndpoints(newEdge);
                    } else {
                        // Conservative fallback: keep the pre-relax behavior
                        // and dirty both the removed edge and the surviving
                        // compacted edge endpoints.
                        markDirtyEdgeEndpoints(edge);
                        markDirtyEdgeEndpoints(newEdge);
                    }

                    ++compactedEdges;
                }
                compactMs = toMs(std::chrono::steady_clock::now() - compactStart);
                stats.totalCompactionMs += compactMs;

                if (compactedEdges > 0) {
                    stats.numEdgesRemoved += compactRemovedEdges;
                    stats.numEdgesAdded += compactAddedEdges;
                    stats.numNodesRemoved += compactRemovedNodes;
                    view.invalidateCaches();
                    if (dumpStats) {
                        std::cout << "[GraphRewriter]   Edge compaction: compacted=" << compactedEdges
                                  << " removedEdges=" << compactRemovedEdges
                                  << " addedEdges=" << compactAddedEdges
                                  << " removedNodes=" << compactRemovedNodes
                                  << " time=" << compactMs << " ms"
                                  << std::endl;
                    }
                }
            }

            // Cleanup isolated fact/shadow nodes at end of iteration.
            double cleanupMs = 0.0;
            size_t cleanupRemovedNodes = 0;
            if (flags.enableCleanupIsolated) {
                auto cleanupStart = std::chrono::steady_clock::now();
                std::vector<NodePtr> nodesList;
                nodesList.reserve(view.getNodes().size());
                for (auto node : view.getNodes()) {
                    nodesList.push_back(node);
                }
                auto& nodes = view.mutableNodes();
                for (auto n : nodesList) {
                    if (!n) continue;
                    if (!n->isFact) continue;
                    if (n->needOutput || n->hasEvidence()) continue;
                    if (!view.getIncomingEdges(n).empty() || !view.getOutgoingEdges(n).empty()) continue;
                    nodes.erase(n);
                    ++cleanupRemovedNodes;
                }
                if (cleanupRemovedNodes > 0) {
                    stats.numNodesRemoved += cleanupRemovedNodes;
                    view.invalidateCaches();
                }
                cleanupMs = toMs(std::chrono::steady_clock::now() - cleanupStart);
                stats.totalCleanupMs += cleanupMs;
                if (dumpStats && cleanupRemovedNodes > 0) {
                    std::cout << "[GraphRewriter]   Cleanup isolated nodes: removed="
                              << cleanupRemovedNodes << " time=" << cleanupMs << " ms"
                              << std::endl;
                }
            }

            size_t precomputedNow = precomputeOutputFacts(view, evidenceAffectedNodes);
            if (dumpStats && precomputedNow > 0) {
                std::cout << "[GraphRewriter]   Precomputed output facts: "
                          << precomputedNow << std::endl;
            }

            auto countAfterStart = std::chrono::steady_clock::now();
            size_t iterRandomVarsAfter = countRandomVarsInView(view);
            double countAfterMs = toMs(std::chrono::steady_clock::now() - countAfterStart);

            auto preLogStart = std::chrono::steady_clock::now();

            stats.randomVarsAfter = iterRandomVarsAfter;
            auto iterDelta =
                    static_cast<long long>(iterRandomVarsBefore) - static_cast<long long>(iterRandomVarsAfter);
            double iterRatio = iterRandomVarsBefore == 0
                                       ? 0.0
                                       : static_cast<double>(iterRandomVarsAfter) /
                                                 static_cast<double>(iterRandomVarsBefore);

            double iterMs = toMs(std::chrono::steady_clock::now() - iterStart);

            double dumpAfterDotMs = 0.0;
            double preLogPrepMs = toMs(std::chrono::steady_clock::now() - preLogStart);
            auto logStart = std::chrono::steady_clock::now();

            std::cout << "[GraphRewriter] Iteration " << stats.numIterations
                      << " detected(single=" << detectedSingle
                      << ", linear=" << detectedLinear
                      << ", parallel=" << detectedParallel
                      << ", all-facts=" << detectedAllFacts
                      << ", fan-out=" << detectedFan
                      << ", general=" << detectedGeneral
                      << "), rewritten(total=" << rewrittenThisRound
                      << ") in " << iterMs << " ms" << std::endl;
            std::cout << "[GraphRewriter]   timings(ms): total=" << iterMs
                      << " countBefore=" << countBeforeMs
                      << " detect=" << detectMs
                      << " loop=" << loopMs
                      << " loopCond=" << loopCondMs
                      << " loopRewritten=" << loopRewrittenMs
                      << " loopSkip=" << loopSkipMs
                      << " edgeList=" << edgeListMs
                      << " compact=" << compactMs
                      << " cleanup=" << cleanupMs
                      << " countAfter=" << countAfterMs
                      << " preLog=" << preLogPrepMs
                      << " dumpRegions=" << dumpRegionsMs;
            double logMs = toMs(std::chrono::steady_clock::now() - logStart);
            double remainderMs = iterMs - countBeforeMs
                    - detectMs
                    - loopMs
                    - dumpRegionsMs
                    - dumpBeforeDotMs
                    - dumpAfterDotMs
                    - edgeListMs
                    - compactMs
                    - cleanupMs
                    - countAfterMs
                    - preLogPrepMs
                    - logMs;
            if (remainderMs < 0) remainderMs = 0.0;
            std::cout << " log=" << logMs
                      << " other=" << remainderMs
                      << " (apply includes dot if dumpdot)" << std::endl;
            std::cout << "[GraphRewriter]   random vars: before=" << iterRandomVarsBefore
                      << ", after=" << iterRandomVarsAfter
                      << ", delta=" << iterDelta
                      << ", ratio=" << iterRatio << std::endl;

            if (rewrittenThisRound == 0) {
                if (dumpStats) {
                    std::cout << "[GraphRewriter] No region rewritten in iteration "
                              << stats.numIterations << " (rewrite fixpoint reached in "
                              << iterMs << " ms)." << std::endl;
                }
                const auto* splitSeedNodes = hasDetectDirty ? &detectDirtyNodes : nullptr;
                const auto* splitSeedEdges = hasDetectDirty ? &detectDirtyEdges : nullptr;
                if (runSplitPass(splitSeedNodes, splitSeedEdges, &iterDirtyNodes, &iterDirtyEdges)) {
                    previousPassOnlyLinearParallel = false;
                    previousPassOnlyFactAbsorption = false;
                    hasDetectDirty = !iterDirtyNodes.empty() || !iterDirtyEdges.empty();
                    if (hasDetectDirty) {
                        detectDirtyNodes.swap(iterDirtyNodes);
                        detectDirtyEdges.swap(iterDirtyEdges);
                    } else {
                        detectDirtyNodes.clear();
                        detectDirtyEdges.clear();
                    }
                    continue;
                }
                break;
            }

            hasDetectDirty = !iterDirtyNodes.empty() || !iterDirtyEdges.empty();
            previousPassOnlyLinearParallel = rewrittenThisRound > 0 && detectedSingle == 0 &&
                    detectedAllFacts == 0 && detectedFan == 0 && detectedGeneral == 0 &&
                    detectedLinear + detectedParallel == rewrittenThisRound;
            previousPassOnlyFactAbsorption = rewrittenThisRound > 0 && detectedLinear == 0 &&
                    detectedParallel == 0 && detectedFan == 0 && detectedGeneral == 0 &&
                    detectedSingle + detectedAllFacts == rewrittenThisRound;
            if (hasDetectDirty) {
                detectDirtyNodes.swap(iterDirtyNodes);
                detectDirtyEdges.swap(iterDirtyEdges);
            } else {
                detectDirtyNodes.clear();
                detectDirtyEdges.clear();
            }

            stats.numRegionsRewritten += rewrittenThisRound;

            if (dumpDot) {
                if (rewrittenThisRound > 0) {
                    std::ostringstream dotAfter;
                    dotAfter << "rewrite_iter" << stats.numIterations << "_after.dot";
                    auto dotAfterStart = std::chrono::steady_clock::now();
                    view.dumpDot(dotAfter.str());
                    dumpAfterDotMs = toMs(std::chrono::steady_clock::now() - dotAfterStart);
                    if (dumpStats) {
                        std::cout << "[GraphRewriter]   dumpDot(after) took "
                                  << dumpAfterDotMs << " ms" << std::endl;
                    }
                }
            }
            if (dumpStats) {
                std::cout << "[GraphRewriter]   Rewrote " << rewrittenThisRound
                          << " region(s) in iteration " << stats.numIterations
                          << " in " << iterMs << " ms. Current stats: "
                          << "nodesRemoved=" << stats.numNodesRemoved
                          << ", edgesRemoved=" << stats.numEdgesRemoved
                          << ", edgesAdded=" << stats.numEdgesAdded
                          << std::endl;
            }
        }

        if (stats.numRegionsRewritten > 0) {
            double avgRandomVars = static_cast<double>(stats.totalRandomVars) /
                    static_cast<double>(stats.numRegionsRewritten);
            std::cout << "[GraphRewriter] Avg random vars per region: "
                      << avgRandomVars << " (max=" << stats.maxRandomVars << ")"
                      << std::endl;
        }

        auto rewriteTotalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - rewriteStart)
                                      .count();
        std::cout << "[GraphRewriter] Rewrite total: " << rewriteTotalMs
                  << " ms over " << stats.numIterations << " iterations"
                  << " (randomVars " << stats.randomVarsBefore << " -> " << stats.randomVarsAfter << ")"
                  << std::endl;

        return stats;
    }

private:
    struct SplitStats {
        size_t nodesAdded = 0;
        size_t edgesRewritten = 0;
        double elapsedMs = 0.0;
    };

    struct RegionTiming {
        double mgrInitMs = 0.0;
        double buildMs = 0.0;
        double wmcMs = 0.0;
        double applyMs = 0.0;
        std::vector<double> roundTimingsMs;
        size_t liveNodes = 0;
        double memMb = 0.0;
    };

    static WeightedBDDManager::InitConfig makeLocalRewriteBddConfig() {
        WeightedBDDManager::InitConfig config;
        config.numVars = 32;
        config.numSlots = 512;
        config.cacheSize = 1u << 18;
        config.maxMemory = 1024UL * 1024 * 1024;
        return config;
    }

    static const char* splitModeToString(SplitMode mode) {
        switch (mode) {
            case SplitMode::None: return "none";
            case SplitMode::Naive: return "naive";
            case SplitMode::Complete: return "complete";
        }
        return "unknown";
    }

    static bool isRandomVarNode(const NodePtr& node) {
        if (!node) return false;
        double p = node->getProbability();
        return p > 0.0 && p < 1.0;
    }

    static bool isRandomVarEdge(const EdgePtr& edge) {
        if (!edge) return false;
        double p = edge->getProbability();
        return p > 0.0 && p < 1.0;
    }

    static bool addIndependentSupportTokens(const std::vector<SupportToken>& tokens,
            const std::unordered_set<SupportToken>& blocked,
            std::unordered_set<SupportToken>& seen) {
        for (SupportToken token : tokens) {
            if (blocked.count(token) > 0) {
                return false;
            }
            if (!seen.insert(token).second) {
                return false;
            }
        }
        return true;
    }

    static bool fastConjSupportsIndependent(const SISORegionInfo& region) {
        std::unordered_set<SupportToken> entrySupport;
        if (region.entry) {
            for (SupportToken token : region.entry->getProbabilisticSupportTokens()) {
                entrySupport.insert(token);
            }
        }

        std::unordered_set<SupportToken> seenSupport;
        for (const auto& node : region.internalNodes) {
            if (!node || node == region.entry || node == region.exit || !isRandomVarNode(node)) {
                continue;
            }
            if (!addIndependentSupportTokens(
                        node->getProbabilisticSupportTokens(), entrySupport, seenSupport)) {
                return false;
            }
        }
        for (const auto& edge : region.internalEdges) {
            if (!edge || !isRandomVarEdge(edge)) {
                continue;
            }
            if (!addIndependentSupportTokens(
                        edge->getProbabilisticSupportTokens(), entrySupport, seenSupport)) {
                return false;
            }
        }
        return true;
    }

    std::unordered_set<NodePtr> collectEvidenceAffectedNodes(const WorkingSubgraphView& view) const {
        std::unordered_set<NodePtr> affected;
        // Most benchmark/timing runs do not use evidence at all. In that common
        // case, avoid constructing the cycle-dependency graph just to discover
        // that no SCC is evidence-constrained.
        if (view.getEvidenceNodes().empty()) {
            return affected;
        }
        auto& depGraph = view.getCycleDependencyGraph();
        size_t componentCount = depGraph.getComponentCount();
        if (componentCount == 0) {
            return affected;
        }
        std::vector<char> componentHasEvidence(componentCount, 0);
        for (size_t cid = 0; cid < componentCount; ++cid) {
            if (!depGraph.getComponentEvidences(cid).empty()) {
                componentHasEvidence[cid] = 1;
            }
        }
        for (const auto& node : view.getNodes()) {
            if (!node) continue;
            if (componentHasEvidence[depGraph.getComponentId(node)]) {
                affected.insert(node);
            }
        }
        return affected;
    }

    static NodePtr createShadowFact(WorkingDerivationGraph& graph, const NodePtr& fact,
            const EdgePtr& edgeHint) {
        if (!fact || !edgeHint) return nullptr;
        UntypedTuple shadowTuple;
        const auto& origTuple = fact->getTuple();
        shadowTuple.relation_name =
                "Shadow_" + origTuple.relation_name + "_" +
                std::to_string(fact->getId()) + "_" + std::to_string(edgeHint->getId());
        shadowTuple.fields = origTuple.fields;
        NodePtr shadow = graph.createNode(shadowTuple);
        if (!shadow) return nullptr;
        shadow->setProbability(fact->getProbability());
        shadow->isFact = true;
        shadow->needOutput = false;
        shadow->isShadow = true;
        shadow->setOriginalFact(fact->isOriginalFactNode());
        shadow->setSemanticFactId(fact->getSemanticFactId());
        shadow->setProbabilisticSupportTokens(fact->getProbabilisticSupportTokens());
        return shadow;
    }

    struct SemanticFactUseStats {
        std::unordered_map<std::size_t, std::size_t> inputOccurrences;
        std::unordered_map<std::size_t, std::size_t> pinnedNodes;
    };

    static bool isProbabilisticFactNode(const NodePtr& node) {
        return node && node->isFact && node->getProbability() > 0.0 && node->getProbability() < 1.0;
    }

    static SemanticFactUseStats buildSemanticFactUseStats(const WorkingSubgraphView& view) {
        SemanticFactUseStats stats;
        for (auto node : view.getNodes()) {
            if (!isProbabilisticFactNode(node)) continue;
            if (node->needOutput || node->hasEvidence()) {
                ++stats.pinnedNodes[node->getSemanticFactId()];
            }
        }
        for (auto edge : view.getEdges()) {
            if (!edge) continue;
            for (auto input : edge->getInputs()) {
                if (!isProbabilisticFactNode(input)) continue;
                ++stats.inputOccurrences[input->getSemanticFactId()];
            }
        }
        return stats;
    }

    static bool canAbsorbFactLiteral(const WorkingSubgraphView& view, const NodePtr& node,
            const SemanticFactUseStats& semanticStats, std::size_t localOccurrences = 1) {
        if (!node || !node->isFact || node->hasEvidence() || node->needOutput) {
            return false;
        }
        if (!view.getIncomingEdges(node).empty()) {
            return false;
        }
        if (!isProbabilisticFactNode(node)) {
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

    static bool canPrecomputeOutputFact(const WorkingSubgraphView& view, const NodePtr& node,
            const std::unordered_set<NodePtr>& evidenceAffectedNodes) {
        if (!node || !node->needOutput || !node->isFact) return false;
        if (node->hasEvidence()) return false;
        if (evidenceAffectedNodes.count(node)) return false;
        if (!view.getIncomingEdges(node).empty()) return false;
        if (precomputedProbResult.count(node)) return false;
        return true;
    }

    static size_t precomputeOutputFacts(WorkingSubgraphView& view,
            const std::unordered_set<NodePtr>& evidenceAffectedNodes) {
        size_t count = 0;
        for (auto node : view.getNodes()) {
            if (!canPrecomputeOutputFact(view, node, evidenceAffectedNodes)) {
                continue;
            }
            precomputedProbResult[node] = node->getProbability();
            node->needOutput = false;
            node->isQuery = false;
            ++count;
        }
        return count;
    }

    SplitStats splitFanoutNaive(WorkingDerivationGraph& graph, WorkingSubgraphView& view,
            GraphRewriteStats& stats,
            const std::unordered_set<NodePtr>& evidenceAffectedNodes,
            const std::unordered_set<NodePtr>* splitSeedNodes = nullptr,
            const std::unordered_set<EdgePtr>* splitSeedEdges = nullptr,
            std::unordered_set<NodePtr>* dirtyNodes = nullptr,
            std::unordered_set<EdgePtr>* dirtyEdges = nullptr) const {
        SplitStats out;
        auto splitStart = std::chrono::steady_clock::now();
        constexpr size_t kMaxReachable = 50;
        std::vector<NodePtr> factScan;
        if ((splitSeedNodes && !splitSeedNodes->empty()) || (splitSeedEdges && !splitSeedEdges->empty())) {
            // Split opportunities only change near facts/edges rewritten in the
            // previous iteration; re-scanning the whole graph here dominated the
            // final no-region tail on large DDisasm-like hosts.
            std::unordered_set<NodePtr> candidateFacts;
            if (splitSeedNodes) {
                for (auto node : *splitSeedNodes) {
                    if (node && node->isFact && view.getNodes().count(node) > 0) {
                        candidateFacts.insert(node);
                    }
                }
            }
            if (splitSeedEdges) {
                for (auto edge : *splitSeedEdges) {
                    if (!edge || view.getEdges().count(edge) == 0) continue;
                    NodePtr outNode = view.getOutput(edge);
                    if (outNode && outNode->isFact && view.getNodes().count(outNode) > 0) {
                        candidateFacts.insert(outNode);
                    }
                    for (auto input : edge->getInputs()) {
                        if (input && input->isFact && view.getNodes().count(input) > 0) {
                            candidateFacts.insert(input);
                        }
                    }
                }
            }
            factScan.assign(candidateFacts.begin(), candidateFacts.end());
        } else {
            factScan.reserve(view.getNodes().size());
            for (auto node : view.getNodes()) {
                if (node && node->isFact) {
                    factScan.push_back(node);
                }
            }
        }
        for (auto fact : factScan) {
            if (!fact || !fact->isFact || fact->hasEvidence() || fact->needOutput) continue;
            if (evidenceAffectedNodes.count(fact)) continue;
            const auto& outs = view.getOutgoingEdges(fact);
            if (outs.size() < 2) continue;

            // Compute reachable sets for each outgoing edge's output.
            std::vector<std::unordered_set<NodePtr>> reachSets;
            reachSets.reserve(outs.size());
            bool skipFact = false;
            for (auto e : outs) {
                if (!e) {
                    skipFact = true;
                    break;
                }
                NodePtr start = view.getOutput(e);
                if (!start) {
                    skipFact = true;
                    break;
                }
                std::unordered_set<NodePtr> visited;
                std::queue<NodePtr> q;
                visited.insert(start);
                q.push(start);
                while (!q.empty()) {
                    NodePtr cur = q.front();
                    q.pop();
                    const auto& nextEdges = view.getOutgoingEdges(cur);
                    for (auto ne : nextEdges) {
                        if (!ne) continue;
                        NodePtr outNode = view.getOutput(ne);
                        if (!outNode) continue;
                        if (visited.insert(outNode).second) {
                            if (visited.size() > kMaxReachable) {
                                skipFact = true;
                                break;
                            }
                            q.push(outNode);
                        }
                    }
                    if (skipFact) break;
                }
                if (skipFact) break;
                reachSets.push_back(std::move(visited));
            }
            if (skipFact || reachSets.size() != outs.size()) continue;

            // Identify branches disjoint from all others by marking per-node branch ownership.
            // This avoids O(k^2) pairwise set-intersection checks across branch reachability sets.
            size_t totalReachableNodes = 0;
            for (const auto& set : reachSets) {
                totalReachableNodes += set.size();
            }
            std::unordered_map<NodePtr, int> nodeOwner;
            nodeOwner.reserve(totalReachableNodes * 2 + 1);

            std::vector<char> hasOverlap(reachSets.size(), 0);
            for (size_t i = 0; i < reachSets.size(); ++i) {
                for (auto n : reachSets[i]) {
                    auto [it, inserted] = nodeOwner.emplace(n, static_cast<int>(i));
                    if (inserted) continue;
                    const int prevOwner = it->second;
                    if (prevOwner == static_cast<int>(i)) continue;
                    hasOverlap[i] = 1;
                    if (prevOwner >= 0) {
                        hasOverlap[static_cast<size_t>(prevOwner)] = 1;
                        it->second = -1;
                    }
                }
            }

            std::vector<size_t> independentIdx;
            independentIdx.reserve(reachSets.size());
            for (size_t i = 0; i < hasOverlap.size(); ++i) {
                if (!hasOverlap[i]) {
                    independentIdx.push_back(i);
                }
            }
            if (independentIdx.empty()) continue;

            auto& edges = view.mutableEdges();
            auto& nodes = view.mutableNodes();
            bool changed = false;

            for (size_t idx : independentIdx) {
                EdgePtr edge = outs[idx];
                if (!edge) continue;
                const auto& inputs = edge->getInputs();
                if (inputs.empty()) continue;
                const auto& negs = edge->getBodyNegations();

                NodePtr shadow = createShadowFact(graph, fact, edge);
                if (!shadow) continue;

                std::vector<NodePtr> newInputs(inputs.begin(), inputs.end());
                bool replaced = false;
                for (size_t k = 0; k < newInputs.size(); ++k) {
                    if (newInputs[k] == fact) {
                        newInputs[k] = shadow;
                        replaced = true;
                    }
                }
                if (!replaced) continue;

                EdgePtr newEdge = graph.createHyperedge(
                        newInputs, edge->getOutput(), edge->getRule(), negs, edge->getRuleApp());
                if (!newEdge) continue;
                newEdge->setProbability(edge->getProbability());

                if (edges.erase(edge) > 0) {
                    ++out.edgesRewritten;
                    ++stats.numEdgesRemoved;
                }
                edges.insert(newEdge);
                ++stats.numEdgesAdded;
                nodes.insert(shadow);
                ++out.nodesAdded;
                if (dirtyNodes) {
                    dirtyNodes->insert(fact);
                    dirtyNodes->insert(shadow);
                    dirtyNodes->insert(edge->getOutput());
                    dirtyNodes->insert(newEdge->getOutput());
                    for (auto in : edge->getInputs()) dirtyNodes->insert(in);
                    for (auto in : newEdge->getInputs()) dirtyNodes->insert(in);
                }
                if (dirtyEdges) {
                    dirtyEdges->insert(edge);
                    dirtyEdges->insert(newEdge);
                }
                changed = true;
            }
            if (changed) {
                view.invalidateCaches();
            }
        }
        out.elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - splitStart).count();
        return out;
    }

    SplitStats splitFanoutComplete(WorkingDerivationGraph& graph, WorkingSubgraphView& view,
            GraphRewriteStats& stats, const RewriteFeatureFlags& flags,
            const std::unordered_set<NodePtr>& evidenceAffectedNodes,
            std::unordered_set<NodePtr>* dirtyNodes = nullptr,
            std::unordered_set<EdgePtr>* dirtyEdges = nullptr) const {
        SplitStats out;
        auto splitStart = std::chrono::steady_clock::now();
        const auto& nodeSet = view.getNodes();
        if (nodeSet.empty()) {
            out.elapsedMs = 0.0;
            return out;
        }

        std::vector<NodePtr> nodesList;
        nodesList.reserve(nodeSet.size());
        for (const auto& n : nodeSet) {
            nodesList.push_back(n);
        }

        std::unordered_map<NodePtr, size_t> nodeIndex;
        nodeIndex.reserve(nodesList.size() * 2);
        for (size_t i = 0; i < nodesList.size(); ++i) {
            if (nodesList[i]) nodeIndex[nodesList[i]] = i;
        }

        std::vector<char> hasRvReach(nodesList.size(), 0);
        std::queue<size_t> q;
        auto markNode = [&](const NodePtr& n) {
            auto it = nodeIndex.find(n);
            if (it == nodeIndex.end()) return;
            size_t idx = it->second;
            if (!hasRvReach[idx]) {
                hasRvReach[idx] = 1;
                q.push(idx);
            }
        };

        for (const auto& n : nodesList) {
            if (isRandomVarNode(n)) {
                markNode(n);
            }
        }
        for (const auto& e : view.getEdges()) {
            if (!isRandomVarEdge(e)) continue;
            for (const auto& in : e->getInputs()) {
                markNode(in);
            }
        }
        while (!q.empty()) {
            size_t idx = q.front();
            q.pop();
            NodePtr cur = nodesList[idx];
            if (!cur) continue;
            for (auto inEdge : view.getIncomingEdges(cur)) {
                if (!inEdge) continue;
                for (auto in : inEdge->getInputs()) {
                    markNode(in);
                }
            }
        }

        struct UnionFind {
            std::vector<int> parent;
            std::vector<int> size;
            explicit UnionFind(size_t n) : parent(n), size(n, 1) {
                for (size_t i = 0; i < n; ++i) parent[i] = static_cast<int>(i);
            }
            int find(int x) {
                if (parent[x] == x) return x;
                parent[x] = find(parent[x]);
                return parent[x];
            }
            void unite(int a, int b) {
                a = find(a);
                b = find(b);
                if (a == b) return;
                if (size[a] < size[b]) std::swap(a, b);
                parent[b] = a;
                size[a] += size[b];
            }
        };

        std::vector<int> seenToken(nodesList.size(), 0);
        std::vector<int> owner(nodesList.size(), 0);
        std::vector<int> rep(nodesList.size(), 0);
        int token = 1;

        auto& edges = view.mutableEdges();
        auto& nodes = view.mutableNodes();

        const size_t maxGroups = flags.splitMaxGroupsPerNode;
        const size_t minGroupEdges = std::max<size_t>(1, flags.splitMinGroupEdges);
        size_t remainingNodes = flags.splitMaxNewNodesPerPass;
        size_t remainingEdges = flags.splitMaxNewEdgesPerPass;

        if (maxGroups < 2 || remainingNodes == 0 || remainingEdges == 0) {
            out.elapsedMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - splitStart).count();
            return out;
        }

        // Simple candidate queue seeded once per split pass; can be made more selective later.
        std::deque<NodePtr> queue;
        std::unordered_set<NodePtr> inQueue;
        auto enqueueFact = [&](const NodePtr& n) {
            if (!n || !n->isFact || n->hasEvidence() || n->needOutput) return;
            if (evidenceAffectedNodes.count(n)) return;
            if (view.getOutgoingEdges(n).size() < 2) return;
            auto it = nodeIndex.find(n);
            if (it == nodeIndex.end()) return;
            if (!hasRvReach[it->second]) return;
            if (inQueue.insert(n).second) {
                queue.push_back(n);
            }
        };

        for (auto fact : nodesList) {
            enqueueFact(fact);
        }

        while (!queue.empty()) {
            if (remainingNodes == 0 || remainingEdges == 0) break;
            NodePtr fact = queue.front();
            queue.pop_front();
            inQueue.erase(fact);
            if (!fact || !fact->isFact || fact->hasEvidence() || fact->needOutput) continue;
            if (evidenceAffectedNodes.count(fact)) continue;
            if (view.getNodes().count(fact) == 0) continue;
            const auto& outs = view.getOutgoingEdges(fact);
            if (outs.size() < 2) continue;
            auto itFact = nodeIndex.find(fact);
            if (itFact == nodeIndex.end()) continue;
            if (!hasRvReach[itFact->second]) continue;

            const int kNone = -1;
            const int kMulti = -2;
            ++token;
            if (token == std::numeric_limits<int>::max()) {
                token = 1;
                std::fill(seenToken.begin(), seenToken.end(), 0);
            }

            UnionFind uf(outs.size());
            std::queue<size_t> work;

            auto touchNode = [&](size_t idx) {
                if (seenToken[idx] != token) {
                    seenToken[idx] = token;
                    owner[idx] = kNone;
                    rep[idx] = kNone;
                }
            };

            auto assign = [&](size_t idx, int incomingOwner, int incomingRep) {
                touchNode(idx);
                if (owner[idx] == kNone) {
                    owner[idx] = incomingOwner;
                    rep[idx] = (incomingOwner == kMulti) ? incomingRep : incomingOwner;
                    work.push(idx);
                    return;
                }
                if (owner[idx] == incomingOwner) return;

                int oldRep = rep[idx];
                if (owner[idx] != kMulti) {
                    owner[idx] = kMulti;
                    rep[idx] = oldRep;
                    work.push(idx);
                }
                // Merge on any intersection to preserve correlations across branches.
                int incRep = (incomingOwner == kMulti) ? incomingRep : incomingOwner;
                uf.unite(oldRep, incRep);
            };

            for (size_t i = 0; i < outs.size(); ++i) {
                EdgePtr e = outs[i];
                if (!e) continue;
                NodePtr v = view.getOutput(e);
                if (!v) continue;
                auto it = nodeIndex.find(v);
                if (it == nodeIndex.end()) continue;
                assign(it->second, static_cast<int>(i), static_cast<int>(i));
            }

            while (!work.empty()) {
                size_t idx = work.front();
                work.pop();
                int ox = owner[idx];
                int rx = rep[idx];
                NodePtr node = nodesList[idx];
                if (!node) continue;
                for (auto eId : view.getOutgoingEdges(node)) {
                    if (!eId) continue;
                    NodePtr outNode = view.getOutput(eId);
                    if (!outNode) continue;
                    auto itOut = nodeIndex.find(outNode);
                    if (itOut == nodeIndex.end()) continue;
                    if (ox == kMulti) {
                        assign(itOut->second, kMulti, rx);
                    } else {
                        assign(itOut->second, ox, ox);
                    }
                }
            }

            std::unordered_map<int, size_t> rootIndex;
            std::vector<std::vector<EdgePtr>> groups;
            groups.reserve(outs.size());
            for (size_t i = 0; i < outs.size(); ++i) {
                int root = uf.find(static_cast<int>(i));
                auto it = rootIndex.find(root);
                if (it == rootIndex.end()) {
                    rootIndex[root] = groups.size();
                    groups.push_back({});
                    it = rootIndex.find(root);
                }
                groups[it->second].push_back(outs[i]);
            }

            if (groups.size() <= 1) continue;
            std::sort(groups.begin(), groups.end(),
                    [](const std::vector<EdgePtr>& a, const std::vector<EdgePtr>& b) {
                        return a.size() > b.size();
                    });

            if (minGroupEdges > 1 && groups.size() > 1) {
                std::vector<std::vector<EdgePtr>> filtered;
                filtered.reserve(groups.size());
                filtered.push_back(std::move(groups[0]));
                for (size_t i = 1; i < groups.size(); ++i) {
                    if (groups[i].size() < minGroupEdges) {
                        filtered[0].insert(filtered[0].end(), groups[i].begin(), groups[i].end());
                    } else {
                        filtered.push_back(std::move(groups[i]));
                    }
                }
                groups.swap(filtered);
            }

            if (groups.size() <= 1) continue;
            if (groups.size() > maxGroups) {
                for (size_t i = maxGroups; i < groups.size(); ++i) {
                    groups[0].insert(groups[0].end(), groups[i].begin(), groups[i].end());
                }
                groups.resize(maxGroups);
            }

            if (groups.size() <= 1) continue;
            std::vector<std::vector<EdgePtr>> selected;
            selected.reserve(groups.size());
            selected.push_back(std::move(groups[0]));
            for (size_t i = 1; i < groups.size(); ++i) {
                const auto& group = groups[i];
                if (remainingNodes == 0 || remainingEdges < group.size()) {
                    selected[0].insert(selected[0].end(), group.begin(), group.end());
                    continue;
                }
                selected.push_back(std::move(groups[i]));
                --remainingNodes;
                remainingEdges -= selected.back().size();
            }
            groups.swap(selected);
            if (groups.size() <= 1) continue;

            bool changed = false;
            for (size_t gi = 1; gi < groups.size(); ++gi) {
                const auto& group = groups[gi];
                if (group.empty()) continue;
                NodePtr shadow = createShadowFact(graph, fact, group.front());
                if (!shadow) continue;
                size_t rewired = 0;
                for (auto edge : group) {
                    if (!edge) continue;
                    const auto& inputs = edge->getInputs();
                    if (inputs.empty()) continue;
                    const auto& negs = edge->getBodyNegations();

                    std::vector<NodePtr> newInputs(inputs.begin(), inputs.end());
                    bool replaced = false;
                    for (size_t k = 0; k < newInputs.size(); ++k) {
                        if (newInputs[k] == fact) {
                            newInputs[k] = shadow;
                            replaced = true;
                        }
                    }
                    if (!replaced) continue;

                    EdgePtr newEdge = graph.createHyperedge(
                            newInputs, edge->getOutput(), edge->getRule(), negs, edge->getRuleApp());
                    if (!newEdge) continue;
                    newEdge->setProbability(edge->getProbability());

                    if (edges.erase(edge) > 0) {
                        ++out.edgesRewritten;
                        ++stats.numEdgesRemoved;
                        ++rewired;
                    }
                    edges.insert(newEdge);
                    ++stats.numEdgesAdded;
                    if (dirtyNodes) {
                        dirtyNodes->insert(fact);
                        dirtyNodes->insert(shadow);
                        dirtyNodes->insert(edge->getOutput());
                        dirtyNodes->insert(newEdge->getOutput());
                        for (auto in : edge->getInputs()) dirtyNodes->insert(in);
                        for (auto in : newEdge->getInputs()) dirtyNodes->insert(in);
                    }
                    if (dirtyEdges) {
                        dirtyEdges->insert(edge);
                        dirtyEdges->insert(newEdge);
                    }
                }
                if (rewired > 0) {
                    nodes.insert(shadow);
                    ++out.nodesAdded;
                    changed = true;
                }
            }

            if (changed) {
                view.invalidateCaches();
            }
        }

        out.elapsedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - splitStart).count();
        return out;
    }

    bool isRegionNonTrivial(const SISORegionInfo& region) const {
        constexpr size_t kDefaultMaxEdges = 5;
        size_t maxEdges = kDefaultMaxEdges;

        size_t edgeCount = region.internalEdges.size();
        size_t nodeCount = region.internalNodes.size();
        if (isSimpleFactRegion(region)) {
            return true;
        }
        if (edgeCount == 0 || nodeCount <= 2) {
            return false;
        }
        if (edgeCount > maxEdges) {
            return false;
        }
        return true;
    }

    size_t countRandomVars(const SISORegionInfo& region) const {
        size_t randomCount = 0;

        for (const auto& node : region.internalNodes) {
            if (!node) continue;
            if (node == region.entry || node == region.exit) {
                continue;
            }
            if (!node->isFact) {
                continue;
            }
            double p = node->getProbability();
            if (p > 0.0 && p < 1.0) {
                ++randomCount;
            }
        }

        for (const auto& edge : region.internalEdges) {
            if (!edge) continue;
            double p = edge->getProbability();
            if (p > 0.0 && p < 1.0) {
                ++randomCount;
            }
        }

        return randomCount;
    }

    static size_t countRandomVarsInView(const DerivationGraphViewInterface& view) {
        size_t randomCount = 0;

        for (const auto& node : view.getNodes()) {
            if (!node) continue;
            if (!node->isFact) {
                continue;
            }
            double p = node->getProbability();
            if (p > 0.0 && p < 1.0) {
                ++randomCount;
            }
        }

        for (const auto& edge : view.getEdges()) {
            if (!edge) continue;
            double p = edge->getProbability();
            if (p > 0.0 && p < 1.0) {
                ++randomCount;
            }
        }

        return randomCount;
    }

    static bool isSimpleFactRegion(const SISORegionInfo& region) {
        // simple chain: entry fact -> exit (single edge), only two nodes and one edge
        if (region.internalNodes.size() != 2) return false;
        if (region.internalEdges.size() != 1) return false;
        if (!region.entry || !region.exit) return false;
        if (!region.entry->isFact) return false;
        // ignore regions where entry==exit
        if (region.entry == region.exit) return false;
        return true;
    }

    static std::string regionToString(const SISORegionInfo& region) {
        std::ostringstream oss;
        oss << "[entry=";
        if (region.entry) {
            oss << region.entry->toString();
        } else {
            oss << "null";
        }
        oss << ", exit=";
        if (region.exit) {
            oss << region.exit->toString();
        } else {
            oss << "null";
        }
        oss << ", |nodes|=" << region.internalNodes.size()
            << ", |edges|=" << region.internalEdges.size()
            << "]";
        return oss.str();
    }

    bool tryComputeFastConjConditionalProbability(const SISORegionInfo& region,
            SubgraphView& regionView, double& condProb, bool debug,
            RegionTiming* timingOut = nullptr) const {
        if (!region.entry || !region.exit) {
            return false;
        }
        auto start = std::chrono::steady_clock::now();
        if (!fastConjSupportsIndependent(region)) {
            return false;
        }

        ConjFormulaManager manager;
        std::map<NodePtr, ConjNodeRef> nodeFormulas;
        std::map<EdgePtr, ConjNodeRef> edgeFormulas;
        std::unordered_set<NodePtr> seedTrue = {region.entry};

        buildFormulasCyclewise(regionView, manager, nodeFormulas, edgeFormulas, seedTrue);
        auto end = std::chrono::steady_clock::now();
        double evalMs = std::chrono::duration<double, std::milli>(end - start).count();

        if (!manager.isValid()) {
            return false;
        }
        auto itExit = nodeFormulas.find(region.exit);
        if (itExit == nodeFormulas.end() || !itExit->second.get()) {
            return false;
        }

        double pExitGivenEntry = manager.computeWeightedModelCount(itExit->second);
        if (pExitGivenEntry < 0.0) {
            pExitGivenEntry = 0.0;
        }
        if (pExitGivenEntry > 1.0) {
            pExitGivenEntry = 1.0;
        }
        condProb = pExitGivenEntry;

        if (timingOut) {
            timingOut->mgrInitMs = 0.0;
            timingOut->buildMs = evalMs;
            timingOut->wmcMs = 0.0;
            timingOut->roundTimingsMs.clear();
            timingOut->liveNodes = 0;
            timingOut->memMb = 0.0;
        }
        if (debug) {
            std::cout << "[GraphRewriter]   Fast conjunctive SISO "
                      << regionToString(region)
                      << " => Pr(exit|entry)=" << condProb
                      << " [eval " << evalMs << " ms]" << std::endl;
        }
        return true;
    }

    double computeRegionConditionalProbability(WeightedBDDManager& bddManager,
                                               double managerInitMs,
                                               SubgraphView& regionView,
                                               NodePtr entry,
                                               NodePtr exit,
                                               bool debug,
                                               RegionTiming* timingOut = nullptr) const {
        if (!entry || !exit) {
            return 0.0;
        }

        std::map<NodePtr, BddNodeRef> nodeFormulas;
        std::map<EdgePtr, BddNodeRef> edgeFormulas;
        std::unordered_set<NodePtr> seedTrue = { entry };
        std::vector<double> roundTimings;

        auto t0 = std::chrono::steady_clock::now();
        buildFormulasCyclewise(regionView, bddManager, nodeFormulas, edgeFormulas, seedTrue,
                               debug ? &roundTimings : nullptr);
        auto t1 = std::chrono::steady_clock::now();
        double buildMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        auto itExit = nodeFormulas.find(exit);
        if (itExit == nodeFormulas.end()) {
            if (debug) {
                std::cout << "[GraphRewriter]   WARNING: No formula for exit node "
                          << exit->toString() << " in region." << std::endl;
            }
            return 0.0;
        }

        auto itEntry = nodeFormulas.find(entry);
        if (itEntry == nodeFormulas.end()) {
            auto t2 = std::chrono::steady_clock::now();
            double pExit = bddManager.computeWeightedModelCount(itExit->second);
            auto t3 = std::chrono::steady_clock::now();
            double wmcMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
            if (timingOut) {
                timingOut->mgrInitMs = managerInitMs;
                timingOut->buildMs = buildMs;
                timingOut->wmcMs = wmcMs;
                timingOut->roundTimingsMs = roundTimings;
                timingOut->liveNodes = Cudd_ReadNodeCount(bddManager.getManager());
                timingOut->memMb = Cudd_ReadMemoryInUse(bddManager.getManager()) / (1024.0 * 1024);
            }
            if (debug) {
                std::cout << "[GraphRewriter]   Entry has no local formula; use Pr(exit)="
                          << pExit << " as conditional probability."
                          << " [mgrInit " << managerInitMs
                          << " ms, local build " << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                          << " ms]" << std::endl;
            }
            return pExit;
        }

        auto t2 = std::chrono::steady_clock::now();
        double pExit  = bddManager.computeWeightedModelCount(itExit->second);
        double pEntry = bddManager.computeWeightedModelCount(itEntry->second);
        auto t3 = std::chrono::steady_clock::now();
        double wmcMs = std::chrono::duration<double, std::milli>(t3 - t2).count();

        if (timingOut) {
            timingOut->mgrInitMs = managerInitMs;
            timingOut->buildMs = buildMs;
            timingOut->wmcMs = wmcMs;
            timingOut->roundTimingsMs = roundTimings;
            timingOut->liveNodes = Cudd_ReadNodeCount(bddManager.getManager());
            timingOut->memMb = Cudd_ReadMemoryInUse(bddManager.getManager()) / (1024.0 * 1024);
        }
        if (debug) {
            if (!roundTimings.empty()) {
                std::cout << "[GraphRewriter]   Forward compilation rounds (ms):";
                for (size_t i = 0; i < roundTimings.size(); ++i) {
                    std::cout << (i == 0 ? " " : ", ") << roundTimings[i];
                }
                std::cout << std::endl;
            }

            size_t liveNodes = Cudd_ReadNodeCount(bddManager.getManager());
            double memMb = Cudd_ReadMemoryInUse(bddManager.getManager()) / (1024.0 * 1024);

            std::cout << "[GraphRewriter]   Region "
                      << exit->toString() << " <- " << entry->toString()
                      << " : Pr(exit)=" << pExit
                      << ", Pr(entry)=" << pEntry
                      << " [mgrInit " << managerInitMs
                      << " ms, build " << buildMs
                      << " ms, WMC " << wmcMs
                      << " ms]"
                      << " ; BDD live nodes=" << liveNodes
                      << ", mem=" << memMb << " MB";
        }

        if (pEntry <= std::numeric_limits<double>::epsilon()) {
            if (debug) {
                std::cout << " (entryProb≈0, treat as 0)" << std::endl;
            }
            return 0.0;
        }

        double pCond = pExit / pEntry;

        if (pCond < 0.0) pCond = 0.0;
        if (pCond > 1.0) pCond = 1.0;

        if (debug) {
            std::cout << ", Pr(exit|entry)=" << pCond << std::endl;
        }

        return pCond;
    }

    EdgePtr applyRegionRewrite(WorkingDerivationGraph& graph,
                               WorkingSubgraphView& view,
                               const SISORegionInfo& region,
                               double condProb,
                               GraphRewriteStats& stats,
                               bool debug,
                               bool isSimpleFact = false) const {
        if (!region.entry || !region.exit) {
            return nullptr;
        }

        if (isSimpleFact && region.internalEdges.size() == 1) {
            // Fold only when SI has no other outgoing edges and SO has no other incoming edges,
            // and SO is not an output/query node; otherwise just update the edge prob.
            EdgePtr oldEdge = region.internalEdges.front();
            // Update the single edge probability (keep nodes/edge to preserve correlations).
            if (oldEdge) {
                oldEdge->setProbability(condProb);
            }
            if (debug) {
                std::cout << "[GraphRewriter]   Updated simple fact region edge "
                          << regionToString(region)
                          << " with newPr=" << condProb << " (no node/edge removal)."
                          << std::endl;
            }
            return oldEdge;
        }

        std::vector<NodePtr> inputs = { region.entry };
        EdgePtr newEdge = graph.createHyperedge(inputs, region.exit);
        if (!newEdge) {
            if (debug) {
                std::cout << "[GraphRewriter]   WARNING: createHyperedge failed for region "
                          << regionToString(region) << std::endl;
            }
            return nullptr;
        }

        newEdge->setProbability(condProb);

        SubgraphView& baseView = static_cast<SubgraphView&>(view);
        auto& nodes = baseView.mutableNodes();
        auto& edges = baseView.mutableEdges();

        edges.insert(newEdge);
        ++stats.numEdgesAdded;

        size_t removedEdges = 0;
        for (const auto& e : region.internalEdges) {
            if (edges.erase(e) > 0) {
                ++removedEdges;
            }
        }

        size_t removedNodes = 0;
        for (const auto& n : region.internalNodes) {
            if (!n) continue;
            if (n == region.entry || n == region.exit) {
                continue;
            }
            if (nodes.erase(n) > 0) {
                ++removedNodes;
            }
        }

        stats.numEdgesRemoved += removedEdges;
        stats.numNodesRemoved += removedNodes;

        view.invalidateCaches();

        if (debug) {
            std::cout << "[GraphRewriter]   Rewrote region "
                      << regionToString(region)
                      << " -> new edge id=" << newEdge->getId()
                      << " with Pr(exit|entry)=" << condProb
                      << " ; removed " << removedNodes << " node(s), "
                      << removedEdges << " edge(s)." << std::endl;
        }
        return newEdge;
    }

    mutable std::unordered_set<size_t> simpleProcessedEdges_;
};

}  // namespace souffle::problog
