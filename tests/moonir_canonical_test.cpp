#include "moonir/MoonIR.h"
#include "moonir/Verifier.h"

#include <iostream>
#include <type_traits>

static_assert(std::is_same_v<decltype(moon::MetadataField::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::DeclarationRecord::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::Param::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::Expr::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::LetStmt::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::CallExpr::intrinsicType), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::CallExpr::calleeRef), moon::DeclarationRef>);
static_assert(std::is_same_v<decltype(moon::ForStmt::protocolNext), moon::DeclarationRef>);
static_assert(std::is_same_v<decltype(moon::LaunchExpr::kernelRef), moon::DeclarationRef>);
static_assert(std::is_same_v<decltype(moon::TryExpr::errorConversion), moon::DeclarationRef>);
static_assert(std::is_same_v<decltype(moon::FunctionDecl::returnType), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::StructDecl::type), moon::TypeRef>);

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    auto shortArray = Type::makeArray(TyI32, 2);
    auto longArray = Type::makeArray(TyI32, 5);
    auto shortIterator = Type::makeIterator(
        TyI32, IteratorMode::Consuming, shortArray);
    auto longIterator = Type::makeIterator(
        TyI32, IteratorMode::Consuming, longArray);

    moon::Module module;
    module.name = "canonical.test";
    const auto shortId = module.registerType(shortIterator);
    const auto longId = module.registerType(longIterator);
    if (shortId == longId)
        return fail("backend-significant iterator sources collapsed to one TypeId");

    auto product = Type::makeStruct(
        "Snapshot", {{"value", TyI32}}, "canonical.test::Snapshot");
    const auto productId = module.registerType(product);
    auto forward = Type::makeStruct(
        "Forward", {}, "canonical.test::Forward");
    auto completedForward = Type::makeStruct(
        "Forward", {{"value", TyI32}}, "canonical.test::Forward");
    const auto forwardId = module.registerType(forward);
    if (module.registerType(completedForward) != forwardId)
        return fail("completed nominal declaration changed its stable TypeId");
    module.sealTypeTable();

    // The sealed payload must not observe later mutations of the frontend
    // object from which it was frozen.
    product->fields.clear();
    moon::TypeMaterializer materializer(module);
    const auto restoredProduct = materializer.materialize(productId);
    if (!restoredProduct || restoredProduct.get() == product.get() ||
        restoredProduct->fields.size() != 1 ||
        restoredProduct->fields.front().name != "value")
        return fail("sealed MoonIR retained frontend Type object identity");
    const auto restoredForward = materializer.materialize(forwardId);
    if (!restoredForward || restoredForward->fields.size() != 1)
        return fail("completed nominal payload did not replace its forward placeholder");

    const auto restoredShort = materializer.materialize(shortId);
    const auto restoredLong = materializer.materialize(longId);
    if (!restoredShort || !restoredLong ||
        restoredShort->typeArgs.size() != 1 ||
        restoredLong->typeArgs.size() != 1 ||
        restoredShort->typeArgs.front()->arrayLength != 2 ||
        restoredLong->typeArgs.front()->arrayLength != 5)
        return fail("canonical type materialization lost iterator source shape");

    moon::Module reverse;
    reverse.name = module.name;
    reverse.registerType(longIterator);
    reverse.registerType(shortIterator);
    reverse.registerType(Type::makeStruct(
        "Snapshot", {{"value", TyI32}}, "canonical.test::Snapshot"));
    reverse.registerType(completedForward);
    reverse.registerType(forward);
    reverse.sealTypeTable();
    if (module.typeTable.size() != reverse.typeTable.size())
        return fail("type table depends on registration order");
    for (size_t index = 0; index < module.typeTable.size(); ++index) {
        if (module.typeTable[index].id != reverse.typeTable[index].id ||
            module.typeTable[index].canonicalType !=
                reverse.typeTable[index].canonicalType ||
            module.typeTable[index].canonicalShape !=
                reverse.typeTable[index].canonicalShape)
            return fail("sealed type table is not deterministic");
    }

    moon::Verifier verifier;
    if (!verifier.verify(module))
        return fail("canonical type-only module failed independent verification");
    if (!verifier.verify(reverse))
        return fail("reverse-order canonical module failed independent verification");
    const auto reverseIterator = reverse.typesById.find(shortId.value);
    if (reverseIterator == reverse.typesById.end())
        return fail("sealed type index lost the iterator type");
    reverse.typeTable[reverseIterator->second].sysmeta.resource.usage =
        luna::ownership::Usage::Copy;
    if (verifier.verify(reverse))
        return fail("verifier accepted a forged derived Resource contract");

    moon::Module symbols;
    symbols.name = "canonical.symbols";
    const auto calleeType = Type::makeFunction({TyI32}, TyI32);
    const auto callerType = Type::makeFunction({}, TyUnit);
    const auto calleeTypeRef = symbols.registerType(calleeType);
    const auto callerTypeRef = symbols.registerType(callerType);
    auto addFunctionRecord = [&](const std::string& id,
                                 const std::string& linkage,
                                 const TypePtr& type,
                                 const moon::TypeRef& typeReference) {
        moon::DeclarationRecord record;
        record.id = id;
        record.familyId = id;
        record.symbolId = luna::identity::symbolIdFromCanonical(id);
        record.sourceName = linkage;
        record.linkageName = linkage;
        record.kind = moon::DeclarationKind::Function;
        record.type = typeReference;
        record.sysmeta = type->sysmeta;
        record.canonicalContract = moon::canonicalContract(record);
        record.contractId = luna::identity::contractIdFromCanonical(
            record.canonicalContract);
        record.sysmeta.identity.symbol = record.symbolId;
        record.sysmeta.identity.contract = record.contractId;
        symbols.declarationTable.push_back(std::move(record));
    };
    const std::string calleeId =
        "canonical.symbols::fn::callee";
    const std::string callerId =
        "canonical.symbols::fn::caller";
    addFunctionRecord(calleeId, "callee", calleeType, calleeTypeRef);
    addFunctionRecord(callerId, "caller", callerType, callerTypeRef);

    auto caller = std::make_unique<moon::FunctionDecl>();
    caller->packageId = symbols.name;
    caller->declarationId = callerId;
    caller->familyId = callerId;
    caller->symbolId = luna::identity::symbolIdFromCanonical(callerId);
    caller->name = "caller";
    caller->generatedSymbolName = "caller";
    caller->returnType = symbols.registerType(TyUnit);
    caller->body = std::make_unique<moon::BlockStmt>();
    auto statement = std::make_unique<moon::ExprStmt>();
    auto call = std::make_unique<moon::CallExpr>();
    auto callee = std::make_unique<moon::IdentifierExpr>();
    callee->name = "callee";
    callee->type = calleeTypeRef;
    call->callee = std::move(callee);
    auto argument = std::make_unique<moon::IntLiteralExpr>();
    argument->value = 1;
    argument->type = symbols.registerType(TyI32);
    call->args.push_back(std::move(argument));
    call->type = symbols.registerType(TyI32);
    auto* loweredCall = call.get();
    statement->expr = std::move(call);
    caller->body->stmts.push_back(std::move(statement));
    symbols.declarations.push_back(std::move(caller));
    symbols.sealTypeTable();

    const auto* calleeRecord = symbols.findDeclarationById(calleeId);
    const auto* callerRecord = symbols.findDeclarationById(callerId);
    if (!calleeRecord || !callerRecord)
        return fail("sealed declaration table lost stable symbol rows");
    loweredCall->calleeRef = {
        calleeRecord->symbolId, calleeRecord->contractId};
    auto* loweredIdentifier = static_cast<moon::IdentifierExpr*>(
        loweredCall->callee.get());
    loweredIdentifier->declaration = loweredCall->calleeRef;
    symbols.declarations.front()->contractId = callerRecord->contractId;
    if (!verifier.verify(symbols))
        return fail("stable declaration references failed independent verification");
    loweredCall->calleeRef.contract = {"contract_forged"};
    if (verifier.verify(symbols))
        return fail("verifier accepted a forged call-site ContractId");

    auto productRecord = module.typesById.find(productId.value);
    if (productRecord == module.typesById.end())
        return fail("sealed type index lost the product type");
    module.typeTable[productRecord->second].fields.clear();
    if (verifier.verify(module))
        return fail("verifier accepted a frozen payload inconsistent with its TypeId");
    return 0;
}
