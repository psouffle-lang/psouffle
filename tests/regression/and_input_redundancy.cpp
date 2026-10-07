#include "souffle/problog/AndInputRedundancy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using souffle::problog::AndInputRedundancyReport;
using souffle::problog::AndInputRedundancyPassStats;
using souffle::problog::detectAndInputRedundancy;
using souffle::problog::eliminateAndInputRedundancy;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct FixtureGraph : WorkingDerivationGraph {
    void startNodeIdsAt(std::size_t first) {
        nextNodeId = first;
    }
};

struct Fixture {
    FixtureGraph graph;
    souffle::RamDomain nextRuleId = 100;

    NodePtr node(const std::string& name) {
        return graph.createNode(UntypedTuple{name, {}});
    }

    NodePtr fact(const std::string& name, double probability = 0.5) {
        auto result = node(name);
        result->isFact = true;
        result->setOriginalFact();
        result->setProbability(probability);
        return result;
    }

    EdgePtr edge(const std::vector<NodePtr>& inputs, const NodePtr& output,
            double probability = 1.0, std::vector<bool> negations = {}) {
        RuleApplication application;
        application.ruleId = nextRuleId++;
        EdgePtr result;
        if (inputs.empty()) {
            result = graph.createHyperedge(inputs, output, application);
        } else {
            result = graph.createHyperedge(inputs, output, nullptr, negations, application);
        }
        result->setProbability(probability);
        return result;
    }
};

bool hasProof(const AndInputRedundancyReport& report, const EdgePtr& edge, const NodePtr& input) {
    return std::any_of(report.proofs.begin(), report.proofs.end(), [&](const auto& proof) {
        return proof.edge == edge && proof.redundant == input;
    });
}

template <class Edges>
bool containsEdge(const Edges& edges, const EdgePtr& edge) {
    return std::find(edges.begin(), edges.end(), edge) != edges.end();
}

std::string snapshot(const DerivationGraphViewInterface& view) {
    std::ostringstream out;
    auto nodes = std::vector<NodePtr>(view.getNodes().begin(), view.getNodes().end());
    auto edges = std::vector<EdgePtr>(view.getEdges().begin(), view.getEdges().end());
    std::sort(nodes.begin(), nodes.end(), [](const auto& left, const auto& right) {
        return left->getId() < right->getId();
    });
    std::sort(edges.begin(), edges.end(), [](const auto& left, const auto& right) {
        return left->getId() < right->getId();
    });
    out.precision(17);
    for (const auto& node : nodes) {
        out << node.get() << ' ' << node->getId() << ' ' << node->getSemanticFactId() << ' '
            << node->getTuple().toString() << ' ' << node->getProbability() << ' '
            << node->isFact << node->isOriginalFactNode() << node->isShadow << node->pruned
            << node->needOutput << node->isQuery << node->hasEvidence() << node->getEvidenceValue();
        for (auto token : node->getProbabilisticSupportTokens()) out << " s" << token;
        for (const auto& edge : node->getIncomingEdges()) out << " i" << edge.get();
        for (const auto& edge : node->getOutgoingEdges()) out << " o" << edge.get();
        out << '\n';
    }
    for (const auto& edge : edges) {
        out << edge.get() << ' ' << edge->getId() << ' ' << edge->getOutput().get() << ' '
            << edge->getProbability() << ' ' << edge->getRule() << ' '
            << edge->getRuleApp().ruleId << ' ' << edge->pruned;
        for (auto value : edge->getRuleApp().varValuesPure) out << " v" << value;
        for (const auto& input : edge->getInputs()) out << " i" << input.get();
        for (bool negated : edge->getBodyNegations()) out << " n" << negated;
        for (auto token : edge->getProbabilisticSupportTokens()) out << " s" << token;
        out << ' ' << edge->cachedSortedInputs.has_value() << edge->cachedSortedBodyNegations.has_value()
            << edge->cachedEdgeKey.has_value() << edge->cachedSelfDependency.has_value() << '\n';
    }
    out << view.cachedIncomingEdges.size() << ' ' << view.cachedOutgoingEdges.size() << ' '
        << view.cachedSortedIncomingEdges.size();
    return out.str();
}

using Truth = std::unordered_map<NodePtr, bool>;

struct WorldEvaluator {
    const DerivationGraphViewInterface& view;
    std::unordered_map<NodePtr, std::size_t> factBits;
    std::unordered_map<EdgePtr, std::size_t> edgeBits;
    std::unordered_map<NodePtr, std::vector<EdgePtr>> incoming;
    std::size_t randomEvents = 0;

    explicit WorldEvaluator(const DerivationGraphViewInterface& graph) : view(graph) {
        for (const auto& node : view.getNodes()) {
            if (node->isFact && node->getProbability() > 0.0 && node->getProbability() < 1.0) {
                factBits[node] = randomEvents++;
            }
        }
        for (const auto& edge : view.getEdges()) {
            incoming[edge->getOutput()].push_back(edge);
            if (edge->getProbability() > 0.0 && edge->getProbability() < 1.0) {
                edgeBits[edge] = randomEvents++;
            }
        }
        require(randomEvents < 20, "test graph has too many random worlds");
    }

