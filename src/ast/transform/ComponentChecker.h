/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2021, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file ComponentChecker.h
 *
 * Defines the component semantic checker pass.
 *
 ***********************************************************************/

#pragma once

#include "ast/Component.h"
#include "ast/ComponentInit.h"
#include "ast/ComponentType.h"
#include "ast/Program.h"
#include "ast/TranslationUnit.h"
#include "ast/analysis/ComponentLookup.h"
#include "ast/transform/Transformer.h"
#include "parser/SrcLocation.h"
#include "reports/ErrorReport.h"
#include <string>

namespace souffle::ast::transform {

class ComponentChecker : public Transformer {
public:
    ~ComponentChecker() override = default;

    std::string getName() const override {
        return "ComponentChecker";
    }

private:
    ComponentChecker* cloning() const override {
        return new ComponentChecker();
    }

    bool transform(TranslationUnit& translationUnit) override;

    static const Component* checkComponentNameReference(ErrorReport& report,
            const Component* enclosingComponent, const analysis::ComponentLookupAnalysis& componentLookup,
            const std::string& name, const SrcLocation& loc, const analysis::TypeBinding& binding);
    static void checkComponentReference(ErrorReport& report, const Component* enclosingComponent,
            const analysis::ComponentLookupAnalysis& componentLookup, const ast::ComponentType& type,
            const SrcLocation& loc, const analysis::TypeBinding& binding);
    static void checkComponentInit(ErrorReport& report, const Component* enclosingComponent,
            const analysis::ComponentLookupAnalysis& componentLookup, const ComponentInit& init,
            const analysis::TypeBinding& binding);
    static void checkComponent(ErrorReport& report, const Component* enclosingComponent,
            const analysis::ComponentLookupAnalysis& componentLookup, const Component& component,
            const analysis::TypeBinding& binding);
    static void checkComponents(ErrorReport& report, const Program& program,
            const analysis::ComponentLookupAnalysis& componentLookup);
    static void checkComponentNamespaces(ErrorReport& report, const Program& program);
};

}  // namespace souffle::ast::transform
