#pragma once

#include <stddef.h>
#include <stdint.h>

// Stable, C-compatible host boundary used by generated Luna code and future
// MoonRuntime/module loaders.  This header describes contracts only; the
// default runtime implementation may use the platform C/C++ runtime behind
// the boundary.
#ifdef __cplusplus
extern "C" {
#endif

#define LUNA_RUNTIME_ABI_V1 1u
#define LUNA_HOST_SERVICES_MAGIC_V1 0x4c485331u /* "LHS1" */
#define LUNA_DEFAULT_HOST_ALIGNMENT ((size_t)16u)

enum LunaRuntimeStatusV1 {
    LUNA_RUNTIME_STATUS_OK = 0,
    LUNA_RUNTIME_STATUS_INVALID_ARGUMENT = -1,
    LUNA_RUNTIME_STATUS_UNSUPPORTED_ABI = -2,
    LUNA_RUNTIME_STATUS_ALREADY_ACTIVE = -3,
    LUNA_RUNTIME_STATUS_UNSUPPORTED_OPERATION = -4,
    LUNA_RUNTIME_STATUS_BUFFER_TOO_SMALL = -5,
    LUNA_RUNTIME_STATUS_IO_ERROR = -6,
    LUNA_RUNTIME_STATUS_ALLOCATION_ERROR = -7,
};

// Stable error identities for recoverable Runtime boundaries. Diagnostic text
// is deliberately not an identity and may be omitted by callers that cannot
// allocate storage for it.
enum LunaRuntimeErrorDomainV1 {
    LUNA_RUNTIME_ERROR_DOMAIN_NONE = 0,
    LUNA_RUNTIME_ERROR_DOMAIN_FRAGMENT_PLUGIN = 1,
    LUNA_RUNTIME_ERROR_DOMAIN_GPU = 2,
};

enum LunaRuntimeErrorCodeV1 {
    LUNA_RUNTIME_ERROR_NONE = 0,
    LUNA_RUNTIME_ERROR_UNKNOWN = 1,
    LUNA_RUNTIME_ERROR_INVALID_ARGUMENT = 2,
    LUNA_RUNTIME_ERROR_UNSUPPORTED_ABI = 3,
    LUNA_RUNTIME_ERROR_DYNAMIC_LIBRARY = 4,
    LUNA_RUNTIME_ERROR_MISSING_SYMBOL = 5,
    LUNA_RUNTIME_ERROR_INVALID_DESCRIPTOR = 6,
    LUNA_RUNTIME_ERROR_DUPLICATE_REGISTRATION = 7,
    LUNA_RUNTIME_ERROR_NOT_FOUND = 8,
    LUNA_RUNTIME_ERROR_INVALID_RESULT = 9,
    LUNA_RUNTIME_ERROR_BACKEND_UNAVAILABLE = 10,
    LUNA_RUNTIME_ERROR_BACKEND_OPERATION = 11,
    LUNA_RUNTIME_ERROR_INVALID_STATE = 12,
};

typedef struct LunaRuntimeErrorSnapshotV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t domain;
    int32_t code;
    // UTF-8 bytes excluding the trailing NUL. A zero value means that no
    // diagnostic text is available.
    uint64_t message_size;
} LunaRuntimeErrorSnapshotV1;

enum LunaHostCapabilityV1 {
    LUNA_HOST_CAP_ALLOCATOR = UINT64_C(1) << 0,
    LUNA_HOST_CAP_CONSOLE = UINT64_C(1) << 1,
    // Executable memory is intentionally optional. A MoonRuntime/JIT must not
    // assume it is available merely because ordinary allocation is present.
    LUNA_HOST_CAP_EXECUTABLE_MEMORY = UINT64_C(1) << 2,
    // Console input is an optional appended operation. Output-only v1 hosts
    // remain valid when this bit is absent.
    LUNA_HOST_CAP_CONSOLE_INPUT = UINT64_C(1) << 3,
    LUNA_HOST_CAP_FILESYSTEM = UINT64_C(1) << 4,
};

