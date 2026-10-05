#ifndef CUDDMANAGER_H
#define CUDDMANAGER_H

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <iostream>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_set>
#include <cstdint>
#include "souffle/problog/formula/FormulaManager.h"
#include "souffle/Derivation.h"
#include "souffle/problog/DerivationGraph.h"
#include "souffle/problog/debug/Debugger.h"
extern "C" {
#include <cudd.h>
}

using Clock = std::chrono::steady_clock;
using Duration = std::chrono::duration<double>;

static constexpr bool kCuddVerbose = false;
extern bool fcProfileEnabled;
inline std::string cuddPreConfigTag;
inline void setCuddPreConfigTag(const std::string& tag) {
    cuddPreConfigTag = tag;
}
inline const std::string& getCuddPreConfigTag() {
    return cuddPreConfigTag;
}
inline Cudd_ReorderingType currentReorderingType = CUDD_REORDER_SAME;
inline std::chrono::time_point<Clock> _cudd_gc_start_time;
inline std::chrono::time_point<Clock> _cudd_gc_end_time;
inline int _cudd_gc_count = 0;
inline bool gc_begin = true;
inline int _cudd_reordering_count = 0;
inline std::chrono::time_point<Clock> _cudd_reordering_start_time;
inline std::chrono::time_point<Clock> _cudd_reordering_end_time;
inline bool reordering_begin = true;

static inline void resetReorderingState() {
    currentReorderingType = CUDD_REORDER_SAME;
    gc_begin = true;
    reordering_begin = true;
    _cudd_gc_count = 0;
    _cudd_reordering_count = 0;
}

inline void adaptiveReorder(DdManager* manager);
inline double getCacheHitRate(DdManager* manager);

class BddNodeRef {
friend class WeightedBDDManager;
public:
    // Default constructor
    BddNodeRef() : manager(nullptr), ddNode(nullptr) {}

    // Primary constructor
    BddNodeRef(std::shared_ptr<DdManager> m, DdNode* n) : manager(m), ddNode(n) {
        if (ddNode) Cudd_Ref(ddNode);
    }

    // Constructor with Node
    BddNodeRef(std::shared_ptr<DdManager> m, DdNode* n, const Node& node)
        : manager(m), ddNode(n), node(&node) {
        if (ddNode) Cudd_Ref(ddNode);
    }

    // Constructor with Hyperedge
    BddNodeRef(std::shared_ptr<DdManager> m, DdNode* n, const Hyperedge& edge)
        : manager(m), ddNode(n), edge(&edge) {
        if (ddNode) Cudd_Ref(ddNode);
    }

    // Copy constructor
    BddNodeRef(const BddNodeRef& other)
        : manager(other.manager), ddNode(other.ddNode),
          node(other.node), edge(other.edge) {
        if (ddNode) Cudd_Ref(ddNode);
    }

    // Copy assignment operator
    BddNodeRef& operator=(const BddNodeRef& other) {
        if (this != &other) {
            // Release existing resources
            if (ddNode && manager) Cudd_RecursiveDeref(manager.get(), ddNode);

            // Copy from other
            manager = other.manager;
            ddNode = other.ddNode;
            node = other.node;
            edge = other.edge;

            // Increment reference count for new node
            if (ddNode) Cudd_Ref(ddNode);
        }
        return *this;
    }

    // Move constructor
    BddNodeRef(BddNodeRef&& other) noexcept
        : manager(std::move(other.manager)),
          ddNode(other.ddNode),
          node(std::move(other.node)),
          edge(std::move(other.edge)) {
        other.ddNode = nullptr;
    }

    // Move assignment operator
    BddNodeRef& operator=(BddNodeRef&& other) noexcept {
        if (this != &other) {
            // Release existing resources
            if (ddNode && manager) Cudd_RecursiveDeref(manager.get(), ddNode);

            // Move from other
            manager = std::move(other.manager);
            ddNode = other.ddNode;
            node = std::move(other.node);
            edge = std::move(other.edge);

            // Reset other
            other.ddNode = nullptr;
        }
        return *this;
    }

    // Destructor
    ~BddNodeRef() {
        if (ddNode && manager) Cudd_RecursiveDeref(manager.get(), ddNode);
    }

    // Getter for the BDD node
    DdNode* get() const { return ddNode; }

private:
    std::shared_ptr<DdManager> manager;  // Shared ownership of manager
    DdNode* ddNode;
    std::optional<const Node*> node;
    std::optional<const Hyperedge*> edge;
};

struct VariableWeight {
    double posWeight;  // Weight when variable is true
    double negWeight;  // Weight when variable is false
};
class WeightedBDDManager: public DDManager<BddNodeRef> {
public:
    struct InitConfig {
        unsigned int numVars;
        unsigned int numVarsZ;
        unsigned int numSlots;
        unsigned int cacheSize;
        unsigned long maxMemory;

        InitConfig()
                : numVars(1000), numVarsZ(0), numSlots(512), cacheSize(1u << 24),
                  maxMemory(32UL * 1024 * 1024 * 1024) {}
    };

    explicit WeightedBDDManager(InitConfig config = InitConfig());
    ~WeightedBDDManager() override = default;

    int getVarIndex(const Node& node) override {
        auto it = nodeIndex_.find(&node);
        if (it != nodeIndex_.end()) return it->second;
        const auto semanticId = node.getSemanticFactId();
        if (node.isFact) {
            auto itSemantic = factSemanticIndex_.find(semanticId);
            if (itSemantic != factSemanticIndex_.end()) {
                nodeIndex_[&node] = itSemantic->second;
                ++factSemanticRefCount_[semanticId];
                return itSemantic->second;
            }
        }
        int idx = -1;
        if (reuseVarIndexEnabled) {
            auto freeIt = freeNodeIndexByTuple_.find(node.getTuple());
            if (freeIt != freeNodeIndexByTuple_.end()) {
                idx = freeIt->second;
                dropFreeIndex(idx);
            } else if (!freeIndices_.empty()) {
                idx = *freeIndices_.begin();
                dropFreeIndex(idx);
            }
        }
        if (idx < 0) {
            idx = nextVarIndex_++;
        }
        nodeIndex_[&node] = idx;
        if (node.isFact) {
            factSemanticIndex_[semanticId] = idx;
            ++factSemanticRefCount_[semanticId];
        }
        return idx;
    }

    bool peekVarIndex(const Node& node, int& idx) const override {
        auto it = nodeIndex_.find(&node);
        if (it != nodeIndex_.end()) {
            idx = it->second;
            return true;
        }
        if (!node.isFact) {
            return false;
        }
        auto itSemantic = factSemanticIndex_.find(node.getSemanticFactId());
        if (itSemantic == factSemanticIndex_.end()) {
            return false;
        }
        idx = itSemantic->second;
        return true;
    }

    int getVarIndex(const Hyperedge& edge) override {
        auto it = edgeIndex_.find(&edge);
        if (it != edgeIndex_.end()) return it->second;
        int idx = -1;
        if (reuseVarIndexEnabled) {
            auto freeIt = freeEdgeIndexByRuleApp_.find(edge.getRuleApp());
            if (freeIt != freeEdgeIndexByRuleApp_.end()) {
                idx = freeIt->second;
                dropFreeIndex(idx);
            } else if (!freeIndices_.empty()) {
                idx = *freeIndices_.begin();
                dropFreeIndex(idx);
            }
        }
        if (idx < 0) {
            idx = nextVarIndex_++;
        }
        edgeIndex_[&edge] = idx;
        return idx;
    }

    bool peekVarIndex(const Hyperedge& edge, int& idx) const override {
        auto it = edgeIndex_.find(&edge);
        if (it == edgeIndex_.end()) {
            return false;
        }
        idx = it->second;
        return true;
    }

    void bindVarIndex(const Node& node, int idx) override {
        if (idx < 0) {
            return;
        }
        auto it = nodeIndex_.find(&node);
        if (it != nodeIndex_.end() && it->second == idx) {
            return;
        }
        dropFreeIndex(idx);
        if (node.isFact && it == nodeIndex_.end()) {
            const auto semanticId = node.getSemanticFactId();
            factSemanticIndex_[semanticId] = idx;
            ++factSemanticRefCount_[semanticId];
        }
        nodeIndex_[&node] = idx;
        if (idx >= nextVarIndex_) {
            nextVarIndex_ = idx + 1;
        }
    }

