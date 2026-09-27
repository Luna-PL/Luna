#include "core/CoreContracts.h"
#include "core/SysMeta.h"
#include "core/TypeRelations.h"
#include "core/TypeSystem.h"

#include <cstring>
#include <iostream>
#include <type_traits>

namespace {

bool equal(const char* left, const char* right) {
    return std::strcmp(left, right) == 0;
}

} // namespace

int testRuntimeFragmentRefTypes() {
    static_assert(static_cast<unsigned>(TypeKind::Slot) == 27 &&
                  static_cast<unsigned>(TypeKind::Fragment) == 28 &&
                  static_cast<unsigned>(TypeKind::Unknown) == 40 &&
                  static_cast<unsigned>(TypeKind::RuntimeFragmentRef) == 41,
                  "internal Ref preparation must not shift existing wire type kinds");
    using namespace luna::types;
    using namespace luna::ownership;
    const auto slot = [](const std::string& identity) {
        auto type = Type::makeSlot({TyI32});
        type->identityMode = IdentityMode::Nominal;
        type->nominalId = identity;
        type->name = "checkpoint";
        return type;
    };
    const auto firstSlot = slot("org.luna.ref::first");
    const auto secondSlot = slot("org.luna.ref::second");
    const auto first = Type::makeRuntimeFragmentRef(firstSlot);
    const auto repeated = Type::makeRuntimeFragmentRef(slot(firstSlot->nominalId));
    const auto second = Type::makeRuntimeFragmentRef(secondSlot);
    const auto resource = resourceContractForType(first);
    if (!isWellFormedTypeDomain(first) || !sameType(first, repeated) ||
        sameType(first, second) || !sameShape(first, second) ||
        isAssignable(first, second) || isExplicitlyConvertible(first, second) ||
        isAbiCompatible(first, second) ||
        !isExplicitlyConvertible(first, repeated) || !isAbiCompatible(first, repeated) ||
        first->toString() != "RuntimeFragmentRef<checkpoint>" ||
        first->sysmeta.control.form != luna::sysmeta::ControlForm::Plain ||
        resource.usage != Usage::Affine || resource.relation != Relation::Owned ||
        resource.cleanup != CleanupAction::Drop || !resource.cleanupRequired ||
        resource.recursiveCleanup || resource.management != luna::sysmeta::ResourceManagement::Unique ||
        resource.releaseDomain != luna::sysmeta::ReleaseDomain::Executable ||
        resource.lifetime != luna::sysmeta::ResourceLifetime::Lexical)
        return (std::cerr << "runtime Fragment Ref identity/resource contract is inconsistent\n", 1);
    const auto borrowed = parameterContractFor(first);
    const auto owning = parameterContractFor(first, Usage::Affine, true);
    if (borrowed.relation != Relation::SharedBorrow || borrowed.usage != Usage::Copy ||
        owning.relation != Relation::Owned || owning.usage != Usage::Affine)
        return (std::cerr << "runtime Fragment Ref parameter conventions weakened ownership\n", 1);
    const auto wrap = [](const TypePtr& ref) {
        return TypeVec{
            Type::makeArray(ref, 2), Type::makeRecord({{"ref", ref}}),
            Type::makeResult(ref, TyI32), Type::makeReference(ref),
            Type::makeRawPointer(ref), Type::makeFunction({ref}, TyUnit),
            Type::makeClosure({}, TyUnit, {}, {}, {{"ref", ref}})};
    };
    const auto firstWrappers = wrap(first);
    const auto secondWrappers = wrap(second);
    for (size_t index = 0; index < firstWrappers.size(); ++index) {
        if (!containsRuntimeFragmentRef(firstWrappers[index]) ||
            !sameShape(firstWrappers[index], secondWrappers[index]) ||
            isExplicitlyConvertible(firstWrappers[index], secondWrappers[index]) ||
            isAbiCompatible(firstWrappers[index], secondWrappers[index]))
            return (std::cerr << "a structural wrapper erased nominal Fragment Ref safety\n", 1);
    }
    const auto nominalFirst = Type::makeStruct("Holder", {{"ref", first}}, "org.luna.ref::Holder");
    const auto nominalSecond = Type::makeStruct("Holder", {{"ref", second}}, "org.luna.ref::Holder");
    if (!sameType(nominalFirst, nominalSecond) || !sameShape(nominalFirst, nominalSecond) ||
        isExplicitlyConvertible(nominalFirst, nominalSecond) ||
        isAbiCompatible(nominalFirst, nominalSecond))
        return (std::cerr << "nominal aggregate payload changes erased a Ref constraint\n", 1);
    for (size_t index : {size_t{0}, size_t{1}, size_t{2}, size_t{6}}) {
        if (defaultUsageForType(firstWrappers[index]) != Usage::Affine ||
            !typeRequiresCleanup(firstWrappers[index]) ||
            !typeHasRecursiveCleanup(firstWrappers[index]))
            return (std::cerr << "Fragment Ref aggregate lost affine recursive cleanup\n", 1);
    }
    if (defaultUsageForType(firstWrappers[3]) != Usage::Copy ||
        typeRequiresCleanup(firstWrappers[3]))
        return (std::cerr << "a shared Ref borrow acquired owner cleanup\n", 1);
    auto changedContractSlot = slot(firstSlot->nominalId);
    changedContractSlot->paramTypes = {TyI64};
    const auto changedContract = Type::makeRuntimeFragmentRef(changedContractSlot);
    if (!sameType(first, changedContract) || sameShape(first, changedContract) ||
        isAbiCompatible(first, changedContract))
        return (std::cerr << "Slot nominal identity and contract shape were conflated\n", 1);
    const auto invalid = [](const TypePtr& target) {
        std::string reason;
        return !isWellFormedTypeDomain(Type::makeRuntimeFragmentRef(target), &reason) &&
            reason.find("RuntimeFragmentRef") != std::string::npos;
    };
    auto nonunit = slot("org.luna.ref::nonunit");
    nonunit->returnType = TyI32;
    auto nested = slot("org.luna.ref::nested");
    nested->paramTypes = {Type::makeReference(Type::makeRecord({{"ref", first}}))};
    auto affine = slot("org.luna.ref::affine");
    affine->paramTypes = {TyString};
    auto control = slot("org.luna.ref::control");
    control->sysmeta.control.cardinality = luna::sysmeta::Cardinality::None;
    auto unresolved = slot("org.luna.ref::unresolved");
    unresolved->paramTypes = {Type::makeRawPointer(Type::makeTypeParam("T"))};
    auto unknown = slot("org.luna.ref::unknown");
    unknown->paramTypes = {Type::makeUnknown()};
    auto inconsistent = slot("org.luna.ref::inconsistent");
    inconsistent->sysmeta.resource.parameters.clear();
    if (!invalid(nullptr) || !invalid(TyI32) || !invalid(Type::makeFunction({}, TyUnit)) ||
        !invalid(Type::makeSlot({TyI32})) || !invalid(nonunit) || !invalid(nested) ||
        !invalid(affine) || !invalid(control) || !invalid(unresolved) ||
        !invalid(unknown) || !invalid(inconsistent))
        return (std::cerr << "RuntimeFragmentRef accepted an invalid target Slot\n", 1);
    for (unsigned violation = 0; violation < 17; ++violation) {
        auto forged = Type::makeRuntimeFragmentRef(firstSlot);
        switch (violation) {
            case 0: forged->sysmeta.resource.needsDrop = false; break;
            case 1: forged->sysmeta.resource.management = luna::sysmeta::ResourceManagement::Value; break;
            case 2: forged->sysmeta.resource.releaseDomain = luna::sysmeta::ReleaseDomain::LunaGlobal; break;
            case 3: forged->sysmeta.resource.lifetime = luna::sysmeta::ResourceLifetime::Explicit; break;
            case 4: forged->sysmeta.resource.relation = Relation::SharedBorrow; break;
            case 5: forged->sysmeta.resource.usage = Usage::Copy; break;
            case 6: forged->sysmeta.resource.cleanup = CleanupAction::Deallocate; break;
            case 7: forged->sysmeta.resource.cleanupRequired = false; break;
            case 8: forged->sysmeta.resource.recursiveCleanup = true; break;
            case 9: forged->identityMode = IdentityMode::Builtin; break;
            case 10: forged->sysmeta.control.form = luna::sysmeta::ControlForm::Fragment; break;
            case 11: forged->sysmeta.capability.hostOnly = false; break;
            case 12: forged->fields = {{"hidden", first}}; break;
            case 13: forged->sysmeta.control.cardinality = luna::sysmeta::Cardinality::Once; break;
            case 14: forged->sysmeta.control.storage = luna::sysmeta::ContinuationStorage::ScopedStack; break;
            case 15: forged->sysmeta.control.forwarding = luna::sysmeta::Forwarding::Explicit; break;
            case 16: forged->sysmeta.control.abortPermitted = true; break;
        }
        if (isWellFormedTypeDomain(forged))
            return (std::cerr << "RuntimeFragmentRef accepted forged type/resource facts\n", 1);
    }
    auto recursive = Type::makeRecord({});
    recursive->fields = {{"cycle", recursive}, {"ref", first}};
    const bool foundRef = containsRuntimeFragmentRef(recursive);
    recursive->fields.clear(); // Avoid retaining a fixture shared_ptr cycle.
    if (!foundRef)
        return (std::cerr << "recursive type graph hid a RuntimeFragmentRef\n", 1);
    return 0;
}

