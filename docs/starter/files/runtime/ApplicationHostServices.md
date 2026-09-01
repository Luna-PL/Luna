# src/runtime/ApplicationHostServices.cpp

Complete implementation of Luna's application-level host services. Following the two factory functions declared in `ApplicationHostServices.h`, it implements console input (stdin reading) and a cross-platform file system (POSIX + Windows native APIs).

## What This File Does

- Implements `lunaApplicationConsoleV1()` — returns a `LunaConsoleV1` constant containing stdin console input capability.
- Implements `lunaApplicationFileSystemV1()` — returns a complete 11-function file system `LunaFileSystemV1` constant.
- Manages the mapping from file descriptors to `LunaFileHandleV1` (`uint64_t`) (the `FileRegistry` class, thread-safe).
- Unifies and wraps POSIX (`open`/`read`/`write`/`lseek`/`fsync`/`close`/`fstat`/`lstat`/`unlink`/`mkdir`) and Windows (`_wopen`/`_read`/`_write`/`_lseeki64`/`_commit`/`_close`/`_fstat64`/`_wstat64`/`_wunlink`/`_wmkdir`) through platform-adaptation functions such as `nativePath`, `nativeOpen`, `nativeRead`, etc.
- Maps platform-native `errno` error codes to Luna `LunaIoErrorKindV1` enum values.
- Validates UTF-8 path legality (the `validUtf8` function, which manually scans UTF-8 sequences and checks for overlong encodings, surrogate pairs, out-of-range code points, etc.).

## Key Structs, Classes, and Enums

### `FileRegistry` (anonymous-namespace class)

| Member | Type | Purpose |
|---|---|---|
| `mMutex` | `std::mutex` | Ensures thread safety of `insert`, `withDescriptor`, `take` |
| `mDescriptors` | `std::unordered_map<LunaFileHandleV1, int>` | Luna handle → native file descriptor mapping |
| `mNextHandle` | `LunaFileHandleV1` | Monotonically increasing handle generator, starting from 1 |

Key methods:
- `insert(int descriptor)` — inserts a native fd and returns a new `LunaFileHandleV1`, skipping `LUNA_INVALID_FILE_HANDLE_V1` (0).
- `withDescriptor(handle, function)` — thread-safely looks up the fd by handle and executes the callback, returning the callback's result; returns `EBADF` if the handle does not exist.
- `take(handle, descriptor)` — removes the handle from the mapping and returns the native fd, used for close operations.

### Platform Adaptation Constants

| Constant | Value | Purpose |
|---|---|---|
| `applicationConsole` | `LunaConsoleV1` | Supports `consoleWrite` + `consoleFlush` + `consoleRead` (stdin) |
| `applicationFileSystem` | `LunaFileSystemV1` | Complete 11-function file system |

## Key Functions and Methods

### Factory Functions

| Function | Purpose |
|---|---|
| `lunaApplicationConsoleV1()` | Returns `&applicationConsole`, including stdin console input |
| `lunaApplicationFileSystemV1()` | Returns `&applicationFileSystem`, including the complete file system |

### Console Callbacks

| Function | Purpose |
|---|---|
| `consoleWrite` | Writes to `stdout`/`stderr` via `fwrite` |
| `consoleFlush` | Flushes the `stdout`/`stderr` buffers |
| `consoleRead` | Reads from `stdin` via `fread`; on failure clears the error flag via `std::clearerr` and maps `errno` |

### File System Callbacks (11 functions)

| Function | Underlying Platform Call | Purpose |
|---|---|---|
| `openFile` | POSIX: `open` / Windows: `_wopen` | Parses flag bits (READ/WRITE/APPEND/TRUNCATE/CREATE/CREATE_NEW), opens the file, and registers the handle |
| `readFile` | `read` / `_read` | Reads file contents |
| `writeFile` | `write` / `_write` | Writes file contents |
| `seekFile` | `lseek` / `_lseeki64` | Positions the file pointer |
| `flushFile` | Only validates that the handle is valid | No-op for regular files |
| `syncFile` | `fsync` / `_commit` | Syncs to disk |
| `closeFile` | `close` / `_close` | Removes the handle from the registry and closes the file |
| `handleMetadata` | `fstat` / `_fstat64` | Queries metadata by file handle |
| `pathMetadata` | `lstat` / `_wstat64` | Queries metadata by path (POSIX uses `lstat` to read the symlink itself) |
| `removeFile` | `unlink` / `_wunlink` | Deletes a file |
| `createDirectory` | `mkdir` / `_wmkdir` | Creates a directory (POSIX mode 0777) |

### Helper Functions

