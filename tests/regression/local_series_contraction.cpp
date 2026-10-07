#include "souffle/problog/LocalSeriesContraction.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

using souffle::problog::contractLocalSeries;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    WorkingDerivationGraph graph;
    souffle::RamDomain nextRule = 100;
    NodePtr node(const std::string& name) { return graph.createNode(UntypedTuple{name, {}}); }
    NodePtr fact(const std::string& name, double probability = 0.4, bool original = true) {
        auto result = node(name);
        result->isFact = true;
        result->setOriginalFact(original);
        result->setProbability(probability);
        return result;
    }
    EdgePtr edge(const std::vector<NodePtr>& inputs, const NodePtr& head, double probability = 1.0,
            const std::vector<bool>& signs = {}) {
        RuleApplication application;
        application.ruleId = nextRule++;
        auto result = inputs.empty() ? graph.createHyperedge(inputs, head, application)
                : graph.createHyperedge(inputs, head, nullptr, signs, application);
        result->setProbability(probability);
        return result;
    }
    WorkingSubgraphView view() { return WorkingSubgraphView(graph.getNodes(), graph.getEdges()); }
};

using Truth = std::unordered_map<NodePtr, bool>;

Truth evaluate(const DerivationGraphViewInterface& view,
        const std::function<bool(const NodePtr&)>& factEvent,
        const std::function<bool(const EdgePtr&)>& ruleEvent) {
    Truth truth;
    std::unordered_set<NodePtr> visiting;
    std::unordered_map<NodePtr, std::vector<EdgePtr>> incoming;
    for (const auto& edge : view.getEdges()) incoming[edge->getOutput()].push_back(edge);
    std::function<bool(const NodePtr&)> visit = [&](const NodePtr& node) {
        if (const auto found = truth.find(node); found != truth.end()) return found->second;
        require(view.getNodes().count(node), "retained edge has a missing endpoint");
        require(visiting.insert(node).second, "world fixture must be a signed DAG");
        bool value = node->isFact && factEvent(node);
        for (const auto& edge : incoming[node]) {
            bool contribution = ruleEvent(edge);
            require(edge->getInputs().size() == edge->getBodyNegations().size(), "body/sign mismatch");
            for (std::size_t i = 0; i < edge->getInputs().size(); ++i) {
                const bool input = visit(edge->getInputs()[i]);
                contribution = contribution && (edge->getBodyNegations()[i] ? !input : input);
            }
            value = value || contribution;
        }
        visiting.erase(node);
        truth.emplace(node, value);
        return value;
    };
    for (const auto& node : view.getNodes()) visit(node);
    return truth;
}

struct Worlds {
    std::unordered_map<NodePtr, std::size_t> facts;
    std::unordered_map<EdgePtr, std::size_t> rules;
    std::unordered_map<SupportToken, std::size_t> factors;
    std::vector<double> probabilities;
    std::vector<Truth> original;