    Truth evaluate(std::uint64_t world, const EdgePtr& changedEdge = {},
            const std::unordered_set<std::size_t>& omittedInputs = {}) const {
        Truth truth;
        std::unordered_set<NodePtr> visiting;
        std::function<bool(const NodePtr&)> visit = [&](const NodePtr& node) {
            const auto known = truth.find(node);
            if (known != truth.end()) return known->second;
            require(visiting.insert(node).second, "world enumerator requires an acyclic graph");
            bool value = false;
            if (node->isFact) {
                const auto bit = factBits.find(node);
                value = bit == factBits.end() ? node->getProbability() == 1.0
                                              : ((world >> bit->second) & 1U) != 0;
            }
            const auto sources = incoming.find(node);
            if (sources != incoming.end()) {
                for (const auto& edge : sources->second) {
                    const auto bit = edgeBits.find(edge);
                    bool contribution = bit == edgeBits.end() ? edge->getProbability() == 1.0
                                                              : ((world >> bit->second) & 1U) != 0;
                    const auto& inputs = edge->getInputs();
                    const auto& negations = edge->getBodyNegations();
                    for (std::size_t i = 0; i < inputs.size(); ++i) {
                        if (edge == changedEdge && omittedInputs.count(i)) continue;
                        const bool input = visit(inputs[i]);
                        contribution = contribution && (negations[i] ? !input : input);
                    }
                    value = value || contribution;
                }
            }
            visiting.erase(node);
            truth[node] = value;
            return value;
        };
        for (const auto& node : view.getNodes()) visit(node);
        return truth;
    }
};

std::string eventIdentity(const DerivationGraphViewInterface& view) {
    std::ostringstream out;
    out.precision(17);
    auto nodes = std::vector<NodePtr>(view.getNodes().begin(), view.getNodes().end());
    auto edges = std::vector<EdgePtr>(view.getEdges().begin(), view.getEdges().end());
    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) { return a->getId() < b->getId(); });
    std::sort(edges.begin(), edges.end(), [](const auto& a, const auto& b) { return a->getId() < b->getId(); });
    for (const auto& node : nodes) {
        out << node.get() << ' ' << node->getId() << ' ' << node->getSemanticFactId() << ' '
            << node->getTuple().toString() << ' ' << node->getProbability() << ' '
            << node->isFact << node->isOriginalFactNode() << node->isShadow << node->pruned
            << node->needOutput << node->isQuery << node->hasEvidence() << node->getEvidenceValue();
        for (auto token : node->getProbabilisticSupportTokens()) out << " s" << token;
        out << '\n';
    }
    for (const auto& edge : edges) {
        out << edge.get() << ' ' << edge->getId() << ' ' << edge->getOutput().get() << ' '
            << edge->getProbability() << ' ' << edge->getRule() << ' '
            << edge->getRuleApp().ruleId << ' ' << edge->pruned;
        for (auto value : edge->getRuleApp().varValuesPure) out << " v" << value;
        for (auto token : edge->getProbabilisticSupportTokens()) out << " s" << token;
        out << '\n';
    }
    return out.str();
}

AndInputRedundancyPassStats verifyActualPass(WorkingSubgraphView& view, std::size_t expectedDeletions) {
    WorldEvaluator evaluator(view);
    std::vector<Truth> before;
    for (std::uint64_t world = 0; world < (std::uint64_t{1} << evaluator.randomEvents); ++world) {
        before.push_back(evaluator.evaluate(world));
    }
    const auto identity = eventIdentity(view);
    std::size_t associations = 0;
    for (const auto& edge : view.getEdges()) associations += edge->getInputs().size();
    const auto stats = eliminateAndInputRedundancy(view, true);
    require(stats.deletedInputAssociations == expectedDeletions, "actual pass deleted the wrong number of inputs");
    require(stats.initialInputAssociations == associations &&
                    stats.finalInputAssociations + stats.deletedInputAssociations == associations,
            "actual pass association accounting is incorrect");
    require(eventIdentity(view) == identity, "actual pass changed a node, rule event, edge identity or probability");
    require(stats.remainingInputAssociations == 0 && detectAndInputRedundancy(view, true).proofs.empty(),
            "actual pass did not reach a locally certified fixpoint");
    // Keep the original factBits and edgeBits: renumbering or replacing an
    // independent random event cannot hide behind matching marginal probabilities.
    for (std::uint64_t world = 0; world < before.size(); ++world) {
        require(before[world] == evaluator.evaluate(world), "actual mutation changed the complete world truth vector");
    }
    std::size_t finalAssociations = 0;
    for (const auto& edge : view.getEdges()) {
        finalAssociations += edge->getInputs().size();
        require(edge->getInputs().size() == edge->getBodyNegations().size(), "mutation misaligned body polarity");
    }
    require(finalAssociations == stats.finalInputAssociations, "final input count does not describe the mutated graph");
    const auto final = snapshot(view);
    const auto second = eliminateAndInputRedundancy(view, true);
    require(second.deletedInputAssociations == 0 && snapshot(view) == final, "pass is not idempotent");
    return stats;
}

AndInputRedundancyPassStats verifyActualPass(Fixture& fixture, std::size_t expectedDeletions) {
    WorkingSubgraphView view(fixture.graph.getNodes(), fixture.graph.getEdges());
    return verifyActualPass(view, expectedDeletions);
}

void requireNoMutation(Fixture& fixture, bool complete = true) {
    WorkingSubgraphView view(fixture.graph.getNodes(), fixture.graph.getEdges());
    const auto before = snapshot(view);
    const auto stats = eliminateAndInputRedundancy(view, complete);
    require(stats.deletedInputAssociations == 0 && snapshot(view) == before,
            "unsupported structure was changed by the actual pass");
}

void verifyEveryProof(const DerivationGraphViewInterface& view, const AndInputRedundancyReport& report) {
    const auto before = snapshot(view);
    WorldEvaluator evaluator(view);
    for (const auto& proof : report.proofs) {
        require(proof.inputIndex < proof.edge->getInputs().size(), "proof has an invalid input index");
        require(proof.edge->getInputs()[proof.inputIndex] == proof.redundant,
                "proof input index does not identify the redundant node");
        require(proof.definition->getOutput() == proof.redundant, "proof has the wrong definition");
        for (std::uint64_t world = 0; world < (std::uint64_t{1} << evaluator.randomEvents); ++world) {
            const auto original = evaluator.evaluate(world);
            const auto rewritten = evaluator.evaluate(world, proof.edge, {proof.inputIndex});
            // Equality of the complete truth vector also preserves every joint query
            // and the event selected by any positive or negative evidence assignment.
            require(original == rewritten, "certified deletion changed a node in a random world");
        }
    }
    require(snapshot(view) == before, "proof verification changed the graph");
}

