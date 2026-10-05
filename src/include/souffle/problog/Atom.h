#ifndef ATOM_H
#define ATOM_H

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <variant>
#include <memory>
#include <sstream>
#include <map>
#include "souffle/RamTypes.h"
#include "souffle/SymbolTable.h"

namespace souffle::problog {
void setActiveSymbolTable(SymbolTable* symbolTable);
SymbolTable* getActiveSymbolTable();
}

struct IntegerField {
    int value;
};

struct FloatField {
    double value;
};

struct StringField {
    std::string value;
};

struct VariableField {
    std::string name;
};

using AtomicField = std::variant<IntegerField, FloatField, StringField, VariableField>;
std::string atomicToString(const AtomicField& field);
souffle::RamDomain evaluateAtomic(
        const AtomicField& field, const std::vector<std::string>& vars,
        const std::vector<souffle::RamDomain>& values);

struct ExprField;
using ExprFieldPtr = std::shared_ptr<ExprField>;
using ExprFieldReal = std::variant<AtomicField, ExprFieldPtr>;
souffle::RamDomain evaluateExprReal(
        const ExprFieldReal& real, const std::vector<std::string>& vars,
        const std::vector<souffle::RamDomain>& values);


struct ExprField {
    enum class OpType { And, Or, Neg, Atom, Add, Sub };
    OpType op;
    std::vector<ExprFieldReal> operands;
    ExprField(OpType op, std::vector<ExprFieldReal> ops)
    : op(op), operands(std::move(ops)) {}

    souffle::RamDomain evaluate(const std::vector<std::string>& vars,
            const std::vector<souffle::RamDomain>& values);

    static std::shared_ptr<ExprField> makeAtom(IntegerField field) {
        return std::make_shared<ExprField>(OpType::Atom, std::vector{ExprFieldReal{std::move(field)}});
    }
    static std::shared_ptr<ExprField> makeAtom(FloatField field) {
        return std::make_shared<ExprField>(OpType::Atom, std::vector{ExprFieldReal{std::move(field)}});
    }
    static std::shared_ptr<ExprField> makeAtom(StringField field) {
        return std::make_shared<ExprField>(OpType::Atom, std::vector{ExprFieldReal{std::move(field)}});
    }
    static std::shared_ptr<ExprField> makeAtom(VariableField field) {
        return std::make_shared<ExprField>(OpType::Atom, std::vector{ExprFieldReal{std::move(field)}});
    }
    static std::shared_ptr<ExprField> makeAtom(AtomicField field) {
        return std::make_shared<ExprField>(OpType::Atom, std::vector{ExprFieldReal{std::move(field)}});
    }

    static std::shared_ptr<ExprField> makeAdd(ExprFieldReal lhs, ExprFieldReal rhs) {
        return std::make_shared<ExprField>(OpType::Add, std::vector{std::move(lhs), std::move(rhs)});
    }

    static std::shared_ptr<ExprField> makeSub(ExprFieldReal lhs, ExprFieldReal rhs) {
        return std::make_shared<ExprField>(OpType::Sub, std::vector{std::move(lhs), std::move(rhs)});
    }

    static std::shared_ptr<ExprField> makeAnd(ExprFieldReal lhs, ExprFieldReal rhs) {
        return std::make_shared<ExprField>(OpType::And, std::vector{std::move(lhs), std::move(rhs)});
    }

    static std::shared_ptr<ExprField> makeOr(ExprFieldReal lhs, ExprFieldReal rhs) {
        return std::make_shared<ExprField>(OpType::Or, std::vector{std::move(lhs), std::move(rhs)});
    }

    static std::shared_ptr<ExprField> makeNeg(ExprFieldReal child) {
        return std::make_shared<ExprField>(OpType::Neg, std::vector{std::move(child)});
    }

    std::string exprFieldRealToString(const ExprFieldReal& expr) {
        if (std::holds_alternative<AtomicField>(expr)) {
            return atomicToString(std::get<AtomicField>(expr));
        } else if (std::holds_alternative<ExprFieldPtr>(expr)) {
            return exprFieldToString(*std::get<ExprFieldPtr>(expr));
        }
        assert(false && "Unknown ExprFieldReal type");
        return "<invalid-expr-real>";
    }

    std::string exprFieldToString(const ExprField& expr) {
        using Op = ExprField::OpType;
        const auto& ops = expr.operands;

        switch (expr.op) {
            case Op::Add:
                assert(ops.size() == 2);
            return "(" + exprFieldRealToString(ops[0]) + " + " + exprFieldRealToString(ops[1]) + ")";
            case Op::Sub:
                assert(ops.size() == 2);
            return "(" + exprFieldRealToString(ops[0]) + " - " + exprFieldRealToString(ops[1]) + ")";
            case Op::Neg:
                assert(ops.size() == 1);
            return "(not " + exprFieldRealToString(ops[0]) + ")";
            case Op::And:
                assert(ops.size() == 2);
            return "(" + exprFieldRealToString(ops[0]) + " and " + exprFieldRealToString(ops[1]) + ")";
            case Op::Or:
                assert(ops.size() == 2);
            return "(" + exprFieldRealToString(ops[0]) + " or " + exprFieldRealToString(ops[1]) + ")";
            case Op::Atom:
                assert(ops.size() == 1);
            return exprFieldRealToString(ops[0]);
        }
        return "<invalid-expr>";
    }
};