    explicit Worlds(const DerivationGraphViewInterface& view) {
        for (const auto& node : view.getNodes()) {
            if (node->isFact && node->getProbability() > 0 && node->getProbability() < 1) {
                const auto bit = probabilities.size();
                facts.emplace(node, bit);
                probabilities.push_back(node->getProbability());
            }
        }
        for (const auto& edge : view.getEdges()) {
            if (edge->getProbability() > 0 && edge->getProbability() < 1) {
                const auto bit = probabilities.size();
                rules.emplace(edge, bit);
                probabilities.push_back(edge->getProbability());
                for (const auto token : edge->getProbabilisticSupportTokens()) {
                    require(factors.emplace(token, bit).second, "accepted fixture has overlapping rule factors");
                }
            }
        }
        require(probabilities.size() <= 12, "too many original random worlds");
        for (std::uint64_t world = 0; world < (std::uint64_t{1} << probabilities.size()); ++world) {
            original.push_back(at(view, world));
        }
    }
    Truth at(const DerivationGraphViewInterface& view, std::uint64_t world) const {
        return evaluate(view, [&](const NodePtr& node) {
            const auto found = facts.find(node);
            return found == facts.end() ? node->getProbability() == 1 : ((world >> found->second) & 1U) != 0;
        }, [&](const EdgePtr& edge) {
            if (edge->getProbability() == 0 || edge->getProbability() == 1) return edge->getProbability() == 1;
            if (const auto found = rules.find(edge); found != rules.end()) return ((world >> found->second) & 1U) != 0;
            require(!edge->getProbabilisticSupportTokens().empty(), "compound event lost its original factors");
            bool value = true;
            for (const auto token : edge->getProbabilisticSupportTokens()) {
                const auto found = factors.find(token);
                require(found != factors.end(), "compound event introduced an unrelated random factor");
                value = value && ((world >> found->second) & 1U) != 0;
            }
            return value;
        });
    }
    double weight(std::uint64_t world) const {
        double result = 1;
        for (std::size_t bit = 0; bit < probabilities.size(); ++bit) {
            result *= ((world >> bit) & 1U) ? probabilities[bit] : 1 - probabilities[bit];
        }
        return result;
    }
    void verify(const DerivationGraphViewInterface& view) const {
        std::vector<NodePtr> retained(view.getNodes().begin(), view.getNodes().end());
        require(retained.size() < 64, "distribution signature is too large");
        auto signature = [&](const Truth& values) {
            std::uint64_t result = 0;
            for (std::size_t bit = 0; bit < retained.size(); ++bit) if (values.at(retained[bit])) result |= std::uint64_t{1} << bit;
            return result;
        };
        std::unordered_map<std::uint64_t, double> expected, actual;
        for (std::uint64_t world = 0; world < original.size(); ++world) {
            const auto values = at(view, world);
            for (const auto& node : retained) require(values.at(node) == original[world].at(node),
                    "series contraction changed a retained event in an original random world");
            expected[signature(values)] += weight(world);
        }
        // Also enumerate the rewritten graph using its ordinary independent
        // compound edge probabilities, checking the complete joint distribution.
        std::unordered_map<NodePtr, std::size_t> currentFacts;
        std::unordered_map<EdgePtr, std::size_t> currentRules;
        std::vector<double> currentProbabilities;
        for (const auto& node : retained) if (node->isFact && node->getProbability() > 0 && node->getProbability() < 1) {
            currentFacts[node] = currentProbabilities.size();
            currentProbabilities.push_back(node->getProbability());
        }
        for (const auto& edge : view.getEdges()) if (edge->getProbability() > 0 && edge->getProbability() < 1) {
            currentRules[edge] = currentProbabilities.size();
            currentProbabilities.push_back(edge->getProbability());
        }
        require(currentProbabilities.size() <= 12, "too many rewritten random worlds");
        for (std::uint64_t world = 0; world < (std::uint64_t{1} << currentProbabilities.size()); ++world) {
            const auto values = evaluate(view, [&](const NodePtr& node) {
                const auto found = currentFacts.find(node);
                return found == currentFacts.end() ? node->getProbability() == 1 : ((world >> found->second) & 1U) != 0;
            }, [&](const EdgePtr& edge) {
                const auto found = currentRules.find(edge);
                return found == currentRules.end() ? edge->getProbability() == 1 : ((world >> found->second) & 1U) != 0;
            });
            double probability = 1;
            for (std::size_t bit = 0; bit < currentProbabilities.size(); ++bit) {
                probability *= ((world >> bit) & 1U) ? currentProbabilities[bit] : 1 - currentProbabilities[bit];
            }
            actual[signature(values)] += probability;
        }
        for (const auto& [event, probability] : expected) require(std::abs(actual[event] - probability) < 1e-12,
                "ordinary compound-edge WMC changed a joint/evidence distribution");
        for (const auto& [event, probability] : actual) require(std::abs(expected[event] - probability) < 1e-12,
                "ordinary compound-edge WMC introduced a new joint event");
    }
};

