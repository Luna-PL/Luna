# Runtime ABI v1 and Allocation Domains

Starting with Luna 0.2.0-alpha, language facilities are separated from raw C FFI. Compiler-
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

The compiler carries the same exact layout through allocation and every cleanup path, so a
custom allocator does not need a hidden header on each object. `rt_malloc/rt_free` remain
compatibility entries for already-generated Alpha IR; new IR does not use them.

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

## Four resource domains that must not be mixed

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
check `struct_size`. `LunaRuntimeModuleContextV1` is reserved for verified Moon
containers and a future plugin ABI v2, allowing dynamic modules to use an authorized
service table rather than relying on accidentally visible process C symbols. The current
external fragment ABI v1 remains unchanged and will not be extended silently.

## Pay only for what is used

- Static code without `new`, cleanup, or `print` generates no corresponding runtime call.
- An ordinary allocation is one fixed `rt_alloc` boundary; indirect calls through a
  replaceable service table happen inside Runtime, not as an embedded capability/header in
  every language object.
- Executable memory, GPU, dynamic selection, and dynamic apply are independent capabilities;
  linking `libruntime` does not enable them automatically.
