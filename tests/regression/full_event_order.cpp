#include "souffle/problog/ForwardCompilation.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The focused header-level test does not link the pipeline entry points.
Debugger& debugger = Debugger::getInstance();
void assertProbabilityInRange(double probability, const std::string& context) {
    if (!(probability >= 0.0 && probability <= 1.0)) throw std::runtime_error(context);
}

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void close(double actual, double expected, const std::string& label) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-11) {
        throw std::runtime_error(label + ": actual=" + std::to_string(actual) +
                " expected=" + std::to_string(expected));
    }
}

WeightedBDDManager::InitConfig smallConfig() {
    WeightedBDDManager::InitConfig config;
    config.numVars = 0;
    config.numSlots = 256;
    config.cacheSize = 256;
    config.maxMemory = 64UL * 1024 * 1024;
    return config;
}

struct Fixture {
    WorkingDerivationGraph graph;
    std::vector<NodePtr> nodes;
    std::vector<EdgePtr> edges;
    souffle::RamDomain nextRule = 100;

    NodePtr node(const std::string& name) {
        auto result = graph.createNode(UntypedTuple{name, {}});
        nodes.push_back(result);
        return result;
    }

    NodePtr fact(const std::string& name, double probability) {
        auto result = node(name);
        result->isFact = true;
        result->setOriginalFact();
        result->setProbability(probability);
        return result;
    }

    EdgePtr edge(const std::vector<NodePtr>& inputs, const NodePtr& output,
            double probability = 1.0, const std::vector<bool>& negative = {}) {
        RuleApplication application;
        application.ruleId = nextRule++;
        auto result = inputs.empty() ? graph.createHyperedge(inputs, output, application)
                                    : graph.createHyperedge(inputs, output, nullptr, negative, application);
        result->setProbability(probability);
        edges.push_back(result);
        return result;
    }

    SubgraphView view(bool reverse) const {
        // Keep few buckets so opposite insertion orders exercise different
        // actual iteration orders, rather than only different source vectors.
        std::unordered_set<NodePtr> nodeSet;
        std::unordered_set<EdgePtr> edgeSet;
        nodeSet.max_load_factor(1000.0);
        edgeSet.max_load_factor(1000.0);
        nodeSet.rehash(1);
        edgeSet.rehash(1);
        if (reverse) {
            nodeSet.insert(nodes.rbegin(), nodes.rend());
            edgeSet.insert(edges.rbegin(), edges.rend());
        } else {
            nodeSet.insert(nodes.begin(), nodes.end());
            edgeSet.insert(edges.begin(), edges.end());
        }
        return SubgraphView(std::move(nodeSet), std::move(edgeSet));
    }
};

using Truth = std::unordered_map<NodePtr, bool>;

// The oracle keys facts by semantic identity and rules by their exact edge.
// Its events are independent of CUDD variable numbers and iteration order.
struct Worlds {
    std::unordered_map<std::size_t, std::size_t> facts;
    std::unordered_map<EdgePtr, std::size_t> rules;
    std::unordered_map<NodePtr, std::vector<EdgePtr>> incoming;
    std::vector<double> probabilities;
    std::vector<Truth> truth;
    std::vector<double> masses;

