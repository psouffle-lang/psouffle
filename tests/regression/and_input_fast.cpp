#include "souffle/problog/AndInputRedundancyFast.h"

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

using souffle::problog::AndInputRedundancyCleanupPlan;
using souffle::problog::AndInputRedundancyFreshDagCertificate;
using souffle::problog::AndInputRedundancyWorkspace;
using souffle::problog::canUseAndInputRedundancyFreshDag;
using souffle::problog::detectAndInputRedundancy;
using souffle::problog::eliminateAndInputRedundancyFreshDag;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct FixtureGraph : WorkingDerivationGraph {
    void startNodeIdsAt(std::size_t first) { nextNodeId = first; }
    void startEdgeIdsAt(std::size_t first) { nextEdgeId = first; }
    void adopt(const NodePtr& node) { nodes.insert(node); }
};

struct Fixture {
    FixtureGraph graph;
    souffle::RamDomain nextRuleId = 100;

    NodePtr node(const std::string& name) { return graph.createNode(UntypedTuple{name, {}}); }

    NodePtr fact(const std::string& name, double probability = 0.5) {
        auto result = node(name);
        result->isFact = true;
        result->setOriginalFact();
        result->setProbability(probability);
        return result;
    }

    EdgePtr edge(const std::vector<NodePtr>& inputs, const NodePtr& output, double probability = 1.0,
            const std::vector<bool>& negations = {}) {
        RuleApplication application;
        application.ruleId = nextRuleId++;
        application.varValuesPure = {5, 9};
        auto result = inputs.empty() ? graph.createHyperedge(inputs, output, application)
                                    : graph.createHyperedge(inputs, output, nullptr, negations, application);
        result->setProbability(probability);
        return result;
    }

    WorkingSubgraphView retainAll() {
        for (const auto& node : graph.getNodes()) node->setQuery();
        return graph.prune(std::vector<std::string>{});
    }
};

// Reproduce the public initial-summary contract, independently of the pass.
AndInputRedundancyWorkspace workspaceFor(const WorkingSubgraphView& view) {
    AndInputRedundancyWorkspace workspace;
    workspace.completeEndpoints = true;
    for (const auto& node : view.getNodes()) workspace.degrees.emplace(node, souffle::problog::AndInputRedundancyDegree{});
    for (const auto& edge : view.getEdges()) {
        const auto& body = edge->getInputs();
        workspace.initialInputAssociations += body.size();
        if (workspace.arityCounts.size() <= body.size()) workspace.arityCounts.resize(body.size() + 1);
        ++workspace.arityCounts[body.size()];
        if (body.size() >= 2) workspace.potentialTargets.push_back(edge);
        const auto head = workspace.degrees.find(edge->getOutput());
        if (head == workspace.degrees.end()) workspace.completeEndpoints = false;
        else ++head->second.incoming;
        for (const auto& input : body) {
            const auto degree = workspace.degrees.find(input);
            if (degree == workspace.degrees.end()) workspace.completeEndpoints = false;
            else ++degree->second.outgoing;
        }
    }
    return workspace;
}

using Truth = std::unordered_map<NodePtr, bool>;

// Fact and rule event bits remain those of the original graph, so equivalence
// cannot result from renumbering events or accidentally matching marginals.
struct Worlds {
    std::unordered_map<NodePtr, std::size_t> factBits;
    std::unordered_map<EdgePtr, std::size_t> edgeBits;
    std::vector<Truth> original;

    explicit Worlds(const DerivationGraphViewInterface& view) {
        std::size_t events = 0;
        for (const auto& node : view.getNodes()) {
            if (node->isFact && node->getProbability() > 0.0 && node->getProbability() < 1.0) factBits[node] = events++;
        }
        for (const auto& edge : view.getEdges()) {
            if (edge->getProbability() > 0.0 && edge->getProbability() < 1.0) edgeBits[edge] = events++;
        }
        require(events < 12, "fastpath world fixture unexpectedly large");
        for (std::uint64_t world = 0; world < (std::uint64_t{1} << events); ++world) original.push_back(evaluate(view, world));
    }

