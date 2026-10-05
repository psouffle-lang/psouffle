#ifndef SOUFFLE_PROBLOG_PIPELINE_COMPONENTS_H
#define SOUFFLE_PROBLOG_PIPELINE_COMPONENTS_H

#include "souffle/problog/ForwardCompilation.h"

#include <algorithm>
#include <chrono>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace souffle::problog {

struct FastComponentEval {
    ComponentSubgraph comp;
    SingleRandVarInfo var;
    std::unordered_map<NodePtr, bool> valuesTrue;
    std::unordered_map<NodePtr, bool> valuesFalse;
    double evalMsTrue = 0.0;
    double evalMsFalse = 0.0;
};

struct ConjComponentEval {
    ComponentSubgraph comp;
    std::unordered_map<NodePtr, double> probabilities;
    double evalMs = 0.0;
};

struct ComponentAnalysis {
    ComponentSubgraph comp;
    SingleRandVarInfo singleRand;
    std::size_t randVars = 0;
    bool hasNegation = false;
    bool hasOr = false;
    bool hasCycle = false;
};

struct SlowComponentEval {
    ComponentSubgraph comp;
    std::size_t randVars = 0;
};

struct FastComponentStats {
    std::size_t candidates = 0;
    std::size_t used = 0;
    std::size_t skipped = 0;
    long long evalMs = 0;
};

struct ConjFastStats {
    std::size_t candidates = 0;
    std::size_t used = 0;
    std::size_t skipped = 0;
    long long evalMs = 0;
};

struct ComponentDecision {
    std::size_t id = 0;
    std::size_t nodes = 0;
    std::size_t edges = 0;
    std::size_t randVars = 0;
    bool hasEvidence = false;
    bool hasNegation = false;
    bool hasOr = false;
    bool hasCycle = false;
    std::string mode;
    std::string reason;
};