    void bindVarIndex(const Hyperedge& edge, int idx) override {
        if (idx < 0) {
            return;
        }
        auto it = edgeIndex_.find(&edge);
        if (it != edgeIndex_.end() && it->second == idx) {
            return;
        }
        dropFreeIndex(idx);
        edgeIndex_[&edge] = idx;
        if (idx >= nextVarIndex_) {
            nextVarIndex_ = idx + 1;
        }
    }

    void releaseVarIndex(const Node& node) override {
        if (!reuseVarIndexEnabled) {
            return;
        }
        auto it = nodeIndex_.find(&node);
        if (it == nodeIndex_.end()) {
            return;
        }
        int idx = it->second;
        nodeIndex_.erase(it);
        if (node.isFact) {
            const auto semanticId = node.getSemanticFactId();
            auto refIt = factSemanticRefCount_.find(semanticId);
            if (refIt != factSemanticRefCount_.end()) {
                if (refIt->second > 1) {
                    --refIt->second;
                    return;
                }
                factSemanticRefCount_.erase(refIt);
                factSemanticIndex_.erase(semanticId);
            }
        }
        variableRegistry.erase(idx);
        if (weights.erase(idx) > 0) {
            ++weightsEpoch_;
        }
        dropFreeIndex(idx);
        freeIndices_.insert(idx);
        if (!node.isShadow) {
            freeNodeIndexByTuple_[node.getTuple()] = idx;
            freeIndexToTuple_[idx] = node.getTuple();
        }
    }
    void releaseVarIndex(const Hyperedge& edge) override {
        if (!reuseVarIndexEnabled) {
            return;
        }
        auto it = edgeIndex_.find(&edge);
        if (it == edgeIndex_.end()) {
            return;
        }
        int idx = it->second;
        edgeIndex_.erase(it);
        variableRegistry.erase(idx);
        if (weights.erase(idx) > 0) {
            ++weightsEpoch_;
        }
        dropFreeIndex(idx);
        freeIndices_.insert(idx);
        freeEdgeIndexByRuleApp_[edge.getRuleApp()] = idx;
        freeIndexToRuleApp_[idx] = edge.getRuleApp();
    }

    void tryGarbageCollection() {
        Cudd_ReduceHeap(manager.get(), CUDD_REORDER_NONE, 0);
    }
    void reset() override {
        nodeIndex_.clear();
        edgeIndex_.clear();
        factSemanticIndex_.clear();
        factSemanticRefCount_.clear();
        nextVarIndex_ = 0;
        freeIndices_.clear();
        freeNodeIndexByTuple_.clear();
        freeEdgeIndexByRuleApp_.clear();
        freeIndexToTuple_.clear();
        freeIndexToRuleApp_.clear();
        variableRegistry.clear();
        weights.clear();
        wmcCache_.clear();
        weightsEpoch_ = 0;
        wmcCacheEpoch_ = 0;
        reorderConfigured_ = false;
        unsigned int maxCacheHard = Cudd_ReadMaxCacheHard(manager.get());
        if (maxCacheHard > 1) {
            Cudd_SetMaxCacheHard(manager.get(), 1);
            Cudd_SetMaxCacheHard(manager.get(), maxCacheHard);
        }
        tryGarbageCollection();
    }
    void resetHard() override {
        nodeIndex_.clear();
        edgeIndex_.clear();
        factSemanticIndex_.clear();
        factSemanticRefCount_.clear();
        nextVarIndex_ = 0;
        freeIndices_.clear();
        freeNodeIndexByTuple_.clear();
        freeEdgeIndexByRuleApp_.clear();
        freeIndexToTuple_.clear();
        freeIndexToRuleApp_.clear();
        variableRegistry.clear();
        weights.clear();
        wmcCache_.clear();
        weightsEpoch_ = 0;
        wmcCacheEpoch_ = 0;
        last_reordering_time_ = 0;
        reorderConfigured_ = false;
        manager.reset();
        manager = initManager();
    }
    Debugger& debugger = Debugger::getInstance();
    int nextVarIndex_ = 0;
    std::unordered_map<const Node*, int> nodeIndex_;
    std::unordered_map<std::size_t, int> factSemanticIndex_;
    std::unordered_map<std::size_t, std::size_t> factSemanticRefCount_;
    std::unordered_map<const Hyperedge*, int> edgeIndex_;
    std::unordered_set<int> freeIndices_;
    std::unordered_map<UntypedTuple, int> freeNodeIndexByTuple_;
    std::unordered_map<RuleApplication, int> freeEdgeIndexByRuleApp_;
    std::unordered_map<int, UntypedTuple> freeIndexToTuple_;
    std::unordered_map<int, RuleApplication> freeIndexToRuleApp_;