    Truth evaluate(const DerivationGraphViewInterface& view, std::uint64_t world) const {
        std::unordered_map<NodePtr, std::vector<EdgePtr>> incoming;
        for (const auto& edge : view.getEdges()) incoming[edge->getOutput()].push_back(edge);
        Truth values;
        std::unordered_set<NodePtr> visiting;
        std::function<bool(const NodePtr&)> visit = [&](const NodePtr& node) {
            const auto known = values.find(node);
            if (known != values.end()) return known->second;
            require(visiting.insert(node).second, "acyclic certificate fixture contains a cycle");
            bool value = node->isFact && node->getProbability() == 1.0;
            const auto factBit = factBits.find(node);
            if (factBit != factBits.end()) value = ((world >> factBit->second) & 1) != 0;
            for (const auto& edge : incoming[node]) {
                bool contribution = edge->getProbability() == 1.0;
                const auto eventBit = edgeBits.find(edge);
                if (eventBit != edgeBits.end()) contribution = ((world >> eventBit->second) & 1) != 0;
                for (std::size_t i = 0; i < edge->getInputs().size(); ++i) {
                    const bool input = visit(edge->getInputs()[i]);
                    contribution = contribution && (edge->getBodyNegations()[i] ? !input : input);
                }
                value = value || contribution;
            }
            visiting.erase(node);
            values.emplace(node, value);
            return value;
        };
        for (const auto& node : view.getNodes()) visit(node);
        return values;
    }

    void verify(const DerivationGraphViewInterface& view) const {
        for (std::uint64_t world = 0; world < original.size(); ++world) {
            const auto values = evaluate(view, world);
            for (const auto& node : view.getNodes()) require(values.at(node) == original[world].at(node),
                    "fastpath changed a retained event in a random world");
        }
    }
};

struct Events {
    std::unordered_map<NodePtr, std::string> nodes;
    std::unordered_map<EdgePtr, std::string> edges;

    explicit Events(const DerivationGraphViewInterface& view) {
        for (const auto& node : view.getNodes()) nodes.emplace(node, record(node));
        for (const auto& edge : view.getEdges()) edges.emplace(edge, record(edge));
    }

    static std::string record(const NodePtr& node) {
        std::ostringstream out;
        out.precision(17);
        out << node->getId() << ' ' << node->getSemanticFactId() << ' ' << node->getTuple().toString() << ' '
            << node->getProbability() << ' ' << node->isFact << node->isOriginalFactNode() << node->isShadow
            << node->pruned << node->needOutput << node->isQuery << node->hasEvidence() << node->getEvidenceValue();
        for (const auto token : node->getProbabilisticSupportTokens()) out << ' ' << token;
        return out.str();
    }

    static std::string record(const EdgePtr& edge) {
        std::ostringstream out;
        out.precision(17);
        out << edge->getId() << ' ' << edge->getOutput().get() << ' ' << edge->getProbability() << ' '
            << edge->getRule() << ' ' << edge->getRuleApp().ruleId << ' ' << edge->pruned;
        for (const auto value : edge->getRuleApp().varValuesPure) out << ' ' << value;
        for (const auto token : edge->getProbabilisticSupportTokens()) out << ' ' << token;
        return out.str();
    }

    void verify(const DerivationGraphViewInterface& view) const {
        for (const auto& node : view.getNodes()) require(nodes.at(node) == record(node), "fastpath changed a retained node event");
        for (const auto& edge : view.getEdges()) require(edges.at(edge) == record(edge), "fastpath changed a retained rule event or edge identity");
    }
};

void warmCaches(WorkingSubgraphView& view) {
    view.getValidNodes();
    view.getValidEdges();
    view.getCycleDependencyGraph();
    for (const auto& node : view.getNodes()) {
        view.getIncomingEdges(node);
        view.getIncomingEdgesStable(node);
        view.getOutgoingEdges(node);
    }
    for (const auto& edge : view.getEdges()) {
        edge->getInputsStable();
        edge->getBodyNegationsStable();
        edge->getEdgeKey();
        edge->hasSelfDependency();
    }
}