    Worlds(const Fixture& fixture, const std::unordered_set<NodePtr>& seeds) {
        for (const auto& node : fixture.nodes) {
            if (node->isFact && node->getProbability() > 0.0 && node->getProbability() < 1.0 &&
                    !facts.count(node->getSemanticFactId())) {
                facts.emplace(node->getSemanticFactId(), probabilities.size());
                probabilities.push_back(node->getProbability());
            }
        }
        for (const auto& edge : fixture.edges) {
            incoming[edge->getOutput()].push_back(edge);
            if (edge->getProbability() > 0.0 && edge->getProbability() < 1.0) {
                rules.emplace(edge, probabilities.size());
                probabilities.push_back(edge->getProbability());
            }
        }
        require(probabilities.size() < 12, "world fixture too large");
        for (std::uint64_t world = 0; world < (std::uint64_t{1} << probabilities.size()); ++world) {
            double mass = 1.0;
            for (std::size_t bit = 0; bit < probabilities.size(); ++bit) {
                mass *= (world & (std::uint64_t{1} << bit)) ? probabilities[bit] : 1.0 - probabilities[bit];
            }
            masses.push_back(mass);
            Truth values;
            std::unordered_set<NodePtr> visiting;
            std::function<bool(const NodePtr&)> visit = [&](const NodePtr& node) {
                const auto known = values.find(node);
                if (known != values.end()) return known->second;
                require(visiting.insert(node).second, "world fixture contains a cycle");
                bool value = seeds.count(node) || (node->isFact && node->getProbability() == 1.0);
                const auto bit = facts.find(node->getSemanticFactId());
                if (!seeds.count(node) && node->isFact && bit != facts.end()) {
                    value = (world & (std::uint64_t{1} << bit->second)) != 0;
                }
                for (const auto& edge : incoming[node]) {
                    bool contribution = edge->isDeterministic();
                    const auto rule = rules.find(edge);
                    if (rule != rules.end()) contribution = (world & (std::uint64_t{1} << rule->second)) != 0;
                    for (std::size_t i = 0; i < edge->getInputs().size(); ++i) {
                        const bool input = visit(edge->getInputs()[i]);
                        contribution &= edge->getBodyNegations()[i] ? !input : input;
                    }
                    value |= contribution;
                }
                visiting.erase(node);
                values.emplace(node, value);
                return value;
            };
            for (const auto& node : fixture.nodes) visit(node);
            truth.push_back(std::move(values));
        }
    }

    double probability(const std::vector<std::pair<NodePtr, bool>>& literals) const {
        double result = 0.0;
        for (std::size_t world = 0; world < truth.size(); ++world) {
            bool matches = true;
            for (const auto& [node, positive] : literals) matches &= truth[world].at(node) == positive;
            if (matches) result += masses[world];
        }
        return result;
    }
};

using NodeFormulas = std::map<NodePtr, BddNodeRef>;
using EdgeFormulas = std::map<EdgePtr, BddNodeRef>;

struct EventIndices {
    std::map<std::size_t, int> facts;
    std::map<std::size_t, int> rules;
    bool operator==(const EventIndices& other) const { return facts == other.facts && rules == other.rules; }
};

EventIndices indices(WeightedBDDManager& manager, const Fixture& fixture, bool initialized) {
    EventIndices result;
    std::unordered_set<int> unique;
    for (const auto& node : fixture.nodes) {
        int index = -1;
        const bool found = manager.peekVarIndex(*node, index);
        require(found == (node->isFact && (initialized || node->getProbability() < 1.0)),
                "fact registration filter changed for " + node->toString());
        if (found) {
            const auto inserted = result.facts.emplace(node->getSemanticFactId(), index);
            require(inserted.second || inserted.first->second == index, "fact aliases have different indices");
            unique.insert(index);
        }
    }
    for (const auto& edge : fixture.edges) {
        int index = -1;
        const bool found = manager.peekVarIndex(*edge, index);
        require(found == !edge->isDeterministic(), "rule registration filter changed");
        if (found) {
            require(unique.insert(index).second, "independent event identities share an index");
            result.rules.emplace(edge->getId(), index);
        }
    }
    require(unique.size() == static_cast<std::size_t>(manager.nextVarIndex_), "unexpected event-index slots");
    return result;
}

void checkProbabilities(WeightedBDDManager& manager, const Fixture& fixture,
        const NodeFormulas& formulas, const Worlds& worlds, const NodePtr& evidence) {
    for (const auto& node : fixture.nodes) {
        require(formulas.count(node), "missing node formula");
        close(manager.computeWeightedModelCount(formulas.at(node)), worlds.probability({{node, true}}),
                "marginal " + node->toString());
        for (const auto& other : fixture.nodes) {
            const auto joint = manager.makeAnd(formulas.at(node), formulas.at(other));
            close(manager.computeWeightedModelCount(joint), worlds.probability({{node, true}, {other, true}}),
                    "joint " + node->toString() + "/" + other->toString());
        }
        for (bool positive : {true, false}) {
            const auto observation = positive ? formulas.at(evidence) : manager.makeNot(formulas.at(evidence));
            const double denominator = manager.computeWeightedModelCount(observation);
            const double expectedEvidence = worlds.probability({{evidence, positive}});
            close(denominator, expectedEvidence, "evidence mass");
            require(expectedEvidence > 0.0, "fixture evidence has zero mass");
            const double posterior = manager.computeWeightedModelCount(manager.makeAnd(formulas.at(node), observation)) /
                    denominator;
            close(posterior, worlds.probability({{node, true}, {evidence, positive}}) / expectedEvidence,
                    "posterior " + node->toString());
        }
    }
}

