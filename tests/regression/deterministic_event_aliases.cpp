#include "souffle/problog/DeterministicEventAliases.h"

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
#include <utility>
#include <vector>

using souffle::problog::DeterministicEventAliasResult;
using souffle::problog::eliminateDeterministicEventAliases;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct FixtureGraph : WorkingDerivationGraph {
    // Exercise the real aggregate replay builder with two distinct witness
    // records. The index contains records, not equivalence classes of events.
    NodePtr sum(const std::vector<NodePtr>& records, souffle::RamSigned total) {
        AggregateSpec spec("sum", "Total",
                Atom("Score", {SymbolicField::makeVariable("Weight"),
                                      SymbolicField::makeVariable("Record")}),
                SymbolicField::makeVariable("Weight"));
        AggregateTupleIndex index;
        index.spec = &spec;
        for (const auto& record : records) index.tuplesByBoundKey[{}].push_back(record->getTuple());
        aggregateTupleIndices[700] = {std::move(index)};
        const auto head = createNode(UntypedTuple{"Sum", {souffle::ramBitCast<souffle::RamDomain>(total)}});
        return buildAggregateSumNode(700, 0, head, spec, {"Total"},
                {souffle::ramBitCast<souffle::RamDomain>(total)}, {"Total"});
    }
};

struct Fixture {
    FixtureGraph graph;
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
            double probability = 1.0, std::vector<bool> negations = {}) {
        RuleApplication application;
        application.ruleId = nextRule++;
        application.varValuesPure = {static_cast<souffle::RamDomain>(nextRule)};
        auto result = inputs.empty() ? graph.createHyperedge(inputs, output, application)
                                     : graph.createHyperedge(inputs, output, nullptr, negations, application);
        require(result != nullptr, "fixture could not create its definition");
        result->setProbability(probability);
        return result;
    }
};

using Truth = std::unordered_map<NodePtr, bool>;

// Original random-event bits survive every rewrite. Signed DAG evaluation
// does not rely on graph probabilities or any alias produced by the pass.
struct Worlds {
    std::unordered_map<NodePtr, std::size_t> factBits;
    std::unordered_map<EdgePtr, std::size_t> edgeBits;
    std::vector<double> probabilities;
    std::vector<Truth> before;

    explicit Worlds(const DerivationGraphViewInterface& view) {
        for (const auto& node : view.getNodes()) {
            if (node->isFact && node->getProbability() > 0 && node->getProbability() < 1) {
                factBits.emplace(node, probabilities.size());
                probabilities.push_back(node->getProbability());
            }
        }
        for (const auto& edge : view.getEdges()) {
            if (edge->getProbability() > 0 && edge->getProbability() < 1) {
                edgeBits.emplace(edge, probabilities.size());
                probabilities.push_back(edge->getProbability());
            }
        }
        require(probabilities.size() <= 11, "fixture has too many random worlds");
        for (std::uint64_t world = 0; world < (std::uint64_t{1} << probabilities.size()); ++world) {
            before.push_back(evaluate(view, world));
        }
    }

