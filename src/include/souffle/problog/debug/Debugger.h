#ifndef DEBUGGER_HPP
#define DEBUGGER_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <iostream>
#include <fstream>
#include <sstream>
#include <utility>
#include <cassert>
class Debugger;
enum class Level { INFO, WARNING, ERROR, DEBUG };

inline std::string levelToString(Level level) {
    switch (level) {
        case Level::INFO: return "INFO";
        case Level::WARNING: return "WARNING";
        case Level::ERROR: return "ERROR";
        case Level::DEBUG: return "DEBUG";
        default: return "UNKNOWN";
    }
}

enum class StageKind {
    SEMINAIVE,
    CREATE_GRAPH,
    PRUNING,
    DETERMINISTIC_EVENT_ALIASES,
    AND_INPUT_REDUNDANCY,
    FORWARD_COMPILATION,
    WEIGHTED_MODEL_COUNTING,
    FC_WMC_HYBRID,
    LIFTED_WMC,
    IO_DUMP,
    IO_LOAD_FULL,
    CONSTRUCT_RULE_FULL,
    SEMINAIVE_FULL,
    CREATE_GRAPH_FULL,
    PRUNING_FULL,
    PRECONFIG_FULL,
    FORWARD_COMPILATION_FULL,
    WEIGHTED_MODEL_COUNTING_FULL,
    IO_DUMP_FULL,
    SEMINAIVE_INC,
    CREATE_GRAPH_INC,
    PRUNING_INC,
    PRECONFIG_INC,
    FORWARD_COMPILATION_INC,
    WEIGHTED_MODEL_COUNTING_INC,
    IO_LOAD,
    CONSTRUCT_RULE
};

inline std::string stageKindToString(const StageKind kind) {
    switch (kind) {
        case StageKind::IO_LOAD: return "IO_LOAD";
        case StageKind::CONSTRUCT_RULE: return "CONSTRUCT_RULE";
        case StageKind::SEMINAIVE: return "SEMINAIVE";
        case StageKind::CREATE_GRAPH: return "CREATE_GRAPH";
        case StageKind::PRUNING: return "PRUNING";
        case StageKind::DETERMINISTIC_EVENT_ALIASES: return "DETERMINISTIC_EVENT_ALIASES";
        case StageKind::AND_INPUT_REDUNDANCY: return "AND_INPUT_REDUNDANCY";
        case StageKind::FORWARD_COMPILATION: return "FORWARD_COMPILATION";
        case StageKind::WEIGHTED_MODEL_COUNTING: return "WEIGHTED_MODEL_COUNTING";
        case StageKind::FC_WMC_HYBRID: return "FC_WMC_HYBRID";
        case StageKind::LIFTED_WMC: return "LIFTED_WMC";
        case StageKind::IO_DUMP: return "IO_DUMP";
        case StageKind::IO_LOAD_FULL: return "IO_LOAD_FULL";
        case StageKind::CONSTRUCT_RULE_FULL: return "CONSTRUCT_RULE_FULL";
        case StageKind::SEMINAIVE_FULL: return "SEMINAIVE_FULL";
        case StageKind::CREATE_GRAPH_FULL: return "CREATE_GRAPH_FULL";
        case StageKind::PRUNING_FULL: return "PRUNING_FULL";
        case StageKind::PRECONFIG_FULL: return "PRECONFIG_FULL";
        case StageKind::FORWARD_COMPILATION_FULL: return "FORWARD_COMPILATION_FULL";
        case StageKind::WEIGHTED_MODEL_COUNTING_FULL: return "WEIGHTED_MODEL_COUNTING_FULL";
        case StageKind::IO_DUMP_FULL: return "IO_DUMP_FULL";
        case StageKind::SEMINAIVE_INC: return "SEMINAIVE_INC";
        case StageKind::CREATE_GRAPH_INC: return "CREATE_GRAPH_INC";
        case StageKind::PRUNING_INC: return "PRUNING_INC";
        case StageKind::PRECONFIG_INC: return "PRECONFIG_INC";
        case StageKind::FORWARD_COMPILATION_INC: return "FORWARD_COMPILATION_INC";
        case StageKind::WEIGHTED_MODEL_COUNTING_INC: return "WEIGHTED_MODEL_COUNTING_INC";
        default: return "UNKNOWN";
    }
}

