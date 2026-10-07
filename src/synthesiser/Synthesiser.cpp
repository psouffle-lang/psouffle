/**
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file Synthesiser.cpp
 *
 * Implementation of the C++ synthesiser for RAM programs.
 *
 ***********************************************************************/

#include "synthesiser/Synthesiser.h"
#include "ast/BinaryConstraint.h"
#include "ast/IntrinsicAggregator.h"
#include "AggregateOp.h"
#include "FunctorOps.h"
#include "GenDb.h"
#include "Global.h"
#include "RelationTag.h"
#include "config.h"
#include "ram/AbstractParallel.h"
#include "ram/Aggregate.h"
#include "ram/Aggregator.h"
#include "ram/AutoIncrement.h"
#include "ram/Break.h"
#include "ram/Call.h"
#include "ram/Clear.h"
#include "ram/Condition.h"
#include "ram/Conjunction.h"
#include "ram/Constraint.h"
#include "ram/DebugInfo.h"
#include "ram/DeltaUnion.h"
#include "ram/EmptinessCheck.h"
#include "ram/EmptyStatement.h"
#include "ram/Erase.h"
#include "ram/Evidence.h"
#include "ram/ExistenceCheck.h"
#include "ram/Exit.h"
#include "ram/Expression.h"
#include "ram/False.h"
#include "ram/Filter.h"
#include "ram/FloatConstant.h"
#include "ram/IO.h"
#include "ram/IfExists.h"
#include "ram/IndexAggregate.h"
#include "ram/IndexIfExists.h"
#include "ram/IndexScan.h"
#include "ram/Insert.h"
#include "ram/IntrinsicAggregator.h"
#include "ram/IntrinsicOperator.h"
#include "ram/LogRelationTimer.h"
#include "ram/LogSize.h"
#include "ram/LogTimer.h"
#include "ram/Loop.h"
#include "ram/MergeExtend.h"
#include "ram/Negation.h"
#include "ram/NestedIntrinsicOperator.h"
#include "ram/NestedOperation.h"
#include "ram/Node.h"
#include "ram/Operation.h"
#include "ram/PackRecord.h"
#include "ram/Parallel.h"
#include "ram/ParallelAggregate.h"
#include "ram/ParallelIfExists.h"
#include "ram/ParallelIndexAggregate.h"
#include "ram/ParallelIndexIfExists.h"
#include "ram/ParallelIndexScan.h"
#include "ram/ParallelScan.h"
#include "ram/Program.h"
#include "ram/ProvenanceExistenceCheck.h"
#include "ram/Query.h"
#include "ram/RecordDerivation.h"
#include "ram/Relation.h"
#include "ram/RelationOperation.h"
#include "ram/RelationSize.h"
#include "ram/Scan.h"
#include "ram/SequantialOperation.h"
#include "ram/Sequence.h"
#include "ram/SignedConstant.h"
#include "ram/Statement.h"
#include "ram/SubroutineArgument.h"
#include "ram/SubroutineReturn.h"
#include "ram/Swap.h"
#include "ram/TranslationUnit.h"
#include "ram/True.h"
#include "ram/TupleElement.h"
#include "ram/TupleOperation.h"
#include "ram/UndefValue.h"
#include "ram/UnpackRecord.h"
#include "ram/UnsignedConstant.h"
#include "ram/UserDefinedAggregator.h"
#include "ram/UserDefinedOperator.h"
#include "ram/analysis/Index.h"
#include "ram/utility/Utils.h"
#include "ram/utility/Visitor.h"
#include "souffle/BinaryConstraintOps.h"
#include "souffle/RamTypes.h"
#include "souffle/TypeAttribute.h"
#include "souffle/problog/Atom.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/FileUtil.h"
#include "souffle/utility/MiscUtil.h"
#include "souffle/utility/StreamUtil.h"
#include "souffle/utility/StringUtil.h"
#include "souffle/utility/json11.h"
#include "souffle/utility/tinyformat.h"
#include "ast/TranslationUnit.h"
#include "ast/analysis/SCCGraph.h"
#include "ast/analysis/TopologicallySortedSCCGraph.h"
#include "reports/DebugReport.h"
#include "reports/ErrorReport.h"
#include "synthesiser/GenDb.h"
#include "synthesiser/Relation.h"
#include "synthesiser/Utils.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <unordered_set>
#include <ranges>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>
#include <ast/Constant.h>
#include <ast/IntrinsicFunctor.h>
#include <ast/Negation.h>
#include <ast/NumericConstant.h>
#include <ast/StringConstant.h>
#include <ast/Term.h>
#include <ast/UnnamedVariable.h>
#include <ast2ram/utility/Utils.h>
#include <souffle/SouffleInterface.h>

