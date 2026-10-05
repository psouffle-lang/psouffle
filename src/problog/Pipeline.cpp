#include "souffle/problog/Pipeline.h"

#include "souffle/Derivation.h"
#include "souffle/cli/Cli.h"
#include "souffle/problog/DerivationGraph.h"
#include "souffle/problog/ForwardCompilation.h"
#include "souffle/problog/QueryManager.h"
#include "souffle/problog/RuleManager.h"
#include "souffle/problog/debug/Debugger.h"
#include "souffle/problog/formula/CuddManager.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

Debugger& debugger = Debugger::getInstance();

void assertProbabilityInRange(double p, const std::string& ctx) {
    if (p < 0.0 || p > 1.0) {
        std::cerr << "[ForwardCompilation] invalid probability " << p << " at " << ctx << std::endl;
        assert(false && "probability out of [0,1]");
    }
}

std::unordered_map<UntypedTuple, std::vector<EdgePtr>> buildDeletedOutEdges(
        const std::set<EdgePtr>& deletedEdges) {
    std::unordered_map<UntypedTuple, std::vector<EdgePtr>> deletedOutEdges;
    for (const auto& edge : deletedEdges) {
        if (!edge) {
            continue;
        }
        for (const auto& input : edge->getInputs()) {
            if (!input) {
                continue;
            }
            deletedOutEdges[input->getTuple()].push_back(edge);
        }
    }
    return deletedOutEdges;
}

void collectImpactUnionWithDeletedEdges(
        const IncrementalDerivationGraphViewInterface& view,
        const std::vector<NodePtr>& sources,
        const std::unordered_map<UntypedTuple, std::vector<EdgePtr>>& deletedOutEdges,
        std::unordered_set<NodePtr>& outNodes,
        std::unordered_set<EdgePtr>& outEdges) {
    if (sources.empty()) {
        return;
    }
    const auto& liveNodes = view.getNodes();
    const auto& liveEdges = view.getEdges();
    std::unordered_map<UntypedTuple, NodePtr> liveNodeByTuple;
    liveNodeByTuple.reserve(liveNodes.size());
    for (const auto& node : liveNodes) {
        if (!node) {
            continue;
        }
        liveNodeByTuple.emplace(node->getTuple(), node);
    }

    std::queue<NodePtr> liveQueue;
    std::queue<UntypedTuple> deletedQueue;
    std::unordered_set<UntypedTuple> deletedVisited;
    auto enqueueByTuple = [&](const UntypedTuple& tuple) {
        auto liveIt = liveNodeByTuple.find(tuple);
        if (liveIt != liveNodeByTuple.end()) {
            if (outNodes.insert(liveIt->second).second) {
                liveQueue.push(liveIt->second);
            }
            return;
        }
        if (deletedOutEdges.count(tuple) && deletedVisited.insert(tuple).second) {
            deletedQueue.push(tuple);
        }
    };

    for (const auto& src : sources) {
        if (!src) {
            continue;
        }
        if (outNodes.insert(src).second) {
            liveQueue.push(src);
        }
    }

    while (!liveQueue.empty() || !deletedQueue.empty()) {
        while (!liveQueue.empty()) {
            NodePtr cur = liveQueue.front();
            liveQueue.pop();
            const bool curIsLive = liveNodes.count(cur) > 0;
            if (curIsLive) {
                for (const auto& e : cur->getOutgoingEdges()) {
                    if (!liveEdges.count(e)) {
                        continue;
                    }
                    outEdges.insert(e);
                    NodePtr nxt = e->getOutput();
                    if (nxt && liveNodes.count(nxt) && outNodes.insert(nxt).second) {
                        liveQueue.push(nxt);
                    }
                }
            }
            auto it = deletedOutEdges.find(cur->getTuple());
            if (it != deletedOutEdges.end()) {
                for (const auto& e : it->second) {
                    outEdges.insert(e);
                    NodePtr nxt = e->getOutput();
                    if (!nxt) {
                        continue;
                    }
                    enqueueByTuple(nxt->getTuple());
                }
            }
        }
        if (deletedQueue.empty()) {
            continue;
        }
        UntypedTuple curTuple = deletedQueue.front();
        deletedQueue.pop();
        auto it = deletedOutEdges.find(curTuple);
        if (it == deletedOutEdges.end()) {
            continue;
        }
        for (const auto& e : it->second) {
            if (!e) {
                continue;
            }
            outEdges.insert(e);
            NodePtr nxt = e->getOutput();
            if (!nxt) {
                continue;
            }
            enqueueByTuple(nxt->getTuple());
        }
    }
}

