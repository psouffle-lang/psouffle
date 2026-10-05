#include "ast/transform/ProbQueryConstraintLifting.h"

#include "Global.h"
#include "ast/Atom.h"
#include "ast/BinaryConstraint.h"
#include "ast/Clause.h"
#include "ast/Constant.h"
#include "ast/ProbQuery.h"
#include "ast/Program.h"
#include "ast/Relation.h"
#include "ast/TranslationUnit.h"
#include "ast/Variable.h"
#include "ast/analysis/SCCGraph.h"
#include "ast/utility/Utils.h"          // isEqConstraint(...)
#include "souffle/utility/StringUtil.h" // splitString etc (optional)

#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace souffle::ast::transform {

namespace {

// ---------- tiny dump helpers (replace souffle::toString) ----------
static std::string dump(const Node& n) {
    std::ostringstream os;
    os << n;
    return os.str();
}
static std::string dumpArg(const Argument* a) {
    if (a == nullptr) return "<null>";
    std::ostringstream os;
    os << *a;
    return os.str();
}

// ---------- logging ----------
static bool logEnabled(const TranslationUnit& tu) {
    const auto& cfg = tu.global().config();
    return cfg.has("debug-report") || cfg.has("verbose");
}
static void log(const TranslationUnit& tu, const std::string& msg) {
    if (!logEnabled(tu)) return;
    std::cerr << "[ProbQCL] " << msg << "\n";
}
static void warn([[maybe_unused]] const TranslationUnit& tu, const std::string& msg) {
    std::cerr << "[ProbQCL][warn] " << msg << "\n";
}

// ---------- query filter ----------
static bool isUnaryConstQuery(const Atom& a) {
    if (a.getArguments().size() != 1) return false;
    return isA<Constant>(a.getArguments()[0]);
}

// ---------- idempotency: already has (var = const) ? ----------
static bool alreadyHasEqConstraint(const Clause& clause, const std::string& varName, const Argument& c) {
    const std::string cKey = dump(c);

    for (const auto* lit : clause.getBodyLiterals()) {
        const auto* bc = as<BinaryConstraint>(lit);
        if (bc == nullptr) continue;
        if (!isEqConstraint(bc->getBaseOperator())) continue;

        const auto* lhsVar = as<ast::Variable>(bc->getLHS());
        if (lhsVar == nullptr) continue;
        if (lhsVar->getName() != varName) continue;

        const Argument* rhs = bc->getRHS();
        if (rhs == nullptr) continue;

        if (dump(*rhs) == cKey) return true;
    }
    return false;
}

}  // namespace

bool ProbQueryConstraintLiftingTransformer::transform(TranslationUnit& tu) {
    Program& program = tu.getProgram();
    const auto& queries = program.getProbQueries();

    if (queries.empty()) {
        log(tu, "no prob queries; skip.");
        return false;
    }

    const auto& scc = tu.getAnalysis<analysis::SCCGraphAnalysis>();

    // relationName -> one chosen constant
    std::unordered_map<std::string, const Argument*> rel2const;
    // relationName -> set of constants (stringified) for conflict detection
    std::unordered_map<std::string, std::unordered_set<std::string>> rel2allConsts;

    // (1) collect constraints from ProbQuery
    for (const auto& pq : queries) {
        const Atom& qa = pq->getAtom();

        if (!isUnaryConstQuery(qa)) {
            log(tu, "skip non-unary-const prob query: " + pq->toString());
            continue;
        }

        const QualifiedName& relName = qa.getQualifiedName();
        const Argument* c = qa.getArguments()[0];

        const std::string k = relName.toString();
        rel2allConsts[k].insert(dumpArg(c));
        if (!rel2const.count(k)) rel2const[k] = c;

        log(tu, "seen prob query constraint: " + k + " const=" + dumpArg(c));
    }

    bool changed = false;

    // (2) lift into defining clauses of selected relations
    for (const auto& [relKey, c] : rel2const) {
        const QualifiedName relName = QualifiedName::fromString(relKey);
        Relation* rel = program.getRelation(relName);
        if (rel == nullptr) {
            warn(tu, "relation not found for prob query: " + relKey);
            continue;
        }

        // conflict: multiple different constants for same relation (needs OR)
        const auto& allCs = rel2allConsts[relKey];
        if (allCs.size() > 1) {
            std::string cs;
            for (const auto& s : allCs) cs += (cs.empty() ? "" : ", ") + s;
            warn(tu, "multiple constants for " + relKey + " (need OR), skip. consts={" + cs + "}");
            continue;
        }

        // schema check
        if (rel->getArity() != 1) {
            warn(tu, "prob query is unary but relation arity != 1: rel=" + relKey +
                             " arity=" + std::to_string(rel->getArity()));
            continue;
        }

        // non-recursive + leaf SCC
        const std::size_t sccId = scc.getSCC(rel);
        if (scc.isRecursive(sccId)) {
            log(tu, "skip recursive rel " + relKey + " (scc=" + std::to_string(sccId) + ")");
            continue;
        }
        if (!scc.getSuccessorSCCs(sccId).empty()) {
            log(tu, "skip non-leaf SCC rel " + relKey + " (scc=" + std::to_string(sccId) +
                            ", succ=" + std::to_string(scc.getSuccessorSCCs(sccId).size()) + ")");
            continue;
        }

        if (c == nullptr) {
            warn(tu, "null constant? rel=" + relKey + " skip.");
            continue;
        }

        std::size_t injected = 0;
        std::size_t skippedNonVarHead = 0;
        std::size_t skippedDup = 0;

        // IMPORTANT: getClauses(relName) is a temporary vector<Clause*>
        for (Clause* clause : program.getClauses(relName)) {
            if (clause == nullptr) continue;

            const auto& headArgs = clause->getHead()->getArguments();
            if (headArgs.size() != 1) continue;

            const auto* v = as<ast::Variable>(headArgs[0]);
            if (v == nullptr) {
                // this means you are running before normaliseArguments (or someone introduced non-var head)
                skippedNonVarHead++;
                continue;
            }

            if (alreadyHasEqConstraint(*clause, v->getName(), *c)) {
                skippedDup++;
                continue;
            }

            clause->addToBody(mk<BinaryConstraint>(
                    BinaryConstraintOp::EQ,
                    mk<ast::Variable>(v->getName()),
                    clone(c)));


            injected++;
            changed = true;
        }

        log(tu, "rel=" + relKey + " injected=" + std::to_string(injected) +
                        " skippedDup=" + std::to_string(skippedDup) +
                        " skippedNonVarHead=" + std::to_string(skippedNonVarHead) +
                        " const=" + dumpArg(c));
        if (skippedNonVarHead > 0) {
            warn(tu, "some clauses had non-variable head args; ensure this pass runs AFTER argument normalisation.");
        }
    }

    if (changed) {
        tu.invalidateAnalyses();
        log(tu, "changed; analyses invalidated.");
    }
    return changed;
}

}  // namespace souffle::ast::transform