namespace souffle::synthesiser {
namespace {
std::string cppStringLiteral(const std::string& value) {
    return "\"" + souffle::stringify(value) + "\"";
}
std::vector<std::string> splitRenderedTupleFieldsForCodegen(const std::string& renderedFields) {
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
}

using json11::Json;
using ram::analysis::IndexAnalysis;
using namespace ram;
using namespace stream_write_qualified_char_as_number;

const std::string getBaseRelationName(const std::string& name) {
    auto res =
    stripPrefix("$inc_delta_tuple_delete_",
    stripPrefix("$inc_delta_tuple_insert_",
    stripPrefix("$inc_delta_derv_delete_",
    stripPrefix("$inc_delta_derv_insert_",
    stripPrefix("@tmp4_",
    stripPrefix("@tmp3_",
    stripPrefix("@tmp2_",
    stripPrefix("@tmp_",
    stripPrefix("@post_delete_",
    stripPrefix("@old_",
    stripPrefix("@new_",
    stripPrefix("@delta_",
    stripPrefix("@info_", name)))))))))))));
    res = stripPrefix("@delta_tuple_delete_", res);
    res = stripPrefix("@delta_tuple_insert_", res);
    res = stripPrefix("@new_derv_delete_", res);
    res = stripPrefix("@new_derv_insert_", res);
    res = stripPrefix( "@inc_tuple_overdelete_", res);
    res = stripPrefix( "@inc_derv_overdelete_", res);
    res = stripPrefix( "@inc_new_derv_rederive_", res);
    res = stripPrefix( "@inc_delta_tuple_rederive_", res);
    return res;
}

/** Lookup frequency counter */
unsigned Synthesiser::lookupFreqIdx(const std::string& txt) {
    static unsigned ctr;
    auto pos = idxMap.find(txt);
    if (pos == idxMap.end()) {
        return idxMap[txt] = ctr++;
    } else {
        return idxMap[txt];
    }
}

/** Lookup frequency counter */
std::size_t Synthesiser::lookupReadIdx(const std::string& txt) {
    std::string modifiedTxt = txt;
    std::replace(modifiedTxt.begin(), modifiedTxt.end(), '-', '.');
    static unsigned counter;
    auto pos = neIdxMap.find(modifiedTxt);
    if (pos == neIdxMap.end()) {
        return neIdxMap[modifiedTxt] = counter++;
    } else {
        return neIdxMap[modifiedTxt];
    }
}

/** Convert RAM identifier */
const std::string Synthesiser::convertRamIdent(const std::string& name) {
    auto it = identifiers.find(name);
    if (it != identifiers.end()) {
        return it->second;
    }
    std::string id = uniqueCppIdent(name);
    identifiers.insert(std::make_pair(name, id));
    return id;
}

const std::string Synthesiser::convertStratumIdent(const std::string& name) {
    return convertRamIdent(name);
}

/** Get relation name */
const std::string Synthesiser::getRelationName(const ram::Relation& rel) {
    return "rel_" + convertRamIdent(rel.getName());
}

const std::string Synthesiser::getRelationName(const ram::Relation* rel) {
    return "rel_" + convertRamIdent(rel->getName());
}

/** Get context name */
const std::string Synthesiser::getOpContextName(const ram::Relation& rel) {
    return getRelationName(rel) + "_op_ctxt";
}

/** Get relation type struct */
void Synthesiser::generateRelationTypeStruct(GenDb& db, Own<Relation> relationType) {
    std::string name = relationType->getTypeName();
    // If this type has been generated already, use the cached version
    if (typeCache.find(name) != typeCache.end()) {
        return;
    }
    typeCache.insert(name);

    // Generate the type struct for the relation
    relationType->generateTypeStruct(db);
}

/** Get referenced relations */
ram::RelationSet Synthesiser::getReferencedRelations(const Operation& op) {
    ram::RelationSet res;
    visit(op, [&](const Node& node) {
        if (auto scan = as<RelationOperation>(node)) {
            res.insert(lookup(scan->getRelation()));
        } else if (auto agg = as<Aggregate>(node)) {
            res.insert(lookup(agg->getRelation()));
        } else if (auto exists = as<ExistenceCheck>(node)) {
            res.insert(lookup(exists->getRelation()));
        } else if (auto provExists = as<ProvenanceExistenceCheck>(node)) {
            res.insert(lookup(provExists->getRelation()));
        } else if (auto insert = as<Insert>(node)) {
            res.insert(lookup(insert->getRelation()));
        } else if (auto derExists = as<DerivationCheck>(node)) {
            res.insert(lookup((derExists->getRelation())));
            for (auto& [_, expr] : derExists->varExprMap) {
                if (auto* rel = as<RelationOperation>(expr)) {

                    res.insert(lookup(rel->getRelation()));
                }
            }
        } else if (auto record = as<RecordDerivation>(node)) {
            res.insert(lookup(record->getRelation()));
            for (auto& [_, expr] : record->varExprMap) {
                if (auto* rel = as<RelationOperation>(expr)) {
                    res.insert(lookup(rel->getRelation()));
                }
            }
        }
    });
    return res;
}

std::optional<std::size_t> Synthesiser::compileRegex(const std::string& pattern) {
    auto i = regexes.find(pattern);
    if (i != regexes.end()) {
        return i->second;
    }
    try {
        const std::regex regex(pattern);
        std::size_t index = regexes.size();
        return regexes.emplace(pattern, index).first->second;
    } catch (const std::exception&) {
        std::cerr << "warning: wrong pattern provided \"" << pattern << "\"\n";
        return std::nullopt;
    }
}

struct DetOptMeta {
    std::vector<std::string> relNames;
    std::vector<std::size_t> relToScc;
    std::vector<std::vector<std::size_t>> sccSucc;
    std::vector<std::size_t> sccTopo;
    std::vector<int> ruleSeed;
};

static DetOptMeta buildDetOptMeta(const ast::Program& program, Global& glb) {
    DetOptMeta meta;
    auto* programPtr = dynamic_cast<ast::Program*>(program.cloneImpl().release());
    if (programPtr == nullptr) {
        return meta;
    }
    Own<ast::Program> programClone(programPtr);
    ErrorReport errors;
    DebugReport debug;
    ast::TranslationUnit tu(glb, std::move(programClone), errors, debug);
    auto& scc = tu.getAnalysis<ast::analysis::SCCGraphAnalysis>();
    auto& topo = tu.getAnalysis<ast::analysis::TopologicallySortedSCCGraphAnalysis>();
    auto& prog = tu.getProgram();

    auto relations = prog.getRelations();
    meta.relNames.reserve(relations.size());
    meta.relToScc.reserve(relations.size());
    meta.ruleSeed.assign(relations.size(), 0);

    std::unordered_map<const ast::Relation*, std::size_t> relIndex;
    relIndex.reserve(relations.size());
    std::size_t idx = 0;
    for (const auto* rel : relations) {
        relIndex.emplace(rel, idx++);
        meta.relNames.push_back(rel->getQualifiedName().toString());
        meta.relToScc.push_back(scc.getSCC(rel));
    }

    for (const auto* clause : prog.getClauses()) {
        if (clause == nullptr) {
            continue;
        }
        double p = clause->getProbability();
        // A zero-weight rule also needs provenance: the RAM evaluation
        // derives its head, but that tuple is false in every possible world.
        if (p == 1.0) {
            continue;
        }
        const auto* headAtom = as<ast::Atom>(clause->getHead());
        if (headAtom == nullptr) {
            continue;
        }
        const auto* headRel = prog.getRelation(*headAtom);
        auto it = relIndex.find(headRel);
        if (it != relIndex.end()) {
            meta.ruleSeed[it->second] = 1;
        }
    }

    meta.sccSucc.resize(scc.getNumberOfSCCs());
    for (std::size_t sid = 0; sid < meta.sccSucc.size(); ++sid) {
        for (auto succ : scc.getSuccessorSCCs(sid)) {
            meta.sccSucc[sid].push_back(succ);
        }
    }
    meta.sccTopo = topo.order();

    return meta;
}

void Synthesiser::emitRules (std::ostream& out) {
    // out << "class RuleComponents {" << std::endl;
    std::size_t anonVarCounter = 0;
    std::vector<std::string> ruleNames;
    // const auto& initialClauses = this->initialAstProgram->getClauses();
    const auto& newClauses = this->newAstProgram->getClauses();
    auto encodeArgumentField = [&](ast::Argument* field, std::size_t ruleId, std::size_t atomId,
                                   std::size_t argIdx, const std::string& relName,
                                   bool preserveUnnamed = false) -> std::pair<char, std::string> {
        if (isA<ast::Variable>(field)) {
            auto var = as<ast::Variable>(field);
            return {'V', var->getName()};
        }
        if (isA<ast::UnnamedVariable>(field)) {
            if (preserveUnnamed) {
                return {'W', "_"};
            }
            return {'V', "__anon_" + std::to_string(ruleId) + "_" +
                                 std::to_string(atomId) + "_" +
                                 std::to_string(argIdx) + "_" +
                                 std::to_string(anonVarCounter++)};
        }
        if (isA<ast::NumericConstant>(field)) {
            auto constant = as<ast::NumericConstant>(field);
            if (!constant->getFixedType().has_value()) {
                return {'I', constant->getConstant()};
            }
            switch (constant->getFixedType().value()) {
                case ast::NumericConstant::Type::Int: return {'I', constant->getConstant()};
                case ast::NumericConstant::Type::Float: return {'F', constant->getConstant()};
                case ast::NumericConstant::Type::Uint: return {'U', constant->getConstant()};
            }
        }
        if (isA<ast::StringConstant>(field)) {
            auto constant = as<ast::StringConstant>(field);
            return {'S', constant->getConstant()};
        }
        if (isA<ast::IntrinsicFunctor>(field)) {
            auto fieldFunctor = as<ast::IntrinsicFunctor>(field);
            return {'E', fieldFunctor->serialize()};
        }
        std::cout << relName << std::endl;
        assert(false && "Not impl yet, atom");
        return {'I', "0"};
    };
    auto emitEncodedField = [&](std::ostream& os, const std::pair<char, std::string>& encoded) {
        const auto& [tag, field] = encoded;
        switch (tag) {
            case 'V':
                os << "SymbolicField::makeVariable(\"" << field << "\")";
                break;
            case 'W':
                os << "SymbolicField::makeUnnamedVariable()";
                break;
            case 'I':
            case 'F':
            case 'U':
                os << "SymbolicField{" << field << "}";
                break;
            case 'S':
                os << "SymbolicField{StringField{" << cppStringLiteral(field) << "}}";
                break;
            case 'E':
                os << "SymbolicField(std::shared_ptr<ExprField>(" << field << "))";
                break;
            default:
                assert(false && "Unknown field type");
        }
    };
    auto emitAtomInline = [&](std::ostream& os, const ast::Atom& atom, std::size_t ruleId,
                              std::size_t atomId, bool preserveUnnamed = false) {
        std::vector<std::pair<char, std::string>> fields;
        const auto args = atom.getArguments();
        for (std::size_t argIdx = 0; argIdx < args.size(); ++argIdx) {
            fields.push_back(encodeArgumentField(
                    args[argIdx], ruleId, atomId, argIdx, atom.getQualifiedName().toString(), preserveUnnamed));
        }
        os << "Atom{\"" << atom.getQualifiedName().toString() << "\", {";
        for (size_t j = 0; j < fields.size(); ++j) {
            emitEncodedField(os, fields[j]);
            if (j + 1 != fields.size()) {
                os << ", ";
            }
        }
        os << "}}";
    };
    auto encodeArgumentAsSymbolicField = [&](ast::Argument* arg, std::size_t ruleId,
                                             std::size_t atomId) -> std::pair<char, std::string> {
        return encodeArgumentField(arg, ruleId, atomId, 0, "<aggregate-expr>", true);
    };
    // assert (initialClauses.size() == newClauses.size() && "Initial and new program should have the same number of clauses; no optimization for now");
    for (size_t i = 0; i < newClauses.size(); ++i) {
        // auto& initClause = initialClauses[i];
        auto& clause = newClauses[i];
        // if (ast::isFact(*clause)) {
        //     continue;
        // }
        // std::cout << clause->getProbability() << std::endl;
        auto ruleId = clause->getClauseId();
        std::size_t atomId = 0;
        std::vector<std::string> atomNames;
        std::vector<std::string> aggregateNames;
        std::string headAtomName = "rule" + std::to_string(ruleId) + "_head";
        std::string headRelName;
        {
            ast::Atom* headAtom = as<ast::Atom>(clause->getHead());
            headRelName = headAtom->getQualifiedName().toString();
            std::vector<std::pair<char, std::string>> fields;
            // std::vector<std::string> fieldVars{};  // only consider variables for now
            // TODO: change to use serialize()
            for (auto field: headAtom->getArguments()) {
                if (isA<ast::Variable>(field)) {
                    auto var = as<ast::Variable>(field);
                    fields.emplace_back('V', var->getName());
                } else if (isA<ast::NumericConstant>(field)) {
                    auto constant = as<ast::NumericConstant>(field);
                    if (!constant->getFixedType().has_value()) {
                        // assert (false && "Fixed type should be set for numeric constant");
                        fields.emplace_back('I', constant->getConstant());
                        continue;
                    }
                    switch (constant->getFixedType().value()) {
                        case ast::NumericConstant::Type::Int:
                            fields.emplace_back('I', constant->getConstant());
                            break;
                        case ast::NumericConstant::Type::Float:
                            fields.emplace_back('F', constant->getConstant());
                            break;
                        case ast::NumericConstant::Type::Uint:
                            fields.emplace_back('U', constant->getConstant());
                            break;
                    }
                } else if (isA<ast::StringConstant>(field)) {
                    auto constant = as<ast::StringConstant>(field);
                    fields.emplace_back('S', constant->getConstant());
                } else if (isA<ast::IntrinsicFunctor>(field)) {
                    // rec_rand_var(Var_z,Times,(X||Y)) :- ...
                    auto fieldFunctor = as<ast::IntrinsicFunctor>(field);
                    fields.emplace_back('E', fieldFunctor->serialize());
                } else {
                    std::cout << headAtom->getQualifiedName().toString() << std::endl;
                    assert (false && "Not impl yet, atom");
                    // TODO: can we just omit all constraints?
                    continue;
                }
            }
            out << "const Atom " << headAtomName << " = Atom(\"" << headRelName << "\", std::vector<SymbolicField>{";
            for (size_t ii = 0; ii < fields.size(); ++ii) {
                const auto& [tag, field] = fields[ii];
                switch (tag) {
                    case 'V':
                        out << "SymbolicField::makeVariable(\"" << field << "\")";
                        break;
                    case 'I':
                    case 'F':
                    case 'U':
                        out << "SymbolicField{" << field << "}"; break;
                    case 'S':
                        out << "SymbolicField{StringField{" << cppStringLiteral(field) << "}}"; break;
                    case 'E':
                        out << "SymbolicField(std::shared_ptr<ExprField>(" << field << "))"; break;
                        // out << field << ", "; break;
                    default:
                        assert (false && "Unknown field type");
                }
                if (ii + 1 != fields.size()) {
                    out << ", ";
                }
            }
            out << "});" << std::endl;
        }
        // const auto& initialBodyLiterals = initClause->getBodyLiterals();
        const auto& bodyLiterals = clause->getBodyLiterals();
        for (size_t ii = 0; ii < bodyLiterals.size(); ++ii) {
            auto& bodyLiteral = bodyLiterals[ii];
            atomId ++;
            if (isA<ast::Atom>(bodyLiteral)) {
                ast::Atom* atom = as<ast::Atom>(bodyLiteral);
                std::vector<std::pair<char, std::string>> fields;
                const auto args = atom->getArguments();
                for (std::size_t argIdx = 0; argIdx < args.size(); ++argIdx) {
                    auto* field = args[argIdx];
                    if (isA<ast::Variable>(field)) {
                        auto var = as<ast::Variable>(field);
                        fields.emplace_back('V', var->getName());
                    } else if (isA<ast::UnnamedVariable>(field)) {
                        fields.emplace_back('V', "__anon_" + std::to_string(ruleId) + "_" +
                                                     std::to_string(atomId) + "_" +
                                                     std::to_string(argIdx) + "_" +
                                                     std::to_string(anonVarCounter++));
                    } else if (isA<ast::NumericConstant>(field)) {
                        auto constant = as<ast::NumericConstant>(field);
                        if (!constant->getFixedType().has_value()) {
                            fields.emplace_back('I', constant->getConstant());
                            continue;
                        }
                        switch (constant->getFixedType().value()) {
                            case ast::NumericConstant::Type::Int:
                                fields.emplace_back('I', constant->getConstant());
                                break;
                            case ast::NumericConstant::Type::Float:
                                fields.emplace_back('F', constant->getConstant());
                                break;
                            case ast::NumericConstant::Type::Uint:
                                fields.emplace_back('U', constant->getConstant());
                                break;
                        }
                    } else if (isA<ast::StringConstant>(field)) {
                        auto constant = as<ast::StringConstant>(field);
                        fields.emplace_back('S', constant->getConstant());
                    } else if (isA<ast::IntrinsicFunctor>(field)) {
                        auto fieldFunctor = as<ast::IntrinsicFunctor>(field);
                        fields.emplace_back('E', fieldFunctor->serialize());
                    } else {
                        std::cout << atom->getQualifiedName().toString() << std::endl;
                        assert (false && "Not impl yet, atom");
                    }
                }
                std::string atomName = "atom_" + std::to_string(ruleId) + "_" + std::to_string(atomId);
                std::string res = atom->getQualifiedName().toString();
                atomNames.push_back(atomName);
                out << "const Atom " << atomName << " = Atom{\"" << res << "\", {";
                for (size_t j = 0; j < fields.size(); ++j) {
                    const auto& [tag, field] = fields[j];
                    switch (tag) {
                        case 'V':
                            out << "SymbolicField::makeVariable(\"" << field << "\")";
                            break;
                        case 'I':
                        case 'F':
                        case 'U':
                            out << "SymbolicField{" << field << "}";
                            break;
                        case 'S':
                            out << "SymbolicField{StringField{" << cppStringLiteral(field) << "}}";
                            break;
                        case 'E':
                            out << "SymbolicField(std::shared_ptr<ExprField>(" << field << "))";
                            break;
                        default:
                            assert (false && "Unknown field type");
                    }
                    if (j + 1 != fields.size()) {
                        out << ", ";
                    }
                }
                out << "}};" << std::endl;
            } else if (isA<ast::Negation>(bodyLiteral)) {
                ast::Negation* negation = as<ast::Negation>(bodyLiteral);
                ast::Atom* atom = negation->getAtom();
                std::vector<std::pair<char, std::string>> fields;
                const auto args = atom->getArguments();
                for (std::size_t argIdx = 0; argIdx < args.size(); ++argIdx) {
                    auto* field = args[argIdx];
                    if (isA<ast::Variable>(field)) {
                        auto var = as<ast::Variable>(field);
                        fields.emplace_back('V', var->getName());
                    } else if (isA<ast::UnnamedVariable>(field)) {
                        fields.emplace_back('V', "__anon_" + std::to_string(ruleId) + "_" +
                                                     std::to_string(atomId) + "_" +
                                                     std::to_string(argIdx) + "_" +
                                                     std::to_string(anonVarCounter++));
                    } else if (isA<ast::NumericConstant>(field)) {
                        auto constant = as<ast::NumericConstant>(field);
                        if (!constant->getFixedType().has_value()) {
                            fields.emplace_back('I', constant->getConstant());
                            continue;
                        }
                        switch (constant->getFixedType().value()) {
                            case ast::NumericConstant::Type::Int:
                                fields.emplace_back('I', constant->getConstant());
                                break;
                            case ast::NumericConstant::Type::Float:
                                fields.emplace_back('F', constant->getConstant());
                                break;
                            case ast::NumericConstant::Type::Uint:
                                fields.emplace_back('U', constant->getConstant());
                                break;
                        }
                    } else if (isA<ast::StringConstant>(field)) {
                        auto constant = as<ast::StringConstant>(field);
                        fields.emplace_back('S', constant->getConstant());
                    } else if (isA<ast::IntrinsicFunctor>(field)) {
                        auto fieldFunctor = as<ast::IntrinsicFunctor>(field);
                        fields.emplace_back('E', fieldFunctor->serialize());
                    } else {
                        std::cout << atom->getQualifiedName().toString() << std::endl;
                        assert (false && "Not impl yet, atom");
                    }
                }
                std::string atomName = "atom_" + std::to_string(clause->getClauseId()) + "_" + std::to_string(atomId);
                atomNames.push_back(atomName);
                out << "static const Atom " << atomName << " = Atom{\"" << atom->getQualifiedName().toString() << "\", {";
                for (size_t j = 0; j < fields.size(); ++j) {
                    const auto& [tag, field] = fields[j];
                    switch (tag) {
                        case 'V':
                            out << "SymbolicField::makeVariable(\"" << field << "\")";
                            break;
                        case 'I':
                        case 'F':
                        case 'U':
                            out << "SymbolicField{" << field << "}";
                            break;
                        case 'S':
                            out << "SymbolicField{StringField{" << cppStringLiteral(field) << "}}";
                            break;
                        case 'E':
                            out << "SymbolicField(std::shared_ptr<ExprField>(" << field << "))";
                            break;
                        default:
                            assert (false && "Unknown field type");
                    }
                    if (j + 1 != fields.size()) {
                        out << ", ";
                    }
                }
                out << "}, true};" << std::endl;
            } else if (isA<ast::Constraint>(bodyLiteral)) {
                auto* constraint = as<ast::Constraint>(bodyLiteral);
                if (auto* binary = as<ast::BinaryConstraint>(constraint)) {
                    if (binary->getBaseOperator() == BinaryConstraintOp::EQ) {
                        ast::IntrinsicAggregator* aggregate = nullptr;
                        ast::Variable* resultVar = nullptr;
                        if (isA<ast::IntrinsicAggregator>(binary->getLHS()) &&
                                isA<ast::Variable>(binary->getRHS())) {
                            aggregate = as<ast::IntrinsicAggregator>(binary->getLHS());
                            resultVar = as<ast::Variable>(binary->getRHS());
                        } else if (isA<ast::Variable>(binary->getLHS()) &&
                                   isA<ast::IntrinsicAggregator>(binary->getRHS())) {
                            aggregate = as<ast::IntrinsicAggregator>(binary->getRHS());
                            resultVar = as<ast::Variable>(binary->getLHS());
                        }
                        if (aggregate != nullptr && resultVar != nullptr &&
                                aggregate->getBaseOperator() == AggregateOp::SUM) {
                            const auto aggBody = aggregate->getBodyLiterals();
                            assert(aggBody.size() == 1 && "Only single-literal SUM aggregates are supported");
                            assert(isA<ast::Atom>(aggBody[0]) && "Only positive-atom SUM aggregates are supported");
                            auto* witnessAtom = as<ast::Atom>(aggBody[0]);
                            assert(aggregate->getTargetExpression() != nullptr &&
                                    "SUM aggregate target expression required");
                            std::string aggregateName =
                                    "agg_" + std::to_string(ruleId) + "_" + std::to_string(aggregateNames.size());
                            aggregateNames.push_back(aggregateName);
                            out << "const AggregateSpec " << aggregateName << " = AggregateSpec("
                                << cppStringLiteral("sum") << ", "
                                << cppStringLiteral(resultVar->getName()) << ", ";
                            emitAtomInline(out, *witnessAtom, ruleId, atomId, true);
                            out << ", ";
                            emitEncodedField(out,
                                    encodeArgumentAsSymbolicField(aggregate->getTargetExpression(), ruleId, atomId));
                            out << ");" << std::endl;
                        }
                    }
                }
                continue;
            } else {
                assert (false && "Not impl yet, atom");
            }
        }
        std::string ruleName = "rule" + std::to_string(ruleId);
        ruleNames.push_back(ruleName);
        bool isEqrelHead = false;
        if (!headRelName.empty()) {
            if (auto* headRel = newAstProgram->getRelation(ast::QualifiedName::fromString(headRelName))) {
                isEqrelHead = headRel->getRepresentation() == RelationRepresentation::EQREL;
            }
        }
        out << "const Rule " << ruleName << " = Rule(" << std::to_string(ruleId) + "," + headAtomName
        << ", {" << join(atomNames, ", ") << "}, "
        << "{" << join(map(clause->getVariables(),
            [](const std::string& s) { return "\"" + s + "\"";}), ", ") << "}, "
        << std::setprecision(std::numeric_limits<double>::max_digits10) << clause->getProbability()
        << ", " << std::to_string(clause->isRecursive())
        << ", " << std::to_string(clause->isInRecursiveStratum())
        << ", " << (isEqrelHead ? "true" : "false")
        << ", {" << join(aggregateNames, ", ") << "}"
        << ");" << std::endl;
    }
    std::vector<std::string> eqrelNames;
    for (const auto* rel : this->newAstProgram->getRelations()) {
        if (rel->getRepresentation() == RelationRepresentation::EQREL) {
            eqrelNames.push_back("\"" + rel->getQualifiedName().toString() + "\"");
        }
    }

    // Aggregate replay uses this runtime-only relation. A user declaration can
    // collide with those states, invalidating the compiler DAG certificate.
    const bool compilerRecursionMetadata = eqrelNames.empty() &&
            newAstProgram->getRelation(ast::QualifiedName::fromString("__agg_sum_state")) == nullptr;
    out << "ruleManager = RuleManager({" << join(ruleNames, ", ") << "}"
        << ", {" << join(eqrelNames, ", ") << "}, "
        << (compilerRecursionMetadata ? "true" : "false") << ");" << std::endl;
        // out << "std::cout << ruleManager.toString();\n";
    const auto& queries = this->newAstProgram->getProbQueries();
    std::vector<std::string> queryNames;

    for (const auto& queryPtr : queries) {
        if (!queryPtr) continue;
        const auto& query = *queryPtr;

        const auto& atom = query.getAtom();
        std::vector<std::string> fieldStrs;

        for (auto* arg : atom.getArguments()) {
            if (isA<ast::Variable>(arg)) {
                auto var = as<ast::Variable>(arg);
                fieldStrs.push_back("SymbolicField::makeVariable(\"" + var->getName() + "\")");
            } else if (isA<ast::UnnamedVariable>(arg)) {
                fieldStrs.push_back("SymbolicField::makeUnnamedVariable()");
            } else if (isA<ast::NumericConstant>(arg)) {
                auto c = as<ast::NumericConstant>(arg);
                fieldStrs.push_back("SymbolicField{" + c->getConstant() + "}");
            } else if (isA<ast::StringConstant>(arg)) {
                auto s = as<ast::StringConstant>(arg);
                fieldStrs.push_back("SymbolicField{StringField{" + cppStringLiteral(s->getConstant()) + "}}");
            } else if (isA<ast::IntrinsicFunctor>(arg)) {
                auto f = as<ast::IntrinsicFunctor>(arg);
                fieldStrs.push_back("SymbolicField(std::shared_ptr<ExprField>(" + f->serialize() + "))");
            }
        }

        std::string queryName =
            "query_" + atom.getQualifiedName().toString() + "_";
        queryNames.push_back(queryName);

        out << "const Query " << queryName
            << " = Query(Atom(\"" << atom.getQualifiedName().toString()
            << "\", {" << join(fieldStrs, ", ") << "}));\n";
    }
    out << "QueryManager queryManager = QueryManager({" << join(queryNames, ", ") << "});" << std::endl;
}

void Synthesiser::emitProblogPipeline(std::ostream& out) {
    if (glb.config().has("full-only")) {
        out << "souffle::problog::runFullPipeline(opt, obj, ruleManager, queryManager, fact_prob, evidences);\n";
        return;
    }
    if (glb.config().has("inc-only")) {
        out << "souffle::problog::runPipeline(opt, obj, ruleManager, queryManager, fact_prob, evidences, true);\n";
        return;
    }
    out << "if (opt.isOnlineExecution()) {\n"
        << "souffle::problog::runPipeline(opt, obj, ruleManager, queryManager, fact_prob, evidences, true);\n"
        << "} else {\n"
        << "souffle::problog::runFullPipeline(opt, obj, ruleManager, queryManager, fact_prob, evidences);\n"
        << "}\n";
}

void Synthesiser::emitCode(std::ostream& out, const Statement& stmt) {
    // if (stmt == nullptr) {
    //     return;
    // }
    class CodeEmitter : public ram::Visitor<void, Node const, std::ostream&> {
        using ram::Visitor<void, Node const, std::ostream&>::visit_;

    private:
        Synthesiser& synthesiser;
        Global& glb;
        IndexAnalysis* const isa = &synthesiser.getTranslationUnit().getAnalysis<IndexAnalysis>();

// macros to add comments to generated code for debugging
#ifndef PRINT_BEGIN_COMMENT
#define PRINT_BEGIN_COMMENT(os)                                          \
    if (glb.config().has("debug-report") || glb.config().has("verbose")) \
    os << "/* BEGIN " << __FUNCTION__ << " @" << __FILE__ << ":" << __LINE__ << " */\n"
#endif

#ifndef PRINT_END_COMMENT
#define PRINT_END_COMMENT(os)                                            \
    if (glb.config().has("debug-report") || glb.config().has("verbose")) \
    os << "/* END " << __FUNCTION__ << " @" << __FILE__ << ":" << __LINE__ << " */\n"
#endif

        // used to populate tuple literal init expressions
        std::function<void(std::ostream&, const Expression*)> rec;
        std::function<void(std::ostream&, const Expression*)> recWithDefault;

        std::ostringstream preamble;
        bool preambleIssued = false;

    public:
        CodeEmitter(Synthesiser& syn) : synthesiser(syn), glb(synthesiser.glb) {
            rec = [&](auto& out, const auto* value) {
                out << "ramBitCast(";
                dispatch(*value, out);
                out << ")";
            };
            recWithDefault = [&](auto& out, const auto* value) {
                if (!isUndefValue(&*value)) {
                    rec(out, value);
                } else {
                    out << "0";
                }
            };
        }

        std::pair<std::stringstream, std::stringstream> getPaddedRangeBounds(const ram::Relation& rel,
                const std::vector<Expression*>& rangePatternLower,
                const std::vector<Expression*>& rangePatternUpper) {
            std::stringstream low;
            std::stringstream high;

            // making this distinction for provenance
            std::size_t realArity = rel.getArity();
            std::size_t arity = rangePatternLower.size();

            low << "Tuple<RamDomain," << realArity << ">{{";
            high << "Tuple<RamDomain," << realArity << ">{{";

            for (std::size_t column = 0; column < arity; column++) {
                std::string supremum;
                std::string infimum;

                switch (rel.getAttributeTypes()[column][0]) {
                    case 'f':
                        supremum = "ramBitCast<RamDomain>(MIN_RAM_FLOAT)";
                        infimum = "ramBitCast<RamDomain>(MAX_RAM_FLOAT)";
                        break;
                    case 'u':
                        supremum = "ramBitCast<RamDomain>(MIN_RAM_UNSIGNED)";
                        infimum = "ramBitCast<RamDomain>(MAX_RAM_UNSIGNED)";
                        break;
                    default:
                        supremum = "ramBitCast<RamDomain>(MIN_RAM_SIGNED)";
                        infimum = "ramBitCast<RamDomain>(MAX_RAM_SIGNED)";
                }

                // if we have an inequality where either side is not set
                if (column != 0) {
                    low << ", ";
                    high << ", ";
                }

                if (isUndefValue(rangePatternLower[column])) {
                    low << supremum;
                } else {
                    low << "ramBitCast(";
                    dispatch(*rangePatternLower[column], low);
                    low << ")";
                }

                if (isUndefValue(rangePatternUpper[column])) {
                    high << infimum;
                } else {
                    high << "ramBitCast(";
                    dispatch(*rangePatternUpper[column], high);
                    high << ")";
                }
            }

            low << "}}";
            high << "}}";
            return std::make_pair(std::move(low), std::move(high));
        }

        // -- relation statements --

        void visit_(type_identity<IO>, const IO& io, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);

            synthesiser.currentClass->addInclude("\"souffle/io/IOSystem.h\"", true);

            // print directives as C++ initializers
            auto printDirectives = [&](const std::map<std::string, std::string>& registry) {
                auto cur = registry.begin();
                if (cur == registry.end()) {
                    return;
                }
                out << "{{\"" << cur->first << "\",\"" << escape(cur->second) << "\"}";
                ++cur;
                for (; cur != registry.end(); ++cur) {
                    out << ",{\"" << cur->first << "\",\"" << escape(cur->second) << "\"}";
                }
                out << '}';
            };

            const auto& directives = io.getDirectives();

            const std::string& op = io.get("operation");
            const std::string& inc = io.get("incDelta");

            out << "if (performIO) {\n";

            // get some table details
            if (op == "input") {
                out << "try {";
                out << "std::map<std::string, std::string> directiveMap(";
                printDirectives(directives);
                out << ");\n";
                out << R"_(if (!inputDirectory.empty()) {)_";
                out << R"_(directiveMap["fact-dir"] = inputDirectory;)_";
                out << "}\n";
                out << "{\n";
                out << "FunctionTimer timer(\"reading relation "<< synthesiser.getRelationName(synthesiser.lookup(io.getRelation()))<< "\");\n";
                if (inc == "false") {
                    out << "IOSystem::getInstance().getReader(";
                    out << "directiveMap, symTable, recordTable";
                    out << ")->readAll(*" << synthesiser.getRelationName(synthesiser.lookup(io.getRelation()));
                    out << ");\n";
                    const std::string& cache = io.get("cache");
                    // we cache all input facts to a set
                    if (cache == "true") {
                        out << "for (auto& tuple: *" << synthesiser.getRelationName(synthesiser.lookup(io.getRelation()))
                            << ") {" << std::endl;
                            out << "auto untypedTuple = UntypedTuple::fromTypedTuple(\"" << getBaseRelationName(io.getRelation()) << "\",tuple);\n";
                            out << "inputFactSet.insert(untypedTuple);\n";
                            out << "initialInputRelations[\"" << getBaseRelationName(io.getRelation()) <<"\"].insert(untypedTuple);\n";
                        out << "}" << std::endl;
                    }
                } else {
                    const std::string& isInsert = io.get("inc-insert");
                    const std::string& isDelete = io.get("inc-delete");
                    out << "IOSystem::getInstance().getReader(";
                    out << "directiveMap, symTable, recordTable";
                    out << ")->readAll(*" << synthesiser.getRelationName(synthesiser.lookup(io.getRelation()));
                    out << ");\n";
                    assert(!(isInsert == "true" && isDelete == "true") && "no same-time insertion and deletion");
                }
                out << "}\n";
                out << "} catch (std::exception& e) {std::cerr << \"Error loading " << io.getRelation()
                    << " data: \" << e.what() "
                       "<< "
                       "'\\n';\nexit(1);\n}\n";
            } else if (op == "output" || op == "printsize") {
                out << "try {";
                out << "std::map<std::string, std::string> directiveMap(";
                printDirectives(directives);
                out << ");\n";
                out << R"_(if (outputDirectory == "-"){)_";
                out << R"_(directiveMap["IO"] = "stdout"; directiveMap["headers"] = "true";)_";
                out << "}\n";
                out << R"_(else if (!outputDirectory.empty()) {)_";
                out << R"_(directiveMap["output-dir"] = outputDirectory;)_";
                out << "}\n";
                out << "{\n";
                    out << "FunctionTimer timer(\"writing relation "<< synthesiser.getRelationName(synthesiser.lookup(io.getRelation()))<< "\");\n";
                    out << "IOSystem::getInstance().getWriter(";
                    out << "directiveMap, symTable, recordTable";
                    out << ")->writeAll(*" << synthesiser.getRelationName(synthesiser.lookup(io.getRelation()))
                        << ");\n";
                out << "}\n";
                out << "} catch (std::exception& e) {std::cerr << e.what();exit(1);}\n";
            } else {
                assert("Wrong i/o operation");
            }
            out << "}\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Query>, const Query& query, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);

            // split terms of conditions of outer filter operation
            // into terms that require a context and terms that
            // do not require a context
            const Operation* next = &query.getOperation();
            VecOwn<Condition> requireCtx;
            VecOwn<Condition> freeOfCtx;
            if (const auto* filter = as<Filter>(query.getOperation())) {
                next = &filter->getOperation();
                // Check terms of outer filter operation whether they can be pushed before
                // the context-generation for speed imrovements
                // Split context-free filters before context generation for cheaper checks.
                auto conditions = toConjunctionList(&filter->getCondition());
                for (auto const& cur : conditions) {
                    bool needContext = false;
                    visit(*cur, [&](const ExistenceCheck&) { needContext = true; });
                    visit(*cur, [&](const ProvenanceExistenceCheck&) { needContext = true; });
                    if (needContext) {
                        requireCtx.push_back(clone(cur));
                    } else {
                        freeOfCtx.push_back(clone(cur));
                    }
                }
                // discharge conditions that do not require a context
                if (freeOfCtx.size() > 0) {
                    out << "if(";
                    dispatch(*toCondition(freeOfCtx), out);
                    out << ") {\n";
                }
            }

            // outline each search operation to improve compilation time
            out << "[&]()";
            // enclose operation in its own scope
            out << "{\n";

            // check whether loop nest can be parallelized
            bool isParallel = visitExists(
                    *next, [&](const Node& n) { return as<AbstractParallel, AllowCrossCast>(n); });

            // reset preamble
            preamble.str("");
            preamble.clear();
            preambleIssued = false;

            // create operation contexts for this operation
            for (const ram::Relation* rel : synthesiser.getReferencedRelations(query.getOperation())) {
                preamble << "CREATE_OP_CONTEXT(" << synthesiser.getOpContextName(*rel);
                preamble << "," << synthesiser.getRelationName(*rel);
                preamble << "->createContext());\n";
            }

            // discharge conditions that require a context
            if (isParallel) {
                if (requireCtx.size() > 0) {
                    preamble << "if(";
                    dispatch(*toCondition(requireCtx), preamble);
                    preamble << ") {\n";
                    dispatch(*next, out);
                    out << "}\n";
                } else {
                    dispatch(*next, out);
                }
            } else {
                out << preamble.str();
                if (requireCtx.size() > 0) {
                    out << "if(";
                    dispatch(*toCondition(requireCtx), out);
                    out << ") {\n";
                    dispatch(*next, out);
                    out << "}\n";
                } else {
                    dispatch(*next, out);
                }
            }

            if (isParallel) {
                out << "PARALLEL_END\n";  // end parallel
            }

            out << "}\n";
            out << "();";  // call lambda

            if (freeOfCtx.size() > 0) {
                out << "}\n";
            }

            PRINT_END_COMMENT(out);
        }



        void visit_(type_identity<Clear>, const Clear& clear, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);

            auto relation = synthesiser.lookup(clear.getRelation());
            // bool isIntermediate =
            //     !contains(synthesiser.storeRelations, Relation->getName()) && !Relation->isTemp();

            // if (isIntermediate) {
            //     out << "if (pruneImdtRels) ";
            // }
            // std::cout << "purging " << clear.getRelation() << std::endl
            //                 << synthesiser.getRelationName(relation) << std::endl;
            if (relation->isTemp()) {
                out << synthesiser.getRelationName(relation) << "->purge();\n";
            } else {
                // Concrete relations are preserved here; ExactClear handles explicit full purges.
                // out << synthesiser.getRelationName(relation) << "->purge();\n";
            }

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<ExactClear>, const ExactClear& clear, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            auto relation = synthesiser.lookup(clear.getRelation());
            out << synthesiser.getRelationName(relation) << "->purge();\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<LogSize>, const LogSize& size, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            const std::string& message = size.getMessage();
            constexpr const char* dredLoopPrefix = "@dred-loop;";
            const auto* rel = synthesiser.lookup(size.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            if (message.rfind(dredLoopPrefix, 0) == 0) {
                out << "if (dredProfileEnabled) {\n";
                out << "auto __dred_loop_size = " << relName << "->size();\n";
                out << "std::cout << \"[dred-loop] " << message
                    << " iter=\" << iter << \" size=\" << __dred_loop_size << std::endl;\n";
                out << "}\n";
            } else {
                out << "ProfileEventSingleton::instance().makeQuantityEvent( R\"(";
                out << message << ")\",";
                out << relName << "->size(),iter);";
            }
            PRINT_END_COMMENT(out);
        }

        // -- control flow statements --

        void visit_(type_identity<Sequence>, const Sequence& seq, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            for (const auto& cur : seq.getStatements()) {
                dispatch(*cur, out);
            }
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Parallel>, const Parallel& parallel, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            auto stmts = parallel.getStatements();

            // special handling cases
            if (stmts.empty()) {
                PRINT_END_COMMENT(out);
                return;
            }

            // a single statement => save the overhead
            if (stmts.size() == 1) {
                dispatch(*stmts[0], out);
                PRINT_END_COMMENT(out);
                return;
            }

            // more than one => parallel sections

            // start parallel section
            out << "SECTIONS_START;\n";

            // put each thread in another section
            for (const auto& cur : stmts) {
                out << "SECTION_START;\n";
                dispatch(*cur, out);
                out << "SECTION_END\n";
            }

            // done
            out << "SECTIONS_END;\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Loop>, const Loop& loop, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "iter = 0;\n";
            out << "for(;;) {\n";
            dispatch(loop.getBody(), out);
            out << "iter++;\n";
            out << "}\n";
            out << "iter = 0;\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Assign>, const Assign& assign, std::ostream& out) override {
            if (assign.isInit()) {
                out << "auto ";
            }
            dispatch(assign.getVariable(), out);
            out << " = ";
            dispatch(assign.getValue(), out);
            assign.getValue();
            out << ";\n";
        }

        void visit_(type_identity<Swap>, const Swap& swap, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            const std::string& deltaKnowledge =
                    synthesiser.getRelationName(synthesiser.lookup(swap.getFirstRelation()));
            const std::string& newKnowledge =
                    synthesiser.getRelationName(synthesiser.lookup(swap.getSecondRelation()));

            out << "std::swap(" << deltaKnowledge << ", " << newKnowledge << ");\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<MergeExtend>, const MergeExtend& extend, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << synthesiser.getRelationName(synthesiser.lookup(extend.getSourceRelation())) << "->"
                << "extendAndInsert("
                << "*" << synthesiser.getRelationName(synthesiser.lookup(extend.getTargetRelation()))
                << ");\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Exit>, const Exit& exit, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "if(";
            dispatch(exit.getCondition(), out);
            out << ") break;\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Call>, const Call& call, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "{\n";
            out << "FunctionTimer timer(\"" <<  call.getName() << "\");\n";
            out << " std::vector<RamDomain> args, ret;\n";
            out << synthesiser.convertStratumIdent(call.getName()) << ".run(args, ret);\n";
            // out << "debugger.endStage();\n";
            out << "}\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(
                type_identity<LogRelationTimer>, const LogRelationTimer& timer, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            // create local scope for name resolution
            out << "{\n";

            const std::string ext = fileExtension(glb.config().get("profile"));

            const auto* rel = synthesiser.lookup(timer.getRelation());
            auto relName = synthesiser.getRelationName(rel);

            out << "\tLogger logger(R\"_(" << timer.getMessage() << ")_\",iter, [&](){return " << relName
                << "->size();});\n";
            // insert statement to be measured
            dispatch(timer.getStatement(), out);

            // done
            out << "}\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<LogTimer>, const LogTimer& timer, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            // create local scope for name resolution
            out << "{\n";

            const std::string ext = fileExtension(glb.config().get("profile"));

            const std::string& message = timer.getMessage();
            bool isDredTimer = false;
            std::size_t dredSccId = 0;
            std::string dredPhase;
            std::string dredLabel;
            std::string dredBucketEnum;
            constexpr const char* dredPrefix = "@t-recursive-relation;";
            constexpr std::size_t dredPrefixLen = sizeof("@t-recursive-relation;") - 1;
            if (message.rfind(dredPrefix, 0) == 0) {
                std::size_t nameStart = dredPrefixLen;
                std::size_t nameEnd = message.find(';', nameStart);
                if (nameEnd != std::string::npos) {
                    std::string relName = message.substr(nameStart, nameEnd - nameStart);
                    if (relName.rfind("__inc_dred_", 0) == 0) {
                        std::size_t sccPos = relName.rfind("_scc");
                        if (sccPos != std::string::npos) {
                            constexpr std::size_t dredNameStart = sizeof("__inc_dred_") - 1;
                            const std::string dredName = relName.substr(dredNameStart, sccPos - dredNameStart);
                            const std::size_t labelPos = dredName.find('_');
                            if (labelPos == std::string::npos) {
                                dredPhase = dredName;
                            } else {
                                dredPhase = dredName.substr(0, labelPos);
                                dredLabel = dredName.substr(labelPos + 1);
                            }
                            std::string sccStr = relName.substr(sccPos + 4);
                            if (!sccStr.empty()) {
                                dredSccId = static_cast<std::size_t>(std::stoull(sccStr));
                                isDredTimer = true;
                            }
                        }
                    }
                }
            }

            if (isDredTimer) {
                auto dredBucketFor = [](const std::string& phase, const std::string& label) -> std::string {
                    if (phase == "delete") {
                        if (label.empty()) return "DelTotal";
                        if (label == "copy_old") return "DelCopyOld";
                        if (label == "preamble") return "DelPreamble";
                        if (label == "prefill") return "DelPrefill";
                        if (label == "prefill_update") return "DelPrefillUpdate";
                        if (label == "loop_body") return "DelLoopBody";
                        if (label == "loop_exit") return "DelLoopExit";
                        if (label == "loop_update") return "DelLoopUpdate";
                        if (label == "postamble") return "DelPostamble";
                    } else if (phase == "insert") {
                        if (label.empty()) return "InsTotal";
                        if (label == "preamble") return "InsPreamble";
                        if (label == "prefill") return "InsPrefill";
                        if (label == "prefill_update") return "InsPrefillUpdate";
                        if (label == "loop_body") return "InsLoopBody";
                        if (label == "loop_exit") return "InsLoopExit";
                        if (label == "loop_update") return "InsLoopUpdate";
                        if (label == "postamble") return "InsPostamble";
                    } else if (phase == "rederive") {
                        if (label.empty()) return "RedTotal";
                        if (label == "loop_body") return "RedLoopBody";
                        if (label == "loop_exit") return "RedLoopExit";
                        if (label == "loop_update") return "RedLoopUpdate";
                        if (label == "postamble") return "RedPostamble";
                    }
                    return {};
                };
                dredBucketEnum = dredBucketFor(dredPhase, dredLabel);
            }

            if (isDredTimer) {
                out << "if (dredProfileEnabled) {\n";
                out << "auto prev_dred_scc = DerivationManager::getDredCurrentScc();\n";
                out << "DerivationManager::setDredCurrentScc(" << dredSccId << ");\n";
                if (!dredBucketEnum.empty()) {
                    out << "std::uint64_t __dred_timer_start = DerivationManager::nowNanos();\n";
                }
                out << "\tLogger logger(R\"_(" << message << ")_\",iter);\n";
                dispatch(timer.getStatement(), out);
                if (!dredBucketEnum.empty()) {
                    out << "DerivationManager::addDredTime(DerivationManager::DredTimeBucket::"
                        << dredBucketEnum << ", DerivationManager::elapsedNanos(__dred_timer_start));\n";
                }
                out << "DerivationManager::setDredCurrentScc(prev_dred_scc);\n";
                out << "} else {\n";
                dispatch(timer.getStatement(), out);
                out << "}\n";
            } else {
                // create local timer
                out << "\tLogger logger(R\"_(" << message << ")_\",iter);\n";
                // insert statement to be measured
                dispatch(timer.getStatement(), out);
            }

            // done
            out << "}\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<DebugInfo>, const DebugInfo& dbg, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "signalHandler->setMsg(R\"_(";
            out << dbg.getMessage();
            out << ")_\");\n";

            // insert statements of the rule
            dispatch(dbg.getStatement(), out);
            PRINT_END_COMMENT(out);
        }

        // -- operations --

        void visit_(
                type_identity<NestedOperation>, const NestedOperation& nested, std::ostream& out) override {
            dispatch(nested.getOperation(), out);
            if (glb.config().has("profile") && glb.config().has("profile-frequency") &&
                    !nested.getProfileText().empty()) {
                out << "freqs[" << synthesiser.lookupFreqIdx(nested.getProfileText()) << "]++;\n";
            }
        }

        void visit_(type_identity<TupleOperation>, const TupleOperation& search, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            visit_(type_identity<NestedOperation>(), search, out);
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<ParallelScan>, const ParallelScan& pscan, std::ostream& out) override {
            const auto* rel = synthesiser.lookup(pscan.getRelation());
            const auto& relName = synthesiser.getRelationName(rel);

            assert(pscan.getTupleId() == 0 && "not outer-most loop");

            assert(rel->getArity() > 0 && "AstToRamTranslator failed/no parallel scans for nullaries");

            assert(!preambleIssued && "only first loop can be made parallel");
            preambleIssued = true;

            PRINT_BEGIN_COMMENT(out);

            out << "auto part = " << relName << "->partition();\n";
            out << "PARALLEL_START\n";
            out << preamble.str();
            out << R"cpp(
                   #if defined _OPENMP && _OPENMP < 200805
                           auto count = std::distance(part.begin(), part.end());
                           auto base = part.begin();
                           pfor(int index  = 0; index < count; index++) {
                               auto it = base + index;
                   #else
                           pfor(auto it = part.begin(); it < part.end(); it++) {
                   #endif
                   )cpp";
            out << "try{\n";
            out << "for(const auto& env0 : *it) {\n";

            visit_(type_identity<TupleOperation>(), pscan, out);

            out << "}\n";
            out << "} catch(std::exception &e) { signalHandler->error(e.what());}\n";
            out << "}\n";

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Scan>, const Scan& scan, std::ostream& out) override {
            const auto* rel = synthesiser.lookup(scan.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto id = scan.getTupleId();

            PRINT_BEGIN_COMMENT(out);

            assert(rel->getArity() > 0 && "AstToRamTranslator failed/no scans for nullaries");

            out << "for(const auto& env" << id << " : "
                << "*" << relName << ") {\n";

            visit_(type_identity<TupleOperation>(), scan, out);

            out << "}\n";

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<IfExists>, const IfExists& ifexists, std::ostream& out) override {
            const auto* rel = synthesiser.lookup(ifexists.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto identifier = ifexists.getTupleId();

            assert(rel->getArity() > 0 && "AstToRamTranslator failed/no ifexists for nullaries");

            PRINT_BEGIN_COMMENT(out);

            out << "for(const auto& env" << identifier << " : "
                << "*" << relName << ") {\n";
            out << "if( ";

            dispatch(ifexists.getCondition(), out);

            out << ") {\n";

            visit_(type_identity<TupleOperation>(), ifexists, out);

            out << "break;\n";
            out << "}\n";
            out << "}\n";

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<ParallelIfExists>, const ParallelIfExists& pifexists,
                std::ostream& out) override {
            const auto* rel = synthesiser.lookup(pifexists.getRelation());
            auto relName = synthesiser.getRelationName(rel);

            assert(pifexists.getTupleId() == 0 && "not outer-most loop");

            assert(rel->getArity() > 0 && "AstToRamTranslator failed/no parallel ifexists for nullaries");

            assert(!preambleIssued && "only first loop can be made parallel");
            preambleIssued = true;

            PRINT_BEGIN_COMMENT(out);

            out << "auto part = " << relName << "->partition();\n";
            out << "PARALLEL_START\n";
            out << preamble.str();
            out << R"cpp(
                   #if defined _OPENMP && _OPENMP < 200805
                           auto count = std::distance(part.begin(), part.end());
                           auto base = part.begin();
                           pfor(int index  = 0; index < count; index++) {
                               auto it = base + index;
                   #else
                           pfor(auto it = part.begin(); it < part.end(); it++) {
                   #endif
                   )cpp";
            out << "try{\n";
            out << "for(const auto& env0 : *it) {\n";
            out << "if( ";

            dispatch(pifexists.getCondition(), out);

            out << ") {\n";

            visit_(type_identity<TupleOperation>(), pifexists, out);

            out << "break;\n";
            out << "}\n";
            out << "}\n";
            out << "} catch(std::exception &e) { signalHandler->error(e.what());}\n";
            out << "}\n";

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<IndexScan>, const IndexScan& iscan, std::ostream& out) override {
            const auto* rel = synthesiser.lookup(iscan.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto identifier = iscan.getTupleId();
            auto keys = isa->getSearchSignature(&iscan);

            const auto& rangePatternLower = iscan.getRangePattern().first;
            const auto& rangePatternUpper = iscan.getRangePattern().second;

            assert(0 < rel->getArity() && "AstToRamTranslator failed/no index scans for nullaries");

            PRINT_BEGIN_COMMENT(out);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            auto rangeBounds = getPaddedRangeBounds(*rel, rangePatternLower, rangePatternUpper);

            out << "auto range = " << relName << "->"
                << "lowerUpperRange_" << keys << "(" << rangeBounds.first.str() << ","
                << rangeBounds.second.str() << "," << ctxName << ");\n";
            out << "for(const auto& env" << identifier << " : range) {\n";

            visit_(type_identity<TupleOperation>(), iscan, out);

            out << "}\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<EstimateJoinSize>, const EstimateJoinSize& estimateJoinSize,
                std::ostream& out) override {
            const auto* rel = synthesiser.lookup(estimateJoinSize.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto keys = isa->getSearchSignature(&estimateJoinSize);

            std::size_t indexNumber = 0;
            if (!keys.empty()) {
                indexNumber = isa->getIndexSelection(estimateJoinSize.getRelation()).getLexOrderNum(keys);
            }

            auto relationType =
                    Relation::getSynthesiserRelation(*rel, isa->getIndexSelection(rel->getName()));
            const std::string& type = relationType->getTypeName();
            auto indexName = relName + (type == "t_eqrel" ? "->ind" : "->ind_" + std::to_string(indexNumber));

            bool onlyConstants = true;
            for (auto col : estimateJoinSize.getKeyColumns()) {
                if (estimateJoinSize.getConstantsMap().count(col) == 0) {
                    onlyConstants = false;
                    break;
                }
            }

            // create a copy of the map to the real numeric constants
            std::map<std::size_t, RamDomain> keyConstants;
            for (auto [k, constant] : estimateJoinSize.getConstantsMap()) {
                RamDomain value;
                if (const auto* signedConstant = as<ram::SignedConstant>(constant)) {
                    value = ramBitCast<RamDomain>(signedConstant->getValue());
                } else if (const auto* stringConstant = as<ram::StringConstant>(constant)) {
                    value = ramBitCast<RamDomain>(
                            synthesiser.convertSymbol2Idx(stringConstant->getConstant()));
                } else if (const auto* unsignedConstant = as<ram::UnsignedConstant>(constant)) {
                    value = ramBitCast<RamDomain>(unsignedConstant->getValue());
                } else if (const auto* floatConstant = as<ram::FloatConstant>(constant)) {
                    value = ramBitCast<RamDomain>(floatConstant->getValue());
                } else {
                    fatal("Something went wrong. Should have gotten a constant!");
                }

                keyConstants[k] = value;
            }
            std::stringstream columnsStream;
            columnsStream << estimateJoinSize.getKeyColumns();
            std::string columns = columnsStream.str();

            std::stringstream constantsStream;
            constantsStream << "{";
            bool first = true;
            for (auto& [k, constant] : estimateJoinSize.getConstantsMap()) {
                if (first) {
                    first = false;
                } else {
                    constantsStream << ",";
                }
                constantsStream << k << "->" << *constant;
            }
            constantsStream << "}";
            std::string constants = stringify(constantsStream.str());

            std::string profilerText =
                    (estimateJoinSize.isRecursiveRelation() ? stringify("@recursive-estimate-join-size;" +
                                                                        estimateJoinSize.getRelation() + ";" +
                                                                        columns + ";" + constants)
                                                            : stringify("@non-recursive-estimate-join-size;" +
                                                                        estimateJoinSize.getRelation() + ";" +
                                                                        columns + ";" + constants));

            PRINT_BEGIN_COMMENT(out);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            out << "{\n";
            out << "double total = 0;\n";
            out << "double duplicates = 0;\n";

            out << "if (!" << indexName << ".empty()) {\n";
            out << "bool first = true;\n";
            out << "auto prev = *" << indexName << ".begin();\n";
            out << "for(const auto& tup : " << indexName << ") {\n";
            out << "    bool matchesConstants = true;\n";
            for (auto& [k, constant] : keyConstants) {
                if (rel->getArity() > 6) {
                    out << "matchesConstants &= (tup[0][" << k << "] == " << constant << ");\n";
                } else {
                    out << "matchesConstants &= (tup[" << k << "] == " << constant << ");\n";
                }
            }
            out << "if (!matchesConstants) {\n";
            out << "    continue;\n";
            out << "}\n";
            out << "if (first) { first = false; }\n";
            out << "else {\n";
            out << "    bool matchesPrev = true;\n";
            for (auto k : estimateJoinSize.getKeyColumns()) {
                if (rel->getArity() > 6) {
                    out << "matchesPrev &= (tup[0][" << k << "] == prev[0][" << k << "]);\n";
                } else {
                    out << "matchesPrev &= (tup[" << k << "] == prev[" << k << "]);\n";
                }
            }
            out << "if (matchesPrev) { ++duplicates; }\n";
            out << "}\n";
            out << "prev = tup; ++total;\n";
            out << "\n";
            out << "}\n";
            out << "}\n";
            out << "double joinSize = ("
                << (onlyConstants ? "total" : "total / std::max(1.0, (total - duplicates))") << ");\n";
            if (estimateJoinSize.isRecursiveRelation()) {
                out << "ProfileEventSingleton::instance().makeRecursiveCountEvent(\"" << profilerText
                    << "\", joinSize, iter);\n";
            } else {
                out << "ProfileEventSingleton::instance().makeNonRecursiveCountEvent(\"" << profilerText
                    << "\", joinSize);\n";
            }
            out << "}\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<ParallelIndexScan>, const ParallelIndexScan& piscan,
                std::ostream& out) override {
            const auto* rel = synthesiser.lookup(piscan.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto keys = isa->getSearchSignature(&piscan);

            const auto& rangePatternLower = piscan.getRangePattern().first;
            const auto& rangePatternUpper = piscan.getRangePattern().second;

            assert(piscan.getTupleId() == 0 && "not outer-most loop");

            assert(0 < rel->getArity() && "AstToRamTranslator failed/no parallel index scan for nullaries");

            assert(!preambleIssued && "only first loop can be made parallel");
            preambleIssued = true;

            PRINT_BEGIN_COMMENT(out);
            auto rangeBounds = getPaddedRangeBounds(*rel, rangePatternLower, rangePatternUpper);
            out << "auto range = " << relName
                << "->"
                // Parallel range iteration uses the relation-level range API.
                << "lowerUpperRange_" << keys << "(" << rangeBounds.first.str() << ","
                << rangeBounds.second.str() << ");\n";
            out << "auto part = range.partition();\n";
            out << "PARALLEL_START\n";
            out << preamble.str();
            out << R"cpp(
                   #if defined _OPENMP && _OPENMP < 200805
                           auto count = std::distance(part.begin(), part.end());
                           auto base = part.begin();
                           pfor(int index  = 0; index < count; index++) {
                               auto it = base + index;
                   #else
                           pfor(auto it = part.begin(); it < part.end(); it++) {
                   #endif
                   )cpp";
            out << "try{\n";
            out << "for(const auto& env0 : *it) {\n";

            visit_(type_identity<TupleOperation>(), piscan, out);

            out << "}\n";
            out << "} catch(std::exception &e) { signalHandler->error(e.what());}\n";
            out << "}\n";

            PRINT_END_COMMENT(out);
        }

        void visit_(
                type_identity<IndexIfExists>, const IndexIfExists& iifexists, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            const auto* rel = synthesiser.lookup(iifexists.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto identifier = iifexists.getTupleId();
            const auto& rangePatternLower = iifexists.getRangePattern().first;
            const auto& rangePatternUpper = iifexists.getRangePattern().second;
            auto keys = isa->getSearchSignature(&iifexists);

            // check list of keys
            assert(0 < rel->getArity() && "AstToRamTranslator failed");
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            auto rangeBounds = getPaddedRangeBounds(*rel, rangePatternLower, rangePatternUpper);

            out << "auto range = " << relName << "->"
                << "lowerUpperRange_" << keys << "(" << rangeBounds.first.str() << ","
                << rangeBounds.second.str() << "," << ctxName << ");\n";
            out << "for(const auto& env" << identifier << " : range) {\n";
            out << "if( ";

            dispatch(iifexists.getCondition(), out);

            out << ") {\n";

            visit_(type_identity<TupleOperation>(), iifexists, out);

            out << "break;\n";
            out << "}\n";
            out << "}\n";

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<ParallelIndexIfExists>, const ParallelIndexIfExists& piifexists,
                std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            const auto* rel = synthesiser.lookup(piifexists.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            const auto& rangePatternLower = piifexists.getRangePattern().first;
            const auto& rangePatternUpper = piifexists.getRangePattern().second;
            auto keys = isa->getSearchSignature(&piifexists);

            assert(piifexists.getTupleId() == 0 && "not outer-most loop");
            assert(0 < rel->getArity() && "AstToRamTranslator failed");
            assert(!preambleIssued && "only first loop can be made parallel");
            preambleIssued = true;

            PRINT_BEGIN_COMMENT(out);
            auto rangeBounds = getPaddedRangeBounds(*rel, rangePatternLower, rangePatternUpper);
            out << "auto range = " << relName
                << "->"
                // Parallel range iteration uses the relation-level range API.
                << "lowerUpperRange_" << keys << "(" << rangeBounds.first.str() << ","
                << rangeBounds.second.str() << ");\n";
            out << "auto part = range.partition();\n";
            out << "PARALLEL_START\n";
            out << preamble.str();
            out << R"cpp(
                   #if defined _OPENMP && _OPENMP < 200805
                           auto count = std::distance(part.begin(), part.end());
                           auto base = part.begin();
                           pfor(int index  = 0; index < count; index++) {
                               auto it = base + index;
                   #else
                           pfor(auto it = part.begin(); it < part.end(); it++) {
                   #endif
                   )cpp";
            out << "try{";
            out << "for(const auto& env0 : *it) {\n";
            out << "if( ";

            dispatch(piifexists.getCondition(), out);

            out << ") {\n";

            visit_(type_identity<TupleOperation>(), piifexists, out);

            out << "break;\n";
            out << "}\n";
            out << "}\n";
            out << "} catch(std::exception &e) { signalHandler->error(e.what());}\n";
            out << "}\n";

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<UnpackRecord>, const UnpackRecord& unpack, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            auto arity = unpack.getArity();

            synthesiser.arities.emplace(arity);

            // look up reference
            out << "RamDomain const ref = ";
            dispatch(unpack.getExpression(), out);
            out << ";\n";

            // Handle nil case.
            out << "if (ref == 0) continue;\n";

            // Unpack tuple
            out << "const RamDomain *"
                << "env" << unpack.getTupleId() << " = "
                << "recordTable.unpack(ref," << arity << ");"
                << "\n";

            out << "{\n";

            // continue with condition checks and nested body
            visit_(type_identity<TupleOperation>(), unpack, out);

            out << "}\n";
            PRINT_END_COMMENT(out);
        }

        std::string initValue(const Aggregator& aggregator) {
            if (const auto* ia = as<ram::IntrinsicAggregator>(aggregator)) {
                switch (ia->getFunction()) {
                    case AggregateOp::MIN: return "MAX_RAM_SIGNED";
                    case AggregateOp::FMIN: return "MAX_RAM_FLOAT";
                    case AggregateOp::UMIN: return "MAX_RAM_UNSIGNED";
                    case AggregateOp::MAX: return "MIN_RAM_SIGNED";
                    case AggregateOp::FMAX: return "MIN_RAM_FLOAT";
                    case AggregateOp::UMAX: return "MIN_RAM_UNSIGNED";
                    case AggregateOp::COUNT:
                    case AggregateOp::MEAN:
                    case AggregateOp::FSUM:
                    case AggregateOp::USUM:
                    case AggregateOp::SUM: return "0";
                }
            } else if (const auto* uda = as<ram::UserDefinedAggregator>(aggregator)) {
                assert(uda);
                std::stringstream ss;
                dispatch(*uda->getInitValue(), ss);
                return ss.str();
            }
            fatal("Unhandled aggregate operation");
        }

        void updateRes(std::ostream& out, const AbstractAggregate& aggregate) {
            const auto& aggregator = aggregate.getAggregator();
            if (const auto* ia = as<ram::IntrinsicAggregator>(aggregator)) {
                AggregateOp aggregateFun = ia->getFunction();
                std::string type = getType(aggregator);
                switch (aggregateFun) {
                    case AggregateOp::FMIN:
                    case AggregateOp::UMIN:
                    case AggregateOp::MIN:
                        out << "res0 = std::min(res0,ramBitCast<" << type << ">(";
                        dispatch(aggregate.getExpression(), out);
                        out << "));\n";
                        break;
                    case AggregateOp::FMAX:
                    case AggregateOp::UMAX:
                    case AggregateOp::MAX:
                        out << "res0 = std::max(res0,ramBitCast<" << type << ">(";
                        dispatch(aggregate.getExpression(), out);
                        out << "));\n";
                        break;
                    case AggregateOp::COUNT: out << "++res0\n;"; break;
                    case AggregateOp::FSUM:
                    case AggregateOp::USUM:
                    case AggregateOp::SUM:
                        out << "res0 += "
                            << "ramBitCast<" << type << ">(";
                        dispatch(aggregate.getExpression(), out);
                        out << ");\n";
                        break;

                    case AggregateOp::MEAN:
                        out << "res0 += "
                            << "ramBitCast<RamFloat>(";
                        dispatch(aggregate.getExpression(), out);
                        out << ");\n";
                        out << "++res1;\n";
                        break;
                }
            } else if (const auto* uda = as<ram::UserDefinedAggregator>(aggregator)) {
                out << "res0 = " << uda->getName() << "(";
                if (uda->isStateful()) {
                    out << "&symTable, &recordTable, ";
                }
                out << "res0, ";
                dispatch(aggregate.getExpression(), out);
                out << ");\n";
            }
        }

        bool shouldRunNested(const Aggregator& aggregator) {
            if (const auto* ia = as<ram::IntrinsicAggregator>(aggregator)) {
                switch (ia->getFunction()) {
                    case AggregateOp::COUNT:
                    case AggregateOp::FSUM:
                    case AggregateOp::USUM:
                    case AggregateOp::SUM: return true;
                    default: return false;
                }
            } else if (isA<ram::UserDefinedAggregator>(aggregator)) {
                return true;
            }
            fatal("Unhandled aggregate operation");
        }

        std::string getType(const Aggregator& aggregator) {
            auto str = [&](souffle::TypeAttribute ta) {
                switch (ta) {
                    case TypeAttribute::Signed: return "RamSigned";
                    case TypeAttribute::Unsigned: return "RamUnsigned";
                    case TypeAttribute::Float: return "RamFloat";
                    case TypeAttribute::Symbol:
                    case TypeAttribute::ADT:
                    case TypeAttribute::Record: return "RamDomain";
                    default: return "RamDomain";
                }
            };
            if (const auto* ia = as<ram::IntrinsicAggregator>(aggregator)) {
                return str(getTypeAttributeAggregate(ia->getFunction()));
            } else if (const auto* uda = as<ram::UserDefinedAggregator>(aggregator)) {
                return str(uda->getReturnType());
            }
            fatal("Unhandled aggregator");
        }

        std::tuple<std::string, std::string, int> reductionOperation(const Aggregator& aggregator) {
            if (const auto* ia = as<ram::IntrinsicAggregator>(aggregator)) {
                switch (ia->getFunction()) {
                    case AggregateOp::MIN:
                    case AggregateOp::FMIN:
                    case AggregateOp::UMIN: return std::make_tuple("min", "", 200805);
                    case AggregateOp::MAX:
                    case AggregateOp::FMAX:
                    case AggregateOp::UMAX: return std::make_tuple("max", "", 200805);
                    case AggregateOp::MEAN:
                    case AggregateOp::FSUM:
                    case AggregateOp::USUM:
                    case AggregateOp::COUNT:
                    case AggregateOp::SUM: return std::make_tuple("+", "", 0);
                    default: fatal("Unhandled aggregate operation");
                }
            } else if (const auto* uda = as<ram::UserDefinedAggregator>(aggregator)) {
                std::stringstream def;
                std::string name = uda->getName();
                def << "#pragma omp declare reduction("
                    << "reduction_" << name << " : " << getType(aggregator) << " : \\\n";
                // OpenMP reductions assume stateless user-defined aggregators.
                def << "omp_out = " << name << "(omp_out, omp_in) )\\\n";
                def << "initializer (omp_priv=(omp_orig))\n";
                return std::make_tuple("reduction_" + name, def.str(), 0);
            }
            fatal("Unhandled aggregator");
        }

        void ifIntrinsic(const ram::Aggregator& aggregator, AggregateOp op, std::function<void()> fn) {
            if (const auto* ia = as<ram::IntrinsicAggregator>(aggregator)) {
                if (ia->getFunction() == op) {
                    fn();
                };
            }
        }

        void visit_(type_identity<ParallelIndexAggregate>, const ParallelIndexAggregate& aggregate,
                std::ostream& out) override {
            assert(aggregate.getTupleId() == 0 && "not outer-most loop");
            assert(!preambleIssued && "only first loop can be made parallel");
            preambleIssued = true;
            PRINT_BEGIN_COMMENT(out);
            // get some properties
            const auto* rel = synthesiser.lookup(aggregate.getRelation());
            auto arity = rel->getArity();
            auto relName = synthesiser.getRelationName(rel);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            auto identifier = aggregate.getTupleId();

            // aggregate tuple storing the result of aggregate
            std::string tuple_type = "Tuple<RamDomain," + toString(arity) + ">";

            // declare environment variable
            out << "Tuple<RamDomain,1> env" << identifier << ";\n";

            // get range to aggregate
            auto keys = isa->getSearchSignature(&aggregate);

            const ram::Aggregator& aggregator = aggregate.getAggregator();

            bool isCount = false;
            ifIntrinsic(aggregator, AggregateOp::COUNT, [&]() { isCount = true; });

            // special case: counting number elements over an unrestricted predicate
            if (isCount && keys.empty() && isTrue(&aggregate.getCondition())) {
                // shortcut: use relation size
                out << "env" << identifier << "[0] = " << relName << "->"
                    << "size();\n";
                out << "{\n";  // to match PARALLEL_END closing bracket
                out << preamble.str();
                visit_(type_identity<TupleOperation>(), aggregate, out);
                PRINT_END_COMMENT(out);
                return;
            }

            // init result and reduction operation
            std::string init = initValue(aggregator);
            out << "bool shouldRunNested = " << (shouldRunNested(aggregator) ? "true" : "false") << ";\n";

            // Set reduction operation
            std::string op;
            std::string op_def;
            int omp_min_ver;
            std::tie(op, op_def, omp_min_ver) = reductionOperation(aggregator);

            // res0 stores the aggregate result
            std::string sharedVariable = "res0";

            std::string type = getType(aggregator);

            out << type << " res0 = " << init << ";\n";
            ifIntrinsic(aggregator, AggregateOp::MEAN, [&]() {
                out << "RamUnsigned res1 = 0;\n";
                sharedVariable += ", res1";
            });

            out << preamble.str();
            out << "PARALLEL_START\n";
            // check whether there is an index to use
            if (keys.empty()) {
                // OMP reduction is not available on all versions of OpenMP
                out << "#if defined _OPENMP && _OPENMP >= " << omp_min_ver << "\n";
                out << op_def << "\n";
                out << "#pragma omp for reduction(" << op << ":" << sharedVariable << ")\n";
                out << "#endif\n";

                out << "for(const auto& env" << identifier << " : "
                    << "*" << relName << ") {\n";
            } else {
                const auto& rangePatternLower = aggregate.getRangePattern().first;
                const auto& rangePatternUpper = aggregate.getRangePattern().second;

                auto rangeBounds = getPaddedRangeBounds(*rel, rangePatternLower, rangePatternUpper);
                out << "auto range = " << relName << "->"
                    << "lowerUpperRange_" << keys << "(" << rangeBounds.first.str() << ","
                    << rangeBounds.second.str() << "," << ctxName << ");\n";

                out << "auto part = range.partition();\n";

                // old OpenMP versions cannot loop on iterators
                out << R"cpp(
                   #if defined _OPENMP && _OPENMP < 200805
                           auto count = std::distance(part.begin(), part.end());
                           auto base = part.begin();
                   #endif
                   )cpp";

                // OMP reduction is not available on all versions of OpenMP
                out << "#if defined _OPENMP && _OPENMP >= " << omp_min_ver << "\n";
                out << op_def << "\n";
                out << "#pragma omp for reduction(" << op << ":" << sharedVariable << ")\n";
                out << "#endif\n";

                // iterate over each part
                out << R"cpp(
                   #if defined _OPENMP && _OPENMP < 200805
                           for(int index  = 0; index < count; index++) {
                               auto it = base + index;
                   #else
                           for(auto it = part.begin(); it < part.end(); ++it) {
                   #endif
                   )cpp";
                // iterate over tuples in each part
                out << "for (const auto& env" << identifier << ": *it) {\n";
            }

            // produce condition inside the loop if necessary
            out << "if( ";
            dispatch(aggregate.getCondition(), out);
            out << ") {\n";

            out << "shouldRunNested = true;\n";

            // pick function
            updateRes(out, aggregate);

            // end if statement
            out << "}\n";

            // end aggregator loop
            out << "}\n";

            // if keys weren't empty then there'll be another loop to close off
            if (!keys.empty()) {
                out << "}\n";
            }

            // start single-threaded section
            out << "#pragma omp single\n{\n";

            ifIntrinsic(aggregator, AggregateOp::MEAN, [&]() {
                out << "if (res1 != 0) {\n";
                out << "res0 = res0 / res1;\n";
                out << "}\n";
            });

            // write result into environment tuple
            out << "env" << identifier << "[0] = ramBitCast(res0);\n";

            // check whether there exists a min/max first before next loop
            out << "if (shouldRunNested) {\n";
            visit_(type_identity<TupleOperation>(), aggregate, out);
            out << "}\n";
            // end single-threaded section
            out << "}\n";
            PRINT_END_COMMENT(out);
        }

        bool isGuaranteedToBeMinimum(const IndexAggregate& aggregate) {
            auto identifier = aggregate.getTupleId();
            auto keys = isa->getSearchSignature(&aggregate);
            RelationRepresentation repr = synthesiser.lookup(aggregate.getRelation())->getRepresentation();

            const auto* tupleElem = as<TupleElement>(aggregate.getExpression());
            return tupleElem && tupleElem->getTupleId() == identifier &&
                   keys[tupleElem->getElement()] != ram::analysis::AttributeConstraint::None &&
                   (repr == RelationRepresentation::BTREE || repr == RelationRepresentation::DEFAULT);
        }

        void visit_(
                type_identity<IndexAggregate>, const IndexAggregate& aggregate, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            // get some properties
            const auto* rel = synthesiser.lookup(aggregate.getRelation());
            auto arity = rel->getArity();
            auto relName = synthesiser.getRelationName(rel);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            auto identifier = aggregate.getTupleId();

            // aggregate tuple storing the result of aggregate
            std::string tuple_type = "Tuple<RamDomain," + toString(arity) + ">";

            // declare environment variable
            out << "Tuple<RamDomain,1> env" << identifier << ";\n";

            // get range to aggregate
            auto keys = isa->getSearchSignature(&aggregate);

            const ram::Aggregator& aggregator = aggregate.getAggregator();

            bool isCount = false;
            ifIntrinsic(aggregator, AggregateOp::COUNT, [&]() { isCount = true; });

            // special case: counting number elements over an unrestricted predicate
            if (isCount && keys.empty() && isTrue(&aggregate.getCondition())) {
                // shortcut: use relation size
                out << "env" << identifier << "[0] = " << relName << "->"
                    << "size();\n";
                visit_(type_identity<TupleOperation>(), aggregate, out);
                PRINT_END_COMMENT(out);
                return;
            }

            // init result
            std::string init = initValue(aggregator);
            out << "bool shouldRunNested = " << (shouldRunNested(aggregator) ? "true" : "false") << ";\n";

            std::string type = getType(aggregator);

            out << type << " res0 = " << init << ";\n";

            ifIntrinsic(aggregator, AggregateOp::MEAN, [&]() { out << "RamUnsigned res1 = 0;\n"; });

            // check whether there is an index to use
            if (keys.empty()) {
                out << "for(const auto& env" << identifier << " : "
                    << "*" << relName << ") {\n";
            } else {
                const auto& rangePatternLower = aggregate.getRangePattern().first;
                const auto& rangePatternUpper = aggregate.getRangePattern().second;

                auto rangeBounds = getPaddedRangeBounds(*rel, rangePatternLower, rangePatternUpper);

                out << "auto range = " << relName << "->"
                    << "lowerUpperRange_" << keys << "(" << rangeBounds.first.str() << ","
                    << rangeBounds.second.str() << "," << ctxName << ");\n";

                // aggregate result
                out << "for(const auto& env" << identifier << " : range) {\n";
            }

            // produce condition inside the loop
            out << "if( ";
            dispatch(aggregate.getCondition(), out);
            out << ") {\n";

            out << "shouldRunNested = true;\n";

            // pick function
            updateRes(out, aggregate);
            auto printBreak = [&]() {
                if (isGuaranteedToBeMinimum(aggregate)) {
                    out << "break;\n";
                }
            };
            ifIntrinsic(aggregator, AggregateOp::FMIN, printBreak);
            ifIntrinsic(aggregator, AggregateOp::UMIN, printBreak);
            ifIntrinsic(aggregator, AggregateOp::MIN, printBreak);

            out << "}\n";

            // end aggregator loop
            out << "}\n";

            ifIntrinsic(aggregator, AggregateOp::MEAN, [&]() {
                out << "if (res1 != 0) {\n";
                out << "res0 = res0 / res1;\n";
                out << "}\n";
            });

            // write result into environment tuple
            out << "env" << identifier << "[0] = ramBitCast(res0);\n";

            // check whether there exists a min/max first before next loop
            out << "if (shouldRunNested) {\n";
            visit_(type_identity<TupleOperation>(), aggregate, out);
            out << "}\n";

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<ParallelAggregate>, const ParallelAggregate& aggregate,
                std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            // get some properties
            const auto* rel = synthesiser.lookup(aggregate.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            auto identifier = aggregate.getTupleId();

            assert(aggregate.getTupleId() == 0 && "not outer-most loop");
            assert(!preambleIssued && "only first loop can be made parallel");
            preambleIssued = true;

            // declare environment variable
            out << "Tuple<RamDomain,1> env" << identifier << ";\n";

            const ram::Aggregator& aggregator = aggregate.getAggregator();

            bool isCount = false;
            ifIntrinsic(aggregator, AggregateOp::COUNT, [&]() { isCount = true; });

            // special case: counting number elements over an unrestricted predicate
            if (isCount && isTrue(&aggregate.getCondition())) {
                // shortcut: use relation size
                out << "env" << identifier << "[0] = " << relName << "->"
                    << "size();\n";
                out << "PARALLEL_START\n";
                out << preamble.str();
                visit_(type_identity<TupleOperation>(), aggregate, out);
                PRINT_END_COMMENT(out);
                return;
            }

            // init result
            std::string init = initValue(aggregator);
            out << "bool shouldRunNested = " << (shouldRunNested(aggregator) ? "true" : "false") << ";\n";

            // Set reduction operation
            std::string op;
            std::string op_def;
            int omp_min_ver;
            std::tie(op, op_def, omp_min_ver) = reductionOperation(aggregator);

            std::string type = getType(aggregator);

            out << type << " res0 = " << init << ";\n";

            std::string sharedVariable = "res0";
            ifIntrinsic(aggregator, AggregateOp::MEAN, [&]() {
                out << "RamUnsigned res1 = " << init << ";\n";
                sharedVariable += ", res1";
            });

            // create a partitioning of the relation to iterate over simeltaneously
            out << "auto part = " << relName << "->partition();\n";
            out << "PARALLEL_START\n";
            out << preamble.str();

            // old OpenMP versions cannot loop on iterators
            out << R"cpp(
                   #if defined _OPENMP && _OPENMP < 200805
                           auto count = std::distance(part.begin(), part.end());
                           auto base = part.begin();
                   #endif
                   )cpp";

            // pragma statement
            out << "#if defined _OPENMP && _OPENMP >= " << omp_min_ver << "\n";
            out << op_def << "\n";
            out << "#pragma omp for reduction(" << op << ":" << sharedVariable << ")\n";
            out << "#endif\n";

            // iterate over each part
            out << R"cpp(
                   #if defined _OPENMP && _OPENMP < 200805
                           for(int index  = 0; index < count; index++) {
                               auto it = base + index;
                   #else
                           for(auto it = part.begin(); it < part.end(); ++it) {
                   #endif
                   )cpp";
            // iterate over tuples in each part
            out << "for (const auto& env" << identifier << ": *it) {\n";

            // produce condition inside the loop
            out << "if( ";
            dispatch(aggregate.getCondition(), out);
            out << ") {\n";

            out << "shouldRunNested = true;\n";
            // pick function
            updateRes(out, aggregate);

            out << "}\n";

            // end aggregator loop
            out << "}\n";
            // end partition loop
            out << "}\n";

            // the rest shouldn't be run in parallel
            out << "#pragma omp single\n{\n";

            ifIntrinsic(aggregator, AggregateOp::MEAN, [&]() {
                out << "if (res1 != 0) {\n";
                out << "res0 = res0 / res1;\n";
                out << "}\n";
            });

            // write result into environment tuple
            out << "env" << identifier << "[0] = ramBitCast(res0);\n";

            // check whether there exists a min/max first before next loop
            out << "if (shouldRunNested) {\n";
            visit_(type_identity<TupleOperation>(), aggregate, out);
            out << "}\n";
            out << "}\n";  // to close off pragma omp single section
            PRINT_END_COMMENT(out);
        }
        void visit_(type_identity<Aggregate>, const Aggregate& aggregate, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            // get some properties
            const auto* rel = synthesiser.lookup(aggregate.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            auto identifier = aggregate.getTupleId();

            // declare environment variable
            out << "Tuple<RamDomain,1> env" << identifier << ";\n";

            const ram::Aggregator& aggregator = aggregate.getAggregator();

            bool isCount = false;
            ifIntrinsic(aggregator, AggregateOp::COUNT, [&]() { isCount = true; });

            // special case: counting number elements over an unrestricted predicate
            if (isCount && isTrue(&aggregate.getCondition())) {
                // shortcut: use relation size
                out << "env" << identifier << "[0] = " << relName << "->"
                    << "size();\n";
                visit_(type_identity<TupleOperation>(), aggregate, out);
                PRINT_END_COMMENT(out);
                return;
            }

            // init result
            std::string init = initValue(aggregator);
            out << "bool shouldRunNested = " << (shouldRunNested(aggregator) ? "true" : "false") << ";\n";

            std::string type = getType(aggregator);

            out << type << " res0 = " << init << ";\n";

            ifIntrinsic(aggregator, AggregateOp::MEAN, [&]() { out << "RamUnsigned res1 = 0;\n"; });

            // check whether there is an index to use
            out << "for(const auto& env" << identifier << " : "
                << "*" << relName << ") {\n";

            // produce condition inside the loop
            out << "if( ";
            dispatch(aggregate.getCondition(), out);
            out << ") {\n";

            out << "shouldRunNested = true;\n";
            // pick function
            updateRes(out, aggregate);

            out << "}\n";

            // end aggregator loop
            out << "}\n";

            ifIntrinsic(aggregator, AggregateOp::MEAN, [&]() { out << "res0 = res0 / res1;\n"; });

            // write result into environment tuple
            out << "env" << identifier << "[0] = ramBitCast(res0);\n";

            // check whether there exists a min/max first before next loop
            out << "if (shouldRunNested) {\n";
            visit_(type_identity<TupleOperation>(), aggregate, out);
            out << "}\n";

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Filter>, const Filter& filter, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "if( ";
            dispatch(filter.getCondition(), out);
            out << ") {\n";
            visit_(type_identity<NestedOperation>(), filter, out);
            out << "}\n";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Break>, const Break& breakOp, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "if( ";
            dispatch(breakOp.getCondition(), out);
            out << ") break;\n";
            visit_(type_identity<NestedOperation>(), breakOp, out);
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<GuardedInsert>, const GuardedInsert& guardedInsert,
                std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            const auto* rel = synthesiser.lookup(guardedInsert.getRelation());
            auto arity = rel->getArity();
            auto relName = synthesiser.getRelationName(rel);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";

            auto condition = guardedInsert.getCondition();
            // guarded conditions
            out << "if( ";
            dispatch(*condition, out);
            out << ") {\n";

            // create inserted tuple
            out << "Tuple<RamDomain," << arity << "> tuple{{" << join(guardedInsert.getValues(), ",", rec)
                << "}};\n";

            // insert tuple
            out << relName << "->"
                << "insert(tuple," << ctxName << ");\n";

            // end of conseq body.
            out << "}\n";

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Insert>, const Insert& insert, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            const auto* rel = synthesiser.lookup(insert.getRelation());
            auto arity = rel->getArity();
            auto relName = synthesiser.getRelationName(rel);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            auto tempRelName = rel->getName();
            // create (typed) inserted tuple
            out << "Tuple<RamDomain," << arity << "> tuple{{" << join(insert.getValues(), ",", rec)
                << "}};\n";

            // insert tuple and record derivation
            // case 1: insert to delta, (new) to old
            // Delta/new insert handling.
            if (insert.getClauseStr() == "UNKNOWN CLAUSE") {
                // out << relName << "->"
                //     << "insert(tuple," << ctxName << ");\n";
                if (tempRelName.size() >= 4 && tempRelName.substr(0, 4) == "@new"
                    && !(tempRelName.size() >= 10 && tempRelName.substr(0, 10) != "@new_derv_")){
                    tempRelName.erase(tempRelName.begin(), tempRelName.begin() + 5);  // there is an extra '_'
                    auto origRelName = synthesiser.getRelationName(synthesiser.lookup(tempRelName));
                    // out << "if (origRelName->contains("
                    out << "if (!" << origRelName << "->contains(tuple)) {\n";
                    out << relName << "->"
                        << "insert(tuple," << ctxName << ");\n";
                    out << "}\n";
                } else {
                    out << relName << "->"
                        << "insert(tuple," << ctxName << ");\n";
                }
            } else {
                // Non-UNKNOWN clauses follow the same guarded new-relation insertion policy.
                if (tempRelName.size() >= 4 && tempRelName.substr(0, 4) == "@new"
    && !(tempRelName.size() >= 9 && tempRelName.substr(0, 9) == "@new_derv")) {
                    tempRelName.erase(tempRelName.begin(), tempRelName.begin() + 5);  // there is an extra '_'
                    auto origRelName = synthesiser.getRelationName(synthesiser.lookup(tempRelName));
                    // out << "if (origRelName->contains("
                    out << "if (!" << origRelName << "->contains(tuple)) {\n";
                    out << relName << "->"
                        << "insert(tuple," << ctxName << ");\n";
                    out << "}\n";
                } else {
                    out << relName << "->"
                        << "insert(tuple," << ctxName << ");\n";
                }


                // retrieve the tuple first
                // out << "auto untypedTuple = UntypedTuple::fromTypedTuple(\"" << tempRelName << "\",tuple);\n";
                // out << "auto*& ruleSet = DerivationManager::untypedTuple2RuleApplications[untypedTuple];\n";
                // out << "if (ruleSet == nullptr) {\n";
                // out << relName << "->"
                    // << "insert(tuple," << ctxName << ");\n";  // only insert tuple to rel when it wasn't recorded
                // out << "ruleSet = new std::set<RuleApplication>();\n";
                // out << "}\n";
                // record derivation info about realTuple
                // out << "std::map<std::string, souffle::RamDomain> varValues{};\n";
                // for (const auto& [var, expr]: insert.varExprMap) {
                //     out << "varValues.insert({\"" << var  << "\", "; rec(out, expr.get()); out << "});\n";
                // }
                // out << "RuleApplication ruleApplication{" << insert.getClauseID() << ", varValues};\n";
                // out << "ruleSet->insert(ruleApplication);\n";
            }

            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<SequentialOperation>, const SequentialOperation& sequentialOperation, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            for (const auto& cur : sequentialOperation.getOperations()) {
                dispatch(*cur, out);
            }
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<RecordDerivation>, const RecordDerivation& recordDerivation, std::ostream& out) override {
            auto relName = getBaseRelationName(recordDerivation.getRelation());
            bool isRecursive = recordDerivation.isRecursive;
            out << "if (!detOptEnabled || !isDetRelation(\"" << relName << "\")) {\n";
            const char* dredRecordBucket =
                    recordDerivation.isInsert() ? "InsRecord" : "DelRecord";
            out << "std::uint64_t __dred_record_start = 0;\n";
            out << "if (dredProfileEnabled) { __dred_record_start = DerivationManager::nowNanos(); }\n";
            out << "auto untypedTuple = UntypedTuple::fromTypedTuple(\"" << relName << "\",tuple);\n";
            if (recordDerivation.isComplete()) {
                out << "auto*& ruleSet = DerivationManager::untypedTuple2RuleApplications[untypedTuple];\n";
            } else {
                if (recordDerivation.isInsert()) {
                    if (!recordDerivation.isRederive()) {
                        out << "auto*& ruleSet = DerivationManager::untypedTuple2DeltaInsertRuleApplications[untypedTuple];\n";
                    } else {
                        out << "auto*& ruleSet = DerivationManager::untypedTuple2DeltaDeleteRuleApplications[untypedTuple];\n";
                    }
                    // if (isRecursive) {
                    out << "auto*& ruleSet2 = DerivationManager::untypedTuple2DeltaDeltaInsertRuleApplications[untypedTuple];\n";
                    // }
                } else {
                    out << "auto*& ruleSet = DerivationManager::untypedTuple2DeltaDeleteRuleApplications[untypedTuple];\n";
                    // if (isRecursive) {
                        out << "auto*& ruleSet2 = DerivationManager::untypedTuple2DeltaDeltaDeleteRuleApplications[untypedTuple];\n";
                    // }
                }
            }
            out << "if (ruleSet == nullptr) {\n";
            out << "ruleSet = new std::unordered_set<RuleApplication>();\n";
            out << "}\n";
            if (!recordDerivation.isComplete()) {
                out << "if (ruleSet2 == nullptr) {\n";
                out << "ruleSet2 = new std::unordered_set<RuleApplication>();\n";
                out << "}\n";
            }
            if (recordDerivation.isInsert()) {
                out << "std::vector<souffle::RamDomain> varValues{};\n";
                for (const auto& expr: recordDerivation.varExprs) {
                    out << "varValues.emplace_back("; rec(out, expr.get()); out << ");\n";
                }
                out << "RuleApplication ruleApplication{" << recordDerivation.getClauseID() << ", varValues};\n";
                if (!recordDerivation.isRederive()) {
                    out << "ruleSet->insert(ruleApplication);\n";
                    out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                    out << "DerivationManager::dredStats.ins_ruleapp_recorded++;\n";
                    out << "}\n";
                } else {
                    // rederiving overdeleted derivations
                    // out << "std::cout << \"rederive: \" << untypedTuple.toString() << \" \" << ruleApplication.toString() << std::endl;\n";
                    out << "ruleSet->erase(ruleApplication);\n";
                    out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                    out << "DerivationManager::dredStats.ins_ruleapp_rederive_erases++;\n";
                    out << "DerivationManager::bumpDredSccRederiveRuleappErases();\n";
                    out << "}\n";
                }
                if (!recordDerivation.isComplete()) {
                    out << "ruleSet2->insert(ruleApplication);\n";
                    out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                    out << "DerivationManager::dredStats.ins_ruleapp_delta_delta++;\n";
                    out << "}\n";
                }
            } else {
                // is delete
                if (recordDerivation.isComplete()) {
                    out << "std::vector<souffle::RamDomain> varValues{};\n";
                    for (const auto& expr: recordDerivation.varExprs) {
                        out << "varValues.emplace_back("; rec(out, expr.get()); out << ");\n";
                    }
                    out << "RuleApplication ruleApplication{" << recordDerivation.getClauseID() << ", varValues};\n";
                    out << "ruleSet->insert(ruleApplication);\n";
                    out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                    out << "DerivationManager::dredStats.del_ruleapp_recorded++;\n";
                    out << "}\n";
                } else {
                    out << "std::vector<souffle::RamDomain> varValues{};\n";
                    for (const auto& expr: recordDerivation.varExprs) {
                        out << "varValues.emplace_back("; rec(out, expr.get()); out << ");\n";
                    }
                    out << "RuleApplication ruleApplication{" << recordDerivation.getClauseID() << ", varValues};\n";
                    out << "ruleSet->insert(ruleApplication);\n";
                    out << "ruleSet2->insert(ruleApplication);\n";
                    out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                    out << "DerivationManager::dredStats.del_ruleapp_recorded++;\n";
                    out << "DerivationManager::dredStats.del_ruleapp_delta_delta++;\n";
                    out << "}\n";

                    // Keep recursive-stratum detection dynamic because rule ids are generated.
                    out << "if (ruleManager.isInRecursiveStratum(ruleApplication.ruleId)) {\n";
                    out << "auto*& ruleSetComplete = DerivationManager::untypedTuple2RuleApplications[untypedTuple];\n";
                    out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                    out << "DerivationManager::dredStats.del_complete_scan_calls++;\n";
                    out << "DerivationManager::bumpDredSccCompleteScanCalls();\n";
                    out << "if (ruleSetComplete != nullptr) {\n";
                    out << "DerivationManager::dredStats.del_complete_scan_elems += ruleSetComplete->size();\n";
                    out << "DerivationManager::bumpDredSccCompleteScanElems(ruleSetComplete->size());\n";
                    out << "}\n";
                    out << "}\n";
                    // over-deletion...
                    out << "std::uint64_t __dred_overdelete_start = 0;\n";
                    out << "if (dredProfileEnabled) { __dred_overdelete_start = DerivationManager::nowNanos(); }\n";
                    out << "if (ruleSetComplete != nullptr) {\n";
                    out << "for (const auto& ruleApp: *ruleSetComplete) {" << std::endl;
                    out << "if(ruleManager.isRecursive(ruleApp.ruleId)) {\n";
                    out << "ruleSet->insert(ruleApp);\n";
                    out << "ruleSet2->insert(ruleApp);\n";
                    out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                    out << "DerivationManager::dredStats.del_ruleapp_overdelete++;\n";
                    out << "DerivationManager::bumpDredSccOverdelete();\n";
                    out << "}\n";
                    out << "}\n";
                    out << "}" << std::endl;
                    out << "}\n";
                    out << "if (dredProfileEnabled) {\n";
                    out << "DerivationManager::addDredTime(DerivationManager::DredTimeBucket::DelOverdelete,\n";
                    out << "        DerivationManager::elapsedNanos(__dred_overdelete_start));\n";
                    out << "}\n";
                    out << "}\n";
                }
            }
            out << "if (dredProfileEnabled) {\n";
            out << "DerivationManager::addDredTime(DerivationManager::DredTimeBucket::"
                << dredRecordBucket << ", DerivationManager::elapsedNanos(__dred_record_start));\n";
            out << "}\n";
            out << "}\n";
        }

        void visit_(type_identity<EmptyStatement>, const EmptyStatement& emptyStmt, std::ostream& out) override {

        }

        void visit_(type_identity<DeltaUnion>, const DeltaUnion& deltaUnion, std::ostream& out) override {
            out << "{\n";
            /**
             * Rnew, Rrealinsert, Rrealdelete <= Rold, Rderinsert, Rderdelete
             */
            /* for non-recursion case
            forall t in Rderinsert
                get deltader_insert for t
                if t not in Rold
                    Rrealinsert.insert(t)
                    der for t = deltader_insert
                else
                    der for t = der + deltader_insert
            forall t in Rderdelete
                get delteder_delete for t
                get der for t
                der for t = der - deltader_delete
                if der is empty:
                    Rdrealdelete.insert(t)
            Rnew = Rold + Rrealinsert - Rrealdelete
            */
            /* for recursive case, we separate deletion and insertion, so there is delta union for del / ins
             * note that here Rold is just Rnew, since we do not make use of Rold in recursive case
             * Rnew = Rnew - Rrealdelete
             * Rnew = Rnew + Rrealinsert
             **/
            bool insertOnly = false, deleteOnly = false, both = false;
            if (deltaUnion.getDeltaDervDeleteRel() != "" && deltaUnion.getDeltaTupleDeleteRel() != ""
                && !(deltaUnion.getDeltaDervInsertRel() != "" && deltaUnion.getDeltaTupleInsertRel() != "")) {
                deleteOnly = true;
            } else if (deltaUnion.getDeltaDervInsertRel() != "" && deltaUnion.getDeltaTupleInsertRel() != ""
                && !(deltaUnion.getDeltaDervDeleteRel() != "" && deltaUnion.getDeltaTupleDeleteRel() != "")) {
                insertOnly = true;
            } else {
                both = true;
            }
            assert ((deleteOnly && insertOnly) == false);
            out << "const bool detRel = detOptEnabled && isDetRelation(\""
                << deltaUnion.getRelation() << "\");\n";
            // if (deltaUnion.getDel)
            // INSERT
            if (both || insertOnly) {
                out << "{\n";
                out << "std::uint64_t __dred_delta_ins_start = 0;\n";
                out << "if (dredProfileEnabled) { __dred_delta_ins_start = DerivationManager::nowNanos(); }\n";
                const bool isRederiveDeltaTuple =
                        deltaUnion.getDeltaTupleInsertRel().find("@inc_delta_tuple_rederive_") == 0;
                const auto deltaDervInsertRelName =
                        synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getDeltaDervInsertRel()));
                const auto deltaTupleInsertRelName =
                        synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getDeltaTupleInsertRel()));
                out << "if (detRel) {\n";
                out << "for(const auto& tupleDeltaDervInsert: *" << deltaDervInsertRelName << ") {\n";
                out << "auto untypedDeltaDervTupleInsert = UntypedTuple::fromTypedTuple(\""
                    << deltaUnion.getRelation() << "\",tupleDeltaDervInsert);\n";
                out << "std::size_t deltaInsertRuleAppCount = 0;\n";
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.ins_delta_tuples++;\n";
                out << "DerivationManager::dredStats.ins_delta_ruleapps += deltaInsertRuleAppCount;\n";
                out << "}\n";
                if (isRederiveDeltaTuple) {
                    out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                    out << "DerivationManager::dredStats.rederive_delta_tuples++;\n";
                    out << "DerivationManager::dredStats.rederive_delta_ruleapps += deltaInsertRuleAppCount;\n";
                    out << "DerivationManager::bumpDredSccRederiveDeltaTuples();\n";
                    out << "DerivationManager::bumpDredSccRederiveDeltaRuleapps(deltaInsertRuleAppCount);\n";
                    out << "}\n";
                }
                out << "DerivationManager::recordDetDeltaInsert(untypedDeltaDervTupleInsert);\n";
                out << "if(!isInputFact(untypedDeltaDervTupleInsert)) {\n";
                out << deltaTupleInsertRelName << "->insert(tupleDeltaDervInsert);\n";
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.ins_tuple_inserts++;\n";
                out << "}\n";
                out << "}\n";
                out << "}\n";
                out << "} else {\n";
                out << "for(const auto& tupleDeltaDervInsert: *" << deltaDervInsertRelName << ") {\n";
                out << "auto untypedDeltaDervTupleInsert = UntypedTuple::fromTypedTuple(\""
                    << deltaUnion.getRelation() << "\",tupleDeltaDervInsert);\n";
                out << "std::size_t deltaInsertRuleAppCount = 0;\n";
                // if (insertOnly) { // also means recursive...
                    out << "std::unordered_set<RuleApplication>* untypedDeltaDervTupleInsertRuleSet = nullptr;\n";
                    out << "if (!detRel) {\n";
                    out << "auto*& untypedDeltaDervTupleInsertRuleSetRef = DerivationManager::untypedTuple2DeltaDeltaInsertRuleApplications[untypedDeltaDervTupleInsert];\n";
                    out << "untypedDeltaDervTupleInsertRuleSet = untypedDeltaDervTupleInsertRuleSetRef;\n";
                    out << "if (untypedDeltaDervTupleInsertRuleSet != nullptr) { deltaInsertRuleAppCount = untypedDeltaDervTupleInsertRuleSet->size(); }\n";
                    out << "}\n";
                // out << "if (untypedDeltaDervTupleInsertRuleSet == nullptr) {\n";
                // out << "untypedDeltaDervTupleInsertRuleSet = new std::unordered_set<RuleApplication>();\n" << std::endl;
                // out << "}\n";
                // } else {
                //     out << "auto*& untypedDeltaDervTupleInsertRuleSet = DerivationManager::untypedTuple2DeltaInsertRuleApplications[untypedDeltaDervTupleInsert];\n" << std::endl;
                // }
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.ins_delta_tuples++;\n";
                out << "DerivationManager::dredStats.ins_delta_ruleapps += deltaInsertRuleAppCount;\n";
                out << "}\n";
                if (isRederiveDeltaTuple) {
                    out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                    out << "DerivationManager::dredStats.rederive_delta_tuples++;\n";
                    out << "DerivationManager::dredStats.rederive_delta_ruleapps += deltaInsertRuleAppCount;\n";
                    out << "DerivationManager::bumpDredSccRederiveDeltaTuples();\n";
                    out << "DerivationManager::bumpDredSccRederiveDeltaRuleapps(deltaInsertRuleAppCount);\n";
                    out << "}\n";
                }
                out << "if (detRel) {\n";
                out << "DerivationManager::recordDetDeltaInsert(untypedDeltaDervTupleInsert);\n";
                out << "if(!isInputFact(untypedDeltaDervTupleInsert)) {\n";
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.ins_tuple_inserts++;\n";
                out << "}\n";
                out << "}\n";
                out << "continue;\n";
                out << "}\n";
                out << "auto*& untypedDeltaDervTupleRuleSet = DerivationManager::untypedTuple2RuleApplications[untypedDeltaDervTupleInsert];\n" << std::endl;
                out << "if (untypedDeltaDervTupleRuleSet == nullptr) {" << std::endl;
                out << "untypedDeltaDervTupleRuleSet = untypedDeltaDervTupleInsertRuleSet;\n" << std::endl;
                out << "DerivationManager::untypedTuple2DeltaDeltaInsertRuleApplications[untypedDeltaDervTupleInsert] = nullptr;\n" << std::endl;
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.ins_complete_sets_attached++;\n";
                out << "}\n";
                out << "if(!isInputFact(untypedDeltaDervTupleInsert)) {\n";
                out << synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getDeltaTupleInsertRel())) << "->insert(tupleDeltaDervInsert);\n";
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.ins_tuple_inserts++;\n";
                out << "}\n";
                out << "}\n";
                out << "} else {" << std::endl;
                out << "untypedDeltaDervTupleRuleSet->insert(untypedDeltaDervTupleInsertRuleSet->begin(), untypedDeltaDervTupleInsertRuleSet->end());\n" << std::endl;
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.ins_ruleapp_merges += deltaInsertRuleAppCount;\n";
                out << "}\n";
                out << "}" << std::endl;
                out << "}" << std::endl;
                // if (insertOnly) {
                out << "DerivationManager::freeRuleApplicationMap(\n";
                out << "        DerivationManager::untypedTuple2DeltaDeltaInsertRuleApplications);\n";
                // }
                out << "if (dredProfileEnabled) {\n";
                out << "DerivationManager::addDredTime(DerivationManager::DredTimeBucket::InsDeltaUnion,\n";
                out << "        DerivationManager::elapsedNanos(__dred_delta_ins_start));\n";
                out << "}\n";
                out << "}\n";
                out << "}\n";
            }
            // DELETE
            if (both || deleteOnly) {
                out << "{\n";
                out << "std::uint64_t __dred_delta_del_start = 0;\n";
                out << "if (dredProfileEnabled) { __dred_delta_del_start = DerivationManager::nowNanos(); }\n";
                const auto deltaDervDeleteRelName =
                        synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getDeltaDervDeleteRel()));
                const auto deltaTupleDeleteRelName =
                        synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getDeltaTupleDeleteRel()));
                out << "if (detRel) {\n";
                out << "for(const auto& tupleDeltaDervDelete: *" << deltaDervDeleteRelName << ") {\n";
                out << "auto untypedDeltaDervTupleDelete = UntypedTuple::fromTypedTuple(\""
                    << deltaUnion.getRelation() << "\",tupleDeltaDervDelete);\n";
                out << "std::size_t deltaDeleteRuleAppCount = 0;\n";
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.del_delta_tuples++;\n";
                out << "DerivationManager::dredStats.del_delta_ruleapps += deltaDeleteRuleAppCount;\n";
                out << "DerivationManager::dredStats.del_ruleapp_erases += deltaDeleteRuleAppCount;\n";
                out << "}\n";
                out << "DerivationManager::recordDetDeltaDelete(untypedDeltaDervTupleDelete);\n";
                out << "if(!isInputFact(untypedDeltaDervTupleDelete)) {\n";
                out << deltaTupleDeleteRelName << "->insert(tupleDeltaDervDelete);\n";
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.del_tuple_deletes++;\n";
                out << "}\n";
                out << "}\n";
                out << "}\n";
                out << "} else {\n";
                out << "for(const auto& tupleDeltaDervDelete: *" << deltaDervDeleteRelName << ") {\n";
                out << "auto untypedDeltaDervTupleDelete = UntypedTuple::fromTypedTuple(\""
                    << deltaUnion.getRelation() << "\",tupleDeltaDervDelete);\n";
                out << "std::size_t deltaDeleteRuleAppCount = 0;\n";
                // if (deleteOnly) {
                    out << "std::unordered_set<RuleApplication>* untypedDeltaDervTupleDeleteRuleSet = nullptr;\n";
                    out << "if (!detRel) {\n";
                    out << "auto*& untypedDeltaDervTupleDeleteRuleSetRef = DerivationManager::untypedTuple2DeltaDeltaDeleteRuleApplications[untypedDeltaDervTupleDelete];\n" << std::endl;
                    out << "untypedDeltaDervTupleDeleteRuleSet = untypedDeltaDervTupleDeleteRuleSetRef;\n";
                    out << "if (untypedDeltaDervTupleDeleteRuleSet != nullptr) { deltaDeleteRuleAppCount = untypedDeltaDervTupleDeleteRuleSet->size(); }\n";
                    out << "}\n";
                // } else {
                //     out << "auto*& untypedDeltaDervTupleDeleteRuleSet = DerivationManager::untypedTuple2DeltaDeleteRuleApplications[untypedDeltaDervTupleDelete];\n" << std::endl;
                // }
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.del_delta_tuples++;\n";
                out << "DerivationManager::dredStats.del_delta_ruleapps += deltaDeleteRuleAppCount;\n";
                out << "DerivationManager::dredStats.del_ruleapp_erases += deltaDeleteRuleAppCount;\n";
                out << "}\n";
                out << "if (detRel) {\n";
                out << "DerivationManager::recordDetDeltaDelete(untypedDeltaDervTupleDelete);\n";
                out << "if(!isInputFact(untypedDeltaDervTupleDelete)) {\n";
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.del_tuple_deletes++;\n";
                out << "}\n";
                out << "}\n";
                out << "continue;\n";
                out << "}\n";
                out << "auto untypedDeltaDervTupleRuleSetIt = DerivationManager::untypedTuple2RuleApplications.find(untypedDeltaDervTupleDelete);\n" << std::endl;
                out << "std::unordered_set<RuleApplication>* untypedDeltaDervTupleRuleSet = "
                       "(untypedDeltaDervTupleRuleSetIt == DerivationManager::untypedTuple2RuleApplications.end()) "
                       "? nullptr : untypedDeltaDervTupleRuleSetIt->second;\n" << std::endl;
                out << "std::uint64_t __dred_ruleapp_erase_start = 0;\n";
                out << "if (dredProfileEnabled) { __dred_ruleapp_erase_start = DerivationManager::nowNanos(); }\n";
                out << "if (untypedDeltaDervTupleDeleteRuleSet != nullptr && untypedDeltaDervTupleRuleSet != nullptr) {\n";
                out << "for(const auto& deletedRuleAppl: *untypedDeltaDervTupleDeleteRuleSet) {" << std::endl;
                out << "untypedDeltaDervTupleRuleSet->erase(deletedRuleAppl);\n" << std::endl;
                out << "}" << std::endl;
                out << "}\n";
                out << "if (dredProfileEnabled) {\n";
                out << "DerivationManager::addDredTime(DerivationManager::DredTimeBucket::DelRuleappErase,\n";
                out << "        DerivationManager::elapsedNanos(__dred_ruleapp_erase_start));\n";
                out << "}\n";
                out << "if (untypedDeltaDervTupleRuleSet == nullptr || untypedDeltaDervTupleRuleSet->empty()) {" << std::endl;
                out << "if (untypedDeltaDervTupleRuleSet != nullptr) {\n";
                out << "delete untypedDeltaDervTupleRuleSet;\n" << std::endl;
                out << "DerivationManager::untypedTuple2RuleApplications.erase(untypedDeltaDervTupleRuleSetIt);\n" << std::endl;
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.del_complete_sets_freed++;\n";
                out << "}\n";
                out << "}\n";
                out << "if(!isInputFact(untypedDeltaDervTupleDelete)) {\n";
                out << synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getDeltaTupleDeleteRel())) << "->insert(tupleDeltaDervDelete);\n";
                out << "if (DerivationManager::isSemStatsEnabled()) {\n";
                out << "DerivationManager::dredStats.del_tuple_deletes++;\n";
                out << "}\n";
                out << "}\n";
                out << "}" << std::endl;
                out << "}" << std::endl;
                out << "DerivationManager::freeRuleApplicationMap(\n";
                out << "        DerivationManager::untypedTuple2DeltaDeltaDeleteRuleApplications);\n";
                out << "if (dredProfileEnabled) {\n";
                out << "DerivationManager::addDredTime(DerivationManager::DredTimeBucket::DelDeltaUnion,\n";
                out << "        DerivationManager::elapsedNanos(__dred_delta_del_start));\n";
                out << "}\n";
                out << "}\n";
                out << "}\n";
            }
            // ADD EVERYTHING UP TO NEW
            // out <<
            if (both) {
                // Apply tuple deletes explicitly before tuple inserts.
                out << "for(const auto& deletedTuple: *" << synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getDeltaTupleDeleteRel())) << ") {\n" << std::endl;
                out << synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getNewRel())) << "->erase(deletedTuple);\n" << std::endl;
                out << "}\n" << std::endl;
                out << "for(const auto& insertedTuple: *" << synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getDeltaTupleInsertRel())) << ") {\n" << std::endl;
                out << synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getNewRel())) << "->insert(insertedTuple);\n" << std::endl;
                out << "}\n" << std::endl;
            } else if (deleteOnly) {
                out << "for(const auto& deletedTuple: *" << synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getDeltaTupleDeleteRel())) << ") {\n" << std::endl;
                    out << synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getNewRel())) << "->erase(deletedTuple);\n" << std::endl;
                out << "}\n" << std::endl;
            } else if (insertOnly) {
                out << "for(const auto& insertedTuple: *" << synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getDeltaTupleInsertRel())) << ") {\n" << std::endl;
                out << synthesiser.getRelationName(synthesiser.lookup(deltaUnion.getNewRel())) << "->insert(insertedTuple);\n" << std::endl;
                out << "}\n" << std::endl;
            } else {
                assert (false);
            }
            out << "}\n";
        }

        void visit_(type_identity<Erase>, const Erase& erase, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            const auto* rel = synthesiser.lookup(erase.getRelation());
            auto arity = rel->getArity();
            auto relName = synthesiser.getRelationName(rel);
            // create inserted tuple
            out << "Tuple<RamDomain," << arity << "> tuple{{" << join(erase.getValues(), ",", rec) << "}};\n";

            // insert tuple
            out << relName << "->erase(tuple);\n";
            PRINT_END_COMMENT(out);
        }

        // -- conditions --

        void visit_(type_identity<True>, const True&, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "true";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<False>, const False&, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "false";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Conjunction>, const Conjunction& conj, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            dispatch(conj.getLHS(), out);
            out << " && ";
            dispatch(conj.getRHS(), out);
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Negation>, const Negation& neg, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "!(";
            dispatch(neg.getOperand(), out);
            out << ")";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Constraint>, const Constraint& rel, std::ostream& out) override {
            // clang-format off
#define EVAL_CHILD(ty, idx)        \
    out << "ramBitCast<" #ty ">("; \
    dispatch(rel.idx(), out);      \
    out << ")"
#define COMPARE_NUMERIC(ty, op) \
    out << "(";                 \
    EVAL_CHILD(ty, getLHS);     \
    out << " " #op " ";         \
    EVAL_CHILD(ty, getRHS);     \
    out << ")";                 \
    break
#define COMPARE_STRING(op)                \
    out << "(symTable.decode(";           \
    EVAL_CHILD(RamDomain, getLHS);        \
    out << ") " #op " symTable.decode(";  \
    EVAL_CHILD(RamDomain, getRHS);        \
    out << "))";                          \
    break
#define COMPARE_EQ_NE(opCode, op)                                         \
    case BinaryConstraintOp::   opCode: COMPARE_NUMERIC(RamDomain  , op); \
    case BinaryConstraintOp::F##opCode: COMPARE_NUMERIC(RamFloat   , op);
#define COMPARE(opCode, op)                                               \
    case BinaryConstraintOp::   opCode: COMPARE_NUMERIC(RamSigned  , op); \
    case BinaryConstraintOp::U##opCode: COMPARE_NUMERIC(RamUnsigned, op); \
    case BinaryConstraintOp::F##opCode: COMPARE_NUMERIC(RamFloat   , op); \
    case BinaryConstraintOp::S##opCode: COMPARE_STRING(op);
            // clang-format on

            PRINT_BEGIN_COMMENT(out);
            switch (rel.getOperator()) {
                // comparison operators
                COMPARE_EQ_NE(EQ, ==)
                COMPARE_EQ_NE(NE, !=)

                COMPARE(LT, <)
                COMPARE(LE, <=)
                COMPARE(GT, >)
                COMPARE(GE, >=)

                // strings
                case BinaryConstraintOp::MATCH: {
                    if (const StringConstant* str = as<StringConstant>(&rel.getLHS()); str) {
                        const auto& regex = synthesiser.compileRegex(str->getConstant());
                        if (regex) {
                            out << "std::regex_match(symTable.decode(";
                            dispatch(rel.getRHS(), out);
                            out << "), regexes.at(" << *regex << "))";
                        } else {
                            out << "false";
                        }
                    } else {
                        synthesiser.SubroutineUsingStdRegex = true;
                        out << "regex_wrapper(symTable.decode(";
                        dispatch(rel.getLHS(), out);
                        out << "),symTable.decode(";
                        dispatch(rel.getRHS(), out);
                        out << "))";
                    }
                    break;
                }
                case BinaryConstraintOp::NOT_MATCH: {
                    if (const StringConstant* str = as<StringConstant>(&rel.getLHS()); str) {
                        const auto& regex = synthesiser.compileRegex(str->getConstant());
                        if (regex) {
                            out << "!std::regex_match(symTable.decode(";
                            dispatch(rel.getRHS(), out);
                            out << "), regexes.at(" << *regex << "))";
                        } else {
                            out << "false";
                        }
                    } else {
                        synthesiser.SubroutineUsingStdRegex = true;
                        out << "!regex_wrapper(symTable.decode(";
                        dispatch(rel.getLHS(), out);
                        out << "),symTable.decode(";
                        dispatch(rel.getRHS(), out);
                        out << "))";
                    }
                    break;
                }
                case BinaryConstraintOp::CONTAINS: {
                    out << "(symTable.decode(";
                    dispatch(rel.getRHS(), out);
                    out << ").find(symTable.decode(";
                    dispatch(rel.getLHS(), out);
                    out << ")) != std::string::npos)";
                    break;
                }
                case BinaryConstraintOp::NOT_CONTAINS: {
                    out << "(symTable.decode(";
                    dispatch(rel.getRHS(), out);
                    out << ").find(symTable.decode(";
                    dispatch(rel.getLHS(), out);
                    out << ")) == std::string::npos)";
                    break;
                }
            }

            PRINT_END_COMMENT(out);

#undef EVAL_CHILD
#undef COMPARE_NUMERIC
#undef COMPARE_STRING
#undef COMPARE
#undef COMPARE_EQ_NE
        }

        void visit_(
                type_identity<EmptinessCheck>, const EmptinessCheck& emptiness, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << synthesiser.getRelationName(synthesiser.lookup(emptiness.getRelation())) << "->"
                << "empty()";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<RelationSize>, const RelationSize& size, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "(RamDomain)" << synthesiser.getRelationName(synthesiser.lookup(size.getRelation()))
                << "->"
                << "size()";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<ExistenceCheck>, const ExistenceCheck& exists, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            // get some details
            const auto* rel = synthesiser.lookup(exists.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            auto arity = rel->getArity();
            assert(arity > 0 && "AstToRamTranslator failed");
            std::string after;
            if (glb.config().has("profile") && glb.config().has("profile-frequency") &&
                    !synthesiser.lookup(exists.getRelation())->isTemp()) {
                out << R"_((reads[)_" << synthesiser.lookupReadIdx(rel->getName()) << R"_(]++,)_";
                after = ")";
            }

            // if it is total we use the contains function
            if (isa->isTotalSignature(&exists)) {
                out << relName << "->"
                    << "contains(Tuple<RamDomain," << arity << ">{{" << join(exists.getValues(), ",", rec)
                    << "}}," << ctxName << ")" << after;
                PRINT_END_COMMENT(out);
                return;
            }

            auto rangePatternLower = exists.getValues();
            auto rangePatternUpper = exists.getValues();

            auto rangeBounds = getPaddedRangeBounds(*rel, rangePatternLower, rangePatternUpper);
            // else we conduct a range query
            out << "!" << relName << "->"
                << "lowerUpperRange";
            out << "_" << isa->getSearchSignature(&exists);
            out << "(" << rangeBounds.first.str() << "," << rangeBounds.second.str() << "," << ctxName
                << ").empty()" << after;
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<DerivationCheck>, const DerivationCheck& derivationCheck, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            // get some details
            const auto* rel = synthesiser.lookup(derivationCheck.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            auto arity = rel->getArity();
            std::string after;
            // if (glb.config().has("profile") && glb.config().has("profile-frequency") &&
            //         !synthesiser.lookup(derivationCheck.getRelation())->isTemp()) {
            //     out << R"_((reads[)_" << synthesiser.lookupReadIdx(rel->getName()) << R"_(]++,)_";
            //     after = ")";
            //         }

            // if it is total we use the contains function
            if (isa->isTotalSignature(&derivationCheck)) {
                const std::string baseRelName = getBaseRelationName(derivationCheck.getRelation());
                out << "(" << relName << "->"
                    << "contains(Tuple<RamDomain," << arity << ">{{" << join(derivationCheck.getValues(), ",", rec)
                    << "}}";
                // Nullary guards may be hoisted outside the query's context
                // scope, and their relation lookup needs no index hints.
                if (arity != 0) {
                    out << "," << ctxName;
                }
                out << ")" << ")";
                out << "&& ";
                out << "(";
                out << "(detOptEnabled && isDetRelation(\"" << baseRelName << "\"))";
                out << " || ";
                out << "DerivationManager::ruleAppExistsInCompleteSet("
                    << "UntypedTuple::fromTypedTuple("
                    << "\"" << baseRelName << "\""
                    << ", Tuple<RamDomain," << arity << ">{{" << join(derivationCheck.getValues(), ",", rec)
                    << "}}),"
                    << "RuleApplication{"
                    << derivationCheck.clauseID
                    << ",";
                derivationCheck.outputVarExprsString(out, rec);
                out << "})";
                out << ")";
                PRINT_END_COMMENT(out);
                return;
            }

            assert (false && "unnamed arg TBD");

            // auto rangePatternLower = derivationCheck.getValues();
            // auto rangePatternUpper = derivationCheck.getValues();
            //
            // auto rangeBounds = getPaddedRangeBounds(*rel, rangePatternLower, rangePatternUpper);
            // // else we conduct a range query
            // out << "(!" << relName << "->"
            //     << "lowerUpperRange";
            // out << "_" << isa->getSearchSignature(&derivationCheck);
            // out << "(" << rangeBounds.first.str() << "," << rangeBounds.second.str() << "," << ctxName
            //     << ").empty())";
            // out << "&&" << "DerivationManager::untypedTuple2RuleApplications.contain(" << ");";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<ProvenanceExistenceCheck>, const ProvenanceExistenceCheck& provExists,
                std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            // get some details
            const auto* rel = synthesiser.lookup(provExists.getRelation());
            auto relName = synthesiser.getRelationName(rel);
            auto ctxName = "READ_OP_CONTEXT(" + synthesiser.getOpContextName(*rel) + ")";
            auto arity = rel->getArity();
            auto auxiliaryArity = rel->getAuxiliaryArity();

            // provenance not exists is never total, conduct a range query
            out << "[&]() -> bool {\n";
            out << "auto existenceCheck = " << relName << "->"
                << "lowerUpperRange";
            out << "_" << isa->getSearchSignature(&provExists);

            // parts refers to payload + rule number
            std::size_t parts = arity - auxiliaryArity + 1;

            // make a copy of provExists.getValues() so we can be sure that vals is always the same vector
            // since provExists.getValues() creates a new vector on the stack each time
            auto vals = provExists.getValues();

            // sanity check to ensure that all payload values are specified
            for (std::size_t i = 0; i < arity - auxiliaryArity; i++) {
                assert(!isUndefValue(vals[i]) &&
                        "ProvenanceExistenceCheck should always be specified for payload");
            }

            auto valsCopy = std::vector<Expression*>(vals.begin(), vals.begin() + parts);
            auto rangeBounds = getPaddedRangeBounds(*rel, valsCopy, valsCopy);

            // remove the ending }} from both strings
            rangeBounds.first.seekp(-2, std::ios_base::end);
            rangeBounds.second.seekp(-2, std::ios_base::end);

            // extra bounds for provenance height annotations
            for (std::size_t i = 0; i < auxiliaryArity - 2; i++) {
                rangeBounds.first << ",ramBitCast<RamDomain, RamSigned>(MIN_RAM_SIGNED)";
                rangeBounds.second << ",ramBitCast<RamDomain, RamSigned>(MAX_RAM_SIGNED)";
            }
            rangeBounds.first << ",ramBitCast<RamDomain, RamSigned>(MIN_RAM_SIGNED)}}";
            rangeBounds.second << ",ramBitCast<RamDomain, RamSigned>(MAX_RAM_SIGNED)}}";

            out << "(" << rangeBounds.first.str() << "," << rangeBounds.second.str() << "," << ctxName
                << ");\n";
            out << "if (existenceCheck.empty()) return false; else return ((*existenceCheck.begin())["
                << arity - auxiliaryArity + 1 << "] <= ";

            dispatch(*(provExists.getValues()[arity - auxiliaryArity + 1]), out);
            out << ")";
            out << ";}()\n";
            PRINT_END_COMMENT(out);
        }

        // -- values --
        void visit_(type_identity<UnsignedConstant>, const UnsignedConstant& constant,
                std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "RamUnsigned(" << constant.getValue() << ")";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<FloatConstant>, const FloatConstant& constant, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "RamFloat(" << constant.getValue() << ")";
            PRINT_END_COMMENT(out);
        }

        void visit_(
                type_identity<SignedConstant>, const SignedConstant& constant, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "RamSigned(" << constant.getConstant() << ")";
            PRINT_END_COMMENT(out);
        }

        void visit_(
                type_identity<StringConstant>, const StringConstant& constant, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "RamSigned(" << synthesiser.convertSymbol2Idx(constant.getConstant()) << ")";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<Variable>, const Variable& v, std::ostream& out) override {
            out << v.getName();
        }

        void visit_(type_identity<TupleElement>, const TupleElement& access, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "env" << access.getTupleId() << "[" << access.getElement() << "]";
            PRINT_END_COMMENT(out);
        }

        void visit_(type_identity<AutoIncrement>, const AutoIncrement& /*inc*/, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);
            out << "(ctr++)";
            PRINT_END_COMMENT(out);
        }

        void visit_(
                type_identity<IntrinsicOperator>, const IntrinsicOperator& op, std::ostream& out) override {
#define MINMAX_SYMBOL(op)                   \
    {                                       \
        out << "symTable.encode(" #op "({"; \
        for (auto& cur : args) {            \
            out << "symTable.decode(";      \
            dispatch(*cur, out);            \
            out << "), ";                   \
        }                                   \
        out << "}))";                       \
        break;                              \
    }

            PRINT_BEGIN_COMMENT(out);

            // clang-format off
#define UNARY_OP(opcode, ty, op)                \
    case FunctorOp::opcode: {                   \
        out << "(" #op "(ramBitCast<" #ty ">("; \
        dispatch(*args[0], out);                \
        out << ")))";                           \
        break;                                  \
    }
#define UNARY_OP_I(opcode, op) UNARY_OP(   opcode, RamSigned  , op)
#define UNARY_OP_U(opcode, op) UNARY_OP(U##opcode, RamUnsigned, op)
#define UNARY_OP_F(opcode, op) UNARY_OP(F##opcode, RamFloat   , op)
#define UNARY_OP_INTEGRAL(opcode, op) \
    UNARY_OP_I(opcode, op)            \
    UNARY_OP_U(opcode, op)


#define BINARY_OP_EXPR_EX(ty, op, rhs_post)      \
    {                                            \
        out << "(ramBitCast<" #ty ">(";          \
        dispatch(*args[0], out);                 \
        out << ") " #op " ramBitCast<" #ty ">("; \
        dispatch(*args[1], out);                 \
        out << rhs_post "))";                    \
        break;                                   \
    }
#define BINARY_OP_EXPR(ty, op) BINARY_OP_EXPR_EX(ty, op, "")
#define BINARY_OP_EXPR_SHIFT(ty, op) BINARY_OP_EXPR_EX(ty, op, " & RAM_BIT_SHIFT_MASK")
#define BINARY_OP_EXPR_LOGICAL(ty, op) out << "RamDomain"; BINARY_OP_EXPR(ty, op)

#define BINARY_OP_INTEGRAL(opcode, op)                         \
    case FunctorOp::   opcode: BINARY_OP_EXPR(RamSigned  , op) \
    case FunctorOp::U##opcode: BINARY_OP_EXPR(RamUnsigned, op)
#define BINARY_OP_LOGICAL(opcode, op)                                  \
    case FunctorOp::   opcode: BINARY_OP_EXPR_LOGICAL(RamSigned  , op) \
    case FunctorOp::U##opcode: BINARY_OP_EXPR_LOGICAL(RamUnsigned, op)
#define BINARY_OP_NUMERIC(opcode, op)                          \
    BINARY_OP_INTEGRAL(opcode, op)                             \
    case FunctorOp::F##opcode: BINARY_OP_EXPR(RamFloat   , op)
#define BINARY_OP_BITWISE(opcode, op)                        \
    case FunctorOp::   opcode: /* fall through */            \
    case FunctorOp::U##opcode: BINARY_OP_EXPR(RamDomain, op)
#define BINARY_OP_INTEGRAL_SHIFT(opcode, op, tySigned, tyUnsigned)  \
    case FunctorOp::   opcode: BINARY_OP_EXPR_SHIFT(tySigned  , op) \
    case FunctorOp::U##opcode: BINARY_OP_EXPR_SHIFT(tyUnsigned, op)

#define BINARY_OP_EXP(opcode, ty, tyTemp)                                                     \
    case FunctorOp::opcode: {                                                                 \
        out << "static_cast<" #ty ">(static_cast<" #tyTemp ">(std::pow(ramBitCast<" #ty ">("; \
        dispatch(*args[0], out);                                                              \
        out << "), ramBitCast<" #ty ">(";                                                     \
        dispatch(*args[1], out);                                                              \
        out << "))))";                                                                        \
        break;                                                                                \
    }

#define NARY_OP(opcode, ty, op)            \
    case FunctorOp::opcode: {              \
        out << #op "({";                   \
        for (auto& cur : args) {           \
            out << "ramBitCast<" #ty ">("; \
            dispatch(*cur, out);           \
            out << "), ";                  \
        }                                  \
        out << "})";                       \
        break;                             \
    }
#define NARY_OP_ORDERED(opcode, op)     \
    NARY_OP(   opcode, RamSigned  , op) \
    NARY_OP(U##opcode, RamUnsigned, op) \
    NARY_OP(F##opcode, RamFloat   , op)


#define CONV_TO_STRING(opcode, ty)                \
    case FunctorOp::opcode: {                     \
        out << "symTable.encode(std::to_string("; \
        dispatch(*args[0], out);                  \
        out << "))";                              \
    } break;
#define CONV_FROM_STRING(opcode, ty)                                                       \
    case FunctorOp::opcode: {                                                              \
        synthesiser.currentClass->addInclude("\"souffle/utility/EvaluatorUtil.h\"", true); \
        out << "souffle::evaluator::symbol2numeric<" #ty ">(symTable.decode(";             \
        dispatch(*args[0], out);                                                           \
        out << "))";                                                                       \
    } break;
            // clang-format on
            if (op.getOperator() == FunctorOp::LXOR) {
                synthesiser.currentClass->addInclude("\"souffle/utility/EvaluatorUtil.h\"", true);
            }
            auto args = op.getArguments();
            switch (op.getOperator()) {
                /** Unary Functor Operators */
                case FunctorOp::ORD: {
                    dispatch(*args[0], out);
                    break;
                }
                // STRLEN currently returns a signed RAM value.
                case FunctorOp::STRLEN: {
                    out << "static_cast<RamSigned>(symTable.decode(";
                    dispatch(*args[0], out);
                    out << ").size())";
                    break;
                }

                    // clang-format off
                UNARY_OP_I(NEG, -)
                UNARY_OP_F(NEG, -)

                UNARY_OP_INTEGRAL(BNOT, ~)
                UNARY_OP_INTEGRAL(LNOT, (RamDomain)!)

                /** numeric coersions follow C++ semantics. */
                // identities
                case FunctorOp::F2F:
                case FunctorOp::I2I:
                case FunctorOp::U2U:
                case FunctorOp::S2S: {
                    dispatch(*args[0], out);
                    break;
                }

                UNARY_OP(F2I, RamFloat   , static_cast<RamSigned>)
                UNARY_OP(F2U, RamFloat   , static_cast<RamUnsigned>)

                UNARY_OP(I2U, RamSigned  , static_cast<RamUnsigned>)
                UNARY_OP(I2F, RamSigned  , static_cast<RamFloat>)

                UNARY_OP(U2I, RamUnsigned, static_cast<RamSigned>)
                UNARY_OP(U2F, RamUnsigned, static_cast<RamFloat>)

                CONV_TO_STRING(F2S, RamFloat)
                CONV_TO_STRING(I2S, RamSigned)
                CONV_TO_STRING(U2S, RamUnsigned)

                CONV_FROM_STRING(S2F, RamFloat)
                CONV_FROM_STRING(S2I, RamSigned)
                CONV_FROM_STRING(S2U, RamUnsigned)

                /** Binary Functor Operators */
                // arithmetic

                BINARY_OP_NUMERIC(ADD, +)
                BINARY_OP_NUMERIC(SUB, -)
                BINARY_OP_NUMERIC(MUL, *)
                BINARY_OP_NUMERIC(DIV, /)
                BINARY_OP_INTEGRAL(MOD, %)

                BINARY_OP_EXP(FEXP, RamFloat   , RamFloat)
#if RAM_DOMAIN_SIZE == 32
                BINARY_OP_EXP(UEXP, RamUnsigned, int64_t)
                BINARY_OP_EXP( EXP, RamSigned  , int64_t)
#elif RAM_DOMAIN_SIZE == 64
                BINARY_OP_EXP(UEXP, RamUnsigned, RamUnsigned)
                BINARY_OP_EXP( EXP, RamSigned  , RamSigned)
#else
#error "unhandled domain size"
#endif

                BINARY_OP_LOGICAL(LAND, &&)
                BINARY_OP_LOGICAL(LOR , ||)
                BINARY_OP_LOGICAL(LXOR, + souffle::evaluator::lxor_infix() +)

                BINARY_OP_BITWISE(BAND, &)
                BINARY_OP_BITWISE(BOR , |)
                BINARY_OP_BITWISE(BXOR, ^)
                // Handle left-shift as unsigned to match Java semantics of `<<`, namely:
                //  "... `n << s` is `n` left-shifted `s` bit positions; ..."
                // Using `RamSigned` would imply UB due to signed overflow when shifting negatives.
                BINARY_OP_INTEGRAL_SHIFT(BSHIFT_L         , <<, RamUnsigned, RamUnsigned)
                // For right-shift, we do need sign extension.
                BINARY_OP_INTEGRAL_SHIFT(BSHIFT_R         , >>, RamSigned  , RamUnsigned)
                BINARY_OP_INTEGRAL_SHIFT(BSHIFT_R_UNSIGNED, >>, RamUnsigned, RamUnsigned)

                NARY_OP_ORDERED(MAX, std::max)
                NARY_OP_ORDERED(MIN, std::min)
                    // clang-format on

                case FunctorOp::SMAX: MINMAX_SYMBOL(std::max)

                case FunctorOp::SMIN: MINMAX_SYMBOL(std::min)

                // strings
                case FunctorOp::CAT: {
                    out << "symTable.encode(";
                    std::size_t i = 0;
                    while (i < args.size() - 1) {
                        out << "symTable.decode(";
                        dispatch(*args[i], out);
                        out << ") + ";
                        i++;
                    }
                    out << "symTable.decode(";
                    dispatch(*args[i], out);
                    out << "))";
                    break;
                }

                /** Ternary Functor Operators */
                case FunctorOp::SUBSTR: {
                    synthesiser.SubroutineUsingSubstr = true;
                    out << "symTable.encode(";
                    out << "substr_wrapper(symTable.decode(";
                    dispatch(*args[0], out);
                    out << "),(";
                    dispatch(*args[1], out);
                    out << "),(";
                    dispatch(*args[2], out);
                    out << ")))";
                    break;
                }

                case FunctorOp::RANGE:
                case FunctorOp::URANGE:
                case FunctorOp::FRANGE:
                    fatal("ICE: functor `%s` must map onto `NestedIntrinsicOperator`", op.getOperator());

                case FunctorOp::SSADD: {
                    const StringConstant* lstr = as<StringConstant>(args[0]);
                    const StringConstant* rstr = as<StringConstant>(args[1]);
                    if (lstr && rstr) {
                        out << "RamSigned("
                            << synthesiser.convertSymbol2Idx(lstr->getConstant() + rstr->getConstant())
                            << ")";
                    } else {
                        out << "symTable.encode(";
                        if (lstr) {
                            out << "R\"_(" << lstr->getConstant() << ")_\"";
                        } else {
                            out << "symTable.decode(";
                            dispatch(*args[0], out);
                            out << ")";
                        }
                        out << " + ";
                        if (rstr) {
                            out << "R\"_(" << rstr->getConstant() << ")_\"";
                        } else {
                            out << "symTable.decode(";
                            dispatch(*args[1], out);
                            out << ")";
                        }
                        out << ")";
                    }
                    break;
                }
            }
            PRINT_END_COMMENT(out);

#undef MINMAX_SYMBOL
        }

        void visit_(type_identity<NestedIntrinsicOperator>, const NestedIntrinsicOperator& op,
                std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);

            auto emitHelper = [&](auto&& func) {
                tfm::format(out, "%s(%s, [&](auto&& env%d) {\n", func,
                        join(op.getArguments(), ",", [&](auto& os, auto* arg) { return dispatch(*arg, os); }),
                        op.getTupleId());
                visit_(type_identity<TupleOperation>(), op, out);
                out << "});\n";

                PRINT_END_COMMENT(out);
            };

            auto emitRange = [&](char const* ty) {
                synthesiser.currentClass->addInclude("\"souffle/utility/EvaluatorUtil.h\"", true);
                return emitHelper(tfm::format("souffle::evaluator::runRange<%s>", ty));
            };

            switch (op.getFunction()) {
                case NestedIntrinsicOp::RANGE: return emitRange("RamSigned");
                case NestedIntrinsicOp::URANGE: return emitRange("RamUnsigned");
                case NestedIntrinsicOp::FRANGE: return emitRange("RamFloat");
            }

            UNREACHABLE_BAD_CASE_ANALYSIS
        }

        void visit_(type_identity<UserDefinedOperator>, const UserDefinedOperator& op,
                std::ostream& out) override {
            const std::string& name = op.getName();

            auto args = op.getArguments();
            if (op.isStateful()) {
                out << name << "(&symTable, &recordTable";
                for (auto& arg : args) {
                    out << ",";
                    dispatch(*arg, out);
                }
                out << ")";
            } else {
                const std::vector<TypeAttribute>& argTypes = op.getArgsTypes();

                if (op.getReturnType() == TypeAttribute::Symbol) {
                    out << "symTable.encode(";
                }
                out << name << "(";

                for (std::size_t i = 0; i < args.size(); i++) {
                    if (i > 0) {
                        out << ",";
                    }
                    switch (argTypes[i]) {
                        case TypeAttribute::Signed:
                            out << "((RamSigned)";
                            dispatch(*args[i], out);
                            out << ")";
                            break;
                        case TypeAttribute::Unsigned:
                            out << "((RamUnsigned)";
                            dispatch(*args[i], out);
                            out << ")";
                            break;
                        case TypeAttribute::Float:
                            out << "((RamFloat)";
                            dispatch(*args[i], out);
                            out << ")";
                            break;
                        case TypeAttribute::Symbol:
                            out << "symTable.decode(";
                            dispatch(*args[i], out);
                            out << ").c_str()";
                            break;
                        case TypeAttribute::ADT:
                        case TypeAttribute::Record: fatal("unhandled type");
                    }
                }
                out << ")";
                if (op.getReturnType() == TypeAttribute::Symbol) {
                    out << ")";
                }
            }
        }

        // -- records --

        void visit_(type_identity<PackRecord>, const PackRecord& pack, std::ostream& out) override {
            PRINT_BEGIN_COMMENT(out);

            const auto arity = pack.getArguments().size();

            synthesiser.arities.emplace(arity);

            out << "pack(recordTable,"
                << "Tuple<RamDomain," << arity << ">";
            if (pack.getArguments().size() == 0) {
                out << "{{}}";
            } else {
                out << "{{ramBitCast(" << join(pack.getArguments(), "),ramBitCast(", rec) << ")}}\n";
            }
            out << ")";

            PRINT_END_COMMENT(out);
        }

        // -- subroutine argument --

        void visit_(type_identity<SubroutineArgument>, const SubroutineArgument& arg,
                std::ostream& out) override {
            out << "(args)[" << arg.getArgument() << "]";
        }

        // -- subroutine return --

        void visit_(
                type_identity<SubroutineReturn>, const SubroutineReturn& ret, std::ostream& out) override {
            out << "std::lock_guard<std::mutex> guard(lock);\n";
            for (auto val : ret.getValues()) {
                if (isUndefValue(val)) {
                    out << "ret.push_back(0);\n";
                } else {
                    out << "ret.push_back(";
                    dispatch(*val, out);
                    out << ");\n";
                }
            }
        }

        // -- safety net --

        void visit_(type_identity<UndefValue>, const UndefValue&, std::ostream& /*out*/) override {
            fatal("Compilation error");
        }

        void visit_(type_identity<Node>, const Node& node, std::ostream& /*out*/) override {
            fatal("Unsupported node type: %s", typeid(node).name());
        }
    };

    out << std::setprecision(std::numeric_limits<RamFloat>::max_digits10);
    // emit code
    CodeEmitter(*this).dispatch(stmt, out);
}