namespace souffle::problog {

namespace {
struct RuleAppInventory {
    std::size_t headTuples = 0;
    std::size_t nullRuleSets = 0;
    std::size_t totalRuleApps = 0;
    std::size_t totalBindings = 0;
    std::size_t maxRuleAppsPerHead = 0;
    std::size_t uniqueRuleIds = 0;
};

static std::size_t countInitialInputFacts() {
    return inputFactSet.size();
}

static RuleAppInventory collectRuleAppInventory() {
    RuleAppInventory inv;
    std::unordered_set<souffle::RamDomain> uniqueRuleIds;
    uniqueRuleIds.reserve(DerivationManager::untypedTuple2RuleApplications.size());
    for (const auto& [tuple, ruleSetPtr] : DerivationManager::untypedTuple2RuleApplications) {
        (void)tuple;
        ++inv.headTuples;
        if (ruleSetPtr == nullptr) {
            ++inv.nullRuleSets;
            continue;
        }
        inv.maxRuleAppsPerHead = std::max(inv.maxRuleAppsPerHead, ruleSetPtr->size());
        inv.totalRuleApps += ruleSetPtr->size();
        for (const auto& ruleApp : *ruleSetPtr) {
            inv.totalBindings += ruleApp.varValuesPure.size();
            uniqueRuleIds.insert(ruleApp.ruleId);
        }
    }
    inv.uniqueRuleIds = uniqueRuleIds.size();
    return inv;
}

static void writeJsonEscapedString(std::ostream& out, const std::string& value) {
    out.put('"');
    for (unsigned char c : value) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out << buf;
                } else {
                    out.put(static_cast<char>(c));
                }
        }
    }
    out.put('"');
}

static bool isFactLikeRule(const Rule* rule) {
    return rule != nullptr && (rule->isFact() || rule->getBodyAtoms().empty());
}

struct CachedRuleDumpInfo {
    const Rule* rule = nullptr;
    std::vector<std::string> vars;
};

static const CachedRuleDumpInfo& getCachedRuleDumpInfo(
        std::unordered_map<souffle::RamDomain, CachedRuleDumpInfo>& cache,
        const RuleManager& ruleManager,
        souffle::RamDomain ruleId) {
    auto it = cache.find(ruleId);
    if (it != cache.end()) {
        return it->second;
    }
    CachedRuleDumpInfo info;
    info.rule = ruleManager.getRule(ruleId);
    if (info.rule != nullptr) {
        info.vars = info.rule->getVars();
    }
    auto [insertedIt, _] = cache.emplace(ruleId, std::move(info));
    return insertedIt->second;
}

