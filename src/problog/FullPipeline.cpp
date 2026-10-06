#include "souffle/problog/Pipeline.h"

#include "souffle/Derivation.h"
#include "souffle/problog/DerivationGraph.h"
#include "souffle/problog/ForwardCompilation.h"
#include "souffle/problog/GraphAnalyzer.h"
#include "souffle/problog/GraphRewriter.h"
#include "souffle/problog/ImplicitSplitRewrite.h"
#include "souffle/problog/LiftedWmc.h"
#include "souffle/problog/PipelineComponents.h"
#include "souffle/problog/QueryManager.h"
#include "souffle/problog/RuleManager.h"
#include "souffle/problog/debug/Debugger.h"
#include "souffle/problog/formula/CuddManager.h"
#ifdef SOUFFLE_HAVE_SDD
#include "souffle/problog/formula/SddManager.h"
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

// Build the probabilistic pipeline as one translation unit because
// probabilistic headers still define non-inline symbols. Compiling
// ImplicitSplitRewrite.cpp separately would duplicate those symbols in
// generated compiled-mode binaries.
#include "ImplicitSplitRewrite.cpp"

namespace souffle::problog::full_detail {

namespace {
bool exactInferenceMode = false;

static std::unordered_map<std::string, std::vector<char>> collectRelationAttributeTypes(
        SouffleProgram& program) {
    std::unordered_map<std::string, std::vector<char>> relationTypes;
    for (auto* rel : program.getAllRelations()) {
        std::vector<char> attrs;
        attrs.reserve(rel->getArity());
        for (std::size_t i = 0; i < rel->getArity(); ++i) {
            const char* attrType = rel->getAttrType(i);
            attrs.push_back((attrType != nullptr && attrType[0] != '\0') ? attrType[0] : '?');
        }
        relationTypes.emplace(rel->getName(), std::move(attrs));
    }
    return relationTypes;
}

static std::size_t countInitialInputFacts() {
    return inputFactSet.size();
}

struct RewriteDispatchDecision {
    bool useImplicit = false;
    std::string impl = "graph_rewrite";
    std::string reason = "no_probabilistic_rule_weights";
    std::string splitPolicy = "none";
    std::size_t totalRules = 0;
    std::size_t probabilisticRules = 0;
};

static RewriteDispatchDecision chooseRewriteDispatch(const CmdOptions& opt, const RuleManager& ruleManager) {
    RewriteDispatchDecision decision;
    for (const auto* rule : ruleManager.getAllRules()) {
        if (rule == nullptr) continue;
        ++decision.totalRules;
        if (std::abs(rule->getProbability() - 1.0) > 1e-12) {
            ++decision.probabilisticRules;
        }
    }

    if (opt.isGraphRewriteForced()) {
        decision.useImplicit = false;
        decision.impl = "graph_rewrite";
        decision.reason = "compat_graph_rewrite_alias";
        decision.splitPolicy = "none";
        return decision;
    }
    if (opt.isImplicitRewriteForced()) {
        decision.useImplicit = true;
        decision.impl = "implicit_split";
        decision.reason = "compat_implicit_rewrite_alias";
        decision.splitPolicy = "local";
        return decision;
    }
    if (decision.probabilisticRules > 0) {
        decision.useImplicit = true;
        decision.impl = "implicit_split";
        decision.reason = "probabilistic_rule_weights";
        decision.splitPolicy = "local";
    }
    return decision;
}

static std::size_t estimateBddVarCount(const SubgraphView& view) {
    auto isSemanticRandomProb = [](double p) {
        return p > 0.0 && p < 1.0;
    };
    std::size_t count = 0;
    for (const auto& node : view.getNodes()) {
        if (node->isFact && isSemanticRandomProb(node->getProbability())) {
            ++count;
        }
    }
    for (const auto& edge : view.getEdges()) {
        if (isSemanticRandomProb(edge->getProbability())) {
            ++count;
        }
    }
    return count;
}

static WorkingSubgraphView buildWorkingViewLocal(WorkingDerivationGraph& graph) {
    return WorkingSubgraphView(graph.getNodes(), graph.getEdges());
}

static WorkingSubgraphView buildWorkingViewLocal(
        const std::unordered_set<NodePtr>& nodes, const std::unordered_set<EdgePtr>& edges) {
    return WorkingSubgraphView(nodes, edges);
}

static std::size_t precomputeIsolatedOutputFactsLocal(SubgraphView& view) {
    std::size_t count = 0;
    for (const auto& node : view.getNodes()) {
        if (!node || !node->needOutput || !node->isFact || node->hasEvidence()) {
            continue;
        }
        if (!view.getIncomingEdges(node).empty() || !view.getOutgoingEdges(node).empty()) {
            continue;
        }
        if (precomputedProbResult.count(node)) {
            continue;
        }
        precomputedProbResult[node] = node->getProbability();
        node->needOutput = false;
        node->isQuery = false;
        ++count;
    }
    return count;
}

static std::size_t sweepIsolatedNonOutputNodesLocal(WorkingSubgraphView& view) {
    std::vector<NodePtr> candidates;
    candidates.reserve(view.getNodes().size());
    for (const auto& node : view.getNodes()) {
        if (node) {
            candidates.push_back(node);
        }
    }
    auto& nodes = view.mutableNodes();
    std::size_t removed = 0;
    for (const auto& node : candidates) {
        if (!node || node->needOutput || node->hasEvidence()) {
            continue;
        }
        if (!view.getIncomingEdges(node).empty() || !view.getOutgoingEdges(node).empty()) {
            continue;
        }
        removed += nodes.erase(node);
    }
    if (removed > 0) {
        view.invalidateCaches();
    }
    return removed;
}

static bool canUseImplicitOverlayCommit(const ImplicitSplitPipelineResult& result) {
    if (!result.needsResidualGraph) {
        return false;
    }
    return result.materialized.graph == nullptr &&
            (!result.factCommits.empty() || !result.edgeCommits.empty() ||
                    !result.inactiveBaseEdges.empty());
}

struct ImplicitOverlayCommitSummary {
    std::size_t factNodes = 0;
    std::size_t removedEdges = 0;
    std::size_t addedEdges = 0;
    std::size_t removedNodes = 0;
};

static ImplicitOverlayCommitSummary applyImplicitOverlayCommit(
        WorkingDerivationGraph& graph, WorkingSubgraphView& view, const ImplicitSplitPipelineResult& result) {
    ImplicitOverlayCommitSummary summary;
    precomputedProbResult.clear();
    precomputedTupleProbResult.clear();
    for (const auto& [tupleStr, prob] : result.carriedPrecomputedTupleProbs) {
        precomputedTupleProbResult.emplace(tupleStr, prob);
    }

    for (const auto& commit : result.factCommits) {
        if (!commit.node) {
            continue;
        }
        commit.node->isFact = commit.isFact;
        commit.node->setProbability(commit.probability);
        commit.node->setProbabilisticSupportTokens(commit.supportTokens);
        ++summary.factNodes;
    }

    std::unordered_map<SplitNodeRef, NodePtr, SplitNodeRefHash> shadowAliasNodes;
    shadowAliasNodes.reserve(result.stats.activeAliasRefs);
    auto ensureCommittedInputNode = [&](const SplitNodeRef& ref) -> NodePtr {
        if (ref.alias == 0) {
            return ref.base;
        }
        auto it = shadowAliasNodes.find(ref);
        if (it != shadowAliasNodes.end()) {
            return it->second;
        }
        if (!ref.base) {
            throw std::runtime_error("implicit overlay commit encountered null aliased input");
        }
        UntypedTuple shadowTuple = ref.base->getTuple();
        shadowTuple.relation_name = std::string("_split_shadow_") +
                std::to_string(ref.base->getId()) + "_alias" + std::to_string(ref.alias);
        NodePtr shadow = graph.createNode(shadowTuple, ref.base->getProbability());
        shadow->isFact = true;
        shadow->isShadow = true;
        shadow->setOriginalFact(ref.base->isOriginalFactNode());
        shadow->setProbability(ref.base->getProbability());
        shadow->setProbabilisticSupportTokens(ref.base->getProbabilisticSupportTokens());
        shadow->setSemanticFactId(ref.base->getSemanticFactId());
        view.mutableNodes().insert(shadow);
        shadowAliasNodes.emplace(ref, shadow);
        return shadow;
    };

    auto& edges = view.mutableEdges();
    for (const auto& edge : result.inactiveBaseEdges) {
        if (!edge) {
            continue;
        }
        summary.removedEdges += edges.erase(edge);
    }
    for (const auto& commit : result.edgeCommits) {
        if (commit.baseEdge) {
            summary.removedEdges += edges.erase(commit.baseEdge);
        }
        if (!commit.output) {
            continue;
        }
        std::vector<NodePtr> inputs;
        inputs.reserve(commit.inputs.size());
        for (const auto& input : commit.inputs) {
            NodePtr committedInput = ensureCommittedInputNode(input);
            if (!committedInput) {
                throw std::runtime_error("implicit overlay commit failed to map edge input");
            }
            inputs.push_back(committedInput);
        }
        EdgePtr newEdge = graph.createHyperedge(inputs, commit.output, nullptr, commit.negations);
        if (!newEdge) {
            throw std::runtime_error("implicit overlay commit failed to create hyperedge");
        }
        newEdge->setProbability(commit.probability);
        newEdge->setProbabilisticSupportTokens(commit.supportTokens);
        edges.insert(newEdge);
        ++summary.addedEdges;
    }

    view.invalidateCaches();
    summary.removedNodes = sweepIsolatedNonOutputNodesLocal(view);
    return summary;
}

static std::pair<std::size_t, std::size_t> recoverImplicitOutputFactsLocal(WorkingDerivationGraph& graph,
        WorkingSubgraphView& view, const std::unordered_set<UntypedTuple>& originalOutputTuples,
        const std::unordered_set<std::string>& originalOutputRelations) {
    std::unordered_set<UntypedTuple> liveViewTuples;
    liveViewTuples.reserve(view.getNodes().size());
    for (const auto& node : view.getNodes()) {
        if (node) {
            liveViewTuples.insert(node->getTuple());
        }
    }

    std::unordered_set<UntypedTuple> precomputedTuples;
    std::unordered_set<std::string> precomputedTupleStrings;
    auto rebuildPrecomputedSets = [&]() {
        precomputedTuples.clear();
        precomputedTupleStrings.clear();
        precomputedTuples.reserve(precomputedProbResult.size() + precomputedTupleProbResult.size());
        precomputedTupleStrings.reserve(precomputedProbResult.size() + precomputedTupleProbResult.size());
        for (const auto& [node, _] : precomputedProbResult) {
            if (node) {
                precomputedTuples.insert(node->getTuple());
                precomputedTupleStrings.insert(node->getTuple().toString());
            }
        }
        for (const auto& [tupleStr, _] : precomputedTupleProbResult) {
            precomputedTupleStrings.insert(tupleStr);
        }
    };
    rebuildPrecomputedSets();

    for (const auto& node : view.getNodes()) {
        if (node && originalOutputRelations.count(node->getTuple().relation_name) &&
                !precomputedTuples.count(node->getTuple()) &&
                !precomputedTupleStrings.count(node->getTuple().toString())) {
            node->setQuery();
        }
    }

    const std::size_t recoveredIsolatedFacts = precomputeIsolatedOutputFactsLocal(view);
    if (recoveredIsolatedFacts > 0) {
        rebuildPrecomputedSets();
    }

    std::size_t recoveredOutputFacts = 0;
    for (const auto& tuple : originalOutputTuples) {
        if (liveViewTuples.count(tuple) || precomputedTuples.count(tuple) ||
                precomputedTupleStrings.count(tuple.toString())) {
            continue;
        }
        NodePtr recovered = graph.findNode(tuple);
        if (!recovered || !recovered->isFact) {
            continue;
        }
        precomputedTupleProbResult.emplace(tuple.toString(), recovered->getProbability());
        ++recoveredOutputFacts;
    }

    return {recoveredIsolatedFacts, recoveredOutputFacts};
}

struct GraphSummary {
    std::size_t nodes = 0;
    std::size_t edges = 0;
    std::size_t factNodes = 0;
    std::size_t derivedNodes = 0;
    std::size_t queryNodes = 0;
    std::size_t outputNodes = 0;
    std::size_t evidenceNodes = 0;
    std::size_t shadowNodes = 0;
    std::size_t probabilisticFactNodes = 0;
    std::size_t probabilisticEdges = 0;
    std::size_t randomVariables = 0;
    std::size_t disjunctionNodes = 0;
    std::size_t maxInDegree = 0;
    std::size_t maxOutDegree = 0;
    std::size_t maxHyperedgeInputs = 0;
};

static GraphSummary summarizeGraphLight(const DerivationGraphViewInterface& view) {
    GraphSummary s;
    const auto& nodes = view.getNodes();
    const auto& edges = view.getEdges();
    s.nodes = nodes.size();
    s.edges = edges.size();

    std::unordered_map<NodePtr, std::size_t> indeg;
    std::unordered_map<NodePtr, std::size_t> outdeg;
    indeg.reserve(nodes.size());
    outdeg.reserve(nodes.size());
    for (const auto& n : nodes) {
        indeg.emplace(n, 0);
        outdeg.emplace(n, 0);
    }

    for (const auto& e : edges) {
        const auto inputs = view.getInputs(e);
        s.maxHyperedgeInputs = std::max(s.maxHyperedgeInputs, inputs.size());
        NodePtr out = view.getOutput(e);
        if (out) {
            auto it = indeg.find(out);
            if (it != indeg.end()) {
                ++it->second;
            }
        }
        for (const auto& in : inputs) {
            auto it = outdeg.find(in);
            if (it != outdeg.end()) {
                ++it->second;
            }
        }
        if (!e->isDeterministic()) {
            ++s.probabilisticEdges;
        }
    }

    for (const auto& n : nodes) {
        if (n->isFact) {
            ++s.factNodes;
            if (n->getProbability() < 1.0) {
                ++s.probabilisticFactNodes;
            }
        } else {
            ++s.derivedNodes;
        }
        if (n->isQuery) {
            ++s.queryNodes;
        }
        if (n->needOutput) {
            ++s.outputNodes;
        }
        if (n->hasEvidence()) {
            ++s.evidenceNodes;
        }
        if (n->isShadow) {
            ++s.shadowNodes;
        }
        const auto inIt = indeg.find(n);
        const auto outIt = outdeg.find(n);
        const std::size_t in = (inIt == indeg.end()) ? 0 : inIt->second;
        const std::size_t out = (outIt == outdeg.end()) ? 0 : outIt->second;
        s.maxInDegree = std::max(s.maxInDegree, in);
        s.maxOutDegree = std::max(s.maxOutDegree, out);
        if ((!n->isFact && in > 1) || (n->isFact && in > 0)) {
            ++s.disjunctionNodes;
        }
    }
    s.randomVariables = s.probabilisticFactNodes + s.probabilisticEdges;
    return s;
}

static void addGraphSummaryInfo(
        Debugger& debugger, const std::string& prefix, const GraphSummary& s) {
    auto add = [&](const std::string& key, const std::size_t value) {
        debugger.addInfo(prefix + key, std::to_string(value));
    };
    add("nodes", s.nodes);
    add("edges", s.edges);
    add("fact_nodes", s.factNodes);
    add("derived_nodes", s.derivedNodes);
    add("query_nodes", s.queryNodes);
    add("output_nodes", s.outputNodes);
    add("evidence_nodes", s.evidenceNodes);
    add("shadow_nodes", s.shadowNodes);
    add("prob_fact_nodes", s.probabilisticFactNodes);
    add("prob_rule_edges", s.probabilisticEdges);
    add("random_variables", s.randomVariables);
    add("disjunction_nodes", s.disjunctionNodes);
    add("max_in_degree", s.maxInDegree);
    add("max_out_degree", s.maxOutDegree);
    add("max_hyperedge_inputs", s.maxHyperedgeInputs);
}

static void recordFcHeartbeat(
        Debugger& debugger,
        StageInfo* stage,
        const FcHeartbeatSnapshot& hb,
        const std::string& mode,
        std::size_t slowDone = 0,
        std::size_t slowTotal = 0,
        std::size_t componentId = std::numeric_limits<std::size_t>::max(),
        std::size_t liveNodes = 0) {
    debugger.addInfo("fc_heartbeat_mode", mode);
    debugger.addInfo("fc_heartbeat_elapsed_ms", std::to_string(hb.elapsedMs));
    debugger.addInfo("fc_heartbeat_round", std::to_string(hb.round));
    debugger.addInfo("fc_heartbeat_cycle_id", std::to_string(hb.currentCycleId));
    debugger.addInfo("fc_heartbeat_cycles_done", std::to_string(hb.completedCycles));
    debugger.addInfo("fc_heartbeat_cycles_total", std::to_string(hb.totalCycles));
    debugger.addInfo("fc_heartbeat_worklist_size", std::to_string(hb.worklistSize));
    debugger.addInfo("fc_heartbeat_ready_queue_size", std::to_string(hb.readyQueueSize));
    debugger.addInfo("fc_heartbeat_node_formulas", std::to_string(hb.nodeFormulaCount));
    debugger.addInfo("fc_heartbeat_edge_formulas", std::to_string(hb.edgeFormulaCount));
    debugger.addInfo("fc_heartbeat_live_nodes", std::to_string(liveNodes));
    if (slowTotal > 0) {
        debugger.addInfo("fc_heartbeat_slow_components_done", std::to_string(slowDone));
        debugger.addInfo("fc_heartbeat_slow_components_total", std::to_string(slowTotal));
    }
    if (componentId != std::numeric_limits<std::size_t>::max()) {
        debugger.addInfo("fc_heartbeat_component_id", std::to_string(componentId));
    }
    if (stage) {
        std::string msg = "heartbeat mode=" + mode + " elapsed_ms=" + std::to_string(hb.elapsedMs) +
                " round=" + std::to_string(hb.round) + " cycle=" + std::to_string(hb.currentCycleId) +
                " cycles=" + std::to_string(hb.completedCycles) + "/" + std::to_string(hb.totalCycles) +
                " worklist=" + std::to_string(hb.worklistSize) +
                " ready=" + std::to_string(hb.readyQueueSize) +
                " node_formulas=" + std::to_string(hb.nodeFormulaCount) +
                " edge_formulas=" + std::to_string(hb.edgeFormulaCount) +
                " live_nodes=" + std::to_string(liveNodes);
        if (componentId != std::numeric_limits<std::size_t>::max()) {
            msg += " component=" + std::to_string(componentId);
        }
        if (slowTotal > 0) {
            msg += " slow_components=" + std::to_string(slowDone) + "/" + std::to_string(slowTotal);
        }
        stage->logMessage(Level::INFO, msg);
    }
    debugger.dumpReportJsonToFile();
}
static WeightedBDDManager::InitConfig makeCuddInitConfig(std::size_t varCount) {
    WeightedBDDManager::InitConfig cfg;
    const auto maxVars = std::numeric_limits<unsigned int>::max();
    const auto doubledVars = varCount > maxVars / 2 ? maxVars : static_cast<unsigned int>(varCount * 2);
    cfg.numVars = doubledVars;
    cfg.numSlots = 512;
    // Smaller graphs downscale cache/memory; large graphs keep the default (largest) config.
    if (varCount <= 256) {
        cfg.cacheSize = 1u << 18;
        cfg.maxMemory = 1UL << 30;
    } else if (varCount <= 1024) {
        cfg.cacheSize = 1u << 20;
        cfg.maxMemory = 4UL << 30;
    } else if (varCount <= 4096) {
        cfg.cacheSize = 1u << 22;
        cfg.maxMemory = 8UL << 30;
    } else if (varCount > 10000) {
        cfg.cacheSize = 1u << 26;
    }
    return cfg;
}

static std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += sep;
        }
        out += parts[i];
    }
    return out;
}

} // namespace