std::set<std::string> Synthesiser::accessedRelations(Statement& stmt) {
    std::set<std::string> accessed;
    visit(stmt, [&](const Insert& node) { accessed.insert(node.getRelation()); });
    visit(stmt, [&](const DeltaUnion& node) {
        if (node.getOldRel() != "") {
            accessed.insert(node.getOldRel());
        }
        if (node.getNewRel() != "") {
            accessed.insert(node.getNewRel());
        }
        if (node.getDeltaDervInsertRel() != "") {
            accessed.insert(node.getDeltaDervInsertRel());
        }
        if (node.getDeltaDervDeleteRel() != "") {
            accessed.insert(node.getDeltaDervDeleteRel());
        }
        if (node.getDeltaTupleDeleteRel() != "") {
            accessed.insert(node.getDeltaTupleDeleteRel());
        }
        if (node.getDeltaTupleInsertRel() != "") {
            accessed.insert(node.getDeltaTupleInsertRel());
        }
    });
    visit(stmt, [&](const RelationOperation& node) { accessed.insert(node.getRelation()); });
    visit(stmt, [&](const RelationStatement& node) { accessed.insert(node.getRelation()); });
    visit(stmt, [&](const AbstractExistenceCheck& node) { accessed.insert(node.getRelation()); });
    visit(stmt, [&](const EmptinessCheck& node) { accessed.insert(node.getRelation()); });
    visit(stmt, [&](const RelationSize& node) { accessed.insert(node.getRelation()); });
    visit(stmt, [&](const BinRelationStatement& node) {
        accessed.insert(node.getFirstRelation());
        accessed.insert(node.getSecondRelation());
    });
    return accessed;
}

