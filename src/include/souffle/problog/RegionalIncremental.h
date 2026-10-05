#pragma once

#include <map>
#include <queue>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>
#include <type_traits>
#include <cmath>
#include <chrono>
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "souffle/problog/DerivationGraph.h"
#include "souffle/problog/IncRegionAnalyzer.h"
#include "souffle/problog/formula/FormulaManager.h"

template<typename FormulaManagerT, typename FormulaNodeRef>
static inline bool regionalInputFormulaLiteral(FormulaManagerT& formulaManager,
        const std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
        const NodePtr& node, bool negated, FormulaNodeRef& out) {
    auto it = nodeFormulas.find(node);
    if (it == nodeFormulas.end()) {
        return false;
    }
    out = negated ? formulaManager.makeNot(it->second) : it->second;
    return true;
}

struct IncRegionalOutputProfile {
    bool active = false;
    size_t overrideCount = 0;
    std::set<NodePtr> regionNodes;
    std::set<NodePtr> boundaryNodes;
    std::unordered_set<NodePtr> deltaReachableNodes;
    std::unordered_set<EdgePtr> deltaReachableEdges;
    std::unordered_map<int, std::pair<double, double>> originalWeights;
    std::unordered_map<int, std::pair<double, double>> overrideWeights;
    std::unordered_map<NodePtr, std::vector<int>> boundaryUpstreamAnchorVars;
    std::unordered_map<NodePtr, double> boundaryOutputTargets;

    void reset() {
        active = false;
        overrideCount = 0;
        regionNodes.clear();
        boundaryNodes.clear();
        deltaReachableNodes.clear();
        deltaReachableEdges.clear();
        originalWeights.clear();
        overrideWeights.clear();
        boundaryUpstreamAnchorVars.clear();
        boundaryOutputTargets.clear();
    }
};

inline IncRegionalOutputProfile incRegionalOutputProfile;

struct IncRegionalTurnSummary {
    bool valid = false;
    size_t boundaryTotal = 0;
    size_t regionNodeCount = 0;
    size_t deltaReachNodeCount = 0;
    bool usedCalibrationOverrides = false;
    bool trueRegionalReuse = false;

    void reset() {
        valid = false;
        boundaryTotal = 0;
        regionNodeCount = 0;
        deltaReachNodeCount = 0;
        usedCalibrationOverrides = false;
        trueRegionalReuse = false;
    }
};

inline IncRegionalTurnSummary incRegionalTurnSummary;

// forward declare the classic incremental builder to avoid header cycles
template<typename FormulaNodeRef>
void buildFormulasIncCyclewise(
    IncrementalDerivationGraphViewInterface& view,
    FormulaManager<FormulaNodeRef>& formulaManager,
    std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
    std::map<EdgePtr, FormulaNodeRef>& edgeFormulas,
    std::set<NodePtr>& changedNodes);

// Utility: ensure region is SCC-closed by expanding to include full SCCs of
// any node already inside the region.
struct RegionSccClosure {
    static bool closeToScc(
        incra::Region& R,
        const incra::Region& reachFilter,
        const CycleDependencyGraph& scc) {
        bool changed = false;
        std::unordered_set<size_t> touched;
        for (auto n : R.nodes) {
            auto it = scc.nodeToCycleIndex.find(n);
            if (it != scc.nodeToCycleIndex.end()) {
                touched.insert(it->second);
            }
        }
        for (auto cid : touched) {
            if (cid >= scc.nodeCycles.size()) continue;
            for (auto n : scc.nodeCycles[cid]) {
                if (!reachFilter.nodes.empty() && !reachFilter.nodes.count(n)) continue;
                if (R.nodes.insert(n).second) changed = true;
            }
            if (cid < scc.edgeCycles.size()) {
                for (auto e : scc.edgeCycles[cid]) {
                    if (!reachFilter.edges.empty() && !reachFilter.edges.count(e)) continue;
                    if (R.edges.insert(e).second) changed = true;
                }
            }
        }
        return changed;
    }
};

struct LocalDepGraph {
    SubgraphView view;
    CycleDependencyGraph depGraph;
    LocalDepGraph(std::unordered_set<NodePtr> nodes, std::unordered_set<EdgePtr> edges)
            : view(std::move(nodes), std::move(edges)), depGraph(view) {}
};

static bool regionTouchesCycle(
    IncrementalDerivationGraphViewInterface& view,
    const std::unordered_set<NodePtr>& regionNodes) {
    if (regionNodes.empty()) {
        return false;
    }
    std::unordered_map<NodePtr, uint8_t> state;
    state.reserve(regionNodes.size());
    std::vector<NodePtr> stack;
    stack.reserve(regionNodes.size());
    std::unordered_map<NodePtr, size_t> stackIndex;
    stackIndex.reserve(regionNodes.size());

    auto dfs = [&](auto&& self, const NodePtr& node) -> bool {
        state[node] = 1;
        stackIndex[node] = stack.size();
        stack.push_back(node);
        for (const auto& edge : view.getOutgoingEdges(node)) {
            NodePtr out = view.getOutput(edge);
            if (!out) continue;
            auto it = state.find(out);
            if (it == state.end()) {
                if (self(self, out)) {
                    return true;
                }
            } else if (it->second == 1) {
                auto sit = stackIndex.find(out);
                if (sit == stackIndex.end()) {
                    continue;
                }
                for (size_t i = sit->second; i < stack.size(); ++i) {
                    if (regionNodes.count(stack[i])) {
                        return true;
                    }
                }
            }
        }
        stack.pop_back();
        stackIndex.erase(node);
        state[node] = 2;
        return false;
    };

    for (const auto& node : regionNodes) {
        if (!node) continue;
        auto it = state.find(node);
        if (it != state.end() && it->second == 2) {
            continue;
        }
        if (dfs(dfs, node)) {
            return true;
        }
    }
    return false;
}

static bool regionTouchesCycle(
    IncrementalDerivationGraphViewInterface& view,
    const std::set<NodePtr>& regionNodes) {
    if (regionNodes.empty()) {
        return false;
    }
    std::unordered_set<NodePtr> tmp(regionNodes.begin(), regionNodes.end());
    return regionTouchesCycle(view, tmp);
}

// Analysis cache to hand downstream without recomputing.
struct AnalyzerResultCache {
    incra::Region region;
    incra::Boundaries boundaries;
    std::unordered_map<NodePtr, std::vector<incra::IncRegionAnalysis::AnchorCandidate>> mergeableAnchorsByHead;
};

struct RegionalInsertPlan {
    std::set<NodePtr> regionNodes;
    std::set<NodePtr> boundaryNodes;
    std::unordered_map<NodePtr, std::vector<incra::IncRegionAnalysis::AnchorCandidate>> anchorCandidates;
    bool mergeReady = false;
    bool regionClosed = false;
};

struct RegionalInsertPlanBuilder {
    static RegionalInsertPlan build(const AnalyzerResultCache& analysis) {
        RegionalInsertPlan plan;
        plan.regionNodes.insert(analysis.region.nodes.begin(), analysis.region.nodes.end());
        std::set<NodePtr> b;
        b.insert(analysis.boundaries.out_induced.begin(), analysis.boundaries.out_induced.end());
        b.insert(analysis.boundaries.scope_induced.begin(), analysis.boundaries.scope_induced.end());
        b.insert(analysis.boundaries.residual.begin(), analysis.boundaries.residual.end());
        plan.boundaryNodes.swap(b);
        plan.regionClosed = plan.boundaryNodes.empty();
        plan.anchorCandidates = analysis.mergeableAnchorsByHead;
        plan.mergeReady = plan.regionClosed;
        if (!plan.regionClosed) {
            bool ok = true;
            for (auto& v : plan.boundaryNodes) {
                auto it = plan.anchorCandidates.find(v);
                if (it == plan.anchorCandidates.end() || it->second.empty()) {
                    ok = false;
                    break;
                }
            }
            plan.mergeReady = ok;
        }
        return plan;
    }
};

struct RegionPrioritizedEdge {
    EdgePtr edge;
    size_t priority;
    int sequence_id;
    bool operator<(const RegionPrioritizedEdge& other) const {
        if (priority != other.priority) return priority > other.priority;
        return sequence_id > other.sequence_id;
    }
};

template <typename FormulaManagerT, typename FormulaNodeRef>
class RegionalDDRebuilder {
public:
    struct Snapshot {
        std::map<NodePtr, FormulaNodeRef> boundarySnapshots;
    };
    struct DagPlan {
        std::vector<NodePtr> topoOrder;
        std::unordered_map<NodePtr, std::vector<EdgePtr>> headEdges;
    };
    struct Timing {
        double snapshotMs = 0.0;
        double preConfigMs = 0.0;
        double initNodesMs = 0.0;
        double initEdgesMs = 0.0;
        double depGraphMs = 0.0;
        double regionCyclesMs = 0.0;
        double indegreeMs = 0.0;
        double rebuildLoopMs = 0.0;
        double totalMs = 0.0;
        double reorderMs = 0.0;
        size_t regionCycleCount = 0;
        size_t edgesProcessed = 0;
        size_t edgeRequeued = 0;
        size_t edgesRebuilt = 0;
        size_t nodesUpdated = 0;
        size_t makeAndCalls = 0;
        double makeAndMs = 0.0;
        size_t makeOrCalls = 0;
        double makeOrMs = 0.0;
        size_t inputLiteralCalls = 0;
        size_t inputLiteralMissing = 0;
        double inputLiteralMs = 0.0;
    };
    struct Result {
        Snapshot snapshot;
        std::set<NodePtr> changedNodes;
        std::unordered_set<EdgePtr> rebuiltEdges;
        Timing timing;
        bool fallbackToCycle = false;
        size_t missingInsideRegion = 0;
    };

    static bool buildDagPlan(
        IncrementalDerivationGraphViewInterface& view,
        const RegionalInsertPlan& plan,
        DagPlan& dag,
        double* buildMs = nullptr) {
        auto nowMs = []{ return std::chrono::steady_clock::now(); };
        auto toMs = [](auto dur){
            return std::chrono::duration<double, std::milli>(dur).count();
        };
        auto t0 = nowMs();
        dag.topoOrder.clear();
        dag.headEdges.clear();

        std::unordered_map<NodePtr, size_t> indeg;
        indeg.reserve(plan.regionNodes.size());
        std::unordered_map<NodePtr, std::unordered_set<NodePtr>> succs;
        succs.reserve(plan.regionNodes.size());

        for (const auto& n : plan.regionNodes) {
            indeg[n] = 0;
        }

        const auto& deltaInsertedEdges = view.getDeltaInsertEdges();
        auto shouldRebuildEdge = [&](const EdgePtr& e) {
            if (!e) return false;
            auto head = view.getOutput(e);
            if (!head || !plan.regionNodes.count(head)) return false;
            if (deltaInsertedEdges.count(e)) return true;
            for (auto& in : view.getInputs(e)) {
                if (plan.regionNodes.count(in)) return true;
            }
            return false;
        };

        for (const auto& head : plan.regionNodes) {
            for (const auto& e : view.getIncomingEdges(head)) {
                if (!shouldRebuildEdge(e)) continue;
                dag.headEdges[head].push_back(e);
                for (const auto& in : view.getInputs(e)) {
                    if (!plan.regionNodes.count(in)) continue;
                    auto& succSet = succs[in];
                    if (succSet.insert(head).second) {
                        indeg[head] += 1;
                    }
                }
            }
        }

        std::queue<NodePtr> ready;
        for (const auto& kv : indeg) {
            if (kv.second == 0) ready.push(kv.first);
        }
        while (!ready.empty()) {
            NodePtr cur = ready.front();
            ready.pop();
            dag.topoOrder.push_back(cur);
            auto sit = succs.find(cur);
            if (sit == succs.end()) continue;
            for (const auto& succ : sit->second) {
                auto it = indeg.find(succ);
                if (it == indeg.end()) continue;
                if (--it->second == 0) {
                    ready.push(succ);
                }
            }
        }

        if (buildMs) {
            *buildMs = toMs(nowMs() - t0);
        }
        return dag.topoOrder.size() == plan.regionNodes.size();
    }

