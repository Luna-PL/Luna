#include "ContainerModel.h"

namespace moon {

std::optional<CodeOperationOpcode> codeOperationOpcode(const Stmt& operation) {
    if (dynamic_cast<const LetStmt*>(&operation))
        return CodeOperationOpcode::Let;
    if (dynamic_cast<const AllocateStmt*>(&operation))
        return CodeOperationOpcode::Allocate;
    if (dynamic_cast<const ExprStmt*>(&operation))
        return CodeOperationOpcode::Expression;
    if (dynamic_cast<const FreeStmt*>(&operation))
        return CodeOperationOpcode::Free;
    if (dynamic_cast<const AwaitStmt*>(&operation))
        return CodeOperationOpcode::Await;
    return std::nullopt;
}

std::optional<CodeExpressionOpcode> codeExpressionOpcode(
    const Expr& expression) {
    if (dynamic_cast<const IntLiteralExpr*>(&expression)) return CodeExpressionOpcode::Integer;
    if (dynamic_cast<const FloatLiteralExpr*>(&expression)) return CodeExpressionOpcode::Floating;
    if (dynamic_cast<const StringLiteralExpr*>(&expression)) return CodeExpressionOpcode::String;
    if (dynamic_cast<const BoolLiteralExpr*>(&expression)) return CodeExpressionOpcode::Boolean;
    if (dynamic_cast<const UnitExpr*>(&expression)) return CodeExpressionOpcode::Unit;
    if (dynamic_cast<const IdentifierExpr*>(&expression)) return CodeExpressionOpcode::Identifier;
    if (dynamic_cast<const BinaryExpr*>(&expression)) return CodeExpressionOpcode::Binary;
    if (dynamic_cast<const UnaryExpr*>(&expression)) return CodeExpressionOpcode::Unary;
    if (dynamic_cast<const CallExpr*>(&expression)) return CodeExpressionOpcode::Call;
    if (dynamic_cast<const LaunchExpr*>(&expression)) return CodeExpressionOpcode::Launch;
    if (dynamic_cast<const VariantConstructExpr*>(&expression)) return CodeExpressionOpcode::VariantConstruct;
    if (dynamic_cast<const ResultConstructExpr*>(&expression)) return CodeExpressionOpcode::ResultConstruct;
    if (dynamic_cast<const FieldAccessExpr*>(&expression)) return CodeExpressionOpcode::FieldAccess;
    if (dynamic_cast<const IndexExpr*>(&expression)) return CodeExpressionOpcode::Index;
    if (dynamic_cast<const SliceLengthExpr*>(&expression)) return CodeExpressionOpcode::SliceLength;
    if (dynamic_cast<const ArrayLiteralExpr*>(&expression)) return CodeExpressionOpcode::ArrayLiteral;
    if (dynamic_cast<const RecordLiteralExpr*>(&expression)) return CodeExpressionOpcode::RecordLiteral;
    if (dynamic_cast<const HeapAllocExpr*>(&expression)) return CodeExpressionOpcode::HeapAllocate;
    if (dynamic_cast<const InitAllocationExpr*>(&expression)) return CodeExpressionOpcode::InitializeAllocation;
    if (dynamic_cast<const MoveExpr*>(&expression)) return CodeExpressionOpcode::Move;
    if (dynamic_cast<const BorrowExpr*>(&expression)) return CodeExpressionOpcode::Borrow;
    if (dynamic_cast<const DerefExpr*>(&expression)) return CodeExpressionOpcode::Dereference;
    if (dynamic_cast<const AddrOfExpr*>(&expression)) return CodeExpressionOpcode::AddressOf;
    if (dynamic_cast<const LambdaExpr*>(&expression)) return CodeExpressionOpcode::Lambda;
    if (dynamic_cast<const MakeClosureExpr*>(&expression)) return CodeExpressionOpcode::MakeClosure;
    if (dynamic_cast<const EnvLoadExpr*>(&expression)) return CodeExpressionOpcode::EnvironmentLoad;
    if (dynamic_cast<const AssignExpr*>(&expression)) return CodeExpressionOpcode::Assign;
    return std::nullopt;
}

} // namespace moon
