#pragma once

#include "../core/TypeSystem.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace luna::selector {

enum class Retention : uint8_t {
    CompileTime,
    Runtime,
    Dynamic,
};

using MetadataValue = std::variant<int64_t, double, bool, std::string>;

struct Metadata {
    std::string schemaId;
    std::vector<MetadataValue> values;
    Retention retention = Retention::CompileTime;
};

struct Candidate {
    std::string declarationId;
    std::string familyId;
    TypePtr callableType;
    Retention retention = Retention::CompileTime;
    std::vector<Metadata> metadata;
};

// Compiler-owned, immutable view injected as a selector function's hidden
// first argument. It never transfers declaration ownership to user code.
class DeclarationView {
public:
    explicit DeclarationView(std::vector<Candidate> candidates = {});

    const std::vector<Candidate>& candidates() const { return mCandidates; }
    const Candidate* find(const std::string& declarationId) const;
    const std::string& familyId() const { return mFamilyId; }
    bool valid() const { return mValid; }
    const std::string& error() const { return mError; }

private:
    std::vector<Candidate> mCandidates;
    std::string mFamilyId;
    bool mValid = true;
    std::string mError;
};

enum class ResultKind {
    Unique,
    NoMatch,
    Ambiguous,
    InvalidCandidate,
    InvalidView,
};

struct Result {
    ResultKind kind = ResultKind::NoMatch;
    std::optional<Candidate> selected;
    std::string message;

    bool success() const { return kind == ResultKind::Unique && selected.has_value(); }
};

struct DynamicPlan {
    std::string familyId;
    std::vector<std::string> candidateIds;
    std::string selectorDeclarationId;
};

class Engine {
public:
    // The ordinary selector function returns declaration identities. Engine
    // applies the compiler-owned safety rules and accepts exactly one member
    // of the injected view.
    Result validate(const DeclarationView& view,
                    const std::vector<std::string>& returnedIds) const;

    std::optional<DynamicPlan> planDynamic(
        const DeclarationView& view,
        const std::string& selectorDeclarationId,
        std::string& error) const;
};

} // namespace luna::selector
