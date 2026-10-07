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

    NodePtr node(const UntypedTuple& tuple) {
        auto result = graph.createNode(tuple);
        nodes.push_back(result);
        return result;
    }

    NodePtr node(const std::string& name) { return node(UntypedTuple{name, {}}); }

    NodePtr fact(const UntypedTuple& tuple, double probability) {
        auto result = node(tuple);
        result->isFact = true;
        result->setOriginalFact();
        result->setProbability(probability);
        return result;
    }

    NodePtr fact(const std::string& name, double probability) { return fact(UntypedTuple{name, {}}, probability); }

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

struct Compilation {
    WeightedBDDManager manager{smallConfig()};
    NodeFormulas nodes;
    EdgeFormulas edges;
    std::size_t batchedCycles = 0;
    std::size_t batchedEdges = 0;
    std::string registrationOrder;
};

void compile(Compilation& compilation, SubgraphView& view, bool standalone,
        const std::unordered_set<NodePtr>& seeds) {
    debugger.startTurn("FULL");
    auto* stage = debugger.startStage(StageKind::FORWARD_COMPILATION);
    if (standalone) {
        buildFormulasCyclewiseStandaloneFull(view, compilation.manager, compilation.nodes, compilation.edges, seeds);
    } else {
        buildFormulasCyclewise(view, compilation.manager, compilation.nodes, compilation.edges, seeds);
    }
    require(stage != nullptr, "missing compilation diagnostics");
    compilation.batchedCycles = std::stoull(stage->getInfo("fc_singleton_batched_cycles"));
    compilation.batchedEdges = std::stoull(stage->getInfo("fc_singleton_batched_edges"));
    compilation.registrationOrder = stage->getInfo("fc_event_registration_order");
    debugger.endStage();
    debugger.endTurn();
}

// Include zero-weight event assignments too. Equality is checked by event
// identity and actual variable indices, never by BDD pointer equality across
// managers or only by marginal probabilities.
struct EventWorlds {
    std::unordered_map<std::size_t, std::size_t> facts;
    std::unordered_map<EdgePtr, std::size_t> rules;
    std::size_t count = 0;

    explicit EventWorlds(const Fixture& fixture) {
        for (const auto& node : fixture.nodes) {
            if (node->isFact && node->getProbability() < 1.0 && !facts.count(node->getSemanticFactId())) {
                facts.emplace(node->getSemanticFactId(), count++);
            }
        }
        for (const auto& edge : fixture.edges) {
            if (!edge->isDeterministic()) rules.emplace(edge, count++);
        }
        require(count < 12, "batch event-world fixture too large");
    }

    std::vector<int> assignment(WeightedBDDManager& manager, const Fixture& fixture, std::uint64_t world) const {
        std::vector<int> result(static_cast<std::size_t>(Cudd_ReadSize(manager.getManager())), 0);
        for (const auto& node : fixture.nodes) {
            if (!node->isFact) continue;
            int index = -1;
            require(manager.peekVarIndex(*node, index), "compiled fact has no variable identity");
            // The base initializer retains a p1 index slot but uses True
            // directly, so that slot need not exist in CUDD's variable pool.
            if (node->getProbability() == 1.0) continue;
            result.at(static_cast<std::size_t>(index)) =
                    static_cast<int>((world >> facts.at(node->getSemanticFactId())) & 1);
        }
        for (const auto& [edge, bit] : rules) {
            int index = -1;
            require(manager.peekVarIndex(*edge, index), "compiled rule has no variable identity");
            result.at(static_cast<std::size_t>(index)) = static_cast<int>((world >> bit) & 1);
        }
        return result;
    }
};

bool truthAt(WeightedBDDManager& manager, const BddNodeRef& formula, std::vector<int>& assignment) {
    require(formula.get() != nullptr, "attempt to evaluate unset BDD");
    const auto value = Cudd_Eval(manager.getManager(), formula.get(), assignment.data());
    const auto one = Cudd_ReadOne(manager.getManager());
    require(value == one || value == Cudd_Not(one), "event assignment did not produce a Boolean value");
    return value == one;
}

