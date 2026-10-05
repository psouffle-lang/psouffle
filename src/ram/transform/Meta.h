/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2018, The Souffle Developers. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Meta.h
 *
 * Defines the interface for RAM transformation passes.
 *
 ***********************************************************************/

#pragma once

#include "ram/transform/Transformer.h"

namespace souffle::ram::transform {

/**
 * @Class MetaTransformer
 * @Brief Abstract class to identifier meta transformer
 */
class MetaTransformer : public Transformer {};

}  // namespace souffle::ram::transform