void standaloneOrderPreservesEventsAndProbabilities() {
    Fixture fixture;
    const auto a = fixture.fact("A", 0.6);
    const auto alias = fixture.fact("AliasA", 0.6);
    alias->setSemanticFactId(a->getSemanticFactId());
    const auto b = fixture.fact("B", 0.4);
    const auto zero = fixture.fact("Zero", 0.0);
    const auto one = fixture.fact("One", 1.0);
    const auto seed = fixture.fact("Seed", 0.3);
    const auto x = fixture.node("X");
    const auto y = fixture.node("Y");
    const auto negative = fixture.node("Negative");
    const auto joint = fixture.node("Joint");
    const auto zeroRule = fixture.node("ZeroRule");
    const auto aliasJoint = fixture.node("AliasJoint");
    const auto seedResult = fixture.node("SeedResult");
    fixture.edge({a, b}, x, 0.7);
    fixture.edge({alias, b}, x, 0.2);
    fixture.edge({x, one}, y);
    fixture.edge({a, b}, negative, 0.45, {false, true});
    fixture.edge({x, negative}, joint);
    fixture.edge({a}, zeroRule, 0.0);
    fixture.edge({a, alias}, aliasJoint);
    fixture.edge({seed}, seedResult);
    fixture.edge({b}, zero);
    const std::unordered_set<NodePtr> seeds{seed};
    auto firstView = fixture.view(false);
    auto reverseView = fixture.view(true);
    require(std::vector<NodePtr>(firstView.getNodes().begin(), firstView.getNodes().end()) !=
                    std::vector<NodePtr>(reverseView.getNodes().begin(), reverseView.getNodes().end()),
            "views did not exercise different node iteration orders");
    require(std::vector<EdgePtr>(firstView.getEdges().begin(), firstView.getEdges().end()) !=
                    std::vector<EdgePtr>(reverseView.getEdges().begin(), reverseView.getEdges().end()),
            "views did not exercise different edge iteration orders");
    Worlds worlds(fixture, seeds);
    WeightedBDDManager first(smallConfig());
    WeightedBDDManager reverse(smallConfig());
    NodeFormulas firstNodes, reverseNodes;
    EdgeFormulas firstEdges, reverseEdges;
    buildFormulasCyclewiseStandaloneFull(firstView, first, firstNodes, firstEdges, seeds);
    buildFormulasCyclewiseStandaloneFull(reverseView, reverse, reverseNodes, reverseEdges, seeds);
    const auto expectedIndices = indices(first, fixture, true);
    require(indices(reverse, fixture, true) == expectedIndices, "full event order depends on view insertion");
    require(expectedIndices.facts.at(one->getSemanticFactId()) == first.nextVarIndex_ - 1,
            "probability-one fact slot moved before stochastic events");
    for (const auto& node : fixture.nodes) {
        if (!node->isFact || seeds.count(node)) continue;
        const auto weight = first.getVariableWeight(expectedIndices.facts.at(node->getSemanticFactId()));
        close(weight.posWeight, node->getProbability(), "fact positive weight");
        close(weight.negWeight, 1.0 - node->getProbability(), "fact negative weight");
    }
    for (const auto& edge : fixture.edges) {
        if (edge->isDeterministic()) continue;
        const auto weight = first.getVariableWeight(expectedIndices.rules.at(edge->getId()));
        close(weight.posWeight, edge->getProbability(), "rule positive weight");
        close(weight.negWeight, 1.0 - edge->getProbability(), "rule negative weight");
    }
    checkProbabilities(first, fixture, firstNodes, worlds, x);
    checkProbabilities(reverse, fixture, reverseNodes, worlds, x);

    // Default full entry remains available and gives the same semantic events.
    WeightedBDDManager ordinary(smallConfig());
    NodeFormulas ordinaryNodes;
    EdgeFormulas ordinaryEdges;
    buildFormulasCyclewise(reverseView, ordinary, ordinaryNodes, ordinaryEdges, seeds);
    indices(ordinary, fixture, true);
    checkProbabilities(ordinary, fixture, ordinaryNodes, worlds, x);

    firstNodes.clear();
    firstEdges.clear();
    first.reset();
    require(first.nextVarIndex_ == 0 && first.nodeIndex_.empty() && first.edgeIndex_.empty(),
            "soft reset retained event mappings");
    int index = -1;
    require(!first.peekVarIndex(*a, index) && !first.hasVariableWeight(0), "soft reset retained alias or weight state");
    buildFormulasCyclewiseStandaloneFull(reverseView, first, firstNodes, firstEdges, seeds);
    require(indices(first, fixture, true) == expectedIndices, "soft reset changed canonical semantic indices");
    checkProbabilities(first, fixture, firstNodes, worlds, x);
}

