#ifndef FORWARDCOMPILATION_H
#define FORWARDCOMPILATION_H

#include <iostream>
#include "souffle/Derivation.h"
#include "souffle/problog/DerivationGraph.h"
#include "souffle/problog/ForwardCompilationSupport.h"
#include "souffle/problog/formula/FormulaManager.h"
#include "souffle/problog/formula/CuddManager.h"
#include <queue>
#include <set>
#include <map>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>
#include <cstdlib>
#include <functional>
#include <sstream>
#include "souffle/problog/RegionalIncremental.h"

struct PrioritizedEdge {
    EdgePtr edge;
    size_t priority;
    int sequence_id;
    bool operator<(const PrioritizedEdge& other) const {
        if (priority != other.priority)
            return priority > other.priority;  // Smaller value first
        return sequence_id > other.sequence_id;  // Smaller sequence id first
    }
};

static inline bool stableNodeOrder(const NodePtr& lhs, const NodePtr& rhs) {
    if (lhs == rhs) {
        return false;
    }
    if (!lhs) {
        return true;
    }
    if (!rhs) {
        return false;
    }
    if (lhs->getId() != rhs->getId()) {
        return lhs->getId() < rhs->getId();
    }
    return lhs->getTuple().toString() < rhs->getTuple().toString();
}

static inline bool stableEdgeOrder(const EdgePtr& lhs, const EdgePtr& rhs) {
    if (lhs == rhs) {
        return false;
    }
    if (!lhs) {
        return true;
    }
    if (!rhs) {
        return false;
    }
    if (lhs->getId() != rhs->getId()) {
        return lhs->getId() < rhs->getId();
    }
    return lhs->toString() < rhs->toString();
}

// Standalone registration uses existing tuple and normalized support metadata.
// These keys order variables only; their equality never identifies events.
static inline bool standaloneEventNodeOrder(const NodePtr& lhs, const NodePtr& rhs) {
    if (!lhs || !rhs || lhs == rhs) return stableNodeOrder(lhs, rhs);
    if (!(lhs->getTuple() == rhs->getTuple())) return lhs->getTuple() < rhs->getTuple();
    if (lhs->getSemanticFactId() != rhs->getSemanticFactId()) {
        return lhs->getSemanticFactId() < rhs->getSemanticFactId();
    }
    return stableNodeOrder(lhs, rhs);
}

static inline bool standaloneEventEdgeOrder(const EdgePtr& lhs, const EdgePtr& rhs) {
    if (!lhs || !rhs || lhs == rhs) return stableEdgeOrder(lhs, rhs);
    const auto& leftHead = lhs->getOutputRef();
    const auto& rightHead = rhs->getOutputRef();
    if (!leftHead || !rightHead) {
        if (leftHead != rightHead) return !leftHead;
    } else if (!(leftHead->getTuple() == rightHead->getTuple())) {
        return leftHead->getTuple() < rightHead->getTuple();
    }
    const auto& leftSupport = lhs->getProbabilisticSupportTokens();
    const auto& rightSupport = rhs->getProbabilisticSupportTokens();
    if (leftSupport != rightSupport) return leftSupport < rightSupport;
    return stableEdgeOrder(lhs, rhs);
}

static inline bool incReorderPolicyUsesAuto() {
    return incReorderPolicy == "auto" || incReorderPolicy == "both";
}

static inline bool incReorderPolicyDisablesAuto() {
    return incReorderPolicy == "pressure" || incReorderPolicy == "explicit" || incReorderPolicy == "off";
}

static inline bool incReorderPolicyUsesExplicit() {
    return incReorderPolicy == "pressure" || incReorderPolicy == "explicit" || incReorderPolicy == "both";
}

static inline void addIncReorderStats(
        Debugger& debugger, const std::string& prefix, const FormulaReorderStats& stats) {
    debugger.addInfo(prefix + "_supported", stats.supported ? "1" : "0");
    debugger.addInfo(prefix + "_attempted", stats.attempted ? "1" : "0");
    debugger.addInfo(prefix + "_success", stats.success ? "1" : "0");
    debugger.addInfo(prefix + "_triggered", stats.triggered ? "1" : "0");
    debugger.addInfo(prefix + "_elapsed_ms", std::to_string(stats.elapsedMs));
    debugger.addInfo(prefix + "_live_before", std::to_string(stats.liveBefore));
    debugger.addInfo(prefix + "_live_after", std::to_string(stats.liveAfter));
    debugger.addInfo(prefix + "_keys_before", std::to_string(stats.keysBefore));
    debugger.addInfo(prefix + "_keys_after", std::to_string(stats.keysAfter));
    debugger.addInfo(prefix + "_dead_before", std::to_string(stats.deadBefore));
    debugger.addInfo(prefix + "_dead_after", std::to_string(stats.deadAfter));
    debugger.addInfo(prefix + "_next_before", std::to_string(stats.nextBefore));
    debugger.addInfo(prefix + "_next_after", std::to_string(stats.nextAfter));
    debugger.addInfo(prefix + "_reorderings_before", std::to_string(stats.reorderingsBefore));
    debugger.addInfo(prefix + "_reorderings_after", std::to_string(stats.reorderingsAfter));
    debugger.addInfo(prefix + "_swaps_before", std::to_string(stats.swapsBefore));
    debugger.addInfo(prefix + "_swaps_after", std::to_string(stats.swapsAfter));
    debugger.addInfo(prefix + "_time_before_s", std::to_string(stats.reorderingTimeBeforeSec));
    debugger.addInfo(prefix + "_time_after_s", std::to_string(stats.reorderingTimeAfterSec));
    debugger.addInfo(prefix + "_autodyn_before", stats.autoEnabledBefore ? "1" : "0");
    debugger.addInfo(prefix + "_autodyn_after", stats.autoEnabledAfter ? "1" : "0");
    debugger.addInfo(prefix + "_dead_counted_before", stats.deadCountedBefore ? "1" : "0");
    debugger.addInfo(prefix + "_dead_counted_after", stats.deadCountedAfter ? "1" : "0");
    debugger.addInfo(prefix + "_method_before", std::to_string(stats.methodBefore));
    debugger.addInfo(prefix + "_method_after", std::to_string(stats.methodAfter));
}

template <typename FormulaNodeRef>
static inline void maybePrepareIncReorderPolicy(
        FormulaManager<FormulaNodeRef>& formulaManager, std::size_t workScore) {
    if (incReorderPolicyDisablesAuto()) {
        Debugger& debugger = Debugger::getInstance();
        debugger.addInfo("inc_reord_auto_policy", incReorderPolicy);
        debugger.addInfo("inc_reord_auto_work_score", std::to_string(workScore));
        auto stats = formulaManager.disableIncrementalAutoReorder();
        addIncReorderStats(debugger, "inc_reord_auto_disable", stats);
        return;
    }
    if (!incReorderPolicyUsesAuto()) {
        return;
    }
    Debugger& debugger = Debugger::getInstance();
    debugger.addInfo("inc_reord_auto_policy", incReorderPolicy);
    debugger.addInfo("inc_reord_auto_work_score", std::to_string(workScore));
    debugger.addInfo("inc_reord_auto_gap", std::to_string(incReorderAutoGap));
    debugger.addInfo("inc_reord_auto_count_dead", incReorderCountDead ? "1" : "0");
    debugger.addInfo("inc_reord_auto_allow_large", incReorderAllowLarge ? "1" : "0");
    auto stats = formulaManager.configureIncrementalAutoReorder(
            incReorderAutoGap, incReorderCountDead, incReorderAllowLarge);
    addIncReorderStats(debugger, "inc_reord_auto", stats);
}

static inline std::size_t readIncReorderSizeEnv(const char* name, std::size_t fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (end == raw || (end != nullptr && *end != '\0')) {
        return fallback;
    }
    return static_cast<std::size_t>(parsed);
}

static inline std::size_t ceilMulDiv(std::size_t lhs, std::size_t rhs, std::size_t divisor) {
    if (lhs == 0 || rhs == 0 || divisor == 0) {
        return 0;
    }
    const std::size_t max = std::numeric_limits<std::size_t>::max();
    if (lhs > max / rhs) {
        return max;
    }
    const std::size_t product = lhs * rhs;
    if (product > max - (divisor - 1)) {
        return max;
    }
    return (product + divisor - 1) / divisor;
}

static inline std::size_t effectiveIncReorderWorkThreshold(
        std::size_t fixedThreshold, std::size_t baseWorkScore, Debugger& debugger) {
    const std::size_t adaptiveBp =
            readIncReorderSizeEnv("SOUFFLE_INC_REORDER_ADAPTIVE_BP", 150);
    const std::size_t adaptiveMin =
            readIncReorderSizeEnv("SOUFFLE_INC_REORDER_ADAPTIVE_MIN", 800);
    debugger.addInfo("inc_reord_explicit_adaptive_bp", std::to_string(adaptiveBp));
    debugger.addInfo("inc_reord_explicit_adaptive_min", std::to_string(adaptiveMin));
    debugger.addInfo("inc_reord_explicit_base_work_score", std::to_string(baseWorkScore));
    if (adaptiveBp == 0 || baseWorkScore == 0 || fixedThreshold == 0) {
        debugger.addInfo("inc_reord_explicit_effective_threshold", std::to_string(fixedThreshold));
        return fixedThreshold;
    }
    std::size_t adaptiveThreshold = ceilMulDiv(baseWorkScore, adaptiveBp, 10000);
    if (adaptiveThreshold < adaptiveMin) {
        adaptiveThreshold = adaptiveMin;
    }
    const std::size_t effectiveThreshold =
            std::min(fixedThreshold, adaptiveThreshold);
    debugger.addInfo("inc_reord_explicit_adaptive_threshold", std::to_string(adaptiveThreshold));
    debugger.addInfo("inc_reord_explicit_effective_threshold", std::to_string(effectiveThreshold));
    return effectiveThreshold;
}

struct IncReorderPressureScore {
    std::string mode;
    std::size_t rawDelta = 0;
    std::size_t frontierNodes = 0;
    std::size_t frontierEdges = 0;
    std::size_t bddEdgesProcessed = 0;
    std::size_t bddNodesUpdated = 0;

    std::size_t total() const {
        return rawDelta + frontierNodes + frontierEdges + bddEdgesProcessed + bddNodesUpdated;
    }

    std::size_t frontierWork() const {
        return frontierNodes + frontierEdges;
    }

    std::size_t bddWork() const {
        return bddEdgesProcessed + bddNodesUpdated;
    }
};

static inline void addIncReorderPressureScore(
        Debugger& debugger, const IncReorderPressureScore& score) {
    debugger.addInfo("inc_reord_pressure_score_version", "unified-v1");
    debugger.addInfo("inc_reord_pressure_mode", score.mode);
    debugger.addInfo("inc_reord_pressure_raw_delta", std::to_string(score.rawDelta));
    debugger.addInfo("inc_reord_pressure_frontier_nodes", std::to_string(score.frontierNodes));
    debugger.addInfo("inc_reord_pressure_frontier_edges", std::to_string(score.frontierEdges));
    debugger.addInfo("inc_reord_pressure_frontier_work", std::to_string(score.frontierWork()));
    debugger.addInfo(
            "inc_reord_pressure_bdd_edges_processed", std::to_string(score.bddEdgesProcessed));
    debugger.addInfo("inc_reord_pressure_bdd_nodes_updated", std::to_string(score.bddNodesUpdated));
    debugger.addInfo("inc_reord_pressure_bdd_work", std::to_string(score.bddWork()));
    debugger.addInfo("inc_reord_pressure_score", std::to_string(score.total()));
}

static inline std::size_t weightedIncReorderComponent(
        std::size_t component, std::size_t weightBp) {
    return ceilMulDiv(component, weightBp, 10000);
}

static inline std::size_t weightedMaxIncReorderScore(
        const IncReorderPressureScore& score, Debugger& debugger) {
    const std::size_t rawWeightBp =
            readIncReorderSizeEnv("SOUFFLE_INC_REORDER_WEIGHT_RAW_BP", 10000);
    const std::size_t frontierWeightBp =
            readIncReorderSizeEnv("SOUFFLE_INC_REORDER_WEIGHT_FRONTIER_BP", 10000);
    const std::size_t bddWeightBp =
            readIncReorderSizeEnv("SOUFFLE_INC_REORDER_WEIGHT_BDD_BP", 10000);
    const std::size_t rawWeighted = weightedIncReorderComponent(score.rawDelta, rawWeightBp);
    const std::size_t frontierWeighted =
            weightedIncReorderComponent(score.frontierWork(), frontierWeightBp);
    const std::size_t bddWeighted = weightedIncReorderComponent(score.bddWork(), bddWeightBp);
    const std::size_t weightedMax =
            std::max(rawWeighted, std::max(frontierWeighted, bddWeighted));
    debugger.addInfo("inc_reord_trigger_score_version", "weighted-max-v1");
    debugger.addInfo("inc_reord_trigger_weight_raw_bp", std::to_string(rawWeightBp));
    debugger.addInfo("inc_reord_trigger_weight_frontier_bp", std::to_string(frontierWeightBp));
    debugger.addInfo("inc_reord_trigger_weight_bdd_bp", std::to_string(bddWeightBp));
    debugger.addInfo("inc_reord_trigger_raw_weighted", std::to_string(rawWeighted));
    debugger.addInfo("inc_reord_trigger_frontier_weighted", std::to_string(frontierWeighted));
    debugger.addInfo("inc_reord_trigger_bdd_weighted", std::to_string(bddWeighted));
    debugger.addInfo("inc_reord_trigger_score", std::to_string(weightedMax));
    return weightedMax;
}

template <typename FormulaNodeRef>
static inline void maybeRunExplicitIncReorder(
        FormulaManager<FormulaNodeRef>& formulaManager, std::size_t workScore,
        std::size_t baseWorkScore = 0) {
    if (!incReorderPolicyUsesExplicit()) {
        return;
    }
    Debugger& debugger = Debugger::getInstance();
    std::size_t effectiveWorkScore = workScore;
    std::size_t accumulatedBefore = 0;
    std::size_t accumulatedAfter = 0;
    std::uintptr_t managerId = 0;
    const bool accumulatePressure = incReorderPolicy == "pressure";
    if (accumulatePressure) {
        managerId = formulaManager.getReorderManagerId();
        auto& accumulated = incReorderAccumulatedWorkScore[managerId];
        accumulatedBefore = accumulated;
        const std::size_t room = std::numeric_limits<std::size_t>::max() - accumulated;
        accumulated += workScore > room ? room : workScore;
        accumulatedAfter = accumulated;
        effectiveWorkScore = accumulated;
    }
    debugger.addInfo("inc_reord_explicit_policy", incReorderPolicy);
    debugger.addInfo("inc_reord_explicit_work_score", std::to_string(workScore));
    debugger.addInfo("inc_reord_explicit_effective_work_score", std::to_string(effectiveWorkScore));
    debugger.addInfo("inc_reord_explicit_accumulated_before", std::to_string(accumulatedBefore));
    debugger.addInfo("inc_reord_explicit_accumulated_after", std::to_string(accumulatedAfter));
    debugger.addInfo("inc_reord_explicit_threshold", std::to_string(incReorderWorkThreshold));
    const std::size_t effectiveThreshold =
            effectiveIncReorderWorkThreshold(incReorderWorkThreshold, baseWorkScore, debugger);
    if (incReorderPolicy == "pressure" && incReorderWorkThreshold == 0) {
        debugger.addInfo("inc_reord_explicit_skip_reason", "pressure_threshold_not_set");
        FormulaReorderStats skipped;
        skipped.supported = true;
        skipped.success = true;
        addIncReorderStats(debugger, "inc_reord_explicit", skipped);
        return;
    }
    if (effectiveWorkScore < effectiveThreshold) {
        FormulaReorderStats skipped;
        skipped.supported = true;
        skipped.success = true;
        addIncReorderStats(debugger, "inc_reord_explicit", skipped);
        return;
    }
    auto stats = formulaManager.explicitIncrementalReorder();
    addIncReorderStats(debugger, "inc_reord_explicit", stats);
    if (accumulatePressure && stats.attempted) {
        incReorderAccumulatedWorkScore[managerId] = 0;
        debugger.addInfo("inc_reord_explicit_accumulated_reset", "1");
    }
}

template <typename NodeRange>
static inline std::vector<NodePtr> collectSortedNodes(const NodeRange& nodes,
        bool (*order)(const NodePtr&, const NodePtr&) = stableNodeOrder) {
    std::vector<NodePtr> ordered(nodes.begin(), nodes.end());
    std::sort(ordered.begin(), ordered.end(), order);
    return ordered;
}

template <typename EdgeRange>
static inline std::vector<EdgePtr> collectSortedEdges(const EdgeRange& edges,
        bool (*order)(const EdgePtr&, const EdgePtr&) = stableEdgeOrder) {
    std::vector<EdgePtr> ordered(edges.begin(), edges.end());
    std::sort(ordered.begin(), ordered.end(), order);
    return ordered;
}

template<typename FormulaNodeRef>
static inline bool fcInputFormulaLiteral(FormulaManager<FormulaNodeRef>& formulaManager,
        const std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
        const NodePtr& node, bool negated, FormulaNodeRef& out) {
    auto it = nodeFormulas.find(node);
    if (it == nodeFormulas.end()) {
        return false;
    }
    out = negated ? formulaManager.makeNot(it->second) : it->second;
    return true;
}

struct FcProfileStats {
    std::size_t edge_processed = 0;
    std::size_t edge_requeued = 0;
    std::size_t edge_updated = 0;
    std::size_t node_recomputed = 0;
    std::size_t node_updated = 0;
    std::size_t make_and_calls = 0;
    double make_and_ms = 0.0;
    std::size_t make_or_calls = 0;
    double make_or_ms = 0.0;
    std::size_t make_condition_calls = 0;
    double make_condition_ms = 0.0;
    std::size_t input_literal_calls = 0;
    std::size_t input_literal_missing = 0;
    double input_literal_ms = 0.0;
};

struct FcHeartbeatSnapshot {
    std::size_t elapsedMs = 0;
    std::size_t totalCycles = 0;
    std::size_t completedCycles = 0;
    std::size_t currentCycleId = 0;
    std::size_t round = 0;
    std::size_t readyQueueSize = 0;
    std::size_t worklistSize = 0;
    std::size_t nodeFormulaCount = 0;
    std::size_t edgeFormulaCount = 0;
};

