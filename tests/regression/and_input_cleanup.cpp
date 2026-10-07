#include "souffle/problog/AndInputRedundancy.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using souffle::problog::AndInputRedundancyCleanupPlan;
using souffle::problog::eliminateAndInputRedundancy;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct FixtureGraph : WorkingDerivationGraph {
    void startNodeIdsAt(std::size_t first) { nextNodeId = first; }
    void adopt(const NodePtr& node) { nodes.insert(node); }
};

struct Fixture {
    FixtureGraph graph;
    souffle::RamDomain nextRuleId = 100;

    NodePtr node(const std::string& name) { return graph.createNode(UntypedTuple{name, {}}); }

    NodePtr fact(const std::string& name, double probability = 0.5) {
        const auto result = node(name);
        result->isFact = true;
        result->setOriginalFact();
        result->setProbability(probability);
        return result;
    }

    EdgePtr edge(const std::vector<NodePtr>& inputs, const NodePtr& output, double probability = 1.0) {
        RuleApplication application;
        application.ruleId = nextRuleId++;
        const auto result = graph.createHyperedge(inputs, output, nullptr, {}, application);
        result->setProbability(probability);
        return result;
    }
};

using Truth = std::unordered_map<NodePtr, bool>;

// The original random-event assignment stays fixed while bodies and view
// membership change. Positive fixed-point evaluation also covers cycle cases.
struct Worlds {
    std::unordered_map<NodePtr, std::size_t> factBits;
    std::unordered_map<EdgePtr, std::size_t> edgeBits;
    std::vector<Truth> original;

    explicit Worlds(const DerivationGraphViewInterface& view) {
        std::size_t randomEvents = 0;
        for (const auto& node : view.getNodes()) {
            if (node->isFact && node->getProbability() > 0.0 && node->getProbability() < 1.0) {
                factBits.emplace(node, randomEvents++);
            }
        }
        for (const auto& edge : view.getEdges()) {
            if (edge->getProbability() > 0.0 && edge->getProbability() < 1.0) {
                edgeBits.emplace(edge, randomEvents++);
            }
        }
        require(randomEvents < 12, "cleanup world fixture unexpectedly large");
        for (std::uint64_t world = 0; world < (std::uint64_t{1} << randomEvents); ++world) {
            original.push_back(evaluate(view, world));
        }
    }

    Truth evaluate(const DerivationGraphViewInterface& view, std::uint64_t world) const {
        Truth values;
        for (const auto& node : view.getNodes()) {
            bool value = node->isFact && node->getProbability() == 1.0;
            const auto bit = factBits.find(node);
            if (bit != factBits.end()) value = ((world >> bit->second) & 1) != 0;
            values.emplace(node, value);
        }
        for (std::size_t iteration = 0; iteration <= view.getNodes().size(); ++iteration) {
            bool changed = false;
            for (const auto& edge : view.getEdges()) {
                bool value = edge->getProbability() == 1.0;
                const auto bit = edgeBits.find(edge);
                if (bit != edgeBits.end()) value = ((world >> bit->second) & 1) != 0;
                for (const auto& input : edge->getInputs()) value = value && values.at(input);
                if (value && !values.at(edge->getOutput())) {
                    values.at(edge->getOutput()) = true;
                    changed = true;
                }
            }
            if (!changed) return values;
        }
        throw std::runtime_error("positive world evaluation did not converge");
    }

    void verify(const DerivationGraphViewInterface& view) const {
        for (std::uint64_t world = 0; world < original.size(); ++world) {
            const auto values = evaluate(view, world);
            for (const auto& node : view.getNodes()) {
                require(values.at(node) == original[world].at(node),
                        "cleanup changed a retained event in a random world");
            }
        }
    }
};

struct PrunedFlags {
    std::unordered_map<NodePtr, bool> nodes;
    std::unordered_map<EdgePtr, bool> edges;

    explicit PrunedFlags(const FixtureGraph& graph) {
        for (const auto& node : graph.getNodes()) nodes.emplace(node, node->pruned);
        for (const auto& edge : graph.getEdges()) edges.emplace(edge, edge->pruned);
    }

    void verifyUnchanged() const {
        for (const auto& [node, flag] : nodes) require(node->pruned == flag, "plan preparation marked a node");
        for (const auto& [edge, flag] : edges) require(edge->pruned == flag, "plan preparation marked an edge");
    }
};