    Truth evaluate(const DerivationGraphViewInterface& view, std::uint64_t world) const {
        std::unordered_map<NodePtr, std::vector<EdgePtr>> incoming;
        for (const auto& edge : view.getEdges()) incoming[edge->getOutput()].push_back(edge);
        Truth truth;
        std::unordered_set<NodePtr> visiting;
        std::function<bool(const NodePtr&)> visit = [&](const NodePtr& node) {
            const auto known = truth.find(node);
            if (known != truth.end()) return known->second;
            require(view.getNodes().count(node) != 0, "active edge references a missing node");
            require(visiting.insert(node).second, "signed-world fixture must be acyclic");
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
                require(body.size() == signs.size(), "body/sign alignment changed");
                for (std::size_t i = 0; i < body.size(); ++i) {
                    const bool input = visit(body[i]);
                    contribution = contribution && (signs[i] ? !input : input);
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

    double weight(std::uint64_t world) const {
        double result = 1;
        for (std::size_t bit = 0; bit < probabilities.size(); ++bit) {
            result *= ((world >> bit) & 1U) ? probabilities[bit] : 1 - probabilities[bit];
        }
        return result;
    }
};

struct EdgeEvent {
    EdgePtr edge;
    std::size_t id;
    NodePtr head;
    double probability;
    const Rule* rule;
    RuleApplication application;
    std::vector<SupportToken> support;
};

struct FactEvent {
    NodePtr node;
    std::size_t id;
    std::size_t semanticId;
    UntypedTuple tuple;
    double probability;
    std::vector<SupportToken> support;
};

std::string structure(const DerivationGraphViewInterface& view) {
    std::ostringstream out;
    out.precision(17);
    auto nodes = std::vector<NodePtr>(view.getNodes().begin(), view.getNodes().end());
    auto edges = std::vector<EdgePtr>(view.getEdges().begin(), view.getEdges().end());
    std::sort(nodes.begin(), nodes.end());
    std::sort(edges.begin(), edges.end());
    for (const auto& node : nodes) {
        out << node.get() << ' ' << node->getId() << ' ' << node->getTuple().toString() << ' '
            << node->isFact << node->isOriginalFactNode() << node->isShadow << node->pruned
            << node->needOutput << node->isQuery << node->hasEvidence() << node->getEvidenceValue()
            << ' ' << node->getProbability();
        for (auto token : node->getProbabilisticSupportTokens()) out << " s" << token;
        for (const auto& edge : node->getIncomingEdges()) out << " i" << edge.get();
        for (const auto& edge : node->getOutgoingEdges()) out << " o" << edge.get();
        out << '\n';
    }
    for (const auto& edge : edges) {
        out << edge.get() << ' ' << edge->getId() << ' ' << edge->getOutput().get() << ' '
            << edge->getProbability() << ' ' << edge->getRuleApp().ruleId << ' ' << edge->pruned;
        for (const auto& input : edge->getInputs()) out << " i" << input.get();
        for (bool sign : edge->getBodyNegations()) out << " n" << sign;
        for (auto token : edge->getProbabilisticSupportTokens()) out << " s" << token;
        out << '\n';
    }
    return out.str();
}

DeterministicEventAliasResult verify(Fixture& fixture, WorkingSubgraphView& view,
        std::size_t expectedAliases, const std::vector<NodePtr>& joint = {},
        const souffle::problog::detail::AndInputRedundancySnapshot* prepared = nullptr) {
    Worlds worlds(view);
    const auto originalNodes = view.getNodes();
    std::vector<std::pair<NodePtr, bool>> observations;
    std::vector<EdgeEvent> events;
    std::vector<FactEvent> facts;
    for (const auto& node : originalNodes) {
        if (node->hasEvidence()) observations.emplace_back(node, node->getEvidenceValue());
        if (node->isFact) facts.push_back({node, node->getId(), node->getSemanticFactId(), node->getTuple(),
                node->getProbability(), node->getProbabilisticSupportTokens()});
    }
    for (const auto& edge : view.getEdges()) {
        if (!edge->isDeterministic() || !edge->getProbabilisticSupportTokens().empty()) {
            events.push_back({edge, edge->getId(), edge->getOutput(), edge->getProbability(),
                    edge->getRule(), edge->getRuleApp(), edge->getProbabilisticSupportTokens()});
        }
        (void)edge->getInputsStable();
        (void)edge->hasSelfDependency();
    }
    for (const auto& node : originalNodes) {
        (void)view.getIncomingEdges(node);
        (void)view.getOutgoingEdges(node);
    }
    (void)view.getValidNodes();
    (void)view.getValidEdges();
    auto result = eliminateDeterministicEventAliases(fixture.graph, view, true, prepared);
    require(result.aliases.size() == expectedAliases, "unexpected deterministic alias count");
    std::unordered_map<NodePtr, NodePtr> replacements;
    for (const auto& [alias, root] : result.aliases) {
        require(alias != root && originalNodes.count(alias) && view.getNodes().count(root),
                "alias result lost its original name or live representative");
        require(replacements.emplace(alias, root).second, "duplicate alias result");
        require(!view.getNodes().count(alias) && !fixture.graph.getNodes().count(alias),
                "alias node was not physically removed");
        require(fixture.graph.findNode(alias->getTuple()) == root, "tuple lookup lost the alias output name");
        if (alias->isQuery || alias->needOutput) require(root->isQuery && root->needOutput,
                "query alias was not transferred to its representative");
    }
    const auto rootOf = [&](const NodePtr& node) {
        const auto found = replacements.find(node);
        return found == replacements.end() ? node : found->second;
    };
    for (const auto& event : events) {
        const auto& edge = event.edge;
        require(view.getEdges().count(edge) && fixture.graph.getEdges().count(edge),
                "independent rule event was removed or merged");
        require(edge->getId() == event.id && edge->getOutput() == event.head &&
                        edge->getProbability() == event.probability && edge->getRule() == event.rule &&
                        edge->getRuleApp() == event.application &&
                        edge->getProbabilisticSupportTokens() == event.support,
                "rule event identity, application or probability changed");
    }
    for (const auto& fact : facts) {
        const auto& node = fact.node;
        require(view.getNodes().count(node) && fixture.graph.getNodes().count(node) && node->isFact &&
                        node->getId() == fact.id && node->getSemanticFactId() == fact.semanticId &&
                        node->getTuple() == fact.tuple && node->getProbability() == fact.probability &&
                        node->getProbabilisticSupportTokens() == fact.support,
                "independent input fact identity or probability changed");
    }
    double beforeEvidence = 0, afterEvidence = 0, beforeJoint = 0, afterJoint = 0;
    double beforeConditionedJoint = 0, afterConditionedJoint = 0;
    for (std::uint64_t world = 0; world < worlds.before.size(); ++world) {
        const auto truth = worlds.evaluate(view, world);
        const auto& original = worlds.before[world];
        for (const auto& node : originalNodes) {
            require(truth.at(rootOf(node)) == original.at(node), "a retained or aliased event changed in a world");
        }
        bool oldEvidence = true, newEvidence = true, oldJoint = true, newJoint = true;
        for (const auto& [node, value] : observations) {
            oldEvidence &= original.at(node) == value;
            newEvidence &= truth.at(rootOf(node)) == value;
        }
        for (const auto& node : joint) {
            oldJoint &= original.at(node);
            newJoint &= truth.at(rootOf(node));
        }
        const auto weight = worlds.weight(world);
        beforeEvidence += weight * oldEvidence;
        afterEvidence += weight * newEvidence;
        beforeJoint += weight * oldJoint;
        afterJoint += weight * newJoint;
        beforeConditionedJoint += weight * oldEvidence * oldJoint;
        afterConditionedJoint += weight * newEvidence * newJoint;
    }
    require(std::abs(beforeEvidence - afterEvidence) < 1e-12 &&
                    std::abs(beforeJoint - afterJoint) < 1e-12 &&
                    std::abs(beforeConditionedJoint - afterConditionedJoint) < 1e-12,
            "joint query or evidence-conditioned numerator changed");
    require(view.getValidNodes().size() == view.getNodes().size() &&
                    view.getValidEdges().size() == view.getEdges().size(), "validity cache contains retired aliases");
    for (const auto& [alias, root] : result.aliases) {
        (void)root;
        for (const auto& edge : fixture.graph.getEdges()) {
            require(edge->getOutput() != alias &&
                            std::find(edge->getInputs().begin(), edge->getInputs().end(), alias) == edge->getInputs().end(),
                    "owning graph retained an alias endpoint");
        }
    }
    for (const auto& node : view.getNodes()) {
        std::unordered_set<EdgePtr> expectedIncoming, expectedOutgoing;
        for (const auto& edge : view.getEdges()) {
            if (edge->getOutput() == node) expectedIncoming.insert(edge);
            if (std::find(edge->getInputs().begin(), edge->getInputs().end(), node) != edge->getInputs().end()) {
                expectedOutgoing.insert(edge);
            }
        }
        const auto& incoming = view.getIncomingEdges(node);
        const auto& outgoing = view.getOutgoingEdges(node);
        require(std::unordered_set<EdgePtr>(incoming.begin(), incoming.end()) == expectedIncoming &&
                        std::unordered_set<EdgePtr>(outgoing.begin(), outgoing.end()) == expectedOutgoing,
                "adjacency cache disagrees with rewritten edge endpoints");
    }
    const auto stable = structure(view);
    require(eliminateDeterministicEventAliases(fixture.graph, view, true).aliases.empty(),
            "alias elimination did not reach a fixpoint");
    require(structure(view) == stable, "idempotent alias call changed the graph");
    return result;
}

void chainQueriesSignedConsumersAndOwnerPrune() {
    Fixture f;
    const auto root = f.fact("Root", 0.4);
    const auto other = f.fact("Other", 0.3);
    const auto assign = f.fact("Assign", 1);
    const auto a = f.node("AliasA"), b = f.node("AliasB"), c = f.node("AliasC");
    const auto y = f.node("Y"), n = f.node("N"), mixed = f.node("Mixed");
    const auto first = f.edge({root, assign}, a);
    const auto second = f.edge({a, assign}, b);
    const auto third = f.edge({b}, c);
    const auto repeated = f.edge({a, b, root}, y, 0.6);
    const auto negative = f.edge({c, a, other}, n, 0.7, {true, true, false});
    const auto contradictory = f.edge({b, c}, mixed, 0.9, {false, true});
    a->setQuery();
    c->setQuery();
    y->setQuery();
    n->setQuery();
    mixed->setQuery();
    b->setEvidence(true);
    WorkingSubgraphView view(f.graph.getNodes(), f.graph.getEdges());
    const auto result = verify(f, view, 3, {a, c, y});
    require(result.outputAliases.size() == 2, "query names were not retained for output replay");
    require(repeated->getInputs() == std::vector<NodePtr>{root} &&
                    repeated->getBodyNegations() == std::vector<bool>{false},
            "equal-event positive AND occurrences were not deduplicated");
    require(negative->getInputs() == std::vector<NodePtr>({root, other}) &&
                    negative->getBodyNegations() == std::vector<bool>({true, false}),
            "negative consumer substitution changed literal signs");
    require(contradictory->getInputs() == std::vector<NodePtr>({root, root}) &&
                    contradictory->getBodyNegations() == std::vector<bool>({false, true}),
            "mixed-sign occurrences were incorrectly deduplicated");
    require(root->hasEvidence() && root->getEvidenceValue() &&
                    view.getEvidenceNodes() == std::vector<NodePtr>{root}, "evidence root was lost or duplicated");
    const auto repruned = f.graph.prune(std::vector<std::string>{"AliasA", "AliasC", "Y", "N", "Mixed"});
    require(!repruned.getNodes().count(a) && !repruned.getNodes().count(b) && !repruned.getNodes().count(c) &&
                    !repruned.getEdges().count(first) && !repruned.getEdges().count(second) &&
                    !repruned.getEdges().count(third), "owner re-prune revived a retired alias definition");
    require(repruned.getNodes().count(root) && repruned.getEdges().count(repeated) &&
                    repruned.getEdges().count(negative) && repruned.getEdges().count(contradictory),
            "owner re-prune lost query/evidence or consumer events");
}

void derivedTrueConditions() {
    Fixture f;
    const auto root = f.fact("Root"), other = f.fact("Other", 0.6), gate = f.fact("Gate", 1);
    const auto assign = f.node("Assign"), guaranteed = f.node("Guaranteed"), empty = f.node("Empty");
    f.edge({gate}, assign);
    f.edge({assign, gate, gate}, guaranteed);
    // An additional random derivation does not invalidate the certain source.
    f.edge({other}, guaranteed, 0.3);
    f.edge({}, empty);
    const auto a = f.node("AliasA"), b = f.node("AliasB"), c = f.node("AliasEmpty");
    f.edge({root, guaranteed}, a);
    f.edge({a, assign}, b);
    f.edge({root, empty}, c);
    const auto joint = f.node("Joint");
    f.edge({a, b, c}, joint, 0.8);
    for (const auto& node : {a, b, c, joint}) node->setQuery();
    WorkingSubgraphView view(f.graph.getNodes(), f.graph.getEdges());
    const auto result = verify(f, view, 3, {a, b, c, joint});
    require(result.stats.provenTrueNodes == 4 && result.stats.provenDerivedTrueNodes == 3,
            "derived top closure lost a repeated input, alternative source or empty-body proof");
    require(f.graph.findNode(a->getTuple()) == root && f.graph.findNode(b->getTuple()) == root &&
                    f.graph.findNode(c->getTuple()) == root,
            "structurally true derived conditions blocked exact event aliases");
}

void derivedTrueCycles() {
    Fixture f;
    const auto root = f.fact("Root"), gate = f.fact("Gate", 1);
    const auto a = f.node("SeededA"), b = f.node("SeededB");
    f.edge({gate}, a);
    f.edge({b}, a);
    f.edge({a}, b);
    const auto u = f.node("UnseededA"), v = f.node("UnseededB");
    f.edge({v}, u);
    f.edge({u}, v);
    const auto good = f.node("GoodCopy"), bad = f.node("BadCopy");
    f.edge({root, b}, good);
    f.edge({root, u}, bad);
    good->setQuery();
    bad->setQuery();
    // An observation must not turn an unsupported cycle into a true condition.
    u->setEvidence(true);
    WorkingSubgraphView view(f.graph.getNodes(), f.graph.getEdges());
    auto leastFixedPoint = [&](const WorkingSubgraphView& active, bool rootValue) {
        Truth truth;
        for (const auto& node : active.getNodes()) truth.emplace(node, node == gate || (node == root && rootValue));
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& edge : active.getEdges()) {
                bool body = true;
                for (const auto& input : edge->getInputs()) body = body && truth.at(input);
                auto& head = truth.at(edge->getOutput());
                if (body && !head) { head = true; changed = true; }
            }
        }
        return truth;
    };
    const auto falseWorld = leastFixedPoint(view, false), trueWorld = leastFixedPoint(view, true);
    const auto result = eliminateDeterministicEventAliases(f.graph, view, true);
    require(result.aliases.size() == 1 && result.aliases.front() == std::make_pair(good, root),
            "true closure inferred an unseeded cycle or failed to propagate a seed");
    require(result.stats.provenTrueNodes == 3 && result.stats.provenDerivedTrueNodes == 2,
            "cycle truth proof used evidence or discarded a valid seed");
    for (bool rootValue : {false, true}) {
        const auto after = leastFixedPoint(view, rootValue);
        const auto& before = rootValue ? trueWorld : falseWorld;
        for (const auto& [node, value] : before) {
            require(after.at(node == good ? root : node) == value,
                    "structural true closure changed positive least-fixed-point events");
        }
    }
}

void longSignedBodyAndPreparedTraversal() {
    Fixture f;
    const auto root = f.fact("Root"), gate = f.fact("Gate", 1), other = f.fact("Other", 0.6);
    const auto assign = f.node("Assign"), a = f.node("AliasA"), b = f.node("AliasB");
    f.edge({gate}, assign);
    f.edge({root, assign}, a);
    f.edge({a, assign}, b);
    const auto query = f.node("Query");
    const auto consumer = f.edge({a, other, b, root, a, gate, b, other, root, a, b, gate}, query,
            0.7, {false, false, false, false, true, false, true, true, true, false, false, false});
    a->setQuery();
    b->setQuery();
    query->setQuery();
    struct Collector : BackwardTraversalObserver {
        souffle::problog::detail::AndInputRedundancySnapshot snapshot;
        explicit Collector(const Fixture& fixture) {
            snapshot.beginTraversal(fixture.graph.getNodes().size(), fixture.graph.getEdges().size(),
                    fixture.graph.getNodes().size());
        }
        void node(const NodePtr& value) override { snapshot.addTraversalNode(value); }
        void beginEdge(const EdgePtr& value) override { snapshot.beginTraversalEdge(value); }
        void input(const NodePtr& value, bool negative) override { snapshot.addTraversalInput(value, negative); }
        void endEdge() override { snapshot.endTraversalEdge(); }
    } collector(f);
    auto view = f.graph.prune(std::vector<std::string>{"AliasA", "AliasB", "Query"}, &collector);
    collector.snapshot.finishTraversalSources();
    require(collector.snapshot.complete && collector.snapshot.nodes.size() == view.getNodes().size() &&
                    collector.snapshot.edges.size() == view.getEdges().size(),
            "backward pruning did not collect complete alias proof sources");
    collector.snapshot.preparedOwner = &f.graph;
    collector.snapshot.preparedViewNodes = &view.getNodes();
    collector.snapshot.preparedViewEdges = &view.getEdges();
    const auto result = verify(f, view, 2, {a, b, query}, &collector.snapshot);
    require(result.stats.reusedPruneIndexes, "alias pass rebuilt indexes after backward collection");
    require(consumer->getInputs() == std::vector<NodePtr>({root, other, root, gate, other}) &&
                    consumer->getBodyNegations() == std::vector<bool>({false, false, true, false, true}),
            "large alias body compaction changed order or mixed-sign event semantics");
    require(collector.snapshot.rebaseEventAliases(view, result.aliases),
            "shared source index could not follow physical alias mutation");
    require(collector.snapshot.nodes.size() == view.getNodes().size() &&
                    collector.snapshot.edges.size() == view.getEdges().size(),
            "rebased source index retained retired alias definitions");
}

void refusals() {
    for (unsigned mode = 0; mode < 13; ++mode) {
        Fixture f;
        const auto root = f.fact("Root"), other = f.fact("Other");
        auto alias = f.node("Alias");
        std::vector<NodePtr> body{root};
        if (mode == 8) body.push_back(other);
        if (mode == 9) {
            // Its default node probability is one, but its defining event is
            // probabilistic: it cannot act as an always-true assign condition.
            const auto condition = f.node("DerivedCondition");
            f.edge({other}, condition, 0.7);
            body.push_back(condition);
        }
        if (mode >= 10) {
            const auto gate = f.fact("Gate", 1), condition = f.node("Condition");
            const auto proof = f.edge({gate}, condition, 1,
                    mode == 11 ? std::vector<bool>{true} : std::vector<bool>{});
            if (mode == 10) proof->setProbabilisticSupportTokens({makeEdgeSupportToken(901)});
            if (mode == 12) {
                condition->isFact = true;
                condition->setOriginalFact();
                condition->setProbability(0.7);
            }
            body.push_back(condition);
        }
        auto definition = f.edge(body,
                alias, mode == 0 ? 0.999 : 1, mode == 1 ? std::vector<bool>{true} : std::vector<bool>{});
        if (mode == 2) { alias->isFact = true; alias->setOriginalFact(); alias->setProbability(0.2); }
        if (mode == 3) f.edge({other}, alias);
        if (mode == 4) definition->setProbabilisticSupportTokens({makeEdgeSupportToken(900)});
        if (mode == 5) alias->isShadow = true;
        if (mode == 6) root->isShadow = true;
        if (mode == 7) alias->setOriginalFact();
        const auto query = f.node("Query");
        f.edge({alias}, query, 0.6);
        query->setQuery();
        WorkingSubgraphView view(f.graph.getNodes(), f.graph.getEdges());
        const auto before = structure(view);
        verify(f, view, 0, {query, root});
        require(structure(view) == before, "rejected source changed graph structure, mode " + std::to_string(mode));
    }
}

void incompleteAndMissingSources() {
    Fixture f;
    const auto root = f.fact("Root"), alias = f.node("Alias"), alternative = f.fact("Alternative");
    const auto first = f.edge({root}, alias);
    const auto second = f.edge({alternative}, alias);
    WorkingSubgraphView view(f.graph.getNodes(), std::unordered_set<EdgePtr>{first});
    const auto before = structure(view);
    require(eliminateDeterministicEventAliases(f.graph, view).aliases.empty(),
            "unknown source completeness accepted an alias");
    require(structure(view) == before && f.graph.getEdges().count(second), "incomplete snapshot changed owner graph");
    auto missingNodes = f.graph.getNodes();
    missingNodes.erase(root);
    WorkingSubgraphView missing(std::move(missingNodes), std::unordered_set<EdgePtr>{first});
    require(eliminateDeterministicEventAliases(f.graph, missing, true).aliases.empty(),
            "missing active endpoint accepted an alias");
}

void copyCyclesAndConflictingEvidence() {
    {
        Fixture f;
        const auto a = f.node("A"), b = f.node("B"), tail = f.node("Tail");
        f.edge({a}, b);
        f.edge({b}, a);
        f.edge({a}, tail);
        tail->setQuery();
        WorkingSubgraphView view(f.graph.getNodes(), f.graph.getEdges());
        const auto before = structure(view);
        const auto result = eliminateDeterministicEventAliases(f.graph, view, true);
        require(result.aliases.empty() && structure(view) == before,
                "copy cycle or chain reaching it was assigned an arbitrary root");
        // Positive least fixed point has no seed in this SCC: all events stay
        // false. Refusal preserves that, rather than treating equality as true.
        Truth truth;
        for (const auto& node : view.getNodes()) truth[node] = false;
        for (std::size_t round = 0; round < view.getNodes().size(); ++round) {
            for (const auto& edge : view.getEdges()) {
                bool body = true;
                for (const auto& input : edge->getInputs()) body &= truth.at(input);
                truth[edge->getOutput()] = truth.at(edge->getOutput()) || body;
            }
        }
        require(!truth.at(a) && !truth.at(b) && !truth.at(tail), "unseeded copy cycle became supported");
    }
    {
        Fixture f;
        const auto root = f.fact("Root"), a = f.node("A"), b = f.node("B");
        f.edge({root}, a);
        f.edge({a}, b);
        root->setEvidence(false);
        b->setEvidence(true);
        b->setQuery();
        WorkingSubgraphView view(f.graph.getNodes(), f.graph.getEdges());
        const auto before = structure(view);
        verify(f, view, 0, {a, b});
        require(structure(view) == before && view.getEvidenceNodes().size() == 2,
                "conflicting alias evidence was overwritten or partially merged");
    }
}

void independentEventsAndInactiveConsumers() {
    Fixture f;
    const auto root = f.fact("Root", 0.5), independent = f.fact("Independent", 0.5);
    const auto alias = f.node("Alias"), query = f.node("Query"), hidden = f.node("Hidden");
    f.edge({root, root}, alias);
    const auto first = f.edge({alias, independent}, query, 0.7);
    const auto second = f.edge({alias, independent}, query, 0.7);
    const auto zero = f.edge({alias, independent}, query, 0);
    const auto inactive = f.edge({alias}, hidden, 0.8);
    // Matching/overlapping provenance tokens never identify rule events.
    first->setProbabilisticSupportTokens({makeEdgeSupportToken(800), makeEdgeSupportToken(801)});
    second->setProbabilisticSupportTokens({makeEdgeSupportToken(801)});
    query->setQuery();
    auto activeNodes = f.graph.getNodes();
    activeNodes.erase(hidden);
    auto activeEdges = f.graph.getEdges();
    activeEdges.erase(inactive);
    hidden->pruned = inactive->pruned = true;
    WorkingSubgraphView view(std::move(activeNodes), std::move(activeEdges));
    verify(f, view, 1, {query, root, independent});
    require(first != second && first->getInputs() == std::vector<NodePtr>({root, independent}) &&
                    second->getInputs() == first->getInputs() && view.getEdges().count(zero),
            "independent equal-probability or probability-zero rules were merged");
    require(inactive->getInputs() == std::vector<NodePtr>{root} && inactive->pruned && hidden->pruned &&
                    !view.getEdges().count(inactive), "inactive raw consumer was revived or left on a retired alias");
    hidden->setQuery();
    const auto repruned = f.graph.prune(std::vector<std::string>{"Query", "Hidden"});
    require(repruned.getEdges().count(inactive) && repruned.getNodes().count(root) &&
                    !repruned.getNodes().count(alias), "later owner query restored a retired alias dependency");
}

void sumRecordMultiplicity() {
    Fixture f;
    const auto event = f.fact("Event", 0.35);
    const auto first = f.graph.createNode(UntypedTuple{"Score", {2, 11}});
    const auto second = f.graph.createNode(UntypedTuple{"Score", {3, 22}});
    f.edge({event}, first);
    f.edge({event}, second);
    const auto sum5 = f.graph.sum({first, second}, 5);
    require(sum5 != nullptr, "real sum replay failed to build");
    const auto stateTuple = sum5->getTuple();
    auto sumNode = [&](souffle::RamSigned value) {
        auto tuple = stateTuple;
        tuple.fields.back() = souffle::ramBitCast<souffle::RamDomain>(value);
        return f.graph.findNode(tuple);
    };
    const auto sum0 = sumNode(0), sum2 = sumNode(2), sum3 = sumNode(3);
    require(sum0 && sum2 && sum3, "sum replay lost distinct reachable numeric states");
    for (const auto& node : {sum0, sum2, sum3, sum5}) node->setQuery();
    WorkingSubgraphView view(f.graph.getNodes(), f.graph.getEdges());
    Worlds worlds(view);
    for (const auto& truth : worlds.before) {
        require(truth.at(sum5) == truth.at(event) && truth.at(sum0) != truth.at(event) &&
                        !truth.at(sum2) && !truth.at(sum3), "sum records were treated as independent events");
    }
    // The pass may also find replay-state copies. Assert exact world semantics
    // independently without predicting which harmless state aliases it chooses.
    auto result = eliminateDeterministicEventAliases(f.graph, view, true);
    std::unordered_map<NodePtr, NodePtr> roots;
    for (const auto& [alias, root] : result.aliases) roots.emplace(alias, root);
    require(roots.count(first) && roots.count(second) && roots.at(first) == event && roots.at(second) == event,
            "sum witness records did not share the certified event");
    const auto representative = [&](const NodePtr& node) {
        const auto found = roots.find(node);
        return found == roots.end() ? node : found->second;
    };
    for (std::uint64_t world = 0; world < worlds.before.size(); ++world) {
        const auto truth = worlds.evaluate(view, world);
        for (const auto& [node, value] : worlds.before[world]) {
            require(truth.at(representative(node)) == value, "alias rewrite changed weighted sum record multiplicity");
        }
    }
    require(first->getTuple().fields == std::vector<souffle::RamDomain>({2, 11}) &&
                    second->getTuple().fields == std::vector<souffle::RamDomain>({3, 22}),
            "sum witness identity or weight metadata was coalesced");
}

}  // namespace

int main() {
    try {
        DerivationGraph::setMergeBiImpEnabled(false);
        DerivationGraph::setPruneExtraEnabled(false);
        chainQueriesSignedConsumersAndOwnerPrune();
        derivedTrueConditions();
        derivedTrueCycles();
        longSignedBodyAndPreparedTraversal();
        refusals();
        incompleteAndMissingSources();
        copyCyclesAndConflictingEvidence();
        independentEventsAndInactiveConsumers();
        sumRecordMultiplicity();
        std::cout << "deterministic event alias regression passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "deterministic event alias regression failed: " << error.what() << '\n';
        return 1;
    }
}