enum LunaConsoleStreamV1 {
    LUNA_CONSOLE_STDOUT = 1u,
    LUNA_CONSOLE_STDERR = 2u,
};

typedef void* (*LunaAllocateFnV1)(void* context, size_t size, size_t alignment);
typedef void* (*LunaReallocateFnV1)(void* context, void* pointer,
                                    size_t old_size, size_t new_size,
                                    size_t alignment);
typedef void (*LunaDeallocateFnV1)(void* context, void* pointer,
                                   size_t size, size_t alignment);

typedef struct LunaAllocatorV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;
    LunaAllocateFnV1 allocate;
    LunaReallocateFnV1 reallocate;
    LunaDeallocateFnV1 deallocate;
} LunaAllocatorV1;

enum LunaAllocErrorKindV1 {
    LUNA_ALLOC_ERROR_NONE = 0,
    LUNA_ALLOC_ERROR_INVALID_ALIGNMENT = 1,
    LUNA_ALLOC_ERROR_SIZE_OVERFLOW = 2,
    LUNA_ALLOC_ERROR_OUT_OF_MEMORY = 3,
};

typedef struct LunaAllocErrorV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t kind;
    uint32_t reserved_zero;
    uint64_t requested_size;
    uint64_t alignment;
} LunaAllocErrorV1;

// A positive-size reallocate failure must leave `pointer` allocated and
// unchanged. Success may return the same address or consume it and return a
// replacement. Safe callers use rt_try_realloc_v1 below; new_size == 0 is not
// a reallocation operation and must use rt_dealloc explicitly.

typedef int (*LunaConsoleWriteFnV1)(void* context, uint32_t stream,
                                    const char* bytes, size_t byte_count);
typedef int (*LunaConsoleFlushFnV1)(void* context, uint32_t stream);
struct LunaIoErrorV1;
typedef int (*LunaConsoleReadFnV1)(void* context, char* bytes,
                                   size_t byte_capacity,
                                   size_t* bytes_read,
                                   struct LunaIoErrorV1* error);

typedef struct LunaConsoleV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;
    LunaConsoleWriteFnV1 write;
    LunaConsoleFlushFnV1 flush;
    // Appended operation. Read only when struct_size covers this field and
    // LUNA_HOST_CAP_CONSOLE_INPUT is present.
    LunaConsoleReadFnV1 read;
} LunaConsoleV1;

#define LUNA_CONSOLE_V1_OUTPUT_SIZE offsetof(LunaConsoleV1, read)

typedef uint64_t LunaFileHandleV1;
#define LUNA_INVALID_FILE_HANDLE_V1 UINT64_C(0)

enum LunaIoErrorKindV1 {
    LUNA_IO_ERROR_NONE = 0,
    LUNA_IO_ERROR_NOT_FOUND = 1,
    LUNA_IO_ERROR_PERMISSION_DENIED = 2,
    LUNA_IO_ERROR_ALREADY_EXISTS = 3,
    LUNA_IO_ERROR_INVALID_INPUT = 4,
    LUNA_IO_ERROR_UNEXPECTED_EOF = 5,
    LUNA_IO_ERROR_INTERRUPTED = 6,
    LUNA_IO_ERROR_WOULD_BLOCK = 7,
    LUNA_IO_ERROR_UNSUPPORTED = 8,
    LUNA_IO_ERROR_OTHER = 9,
};

enum LunaIoOperationV1 {
    LUNA_IO_OPERATION_NONE = 0,
    LUNA_IO_OPERATION_OPEN = 1,
    LUNA_IO_OPERATION_READ = 2,
    LUNA_IO_OPERATION_WRITE = 3,
    LUNA_IO_OPERATION_SEEK = 4,
    LUNA_IO_OPERATION_FLUSH = 5,
    LUNA_IO_OPERATION_SYNC = 6,
    LUNA_IO_OPERATION_CLOSE = 7,
    LUNA_IO_OPERATION_METADATA = 8,
    LUNA_IO_OPERATION_REMOVE_FILE = 9,
    LUNA_IO_OPERATION_CREATE_DIRECTORY = 10,
};