int main() {
    static_assert(!std::is_same<luna::identity::SymbolId,
                                luna::identity::ContractId>::value,
                  "SymbolId and ContractId must remain distinct types");
    static_assert(!std::is_same<luna::identity::TypeId,
                                luna::identity::AbiLayoutId>::value,
                  "TypeId and AbiLayoutId must remain distinct types");
    static_assert(luna::ownership::usageStrength(
                      luna::ownership::Usage::Copy) <
                      luna::ownership::usageStrength(
                          luna::ownership::Usage::Affine) &&
                  luna::ownership::usageStrength(
                      luna::ownership::Usage::Affine) <
                      luna::ownership::usageStrength(
                          luna::ownership::Usage::Linear),
                  "usage requirements must remain monotonically ordered");
    static_assert(luna::ownership::strongerUsage(
                      luna::ownership::Usage::Copy,
                      luna::ownership::Usage::Linear) ==
                      luna::ownership::Usage::Linear &&
                  !luna::ownership::satisfiesUsageRequirement(
                      luna::ownership::Usage::Copy,
                      luna::ownership::Usage::Affine),
                  "binding defaults must not weaken inherent usage");

    using namespace luna::core_contracts;
    if (luna::sysmeta::SchemaMajor != 1 ||
        luna::sysmeta::SchemaMinor != 3 ||
        !equal(PackageId, "org.luna.core") ||
        !equal(canonical_0_3::ResultTypeId,
               "org.luna.core::result::Result") ||
        !equal(canonical_0_3::DropTraitId,
               "org.luna.core::resource::Drop") ||
        !equal(canonical_0_3::FromTraitId,
               "org.luna.core::convert::From") ||
        !equal(canonical_0_3::TryFromIteratorTraitId,
               "org.luna.core::iter::TryFromIterator")) {
        std::cerr << "canonical Core identity changed unexpectedly\n";
        return 1;
    }
    const auto firstSymbol = luna::identity::symbolIdFromCanonical(
        "org.luna.example::main::fn::run");
    const auto repeatedSymbol = luna::identity::symbolIdFromCanonical(
        "org.luna.example::main::fn::run");
    const auto secondSymbol = luna::identity::symbolIdFromCanonical(
        "org.luna.example::main::fn::stop");
    const auto contract = luna::identity::contractIdFromCanonical(
        "luna.contract.v1;example");
    const auto layout = luna::identity::abiLayoutIdFromCanonical(
        "luna.abi-layout.v1;example");
    if (firstSymbol.empty() || contract.empty() || layout.empty() ||
        firstSymbol != repeatedSymbol || firstSymbol == secondSymbol ||
        firstSymbol.value.rfind("symbol_", 0) != 0 ||
        contract.value.rfind("contract_", 0) != 0 ||
        layout.value.rfind("abi_", 0) != 0) {
        std::cerr << "stable identity domains are not deterministic or separated\n";
        return 1;
    }
    if (!equal(luna::sysmeta::DropTraitId,
               canonical_0_3::DropTraitId) ||
        !equal(luna::sysmeta::FromTraitId,
               canonical_0_3::FromTraitId) ||
        !equal(luna::sysmeta::ResultTypeId,
               canonical_0_3::ResultTypeId)) {
        std::cerr << "active compiler-known Core identity is inconsistent\n";
        return 1;
    }
    const auto result = Type::makeResult(
        Type::makePrimitive(TypeKind::I32),
        Type::makePrimitive(TypeKind::String));
    if (result->identityMode != luna::types::IdentityMode::Nominal ||
        result->nominalId != canonical_0_3::ResultTypeId ||
        result->typeArgs.size() != 2) {
        std::cerr << "Result did not acquire its canonical nominal identity\n";
        return 1;
    }
    const auto metadata = Type::makeMetadata("org.luna.test::Schema");
    const auto validMetadataView = Type::makeMetadataView(metadata);
    const auto invalidArray = Type::makeArray(metadata, 1);
    const auto invalidFunction = Type::makeFunction({metadata}, TyI32);
    const auto genericValue = Type::makeStruct(
        "Holder", {{"value", Type::makeTypeParam("T")}},
        "org.luna.test::Holder");
    std::string domainError;
    if (!luna::types::isWellFormedTypeDomain(result) ||
        !luna::types::isWellFormedTypeDomain(validMetadataView) ||
        !luna::types::isWellFormedTypeDomain(genericValue) ||
        luna::types::isWellFormedTypeDomain(invalidArray, &domainError) ||
        domainError.find("cannot contain meta-domain type") ==
            std::string::npos ||
        luna::types::isWellFormedTypeDomain(invalidFunction)) {
        std::cerr << "Value type-domain formation is not fail-closed\n";
        return 1;
    }
    if (!equal(luna::sysmeta::releaseDomainName(
                   luna::sysmeta::ReleaseDomain::LunaGlobal),
               "luna_global") ||
        !equal(luna::sysmeta::releaseDomainName(
                   luna::sysmeta::ReleaseDomain::HostService),
               "host_service") ||
        !equal(GlobalAllocatorDomainId,
               "org.luna.alloc::global::Global")) {
        std::cerr << "global allocator domain contract is inconsistent\n";
        return 1;
    }
    if (!equal(luna::sysmeta::resourceLifetimeName(
                   luna::sysmeta::ResourceLifetime::Lexical),
               "lexical") ||
        !equal(luna::sysmeta::resourceLifetimeName(
                   luna::sysmeta::ResourceLifetime::Explicit),
               "explicit")) {
        std::cerr << "resource lifetime contract is inconsistent\n";
        return 1;
    }
    return testRuntimeFragmentRefTypes();
}
