#pragma once

#include "ast/transform/Transformer.h"

namespace souffle::ast::transform {
    class ProbQueryChecker : public Transformer {
    public:
        ProbQueryChecker() = default;

        bool transform(TranslationUnit& translationUnit) override;

        std::string getName() const override {
            return "ProbQueryChecker";
        }

        Transformer* cloning() const override {
            return new ProbQueryChecker(*this);
        }
    private:
        bool checkProbQuery(const ProbQuery* probQuery, TranslationUnit& translationUnit);
    };
}