    void dropFreeIndex(int idx) {
        freeIndices_.erase(idx);
        auto itTuple = freeIndexToTuple_.find(idx);
        if (itTuple != freeIndexToTuple_.end()) {
            freeNodeIndexByTuple_.erase(itTuple->second);
            freeIndexToTuple_.erase(itTuple);
        }
        auto itRule = freeIndexToRuleApp_.find(idx);
        if (itRule != freeIndexToRuleApp_.end()) {
            freeEdgeIndexByRuleApp_.erase(itRule->second);
            freeIndexToRuleApp_.erase(itRule);
        }
    }
    void preConfig(DerivationGraphViewInterface& view) override {
        static size_t iteration = 0;
        using namespace std::chrono;
        auto toMs = [](auto d) { return duration<double, std::milli>(d).count(); };
        const bool fcProfile = fcProfileEnabled;
        const bool verbose = DerivationGraphViewInterface::isVerboseEnabled();
        const auto totalStart = steady_clock::now();
        double cacheMs = 0.0;
        double factCreateMs = 0.0;
        double edgeCreateMs = 0.0;
        double reorderMs = 0.0;
        double factCreateMaxMs = 0.0;
        double edgeCreateMaxMs = 0.0;
        std::size_t factCreateCount = 0;
        std::size_t edgeCreateCount = 0;
        std::size_t factCreateSlow = 0;
        std::size_t edgeCreateSlow = 0;
        std::size_t factVarHits = 0;
        std::size_t factVarMisses = 0;
        std::size_t edgeVarHits = 0;
        std::size_t edgeVarMisses = 0;
        constexpr double slowCreateThresholdMs = 1.0;
        const bool profileCreate = fcProfile || incProfileEnabled;
        const bool profileTime = profileCreate;

        auto recordCreate = [&](double ms, double& total, double& maxMs, std::size_t& count, std::size_t& slowCount) {
            total += ms;
            count += 1;
            if (ms > maxMs) {
                maxMs = ms;
            }
            if (ms >= slowCreateThresholdMs) {
                slowCount += 1;
            }
        };

        if (profileTime) {
            auto cacheStart = steady_clock::now();
            wmcCache_.clear();
            wmcCacheEpoch_ = weightsEpoch_;
            cacheMs = toMs(steady_clock::now() - cacheStart);
        } else {
            wmcCache_.clear();
            wmcCacheEpoch_ = weightsEpoch_;
        }
        auto oldCuddVarSize = Cudd_ReadSize(manager.get());
        size_t factVars = 0;
        size_t edgeVars = 0;

        const auto* incView = dynamic_cast<const IncrementalDerivationGraphViewInterface*>(&view);
        const bool hasDelta =
                incView != nullptr &&
                (!incView->getDeltaInsertNodes().empty() || !incView->getDeltaInsertEdges().empty() ||
                        !incView->getDeltaDeleteNodes().empty() || !incView->getDeltaDeleteEdges().empty());
        auto factStart = steady_clock::now();
        if (incView != nullptr && hasDelta) {
            for (const auto& node : incView->getDeltaInsertNodes()) {
                    if (node->isFact && node->getProbability() < 1.0) {
                        int idx = getVarIndex(*node);
                        if (profileCreate) {
                            if (variableRegistry.find(idx) != variableRegistry.end()) {
                                ++factVarHits;
                            } else {
                                ++factVarMisses;
                            }
                        }
                        if (profileCreate) {
                            auto cvStart = steady_clock::now();
                            createVar(idx, *node);
                            recordCreate(toMs(steady_clock::now() - cvStart),
                                    factCreateMs, factCreateMaxMs, factCreateCount, factCreateSlow);
                        } else {
                            createVar(idx, *node);
                        }
                        ++factVars;
                    }
            }
        } else {
            for (const auto& node : view.getNodes()) {
                if (node->isFact && node->getProbability() < 1.0) {
                    int idx = getVarIndex(*node);
                    if (profileCreate) {
                        if (variableRegistry.find(idx) != variableRegistry.end()) {
                            ++factVarHits;
                        } else {
                            ++factVarMisses;
                        }
                    }
                    if (profileCreate) {
                        auto cvStart = steady_clock::now();
                        createVar(idx, *node);
                        recordCreate(toMs(steady_clock::now() - cvStart),
                                factCreateMs, factCreateMaxMs, factCreateCount, factCreateSlow);
                    } else {
                        createVar(idx, *node);
                    }
                    ++factVars;
                }
            }
        }
        double factMs = toMs(steady_clock::now() - factStart);

        auto edgeStart = steady_clock::now();
        if (incView != nullptr && hasDelta) {
            for (const auto& edge : incView->getDeltaInsertEdges()) {
                if (!edge->isDeterministic()) {
                    int idx = getVarIndex(*edge);
                    if (profileCreate) {
                        if (variableRegistry.find(idx) != variableRegistry.end()) {
                            ++edgeVarHits;
                        } else {
                            ++edgeVarMisses;
                        }
                    }
                    if (profileCreate) {
                        auto cvStart = steady_clock::now();
                        createVar(idx, *edge);
                        recordCreate(toMs(steady_clock::now() - cvStart),
                                edgeCreateMs, edgeCreateMaxMs, edgeCreateCount, edgeCreateSlow);
                    } else {
                        createVar(idx, *edge);
                    }
                    ++edgeVars;
                }
            }
        } else {
            for (const auto& edge : view.getEdges()) {
                if (!edge->isDeterministic()) {
                    int idx = getVarIndex(*edge);
                    if (profileCreate) {
                        if (variableRegistry.find(idx) != variableRegistry.end()) {
                            ++edgeVarHits;
                        } else {
                            ++edgeVarMisses;
                        }
                    }
                    if (profileCreate) {
                        auto cvStart = steady_clock::now();
                        createVar(idx, *edge);
                        recordCreate(toMs(steady_clock::now() - cvStart),
                                edgeCreateMs, edgeCreateMaxMs, edgeCreateCount, edgeCreateSlow);
                    } else {
                        createVar(idx, *edge);
                    }
                    ++edgeVars;
                }
            }
        }
        double edgeMs = toMs(steady_clock::now() - edgeStart);
        auto newCuddVarSize = Cudd_ReadSize(manager.get());
        size_t totalVarsAdded = factVars + edgeVars;
        if (verbose) {
            std::cout << "[CUDD] vars created: facts=" << factVars
                      << " edges=" << edgeVars
                      << " total=" << totalVarsAdded
                      << " (facts " << factMs << " ms, edges " << edgeMs << " ms)"
                      << std::endl;
        }

        if (!reorderConfigured_) {
            // Rely on CUDD adaptive dynamic reordering; skip heavy static heuristic ordering.
            if (verbose) {
                std::cout << "[CUDD] Enabling adaptive dynamic reordering (skip static ordering)" << std::endl;
            }
            if (profileTime) {
                auto reorderStart = steady_clock::now();
                adaptiveReorder(manager.get());
                reorderMs = toMs(steady_clock::now() - reorderStart);
            } else {
                adaptiveReorder(manager.get());
            }
            if (verbose) {
                std::cout << "[CUDD] Adaptive reordering initialized" << std::endl;
            }
            reorderConfigured_ = true;
        }
        if (fcProfile) {
            const auto totalMs = toMs(steady_clock::now() - totalStart);
            const std::string& rawTag = getCuddPreConfigTag();
            const std::string tag = rawTag.empty() ? "unknown" : rawTag;
            std::cout << "[fc-profile] stage=CUDD_PRECONFIG tag=" << tag
                      << " total_ms=" << totalMs
                      << " cache_ms=" << cacheMs
                      << " fact_loop_ms=" << factMs
                      << " fact_create_ms=" << factCreateMs
                      << " edge_loop_ms=" << edgeMs
                      << " edge_create_ms=" << edgeCreateMs
                      << " reorder_ms=" << reorderMs
                      << " old_var_size=" << oldCuddVarSize
                      << " new_var_size=" << newCuddVarSize
                      << " fact_vars=" << factVars
                      << " edge_vars=" << edgeVars
                      << " fact_create_count=" << factCreateCount
                      << " edge_create_count=" << edgeCreateCount
                      << " fact_create_max_ms=" << factCreateMaxMs
                      << " edge_create_max_ms=" << edgeCreateMaxMs
                      << " create_slow_threshold_ms=" << slowCreateThresholdMs
                      << " fact_create_slow=" << factCreateSlow
                      << " edge_create_slow=" << edgeCreateSlow
                      << " fact_hit=" << factVarHits
                      << " fact_miss=" << factVarMisses
                      << " edge_hit=" << edgeVarHits
                      << " edge_miss=" << edgeVarMisses
                      << std::endl;
        } else if (incProfileEnabled) {
            const auto totalMs = toMs(steady_clock::now() - totalStart);
            const std::string& rawTag = getCuddPreConfigTag();
            const std::string tag = rawTag.empty() ? "unknown" : rawTag;
            std::cout << "[inc-profile] stage=CUDD_PRECONFIG tag=" << tag
                      << " total_ms=" << totalMs
                      << " cache_ms=" << cacheMs
                      << " fact_loop_ms=" << factMs
                      << " fact_create_ms=" << factCreateMs
                      << " edge_loop_ms=" << edgeMs
                      << " edge_create_ms=" << edgeCreateMs
                      << " reorder_ms=" << reorderMs
                      << " old_var_size=" << oldCuddVarSize
                      << " new_var_size=" << newCuddVarSize
                      << " fact_vars=" << factVars
                      << " edge_vars=" << edgeVars
                      << " fact_create_count=" << factCreateCount
                      << " edge_create_count=" << edgeCreateCount
                      << " fact_create_max_ms=" << factCreateMaxMs
                      << " edge_create_max_ms=" << edgeCreateMaxMs
                      << " create_slow_threshold_ms=" << slowCreateThresholdMs
                      << " fact_create_slow=" << factCreateSlow
                      << " edge_create_slow=" << edgeCreateSlow
                      << " fact_hit=" << factVarHits
                      << " fact_miss=" << factVarMisses
                      << " edge_hit=" << edgeVarHits
                      << " edge_miss=" << edgeVarMisses
                      << std::endl;
        }
    }

    // Basic BDD operations
    BddNodeRef createVar(int index) override;
    BddNodeRef createVar(int index, const Node& node) override;
    BddNodeRef createVar(int index, const Hyperedge& edge) override;

    BddNodeRef makeAnd(const BddNodeRef& a, const BddNodeRef& b) override;
    BddNodeRef makeAnd(const std::vector<BddNodeRef>& nodes) override;
    BddNodeRef makeOr(const BddNodeRef& a, const BddNodeRef& b) override;
    BddNodeRef makeOr(const std::vector<BddNodeRef>& nodes) override;
    BddNodeRef makeNot(const BddNodeRef& a) override;
    BddNodeRef makeCondition(const BddNodeRef& f,
        const std::vector<int>& trueIndexes, const std::vector<int>& falseIndexes) override;
    bool isSame(const BddNodeRef& a, const BddNodeRef& b) override;
    BddNodeRef getTrue() override {
        return BddNodeRef(manager, Cudd_ReadOne(manager.get()));
    }
    BddNodeRef getFalse() override {
        return BddNodeRef(manager, Cudd_ReadLogicZero(manager.get()));
    }