static void dumpRuleAppsBeforeGraphJson(
        const CmdOptions& opt,
        const RuleManager& ruleManager,
        const std::unordered_map<UntypedTuple, double>& factProb) {
    const std::string outputPath = makeOutputPath(opt, "derivation-before-graph.json");
    std::ofstream out(outputPath);
    if (!out.good()) {
        throw std::runtime_error("failed to open pre-graph dump output: " + outputPath);
    }

    out << std::setprecision(17);
    std::unordered_map<souffle::RamDomain, CachedRuleDumpInfo> ruleCache;
    ruleCache.reserve(ruleManager.size());

    out << "{\"facts\":[";
    bool firstFact = true;
    auto emitFact = [&](const UntypedTuple& tuple, double probability) {
        if (!firstFact) {
            out << ',';
        }
        firstFact = false;
        out << "{\"name\":";
        writeJsonEscapedString(out, tuple.toString());
        out << ",\"probability\":" << probability << "}";
    };

    for (const auto& [tuple, probability] : factProb) {
        emitFact(tuple, probability);
    }

    std::unordered_set<UntypedTuple> emittedDerivedFacts;
    for (const auto& [headTuple, ruleSetPtr] : DerivationManager::untypedTuple2RuleApplications) {
        if (ruleSetPtr == nullptr) {
            continue;
        }
        for (const auto& ruleApp : *ruleSetPtr) {
            const auto& cached = getCachedRuleDumpInfo(ruleCache, ruleManager, ruleApp.ruleId);
            if (!isFactLikeRule(cached.rule)) {
                continue;
            }
            if (factProb.find(headTuple) != factProb.end()) {
                continue;
            }
            if (!emittedDerivedFacts.insert(headTuple).second) {
                continue;
            }
            emitFact(headTuple, cached.rule ? cached.rule->getProbability() : 1.0);
        }
    }

    out << "],\"rules\":[";
    bool firstRule = true;
    for (const auto& [headTuple, ruleSetPtr] : DerivationManager::untypedTuple2RuleApplications) {
        if (ruleSetPtr == nullptr) {
            continue;
        }
        for (const auto& ruleApp : *ruleSetPtr) {
            const auto& cached = getCachedRuleDumpInfo(ruleCache, ruleManager, ruleApp.ruleId);
            const Rule* rule = cached.rule;
            if (rule == nullptr || isFactLikeRule(rule)) {
                continue;
            }
            if (!firstRule) {
                out << ',';
            }
            firstRule = false;
            out << "{\"head\":";
            writeJsonEscapedString(out, headTuple.toString());
            out << ",\"probability\":" << rule->getProbability();
            out << ",\"rule_id\":" << ruleApp.ruleId;
            out << ",\"mapping\":[";
            for (std::size_t i = 0; i < ruleApp.varValuesPure.size(); ++i) {
                if (i != 0) {
                    out << ',';
                }
                out << ruleApp.varValuesPure[i];
            }
            out << "],\"bodies\":[";
            bool firstBody = true;
            for (const auto& bodyAtom : rule->getBodyAtoms()) {
                if (!firstBody) {
                    out << ',';
                }
                firstBody = false;
                const UntypedTuple bodyTuple{
                        bodyAtom.getRelation(),
                        bodyAtom.instantiatedFields(cached.vars, ruleApp.varValuesPure)};
                out << "{\"negation\":" << (bodyAtom.isNegatedAtom() ? "true" : "false")
                    << ",\"name\":";
                writeJsonEscapedString(out, bodyTuple.toString());
                out << "}";
            }
            out << "]}";
        }
    }
    out << "]}";
    out.close();
    if (DerivationGraphViewInterface::isVerboseEnabled()) {
        std::cout << "[pipeline] wrote pre-graph ruleapp JSON to " << outputPath << std::endl;
    }
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

static IncSubgraphView buildFullIncViewLocal(IncrementalDerivationGraph& graph) {
    return IncSubgraphView(graph.getNodes(), graph.getEdges(), {}, {}, {}, {});
}

static std::size_t precomputeIsolatedOutputFactsLocal(IncSubgraphView& view) {
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

static bool isSemanticRandomProb(double p) {
    return p > 0.0 && p < 1.0;
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

static std::vector<std::vector<std::pair<NodePtr, bool>>> groupEvidencesByComponent(
        const DerivationGraphViewInterface& view,
        const std::vector<std::pair<NodePtr, bool>>& evidences) {
    auto& depGraph = view.getCycleDependencyGraph();
    const size_t componentCount = depGraph.getComponentCount();
    std::vector<std::vector<std::pair<NodePtr, bool>>> byComponent(componentCount);
    std::vector<std::unordered_map<NodePtr, bool>> seen(componentCount);

    for (const auto& ev : evidences) {
        const NodePtr& node = ev.first;
        if (!node || view.getNodes().count(node) == 0) {
            throw std::runtime_error("Evidence node not found in view: " +
                    (node ? node->toString() : std::string("null")));
        }
        size_t cid = depGraph.getComponentId(node);
        auto& seenMap = seen[cid];
        auto it = seenMap.find(node);
        if (it != seenMap.end()) {
            if (it->second != ev.second) {
                throw std::runtime_error("Conflicting evidence for node: " + node->toString());
            }
            continue;
        }
        seenMap.emplace(node, ev.second);
        byComponent[cid].push_back(ev);
    }
    return byComponent;
}

} // namespace

std::string makeOutputPath(const CmdOptions& opt, const std::string& filename) {
    return joinOutputPath(opt.getOutputFileDir(), filename);
}

static std::vector<std::pair<NodePtr, bool>> applyEvidence(
        IncrementalDerivationGraph& graph,
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
        SouffleProgram& program,
        RuleManager& ruleManager,
        QueryManager& queryManager,
        std::unique_ptr<IncrementalDerivationGraph>& graph,
        SubgraphView& view,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences,
        bool enableOnlineCli) {
    Debugger& debugger = Debugger::getInstance();

    std::map<NodePtr, BddNodeRef> nodeFormulas;
    std::map<EdgePtr, BddNodeRef> edgeFormulas;
    std::unique_ptr<WeightedBDDManager> bddManager;
    std::unordered_set<NodePtr> seedTrueNodes;
    seedTrueNodes.reserve(view.getNodes().size());
    for (const auto& n : view.getNodes()) {
        const std::string s = n->getTuple().toString();
        if (s.rfind("@magic.", 0) != 0) continue;

        if (view.getIncomingEdges(n).empty()) {
            seedTrueNodes.insert(n);
        }
    }
    {
        {
            auto* fcStage = debugger.startStage(StageKind::FORWARD_COMPILATION_FULL);
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
            auto heartbeatCallback = [&](const FcHeartbeatSnapshot& hb) {
                std::size_t liveNodes = 0;
                if (bddManager) {
                    liveNodes = bddManager->getLiveNodeCount();
                }
                recordFcHeartbeat(debugger, fcStage, hb, "full", 0, 0,
                        std::numeric_limits<std::size_t>::max(), liveNodes);
            };
            buildFormulasCyclewise(view, *bddManager, nodeFormulas, edgeFormulas, seedTrueNodes, nullptr,
                    heartbeatCallback);
            auto t1 = std::chrono::steady_clock::now();
            if (opt.isVerboseEnabled()) {
                std::cout << "[pipeline] BDD formula build took "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                          << " ms\n";
            }
            debugger.endStage();

            debugger.startStage(StageKind::WEIGHTED_MODEL_COUNTING_FULL);

            auto t2 = std::chrono::steady_clock::now();
            auto resolvedEvs = applyEvidence(*graph, evidences);
            auto t3 = std::chrono::steady_clock::now();

            auto components = buildComponentSubgraphs(view);
            auto evidencesByComponent = groupEvidencesByComponent(view, resolvedEvs);
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
            auto computeWmcProfile = [&](const BddNodeRef& node, double& ms, std::size_t& calls) {
                calls++;
                if (!wmcProfile) {
                    return bddManager->computeWeightedModelCount(node);
                }
                auto wmcStart = Clock::now();
                double res = bddManager->computeWeightedModelCount(node);
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
                double evidenceWeight = 1.0;
                if (!componentEvs.empty()) {
                    evidenceWeight = computeWmcProfile(evidenceBdd, evidenceWmcComputeMs, evidenceWmcCalls);
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
                    } else if (evidenceWeight == 0.0) {
                        prob = 0.0;
                    } else {
                        auto joint = makeAndProfile(bdd, evidenceBdd, nodeMakeAndMs, nodeMakeAndCalls);
                        double jointW = computeWmcProfile(joint, nodeWmcComputeMs, nodeWmcCalls);
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

            if (opt.isVerboseEnabled()) {
                std::cout << "[pipeline] evidence resolve/tag took "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count()
                          << " ms\n";
                std::cout << "[pipeline] component evidence build took " << evidenceBuildMs << " ms\n";
                std::cout << "[pipeline] component evidence WMC took " << evidenceWmcMs << " ms\n";
                std::cout << "[pipeline] per-node conditional WMC took "
                          << perNodeWmcMs << " ms\n";
            }
            if (wmcProfile) {
                std::cout << "[wmc-profile] stage=FULL"
                          << " mode=full"
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
            debugger.startStage(StageKind::IO_DUMP_FULL);
            auto tDumpStart = std::chrono::steady_clock::now();
            dumpProbabilities(probResult, opt.getOutputFileDir());
            auto tDumpMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - tDumpStart)
                                   .count();
            if (opt.isVerboseEnabled()) {
                std::cout << "[pipeline] probability dump took " << tDumpMs << " ms\n";
            }
            debugger.endStage();
        }
    }

    debugger.endTurn();

    if (enableOnlineCli) {
        IncrementalCLI<BddNodeRef> cli(
                &program, graph.get(), &graph, &ruleManager, &queryManager, bddManager.get(), &nodeFormulas,
                &edgeFormulas);
        cli.setCmdOptions(opt);
        cli.run();
    }
}

static const char* fullRuntimeLaneLabel() {
    return "exact";
}

static const char* knowledgeBackendLabel(Knowledge knowledge) {
    switch (knowledge) {
        case Knowledge::BDD:
            return "bdd";
        default:
            return "unknown";
    }
}

static void configureRuntimeFromOptions(const CmdOptions& opt) {
    fcProfileEnabled = opt.isFcProfileEnabled();
    incDeleteProfileEnabled = opt.isIncDeleteProfileEnabled();
    wmcProfileEnabled = opt.isWmcProfileEnabled();
    incRegionalProfileEnabled = opt.isIncRegionalProfileEnabled();
    depGraphProfileEnabled = opt.isDepGraphProfileEnabled();
    incReorderPolicy = opt.getIncReorderPolicy();
    incReorderAutoGap = opt.getIncReorderAutoGap();
    incReorderWorkThreshold = opt.getIncReorderWorkThreshold();
    incReorderCountDead = opt.isIncReorderCountDeadEnabled();
    incReorderAllowLarge = opt.isIncReorderAllowLargeEnabled();
    DerivationGraphViewInterface::setDumpDotEnabled(opt.isDumpDotEnabled());
    DerivationGraphViewInterface::setDumpJsonEnabled(opt.isDumpJsonEnabled());
    DerivationGraphViewInterface::setDumpStatsEnabled(opt.isDumpStatEnabled());
    DerivationGraphViewInterface::setVerboseEnabled(opt.isVerboseEnabled());
    setFunctionTimerOutputEnabled(opt.isVerboseEnabled());
    DerivationGraphViewInterface::setDumpOutputDir(opt.getOutputFileDir());
}

static void runKnowledgeRuntimeLane(const CmdOptions& opt, SouffleProgram& program,
        RuleManager& ruleManager, QueryManager& queryManager,
        std::unique_ptr<IncrementalDerivationGraph>& graph, IncSubgraphView& view,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences, bool allowOnlineCli) {
    switch (program.getKnowledge()) {
        case souffle::Knowledge::BDD:
            runBddPipeline(
                    opt, program, ruleManager, queryManager, graph, view, evidences, allowOnlineCli);
            return;
        default:
            std::cerr << "Unknown knowledge representation" << std::endl;
            return;
    }
}

void runPipeline(
        const CmdOptions& opt,
        SouffleProgram& program,
        RuleManager& ruleManager,
        QueryManager& queryManager,
        const std::unordered_map<UntypedTuple, double>& factProb,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences,
        bool enableOnlineCli) {
    if (opt.isRewriteEnabled()) {
        throw std::runtime_error("Online initialization and recomputation cannot rewrite the graph");
    }
    std::unordered_map<std::string, std::vector<char>> relationTypes;
    for (auto* relation : program.getAllRelations()) {
        auto& types = relationTypes[relation->getName()];
        for (std::size_t i = 0; i < relation->getArity(); ++i) {
            const char* type = relation->getAttrType(i);
            types.push_back(type && type[0] ? type[0] : '?');
        }
    }
    ScopedUntypedTupleRenderingContext rendering(&program.getSymbolTable(), std::move(relationTypes));
    std::cout << std::fixed << std::setprecision(8);
    Debugger& debugger = Debugger::getInstance();
    configureRuntimeFromOptions(opt);
    precomputedProbResult.clear();
    precomputedTupleProbResult.clear();
    debugger.addInfo("full_runtime_lane", fullRuntimeLaneLabel());
    debugger.addInfo("knowledge_backend", knowledgeBackendLabel(program.getKnowledge()));

    if (!evidences.empty()) {
        throw std::runtime_error("Evidence is not supported by the incremental artifact runtime.");
    }
    for (const auto* rule : ruleManager.getAllRules()) {
        if (!rule->getAggregates().empty()) {
            throw std::runtime_error("Aggregate replay requires standalone full execution; online aggregate updates are not supported.");
        }
    }

    debugger.startStage(StageKind::CREATE_GRAPH_FULL);
    debugger.addInfo("input_fact_size", std::to_string(countInitialInputFacts()));
    const RuleAppInventory ruleAppInventory = collectRuleAppInventory();
    debugger.addInfo("ruleapp_head_tuples", std::to_string(ruleAppInventory.headTuples));
    debugger.addInfo("ruleapp_null_sets", std::to_string(ruleAppInventory.nullRuleSets));
    debugger.addInfo("ruleapp_total", std::to_string(ruleAppInventory.totalRuleApps));
    debugger.addInfo("ruleapp_total_bindings", std::to_string(ruleAppInventory.totalBindings));
    debugger.addInfo("ruleapp_max_per_head", std::to_string(ruleAppInventory.maxRuleAppsPerHead));
    debugger.addInfo("ruleapp_unique_rules", std::to_string(ruleAppInventory.uniqueRuleIds));
    const double avgRuleAppsPerHead = ruleAppInventory.headTuples
            ? static_cast<double>(ruleAppInventory.totalRuleApps) /
                    static_cast<double>(ruleAppInventory.headTuples)
            : 0.0;
    const double avgBindingsPerRuleApp = ruleAppInventory.totalRuleApps
            ? static_cast<double>(ruleAppInventory.totalBindings) /
                    static_cast<double>(ruleAppInventory.totalRuleApps)
            : 0.0;
    debugger.addInfo("ruleapp_avg_per_head", std::to_string(avgRuleAppsPerHead));
    debugger.addInfo("ruleapp_avg_bindings", std::to_string(avgBindingsPerRuleApp));
    if (opt.isVerboseEnabled()) {
        std::cout << "[pipeline] recorded ruleapps: heads=" << ruleAppInventory.headTuples
                  << " null_sets=" << ruleAppInventory.nullRuleSets
                  << " total=" << ruleAppInventory.totalRuleApps
                  << " unique_rules=" << ruleAppInventory.uniqueRuleIds
                  << " avg_per_head=" << avgRuleAppsPerHead
                  << " avg_bindings=" << avgBindingsPerRuleApp
                  << " max_per_head=" << ruleAppInventory.maxRuleAppsPerHead << std::endl;
    }
    if (opt.isDumpJsonBeforeGraphEnabled()) {
        const auto tDump0 = std::chrono::steady_clock::now();
        dumpRuleAppsBeforeGraphJson(opt, ruleManager, factProb);
        const auto tDump1 = std::chrono::steady_clock::now();
        debugger.addInfo("dumpjson_before_graph_ms", std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(tDump1 - tDump0).count()));
    }
    auto t0 = std::chrono::steady_clock::now();
    auto graph = std::unique_ptr<IncrementalDerivationGraph>(IncrementalDerivationGraph::createFrom(
            DerivationManager::untypedTuple2RuleApplications, ruleManager, queryManager, factProb, evidences));
    auto t1 = std::chrono::steady_clock::now();
    const GraphSummary createdSummary = summarizeGraphLight(*graph);
    addGraphSummaryInfo(debugger, "graph_", createdSummary);
    if (opt.isVerboseEnabled()) {
        std::cout << "[pipeline] create graph took "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
                  << " ms\n";
    }
    debugger.endStage();

    if (opt.isDumpDotEnabled()) {
        graph->dumpDot(makeOutputPath(opt, "before_prune.dot"));
    }
    if (opt.isDumpJsonBeforePruneEnabled()) {
        graph->dumpJson(makeOutputPath(opt, "derivation-before-prune.json"));
    }

    debugger.startStage(StageKind::PRUNING_FULL);
    auto t2 = std::chrono::steady_clock::now();
    IncSubgraphView view = buildFullIncViewLocal(*graph);
    const GraphSummary beforePrune = summarizeGraphLight(view);
    addGraphSummaryInfo(debugger, "before_", beforePrune);
    view = graph->prune(program.getOutputRelations());

    const GraphSummary afterPrune = summarizeGraphLight(view);
    addGraphSummaryInfo(debugger, "after_", afterPrune);
    debugger.addInfo("removed_nodes", std::to_string(
            beforePrune.nodes > afterPrune.nodes ? beforePrune.nodes - afterPrune.nodes : 0));
    debugger.addInfo("removed_edges", std::to_string(
            beforePrune.edges > afterPrune.edges ? beforePrune.edges - afterPrune.edges : 0));
    auto t3 = std::chrono::steady_clock::now();
    if (opt.isVerboseEnabled()) {
        std::cout << "[pipeline] pruning took "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count()
                  << " ms\n";
    }
    debugger.endStage();

    if (opt.isDumpDotEnabled()) {
        view.dumpDot(makeOutputPath(opt, "after_prune.dot"));
    }
    if (opt.isDumpJsonEnabled()) {
        view.dumpJson(makeOutputPath(opt, "derivation.json"));
    }
    if (opt.isVerboseEnabled()) {
        std::cout << "[pipeline] selected runtime lane=" << fullRuntimeLaneLabel() << std::endl;
    }

    runKnowledgeRuntimeLane(
            opt, program, ruleManager, queryManager, graph, view, evidences, enableOnlineCli);
}

}  // namespace souffle::problog

// Keep graph/FC header definitions in one translation unit.
#include "FullPipeline.cpp"
