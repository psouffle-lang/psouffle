#include "souffle/problog/LiftedWmc.h"

#include "souffle/problog/Atom.h"
#include "souffle/problog/Rule.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace souffle::problog {
namespace {

std::string escapeDotLabel(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
    case '\\':
      escaped += "\\\\";
      break;
    case '"':
      escaped += "\\\"";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += ch;
      break;
    }
  }
  return escaped;
}

enum class ParamTermKind {
  RootParameter,
  WitnessParameter,
  Constant,
};

struct ParamTerm {
  ParamTermKind kind = ParamTermKind::Constant;
  std::size_t parameter = 0;
  std::size_t witnessEdge = 0;
  RamDomain constant = 0;

  static ParamTerm root(std::size_t index) {
    ParamTerm term;
    term.kind = ParamTermKind::RootParameter;
    term.parameter = index;
    return term;
  }

  static ParamTerm witness(std::size_t edge, std::size_t index) {
    ParamTerm term;
    term.kind = ParamTermKind::WitnessParameter;
    term.parameter = index;
    term.witnessEdge = edge;
    return term;
  }

  static ParamTerm value(RamDomain value) {
    ParamTerm term;
    term.kind = ParamTermKind::Constant;
    term.constant = value;
    return term;
  }

  bool operator==(const ParamTerm &other) const {
    return std::tie(kind, parameter, witnessEdge, constant) ==
           std::tie(other.kind, other.parameter, other.witnessEdge,
                    other.constant);
  }

  bool operator<(const ParamTerm &other) const {
    return std::tie(kind, parameter, witnessEdge, constant) <
           std::tie(other.kind, other.parameter, other.witnessEdge,
                    other.constant);
  }

  std::string toString() const {
    if (kind == ParamTermKind::RootParameter) {
      return "$" + std::to_string(parameter);
    }
    if (kind == ParamTermKind::WitnessParameter) {
      return "$e" + std::to_string(witnessEdge) + "v" +
             std::to_string(parameter);
    }
    return std::to_string(constant);
  }
};

enum class SymbolicVariableKind {
  RuleApplicationGuard,
  ExtensionalFact,
  ProbabilisticRule,
};

using AbstractNodeId = std::uint32_t;
using AbstractEdgeId = std::uint32_t;

struct SymbolicVariable {
  SymbolicVariableKind kind = SymbolicVariableKind::ExtensionalFact;
  AbstractNodeId node = 0;
  AbstractEdgeId edge = 0;

  bool operator==(const SymbolicVariable &other) const {
    return std::tie(kind, node, edge) ==
           std::tie(other.kind, other.node, other.edge);
  }

  bool operator<(const SymbolicVariable &other) const {
    return std::tie(kind, node, edge) <
           std::tie(other.kind, other.node, other.edge);
  }
};

using BddId = std::uint32_t;

enum class BddOperation {
  And,
  Or,
};

struct BddNode {
  SymbolicVariable variable;
  BddId low = 0;
  BddId high = 0;
  bool terminal = true;
};

struct BddNodeKey {
  SymbolicVariable variable;
  BddId low = 0;
  BddId high = 0;

  bool operator<(const BddNodeKey &other) const {
    if (variable < other.variable) {
      return true;
    }
    if (other.variable < variable) {
      return false;
    }
    return std::tie(low, high) < std::tie(other.low, other.high);
  }
};

class SymbolicBdd {
public:
  SymbolicBdd() {
    nodes.emplace_back();
    nodes.emplace_back();
  }

  BddId getFalse() const { return 0; }

  BddId getTrue() const { return 1; }

  BddId makeVariable(const SymbolicVariable &variable) {
    variables.insert(variable);
    return makeNode(variable, getFalse(), getTrue());
  }

  BddId makeAnd(BddId lhs, BddId rhs) {
    return apply(BddOperation::And, lhs, rhs);
  }

  BddId makeOr(BddId lhs, BddId rhs) {
    return apply(BddOperation::Or, lhs, rhs);
  }

  const BddNode &getNode(BddId id) const { return nodes.at(id); }

  const std::set<SymbolicVariable> &getVariables() const { return variables; }

  std::size_t nodeCount() const { return nodes.size(); }

private:
  BddId makeNode(const SymbolicVariable &variable, BddId low, BddId high) {
    if (low == high) {
      return low;
    }
    BddNodeKey key{variable, low, high};
    auto it = uniqueNodes.find(key);
    if (it != uniqueNodes.end()) {
      return it->second;
    }
    if (nodes.size() >= std::numeric_limits<BddId>::max()) {
      throw std::runtime_error("lifted BDD node limit exceeded");
    }
    const auto id = static_cast<BddId>(nodes.size());
    BddNode node;
    node.variable = variable;
    node.low = low;
    node.high = high;
    node.terminal = false;
    nodes.push_back(std::move(node));
    uniqueNodes.emplace(std::move(key), id);
    return id;
  }

  BddId apply(BddOperation operation, BddId lhs, BddId rhs) {
    if (lhs > rhs) {
      std::swap(lhs, rhs);
    }
    if (operation == BddOperation::And) {
      if (lhs == getFalse() || rhs == getFalse()) {
        return getFalse();
      }
      if (lhs == getTrue()) {
        return rhs;
      }
      if (lhs == rhs) {
        return lhs;
      }
    } else {
      if (lhs == getTrue() || rhs == getTrue()) {
        return getTrue();
      }
      if (lhs == getFalse()) {
        return rhs;
      }
      if (lhs == rhs) {
        return lhs;
      }
    }

    const auto cacheKey = std::make_tuple(operation, lhs, rhs);
    auto cached = applyCache.find(cacheKey);
    if (cached != applyCache.end()) {
      return cached->second;
    }

    const BddNode &lhsNode = nodes.at(lhs);
    const BddNode &rhsNode = nodes.at(rhs);
    SymbolicVariable top;
    if (lhsNode.terminal) {
      top = rhsNode.variable;
    } else if (rhsNode.terminal || lhsNode.variable < rhsNode.variable) {
      top = lhsNode.variable;
    } else {
      top = rhsNode.variable;
    }

    auto cofactor = [&](BddId id, bool high) {
      const BddNode &node = nodes.at(id);
      if (!node.terminal && node.variable == top) {
        return high ? node.high : node.low;
      }
      return id;
    };

    const BddId low =
        apply(operation, cofactor(lhs, false), cofactor(rhs, false));
    const BddId high =
        apply(operation, cofactor(lhs, true), cofactor(rhs, true));
    const BddId result = makeNode(top, low, high);
    applyCache.emplace(cacheKey, result);
    return result;
  }

