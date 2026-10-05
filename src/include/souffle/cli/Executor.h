#ifndef SOUFFLE_CLI_EXECUTOR_H
#define SOUFFLE_CLI_EXECUTOR_H

#include <algorithm>
#include <set>
#include <sstream>
#include <vector>

#include "souffle/cli/Command.h"
#include "souffle/cli/PendingOperation.h"

template <typename NodeRef>
class IncrementalCLI;

namespace souffle::cli {

template <typename NodeRef>
class IncrementalCommandExecutor {
public:
    using Cli = ::IncrementalCLI<NodeRef>;
    using ParsedCommand = souffle::cli::ParsedCommand;
    using CommandKind = souffle::cli::CommandKind;

    explicit IncrementalCommandExecutor(Cli& cli) : cli(cli) {}

    bool execute(const ParsedCommand& command) {
        switch (command.kind) {
            case CommandKind::HELP:
                cli.printHelp();
                return true;
            case CommandKind::INSERT:
                cli.handleInsertCommand(command);
                return true;
            case CommandKind::DELETE:
                cli.handleDeleteCommand(command);
                return true;
            case CommandKind::LIST:
                cli.handleListCommand();
                return true;
            case CommandKind::SETMODE:
                cli.handleSetModeCommand(command);
                return true;
            case CommandKind::SET:
                cli.handleSetCommand(command);
                return true;
            case CommandKind::UNSET:
                cli.handleUnsetCommand(command);
                return true;
            case CommandKind::SHOW:
                cli.handleShowCommand(command);
                return true;
            case CommandKind::COMMIT:
                handleCommitCommand();
                return true;
            case CommandKind::EXIT:
                return false;
            case CommandKind::UNKNOWN:
                std::cout << "Unknown command: " << command.verb << std::endl;
                std::cout << "Use 'help' to see available commands" << std::endl;
                return true;
        }
        return true;
    }

    void commit() {
        DerivationManager::freeRuleApplicationMap(
                DerivationManager::untypedTuple2DeltaInsertRuleApplications);
        DerivationManager::freeRuleApplicationMap(
                DerivationManager::untypedTuple2DeltaDeleteRuleApplications);
        DerivationManager::freeRuleApplicationMap(
                DerivationManager::untypedTuple2DeltaDeltaInsertRuleApplications);
        DerivationManager::freeRuleApplicationMap(
                DerivationManager::untypedTuple2DeltaDeltaDeleteRuleApplications);
        if (cli.program == nullptr) {
            assert(false && "No program loaded.");
        }

        const IncrementalModeSpec requestedMode = cli.modeSpec;
        const IncrementalModeSpec effectiveMode = cli.resolveCommittedModeSpec();
        cli.logCommittedModeResolution(requestedMode, effectiveMode);
        cli.assertModeCompatibleWithGraphState(effectiveMode);
        struct ModeRestoreGuard {
            Cli& cli;
            IncrementalModeSpec saved;
            ~ModeRestoreGuard() { cli.modeSpec = saved; }
        } modeRestore{cli, requestedMode};
        cli.modeSpec = effectiveMode;

        cli.stagePendingOperationsForProgram();

        if (cli.isIncrementalSemMode()) {
            handleIncrementalSemCommit();
        } else if (cli.isFullSemMode()) {
            handleFullSemCommit();
        } else {
            assert(false && "Unsupported online mode");
        }

        cli.updateFcStateAfterTurn(requestedMode, effectiveMode);

        cli.pendingOperations.clear();
    }

private:
    static void stabilizeFactSemanticIds(
            const IncrementalDerivationGraph& oldGraph, IncrementalDerivationGraph& newGraph) {
        std::unordered_map<UntypedTuple, size_t> oldFactSemanticIds;
        oldFactSemanticIds.reserve(oldGraph.getNodes().size());
        size_t nextSemanticId = 0;
        for (const auto& node : oldGraph.getNodes()) {
            if (!node || !node->isFact) {
                continue;
            }
            oldFactSemanticIds.emplace(node->getTuple(), node->getSemanticFactId());
            nextSemanticId = std::max(nextSemanticId, node->getSemanticFactId() + 1);
        }

        std::vector<NodePtr> newFactNodes;
        newFactNodes.reserve(newGraph.getNodes().size());
        for (const auto& node : newGraph.getNodes()) {
            if (node && node->isFact) {
                newFactNodes.push_back(node);
            }
        }
        std::sort(newFactNodes.begin(), newFactNodes.end(),
                [](const NodePtr& lhs, const NodePtr& rhs) {
                    return lhs->getTuple() < rhs->getTuple();
                });

        for (const auto& node : newFactNodes) {
            auto it = oldFactSemanticIds.find(node->getTuple());
            if (it != oldFactSemanticIds.end()) {
                node->setSemanticFactId(it->second);
            } else {
                node->setSemanticFactId(nextSemanticId++);
            }
        }
    }

