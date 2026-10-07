#include "souffle/problog/PrivateFactorRewrite.h"
#include "souffle/problog/TerminalQueryFactors.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using souffle::problog::evaluateTerminalQueryFactors;
using souffle::problog::rewritePrivateFactors;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void close(double actual, double expected, const std::string& message) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-12, message);
}

struct TestGraph : WorkingDerivationGraph {
    std::size_t allocatedEdges() const { return nextEdgeId; }
};
struct Fixture {
    TestGraph graph;
    souffle::RamDomain nextRule = 100;
    NodePtr node(const std::string& name) { return graph.createNode(UntypedTuple{name, {}}); }
    NodePtr fact(const std::string& name, double probability = 0.4) {
        auto result = node(name);
        result->isFact = true;
        result->setOriginalFact();
        result->setProbability(probability);
        return result;
    }
    EdgePtr edge(const std::vector<NodePtr>& inputs, const NodePtr& head,
            double probability = 1.0, const std::vector<bool>& signs = {}) {
        RuleApplication application;
        application.ruleId = nextRule++;
        application.varValuesPure = {nextRule};
        auto result = inputs.empty() ? graph.createHyperedge(inputs, head, application)
                                    : graph.createHyperedge(inputs, head, nullptr, signs, application);
        result->setProbability(probability);
        return result;
    }
    WorkingSubgraphView view() { return WorkingSubgraphView(graph.getNodes(), graph.getEdges()); }
};

using Truth = std::unordered_map<NodePtr, bool>;
using Marginals = std::unordered_map<NodePtr, double>;

