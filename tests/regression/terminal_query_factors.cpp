#include "souffle/problog/DeterministicEventAliases.h"
#include "souffle/problog/TerminalQueryFactors.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using souffle::problog::factorTerminalQueries;
using souffle::problog::evaluateTerminalQueryFactors;
using souffle::problog::restoreTerminalQueryFactorParents;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void close(double actual, double expected, const std::string& message) {
    require(std::abs(actual - expected) < 1e-12, message);
}

struct Fixture {
    WorkingDerivationGraph graph;
    souffle::RamDomain nextRule = 100;
    NodePtr node(const std::string& name) { return graph.createNode(UntypedTuple{name, {}}); }
    NodePtr fact(const std::string& name, double probability = 0.4) {
        auto result = node(name);
        result->isFact = true;
        result->setOriginalFact();
        result->setProbability(probability);
        return result;
    }
    EdgePtr edge(const std::vector<NodePtr>& inputs, const NodePtr& output,
            double probability = 1.0, std::vector<bool> signs = {}) {
        RuleApplication application;
        application.ruleId = nextRule++;
        application.varValuesPure = {nextRule};
        auto result = graph.createHyperedge(inputs, output, nullptr, signs, application);
        result->setProbability(probability);
        return result;
    }
    WorkingSubgraphView view() { return WorkingSubgraphView(graph.getNodes(), graph.getEdges()); }
};

using Truth = std::unordered_map<NodePtr, bool>;
struct Worlds {
    std::unordered_map<NodePtr, std::size_t> factBits;
    std::unordered_map<EdgePtr, std::size_t> edgeBits;
    std::vector<double> probabilities;
    std::vector<Truth> original;
    explicit Worlds(const DerivationGraphViewInterface& graph) {
        for (const auto& node : graph.getNodes()) {
            if (node->isFact && node->getProbability() > 0 && node->getProbability() < 1) {
                factBits.emplace(node, probabilities.size());
                probabilities.push_back(node->getProbability());
            }
        }
        for (const auto& edge : graph.getEdges()) {
            if (edge->getProbability() > 0 && edge->getProbability() < 1) {
                edgeBits.emplace(edge, probabilities.size());
                probabilities.push_back(edge->getProbability());
            }
        }
        require(probabilities.size() <= 10, "too many random bits in fixture");
        for (std::uint64_t world = 0; world < (std::uint64_t{1} << probabilities.size()); ++world) {
            original.push_back(evaluate(graph, world));
        }
    }
    Truth evaluate(const DerivationGraphViewInterface& graph, std::uint64_t world) const {
        std::unordered_map<NodePtr, std::vector<EdgePtr>> incoming;
        for (const auto& edge : graph.getEdges()) incoming[edge->getOutput()].push_back(edge);
        Truth values;
        std::unordered_set<NodePtr> visiting;
        std::function<bool(const NodePtr&)> visit = [&](const NodePtr& node) {
            const auto known = values.find(node);
            if (known != values.end()) return known->second;
            require(visiting.insert(node).second, "world fixture must be acyclic");
            bool value = false;
            if (node->isFact) {
                const auto bit = factBits.find(node);
                value = bit == factBits.end() ? node->getProbability() == 1
                                             : ((world >> bit->second) & 1U) != 0;
            }
            for (const auto& edge : incoming[node]) {
                const auto bit = edgeBits.find(edge);
                bool contribution = bit == edgeBits.end() ? edge->getProbability() == 1
                                                         : ((world >> bit->second) & 1U) != 0;
                const auto& body = edge->getInputs();
                const auto& signs = edge->getBodyNegations();
                require(body.size() == signs.size(), "body/sign mismatch");
                for (std::size_t i = 0; i < body.size(); ++i) {
                    const bool input = visit(body[i]);
                    contribution = contribution && (signs[i] ? !input : input);
                }
                value = value || contribution;
            }
            visiting.erase(node);
            values.emplace(node, value);
            return value;
        };
        for (const auto& node : graph.getNodes()) visit(node);
        return values;
    }
    double weight(std::uint64_t world) const {
        double weight = 1;
        for (std::size_t i = 0; i < probabilities.size(); ++i) {
            weight *= ((world >> i) & 1U) ? probabilities[i] : 1 - probabilities[i];
        }
        return weight;
    }
    double conditional(const NodePtr& query, const std::vector<NodePtr>& evidence = {}) const {
        double numerator = 0, denominator = 0;
        for (std::uint64_t world = 0; world < original.size(); ++world) {
            bool admitted = true;
            for (const auto& node : evidence) {
                admitted = admitted && original[world].at(node) == node->getEvidenceValue();
            }
            if (!admitted) continue;
            const auto probability = weight(world);
            denominator += probability;
            if (original[world].at(query)) numerator += probability;
        }
        require(denominator > 0, "fixture has impossible evidence");
        return numerator / denominator;
    }
    void retainedWorlds(const DerivationGraphViewInterface& graph) const {
        for (std::uint64_t world = 0; world < original.size(); ++world) {
            const auto after = evaluate(graph, world);
            for (const auto& [node, value] : after) {
                require(original[world].at(node) == value, "terminal factoring changed a retained event");
            }
        }
    }
};

