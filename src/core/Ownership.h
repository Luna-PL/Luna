#pragma once

#include <cstdint>
#include <string_view>

namespace luna::ownership {

// Ownership answers who is responsible for the value. Usage answers how many
// times that responsibility may be consumed. They are deliberately separate:
// `move` is a state transition, not a third ownership or usage category.
enum class Relation : uint8_t {
    Owned,
    SharedBorrow,
    MutableBorrow,
};

enum class Usage : uint8_t {
    Copy,
    Affine,
    Linear,
};

struct Contract {
    Relation relation = Relation::Owned;
    Usage usage = Usage::Copy;

    bool operator==(const Contract& other) const {
        return relation == other.relation && usage == other.usage;
    }
    bool operator!=(const Contract& other) const { return !(*this == other); }
};

inline constexpr bool isBorrowed(Relation relation) {
    return relation != Relation::Owned;
}

inline constexpr bool isMoveOnly(Usage usage) {
    return usage == Usage::Affine || usage == Usage::Linear;
}

inline constexpr bool mustConsume(Usage usage) {
    return usage == Usage::Linear;
}

inline constexpr std::string_view relationName(Relation relation) {
    switch (relation) {
        case Relation::Owned: return "owned";
        case Relation::SharedBorrow: return "shared_borrow";
        case Relation::MutableBorrow: return "mutable_borrow";
    }
    return "invalid";
}

inline constexpr std::string_view usageName(Usage usage) {
    switch (usage) {
        case Usage::Copy: return "copy";
        case Usage::Affine: return "affine";
        case Usage::Linear: return "linear";
    }
    return "invalid";
}

enum class CleanupAction : uint8_t {
    Drop,
    Deallocate,
};

inline constexpr std::string_view cleanupActionName(CleanupAction action) {
    switch (action) {
        case CleanupAction::Drop: return "drop";
        case CleanupAction::Deallocate: return "deallocate";
    }
    return "invalid";
}

} // namespace luna::ownership