Truth evaluate(const DerivationGraphViewInterface& view,
        const std::function<bool(const NodePtr&)>& factEvent,
        const std::function<bool(const EdgePtr&)>& ruleEvent) {
    std::unordered_map<NodePtr, std::vector<EdgePtr>> incoming;
    for (const auto& edge : view.getEdges()) incoming[edge->getOutput()].push_back(edge);
    Truth truth;
    std::unordered_set<NodePtr> visiting;
    std::function<bool(const NodePtr&)> visit = [&](const NodePtr& node) {
        if (const auto found = truth.find(node); found != truth.end()) return found->second;
        require(view.getNodes().count(node), "missing retained endpoint");
        require(visiting.insert(node).second, "world oracle requires a signed DAG");
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

double weight(const std::vector<double>& probabilities, std::uint64_t world) {
    double result = 1.0;
    for (std::size_t i = 0; i < probabilities.size(); ++i) {
        result *= ((world >> i) & 1U) ? probabilities[i] : 1.0 - probabilities[i];
    }
    return result;
}
bool admitted(const Truth& truth, const std::vector<NodePtr>& evidence) {
    for (const auto& node : evidence) {
        if (truth.at(node) != node->getEvidenceValue()) return false;
    }
    return true;
}

// The original-world oracle maps a compound edge back to its primitive rule
// events. A separate enumeration below treats the rewritten factors as ordinary
// independent Bernoulli variables, checking the graph's WMC contract too.
struct Worlds {
    std::unordered_map<NodePtr, std::size_t> facts;
    std::unordered_map<EdgePtr, std::size_t> rules;
    std::unordered_map<SupportToken, std::size_t> factors;
    std::vector<double> probabilities;
    std::vector<Truth> original;

    explicit Worlds(const DerivationGraphViewInterface& view) {
        for (const auto& node : view.getNodes()) {
            if (node->isFact && node->getProbability() > 0 && node->getProbability() < 1) {
                facts.emplace(node, probabilities.size());
                probabilities.push_back(node->getProbability());
            }
        }
        for (const auto& edge : view.getEdges()) {
            if (edge->getProbability() > 0 && edge->getProbability() < 1) {
                const auto bit = probabilities.size();
                rules.emplace(edge, bit);
                probabilities.push_back(edge->getProbability());
                for (const auto token : edge->getProbabilisticSupportTokens()) {
                    require(factors.emplace(token, bit).second, "oracle fixture has shared primitive tokens");
                }
            }
        }
        require(probabilities.size() <= 12, "too many oracle worlds");
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
            require(!edge->getProbabilisticSupportTokens().empty(), "new compound factor lost provenance");
            bool value = true;
            for (const auto token : edge->getProbabilisticSupportTokens()) {
                const auto found = factors.find(token);
                require(found != factors.end(), "new compound factor introduced a primitive event");
                value = value && ((world >> found->second) & 1U) != 0;
            }
            return value;
        });
    }
    double conditional(const NodePtr& query, const std::vector<NodePtr>& evidence) const {
        double numerator = 0.0, denominator = 0.0;
        for (std::uint64_t world = 0; world < original.size(); ++world) {
            if (!admitted(original[world], evidence)) continue;
            const auto probability = weight(probabilities, world);
            denominator += probability;
            if (original[world].at(query)) numerator += probability;
        }
        require(denominator > 0, "impossible oracle evidence");
        return numerator / denominator;
    }
    Marginals verify(const DerivationGraphViewInterface& view, const std::vector<NodePtr>& evidence = {}) const {
        std::vector<NodePtr> retained(view.getNodes().begin(), view.getNodes().end());
        require(retained.size() < 64, "joint signature is too large");
        auto signature = [&](const Truth& truth) {
            std::uint64_t result = 0;
            for (std::size_t i = 0; i < retained.size(); ++i) {
                if (truth.at(retained[i])) result |= std::uint64_t{1} << i;
            }
            return result;
        };
        std::unordered_map<std::uint64_t, double> expectedJoint, actualJoint;
        for (std::uint64_t world = 0; world < original.size(); ++world) {
            const auto truth = at(view, world);
            for (const auto& node : retained) {
                require(truth.at(node) == original[world].at(node), "retained event changed in a primitive world");
            }
            expectedJoint[signature(truth)] += weight(probabilities, world);
        }

        std::unordered_map<NodePtr, std::size_t> currentFacts;
        std::unordered_map<EdgePtr, std::size_t> currentRules;
        std::vector<double> currentProbabilities;
        for (const auto& node : retained) {
            if (node->isFact && node->getProbability() > 0 && node->getProbability() < 1) {
                currentFacts[node] = currentProbabilities.size();
                currentProbabilities.push_back(node->getProbability());
            }
        }
        for (const auto& edge : view.getEdges()) {
            if (edge->getProbability() > 0 && edge->getProbability() < 1) {
                currentRules[edge] = currentProbabilities.size();
                currentProbabilities.push_back(edge->getProbability());
            }
        }
        require(currentProbabilities.size() <= 12, "too many rewritten worlds");
        Marginals result;
        double evidenceWeight = 0.0;
        for (std::uint64_t world = 0; world < (std::uint64_t{1} << currentProbabilities.size()); ++world) {
            const auto truth = evaluate(view, [&](const NodePtr& node) {
                const auto found = currentFacts.find(node);
                return found == currentFacts.end() ? node->getProbability() == 1 : ((world >> found->second) & 1U) != 0;
            }, [&](const EdgePtr& edge) {
                const auto found = currentRules.find(edge);
                return found == currentRules.end() ? edge->getProbability() == 1 : ((world >> found->second) & 1U) != 0;
            });
            const auto probability = weight(currentProbabilities, world);
            actualJoint[signature(truth)] += probability;
            if (!admitted(truth, evidence)) continue;
            evidenceWeight += probability;
            for (const auto& node : retained) if (truth.at(node)) result[node] += probability;
        }
        for (const auto& [event, probability] : expectedJoint) close(actualJoint[event], probability,
                "independent compound WMC changed the complete retained joint distribution");
        for (const auto& [event, probability] : actualJoint) close(expectedJoint[event], probability,
                "independent compound WMC introduced a joint outcome");
        require(evidenceWeight > 0, "rewritten evidence became impossible");
        for (const auto& node : retained) {
            result[node] /= evidenceWeight;
            close(result[node], conditional(node, evidence), "retained conditional marginal changed");
        }
        return result;
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
        out << node.get() << ':' << node->getProbability() << ':' << node->pruned << node->isFact
            << node->isOriginalFactNode() << node->needOutput << node->isQuery << node->isShadow
            << node->hasEvidence() << node->getEvidenceValue();
        for (const auto token : node->getProbabilisticSupportTokens()) out << 's' << token;
        for (const auto& edge : node->getIncomingEdges()) out << 'i' << edge.get();
        for (const auto& edge : node->getOutgoingEdges()) out << 'o' << edge.get();
    }
    for (const auto& edge : edges) {
        out << edge.get() << ':' << edge->getOutput().get() << ':' << edge->getProbability() << ':' << edge->pruned;
        for (const auto& input : edge->getInputs()) out << 'b' << input.get();
        for (const auto sign : edge->getBodyNegations()) out << sign;
        for (const auto token : edge->getProbabilisticSupportTokens()) out << 's' << token;
    }
    return out.str();
}

