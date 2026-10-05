#include "souffle/Derivation.h"
#include "souffle/datastructure/SymbolTableImpl.h"
#include "souffle/problog/Atom.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <cctype>

#include "souffle/problog/debug/Debugger.h"

#include <cassert>
#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
souffle::SymbolTable* tupleRenderSymbolTable = nullptr;
std::unordered_map<std::string, std::vector<char>> tupleRenderRelationTypes;

bool tokenLooksIntegral(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    std::size_t start = 0;
    if (token.front() == '-' || token.front() == '+') {
        if (token.size() == 1) {
            return false;
        }
        start = 1;
    }
    for (std::size_t i = start; i < token.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(token[i]))) {
            return false;
        }
    }
    return true;
}

std::string renderTupleField(
        const std::string& relationName, std::size_t index, souffle::RamDomain field) {
    auto relIt = tupleRenderRelationTypes.find(relationName);
    if (relIt == tupleRenderRelationTypes.end() || index >= relIt->second.size()) {
        return std::to_string(field);
    }
    switch (relIt->second[index]) {
        case 's':
            if (tupleRenderSymbolTable == nullptr) {
                return std::to_string(field);
            }
            return json11::Json(tupleRenderSymbolTable->decode(field)).dump();
        default:
            return std::to_string(field);
    }
}

json11::Json renderTupleFieldJson(
        const std::string& relationName, std::size_t index, souffle::RamDomain field) {
    auto relIt = tupleRenderRelationTypes.find(relationName);
    if (relIt == tupleRenderRelationTypes.end() || index >= relIt->second.size()) {
        return json11::Json::object{{"ram", std::to_string(field)}};
    }
    switch (relIt->second[index]) {
        case 's':
            if (tupleRenderSymbolTable == nullptr) {
                return json11::Json::object{{"ram", std::to_string(field)}};
            }
            return json11::Json(tupleRenderSymbolTable->decode(field));
        default:
            return json11::Json::object{{"ram", std::to_string(field)}};
    }
}

json11::Json renderTupleFieldRawJson(souffle::RamDomain field) {
    return json11::Json::object{{"ram", std::to_string(field)}};
}

const std::vector<char>* lookupRelationTypes(const std::string& relationName) {
    auto relIt = tupleRenderRelationTypes.find(relationName);
    if (relIt == tupleRenderRelationTypes.end()) {
        return nullptr;
    }
    return &relIt->second;
}

std::string trimAscii(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    s.erase(0, first);
    const auto last = s.find_last_not_of(" \t\r\n");
    s.erase(last + 1);
    return s;
}

std::vector<std::string> splitRenderedTupleFields(const std::string& renderedFields) {
    std::vector<std::string> tokens;
    std::string current;
    bool inString = false;
    bool escaping = false;
    for (char c : renderedFields) {
        if (inString) {
            current.push_back(c);
            if (escaping) {
                escaping = false;
            } else if (c == '\\') {
                escaping = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            current.push_back(c);
            continue;
        }
        if (c == ',') {
            tokens.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty() || !renderedFields.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

void observeRelationTypes(std::unordered_map<std::string, std::vector<char>>& relationTypes,
        const std::string& relation, const std::vector<char>& observed) {
    if (relation.empty()) {
        return;
    }
    auto& current = relationTypes[relation];
    if (current.size() < observed.size()) {
        current.resize(observed.size(), '?');
    }
    for (std::size_t i = 0; i < observed.size(); ++i) {
        if (observed[i] == 's') {
            current[i] = 's';
        }
    }
}

void observeTupleJsonTypes(std::unordered_map<std::string, std::vector<char>>& relationTypes,
        const json11::Json& tupleJson) {
    if (!tupleJson.is_object()) {
        return;
    }
    const std::string relation = tupleJson["rel"].string_value();
    if (relation.empty() && !tupleJson["rel"].is_string()) {
        return;
    }
    std::vector<char> observed;
    for (const auto& field : tupleJson["fields"].array_items()) {
        observed.push_back(field.is_string() ? 's' : '?');
    }
    observeRelationTypes(relationTypes, relation, observed);
}

void observeTupleSpecTypes(std::unordered_map<std::string, std::vector<char>>& relationTypes,
        const std::string& tupleSpec) {
    const auto lp = tupleSpec.find('(');
    if (lp == std::string::npos) {
        return;
    }
    const auto rp = tupleSpec.rfind(')');
    if (rp == std::string::npos || rp <= lp) {
        return;
    }
    std::string relation = tupleSpec.substr(0, lp);
    const auto first = relation.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return;
    }
    relation.erase(0, first);
    const auto last = relation.find_last_not_of(" \t\r\n");
    relation.erase(last + 1);
    std::vector<char> observed;
    for (auto token : splitRenderedTupleFields(tupleSpec.substr(lp + 1, rp - lp - 1))) {
        const auto tokenFirst = token.find_first_not_of(" \t\r\n");
        if (tokenFirst == std::string::npos) {
            continue;
        }
        token.erase(0, tokenFirst);
        const auto tokenLast = token.find_last_not_of(" \t\r\n");
        token.erase(tokenLast + 1);
        observed.push_back((token.size() >= 2 && token.front() == '"' && token.back() == '"') ? 's' : '?');
    }
    observeRelationTypes(relationTypes, relation, observed);
}

void observeTupleJsonOrRendered(std::unordered_map<std::string, std::vector<char>>& relationTypes,
        const json11::Json& tupleJson, const json11::Json& rendered) {
    if (tupleJson.is_object()) {
        observeTupleJsonTypes(relationTypes, tupleJson);
    }
    if (rendered.is_string()) {
        observeTupleSpecTypes(relationTypes, rendered.string_value());
    }
}

std::vector<std::optional<std::string>> extractRenderedTupleStringHints(
        const std::string& relationName, const std::string& renderedTupleSpec) {
    std::vector<std::optional<std::string>> hints;
    const auto lp = renderedTupleSpec.find('(');
    if (lp == std::string::npos) {
        return hints;
    }
    const auto rp = renderedTupleSpec.rfind(')');
    if (rp == std::string::npos || rp <= lp) {
        return hints;
    }
    const std::string renderedRelation = trimAscii(renderedTupleSpec.substr(0, lp));
    if (!relationName.empty() && renderedRelation != relationName) {
        return hints;
    }
    for (auto token : splitRenderedTupleFields(renderedTupleSpec.substr(lp + 1, rp - lp - 1))) {
        const auto first = token.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            hints.emplace_back(std::nullopt);
            continue;
        }
        token.erase(0, first);
        const auto last = token.find_last_not_of(" \t\r\n");
        token.erase(last + 1);
        if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
            std::string err;
            auto parsed = json11::Json::parse(token, err);
            if (err.empty() && parsed.is_string()) {
                hints.emplace_back(parsed.string_value());
                continue;
            }
        }
        hints.emplace_back(std::nullopt);
    }
    return hints;
}
}

