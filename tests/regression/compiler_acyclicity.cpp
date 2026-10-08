#include "souffle/problog/RuleManager.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Rule rule(std::size_t id = 1, bool recursive = false, bool recursiveStratum = false,
        bool eqrel = false, std::vector<AggregateSpec> aggregates = {}) {
    return Rule(id, Atom("Head", {}), {Atom("Input", {})}, {}, 1.0,
            recursive, recursiveStratum, eqrel, std::move(aggregates));
}

void constructorAttestation() {
    require(!RuleManager({rule()}).hasCompilerAcyclicityCertificate(),
            "manually created rules acquired compiler attestation");
    require(!RuleManager({rule()}, {}, false).hasCompilerAcyclicityCertificate(),
            "explicitly untrusted rules acquired compiler attestation");
    require(RuleManager({rule()}, {}, true).hasCompilerAcyclicityCertificate(),
            "constructor population invalidated its compiler attestation");
    require(RuleManager({}, {}, true).hasCompilerAcyclicityCertificate(),
            "empty compiler-certified DAG was rejected");

    RuleManager original({rule()}, {}, true);
    RuleManager copy(original);
    copy.addRule(rule(2));
    require(original.hasCompilerAcyclicityCertificate() &&
                    !copy.hasCompilerAcyclicityCertificate(),
            "copied rule manager mutation affected the original certificate");
}

void structuralMutations() {
    RuleManager added({rule()}, {}, true);
    added.addRule(rule(2));
    require(!added.hasCompilerAcyclicityCertificate(), "adding a rule retained compiler attestation");

    RuleManager duplicate({rule()}, {}, true);
    duplicate.addRule(rule());
    require(!duplicate.hasCompilerAcyclicityCertificate(),
            "a public duplicate-rule update retained compiler attestation");

    RuleManager removed({rule()}, {}, true);
    require(removed.removeRule(1), "existing test rule was not removed");
    require(!removed.hasCompilerAcyclicityCertificate(),
            "removing a rule retained compiler attestation");

    RuleManager missing({rule()}, {}, true);
    require(!missing.removeRule(99), "missing test rule unexpectedly existed");
    require(!missing.hasCompilerAcyclicityCertificate(),
            "a public remove attempt retained compiler attestation");

    RuleManager eqrel({rule()}, {}, true);
    eqrel.addEqrelRelation("Equivalence");
    require(!eqrel.hasCompilerAcyclicityCertificate(),
            "adding eqrel metadata retained compiler attestation");
}

void recursionAndEqrelGuards() {
    // A base clause can be individually nonrecursive while its head relation
    // belongs to a mutually/self-recursive stratum.
    require(!RuleManager({rule(1, false, true)}, {}, true).hasCompilerAcyclicityCertificate(),
            "nonrecursive base clause of a recursive stratum was certified");
    require(!RuleManager({rule(1, true, false)}, {}, true).hasCompilerAcyclicityCertificate(),
            "recursive clause with inconsistent stratum metadata was certified");
    require(!RuleManager({rule(1, true, true)}, {}, true).hasCompilerAcyclicityCertificate(),
            "recursive rule was certified");
    require(!RuleManager({}, {"Equivalence"}, true).hasCompilerAcyclicityCertificate(),
            "input-only eqrel relation was certified");
    require(!RuleManager({rule(1, false, false, true)}, {}, true).hasCompilerAcyclicityCertificate(),
            "eqrel head without a separate relation declaration was certified");
}

void aggregateEncodingGuards() {
    const auto maximum = static_cast<std::size_t>(souffle::MAX_RAM_SIGNED);
    const std::vector<AggregateSpec> aggregates = {
            AggregateSpec("sum", "Sum", Atom("Witness", {}), SymbolicField(1))};
    require(RuleManager({rule(maximum, false, false, false, aggregates)}, {}, true)
                    .hasCompilerAcyclicityCertificate(),
            "largest representable aggregate rule identity was rejected");
    if (maximum < std::numeric_limits<std::size_t>::max()) {
        require(!RuleManager({rule(maximum + 1, false, false, false, aggregates)}, {}, true)
                        .hasCompilerAcyclicityCertificate(),
                "aggregate rule identity that wraps in RamDomain was certified");
    }
}

}  // namespace

int main() {
    try {
        constructorAttestation();
        structuralMutations();
        recursionAndEqrelGuards();
        aggregateEncodingGuards();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