void sharedOrParentAndConditionalEvidence(bool evidenceValue) {
    Fixture f;
    auto a = f.fact("A", 0.3), b = f.fact("B", 0.4);
    auto parent = f.node("Parent"), query = f.node("Q"), other = f.node("Other");
    auto evidence = f.node("Evidence");
    f.edge({a}, parent);
    f.edge({b}, parent);
    f.edge({parent}, query, 0.7);
    f.edge({parent, b}, other);
    f.edge({a}, evidence);
    query->setQuery();
    other->setQuery();
    evidence->setEvidence(evidenceValue);
    auto view = f.view();
    Worlds worlds(view);
    const auto result = factorTerminalQueries(f.graph, view, true);
    require(result.records.size() == 1 && result.records[0].query == query &&
                    result.records[0].parent == parent, "shared OR query was not factored");
    require(view.getNodes().count(parent) && view.getNodes().count(other) &&
                    !view.getNodes().count(query), "incorrect terminal graph membership");
    require(parent->needOutput && !parent->isQuery, "hidden parent changed original query identity");
    require(result.promotedOutputRoots == std::vector<NodePtr>{parent}, "hidden parent missing");
    worlds.retainedWorlds(view);
    std::unordered_map<NodePtr, double> probabilities{{parent, worlds.conditional(parent, {evidence})}};
    precomputedProbResult[parent] = 0.01;  // An old marginal cannot override current conditional inference.
    evaluateTerminalQueryFactors(result, probabilities);
    precomputedProbResult.clear();
    close(probabilities.at(query), worlds.conditional(query, {evidence}),
            "terminal output changed conditional marginal");
    require(!f.graph.findNode(query->getTuple()), "retired query tuple became a parent event alias");
    auto repruned = f.graph.prune(std::vector<std::string>{"Q", "Other"});
    require(!repruned.getNodes().count(query) && repruned.getNodes().count(parent),
            "owner pruning revived retired query or lost hidden parent");
}

void sharedParentOutputsAndDirectEvidence() {
    for (const bool observed : {false, true}) {
        Fixture f;
        auto parent = f.fact("Parent", 0.3), first = f.node("First"), second = f.node("Second");
        f.edge({parent}, first, 0.7);
        f.edge({parent}, second, 0.5);
        parent->setQuery();
        first->setQuery();
        second->setQuery();
        parent->setEvidence(observed);
        auto view = f.view();
        Worlds worlds(view);
        const auto result = factorTerminalQueries(f.graph, view, true);
        require(result.records.size() == 2 && result.promotedOutputRoots.empty() && parent->isQuery &&
                        parent->needOutput, "shared original parent output was hidden or changed");
        require(result.stats.factoredOutputQueries == 2 && result.stats.hiddenChainSteps == 0,
                "original query count includes hidden chain steps");
        std::unordered_map<NodePtr, double> probabilities{{parent, observed ? 1.0 : 0.0}};
        evaluateTerminalQueryFactors(result, probabilities);
        close(probabilities.at(first), worlds.conditional(first, {parent}),
                "direct parent evidence changed first marginal");
        close(probabilities.at(second), worlds.conditional(second, {parent}),
                "direct parent evidence changed second marginal");
        worlds.retainedWorlds(view);
    }
}