inline std::vector<std::vector<std::pair<NodePtr, bool>>> groupEvidencesByComponentMap(
        std::size_t componentCount,
        const std::unordered_map<NodePtr, std::size_t>& nodeToComponent,
        const std::vector<std::pair<NodePtr, bool>>& evidences) {
    std::vector<std::vector<std::pair<NodePtr, bool>>> byComponent(componentCount);
    std::vector<std::unordered_map<NodePtr, bool>> seen(componentCount);

    for (const auto& ev : evidences) {
        const NodePtr& node = ev.first;
        auto compIt = nodeToComponent.find(node);
        if (!node || compIt == nodeToComponent.end()) {
            throw std::runtime_error("Evidence node not found in component map: " +
                    (node ? node->toString() : std::string("null")));
        }
        const std::size_t cid = compIt->second;
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

inline std::vector<std::vector<std::pair<NodePtr, bool>>> groupEvidencesByComponent(
        const std::vector<ComponentSubgraph>& components,
        const std::vector<std::pair<NodePtr, bool>>& evidences) {
    if (evidences.empty()) {
        std::size_t componentCount = 0;
        for (const auto& comp : components) {
            componentCount = std::max(componentCount, comp.id + 1);
        }
        return std::vector<std::vector<std::pair<NodePtr, bool>>>(componentCount);
    }
    std::unordered_map<NodePtr, std::size_t> nodeToComponent;
    std::size_t componentCount = 0;
    for (const auto& comp : components) {
        componentCount = std::max(componentCount, comp.id + 1);
        for (const auto& node : comp.nodes) {
            nodeToComponent.emplace(node, comp.id);
        }
    }
    return groupEvidencesByComponentMap(componentCount, nodeToComponent, evidences);
}

inline std::vector<std::vector<std::pair<NodePtr, bool>>> groupEvidencesByComponent(
        const std::vector<ComponentAnalysis>& analyses,
        const std::vector<std::pair<NodePtr, bool>>& evidences) {
    if (evidences.empty()) {
        std::size_t componentCount = 0;
        for (const auto& analysis : analyses) {
            componentCount = std::max(componentCount, analysis.comp.id + 1);
        }
        return std::vector<std::vector<std::pair<NodePtr, bool>>>(componentCount);
    }
    std::unordered_map<NodePtr, std::size_t> nodeToComponent;
    std::size_t componentCount = 0;
    for (const auto& analysis : analyses) {
        componentCount = std::max(componentCount, analysis.comp.id + 1);
        for (const auto& node : analysis.comp.nodes) {
            nodeToComponent.emplace(node, analysis.comp.id);
        }
    }
    return groupEvidencesByComponentMap(componentCount, nodeToComponent, evidences);
}

inline bool evaluateZeroRandConjComponent(
        const ComponentSubgraph& comp,
        std::unordered_map<NodePtr, double>& nodeProbs,
        double* evalMs = nullptr) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    BorrowedComponentSubgraphView subview(comp);
    std::unordered_map<NodePtr, std::size_t> indegree;
    indegree.reserve(comp.nodes.size());
    for (const auto& node : comp.nodes) {
        if (node) {
            indegree.emplace(node, 0);
        }
    }
    for (const auto& edge : comp.edges) {
        if (!edge) {
            continue;
        }
        NodePtr out = subview.getOutput(edge);
        if (out) {
            ++indegree[out];
        }
    }

    std::queue<NodePtr> ready;
    std::vector<NodePtr> topo;
    topo.reserve(comp.nodes.size());
    for (const auto& node : comp.nodes) {
        if (node && indegree[node] == 0) {
            ready.push(node);
        }
    }
    while (!ready.empty()) {
        NodePtr node = ready.front();
        ready.pop();
        topo.push_back(node);
        for (const auto& edge : subview.getOutgoingEdges(node)) {
            NodePtr out = subview.getOutput(edge);
            if (!out) {
                continue;
            }
            auto it = indegree.find(out);
            if (it == indegree.end() || it->second == 0) {
                continue;
            }
            --it->second;
            if (it->second == 0) {
                ready.push(out);
            }
        }
    }
    if (topo.size() != comp.nodes.size()) {
        return false;
    }

    nodeProbs.clear();
    nodeProbs.reserve(comp.nodes.size());
    for (const auto& node : topo) {
        if (!node) {
            continue;
        }
        const auto& incoming = subview.getIncomingEdges(node);
        if (incoming.empty()) {
            nodeProbs[node] = node->isFact ? node->getProbability() : 0.0;
            continue;
        }
        if (incoming.size() != 1) {
            return false;
        }
        const auto& edge = incoming.front();
        double prob = edge->isDeterministic() ? 1.0 : edge->getProbability();
        const auto& inputs = subview.getInputs(edge);
        const auto& negs = subview.getBodyNegations(edge);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            auto it = nodeProbs.find(inputs[i]);
            if (it == nodeProbs.end()) {
                return false;
            }
            double inputProb = it->second;
            if (i < negs.size() && negs[i]) {
                inputProb = 1.0 - inputProb;
            }
            prob *= inputProb;
        }
        nodeProbs[node] = prob;
    }
    if (evalMs) {
        *evalMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    return true;
}

inline std::vector<ComponentAnalysis> analyzeComponents(
        const DerivationGraphViewInterface& view,
        std::vector<ComponentSubgraph> components,
        bool needCycleInfo = true) {
    auto isSemanticRandomProb = [](double p) {
        return p > 0.0 && p < 1.0;
    };
    std::vector<ComponentAnalysis> analyses;
    analyses.reserve(components.size());

    for (auto& comp : components) {
        ComponentAnalysis analysis;
        analysis.comp = std::move(comp);
        analysis.singleRand = SingleRandVarInfo{};
        std::unordered_map<NodePtr, std::size_t> incomingCounts;
        incomingCounts.reserve(analysis.comp.nodes.size());

        for (const auto& node : analysis.comp.nodes) {
            if (node->isFact && isSemanticRandomProb(node->getProbability())) {
                analysis.randVars++;
                if (analysis.randVars == 1) {
                    analysis.singleRand.node = node;
                    analysis.singleRand.edge.reset();
                    analysis.singleRand.probability = node->getProbability();
                }
            }
        }

        for (const auto& edge : analysis.comp.edges) {
            if (isSemanticRandomProb(edge->getProbability())) {
                analysis.randVars++;
                if (analysis.randVars == 1) {
                    analysis.singleRand.node.reset();
                    analysis.singleRand.edge = edge;
                    analysis.singleRand.probability = edge->getProbability();
                }
            }
            auto negs = view.getBodyNegations(edge);
            for (bool neg : negs) {
                if (neg) {
                    analysis.hasNegation = true;
                    break;
                }
            }
            NodePtr out = view.getOutput(edge);
            if (out) {
                incomingCounts[out]++;
                for (const auto& in : view.getInputs(edge)) {
                    if (in == out) {
                        analysis.hasCycle = true;
                        break;
                    }
                }
            }
        }

        if (needCycleInfo && !analysis.hasCycle) {
            std::unordered_map<NodePtr, std::size_t> indegree;
            indegree.reserve(analysis.comp.nodes.size());
            for (const auto& node : analysis.comp.nodes) {
                indegree.emplace(node, 0);
            }
            for (const auto& edge : analysis.comp.edges) {
                NodePtr out = view.getOutput(edge);
                auto outIt = indegree.find(out);
                if (outIt == indegree.end()) {
                    continue;
                }
                for (const auto& in : view.getInputs(edge)) {
                    if (indegree.count(in) > 0) {
                        ++outIt->second;
                    }
                }
            }
            std::queue<NodePtr> ready;
            for (const auto& [node, degree] : indegree) {
                if (degree == 0) {
                    ready.push(node);
                }
            }
            std::size_t visited = 0;
            while (!ready.empty()) {
                NodePtr node = ready.front();
                ready.pop();
                ++visited;
                for (const auto& edge : view.getOutgoingEdges(node)) {
                    NodePtr out = view.getOutput(edge);
                    auto outIt = indegree.find(out);
                    if (outIt == indegree.end()) {
                        continue;
                    }
                    bool dependsOnNode = false;
                    for (const auto& in : view.getInputs(edge)) {
                        if (in == node) {
                            dependsOnNode = true;
                            break;
                        }
                    }
                    if (!dependsOnNode || outIt->second == 0) {
                        continue;
                    }
                    --outIt->second;
                    if (outIt->second == 0) {
                        ready.push(out);
                    }
                }
            }
            analysis.hasCycle = visited != analysis.comp.nodes.size();
        }

        for (const auto& node : analysis.comp.nodes) {
            auto it = incomingCounts.find(node);
            if (it != incomingCounts.end()) {
                if (it->second > 1) {
                    analysis.hasOr = true;
                }
                if (node->isFact && it->second > 0 && isSemanticRandomProb(node->getProbability())) {
                    analysis.hasOr = true;
                }
            }
        }

        analyses.push_back(std::move(analysis));
    }

    return analyses;
}

} // namespace souffle::problog

#endif