template<typename FormulaNodeRef>
void buildFormulasCyclewiseInternal(
    DerivationGraphViewInterface& view,
    FormulaManager<FormulaNodeRef>& formulaManager,
    std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
    std::map<EdgePtr, FormulaNodeRef>& edgeFormulas,
    const std::unordered_set<NodePtr>& seedTrueNodes = {},
    std::vector<double>* roundTimingsMs = nullptr,
    const std::function<void(const FcHeartbeatSnapshot&)>& heartbeatCallback = nullptr,
    std::size_t heartbeatIntervalMs = 5000,
    bool standaloneFull = false
) {
    const bool freshFormulaMaps = nodeFormulas.empty() && edgeFormulas.empty();
     FunctionTimer timer("Build Formulas Cyclewise using DAG + Depth");
     const bool fcProfile = fcProfileEnabled;
     using Clock = std::chrono::steady_clock;
     auto toMs = [](auto d) {
         return std::chrono::duration<double, std::milli>(d).count();
     };
     auto overallStart = Clock::now();
     FcProfileStats stats;
     const std::size_t nodeCount = view.getNodes().size();
     const std::size_t edgeCount = view.getEdges().size();
     std::size_t factNodes = 0;
     std::size_t detEdges = 0;
     std::size_t nonDetEdges = 0;
    auto preStart = Clock::now();
    WeightedBDDManager* orderedManager = nullptr;
    if constexpr (std::is_same_v<FormulaNodeRef, BddNodeRef>) {
        if (standaloneFull) {
            orderedManager = dynamic_cast<WeightedBDDManager*>(&formulaManager);
        }
    }
    const bool orderedRegistration = orderedManager != nullptr;
    std::vector<NodePtr> orderedNodes;
    std::vector<EdgePtr> orderedEdges;
    if (orderedRegistration) {
        orderedNodes = collectSortedNodes(view.getNodes(), standaloneEventNodeOrder);
        orderedEdges = collectSortedEdges(view.getEdges(), standaloneEventEdgeOrder);
    }
    setCuddPreConfigTag("full_cyclewise");
    if (orderedRegistration) {
        orderedManager->preConfigOrdered(view, orderedNodes, orderedEdges);
    } else {
        formulaManager.preConfig(view);
    }
    setCuddPreConfigTag("");
    debugger.addInfo("fc_event_registration_order", orderedRegistration ? "stable_tuples" : "existing");
    auto preConfigMs = toMs(Clock::now() - preStart);
    debugger.logMessage(Level::INFO,
            "preConfig (cache clear + var scan/create + dyn-reorder setup) took " +
                    std::to_string(preConfigMs) + " ms");

    auto depStart = Clock::now();
    auto& depGraph = view.getCycleDependencyGraph();
    auto depMs = toMs(Clock::now() - depStart);

    auto baseStart = Clock::now();
    std::map<NodePtr, FormulaNodeRef> baseNodeFormulas;
    std::map<EdgePtr, FormulaNodeRef> baseEdgeFormulas;
    size_t round = 0;
    // 1. Initialize formulas
    auto initializeNode = [&](const NodePtr& node) {
        if (seedTrueNodes.count(node)) {
            FormulaNodeRef var = formulaManager.getTrue();
            nodeFormulas[node] = var;
            baseNodeFormulas[node] = var;
        } else if (node->isFact) {
            ++factNodes;
            int idx = formulaManager.getVarIndex(*node);
            FormulaNodeRef var = (node->getProbability() == 1.0)
                ? formulaManager.getTrue()
                : formulaManager.createVar(idx, *node);
            assertProbabilityInRange(node->getProbability(), "fact init " + node->toString());
            formulaManager.setVariableWeight(idx, node->getProbability(), 1 - node->getProbability());
            nodeFormulas[node] = var;
            baseNodeFormulas[node] = var;
        }
    };
    if (orderedRegistration) {
        for (const auto& node : orderedNodes) initializeNode(node);
    } else {
        for (const auto& node : collectSortedNodes(view.getNodes())) initializeNode(node);
    }

    auto initializeEdge = [&](const EdgePtr& edge) {
        int idx = edge->isDeterministic() ? -1 : formulaManager.getVarIndex(*edge);
        if (edge->isDeterministic()) {
            ++detEdges;
        } else {
            ++nonDetEdges;
        }
        FormulaNodeRef f = edge->isDeterministic()
            ? formulaManager.getTrue()
            : formulaManager.createVar(idx, *edge);
        if (!edge->isDeterministic()) {
            assertProbabilityInRange(edge->getProbability(), "edge init " + edge->toString());
            formulaManager.setVariableWeight(idx, edge->getProbability(), 1 - edge->getProbability());
        }
        baseEdgeFormulas[edge] = f;
    };
    if (orderedRegistration) {
        for (const auto& edge : orderedEdges) initializeEdge(edge);
        // Drop registration ranges before SCC iteration; retained formulas and
        // the view already own all entities needed by compilation.
        std::vector<NodePtr>{}.swap(orderedNodes);
        std::vector<EdgePtr>{}.swap(orderedEdges);
    } else {
        for (const auto& edge : collectSortedEdges(view.getEdges())) initializeEdge(edge);
    }
    auto baseInitMs = toMs(Clock::now() - baseStart);

    // 2. Schedule SCCs
    auto cycleTotalStart = Clock::now();
    auto lastHeartbeat = overallStart;
    const std::size_t totalCycles = depGraph.nodeCycles.size();
    std::size_t completedCycles = 0;
    std::size_t singletonBatchedCycles = 0;
    std::size_t singletonBatchedEdges = 0;
    std::vector<size_t> remainingInDegrees = depGraph.inDegrees;
    std::vector<bool> visited(depGraph.nodeCycles.size(), false);
    std::queue<size_t> ready;
    auto maybeEmitHeartbeat = [&](std::size_t cycleId, std::size_t worklistSize, bool force) {
        if (!heartbeatCallback) {
            return;
        }
        const auto now = Clock::now();
        const auto elapsedSinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              now - lastHeartbeat)
                                              .count();
        if (!force && elapsedSinceLast < static_cast<long long>(heartbeatIntervalMs)) {
            return;
        }
        FcHeartbeatSnapshot snapshot;
        snapshot.elapsedMs = static_cast<std::size_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - overallStart).count());
        snapshot.totalCycles = totalCycles;
        snapshot.completedCycles = completedCycles;
        snapshot.currentCycleId = cycleId;
        snapshot.round = round;
        snapshot.readyQueueSize = ready.size();
        snapshot.worklistSize = worklistSize;
        snapshot.nodeFormulaCount = nodeFormulas.size();
        snapshot.edgeFormulaCount = edgeFormulas.size();
        heartbeatCallback(snapshot);
        lastHeartbeat = now;
    };
    for (size_t i = 0; i < remainingInDegrees.size(); ++i) {
        if (remainingInDegrees[i] == 0)
            ready.push(i);
    }
    maybeEmitHeartbeat(0, 0, true);

    while (!ready.empty()) {
        auto cycleStart = Clock::now();
        size_t cid = ready.front(); ready.pop();
        if (visited[cid]) continue;
        visited[cid] = true;

        const auto& cycleEdges = depGraph.edgeCycles[cid];
        const auto orderedCycleEdges = collectSortedEdges(cycleEdges);
        bool batched = false;
        if (freshFormulaMaps && orderedRegistration && depGraph.nodeCycles[cid].size() == 1 &&
                !orderedCycleEdges.empty()) {
            const NodePtr head = *depGraph.nodeCycles[cid].begin();
            if (!head->isFact && !seedTrueNodes.count(head)) {
                // The ready SCC has final predecessor formulas. A singleton
                // without self inputs can publish all its sources together;
                // keep maps untouched until every source has been computed.
                std::vector<FormulaNodeRef> sources;
                sources.reserve(orderedCycleEdges.size());
                bool complete = true;
                const std::size_t firstTiming = roundTimingsMs ? roundTimingsMs->size() : 0;
                for (const auto& edge : orderedCycleEdges) {
                    const auto roundStart = Clock::now();
                    ++round;
                    if ((round & 0x1ffU) == 0U) {
                        maybeEmitHeartbeat(cid, orderedCycleEdges.size() - sources.size(), false);
                    }
                    formulaManager.dumpProfilingStatistics();
                    if (fcProfile) ++stats.edge_processed;
                    const auto body = view.getInputs(edge);
                    const auto negations = view.getBodyNegations(edge);
                    std::vector<FormulaNodeRef> inputs{baseEdgeFormulas.at(edge)};
                    inputs.reserve(body.size() + 1);
                    for (std::size_t i = 0; i < body.size(); ++i) {
                        if (body[i] == head) {
                            complete = false;
                            break;
                        }
                        FormulaNodeRef lit;
                        bool available;
                        if (fcProfile) {
                            const auto litStart = Clock::now();
                            available = fcInputFormulaLiteral(
                                    formulaManager, nodeFormulas, body[i], negations[i], lit);
                            ++stats.input_literal_calls;
                            stats.input_literal_ms += toMs(Clock::now() - litStart);
                            if (!available) ++stats.input_literal_missing;
                        } else {
                            available = fcInputFormulaLiteral(
                                    formulaManager, nodeFormulas, body[i], negations[i], lit);
                        }
                        if (!available) {
                            complete = false;
                            break;
                        }
                        inputs.push_back(lit);
                    }
                    if (complete) {
                        if (inputs.size() == 1) {
                            sources.push_back(inputs[0]);
                        } else if (fcProfile) {
                            const auto andStart = Clock::now();
                            sources.push_back(formulaManager.makeAnd(inputs));
                            ++stats.make_and_calls;
                            stats.make_and_ms += toMs(Clock::now() - andStart);
                        } else {
                            sources.push_back(formulaManager.makeAnd(inputs));
                        }
                    }
                    if (roundTimingsMs) {
                        roundTimingsMs->push_back(toMs(Clock::now() - roundStart));
                    }
                    if (!complete) break;
                }
                if (complete) {
                    const auto publishStart = Clock::now();
                    FormulaNodeRef headFormula;
                    if (sources.size() == 1) {
                        headFormula = sources[0];
                    } else if (fcProfile) {
                        const auto orStart = Clock::now();
                        headFormula = formulaManager.makeOr(sources);
                        ++stats.make_or_calls;
                        stats.make_or_ms += toMs(Clock::now() - orStart);
                    } else {
                        headFormula = formulaManager.makeOr(sources);
                    }
                    for (std::size_t i = 0; i < orderedCycleEdges.size(); ++i) {
                        edgeFormulas[orderedCycleEdges[i]] = sources[i];
                    }
                    nodeFormulas[head] = headFormula;
                    if (fcProfile) {
                        stats.edge_updated += sources.size();
                        ++stats.node_recomputed;
                        ++stats.node_updated;
                    }
                    if (roundTimingsMs && roundTimingsMs->size() > firstTiming) {
                        roundTimingsMs->back() += toMs(Clock::now() - publishStart);
                    }
                    ++singletonBatchedCycles;
                    singletonBatchedEdges += sources.size();
                    batched = true;
                }
            }
        }
        std::priority_queue<PrioritizedEdge> worklist;
        std::set<EdgePtr> inWorklist;

        if (!batched) {
            for (const auto& edge : orderedCycleEdges) {
                worklist.push({edge, depGraph.edgeDepthsGlobal.at(edge), static_cast<int>(edge->getId())});
                inWorklist.insert(edge);
            }
        }
        maybeEmitHeartbeat(cid, worklist.size(), false);

        // Track repeated stalls to help diagnose infinite loops.
        std::map<EdgePtr, size_t> stallCount;
        constexpr size_t kMaxStall = 100000;  // defensive cap to avoid infinite requeue
        std::set<EdgePtr> loggedFirstStall;

        while (!worklist.empty()) {
            auto roundStart = Clock::now();
            round++;
            if ((round & 0x1ffU) == 0U) {
                maybeEmitHeartbeat(cid, worklist.size(), false);
            }
            formulaManager.dumpProfilingStatistics();

            EdgePtr edge = worklist.top().edge;
            size_t depth = worklist.top().priority;

            worklist.pop();
            inWorklist.erase(edge);
            if (fcProfile) {
                stats.edge_processed++;
            }
            FormulaNodeRef newEdgeF;
            bool allAvailable = true;
            {
                std::vector<FormulaNodeRef> inputs = { baseEdgeFormulas[edge] };
                const auto body = view.getInputs(edge);
                const auto negations = view.getBodyNegations(edge);
                for (size_t i = 0; i < body.size(); ++i) {
                    NodePtr input = body[i];
                    FormulaNodeRef lit;
                    bool ok = true;
                    if (fcProfile) {
                        auto litStart = Clock::now();
                        ok = fcInputFormulaLiteral(formulaManager, nodeFormulas, input, negations[i], lit);
                        stats.input_literal_calls++;
                        stats.input_literal_ms += toMs(Clock::now() - litStart);
                        if (!ok) {
                            stats.input_literal_missing++;
                        }
                    } else {
                        ok = fcInputFormulaLiteral(formulaManager, nodeFormulas, input, negations[i], lit);
                    }
                    if (!ok) {
                        allAvailable = false;
                        auto& sc = stallCount[edge];
                        sc++;
                        if (DerivationGraphViewInterface::isVerboseEnabled() &&
                                (loggedFirstStall.insert(edge).second || sc == 100 || sc == 1000)) {
                            std::cout << "[buildFormulasCyclewise] stall edge " << edge->toString()
                                      << " missing input formula for node " << input->toString()
                                      << " (stall #" << sc << ")" << std::endl;
                        }
                        if (sc > kMaxStall) {
                            if (DerivationGraphViewInterface::isVerboseEnabled()) {
                                std::cout << "[buildFormulasCyclewise] giving up on edge " << edge->toString()
                                          << " after " << sc << " stalls; setting formula to False to continue."
                                          << std::endl;
                            }
                            edgeFormulas[edge] = formulaManager.getFalse();
                            allAvailable = true;  // allow propagation of False to break the cycle
                        }
                        break;
                    }
                    inputs.push_back(lit);
                }

                if (!allAvailable) {
                    worklist.push({edge, depGraph.edgeDepthsGlobal.at(edge), static_cast<int>(edge->getId())});
                    inWorklist.insert(edge);
                    if (fcProfile) {
                        stats.edge_requeued++;
                    }
                    continue;
                }

                if (inputs.size() == 1) {
                    newEdgeF = inputs[0];
                } else if (fcProfile) {
                    auto andStart = Clock::now();
                    newEdgeF = formulaManager.makeAnd(inputs);
                    stats.make_and_calls++;
                    stats.make_and_ms += toMs(Clock::now() - andStart);
                } else {
                    newEdgeF = formulaManager.makeAnd(inputs);
                }
            }
            if (!formulaManager.isSame(edgeFormulas[edge], newEdgeF)) {
                edgeFormulas[edge] = newEdgeF;
                if (fcProfile) {
                    stats.edge_updated++;
                }
                NodePtr out = view.getOutput(edge);

                FormulaNodeRef newNodeF;
                bool hasNewNodeF = false;
                if (!hasNewNodeF) {
                    std::vector<FormulaNodeRef> inFs;
                    for (auto& inEdge : collectSortedEdges(view.getIncomingEdges(out))) {
                        auto it = edgeFormulas.find(inEdge);
                        if (it != edgeFormulas.end() && it->second.get()) {
                            inFs.push_back(it->second);
                        }
                    }

                    if (!inFs.empty()) {
                        if (fcProfile) {
                            stats.node_recomputed++;
                        }
                        if (inFs.size() == 1) {
                            newNodeF = inFs[0];
                        } else if (fcProfile) {
                            auto orStart = Clock::now();
                            newNodeF = formulaManager.makeOr(inFs);
                            stats.make_or_calls++;
                            stats.make_or_ms += toMs(Clock::now() - orStart);
                        } else {
                            newNodeF = formulaManager.makeOr(inFs);
                        }
                        hasNewNodeF = true;
                    }
                }

                if (hasNewNodeF) {
                    if (!nodeFormulas.count(out) || !formulaManager.isSame(nodeFormulas[out], newNodeF)) {
                        nodeFormulas[out] = newNodeF;
                        if (fcProfile) {
                            stats.node_updated++;
                        }

                        for (auto& outEdge : collectSortedEdges(view.getOutgoingEdges(out))) {
                            auto it = depGraph.edgeToCycleIndex.find(outEdge);
                            if (it != depGraph.edgeToCycleIndex.end() && it->second == cid && !inWorklist.count(outEdge)) {
                                worklist.push({outEdge, depGraph.edgeDepthsGlobal.at(outEdge),
                                        static_cast<int>(outEdge->getId())});
                                inWorklist.insert(outEdge);
                            }
                        }
                    }
                }
            }

            if (roundTimingsMs != nullptr) {
                auto roundEnd = Clock::now();
                double roundMs = std::chrono::duration<double, std::milli>(roundEnd - roundStart).count();
                roundTimingsMs->push_back(roundMs);
            }
        }

        for (auto succ : depGraph.reverseDependencies[cid]) {
            if (--remainingInDegrees[succ] == 0) {
                ready.push(succ);
            }
        }
        completedCycles++;
        maybeEmitHeartbeat(cid, 0, false);

        (void)cycleStart;  // silence unused warning if roundTimingsMs is null
    }
    maybeEmitHeartbeat(totalCycles == 0 ? 0 : totalCycles - 1, 0, true);
    auto cycleMs = toMs(Clock::now() - cycleTotalStart);

    auto end = Clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - baseStart).count();
    formulaManager.dumpProfilingStatistics();
    for (auto& [key, value]: formulaManager.getProfilingStatistics()) {
        debugger.addInfo(key, value);
    }
    debugger.logMessage(Level::INFO, "Total rounds: " + std::to_string(round));
    debugger.logMessage(Level::INFO, "Insertion time: " + std::to_string(duration) + " ms");
    double overallMs = toMs(Clock::now() - overallStart);
    debugger.addInfo("fc_lite_mode", "full");
    debugger.addInfo("fc_lite_total_ms", std::to_string(overallMs));
    debugger.addInfo("fc_lite_preconfig_ms", std::to_string(preConfigMs));
    debugger.addInfo("fc_lite_dep_graph_ms", std::to_string(depMs));
    debugger.addInfo("fc_lite_base_init_ms", std::to_string(baseInitMs));
    debugger.addInfo("fc_lite_cycles_ms", std::to_string(cycleMs));
    debugger.addInfo("fc_lite_rounds", std::to_string(round));
    debugger.addInfo("fc_lite_nodes", std::to_string(nodeCount));
    debugger.addInfo("fc_lite_edges", std::to_string(edgeCount));
    debugger.addInfo("fc_lite_fact_nodes", std::to_string(factNodes));
    debugger.addInfo("fc_lite_det_edges", std::to_string(detEdges));
    debugger.addInfo("fc_lite_nondet_edges", std::to_string(nonDetEdges));
    debugger.addInfo("fc_lite_node_formulas", std::to_string(nodeFormulas.size()));
    debugger.addInfo("fc_lite_edge_formulas", std::to_string(edgeFormulas.size()));
    debugger.addInfo("fc_singleton_batched_cycles", std::to_string(singletonBatchedCycles));
    debugger.addInfo("fc_singleton_batched_edges", std::to_string(singletonBatchedEdges));
    if (DerivationGraphViewInterface::isVerboseEnabled()) {
        std::cout << "[buildFormulasCyclewise] timings(ms): total=" << overallMs
                  << " preConfig=" << preConfigMs
                  << " depGraph=" << depMs
                  << " baseInit=" << baseInitMs
                  << " cycles=" << cycleMs
                  << " rounds=" << round
                  << std::endl;
    }
    if (fcProfile) {
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_FULL total_ms=" << overallMs
                  << " preConfig_ms=" << preConfigMs
                  << " depGraph_ms=" << depMs
                  << " baseInit_ms=" << baseInitMs
                  << " cycles_ms=" << cycleMs
                  << " rounds=" << round
                  << " nodes=" << nodeCount
                  << " edges=" << edgeCount
                  << " fact_nodes=" << factNodes
                  << " det_edges=" << detEdges
                  << " nondet_edges=" << nonDetEdges
                  << " edge_processed=" << stats.edge_processed
                  << " edge_requeued=" << stats.edge_requeued
                  << " edge_updated=" << stats.edge_updated
                  << " node_recomputed=" << stats.node_recomputed
                  << " node_updated=" << stats.node_updated
                  << " make_and_calls=" << stats.make_and_calls
                  << " make_and_ms=" << stats.make_and_ms
                  << " make_or_calls=" << stats.make_or_calls
                  << " make_or_ms=" << stats.make_or_ms
                  << " make_condition_calls=" << stats.make_condition_calls
                  << " make_condition_ms=" << stats.make_condition_ms
                  << " input_literal_calls=" << stats.input_literal_calls
                  << " input_literal_missing=" << stats.input_literal_missing
                  << " input_literal_ms=" << stats.input_literal_ms
                  << std::endl;
    }
}

template<typename FormulaNodeRef>
void buildFormulasCyclewise(
    DerivationGraphViewInterface& view,
    FormulaManager<FormulaNodeRef>& formulaManager,
    std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
    std::map<EdgePtr, FormulaNodeRef>& edgeFormulas,
    const std::unordered_set<NodePtr>& seedTrueNodes = {},
    std::vector<double>* roundTimingsMs = nullptr,
    const std::function<void(const FcHeartbeatSnapshot&)>& heartbeatCallback = nullptr,
    std::size_t heartbeatIntervalMs = 5000
) {
    buildFormulasCyclewiseInternal(view, formulaManager, nodeFormulas, edgeFormulas, seedTrueNodes,
            roundTimingsMs, heartbeatCallback, heartbeatIntervalMs);
}

// Standalone full execution opts into canonical CUDD event registration;
// online initialization, recomputation and deltas retain the ordinary wrapper.
template<typename FormulaNodeRef>
void buildFormulasCyclewiseStandaloneFull(
    DerivationGraphViewInterface& view,
    FormulaManager<FormulaNodeRef>& formulaManager,
    std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
    std::map<EdgePtr, FormulaNodeRef>& edgeFormulas,
    const std::unordered_set<NodePtr>& seedTrueNodes = {},
    std::vector<double>* roundTimingsMs = nullptr,
    const std::function<void(const FcHeartbeatSnapshot&)>& heartbeatCallback = nullptr,
    std::size_t heartbeatIntervalMs = 5000
) {
    buildFormulasCyclewiseInternal(view, formulaManager, nodeFormulas, edgeFormulas, seedTrueNodes,
            roundTimingsMs, heartbeatCallback, heartbeatIntervalMs, true);
}

struct ComponentSubgraph {
    size_t id;
    std::unordered_set<NodePtr> nodes;
    std::unordered_set<EdgePtr> edges;
};

class BorrowedComponentSubgraphView : public virtual DerivationGraphViewInterface {
public:
    explicit BorrowedComponentSubgraphView(const ComponentSubgraph& comp)
            : nodes_(&comp.nodes), edges_(&comp.edges) {}

    const std::unordered_set<NodePtr>& getNodes() const override {
        return *nodes_;
    }

    const std::unordered_set<EdgePtr>& getEdges() const override {
        return *edges_;
    }

private:
    const std::unordered_set<NodePtr>* nodes_;
    const std::unordered_set<EdgePtr>* edges_;
};

inline std::size_t countComponentRandomVars(const ComponentSubgraph& comp) {
    std::size_t count = 0;
    for (const auto& node : comp.nodes) {
        if (node->isFact && node->getProbability() != 1.0) {
            ++count;
        }
    }
    for (const auto& edge : comp.edges) {
        if (!edge->isDeterministic()) {
            ++count;
        }
    }
    return count;
}

inline std::vector<ComponentSubgraph> buildComponentSubgraphs(const DerivationGraphViewInterface& view) {
    std::vector<NodePtr> nodes;
    nodes.reserve(view.getNodes().size());
    std::unordered_map<NodePtr, std::size_t> nodeIndex;
    nodeIndex.reserve(view.getNodes().size());
    for (const auto& node : view.getNodes()) {
        if (!node) {
            continue;
        }
        nodeIndex.emplace(node, nodes.size());
        nodes.push_back(node);
    }

    std::vector<std::size_t> parent(nodes.size());
    std::vector<unsigned char> rank(nodes.size(), 0);
    for (std::size_t i = 0; i < parent.size(); ++i) {
        parent[i] = i;
    }

    auto findRoot = [&](std::size_t idx) {
        std::size_t root = idx;
        while (parent[root] != root) {
            root = parent[root];
        }
        while (parent[idx] != idx) {
            std::size_t next = parent[idx];
            parent[idx] = root;
            idx = next;
        }
        return root;
    };
    auto unite = [&](std::size_t a, std::size_t b) {
        std::size_t ra = findRoot(a);
        std::size_t rb = findRoot(b);
        if (ra == rb) {
            return;
        }
        if (rank[ra] < rank[rb]) {
            std::swap(ra, rb);
        }
        parent[rb] = ra;
        if (rank[ra] == rank[rb]) {
            ++rank[ra];
        }
    };

    for (const auto& edge : view.getEdges()) {
        if (!edge) {
            continue;
        }
        NodePtr out = view.getOutput(edge);
        auto outIt = nodeIndex.find(out);
        if (outIt == nodeIndex.end()) {
            continue;
        }
        const std::size_t outIdx = outIt->second;
        for (const auto& input : view.getInputs(edge)) {
            auto inIt = nodeIndex.find(input);
            if (inIt != nodeIndex.end()) {
                unite(outIdx, inIt->second);
            }
        }
    }

    std::unordered_map<std::size_t, std::size_t> rootToComponent;
    rootToComponent.reserve(nodes.size());
    std::vector<ComponentSubgraph> components;
    auto componentForRoot = [&](std::size_t root) -> ComponentSubgraph& {
        auto [it, inserted] = rootToComponent.emplace(root, components.size());
        if (inserted) {
            components.push_back(ComponentSubgraph{it->second, {}, {}});
        }
        return components[it->second];
    };

    for (std::size_t idx = 0; idx < nodes.size(); ++idx) {
        auto& component = componentForRoot(findRoot(idx));
        component.nodes.insert(nodes[idx]);
    }
    for (const auto& edge : view.getEdges()) {
        if (!edge) {
            continue;
        }
        NodePtr out = view.getOutput(edge);
        auto outIt = nodeIndex.find(out);
        if (outIt == nodeIndex.end()) {
            continue;
        }
        auto& component = componentForRoot(findRoot(outIt->second));
        component.edges.insert(edge);
    }
    return components;
}

struct SingleRandVarInfo {
    NodePtr node;
    EdgePtr edge;
    double probability = 1.0;
};

inline bool findSingleRandVar(const ComponentSubgraph& comp, SingleRandVarInfo& out) {
    std::size_t count = 0;
    out = SingleRandVarInfo{};

    for (const auto& node : comp.nodes) {
        if (node->isFact && node->getProbability() != 1.0) {
            ++count;
            if (count > 1) {
                return false;
            }
            out.node = node;
            out.edge.reset();
            out.probability = node->getProbability();
        }
    }
    for (const auto& edge : comp.edges) {
        if (!edge->isDeterministic()) {
            ++count;
            if (count > 1) {
                return false;
            }
            out.node.reset();
            out.edge = edge;
            out.probability = edge->getProbability();
        }
    }
    return count == 1;
}

struct BoolNodeRef {
    bool value = false;
    bool valid = false;
    void* get() const { return valid ? const_cast<BoolNodeRef*>(this) : nullptr; }
};