void warmCaches(WorkingSubgraphView& view) {
    view.getValidNodes();
    view.getValidEdges();
    for (const auto& node : view.getNodes()) {
        view.getIncomingEdges(node);
        view.getOutgoingEdges(node);
        view.getIncomingEdgesStable(node);
    }
    view.getCycleDependencyGraph();
}

void verifyCaches(WorkingSubgraphView& view) {
    require(view.getValidNodes().size() == view.getNodes().size(), "valid-node cache retained a removed node");
    require(view.getValidEdges().size() == view.getEdges().size(), "valid-edge cache retained a removed edge");
    for (const auto& node : view.getNodes()) {
        for (const auto& edge : view.getIncomingEdges(node)) {
            require(view.getEdges().count(edge) != 0 && edge->getOutput() == node,
                    "incoming cache retained an inactive edge");
        }
        for (const auto& edge : view.getOutgoingEdges(node)) {
            require(view.getEdges().count(edge) != 0 &&
                            std::find(edge->getInputs().begin(), edge->getInputs().end(), node) !=
                                    edge->getInputs().end(),
                    "outgoing cache retained an erased dependency");
        }
    }
    const auto& cycles = view.getCycleDependencyGraph();
    require(cycles.nodeToCycleIndex.size() == view.getNodes().size(), "SCC cache retained removed nodes");
    for (const auto& [node, index] : cycles.nodeToCycleIndex) {
        (void)index;
        require(view.getNodes().count(node) != 0, "SCC cache references a removed node");
    }
}

struct GraphShape {
    std::size_t associations = 0, disjunctions = 0;
    std::size_t maxIncoming = 0, maxOutgoing = 0, maxBody = 0;
};

GraphShape graphShape(const DerivationGraphViewInterface& view) {
    GraphShape shape;
    std::unordered_map<NodePtr, std::size_t> incoming, outgoing;
    for (const auto& edge : view.getEdges()) {
        ++incoming[edge->getOutput()];
        const auto& body = edge->getInputs();
        shape.associations += body.size();
        shape.maxBody = std::max(shape.maxBody, body.size());
        for (const auto& input : body) ++outgoing[input];
    }
    for (const auto& node : view.getNodes()) {
        const auto in = incoming[node];
        if ((!node->isFact && in > 1) || (node->isFact && in > 0)) ++shape.disjunctions;
        shape.maxIncoming = std::max(shape.maxIncoming, in);
        shape.maxOutgoing = std::max(shape.maxOutgoing, outgoing[node]);
    }
    return shape;
}

