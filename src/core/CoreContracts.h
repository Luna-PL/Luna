#pragma once

// Compiler-known standard-library identities live here rather than being
// scattered through Sema. Luna 0.2 keeps its two legacy compiler traits for
// source compatibility; an explicit 0.3 language mode will select the
// canonical Core identities below.
namespace luna::core_contracts {

inline constexpr const char* PackageId = "org.luna.core";

namespace legacy_0_2 {
inline constexpr const char* DropTraitId = "luna.compiler.Drop";
inline constexpr const char* FromTraitId = "luna.compiler.From";
inline constexpr const char* ResultTypeId = "luna.compiler.Result";
} // namespace legacy_0_2

namespace canonical_0_3 {
inline constexpr const char* DropTraitId =
    "org.luna.core::resource::Drop";
inline constexpr const char* CloneTraitId =
    "org.luna.core::resource::Clone";
inline constexpr const char* FromTraitId =
    "org.luna.core::convert::From";
inline constexpr const char* TryFromTraitId =
    "org.luna.core::convert::TryFrom";
inline constexpr const char* OptionTypeId =
    "org.luna.core::option::Option";
inline constexpr const char* ResultTypeId =
    "org.luna.core::result::Result";
inline constexpr const char* IteratorTraitId =
    "org.luna.core::iter::Iterator";
inline constexpr const char* IntoIteratorTraitId =
    "org.luna.core::iter::IntoIterator";
inline constexpr const char* FromIteratorTraitId =
    "org.luna.core::iter::FromIterator";
inline constexpr const char* TryFromIteratorTraitId =
    "org.luna.core::iter::TryFromIterator";
} // namespace canonical_0_3

inline constexpr const char* DropMethodName = "drop";
inline constexpr const char* CloneMethodName = "clone";
inline constexpr const char* FromMethodName = "from";
inline constexpr const char* TryFromMethodName = "try_from";
inline constexpr const char* IteratorNextMethodName = "next";
inline constexpr const char* IntoIteratorMethodName = "into_iter";
inline constexpr const char* FromIteratorBeginMethodName = "begin";
inline constexpr const char* FromIteratorPushMethodName = "push";
inline constexpr const char* FromIteratorFinishMethodName = "finish";

inline constexpr const char* GlobalAllocatorDomainId =
    "org.luna.alloc::global::Global";

} // namespace luna::core_contracts
