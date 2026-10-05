#ifndef CLI_H
#define CLI_H
#include <chrono>
#include <ctime>
#include <array>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <regex>
#include <iomanip>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <memory>
#include <stdexcept>
#include <readline/readline.h>
#include <readline/history.h>
#include "souffle/SouffleInterface.h"
#include "souffle/Derivation.h"
#include "souffle/cli/Command.h"
#include "souffle/cli/PendingOperation.h"
#include "souffle/cli/PendingOperationStager.h"
#include "souffle/problog/DerivationGraph.h"
#include "souffle/problog/RuleManager.h"
#include "souffle/problog/formula/CuddManager.h"
#include "souffle/problog/ForwardCompilation.h"
#include "souffle/CompiledOptions.h"
#include <unistd.h> // Required for isatty()

namespace souffle::cli {
template <typename NodeRef>
class IncrementalCommandExecutor;
}

using SemMode = souffle::SemMode;
using FcMode = souffle::FcMode;
using WmcMode = souffle::WmcMode;
using IncrementalModeSpec = souffle::IncrementalModeSpec;
using FcConsumerClass = souffle::FcConsumerClass;
using FcStateClass = souffle::FcStateClass;

inline std::string makeTimestampLabel() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTm{};
#if defined(_MSC_VER)
    localtime_s(&localTm, &nowTime);
#else
    if (auto* tmPtr = std::localtime(&nowTime)) {
        localTm = *tmPtr;
    }
#endif
    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y%m%d-%H%M%S");
    return oss.str();
}

inline std::string makeTimestampedFilename(const std::string& prefix, size_t iteration,
        const std::string& extension) {
    return prefix + std::to_string(iteration) + "-" + makeTimestampLabel() + extension;
}

template<typename NodeRef>
class IncrementalCLI {
private:
    template <typename>
    friend class souffle::cli::IncrementalCommandExecutor;

    using ParsedCommand = souffle::cli::ParsedCommand;
    using Operation = souffle::cli::PendingOperation;

    // Store all pending operations
    std::vector<Operation> pendingOperations;

    void logTurnMode(const std::string& modeLabel) const {
        if (opt.isVerboseEnabled()) {
            std::cout << "[inc-iter " << iteration << "] mode=" << modeLabel << std::endl;
        }
    }

    IncrementalModeSpec currentModeSpec() const {
        return modeSpec;
    }

    IncrementalModeSpec resolveCommittedModeSpec() const {
        IncrementalModeSpec resolved = modeSpec;
        if (fcStateClass != FcStateClass::NORMALIZED &&
                classifyFcConsumer(resolved.fc) == FcConsumerClass::REGIONAL) {
            resolved.fc = FcMode::INC_NAIVE;
        }
        return resolved;
    }

    bool shouldFallbackRegionalConsumer(
            const IncrementalModeSpec& requested, const IncrementalModeSpec& effective) const {
        return requested.fc != effective.fc &&
               classifyFcConsumer(requested.fc) == FcConsumerClass::REGIONAL &&
               classifyFcConsumer(effective.fc) == FcConsumerClass::NORMALIZING;
    }

    bool isIncrementalSemMode() const {
        return souffle::isIncrementalSemMode(modeSpec.sem);
    }

    bool isFullSemMode() const {
        return souffle::isFullSemMode(modeSpec.sem);
    }

    bool isIncrementalFcMode() const {
        return souffle::isIncrementalFcMode(modeSpec.fc);
    }

    bool isFullFcMode() const {
        return souffle::isFullFcMode(modeSpec.fc);
    }

    bool isRegionalFcMode() const {
        return souffle::isRegionalFcMode(modeSpec.fc);
    }

    const char* semModeLabel() const {
        return souffle::semModeLabel(modeSpec.sem);
    }

    const char* fcModeLabel() const {
        return souffle::fcModeLabel(modeSpec.fc);
    }

    WmcMode getWmcMode() const {
        return souffle::getWmcMode(modeSpec.fc);
    }

    const char* wmcModeLabel() const {
        return souffle::wmcModeLabel(getWmcMode());
    }

    std::string modeSummaryLabel() const {
        return souffle::modeSummaryLabel(currentModeSpec());
    }

    void logCommittedModeResolution(
            const IncrementalModeSpec& requested, const IncrementalModeSpec& effective) const {
        if (!shouldFallbackRegionalConsumer(requested, effective)) {
            return;
        }
        if (opt.isVerboseEnabled()) {
            std::cout << "[cli] fc-state=" << souffle::fcStateClassLabel(fcStateClass)
                      << " requested=" << souffle::modeSummaryLabel(requested)
                      << " fallback=" << souffle::modeSummaryLabel(effective) << std::endl;
        }
    }

    void updateFcStateAfterTurn(
            const IncrementalModeSpec& requested, const IncrementalModeSpec& effective) {
        lastRequestedTurnMode = requested;
        lastEffectiveTurnMode = effective;
        lastTurnFallbackToNormalizer = shouldFallbackRegionalConsumer(requested, effective);
        lastTurnBoundaryTotal = 0;
        lastTurnRegionNodes = 0;
        lastTurnDeltaReachNodes = 0;
        lastTurnTrueRegionalReuse = false;
        lastTurnUsedCalibrationOverrides = false;

        if (classifyFcConsumer(effective.fc) == FcConsumerClass::REGIONAL) {
            lastTurnBoundaryTotal = incRegionalTurnSummary.boundaryTotal;
            lastTurnRegionNodes = incRegionalTurnSummary.regionNodeCount;
            lastTurnDeltaReachNodes = incRegionalTurnSummary.deltaReachNodeCount;
            lastTurnTrueRegionalReuse = incRegionalTurnSummary.valid &&
                    incRegionalTurnSummary.trueRegionalReuse;
            lastTurnUsedCalibrationOverrides =
                    incRegionalTurnSummary.valid && incRegionalTurnSummary.usedCalibrationOverrides;

            if (lastTurnTrueRegionalReuse) {
                fcStateClass = FcStateClass::REGIONALIZED;
                consecutiveRegionalTurns += 1;
            } else {
                fcStateClass = FcStateClass::NORMALIZED;
                consecutiveRegionalTurns = 0;
            }
        } else {
            fcStateClass = FcStateClass::NORMALIZED;
            consecutiveRegionalTurns = 0;
        }
    }

    const char* debuggerTurnModeLabel() const {
        return souffle::debuggerTurnModeLabel(currentModeSpec());
    }

    bool currentModeUsesIncrementalState() const {
        return souffle::modeUsesIncrementalState(currentModeSpec());
    }

    std::string outputPath(const std::string& filename) const {
        return souffle::joinOutputPath(opt.getOutputFileDir(), filename);
    }

    std::string outputTimestampedPath(const std::string& prefix, size_t iter,
            const std::string& extension) const {
        return outputPath(makeTimestampedFilename(prefix, iter, extension));
    }

    std::string currentTurnProbabilityPrefix() const {
        return souffle::makeOnlineTurnProbabilityPrefix(iteration, currentModeSpec());
    }

    void collectGarbageIfNeeded() {
        if (ddManager != nullptr) {
            ddManager->tryGarbageCollection();
        }
    }

    void beginTurnTrace() {
        debugger.startTurn(debuggerTurnModeLabel());
        DerivationGraphViewInterface::setDumpOutputDir(opt.getOutputFileDir());
        logTurnMode(modeSummaryLabel());
    }

    void finishTurn() {
        collectGarbageIfNeeded();
        debugger.endTurn();
        iteration++;
    }

    void dumpCurrentTurnProbabilities() {
        dumpProbabilities(probResult, opt.getOutputFileDir(), currentTurnProbabilityPrefix());
    }

    void dumpCurrentTurnProbabilitiesWithStage(StageKind stageKind) {
        debugger.startStage(stageKind);
        dumpCurrentTurnProbabilities();
        debugger.endStage();
    }

    void assertModeCompatibleWithGraphState(const IncrementalModeSpec&) const {
    }

    void assertCurrentModeCompatibleWithGraphState() const {
        assertModeCompatibleWithGraphState(currentModeSpec());
    }

    static std::string formatRatio(size_t part, size_t total) {
        if (total == 0) {
            return "n/a";
        }
        const double pct = 100.0 * static_cast<double>(part) / static_cast<double>(total);
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << pct << "%";
        return oss.str();
    }

    void logApplyDeltaSummary(const IncrementalDerivationGraph& graph, const std::string& modeLabel) const {
        const size_t insNodes = graph.getDeltaInsertNodes().size();
        const size_t insEdges = graph.getDeltaInsertEdges().size();
        const size_t delNodes = graph.getDeltaDeleteNodes().size();
        const size_t delEdges = graph.getDeltaDeleteEdges().size();
        std::ostringstream oss;
        oss << "[inc-iter " << iteration << "] mode=" << modeLabel
            << " apply_delta_view: insNodes=" << insNodes
            << " insEdges=" << insEdges
            << " delNodes=" << delNodes
            << " delEdges=" << delEdges;
        if (opt.isVerboseEnabled()) {
            std::cout << oss.str() << std::endl;
        }
        debugger.logMessage(Level::INFO, oss.str());
    }

