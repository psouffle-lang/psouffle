/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ConstantNormalizationTransformer.cpp
 *
 ***********************************************************************/

#include "ast/transform/ConstantNormalizationTransformer.h"
#include "ast/Aggregator.h"
#include "ast/Argument.h"
#include "ast/BinaryConstraint.h"
#include "ast/Clause.h"
#include "ast/Constant.h"
#include "ast/IntrinsicAggregator.h"
#include "ast/Literal.h"
#include "ast/Node.h"
#include "ast/Program.h"
#include "ast/RecordInit.h"
#include "ast/TranslationUnit.h"
#include "ast/UserDefinedAggregator.h"
#include "ast/Variable.h"
#include "ast/utility/Visitor.h"
#include "souffle/BinaryConstraintOps.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/MiscUtil.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace souffle::ast::transform {

bool ConstantNormalizationTransformer::transform(TranslationUnit& translationUnit) {
    using Constraints = VecOwn<BinaryConstraint>;

    Program& program = translationUnit.getProgram();

    // Replace constants with fresh variables and add equality constraints.
    struct constant_normaliser : public NodeMapper {
        Constraints& constraints;
        std::vector<std::string>& newVarNames;
        int& changeCount;

        constant_normaliser(Constraints& constraints, std::vector<std::string>& newVarNames, int& changeCount)
                : constraints(constraints), newVarNames(newVarNames), changeCount(changeCount) {}

        Own<Node> operator()(Own<Node> node) const override {
            if (isA<BinaryConstraint>(node)) {
                return node;
            }

            if (auto* aggr = as<Aggregator>(node)) {
                // Keep aggregator scope: collect constraints locally.
                Constraints subConstraints;
                std::vector<std::string> subNewVars;
                constant_normaliser aggrUpdate(subConstraints, subNewVars, changeCount);
                aggr->apply(aggrUpdate);

                VecOwn<Literal> newBodyLiterals;
                append(newBodyLiterals, cloneRange(aggr->getBodyLiterals()));
                append(newBodyLiterals, cloneRange(subConstraints));

                node = [&]() -> Own<Aggregator> {
                    if (auto* intrinsicAggr = as<IntrinsicAggregator>(aggr)) {
                        return mk<IntrinsicAggregator>(intrinsicAggr->getBaseOperator(),
                                (aggr->getTargetExpression() != nullptr ? clone(aggr->getTargetExpression())
                                                                        : nullptr),
                                std::move(newBodyLiterals));
                    } else {
                        auto* uda = as<UserDefinedAggregator>(aggr);
                        return mk<UserDefinedAggregator>(uda->getBaseOperatorName(), clone(uda->getInit()),
                                (aggr->getTargetExpression() != nullptr ? clone(aggr->getTargetExpression())
                                                                        : nullptr),
                                std::move(newBodyLiterals));
                    }
                }();
            } else {
                node->apply(*this);
            }

            if (auto* arg = as<Argument>(node)) {
                if (isA<Constant>(arg)) {
                    std::stringstream name;
                    name << "@const" << changeCount++;

                    newVarNames.push_back(name.str());
                    constraints.push_back(mk<BinaryConstraint>(
                            BinaryConstraintOp::EQ, mk<ast::Variable>(name.str()), clone(arg)));
                    return mk<ast::Variable>(name.str());
                }
            }
            return node;
        }
    };

    bool changed = false;
    for (auto* clause : program.getClauses()) {
        int changeCount = 0;
        Constraints constraintsToAdd;
        std::vector<std::string> newVarNames;
        constant_normaliser update(constraintsToAdd, newVarNames, changeCount);

        for (Literal* lit : clause->getBodyLiterals()) {
            if (isA<BinaryConstraint>(lit)) {
                continue;
            }
            lit->apply(update);
        }

        clause->addToBody(clone<Literal>(constraintsToAdd));
        if (!newVarNames.empty()) {
            auto vars = clause->getVariables();
            for (const auto& name : newVarNames) {
                if (std::find(vars.begin(), vars.end(), name) == vars.end()) {
                    vars.push_back(name);
                }
            }
            clause->setVariables(vars);
        }
        changed |= changeCount != 0;
    }

    return changed;
}

}  // namespace souffle::ast::transform