typedef struct LunaIoErrorV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t kind;
    uint32_t operation;
    // Platform-native code captured at the failing call site. Zero means the
    // host has no additional numeric code.
    int64_t raw_code;
} LunaIoErrorV1;

// I/O callbacks return LUNA_RUNTIME_STATUS_OK for successful operations,
// including partial transfers and EOF (a zero-byte read). A recoverable host
// operation failure returns LUNA_RUNTIME_STATUS_IO_ERROR and initializes the
// caller-owned error record. Other statuses report an ABI/caller contract
// failure and need not initialize it. Transfer buffers may be null only when
// their byte count/capacity is zero; non-error out parameters are read by the
// caller only after success.

enum LunaFileOpenFlagV1 {
    LUNA_FILE_OPEN_READ = UINT32_C(1) << 0,
    LUNA_FILE_OPEN_WRITE = UINT32_C(1) << 1,
    LUNA_FILE_OPEN_APPEND = UINT32_C(1) << 2,
    LUNA_FILE_OPEN_TRUNCATE = UINT32_C(1) << 3,
    LUNA_FILE_OPEN_CREATE = UINT32_C(1) << 4,
    LUNA_FILE_OPEN_CREATE_NEW = UINT32_C(1) << 5,
};

// At least READ or WRITE is required. APPEND, TRUNCATE, CREATE, and
// CREATE_NEW require WRITE. CREATE_NEW implies creation and fails atomically
// when the path already exists; callers need not also set CREATE.

enum LunaSeekWhenceV1 {
    LUNA_SEEK_FROM_START = 0,
    LUNA_SEEK_FROM_CURRENT = 1,
    LUNA_SEEK_FROM_END = 2,
};

enum LunaFileTypeV1 {
    LUNA_FILE_TYPE_UNKNOWN = 0,
    LUNA_FILE_TYPE_REGULAR = 1,
    LUNA_FILE_TYPE_DIRECTORY = 2,
    LUNA_FILE_TYPE_SYMLINK = 3,
    LUNA_FILE_TYPE_OTHER = 4,
};

typedef struct LunaFileMetadataV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t file_type;
    uint32_t reserved_zero;
    uint64_t byte_size;
} LunaFileMetadataV1;

typedef int (*LunaFileOpenFnV1)(
    void* context, const char* path_utf8, size_t path_size,
    uint32_t flags, LunaFileHandleV1* handle, LunaIoErrorV1* error);
typedef int (*LunaFileReadFnV1)(
    void* context, LunaFileHandleV1 handle, void* bytes,
    size_t byte_capacity, size_t* bytes_read, LunaIoErrorV1* error);
typedef int (*LunaFileWriteFnV1)(
    void* context, LunaFileHandleV1 handle, const void* bytes,
    size_t byte_count, size_t* bytes_written, LunaIoErrorV1* error);
typedef int (*LunaFileSeekFnV1)(
    void* context, LunaFileHandleV1 handle, int64_t offset,
    uint32_t whence, uint64_t* position, LunaIoErrorV1* error);
typedef int (*LunaFileFlushFnV1)(
    void* context, LunaFileHandleV1 handle, LunaIoErrorV1* error);
typedef int (*LunaFileSyncFnV1)(
    void* context, LunaFileHandleV1 handle, LunaIoErrorV1* error);
typedef int (*LunaFileCloseFnV1)(
    void* context, LunaFileHandleV1 handle, LunaIoErrorV1* error);
typedef int (*LunaFileMetadataByHandleFnV1)(
    void* context, LunaFileHandleV1 handle,
    LunaFileMetadataV1* metadata, LunaIoErrorV1* error);
