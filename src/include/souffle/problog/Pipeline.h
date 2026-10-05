#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "souffle/Derivation.h"
#include "souffle/CompiledOptions.h"
#include "souffle/CompiledSouffle.h"

class RuleManager;
class QueryManager;

namespace souffle::problog {

std::string makeOutputPath(const CmdOptions& opt, const std::string& filename);

void runFullPipeline(const CmdOptions& opt, SouffleProgram& program,
        RuleManager& ruleManager, QueryManager& queryManager,
        const std::unordered_map<UntypedTuple, double>& factProb,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences);

void runPipeline(
        const CmdOptions& opt,
        SouffleProgram& program,
        RuleManager& ruleManager,
        QueryManager& queryManager,
        const std::unordered_map<UntypedTuple, double>& factProb,
        const std::vector<std::pair<UntypedTuple, bool>>& evidences,
        bool enableOnlineCli = false);

}  // namespace souffle::problog