  std::vector<BddNode> nodes;
  std::map<BddNodeKey, BddId> uniqueNodes;
  std::map<std::tuple<BddOperation, BddId, BddId>, BddId> applyCache;
  std::set<SymbolicVariable> variables;
};

struct RelationCallKey {
  std::string relation;
  std::vector<ParamTerm> arguments;

  bool operator<(const RelationCallKey &other) const {
    if (relation != other.relation) {
      return relation < other.relation;
    }
    return std::lexicographical_compare(arguments.begin(), arguments.end(),
                                        other.arguments.begin(),
                                        other.arguments.end());
  }
};

struct AbstractBindingDomain {
  std::size_t ruleId = 0;
  std::string headRelation;
  std::vector<ParamTerm> headArguments;
  std::vector<ParamTerm> groundingKey;
};

struct AbstractDerivationNode {
  AbstractNodeId id = 0;
  std::string relation;
  std::vector<ParamTerm> arguments;
  bool extensional = false;
  std::vector<AbstractEdgeId> incomingEdges;
};

struct AbstractDerivationEdge {
  AbstractEdgeId id = 0;
  AbstractNodeId head = 0;
  std::vector<AbstractNodeId> body;
  AbstractBindingDomain domain;
  double probability = 1.0;
};

class AbstractDerivationGraph {
public:
  AbstractDerivationGraph(const RuleManager &ruleManager,
                          const std::unordered_set<std::size_t> &activeRuleIds,
                          std::unordered_set<std::string> inputRelations)
      : ruleManager(ruleManager), activeRuleIds(activeRuleIds),
        inputRelations(std::move(inputRelations)) {}

  AbstractNodeId buildOutput(const Relation &relation) {
    std::vector<ParamTerm> arguments;
    arguments.reserve(relation.getArity());
    for (std::size_t i = 0; i < relation.getArity(); ++i) {
      arguments.push_back(ParamTerm::root(i));
    }
    return getOrCreateNode(relation.getName(), arguments);
  }

  const AbstractDerivationNode &getNode(AbstractNodeId id) const {
    return nodes.at(id);
  }

  const AbstractDerivationEdge &getEdge(AbstractEdgeId id) const {
    return edges.at(id);
  }

  std::size_t nodeCount() const { return nodes.size(); }

  std::size_t edgeCount() const { return edges.size(); }

  std::string toDot() const {
    auto formatCall = [](const AbstractDerivationNode &node) {
      std::ostringstream label;
      label << node.relation << "(";
      for (std::size_t i = 0; i < node.arguments.size(); ++i) {
        if (i > 0) {
          label << ",";
        }
        label << node.arguments[i].toString();
      }
      label << ")";
      if (node.extensional) {
        label << "\ninput";
      }
      return label.str();
    };

    std::ostringstream out;
    out << "digraph AbstractDerivationGraph {\n";
    out << "  rankdir=LR;\n";
    out << "  node [shape=box, style=filled, fillcolor=lightblue];\n";
    for (const auto &node : nodes) {
      out << "  node" << node.id << " [label=\""
          << escapeDotLabel(formatCall(node)) << "\"];\n";
    }
    out << "  node [shape=point, fillcolor=red, width=0.2];\n";
    for (const auto &edge : edges) {
      out << "  edge" << edge.id << " [xlabel=\"rule " << edge.domain.ruleId;
      if (edge.probability != 1.0) {
        out << ", p=" << edge.probability;
      }
      out << "\"];\n";
      for (const auto bodyNode : edge.body) {
        out << "  node" << bodyNode << " -> edge" << edge.id << ";\n";
      }
      out << "  edge" << edge.id << " -> node" << edge.head << ";\n";
    }
    out << "}\n";
    return out.str();
  }

  std::string toTsv() const {
    auto joinTerms = [](const std::vector<ParamTerm> &terms) {
      std::ostringstream out;
      for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i > 0) {
          out << ",";
        }
        out << terms[i].toString();
      }
      return out.str();
    };
    auto joinIds = [](const std::vector<AbstractNodeId> &ids) {
      std::ostringstream out;
      for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
          out << ",";
        }
        out << ids[i];
      }
      return out.str();
    };

    std::ostringstream out;
    out << "kind\tid\trelation_or_head\targuments_or_body"
        << "\trule_id\tprobability\tbinding_key\n";
    for (const auto &node : nodes) {
      out << "node\t" << node.id << "\t" << node.relation << "\t"
          << joinTerms(node.arguments) << "\t\t\t"
          << (node.extensional ? "extensional" : "derived") << "\n";
    }
    for (const auto &edge : edges) {
      out << "edge\t" << edge.id << "\t" << edge.head << "\t"
          << joinIds(edge.body) << "\t" << edge.domain.ruleId << "\t"
          << edge.probability << "\t" << joinTerms(edge.domain.groundingKey)
          << "\n";
    }
    return out.str();
  }

