# Packages, Modules, and Explicit Exports

Luna separates release units from source namespaces:

- **Package** is the unit of versioning, dependencies, signatures, caching, and Moon containers.
- **module** is a source namespace inside a package. A package may contain multiple modules,
  and modules may contain child modules.
- **workspace** organizes related local packages; a package does not have semantic subpackages.

## Naming rules

A Package ID uses a reverse-DNS name separated by `.`:

```luna
package org.luna.std;
package com.example.graphics.vulkan;
```

Modules use `::` for hierarchy:

```luna
module io;
module io::format;
module collections::ordered;
```

Therefore `.` belongs only to Package IDs, while `::` belongs only to language name
paths.

`com.example.graphics` and `com.example.graphics.vulkan` are two independent packages.
The latter does not inherit dependencies, visibility, or version from the former. A reverse-DNS
prefix conveys publishing namespace ownership, not a language parent-child relationship.

## Source-file header

Each source file may declare at most one package and one module, in package/module/using
order:

```luna
package com.example.application;
module commands::build;

using org.luna.std as std;
using com.example.serialization as serde;
```

`using` refers to a package, not a module. An `as` alias must be unique within the current
package; one alias cannot refer to two Package IDs, and a package cannot `using` itself.
A file without a `module` declaration belongs to the package root module for compatibility
with existing source.

Fully qualified paths are:

```luna
std::io::print("value = {}", value);
serde::json::decode<Data>(source);
```

Here `std`/`serde` are package aliases, `io`/`json` are modules, and the final
component is a declaration.

## Local package assembly

For directory input without a manifest, the driver assembles the `.luna` files in
filename order into a compatibility package. For input with `luna.package`, it recursively
loads sources according to `sources`. All explicit `package` declarations must agree,
while files may declare different modules:

```text
application/
  01_io.luna       # module io;
  02_format.luna   # module io::format;
  03_main.luna     # module application;
```

PackageManager retains the module graph and `Package ID -> alias` dependency edges, and
MoonIR retains `moon.source_module` and `moon.using`. A manifest package uses this strict
minimal TOML schema:

```toml
# luna.package
[package]
id = "com.example.application"
version = "1.0.0"
kind = "application"
sources = ["src"]

[dependencies]
"org.luna.std" = "0.2.1"
```

`kind` is required and must be `"application"` or `"library"`. An application must
define exactly one package-root `main`; a library must not define one. `sources` must be a
relative file or directory inside the package directory; absolute paths
and `..` escapes are not allowed. `.luna` files in directories are enumerated recursively
and sorted by path. Every source `using` must have a corresponding `[dependencies]` entry.

```toml
# luna.workspace
[workspace]
members = ["core", "std", "application"]
```

PackageManager searches upward from the current package for the nearest
`luna.workspace`, then reads each member's `luna.package` and locates local dependencies
by canonical Package ID. It does not implicitly search other parent directories, networks, or
system library paths.

```toml
# luna.lock (tool-generated and canonically sorted by Package ID)
[[package]]
id = "org.luna.std"
version = "0.2.1"
source = "workspace:std"
hash = "..."
```

The current Alpha local resolver requires exact versions and checks Package ID, version, and
workspace source against the lock file. The `hash` field is already a required non-empty
integrity slot, but content digests are not yet computed or verified until Moon-container and
registry artifact formats are frozen.

The resolver recursively loads the local dependency closure. Declaration identity consists of
Package ID, module path, and source name: an unqualified name is looked up only in the
current module; another module in the same package uses `module::symbol`; a dependency uses
`alias::module::symbol`. Different modules may safely declare same-named symbols, and the
compiler generates isolated deterministic linkage names. A reference across Package IDs must
target an `export` declaration; an intra-package cross-module reference does not require
`export`.

```luna
// org.luna.fixture.app / module application
using org.luna.fixture.core as core;

fn main() -> i32 {
    return core::values::library_value();
}
```

A dependency package's `using` aliases remain in its owner's namespace, so different
packages may reuse an alias without contamination. This ownership relation and each
dependency's Package ID/module identity remain in MoonIR and are checked by the verifier.

For a library package without `main`, use `luna check <package>` for the full lexer-to-
verified-MoonIR check; it does not generate LLVM IR or an executable.

## Explicit exports

Only declarations written with `export` enter the package public interface; non-exported
functions, types, and fragments remain package-internal. `export` is an ABI commitment, not
merely a name-resolution marker. Non-exported package-level functions retain internal
linkage in LLVM IR; exported functions use external symbols.

Functions, structs, enums, traits, `interceptor`, and `context` may be exported.
`extern` functions cannot also be exported. See [versioning.md](versioning.md) for public
Metadata/Selector interface rules.