void compareCompilations(const std::string& label, const Fixture& fixture,
        Compilation& reference, Compilation& candidate, const NodePtr& evidence) {
    require(reference.batchedCycles == 0 && reference.batchedEdges == 0,
            label + ": default entry unexpectedly batched sources");
    require(reference.nodes.size() == candidate.nodes.size() && reference.edges.size() == candidate.edges.size(),
            label + ": formula membership changed");
    const EventWorlds worlds(fixture);
    for (std::uint64_t world = 0; world < (std::uint64_t{1} << worlds.count); ++world) {
        auto original = worlds.assignment(reference.manager, fixture, world);
        auto optimized = worlds.assignment(candidate.manager, fixture, world);
        for (const auto& node : fixture.nodes) {
            require(reference.nodes.count(node) == candidate.nodes.count(node), label + ": node formula missing");
            if (!reference.nodes.count(node)) continue;
            require(truthAt(reference.manager, reference.nodes.at(node), original) ==
                            truthAt(candidate.manager, candidate.nodes.at(node), optimized),
                    label + ": changed node event in world " + std::to_string(world));
        }
        for (const auto& edge : fixture.edges) {
            require(reference.edges.count(edge) == candidate.edges.count(edge), label + ": edge formula missing");
            if (!reference.edges.count(edge)) continue;
            require(truthAt(reference.manager, reference.edges.at(edge), original) ==
                            truthAt(candidate.manager, candidate.edges.at(edge), optimized),
                    label + ": changed rule contribution in world " + std::to_string(world));
        }
    }
    for (const auto& edge : fixture.edges) {
        if (reference.edges.count(edge)) close(candidate.manager.computeWeightedModelCount(candidate.edges.at(edge)),
                reference.manager.computeWeightedModelCount(reference.edges.at(edge)), label + ": edge WMC");
    }
    for (const auto& node : fixture.nodes) {
        if (!reference.nodes.count(node)) continue;
        close(candidate.manager.computeWeightedModelCount(candidate.nodes.at(node)),
                reference.manager.computeWeightedModelCount(reference.nodes.at(node)), label + ": node WMC");
        for (const auto& other : fixture.nodes) {
            if (!reference.nodes.count(other)) continue;
            close(candidate.manager.computeWeightedModelCount(candidate.manager.makeAnd(candidate.nodes.at(node), candidate.nodes.at(other))),
                    reference.manager.computeWeightedModelCount(reference.manager.makeAnd(reference.nodes.at(node), reference.nodes.at(other))),
                    label + ": joint WMC");
        }
        for (bool positive : {false, true}) {
            const auto originalEvidence = positive ? reference.nodes.at(evidence) : reference.manager.makeNot(reference.nodes.at(evidence));
            const auto optimizedEvidence = positive ? candidate.nodes.at(evidence) : candidate.manager.makeNot(candidate.nodes.at(evidence));
            const double originalMass = reference.manager.computeWeightedModelCount(originalEvidence);
            const double optimizedMass = candidate.manager.computeWeightedModelCount(optimizedEvidence);
            close(optimizedMass, originalMass, label + ": evidence mass");
            require(originalMass > 0.0, label + ": degenerate evidence fixture");
            close(candidate.manager.computeWeightedModelCount(candidate.manager.makeAnd(candidate.nodes.at(node), optimizedEvidence)) / optimizedMass,
                    reference.manager.computeWeightedModelCount(reference.manager.makeAnd(reference.nodes.at(node), originalEvidence)) / originalMass,
                    label + ": evidence posterior");
        }
    }
}

void batchedWideDisjunctionPreservesEvents() {
    Fixture fixture;
    const auto a = fixture.fact("WideA", 0.6);
    const auto alias = fixture.fact("WideAliasA", 0.6);
    alias->setSemanticFactId(a->getSemanticFactId());
    const auto b = fixture.fact("WideB", 0.4);
    const auto head = fixture.node("WideHead");
    const auto downstream = fixture.node("WideDownstream");
    fixture.edge({a, b}, head, 0.0);
    fixture.edge({a, b}, head, 0.7);
    fixture.edge({alias, b}, head, 0.2);
    fixture.edge({a, b}, head, 0.45, {false, true});
    fixture.edge({a, alias}, head, 0.3);
    fixture.edge({a, alias}, head, 1.0, {false, true});
    fixture.edge({head, b}, downstream, 0.8);
    auto defaultView = fixture.view(false);
    auto fullView = fixture.view(true);
    Compilation reference, candidate;
    compile(reference, defaultView, false, {});
    compile(candidate, fullView, true, {});
    require(candidate.batchedCycles == 2 && candidate.batchedEdges == fixture.edges.size(),
            "wide OR did not batch all eligible singleton sources");
    compareCompilations("wide OR", fixture, reference, candidate, head);
    const Worlds oracle(fixture, {});
    checkProbabilities(candidate.manager, fixture, candidate.nodes, oracle, head);
}