void configureUntypedTupleRenderingContext(souffle::SymbolTable* symbolTable,
        std::unordered_map<std::string, std::vector<char>> relationAttributeTypes) {
    tupleRenderSymbolTable = symbolTable;
    tupleRenderRelationTypes = std::move(relationAttributeTypes);
    souffle::problog::setActiveSymbolTable(symbolTable);
}

void clearUntypedTupleRenderingContext() {
    tupleRenderSymbolTable = nullptr;
    tupleRenderRelationTypes.clear();
    souffle::problog::setActiveSymbolTable(nullptr);
}

souffle::SymbolTable* getUntypedTupleRenderingSymbolTable() {
    return tupleRenderSymbolTable;
}

std::unordered_map<std::string, std::vector<char>> copyUntypedTupleRenderingRelationTypes() {
    return tupleRenderRelationTypes;
}

ScopedUntypedTupleRenderingContext::ScopedUntypedTupleRenderingContext(
        souffle::SymbolTable* symbolTable,
        std::unordered_map<std::string, std::vector<char>> relationAttributeTypes)
        : previousSymbolTable(tupleRenderSymbolTable),
          previousRelationTypes(tupleRenderRelationTypes) {
    configureUntypedTupleRenderingContext(symbolTable, std::move(relationAttributeTypes));
}

ScopedUntypedTupleRenderingContext::~ScopedUntypedTupleRenderingContext() {
    configureUntypedTupleRenderingContext(previousSymbolTable, std::move(previousRelationTypes));
}

std::vector<souffle::RamDomain> parseUntypedTupleFields(
        const std::string& relationName, const std::string& renderedFields) {
    std::vector<souffle::RamDomain> fields;
    if (renderedFields.empty()) {
        return fields;
    }
    const auto* relationTypes = lookupRelationTypes(relationName);
    auto tokens = splitRenderedTupleFields(renderedFields);
    fields.reserve(tokens.size());
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        auto token = tokens[i];
        const auto first = token.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            continue;
        }
        token.erase(0, first);
        const auto last = token.find_last_not_of(" \t\r\n");
        token.erase(last + 1);
        const bool looksLikeJsonString =
                token.size() >= 2 && token.front() == '"' && token.back() == '"';
        const bool isSymbol = looksLikeJsonString ||
                (relationTypes != nullptr && i < relationTypes->size() && (*relationTypes)[i] == 's');
        if (isSymbol) {
            if (tupleRenderSymbolTable == nullptr) {
                throw std::runtime_error(
                        "cannot parse symbolic tuple field without an active symbol table");
            }
            if (!looksLikeJsonString && tokenLooksIntegral(token)) {
                fields.push_back(static_cast<souffle::RamDomain>(std::stoll(token)));
                continue;
            }
            if (!looksLikeJsonString) {
                fields.push_back(tupleRenderSymbolTable->encode(token));
                continue;
            }
            std::string err;
            auto parsed = json11::Json::parse(token, err);
            if (!err.empty() || !parsed.is_string()) {
                throw std::runtime_error("cannot parse symbolic tuple field: " + token);
            }
            fields.push_back(tupleRenderSymbolTable->encode(parsed.string_value()));
            continue;
        }
        fields.push_back(static_cast<souffle::RamDomain>(std::stoll(token)));
    }
    return fields;
}

