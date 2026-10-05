//
// Created by 17308 on 2025/6/25.
//

#pragma once

#include "ast/transform/Transformer.h"

namespace souffle::ast::transform {

class EvidenceSemanticChecker : public Transformer {
public:
    EvidenceSemanticChecker() = default;

    bool transform(TranslationUnit& translationUnit) override;

    std::string getName() const override {
        return "EvidenceSemanticChecker";
    }

    Transformer* cloning() const override {
        return new EvidenceSemanticChecker(*this);
    }

private:
    bool checkEvidence(const Evidence* evidence,  TranslationUnit& translationUnit);
};

}