#ifndef DERIVATIONGRAPH_H
#define DERIVATIONGRAPH_H

#include <iostream>
#pragma once

#include "souffle/Derivation.h"
#include "souffle/CompiledOptions.h"
#include "souffle/RamTypes.h"
#include "souffle/SouffleInterface.h"
#include "souffle/problog/Rule.h"
#include "souffle/problog/RuleManager.h"
#include "souffle/problog/QueryManager.h"
#include <fstream>
#include <functional>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <tuple>
#include <filesystem>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <limits>
#include "souffle/utility/json11.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>

// Forward declarations
int nextFormulaNodeId = 0;
std::unordered_map<size_t, int> nodeIdMap;
std::unordered_map<int, size_t> idNodeMap;
std::unordered_map<size_t, int> edgeIdMap;
std::unordered_map<int, size_t> idEdgeMap;

inline void resetFormulaIdMapping() {
    nextFormulaNodeId = 0;
    nodeIdMap.clear();
    idNodeMap.clear();
    edgeIdMap.clear();
    idEdgeMap.clear();
}

int mapNodeId(size_t id) {
    if (nodeIdMap.find(id) == nodeIdMap.end()) {
        nodeIdMap[id] = nextFormulaNodeId++;
        idNodeMap[nextFormulaNodeId - 1] = id;
    }
    return nodeIdMap[id];
}
int mapEdgeId(size_t id) {
    if (edgeIdMap.find(id) == edgeIdMap.end()) {
        edgeIdMap[id] = nextFormulaNodeId++;
        idEdgeMap[nextFormulaNodeId - 1] = id;
    }
    return edgeIdMap[id];
}

// example rule application sets
class Node;
class Hyperedge;
class DerivationGraph;
struct CycleDependencyGraph;

using NodePtr = std::shared_ptr<Node>;
using EdgePtr = std::shared_ptr<Hyperedge>;
using SupportToken = std::uint64_t;

// Optional collection during regular query/evidence backward traversal. The
// observer receives each surviving node, edge and body occurrence once; it does
// not change reachability or mutate the graph while its sources are collected.
struct BackwardTraversalObserver {
    virtual ~BackwardTraversalObserver() = default;
    virtual void node(const NodePtr&) = 0;
    virtual void beginEdge(const EdgePtr&) = 0;
    virtual void input(const NodePtr&, bool negative) = 0;
    virtual void endEdge() = 0;
};

inline constexpr SupportToken kEdgeSupportTokenMask = SupportToken{1} << 63;

inline SupportToken makeFactSupportToken(size_t semanticId) {
    return static_cast<SupportToken>(semanticId) & ~kEdgeSupportTokenMask;
}

inline SupportToken makeEdgeSupportToken(size_t edgeId) {
    return kEdgeSupportTokenMask | (static_cast<SupportToken>(edgeId) & ~kEdgeSupportTokenMask);
}

inline void sortUniqueSupportTokens(std::vector<SupportToken>& tokens) {
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
}

