/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file CompiledOptions.h
 *
 * A header file offering command-line option support for compiled
 * RAM programs.
 *
 ***********************************************************************/

#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifdef USE_CUSTOM_GETOPTLONG
#include "souffle/utility/GetOptLongImpl.h"
#else
#include <getopt.h>
#endif

namespace souffle {

enum class SemMode {
    INC,
    FULL
};

enum class FcMode {
    FULL_HARD,
    INC_NAIVE,
    INC_REGIONAL
};

enum class WmcMode {
    FULL,
    INC_NAIVE,
    INC_REGIONAL
};

enum class FcConsumerClass {
    NORMALIZING,
    REGIONAL
};

enum class FcStateClass {
    NORMALIZED,
    REGIONALIZED,
    UNKNOWN
};

struct IncrementalModeSpec {
    SemMode sem = SemMode::INC;
    FcMode fc = FcMode::INC_NAIVE;
};

inline FcMode effectiveFcMode(FcMode fc) {
    return fc;
}

inline bool isIncrementalSemMode(SemMode sem) {
    return sem == SemMode::INC;
}

inline bool isFullSemMode(SemMode sem) {
    return sem == SemMode::FULL;
}

inline bool isIncrementalFcMode(FcMode fc) {
    fc = effectiveFcMode(fc);
    return fc == FcMode::INC_NAIVE || fc == FcMode::INC_REGIONAL;
}

inline bool isFullFcMode(FcMode fc) {
    fc = effectiveFcMode(fc);
    return fc == FcMode::FULL_HARD;
}

inline bool isRegionalFcMode(FcMode fc) {
    return effectiveFcMode(fc) == FcMode::INC_REGIONAL;
}

inline bool modeUsesIncrementalState(const IncrementalModeSpec& mode) {
    return isIncrementalSemMode(mode.sem) || isIncrementalFcMode(mode.fc);
}

inline WmcMode getWmcMode(FcMode fc) {
    switch (effectiveFcMode(fc)) {
        case FcMode::FULL_HARD:
            return WmcMode::FULL;
        case FcMode::INC_NAIVE:
            return WmcMode::INC_NAIVE;
        case FcMode::INC_REGIONAL:
            return WmcMode::INC_REGIONAL;
    }
    return WmcMode::FULL;
}

inline FcConsumerClass classifyFcConsumer(FcMode fc) {
    return effectiveFcMode(fc) == FcMode::INC_REGIONAL ? FcConsumerClass::REGIONAL
                                                       : FcConsumerClass::NORMALIZING;
}

inline bool isNormalizingFcConsumer(FcMode fc) {
    return classifyFcConsumer(fc) == FcConsumerClass::NORMALIZING;
}

inline const char* fcConsumerClassLabel(FcConsumerClass consumerClass) {
    switch (consumerClass) {
        case FcConsumerClass::NORMALIZING:
            return "C_NORM";
        case FcConsumerClass::REGIONAL:
            return "C_REG";
    }
    return "C_UNKNOWN";
}

inline const char* fcStateClassLabel(FcStateClass stateClass) {
    switch (stateClass) {
        case FcStateClass::NORMALIZED:
            return "N";
        case FcStateClass::REGIONALIZED:
            return "R";
        case FcStateClass::UNKNOWN:
            return "U";
    }
    return "U";
}

inline const char* semModeLabel(SemMode sem) {
    return sem == SemMode::FULL ? "SEM-FULL" : "SEM-INC";
}

inline const char* semModeTokenLabel(SemMode sem) {
    return sem == SemMode::FULL ? "full" : "inc";
}

inline const char* fcModeLabel(FcMode fc) {
    switch (fc) {
        case FcMode::FULL_HARD:
            return "FULL";
        case FcMode::INC_NAIVE:
            return "INC-NAIVE";
        case FcMode::INC_REGIONAL:
            return "INC-REGIONAL";
    }
    return "UNKNOWN";
}

inline const char* fcModeTokenLabel(FcMode fc) {
    switch (fc) {
        case FcMode::FULL_HARD:
            return "full";
        case FcMode::INC_NAIVE:
            return "inc-naive";
        case FcMode::INC_REGIONAL:
            return "inc-regional";
    }
    return "inc-naive";
}

inline const char* wmcModeLabel(WmcMode wmcMode) {
    switch (wmcMode) {
        case WmcMode::FULL:
            return "WMC-FULL";
        case WmcMode::INC_NAIVE:
            return "WMC-INC-NAIVE";
        case WmcMode::INC_REGIONAL:
            return "WMC-INC-REGIONAL";
    }
    return "WMC-UNKNOWN";
}

inline const char* incrementalModeOptionSyntax() {
    return "[ inc-naive | inc-regional | full | sem=<inc|full>,fc=<full|inc-naive|inc-regional> ]";
}

inline const char* incrementalModeHelpText() {
    return "inc-naive, inc-regional, full, or split specs sem=<inc|full>,fc=<full|inc-naive|inc-regional>";
}

inline const char* dumpKindsOptionSyntax() {
    return "[ json | json-before-graph | json-before-prune | dot | stat ]";
}

inline const char* profileStageOptionSyntax() {
    return "[ dred | inc | fc | wmc | inc-delete | inc-regional | dep-graph ]";
}

inline const char* incReorderPolicyOptionSyntax() {
    return "[ default | off | pressure | auto | explicit | both ]";
}

inline std::string trimModeSpecToken(const std::string& value);

inline bool parseLiftedThreshold(const std::string& token, std::size_t& value) {
    if (token.empty()) return false;
    std::size_t parsed = 0;
    for (const char digit : token) {
        if (digit < '0' || digit > '9') return false;
        const auto next = static_cast<std::size_t>(digit - '0');
        if (parsed > (std::numeric_limits<std::size_t>::max() - next) / 10) return false;
        parsed = parsed * 10 + next;
    }
    value = parsed;
    return true;
}

inline std::string joinOutputPath(const std::string& dir, const std::string& filename) {
    if (dir.empty()) {
        return filename;
    }
    if (dir.back() == '/') {
        return dir + filename;
    }
    return dir + "/" + filename;
}

inline std::string normalizeFlagToken(const std::string& raw) {
    const auto trimmed = trimModeSpecToken(raw);
    std::string out;
    out.reserve(trimmed.size());
    for (unsigned char c : trimmed) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline const char* fcProbabilityOutputSuffix(FcMode fc) {
    switch (effectiveFcMode(fc)) {
        case FcMode::FULL_HARD:
            return "-full";
        case FcMode::INC_NAIVE:
            return "-inc-naive";
        case FcMode::INC_REGIONAL:
            return "-inc-regional";
    }
    return "-unknown";
}

inline const char* incrementalFcProfileModeLabel(FcMode fc) {
    switch (effectiveFcMode(fc)) {
        case FcMode::FULL_HARD:
            return "full";
        case FcMode::INC_NAIVE:
            return "inc-naive";
        case FcMode::INC_REGIONAL:
            return "inc-regional";
    }
    return "unknown";
}

inline const char* fullSemProfileModeLabel(FcMode fc) {
    switch (effectiveFcMode(fc)) {
        case FcMode::INC_NAIVE:
            return "cli-full-inc-naive";
        case FcMode::INC_REGIONAL:
            return "cli-full-inc-regional";
        case FcMode::FULL_HARD:
            return "cli-full";
    }
    return "cli-full-unknown";
}

inline std::string modeSummaryLabel(const IncrementalModeSpec& mode) {
    std::ostringstream oss;
    oss << semModeLabel(mode.sem) << "+";
    oss << fcModeLabel(mode.fc);
    oss << "+" << wmcModeLabel(getWmcMode(mode.fc));
    return oss.str();
}

inline const char* debuggerTurnModeLabel(const IncrementalModeSpec& mode) {
    if (mode.sem == SemMode::FULL) {
        if (mode.fc == FcMode::FULL_HARD) {
            return "FULL";
        }
        return "FULL";
    }
    return "INC";
}

inline std::string onlineTurnProbabilityOutputSuffix(const IncrementalModeSpec& mode) {
    if (mode.sem == SemMode::FULL && isFullFcMode(mode.fc)) {
        return "-full";
    }
    return fcProbabilityOutputSuffix(mode.fc);
}

inline std::string makeOnlineTurnProbabilityPrefix(
        std::size_t iteration, const IncrementalModeSpec& mode) {
    return "fact-iter" + std::to_string(iteration) + onlineTurnProbabilityOutputSuffix(mode);
}

inline const char* incrementalModeTokenLabel(const IncrementalModeSpec& mode) {
    if (mode.sem == SemMode::FULL && mode.fc == FcMode::FULL_HARD) {
        return "full";
    }
    if (mode.sem == SemMode::INC && mode.fc == FcMode::INC_REGIONAL) {
        return "inc-regional";
    }
    return "inc-naive";
}

inline const char* incrementalSetModeUsageText() {
    return "setmode <inc-naive|inc-regional|full|sem=<inc|full> fc=<full|inc-naive|inc-regional>>";
}

inline bool parseModeToken(const std::string& token, IncrementalModeSpec& mode,
        std::string* canonicalToken = nullptr) {
    const std::string value = normalizeFlagToken(token);
    if (value == "inc-naive") {
        mode.sem = SemMode::INC;
        mode.fc = FcMode::INC_NAIVE;
        if (canonicalToken) {
            *canonicalToken = "inc-naive";
        }
        return true;
    }
    if (value == "inc-regional") {
        mode.sem = SemMode::INC;
        mode.fc = FcMode::INC_REGIONAL;
        if (canonicalToken) {
            *canonicalToken = "inc-regional";
        }
        return true;
    }
    if (value == "full") {
        mode.sem = SemMode::FULL;
        mode.fc = FcMode::FULL_HARD;
        if (canonicalToken) {
            *canonicalToken = "full";
        }
        return true;
    }
    return false;
}

inline bool parseSemModeToken(const std::string& token, SemMode& sem) {
    const std::string value = normalizeFlagToken(token);
    if (value == "inc" || value == "incremental") {
        sem = SemMode::INC;
        return true;
    }
    if (value == "full") {
        sem = SemMode::FULL;
        return true;
    }
    return false;
}

inline bool parseFcModeToken(const std::string& token, FcMode& fc) {
    const std::string value = normalizeFlagToken(token);
    if (value == "full" || value == "full-hard") {
        fc = FcMode::FULL_HARD;
        return true;
    }
    if (value == "inc" || value == "inc-naive" || value == "naive") {
        fc = FcMode::INC_NAIVE;
        return true;
    }
    if (value == "inc-regional" || value == "regional") {
        fc = FcMode::INC_REGIONAL;
        return true;
    }
    return false;
}

inline std::string canonicalIncrementalModeSpecToken(const IncrementalModeSpec& mode) {
    if (mode.sem == SemMode::INC && mode.fc == FcMode::INC_NAIVE) {
        return "inc-naive";
    }
    if (mode.sem == SemMode::INC && mode.fc == FcMode::INC_REGIONAL) {
        return "inc-regional";
    }
    if (mode.sem == SemMode::FULL && mode.fc == FcMode::FULL_HARD) {
        return "full";
    }
    std::ostringstream oss;
    oss << "sem=" << semModeTokenLabel(mode.sem) << ",fc=" << fcModeTokenLabel(mode.fc);
    return oss.str();
}

inline bool parseDumpKindToken(const std::string& token, std::string& kind,
        std::string* canonicalToken = nullptr) {
    const std::string value = normalizeFlagToken(token);
    if (value == "json" || value == "json-before-graph" || value == "json-before-prune" ||
            value == "dot" || value == "stat") {
        kind = value;
        if (canonicalToken) {
            *canonicalToken = value;
        }
        return true;
    }
    return false;
}

inline bool parseProfileStageToken(const std::string& token, std::string& stage,
        std::string* canonicalToken = nullptr) {
    const std::string value = normalizeFlagToken(token);
    if (value == "dred" || value == "inc" || value == "fc" || value == "wmc" ||
            value == "inc-delete" || value == "inc-regional" || value == "dep-graph") {
        stage = value;
        if (canonicalToken) {
            *canonicalToken = value;
        }
        return true;
    }
    return false;
}

inline bool parseIncReorderPolicyToken(const std::string& token, std::string& policy) {
    const std::string value = normalizeFlagToken(token);
    if (value == "default" || value == "off" || value == "pressure" || value == "auto" ||
            value == "explicit" || value == "both") {
        policy = value;
        return true;
    }
    return false;
}

inline std::string trimModeSpecToken(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline void appendSplitModeSpecs(std::vector<std::string>& specs, const std::string& token) {
    std::size_t begin = 0;
    while (begin <= token.size()) {
        const std::size_t comma = token.find(',', begin);
        const std::size_t end = (comma == std::string::npos) ? token.size() : comma;
        const std::string spec = trimModeSpecToken(token.substr(begin, end - begin));
        if (!spec.empty()) {
            specs.push_back(spec);
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
}

inline bool parseIncrementalModeSpecs(const std::vector<std::string>& specs,
        const IncrementalModeSpec& current, IncrementalModeSpec& next, std::string* error = nullptr,
        std::string* canonicalToken = nullptr) {
    if (specs.empty()) {
        if (error) {
            *error = "empty mode spec";
        }
        return false;
    }

    next = current;
    if (specs.size() == 1 && specs[0].find('=') == std::string::npos) {
        if (!parseModeToken(specs[0], next)) {
            if (error) {
                *error = "Unknown mode: " + specs[0];
            }
            return false;
        }
        if (canonicalToken) {
            *canonicalToken = canonicalIncrementalModeSpecToken(next);
        }
        return true;
    }

    bool sawSem = false;
    bool sawFc = false;
    for (const auto& spec : specs) {
        const std::size_t eq = spec.find('=');
        if (eq == std::string::npos) {
            if (error) {
                *error = "Split mode item must use key=value: " + spec;
            }
            return false;
        }
        const std::string key = normalizeFlagToken(spec.substr(0, eq));
        const std::string value = trimModeSpecToken(spec.substr(eq + 1));
        if (value.empty()) {
            if (error) {
                *error = "Empty value in mode spec: " + spec;
            }
            return false;
        }
        if (key == "sem" || key == "semantic") {
            if (sawSem) {
                if (error) {
                    *error = "Duplicate sem mode spec";
                }
                return false;
            }
            if (!parseSemModeToken(value, next.sem)) {
                if (error) {
                    *error = "Unknown sem mode: " + value;
                }
                return false;
            }
            sawSem = true;
            continue;
        }
        if (key == "fc" || key == "bdd") {
            if (sawFc) {
                if (error) {
                    *error = "Duplicate fc mode spec";
                }
                return false;
            }
            if (!parseFcModeToken(value, next.fc)) {
                if (error) {
                    *error = "Unknown fc mode: " + value;
                }
                return false;
            }
            sawFc = true;
            continue;
        }
        if (error) {
            *error = "Unknown mode spec key: " + key;
        }
        return false;
    }

    if (!sawSem && !sawFc) {
        if (error) {
            *error = "setmode expects one mode or split specs: " + std::string(incrementalModeHelpText());
        }
        return false;
    }
    if (canonicalToken) {
        *canonicalToken = canonicalIncrementalModeSpecToken(next);
    }
    return true;
}

/**
 * A utility class for parsing command line arguments within generated
 * query programs.
 */
class CmdOptions {
protected:
    /**
     * source file
     */
    std::string src;

    /**
     * fact directory
     */
    std::string input_dir;

    /**
     * output directory
     */
    std::string output_dir;

    /**
     * profiling flag
     */
    bool profiling;

    /**
     * profile filename
     */
    std::string profile_name;

    /**
     * number of threads
     */
    std::size_t num_jobs;
    std::string log_file_name = "log.txt";  // default log file name
    bool dump_json = false;  // dump derivation graph JSON after prune
    bool dump_json_before_graph = false;  // dump ruleapp-reconstructed JSON before graph materialization
    bool dump_json_before_prune = false;  // dump derivation graph JSON before prune
    bool dump_dot = false;  // dump derivation graph DOT after prune
    bool dump_stat = false;  // dump derivation graph stats after prune
    bool dred_profile = false;  // enable detailed DRed profiling
    bool inc_profile = false;  // enable incremental stage profiling
    bool fc_profile = false;  // enable detailed forward-compilation profiling
    bool inc_delete_profile = false;  // enable delete-phase profiling for incremental FC
    bool wmc_profile = false;  // enable weighted model counting profiling
    bool inc_regional_profile = false;  // enable inc-regional profiling/diagnostics
    bool dep_graph_profile = false;  // enable dependency-graph profiling
    bool verbose = false;  // enable informational runtime output
    std::string inc_reorder_policy = "pressure";
    std::size_t inc_reorder_auto_gap = 0;
    std::size_t inc_reorder_work_threshold = 2500;
    bool inc_reorder_count_dead = false;
    bool inc_reorder_allow_large = false;
    bool full_capable = true;
    bool inc_capable = true;
    bool online_execution = false;
    bool enable_rewrite = false;
    bool force_graph_rewrite = false;
    bool force_implicit_rewrite = false;
    bool det_opt = true;
    bool help_requested = false;
    bool derivation_only = false;
    bool merge_bi_imp = false;
    bool prune_extra = false;
    bool lifted_wmc = false;
    std::size_t lifted_threshold = 1024;
public:
    bool isLiftedWmcEnabled() const { return lifted_wmc; }
    void setLiftedWmcEnabled(bool enabled) { lifted_wmc = enabled; }
    std::size_t getLiftedWmcThreshold() const { return lifted_threshold; }
    void setLiftedWmcThreshold(std::size_t threshold) { lifted_threshold = threshold; }
    // all argument constructor
    CmdOptions(const char* s, const char* id, const char* od, bool pe, const char* pfn, std::size_t nj,
            std::string lfn = "log.txt", const std::string& mode = "inc-naive",
            bool dumpjson = false, bool dumpjsonBeforeGraph = false, bool dumpjsonBeforePrune = false,
            bool dumpdot = false, bool dumpstat = false,
            bool dredProfile = false,
            bool incProfile = false,
            bool fcProfile = false,
            bool incDeleteProfile = false,
            bool wmcProfile = false,
            bool incRegionalProfile = false,
            bool depGraphProfile = false,
            bool verboseOutput = false)
            : src(s), input_dir(id), output_dir(od), profiling(pe), profile_name(pfn), num_jobs(nj), log_file_name(lfn)
    , dump_json(dumpjson), dump_json_before_graph(dumpjsonBeforeGraph),
      dump_json_before_prune(dumpjsonBeforePrune),
      dump_dot(dumpdot), dump_stat(dumpstat),
      dred_profile(dredProfile), inc_profile(incProfile), fc_profile(fcProfile),
      inc_delete_profile(incDeleteProfile),
      wmc_profile(wmcProfile),
      inc_regional_profile(incRegionalProfile),
      dep_graph_profile(depGraphProfile),
      verbose(verboseOutput) {
        setIncrementalMode(mode);
    }

    CmdOptions() {}
    void setExecutionCapabilities(bool full, bool inc, bool onlineDefault) {
        full_capable = full;
        inc_capable = inc;
        online_execution = onlineDefault;
    }
    bool isOnlineExecution() const { return online_execution; }
    bool isRewriteEnabled() const { return enable_rewrite; }
    bool isGraphRewriteForced() const { return force_graph_rewrite; }
    bool isImplicitRewriteForced() const { return force_implicit_rewrite; }
    void setRewriteDefaults(bool rewrite, bool explicitRewrite, bool implicitRewrite) {
        enable_rewrite = rewrite || explicitRewrite || implicitRewrite;
        force_graph_rewrite = explicitRewrite;
        force_implicit_rewrite = implicitRewrite;
    }
    bool isDetOptEnabled() const { return det_opt; }
    bool isHelpRequested() const { return help_requested; }
    void setDerivationOnly(bool enabled) { derivation_only = enabled; }
    bool isDerivationOnly() const { return derivation_only; }
    bool isMergeBiImpEnabled() const { return merge_bi_imp; }
    bool isPruneExtraEnabled() const { return prune_extra; }
    bool isSingleRandFastEnabled() const { return true; }
    bool isForceFullSisoDetectEnabled() const { return false; }
    bool isRelaxCompactionDirtyEnabled() const { return true; }
    /**
     * get source code name
     */
    const std::string& getSourceFileName() const {
        return src;
    }

    /**
     * get input directory
     */
    const std::string& getInputFileDir() const {
        return input_dir;
    }

    /**
     * get output directory
     */
    const std::string& getOutputFileDir() const {
        return output_dir;
    }
    const std::string& getLogFileName() const {
        return log_file_name;
    }

    std::string incMode = "inc-naive";
    const std::string& getIncMode() const {
        return incMode;
    }
    const IncrementalModeSpec& getIncrementalModeSpec() const {
        return incModeSpec;
    }
    bool setIncrementalMode(const std::string& token) {
        IncrementalModeSpec parsed = incModeSpec;
        std::vector<std::string> specs;
        appendSplitModeSpecs(specs, token);
        std::string canonical;
        if (!parseIncrementalModeSpecs(specs, incModeSpec, parsed, nullptr, &canonical)) {
            return false;
        }
        setIncrementalModeSpec(parsed, canonical);
        return true;
    }
    void setIncrementalModeSpec(const IncrementalModeSpec& mode,
            const std::string& canonicalToken = "") {
        incModeSpec = mode;
        if (!canonicalToken.empty()) {
            incMode = canonicalToken;
            return;
        }
        incMode = incrementalModeTokenLabel(mode);
    }
    void setInputFileDir(std::string dir) {
        input_dir = std::move(dir);
    }
    void setOutputFileDir(std::string dir) {
        output_dir = std::move(dir);
    }
    void setLogFileName(std::string name) {
        log_file_name = std::move(name);
    }
    /**
     * is profiling switched on
     */
    bool isProfiling() const {
        return profiling;
    }

    bool isDumpJsonEnabled() const {
        return dump_json;
    }
    void setDumpJsonEnabled(bool enabled) {
        dump_json = enabled;
    }
    bool isDumpJsonBeforeGraphEnabled() const {
        return dump_json_before_graph;
    }
    void setDumpJsonBeforeGraphEnabled(bool enabled) {
        dump_json_before_graph = enabled;
    }
    bool isDumpJsonBeforePruneEnabled() const {
        return dump_json_before_prune;
    }
    void setDumpJsonBeforePruneEnabled(bool enabled) {
        dump_json_before_prune = enabled;
    }
    bool isDumpDotEnabled() const {
        return dump_dot;
    }
    void setDumpDotEnabled(bool enabled) {
        dump_dot = enabled;
    }
    bool isDumpStatEnabled() const {
        return dump_stat;
    }
    void setDumpStatEnabled(bool enabled) {
        dump_stat = enabled;
    }
    bool setDumpKindToken(const std::string& token, bool enabled) {
        std::string kind;
        if (!parseDumpKindToken(token, kind)) {
            return false;
        }
        if (kind == "json") {
            dump_json = enabled;
        } else if (kind == "json-before-graph") {
            dump_json_before_graph = enabled;
        } else if (kind == "json-before-prune") {
            dump_json_before_prune = enabled;
        } else if (kind == "dot") {
            dump_dot = enabled;
        } else if (kind == "stat") {
            dump_stat = enabled;
        }
        return true;
    }
    bool isDredProfileEnabled() const {
        return dred_profile;
    }
    bool isIncProfileEnabled() const {
        return inc_profile;
    }
    bool isFcProfileEnabled() const {
        return fc_profile;
    }
    bool isIncDeleteProfileEnabled() const {
        return inc_delete_profile;
    }
    bool isWmcProfileEnabled() const {
        return wmc_profile;
    }
    bool isIncRegionalProfileEnabled() const {
        return inc_regional_profile;
    }
    bool isDepGraphProfileEnabled() const {
        return dep_graph_profile;
    }
    bool isVerboseEnabled() const {
        return verbose;
    }
    void setVerboseEnabled(bool enabled) {
        verbose = enabled;
    }
    const std::string& getIncReorderPolicy() const {
        return inc_reorder_policy;
    }
    bool setIncReorderPolicy(const std::string& token) {
        std::string parsed;
        if (!parseIncReorderPolicyToken(token, parsed)) {
            return false;
        }
        inc_reorder_policy = parsed;
        return true;
    }
    std::size_t getIncReorderAutoGap() const {
        return inc_reorder_auto_gap;
    }
    void setIncReorderAutoGap(std::size_t gap) {
        inc_reorder_auto_gap = gap;
    }
    std::size_t getIncReorderWorkThreshold() const {
        return inc_reorder_work_threshold;
    }
    void setIncReorderWorkThreshold(std::size_t threshold) {
        inc_reorder_work_threshold = threshold;
    }
    bool isIncReorderCountDeadEnabled() const {
        return inc_reorder_count_dead;
    }
    void setIncReorderCountDeadEnabled(bool enabled) {
        inc_reorder_count_dead = enabled;
    }
    bool isIncReorderAllowLargeEnabled() const {
        return inc_reorder_allow_large;
    }
    void setIncReorderAllowLargeEnabled(bool enabled) {
        inc_reorder_allow_large = enabled;
    }
    bool setProfileStageToken(const std::string& token, bool enabled) {
        std::string stage;
        if (!parseProfileStageToken(token, stage)) {
            return false;
        }
        if (stage == "dred") {
            dred_profile = enabled;
        } else if (stage == "inc") {
            inc_profile = enabled;
        } else if (stage == "fc") {
            fc_profile = enabled;
        } else if (stage == "wmc") {
            wmc_profile = enabled;
        } else if (stage == "inc-delete") {
            inc_delete_profile = enabled;
        } else if (stage == "inc-regional") {
            inc_regional_profile = enabled;
        } else if (stage == "dep-graph") {
            dep_graph_profile = enabled;
        }
        return true;
    }
    std::vector<std::string> getEnabledDumpKinds() const {
        std::vector<std::string> kinds;
        if (dump_json) kinds.push_back("json");
        if (dump_json_before_graph) kinds.push_back("json-before-graph");
        if (dump_json_before_prune) kinds.push_back("json-before-prune");
        if (dump_dot) kinds.push_back("dot");
        if (dump_stat) kinds.push_back("stat");
        return kinds;
    }
    std::vector<std::string> getEnabledProfileStages() const {
        std::vector<std::string> stages;
        if (dred_profile) stages.push_back("dred");
        if (inc_profile) stages.push_back("inc");
        if (fc_profile) stages.push_back("fc");
        if (wmc_profile) stages.push_back("wmc");
        if (inc_delete_profile) stages.push_back("inc-delete");
        if (inc_regional_profile) stages.push_back("inc-regional");
        if (dep_graph_profile) stages.push_back("dep-graph");
        return stages;
    }
    /**
     * get filename of profile
     */
    const std::string& getProfileName() const {
        return profile_name;
    }

    /**
     * get number of jobs
     */
    std::size_t getNumJobs() const {
        return num_jobs;
    }

    /**
     * Parses the given command line parameters, handles -h help requests or errors
     * and returns whether the parsing was successful or not.
     */
    bool parse(int argc, char** argv) {
        // get executable name
        std::string exec_name = "analysis";
        if (argc > 0) {
            exec_name = argv[0];
        }

        // local options
        std::string fact_dir = input_dir;
        std::string out_dir = output_dir;

        // long options
        option longOptions[] = {{"facts", true, nullptr, 'F'}, {"input-dir", true, nullptr, 'F'},
                {"output", true, nullptr, 'D'}, {"output-dir", true, nullptr, 'D'},
                {"profile", true, nullptr, 'p'},
                {"jobs", true, nullptr, 'j'},
                {"logfile", true, nullptr, 'l'}, {"log-file", true, nullptr, 'l'},
                {"setmode", true, nullptr, 'm'},
                {"dump", true, nullptr, 1032},
                {"profile-stage", true, nullptr, 1033},
                {"inc-reorder-policy", true, nullptr, 1034},
                {"inc-reorder-auto-gap", true, nullptr, 1035},
                {"inc-reorder-work-threshold", true, nullptr, 1036},
                {"inc-reorder-count-dead", false, nullptr, 1037},
                {"inc-reorder-allow-large", false, nullptr, 1038},
                {"verbose", false, nullptr, 'v'},
                {"help", false, nullptr, 'h'},
                {"full-only", false, nullptr, 1040},
                {"inc-only", false, nullptr, 1041},
                {"online", false, nullptr, 1041},
                {"rewrite", false, nullptr, 'r'},
                {"explicit-rewrite", false, nullptr, 1042},
                {"implicit-rewrite", false, nullptr, 1043},
                {"det-opt", false, nullptr, 'Z'},
                {"dumpjson", false, nullptr, 1044},
                {"dumpdot", false, nullptr, 1045},
                {"dumpstat", false, nullptr, 1046},
                {"fc-profile", false, nullptr, 1047},
                {"profile-wmc", false, nullptr, 1048},
                {"profile-dep-graph", false, nullptr, 1049},
                {"derv-only", optional_argument, nullptr, 'd'},
                {"merge-bi-imp", false, nullptr, 'e'},
                {"prune-extra", false, nullptr, 1050},
                {"lifted-wmc", false, nullptr, 1051},
                {"lifted-threshold", true, nullptr, 1052},
                // the terminal option -- needs to be null
                {nullptr, false, nullptr, 0}};

        // check whether all options are fine
        bool ok = true;
        bool requestedFull = false;
        bool requestedInc = false;
        bool requestedMode = false;
        int c; /* command-line arguments processing */
        while ((c = getopt_long(argc, argv, "D:F:hp:j:m:vrZd::eJTSl:", longOptions, nullptr)) != EOF) {
            switch (c) {
                case 'h':
                    help_requested = true;
                    printHelpPage(exec_name);
                    return false;
                /* Fact directories */
                case 'F':
                    if (!existDir(optarg)) {
                        printf("Fact directory %s does not exists!\n", optarg);
                        ok = false;
                    }
                    fact_dir = optarg;
                    break;
                /* Output directory for resulting .csv files */
                case 'D':
                    if (*optarg && !existDir(optarg) && !dirIsStdout(optarg)) {
                        printf("Output directory %s does not exists!\n", optarg);
                        ok = false;
                    }
                    out_dir = optarg;
                    break;
                case 'p':
                    if (!profiling) {
                        std::cerr << "\nError: profiling was not enabled in compilation\n\n";
                        printHelpPage(exec_name);
                        exit(EXIT_FAILURE);
                    }
                    profile_name = optarg;
                    break;
                case 'j':
#ifdef _OPENMP
                    if (std::string(optarg) == "auto") {
                        num_jobs = 0;
                    } else {
                        int num = atoi(optarg);
                        if (num > 0) {
                            num_jobs = num;
                        } else {
                            std::cerr << "Invalid number of jobs [-j]: " << optarg << "\n";
                            ok = false;
                        }
                    }
#else
                    std::cerr << "\nWarning: OpenMP was not enabled in compilation\n\n";
#endif
                    break;
                case 'l':
                    if (*optarg)
                        log_file_name = optarg;
                    else {
                        log_file_name = "log";
                    }
                    break;
                case 'm': {
                    requestedMode = true;
                    IncrementalModeSpec parsed = incModeSpec;
                    std::string error;
                    std::string canonical;
                    std::vector<std::string> specs;
                    appendSplitModeSpecs(specs, optarg);
                    if (!parseIncrementalModeSpecs(specs, currentIncrementalModeSpec(), parsed, &error, &canonical)) {
                        std::cerr << "Invalid incremental mode [-m]: " << optarg << "\n";
                        ok = false;
                    } else {
                        setIncrementalModeSpec(parsed, canonical);
                    }
                    break;
                }
                case 1032: {
                    std::vector<std::string> specs;
                    appendSplitModeSpecs(specs, optarg);
                    if (specs.empty()) {
                        std::cerr << "Invalid dump kind [--dump]: " << optarg << "\n";
                        ok = false;
                        break;
                    }
                    for (const auto& spec : specs) {
                        if (!setDumpKindToken(spec, true)) {
                            std::cerr << "Invalid dump kind [--dump]: " << spec << "\n";
                            ok = false;
                        }
                    }
                    break;
                }
                case 1033: {
                    std::vector<std::string> specs;
                    appendSplitModeSpecs(specs, optarg);
                    if (specs.empty()) {
                        std::cerr << "Invalid profile stage [--profile-stage]: " << optarg << "\n";
                        ok = false;
                        break;
                    }
                    for (const auto& spec : specs) {
                        if (!setProfileStageToken(spec, true)) {
                            std::cerr << "Invalid profile stage [--profile-stage]: " << spec << "\n";
                            ok = false;
                        }
                    }
                    break;
                }
                case 1034:
                    if (!setIncReorderPolicy(optarg)) {
                        std::cerr << "Invalid inc reorder policy [--inc-reorder-policy]: "
                                  << optarg << "\n";
                        ok = false;
                    }
                    break;
                case 1035: {
                    char* end = nullptr;
                    unsigned long long parsed = std::strtoull(optarg, &end, 10);
                    if (!optarg[0] || (end && *end)) {
                        std::cerr << "Invalid inc reorder auto gap [--inc-reorder-auto-gap]: "
                                  << optarg << "\n";
                        ok = false;
                    } else {
                        inc_reorder_auto_gap = static_cast<std::size_t>(parsed);
                    }
                    break;
                }
                case 1036: {
                    char* end = nullptr;
                    unsigned long long parsed = std::strtoull(optarg, &end, 10);
                    if (!optarg[0] || (end && *end)) {
                        std::cerr << "Invalid inc reorder work threshold [--inc-reorder-work-threshold]: "
                                  << optarg << "\n";
                        ok = false;
                    } else {
                        inc_reorder_work_threshold = static_cast<std::size_t>(parsed);
                    }
                    break;
                }
                case 1037:
                    inc_reorder_count_dead = true;
                    break;
                case 1038:
                    inc_reorder_allow_large = true;
                    break;
                case 'v':
                    verbose = true;
                    break;
                case 1040: requestedFull = true; break;
                case 1041: requestedInc = true; break;
                case 'r': enable_rewrite = true; break;
                case 1042: enable_rewrite = true; force_graph_rewrite = true; break;
                case 1043: enable_rewrite = true; force_implicit_rewrite = true; break;
                case 'Z': det_opt = true; break;
                case 1044: dump_json = true; break;
                case 1045: dump_dot = true; break;
                case 1046: dump_stat = true; break;
                case 1047: fc_profile = true; break;
                case 1048: wmc_profile = true; break;
                case 1049: dep_graph_profile = true; break;
                case 'J': dump_json = true; break;
                case 'T': dump_dot = true; break;
                case 'S': dump_stat = true; break;
                case 'e': merge_bi_imp = true; break;
                case 1050: prune_extra = true; break;
                case 1051: lifted_wmc = true; break;
                case 1052:
                    if (!parseLiftedThreshold(optarg, lifted_threshold)) {
                        std::cerr << "Invalid value for --lifted-threshold: " << optarg << '\n';
                        ok = false;
                    }
                    break;
                case 'd':
                    if (!optarg || std::string(optarg) == "true") {
                        derivation_only = true;
                    } else if (std::string(optarg) == "false") {
                        derivation_only = false;
                    } else {
                        std::cerr << "Invalid value for --derv-only: " << optarg << '\n';
                        ok = false;
                    }
                    break;
                default: printHelpPage(exec_name); return false;
            }
        }

        // update member fields
        if (requestedFull && (requestedInc || requestedMode)) {
            std::cerr << "Cannot combine --full-only with --inc-only, --online, or --setmode\n";
            ok = false;
        }
        if (requestedFull) online_execution = false;
        if (requestedInc || requestedMode) online_execution = true;
        if ((online_execution && !inc_capable) || (!online_execution && !full_capable)) {
            std::cerr << "Requested execution path was not generated by the compiler\n";
            ok = false;
        }
        if (online_execution && enable_rewrite) {
            std::cerr << "Rewrite is supported only in standalone full execution; online baseline and recomputation use the original graph\n";
            ok = false;
        }
        if (online_execution && lifted_wmc) {
            std::cerr << "--lifted-wmc requires standalone full execution\n";
            ok = false;
        }
        if (online_execution && (derivation_only || merge_bi_imp || prune_extra)) {
            std::cerr << "--derv-only, --merge-bi-imp, and --prune-extra require standalone full execution\n";
            ok = false;
        }
        if (force_graph_rewrite && force_implicit_rewrite) {
            std::cerr << "Cannot combine --explicit-rewrite and --implicit-rewrite\n";
            ok = false;
        }
        input_dir = fact_dir;
        output_dir = out_dir;

        // return success state
        return ok;
    }

private:
    const IncrementalModeSpec& currentIncrementalModeSpec() const {
        return incModeSpec;
    }

    /**
     * Prints the help page if it has been requested or there was a typo in the command line arguments.
     */
    void printHelpPage(const std::string& exec_name) const {
        std::cerr << "====================================================================\n";
        std::cerr << " Datalog Program: " << src << "\n";
        std::cerr << " Usage: " << exec_name << " [OPTION]\n\n";
        std::cerr << " Options:\n";
        std::cerr << "    --full-only                  -- Run standalone full inference and exit (default)\n";
        std::cerr << "    --inc-only, --online         -- Run an online session with a plain full baseline\n";
        std::cerr << "    --rewrite                    -- Rewrite standalone full inference\n";
        std::cerr << "    --explicit-rewrite           -- Force graph rewrite\n";
        std::cerr << "    --implicit-rewrite           -- Force implicit split rewrite\n";
        std::cerr << "    --lifted-wmc                 -- Enable the exact pointwise fastpath\n";
        std::cerr << "    --lifted-threshold=<N>       -- Minimum output cardinality (default: "
                  << lifted_threshold << ")\n";
        std::cerr << "    --derv-only[=true|false]     -- Build the full derivation graph without inference\n";
        std::cerr << "    --merge-bi-imp, --prune-extra -- Opt-in standalone full pruning passes\n";
        std::cerr << "    -D <DIR>, --output=<DIR>     -- Specify directory for output relations\n";
        std::cerr << "                                    (default: " << output_dir << ")\n";
        std::cerr << "                                    (suppress output with \"\")\n";
        std::cerr << "    -F <DIR>, --facts=<DIR>      -- Specify directory for fact files\n";
        std::cerr << "                                    (default: " << input_dir << ")\n";
        std::cerr << "             --input-dir=<DIR>   -- Canonical alias for --facts\n";
        if (profiling) {
            std::cerr << "    -p <file>, --profile=<file>  -- Specify filename for profiling\n";
            std::cerr << "                                    (default: " << profile_name << ")\n";
        }
        std::cerr << "    -m, --setmode=<MODE>          -- Default online mode: "
                  << incrementalModeHelpText() << "\n";
        std::cerr << "    --dump=<LIST>                -- Canonical dump selector "
                  << dumpKindsOptionSyntax() << "\n";
        std::cerr << "    --profile-stage=<LIST>       -- Canonical profile selector "
                  << profileStageOptionSyntax() << "\n";
        std::cerr << "    --inc-reorder-policy=<POLICY> -- Incremental CUDD reordering policy "
                  << incReorderPolicyOptionSyntax() << "\n";
        std::cerr << "    --inc-reorder-auto-gap=<N>   -- Incremental CUDD next-reordering gap\n";
        std::cerr << "    --inc-reorder-work-threshold=<N>\n";
        std::cerr << "                                  -- Incremental BDD-pressure reorder threshold\n";
        std::cerr << "    --inc-reorder-count-dead     -- Count dead CUDD nodes for auto trigger\n";
        std::cerr << "    --inc-reorder-allow-large    -- Allow auto reorder on >=3M live-node managers\n";
        std::cerr << "    -v, --verbose                -- Print informational runtime diagnostics\n";
        std::cerr << "             --logfile=<FILE>    -- Runtime log filename\n";
        std::cerr << "             --log-file=<FILE>   -- Canonical alias for --logfile\n";
#ifdef _OPENMP
        std::cerr << "    -j <NUM>, --jobs=<NUM>       -- Specify number of threads\n";
        if (num_jobs > 0) {
            std::cerr << "                                    (default: " << num_jobs << ")\n";
        } else {
            std::cerr << "                                    (default: auto)\n";
        }
#endif
        std::cerr << "    -h                           -- prints this help page.\n";
        std::cerr << "--------------------------------------------------------------------\n";
#ifdef SOUFFLE_GENERATOR_VERSION
        std::cerr << " Version: " << SOUFFLE_GENERATOR_VERSION << std::endl;
#endif
        std::cerr << " Word size: " << RAM_DOMAIN_SIZE << " bits" << std::endl;
        std::cerr << "--------------------------------------------------------------------\n";
        std::cerr << " Copyright (c) 2016-22 The Souffle Developers." << std::endl;
        std::cerr << " Copyright (c) 2013-16 Oracle and/or its affiliates." << std::endl;
        std::cerr << " All rights reserved.\n";
        std::cerr << "====================================================================\n";
    }

    /**
     *  Check whether a file exists in the file system
     */
    inline bool existFile(const std::string& name) const {
        struct stat buffer;
        if (stat(name.c_str(), &buffer) == 0) {
            if ((buffer.st_mode & S_IFREG) != 0) {
                return true;
            }
        }
        return false;
    }

    /**
     *  Check whether a directory exists in the file system
     */
    bool existDir(const std::string& name) const {
        struct stat buffer;
        if (stat(name.c_str(), &buffer) == 0) {
            if ((buffer.st_mode & S_IFDIR) != 0) {
                return true;
            }
        }
        return false;
    }

    /**
     *  Check whether the output is "-", for which the output should be stdout
     */
    bool dirIsStdout(const std::string& name) const {
        return name == "-";
    }

    IncrementalModeSpec incModeSpec;
};

}  // end of namespace souffle