struct SymbolicField {
    std::variant<IntegerField,
                 FloatField,
                 StringField,
                 VariableField,
                 std::shared_ptr<ExprField>> field;

    explicit SymbolicField(int value) : field(IntegerField{value}) {}
    explicit SymbolicField(double value) : field(FloatField{value}) {}
    explicit SymbolicField(const StringField& field) : field(field) {}
    explicit SymbolicField(const VariableField& field) : field(field) {}
    SymbolicField(const std::shared_ptr<ExprField>& ptr) : field(ptr) {}
    SymbolicField(std::shared_ptr<ExprField>&& ptr) : field(std::move(ptr)) {}

    static SymbolicField makeVariable(const std::string& varname) {
        SymbolicField result{VariableField{varname}};
        return result;
    }

    static SymbolicField makeUnnamedVariable() {
        SymbolicField result{VariableField{"_"}};
        return result;
    }

    std::string toString() const {
        if (std::holds_alternative<IntegerField>(field)) {
            return std::to_string(std::get<IntegerField>(field).value);
        } else if (std::holds_alternative<FloatField>(field)) {
            return std::to_string(std::get<FloatField>(field).value);
        } else if (std::holds_alternative<StringField>(field)) {
            return '"' + std::get<StringField>(field).value + '"';
        } else if (std::holds_alternative<VariableField>(field)) {
            return std::get<VariableField>(field).name;
        } else if (std::holds_alternative<std::shared_ptr<ExprField>>(field)) {
            return std::get<std::shared_ptr<ExprField>>(field)->exprFieldToString(*std::get<std::shared_ptr<ExprField>>(field));
        }
        assert (false && "Unknown field type");
    }
};

inline souffle::RamDomain evaluateSymbolicField(
        const SymbolicField& field, const std::vector<std::string>& vars,
        const std::vector<souffle::RamDomain>& values) {
    if (std::holds_alternative<IntegerField>(field.field)) {
        return std::get<IntegerField>(field.field).value;
    }
    if (std::holds_alternative<FloatField>(field.field)) {
        return souffle::ramBitCast<souffle::RamDomain>(
                static_cast<souffle::RamFloat>(std::get<FloatField>(field.field).value));
    }
    if (std::holds_alternative<StringField>(field.field)) {
        auto* symbolTable = souffle::problog::getActiveSymbolTable();
        assert(symbolTable != nullptr && "Active symbol table required for StringField evaluation");
        return symbolTable->encode(std::get<StringField>(field.field).value);
    }
    if (std::holds_alternative<VariableField>(field.field)) {
        const std::string& varName = std::get<VariableField>(field.field).name;
        for (size_t i = 0; i < vars.size(); ++i) {
            if (vars[i] == varName) {
                return values[i];
            }
        }
        assert(false && "Variable not found in map");
    }
    if (std::holds_alternative<std::shared_ptr<ExprField>>(field.field)) {
        return std::get<std::shared_ptr<ExprField>>(field.field)->evaluate(vars, values);
    }
    assert(false && "Unsupported symbolic field in evaluation");
    return 0;
}
class Atom {
public:
    Atom(std::string relation, std::vector<SymbolicField> fields, bool isNegated = false)
        : relation(std::move(relation)), fields(std::move(fields)), isNegated(isNegated) {}

    Atom(const Atom& other)
        : relation(other.relation), fields(other.fields), isNegated(other.isNegated) {}

    Atom(Atom&& other) noexcept
        : relation(std::move(other.relation))
        , fields(std::move(other.fields))
        , isNegated(other.isNegated) {}

    std::string toString() const {
        std::string result = isNegated ? "!" : "";
        result += relation + "(";

        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) result += ", ";
            result += fields[i].toString();
        }
        result += ")";
        return result;
    }

    const std::string& getRelation() const {
        return relation;
    }

    const std::vector<SymbolicField>& getFields() const {
        return fields;
    }

    bool isNegatedAtom() const {
        return isNegated;
    }

    std::vector<std::string> getVars() {
        std::vector<std::string> vars;
        for (const auto& field : fields) {
            if (std::holds_alternative<VariableField>(field.field)) {
                vars.push_back(std::get<VariableField>(field.field).name);
            }
        }
        return vars;
    }

    std::vector<souffle::RamDomain> instantiatedFields(const std::vector<std::string>& vars,
            const std::vector<souffle::RamDomain>& values) const {
        std::vector<souffle::RamDomain> result;
        for (const auto& field : fields) {
            result.push_back(evaluateSymbolicField(field, vars, values));
        }
        return result;
    }
private:
    std::string relation;
    std::vector<SymbolicField> fields;
    bool isNegated;
};

#endif //ATOM_H