typedef int (*LunaPathMetadataFnV1)(
    void* context, const char* path_utf8, size_t path_size,
    LunaFileMetadataV1* metadata, LunaIoErrorV1* error);
typedef int (*LunaFileRemoveFnV1)(
    void* context, const char* path_utf8, size_t path_size,
    LunaIoErrorV1* error);
typedef int (*LunaDirectoryCreateFnV1)(
    void* context, const char* path_utf8, size_t path_size,
    LunaIoErrorV1* error);

typedef struct LunaFileSystemV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;
    LunaFileOpenFnV1 open;
    LunaFileReadFnV1 read;
    LunaFileWriteFnV1 write;
    LunaFileSeekFnV1 seek;
    LunaFileFlushFnV1 flush;
    LunaFileSyncFnV1 sync;
    LunaFileCloseFnV1 close;
    LunaFileMetadataByHandleFnV1 metadata;
    LunaPathMetadataFnV1 path_metadata;
    LunaFileRemoveFnV1 remove_file;
    LunaDirectoryCreateFnV1 create_directory;
} LunaFileSystemV1;

// Reserved W^X-capable interface for MoonRuntime/JIT code memory. `reserve`
// returns writable, non-executable memory; `seal` transitions it to executable
// and non-writable. The v1 runtime advertises this capability only when a host
// explicitly installs an implementation.
typedef void* (*LunaExecutableReserveFnV1)(void* context, size_t size,
                                           size_t alignment);
typedef int (*LunaExecutableSealFnV1)(void* context, void* pointer,
                                      size_t size);
typedef void (*LunaExecutableReleaseFnV1)(void* context, void* pointer,
                                          size_t size);

typedef struct LunaExecutableMemoryV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;
    LunaExecutableReserveFnV1 reserve;
    LunaExecutableSealFnV1 seal;
    LunaExecutableReleaseFnV1 release;
} LunaExecutableMemoryV1;

typedef struct LunaHostServicesV1 {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t reserved_zero;
    uint64_t capabilities;
    const LunaAllocatorV1* allocator;
    const LunaConsoleV1* console;
    const LunaExecutableMemoryV1* executable_memory;
    // Appended v1 field. Consumers must check struct_size and the filesystem
    // capability before reading it.
    const LunaFileSystemV1* filesystem;
} LunaHostServicesV1;

#define LUNA_HOST_SERVICES_V1_BASE_SIZE \
    offsetof(LunaHostServicesV1, filesystem)

// Foreign resources keep their originating release capability. They are not
// Luna heap allocations and must never be passed to rt_dealloc. This carrier
// is reserved for typed C-FFI adapters and Moon container/module boundaries.
typedef void (*LunaForeignReleaseFnV1)(void* context, void* resource,
                                       size_t size, size_t alignment);

typedef struct LunaOwnedForeignMemoryV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    void* data;
    size_t size;
    size_t alignment;
    void* release_context;
    LunaForeignReleaseFnV1 release;
    uint64_t allocator_domain;
} LunaOwnedForeignMemoryV1;

// Future dynamically loaded Moon modules receive this context instead of
// importing ambient process symbols. Fields may only be appended in v1 and
// consumers must gate reads with struct_size.
typedef struct LunaRuntimeModuleContextV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const LunaHostServicesV1* host_services;
    const char* module_id;
    uint64_t granted_capabilities;
} LunaRuntimeModuleContextV1;

// Process-wide host services. Installation is optional and must happen
// before the first allocation, console call, or explicit descriptor query.
int rt_install_host_services_v1(const LunaHostServicesV1* services);
// Installs the process-lifetime console-input/filesystem services used by an
// ordinary generated Luna application. It never replaces a host descriptor
// already installed by an embedding environment.
int rt_install_application_host_services_v1(void);
const LunaHostServicesV1* rt_host_services_v1(void);

// Stable forwarding entries for Sys bindings. They retain host authorization
// and opaque handle ownership inside Runtime instead of requiring Luna code to
// dereference C service-table layouts. A missing capability is reported as a
// recoverable LUNA_IO_ERROR_UNSUPPORTED.
int rt_console_write_v1(uint32_t stream, const void* bytes, size_t byte_count,
                        LunaIoErrorV1* error);