void verifyCaches(WorkingSubgraphView& view) {
    require(view.getValidNodes().size() == view.getNodes().size() &&
                    view.getValidEdges().size() == view.getEdges().size(), "fastpath retained stale validity caches");
    for (const auto& edge : view.getEdges()) {
        require(edge->getInputs().size() == edge->getBodyNegations().size() &&
                        edge->getInputsStable().size() == edge->getInputs().size() &&
                        edge->getBodyNegationsStable().size() == edge->getInputs().size(),
                "fastpath retained stale/alignment-invalid body caches");
    }
    for (const auto& node : view.getNodes()) {
        for (const auto& edge : view.getOutgoingEdges(node)) {
            require(view.getEdges().count(edge) != 0 &&
                            std::find(edge->getInputs().begin(), edge->getInputs().end(), node) != edge->getInputs().end(),
                    "fastpath retained an erased outgoing dependency");
        }
    }
    require(view.getCycleDependencyGraph().nodeToCycleIndex.size() == view.getNodes().size(),
            "fastpath retained a stale SCC cache");
}

struct Shape {
    std::size_t associations = 0, disjunctions = 0;
    std::size_t maxIncoming = 0, maxOutgoing = 0, maxBody = 0;
};

Shape shapeOf(const DerivationGraphViewInterface& view) {
    Shape shape;
    std::unordered_map<NodePtr, std::size_t> incoming, outgoing;
    for (const auto& edge : view.getEdges()) {
        ++incoming[edge->getOutput()];
        shape.associations += edge->getInputs().size();
        shape.maxBody = std::max(shape.maxBody, edge->getInputs().size());
        for (const auto& input : edge->getInputs()) ++outgoing[input];
    }
    for (const auto& node : view.getNodes()) {
        const auto in = incoming[node];
        if ((!node->isFact && in > 1) || (node->isFact && in > 0)) ++shape.disjunctions;
        shape.maxIncoming = std::max(shape.maxIncoming, in);
        shape.maxOutgoing = std::max(shape.maxOutgoing, outgoing[node]);
    }
    return shape;
}

void verifySummary(const AndInputRedundancyCleanupPlan& cleanup, const Shape& before,
        const DerivationGraphViewInterface& after) {
    const auto final = shapeOf(after);
    require(cleanup.hasSummary && cleanup.finalInputAssociations == final.associations &&
                    cleanup.removedDisjunctionNodes + final.disjunctions == before.disjunctions &&
                    cleanup.maxInDegree == final.maxIncoming && cleanup.maxOutDegree == final.maxOutgoing &&
                    cleanup.maxHyperedgeInputs == final.maxBody,
            "fastpath cleanup summary differs from an independent graph-shape scan");
}