void chainsConstantsAndParentRecovery() {
    for (const double firstFactor : {0.0, 0.5, 1.0}) {
        Fixture f;
        auto parent = f.fact("Parent", 0.4), middle = f.node("Middle"), query = f.node("Q");
        f.edge({parent}, middle, 0.6);
        f.edge({middle}, query, firstFactor);
        query->setQuery();
        auto view = f.view();
        Worlds worlds(view);
        auto result = factorTerminalQueries(f.graph, view, true);
        require(result.records.size() == 2 && result.stats.removedNodes == 2 &&
                        result.stats.removedEdges == 2, "terminal chain was not removed");
        require(result.stats.factoredOutputQueries == 1 && result.stats.hiddenChainSteps == 1,
                "chain statistics confused original and hidden outputs");
        require(result.records[0].parent == parent && result.records[1].parent == parent,
                "terminal chain did not resolve its final parent");
        require(result.promotedOutputRoots.size() == 2 && parent->needOutput && !parent->isQuery,
                "chain promoted outputs were not hidden");
        worlds.retainedWorlds(view);
        std::unordered_map<NodePtr, double> probabilities{{parent, 0.4}};
        evaluateTerminalQueryFactors(result, probabilities);
        close(probabilities.at(query), worlds.conditional(query), "terminal chain marginal changed");
        close(probabilities.at(middle), 0.24, "intermediate output calculation changed");

        // Simulate implicit SISO replacing the owner and then tuple precompute.
        Fixture replacement;
        auto replacementParent = replacement.fact("Parent", 0.4);
        restoreTerminalQueryFactorParents(replacement.graph, result);
        require(result.records[0].parent == replacementParent, "new owner parent was not rebound");
        probabilities.clear();
        precomputedProbResult[replacementParent] = 0.4;
        evaluateTerminalQueryFactors(result, probabilities);
        close(probabilities.at(query), firstFactor * 0.24, "node precomputed parent lookup failed");
        precomputedProbResult.clear();
        probabilities.clear();
        precomputedTupleProbResult[replacementParent->getTuple().toString()] = 0.4;
        evaluateTerminalQueryFactors(result, probabilities);
        close(probabilities.at(query), firstFactor * 0.24, "tuple precomputed parent lookup failed");
        precomputedTupleProbResult.clear();
    }
}