AndInputRedundancyReport detectReadOnly(const DerivationGraphViewInterface& view, bool complete = true) {
    const auto before = snapshot(view);
    auto report = detectAndInputRedundancy(view, complete);
    require(snapshot(view) == before, "detector mutated graph state or adjacency caches");
    require(report.stats.provenInputAssociations == report.proofs.size(), "proof count disagrees with stats");
    return report;
}

void collectiveProofAndEventIdentity() {
    Fixture f;
    const auto a = f.fact("CandidateX");
    const auto b = f.fact("CandidateY", 0.4);
    const auto q = f.fact("IndependentAlternative", 0.3);
    const auto conflict = f.node("Conflict");
    const auto x = f.node("TotalX");
    const auto z = f.node("TotalY");
    const auto y = f.node("Output");
    const auto joint = f.node("Joint");
    const auto definition = f.edge({a, b}, conflict);
    f.edge({a}, x, 0.7);
    f.edge({b}, z, 0.8);
    const auto target = f.edge({conflict, x, z}, y, 0.6);
    f.edge({q}, y, 0.2);
    f.edge({y, a}, joint);
    y->setQuery();
    x->setEvidence(true);
    b->setEvidence(false);
    const auto report = detectReadOnly(f.graph);
    require(report.proofs.size() == 1 && hasProof(report, target, conflict), "collective proof was missed");
    require(report.proofs.front().definition == definition, "definition identity was lost");
    require(report.stats.affectedEdges == 1 && report.stats.distinctRedundantNodes == 1,
            "collective-proof counts are incorrect");
    verifyEveryProof(f.graph, report);
    const auto stats = verifyActualPass(f, 1);
    require(stats.rounds == 1, "unchanged definitions required a redundant terminal detection round");
    require(target->getInputs() == std::vector<NodePtr>{x, z}, "collective deletion did not preserve other input order");
}

void randomDefinitionAndFactSources() {
    for (double probability : {0.7, std::nextafter(1.0, 0.0)}) {
        Fixture f;
        const auto a = f.fact("A");
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        f.edge({a}, c, probability);
        f.edge({a}, x, 0.8);
        const auto target = f.edge({c, x}, y);
        require(!hasProof(detectReadOnly(f.graph), target, c), "random definition was treated as deterministic");
        verifyActualPass(f, 0);
    }
    for (bool currentFact : {false, true}) {
        Fixture f;
        const auto a = f.fact("A");
        const auto c = f.fact("C", 0.4);
        const auto x = f.node("X");
        const auto y = f.node("Y");
        c->isFact = currentFact;
        f.edge({a}, c);
        f.edge({a}, x, 0.8);
        const auto target = f.edge({c, x}, y);
        require(!hasProof(detectReadOnly(f.graph), target, c), "additional/original fact source was ignored");
        verifyActualPass(f, 0);
    }
}

void alternativeDefinitionsAndWitnesses() {
    {
        Fixture f;
        const auto a = f.fact("A");
        const auto b = f.fact("B");
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        f.edge({a}, c);
        f.edge({b}, c);
        f.edge({a}, x, 0.8);
        const auto target = f.edge({c, x}, y);
        require(!hasProof(detectReadOnly(f.graph), target, c), "alternative definition of C was ignored");
        verifyActualPass(f, 0);
    }
    for (int alternative = 0; alternative != 3; ++alternative) {
        Fixture f;
        const auto a = f.fact("A");
        const auto b = f.fact("B");
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        f.edge({a}, c);
        f.edge({a, b}, x, 0.8);
        if (alternative == 0) f.edge({a}, x, 0.6);
        if (alternative == 1) f.edge({b}, x, 0.6);
        if (alternative == 2) f.edge({a}, x, 0.6, {true});
        const auto target = f.edge({c, x}, y, 0.7);
        const auto report = detectReadOnly(f.graph);
        require(hasProof(report, target, c) == (alternative == 0), "Must_1 failed to intersect all sources");
        if (alternative == 0) verifyEveryProof(f.graph, report);
        verifyActualPass(f, alternative == 0 ? 1 : 0);
    }
    {
        Fixture f;
        const auto a = f.fact("A");
        const auto c = f.node("C");
        const auto x = f.fact("X");
        const auto y = f.node("Y");
        f.edge({a}, c);
        f.edge({a}, x, 0.8);
        const auto target = f.edge({c, x}, y);
        require(!hasProof(detectReadOnly(f.graph), target, c), "Must_1 ignored a witness fact source");
        verifyActualPass(f, 0);
    }
}

void absentEmptyNegativeAndAliasCases() {
    for (int mode = 0; mode != 7; ++mode) {
        Fixture f;
        const auto a = f.fact("A");
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        if (mode != 0) f.edge(mode == 1 ? std::vector<NodePtr>{} : std::vector<NodePtr>{a}, c,
                1.0, mode == 2 ? std::vector<bool>{true} : std::vector<bool>{});
        if (mode == 3) f.edge({}, x, 0.8);
        else if (mode != 4) f.edge({a}, x, 0.8);
        if (mode == 6) a->isShadow = true;
        const auto target = f.edge({c, x}, y, 0.7, mode == 5 ? std::vector<bool>{true, false}
                                                                          : std::vector<bool>{});
        require(!hasProof(detectReadOnly(f.graph), target, c), "unsupported empty/negative/alias source accepted");
        verifyActualPass(f, 0);
    }
    {
        Fixture f;
        const auto a = f.fact("A");
        const auto alias = f.fact("Alias");
        alias->isShadow = true;
        alias->setSemanticFactId(a->getSemanticFactId());
        const auto c = f.node("C");
        const auto y = f.node("Y");
        f.edge({a}, c);
        const auto target = f.edge({c, alias}, y);
        require(!hasProof(detectReadOnly(f.graph), target, c), "proof crossed an alias identity");
        requireNoMutation(f);
    }
}

