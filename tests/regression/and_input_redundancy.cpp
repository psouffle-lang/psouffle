#include "souffle/problog/AndInputRedundancy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using souffle::problog::AndInputRedundancyReport;
using souffle::problog::detectAndInputRedundancy;
using souffle::problog::eliminateAndInputRedundancy;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Fixture {
    WorkingDerivationGraph graph;
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

void verifyActualPass(WorkingSubgraphView& view, std::size_t expectedDeletions) {
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
}

void verifyActualPass(Fixture& fixture, std::size_t expectedDeletions) {
    WorkingSubgraphView view(fixture.graph.getNodes(), fixture.graph.getEdges());
    verifyActualPass(view, expectedDeletions);
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
    verifyActualPass(f, 1);
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
    verifyActualPass(f, 1);
    require(source->getInputs() == std::vector<NodePtr>{a} &&
                    target->getInputs() == std::vector<NodePtr>{d, x},
            "cross-edge deletion reused an outdated one-layer witness");
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
    verifyActualPass(f, 2);
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
        duplicateOccurrencesPreserveSurvivingDependency();
        certificateSerialization();
        std::cout << "AND-input redundancy detector and actual world-equivalence checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