private:
  ParamTerm substituteField(
      const SymbolicField &field,
      const std::unordered_map<std::string, ParamTerm> &bindings) const {
    if (std::holds_alternative<VariableField>(field.field)) {
      const auto &name = std::get<VariableField>(field.field).name;
      auto it = bindings.find(name);
      if (it == bindings.end()) {
        throw std::runtime_error(
            "abstract graph variable binding is missing: " + name);
      }
      return it->second;
    }
    static const std::vector<std::string> noVariables;
    static const std::vector<RamDomain> noValues;
    return ParamTerm::value(
        evaluateSymbolicField(field, noVariables, noValues));
  }

  AbstractNodeId getOrCreateNode(const std::string &relation,
                                 const std::vector<ParamTerm> &arguments) {
    RelationCallKey key{relation, arguments};
    if (activeCalls.count(key) > 0) {
      throw std::runtime_error("recursive abstract derivation graph");
    }
    auto cached = nodeByCall.find(key);
    if (cached != nodeByCall.end()) {
      return cached->second;
    }
    if (nodes.size() >= std::numeric_limits<AbstractNodeId>::max()) {
      throw std::runtime_error("abstract derivation node limit exceeded");
    }

    const auto nodeId = static_cast<AbstractNodeId>(nodes.size());
    AbstractDerivationNode node;
    node.id = nodeId;
    node.relation = relation;
    node.arguments = arguments;
    nodes.push_back(std::move(node));
    nodeByCall.emplace(key, nodeId);

    activeCalls.insert(key);

    auto rules = ruleManager.getRulesForPredicate(relation);
    std::sort(rules.begin(), rules.end(), [](const Rule *lhs, const Rule *rhs) {
      return lhs->getRuleId() < rhs->getRuleId();
    });
    nodes[nodeId].extensional = inputRelations.count(relation) > 0;
    if (detOptEnabled && isDetRelation(relation)) {
      nodes[nodeId].extensional = true;
      activeCalls.erase(key);
      return nodeId;
    }
    for (const Rule *rule : rules) {
      if (rule == nullptr || activeRuleIds.count(rule->getRuleId()) == 0) {
        continue;
      }
      if (edges.size() >= std::numeric_limits<AbstractEdgeId>::max()) {
        throw std::runtime_error("abstract derivation edge limit exceeded");
      }
      const auto edgeId = static_cast<AbstractEdgeId>(edges.size());
      AbstractDerivationEdge edge;
      edge.id = edgeId;
      edge.head = nodeId;
      edge.probability = rule->getProbability();
      edge.domain.ruleId = rule->getRuleId();
      edge.domain.headRelation = relation;
      edge.domain.headArguments = arguments;
      edges.push_back(std::move(edge));

      std::unordered_map<std::string, ParamTerm> bindings;
      const auto &headFields = rule->getHead().getFields();
      for (std::size_t i = 0; i < headFields.size(); ++i) {
        const auto &name = std::get<VariableField>(headFields[i].field).name;
        bindings.emplace(name, arguments.at(i));
      }

      const auto ruleVariables = rule->getVars();
      for (std::size_t i = 0; i < ruleVariables.size(); ++i) {
        bindings.emplace(ruleVariables[i], ParamTerm::witness(edgeId, i));
      }

      std::vector<ParamTerm> groundingKey;
      groundingKey.reserve(ruleVariables.size());
      for (const auto &variable : ruleVariables) {
        groundingKey.push_back(bindings.at(variable));
      }

      std::vector<AbstractNodeId> body;
      body.reserve(rule->getBodyAtoms().size());
      for (const auto &bodyAtom : rule->getBodyAtoms()) {
        std::vector<ParamTerm> bodyArguments;
        bodyArguments.reserve(bodyAtom.getFields().size());
        for (const auto &field : bodyAtom.getFields()) {
          bodyArguments.push_back(substituteField(field, bindings));
        }
        body.push_back(getOrCreateNode(bodyAtom.getRelation(), bodyArguments));
      }

      edges[edgeId].body = std::move(body);
      edges[edgeId].domain.groundingKey = std::move(groundingKey);
      nodes[nodeId].incomingEdges.push_back(edgeId);
    }

    activeCalls.erase(key);
    return nodeId;
  }

  const RuleManager &ruleManager;
  const std::unordered_set<std::size_t> &activeRuleIds;
  std::unordered_set<std::string> inputRelations;
  std::vector<AbstractDerivationNode> nodes;
  std::vector<AbstractDerivationEdge> edges;
  std::map<RelationCallKey, AbstractNodeId> nodeByCall;
  std::set<RelationCallKey> activeCalls;
};

class PointwiseEligibility {
public:
  PointwiseEligibility(SouffleProgram &program, const RuleManager &ruleManager,
                       const std::unordered_set<std::size_t> &activeRuleIds)
      : program(program), ruleManager(ruleManager),
        activeRuleIds(activeRuleIds) {}

  bool analyze(const std::vector<Relation *> &outputs) {
    for (const auto *output : outputs) {
      if (output == nullptr || !analyzeRelation(output->getName())) {
        return false;
      }
    }
    return true;
  }

  const std::string &failureReason() const { return reason; }