    static Result rebuildInsertRegion(
        IncrementalDerivationGraphViewInterface& view,
        FormulaManagerT& formulaManager,
        std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
        std::map<EdgePtr, FormulaNodeRef>& edgeFormulas,
        const RegionalInsertPlan& plan,
        const std::unordered_set<EdgePtr>& regionEdges,
        const CycleDependencyGraph* depGraphOverride = nullptr) {
        Result res;
        auto nowMs = []{ return std::chrono::steady_clock::now(); };
        auto toMs = [](auto dur){
            return std::chrono::duration<double, std::milli>(dur).count();
        };
        auto t0 = nowMs();
        const bool profile = incRegionalProfileEnabled;
        double reorderStartSec = getReorderSeconds(formulaManager, 0);
        auto makeAndProfile = [&](const std::vector<FormulaNodeRef>& inputs) {
            if (!profile) {
                return formulaManager.makeAnd(inputs);
            }
            auto t = nowMs();
            auto formula = formulaManager.makeAnd(inputs);
            res.timing.makeAndCalls += 1;
            res.timing.makeAndMs += toMs(nowMs() - t);
            return formula;
        };
        auto makeOrProfile = [&](const std::vector<FormulaNodeRef>& inputs) {
            if (!profile) {
                return formulaManager.makeOr(inputs);
            }
            auto t = nowMs();
            auto formula = formulaManager.makeOr(inputs);
            res.timing.makeOrCalls += 1;
            res.timing.makeOrMs += toMs(nowMs() - t);
            return formula;
        };
        auto inputLiteralProfile = [&](const NodePtr& node, bool neg, FormulaNodeRef& lit) {
            if (!profile) {
                return regionalInputFormulaLiteral(formulaManager, nodeFormulas, node, neg, lit);
            }
            auto t = nowMs();
            bool ok = regionalInputFormulaLiteral(formulaManager, nodeFormulas, node, neg, lit);
            res.timing.inputLiteralCalls += 1;
            res.timing.inputLiteralMs += toMs(nowMs() - t);
            if (!ok) {
                res.timing.inputLiteralMissing += 1;
            }
            return ok;
        };

        // Snapshot old boundary formulas
        auto tSnapshotStart = nowMs();
        for (auto& v : plan.boundaryNodes) {
            auto it = nodeFormulas.find(v);
            if (it != nodeFormulas.end()) {
                res.snapshot.boundarySnapshots[v] = it->second;
            } else {
                res.snapshot.boundarySnapshots[v] = formulaManager.getFalse();
            }
        }
        auto tSnapshotEnd = nowMs();

        auto tPreConfigStart = nowMs();
        setCuddPreConfigTag("inc_insert");
        formulaManager.preConfig(view);
        setCuddPreConfigTag("");
        auto tPreConfigEnd = nowMs();

        const auto& deltaInsertedNodes = view.getDeltaInsertNodes();
        const auto& deltaInsertedEdges = view.getDeltaInsertEdges();

        // Initialize inserted nodes
        auto tInitNodesStart = nowMs();
        for (auto node : deltaInsertedNodes) {
            if (node->isFact) {
                int idx = formulaManager.getVarIndex(*node);
                if (node->getProbability() == 1.0) {
                    nodeFormulas[node] = formulaManager.getTrue();
                } else {
                    nodeFormulas[node] = formulaManager.createVar(idx, *node);
                    formulaManager.setVariableWeight(idx, node->getProbability(), 1 - node->getProbability());
                }
            } else {
                nodeFormulas[node] = formulaManager.getFalse();
            }
            res.changedNodes.insert(node);
        }
        auto tInitNodesEnd = nowMs();
        // Initialize inserted edges
        auto tInitEdgesStart = nowMs();
        for (auto edge : deltaInsertedEdges) {
            edgeFormulas[edge] = formulaManager.getFalse();
            if (!edge->isDeterministic()) {
                int idx = formulaManager.getVarIndex(*edge);
                formulaManager.createVar(idx, *edge);
                formulaManager.setVariableWeight(idx, edge->getProbability(), 1 - edge->getProbability());
            }
        }
        auto tInitEdgesEnd = nowMs();

        auto tDepGraphStart = nowMs();
        const auto& depGraph = depGraphOverride ? *depGraphOverride : view.getCycleDependencyGraph();
        auto tDepGraphEnd = nowMs();
        std::unordered_set<size_t> regionCycles;
        auto tRegionCyclesStart = nowMs();
        for (auto& n : plan.regionNodes) {
            auto it = depGraph.nodeToCycleIndex.find(n);
            if (it != depGraph.nodeToCycleIndex.end()) {
                regionCycles.insert(it->second);
            }
        }
        auto tRegionCyclesEnd = nowMs();

        // Build filtered indegree
        auto tIndegreeStart = nowMs();
        std::unordered_map<size_t, size_t> indeg;
        for (auto cid : regionCycles) indeg[cid] = 0;
        for (auto cid : regionCycles) {
            for (auto succ : depGraph.reverseDependencies[cid]) {
                if (regionCycles.count(succ)) {
                    indeg[succ]++;
                }
            }
        }

        std::queue<size_t> ready;
        for (auto& kv : indeg) if (kv.second == 0) ready.push(kv.first);
        auto tIndegreeEnd = nowMs();

        auto tRebuildStart = nowMs();
        size_t nodesUpdated = 0;
        std::map<size_t, std::priority_queue<RegionPrioritizedEdge>> cycleWorklists;
        std::map<size_t, std::set<EdgePtr>> cycleInWorklists;

        int seedSeq = 0;
        for (auto edge : deltaInsertedEdges) {
            if (!edge || !regionEdges.count(edge)) continue;
            auto it = depGraph.edgeToCycleIndex.find(edge);
            if (it == depGraph.edgeToCycleIndex.end()) continue;
            size_t cid = it->second;
            if (!regionCycles.count(cid)) continue;
            cycleWorklists[cid].push({edge, depGraph.edgeDepthsGlobal.at(edge), seedSeq++});
            cycleInWorklists[cid].insert(edge);
        }

        while (!ready.empty()) {
            auto cid = ready.front(); ready.pop();
            auto& worklist = cycleWorklists[cid];
            auto& inWorklist = cycleInWorklists[cid];
            int seq = 1;
            while (!worklist.empty()) {
                EdgePtr edge = worklist.top().edge;
                worklist.pop();
                res.timing.edgesProcessed += 1;
                inWorklist.erase(edge);
                const auto& ins = view.getInputs(edge);
                const auto& negs = view.getBodyNegations(edge);
                FormulaNodeRef newEdge;
                bool allAvail = true;
                {
                    FormulaNodeRef base = edge->isDeterministic()
                        ? formulaManager.getTrue()
                        : formulaManager.createVar(formulaManager.getVarIndex(*edge), *edge);
                    std::vector<FormulaNodeRef> inputs{base};
                    for (size_t i = 0; i < ins.size(); ++i) {
                        FormulaNodeRef lit;
                        if (!inputLiteralProfile(ins[i], negs[i], lit)) {
                            allAvail = false;
                            break;
                        }
                        inputs.push_back(lit);
                    }
                    if (!allAvail) {
                        if (!inWorklist.count(edge)) {
                            worklist.push({edge, depGraph.edgeDepthsGlobal.at(edge), seq++});
                            inWorklist.insert(edge);
                            res.timing.edgeRequeued += 1;
                        }
                        continue;
                    }
                    newEdge = (inputs.size()==1) ? inputs[0] : makeAndProfile(inputs);
                }
                if (!edgeFormulas.count(edge) || !formulaManager.isSame(edgeFormulas[edge], newEdge)) {
                    edgeFormulas[edge] = newEdge;
                    res.rebuiltEdges.insert(edge);
                    NodePtr out = view.getOutput(edge);
                    if (!out || out->isFact) continue;
                    FormulaNodeRef newNode;
                    bool hasNewNode = false;
                    if (!hasNewNode) {
                        std::vector<FormulaNodeRef> incoming;
                        for (auto eIn : view.getIncomingEdges(out)) {
                            auto it = edgeFormulas.find(eIn);
                            if (it != edgeFormulas.end() && it->second.get()) incoming.push_back(it->second);
                        }
                        if (incoming.empty()) continue;
                        newNode = (incoming.size()==1) ? incoming[0] : makeOrProfile(incoming);
                        hasNewNode = true;
                    }
                    if (hasNewNode && (!nodeFormulas.count(out) || !formulaManager.isSame(nodeFormulas[out], newNode))) {
                        nodeFormulas[out] = newNode;
                        res.changedNodes.insert(out);
                        nodesUpdated += 1;
                        for (auto outEdge : view.getOutgoingEdges(out)) {
                            if (!regionEdges.count(outEdge)) continue;
                            auto eit = depGraph.edgeToCycleIndex.find(outEdge);
                            if (eit == depGraph.edgeToCycleIndex.end()) continue;
                            auto outCid = eit->second;
                            if (!regionCycles.count(outCid)) continue;
                            if (!cycleInWorklists[outCid].count(outEdge)) {
                                cycleWorklists[outCid].push({outEdge, depGraph.edgeDepthsGlobal.at(outEdge), seq++});
                                cycleInWorklists[outCid].insert(outEdge);
                            }
                        }
                    }
                }
            }
            for (auto succ : depGraph.reverseDependencies[cid]) {
                if (regionCycles.count(succ)) {
                    if (--indeg[succ]==0) ready.push(succ);
                }
            }
        }
        auto tRebuildEnd = nowMs();

        double reorderEndSec = getReorderSeconds(formulaManager, 0);
        auto tEnd = nowMs();
        res.timing.snapshotMs = toMs(tSnapshotEnd - tSnapshotStart);
        res.timing.preConfigMs = toMs(tPreConfigEnd - tPreConfigStart);
        res.timing.initNodesMs = toMs(tInitNodesEnd - tInitNodesStart);
        res.timing.initEdgesMs = toMs(tInitEdgesEnd - tInitEdgesStart);
        res.timing.depGraphMs = toMs(tDepGraphEnd - tDepGraphStart);
        res.timing.regionCyclesMs = toMs(tRegionCyclesEnd - tRegionCyclesStart);
        res.timing.indegreeMs = toMs(tIndegreeEnd - tIndegreeStart);
        res.timing.rebuildLoopMs = toMs(tRebuildEnd - tRebuildStart);
        res.timing.totalMs = toMs(tEnd - t0);
        res.timing.reorderMs = (reorderEndSec > reorderStartSec)
            ? (reorderEndSec - reorderStartSec) * 1000.0
            : 0.0;
        res.timing.regionCycleCount = regionCycles.size();
        res.timing.nodesUpdated = nodesUpdated;
        res.timing.edgesRebuilt = res.rebuiltEdges.size();

        return res;
    }

    static Result rebuildInsertRegionDag(
        IncrementalDerivationGraphViewInterface& view,
        FormulaManagerT& formulaManager,
        std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
        std::map<EdgePtr, FormulaNodeRef>& edgeFormulas,
        const RegionalInsertPlan& plan,
        const std::unordered_set<EdgePtr>& regionEdges,
        const DagPlan& dag,
        double dagBuildMs) {
        Result res;
        auto nowMs = []{ return std::chrono::steady_clock::now(); };
        auto toMs = [](auto dur){
            return std::chrono::duration<double, std::milli>(dur).count();
        };
        auto t0 = nowMs();
        const bool profile = incRegionalProfileEnabled;
        double reorderStartSec = getReorderSeconds(formulaManager, 0);
        auto makeAndProfile = [&](const std::vector<FormulaNodeRef>& inputs) {
            if (!profile) {
                return formulaManager.makeAnd(inputs);
            }
            auto t = nowMs();
            auto formula = formulaManager.makeAnd(inputs);
            res.timing.makeAndCalls += 1;
            res.timing.makeAndMs += toMs(nowMs() - t);
            return formula;
        };
        auto makeOrProfile = [&](const std::vector<FormulaNodeRef>& inputs) {
            if (!profile) {
                return formulaManager.makeOr(inputs);
            }
            auto t = nowMs();
            auto formula = formulaManager.makeOr(inputs);
            res.timing.makeOrCalls += 1;
            res.timing.makeOrMs += toMs(nowMs() - t);
            return formula;
        };
        auto inputLiteralProfile = [&](const NodePtr& node, bool neg, FormulaNodeRef& lit) {
            if (!profile) {
                return regionalInputFormulaLiteral(formulaManager, nodeFormulas, node, neg, lit);
            }
            auto t = nowMs();
            bool ok = regionalInputFormulaLiteral(formulaManager, nodeFormulas, node, neg, lit);
            res.timing.inputLiteralCalls += 1;
            res.timing.inputLiteralMs += toMs(nowMs() - t);
            if (!ok) {
                res.timing.inputLiteralMissing += 1;
            }
            return ok;
        };

        // Snapshot old boundary formulas
        auto tSnapshotStart = nowMs();
        for (auto& v : plan.boundaryNodes) {
            auto it = nodeFormulas.find(v);
            if (it != nodeFormulas.end()) {
                res.snapshot.boundarySnapshots[v] = it->second;
            } else {
                res.snapshot.boundarySnapshots[v] = formulaManager.getFalse();
            }
        }
        auto tSnapshotEnd = nowMs();

        auto tPreConfigStart = nowMs();
        setCuddPreConfigTag("inc_insert");
        formulaManager.preConfig(view);
        setCuddPreConfigTag("");
        auto tPreConfigEnd = nowMs();

        const auto& deltaInsertedNodes = view.getDeltaInsertNodes();
        const auto& deltaInsertedEdges = view.getDeltaInsertEdges();

        // Initialize inserted nodes
        auto tInitNodesStart = nowMs();
        for (auto node : deltaInsertedNodes) {
            if (node->isFact) {
                int idx = formulaManager.getVarIndex(*node);
                if (node->getProbability() == 1.0) {
                    nodeFormulas[node] = formulaManager.getTrue();
                } else {
                    nodeFormulas[node] = formulaManager.createVar(idx, *node);
                    formulaManager.setVariableWeight(idx, node->getProbability(), 1 - node->getProbability());
                }
            } else {
                nodeFormulas[node] = formulaManager.getFalse();
            }
            res.changedNodes.insert(node);
        }
        auto tInitNodesEnd = nowMs();

        // Initialize inserted edges
        auto tInitEdgesStart = nowMs();
        for (auto edge : deltaInsertedEdges) {
            edgeFormulas[edge] = formulaManager.getFalse();
            if (!edge->isDeterministic()) {
                int idx = formulaManager.getVarIndex(*edge);
                formulaManager.createVar(idx, *edge);
                formulaManager.setVariableWeight(idx, edge->getProbability(), 1 - edge->getProbability());
            }
        }
        auto tInitEdgesEnd = nowMs();

        auto tRebuildStart = nowMs();
        size_t nodesUpdated = 0;
        bool abortDag = false;
        for (const auto& head : dag.topoOrder) {
            if (abortDag) break;
            auto itEdges = dag.headEdges.find(head);
            if (itEdges == dag.headEdges.end()) {
                continue;
            }
            for (const auto& edge : itEdges->second) {
                if (!regionEdges.count(edge)) {
                    continue;
                }
                res.timing.edgesProcessed += 1;
                const auto& ins = view.getInputs(edge);
                const auto& negs = view.getBodyNegations(edge);
                FormulaNodeRef newEdge;
                bool allAvail = true;
                bool missingInsideRegion = false;
                {
                    FormulaNodeRef base = edge->isDeterministic()
                        ? formulaManager.getTrue()
                        : formulaManager.createVar(formulaManager.getVarIndex(*edge), *edge);
                    std::vector<FormulaNodeRef> inputs{base};
                    for (size_t i = 0; i < ins.size(); ++i) {
                        FormulaNodeRef lit;
                        if (!inputLiteralProfile(ins[i], negs[i], lit)) {
                            allAvail = false;
                            if (plan.regionNodes.count(ins[i])) {
                                missingInsideRegion = true;
                            }
                            break;
                        }
                        inputs.push_back(lit);
                    }
                    if (!allAvail) {
                        if (missingInsideRegion) {
                            res.missingInsideRegion += 1;
                            abortDag = true;
                            if (incRegionalProfileEnabled) {
                                std::cout << "[inc-regional-dag] missing input for head="
                                          << incra::node_id(view.getOutput(edge))
                                          << " input=" << incra::node_id(ins.empty() ? nullptr : ins[0])
                                          << " reason=missing_inside_region\n";
                            }
                            break;
                        }
                        continue;
                    }
                    newEdge = (inputs.size()==1) ? inputs[0] : makeAndProfile(inputs);
                }
                if (!edgeFormulas.count(edge) || !formulaManager.isSame(edgeFormulas[edge], newEdge)) {
                    edgeFormulas[edge] = newEdge;
                    res.rebuiltEdges.insert(edge);
                }
            }

            if (!head || head->isFact) {
                continue;
            }
            if (abortDag) break;
            FormulaNodeRef newNode;
            bool hasNewNode = false;
            if (!hasNewNode) {
                std::vector<FormulaNodeRef> incoming;
                for (auto eIn : view.getIncomingEdges(head)) {
                    auto itEF = edgeFormulas.find(eIn);
                    if (itEF != edgeFormulas.end() && itEF->second.get()) incoming.push_back(itEF->second);
                }
                if (incoming.empty()) {
                    continue;
                }
                        newNode = (incoming.size()==1) ? incoming[0] : makeOrProfile(incoming);
                        hasNewNode = true;
                    }
            if (hasNewNode && (!nodeFormulas.count(head) || !formulaManager.isSame(nodeFormulas[head], newNode))) {
                nodeFormulas[head] = newNode;
                res.changedNodes.insert(head);
                nodesUpdated += 1;
            }
        }
        auto tRebuildEnd = nowMs();

        double reorderEndSec = getReorderSeconds(formulaManager, 0);
        auto tEnd = nowMs();
        res.timing.snapshotMs = toMs(tSnapshotEnd - tSnapshotStart);
        res.timing.preConfigMs = toMs(tPreConfigEnd - tPreConfigStart);
        res.timing.initNodesMs = toMs(tInitNodesEnd - tInitNodesStart);
        res.timing.initEdgesMs = toMs(tInitEdgesEnd - tInitEdgesStart);
        res.timing.depGraphMs = 0.0;
        res.timing.regionCyclesMs = 0.0;
        res.timing.indegreeMs = dagBuildMs;
        res.timing.rebuildLoopMs = toMs(tRebuildEnd - tRebuildStart);
        res.timing.totalMs = toMs(tEnd - t0);
        res.timing.reorderMs = (reorderEndSec > reorderStartSec)
            ? (reorderEndSec - reorderStartSec) * 1000.0
            : 0.0;
        res.timing.regionCycleCount = dag.topoOrder.size();
        res.timing.nodesUpdated = nodesUpdated;
        res.timing.edgesRebuilt = res.rebuiltEdges.size();
        if (abortDag) {
            res.fallbackToCycle = true;
        }

        return res;
    }

private:
    template <typename T>
    static auto getReorderSeconds(T& fm, int) -> decltype(fm.getReorderingTimeSeconds(), double()) {
        return fm.getReorderingTimeSeconds();
    }
    template <typename T>
    static double getReorderSeconds(T&, ...) {
        return 0.0;
    }
};