void compareCleanup(Fixture& fixture, WorkingSubgraphView& view, const std::vector<std::string>& outputs,
        std::size_t expectedDeletions, const std::unordered_set<NodePtr>& expectedNodes,
        const std::unordered_set<EdgePtr>& expectedEdges, bool checkCaches = false) {
    const Worlds worlds(view);
    const auto originalNodes = view.getNodes();
    const auto originalEdges = view.getEdges();
    const auto evidence = view.getEvidenceNodes();
    const auto beforeShape = graphShape(view);
    const PrunedFlags flags(fixture.graph);
    AndInputRedundancyCleanupPlan plan;
    const auto stats = eliminateAndInputRedundancy(view, true, &plan);
    require(stats.deletedInputAssociations == expectedDeletions, "unexpected deletion count in cleanup fixture");
    require(!plan.requiresFullPrune, "safe cleanup fixture unnecessarily required full pruning");
    require(view.getNodes() == originalNodes && view.getEdges() == originalEdges,
            "plan preparation changed view membership");
    flags.verifyUnchanged();
    const std::unordered_set<NodePtr> proposedNodes(plan.nodes.begin(), plan.nodes.end());
    const std::unordered_set<EdgePtr> proposedEdges(plan.edges.begin(), plan.edges.end());
    require(proposedNodes.size() == plan.nodes.size() && proposedEdges.size() == plan.edges.size(),
            "cleanup plan contains duplicate removals");
    require(proposedNodes == expectedNodes && proposedEdges == expectedEdges, "cleanup plan removed the wrong objects");
    if (checkCaches) warmCaches(view);
    view.applyPruning(plan.nodes, plan.edges);
    for (const auto& node : originalNodes) {
        require(view.getNodes().count(node) == (proposedNodes.count(node) == 0), "node plan was not applied exactly");
        require(node->pruned == (proposedNodes.count(node) != 0), "node pruning flag disagrees with active view");
        if (node->isQueryNode() || node->hasEvidence()) {
            require(view.getNodes().count(node) != 0, "cleanup removed a query/evidence root");
        }
    }
    for (const auto& edge : originalEdges) {
        require(view.getEdges().count(edge) == (proposedEdges.count(edge) == 0), "edge plan was not applied exactly");
        require(edge->pruned == (proposedEdges.count(edge) != 0), "edge pruning flag disagrees with active view");
    }
    require(view.getEvidenceNodes() == evidence, "cleanup changed the evidence-root list");
    if (checkCaches) verifyCaches(view);
    worlds.verify(view);

    // Ordinary pruning is the independent oracle. Raw graph ownership and
    // adjacency remain available after applying the local view-only plan.
    const auto ordinary = fixture.graph.prune(outputs);
    require(view.getNodes() == ordinary.getNodes() && view.getEdges() == ordinary.getEdges(),
            "local cleanup differs from query/evidence-aware full pruning");
    if (expectedDeletions != 0) {
        const auto afterShape = graphShape(ordinary);
        require(plan.hasSummary && plan.finalInputAssociations == afterShape.associations &&
                        plan.removedDisjunctionNodes == beforeShape.disjunctions - afterShape.disjunctions &&
                        plan.maxInDegree == afterShape.maxIncoming && plan.maxOutDegree == afterShape.maxOutgoing &&
                        plan.maxHyperedgeInputs == afterShape.maxBody,
                "indexed cleanup summary differs from ordinary pruned graph");
    }
    worlds.verify(ordinary);
}

void deadDagCascade() {
    Fixture f;
    const auto a = f.fact("A");
    const auto d = f.node("D");
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    const auto definitionD = f.edge({a}, d);
    const auto definitionC = f.edge({d}, c);
    // Y is considered before X. Its proof uses X's original mandatory D,
    // then X drops D using A. Removing C's definition finally orphans D.
    const auto target = f.edge({c, x}, y, 0.7);
    const auto source = f.edge({d, a}, x, 0.8);
    const std::vector<std::string> outputs{"Y"};
    auto view = f.graph.prune(outputs);
    compareCleanup(f, view, outputs, 2, {c, d}, {definitionC, definitionD}, true);
    require(target->getInputs() == std::vector<NodePtr>{x} && source->getInputs() == std::vector<NodePtr>{a},
            "cascade fixture did not preserve the surviving bodies");
}

void queryAndEvidenceAreRetained() {
    for (bool evidenceValue : {false, true}) {
        Fixture f;
        const auto a = f.fact("A");
        const auto c = f.node("C");
        const auto e = f.node("Evidence");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        f.edge({a}, c);
        f.edge({a}, e);
        f.edge({a}, x, 0.8);
        f.edge({c, e, x}, y, 0.7);
        e->setEvidence(evidenceValue);
        const std::vector<std::string> outputs{"Y", "C"};
        auto view = f.graph.prune(outputs);
        compareCleanup(f, view, outputs, 2, {}, {}, true);
        require(view.getNodes().count(c) && view.getNodes().count(e), "pinned redundant nodes disappeared");
    }
}

void anotherConsumerKeepsTheDefinition() {
    Fixture f;
    const auto a = f.fact("A");
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    const auto z = f.node("OtherQuery");
    f.edge({a}, c);
    f.edge({a}, x, 0.8);
    f.edge({c, x}, y, 0.7);
    const auto other = f.edge({c}, z, 0.6);
    const std::vector<std::string> outputs{"Y", "OtherQuery"};
    auto view = f.graph.prune(outputs);
    compareCleanup(f, view, outputs, 1, {}, {});
    require(view.getEdges().count(other) && view.getNodes().count(c), "cleanup ignored another consumer");
}