  const std::unordered_set<std::string> &relations() const {
    return relationClosure;
  }

private:
  bool analyzeRelation(const std::string &relation) {
    if (accepted.count(relation) > 0) {
      return true;
    }
    if (active.count(relation) > 0) {
      return fail("recursive relation dependency: " + relation);
    }
    if (program.getRelation(relation) == nullptr) {
      return fail("relation is not materialized at runtime: " + relation);
    }

    active.insert(relation);
    relationClosure.insert(relation);
    if (detOptEnabled && isDetRelation(relation)) {
      active.erase(relation);
      accepted.insert(relation);
      return true;
    }
    auto rules = ruleManager.getRulesForPredicate(relation);
    std::sort(rules.begin(), rules.end(), [](const Rule *lhs, const Rule *rhs) {
      return lhs->getRuleId() < rhs->getRuleId();
    });

    for (const Rule *rule : rules) {
      if (rule == nullptr || rule->isFact()) {
        return fail("source fact rules are not supported for relation: " +
                    relation);
      }
      if (activeRuleIds.count(rule->getRuleId()) == 0) {
        continue;
      }
      // Recursive rules (and any rule in a recursive stratum) are not lifted;
      // their output relations fall back to concrete WMC.
      if (rule->isRecursive() || rule->isInRecursiveStratum()) {
        return fail("recursive rule is not supported: " +
                    std::to_string(rule->getRuleId()));
      }
      if (!rule->getAggregates().empty()) {
        return fail("aggregate rule is not supported: " +
                    std::to_string(rule->getRuleId()));
      }

      std::unordered_set<std::string> headVariables;
      const auto &headFields = rule->getHead().getFields();
      auto *headRelation = program.getRelation(relation);
      if (headRelation == nullptr ||
          headFields.size() != headRelation->getArity()) {
        return fail("head arity mismatch for rule: " +
                    std::to_string(rule->getRuleId()));
      }
      for (const auto &field : headFields) {
        if (!std::holds_alternative<VariableField>(field.field)) {
          return fail("head is not a distinct-variable tuple for rule: " +
                      std::to_string(rule->getRuleId()));
        }
        const auto &name = std::get<VariableField>(field.field).name;
        if (name == "_" || !headVariables.insert(name).second) {
          return fail("head contains wildcard or repeated variable for rule: " +
                      std::to_string(rule->getRuleId()));
        }
      }

      const auto ruleVariables = rule->getVars();
      std::unordered_set<std::string> ruleVariableSet(ruleVariables.begin(),
                                                      ruleVariables.end());
      if (ruleVariableSet.size() != ruleVariables.size()) {
        return fail("rule variable inventory contains duplicates for rule: " +
                    std::to_string(rule->getRuleId()));
      }
      for (const auto &headVariable : headVariables) {
        if (ruleVariableSet.count(headVariable) == 0) {
          return fail(
              "head variable is absent from runtime grounding for rule: " +
              std::to_string(rule->getRuleId()));
        }
      }

      for (const auto &bodyAtom : rule->getBodyAtoms()) {
        if (bodyAtom.isNegatedAtom()) {
          return fail("negated body atom is not supported for rule: " +
                      std::to_string(rule->getRuleId()));
        }
        auto *bodyRelation = program.getRelation(bodyAtom.getRelation());
        if (bodyRelation == nullptr ||
            bodyRelation->getArity() != bodyAtom.getFields().size()) {
          return fail("body relation is missing or has mismatched arity: " +
                      bodyAtom.getRelation());
        }
        for (const auto &field : bodyAtom.getFields()) {
          if (std::holds_alternative<VariableField>(field.field)) {
            const auto &name = std::get<VariableField>(field.field).name;
            if (name == "_" || ruleVariableSet.count(name) == 0) {
              return fail("body contains an ungrounded variable for rule: " +
                          std::to_string(rule->getRuleId()));
            }
          } else if (std::holds_alternative<std::shared_ptr<ExprField>>(
                         field.field)) {
            return fail("body expression field is not supported for rule: " +
                        std::to_string(rule->getRuleId()));
          }
        }
      }

      if (!hasConnectedBindingPath(*rule, headVariables, ruleVariableSet)) {
        return false;
      }
      if (ruleVariableSet != headVariables &&
          !hasUniqueRuntimeWitnesses(*rule, relation)) {
        return false;
      }

      for (const auto &bodyAtom : rule->getBodyAtoms()) {
        if (!analyzeRelation(bodyAtom.getRelation())) {
          return false;
        }
      }
    }

    active.erase(relation);
    accepted.insert(relation);
    return true;
  }

  bool hasConnectedBindingPath(
      const Rule &rule, const std::unordered_set<std::string> &headVariables,
      const std::unordered_set<std::string> &ruleVariables) {
    std::vector<std::unordered_set<std::string>> pendingAtoms;
    pendingAtoms.reserve(rule.getBodyAtoms().size());
    for (const auto &bodyAtom : rule.getBodyAtoms()) {
      std::unordered_set<std::string> atomVariables;
      for (const auto &field : bodyAtom.getFields()) {
        if (std::holds_alternative<VariableField>(field.field)) {
          atomVariables.insert(std::get<VariableField>(field.field).name);
        }
      }
      pendingAtoms.push_back(std::move(atomVariables));
    }

    std::unordered_set<std::string> bound = headVariables;
    std::vector<bool> selected(pendingAtoms.size(), false);
    std::size_t selectedCount = 0;
    while (selectedCount < pendingAtoms.size()) {
      bool madeProgress = false;
      for (std::size_t i = 0; i < pendingAtoms.size(); ++i) {
        if (selected[i]) {
          continue;
        }
        const auto &variables = pendingAtoms[i];
        const bool connected =
            variables.empty() || std::any_of(variables.begin(), variables.end(),
                                             [&](const std::string &variable) {
                                               return bound.count(variable) > 0;
                                             });
        if (!connected) {
          continue;
        }
        bound.insert(variables.begin(), variables.end());
        selected[i] = true;
        ++selectedCount;
        madeProgress = true;
      }
      if (!madeProgress) {
        return fail(
            "body has a disconnected free-variable component for rule: " +
            std::to_string(rule.getRuleId()));
      }
    }

    for (const auto &variable : ruleVariables) {
      if (bound.count(variable) == 0) {
        return fail("runtime grounding variable is not bound by a body atom "
                    "for rule: " +
                    std::to_string(rule.getRuleId()));
      }
    }
    return true;
  }

  bool hasUniqueRuntimeWitnesses(const Rule &rule,
                                 const std::string &relation) {
    const auto ruleVariables = rule.getVars();
    for (const auto &[tuple, applications] :
         DerivationManager::untypedTuple2RuleApplications) {
      if (tuple.relation_name != relation || applications == nullptr) {
        continue;
      }
      std::size_t matchingApplications = 0;
      for (const auto &application : *applications) {
        if (static_cast<std::size_t>(application.ruleId) != rule.getRuleId()) {
          continue;
        }
        if (application.varValuesPure.size() != ruleVariables.size()) {
          return fail("runtime grounding arity mismatch for rule: " +
                      std::to_string(rule.getRuleId()));
        }
        ++matchingApplications;
        if (matchingApplications > 1) {
          return fail("multiple runtime witnesses for rule: " +
                      std::to_string(rule.getRuleId()));
        }
      }
    }
    return true;
  }

  bool fail(std::string message) {
    if (reason.empty()) {
      reason = std::move(message);
    }
    return false;
  }