void recursiveCases() {
    for (int mode = 0; mode != 4; ++mode) {
        Fixture f;
        const auto a = f.fact("A");
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        const auto z = f.node("Z");
        f.edge({a}, c);
        f.edge({a}, x, 0.8);
        const auto target = f.edge({c, x}, y);
        if (mode == 0) f.edge({y}, a);
        if (mode == 1) f.edge({x}, x);
        if (mode == 2) {
            f.edge({y}, z);
            f.edge({z}, y);
        }
        if (mode == 3) {
            f.edge({a}, z);
            f.edge({z}, a);
        }
        require(!hasProof(detectReadOnly(f.graph), target, c), "local recursive structure was accepted");
        requireNoMutation(f);
    }
}

void completeSnapshotsAndStaleAdjacency() {
    Fixture f;
    const auto a = f.fact("A");
    const auto b = f.fact("B");
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    f.edge({a}, c);
    f.edge({a}, x, 0.8);
    const auto oldSource = f.edge({b}, x, 0.7);
    const auto target = f.edge({c, x}, y);
    require(detectAndInputRedundancy(f.graph).proofs.empty(), "default API assumed complete derivations");
    require(detectReadOnly(f.graph, false).proofs.empty(), "incomplete snapshot produced a proof");
    requireNoMutation(f, false);
    require(!hasProof(detectReadOnly(f.graph), target, c), "active alternative source was ignored");
    auto activeEdges = f.graph.getEdges();
    activeEdges.erase(oldSource);
    WorkingSubgraphView residual(f.graph.getNodes(), activeEdges);
    // Removed edges intentionally remain in the underlying node adjacency lists,
    // as they do after existing rewrite passes. Only the active snapshot counts.
    residual.getIncomingEdges(x);
    const auto report = detectReadOnly(residual);
    require(hasProof(report, target, c), "inactive raw adjacency prevented a residual proof");
    verifyEveryProof(residual, report);
    auto incompleteNodes = residual.getNodes();
    incompleteNodes.erase(a);
    WorkingSubgraphView missingEndpoint(incompleteNodes, activeEdges);
    require(!hasProof(detectReadOnly(missingEndpoint), target, c), "missing input endpoint was accepted");
    const auto incomplete = snapshot(missingEndpoint);
    require(eliminateAndInputRedundancy(missingEndpoint, true).deletedInputAssociations == 0 &&
                    snapshot(missingEndpoint) == incomplete,
            "mutation accepted a snapshot with a missing endpoint");
    verifyActualPass(residual, 1);
}

void independentProofsAreNotBatchDeletions() {
    Fixture f;
    const auto a = f.fact("A");
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    f.edge({a}, c);
    f.edge({a}, x);
    const auto target = f.edge({c, x}, y, 0.7);
    const auto report = detectReadOnly(f.graph);
    require(hasProof(report, target, c) && hasProof(report, target, x), "independent redundant inputs missed");
    require(report.stats.provenInputAssociations == 2 && report.stats.affectedEdges == 1 &&
                    report.stats.distinctRedundantNodes == 2,
            "independent opportunity counts are incorrect");
    verifyEveryProof(f.graph, report);
    WorldEvaluator evaluator(f.graph);
    bool batchChangesEvent = false;
    for (std::uint64_t world = 0; world < (std::uint64_t{1} << evaluator.randomEvents); ++world) {
        if (evaluator.evaluate(world) != evaluator.evaluate(world, target, {0, 1})) {
            batchChangesEvent = true;
        }
    }
    require(batchChangesEvent, "test failed to expose mutually dependent batch deletions");
    verifyActualPass(f, 1);
    require(target->getInputs().size() == 1, "mutually redundant inputs were removed together");
}

void multipleDeletionsRefreshBodyAndAdjacencyCaches() {
    Fixture f;
    const auto a = f.fact("A");
    const auto b = f.fact("B");
    const auto c = f.node("C");
    const auto d = f.node("D");
    const auto x = f.node("X");
    const auto z = f.node("Z");
    const auto y = f.node("Y");
    f.edge({a}, c);
    f.edge({b}, d);
    f.edge({a}, x, 0.8);
    f.edge({b}, z, 0.6);
    const auto target = f.edge({c, x, d, z}, y, 0.7);
    WorkingSubgraphView view(f.graph.getNodes(), f.graph.getEdges());
    target->getInputsStable();
    target->getBodyNegationsStable();
    const auto originalKey = target->getEdgeKey();
    target->hasSelfDependency();
    view.getIncomingEdges(y);
    view.getOutgoingEdges(c);
    view.getOutgoingEdges(d);
    verifyActualPass(view, 2);
    require(target->getInputs() == std::vector<NodePtr>{x, z}, "multiple deletions changed surviving input order");
    require(target->getInputsStable().size() == 2 && target->getBodyNegationsStable().size() == 2 &&
                    target->getEdgeKey() != originalKey,
            "edge body caches retained the old inputs");
    require(!containsEdge(view.getOutgoingEdges(c), target) && !containsEdge(view.getOutgoingEdges(d), target),
            "view adjacency caches retained removed input associations");
    require(!containsEdge(c->getOutgoingEdges(), target) && !containsEdge(d->getOutgoingEdges(), target),
            "raw node adjacency retained removed input associations");
    require(containsEdge(view.getIncomingEdges(y), target) && containsEdge(view.getOutgoingEdges(x), target) &&
                    containsEdge(view.getOutgoingEdges(z), target),
            "deletion detached the head or a surviving input");
}