    std::string toString(const BddNodeRef& nodeRef) override;

    // Weight-related operations
    void setVariableWeight(int varIndex, double posWeight, double negWeight) override;
    FormulaManager<BddNodeRef>::VariableWeight getVariableWeight(int varIndex) const override;
    bool hasVariableWeight(int varIndex) const override;
    double computeWeightedModelCount(const BddNodeRef& node) override;

    // Utility functions
    void printInfo(const BddNodeRef& node, const std::string& name) override;
    DdManager* getManager() const { return manager.get(); }
    void dumpProfilingStatistics() override {};
    std::map<std::string, std::string> getProfilingStatistics() override {
        std::map<std::string, std::string> stats;
        const auto live_nodes = static_cast<std::size_t>(Cudd_ReadNodeCount(manager.get()));
        const auto dead_nodes = static_cast<std::size_t>(Cudd_ReadDead(manager.get()));
        stats["live_nodes"] = std::to_string(live_nodes);
        stats["dead_nodes"] = std::to_string(dead_nodes);
        stats["total_nodes"] = std::to_string(live_nodes + dead_nodes);
        stats["cudd_keys"] = std::to_string(Cudd_ReadKeys(manager.get()));
        stats["cudd_next_reordering"] = std::to_string(Cudd_ReadNextReordering(manager.get()));
        stats["cudd_reorderings"] = std::to_string(Cudd_ReadReorderings(manager.get()));
        stats["cudd_swap_steps"] = std::to_string(Cudd_ReadSwapSteps(manager.get()));
        stats["cudd_dead_counted"] = Cudd_DeadAreCounted(manager.get()) ? "1" : "0";
        Cudd_ReorderingType method = CUDD_REORDER_NONE;
        stats["cudd_autodyn_enabled"] = Cudd_ReorderingStatus(manager.get(), &method) ? "1" : "0";
        stats["cudd_autodyn_method"] = std::to_string(static_cast<int>(method));
        stats["memory_usage_mb"] = std::to_string(Cudd_ReadMemoryInUse(manager.get()) / (1024.0 * 1024));
        stats["cache_hits"] = std::to_string(Cudd_ReadCacheHits(manager.get()));
        stats["cache_lookups"] = std::to_string(Cudd_ReadCacheLookUps(manager.get()));
        stats["cache_hit_rate"] = std::to_string(getCacheHitRate(manager.get()) * 100.0) + "%";

        long current_reordering_time = Cudd_ReadReorderingTime(manager.get());
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", (current_reordering_time - last_reordering_time_) / 1000.0);
        stats["reordering_runtime"] = std::string(buf);
        last_reordering_time_ = current_reordering_time;
        return stats;
    }
    std::size_t getLiveNodeCount() const override {
        return static_cast<std::size_t>(Cudd_ReadNodeCount(manager.get()));
    }
    std::size_t getDeadNodeCount() const override {
        return static_cast<std::size_t>(Cudd_ReadDead(manager.get()));
    }
    std::size_t getTotalNodeCount() const override {
        return getLiveNodeCount() + getDeadNodeCount();
    }
    std::uintptr_t getReorderManagerId() const override {
        return reinterpret_cast<std::uintptr_t>(manager.get());
    }
    double getReorderingTimeSeconds() const {
        return static_cast<double>(Cudd_ReadReorderingTime(manager.get())) / 1000.0;
    }
    FormulaReorderStats configureIncrementalAutoReorder(
            std::size_t gap, bool countDead, bool allowLarge) override {
        FormulaReorderStats stats = readReorderStats_();
        stats.supported = true;
        stats.attempted = gap > 0;
        if (!stats.attempted) {
            stats.success = true;
            return stats;
        }

        const auto t0 = Clock::now();
        if (countDead) {
            Cudd_TurnOnCountDead(manager.get());
        } else {
            Cudd_TurnOffCountDead(manager.get());
        }

        Cudd_ReorderingType method = CUDD_REORDER_SAME;
        int enabled = Cudd_ReorderingStatus(manager.get(), &method);
        if (stats.liveBefore >= 3000000 && !allowLarge) {
            adaptiveReorder(manager.get());
        } else if (!enabled || (allowLarge && stats.liveBefore >= 3000000)) {
            Cudd_AutodynEnable(manager.get(), CUDD_REORDER_WINDOW2);
            currentReorderingType = CUDD_REORDER_WINDOW2;
        } else if (!allowLarge) {
            adaptiveReorder(manager.get());
        }

        const std::size_t currentTriggerCount =
                countDead ? static_cast<std::size_t>(Cudd_ReadKeys(manager.get()))
                          : static_cast<std::size_t>(Cudd_ReadKeys(manager.get()) - Cudd_ReadDead(manager.get()));
        const std::size_t target = currentTriggerCount > std::numeric_limits<unsigned int>::max() - gap
                                           ? std::numeric_limits<unsigned int>::max()
                                           : currentTriggerCount + gap;
        Cudd_SetNextReordering(manager.get(), static_cast<unsigned int>(target));

        FormulaReorderStats after = readReorderStats_();
        after.supported = true;
        after.attempted = true;
        after.success = true;
        after.triggered = after.reorderingsAfter > stats.reorderingsBefore;
        after.liveBefore = stats.liveBefore;
        after.keysBefore = stats.keysBefore;
        after.deadBefore = stats.deadBefore;
        after.nextBefore = stats.nextBefore;
        after.reorderingsBefore = stats.reorderingsBefore;
        after.swapsBefore = stats.swapsBefore;
        after.reorderingTimeBeforeSec = stats.reorderingTimeBeforeSec;
        after.autoEnabledBefore = stats.autoEnabledBefore;
        after.deadCountedBefore = stats.deadCountedBefore;
        after.methodBefore = stats.methodBefore;
        after.elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        return after;
    }
    FormulaReorderStats disableIncrementalAutoReorder() override {
        FormulaReorderStats stats = readReorderStats_();
        stats.supported = true;
        stats.attempted = true;
        const auto t0 = Clock::now();
        Cudd_AutodynDisable(manager.get());
        FormulaReorderStats after = readReorderStats_();
        after.supported = true;
        after.attempted = true;
        after.success = true;
        after.triggered = after.reorderingsAfter > stats.reorderingsBefore;
        after.liveBefore = stats.liveBefore;
        after.keysBefore = stats.keysBefore;
        after.deadBefore = stats.deadBefore;
        after.nextBefore = stats.nextBefore;
        after.reorderingsBefore = stats.reorderingsBefore;
        after.swapsBefore = stats.swapsBefore;
        after.reorderingTimeBeforeSec = stats.reorderingTimeBeforeSec;
        after.autoEnabledBefore = stats.autoEnabledBefore;
        after.deadCountedBefore = stats.deadCountedBefore;
        after.methodBefore = stats.methodBefore;
        after.elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        return after;
    }
    FormulaReorderStats explicitIncrementalReorder() override {
        FormulaReorderStats stats = readReorderStats_();
        stats.supported = true;
        stats.attempted = true;
        const auto t0 = Clock::now();
        int ok = Cudd_ReduceHeap(manager.get(), CUDD_REORDER_WINDOW2, 10);
        FormulaReorderStats after = readReorderStats_();
        after.supported = true;
        after.attempted = true;
        after.success = ok != 0;
        after.triggered = after.reorderingsAfter > stats.reorderingsBefore;
        after.liveBefore = stats.liveBefore;
        after.keysBefore = stats.keysBefore;
        after.deadBefore = stats.deadBefore;
        after.nextBefore = stats.nextBefore;
        after.reorderingsBefore = stats.reorderingsBefore;
        after.swapsBefore = stats.swapsBefore;
        after.reorderingTimeBeforeSec = stats.reorderingTimeBeforeSec;
        after.autoEnabledBefore = stats.autoEnabledBefore;
        after.deadCountedBefore = stats.deadCountedBefore;
        after.methodBefore = stats.methodBefore;
        after.elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        return after;
    }


private:
    FormulaReorderStats readReorderStats_() const {
        FormulaReorderStats stats;
        stats.supported = true;
        Cudd_ReorderingType method = CUDD_REORDER_NONE;
        stats.autoEnabledAfter = Cudd_ReorderingStatus(manager.get(), &method) != 0;
        stats.methodAfter = static_cast<int>(method);
        stats.deadCountedAfter = Cudd_DeadAreCounted(manager.get()) != 0;
        stats.liveAfter = static_cast<std::size_t>(Cudd_ReadNodeCount(manager.get()));
        stats.keysAfter = static_cast<std::size_t>(Cudd_ReadKeys(manager.get()));
        stats.deadAfter = static_cast<std::size_t>(Cudd_ReadDead(manager.get()));
        stats.nextAfter = static_cast<std::size_t>(Cudd_ReadNextReordering(manager.get()));
        stats.reorderingsAfter = static_cast<std::size_t>(Cudd_ReadReorderings(manager.get()));
        stats.swapsAfter = static_cast<std::size_t>(Cudd_ReadSwapSteps(manager.get()));
        stats.reorderingTimeAfterSec =
                static_cast<double>(Cudd_ReadReorderingTime(manager.get())) / 1000.0;
        stats.autoEnabledBefore = stats.autoEnabledAfter;
        stats.methodBefore = stats.methodAfter;
        stats.deadCountedBefore = stats.deadCountedAfter;
        stats.liveBefore = stats.liveAfter;
        stats.keysBefore = stats.keysAfter;
        stats.deadBefore = stats.deadAfter;
        stats.nextBefore = stats.nextAfter;
        stats.reorderingsBefore = stats.reorderingsAfter;
        stats.swapsBefore = stats.swapsAfter;
        stats.reorderingTimeBeforeSec = stats.reorderingTimeAfterSec;
        return stats;
    }
    BddNodeRef makeAndBalanced(const std::vector<BddNodeRef>& nodes, size_t begin, size_t end);
    BddNodeRef makeOrBalanced(const std::vector<BddNodeRef>& nodes, size_t begin, size_t end);
    BddNodeRef makeAndSequential(const std::vector<BddNodeRef>& nodes);
    BddNodeRef makeOrSequential(const std::vector<BddNodeRef>& nodes);
    double recursiveWeightedModelCount(DdNode* node,
                                     std::unordered_map<DdNode*, double>& cache);
    std::shared_ptr<DdManager> initManager();