void duplicateOccurrences() {
    {
        Fixture f;
        const auto a = f.fact("A");
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        const auto definition = f.edge({a, a}, c);
        f.edge({a}, x, 0.8);
        const auto target = f.edge({c, c, x}, y, 0.7);
        const std::vector<std::string> outputs{"Y"};
        auto view = f.graph.prune(outputs);
        compareCleanup(f, view, outputs, 2, {c}, {definition}, true);
        require(target->getInputs() == std::vector<NodePtr>{x} && view.getNodes().count(a),
                "duplicate accounting removed a surviving premise");
    }
    {
        Fixture f;
        const auto a = f.fact("A");
        const auto q = f.fact("IndependentQ", 0.4);
        const auto c = f.node("C");
        const auto y = f.node("Y");
        f.edge({a}, c);
        const auto target = f.edge({c, c, q}, y, 0.7);
        const std::vector<std::string> outputs{"Y"};
        auto view = f.graph.prune(outputs);
        compareCleanup(f, view, outputs, 1, {}, {});
        require(target->getInputs() == std::vector<NodePtr>({c, q}) && view.getNodes().count(c),
                "cleanup dropped the last surviving duplicate dependency");
    }
}

void inactiveRawAdjacencyDoesNotBlockCleanup() {
    Fixture f;
    const auto a = f.fact("A");
    const auto b = f.fact("InactiveFact", 0.4);
    const auto c = f.node("C");
    const auto x = f.node("X");
    const auto y = f.node("Y");
    const auto unused = f.node("UnusedHead");
    const auto definition = f.edge({a}, c);
    f.edge({a}, x, 0.8);
    f.edge({c, x}, y, 0.7);
    const std::vector<std::string> outputs{"Y"};
    auto view = f.graph.prune(outputs);
    const auto inactiveSource = f.edge({b}, c, 0.6);
    const auto inactiveConsumer = f.edge({c}, unused, 0.3);
    inactiveSource->pruned = true;
    inactiveConsumer->pruned = true;
    require(!view.getEdges().count(inactiveSource) && !view.getEdges().count(inactiveConsumer),
            "inactive-adjacency fixture accidentally changed view membership");
    compareCleanup(f, view, outputs, 1, {c}, {definition}, true);
    require(inactiveSource->pruned && inactiveConsumer->pruned && b->pruned && unused->pruned,
            "cleanup resurrected inactive raw provenance");
    require(std::find(c->getIncomingEdges().begin(), c->getIncomingEdges().end(), inactiveSource) !=
                            c->getIncomingEdges().end() &&
                    std::find(c->getOutgoingEdges().begin(), c->getOutgoingEdges().end(), inactiveConsumer) !=
                            c->getOutgoingEdges().end(),
            "view cleanup physically erased owning raw adjacency");
}

void recursiveStructuresRemainConservative() {
    {
        Fixture f;
        const auto r = f.fact("Seed");
        const auto u = f.node("U");
        const auto v = f.node("V");
        const auto c = f.node("C");
        const auto y = f.node("Y");
        f.edge({r}, u, 0.4);
        f.edge({v}, u);
        f.edge({u}, v);
        f.edge({u}, c);
        const auto target = f.edge({c, u}, y, 0.7);
        const std::vector<std::string> outputs{"Y"};
        auto view = f.graph.prune(outputs);
        compareCleanup(f, view, outputs, 0, {}, {});
        require(target->getInputs() == std::vector<NodePtr>({c, u}), "recursive rejection changed a body");
    }
    {
        Fixture f;
        const auto r = f.fact("Seed");
        const auto u = f.node("U");
        const auto v = f.node("V");
        f.edge({r}, u, 0.4);
        f.edge({v}, u);
        f.edge({u}, v);
        const auto a = f.fact("A");
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        const auto definition = f.edge({a}, c);
        f.edge({a}, x, 0.8);
        f.edge({c, x}, y, 0.7);
        const std::vector<std::string> outputs{"Y", "V"};
        auto view = f.graph.prune(outputs);
        compareCleanup(f, view, outputs, 1, {c}, {definition}, true);
        require(view.getNodes().count(u) && view.getNodes().count(v), "local cleanup lost unrelated recursion");
    }
}