    void handleCommitCommand() {
        commit();
    }

    void handleIncrementalSemCommit() {
        const bool useRegional = cli.isRegionalFcMode();
        Debugger& debugger = Debugger::getInstance();
        bool hasDelete = false;
        bool hasInsert = false;
        std::string phaseLabel = "mixed";
        bool useRegionalCompute = false;
        std::string computeModeLabel = "INC_NAIVE";
        {
            auto emitDredInfo = [&]() {
                const auto& stats = DerivationManager::dredStats;
                debugger.addInfo("dred_del_ruleapp_recorded", std::to_string(stats.del_ruleapp_recorded));
                debugger.addInfo("dred_del_ruleapp_delta_delta", std::to_string(stats.del_ruleapp_delta_delta));
                debugger.addInfo("dred_del_ruleapp_overdelete", std::to_string(stats.del_ruleapp_overdelete));
                debugger.addInfo("dred_del_complete_scan_calls", std::to_string(stats.del_complete_scan_calls));
                debugger.addInfo("dred_del_complete_scan_elems", std::to_string(stats.del_complete_scan_elems));
                debugger.addInfo("dred_del_delta_tuples", std::to_string(stats.del_delta_tuples));
                debugger.addInfo("dred_del_delta_ruleapps", std::to_string(stats.del_delta_ruleapps));
                debugger.addInfo("dred_del_ruleapp_erases", std::to_string(stats.del_ruleapp_erases));
                debugger.addInfo("dred_del_tuple_deletes", std::to_string(stats.del_tuple_deletes));
                debugger.addInfo("dred_ins_ruleapp_recorded", std::to_string(stats.ins_ruleapp_recorded));
                debugger.addInfo("dred_ins_ruleapp_delta_delta", std::to_string(stats.ins_ruleapp_delta_delta));
                debugger.addInfo(
                        "dred_ins_ruleapp_rederive_erases", std::to_string(stats.ins_ruleapp_rederive_erases));
                debugger.addInfo("dred_ins_delta_tuples", std::to_string(stats.ins_delta_tuples));
                debugger.addInfo("dred_ins_delta_ruleapps", std::to_string(stats.ins_delta_ruleapps));
                debugger.addInfo("dred_rederive_delta_tuples", std::to_string(stats.rederive_delta_tuples));
                debugger.addInfo("dred_rederive_delta_ruleapps", std::to_string(stats.rederive_delta_ruleapps));
                debugger.addInfo("dred_ins_ruleapp_merges", std::to_string(stats.ins_ruleapp_merges));
                debugger.addInfo("dred_ins_tuple_inserts", std::to_string(stats.ins_tuple_inserts));
                debugger.addInfo("dred_del_time_total_ns", std::to_string(stats.del_time_total_ns));
                debugger.addInfo("dred_ins_time_total_ns", std::to_string(stats.ins_time_total_ns));
                debugger.addInfo("dred_red_time_total_ns", std::to_string(stats.red_time_total_ns));
            };
            for (const auto& op : cli.pendingOperations) {
                if (!op.valid) {
                    continue;
                }
                if (op.type == Cli::Operation::DELETE) {
                    hasDelete = true;
                } else if (op.type == Cli::Operation::INSERT) {
                    hasInsert = true;
                }
            }
            phaseLabel = "mixed";
            if (hasDelete && !hasInsert) {
                phaseLabel = "delete";
            } else if (hasInsert && !hasDelete) {
                phaseLabel = "insert";
            }
            // Deletion-only regional turns should be identical to the classic incremental path.
            useRegionalCompute = useRegional && hasInsert;
            computeModeLabel = useRegionalCompute ? "INC_REGIONAL" : "INC_NAIVE";
            DerivationManager::resetDredStats();
            DerivationManager::clearDetDeltaTuples();
            cli.beginTurnTrace();
            debugger.startStage(StageKind::SEMINAIVE_INC);
            cli.program->runAllInc(cli.program->getInputDirectory(), cli.program->getOutputDirectory(), true);
            debugger.addInfo("dred_phase", phaseLabel);
            emitDredInfo();
            debugger.endStage();
            if (DerivationManager::isSemStatsEnabled()) {
                std::ostringstream label;
                label << "iter=" << cli.iteration << " phase=" << phaseLabel;
                DerivationManager::dumpRuleApplicationSummary(std::cout, label.str());
                DerivationManager::dumpDredStats(std::cout, label.str());
            }
            if (cli.opt.isDredProfileEnabled()) {
                std::cout << "[dred-debug] relation sizes after SEMINAIVE_INC:\n";
                const std::array<std::string, 4> prefixes = {
                        "$inc_delta_derv_delete_",
                        "$inc_delta_tuple_delete_",
                        "$inc_derv_overdelete_",
                        "$inc_tuple_overdelete_",
                };
                for (auto* rel : cli.program->getAllRelations()) {
                    const std::string& name = rel->getName();
                    for (const auto& prefix : prefixes) {
                        if (name.rfind(prefix, 0) == 0) {
                            std::cout << "  " << name << " size=" << rel->size() << "\n";
                            break;
                        }
                    }
                }
            }
        }
        DerivationGraphViewInterface::setDumpOutputDir(cli.opt.getOutputFileDir());
        const DeltaMetadataPolicy deltaPolicy =
                cli.isIncrementalFcMode() ? DeltaMetadataPolicy::Retain : DeltaMetadataPolicy::Drop;
        debugger.addInfo("delta_metadata_policy",
                shouldRetainDeltaMetadata(deltaPolicy) ? "retain" : "drop");
        debugger.startStage(StageKind::PRUNING_INC);
        if (cli.opt.isDumpStatEnabled()) {
            std::cout << "[prune-inc] pre-applyDelta statistics:\n";
            cli.graph->dumpStatisticsInc(std::cout);
        }
        auto factProbInc = cli.getFactProbInc();
        auto deletedFacts = cli.getDeletedFacts(&factProbInc);
        {
            FunctionTimer timer("PRUNING_INC: applyDelta");
            cli.graph->applyDelta(
                    DerivationManager::untypedTuple2DeltaInsertRuleApplications,
                    DerivationManager::untypedTuple2DeltaDeleteRuleApplications,
                    *cli.ruleManager, factProbInc, deletedFacts, deltaPolicy);
        }
        DerivationManager::clearDetDeltaTuples();
        cli.logApplyDeltaOpsSummary(
                DerivationManager::untypedTuple2DeltaInsertRuleApplications,
                DerivationManager::untypedTuple2DeltaDeleteRuleApplications, factProbInc, deletedFacts,
                computeModeLabel);
        cli.logApplyDeltaSummary(*cli.graph, computeModeLabel);
        cli.logApplyDeltaGraphSummary(*cli.graph, computeModeLabel);
        if (cli.opt.isDumpDotEnabled()) {
            FunctionTimer timer("PRUNING_INC: dumpDot-before-prune");
            cli.graph->dumpDotInc(
                    cli.outputPath("derivation-inc-before-prune" + std::to_string(cli.iteration) + ".dot"));
        }
        if (cli.opt.isDumpJsonBeforePruneEnabled()) {
            FunctionTimer timer("PRUNING_INC: dumpJson-before-prune");
            cli.graph->dumpJsonInc(
                    cli.outputTimestampedPath("derivation-inc-before-prune", cli.iteration, ".json"));
        }
        IncSubgraphView view = [&] {
            FunctionTimer timer("PRUNING_INC: prune");
            // TODO(inc-region): This hook is currently a no-op after the regional
            // reach-filter path moved into IncRegionAnalyzer. Remove it after the
            // split-mode pruning pipeline is re-audited.
            cli.graph->setBuildInsertImpacts(useRegionalCompute);
            return cli.graph->prune(cli.program->getOutputRelations(), deltaPolicy);
        }();
        if (cli.opt.isDumpDotEnabled()) {
            FunctionTimer timer("PRUNING_INC: dumpDot-after-prune");
            view.dumpDotInc(cli.outputPath(
                    "derivation-inc-after-prune" + std::to_string(cli.iteration) + ".dot"));
        }
        if (cli.opt.isDumpJsonEnabled()) {
            FunctionTimer timer("PRUNING_INC: dumpJson-after-prune");
            view.dumpJsonInc(
                    cli.outputTimestampedPath("derivation-inc-after-prune", cli.iteration, ".json"));
        }
        if (cli.ddManager != nullptr) {
            std::set<int> deletedVarsIndex;
            std::set<int> insertedVarsIndex;
            std::size_t insertedFactVars = 0;
            std::size_t insertedEdgeVars = 0;

            for (const auto& node : view.getDeltaDeleteNodes()) {
                if (!node || !node->isFact || node->getProbability() == 1.0) {
                    continue;
                }
                deletedVarsIndex.insert(cli.ddManager->getVarIndex(*node));
            }
            for (const auto& edge : view.getDeltaDeleteEdges()) {
                if (!edge || edge->isDeterministic()) {
                    continue;
                }
                deletedVarsIndex.insert(cli.ddManager->getVarIndex(*edge));
            }

            for (const auto& node : view.getDeltaInsertFactNodes()) {
                if (!node || node->getProbability() == 1.0) {
                    continue;
                }
                if (insertedVarsIndex.insert(cli.ddManager->getVarIndex(*node)).second) {
                    insertedFactVars++;
                }
            }
            for (const auto& edge : view.getDeltaInsertEdges()) {
                if (!edge || edge->isDeterministic()) {
                    continue;
                }
                if (insertedVarsIndex.insert(cli.ddManager->getVarIndex(*edge)).second) {
                    insertedEdgeVars++;
                }
            }

            debugger.addInfo("fc_deleted_random_vars", std::to_string(deletedVarsIndex.size()));
            debugger.addInfo("fc_inserted_random_vars", std::to_string(insertedVarsIndex.size()));
            debugger.addInfo("fc_inserted_fact_random_vars", std::to_string(insertedFactVars));
            debugger.addInfo("fc_inserted_edge_random_vars", std::to_string(insertedEdgeVars));
        }
        debugger.endStage();
        cli.logPrunedDeltaSummary(view, computeModeLabel);
        cli.changedNodes.clear();
        if (cli.isIncrementalFcMode()) {
            debugger.startStage(StageKind::FORWARD_COMPILATION_INC);
            if (useRegionalCompute) {
                buildFormulasIncRegionalCyclewise(
                        view, *cli.ddManager, *cli.nodeFormulas, *cli.edgeFormulas, cli.changedNodes);
            } else {
                if (useRegional) {
                    clearIncRegionalStateForClassicPath(*cli.ddManager);
                }
                buildFormulasIncCyclewise(
                        view, *cli.ddManager, *cli.nodeFormulas, *cli.edgeFormulas, cli.changedNodes);
            }
            debugger.endStage();
            cli.runIncrementalWmc(view, useRegionalCompute);
            cli.dumpCurrentTurnProbabilities();
        } else if (cli.isFullFcMode()) {
            const auto fcSetupStart = std::chrono::steady_clock::now();
            cli.nodeFormulas->clear();
            cli.edgeFormulas->clear();
            if (cli.modeSpec.fc == FcMode::FULL_HARD) {
                cli.ddManager->resetHard();
            } else {
                cli.ddManager->reset();
            }
            const auto fcSetupMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - fcSetupStart)
                                           .count();
            debugger.addInfo("fc_setup_ms", std::to_string(fcSetupMs));
            debugger.addInfo("fc_setup_scope", "inc-sem-full-fc");
            debugger.startStage(StageKind::FORWARD_COMPILATION_FULL);
            buildFormulasCyclewise(view, *cli.ddManager, *cli.nodeFormulas, *cli.edgeFormulas);
            debugger.endStage();

            debugger.startStage(StageKind::WEIGHTED_MODEL_COUNTING_FULL);
            cli.runFullWeightedModelCounting(view, souffle::incrementalFcProfileModeLabel(cli.modeSpec.fc));
            debugger.endStage();
            cli.dumpCurrentTurnProbabilitiesWithStage(StageKind::IO_DUMP_FULL);
        } else {
            assert(false && "Unsupported incremental turn mode");
        }
        cli.finishTurn();
    }