class Info {
protected:
    std::unordered_map<std::string, std::string> infoMap_;
    std::unordered_map<Level, std::vector<std::string>> logs_;
    size_t memStart_ = 0;
    size_t memEnd_ = 0;
    size_t memPeak_ = 0;

    std::chrono::steady_clock::time_point startTime_;
    std::chrono::steady_clock::time_point endTime_;

public:
    void setInfo(const std::string& key, const std::string& value) {
        infoMap_[key] = value;
    }

    std::string getInfo(const std::string& key) const {
        const auto it = infoMap_.find(key);
        return it != infoMap_.end() ? it->second : "";
    }

    void logMessage(const Level level, const std::string& message) {
        logs_[level].push_back(message);
    }

    void setMemStart(size_t m) { memStart_ = m; }
    size_t getMemStart() const { return memStart_; }

    void setMemEnd(size_t m) { memEnd_ = m; }
    size_t getMemEnd() const { return memEnd_; }

    void setMemPeak(size_t m) { memPeak_ = m; }
    size_t getMemPeak() const { return memPeak_; }

    void markStartTime() { startTime_ = std::chrono::steady_clock::now(); }
    void markEndTime() { endTime_ = std::chrono::steady_clock::now(); }
    bool hasStartTime() const { return startTime_ != std::chrono::steady_clock::time_point{}; }
    bool hasEndTime() const { return endTime_ != std::chrono::steady_clock::time_point{}; }

    double getDurationSeconds() const {
        using Clock = std::chrono::steady_clock;
        if (startTime_ == Clock::time_point{}) {
            return 0.0;
        }
        auto effectiveEnd = endTime_;
        if (effectiveEnd == Clock::time_point{} || effectiveEnd < startTime_) {
            effectiveEnd = Clock::now();
        }
        return std::chrono::duration<double>(effectiveEnd - startTime_).count();
    }

    const std::unordered_map<std::string, std::string>& getInfoMap() const { return infoMap_; }
    const std::unordered_map<Level, std::vector<std::string>>& getLogs() const { return logs_; }

    virtual ~Info() = default;
};

class IterationInfo : public Info {
public:
    int iterationIndex;
    explicit IterationInfo(const int idx) : iterationIndex(idx) {}
};

class StageInfo : public Info {
public:
    StageKind kind;
    std::vector<IterationInfo> iterations;

    explicit StageInfo(const StageKind k)
        : kind(k) {}
};

class TurnInfo : public Info {
public:
    int turnIndex;
    std::string algMode;
    size_t inputSize = 0;
    std::vector<StageInfo> stages;

    TurnInfo(const int idx, std::string mode)
        : turnIndex(idx), algMode(std::move(mode)) {}
};

class Debugger {
public:
    static Debugger& getInstance() {
        static Debugger instance;
        return instance;
    }

    Debugger(const Debugger&) = delete;
    Debugger& operator=(const Debugger&) = delete;

    TurnInfo* startTurn(const std::string& mode = "DEFAULT");
    void endTurn();
    StageInfo* startStage(StageKind kind);
    void endStage();
    IterationInfo* startIteration();
    void endIteration();
    void addInfo(const std::string& key, const std::string& value) const;
    void logMessage(Level level, const std::string& message) const;
    void printReport(std::ostream& os);
    void printReportJson(std::ostream& os);
    void setReportOutputFile(const std::string& path);
    std::string getReportOutputFile() const;
    bool isAutoDumpEnabled() const;
    void setRunStatus(const std::string& status);
    void setTerminationSignal(int signal);
    void dumpReportJsonToFile(const std::string& path = "");

private:
    Debugger();
    void maybeAutoDumpLocked();
    void printReportJsonLocked(std::ostream& os);
    void dumpReportJsonToFileLocked(const std::string& path);

    mutable std::mutex mtx_;
    int turnCount_;
    std::vector<TurnInfo> turns_;
    TurnInfo* currentTurn_;
    StageInfo* currentStage_;
    IterationInfo* currentIteration_;
    std::string reportOutputFile_;
    bool autoDumpEnabled_;
    std::string runStatus_ = "running";
    int terminationSignal_ = 0;

    size_t getCurrentMemoryUsage() const;
    size_t getPeakMemoryUsage() const;
};

#endif // DEBUGGER_HPP