  SouffleProgram &program;
  const RuleManager &ruleManager;
  const std::unordered_set<std::size_t> &activeRuleIds;
  std::unordered_set<std::string> accepted;
  std::unordered_set<std::string> active;
  std::unordered_set<std::string> relationClosure;
  std::string reason;
};

class ParameterizedForwardCompiler {
public:
  ParameterizedForwardCompiler(const AbstractDerivationGraph &graph,
                               SymbolicBdd &bdd)
      : graph(graph), bdd(bdd) {}

  BddId compile(AbstractNodeId nodeId) {
    auto cached = nodeFormulas.find(nodeId);
    if (cached != nodeFormulas.end()) {
      return cached->second;
    }
    if (!activeNodes.insert(nodeId).second) {
      throw std::runtime_error("recursive parameterized forward compilation");
    }

    const auto &node = graph.getNode(nodeId);
    BddId result = bdd.getFalse();
    if (node.extensional) {
      SymbolicVariable fact;
      fact.kind = SymbolicVariableKind::ExtensionalFact;
      fact.node = nodeId;
      result = bdd.makeVariable(fact);
    }
    for (const auto edgeId : node.incomingEdges) {
      const auto &edge = graph.getEdge(edgeId);
      SymbolicVariable guard;
      guard.kind = SymbolicVariableKind::RuleApplicationGuard;
      guard.edge = edgeId;
      BddId alternative = bdd.makeVariable(guard);

      if (edge.probability <= 0.0) {
        alternative = bdd.getFalse();
      } else if (edge.probability < 1.0) {
        SymbolicVariable random;
        random.kind = SymbolicVariableKind::ProbabilisticRule;
        random.edge = edgeId;
        alternative = bdd.makeAnd(alternative, bdd.makeVariable(random));
      }

      for (const auto bodyNode : edge.body) {
        alternative = bdd.makeAnd(alternative, compile(bodyNode));
      }
      result = bdd.makeOr(result, alternative);
    }

    activeNodes.erase(nodeId);
    nodeFormulas.emplace(nodeId, result);
    return result;
  }

private:
  const AbstractDerivationGraph &graph;
  SymbolicBdd &bdd;
  std::unordered_map<AbstractNodeId, BddId> nodeFormulas;
  std::unordered_set<AbstractNodeId> activeNodes;
};

struct ConcreteVariableKey {
  SymbolicVariableKind kind = SymbolicVariableKind::ExtensionalFact;
  std::string relation;
  std::size_t ruleId = 0;
  std::vector<RamDomain> arguments;

  bool operator<(const ConcreteVariableKey &other) const {
    if (kind != other.kind) {
      return kind < other.kind;
    }
    if (relation != other.relation) {
      return relation < other.relation;
    }
    if (ruleId != other.ruleId) {
      return ruleId < other.ruleId;
    }
    return std::lexicographical_compare(arguments.begin(), arguments.end(),
                                        other.arguments.begin(),
                                        other.arguments.end());
  }
};

class RuntimeBindingContext {
public:
  RuntimeBindingContext(const AbstractDerivationGraph &graph,
                        const std::vector<RamDomain> &rootValues)
      : graph(graph), rootValues(rootValues) {}

  RamDomain evaluate(const ParamTerm &term) {
    if (term.kind == ParamTermKind::Constant) {
      return term.constant;
    }
    if (term.kind == ParamTermKind::RootParameter) {
      if (term.parameter >= rootValues.size()) {
        throw std::runtime_error("lifted root parameter index is out of range");
      }
      return rootValues[term.parameter];
    }

    const RuleApplication *application = resolve(term.witnessEdge);
    if (application == nullptr) {
      throw std::runtime_error(
          "witness parameter has no runtime rule application");
    }
    if (term.parameter >= application->varValuesPure.size()) {
      throw std::runtime_error(
          "lifted witness parameter index is out of range");
    }
    return application->varValuesPure[term.parameter];
  }

  std::vector<RamDomain> evaluateTerms(const std::vector<ParamTerm> &terms) {
    std::vector<RamDomain> values;
    values.reserve(terms.size());
    for (const auto &term : terms) {
      values.push_back(evaluate(term));
    }
    return values;
  }

  const RuleApplication *resolve(AbstractEdgeId edgeId) {
    if (resolvedEdges.count(edgeId) > 0) {
      return applications.at(edgeId);
    }
    if (!activeEdges.insert(edgeId).second) {
      throw std::runtime_error("recursive lifted witness resolution");
    }

    const auto &edge = graph.getEdge(edgeId);
    const auto &domain = edge.domain;
    UntypedTuple headTuple{domain.headRelation,
                           evaluateTerms(domain.headArguments)};
    auto tupleIt =
        DerivationManager::untypedTuple2RuleApplications.find(headTuple);
    const RuleApplication *match = nullptr;
    if (tupleIt != DerivationManager::untypedTuple2RuleApplications.end() &&
        tupleIt->second != nullptr) {
      for (const auto &application : *tupleIt->second) {
        if (static_cast<std::size_t>(application.ruleId) != domain.ruleId) {
          continue;
        }
        if (match != nullptr) {
          throw std::runtime_error(
              "multiple runtime witnesses reached lifted evaluation");
        }
        match = &application;
      }
    }

    activeEdges.erase(edgeId);
    resolvedEdges.insert(edgeId);
    applications.emplace(edgeId, match);
    return match;
  }

private:
  const AbstractDerivationGraph &graph;
  const std::vector<RamDomain> &rootValues;
  std::unordered_set<AbstractEdgeId> resolvedEdges;
  std::unordered_set<AbstractEdgeId> activeEdges;
  std::unordered_map<AbstractEdgeId, const RuleApplication *> applications;
};

struct ConcreteBddNode {
  ConcreteVariableKey variable;
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  bool terminal = true;
};

struct ConcreteBddNodeKey {
  ConcreteVariableKey variable;
  std::uint32_t low = 0;
  std::uint32_t high = 0;