std::vector<souffle::RamDomain> parseUntypedTupleJsonFieldsWithHints(
        const std::string& relationName, const json11::Json& renderedFields,
        const std::vector<std::optional<std::string>>& renderedStringHints) {
    if (!renderedFields.is_array()) {
        throw std::runtime_error("tuple fields must be a JSON array");
    }
    std::vector<souffle::RamDomain> fields;
    const auto* relationTypes = lookupRelationTypes(relationName);
    const auto& items = renderedFields.array_items();
    fields.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        const bool isSymbol = item.is_string() ||
                (relationTypes != nullptr && i < relationTypes->size() && (*relationTypes)[i] == 's');
        if (isSymbol) {
            if (item.is_string()) {
                if (tupleRenderSymbolTable == nullptr) {
                    throw std::runtime_error(
                            "cannot parse symbolic tuple field without an active symbol table");
                }
                fields.push_back(tupleRenderSymbolTable->encode(item.string_value()));
            } else if (item.is_object()) {
                const auto& raw = item["ram"];
                if (!raw.is_string()) {
                    throw std::runtime_error("unsupported symbolic tuple JSON object field");
                }
                if (i < renderedStringHints.size() && renderedStringHints[i].has_value()) {
                    if (tupleRenderSymbolTable == nullptr) {
                        throw std::runtime_error(
                                "cannot re-encode symbolic tuple field without an active symbol table");
                    }
                    fields.push_back(tupleRenderSymbolTable->encode(*renderedStringHints[i]));
                } else {
                    fields.push_back(static_cast<souffle::RamDomain>(std::stoll(raw.string_value())));
                }
            } else if (item.is_number()) {
                if (i < renderedStringHints.size() && renderedStringHints[i].has_value()) {
                    if (tupleRenderSymbolTable == nullptr) {
                        throw std::runtime_error(
                                "cannot re-encode symbolic tuple field without an active symbol table");
                    }
                    fields.push_back(tupleRenderSymbolTable->encode(*renderedStringHints[i]));
                } else {
                    fields.push_back(static_cast<souffle::RamDomain>(item.number_value()));
                }
            } else {
                throw std::runtime_error("unsupported symbolic tuple JSON field");
            }
            continue;
        }
        if (item.is_object()) {
            const auto& raw = item["ram"];
            if (!raw.is_string()) {
                throw std::runtime_error("unsupported numeric tuple JSON object field");
            }
            fields.push_back(static_cast<souffle::RamDomain>(std::stoll(raw.string_value())));
            continue;
        }
        if (!item.is_number()) {
            throw std::runtime_error("non-numeric tuple field in numeric position");
        }
        fields.push_back(static_cast<souffle::RamDomain>(item.number_value()));
    }
    return fields;
}

std::vector<souffle::RamDomain> parseUntypedTupleJsonFields(
        const std::string& relationName, const json11::Json& renderedFields) {
    return parseUntypedTupleJsonFieldsWithHints(
            relationName, renderedFields, std::vector<std::optional<std::string>>{});
}

UntypedTuple parseUntypedTupleJson(const json11::Json& tupleJson) {
    return parseUntypedTupleJson(tupleJson, "");
}

UntypedTuple parseUntypedTupleJson(
        const json11::Json& tupleJson, const std::string& renderedTupleSpec) {
    if (!tupleJson.is_object()) {
        throw std::runtime_error("tuple JSON must be an object");
    }
    const std::string relationName = tupleJson["rel"].string_value();
    if (relationName.empty() && !tupleJson["rel"].is_string()) {
        throw std::runtime_error("tuple JSON missing relation name");
    }
    const auto& fieldsJson = tupleJson["fieldsRaw"].is_array() ? tupleJson["fieldsRaw"] : tupleJson["fields"];
    return UntypedTuple{relationName,
            parseUntypedTupleJsonFieldsWithHints(
                    relationName, fieldsJson,
                    extractRenderedTupleStringHints(relationName, renderedTupleSpec))};
}

std::unordered_map<std::string, std::vector<char>> inferRelationTypesFromDerivationInfoJson(
        const json11::Json& root) {
    std::unordered_map<std::string, std::vector<char>> relationTypes;
    if (!root.is_array()) {
        return relationTypes;
    }
    for (const auto& item : root.array_items()) {
        if (!item.is_object()) {
            continue;
        }
            observeTupleJsonOrRendered(relationTypes, item["tuple"], item["name"]);
    }
    return relationTypes;
}

std::unordered_map<std::string, std::vector<char>> inferRelationTypesFromGraphJson(
        const json11::Json& root) {
    std::unordered_map<std::string, std::vector<char>> relationTypes;
    auto observeNodeArray = [&](const json11::Json& arr) {
        if (!arr.is_array()) {
            return;
        }
        for (const auto& item : arr.array_items()) {
            if (!item.is_object()) {
                continue;
            }
            observeTupleJsonOrRendered(relationTypes, item["tuple"], item["name"]);
        }
    };
    auto observeEdgeArray = [&](const json11::Json& arr) {
        if (!arr.is_array()) {
            return;
        }
        for (const auto& edge : arr.array_items()) {
            if (!edge.is_object()) {
                continue;
            }
            observeTupleJsonOrRendered(relationTypes, edge["headTuple"], edge["head"]);
            if (!edge["bodies"].is_array()) {
                continue;
            }
            for (const auto& body : edge["bodies"].array_items()) {
                if (!body.is_object()) {
                    continue;
                }
                observeTupleJsonOrRendered(relationTypes, body["tuple"], body["name"]);
            }
        }
    };
    observeNodeArray(root["facts"]);
    observeEdgeArray(root["rules"]);
    if (root["delta"].is_object()) {
        for (const char* phase : {"insert", "delete"}) {
            const auto& delta = root["delta"][phase];
            observeNodeArray(delta["facts"]);
            observeNodeArray(delta["nodes"]);
            observeEdgeArray(delta["edges"]);
        }
    }
    return relationTypes;
}

