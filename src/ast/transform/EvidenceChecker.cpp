//
// Created by 17308 on 2025/6/25.
//
#include "ast/transform/EvidenceChecker.h"

#include "ast/BooleanConstraint.h"
#include "ast/Evidence.h"
#include "ast/Program.h"
#include "ast/TranslationUnit.h"
#include "reports/ErrorReport.h"

namespace souffle::ast::transform {

bool EvidenceSemanticChecker::transform(TranslationUnit& translationUnit) {
    auto& program = translationUnit.getProgram();

    for (auto&& evidencePtr : program.getEvidences()) {
            const Evidence* evidence = evidencePtr.get();
            checkEvidence(evidence, translationUnit);
    }

    return false;
}

bool EvidenceSemanticChecker::checkEvidence(const Evidence* evidence,  TranslationUnit& translationUnit) {
    auto& report = translationUnit.getErrorReport();
    auto& program = translationUnit.getProgram();

    auto relName = evidence->getAtomName();
    auto rels = program.getRelationAll(relName);

    // Existence
    if (rels.empty()) {
        report.addError("Evidence Undeclared: " + relName.toString(), evidence->getSrcLoc());

        return false;
    }

    Relation* rel = rels.front();

    // Arity
    const auto& args = evidence->getArguments();
    if (args.size() != rel->getArity()) {
        report.addError("Arity doesn't match " + relName.toString(), evidence->getSrcLoc());

        return false;
    }

    return true;
}
};