    std::shared_ptr<DdManager> manager;
    std::unordered_map<int, VariableWeight> weights;
    std::uint64_t weightsEpoch_ = 0;

    std::string toStringRecursive(DdNode* node,
                            std::unordered_map<DdNode*, std::string>& cache);
    std::string getVariableName(int varIndex);
    std::unordered_map<int, BddNodeRef> variableRegistry;
    std::unordered_map<DdNode*, double> wmcCache_;
    std::uint64_t wmcCacheEpoch_ = 0;
    long last_reordering_time_ = 0;
    bool reorderConfigured_ = false;
    InitConfig initConfig_;


};

inline double getCacheHitRate(DdManager* manager) {
    double hits = static_cast<double>(Cudd_ReadCacheHits(manager));
    double lookups = static_cast<double>(Cudd_ReadCacheLookUps(manager));
    if (lookups == 0.0) return 0.0;
    return hits / lookups;
}




inline void adaptiveReorder(DdManager* manager) {
    size_t node_count = Cudd_ReadNodeCount(manager);
    Cudd_ReorderingType next = CUDD_REORDER_NONE;

    if (node_count < 10000) {next = CUDD_REORDER_SIFT_CONVERGE; }
    else if (node_count < 50000) {next = CUDD_REORDER_SIFT; }
    else if (node_count < 100000) {next = CUDD_REORDER_WINDOW4_CONV; }
    else if (node_count < 300000) {next = CUDD_REORDER_WINDOW4;}
    else if (node_count < 1000000) {next = CUDD_REORDER_WINDOW2;}
    else if (node_count < 3000000) {next = CUDD_REORDER_WINDOW2;}
    else {next = CUDD_REORDER_NONE; Cudd_AutodynDisable(manager);}

    if (next != currentReorderingType) {
        Cudd_AutodynEnable(manager, next);
        currentReorderingType = next;
        if (kCuddVerbose) {
            std::cout << "Switched reordering to " << next << " at node count " << node_count << std::endl;
        }
    }
}

inline int myGCFunc(DdManager* dd, const char* str, void* data) {
    if (kCuddVerbose) {
        std::cout << "[GC] current error code = " << Cudd_ReadErrorCode(dd) << "\n";
        fprintf(stdout, "[GC] Dead = %u, Keys = %u, Mem = %zu\n",
                Cudd_ReadDead(dd), Cudd_ReadKeys(dd), Cudd_ReadMemoryInUse(dd));
        fprintf(stdout, "[GC] Recursive calls = %.2f, Cache Used = %.2f, Cache hits = %.0f, lookups = %.0f, hit rate = %.2f%%\n",
                Cudd_ReadRecursiveCalls(dd), Cudd_ReadUsedSlots(dd), Cudd_ReadCacheHits(dd), Cudd_ReadCacheLookUps(dd),
                getCacheHitRate(dd) * 100.0);
    }
    if (gc_begin) {
        if (kCuddVerbose) fprintf(stdout, "[GC] Starting GC: %d\n", ++_cudd_gc_count);
        _cudd_gc_start_time = Clock::now();
        gc_begin = false;
    } else {
        _cudd_gc_end_time = Clock::now();
        Duration duration = _cudd_gc_end_time - _cudd_gc_start_time;
        if (kCuddVerbose) {
            std::cout << "[GC] Time taken: " << duration.count() << " seconds" << std::endl;
        }
        gc_begin = true;
    }
    return 1;
}


inline int myVRFunc(DdManager* dd, const char* str, void* data) {
    if (kCuddVerbose) std::cout << "[VR] current error code = " << Cudd_ReadErrorCode(dd) << "\n";
    if (reordering_begin) {
        _cudd_reordering_start_time = Clock::now();
        reordering_begin = false;
        if (kCuddVerbose) std::cout << "[VR] Starting reordering " << ++_cudd_reordering_count << "..." << std::endl;
    } else {
        _cudd_reordering_end_time = Clock::now();
        Duration duration = _cudd_reordering_end_time - _cudd_reordering_start_time;
        if (kCuddVerbose) std::cout << "[VR] Time taken: " << duration.count() << " seconds" << std::endl;
        reordering_begin = true;
        // set next reordering threshold
        adaptiveReorder(dd);
    }
    return 1;
}
// Implementation
inline WeightedBDDManager::WeightedBDDManager(InitConfig config) : initConfig_(config) {
    manager = initManager();
}

inline std::shared_ptr<DdManager> WeightedBDDManager::initManager() {
    resetReorderingState();
    // Preallocate ~1000 BDD vars to reduce ithVar expansions.
    DdManager* m = Cudd_Init(initConfig_.numVars, initConfig_.numVarsZ, initConfig_.numSlots,
            initConfig_.cacheSize, initConfig_.maxMemory);

    if (m == nullptr) {
        throw std::runtime_error("Failed to initialize CUDD manager");
    }
    Cudd_AddHook(m, myGCFunc, CUDD_PRE_GC_HOOK);
    Cudd_AddHook(m, myGCFunc, CUDD_POST_GC_HOOK);
    Cudd_AddHook(m, myVRFunc, CUDD_PRE_REORDERING_HOOK);
    Cudd_AddHook(m, myVRFunc, CUDD_POST_REORDERING_HOOK);
    return std::shared_ptr<DdManager>(m, [](DdManager* m) {
        if (m) Cudd_Quit(m);
    });
}

struct CuddCreateVarStats {
    unsigned int gc = 0;
    unsigned int reorder = 0;
    unsigned int swaps = 0;
    size_t node = 0;
    unsigned int dead = 0;
    unsigned int slots = 0;
    unsigned int used_slots = 0;
    unsigned int keys = 0;
};

