#pragma once

#include "ast/transform/Transformer.h"

namespace souffle::ast::transform {

class ProbQueryConstraintLiftingTransformer : public Transformer {
public:
    std::string getName() const override { return "ProbQueryConstraintLiftingTransformer"; }
    bool transform(TranslationUnit& tu) override;

    ProbQueryConstraintLiftingTransformer* cloning() const override {
        return new ProbQueryConstraintLiftingTransformer(*this);
    }
};

}
