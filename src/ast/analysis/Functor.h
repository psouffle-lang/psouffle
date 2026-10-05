/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2020 The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Functor.h
 *
 * Analysis that provides type information for functors
 *
 ***********************************************************************/

#pragma once

#include "ast/FunctorDeclaration.h"
#include "ast/TranslationUnit.h"
#include "ast/UserDefinedAggregator.h"
#include "souffle/TypeAttribute.h"
#include <iosfwd>
#include <unordered_map>
#include <vector>

namespace souffle::ast {
class Functor;
class IntrinsicFunctor;
class Type;
class UserDefinedFunctor;
}  // namespace souffle::ast

namespace souffle::ast::analysis {

class TypeAnalysis;
class Type;

class FunctorAnalysis : public Analysis {
public:
    static constexpr const char* name = "functor-analysis";

    FunctorAnalysis() : Analysis(name) {}

    void run(const TranslationUnit& translationUnit) override;

    void print(std::ostream& /* os */) const override {}

    static bool isMultiResult(const Functor& functor);

    std::size_t getFunctorArity(UserDefinedFunctor const& functor) const;
    QualifiedName const& getFunctorReturnType(const UserDefinedFunctor& functor) const;
    bool isStatefulFunctor(const UserDefinedFunctor& functor) const;
    bool isStatefulFunctor(const UserDefinedAggregator& aggregator) const;
    const FunctorDeclaration& getFunctorDeclaration(const UserDefinedFunctor& functor) const;
    const FunctorDeclaration& getFunctorDeclaration(const UserDefinedAggregator& aggregator) const;

    /** Return whether a UDF is stateful */
    bool isStateful(const UserDefinedFunctor& udf) const;

private:
    std::unordered_map<std::string, const FunctorDeclaration*> functorNameToDeclaration;
};

}  // namespace souffle::ast::analysis
