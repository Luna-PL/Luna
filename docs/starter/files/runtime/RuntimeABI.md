# src/runtime/RuntimeABI.h

The core contract header for the Luna runtime C ABI. It defines all struct types — host services descriptor, allocator, console, file system, executable memory, error snapshot, and foreign memory — along with every status code enum and the stable `rt_*` forwarding function declarations.

## What This File Does

- Defines the Luna runtime ABI version constant `LUNA_RUNTIME_ABI_V1` (value 1) and the host services magic number `LUNA_HOST_SERVICES_MAGIC_V1` (`"LHS1"`, i.e. 0x4c485331).
- Defines the host capability enum `LunaHostCapabilityV1` with five capability bits: allocator, console output, executable memory, console input, and file system.
- Defines all service table structs: `LunaAllocatorV1`, `LunaConsoleV1`, `LunaFileSystemV1`, `LunaExecutableMemoryV1`, and `LunaHostServicesV1`.
- Defines error-related structs and enums: `LunaRuntimeStatusV1`, `LunaRuntimeErrorDomainV1`, `LunaRuntimeErrorCodeV1`, `LunaRuntimeErrorSnapshotV1`, `LunaAllocErrorV1`, and `LunaIoErrorV1`.
- Defines file-I/O-related enums and structs: `LunaFileOpenFlagV1`, `LunaSeekWhenceV1`, `LunaFileTypeV1`, `LunaFileMetadataV1`, and `LunaFileHandleV1`.
- Defines the foreign-memory ownership wrapper `LunaOwnedForeignMemoryV1` and the module context `LunaRuntimeModuleContextV1`.
- Declares all `rt_*` forwarding functions for generated code to call directly.

## Key Structs, Classes, and Enums

### Host Services Aggregate

```c
typedef struct LunaHostServicesV1 {
    uint32_t magic;              // LUNA_HOST_SERVICES_MAGIC_V1
    uint32_t abi_version;        // LUNA_RUNTIME_ABI_V1
    uint32_t struct_size;        // >= LUNA_HOST_SERVICES_V1_BASE_SIZE
    uint32_t reserved_zero;      // must be 0
    uint64_t capabilities;       // capability bitmask
    const LunaAllocatorV1* allocator;
    const LunaConsoleV1* console;
    const LunaExecutableMemoryV1* executable_memory;  // nullable
    const LunaFileSystemV1* filesystem;                // nullable (field added in v1)
} LunaHostServicesV1;
```

Across the entire Luna runtime, this serves the role of a C++ "dependency injection container" — the host passes the implementations of infrastructure such as the allocator, console, and file system through it.

### Subservice Tables

| Struct | Purpose | Key function pointers |
|---|---|---|
| `LunaAllocatorV1` | Memory allocator interface | `allocate` / `reallocate` / `deallocate` |
| `LunaConsoleV1` | Console I/O interface | `write` / `flush` / `read` (optional addition) |
| `LunaFileSystemV1` | File system interface (11 function pointers) | `open` / `read` / `write` / `seek` / `flush` / `sync` / `close` / `metadata` / `path_metadata` / `remove_file` / `create_directory` |
| `LunaExecutableMemoryV1` | Executable memory W^X interface (for JIT) | `reserve` / `seal` / `release` |

### Error Enums

| Enum | Value range | Purpose |
|---|---|---|
| `LunaRuntimeStatusV1` | 0, -1 to -7 | Function return values: OK, INVALID_ARGUMENT, UNSUPPORTED_ABI, ALREADY_ACTIVE, UNSUPPORTED_OPERATION, BUFFER_TOO_SMALL, IO_ERROR, ALLOCATION_ERROR |
| `LunaRuntimeErrorDomainV1` | 0, 1, 2 | Error domains: NONE, FRAGMENT_PLUGIN, GPU |
| `LunaRuntimeErrorCodeV1` | 0 to 12 | Error codes (NONE, UNKNOWN, INVALID_ARGUMENT, etc.) |
| `LunaAllocErrorKindV1` | 0 to 3 | Allocation error kinds: NONE, INVALID_ALIGNMENT, SIZE_OVERFLOW, OUT_OF_MEMORY |
| `LunaIoErrorKindV1` | 0 to 9 | I/O error kinds: NONE, NOT_FOUND, PERMISSION_DENIED, etc. |
| `LunaIoOperationV1` | 0 to 10 | Which I/O operation the error occurred on (OPEN, READ, WRITE, etc.) |