template <typename Result>
void sharedWork(const Result& result, std::size_t materialized) {
    require(result.stats.collectionPasses == 1 && result.stats.sccPasses == 1 &&
                    result.stats.supportPasses == 1 && result.stats.retirementBatches == 1,
            "combined rewrite repeated preparation or retirement");
    require(result.stats.materializedCompoundEdges == materialized &&
                    result.series.compoundEdges.size() == materialized,
            "virtual edges were materialized before the terminal plan finished");
    require(result.stats.zeroHitOwnerCommits == 0, "zero-hit owner commit counter is nonzero");
}

void mixedGraphWithConditionalEvidence(bool observed) {
    Fixture f;
    const auto seed = f.fact("F", 0.3), other = f.fact("G", 0.4), h = f.fact("H", 0.5);
    const auto a = f.node("A"), b = f.node("B"), side = f.node("Side");
    const auto middle = f.node("Middle"), out = f.node("Out"), query = f.node("Q");
    const auto z = f.node("Z"), terminal = f.node("Terminal");
    const auto protectedQuery = f.node("Protected"), joint = f.node("Joint");
    f.edge({seed}, a);
    f.edge({other}, a);
    f.edge({a, seed}, b, 0.6);
    f.edge({b, h}, side, 0.8);
    const auto first = f.edge({a, seed}, middle, 0.7);
    const auto second = f.edge({middle, seed, h}, out, 0.8);
    const auto querySource = f.edge({out}, query, 0.9);
    f.edge({a}, z, 0.6);
    f.edge({z}, terminal, 0.8);
    f.edge({a}, protectedQuery, 0.6);
    f.edge({protectedQuery, seed}, joint);
    for (const auto& node : {b, side, out, query, terminal, protectedQuery, joint}) node->setQuery();
    seed->setEvidence(observed);
    auto view = f.view();
    const Worlds worlds(view);
    view.getIncomingEdges(out);
    view.getOutgoingEdges(middle);
    view.getValidEdges();
    const auto allocations = f.graph.allocatedEdges();
    const auto result = rewritePrivateFactors(f.graph, view, true, true, true);
    sharedWork(result, 1);
    require(result.series.stats.contractions == 2 && result.series.stats.addedEdges == 2 &&
                    result.series.stats.duplicateInputsRemoved == 1 && result.terminal.records.size() == 2 &&
                    result.terminal.stats.factoredOutputQueries == 2 && result.terminal.stats.hiddenChainSteps == 0,
            "wrong combined series/terminal opportunities");
    require(f.graph.allocatedEdges() == allocations + 1, "retired virtual unary edge consumed an edge id");
    require(result.stats.nodesBefore - result.stats.nodesAfterSeries == 2 &&
                    result.stats.nodesAfterSeries - result.stats.nodesAfter == 2 &&
                    result.stats.edgesBefore - result.stats.edgesAfterSeries == 2 &&
                    result.stats.edgesAfterSeries - result.stats.edgesAfter == 2,
            "virtual-stage graph metrics disagree with the final plan");
    const auto compound = result.series.compoundEdges.front();
    require(compound->getOutput() == out && compound->getInputs() == std::vector<NodePtr>({a, seed, h}),
            "series lost correlated external inputs");
    close(compound->getProbability(), 0.56, "incorrect surviving private factor product");
    require(compound->getProbabilisticSupportTokens() == mergeSupportTokenLists(
                    {&first->getProbabilisticSupportTokens(), &second->getProbabilisticSupportTokens()}),
            "surviving compound lost primitive rule identities");
    require(view.getIncomingEdges(out) == std::vector<EdgePtr>{compound} &&
                    view.getOutgoingEdges(middle).empty() && !view.getValidEdges().count(first) &&
                    !view.getValidEdges().count(second) && !view.getValidEdges().count(querySource),
            "retirement retained stale source/consumer caches");
    require(view.getNodes().count(protectedQuery) && view.getNodes().count(joint) &&
                    protectedQuery->isQuery && protectedQuery->needOutput,
            "terminal factoring removed an event used in a declared joint query");
    require(a->needOutput && !a->isQuery && result.terminal.promotedOutputRoots == std::vector<NodePtr>{a},
            "OR parent was not retained as a hidden output");
    auto probabilities = worlds.verify(view, {seed});
    precomputedProbResult[a] = 0.01;
    evaluateTerminalQueryFactors(result.terminal, probabilities);
    precomputedProbResult.clear();
    for (const auto& record : result.terminal.records) close(probabilities.at(record.query),
            worlds.conditional(record.query, {seed}), "deferred output changed under conditional evidence");
    require(!f.graph.findNode(query->getTuple()) && !f.graph.findNode(terminal->getTuple()),
            "deferred output name was rebound to a different event");
    const auto pruned = f.graph.prune(std::vector<std::string>{"B", "Side", "Out", "Q", "Terminal", "Protected", "Joint"});
    require(!pruned.getNodes().count(middle) && !pruned.getNodes().count(z) &&
                    !pruned.getNodes().count(query) && !pruned.getNodes().count(terminal),
            "owner prune revived a retired fused rewrite event");
    worlds.verify(pruned, {seed});
}