void verifyFast(WorkingSubgraphView& view, std::size_t expectedDeletions,
        AndInputRedundancyCleanupPlan* cleanup = nullptr) {
    const Worlds worlds(view);  // Also independently verifies that the certified fixture is a DAG.
    const Events events(view);
    auto workspace = workspaceFor(view);
    const auto count = workspace.initialInputAssociations;
    // Run the existing implementation on an independent graph with the same
    // edge ordering. This checks proof opportunities as well as event equality.
    FixtureGraph genericGraph;
    std::unordered_map<NodePtr, NodePtr> copiedNodes;
    std::unordered_map<EdgePtr, EdgePtr> copiedEdges;
    auto nodes = std::vector<NodePtr>(view.getNodes().begin(), view.getNodes().end());
    auto edges = std::vector<EdgePtr>(view.getEdges().begin(), view.getEdges().end());
    auto order = [](const auto& left, const auto& right) {
        if (left->getId() != right->getId()) return left->getId() < right->getId();
        return std::less<const void*>{}(left.get(), right.get());
    };
    std::sort(nodes.begin(), nodes.end(), order);
    std::sort(edges.begin(), edges.end(), order);
    for (const auto& node : nodes) {
        genericGraph.startNodeIdsAt(node->getId());
        const auto copied = genericGraph.createNode(node->getTuple());
        copied->isFact = node->isFact;
        copied->setOriginalFact(node->isOriginalFactNode());
        copied->setProbability(node->getProbability());
        copied->setSemanticFactId(node->getSemanticFactId());
        copied->setProbabilisticSupportTokens(node->getProbabilisticSupportTokens());
        copied->isShadow = node->isShadow;
        copied->needOutput = node->needOutput;
        copied->isQuery = node->isQuery;
        if (node->hasEvidence()) copied->setEvidence(node->getEvidenceValue());
        copiedNodes.emplace(node, copied);
    }
    for (const auto& edge : edges) {
        std::vector<NodePtr> body;
        for (const auto& input : edge->getInputs()) body.push_back(copiedNodes.at(input));
        genericGraph.startEdgeIdsAt(edge->getId());
        const auto copied = body.empty()
                ? genericGraph.createHyperedge(body, copiedNodes.at(edge->getOutput()), edge->getRuleApp())
                : genericGraph.createHyperedge(body, copiedNodes.at(edge->getOutput()), edge->getRule(),
                          edge->getBodyNegations(), edge->getRuleApp());
        copied->setProbability(edge->getProbability());
        copied->setProbabilisticSupportTokens(edge->getProbabilisticSupportTokens());
        copiedEdges.emplace(edge, copied);
    }
    WorkingSubgraphView genericView(genericGraph.getNodes(), genericGraph.getEdges());
    const auto genericStats = souffle::problog::eliminateAndInputRedundancy(genericView, true);
    const AndInputRedundancyFreshDagCertificate certificate{true, true, true};
    require(canUseAndInputRedundancyFreshDag(view, workspace, certificate), "valid fastpath fixture rejected its workspace");
    warmCaches(view);
    const auto stats = eliminateAndInputRedundancyFreshDag(view, workspace, certificate, cleanup);
    require(stats.deletedInputAssociations == expectedDeletions && stats.initialInputAssociations == count &&
                    stats.finalInputAssociations + stats.deletedInputAssociations == count,
            "fastpath expected " + std::to_string(expectedDeletions) + " deletions, got " +
                    std::to_string(stats.deletedInputAssociations) + "; initial=" +
                    std::to_string(stats.initialInputAssociations) + ", expected initial=" +
                    std::to_string(count) + ", final=" + std::to_string(stats.finalInputAssociations));
    require(stats.remainingInputAssociations == 0 && detectAndInputRedundancy(view, true).proofs.empty(),
            "fastpath stopped before reaching the certified local fixed point");
    require(stats.deletedInputAssociations == genericStats.deletedInputAssociations &&
                    stats.affectedEdges == genericStats.affectedEdges,
            "fastpath opportunities differ from the existing generic implementation");
    for (const auto& edge : view.getEdges()) {
        std::vector<NodePtr> expectedBody;
        for (const auto& input : edge->getInputs()) expectedBody.push_back(copiedNodes.at(input));
        require(copiedEdges.at(edge)->getInputs() == expectedBody &&
                        copiedEdges.at(edge)->getBodyNegations() == edge->getBodyNegations(),
                "fastpath final body differs from the existing generic implementation");
    }
    events.verify(view);
    worlds.verify(view);
    verifyCaches(view);
    require(!workspace.completeEndpoints && !canUseAndInputRedundancyFreshDag(view, workspace, certificate),
            "consumed workspace was accepted for a second fastpath invocation");
    const auto repeated = eliminateAndInputRedundancyFreshDag(view, workspace, certificate);
    require(repeated.deletedInputAssociations == 0 && !workspace.completeEndpoints,
            "consumed-workspace generic fallback was not idempotent");
    events.verify(view);
    worlds.verify(view);
}