    void handleFullSemCommit() {
        const bool useIncFc = cli.isIncrementalFcMode();
        const bool useRegionalFc = (cli.modeSpec.fc == FcMode::INC_REGIONAL);
        Debugger& debugger = Debugger::getInstance();
        IncrementalDerivationGraph* oldGraph = cli.graph;
        std::unique_ptr<IncSubgraphView> oldPrunedView;
        std::vector<std::pair<UntypedTuple, bool>> evidenceList;
        if (cli.graph) {
            evidenceList = cli.graph->getEvidences();
        }

        cli.beginTurnTrace();
        if (useIncFc && cli.graph != nullptr) {
            const auto oldPruneStart = std::chrono::steady_clock::now();
            oldPrunedView = std::make_unique<IncSubgraphView>(cli.graph->prune(cli.program->getOutputRelations()));
            const auto oldPruneMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - oldPruneStart)
                                            .count();
            debugger.addInfo("full_inc_old_prune_ms", std::to_string(oldPruneMs));
        } else if (cli.graphOwner != nullptr && cli.graphOwner->get() != nullptr) {
            cli.graphOwner->reset();
            cli.graph = nullptr;
        }

        if (cli.ddManager != nullptr && !useIncFc) {
            const auto fcSetupStart = std::chrono::steady_clock::now();
            cli.nodeFormulas->clear();
            cli.edgeFormulas->clear();
            if (cli.modeSpec.fc == FcMode::FULL_HARD) {
                cli.ddManager->resetHard();
            } else {
                cli.ddManager->reset();
            }
            const auto fcSetupMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - fcSetupStart)
                                           .count();
            debugger.addInfo("fc_setup_ms", std::to_string(fcSetupMs));
            debugger.addInfo("fc_setup_scope", "full-sem-full-fc");
        }
        cli.purgeAllRelations();
        cli.loadInitialInputRelations();
        DerivationManager::freeRuleApplicationMap(DerivationManager::untypedTuple2RuleApplications);
        debugger.startStage(StageKind::SEMINAIVE_FULL);
        cli.program->runAll(cli.opt.getInputFileDir(), cli.opt.getOutputFileDir(), false);
        auto newGraph = std::unique_ptr<IncrementalDerivationGraph>(IncrementalDerivationGraph::createFrom(
                DerivationManager::untypedTuple2RuleApplications, *cli.ruleManager, *cli.queryManager,
                fact_prob, evidenceList));
        if (useIncFc && oldGraph != nullptr && newGraph != nullptr) {
            stabilizeFactSemanticIds(*oldGraph, *newGraph);
        }
        cli.replaceGraph(std::move(newGraph));
        debugger.endStage();
        if (cli.opt.isDumpDotEnabled()) {
            FunctionTimer timer("PRUNING_FULL: dumpDot-before-prune");
            cli.graph->dumpDotInc(
                    cli.outputPath("derivation-full-before-prune" + std::to_string(cli.iteration) + ".dot"));
        }
        if (cli.opt.isDumpJsonBeforePruneEnabled()) {
            FunctionTimer timer("PRUNING_FULL: dumpJson-before-prune");
            cli.graph->dumpJsonInc(
                    cli.outputTimestampedPath("derivation-full-before-prune", cli.iteration, ".json"));
        }
        debugger.startStage(StageKind::PRUNING_FULL);
        IncSubgraphView view = [&] {
            FunctionTimer timer("PRUNING_FULL: prune");
            return cli.graph->prune(cli.program->getOutputRelations());
        }();
        if (cli.opt.isDumpDotEnabled()) {
            FunctionTimer timer("PRUNING_FULL: dumpDot-after-prune");
            view.dumpDotInc(
                    cli.outputPath("derivation-full-after-prune" + std::to_string(cli.iteration) + ".dot"));
        }
        if (cli.opt.isDumpJsonEnabled()) {
            FunctionTimer timer("PRUNING_FULL: dumpJson-after-prune");
            view.dumpJsonInc(
                    cli.outputTimestampedPath("derivation-full-after-prune", cli.iteration, ".json"));
        }
        debugger.endStage();

        std::unique_ptr<IncSubgraphView> diffView;
        IncSubgraphView* activeView = &view;
        if (useIncFc) {
            if (!oldPrunedView) {
                assert(false && "Full refresh with incremental FC requires existing previous pruned view");
            }
            std::unordered_map<NodePtr, NodePtr> oldToNewNodes;
            std::unordered_map<EdgePtr, EdgePtr> oldToNewEdges;
            {
                FunctionTimer timer("PRUNING_FULL: post-prune-diff");
                const auto diffStart = std::chrono::steady_clock::now();
                diffView = std::make_unique<IncSubgraphView>(
                        cli.buildPostPruneDiffView(*oldPrunedView, view, oldToNewNodes, oldToNewEdges));
                cli.remapStateForPostPruneDiff(oldToNewNodes, oldToNewEdges);
                const auto diffMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - diffStart)
                                            .count();
                debugger.addInfo("full_inc_diff_remap_ms", std::to_string(diffMs));
            }
            cli.changedNodes.clear();
            debugger.startStage(StageKind::FORWARD_COMPILATION_INC);
            if (useRegionalFc) {
                buildFormulasIncRegionalCyclewise(
                        *diffView, *cli.ddManager, *cli.nodeFormulas, *cli.edgeFormulas, cli.changedNodes);
            } else {
                buildFormulasIncCyclewise(
                        *diffView, *cli.ddManager, *cli.nodeFormulas, *cli.edgeFormulas, cli.changedNodes);
            }
            debugger.endStage();
            activeView = diffView.get();
            cli.logPrunedDeltaSummary(*diffView, useRegionalFc ? "INC_REGIONAL" : "INC_NAIVE");
        } else {
            debugger.startStage(StageKind::FORWARD_COMPILATION_FULL);
            cli.nodeFormulas->clear();
            cli.edgeFormulas->clear();
            buildFormulasCyclewise(view, *cli.ddManager, *cli.nodeFormulas, *cli.edgeFormulas);
            debugger.endStage();
        }

        if (useIncFc) {
            cli.runIncrementalWmc(*activeView, useRegionalFc);
        } else {
            debugger.startStage(StageKind::WEIGHTED_MODEL_COUNTING_FULL);
            cli.runFullWeightedModelCounting(
                    *activeView,
                    souffle::fullSemProfileModeLabel(useIncFc ? cli.modeSpec.fc : FcMode::FULL_HARD));
            debugger.endStage();
        }
        cli.dumpCurrentTurnProbabilitiesWithStage(StageKind::IO_DUMP_FULL);
        cli.finishTurn();
    }

    Cli& cli;
};

}  // namespace souffle::cli

#endif