std::string generateFilename(const std::string& prefix, const std::string& suffix) {
    std::time_t now = std::time(nullptr);
    std::tm* local = std::localtime(&now);

    std::ostringstream oss;
    oss << prefix << "_"
        << (1900 + local->tm_year)
        << (local->tm_mon + 1)
        << local->tm_mday << "_"
        << local->tm_hour
        << local->tm_min
        << local->tm_sec
        << suffix;

    return oss.str();
}

std::string basenameFromPath(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    const std::string::size_type pos = path.find_last_of("/\\");
    if (pos == std::string::npos || pos + 1 >= path.size()) {
        return path;
    }
    return path.substr(pos + 1);
}

namespace {
bool functionTimerOutputEnabled = false;
}

void setFunctionTimerOutputEnabled(bool enabled) {
    functionTimerOutputEnabled = enabled;
}

bool isFunctionTimerOutputEnabled() {
    return functionTimerOutputEnabled;
}

FunctionTimer::FunctionTimer(const std::string& name, bool print_on_destruction)
        : function_name_(name),
          start_time_(Clock::now()),
          print_on_destruction_(print_on_destruction),
          debugger(Debugger::getInstance()) {}

FunctionTimer::~FunctionTimer() {
    if (print_on_destruction_ && isFunctionTimerOutputEnabled()) {
        printElapsedTime();
    }
}

double FunctionTimer::getElapsedTime() const {
    TimePoint end_time = Clock::now();
    Duration duration = end_time - start_time_;
    return duration.count();
}

void FunctionTimer::printElapsedTime() const {
    double elapsed = getElapsedTime();
    std::cout << function_name_ << " took " << elapsed << " seconds" << std::endl;
}

void FunctionTimer::reset() {
    start_time_ = Clock::now();
}

std::string UntypedTuple::toString(const UntypedTuple& tuple) {
    std::string result = tuple.relation_name + '(' +
            toStringFields(tuple.relation_name, tuple.fields) + ')';
    return result;
}

std::string UntypedTuple::toString() const {
    std::string result = relation_name + '(' + toStringFields(relation_name, fields) + ')';
    return result;
}

json11::Json UntypedTuple::toJson() const {
    json11::Json::array renderedFields;
    json11::Json::array rawFields;
    renderedFields.reserve(fields.size());
    rawFields.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        renderedFields.push_back(renderTupleFieldJson(relation_name, i, fields[i]));
        rawFields.push_back(renderTupleFieldRawJson(fields[i]));
    }
    return json11::Json::object{
            {"rel", relation_name},
            {"fields", renderedFields},
            {"fieldsRaw", rawFields},
    };
}

std::string UntypedTuple::toStringFields(
        const std::string& relationName, const std::vector<souffle::RamDomain>& fields) {
    std::string result;
    bool first = true;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (first) {
            first = false;
        } else {
            result += ",";
        }
        result += renderTupleField(relationName, i, fields[i]);
    }
    return result;
}

std::string UntypedTuple::toStringFields(const std::vector<souffle::RamDomain>& fields) {
    std::string result;
    bool first = true;
    for (const auto& field : fields) {
        if (first) {
            first = false;
            result += std::to_string(field);
        } else {
            result += "," + std::to_string(field);
        }
    }
    return result;
}

bool UntypedTuple::operator<(const UntypedTuple& other) const {
    if (relation_name != other.relation_name) {
        return relation_name < other.relation_name;
    }
    if (fields.size() != other.fields.size()) {
        return fields.size() < other.fields.size();
    }
    for (size_t i = 0; i < fields.size(); i++) {
        if (fields[i] != other.fields[i]) {
            return fields[i] < other.fields[i];
        }
    }
    return false;
}

bool UntypedTuple::operator==(const UntypedTuple& other) const {
    return relation_name == other.relation_name && fields == other.fields;
}

bool UntypedTuple::operator!=(const UntypedTuple& other) const {
    return !(*this == other);
}

UntypedTuple UntypedTuple::fromTypedTuple(const std::string& relationName, const int* const&) {
    return UntypedTuple{relationName, {}};
}

UntypedTuple UntypedTuple::fromSouffleTuple(const souffle::tuple& tuple) {
    UntypedTuple result;
    result.relation_name = tuple.getRelation().getName();
    result.fields.reserve(tuple.getRelation().getArity());
    for (size_t i = 0; i < tuple.getRelation().getArity(); i++) {
        result.fields.push_back(tuple[i]);
    }
    return result;
}

bool RuleApplication::operator==(const RuleApplication& other) const {
    return ruleId == other.ruleId && varValuesPure == other.varValuesPure;
}