std::string structure(const DerivationGraphViewInterface& view) {
    std::ostringstream out;
    out.precision(17);
    std::vector<NodePtr> nodes(view.getNodes().begin(), view.getNodes().end());
    std::vector<EdgePtr> edges(view.getEdges().begin(), view.getEdges().end());
    std::sort(nodes.begin(), nodes.end());
    std::sort(edges.begin(), edges.end());
    for (const auto& node : nodes) {
        out << node.get() << ' ' << node->pruned << ' ';
        for (const auto& edge : node->getIncomingEdges()) out << 'i' << edge.get();
        for (const auto& edge : node->getOutgoingEdges()) out << 'o' << edge.get();
    }
    for (const auto& edge : edges) {
        out << edge.get() << ' ' << edge->getProbability() << ' ' << edge->pruned;
        for (const auto& node : edge->getInputs()) out << 'b' << node.get();
        for (const auto negative : edge->getBodyNegations()) out << negative;
        for (const auto token : edge->getProbabilisticSupportTokens()) out << 's' << token;
    }
    return out.str();
}

void refuse(Fixture& fixture, bool complete = true) {
    auto view = fixture.view();
    const auto before = structure(view);
    const auto result = contractLocalSeries(fixture.graph, view, complete);
    require(result.stats.contractions == 0 && structure(view) == before, "rejected series candidate changed graph");
}

void correlatedInputsAndEvidence() {
    Fixture fixture;
    const auto seed = fixture.fact("Seed", 0.3);
    const auto a = fixture.node("A"), b = fixture.node("B"), z = fixture.node("Z");
    const auto t = fixture.node("T"), side = fixture.node("Side");
    fixture.edge({seed}, a);
    fixture.edge({seed}, b, 0.4);
    const auto first = fixture.edge({a, seed}, z, 0.7);
    const auto second = fixture.edge({z, seed, b}, t, 0.6);
    fixture.edge({a, b}, side, 0.5);
    t->setQuery();
    t->setEvidence(true);
    side->setQuery();
    auto view = fixture.view();
    const Worlds worlds(view);
    const auto result = contractLocalSeries(fixture.graph, view, true);
    require(result.stats.contractions == 1 && result.stats.removedNodes == 1 && result.stats.removedEdges == 2 &&
                    result.stats.addedEdges == 1 && result.stats.ruleVariablesRemoved == 1 &&
                    result.stats.duplicateInputsRemoved == 1, "wrong generalized series counters");
    require(result.compoundEdges[0]->getInputs() == std::vector<NodePtr>({a, seed, b}), "external inputs were lost or reordered");
    require(std::abs(result.compoundEdges[0]->getProbability() - 0.42) < 1e-15, "private rule probabilities were not multiplied");
    require(!fixture.graph.findNode(z->getTuple()) && !fixture.graph.getEdges().count(first) &&
                    !fixture.graph.getEdges().count(second), "owner retained an eliminated event");
    worlds.verify(view);
    const auto pruned = fixture.graph.prune(std::vector<std::string>{"T", "Side"});
    require(!pruned.getNodes().count(z) && !pruned.getEdges().count(first) && !pruned.getEdges().count(second),
            "owner prune revived retired series definitions");
    worlds.verify(pruned);
}

void signedBodiesAndLongDeduplication() {
    Fixture fixture;
    const auto a = fixture.fact("A"), b = fixture.fact("B"), c = fixture.fact("C");
    const auto z = fixture.node("Z"), t = fixture.node("T");
    fixture.edge({a, b}, z, 0.7, {false, true});
    fixture.edge({z, a, c}, t, 0.8, {false, false, true});
    t->setQuery();
    auto view = fixture.view();
    const Worlds worlds(view);
    const auto result = contractLocalSeries(fixture.graph, view, true);
    require(result.stats.contractions == 1 && result.stats.duplicateInputsRemoved == 1, "signed series was not simplified");
    require(result.compoundEdges[0]->getInputs() == std::vector<NodePtr>({a, b, c}) &&
                    result.compoundEdges[0]->getBodyNegations() == std::vector<bool>({false, true, true}),
            "external signs changed during series substitution");
    worlds.verify(view);

    Fixture longFixture;
    std::vector<NodePtr> inputs;
    for (std::size_t i = 0; i < 9; ++i) inputs.push_back(longFixture.fact("Input" + std::to_string(i)));
    const auto middle = longFixture.node("Middle"), output = longFixture.node("Output");
    longFixture.edge(inputs, middle, 0.3);
    longFixture.edge({middle, inputs[0], inputs[1]}, output, 0.7, {false, false, true});
    output->setQuery();
    auto longView = longFixture.view();
    const Worlds longWorlds(longView);
    const auto longResult = contractLocalSeries(longFixture.graph, longView, true);
    require(longResult.stats.contractions == 1 && longResult.stats.duplicateInputsRemoved == 1 &&
                    longResult.compoundEdges[0]->getInputs().size() == 10 &&
                    longResult.compoundEdges[0]->getInputs().back() == inputs[1] &&
                    longResult.compoundEdges[0]->getBodyNegations().back(), "long bodies lost opposite-sign literals");
    longWorlds.verify(longView);
}