void collectiveAndRepeatedProofs() {
    for (bool repeated : {false, true}) {
        Fixture f;
        const auto a = f.fact("A");
        const auto b = f.fact("B", 0.4);
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto z = f.node("Z");
        const auto y = f.node("Y");
        f.edge({a, b}, c);
        f.edge({a}, x, 0.7);
        f.edge({b}, z, 0.8);
        const auto target = f.edge(repeated ? std::vector<NodePtr>{c, c, x, z} : std::vector<NodePtr>{c, x, z}, y, 0.6);
        target->setProbabilisticSupportTokens({1234});
        const auto originalKey = target->getEdgeKey();
        auto view = f.retainAll();
        verifyFast(view, repeated ? 2 : 1);
        require(target->getInputs() == std::vector<NodePtr>{x, z} && target->getEdgeKey() != originalKey,
                "collective proof changed surviving input order or retained an old edge key");
        require(std::find(c->getOutgoingEdges().begin(), c->getOutgoingEdges().end(), target) == c->getOutgoingEdges().end(),
                "last repeated occurrence did not detach raw adjacency");
    }
}

void duplicateCandidateWithPrivatePremise() {
    Fixture f;
    const auto a = f.fact("PrivateA");
    const auto q = f.fact("IndependentQ", 0.4);
    const auto c = f.node("C");
    const auto y = f.node("Y");
    f.edge({a}, c);
    const auto target = f.edge({c, c, q}, y, 0.7);
    auto view = f.retainAll();
    require(workspaceFor(view).degrees.at(a).outgoing == 1,
            "duplicate-candidate fixture has no private singleton premise");
    verifyFast(view, 1);
    require(target->getInputs() == std::vector<NodePtr>{c, q},
            "private premise rejection removed both candidates or refused a duplicate");
}

void refusalCases() {
    for (int mode = 0; mode < 9; ++mode) {
        Fixture f;
        const auto a = f.fact("A");
        const auto b = f.fact("B", 0.3);
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        const auto definition = f.edge(mode == 7 ? std::vector<NodePtr>{} : std::vector<NodePtr>{a}, c,
                mode == 0 ? std::nextafter(1.0, 0.0) : 1.0,
                mode == 4 ? std::vector<bool>{true} : std::vector<bool>{});
        if (mode == 1) c->setOriginalFact();
        if (mode == 2) { c->isFact = true; c->setProbability(0.4); }
        if (mode == 3) f.edge({b}, c);
        if (mode == 6) a->isShadow = true;
        if (mode == 8) definition->setProbabilisticSupportTokens({4321});
        f.edge({a}, x, 0.8);
        f.edge({c, x}, y, 0.7, mode == 5 ? std::vector<bool>{true, false} : std::vector<bool>{});
        auto view = f.retainAll();
        verifyFast(view, 0);
    }
    Fixture f;
    const auto a = f.fact("A");
    const auto q = f.fact("IndependentQ", 0.4);
    const auto c = f.node("C");
    const auto y = f.node("Y");
    f.edge({a, a}, c);
    f.edge({c, q}, y, 0.7);
    auto view = f.retainAll();
    verifyFast(view, 0);
}

void alternativesIntersectAllSources() {
    for (int alternative = 0; alternative != 3; ++alternative) {
        Fixture f;
        const auto a = f.fact("A");
        const auto b = f.fact("B");
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        f.edge({a}, c);
        f.edge({a, b}, x, 0.8);
        f.edge({alternative == 1 ? b : a}, x, 0.6,
                alternative == 2 ? std::vector<bool>{true} : std::vector<bool>{});
        f.edge({c, x}, y, 0.7);
        auto view = f.retainAll();
        verifyFast(view, alternative == 0 ? 1 : 0);
    }
}