void baseFactsAndSeedsKeepDefaultBehavior() {
    Fixture fixture;
    const auto a = fixture.fact("BaseA", 0.4);
    const auto head = fixture.fact("FactAndDerived", 0.65);
    const auto seeded = fixture.node("SeedAndDerived");
    fixture.edge({a}, head, 0.0);
    fixture.edge({a}, head, 0.5);
    fixture.edge({a}, seeded, 0.0);
    fixture.edge({a}, seeded, 0.7);
    const std::unordered_set<NodePtr> seeds{seeded};
    auto defaultView = fixture.view(false);
    auto fullView = fixture.view(true);
    Compilation reference, candidate;
    compile(reference, defaultView, false, seeds);
    compile(candidate, fullView, true, seeds);
    require(candidate.batchedCycles == 0 && candidate.batchedEdges == 0, "fact or seed head was batched");
    compareCompilations("fact/seed sources", fixture, reference, candidate, a);
    // Preserve the current incoming-edge-only recomputation. This fixture
    // deliberately compares the existing contract, not a new fact-OR meaning.
    close(candidate.manager.computeWeightedModelCount(candidate.nodes.at(head)), 0.2, "existing fact-head behavior");
    close(candidate.manager.computeWeightedModelCount(candidate.nodes.at(seeded)), 0.28, "existing seed-head behavior");
}

void recursiveSourcesKeepDefaultBehavior() {
    Fixture fixture;
    const auto a = fixture.fact("CycleA", 0.4);
    const auto self = fixture.node("SelfLoop");
    const auto left = fixture.node("CycleLeft");
    const auto right = fixture.node("CycleRight");
    fixture.edge({a}, self, 0.5);
    fixture.edge({self}, self, 0.6);
    fixture.edge({a}, left, 0.7);
    fixture.edge({left}, right, 0.3);
    fixture.edge({right}, left, 0.8);
    auto defaultView = fixture.view(false);
    auto fullView = fixture.view(true);
    Compilation reference, candidate;
    compile(reference, defaultView, false, {});
    compile(candidate, fullView, true, {});
    require(candidate.batchedCycles == 0 && candidate.batchedEdges == 0, "recursive SCC was batched");
    compareCompilations("recursive sources", fixture, reference, candidate, left);
    close(candidate.manager.computeWeightedModelCount(candidate.nodes.at(self)), 0.2, "self-loop least fixed point");
    close(candidate.manager.computeWeightedModelCount(candidate.nodes.at(right)), 0.084, "two-node least fixed point");
}

void prepopulatedFormulaMapsKeepDefaultBehavior() {
    for (int mode : {1, 2}) {
        Fixture fixture;
        const auto a = fixture.fact("ExistingA", 0.6);
        const auto head = fixture.node("ExistingHead");
        const auto first = fixture.edge({a}, head, 0.4);
        fixture.edge({a}, head, 0.7);
        auto defaultView = fixture.view(false);
        auto fullView = fixture.view(true);
        Compilation reference, candidate;
        if (mode == 1) {
            reference.nodes[head] = reference.manager.getTrue();
            candidate.nodes[head] = candidate.manager.getTrue();
        } else {
            reference.edges[first] = reference.manager.getFalse();
            candidate.edges[first] = candidate.manager.getFalse();
        }
        compile(reference, defaultView, false, {});
        compile(candidate, fullView, true, {});
        require(candidate.batchedCycles == 0 && candidate.batchedEdges == 0, "prepopulated formula map was batched");
        compareCompilations("prepopulated mode " + std::to_string(mode), fixture, reference, candidate, a);
        const Worlds oracle(fixture, {});
        checkProbabilities(candidate.manager, fixture, candidate.nodes, oracle, a);
    }
}