static inline CuddCreateVarStats readCuddCreateVarStats(DdManager* dd) {
    CuddCreateVarStats stats;
    stats.gc = Cudd_ReadGarbageCollections(dd);
    stats.reorder = Cudd_ReadReorderings(dd);
    stats.swaps = Cudd_ReadSwapSteps(dd);
    stats.node = Cudd_ReadNodeCount(dd);
    stats.dead = Cudd_ReadDead(dd);
    stats.slots = Cudd_ReadSlots(dd);
    stats.used_slots = Cudd_ReadUsedSlots(dd);
    stats.keys = Cudd_ReadKeys(dd);
    return stats;
}

static inline void emitCuddCreateVarStats(const std::string& tag, const char* kind, int index,
        const CuddCreateVarStats& before, const CuddCreateVarStats& after) {
    std::cout << "[fc-profile] stage=CUDD_CREATEVAR_STATS tag=" << tag
              << " kind=" << kind
              << " index=" << index
              << " gc_before=" << before.gc << " gc_after=" << after.gc
              << " gc_delta=" << static_cast<long long>(after.gc) - static_cast<long long>(before.gc)
              << " reorder_before=" << before.reorder << " reorder_after=" << after.reorder
              << " reorder_delta=" << static_cast<long long>(after.reorder) - static_cast<long long>(before.reorder)
              << " swap_before=" << before.swaps << " swap_after=" << after.swaps
              << " swap_delta=" << static_cast<long long>(after.swaps) - static_cast<long long>(before.swaps)
              << " node_before=" << before.node << " node_after=" << after.node
              << " node_delta=" << static_cast<long long>(after.node) - static_cast<long long>(before.node)
              << " dead_before=" << before.dead << " dead_after=" << after.dead
              << " dead_delta=" << static_cast<long long>(after.dead) - static_cast<long long>(before.dead)
              << " slots_before=" << before.slots << " slots_after=" << after.slots
              << " slots_delta=" << static_cast<long long>(after.slots) - static_cast<long long>(before.slots)
              << " used_slots_before=" << before.used_slots << " used_slots_after=" << after.used_slots
              << " used_slots_delta=" << static_cast<long long>(after.used_slots) - static_cast<long long>(before.used_slots)
              << " keys_before=" << before.keys << " keys_after=" << after.keys
              << " keys_delta=" << static_cast<long long>(after.keys) - static_cast<long long>(before.keys)
              << std::endl;
}

inline BddNodeRef WeightedBDDManager::createVar(int index) {
    const bool stats_enabled = fcProfileEnabled && !getCuddPreConfigTag().empty();
    if (!stats_enabled) {
        if (variableRegistry.find(index) != variableRegistry.end()) {
            return variableRegistry[index];
        }
        DdNode* var = Cudd_bddIthVar(manager.get(), index);
        BddNodeRef ref(manager, var);
        variableRegistry[index] = ref;
        return ref;
    }

    using Clock = std::chrono::steady_clock;
    auto toMs = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    auto totalStart = Clock::now();
    auto lookupStart = Clock::now();
    auto it = variableRegistry.find(index);
    double lookupMs = toMs(Clock::now() - lookupStart);
    if (it != variableRegistry.end()) {
        auto copyStart = Clock::now();
        BddNodeRef ref = it->second;
        double copyMs = toMs(Clock::now() - copyStart);
        double totalMs = toMs(Clock::now() - totalStart);
        std::cout << "[fc-profile] stage=CUDD_CREATEVAR tag=" << getCuddPreConfigTag()
                  << " kind=index index=" << index
                  << " hit=1"
                  << " lookup_ms=" << lookupMs
                  << " copy_ms=" << copyMs
                  << " total_ms=" << totalMs
                  << std::endl;
        return ref;
    }
    auto statsStart = Clock::now();
    CuddCreateVarStats stats_before = readCuddCreateVarStats(manager.get());
    double statsBeforeMs = toMs(Clock::now() - statsStart);
    auto start = Clock::now();
    DdNode* var = Cudd_bddIthVar(manager.get(), index);
    double ithMs = toMs(Clock::now() - start);
    statsStart = Clock::now();
    CuddCreateVarStats stats_after = readCuddCreateVarStats(manager.get());
    double statsAfterMs = toMs(Clock::now() - statsStart);
    start = Clock::now();
    BddNodeRef ref(manager, var);
    double wrapMs = toMs(Clock::now() - start);
    auto insertStart = Clock::now();
    variableRegistry[index] = ref;
    double insertMs = toMs(Clock::now() - insertStart);
    double totalMs = toMs(Clock::now() - totalStart);
    double statsMs = statsBeforeMs + statsAfterMs;
    std::cout << "[fc-profile] stage=CUDD_CREATEVAR tag=" << getCuddPreConfigTag()
              << " kind=index index=" << index
              << " hit=0"
              << " lookup_ms=" << lookupMs
              << " stats_before_ms=" << statsBeforeMs
              << " stats_after_ms=" << statsAfterMs
              << " stats_ms=" << statsMs
              << " ith_ms=" << ithMs
              << " wrap_ms=" << wrapMs
              << " insert_ms=" << insertMs
              << " total_ms=" << totalMs
              << std::endl;
    emitCuddCreateVarStats(getCuddPreConfigTag(), "index", index, stats_before, stats_after);
    return ref;
//    BddNodeRef(manager, var);
}

inline BddNodeRef WeightedBDDManager::createVar(int index, const Node& node) {
    const bool stats_enabled = fcProfileEnabled && !getCuddPreConfigTag().empty();
    if (!stats_enabled) {
        if (variableRegistry.find(index) != variableRegistry.end()) {
            return variableRegistry[index];
        }
        DdNode* var = Cudd_bddIthVar(manager.get(), index);
        if (var == nullptr) {
            throw std::runtime_error("Failed to create BDD variable");
        }
        BddNodeRef ref(manager, var, node);
        variableRegistry[index] = ref;
        return ref;
    }

    using Clock = std::chrono::steady_clock;
    auto toMs = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    auto totalStart = Clock::now();
    auto lookupStart = Clock::now();
    auto it = variableRegistry.find(index);
    double lookupMs = toMs(Clock::now() - lookupStart);
    if (it != variableRegistry.end()) {
        auto copyStart = Clock::now();
        BddNodeRef ref = it->second;
        double copyMs = toMs(Clock::now() - copyStart);
        double totalMs = toMs(Clock::now() - totalStart);
        std::cout << "[fc-profile] stage=CUDD_CREATEVAR tag=" << getCuddPreConfigTag()
                  << " kind=fact index=" << index
                  << " hit=1"
                  << " lookup_ms=" << lookupMs
                  << " copy_ms=" << copyMs
                  << " total_ms=" << totalMs
                  << std::endl;
        return ref;
    }
    auto statsStart = Clock::now();
    CuddCreateVarStats stats_before = readCuddCreateVarStats(manager.get());
    double statsBeforeMs = toMs(Clock::now() - statsStart);
    auto start = Clock::now();
    DdNode* var = Cudd_bddIthVar(manager.get(), index);
    double ithMs = toMs(Clock::now() - start);
    statsStart = Clock::now();
    CuddCreateVarStats stats_after = readCuddCreateVarStats(manager.get());
    double statsAfterMs = toMs(Clock::now() - statsStart);
    if (var == nullptr) {
        throw std::runtime_error("Failed to create BDD variable");
    }
    start = Clock::now();
    BddNodeRef ref(manager, var, node);
    double wrapMs = toMs(Clock::now() - start);
    auto insertStart = Clock::now();
    variableRegistry[index] = ref;
    double insertMs = toMs(Clock::now() - insertStart);
    double totalMs = toMs(Clock::now() - totalStart);
    double statsMs = statsBeforeMs + statsAfterMs;
    std::cout << "[fc-profile] stage=CUDD_CREATEVAR tag=" << getCuddPreConfigTag()
              << " kind=fact index=" << index
              << " hit=0"
              << " lookup_ms=" << lookupMs
              << " stats_before_ms=" << statsBeforeMs
              << " stats_after_ms=" << statsAfterMs
              << " stats_ms=" << statsMs
              << " ith_ms=" << ithMs
              << " wrap_ms=" << wrapMs
              << " insert_ms=" << insertMs
              << " total_ms=" << totalMs
              << std::endl;
    emitCuddCreateVarStats(getCuddPreConfigTag(), "fact", index, stats_before, stats_after);
    return ref;
}