struct BoolPairNodeRef {
    bool valueFalse = false;
    bool valueTrue = false;
    bool valid = false;
    void* get() const { return valid ? const_cast<BoolPairNodeRef*>(this) : nullptr; }
};

class BoolFormulaManager final : public FormulaManager<BoolNodeRef> {
public:
    BoolFormulaManager(NodePtr targetNode, EdgePtr targetEdge, bool varValue)
            : targetNode(std::move(targetNode)), targetEdge(std::move(targetEdge)), varValue(varValue) {}

    BoolNodeRef createVar(int) override {
        return markVar(nullptr, nullptr);
    }
    BoolNodeRef createVar(int, const Node& node) override {
        return markVar(&node, nullptr);
    }
    BoolNodeRef createVar(int, const Hyperedge& edge) override {
        return markVar(nullptr, &edge);
    }

    BoolNodeRef makeAnd(const BoolNodeRef& a, const BoolNodeRef& b) override {
        return BoolNodeRef{a.value && b.value, true};
    }
    BoolNodeRef makeAnd(const std::vector<BoolNodeRef>& nodes) override {
        bool value = true;
        for (const auto& node : nodes) {
            value = value && node.value;
            if (!value) break;
        }
        return BoolNodeRef{value, true};
    }
    BoolNodeRef makeOr(const BoolNodeRef& a, const BoolNodeRef& b) override {
        return BoolNodeRef{a.value || b.value, true};
    }
    BoolNodeRef makeOr(const std::vector<BoolNodeRef>& nodes) override {
        bool value = false;
        for (const auto& node : nodes) {
            value = value || node.value;
            if (value) break;
        }
        return BoolNodeRef{value, true};
    }
    BoolNodeRef makeNot(const BoolNodeRef& a) override {
        return BoolNodeRef{!a.value, true};
    }
    BoolNodeRef makeCondition(const BoolNodeRef& f, const std::vector<int>&,
            const std::vector<int>&) override {
        return f;
    }
    BoolNodeRef getTrue() override {
        return BoolNodeRef{true, true};
    }
    BoolNodeRef getFalse() override {
        return BoolNodeRef{false, true};
    }
    bool isSame(const BoolNodeRef& a, const BoolNodeRef& b) override {
        return a.valid == b.valid && a.value == b.value;
    }
    std::string toString(const BoolNodeRef& node) override {
        return node.value ? "true" : "false";
    }
    void setVariableWeight(int, double, double) override {}
    double computeWeightedModelCount(const BoolNodeRef& node) override {
        return node.value ? 1.0 : 0.0;
    }
    int getVarIndex(const Node&) override { return 0; }
    int getVarIndex(const Hyperedge&) override { return 0; }
    void printInfo(const BoolNodeRef&, const std::string&) override {}
    void dumpProfilingStatistics() override {}

    bool isValid() const { return !invalid && sawVar; }

private:
    BoolNodeRef markVar(const Node* node, const Hyperedge* edge) {
        if (sawVar) {
            invalid = true;
            return BoolNodeRef{varValue, true};
        }
        if (node != nullptr) {
            if (!targetNode || targetNode.get() != node) {
                invalid = true;
            }
        } else if (edge != nullptr) {
            if (!targetEdge || targetEdge.get() != edge) {
                invalid = true;
            }
        } else {
            invalid = true;
        }
        sawVar = true;
        return BoolNodeRef{varValue, true};
    }

    NodePtr targetNode;
    EdgePtr targetEdge;
    bool varValue = false;
    bool sawVar = false;
    bool invalid = false;
};

class BoolPairFormulaManager final : public FormulaManager<BoolPairNodeRef> {
public:
    BoolPairFormulaManager(NodePtr targetNode, EdgePtr targetEdge)
            : targetNode(std::move(targetNode)), targetEdge(std::move(targetEdge)) {}

    BoolPairNodeRef createVar(int) override {
        return markVar(nullptr, nullptr);
    }
    BoolPairNodeRef createVar(int, const Node& node) override {
        return markVar(&node, nullptr);
    }
    BoolPairNodeRef createVar(int, const Hyperedge& edge) override {
        return markVar(nullptr, &edge);
    }

    BoolPairNodeRef makeAnd(const BoolPairNodeRef& a, const BoolPairNodeRef& b) override {
        return BoolPairNodeRef{a.valueFalse && b.valueFalse, a.valueTrue && b.valueTrue,
                a.valid && b.valid};
    }
    BoolPairNodeRef makeAnd(const std::vector<BoolPairNodeRef>& nodes) override {
        bool valueFalse = true;
        bool valueTrue = true;
        bool valid = true;
        for (const auto& node : nodes) {
            valueFalse = valueFalse && node.valueFalse;
            valueTrue = valueTrue && node.valueTrue;
            valid = valid && node.valid;
            if (!valueFalse && !valueTrue) break;
        }
        return BoolPairNodeRef{valueFalse, valueTrue, valid};
    }
    BoolPairNodeRef makeOr(const BoolPairNodeRef& a, const BoolPairNodeRef& b) override {
        return BoolPairNodeRef{a.valueFalse || b.valueFalse, a.valueTrue || b.valueTrue,
                a.valid && b.valid};
    }
    BoolPairNodeRef makeOr(const std::vector<BoolPairNodeRef>& nodes) override {
        bool valueFalse = false;
        bool valueTrue = false;
        bool valid = true;
        for (const auto& node : nodes) {
            valueFalse = valueFalse || node.valueFalse;
            valueTrue = valueTrue || node.valueTrue;
            valid = valid && node.valid;
            if (valueFalse && valueTrue) break;
        }
        return BoolPairNodeRef{valueFalse, valueTrue, valid};
    }
    BoolPairNodeRef makeNot(const BoolPairNodeRef& a) override {
        return BoolPairNodeRef{!a.valueFalse, !a.valueTrue, a.valid};
    }
    BoolPairNodeRef makeCondition(const BoolPairNodeRef& f, const std::vector<int>&,
            const std::vector<int>&) override {
        return f;
    }
    BoolPairNodeRef getTrue() override {
        return BoolPairNodeRef{true, true, true};
    }
    BoolPairNodeRef getFalse() override {
        return BoolPairNodeRef{false, false, true};
    }
    bool isSame(const BoolPairNodeRef& a, const BoolPairNodeRef& b) override {
        return a.valid == b.valid && a.valueFalse == b.valueFalse && a.valueTrue == b.valueTrue;
    }
    std::string toString(const BoolPairNodeRef& node) override {
        return std::string("pair(") + (node.valueFalse ? "1" : "0") + "," +
                (node.valueTrue ? "1" : "0") + ")";
    }
    void setVariableWeight(int, double, double) override {}
    double computeWeightedModelCount(const BoolPairNodeRef& node) override {
        return node.valueTrue ? 1.0 : 0.0;
    }
    int getVarIndex(const Node&) override { return 0; }
    int getVarIndex(const Hyperedge&) override { return 0; }
    void printInfo(const BoolPairNodeRef&, const std::string&) override {}
    void dumpProfilingStatistics() override {}

    bool isValid() const { return !invalid && sawVar; }

private:
    BoolPairNodeRef markVar(const Node* node, const Hyperedge* edge) {
        if (sawVar) {
            invalid = true;
            return BoolPairNodeRef{false, true, true};
        }
        if (node != nullptr) {
            if (!targetNode || targetNode.get() != node) {
                invalid = true;
            }
        } else if (edge != nullptr) {
            if (!targetEdge || targetEdge.get() != edge) {
                invalid = true;
            }
        } else {
            invalid = true;
        }
        sawVar = true;
        return BoolPairNodeRef{false, true, true};
    }

    NodePtr targetNode;
    EdgePtr targetEdge;
    bool sawVar = false;
    bool invalid = false;
};

struct ConjNodeRef {
    std::vector<int> vars;
    bool valid = true;
    bool isFalse = false;
    void* get() const { return valid ? const_cast<ConjNodeRef*>(this) : nullptr; }
};

class ConjFormulaManager final : public FormulaManager<ConjNodeRef> {
public:
    ConjNodeRef createVar(int idx) override {
        registerVar(idx, 1.0);
        return makeVar(idx);
    }
    ConjNodeRef createVar(int idx, const Node& node) override {
        registerVar(idx, node.getProbability());
        return makeVar(idx);
    }
    ConjNodeRef createVar(int idx, const Hyperedge& edge) override {
        registerVar(idx, edge.getProbability());
        return makeVar(idx);
    }

    ConjNodeRef makeAnd(const ConjNodeRef& a, const ConjNodeRef& b) override {
        if (!a.valid || !b.valid) return invalidRef();
        if (a.isFalse || b.isFalse) return getFalse();
        return ConjNodeRef{mergeVars(a.vars, b.vars), true, false};
    }
    ConjNodeRef makeAnd(const std::vector<ConjNodeRef>& nodes) override {
        std::vector<int> vars;
        for (const auto& node : nodes) {
            if (!node.valid) return invalidRef();
            if (node.isFalse) return getFalse();
            vars = mergeVars(vars, node.vars);
        }
        return ConjNodeRef{std::move(vars), true, false};
    }
    ConjNodeRef makeOr(const ConjNodeRef& a, const ConjNodeRef& b) override {
        invalid = true;
        return invalidRef();
    }
    ConjNodeRef makeOr(const std::vector<ConjNodeRef>& nodes) override {
        if (nodes.size() == 1) {
            return nodes[0];
        }
        invalid = true;
        return invalidRef();
    }
    ConjNodeRef makeNot(const ConjNodeRef& a) override {
        invalid = true;
        return invalidRef();
    }
    ConjNodeRef makeCondition(const ConjNodeRef& f, const std::vector<int>&,
            const std::vector<int>&) override {
        return f;
    }
    ConjNodeRef getTrue() override {
        return ConjNodeRef{{}, true, false};
    }
    ConjNodeRef getFalse() override {
        return ConjNodeRef{{}, true, true};
    }
    bool isSame(const ConjNodeRef& a, const ConjNodeRef& b) override {
        return a.valid == b.valid && a.isFalse == b.isFalse && a.vars == b.vars;
    }
    std::string toString(const ConjNodeRef& node) override {
        if (!node.valid) return "invalid";
        if (node.isFalse) return "false";
        if (node.vars.empty()) return "true";
        return "conj(" + std::to_string(node.vars.size()) + ")";
    }
    void setVariableWeight(int idx, double posWeight, double) override {
        registerVar(idx, posWeight);
    }
    double computeWeightedModelCount(const ConjNodeRef& node) override {
        if (!node.valid) return 0.0;
        if (node.isFalse) return 0.0;
        double prob = 1.0;
        for (int var : node.vars) {
            if (var < 0 || static_cast<size_t>(var) >= varProb_.size()) {
                return 0.0;
            }
            prob *= varProb_[static_cast<size_t>(var)];
        }
        return prob;
    }
    int getVarIndex(const Node& node) override {
        auto it = nodeIndex_.find(&node);
        if (it != nodeIndex_.end()) return it->second;
        int idx = nextVarIndex_++;
        nodeIndex_[&node] = idx;
        registerVar(idx, node.getProbability());
        return idx;
    }
    int getVarIndex(const Hyperedge& edge) override {
        auto it = edgeIndex_.find(&edge);
        if (it != edgeIndex_.end()) return it->second;
        int idx = nextVarIndex_++;
        edgeIndex_[&edge] = idx;
        registerVar(idx, edge.getProbability());
        return idx;
    }
    void printInfo(const ConjNodeRef&, const std::string&) override {}
    void dumpProfilingStatistics() override {}

    bool isValid() const { return !invalid; }

private:
    ConjNodeRef makeVar(int idx) {
        return ConjNodeRef{{idx}, true, false};
    }
    ConjNodeRef invalidRef() {
        return ConjNodeRef{{}, false, false};
    }
    void registerVar(int idx, double prob) {
        if (idx < 0) return;
        if (static_cast<size_t>(idx) >= varProb_.size()) {
            varProb_.resize(static_cast<size_t>(idx) + 1, 1.0);
        }
        varProb_[static_cast<size_t>(idx)] = prob;
    }
    static std::vector<int> mergeVars(const std::vector<int>& a, const std::vector<int>& b) {
        if (a.empty()) return b;
        if (b.empty()) return a;
        std::vector<int> out;
        out.reserve(a.size() + b.size());
        size_t i = 0;
        size_t j = 0;
        while (i < a.size() || j < b.size()) {
            int va = (i < a.size()) ? a[i] : std::numeric_limits<int>::max();
            int vb = (j < b.size()) ? b[j] : std::numeric_limits<int>::max();
            if (va == vb) {
                out.push_back(va);
                ++i;
                ++j;
            } else if (va < vb) {
                out.push_back(va);
                ++i;
            } else {
                out.push_back(vb);
                ++j;
            }
        }
        return out;
    }

    std::vector<double> varProb_;
    int nextVarIndex_ = 0;
    std::unordered_map<const Node*, int> nodeIndex_;
    std::unordered_map<const Hyperedge*, int> edgeIndex_;
    bool invalid = false;
};

inline bool evaluateSingleRandComponent(
        const ComponentSubgraph& comp,
        const SingleRandVarInfo& var,
        bool varValue,
        std::unordered_map<NodePtr, bool>& nodeValues,
        double* evalMs = nullptr) {
    BoolFormulaManager manager(var.node, var.edge, varValue);
    BorrowedComponentSubgraphView subview(comp);
    std::map<NodePtr, BoolNodeRef> nodeFormulas;
    std::map<EdgePtr, BoolNodeRef> edgeFormulas;
    auto start = std::chrono::steady_clock::now();
    buildFormulasCyclewise(subview, manager, nodeFormulas, edgeFormulas);
    auto end = std::chrono::steady_clock::now();
    if (evalMs) {
        *evalMs = std::chrono::duration<double, std::milli>(end - start).count();
    }
    if (!manager.isValid()) {
        return false;
    }
    nodeValues.clear();
    nodeValues.reserve(comp.nodes.size());
    for (const auto& node : comp.nodes) {
        auto it = nodeFormulas.find(node);
        if (it == nodeFormulas.end() || !it->second.get()) {
            continue;
        }
        nodeValues.emplace(node, it->second.value);
    }
    return true;
}

inline bool evaluateSingleRandComponentBoth(
        const ComponentSubgraph& comp,
        const SingleRandVarInfo& var,
        std::unordered_map<NodePtr, bool>& valuesFalse,
        std::unordered_map<NodePtr, bool>& valuesTrue,
        double* evalMs = nullptr) {
    BoolPairFormulaManager manager(var.node, var.edge);
    BorrowedComponentSubgraphView subview(comp);
    std::map<NodePtr, BoolPairNodeRef> nodeFormulas;
    std::map<EdgePtr, BoolPairNodeRef> edgeFormulas;
    auto start = std::chrono::steady_clock::now();
    buildFormulasCyclewise(subview, manager, nodeFormulas, edgeFormulas);
    auto end = std::chrono::steady_clock::now();
    if (evalMs) {
        *evalMs = std::chrono::duration<double, std::milli>(end - start).count();
    }
    if (!manager.isValid()) {
        return false;
    }
    valuesFalse.clear();
    valuesTrue.clear();
    valuesFalse.reserve(comp.nodes.size());
    valuesTrue.reserve(comp.nodes.size());
    for (const auto& node : comp.nodes) {
        auto it = nodeFormulas.find(node);
        if (it == nodeFormulas.end() || !it->second.get()) {
            continue;
        }
        valuesFalse.emplace(node, it->second.valueFalse);
        valuesTrue.emplace(node, it->second.valueTrue);
    }
    return true;
}

inline bool evaluateConjComponent(
        const ComponentSubgraph& comp,
        std::unordered_map<NodePtr, double>& nodeProbs,
        double* evalMs = nullptr) {
    ConjFormulaManager manager;
    BorrowedComponentSubgraphView subview(comp);
    std::map<NodePtr, ConjNodeRef> nodeFormulas;
    std::map<EdgePtr, ConjNodeRef> edgeFormulas;
    auto start = std::chrono::steady_clock::now();
    buildFormulasCyclewise(subview, manager, nodeFormulas, edgeFormulas);
    auto end = std::chrono::steady_clock::now();
    if (evalMs) {
        *evalMs = std::chrono::duration<double, std::milli>(end - start).count();
    }
    if (!manager.isValid()) {
        return false;
    }
    nodeProbs.clear();
    nodeProbs.reserve(comp.nodes.size());
    for (const auto& node : comp.nodes) {
        auto it = nodeFormulas.find(node);
        if (it == nodeFormulas.end() || !it->second.get()) {
            continue;
        }
        double prob = manager.computeWeightedModelCount(it->second);
        nodeProbs.emplace(node, prob);
    }
    return true;
}

template <typename ManagerT, typename FormulaRef>
struct ComponentFormulaBundle {
    size_t id;
    std::unique_ptr<ManagerT> manager;
    std::map<NodePtr, FormulaRef> nodeFormulas;
};

template <typename ManagerT, typename FormulaRef, typename ManagerFactory>
inline std::vector<ComponentFormulaBundle<ManagerT, FormulaRef>>
buildFormulasCyclewiseByComponentList(
        std::vector<ComponentSubgraph> components,
        ManagerFactory&& makeManager,
        long long* initMsTotal = nullptr,
        long long* initMsMax = nullptr) {
    std::vector<ComponentFormulaBundle<ManagerT, FormulaRef>> bundles;
    bundles.reserve(components.size());
    if (initMsTotal) *initMsTotal = 0;
    if (initMsMax) *initMsMax = 0;

    for (auto& comp : components) {
        auto compId = comp.id;
        auto nodeCount = comp.nodes.size();
        auto edgeCount = comp.edges.size();
        auto randVars = countComponentRandomVars(comp);

        SubgraphView subview(std::move(comp.nodes), std::move(comp.edges));
        auto initStart = std::chrono::steady_clock::now();
        auto manager = makeManager(subview);
        long long initMs = static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - initStart)
                        .count());
        if (initMsTotal) *initMsTotal += initMs;
        if (initMsMax) *initMsMax = std::max(*initMsMax, initMs);

        auto buildStart = std::chrono::steady_clock::now();
        std::map<NodePtr, FormulaRef> nodeFormulas;
        std::map<EdgePtr, FormulaRef> edgeFormulas;
        buildFormulasCyclewise(subview, *manager, nodeFormulas, edgeFormulas);
        auto buildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - buildStart)
                               .count();

        if (fcProfileEnabled) {
            std::cout << "[fc-component] id=" << compId
                      << " nodes=" << nodeCount
                      << " edges=" << edgeCount
                      << " rand_vars=" << randVars
                      << " init_ms=" << initMs
                      << " build_ms=" << buildMs
                      << " total_ms=" << (initMs + buildMs)
                      << std::endl;
        }

        bundles.push_back(ComponentFormulaBundle<ManagerT, FormulaRef>{
                compId, std::move(manager), std::move(nodeFormulas)});
    }
    return bundles;
}

template <typename ManagerT, typename FormulaRef, typename ManagerFactory>
inline std::vector<ComponentFormulaBundle<ManagerT, FormulaRef>>
buildFormulasCyclewiseByComponent(
        const DerivationGraphViewInterface& view,
        ManagerFactory&& makeManager,
        long long* initMsTotal = nullptr,
        long long* initMsMax = nullptr) {
    return buildFormulasCyclewiseByComponentList<ManagerT, FormulaRef>(
            buildComponentSubgraphs(view), std::forward<ManagerFactory>(makeManager), initMsTotal,
            initMsMax);
}