void preConfigPreservesDefaultAndDeltaPolicies() {
    Fixture fixture;
    const auto old = fixture.fact("Old", 0.6);
    const auto inserted = fixture.fact("Inserted", 0.0);
    const auto one = fixture.fact("One", 1.0);
    const auto oldHead = fixture.node("OldHead");
    const auto newHead = fixture.node("NewHead");
    const auto oldEdge = fixture.edge({old}, oldHead, 0.4);
    const auto newEdge = fixture.edge({inserted}, newHead, 0.0);
    fixture.edge({one}, newHead);
    auto view = fixture.view(false);
    WeightedBDDManager ordered(smallConfig());
    ordered.preConfigOrdered(view, collectSortedNodes(view.getNodes()), collectSortedEdges(view.getEdges()));
    indices(ordered, fixture, false);

    WeightedBDDManager defaults(smallConfig());
    defaults.preConfig(view);
    indices(defaults, fixture, false);
    IncSubgraphView insertView(view.getNodes(), view.getEdges(), {inserted}, {newEdge}, {}, {});
    WeightedBDDManager delta(smallConfig());
    delta.preConfig(insertView);
    int index = -1;
    require(delta.peekVarIndex(*inserted, index) && index == 0, "delta omitted inserted probability-zero fact");
    require(delta.peekVarIndex(*newEdge, index) && index == 1, "delta omitted inserted probability-zero rule");
    require(!delta.peekVarIndex(*old, index) && !delta.peekVarIndex(*oldEdge, index), "delta scanned old full-view events");
    require(!delta.peekVarIndex(*one, index), "preConfig allocated probability-one fact slot");
    require(delta.nextVarIndex_ == 2, "delta allocated unexpected events");
    delta.preConfig(insertView);
    require(delta.nextVarIndex_ == 2, "repeated delta changed existing indices");

    // A deletion-only delta is still a delta: it must not trigger a full scan.
    IncSubgraphView deleteView(view.getNodes(), view.getEdges(), {}, {}, {old}, {oldEdge});
    delta.preConfig(deleteView);
    require(delta.nextVarIndex_ == 2 && !delta.peekVarIndex(*old, index), "deletion-only delta used full registration");
    WeightedBDDManager deletionOnly(smallConfig());
    deletionOnly.preConfig(deleteView);
    require(deletionOnly.nextVarIndex_ == 0, "empty insertions registered deletion-only events");

    // The existing no-delta incremental fallback deliberately scans the view.
    IncSubgraphView noDelta(view.getNodes(), view.getEdges(), {}, {}, {}, {});
    delta.preConfig(noDelta);
    require(delta.peekVarIndex(*inserted, index) && index == 0, "no-delta scan remapped inserted fact");
    require(delta.peekVarIndex(*newEdge, index) && index == 1, "no-delta scan remapped inserted rule");
    indices(delta, fixture, false);
}

}  // namespace

int main() {
    try {
        standaloneOrderPreservesEventsAndProbabilities();
        preConfigPreservesDefaultAndDeltaPolicies();
        std::cout << "full event order tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "full event order regression failed: " << exception.what() << '\n';
        return 1;
    }
}