void setExactInferenceMode(bool enabled) {
    exactInferenceMode = enabled;
}

bool isExactInferenceMode() {
    return exactInferenceMode;
}

std::string makeOutputPath(const CmdOptions& opt, const std::string& filename) {
    const std::string& dir = opt.getOutputFileDir();
    if (dir.empty()) {
        return filename;
    }
    if (dir.back() == '/') {
        return dir + filename;
    }
    return dir + "/" + filename;
}

static std::vector<std::pair<NodePtr, bool>> applyEvidence(
        DerivationGraph& graph,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences) {
    std::vector<std::pair<NodePtr, bool>> resolved;
    resolved.reserve(evidences.size());

    for (const auto& [tup, val] : evidences) {
        NodePtr node = graph.findNode(tup);
        if (!node) {
            throw std::runtime_error("Evidence " + tup.toString() + " is not found in the graph.");
        }

        resolved.emplace_back(node, val);
    }
    return resolved;
}

static void runBddPipeline(
        const CmdOptions& opt,
        SouffleProgram& /*program*/,
        RuleManager& /*ruleManager*/,
        QueryManager& /*queryManager*/,
        DerivationGraph& graph,
        SubgraphView& view,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences,
        StageInfo* rewriteHybridStage) {
    Debugger& debugger = Debugger::getInstance();

    std::map<NodePtr, BddNodeRef> nodeFormulas;
    std::map<EdgePtr, BddNodeRef> edgeFormulas;
    std::unique_ptr<WeightedBDDManager> bddManager;
    const bool computeProbabilities = !opt.isDerivationOnly();
    std::unordered_set<NodePtr> seedTrueNodes;
    seedTrueNodes.reserve(view.getNodes().size());
    for (const auto& n : view.getNodes()) {
        const std::string s = n->getTuple().toString();
        if (s.rfind("@magic.", 0) != 0) continue;

        if (view.getIncomingEdges(n).empty()) {
            seedTrueNodes.insert(n);
        }
    }
    //std::cout << "[dbg] seedTrueNodes size = " << seedTrueNodes.size() << "\

    if (computeProbabilities) {
        if (opt.isRewriteEnabled()) {
            auto* hybridStage = rewriteHybridStage;
            if (!hybridStage) {
                hybridStage = debugger.startStage(StageKind::FC_WMC_HYBRID);
            }
            auto varEstimate = estimateBddVarCount(view);
            debugger.addInfo("rand_vars", std::to_string(varEstimate));
            if (hybridStage) {
                hybridStage->logMessage(Level::INFO, "rand_vars=" + std::to_string(varEstimate));
            }

            const bool enableFast = opt.isSingleRandFastEnabled();
            auto componentsBuildStart = std::chrono::steady_clock::now();
            auto components = buildComponentSubgraphs(view);
            auto componentsBuildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - componentsBuildStart)
                                             .count();
            auto analysesStart = std::chrono::steady_clock::now();
            auto analyses = analyzeComponents(view, std::move(components), enableFast);
            auto analysesMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - analysesStart)
                                      .count();
            long long initMsTotal = 0;
            long long initMsMax = 0;
            long long buildMs = 0;
            WeightedBDDManager::InitConfig initConfig;
            auto t2 = std::chrono::steady_clock::now();
            auto resolvedEvs = applyEvidence(graph, evidences);
            auto t3 = std::chrono::steady_clock::now();
            auto evidenceApplyMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
            auto evidenceGroupStart = std::chrono::steady_clock::now();
            auto evidencesByComponent = groupEvidencesByComponent(analyses, resolvedEvs);
            auto evidenceGroupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - evidenceGroupStart)
                                           .count();
            long long evidenceBuildMs = 0;
            long long evidenceWmcMs = 0;
            long long perNodeWmcMs = 0;
            long long fastPathMs = 0;
            long long liveNodesSum = 0;
            const bool wmcProfile = opt.isWmcProfileEnabled();
            using Clock = std::chrono::steady_clock;
            auto toMs = [](Clock::time_point start) {
                return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            };
            double evidenceMakeAndMs = 0.0;
            double evidenceWmcComputeMs = 0.0;
            double nodeMakeAndMs = 0.0;
            double nodeWmcComputeMs = 0.0;
            std::size_t evidenceWmcCalls = 0;
            std::size_t nodeWmcCalls = 0;
            std::size_t evidenceMakeAndCalls = 0;
            std::size_t nodeMakeAndCalls = 0;
            auto makeAndProfile = [&](const BddNodeRef& lhs, const BddNodeRef& rhs,
                                      double& ms, std::size_t& calls) {
                if (!wmcProfile) {
                    return bddManager->makeAnd(lhs, rhs);
                }
                auto andStart = Clock::now();
                auto res = bddManager->makeAnd(lhs, rhs);
                ms += toMs(andStart);
                calls++;
                return res;
            };
            auto computeWmcProfile = [&](const BddNodeRef& node, double& ms, std::size_t& calls,
                                         bool logarithmic = false) {
                calls++;
                if (!wmcProfile) {
                    return logarithmic ? bddManager->computeLogWeightedModelCount(node) :
                            bddManager->computeWeightedModelCount(node);
                }
                auto wmcStart = Clock::now();
                double res = logarithmic ? bddManager->computeLogWeightedModelCount(node) :
                        bddManager->computeWeightedModelCount(node);
                ms += toMs(wmcStart);
                return res;
            };

            FastComponentStats fastStats;
            ConjFastStats conjStats;
            std::vector<FastComponentEval> fastComponents;
            std::vector<ConjComponentEval> conjComponents;
            std::vector<SlowComponentEval> slowEvals;
            const bool logFastReasons = opt.isDumpDotEnabled();
            const bool logComponentDetails = opt.isFcProfileEnabled();
            std::vector<ComponentDecision> decisions;
            decisions.reserve(analyses.size());

            probResult.clear();
            double componentFastEvalTotalMs = 0.0;
            auto classifyStart = std::chrono::steady_clock::now();
            for (auto& analysis : analyses) {
                const auto& comp = analysis.comp;
                const auto& compEvs = evidencesByComponent[comp.id];
                const bool hasEvidence = !compEvs.empty();
                const bool singleCandidate = analysis.randVars == 1;
                const bool conjCandidate = !singleCandidate && !analysis.hasNegation &&
                        !analysis.hasOr && !analysis.hasCycle;  // fast path excludes cycles/OR/negation
                ComponentDecision decision{
                        comp.id,
                        comp.nodes.size(),
                        comp.edges.size(),
                        analysis.randVars,
                        hasEvidence,
                        analysis.hasNegation,
                        analysis.hasOr,
                        analysis.hasCycle,
                        "",
                        "",
                };

                if (enableFast) {
                    if (singleCandidate) fastStats.candidates++;
                    if (conjCandidate) conjStats.candidates++;
                }
                if (logFastReasons) {
                    std::vector<std::string> singleReasons;
                    if (!enableFast) singleReasons.emplace_back("fast_disabled");
                    if (!singleCandidate) singleReasons.emplace_back("randvars!=1");
                    if (hasEvidence) singleReasons.emplace_back("has_evidence");
                    if (!singleReasons.empty()) {
                        std::cout << "[fc-component] id=" << comp.id
                                  << " single_skip=" << join(singleReasons, ",")
                                  << " rand_vars=" << analysis.randVars
                                  << " has_negation=" << analysis.hasNegation
                                  << " has_or=" << analysis.hasOr
                                  << " has_cycle=" << analysis.hasCycle
                                  << std::endl;
                    }
                    std::vector<std::string> conjReasons;
                    if (!enableFast) conjReasons.emplace_back("fast_disabled");
                    if (singleCandidate) conjReasons.emplace_back("single_randvar");
                    if (analysis.hasNegation) conjReasons.emplace_back("negation");
                    if (analysis.hasOr) conjReasons.emplace_back("or");
                    if (analysis.hasCycle) conjReasons.emplace_back("cycle");
                    if (hasEvidence) conjReasons.emplace_back("has_evidence");
                    if (!conjReasons.empty()) {
                        std::cout << "[fc-component] id=" << comp.id
                                  << " conj_skip=" << join(conjReasons, ",")
                                  << " rand_vars=" << analysis.randVars
                                  << " has_negation=" << analysis.hasNegation
                                  << " has_or=" << analysis.hasOr
                                  << " has_cycle=" << analysis.hasCycle
                                  << std::endl;
                    }
                }

                if (!enableFast || hasEvidence) {
                    if (enableFast && singleCandidate) fastStats.skipped++;
                    if (enableFast && conjCandidate) conjStats.skipped++;
                    decision.mode = "slow";
                    decision.reason = !enableFast ? "fast_disabled" : "has_evidence";
                    decisions.push_back(decision);
                    SlowComponentEval slow;
                    slow.randVars = analysis.randVars;
                    slow.comp = std::move(analysis.comp);
                    slowEvals.push_back(std::move(slow));
                    continue;
                }

                if (singleCandidate) {
                    FastComponentEval eval;
                    eval.var = analysis.singleRand;
                    auto fastEvalStart = std::chrono::steady_clock::now();
                    const bool evalOk = evaluateSingleRandComponentBoth(
                            analysis.comp, eval.var, eval.valuesFalse, eval.valuesTrue, &eval.evalMsTrue);
                    componentFastEvalTotalMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - fastEvalStart).count();
                    if (!evalOk) {
                        eval.evalMsFalse = 0;
                        if (logFastReasons) {
                            std::cout << "[fc-component] id=" << comp.id
                                      << " single_skip=eval_failed"
                                      << " rand_vars=" << analysis.randVars
                                      << std::endl;
                        }
                        fastStats.skipped++;
                        decision.mode = "slow";
                        decision.reason = "eval_failed";
                        decisions.push_back(decision);
                        SlowComponentEval slow;
                        slow.randVars = analysis.randVars;
                        slow.comp = std::move(analysis.comp);
                        slowEvals.push_back(std::move(slow));
                        continue;
                    }
                    eval.comp = std::move(analysis.comp);
                    fastStats.used++;
                    fastStats.evalMs += eval.evalMsTrue + eval.evalMsFalse;
                    decision.mode = "fast_single";
                    decisions.push_back(decision);
                    if (logComponentDetails) {
                        std::cout << "[fc-component] id=" << eval.comp.id
                                  << " fast_path=1"
                                  << " nodes=" << eval.comp.nodes.size()
                                  << " edges=" << eval.comp.edges.size()
                                  << " rand_vars=" << analysis.randVars
                                  << " eval_ms_true=" << eval.evalMsTrue
                                  << " eval_ms_false=" << eval.evalMsFalse
                                  << " total_ms=" << (eval.evalMsTrue + eval.evalMsFalse)
                                  << std::endl;
                    }
                    if (hybridStage && logComponentDetails) {
                        hybridStage->logMessage(Level::INFO,
                                "component id=" + std::to_string(eval.comp.id) +
                                        " fast_path=single" +
                                        " nodes=" + std::to_string(eval.comp.nodes.size()) +
                                        " edges=" + std::to_string(eval.comp.edges.size()) +
                                        " rand_vars=" + std::to_string(analysis.randVars) +
                                        " eval_ms_true=" + std::to_string(eval.evalMsTrue) +
                                        " eval_ms_false=" + std::to_string(eval.evalMsFalse) +
                                        " total_ms=" + std::to_string(eval.evalMsTrue + eval.evalMsFalse));
                    }
                    fastComponents.push_back(std::move(eval));
                    continue;
                }

                if (conjCandidate) {
                    ConjComponentEval eval;
                    const bool zeroRandConj = (analysis.randVars == 0);
                    auto fastEvalStart = std::chrono::steady_clock::now();
                    const bool conjOk = zeroRandConj
                            ? evaluateZeroRandConjComponent(analysis.comp, eval.probabilities, &eval.evalMs)
                            : evaluateConjComponent(analysis.comp, eval.probabilities, &eval.evalMs);
                    componentFastEvalTotalMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - fastEvalStart).count();
                    if (!conjOk) {
                        if (logFastReasons) {
                            std::cout << "[fc-component] id=" << comp.id
                                      << " conj_skip=eval_failed"
                                      << " rand_vars=" << analysis.randVars
                                      << std::endl;
                        }
                        conjStats.skipped++;
                        decision.mode = "slow";
                        decision.reason = "eval_failed";
                        decisions.push_back(decision);
                        SlowComponentEval slow;
                        slow.randVars = analysis.randVars;
                        slow.comp = std::move(analysis.comp);
                        slowEvals.push_back(std::move(slow));
                        continue;
                    }
                    eval.comp = std::move(analysis.comp);
                    conjStats.used++;
                    conjStats.evalMs += eval.evalMs;
                    decision.mode = "fast_conj";
                    decisions.push_back(decision);
                    if (logComponentDetails) {
                        std::cout << "[fc-component] id=" << eval.comp.id
                                  << " fast_path=conj"
                                  << " nodes=" << eval.comp.nodes.size()
                                  << " edges=" << eval.comp.edges.size()
                                  << " rand_vars=" << analysis.randVars
                                  << " eval_ms=" << eval.evalMs
                                  << " total_ms=" << eval.evalMs
                                  << std::endl;
                    }
                    if (hybridStage && logComponentDetails) {
                        hybridStage->logMessage(Level::INFO,
                                "component id=" + std::to_string(eval.comp.id) +
                                        " fast_path=conj" +
                                        " nodes=" + std::to_string(eval.comp.nodes.size()) +
                                        " edges=" + std::to_string(eval.comp.edges.size()) +
                                        " rand_vars=" + std::to_string(analysis.randVars) +
                                        " eval_ms=" + std::to_string(eval.evalMs));
                    }
                    conjComponents.push_back(std::move(eval));
                    continue;
                }

                SlowComponentEval slow;
                slow.randVars = analysis.randVars;
                decision.mode = "slow";
                if (analysis.hasOr) {
                    decision.reason = "or";
                } else if (analysis.hasNegation) {
                    decision.reason = "negation";
                } else if (analysis.hasCycle) {
                    decision.reason = "cycle";
                } else {
                    decision.reason = "other";
                }
                decisions.push_back(decision);
                slow.comp = std::move(analysis.comp);
                slowEvals.push_back(std::move(slow));
            }
            auto classifyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - classifyStart)
                                      .count();
            const double componentClassifyPureMs =
                    std::max(0.0, static_cast<double>(classifyMs) - componentFastEvalTotalMs);

            std::sort(slowEvals.begin(), slowEvals.end(),
                    [](const SlowComponentEval& a, const SlowComponentEval& b) {
                        return a.randVars > b.randVars;
                    });

            std::size_t maxRandVars = 0;
            for (const auto& slow : slowEvals) {
                maxRandVars = std::max(maxRandVars, slow.randVars);
            }
            if (!slowEvals.empty()) {
                initConfig = makeCuddInitConfig(maxRandVars);
                auto initStart = std::chrono::steady_clock::now();
                bddManager = std::make_unique<WeightedBDDManager>(initConfig);
                initMsTotal = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - initStart)
                                      .count();
                initMsMax = initMsTotal;
            } else {
                initConfig.numVars = 0;
                initConfig.numVarsZ = 0;
                initConfig.numSlots = 0;
                initConfig.cacheSize = 0;
                initConfig.maxMemory = 0;
            }

            debugger.addInfo("manager_init_vars", std::to_string(initConfig.numVars));
            debugger.addInfo("manager_init_slots", std::to_string(initConfig.numSlots));
            debugger.addInfo("manager_init_cache", std::to_string(initConfig.cacheSize));
            debugger.addInfo("manager_init_maxmem_mb",
                    std::to_string(initConfig.maxMemory / (1024UL * 1024UL)));
            debugger.addInfo("manager_init_ms", std::to_string(initMsTotal));
            debugger.addInfo("manager_init_ms_max", std::to_string(initMsMax));
            debugger.addInfo("manager_init_components", std::to_string(slowEvals.empty() ? 0 : 1));
            if (hybridStage) {
                hybridStage->logMessage(Level::INFO, "manager_init_vars=" + std::to_string(initConfig.numVars));
                hybridStage->logMessage(Level::INFO, "manager_init_slots=" + std::to_string(initConfig.numSlots));
                hybridStage->logMessage(Level::INFO, "manager_init_cache=" + std::to_string(initConfig.cacheSize));
                hybridStage->logMessage(Level::INFO, "manager_init_maxmem_mb=" +
                        std::to_string(initConfig.maxMemory / (1024UL * 1024UL)));
                hybridStage->logMessage(Level::INFO, "manager_init_ms=" + std::to_string(initMsTotal));
                hybridStage->logMessage(Level::INFO, "manager_init_ms_max=" + std::to_string(initMsMax));
                hybridStage->logMessage(Level::INFO, "manager_init_components=" +
                        std::to_string(slowEvals.empty() ? 0 : 1));
            }

            debugger.addInfo("slow_components", std::to_string(slowEvals.size()));
            debugger.addInfo("component_build_subgraphs_ms", std::to_string(componentsBuildMs));
            debugger.addInfo("component_analyze_ms", std::to_string(analysesMs));
            debugger.addInfo("evidence_apply_ms", std::to_string(evidenceApplyMs));
            debugger.addInfo("evidence_group_ms", std::to_string(evidenceGroupMs));
            debugger.addInfo("component_classify_ms", std::to_string(classifyMs));
            debugger.addInfo("component_classify_pure_ms", std::to_string(componentClassifyPureMs));
            debugger.addInfo("component_fast_eval_total_ms", std::to_string(componentFastEvalTotalMs));
            debugger.addInfo("fastpath_components", std::to_string(fastStats.used));
            debugger.addInfo("fastpath_candidates", std::to_string(fastStats.candidates));
            debugger.addInfo("fastpath_skipped", std::to_string(fastStats.skipped));
            debugger.addInfo("fastpath_eval_ms", std::to_string(fastStats.evalMs));
            debugger.addInfo("fastpath_conj_components", std::to_string(conjStats.used));
            debugger.addInfo("fastpath_conj_candidates", std::to_string(conjStats.candidates));
            debugger.addInfo("fastpath_conj_skipped", std::to_string(conjStats.skipped));
            debugger.addInfo("fastpath_conj_eval_ms", std::to_string(conjStats.evalMs));

            if (hybridStage) {
                hybridStage->logMessage(Level::INFO, "component_build_subgraphs_ms=" +
                        std::to_string(componentsBuildMs));
                hybridStage->logMessage(Level::INFO, "component_analyze_ms=" +
                        std::to_string(analysesMs));
                hybridStage->logMessage(Level::INFO, "evidence_apply_ms=" +
                        std::to_string(evidenceApplyMs));
                hybridStage->logMessage(Level::INFO, "evidence_group_ms=" +
                        std::to_string(evidenceGroupMs));
                hybridStage->logMessage(Level::INFO, "component_classify_ms=" +
                        std::to_string(classifyMs));
                hybridStage->logMessage(Level::INFO, "component_classify_pure_ms=" +
                        std::to_string(componentClassifyPureMs));
                hybridStage->logMessage(Level::INFO, "component_fast_eval_total_ms=" +
                        std::to_string(componentFastEvalTotalMs));
                hybridStage->logMessage(Level::INFO, "slow_components=" +
                        std::to_string(slowEvals.size()));
                hybridStage->logMessage(Level::INFO, "fastpath_components=" +
                        std::to_string(fastStats.used));
                hybridStage->logMessage(Level::INFO, "fastpath_candidates=" +
                        std::to_string(fastStats.candidates));
                hybridStage->logMessage(Level::INFO, "fastpath_skipped=" +
                        std::to_string(fastStats.skipped));
                hybridStage->logMessage(Level::INFO, "fastpath_eval_ms=" +
                        std::to_string(fastStats.evalMs));
                hybridStage->logMessage(Level::INFO, "fastpath_conj_components=" +
                        std::to_string(conjStats.used));
                hybridStage->logMessage(Level::INFO, "fastpath_conj_candidates=" +
                        std::to_string(conjStats.candidates));
                hybridStage->logMessage(Level::INFO, "fastpath_conj_skipped=" +
                        std::to_string(conjStats.skipped));
                hybridStage->logMessage(Level::INFO, "fastpath_conj_eval_ms=" +
                        std::to_string(conjStats.evalMs));
            }
            if (logFastReasons) {
                std::size_t fastSingle = 0;
                std::size_t fastConj = 0;
                std::size_t slowCount = 0;
                for (const auto& decision : decisions) {
                    if (decision.mode == "fast_single") {
                        fastSingle++;
                    } else if (decision.mode == "fast_conj") {
                        fastConj++;
                    } else {
                        slowCount++;
                    }
                }
                std::cout << "[fc-component-info] total=" << decisions.size()
                          << " fast_single=" << fastSingle
                          << " fast_conj=" << fastConj
                          << " slow=" << slowCount
                          << std::endl;
                for (const auto& decision : decisions) {
                    std::cout << "[fc-component-info] id=" << decision.id
                              << " nodes=" << decision.nodes
                              << " edges=" << decision.edges
                              << " rand_vars=" << decision.randVars
                              << " has_negation=" << decision.hasNegation
                              << " has_or=" << decision.hasOr
                              << " has_cycle=" << decision.hasCycle
                              << " has_evidence=" << decision.hasEvidence
                              << " mode=" << decision.mode
                              << " reason=" << decision.reason
                              << std::endl;
                }
            }

            if (!fastComponents.empty()) {
                for (const auto& fast : fastComponents) {
                    double p = fast.var.probability;
                    for (const auto& node : fast.comp.nodes) {
                        if (!node->needOutput) {
                            continue;
                        }
                        auto itTrue = fast.valuesTrue.find(node);
                        auto itFalse = fast.valuesFalse.find(node);
                        if (itTrue == fast.valuesTrue.end() && itFalse == fast.valuesFalse.end()) {
                            continue;
                        }
                        bool vTrue = (itTrue != fast.valuesTrue.end()) ? itTrue->second : false;
                        bool vFalse = (itFalse != fast.valuesFalse.end()) ? itFalse->second : false;
                        double numerator = (vTrue ? p : 0.0) + (vFalse ? (1.0 - p) : 0.0);
                        probResult[node] = numerator;
                    }
                }
            }
            if (!conjComponents.empty()) {
                for (const auto& conj : conjComponents) {
                    for (const auto& [node, prob] : conj.probabilities) {
                        if (!node->needOutput) {
                            continue;
                        }
                        probResult[node] = prob;
                    }
                }
            }
            fastPathMs = fastStats.evalMs + conjStats.evalMs;
            for (auto& slow : slowEvals) {
                if (!bddManager) {
                    throw std::runtime_error("Missing BDD manager for slow components.");
                }
                auto compId = slow.comp.id;
                auto nodeCount = slow.comp.nodes.size();
                auto edgeCount = slow.comp.edges.size();
                auto randVars = slow.randVars;
                auto compStart = std::chrono::steady_clock::now();

                bddManager->reset();
                std::map<NodePtr, BddNodeRef> compNodeFormulas;
                std::map<EdgePtr, BddNodeRef> compEdgeFormulas;
                SubgraphView subview(std::move(slow.comp.nodes), std::move(slow.comp.edges));

                auto buildStart = std::chrono::steady_clock::now();
                buildFormulasCyclewise(subview, *bddManager, compNodeFormulas, compEdgeFormulas);
                auto buildMsComp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - buildStart)
                                           .count();
                buildMs += buildMsComp;
                liveNodesSum += static_cast<long long>(
                        Cudd_ReadNodeCount(bddManager->getManager()));

                const auto& componentEvs = evidencesByComponent[compId];
                auto evidenceBuildStart = std::chrono::steady_clock::now();
                auto evidenceBdd = bddManager->getTrue();
                for (const auto& [eNode, val] : componentEvs) {
                    auto it = compNodeFormulas.find(eNode);
                    if (it == compNodeFormulas.end()) {
                        throw std::runtime_error("Evidence node has no formula: " +
                                eNode->getTuple().toString());
                    }
                    auto lit = it->second;
                    if (!val) {
                        lit = bddManager->makeNot(lit);
                    }
                    evidenceBdd = makeAndProfile(evidenceBdd, lit, evidenceMakeAndMs, evidenceMakeAndCalls);
                }
                auto evidenceBuildMsComp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now() - evidenceBuildStart)
                                                   .count();
                evidenceBuildMs += evidenceBuildMsComp;

                auto wmcStart = std::chrono::steady_clock::now();
                double evidenceLogWeight = 0.0;
                if (!componentEvs.empty()) {
                    evidenceLogWeight = computeWmcProfile(
                            evidenceBdd, evidenceWmcComputeMs, evidenceWmcCalls, true);
                    if (!std::isfinite(evidenceLogWeight)) {
                        throw std::runtime_error("Inconsistent evidence: observations have zero probability");
                    }
                }
                auto evidenceWmcMsComp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - wmcStart)
                                                 .count();
                evidenceWmcMs += evidenceWmcMsComp;

                auto perNodeStart = std::chrono::steady_clock::now();
                for (const auto& node : subview.getNodes()) {
                    if (!node->needOutput) {
                        continue;
                    }
                    auto it = compNodeFormulas.find(node);
                    if (it == compNodeFormulas.end()) {
                        continue;
                    }
                    const auto& bdd = it->second;
                    double prob = 0.0;
                    if (componentEvs.empty()) {
                        prob = computeWmcProfile(bdd, nodeWmcComputeMs, nodeWmcCalls);
                    } else {
                        auto joint = makeAndProfile(bdd, evidenceBdd, nodeMakeAndMs, nodeMakeAndCalls);
                        double jointLogWeight = computeWmcProfile(joint, nodeWmcComputeMs, nodeWmcCalls, true);
                        prob = std::exp(std::min(0.0, jointLogWeight - evidenceLogWeight));
                    }
                    probResult[node] = prob;
                }
                auto perNodeWmcMsComp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - perNodeStart)
                                                .count();
                perNodeWmcMs += perNodeWmcMsComp;

                auto compTotalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - compStart)
                                           .count();
                if (logComponentDetails) {
                    std::cout << "[fc-component] id=" << compId
                              << " fast_path=0"
                              << " nodes=" << nodeCount
                              << " edges=" << edgeCount
                              << " rand_vars=" << randVars
                              << " build_ms=" << buildMsComp
                              << " evidence_build_ms=" << evidenceBuildMsComp
                              << " evidence_wmc_ms=" << evidenceWmcMsComp
                              << " per_node_wmc_ms=" << perNodeWmcMsComp
                              << " total_ms=" << compTotalMs
                              << std::endl;
                }
            }
            for (const auto& [node, prob] : precomputedProbResult) {
                probResult.emplace(node, prob);
            }

            debugger.addInfo("live_nodes", std::to_string(liveNodesSum));
            debugger.addInfo("fc_build_ms", std::to_string(buildMs));
            debugger.addInfo("wmc_ms", std::to_string(evidenceBuildMs + evidenceWmcMs + perNodeWmcMs + fastPathMs));
            debugger.addInfo("evidence_build_ms", std::to_string(evidenceBuildMs));
            debugger.addInfo("evidence_wmc_ms", std::to_string(evidenceWmcMs));
            debugger.addInfo("per_node_wmc_ms", std::to_string(perNodeWmcMs));
            debugger.addInfo("fastpath_wmc_ms", std::to_string(fastPathMs));
            std::cout << "[pipeline] BDD formula build took " << buildMs << " ms\n";
            std::cout << "[pipeline] component subgraph build took " << componentsBuildMs << " ms\n";
            std::cout << "[pipeline] component analysis took " << analysesMs << " ms\n";
            std::cout << "[pipeline] evidence resolve/tag took " << evidenceApplyMs << " ms\n";
            std::cout << "[pipeline] evidence grouping took " << evidenceGroupMs << " ms\n";
            std::cout << "[pipeline] component classification took " << classifyMs
                      << " ms (pure=" << componentClassifyPureMs
                      << " ms, fast_eval=" << componentFastEvalTotalMs << " ms)\n";
            std::cout << "[pipeline] evidence BDD build took " << evidenceBuildMs << " ms\n";
            std::cout << "[pipeline] evidence WMC took " << evidenceWmcMs << " ms\n";
            std::cout << "[pipeline] per-node conditional WMC took " << perNodeWmcMs << " ms\n";
            if (fastStats.used > 0 || conjStats.used > 0) {
                std::cout << "[pipeline] fastpath WMC took " << fastPathMs << " ms\n";
            }
            if (wmcProfile) {
                std::cout << "[wmc-profile] stage=EXACT"
                          << " mode=rewrite"
                          << " total_ms=" << (evidenceBuildMs + evidenceWmcMs + perNodeWmcMs + fastPathMs)
                          << " components=" << analyses.size()
                          << " nodes=" << view.getNodes().size()
                          << " evidence_build_ms=" << evidenceBuildMs
                          << " evidence_make_and_calls=" << evidenceMakeAndCalls
                          << " evidence_make_and_ms=" << evidenceMakeAndMs
                          << " evidence_wmc_calls=" << evidenceWmcCalls
                          << " evidence_wmc_compute_ms=" << evidenceWmcComputeMs
                          << " node_make_and_calls=" << nodeMakeAndCalls
                          << " node_make_and_ms=" << nodeMakeAndMs
                          << " node_wmc_calls=" << nodeWmcCalls
                          << " node_wmc_compute_ms=" << nodeWmcComputeMs
                          << " fastpath_ms=" << fastPathMs
                          << " live_nodes=" << liveNodesSum
                          << std::endl;
            }

            debugger.endStage();
            debugger.startStage(StageKind::IO_DUMP);
            auto tDumpStart = std::chrono::steady_clock::now();
            dumpProbabilities(probResult, opt.getOutputFileDir());
            auto tDumpMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - tDumpStart)
                                   .count();
            std::cout << "[pipeline] probability dump took " << tDumpMs << " ms\n";
            debugger.endStage();
        } else {
            auto* fcStage = debugger.startStage(StageKind::FORWARD_COMPILATION);
            auto varEstimate = estimateBddVarCount(view);
            debugger.addInfo("rand_vars", std::to_string(varEstimate));
            if (fcStage) {
                fcStage->logMessage(Level::INFO, "rand_vars=" + std::to_string(varEstimate));
            }

            auto initConfig = makeCuddInitConfig(varEstimate);
            debugger.addInfo("manager_init_vars", std::to_string(initConfig.numVars));
            debugger.addInfo("manager_init_slots", std::to_string(initConfig.numSlots));
            debugger.addInfo("manager_init_cache", std::to_string(initConfig.cacheSize));
            debugger.addInfo("manager_init_maxmem_mb",
                    std::to_string(initConfig.maxMemory / (1024UL * 1024UL)));
            if (fcStage) {
                fcStage->logMessage(Level::INFO, "manager_init_vars=" + std::to_string(initConfig.numVars));
                fcStage->logMessage(Level::INFO, "manager_init_slots=" + std::to_string(initConfig.numSlots));
                fcStage->logMessage(Level::INFO, "manager_init_cache=" + std::to_string(initConfig.cacheSize));
                fcStage->logMessage(Level::INFO, "manager_init_maxmem_mb=" +
                        std::to_string(initConfig.maxMemory / (1024UL * 1024UL)));
            }
            auto initStart = std::chrono::steady_clock::now();
            bddManager = std::make_unique<WeightedBDDManager>(initConfig);
            auto initMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - initStart)
                                  .count();
            debugger.addInfo("manager_init_ms", std::to_string(initMs));
            if (fcStage) {
                fcStage->logMessage(Level::INFO, "manager_init_ms=" + std::to_string(initMs));
            }
            auto t0 = std::chrono::steady_clock::now();
            buildFormulasCyclewise(view, *bddManager, nodeFormulas, edgeFormulas, seedTrueNodes);
            auto t1 = std::chrono::steady_clock::now();
            std::cout << "[pipeline] BDD formula build took "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                      << " ms\n";
            debugger.endStage();

            debugger.startStage(StageKind::WEIGHTED_MODEL_COUNTING);

            auto t2 = std::chrono::steady_clock::now();
            auto resolvedEvs = applyEvidence(graph, evidences);
            auto t3 = std::chrono::steady_clock::now();

            auto components = buildComponentSubgraphs(view);
            auto evidencesByComponent = groupEvidencesByComponent(components, resolvedEvs);
            long long evidenceBuildMs = 0;
            long long evidenceWmcMs = 0;
            long long perNodeWmcMs = 0;
            const bool wmcProfile = opt.isWmcProfileEnabled();
            using Clock = std::chrono::steady_clock;
            auto toMs = [](Clock::time_point start) {
                return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            };
            double evidenceMakeAndMs = 0.0;
            double evidenceWmcComputeMs = 0.0;
            double nodeMakeAndMs = 0.0;
            double nodeWmcComputeMs = 0.0;
            std::size_t evidenceWmcCalls = 0;
            std::size_t nodeWmcCalls = 0;
            std::size_t evidenceMakeAndCalls = 0;
            std::size_t nodeMakeAndCalls = 0;
            auto makeAndProfile = [&](const BddNodeRef& lhs, const BddNodeRef& rhs,
                                      double& ms, std::size_t& calls) {
                if (!wmcProfile) {
                    return bddManager->makeAnd(lhs, rhs);
                }
                auto andStart = Clock::now();
                auto res = bddManager->makeAnd(lhs, rhs);
                ms += toMs(andStart);
                calls++;
                return res;
            };
            auto computeWmcProfile = [&](const BddNodeRef& node, double& ms, std::size_t& calls,
                                         bool logarithmic = false) {
                calls++;
                if (!wmcProfile) {
                    return logarithmic ? bddManager->computeLogWeightedModelCount(node) :
                            bddManager->computeWeightedModelCount(node);
                }
                auto wmcStart = Clock::now();
                double res = logarithmic ? bddManager->computeLogWeightedModelCount(node) :
                        bddManager->computeWeightedModelCount(node);
                ms += toMs(wmcStart);
                return res;
            };

            probResult.clear();
            for (const auto& comp : components) {
                const auto& componentEvs = evidencesByComponent[comp.id];
                auto evidenceBuildStart = std::chrono::steady_clock::now();
                auto evidenceBdd = bddManager->getTrue();
                for (const auto& [eNode, val] : componentEvs) {
                    auto it = nodeFormulas.find(eNode);
                    if (it == nodeFormulas.end()) {
                        throw std::runtime_error("Evidence node has no formula: " +
                                eNode->getTuple().toString());
                    }
                    auto lit = it->second;
                    if (!val) {
                        lit = bddManager->makeNot(lit);
                    }
                    evidenceBdd = makeAndProfile(evidenceBdd, lit, evidenceMakeAndMs, evidenceMakeAndCalls);
                }
                evidenceBuildMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - evidenceBuildStart)
                                           .count();

                auto wmcStart = std::chrono::steady_clock::now();
                double evidenceLogWeight = 0.0;
                if (!componentEvs.empty()) {
                    evidenceLogWeight = computeWmcProfile(
                            evidenceBdd, evidenceWmcComputeMs, evidenceWmcCalls, true);
                    if (!std::isfinite(evidenceLogWeight)) {
                        throw std::runtime_error("Inconsistent evidence: observations have zero probability");
                    }
                }
                evidenceWmcMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - wmcStart)
                                         .count();

                auto perNodeStart = std::chrono::steady_clock::now();
                for (const auto& node : comp.nodes) {
                    if (!node->needOutput) {
                        continue;
                    }
                    auto it = nodeFormulas.find(node);
                    if (it == nodeFormulas.end()) {
                        continue;
                    }
                    const auto& bdd = it->second;
                    double prob = 0.0;
                    if (componentEvs.empty()) {
                        prob = computeWmcProfile(bdd, nodeWmcComputeMs, nodeWmcCalls);
                    } else {
                        auto joint = makeAndProfile(bdd, evidenceBdd, nodeMakeAndMs, nodeMakeAndCalls);
                        double jointLogWeight = computeWmcProfile(joint, nodeWmcComputeMs, nodeWmcCalls, true);
                        prob = std::exp(std::min(0.0, jointLogWeight - evidenceLogWeight));
                    }
                    probResult[node] = prob;
                }
                perNodeWmcMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - perNodeStart)
                                        .count();
            }
            for (const auto& [node, prob] : precomputedProbResult) {
                probResult.emplace(node, prob);
            }

            std::cout << "[pipeline] evidence resolve/tag took "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count()
                      << " ms\n";
            std::cout << "[pipeline] component evidence build took " << evidenceBuildMs << " ms\n";
            std::cout << "[pipeline] component evidence WMC took " << evidenceWmcMs << " ms\n";
            std::cout << "[pipeline] per-node conditional WMC took "
                      << perNodeWmcMs << " ms\n";
            if (wmcProfile) {
                std::cout << "[wmc-profile] stage=EXACT"
                          << " mode=plain"
                          << " total_ms=" << (evidenceBuildMs + evidenceWmcMs + perNodeWmcMs)
                          << " components=" << components.size()
                          << " nodes=" << view.getNodes().size()
                          << " evidence_build_ms=" << evidenceBuildMs
                          << " evidence_make_and_calls=" << evidenceMakeAndCalls
                          << " evidence_make_and_ms=" << evidenceMakeAndMs
                          << " evidence_wmc_calls=" << evidenceWmcCalls
                          << " evidence_wmc_compute_ms=" << evidenceWmcComputeMs
                          << " node_make_and_calls=" << nodeMakeAndCalls
                          << " node_make_and_ms=" << nodeMakeAndMs
                          << " node_wmc_calls=" << nodeWmcCalls
                          << " node_wmc_compute_ms=" << nodeWmcComputeMs
                          << " live_nodes=" << bddManager->getLiveNodeCount()
                          << std::endl;
            }

            debugger.endStage();
            debugger.startStage(StageKind::IO_DUMP);
            auto tDumpStart = std::chrono::steady_clock::now();
            dumpProbabilities(probResult, opt.getOutputFileDir());
            auto tDumpMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - tDumpStart)
                                   .count();
            std::cout << "[pipeline] probability dump took " << tDumpMs << " ms\n";
            debugger.endStage();
        }
    }

    debugger.endTurn();
    dumpInitialInputRelations(opt.getOutputFileDir() + "/initial-input-relations-iter0.txt");
}