### File I/O

| Type | Purpose |
|---|---|
| `LunaFileHandleV1` | `uint64_t`, an opaque file handle; `LUNA_INVALID_FILE_HANDLE_V1` (0) denotes invalid |
| `LunaFileOpenFlagV1` | Open-flag bit enum: READ, WRITE, APPEND, TRUNCATE, CREATE, CREATE_NEW |
| `LunaSeekWhenceV1` | Seek origin: FROM_START, FROM_CURRENT, FROM_END |
| `LunaFileTypeV1` | File types: UNKNOWN, REGULAR, DIRECTORY, SYMLINK, OTHER |
| `LunaFileMetadataV1` | File metadata: file_type, byte_size |

### Other Wrapper Types

| Struct | Purpose | C++ analogue |
|---|---|---|
| `LunaOwnedForeignMemoryV1` | Owned non-Luna-heap memory with a release callback | Similar to `std::unique_ptr<void, CustomDeleter>` |
| `LunaRuntimeModuleContextV1` | Context for dynamically loaded Moon modules | A dependency-injected runtime context |
| `LunaForeignReleaseFnV1` | Foreign-resource release callback function pointer | Similar to `std::function<void(void*, void*, size_t, size_t)>` |

## Key Functions and Methods

### Host Services Installation

| Function | Purpose |
|---|---|
| `rt_install_host_services_v1` | Installs a custom host services descriptor; must be the first runtime call |
| `rt_install_application_host_services_v1` | Installs application-level services (console input + file system) |
| `rt_host_services_v1` | Retrieves the currently installed host services pointer |

### Console and Filesystem Forwarding

| Function | Purpose |
|---|---|
| `rt_console_write_v1` / `rt_console_flush_v1` / `rt_console_read_v1` | Console I/O forwarding; returns `LUNA_IO_ERROR_UNSUPPORTED` when the capability is absent |
| `rt_file_open_v1` / `rt_file_read_v1` / `rt_file_write_v1` / `rt_file_seek_v1` / `rt_file_flush_v1` / `rt_file_sync_v1` / `rt_file_close_v1` | File I/O forwarding |
| `rt_file_metadata_v1` / `rt_path_metadata_v1` | File metadata queries |
| `rt_remove_file_v1` / `rt_create_directory_v1` | File/directory operations |

### Allocator

| Function | Purpose |
|---|---|
| `rt_checked_array_layout_v1` | Computes array layout (element size × element count + alignment); returns `ALLOCATION_ERROR` on overflow |
| `rt_try_alloc_v1` | Fallible allocation; a zero size returns null without invoking the host allocator |
| `rt_try_realloc_v1` | Transactional reallocation: on failure the original pointer is left unchanged; `new_size == 0` returns `INVALID_ARGUMENT` |
| `rt_runtime_error_snapshot_v1` | Copies the latest error snapshot for the given domain |

### Reference-Counted Allocation

| Function | Purpose |
|---|---|
| `rt_rc_allocate_v1` / `rt_rc_retain_v1` / `rt_rc_release_v1` | Non-atomic reference counting, for single-threaded use |
| `rt_arc_allocate_v1` / `rt_arc_retain_v1` / `rt_arc_release_v1` | Atomic reference counting, for cross-thread use |
| `rt_panic_cstr` | Unrecoverable error termination |

## Relationship to Surrounding Files and Pipeline Stages

- **Runtime.h** — Declares function signatures with the same struct types as in `RuntimeABI.h`, but `RuntimeABI.h` additionally declares all `rt_*` forwarding functions, while `Runtime.h` declares the internal implementation functions.
- **Runtime.cpp** — Uses the types and constants defined in `RuntimeABI.h` and implements all `rt_*` functions.
- **ApplicationHostServices.h** — Returns `LunaConsoleV1*` and `LunaFileSystemV1*`, whose type definitions originate from this file.
- **FragmentPluginABI.h** — A separate fragment-plugin ABI that, together with this file, forms the complete runtime ABI contract.
- This file is the foundational dependency of all other runtime files; any file that implements runtime behavior must include it.

## Further Reading

- Complete declarations of the C ABI entry points in `Runtime.h`
- Default implementations of each type from this file in `Runtime.cpp`
- Concrete platform adaptation of the file system and console in `ApplicationHostServices.cpp`
- The fragment-plugin ABI specification in `FragmentPluginABI.h`


---