void virtualUnaryOutputWithoutMaterialization() {
    for (const double factor : {0.6, 1.0}) {
        Fixture f;
        const auto seed = f.fact("F", 0.3), other = f.fact("G", 0.4);
        const auto a = f.node("A"), z = f.node("Z"), query = f.node("Q");
        f.edge({seed}, a);
        f.edge({other}, a);
        const auto first = f.edge({a}, z, factor);
        const auto second = f.edge({z, a}, query, 0.8);
        query->setQuery();
        auto view = f.view();
        const Worlds worlds(view);
        const auto allocations = f.graph.allocatedEdges();
        const auto result = rewritePrivateFactors(f.graph, view, true, true, true);
        sharedWork(result, 0);
        require(result.series.stats.contractions == 1 && result.series.stats.addedEdges == 1 &&
                        result.series.stats.duplicateInputsRemoved == 1 && result.terminal.records.size() == 1 &&
                        result.stats.virtualCompoundEdges == 1 && result.stats.terminalVirtualSources == 1,
                "terminal phase used the stale pre-series body/source");
        require(f.graph.allocatedEdges() == allocations && result.terminal.records[0].query == query &&
                        result.terminal.records[0].parent == a,
                "a terminal virtual compound was materialized or lost its output identity");
        close(result.terminal.records[0].factor, factor * 0.8, "virtual compound factor was not deferred");
        require(!f.graph.getNodes().count(z) && !f.graph.getNodes().count(query) &&
                        !f.graph.getEdges().count(first) && !f.graph.getEdges().count(second),
                "one-shot retirement left a virtual intermediate in the owner");
        auto probabilities = worlds.verify(view);
        evaluateTerminalQueryFactors(result.terminal, probabilities);
        close(probabilities.at(query), worlds.conditional(query, {}), "virtual terminal output probability changed");
    }
}

void updatedDegreesExposeASecondSeriesStep() {
    Fixture f;
    const auto seed = f.fact("F", 0.3), other = f.fact("G", 0.4);
    const auto a = f.node("A"), z = f.node("Z"), middle = f.node("Middle"), query = f.node("Q");
    f.edge({seed}, a);
    f.edge({other}, a);
    f.edge({a}, z, 0.6);
    f.edge({z, a}, middle, 0.7);
    f.edge({middle, z}, query, 0.8);
    query->setQuery();
    auto view = f.view();
    const Worlds worlds(view);
    require(view.getOutgoingEdges(z).size() == 2, "fixture does not start with a shared intermediate");
    const auto allocations = f.graph.allocatedEdges();
    const auto result = rewritePrivateFactors(f.graph, view, true, true, true);
    sharedWork(result, 0);
    require(result.series.stats.contractions == 2 && result.series.stats.duplicateInputsRemoved == 2 &&
                    result.series.stats.addedEdges == 1 && result.terminal.records.size() == 1 &&
                    result.stats.virtualCompoundEdges == 2 && result.stats.terminalVirtualSources == 1 &&
                    f.graph.allocatedEdges() == allocations,
            "updated source/body degrees did not expose the second series step");
    close(result.terminal.records[0].factor, 0.336, "multi-step virtual factor product changed");
    auto probabilities = worlds.verify(view);
    evaluateTerminalQueryFactors(result.terminal, probabilities);
    close(probabilities.at(query), worlds.conditional(query, {}), "degree work queue changed output semantics");
}

