#include "souffle/problog/Atom.h"

namespace {
souffle::SymbolTable* activeProblogSymbolTable = nullptr;
}

namespace souffle::problog {
void setActiveSymbolTable(SymbolTable* symbolTable) {
    activeProblogSymbolTable = symbolTable;
}

SymbolTable* getActiveSymbolTable() {
    return activeProblogSymbolTable;
}
}

std::string atomicToString(const AtomicField& field) {
    if (std::holds_alternative<IntegerField>(field)) {
        return std::to_string(std::get<IntegerField>(field).value);
    } else if (std::holds_alternative<FloatField>(field)) {
        return std::to_string(std::get<FloatField>(field).value);
    } else if (std::holds_alternative<StringField>(field)) {
        return '"' + std::get<StringField>(field).value + '"';
    } else if (std::holds_alternative<VariableField>(field)) {
        return std::get<VariableField>(field).name;
    }
    assert(false && "Unknown atomic type");
    return "";
}

souffle::RamDomain evaluateAtomic(
        const AtomicField& field, const std::vector<std::string>& vars,
        const std::vector<souffle::RamDomain>& values) {
    if (std::holds_alternative<IntegerField>(field)) {
        return std::get<IntegerField>(field).value;
    } else if (std::holds_alternative<FloatField>(field)) {
        return souffle::ramBitCast<souffle::RamDomain>(
                static_cast<souffle::RamFloat>(std::get<FloatField>(field).value));
    } else if (std::holds_alternative<StringField>(field)) {
        auto* symbolTable = souffle::problog::getActiveSymbolTable();
        assert(symbolTable != nullptr && "Active symbol table required for StringField evaluation");
        return symbolTable->encode(std::get<StringField>(field).value);
    } else if (std::holds_alternative<VariableField>(field)) {
        const std::string& var = std::get<VariableField>(field).name;
        for (size_t i = 0; i < vars.size(); ++i) {
            if (vars[i] == var) {
                return values[i];
            }
        }
        assert(false && "Variable not found in context");
    }
    assert(false && "Unsupported atomic field type in evaluation");
    return 0;
}

souffle::RamDomain evaluateExprReal(
        const ExprFieldReal& real, const std::vector<std::string>& vars,
        const std::vector<souffle::RamDomain>& values) {
    if (std::holds_alternative<AtomicField>(real)) {
        return evaluateAtomic(std::get<AtomicField>(real), vars, values);
    } else if (std::holds_alternative<ExprFieldPtr>(real)) {
        return std::get<ExprFieldPtr>(real)->evaluate(vars, values);
    }
    assert(false && "Unknown ExprFieldReal type");
    return 0;
}

souffle::RamDomain ExprField::evaluate(const std::vector<std::string>& vars,
        const std::vector<souffle::RamDomain>& values) {
    using Op = ExprField::OpType;
    assert(op == Op::Atom || operands.size() >= 1);

    switch (op) {
        case Op::Atom:
            return evaluateExprReal(operands[0], vars, values);
        case Op::Add:
            assert(operands.size() == 2);
            return evaluateExprReal(operands[0], vars, values) +
                   evaluateExprReal(operands[1], vars, values);
        case Op::Sub:
            assert(operands.size() == 2);
            return evaluateExprReal(operands[0], vars, values) -
                   evaluateExprReal(operands[1], vars, values);
        case Op::And:
            assert(operands.size() == 2);
            return evaluateExprReal(operands[0], vars, values) &&
                   evaluateExprReal(operands[1], vars, values);
        case Op::Or:
            assert(operands.size() == 2);
            return evaluateExprReal(operands[0], vars, values) ||
                   evaluateExprReal(operands[1], vars, values);
        case Op::Neg:
            assert(operands.size() == 1);
            return !evaluateExprReal(operands[0], vars, values);
    }
    assert(false && "Unknown expression op in evaluation");
    return 0;
}
