//
// Created by Hugh on 2024/11/20.
//
#pragma once
/** Seems useless, because the delta info is recorded in separated maps */
/** The relation itself is never extended */
#include "Relation.h"
#ifndef DELTARELATION_H
#define DELTARELATION_H

namespace souffle::ast {
/**
 * @class DeltaRelation
 */
class DeltaRelation : public Relation {

// TODO: Distinguish insertion and deletion
// the synthesized c++ will also I/O the delta relations, by extra suffix ".insert/.delete"
};
}

#endif //DELTARELATION_H