void terminalQueueAndHiddenChainRecords() {
    Fixture f;
    const auto seed = f.fact("F", 0.3), other = f.fact("G", 0.4);
    const auto a = f.node("A"), parent = f.node("Parent"), first = f.node("First"), second = f.node("Second");
    f.edge({seed}, a);
    f.edge({other}, a);
    f.edge({a}, parent, 0.6);
    f.edge({parent}, first, 0.7);
    f.edge({parent}, second, 0.8);
    first->setQuery();
    second->setQuery();
    auto view = f.view();
    const Worlds worlds(view);
    const auto result = rewritePrivateFactors(f.graph, view, true, true, true);
    sharedWork(result, 0);
    require(result.series.stats.contractions == 0 && result.terminal.records.size() == 3 &&
                    result.terminal.stats.factoredOutputQueries == 2 && result.terminal.stats.hiddenChainSteps == 1,
            "terminal degree queue missed a newly exposed hidden chain step");
    const std::unordered_set<NodePtr> hidden(result.terminal.promotedOutputRoots.begin(),
            result.terminal.promotedOutputRoots.end());
    require(hidden == std::unordered_set<NodePtr>{a, parent} && a->needOutput && !a->isQuery &&
                    !parent->isQuery && !parent->needOutput && first->isQuery && second->isQuery,
            "chain records changed visible query names or leaked an intermediate output");
    for (const auto& record : result.terminal.records) require(record.parent == a,
            "terminal chain record still points at a retired graph node");
    auto probabilities = worlds.verify(view);
    evaluateTerminalQueryFactors(result.terminal, probabilities);
    for (const auto& node : {first, second, parent}) close(probabilities.at(node), worlds.conditional(node, {}),
            "flattened terminal chain output changed");
}

void virtualSignsAlternativeSourcesAndJointConsumers() {
    // A positive sole use may substitute a signed external body. The terminal
    // phase must inspect that new body rather than the old positive unary edge.
    {
        Fixture f;
        const auto a = f.fact("A"), z = f.node("Z"), query = f.node("Q");
        f.edge({a}, z, 0.6, {true});
        f.edge({z}, query, 0.8);
        query->setQuery();
        auto view = f.view();
        const Worlds worlds(view);
        const auto result = rewritePrivateFactors(f.graph, view, true, true, true);
        sharedWork(result, 1);
        require(result.series.stats.contractions == 1 && result.terminal.records.empty() &&
                        result.series.compoundEdges[0]->getInputs() == std::vector<NodePtr>{a} &&
                        result.series.compoundEdges[0]->getBodyNegations() == std::vector<bool>{true},
                "terminal phase used stale signs after series substitution");
        worlds.verify(view);
    }
    // Substitution changes one OR contribution without removing another source.
    {
        Fixture f;
        const auto a = f.fact("A"), z = f.node("Z"), query = f.node("Q");
        f.edge({a}, z, 0.6);
        f.edge({z, a}, query, 0.8);
        const auto alternative = f.edge({a}, query, 0.9);
        query->setQuery();
        auto view = f.view();
        const Worlds worlds(view);
        const auto result = rewritePrivateFactors(f.graph, view, true, true, true);
        sharedWork(result, 1);
        require(result.series.stats.contractions == 1 && result.terminal.records.empty() &&
                        view.getIncomingEdges(query).size() == 2 && view.getEdges().count(alternative),
                "terminal phase ignored a retained alternative definition");
        close(alternative->getProbability(), 0.9, "alternative rule identity was changed");
        worlds.verify(view);
    }
    // Both signs count as downstream event uses. A declared joint event cannot
    // use a deferred output marginal as if it were an independent fact.
    for (const bool negative : {false, true}) {
        Fixture f;
        const auto a = f.fact("A"), z = f.node("Z"), query = f.node("Q"), joint = f.node("Joint");
        f.edge({a}, z, 0.6);
        f.edge({z, a}, query, 0.8);
        f.edge({query, a}, joint, 1.0, {negative, false});
        query->setQuery();
        joint->setQuery();
        auto view = f.view();
        const Worlds worlds(view);
        const auto result = rewritePrivateFactors(f.graph, view, true, true, true);
        sharedWork(result, 1);
        require(result.series.stats.contractions == 1 && result.terminal.records.empty() &&
                        view.getNodes().count(query) && view.getNodes().count(joint),
                "a signed joint consumer failed to protect its event");
        worlds.verify(view);
    }
}