void crossEdgeCertificatesAreRevalidated() {
    Fixture f;
    const auto a = f.fact("A");
    const auto c = f.node("C");
    const auto d = f.node("D");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    f.edge({a}, c);
    f.edge({c}, d);
    const auto source = f.edge({c, a}, x, 0.8);
    const auto target = f.edge({d, x}, y, 0.7);
    const auto report = detectReadOnly(f.graph);
    require(hasProof(report, source, c) && hasProof(report, target, d), "cross-edge fixture lacks initial proofs");
    // Mutating the earlier source removes C from Must_1(X). The later
    // certificate must be checked against this new body rather than reused.
    const auto stats = verifyActualPass(f, 1);
    require(stats.rounds == 1, "stale negative reproof required another round without a definition change");
    require(source->getInputs() == std::vector<NodePtr>{a} &&
                    target->getInputs() == std::vector<NodePtr>{d, x},
            "cross-edge deletion reused an outdated one-layer witness");
}

void shortenedDefinitionEnablesAnotherDeletion() {
    Fixture f;
    const auto a = f.fact("A");
    const auto c = f.node("C");
    const auto d = f.node("D");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    f.edge({a}, c);
    const auto target = f.edge({d, x}, y, 0.7);
    f.edge({a}, x, 0.8);
    const auto definition = f.edge({c, a}, d);
    const auto report = detectReadOnly(f.graph);
    require(hasProof(report, definition, c) && !hasProof(report, target, d),
            "definition-shrinking fixture has the wrong initial opportunities");
    verifyEveryProof(f.graph, report);
    // Initially X guarantees A but does not directly guarantee C. Removing C
    // from D's own later definition makes D removable from an earlier target,
    // which must be revisited before declaring the pass finished.
    const auto stats = verifyActualPass(f, 2);
    require(stats.rounds >= 2, "definition mutation incorrectly certified a fixpoint in its first round");
    require(definition->getInputs() == std::vector<NodePtr>{a} &&
                    target->getInputs() == std::vector<NodePtr>{x},
            "pass stopped before a shortened definition enabled another proof");
}

void irrelevantDefinitionMutationCertifiesFixpoint() {
    for (int use = 0; use < 3; ++use) {
        Fixture f;
        const auto a = f.fact("A");
        const auto q = f.fact("IndependentQ", 0.4);
        const auto c = f.node("C");
        const auto d = f.node("D");
        const auto y = f.node("Y");
        f.edge({a}, c);
        const auto definition = f.edge({c, a}, d);
        // D occurs only negatively, in a mixed-sign body, or alone. None is
        // an eligible target that can gain a proof when D's definition shrinks.
        const auto target = use == 2 ? f.edge({d}, y, 0.7) :
                f.edge({d, q}, y, 0.7, use == 0 ? std::vector<bool>{true, false} :
                                                               std::vector<bool>{false, true});
        const auto originalInputs = target->getInputs();
        const auto originalNegations = target->getBodyNegations();
        const auto report = detectReadOnly(f.graph);
        require(report.proofs.size() == 1 && hasProof(report, definition, c),
                "irrelevant-definition fixture has the wrong initial opportunities");
        verifyEveryProof(f.graph, report);
        const auto stats = verifyActualPass(f, 1);
        require(stats.rounds == 1,
                "definition used only by an ineligible target forced another detection round");
        require(definition->getInputs() == std::vector<NodePtr>{a} &&
                        target->getInputs() == originalInputs &&
                        target->getBodyNegations() == originalNegations,
                "irrelevant-definition mutation changed its negative or unary consumer");
    }
}

void sharedProviderIntersectsEveryAlternative() {
    Fixture f;
    const auto a = f.fact("A");
    const auto b = f.fact("B");
    const auto c = f.node("C");
    const auto d = f.node("D");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    const auto z = f.node("Z");
    f.edge({a}, c);
    f.edge({b}, d);
    f.edge({a, b}, x, 0.8);
    f.edge({a, b}, x, 0.4);
    f.edge({a}, x, 0.6);
    const auto first = f.edge({c, x}, y, 0.7);
    const auto second = f.edge({d, x}, z, 0.9);
    const auto report = detectReadOnly(f.graph);
    require(hasProof(report, first, c) && !hasProof(report, second, d),
            "shared provider failed to distinguish required and optional premises");
    // The same provider is queried for two different premises. A cached result
    // must include every derivation, including the alternative that lacks B.
    verifyActualPass(f, 1);
    require(first->getInputs() == std::vector<NodePtr>{x} &&
                    second->getInputs() == std::vector<NodePtr>{d, x},
            "shared-provider analysis lost an alternative random derivation");
}