inline BddNodeRef WeightedBDDManager::createVar(int index, const Hyperedge& edge) {
    const bool stats_enabled = fcProfileEnabled && !getCuddPreConfigTag().empty();
    if (!stats_enabled) {
        if (variableRegistry.find(index) != variableRegistry.end()) {
            return variableRegistry[index];
        }
        DdNode* var = Cudd_bddIthVar(manager.get(), index);
        if (var == nullptr) {
            throw std::runtime_error("Failed to create BDD variable");
        }
        BddNodeRef ref(manager, var, edge);
        variableRegistry[index] = ref;
        return ref;
    }

    using Clock = std::chrono::steady_clock;
    auto toMs = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    auto totalStart = Clock::now();
    auto lookupStart = Clock::now();
    auto it = variableRegistry.find(index);
    double lookupMs = toMs(Clock::now() - lookupStart);
    if (it != variableRegistry.end()) {
        auto copyStart = Clock::now();
        BddNodeRef ref = it->second;
        double copyMs = toMs(Clock::now() - copyStart);
        double totalMs = toMs(Clock::now() - totalStart);
        std::cout << "[fc-profile] stage=CUDD_CREATEVAR tag=" << getCuddPreConfigTag()
                  << " kind=edge index=" << index
                  << " hit=1"
                  << " lookup_ms=" << lookupMs
                  << " copy_ms=" << copyMs
                  << " total_ms=" << totalMs
                  << std::endl;
        return ref;
    }
    auto statsStart = Clock::now();
    CuddCreateVarStats stats_before = readCuddCreateVarStats(manager.get());
    double statsBeforeMs = toMs(Clock::now() - statsStart);
    auto start = Clock::now();
    DdNode* var = Cudd_bddIthVar(manager.get(), index);
    double ithMs = toMs(Clock::now() - start);
    statsStart = Clock::now();
    CuddCreateVarStats stats_after = readCuddCreateVarStats(manager.get());
    double statsAfterMs = toMs(Clock::now() - statsStart);
    if (var == nullptr) {
        throw std::runtime_error("Failed to create BDD variable");
    }
    start = Clock::now();
    BddNodeRef ref(manager, var, edge);
    double wrapMs = toMs(Clock::now() - start);
    auto insertStart = Clock::now();
    variableRegistry[index] = ref;
    double insertMs = toMs(Clock::now() - insertStart);
    double totalMs = toMs(Clock::now() - totalStart);
    double statsMs = statsBeforeMs + statsAfterMs;
    std::cout << "[fc-profile] stage=CUDD_CREATEVAR tag=" << getCuddPreConfigTag()
              << " kind=edge index=" << index
              << " hit=0"
              << " lookup_ms=" << lookupMs
              << " stats_before_ms=" << statsBeforeMs
              << " stats_after_ms=" << statsAfterMs
              << " stats_ms=" << statsMs
              << " ith_ms=" << ithMs
              << " wrap_ms=" << wrapMs
              << " insert_ms=" << insertMs
              << " total_ms=" << totalMs
              << std::endl;
    emitCuddCreateVarStats(getCuddPreConfigTag(), "edge", index, stats_before, stats_after);
//    return BddNodeRef(manager, var, edge);
    return ref;
}

inline BddNodeRef WeightedBDDManager::makeAnd(const BddNodeRef& a, const BddNodeRef& b) {
    DdNode* result = Cudd_bddAnd(manager.get(), a.get(), b.get());
    if (result == nullptr) {
        std::cout << (Cudd_ReadErrorCode(manager.get())) << std::endl;
//        throw std::runtime_error("makeAnd failed");
        assert(false);
    }
    return BddNodeRef(manager, result);
}

inline BddNodeRef WeightedBDDManager::makeAnd(const std::vector<BddNodeRef>& nodes) {
//    return makeAndSequential(nodes);
    return makeAndBalanced(nodes, 0, nodes.size());
}

inline BddNodeRef WeightedBDDManager::makeAndSequential(const std::vector<BddNodeRef>& nodes) {
    if (nodes.empty()) {
        return BddNodeRef(manager, Cudd_ReadOne(manager.get()));
    }
    for (const auto& node : nodes) {
        if (node.get() == nullptr) {
            assert (false && "makeAnd received a null node");
        }
    }
    BddNodeRef result(manager, nodes[0].get());
    for (size_t i = 1; i < nodes.size(); i++) {
        assert (nodes[i].get() != nullptr && "makeAnd received a null node");
        assert (result.get() != nullptr && "makeAnd received a null result node");
        assert (manager.get() != nullptr && "makeAnd received a null manager");
        auto node = Cudd_bddAnd(manager.get(), result.get(), nodes[i].get());
        if (node == nullptr) {
                assert (node != nullptr && "Cudd_bddAnd failed");
        }
        result = BddNodeRef(manager, node);
    }
    return result;
}

inline BddNodeRef WeightedBDDManager::makeAndBalanced(const std::vector<BddNodeRef>& nodes, size_t begin, size_t end) {
    if (begin >= end) {
        return BddNodeRef(manager, Cudd_ReadOne(manager.get()));
    }
    if (end - begin == 1) {
        return nodes[begin];
    }

    size_t mid = begin + (end - begin) / 2;
    BddNodeRef left = makeAndBalanced(nodes, begin, mid);
    BddNodeRef right = makeAndBalanced(nodes, mid, end);

    if (left.get() == nullptr || right.get() == nullptr || manager.get() == nullptr) {
        throw std::runtime_error("makeAndBalanced received null input or manager");
    }

    DdNode* and_node = Cudd_bddAnd(manager.get(), left.get(), right.get());
    if (and_node == nullptr) {
        throw std::runtime_error("Cudd_bddAnd failed in makeAndBalanced");
    }
    return BddNodeRef(manager, and_node);
}

inline BddNodeRef WeightedBDDManager::makeOr(const BddNodeRef& a, const BddNodeRef& b) {
    DdNode* result = Cudd_bddOr(manager.get(), a.get(), b.get());
    if (result == nullptr) {
        throw std::runtime_error("makeOr failed");
    }
    return BddNodeRef(manager, result);
}

inline BddNodeRef WeightedBDDManager::makeOr(const std::vector<BddNodeRef>& nodes) {
//    return makeOrSequential(nodes);
    return makeOrBalanced(nodes, 0, nodes.size());
}

inline BddNodeRef WeightedBDDManager::makeOrSequential(const std::vector<BddNodeRef>& nodes) {
    if (nodes.empty()) {
        return BddNodeRef(manager, Cudd_ReadZero(manager.get()));
    }
    BddNodeRef result(manager, nodes[0].get());
    for (size_t i = 1; i < nodes.size(); i++) {
        result = BddNodeRef(manager, Cudd_bddOr(manager.get(), result.get(), nodes[i].get()));
        if (result.get() == nullptr) {
            throw std::runtime_error("makeOr failed");
        }
    }
    return result;
}

inline BddNodeRef WeightedBDDManager::makeOrBalanced(const std::vector<BddNodeRef>& nodes, size_t begin, size_t end) {
    if (begin >= end) {
        return BddNodeRef(manager, Cudd_ReadZero(manager.get()));
    }
    else if (end - begin == 1) {
        return nodes[begin];
    }
    size_t mid = begin + (end - begin) / 2;
    BddNodeRef left = makeOrBalanced(nodes, begin, mid);
    BddNodeRef right = makeOrBalanced(nodes, mid, end);
//    assert (left.get() != nullptr && "makeOrBalanced received null left node");
//    assert (right.get() != nullptr && "makeOrBalanced received null right node");
    unsigned int r_before = Cudd_ReadReorderings(manager.get());
    DdNode* or_node = Cudd_bddOr(manager.get(), left.get(), right.get());
    unsigned int r_after  = Cudd_ReadReorderings(manager.get());
    if (or_node == nullptr) {
        Cudd_ErrorType ec = Cudd_ReadErrorCode(manager.get());
        dumpProfilingStatistics();
        fprintf(stderr, "[CUDD] bddOr returned NULL. err=%d, reorders:%u->%u, "
                            "timeLimited=%d, nodeCount=%ld, maxMem=%zu\n",
                    (int)ec, r_before, r_after, Cudd_TimeLimited(manager.get()),
                    Cudd_ReadNodeCount(manager.get()), Cudd_ReadMaxMemory(manager.get()));
        assert (false && "makeOrBalanced failed");
    }
    return BddNodeRef(manager, or_node);
}