std::set<std::string> Synthesiser::accessedUserDefinedFunctors(Statement& stmt) {
    std::set<std::string> accessed;
    visit(stmt, [&](const UserDefinedOperator& node) {
        const std::string& name = node.getName();
        accessed.insert(name);
    });
    auto visitAggregate = [&](const AbstractAggregate& op) {
        const Aggregator& aggregator = op.getAggregator();
        if (const auto* uda = as<UserDefinedAggregator>(aggregator)) {
            accessed.insert(uda->getName());
        }
    };
    visit(stmt, [&](const Aggregate& op) { visitAggregate(op); });
    visit(stmt, [&](const IndexAggregate& op) { visitAggregate(op); });
    return accessed;
};

void Synthesiser::generateCode(GenDb& db, const std::string& id, bool& withSharedLibrary) {
    // ---------------------------------------------------------------
    //                      Auto-Index Generation
    // ---------------------------------------------------------------
    const Program& prog = translationUnit.getProgram();
    auto& idxAnalysis = translationUnit.getAnalysis<IndexAnalysis>();
    // ---------------------------------------------------------------
    //                      Code Generation
    // ---------------------------------------------------------------

    withSharedLibrary = false;

    std::string classname = "Sf_" + id;

    // generate C++ program
    std::string package_gen_version = "SOUFFLE_GENERATOR_VERSION \"";
    package_gen_version += PACKAGE_VERSION;
    package_gen_version += "\"";
    db.addGlobalDefine(package_gen_version);

    if (glb.config().has("verbose")) {
        db.addGlobalDefine("_SOUFFLE_STATS");
        db.addGlobalInclude("\"souffle/profile/ProfileEvent.h\"");
    }

    db.addGlobalInclude("\"souffle/utility/MiscUtil.h\"");
    if (glb.config().has("profile")) {
        db.addGlobalInclude("\"souffle/profile/Logger.h\"");
        db.addGlobalInclude("\"souffle/profile/ProfileEvent.h\"");
    }

    if (glb.config().has("generate-namespace")) {
        db.setNS(glb.config().get("generate-namespace"));
    } else {
        db.setNS("souffle");
    }

    // include derivation manager
    db.addGlobalInclude("\"souffle/Derivation.h\"");

    // produce external definitions for user-defined functors
    std::map<std::string, std::tuple<TypeAttribute, std::vector<TypeAttribute>, bool>> functors;
    visit(prog, [&](const UserDefinedOperator& op) {
        if (functors.find(op.getName()) == functors.end()) {
            functors[op.getName()] = std::make_tuple(op.getReturnType(), op.getArgsTypes(), op.isStateful());
        }
        withSharedLibrary = true;
    });
    auto visitAggregate = [&](const AbstractAggregate& op) {
        const Aggregator& aggregator = op.getAggregator();
        if (const auto* uda = as<UserDefinedAggregator>(aggregator)) {
            functors[uda->getName()] =
                    std::make_tuple(uda->getReturnType(), uda->getArgsTypes(), uda->isStateful());
            withSharedLibrary = true;
        }
    };
    visit(prog, [&](const Aggregate& op) { visitAggregate(op); });
    visit(prog, [&](const IndexAggregate& op) { visitAggregate(op); });

    for (const auto& f : functors) {
        const std::string& name = f.first;

        const auto& functorTypes = f.second;
        const auto& returnType = std::get<0>(functorTypes);
        const auto& argsTypes = std::get<1>(functorTypes);
        const auto& stateful = std::get<2>(functorTypes);

        auto cppTypeDecl = [](TypeAttribute ty) -> char const* {
            switch (ty) {
                case TypeAttribute::Signed: return "souffle::RamSigned";
                case TypeAttribute::Unsigned: return "souffle::RamUnsigned";
                case TypeAttribute::Float: return "souffle::RamFloat";
                case TypeAttribute::Symbol: return "const char *";
                case TypeAttribute::ADT: fatal("adts cannot be used by user-defined functors");
                case TypeAttribute::Record: fatal("records cannot be used by user-defined functors");
            }

            UNREACHABLE_BAD_CASE_ANALYSIS
        };

        std::vector<std::string> argsTy;
        std::string retTy;
        if (stateful) {
            retTy = "souffle::RamDomain";
            argsTy.push_back("souffle::SymbolTable*");
            argsTy.push_back("souffle::RecordTable*");
            for (std::size_t i = 0; i < argsTypes.size(); i++) {
                argsTy.push_back("souffle::RamDomain");
            }
        } else {
            retTy = cppTypeDecl(returnType);
            for (auto ty : argsTypes) {
                argsTy.push_back(cppTypeDecl(ty));
            }
        }
        functor_signatures[name] = std::make_pair(argsTy, retTy);
        auto extern_decl = [&](std::ostream& os) {
            os << retTy << " " << name << "("
               << join(argsTy, ", ", [&](auto& out, const std::string ty) { out << ty; }) << ");\n";
        };

        extern_decl(db.externC());
    }

    // main class
    GenClass& mainClass = db.getClass(classname, fs::path(classname));
    mainClass.inherits("public SouffleProgram");
    mainClass.addInclude("\"souffle/CompiledSouffle.h\"");
    mainClass.addInclude("<any>");
    mainClass.isMain = true;

    auto function_ty = [&](std::string name) -> std::string {
        auto [argsTy, retTy] = functor_signatures[name];
        std::stringstream os;
        os << "std::function<" << retTy << "("
           << join(argsTy, ", ", [&](auto& out, const std::string ty) { out << ty; }) << ")>";
        return os.str();
    };
    auto functors_initialize = [&](std::ostream& os, std::string name) {
        auto [argsTy, retTy] = functor_signatures[name];
        os << name << " = functors::" << name << ";\n";
    };

    std::map<std::string, std::string> relationTypes;

    // synthesise data-structures for relations
    for (auto rel : prog.getRelations()) {
        auto relationType =
                Relation::getSynthesiserRelation(*rel, idxAnalysis.getIndexSelection(rel->getName()));

        std::string typeName = relationType->getTypeName();
        generateRelationTypeStruct(db, std::move(relationType));

        relationTypes[getRelationName(*rel)] = typeName;

        db.usesDatastructure(mainClass, typeName);
    }

    std::set<std::string> loadRelations;
    std::set<const IO*> loadIOs;
    std::set<const IO*> storeIOs;

    // collect load/store operations/relations
    visit(prog, [&](const IO& io) {
        auto op = io.get("operation");
        if (op == "input") {
            loadRelations.insert(io.getRelation());
            loadIOs.insert(&io);
        } else if (op == "printsize" || op == "output") {
            storeRelations.insert(io.getRelation());
            storeIOs.insert(&io);
        } else {
            assert("wrong I/O operation");
        }
    });

    //-------------------------------------
    // generate evidences
    //--------------------------------------
    if (prog.getEvidences().empty()) {
        mainClass.hooks() << "std::vector<std::pair<UntypedTuple,bool>> evidences;\n";
    } else {
        std::stringstream ss;
        ss << "std::vector<std::pair<UntypedTuple,bool>> evidences = {\n";

        for (const auto& evi : prog.getEvidences()) {
            const auto* relDecl = lookup(evi->getRelation());
            assert(relDecl != nullptr && "Evidence relation must be declared");
            std::string tupleText = evi->getTupleString();
            auto lp = tupleText.find('(');
            auto rp = tupleText.rfind(')');
            if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
                tupleText = tupleText.substr(lp + 1, rp - lp - 1);
            }
            const auto tupleFields = splitRenderedTupleFieldsForCodegen(tupleText);
            assert(tupleFields.size() == relDecl->getArity() &&
                    "Evidence arity must match declared relation arity");
            ss << "    {UntypedTuple{\"" << evi->getRelation() << "\", {";
            for (std::size_t i = 0; i < tupleFields.size(); ++i) {
                std::string token = tupleFields[i];
                const auto first = token.find_first_not_of(" \t\r\n");
                assert(first != std::string::npos && "Evidence fields must be non-empty");
                token.erase(0, first);
                const auto last = token.find_last_not_of(" \t\r\n");
                token.erase(last + 1);
                const auto typeAttr = relDecl->getAttributeTypes()[i];
                switch (typeAttr[0]) {
                    case 's': {
                        std::string err;
                        auto parsed = json11::Json::parse(token, err);
                        assert(err.empty() && parsed.is_string() &&
                                "Symbolic evidence fields must be quoted strings");
                        ss << "souffle::RamSigned(" << convertSymbol2Idx(parsed.string_value()) << ")";
                        break;
                    }
                    case 'f':
                        ss << "souffle::ramBitCast<souffle::RamDomain>(souffle::RamUnsigned("
                           << souffle::ramBitCast<souffle::RamUnsigned>(souffle::RamFloatFromString(token))
                           << "ULL))";
                        break;
                    case 'u':
                        ss << "souffle::ramBitCast<souffle::RamDomain>(souffle::RamUnsigned("
                           << souffle::RamUnsignedFromString(token, nullptr, 0) << "ULL))";
                        break;
                    case 'i':
                        ss << "souffle::ramBitCast<souffle::RamDomain>(souffle::RamUnsigned("
                           << souffle::ramBitCast<souffle::RamUnsigned>(
                                      souffle::RamSignedFromString(token, nullptr, 0)) << "ULL))";
                        break;
                    default:
                        assert(false && "Evidence codegen only supports primitive ground fields");
                }
                if (i + 1 != tupleFields.size()) {
                    ss << ", ";
                }
            }
            ss << "}}, " << (evi->getValue() ? "true" : "false") << "},\n";
        }

        ss << "};\n";
        mainClass.hooks() << ss.str();
    }

    // identify relations used by each subroutines
    std::multimap<std::string /* stratum_* */, std::string> subroutineUses;

    // generate class for each subroutine
    std::vector<std::pair<std::string, std::string>> subroutineInits;
    for (auto& sub : prog.getSubroutines()) {
        GenClass& gen = db.getClass(convertStratumIdent("Stratum_" + sub.first),
                fs::path(convertStratumIdent("Stratum_" + sub.first)));
        mainClass.addDependency(gen);

        auto accessedRels = accessedRelations(*sub.second);
        auto accessedFunctors = accessedUserDefinedFunctors(*sub.second);

        gen.addInclude("\"souffle/SouffleInterface.h\"");
        gen.addInclude("\"souffle/SignalHandler.h\"");

        GenFunction& constructor = gen.addConstructor(Visibility::Public);

        enum Mode { Reference, Relation };
        std::vector<std::tuple<Mode, std::string /*name*/, std::string /*type*/>> args;
        args.push_back(std::make_tuple(Reference, "symTable", "SymbolTable"));
        args.push_back(std::make_tuple(Reference, "recordTable", "RecordTable"));
        args.push_back(std::make_tuple(Reference, "regexCache", "ConcurrentCache<std::string,std::regex>"));
        args.push_back(std::make_tuple(Reference, "pruneImdtRels", "bool"));
        args.push_back(std::make_tuple(Reference, "performIO", "bool"));
        args.push_back(std::make_tuple(Reference, "signalHandler", "SignalHandler*"));
        args.push_back(std::make_tuple(Reference, "iter", "std::atomic<std::size_t>"));
        args.push_back(std::make_tuple(Reference, "ctr", "std::atomic<RamDomain>"));
        args.push_back(std::make_tuple(Reference, "inputDirectory", "std::string"));
        args.push_back(std::make_tuple(Reference, "outputDirectory", "std::string"));
        for (std::string rel : accessedRels) {
            std::string name = getRelationName(lookup(rel));
            std::string tyname = relationTypes[name];
            args.push_back(std::make_tuple(Relation, name, tyname));
            db.usesDatastructure(gen, tyname);
        }
        for (std::string fn : accessedFunctors) {
            args.push_back(std::make_tuple(Reference, fn, function_ty(fn)));
        }

        for (auto arg : args) {
            Mode kind;
            std::string name, ty;
            std::tie(kind, name, ty) = arg;
            constructor.setNextArg(ty + std::string("&"), name);

            constructor.setNextInitializer(
                    name, (kind == Relation ? std::string("&") : std::string("")) + name);

            gen.addField(ty + (kind == Relation ? "*" : "&"), name, Visibility::Private);
        }
        std::stringstream initStr;
        initStr << join(args, ",", [&](auto& out, const auto arg) {
            Mode kind;
            std::string name, ty;
            std::tie(kind, name, ty) = arg;
            out << (kind == Relation ? "*" : "") << name;
        });
        subroutineInits.push_back(std::make_pair(sub.first, initStr.str()));

        GenFunction& run = gen.addFunction("run", Visibility::Public);
        run.setRetType("void");
        run.setNextArg("[[maybe_unused]] const std::vector<RamDomain>&", "args");
        run.setNextArg("[[maybe_unused]] std::vector<RamDomain>&", "ret");

        bool needLock = false;
        visit(*sub.second, [&](const SubroutineReturn&) { needLock = true; });
        if (needLock) {
            run.body() << "std::mutex lock;\n";
        }
        SubroutineUsingStdRegex = false;
        SubroutineUsingSubstr = false;
        // emit code for subroutine
        currentClass = &gen;
        emitCode(run.body(), *sub.second);
        // issue end of subroutine
        UsingStdRegex |= SubroutineUsingStdRegex;

        if (SubroutineUsingStdRegex) {
            // regex wrapper
            GenFunction& wrapper = gen.addFunction("regex_wrapper", Visibility::Private);
            wrapper.setRetType("inline bool");
            wrapper.setNextArg("const std::string&", "pattern");
            wrapper.setNextArg("const std::string&", "text");
            wrapper.body()
                    << "   bool result = false; \n"
                    << "   try { result = std::regex_match(text, regexCache.getOrCreate(pattern)); } "
                       "catch(...) { "
                       "\n"
                    << "     std::cerr << \"warning: wrong pattern provided for match(\\\"\" << pattern << "
                       "\"\\\",\\\"\" "
                       "<< text << \"\\\").\\n\";\n}\n"
                    << "   return result;\n";
        }

        if (!regexes.empty()) {
            gen.addField("std::vector<std::regex>", "regexes", Visibility::Private);
            std::stringstream rst;
            // we need to collect the patterns first and place each
            // one into the correct slot
            std::vector<std::string> patterns;
            patterns.resize(regexes.size());
            for (const auto& pi : regexes) {
                patterns.at(pi.second) = pi.first;
            }
            rst << "{\n";
            for (const auto& p : patterns) {
                const std::string escaped = escape(p);
                rst << "\tstd::regex(\"" << escaped << "\"),\n";
            }
            rst << "}";

            constructor.setNextInitializer("regexes", rst.str());
            regexes.clear();
        }

        // substring wrapper
        if (SubroutineUsingSubstr) {
            GenFunction& wrapper = gen.addFunction("substr_wrapper", Visibility::Private);
            wrapper.setRetType("inline std::string");
            wrapper.setNextArg("const std::string&", "str");
            wrapper.setNextArg("std::size_t", "idx");
            wrapper.setNextArg("std::size_t", "len");
            wrapper.body() << "std::string result; \n"
                           << "try { result = str.substr(idx,len); } catch(std::out_of_range&) { \n"
                           << "  std::cerr << \"warning: wrong index position provided by substr(\\\"\";\n"
                           << "  std::cerr << str << \"\\\",\" << (int32_t)idx << \",\" << (int32_t)len << "
                              "\") functor.\\n\";\n"
                           << "} return result;\n";
        }
    }

    GenFunction& constructor = mainClass.addConstructor(Visibility::Public);
    constructor.setIsConstructor();

    if (glb.config().has("profile")) {
        mainClass.addField("std::string", "profiling_fname", Visibility::Public);
        constructor.setNextArg("std::string", "pf", std::make_optional("\"profile.log\""));
        constructor.setNextInitializer("profiling_fname", "std::move(pf)");
    }

    // issue symbol table with string constants
    visit(prog, [&](const StringConstant& sc) { convertSymbol2Idx(sc.getConstant()); });
    std::stringstream st;
    if (!symbolMap.empty()) {
        st << "{\n";
        for (const auto& x : symbolIndex) {
            st << "\tR\"_(" << x << ")_\",\n";
        }
        st << "}";
    }
    mainClass.addField("SymbolTableImpl", "symTable", Visibility::Private);
    constructor.setNextInitializer("symTable", st.str());

    // declare record table
    std::stringstream rt;
    rt << "SpecializedRecordTable<0";
    for (std::size_t arity : arities) {
        if (arity > 0) {
            rt << "," << arity;
        }
    }
    rt << ">";
    mainClass.addField(rt.str(), "recordTable", Visibility::Private);
    constructor.setNextInitializer("recordTable", "");

    mainClass.addField("ConcurrentCache<std::string,std::regex>", "regexCache", Visibility::Private);
    constructor.setNextInitializer("regexCache", "");

    if (glb.config().has("profile")) {
        std::size_t numFreq = 0;
        visit(prog, [&](const Statement&) { numFreq++; });
        mainClass.addField("std::size_t", "freqs[" + std::to_string(numFreq) + "]", Visibility::Private);
        constructor.setNextInitializer("freqs", "");
        std::size_t numRead = 0;
        for (auto rel : prog.getRelations()) {
            if (!rel->isTemp()) {
                numRead++;
            }
        }
        mainClass.addField("std::size_t", "reads[" + std::to_string(numRead) + "]", Visibility::Private);
        constructor.setNextInitializer("reads", "");
    }

    for (const auto& f : functors) {
        const std::string& name = f.first;
        mainClass.addField(function_ty(name), name, Visibility::Private);
    }

    int relCtr = 0;
    for (auto rel : prog.getRelations()) {
        // get some table details
        const std::string& datalogName = rel->getName();
        const std::string& cppName = getRelationName(*rel);

        auto relationType =
                Relation::getSynthesiserRelation(*rel, idxAnalysis.getIndexSelection(datalogName));
        const std::string& type = relationType->getTypeName();

        // defining table
        mainClass.addField("Own<" + type + ">", cppName, Visibility::Private);
        constructor.setNextInitializer(cppName, "mk<" + type + ">()");
        if (!rel->isTemp()) {
            std::stringstream ty, init, wrapper_name;
            ty << "souffle::RelationWrapper<" << type << ">";
            wrapper_name << "wrapper_" << cppName;

            auto strLitAry = [](auto&& xs) {
                std::stringstream ss;
                ss << "std::array<const char *," << xs.size() << ">{{"
                   << join(xs, ",", [](auto&& os, auto&& x) { os << '"' << x << '"'; }) << "}}";
                return ss.str();
            };

            auto foundIn = [&](auto&& set) { return contains(set, rel->getName()) ? "true" : "false"; };

            init << relCtr++ << ", *" << cppName << ", *this, \"" << datalogName << "\", "
                 << strLitAry(rel->getAttributeTypes()) << ", " << strLitAry(rel->getAttributeNames()) << ", "
                 << rel->getAuxiliaryArity();
            constructor.body() << "addRelation(\"" << datalogName << "\", wrapper_" << cppName << ", "
                               << foundIn(loadRelations) << ", " << foundIn(storeRelations) << ");\n";

            mainClass.addField(ty.str(), wrapper_name.str(), Visibility::Private);
            constructor.setNextInitializer(wrapper_name.str(), init.str());
        }
    }

    for (auto [name, value] : subroutineInits) {
        std::string clName = convertStratumIdent("Stratum_" + name);
        std::string fName = convertStratumIdent("stratum_" + name);
        mainClass.addField(clName, fName, Visibility::Private);
        constructor.setNextInitializer(fName, value);
    }

    if (glb.config().has("profile")) {
        constructor.body() << "ProfileEventSingleton::instance().setOutputFile(profiling_fname);\n";
    }

    for (const auto& f : functors) {
        const std::string& name = f.first;
        functors_initialize(constructor.body(), name);
    }

    // -- destructor --
    GenFunction& destructor = mainClass.addFunction("~" + classname, Visibility::Public);
    destructor.setIsConstructor();

    // issue state variables for the evaluation
    //
    // Improve compile time by storing the signal handler in one loc instead of
    // emitting thousands of `SignalHandler::instance()`. The volume of calls
    // makes GVN and register alloc very expensive, even if the call is inlined.
    // mainClass.addField("std::string", "inputDirectory", Visibility::Private);
    // mainClass.addField("std::string", "outputDirectory", Visibility::Private);
    mainClass.addField("SignalHandler*", "signalHandler", Visibility::Private, "{SignalHandler::instance()}");
    mainClass.addField("std::atomic<RamDomain>", "ctr", Visibility::Private, "{}");
    mainClass.addField("std::atomic<std::size_t>", "iter", Visibility::Private, "{}");

    GenFunction& runFunction = mainClass.addFunction("runFunction", Visibility::Private);
    runFunction.setRetType("void");
    runFunction.setNextArg("std::string", "inputDirectoryArg");
    runFunction.setNextArg("std::string", "outputDirectoryArg");
    runFunction.setNextArg("bool", "performIOArg");
    runFunction.setNextArg("bool", "pruneImdtRelsArg");

    runFunction.body() << R"_(
    this->inputDirectory  = std::move(inputDirectoryArg);
    this->outputDirectory = std::move(outputDirectoryArg);
    this->performIO       = performIOArg;
    this->pruneImdtRels   = pruneImdtRelsArg;

    // set default threads (in embedded mode)
    // if this is not set, and omp is used, the default omp setting of number of cores is used.