template <typename FormulaManagerT, typename FormulaNodeRef>
class BoundaryGateCalibrator {
public:
    struct CalibrationRecord {
        NodePtr v;
        incra::IncRegionAnalysis::AnchorCandidate anchor;
        int varIdx = -1;
        double alpha = 0.0;
        double beta  = 0.0;
        double pStar = 0.0;
        double target = 0.0;      // Pr_new[v]
        double oldValue = 0.0;    // WMC(BDD_old(v)) under old weights
    };
    struct CalibrationCache {
        struct TargetEntry {
            FormulaNodeRef formula{};
            double value = 0.0;
        };
        struct SnapshotEntry {
            FormulaNodeRef formula{};
            double value = 0.0;
        };
        struct AnchorEntry {
            FormulaNodeRef snapshot{};
            double alpha = 0.0;
            double beta = 0.0;
        };
        std::unordered_map<NodePtr, TargetEntry> targetWmc;
        std::unordered_map<NodePtr, SnapshotEntry> snapshotWmc;
        std::unordered_map<NodePtr, std::unordered_map<int, AnchorEntry>> anchorWmc;
    };
    struct Timing {
        double totalMs = 0.0;
        double targetWmcMs = 0.0;
        double anchorLoopMs = 0.0;
        double snapshotWmcMs = 0.0;
        double alphaWmcMs = 0.0;
        double betaWmcMs = 0.0;
        double weightSetMs = 0.0;
        double anchorWmcMs = 0.0;
        double anchorOverheadMs = 0.0;
        double overheadMs = 0.0;
        size_t boundaryTotal = 0;
        size_t boundaryVisited = 0;
        size_t boundarySkipped = 0;
        size_t anchorCandidates = 0;
        size_t anchorTried = 0;
        size_t anchorUsed = 0;
        size_t anchorInvalid = 0;
        size_t missingSnapshot = 0;
        size_t degenerate = 0;
        size_t calibrated = 0;
        size_t failed = 0;
        size_t targetCacheHits = 0;
        size_t snapshotCacheHits = 0;
        size_t anchorCacheHits = 0;
    };
    struct Result {
        std::vector<CalibrationRecord> applied;
        std::unordered_set<NodePtr> failedNodes;
        std::unordered_map<int, std::pair<double,double>> weightOverrides;
        Timing timing;
    };

    static Result calibrate(
        IncrementalDerivationGraphViewInterface& view,
        FormulaManagerT& formulaManager,
        const RegionalInsertPlan& plan,
        const typename RegionalDDRebuilder<FormulaManagerT, FormulaNodeRef>::Snapshot& snapshot,
        const std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
        double eps = 1e-12,
        const std::unordered_set<NodePtr>* skipNodes = nullptr,
        CalibrationCache* cache = nullptr) {
        Result res;
        auto nowMs = []{ return std::chrono::steady_clock::now(); };
        auto toMs = [](auto dur){
            return std::chrono::duration<double, std::milli>(dur).count();
        };
        auto tStart = nowMs();
        std::unordered_map<int, NodePtr> usedAnchorVars;
        usedAnchorVars.reserve(plan.boundaryNodes.size());
        res.timing.boundaryTotal = plan.boundaryNodes.size();
        for (auto& v : plan.boundaryNodes) {
            res.timing.boundaryVisited++;
            if (skipNodes && skipNodes->count(v)) {
                res.timing.boundarySkipped++;
                continue;
            }
            auto itNF = nodeFormulas.find(v);
            if (itNF == nodeFormulas.end()) {
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] calibrate fail head=" << incra::node_id(v)
                              << " reason=missing_node_formula\n";
                }
                res.failedNodes.insert(v);
                continue;
            }
            double target = 0.0;
            bool targetCached = false;
            if (cache) {
                auto itCache = cache->targetWmc.find(v);
                if (itCache != cache->targetWmc.end() &&
                        formulaManager.isSame(itCache->second.formula, itNF->second)) {
                    target = itCache->second.value;
                    targetCached = true;
                    res.timing.targetCacheHits++;
                }
            }
            if (!targetCached) {
                auto tTargetStart = nowMs();
                target = formulaManager.computeWeightedModelCount(itNF->second);
                res.timing.targetWmcMs += toMs(nowMs() - tTargetStart);
                if (cache) {
                    cache->targetWmc[v] = {itNF->second, target};
                }
            }
            auto anchorIt = plan.anchorCandidates.find(v);
            if (anchorIt == plan.anchorCandidates.end() || anchorIt->second.empty()) {
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] calibrate fail head=" << incra::node_id(v)
                              << " reason=no_anchor_candidates\n";
                }
                res.failedNodes.insert(v);
                continue;
            }
            res.timing.anchorCandidates += anchorIt->second.size();
            bool calibrated = false;
            size_t tried = 0;
            size_t missingSnapshot = 0;
            size_t degenerate = 0;
            size_t candidateIdx = 0;
            auto tAnchorStart = nowMs();
            for (auto& anchor : anchorIt->second) {
                const size_t idx = candidateIdx++;
                const auto anchorLabel = incra::anchor_id(anchor, view);
                auto logDecision = [&](const char* decision, const char* reason) {
                    if (!incRegionalProfileEnabled) return;
                    std::cout << "[inc-regional-calibrate-anchor] head=" << incra::node_id(v)
                              << " idx=" << idx
                              << " anchor=" << anchorLabel
                              << " decision=" << decision
                              << " reason=" << reason
                              << "\n";
                };
                int varIdx = -1;
                if (anchor.kind == incra::IncRegionAnalysis::AnchorKind::Node) {
                    if (!anchor.node) {
                        if (incRegionalProfileEnabled) {
                            std::cout << "[inc-regional] calibrate skip head=" << incra::node_id(v)
                                      << " anchor=node:<null> reason=node_null\n";
                        }
                        res.timing.anchorInvalid++;
                        logDecision("reject", "node_null");
                        continue;
                    }
                    if (!anchor.node->isFact) {
                        if (incRegionalProfileEnabled) {
                            std::cout << "[inc-regional] calibrate skip head=" << incra::node_id(v)
                                      << " anchor=node:" << incra::node_id(anchor.node)
                                      << " reason=node_not_fact\n";
                        }
                        res.timing.anchorInvalid++;
                        logDecision("reject", "node_not_fact");
                        continue;
                    }
                    if (anchor.node->getProbability() >= 1.0) {
                        if (incRegionalProfileEnabled) {
                            std::cout << "[inc-regional] calibrate skip head=" << incra::node_id(v)
                                      << " anchor=node:" << incra::node_id(anchor.node)
                                      << " reason=node_prob_one\n";
                        }
                        res.timing.anchorInvalid++;
                        logDecision("reject", "node_prob_one");
                        continue;
                    }
                    varIdx = formulaManager.getVarIndex(*anchor.node);
                } else {
                    if (!anchor.edge) {
                        if (incRegionalProfileEnabled) {
                            std::cout << "[inc-regional] calibrate skip head=" << incra::node_id(v)
                                      << " anchor=edge:<null> reason=edge_null\n";
                        }
                        res.timing.anchorInvalid++;
                        logDecision("reject", "edge_null");
                        continue;
                    }
                    varIdx = formulaManager.getVarIndex(*anchor.edge);
                }
                auto usedIt = usedAnchorVars.find(varIdx);
                if (usedIt != usedAnchorVars.end()) {
                    if (incRegionalProfileEnabled) {
                        std::cout << "[inc-regional] calibrate skip head=" << incra::node_id(v)
                                  << " anchor=" << incra::anchor_id(anchor, view)
                                  << " reason=anchor_var_used"
                                  << " varIdx=" << varIdx
                                  << " prev_head=" << incra::node_id(usedIt->second)
                                  << "\n";
                    }
                    res.timing.anchorUsed++;
                    logDecision("reject", "anchor_var_used");
                    continue;
                }
                tried++;
                res.timing.anchorTried++;
                auto oldW = formulaManager.getVariableWeight(varIdx);
                auto snapIt = snapshot.boundarySnapshots.find(v);
                if (snapIt == snapshot.boundarySnapshots.end()) {
                    missingSnapshot++;
                    res.timing.missingSnapshot++;
                    if (incRegionalProfileEnabled) {
                        std::cout << "[inc-regional] calibrate skip head=" << incra::node_id(v)
                                  << " anchor=" << incra::anchor_id(anchor, view)
                                  << " reason=missing_snapshot\n";
                    }
                    logDecision("reject", "missing_snapshot");
                    continue;
                }
                double oldVal = 0.0;
                bool snapshotCached = false;
                if (cache) {
                    auto itSnap = cache->snapshotWmc.find(v);
                    if (itSnap != cache->snapshotWmc.end() &&
                            formulaManager.isSame(itSnap->second.formula, snapIt->second)) {
                        oldVal = itSnap->second.value;
                        snapshotCached = true;
                        res.timing.snapshotCacheHits++;
                    }
                }
                if (!snapshotCached) {
                    auto tOldStart = nowMs();
                    oldVal = formulaManager.computeWeightedModelCount(snapIt->second);
                    res.timing.snapshotWmcMs += toMs(nowMs() - tOldStart);
                    if (cache) {
                        cache->snapshotWmc[v] = {snapIt->second, oldVal};
                    }
                }
                double alpha = 0.0;
                double beta = 0.0;
                bool anchorCached = false;
                if (cache) {
                    auto itAnchor = cache->anchorWmc.find(v);
                    if (itAnchor != cache->anchorWmc.end()) {
                        auto itVar = itAnchor->second.find(varIdx);
                        if (itVar != itAnchor->second.end() &&
                                formulaManager.isSame(itVar->second.snapshot, snapIt->second)) {
                            alpha = itVar->second.alpha;
                            beta = itVar->second.beta;
                            anchorCached = true;
                            res.timing.anchorCacheHits++;
                        }
                    }
                }
                if (!anchorCached) {
                    auto tWeightStart = nowMs();
                    formulaManager.setVariableWeight(varIdx, 0.0, 1.0);
                    res.timing.weightSetMs += toMs(nowMs() - tWeightStart);
                    auto tAlphaStart = nowMs();
                    alpha = formulaManager.computeWeightedModelCount(snapIt->second);
                    res.timing.alphaWmcMs += toMs(nowMs() - tAlphaStart);
                    tWeightStart = nowMs();
                    formulaManager.setVariableWeight(varIdx, 1.0, 0.0);
                    res.timing.weightSetMs += toMs(nowMs() - tWeightStart);
                    auto tBetaStart = nowMs();
                    beta  = formulaManager.computeWeightedModelCount(snapIt->second);
                    res.timing.betaWmcMs += toMs(nowMs() - tBetaStart);
                    tWeightStart = nowMs();
                    formulaManager.setVariableWeight(varIdx, oldW.posWeight, oldW.negWeight);
                    res.timing.weightSetMs += toMs(nowMs() - tWeightStart);
                    if (cache) {
                        cache->anchorWmc[v][varIdx] = {snapIt->second, alpha, beta};
                    }
                }
                if (std::fabs(beta - alpha) < eps) {
                    degenerate++;
                    res.timing.degenerate++;
                    logDecision("reject", "degenerate");
                    continue;
                }
                double pStar = (target - alpha) / (beta - alpha);
                if (!std::isfinite(pStar)) {
                    if (incRegionalProfileEnabled) {
                        std::cout << "[inc-regional] calibrate skip head=" << incra::node_id(v)
                                  << " anchor=" << incra::anchor_id(anchor, view)
                                  << " reason=pstar_non_finite"
                                  << " target=" << target
                                  << " alpha=" << alpha
                                  << " beta=" << beta
                                  << " pStar=" << pStar
                                  << "\n";
                    }
                    logDecision("reject", "pstar_non_finite");
                    continue;
                }
                if (pStar < -eps) {
                    if (incRegionalProfileEnabled) {
                        std::cout << "[inc-regional] calibrate skip head=" << incra::node_id(v)
                                  << " anchor=" << incra::anchor_id(anchor, view)
                                  << " reason=pstar_negative"
                                  << " target=" << target
                                  << " alpha=" << alpha
                                  << " beta=" << beta
                                  << " pStar=" << pStar
                                  << "\n";
                    }
                    logDecision("reject", "pstar_negative");
                    continue;
                }
                const bool pStarOutOfRange = (pStar > 1.0 + eps);
                if (pStarOutOfRange && incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] calibrate note head=" << incra::node_id(v)
                              << " anchor=" << incra::anchor_id(anchor, view)
                              << " reason=pstar_out_of_range"
                              << " target=" << target
                              << " alpha=" << alpha
                              << " beta=" << beta
                              << " pStar=" << pStar
                              << "\n";
                }
                CalibrationRecord rec;
                rec.v = v;
                rec.anchor = anchor;
                rec.varIdx = varIdx;
                rec.alpha = alpha;
                rec.beta = beta;
                rec.pStar = pStar;
                rec.target = target;
                rec.oldValue = oldVal;
                res.applied.push_back(rec);
                res.weightOverrides[varIdx] = {pStar, 1 - pStar};
                usedAnchorVars[varIdx] = v;
                calibrated = true;
                if (incRegionalProfileEnabled) {
                    std::ostringstream oss;
                    oss << std::setprecision(12)
                        << "[inc-regional-calibration]"
                        << " head=" << incra::node_id(v)
                        << " anchor=" << anchorLabel
                        << " var=" << varIdx
                        << " old_node_prob=" << oldVal
                        << " target_prob=" << target
                        << " p_when_false=" << alpha
                        << " p_when_true=" << beta
                        << " old_weight=" << oldW.posWeight
                        << " calibrated_weight=" << pStar
                        << " calibrated_neg_weight=" << (1.0 - pStar)
                        << "\n";
                    std::cout << oss.str();
                }
                logDecision("accept", pStarOutOfRange ? "pstar_out_of_range" : "ok");
                break;
            }
            res.timing.anchorLoopMs += toMs(nowMs() - tAnchorStart);
            if (!calibrated) {
                if (incRegionalProfileEnabled) {
                    std::string reason = "no_valid_anchor";
                    if (tried == 0) {
                        reason = "no_anchor_candidates";
                    } else if (missingSnapshot == tried) {
                        reason = "missing_snapshot";
                    } else if (degenerate == tried) {
                        reason = "degenerate_anchor";
                    }
                    std::cout << "[inc-regional] calibrate fail head=" << incra::node_id(v)
                              << " reason=" << reason
                              << " tried=" << tried
                              << " missing_snapshot=" << missingSnapshot
                              << " degenerate=" << degenerate
                              << "\n";
                }
                res.failedNodes.insert(v);
            }
            if (calibrated) {
                res.timing.calibrated++;
            } else {
                res.timing.failed++;
            }
        }
        res.timing.anchorWmcMs = res.timing.snapshotWmcMs + res.timing.alphaWmcMs + res.timing.betaWmcMs;
        res.timing.anchorOverheadMs = std::max(0.0, res.timing.anchorLoopMs -
                                                      (res.timing.anchorWmcMs + res.timing.weightSetMs));
        res.timing.totalMs = toMs(nowMs() - tStart);
        res.timing.overheadMs = std::max(0.0, res.timing.totalMs -
                                                 (res.timing.targetWmcMs + res.timing.anchorLoopMs));
        return res;
    }
};