void aliasesAndVisibleNames() {
    Fixture f;
    auto parent = f.fact("Parent", 0.3), query = f.node("Q"), alias = f.node("Alias");
    f.edge({parent}, query, 0.7);
    f.edge({query}, alias);
    alias->setQuery();
    auto view = f.view();
    const auto aliases = souffle::problog::eliminateDeterministicEventAliases(f.graph, view, true);
    require(aliases.outputAliases.size() == 1 && aliases.outputAliases[0].second == query,
            "fixture alias did not target terminal query");
    const auto terminal = factorTerminalQueries(f.graph, view, true);
    require(terminal.records.size() == 1 && terminal.records[0].query == query,
            "alias representative was not deferred");
    std::unordered_map<NodePtr, double> probabilities{{parent, 0.3}};
    evaluateTerminalQueryFactors(terminal, probabilities);
    close(probabilities.at(aliases.outputAliases[0].second), 0.21,
            "alias output could not resolve deferred representative marginal");
    auto hidden = aliases.promotedOutputRoots;
    hidden.insert(hidden.end(), terminal.promotedOutputRoots.begin(), terminal.promotedOutputRoots.end());
    const auto directory = std::filesystem::temp_directory_path() /
            ("psouffle-terminal-query-test-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    dumpProbabilities(probabilities, directory.string(), "facts", aliases.outputAliases, hidden);
    std::ifstream output(directory / "facts.prob");
    const std::string contents{std::istreambuf_iterator<char>(output), std::istreambuf_iterator<char>()};
    std::filesystem::remove_all(directory);
    require(contents.find("Alias") != std::string::npos && contents.find("Parent") == std::string::npos &&
                    contents.find("Q(") == std::string::npos, "hidden inference roots leaked output names");
}

void refusals() {
    // Each mutation adds a concrete reason that a private unary terminal output
    // certificate cannot be established. No candidate or flag may be rewritten.
    for (int reason = 0; reason < 13; ++reason) {
        Fixture f;
        auto parent = f.fact("Parent", 0.4), query = f.node("Q");
        auto source = f.edge({parent}, query, 0.7);
        query->setQuery();
        if (reason == 0) query->setEvidence(true);
        if (reason == 1) query->setEvidence(false);
        if (reason == 2) { query->isFact = true; query->setOriginalFact(); }
        if (reason == 3) f.edge({parent}, query, 0.8);
        if (reason == 4) f.edge({query}, f.node("Joint"));
        if (reason == 5) f.edge({query}, f.node("NegativeJoint"), 1.0, {true});
        if (reason == 6) source->setProbabilisticSupportTokens(parent->getProbabilisticSupportTokens());
        if (reason == 7) source->clearProbabilisticSupportTokens();
        if (reason == 8) parent->clearProbabilisticSupportTokens();
        if (reason == 9) query->isShadow = true;
        if (reason == 10) parent->isShadow = true;
        if (reason == 11) {
            auto evidence = f.node("Evidence");
            auto shared = f.edge({parent}, evidence, 0.5);
            shared->setProbabilisticSupportTokens(source->getProbabilisticSupportTokens());
            evidence->setEvidence(true);
        }
        if (reason == 12) {
            auto shared = f.node("SharedSupport");
            shared->setProbabilisticSupportTokens(source->getProbabilisticSupportTokens());
        }
        auto view = f.view();
        const auto nodes = view.getNodes();
        const auto edges = view.getEdges();
        const auto result = factorTerminalQueries(f.graph, view, true);
        require(result.records.empty() && view.getNodes() == nodes && view.getEdges() == edges &&
                        query->needOutput && !parent->needOutput,
                "unsupported terminal shape was changed: " + std::to_string(reason));
    }
    {
        Fixture f;
        auto parent = f.fact("Parent"), query = f.node("Q");
        f.edge({parent}, query, 0.7, {true});
        query->setQuery();
        auto view = f.view();
        require(factorTerminalQueries(f.graph, view, true).records.empty(), "negative terminal input accepted");
        require(factorTerminalQueries(f.graph, view).records.empty(), "incomplete derivations accepted");
    }
    {
        Fixture f;
        auto parent = f.fact("Parent"), query = f.node("Q");
        auto source = f.edge({parent}, query, 0.7);
        query->setQuery();
        WorkingSubgraphView partial({query}, {source});
        require(factorTerminalQueries(f.graph, partial, true).records.empty(), "missing parent endpoint accepted");
        auto view = f.view();
        const_cast<std::vector<bool>&>(source->getBodyNegations()).pop_back();
        require(factorTerminalQueries(f.graph, view, true).records.empty(), "malformed signed body accepted");
    }
    for (const bool malformedFact : {false, true}) {
        Fixture f;
        auto parent = f.fact("Parent"), query = f.node("Q"), unrelated = f.fact("Unrelated");
        f.edge({parent}, query, 0.7);
        auto unrelatedSource = f.edge({parent}, f.node("Other"), 0.5);
        query->setQuery();
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        if (malformedFact) unrelated->setProbability(nan);
        else unrelatedSource->setProbability(nan);
        auto view = f.view();
        require(factorTerminalQueries(f.graph, view, true).records.empty() && !parent->needOutput,
                "nonfinite retained probabilistic source did not fail closed");
    }
    {
        Fixture f;
        auto seed = f.fact("Seed"), first = f.node("First"), second = f.node("Second"), query = f.node("Q");
        f.edge({seed}, first);
        f.edge({first}, second);
        f.edge({second}, first);
        f.edge({first}, query, 0.7);
        query->setQuery();
        auto view = f.view();
        const auto result = factorTerminalQueries(f.graph, view, true);
        require(result.records.empty() && result.stats.skippedRecursive == 1,
                "recursive parent was accepted");
    }
    {
        Fixture f;
        auto parent = f.fact("Parent"), query = f.node("Q"), unused = f.node("Unused");
        const auto source = f.edge({parent}, query, 0.7);
        f.edge({query}, unused);
        query->setQuery();
        WorkingSubgraphView view({parent, query}, {source});
        const auto result = factorTerminalQueries(f.graph, view, true);
        require(result.records.empty() && result.stats.skippedOwnerHistory == 1,
                "incomplete owner incident history was silently retired");
        f.graph.retainRewriteView(view);
        require(factorTerminalQueries(f.graph, view, true).records.size() == 1,
                "committed active owner did not expose terminal output");
    }
}

}  // namespace

int main() {
    try {
        DerivationGraph::setMergeBiImpEnabled(false);
        DerivationGraph::setPruneExtraEnabled(false);
        precomputedProbResult.clear();
        precomputedTupleProbResult.clear();
        sharedOrParentAndConditionalEvidence(true);
        sharedOrParentAndConditionalEvidence(false);
        sharedParentOutputsAndDirectEvidence();
        chainsConstantsAndParentRecovery();
        aliasesAndVisibleNames();
        refusals();
        std::cout << "terminal query factor regression passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terminal query factor regression failed: " << error.what() << '\n';
        return 1;
    }
}
