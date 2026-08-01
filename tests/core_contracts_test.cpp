#include "core/CoreContracts.h"
#include "core/SysMeta.h"

#include <cstring>
#include <iostream>

namespace {

bool equal(const char* left, const char* right) {
    return std::strcmp(left, right) == 0;
}

} // namespace

int main() {
    using namespace luna::core_contracts;
    if (luna::sysmeta::SchemaMajor != 1 ||
        luna::sysmeta::SchemaMinor != 1 ||
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
    if (!equal(luna::sysmeta::DropTraitId,
               legacy_0_2::DropTraitId) ||
        !equal(luna::sysmeta::FromTraitId,
               legacy_0_2::FromTraitId)) {
        std::cerr << "0.2 compiler-trait compatibility identity changed\n";
        return 1;
    }
    if (equal(legacy_0_2::DropTraitId,
              canonical_0_3::DropTraitId) ||
        equal(legacy_0_2::FromTraitId,
              canonical_0_3::FromTraitId)) {
        std::cerr << "legacy and canonical identities were conflated\n";
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
    return 0;
}