int rt_console_flush_v1(uint32_t stream, LunaIoErrorV1* error);
int rt_console_read_v1(void* bytes, size_t byte_capacity,
                       size_t* bytes_read, LunaIoErrorV1* error);
int rt_file_open_v1(const char* path_utf8, size_t path_size, uint32_t flags,
                    LunaFileHandleV1* handle, LunaIoErrorV1* error);
int rt_file_read_v1(LunaFileHandleV1 handle, void* bytes,
                    size_t byte_capacity, size_t* bytes_read,
                    LunaIoErrorV1* error);
int rt_file_write_v1(LunaFileHandleV1 handle, const void* bytes,
                     size_t byte_count, size_t* bytes_written,
                     LunaIoErrorV1* error);
int rt_file_seek_v1(LunaFileHandleV1 handle, int64_t offset,
                    uint32_t whence, uint64_t* position,
                    LunaIoErrorV1* error);
int rt_file_flush_v1(LunaFileHandleV1 handle, LunaIoErrorV1* error);
int rt_file_sync_v1(LunaFileHandleV1 handle, LunaIoErrorV1* error);
int rt_file_close_v1(LunaFileHandleV1 handle, LunaIoErrorV1* error);
int rt_file_metadata_v1(LunaFileHandleV1 handle,
                        LunaFileMetadataV1* metadata, LunaIoErrorV1* error);
int rt_path_metadata_v1(const char* path_utf8, size_t path_size,
                        LunaFileMetadataV1* metadata, LunaIoErrorV1* error);
int rt_remove_file_v1(const char* path_utf8, size_t path_size,
                      LunaIoErrorV1* error);
int rt_create_directory_v1(const char* path_utf8, size_t path_size,
                           LunaIoErrorV1* error);

// Fallible Global Luna allocator boundary used by owning standard-library
// containers. Errors are caller-owned and never allocate. A zero-size alloc
// succeeds with a null output without calling the host allocator. Realloc of
// a positive-size allocation is transactional: ALLOCATION_ERROR writes the
// original pointer back to `replacement`; new_size == 0 is INVALID_ARGUMENT.
int rt_checked_array_layout_v1(size_t element_size, size_t element_count,
                               size_t alignment, size_t* byte_size,
                               LunaAllocErrorV1* error);
int rt_try_alloc_v1(size_t size, size_t alignment, void** allocation,
                    LunaAllocErrorV1* error);
int rt_try_realloc_v1(void* pointer, size_t old_size, size_t new_size,
                      size_t alignment, void** replacement,
                      LunaAllocErrorV1* error);

// Copies the most recent error for `domain` into caller-owned storage. The
// snapshot metadata is always written when `snapshot` and `domain` are valid.
// `message_capacity` includes room for the trailing NUL. A null/zero buffer is
// a size query; BUFFER_TOO_SMALL reports a truncated or omitted message while
// preserving domain, code, and the complete required message_size.
int rt_runtime_error_snapshot_v1(uint32_t domain,
                                 LunaRuntimeErrorSnapshotV1* snapshot,
                                 char* message, size_t message_capacity);

void* rt_alloc(size_t size, size_t alignment);
void* rt_realloc(void* pointer, size_t old_size, size_t new_size,
                 size_t alignment);
void rt_dealloc(void* pointer, size_t size, size_t alignment);
void* rt_rc_alloc(size_t size, size_t alignment);
void rt_rc_retain(void* pointer);
int32_t rt_rc_release(void* pointer);
void* rt_arc_alloc(size_t size, size_t alignment);
void rt_arc_retain(void* pointer);
int32_t rt_arc_release(void* pointer);
void rt_shared_dealloc(void* pointer);
void rt_panic_cstr(const char* message);

#ifdef __cplusplus
}
#endif