  bool operator<(const ConcreteBddNodeKey &other) const {
    if (variable < other.variable) {
      return true;
    }
    if (other.variable < variable) {
      return false;
    }
    return std::tie(low, high) < std::tie(other.low, other.high);
  }
};

static bool relationContainsTuple(Relation *relation,
                                  const std::vector<RamDomain> &fields);

class ConcreteWeightedBdd {
public:
  ConcreteWeightedBdd() {
    nodes.emplace_back();
    nodes.emplace_back();
  }

  std::uint32_t getFalse() const { return 0; }
  std::uint32_t getTrue() const { return 1; }

  std::uint32_t makeVariable(const ConcreteVariableKey &variable,
                             double weight) {
    auto [it, inserted] = variableWeights.emplace(variable, weight);
    if (!inserted && std::fabs(it->second - weight) > 1e-12) {
      throw std::runtime_error("multi-witness variable weight conflict");
    }
    return makeNode(variable, getFalse(), getTrue());
  }

  std::uint32_t makeAnd(std::uint32_t lhs, std::uint32_t rhs) {
    return apply(BddOperation::And, lhs, rhs);
  }

  std::uint32_t makeOr(std::uint32_t lhs, std::uint32_t rhs) {
    return apply(BddOperation::Or, lhs, rhs);
  }

  std::uint32_t makeNot(std::uint32_t node) {
    if (node == getFalse()) {
      return getTrue();
    }
    if (node == getTrue()) {
      return getFalse();
    }
    auto it = notCache.find(node);
    if (it != notCache.end()) {
      return it->second;
    }
    // Copy fields before recursing: makeNode may reallocate `nodes`.
    const auto variable = nodes.at(node).variable;
    const auto low = nodes.at(node).low;
    const auto high = nodes.at(node).high;
    const auto result = makeNode(variable, makeNot(low), makeNot(high));
    notCache.emplace(node, result);
    return result;
  }

  double wmc(std::uint32_t root) const {
    std::vector<double> memo(nodes.size(),
                             std::numeric_limits<double>::quiet_NaN());
    memo[getFalse()] = 0.0;
    memo[getTrue()] = 1.0;
    std::function<double(std::uint32_t)> visit = [&](std::uint32_t id) {
      if (!std::isnan(memo.at(id))) {
        return memo[id];
      }
      const auto &node = nodes.at(id);
      const auto weightIt = variableWeights.find(node.variable);
      if (weightIt == variableWeights.end()) {
        throw std::runtime_error("multi-witness BDD variable has no weight");
      }
      const double weight = weightIt->second;
      const double value = weight * visit(node.high) +
                           (1.0 - weight) * visit(node.low);
      memo[id] = value;
      return value;
    };
    return visit(root);
  }


private:
  std::uint32_t makeNode(const ConcreteVariableKey &variable,
                         std::uint32_t low, std::uint32_t high) {
    if (low == high) {
      return low;
    }
    ConcreteBddNodeKey key{variable, low, high};
    auto it = uniqueNodes.find(key);
    if (it != uniqueNodes.end()) {
      return it->second;
    }
    if (nodes.size() >= std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("multi-witness BDD node limit exceeded");
    }
    const auto id = static_cast<std::uint32_t>(nodes.size());
    ConcreteBddNode node;
    node.variable = variable;
    node.low = low;
    node.high = high;
    node.terminal = false;
    nodes.push_back(std::move(node));
    uniqueNodes.emplace(std::move(key), id);
    return id;
  }

  std::uint32_t apply(BddOperation operation, std::uint32_t lhs,
                      std::uint32_t rhs) {
    if (lhs > rhs) {
      std::swap(lhs, rhs);
    }
    if (operation == BddOperation::And) {
      if (lhs == getFalse() || rhs == getFalse()) {
        return getFalse();
      }
      if (lhs == getTrue()) {
        return rhs;
      }
      if (lhs == rhs) {
        return lhs;
      }
    } else {
      if (lhs == getTrue() || rhs == getTrue()) {
        return getTrue();
      }
      if (lhs == getFalse()) {
        return rhs;
      }
      if (lhs == rhs) {
        return lhs;
      }
    }

    const auto cacheKey = std::make_tuple(operation, lhs, rhs);
    auto cached = applyCache.find(cacheKey);
    if (cached != applyCache.end()) {
      return cached->second;
    }

    const auto &lhsNode = nodes.at(lhs);
    const auto &rhsNode = nodes.at(rhs);
    ConcreteVariableKey top;
    if (lhsNode.terminal) {
      top = rhsNode.variable;
    } else if (rhsNode.terminal || lhsNode.variable < rhsNode.variable) {
      top = lhsNode.variable;
    } else {
      top = rhsNode.variable;
    }

    auto cofactor = [&](std::uint32_t id, bool high) {
      const auto &node = nodes.at(id);
      if (!node.terminal && !(top < node.variable) && !(node.variable < top)) {
        return high ? node.high : node.low;
      }
      return id;
    };

    const auto low =
        apply(operation, cofactor(lhs, false), cofactor(rhs, false));
    const auto high =
        apply(operation, cofactor(lhs, true), cofactor(rhs, true));
    const auto result = makeNode(top, low, high);
    applyCache.emplace(cacheKey, result);
    return result;
  }

  std::vector<ConcreteBddNode> nodes;
  std::map<ConcreteBddNodeKey, std::uint32_t> uniqueNodes;
  std::map<std::tuple<BddOperation, std::uint32_t, std::uint32_t>,
           std::uint32_t>
      applyCache;
  std::map<std::uint32_t, std::uint32_t> notCache;
  std::map<ConcreteVariableKey, double> variableWeights;
};