#ifdef SOUFFLE_HAVE_SDD
static void runSddPipeline(
        const CmdOptions& opt,
        SouffleProgram& /*program*/,
        RuleManager& /*ruleManager*/,
        QueryManager& /*queryManager*/,
        DerivationGraph& graph,
        SubgraphView& view,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences,
        StageInfo* rewriteHybridStage) {
    Debugger& debugger = Debugger::getInstance();

    std::map<NodePtr, SddNodeRef> nodeFormulas;
    std::map<EdgePtr, SddNodeRef> edgeFormulas;
    std::unique_ptr<SddFormulaManager> sddManager;
    const bool computeProbabilities = !opt.isDerivationOnly();
    std::unordered_set<NodePtr> seedTrueNodes;
    seedTrueNodes.reserve(view.getNodes().size());
    for (const auto& n : view.getNodes()) {
        const std::string s = n->getTuple().toString();
        if (s.rfind("@magic.", 0) != 0) continue;

        if (view.getIncomingEdges(n).empty()) {
            seedTrueNodes.insert(n);
        }
    }
    //std::cout << "[dbg] seedTrueNodes size = " << seedTrueNodes.size() << "\

    if (computeProbabilities) {
        if (opt.isRewriteEnabled()) {
            auto* hybridStage = rewriteHybridStage;
            if (!hybridStage) {
                hybridStage = debugger.startStage(StageKind::FC_WMC_HYBRID);
            }
            auto varEstimate = estimateBddVarCount(view);
            debugger.addInfo("rand_vars", std::to_string(varEstimate));
            if (hybridStage) {
                hybridStage->logMessage(Level::INFO, "rand_vars=" + std::to_string(varEstimate));
            }

            const bool enableFast = opt.isSingleRandFastEnabled();
            auto componentsBuildStart = std::chrono::steady_clock::now();
            auto components = buildComponentSubgraphs(view);
            auto componentsBuildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - componentsBuildStart)
                                             .count();
            auto analysesStart = std::chrono::steady_clock::now();
            auto analyses = analyzeComponents(view, std::move(components), enableFast);
            auto analysesMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - analysesStart)
                                      .count();

            auto t2 = std::chrono::steady_clock::now();
            auto resolvedEvs = applyEvidence(graph, evidences);
            auto t3 = std::chrono::steady_clock::now();
            auto evidenceApplyMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
            auto evidenceGroupStart = std::chrono::steady_clock::now();
            auto evidencesByComponent = groupEvidencesByComponent(analyses, resolvedEvs);
            auto evidenceGroupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - evidenceGroupStart)
                                           .count();

            std::vector<FastComponentEval> fastComponents;
            std::vector<ConjComponentEval> conjComponents;
            std::vector<ComponentSubgraph> slowComponents;
            FastComponentStats fastStats;
            ConjFastStats conjStats;
            const bool logFastReasons = opt.isDumpDotEnabled();
            std::vector<ComponentDecision> decisions;
            decisions.reserve(analyses.size());

            double componentFastEvalTotalMs = 0.0;
            auto classifyStart = std::chrono::steady_clock::now();
            for (auto& analysis : analyses) {
                const auto& comp = analysis.comp;
                const auto& compEvs = evidencesByComponent[comp.id];
                const bool hasEvidence = !compEvs.empty();
                const bool singleCandidate = analysis.randVars == 1;
                const bool conjCandidate = !singleCandidate && !analysis.hasNegation &&
                        !analysis.hasOr && !analysis.hasCycle;  // fast path excludes cycles/OR/negation
                ComponentDecision decision{
                        comp.id,
                        comp.nodes.size(),
                        comp.edges.size(),
                        analysis.randVars,
                        hasEvidence,
                        analysis.hasNegation,
                        analysis.hasOr,
                        analysis.hasCycle,
                        "",
                        "",
                };

                if (enableFast) {
                    if (singleCandidate) fastStats.candidates++;
                    if (conjCandidate) conjStats.candidates++;
                }
                if (logFastReasons) {
                    std::vector<std::string> singleReasons;
                    if (!enableFast) singleReasons.emplace_back("fast_disabled");
                    if (!singleCandidate) singleReasons.emplace_back("randvars!=1");
                    if (hasEvidence) singleReasons.emplace_back("has_evidence");
                    if (!singleReasons.empty()) {
                        std::cout << "[fc-component] id=" << comp.id
                                  << " single_skip=" << join(singleReasons, ",")
                                  << " rand_vars=" << analysis.randVars
                                  << " has_negation=" << analysis.hasNegation
                                  << " has_or=" << analysis.hasOr
                                  << " has_cycle=" << analysis.hasCycle
                                  << std::endl;
                    }
                    std::vector<std::string> conjReasons;
                    if (!enableFast) conjReasons.emplace_back("fast_disabled");
                    if (singleCandidate) conjReasons.emplace_back("single_randvar");
                    if (analysis.hasNegation) conjReasons.emplace_back("negation");
                    if (analysis.hasOr) conjReasons.emplace_back("or");
                    if (analysis.hasCycle) conjReasons.emplace_back("cycle");
                    if (hasEvidence) conjReasons.emplace_back("has_evidence");
                    if (!conjReasons.empty()) {
                        std::cout << "[fc-component] id=" << comp.id
                                  << " conj_skip=" << join(conjReasons, ",")
                                  << " rand_vars=" << analysis.randVars
                                  << " has_negation=" << analysis.hasNegation
                                  << " has_or=" << analysis.hasOr
                                  << " has_cycle=" << analysis.hasCycle
                                  << std::endl;
                    }
                }

                if (!enableFast || hasEvidence) {
                    if (enableFast && singleCandidate) fastStats.skipped++;
                    if (enableFast && conjCandidate) conjStats.skipped++;
                    decision.mode = "slow";
                    decision.reason = !enableFast ? "fast_disabled" : "has_evidence";
                    decisions.push_back(decision);
                    slowComponents.push_back(std::move(analysis.comp));
                    continue;
                }

                if (singleCandidate) {
                    FastComponentEval eval;
                    eval.var = analysis.singleRand;
                    auto fastEvalStart = std::chrono::steady_clock::now();
                    const bool evalOk = evaluateSingleRandComponentBoth(
                            analysis.comp, eval.var, eval.valuesFalse, eval.valuesTrue, &eval.evalMsTrue);
                    componentFastEvalTotalMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - fastEvalStart).count();
                    if (!evalOk) {
                        eval.evalMsFalse = 0;
                        if (logFastReasons) {
                            std::cout << "[fc-component] id=" << comp.id
                                      << " single_skip=eval_failed"
                                      << " rand_vars=" << analysis.randVars
                                      << std::endl;
                        }
                        fastStats.skipped++;
                        decision.mode = "slow";
                        decision.reason = "eval_failed";
                        decisions.push_back(decision);
                        slowComponents.push_back(std::move(analysis.comp));
                        continue;
                    }
                    eval.comp = std::move(analysis.comp);
                    fastStats.used++;
                    fastStats.evalMs += eval.evalMsTrue + eval.evalMsFalse;
                    decision.mode = "fast_single";
                    decisions.push_back(decision);
                    fastComponents.push_back(std::move(eval));
                    continue;
                }

                if (conjCandidate) {
                    ConjComponentEval eval;
                    auto fastEvalStart = std::chrono::steady_clock::now();
                    const bool conjOk =
                            evaluateConjComponent(analysis.comp, eval.probabilities, &eval.evalMs);
                    componentFastEvalTotalMs += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - fastEvalStart).count();
                    if (!conjOk) {
                        if (logFastReasons) {
                            std::cout << "[fc-component] id=" << comp.id
                                      << " conj_skip=eval_failed"
                                      << " rand_vars=" << analysis.randVars
                                      << std::endl;
                        }
                        conjStats.skipped++;
                        decision.mode = "slow";
                        decision.reason = "eval_failed";
                        decisions.push_back(decision);
                        slowComponents.push_back(std::move(analysis.comp));
                        continue;
                    }
                    eval.comp = std::move(analysis.comp);
                    conjStats.used++;
                    conjStats.evalMs += eval.evalMs;
                    decision.mode = "fast_conj";
                    decisions.push_back(decision);
                    conjComponents.push_back(std::move(eval));
                    continue;
                }

                decision.mode = "slow";
                if (analysis.hasOr) {
                    decision.reason = "or";
                } else if (analysis.hasNegation) {
                    decision.reason = "negation";
                } else if (analysis.hasCycle) {
                    decision.reason = "cycle";
                } else {
                    decision.reason = "other";
                }
                decisions.push_back(decision);
                slowComponents.push_back(std::move(analysis.comp));
            }
            auto classifyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - classifyStart)
                                      .count();
            const double componentClassifyPureMs =
                    std::max(0.0, static_cast<double>(classifyMs) - componentFastEvalTotalMs);

            long long initMsTotal = 0;
            long long initMsMax = 0;
            auto makeManager = [&](SubgraphView&) {
                return std::make_unique<SddFormulaManager>();
            };

            std::vector<ComponentFormulaBundle<SddFormulaManager, SddNodeRef>> bundles;
            long long buildMs = 0;
            if (!slowComponents.empty()) {
                auto t0 = std::chrono::steady_clock::now();
                bundles = buildFormulasCyclewiseByComponentList<SddFormulaManager, SddNodeRef>(
                        std::move(slowComponents), makeManager, &initMsTotal, &initMsMax);
                auto t1 = std::chrono::steady_clock::now();
                auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                buildMs = totalMs - initMsTotal;
                if (buildMs < 0) {
                    buildMs = totalMs;
                }
            }
            long long liveNodesSum = 0;
            for (const auto& bundle : bundles) {
                liveNodesSum += static_cast<long long>(bundle.manager->getLiveNodeCount());
            }

            debugger.addInfo("manager_init_ms", std::to_string(initMsTotal));
            debugger.addInfo("manager_init_ms_max", std::to_string(initMsMax));
            debugger.addInfo("manager_init_components", std::to_string(bundles.size()));
            debugger.addInfo("live_nodes", std::to_string(liveNodesSum));
            debugger.addInfo("component_build_subgraphs_ms", std::to_string(componentsBuildMs));
            debugger.addInfo("component_analyze_ms", std::to_string(analysesMs));
            debugger.addInfo("evidence_apply_ms", std::to_string(evidenceApplyMs));
            debugger.addInfo("evidence_group_ms", std::to_string(evidenceGroupMs));
            debugger.addInfo("component_classify_ms", std::to_string(classifyMs));
            debugger.addInfo("component_classify_pure_ms", std::to_string(componentClassifyPureMs));
            debugger.addInfo("component_fast_eval_total_ms", std::to_string(componentFastEvalTotalMs));
            debugger.addInfo("fastpath_components", std::to_string(fastStats.used));
            debugger.addInfo("fastpath_candidates", std::to_string(fastStats.candidates));
            debugger.addInfo("fastpath_skipped", std::to_string(fastStats.skipped));
            debugger.addInfo("fastpath_eval_ms", std::to_string(fastStats.evalMs));
            debugger.addInfo("fastpath_conj_components", std::to_string(conjStats.used));
            debugger.addInfo("fastpath_conj_candidates", std::to_string(conjStats.candidates));
            debugger.addInfo("fastpath_conj_skipped", std::to_string(conjStats.skipped));
            debugger.addInfo("fastpath_conj_eval_ms", std::to_string(conjStats.evalMs));
            if (hybridStage) {
                hybridStage->logMessage(Level::INFO, "manager_init_ms=" + std::to_string(initMsTotal));
                hybridStage->logMessage(Level::INFO, "manager_init_ms_max=" + std::to_string(initMsMax));
                hybridStage->logMessage(Level::INFO, "manager_init_components=" +
                        std::to_string(bundles.size()));
                hybridStage->logMessage(Level::INFO, "live_nodes=" + std::to_string(liveNodesSum));
                hybridStage->logMessage(Level::INFO, "component_build_subgraphs_ms=" +
                        std::to_string(componentsBuildMs));
                hybridStage->logMessage(Level::INFO, "component_analyze_ms=" +
                        std::to_string(analysesMs));
                hybridStage->logMessage(Level::INFO, "evidence_apply_ms=" +
                        std::to_string(evidenceApplyMs));
                hybridStage->logMessage(Level::INFO, "evidence_group_ms=" +
                        std::to_string(evidenceGroupMs));
                hybridStage->logMessage(Level::INFO, "component_classify_ms=" +
                        std::to_string(classifyMs));
                hybridStage->logMessage(Level::INFO, "component_classify_pure_ms=" +
                        std::to_string(componentClassifyPureMs));
                hybridStage->logMessage(Level::INFO, "component_fast_eval_total_ms=" +
                        std::to_string(componentFastEvalTotalMs));
                hybridStage->logMessage(Level::INFO, "fastpath_components=" + std::to_string(fastStats.used));
                hybridStage->logMessage(Level::INFO, "fastpath_candidates=" + std::to_string(fastStats.candidates));
                hybridStage->logMessage(Level::INFO, "fastpath_skipped=" + std::to_string(fastStats.skipped));
                hybridStage->logMessage(Level::INFO, "fastpath_eval_ms=" + std::to_string(fastStats.evalMs));
                hybridStage->logMessage(Level::INFO, "fastpath_conj_components=" +
                        std::to_string(conjStats.used));
                hybridStage->logMessage(Level::INFO, "fastpath_conj_candidates=" +
                        std::to_string(conjStats.candidates));
                hybridStage->logMessage(Level::INFO, "fastpath_conj_skipped=" +
                        std::to_string(conjStats.skipped));
                hybridStage->logMessage(Level::INFO, "fastpath_conj_eval_ms=" +
                        std::to_string(conjStats.evalMs));
            }
            if (logFastReasons) {
                std::size_t fastSingle = 0;
                std::size_t fastConj = 0;
                std::size_t slowCount = 0;
                for (const auto& decision : decisions) {
                    if (decision.mode == "fast_single") {
                        fastSingle++;
                    } else if (decision.mode == "fast_conj") {
                        fastConj++;
                    } else {
                        slowCount++;
                    }
                }
                std::cout << "[fc-component-info] total=" << decisions.size()
                          << " fast_single=" << fastSingle
                          << " fast_conj=" << fastConj
                          << " slow=" << slowCount
                          << std::endl;
                for (const auto& decision : decisions) {
                    std::cout << "[fc-component-info] id=" << decision.id
                              << " nodes=" << decision.nodes
                              << " edges=" << decision.edges
                              << " rand_vars=" << decision.randVars
                              << " has_negation=" << decision.hasNegation
                              << " has_or=" << decision.hasOr
                              << " has_cycle=" << decision.hasCycle
                              << " has_evidence=" << decision.hasEvidence
                              << " mode=" << decision.mode
                              << " reason=" << decision.reason
                              << std::endl;
                }
            }

            std::cout << "[pipeline] SDD formula build took " << buildMs << " ms\n";

            long long evidenceBuildMs = 0;
            long long evidenceWmcMs = 0;
            long long perNodeWmcMs = 0;
            long long fastPathMs = fastStats.evalMs + conjStats.evalMs;

            probResult.clear();
            if (!fastComponents.empty()) {
                for (const auto& fast : fastComponents) {
                    double p = fast.var.probability;
                    for (const auto& node : fast.comp.nodes) {
                        if (!node->needOutput) {
                            continue;
                        }
                        auto itTrue = fast.valuesTrue.find(node);
                        auto itFalse = fast.valuesFalse.find(node);
                        if (itTrue == fast.valuesTrue.end() && itFalse == fast.valuesFalse.end()) {
                            continue;
                        }
                        bool vTrue = (itTrue != fast.valuesTrue.end()) ? itTrue->second : false;
                        bool vFalse = (itFalse != fast.valuesFalse.end()) ? itFalse->second : false;
                        double numerator = (vTrue ? p : 0.0) + (vFalse ? (1.0 - p) : 0.0);
                        double prob = numerator;
                        probResult[node] = prob;
                    }
                }
            }
            if (!conjComponents.empty()) {
                for (const auto& conj : conjComponents) {
                    for (const auto& [node, prob] : conj.probabilities) {
                        if (!node->needOutput) {
                            continue;
                        }
                        probResult[node] = prob;
                    }
                }
            }
            for (auto& bundle : bundles) {
                auto& manager = *bundle.manager;
                const auto& componentEvs = evidencesByComponent[bundle.id];

                auto buildStart = std::chrono::steady_clock::now();
                auto evidenceSdd = manager.getTrue();
                for (const auto& [eNode, val] : componentEvs) {
                    auto it = bundle.nodeFormulas.find(eNode);
                    if (it == bundle.nodeFormulas.end()) {
                        throw std::runtime_error("Evidence node has no formula: " +
                                eNode->getTuple().toString());
                    }
                    auto lit = it->second;
                    if (!val) {
                        lit = manager.makeNot(lit);
                    }
                    evidenceSdd = manager.makeAnd(evidenceSdd, lit);
                }
                evidenceBuildMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - buildStart)
                                           .count();

                auto wmcStart = std::chrono::steady_clock::now();
                double evidenceWeight = 1.0;
                if (!componentEvs.empty()) {
                    evidenceWeight = manager.computeWeightedModelCount(evidenceSdd);
                }
                evidenceWmcMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - wmcStart)
                                         .count();

                auto perNodeStart = std::chrono::steady_clock::now();
                for (const auto& [node, sdd] : bundle.nodeFormulas) {
                    if (!node->needOutput) {
                        continue;
                    }
                    double prob = 0.0;
                    if (componentEvs.empty()) {
                        prob = manager.computeWeightedModelCount(sdd);
                    } else if (evidenceWeight == 0.0) {
                        prob = 0.0;
                    } else {
                        auto joint = manager.makeAnd(sdd, evidenceSdd);
                        double jointW = manager.computeWeightedModelCount(joint);
                        prob = jointW / evidenceWeight;
                    }
                    probResult[node] = prob;
                }
                perNodeWmcMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - perNodeStart)
                                        .count();
            }
            for (const auto& [node, prob] : precomputedProbResult) {
                probResult.emplace(node, prob);
            }

            view.dumpStatistics(std::cout);
            std::cout << "[pipeline] component subgraph build took " << componentsBuildMs << " ms\n";
            std::cout << "[pipeline] component analysis took " << analysesMs << " ms\n";
            std::cout << "[pipeline] evidence resolve/tag took " << evidenceApplyMs << " ms\n";
            std::cout << "[pipeline] evidence grouping took " << evidenceGroupMs << " ms\n";
            std::cout << "[pipeline] component classification took " << classifyMs
                      << " ms (pure=" << componentClassifyPureMs
                      << " ms, fast_eval=" << componentFastEvalTotalMs << " ms)\n";
            std::cout << "[pipeline] component evidence build took " << evidenceBuildMs << " ms\n";
            std::cout << "[pipeline] component evidence WMC took " << evidenceWmcMs << " ms\n";
            std::cout << "[pipeline] per-node conditional WMC took " << perNodeWmcMs << " ms\n";
            if (fastStats.used > 0 || conjStats.used > 0) {
                std::cout << "[pipeline] fastpath WMC took " << fastPathMs << " ms\n";
            }

            debugger.endStage();
            debugger.startStage(StageKind::IO_DUMP);
            auto tDumpStart = std::chrono::steady_clock::now();
            dumpProbabilities(probResult, opt.getOutputFileDir());
            auto tDumpMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - tDumpStart)
                                   .count();
            std::cout << "[pipeline] probability dump took " << tDumpMs << " ms\n";
            debugger.endStage();
        } else {
            auto* fcStage = debugger.startStage(StageKind::FORWARD_COMPILATION);
            auto initStart = std::chrono::steady_clock::now();
            sddManager = std::make_unique<SddFormulaManager>();
            auto initMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - initStart)
                                  .count();
            debugger.addInfo("manager_init_ms", std::to_string(initMs));
            if (fcStage) {
                fcStage->logMessage(Level::INFO, "manager_init_ms=" + std::to_string(initMs));
            }
            auto t0 = std::chrono::steady_clock::now();
            buildFormulasCyclewise(view, *sddManager, nodeFormulas, edgeFormulas, seedTrueNodes);
            auto t1 = std::chrono::steady_clock::now();
            std::cout << "[pipeline] SDD formula build took "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                      << " ms\n";
            debugger.endStage();

            debugger.startStage(StageKind::WEIGHTED_MODEL_COUNTING);

            auto t2 = std::chrono::steady_clock::now();
            auto resolvedEvs = applyEvidence(graph, evidences);
            auto t3 = std::chrono::steady_clock::now();

            auto components = buildComponentSubgraphs(view);
            auto evidencesByComponent = groupEvidencesByComponent(components, resolvedEvs);
            long long evidenceBuildMs = 0;
            long long evidenceWmcMs = 0;
            long long perNodeWmcMs = 0;

            probResult.clear();
            for (const auto& comp : components) {
                const auto& componentEvs = evidencesByComponent[comp.id];
                auto evidenceBuildStart = std::chrono::steady_clock::now();
                auto evidenceSdd = sddManager->getTrue();
                for (const auto& [eNode, val] : componentEvs) {
                    auto it = nodeFormulas.find(eNode);
                    if (it == nodeFormulas.end()) {
                        throw std::runtime_error("Evidence node has no formula: " +
                                eNode->getTuple().toString());
                    }
                    auto lit = it->second;
                    if (!val) {
                        lit = sddManager->makeNot(lit);
                    }
                    evidenceSdd = sddManager->makeAnd(evidenceSdd, lit);
                }
                evidenceBuildMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - evidenceBuildStart)
                                           .count();

                auto wmcStart = std::chrono::steady_clock::now();
                double evidenceWeight = 1.0;
                if (!componentEvs.empty()) {
                    evidenceWeight = sddManager->computeWeightedModelCount(evidenceSdd);
                }
                evidenceWmcMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - wmcStart)
                                         .count();

                auto perNodeStart = std::chrono::steady_clock::now();
                for (const auto& node : comp.nodes) {
                    if (!node->needOutput) {
                        continue;
                    }
                    auto it = nodeFormulas.find(node);
                    if (it == nodeFormulas.end()) {
                        continue;
                    }
                    const auto& sdd = it->second;
                    double prob = 0.0;
                    if (componentEvs.empty()) {
                        prob = sddManager->computeWeightedModelCount(sdd);
                    } else if (evidenceWeight == 0.0) {
                        prob = 0.0;
                    } else {
                        auto joint = sddManager->makeAnd(sdd, evidenceSdd);
                        double jointW = sddManager->computeWeightedModelCount(joint);
                        prob = jointW / evidenceWeight;
                    }
                    probResult[node] = prob;
                }
                perNodeWmcMs += std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - perNodeStart)
                                        .count();
            }
            for (const auto& [node, prob] : precomputedProbResult) {
                probResult.emplace(node, prob);
            }

            view.dumpStatistics(std::cout);
            std::cout << "[pipeline] evidence resolve/tag took "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count()
                      << " ms\n";
            std::cout << "[pipeline] component evidence build took " << evidenceBuildMs << " ms\n";
            std::cout << "[pipeline] component evidence WMC took " << evidenceWmcMs << " ms\n";
            std::cout << "[pipeline] per-node conditional WMC took "
                      << perNodeWmcMs << " ms\n";

            debugger.endStage();
            debugger.startStage(StageKind::IO_DUMP);
            auto tDumpStart = std::chrono::steady_clock::now();
            dumpProbabilities(probResult, opt.getOutputFileDir());
            auto tDumpMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - tDumpStart)
                                   .count();
            std::cout << "[pipeline] probability dump took " << tDumpMs << " ms\n";
            debugger.endStage();
        }
    }

    debugger.endTurn();
    dumpInitialInputRelations(opt.getOutputFileDir() + "/initial-input-relations-iter0.txt");
}
#endif