template <typename FormulaManagerT, typename FormulaNodeRef>
class RegionalIncrementalForwardCompilation {
public:
    struct Options {
        bool enableFallbackToClassicInsertion = true;
        bool enableNearFullExpansion = false;
        bool enableOverlapExpansion = true;
        double eps = 1e-12;
    };
    struct Stats {
        size_t analyzeRegionNodes = 0;
        size_t analyzeRegionEdges = 0;
        size_t analyzeDrNodes = 0;
        size_t analyzeDrEdges = 0;
        size_t regionNodeCount = 0;
        size_t regionEdgeCount = 0;
        size_t drNodeCount = 0;
        size_t drEdgeCount = 0;
        size_t boundaryNodeCount = 0;
        size_t calibratedCount = 0;
        bool usedFallback = false;
        bool sccExpanded = false;
        bool outputSliceExpanded = false;
        size_t outputSliceMissingOutputs = 0;
        size_t outputSliceRegionNodes = 0;
        size_t outputSliceRegionEdges = 0;
        bool boundaryEmptyExpanded = false;
        size_t boundaryEmptyRegionNodes = 0;
        size_t boundaryEmptyRegionEdges = 0;
        bool nearFullExpanded = false;
        size_t nearFullRegionNodes = 0;
        size_t nearFullRegionEdges = 0;
        int planExpandAttempts = 0;
        size_t planExpandRegionNodes = 0;
        size_t planExpandRegionEdges = 0;
        size_t planExpandFailedBoundaries = 0;
    };
    struct Timing {
        double analyzeMs = 0.0;
        double sccCloseMs = 0.0;
        double planMs = 0.0;
        double rebuildMs = 0.0;
        double rebuildTotalMs = 0.0;
        double calibrateMs = 0.0;
        double calibrateTotalMs = 0.0;
        double fallbackSnapshotMs = 0.0;
        double fallbackRestoreMs = 0.0;
        double totalMs = 0.0;
        double fallbackMs = 0.0;
        double totalWithFallbackMs = 0.0;
        double overheadMs = 0.0;
        std::string fallbackReason;
        typename RegionalDDRebuilder<FormulaManagerT, FormulaNodeRef>::Timing rebuildDetail;
    };

    explicit RegionalIncrementalForwardCompilation(Options opt = {}) : opt_(opt) {}

    using CalibrationRecord = typename BoundaryGateCalibrator<FormulaManagerT, FormulaNodeRef>::CalibrationRecord;