void currentBodiesAndFixedPoint() {
    for (int mode = 0; mode != 3; ++mode) {
        Fixture f;
        const auto a = f.fact("A");
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        f.edge({a}, c);
        if (mode == 0) {
            f.edge({a}, x);
            const auto target = f.edge({c, x}, y, 0.7);
            auto view = f.retainAll();
            verifyFast(view, 1);
            require(target->getInputs().size() == 1, "symmetric certificates were blindly batch-deleted");
        } else {
            const auto d = f.node("D");
            if (mode == 1) {
                f.edge({c}, d);
                const auto source = f.edge({c, a}, x, 0.8);
                const auto target = f.edge({d, x}, y, 0.7);
                auto view = f.retainAll();
                verifyFast(view, 1);
                require(source->getInputs() == std::vector<NodePtr>{a} && target->getInputs() == std::vector<NodePtr>{d, x},
                        "later deletion reused a stale Must_1 certificate");
            } else {
                const auto target = f.edge({d, x}, y, 0.7);
                f.edge({a}, x, 0.8);
                const auto definition = f.edge({a, c}, d);
                auto view = f.retainAll();
                require(workspaceFor(view).degrees.at(c).outgoing == 1 && definition->getInputs().back() == c,
                        "definition-shrinking fixture lacks a private last premise");
                verifyFast(view, 2);
                require(target->getInputs() == std::vector<NodePtr>{x} && definition->getInputs() == std::vector<NodePtr>{a},
                        "definition shrinking did not revisit an earlier target");
            }
        }
    }
}

void cleanupPreservesQueriesEvidenceAndActiveSources() {
    for (int protection = 0; protection != 4; ++protection) {
        Fixture f;
        const auto a = f.fact("A");
        const auto unused = f.node("InactiveConsumer");
        const auto c = f.node("C");
        const auto d = f.node("D");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        f.edge({a}, d);
        f.edge({d}, c);
        // This earlier target uses X's original mandatory D. X then drops D;
        // after both mutations C's removed definition can orphan D.
        f.edge({c, c, x}, y, 0.7);
        f.edge({d, a}, x, 0.8);
        const auto inactive = f.edge({c}, unused, 0.6);
        if (protection == 1) d->setQuery();
        if (protection == 2) d->setEvidence(true);
        if (protection == 3) d->setEvidence(false);
        auto view = f.graph.prune(std::vector<std::string>{"Y"});
        const Worlds worlds(view);
        const Events events(view);
        const auto originalShape = shapeOf(view);
        AndInputRedundancyCleanupPlan cleanup;
        verifyFast(view, 3, &cleanup);
        require(!cleanup.requiresFullPrune && cleanup.hasSummary, "DAG cleanup requested a full prune");
        require(cleanup.nodes.size() == (protection == 0 ? 2 : 1) && cleanup.edges.size() == (protection == 0 ? 2 : 1),
                "DAG cleanup did not preserve query/evidence roots or cascade unused definitions");
        view.applyPruning(cleanup.nodes, cleanup.edges);
        require(view.getNodes().count(y) != 0 && view.getNodes().count(c) == 0 &&
                        view.getNodes().count(d) == (protection == 0 ? 0 : 1),
                "cleanup removed a protected query/evidence event");
        require(view.getEdges().count(inactive) == 0 && inactive->pruned, "cleanup revived an inactive source");
        worlds.verify(view);
        events.verify(view);
        verifyCaches(view);
        verifySummary(cleanup, originalShape, view);
        const auto full = f.graph.prune(std::vector<std::string>{"Y"});
        require(view.getNodes() == full.getNodes() && view.getEdges() == full.getEdges(),
                "local DAG cleanup differs from the full backward-prune oracle");
    }
}

void foreignEndpointCannotBorrowMatchingId() {
    Fixture f;
    Fixture other;
    const auto a = f.fact("A");
    const auto foreign = other.fact("IndependentQ", 0.4);
    require(a != foreign && a->getId() == foreign->getId(), "foreign endpoint fixture lacks colliding numeric IDs");
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    f.edge({a}, c);
    f.edge({foreign}, x, 0.8);
    const auto target = f.edge({c, x}, y, 0.7);
    WorkingSubgraphView view(f.graph.getNodes(), f.graph.getEdges());
    auto workspace = workspaceFor(view);
    const AndInputRedundancyFreshDagCertificate certificate{true, true, true};
    require(!workspace.completeEndpoints && !canUseAndInputRedundancyFreshDag(view, workspace, certificate),
            "numeric-ID collision accepted a foreign pointer as a complete endpoint");
    const Events events(view);
    const auto stats = eliminateAndInputRedundancyFreshDag(view, workspace, certificate);
    require(stats.deletedInputAssociations == 0 && target->getInputs() == std::vector<NodePtr>{c, x},
            "incomplete endpoint fallback mutated an unrelated event");
    events.verify(view);
}