template<typename FormulaNodeRef>
void buildFormulasIncCyclewise(
    IncrementalDerivationGraphViewInterface& view,
    FormulaManager<FormulaNodeRef>& formulaManager,
    std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
    std::map<EdgePtr, FormulaNodeRef>& edgeFormulas,
    std::set<NodePtr>& changedNodes
) {
    using namespace std::chrono;
    static int turn = 1;
    const bool incProfile = incProfileEnabled;
    const bool fcProfile = fcProfileEnabled;
    const bool deleteProfile = incDeleteProfileEnabled || fcProfileEnabled;
    using Clock = std::chrono::steady_clock;
    auto toMs = [](Clock::time_point t0, Clock::time_point t1) {
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    };
    FcProfileStats deleteCondStats;
    FcProfileStats rederiveStats;
    FcProfileStats insertStats;
    double deleteCondMs = 0.0;
    double deleteOverdeleteMs = 0.0;
    double deleteVarOrderMs = 0.0;
    double deleteVarCollectMs = 0.0;
    double deleteVarDumpMs = 0.0;
    double deleteFactsCollectMs = 0.0;
    double deleteWeightUpdateMs = 0.0;
    double deleteEraseDeltaNodesMs = 0.0;
    double deleteEraseDeltaEdgesMs = 0.0;
    double deleteCleanInvalidNodesMs = 0.0;
    double deleteCleanInvalidEdgesMs = 0.0;
    double deleteBuildDeletedOutEdgesMs = 0.0;
    double deleteImpactDetFactsMs = 0.0;
    double deleteImpactDetDeltaMs = 0.0;
    double deleteImpactNonDetFactsMs = 0.0;
    double deleteImpactDeletedEdgeSeedMs = 0.0;
    double deleteImpactDeletedEdgeClosureMs = 0.0;
    double deleteNonDetVarListMs = 0.0;
    double deleteImpactFinalizeMs = 0.0;
    double deleteImpactHashMs = 0.0;
    double deleteTotalMs = 0.0;
    double rederiveLoopMsProfile = 0.0;
    double insertPreConfigMs = 0.0;
    double insertInitNodesMs = 0.0;
    double insertInitEdgesMs = 0.0;
    double insertLoopMsProfile = 0.0;
    std::size_t deletedDetFactsCount = 0;
    std::size_t deletedNonDetFactsCount = 0;
    std::size_t detImpactNodesCount = 0;
    std::size_t detImpactEdgesCount = 0;
    std::size_t nonDetImpactNodesCount = 0;
    std::size_t nonDetImpactEdgesCount = 0;
    std::size_t nonDetOnlyNodesCount = 0;
    std::size_t nonDetOnlyEdgesCount = 0;
    std::size_t deletedVarsIndexCount = 0;
    std::size_t insertFactVars = 0;
    std::size_t insertEdgeVars = 0;
    std::size_t insertFactTrueCount = 0;
    std::size_t insertFactVarCount = 0;
    std::size_t insertNonFactCount = 0;
    std::size_t insertDetEdges = 0;
    std::size_t insertNonDetEdges = 0;
    std::size_t deleteRemovedDeltaNodeFormulas = 0;
    std::size_t deleteRemovedDeltaEdgeFormulas = 0;
    std::size_t deleteRemovedInvalidNodeFormulas = 0;
    std::size_t deleteRemovedInvalidEdgeFormulas = 0;
    std::size_t deleteWorklistEnqueueAttempts = 0;
    std::size_t deleteWorklistEnqueueInserted = 0;
    std::size_t deleteCondChangedNodes = 0;
    std::size_t deleteOverdeleteChangedNodes = 0;
    std::size_t insertChangedNodes = 0;
    std::size_t deleteCondLiveNodes = 0;
    std::size_t deleteOverdeleteLiveNodes = 0;
    std::size_t insertLiveNodes = 0;
    std::unordered_set<NodePtr> deleteCondChangedSet;
    std::unordered_set<NodePtr> deleteOverdeleteChangedSet;
    std::unordered_set<NodePtr> insertChangedSet;
    double insertInitFactTrueMs = 0.0;
    double insertInitFactVarMs = 0.0;
    double insertInitFactWeightMs = 0.0;
    double insertInitNonFactMs = 0.0;
    double insertInitEdgeSetFalseMs = 0.0;
    double insertInitEdgeVarMs = 0.0;
    double insertInitEdgeWeightMs = 0.0;
    double insertInitEdgeEnqueueMs = 0.0;
    const std::size_t viewNodeCount = view.getNodes().size();
    const std::size_t viewEdgeCount = view.getEdges().size();
    struct LocalDepGraph {
        SubgraphView view;
        CycleDependencyGraph depGraph;
        LocalDepGraph(std::unordered_set<NodePtr> nodes, std::unordered_set<EdgePtr> edges)
                : view(std::move(nodes), std::move(edges)), depGraph(view) {}
    };
    struct DeltaReachRegion {
        std::unordered_set<NodePtr> nodes;
        std::unordered_set<EdgePtr> edges;
    };
    auto deltaReachableInsert = [&](const std::set<NodePtr>& deltaInsertedNodes,
                                    const std::set<EdgePtr>& deltaInsertedEdges) -> DeltaReachRegion {
        DeltaReachRegion dr;
        const auto& reachNodes = view.getDeltaInsertReachableNodes();
        const auto& reachEdges = view.getDeltaInsertReachableEdges();
        if (!reachNodes.empty() || !reachEdges.empty()) {
            dr.nodes.insert(reachNodes.begin(), reachNodes.end());
            dr.edges.insert(reachEdges.begin(), reachEdges.end());
        } else {
            const auto& nodeImpacted = view.getNodeImpactedByDeltaInsert();
            const auto& edgeImpacted = view.getEdgeImpactedByDeltaInsert();
            if (!nodeImpacted.empty() || !edgeImpacted.empty()) {
                for (const auto& kv : nodeImpacted) {
                    dr.nodes.insert(kv.first);
                    dr.nodes.insert(kv.second.begin(), kv.second.end());
                }
                for (const auto& kv : edgeImpacted) {
                    dr.edges.insert(kv.second.begin(), kv.second.end());
                }
            } else {
                const auto& liveNodes = view.getNodes();
                const auto& liveEdges = view.getEdges();
                std::queue<NodePtr> q;
                auto seed = [&](const NodePtr& src) {
                    if (!src || !liveNodes.count(src)) {
                        return;
                    }
                    if (dr.nodes.insert(src).second) {
                        q.push(src);
                    }
                };
                for (const auto& n : deltaInsertedNodes) {
                    seed(n);
                }
                for (const auto& e : deltaInsertedEdges) {
                    if (auto h = view.getOutput(e)) {
                        seed(h);
                    }
                }
                while (!q.empty()) {
                    NodePtr cur = q.front();
                    q.pop();
                    for (const auto& e : cur->getOutgoingEdges()) {
                        if (!liveEdges.count(e)) {
                            continue;
                        }
                        dr.edges.insert(e);
                        NodePtr nxt = e->getOutput();
                        if (nxt && liveNodes.count(nxt) && dr.nodes.insert(nxt).second) {
                            q.push(nxt);
                        }
                    }
                }
            }
        }
        for (const auto& n : deltaInsertedNodes) {
            if (n) {
                dr.nodes.insert(n);
            }
        }
        for (const auto& e : deltaInsertedEdges) {
            if (!e) continue;
            dr.edges.insert(e);
            if (auto h = view.getOutput(e)) {
                dr.nodes.insert(h);
            }
        }
        return dr;
    };
    auto markChangedNode = [&](const NodePtr& node,
                               std::unordered_set<NodePtr>& phaseSet,
                               std::size_t& phaseCount,
                               bool profilePhase) {
        changedNodes.insert(node);
        if (phaseSet.insert(node).second && profilePhase) {
            ++phaseCount;
        }
    };
    auto makeAndProfile = [&](const std::vector<FormulaNodeRef>& inputs,
                              FcProfileStats& stats,
                              bool profilePhase) {
        if (!profilePhase) {
            return formulaManager.makeAnd(inputs);
        }
        auto andStart = Clock::now();
        auto res = formulaManager.makeAnd(inputs);
        stats.make_and_calls++;
        stats.make_and_ms += toMs(andStart, Clock::now());
        return res;
    };
    auto makeOrProfile = [&](const std::vector<FormulaNodeRef>& inputs,
                             FcProfileStats& stats,
                             bool profilePhase) {
        if (!profilePhase) {
            return formulaManager.makeOr(inputs);
        }
        auto orStart = Clock::now();
        auto res = formulaManager.makeOr(inputs);
        stats.make_or_calls++;
        stats.make_or_ms += toMs(orStart, Clock::now());
        return res;
    };
    auto makeConditionProfile = [&](const FormulaNodeRef& formula,
                                    const std::vector<int>& trueIdx,
                                    const std::vector<int>& falseIdx,
                                    FcProfileStats& stats,
                                    bool profilePhase) {
        if (!profilePhase) {
            return formulaManager.makeCondition(formula, trueIdx, falseIdx);
        }
        auto condStart = Clock::now();
        auto res = formulaManager.makeCondition(formula, trueIdx, falseIdx);
        stats.make_condition_calls++;
        stats.make_condition_ms += toMs(condStart, Clock::now());
        return res;
    };
    auto totalStart = Clock::now();
    Clock::time_point rederiveStart = totalStart;
    double depGraphMs = 0.0;
    double deletePrepMs = 0.0;
    double rederiveMs = 0.0;
    double insertPrepMs = 0.0;
    double insertLoopMs = 0.0;
    std::string depGraphScope = "full";
    size_t depGraphReachNodes = 0;
    size_t depGraphReachEdges = 0;
    const auto& deltaInsertedEdges = view.getDeltaInsertEdges();
    const auto& deltaDeletedEdges = view.getDeltaDeleteEdges();
    const auto& deltaInsertedNodes = view.getDeltaInsertNodes();
    const auto& deltaDeletedNodes = view.getDeltaDeleteNodes();
    const auto& deltaInsertFactNodes = view.getDeltaInsertFactNodes();
    debugger.logMessage(Level::INFO, "[inc-naive] delta counts: insNodes=" +
        std::to_string(deltaInsertedNodes.size()) + " insEdges=" +
        std::to_string(deltaInsertedEdges.size()) + " delNodes=" +
        std::to_string(deltaDeletedNodes.size()) + " delEdges=" +
        std::to_string(deltaDeletedEdges.size()));
    if (deltaInsertedEdges.empty() && deltaInsertedNodes.empty() &&
            deltaDeletedEdges.empty() && deltaDeletedNodes.empty()) {
        debugger.logMessage(Level::INFO, "No changes to apply, skipping incremental update");
        formulaManager.dumpProfilingStatistics();
        for (auto& [key, value]: formulaManager.getProfilingStatistics()) {
            debugger.addInfo(key, value);
        }
        if (incProfile) {
            const double totalMs = toMs(totalStart, Clock::now());
            std::cout << "[inc-profile] stage=FORWARD_COMPILATION_INC total_ms=" << totalMs
                      << " note=no_delta"
                      << std::endl;
        }
        return;
    }
    const std::size_t rawDeltaWorkScore =
            deltaInsertedNodes.size() + deltaInsertedEdges.size() +
            deltaDeletedNodes.size() + deltaDeletedEdges.size();
    maybePrepareIncReorderPolicy(formulaManager, rawDeltaWorkScore);
    auto start = high_resolution_clock::now();
    auto depStart = Clock::now();
    const bool insertOnly = deltaDeletedEdges.empty() && deltaDeletedNodes.empty();
    std::unique_ptr<LocalDepGraph> localDepGraph;
    CycleDependencyGraph* depGraphPtr = nullptr;
    if (insertOnly) {
        DeltaReachRegion dr = deltaReachableInsert(deltaInsertedNodes, deltaInsertedEdges);
        depGraphReachNodes = dr.nodes.size();
        depGraphReachEdges = dr.edges.size();
        if (!dr.edges.empty()) {
            std::unordered_set<NodePtr> nodes = dr.nodes;
            std::unordered_set<EdgePtr> edges;
            edges.reserve(dr.edges.size());
            for (const auto& e : dr.edges) {
                if (!e) continue;
                NodePtr out = view.getOutput(e);
                if (out) {
                    nodes.insert(out);
                }
                for (const auto& in : view.getInputs(e)) {
                    if (in) {
                        nodes.insert(in);
                    }
                }
                edges.insert(e);
            }
            localDepGraph = std::make_unique<LocalDepGraph>(std::move(nodes), std::move(edges));
            depGraphPtr = &localDepGraph->depGraph;
            depGraphScope = "delta-reach";
        } else {
            depGraphPtr = &view.getCycleDependencyGraph();
            depGraphScope = "full";
        }
    } else {
        depGraphPtr = &view.getCycleDependencyGraph();
        depGraphScope = "full";
    }
    auto& depGraph = *depGraphPtr;  // Includes computeSCCs, computeDependencies, computeDepths
    depGraph.dumpDot("scc" + std::to_string(turn++) + ".dot");
    depGraphMs = toMs(depStart, Clock::now());
    auto end = high_resolution_clock::now();
    debugger.logMessage(Level::INFO, "Finished building dependency graph and preparation. Time: " +
        std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
    auto inputLiteralProfile = [&](const NodePtr& node, bool neg, FormulaNodeRef& lit,
                                   FcProfileStats& stats,
                                   bool profilePhase) {
        if (!profilePhase) {
            return fcInputFormulaLiteral(formulaManager, nodeFormulas, node, neg, lit);
        }
        auto litStart = Clock::now();
        bool ok = fcInputFormulaLiteral(formulaManager, nodeFormulas, node, neg, lit);
        stats.input_literal_calls++;
        stats.input_literal_ms += toMs(litStart, Clock::now());
        if (!ok) {
            stats.input_literal_missing++;
        }
        return ok;
    };
    debugger.logMessage(Level::INFO, "Starting incremental update for deleted edges");

    start = high_resolution_clock::now();
    std::map<size_t, std::priority_queue<PrioritizedEdge> > cycleWorklists;
    std::map<size_t, std::set<EdgePtr> > cycleInWorklists; // initial worklist
//    std::set<EdgePtr> inWorklist;
    std::unordered_set<NodePtr> detImpactNodes;
    std::unordered_set<EdgePtr> detImpactEdges;
    std::unordered_set<NodePtr> nonDetImpactNodes;
    std::unordered_set<EdgePtr> nonDetImpactEdges;
    std::vector<int> deletedNonDetVars;

    debugger.logMessage(Level::INFO, "Performing deletion");
    auto deletePrepStart = Clock::now();
    {
        // for each deleted node, apply its neg to all its reachable edges and nodes
        // however, since we still want the optimizations for deterministic facts, we sperate them
        auto factsCollectStart = Clock::now();
        std::set<NodePtr> deletedFacts = view.getDeletedFacts();
        std::set<NodePtr> deletedDeterminsticFacts = view.getDeletedDeterminsticFacts();
        std::set<NodePtr> deletedNonDeterminsticFacts = view.getDeletedNonDeterministicFacts();
        deletedDetFactsCount = deletedDeterminsticFacts.size();
        deletedNonDetFactsCount = deletedNonDeterminsticFacts.size();
        deleteFactsCollectMs = toMs(factsCollectStart, Clock::now());
        auto weightUpdateStart = Clock::now();
        for (auto deletedFact: deletedFacts) {
            assertProbabilityInRange(0.0, "deleted fact weight");
            formulaManager.setVariableWeight(formulaManager.getVarIndex(*deletedFact), 0.0, 1.0);
        }
        deleteWeightUpdateMs = toMs(weightUpdateStart, Clock::now());
        auto eraseDeltaNodesStart = Clock::now();
        for (auto node : deltaDeletedNodes) {
            deleteRemovedDeltaNodeFormulas += nodeFormulas.erase(node);
            changedNodes.insert(node);
        }
        deleteEraseDeltaNodesMs = toMs(eraseDeltaNodesStart, Clock::now());

        auto eraseDeltaEdgesStart = Clock::now();
        for (auto edge: deltaDeletedEdges) {
            deleteRemovedDeltaEdgeFormulas += edgeFormulas.erase(edge);
        }
        deleteEraseDeltaEdgesMs = toMs(eraseDeltaEdgesStart, Clock::now());

        auto cleanInvalidNodesStart = Clock::now();
        for (auto it = nodeFormulas.begin(); it != nodeFormulas.end(); ) {
            if (view.getValidNodes().find(it->first) == view.getValidNodes().end()) {
                ++deleteRemovedInvalidNodeFormulas;
                it = nodeFormulas.erase(it);  // remove invalid nodes
            } else {
                ++it;  // move to the next element
            }
        }
        deleteCleanInvalidNodesMs = toMs(cleanInvalidNodesStart, Clock::now());

        auto cleanInvalidEdgesStart = Clock::now();
        for (auto it = edgeFormulas.begin(); it != edgeFormulas.end(); ) {
            if (view.getValidEdges().find(it->first) == view.getValidEdges().end()) {
                ++deleteRemovedInvalidEdgeFormulas;
                it = edgeFormulas.erase(it);  // remove invalid edges
            } else {
                ++it;  // move to the next element
            }
        }
        deleteCleanInvalidEdgesMs = toMs(cleanInvalidEdgesStart, Clock::now());
        auto deletedOutEdgesStart = Clock::now();
        const auto deletedOutEdges = buildDeletedOutEdges(deltaDeletedEdges);
        deleteBuildDeletedOutEdgesMs = toMs(deletedOutEdgesStart, Clock::now());
        auto detFactsStart = Clock::now();
        if (!deletedDeterminsticFacts.empty()) {
            std::vector<NodePtr> detSources(deletedDeterminsticFacts.begin(),
                                            deletedDeterminsticFacts.end());
            collectImpactUnionWithDeletedEdges(view, detSources, deletedOutEdges, detImpactNodes, detImpactEdges);
        }
        deleteImpactDetFactsMs = toMs(detFactsStart, Clock::now());
        // Deterministic derived deletions may not be explicit facts; include delta-deleted det nodes
        // that still participate in the current view (old view membership heuristic).
        auto detDeltaStart = Clock::now();
        std::vector<NodePtr> detDeltaDeleteSources;
        detDeltaDeleteSources.reserve(deltaDeletedNodes.size());
        if (!deltaDeletedNodes.empty()) {
            const auto& liveEdges = view.getValidEdges();
            for (const auto& node : deltaDeletedNodes) {
                if (!node || node->getProbability() != 1.0) {
                    continue;
                }
                bool inView = false;
                for (const auto& e : node->getOutgoingEdges()) {
                    if (liveEdges.count(e)) {
                        inView = true;
                        break;
                    }
                }
                if (inView) {
                    detDeltaDeleteSources.push_back(node);
                }
            }
        }
        if (!detDeltaDeleteSources.empty()) {
            collectImpactUnionWithDeletedEdges(view, detDeltaDeleteSources, deletedOutEdges,
                                               detImpactNodes, detImpactEdges);
        }
        deleteImpactDetDeltaMs = toMs(detDeltaStart, Clock::now());
        auto nonDetFactsStart = Clock::now();
        if (!deletedNonDeterminsticFacts.empty()) {
            std::vector<NodePtr> nonDetSources(deletedNonDeterminsticFacts.begin(),
                                               deletedNonDeterminsticFacts.end());
            collectImpactUnionWithDeletedEdges(view, nonDetSources, deletedOutEdges, nonDetImpactNodes, nonDetImpactEdges);
        }
        deleteImpactNonDetFactsMs = toMs(nonDetFactsStart, Clock::now());
        auto deletedEdgeSeedStart = Clock::now();
        for (auto edge : deltaDeletedEdges) {
            NodePtr out = view.getOutput(edge);
            if (!out || out->isFact) {
                continue;
            }
            detImpactNodes.insert(out);
        }
        deleteImpactDeletedEdgeSeedMs = toMs(deletedEdgeSeedStart, Clock::now());
        auto deletedEdgeClosureStart = Clock::now();
        if (!deltaDeletedEdges.empty()) {
            std::vector<NodePtr> detEdgeOutputs;
            detEdgeOutputs.reserve(deltaDeletedEdges.size());
            for (const auto& edge : deltaDeletedEdges) {
                NodePtr out = view.getOutput(edge);
                if (!out || out->isFact) {
                    continue;
                }
                detEdgeOutputs.push_back(out);
            }
            if (!detEdgeOutputs.empty()) {
                collectImpactUnionWithDeletedEdges(view, detEdgeOutputs, deletedOutEdges,
                                                   detImpactNodes, detImpactEdges);
            }
        }
        deleteImpactDeletedEdgeClosureMs = toMs(deletedEdgeClosureStart, Clock::now());
        auto nonDetVarListStart = Clock::now();
        if (!deletedNonDeterminsticFacts.empty()) {
            deletedNonDetVars.reserve(deletedNonDeterminsticFacts.size());
            for (auto node : deletedNonDeterminsticFacts) {
                deletedNonDetVars.push_back(formulaManager.getVarIndex(*node));
            }
        }
        deleteNonDetVarListMs = toMs(nonDetVarListStart, Clock::now());
        auto impactFinalizeStart = Clock::now();
        detImpactNodesCount = detImpactNodes.size();
        detImpactEdgesCount = detImpactEdges.size();
        nonDetImpactNodesCount = nonDetImpactNodes.size();
        nonDetImpactEdgesCount = nonDetImpactEdges.size();

        std::unordered_set<NodePtr> nonDetOnlyNodes;
        std::unordered_set<EdgePtr> nonDetOnlyEdges;
        for (auto node : nonDetImpactNodes) {
            if (!detImpactNodes.count(node)) {
                nonDetOnlyNodes.insert(node);
            }
        }
        for (auto edge : nonDetImpactEdges) {
            if (!detImpactEdges.count(edge)) {
                nonDetOnlyEdges.insert(edge);
            }
        }
        nonDetOnlyNodesCount = nonDetOnlyNodes.size();
        nonDetOnlyEdgesCount = nonDetOnlyEdges.size();
        deleteImpactFinalizeMs = toMs(impactFinalizeStart, Clock::now());

        auto impactHashStart = Clock::now();
        if (incProfile) {
            auto fnv1a = [](const std::string& s) {
                std::uint64_t h = 1469598103934665603ULL;
                for (unsigned char c : s) {
                    h ^= c;
                    h *= 1099511628211ULL;
                }
                return h;
            };
            auto hashNodeSet = [&](const std::unordered_set<NodePtr>& nodes) {
                std::uint64_t h = 0;
                for (const auto& n : nodes) {
                    if (!n) continue;
                    h ^= fnv1a(n->getTuple().toString());
                }
                return h;
            };
            auto hashEdgeSet = [&](const std::unordered_set<EdgePtr>& edges) {
                std::uint64_t h = 0;
                for (const auto& e : edges) {
                    if (!e) continue;
                    std::ostringstream oss;
                    auto ins = view.getInputs(e);
                    for (size_t i = 0; i < ins.size(); ++i) {
                        if (i) oss << ",";
                        oss << (ins[i] ? ins[i]->getTuple().toString() : "<null>");
                    }
                    NodePtr out = view.getOutput(e);
                    oss << "->" << (out ? out->getTuple().toString() : "<null>");
                    h ^= fnv1a(oss.str());
                }
                return h;
            };
            debugger.logMessage(Level::INFO,
                "[inc-delete] detImpactNodes=" + std::to_string(detImpactNodes.size()) +
                " hash=" + std::to_string(hashNodeSet(detImpactNodes)) +
                " detImpactEdges=" + std::to_string(detImpactEdges.size()) +
                " hash=" + std::to_string(hashEdgeSet(detImpactEdges)));
            debugger.logMessage(Level::INFO,
                "[inc-delete] nonDetImpactNodes=" + std::to_string(nonDetImpactNodes.size()) +
                " hash=" + std::to_string(hashNodeSet(nonDetImpactNodes)) +
                " nonDetImpactEdges=" + std::to_string(nonDetImpactEdges.size()) +
                " hash=" + std::to_string(hashEdgeSet(nonDetImpactEdges)));
            debugger.logMessage(Level::INFO,
                "[inc-delete] nonDetOnlyNodes=" + std::to_string(nonDetOnlyNodes.size()) +
                " hash=" + std::to_string(hashNodeSet(nonDetOnlyNodes)) +
                " nonDetOnlyEdges=" + std::to_string(nonDetOnlyEdges.size()) +
                " hash=" + std::to_string(hashEdgeSet(nonDetOnlyEdges)));
        }
        deleteImpactHashMs = toMs(impactHashStart, Clock::now());

        auto enqueueEdge = [&](EdgePtr edge) {
            ++deleteWorklistEnqueueAttempts;
            auto it = depGraph.edgeToCycleIndex.find(edge);
            if (it == depGraph.edgeToCycleIndex.end()) {
                return;
            }
            auto& worklist = cycleWorklists[it->second];
            auto& inWorklist = cycleInWorklists[it->second];
            if (inWorklist.insert(edge).second) {
                ++deleteWorklistEnqueueInserted;
                auto depthIt = depGraph.edgeDepthsGlobal.find(edge);
                const int seqId = depthIt == depGraph.edgeDepthsGlobal.end()
                    ? 0
                    : static_cast<int>(depthIt->second);
                const size_t priority = depthIt == depGraph.edgeDepthsGlobal.end()
                    ? 0
                    : depthIt->second;
                worklist.push({edge, priority, seqId});
            }
        };

        if (!deletedNonDetVars.empty() && (!nonDetOnlyNodes.empty() || !nonDetOnlyEdges.empty())) {
            start = high_resolution_clock::now();
            auto condStart = Clock::now();
            for (auto node : nonDetOnlyNodes) {
                if (deltaDeletedNodes.count(node)) {
                    continue;
                }
                if (nodeFormulas.count(node) == 0 || formulaManager.isSame(nodeFormulas[node], formulaManager.getFalse())) {
                    continue;
                }
                if (node->isFact) {
                    continue;
                }
                auto newNodeFormula = makeConditionProfile(nodeFormulas[node], {}, deletedNonDetVars, deleteCondStats, deleteProfile);
                if (!formulaManager.isSame(nodeFormulas[node], newNodeFormula)) {
                    nodeFormulas[node] = newNodeFormula;
                    markChangedNode(node, deleteCondChangedSet, deleteCondChangedNodes, deleteProfile);
                    if (deleteProfile) {
                        deleteCondStats.node_updated++;
                    }
                    for (EdgePtr outEdge : view.getOutgoingEdges(node)) {
                        enqueueEdge(outEdge);
                    }
                }
            }
            for (auto edge : nonDetOnlyEdges) {
                if (deltaDeletedEdges.count(edge)) {
                    continue;
                }
                auto it = edgeFormulas.find(edge);
                if (it == edgeFormulas.end() || !it->second.get()) {
                    continue;
                }
                NodePtr out = view.getOutput(edge);
                auto newEdgeFormula = makeConditionProfile(it->second, {}, deletedNonDetVars, deleteCondStats, deleteProfile);
                if (!formulaManager.isSame(it->second, newEdgeFormula)) {
                    edgeFormulas[edge] = newEdgeFormula;
                    enqueueEdge(edge);
                    if (deleteProfile) {
                        deleteCondStats.edge_updated++;
                    }
                }
            }
            end = high_resolution_clock::now();
            debugger.logMessage(Level::INFO, "Finished conditioning on deleted non-deterministic facts (non-det only). Time: " +
                std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
            deleteCondMs = toMs(condStart, Clock::now());
        }
        if (deleteProfile) {
            deleteCondLiveNodes = formulaManager.getLiveNodeCount();
        }

        // since we've optimized deterministic facts (which will reduce the size of formulas by a large constant factor) during full compilation,
        // we cannot simply conditioning formulas on the deleted deterministic facts since they are not in the formulas
        // we have to over-delete the formulas to False and then re-derive them
        // how to: change all impacted nodes' formulas to False first, put them into worklists, then rederive their formulas using the worklist algorithm
        start = high_resolution_clock::now();
        auto overdeleteStart = Clock::now();
        if (!detImpactNodes.empty() || !detImpactEdges.empty()) {
            for (auto node : detImpactNodes) {
                if (deltaDeletedNodes.count(node)) {
                    continue;  // skip deleted nodes
                }
                if (nodeFormulas.count(node) == 0 || formulaManager.isSame(nodeFormulas[node], formulaManager.getFalse())) {
                    continue;  // no need to update
                }
                if (node->isFact) {
                    continue;
                }
                nodeFormulas[node] = formulaManager.getFalse();
                markChangedNode(node, deleteOverdeleteChangedSet, deleteOverdeleteChangedNodes, deleteProfile);
                for (EdgePtr inEdge : view.getIncomingEdges(node)) {
                    enqueueEdge(inEdge);
                }
            }
            for (auto edge : detImpactEdges) {
                if (deltaDeletedEdges.count(edge)) {
                    continue;  // skip deleted edges
                }
                NodePtr out = view.getOutput(edge);
                if (!out || out->isFact) {
                    continue;
                }
                edgeFormulas[edge] = formulaManager.getFalse();
                enqueueEdge(edge);
            }
        }
        deleteOverdeleteMs = toMs(overdeleteStart, Clock::now());
        end = high_resolution_clock::now();
        debugger.logMessage(Level::INFO, "Finished over-deleting impacted formulas. Time: " +
            std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");

        start = high_resolution_clock::now();
        auto delVarOrderStart = Clock::now();
        // update the variable ordering for deleted non-deterministic facts
        auto delVarCollectStart = Clock::now();
        std::set<int> deletedVarsIndex;
        for (auto node: deletedNonDeterminsticFacts) {
            auto index = formulaManager.getVarIndex(*node);
            deletedVarsIndex.insert(index);
        }
        for (auto edge: deltaDeletedEdges) {
            if (edge->isDeterministic()) continue;
            auto index = formulaManager.getVarIndex(*edge);
            deletedVarsIndex.insert(index);
        }
        deletedVarsIndexCount = deletedVarsIndex.size();
        deleteVarCollectMs = toMs(delVarCollectStart, Clock::now());
        debugger.logMessage(Level::INFO, "Deletion deletedVarsIndex size: " +
            std::to_string(deletedVarsIndex.size()));

        auto dumpStart = Clock::now();
        formulaManager.dumpProfilingStatistics();
        deleteVarDumpMs += toMs(dumpStart, Clock::now());

        end = high_resolution_clock::now();
        debugger.logMessage(Level::INFO, "Finished updating variable ordering after deletion (non-deterministic). Time: " +
            std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");

        deleteVarOrderMs = toMs(delVarOrderStart, Clock::now());



        deletePrepMs = toMs(deletePrepStart, Clock::now());
        // Ensure inserted fact nodes are available during re-derivation.
        if (!deltaInsertFactNodes.empty()) {
            std::size_t preInitFacts = 0;
            for (auto node : deltaInsertFactNodes) {
                if (!node || nodeFormulas.count(node)) {
                    continue;
                }
                if (!node->isFact) {
                    continue;
                }
                const double prob = node->getProbability();
                if (prob == 1.0) {
                    nodeFormulas[node] = formulaManager.getTrue();
                } else {
                    int idx = formulaManager.getVarIndex(*node);
                    nodeFormulas[node] = formulaManager.createVar(idx, *node);
                    assertProbabilityInRange(prob, "inc preinit fact " + node->toString());
                    formulaManager.setVariableWeight(idx, prob, 1 - prob);
                }
                ++preInitFacts;
            }
            if (fcProfile && preInitFacts > 0) {
                std::cout << "[fc-preinit] phase=rederive facts=" << preInitFacts << std::endl;
            }
        }
        start = high_resolution_clock::now();
        rederiveStart = Clock::now();
        // try to rederive the formulas
        std::queue<size_t> ready;  // cycles with in-degree 0
        std::vector<bool> scheduled(depGraph.nodeCycles.size(), false);  // whether the cycle has been scheduled for insertion phase
        std::vector<size_t> inDegree = depGraph.inDegrees;
        for (size_t cid = 0; cid < depGraph.nodeCycles.size(); ++cid)
            if (inDegree[cid] == 0) {ready.push(cid); scheduled[cid] = true;}  // schedule cycles with in-degree 0

        while (!ready.empty()) {
            size_t cid = ready.front(); ready.pop();
            scheduled[cid] = true;
            auto& worklist = cycleWorklists[cid];
            int round = 0;
            int _seqId = 0;
            while (!worklist.empty()) {
                auto* iteration = debugger.startIteration();
                EdgePtr edge = worklist.top().edge;
                size_t depth = worklist.top().priority;
                worklist.pop();
                cycleInWorklists[cid].erase(edge);
                round++;
                if (deleteProfile) {
                    rederiveStats.edge_processed++;
                }
                if (fcProfile) {
                    std::cout << "[fc-step] phase=rederive cycle=" << cid
                              << " round=" << round
                              << " depth=" << depth
                              << " worklist=" << worklist.size()
                              << " edge=" << (edge ? edge->toString() : "<null>")
                              << "\n";
                }
                FormulaNodeRef newEdge;
                {
                    FormulaNodeRef baseFormula = edge->isDeterministic()
                        ? formulaManager.getTrue()
                        : formulaManager.createVar(formulaManager.getVarIndex(*edge), *edge);
                    std::vector<FormulaNodeRef> inputFormulas{baseFormula};

                    const auto& inputs = view.getInputs(edge);
                    const auto& negs = view.getBodyNegations(edge);
                    bool allAvailable = true;
                    for (size_t i = 0; i < inputs.size(); ++i) {
                        FormulaNodeRef lit;
                        if (!inputLiteralProfile(inputs[i], negs[i], lit, rederiveStats, deleteProfile)) {
                            NodePtr missing = inputs[i];
                            if (missing && nodeFormulas.count(missing) == 0 &&
                                    view.getDeltaInsertNodes().count(missing)) {
                                if (missing->isFact) {
                                    const double prob = missing->getProbability();
                                    if (prob == 1.0) {
                                        nodeFormulas[missing] = formulaManager.getTrue();
                                    } else {
                                        int idx = formulaManager.getVarIndex(*missing);
                                        nodeFormulas[missing] = formulaManager.createVar(idx, *missing);
                                        assertProbabilityInRange(prob, "inc lazy preinit fact " + missing->toString());
                                        formulaManager.setVariableWeight(idx, prob, 1 - prob);
                                    }
                                } else {
                                    nodeFormulas[missing] = formulaManager.getFalse();
                                }
                            }
                            allAvailable = false;
                            if (fcProfile) {
                                NodePtr head = view.getOutput(edge);
                                std::cout << "    [REQUEUE-MISS] input_index=" << i
                                          << " input=" << (missing ? missing->getTuple().toString() : "<null>")
                                          << " neg=" << (negs[i] ? 1 : 0)
                                          << " isFact=" << (missing && missing->isFact ? 1 : 0)
                                          << " in_nodeFormulas=" << (missing && nodeFormulas.count(missing) ? 1 : 0)
                                          << " in_delta_insert_nodes=" << (missing && deltaInsertedNodes.count(missing) ? 1 : 0)
                                          << " in_delta_insert_fact_nodes=" << (missing && deltaInsertFactNodes.count(missing) ? 1 : 0)
                                          << " in_delta_delete_nodes=" << (missing && deltaDeletedNodes.count(missing) ? 1 : 0)
                                          << " in_view_nodes=" << (missing && view.getNodes().count(missing) ? 1 : 0)
                                          << " head=" << (head ? head->getTuple().toString() : "<null>")
                                          << "\n";
                            }
                            break;
                        }
                        inputFormulas.push_back(lit);
                    }

                    if (!allAvailable) {
                        worklist.push({edge, depGraph.edgeDepthsGlobal.at(edge), _seqId++});
                        cycleInWorklists[cid].insert(edge);
                        if (deleteProfile) {
                            rederiveStats.edge_requeued++;
                        }
                        if (fcProfile) {
                            std::cout << "    [DECISION] action=requeue reason=missing_input"
                                      << " edge=" << edge->toString()
                                      << " cycle=" << cid
                                      << " worklist=" << worklist.size()
                                      << "\n";
                        }
                        debugger.endIteration();
                        continue;
                    }

                    if (inputFormulas.size() == 1) {
                        newEdge = inputFormulas[0];
                    } else {
                        newEdge = makeAndProfile(inputFormulas, rederiveStats, deleteProfile);
                    }
                }
                NodePtr output = view.getOutput(edge);
                const bool edgeSame = formulaManager.isSame(edgeFormulas[edge], newEdge);
                const bool forceNodeUpdate = output && deleteOverdeleteChangedSet.count(output);
                if (edgeSame && !forceNodeUpdate) {
                    if (fcProfile) {
                        std::cout << "    [DECISION] action=continue reason=edge_unchanged"
                                  << " edge=" << edge->toString()
                                  << " cycle=" << cid
                                  << "\n";
                    }
                    debugger.endIteration();
                    continue;
                }

                if (!edgeSame) {
                    edgeFormulas[edge] = newEdge;
                    if (deleteProfile) {
                        rederiveStats.edge_updated++;
                    }
                }

                if (!output || output->isFact) {
                    if (fcProfile) {
                        std::cout << "    [DECISION] action=continue reason=output_missing_or_fact"
                                  << " edge=" << edge->toString()
                                  << " cycle=" << cid
                                  << "\n";
                    }
                    debugger.endIteration();
                    continue;
                }

                FormulaNodeRef newNode;
                bool hasNewNode = false;
                if (!hasNewNode) {
                    std::vector<FormulaNodeRef> incoming;
                    for (EdgePtr e : view.getIncomingEdges(output)) {
                        if (edgeFormulas.count(e) && edgeFormulas[e].get()) {
                            incoming.push_back(edgeFormulas[e]);
                        }
                    }
                    if (deleteProfile) {
                        rederiveStats.node_recomputed++;
                    }
                    if (incoming.size() == 1) {
                        newNode = incoming[0];
                    } else {
                        newNode = makeOrProfile(incoming, rederiveStats, deleteProfile);
                    }
                }
                if (!formulaManager.isSame(nodeFormulas[output], newNode)) {
                    if (fcProfile) {
                        std::cout << "    [UPDATE] Node formula changed: " << output->toString() << std::endl;
                    }
                    nodeFormulas[output] = newNode;
                    if (deleteProfile) {
                        rederiveStats.node_updated++;
                    }
                    markChangedNode(output, deleteOverdeleteChangedSet, deleteOverdeleteChangedNodes, deleteProfile);
                    for (EdgePtr outEdge : view.getOutgoingEdges(output)) {
                        assert (depGraph.edgeToCycleIndex.count(outEdge));
                        auto it = depGraph.edgeToCycleIndex.find(outEdge);
                        if (it->second == cid && !cycleInWorklists[cid].count(outEdge)) {
                            cycleWorklists[cid].push({outEdge, depGraph.edgeDepthsGlobal.at(outEdge), _seqId++});
                            cycleInWorklists[cid].insert(outEdge);
                        }
                    }
                } else if (fcProfile) {
                    std::cout << "    [DECISION] action=no_change reason=node_unchanged"
                              << " node=" << output->toString()
                              << " cycle=" << cid
                              << "\n";
                }
                if (fcProfile) {
                    std::cout << "    [DONE] Edge processed\n";
                }
                debugger.endIteration();
            }

            for (size_t succ : depGraph.reverseDependencies[cid]) {
                if (--inDegree[succ] == 0 && !scheduled[succ]) {
                    ready.push(succ);
                }
            }
        }
    }
    end = high_resolution_clock::now();
    rederiveMs = toMs(rederiveStart, Clock::now());
    if (deleteProfile) {
        rederiveLoopMsProfile = toMs(rederiveStart, Clock::now());
    }
    debugger.logMessage(Level::INFO, "rederive time: " + std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
    if (deleteProfile) {
        deleteOverdeleteLiveNodes = formulaManager.getLiveNodeCount();
    }
    if (reuseVarIndexEnabled) {
        for (const auto& node : view.getDeletedFacts()) {
            formulaManager.releaseVarIndex(*node);
        }
        for (const auto& edge : deltaDeletedEdges) {
            formulaManager.releaseVarIndex(*edge);
        }
    }
    if (deleteProfile) {
        deleteTotalMs = toMs(deletePrepStart, Clock::now());
    }

    if (incDeleteProfileEnabled && (!deltaDeletedEdges.empty() || !deltaDeletedNodes.empty())) {
        std::cout << "[inc-delete-profile] section=timing"
                  << " total_ms=" << deleteTotalMs
                  << " prep_ms=" << deletePrepMs
                  << " cond_ms=" << deleteCondMs
                  << " overdelete_ms=" << deleteOverdeleteMs
                  << " varorder_ms=" << deleteVarOrderMs
                  << " rederive_ms=" << rederiveLoopMsProfile
                  << std::endl;
        std::cout << "[inc-delete-profile] section=cond"
                  << " make_condition_calls=" << deleteCondStats.make_condition_calls
                  << " make_condition_ms=" << deleteCondStats.make_condition_ms
                  << " node_updated=" << deleteCondStats.node_updated
                  << " edge_updated=" << deleteCondStats.edge_updated
                  << " delta_live_nodes=" << deleteCondChangedNodes
                  << " live_nodes=" << deleteCondLiveNodes
                  << std::endl;
        std::cout << "[inc-delete-profile] section=overdelete"
                  << " det_imp_nodes=" << detImpactNodesCount
                  << " det_imp_edges=" << detImpactEdgesCount
                  << " delta_live_nodes=" << deleteOverdeleteChangedNodes
                  << " live_nodes=" << deleteOverdeleteLiveNodes
                  << std::endl;
        std::cout << "[inc-delete-profile] section=rederive"
                  << " edge_processed=" << rederiveStats.edge_processed
                  << " edge_requeued=" << rederiveStats.edge_requeued
                  << " edge_updated=" << rederiveStats.edge_updated
                  << " node_recomputed=" << rederiveStats.node_recomputed
                  << " node_updated=" << rederiveStats.node_updated
                  << " make_and_calls=" << rederiveStats.make_and_calls
                  << " make_and_ms=" << rederiveStats.make_and_ms
                  << " make_or_calls=" << rederiveStats.make_or_calls
                  << " make_or_ms=" << rederiveStats.make_or_ms
                  << " make_condition_calls=" << rederiveStats.make_condition_calls
                  << " make_condition_ms=" << rederiveStats.make_condition_ms
                  << " input_literal_calls=" << rederiveStats.input_literal_calls
                  << " input_literal_missing=" << rederiveStats.input_literal_missing
                  << " input_literal_ms=" << rederiveStats.input_literal_ms
                  << std::endl;
    }
    // should reset variable ordering like information after deletion
    // should change to a light weight version?

    size_t round = 0;
    if (deltaInsertedNodes.empty() && deltaInsertedEdges.empty()) {
        start = high_resolution_clock::now();
        debugger.logMessage(Level::INFO, "No inserted edges, skipping insertion phase");
    } else {
        auto insertPrepStart = Clock::now();
        start = high_resolution_clock::now();
        auto insertPreConfigStart = Clock::now();
        setCuddPreConfigTag("inc_insert");
        formulaManager.preConfig(view);
        setCuddPreConfigTag("");
        end = high_resolution_clock::now();
        debugger.logMessage(Level::INFO,
                "preConfig (cache clear + var scan/create + dyn-reorder setup) took " +
                        std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
        insertPreConfigMs = toMs(insertPreConfigStart, Clock::now());
        start = high_resolution_clock::now();
        std::vector<size_t> inDegree = depGraph.inDegrees;
        std::queue<size_t> ready;  // cycles with in-degree 0
        std::vector<bool> scheduled(depGraph.nodeCycles.size(), false);  // whether the cycle has been scheduled for insertion phase
        size_t insertion_impacted_node_count = 0;

        // === Insertion phase ===
        if (DerivationGraphViewInterface::isVerboseEnabled()) {
            std::cout << "Processing inserted edges" << std::endl;
        }
        debugger.logMessage(Level::INFO, "Processing inserted edges");
        // initialized formulas for newly inserted nodes and edges
        auto initNodesStart = Clock::now();
        for (auto node : deltaInsertedNodes) {
            if (node->isFact) {
                const double prob = node->getProbability();
                if (prob == 1.0) {
                    if (fcProfile) {
                        auto t = Clock::now();
                        nodeFormulas[node] = formulaManager.getTrue();
                        insertInitFactTrueMs += toMs(t, Clock::now());
                    } else {
                        nodeFormulas[node] = formulaManager.getTrue();
                    }
                    ++insertFactTrueCount;
                } else {
                    int idx = formulaManager.getVarIndex(*node);
                    if (fcProfile) {
                        auto t = Clock::now();
                        nodeFormulas[node] = formulaManager.createVar(idx, *node);
                        insertInitFactVarMs += toMs(t, Clock::now());
                    } else {
                        nodeFormulas[node] = formulaManager.createVar(idx, *node);
                    }
                    if (fcProfile) {
                        auto t = Clock::now();
                        formulaManager.setVariableWeight(idx, prob, 1 - prob);
                        insertInitFactWeightMs += toMs(t, Clock::now());
                    } else {
                        formulaManager.setVariableWeight(idx, prob, 1 - prob);
                    }
                    ++insertFactVars;
                    ++insertFactVarCount;
                }
            } else {
                if (fcProfile) {
                    auto t = Clock::now();
                    nodeFormulas[node] = formulaManager.getFalse();
                    insertInitNonFactMs += toMs(t, Clock::now());
                } else {
                    nodeFormulas[node] = formulaManager.getFalse();
                }
                ++insertNonFactCount;
            }
            markChangedNode(node, insertChangedSet, insertChangedNodes, fcProfile);
        }
        end = high_resolution_clock::now();
        debugger.logMessage(Level::INFO, "Initialize inserted node formulas. Time: " +
            std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
        insertInitNodesMs = toMs(initNodesStart, Clock::now());

        auto initEdgesStart = Clock::now();
        for (auto edge : deltaInsertedEdges) {
            if (fcProfile) {
                auto t = Clock::now();
                edgeFormulas[edge] = formulaManager.getFalse();
                insertInitEdgeSetFalseMs += toMs(t, Clock::now());
            } else {
                edgeFormulas[edge] = formulaManager.getFalse();
            }
            if (!edge->isDeterministic()) {
                int idx = formulaManager.getVarIndex(*edge);
                if (fcProfile) {
                    auto t = Clock::now();
                    formulaManager.createVar(idx, *edge);
                    insertInitEdgeVarMs += toMs(t, Clock::now());
                } else {
                    formulaManager.createVar(idx, *edge);
                }
                assertProbabilityInRange(edge->getProbability(), "inc inserted edge " + edge->toString());
                if (fcProfile) {
                    auto t = Clock::now();
                    formulaManager.setVariableWeight(idx, edge->getProbability(), 1 - edge->getProbability());
                    insertInitEdgeWeightMs += toMs(t, Clock::now());
                } else {
                    formulaManager.setVariableWeight(idx, edge->getProbability(), 1 - edge->getProbability());
                }
                ++insertEdgeVars;
                ++insertNonDetEdges;
            } else {
                ++insertDetEdges;
            }
            assert (depGraph.edgeToCycleIndex.count(edge));
            size_t cid = depGraph.edgeToCycleIndex.at(edge);
            if (fcProfile) {
                auto t = Clock::now();
                cycleWorklists[cid].push({edge, depGraph.edgeDepthsGlobal.at(edge), 0});
                cycleInWorklists[cid].insert(edge);
                insertInitEdgeEnqueueMs += toMs(t, Clock::now());
            } else {
                cycleWorklists[cid].push({edge, depGraph.edgeDepthsGlobal.at(edge), 0});
                cycleInWorklists[cid].insert(edge);
            }
        }
        end = high_resolution_clock::now();
        debugger.logMessage(Level::INFO, "Initialize inserted edge formulas and worklists. Time: " +
            std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
        insertInitEdgesMs = toMs(initEdgesStart, Clock::now());
        inDegree = depGraph.inDegrees;
        std::fill(scheduled.begin(), scheduled.end(), false);

        assert (ready.empty());  // should be empty after deletion phase

        for (size_t cid = 0; cid < depGraph.nodeCycles.size(); ++cid)
            if (inDegree[cid] == 0) {ready.push(cid); scheduled[cid] = true;}  // schedule cycles with in-degree 0
        end = high_resolution_clock::now();
        debugger.logMessage(Level::INFO, "Preparation for insertion phase. Time: " +
            std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
        insertPrepMs = toMs(insertPrepStart, Clock::now());
        auto insertLoopStart = Clock::now();
        std::unordered_map<EdgePtr, std::size_t> missingInputLogs;
        while (!ready.empty()) {
            size_t cid = ready.front(); ready.pop();
            scheduled[cid] = true;
            auto& worklist = cycleWorklists[cid];
            auto& inWorklist = cycleInWorklists[cid];
            int _seqId = 1;
            while (!worklist.empty()) {
                EdgePtr edge = worklist.top().edge;
                size_t depth = worklist.top().priority;
                worklist.pop();
                inWorklist.erase(edge);
                round++;
                if (fcProfile) {
                    insertStats.edge_processed++;
                }
                if (fcProfile) {
                    std::cout << "  [INSERTION ROUND " << round << "] Cycle " << cid
                              << ", Worklist size: " << worklist.size() << std::endl;
                    std::cout << "[fc-step] phase=insert cycle=" << cid
                              << " round=" << round
                              << " depth=" << depth
                              << " worklist=" << worklist.size()
                              << " edge=" << (edge ? edge->toString() : "<null>")
                              << "\n";
                }
                const auto& inputs = view.getInputs(edge);
                const auto& negs = view.getBodyNegations(edge);
                FormulaNodeRef newEdge;
                bool allAvailable = true;
                {
                    FormulaNodeRef baseFormula = edge->isDeterministic()
                        ? formulaManager.getTrue()
                        : formulaManager.createVar(formulaManager.getVarIndex(*edge), *edge);
                    std::vector<FormulaNodeRef> inputFormulas{baseFormula};
                    for (size_t i = 0; i < inputs.size(); ++i) {
                        FormulaNodeRef lit;
                        if (!inputLiteralProfile(inputs[i], negs[i], lit, insertStats, fcProfile)) {
                            allAvailable = false;
                            auto& cnt = missingInputLogs[edge];
                            if (cnt < 3 && fcProfile) {
                                NodePtr missing = inputs[i];
                                std::cout << "    [REQUEUE-MISS] input="
                                          << (missing ? missing->getTuple().toString() : "<null>")
                                          << " isFact=" << (missing && missing->isFact ? 1 : 0)
                                          << " in_nodeFormulas=" << (missing && nodeFormulas.count(missing) ? 1 : 0)
                                          << " in_delta_insert_nodes=" << (missing && deltaInsertedNodes.count(missing) ? 1 : 0)
                                          << " in_view_nodes=" << (missing && view.getNodes().count(missing) ? 1 : 0)
                                          << " head=" << (view.getOutput(edge) ? view.getOutput(edge)->getTuple().toString() : "<null>")
                                          << "\n";
                            }
                            ++cnt;
                            break;
                        }
                        inputFormulas.push_back(lit);
                    }

                    if (!allAvailable) {
                        worklist.push({edge, depGraph.edgeDepthsGlobal.at(edge), _seqId++});
                        inWorklist.insert(edge);
                        if (fcProfile) {
                            insertStats.edge_requeued++;
                            std::cout << "    [DECISION] action=requeue reason=missing_input"
                                      << " edge=" << edge->toString()
                                      << " cycle=" << cid
                                      << " worklist=" << worklist.size()
                                      << "\n";
                        }
                        if (fcProfile) {
                            std::cout << "    [REQUEUE] Missing input for edge " << edge->toString()
                                      << ", back to worklist (cycle " << cid
                                      << ", new worklist size=" << worklist.size() << ")\n";
                        }
                        continue;
                    }

                    if (inputFormulas.size() == 1) {
                        newEdge = inputFormulas[0];
                    } else {
                        newEdge = makeAndProfile(inputFormulas, insertStats, fcProfile);
                    }
                }
                if (!edgeFormulas[edge].get()) {
                    if (fcProfile) {
                        std::cout << "    [EDGE-OLD-NULL] " << edge->toString() << " old formula is null\n";
                    }
                }
                if (!newEdge.get()) {
                    if (fcProfile) {
                        std::cout << "    [EDGE-NEW-NULL] " << edge->toString() << " new formula is null\n";
                    }
                }
                if (formulaManager.isSame(edgeFormulas[edge], newEdge)) {
                    if (fcProfile) {
                        std::cout << "    [SKIP] Edge formula unchanged for " << edge->toString()
                                  << " (cycle " << cid << ")\n";
                        std::cout << "    [DECISION] action=continue reason=edge_unchanged"
                                  << " edge=" << edge->toString()
                                  << " cycle=" << cid
                                  << "\n";
                    }
                    continue;
                }

                if (fcProfile) {
                    std::cout << "    [EDGE-UPDATE] Formula changed for " << edge->toString()
                              << " (cycle " << cid << ")"
                              << " var=" << formulaManager.getVarIndex(*edge);
                    std::cout << " inputs{";
                    for (size_t i = 0; i < inputs.size(); ++i) {
                        if (i) std::cout << ",";
                        std::cout << formulaManager.getVarIndex(*inputs[i]);
                    }
                    std::cout << "} newPtr=" << reinterpret_cast<const void*>(newEdge.get()) << "\n";
                }
                edgeFormulas[edge] = newEdge;
                if (fcProfile) {
                    insertStats.edge_updated++;
                }
                if (formulaManager.isSame(newEdge, formulaManager.getFalse())) {
                    if (fcProfile) {
                        std::cout << "    [EDGE-FALSE] " << edge->toString() << " became False\n";
                    }
                } else if (formulaManager.isSame(newEdge, formulaManager.getTrue())) {
                    if (fcProfile) {
                        std::cout << "    [EDGE-TRUE] " << edge->toString() << " became True\n";
                    }
                }

                NodePtr output = view.getOutput(edge);
                if (!output || output->isFact) {
                    if (fcProfile) {
                        std::cout << "    [IGNORE] Output missing or fact for " << edge->toString()
                                  << " (cycle " << cid << ")\n";
                        std::cout << "    [DECISION] action=continue reason=output_missing_or_fact"
                                  << " edge=" << edge->toString()
                                  << " cycle=" << cid
                                  << "\n";
                    }
                    continue;
                }

                FormulaNodeRef newNode;
                bool hasNewNode = false;
                if (!hasNewNode) {
                    std::vector<FormulaNodeRef> incoming;
                    for (EdgePtr e : view.getIncomingEdges(output)) {
                        if (edgeFormulas.count(e) && edgeFormulas[e].get()) {
                            incoming.push_back(edgeFormulas[e]);
                        }
                    }
                    if (fcProfile) {
                        insertStats.node_recomputed++;
                    }
                    if (incoming.size() == 1) {
                        newNode = incoming[0];
                    } else {
                        newNode = makeOrProfile(incoming, insertStats, fcProfile);
                    }
                }
                if (!nodeFormulas[output].get()) {
                    if (fcProfile) {
                        std::cout << "    [NODE-OLD-NULL] " << output->toString() << " old formula is null\n";
                    }
                }
                if (!newNode.get()) {
                    if (fcProfile) {
                        std::cout << "    [NODE-NEW-NULL] " << output->toString() << " new formula is null\n";
                    }
                }
                if (!formulaManager.isSame(nodeFormulas[output], newNode)) {
                    nodeFormulas[output] = newNode;
                    if (fcProfile) {
                        insertStats.node_updated++;
                    }
                    if (fcProfile) {
                        std::cout << "    [NODE-UPDATE] " << output->toString()
                                  << " newPtr=" << reinterpret_cast<const void*>(newNode.get()) << "\n";
                    }
                    if (formulaManager.isSame(newNode, formulaManager.getFalse())) {
                        if (fcProfile) {
                            std::cout << "    [NODE-FALSE] " << output->toString() << " became False\n";
                        }
                    } else if (formulaManager.isSame(newNode, formulaManager.getTrue())) {
                        if (fcProfile) {
                            std::cout << "    [NODE-TRUE] " << output->toString() << " became True\n";
                        }
                    }
                    markChangedNode(output, insertChangedSet, insertChangedNodes, fcProfile);
                    for (EdgePtr outEdge : view.getOutgoingEdges(output)) {
                        auto it = depGraph.edgeToCycleIndex.find(outEdge);
                        assert (depGraph.edgeToCycleIndex.count(outEdge));
                        auto outCid = it->second;
                        if (!cycleInWorklists[outCid].count(outEdge)) {
                            cycleWorklists[outCid].push({outEdge, depGraph.edgeDepthsGlobal.at(outEdge), _seqId++});
                            cycleInWorklists[outCid].insert(outEdge);
                            if (fcProfile) {
                                std::cout << "    [ENQUEUE] Node formula changed for "
                                          << output->toString()
                                          << " → enqueue outgoing edge " << outEdge->toString()
                                          << " (cycle " << outCid
                                          << ", new worklist size=" << cycleWorklists[outCid].size()
                                          << ")\n";
                            }
                        } else {
                            if (fcProfile) {
                                std::cout << "    [ENQUEUE-SKIP] Outgoing edge already queued: "
                                          << outEdge->toString()
                                          << " (cycle " << outCid
                                          << ", worklist size=" << cycleWorklists[outCid].size()
                                          << ")\n";
                            }
                        }
                    }
                } else {
                    if (fcProfile) {
                        std::cout << "    [NODE-UNCHANGED] " << output->toString()
                                  << " formula unchanged (cycle " << cid
                                  << ", outgoing edges=" << view.getOutgoingEdges(output).size()
                                  << ")\n";
                        std::cout << "    [DECISION] action=no_change reason=node_unchanged"
                                  << " node=" << output->toString()
                                  << " cycle=" << cid
                                  << "\n";
                    }
                }
            }

            for (size_t succ : depGraph.reverseDependencies[cid]) {
                if (--inDegree[succ] == 0 && !scheduled[succ]) {
                    ready.push(succ);
                }
            }
        }
        insertLoopMs = toMs(insertLoopStart, Clock::now());
        if (fcProfile) {
            insertLoopMsProfile = toMs(insertLoopStart, Clock::now());
        }
    }
    if (fcProfile) {
        insertLiveNodes = formulaManager.getLiveNodeCount();
    }
    end = high_resolution_clock::now();
    debugger.logMessage(Level::INFO, "Insertion time: " + std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
    debugger.logMessage(Level::INFO, "Iteration rounds: " + std::to_string(round));
    IncReorderPressureScore pressureScore;
    pressureScore.mode = "inc-naive";
    pressureScore.rawDelta = deltaInsertedNodes.size() + deltaInsertedEdges.size() +
            deltaDeletedNodes.size() + deltaDeletedEdges.size();
    pressureScore.frontierNodes = depGraphReachNodes;
    pressureScore.frontierEdges = depGraphReachEdges;
    pressureScore.bddEdgesProcessed = rederiveStats.edge_processed + insertStats.edge_processed;
    pressureScore.bddNodesUpdated = changedNodes.size();
    addIncReorderPressureScore(debugger, pressureScore);
    const std::size_t triggerScore = weightedMaxIncReorderScore(pressureScore, debugger);
    const std::size_t fullGraphWorkScore = view.getNodes().size() + view.getEdges().size();
    maybeRunExplicitIncReorder(formulaManager, triggerScore, fullGraphWorkScore);
    formulaManager.dumpProfilingStatistics();
    for (auto& [key, value]: formulaManager.getProfilingStatistics()) {
        debugger.addInfo(key, value);
    }
    debugger.addInfo("changed_node_count", std::to_string(changedNodes.size()));
    const double fcLiteTotalMs = toMs(totalStart, Clock::now());
    debugger.addInfo("fc_lite_mode", "inc");
    debugger.addInfo("fc_lite_total_ms", std::to_string(fcLiteTotalMs));
    debugger.addInfo("fc_lite_dep_graph_ms", std::to_string(depGraphMs));
    debugger.addInfo("fc_lite_delete_prep_ms", std::to_string(deletePrepMs));
    debugger.addInfo("fc_lite_delete_facts_collect_ms", std::to_string(deleteFactsCollectMs));
    debugger.addInfo("fc_lite_delete_weight_update_ms", std::to_string(deleteWeightUpdateMs));
    debugger.addInfo("fc_lite_delete_erase_delta_nodes_ms", std::to_string(deleteEraseDeltaNodesMs));
    debugger.addInfo("fc_lite_delete_erase_delta_edges_ms", std::to_string(deleteEraseDeltaEdgesMs));
    debugger.addInfo("fc_lite_delete_clean_invalid_nodes_ms", std::to_string(deleteCleanInvalidNodesMs));
    debugger.addInfo("fc_lite_delete_clean_invalid_edges_ms", std::to_string(deleteCleanInvalidEdgesMs));
    debugger.addInfo("fc_lite_delete_build_deleted_out_edges_ms", std::to_string(deleteBuildDeletedOutEdgesMs));
    debugger.addInfo("fc_lite_delete_impact_det_facts_ms", std::to_string(deleteImpactDetFactsMs));
    debugger.addInfo("fc_lite_delete_impact_det_delta_ms", std::to_string(deleteImpactDetDeltaMs));
    debugger.addInfo("fc_lite_delete_impact_nondet_facts_ms", std::to_string(deleteImpactNonDetFactsMs));
    debugger.addInfo("fc_lite_delete_impact_deleted_edge_seed_ms", std::to_string(deleteImpactDeletedEdgeSeedMs));
    debugger.addInfo("fc_lite_delete_impact_deleted_edge_closure_ms", std::to_string(deleteImpactDeletedEdgeClosureMs));
    debugger.addInfo("fc_lite_delete_nondet_var_list_ms", std::to_string(deleteNonDetVarListMs));
    debugger.addInfo("fc_lite_delete_impact_finalize_ms", std::to_string(deleteImpactFinalizeMs));
    debugger.addInfo("fc_lite_delete_impact_hash_ms", std::to_string(deleteImpactHashMs));
    debugger.addInfo("fc_lite_delete_condition_ms", std::to_string(deleteCondMs));
    debugger.addInfo("fc_lite_delete_overdelete_ms", std::to_string(deleteOverdeleteMs));
    debugger.addInfo("fc_lite_delete_var_order_ms", std::to_string(deleteVarOrderMs));
    debugger.addInfo("fc_lite_delete_var_collect_ms", std::to_string(deleteVarCollectMs));
    debugger.addInfo("fc_lite_delete_var_dump_ms", std::to_string(deleteVarDumpMs));
    debugger.addInfo("fc_lite_delete_removed_delta_node_formulas",
            std::to_string(deleteRemovedDeltaNodeFormulas));
    debugger.addInfo("fc_lite_delete_removed_delta_edge_formulas",
            std::to_string(deleteRemovedDeltaEdgeFormulas));
    debugger.addInfo("fc_lite_delete_removed_invalid_node_formulas",
            std::to_string(deleteRemovedInvalidNodeFormulas));
    debugger.addInfo("fc_lite_delete_removed_invalid_edge_formulas",
            std::to_string(deleteRemovedInvalidEdgeFormulas));
    debugger.addInfo("fc_lite_delete_worklist_enqueue_attempts",
            std::to_string(deleteWorklistEnqueueAttempts));
    debugger.addInfo("fc_lite_delete_worklist_enqueue_inserted",
            std::to_string(deleteWorklistEnqueueInserted));
    debugger.addInfo("fc_lite_delete_det_facts", std::to_string(deletedDetFactsCount));
    debugger.addInfo("fc_lite_delete_nondet_facts", std::to_string(deletedNonDetFactsCount));
    debugger.addInfo("fc_lite_delete_det_impact_nodes", std::to_string(detImpactNodesCount));
    debugger.addInfo("fc_lite_delete_det_impact_edges", std::to_string(detImpactEdgesCount));
    debugger.addInfo("fc_lite_delete_nondet_impact_nodes", std::to_string(nonDetImpactNodesCount));
    debugger.addInfo("fc_lite_delete_nondet_impact_edges", std::to_string(nonDetImpactEdgesCount));
    debugger.addInfo("fc_lite_delete_nondet_only_nodes", std::to_string(nonDetOnlyNodesCount));
    debugger.addInfo("fc_lite_delete_nondet_only_edges", std::to_string(nonDetOnlyEdgesCount));
    debugger.addInfo("fc_lite_delete_deleted_vars", std::to_string(deletedVarsIndexCount));
    debugger.addInfo("fc_lite_rederive_ms", std::to_string(rederiveMs));
    debugger.addInfo("fc_lite_insert_prep_ms", std::to_string(insertPrepMs));
    debugger.addInfo("fc_lite_insert_preconfig_ms", std::to_string(insertPreConfigMs));
    debugger.addInfo("fc_lite_insert_init_nodes_ms", std::to_string(insertInitNodesMs));
    debugger.addInfo("fc_lite_insert_init_edges_ms", std::to_string(insertInitEdgesMs));
    debugger.addInfo("fc_lite_insert_loop_ms", std::to_string(insertLoopMs));
    debugger.addInfo("fc_lite_rounds", std::to_string(round));
    debugger.addInfo("fc_lite_view_nodes", std::to_string(viewNodeCount));
    debugger.addInfo("fc_lite_view_edges", std::to_string(viewEdgeCount));
    debugger.addInfo("fc_lite_delta_insert_nodes", std::to_string(deltaInsertedNodes.size()));
    debugger.addInfo("fc_lite_delta_insert_edges", std::to_string(deltaInsertedEdges.size()));
    debugger.addInfo("fc_lite_delta_delete_nodes", std::to_string(deltaDeletedNodes.size()));
    debugger.addInfo("fc_lite_delta_delete_edges", std::to_string(deltaDeletedEdges.size()));
    debugger.addInfo("fc_lite_changed_nodes", std::to_string(changedNodes.size()));
    if (fcProfile) {
        debugger.addInfo("del_cond_delta_live_nodes", std::to_string(deleteCondChangedNodes));
        debugger.addInfo("del_cond_live_nodes", std::to_string(deleteCondLiveNodes));
        debugger.addInfo("del_overdelete_delta_live_nodes", std::to_string(deleteOverdeleteChangedNodes));
        debugger.addInfo("del_overdelete_live_nodes", std::to_string(deleteOverdeleteLiveNodes));
        debugger.addInfo("insert_delta_live_nodes", std::to_string(insertChangedNodes));
        debugger.addInfo("insert_live_nodes", std::to_string(insertLiveNodes));
    }
    if (incProfile) {
        const double totalMs = toMs(totalStart, Clock::now());
        std::cout << "[inc-profile] stage=FORWARD_COMPILATION_INC total_ms=" << totalMs
                  << " dep_ms=" << depGraphMs
                  << " delete_prep_ms=" << deletePrepMs
                  << " rederive_ms=" << rederiveMs
                  << " insert_prep_ms=" << insertPrepMs
                  << " insert_loop_ms=" << insertLoopMs
                  << " del_nodes=" << deltaDeletedNodes.size()
                  << " del_edges=" << deltaDeletedEdges.size()
                  << " ins_nodes=" << deltaInsertedNodes.size()
                  << " ins_edges=" << deltaInsertedEdges.size()
                  << " changed_nodes=" << changedNodes.size()
                  << std::endl;
        std::cout << "[inc-naive-profile]"
                  << " dep_graph_ms=" << depGraphMs
                  << " dep_graph_scope=" << depGraphScope
                  << " reach_nodes=" << depGraphReachNodes
                  << " reach_edges=" << depGraphReachEdges
                  << " rederive_ms=" << rederiveMs
                  << " preconfig_ms=" << insertPreConfigMs
                  << " init_nodes_ms=" << insertInitNodesMs
                  << " init_edges_ms=" << insertInitEdgesMs
                  << " prep_insert_ms=" << insertPrepMs
                  << " insert_ms=" << insertLoopMs
                  << " total_ms=" << totalMs
                  << std::endl;
    }
    if (fcProfile) {
        const double totalMs = toMs(totalStart, Clock::now());
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_INC total_ms=" << totalMs
                  << " dep_ms=" << depGraphMs
                  << " view_nodes=" << viewNodeCount
                  << " view_edges=" << viewEdgeCount
                  << " del_nodes=" << deltaDeletedNodes.size()
                  << " del_edges=" << deltaDeletedEdges.size()
                  << " ins_nodes=" << deltaInsertedNodes.size()
                  << " ins_edges=" << deltaInsertedEdges.size()
                  << " det_fact_del=" << deletedDetFactsCount
                  << " nondet_fact_del=" << deletedNonDetFactsCount
                  << " det_imp_nodes=" << detImpactNodesCount
                  << " det_imp_edges=" << detImpactEdgesCount
                  << " nondet_imp_nodes=" << nonDetImpactNodesCount
                  << " nondet_imp_edges=" << nonDetImpactEdgesCount
                  << " nondet_only_nodes=" << nonDetOnlyNodesCount
                  << " nondet_only_edges=" << nonDetOnlyEdgesCount
                  << " del_nondet_vars=" << deletedNonDetVars.size()
                  << std::endl;
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_INC phase=delete_condition ms=" << deleteCondMs
                  << " make_condition_calls=" << deleteCondStats.make_condition_calls
                  << " make_condition_ms=" << deleteCondStats.make_condition_ms
                  << " node_updated=" << deleteCondStats.node_updated
                  << " edge_updated=" << deleteCondStats.edge_updated
                  << " delta_live_nodes=" << deleteCondChangedNodes
                  << " live_nodes=" << deleteCondLiveNodes
                  << std::endl;
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_INC phase=delete_overdelete ms=" << deleteOverdeleteMs
                  << " det_imp_nodes=" << detImpactNodesCount
                  << " det_imp_edges=" << detImpactEdgesCount
                  << " delta_live_nodes=" << deleteOverdeleteChangedNodes
                  << " live_nodes=" << deleteOverdeleteLiveNodes
                  << std::endl;
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_INC phase=delete_varorder ms=" << deleteVarOrderMs
                  << " collect_ms=" << deleteVarCollectMs
                  << " dump_ms=" << deleteVarDumpMs
                  << " deleted_vars=" << deletedVarsIndexCount
                  << std::endl;
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_INC phase=delete_prep_detail"
                  << " facts_collect_ms=" << deleteFactsCollectMs
                  << " weight_update_ms=" << deleteWeightUpdateMs
                  << " erase_delta_nodes_ms=" << deleteEraseDeltaNodesMs
                  << " erase_delta_edges_ms=" << deleteEraseDeltaEdgesMs
                  << " clean_invalid_nodes_ms=" << deleteCleanInvalidNodesMs
                  << " clean_invalid_edges_ms=" << deleteCleanInvalidEdgesMs
                  << " build_deleted_out_edges_ms=" << deleteBuildDeletedOutEdgesMs
                  << " impact_det_facts_ms=" << deleteImpactDetFactsMs
                  << " impact_det_delta_ms=" << deleteImpactDetDeltaMs
                  << " impact_nondet_facts_ms=" << deleteImpactNonDetFactsMs
                  << " impact_deleted_edge_seed_ms=" << deleteImpactDeletedEdgeSeedMs
                  << " impact_deleted_edge_closure_ms=" << deleteImpactDeletedEdgeClosureMs
                  << " nondet_var_list_ms=" << deleteNonDetVarListMs
                  << " impact_finalize_ms=" << deleteImpactFinalizeMs
                  << " impact_hash_ms=" << deleteImpactHashMs
                  << " condition_ms=" << deleteCondMs
                  << " overdelete_ms=" << deleteOverdeleteMs
                  << " varorder_ms=" << deleteVarOrderMs
                  << " removed_delta_node_formulas=" << deleteRemovedDeltaNodeFormulas
                  << " removed_delta_edge_formulas=" << deleteRemovedDeltaEdgeFormulas
                  << " removed_invalid_node_formulas=" << deleteRemovedInvalidNodeFormulas
                  << " removed_invalid_edge_formulas=" << deleteRemovedInvalidEdgeFormulas
                  << " worklist_enqueue_attempts=" << deleteWorklistEnqueueAttempts
                  << " worklist_enqueue_inserted=" << deleteWorklistEnqueueInserted
                  << " det_imp_nodes=" << detImpactNodesCount
                  << " det_imp_edges=" << detImpactEdgesCount
                  << " nondet_imp_nodes=" << nonDetImpactNodesCount
                  << " nondet_imp_edges=" << nonDetImpactEdgesCount
                  << std::endl;
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_INC phase=rederive ms=" << rederiveLoopMsProfile
                  << " edge_processed=" << rederiveStats.edge_processed
                  << " edge_requeued=" << rederiveStats.edge_requeued
                  << " edge_updated=" << rederiveStats.edge_updated
                  << " node_recomputed=" << rederiveStats.node_recomputed
                  << " node_updated=" << rederiveStats.node_updated
                  << " make_and_calls=" << rederiveStats.make_and_calls
                  << " make_and_ms=" << rederiveStats.make_and_ms
                  << " make_or_calls=" << rederiveStats.make_or_calls
                  << " make_or_ms=" << rederiveStats.make_or_ms
                  << " make_condition_calls=" << rederiveStats.make_condition_calls
                  << " make_condition_ms=" << rederiveStats.make_condition_ms
                  << " input_literal_calls=" << rederiveStats.input_literal_calls
                  << " input_literal_missing=" << rederiveStats.input_literal_missing
                  << " input_literal_ms=" << rederiveStats.input_literal_ms
                  << std::endl;
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_INC phase=insert_init ms="
                  << (insertPreConfigMs + insertInitNodesMs + insertInitEdgesMs)
                  << " preConfig_ms=" << insertPreConfigMs
                  << " init_nodes_ms=" << insertInitNodesMs
                  << " init_edges_ms=" << insertInitEdgesMs
                  << " insert_fact_vars=" << insertFactVars
                  << " insert_edge_vars=" << insertEdgeVars
                  << std::endl;
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_INC phase=insert_init_nodes ms=" << insertInitNodesMs
                  << " fact_true_ms=" << insertInitFactTrueMs
                  << " fact_var_ms=" << insertInitFactVarMs
                  << " fact_weight_ms=" << insertInitFactWeightMs
                  << " nonfact_ms=" << insertInitNonFactMs
                  << " fact_true=" << insertFactTrueCount
                  << " fact_var=" << insertFactVarCount
                  << " nonfact=" << insertNonFactCount
                  << std::endl;
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_INC phase=insert_init_edges ms=" << insertInitEdgesMs
                  << " setfalse_ms=" << insertInitEdgeSetFalseMs
                  << " var_ms=" << insertInitEdgeVarMs
                  << " weight_ms=" << insertInitEdgeWeightMs
                  << " enqueue_ms=" << insertInitEdgeEnqueueMs
                  << " det_edges=" << insertDetEdges
                  << " nondet_edges=" << insertNonDetEdges
                  << std::endl;
        std::cout << "[fc-profile] stage=FORWARD_COMPILATION_INC phase=insert_loop ms=" << insertLoopMsProfile
                  << " edge_processed=" << insertStats.edge_processed
                  << " edge_requeued=" << insertStats.edge_requeued
                  << " edge_updated=" << insertStats.edge_updated
                  << " node_recomputed=" << insertStats.node_recomputed
                  << " node_updated=" << insertStats.node_updated
                  << " make_and_calls=" << insertStats.make_and_calls
                  << " make_and_ms=" << insertStats.make_and_ms
                  << " make_or_calls=" << insertStats.make_or_calls
                  << " make_or_ms=" << insertStats.make_or_ms
                  << " make_condition_calls=" << insertStats.make_condition_calls
                  << " make_condition_ms=" << insertStats.make_condition_ms
                  << " input_literal_calls=" << insertStats.input_literal_calls
                  << " input_literal_missing=" << insertStats.input_literal_missing
                  << " input_literal_ms=" << insertStats.input_literal_ms
                  << " delta_live_nodes=" << insertChangedNodes
                  << " live_nodes=" << insertLiveNodes
                  << std::endl;
    }


}


template<typename FormulaNodeRef>
void clearIncRegionalStateForClassicPath(FormulaManager<FormulaNodeRef>& formulaManager) {
    incRegionalTurnSummary.reset();
    if (incRegionalOutputProfile.active && !incRegionalOutputProfile.overrideWeights.empty()) {
        for (const auto& [varIdx, weights] : incRegionalOutputProfile.originalWeights) {
            formulaManager.setVariableWeight(varIdx, weights.first, weights.second);
        }
    }
    incRegionalOutputProfile.reset();
}


template<typename FormulaNodeRef>
void buildFormulasIncRegionalCyclewise(
    IncrementalDerivationGraphViewInterface& view,
    FormulaManager<FormulaNodeRef>& formulaManager,
    std::map<NodePtr, FormulaNodeRef>& nodeFormulas,
    std::map<EdgePtr, FormulaNodeRef>& edgeFormulas,
    std::set<NodePtr>& changedNodes
) {
    const auto& deltaInsertedEdges = view.getDeltaInsertEdges();
    const auto& deltaInsertedNodes = view.getDeltaInsertNodes();
    if (deltaInsertedEdges.empty() && deltaInsertedNodes.empty()) {
        clearIncRegionalStateForClassicPath(formulaManager);
        buildFormulasIncCyclewise(view, formulaManager, nodeFormulas, edgeFormulas, changedNodes);
        return;
    }
    debugger.logMessage(Level::INFO, "[inc-regional] start pipeline");
    clearIncRegionalStateForClassicPath(formulaManager);
    using namespace std::chrono;
    const bool fcProfile = fcProfileEnabled;
    using Clock = std::chrono::steady_clock;
    auto toMs = [](Clock::time_point t0, Clock::time_point t1) {
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
    };
    auto fcLiteStart = Clock::now();
    double deleteDepGraphMs = 0.0;
    double deleteConditionMs = 0.0;
    double deleteOverdeleteMs = 0.0;
    double deleteVarOrderMs = 0.0;
    double deleteRederiveMs = 0.0;
    std::size_t deleteRederiveRounds = 0;
    const auto& deltaDeletedEdges = view.getDeltaDeleteEdges();
    const auto& deltaDeletedNodes = view.getDeltaDeleteNodes();
    const auto& deltaInsertFactNodes = view.getDeltaInsertFactNodes();
    debugger.logMessage(Level::INFO, "[inc-regional] delta counts: insNodes=" +
        std::to_string(deltaInsertedNodes.size()) + " insEdges=" +
        std::to_string(deltaInsertedEdges.size()) + " delNodes=" +
        std::to_string(deltaDeletedNodes.size()) + " delEdges=" +
        std::to_string(deltaDeletedEdges.size()));

    if (deltaInsertedEdges.empty() && deltaInsertedNodes.empty() &&
            deltaDeletedEdges.empty() && deltaDeletedNodes.empty()) {
        incRegionalTurnSummary.valid = true;
        debugger.logMessage(Level::INFO, "No changes to apply, skipping incremental update");
        formulaManager.dumpProfilingStatistics();
        for (auto& [key, value]: formulaManager.getProfilingStatistics()) {
            debugger.addInfo(key, value);
        }
        debugger.logMessage(Level::INFO, "[inc-regional] pipeline finished; usedFallback=false");
        return;
    }
    if (deltaInsertedEdges.empty() && deltaInsertedNodes.empty()) {
        debugger.logMessage(Level::INFO,
                "[inc-regional] deletion-only delta; using shared inc-naive FC path");
        buildFormulasIncCyclewise(view, formulaManager, nodeFormulas, edgeFormulas, changedNodes);
        debugger.logMessage(Level::INFO,
                "[inc-regional] deletion-only shared path finished; usedFallback=false");
        return;
    }
    const std::size_t rawDeltaWorkScore =
            deltaInsertedNodes.size() + deltaInsertedEdges.size() +
            deltaDeletedNodes.size() + deltaDeletedEdges.size();
    maybePrepareIncReorderPolicy(formulaManager, rawDeltaWorkScore);
    const CycleDependencyGraph* regionalInsertDepGraph = nullptr;

    if (!deltaDeletedEdges.empty() || !deltaDeletedNodes.empty()) {
        auto start = high_resolution_clock::now();
        auto depGraphStart = Clock::now();
        auto& depGraph = view.getCycleDependencyGraph();  // includes SCC/dependencies/depths
        regionalInsertDepGraph = &depGraph;
        deleteDepGraphMs = toMs(depGraphStart, Clock::now());
        auto end = high_resolution_clock::now();
        debugger.logMessage(Level::INFO, "Finished building dependency graph and preparation. Time: " +
            std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");

        debugger.logMessage(Level::INFO, "[inc-regional] Performing deletion");
        start = high_resolution_clock::now();
        std::map<size_t, std::priority_queue<PrioritizedEdge> > cycleWorklists;
        std::map<size_t, std::set<EdgePtr> > cycleInWorklists;
        std::unordered_set<NodePtr> detImpactNodes;
        std::unordered_set<EdgePtr> detImpactEdges;
        std::unordered_set<NodePtr> nonDetImpactNodes;
        std::unordered_set<EdgePtr> nonDetImpactEdges;
        std::vector<int> deletedNonDetVars;
        std::unordered_set<NodePtr> regionalOverdeleteChangedSet;
        auto markRegionalOverdelete = [&](const NodePtr& node) {
            changedNodes.insert(node);
            regionalOverdeleteChangedSet.insert(node);
        };

        std::set<NodePtr> deletedFacts = view.getDeletedFacts();
        std::set<NodePtr> deletedDeterminsticFacts = view.getDeletedDeterminsticFacts();
        std::set<NodePtr> deletedNonDeterminsticFacts = view.getDeletedNonDeterministicFacts();
        for (auto deletedFact: deletedFacts) {
            assertProbabilityInRange(0.0, "deleted fact weight");
            formulaManager.setVariableWeight(formulaManager.getVarIndex(*deletedFact), 0.0, 1.0);
        }
        for (auto node : deltaDeletedNodes) {
            nodeFormulas.erase(node);
            changedNodes.insert(node);
        }

        for (auto edge: deltaDeletedEdges) {
            edgeFormulas.erase(edge);
        }

        for (auto it = nodeFormulas.begin(); it != nodeFormulas.end(); ) {
            if (view.getValidNodes().find(it->first) == view.getValidNodes().end()) {
                it = nodeFormulas.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = edgeFormulas.begin(); it != edgeFormulas.end(); ) {
            if (view.getValidEdges().find(it->first) == view.getValidEdges().end()) {
                it = edgeFormulas.erase(it);
            } else {
                ++it;
            }
        }

        const auto deletedOutEdges = buildDeletedOutEdges(deltaDeletedEdges);
        if (!deletedDeterminsticFacts.empty()) {
            std::vector<NodePtr> detSources(deletedDeterminsticFacts.begin(),
                                            deletedDeterminsticFacts.end());
            collectImpactUnionWithDeletedEdges(view, detSources, deletedOutEdges, detImpactNodes, detImpactEdges);
        }
        std::vector<NodePtr> detDeltaDeleteSources;
        detDeltaDeleteSources.reserve(deltaDeletedNodes.size());
        if (!deltaDeletedNodes.empty()) {
            const auto& liveEdges = view.getValidEdges();
            for (const auto& node : deltaDeletedNodes) {
                if (!node || node->getProbability() != 1.0) {
                    continue;
                }
                bool inView = false;
                for (const auto& e : node->getOutgoingEdges()) {
                    if (liveEdges.count(e)) {
                        inView = true;
                        break;
                    }
                }
                if (inView) {
                    detDeltaDeleteSources.push_back(node);
                }
            }
        }
        if (!detDeltaDeleteSources.empty()) {
            collectImpactUnionWithDeletedEdges(view, detDeltaDeleteSources, deletedOutEdges,
                                               detImpactNodes, detImpactEdges);
        }
        if (!deletedNonDeterminsticFacts.empty()) {
            std::vector<NodePtr> nonDetSources(deletedNonDeterminsticFacts.begin(),
                                               deletedNonDeterminsticFacts.end());
            collectImpactUnionWithDeletedEdges(view, nonDetSources, deletedOutEdges, nonDetImpactNodes, nonDetImpactEdges);
        }
        if (!deltaDeletedEdges.empty()) {
            std::vector<NodePtr> detEdgeOutputs;
            detEdgeOutputs.reserve(deltaDeletedEdges.size());
            for (const auto& edge : deltaDeletedEdges) {
                NodePtr out = view.getOutput(edge);
                if (!out || out->isFact) {
                    continue;
                }
                detImpactNodes.insert(out);
                detEdgeOutputs.push_back(out);
            }
            if (!detEdgeOutputs.empty()) {
                collectImpactUnionWithDeletedEdges(view, detEdgeOutputs, deletedOutEdges,
                                                   detImpactNodes, detImpactEdges);
            }
        }
        if (!deletedNonDeterminsticFacts.empty()) {
            deletedNonDetVars.reserve(deletedNonDeterminsticFacts.size());
            for (auto node : deletedNonDeterminsticFacts) {
                deletedNonDetVars.push_back(formulaManager.getVarIndex(*node));
            }
        }

        std::unordered_set<NodePtr> nonDetOnlyNodes;
        std::unordered_set<EdgePtr> nonDetOnlyEdges;
        for (auto node : nonDetImpactNodes) {
            if (!detImpactNodes.count(node)) {
                nonDetOnlyNodes.insert(node);
            }
        }
        for (auto edge : nonDetImpactEdges) {
            if (!detImpactEdges.count(edge)) {
                nonDetOnlyEdges.insert(edge);
            }
        }

        if (incRegionalProfileEnabled || incProfileEnabled) {
            auto fnv1a = [](const std::string& s) {
                std::uint64_t h = 1469598103934665603ULL;
                for (unsigned char c : s) {
                    h ^= c;
                    h *= 1099511628211ULL;
                }
                return h;
            };
            auto hashNodeSet = [&](const std::unordered_set<NodePtr>& nodes) {
                std::uint64_t h = 0;
                for (const auto& n : nodes) {
                    if (!n) continue;
                    h ^= fnv1a(n->getTuple().toString());
                }
                return h;
            };
            auto hashEdgeSet = [&](const std::unordered_set<EdgePtr>& edges) {
                std::uint64_t h = 0;
                for (const auto& e : edges) {
                    if (!e) continue;
                    std::ostringstream oss;
                    auto ins = view.getInputs(e);
                    for (size_t i = 0; i < ins.size(); ++i) {
                        if (i) oss << ",";
                        oss << (ins[i] ? ins[i]->getTuple().toString() : "<null>");
                    }
                    NodePtr out = view.getOutput(e);
                    oss << "->" << (out ? out->getTuple().toString() : "<null>");
                    h ^= fnv1a(oss.str());
                }
                return h;
            };
            debugger.logMessage(Level::INFO,
                "[inc-regional delete] detImpactNodes=" + std::to_string(detImpactNodes.size()) +
                " hash=" + std::to_string(hashNodeSet(detImpactNodes)) +
                " detImpactEdges=" + std::to_string(detImpactEdges.size()) +
                " hash=" + std::to_string(hashEdgeSet(detImpactEdges)));
            debugger.logMessage(Level::INFO,
                "[inc-regional delete] nonDetImpactNodes=" + std::to_string(nonDetImpactNodes.size()) +
                " hash=" + std::to_string(hashNodeSet(nonDetImpactNodes)) +
                " nonDetImpactEdges=" + std::to_string(nonDetImpactEdges.size()) +
                " hash=" + std::to_string(hashEdgeSet(nonDetImpactEdges)));
            debugger.logMessage(Level::INFO,
                "[inc-regional delete] nonDetOnlyNodes=" + std::to_string(nonDetOnlyNodes.size()) +
                " hash=" + std::to_string(hashNodeSet(nonDetOnlyNodes)) +
                " nonDetOnlyEdges=" + std::to_string(nonDetOnlyEdges.size()) +
                " hash=" + std::to_string(hashEdgeSet(nonDetOnlyEdges)));
        }

        auto enqueueEdge = [&](EdgePtr edge) {
            auto it = depGraph.edgeToCycleIndex.find(edge);
            if (it == depGraph.edgeToCycleIndex.end()) {
                return;
            }
            auto& worklist = cycleWorklists[it->second];
            auto& inWorklist = cycleInWorklists[it->second];
            if (inWorklist.insert(edge).second) {
                auto depthIt = depGraph.edgeDepthsGlobal.find(edge);
                const int seqId = depthIt == depGraph.edgeDepthsGlobal.end()
                    ? 0
                    : static_cast<int>(depthIt->second);
                const size_t priority = depthIt == depGraph.edgeDepthsGlobal.end()
                    ? 0
                    : depthIt->second;
                worklist.push({edge, priority, seqId});
            }
        };

        if (!deletedNonDetVars.empty() && (!nonDetOnlyNodes.empty() || !nonDetOnlyEdges.empty())) {
            start = high_resolution_clock::now();
            auto conditionStart = Clock::now();
            for (auto node : nonDetOnlyNodes) {
                if (deltaDeletedNodes.count(node)) {
                    continue;
                }
                if (nodeFormulas.count(node) == 0 || formulaManager.isSame(nodeFormulas[node], formulaManager.getFalse())) {
                    continue;
                }
                if (node->isFact) {
                    continue;
                }
                auto newNodeFormula = formulaManager.makeCondition(nodeFormulas[node], {}, deletedNonDetVars);
                if (!formulaManager.isSame(nodeFormulas[node], newNodeFormula)) {
                    nodeFormulas[node] = newNodeFormula;
                    changedNodes.insert(node);
                    for (EdgePtr outEdge : view.getOutgoingEdges(node)) {
                        enqueueEdge(outEdge);
                    }
                }
            }
            for (auto edge : nonDetOnlyEdges) {
                if (deltaDeletedEdges.count(edge)) {
                    continue;
                }
                auto it = edgeFormulas.find(edge);
                if (it == edgeFormulas.end() || !it->second.get()) {
                    continue;
                }
                auto newEdgeFormula = formulaManager.makeCondition(it->second, {}, deletedNonDetVars);
                if (!formulaManager.isSame(it->second, newEdgeFormula)) {
                    edgeFormulas[edge] = newEdgeFormula;
                    enqueueEdge(edge);
                }
            }
            end = high_resolution_clock::now();
            deleteConditionMs = toMs(conditionStart, Clock::now());
            debugger.logMessage(Level::INFO, "Finished conditioning on deleted non-deterministic facts (non-det only). Time: " +
                std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
        }

        start = high_resolution_clock::now();
        auto overdeleteStart = Clock::now();
        if (!detImpactNodes.empty() || !detImpactEdges.empty()) {
            for (auto node : detImpactNodes) {
                if (deltaDeletedNodes.count(node)) {
                    continue;
                }
                if (nodeFormulas.count(node) == 0 || formulaManager.isSame(nodeFormulas[node], formulaManager.getFalse())) {
                    continue;
                }
                if (node->isFact) {
                    continue;
                }
                nodeFormulas[node] = formulaManager.getFalse();
                markRegionalOverdelete(node);
                for (EdgePtr inEdge : view.getIncomingEdges(node)) {
                    enqueueEdge(inEdge);
                }
            }
            for (auto edge : detImpactEdges) {
                if (deltaDeletedEdges.count(edge)) {
                    continue;
                }
                NodePtr out = view.getOutput(edge);
                if (!out || out->isFact) {
                    continue;
                }
                edgeFormulas[edge] = formulaManager.getFalse();
                enqueueEdge(edge);
            }
        }
        end = high_resolution_clock::now();
        debugger.logMessage(Level::INFO, "Finished over-deleting impacted formulas. Time: " +
            std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
        deleteOverdeleteMs = toMs(overdeleteStart, Clock::now());

        start = high_resolution_clock::now();
        auto varOrderStart = Clock::now();
        std::set<int> deletedVarsIndex;
        for (auto node: deletedNonDeterminsticFacts) {
            auto index = formulaManager.getVarIndex(*node);
            deletedVarsIndex.insert(index);
        }
        for (auto edge: deltaDeletedEdges) {
            if (edge->isDeterministic()) continue;
            auto index = formulaManager.getVarIndex(*edge);
            deletedVarsIndex.insert(index);
        }
        debugger.logMessage(Level::INFO, "Deletion deletedVarsIndex size: " +
            std::to_string(deletedVarsIndex.size()));
        formulaManager.dumpProfilingStatistics();
        end = high_resolution_clock::now();
        deleteVarOrderMs = toMs(varOrderStart, Clock::now());
        debugger.logMessage(Level::INFO, "Finished updating variable ordering after deletion (non-deterministic). Time: " +
            std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");

        // Ensure inserted fact nodes are available during re-derivation.
        if (!deltaInsertFactNodes.empty()) {
            std::size_t preInitFacts = 0;
            for (auto node : deltaInsertFactNodes) {
                if (!node || nodeFormulas.count(node)) {
                    continue;
                }
                if (!node->isFact) {
                    continue;
                }
                const double prob = node->getProbability();
                if (prob == 1.0) {
                    nodeFormulas[node] = formulaManager.getTrue();
                } else {
                    int idx = formulaManager.getVarIndex(*node);
                    nodeFormulas[node] = formulaManager.createVar(idx, *node);
                    assertProbabilityInRange(prob, "inc preinit fact " + node->toString());
                    formulaManager.setVariableWeight(idx, prob, 1 - prob);
                }
                ++preInitFacts;
            }
            if (fcProfile && preInitFacts > 0) {
                std::cout << "[fc-preinit] phase=rederive facts=" << preInitFacts << std::endl;
            }
        }

        start = high_resolution_clock::now();
        auto rederiveStart = Clock::now();
        std::queue<size_t> ready;
        std::vector<bool> scheduled(depGraph.nodeCycles.size(), false);
        std::vector<size_t> inDegree = depGraph.inDegrees;
        for (size_t cid = 0; cid < depGraph.nodeCycles.size(); ++cid)
            if (inDegree[cid] == 0) {ready.push(cid); scheduled[cid] = true;}

        while (!ready.empty()) {
            size_t cid = ready.front(); ready.pop();
            scheduled[cid] = true;
            auto& worklist = cycleWorklists[cid];
            int round = 0;
            int _seqId = 0;
            while (!worklist.empty()) {
                auto* iteration = debugger.startIteration();
                EdgePtr edge = worklist.top().edge; worklist.pop();
                cycleInWorklists[cid].erase(edge);
                round++;
                deleteRederiveRounds++;
                FormulaNodeRef newEdge;
                bool allAvailable = true;
                {
                    FormulaNodeRef baseFormula = edge->isDeterministic()
                        ? formulaManager.getTrue()
                        : formulaManager.createVar(formulaManager.getVarIndex(*edge), *edge);
                    std::vector<FormulaNodeRef> inputFormulas{baseFormula};

                    const auto& inputs = view.getInputs(edge);
                    const auto& negs = view.getBodyNegations(edge);
                    for (size_t i = 0; i < inputs.size(); ++i) {
                        FormulaNodeRef lit;
                        if (!fcInputFormulaLiteral(formulaManager, nodeFormulas, inputs[i], negs[i], lit)) {
                            NodePtr missing = inputs[i];
                            if (missing && nodeFormulas.count(missing) == 0 &&
                                    deltaInsertedNodes.count(missing)) {
                                if (missing->isFact) {
                                    const double prob = missing->getProbability();
                                    if (prob == 1.0) {
                                        nodeFormulas[missing] = formulaManager.getTrue();
                                    } else {
                                        int idx = formulaManager.getVarIndex(*missing);
                                        nodeFormulas[missing] = formulaManager.createVar(idx, *missing);
                                        assertProbabilityInRange(prob, "inc lazy preinit fact " + missing->toString());
                                        formulaManager.setVariableWeight(idx, prob, 1 - prob);
                                    }
                                } else {
                                    nodeFormulas[missing] = formulaManager.getFalse();
                                }
                            }
                            allAvailable = false;
                            break;
                        }
                        inputFormulas.push_back(lit);
                    }

                    if (!allAvailable) {
                        worklist.push({edge, depGraph.edgeDepthsGlobal.at(edge), _seqId++});
                        cycleInWorklists[cid].insert(edge);
                        debugger.endIteration();
                        continue;
                    }

                    newEdge = formulaManager.makeAnd(inputFormulas);
                }
                NodePtr output = view.getOutput(edge);
                const bool edgeSame = formulaManager.isSame(edgeFormulas[edge], newEdge);
                const bool forceNodeUpdate = output && regionalOverdeleteChangedSet.count(output);
                if (edgeSame && !forceNodeUpdate) {
                    debugger.endIteration();
                    continue;
                }

                if (!edgeSame) {
                    edgeFormulas[edge] = newEdge;
                }

                if (!output || output->isFact) {
                    debugger.endIteration();
                    continue;
                }

                FormulaNodeRef newNode;
                bool hasNewNode = false;
                if (!hasNewNode) {
                    std::vector<FormulaNodeRef> incoming;
                    for (EdgePtr e : view.getIncomingEdges(output)) {
                        if (edgeFormulas.count(e) && edgeFormulas[e].get()) {
                            incoming.push_back(edgeFormulas[e]);
                        }
                    }
                    if (!incoming.empty()) {
                        newNode = formulaManager.makeOr(incoming);
                        hasNewNode = true;
                    }
                }
                if (hasNewNode && !formulaManager.isSame(nodeFormulas[output], newNode)) {
                    nodeFormulas[output] = newNode;
                    markRegionalOverdelete(output);
                    for (EdgePtr outEdge : view.getOutgoingEdges(output)) {
                        assert (depGraph.edgeToCycleIndex.count(outEdge));
                        auto it = depGraph.edgeToCycleIndex.find(outEdge);
                        if (it->second == cid && !cycleInWorklists[cid].count(outEdge)) {
                            cycleWorklists[cid].push({outEdge, depGraph.edgeDepthsGlobal.at(outEdge), _seqId++});
                            cycleInWorklists[cid].insert(outEdge);
                        }
                    }
                }
                debugger.endIteration();
            }

            for (size_t succ : depGraph.reverseDependencies[cid]) {
                if (--inDegree[succ] == 0 && !scheduled[succ]) {
                    ready.push(succ);
                }
            }
        }
        end = high_resolution_clock::now();
        deleteRederiveMs = toMs(rederiveStart, Clock::now());
        debugger.logMessage(Level::INFO, "rederive time: " + std::to_string(duration_cast<milliseconds>(end - start).count()) + " milliseconds");
        if (reuseVarIndexEnabled) {
            for (const auto& node : view.getDeletedFacts()) {
                formulaManager.releaseVarIndex(*node);
            }
            for (const auto& edge : deltaDeletedEdges) {
                formulaManager.releaseVarIndex(*edge);
            }
        }
    } else {
        debugger.logMessage(Level::INFO, "[inc-regional] No deletions; skipping deletion phase");
    }

    if (deltaInsertedEdges.empty() && deltaInsertedNodes.empty()) {
        debugger.logMessage(Level::INFO, "[inc-regional] No inserted edges/nodes, skipping insertion");
        formulaManager.dumpProfilingStatistics();
        for (auto& [key, value]: formulaManager.getProfilingStatistics()) {
            debugger.addInfo(key, value);
        }
        const double fcLiteTotalMs = toMs(fcLiteStart, Clock::now());
        debugger.addInfo("fc_lite_mode", "inc-regional-delete");
        debugger.addInfo("fc_lite_total_ms", std::to_string(fcLiteTotalMs));
        debugger.addInfo("fc_lite_delete_dep_graph_ms", std::to_string(deleteDepGraphMs));
        debugger.addInfo("fc_lite_delete_condition_ms", std::to_string(deleteConditionMs));
        debugger.addInfo("fc_lite_delete_overdelete_ms", std::to_string(deleteOverdeleteMs));
        debugger.addInfo("fc_lite_delete_var_order_ms", std::to_string(deleteVarOrderMs));
        debugger.addInfo("fc_lite_delete_rederive_ms", std::to_string(deleteRederiveMs));
        debugger.addInfo("fc_lite_delete_rederive_rounds", std::to_string(deleteRederiveRounds));
        debugger.addInfo("fc_lite_delta_insert_nodes", std::to_string(deltaInsertedNodes.size()));
        debugger.addInfo("fc_lite_delta_insert_edges", std::to_string(deltaInsertedEdges.size()));
        debugger.addInfo("fc_lite_delta_delete_nodes", std::to_string(deltaDeletedNodes.size()));
        debugger.addInfo("fc_lite_delta_delete_edges", std::to_string(deltaDeletedEdges.size()));
        debugger.addInfo("fc_lite_changed_nodes", std::to_string(changedNodes.size()));
        debugger.logMessage(Level::INFO, "[inc-regional] pipeline finished; usedFallback=false");
        return;
    }

    auto insertStart = std::chrono::steady_clock::now();

    // preConfig is handled inside RegionalIncrementalForwardCompilation::applyUpdate

    using FMType = std::remove_reference_t<decltype(formulaManager)>;
    RegionalIncrementalForwardCompilation<FMType, FormulaNodeRef> orchestrator;
    auto updateStart = std::chrono::steady_clock::now();
    orchestrator.applyUpdate(
        view, formulaManager, nodeFormulas, edgeFormulas, changedNodes, regionalInsertDepGraph);
    auto updateEnd = std::chrono::steady_clock::now();
    const auto& timing = orchestrator.getTiming();
    const auto& liteStats = orchestrator.getStats();
    const auto& liteRebuild = timing.rebuildDetail;
    const double fcLiteTotalMs = toMs(fcLiteStart, Clock::now());
    const double updateMs =
            std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();
    debugger.addInfo("inc_regional_analyze_ms", std::to_string(timing.analyzeMs));
    if (!timing.fallbackReason.empty()) {
        debugger.addInfo("inc_regional_fallback_reason", timing.fallbackReason);
    }
    debugger.addInfo("fc_lite_mode", "inc-regional");
    debugger.addInfo("fc_lite_total_ms", std::to_string(fcLiteTotalMs));
    debugger.addInfo("fc_lite_apply_update_ms", std::to_string(updateMs));
    debugger.addInfo("fc_lite_delete_dep_graph_ms", std::to_string(deleteDepGraphMs));
    debugger.addInfo("fc_lite_delete_condition_ms", std::to_string(deleteConditionMs));
    debugger.addInfo("fc_lite_delete_overdelete_ms", std::to_string(deleteOverdeleteMs));
    debugger.addInfo("fc_lite_delete_var_order_ms", std::to_string(deleteVarOrderMs));
    debugger.addInfo("fc_lite_delete_rederive_ms", std::to_string(deleteRederiveMs));
    debugger.addInfo("fc_lite_delete_rederive_rounds", std::to_string(deleteRederiveRounds));
    debugger.addInfo("fc_lite_analyze_ms", std::to_string(timing.analyzeMs));
    debugger.addInfo("fc_lite_scc_close_ms", std::to_string(timing.sccCloseMs));
    debugger.addInfo("fc_lite_plan_ms", std::to_string(timing.planMs));
    debugger.addInfo("fc_lite_rebuild_ms", std::to_string(timing.rebuildMs));
    debugger.addInfo("fc_lite_calibrate_ms", std::to_string(timing.calibrateMs));
    debugger.addInfo("fc_lite_fallback_ms", std::to_string(timing.fallbackMs));
    debugger.addInfo("fc_lite_rebuild_snapshot_ms", std::to_string(liteRebuild.snapshotMs));
    debugger.addInfo("fc_lite_rebuild_init_nodes_ms", std::to_string(liteRebuild.initNodesMs));
    debugger.addInfo("fc_lite_rebuild_init_edges_ms", std::to_string(liteRebuild.initEdgesMs));
    debugger.addInfo("fc_lite_rebuild_dep_graph_ms", std::to_string(liteRebuild.depGraphMs));
    debugger.addInfo("fc_lite_rebuild_region_cycles_ms", std::to_string(liteRebuild.regionCyclesMs));
    debugger.addInfo("fc_lite_rebuild_indegree_ms", std::to_string(liteRebuild.indegreeMs));
    debugger.addInfo("fc_lite_rebuild_loop_ms", std::to_string(liteRebuild.rebuildLoopMs));
    debugger.addInfo("fc_lite_rebuild_total_ms", std::to_string(liteRebuild.totalMs));
    debugger.addInfo("fc_lite_rebuild_reorder_ms", std::to_string(liteRebuild.reorderMs));
    debugger.addInfo("fc_lite_region_nodes", std::to_string(liteStats.regionNodeCount));
    debugger.addInfo("fc_lite_region_edges", std::to_string(liteStats.regionEdgeCount));
    debugger.addInfo("fc_lite_analyze_region_edges", std::to_string(liteStats.analyzeRegionEdges));
    debugger.addInfo("fc_lite_dr_nodes", std::to_string(liteStats.drNodeCount));
    debugger.addInfo("fc_lite_dr_edges", std::to_string(liteStats.drEdgeCount));
    debugger.addInfo("fc_lite_boundary_nodes", std::to_string(liteStats.boundaryNodeCount));
    debugger.addInfo("fc_lite_calibrated_nodes", std::to_string(liteStats.calibratedCount));
    debugger.addInfo("fc_lite_used_fallback", liteStats.usedFallback ? "1" : "0");
    IncReorderPressureScore pressureScore;
    pressureScore.mode = "inc-regional";
    pressureScore.rawDelta = deltaInsertedNodes.size() + deltaInsertedEdges.size() +
            deltaDeletedNodes.size() + deltaDeletedEdges.size();
    pressureScore.frontierNodes = liteStats.regionNodeCount;
    pressureScore.frontierEdges = liteStats.regionEdgeCount;
    pressureScore.bddEdgesProcessed = liteRebuild.edgesProcessed;
    pressureScore.bddNodesUpdated = liteRebuild.nodesUpdated;
    addIncReorderPressureScore(debugger, pressureScore);
    const std::size_t triggerScore = weightedMaxIncReorderScore(pressureScore, debugger);
    const std::size_t fullGraphWorkScore = view.getNodes().size() + view.getEdges().size();
    maybeRunExplicitIncReorder(formulaManager, triggerScore, fullGraphWorkScore);
    if (incRegionalProfileEnabled) {
        debugger.addInfo("inc_regional_apply_update_ms", std::to_string(updateMs));
        const auto& calibrations = orchestrator.getCalibrations();
        const auto& regionNodes = orchestrator.getRegionNodes();
        const auto& boundaryNodes = orchestrator.getBoundaryNodes();
        for (const auto& rec : calibrations) {
            bool anchorInRegion = false;
            bool anchorInDelta = false;
            if (rec.anchor.kind == incra::IncRegionAnalysis::AnchorKind::Node) {
                if (rec.anchor.node) {
                    anchorInRegion = regionNodes.count(rec.anchor.node) > 0;
                    anchorInDelta = incRegionalOutputProfile.deltaReachableNodes.count(rec.anchor.node) > 0;
                }
            } else {
                if (rec.anchor.edge) {
                    NodePtr out = view.getOutput(rec.anchor.edge);
                    if (out) {
                        anchorInRegion = regionNodes.count(out) > 0;
                        anchorInDelta = incRegionalOutputProfile.deltaReachableNodes.count(out) > 0;
                    }
                }
            }
            debugger.logMessage(
                Level::INFO,
                std::string("[inc-regional-calibration] head=") + incra::node_id(rec.v) +
                    " head_in_region=" + (regionNodes.count(rec.v) ? "1" : "0") +
                    " head_in_boundary=" + (boundaryNodes.count(rec.v) ? "1" : "0") +
                    " anchor=" + incra::anchor_id(rec.anchor, view) +
                    " anchor_in_region=" + (anchorInRegion ? "1" : "0") +
                    " anchor_in_delta_reach=" + (anchorInDelta ? "1" : "0") +
                    " pStar=" + std::to_string(rec.pStar) +
                    " target=" + std::to_string(rec.target) +
                    " alpha=" + std::to_string(rec.alpha) +
                    " beta=" + std::to_string(rec.beta));
        }
    }
    if (incRegionalProfileEnabled) {
        auto debugStart = std::chrono::steady_clock::now();
        const auto& stats = orchestrator.getStats();
        const auto& rt = timing.rebuildDetail;
        debugger.addInfo("inc_regional_scc_close_ms", std::to_string(timing.sccCloseMs));
        debugger.addInfo("inc_regional_plan_ms", std::to_string(timing.planMs));
        debugger.addInfo("inc_regional_rebuild_ms", std::to_string(timing.rebuildMs));
        debugger.addInfo("inc_regional_calibrate_ms", std::to_string(timing.calibrateMs));
        debugger.addInfo("inc_regional_total_ms", std::to_string(timing.totalMs));
        debugger.addInfo("inc_regional_analyze_region_nodes", std::to_string(stats.analyzeRegionNodes));
        debugger.addInfo("inc_regional_analyze_region_edges", std::to_string(stats.analyzeRegionEdges));
        debugger.addInfo("inc_regional_analyze_dr_nodes", std::to_string(stats.analyzeDrNodes));
        debugger.addInfo("inc_regional_analyze_dr_edges", std::to_string(stats.analyzeDrEdges));
        double analyzeRatio = stats.analyzeDrNodes == 0 ? 1.0
                                                        : static_cast<double>(stats.analyzeRegionNodes) /
                                                                  static_cast<double>(stats.analyzeDrNodes);
        debugger.addInfo("inc_regional_analyze_region_dr_ratio", std::to_string(analyzeRatio));
        debugger.addInfo("inc_regional_region_nodes", std::to_string(stats.regionNodeCount));
        debugger.addInfo("inc_regional_region_edges", std::to_string(stats.regionEdgeCount));
        debugger.addInfo("inc_regional_dr_nodes", std::to_string(stats.drNodeCount));
        debugger.addInfo("inc_regional_dr_edges", std::to_string(stats.drEdgeCount));
        double regionDrRatio = stats.drNodeCount == 0 ? 1.0
                                                      : static_cast<double>(stats.regionNodeCount) /
                                                                static_cast<double>(stats.drNodeCount);
        debugger.addInfo("inc_regional_region_dr_ratio", std::to_string(regionDrRatio));
        debugger.addInfo("inc_regional_output_slice_expanded", stats.outputSliceExpanded ? "1" : "0");
        debugger.addInfo("inc_regional_output_slice_missing_outputs",
                std::to_string(stats.outputSliceMissingOutputs));
        debugger.addInfo("inc_regional_output_slice_region_nodes",
                std::to_string(stats.outputSliceRegionNodes));
        debugger.addInfo("inc_regional_output_slice_region_edges",
                std::to_string(stats.outputSliceRegionEdges));
        debugger.addInfo("inc_regional_boundary_empty_expanded", stats.boundaryEmptyExpanded ? "1" : "0");
        debugger.addInfo("inc_regional_boundary_empty_region_nodes",
                std::to_string(stats.boundaryEmptyRegionNodes));
        debugger.addInfo("inc_regional_boundary_empty_region_edges",
                std::to_string(stats.boundaryEmptyRegionEdges));
        debugger.addInfo("inc_regional_near_full_expanded", stats.nearFullExpanded ? "1" : "0");
        debugger.addInfo("inc_regional_near_full_region_nodes",
                std::to_string(stats.nearFullRegionNodes));
        debugger.addInfo("inc_regional_near_full_region_edges",
                std::to_string(stats.nearFullRegionEdges));
        debugger.addInfo("inc_regional_plan_expand_attempts",
                std::to_string(stats.planExpandAttempts));
        debugger.addInfo("inc_regional_plan_expand_region_nodes",
                std::to_string(stats.planExpandRegionNodes));
        debugger.addInfo("inc_regional_plan_expand_region_edges",
                std::to_string(stats.planExpandRegionEdges));
        debugger.addInfo("inc_regional_plan_expand_failed_boundaries",
                std::to_string(stats.planExpandFailedBoundaries));
        debugger.addInfo("inc_regional_boundary_nodes", std::to_string(stats.boundaryNodeCount));
        debugger.addInfo("inc_regional_calibrated_nodes", std::to_string(stats.calibratedCount));
        debugger.addInfo("inc_regional_scc_expanded", stats.sccExpanded ? "1" : "0");
        debugger.addInfo("inc_regional_used_fallback", stats.usedFallback ? "1" : "0");
        debugger.addInfo("inc_regional_rebuild_snapshot_ms", std::to_string(rt.snapshotMs));
        debugger.addInfo("inc_regional_rebuild_init_nodes_ms", std::to_string(rt.initNodesMs));
        debugger.addInfo("inc_regional_rebuild_init_edges_ms", std::to_string(rt.initEdgesMs));
        debugger.addInfo("inc_regional_rebuild_dep_graph_ms", std::to_string(rt.depGraphMs));
        debugger.addInfo("inc_regional_rebuild_region_cycles_ms", std::to_string(rt.regionCyclesMs));
        debugger.addInfo("inc_regional_rebuild_indegree_ms", std::to_string(rt.indegreeMs));
        debugger.addInfo("inc_regional_rebuild_loop_ms", std::to_string(rt.rebuildLoopMs));
        debugger.addInfo("inc_regional_rebuild_total_ms", std::to_string(rt.totalMs));
        debugger.addInfo("inc_regional_rebuild_reorder_ms", std::to_string(rt.reorderMs));
        debugger.addInfo("inc_regional_rebuild_edges_processed", std::to_string(rt.edgesProcessed));
        debugger.addInfo("inc_regional_rebuild_edges_rebuilt", std::to_string(rt.edgesRebuilt));
        debugger.addInfo("inc_regional_rebuild_nodes_updated", std::to_string(rt.nodesUpdated));
        debugger.addInfo("inc_regional_rebuild_cycle_count", std::to_string(rt.regionCycleCount));
        debugger.addInfo("inc_regional_fallback_ms", std::to_string(timing.fallbackMs));
        debugger.addInfo("inc_regional_total_with_fallback_ms", std::to_string(timing.totalWithFallbackMs));
        debugger.logMessage(Level::INFO,
            "[inc-regional-profile] analyze_ms=" + std::to_string(timing.analyzeMs) +
            " scc_close_ms=" + std::to_string(timing.sccCloseMs) +
            " plan_ms=" + std::to_string(timing.planMs) +
            " rebuild_ms=" + std::to_string(timing.rebuildMs) +
            " calibrate_ms=" + std::to_string(timing.calibrateMs) +
            " total_ms=" + std::to_string(timing.totalMs) +
            " fallback_ms=" + std::to_string(timing.fallbackMs) +
            " total_with_fallback_ms=" + std::to_string(timing.totalWithFallbackMs));
        debugger.logMessage(Level::INFO,
            "[inc-regional-profile] rebuild snapshot_ms=" + std::to_string(rt.snapshotMs) +
            " init_nodes_ms=" + std::to_string(rt.initNodesMs) +
            " init_edges_ms=" + std::to_string(rt.initEdgesMs) +
            " dep_graph_ms=" + std::to_string(rt.depGraphMs) +
            " region_cycles_ms=" + std::to_string(rt.regionCyclesMs) +
            " indegree_ms=" + std::to_string(rt.indegreeMs) +
            " rebuild_loop_ms=" + std::to_string(rt.rebuildLoopMs) +
            " total_ms=" + std::to_string(rt.totalMs) +
            " reorder_ms=" + std::to_string(rt.reorderMs));
        if (stats.usedFallback) {
            debugger.logMessage(Level::INFO, "[inc-regional-profile] usedFallback=1");
        }
        if (!timing.fallbackReason.empty()) {
            debugger.logMessage(Level::INFO,
                "[inc-regional-profile] fallback_reason=" + timing.fallbackReason);
        }
        auto debugEnd = std::chrono::steady_clock::now();
        const auto debugMs = std::chrono::duration_cast<std::chrono::milliseconds>(debugEnd - debugStart).count();
        debugger.addInfo("inc_regional_debug_info_ms", std::to_string(debugMs));
    }
    if (incRegionalProfileEnabled) {
        auto dumpStart = std::chrono::steady_clock::now();
        formulaManager.dumpProfilingStatistics();
        auto dumpEnd = std::chrono::steady_clock::now();
        const auto dumpMs = std::chrono::duration_cast<std::chrono::milliseconds>(dumpEnd - dumpStart).count();
        debugger.addInfo("inc_regional_dump_profile_ms", std::to_string(dumpMs));
    } else {
        formulaManager.dumpProfilingStatistics();
    }
    if (incRegionalProfileEnabled) {
        auto statStart = std::chrono::steady_clock::now();
        for (auto& [key, value]: formulaManager.getProfilingStatistics()) {
            debugger.addInfo(key, value);
        }
        auto statEnd = std::chrono::steady_clock::now();
        const auto statsMs = std::chrono::duration_cast<std::chrono::milliseconds>(statEnd - statStart).count();
        debugger.addInfo("inc_regional_profile_collect_ms", std::to_string(statsMs));
    } else {
        for (auto& [key, value]: formulaManager.getProfilingStatistics()) {
            debugger.addInfo(key, value);
        }
    }
    if (incRegionalProfileEnabled) {
        auto insertEnd = std::chrono::steady_clock::now();
        const auto insertMs = std::chrono::duration_cast<std::chrono::milliseconds>(insertEnd - insertStart).count();
        debugger.addInfo("inc_regional_fc_insert_ms", std::to_string(insertMs));
    }
    debugger.logMessage(Level::INFO, "[inc-regional] pipeline finished; usedFallback="
        + std::string(orchestrator.getStats().usedFallback ? "true" : "false"));
    (void)deleteOverdeleteMs;
}
#endif //FORWARDCOMPILATION_H
