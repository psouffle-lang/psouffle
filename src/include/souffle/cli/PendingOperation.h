#ifndef SOUFFLE_CLI_PENDING_OPERATION_H
#define SOUFFLE_CLI_PENDING_OPERATION_H

#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace souffle::cli {

struct PendingOperation {
    enum Type { INSERT, DELETE } type;
    bool valid = true;
    std::string relationName;
    std::vector<std::string> values;
    double probability = 1.0;

    std::string toString() const {
        std::stringstream ss;
        ss << (type == INSERT ? "insert " : "delete ");
        if (type == INSERT) {
            ss << probability << "::";
        }
        ss << relationName << "(";
        for (size_t i = 0; i < values.size(); i++) {
            if (i > 0) {
                ss << ", ";
            }
            ss << values[i];
        }
        ss << ")";
        return ss.str();
    }
};

inline std::vector<std::string> parsePendingOperationValues(const std::string& valuesStr) {
    std::vector<std::string> values;
    std::regex valueRegex("\\s*([^,]+)\\s*,?");
    std::string::const_iterator searchStart(valuesStr.cbegin());
    std::smatch valueMatch;

    while (std::regex_search(searchStart, valuesStr.cend(), valueMatch, valueRegex)) {
        std::string value = valueMatch[1].str();
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        values.push_back(value);
        searchStart = valueMatch.suffix().first;
    }

    return values;
}

inline std::tuple<std::string, std::vector<std::string>, double, bool>
parseTupleWithOptionalProbability(const std::string& str) {
    std::string relName;
    std::vector<std::string> values;
    double probability = 1.0;
    bool success = false;

    std::regex prefixProbRegex("(0?\\.[0-9]+)\\s*::\\s*([a-zA-Z][a-zA-Z0-9_]*)\\s*\\(([^)]*)\\)");
    std::regex suffixProbRegex("([a-zA-Z][a-zA-Z0-9_]*)\\s*\\(([^)]*)\\)\\s*(0?\\.[0-9]+)");
    std::regex standardRegex("([a-zA-Z][a-zA-Z0-9_]*)\\s*\\(([^)]*)\\)");

    std::smatch matches;
    if (std::regex_search(str, matches, prefixProbRegex) && matches.size() > 3) {
        try {
            probability = std::stod(matches[1].str());
            if (probability < 0.0 || probability > 1.0) {
                return std::make_tuple("", std::vector<std::string>(), 0.0, false);
            }
            relName = matches[2].str();
            values = parsePendingOperationValues(matches[3].str());
            success = true;
        } catch (const std::exception&) {
            return std::make_tuple("", std::vector<std::string>(), 0.0, false);
        }
    } else if (std::regex_search(str, matches, suffixProbRegex) && matches.size() > 3) {
        try {
            relName = matches[1].str();
            values = parsePendingOperationValues(matches[2].str());
            probability = std::stod(matches[3].str());
            if (probability < 0.0 || probability > 1.0) {
                return std::make_tuple("", std::vector<std::string>(), 0.0, false);
            }
            success = true;
        } catch (const std::exception&) {
            return std::make_tuple("", std::vector<std::string>(), 0.0, false);
        }
    } else if (std::regex_search(str, matches, standardRegex) && matches.size() > 2) {
        relName = matches[1].str();
        values = parsePendingOperationValues(matches[2].str());
        success = true;
    }

    return std::make_tuple(relName, values, probability, success);
}

}  // namespace souffle::cli

#endif