    void logApplyDeltaGraphSummary(const IncrementalDerivationGraph& graph, const std::string& modeLabel) const {
        const size_t totalNodes = graph.getNodes().size();
        const size_t totalEdges = graph.getEdges().size();
        std::ostringstream oss;
        oss << "[inc-iter " << iteration << "] mode=" << modeLabel
            << " apply_delta_graph: totalNodes=" << totalNodes
            << " totalEdges=" << totalEdges;
        if (opt.isVerboseEnabled()) {
            std::cout << oss.str() << std::endl;
        }
        debugger.logMessage(Level::INFO, oss.str());
    }

    void logApplyDeltaOpsSummary(
            const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& deltaInsertRuleApps,
            const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& deltaDeleteRuleApps,
            const std::unordered_map<UntypedTuple, double>& factProb,
            const std::vector<UntypedTuple>& deletedFacts,
            const std::string& modeLabel) const {
        auto countRuleApps = [](const auto& m) {
            size_t total = 0;
            for (const auto& [_, s] : m) {
                total += s ? s->size() : 0;
            }
            return total;
        };
        const size_t delTuples = deltaDeleteRuleApps.size();
        const size_t delRuleApps = countRuleApps(deltaDeleteRuleApps);
        const size_t delFacts = deletedFacts.size();
        const size_t insTuples = deltaInsertRuleApps.size();
        const size_t insRuleApps = countRuleApps(deltaInsertRuleApps);
        const size_t insFacts = factProb.size();
        std::ostringstream oss;
        oss << "[inc-iter " << iteration << "] mode=" << modeLabel
            << " apply_delta_ops: delTuples=" << delTuples
            << " delRuleApps=" << delRuleApps
            << " delFacts=" << delFacts
            << " insTuples=" << insTuples
            << " insRuleApps=" << insRuleApps
            << " insFacts=" << insFacts;
        if (opt.isVerboseEnabled()) {
            std::cout << oss.str() << std::endl;
        }
        debugger.logMessage(Level::INFO, oss.str());
    }

    void logPrunedDeltaSummary(const IncSubgraphView& view, const std::string& modeLabel) const {
        const size_t insNodes = view.getDeltaInsertNodes().size();
        const size_t insEdges = view.getDeltaInsertEdges().size();
        const size_t delNodes = view.getDeltaDeleteNodes().size();
        const size_t delEdges = view.getDeltaDeleteEdges().size();
        const size_t totalNodes = view.getNodes().size();
        const size_t totalEdges = view.getEdges().size();
        if (opt.isVerboseEnabled()) {
            std::cout << "[inc-iter " << iteration << "] mode=" << modeLabel
                      << " pruned_delta: insNodes=" << insNodes
                      << " insEdges=" << insEdges
                      << " delNodes=" << delNodes
                      << " delEdges=" << delEdges
                      << " totalNodes=" << totalNodes
                      << " totalEdges=" << totalEdges
                      << std::endl;
            std::cout << "[inc-iter " << iteration << "] mode=" << modeLabel
                      << " pruned_delta_ratio: insNodes=" << formatRatio(insNodes, totalNodes)
                      << " insEdges=" << formatRatio(insEdges, totalEdges)
                      << " delNodes=" << formatRatio(delNodes, totalNodes)
                      << " delEdges=" << formatRatio(delEdges, totalEdges)
                      << std::endl;
        }
    }

    static std::unordered_map<UntypedTuple, NodePtr> buildNodeTupleIndex(
            const std::unordered_set<NodePtr>& nodes) {
        std::unordered_map<UntypedTuple, NodePtr> byTuple;
        byTuple.reserve(nodes.size());
        for (const auto& node : nodes) {
            if (!node) {
                continue;
            }
            byTuple.emplace(node->getTuple(), node);
        }
        return byTuple;
    }

    static std::map<EdgeKey, EdgePtr> buildEdgeKeyIndex(const std::unordered_set<EdgePtr>& edges) {
        std::map<EdgeKey, EdgePtr> byKey;
        for (const auto& edge : edges) {
            if (!edge) {
                continue;
            }
            byKey.emplace(edge->getEdgeKey(), edge);
        }
        return byKey;
    }

