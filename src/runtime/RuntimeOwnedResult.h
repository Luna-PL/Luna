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

int32_t luna_runtime_owned_result_drop_v1(void** owner_cell);

#ifdef __cplusplus
}

#include <memory>
#include <string>

namespace luna::runtime {

// Owns a stable, unique handle cell. The returned payload, exact generated
// Drop entry and code lease are retained as one Runtime lifetime unit.
class RuntimeOwnedResultHandle {
public:
    using DropEntry = int32_t (*)(void**);

    RuntimeOwnedResultHandle() = default;
    RuntimeOwnedResultHandle(const RuntimeOwnedResultHandle&) = delete;
    RuntimeOwnedResultHandle& operator=(const RuntimeOwnedResultHandle&) = delete;
    RuntimeOwnedResultHandle(RuntimeOwnedResultHandle&&) noexcept = default;
    RuntimeOwnedResultHandle& operator=(RuntimeOwnedResultHandle&& other) noexcept;
    ~RuntimeOwnedResultHandle();

    explicit operator bool() const { return cell_ && *cell_; }
    const void* opaque() const { return cell_ ? *cell_ : nullptr; }
    void** cell() noexcept { return cell_.get(); }
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