void chainsAndAlternativeTargetSources() {
    Fixture fixture;
    const auto a = fixture.fact("A"), b = fixture.fact("B"), c = fixture.fact("C");
    const auto z1 = fixture.node("Z1"), z2 = fixture.node("Z2"), t = fixture.fact("T", 0.2);
    fixture.edge({a}, z1, 0.6);
    fixture.edge({z1, b}, z2, 0.7);
    fixture.edge({z2, c}, t, 0.8);
    const auto alternative = fixture.edge({b}, t, 0.5);
    t->setQuery();
    auto view = fixture.view();
    const Worlds worlds(view);
    const auto result = contractLocalSeries(fixture.graph, view, true);
    require(result.stats.contractions == 2 && result.stats.removedEdges == 3 && result.stats.addedEdges == 1 &&
                    result.stats.ruleVariablesRemoved == 2, "series worklist did not contract a chain");
    require(result.compoundEdges[0]->getProbabilisticSupportTokens().size() == 3 &&
                    std::abs(result.compoundEdges[0]->getProbability() - 0.336) < 1e-15 &&
                    view.getEdges().count(alternative) && t->isFact && t->getProbability() == 0.2,
            "chain contraction changed alternative target/fact sources");
    worlds.verify(view);
    require(contractLocalSeries(fixture.graph, view, true).stats.contractions == 0, "series pass was not at fixpoint");
}

void constantsAndEmptyBodies() {
    for (const double firstProbability : {0.0, 1.0}) {
        for (const double secondProbability : {0.0, 0.7, 1.0}) {
            Fixture fixture;
            const auto a = fixture.fact("A"), b = fixture.fact("B");
            const auto z = fixture.node("Z"), t = fixture.node("T");
            fixture.edge({a}, z, firstProbability);
            fixture.edge({z, b}, t, secondProbability);
            t->setQuery();
            auto view = fixture.view();
            const Worlds worlds(view);
            require(contractLocalSeries(fixture.graph, view, true).stats.contractions == 1,
                    "constant series factors were incorrectly rejected");
            worlds.verify(view);
        }
    }
    Fixture fixture;
    const auto z = fixture.node("Z"), t = fixture.node("T");
    fixture.edge({}, z, 0.3);
    fixture.edge({z}, t, 0.4);
    t->setQuery();
    auto view = fixture.view();
    const Worlds worlds(view);
    const auto result = contractLocalSeries(fixture.graph, view, true);
    require(result.stats.contractions == 1 && result.compoundEdges[0]->getInputs().empty(), "empty conjunction was not preserved");
    worlds.verify(view);
}

