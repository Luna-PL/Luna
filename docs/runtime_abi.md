# Runtime ABI v1 and Allocation Domains

Starting with Luna 0.2.1, language facilities are separated from raw C FFI. Compiler-
generated `new`, path-sensitive automatic cleanup, explicit `free`, and language `print`
call only the Luna Runtime ABI; they no longer resolve `malloc/free/printf` directly. The
public C-compatible header is `runtime/RuntimeABI.h`.

## Design boundary

The Runtime ABI is an allocation contract, not a fixed allocation algorithm. The default
implementation may use the platform C/C++ runtime. An embedded host may install its own
allocator and console before the first runtime service call with
`rt_install_host_services_v1`. The installed descriptor and its nested service tables must
remain valid until process exit.

New generated allocation calls are:

```c
void* rt_alloc(size_t size, size_t alignment);
void* rt_realloc(void* pointer, size_t old_size, size_t new_size,
                 size_t alignment);
void  rt_dealloc(void* pointer, size_t size, size_t alignment);
```

Owning library containers use the recoverable companion boundary instead:

```c
int rt_checked_array_layout_v1(size_t element_size, size_t element_count,
                               size_t alignment, size_t* byte_size,
                               LunaAllocErrorV1* error);
int rt_try_alloc_v1(size_t size, size_t alignment, void** allocation,
                    LunaAllocErrorV1* error);
int rt_try_realloc_v1(void* pointer, size_t old_size, size_t new_size,
                      size_t alignment, void** replacement,
                      LunaAllocErrorV1* error);
```

These entries report invalid alignment, array-size overflow, and out-of-memory through a
caller-owned, allocation-free `LunaAllocErrorV1`. A zero-size allocation succeeds with a
null output and does not call the host allocator. Positive-size reallocation is
transactional: on allocation failure the original allocation remains valid and is written
back to `replacement`. `new_size == 0` is rejected; the caller must destroy initialized
elements and call `rt_dealloc` explicitly. `org.luna.sys::alloc` contains the raw Luna FFI
bridge. Mapping this record into `core::AllocError` belongs in the future safe Alloc adapter.

The compiler carries the same exact layout through allocation and every cleanup path, so a
custom allocator does not need a hidden header on each object. `rt_malloc/rt_free` remain
compatibility entries for already-generated Alpha IR; new IR does not use them.

## Core shared cells

Ordinary Core `Rc<T>`/`Arc<T>` implement counting through these Runtime ABI v1 entries:

```c
typedef void (*LunaDropCallbackV1)(void* value_storage);
void* rt_rc_allocate_v1(int32_t size, int32_t alignment, LunaDropCallbackV1 drop);
void  rt_rc_retain_v1(void* pointer);
void  rt_rc_release_v1(void* pointer);
void* rt_arc_allocate_v1(int32_t size, int32_t alignment, LunaDropCallbackV1 drop);
void  rt_arc_retain_v1(void* pointer);
void  rt_arc_release_v1(void* pointer);
```

Rc counts are non-atomic. Arc retain is relaxed atomic; the final release uses
acquire-release, invokes `drop` exactly once, and then returns the whole shared cell to the
same Luna allocator domain. The callback destroys only the initialized payload and does not
free the surrounding cell. These calls are a low-level Core-library boundary; the compiler
has no Rc/Arc TypeKind or dedicated cleanup node.

Non-recoverable errors call `rt_panic_cstr`. This writes the diagnostic to stderr through
the installed console, flushes, and aborts; it does not unwind the language stack or perform
local Drop. Recoverable errors should use `Result<T, E>`; generated code performs
path-sensitive cleanup before returning early.

## Recoverable error snapshots

Runtime ABI v1 exposes recoverable boundary errors through
`rt_runtime_error_snapshot_v1`. The caller selects
`LUNA_RUNTIME_ERROR_DOMAIN_FRAGMENT_PLUGIN` or
`LUNA_RUNTIME_ERROR_DOMAIN_GPU` and receives stable `domain/code` fields plus optional
UTF-8 diagnostic text:

```c
LunaRuntimeErrorSnapshotV1 snapshot;
int status = rt_runtime_error_snapshot_v1(
    LUNA_RUNTIME_ERROR_DOMAIN_GPU, &snapshot, NULL, 0);
```

An empty buffer queries `message_size`. When diagnostic text exists, the call returns
`LUNA_RUNTIME_STATUS_BUFFER_TOO_SMALL`. A second call can copy the NUL-terminated text
when the adapter supplies `message_size + 1` bytes. Even when the buffer is too small,
the call writes complete `domain/code/message_size` fields and leaves safely truncated,
NUL-terminated text in a non-empty buffer.

Error identity is determined only by `domain/code`; text is diagnostic. Snapshot operations
do not allocate. If a safe adapter cannot allocate an owned message, it must preserve the
machine fields and omit text, not panic, and must not store the volatile pointer returned by
`rt_gpu_last_error` or `rt_fragment_plugin_last_error` in a long-lived value. The two
legacy `last_error` entries remain for Alpha compatibility; new adapters should use the
snapshot interface and copy immediately after a failing call.

## Console input and filesystem services

Runtime ABI v1 extends its original output-only console and host-services structures only
at their tails. `LUNA_CONSOLE_V1_OUTPUT_SIZE` and
`LUNA_HOST_SERVICES_V1_BASE_SIZE` name the previously published prefixes. A host that
advertises only the older capabilities may continue to pass those prefix sizes; a host that
advertises `LUNA_HOST_CAP_CONSOLE_INPUT` or `LUNA_HOST_CAP_FILESYSTEM` must provide the
complete extended table. The default runtime currently advertises neither new capability.
Compiler-generated application `main` functions explicitly call
`rt_install_application_host_services_v1` before other Runtime operations. That application
profile adds native stdin and filesystem services for ordinary JIT and AOT executables. It
does not replace a service table already supplied by an embedding host; a library/module with
no application entry point does not acquire the capabilities implicitly.

Console input uses the same `LunaConsoleV1` table as output. `read` may complete with fewer
bytes than requested; success with `bytes_read == 0` is EOF. A recoverable operation failure
returns `LUNA_RUNTIME_STATUS_IO_ERROR` and fills the caller-owned `LunaIoErrorV1`. The
callback does not allocate or return a process-global diagnostic pointer.

`LunaFileSystemV1` provides synchronous open/read/write/seek/flush/sync/close/metadata and
basic path operations. Its contract is:

- paths are valid UTF-8 pointer-plus-length views and are not NUL-terminated C strings;
- an adapter that calls an API requiring a C string rejects embedded NUL as `INVALID_INPUT`;
- handles are opaque unsigned values, and zero is always invalid;
- successful read/write may be partial, and a successful zero-byte read is EOF;
- a recoverable host failure returns `LUNA_RUNTIME_STATUS_IO_ERROR` and initializes the
  supplied `LunaIoErrorV1`; error identity is `kind/operation/raw_code`, with no mandatory
  allocated message; other Runtime statuses denote ABI/caller contract failures;
- out parameters other than the error record are consumed only after a successful status;
- the host owns handle implementation details, while the safe Std adapter owns close policy.

The native application profile validates UTF-8 and embedded NUL before path conversion,
uses opaque registry IDs rather than exposing native descriptors, and consumes a handle on
the first close attempt even if the platform close reports an error. `flush` drains only
library buffering; the descriptor-based application profile has none. `sync` is the explicit
durability operation.

Sys bindings call the fixed `rt_console_*_v1`, `rt_file_*_v1`, and
`rt_path_metadata_v1` forwarding entries. They do not load or dereference the host tables
themselves. Runtime checks the installed capability on each forwarding call; absence becomes
an allocation-free `LUNA_IO_ERROR_UNSUPPORTED`, while authorized calls retain the host's
opaque context and handle domain.

Handle metadata and path metadata are separate operations; a safe `File::metadata` never
depends on retaining the path used to open the file. The raw ABI deliberately does not promise
`read_exact`, `write_all`, text decoding, recursive directory creation, or best-effort Drop.
Those policies belong in Std and are built by
repeating these partial operations while handling `INTERRUPTED` explicitly.