inline bool supportTokensIntersect(
        const std::vector<SupportToken>& lhs, const std::vector<SupportToken>& rhs) {
    size_t i = 0;
    size_t j = 0;
    while (i < lhs.size() && j < rhs.size()) {
        if (lhs[i] == rhs[j]) {
            return true;
        }
        if (lhs[i] < rhs[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return false;
}

inline std::vector<SupportToken> mergeSupportTokenLists(
        std::initializer_list<const std::vector<SupportToken>*> parts) {
    std::vector<SupportToken> merged;
    size_t total = 0;
    for (const auto* part : parts) {
        if (part) {
            total += part->size();
        }
    }
    merged.reserve(total);
    for (const auto* part : parts) {
        if (!part) {
            continue;
        }
        merged.insert(merged.end(), part->begin(), part->end());
    }
    sortUniqueSupportTokens(merged);
    return merged;
}

enum class DeltaMetadataPolicy {
    Retain,
    Drop,
};

inline bool shouldRetainDeltaMetadata(DeltaMetadataPolicy policy) {
    return policy == DeltaMetadataPolicy::Retain;
}

struct RamDomainVectorHash {
    std::size_t operator()(const std::vector<souffle::RamDomain>& values) const {
        std::size_t seed = 0;
        for (const auto& value : values) {
            hash_combine(seed, value);
        }
        return seed;
    }
};
class Node {
public:
    friend class DerivationGraph;
    friend class IncrementalDerivationGraph;
    friend class WorkingDerivationGraph;
    friend class Hyperedge;

    const UntypedTuple& getTuple() const { return tuple; }
    const std::vector<EdgePtr>& getIncomingEdges() const { return incomingEdges; }
    std::vector<EdgePtr>& getIncomingEdges() { return incomingEdges; }

    const std::vector<EdgePtr>& getOutgoingEdges() const { return outgoingEdges; }
    std::vector<EdgePtr>& getOutgoingEdges() { return outgoingEdges; }
    size_t getId() const { return id; }
    size_t getSemanticFactId() const { return semanticFactId; }
    void setSemanticFactId(size_t semanticId) {
        semanticFactId = semanticId;
        syncOriginalFactSupport();
    }
    bool isOriginalFactNode() const { return originalFact; }
    void setOriginalFact(bool value = true) {
        originalFact = value;
        syncOriginalFactSupport();
    }
    const std::vector<SupportToken>& getProbabilisticSupportTokens() const {
        return probabilisticSupportTokens;
    }
    void setProbabilisticSupportTokens(std::vector<SupportToken> tokens) {
        sortUniqueSupportTokens(tokens);
        probabilisticSupportTokens = std::move(tokens);
    }
    void clearProbabilisticSupportTokens() { probabilisticSupportTokens.clear(); }
    void setProbability(double prob) {
        if (prob < 0.0 || prob > 1.0) {
            std::cerr << "[DerivationGraph] Node probability out of range: " << prob
                      << " for tuple=" << tuple.toString() << " id=" << id << std::endl;
            assert(false && "Node probability out of [0,1]");
        }
        probability = prob;
        syncOriginalFactSupport();
    }
    double getProbability() const { return probability; }
    std::string toString() const {
        std::stringstream ss;
        ss << tuple.toString();
        if (has_evidence) {
            ss << "[E:" << (evidenceValue ? "true" : "false") << "]";
        }
        return ss.str();
    }
    void setQuery() {
        needOutput = true;
        isQuery = true;
    }
    bool isShadow = false;  // synthetic alias node (not part of original tuple space)
    bool isQueryNode() {
        return needOutput;
    }
    bool hasEvidence() const { return has_evidence; }
    bool getEvidenceValue() const { return evidenceValue; }
    void setEvidence(bool value) {
        evidenceValue = value;
        has_evidence = true;
    }

    bool isFact = false;
    bool originalFact = false;
    bool pruned = false;
    bool needOutput = false;
    bool isQuery = false;

private:
    explicit Node(const UntypedTuple& t, size_t nodeId, double prob = 1.0)
        : tuple(t), id(nodeId), probability(prob), semanticFactId(nodeId) {}

    UntypedTuple tuple;
    std::vector<EdgePtr> incomingEdges;
    std::vector<EdgePtr> outgoingEdges;
    size_t id;
    double probability;
    size_t semanticFactId;

    bool has_evidence = false;
    bool evidenceValue = false;
    std::vector<SupportToken> probabilisticSupportTokens;

    void addIncomingEdge(EdgePtr edge);
    void addOutgoingEdge(EdgePtr edge);
    void syncOriginalFactSupport() {
        if (!originalFact) {
            return;
        }
        if (probability > 0.0 && probability < 1.0) {
            probabilisticSupportTokens = {makeFactSupportToken(semanticFactId)};
        } else {
            probabilisticSupportTokens.clear();
        }
    }
};

using EdgeKey = std::tuple<
    unsigned long,                                     // Rule ID
    UntypedTuple,                             // Output Node's Tuple
    std::vector<std::pair<UntypedTuple, bool>>  // Vector of (Input Node Tuple, isNegated)
>;

class Hyperedge {
public:
    friend class DerivationGraph;
    friend class WorkingDerivationGraph;

    const std::vector<NodePtr>& getInputs() const { return inputs; }

    mutable std::optional<std::vector<NodePtr>> cachedSortedInputs;
    mutable std::optional<std::vector<bool>> cachedSortedBodyNegations;

    const std::vector<NodePtr>& getInputsStable() const {
        if (cachedSortedInputs.has_value()) {
            return *cachedSortedInputs;
        }
        std::vector<NodePtr> sortedInputs = inputs;
        std::sort(sortedInputs.begin(), sortedInputs.end(), [](const NodePtr& a, const NodePtr& b) {
            return a->getTuple() < b->getTuple();
        });
        cachedSortedInputs.emplace(sortedInputs.begin(), sortedInputs.end());
        return *cachedSortedInputs;
    }
    NodePtr getOutput() const { return output; }
    const NodePtr& getOutputRef() const { return output; }
    size_t getId() const { return id; }
    const Rule* getRule() const { return rule; }
    const std::vector<bool>& getBodyNegations() const { return bodyNegations; }
    const std::vector<SupportToken>& getProbabilisticSupportTokens() const {
        return probabilisticSupportTokens;
    }
    void setProbabilisticSupportTokens(std::vector<SupportToken> tokens) {
        sortUniqueSupportTokens(tokens);
        probabilisticSupportTokens = std::move(tokens);
    }
    void clearProbabilisticSupportTokens() { probabilisticSupportTokens.clear(); }
    const std::vector<bool>& getBodyNegationsStable() const {
        if (cachedSortedBodyNegations.has_value()) {
            return *cachedSortedBodyNegations;
        }
        std::vector<std::pair<NodePtr, bool>> inputNegPairs;
        for (size_t i = 0; i < inputs.size(); ++i) {
            inputNegPairs.emplace_back(inputs[i], bodyNegations[i]);
        }
        std::sort(inputNegPairs.begin(), inputNegPairs.end(), [](const auto& a, const auto& b) {
            return a.first->getTuple() < b.first->getTuple();
        });
        std::vector<bool> sortedNegations;
        for (const auto& pair : inputNegPairs) {
            sortedNegations.push_back(pair.second);
        }
        cachedSortedBodyNegations.emplace(sortedNegations.begin(), sortedNegations.end());
        return *cachedSortedBodyNegations;
    }
    void setProbability(double probability) {
        if (rule) {
            this->probability = rule->getProbability();
        } else {
            if (probability < 0.0 || probability > 1.0) {
                std::cerr << "[DerivationGraph] Edge probability out of range: " << probability
                          << " for edge id=" << id << " output=" << output->toString()
                          << std::endl;
                assert(false && "Edge probability out of [0,1]");
            }
            this->probability = probability;
            if (this->probability > 0.0 && this->probability < 1.0) {
                if (probabilisticSupportTokens.empty()) {
                    probabilisticSupportTokens = {makeEdgeSupportToken(id)};
                }
            } else {
                probabilisticSupportTokens.clear();
            }
        }
    }

    double getProbability() const { return probability; }
    bool isDeterministic() const {return probability == 1.0;}

    std::string toString() const {
        std::stringstream ss;
        ss << "Hyperedge[";
        ss << "rule" << ((rule)?std::to_string(rule->getRuleId()):" nil") << ",";
        for (size_t i = 0; i < bodyNegations.size(); ++i) {
            ss << (bodyNegations[i] ? "!" : "") << inputs[i]->getTuple().toString();
            if (i != bodyNegations.size() - 1) {
                ss << ",";
            }
        }
        ss << "->" << output->getTuple().toString() << "]";

        return ss.str();
    }
    RuleApplication getRuleApp() const { return ruleApp; }
    bool pruned = false;
    mutable std::optional<EdgeKey> cachedEdgeKey;
    mutable std::optional<bool> cachedSelfDependency;

    /**
     * @brief Generates a stable, comparable key representing the edge's content.
     * This key is composed of the rule ID, the output node's content, and an
     * ordered list of input nodes' content along with their negation status.
     * @return An EdgeKey tuple that can be used for lexicographical comparison.
     */
    EdgeKey getEdgeKey() {
        if (cachedEdgeKey.has_value()) {
            return *cachedEdgeKey;
        }
        // 1. Get the Rule ID from the RuleApplication.
        // This is the primary identifier for the rule being applied.
        size_t rule_id = ruleApp.ruleId;

        // 2. Get the output node's tuple.
        UntypedTuple output_tuple = output->getTuple();

        // 3. Build a vector of input tuples and their negation status.
        // The order is critical and is preserved by iterating from 0 to N.
        std::vector<std::pair<UntypedTuple, bool>> input_data;
        input_data.reserve(inputs.size());
        for (size_t i = 0; i < inputs.size(); ++i) {
            input_data.emplace_back(inputs[i]->getTuple(), bodyNegations[i]);
        }

        // 4. Combine these stable components into a single, comparable std::tuple.
        std::sort(input_data.begin(), input_data.end());

        cachedEdgeKey.emplace(std::make_tuple(rule_id, output_tuple, std::move(input_data)));
        return *cachedEdgeKey;
    }

    bool hasSelfDependency() const {
        if (cachedSelfDependency.has_value()) {
            return *cachedSelfDependency;
        }
        const bool hasSelf = std::find(inputs.begin(), inputs.end(), output) != inputs.end();
        cachedSelfDependency = hasSelf;
        return hasSelf;
    }

    // Rewrite endpoints after eqrel merging; callers must keep node edge lists in sync.
    void replaceOutput(const NodePtr& newOutput) {
        output = newOutput;
        cachedSortedInputs.reset();
        cachedSortedBodyNegations.reset();
        cachedEdgeKey.reset();
        cachedSelfDependency.reset();
    }

    void replaceInput(const NodePtr& oldNode, const NodePtr& newNode) {
        for (auto& input : inputs) {
            if (input == oldNode) {
                input = newNode;
            }
        }
        cachedSortedInputs.reset();
        cachedSortedBodyNegations.reset();
        cachedEdgeKey.reset();
        cachedSelfDependency.reset();
    }

    // Apply a certified event-alias map in one body traversal. The caller owns
    // adjacency updates; signs, rule applications and random events stay intact.
    size_t replaceInputs(const std::unordered_map<NodePtr, NodePtr>& replacements) {
        size_t changed = 0;
        for (auto& input : inputs) {
            const auto replacement = replacements.find(input);
            if (replacement != replacements.end()) {
                input = replacement->second;
                ++changed;
            }
        }
        if (changed != 0) {
            cachedSortedInputs.reset();
            cachedSortedBodyNegations.reset();
            cachedEdgeKey.reset();
            cachedSelfDependency.reset();
        }
        return changed;
    }

    // The alias owner has detached all old adjacency before this call. Compact
    // equal signed literals while replacing endpoints, rather than allocating a
    // hash table and erasing body positions separately for every small edge.
    std::pair<size_t, size_t> replaceInputsAndDeduplicate(
            const std::unordered_map<NodePtr, NodePtr>& replacements,
            std::unordered_map<const Node*, unsigned char>& longBodySigns) {
        if (inputs.size() != bodyNegations.size()) {
            return {replaceInputs(replacements), 0};
        }
        const bool smallBody = inputs.size() <= 8;
        if (!smallBody) longBodySigns.clear();
        size_t changed = 0;
        size_t kept = 0;
        const size_t oldSize = inputs.size();
        for (size_t i = 0; i < oldSize; ++i) {
            auto& input = inputs[i];
            const auto replacement = replacements.find(input);
            if (replacement != replacements.end()) {
                input = replacement->second;
                ++changed;
            }
            const bool negated = bodyNegations[i];
            bool duplicate = false;
            if (smallBody) {
                for (size_t j = 0; j < kept; ++j) {
                    if (inputs[j] == input && bodyNegations[j] == negated) {
                        duplicate = true;
                        break;
                    }
                }
            } else {
                const unsigned char sign = negated ? 2 : 1;
                auto& seen = longBodySigns[input.get()];
                duplicate = (seen & sign) != 0;
                seen |= sign;
            }
            if (duplicate) continue;
            if (kept != i) {
                inputs[kept] = std::move(input);
                bodyNegations[kept] = negated;
            }
            ++kept;
        }
        const size_t duplicates = oldSize - kept;
        inputs.resize(kept);
        bodyNegations.resize(kept);
        if (changed != 0 || duplicates != 0) {
            cachedSortedInputs.reset();
            cachedSortedBodyNegations.reset();
            cachedEdgeKey.reset();
            cachedSelfDependency.reset();
        }
        return {changed, duplicates};
    }

    // Remove one certified AND-input occurrence without replacing its rule event.
    // The caller owns the proof and must invalidate every graph/view cache that
    // observes this edge. Keep at least one input and preserve negation alignment.
    bool eraseInputOccurrence(size_t inputIndex) {
        if (inputs.size() <= 1 || inputIndex >= inputs.size() || bodyNegations.size() != inputs.size()) {
            return false;
        }
        const auto removed = inputs[inputIndex];
        inputs.erase(inputs.begin() + inputIndex);
        bodyNegations.erase(bodyNegations.begin() + inputIndex);
        if (removed && std::find(inputs.begin(), inputs.end(), removed) == inputs.end()) {
            auto& outgoing = removed->outgoingEdges;
            outgoing.erase(std::remove_if(outgoing.begin(), outgoing.end(),
                                   [&](const EdgePtr& edge) { return edge.get() == this; }),
                    outgoing.end());
        }
        cachedSortedInputs.reset();
        cachedSortedBodyNegations.reset();
        cachedEdgeKey.reset();
        cachedSelfDependency.reset();
        return true;
    }

private:
    Hyperedge(const std::vector<NodePtr>& inputs, NodePtr output, size_t edgeId, RuleApplication ruleApp)
        : inputs(inputs), output(output), id(edgeId), rule(nullptr), ruleApp(ruleApp) {
        // Synthetic edges default to non-negated inputs with deterministic weight
        bodyNegations = std::vector<bool>(this->inputs.size(), false);
        probability = 1.0;
    }
    Hyperedge(const std::vector<NodePtr>& inputs, NodePtr output, size_t edgeId, const Rule* rule, const std::vector<bool>& bodyNegations, RuleApplication ruleApp)
        : inputs(inputs), output(output), id(edgeId), rule(rule), ruleApp(ruleApp) {
        if (rule) {
            probability = rule->getProbability();
        } else {
            probability = 1.0;
        }
        if (bodyNegations.size() > 0) {
            assert (bodyNegations.size() == inputs.size());
            this->bodyNegations = bodyNegations;
        } else {
            this->bodyNegations = std::vector<bool>(inputs.size(), false);
        }
        if (probability > 0.0 && probability < 1.0) {
            probabilisticSupportTokens = {makeEdgeSupportToken(id)};
        }
    }


    std::vector<NodePtr> inputs;
    std::vector<bool> bodyNegations;
    NodePtr output;
    size_t id;
    double probability;
    const Rule* rule;
    const RuleApplication ruleApp;
    std::vector<SupportToken> probabilisticSupportTokens;
};

class DerivationGraphViewInterface {
public:
    virtual const std::unordered_set<NodePtr>& getNodes() const = 0;
    virtual const std::unordered_set<EdgePtr>& getEdges() const = 0;
    void dumpDot(const std::string& filename) const;
    void dumpJson(const std::string& filename) const;
    // include_heavy=true enables SCC-based cycle stats (expensive on large graphs).
    void writeGraphStatsJson(bool include_heavy = false) const;

    const std::vector<EdgePtr>& getIncomingEdges(NodePtr node) const;
    const std::vector<EdgePtr>& getOutgoingEdges(NodePtr node) const;
    std::vector<NodePtr> getInputs(EdgePtr edge) const;
    std::vector<NodePtr> getInputsStable(EdgePtr edge) const;

    NodePtr getOutput(EdgePtr edge) const;
    std::vector<bool> getBodyNegations(EdgePtr edge) const;
    std::vector<bool> getBodyNegationsStable(EdgePtr edge) const;
    static void setDumpDotEnabled(bool enabled) { dumpDotEnabled = enabled; }
    static bool isDumpDotEnabled() { return dumpDotEnabled; }
    static void setDumpJsonEnabled(bool enabled) { dumpJsonEnabled = enabled; }
    static bool isDumpJsonEnabled() { return dumpJsonEnabled; }
    static void setDumpStatsEnabled(bool enabled) { dumpStatsEnabled = enabled; }
    static bool isDumpStatsEnabled() { return dumpStatsEnabled; }
    static void setVerboseEnabled(bool enabled) { verboseEnabled = enabled; }
    static bool isVerboseEnabled() { return verboseEnabled; }
    static void setDumpOutputDir(const std::string& dir) { dumpOutputDir = dir; }
    static const std::string& getDumpOutputDir() { return dumpOutputDir; }
    static std::string qualifyDumpPath(const std::string& filename) {
        if (filename.empty()) {
            return filename;
        }
        if (!dumpOutputDir.empty()) {
            if (filename.front() == '/') {
                return filename;
            }
            if (filename.find('/') != std::string::npos) {
                return filename;
            }
            if (dumpOutputDir.back() == '/') {
                return dumpOutputDir + filename;
            }
            return dumpOutputDir + "/" + filename;
        }
        return filename;
    }

    void clearViewCaches() const;

    struct EdgeAdjacencyCacheEntry {
        size_t epoch = 0;
        std::vector<EdgePtr> edges;
    };

    mutable std::unordered_map<size_t, EdgeAdjacencyCacheEntry> cachedIncomingEdges;
    mutable std::unordered_map<size_t, EdgeAdjacencyCacheEntry> cachedOutgoingEdges;
    mutable std::unordered_map<size_t, std::vector<EdgePtr>> cachedSortedIncomingEdges;
    CycleDependencyGraph& getCycleDependencyGraph() const;
    void clearCycleDependencyGraphCache() const;
    const std::vector<EdgePtr>& getIncomingEdgesStable(NodePtr node) const;

    virtual ~DerivationGraphViewInterface() = default;
protected:
    mutable size_t adjacencyCacheEpoch_ = 1;
    mutable std::shared_ptr<CycleDependencyGraph> cachedCycleDependencyGraph_;
    static inline bool dumpDotEnabled = false;
    static inline bool dumpJsonEnabled = false;
    static inline bool dumpStatsEnabled = false;
    static inline bool verboseEnabled = false;
    static inline std::string dumpOutputDir = "";
};

inline bool nodeMayDependOnSupportTokens(const DerivationGraphViewInterface& g, NodePtr start,
        const std::vector<SupportToken>& targetTokens) {
    if (!start || targetTokens.empty()) {
        return false;
    }
    std::queue<NodePtr> work;
    std::unordered_set<size_t> seenNodes;
    std::unordered_set<size_t> seenEdges;
    work.push(start);
    seenNodes.insert(start->getId());
    while (!work.empty()) {
        NodePtr node = work.front();
        work.pop();
        if (!node) {
            continue;
        }
        if (supportTokensIntersect(node->getProbabilisticSupportTokens(), targetTokens)) {
            return true;
        }
        for (auto edge : g.getIncomingEdges(node)) {
            if (!edge || !seenEdges.insert(edge->getId()).second) {
                continue;
            }
            if (supportTokensIntersect(edge->getProbabilisticSupportTokens(), targetTokens)) {
                return true;
            }
            for (auto input : g.getInputs(edge)) {
                if (input && seenNodes.insert(input->getId()).second) {
                    work.push(input);
                }
            }
        }
    }
    return false;
}

inline bool edgeInputHasSupportOverlap(const DerivationGraphViewInterface& g, EdgePtr edge,
        size_t inputIndex, const std::vector<SupportToken>& targetTokens) {
    if (!edge || targetTokens.empty()) {
        return false;
    }
    if (supportTokensIntersect(edge->getProbabilisticSupportTokens(), targetTokens)) {
        return true;
    }
    const auto inputs = g.getInputs(edge);
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (i == inputIndex) {
            continue;
        }
        NodePtr other = inputs[i];
        if (!other) {
            continue;
        }
        if (supportTokensIntersect(other->getProbabilisticSupportTokens(), targetTokens)) {
            return true;
        }
        if (!other->isFact && nodeMayDependOnSupportTokens(g, other, targetTokens)) {
            return true;
        }
    }
    return false;
}

const std::vector<EdgePtr>& DerivationGraphViewInterface::getIncomingEdges(NodePtr node) const {
    static const std::vector<EdgePtr> empty;
    if (!node) {
        return empty;
    }
    auto& cached = cachedIncomingEdges[node->getId()];
    if (cached.epoch == adjacencyCacheEpoch_) {
        return cached.edges;
    }
    auto& result = cached.edges;
    result.clear();
    result.reserve(node->getIncomingEdges().size());
    const auto& edges = getEdges();
    for (const auto& edge : node->getIncomingEdges()) {
        if (edges.count(edge)) {
            result.push_back(edge);
        }
    }
    cached.epoch = adjacencyCacheEpoch_;
    return result;
}

const std::vector<EdgePtr>& DerivationGraphViewInterface::getIncomingEdgesStable(NodePtr node) const {
    static const std::vector<EdgePtr> empty;
    if (!node) return empty;
    auto it = cachedSortedIncomingEdges.find(node->getId());
    if (it != cachedSortedIncomingEdges.end()) {
        return it->second;
    }
    std::vector<EdgePtr> sorted = getIncomingEdges(node);
    std::sort(sorted.begin(), sorted.end(), [](const EdgePtr& a, const EdgePtr& b) {
        return a->getEdgeKey() < b->getEdgeKey();
    });
    auto inserted = cachedSortedIncomingEdges.emplace(node->getId(), std::move(sorted));
    return inserted.first->second;
}



const std::vector<EdgePtr>& DerivationGraphViewInterface::getOutgoingEdges(NodePtr node) const {
    static const std::vector<EdgePtr> empty;
    if (!node) {
        return empty;
    }
    auto& cached = cachedOutgoingEdges[node->getId()];
    if (cached.epoch == adjacencyCacheEpoch_) {
        return cached.edges;
    }
    auto& result = cached.edges;
    result.clear();
    result.reserve(node->getOutgoingEdges().size());
    const auto& edges = getEdges();
    for (const auto& edge : node->getOutgoingEdges()) {
        if (edges.count(edge)) {
            result.push_back(edge);
        }
    }
    cached.epoch = adjacencyCacheEpoch_;
    return result;
}

void DerivationGraphViewInterface::clearViewCaches() const {
    if (adjacencyCacheEpoch_ == std::numeric_limits<size_t>::max()) {
        adjacencyCacheEpoch_ = 1;
        cachedIncomingEdges.clear();
        cachedOutgoingEdges.clear();
    } else {
        ++adjacencyCacheEpoch_;
    }
    cachedSortedIncomingEdges.clear();
    clearCycleDependencyGraphCache();
}

std::vector<NodePtr> DerivationGraphViewInterface::getInputs(EdgePtr edge) const {
    if (getEdges().count(edge) == 0) {
        return std::vector<NodePtr>();
    }
    return edge->getInputs();
}

std::vector<NodePtr> DerivationGraphViewInterface::getInputsStable(EdgePtr edge) const {
    if (getEdges().count(edge) == 0) {
        return std::vector<NodePtr>();
    }
    return edge->getInputsStable();
}

NodePtr DerivationGraphViewInterface::getOutput(EdgePtr edge) const {
    NodePtr out = edge->getOutput();
    assert (out != nullptr);
    if (getNodes().count(out) == 0) {
        if (fcProfileEnabled) {
            std::cout << "Node not in view: " << out->toString() << std::endl;
        }
    }
    return getNodes().count(out) ? out : nullptr;
}

std::vector<bool> DerivationGraphViewInterface::getBodyNegations(EdgePtr edge) const {
    if (getEdges().count(edge) == 0) {
        return std::vector<bool>();
    }
    return edge->getBodyNegations();
}

std::vector<bool> DerivationGraphViewInterface::getBodyNegationsStable(EdgePtr edge) const {
    if (getEdges().count(edge) == 0) {
        return std::vector<bool>();
    }
    return edge->getBodyNegationsStable();
}

class SubgraphView : public virtual DerivationGraphViewInterface {
public:
    SubgraphView(std::unordered_set<NodePtr> nodes,
                 std::unordered_set<EdgePtr> edges)
        : nodes_(std::move(nodes)), edges_(std::move(edges)) {}

    const std::unordered_set<NodePtr>& getNodes() const override { return nodes_; }
    const std::unordered_set<EdgePtr>& getEdges() const override { return edges_; }

    // Allow callers to mutate the working view in-place.
    std::unordered_set<NodePtr>& mutableNodes() { return nodes_; }
    std::unordered_set<EdgePtr>& mutableEdges() { return edges_; }

    void dumpStatistics(std::ostream& out) const {
        if (!DerivationGraphViewInterface::isDumpStatsEnabled()) {
            return;
        }
        out << "DerivationGraph Statistics:" << std::endl;
        out << "  Number of nodes: " << nodes_.size() << std::endl;
        out << "  Number of edges: " << edges_.size() << std::endl;
    }
protected:
    std::unordered_set<NodePtr> nodes_;
    std::unordered_set<EdgePtr> edges_;
};

void DerivationGraphViewInterface::dumpJson(const std::string& filename) const {
    if (!isDumpJsonEnabled()) {
        return;
    }
    const std::string path = qualifyDumpPath(filename);
    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    out << "{\n";

    // Facts section
    out << "  \"facts\": [\n";
    bool first = true;
    for (const auto& node : getNodes()) {
        if (node->isFact) {
            if (!first) {
                out << ",\n";
            }
            first = false;
            out << "    {\"name\": " << json11::Json(node->getTuple().toString()).dump() << ",";
            out << "     \"tuple\": " << node->getTuple().toJson().dump() << ",";
            out << "     \"probability\": " << node->getProbability() << "}";
        }
    }
    out << "\n  ],\n";

    // Rules section
    out << "  \"rules\": [\n";
    first = true;
    for (const auto& edge : getEdges()) {
        if (!first) {
            out << ",\n";
        }
        first = false;

        out << "    {\n";

        // Head
        NodePtr headNode = this->getOutput(edge);
        out << "      \"head\": " << json11::Json(headNode->getTuple().toString()).dump() << ",\n";
        out << "      \"headTuple\": " << headNode->getTuple().toJson().dump() << ",\n";
        // Probability
        out << "      \"probability\": " << edge->getProbability() << ",\n";
        // Bodies
        out << "      \"bodies\": [\n";

        std::vector<NodePtr> inputs = this->getInputs(edge);
        std::vector<bool> negations = this->getBodyNegations(edge);

        for (size_t i = 0; i < inputs.size(); ++i) {
            if (i > 0) {
                out << ",\n";
            }
            out << "        {\n";

            // Handle negation - default to false if negations vector is too short
            bool isNegated = (i < negations.size()) ? negations[i] : false;
            out << "          \"negation\": " << (isNegated ? "true" : "false") << ",\n";
            out << "          \"name\": " << json11::Json(inputs[i]->getTuple().toString()).dump() << ",\n";
            out << "          \"tuple\": " << inputs[i]->getTuple().toJson().dump() << "\n";
            out << "        }";
        }

        out << "\n      ]\n";
        out << "    }";
    }

    out << "\n  ]\n";
    out << "}\n";
    out.close();
}

void DerivationGraphViewInterface::dumpDot(const std::string& filename) const {
    if (!isDumpDotEnabled()) {
        return;
    }
    const std::string path = qualifyDumpPath(filename);
    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    out << "digraph SubgraphView {\n";
    out << "  rankdir=LR;\n";

    // Node style
    out << "  node [shape=box, style=filled, fillcolor=lightblue];\n";
    for (const auto& node : getNodes()) {
        out << "  node" << node->getId() << " [label=\""
            << node->getTuple().toString() << "\"];\n";
    }

    // Edge style
    out << "  node [shape=point, fillcolor=red, width=0.2];\n";
    for (const auto& edge : getEdges()) {
        if (edge->pruned) continue;
        if (edge->hasSelfDependency()) {
            continue;  // skip edges whose head appears in body (self-loop style)
        }
        NodePtr outNode = this->getOutput(edge);
        auto inputs = this->getInputs(edge);
        out << "  edge" << edge->getId() << ";\n";

        for (const auto& input : inputs) {
            if (getNodes().count(input)) {
                out << "  node" << input->getId()
                    << " -> edge" << edge->getId() << ";\n";
            }
        }

        if (getNodes().count(outNode)) {
            out << "  edge" << edge->getId()
                << " -> node" << outNode->getId() << ";\n";
        }
    }

    out << "}\n";
    out.close();
}

inline void writeGraphStatsJson(const DerivationGraphViewInterface& g, bool include_heavy = false);

class IncrementalDerivationGraphViewInterface : virtual public DerivationGraphViewInterface {
public:
    virtual const std::set<NodePtr>& getDeltaInsertNodes() const = 0;
    virtual const std::set<EdgePtr>& getDeltaInsertEdges() const = 0;
    virtual const std::set<NodePtr>& getDeltaDeleteNodes() const = 0;
    virtual const std::set<EdgePtr>& getDeltaDeleteEdges() const = 0;
    virtual const std::set<NodePtr>& getDeltaInsertFactNodes() const {
        static const std::set<NodePtr> empty;
        return empty;
    }
    virtual const std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& getNodeImpactedByDeltaDelete() const = 0;
    virtual const std::unordered_map<NodePtr, std::unordered_set<EdgePtr>>& getEdgeImpactedByDeltaDelete() const = 0;
    virtual const std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& getNodeImpactedByDeltaInsert() const = 0;
    virtual const std::unordered_map<NodePtr, std::unordered_set<EdgePtr>>& getEdgeImpactedByDeltaInsert() const = 0;
    virtual const std::unordered_set<NodePtr>& getDeltaInsertReachableNodes() const = 0;
    virtual const std::unordered_set<EdgePtr>& getDeltaInsertReachableEdges() const = 0;
    virtual const std::unordered_set<NodePtr>& getDeleteImpactDetNodes() const {
        static const std::unordered_set<NodePtr> empty;
        return empty;
    }
    virtual const std::unordered_set<EdgePtr>& getDeleteImpactDetEdges() const {
        static const std::unordered_set<EdgePtr> empty;
        return empty;
    }
    virtual const std::unordered_set<NodePtr>& getDeleteImpactNonDetNodes() const {
        static const std::unordered_set<NodePtr> empty;
        return empty;
    }
    virtual const std::unordered_set<EdgePtr>& getDeleteImpactNonDetEdges() const {
        static const std::unordered_set<EdgePtr> empty;
        return empty;
    }
    const std::set<NodePtr>& getValidNodes() {
        if (validNodes_.size() > 0) {
            return validNodes_;
        }
        for (const auto& node : getNodes()) {
            if (getDeltaDeleteNodes().count(node) == 0 && node->pruned == false) {
                validNodes_.insert(node);
            }
        }
        return validNodes_;
     }
    const std::set<EdgePtr>& getValidEdges() {
        if (validEdges_.size() > 0) {
            return validEdges_;
        }
        for (const auto& edge : getEdges()) {
            if (getDeltaDeleteEdges().count(edge) == 0 && edge->pruned == false) {
                validEdges_.insert(edge);
            }
        }
        return validEdges_;
     }
     const std::set<NodePtr>& getValidNodes() const {
        return const_cast<IncrementalDerivationGraphViewInterface*>(this)->getValidNodes();
    }
    const std::set<EdgePtr>& getValidEdges() const {
        return const_cast<IncrementalDerivationGraphViewInterface*>(this)->getValidEdges();
    }
    const std::set<NodePtr>& getDeletedFacts() {
        if (!deletedFacts_.empty()) {
            return deletedFacts_;
        }
        deletedFacts_.insert(explicitDeletedFacts_.begin(), explicitDeletedFacts_.end());
        return deletedFacts_;
    }
    const std::set<NodePtr>& getDeletedDeterminsticFacts() {
        if (deletedDeterminsticFacts_.size() > 0) {
            return deletedDeterminsticFacts_;
        }
        for (const auto& deletedFact: getDeletedFacts()) {
            if (deletedFact->getProbability() == 1.0) {
                deletedDeterminsticFacts_.insert(deletedFact);
            }
        }
        return deletedDeterminsticFacts_;
    }
    const std::set<NodePtr>& getDeletedNonDeterministicFacts() {
        if (deletedNonDeterministicFacts_.size() > 0) {
            return deletedNonDeterministicFacts_;
        }
        for (const auto& deletedFact: getDeletedFacts()) {
            if (deletedFact->getProbability() < 1.0) {
                deletedNonDeterministicFacts_.insert(deletedFact);
            }
        }
        return deletedNonDeterministicFacts_;
    }

    // Drop cached validity/adjacency info after structural updates.
    void invalidateCaches() {
        validNodes_.clear();
        validEdges_.clear();
        deletedFacts_.clear();
        deletedDeterminsticFacts_.clear();
        deletedNonDeterministicFacts_.clear();
        clearViewCaches();
    }

    // deletion impacted
    // insertion impacted

    void dumpDotInc(const std::string& filename) const;
    void dumpJsonInc(const std::string& filename) const;
    void dumpStatisticsInc(std::ostream& out) {
        if (!DerivationGraphViewInterface::isDumpStatsEnabled()) {
            return;
        }
        out << "IncrementalDerivationGraph Statistics:" << std::endl;
        out << "  Number of nodes: " << getNodes().size() << std::endl;
        out << "  Number of edges: " << getEdges().size() << std::endl;
        out << "  Number of delta insert nodes: " << getDeltaInsertNodes().size() << std::endl;
        out << "  Number of delta insert edges: " << getDeltaInsertEdges().size() << std::endl;
        out << "  Number of delta delete nodes: " << getDeltaDeleteNodes().size() << std::endl;
        out << "  Number of delta delete edges: " << getDeltaDeleteEdges().size() << std::endl;
        out << "  Impact (del nodes): " << getNodeImpactedByDeltaDelete().size() << std::endl;
        out << "  Impact (del edges): " << getEdgeImpactedByDeltaDelete().size() << std::endl;
        out << "  Impact (ins nodes): " << getNodeImpactedByDeltaInsert().size() << std::endl;
        out << "  Impact (ins edges): " << getEdgeImpactedByDeltaInsert().size() << std::endl;
        this->writeGraphStatsJson();
    }
protected:
    std::set<NodePtr> validNodes_;
    std::set<EdgePtr> validEdges_;
    std::set<NodePtr> explicitDeletedFacts_;
    std::set<NodePtr> deletedFacts_;
    std::set<NodePtr> deletedDeterminsticFacts_;
    std::set<NodePtr> deletedNonDeterministicFacts_;
};

class IncSubgraphView : public SubgraphView, public virtual IncrementalDerivationGraphViewInterface {
public:
    IncSubgraphView(const std::unordered_set<NodePtr>& nodes,
                    const std::unordered_set<EdgePtr>& edges,
                    const std::set<NodePtr>& deltaInsertNodes,
                    const std::set<EdgePtr>& deltaInsertEdges,
                    const std::set<NodePtr>& deltaDeleteNodes,
                    const std::set<EdgePtr>& deltaDeleteEdges,
                    const std::set<NodePtr>& deltaInsertFactNodes = {},
            const std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& nodeImpactedByDeltaDelete = {},
            const std::unordered_map<NodePtr, std::unordered_set<EdgePtr>>& edgeImpactedByDeltaDelete = {},
            const std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& nodeImpactedByDeltaInsert = {},
            const std::unordered_map<NodePtr, std::unordered_set<EdgePtr>>& edgeImpactedByDeltaInsert = {},
            const std::unordered_set<NodePtr>& deltaInsertReachableNodes = {},
            const std::unordered_set<EdgePtr>& deltaInsertReachableEdges = {},
            const std::unordered_set<NodePtr>& deleteImpactDetNodes = {},
            const std::unordered_set<EdgePtr>& deleteImpactDetEdges = {},
            const std::unordered_set<NodePtr>& deleteImpactNonDetNodes = {},
            const std::unordered_set<EdgePtr>& deleteImpactNonDetEdges = {},
            const std::set<NodePtr>& explicitDeletedFacts = {},
            std::vector<NodePtr> deletedOutputNodes = {},
            std::vector<NodePtr> outputNodes = {},
            std::vector<NodePtr> evidenceNodes = {})
            : SubgraphView(nodes, edges),
              deltaInsertNodes_(deltaInsertNodes),
              deltaInsertEdges_(deltaInsertEdges),
              deltaDeleteNodes_(deltaDeleteNodes),
              deltaDeleteEdges_(deltaDeleteEdges),
            deltaInsertFactNodes_(deltaInsertFactNodes),
            nodeImpactedByDeltaDelete_(nodeImpactedByDeltaDelete),
            edgeImpactedByDeltaDelete_(edgeImpactedByDeltaDelete),
            nodeImpactedByDeltaInsert_(nodeImpactedByDeltaInsert),
            edgeImpactedByDeltaInsert_(edgeImpactedByDeltaInsert),
            deltaInsertReachableNodes_(deltaInsertReachableNodes),
            deltaInsertReachableEdges_(deltaInsertReachableEdges),
            deleteImpactDetNodes_(deleteImpactDetNodes),
            deleteImpactDetEdges_(deleteImpactDetEdges),
            deleteImpactNonDetNodes_(deleteImpactNonDetNodes),
            deleteImpactNonDetEdges_(deleteImpactNonDetEdges),
            deletedOutputNodes_(std::move(deletedOutputNodes)),
            outputNodes_(std::move(outputNodes)),
            evidenceNodes_(std::move(evidenceNodes)) {
            explicitDeletedFacts_ = explicitDeletedFacts;
        }

    // getNodes() / getEdges() from DerivationGraphView
    const std::unordered_set<NodePtr>& getNodes() const override {return nodes_; };
    const std::unordered_set<EdgePtr>& getEdges() const override {return edges_; };

    const std::set<NodePtr>& getDeltaInsertNodes() const override {return deltaInsertNodes_; };
    const std::set<EdgePtr>& getDeltaInsertEdges() const override {return deltaInsertEdges_; };
    const std::set<NodePtr>& getDeltaDeleteNodes() const override {return deltaDeleteNodes_; };
    const std::set<EdgePtr>& getDeltaDeleteEdges() const override {return deltaDeleteEdges_; };
    const std::set<NodePtr>& getDeltaInsertFactNodes() const override {return deltaInsertFactNodes_; };


    const std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& getNodeImpactedByDeltaDelete() const override {
        return nodeImpactedByDeltaDelete_;
    }
    const std::unordered_map<NodePtr, std::unordered_set<EdgePtr>>& getEdgeImpactedByDeltaDelete() const override {
        return edgeImpactedByDeltaDelete_;
    }
    const std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& getNodeImpactedByDeltaInsert() const override {
        return nodeImpactedByDeltaInsert_;
    }
    const std::unordered_map<NodePtr, std::unordered_set<EdgePtr>>& getEdgeImpactedByDeltaInsert() const override {
        return edgeImpactedByDeltaInsert_;
    }
    const std::unordered_set<NodePtr>& getDeltaInsertReachableNodes() const override {
        return deltaInsertReachableNodes_;
    }
    const std::unordered_set<EdgePtr>& getDeltaInsertReachableEdges() const override {
        return deltaInsertReachableEdges_;
    }
    const std::unordered_set<NodePtr>& getDeleteImpactDetNodes() const override {
        return deleteImpactDetNodes_;
    }
    const std::unordered_set<EdgePtr>& getDeleteImpactDetEdges() const override {
        return deleteImpactDetEdges_;
    }
    const std::unordered_set<NodePtr>& getDeleteImpactNonDetNodes() const override {
        return deleteImpactNonDetNodes_;
    }
    const std::unordered_set<EdgePtr>& getDeleteImpactNonDetEdges() const override {
        return deleteImpactNonDetEdges_;
    }
    const std::vector<NodePtr>& getOutputNodes() const {
        return outputNodes_;
    }
    const std::vector<NodePtr>& getDeletedOutputNodes() const {
        return deletedOutputNodes_;
    }
    const std::vector<NodePtr>& getEvidenceNodes() const {
        return evidenceNodes_;
    }


    // Optional: dumpDot for incremental view

protected:
    std::set<NodePtr> deltaInsertNodes_;
    std::set<EdgePtr> deltaInsertEdges_;
    std::set<NodePtr> deltaDeleteNodes_;
    std::set<EdgePtr> deltaDeleteEdges_;
    std::set<NodePtr> deltaInsertFactNodes_;
    std::unordered_map<NodePtr, std::unordered_set<NodePtr>> nodeImpactedByDeltaDelete_;
    std::unordered_map<NodePtr, std::unordered_set<EdgePtr>> edgeImpactedByDeltaDelete_;
    std::unordered_map<NodePtr, std::unordered_set<NodePtr>> nodeImpactedByDeltaInsert_;
    std::unordered_map<NodePtr, std::unordered_set<EdgePtr>> edgeImpactedByDeltaInsert_;
    std::unordered_set<NodePtr> deltaInsertReachableNodes_;
    std::unordered_set<EdgePtr> deltaInsertReachableEdges_;
    std::unordered_set<NodePtr> deleteImpactDetNodes_;
    std::unordered_set<EdgePtr> deleteImpactDetEdges_;
    std::unordered_set<NodePtr> deleteImpactNonDetNodes_;
    std::unordered_set<EdgePtr> deleteImpactNonDetEdges_;
    std::vector<NodePtr> deletedOutputNodes_;
    std::vector<NodePtr> outputNodes_;
    std::vector<NodePtr> evidenceNodes_;
};

class WorkingDerivationGraphViewInterface : virtual public DerivationGraphViewInterface {
public:
    const std::set<NodePtr>& getValidNodes() {
        if (validNodes_.size() > 0) {
            return validNodes_;
        }
        for (const auto& node : getNodes()) {
            if (node->pruned == false) {
                validNodes_.insert(node);
            }
        }
        return validNodes_;
     }
    const std::set<EdgePtr>& getValidEdges() {
        if (validEdges_.size() > 0) {
            return validEdges_;
        }
        for (const auto& edge : getEdges()) {
            if (edge->pruned == false) {
                validEdges_.insert(edge);
            }
        }
        return validEdges_;
     }
     const std::set<NodePtr>& getValidNodes() const {
        return const_cast<WorkingDerivationGraphViewInterface*>(this)->getValidNodes();
    }
     const std::set<EdgePtr>& getValidEdges() const {
        return const_cast<WorkingDerivationGraphViewInterface*>(this)->getValidEdges();
    }

    // Drop cached validity/adjacency info after structural rewrites.
    void invalidateCaches() {
        validNodes_.clear();
        validEdges_.clear();
        clearViewCaches();
    }

    void dumpWorkingStatistics(std::ostream& out) {
        if (!DerivationGraphViewInterface::isDumpStatsEnabled()) {
            return;
        }
        out << "WorkingDerivationGraph Statistics:" << std::endl;
        out << "  Number of nodes: " << getNodes().size() << std::endl;
        out << "  Number of edges: " << getEdges().size() << std::endl;
        this->writeGraphStatsJson();
    }
protected:
    std::set<NodePtr> validNodes_;
    std::set<EdgePtr> validEdges_;
};

class WorkingSubgraphView : public SubgraphView, public virtual WorkingDerivationGraphViewInterface {
public:
    enum class EvidenceRoots { Discover, Complete };

    WorkingSubgraphView(std::unordered_set<NodePtr> nodes,
                    std::unordered_set<EdgePtr> edges,
            std::vector<NodePtr> evidenceNodes = {},
            EvidenceRoots evidenceRoots = EvidenceRoots::Discover)
            : SubgraphView(std::move(nodes), std::move(edges)),
            evidenceNodes_(std::move(evidenceNodes)) {
            if (evidenceNodes_.empty() && evidenceRoots == EvidenceRoots::Discover) {
                for (const auto& node : nodes_) {
                    if (node && node->hasEvidence()) {
                        evidenceNodes_.push_back(node);
                    }
                }
            }
        }

    WorkingSubgraphView(const WorkingSubgraphView& other)
            : SubgraphView(other.nodes_, other.edges_),
              evidenceNodes_(other.evidenceNodes_) {}

    WorkingSubgraphView(WorkingSubgraphView&& other) noexcept
            : SubgraphView(std::move(other.nodes_), std::move(other.edges_)),
              evidenceNodes_(std::move(other.evidenceNodes_)) {}

    WorkingSubgraphView& operator=(const WorkingSubgraphView& other) {
        if (this != &other) {
            nodes_ = other.nodes_;
            edges_ = other.edges_;
            evidenceNodes_ = other.evidenceNodes_;
            invalidateCaches();
        }
        return *this;
    }

    WorkingSubgraphView& operator=(WorkingSubgraphView&& other) noexcept {
        if (this != &other) {
            nodes_ = std::move(other.nodes_);
            edges_ = std::move(other.edges_);
            evidenceNodes_ = std::move(other.evidenceNodes_);
            invalidateCaches();
        }
        return *this;
    }

    // getNodes() / getEdges() from DerivationGraphView
    const std::unordered_set<NodePtr>& getNodes() const override {return nodes_; };
    const std::unordered_set<EdgePtr>& getEdges() const override {return edges_; };

    const std::vector<NodePtr>& getEvidenceNodes() const {
        return evidenceNodes_;
    }

    // Apply a certified cleanup plan that preserves every query/evidence root.
    void applyPruning(const std::vector<NodePtr>& nodes, const std::vector<EdgePtr>& edges) {
        for (const auto& edge : edges) {
            if (edges_.erase(edge) != 0) {
                edge->pruned = true;
            }
        }
        for (const auto& node : nodes) {
            if (nodes_.erase(node) != 0) {
                node->pruned = true;
            }
        }
        invalidateCaches();
    }

protected:
    std::vector<NodePtr> evidenceNodes_;
};

class DerivationGraph: virtual public DerivationGraphViewInterface {
public:
    static void setMergeBiImpEnabled(bool enabled) {
        mergeBiImpEnabled = enabled;
    }
    static void setPruneExtraEnabled(bool enabled) {
        pruneExtraEnabled = enabled;
    }
    static bool isPruneExtraEnabled() {
        return pruneExtraEnabled;
    }
    bool isBiImpMerged() const {
        return biImpMerged;
    }

    struct EdgeLookupTiming {
        size_t find_calls;
        double find_key_s;
        double find_map_s;
        size_t insert_calls;
        double insert_key_s;
        double insert_map_s;

        EdgeLookupTiming()
                : find_calls(0),
                  find_key_s(0.0),
                  find_map_s(0.0),
                  insert_calls(0),
                  insert_key_s(0.0),
                  insert_map_s(0.0) {}
    };

    struct EdgeBuildTiming {
        size_t calls;
        size_t existing_hits;
        size_t body_atoms;
        double total_s;
        double rule_lookup_s;
        double get_vars_s;
        double find_existing_s;
        double head_instantiate_s;
        double head_node_s;
        double body_instantiate_s;
        double body_node_s;
        double body_neg_s;
        double create_edge_s;
        double insert_total_s;

        EdgeBuildTiming()
                : calls(0),
                  existing_hits(0),
                  body_atoms(0),
                  total_s(0.0),
                  rule_lookup_s(0.0),
                  get_vars_s(0.0),
                  find_existing_s(0.0),
                  head_instantiate_s(0.0),
                  head_node_s(0.0),
                  body_instantiate_s(0.0),
                  body_node_s(0.0),
                  body_neg_s(0.0),
                  create_edge_s(0.0),
                  insert_total_s(0.0) {}
    };

    DerivationGraph() : nextNodeId(0), nextEdgeId(0) {}
    DerivationGraph(const RuleManager* rm) : nextNodeId(0), nextEdgeId(0), ruleManager(rm) {}
    void dumpStatistics(std::ostream& out) const {
        if (!DerivationGraphViewInterface::isDumpStatsEnabled()) {
            return;
        }
        out << "DerivationGraph Statistics:" << std::endl;
        out << "  Number of nodes: " << nodes.size() << std::endl;
        out << "  Number of edges: " << edges.size() << std::endl;
    }
    NodePtr createNode(const UntypedTuple& tuple, const double weight = 1.0) {
        // Check if it already exists.
        NodePtr existingNode = findNode(tuple);
        if (existingNode) {
            return existingNode;
        }

        // Create a new node if not found.
        auto node = std::shared_ptr<Node>(new Node(tuple, nextNodeId++));
        nodes.insert(node);
        node->setProbability(weight);
        if (detOptEnabled && isDetRelation(tuple.relation_name)) {
            node->isFact = true;
            node->setOriginalFact(true);
        }

        // Add to the map.
        tupleToNodeMap[tuple] = node;
        existingTuples.insert(tuple);
        return node;
    }

    void createQuery(NodePtr node, const QueryManager& queryManager) {
        const auto& tuple = node->getTuple();
        const std::string& relation = tuple.relation_name;
        const auto& fields = tuple.fields;

        for (const auto& query : queryManager.getAllQuery()) {
            if (query->matchesTuple(relation, fields)) {
                node->setQuery();
            }
        }
    }
    struct AggregateTupleIndex {
        const AggregateSpec* spec = nullptr;
        std::unordered_map<std::vector<souffle::RamDomain>, std::vector<UntypedTuple>, RamDomainVectorHash>
                tuplesByBoundKey;
    };

    struct AggregateMatchResult {
        std::vector<std::string> localVarNames;
        std::vector<souffle::RamDomain> localVarValues;
        souffle::RamSigned weight = 0;
    };

    static bool isWildcardAggregateField(const SymbolicField& field) {
        if (!std::holds_alternative<VariableField>(field.field)) {
            return false;
        }
        return std::get<VariableField>(field.field).name == "_";
    }

    static std::optional<std::string> getAggregateVariableName(const SymbolicField& field) {
        if (!std::holds_alternative<VariableField>(field.field)) {
            return std::nullopt;
        }
        return std::get<VariableField>(field.field).name;
    }

    std::vector<souffle::RamDomain> buildAggregateBoundKeyFromTuple(
            const AggregateSpec& spec, const UntypedTuple& tuple,
            const std::unordered_set<std::string>& ruleVarSet) const {
        std::vector<souffle::RamDomain> key;
        const auto& fields = spec.witnessAtom.getFields();
        if (fields.size() != tuple.fields.size()) {
            return key;
        }
        for (size_t i = 0; i < fields.size(); ++i) {
            if (isWildcardAggregateField(fields[i])) {
                continue;
            }
            if (const auto varName = getAggregateVariableName(fields[i]); varName.has_value()) {
                if (ruleVarSet.count(*varName) == 0) {
                    continue;
                }
            }
            key.push_back(tuple.fields[i]);
        }
        return key;
    }

    std::vector<souffle::RamDomain> buildAggregateBoundKeyFromRuleApp(
            const AggregateSpec& spec, const std::vector<std::string>& vars,
            const std::vector<souffle::RamDomain>& values,
            const std::unordered_set<std::string>& ruleVarSet) const {
        std::vector<souffle::RamDomain> key;
        const auto& fields = spec.witnessAtom.getFields();
        for (const auto& field : fields) {
            if (isWildcardAggregateField(field)) {
                continue;
            }
            if (const auto varName = getAggregateVariableName(field); varName.has_value()) {
                if (ruleVarSet.count(*varName) == 0) {
                    continue;
                }
            }
            key.push_back(evaluateSymbolicField(field, vars, values));
        }
        return key;
    }

    std::optional<AggregateMatchResult> matchAggregateWitnessTuple(
            const AggregateSpec& spec, const std::vector<std::string>& vars,
            const std::vector<souffle::RamDomain>& values, const UntypedTuple& tuple,
            const std::unordered_set<std::string>& ruleVarSet) const {
        const auto& fields = spec.witnessAtom.getFields();
        if (fields.size() != tuple.fields.size()) {
            return std::nullopt;
        }
        std::unordered_map<std::string, souffle::RamDomain> localBindings;
        for (size_t i = 0; i < fields.size(); ++i) {
            const auto& field = fields[i];
            const auto tupleValue = tuple.fields[i];
            if (isWildcardAggregateField(field)) {
                continue;
            }
            if (const auto varName = getAggregateVariableName(field); varName.has_value()) {
                if (ruleVarSet.count(*varName) > 0) {
                    if (evaluateSymbolicField(field, vars, values) != tupleValue) {
                        return std::nullopt;
                    }
                    continue;
                }
                auto [it, inserted] = localBindings.emplace(*varName, tupleValue);
                if (!inserted && it->second != tupleValue) {
                    return std::nullopt;
                }
                continue;
            }
            if (evaluateSymbolicField(field, vars, values) != tupleValue) {
                return std::nullopt;
            }
        }

        AggregateMatchResult result;
        result.localVarNames.reserve(localBindings.size());
        result.localVarValues.reserve(localBindings.size());
        for (const auto& [name, value] : localBindings) {
            result.localVarNames.push_back(name);
            result.localVarValues.push_back(value);
        }
        std::vector<std::string> mergedVars = vars;
        std::vector<souffle::RamDomain> mergedValues = values;
        mergedVars.insert(mergedVars.end(), result.localVarNames.begin(), result.localVarNames.end());
        mergedValues.insert(mergedValues.end(), result.localVarValues.begin(), result.localVarValues.end());
        result.weight = souffle::ramBitCast<souffle::RamSigned>(
                evaluateSymbolicField(spec.weightExpr, mergedVars, mergedValues));
        return result;
    }

    UntypedTuple makeAggregateStateTuple(
            souffle::RamDomain ruleId, size_t headNodeId, size_t aggregateIndex,
            size_t step, souffle::RamSigned sum) const {
        UntypedTuple tuple;
        tuple.relation_name = "__agg_sum_state";
        tuple.fields = {
                static_cast<souffle::RamDomain>(ruleId),
                static_cast<souffle::RamDomain>(headNodeId),
                static_cast<souffle::RamDomain>(aggregateIndex),
                static_cast<souffle::RamDomain>(step),
                souffle::ramBitCast<souffle::RamDomain>(sum)};
        return tuple;
    }

    NodePtr buildAggregateSumNode(
            souffle::RamDomain ruleId, size_t aggregateIndex, NodePtr headNode,
            const AggregateSpec& spec, const std::vector<std::string>& vars,
            const std::vector<souffle::RamDomain>& values,
            const std::unordered_set<std::string>& ruleVarSet) {
        auto itPerRule = aggregateTupleIndices.find(static_cast<std::size_t>(ruleId));
        if (itPerRule == aggregateTupleIndices.end() || aggregateIndex >= itPerRule->second.size()) {
            return nullptr;
        }
        const auto& tupleIndex = itPerRule->second[aggregateIndex];
        const auto lookupKey = buildAggregateBoundKeyFromRuleApp(spec, vars, values, ruleVarSet);
        std::vector<std::pair<NodePtr, souffle::RamSigned>> witnesses;
        if (auto it = tupleIndex.tuplesByBoundKey.find(lookupKey); it != tupleIndex.tuplesByBoundKey.end()) {
            witnesses.reserve(it->second.size());
            for (const auto& tuple : it->second) {
                auto match = matchAggregateWitnessTuple(spec, vars, values, tuple, ruleVarSet);
                if (!match.has_value()) {
                    continue;
                }
                witnesses.emplace_back(createNode(tuple), match->weight);
            }
        }
        std::sort(witnesses.begin(), witnesses.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first->getTuple() < rhs.first->getTuple();
        });

        const auto targetSum = souffle::ramBitCast<souffle::RamSigned>(
                evaluateSymbolicField(SymbolicField::makeVariable(spec.resultVar), vars, values));
        std::map<souffle::RamSigned, NodePtr> currentStates;
        auto baseNode = createNode(makeAggregateStateTuple(ruleId, headNode->getId(), aggregateIndex, 0, 0));
        baseNode->isFact = true;
        baseNode->setProbability(1.0);
        currentStates.emplace(0, baseNode);

        for (size_t i = 0; i < witnesses.size(); ++i) {
            std::map<souffle::RamSigned, NodePtr> nextStates;
            const auto& [witnessNode, weight] = witnesses[i];
            auto ensureState = [&](souffle::RamSigned sum) -> NodePtr {
                auto existing = nextStates.find(sum);
                if (existing != nextStates.end()) {
                    return existing->second;
                }
                auto node = createNode(makeAggregateStateTuple(
                        ruleId, headNode->getId(), aggregateIndex, i + 1, sum));
                nextStates.emplace(sum, node);
                return node;
            };
            for (const auto& [sum, prevNode] : currentStates) {
                auto skipNode = ensureState(sum);
                createHyperedge({prevNode, witnessNode}, skipNode, nullptr, {false, true});
                auto takeNode = ensureState(sum + weight);
                createHyperedge({prevNode, witnessNode}, takeNode, nullptr, {false, false});
            }
            currentStates = std::move(nextStates);
        }

        auto finalIt = currentStates.find(targetSum);
        if (finalIt == currentStates.end()) {
            return nullptr;
        }
        return finalIt->second;
    }

    void buildAggregateTupleIndices(
            const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& ruleApps,
            const std::unordered_map<UntypedTuple, double>& factProb,
            const RuleManager& ruleManager) {
        std::unordered_map<std::string, std::vector<UntypedTuple>> tuplesByRelation;
        for (const auto& [tuple, _] : factProb) {
            tuplesByRelation[tuple.relation_name].push_back(tuple);
        }
        for (const auto& [tuple, _] : ruleApps) {
            tuplesByRelation[tuple.relation_name].push_back(tuple);
        }
        aggregateTupleIndices.clear();
        for (const auto* rule : ruleManager.getAllRules()) {
            if (rule == nullptr || rule->getAggregates().empty()) {
                continue;
            }
            const auto ruleVars = rule->getVars();
            const std::unordered_set<std::string> ruleVarSet(
                    ruleVars.begin(), ruleVars.end());
            std::vector<AggregateTupleIndex> perRule;
            perRule.reserve(rule->getAggregates().size());
            for (const auto& spec : rule->getAggregates()) {
                AggregateTupleIndex index;
                index.spec = &spec;
                auto tuplesIt = tuplesByRelation.find(spec.witnessAtom.getRelation());
                if (tuplesIt != tuplesByRelation.end()) {
                    for (const auto& tuple : tuplesIt->second) {
                        if (spec.witnessAtom.getFields().size() != tuple.fields.size()) {
                            continue;
                        }
                        index.tuplesByBoundKey[buildAggregateBoundKeyFromTuple(spec, tuple, ruleVarSet)]
                                .push_back(tuple);
                    }
                }
                perRule.push_back(std::move(index));
            }
            aggregateTupleIndices.emplace(rule->getRuleId(), std::move(perRule));
        }
    }

    const std::vector<std::pair<UntypedTuple, bool>>& getEvidences() const {
        return evidences;
    }
    std::vector<std::pair<NodePtr, bool>> resolveEvidenceNodes() const {
        std::vector<std::pair<NodePtr, bool>> resolved;
        resolved.reserve(evidences.size());
        for (const auto& [tup, val] : evidences) {
            NodePtr node = findNode(tup);
            if (!node) {
                throw std::runtime_error("Evidence " + tup.toString() + " is not found in the graph.");
            }
            resolved.emplace_back(node, val);
        }
        return resolved;
    }
    void attachEvidence(const std::vector<std::pair<UntypedTuple,bool>>& evidenceList) {
        evidences.clear();
        for (const auto& [tuple, value] : evidenceList) {
            NodePtr node = findNode(tuple);
            if (node) {
                if (node->hasEvidence() && node->getEvidenceValue() != value) {
                    throw std::runtime_error("Inconsistent evidence: conflicting values for " + tuple.toString());
                }
                node->setEvidence(value);
                evidences.emplace_back(tuple, value);
                if (DerivationGraphViewInterface::isVerboseEnabled()) {
                    std::cout << "[Info] Attached evidence (" << tuple.toString() << ","
                                          << (value ? "true" : "false")
                                          << ") to node " << node->toString() << std::endl;
                }
            } else if (value) {
                throw std::runtime_error("Inconsistent evidence: " + tuple.toString() +
                        " is deterministically false");
            }
        }
    }


    bool tupleExistsInUniverse(const UntypedTuple& tuple) const {
        return existingTuples.find(tuple) != existingTuples.end();
    }

    EdgePtr createHyperedgeFromRuleApp(const RuleApplication& ruleApp, const RuleManager& rm) {
        const bool timing = DerivationGraphViewInterface::isDumpStatsEnabled();

        if (!timing) {
            // Check if the corresponding edge already exists.
            const Rule* rule = rm.getRule(ruleApp.ruleId);
            assert(rule != nullptr && "Rule not found");
            std::vector<std::string> vars = rule->getVars();

            EdgePtr existingEdge = findHyperedgeFromRuleApp(ruleApp, vars);
            if (existingEdge) {
                return existingEdge;
            }

            // Create output tuple from rule head and variable values.
            UntypedTuple headTuple{rule->getHead().getRelation(),
                    rule->getHead().instantiatedFields(vars, ruleApp.varValuesPure)};
            auto headNode = createNode(headTuple);
            if (rule->isFact() && !headNode->isFact) {
                // Fact rules are represented as fact nodes (no hyperedge is created for empty-body rules).
                headNode->isFact = true;
                headNode->setProbability(rule->getProbability());
                headNode->setOriginalFact(true);
            }

            // Create input nodes.
            std::vector<NodePtr> bodyNodes;
            std::vector<bool> bodyNegations;
            const std::unordered_set<std::string> ruleVarSet(vars.begin(), vars.end());
            for (const auto& bodyAtom : rule->getBodyAtoms()) {
                UntypedTuple bodyTuple{bodyAtom.getRelation(),
                        bodyAtom.instantiatedFields(vars, ruleApp.varValuesPure)};
                if (bodyAtom.isNegatedAtom() && !tupleExistsInUniverse(bodyTuple)) {
                    continue;
                }
                auto bodyNode = createNode(bodyTuple);
                bodyNodes.push_back(bodyNode);
                bodyNegations.push_back(bodyAtom.isNegatedAtom());
            }
            for (size_t aggregateIndex = 0; aggregateIndex < rule->getAggregates().size(); ++aggregateIndex) {
                auto aggNode = buildAggregateSumNode(
                        ruleApp.ruleId, aggregateIndex, headNode, rule->getAggregates()[aggregateIndex],
                        vars, ruleApp.varValuesPure, ruleVarSet);
                assert(aggNode != nullptr && "Aggregate replay failed to reconstruct target sum");
                bodyNodes.push_back(aggNode);
                bodyNegations.push_back(false);
            }
            auto newEdge = createHyperedge(bodyNodes, headNode, rule, bodyNegations, ruleApp);

            // Add the new edge to the map.
            std::string key = createEdgeKey(ruleApp.ruleId, vars, ruleApp.varValuesPure);
            edgeKeyToEdgeMap[key] = newEdge;

            return newEdge;
        }

        const auto t_begin = std::chrono::steady_clock::now();
        const auto t_rule0 = t_begin;
        // Check if the corresponding edge already exists.
        const Rule* rule = rm.getRule(ruleApp.ruleId);
        assert(rule != nullptr && "Rule not found");
        const auto t_rule1 = std::chrono::steady_clock::now();
        const auto t_vars0 = t_rule1;
        std::vector<std::string> vars = rule->getVars();
        const auto t_vars1 = std::chrono::steady_clock::now();

        const auto t_find0 = t_vars1;
        EdgePtr existingEdge = findHyperedgeFromRuleApp(ruleApp, vars);
        const auto t_find1 = std::chrono::steady_clock::now();
        if (existingEdge) {
            const auto t_end = std::chrono::steady_clock::now();
            edgeBuildTiming.calls++;
            edgeBuildTiming.existing_hits++;
            edgeBuildTiming.total_s += std::chrono::duration<double>(t_end - t_begin).count();
            edgeBuildTiming.rule_lookup_s += std::chrono::duration<double>(t_rule1 - t_rule0).count();
            edgeBuildTiming.get_vars_s += std::chrono::duration<double>(t_vars1 - t_vars0).count();
            edgeBuildTiming.find_existing_s += std::chrono::duration<double>(t_find1 - t_find0).count();
            return existingEdge;
        }

        // Create output tuple from rule head and variable values.
        const auto t_head_inst0 = std::chrono::steady_clock::now();
        UntypedTuple headTuple{rule->getHead().getRelation(),
                rule->getHead().instantiatedFields(vars, ruleApp.varValuesPure)};
        const auto t_head_inst1 = std::chrono::steady_clock::now();
        const auto t_head_node0 = t_head_inst1;
        auto headNode = createNode(headTuple);
        const auto t_head_node1 = std::chrono::steady_clock::now();
        if (rule->isFact() && !headNode->isFact) {
            // Fact rules are represented as fact nodes (no hyperedge is created for empty-body rules).
            headNode->isFact = true;
            headNode->setProbability(rule->getProbability());
            headNode->setOriginalFact(true);
        }

        // Create input nodes.
        std::vector<NodePtr> bodyNodes;
        std::vector<bool> bodyNegations;
        const std::unordered_set<std::string> ruleVarSet(vars.begin(), vars.end());
        size_t body_atoms = 0;
        double body_inst_s = 0.0;
        double body_node_s = 0.0;
        double body_neg_s = 0.0;
        for (const auto& bodyAtom : rule->getBodyAtoms()) {
            const auto t_body_inst0 = std::chrono::steady_clock::now();
            UntypedTuple bodyTuple{bodyAtom.getRelation(),
                    bodyAtom.instantiatedFields(vars, ruleApp.varValuesPure)};
            const auto t_body_inst1 = std::chrono::steady_clock::now();
            if (bodyAtom.isNegatedAtom() && !tupleExistsInUniverse(bodyTuple)) {
                body_inst_s += std::chrono::duration<double>(t_body_inst1 - t_body_inst0).count();
                continue;
            }
            auto bodyNode = createNode(bodyTuple);
            const auto t_body_node1 = std::chrono::steady_clock::now();
            bodyNodes.push_back(bodyNode);
            const auto t_body_neg0 = std::chrono::steady_clock::now();
            bodyNegations.push_back(bodyAtom.isNegatedAtom());
            const auto t_body_neg1 = std::chrono::steady_clock::now();
            body_inst_s += std::chrono::duration<double>(t_body_inst1 - t_body_inst0).count();
            body_node_s += std::chrono::duration<double>(t_body_node1 - t_body_inst1).count();
            body_neg_s += std::chrono::duration<double>(t_body_neg1 - t_body_neg0).count();
            body_atoms++;
        }
        for (size_t aggregateIndex = 0; aggregateIndex < rule->getAggregates().size(); ++aggregateIndex) {
            const auto t_body_inst0 = std::chrono::steady_clock::now();
            auto aggNode = buildAggregateSumNode(
                    ruleApp.ruleId, aggregateIndex, headNode, rule->getAggregates()[aggregateIndex],
                    vars, ruleApp.varValuesPure, ruleVarSet);
            const auto t_body_inst1 = std::chrono::steady_clock::now();
            assert(aggNode != nullptr && "Aggregate replay failed to reconstruct target sum");
            bodyNodes.push_back(aggNode);
            const auto t_body_node1 = std::chrono::steady_clock::now();
            bodyNegations.push_back(false);
            const auto t_body_neg1 = std::chrono::steady_clock::now();
            body_inst_s += std::chrono::duration<double>(t_body_inst1 - t_body_inst0).count();
            body_node_s += std::chrono::duration<double>(t_body_node1 - t_body_inst1).count();
            body_neg_s += std::chrono::duration<double>(t_body_neg1 - t_body_node1).count();
            body_atoms++;
        }
//        std::cout << "creating hyperedge from ruleApp: " << ruleApp.ruleId << std::endl;
//        for (size_t i = 0; i < bodyNodes.size(); ++i) {
//            std::cout << "bodyNode: " << bodyNodes[i]->toString() << std::endl;
//            std::cout << "isNegated: " << bodyNegations[i] << std::endl;
//        }
        const auto t_edge0 = std::chrono::steady_clock::now();
        auto newEdge = createHyperedge(bodyNodes, headNode, rule, bodyNegations, ruleApp);
        const auto t_edge1 = std::chrono::steady_clock::now();

        // Add the new edge to the map.
        double insert_total_s = 0.0;
        {
            const auto t0 = std::chrono::steady_clock::now();
            std::string key = createEdgeKey(ruleApp.ruleId, vars, ruleApp.varValuesPure);
            const auto t1 = std::chrono::steady_clock::now();
            edgeKeyToEdgeMap[key] = newEdge;
            const auto t2 = std::chrono::steady_clock::now();
            edgeLookupTiming.insert_calls++;
            edgeLookupTiming.insert_key_s += std::chrono::duration<double>(t1 - t0).count();
            edgeLookupTiming.insert_map_s += std::chrono::duration<double>(t2 - t1).count();
            insert_total_s = std::chrono::duration<double>(t2 - t0).count();
        }

        const auto t_end = std::chrono::steady_clock::now();
        edgeBuildTiming.calls++;
        edgeBuildTiming.body_atoms += body_atoms;
        edgeBuildTiming.total_s += std::chrono::duration<double>(t_end - t_begin).count();
        edgeBuildTiming.rule_lookup_s += std::chrono::duration<double>(t_rule1 - t_rule0).count();
        edgeBuildTiming.get_vars_s += std::chrono::duration<double>(t_vars1 - t_vars0).count();
        edgeBuildTiming.find_existing_s += std::chrono::duration<double>(t_find1 - t_find0).count();
        edgeBuildTiming.head_instantiate_s += std::chrono::duration<double>(t_head_inst1 - t_head_inst0).count();
        edgeBuildTiming.head_node_s += std::chrono::duration<double>(t_head_node1 - t_head_node0).count();
        edgeBuildTiming.body_instantiate_s += body_inst_s;
        edgeBuildTiming.body_node_s += body_node_s;
        edgeBuildTiming.body_neg_s += body_neg_s;
        edgeBuildTiming.create_edge_s += std::chrono::duration<double>(t_edge1 - t_edge0).count();
        edgeBuildTiming.insert_total_s += insert_total_s;

        return newEdge;
    }

    NodePtr findNode(const UntypedTuple& tuple) const {
        auto it = tupleToNodeMap.find(tuple);
        if (it != tupleToNodeMap.end()) {
            return it->second;
        }
        return nullptr;
    }

    EdgePtr findHyperedge(souffle::RamDomain ruleId,
                         const std::vector<std::string>& vars,
                            const std::vector<souffle::RamDomain>& values) const {
        if (DerivationGraphViewInterface::isDumpStatsEnabled()) {
            const auto t0 = std::chrono::steady_clock::now();
            std::string key = createEdgeKey(ruleId, vars, values);
            const auto t1 = std::chrono::steady_clock::now();
            auto it = edgeKeyToEdgeMap.find(key);
            const auto t2 = std::chrono::steady_clock::now();
            edgeLookupTiming.find_calls++;
            edgeLookupTiming.find_key_s += std::chrono::duration<double>(t1 - t0).count();
            edgeLookupTiming.find_map_s += std::chrono::duration<double>(t2 - t1).count();
            if (it != edgeKeyToEdgeMap.end()) {
                return it->second;
            }
            return nullptr;
        }
        std::string key = createEdgeKey(ruleId, vars, values);
        auto it = edgeKeyToEdgeMap.find(key);
        if (it != edgeKeyToEdgeMap.end()) {
            return it->second;
        }
        return nullptr;
    }

    EdgePtr findHyperedgeFromRuleApp(const RuleApplication& ruleApp, const std::vector<std::string>& vars) const {
        return findHyperedge(ruleApp.ruleId, vars, ruleApp.varValuesPure);
    }

    const std::unordered_set<NodePtr>& getNodes() const { return nodes; }
    const std::unordered_set<EdgePtr>& getEdges() const { return edges; }

    static DerivationGraph* createFrom(const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& ruleApps, const RuleManager& ruleManager, const QueryManager& queryManager, const std::unordered_map<UntypedTuple, double>& fact_prob = {}, const std::vector<std::pair<UntypedTuple,bool>>& evidences = {})  {
        if (DerivationGraphViewInterface::isVerboseEnabled()) {
            std::cout << "[Debug] Enter DerivationGraph::createFrom()" << std::endl;
        }
        FunctionTimer timer(" creating derivation graph ");
        if (DerivationGraphViewInterface::isDumpStatsEnabled()) {
            resetEdgeLookupTiming();
            resetEdgeBuildTiming();
        }
        auto graph = new DerivationGraph(&ruleManager);
        {
            FunctionTimer scopeTimer("create graph: init fact nodes");
            for (const auto& [tuple, prob] : fact_prob) {
                graph->existingTuples.insert(tuple);
                auto node = graph->createNode(tuple);  // actually "find node" here
                node->setProbability(prob);
                node->isFact = true;
                node->setOriginalFact(true);
            }
        }
        for (const auto& [tuple, _] : ruleApps) {
            graph->existingTuples.insert(tuple);
        }
        graph->buildAggregateTupleIndices(ruleApps, fact_prob, ruleManager);
        {
            FunctionTimer scopeTimer("create graph: build rule apps");
            for (const auto& [tuple, ruleAppSet] : ruleApps) {
                auto node = graph->createNode(tuple);
                for (const auto& ruleApp : *ruleAppSet) {
                    auto edge = graph->createHyperedgeFromRuleApp(ruleApp, ruleManager);
                }
            }
        }
        if (DerivationGraphViewInterface::isVerboseEnabled()) {
            std::cout << "[Debug] Current nodes in graph:" << std::endl;
        }
        {
            FunctionTimer scopeTimer("create graph: attach evidence");
            graph->attachEvidence(evidences);
        }
        if (DerivationGraphViewInterface::isDumpStatsEnabled()) {
            dumpEdgeLookupTiming("create graph");
            const EdgeBuildTiming& t = edgeBuildTiming;
            const double avg_ms = t.calls ? (t.total_s * 1000.0 / t.calls) : 0.0;
            const double avg_body = t.calls ? (static_cast<double>(t.body_atoms) / t.calls) : 0.0;
            std::cout << "[timing] create graph edgeBuild: calls=" << t.calls
                      << " hits=" << t.existing_hits
                      << " body_atoms=" << t.body_atoms
                      << " avg_body=" << avg_body
                      << " total_s=" << t.total_s
                      << " avg_ms=" << avg_ms
                      << " rule_s=" << t.rule_lookup_s
                      << " vars_s=" << t.get_vars_s
                      << " find_s=" << t.find_existing_s
                      << " head_inst_s=" << t.head_instantiate_s
                      << " head_node_s=" << t.head_node_s
                      << " body_inst_s=" << t.body_instantiate_s
                      << " body_node_s=" << t.body_node_s
                      << " body_neg_s=" << t.body_neg_s
                      << " edge_s=" << t.create_edge_s
                      << " insert_s=" << t.insert_total_s
                      << std::endl;
        }
        if (DerivationGraphViewInterface::isVerboseEnabled()) {
            for (const auto& node : graph->getNodes()) {
                std::cout << "  " << node->getTuple().toString() << std::endl;
            }
        }

        return graph;
    }

    SubgraphView prune(const std::vector<souffle::Relation*>& outputRelations) {
        std::vector<std::string> outputRelationNames;
        for (const auto* rel : outputRelations) {
            outputRelationNames.push_back(rel->getName());
        }
        return prune(outputRelationNames);
    }

    SubgraphView prune(const std::vector<std::string>& outputRelations) {
        const bool profileEnabled = fcProfileEnabled;
        using Clock = std::chrono::steady_clock;
        auto toMs = [](Clock::time_point start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        };
        auto totalStart = Clock::now();
        double initMs = 0.0;
        double bfsMs = 0.0;
        double filterNodeMs = 0.0;
        double filterEdgeMs = 0.0;
        double updateEdgesMs = 0.0;
        double cleanupMs = 0.0;
        std::unordered_set<std::string> outputRelationNames(outputRelations.begin(), outputRelations.end());

        // Mark reachable nodes and edges.
        std::unordered_set<NodePtr> reachableNodes;
        std::unordered_set<EdgePtr> reachableEdges;
        std::queue<NodePtr> workQueue;

        // Initialize: start from all output relation nodes.
        {
            auto t0 = Clock::now();
            for (const auto& node : nodes) {
                if (outputRelationNames.count(node->getTuple().relation_name) > 0) {
                    reachableNodes.insert(node);
                    workQueue.push(node);
                    node->setQuery();
                }
            }
            if (profileEnabled) {
                initMs = toMs(t0);
            }
        }

        // Reverse BFS traversal.
        {
            auto t0 = Clock::now();
            while (!workQueue.empty()) {
                NodePtr current = workQueue.front();
                workQueue.pop();
                if (current->isFact) {
                    continue;  // skip input fact nodes
                }
                for (const auto& edge : current->getIncomingEdges()) {
                    if (edge->hasSelfDependency()) {
                        continue;  // if the edge reports self-dependency, then it is redundant
                    }
                    reachableEdges.insert(edge);
                    for (const auto& inputNode : edge->getInputs()) {
                        if (reachableNodes.insert(inputNode).second) {
                            workQueue.push(inputNode);
                        }
                    }
                }
            }
            if (profileEnabled) {
                bfsMs = toMs(t0);
            }
        }

        // Filter nodes and edges.
        std::unordered_set<NodePtr> newNodes;
        {
            auto t0 = Clock::now();
            for (const auto& node : nodes) {
                if (reachableNodes.count(node)){
                    newNodes.insert(node);
                if (!reachableNodes.count(node)) {
                    node->setQuery();
                }
                }
            }
            if (profileEnabled) {
                filterNodeMs = toMs(t0);
            }
        }

        std::unordered_set<EdgePtr> newEdges;
        {
            auto t0 = Clock::now();
            for (const auto& edge : edges) {
                if (reachableEdges.count(edge)) {
                    newEdges.insert(edge);
                }
            }
            if (profileEnabled) {
                filterEdgeMs = toMs(t0);
            }
        }

        {
            auto t0 = Clock::now();
            for (const auto& node : newNodes) {
                std::vector<EdgePtr> newIncomingEdges;
                std::vector<EdgePtr> newOutgoingEdges;
                for (const auto& edge : node->getIncomingEdges()) {
                    if (reachableEdges.count(edge)) {
                        newIncomingEdges.push_back(edge);
                    }
                }
            for (const auto& edge : node->getOutgoingEdges()) {
                if (reachableEdges.count(edge)) {
                    newOutgoingEdges.push_back(edge);
                }
            }
        node->incomingEdges = std::move(newIncomingEdges);
        node->outgoingEdges = std::move(newOutgoingEdges);
    }
            if (profileEnabled) {
                updateEdgesMs = toMs(t0);
            }
        }

        // Cleanup self dependencies after reachability pruning.
        {
            auto t0 = Clock::now();
            removeSelfLoopEdges(newNodes, newEdges);
            if (profileEnabled) {
                cleanupMs = toMs(t0);
            }
        }
        if (profileEnabled) {
            const double totalMs = toMs(totalStart);
            const size_t liveNodeCount = newNodes.size();
            const size_t liveEdgeCount = newEdges.size();
            std::cout << "[profile] stage=PRUNING prune_ms=" << totalMs
                      << " init_ms=" << initMs
                      << " bfs_ms=" << bfsMs
                      << " filter_nodes_ms=" << filterNodeMs
                      << " filter_edges_ms=" << filterEdgeMs
                      << " update_edges_ms=" << updateEdgesMs
                      << " cleanup_ms=" << cleanupMs
                      << " live_nodes=" << liveNodeCount
                      << " live_edges=" << liveEdgeCount
                      << std::endl;
        }

        return SubgraphView(std::move(newNodes), std::move(newEdges));
    }

    void setRuleManager(RuleManager* rm) {
        ruleManager = rm;
    }

    // this one does not check if the edge already exists
    EdgePtr createHyperedge(const std::vector<NodePtr>& inputs, NodePtr output, const Rule* rule, const std::vector<bool>& bodyNegations, RuleApplication ruleApp = naiveRuleApplication) {
        if (inputs.empty() || (rule != nullptr && rule->isFact())) {
            // we do not create edges with no inputs or edges for fact rules
            return nullptr;
        }
        auto edge = std::shared_ptr<Hyperedge>(new Hyperedge(inputs, output, nextEdgeId++, rule, bodyNegations, ruleApp));
        for (const auto& input : inputs) {
            assert (input != nullptr);
            input->addOutgoingEdge(edge);
        }
        assert (output != nullptr);
        output->addIncomingEdge(edge);

        edges.insert(edge);

        return edge;
    }

    // Test helper; this one does not check if the edge already exists.
    EdgePtr createHyperedge(const std::vector<NodePtr>& inputs, NodePtr output, RuleApplication ruleApp = naiveRuleApplication) {
        auto edge = std::shared_ptr<Hyperedge>(new Hyperedge(inputs, output, nextEdgeId++, ruleApp));

        for (const auto& input : inputs) {
            input->addOutgoingEdge(edge);
        }
        output->addIncomingEdge(edge);

        edges.insert(edge);
        return edge;
    }

protected:
//    std::vector<NodePtr> nodes;
//    std::vector<EdgePtr> edges;
    std::unordered_set<NodePtr> nodes;
    std::unordered_set<EdgePtr> edges;
    size_t nextNodeId;
    size_t nextEdgeId;
    const RuleManager* ruleManager;
    static inline bool mergeBiImpEnabled = false;
    static inline bool pruneExtraEnabled = false;
    bool biImpMerged = false;
    // map original node id to its current representative after merges
    std::unordered_map<size_t, NodePtr> nodeRepMap;

    // Map tuples to nodes.
    std::map<UntypedTuple, NodePtr> tupleToNodeMap;
    std::unordered_set<UntypedTuple> existingTuples;

    // Map edge keys to edges.
    std::map<std::string, EdgePtr> edgeKeyToEdgeMap;
    std::unordered_map<std::size_t, std::vector<AggregateTupleIndex>> aggregateTupleIndices;
    std::vector<std::pair<UntypedTuple, bool>> evidences;

    // Create a unique key for an edge.
    std::string createEdgeKey(souffle::RamDomain ruleId,
                             const std::vector<std::string>& vars,
                             const std::vector<souffle::RamDomain>& values) const {
        std::stringstream ss;
        ss << ruleId << "_";

        // Sort by variable name to ensure a stable key.
        std::vector<std::string> varNames = vars;
        std::sort(varNames.begin(), varNames.end());

        for (size_t i = 0; i < varNames.size(); ++i) {
            ss << varNames[i] << ":" << values[i] << ";";
        }

        return ss.str();
    }

    NodePtr findRepresentative(const NodePtr& node) const {
        if (!node) return node;
        auto it = nodeRepMap.find(node->getId());
        if (it != nodeRepMap.end()) {
            return it->second;
        }
        return node;
    }

    void setRepresentative(const NodePtr& node, const NodePtr& rep) {
        if (node) {
            nodeRepMap[node->getId()] = rep;
        }
    }

    void mergeBiImpEquivalences(
            std::unordered_set<NodePtr>& liveNodes, std::unordered_set<EdgePtr>& liveEdges);
    void pruneOutputlessComponents(std::unordered_set<NodePtr>& nodes,
            std::unordered_set<EdgePtr>& edges,
            std::unordered_set<NodePtr>* removedNodes = nullptr,
            std::unordered_set<EdgePtr>* removedEdges = nullptr);
    void removeSelfLoopEdges(std::unordered_set<NodePtr>& liveNodes, std::unordered_set<EdgePtr>& liveEdges);

    static void resetEdgeLookupTiming() {
        edgeLookupTiming = EdgeLookupTiming();
    }

    static void resetEdgeBuildTiming() {
        edgeBuildTiming = EdgeBuildTiming();
    }

    static void dumpEdgeLookupTiming(const char* label) {
        const EdgeLookupTiming& t = edgeLookupTiming;
        const double find_avg_key_ms = t.find_calls ? (t.find_key_s * 1000.0 / t.find_calls) : 0.0;
        const double find_avg_map_ms = t.find_calls ? (t.find_map_s * 1000.0 / t.find_calls) : 0.0;
        const double insert_avg_key_ms = t.insert_calls ? (t.insert_key_s * 1000.0 / t.insert_calls) : 0.0;
        const double insert_avg_map_ms = t.insert_calls ? (t.insert_map_s * 1000.0 / t.insert_calls) : 0.0;
        std::cout << "[timing] " << label
                  << " edgeKey lookup: find_calls=" << t.find_calls
                  << " find_key_s=" << t.find_key_s
                  << " find_map_s=" << t.find_map_s
                  << " find_avg_key_ms=" << find_avg_key_ms
                  << " find_avg_map_ms=" << find_avg_map_ms
                  << " insert_calls=" << t.insert_calls
                  << " insert_key_s=" << t.insert_key_s
                  << " insert_map_s=" << t.insert_map_s
                  << " insert_avg_key_ms=" << insert_avg_key_ms
                  << " insert_avg_map_ms=" << insert_avg_map_ms
                  << std::endl;
    }

    static inline EdgeLookupTiming edgeLookupTiming;
    static inline EdgeBuildTiming edgeBuildTiming;

};

void Node::addIncomingEdge(EdgePtr edge) {
    assert(edge->getOutput().get() == this);
    incomingEdges.push_back(edge);
}

void Node::addOutgoingEdge(EdgePtr edge) {
    const auto& inputs = edge->getInputs();
    assert(std::find_if(inputs.begin(), inputs.end(), [this](const NodePtr& node) {
        return node.get() == this;
    }) != inputs.end());
    outgoingEdges.push_back(edge);
}

class WorkingDerivationGraph : public DerivationGraph, virtual public WorkingDerivationGraphViewInterface {
public:
    struct EventAliasMutationStats {
        size_t inputReplacements = 0;
        size_t duplicateInputsRemoved = 0;
        size_t activeInputReplacements = 0;
        size_t activeDuplicateInputsRemoved = 0;
        size_t removedNodes = 0;
        size_t removedEdges = 0;
        size_t removedOwnerEdges = 0;
    };
    // Constructors
    WorkingDerivationGraph() : DerivationGraph() {}
    WorkingDerivationGraph(const RuleManager* rm) : DerivationGraph(rm) {}
    WorkingSubgraphView prune(const std::vector<souffle::Relation*>& outputRelations,
            BackwardTraversalObserver* observer = nullptr);
    WorkingSubgraphView prune(const std::vector<std::string>& outputRelations,
            BackwardTraversalObserver* observer = nullptr);

    // Consume an event-equality certificate. Roots must be surviving active
    // nodes and every alias must have no other active source. Update the owner
    // as well as the view so a later owner prune cannot revive alias definitions.
    EventAliasMutationStats applyEventAliases(WorkingSubgraphView& view,
            const std::vector<std::pair<NodePtr, NodePtr>>& aliases);

    struct RewriteRetirementStats {
        size_t removedNodes = 0;
        size_t removedEdges = 0;
        size_t removedOwnerNodes = 0;
        size_t removedOwnerEdges = 0;
        size_t ownerEdgesExamined = 0;
        size_t touchedOwnerNodes = 0;
        size_t terminalViewCommits = 0;
    };
    enum class OwnerCommitMode { CompleteOwner, TerminalView, FinalView };
    struct RewriteEdgeSpec {
        std::vector<NodePtr> inputs;
        NodePtr output;
        double probability = 1.0;
        std::vector<bool> bodyNegations;
        std::vector<SupportToken> supportTokens;
    };
    struct RewriteViewCommitResult {
        RewriteRetirementStats stats;
        std::vector<EdgePtr> insertedEdges;
    };
    // Full-only rewrites may retire definitions and intermediate/output nodes.
    // Every owned edge incident to a retired node must be retired in this batch.
    RewriteRetirementStats retireRewriteObjects(WorkingSubgraphView& view,
            const std::vector<NodePtr>& retiredNodes, const std::vector<EdgePtr>& retiredEdges);
    // Consume a plan over the original active view in one physical commit.
    // Insertions are staged without changing ownership, adjacency or IDs.
    // The complete-view certificate permits retiring owner-only SISO history;
    // ordinary callers preserve that history and require full owned closure.
    // TerminalView retains unrelated history for the final full-inference view.
    // It requires a complete view and forbids subsequent owner pruning.
    // FinalView selects complete ownership for dense node retirements, otherwise
    // it uses TerminalView. Both final modes require a complete active view.
    RewriteViewCommitResult commitRewriteView(WorkingSubgraphView& view,
            const std::vector<NodePtr>& retiredActiveNodes, const std::vector<EdgePtr>& retiredActiveEdges,
            const std::vector<RewriteEdgeSpec>& insertedEdgeSpecs, bool completeActiveDerivations = false,
            OwnerCommitMode mode = OwnerCommitMode::CompleteOwner);
    // Commit a complete residual view after SISO. Historical owner definitions
    // must not revive during later owner pruning or appear as factor consumers.
    RewriteRetirementStats retainRewriteView(WorkingSubgraphView& view);

    // Restore a certified original name after an implicit rewrite replaces the
    // graph owner. The caller resolves its representative in this new owner;
    // registering the name does not rewrite any event or graph endpoint.
    void bindEventAliasTuple(const UntypedTuple& aliasTuple, const NodePtr& currentRoot) {
        if (!currentRoot || !nodes.count(currentRoot)) {
            throw std::logic_error("Event-alias binding requires an owned representative");
        }
        tupleToNodeMap[aliasTuple] = currentRoot;
        existingTuples.insert(aliasTuple);
    }

    static WorkingDerivationGraph* createFrom(const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& ruleApps, const RuleManager& ruleManager, const QueryManager& queryManager, const std::unordered_map<UntypedTuple, double>& fact_prob = {}, const std::vector<std::pair<UntypedTuple,bool>>& evidences = {}) {
        FunctionTimer timer(" creating derivation graph ");
        if (DerivationGraphViewInterface::isDumpStatsEnabled()) {
            resetEdgeLookupTiming();
            resetEdgeBuildTiming();
        }
        auto graph = new WorkingDerivationGraph(&ruleManager);
        {
            FunctionTimer scopeTimer("create graph: init fact nodes");
            for (const auto& [tuple, prob] : fact_prob) {
                graph->existingTuples.insert(tuple);
                auto node = graph->createNode(tuple);  // actually "find node" here
                node->setProbability(prob);
                node->isFact = true;
                node->setOriginalFact(true);
            }
        }
        for (const auto& [tuple, _] : ruleApps) {
            graph->existingTuples.insert(tuple);
        }
        graph->buildAggregateTupleIndices(ruleApps, fact_prob, ruleManager);
        {
            FunctionTimer scopeTimer("create graph: build rule apps");
            for (const auto& [tuple, ruleAppSet] : ruleApps) {
                auto node = graph->createNode(tuple, 0.0);
//            if (node->isFact) {
//                std::cout << "Found fact node: " << node->getTuple().toString() << std::endl;
//                continue;  // skip fact nodes currently
//            }
                for (const auto& ruleApp : *ruleAppSet) {
                    auto edge = graph->createHyperedgeFromRuleApp(ruleApp, ruleManager);
                }
            }
        }

        {
            FunctionTimer scopeTimer("create graph: attach queries");
            for (auto& node : graph->getNodes()) {
                graph->createQuery(node, queryManager);
            }
        }
        {
            FunctionTimer scopeTimer("create graph: attach evidence");
            graph->attachEvidence(evidences);
        }
        if (DerivationGraphViewInterface::isDumpStatsEnabled()) {
            dumpEdgeLookupTiming("create graph");
            const EdgeBuildTiming& t = edgeBuildTiming;
            const double avg_ms = t.calls ? (t.total_s * 1000.0 / t.calls) : 0.0;
            const double avg_body = t.calls ? (static_cast<double>(t.body_atoms) / t.calls) : 0.0;
            std::cout << "[timing] create graph edgeBuild: calls=" << t.calls
                      << " hits=" << t.existing_hits
                      << " body_atoms=" << t.body_atoms
                      << " avg_body=" << avg_body
                      << " total_s=" << t.total_s
                      << " avg_ms=" << avg_ms
                      << " rule_s=" << t.rule_lookup_s
                      << " vars_s=" << t.get_vars_s
                      << " find_s=" << t.find_existing_s
                      << " head_inst_s=" << t.head_instantiate_s
                      << " head_node_s=" << t.head_node_s
                      << " body_inst_s=" << t.body_instantiate_s
                      << " body_node_s=" << t.body_node_s
                      << " body_neg_s=" << t.body_neg_s
                      << " edge_s=" << t.create_edge_s
                      << " insert_s=" << t.insert_total_s
                      << std::endl;
        }
        return graph;
    }
private:
    bool terminalViewCommitted = false;
};

inline WorkingDerivationGraph::EventAliasMutationStats WorkingDerivationGraph::applyEventAliases(
        WorkingSubgraphView& view, const std::vector<std::pair<NodePtr, NodePtr>>& aliases) {
    EventAliasMutationStats stats;
    if (aliases.empty()) return stats;
    std::unordered_map<NodePtr, NodePtr> replacements;
    replacements.reserve(aliases.size());
    for (const auto& [alias, root] : aliases) {
        if (!alias || !root || alias == root || !nodes.count(alias) || !nodes.count(root) ||
                !view.getNodes().count(alias) || !view.getNodes().count(root) ||
                !replacements.emplace(alias, root).second) {
            throw std::logic_error("Invalid deterministic event-alias certificate");
        }
    }
    for (const auto& [alias, root] : aliases) {
        if (replacements.count(root) || (alias->hasEvidence() && root->hasEvidence() &&
                alias->getEvidenceValue() != root->getEvidenceValue())) {
            throw std::logic_error("Noncanonical deterministic event-alias certificate");
        }
    }

    std::unordered_set<EdgePtr> retiredSources, consumers;
    retiredSources.reserve(aliases.size());
    consumers.reserve(aliases.size());
    for (const auto& [alias, root] : aliases) {
        for (const auto& edge : alias->getIncomingEdges()) {
            if (edge && edge->getOutputRef() == alias) retiredSources.insert(edge);
        }
    }
    // Sources being retired do not need endpoint mutation or consumer ordering.
    // Copy chains otherwise put nearly every removed definition in both sets.
    for (const auto& [alias, root] : aliases) {
        for (const auto& edge : alias->getOutgoingEdges()) {
            if (edge && !retiredSources.count(edge)) consumers.insert(edge);
        }
    }
    std::vector<EdgePtr> orderedConsumers(consumers.begin(), consumers.end());
    std::sort(orderedConsumers.begin(), orderedConsumers.end(), [](const EdgePtr& a, const EdgePtr& b) {
        if (a->getId() != b->getId()) return a->getId() < b->getId();
        return std::less<const Hyperedge*>{}(a.get(), b.get());
    });
    std::unordered_set<EdgePtr> changedEdges;
    changedEdges.reserve(retiredSources.size() + consumers.size());
    changedEdges.insert(retiredSources.begin(), retiredSources.end());
    std::unordered_set<NodePtr> touchedInputs;
    touchedInputs.reserve(aliases.size() + consumers.size());
    for (const auto& [alias, root] : aliases) touchedInputs.insert(root);
    for (const auto& edge : retiredSources) {
        touchedInputs.insert(edge->getInputs().begin(), edge->getInputs().end());
    }
    orderedConsumers.erase(std::remove_if(orderedConsumers.begin(), orderedConsumers.end(),
            [&](const EdgePtr& edge) {
                const auto& inputs = edge->getInputs();
                const bool changed = std::any_of(inputs.begin(), inputs.end(),
                        [&](const NodePtr& input) { return replacements.count(input) != 0; });
                if (changed) {
                    changedEdges.insert(edge);
                    touchedInputs.insert(inputs.begin(), inputs.end());
                }
                return !changed;
            }), orderedConsumers.end());
    // Filter each affected node once. Per-edge filtering would be quadratic
    // when many aliases share one high-fanout representative.
    for (const auto& input : touchedInputs) {
        if (!input) continue;
        auto& outgoing = input->getOutgoingEdges();
        outgoing.erase(std::remove_if(outgoing.begin(), outgoing.end(),
                [&](const EdgePtr& edge) { return changedEdges.count(edge) != 0; }), outgoing.end());
    }
    std::unordered_map<const Node*, unsigned char> longBodySigns;
    for (const auto& edge : orderedConsumers) {
        const bool active = view.getEdges().count(edge) != 0;
        const auto [changed, duplicates] = edge->replaceInputsAndDeduplicate(replacements, longBodySigns);
        stats.inputReplacements += changed;
        stats.duplicateInputsRemoved += duplicates;
        if (active) stats.activeInputReplacements += changed;
        if (active) stats.activeDuplicateInputsRemoved += duplicates;
        const auto& inputs = edge->getInputs();
        // Rebuild exactly one raw reference per surviving body occurrence,
        // including opposite-sign uses of the same event.
        for (const auto& input : inputs) {
            if (input) input->addOutgoingEdge(edge);
        }
    }
    for (const auto& edge : retiredSources) {
        // Every retired source has an alias head; those incoming vectors are
        // cleared once below rather than filtered once per definition.
        stats.removedEdges += view.mutableEdges().erase(edge);
        stats.removedOwnerEdges += edges.erase(edge);
        edge->pruned = true;
    }
    // Existing tuple/representative bindings may already point at an alias.
    // Redirect those bindings too; each original output name remains resolvable.
    for (auto& [tuple, node] : tupleToNodeMap) {
        const auto replacement = replacements.find(node);
        if (replacement != replacements.end()) node = replacement->second;
    }
    for (auto& [id, node] : nodeRepMap) {
        const auto replacement = replacements.find(node);
        if (replacement != replacements.end()) node = replacement->second;
    }
    nodeRepMap.reserve(nodeRepMap.size() + aliases.size());
    for (auto it = edgeKeyToEdgeMap.begin(); it != edgeKeyToEdgeMap.end();) {
        if (retiredSources.count(it->second)) it = edgeKeyToEdgeMap.erase(it);
        else ++it;
    }
    for (const auto& [alias, root] : aliases) {
        root->needOutput = root->needOutput || alias->needOutput;
        root->isQuery = root->isQuery || alias->isQuery;
        if (alias->hasEvidence()) root->setEvidence(alias->getEvidenceValue());
        setRepresentative(alias, root);
        // Its original tuple and any earlier names were redirected in the
        // single tuple-map traversal above; avoid another ordered lookup here.
        alias->getIncomingEdges().clear();
        alias->getOutgoingEdges().clear();
        alias->pruned = true;
        stats.removedNodes += view.mutableNodes().erase(alias);
        nodes.erase(alias);
    }
    std::vector<NodePtr> evidenceNodes;
    for (const auto& node : view.getNodes()) {
        if (node->hasEvidence()) evidenceNodes.push_back(node);
    }
    // Preserve active membership exactly; do not reconstruct it from raw owner
    // adjacency, which can also contain retired rewrite sources.
    view = WorkingSubgraphView(std::move(view.mutableNodes()), std::move(view.mutableEdges()),
            std::move(evidenceNodes), WorkingSubgraphView::EvidenceRoots::Complete);
    invalidateCaches();
    return stats;
}

inline WorkingDerivationGraph::RewriteRetirementStats WorkingDerivationGraph::retireRewriteObjects(
        WorkingSubgraphView& view, const std::vector<NodePtr>& retiredNodes,
        const std::vector<EdgePtr>& retiredEdges) {
    RewriteRetirementStats stats;
    if (retiredNodes.empty() && retiredEdges.empty()) return stats;
    const std::unordered_set<NodePtr> nodeSet(retiredNodes.begin(), retiredNodes.end());
    const std::unordered_set<EdgePtr> edgeSet(retiredEdges.begin(), retiredEdges.end());
    if (nodeSet.size() != retiredNodes.size() || edgeSet.size() != retiredEdges.size()) {
        throw std::logic_error("Duplicate full-rewrite retirement object");
    }
    for (const auto& edge : retiredEdges) {
        if (!edge || !edges.count(edge)) throw std::logic_error("Retiring an unowned rewrite edge");
    }
    for (const auto& node : retiredNodes) {
        if (!node || !nodes.count(node) || node->hasEvidence()) {
            throw std::logic_error("Retiring an unowned or observed rewrite node");
        }
        for (const auto& edge : node->getIncomingEdges()) {
            if (edges.count(edge) && !edgeSet.count(edge)) {
                throw std::logic_error("Retired rewrite node still has an owned source");
            }
        }
        for (const auto& edge : node->getOutgoingEdges()) {
            if (edges.count(edge) && !edgeSet.count(edge)) {
                throw std::logic_error("Retired rewrite node still has an owned consumer");
            }
        }
    }
    std::unordered_set<NodePtr> touched(nodeSet);
    touched.reserve(nodeSet.size() + retiredEdges.size());
    for (const auto& edge : retiredEdges) {
        touched.insert(edge->getOutput());
        touched.insert(edge->getInputs().begin(), edge->getInputs().end());
    }
    for (const auto& node : touched) {
        if (!node) continue;
        auto& incoming = node->getIncomingEdges();
        incoming.erase(std::remove_if(incoming.begin(), incoming.end(),
                [&](const EdgePtr& edge) { return edgeSet.count(edge); }), incoming.end());
        auto& outgoing = node->getOutgoingEdges();
        outgoing.erase(std::remove_if(outgoing.begin(), outgoing.end(),
                [&](const EdgePtr& edge) { return edgeSet.count(edge); }), outgoing.end());
    }
    for (const auto& edge : retiredEdges) {
        stats.removedEdges += view.mutableEdges().erase(edge);
        stats.removedOwnerEdges += edges.erase(edge);
        edge->pruned = true;
    }
    for (auto it = tupleToNodeMap.begin(); it != tupleToNodeMap.end();) {
        if (nodeSet.count(it->second)) {
            existingTuples.erase(it->first);
            it = tupleToNodeMap.erase(it);
        } else ++it;
    }
    for (auto it = nodeRepMap.begin(); it != nodeRepMap.end();) {
        if (nodeSet.count(it->second)) it = nodeRepMap.erase(it);
        else ++it;
    }
    for (auto it = edgeKeyToEdgeMap.begin(); it != edgeKeyToEdgeMap.end();) {
        if (edgeSet.count(it->second)) it = edgeKeyToEdgeMap.erase(it);
        else ++it;
    }
    for (const auto& node : retiredNodes) {
        node->getIncomingEdges().clear();
        node->getOutgoingEdges().clear();
        node->pruned = true;
        stats.removedNodes += view.mutableNodes().erase(node);
        stats.removedOwnerNodes += nodes.erase(node);
    }
    view.invalidateCaches();
    invalidateCaches();
    return stats;
}

inline WorkingDerivationGraph::RewriteRetirementStats WorkingDerivationGraph::retainRewriteView(
        WorkingSubgraphView& view) {
    return commitRewriteView(view, {}, {}, {}, true).stats;
}

inline WorkingDerivationGraph::RewriteViewCommitResult WorkingDerivationGraph::commitRewriteView(
        WorkingSubgraphView& view, const std::vector<NodePtr>& retiredActiveNodes,
        const std::vector<EdgePtr>& retiredActiveEdges, const std::vector<RewriteEdgeSpec>& insertedEdgeSpecs,
        bool completeActiveDerivations, OwnerCommitMode mode) {
    if (mode == OwnerCommitMode::FinalView) {
        if (!completeActiveDerivations) {
            throw std::logic_error("Final rewrite commit requires a complete active view");
        }
        mode = retiredActiveNodes.size() > nodes.size() / 4 ?
                OwnerCommitMode::CompleteOwner : OwnerCommitMode::TerminalView;
    }
    if (mode == OwnerCommitMode::TerminalView && !completeActiveDerivations) {
        throw std::logic_error("Terminal rewrite commit requires a complete active view");
    }
    const auto& originalNodes = view.getNodes();
    const auto& originalEdges = view.getEdges();
    if (nodes.count(NodePtr{}) || edges.count(EdgePtr{})) {
        throw std::logic_error("Residual rewrite owner has a null object");
    }
    for (const auto& node : originalNodes) {
        if (!node || !nodes.count(node)) throw std::logic_error("Residual rewrite view has an unowned node");
    }
    // All allocation and plan validation precede the first adjacency change.
    // Erasing from these local sets also detects duplicate retirement objects.
    std::unordered_set<NodePtr> finalNodes;
    finalNodes.reserve(originalNodes.size());
    finalNodes.insert(originalNodes.begin(), originalNodes.end());
    for (const auto& node : retiredActiveNodes) {
        if (!node || node->hasEvidence() || finalNodes.erase(node) != 1) {
            throw std::logic_error("Invalid or duplicate active rewrite node retirement");
        }
    }
    std::unordered_set<EdgePtr> finalEdges;
    finalEdges.reserve(originalEdges.size() + insertedEdgeSpecs.size());
    finalEdges.insert(originalEdges.begin(), originalEdges.end());
    for (const auto& edge : retiredActiveEdges) {
        if (!edge || finalEdges.erase(edge) != 1) {
            throw std::logic_error("Invalid or duplicate active rewrite edge retirement");
        }
    }
    // Validate original rows once. A surviving row must reference final nodes;
    // a retired row only needs the original complete view. New rows are checked
    // below as specs, before allocation, and need no second endpoint scan.
    for (const auto& edge : originalEdges) {
        if (!edge || !edges.count(edge) || edge->getInputs().size() != edge->getBodyNegations().size()) {
            throw std::logic_error("Residual rewrite view has an unowned edge or invalid body");
        }
        const auto& allowedNodes = finalEdges.count(edge) ? finalNodes : originalNodes;
        if (!allowedNodes.count(edge->getOutput())) {
            throw std::logic_error("Residual rewrite edge has an absent head");
        }
        for (const auto& input : edge->getInputs()) {
            if (!allowedNodes.count(input)) throw std::logic_error("Residual rewrite edge has an absent input");
        }
    }
    if (insertedEdgeSpecs.size() > std::numeric_limits<size_t>::max() - nextEdgeId) {
        throw std::logic_error("Full rewrite exhausted edge IDs");
    }
    for (const auto& spec : insertedEdgeSpecs) {
        if (!finalNodes.count(spec.output) || spec.inputs.size() != spec.bodyNegations.size() ||
                !std::isfinite(spec.probability) || spec.probability < 0.0 || spec.probability > 1.0) {
            throw std::logic_error("Invalid planned rewrite edge");
        }
        for (const auto& input : spec.inputs) {
            if (!finalNodes.count(input)) throw std::logic_error("Planned rewrite edge has an absent input");
        }
    }
    RewriteViewCommitResult result;
    result.insertedEdges.reserve(insertedEdgeSpecs.size());
    auto stagedNextEdgeId = nextEdgeId;
    for (const auto& spec : insertedEdgeSpecs) {
        auto edge = std::shared_ptr<Hyperedge>(new Hyperedge(spec.inputs, spec.output, stagedNextEdgeId++,
                nullptr, spec.bodyNegations, naiveRuleApplication));
        edge->setProbability(spec.probability);
        edge->setProbabilisticSupportTokens(spec.supportTokens);
        finalEdges.insert(edge);
        result.insertedEdges.push_back(std::move(edge));
    }
    for (const auto& node : retiredActiveNodes) {
        const auto closed = [&](const std::vector<EdgePtr>& incident) {
            for (const auto& edge : incident) {
                if (!edges.count(edge)) continue;
                if (originalEdges.count(edge)) {
                    if (finalEdges.count(edge)) return false;
                } else if (!completeActiveDerivations) return false;
            }
            return true;
        };
        if (!closed(node->getIncomingEdges()) || !closed(node->getOutgoingEdges())) {
            throw std::logic_error("Retired rewrite node still has an uncertified owned incident");
        }
    }
    for (const auto& node : view.getEvidenceNodes()) {
        if (!node || !finalNodes.count(node) || !node->hasEvidence()) {
            throw std::logic_error("Residual rewrite view has an invalid evidence root");
        }
    }

    if (mode == OwnerCommitMode::TerminalView) {
        // Unlike a complete owner synchronization, the final full-inference
        // commit only retires history that can touch a changed definition.
        // The active view remains authoritative; unrelated owner history stays
        // allocated and must never be used to reconstruct this view later.
        const std::unordered_set<NodePtr> retiredNodes(retiredActiveNodes.begin(), retiredActiveNodes.end());
        std::unordered_set<NodePtr> changedHeads;
        changedHeads.reserve(retiredActiveEdges.size() + insertedEdgeSpecs.size());
        for (const auto& edge : retiredActiveEdges) changedHeads.insert(edge->getOutput());
        for (const auto& spec : insertedEdgeSpecs) changedHeads.insert(spec.output);
        std::unordered_set<EdgePtr> retiredOwnerEdges;
        retiredOwnerEdges.reserve(retiredActiveEdges.size());
        std::unordered_set<NodePtr> touched(retiredNodes);
        touched.reserve(retiredNodes.size() + retiredActiveEdges.size() + insertedEdgeSpecs.size());
        const auto touchEdge = [&](const EdgePtr& edge) {
            touched.insert(edge->getOutput());
            touched.insert(edge->getInputs().begin(), edge->getInputs().end());
        };
        // Historical raw adjacency can omit owned edges after SISO. Inspect
        // actual endpoints once instead of trusting retiring nodes' lists.
        for (const auto& edge : edges) {
            ++result.stats.ownerEdgesExamined;
            bool retire;
            if (originalEdges.count(edge)) {
                // The common validation already proved closure of every
                // original active survivor; never retire it as history.
                retire = !finalEdges.count(edge);
            } else {
                retire = retiredNodes.count(edge->getOutput()) || changedHeads.count(edge->getOutput());
                if (!retire) {
                    for (const auto& input : edge->getInputs()) {
                        if (retiredNodes.count(input)) {
                            retire = true;
                            break;
                        }
                    }
                }
            }
            if (retire) {
                retiredOwnerEdges.insert(edge);
                touchEdge(edge);
            }
        }
        for (const auto& edge : result.insertedEdges) touchEdge(edge);
        for (const auto& [tuple, value] : evidences) {
            const auto node = findNode(tuple);
            if (!node || !finalNodes.count(node) || !node->hasEvidence() || node->getEvidenceValue() != value) {
                throw std::logic_error("Terminal rewrite view loses attached evidence");
            }
        }

        struct StagedAdjacency {
            std::vector<EdgePtr> incoming, outgoing;
        };
        std::unordered_map<NodePtr, StagedAdjacency> stagedAdjacency;
        stagedAdjacency.reserve(touched.size());
        for (const auto& node : touched) {
            if (!node) continue;
            if (node->hasEvidence() && !finalNodes.count(node)) {
                throw std::logic_error("Terminal rewrite affects an absent observed node");
            }
            if (nodes.count(node)) ++result.stats.touchedOwnerNodes;
            if (retiredNodes.count(node)) continue;
            auto& staged = stagedAdjacency.try_emplace(node).first->second;
            // Scrub unowned stale handles as well as the retired owned edges.
            // Every actual counterpart of a retired edge is in touched.
            for (const auto& edge : node->getIncomingEdges()) {
                if (edges.count(edge) && !retiredOwnerEdges.count(edge)) staged.incoming.push_back(edge);
            }
            for (const auto& edge : node->getOutgoingEdges()) {
                if (edges.count(edge) && !retiredOwnerEdges.count(edge)) staged.outgoing.push_back(edge);
            }
        }
        for (const auto& edge : result.insertedEdges) {
            stagedAdjacency.at(edge->getOutput()).incoming.push_back(edge);
            // Preserve signed body occurrences, including repeated endpoints.
            for (const auto& input : edge->getInputs()) stagedAdjacency.at(input).outgoing.push_back(edge);
        }
        // Allocate new owner hash nodes while the edges are still detached.
        // The commit can then transfer them without allocating per insertion.
        std::unordered_set<EdgePtr> stagedOwnerEdges;
        stagedOwnerEdges.reserve(result.insertedEdges.size());
        stagedOwnerEdges.insert(result.insertedEdges.begin(), result.insertedEdges.end());
        const size_t finalOwnerEdgeCount = edges.size() - retiredOwnerEdges.size() + stagedOwnerEdges.size();
        if (static_cast<double>(finalOwnerEdgeCount) >
                static_cast<double>(edges.bucket_count()) * edges.max_load_factor()) {
            // This is the last possible allocation before visible mutation.
            // Ordinary contractions only shrink membership and avoid rehash.
            edges.reserve(finalOwnerEdgeCount);
        }

        auto& stats = result.stats;
        stats.removedNodes = retiredActiveNodes.size();
        stats.removedEdges = retiredActiveEdges.size();
        stats.removedOwnerNodes = retiredNodes.size();
        stats.removedOwnerEdges = retiredOwnerEdges.size();
        stats.terminalViewCommits = 1;
        for (auto& [node, staged] : stagedAdjacency) {
            node->getIncomingEdges().swap(staged.incoming);
            node->getOutgoingEdges().swap(staged.outgoing);
        }
        for (const auto& node : retiredActiveNodes) {
            node->getIncomingEdges().clear();
            node->getOutgoingEdges().clear();
            node->pruned = true;
            nodes.erase(node);
        }
        for (const auto& edge : retiredOwnerEdges) {
            edge->pruned = true;
            edges.erase(edge);
        }
        for (auto it = tupleToNodeMap.begin(); it != tupleToNodeMap.end();) {
            if (retiredNodes.count(it->second)) {
                existingTuples.erase(it->first);
                it = tupleToNodeMap.erase(it);
            } else ++it;
        }
        for (auto it = nodeRepMap.begin(); it != nodeRepMap.end();) {
            if (retiredNodes.count(it->second)) it = nodeRepMap.erase(it);
            else ++it;
        }
        for (auto it = edgeKeyToEdgeMap.begin(); it != edgeKeyToEdgeMap.end();) {
            if (retiredOwnerEdges.count(it->second)) it = edgeKeyToEdgeMap.erase(it);
            else ++it;
        }
        edges.merge(stagedOwnerEdges);
        view.mutableNodes().swap(finalNodes);
        view.mutableEdges().swap(finalEdges);
        nextEdgeId = stagedNextEdgeId;
        terminalViewCommitted = true;
        view.invalidateCaches();
        invalidateCaches();
        return result;
    }

    // Compact owner membership once. In the uncertified case, unrelated owner
    // history remains owned and cannot silently disappear with a partial view.
    std::unordered_set<NodePtr> retainedNodes;
    retainedNodes.reserve(completeActiveDerivations ? finalNodes.size() : nodes.size() - retiredActiveNodes.size());
    std::unordered_set<EdgePtr> retainedEdges;
    retainedEdges.reserve(completeActiveDerivations ? finalEdges.size() :
            edges.size() - retiredActiveEdges.size() + result.insertedEdges.size());
    if (completeActiveDerivations) {
        retainedNodes.insert(finalNodes.begin(), finalNodes.end());
        retainedEdges.insert(finalEdges.begin(), finalEdges.end());
    } else {
        for (const auto& node : nodes) {
            if (!originalNodes.count(node) || finalNodes.count(node)) retainedNodes.insert(node);
        }
        for (const auto& edge : edges) {
            if (!originalEdges.count(edge) || finalEdges.count(edge)) retainedEdges.insert(edge);
        }
        retainedEdges.insert(result.insertedEdges.begin(), result.insertedEdges.end());
        // Historical adjacency may be stale. Check actual edge endpoints as
        // well as the retiring nodes' raw incident lists before preserving it.
        for (const auto& edge : retainedEdges) {
            if (!retainedNodes.count(edge->getOutput())) {
                throw std::logic_error("Retained owner edge has a retired head");
            }
            for (const auto& input : edge->getInputs()) {
                if (!retainedNodes.count(input)) {
                    throw std::logic_error("Retained owner edge has a retired input");
                }
            }
        }
    }
    for (const auto& [tuple, value] : evidences) {
        const auto node = findNode(tuple);
        if (!node || !retainedNodes.count(node) || !node->hasEvidence() || node->getEvidenceValue() != value) {
            throw std::logic_error("Residual rewrite view loses attached evidence");
        }
    }
    // Validate all retiring evidence before changing any adjacency. Raw handles
    // avoid copying ownership or building large retirement membership tables;
    // the old owner keeps these nodes alive through the complete commit.
    std::vector<Node*> retiredNodes;
    retiredNodes.reserve(nodes.size() - retainedNodes.size());
    for (const auto& node : nodes) {
        if (!retainedNodes.count(node)) {
            if (node->hasEvidence()) throw std::logic_error("Residual rewrite view retires an observed node");
            retiredNodes.push_back(node.get());
        }
    }

    // Stage adjacency only for endpoints receiving new edges. Each raw vector
    // is filtered once, and all vector/hash allocation finishes before commit.
    // Unaffected endpoints can be filtered in place without allocating.
    struct StagedAdjacency {
        std::vector<EdgePtr> incoming, outgoing;
    };
    std::unordered_map<NodePtr, StagedAdjacency> stagedAdjacency;
    stagedAdjacency.reserve(std::min(retainedNodes.size(), result.insertedEdges.size()));
    auto stageNode = [&](const NodePtr& node) -> StagedAdjacency& {
        auto [it, inserted] = stagedAdjacency.try_emplace(node);
        if (inserted) {
            for (const auto& edge : node->getIncomingEdges()) {
                if (retainedEdges.count(edge)) it->second.incoming.push_back(edge);
            }
            for (const auto& edge : node->getOutgoingEdges()) {
                if (retainedEdges.count(edge)) it->second.outgoing.push_back(edge);
            }
        }
        return it->second;
    };
    for (const auto& edge : result.insertedEdges) {
        stageNode(edge->getOutput()).incoming.push_back(edge);
        for (const auto& input : edge->getInputs()) stageNode(input).outgoing.push_back(edge);
    }

    auto& stats = result.stats;
    stats.removedNodes = retiredActiveNodes.size();
    stats.removedEdges = retiredActiveEdges.size();
    stats.removedOwnerNodes = retiredNodes.size();
    stats.removedOwnerEdges = edges.size() + result.insertedEdges.size() - retainedEdges.size();
    for (const auto& node : retainedNodes) {
        const auto staged = stagedAdjacency.find(node);
        if (staged != stagedAdjacency.end()) {
            node->getIncomingEdges().swap(staged->second.incoming);
            node->getOutgoingEdges().swap(staged->second.outgoing);
            continue;
        }
        auto& incoming = node->getIncomingEdges();
        incoming.erase(std::remove_if(incoming.begin(), incoming.end(),
                [&](const EdgePtr& edge) { return !retainedEdges.count(edge); }), incoming.end());
        auto& outgoing = node->getOutgoingEdges();
        outgoing.erase(std::remove_if(outgoing.begin(), outgoing.end(),
                [&](const EdgePtr& edge) { return !retainedEdges.count(edge); }), outgoing.end());
    }
    for (const auto node : retiredNodes) {
        node->getIncomingEdges().clear();
        node->getOutgoingEdges().clear();
        node->pruned = true;
    }
    for (const auto& edge : edges) if (!retainedEdges.count(edge)) edge->pruned = true;
    // Keep surviving alias keys in their original containers. Only bindings
    // whose target retired are erased; no retained tuple/key is copied.
    for (auto it = tupleToNodeMap.begin(); it != tupleToNodeMap.end();) {
        if (!retainedNodes.count(it->second)) {
            existingTuples.erase(it->first);
            it = tupleToNodeMap.erase(it);
        } else ++it;
    }
    if (completeActiveDerivations) {
        for (auto it = existingTuples.begin(); it != existingTuples.end();) {
            if (!tupleToNodeMap.count(*it)) it = existingTuples.erase(it);
            else ++it;
        }
    }
    for (auto it = nodeRepMap.begin(); it != nodeRepMap.end();) {
        if (!retainedNodes.count(it->second)) it = nodeRepMap.erase(it);
        else ++it;
    }
    for (auto it = edgeKeyToEdgeMap.begin(); it != edgeKeyToEdgeMap.end();) {
        if (!retainedEdges.count(it->second)) it = edgeKeyToEdgeMap.erase(it);
        else ++it;
    }
    nodes.swap(retainedNodes);
    edges.swap(retainedEdges);
    view.mutableNodes().swap(finalNodes);
    view.mutableEdges().swap(finalEdges);
    nextEdgeId = stagedNextEdgeId;
    // Attached evidence was validated against surviving bindings above. Keep
    // its original names/values, including names bound through event aliases.
    view.invalidateCaches();
    invalidateCaches();
    return result;
}

WorkingSubgraphView WorkingDerivationGraph::prune(const std::vector<souffle::Relation*>& outputRelations,
        BackwardTraversalObserver* observer) {
    std::vector<std::string> outputRelationNames;
    outputRelationNames.reserve(outputRelations.size());
    for (const auto* rel : outputRelations) {
        outputRelationNames.push_back(rel->getName());
    }
    return prune(outputRelationNames, observer);
}

WorkingSubgraphView WorkingDerivationGraph::prune(const std::vector<std::string>& outputRelations,
        BackwardTraversalObserver* observer) {
    if (terminalViewCommitted) {
        throw std::logic_error("Cannot reconstruct an owner view after a terminal rewrite commit");
    }
    FunctionTimer totalTimer("prune working graph");
    const bool profileEnabled = fcProfileEnabled;
    using Clock = std::chrono::steady_clock;
    auto toMs = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    const auto totalStart = Clock::now();
    double initOutputsMs = 0.0;
    double bfsMs = 0.0;
    double markMs = 0.0;
    double outputlessMs = 0.0;
    double cleanupMs = 0.0;

    dumpWorkingStatistics(std::cout);

    std::unordered_set<std::string> outputRelationNames;
    outputRelationNames.reserve(outputRelations.size());
    outputRelationNames.insert(outputRelations.begin(), outputRelations.end());

    std::unordered_set<NodePtr> liveNodes;
    std::unordered_set<EdgePtr> liveEdges;
    liveNodes.reserve(nodes.size());
    liveEdges.reserve(edges.size());
    std::queue<NodePtr> workQueue;
    std::vector<NodePtr> evidenceNodes;

    {
        const auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune: initialize outputs");
        for (const auto& node : nodes) {
            if (outputRelationNames.count(node->getTuple().relation_name) > 0 || node->isQueryNode()) {
                liveNodes.insert(node);
                workQueue.push(node);
                node->setQuery();
                if (observer) observer->node(node);
            }
            if (node->hasEvidence()) {
                evidenceNodes.push_back(node);
            }
        }
        if (profileEnabled) {
            initOutputsMs = toMs(t0);
        }
    }

    {
        const auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune: evidence and backward BFS");
        for (const auto& node : evidenceNodes) {
            if (liveNodes.insert(node).second) {
                workQueue.push(node);
                if (observer) observer->node(node);
            }
        }

        while (!workQueue.empty()) {
            NodePtr current = workQueue.front();
            workQueue.pop();
            if (current->isFact) {
                continue;
            }
            for (const auto& edge : current->getIncomingEdges()) {
                if (edge->hasSelfDependency()) {
                    continue;
                }
                if (!liveEdges.insert(edge).second) continue;
                if (observer) observer->beginEdge(edge);
                std::size_t inputIndex = 0;
                for (const auto& inputNode : edge->getInputs()) {
                    if (liveNodes.insert(inputNode).second) {
                        workQueue.push(inputNode);
                        if (observer) observer->node(inputNode);
                    }
                    if (observer) {
                        const auto& signs = edge->getBodyNegations();
                        observer->input(inputNode, inputIndex < signs.size() && signs[inputIndex]);
                    }
                    ++inputIndex;
                }
                if (observer) observer->endEdge();
            }
        }
        if (profileEnabled) {
            bfsMs = toMs(t0);
        }
    }

    {
        const auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune: mark live nodes and edges");
        for (const auto& node : nodes) {
            node->pruned = liveNodes.count(node) == 0;
        }
        for (const auto& edge : edges) {
            edge->pruned = liveEdges.count(edge) == 0;
        }
        if (profileEnabled) {
            markMs = toMs(t0);
        }
    }

    if (pruneExtraEnabled) {
        const auto t0 = Clock::now();
        pruneOutputlessComponents(liveNodes, liveEdges);
        if (profileEnabled) {
            outputlessMs = toMs(t0);
        }
    }

    {
        const auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune: canonical cleanup");
        if (mergeBiImpEnabled) {
            mergeBiImpEquivalences(liveNodes, liveEdges);
            // Merging may move evidence to a representative and remove the
            // original observed node. Return only the current live roots.
            evidenceNodes.clear();
            for (const auto& node : liveNodes) {
                if (node->hasEvidence()) {
                    evidenceNodes.push_back(node);
                }
            }
        }
        removeSelfLoopEdges(liveNodes, liveEdges);
        if (profileEnabled) {
            cleanupMs = toMs(t0);
        }
    }

    const size_t liveNodeCount = liveNodes.size();
    const size_t liveEdgeCount = liveEdges.size();
    // The full pruning scan already collected every surviving evidence root,
    // including the confirmed-empty case. Other view callers still discover.
    WorkingSubgraphView view(std::move(liveNodes), std::move(liveEdges), std::move(evidenceNodes),
            WorkingSubgraphView::EvidenceRoots::Complete);
    view.dumpWorkingStatistics(std::cout);

    if (profileEnabled) {
        const double totalMs = toMs(totalStart);
        std::cout << "[profile] stage=PRUNING prune_ms=" << totalMs
                  << " init_ms=" << initOutputsMs
                  << " bfs_ms=" << bfsMs
                  << " mark_ms=" << markMs
                  << " outputless_ms=" << outputlessMs
                  << " cleanup_ms=" << cleanupMs
                  << " live_nodes=" << liveNodeCount
                  << " live_edges=" << liveEdgeCount
                  << std::endl;
    }
    return view;
}

void DerivationGraph::mergeBiImpEquivalences(
        std::unordered_set<NodePtr>& liveNodes, std::unordered_set<EdgePtr>& liveEdges) {
    std::cout << "[bi-imp] mergeBiImpEquivalences: enabled=" << std::boolalpha << mergeBiImpEnabled
              << ", liveNodes=" << liveNodes.size() << ", liveEdges=" << liveEdges.size()
              << std::endl;
    if (!mergeBiImpEnabled || liveNodes.size() < 2) {
        return;
    }

    auto isEqEdge = [](const EdgePtr& e) {
        if (!e || !e->isDeterministic()) return false;
        if (e->getInputs().size() != 1) return false;
        for (bool neg : e->getBodyNegations()) {
            if (neg) return false;
        }
        return true;
    };

    std::unordered_map<NodePtr, std::vector<NodePtr>> adj;
    for (const auto& n : liveNodes) {
        adj[n];  // consider all nodes; eqrel tag no longer gates merging
    }
    for (const auto& e : liveEdges) {
        if (!isEqEdge(e)) continue;
        NodePtr dst = e->getOutput();
        for (const auto& src : e->getInputs()) {
            if (liveNodes.count(src) && liveNodes.count(dst)) {
                adj[src].push_back(dst);
            }
        }
    }

    // Tarjan SCC
    std::unordered_map<NodePtr, int> index, lowlink;
    std::vector<NodePtr> stack;
    std::unordered_set<NodePtr> onStack;
    int idx = 0;
    std::vector<std::vector<NodePtr>> sccs;

    std::function<void(NodePtr)> strongConnect = [&](NodePtr v) {
        index[v] = lowlink[v] = idx++;
        stack.push_back(v);
        onStack.insert(v);
        for (const auto& w : adj[v]) {
            if (!index.count(w)) {
                strongConnect(w);
                lowlink[v] = std::min(lowlink[v], lowlink[w]);
            } else if (onStack.count(w)) {
                lowlink[v] = std::min(lowlink[v], index[w]);
            }
        }
        if (lowlink[v] == index[v]) {
            std::vector<NodePtr> component;
            while (true) {
                NodePtr w = stack.back();
                stack.pop_back();
                onStack.erase(w);
                component.push_back(w);
                if (w == v) break;
            }
            sccs.push_back(std::move(component));
        }
    };

    for (const auto& [node, _] : adj) {
        if (!index.count(node)) {
            strongConnect(node);
        }
    }
    std::cout << "[bi-imp] detected SCCs=" << sccs.size() << std::endl;

    auto hasSelfLoop = [&](NodePtr n) {
        auto it = adj.find(n);
        if (it == adj.end()) return false;
        return std::find(it->second.begin(), it->second.end(), n) != it->second.end();
    };

    std::size_t mergedClasses = 0;
    std::size_t mergedNodes = 0;
    bool mergeChanged = false;

    for (const auto& comp : sccs) {
        if (comp.size() <= 1 && !hasSelfLoop(comp.front())) continue;

        NodePtr rep = *std::min_element(comp.begin(), comp.end(),
                [](const NodePtr& a, const NodePtr& b) { return a->getId() < b->getId(); });

        bool hasEvidence = false;
        bool evidenceValue = false;
        bool conflict = false;
        for (const auto& n : comp) {
            if (n->hasEvidence()) {
                if (!hasEvidence) {
                    hasEvidence = true;
                    evidenceValue = n->getEvidenceValue();
                } else if (n->getEvidenceValue() != evidenceValue) {
                    conflict = true;
                    break;
                }
            }
        }
        if (conflict) {
            std::cerr << "Skip merging SCC with conflicting evidence: " << rep->toString() << std::endl;
            continue;
        }

        std::cerr << "[bi-imp-merge] merging class (size=" << comp.size()
                  << ") -> rep " << rep->toString() << "_" << rep->getId() << std::endl;
        for (const auto& n : comp) {
            std::cerr << "    member: " << n->toString() << "_" << n->getId() << std::endl;
        }

        for (const auto& n : comp) {
            setRepresentative(n, rep);
            if (n->isQueryNode()) rep->setQuery();
            rep->needOutput = rep->needOutput || n->needOutput;
        }
        if (hasEvidence) {
            rep->setEvidence(evidenceValue);
        }

        for (const auto& n : comp) {
            if (n == rep) continue;

            auto incoming = n->incomingEdges;
            for (const auto& e : incoming) {
                e->replaceOutput(rep);
                // drop self-loop edges created by symmetric fusion
                bool allInputsRep = std::all_of(e->inputs.begin(), e->inputs.end(),
                        [&](const NodePtr& in) { return in == rep; });
                if (allInputsRep && e->getOutput() == rep) {
                    e->pruned = true;
                } else {
                    rep->incomingEdges.push_back(e);
                }
            }

            auto outgoing = n->outgoingEdges;
            for (const auto& e : outgoing) {
                e->replaceInput(n, rep);
                bool allInputsRep = std::all_of(e->inputs.begin(), e->inputs.end(),
                        [&](const NodePtr& in) { return in == rep; });
                if (allInputsRep && e->getOutput() == rep) {
                    e->pruned = true;
                } else {
                    rep->outgoingEdges.push_back(e);
                }
            }

            tupleToNodeMap[n->getTuple()] = rep;
            n->incomingEdges.clear();
            n->outgoingEdges.clear();
            liveNodes.erase(n);
            nodes.erase(n);
            mergedNodes++;
        }
        mergedClasses++;
    }
    if (mergedClasses > 0) {
        mergeChanged = true;
    }

    auto edgeSig = [](const EdgePtr& e) {
        std::stringstream ss;
        ss << (e->getRule() ? e->getRule()->getRuleId() : e->getRuleApp().ruleId) << "|";
        ss << e->getOutput()->getTuple().toString() << "|";
        auto inputs = e->getInputsStable();
        auto negs = e->getBodyNegationsStable();
        for (size_t i = 0; i < inputs.size(); ++i) {
            ss << (negs[i] ? "!" : "") << inputs[i]->getTuple().toString() << ";";
        }
        return ss.str();
    };

    std::unordered_map<std::string, EdgePtr> seen;
    std::unordered_set<EdgePtr> toRemove;
    // remove edges whose body already contains the head (self-loop style)
    for (const auto& e : liveEdges) {
        if (e->hasSelfDependency()) {
            e->pruned = true;
            toRemove.insert(e);
            continue;
        }
        auto sig = edgeSig(e);
        auto[it, inserted] = seen.emplace(sig, e);
        if (!inserted) {
            toRemove.insert(e);
        }
    }

    if (!toRemove.empty()) {
        mergeChanged = true;
    }
    for (const auto& e : toRemove) {
        liveEdges.erase(e);
        edges.erase(e);
        auto out = e->getOutput();
        auto& inVec = out->incomingEdges;
        inVec.erase(std::remove(inVec.begin(), inVec.end(), e), inVec.end());
        for (const auto& in : e->getInputs()) {
            auto& outVec = in->outgoingEdges;
            outVec.erase(std::remove(outVec.begin(), outVec.end(), e), outVec.end());
        }
    }

    for (const auto& n : liveNodes) {
        std::vector<EdgePtr> newIn;
        std::vector<EdgePtr> newOut;
        std::unordered_set<EdgePtr> seenIn, seenOut;
        for (const auto& e : n->incomingEdges) {
            if (liveEdges.count(e) && seenIn.insert(e).second) {
                newIn.push_back(e);
            }
        }
        for (const auto& e : n->outgoingEdges) {
            if (liveEdges.count(e) && seenOut.insert(e).second) {
                newOut.push_back(e);
            }
        }
        n->incomingEdges = std::move(newIn);
        n->outgoingEdges = std::move(newOut);
    }

    if (mergeChanged) {
        biImpMerged = true;
    }
    if (mergedClasses > 0) {
        std::cerr << "[bi-imp-merge] merged classes: " << mergedClasses
                  << ", merged nodes: " << mergedNodes
                  << ", remaining nodes: " << liveNodes.size()
                  << ", remaining edges: " << liveEdges.size() << std::endl;
    } else {
        std::cerr << "[bi-imp-merge] no deterministic SCCs found" << std::endl;
    }
}

class IncrementalDerivationGraph : public DerivationGraph, virtual public IncrementalDerivationGraphViewInterface {
public:
    // Constructors
    IncrementalDerivationGraph() : DerivationGraph() {}
    IncrementalDerivationGraph(const RuleManager* rm) : DerivationGraph(rm) {}
    IncSubgraphView prune(const std::vector<souffle::Relation*>& outputRelations,
            DeltaMetadataPolicy deltaPolicy = DeltaMetadataPolicy::Retain);
    IncSubgraphView prune(const std::vector<std::string>& outputRelations,
            DeltaMetadataPolicy deltaPolicy = DeltaMetadataPolicy::Retain);
    
    static IncrementalDerivationGraph* createFrom(const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& ruleApps, const RuleManager& ruleManager, const QueryManager& queryManager, const std::unordered_map<UntypedTuple, double>& fact_prob = {}, const std::vector<std::pair<UntypedTuple,bool>>& evidences = {}) {
        FunctionTimer timer(" creating derivation graph ");
        if (DerivationGraphViewInterface::isDumpStatsEnabled()) {
            resetEdgeLookupTiming();
            resetEdgeBuildTiming();
        }
        auto graph = new IncrementalDerivationGraph(&ruleManager);
        {
            FunctionTimer scopeTimer("create graph: init fact nodes");
            for (const auto& [tuple, prob] : fact_prob) {
                graph->existingTuples.insert(tuple);
                auto node = graph->createNode(tuple);  // actually "find node" here
                node->setProbability(prob);
                node->isFact = true;
                node->setOriginalFact(true);
            }
        }
        for (const auto& [tuple, _] : ruleApps) {
            graph->existingTuples.insert(tuple);
        }
        {
            FunctionTimer scopeTimer("create graph: build rule apps");
            for (const auto& [tuple, ruleAppSet] : ruleApps) {
                auto node = graph->createNode(tuple, 0.0);
                for (const auto& ruleApp : *ruleAppSet) {
                    auto edge = graph->createHyperedgeFromRuleApp(ruleApp, ruleManager);
                }
            }
        }

        {
            FunctionTimer scopeTimer("create graph: attach queries");
            for (auto& node : graph->getNodes()) {
                graph->createQuery(node, queryManager);
            }
        }
        {
            FunctionTimer scopeTimer("create graph: attach evidence");
            graph->attachEvidence(evidences);
        }
        if (DerivationGraphViewInterface::isDumpStatsEnabled()) {
            dumpEdgeLookupTiming("create graph");
            const EdgeBuildTiming& t = edgeBuildTiming;
            const double avg_ms = t.calls ? (t.total_s * 1000.0 / t.calls) : 0.0;
            const double avg_body = t.calls ? (static_cast<double>(t.body_atoms) / t.calls) : 0.0;
            std::cout << "[timing] create graph edgeBuild: calls=" << t.calls
                      << " hits=" << t.existing_hits
                      << " body_atoms=" << t.body_atoms
                      << " avg_body=" << avg_body
                      << " total_s=" << t.total_s
                      << " avg_ms=" << avg_ms
                      << " rule_s=" << t.rule_lookup_s
                      << " vars_s=" << t.get_vars_s
                      << " find_s=" << t.find_existing_s
                      << " head_inst_s=" << t.head_instantiate_s
                      << " head_node_s=" << t.head_node_s
                      << " body_inst_s=" << t.body_instantiate_s
                      << " body_node_s=" << t.body_node_s
                      << " body_neg_s=" << t.body_neg_s
                      << " edge_s=" << t.create_edge_s
                      << " insert_s=" << t.insert_total_s
                      << std::endl;
        }
        return graph;
    }

    // static std::unique_ptr<IncrementalDerivationGraph> 
    //     loadFromJson(const std::string& filename, const RuleManager* rm);
    // Apply incremental inserts
    void applyDeltaInserts(
        const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& deltaInsertRuleApps,
        const RuleManager& ruleManager,
        const std::unordered_map<UntypedTuple, double>& fact_prob = {}
    );

    // Apply incremental deletes
    void applyDeltaDeletes(
        const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& deltaDeleteRuleApps,
        const RuleManager& ruleManager,
        const std::vector<UntypedTuple>& deletedFacts
    );

    // Merge method: apply inserts and deletes together
    void applyDelta(
        const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& deltaInsertRuleApps,
        const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& deltaDeleteRuleApps,
        const RuleManager& ruleManager,
        const std::unordered_map<UntypedTuple, double>& fact_prob = {},
        const std::vector<UntypedTuple>& deletedFacts = {},
        DeltaMetadataPolicy deltaPolicy = DeltaMetadataPolicy::Retain
    ) {
        const bool incProfile = incProfileEnabled;
        using Clock = std::chrono::steady_clock;
        auto toMs = [](Clock::time_point start) {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        };
        auto countRuleApps = [](const auto& m) {
            size_t total = 0;
            for (const auto& [_, s] : m) {
                total += s ? s->size() : 0;
            }
            return total;
        };
        if (DerivationGraphViewInterface::isVerboseEnabled()) {
            std::cout << "[applyDelta] delTuples=" << deltaDeleteRuleApps.size()
                      << " delRuleApps=" << countRuleApps(deltaDeleteRuleApps)
                      << " delFacts=" << deletedFacts.size()
                      << " insTuples=" << deltaInsertRuleApps.size()
                      << " insRuleApps=" << countRuleApps(deltaInsertRuleApps)
                      << " insFacts=" << fact_prob.size()
                      << std::endl;
        }
        double clearMs = 0.0;
        double deletesMs = 0.0;
        double insertsMs = 0.0;
        {
            auto t0 = Clock::now();
            FunctionTimer timer("applyDelta: clear state");
            this->deltaInsertNodes.clear();
            this->deltaInsertEdges.clear();
            this->deltaDeleteNodes.clear();
            this->deltaDeleteEdges.clear();
            this->deltaInsertFactNodes.clear();

            this->deletedFactImpactedNodes.clear();
            this->deletedFactImpactedEdges.clear();
            this->insertedFactImpactedNodes.clear();
            this->insertedFactImpactedEdges.clear();
            this->deltaInsertReachableNodes.clear();
            this->deltaInsertReachableEdges.clear();
            this->explicitDeletedFacts_.clear();
            this->deletedFacts_.clear();
            this->deletedDeterminsticFacts_.clear();
            this->deletedNonDeterministicFacts_.clear();
            // Invalidate all view-level caches before structural delta updates.
            // applyDeltaDeletes/applyDeltaInserts mutate node/edge adjacencies.
            this->clearViewCaches();
            if (incProfile) {
                clearMs = toMs(t0);
            }
        }

        // Apply deletes first, then inserts
        {
            auto t0 = Clock::now();
            FunctionTimer timer("applyDelta: deletes");
            applyDeltaDeletes(deltaDeleteRuleApps, ruleManager, deletedFacts);
            if (incProfile) {
                deletesMs = toMs(t0);
            }
        }
        {
            auto t0 = Clock::now();
            FunctionTimer timer("applyDelta: inserts");
            applyDeltaInserts(deltaInsertRuleApps, ruleManager, fact_prob);
            if (incProfile) {
                insertsMs = toMs(t0);
            }
        }
        if (!shouldRetainDeltaMetadata(deltaPolicy)) {
            clearDeltaMetadata();
        }
        if (incProfile) {
            std::cout << "[inc-profile] stage=PRUNING_INC applyDelta_ms=" << (clearMs + deletesMs + insertsMs)
                      << " clear_ms=" << clearMs
                      << " del_ms=" << deletesMs
                      << " ins_ms=" << insertsMs
                      << " delRuleApps=" << countRuleApps(deltaDeleteRuleApps)
                      << " insRuleApps=" << countRuleApps(deltaInsertRuleApps)
                      << " delFacts=" << deletedFacts.size()
                      << " insFacts=" << fact_prob.size()
                      << std::endl;
        }
    }

    void clearDeltaMetadata() {
        deltaInsertNodes.clear();
        deltaInsertEdges.clear();
        deltaDeleteNodes.clear();
        deltaDeleteEdges.clear();
        deltaInsertFactNodes.clear();
        deletedFactImpactedNodes.clear();
        deletedFactImpactedEdges.clear();
        insertedFactImpactedNodes.clear();
        insertedFactImpactedEdges.clear();
        deltaInsertReachableNodes.clear();
        deltaInsertReachableEdges.clear();
        explicitDeletedFacts_.clear();
        deletedFacts_.clear();
        deletedDeterminsticFacts_.clear();
        deletedNonDeterministicFacts_.clear();
        clearViewCaches();
    }

    // Sets tracking incremental changes to nodes and edges
    std::set<NodePtr> deltaInsertNodes;
    std::set<EdgePtr> deltaInsertEdges;
    std::set<NodePtr> deltaDeleteNodes;
    std::set<EdgePtr> deltaDeleteEdges;
    std::set<NodePtr> deltaInsertFactNodes;

    std::unordered_map<NodePtr, std::unordered_set<NodePtr>> deletedFactImpactedNodes;
    std::unordered_map<NodePtr, std::unordered_set<EdgePtr>> deletedFactImpactedEdges;
    std::unordered_map<NodePtr, std::unordered_set<NodePtr>> insertedFactImpactedNodes;
    std::unordered_map<NodePtr, std::unordered_set<EdgePtr>> insertedFactImpactedEdges;
    std::unordered_set<NodePtr> deltaInsertReachableNodes;
    std::unordered_set<EdgePtr> deltaInsertReachableEdges;
    const std::set<NodePtr>& getDeltaInsertNodes() const {
        return deltaInsertNodes;
    }
    const std::set<NodePtr>& getDeltaInsertFactNodes() const override {
        return deltaInsertFactNodes;
    }

    const std::set<EdgePtr>& getDeltaInsertEdges() const {
        return deltaInsertEdges;
    }

    const std::set<NodePtr>& getDeltaDeleteNodes() const {
        return deltaDeleteNodes;
    }

    const std::set<EdgePtr>& getDeltaDeleteEdges() const {
        return deltaDeleteEdges;
    }

    const std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& getNodeImpactedByDeltaDelete() const {
        return deletedFactImpactedNodes;
    }

    const std::unordered_map<NodePtr, std::unordered_set<EdgePtr>>& getEdgeImpactedByDeltaDelete() const {
        return deletedFactImpactedEdges;
    }

    const std::unordered_map<NodePtr, std::unordered_set<NodePtr>>& getNodeImpactedByDeltaInsert() const {
        return insertedFactImpactedNodes;
    }

    const std::unordered_map<NodePtr, std::unordered_set<EdgePtr>>& getEdgeImpactedByDeltaInsert() const {
        return insertedFactImpactedEdges;
    }

    const std::unordered_set<NodePtr>& getDeltaInsertReachableNodes() const {
        return deltaInsertReachableNodes;
    }

    const std::unordered_set<EdgePtr>& getDeltaInsertReachableEdges() const {
        return deltaInsertReachableEdges;
    }

};

void IncrementalDerivationGraph::applyDeltaInserts(
    const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& deltaInsertRuleApps,
    const RuleManager& ruleManager,
    const std::unordered_map<UntypedTuple, double>& fact_prob
) {
    if (deltaInsertRuleApps.empty() && fact_prob.empty()) {
        return;
    }
    FunctionTimer timer("applying delta inserts");

    // all facts
    {
        FunctionTimer scope("applyDeltaInserts: upsert facts");
        for (const auto& [tuple, prob] : fact_prob) {
            existingTuples.insert(tuple);
            auto node = this->findNode(tuple);
            if (node == nullptr) {
                node = createNode(tuple);
                deltaInsertNodes.insert(node);
            }
            node->setProbability(prob);
            node->isFact = true;
            node->setOriginalFact(true);
            deltaInsertFactNodes.insert(node);
        }
    }

    {
        FunctionTimer scope("applyDeltaInserts: add rule edges");
        for (const auto& [tuple, _] : deltaInsertRuleApps) {
            existingTuples.insert(tuple);
        }
        for (const auto& [tuple, ruleAppSet] : deltaInsertRuleApps) {
            // Process each rule application associated with this tuple.
            for (const auto& ruleApp : *ruleAppSet) {
                // Check whether the edge for this rule application already exists.
                const Rule* rule = ruleManager.getRule(ruleApp.ruleId);
                EdgePtr existingEdge = findHyperedgeFromRuleApp(ruleApp, rule->getVars());
                if (existingEdge != nullptr && DerivationGraphViewInterface::isVerboseEnabled()) {
                    std::cout << existingEdge->toString() << std::endl;
                }
                assert(existingEdge == nullptr && "Delta insert edge already exists in the graph");

                // Create the hyperedge for this rule application.
                EdgePtr newEdge = createHyperedgeFromRuleApp(ruleApp, ruleManager);

                // Mark the edge as a delta insert.
                deltaInsertEdges.insert(newEdge);

                // Ensure fact inputs are initialized during incremental FC.
                for (const auto& inputNode : newEdge->getInputs()) {
                    if (inputNode->isFact) {
                        deltaInsertNodes.insert(inputNode);
                        deltaInsertFactNodes.insert(inputNode);
                    }
                }

                // Get the output node.
                NodePtr outputNode = newEdge->getOutput();
                if (outputNode && outputNode->isFact) {
                    deltaInsertFactNodes.insert(outputNode);
                }

                // If this is a reinserted node (previously marked deleted)
                if (deltaDeleteNodes.find(outputNode) != deltaDeleteNodes.end()) {
                    // Remove from delete set.
                    deltaDeleteNodes.erase(outputNode);
                } else {
                    // Otherwise, add to insert set (if not already added).
                    if (!outputNode->isFact && (outputNode->pruned || outputNode->getIncomingEdges().size() == 1))
                        deltaInsertNodes.insert(outputNode);
                }
            }
        }
    }

    {
        // Impacted info is rebuilt after prune on the subgraph; no propagation here.
    }

}

void IncrementalDerivationGraph::applyDeltaDeletes(
    const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& deltaDeleteRuleApps,
    const RuleManager& ruleManager,
    const std::vector<UntypedTuple>& deletedFacts
) {
    if (deltaDeleteRuleApps.empty() && deletedFacts.empty()) {
        return;
    }
    FunctionTimer timer("applying delta deletes");
    using Clock = std::chrono::steady_clock;
    auto elapsedMs = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    const bool incProfile = incProfileEnabled;
    double tFindEdgeMs = 0.0;
    double tEraseVecMs = 0.0;
    double tEraseEdgeSetMs = 0.0;
    double tEraseKeyMs = 0.0;
    double tOutputDecisionMs = 0.0;
    double collectImpactMs = 0.0;
    double removeRuleAppsMs = 0.0;
    double removeDanglingMs = 0.0;
    double removeFactMs = 0.0;
    double cleanupMs = 0.0;
    size_t totalInVecScan = 0;
    size_t totalOutVecScan = 0;
    size_t removedEdgeCount = 0;
    size_t missingEdgeCount = 0;
    std::vector<EdgePtr> edgesToRemoveList;
    std::vector<std::string> edgeKeysToRemove;
    std::unordered_set<EdgePtr> edgesToRemoveSet;
    std::unordered_set<NodePtr> outputsToFix;
    std::unordered_set<NodePtr> inputsToFix;
    std::unordered_set<UntypedTuple> deletedFactsSet(deletedFacts.begin(), deletedFacts.end());
    {
        auto tCollectStart = Clock::now();
        FunctionTimer scope("applyDeltaDeletes: collect impact of deleted facts");
        // Old logic rebuilds impacted maps after prune; keep interface as a placeholder to minimize overhead.
        if (incProfile) {
            collectImpactMs = elapsedMs(tCollectStart);
        }
    }

    // Track nodes to delete.
    std::vector<NodePtr> nodesToRemove;

    {
        auto tRuleAppsStart = Clock::now();
        FunctionTimer scope("applyDeltaDeletes: remove rule applications");
        for (const auto& [tuple, ruleAppSet] : deltaDeleteRuleApps) {
            for (const auto& ruleApp : *ruleAppSet) {
                // Find the edge to delete - direct map lookup.
                const Rule* rule = ruleManager.getRule(ruleApp.ruleId);
                const std::vector<std::string>& vars = rule->getVars();
                auto tFindStart = Clock::now();
                EdgePtr existingEdge = findHyperedgeFromRuleApp(ruleApp, vars);
                tFindEdgeMs += elapsedMs(tFindStart);
                if (existingEdge == nullptr) {
                    if (DerivationGraphViewInterface::isVerboseEnabled()) {
                        std::cout << "Did not find the edge to delete: "
                                  << createEdgeKey(ruleApp.ruleId, vars, ruleApp.varValuesPure) << std::endl;
                    }
                    missingEdgeCount++;
                    continue;
                }

                // Mark the edge as a delta delete.
                deltaDeleteEdges.insert(existingEdge);

                // Get output node and input nodes.
                NodePtr outputNode = existingEdge->getOutput();
                const std::vector<NodePtr>& inputNodes = existingEdge->getInputs();

                edgesToRemoveList.push_back(existingEdge);
                edgesToRemoveSet.insert(existingEdge);
                outputsToFix.insert(outputNode);
                for (const auto& inputNode : inputNodes) {
                    if (tupleToNodeMap.find(inputNode->getTuple()) != tupleToNodeMap.end()) {
                        inputsToFix.insert(inputNode);
                    }
                }
                std::string edgeKey = createEdgeKey(ruleApp.ruleId, vars, ruleApp.varValuesPure);
                edgeKeysToRemove.push_back(edgeKey);
                removedEdgeCount++;
            }
        }

        // Batch-remove edges to delete from affected nodes.
        auto tEraseVecStart = Clock::now();
        for (const auto& head : outputsToFix) {
            auto& inEdges = head->getIncomingEdges();
            totalInVecScan += inEdges.size();
            inEdges.erase(std::remove_if(inEdges.begin(), inEdges.end(), [&](const EdgePtr& e) {
                return edgesToRemoveSet.count(e) > 0;
            }), inEdges.end());
        }
        for (const auto& tail : inputsToFix) {
            auto& outEdges = tail->getOutgoingEdges();
            totalOutVecScan += outEdges.size();
            outEdges.erase(std::remove_if(outEdges.begin(), outEdges.end(), [&](const EdgePtr& e) {
                return edgesToRemoveSet.count(e) > 0;
            }), outEdges.end());
        }
        tEraseVecMs += elapsedMs(tEraseVecStart);

        // Remove from the global edge set.
        auto tEraseEdgeStart = Clock::now();
        for (const auto& edge : edgesToRemoveList) {
            edges.erase(edge);
        }
        tEraseEdgeSetMs += elapsedMs(tEraseEdgeStart);

        // Remove from edgeKeyToEdgeMap.
        auto tEraseKeyStart = Clock::now();
        for (const auto& key : edgeKeysToRemove) {
            edgeKeyToEdgeMap.erase(key);
        }
        tEraseKeyMs += elapsedMs(tEraseKeyStart);

        // Check whether output nodes have other derivation paths.
        auto tOutputStart = Clock::now();
        for (const auto& outputNode : outputsToFix) {
            if ((!outputNode->isFact || deletedFactsSet.count(outputNode->getTuple())) && outputNode->getIncomingEdges().empty()) {
                if (deltaInsertNodes.find(outputNode) != deltaInsertNodes.end()) {
                    deltaInsertNodes.erase(outputNode);
                } else {
                    deltaDeleteNodes.insert(outputNode);
                    nodesToRemove.push_back(outputNode);
                }
            }
        }
        tOutputDecisionMs += elapsedMs(tOutputStart);
        if (incProfile) {
            removeRuleAppsMs = elapsedMs(tRuleAppsStart);
        }
    }

    {
        auto t0 = Clock::now();
        FunctionTimer scope("applyDeltaDeletes: remove dangling nodes");
        // Finally, remove derived nodes with no incoming edges from the graph.
        for (const auto& nodeToRemove : nodesToRemove) {
            // Remove from map.
            tupleToNodeMap.erase(nodeToRemove->getTuple());
            existingTuples.erase(nodeToRemove->getTuple());
            // Remove from the node list.
            nodes.erase(nodeToRemove);
        }
        if (incProfile) {
            removeDanglingMs = elapsedMs(t0);
        }
    }

    {
        auto t0 = Clock::now();
        FunctionTimer scope("applyDeltaDeletes: remove fact nodes");
        for (const auto& tuple : deletedFacts) {
            auto node = this->findNode(tuple);
            // it's possible that a deleted fact has deleted derivation...
            if (node != nullptr) {
                explicitDeletedFacts_.insert(node);
                // Remove from map.

                tupleToNodeMap.erase(node->getTuple());
                deltaDeleteNodes.insert(node);
                if (!node->getIncomingEdges().empty()) {
                    // the node changes from an input fact to a derived node
                    node->isFact = false;
                    node->pruned = true; // pretend to be pruned
                    deltaInsertNodes.insert(node); // but still keep it in the graph
                    for (auto edge: node->getIncomingEdges()) {
                        if (deltaDeleteEdges.find(edge) == deltaDeleteEdges.end()) {
                            deltaInsertEdges.insert(edge);
                        }
                    }
                } else {
                    existingTuples.erase(node->getTuple());
                    nodes.erase(node);
                    nodesToRemove.push_back(node);
                }
                // Remove from the node list.

            } else {
                // It's possible that the deleted fact has been removed from the graph; but it has to be with deleted derivations
                if (DerivationGraphViewInterface::isVerboseEnabled()) {
                    std::cout << "deleted fact not found: " << tuple.toString() << std::endl;
                }
            }
        }
        if (incProfile) {
            removeFactMs = elapsedMs(t0);
        }
    }

    {
        auto t0 = Clock::now();
        FunctionTimer scope("applyDeltaDeletes: cleanup impacted sets");
        for (auto& [fact, impactedEdges] : deletedFactImpactedEdges) {
            for (auto it = impactedEdges.begin(); it != impactedEdges.end(); ) {
                if (deltaDeleteEdges.find(*it) != deltaDeleteEdges.end()) {
                    auto toErase = it++;
                    impactedEdges.erase(toErase);
                } else {
                    ++it;
                }
            }
        }
        for (auto& [fact, impactedNodes] : deletedFactImpactedNodes) {
            for (auto it = impactedNodes.begin(); it != impactedNodes.end(); ) {
                if (deltaDeleteNodes.find(*it) != deltaDeleteNodes.end()) {
                    auto toErase = it++;
                    impactedNodes.erase(toErase);
                } else {
                    ++it;
                }
            }
        }
        if (incProfile) {
            cleanupMs = elapsedMs(t0);
        }
    }
    if (incProfile) {
        std::cout << "[inc-profile] stage=PRUNING_INC applyDeltaDeletes_ms="
                  << (collectImpactMs + removeRuleAppsMs + removeDanglingMs + removeFactMs + cleanupMs)
                  << " collect_ms=" << collectImpactMs
                  << " ruleapps_ms=" << removeRuleAppsMs
                  << " dangling_ms=" << removeDanglingMs
                  << " facts_ms=" << removeFactMs
                  << " cleanup_ms=" << cleanupMs
                  << " removedEdges=" << removedEdgeCount
                  << " delRuleApps=" << removedEdgeCount
                  << " inVecScan=" << totalInVecScan
                  << " outVecScan=" << totalOutVecScan
                  << std::endl;
    }
    if (DerivationGraphViewInterface::isVerboseEnabled()) {
        std::cout << "[applyDeltaDeletes] detailed: "
                  << "findEdge=" << tFindEdgeMs << "ms, "
                  << "eraseVec=" << tEraseVecMs << "ms, "
                  << "eraseEdgeSet=" << tEraseEdgeSetMs << "ms, "
                  << "eraseKey=" << tEraseKeyMs << "ms, "
                  << "outputDecision=" << tOutputDecisionMs << "ms, "
                  << "removedEdges=" << removedEdgeCount
                  << ", missingEdges=" << missingEdgeCount
                  << ", inVecScan=" << totalInVecScan
                  << ", outVecScan=" << totalOutVecScan
                  << std::endl;
    }
}

IncSubgraphView IncrementalDerivationGraph::prune(const std::vector<souffle::Relation*>& outputRelations,
        DeltaMetadataPolicy deltaPolicy) {
    std::vector<std::string> outputRelationNames;
    for (const auto* rel : outputRelations) {
        outputRelationNames.push_back(rel->getName());
    }
    return prune(outputRelationNames, deltaPolicy);
}

// pruning is not incremental for now
IncSubgraphView IncrementalDerivationGraph::prune(const std::vector<std::string>& outputRelations,
        DeltaMetadataPolicy deltaPolicy) {
    FunctionTimer totalTimer("prune-inc total");
    const bool retainDeltaMetadata = shouldRetainDeltaMetadata(deltaPolicy);
    if (!retainDeltaMetadata) {
        clearDeltaMetadata();
    }
    const bool incProfile = incProfileEnabled;
    using Clock = std::chrono::steady_clock;
    auto toMs = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    auto totalStart = Clock::now();
    double dumpStatsMs = 0.0;
    double initOutputsMs = 0.0;
    double bfsMs = 0.0;
    double markMs = 0.0;
    double filterMs = 0.0;
    double cleanupMs = 0.0;
    double impactMs = 0.0;
    double buildViewMs = 0.0;
    double dumpViewMs = 0.0;
    const bool verbose = DerivationGraphViewInterface::isVerboseEnabled();
    if (verbose) {
        std::cout << "[prune-inc] delta-delete counts (start): nodes=" << deltaDeleteNodes.size()
                  << " edges=" << deltaDeleteEdges.size() << std::endl;
    }
    if (DerivationGraphViewInterface::isDumpStatsEnabled()) {
        auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune-inc: dumpStatisticsInc");
        dumpStatisticsInc(std::cout);
        if (incProfile) {
            dumpStatsMs = toMs(t0);
        }
    }
    std::unordered_set<std::string> outputRelationNames;
    outputRelationNames.reserve(outputRelations.size());
    outputRelationNames.insert(outputRelations.begin(), outputRelations.end());
    // Mark reachable nodes and edges.
    std::unordered_set<NodePtr> reachableNodes;
    std::unordered_set<EdgePtr> reachableEdges;
    reachableNodes.reserve(nodes.size());
    reachableEdges.reserve(edges.size());
    std::queue<NodePtr> workQueue;
    std::vector<NodePtr> outputNodes;
    std::vector<NodePtr> evidenceNodes;

    {
        auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune-inc: init outputs");
        // Initialize: start from all output relation nodes.
        for (const auto& node : nodes) {
            if (outputRelationNames.count(node->getTuple().relation_name) > 0 || node->isQueryNode()) {
                reachableNodes.insert(node);
                workQueue.push(node);
                node->setQuery();
                outputNodes.push_back(node);
            }
            if (node->hasEvidence()) {
                evidenceNodes.push_back(node);
            }
        }
        if (incProfile) {
            initOutputsMs = toMs(t0);
        }
    }

    {
        auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune-inc: evidence + backward BFS");
        for (const auto& node : evidenceNodes) {
            if (reachableNodes.insert(node).second) {
                if (verbose) {
                    std::cout << "Found evidence node: " << node->toString()
                              << " with value " << (node->getEvidenceValue() ? "true" : "false")
                              << std::endl;
                }
                workQueue.push(node);
            }
        }

        // Reverse BFS traversal.
        while (!workQueue.empty()) {
            NodePtr current = workQueue.front();
            workQueue.pop();
            if (current->isFact) {
                continue;  // currently skip input facts
            }
            for (const auto& edge : current->getIncomingEdges()) {
                if (edge->hasSelfDependency()) {
                    continue;  // if the edge reports self-dependency, then it is redundant
                }
                reachableEdges.insert(edge);
                for (const auto& inputNode : edge->getInputs()) {
                    if (reachableNodes.insert(inputNode).second) {
                        workQueue.push(inputNode);
                    }
                }
            }
        }
        if (incProfile) {
            bfsMs = toMs(t0);
        }
    }

    std::unordered_set<NodePtr> liveNodes = std::move(reachableNodes);
    std::unordered_set<EdgePtr> liveEdges = std::move(reachableEdges);
    {
        auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune-inc: mark live/pruned nodes+edges");
        for (const auto& node : nodes) {
            if (liveNodes.count(node)){
                if (retainDeltaMetadata && node->pruned) {
                    deltaInsertNodes.insert(node);
                }
                node->pruned = false;  // reset pruned flag
            } else {
                node->pruned = true;
            }
        }

        for (const auto& edge : edges) {
            if (liveEdges.count(edge)) {
                if (retainDeltaMetadata && edge->pruned) {
                    deltaInsertEdges.insert(edge);
                }
                edge->pruned = false;  // reset pruned flag
            } else {
                edge->pruned = true;
            }
        }
        if (incProfile) {
            markMs = toMs(t0);
        }
    }
    if (verbose) {
        std::cout << "[prune-inc] delta-delete counts (post-mark-pruned): nodes=" << deltaDeleteNodes.size()
                  << " edges=" << deltaDeleteEdges.size() << std::endl;
    }

    // Filter nodes and edges.
    std::set<NodePtr> newDeltaDeletedNodes;
    std::set<EdgePtr> newDeltaDeletedEdges;
    {
        auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune-inc: filter delta-deleted");
        for (const auto& deletedNode : deltaDeleteNodes) {
            if (!deletedNode->pruned) {
                newDeltaDeletedNodes.insert(deletedNode);
            }
        }
        for (const auto& deletedEdge : deltaDeleteEdges) {
            if (!deletedEdge->pruned) {
                newDeltaDeletedEdges.insert(deletedEdge);
            }
        }
        if (incProfile) {
            filterMs = toMs(t0);
        }
    }
    if (verbose) {
        std::cout << "[prune-inc] delta-delete counts (filtered): nodes=" << newDeltaDeletedNodes.size()
                  << " edges=" << newDeltaDeletedEdges.size() << std::endl;
    }
    std::vector<NodePtr> deletedOutputNodes;
    deletedOutputNodes.reserve(newDeltaDeletedNodes.size());
    for (const auto& node : newDeltaDeletedNodes) {
        if (outputRelationNames.count(node->getTuple().relation_name) > 0 || node->isQueryNode()) {
            deletedOutputNodes.push_back(node);
        }
    }

    std::set<NodePtr> newDeltaInsertedNodes;
    std::set<EdgePtr> newDeltaInsertedEdges;
    std::set<NodePtr> newDeltaInsertFactNodes;

    for (const auto& insertedNode : deltaInsertNodes) {
        if (liveNodes.count(insertedNode)) {
            newDeltaInsertedNodes.insert(insertedNode);
        }
    }
    for (const auto& insertedEdge : deltaInsertEdges) {
        if (liveEdges.count(insertedEdge)) {
            newDeltaInsertedEdges.insert(insertedEdge);
        }
    }
    for (const auto& insertedNode : deltaInsertFactNodes) {
        if (liveNodes.count(insertedNode)) {
            newDeltaInsertFactNodes.insert(insertedNode);
        }
    }

    // Cleanup self dependencies after reachability pruning.
    {
        auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune-inc: cleanup self-loop edges");
        removeSelfLoopEdges(liveNodes, liveEdges);
        if (incProfile) {
            cleanupMs = toMs(t0);
        }
    }

    if (verbose) {
        std::cout << "[prune-inc] delta-delete counts (canonicalised): nodes=" << newDeltaDeletedNodes.size()
                  << " edges=" << newDeltaDeletedEdges.size() << std::endl;
    }

    std::unordered_set<NodePtr> newInsertedReachableNodes;
    std::unordered_set<EdgePtr> newInsertedReachableEdges;
    std::unordered_map<NodePtr, std::unordered_set<NodePtr>> newInsertedFactImpactedNodes;
    std::unordered_map<NodePtr, std::unordered_set<EdgePtr>> newInsertedFactImpactedEdges;
    std::unordered_map<NodePtr, std::unordered_set<NodePtr>> newDeletedFactImpactedNodes;
    std::unordered_map<NodePtr, std::unordered_set<EdgePtr>> newDeletedFactImpactedEdges;
    std::unordered_set<NodePtr> newDeletedFactImpactDetNodes;
    std::unordered_set<EdgePtr> newDeletedFactImpactDetEdges;
    std::unordered_set<NodePtr> newDeletedFactImpactNonDetNodes;
    std::unordered_set<EdgePtr> newDeletedFactImpactNonDetEdges;
    newInsertedReachableNodes.reserve(newDeltaInsertedNodes.size());
    newInsertedReachableEdges.reserve(newDeltaInsertedEdges.size());
    newInsertedFactImpactedNodes.reserve(newDeltaInsertedNodes.size());
    newInsertedFactImpactedEdges.reserve(newDeltaInsertedNodes.size());
    newDeletedFactImpactedNodes.reserve(newDeltaDeletedNodes.size());
    newDeletedFactImpactedEdges.reserve(newDeltaDeletedNodes.size());
    {
        auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune-inc: rebuild impacted maps");
        double insImpactMs = 0.0, delImpactMs = 0.0;
        size_t insImpactNodes = 0, insImpactEdges = 0, insSources = 0;
        size_t delImpactNodes = 0, delImpactEdges = 0, delSources = 0;
        if (verbose) {
            std::cout << "[prune-inc impact] insSources=" << insSources
                      << " insNodes=" << insImpactNodes << " insEdges=" << insImpactEdges
                      << " insTimeMs=" << insImpactMs
                      << " delSources=" << delSources
                      << " delNodes=" << delImpactNodes << " delEdges=" << delImpactEdges
                      << " delTimeMs=" << delImpactMs
                      << std::endl;
        }
        if (incProfile) {
            impactMs = toMs(t0);
        }
    }

    const size_t liveNodeCount = liveNodes.size();
    const size_t liveEdgeCount = liveEdges.size();
    IncSubgraphView view = [&]() {
        auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune-inc: build view");
        auto built = IncSubgraphView(std::move(liveNodes), std::move(liveEdges),
                               std::move(newDeltaInsertedNodes),
                               std::move(newDeltaInsertedEdges),
                               std::move(newDeltaDeletedNodes),
                               std::move(newDeltaDeletedEdges),
                               std::move(newDeltaInsertFactNodes),
                               std::move(newDeletedFactImpactedNodes),
                               std::move(newDeletedFactImpactedEdges),
                               std::move(newInsertedFactImpactedNodes),
                               std::move(newInsertedFactImpactedEdges),
                               std::move(newInsertedReachableNodes),
                               std::move(newInsertedReachableEdges),
                               std::move(newDeletedFactImpactDetNodes),
                               std::move(newDeletedFactImpactDetEdges),
                               std::move(newDeletedFactImpactNonDetNodes),
                               std::move(newDeletedFactImpactNonDetEdges),
                               explicitDeletedFacts_,
                               std::move(deletedOutputNodes),
                               std::move(outputNodes),
                               std::move(evidenceNodes));
        if (incProfile) {
            buildViewMs = toMs(t0);
        }
        return built;
    }();
    if (DerivationGraphViewInterface::isDumpStatsEnabled()) {
        auto t0 = Clock::now();
        FunctionTimer scopeTimer("prune-inc: dumpStatisticsInc(view)");
        view.dumpStatisticsInc(std::cout);
        if (incProfile) {
            dumpViewMs = toMs(t0);
        }
    }
    if (verbose) {
        std::cout << "[prune-inc] delta-delete counts (view): nodes=" << view.getDeltaDeleteNodes().size()
                  << " edges=" << view.getDeltaDeleteEdges().size() << std::endl;
    }
    if (incProfile) {
        const double totalMs = toMs(totalStart);
        std::cout << "[inc-profile] stage=PRUNING_INC prune_ms=" << totalMs
                  << " dumpstats_ms=" << dumpStatsMs
                  << " init_ms=" << initOutputsMs
                  << " bfs_ms=" << bfsMs
                  << " mark_ms=" << markMs
                  << " filter_ms=" << filterMs
                  << " cleanup_ms=" << cleanupMs
                  << " impact_ms=" << impactMs
                  << " build_view_ms=" << buildViewMs
                  << " dump_view_ms=" << dumpViewMs
                  << " live_nodes=" << liveNodeCount
                  << " live_edges=" << liveEdgeCount
                  << std::endl;
    }
    return view;
}

void DerivationGraph::removeSelfLoopEdges(
        std::unordered_set<NodePtr>& liveNodes, std::unordered_set<EdgePtr>& liveEdges) {
    std::vector<EdgePtr> toRemove;
    for (const auto& e : liveEdges) {
        if (e->hasSelfDependency()) {
            toRemove.push_back(e);
        }
    }
    if (toRemove.empty()) return;

    for (const auto& e : toRemove) {
        e->pruned = true;
        liveEdges.erase(e);
        edges.erase(e);

        auto out = e->getOutput();
        auto& inVec = out->incomingEdges;
        inVec.erase(std::remove(inVec.begin(), inVec.end(), e), inVec.end());
        for (const auto& in : e->getInputs()) {
            auto& outVec = in->outgoingEdges;
            outVec.erase(std::remove(outVec.begin(), outVec.end(), e), outVec.end());
        }
    }

    for (const auto& n : liveNodes) {
        std::vector<EdgePtr> newIn;
        std::vector<EdgePtr> newOut;
        std::unordered_set<EdgePtr> seenIn, seenOut;
        for (const auto& e : n->incomingEdges) {
            if (liveEdges.count(e) && seenIn.insert(e).second) {
                newIn.push_back(e);
            }
        }
        for (const auto& e : n->outgoingEdges) {
            if (liveEdges.count(e) && seenOut.insert(e).second) {
                newOut.push_back(e);
            }
        }
        n->incomingEdges.swap(newIn);
        n->outgoingEdges.swap(newOut);
    }
}

void IncrementalDerivationGraphViewInterface::dumpDotInc(const std::string& filename) const {
    if (!isDumpDotEnabled()) {
        return;
    }
    const std::string path = qualifyDumpPath(filename);
    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    out << "digraph IncrementalDerivationGraph {\n";
    out << "  rankdir=LR;\n";

    // Add legend
    out << "  subgraph cluster_legend {\n";
    out << "    label=\"Legend\";\n";
    out << "    style=filled;\n";
    out << "    color=lightgrey;\n";
    out << "    node [style=filled];\n";
    out << "    \"Normal Node\" [fillcolor=lightblue];\n";
    out << "    \"Inserted Node\" [fillcolor=lightgreen];\n";
    out << "    \"Deleted Node\" [fillcolor=pink, style=\"filled,dashed\"];\n";
    out << "    \"Normal Node\" -> \"Normal Edge\" [color=black];\n";
    out << "    \"Inserted Node\" -> \"Inserted Edge\" [color=green];\n";
    out << "    \"Deleted Node\" -> \"Deleted Edge\" [color=red, style=dashed];\n";
    out << "    \"Normal Edge\" [shape=point, fillcolor=black, width=0.2];\n";
    out << "    \"Inserted Edge\" [shape=point, fillcolor=green, width=0.2];\n";
    out << "    \"Deleted Edge\" [shape=point, fillcolor=red, width=0.2];\n";
    out << "  }\n\n";

    // Normal nodes - blue fill
    out << "  // Regular nodes\n";
    out << "  node [shape=box, style=filled, fillcolor=lightblue];\n";

    for (const auto& node : getNodes()) {
        // Skip inserted and deleted nodes; handled separately.
        if (getDeltaInsertNodes().find(node) == getDeltaInsertNodes().end() &&
            getDeltaDeleteNodes().find(node) == getDeltaDeleteNodes().end()) {
            out << "  node" << node->getId() << " [label=\""
                << node->getTuple().toString();

            // If probability info exists, add it to the label.
            if (node->isFact) {
                out << "\\nP=" << node->getProbability();
            }

            out << "\"];\n";
        }
    }

    if (DerivationGraphViewInterface::isVerboseEnabled()) {
        std::cout << "Dumping " << getNodes().size() << " nodes, "
                  << getEdges().size() << " edges, "
                  << getDeltaInsertNodes().size() << " inserted nodes, "
                  << getDeltaInsertEdges().size() << " inserted edges, "
                  << getDeltaDeleteNodes().size() << " deleted nodes, "
                  << getDeltaDeleteEdges().size() << " deleted edges." << std::endl;
    }

    // Inserted nodes - green fill
    out << "\n  // Inserted nodes\n";
    out << "  node [shape=box, style=filled, fillcolor=lightgreen];\n";

    for (const auto& node : getDeltaInsertNodes()) {
        if (DerivationGraphViewInterface::isVerboseEnabled()) {
            std::cout << "DumpDotInc Inserting node: " << node->getTuple().toString() << std::endl;
        }
        out << "  node" << node->getId() << " [label=\""
            << node->getTuple().toString();

        // If probability info exists, add it to the label.
        if (node->isFact) {
            out << "\\nP=" << node->getProbability();
        }

        out << "\"];\n";
    }

    // Deleted nodes - red dashed fill
    out << "\n  // Deleted nodes\n";
    out << "  node [shape=box, style=\"filled,dashed\", fillcolor=pink];\n";

    for (const auto& node : getDeltaDeleteNodes()) {
        out << "  node" << node->getId() << " [label=\""
            << node->getTuple().toString();

        // If probability info exists, add it to the label.
        if (node->isFact) {
            out << "\\nP=" << node->getProbability();
        }

        out << "\"];\n";
    }

    // Normal edges - black points and lines
    out << "\n  // Regular edges\n";
    out << "  node [shape=point, fillcolor=black, width=0.2];\n";

    for (const auto& edge : getEdges()) {
        // Skip inserted and deleted edges; handled separately.
        if (getDeltaInsertEdges().find(edge) == getDeltaInsertEdges().end() &&
            getDeltaDeleteEdges().find(edge) == getDeltaDeleteEdges().end()) {

            // Add rule ID to the label (if any).
            std::string edgeLabel = "";
            if (edge->getRule() != nullptr) {
                edgeLabel = " [label=\"R" + std::to_string(edge->getRule()->getRuleId()) + "\"";

                // If probability is not 1.0, add probability info.
                if (edge->getProbability() < 1.0) {
                    edgeLabel += ", tooltip=\"P=" + std::to_string(edge->getProbability()) + "\"";
                }

                edgeLabel += "]";
            }
            out << "  edge" << edge->getId() << edgeLabel << ";\n";

            // Input-node to edge connections.
            for (const auto& input : this->getInputs(edge)) {
                // Check whether the body atom is negated.
                size_t inputIdx = std::distance(this->getInputs(edge).begin(),
                                 std::find(this->getInputs(edge).begin(), this->getInputs(edge).end(), input));
                bool isNegated = inputIdx < this->getBodyNegations(edge).size() ?
                                 this->getBodyNegations(edge)[inputIdx] : false;

                // If negated, use a dashed line and a different arrowhead.
                std::string edgeStyle = isNegated ? " [style=dashed, arrowhead=odot]" : "";

                out << "  node" << input->getId()
                    << " -> edge" << edge->getId() << edgeStyle << ";\n";
            }

            // Edge-to-output node connection.
            out << "  edge" << edge->getId()
                << " -> node" << this->getOutput(edge)->getId() << ";\n";
        }
    }

    // Inserted edges - green points and lines
    out << "\n  // Inserted edges\n";
    out << "  node [shape=point, fillcolor=green, width=0.2];\n";

    for (const auto& edge : getDeltaInsertEdges()) {
        // Add rule ID to the label (if any).
        std::string edgeLabel = "";
        if (edge->getRule() != nullptr) {
            edgeLabel = " [label=\"R" + std::to_string(edge->getRule()->getRuleId()) + "\"";

            // If probability is not 1.0, add probability info.
            if (edge->getProbability() < 1.0) {
                edgeLabel += ", tooltip=\"P=" + std::to_string(edge->getProbability()) + "\"";
            }

            edgeLabel += ", color=green]";
        } else {
            edgeLabel = " [color=green]";
        }
        out << "  edge" << edge->getId() << edgeLabel << ";\n";

        // Input-node to edge connection - green
        for (const auto& input : this->getInputs(edge)) {
            // Check whether the body atom is negated.
            size_t inputIdx = std::distance(this->getInputs(edge).begin(),
                             std::find(this->getInputs(edge).begin(), this->getInputs(edge).end(), input));
            bool isNegated = inputIdx < this->getBodyNegations(edge).size() ?
                             this->getBodyNegations(edge)[inputIdx] : false;

            // If negated, use a dashed line and a different arrowhead, while keeping green.
            std::string edgeStyle = isNegated ?
                " [color=green, style=dashed, arrowhead=odot]" : " [color=green]";

            out << "  node" << input->getId()
                << " -> edge" << edge->getId() << edgeStyle << ";\n";
        }

        // Edge-to-output node connection - green
        out << "  edge" << edge->getId()
            << " -> node" << this->getOutput(edge)->getId() << " [color=green];\n";
    }

    // Deleted edges - red dashed points and lines
    out << "\n  // Deleted edges\n";
    out << "  node [shape=point, fillcolor=red, width=0.2];\n";

    for (const auto& edge : getDeltaDeleteEdges()) {
        // Add rule ID to the label (if any).
        std::string edgeLabel = "";
        if (edge->getRule() != nullptr) {
            edgeLabel = " [label=\"R" + std::to_string(edge->getRule()->getRuleId()) + "\"";

            // If probability is not 1.0, add probability info.
            if (edge->getProbability() < 1.0) {
                edgeLabel += ", tooltip=\"P=" + std::to_string(edge->getProbability()) + "\"";
            }

            edgeLabel += ", color=red, style=dashed]";
        } else {
            edgeLabel = " [color=red, style=dashed]";
        }

        out << "  edge" << edge->getId() << edgeLabel << ";\n";

        // Input-node to edge connection - red dashed
        for (const auto& input : edge->getInputs()) {
            // Check whether the body atom is negated.
            size_t inputIdx = std::distance(edge->getInputs().begin(),
                             std::find(edge->getInputs().begin(), edge->getInputs().end(), input));
            bool isNegated = inputIdx < edge->getBodyNegations().size() ?
                             edge->getBodyNegations()[inputIdx] : false;

            // If negated, use a double-dashed line and a different arrowhead, still red.
            std::string edgeStyle = isNegated ?
                " [color=red, style=\"dashed,dotted\", arrowhead=odot]" : " [color=red, style=dashed]";

            out << "  node" << input->getId()
                << " -> edge" << edge->getId() << edgeStyle << ";\n";
        }

        // Edge-to-output node connection - red dashed
        out << "  edge" << edge->getId()
            << " -> node" << edge->getOutput()->getId() << " [color=red, style=dashed];\n";
    }

    out << "}\n";
    out.close();
}

inline std::unordered_map<NodePtr, double> precomputedProbResult;
inline std::unordered_map<std::string, double> precomputedTupleProbResult;
inline std::unordered_map<NodePtr, double> probResult;

void dumpProbabilities(
    std::unordered_map<NodePtr, double>& nodeProbabilities, const std::string& outputDir = "./output/",
          const std::string& fileName = "facts",
          const std::vector<std::pair<NodePtr, NodePtr>>& outputAliases = {},
          const std::vector<NodePtr>& hiddenOutputRoots = {}) {
    std::ofstream outputFile(souffle::joinOutputPath(outputDir, fileName + ".prob"));
    outputFile << std::setprecision(8);
    std::map<std::string, double> tupleProbabilities;
    std::vector<NodePtr> sortedNodes;
    std::unordered_set<NodePtr> seen;
    std::unordered_set<NodePtr> hiddenRoots(hiddenOutputRoots.begin(), hiddenOutputRoots.end());
    std::unordered_set<std::string> hiddenTuples;
    for (const auto& root : hiddenOutputRoots) hiddenTuples.insert(root->getTuple().toString());
    for (const auto& [node, prob] : nodeProbabilities) {
        if (seen.insert(node).second) {
            sortedNodes.push_back(node);
        }
    }
    for (const auto& [node, prob] : precomputedProbResult) {
        if (seen.insert(node).second) {
            sortedNodes.push_back(node);
        }
    }
    for (auto& node: sortedNodes) {
        double prob = 0.0;
        auto it = nodeProbabilities.find(node);
        if (it != nodeProbabilities.end()) {
            prob = it->second;
        } else {
            auto itPre = precomputedProbResult.find(node);
            if (itPre == precomputedProbResult.end()) {
                continue;
            }
            prob = itPre->second;
        }
        if (!hiddenRoots.count(node) && !hiddenTuples.count(node->getTuple().toString()) &&
                (node->needOutput || precomputedProbResult.count(node))) {
            tupleProbabilities.emplace(node->getTuple().toString(), prob);
        }
    }
    for (const auto& [tupleStr, prob] : precomputedTupleProbResult) {
        if (!hiddenTuples.count(tupleStr)) tupleProbabilities.emplace(tupleStr, prob);
    }
    for (const auto& [alias, root] : outputAliases) {
        double probability;
        const auto current = nodeProbabilities.find(root);
        const auto precomputed = precomputedProbResult.find(root);
        if (current != nodeProbabilities.end()) probability = current->second;
        else if (precomputed != precomputedProbResult.end()) probability = precomputed->second;
        else {
            const auto tuple = precomputedTupleProbResult.find(root->getTuple().toString());
            if (tuple == precomputedTupleProbResult.end()) {
                throw std::runtime_error("Missing event-alias output probability for " +
                        alias->getTuple().toString());
            }
            probability = tuple->second;
        }
        // Conditional inference is complete here. Override any old marginal
        // recovered by another rewrite path with the representative's result.
        tupleProbabilities[alias->getTuple().toString()] = probability;
    }
    for (const auto& [tupleStr, prob] : tupleProbabilities) {
        outputFile << tupleStr << " : " << prob << std::endl;
    }
    if (DerivationGraphViewInterface::isVerboseEnabled()) {
        std::cout << "[pipeline] dumpProbabilities nodes=" << sortedNodes.size()
                  << " outputs=" << tupleProbabilities.size()
                  << " prob_nodes=" << nodeProbabilities.size()
                  << " precomputed_nodes=" << precomputedProbResult.size()
                  << " precomputed_tuple_nodes=" << precomputedTupleProbResult.size()
                  << std::endl;
    }

}

struct CycleDependencyGraph {
    const DerivationGraphViewInterface& graph;

    std::vector<std::unordered_set<NodePtr>> nodeCycles;
    std::vector<std::unordered_set<EdgePtr>> edgeCycles;

    std::unordered_map<NodePtr, size_t> nodeToCycleIndex;
    std::unordered_map<EdgePtr, size_t> edgeToCycleIndex;

    std::vector<std::unordered_set<size_t>> dependencies;  // edges: i -> j means i depends on j
    std::vector<std::unordered_set<size_t>> reverseDependencies;
    std::vector<size_t> inDegrees;

    std::unordered_map<NodePtr, size_t> nodeDepthsGlobal;
    std::unordered_map<EdgePtr, size_t> edgeDepthsGlobal;

    std::vector<size_t> cycleToComponent;
    std::unordered_map<NodePtr, size_t> nodeToComponent;
    std::vector<std::vector<std::pair<NodePtr, bool>>> componentEvidences;

    explicit CycleDependencyGraph(const DerivationGraphViewInterface& g) : graph(g) {
        if (depGraphProfileEnabled) {
            DepGraphProfile profile;
            profile.node_count = graph.getNodes().size();
            profile.edge_count = graph.getEdges().size();

            using Clock = std::chrono::steady_clock;
            auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
            auto t0 = Clock::now();

            computeSCCs(&profile);
            auto t1 = Clock::now();
            profile.scc_ms = toMs(t1 - t0);

            computeDependencies(&profile);
            auto t2 = Clock::now();
            profile.dep_ms = toMs(t2 - t1);

            computeComponents(&profile);
            auto t3 = Clock::now();
            profile.comp_ms = toMs(t3 - t2);

            computeDepths(&profile);
            auto t4 = Clock::now();
            profile.depth_ms = toMs(t4 - t3);
            profile.total_ms = toMs(t4 - t0);

            profile.scc_count = nodeCycles.size();
            profile.component_count = componentEvidences.size();

            std::cout << "[dep-graph] timing(ms):"
                      << " scc=" << profile.scc_ms
                      << " scc_tarjan=" << profile.scc_tarjan_ms
                      << " scc_edge_map=" << profile.scc_edge_map_ms
                      << " dep=" << profile.dep_ms
                      << " comp=" << profile.comp_ms
                      << " depth=" << profile.depth_ms
                      << " depth_entry=" << profile.depth_entry_ms
                      << " depth_bfs=" << profile.depth_bfs_ms
                      << " depth_edge=" << profile.depth_edge_ms
                      << " total=" << profile.total_ms
                      << " nodes=" << profile.node_count
                      << " edges=" << profile.edge_count
                      << " sccs=" << profile.scc_count
                      << " deps=" << profile.dep_edges
                      << " components=" << profile.component_count
                      << "\n";
        } else {
            computeSCCs(nullptr);
            computeDependencies(nullptr);
            computeComponents(nullptr);
            computeDepths(nullptr);
        }
    }

    size_t getComponentId(const NodePtr& node) const {
        auto it = nodeToComponent.find(node);
        if (it == nodeToComponent.end()) {
            throw std::runtime_error("Node not found in component map: " + node->toString());
        }
        return it->second;
    }

    const std::vector<std::pair<NodePtr, bool>>& getComponentEvidences(size_t componentId) const {
        if (componentId >= componentEvidences.size()) {
            throw std::runtime_error("Component id out of range: " + std::to_string(componentId));
        }
        return componentEvidences[componentId];
    }

    const std::vector<std::pair<NodePtr, bool>>& getComponentEvidencesForNode(const NodePtr& node) const {
        return getComponentEvidences(getComponentId(node));
    }

    size_t getComponentCount() const {
        return componentEvidences.size();
    }

    void dumpCycles(std::ostream& out = std::cout) const {
        out << "Number of SCCs: " << nodeCycles.size() << "\n";
        for (size_t i = 0; i < nodeCycles.size(); ++i) {
            out << "Cycle " << i << " nodes:\n";
            for (const auto& n : nodeCycles[i]) {
                out << "  " << n->toString() << "\n";
                if (nodeDepthsGlobal.count(n) > 0) {
                    out << "    Depth: " << nodeDepthsGlobal.at(n) << "\n";
                } else {
                    out << "    Depth: N/A\n";
                }
            }
            out << "Cycle " << i << " edges:\n";
            for (const auto& e : edgeCycles[i]) {
                out << "  " << e->toString() << "\n";
                if (edgeDepthsGlobal.count(e) > 0) {
                    out << "    Depth: " << edgeDepthsGlobal.at(e) << "\n";
                } else {
                    out << "    Depth: N/A\n";
                }
            }
            // depth

            out << "Depends on: ";
            for (auto d : dependencies[i]) {
                out << d << " ";
            }
            out << "\nReverse depends on: ";
            for (auto d : reverseDependencies[i]) {
                out << d << " ";
            }
            out << "\nIn-degree: " << inDegrees[i] << "\n";
            out << "\n----\n";
        }
    }


    void dumpDot(const std::string& filename) const {
        if (!DerivationGraphViewInterface::isDumpDotEnabled()) {
            return;
        }
        const std::string path = DerivationGraphViewInterface::qualifyDumpPath(filename);
        std::ofstream out(path);
        if (!out.is_open()) {
            throw std::runtime_error("Cannot open file: " + path);
        }

        out << "digraph CycleDependencyGraph {\n";
        out << "  rankdir=LR;\n";
        out << "  node [shape=box, style=filled, fillcolor=lightyellow];\n";

        // Output each SCC node.
        for (size_t i = 0; i < nodeCycles.size(); ++i) {
            out << "  C" << i << " [label=\"Cycle " << i << "\\n";
            size_t count = 0;
            for (const auto& n : nodeCycles[i]) {
                out << n->getTuple().toString();
                if (++count >= 3) { out << "\\n..."; break; }
                out << "\\n";
            }
            out << "\"];\n";
        }

        // Output dependency edges.
        for (size_t i = 0; i < dependencies.size(); ++i) {
            for (auto dep : dependencies[i]) {
                out << "  C" << dep << " -> C" << i << ";\n";
            }
        }

        out << "}\n";
        out.close();
    }

private:
    struct DepGraphProfile {
        double scc_ms = 0.0;
        double scc_tarjan_ms = 0.0;
        double scc_edge_map_ms = 0.0;
        double dep_ms = 0.0;
        double comp_ms = 0.0;
        double depth_ms = 0.0;
        double depth_entry_ms = 0.0;
        double depth_bfs_ms = 0.0;
        double depth_edge_ms = 0.0;
        double total_ms = 0.0;
        size_t node_count = 0;
        size_t edge_count = 0;
        size_t scc_count = 0;
        size_t dep_edges = 0;
        size_t component_count = 0;
    };

    void computeSCCs(DepGraphProfile* profile) {
        using Clock = std::chrono::steady_clock;
        auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
        size_t index = 0, currentSCC = 0;
        std::unordered_map<NodePtr, size_t> indices, lowlinks;
        std::stack<NodePtr> stack;
        std::unordered_set<NodePtr> onStack;
        Clock::time_point t_tarjan_start;
        if (profile) t_tarjan_start = Clock::now();

        std::function<void(NodePtr)> strongconnect = [&](NodePtr v) {
            indices[v] = lowlinks[v] = index++;
            stack.push(v);
            onStack.insert(v);

            for (auto& edge : graph.getOutgoingEdges(v)) {
                NodePtr w = graph.getOutput(edge);
                if (!indices.count(w)) {
                    strongconnect(w);
                    lowlinks[v] = std::min(lowlinks[v], lowlinks[w]);
                } else if (onStack.count(w)) {
                    lowlinks[v] = std::min(lowlinks[v], indices[w]);
                }
            }

            if (lowlinks[v] == indices[v]) {
                std::unordered_set<NodePtr> scc;
                NodePtr w;
                do {
                    w = stack.top(); stack.pop();
                    onStack.erase(w);
                    scc.insert(w);
                    nodeToCycleIndex[w] = currentSCC;
                } while (w != v);
                nodeCycles.push_back(std::move(scc));
                edgeCycles.emplace_back();
                ++currentSCC;
            }
        };

        for (auto& node : graph.getNodes()) {
            if (!indices.count(node)) {
                strongconnect(node);
            }
        }
        if (profile) {
            profile->scc_tarjan_ms = toMs(Clock::now() - t_tarjan_start);
        }

        Clock::time_point t_edge_start;
        if (profile) t_edge_start = Clock::now();
        for (auto& edge : graph.getEdges()) {
            NodePtr out = graph.getOutput(edge);
            if (nodeToCycleIndex.count(out)) {
                size_t cid = nodeToCycleIndex[out];
                edgeToCycleIndex[edge] = cid;
                edgeCycles[cid].insert(edge);
            }
        }
        if (profile) {
            profile->scc_edge_map_ms = toMs(Clock::now() - t_edge_start);
        }
    }

    void computeDependencies(DepGraphProfile* profile) {
        size_t dep_edges = 0;
        size_t n = nodeCycles.size();
        dependencies.resize(n);
        reverseDependencies.resize(n);
        inDegrees.resize(n, 0);  // ensure all entries initialized

        for (auto& edge : graph.getEdges()) {
            size_t outCid = nodeToCycleIndex[graph.getOutput(edge)];
            for (auto& input : graph.getInputs(edge)) {
                size_t inCid = nodeToCycleIndex[input];
                if (inCid != outCid && !dependencies[outCid].count(inCid)) {
                    dependencies[outCid].insert(inCid);             // out depends on in
                    reverseDependencies[inCid].insert(outCid);      // in is depended on by out
                    inDegrees[outCid]++;
                    dep_edges++;
                }
            }
        }
        if (profile) {
            profile->dep_edges = dep_edges;
        }
    }
    void computeComponents(DepGraphProfile* /*profile*/) {
        const size_t n = nodeCycles.size();
        cycleToComponent.assign(n, std::numeric_limits<size_t>::max());
        std::vector<bool> visited(n, false);
        componentEvidences.clear();
        nodeToComponent.clear();

        for (size_t cid = 0; cid < n; ++cid) {
            if (visited[cid]) {
                continue;
            }
            size_t componentId = componentEvidences.size();
            std::vector<size_t> stack;
            std::vector<size_t> componentCycles;
            stack.push_back(cid);
            visited[cid] = true;

            while (!stack.empty()) {
                size_t cur = stack.back();
                stack.pop_back();
                componentCycles.push_back(cur);

                for (size_t nb : dependencies[cur]) {
                    if (!visited[nb]) {
                        visited[nb] = true;
                        stack.push_back(nb);
                    }
                }
                for (size_t nb : reverseDependencies[cur]) {
                    if (!visited[nb]) {
                        visited[nb] = true;
                        stack.push_back(nb);
                    }
                }
            }

            std::vector<std::pair<NodePtr, bool>> evidences;
            for (size_t cycleId : componentCycles) {
                cycleToComponent[cycleId] = componentId;
                for (const auto& node : nodeCycles[cycleId]) {
                    nodeToComponent[node] = componentId;
                    if (node->hasEvidence()) {
                        evidences.emplace_back(node, node->getEvidenceValue());
                    }
                }
            }
            componentEvidences.push_back(std::move(evidences));
        }
    }
    void computeDepths(DepGraphProfile* profile) {
        using Clock = std::chrono::steady_clock;
        auto toMs = [](auto dur) { return std::chrono::duration<double, std::milli>(dur).count(); };
        double entry_ms = 0.0;
        double bfs_ms = 0.0;
        double edge_ms = 0.0;
        nodeDepthsGlobal.clear();
        edgeDepthsGlobal.clear();

        for (size_t cid = 0; cid < nodeCycles.size(); ++cid) {
            const auto& nodes = nodeCycles[cid];
            const auto& edges = edgeCycles[cid];

            std::queue<NodePtr> q;
            Clock::time_point t_entry_start;
            if (profile) t_entry_start = Clock::now();
            for (NodePtr node : nodes) {
                bool isEntry = false;
                for (auto& inEdge : graph.getIncomingEdges(node)) {
                    for (auto& inNode : graph.getInputs(inEdge)) {
                        if (nodeToCycleIndex[inNode] != cid) {
                            isEntry = true;
                            break;
                        }
                    }
                }
                if (node->isFact || graph.getIncomingEdges(node).empty()) isEntry = true;
                if (isEntry) {
                    nodeDepthsGlobal[node] = 0;
                    q.push(node);
                }
            }
            if (profile) {
                entry_ms += toMs(Clock::now() - t_entry_start);
            }

            Clock::time_point t_bfs_start;
            if (profile) t_bfs_start = Clock::now();
            while (!q.empty()) {
                NodePtr curr = q.front(); q.pop();
                size_t d = nodeDepthsGlobal[curr];
                for (auto& outEdge : graph.getOutgoingEdges(curr)) {
                    NodePtr out = graph.getOutput(outEdge);
                    if (nodeToCycleIndex[out] == cid) {
                        if (!nodeDepthsGlobal.count(out) || nodeDepthsGlobal[out] > d + 1) {
                            nodeDepthsGlobal[out] = d + 1;
                            q.push(out);
                        }
                    }
                }
            }
            if (profile) {
                bfs_ms += toMs(Clock::now() - t_bfs_start);
            }

            Clock::time_point t_edge_start;
            if (profile) t_edge_start = Clock::now();
            for (auto edge : edges) {
                size_t d = 0;
                for (auto in : graph.getInputs(edge)) {
                    if (nodeToCycleIndex[in] == cid && nodeDepthsGlobal.count(in)) {
                        d = std::max(d, nodeDepthsGlobal[in] + 1);
                    }
                }
                edgeDepthsGlobal[edge] = d;
            }
            if (profile) {
                edge_ms += toMs(Clock::now() - t_edge_start);
            }
        }
        if (profile) {
            profile->depth_entry_ms = entry_ms;
            profile->depth_bfs_ms = bfs_ms;
            profile->depth_edge_ms = edge_ms;
        }
    }
};

void DerivationGraph::pruneOutputlessComponents(
        std::unordered_set<NodePtr>& liveNodes,
        std::unordered_set<EdgePtr>& liveEdges,
        std::unordered_set<NodePtr>* reachableNodes,
        std::unordered_set<EdgePtr>* reachableEdges) {
    if (liveNodes.empty()) {
        return;
    }
    struct LocalView : DerivationGraphViewInterface {
        std::unordered_set<NodePtr>& nodes;
        std::unordered_set<EdgePtr>& edges;
        LocalView(std::unordered_set<NodePtr>& n, std::unordered_set<EdgePtr>& e)
                : nodes(n), edges(e) {}
        const std::unordered_set<NodePtr>& getNodes() const override { return nodes; }
        const std::unordered_set<EdgePtr>& getEdges() const override { return edges; }
    };
    LocalView view(liveNodes, liveEdges);
    auto& depGraph = view.getCycleDependencyGraph();
    size_t componentCount = depGraph.getComponentCount();
    if (componentCount == 0) {
        return;
    }
    std::vector<bool> componentHasOutput(componentCount, false);
    for (const auto& node : liveNodes) {
        if (node->needOutput || node->hasEvidence()) {
            componentHasOutput[depGraph.getComponentId(node)] = true;
        }
    }

    bool removed = false;
    std::unordered_set<NodePtr> keptNodes;
    keptNodes.reserve(liveNodes.size());
    for (const auto& node : liveNodes) {
        if (componentHasOutput[depGraph.getComponentId(node)]) {
            keptNodes.insert(node);
        } else {
            node->pruned = true;
            removed = true;
        }
    }
    if (!removed) {
        return;
    }

    std::unordered_set<EdgePtr> keptEdges;
    keptEdges.reserve(liveEdges.size());
    for (const auto& edge : liveEdges) {
        NodePtr out = edge->getOutput();
        if (!keptNodes.count(out)) {
            edge->pruned = true;
            continue;
        }
        bool keep = true;
        for (const auto& in : edge->getInputs()) {
            if (!keptNodes.count(in)) {
                keep = false;
                break;
            }
        }
        if (keep) {
            keptEdges.insert(edge);
        } else {
            edge->pruned = true;
        }
    }

    liveNodes.swap(keptNodes);
    liveEdges.swap(keptEdges);
    if (reachableNodes) {
        *reachableNodes = liveNodes;
    }
    if (reachableEdges) {
        *reachableEdges = liveEdges;
    }
}

CycleDependencyGraph& DerivationGraphViewInterface::getCycleDependencyGraph() const {
    if (!cachedCycleDependencyGraph_) {
        cachedCycleDependencyGraph_ = std::make_shared<CycleDependencyGraph>(*this);
    }
    return *cachedCycleDependencyGraph_;
}

void DerivationGraphViewInterface::clearCycleDependencyGraphCache() const {
    cachedCycleDependencyGraph_.reset();
}

void DerivationGraphViewInterface::writeGraphStatsJson(bool include_heavy) const {
    if (!isDumpStatsEnabled()) {
        return;
    }
    static size_t s_idx = 0;  // Controls output file index
    const bool hasOutputDir = !dumpOutputDir.empty();
    const std::string dir = hasOutputDir ? dumpOutputDir : "output";
    const std::string filename = "graph-" + std::to_string(s_idx++) + ".json";
    const std::string path = hasOutputDir ? qualifyDumpPath(filename) : (dir + "/" + filename);

#if __cplusplus >= 201703L
    // Create directory if missing (C++17)
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#endif

    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    const auto& nodes = getNodes();
    const auto& edges = getEdges();

    // Basic counts
    const size_t num_nodes = nodes.size();
    const size_t num_edges = edges.size();

    // #queries: based on Node isQuery flag
    size_t num_queries = 0;

    // in-degree (exclude facts)
    size_t indeg_sum = 0, indeg_cnt = 0, indeg_max = 0;

    // out-degree (only nodes with outdeg>0)
    size_t outdeg_sum = 0, outdeg_cnt = 0, outdeg_max = 0;

    for (const auto& n : nodes) {
        const size_t indeg = getIncomingEdges(n).size();
        const size_t outdeg = getOutgoingEdges(n).size();

        // avg_in_degree: exclude input facts
        if (!n->isFact) {
            indeg_sum += indeg;
            ++indeg_cnt;
            if (indeg > indeg_max) indeg_max = indeg;
        }

        // avg_out_degree: exclude outdeg==0
        if (outdeg > 0) {
            outdeg_sum += outdeg;
            ++outdeg_cnt;
            if (outdeg > outdeg_max) outdeg_max = outdeg;
        }

        if (n->isQuery) ++num_queries;
    }

    const double avg_in_degree  = indeg_cnt  ? static_cast<double>(indeg_sum)  / indeg_cnt  : 0.0;
    const double avg_out_degree = outdeg_cnt ? static_cast<double>(outdeg_sum) / outdeg_cnt : 0.0;

    // Hyperedge input count (hyperedge arity)
    size_t inp_sum = 0, inp_cnt = 0, inp_max = 0;
    for (const auto& e : edges) {
        const size_t k = getInputs(e).size();
        inp_sum += k;
        ++inp_cnt;
        if (k > inp_max) inp_max = k;
    }
    const double avg_hyperedge_inputs = inp_cnt ? static_cast<double>(inp_sum) / inp_cnt : 0.0;

    // Only output numeric values (keys are strings, values are numbers)
    out.setf(std::ios::fixed);
    out << std::setprecision(6);
    out << "{\n"
        << "  \"nodes\": " << num_nodes << ",\n"
        << "  \"edges\": " << num_edges << ",\n"
        << "  \"queries\": " << num_queries << ",\n"
        << "  \"avg_in_degree\": " << avg_in_degree << ",\n"
        << "  \"max_in_degree\": " << indeg_max << ",\n"
        << "  \"avg_out_degree\": " << avg_out_degree << ",\n"
        << "  \"max_out_degree\": " << outdeg_max << ",\n"
        << "  \"avg_hyperedge_inputs\": " << avg_hyperedge_inputs << ",\n"
        << "  \"max_hyperedge_inputs\": " << inp_max;
    if (include_heavy) {
        // Cycle stats (based on SCC; only non-trivial cycles |SCC|>=2)
        CycleDependencyGraph cdg(*this);
        size_t cycles = 0, cyc_size_sum = 0, cyc_size_max = 0;
        for (const auto& scc : cdg.nodeCycles) {
            const size_t s = scc.size();
            if (s >= 2) {
                ++cycles;
                cyc_size_sum += s;
                if (s > cyc_size_max) cyc_size_max = s;
            }
        }
        const double avg_cycle_size = cycles ? static_cast<double>(cyc_size_sum) / cycles : 0.0;
        out << ",\n"
            << "  \"cycles\": " << cycles << ",\n"
            << "  \"avg_cycle_size\": " << avg_cycle_size << ",\n"
            << "  \"max_cycle_size\": " << cyc_size_max;
    }
    out << "\n}\n";
}

// ====================== dumpJsonInc implementation ======================
void IncrementalDerivationGraphViewInterface::dumpJsonInc(const std::string& filename) const {
    using json11::Json;
    if (!DerivationGraphViewInterface::isDumpJsonEnabled()) {
        return;
    }

    auto fact_to_json = [](const NodePtr& n) -> Json {
        return Json::object{
            {"name", n->getTuple().toString()},
            {"probability", n->getProbability()}
        };
    };

    auto edge_to_json = [this](const EdgePtr& e) -> Json {
        Json bodies = Json::array();
        auto inputs = this->getInputs(e);
        auto negs   = this->getBodyNegations(e);
        for (size_t i = 0; i < inputs.size(); ++i) {
            bool neg = (i < negs.size()) ? negs[i] : false;
            json_array_append(bodies, Json::object{
                {"negation", neg},
                {"name", inputs[i]->getTuple().toString()}
            });
        }
        NodePtr head = this->getOutput(e);
        return Json::object{
            {"head", head ? head->getTuple().toString() : std::string("<null-head>")},
            {"probability", e->getProbability()},
            {"bodies", bodies}
        };
    };
    // For delta edges, avoid view-filtered accessors so we can emit full head/body info
    // even if the edge's nodes are no longer in the view after prune.
    auto edge_to_json_raw = [](const EdgePtr& e) -> Json {
        Json bodies = Json::array();
        const auto& inputs = e->getInputsStable();
        const auto& negs   = e->getBodyNegationsStable();
        for (size_t i = 0; i < inputs.size(); ++i) {
            bool neg = (i < negs.size()) ? negs[i] : false;
            json_array_append(bodies, Json::object{
                {"negation", neg},
                {"name", inputs[i]->getTuple().toString()}
            });
        }
        NodePtr head = e->getOutput();
        return Json::object{
            {"head", head ? head->getTuple().toString() : std::string("<null-head>")},
            {"probability", e->getProbability()},
            {"bodies", bodies}
        };
    };

    // Main body: use the "valid subgraph", excluding delta-delete.
    Json facts = Json::array();
    for (const auto& n : this->getValidNodes()) {
        if (n->isFact) {
            json_array_append(facts, fact_to_json(n));
        }
    }

    Json rules = Json::array();
    for (const auto& e : this->getValidEdges()) {
        json_array_append(rules, edge_to_json(e));
    }

    // delta.insert
    Json ins_nodes = Json::array();
    Json ins_edges = Json::array();
    Json ins_facts = Json::array();
    for (const auto& n : this->getDeltaInsertNodes()) {
        json_array_append(ins_nodes, n->getTuple().toString());
        if (n->isFact) json_array_append(ins_facts, fact_to_json(n));
    }
    for (const auto& e : this->getDeltaInsertEdges()) {
        json_array_append(ins_edges, edge_to_json(e));
    }

    // delta.delete
    Json del_nodes = Json::array();
    Json del_edges = Json::array();
    Json del_facts = Json::array();
    for (const auto& n : this->getDeltaDeleteNodes()) {
        json_array_append(del_nodes, n->getTuple().toString());
        if (n->isFact) 
            json_array_append(del_facts, fact_to_json(n));
    }
    for (const auto& e : this->getDeltaDeleteEdges()) {
        json_array_append(del_edges, edge_to_json_raw(e));
    }

    Json root = Json::object{
        {"facts", facts},
        {"rules", rules},
        {"delta", Json::object{
            {"insert", Json::object{
                {"nodes", ins_nodes}, {"edges", ins_edges}, {"facts", ins_facts}
            }},
            {"delete", Json::object{
                {"nodes", del_nodes}, {"edges", del_edges}, {"facts", del_facts}
            }}
        }}
    };

    const std::string path = qualifyDumpPath(filename);
    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    out << root.dump();
    out.close();
}

#endif //DERIVATIONGRAPH_H
