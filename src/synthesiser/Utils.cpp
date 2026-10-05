/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2022, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Utils.cpp
 *
 * Some utility functions for the synthesiser
 *
 ***********************************************************************/

#include "synthesiser/Utils.h"
#include <iomanip>
#include <sstream>
#include <string>

namespace souffle::synthesiser {

std::string toHex(const std::size_t V) {
    static_assert(sizeof(std::size_t) <= 64);
    std::stringstream s;
    s << std::hex << std::setw(16) << std::setfill('0') << V;
    return s.str();
}

std::string uniqueCppIdent(const std::string& name, std::size_t maxLength) {
    bool requiresHash = false;

    // strip leading numbers
    unsigned int i;
    for (i = 0; i < name.length(); ++i) {
        if ((isalnum(name.at(i)) != 0) || name.at(i) == '_') {
            break;
        }
        requiresHash = true;
    }
    std::string id;
    for (auto ch : name.substr(i)) {
        // alphanumeric characters are allowed
        if (isalnum(ch) != 0) {
            id += ch;
        }
        // all other characters are replaced by an underscore, except when
        // the previous character was an underscore as double underscores
        // in identifiers are reserved by the standard
        else if (id.empty() || id.back() != '_') {
            id += '_';
            requiresHash = true;
        }
        requiresHash = true;
    }
    // most compilers have a limit of 2048 characters (if they have a limit at all) for
    // identifiers; we use half of that for safety
    if (id.length() > maxLength) {
        requiresHash = true;
        id = id.substr(0, maxLength);
    }
    if (requiresHash) {
        id += "_" + toHex(std::hash<std::string>{}(name));
    }
    return id;
}

}  // namespace souffle::synthesiser