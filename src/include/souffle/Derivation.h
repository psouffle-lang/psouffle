//
// Created by Hugh on 2024/11/13.
//

#ifndef DERIVATION_H
#define DERIVATION_H

#include "souffle/RamTypes.h"
#include "souffle/SymbolTable.h"
#include "souffle/utility/json11.h"
#include "souffle/SouffleInterface.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct UntypedTuple;

void configureUntypedTupleRenderingContext(souffle::SymbolTable* symbolTable,
        std::unordered_map<std::string, std::vector<char>> relationAttributeTypes);
void clearUntypedTupleRenderingContext();
souffle::SymbolTable* getUntypedTupleRenderingSymbolTable();
std::unordered_map<std::string, std::vector<char>> copyUntypedTupleRenderingRelationTypes();
class ScopedUntypedTupleRenderingContext {
public:
    ScopedUntypedTupleRenderingContext(souffle::SymbolTable* symbolTable,
            std::unordered_map<std::string, std::vector<char>> relationAttributeTypes);
    ~ScopedUntypedTupleRenderingContext();

    ScopedUntypedTupleRenderingContext(const ScopedUntypedTupleRenderingContext&) = delete;
    ScopedUntypedTupleRenderingContext& operator=(const ScopedUntypedTupleRenderingContext&) = delete;

private:
    souffle::SymbolTable* previousSymbolTable;
    std::unordered_map<std::string, std::vector<char>> previousRelationTypes;
};
std::vector<souffle::RamDomain> parseUntypedTupleFields(
        const std::string& relationName, const std::string& renderedFields);
std::vector<souffle::RamDomain> parseUntypedTupleJsonFields(
        const std::string& relationName, const json11::Json& renderedFields);
UntypedTuple parseUntypedTupleJson(const json11::Json& tupleJson);
UntypedTuple parseUntypedTupleJson(const json11::Json& tupleJson, const std::string& renderedTupleSpec);
std::unordered_map<std::string, std::vector<char>> inferRelationTypesFromDerivationInfoJson(
        const json11::Json& root);
std::unordered_map<std::string, std::vector<char>> inferRelationTypesFromGraphJson(
        const json11::Json& root);

class Debugger;

std::string generateFilename(const std::string& prefix = "log", const std::string& suffix = ".txt");
std::string basenameFromPath(const std::string& path);
void setFunctionTimerOutputEnabled(bool enabled);
bool isFunctionTimerOutputEnabled();

class FunctionTimer {
private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<Clock>;
    using Duration = std::chrono::duration<double>;

    std::string function_name_;
    TimePoint start_time_;
    bool print_on_destruction_;
    Debugger& debugger;

public:
    explicit FunctionTimer(const std::string& name = "Function", bool print_on_destruction = true);
    ~FunctionTimer();

    double getElapsedTime() const;
    void printElapsedTime() const;
    void reset();
};

// Utility function to time any function call
template<typename Func, typename... Args>
double timeFunction(const std::string& name, Func&& func, Args&&... args) {
    FunctionTimer timer(name, false);
    std::invoke(std::forward<Func>(func), std::forward<Args>(args)...);
    return timer.getElapsedTime();
}

struct UntypedTuple {
    std::string relation_name;
    std::vector<souffle::RamDomain> fields;
    static std::string toString(const UntypedTuple& tuple);
    std::string toString() const;
    json11::Json toJson() const;
    static std::string toStringFields(const std::string& relationName, const std::vector<souffle::RamDomain>& fields);
    static std::string toStringFields(const std::vector<souffle::RamDomain>& fields);
    bool operator<(const UntypedTuple& other) const;
    bool operator==(const UntypedTuple& other) const;
    bool operator!=(const UntypedTuple& other) const;

    template<std::size_t N>
    static UntypedTuple fromTypedTuple(
            const std::string& relationName, const souffle::Tuple<souffle::RamDomain, N>& typedTuple) {
        UntypedTuple result;
        result.relation_name = relationName;
        for (const auto& field : typedTuple) {
            result.fields.push_back(field);
        }
        return result;
    }

    // for nullary
    static UntypedTuple fromTypedTuple(const std::string& relationName, const int* const&);
    static UntypedTuple fromSouffleTuple(const souffle::tuple& tuple);
};