The non-v1 `rt_compat_console_*_0_2` helpers are an intentionally temporary adapter for the
0.2.1 `std::io` module. They provide cstr/i32 formatting and bounded line input without
freezing the future owned String or formatting traits. They are declared in `Runtime.h`, not
the stable public `RuntimeABI.h`, and are removed by the one-time 0.3 implementation switch.

## Five resource domains that must not be mixed

1. **Luna host heap**: uses `rt_alloc/rt_dealloc`; layout is determined by MoonIR/compiler.
2. **Foreign/C resources**: released by the capability supplied by the creating library.
   `LunaOwnedForeignMemoryV1` is reserved for a future typed C FFI adapter; these pointers
   must not be passed to `rt_dealloc`.
3. **Device resources**: use `rt_gpu_*` and the backend's address space/release protocol.
   The CPU simulator is only a host-backed implementation of the same device ABI; it does
   not turn a device pointer into a Luna host-heap pointer.
4. **Executable memory**: `LunaExecutableMemoryV1` reserves a W^X contract of
   `reserve -> write -> seal -> execute -> release`. The default runtime does not declare
   this capability; a future MoonRuntime/hotspot JIT must obtain explicit host authorization.
5. **Host service handles**: filesystem and later process/clock resources are opaque values
   created and released by the same installed host service. They are neither Luna pointers nor
   raw C resources that user code may close through an unrelated API.

## C FFI boundary

The Runtime ABI serves compiler-generated language operations; user-declared raw C
interfaces use explicit `extern "C"`, and the two must not be mixed:

```luna
extern "C" fn puts(message: cstr) -> i32;
extern "C" fn malloc(size: usize) -> linear raw<u8>;
extern "C" fn c_free(linear pointer: raw<u8>) as "free";
```

Exports use `export "C" fn`. An `extern` declaration cannot also be exported and cannot
be generic or `constexpr`. The current C ABI accepts only integers, floats, `cstr`,
`raw<T>`, `unit`, and references to those scalars; `string`, ADTs, closures, trait
objects, `device_buffer<T>`, and `Result<T, E>` cannot cross the boundary directly.

Every parameter must have an explicit type. Ownership transfer uses a `linear` parameter
and a `move` call; an owned return from a foreign allocator is written
`linear raw<T>`. A foreign pointer must be returned to the domain that created it and must
not be passed to Luna `free`/`rt_dealloc`. The declaration author currently owns the
pairing responsibility; a future safe adapter may use the release capability of
`LunaOwnedForeignMemoryV1`.

A recoverable foreign API should preserve its raw status/out-parameter declaration, capture
errno/status and the diagnostic snapshot immediately in an ordinary Luna adapter, then
return `Result<T, FfiError>`. The compiler does not read errno implicitly or extend the
lifetime of a foreign `last_error` pointer. JIT resolves user C symbols through the host
process, while AOT uses the system linker; both paths have `puts`/`free` regressions.

## Versioning and future modules

`LunaHostServicesV1` is validated with `magic`, `abi_version`, `struct_size`, and
capability bits. v1 may append fields only at the end of the structure, and consumers must
check the minimum prefix needed by every advertised capability rather than requiring the
latest known structure size. Nested tables follow the same rule. `LunaRuntimeModuleContextV1`
is reserved for verified Moon
containers and a future plugin ABI v2, allowing dynamic modules to use an authorized
service table rather than relying on accidentally visible process C symbols. The current
external fragment ABI v1 remains unchanged and will not be extended silently.

## Pay only for what is used

- Static code without `new`, cleanup, or `print` generates no corresponding runtime call.
- An ordinary allocation is one fixed `rt_alloc` boundary. The default allocator takes a
  direct Runtime fast path; an installed host allocator uses the replaceable service table.
  Neither path embeds a capability or hidden allocation header in every language object.
- Recoverable containers use `rt_try_alloc_v1`/`rt_try_realloc_v1`; failure neither aborts
  nor consumes an existing positive-size allocation.
- Executable memory, GPU, dynamic selection, and dynamic apply are independent capabilities;
  linking `libruntime` does not enable them automatically.