    IncSubgraphView buildPostPruneDiffView(
            const IncSubgraphView& oldView,
            const IncSubgraphView& newView,
            std::unordered_map<NodePtr, NodePtr>& oldToNewNodes,
            std::unordered_map<EdgePtr, EdgePtr>& oldToNewEdges) const {
        oldToNewNodes.clear();
        oldToNewEdges.clear();

        const auto oldNodeByTuple = buildNodeTupleIndex(oldView.getNodes());
        const auto newNodeByTuple = buildNodeTupleIndex(newView.getNodes());
        const auto oldEdgeByKey = buildEdgeKeyIndex(oldView.getEdges());
        const auto newEdgeByKey = buildEdgeKeyIndex(newView.getEdges());

        std::set<NodePtr> deltaInsertNodes;
        std::set<EdgePtr> deltaInsertEdges;
        std::set<NodePtr> deltaDeleteNodes;
        std::set<EdgePtr> deltaDeleteEdges;
        std::set<NodePtr> deltaInsertFactNodes;
        std::set<NodePtr> explicitDeletedFacts;
        std::vector<NodePtr> deletedOutputNodes;

        oldToNewNodes.reserve(oldNodeByTuple.size());
        oldToNewEdges.reserve(oldEdgeByKey.size());
        deletedOutputNodes.reserve(oldNodeByTuple.size());

        for (const auto& [tuple, oldNode] : oldNodeByTuple) {
            auto itNew = newNodeByTuple.find(tuple);
            if (itNew == newNodeByTuple.end()) {
                deltaDeleteNodes.insert(oldNode);
                if (oldNode->isFact) {
                    explicitDeletedFacts.insert(oldNode);
                }
                if (oldNode->needOutput || oldNode->isQueryNode()) {
                    deletedOutputNodes.push_back(oldNode);
                }
                continue;
            }
            oldToNewNodes.emplace(oldNode, itNew->second);
        }

        for (const auto& [tuple, newNode] : newNodeByTuple) {
            if (!oldNodeByTuple.count(tuple)) {
                deltaInsertNodes.insert(newNode);
                if (newNode->isFact) {
                    deltaInsertFactNodes.insert(newNode);
                }
            }
        }

        for (const auto& [key, oldEdge] : oldEdgeByKey) {
            auto itNew = newEdgeByKey.find(key);
            if (itNew == newEdgeByKey.end()) {
                deltaDeleteEdges.insert(oldEdge);
                continue;
            }
            oldToNewEdges.emplace(oldEdge, itNew->second);
        }

        for (const auto& [key, newEdge] : newEdgeByKey) {
            if (!oldEdgeByKey.count(key)) {
                deltaInsertEdges.insert(newEdge);
            }
        }

        std::unordered_set<NodePtr> liveNodes = newView.getNodes();
        std::unordered_set<EdgePtr> liveEdges = newView.getEdges();
        std::vector<NodePtr> outputNodes = newView.getOutputNodes();
        std::vector<NodePtr> evidenceNodes = newView.getEvidenceNodes();

        if (opt.isVerboseEnabled()) {
            std::cout << "[inc-full-diff] pruned-diff"
                      << " insNodes=" << deltaInsertNodes.size()
                      << " insEdges=" << deltaInsertEdges.size()
                      << " delNodes=" << deltaDeleteNodes.size()
                      << " delEdges=" << deltaDeleteEdges.size()
                      << " oldNodes=" << oldNodeByTuple.size()
                      << " oldEdges=" << oldEdgeByKey.size()
                      << " newNodes=" << newNodeByTuple.size()
                      << " newEdges=" << newEdgeByKey.size()
                      << std::endl;
        }

        return IncSubgraphView(std::move(liveNodes), std::move(liveEdges), std::move(deltaInsertNodes),
                std::move(deltaInsertEdges), std::move(deltaDeleteNodes), std::move(deltaDeleteEdges),
                std::move(deltaInsertFactNodes), {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
                std::move(explicitDeletedFacts), std::move(deletedOutputNodes), std::move(outputNodes),
                std::move(evidenceNodes));
    }

    void remapStateForPostPruneDiff(const std::unordered_map<NodePtr, NodePtr>& oldToNewNodes,
            const std::unordered_map<EdgePtr, EdgePtr>& oldToNewEdges) {
        std::map<NodePtr, NodeRef> remappedNodeFormulas;
        std::map<EdgePtr, NodeRef> remappedEdgeFormulas;
        std::unordered_map<NodePtr, double> remappedProbResult;
        std::unordered_map<NodePtr, double> remappedPrecomputed;
        remappedProbResult.reserve(oldToNewNodes.size());
        remappedPrecomputed.reserve(oldToNewNodes.size());
        size_t reusedNodeFormulas = 0;
        size_t reusedEdgeFormulas = 0;
        size_t reusedProbNodes = 0;
        size_t reusedPrecomputedNodes = 0;
        size_t reboundNodeIndices = 0;
        size_t reboundEdgeIndices = 0;

        for (const auto& [oldNode, newNode] : oldToNewNodes) {
            auto nodeIt = nodeFormulas->find(oldNode);
            if (nodeIt != nodeFormulas->end()) {
                remappedNodeFormulas[newNode] = nodeIt->second;
                reusedNodeFormulas++;
            }
            auto probIt = probResult.find(oldNode);
            if (probIt != probResult.end()) {
                remappedProbResult[newNode] = probIt->second;
                reusedProbNodes++;
            }
            auto preIt = precomputedProbResult.find(oldNode);
            if (preIt != precomputedProbResult.end()) {
                remappedPrecomputed[newNode] = preIt->second;
                reusedPrecomputedNodes++;
            }
            if (ddManager != nullptr) {
                int idx = -1;
                if (ddManager->peekVarIndex(*oldNode, idx)) {
                    ddManager->bindVarIndex(*newNode, idx);
                    reboundNodeIndices++;
                }
            }
        }

        for (const auto& [oldEdge, newEdge] : oldToNewEdges) {
            auto edgeIt = edgeFormulas->find(oldEdge);
            if (edgeIt != edgeFormulas->end()) {
                remappedEdgeFormulas[newEdge] = edgeIt->second;
                reusedEdgeFormulas++;
            }
            if (ddManager != nullptr) {
                int idx = -1;
                if (ddManager->peekVarIndex(*oldEdge, idx)) {
                    ddManager->bindVarIndex(*newEdge, idx);
                    reboundEdgeIndices++;
                }
            }
        }

        nodeFormulas->swap(remappedNodeFormulas);
        edgeFormulas->swap(remappedEdgeFormulas);
        probResult.swap(remappedProbResult);
        precomputedProbResult.swap(remappedPrecomputed);

        if (opt.isVerboseEnabled()) {
            std::cout << "[inc-full-diff] remap"
                      << " nodeFormulas=" << reusedNodeFormulas
                      << " edgeFormulas=" << reusedEdgeFormulas
                      << " probNodes=" << reusedProbNodes
                      << " precomputedNodes=" << reusedPrecomputedNodes
                      << " nodeVarRebind=" << reboundNodeIndices
                      << " edgeVarRebind=" << reboundEdgeIndices
                      << std::endl;
        }
    }

    static std::vector<std::string> collectModeSpecs(const std::vector<std::string>& tokens) {
        std::vector<std::string> specs;
        for (const auto& token : tokens) {
            souffle::appendSplitModeSpecs(specs, token);
        }
        return specs;
    }

    void handleSetModeCommand(const ParsedCommand& command) {
        const std::vector<std::string> specs = collectModeSpecs(command.args);
        if (specs.empty()) {
            std::cout << "Usage: " << souffle::incrementalSetModeUsageText() << std::endl;
            std::cout << "Current mode: " << modeSummaryLabel() << std::endl;
            return;
        }
        IncrementalModeSpec parsed = currentModeSpec();
        std::string error;
        if (souffle::parseIncrementalModeSpecs(specs, currentModeSpec(), parsed, &error)) {
            setModeSpec(parsed);
            std::cout << "Set mode to " << modeSummaryLabel() << std::endl;
            return;
        }

        std::cout << error << std::endl;
        std::cout << "Available modes: " << souffle::incrementalModeHelpText() << std::endl;
        std::cout << "Current mode unchanged: " << modeSummaryLabel() << std::endl;
    }

    souffle::SouffleProgram* program;
    IncrementalDerivationGraph* graph;
    std::unique_ptr<IncrementalDerivationGraph>* graphOwner = nullptr;
    RuleManager* ruleManager;
    QueryManager* queryManager;
    DDManager<NodeRef>* ddManager = nullptr;
    std::map<NodePtr, NodeRef>* nodeFormulas;
    std::map<EdgePtr, NodeRef>* edgeFormulas;
    std::set<NodePtr> changedNodes;

    IncrementalModeSpec modeSpec{};
    FcStateClass fcStateClass = FcStateClass::NORMALIZED;
    IncrementalModeSpec lastRequestedTurnMode{};
    IncrementalModeSpec lastEffectiveTurnMode{};
    bool lastTurnFallbackToNormalizer = false;
    bool lastTurnTrueRegionalReuse = false;
    bool lastTurnUsedCalibrationOverrides = false;
    size_t lastTurnBoundaryTotal = 0;
    size_t lastTurnRegionNodes = 0;
    size_t lastTurnDeltaReachNodes = 0;
    size_t consecutiveRegionalTurns = 0;
    void syncRuntimeTogglesFromOptions() {
        DerivationGraphViewInterface::setDumpDotEnabled(opt.isDumpDotEnabled());
        DerivationGraphViewInterface::setDumpJsonEnabled(opt.isDumpJsonEnabled());
        DerivationGraphViewInterface::setDumpStatsEnabled(opt.isDumpStatEnabled());
        DerivationManager::setSemStatsEnabled(opt.isDumpStatEnabled());
        dredProfileEnabled = opt.isDredProfileEnabled();
        incProfileEnabled = opt.isIncProfileEnabled();
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
        DerivationGraphViewInterface::setVerboseEnabled(opt.isVerboseEnabled());
        setFunctionTimerOutputEnabled(opt.isVerboseEnabled());
    }
public:
    IncrementalCLI(souffle::SouffleProgram* prog = nullptr,
            IncrementalDerivationGraph* graph = nullptr,
            std::unique_ptr<IncrementalDerivationGraph>* graphOwner = nullptr,
            RuleManager* rm = nullptr,
            QueryManager* qm = nullptr,
            DDManager<NodeRef>* ddManager = nullptr,
            std::map<NodePtr, NodeRef>* nodeFormulas = {},
            std::map<EdgePtr, NodeRef>* edgeFormulas = {}
            )
            : program(prog),
              graph(graph),
              graphOwner(graphOwner),
              ruleManager(rm),
              queryManager(qm),
              ddManager(ddManager),
              nodeFormulas(nodeFormulas),
              edgeFormulas(edgeFormulas),
              changedNodes(),
              running(true) {
        // Initialize readline
        using_history();
    }

//    IncrementalCLI(): {}

    ~IncrementalCLI() {
        // Clean up readline history
        clear_history();
    }

    souffle::CmdOptions opt;
    void setCmdOptions(const souffle::CmdOptions& options) {
        opt = options;
        syncRuntimeTogglesFromOptions();
        setModeSpec(options.getIncrementalModeSpec());
    }

    void setModeSpec(const IncrementalModeSpec& nextMode) {
        assertModeCompatibleWithGraphState(nextMode);
        modeSpec = nextMode;
    }

    void setModes(SemMode sem, FcMode fc) {
        setModeSpec(IncrementalModeSpec{sem, fc});
    }

private:
    using OperationStager = souffle::cli::PendingOperationStager<Operation>;

    void replaceGraph(std::unique_ptr<IncrementalDerivationGraph> nextGraph) {
        if (graphOwner != nullptr) {
            *graphOwner = std::move(nextGraph);
            graph = graphOwner->get();
        } else {
            graph = nextGraph.release();
        }
    }

    OperationStager makeOperationStager() {
        return OperationStager(program, initialInputRelations, fact_prob, opt.isVerboseEnabled());
    }

    std::vector<std::pair<NodePtr, bool>> resolveEvidenceNodes() const {
        if (!graph) {
            throw std::runtime_error("IncrementalCLI: graph is null for evidence resolution");
        }
        return graph->resolveEvidenceNodes();
    }

    NodeRef buildEvidenceFormula(const std::vector<std::pair<NodePtr, bool>>& resolved) const {
        NodeRef evidenceNode = ddManager->getTrue();
        for (const auto& [node, val] : resolved) {
            auto it = nodeFormulas->find(node);
            if (it == nodeFormulas->end()) {
                throw std::runtime_error("Evidence node has no formula: " + node->getTuple().toString());
            }
            NodeRef lit = it->second;
            if (!val) {
                lit = ddManager->makeNot(lit);
            }
            evidenceNode = ddManager->makeAnd(evidenceNode, lit);
        }
        return evidenceNode;
    }

    void printHelp() const {
        std::cout << "Incremental Souffle CLI Commands:\n"
                  << "--------------------------------\n"
                  << "insert [probability::]relation_name(val1, val2, ...) [probability]\n"
                  << "       Queue a tuple for insertion with optional probability (0-1)\n"
                  << "delete/remove relation_name(val1, val2, ...)\n"
                  << "       Queue a tuple for deletion\n"
                  << "list   List all pending operations\n"
                  << "show config\n"
                  << "       Show current online mode and mutable runtime toggles\n"
                  << souffle::incrementalSetModeUsageText() << "\n"
                  << "set dump <json|json-before-prune|dot|stat>\n"
                  << "unset dump <json|json-before-prune|dot|stat>\n"
                  << "set profile-stage <dred|inc|fc|wmc|inc-delete|inc-regional|dep-graph>\n"
                  << "unset profile-stage <...>\n"
                  << "commit Apply queued changes and run incremental computation\n"
                  << "help, h Display this help message\n"
                  << "exit, quit, q Exit the CLI\n"
                  << std::endl;
    }

    void handleInsertCommand(const ParsedCommand& command) {
        auto [relName, values, probability, success] =
                souffle::cli::parseTupleWithOptionalProbability(command.remainder);
        if (!success || relName.empty()) {
            std::cout << "Error: Invalid format. Use: [probability::]relation_name(val1, val2, ...) [probability]"
                      << std::endl;
            return;
        }

        Operation op;
        op.type = Operation::INSERT;
        op.relationName = relName;
        op.values = values;
        op.probability = probability;
        pendingOperations.push_back(op);
    }

    void handleDeleteCommand(const ParsedCommand& command) {
        auto [relName, values, probability, success] =
                souffle::cli::parseTupleWithOptionalProbability(command.remainder);
        static_cast<void>(probability);
        if (!success || relName.empty()) {
            std::cout << "Error: Invalid format. Use: relation_name(val1, val2, ...)" << std::endl;
            return;
        }

        Operation op;
        op.type = Operation::DELETE;
        op.relationName = relName;
        op.values = values;
        op.probability = 1.0;

        bool overlap = false;
        for (size_t i = 0; i < pendingOperations.size(); i++) {
            if (pendingOperations[i].type == Operation::INSERT &&
                    pendingOperations[i].relationName == relName) {
                if (pendingOperations[i].values == values) {
                    if (opt.isVerboseEnabled()) {
                        std::cout << "Overlapped insertion and deletion removed." << std::endl;
                    }
                    overlap = true;
                    pendingOperations.erase(pendingOperations.begin() + i);
                    break;
                }
            }
        }
        if (!overlap) {
            pendingOperations.push_back(op);
        }
    }

    void handleListCommand() const {
        if (pendingOperations.empty()) {
            std::cout << "No pending operations." << std::endl;
            return;
        }

        std::cout << "Pending operations:" << std::endl;
        for (size_t i = 0; i < pendingOperations.size(); i++) {
            const Operation& op = pendingOperations[i];
            std::cout << i + 1 << ". " << op.toString() << std::endl;
        }
    }

    static std::string joinTokens(const std::vector<std::string>& values) {
        if (values.empty()) {
            return "(none)";
        }
        std::ostringstream oss;
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                oss << ",";
            }
            oss << values[i];
        }
        return oss.str();
    }

    void handleShowCommand(const ParsedCommand& command) const {
        const std::string topic = command.args.empty() ? std::string() : command.args.front();
        if (topic.empty() || topic == "config") {
            std::cout << "mode=" << souffle::incrementalModeTokenLabel(modeSpec)
                      << " dumps=" << joinTokens(opt.getEnabledDumpKinds())
                      << " profile-stages=" << joinTokens(opt.getEnabledProfileStages())
                      << " fc-state=" << souffle::fcStateClassLabel(fcStateClass)
                      << " regional-turns=" << consecutiveRegionalTurns
                      << std::endl;
            return;
        }
        std::cout << "Unknown show topic: " << topic << std::endl;
    }

    bool setMutableConfigOption(
            const std::string& key, const std::vector<std::string>& args, bool enabled) {
        if (key == "dump") {
            const std::string value = args.size() > 1 ? args[1] : std::string();
            const std::string normalized = souffle::normalizeFlagToken(value);
            if (normalized == "json-before-graph") {
                std::cout << "dump json-before-graph is only evaluated during startup graph construction"
                          << std::endl;
                return true;
            }
            if (value.empty() || !opt.setDumpKindToken(value, enabled)) {
                std::cout << "Usage: " << (enabled ? "set" : "unset") << " dump "
                          << "[ json | json-before-prune | dot | stat ]" << std::endl;
                return true;
            }
            syncRuntimeTogglesFromOptions();
            std::cout << (enabled ? "Enabled" : "Disabled") << " dump " << souffle::normalizeFlagToken(value)
                      << std::endl;
            return true;
        }
        if (key == "profile-stage") {
            const std::string value = args.size() > 1 ? args[1] : std::string();
            if (value.empty() || !opt.setProfileStageToken(value, enabled)) {
                std::cout << "Usage: " << (enabled ? "set" : "unset") << " profile-stage "
                          << souffle::profileStageOptionSyntax() << std::endl;
                return true;
            }
            syncRuntimeTogglesFromOptions();
            std::cout << (enabled ? "Enabled" : "Disabled") << " profile-stage "
                      << souffle::normalizeFlagToken(value) << std::endl;
            return true;
        }
        return false;
    }

    void handleSetCommand(const ParsedCommand& command) {
        const std::string key = command.args.empty() ? std::string() : command.args.front();
        if (key.empty()) {
            std::cout << "Usage: set <option> <value>" << std::endl;
            return;
        }
        if (!setMutableConfigOption(key, command.args, true)) {
            std::cout << "Unknown option: " << key << std::endl;
        }
    }

    void handleUnsetCommand(const ParsedCommand& command) {
        const std::string key = command.args.empty() ? std::string() : command.args.front();
        if (key.empty()) {
            std::cout << "Usage: unset <option> <value>" << std::endl;
            return;
        }
        if (!setMutableConfigOption(key, command.args, false)) {
            std::cout << "Unknown option: " << key << std::endl;
        }
    }

    bool dispatchCommand(const ParsedCommand& command);

