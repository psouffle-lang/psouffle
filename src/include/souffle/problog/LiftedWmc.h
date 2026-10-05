#pragma once

#include "souffle/RamTypes.h"
#include "souffle/CompiledOptions.h"
#include "souffle/Derivation.h"
#include "souffle/SouffleInterface.h"
#include "souffle/problog/RuleManager.h"

#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace souffle::problog {

struct LiftedWmcResult {
    bool handled = false;
    bool complete = false;
    std::string reason;
    std::string executionMode;
    std::size_t outputTuples = 0;
    std::size_t liftedOutputTuples = 0;
    std::size_t concreteOutputTuples = 0;
    std::size_t relationTemplates = 0;
    std::size_t abstractNodes = 0;
    std::size_t abstractEdges = 0;
    std::size_t symbolicVariables = 0;
    std::size_t bddNodes = 0;
    std::size_t closedFormTuples = 0;
    double eligibilityMs = 0.0;
    double abstractGraphMs = 0.0;
    double symbolicDdMs = 0.0;
    double instantiateWmcMs = 0.0;
    std::vector<std::string> handledOutputRelations;
    std::map<std::string, std::string> rejectedOutputReasons;
    std::map<std::string, double> probabilities;
    std::string abstractGraph;
    std::string abstractGraphDot;
};

LiftedWmcResult tryEvaluateLiftedPointwise(const CmdOptions& opt, SouffleProgram& program,
        const RuleManager& ruleManager, const std::unordered_map<UntypedTuple, double>& factProb,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences);

}  // namespace souffle::problog
