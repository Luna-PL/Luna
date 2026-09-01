#include "core/CoreContracts.h"
#include "core/SysMeta.h"
#include "core/TypeSystem.h"

#include <cstring>
#include <iostream>
#include <type_traits>

namespace {

bool equal(const char* left, const char* right) {
    return std::strcmp(left, right) == 0;
}

} // namespace

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
    return 0;
}