void observedVirtualOutputsRemainEvents(bool observed) {
    Fixture f;
    const auto a = f.fact("A"), z = f.node("Z"), query = f.node("Q");
    f.edge({a}, z, 0.6);
    f.edge({z, a}, query, 0.8);
    query->setQuery();
    query->setEvidence(observed);
    auto view = f.view();
    const Worlds worlds(view);
    const auto result = rewritePrivateFactors(f.graph, view, true, true, true);
    sharedWork(result, 1);
    require(result.series.stats.contractions == 1 && result.terminal.records.empty() &&
                    view.getNodes().count(query) && query->hasEvidence() && query->getEvidenceValue() == observed,
            "terminal phase deferred an observed virtual query");
    worlds.verify(view, {query});
}

void conservativeCombinedRefusals() {
    for (unsigned scenario = 0; scenario < 13; ++scenario) {
        Fixture f;
        const auto a = f.fact("A"), z = f.node("Z"), query = f.node("Q");
        const auto first = f.edge({a}, z, 0.6);
        f.edge({z, a}, query, 0.8, scenario == 9 ? std::vector<bool>{true, false} : std::vector<bool>{});
        query->setQuery();
        bool complete = true;
        switch (scenario) {
            case 0:
                z->isFact = true;
                z->setOriginalFact();
                z->setProbability(0.2);
                break;
            case 1: z->isShadow = true; break;
            case 2: a->isShadow = true; break;
            case 3: z->setEvidence(true); break;
            case 4: z->setEvidence(false); break;
            case 5: {
                const auto shared = f.node("Shared");
                shared->setQuery();
                const auto edge = f.edge({a, query}, shared, 0.9);
                edge->setProbabilisticSupportTokens(first->getProbabilisticSupportTokens());
                break;
            }
            case 6: first->clearProbabilisticSupportTokens(); break;
            case 7: f.edge({a}, z, 0.9); break;
            case 8: {
                const auto consumer = f.node("Consumer");
                consumer->setQuery();
                f.edge({z, a}, consumer);
                break;
            }
            case 9: break;  // Negative sole use of the intermediate.
            case 10: complete = false; break;
            case 11: {
                const auto recursive = f.node("Recursive");
                f.edge({query, a}, recursive);
                f.edge({recursive}, a);
                break;
            }
            case 12: {
                const auto unrelated = f.fact("Malformed", 0.2);
                unrelated->setProbability(std::numeric_limits<double>::quiet_NaN());
                break;
            }
        }
        auto view = f.view();
        const auto ownerBefore = structure(f.graph), viewBefore = structure(view);
        const auto allocations = f.graph.allocatedEdges();
        const auto result = rewritePrivateFactors(f.graph, view, true, true, complete);
        require(result.series.stats.contractions == 0 && result.terminal.records.empty() &&
                        result.stats.retirementBatches == 0 && result.stats.materializedCompoundEdges == 0 &&
                        f.graph.allocatedEdges() == allocations && structure(f.graph) == ownerBefore &&
                        structure(view) == viewBefore,
                "combined refusal mutated graph/flags in scenario " + std::to_string(scenario));
    }
    // Evidence whose primitive factor is shared with a proposed terminal factor
    // also blocks deferral, even when the evidence is outside its ancestor DAG.
    Fixture f;
    const auto a = f.fact("A"), query = f.node("Q"), evidence = f.node("Evidence");
    const auto source = f.edge({a}, query, 0.7);
    const auto observedSource = f.edge({a}, evidence, 0.6);
    observedSource->setProbabilisticSupportTokens(source->getProbabilisticSupportTokens());
    query->setQuery();
    evidence->setEvidence(true);
    auto view = f.view();
    const auto before = structure(f.graph);
    const auto result = rewritePrivateFactors(f.graph, view, true, true, true);
    require(result.series.stats.contractions == 0 && result.terminal.records.empty() &&
                    result.stats.retirementBatches == 0 && structure(f.graph) == before,
            "global evidence support overlap was ignored");
}