std::string RuleApplication::toString(const RuleApplication& ruleApplication) {
    std::string result = std::to_string(ruleApplication.ruleId) + "[" +
                         toStringVarValuesPure(ruleApplication.varValuesPure) + "]";
    return result;
}

std::string RuleApplication::toString() const {
    std::string result = std::to_string(ruleId) + "[" + toStringVarValuesPure(varValuesPure) + "]";
    return result;
}

std::string RuleApplication::toStringVarValues(
        const std::map<std::string, souffle::RamDomain>& varValues) {
    std::string result;
    bool first = true;
    for (const auto& [var, value] : varValues) {
        if (first) {
            first = false;
            result += var + "->" + std::to_string(value);
        } else {
            result += "," + var + "->" + std::to_string(value);
        }
    }
    return result;
}

std::string RuleApplication::toStringVarValuesPure(const std::vector<souffle::RamDomain>& values) {
    std::string result;
    bool first = true;
    for (const auto& value : values) {
        if (first) {
            first = false;
            result += std::to_string(value);
        } else {
            result += "," + std::to_string(value);
        }
    }
    return result;
}

bool RuleApplication::operator<(const RuleApplication& other) const {
    if (ruleId != other.ruleId) {
        return ruleId < other.ruleId;
    }
    assert(varValuesPure.size() == other.varValuesPure.size());
    for (int i = 0; i < varValuesPure.size(); i++) {
        if (varValuesPure[i] != other.varValuesPure[i]) {
            return varValuesPure[i] < other.varValuesPure[i];
        }
    }
    return false;
}

RuleApplication naiveRuleApplication{0, {}};

std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>
        DerivationManager::untypedTuple2RuleApplications = {};
std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>
        DerivationManager::untypedTuple2DeltaInsertRuleApplications = {};
std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>
        DerivationManager::untypedTuple2DeltaDeleteRuleApplications = {};
std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>
        DerivationManager::untypedTuple2DeltaDeltaInsertRuleApplications = {};
std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>
        DerivationManager::untypedTuple2DeltaDeltaDeleteRuleApplications = {};
DerivationManager::DredStats DerivationManager::dredStats = {};
std::vector<DerivationManager::DredSccStats> DerivationManager::dredSccStats = {};
std::size_t DerivationManager::dredCurrentScc = DerivationManager::kInvalidDredScc;
bool DerivationManager::semStatsEnabled = false;
std::unordered_set<UntypedTuple> DerivationManager::detDeltaDeleteTuples = {};
std::unordered_set<UntypedTuple> DerivationManager::detDeltaInsertTuples = {};
bool dredProfileEnabled = false;
bool incProfileEnabled = false;
bool fcProfileEnabled = false;
bool incDeleteProfileEnabled = false;
bool wmcProfileEnabled = false;
bool incRegionalProfileEnabled = false;
bool depGraphProfileEnabled = false;
bool reuseVarIndexEnabled = true;
std::string incReorderPolicy = "pressure";
std::size_t incReorderAutoGap = 0;
std::size_t incReorderWorkThreshold = 2500;
bool incReorderCountDead = false;
bool incReorderAllowLarge = false;
std::map<std::uintptr_t, std::size_t> incReorderAccumulatedWorkScore;

void DerivationManager::clearDetDeltaTuples() {
    detDeltaDeleteTuples.clear();
    detDeltaInsertTuples.clear();
}

void DerivationManager::recordDetDeltaDelete(const UntypedTuple& tuple) {
    detDeltaDeleteTuples.insert(tuple);
}

void DerivationManager::recordDetDeltaInsert(const UntypedTuple& tuple) {
    detDeltaInsertTuples.insert(tuple);
}

const std::unordered_set<UntypedTuple>& DerivationManager::getDetDeltaDeleteTuples() {
    return detDeltaDeleteTuples;
}

const std::unordered_set<UntypedTuple>& DerivationManager::getDetDeltaInsertTuples() {
    return detDeltaInsertTuples;
}

void DerivationManager::freeRuleApplicationMap(
        std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& derivationInfo) {
    for (auto& [_, ruleSet] : derivationInfo) {
        delete ruleSet;
        ruleSet = nullptr;
    }
    derivationInfo.clear();
}

