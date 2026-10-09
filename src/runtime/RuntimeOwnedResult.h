#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runtime-internal candidate carrier. This is not a published Native export
// ABI. A handle is valid only in the unique cell to which Runtime bound it.
enum LunaRuntimeOwnedResultDropStatusV1 {
    LUNA_RUNTIME_OWNED_RESULT_DROP_SUCCESS_V1 = 0,
    LUNA_RUNTIME_OWNED_RESULT_DROP_EMPTY_V1 = 1,
    LUNA_RUNTIME_OWNED_RESULT_DROP_INVALID_HANDLE_V1 = 2,
};

typedef int32_t (*LunaRuntimeOwnedResultDropEntryV1)(void**);

enum LunaRuntimeOwnedResultAdoptStatusV1 {
    LUNA_RUNTIME_OWNED_RESULT_ADOPT_SUCCESS_V1 = 0,
    LUNA_RUNTIME_OWNED_RESULT_ADOPT_INVALID_OUTPUT_V1 = 1,
    LUNA_RUNTIME_OWNED_RESULT_ADOPT_INVALID_RESOURCE_V1 = 2,
    LUNA_RUNTIME_OWNED_RESULT_ADOPT_FAILED_V1 = 3,
};

// Internal compiler bridge. code_lease is a live Runtime-issued opaque token,
// never a pointer to a C++ object. Runtime copies the retained lease before
// publishing the owner token. The owner cell must stay at one address until
// Drop and be empty on entry. No arbitrary-pointer import is supported.
int32_t luna_runtime_owned_result_adopt_v1(
    void* payload, LunaRuntimeOwnedResultDropEntryV1 drop,
    const void* code_lease, void** owner_cell);

enum LunaRuntimeOwnedResultLeaseStatusV1 {
    LUNA_RUNTIME_OWNED_RESULT_LEASE_LIVE_V1 = 0,
    LUNA_RUNTIME_OWNED_RESULT_LEASE_INVALID_V1 = 1,
};

// Read-only preflight for a Runtime-issued lease token.
int32_t luna_runtime_owned_result_lease_check_v1(const void* code_lease);

int32_t luna_runtime_owned_result_drop_v1(void** owner_cell);

#ifdef __cplusplus
}

#include <memory>
#include <string>

namespace luna::runtime {

// Keeps a Runtime-issued code-lease token live for one host call. The token
// can cross the generated C boundary without exposing std::shared_ptr layout.
class RuntimeOwnedResultCodeLease {
public:
    RuntimeOwnedResultCodeLease() = default;
    RuntimeOwnedResultCodeLease(const RuntimeOwnedResultCodeLease&) = delete;
    RuntimeOwnedResultCodeLease& operator=(const RuntimeOwnedResultCodeLease&) = delete;
    RuntimeOwnedResultCodeLease(RuntimeOwnedResultCodeLease&& other) noexcept;
    RuntimeOwnedResultCodeLease& operator=(RuntimeOwnedResultCodeLease&& other) noexcept;
    ~RuntimeOwnedResultCodeLease();

    bool prepare(std::shared_ptr<const void> codeLease, std::string& error);
    const void* opaque() const noexcept {
        return reinterpret_cast<const void*>(token_);
    }
    explicit operator bool() const noexcept { return token_ != 0; }
    void reset() noexcept;

private:
    uintptr_t token_ = 0;
};

// Owns a stable, unique handle cell. The returned payload, exact generated
// Drop entry and code lease are retained as one Runtime lifetime unit.
class RuntimeOwnedResultHandle {
public:
    using DropEntry = LunaRuntimeOwnedResultDropEntryV1;

    RuntimeOwnedResultHandle() = default;
    RuntimeOwnedResultHandle(const RuntimeOwnedResultHandle&) = delete;
    RuntimeOwnedResultHandle& operator=(const RuntimeOwnedResultHandle&) = delete;
    RuntimeOwnedResultHandle(RuntimeOwnedResultHandle&&) noexcept = default;
    RuntimeOwnedResultHandle& operator=(RuntimeOwnedResultHandle&& other) noexcept;
    ~RuntimeOwnedResultHandle();

    explicit operator bool() const { return cell_ && *cell_; }
    const void* opaque() const { return cell_ ? *cell_ : nullptr; }
    void** cell() noexcept { return cell_.get(); }
    // Reserves a stable empty cell for a generated C entry before body call.
    bool prepareEmptyCell(std::string& error);
    int32_t dropOnce() noexcept;
    void reset() noexcept;

private:
    friend bool makeRuntimeOwnedResultHandle(
        void*, DropEntry, std::shared_ptr<const void>,
        RuntimeOwnedResultHandle&, std::string&);
    std::unique_ptr<void*> cell_;
};

// On success adopts payload and publishes into an empty stable cell. On
// failure payload remains with the caller and output is unchanged.
bool makeRuntimeOwnedResultHandle(
    void* payload, RuntimeOwnedResultHandle::DropEntry drop,
    std::shared_ptr<const void> codeLease,
    RuntimeOwnedResultHandle& output, std::string& error);

} // namespace luna::runtime
#endif