void runPipeline(
        const CmdOptions& opt,
        SouffleProgram& program,
        RuleManager& ruleManager,
        QueryManager& queryManager,
        const std::unordered_map<UntypedTuple, double>& factProb,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences) {
    setActiveSymbolTable(&program.getSymbolTable());
    std::cout << std::fixed << std::setprecision(8);
    Debugger& debugger = Debugger::getInstance();
    configureUntypedTupleRenderingContext(
            &program.getSymbolTable(), collectRelationAttributeTypes(program));
    struct ClearTupleRenderingContext {
        ~ClearTupleRenderingContext() {
            clearUntypedTupleRenderingContext();
        }
    } clearTupleRenderingContext;
    fcProfileEnabled = opt.isFcProfileEnabled();
    wmcProfileEnabled = opt.isWmcProfileEnabled();
    depGraphProfileEnabled = opt.isDepGraphProfileEnabled();
    DerivationGraphViewInterface::setDumpDotEnabled(opt.isDumpDotEnabled());
    DerivationGraphViewInterface::setDumpJsonEnabled(opt.isDumpJsonEnabled());
    DerivationGraphViewInterface::setDumpStatsEnabled(opt.isDumpStatEnabled());
    DerivationGraphViewInterface::setDumpOutputDir(opt.getOutputFileDir());
    DerivationGraph::setMergeBiImpEnabled(exactInferenceMode && opt.isMergeBiImpEnabled());
    DerivationGraph::setPruneExtraEnabled(opt.isPruneExtraEnabled());
    precomputedProbResult.clear();
    precomputedTupleProbResult.clear();

    std::map<std::string, double> liftedProbabilities;
    std::unordered_set<std::string> liftedOutputs;
    auto concreteOutputs = program.getOutputRelations();
    if (opt.isLiftedWmcEnabled()) {
        debugger.startStage(StageKind::LIFTED_WMC);
        auto lifted = tryEvaluateLiftedPointwise(opt, program, ruleManager, factProb, evidences);
        liftedOutputs.insert(lifted.handledOutputRelations.begin(), lifted.handledOutputRelations.end());
        // Explicit queries outside the lifted outputs still need concrete WMC.
        for (const auto* query : queryManager.getAllQuery()) {
            if (query && liftedOutputs.count(query->getRelationName()) == 0) lifted.complete = false;
        }
        if (lifted.handled && !lifted.complete) lifted.reason = "partial_pointwise_lifted";
        debugger.addInfo("lifted_handled", lifted.handled ? "true" : "false");
        debugger.addInfo("lifted_complete", lifted.complete ? "true" : "false");
        debugger.addInfo("lifted_reason", lifted.reason);
        debugger.addInfo("lifted_execution_mode", lifted.executionMode);
        debugger.addInfo("lifted_output_tuples", std::to_string(lifted.liftedOutputTuples));
        debugger.addInfo("lifted_concrete_output_tuples", std::to_string(lifted.concreteOutputTuples));
        debugger.addInfo("lifted_relation_templates", std::to_string(lifted.relationTemplates));
        debugger.addInfo("lifted_abstract_nodes", std::to_string(lifted.abstractNodes));
        debugger.addInfo("lifted_abstract_edges", std::to_string(lifted.abstractEdges));
        debugger.addInfo("lifted_symbolic_variables", std::to_string(lifted.symbolicVariables));
        debugger.addInfo("lifted_bdd_nodes", std::to_string(lifted.bddNodes));
        debugger.addInfo("lifted_closed_form_tuples", std::to_string(lifted.closedFormTuples));
        debugger.addInfo("lifted_eligibility_ms", std::to_string(lifted.eligibilityMs));
        debugger.addInfo("lifted_abstract_graph_ms", std::to_string(lifted.abstractGraphMs));
        debugger.addInfo("lifted_symbolic_dd_ms", std::to_string(lifted.symbolicDdMs));
        debugger.addInfo("lifted_instantiate_wmc_ms", std::to_string(lifted.instantiateWmcMs));
        std::cout << "[lifted-wmc] handled=" << lifted.handled << " complete=" << lifted.complete
                  << " lifted_output_tuples=" << lifted.liftedOutputTuples
                  << " closed_form_tuples=" << lifted.closedFormTuples
                  << " reason=" << lifted.reason << '\n';
        for (const auto& [relation, reason] : lifted.rejectedOutputReasons) {
            debugger.addInfo("lifted_rejected_" + relation, reason);
            std::cout << "[lifted-wmc] concrete_output=" << relation << " reason=" << reason << '\n';
        }
        if (!lifted.abstractGraph.empty()) {
            std::ofstream out(joinOutputPath(opt.getOutputFileDir(), "abstract-derivation-graph.tsv"));
            out << lifted.abstractGraph;
        }
        if (!lifted.abstractGraphDot.empty()) {
            std::ofstream out(joinOutputPath(opt.getOutputFileDir(), "abstract-derivation-graph.dot"));
            out << lifted.abstractGraphDot;
        }
        debugger.endStage();
        if (lifted.handled) {
            liftedProbabilities = std::move(lifted.probabilities);
            concreteOutputs.erase(std::remove_if(concreteOutputs.begin(), concreteOutputs.end(),
                    [&](const auto* relation) {
                        return relation && liftedOutputs.count(relation->getName()) > 0;
                    }), concreteOutputs.end());
            if (lifted.complete) {
                precomputedTupleProbResult.insert(liftedProbabilities.begin(), liftedProbabilities.end());
                debugger.startStage(StageKind::IO_DUMP);
                std::unordered_map<NodePtr, double> noConcreteProbabilities;
                dumpProbabilities(noConcreteProbabilities, opt.getOutputFileDir());
                debugger.endStage();
                debugger.endTurn();
                dumpInitialInputRelations(opt.getOutputFileDir() + "/initial-input-relations-iter0.txt");
                setActiveSymbolTable(nullptr);
                return;
            }
        }
    }

    debugger.startStage(StageKind::CREATE_GRAPH);
    debugger.addInfo("input_fact_size", std::to_string(countInitialInputFacts()));
    auto t0 = std::chrono::steady_clock::now();
    auto graph = std::unique_ptr<WorkingDerivationGraph>(WorkingDerivationGraph::createFrom(
            DerivationManager::untypedTuple2RuleApplications, ruleManager, queryManager, factProb, evidences));
    const auto graphEvidences = graph->getEvidences();
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "[pipeline] create graph took "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
              << " ms\n";
    debugger.endStage();

    // Keep the full provenance of lifted tuples when concrete outputs depend
    // on them. Only remove their output-root status, never substitute marginals.
    if (!liftedOutputs.empty()) {
        for (const auto& node : graph->getNodes()) {
            if (liftedOutputs.count(node->getTuple().relation_name) > 0) {
                node->needOutput = false;
                node->isQuery = false;
            }
        }
    }

    if (opt.isDumpDotEnabled()) {
        graph->dumpDot(makeOutputPath(opt, "before_prune.dot"));
    }

    debugger.startStage(StageKind::PRUNING);
    debugger.addInfo("before_prune_nodes", std::to_string(graph->getNodes().size()));
    debugger.addInfo("before_prune_edges", std::to_string(graph->getEdges().size()));
    auto t2 = std::chrono::steady_clock::now();
    auto prunedView = graph->prune(concreteOutputs);
    auto view = buildWorkingViewLocal(prunedView.getNodes(), prunedView.getEdges());
    const GraphSummary afterPruneSummary = summarizeGraphLight(view);
    addGraphSummaryInfo(debugger, "after_prune_", afterPruneSummary);
    auto t3 = std::chrono::steady_clock::now();
    std::cout << "[pipeline] pruning took "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count()
              << " ms\n";
    debugger.endStage();

    if (opt.isDumpDotEnabled()) {
        view.dumpDot(makeOutputPath(opt, "after_prune.dot"));
    }
    if (opt.isDumpJsonEnabled()) {
        view.dumpJson(makeOutputPath(opt, "derivation.json"));
    }
    StageInfo* rewriteHybridStage = nullptr;
    RewriteDispatchDecision rewriteDecision;
    bool haveRewriteDecision = false;
    if (opt.isRewriteEnabled() && !opt.isDerivationOnly()) {
        rewriteHybridStage = debugger.startStage(StageKind::FC_WMC_HYBRID);
    }
    if (opt.isRewriteEnabled() && !opt.isDerivationOnly()) {
        auto rewriteStart = std::chrono::steady_clock::now();
        GraphRewriteStats rewriteStats;
        ImplicitSplitOverlayStats overlayRewriteStats;
        rewriteDecision = chooseRewriteDispatch(opt, ruleManager);
        haveRewriteDecision = true;
        std::cout << "[pipeline] rewrite dispatch"
                  << " impl=" << rewriteDecision.impl
                  << " reason=" << rewriteDecision.reason
                  << " split_policy=" << rewriteDecision.splitPolicy
                  << " total_rules=" << rewriteDecision.totalRules
                  << " probabilistic_rules=" << rewriteDecision.probabilisticRules
                  << std::endl;
        if (rewriteHybridStage) {
            debugger.addInfo("rewrite_strategy", "default");
            debugger.addInfo("rewrite_impl", rewriteDecision.impl);
            debugger.addInfo("rewrite_reason", rewriteDecision.reason);
            debugger.addInfo("rewrite_split_policy", rewriteDecision.splitPolicy);
            debugger.addInfo("rewrite_dispatch_total_rules", std::to_string(rewriteDecision.totalRules));
            debugger.addInfo("rewrite_dispatch_probabilistic_rules",
                    std::to_string(rewriteDecision.probabilisticRules));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_strategy=default");
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_impl=" + rewriteDecision.impl);
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_reason=" + rewriteDecision.reason);
            rewriteHybridStage->logMessage(Level::INFO,
                    "rewrite_split_policy=" + rewriteDecision.splitPolicy);
            rewriteHybridStage->logMessage(Level::INFO,
                    "rewrite_dispatch_total_rules=" + std::to_string(rewriteDecision.totalRules));
            rewriteHybridStage->logMessage(Level::INFO,
                    "rewrite_dispatch_probabilistic_rules=" +
                            std::to_string(rewriteDecision.probabilisticRules));
        }
        const auto runGraphRewrite = [&] {
            GraphRewriter rewriter;
            RewriteFeatureFlags rewriteFlags;
            rewriteFlags.splitMode = SplitMode::None;
            // Keep compaction global even for no-split: symbolization uses many
            // deterministic aggregate witnesses, and dirty-only compaction can
            // leave enough redundant deterministic structure to make CUDD blow up.
            rewriteFlags.restrictCompactionToDirty = false;
            // Keep the graph-level rewrite policy aligned with the CLI. Without
            // threading this flag through, implicit exact-inference runs silently fall
            // back to dirty-frontier detect even when the user explicitly asks
            // for a full-graph SISO scan, which makes diagnosis of post-commit
            // rewrite behavior misleading.
            rewriteFlags.forceFullSisoDetect = opt.isForceFullSisoDetectEnabled();
            rewriteFlags.relaxCompactionDirty = opt.isRelaxCompactionDirtyEnabled();
            rewriteStats = rewriter.rewriteUntilFixpoint(*graph, view, opt.isProfiling(), rewriteFlags);
        };
        if (rewriteDecision.useImplicit) {
            const auto implicitRewriteStart = std::chrono::steady_clock::now();
            std::unordered_set<UntypedTuple> originalOutputTuples;
            std::unordered_set<std::string> originalOutputRelations;
            for (const auto& node : view.getNodes()) {
                if (node && node->needOutput) {
                    originalOutputTuples.insert(node->getTuple());
                    originalOutputRelations.insert(node->getTuple().relation_name);
                }
            }
            ImplicitSplitPipelineOptions rewriteOptions;
            rewriteOptions.splitMode = ImplicitSplitMode::Naive;
            rewriteOptions.runOverlayFastPaths = true;
            rewriteOptions.runOverlaySingleHyperedge = true;
            rewriteOptions.runOverlayAllFacts = true;
            rewriteOptions.computeOutputMarginals = false;
            rewriteOptions.collectPatternStats = false;
            rewriteOptions.iterateSplitRewrite = false;
            auto implicitResult = runImplicitSplitRewritePipeline(view, rewriteOptions);
            overlayRewriteStats = implicitResult.stats.overlayStats;
            auto implicitNodesBefore = implicitResult.stats.materializedNodesBefore;
            auto implicitEdgesBefore = implicitResult.stats.materializedEdgesBefore;
            double implicitGraphRewriteMs = implicitResult.stats.graphRewriteMs;
            precomputedTupleProbResult.clear();
            for (const auto& [tupleStr, prob] : implicitResult.carriedPrecomputedTupleProbs) {
                precomputedTupleProbResult[tupleStr] = prob;
            }
            if (canUseImplicitOverlayCommit(implicitResult)) {
                const auto commitSummary = applyImplicitOverlayCommit(*graph, view, implicitResult);
                const auto [recoveredIsolatedFacts, recoveredOutputFacts] =
                        recoverImplicitOutputFactsLocal(*graph, view, originalOutputTuples, originalOutputRelations);
                std::cout << "[pipeline] implicit overlay commit"
                          << " fact_nodes=" << commitSummary.factNodes
                          << " removed_edges=" << commitSummary.removedEdges
                          << " added_edges=" << commitSummary.addedEdges
                          << " removed_nodes=" << commitSummary.removedNodes
                          << " recovered_isolated_output_facts=" << recoveredIsolatedFacts
                          << " recovered_tuple_output_facts=" << recoveredOutputFacts
                          << std::endl;
                implicitNodesBefore = view.getNodes().size();
                implicitEdgesBefore = view.getEdges().size();
                const auto residualRewriteStart = std::chrono::steady_clock::now();
                runGraphRewrite();
                implicitGraphRewriteMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - residualRewriteStart).count();
                const auto [postRewriteRecoveredIsolatedFacts, postRewriteRecoveredOutputFacts] =
                        recoverImplicitOutputFactsLocal(*graph, view, originalOutputTuples, originalOutputRelations);
                if (postRewriteRecoveredIsolatedFacts > 0 || postRewriteRecoveredOutputFacts > 0) {
                    std::cout << "[pipeline] implicit recovered isolated_output_facts="
                              << postRewriteRecoveredIsolatedFacts
                              << " tuple_output_facts=" << postRewriteRecoveredOutputFacts << std::endl;
                }
            } else {
                rewriteStats = implicitResult.stats.graphRewriteStats;
                graph = std::move(implicitResult.materialized.graph);
                if (!graph) {
                    graph = std::make_unique<WorkingDerivationGraph>();
                }
                view = buildWorkingViewLocal(
                        implicitResult.materialized.liveNodes, implicitResult.materialized.liveEdges);
                const auto [recoveredIsolatedFacts, recoveredOutputFacts] =
                        recoverImplicitOutputFactsLocal(*graph, view, originalOutputTuples, originalOutputRelations);
                if (recoveredIsolatedFacts > 0 || recoveredOutputFacts > 0) {
                    std::cout << "[pipeline] implicit recovered isolated_output_facts="
                              << recoveredIsolatedFacts
                              << " tuple_output_facts=" << recoveredOutputFacts << std::endl;
                }
            }
            const GraphSummary implicitHandoffSummary = summarizeGraphLight(view);
            const double implicitGraphDetectMs = rewriteStats.totalDetectMs;
            const double implicitTotalMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - implicitRewriteStart).count();
            std::cout << "[pipeline] implicit handoff"
                      << " nodes=" << implicitHandoffSummary.nodes
                      << " edges=" << implicitHandoffSummary.edges
                      << " random_vars=" << implicitHandoffSummary.randomVariables
                      << " output_nodes=" << implicitHandoffSummary.outputNodes
                      << " precomputed_nodes=" << precomputedProbResult.size()
                      << " precomputed_tuples=" << precomputedTupleProbResult.size()
                      << std::endl;
            std::cout << "[pipeline] implicit rewrite total_ms=" << implicitTotalMs
                      << " overlay_prep_ms=" << implicitResult.stats.overlayPrepMs
                      << " overlay_split_ms=" << implicitResult.stats.overlaySplitMs
                      << " overlay_fastpath_ms=" << implicitResult.stats.overlayFastPathMs
                      << " overlay_siso_detect_ms=" << implicitResult.stats.overlayStats.fastPathDetectMs
                      << " overlay_siso_summarize_ms=" << implicitResult.stats.overlayStats.fastPathSummarizeMs
                      << " materialize_ms=" << implicitResult.stats.materializeMs
                      << " detect_ms=" << implicitGraphDetectMs
                      << " graph_rewrite_ms=" << implicitGraphRewriteMs
                      << " graph_bdd_compile_ms=" << rewriteStats.totalBddBuildMs
                      << " graph_detected_regions=" << rewriteStats.numRegionsDetected
                      << " overlay_aliases=" << implicitResult.stats.overlayStats.aliasesCreated
                      << " overlay_active_alias_refs=" << implicitResult.stats.activeAliasRefs
                      << " overlay_active_aliased_edges=" << implicitResult.stats.activeAliasedEdges
                      << " overlay_edges_aliased=" << implicitResult.stats.overlayStats.edgesAliased
                      << " overlay_all_facts=" << implicitResult.stats.overlayStats.allFactsRewrites
                      << " overlay_single=" << implicitResult.stats.overlayStats.singleHyperedgeRewrites
                      << std::endl;
            if (rewriteHybridStage) {
                const auto formatMs = [](double value) {
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(6) << value;
                    return oss.str();
                };
                const auto addImplicitInfo = [&](const std::string& key, double value) {
                    const std::string text = formatMs(value);
                    debugger.addInfo(key, text);
                    rewriteHybridStage->logMessage(Level::INFO, key + "=" + text);
                };
                const auto addImplicitTextInfo = [&](const std::string& key, const std::string& value) {
                    debugger.addInfo(key, value);
                    rewriteHybridStage->logMessage(Level::INFO, key + "=" + value);
                };
                const auto& implicitGraphStats = rewriteStats;
                addImplicitTextInfo("implicit_overlay_all_facts_regions",
                        std::to_string(overlayRewriteStats.allFactsRewrites));
                addImplicitTextInfo("implicit_overlay_single_regions",
                        std::to_string(overlayRewriteStats.singleHyperedgeRewrites));
                addImplicitTextInfo("implicit_overlay_linear_regions",
                        std::to_string(overlayRewriteStats.linearTwoEdgeRewrites));
                addImplicitTextInfo("implicit_overlay_parallel_regions",
                        std::to_string(overlayRewriteStats.parallelEdgeRewrites));
                addImplicitTextInfo("implicit_overlay_fan_out_regions",
                        std::to_string(overlayRewriteStats.fanOutConvergeRewrites));
                addImplicitTextInfo("implicit_graph_rewritten_regions",
                        std::to_string(implicitGraphStats.numRegionsRewritten));
                addImplicitTextInfo("implicit_graph_nodes_removed",
                        std::to_string(implicitGraphStats.numNodesRemoved));
                addImplicitTextInfo("implicit_graph_edges_removed",
                        std::to_string(implicitGraphStats.numEdgesRemoved));
                addImplicitTextInfo("implicit_graph_edges_added",
                        std::to_string(implicitGraphStats.numEdgesAdded));
                addImplicitTextInfo("implicit_general_nodes_removed",
                        std::to_string(implicitGraphStats.numGeneralNodesRemoved));
                addImplicitTextInfo("implicit_general_edges_removed",
                        std::to_string(implicitGraphStats.numGeneralEdgesRemoved));
                addImplicitTextInfo("implicit_general_edges_added",
                        std::to_string(implicitGraphStats.numGeneralEdgesAdded));
                addImplicitTextInfo("implicit_materialized_nodes_before", std::to_string(implicitNodesBefore));
                addImplicitTextInfo("implicit_materialized_edges_before", std::to_string(implicitEdgesBefore));
                addImplicitTextInfo("implicit_materialized_nodes_after", std::to_string(implicitHandoffSummary.nodes));
                addImplicitTextInfo("implicit_materialized_edges_after", std::to_string(implicitHandoffSummary.edges));
                debugger.addInfo("rewrite_impl", rewriteDecision.impl);
                rewriteHybridStage->logMessage(Level::INFO, "rewrite_impl=" + rewriteDecision.impl);
                addImplicitInfo("implicit_total_ms", implicitTotalMs);
                addImplicitInfo("implicit_overlay_prep_ms", implicitResult.stats.overlayPrepMs);
                addImplicitInfo("implicit_overlay_split_ms", implicitResult.stats.overlaySplitMs);
                addImplicitInfo("implicit_overlay_fastpath_ms", implicitResult.stats.overlayFastPathMs);
                debugger.addInfo("implicit_overlay_active_alias_refs",
                        std::to_string(implicitResult.stats.activeAliasRefs));
                rewriteHybridStage->logMessage(Level::INFO,
                        "implicit_overlay_active_alias_refs=" +
                                std::to_string(implicitResult.stats.activeAliasRefs));
                debugger.addInfo("implicit_overlay_active_aliased_edges",
                        std::to_string(implicitResult.stats.activeAliasedEdges));
                rewriteHybridStage->logMessage(Level::INFO,
                        "implicit_overlay_active_aliased_edges=" +
                                std::to_string(implicitResult.stats.activeAliasedEdges));
                addImplicitInfo("implicit_overlay_siso_detect_ms",
                        implicitResult.stats.overlayStats.fastPathDetectMs);
                addImplicitInfo("implicit_overlay_siso_summarize_ms",
                        implicitResult.stats.overlayStats.fastPathSummarizeMs);
                addImplicitInfo("implicit_overlay_rebuild_index_ms",
                        implicitResult.stats.overlayStats.rebuildIndexMs);
                addImplicitInfo("implicit_overlay_fastpath_single_ms",
                        implicitResult.stats.overlayStats.fastPathSingleMs);
                addImplicitInfo("implicit_overlay_fastpath_linear_ms",
                        implicitResult.stats.overlayStats.fastPathLinearMs);
                addImplicitInfo("implicit_overlay_fastpath_parallel_ms",
                        implicitResult.stats.overlayStats.fastPathParallelMs);
                addImplicitInfo("implicit_overlay_fastpath_fan_out_ms",
                        implicitResult.stats.overlayStats.fastPathFanOutMs);
                addImplicitInfo("implicit_overlay_fastpath_allfacts_ms",
                        implicitResult.stats.overlayStats.fastPathAllFactsMs);
                addImplicitInfo("implicit_materialize_ms", implicitResult.stats.materializeMs);
                addImplicitInfo("implicit_graph_detect_ms", implicitGraphDetectMs);
                addImplicitInfo("implicit_graph_rewrite_ms", implicitGraphRewriteMs);
                addImplicitInfo("implicit_graph_detect_total_ms",
                        implicitGraphStats.totalDetectMs);
                addImplicitInfo("implicit_graph_bdd_manager_init_ms",
                        implicitGraphStats.totalBddManagerInitMs);
                addImplicitInfo("implicit_graph_bdd_compile_ms",
                        implicitGraphStats.totalBddBuildMs);
                addImplicitInfo("implicit_graph_bdd_wmc_ms",
                        implicitGraphStats.totalBddWmcMs);
                addImplicitInfo("implicit_graph_fast_general_ms",
                        implicitGraphStats.totalFastGeneralMs);
                addImplicitInfo("implicit_graph_apply_ms", implicitGraphStats.totalApplyMs);
                addImplicitTextInfo("implicit_graph_detected_regions",
                        std::to_string(implicitGraphStats.numRegionsDetected));
                addImplicitTextInfo("implicit_graph_detected_region_total_edges",
                        std::to_string(implicitGraphStats.totalDetectedRegionEdges));
                addImplicitTextInfo("implicit_graph_detected_region_total_nodes",
                        std::to_string(implicitGraphStats.totalDetectedRegionNodes));
                debugger.addInfo("implicit_graph_general_regions",
                        std::to_string(implicitGraphStats.numGeneralRegionsRewritten));
                rewriteHybridStage->logMessage(Level::INFO,
                        "implicit_graph_general_regions=" +
                                std::to_string(implicitGraphStats.numGeneralRegionsRewritten));
                debugger.addInfo("implicit_graph_fast_general_regions",
                        std::to_string(implicitGraphStats.numFastGeneralRegions));
                rewriteHybridStage->logMessage(Level::INFO,
                        "implicit_graph_fast_general_regions=" +
                                std::to_string(implicitGraphStats.numFastGeneralRegions));
            }
        } else {
            runGraphRewrite();
        }
        const GraphSummary rewriteFinalSummary = summarizeGraphLight(view);
        auto rewriteMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - rewriteStart)
                                 .count();
        auto randomVarsDelta = static_cast<long long>(rewriteStats.randomVarsBefore) -
                static_cast<long long>(rewriteStats.randomVarsAfter);
        double randomVarsRatio = rewriteStats.randomVarsBefore == 0
                                         ? 0.0
                                         : static_cast<double>(rewriteStats.randomVarsAfter) /
                                                   static_cast<double>(rewriteStats.randomVarsBefore);
        std::cout << "[pipeline] rewrite took " << rewriteMs << " ms; iterations="
                  << rewriteStats.numIterations << ", regions=" << rewriteStats.numRegionsRewritten
                  << ", detectedRegions=" << rewriteStats.numRegionsDetected
                  << ", nodesRemoved=" << rewriteStats.numNodesRemoved
                  << ", edgesRemoved=" << rewriteStats.numEdgesRemoved
                  << ", edgesAdded=" << rewriteStats.numEdgesAdded
                  << ", randomVarsBefore=" << rewriteStats.randomVarsBefore
                  << ", randomVarsAfter=" << rewriteStats.randomVarsAfter
                  << ", randomVarsDelta=" << randomVarsDelta
                  << ", randomVarsRatio=" << randomVarsRatio
                  << ", randomVarsRemoved=" << rewriteStats.totalRandomVars
                  << ", compactionMs=" << rewriteStats.totalCompactionMs
                  << ", cleanupMs=" << rewriteStats.totalCleanupMs
                  << ", simpleFactRegions=" << rewriteStats.simpleFactRegions << std::endl;
        if (rewriteHybridStage) {
            debugger.addInfo("rewrite_impl", rewriteDecision.impl);
            debugger.addInfo("rewrite_reason", rewriteDecision.reason);
            debugger.addInfo("rewrite_split_policy", rewriteDecision.splitPolicy);
            debugger.addInfo("rewrite_ms", std::to_string(rewriteMs));
            addGraphSummaryInfo(debugger, "rewrite_final_", rewriteFinalSummary);
            const auto allFactsRegions = rewriteStats.numAllFactsRegionsRewritten + overlayRewriteStats.allFactsRewrites;
            const auto singleRegions = rewriteStats.numSingleHyperedgeRegionsRewritten + overlayRewriteStats.singleHyperedgeRewrites;
            const auto linearRegions = rewriteStats.numLinearRegionsRewritten + overlayRewriteStats.linearTwoEdgeRewrites;
            const auto parallelRegions = rewriteStats.numParallelRegionsRewritten + overlayRewriteStats.parallelEdgeRewrites;
            const auto fanOutRegions = rewriteStats.numFanOutRegionsRewritten + overlayRewriteStats.fanOutConvergeRewrites;
            debugger.addInfo("rewrite_all_facts_regions", std::to_string(allFactsRegions));
            debugger.addInfo("rewrite_single_regions", std::to_string(singleRegions));
            debugger.addInfo("rewrite_linear_regions", std::to_string(linearRegions));
            debugger.addInfo("rewrite_parallel_regions", std::to_string(parallelRegions));
            debugger.addInfo("rewrite_fan_out_regions", std::to_string(fanOutRegions));
            debugger.addInfo("rewrite_simple_fact_regions", std::to_string(rewriteStats.simpleFactRegions));
            debugger.addInfo("rewrite_simple_regions", std::to_string(allFactsRegions + singleRegions +
                    linearRegions + parallelRegions + fanOutRegions + rewriteStats.simpleFactRegions));
            debugger.addInfo("rewrite_general_regions", std::to_string(rewriteStats.numGeneralRegionsRewritten));
            // Net contributions include overlay commits, splitting, compaction,
            // cleanup, and output recovery. Preserve signed graph growth.
            const auto generalNodesRemoved = static_cast<long long>(rewriteStats.numGeneralNodesRemoved);
            const auto generalEdgesRemoved = static_cast<long long>(rewriteStats.numGeneralEdgesRemoved) -
                    static_cast<long long>(rewriteStats.numGeneralEdgesAdded);
            debugger.addInfo("rewrite_general_nodes_net_removed", std::to_string(generalNodesRemoved));
            debugger.addInfo("rewrite_general_edges_net_removed", std::to_string(generalEdgesRemoved));
            debugger.addInfo("rewrite_simple_nodes_net_removed", std::to_string(
                    static_cast<long long>(afterPruneSummary.nodes) -
                    static_cast<long long>(rewriteFinalSummary.nodes) - generalNodesRemoved));
            debugger.addInfo("rewrite_simple_edges_net_removed", std::to_string(
                    static_cast<long long>(afterPruneSummary.edges) -
                    static_cast<long long>(rewriteFinalSummary.edges) - generalEdgesRemoved));
            debugger.addInfo("graph_rewrite_rewritten_regions", std::to_string(rewriteStats.numRegionsRewritten));
            debugger.addInfo("graph_rewrite_general_regions", std::to_string(rewriteStats.numGeneralRegionsRewritten));
            debugger.addInfo("graph_rewrite_general_nodes_removed", std::to_string(rewriteStats.numGeneralNodesRemoved));
            debugger.addInfo("graph_rewrite_general_edges_removed", std::to_string(rewriteStats.numGeneralEdgesRemoved));
            debugger.addInfo("graph_rewrite_general_edges_added", std::to_string(rewriteStats.numGeneralEdgesAdded));
            debugger.addInfo("rewrite_nodes_removed", std::to_string(rewriteStats.numNodesRemoved));
            debugger.addInfo("rewrite_edges_removed", std::to_string(rewriteStats.numEdgesRemoved));
            debugger.addInfo("rewrite_edges_added", std::to_string(rewriteStats.numEdgesAdded));
            debugger.addInfo("rewrite_random_vars_before", std::to_string(rewriteStats.randomVarsBefore));
            debugger.addInfo("rewrite_random_vars_after", std::to_string(rewriteStats.randomVarsAfter));
            debugger.addInfo("rewrite_random_vars_delta", std::to_string(randomVarsDelta));
            debugger.addInfo("rewrite_random_vars_removed", std::to_string(rewriteStats.totalRandomVars));
            debugger.addInfo("rewrite_precomputed_nodes", std::to_string(precomputedProbResult.size()));
            debugger.addInfo("rewrite_precomputed_tuples", std::to_string(precomputedTupleProbResult.size()));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_impl=" + rewriteDecision.impl);
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_reason=" + rewriteDecision.reason);
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_split_policy=" + rewriteDecision.splitPolicy);
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_ms=" + std::to_string(rewriteMs));

            debugger.addInfo("rewrite_initial_count_random_vars_ms",
                    std::to_string(rewriteStats.initialCountRandomVarsMs));
            debugger.addInfo("rewrite_collect_evidence_affected_ms",
                    std::to_string(rewriteStats.initialEvidenceAffectedMs));
            debugger.addInfo("rewrite_detect_total_ms", std::to_string(rewriteStats.totalDetectMs));
            debugger.addInfo("rewrite_bdd_manager_init_ms",
                    std::to_string(rewriteStats.totalBddManagerInitMs));
            debugger.addInfo("rewrite_bdd_compile_ms", std::to_string(rewriteStats.totalBddBuildMs));
            debugger.addInfo("rewrite_bdd_wmc_ms", std::to_string(rewriteStats.totalBddWmcMs));
            debugger.addInfo("rewrite_fast_general_ms", std::to_string(rewriteStats.totalFastGeneralMs));
            debugger.addInfo("rewrite_fast_general_regions",
                    std::to_string(rewriteStats.numFastGeneralRegions));
            debugger.addInfo("rewrite_apply_total_ms", std::to_string(rewriteStats.totalApplyMs));
            debugger.addInfo("rewrite_compaction_ms", std::to_string(rewriteStats.totalCompactionMs));
            debugger.addInfo("rewrite_cleanup_ms", std::to_string(rewriteStats.totalCleanupMs));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_initial_count_random_vars_ms=" +
                    std::to_string(rewriteStats.initialCountRandomVarsMs));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_collect_evidence_affected_ms=" +
                    std::to_string(rewriteStats.initialEvidenceAffectedMs));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_detect_total_ms=" +
                    std::to_string(rewriteStats.totalDetectMs));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_bdd_manager_init_ms=" +
                    std::to_string(rewriteStats.totalBddManagerInitMs));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_bdd_compile_ms=" +
                    std::to_string(rewriteStats.totalBddBuildMs));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_bdd_wmc_ms=" +
                    std::to_string(rewriteStats.totalBddWmcMs));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_fast_general_ms=" +
                    std::to_string(rewriteStats.totalFastGeneralMs));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_fast_general_regions=" +
                    std::to_string(rewriteStats.numFastGeneralRegions));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_apply_total_ms=" +
                    std::to_string(rewriteStats.totalApplyMs));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_compaction_ms=" +
                    std::to_string(rewriteStats.totalCompactionMs));
            rewriteHybridStage->logMessage(Level::INFO, "rewrite_cleanup_ms=" +
                    std::to_string(rewriteStats.totalCleanupMs));

            if (!rewriteDecision.useImplicit) {
                const auto formatMs = [](double value) {
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(6) << value;
                    return oss.str();
                };
                const auto addGraphRewriteInfo = [&](const std::string& key, double value) {
                    const std::string text = formatMs(value);
                    debugger.addInfo(key, text);
                    rewriteHybridStage->logMessage(Level::INFO, key + "=" + text);
                };
                const auto addGraphRewriteTextInfo = [&](const std::string& key, const std::string& value) {
                    debugger.addInfo(key, value);
                    rewriteHybridStage->logMessage(Level::INFO, key + "=" + value);
                };
                const double rewriteMsDouble = static_cast<double>(rewriteMs);
                addGraphRewriteInfo("graph_rewrite_total_ms", rewriteMsDouble);
                addGraphRewriteInfo("graph_rewrite_detect_ms", rewriteStats.totalDetectMs);
                addGraphRewriteInfo("graph_rewrite_detect_total_ms", rewriteStats.totalDetectMs);
                addGraphRewriteInfo("graph_rewrite_bdd_manager_init_ms",
                        rewriteStats.totalBddManagerInitMs);
                addGraphRewriteInfo("graph_rewrite_bdd_compile_ms", rewriteStats.totalBddBuildMs);
                addGraphRewriteInfo("graph_rewrite_bdd_wmc_ms", rewriteStats.totalBddWmcMs);
                addGraphRewriteInfo("graph_rewrite_fast_general_ms",
                        rewriteStats.totalFastGeneralMs);
                addGraphRewriteInfo("graph_rewrite_apply_ms", rewriteStats.totalApplyMs);
                addGraphRewriteInfo("graph_rewrite_compaction_ms", rewriteStats.totalCompactionMs);
                addGraphRewriteInfo("graph_rewrite_cleanup_ms", rewriteStats.totalCleanupMs);
                addGraphRewriteTextInfo("graph_rewrite_detected_regions",
                        std::to_string(rewriteStats.numRegionsDetected));
                addGraphRewriteTextInfo("graph_rewrite_detected_region_total_edges",
                        std::to_string(rewriteStats.totalDetectedRegionEdges));
                addGraphRewriteTextInfo("graph_rewrite_detected_region_total_nodes",
                        std::to_string(rewriteStats.totalDetectedRegionNodes));
                debugger.addInfo("graph_rewrite_general_regions",
                        std::to_string(rewriteStats.numGeneralRegionsRewritten));
                rewriteHybridStage->logMessage(Level::INFO,
                        "graph_rewrite_general_regions=" +
                                std::to_string(rewriteStats.numGeneralRegionsRewritten));
                debugger.addInfo("graph_rewrite_fast_general_regions",
                        std::to_string(rewriteStats.numFastGeneralRegions));
                rewriteHybridStage->logMessage(Level::INFO,
                        "graph_rewrite_fast_general_regions=" +
                                std::to_string(rewriteStats.numFastGeneralRegions));
            }
        }
        if (opt.isDumpDotEnabled()) {
            view.dumpDot(makeOutputPath(opt, "rewrite_final.dot"));
        }
        if (opt.isDumpJsonEnabled()) {
            view.dumpJson(makeOutputPath(opt, "rewrite_final.json"));
        }
    } else if (opt.isRewriteEnabled() && opt.isDerivationOnly()) {
        std::cout << "[pipeline] derivation-only mode; skip rewrite" << std::endl;
    }

    precomputedTupleProbResult.insert(liftedProbabilities.begin(), liftedProbabilities.end());
    if (program.getKnowledge() == souffle::Knowledge::BDD) {
        runBddPipeline(opt, program, ruleManager, queryManager, *graph, view, graphEvidences, rewriteHybridStage);
    } else {
        std::cerr << "Unknown knowledge representation" << std::endl;
    }
    setActiveSymbolTable(nullptr);
}

}  // namespace souffle::problog::full_detail

namespace souffle::problog {
void runFullPipeline(const CmdOptions& opt, SouffleProgram& program,
        RuleManager& ruleManager, QueryManager& queryManager,
        const std::unordered_map<UntypedTuple, double>& factProb,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences) {
    full_detail::setExactInferenceMode(true);
    full_detail::runPipeline(opt, program, ruleManager, queryManager, factProb, evidences);
}
}  // namespace souffle::problog