void DerivationManager::DredStats::dump(std::ostream& out, const std::string& label) const {
    out << "[seminaive-dred] " << label << " del"
        << " delta_tuples=" << del_delta_tuples
        << " delta_ruleapps=" << del_delta_ruleapps
        << " ruleapps_recorded=" << del_ruleapp_recorded
        << " ruleapps_delta_delta=" << del_ruleapp_delta_delta
        << " ruleapps_overdelete=" << del_ruleapp_overdelete
        << " ruleapps_erased=" << del_ruleapp_erases
        << " complete_scan_calls=" << del_complete_scan_calls
        << " complete_scan_elems=" << del_complete_scan_elems
        << " tuples_deleted=" << del_tuple_deletes
        << " complete_sets_freed=" << del_complete_sets_freed
        << " time_total_ns=" << del_time_total_ns
        << " time_copy_old_ns=" << del_time_copy_old_ns
        << " time_preamble_ns=" << del_time_preamble_ns
        << " time_prefill_ns=" << del_time_prefill_ns
        << " time_prefill_update_ns=" << del_time_prefill_update_ns
        << " time_loop_body_ns=" << del_time_loop_body_ns
        << " time_loop_exit_ns=" << del_time_loop_exit_ns
        << " time_loop_update_ns=" << del_time_loop_update_ns
        << " time_postamble_ns=" << del_time_postamble_ns
        << " time_record_ns=" << del_time_record_ns
        << " time_overdelete_ns=" << del_time_overdelete_ns
        << " time_delta_union_ns=" << del_time_delta_union_ns
        << " time_ruleapp_erase_ns=" << del_time_ruleapp_erase_ns
        << "\n";
    out << "[seminaive-dred] " << label << " ins"
        << " delta_tuples=" << ins_delta_tuples
        << " delta_ruleapps=" << ins_delta_ruleapps
        << " ruleapps_recorded=" << ins_ruleapp_recorded
        << " ruleapps_delta_delta=" << ins_ruleapp_delta_delta
        << " ruleapps_rederive_erased=" << ins_ruleapp_rederive_erases
        << " ruleapps_merged=" << ins_ruleapp_merges
        << " rederive_delta_tuples=" << rederive_delta_tuples
        << " rederive_delta_ruleapps=" << rederive_delta_ruleapps
        << " tuples_inserted=" << ins_tuple_inserts
        << " complete_sets_attached=" << ins_complete_sets_attached
        << " time_total_ns=" << ins_time_total_ns
        << " time_preamble_ns=" << ins_time_preamble_ns
        << " time_prefill_ns=" << ins_time_prefill_ns
        << " time_prefill_update_ns=" << ins_time_prefill_update_ns
        << " time_loop_body_ns=" << ins_time_loop_body_ns
        << " time_loop_exit_ns=" << ins_time_loop_exit_ns
        << " time_loop_update_ns=" << ins_time_loop_update_ns
        << " time_postamble_ns=" << ins_time_postamble_ns
        << " time_record_ns=" << ins_time_record_ns
        << " time_delta_union_ns=" << ins_time_delta_union_ns
        << " rederive_time_total_ns=" << red_time_total_ns
        << " rederive_time_loop_body_ns=" << red_time_loop_body_ns
        << " rederive_time_loop_exit_ns=" << red_time_loop_exit_ns
        << " rederive_time_loop_update_ns=" << red_time_loop_update_ns
        << " rederive_time_postamble_ns=" << red_time_postamble_ns
        << "\n";
}

void DerivationManager::setSemStatsEnabled(bool enabled) {
    semStatsEnabled = enabled;
}

bool DerivationManager::isSemStatsEnabled() {
    return semStatsEnabled;
}

void DerivationManager::resetDredStats() {
    dredStats.reset();
    dredSccStats.clear();
    dredCurrentScc = kInvalidDredScc;
}

void DerivationManager::dumpDredStats(std::ostream& out, const std::string& label) {
    if (!semStatsEnabled) {
        return;
    }
    dredStats.dump(out, label);
    dumpDredSccStats(out, label);
}

void DerivationManager::dumpDredSccStats(std::ostream& out, const std::string& label) {
    if (!semStatsEnabled || dredSccStats.empty()) {
        return;
    }
    for (std::size_t sccId = 0; sccId < dredSccStats.size(); ++sccId) {
        const auto& stats = dredSccStats[sccId];
        if (stats.del_ruleapp_overdelete == 0 && stats.del_complete_scan_calls == 0 &&
                stats.del_complete_scan_elems == 0 && stats.rederive_delta_tuples == 0 &&
                stats.rederive_delta_ruleapps == 0 && stats.rederive_ruleapp_erases == 0 &&
                stats.del_time_total_ns == 0 && stats.del_time_loop_body_ns == 0 &&
                stats.del_time_loop_update_ns == 0 && stats.ins_time_total_ns == 0 &&
                stats.ins_time_loop_body_ns == 0 && stats.ins_time_loop_update_ns == 0 &&
                stats.red_time_total_ns == 0 && stats.red_time_loop_body_ns == 0 &&
                stats.red_time_loop_update_ns == 0) {
            continue;
        }
        out << "[seminaive-dred-scc] " << label << " scc=" << sccId
            << " ruleapps_overdelete=" << stats.del_ruleapp_overdelete
            << " complete_scan_calls=" << stats.del_complete_scan_calls
            << " complete_scan_elems=" << stats.del_complete_scan_elems
            << " rederive_delta_tuples=" << stats.rederive_delta_tuples
            << " rederive_delta_ruleapps=" << stats.rederive_delta_ruleapps
            << " ruleapps_rederive_erased=" << stats.rederive_ruleapp_erases
            << " del_time_total_ns=" << stats.del_time_total_ns
            << " del_time_loop_body_ns=" << stats.del_time_loop_body_ns
            << " del_time_loop_update_ns=" << stats.del_time_loop_update_ns
            << " ins_time_total_ns=" << stats.ins_time_total_ns
            << " ins_time_loop_body_ns=" << stats.ins_time_loop_body_ns
            << " ins_time_loop_update_ns=" << stats.ins_time_loop_update_ns
            << " rederive_time_total_ns=" << stats.red_time_total_ns
            << " rederive_time_loop_body_ns=" << stats.red_time_loop_body_ns
            << " rederive_time_loop_update_ns=" << stats.red_time_loop_update_ns
            << "\n";
    }
}