void sparseAndCollidingNodeIds() {
    {
        Fixture f;
        const auto a = f.fact("A");
        f.graph.startNodeIdsAt(std::numeric_limits<std::size_t>::max() - 2);
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        const auto definition = f.edge({a}, c);
        f.edge({a}, x, 0.8);
        f.edge({c, x}, y, 0.7);
        const std::vector<std::string> outputs{"Y"};
        auto view = f.graph.prune(outputs);
        compareCleanup(f, view, outputs, 1, {c}, {definition});
    }
    {
        Fixture f;
        Fixture other;
        const auto a = f.fact("A");
        const auto q = other.fact("IndependentQ", 0.4);
        require(a != q && a->getId() == q->getId(), "fixture lacks colliding pointer IDs");
        f.graph.adopt(q);
        const auto c = f.node("C");
        const auto x = f.node("X");
        const auto y = f.node("Y");
        const auto z = f.node("Z");
        f.edge({a}, c);
        f.edge({q}, x, 0.8);
        const auto independent = f.edge({c, x}, y, 0.7);
        f.edge({c, a}, z, 0.9);
        const std::vector<std::string> outputs{"Y", "Z"};
        auto view = f.graph.prune(outputs);
        compareCleanup(f, view, outputs, 1, {}, {});
        require(independent->getInputs() == std::vector<NodePtr>({c, x}) && view.getNodes().count(q),
                "colliding IDs confused independent events or live consumers");
    }
}

void zeroHitDoesNotProposeCleanup() {
    Fixture f;
    const auto a = f.fact("A");
    const auto c = f.node("RandomDefinition");
    const auto y = f.node("Y");
    f.edge({a}, c, 0.6);
    const auto target = f.edge({c, a}, y, 0.7);
    const std::vector<std::string> outputs{"Y"};
    auto view = f.graph.prune(outputs);
    compareCleanup(f, view, outputs, 0, {}, {});
    require(target->getInputs() == std::vector<NodePtr>({c, a}), "zero-hit cleanup modified a body");
}

void biImpMergeReturnsLiveEvidenceRoots() {
    DerivationGraph::setMergeBiImpEnabled(true);
    for (bool evidenceValue : {false, true}) {
        Fixture f;
        const auto seed = f.fact("Seed");
        const auto representative = f.node("Representative");
        const auto observed = f.node("ObservedAlias");
        const auto output = f.node("Y");
        f.edge({seed}, representative, 0.7);
        f.edge({representative}, observed);
        f.edge({observed}, representative);
        f.edge({observed}, output, 0.8);
        f.graph.attachEvidence({{observed->getTuple(), evidenceValue}});
        // Repeated observations of an equivalent event should collapse to
        // one live evidence root after the deterministic SCC is merged.
        if (evidenceValue) representative->setEvidence(evidenceValue);
        const Worlds worlds(f.graph);
        auto view = f.graph.prune(std::vector<std::string>{"Y"});
        require(view.getNodes().count(observed) == 0 && view.getNodes().count(representative) != 0,
                "bi-imp fixture did not merge the observed alias into its representative");
        require(view.getEvidenceNodes() == std::vector<NodePtr>{representative},
                "pruning returned the removed evidence alias instead of its live representative");
        require(representative->hasEvidence() && representative->getEvidenceValue() == evidenceValue,
                "bi-imp merge lost the evidence value");
        const auto resolved = f.graph.resolveEvidenceNodes();
        require(resolved.size() == 1 && resolved.front() == std::make_pair(representative, evidenceValue),
                "original evidence tuple no longer resolves to the live representative");
        const auto& componentEvidence = view.getCycleDependencyGraph().getComponentEvidencesForNode(representative);
        require(componentEvidence.size() == 1 &&
                        componentEvidence.front() == std::make_pair(representative, evidenceValue),
                "dependency graph did not retain the merged evidence root");
        worlds.verify(view);
    }
    DerivationGraph::setMergeBiImpEnabled(false);
}

}  // namespace

int main() {
    try {
        DerivationGraph::setMergeBiImpEnabled(false);
        DerivationGraph::setPruneExtraEnabled(false);
        deadDagCascade();
        queryAndEvidenceAreRetained();
        anotherConsumerKeepsTheDefinition();
        duplicateOccurrences();
        inactiveRawAdjacencyDoesNotBlockCleanup();
        recursiveStructuresRemainConservative();
        sparseAndCollidingNodeIds();
        zeroHitDoesNotProposeCleanup();
        biImpMergeReturnsLiveEvidenceRoots();
        std::cout << "AND-input local cleanup matches full pruning and random-world events\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