void zeroHitsDoNotCommitOwnerHistory() {
    Fixture f;
    const auto a = f.fact("A"), b = f.fact("B"), query = f.node("Q"), history = f.node("History");
    const auto active = f.edge({a, b}, query, 0.7);
    const auto old = f.edge({a}, history, 0.8);
    query->setQuery();
    history->pruned = true;
    old->pruned = true;
    WorkingSubgraphView view({a, b, query}, {active});
    view.getIncomingEdges(query);
    view.getOutgoingEdges(a);
    view.getValidNodes();
    const auto ownerBefore = structure(f.graph), viewBefore = structure(view);
    const auto allocations = f.graph.allocatedEdges();
    const auto result = rewritePrivateFactors(f.graph, view, true, true, true, true);
    require(result.series.stats.contractions == 0 && result.terminal.records.empty() &&
                    result.stats.retirementBatches == 0 && result.stats.zeroHitOwnerCommits == 0 &&
                    result.stats.removedOwnerNodes == 0 && result.stats.removedOwnerEdges == 0 &&
                    f.graph.allocatedEdges() == allocations && structure(f.graph) == ownerBefore &&
                    structure(view) == viewBefore && f.graph.findNode(history->getTuple()) == history,
            "zero-hit certified view performed owner cleanup or changed flags");
    const auto disabled = rewritePrivateFactors(f.graph, view, false, false, true, true);
    require(disabled.stats.collectionPasses == 0 && disabled.stats.retirementBatches == 0 &&
                    structure(f.graph) == ownerBefore && structure(view) == viewBefore,
            "disabled rewrites collected or changed the source graph");
}

void authoritativeViewDefersOwnerCommitUntilAHit() {
    Fixture f;
    const auto a = f.fact("A"), query = f.node("Q"), history = f.node("History");
    const auto source = f.edge({a}, query, 0.7);
    const auto oldConsumer = f.edge({query}, history, 0.8);
    query->setQuery();
    history->pruned = true;
    oldConsumer->pruned = true;
    WorkingSubgraphView view({a, query}, {source});
    const Worlds worlds(view);
    const auto ownerBefore = structure(f.graph), viewBefore = structure(view);
    const auto standalone = rewritePrivateFactors(f.graph, view, true, true, true);
    require(standalone.series.stats.contractions == 0 && standalone.terminal.records.empty() &&
                    standalone.stats.retirementBatches == 0 && structure(f.graph) == ownerBefore &&
                    structure(view) == viewBefore,
            "standalone incomplete owner history was treated as authoritative");
    const auto allocations = f.graph.allocatedEdges();
    const auto certified = rewritePrivateFactors(f.graph, view, true, true, true, true);
    sharedWork(certified, 0);
    require(certified.terminal.records.size() == 1 && certified.terminal.records[0].query == query &&
                    certified.terminal.records[0].parent == a && f.graph.allocatedEdges() == allocations,
            "authoritative active view did not defer its terminal output");
    require(f.graph.getNodes() == view.getNodes() && f.graph.getEdges() == view.getEdges() &&
                    !f.graph.findNode(history->getTuple()) && !f.graph.findNode(query->getTuple()) &&
                    a->getOutgoingEdges().empty() && query->getIncomingEdges().empty() &&
                    query->getOutgoingEdges().empty(),
            "final retirement did not atomically remove inactive owner history");
    require(certified.stats.removedOwnerNodes == 2 && certified.stats.removedOwnerEdges == 2,
            "owner retirement counters omit historical objects");
    auto probabilities = worlds.verify(view);
    evaluateTerminalQueryFactors(certified.terminal, probabilities);
    close(probabilities.at(query), worlds.conditional(query, {}), "certified owner cleanup changed the marginal");
}

}  // namespace

int main() {
    try {
        DerivationGraph::setMergeBiImpEnabled(false);
        DerivationGraph::setPruneExtraEnabled(false);
        precomputedProbResult.clear();
        precomputedTupleProbResult.clear();
        mixedGraphWithConditionalEvidence(false);
        mixedGraphWithConditionalEvidence(true);
        virtualUnaryOutputWithoutMaterialization();
        updatedDegreesExposeASecondSeriesStep();
        terminalQueueAndHiddenChainRecords();
        virtualSignsAlternativeSourcesAndJointConsumers();
        observedVirtualOutputsRemainEvents(false);
        observedVirtualOutputsRemainEvents(true);
        conservativeCombinedRefusals();
        zeroHitsDoNotCommitOwnerHistory();
        authoritativeViewDefersOwnerCommitUntilAHit();
        std::cout << "private factor fused rewrite regression passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "private factor fused rewrite regression: " << error.what() << '\n';
        return 1;
    }
}
