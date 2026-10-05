#include "ast/transform/ProbQueryChecker.h"
#include "ast/BooleanConstraint.h"
#include "ast/ProbQuery.h"
#include "ast/Program.h"
#include "ast/TranslationUnit.h"
#include "reports/ErrorReport.h"

namespace souffle::ast::transform {
    bool ProbQueryChecker::transform(TranslationUnit& translationUnit) {
        auto& program = translationUnit.getProgram();

        for (auto&& probQueryPtr : program.getProbQueries()) {
            const ProbQuery* probQuery = probQueryPtr.get();
            checkProbQuery(probQuery, translationUnit);
        }
        return false;
    }

    bool ProbQueryChecker::checkProbQuery(const ProbQuery* probQuery, TranslationUnit& translationUnit) {
        auto& report = translationUnit.getErrorReport();
        auto& program = translationUnit.getProgram();

        auto relName = probQuery->getAtomName();
        auto rels = program.getRelationAll(relName);

        // Existence
        if (rels.empty()) {
            report.addError("Query Node Undeclared: " + relName.toString(), probQuery->getSrcLoc());
            return false;
        }

        // Arity
        Relation* rel = rels.front();
        const auto& args = probQuery->getArguments();
        if (args.size() != rel->getArity()) {
            report.addError("Arity doesn't match: " + relName.toString(), probQuery->getSrcLoc());
            return false;
        }
        return true;
    }
}