    void applyUpdate(
        IncrementalDerivationGraphViewInterface& view,
        FormulaManagerT& formulaManager,
        std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
        std::map<EdgePtr, FormulaNodeRef>& edgeFormulas,
        std::set<NodePtr>& changedNodes,
        const CycleDependencyGraph* existingDepGraph = nullptr) {
        Debugger& debugger = Debugger::getInstance();
        const auto& deltaInsertedEdges = view.getDeltaInsertEdges();
        const auto& deltaInsertedNodes = view.getDeltaInsertNodes();
        const bool verboseOutput = DerivationGraphViewInterface::isVerboseEnabled();
        const bool emitRegionalDiagnostics = incRegionalProfileEnabled || verboseOutput;
        if (deltaInsertedEdges.empty() && deltaInsertedNodes.empty()) {
            formulaManager.dumpProfilingStatistics();
            for (auto& [key, value]: formulaManager.getProfilingStatistics()) {
                debugger.addInfo(key, value);
            }
            return;
        }
        lastOverlapMultiNodes_.clear();

        auto nowMs = []{ return std::chrono::steady_clock::now(); };
        auto toMs = [](auto dur){
            return std::chrono::duration<double, std::milli>(dur).count();
        };
        auto t0 = nowMs();

        // === 1) Analyze region ===
        incra::RegionAnalyzer analyzer(view);
        std::unordered_set<NodePtr> delta_input_set;
        delta_input_set.reserve(deltaInsertedNodes.size() + deltaInsertedEdges.size());
        for (auto n : deltaInsertedNodes) {
            if (n) {
                delta_input_set.insert(n);
            }
        }
        for (auto e : deltaInsertedEdges) {
            NodePtr out = view.getOutput(e);
            if (out) {
                delta_input_set.insert(out);
            }
        }
        std::vector<NodePtr> delta_inputs;
        delta_inputs.reserve(delta_input_set.size());
        for (auto n : delta_input_set) {
            delta_inputs.push_back(n);
        }
        // Regional pipeline expects an existing baseline; enforce it.
        assert(!(nodeFormulas.empty() && edgeFormulas.empty()) &&
               "Regional incremental requires pre-existing formulas; build baseline first.");
        auto analysisStats = analyzer.analyze(delta_inputs);
        auto analysis = analyzer.getLastAnalysis();
        stats_.analyzeRegionNodes = analysisStats.region_nodes;
        stats_.analyzeRegionEdges = analysisStats.region_edges;
        stats_.analyzeDrNodes = analysisStats.dr_nodes;
        stats_.analyzeDrEdges = analysisStats.dr_edges;
        auto t1 = nowMs();

        auto recordRegionChange = [&](const char* stage) {
            (void)stage;
        };
        recordRegionChange("initial");

        auto buildLocalDepGraph = [&](const incra::Region& reach) -> std::unique_ptr<LocalDepGraph> {
            std::unordered_set<NodePtr> nodes = reach.nodes;
            std::unordered_set<EdgePtr> edges;
            edges.reserve(reach.edges.size());
            size_t addedInputs = 0;
            size_t addedOutputs = 0;
            for (const auto& e : reach.edges) {
                if (!e) continue;
                NodePtr out = view.getOutput(e);
                if (out) {
                    if (nodes.insert(out).second) {
                        addedOutputs++;
                    }
                }
                for (const auto& in : view.getInputs(e)) {
                    if (in) {
                        if (nodes.insert(in).second) {
                            addedInputs++;
                        }
                    }
                }
                edges.insert(e);
            }
            if (incRegionalProfileEnabled) {
                std::cout << "[inc-regional] dep_graph scope=delta-reach"
                          << " reach_nodes=" << reach.nodes.size()
                          << " reach_edges=" << reach.edges.size()
                          << " local_nodes=" << nodes.size()
                          << " local_edges=" << edges.size()
                          << " added_inputs=" << addedInputs
                          << " added_outputs=" << addedOutputs
                          << " full_nodes=" << view.getNodes().size()
                          << " full_edges=" << view.getEdges().size()
                          << "\n";
            }
            return std::make_unique<LocalDepGraph>(std::move(nodes), std::move(edges));
        };

        std::unique_ptr<LocalDepGraph> localDepGraph;
        const CycleDependencyGraph* depGraphPtr = nullptr;
        const char* depGraphScope = nullptr;
        auto depGraphForReachable = [&](const char* stage, const incra::Region& reach) -> const CycleDependencyGraph& {
            const bool cached = depGraphPtr != nullptr;
            if (depGraphPtr) {
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] dep_graph_use stage=" << stage
                              << " scope=" << (depGraphScope ? depGraphScope : "unknown")
                              << " cached=1"
                              << " reach_nodes=" << reach.nodes.size()
                              << " reach_edges=" << reach.edges.size()
                              << "\n";
                }
                return *depGraphPtr;
            }
            if (existingDepGraph) {
                depGraphPtr = existingDepGraph;
                depGraphScope = "full-shared";
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] dep_graph_use stage=" << stage
                              << " scope=full-shared"
                              << " cached=" << (cached ? 1 : 0)
                              << " reach_nodes=" << reach.nodes.size()
                              << " reach_edges=" << reach.edges.size()
                              << "\n";
                }
                return *depGraphPtr;
            }
            if (reach.edges.empty()) {
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] dep_graph scope=full reason=reach_edges_empty"
                              << " reach_nodes=" << reach.nodes.size()
                              << " reach_edges=" << reach.edges.size()
                              << "\n";
                }
                depGraphPtr = &view.getCycleDependencyGraph();
                depGraphScope = "full";
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] dep_graph_use stage=" << stage
                              << " scope=full"
                              << " cached=" << (cached ? 1 : 0)
                              << " reach_nodes=" << reach.nodes.size()
                              << " reach_edges=" << reach.edges.size()
                              << "\n";
                }
                return *depGraphPtr;
            }
            if (!localDepGraph) {
                localDepGraph = buildLocalDepGraph(reach);
            }
            depGraphPtr = &localDepGraph->depGraph;
            depGraphScope = "delta-reach";
            if (incRegionalProfileEnabled) {
                std::cout << "[inc-regional] dep_graph_use stage=" << stage
                          << " scope=delta-reach"
                          << " cached=" << (cached ? 1 : 0)
                          << " reach_nodes=" << reach.nodes.size()
                          << " reach_edges=" << reach.edges.size()
                          << "\n";
            }
            return *depGraphPtr;
        };

        // === 2) Ensure SCC-closed ===
        bool expanded = false;
        const bool analyzeTouchesCycle = regionTouchesCycle(view, analysis.region.nodes);
        if (incRegionalProfileEnabled) {
            std::cout << "[inc-regional] analyze region_nodes=" << analysisStats.region_nodes
                      << " region_edges=" << analysisStats.region_edges
                      << " dr_nodes=" << analysisStats.dr_nodes
                      << " dr_edges=" << analysisStats.dr_edges
                      << "\n";
            std::cout << "[inc-regional] regionTouchesCycle analyze=" << (analyzeTouchesCycle ? 1 : 0)
                      << "\n";
        }
        // Avoid full SCC dependency construction when the region is acyclic.
        if (analyzeTouchesCycle) {
            auto& depGraph = depGraphForReachable("scc_close_initial", analyzer.lastDeltaReachable());
            expanded = RegionSccClosure::closeToScc(analysis.region, analyzer.lastDeltaReachable(), depGraph);
            if (expanded) {
                analysis.boundaries = analyzer.recomputeBoundaries(analysis.region);
                analysis.mergeableAnchorsByHead = analyzer.recomputeAnchors(analysis.region, analysis.boundaries);
                recordRegionChange("scc_close_initial");
            }
        }
        stats_.sccExpanded = expanded;
        auto t2 = nowMs();

        // If any delta-reachable output node is outside the region, record it.
        // We deliberately avoid expanding: outputs may stay outside the region,
        // and will be handled via calibrated boundary weights.
        {
            const auto& dr = analyzer.lastDeltaReachable();
            size_t missingOutputs = 0;
            for (const auto& n : dr.nodes) {
                if (!n || !n->needOutput) continue;
                if (!analysis.region.nodes.count(n)) {
                    missingOutputs++;
                }
            }
            if (missingOutputs > 0) {
                stats_.outputSliceMissingOutputs = missingOutputs;
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] output nodes outside region (count="
                              << missingOutputs << "); skip output-slice expansion"
                              << " dr_nodes=" << dr.nodes.size()
                              << " dr_edges=" << dr.edges.size()
                              << "\n";
                }
            }
        }

        struct PlanProfile {
            double buildMs = 0.0;
            double snapshotMs = 0.0;
            double anchorFilterMs = 0.0;
            double expandMs = 0.0;
            double overlapClosureMs = 0.0;
            double inputClosureMs = 0.0;
            double recomputeBoundariesMs = 0.0;
            double recomputeAnchorsMs = 0.0;
            double sccExpandMs = 0.0;
            size_t anchorsChecked = 0;
            size_t anchorsFiltered = 0;
            size_t overlapClosureRuns = 0;
            size_t overlapClosureAddedNodes = 0;
            size_t overlapClosureDependent = 0;
            size_t overlapClosureIndependent = 0;
            size_t overlapClosureEmptyOwners = 0;
            size_t inputClosureAddedNodes = 0;
            size_t inputClosureAddedEdges = 0;
            size_t inputClosureRuns = 0;
            size_t boundaryCount = 0;
            size_t anchorCandidateTotal = 0;
            size_t anchorCandidateMax = 0;
            int expandAttempts = 0;
            bool anchorFilterSkipped = false;
        } planProfile;

        auto boundariesEmpty = [&](const incra::Boundaries& b) {
            return b.out_induced.empty() && b.scope_induced.empty() && b.residual.empty();
        };
        if (boundariesEmpty(analysis.boundaries)) {
            const auto& dr = analyzer.lastDeltaReachable();
            bool missing = false;
            for (const auto& n : dr.nodes) {
                if (!analysis.region.nodes.count(n)) {
                    missing = true;
                    break;
                }
            }
            if (missing) {
                stats_.boundaryEmptyExpanded = true;
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] boundary empty but region misses delta-reachable nodes; "
                              << "expanding region to delta-reachable\n";
                }
                analysis.region.nodes = dr.nodes;
                analysis.region.edges = dr.edges;
                stats_.boundaryEmptyRegionNodes = analysis.region.nodes.size();
                stats_.boundaryEmptyRegionEdges = analysis.region.edges.size();
                {
                    auto tB = nowMs();
                    analysis.boundaries = analyzer.recomputeBoundaries(analysis.region);
                    planProfile.recomputeBoundariesMs += toMs(nowMs() - tB);
                }
                {
                    auto tA = nowMs();
                    analysis.mergeableAnchorsByHead = analyzer.recomputeAnchors(analysis.region, analysis.boundaries);
                    planProfile.recomputeAnchorsMs += toMs(nowMs() - tA);
                }
                recordRegionChange("boundary_empty_expand");
            }
        }

        auto closeRegionInputs = [&](const char* reason) -> bool {
            const auto& dr = analyzer.lastDeltaReachable();
            auto tStart = nowMs();
            std::queue<NodePtr> q;
            std::unordered_set<NodePtr> seen;
            seen.reserve(analysis.region.nodes.size());
            for (const auto& n : analysis.region.nodes) {
                if (!n) continue;
                if (seen.insert(n).second) {
                    q.push(n);
                }
            }
            auto inDrNode = [&](const NodePtr& n) {
                return dr.nodes.empty() || dr.nodes.count(n);
            };
            auto inDrEdge = [&](const EdgePtr& e) {
                return dr.edges.empty() || dr.edges.count(e);
            };
            bool changed = false;
            size_t addedNodes = 0;
            size_t addedEdges = 0;
            while (!q.empty()) {
                NodePtr cur = q.front();
                q.pop();
                for (const auto& e : view.getIncomingEdges(cur)) {
                    if (!inDrEdge(e)) continue;
                    if (analysis.region.edges.insert(e).second) {
                        addedEdges++;
                        changed = true;
                    }
                    for (const auto& in : view.getInputs(e)) {
                        if (!inDrNode(in)) continue;
                        if (analysis.region.nodes.insert(in).second) {
                            addedNodes++;
                            changed = true;
                            q.push(in);
                        }
                    }
                }
            }
            planProfile.inputClosureRuns += 1;
            planProfile.inputClosureAddedNodes += addedNodes;
            planProfile.inputClosureAddedEdges += addedEdges;
            planProfile.inputClosureMs += toMs(nowMs() - tStart);
            if (incRegionalProfileEnabled && (changed || addedNodes || addedEdges)) {
                std::cout << "[inc-regional] close region inputs reason=" << reason
                          << " added_nodes=" << addedNodes
                          << " added_edges=" << addedEdges
                          << " region_nodes=" << analysis.region.nodes.size()
                          << " region_edges=" << analysis.region.edges.size()
                          << "\n";
            }
            return changed;
        };

        auto closeDependentBoundaryOverlaps = [&](const char* reason) -> bool {
            const auto& dr = analyzer.lastDeltaReachable();
            auto tStart = nowMs();
            planProfile.overlapClosureRuns += 1;
            auto inDrNode = [&](const NodePtr& n) {
                return dr.nodes.empty() || dr.nodes.count(n);
            };
            auto inDrEdge = [&](const EdgePtr& e) {
                return dr.edges.empty() || dr.edges.count(e);
            };

            std::vector<NodePtr> boundaries;
            std::unordered_set<NodePtr> boundarySet;
            boundarySet.reserve(analysis.boundaries.out_induced.size() +
                                analysis.boundaries.scope_induced.size() +
                                analysis.boundaries.residual.size());
            auto addBoundary = [&](const std::set<NodePtr>& src) {
                for (const auto& n : src) {
                    if (!n) continue;
                    if (!dr.nodes.empty() && !dr.nodes.count(n)) continue;
                    if (boundarySet.insert(n).second) {
                        boundaries.push_back(n);
                    }
                }
            };
            addBoundary(analysis.boundaries.out_induced);
            addBoundary(analysis.boundaries.scope_induced);
            addBoundary(analysis.boundaries.residual);
            if (boundaries.size() < 2) {
                planProfile.overlapClosureMs += toMs(nowMs() - tStart);
                return false;
            }


            struct NodeLabel {
                bool multi = false;
                NodePtr boundary; // representative boundary head for debug
            };
            std::unordered_map<NodePtr, NodeLabel> labels;
            std::queue<NodePtr> q;
            std::unordered_set<NodePtr> overlapNodes;
            std::unordered_set<NodePtr> multiNodes;

            auto updateMulti = [&](const NodePtr& node) -> bool {
                auto it = labels.find(node);
                if (it == labels.end()) {
                    labels.emplace(node, NodeLabel{true, {}});
                    q.push(node);
                    return true;
                }
                if (!it->second.multi) {
                    it->second.multi = true;
                    it->second.boundary.reset();
                    q.push(node);
                    return true;
                }
                return false;
            };
            auto updateWithBoundary = [&](const NodePtr& node, const NodePtr& boundary) -> bool {
                auto it = labels.find(node);
                if (it == labels.end()) {
                    labels.emplace(node, NodeLabel{false, boundary});
                    q.push(node);
                    return true;
                }
                auto& label = it->second;
                if (label.multi) {
                    return false;
                }
                if (label.boundary && label.boundary.get() != boundary.get()) {
                    planProfile.overlapClosureDependent += 1;
                    label.multi = true;
                    label.boundary.reset();
                    q.push(node);
                    return true;
                }
                return false;
            };

            for (const auto& b : boundaries) {
                if (inDrNode(b)) {
                    updateWithBoundary(b, b);
                }
                for (const auto& e : view.getIncomingEdges(b)) {
                    if (!inDrEdge(e)) continue;
                    for (const auto& in : view.getInputs(e)) {
                        if (!inDrNode(in)) continue;
                        updateWithBoundary(in, b);
                    }
                }
            }

            while (!q.empty()) {
                NodePtr cur = q.front();
                q.pop();
                auto it = labels.find(cur);
                if (it == labels.end()) continue;
                auto& label = it->second;
                if (label.multi) {
                    multiNodes.insert(cur);
                }
                for (const auto& e : view.getIncomingEdges(cur)) {
                    if (!inDrEdge(e)) continue;
                    for (const auto& in : view.getInputs(e)) {
                        if (!inDrNode(in)) continue;
                        if (label.multi) {
                            updateMulti(in);
                        } else {
                            updateWithBoundary(in, label.boundary);
                        }
                    }
                }
            }
            size_t addedNodes = 0;
            size_t addedEdges = 0;
            size_t scopeSeeds = 0;
            size_t scopeEmpty = 0;
            size_t multiNonEmpty = 0;
            size_t multiEmpty = 0;
            std::unordered_set<NodePtr> effectiveMultiNodes;
            auto addScopeForSource = [&](const NodePtr& source, bool& anyFlag) {
                const auto& scopeNodes = analyzer.getScopeNodesForSource(source);
                const auto& scopeEdges = analyzer.getScopeEdgesForSource(source);
                for (const auto& sn : scopeNodes) {
                    if (!inDrNode(sn)) continue;
                    anyFlag = true;
                    overlapNodes.insert(sn);
                    if (analysis.region.nodes.insert(sn).second) {
                        addedNodes++;
                    }
                }
                for (const auto& se : scopeEdges) {
                    if (!inDrEdge(se)) continue;
                    anyFlag = true;
                    if (analysis.region.edges.insert(se).second) {
                        addedEdges++;
                    }
                }
            };
            auto expandOverlapBoundary = [&](const NodePtr& boundary,
                                             std::vector<NodePtr>& reached) -> bool {
                reached.clear();
                if (!boundary || !inDrNode(boundary)) {
                    return false;
                }
                bool changed = false;
                std::unordered_set<NodePtr> visited;
                std::queue<NodePtr> localQ;
                visited.insert(boundary);
                localQ.push(boundary);
                while (!localQ.empty()) {
                    NodePtr cur = localQ.front();
                    localQ.pop();
                    overlapNodes.insert(cur);
                    if (analysis.region.nodes.insert(cur).second) {
                        addedNodes++;
                        changed = true;
                    }
                    for (const auto& e : view.getOutgoingEdges(cur)) {
                        if (!inDrEdge(e)) continue;
                        if (analysis.region.edges.insert(e).second) {
                            addedEdges++;
                            changed = true;
                        }
                        auto out = view.getOutput(e);
                        if (!out) continue;
                        if (!inDrNode(out)) continue;
                        if (visited.insert(out).second) {
                            localQ.push(out);
                        }
                    }
                }
                reached.assign(visited.begin(), visited.end());
                return changed;
            };
            for (const auto& n : multiNodes) {
                bool any = false;
                std::vector<NodePtr> boundaryReach;
                if (expandOverlapBoundary(n, boundaryReach)) {
                    any = true;
                }
                for (const auto& source : boundaryReach) {
                    addScopeForSource(source, any);
                }
                addScopeForSource(n, any);
                if (any) {
                    scopeSeeds++;
                    multiNonEmpty++;
                    effectiveMultiNodes.insert(n);
                } else {
                    scopeEmpty++;
                    multiEmpty++;
                }
            }
            lastOverlapMultiNodes_.clear();
            lastOverlapMultiNodes_.insert(effectiveMultiNodes.begin(), effectiveMultiNodes.end());
            planProfile.overlapClosureAddedNodes += addedNodes;
            planProfile.overlapClosureMs += toMs(nowMs() - tStart);
            if (incRegionalProfileEnabled) {
                std::cout << "[inc-regional] overlap multi_nodes=" << multiNonEmpty
                          << " multi_nodes_trivial=" << multiEmpty
                          << " multi_nodes_total=" << (multiNonEmpty + multiEmpty)
                          << "\n";
            }
            if (incRegionalProfileEnabled && (addedNodes > 0 || addedEdges > 0)) {
                std::cout << "[inc-regional] overlap closure reason=" << reason
                          << " added_nodes=" << addedNodes
                          << " added_edges=" << addedEdges
                          << " scope_seeds=" << scopeSeeds
                          << " scope_empty=" << scopeEmpty
                          << " region_nodes=" << analysis.region.nodes.size()
                          << " region_edges=" << analysis.region.edges.size()
                          << "\n";
            }
            return addedNodes > 0 || addedEdges > 0;
        };

        auto expandRegionToBoundaryComponents = [&](const std::unordered_set<NodePtr>& seeds) -> bool {
            const auto& dr = analyzer.lastDeltaReachable();
            if (seeds.empty()) return false;
            auto inDrNode = [&](const NodePtr& n) {
                return dr.nodes.empty() || dr.nodes.count(n);
            };
            auto inDrEdge = [&](const EdgePtr& e) {
                return dr.edges.empty() || dr.edges.count(e);
            };
            bool changed = false;
            std::queue<NodePtr> q;
            std::unordered_set<NodePtr> seen;
            for (const auto& n : seeds) {
                if (!n) continue;
                if (!inDrNode(n)) continue;
                if (analysis.region.nodes.insert(n).second) {
                    changed = true;
                }
                if (seen.insert(n).second) {
                    q.push(n);
                }
            }
            while (!q.empty()) {
                NodePtr cur = q.front();
                q.pop();
                for (const auto& e : view.getOutgoingEdges(cur)) {
                    if (!inDrEdge(e)) continue;
                    if (analysis.region.edges.insert(e).second) {
                        changed = true;
                    }
                    auto out = view.getOutput(e);
                    if (!out) continue;
                    if (!inDrNode(out)) continue;
                    if (analysis.region.nodes.insert(out).second) {
                        changed = true;
                    }
                    if (seen.insert(out).second) {
                        q.push(out);
                    }
                }
                for (const auto& e : view.getIncomingEdges(cur)) {
                    if (!inDrEdge(e)) continue;
                    if (analysis.region.edges.insert(e).second) {
                        changed = true;
                    }
                    for (const auto& in : view.getInputs(e)) {
                        if (!inDrNode(in)) continue;
                        if (analysis.region.nodes.insert(in).second) {
                            changed = true;
                        }
                        if (seen.insert(in).second) {
                            q.push(in);
                        }
                    }
                }
            }
            return changed;
        };

        auto buildPlan = [&]() {
            AnalyzerResultCache cache;
            cache.region = analysis.region;
            cache.boundaries = analysis.boundaries;
            cache.mergeableAnchorsByHead = analysis.mergeableAnchorsByHead;
            auto tBuildStart = nowMs();
            RegionalInsertPlan plan = RegionalInsertPlanBuilder::build(cache);
            planProfile.buildMs += toMs(nowMs() - tBuildStart);

            auto tFilterStart = nowMs();
            size_t checked = 0;
            size_t filtered = 0;
            size_t candidateTotal = 0;
            size_t candidateMax = 0;
            for (const auto& v : plan.boundaryNodes) {
                checked++;
                auto it = plan.anchorCandidates.find(v);
                const size_t count = (it == plan.anchorCandidates.end()) ? 0 : it->second.size();
                candidateTotal += count;
                if (count > candidateMax) {
                    candidateMax = count;
                }
                if (count == 0) {
                    filtered++;
                }
            }
            planProfile.anchorFilterMs += toMs(nowMs() - tFilterStart);
            planProfile.anchorsChecked += checked;
            planProfile.anchorsFiltered += filtered;
            planProfile.boundaryCount = plan.boundaryNodes.size();
            planProfile.anchorCandidateTotal = candidateTotal;
            planProfile.anchorCandidateMax = candidateMax;
            planProfile.anchorFilterSkipped = planProfile.anchorFilterSkipped || !plan.boundaryNodes.empty();
            return plan;
        };

        auto hasIndependentAnchorCover = [&](const RegionalInsertPlan& candidatePlan) {
            if (candidatePlan.boundaryNodes.empty()) {
                return true;
            }
            if (!candidatePlan.mergeReady) {
                return false;
            }
            std::unordered_map<int, NodePtr> anchorVarOwner;
            for (const auto& boundary : candidatePlan.boundaryNodes) {
                auto it = candidatePlan.anchorCandidates.find(boundary);
                if (it == candidatePlan.anchorCandidates.end() || it->second.empty()) {
                    return false;
                }
                bool hasVar = false;
                for (const auto& anchor : it->second) {
                    int varIdx = -1;
                    bool ok = false;
                    if (anchor.kind == incra::IncRegionAnalysis::AnchorKind::Node) {
                        ok = anchor.node && anchor.node->isFact && anchor.node->getProbability() < 1.0 &&
                                formulaManager.peekVarIndex(*anchor.node, varIdx);
                    } else {
                        ok = anchor.edge && !anchor.edge->isDeterministic() &&
                                formulaManager.peekVarIndex(*anchor.edge, varIdx);
                    }
                    if (ok && varIdx >= 0) {
                        hasVar = true;
                        auto [ownerIt, inserted] = anchorVarOwner.emplace(varIdx, boundary);
                        if (!inserted && ownerIt->second.get() != boundary.get()) {
                            return false;
                        }
                    }
                }
                if (!hasVar) {
                    return false;
                }
            }
            return true;
        };

        auto boundaryDownstreamConverges = [&](const RegionalInsertPlan& candidatePlan) {
            if (candidatePlan.boundaryNodes.size() < 2) {
                return false;
            }
            const auto& dr = analyzer.lastDeltaReachable();
            auto inDrNode = [&](const NodePtr& n) {
                return dr.nodes.empty() || dr.nodes.count(n);
            };
            auto inDrEdge = [&](const EdgePtr& e) {
                return dr.edges.empty() || dr.edges.count(e);
            };
            std::unordered_map<NodePtr, NodePtr> owner;
            for (const auto& boundary : candidatePlan.boundaryNodes) {
                if (!boundary || !inDrNode(boundary)) {
                    continue;
                }
                std::queue<NodePtr> q;
                std::unordered_set<NodePtr> seenLocal;
                q.push(boundary);
                seenLocal.insert(boundary);
                while (!q.empty()) {
                    NodePtr cur = q.front();
                    q.pop();
                    auto [it, inserted] = owner.emplace(cur, boundary);
                    if (!inserted && it->second.get() != boundary.get()) {
                        return true;
                    }
                    for (const auto& e : view.getOutgoingEdges(cur)) {
                        if (!inDrEdge(e)) {
                            continue;
                        }
                        NodePtr out = view.getOutput(e);
                        if (!out || !inDrNode(out)) {
                            continue;
                        }
                        if (seenLocal.insert(out).second) {
                            q.push(out);
                        }
                    }
                }
            }
            return false;
        };

        // === 3) Build plan (with degenerate filtering) ===
        RegionalInsertPlan plan;
        bool planBuilt = false;

        lastTiming_ = Timing{};
        lastTiming_.analyzeMs = toMs(t1 - t0);
        lastTiming_.sccCloseMs = toMs(t2 - t1);

        const int kMaxExpandAttempts = 3;
        int expandAttempts = 0;
        bool regionDirty = true;
        bool boundaryFallbackApplied = false;
        int loopGuard = 0;
        while (loopGuard++ < 10) {
            bool recompute = regionDirty;
            if (regionDirty) {
                if (regionTouchesCycle(view, analysis.region.nodes)) {
                    auto& depGraph = depGraphForReachable("scc_close_loop", analyzer.lastDeltaReachable());
                    auto tScc = nowMs();
                    bool sccExpanded = RegionSccClosure::closeToScc(
                            analysis.region, analyzer.lastDeltaReachable(), depGraph);
                    planProfile.sccExpandMs += toMs(nowMs() - tScc);
                    if (sccExpanded) {
                        recompute = true;
                        recordRegionChange("scc_close_loop");
                    }
                }
                if (closeRegionInputs("loop")) {
                    recompute = true;
                    recordRegionChange("input_close_loop");
                }
                if (recompute) {
                    auto tB = nowMs();
                    analysis.boundaries = analyzer.recomputeBoundaries(analysis.region);
                    planProfile.recomputeBoundariesMs += toMs(nowMs() - tB);
                    auto tA = nowMs();
                    analysis.mergeableAnchorsByHead = analyzer.recomputeAnchors(analysis.region, analysis.boundaries);
                    planProfile.recomputeAnchorsMs += toMs(nowMs() - tA);
                }
                regionDirty = false;
            }

            plan = buildPlan();
            planBuilt = true;

            if (opt_.enableOverlapExpansion) {
                const bool independentAnchors = hasIndependentAnchorCover(plan);
                const bool downstreamConverges = independentAnchors && boundaryDownstreamConverges(plan);
                if ((!independentAnchors || downstreamConverges) &&
                        closeDependentBoundaryOverlaps("preplan")) {
                    recordRegionChange("overlap_preplan");
                    regionDirty = true;
                    planBuilt = false;
                    continue;
                }
                if (independentAnchors && !downstreamConverges &&
                        incRegionalProfileEnabled && !plan.boundaryNodes.empty()) {
                    std::cout << "[inc-regional] overlap closure skip reason=independent_anchors"
                              << " boundary_nodes=" << plan.boundaryNodes.size()
                              << "\n";
                }
            }

            if (!opt_.enableFallbackToClassicInsertion || plan.mergeReady) {
                break;
            }

            std::unordered_set<NodePtr> failed;
            for (const auto& v : plan.boundaryNodes) {
                auto it = plan.anchorCandidates.find(v);
                if (it == plan.anchorCandidates.end() || it->second.empty()) {
                    failed.insert(v);
                }
            }
            if (failed.empty()) break;
            stats_.planExpandFailedBoundaries += failed.size();
            if (incRegionalProfileEnabled) {
                std::vector<NodePtr> failedList(failed.begin(), failed.end());
                std::sort(failedList.begin(), failedList.end(),
                        [&](const NodePtr& a, const NodePtr& b) {
                            return incra::node_id(a) < incra::node_id(b);
                        });
                for (const auto& v : failedList) {
                    std::cout << "[inc-regional] plan_expand_failed head=" << incra::node_id(v) << "\n";
                }
            }

            if (expandAttempts >= kMaxExpandAttempts) {
                if (!boundaryFallbackApplied) {
                    auto tFallback = nowMs();
                    bool fallbackExpanded = expandRegionToBoundaryComponents(failed);
                    planProfile.expandMs += toMs(nowMs() - tFallback);
                    boundaryFallbackApplied = true;
                    if (fallbackExpanded) {
                        recordRegionChange("fallback_boundary_components");
                        regionDirty = true;
                        continue;
                    }
                }
                break;
            }

            planProfile.expandAttempts++;
            auto tExpand = nowMs();
            bool expanded = analyzer.expandRegionFromSources(analysis.region, failed, analyzer.lastDeltaReachable());
            planProfile.expandMs += toMs(nowMs() - tExpand);
            if (!expanded) break;
            expandAttempts++;
            regionDirty = true;
            recordRegionChange("expand_missing_anchors");
            if (incRegionalProfileEnabled) {
                std::cout << "[inc-regional] expanded region after missing anchors: "
                          << "attempt=" << expandAttempts
                          << " added_nodes=" << analysis.region.nodes.size()
                          << " added_edges=" << analysis.region.edges.size()
                          << " failed=" << failed.size() << "\n";
            }
        }
        if (!planBuilt) {
            plan = buildPlan();
            planBuilt = true;
        }

        stats_.planExpandAttempts = planProfile.expandAttempts;
        if (planProfile.expandAttempts > 0) {
            stats_.planExpandRegionNodes = analysis.region.nodes.size();
            stats_.planExpandRegionEdges = analysis.region.edges.size();
        }

        // If the region is already close to delta-reachable, expand to full DR to avoid
        // partial updates on a tiny remainder.
        if (opt_.enableNearFullExpansion) {
            const auto& dr = analyzer.lastDeltaReachable();
            constexpr double kNearFullThreshold = 0.95;
            if (!dr.nodes.empty()) {
                double ratio = double(analysis.region.nodes.size()) / double(dr.nodes.size());
                if (ratio >= kNearFullThreshold && analysis.region.nodes.size() < dr.nodes.size()) {
                    if (incRegionalProfileEnabled) {
                        std::cout << "[inc-regional] expand region to delta-reach (near-full ratio="
                                  << ratio << ")\n";
                    }
                    analysis.region.nodes = dr.nodes;
                    analysis.region.edges = dr.edges;
                    stats_.nearFullExpanded = true;
                    stats_.nearFullRegionNodes = analysis.region.nodes.size();
                    stats_.nearFullRegionEdges = analysis.region.edges.size();
                    auto tB = nowMs();
                    analysis.boundaries = analyzer.recomputeBoundaries(analysis.region);
                    planProfile.recomputeBoundariesMs += toMs(nowMs() - tB);
                    auto tA = nowMs();
                    analysis.mergeableAnchorsByHead = analyzer.recomputeAnchors(analysis.region, analysis.boundaries);
                    planProfile.recomputeAnchorsMs += toMs(nowMs() - tA);
                    plan = buildPlan();
                    recordRegionChange("near_full_expand");
                }
            }
        }

        {
            const auto& dr = analyzer.lastDeltaReachable();
            const size_t regionNodes = plan.regionNodes.size();
            const size_t drNodes = dr.nodes.size();
            const double ratio = drNodes == 0 ? 0.0 : double(regionNodes) / double(drNodes);
            if (emitRegionalDiagnostics) {
                std::cout << "[inc-regional-final] region_nodes=" << regionNodes
                          << " dr_nodes=" << drNodes
                          << " ratio=" << ratio
                          << "\n";
            }
        }

        auto t3 = nowMs();
        lastTiming_.planMs = toMs(t3 - t2);

        if (incRegionalProfileEnabled) {
            std::cout << "[inc-regional-plan]"
                      << " build_ms=" << planProfile.buildMs
                      << " snapshot_ms=" << planProfile.snapshotMs
                      << " anchor_filter_ms=" << planProfile.anchorFilterMs
                      << " anchors_checked=" << planProfile.anchorsChecked
                      << " anchors_filtered=" << planProfile.anchorsFiltered
                      << " boundary_nodes=" << planProfile.boundaryCount
                      << " anchor_candidates_total=" << planProfile.anchorCandidateTotal
                      << " anchor_candidates_max=" << planProfile.anchorCandidateMax
                      << " anchor_filter_skipped=" << (planProfile.anchorFilterSkipped ? 1 : 0)
                      << " expand_ms=" << planProfile.expandMs
                      << " expand_attempts=" << planProfile.expandAttempts
                      << " overlap_closure_ms=" << planProfile.overlapClosureMs
                      << " overlap_closure_runs=" << planProfile.overlapClosureRuns
                      << " overlap_closure_added_nodes=" << planProfile.overlapClosureAddedNodes
                      << " overlap_closure_dependent=" << planProfile.overlapClosureDependent
                      << " overlap_closure_independent=" << planProfile.overlapClosureIndependent
                      << " overlap_closure_empty_owners=" << planProfile.overlapClosureEmptyOwners
                      << " input_closure_ms=" << planProfile.inputClosureMs
                      << " input_closure_runs=" << planProfile.inputClosureRuns
                      << " input_closure_added_nodes=" << planProfile.inputClosureAddedNodes
                      << " input_closure_added_edges=" << planProfile.inputClosureAddedEdges
                      << " recompute_boundaries_ms=" << planProfile.recomputeBoundariesMs
                      << " recompute_anchors_ms=" << planProfile.recomputeAnchorsMs
                      << " scc_expand_ms=" << planProfile.sccExpandMs
                      << "\n";
        }

        stats_.regionNodeCount = plan.regionNodes.size();
        stats_.regionEdgeCount = analysis.region.edges.size();
        stats_.drNodeCount = analysisStats.dr_nodes;
        stats_.drEdgeCount = analysisStats.dr_edges;
        stats_.boundaryNodeCount = plan.boundaryNodes.size();

        struct FormulaSnapshot {
            bool has = false;
            FormulaNodeRef formula{};
        };
        std::unordered_map<NodePtr, FormulaSnapshot> preNodeSnapshot;
        std::unordered_map<EdgePtr, FormulaSnapshot> preEdgeSnapshot;
        bool snapshotTaken = false;
        auto takeSnapshot = [&]() {
            auto tSnapStart = nowMs();
            if (!snapshotTaken) {
                snapshotTaken = true;
                preNodeSnapshot.reserve(analysis.region.nodes.size());
                preEdgeSnapshot.reserve(analysis.region.edges.size());
            }
            for (const auto& node : analysis.region.nodes) {
                if (preNodeSnapshot.count(node)) {
                    continue;
                }
                auto it = nodeFormulas.find(node);
                if (it != nodeFormulas.end()) {
                    preNodeSnapshot[node] = {true, it->second};
                } else {
                    preNodeSnapshot[node] = {false, formulaManager.getFalse()};
                }
            }
            for (const auto& edge : analysis.region.edges) {
                if (preEdgeSnapshot.count(edge)) {
                    continue;
                }
                auto it = edgeFormulas.find(edge);
                if (it != edgeFormulas.end()) {
                    preEdgeSnapshot[edge] = {true, it->second};
                } else {
                    preEdgeSnapshot[edge] = {false, formulaManager.getFalse()};
                }
            }
            lastTiming_.fallbackSnapshotMs += toMs(nowMs() - tSnapStart);
        };
        auto restoreSnapshot = [&]() {
            if (!snapshotTaken) return;
            auto tRestoreStart = nowMs();
            for (const auto& [node, snap] : preNodeSnapshot) {
                if (snap.has) {
                    nodeFormulas[node] = snap.formula;
                } else {
                    nodeFormulas.erase(node);
                }
            }
            for (const auto& [edge, snap] : preEdgeSnapshot) {
                if (snap.has) {
                    edgeFormulas[edge] = snap.formula;
                } else {
                    edgeFormulas.erase(edge);
                }
            }
            lastTiming_.fallbackRestoreMs += toMs(nowMs() - tRestoreStart);
        };

        auto fallbackClassic = [&]() {
            auto tFallbackStart = nowMs();
            restoreSnapshot();
            buildFormulasIncCyclewise(view, formulaManager, nodeFormulas, edgeFormulas, changedNodes);
            lastTiming_.fallbackMs = toMs(nowMs() - tFallbackStart);
            lastTiming_.totalWithFallbackMs = lastTiming_.totalMs + lastTiming_.fallbackMs;
            stats_.usedFallback = true;
        };

        if (!plan.mergeReady && opt_.enableFallbackToClassicInsertion) {
            if (incRegionalProfileEnabled) {
                std::cout << "[inc-regional] mergeReady=false, fallback to classic\n";
            }
            lastTiming_.fallbackReason = "mergeReady=false";
            lastTiming_.totalMs = toMs(nowMs() - t0);
            lastTiming_.totalWithFallbackMs = lastTiming_.totalMs;
            fallbackClassic();
            return;
        }

        std::unordered_set<NodePtr> calibrationSkip;
        bool needRebuild = true;
        bool rebuiltOnce = false;
        typename RegionalDDRebuilder<FormulaManagerT, FormulaNodeRef>::Result rebuildRes;
        typename BoundaryGateCalibrator<FormulaManagerT, FormulaNodeRef>::Result calibRes;
        size_t calibrateAttempt = 0;
        typename BoundaryGateCalibrator<FormulaManagerT, FormulaNodeRef>::CalibrationCache calibrateCache;

        while (true) {
            if (needRebuild) {
                const bool needSnapshot = opt_.enableFallbackToClassicInsertion && !plan.boundaryNodes.empty();
                if (rebuiltOnce && needSnapshot) {
                    restoreSnapshot();
                }
                if (needSnapshot) {
                    takeSnapshot();
                }

                auto tRebuildStart = nowMs();
                // === 4) Regional rebuild ===
                const bool rebuildTouchesCycle = regionTouchesCycle(view, plan.regionNodes);
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] regionTouchesCycle rebuild=" << (rebuildTouchesCycle ? 1 : 0)
                              << "\n";
                }
                typename RegionalDDRebuilder<FormulaManagerT, FormulaNodeRef>::DagPlan dagPlan;
                double dagBuildMs = 0.0;
                const bool dagFastPath = false;  // keep insert algorithm aligned with inc-naive (cyclewise)
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional] dag_fast_path=" << (dagFastPath ? 1 : 0)
                              << " dag_build_ms=" << dagBuildMs << "\n";
                }
                if (dagFastPath) {
                    rebuildRes = RegionalDDRebuilder<FormulaManagerT, FormulaNodeRef>::rebuildInsertRegionDag(
                        view, formulaManager, nodeFormulas, edgeFormulas, plan, analysis.region.edges, dagPlan, dagBuildMs);
                    if (rebuildRes.fallbackToCycle) {
                        if (incRegionalProfileEnabled) {
                            std::cout << "[inc-regional] dag_fast_path fallback reason=missing_inside_region"
                                      << " missing=" << rebuildRes.missingInsideRegion
                                      << "\n";
                        }
                        auto tDepGraphStart = nowMs();
                        const CycleDependencyGraph* depGraphPtr =
                                &depGraphForReachable("rebuild_fallback", analyzer.lastDeltaReachable());
                        auto tDepGraphEnd = nowMs();
                        const double depGraphMs = toMs(tDepGraphEnd - tDepGraphStart);
                        rebuildRes = RegionalDDRebuilder<FormulaManagerT, FormulaNodeRef>::rebuildInsertRegion(
                            view, formulaManager, nodeFormulas, edgeFormulas, plan, analysis.region.edges, depGraphPtr);
                        rebuildRes.timing.depGraphMs += depGraphMs;
                        rebuildRes.timing.totalMs += depGraphMs;
                    }
                } else {
                    auto tDepGraphStart = nowMs();
                    const CycleDependencyGraph* depGraphPtr =
                            &depGraphForReachable("rebuild", analyzer.lastDeltaReachable());
                    auto tDepGraphEnd = nowMs();
                    const double depGraphMs = toMs(tDepGraphEnd - tDepGraphStart);
                    rebuildRes = RegionalDDRebuilder<FormulaManagerT, FormulaNodeRef>::rebuildInsertRegion(
                        view, formulaManager, nodeFormulas, edgeFormulas, plan, analysis.region.edges, depGraphPtr);
                    rebuildRes.timing.depGraphMs += depGraphMs;
                    rebuildRes.timing.totalMs += depGraphMs;
                }
                changedNodes.insert(rebuildRes.changedNodes.begin(), rebuildRes.changedNodes.end());
                const auto& rt = rebuildRes.timing;
                if (incRegionalProfileEnabled) {
                    std::cout << "[inc-regional rebuild] timing(ms):"
                              << " snapshot=" << rt.snapshotMs
                              << " preConfig=" << rt.preConfigMs
                              << " initNodes=" << rt.initNodesMs
                              << " initEdges=" << rt.initEdgesMs
                              << " depGraph=" << rt.depGraphMs
                              << " regionCycles=" << rt.regionCyclesMs
                              << " indegree=" << rt.indegreeMs
                              << " rebuildLoop=" << rt.rebuildLoopMs
                              << " total=" << rt.totalMs
                              << " reorder=" << rt.reorderMs
                              << " edgesProcessed=" << rt.edgesProcessed
                              << " edgeRequeued=" << rt.edgeRequeued
                              << " edgesRebuilt=" << rt.edgesRebuilt
                              << " nodesUpdated=" << rt.nodesUpdated
                              << " cycleCount=" << rt.regionCycleCount
                              << " make_and_calls=" << rt.makeAndCalls
                              << " make_and_ms=" << rt.makeAndMs
                              << " make_or_calls=" << rt.makeOrCalls
                              << " make_or_ms=" << rt.makeOrMs
                              << " input_literal_calls=" << rt.inputLiteralCalls
                              << " input_literal_missing=" << rt.inputLiteralMissing
                              << " input_literal_ms=" << rt.inputLiteralMs
                              << "\n";
                }
                auto tRebuildEnd = nowMs();
                const double rebuildMs = toMs(tRebuildEnd - tRebuildStart);
                lastTiming_.rebuildMs = rebuildMs;
                lastTiming_.rebuildTotalMs += rebuildMs;
                lastTiming_.rebuildDetail = rebuildRes.timing;

                rebuiltOnce = true;
            }

            // === 5) Calibrate boundary gates ===
            auto t4 = nowMs();
            calibRes = BoundaryGateCalibrator<FormulaManagerT, FormulaNodeRef>::calibrate(
                view, formulaManager, plan, rebuildRes.snapshot, nodeFormulas, opt_.eps,
                calibrationSkip.empty() ? nullptr : &calibrationSkip,
                &calibrateCache);
            auto t5 = nowMs();
            const double calibrateMs = toMs(t5 - t4);
            lastTiming_.calibrateMs = calibrateMs;
            lastTiming_.calibrateTotalMs += calibrateMs;
            lastTiming_.totalMs = toMs(t5 - t0);

            const auto& ct = calibRes.timing;
            const size_t calibIdx = calibrateAttempt++;
            auto addCalibInfoDouble = [&](const char* key, double value) {
                debugger.addInfo("inc_regional_calibrate_" + std::to_string(calibIdx) + "_" + key,
                                 std::to_string(value));
            };
            auto addCalibInfoSize = [&](const char* key, size_t value) {
                debugger.addInfo("inc_regional_calibrate_" + std::to_string(calibIdx) + "_" + key,
                                 std::to_string(value));
            };
            addCalibInfoDouble("total_ms", ct.totalMs);
            addCalibInfoDouble("target_wmc_ms", ct.targetWmcMs);
            addCalibInfoDouble("anchor_loop_ms", ct.anchorLoopMs);
            addCalibInfoDouble("anchor_wmc_ms", ct.anchorWmcMs);
            addCalibInfoDouble("anchor_overhead_ms", ct.anchorOverheadMs);
            addCalibInfoDouble("weight_set_ms", ct.weightSetMs);
            addCalibInfoDouble("snapshot_wmc_ms", ct.snapshotWmcMs);
            addCalibInfoDouble("alpha_wmc_ms", ct.alphaWmcMs);
            addCalibInfoDouble("beta_wmc_ms", ct.betaWmcMs);
            addCalibInfoDouble("overhead_ms", ct.overheadMs);
            addCalibInfoSize("boundary_total", ct.boundaryTotal);
            addCalibInfoSize("boundary_visited", ct.boundaryVisited);
            addCalibInfoSize("boundary_skipped", ct.boundarySkipped);
            addCalibInfoSize("candidates", ct.anchorCandidates);
            addCalibInfoSize("tried", ct.anchorTried);
            addCalibInfoSize("used", ct.anchorUsed);
            addCalibInfoSize("invalid", ct.anchorInvalid);
            addCalibInfoSize("missing_snapshot", ct.missingSnapshot);
            addCalibInfoSize("degenerate", ct.degenerate);
            addCalibInfoSize("calibrated", ct.calibrated);
            addCalibInfoSize("failed", ct.failed);
            addCalibInfoSize("target_cache_hits", ct.targetCacheHits);
            addCalibInfoSize("snapshot_cache_hits", ct.snapshotCacheHits);
            addCalibInfoSize("anchor_cache_hits", ct.anchorCacheHits);
            if (emitRegionalDiagnostics) {
                std::cout << "[inc-regional-calibrate-profile]"
                          << " total_ms=" << ct.totalMs
                          << " target_wmc_ms=" << ct.targetWmcMs
                          << " anchor_loop_ms=" << ct.anchorLoopMs
                          << " anchor_wmc_ms=" << ct.anchorWmcMs
                          << " anchor_overhead_ms=" << ct.anchorOverheadMs
                          << " weight_set_ms=" << ct.weightSetMs
                          << " snapshot_wmc_ms=" << ct.snapshotWmcMs
                          << " alpha_wmc_ms=" << ct.alphaWmcMs
                          << " beta_wmc_ms=" << ct.betaWmcMs
                          << " overhead_ms=" << ct.overheadMs
                          << " boundary_total=" << ct.boundaryTotal
                          << " boundary_visited=" << ct.boundaryVisited
                          << " boundary_skipped=" << ct.boundarySkipped
                          << " candidates=" << ct.anchorCandidates
                          << " tried=" << ct.anchorTried
                          << " used=" << ct.anchorUsed
                          << " invalid=" << ct.anchorInvalid
                          << " missing_snapshot=" << ct.missingSnapshot
                          << " degenerate=" << ct.degenerate
                          << " calibrated=" << ct.calibrated
                          << " failed=" << ct.failed
                          << " target_cache_hits=" << ct.targetCacheHits
                          << " snapshot_cache_hits=" << ct.snapshotCacheHits
                          << " anchor_cache_hits=" << ct.anchorCacheHits
                          << "\n";
            }

            if (calibRes.failedNodes.empty()) {
                break;
            }

            if (!opt_.enableFallbackToClassicInsertion) {
                break;
            }

            if (incRegionalProfileEnabled) {
                std::cout << "[inc-regional] calibration failed for " << calibRes.failedNodes.size()
                          << " nodes; expanding region to delta-reach\n";
            }
            calibrationSkip.insert(calibRes.failedNodes.begin(), calibRes.failedNodes.end());
            bool expanded = expandRegionToBoundaryComponents(calibRes.failedNodes);
            if (expanded) {
                recordRegionChange("calibration_expand");
                auto tB = nowMs();
                analysis.boundaries = analyzer.recomputeBoundaries(analysis.region);
                planProfile.recomputeBoundariesMs += toMs(nowMs() - tB);
                auto tA = nowMs();
                analysis.mergeableAnchorsByHead = analyzer.recomputeAnchors(analysis.region, analysis.boundaries);
                planProfile.recomputeAnchorsMs += toMs(nowMs() - tA);
                plan = buildPlan();
                planBuilt = true;
            }
            needRebuild = expanded;
            if (!needRebuild) {
                continue;
            }
        }

        stats_.calibratedCount = calibRes.applied.size();
        lastCalibrations_ = calibRes.applied;
        lastOverrides_ = calibRes.weightOverrides;
        lastPlanRegionNodes_ = plan.regionNodes;
        lastPlanBoundaryNodes_ = plan.boundaryNodes;
        if (DerivationGraphViewInterface::isDumpDotEnabled()) {
            static std::atomic<size_t> regionCounter{0};
            const size_t idx = ++regionCounter;
            const std::string filename = "inc-region-" + std::to_string(idx) + ".dot";
            analyzer.toDotForAnalysis(
                    DerivationGraphViewInterface::qualifyDumpPath(filename), analysis);
        }
        stats_.regionNodeCount = plan.regionNodes.size();
        stats_.regionEdgeCount = analysis.region.edges.size();
        stats_.boundaryNodeCount = plan.boundaryNodes.size();
        incRegionalTurnSummary.valid = true;
        incRegionalTurnSummary.boundaryTotal = plan.boundaryNodes.size();
        incRegionalTurnSummary.regionNodeCount = plan.regionNodes.size();
        incRegionalTurnSummary.deltaReachNodeCount = analyzer.lastDeltaReachable().nodes.size();
        incRegionalTurnSummary.usedCalibrationOverrides = !calibRes.weightOverrides.empty();
        incRegionalTurnSummary.trueRegionalReuse =
                incRegionalTurnSummary.boundaryTotal > 0 &&
                incRegionalTurnSummary.regionNodeCount < incRegionalTurnSummary.deltaReachNodeCount;

        // Apply calibrated gate weights so the existing (non-rebuilt) outside-of-region formulas
        // evaluate consistently under the new boundary targets. Also snapshot original weights for WMC routing.
        if (!calibRes.weightOverrides.empty()) {
            incRegionalOutputProfile.active = true;
            incRegionalOutputProfile.overrideCount = calibRes.weightOverrides.size();
            incRegionalOutputProfile.regionNodes = plan.regionNodes;
            incRegionalOutputProfile.boundaryNodes = plan.boundaryNodes;
            incRegionalOutputProfile.deltaReachableNodes = analyzer.lastDeltaReachable().nodes;
            incRegionalOutputProfile.deltaReachableEdges = analyzer.lastDeltaReachable().edges;
            incRegionalOutputProfile.originalWeights.clear();
            incRegionalOutputProfile.overrideWeights.clear();
            incRegionalOutputProfile.boundaryUpstreamAnchorVars.clear();
            incRegionalOutputProfile.boundaryOutputTargets.clear();
        }
        for (const auto& [varIdx, weights] : calibRes.weightOverrides) {
            auto oldW = formulaManager.getVariableWeight(varIdx);
            incRegionalOutputProfile.originalWeights[varIdx] = {oldW.posWeight, oldW.negWeight};
            incRegionalOutputProfile.overrideWeights[varIdx] = {weights.first, weights.second};
            formulaManager.setVariableWeight(varIdx, weights.first, weights.second);
        }
        if (!calibRes.applied.empty()) {
            std::unordered_map<NodePtr, std::unordered_set<int>> boundaryAnchorVars;
            boundaryAnchorVars.reserve(calibRes.applied.size());
            for (const auto& rec : calibRes.applied) {
                if (!rec.v) {
                    continue;
                }
                boundaryAnchorVars[rec.v].insert(rec.varIdx);
                if (rec.v->needOutput) {
                    incRegionalOutputProfile.boundaryOutputTargets[rec.v] = rec.target;
                }
            }
            const auto& dr = analyzer.lastDeltaReachable();
            auto inDrNode = [&](const NodePtr& n) {
                return dr.nodes.empty() || dr.nodes.count(n);
            };
            auto inDrEdge = [&](const EdgePtr& e) {
                return dr.edges.empty() || dr.edges.count(e);
            };
            for (const auto& b : plan.boundaryNodes) {
                if (!b || !inDrNode(b)) {
                    continue;
                }
                std::unordered_set<int> vars;
                std::unordered_set<NodePtr> visited;
                std::queue<NodePtr> q;
                visited.insert(b);
                q.push(b);
                while (!q.empty()) {
                    NodePtr cur = q.front();
                    q.pop();
                    if (cur.get() != b.get()) {
                        auto it = boundaryAnchorVars.find(cur);
                        if (it != boundaryAnchorVars.end()) {
                            for (int varIdx : it->second) {
                                vars.insert(varIdx);
                            }
                        }
                    }
                    for (const auto& e : view.getIncomingEdges(cur)) {
                        if (!inDrEdge(e)) {
                            continue;
                        }
                        const auto& ins = view.getInputs(e);
                        for (const auto& in : ins) {
                            if (!inDrNode(in)) {
                                continue;
                            }
                            if (visited.insert(in).second) {
                                q.push(in);
                            }
                        }
                    }
                }
                if (!vars.empty()) {
                    std::vector<int> varList(vars.begin(), vars.end());
                    std::sort(varList.begin(), varList.end());
                    incRegionalOutputProfile.boundaryUpstreamAnchorVars.emplace(b, std::move(varList));
                }
            }
        }
        if (incRegionalProfileEnabled) {
            std::cout << "[inc-regional] applied_gate_overrides=" << calibRes.weightOverrides.size() << "\n";
        }

        stats_.usedFallback = false;

        const auto& rt = rebuildRes.timing;
        const double accountedMs = lastTiming_.analyzeMs + lastTiming_.sccCloseMs + lastTiming_.planMs
                + lastTiming_.rebuildTotalMs + lastTiming_.calibrateTotalMs
                + lastTiming_.fallbackSnapshotMs + lastTiming_.fallbackRestoreMs;
        lastTiming_.overheadMs = std::max(0.0, lastTiming_.totalMs - accountedMs);

        if (emitRegionalDiagnostics) {
            std::cout << "[inc-regional] timing(ms): analyze=" << lastTiming_.analyzeMs
                      << " sccClose=" << lastTiming_.sccCloseMs
                      << " plan=" << lastTiming_.planMs
                      << " rebuild=" << lastTiming_.rebuildMs
                      << " rebuild_total=" << lastTiming_.rebuildTotalMs
                      << " calibrate=" << lastTiming_.calibrateMs
                      << " calibrate_total=" << lastTiming_.calibrateTotalMs
                      << " fallback_snapshot=" << lastTiming_.fallbackSnapshotMs
                      << " fallback_restore=" << lastTiming_.fallbackRestoreMs
                      << " overhead=" << lastTiming_.overheadMs
                      << " total=" << lastTiming_.totalMs
                      << " fallback=" << lastTiming_.fallbackMs
                      << " total_with_fallback=" << lastTiming_.totalWithFallbackMs
                      << "\n";
        }
        const double prepInsertMs = rt.regionCyclesMs + rt.indegreeMs;
        const double prepTotalMs = rt.preConfigMs + rt.initNodesMs + rt.initEdgesMs
                + rt.regionCyclesMs + rt.indegreeMs;
        if (emitRegionalDiagnostics) {
            std::cout << "[inc-regional-insert-profile]"
                      << " dep_graph_ms=" << rt.depGraphMs
                      << " dep_graph_scope=" << (depGraphScope ? depGraphScope : "unknown")
                      << " reach_nodes=" << analysisStats.dr_nodes
                      << " reach_edges=" << analysisStats.dr_edges
                      << " rederive_ms=0"
                      << " preconfig_ms=" << rt.preConfigMs
                      << " init_nodes_ms=" << rt.initNodesMs
                      << " init_edges_ms=" << rt.initEdgesMs
                      << " prep_insert_ms=" << prepInsertMs
                      << " prep_total_ms=" << prepTotalMs
                      << " insert_ms=" << rt.rebuildLoopMs
                      << " fc_ms=" << lastTiming_.rebuildMs
                      << " total_ms=" << lastTiming_.totalMs
                      << " rebuild_total_ms=" << rt.totalMs
                      << " make_and_calls=" << rt.makeAndCalls
                      << " make_and_ms=" << rt.makeAndMs
                      << " make_or_calls=" << rt.makeOrCalls
                      << " make_or_ms=" << rt.makeOrMs
                      << " edge_requeued=" << rt.edgeRequeued
                      << " input_literal_calls=" << rt.inputLiteralCalls
                      << " input_literal_missing=" << rt.inputLiteralMissing
                      << " input_literal_ms=" << rt.inputLiteralMs
                      << "\n";
        }
    }

    const Stats& getStats() const { return stats_; }
    const Timing& getTiming() const { return lastTiming_; }
    const std::vector<CalibrationRecord>& getCalibrations() const { return lastCalibrations_; }
    const std::unordered_map<int, std::pair<double,double>>& getWeightOverrides() const {
        return lastOverrides_;
    }
    const std::set<NodePtr>& getRegionNodes() const { return lastPlanRegionNodes_; }
    const std::set<NodePtr>& getBoundaryNodes() const { return lastPlanBoundaryNodes_; }

private:
    Options opt_;
    Stats stats_;
    Timing lastTiming_;
    std::vector<CalibrationRecord> lastCalibrations_;
    std::unordered_map<int, std::pair<double,double>> lastOverrides_;
    std::set<NodePtr> lastPlanRegionNodes_;
    std::set<NodePtr> lastPlanBoundaryNodes_;
    std::unordered_set<NodePtr> lastOverlapMultiNodes_;
};