void longPrivateChain() {
    Fixture fixture;
    const auto input = fixture.fact("Input", 0.4);
    auto parent = input;
    constexpr std::size_t length = 2048;
    for (std::size_t i = 0; i <= length; ++i) {
        const auto child = fixture.node("Chain" + std::to_string(i));
        fixture.edge({parent}, child, 0.9999);
        parent = child;
    }
    parent->setQuery();
    auto view = fixture.view();
    const auto result = contractLocalSeries(fixture.graph, view, true);
    require(result.stats.contractions == length && result.compoundEdges.size() == 1 &&
                    result.compoundEdges[0]->getInputs() == std::vector<NodePtr>{input} &&
                    result.compoundEdges[0]->getProbabilisticSupportTokens().size() == length + 1 &&
                    result.stats.rejectedWorkBudget == 0,
            "long private chain lost factors or required rebuilding intermediate edges");
    require(std::abs(result.compoundEdges[0]->getProbability() - std::pow(0.9999, length + 1)) < 1e-12,
            "long private chain has an incorrect compound probability");

    // A chain growing by one external body input per step has a bounded local
    // analysis budget. Accepted prefix substitutions remain exact, and the
    // remaining expressions stay in the graph.
    Fixture growing;
    parent = growing.fact("Seed", 0.4);
    constexpr std::size_t growingLength = 192;
    for (std::size_t i = 0; i <= growingLength; ++i) {
        const auto external = growing.fact("External" + std::to_string(i), 0.5);
        const auto child = growing.node("Growing" + std::to_string(i));
        growing.edge({parent, external}, child, 0.9999);
        parent = child;
    }
    parent->setQuery();
    auto growingView = growing.view();
    const auto growingResult = contractLocalSeries(growing.graph, growingView, true);
    require(growingResult.stats.contractions > 0 && growingResult.stats.contractions < growingLength &&
                    growingResult.stats.rejectedWorkBudget > 0 &&
                    growingResult.stats.bodyOccurrencesAnalyzed <= growingResult.stats.inputAssociationsBefore * 4 + 4096,
            "growing series bodies exceeded their analysis work budget");
    for (const auto& edge : growingView.getEdges()) {
        require(edge->getInputs().size() == edge->getBodyNegations().size(), "budgeted series left malformed body signs");
        for (const auto& node : edge->getInputs()) require(growingView.getNodes().count(node), "budgeted series lost an endpoint");
    }
    require(growing.graph.getNodes() == growingView.getNodes() && growing.graph.getEdges() == growingView.getEdges(),
            "budgeted series left the owner out of sync");

    Fixture fanin;
    const auto shared = fanin.fact("Shared", 0.4), output = fanin.node("Output");
    std::vector<NodePtr> middles;
    for (std::size_t i = 0; i < 256; ++i) {
        const auto middle = fanin.node("Middle" + std::to_string(i));
        fanin.edge({shared}, middle, 0.9999);
        middles.push_back(middle);
    }
    fanin.edge(middles, output, 0.9999);
    output->setQuery();
    auto faninView = fanin.view();
    const auto faninResult = contractLocalSeries(fanin.graph, faninView, true);
    require(faninResult.stats.contractions > 0 && faninResult.stats.contractions < middles.size() &&
                    faninResult.stats.rejectedWorkBudget > 0 &&
                    faninResult.stats.bodyOccurrencesAnalyzed <= faninResult.stats.inputAssociationsBefore * 4 + 4096,
            "many middles sharing one large consumer bypassed the analysis budget");
}

void protectedAndMalformedCases() {
    for (int kind = 0; kind < 12; ++kind) {
        Fixture fixture;
        const auto a = fixture.fact("A"), b = fixture.fact("B");
        const auto z = fixture.node("Z"), t = fixture.node("T");
        const auto first = fixture.edge({a}, z, 0.6);
        const auto second = fixture.edge({z, b}, t, 0.7);
        t->setQuery();
        switch (kind) {
            case 0: z->isFact = true; z->setProbability(1); break;
            case 1: z->setOriginalFact(); break;
            case 2: z->isShadow = true; break;
            case 3: z->setQuery(); break;
            case 4: z->setEvidence(false); break;
            case 5: fixture.edge({b}, z); break;
            case 6: fixture.edge({z}, fixture.node("Other")); break;
            case 7: fixture.edge({z}, fixture.node("Other"), 1, {true}); break;
            case 8: second->replaceInput(b, z); break;
            case 9: const_cast<std::vector<bool>&>(second->getBodyNegations())[0] = true; break;
            case 10: const_cast<std::vector<bool>&>(first->getBodyNegations()).clear(); break;
            case 11: first->setProbability(1e-300); second->setProbability(1e-300); break;
        }
        refuse(fixture);
    }
    Fixture fixture;
    const auto a = fixture.fact("A"), b = fixture.fact("B"), z = fixture.node("Z"), t = fixture.node("T");
    fixture.edge({a}, z, 0.6);
    fixture.edge({z, b}, t, 0.7);
    t->setQuery();
    refuse(fixture, false);
}