void untrustedCertificateFallsBack() {
    for (int mode = 0; mode != 4; ++mode) {
        Fixture f;
        const auto a = f.fact("A");
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        f.edge({a}, c);
        f.edge({a}, x, 0.8);
        f.edge({c, x}, y, 0.7);
        auto view = f.retainAll();
        auto workspace = workspaceFor(view);
        AndInputRedundancyFreshDagCertificate certificate{true, true, true};
        if (mode == 0) certificate.completeDerivations = false;
        if (mode == 1) certificate.compilerAttestedDag = false;
        if (mode == 2) certificate.originalGraph = false;
        if (mode == 3) workspace.completeEndpoints = false;
        require(!canUseAndInputRedundancyFreshDag(view, workspace, certificate), "untrusted certificate enabled fastpath");
        const Worlds worlds(view);
        const Events events(view);
        const auto stats = eliminateAndInputRedundancyFreshDag(view, workspace, certificate);
        require(stats.deletedInputAssociations == (mode == 0 ? 0 : 1), "fallback did not respect source completeness");
        require(!workspace.completeEndpoints, "fallback retained a reusable stale workspace");
        // An incomplete call leaves the graph intact, so a fresh complete
        // analysis may still discover the original opportunity through fallback.
        const auto repeated = eliminateAndInputRedundancyFreshDag(
                view, workspace, AndInputRedundancyFreshDagCertificate{true, true, true});
        require(repeated.deletedInputAssociations == (mode == 0 ? 1 : 0),
                "consumed fallback workspace did not reanalyze the current graph");
        worlds.verify(view);
        events.verify(view);
    }
    Fixture cyclic;
    const auto a = cyclic.fact("A");
    const auto c = cyclic.node("C");
    const auto x = cyclic.node("X");
    const auto y = cyclic.node("Y");
    cyclic.edge({a}, c);
    cyclic.edge({a}, x, 0.8);
    const auto target = cyclic.edge({c, x}, y);
    cyclic.edge({y}, a);
    WorkingSubgraphView view(cyclic.graph.getNodes(), cyclic.graph.getEdges());
    auto workspace = workspaceFor(view);
    const AndInputRedundancyFreshDagCertificate certificate{true, false, true};
    const auto stats = eliminateAndInputRedundancyFreshDag(view, workspace, certificate);
    require(stats.deletedInputAssociations == 0 && target->getInputs() == std::vector<NodePtr>{c, x},
            "uncertified cyclic graph bypassed the generic cycle guard");
}

}  // namespace

int main() {
    const char* current = "collectiveAndRepeatedProofs";
    try {
        collectiveAndRepeatedProofs();
        current = "duplicateCandidateWithPrivatePremise";
        duplicateCandidateWithPrivatePremise();
        current = "refusalCases";
        refusalCases();
        current = "alternativesIntersectAllSources";
        alternativesIntersectAllSources();
        current = "currentBodiesAndFixedPoint";
        currentBodiesAndFixedPoint();
        current = "cleanupPreservesQueriesEvidenceAndActiveSources";
        cleanupPreservesQueriesEvidenceAndActiveSources();
        current = "foreignEndpointCannotBorrowMatchingId";
        foreignEndpointCannotBorrowMatchingId();
        current = "untrustedCertificateFallsBack";
        untrustedCertificateFallsBack();
    } catch (const std::exception& error) {
        std::cerr << current << ": " << error.what() << '\n';
        return 1;
    }
    return 0;
}
