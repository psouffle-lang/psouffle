//
// Created by 17308 on 2025/6/25.
//
#include "ast/transform/EvidenceChecker.h"

#include "ast/BooleanConstraint.h"
#include "ast/Attribute.h"
#include "ast/Evidence.h"
#include "ast/NumericConstant.h"
#include "ast/Program.h"
#include "ast/StringConstant.h"
#include "ast/TranslationUnit.h"
#include "ast/analysis/typesystem/TypeEnvironment.h"
#include "reports/ErrorReport.h"
#include "souffle/utility/StringUtil.h"
#include <cmath>
#include <limits>

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

    const auto& env = translationUnit.getAnalysis<analysis::TypeEnvironmentAnalysis>().getTypeEnvironment();
    bool valid = true;
    const auto attributes = rel->getAttributes();
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& typeName = attributes[i]->getTypeName();
        if (!env.isType(typeName)) {
            report.addError("Unknown evidence argument type: " + typeName.toString(), args[i]->getSrcLoc());
            valid = false;
            continue;
        }
        const auto& type = env.getType(typeName);
        const auto* number = as<NumericConstant>(args[i]);
        bool matches = false;
        if (analysis::isBaseOfKind(type, TypeAttribute::Symbol)) {
            matches = isA<StringConstant>(args[i]);
        } else if (number) {
            const auto& text = number->getConstant();
            const auto fixedType = number->getFixedType();
            if (analysis::isBaseOfKind(type, TypeAttribute::Signed)) {
                matches = (!fixedType || *fixedType == NumericConstant::Type::Int) &&
                        canBeParsedAsRamSigned(text);
            } else if (analysis::isBaseOfKind(type, TypeAttribute::Unsigned)) {
                matches = (!fixedType || *fixedType == NumericConstant::Type::Uint) &&
                        canBeParsedAsRamUnsigned(text);
                if (matches) {
                    // RamUnsignedFromString narrows on 32-bit domains, so check
                    // the original value before code generation can wrap it.
                    const bool binary = isPrefix("0b", text);
                    const int base = binary ? 2 : (isPrefix("0x", text) ? 16 : 10);
                    matches = std::stoull(binary ? text.substr(2) : text, nullptr, base) <=
                            std::numeric_limits<RamUnsigned>::max();
                }
            } else if (analysis::isBaseOfKind(type, TypeAttribute::Float)) {
                matches = (!fixedType || *fixedType == NumericConstant::Type::Float) &&
                        canBeParsedAsRamFloat(text) && std::isfinite(RamFloatFromString(text));
            }
        }
        if (!matches) {
            report.addError("Evidence arguments must be ground primitive literals matching type " +
                    typeName.toString(), args[i]->getSrcLoc());
            valid = false;
        }
    }
    return valid;
}
};
