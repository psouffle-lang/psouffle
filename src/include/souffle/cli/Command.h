#ifndef SOUFFLE_CLI_COMMAND_H
#define SOUFFLE_CLI_COMMAND_H

#include <sstream>
#include <string>
#include <vector>

namespace souffle::cli {

enum class CommandKind {
    HELP,
    INSERT,
    DELETE,
    LIST,
    SETMODE,
    SET,
    UNSET,
    SHOW,
    COMMIT,
    EXIT,
    UNKNOWN,
};

struct ParsedCommand {
    CommandKind kind = CommandKind::UNKNOWN;
    std::string verb;
    std::vector<std::string> args;
    std::string remainder;
};

inline std::string trimWhitespace(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return std::string();
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> tokens;
    std::istringstream input(text);
    std::string token;
    while (input >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

inline CommandKind classifyCommand(const std::string& cmd) {
    if (cmd == "help" || cmd == "h") {
        return CommandKind::HELP;
    }
    if (cmd == "insert") {
        return CommandKind::INSERT;
    }
    if (cmd == "delete" || cmd == "remove") {
        return CommandKind::DELETE;
    }
    if (cmd == "list") {
        return CommandKind::LIST;
    }
    if (cmd == "setmode") {
        return CommandKind::SETMODE;
    }
    if (cmd == "set") {
        return CommandKind::SET;
    }
    if (cmd == "unset") {
        return CommandKind::UNSET;
    }
    if (cmd == "show") {
        return CommandKind::SHOW;
    }
    if (cmd == "commit") {
        return CommandKind::COMMIT;
    }
    if (cmd == "exit" || cmd == "quit" || cmd == "q") {
        return CommandKind::EXIT;
    }
    return CommandKind::UNKNOWN;
}

inline ParsedCommand parseCommandLine(const std::string& command) {
    ParsedCommand parsed;
    parsed.remainder = trimWhitespace(command);
    if (parsed.remainder.empty()) {
        return parsed;
    }

    const auto split = parsed.remainder.find_first_of(" \t\r\n");
    if (split == std::string::npos) {
        parsed.verb = parsed.remainder;
        parsed.remainder.clear();
    } else {
        parsed.verb = parsed.remainder.substr(0, split);
        parsed.remainder = trimWhitespace(parsed.remainder.substr(split + 1));
    }
    parsed.kind = classifyCommand(parsed.verb);
    parsed.args = splitWords(parsed.remainder);
    return parsed;
}

inline std::string normalizeInputLine(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

}  // namespace souffle::cli

#endif