void duplicatePremisesKeepFirstCertificateAndSkipUnneededProvider() {
    for (int mode = 0; mode < 4; ++mode) {
        const bool singlePremise = mode == 1;
        const bool wideDefinition = mode >= 2;
        const bool wrapEpoch = mode == 3;
        Fixture f;
        const auto a = f.fact("A");
        const auto b = f.fact("B", 0.4);
        const auto q = f.fact("IndependentQ", 0.3);
        const auto c = f.node("C");
        const auto x = f.node("FirstProvider");
        const auto z = f.node("UnusedProvider");
        const auto y = f.node("Y");
        const auto definition = f.edge(wideDefinition ? std::vector<NodePtr>{a, a, b, b} :
                (singlePremise ? std::vector<NodePtr>{a, a} : std::vector<NodePtr>{a, b}), c);
        const auto firstSource = f.edge({a, b}, x, 0.7);
        f.edge({a, b, q}, z, 0.8);
        f.edge({a, b}, z, 0.6);
        f.edge({a, b, q}, z, 0.5);
        const auto target = f.edge({c, x, z}, y, 0.9);
        const auto before = snapshot(f.graph);
        souffle::problog::detail::AndInputRedundancySnapshot analysis;
        analysis.initialize(f.graph, true, true, true);
        require(analysis.coverageMarks.empty(), "initialization eagerly allocated wide-proof stamps");
        if (wrapEpoch) analysis.coverageEpoch = std::numeric_limits<std::size_t>::max() - 1;
        const auto report = analysis.detect(true);
        require(wideDefinition ? analysis.coverageMarks.size() == analysis.nodes.size() : analysis.coverageMarks.empty(),
                "proof did not allocate coverage stamps only when the wide branch needed them");
        require(report.proofs.size() == 1 && hasProof(report, target, c),
                "duplicate required premises prevented an otherwise complete proof");
        const auto& proof = report.proofs.front();
        require(proof.definition == definition && proof.witnesses.size() == (singlePremise ? 1U : 2U),
                "duplicate premises changed definition identity or certificate cardinality");
        std::unordered_set<NodePtr> premises;
        for (const auto& witness : proof.witnesses) {
            premises.insert(witness.premise);
            require(witness.provider == x && witness.sourceEdges == std::vector<EdgePtr>{firstSource},
                    "later alternative provider replaced the first covering certificate");
        }
        require(premises == (singlePremise ? std::unordered_set<NodePtr>{a} :
                                           std::unordered_set<NodePtr>{a, b}),
                "certificate omitted a required event");
        // Lazy analysis is part of this optimization: the later provider's
        // complete alternative-source intersection is unnecessary for proof.
        require(analysis.mustSlots.at(analysis.indexOf(z)) == analysis.none,
                "complete early coverage still analyzed an unrelated provider");
        require(analysis.bodyMarks.empty(), "single-source proof allocated an unnecessary intersection workspace");
        require(snapshot(f.graph) == before, "early-coverage detection changed graph state");
        verifyEveryProof(f.graph, report);
        verifyActualPass(f, 1);
        require(target->getInputs() == std::vector<NodePtr>{x, z},
                "early coverage changed the retained inputs or independently random provider");
    }
}

void singleSourceThenAlternativeSourcesAllocateIntersectionWorkspace() {
    Fixture f;
    const auto a = f.fact("A");
    const auto b = f.fact("B", 0.4);
    const auto q = f.fact("IndependentQ", 0.3);
    const auto c = f.node("C");
    const auto d = f.node("D");
    const auto x = f.node("SingleSource");
    const auto z = f.node("AlternativeSources");
    const auto y = f.node("Y");
    const auto w = f.node("W");
    f.edge({a, b}, c);
    f.edge({a, b}, d);
    f.edge({a, b}, x, 0.7);
    f.edge({a, b, q}, z, 0.8);
    f.edge({a, b}, z, 0.6);
    const auto first = f.edge({c, x}, y, 0.9);
    const auto second = f.edge({d, z}, w, 0.5);
    const auto before = snapshot(f.graph);
    souffle::problog::detail::AndInputRedundancySnapshot analysis;
    analysis.initialize(f.graph, true, true, true);
    require(analysis.bodyMarks.empty(), "initialization eagerly allocated an intersection workspace");
    const auto firstIndex = static_cast<std::size_t>(std::find_if(analysis.edges.begin(), analysis.edges.end(),
            [&](const auto& edge) { return edge.edge == first; }) - analysis.edges.begin());
    require(firstIndex < analysis.edges.size() && analysis.prove(firstIndex, 0, true),
            "single-source fixture lacks its expected collective proof");
    require(analysis.bodyMarks.empty(), "borrowed single-source proof allocated intersection stamps");
    const auto report = analysis.detect(true);
    require(report.proofs.size() == 2 && hasProof(report, first, c) && hasProof(report, second, d),
            "complete alternative sources lost their common required inputs");
    require(analysis.bodyMarks.size() == analysis.nodes.size(),
            "a needed alternative-source intersection did not initialize its workspace");
    // The certified DAG count must agree with an independently checked snapshot.
    souffle::problog::detail::AndInputRedundancySnapshot certified;
    certified.begin(f.graph.getNodes().size(), f.graph.getEdges().size(), true, true);
    for (const auto& node : f.graph.getNodes()) certified.addNode(node);
    certified.finishNodes();
    for (const auto& edge : f.graph.getEdges()) certified.addEdge(edge);
    certified.finish(false, true);
    require(report.stats.eligibleDefinitions == 2 &&
                    certified.detect(true).stats.eligibleDefinitions == report.stats.eligibleDefinitions,
            "certified DAG eligibility count disagrees with complete source/cycle analysis");
    require(snapshot(f.graph) == before, "lazy workspace preparation changed graph state");
    verifyEveryProof(f.graph, report);
    verifyActualPass(f, 2);
    require(first->getInputs() == std::vector<NodePtr>{x} && second->getInputs() == std::vector<NodePtr>{z},
            "single/multiple-source proofs changed their surviving random providers");
}

void repeatedPremiseDoesNotProvideItsOwnWitness() {
    Fixture f;
    const auto a = f.fact("A");
    const auto q = f.fact("IndependentQ", 0.4);
    const auto c = f.node("C");
    const auto y = f.node("Y");
    f.edge({a, a}, c);
    const auto target = f.edge({c, q}, y, 0.7);
    const auto report = detectReadOnly(f.graph);
    require(!hasProof(report, target, c), "repeated own premise was counted as an independent witness");
    // C is still the event A. Repeating A in its definition cannot allow an
    // unrelated random fact Q to justify removing C from C AND Q.
    verifyActualPass(f, 0);
}