inline BddNodeRef WeightedBDDManager::makeNot(const BddNodeRef& a) {
    DdNode* result = Cudd_Not(a.get());
    return BddNodeRef(manager, result);
}

inline BddNodeRef WeightedBDDManager::makeCondition(const BddNodeRef& f,
        const std::vector<int>& trueIndexes, const std::vector<int>& falseIndexes) {
    BddNodeRef cube = getTrue();
    for (int index : trueIndexes) {
        BddNodeRef x = createVar(index);  // find
        cube = makeAnd(cube, x);
    }
    for (int index : falseIndexes) {
        BddNodeRef x = createVar(index);  // find
        cube = makeAnd(cube, makeNot(x));
    }
    return BddNodeRef(manager, Cudd_bddRestrict(manager.get(), f.get(), cube.get()));
}

bool WeightedBDDManager::isSame(const BddNodeRef& a, const BddNodeRef& b) {
    return a.get() == b.get();
}

inline void WeightedBDDManager::setVariableWeight(int varIndex, double posWeight, double negWeight) {
    auto it = weights.find(varIndex);
    if (it != weights.end() && it->second.posWeight == posWeight && it->second.negWeight == negWeight) {
        return;
    }
    weights[varIndex] = VariableWeight{posWeight, negWeight};
    ++weightsEpoch_;
}

FormulaManager<BddNodeRef>::VariableWeight WeightedBDDManager::getVariableWeight(int varIndex) const {
    auto it = weights.find(varIndex);
    if (it == weights.end()) {
        return FormulaManager<BddNodeRef>::VariableWeight{1.0, 0.0};
    }
    return FormulaManager<BddNodeRef>::VariableWeight{it->second.posWeight, it->second.negWeight};
}

bool WeightedBDDManager::hasVariableWeight(int varIndex) const {
    return weights.find(varIndex) != weights.end();
}

inline double WeightedBDDManager::computeWeightedModelCount(const BddNodeRef& node) {
//    std::cout << "wmc..." << std::endl;
//    static int count = 0;
//    if (count++ == 0) {
//        for (auto& [index, weight] : weights) {
//            std::cout << "Variable " << index << ": posWeight = " << weight.posWeight
//                      << ", negWeight = " << weight.negWeight << std::endl;
//        }
//    }
    if (wmcCacheEpoch_ != weightsEpoch_) {
        wmcCache_.clear();
        wmcCacheEpoch_ = weightsEpoch_;
    }
    return recursiveWeightedModelCount(node.get(), wmcCache_);
}

inline double WeightedBDDManager::recursiveWeightedModelCount(
    DdNode* node,
    std::unordered_map<DdNode*, double>& cache) {
//    std::cout << "wmc..." << std::endl;
    // Check if node is constant
    if (Cudd_IsConstant(node)) {
        return Cudd_IsComplement(node) ? 0.0 : 1.0;
    }

    // Check cache
    auto it = cache.find(node);
    if (it != cache.end()) {
        return it->second;
    }

    // Get node's variable index
    int varIndex = Cudd_NodeReadIndex(node);
    // Get then and else cofactors
    DdNode* T = Cudd_T(node);
    DdNode* E = Cudd_E(node);

    // If node is complemented, adjust cofactors
    if (Cudd_IsComplement(node)) {
        T = Cudd_Not(T);
        E = Cudd_Not(E);
    }

    // Get weights for this variable
    auto weightIt = weights.find(varIndex);
    double posWeight = weightIt != weights.end() ? weightIt->second.posWeight : 1.0;
    double negWeight = weightIt != weights.end() ? weightIt->second.negWeight : 0.0;

    // Recursive computation
    double tWeight = recursiveWeightedModelCount(T, cache);
    double eWeight = recursiveWeightedModelCount(E, cache);

    // Combine results
    double result = posWeight * tWeight + negWeight * eWeight;
    // Cache and return result
    cache[node] = result;
    return result;
}

inline std::string WeightedBDDManager::toString(const BddNodeRef& nodeRef) {
    if (nodeRef.get() == nullptr) {
        return "NULL";
    }

    static std::unordered_map<DdNode*, std::string> cache;
    return toStringRecursive(nodeRef.get(), cache);
}

inline std::string WeightedBDDManager::getVariableName(int varIndex) {
    auto it = variableRegistry.find(varIndex);
    if (it != variableRegistry.end()) {
        const BddNodeRef& ref = it->second;
        // Direct access to private members thanks to friendship
        if (ref.node.has_value()) {
            if (ref.node.value()->pruned) {
                return "pruned_" + std::to_string(varIndex) + "_n";
            }
            return ref.node.value()->toString() + "_" + std::to_string(varIndex);
        } else if (ref.edge.has_value()) {
            if (ref.edge.value()->pruned) {
                return "pruned_" + std::to_string(varIndex) + "_e";
            }
            return ref.edge.value()->toString() + "_" + std::to_string(varIndex);
        }
        std::cout << "Warning: Variable index " << varIndex << " has no associated Node or Edge." << std::endl;
        assert (false && "Variable has no associated Node or Edge");
    }
    // Fallback to default naming
    return "x" + std::to_string(varIndex);
}

inline std::string WeightedBDDManager::toStringRecursive(
    DdNode* node,
    std::unordered_map<DdNode*, std::string>& cache) {

    // Check cache first
    auto it = cache.find(node);
    if (it != cache.end()) {
        return it->second;
    }

    // Handle constant nodes
    if (Cudd_IsConstant(node)) {
        return Cudd_IsComplement(node) ? "0" : "1";
    }

    // Get the variable index for this node
    int index = Cudd_NodeReadIndex(Cudd_Regular(node));
    std::string varName = getVariableName(index);

    // Get then and else cofactors
    DdNode* tNode = Cudd_T(Cudd_Regular(node));
    DdNode* eNode = Cudd_E(Cudd_Regular(node));

    // Account for complement if needed
    if (Cudd_IsComplement(node)) {
        tNode = Cudd_Not(tNode);
        eNode = Cudd_Not(eNode);
    }

    // Recursively convert cofactors to formulas
    std::string tFormula = toStringRecursive(tNode, cache);
    std::string eFormula = toStringRecursive(eNode, cache);

    // Apply simplifications for more readable formulas
    std::string formula;

    if (tFormula == "1" && eFormula == "0") {
        formula = varName;
    }
    else if (tFormula == "0" && eFormula == "1") {
        formula = "¬" + varName;
    }
    else if (tFormula == eFormula) {
        formula = tFormula;
    }
    else if (tFormula == "1") {
        formula = varName + " ∨ (¬" + varName + " ∧ " + eFormula + ")";
    }
    else if (tFormula == "0") {
        formula = "¬" + varName + " ∧ " + eFormula;
    }
    else if (eFormula == "1") {
        formula = "¬" + varName + " ∨ (" + varName + " ∧ " + tFormula + ")";
    }
    else if (eFormula == "0") {
        formula = varName + " ∧ " + tFormula;
    }
    else {
        formula = "(" + varName + " ∧ " + tFormula + ") ∨ (¬" + varName + " ∧ " + eFormula + ")";
    }

    // Cache and return result
    cache[node] = formula;
    return formula;
}

inline void WeightedBDDManager::printInfo(const BddNodeRef& node, const std::string& name) {
    std::cout << "BDD Info for " << name << ":" << std::endl;
    std::cout << "Number of nodes: " << Cudd_DagSize(node.get()) << std::endl;
    std::cout << "Number of paths: " << Cudd_CountPath(node.get()) << std::endl;
    std::cout << "Weighted model count: " << computeWeightedModelCount(node) << std::endl;
}

#endif //CUDDMANAGER_H
