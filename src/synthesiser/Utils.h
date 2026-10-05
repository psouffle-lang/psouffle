/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2022, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Utils.h
 *
 * Some utility functions for the synthesiser
 *
 ***********************************************************************/

#pragma once

#include <cstddef>
#include <string>

namespace souffle::synthesiser {

/** return the hexadecimal representation of the given value, 16 characters long. */
std::string toHex(const std::size_t value);

/** return a unique C++ identifier for the given str string */
std::string uniqueCppIdent(const std::string& str, std::size_t maxLength = 1024);

}  // namespace souffle::synthesiser