std::uint64_t DerivationManager::countRuleApplications(
        const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& derivationInfo) {
    std::uint64_t total = 0;
    for (const auto& [tuple, ruleApplicationSet] : derivationInfo) {
        (void)tuple;
        if (ruleApplicationSet != nullptr) {
            total += static_cast<std::uint64_t>(ruleApplicationSet->size());
        }
    }
    return total;
}

std::uint64_t DerivationManager::countRuleApplicationTuples(
        const std::unordered_map<UntypedTuple, std::unordered_set<RuleApplication>*>& derivationInfo) {
    std::uint64_t total = 0;
    for (const auto& [tuple, ruleApplicationSet] : derivationInfo) {
        (void)tuple;
        if (ruleApplicationSet != nullptr && !ruleApplicationSet->empty()) {
            total++;
        }
    }
    return total;
}

void DerivationManager::dumpRuleApplicationSummary(std::ostream& out, const std::string& label) {
    if (!semStatsEnabled) {
        return;
    }
    out << "[seminaive-ruleapps] " << label
        << " complete_tuples=" << countRuleApplicationTuples(untypedTuple2RuleApplications)
        << " complete_ruleapps=" << countRuleApplications(untypedTuple2RuleApplications)
        << " delta_insert_tuples=" << countRuleApplicationTuples(untypedTuple2DeltaInsertRuleApplications)
        << " delta_insert_ruleapps=" << countRuleApplications(untypedTuple2DeltaInsertRuleApplications)
        << " delta_delete_tuples=" << countRuleApplicationTuples(untypedTuple2DeltaDeleteRuleApplications)
        << " delta_delete_ruleapps=" << countRuleApplications(untypedTuple2DeltaDeleteRuleApplications)
        << " delta_delta_insert_tuples=" << countRuleApplicationTuples(untypedTuple2DeltaDeltaInsertRuleApplications)
        << " delta_delta_insert_ruleapps=" << countRuleApplications(untypedTuple2DeltaDeltaInsertRuleApplications)
        << " delta_delta_delete_tuples=" << countRuleApplicationTuples(untypedTuple2DeltaDeltaDeleteRuleApplications)
        << " delta_delta_delete_ruleapps=" << countRuleApplications(untypedTuple2DeltaDeltaDeleteRuleApplications)
        << "\n";
}

std::size_t DerivationManager::getDredCurrentScc() {
    return dredCurrentScc;
}

void DerivationManager::setDredCurrentScc(std::size_t sccId) {
    dredCurrentScc = sccId;
    if (dredCurrentScc == kInvalidDredScc) {
        return;
    }
    if (dredSccStats.size() <= dredCurrentScc) {
        dredSccStats.resize(dredCurrentScc + 1);
    }
}

static DerivationManager::DredSccStats* getDredSccStats() {
    if (DerivationManager::getDredCurrentScc() == DerivationManager::kInvalidDredScc) {
        return nullptr;
    }
    if (DerivationManager::dredSccStats.size() <= DerivationManager::getDredCurrentScc()) {
        DerivationManager::dredSccStats.resize(DerivationManager::getDredCurrentScc() + 1);
    }
    return &DerivationManager::dredSccStats[DerivationManager::getDredCurrentScc()];
}

void DerivationManager::bumpDredSccOverdelete(std::uint64_t inc) {
    if (auto* stats = getDredSccStats()) {
        stats->del_ruleapp_overdelete += inc;
    }
}

void DerivationManager::bumpDredSccCompleteScanCalls(std::uint64_t inc) {
    if (auto* stats = getDredSccStats()) {
        stats->del_complete_scan_calls += inc;
    }
}

void DerivationManager::bumpDredSccCompleteScanElems(std::uint64_t inc) {
    if (auto* stats = getDredSccStats()) {
        stats->del_complete_scan_elems += inc;
    }
}

void DerivationManager::bumpDredSccRederiveDeltaTuples(std::uint64_t inc) {
    if (auto* stats = getDredSccStats()) {
        stats->rederive_delta_tuples += inc;
    }
}

void DerivationManager::bumpDredSccRederiveDeltaRuleapps(std::uint64_t inc) {
    if (auto* stats = getDredSccStats()) {
        stats->rederive_delta_ruleapps += inc;
    }
}

void DerivationManager::bumpDredSccRederiveRuleappErases(std::uint64_t inc) {
    if (auto* stats = getDredSccStats()) {
        stats->rederive_ruleapp_erases += inc;
    }
}