#if defined(_OPENMP)
    if (0 < getNumThreads()) { omp_set_num_threads(static_cast<int>(getNumThreads())); }
#endif

    signalHandler->set();
)_";

    GenFunction& runFunctionInc = mainClass.addFunction("runFunctionInc", Visibility::Private);
    runFunctionInc.setRetType("void");
    runFunctionInc.setNextArg("std::string", "inputDirectoryArg");
    runFunctionInc.setNextArg("std::string", "outputDirectoryArg");
    runFunctionInc.setNextArg("bool", "performIOArg");
    runFunctionInc.setNextArg("bool", "pruneImdtRelsArg");

    runFunctionInc.body() << R"_(
    this->inputDirectory  = std::move(inputDirectoryArg);
    this->outputDirectory = std::move(outputDirectoryArg);
    this->performIO       = performIOArg;
    this->pruneImdtRels   = pruneImdtRelsArg;

    // set default threads (in embedded mode)
    // if this is not set, and omp is used, the default omp setting of number of cores is used.
#if defined(_OPENMP)
    if (0 < getNumThreads()) { omp_set_num_threads(static_cast<int>(getNumThreads())); }
#endif

    signalHandler->set();
)_";

    if (glb.config().has("verbose")) {
        runFunction.body() << "signalHandler->enableLogging();\n";
        runFunctionInc.body() << "signalHandler->enableLogging();\n";
    }

    // add actual program body
    runFunction.body() << "// -- query evaluation --\n";
    runFunctionInc.body() << "// -- query evaluation --\n";
    if (glb.config().has("profile")) {
        runFunction.body() << "ProfileEventSingleton::instance().startTimer();\n"
                           << R"_(ProfileEventSingleton::instance().makeTimeEvent("@time;starttime");)_"
                           << '\n'
                           << "{\n"
                           << R"_(Logger logger("@runtime;", 0);)_" << '\n';
        runFunctionInc.body() << "ProfileEventSingleton::instance().startTimer();\n"
                   << R"_(ProfileEventSingleton::instance().makeTimeEvent("@time;starttime");)_"
                   << '\n'
                   << "{\n"
                   << R"_(Logger logger("@runtime;", 0);)_" << '\n';
        // Store count of relations
        std::size_t relationCount = 0;
        for (auto rel : prog.getRelations()) {
            if (rel->getName()[0] != '@') {
                ++relationCount;
            }
        }
        // Store configuration
        runFunction.body()
                << R"_(ProfileEventSingleton::instance().makeConfigRecord("relationCount", std::to_string()_"
                << relationCount << "));";
        runFunctionInc.body()
                << R"_(ProfileEventSingleton::instance().makeConfigRecord("relationCount", std::to_string()_"
                << relationCount << "));";
    }

    // emit code
    currentClass = &mainClass;
    emitCode(runFunction.body(), prog.getMain());
    emitCode(runFunctionInc.body(), prog.getInc());

    if (glb.config().has("profile")) {
        runFunction.body() << "}\n"
                           << "ProfileEventSingleton::instance().stopTimer();\n"
                           << "dumpFreqs();\n";
        runFunctionInc.body() << "}\n"
                   << "ProfileEventSingleton::instance().stopTimer();\n"
                   << "dumpFreqs();\n";
    }

    // add code printing hint statistics
    runFunction.body() << "\n// -- relation hint statistics --\n";
    runFunctionInc.body() << "\n// -- relation hint statistics --\n";

    if (glb.config().has("verbose")) {
        for (auto rel : prog.getRelations()) {
            auto name = getRelationName(*rel);
            runFunction.body() << "std::cout << \"Statistics for Relation " << name << ":\\n\";\n"
                               << name << "->printStatistics(std::cout);\n"
                               << "std::cout << \"\\n\";\n";
            runFunctionInc.body() << "std::cout << \"Statistics for Relation " << name << ":\\n\";\n"
                   << name << "->printStatistics(std::cout);\n"
                   << "std::cout << \"\\n\";\n";
        }
    }

    runFunction.body() << "signalHandler->reset();\n";
    runFunctionInc.body() << "signalHandler->reset();\n";

    // add methods to run with and without performing IO (mainly for the interface)
    GenFunction& run = mainClass.addFunction("run", Visibility::Public);
    run.setOverride();
    run.setRetType("void");
    run.body() << "runFunction(\"\", \"\", false, false);\n";

    GenFunction& runInc = mainClass.addFunction("runInc", Visibility::Public);
    runInc.setOverride();
    runInc.setRetType("void");
    runInc.body() << "runFunctionInc(\"\", \"\", false, false);\n";

    GenFunction& runAll = mainClass.addFunction("runAll", Visibility::Public);
    runAll.setOverride();
    runAll.setRetType("void");
    runAll.setNextArg("std::string", "inputDirectoryArg", std::make_optional("\"\""));
    runAll.setNextArg("std::string", "outputDirectoryArg", std::make_optional("\"\""));
    runAll.setNextArg("bool", "performIOArg", std::make_optional("true"));
    runAll.setNextArg("bool", "pruneImdtRelsArg", std::make_optional("false"));
    runAll.body() << "runFunction(inputDirectoryArg, outputDirectoryArg, performIOArg, pruneImdtRelsArg);\n";

    GenFunction& runAllInc = mainClass.addFunction("runAllInc", Visibility::Public);
    runAllInc.setOverride();
    runAllInc.setRetType("void");
    runAllInc.setNextArg("std::string", "inputDirectoryArg", std::make_optional("\"\""));
    runAllInc.setNextArg("std::string", "outputDirectoryArg", std::make_optional("\"\""));
    runAllInc.setNextArg("bool", "performIOArg", std::make_optional("true"));
    runAllInc.setNextArg("bool", "pruneImdtRelsArg", std::make_optional("false"));
    runAllInc.body() << "runFunctionInc(inputDirectoryArg, outputDirectoryArg, performIOArg, pruneImdtRelsArg);\n";

    // issue printAll method
    GenFunction& printAll = mainClass.addFunction("printAll", Visibility::Public);
    printAll.setOverride();
    printAll.setRetType("void");
    printAll.setNextArg("[[maybe_unused]] std::string", "outputDirectoryArg", std::make_optional("\"\""));

    // print directives as C++ initializers
    auto printDirectives = [&](std::ostream& o, const std::map<std::string, std::string>& registry) {
        auto cur = registry.begin();
        if (cur == registry.end()) {
            return;
        }
        o << "{{\"" << cur->first << "\",\"" << escape(cur->second) << "\"}";
        ++cur;
        for (; cur != registry.end(); ++cur) {
            o << ",{\"" << cur->first << "\",\"" << escape(cur->second) << "\"}";
        }
        o << '}';
    };

    for (auto store : storeIOs) {
        auto const& directive = store->getDirectives();
        printAll.body() << "try {";
        printAll.body() << "std::map<std::string, std::string> directiveMap(";
        printDirectives(printAll.body(), directive);
        printAll.body() << ");\n";
        printAll.body() << R"_(if (!outputDirectoryArg.empty()) {)_";
        printAll.body() << R"_(directiveMap["output-dir"] = outputDirectoryArg;)_";
        printAll.body() << "}\n";
        printAll.body() << "IOSystem::getInstance().getWriter(";
        printAll.body() << "directiveMap, symTable, recordTable";
        printAll.body() << ")->writeAll(*" << getRelationName(lookup(store->getRelation())) << ");\n";

        printAll.body() << "} catch (std::exception& e) {std::cerr << e.what();exit(1);}\n";
    }

    // issue loadAll method
    GenFunction& loadAll = mainClass.addFunction("loadAll", Visibility::Public);
    loadAll.setOverride();
    loadAll.setRetType("void");
    loadAll.setNextArg("[[maybe_unused]] std::string", "inputDirectoryArg", std::make_optional("\"\""));

    for (auto load : loadIOs) {
        loadAll.body() << "try {";
        loadAll.body() << "std::map<std::string, std::string> directiveMap(";
        printDirectives(loadAll.body(), load->getDirectives());
        loadAll.body() << ");\n";
        loadAll.body() << R"_(if (!inputDirectoryArg.empty()) {)_";
        loadAll.body() << R"_(directiveMap["fact-dir"] = inputDirectoryArg;)_";
        loadAll.body() << "}\n";
        loadAll.body() << "IOSystem::getInstance().getReader(";
        loadAll.body() << "directiveMap, symTable, recordTable";
        loadAll.body() << ")->readAll(*" << getRelationName(lookup(load->getRelation()));
        loadAll.body() << ");\n";
        loadAll.body() << "} catch (std::exception& e) {std::cerr << \"Error loading " << load->getRelation()
                       << " data: \" << e.what() << "
                          "'\\n';\nexit(1);\n}\n";
    }


    // issue loadAllExcept method
    GenFunction& loadAllExcept = mainClass.addFunction("loadAllExcept", Visibility::Public);
    loadAllExcept.setOverride();
    loadAllExcept.setRetType("void");
    loadAllExcept.setNextArg("[[maybe_unused]] std::string", "inputDirectoryArg", std::make_optional("\"\""));

    for (auto load : loadIOs) {
        loadAllExcept.body() << "try {";
        loadAllExcept.body() << "std::map<std::string, std::string> directiveMap(";
        printDirectives(loadAllExcept.body(), load->getDirectives());
        loadAllExcept.body() << ");\n";
        loadAllExcept.body() << R"_(if (!inputDirectoryArg.empty()) {)_";
        loadAllExcept.body() << R"_(directiveMap["fact-dir"] = inputDirectoryArg;)_";
        loadAllExcept.body() << "}\n";
        loadAllExcept.body() << "IOSystem::getInstance().getReader(";
        loadAllExcept.body() << "directiveMap, symTable, recordTable";
        if (glb.config().has("full-only")) {
            // Full-only programs have no incremental deletion filters.
            loadAllExcept.body() << ")->readAll(*" << getRelationName(lookup(load->getRelation()));
        } else {
            loadAllExcept.body() << ")->readAllExcept(*" << getRelationName(lookup(load->getRelation())) << ", *";
            loadAllExcept.body() << getRelationName(lookup("$inc_delta_tuple_delete_" + load->getRelation()));
        }
        loadAllExcept.body() << ");\n";
        loadAllExcept.body() << "} catch (std::exception& e) {std::cerr << \"Error loading with filter" << load->getRelation()
                       << " data: \" << e.what() << "
                          "'\\n';\nexit(1);\n}\n";
    }

    // issue dump methods
    auto dumpRelation = [&](std::ostream& os, const ram::Relation& ramRelation) {
        const auto& relName = getRelationName(ramRelation);
        const auto& name = ramRelation.getName();
        const auto& attributesTypes = ramRelation.getAttributeTypes();

        Json relJson = Json::object{{"arity", static_cast<long long>(attributesTypes.size())},
                {"auxArity", static_cast<long long>(0)},
                {"types", Json::array(attributesTypes.begin(), attributesTypes.end())}};

        Json types = Json::object{{"relation", relJson}};

        os << "try {";
        os << "std::map<std::string, std::string> rwOperation;\n";
        os << "rwOperation[\"IO\"] = \"stdout\";\n";
        os << R"(rwOperation["name"] = ")" << name << "\";\n";
        os << "rwOperation[\"types\"] = ";
        os << "\"" << escapeJSONstring(types.dump()) << "\"";
        os << ";\n";
        os << "IOSystem::getInstance().getWriter(";
        os << "rwOperation, symTable, recordTable";
        os << ")->writeAll(*" << relName << ");\n";
        os << "} catch (std::exception& e) {std::cerr << e.what();exit(1);}\n";
    };

    // dump inputs
    GenFunction& dumpInputs = mainClass.addFunction("dumpInputs", Visibility::Public);
    dumpInputs.setOverride();
    dumpInputs.setRetType("void");
    for (auto load : loadIOs) {
        dumpRelation(dumpInputs.body(), *lookup(load->getRelation()));
    }

    // dump outputs
    GenFunction& dumpOutputs = mainClass.addFunction("dumpOutputs", Visibility::Public);
    dumpOutputs.setOverride();
    dumpOutputs.setRetType("void");
    for (auto store : storeIOs) {
        dumpRelation(dumpOutputs.body(), *lookup(store->getRelation()));
    }

    GenFunction& getSymbolTable = mainClass.addFunction("getSymbolTable", Visibility::Public);
    getSymbolTable.setOverride();
    getSymbolTable.setRetType("SymbolTable&");
    getSymbolTable.body() << "return symTable;\n";

    GenFunction& getRecordTable = mainClass.addFunction("getRecordTable", Visibility::Public);
    getRecordTable.setOverride();
    getRecordTable.setRetType("RecordTable&");
    getRecordTable.body() << "return recordTable;\n";

    GenFunction& setNumThreads = mainClass.addFunction("setNumThreads", Visibility::Public);
    setNumThreads.setRetType("void");
    setNumThreads.setNextArg("std::size_t", "numThreadsValue");

    setNumThreads.body() << "SouffleProgram::setNumThreads(numThreadsValue);\n";
    setNumThreads.body() << "symTable.setNumLanes(getNumThreads());\n";
    setNumThreads.body() << "recordTable.setNumLanes(getNumThreads());\n";
    setNumThreads.body() << "regexCache.setNumLanes(getNumThreads());\n";

    if (!prog.getSubroutines().empty()) {
        // generate subroutine adapter
        GenFunction& executeSubroutine = mainClass.addFunction("executeSubroutine", Visibility::Public);
        executeSubroutine.setRetType("void");
        executeSubroutine.setOverride();
        executeSubroutine.setNextArg("std::string", "name");
        executeSubroutine.setNextArg("const std::vector<RamDomain>&", "args");
        executeSubroutine.setNextArg("std::vector<RamDomain>&", "ret");

        for (auto& sub : prog.getSubroutines()) {
            executeSubroutine.body() << "if (name == \"" << sub.first << "\") {\n"
                                     << convertStratumIdent("stratum_" + sub.first) << ".run(args, ret);\n"
                                     << "return;"
                                     << "}\n";
        }
        executeSubroutine.body() << "fatal((\"unknown subroutine \" + name).c_str());\n";
    }

    // dumpFreqs method
    //  Frequency counts must be emitted after subroutines otherwise lookup tables
    //  are not populated.
    if (glb.config().has("profile")) {
        GenFunction& dumpFreqs = mainClass.addFunction("dumpFreqs", Visibility::Private);
        dumpFreqs.setRetType("void");

        for (auto const& cur : idxMap) {
            dumpFreqs.body() << "\tProfileEventSingleton::instance().makeQuantityEvent(R\"_(" << cur.first
                             << ")_\", freqs[" << cur.second << "],0);\n";
        }
        for (auto const& cur : neIdxMap) {
            dumpFreqs.body() << "\tProfileEventSingleton::instance().makeQuantityEvent(R\"_(@relation-reads;"
                             << cur.first << ")_\", reads[" << cur.second << "],0);\n";
        }
    }

    GenClass& factory = db.getClass("factory_" + classname, fs::path("factory_" + classname));
    factory.addInclude("\"souffle/SouffleInterface.h\"");
    factory.addDependency(mainClass, true);
    factory.inherits("souffle::ProgramFactory");
    GenFunction& newInstance = factory.addFunction("newInstance", Visibility::Public);
    newInstance.setRetType("souffle::SouffleProgram*");
    newInstance.body() << "return new " << db.getNS() << "::" << classname << "();\n";
    GenFunction& factoryConstructor = factory.addConstructor(Visibility::Public);
    factoryConstructor.setNextInitializer("souffle::ProgramFactory", "\"" + id + "\"");

    std::ostream& hook = mainClass.hooks();
    std::ostream& factory_hook = factory.hooks();
    DetOptMeta detMeta;
    if (newAstProgram) {
        detMeta = buildDetOptMeta(*newAstProgram, glb);
    }

    // hidden hooks
    hook << "namespace souffle {\n";
    hook << "SouffleProgram *newInstance_" << id << "(){return new " << db.getNS() << "::" << classname
         << ";}\n";
    hook << "SymbolTable *getST_" << id << "(SouffleProgram *p){return &reinterpret_cast<" << db.getNS(false)
         << "::" << classname << "*>(p)->getSymbolTable();}\n";

    hook << "} // namespace souffle\n";

    factory_hook << "namespace souffle {\n";
    factory_hook << "\n#ifdef __EMBEDDED_SOUFFLE__\n";
    factory_hook << "extern \"C\" {\n";
    factory_hook << db.getNS(false) << "::factory_" << classname << " __factory_" << classname
                 << "_instance;\n";
    factory_hook << "}\n";
    factory_hook << "#endif\n";
    factory_hook << "} // namespace souffle\n";

    hook << "\n#ifndef __EMBEDDED_SOUFFLE__\n";
    hook << "#include \"souffle/CompiledOptions.h\"\n";
    auto emitStringVector = [&](const std::string& name, const std::vector<std::string>& values) {
        hook << "static const std::vector<std::string> " << name << " = {";
        for (std::size_t i = 0; i < values.size(); ++i) {
            hook << "\"" << values[i] << "\"";
            if (i + 1 != values.size()) {
                hook << ",";
            }
        }
        hook << "};\n";
    };
    auto emitSizeVector = [&](const std::string& name, const std::vector<std::size_t>& values) {
        hook << "static const std::vector<std::size_t> " << name << " = {";
        for (std::size_t i = 0; i < values.size(); ++i) {
            hook << values[i];
            if (i + 1 != values.size()) {
                hook << ",";
            }
        }
        hook << "};\n";
    };
    auto emitIntVector = [&](const std::string& name, const std::vector<int>& values) {
        hook << "static const std::vector<int> " << name << " = {";
        for (std::size_t i = 0; i < values.size(); ++i) {
            hook << values[i];
            if (i + 1 != values.size()) {
                hook << ",";
            }
        }
        hook << "};\n";
    };
    auto emitSccSucc = [&](const std::string& name, const std::vector<std::vector<std::size_t>>& values) {
        hook << "static const std::vector<std::vector<std::size_t>> " << name << " = {";
        for (std::size_t i = 0; i < values.size(); ++i) {
            hook << "{";
            for (std::size_t j = 0; j < values[i].size(); ++j) {
                hook << values[i][j];
                if (j + 1 != values[i].size()) {
                    hook << ",";
                }
            }
            hook << "}";
            if (i + 1 != values.size()) {
                hook << ",";
            }
        }
        hook << "};\n";
    };
    emitStringVector("det_rel_names", detMeta.relNames);
    emitSizeVector("det_rel_to_scc", detMeta.relToScc);
    emitSccSucc("det_scc_succ", detMeta.sccSucc);
    emitSizeVector("det_scc_topo", detMeta.sccTopo);
    emitIntVector("det_rule_seed", detMeta.ruleSeed);

    hook << "int main(int argc, char** argv)\n{\n";
    hook << "try{\n";

    // parse arguments
    hook << "souffle::CmdOptions opt(";
    hook << "R\"(" << glb.config().get("") << ")\",\n";
    if (glb.config().has("fact-dir")) {
        hook << "R\"(" << glb.config().get("fact-dir") << ")\",\n";
    } else {
        hook << "R\"()\",\n";
    }
    if (glb.config().has("output-dir")) {
        hook << "R\"(" << glb.config().get("output-dir") << ")\",\n";
    } else {
        hook << "R\"()\",\n";
    }
    if (glb.config().has("profile")) {
        hook << "true,\n";
        hook << "R\"(" << glb.config().get("profile") << ")\",\n";
    } else {
        hook << "false,\n";
        hook << "R\"()\",\n";
    }
    hook << std::stoi(glb.config().get("jobs"));
    hook << ");\n";
    hook << "opt.setExecutionCapabilities("
         << (glb.config().has("inc-only") ? "false" : "true") << ", "
         << (glb.config().has("full-only") ? "false" : "true") << ", "
         << (glb.config().has("runtime-online") ? "true" : "false") << ");\n";
    hook << "opt.setRewriteDefaults("
         << (glb.config().has("rewrite") ? "true" : "false") << ", "
         << (glb.config().has("explicit-rewrite") ? "true" : "false") << ", "
         << (glb.config().has("implicit-rewrite") ? "true" : "false") << ");\n";
    hook << "opt.setDerivationOnly(" << (glb.config().has("derv-only") ? "true" : "false") << ");\n";
    hook << "opt.setAndInputRedundancyEnabled("
         << (glb.config().has("and-input-redundancy") ? "true" : "false") << ");\n";
    hook << "opt.setLiftedWmcEnabled(" << (glb.config().has("lifted-wmc") ? "true" : "false") << ");\n";
    hook << "opt.setLiftedWmcThreshold(" << glb.config().get("lifted-threshold") << "ULL);\n";
    hook << "opt.setLogFileName(R\"("
         << (glb.config().has("logfile") ? glb.config().get("logfile") : std::string("log.txt"))
         << ")\");\n";
    hook << "opt.setIncrementalMode(\"" << glb.config().get("setmode") << "\");\n";
    hook << "opt.setDumpJsonEnabled(" << (glb.config().has("dumpjson") ? "true" : "false") << ");\n";
    hook << "opt.setDumpJsonBeforeGraphEnabled("
         << (glb.config().has("dumpjson-before-graph") ? "true" : "false") << ");\n";
    hook << "opt.setDumpJsonBeforePruneEnabled("
         << (glb.config().has("dumpjson-before-prune") ? "true" : "false") << ");\n";
    hook << "opt.setDumpDotEnabled(" << (glb.config().has("dumpdot") ? "true" : "false") << ");\n";
    hook << "opt.setDumpStatEnabled(" << (glb.config().has("dumpstat") ? "true" : "false") << ");\n";
    hook << "opt.setDumpAndRedundancyEnabled("
         << (glb.config().has("dump-and-redundancy") ? "true" : "false") << ");\n";
    hook << "opt.setVerboseEnabled(" << (glb.config().has("verbose") ? "true" : "false") << ");\n";
    hook << "opt.setProfileStageToken(\"dred\", "
         << (glb.config().has("dred-profile") ? "true" : "false") << ");\n";
    hook << "opt.setProfileStageToken(\"inc\", "
         << (glb.config().has("inc-profile") ? "true" : "false") << ");\n";
    hook << "opt.setProfileStageToken(\"fc\", "
         << (glb.config().has("fc-profile") ? "true" : "false") << ");\n";
    hook << "opt.setProfileStageToken(\"wmc\", "
         << (glb.config().has("profile-wmc") ? "true" : "false") << ");\n";
    hook << "opt.setProfileStageToken(\"inc-delete\", "
         << (glb.config().has("profile-inc-delete") ? "true" : "false") << ");\n";
    hook << "opt.setProfileStageToken(\"inc-regional\", "
         << (glb.config().has("profile-inc-regional") ? "true" : "false") << ");\n";
    hook << "opt.setProfileStageToken(\"dep-graph\", "
         << (glb.config().has("profile-dep-graph") ? "true" : "false") << ");\n";
    if (glb.config().has("inc-reorder-policy")) {
        hook << "opt.setIncReorderPolicy(R\"(" << glb.config().get("inc-reorder-policy") << ")\");\n";
    }
    if (glb.config().has("inc-reorder-auto-gap")) {
        hook << "opt.setIncReorderAutoGap("
             << glb.config().get("inc-reorder-auto-gap") << ");\n";
    }
    if (glb.config().has("inc-reorder-work-threshold")) {
        hook << "opt.setIncReorderWorkThreshold("
             << glb.config().get("inc-reorder-work-threshold") << ");\n";
    }
    hook << "opt.setIncReorderCountDeadEnabled("
         << (glb.config().has("inc-reorder-count-dead") ? "true" : "false") << ");\n";
    hook << "opt.setIncReorderAllowLargeEnabled("
         << (glb.config().has("inc-reorder-allow-large") ? "true" : "false") << ");\n";

    hook << "if (!opt.parse(argc,argv)) return opt.isHelpRequested() ? 0 : 1;\n";
    hook << "setFunctionTimerOutputEnabled(opt.isVerboseEnabled());\n";
    hook << "detOptEnabled = true;\n";
    hook << "dredProfileEnabled = opt.isDredProfileEnabled();\n";
    hook << "incProfileEnabled = opt.isIncProfileEnabled();\n";
    hook << "incRegionalProfileEnabled = opt.isIncRegionalProfileEnabled();\n";
    hook << "depGraphProfileEnabled = opt.isDepGraphProfileEnabled();\n";
    hook << "incReorderPolicy = opt.getIncReorderPolicy();\n";
    hook << "incReorderAutoGap = opt.getIncReorderAutoGap();\n";
    hook << "incReorderWorkThreshold = opt.getIncReorderWorkThreshold();\n";
    hook << "incReorderCountDead = opt.isIncReorderCountDeadEnabled();\n";
    hook << "incReorderAllowLarge = opt.isIncReorderAllowLargeEnabled();\n";

    if (!db.getNS(false).empty()) {
        hook << db.getNS(false) << "::";
    }
    if (glb.config().has("profile")) {
        hook << classname + " obj(opt.getProfileName());\n";
    } else {
        hook << classname + " obj;\n";
    }

    hook << "#if defined(_OPENMP) \n";
    hook << "obj.setNumThreads(opt.getNumJobs());\n";
    hook << "\n#endif\n";

    if (glb.config().has("profile")) {
        hook << R"_(souffle::ProfileEventSingleton::instance().makeConfigRecord("", opt.getSourceFileName());)_"
             << '\n';
        hook << R"_(souffle::ProfileEventSingleton::instance().makeConfigRecord("fact-dir", opt.getInputFileDir());)_"
             << '\n';
        hook << R"_(souffle::ProfileEventSingleton::instance().makeConfigRecord("jobs", std::to_string(opt.getNumJobs()));)_"
             << '\n';
        hook << R"_(souffle::ProfileEventSingleton::instance().makeConfigRecord("output-dir", opt.getOutputFileDir());)_"
             << '\n';
        hook << R"_(souffle::ProfileEventSingleton::instance().makeConfigRecord("version", ")_"
             << glb.config().get("version") << R"_(");)_" << '\n';
    }
    hook << "Debugger& debugger = Debugger::getInstance();\n";
    hook << "const bool deferDebuggerReportSetup = !debugger.isAutoDumpEnabled();\n";
    hook << "std::string reportFileName;\n";
    hook << "std::string reportFile;\n";
    hook << "std::string reportTerminationFile;\n";
    hook << "auto ensureDebuggerReportPath = [&]() {\n";
    hook << "    if (!reportFile.empty()) return;\n";
    hook << "    std::string logBase = basenameFromPath(opt.getLogFileName());\n";
    hook << "    reportFileName = generateFilename(logBase, \".json\");\n";
    hook << "    reportFile = souffle::problog::makeOutputPath(opt, reportFileName);\n";
    hook << "    reportTerminationFile = reportFile + \".termination\";\n";
    hook << "    debugger.setReportOutputFile(reportFile);\n";
    hook << "    souffle::SignalHandler::instance()->setTerminationStatusFile(reportTerminationFile);\n";
    hook << "};\n";
    hook << "if (!deferDebuggerReportSetup) {\n";
    hook << "    ensureDebuggerReportPath();\n";
    hook << "}\n";
    hook << "debugger.setRunStatus(\"running\");\n";
    hook << "souffle::SignalHandler::instance()->set();\n";
    hook << "debugger.startTurn(opt.isOnlineExecution() ? \"DEFAULT\" : \"EXACT\");\n";
    hook << "auto parseProbFast = [](const std::string& line, double& probOut) {\n";
    hook << "    const char* p = line.c_str();\n";
    hook << "    while (*p != '\\0' && std::isspace(static_cast<unsigned char>(*p))) { ++p; }\n";
    hook << "    if (*p == '\\0') {\n";
    hook << "        return false;\n";
    hook << "    }\n";
    hook << "    errno = 0;\n";
    hook << "    char* end = nullptr;\n";
    hook << "    double parsed = std::strtod(p, &end);\n";
    hook << "    if (p == end || errno == ERANGE || parsed < 0.0 || parsed > 1.0) {\n";
    hook << "        return false;\n";
    hook << "    }\n";
    hook << "    probOut = parsed;\n";
    hook << "    return true;\n";
    hook << "};\n";
    hook << "auto parseFactFieldsFast = [](const std::string& line, std::vector<souffle::RamDomain>& fields) {\n";
    hook << "    fields.clear();\n";
    hook << "    const char* p = line.c_str();\n";
    hook << "    while (*p != '\\0') {\n";
    hook << "        while (*p != '\\0' && std::isspace(static_cast<unsigned char>(*p))) { ++p; }\n";
    hook << "        if (*p == '\\0') {\n";
    hook << "            break;\n";
    hook << "        }\n";
    hook << "        errno = 0;\n";
    hook << "        char* end = nullptr;\n";
    hook << "        long long value = std::strtoll(p, &end, 10);\n";
    hook << "        if (p == end || errno == ERANGE) {\n";
    hook << "            break;\n";
    hook << "        }\n";
    hook << "        if (value < static_cast<long long>(std::numeric_limits<souffle::RamDomain>::min()) ||\n";
    hook << "                value > static_cast<long long>(std::numeric_limits<souffle::RamDomain>::max())) {\n";
    hook << "            break;\n";
    hook << "        }\n";
    hook << "        fields.push_back(static_cast<souffle::RamDomain>(value));\n";
    hook << "        p = end;\n";
    hook << "    }\n";
    hook << "};\n";
    hook << "try {\n";
    hook << "if (detOptEnabled) {\n";
    hook << "auto* detStage = debugger.startStage(opt.isOnlineExecution() ? StageKind::IO_LOAD_FULL : StageKind::IO_LOAD);\n";
    hook << "auto detNowMs = [](auto start) {\n";
    hook << "    return std::chrono::duration_cast<std::chrono::milliseconds>(\n";
    hook << "            std::chrono::steady_clock::now() - start).count();\n";
    hook << "};\n";
    hook << "{\n";
    hook << "auto preStart = std::chrono::steady_clock::now();\n";
    hook << "fact_prob.clear();\n";
    hook << "relationHasProbFact.clear();\n";
    for (auto input : loadIOs) {
        auto rel = input->getRelation();
        const auto& relationTypeName = relationTypes[getRelationName(*lookup(rel))];
        const auto qualifiedRelationValueTypeName = "souffle::" + relationTypeName;
        const auto& directives = input->getDirectives();
        hook << "{\n";
        hook << "std::string rel = \"" << rel << "\";\n";
        hook << "std::map<std::string, std::string> directiveMap(";
        printDirectives(hook, directives);
        hook << ");\n";
        hook << "if (!opt.getInputFileDir().empty()) {\n";
        hook << "    directiveMap[\"fact-dir\"] = opt.getInputFileDir();\n";
        hook << "}\n";
        hook << "auto toFileRel = [&](std::string r) {\n"
             << "  if (r.rfind(\"@magic.\", 0) == 0) return r; \n"
             << "  if (r.rfind(\"@neglabel.\", 0) == 0) return r; \n"
             << "\n"
             << "  for (;;) {\n"
             << "    bool changed = false;\n"
             << "    auto strip = [&](const std::string& p) {\n"
             << "      if (r.rfind(p, 0) == 0) { r = r.substr(p.size()); changed = true; }\n"
             << "    };\n"
             << "    strip(\"@split_in.\");\n"
             << "    strip(\"@interm_in.\");\n"
             << "    strip(\"@interm_out.\");\n"
             << "    if (r.rfind(\"@poscopy_\", 0) == 0) {\n"
             << "      auto dot = r.find('.');\n"
             << "      if (dot != std::string::npos) { r = r.substr(dot + 1); changed = true; }\n"
             << "    }\n"
             << "    if (!changed) break;\n"
             << "  }\n"
             << "\n"
             << "  if (!r.empty()) {\n"
             << "    auto dot = r.rfind('.');\n"
             << "    if (dot != std::string::npos) {\n"
             << "      auto last = r.substr(dot + 1);\n"
             << "      if (last.size() >= 2 && last.front() == '{' && last.back() == '}') {\n"
             << "        bool ok = true;\n"
             << "        for (size_t i = 1; i + 1 < last.size(); ++i) {\n"
             << "          if (last[i] != 'b' && last[i] != 'f') { ok = false; break; }\n"
             << "        }\n"
             << "        if (ok) r = r.substr(0, dot);\n"
             << "      }\n"
             << "    }\n"
             << "  }\n"
             << "  return r;\n"
             << "};\n";
        hook << "std::string fileRel = toFileRel(rel);\n";
        hook << "directiveMap[\"name\"] = fileRel;\n";
        hook << "directiveMap[\"filename\"] = fileRel + \".facts\";\n";
        hook << "relationHasProbFact[rel] = false;\n";
        hook << "if (opt.isVerboseEnabled()) std::cout << \"reading: \" << opt.getInputFileDir() << \"/\" << fileRel << \".facts and \" << opt.getInputFileDir() << \"/\" << fileRel << \".prob\" << std::endl;\n";
        hook << "std::ifstream factFile(opt.getInputFileDir() + \"/\" + fileRel + \".facts\");";
        hook << "std::ifstream probFile(opt.getInputFileDir() + \"/\" + fileRel + \".prob\");\n";
        hook << "if (!factFile.is_open()) {\n";
        hook << "    std::cerr << \"Missing facts file for relation: \" << fileRel << \" (ioRel=\" << rel << \")\" << std::endl;\n";
        hook << "    assert(false && \"facts file not found\");\n";
        hook << "}\n";
        hook << "bool probExists = probFile.is_open();\n";
        hook << "if (!probExists) {\n";
        hook << "    std::cerr << \"[Warning] Missing prob file for relation: \" << fileRel << \" (ioRel=\" << rel << \"), defaulting probabilities to 1.0\" << std::endl;\n";
        hook << "}\n";
        hook << "std::string probLine;\n";
        hook << qualifiedRelationValueTypeName << " parsedFacts;\n";
        hook << "struct ProbTrackingSink {\n";
        hook << "    " << qualifiedRelationValueTypeName << "& relation;\n";
        hook << "    const std::string& relationName;\n";
        hook << "    const std::string& fileRelation;\n";
        hook << "    bool probExists;\n";
        hook << "    std::ifstream& probFile;\n";
        hook << "    std::string& probLine;\n";
        hook << "    std::unordered_map<UntypedTuple, double>& factProb;\n";
        hook << "    std::unordered_map<std::string, bool>& relationHasProb;\n";
        hook << "    void insert(const souffle::RamDomain* ramDomain) {\n";
        hook << "        relation.insert(ramDomain);\n";
        hook << "        double prob = 1.0;\n";
        hook << "        if (probExists && std::getline(probFile, probLine)) {\n";
        hook << "            std::istringstream ps(probLine);\n";
        hook << "            if (!(ps >> prob)) {\n";
        hook << "                std::cerr << \"[Warning] Invalid probability in \" << fileRelation\n";
        hook << "                          << \".prob, defaulting to 1.0\" << std::endl;\n";
        hook << "                prob = 1.0;\n";
        hook << "            }\n";
        hook << "        }\n";
        hook << "        std::vector<souffle::RamDomain> tupleFields(ramDomain, ramDomain + "
             << qualifiedRelationValueTypeName << "::Arity);\n";
        hook << "        factProb.insert_or_assign(UntypedTuple{relationName, std::move(tupleFields)}, prob);\n";
        hook << "        if (prob != 1.0) {\n";
        hook << "            relationHasProb[relationName] = true;\n";
        hook << "        }\n";
        hook << "    }\n";
        hook << "} probSink{parsedFacts, rel, fileRel, probExists, probFile, probLine, fact_prob, relationHasProbFact};\n";
        hook << "auto reader = souffle::IOSystem::getInstance().getReader(\n";
        hook << "        directiveMap, obj.getSymbolTable(), obj.getRecordTable());\n";
        hook << "reader->readAll(probSink);\n";
        hook << "}\n";
    }
    hook << "auto preMs = detNowMs(preStart);\n";
    hook << "if (opt.isVerboseEnabled()) std::cout << \"[det-analysis] prepass took \" << preMs << \" ms\" << std::endl;\n";
    hook << "if (detStage) detStage->logMessage(Level::INFO, \"prepass_ms=\" + std::to_string(preMs));\n";
    hook << "}\n";
    hook << "{\n";
    hook << "auto analyzeStart = std::chrono::steady_clock::now();\n";
    hook << "std::vector<bool> probScc(det_scc_succ.size(), false);\n";
    hook << "for (std::size_t i = 0; i < det_rel_names.size(); ++i) {\n";
    hook << "    if (det_rule_seed[i]) { probScc[det_rel_to_scc[i]] = true; }\n";
    hook << "    auto it = relationHasProbFact.find(det_rel_names[i]);\n";
    hook << "    if (it != relationHasProbFact.end() && it->second) {\n";
    hook << "        probScc[det_rel_to_scc[i]] = true;\n";
    hook << "    }\n";
    hook << "}\n";
    hook << "for (auto sccId : det_scc_topo) {\n";
    hook << "    if (!probScc[sccId]) continue;\n";
    hook << "    for (auto succ : det_scc_succ[sccId]) {\n";
    hook << "        probScc[succ] = true;\n";
    hook << "    }\n";
    hook << "}\n";
    hook << "std::vector<bool> relIsDet(det_rel_names.size(), false);\n";
    hook << "for (std::size_t i = 0; i < det_rel_names.size(); ++i) {\n";
    hook << "    relIsDet[i] = !probScc[det_rel_to_scc[i]];\n";
    hook << "}\n";
    hook << "relationIsDet.clear();\n";
    hook << "relationIsDet.reserve(det_rel_names.size());\n";
    hook << "for (std::size_t i = 0; i < det_rel_names.size(); ++i) {\n";
    hook << "    relationIsDet[det_rel_names[i]] = relIsDet[i];\n";
    hook << "}\n";
    hook << "auto analyzeMs = detNowMs(analyzeStart);\n";
    hook << "if (opt.isVerboseEnabled()) std::cout << \"[det-analysis] analyze took \" << analyzeMs << \" ms\" << std::endl;\n";
    hook << "if (detStage) detStage->logMessage(Level::INFO, \"analyze_ms=\" + std::to_string(analyzeMs));\n";
    hook << "\n";
    hook << "if (opt.isDumpStatEnabled()) {\n";
    hook << "auto dumpStart = std::chrono::steady_clock::now();\n";
    hook << "std::string detPath = souffle::problog::makeOutputPath(opt, \"det-relations.txt\");\n";
    hook << "std::ofstream detOut(detPath);\n";
    hook << "detOut << \"relation\\tscc\\trule_seed\\tfact_seed\\tprob_scc\\tdet\\n\";\n";
    hook << "for (std::size_t i = 0; i < det_rel_names.size(); ++i) {\n";
    hook << "    bool factSeed = false;\n";
    hook << "    auto it = relationHasProbFact.find(det_rel_names[i]);\n";
    hook << "    if (it != relationHasProbFact.end() && it->second) { factSeed = true; }\n";
    hook << "    std::size_t sccId = det_rel_to_scc[i];\n";
    hook << "    detOut << det_rel_names[i] << \"\\t\" << sccId << \"\\t\" << det_rule_seed[i]\n";
    hook << "           << \"\\t\" << (factSeed ? 1 : 0) << \"\\t\" << (probScc[sccId] ? 1 : 0)\n";
    hook << "           << \"\\t\" << (relIsDet[i] ? 1 : 0) << \"\\n\";\n";
    hook << "}\n";
    hook << "std::string detSccPath = souffle::problog::makeOutputPath(opt, \"det-scc.txt\");\n";
    hook << "std::ofstream detSccOut(detSccPath);\n";
    hook << "detSccOut << \"scc\\tprob\\trelations\\n\";\n";
    hook << "for (std::size_t sccId = 0; sccId < det_scc_succ.size(); ++sccId) {\n";
    hook << "    detSccOut << sccId << \"\\t\" << (probScc[sccId] ? 1 : 0) << \"\\t\";\n";
    hook << "    bool first = true;\n";
    hook << "    for (std::size_t i = 0; i < det_rel_names.size(); ++i) {\n";
    hook << "        if (det_rel_to_scc[i] != sccId) continue;\n";
    hook << "        if (!first) detSccOut << \",\";\n";
    hook << "        detSccOut << det_rel_names[i];\n";
    hook << "        first = false;\n";
    hook << "    }\n";
    hook << "    detSccOut << \"\\n\";\n";
    hook << "}\n";
    hook << "auto dumpMs = detNowMs(dumpStart);\n";
    hook << "if (opt.isVerboseEnabled()) std::cout << \"[det-analysis] dump took \" << dumpMs << \" ms\" << std::endl;\n";
    hook << "if (detStage) detStage->logMessage(Level::INFO, \"dump_ms=\" + std::to_string(dumpMs));\n";
    hook << "}\n";
    hook << "}\n";
    hook << "debugger.endStage();\n";
    hook << "}\n";
    hook << "debugger.startStage(opt.isOnlineExecution() ? StageKind::SEMINAIVE_FULL : StageKind::SEMINAIVE);\n";
    hook << "obj.runAll(opt.getInputFileDir(), opt.getOutputFileDir());\n";
    hook << "debugger.endStage();\n";
    hook << "souffle::SignalHandler::instance()->setTerminationStatusFile(reportTerminationFile);\n";
    hook << "souffle::SignalHandler::instance()->set();\n";
    hook << "if (!detOptEnabled) {\n";
    hook << "debugger.startStage(opt.isOnlineExecution() ? StageKind::IO_LOAD_FULL : StageKind::IO_LOAD);\n";
    hook << "{\n";
    hook << "FunctionTimer timer(\"Reading fact probability from \" + opt.getInputFileDir());\n";
    for (auto input : loadIOs) {
        auto rel = input->getRelation();
        auto relArity = lookup(rel)->getArity();
        hook << "{\n";

        hook << "std::string ioRel = \"" << rel << "\";\n";

         hook << "auto toFileRel = [&](std::string r) {\n"
              << "  if (r.rfind(\"@magic.\", 0) == 0) return r; \n"
              << "  if (r.rfind(\"@neglabel.\", 0) == 0) return r; \n"
              << "\n"
              << "  for (;;) {\n"
              << "    bool changed = false;\n"
              << "    auto strip = [&](const std::string& p) {\n"
              << "      if (r.rfind(p, 0) == 0) { r = r.substr(p.size()); changed = true; }\n"
              << "    };\n"
              << "    strip(\"@split_in.\");\n"
              << "    strip(\"@interm_in.\");\n"
              << "    strip(\"@interm_out.\");\n"
              << "    if (r.rfind(\"@poscopy_\", 0) == 0) {\n"
              << "      auto dot = r.find('.');\n"
              << "      if (dot != std::string::npos) { r = r.substr(dot + 1); changed = true; }\n"
              << "    }\n"
              << "    if (!changed) break;\n"
              << "  }\n"
              << "\n"
              << "  if (!r.empty()) {\n"
              << "    auto dot = r.rfind('.');\n"
              << "    if (dot != std::string::npos) {\n"
              << "      auto last = r.substr(dot + 1);\n"
              << "      if (last.size() >= 2 && last.front() == '{' && last.back() == '}') {\n"
              << "        bool ok = true;\n"
              << "        for (size_t i = 1; i + 1 < last.size(); ++i) {\n"
              << "          if (last[i] != 'b' && last[i] != 'f') { ok = false; break; }\n"
              << "        }\n"
              << "        if (ok) r = r.substr(0, dot);\n"
              << "      }\n"
              << "    }\n"
              << "  }\n"
              << "  return r;\n"
              << "};\n"
              << "std::string fileRel = toFileRel(ioRel);\n";

        hook << "if (opt.isVerboseEnabled()) {\n";
        hook << "std::cout << \"reading: \" << opt.getInputFileDir() << \"/\" << fileRel"
             << " << \".facts and \" << opt.getInputFileDir() << \"/\" << fileRel"
             << " << \".prob\" << std::endl;\n";
        hook << "}\n";

        hook << "std::ifstream factFile(opt.getInputFileDir() + \"/\" + fileRel + \".facts\");\n";
        hook << "std::ifstream probFile(opt.getInputFileDir() + \"/\" + fileRel + \".prob\");\n";

        hook << "if (!factFile.is_open()) {\n";
        hook << "  std::cerr << \"Missing facts file for relation: \" << fileRel"
             << "            << \" (ioRel=\" << ioRel << \")\" << std::endl;\n";
        hook << "  assert(false && \"facts file not found\");\n";
        hook << "}\n";

        hook << "bool probExists = probFile.is_open();\n";
        hook << "if (!probExists) {\n";
        hook << "  std::cerr << \"[Warning] Missing prob file for relation: \" << fileRel"
             << "            << \" (ioRel=\" << ioRel << \")\""
             << "            << \", defaulting probabilities to 1.0\" << std::endl;\n";
        hook << "}\n";

        hook << "std::string factLine, probLine;\n";
        hook << "std::vector<souffle::RamDomain> fields;\n";
        hook << "fields.reserve(" << relArity << ");\n";
        hook << "while (std::getline(factFile, factLine)) {\n";
        hook << "  double prob = 1.0;\n";
        hook << "  if (probExists && std::getline(probFile, probLine)) {\n";
        hook << "    if (!parseProbFast(probLine, prob)) prob = 1.0;\n";
        hook << "  }\n";
        hook << "  parseFactFieldsFast(factLine, fields);\n";
        hook << "  UntypedTuple tuple{ioRel, fields};\n";
        hook << "  fact_prob.insert_or_assign(std::move(tuple), prob);\n";
        hook << "}\n";

        hook << "}\n";
    }
    hook << "}\n";
    hook << "debugger.endStage();\n";
    hook << "}\n";
    db.addGlobalInclude("\"souffle/problog/Atom.h\"");
    db.addGlobalInclude("\"souffle/problog/Rule.h\"");
    db.addGlobalInclude("\"souffle/problog/RuleManager.h\"");
    db.addGlobalInclude("\"souffle/problog/Query.h\"");
    db.addGlobalInclude("\"souffle/problog/QueryManager.h\"");
    db.addGlobalInclude("\"souffle/problog/Pipeline.h\"");
    db.addGlobalInclude("\"souffle/problog/debug/Debugger.h\"");
    // synthesize rules
    // Rule synthesis currently uses the instance emitter state.
    hook << "debugger.startStage(opt.isOnlineExecution() ? StageKind::CONSTRUCT_RULE_FULL : StageKind::CONSTRUCT_RULE);\n";
    emitRules(hook);
    hook << "debugger.endStage();\n";
    // synthesize forward compilation

    emitProblogPipeline(hook);

    hook << "ensureDebuggerReportPath();\n";
    hook << "debugger.setRunStatus(\"completed\");\n";
    hook << "debugger.dumpReportJsonToFile();\n";
    hook << "if (opt.isVerboseEnabled()) {\n";
    hook << "std::cout << \"[pipeline] debugger log: \" << reportFileName << std::endl;\n";
    hook << "}\n";
    hook << "souffle::SignalHandler::instance()->reset();\n";
    hook << "// debugger.printReport(std::cout);\n";
    // add online incremental&interactive computation


    hook << "} catch (std::exception& e) { ensureDebuggerReportPath(); debugger.setRunStatus(\"exception\"); debugger.dumpReportJsonToFile(); souffle::SignalHandler::instance()->reset(); std::cerr << \"Problog calc failed\" << e.what() << std::endl; return 1;}\n";
    // }


    hook << "return 0;\n";
    hook << "} catch(std::exception &e) { souffle::SignalHandler::instance()->error(e.what());}\n";
    hook << "}\n";
    hook << "#endif\n";
}

}  // namespace souffle::synthesiser