void unrelatedRecursionDoesNotBlockSafeDeletion() {
    Fixture f;
    const auto a = f.fact("A");
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    const auto definition = f.edge({a}, c);
    const auto source = f.edge({a}, x, 0.8);
    const auto target = f.edge({c, x}, y, 0.7);
    WorkingSubgraphView acyclic({a, c, x, y}, {definition, source, target});
    WorldEvaluator evaluator(acyclic);
    std::vector<Truth> original;
    for (std::uint64_t world = 0; world < (std::uint64_t{1} << evaluator.randomEvents); ++world) {
        original.push_back(evaluator.evaluate(world));
    }
    const auto r = f.fact("IndependentCycleSeed", 0.4);
    const auto u = f.node("RecursiveU");
    const auto v = f.node("RecursiveV");
    const auto seed = f.edge({r}, u, 0.3);
    const auto forward = f.edge({r, v}, u, 0.6);
    const auto backward = f.edge({u}, v);
    const auto descendant = f.node("AcyclicCycleDescendant");
    f.edge({u, u}, descendant);
    WorkingSubgraphView recursive({r, u, v}, {seed, forward, backward});
    const auto cycleSnapshot = snapshot(recursive);
    WorkingSubgraphView full(f.graph.getNodes(), f.graph.getEdges());
    require(detectReadOnly(full).stats.recursiveNodes == 2,
            "acyclic descendant or duplicate arc changed the exact cycle statistic");
    const auto identity = eventIdentity(full);
    const auto stats = eliminateAndInputRedundancy(full, true);
    require(stats.deletedInputAssociations == 1 && eventIdentity(full) == identity,
            "unrelated recursion blocked the safe deletion or changed an event");
    // The disjoint recursive component is unchanged in every random world;
    // enumerate the complete truth vector of the component that was modified.
    require(snapshot(recursive) == cycleSnapshot, "pass changed the disjoint recursive component");
    for (std::uint64_t world = 0; world < original.size(); ++world) {
        require(original[world] == evaluator.evaluate(world), "safe deletion changed an acyclic world");
    }
    require(detectReadOnly(full).proofs.empty(), "mixed recursive graph did not reach the safe fixpoint");
}

void collidingNodeIdsPreservePointerIdentity() {
    Fixture f;
    Fixture otherOwner;
    const auto a = f.fact("A");
    const auto q = otherOwner.fact("IndependentQ", 0.4);
    require(a != q && a->getId() == q->getId(), "fixture did not create distinct nodes with colliding IDs");
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    const auto z = f.node("Z");
    f.edge({a}, c);
    f.edge({q}, x, 0.8);
    const auto unrelated = f.edge({c, x}, y, 0.7);
    const auto justified = f.edge({c, a}, z, 0.9);
    auto nodes = f.graph.getNodes();
    nodes.insert(q);
    WorkingSubgraphView view(nodes, f.graph.getEdges());
    const auto report = detectReadOnly(view);
    require(report.completeDerivations && !hasProof(report, unrelated, c) && hasProof(report, justified, c),
            "node-ID collision merged independent inputs or hid a real proof");
    verifyEveryProof(view, report);
    verifyActualPass(view, 1);
    require(unrelated->getInputs() == std::vector<NodePtr>{c, x} &&
                    justified->getInputs() == std::vector<NodePtr>{a},
            "colliding node IDs changed which occurrence was removed");
}

void foreignEndpointWithMatchingIdIsIncomplete() {
    Fixture f;
    Fixture otherOwner;
    const auto a = f.fact("A");
    const auto foreign = otherOwner.fact("Foreign", 0.4);
    require(a != foreign && a->getId() == foreign->getId(), "foreign endpoint lacks a colliding ID");
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    f.edge({a}, c);
    f.edge({foreign}, x, 0.8);
    f.edge({c, x}, y, 0.7);
    // Foreign is deliberately absent; a matching numeric ID cannot make its
    // independent fact source part of this supposedly complete snapshot.
    WorkingSubgraphView view(f.graph.getNodes(), f.graph.getEdges());
    const auto before = snapshot(view);
    const auto report = detectReadOnly(view);
    require(!report.completeDerivations && report.proofs.empty(), "foreign pointer was resolved by numeric ID");
    require(eliminateAndInputRedundancy(view, true).deletedInputAssociations == 0 && snapshot(view) == before,
            "mutation accepted a foreign endpoint with a matching ID");
}

void sparseAndMaximumNodeIdsPreserveEvents() {
    Fixture f;
    const auto a = f.fact("A");
    f.graph.startNodeIdsAt(std::numeric_limits<std::size_t>::max() - 4);
    const auto q = f.fact("IndependentQ", 0.4);
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    const auto z = f.node("Z");
    require(z->getId() == std::numeric_limits<std::size_t>::max(), "fixture did not reach the maximum node ID");
    f.edge({a}, c);
    f.edge({q}, x, 0.8);
    const auto unrelated = f.edge({c, x}, y, 0.7);
    const auto justified = f.edge({c, a}, z, 0.9);
    const auto report = detectReadOnly(f.graph);
    require(report.completeDerivations && !hasProof(report, unrelated, c) && hasProof(report, justified, c),
            "sparse large IDs merged events or lost a valid endpoint");
    verifyEveryProof(f.graph, report);
    verifyActualPass(f, 1);
    require(unrelated->getInputs() == std::vector<NodePtr>{c, x} &&
                    justified->getInputs() == std::vector<NodePtr>{a},
            "sparse large IDs changed which event dependency was removed");
}