static std::uint32_t instantiatePointwiseBdd(
    SouffleProgram &program,
    const std::unordered_map<UntypedTuple, double> &factProb,
    const AbstractDerivationGraph &graph, const SymbolicBdd &symbolic,
    BddId root, const std::vector<RamDomain> &rootValues,
    ConcreteWeightedBdd &concrete) {
  RuntimeBindingContext bindings(graph, rootValues);
  std::unordered_map<BddId, std::uint32_t> memo;
  memo.emplace(symbolic.getFalse(), concrete.getFalse());
  memo.emplace(symbolic.getTrue(), concrete.getTrue());

  auto concretize = [&](const SymbolicVariable &variable) {
    ConcreteVariableKey key;
    key.kind = variable.kind;
    if (variable.kind == SymbolicVariableKind::ExtensionalFact) {
      const auto &node = graph.getNode(variable.node);
      key.relation = node.relation;
      key.arguments = bindings.evaluateTerms(node.arguments);
    } else {
      const auto &edge = graph.getEdge(variable.edge);
      key.relation = edge.domain.headRelation;
      key.ruleId = edge.domain.ruleId;
      key.arguments = bindings.evaluateTerms(edge.domain.groundingKey);
    }
    return key;
  };

  std::function<std::uint32_t(BddId)> visit = [&](BddId nodeId) {
    const auto cached = memo.find(nodeId);
    if (cached != memo.end()) {
      return cached->second;
    }
    const auto &node = symbolic.getNode(nodeId);
    if (node.variable.kind == SymbolicVariableKind::RuleApplicationGuard) {
      const bool present = bindings.resolve(node.variable.edge) != nullptr;
      const auto result = visit(present ? node.high : node.low);
      memo.emplace(nodeId, result);
      return result;
    }

    const ConcreteVariableKey key = concretize(node.variable);
    double weight = 0.0;
    if (node.variable.kind == SymbolicVariableKind::ExtensionalFact) {
      UntypedTuple tuple{key.relation, key.arguments};
      const auto probability = factProb.find(tuple);
      if (probability != factProb.end()) {
        weight = probability->second;
      } else {
        weight = relationContainsTuple(program.getRelation(key.relation),
                                       key.arguments)
                     ? 1.0
                     : 0.0;
      }
    } else {
      weight = graph.getEdge(node.variable.edge).probability;
    }

    const auto low = visit(node.low);
    const auto high = visit(node.high);
    std::uint32_t result;
    if (weight <= 0.0) {
      result = low;
    } else if (weight >= 1.0) {
      result = high;
    } else {
      const auto variable = concrete.makeVariable(key, weight);
      result = concrete.makeOr(
          concrete.makeAnd(concrete.makeNot(variable), low),
          concrete.makeAnd(variable, high));
    }
    memo.emplace(nodeId, result);
    return result;
  };
  return visit(root);
}

// A conjunction of distinct concrete events has probability equal to their
// weight product. Repeated events and disjunctions use the exact BDD path.
static bool tryPointwiseConjunctionProduct(
    SouffleProgram &program,
    const std::unordered_map<UntypedTuple, double> &factProb,
    const AbstractDerivationGraph &graph, const SymbolicBdd &symbolic,
    BddId root, const std::vector<RamDomain> &rootValues, double &probability) {
  RuntimeBindingContext bindings(graph, rootValues);
  // Keyed and ordered exactly as ConcreteWeightedBdd orders its chain: the
  // root carries the smallest ConcreteVariableKey, and wmc() folds
  // w_min * (w_next * (... * (w_max * 1))).  Floating-point multiplication is
  // commutative but not associative, so folding in symbolic path order instead
  // of concrete key order differs in the last ulp on about 1 tuple in 17,000.
  // The map also detects a repeated concrete event, which a product would
  // double-count where the BDD absorbs it.
  std::map<ConcreteVariableKey, double> byKey;
  BddId current = root;
  while (true) {
    if (current == symbolic.getFalse()) {
      probability = 0.0;
      return true;
    }
    if (current == symbolic.getTrue()) {
      double acc = 1.0;
      for (auto it = byKey.rbegin(); it != byKey.rend(); ++it) {
        acc = it->second * acc;
      }
      probability = acc;
      return true;
    }
    const auto &node = symbolic.getNode(current);
    if (node.low != symbolic.getFalse()) {
      return false;  // a genuine disjunction; leave it to the BDD
    }
    if (node.variable.kind == SymbolicVariableKind::RuleApplicationGuard) {
      if (bindings.resolve(node.variable.edge) == nullptr) {
        probability = 0.0;
        return true;
      }
      current = node.high;
      continue;
    }

    ConcreteVariableKey key;
    key.kind = node.variable.kind;
    double weight = 0.0;
    if (node.variable.kind == SymbolicVariableKind::ExtensionalFact) {
      const auto &graphNode = graph.getNode(node.variable.node);
      key.relation = graphNode.relation;
      key.arguments = bindings.evaluateTerms(graphNode.arguments);
      UntypedTuple tuple{key.relation, key.arguments};
      const auto probabilityIt = factProb.find(tuple);
      if (probabilityIt != factProb.end()) {
        weight = probabilityIt->second;
      } else {
        weight = relationContainsTuple(program.getRelation(key.relation),
                                       key.arguments)
                     ? 1.0
                     : 0.0;
      }
    } else {
      const auto &edge = graph.getEdge(node.variable.edge);
      key.relation = edge.domain.headRelation;
      key.ruleId = edge.domain.ruleId;
      key.arguments = bindings.evaluateTerms(edge.domain.groundingKey);
      weight = edge.probability;
    }

    if (weight <= 0.0) {
      probability = 0.0;
      return true;
    }
    if (weight < 1.0) {
      if (!byKey.emplace(key, weight).second) {
        return false;  // repeated event; a product would double-count it
      }
    }
    current = node.high;
  }
}

static bool relationContainsTuple(Relation *relation,
                                  const std::vector<RamDomain> &fields) {
  if (relation == nullptr || relation->getArity() != fields.size()) {
    return false;
  }
  souffle::tuple probe(relation);
  for (std::size_t i = 0; i < fields.size(); ++i) {
    probe[i] = fields[i];
  }
  return relation->contains(probe);
}

} // namespace