| Function | Purpose |
|---|---|
| `validUtf8` | Manually validates UTF-8 sequences: checks the legality of single-byte, two-byte (0xc2-0xdf), three-byte (0xe0-0xef), and four-byte (0xf0-0xf4) sequences, rejecting overlong encodings, surrogate pairs (U+D800-U+DFFF), and out-of-range code points (>U+10FFFF) |
| `validPath` | Combines UTF-8 validation and a null-byte check (embedded NUL is not allowed in paths) |
| `nativePath` | POSIX: direct `assign`; Windows: converts to `std::wstring` via `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS)` |
| `errorKind` | Maps `errno` values to the `LunaIoErrorKindV1` enum (e.g. `ENOENT`→`NOT_FOUND`, `EACCES`→`PERMISSION_DENIED`) |
| `ioFailure` | Constructs a `LunaIoErrorV1` and returns `LUNA_RUNTIME_STATUS_IO_ERROR` |
| `metadataType` | Converts the platform `st_mode` to `LunaFileTypeV1` (REGULAR/DIRECTORY/SYMLINK/OTHER) |

## Relationship to Surrounding Files and Pipeline Stages

- **ApplicationHostServices.h** — declares the factory functions implemented in this file; includes `RuntimeABI.h`.
- **RuntimeABI.h** — provides type definitions such as `LunaConsoleV1`, `LunaFileSystemV1`, `LunaFileHandleV1`, `LunaIoErrorV1`, `LunaFileMetadataV1`, as well as all `LUNA_*` constants.
- **Runtime.cpp** — combines `lunaApplicationConsoleV1` and `lunaApplicationFileSystemV1` provided by this file into the `applicationHostServices` constant, installed via `rt_install_application_host_services_v1`.
- This file is the only translation unit in the Luna runtime that directly calls the platform POSIX/Windows file I/O APIs.

## Further Reading

- The declarations of the two factory functions in `ApplicationHostServices.h`
- The complete field definitions of `LunaFileSystemV1` and `LunaConsoleV1` in `RuntimeABI.h`
- The construction and activation of the `applicationHostServices` constant in `Runtime.cpp`
- Documentation for POSIX system calls such as `open`(2), `lseek`(2), `fsync`(2), `fstat`(2)
- Documentation for Windows CRT APIs `_wopen`, `_lseeki64`, `_commit`, `_fstat64`


---

---
title: ApplicationHostServices.h
source: src/runtime/ApplicationHostServices.h
language: en
audience: Luna runtime implementers / application embedders
---

# src/runtime/ApplicationHostServices.h

Factory-function declaration header for Luna's application-level host services, providing two functions that return pointers to process-lifetime service tables: `lunaApplicationConsoleV1` and `lunaApplicationFileSystemV1`.

## What This File Does

- Declares `lunaApplicationConsoleV1` — returns a `LunaConsoleV1*` service table that includes console input (stdin reading).
- Declares `lunaApplicationFileSystemV1` — returns a complete `LunaFileSystemV1*` file-system service table.
- Differs from the default services in `Runtime.h`: the default Runtime intentionally does not expose input or the file system; application-level services are enabled through explicit installation.
- Has only a single `#include "RuntimeABI.h"` dependency.

## Key Structs, Classes, and Enums

This file does not define any structs or enums; it only references those defined in `RuntimeABI.h`:

- `LunaConsoleV1` — console I/O service table (containing three function pointers: `write`, `flush`, `read`).
- `LunaFileSystemV1` — file-system service table (containing 11 function pointers).

## Key Functions and Methods

| Function | Return Type | Purpose |
|---|---|---|
| `lunaApplicationConsoleV1()` | `const LunaConsoleV1*` | Returns the process-level console service table, supporting stdout/stderr output and stdin input |
| `lunaApplicationFileSystemV1()` | `const LunaFileSystemV1*` | Returns the process-level file-system service table, supporting all operations: open, read/write, seek, sync, close, metadata, delete file, create directory, etc. |

The implementations of these two functions are located in `ApplicationHostServices.cpp`; they return pointers to file-internal static constant objects, so their lifetime matches that of the process.

## Relationship to Surrounding Files and Pipeline Stages

- **RuntimeABI.h** — provides the type definitions for `LunaConsoleV1` and `LunaFileSystemV1`. This file directly depends on it.
- **ApplicationHostServices.cpp** — the implementation of the two functions declared in this file, containing the complete platform-adaptation code (POSIX + Windows).
- **Runtime.h / Runtime.cpp** — the `applicationHostServices` constant in `Runtime.cpp` initializes its console and file-system sub-tables by calling these two functions. `rt_install_application_host_services_v1` installs this service.
- **Generated code (Generated IR)** — the compiler-generated application entry point installs these services via `rt_install_application_host_services_v1`, then uses forwarding functions such as `rt_console_write_v1`, `rt_file_open_v1`, etc.

## Further Reading

- The specific platform implementations of each function in `ApplicationHostServices.cpp`
- The construction and use of the `applicationHostServices` constant in `Runtime.cpp`
- The complete field definitions of `LunaConsoleV1` and `LunaFileSystemV1` in `RuntimeABI.h`