template <typename T>
inline void hash_combine(std::size_t& seed, const T& val) {
    seed ^= std::hash<T>{}(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

// Hash specialization
namespace std {
template <>
struct hash<UntypedTuple> {
    std::size_t operator()(const UntypedTuple& tup) const {
        std::size_t seed = std::hash<std::string>{}(tup.relation_name);
        for (const auto& x : tup.fields) {
            hash_combine(seed, x);  // RamDomain is just an int
        }
        return seed;
    }
};
}

struct RuleApplication {
    souffle::RamDomain ruleId{};
    std::vector<souffle::RamDomain> varValuesPure;
    bool operator==(const RuleApplication& other) const;

    static std::string toString(const RuleApplication& ruleApplication);
    std::string toString() const;
    static std::string toStringVarValues(const std::map<std::string, souffle::RamDomain>& varValues);
    static std::string toStringVarValuesPure(const std::vector<souffle::RamDomain>& values);
    bool operator<(const RuleApplication& other) const;
};

// hash specialization
namespace std {
template <>
struct hash<RuleApplication> {
    std::size_t operator()(const RuleApplication& app) const {
        std::size_t seed = std::hash<souffle::RamDomain>{}(app.ruleId);
        for (const auto& value : app.varValuesPure) {
            hash_combine(seed, std::hash<souffle::RamDomain>{}(value));
        }
        return seed;
    }
};
}

/** Fact is trivially true; use the naiveRuleApplication for such cases when needed */
extern RuleApplication naiveRuleApplication;
extern bool dredProfileEnabled;
extern bool incProfileEnabled;
extern bool fcProfileEnabled;
extern bool incDeleteProfileEnabled;
extern bool wmcProfileEnabled;
extern bool incRegionalProfileEnabled;
extern bool depGraphProfileEnabled;
extern bool reuseVarIndexEnabled;
extern std::string incReorderPolicy;
extern std::size_t incReorderAutoGap;
extern std::size_t incReorderWorkThreshold;
extern bool incReorderCountDead;
extern bool incReorderAllowLarge;
extern std::map<std::uintptr_t, std::size_t> incReorderAccumulatedWorkScore;

class DerivationManager {
public:
    enum class DredTimeBucket : std::uint8_t {
        DelTotal,
        DelCopyOld,
        DelPreamble,
        DelPrefill,
        DelPrefillUpdate,
        DelLoopBody,
        DelLoopExit,
        DelLoopUpdate,
        DelPostamble,
        DelRecord,
        DelOverdelete,
        DelDeltaUnion,
        DelRuleappErase,
        InsTotal,
        InsPreamble,
        InsPrefill,
        InsPrefillUpdate,
        InsLoopBody,
        InsLoopExit,
        InsLoopUpdate,
        InsPostamble,
        InsRecord,
        InsDeltaUnion,
        RedTotal,
        RedLoopBody,
        RedLoopExit,
        RedLoopUpdate,
        RedPostamble,
    };

    struct DredStats {
        std::uint64_t del_ruleapp_recorded = 0;
        std::uint64_t del_ruleapp_delta_delta = 0;
        std::uint64_t del_ruleapp_overdelete = 0;
        std::uint64_t del_complete_scan_calls = 0;
        std::uint64_t del_complete_scan_elems = 0;
        std::uint64_t del_delta_tuples = 0;
        std::uint64_t del_delta_ruleapps = 0;
        std::uint64_t del_ruleapp_erases = 0;
        std::uint64_t del_tuple_deletes = 0;
        std::uint64_t del_complete_sets_freed = 0;

        std::uint64_t ins_ruleapp_recorded = 0;
        std::uint64_t ins_ruleapp_delta_delta = 0;
        std::uint64_t ins_ruleapp_rederive_erases = 0;
        std::uint64_t ins_delta_tuples = 0;
        std::uint64_t ins_delta_ruleapps = 0;
        std::uint64_t rederive_delta_tuples = 0;
        std::uint64_t rederive_delta_ruleapps = 0;
        std::uint64_t ins_ruleapp_merges = 0;
        std::uint64_t ins_tuple_inserts = 0;
        std::uint64_t ins_complete_sets_attached = 0;

        std::uint64_t del_time_total_ns = 0;
        std::uint64_t del_time_copy_old_ns = 0;
        std::uint64_t del_time_preamble_ns = 0;
        std::uint64_t del_time_prefill_ns = 0;
        std::uint64_t del_time_prefill_update_ns = 0;
        std::uint64_t del_time_loop_body_ns = 0;
        std::uint64_t del_time_loop_exit_ns = 0;
        std::uint64_t del_time_loop_update_ns = 0;
        std::uint64_t del_time_postamble_ns = 0;
        std::uint64_t del_time_record_ns = 0;
        std::uint64_t del_time_overdelete_ns = 0;
        std::uint64_t del_time_delta_union_ns = 0;
        std::uint64_t del_time_ruleapp_erase_ns = 0;

        std::uint64_t ins_time_total_ns = 0;
        std::uint64_t ins_time_preamble_ns = 0;
        std::uint64_t ins_time_prefill_ns = 0;
        std::uint64_t ins_time_prefill_update_ns = 0;
        std::uint64_t ins_time_loop_body_ns = 0;
        std::uint64_t ins_time_loop_exit_ns = 0;
        std::uint64_t ins_time_loop_update_ns = 0;
        std::uint64_t ins_time_postamble_ns = 0;
        std::uint64_t ins_time_record_ns = 0;
        std::uint64_t ins_time_delta_union_ns = 0;

        std::uint64_t red_time_total_ns = 0;
        std::uint64_t red_time_loop_body_ns = 0;
        std::uint64_t red_time_loop_exit_ns = 0;
        std::uint64_t red_time_loop_update_ns = 0;
        std::uint64_t red_time_postamble_ns = 0;

        void reset() { *this = DredStats{}; }
        void dump(std::ostream& out, const std::string& label) const;
    };

    struct DredSccStats {
        std::uint64_t del_ruleapp_overdelete = 0;
        std::uint64_t del_complete_scan_calls = 0;
        std::uint64_t del_complete_scan_elems = 0;
        std::uint64_t rederive_delta_tuples = 0;
        std::uint64_t rederive_delta_ruleapps = 0;
        std::uint64_t rederive_ruleapp_erases = 0;
        std::uint64_t del_time_total_ns = 0;
        std::uint64_t del_time_loop_body_ns = 0;
        std::uint64_t del_time_loop_update_ns = 0;
        std::uint64_t ins_time_total_ns = 0;
        std::uint64_t ins_time_loop_body_ns = 0;
        std::uint64_t ins_time_loop_update_ns = 0;
        std::uint64_t red_time_total_ns = 0;
        std::uint64_t red_time_loop_body_ns = 0;
        std::uint64_t red_time_loop_update_ns = 0;
    };

    static constexpr std::size_t kInvalidDredScc = static_cast<std::size_t>(-1);

    static std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*> untypedTuple2RuleApplications;
    static std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*> untypedTuple2DeltaInsertRuleApplications;
    static std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*> untypedTuple2DeltaDeleteRuleApplications;
    static std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*> untypedTuple2DeltaDeltaInsertRuleApplications;
    static std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*> untypedTuple2DeltaDeltaDeleteRuleApplications;
    static DredStats dredStats;
    static std::vector<DredSccStats> dredSccStats;
    static std::size_t dredCurrentScc;

    static bool ruleAppExistsInCompleteSet(
            const UntypedTuple& untypedTuple, const RuleApplication& ruleAppl);

    static void setSemStatsEnabled(bool enabled);
    static bool isSemStatsEnabled();
    static void resetDredStats();
    static void dumpDredStats(std::ostream& out, const std::string& label);
    static void dumpDredSccStats(std::ostream& out, const std::string& label);
    static std::uint64_t countRuleApplications(
            const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& derivationInfo);
    static std::uint64_t countRuleApplicationTuples(
            const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& derivationInfo);
    static void dumpRuleApplicationSummary(std::ostream& out, const std::string& label);

    static std::size_t getDredCurrentScc();
    static void setDredCurrentScc(std::size_t sccId);
    static void bumpDredSccOverdelete(std::uint64_t inc = 1);
    static void bumpDredSccCompleteScanCalls(std::uint64_t inc = 1);
    static void bumpDredSccCompleteScanElems(std::uint64_t inc);
    static void bumpDredSccRederiveDeltaTuples(std::uint64_t inc = 1);
    static void bumpDredSccRederiveDeltaRuleapps(std::uint64_t inc);
    static void bumpDredSccRederiveRuleappErases(std::uint64_t inc = 1);
    static void addDredTime(DredTimeBucket bucket, std::uint64_t ns);

    static void clearDetDeltaTuples();
    static void recordDetDeltaDelete(const UntypedTuple& tuple);
    static void recordDetDeltaInsert(const UntypedTuple& tuple);
    static const std::unordered_set<UntypedTuple>& getDetDeltaDeleteTuples();
    static const std::unordered_set<UntypedTuple>& getDetDeltaInsertTuples();
    static void freeRuleApplicationMap(
            std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& derivationInfo);

    static std::uint64_t nowNanos() {
        return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
    }
    static std::uint64_t elapsedNanos(std::uint64_t start_ns) {
        return nowNanos() - start_ns;
    }

private:
    static bool semStatsEnabled;
    static std::unordered_set<UntypedTuple> detDeltaDeleteTuples;
    static std::unordered_set<UntypedTuple> detDeltaInsertTuples;
};

// relation string -> int mapping, for optimization, reuse string
extern std::unordered_set<UntypedTuple> inputFactSet;
bool isInputFact(UntypedTuple tuple);

extern std::unordered_map<UntypedTuple, double> fact_prob;
extern std::unordered_map<std::string, bool> relationHasProbFact;
extern bool detOptEnabled;
extern std::unordered_map<std::string, bool> relationIsDet;
inline bool isDetRelation(const std::string& rel) {
    auto it = relationIsDet.find(rel);
    return it != relationIsDet.end() && it->second;
}

extern std::map<std::string, std::set<UntypedTuple>> initialInputRelations;

void dumpInitialInputRelations(std::string filename = "");

#endif //DERIVATION_H