void supportAndCycleRefusals() {
    for (int kind = 0; kind < 7; ++kind) {
        Fixture fixture;
        const auto a = fixture.fact("A"), b = fixture.fact("B");
        const auto z = fixture.node("Z"), t = fixture.node("T");
        const auto first = fixture.edge({a}, z, 0.6);
        const auto second = fixture.edge({z, b}, t, 0.7);
        t->setQuery();
        const auto token = first->getProbabilisticSupportTokens()[0];
        switch (kind) {
            case 0: second->setProbabilisticSupportTokens({token}); break;
            case 1: first->setProbabilisticSupportTokens({token, 9001});
                    second->setProbabilisticSupportTokens({9001, 9002}); break;
            case 2: fixture.edge({b}, fixture.node("Other"), 0.5)->setProbabilisticSupportTokens({token}); break;
            case 3: b->setProbabilisticSupportTokens({token}); b->setEvidence(true); break;
            case 4: first->clearProbabilisticSupportTokens(); break;
            case 5: fixture.fact("UnknownRandom", 0.5, false); break;
            case 6: fixture.edge({t}, a); break;
        }
        refuse(fixture);
    }
}

void commitHistoricalOwnerAndTransactionalRetirement() {
    Fixture fixture;
    const auto a = fixture.fact("A"), b = fixture.fact("B"), dead = fixture.fact("Absorbed", 0.2);
    const auto z = fixture.node("Z"), t = fixture.node("T");
    const auto historical = fixture.edge({a, dead}, z, 0.4);
    const auto replacement = fixture.edge({a}, z, 0.08);
    replacement->setProbabilisticSupportTokens(mergeSupportTokenLists(
            {&historical->getProbabilisticSupportTokens(), &dead->getProbabilisticSupportTokens()}));
    fixture.edge({z, b}, t, 0.5);
    t->setQuery();
    const UntypedTuple alias{"SavedAlias", {}};
    fixture.graph.bindEventAliasTuple(alias, a);
    auto view = fixture.view();
    view.mutableNodes().erase(dead);
    view.mutableEdges().erase(historical);
    view.invalidateCaches();
    require(contractLocalSeries(fixture.graph, view, true).stats.contractions == 0,
            "pass ignored a historical owner source before view commit");
    const auto retired = fixture.graph.retainRewriteView(view);
    require(retired.removedOwnerNodes == 1 && retired.removedOwnerEdges == 1 &&
                    fixture.graph.findNode(alias) == a && !fixture.graph.findNode(dead->getTuple()) &&
                    dead->getIncomingEdges().empty() && dead->getOutgoingEdges().empty(),
            "residual owner commit lost an active alias or kept historical definitions");
    const auto before = structure(view);
    bool rejected = false;
    try { fixture.graph.retireRewriteObjects(view, {z}, {}); }
    catch (const std::logic_error&) { rejected = true; }
    require(rejected && before == structure(view), "invalid retirement mutated the graph before rejecting");
    const Worlds worlds(view);
    require(contractLocalSeries(fixture.graph, view, true).stats.contractions == 1,
            "committed residual graph missed an absorbed private-factor series");
    worlds.verify(view);
    const auto pruned = fixture.graph.prune(std::vector<std::string>{"T"});
    require(!pruned.getNodes().count(z) && !pruned.getNodes().count(dead) &&
                    !pruned.getEdges().count(historical), "owner prune revived historical rewrite inputs");
    worlds.verify(pruned);
}
}  // namespace

int main() {
    try {
        DerivationGraphViewInterface::setVerboseEnabled(false);
        DerivationGraph::setPruneExtraEnabled(false);
        DerivationGraph::setMergeBiImpEnabled(false);
        correlatedInputsAndEvidence();
        signedBodiesAndLongDeduplication();
        chainsAndAlternativeTargetSources();
        constantsAndEmptyBodies();
        longPrivateChain();
        protectedAndMalformedCases();
        supportAndCycleRefusals();
        commitHistoricalOwnerAndTransactionalRetirement();
        std::cout << "local series contraction preserves original worlds and joint distributions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