LiftedWmcResult tryEvaluateLiftedPointwise(
    const CmdOptions &opt, SouffleProgram &program,
    const RuleManager &ruleManager,
    const std::unordered_map<UntypedTuple, double> &factProb,
    const std::vector<std::pair<UntypedTuple, bool>> &evidences) {
  using ProfileClock = std::chrono::steady_clock;
  const auto elapsedMs = [](ProfileClock::time_point start) {
    return std::chrono::duration<double, std::milli>(
               ProfileClock::now() - start)
        .count();
  };
  LiftedWmcResult result;
  result.executionMode = "none";
  if (!opt.isLiftedWmcEnabled()) {
    result.reason = "disabled";
    return result;
  }
  if (opt.isDerivationOnly()) {
    result.reason = "derivation_only";
    return result;
  }
  if (program.getKnowledge() != souffle::Knowledge::BDD) {
    result.reason = "non_bdd_backend";
    return result;
  }
  if (!evidences.empty()) {
    result.reason = "evidence_present";
    return result;
  }

  const auto outputs = program.getOutputRelations();
  if (outputs.empty()) {
    result.reason = "no_output_relations";
    return result;
  }
  std::size_t outputRelationCount = 0;
  for (const auto *output : outputs) {
    if (output != nullptr) {
      ++outputRelationCount;
      result.outputTuples += output->size();
    }
  }

  const auto eligibilityStart = ProfileClock::now();
  std::unordered_set<std::size_t> activeRuleIds;
  for (const auto &[tuple, applications] :
       DerivationManager::untypedTuple2RuleApplications) {
    (void)tuple;
    if (applications == nullptr) {
      continue;
    }
    for (const auto &application : *applications) {
      activeRuleIds.insert(static_cast<std::size_t>(application.ruleId));
    }
  }

  std::unordered_set<std::string> inputRelations;
  for (const auto *input : program.getInputRelations()) {
    if (input != nullptr) {
      inputRelations.insert(input->getName());
    }
  }

  std::vector<Relation *> liftedOutputs;
  std::unordered_set<std::string> relationTemplates;
  for (auto *output : outputs) {
    if (output == nullptr) {
      continue;
    }
    const std::string relation = output->getName();
    if (output->size() < opt.getLiftedWmcThreshold()) {
      result.rejectedOutputReasons.emplace(relation, "below_runtime_threshold");
      continue;
    }

    PointwiseEligibility eligibility(program, ruleManager, activeRuleIds);
    if (!eligibility.analyze({output})) {
      result.rejectedOutputReasons.emplace(relation,
                                           eligibility.failureReason());
      continue;
    }
    liftedOutputs.push_back(output);
    result.handledOutputRelations.push_back(relation);
    result.liftedOutputTuples += output->size();
    relationTemplates.insert(eligibility.relations().begin(),
                             eligibility.relations().end());
  }
  result.eligibilityMs = elapsedMs(eligibilityStart);
  result.concreteOutputTuples = result.outputTuples - result.liftedOutputTuples;
  if (liftedOutputs.empty()) {
    if (!result.rejectedOutputReasons.empty()) {
      result.reason = result.rejectedOutputReasons.begin()->second;
    } else {
      result.reason = "no_eligible_output_relations";
    }
    return result;
  }
  result.relationTemplates = relationTemplates.size();

  try {
    const auto graphStart = ProfileClock::now();
    AbstractDerivationGraph graph(ruleManager, activeRuleIds,
                                  std::move(inputRelations));
    std::map<std::string, AbstractNodeId> outputNodes;
    for (const auto *output : liftedOutputs) {
      if (output != nullptr) {
        outputNodes.emplace(output->getName(), graph.buildOutput(*output));
      }
    }
    result.abstractNodes = graph.nodeCount();
    result.abstractEdges = graph.edgeCount();
    if (opt.isDumpStatEnabled()) result.abstractGraph = graph.toTsv();
    if (opt.isDumpDotEnabled()) result.abstractGraphDot = graph.toDot();
    result.abstractGraphMs = elapsedMs(graphStart);

    const auto symbolicDdStart = ProfileClock::now();
    SymbolicBdd bdd;
    ParameterizedForwardCompiler compiler(graph, bdd);
    std::map<std::string, BddId> outputRoots;
    for (const auto *output : liftedOutputs) {
      if (output != nullptr) {
        outputRoots.emplace(output->getName(), compiler.compile(outputNodes.at(
                                                   output->getName())));
      }
    }
    result.symbolicDdMs = elapsedMs(symbolicDdStart);

    const auto instantiateStart = ProfileClock::now();
    for (const auto *output : liftedOutputs) {
      if (output == nullptr) {
        continue;
      }
      const BddId root = outputRoots.at(output->getName());
      for (auto it = output->begin(); it != output->end(); ++it) {
        const UntypedTuple tuple = UntypedTuple::fromSouffleTuple(*it);
        double probability = 0.0;
        if (tryPointwiseConjunctionProduct(program, factProb, graph, bdd,
                                           root, tuple.fields, probability)) {
          ++result.closedFormTuples;
        } else {
          ConcreteWeightedBdd concreteBdd;
          const auto concreteRoot = instantiatePointwiseBdd(
              program, factProb, graph, bdd, root, tuple.fields, concreteBdd);
          probability = concreteBdd.wmc(concreteRoot);
        }
        result.probabilities.emplace(tuple.toString(), probability);
      }
    }
    result.instantiateWmcMs = elapsedMs(instantiateStart);

    result.symbolicVariables = bdd.getVariables().size();
    result.bddNodes = bdd.nodeCount();
    result.handled = true;
    result.complete =
        result.handledOutputRelations.size() == outputRelationCount;
    result.executionMode = "pointwise";
    result.reason =
        result.complete ? "pointwise_lifted" : "partial_pointwise_lifted";
  } catch (const std::exception &error) {
    result.handled = false;
    result.complete = false;
    result.reason = std::string("lifted_runtime_fallback: ") + error.what();
    result.probabilities.clear();
    result.handledOutputRelations.clear();
    result.liftedOutputTuples = 0;
    result.closedFormTuples = 0;
    result.concreteOutputTuples = result.outputTuples;
  }
  return result;
}

} // namespace souffle::problog