public:
    bool processCommand(const std::string& command) {
        const ParsedCommand parsed = souffle::cli::parseCommandLine(command);
        if (parsed.verb.empty()) {
            return true;
        }
        return dispatchCommand(parsed);
    }

    UntypedTuple getTuple(const Operation& op) {
        UntypedTuple tuple{op.relationName, {}};
        for (const auto& value : op.values) {
            tuple.fields.push_back(std::stoi(value));
        }
        return tuple;
    }

    std::unordered_map<UntypedTuple, double> getFactProbInc() {
        std::unordered_map<UntypedTuple, double> fact_prob_inc;
        for (const auto& op : pendingOperations) {
            if (op.valid && op.type == Operation::INSERT) {
                fact_prob_inc[getTuple(op)] = op.probability;
            }
        }
        if (detOptEnabled) {
            const std::string prefix = "$inc_delta_tuple_insert_";
            for (auto* rel : program->getAllRelations()) {
                const std::string& relName = rel->getName();
                if (relName.rfind(prefix, 0) != 0) {
                    continue;
                }
                const std::string baseName = relName.substr(prefix.size());
                if (!isDetRelation(baseName)) {
                    continue;
                }
                const auto arity = rel->getArity();
                for (auto& tuple : *rel) {
                    UntypedTuple detTuple{baseName, {}};
                    detTuple.fields.reserve(arity);
                    for (size_t i = 0; i < arity; ++i) {
                        detTuple.fields.push_back(tuple[i]);
                    }
                    if (fact_prob_inc.find(detTuple) == fact_prob_inc.end()) {
                        fact_prob_inc.emplace(std::move(detTuple), 1.0);
                    }
                }
            }
        }
        return fact_prob_inc;
    }

    std::vector<UntypedTuple> getDeletedFacts(
            const std::unordered_map<UntypedTuple, double>* insertedFacts = nullptr) {
        std::unordered_set<UntypedTuple> deletedFacts;
        deletedFacts.reserve(pendingOperations.size());
        for (const auto& op : pendingOperations) {
            if (op.valid && op.type == Operation::DELETE) {
                deletedFacts.insert(getTuple(op));
            }
        }
        if (detOptEnabled) {
            const std::string prefix = "$inc_delta_tuple_delete_";
            for (auto* rel : program->getAllRelations()) {
                const std::string& relName = rel->getName();
                if (relName.rfind(prefix, 0) != 0) {
                    continue;
                }
                const std::string baseName = relName.substr(prefix.size());
                if (!isDetRelation(baseName)) {
                    continue;
                }
                const auto arity = rel->getArity();
                for (auto& tuple : *rel) {
                    UntypedTuple detTuple{baseName, {}};
                    detTuple.fields.reserve(arity);
                    for (size_t i = 0; i < arity; ++i) {
                        detTuple.fields.push_back(tuple[i]);
                    }
                    deletedFacts.insert(std::move(detTuple));
                }
            }
            const auto& detDeletes = DerivationManager::getDetDeltaDeleteTuples();
            const auto& detInserts = DerivationManager::getDetDeltaInsertTuples();
            for (const auto& tuple : detDeletes) {
                if (detInserts.count(tuple)) {
                    continue;
                }
                if (graph && graph->findNode(tuple) == nullptr) {
                    continue;
                }
                deletedFacts.insert(tuple);
            }
        }
        if (insertedFacts != nullptr && !insertedFacts->empty()) {
            for (const auto& [tuple, _] : *insertedFacts) {
                deletedFacts.erase(tuple);
            }
        }
        std::vector<UntypedTuple> out;
        out.reserve(deletedFacts.size());
        for (const auto& tuple : deletedFacts) {
            out.push_back(tuple);
        }
        return out;
    }

    void stagePendingOperationsForProgram() {
        auto stager = makeOperationStager();
        stager.stageAll(pendingOperations);
    }

    void purgeAllIncDeltaRelations() {
        auto stager = makeOperationStager();
        stager.purgeAllIncDeltaRelations();
    }

    void purgeAllNonIncDeltaRelations() {
        auto stager = makeOperationStager();
        stager.purgeAllNonIncDeltaRelations();
    }

    void purgeAllRelations() {
        auto stager = makeOperationStager();
        stager.purgeAllRelations();
    }

    void loadInitialInputRelations() {
        auto stager = makeOperationStager();
        stager.loadInitialInputRelations();
    }


    void runIncrementalWmc(IncSubgraphView& view, bool useRegional) {
        struct WeightRestore {
            DDManager<NodeRef>* mgr = nullptr;
            const std::unordered_map<int, std::pair<double, double>>* base = nullptr;
            struct SavedWeight {
                int varIdx = -1;
                double pos = 0.0;
                double neg = 0.0;
            };
            std::vector<SavedWeight> saved;
            WeightRestore(DDManager<NodeRef>* manager,
                          const std::unordered_map<int, std::pair<double, double>>* baseWeights)
                    : mgr(manager), base(baseWeights) {}
            void apply(const std::vector<int>& vars) {
                if (!mgr || !base) {
                    return;
                }
                for (int varIdx : vars) {
                    auto it = base->find(varIdx);
                    if (it == base->end()) {
                        continue;
                    }
                    auto cur = mgr->getVariableWeight(varIdx);
                    if (cur.posWeight == it->second.first && cur.negWeight == it->second.second) {
                        continue;
                    }
                    saved.push_back(SavedWeight{varIdx, cur.posWeight, cur.negWeight});
                    mgr->setVariableWeight(varIdx, it->second.first, it->second.second);
                }
            }
            ~WeightRestore() {
                if (!mgr) {
                    return;
                }
                for (const auto& entry : saved) {
                    mgr->setVariableWeight(entry.varIdx, entry.pos, entry.neg);
                }
            }
        };
        auto upstreamKey = [](const std::vector<int>& vars) -> std::string {
            if (vars.empty()) {
                return {};
            }
            std::ostringstream oss;
            for (size_t i = 0; i < vars.size(); ++i) {
                if (i) {
                    oss << ",";
                }
                oss << vars[i];
            }
            return oss.str();
        };
        std::unordered_map<size_t, std::unordered_map<std::string, double>> evidenceWeightOverrideCache;
        evidenceWeightOverrideCache.clear();
        debugger.startStage(StageKind::WEIGHTED_MODEL_COUNTING_INC);
        FunctionTimer timer("incrementally compute probabilities, size " + std::to_string(changedNodes.size()));
        const bool incProfile = incProfileEnabled;
        const bool wmcProfile = wmcProfileEnabled;
        using Clock = std::chrono::steady_clock;
        auto toMs = [](Clock::time_point start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        };
        auto stageStart = Clock::now();
        double evidenceBuildMs = 0.0;
        double evidenceWmcMs = 0.0;
        double nodeLoopMs = 0.0;
        double evidenceMakeAndMs = 0.0;
        double evidenceWmcComputeMs = 0.0;
        double nodeMakeAndMs = 0.0;
        double nodeWmcComputeMs = 0.0;
        double weightApplyMs = 0.0;
        std::size_t evidenceWmcCalls = 0;
        std::size_t nodeWmcCalls = 0;
        std::size_t evidenceMakeAndCalls = 0;
        std::size_t nodeMakeAndCalls = 0;
        std::size_t weightApplyCalls = 0;
        std::size_t weightApplyVars = 0;
        std::size_t weightToggleCalls = 0;
        std::size_t nodeReuse = 0;
        std::size_t nodeRecompute = 0;
        std::size_t nodeZero = 0;
        const auto& outputNodes = view.getOutputNodes();
        const auto& evidenceNodes = view.getEvidenceNodes();
        const auto& deletedOutputNodes = view.getDeletedOutputNodes();
        CycleDependencyGraph* depGraphPtr = nullptr;
        double depGraphMs = 0.0;
        double outputClassifyMs = 0.0;
        double regionLookupMs = 0.0;
        double changedLookupMs = 0.0;
        double componentIdMs = 0.0;
        double probLookupMs = 0.0;
        double probWriteMs = 0.0;
        double outputLoopMs = 0.0;
        if (!evidenceNodes.empty()) {
            auto depStart = Clock::now();
            depGraphPtr = &view.getCycleDependencyGraph();
            if (wmcProfile) {
                depGraphMs = toMs(depStart);
            }
        }
        size_t componentCount = depGraphPtr ? depGraphPtr->getComponentCount() : 0;
        std::vector<NodeRef> componentEvidence(componentCount, ddManager->getTrue());
        std::vector<double> componentEvidenceWeight(componentCount, 1.0);
        std::vector<double> componentEvidenceWeightOriginal;
        std::vector<bool> componentHasEvidence(componentCount, false);
        std::vector<bool> componentEvidenceChanged(componentCount, false);
        const bool regionalOutputProfile = (useRegional && incRegionalOutputProfile.active);
        const bool hasOverrideWeights =
                regionalOutputProfile && !incRegionalOutputProfile.overrideWeights.empty();
        if (useRegional && incRegionalProfileEnabled) {
            debugger.addInfo("inc_regional_profile_active", regionalOutputProfile ? "1" : "0");
        }
        auto applyWeights = [&](const std::unordered_map<int, std::pair<double, double>>& weights) {
            if (!wmcProfile) {
                for (const auto& [varIdx, w] : weights) {
                    ddManager->setVariableWeight(varIdx, w.first, w.second);
                }
                return;
            }
            auto applyStart = Clock::now();
            for (const auto& [varIdx, w] : weights) {
                ddManager->setVariableWeight(varIdx, w.first, w.second);
            }
            weightApplyMs += toMs(applyStart);
            weightApplyCalls++;
            weightApplyVars += weights.size();
        };
        auto makeAndProfile = [&](const NodeRef& lhs, const NodeRef& rhs,
                                  double& ms, std::size_t& calls) {
            if (!wmcProfile) {
                return ddManager->makeAnd(lhs, rhs);
            }
            auto andStart = Clock::now();
            auto res = ddManager->makeAnd(lhs, rhs);
            ms += toMs(andStart);
            calls++;
            return res;
        };
        auto computeWmcProfile = [&](const NodeRef& node, double& ms, std::size_t& calls) {
            calls++;
            if (!wmcProfile) {
                return ddManager->computeWeightedModelCount(node);
            }
            auto wmcStart = Clock::now();
            double res = ddManager->computeWeightedModelCount(node);
            ms += toMs(wmcStart);
            return res;
        };

        if (depGraphPtr) {
            auto evidenceBuildStart = Clock::now();
            for (size_t cid = 0; cid < componentCount; ++cid) {
                const auto& evidences = depGraphPtr->getComponentEvidences(cid);
                if (evidences.empty()) {
                    continue;
                }
                componentHasEvidence[cid] = true;
                NodeRef evidenceNode = ddManager->getTrue();
                for (const auto& [node, val] : evidences) {
                    if (changedNodes.find(node) != changedNodes.end() ||
                            (regionalOutputProfile &&
                             incRegionalOutputProfile.deltaReachableNodes.count(node) > 0)) {
                        componentEvidenceChanged[cid] = true;
                    }
                    auto it = nodeFormulas->find(node);
                    if (it == nodeFormulas->end()) {
                        throw std::runtime_error("Evidence node has no formula: " + node->getTuple().toString());
                    }
                    NodeRef lit = it->second;
                    if (!val) {
                        lit = ddManager->makeNot(lit);
                    }
                    evidenceNode = makeAndProfile(evidenceNode, lit, evidenceMakeAndMs, evidenceMakeAndCalls);
                }
                componentEvidence[cid] = evidenceNode;
            }
            if (incProfile) {
                evidenceBuildMs = toMs(evidenceBuildStart);
            }

            auto evidenceWmcStart = Clock::now();
            if (hasOverrideWeights) {
                for (size_t cid = 0; cid < componentCount; ++cid) {
                    if (componentHasEvidence[cid]) {
                        componentEvidenceWeight[cid] = computeWmcProfile(
                                componentEvidence[cid], evidenceWmcComputeMs, evidenceWmcCalls);
                    }
                }
                if (wmcProfile) {
                    weightToggleCalls++;
                }
                applyWeights(incRegionalOutputProfile.originalWeights);
                componentEvidenceWeightOriginal.assign(componentCount, 1.0);
                for (size_t cid = 0; cid < componentCount; ++cid) {
                    if (componentHasEvidence[cid]) {
                        componentEvidenceWeightOriginal[cid] = computeWmcProfile(
                                componentEvidence[cid], evidenceWmcComputeMs, evidenceWmcCalls);
                    }
                }
                if (wmcProfile) {
                    weightToggleCalls++;
                }
                applyWeights(incRegionalOutputProfile.overrideWeights);
            } else {
                for (size_t cid = 0; cid < componentCount; ++cid) {
                    if (componentHasEvidence[cid]) {
                        componentEvidenceWeight[cid] = computeWmcProfile(
                                componentEvidence[cid], evidenceWmcComputeMs, evidenceWmcCalls);
                    }
                }
            }
            if (incProfile) {
                evidenceWmcMs = toMs(evidenceWmcStart);
            }
        }

        auto nodeLoopStart = Clock::now();
        auto logOutputDecision = [&](const NodePtr& node, const char* action, const char* reason,
                                     bool useOriginalWeights) {
            if (!regionalOutputProfile || !incRegionalProfileEnabled || !node) {
                return;
            }
            const bool inRegion = incRegionalOutputProfile.regionNodes.count(node) > 0;
            const bool inBoundary = incRegionalOutputProfile.boundaryNodes.count(node) > 0;
            const bool inDeltaReach = incRegionalOutputProfile.deltaReachableNodes.count(node) > 0;
            const bool changed = changedNodes.find(node) != changedNodes.end();
            const char* mode = inRegion ? "region_new_bdd" : "outside_old_bdd";
            const char* weightMode = useOriginalWeights ? "orig" : "calib";
            debugger.logMessage(
                Level::INFO,
                std::string("[inc-regional-output] node=") + node->getTuple().toString() +
                    " in_region=" + (inRegion ? "1" : "0") +
                    " in_boundary=" + (inBoundary ? "1" : "0") +
                    " in_delta_reach=" + (inDeltaReach ? "1" : "0") +
                    " changed=" + (changed ? "1" : "0") +
                    " action=" + action +
                    " reason=" + reason +
                    " mode=" + mode +
                    " weight_mode=" + weightMode +
                    " override_count=" + std::to_string(incRegionalOutputProfile.overrideCount));
        };
        auto logUpstreamRestore = [&](const NodePtr& node, const std::vector<int>& vars,
                                      const char* reason) {
            if (!regionalOutputProfile || !incRegionalProfileEnabled ||
                    !node || vars.empty()) {
                return;
            }
            debugger.logMessage(
                Level::INFO,
                std::string("[inc-regional-upstream-restore] node=") +
                    node->getTuple().toString() +
                    " vars=" + std::to_string(vars.size()) +
                    " reason=" + reason);
        };
        auto applyBoundaryTarget = [&](const NodePtr& node, bool useOriginalWeights,
                                       bool hasEvidence) -> bool {
            if (!regionalOutputProfile || !useOriginalWeights || hasEvidence || !node) {
                return false;
            }
            auto it = incRegionalOutputProfile.boundaryOutputTargets.find(node);
            if (it == incRegionalOutputProfile.boundaryOutputTargets.end()) {
                return false;
            }
            auto writeStart = Clock::now();
            probResult[node] = it->second;
            if (wmcProfile) {
                probWriteMs += toMs(writeStart);
            }
            nodeReuse++;
            logOutputDecision(node, "reuse_precomputed", "boundary_target", useOriginalWeights);
            return true;
        };
        bool weightsCalibrated = true;
        auto ensureWeights = [&](bool wantCalibrated) {
            if (!hasOverrideWeights) {
                return;
            }
            if (wantCalibrated == weightsCalibrated) {
                return;
            }
            if (wmcProfile) {
                weightToggleCalls++;
            }
            applyWeights(wantCalibrated ? incRegionalOutputProfile.overrideWeights
                                        : incRegionalOutputProfile.originalWeights);
            weightsCalibrated = wantCalibrated;
        };
        const bool hasUpstreamRestore =
                hasOverrideWeights && regionalOutputProfile &&
                !incRegionalOutputProfile.boundaryUpstreamAnchorVars.empty();
        std::unordered_map<NodePtr, std::vector<int>> upstreamCache;
        auto inDrNode = [&](const NodePtr& n) {
            return incRegionalOutputProfile.deltaReachableNodes.empty() ||
                   incRegionalOutputProfile.deltaReachableNodes.count(n);
        };
        auto inDrEdge = [&](const EdgePtr& e) {
            return incRegionalOutputProfile.deltaReachableEdges.empty() ||
                   incRegionalOutputProfile.deltaReachableEdges.count(e);
        };
        auto collectUpstreamVars = [&](const NodePtr& node) -> const std::vector<int>& {
            static const std::vector<int> empty;
            if (!hasUpstreamRestore || !node) {
                return empty;
            }
            auto it = upstreamCache.find(node);
            if (it != upstreamCache.end()) {
                return it->second;
            }
            if (!inDrNode(node)) {
                upstreamCache.emplace(node, std::vector<int>{});
                return upstreamCache[node];
            }
            std::unordered_set<NodePtr> visited;
            std::unordered_set<NodePtr> boundaryHits;
            std::queue<NodePtr> q;
            visited.insert(node);
            q.push(node);
            while (!q.empty()) {
                NodePtr cur = q.front();
                q.pop();
                if (incRegionalOutputProfile.boundaryNodes.count(cur)) {
                    boundaryHits.insert(cur);
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
            std::unordered_set<int> vars;
            for (const auto& b : boundaryHits) {
                auto itB = incRegionalOutputProfile.boundaryUpstreamAnchorVars.find(b);
                if (itB == incRegionalOutputProfile.boundaryUpstreamAnchorVars.end()) {
                    continue;
                }
                for (int varIdx : itB->second) {
                    vars.insert(varIdx);
                }
            }
            std::vector<int> varList(vars.begin(), vars.end());
            std::sort(varList.begin(), varList.end());
            upstreamCache.emplace(node, std::move(varList));
            return upstreamCache[node];
        };
        if (!deletedOutputNodes.empty()) {
            for (const auto& node : deletedOutputNodes) {
                probResult.erase(node);
            }
        }
        struct OutputInfo {
            NodePtr node;
            bool inRegion;
            bool inDeltaReach;
        };
        std::vector<OutputInfo> orderedOutputs;
        const std::vector<OutputInfo>* outputInfoPtr = nullptr;
        if (hasOverrideWeights && regionalOutputProfile) {
            orderedOutputs.reserve(outputNodes.size());
            std::vector<OutputInfo> nonRegionOutputs;
            nonRegionOutputs.reserve(outputNodes.size());
            for (const auto& node : outputNodes) {
                auto regionStart = Clock::now();
                bool inDeltaReach = incRegionalOutputProfile.deltaReachableNodes.count(node) > 0;
                bool inRegion = incRegionalOutputProfile.regionNodes.count(node) > 0;
                if (wmcProfile) {
                    regionLookupMs += toMs(regionStart);
                }
                OutputInfo info{node, inRegion, inDeltaReach};
                if (inRegion) {
                    orderedOutputs.push_back(info);
                } else {
                    nonRegionOutputs.push_back(info);
                }
            }
            orderedOutputs.insert(orderedOutputs.end(),
                                  nonRegionOutputs.begin(),
                                  nonRegionOutputs.end());
            outputInfoPtr = &orderedOutputs;
        }
        auto forEachOutput = [&](auto&& fn) {
            if (outputInfoPtr) {
                for (const auto& info : *outputInfoPtr) {
                    fn(info.node, info.inRegion, info.inDeltaReach);
                }
                return;
            }
            for (const auto& node : outputNodes) {
                bool inRegion = false;
                bool inDeltaReach = false;
                if (regionalOutputProfile) {
                    auto regionStart = Clock::now();
                    inDeltaReach = incRegionalOutputProfile.deltaReachableNodes.count(node) > 0;
                    inRegion = incRegionalOutputProfile.regionNodes.count(node) > 0;
                    if (wmcProfile) {
                        regionLookupMs += toMs(regionStart);
                    }
                }
                fn(node, inRegion, inDeltaReach);
            }
        };
        if (!depGraphPtr) {
            forEachOutput([&](const NodePtr& node, bool inRegion, bool inDeltaReach) {
                if (!node->needOutput) {
                    return;
                }
                auto classifyStart = Clock::now();
                const bool regionalTouched = regionalOutputProfile && (inRegion || inDeltaReach);
                const bool useOriginalWeights = hasOverrideWeights && inRegion;
                if (wmcProfile) {
                    outputClassifyMs += toMs(classifyStart);
                }

                bool isChanged = false;
                auto changedStart = Clock::now();
                if (changedNodes.find(node) != changedNodes.end()) {
                    isChanged = true;
                }
                if (wmcProfile) {
                    changedLookupMs += toMs(changedStart);
                }
                if (regionalTouched || isChanged) {
                    ensureWeights(!useOriginalWeights);
                    WeightRestore restore(ddManager, &incRegionalOutputProfile.originalWeights);
                    const auto& upstreamVars = collectUpstreamVars(node);
                    if (!useOriginalWeights && !upstreamVars.empty()) {
                        logUpstreamRestore(node, upstreamVars, "node_wmc");
                        restore.apply(upstreamVars);
                    }
                    auto value = computeWmcProfile((*nodeFormulas)[node],
                                                   nodeWmcComputeMs, nodeWmcCalls);
                    auto writeStart = Clock::now();
                    probResult[node] = value;
                    if (wmcProfile) {
                        probWriteMs += toMs(writeStart);
                    }
                    nodeRecompute++;
                    logOutputDecision(node, "recompute_wmc",
                                      regionalTouched ? "regional_no_evidence" : "no_evidence_changed",
                                      useOriginalWeights);
                } else {
                    auto lookupStart = Clock::now();
                    auto it = probResult.find(node);
                    if (wmcProfile) {
                        probLookupMs += toMs(lookupStart);
                    }
                    if (it != probResult.end()) {
                        auto writeStart = Clock::now();
                        probResult[node] = it->second;
                        if (wmcProfile) {
                            probWriteMs += toMs(writeStart);
                        }
                        nodeReuse++;
                        logOutputDecision(node, "reuse_old_prob", "no_evidence_no_change",
                                          useOriginalWeights);
                    } else {
                        WeightRestore restore(ddManager, &incRegionalOutputProfile.originalWeights);
                        const auto& upstreamVars = collectUpstreamVars(node);
                        if (!useOriginalWeights && !upstreamVars.empty()) {
                            logUpstreamRestore(node, upstreamVars, "node_wmc");
                            restore.apply(upstreamVars);
                        }
                        auto value = computeWmcProfile((*nodeFormulas)[node],
                                                       nodeWmcComputeMs, nodeWmcCalls);
                        auto writeStart = Clock::now();
                        probResult[node] = value;
                        if (wmcProfile) {
                            probWriteMs += toMs(writeStart);
                        }
                        nodeRecompute++;
                        logOutputDecision(node, "recompute_wmc", "no_evidence_missing_prob",
                                          useOriginalWeights);
                    }
                }
            });
        } else {
            forEachOutput([&](const NodePtr& node, bool inRegion, bool inDeltaReach) {
                if (!node->needOutput) {
                    return;
                }
                size_t cid = 0;
                auto compStart = Clock::now();
                cid = depGraphPtr->getComponentId(node);
                if (wmcProfile) {
                    componentIdMs += toMs(compStart);
                }
                auto classifyStart = Clock::now();
                const bool regionalTouched = regionalOutputProfile && (inRegion || inDeltaReach);
                const bool useOriginalWeights = hasOverrideWeights && inRegion;
                if (wmcProfile) {
                    outputClassifyMs += toMs(classifyStart);
                }
                const auto& upstreamVars = collectUpstreamVars(node);
                auto evidenceWeightFor = [&](size_t compId) {
                    if (useOriginalWeights && !componentEvidenceWeightOriginal.empty()) {
                        return componentEvidenceWeightOriginal[compId];
                    }
                    if (upstreamVars.empty()) {
                        return componentEvidenceWeight[compId];
                    }
                    auto key = upstreamKey(upstreamVars);
                    auto& cache = evidenceWeightOverrideCache[compId];
                    auto it = cache.find(key);
                    if (it != cache.end()) {
                        return it->second;
                    }
                    WeightRestore restore(ddManager, &incRegionalOutputProfile.originalWeights);
                    logUpstreamRestore(node, upstreamVars, "evidence_wmc");
                    restore.apply(upstreamVars);
                    double w = computeWmcProfile(componentEvidence[compId],
                                                 evidenceWmcComputeMs, evidenceWmcCalls);
                    cache.emplace(std::move(key), w);
                    return w;
                };

                if (!componentHasEvidence[cid]) {
                    bool isChanged = false;
                    auto changedStart = Clock::now();
                    if (changedNodes.find(node) != changedNodes.end()) {
                        isChanged = true;
                    }
                    if (wmcProfile) {
                        changedLookupMs += toMs(changedStart);
                    }
                    if (regionalTouched || isChanged) {
                        ensureWeights(!useOriginalWeights);
                        WeightRestore restore(ddManager, &incRegionalOutputProfile.originalWeights);
                        if (!useOriginalWeights && !upstreamVars.empty()) {
                            logUpstreamRestore(node, upstreamVars, "node_wmc");
                            restore.apply(upstreamVars);
                        }
                        auto value = computeWmcProfile((*nodeFormulas)[node],
                                                       nodeWmcComputeMs, nodeWmcCalls);
                        auto writeStart = Clock::now();
                        probResult[node] = value;
                        if (wmcProfile) {
                            probWriteMs += toMs(writeStart);
                        }
                        nodeRecompute++;
                        logOutputDecision(node, "recompute_wmc",
                                          regionalTouched ? "regional_no_evidence" : "no_evidence_changed",
                                          useOriginalWeights);
                    } else {
                        auto lookupStart = Clock::now();
                        auto it = probResult.find(node);
                        if (wmcProfile) {
                            probLookupMs += toMs(lookupStart);
                        }
                        if (it != probResult.end()) {
                            auto writeStart = Clock::now();
                            probResult[node] = it->second;
                            if (wmcProfile) {
                                probWriteMs += toMs(writeStart);
                            }
                            nodeReuse++;
                            logOutputDecision(node, "reuse_old_prob", "no_evidence_no_change",
                                              useOriginalWeights);
                        } else {
                            WeightRestore restore(ddManager, &incRegionalOutputProfile.originalWeights);
                            if (!useOriginalWeights && !upstreamVars.empty()) {
                                logUpstreamRestore(node, upstreamVars, "node_wmc");
                                restore.apply(upstreamVars);
                            }
                            auto value =
                                    computeWmcProfile((*nodeFormulas)[node],
                                                      nodeWmcComputeMs, nodeWmcCalls);
                            auto writeStart = Clock::now();
                            probResult[node] = value;
                            if (wmcProfile) {
                                probWriteMs += toMs(writeStart);
                            }
                            nodeRecompute++;
                            logOutputDecision(node, "recompute_wmc", "no_evidence_missing_prob",
                                              useOriginalWeights);
                        }
                    }
                    return;
                }
                if (evidenceWeightFor(cid) == 0.0) {
                    auto writeStart = Clock::now();
                    probResult[node] = 0.0;
                    if (wmcProfile) {
                        probWriteMs += toMs(writeStart);
                    }
                    nodeZero++;
                    logOutputDecision(node, "assign_zero", "evidence_weight_zero", useOriginalWeights);
                    return;
                }
                const bool evidenceChanged = componentEvidenceChanged[cid];
                bool isChanged = false;
                auto changedStart = Clock::now();
                if (changedNodes.find(node) != changedNodes.end()) {
                    isChanged = true;
                }
                if (wmcProfile) {
                    changedLookupMs += toMs(changedStart);
                }
                if (regionalTouched || isChanged || evidenceChanged) {
                    ensureWeights(!useOriginalWeights);
                    WeightRestore restore(ddManager, &incRegionalOutputProfile.originalWeights);
                    if (!useOriginalWeights && !upstreamVars.empty()) {
                        logUpstreamRestore(node, upstreamVars, "node_wmc");
                        restore.apply(upstreamVars);
                    }
                    auto joint = makeAndProfile((*nodeFormulas)[node], componentEvidence[cid],
                                                nodeMakeAndMs, nodeMakeAndCalls);
                    double jointW = computeWmcProfile(joint, nodeWmcComputeMs, nodeWmcCalls);
                    auto writeStart = Clock::now();
                    probResult[node] = jointW / evidenceWeightFor(cid);
                    if (wmcProfile) {
                        probWriteMs += toMs(writeStart);
                    }
                    nodeRecompute++;
                    logOutputDecision(node, "recompute_wmc",
                                      regionalTouched ? "regional_evidence" : "evidence_changed_or_node_changed",
                                      useOriginalWeights);
                    return;
                }
                auto lookupStart = Clock::now();
                auto it = probResult.find(node);
                if (wmcProfile) {
                    probLookupMs += toMs(lookupStart);
                }
                if (it != probResult.end()) {
                    auto writeStart = Clock::now();
                    probResult[node] = it->second;
                    if (wmcProfile) {
                        probWriteMs += toMs(writeStart);
                    }
                    nodeReuse++;
                    logOutputDecision(node, "reuse_old_prob", "evidence_unchanged_no_change",
                                      useOriginalWeights);
                } else {
                    WeightRestore restore(ddManager, &incRegionalOutputProfile.originalWeights);
                    if (!useOriginalWeights && !upstreamVars.empty()) {
                        logUpstreamRestore(node, upstreamVars, "node_wmc");
                        restore.apply(upstreamVars);
                    }
                    auto joint = makeAndProfile((*nodeFormulas)[node], componentEvidence[cid],
                                                nodeMakeAndMs, nodeMakeAndCalls);
                    double jointW = computeWmcProfile(joint, nodeWmcComputeMs, nodeWmcCalls);
                    auto writeStart = Clock::now();
                    probResult[node] = jointW / evidenceWeightFor(cid);
                    if (wmcProfile) {
                        probWriteMs += toMs(writeStart);
                    }
                    nodeRecompute++;
                    logOutputDecision(node, "recompute_wmc", "evidence_unchanged_missing_prob",
                                      useOriginalWeights);
                }
            });
        }
        if (incProfile) {
            nodeLoopMs = toMs(nodeLoopStart);
        }
        if (wmcProfile) {
            outputLoopMs = toMs(nodeLoopStart);
        }
        if (hasOverrideWeights) {
            applyWeights(incRegionalOutputProfile.originalWeights);
            incRegionalOutputProfile.reset();
        }
        if (incProfile) {
            const double totalMs = toMs(stageStart);
            std::size_t componentWithEvidence = 0;
            for (bool hasEv : componentHasEvidence) {
                if (hasEv) {
                    componentWithEvidence++;
                }
            }
            std::cout << "[inc-profile] stage=WMC_INC total_ms=" << totalMs
                      << " evidence_build_ms=" << evidenceBuildMs
                      << " evidence_wmc_ms=" << evidenceWmcMs
                      << " node_ms=" << nodeLoopMs
                      << " components=" << componentCount
                      << " components_ev=" << componentWithEvidence
                      << " nodes=" << outputNodes.size()
                      << " changed_nodes=" << changedNodes.size()
                      << " node_reuse=" << nodeReuse
                      << " node_recompute=" << nodeRecompute
                      << " node_zero=" << nodeZero
                      << " evidence_wmc_calls=" << evidenceWmcCalls
                      << " node_wmc_calls=" << nodeWmcCalls
                      << std::endl;
        }
        if (wmcProfile) {
            const double totalMs = toMs(stageStart);
            std::size_t componentWithEvidence = 0;
            for (bool hasEv : componentHasEvidence) {
                if (hasEv) {
                    componentWithEvidence++;
                }
            }
            std::cout << "[wmc-profile] stage=INC"
                      << " mode=" << souffle::incrementalFcProfileModeLabel(
                              useRegional ? FcMode::INC_REGIONAL : FcMode::INC_NAIVE)
                      << " total_ms=" << totalMs
                      << " components=" << componentCount
                      << " components_ev=" << componentWithEvidence
                      << " nodes=" << outputNodes.size()
                      << " changed_nodes=" << changedNodes.size()
                      << " node_reuse=" << nodeReuse
                      << " node_recompute=" << nodeRecompute
                      << " evidence_build_ms=" << evidenceBuildMs
                      << " evidence_make_and_calls=" << evidenceMakeAndCalls
                      << " evidence_make_and_ms=" << evidenceMakeAndMs
                      << " evidence_wmc_calls=" << evidenceWmcCalls
                      << " evidence_wmc_compute_ms=" << evidenceWmcComputeMs
                      << " node_make_and_calls=" << nodeMakeAndCalls
                      << " node_make_and_ms=" << nodeMakeAndMs
                      << " node_wmc_calls=" << nodeWmcCalls
                      << " node_wmc_compute_ms=" << nodeWmcComputeMs
                      << " dep_graph_ms=" << depGraphMs
                      << " output_loop_ms=" << outputLoopMs
                      << " output_classify_ms=" << outputClassifyMs
                      << " region_lookup_ms=" << regionLookupMs
                      << " changed_lookup_ms=" << changedLookupMs
                      << " component_id_ms=" << componentIdMs
                      << " prob_lookup_ms=" << probLookupMs
                      << " prob_write_ms=" << probWriteMs
                      << " weight_apply_calls=" << weightApplyCalls
                      << " weight_apply_vars=" << weightApplyVars
                      << " weight_apply_ms=" << weightApplyMs
                      << " weight_toggle_calls=" << weightToggleCalls
                      << " live_nodes=" << ddManager->getLiveNodeCount()
                      << std::endl;
        }
        debugger.endStage();
    }

    std::vector<std::string> outputRelations;
    void setOutputRelations(const std::vector<std::string> outputRelationNames) {
        this->outputRelations = outputRelationNames;
    }

    void runFullWeightedModelCounting(const IncSubgraphView& view, const char* profileModeLabel) {
        probResult.clear();
        const bool wmcProfile = wmcProfileEnabled;
        using Clock = std::chrono::steady_clock;
        auto toMs = [](Clock::time_point start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        };
        auto stageStart = Clock::now();
        double evidenceBuildMs = 0.0;
        double evidenceMakeAndMs = 0.0;
        double evidenceWmcComputeMs = 0.0;
        double nodeMakeAndMs = 0.0;
        double nodeWmcComputeMs = 0.0;
        std::size_t evidenceWmcCalls = 0;
        std::size_t nodeWmcCalls = 0;
        std::size_t evidenceMakeAndCalls = 0;
        std::size_t nodeMakeAndCalls = 0;
        auto makeAndProfile = [&](const NodeRef& lhs, const NodeRef& rhs,
                                  double& ms, std::size_t& calls) {
            if (!wmcProfile) {
                return ddManager->makeAnd(lhs, rhs);
            }
            auto andStart = Clock::now();
            auto res = ddManager->makeAnd(lhs, rhs);
            ms += toMs(andStart);
            calls++;
            return res;
        };
        auto computeWmcProfile = [&](const NodeRef& node, double& ms, std::size_t& calls) {
            calls++;
            if (!wmcProfile) {
                return ddManager->computeWeightedModelCount(node);
            }
            auto wmcStart = Clock::now();
            double res = ddManager->computeWeightedModelCount(node);
            ms += toMs(wmcStart);
            return res;
        };

        auto& depGraph = view.getCycleDependencyGraph();
        size_t componentCount = depGraph.getComponentCount();
        std::vector<NodeRef> componentEvidence(componentCount, ddManager->getTrue());
        std::vector<double> componentEvidenceWeight(componentCount, 1.0);
        std::vector<bool> componentHasEvidence(componentCount, false);

        auto evidenceBuildStart = Clock::now();
        for (size_t cid = 0; cid < componentCount; ++cid) {
            const auto& evidences = depGraph.getComponentEvidences(cid);
            if (evidences.empty()) {
                continue;
            }
            componentHasEvidence[cid] = true;
            NodeRef evidenceNode = ddManager->getTrue();
            for (const auto& [node, val] : evidences) {
                auto it = nodeFormulas->find(node);
                if (it == nodeFormulas->end()) {
                    throw std::runtime_error(
                            "Evidence node has no formula: " + node->getTuple().toString());
                }
                NodeRef lit = it->second;
                if (!val) {
                    lit = ddManager->makeNot(lit);
                }
                evidenceNode = makeAndProfile(
                        evidenceNode, lit, evidenceMakeAndMs, evidenceMakeAndCalls);
            }
            componentEvidence[cid] = evidenceNode;
        }
        evidenceBuildMs = toMs(evidenceBuildStart);

        for (size_t cid = 0; cid < componentCount; ++cid) {
            if (componentHasEvidence[cid]) {
                componentEvidenceWeight[cid] = computeWmcProfile(
                        componentEvidence[cid], evidenceWmcComputeMs, evidenceWmcCalls);
            }
        }

        for (auto& [node, formula] : *nodeFormulas) {
            if (!node->needOutput) {
                continue;
            }
            size_t cid = depGraph.getComponentId(node);
            if (!componentHasEvidence[cid]) {
                probResult[node] = computeWmcProfile(formula, nodeWmcComputeMs, nodeWmcCalls);
                continue;
            }
            if (componentEvidenceWeight[cid] == 0.0) {
                probResult[node] = 0.0;
                continue;
            }
            auto joint = makeAndProfile(
                    formula, componentEvidence[cid], nodeMakeAndMs, nodeMakeAndCalls);
            double jointW = computeWmcProfile(joint, nodeWmcComputeMs, nodeWmcCalls);
            probResult[node] = jointW / componentEvidenceWeight[cid];
        }
        for (const auto& [node, prob] : precomputedProbResult) {
            probResult.emplace(node, prob);
        }
        if (wmcProfile) {
            std::size_t componentWithEvidence = 0;
            for (bool hasEv : componentHasEvidence) {
                if (hasEv) {
                    componentWithEvidence++;
                }
            }
            std::cout << "[wmc-profile] stage=FULL"
                      << " mode=" << profileModeLabel
                      << " total_ms=" << toMs(stageStart)
                      << " components=" << componentCount
                      << " components_ev=" << componentWithEvidence
                      << " nodes=" << view.getValidNodes().size()
                      << " evidence_build_ms=" << evidenceBuildMs
                      << " evidence_make_and_calls=" << evidenceMakeAndCalls
                      << " evidence_make_and_ms=" << evidenceMakeAndMs
                      << " evidence_wmc_calls=" << evidenceWmcCalls
                      << " evidence_wmc_compute_ms=" << evidenceWmcComputeMs
                      << " node_make_and_calls=" << nodeMakeAndCalls
                      << " node_make_and_ms=" << nodeMakeAndMs
                      << " node_wmc_calls=" << nodeWmcCalls
                      << " node_wmc_compute_ms=" << nodeWmcComputeMs
                      << " live_nodes=" << ddManager->getLiveNodeCount()
                      << std::endl;
        }
    }
    size_t iteration = 1;
    void commit();

    static IncrementalCLI* instance;
    bool running;

    bool processInputLine(std::string line) {
        line = souffle::cli::normalizeInputLine(std::move(line));
        if (line.empty()) {
            return true;
        }
        return processCommand(line);
    }

    void processCommandStream(std::istream& input) {
        std::string singleCommand;
        while (running && std::getline(input, singleCommand)) {
            running = processInputLine(std::move(singleCommand));
        }
    }

    static void line_handler(char* line) {
        if (!line) {
            instance->running = false;
            std::cout << std::endl;
            return;
        }

        // Add the raw input (possibly multi-line) to history (only once).
        add_history(line);

        // Convert C string to a C++ stringstream for line splitting.
        std::stringstream ss(line);
        free(line); // Free memory immediately after conversion.
        instance->processCommandStream(ss);
    }

    void runNonInteractiveShell() {
        processCommandStream(std::cin);
    }

    void runInteractiveShell() {
        // 1. Install the callback handler.
        //    Arg 1: interactive prompt
        //    Arg 2: pointer to the line_handler defined above
        rl_callback_handler_install("> ", line_handler);

        // 2. Enter the main event loop.
        while (this->running) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds); // STDIN_FILENO is the file descriptor for stdin, usually 0.

            int result = select(STDIN_FILENO + 1, &fds, NULL, NULL, NULL);

            if (result < 0) { // If select fails
                perror("select"); // Print error info
                break;
            }

            if (FD_ISSET(STDIN_FILENO, &fds)) {
                rl_callback_read_char();
            }
        }

        // 5. On exit, clean up and remove the callback handler.
        rl_callback_handler_remove();
    }

    void run() {
        IncrementalCLI::instance = this;
        const bool interactive = isatty(STDIN_FILENO);
        if (interactive || opt.isVerboseEnabled()) {
            std::cout << "Incremental Souffle CLI (Callback Version)" << std::endl;
            std::cout << "Type 'help' for a list of available commands" << std::endl;
        }
        if (!interactive) {
            runNonInteractiveShell();
            return;
        }
        runInteractiveShell();
    }

};

template <typename T>
IncrementalCLI<T>* IncrementalCLI<T>::instance = nullptr;

#include "souffle/cli/Executor.h"

template <typename NodeRef>
bool IncrementalCLI<NodeRef>::dispatchCommand(const ParsedCommand& command) {
    return souffle::cli::IncrementalCommandExecutor<NodeRef>(*this).execute(command);
}

template <typename NodeRef>
void IncrementalCLI<NodeRef>::commit() {
    souffle::cli::IncrementalCommandExecutor<NodeRef>(*this).commit();
}

#endif //CLI_H