void tupleAndSupportOrderPreservesIndependentEvents() {
    Fixture fixture;
    const auto z = fixture.fact(UntypedTuple{"EventInput", {100}}, 0.6);
    const auto a = fixture.fact(UntypedTuple{"EventInput", {2}}, 0.4);
    const auto alias = fixture.fact(UntypedTuple{"EventInput", {20}}, 0.6);
    alias->setSemanticFactId(z->getSemanticFactId());
    const auto zero = fixture.fact(UntypedTuple{"EventInput", {10}}, 0.0);
    const auto one = fixture.fact(UntypedTuple{"EventInput", {50}}, 1.0);
    const auto zHead = fixture.node(UntypedTuple{"EventOutput", {100}});
    const auto aHead = fixture.node(UntypedTuple{"EventOutput", {2}});
    const auto zRule = fixture.edge({z, a}, zHead, 0.7);
    zRule->setProbabilisticSupportTokens({makeEdgeSupportToken(5)});
    const auto first = fixture.edge({a, z}, aHead, 0.2);
    first->setProbabilisticSupportTokens({makeEdgeSupportToken(40), makeEdgeSupportToken(20)});
    const auto equal = fixture.edge({a, z}, aHead, 0.3);
    equal->setProbabilisticSupportTokens(first->getProbabilisticSupportTokens());
    const auto overlap = fixture.edge({a, alias}, aHead, 0.4);
    overlap->setProbabilisticSupportTokens({makeEdgeSupportToken(60), makeEdgeSupportToken(20)});
    const auto firstZero = fixture.edge({a, z}, aHead, 0.0);
    const auto secondZero = fixture.edge({a, z}, aHead, 0.0);
    fixture.edge({z, alias}, aHead, 1.0, {false, true});
    const auto earlierSupport = fixture.edge({a, alias}, aHead, 0.5, {false, true});
    earlierSupport->setProbabilisticSupportTokens({makeEdgeSupportToken(10)});
    require(z->getId() < a->getId() && zHead->getId() < aHead->getId(),
            "tuple-order fixture did not reverse numeric IDs");
    require(first->getProbabilisticSupportTokens() == equal->getProbabilisticSupportTokens() &&
                    supportTokensIntersect(first->getProbabilisticSupportTokens(), overlap->getProbabilisticSupportTokens()),
            "tuple-order fixture lacks equal/overlapping provenance keys");
    require(firstZero->getProbabilisticSupportTokens().empty() && secondZero->getProbabilisticSupportTokens().empty(),
            "zero-probability fixture unexpectedly has support keys");
    auto defaultView = fixture.view(false);
    auto fullView = fixture.view(true);
    auto reverseView = fixture.view(false);
    Compilation reference, candidate, reversed;
    compile(reference, defaultView, false, {});
    compile(candidate, fullView, true, {});
    compile(reversed, reverseView, true, {});
    require(reference.registrationOrder == "existing" && candidate.registrationOrder == "stable_tuples",
            "tuple registration escaped its standalone entry point");
    const auto registered = indices(candidate.manager, fixture, true);
    require(indices(reversed.manager, fixture, true) == registered, "tuple/support indices depend on view insertion");
    require(registered.facts.at(a->getSemanticFactId()) == 0 && registered.facts.at(zero->getSemanticFactId()) == 1 &&
                    registered.facts.at(z->getSemanticFactId()) == 2,
            "fact tuple order was replaced by numeric or semantic ID order");
    const std::vector<EdgePtr> expectedRules{firstZero, secondZero, earlierSupport, first, equal, overlap, zRule};
    for (std::size_t i = 0; i < expectedRules.size(); ++i) {
        require(registered.rules.at(expectedRules[i]->getId()) == static_cast<int>(i + 3),
                "rule order ignored head tuple, normalized support, or ID tie");
    }
    require(registered.facts.at(one->getSemanticFactId()) == candidate.manager.nextVarIndex_ - 1,
            "tuple sorting moved the probability-one fact slot");
    require(candidate.batchedCycles == 2 && candidate.batchedEdges == fixture.edges.size(),
            "tuple/support fixture did not exercise the wide source batch");
    compareCompilations("tuple/support order", fixture, reference, candidate, aHead);
    compareCompilations("tuple/support reverse insertion", fixture, reference, reversed, aHead);
    const Worlds oracle(fixture, {});
    checkProbabilities(candidate.manager, fixture, candidate.nodes, oracle, aHead);
}

}  // namespace

int main() {
    try {
        standaloneOrderPreservesEventsAndProbabilities();
        preConfigPreservesDefaultAndDeltaPolicies();
        batchedWideDisjunctionPreservesEvents();
        baseFactsAndSeedsKeepDefaultBehavior();
        recursiveSourcesKeepDefaultBehavior();
        prepopulatedFormulaMapsKeepDefaultBehavior();
        tupleAndSupportOrderPreservesIndependentEvents();
        std::cout << "full event order tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "full event order regression failed: " << exception.what() << '\n';
        return 1;
    }
}