void duplicateOccurrencesPreserveSurvivingDependency() {
    Fixture f;
    const auto a = f.fact("A");
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    f.edge({a}, c);
    f.edge({a}, x, 0.8);
    const auto target = f.edge({c, c, x}, y, 0.7);
    const auto stats = verifyActualPass(f, 2);
    require(stats.rounds == 1, "duplicate occurrences required a terminal round despite unchanged definitions");
    require(target->getInputs() == std::vector<NodePtr>{x} &&
                    !containsEdge(c->getOutgoingEdges(), target) && containsEdge(x->getOutgoingEdges(), target),
            "duplicate occurrences detached the surviving dependency or retained a deleted one");
}

void certificateSerialization() {
    Fixture f;
    const auto a = f.fact("A\"quoted\\line\n");
    const auto b = f.fact("B");
    const auto q = f.fact("Q");
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    const auto definition = f.edge({a, b}, c);
    const auto source1 = f.edge({a, b, q}, x, 0.8);
    const auto source2 = f.edge({a, b}, x, 0.6);
    const auto target = f.edge({c, b, x}, y, 0.7);
    // A warmed view cache must remain unchanged during detection and writing.
    f.graph.getIncomingEdges(x);
    const auto report = detectReadOnly(f.graph);
    require(report.proofs.size() == 1 && hasProof(report, target, c), "serialization fixture lost its proof");
    const auto before = snapshot(f.graph);
    std::ostringstream out;
    souffle::problog::writeAndInputRedundancyReport(out, report, "after-\"rewrite");
    require(snapshot(f.graph) == before, "certificate serialization mutated graph state or caches");
    std::string error;
    const auto json = json11::Json::parse(out.str(), error);
    require(error.empty() && json.is_object(), "certificate report is invalid JSON");
    require(json["schema"].string_value() == "and-input-redundancy-v1" &&
                    json["phase"].string_value() == "after-\"rewrite" &&
                    json["read_only"].bool_value() && json["independent_certificates"].bool_value(),
            "certificate metadata is incorrect");
    require(json["stats"]["proven_input_associations"].int_value() == 1 &&
                    json["proofs"].array_items().size() == 1,
            "serialized proof count is incorrect");
    const auto& proof = json["proofs"][0];
    require(proof["edge"]["id"].string_value() == std::to_string(target->getId()) &&
                    proof["edge"]["rule_id"].string_value() == std::to_string(target->getRuleApp().ruleId) &&
                    proof["definition"]["id"].string_value() == std::to_string(definition->getId()) &&
                    proof["edge"]["input_ids"].array_items().size() == 3 &&
                    proof["edge"]["negations"].array_items().size() == 3 &&
                    proof["edge"]["support_tokens"][0].string_value() ==
                            std::to_string(makeEdgeSupportToken(target->getId())),
            "serialized target identity/body/event metadata is incorrect");
    require(proof["witnesses"].array_items().size() == 2, "serialized premise witnesses are incomplete");
    bool sawMust = false, sawDirect = false;
    for (const auto& witness : proof["witnesses"].array_items()) {
        const auto premiseId = witness["premise"]["id"].string_value();
        if (premiseId == std::to_string(a->getId())) {
            sawMust = true;
            require(witness["premise"]["tuple"]["rel"].string_value() == a->getTuple().relation_name &&
                            witness["provider"]["id"].string_value() == std::to_string(x->getId()) &&
                            witness["kind"].string_value() == "must_1" &&
                            witness["source_edges"].array_items().size() == 2,
                    "Must_1 certificate lost escaped tuple names or alternative sources");
            std::unordered_set<std::string> sourceIds;
            for (const auto& source : witness["source_edges"].array_items()) {
                sourceIds.insert(source["id"].string_value());
                require(!source["input_ids"].array_items().empty() &&
                                source["support_tokens"].array_items().size() == 1 &&
                                source["head_id"].string_value() == std::to_string(x->getId()),
                        "serialized witness source omitted its body or event metadata");
            }
            require(sourceIds == std::unordered_set<std::string>{std::to_string(source1->getId()),
                                        std::to_string(source2->getId())},
                    "Must_1 certificate contains the wrong alternative sources");
        } else if (premiseId == std::to_string(b->getId())) {
            sawDirect = true;
            require(witness["provider"]["id"].string_value() == premiseId &&
                            witness["kind"].string_value() == "direct" &&
                            witness["source_edges"].array_items().empty(),
                    "direct-input certificate is incorrect");
        }
    }
    require(sawMust && sawDirect, "serialized report did not cover each premise");
    verifyEveryProof(f.graph, report);
}

}  // namespace

int main() {
    try {
        collectiveProofAndEventIdentity();
        randomDefinitionAndFactSources();
        alternativeDefinitionsAndWitnesses();
        absentEmptyNegativeAndAliasCases();
        recursiveCases();
        completeSnapshotsAndStaleAdjacency();
        independentProofsAreNotBatchDeletions();
        multipleDeletionsRefreshBodyAndAdjacencyCaches();
        crossEdgeCertificatesAreRevalidated();
        shortenedDefinitionEnablesAnotherDeletion();
        irrelevantDefinitionMutationCertifiesFixpoint();
        sharedProviderIntersectsEveryAlternative();
        duplicatePremisesKeepFirstCertificateAndSkipUnneededProvider();
        singleSourceThenAlternativeSourcesAllocateIntersectionWorkspace();
        repeatedPremiseDoesNotProvideItsOwnWitness();
        unrelatedRecursionDoesNotBlockSafeDeletion();
        collidingNodeIdsPreservePointerIdentity();
        foreignEndpointWithMatchingIdIsIncomplete();
        sparseAndMaximumNodeIdsPreserveEvents();
        duplicateOccurrencesPreserveSurvivingDependency();
        certificateSerialization();
        std::cout << "AND-input redundancy detector and actual world-equivalence checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