void DerivationManager::addDredTime(DredTimeBucket bucket, std::uint64_t ns) {
    if (ns == 0) {
        return;
    }
    switch (bucket) {
        case DredTimeBucket::DelTotal:
            dredStats.del_time_total_ns += ns;
            if (auto* stats = getDredSccStats()) stats->del_time_total_ns += ns;
            break;
        case DredTimeBucket::DelCopyOld:
            dredStats.del_time_copy_old_ns += ns;
            break;
        case DredTimeBucket::DelPreamble:
            dredStats.del_time_preamble_ns += ns;
            break;
        case DredTimeBucket::DelPrefill:
            dredStats.del_time_prefill_ns += ns;
            break;
        case DredTimeBucket::DelPrefillUpdate:
            dredStats.del_time_prefill_update_ns += ns;
            break;
        case DredTimeBucket::DelLoopBody:
            dredStats.del_time_loop_body_ns += ns;
            if (auto* stats = getDredSccStats()) stats->del_time_loop_body_ns += ns;
            break;
        case DredTimeBucket::DelLoopExit:
            dredStats.del_time_loop_exit_ns += ns;
            break;
        case DredTimeBucket::DelLoopUpdate:
            dredStats.del_time_loop_update_ns += ns;
            if (auto* stats = getDredSccStats()) stats->del_time_loop_update_ns += ns;
            break;
        case DredTimeBucket::DelPostamble:
            dredStats.del_time_postamble_ns += ns;
            break;
        case DredTimeBucket::DelRecord:
            dredStats.del_time_record_ns += ns;
            break;
        case DredTimeBucket::DelOverdelete:
            dredStats.del_time_overdelete_ns += ns;
            break;
        case DredTimeBucket::DelDeltaUnion:
            dredStats.del_time_delta_union_ns += ns;
            break;
        case DredTimeBucket::DelRuleappErase:
            dredStats.del_time_ruleapp_erase_ns += ns;
            break;
        case DredTimeBucket::InsTotal:
            dredStats.ins_time_total_ns += ns;
            if (auto* stats = getDredSccStats()) stats->ins_time_total_ns += ns;
            break;
        case DredTimeBucket::InsPreamble:
            dredStats.ins_time_preamble_ns += ns;
            break;
        case DredTimeBucket::InsPrefill:
            dredStats.ins_time_prefill_ns += ns;
            break;
        case DredTimeBucket::InsPrefillUpdate:
            dredStats.ins_time_prefill_update_ns += ns;
            break;
        case DredTimeBucket::InsLoopBody:
            dredStats.ins_time_loop_body_ns += ns;
            if (auto* stats = getDredSccStats()) stats->ins_time_loop_body_ns += ns;
            break;
        case DredTimeBucket::InsLoopExit:
            dredStats.ins_time_loop_exit_ns += ns;
            break;
        case DredTimeBucket::InsLoopUpdate:
            dredStats.ins_time_loop_update_ns += ns;
            if (auto* stats = getDredSccStats()) stats->ins_time_loop_update_ns += ns;
            break;
        case DredTimeBucket::InsPostamble:
            dredStats.ins_time_postamble_ns += ns;
            break;
        case DredTimeBucket::InsRecord:
            dredStats.ins_time_record_ns += ns;
            break;
        case DredTimeBucket::InsDeltaUnion:
            dredStats.ins_time_delta_union_ns += ns;
            break;
        case DredTimeBucket::RedTotal:
            dredStats.red_time_total_ns += ns;
            if (auto* stats = getDredSccStats()) stats->red_time_total_ns += ns;
            break;
        case DredTimeBucket::RedLoopBody:
            dredStats.red_time_loop_body_ns += ns;
            if (auto* stats = getDredSccStats()) stats->red_time_loop_body_ns += ns;
            break;
        case DredTimeBucket::RedLoopExit:
            dredStats.red_time_loop_exit_ns += ns;
            break;
        case DredTimeBucket::RedLoopUpdate:
            dredStats.red_time_loop_update_ns += ns;
            if (auto* stats = getDredSccStats()) stats->red_time_loop_update_ns += ns;
            break;
        case DredTimeBucket::RedPostamble:
            dredStats.red_time_postamble_ns += ns;
            break;
    }
}

bool DerivationManager::ruleAppExistsInCompleteSet(
        const UntypedTuple& untypedTuple, const RuleApplication& ruleAppl) {
    auto it = untypedTuple2RuleApplications.find(untypedTuple);
    if (it == untypedTuple2RuleApplications.end() || it->second == nullptr) {
        return false;
    }
    if (it->second->find(ruleAppl) != it->second->end()) {
        return true;
    }
    return false;
}

std::unordered_set<UntypedTuple> inputFactSet;

bool isInputFact(UntypedTuple tuple) {
    return inputFactSet.count(tuple) != 0;
}

std::unordered_map<UntypedTuple, double> fact_prob;
std::unordered_map<std::string, bool> relationHasProbFact;
bool detOptEnabled = true;
std::unordered_map<std::string, bool> relationIsDet;

std::map<std::string, std::set<UntypedTuple>> initialInputRelations;

void dumpInitialInputRelations(std::string filename) {
    std::ostream* os;
    std::ofstream ofs;
    if (!filename.empty()) {
        ofs.open(filename);
        os = &ofs;
    } else {
        os = &std::cout;
    }
    for (const auto& [rel, tuples] : initialInputRelations) {
        if (tuples.empty()) continue;
        *os << "Relation: " << rel << '\n';
        for (const auto& tuple : tuples) {
            *os << "  " << tuple.toString() << '\n';
        }
    }
    if (ofs.is_open()) {
        ofs.close();
    }
}
