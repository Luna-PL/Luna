# Dynamic fragment plugins

## 中文说明

动态插件用于在不重新编译 Luna 程序的情况下按名字选择片段。Alpha v1 已支持
共享库提供的 host-only、single-shot、显式参数 `interceptor`；运行时会校验槽名、
片段名和契约标识。外部 `context`、`resume()`、多发射和词法捕获仍需后续持久化
continuation frame ABI，不能越过当前安全边界。

## Implemented stage: linked runtime weaving

Luna supports a host-only, runtime-selected fragment candidate set:

```luna
dynamic slot context pipeline(value: i32);

dynamic apply pipeline(trace, audit) {
    pipeline(41) { print(42); }
}
```

The first candidate is the deterministic fallback. At runtime,
`LUNA_FRAGMENT_PIPELINE=audit` selects `audit`; an unknown name terminates with
a diagnostic instead of falling through to arbitrary native code. The compiler
emits a runtime dispatch branch but keeps each candidate's source continuation
in the generated host module. Consequently, the same AOT binary can select a
different linked behavior without recompilation.

The slot invocation may appear inside a loop. Runtime selection is performed
at each invocation, so a loop can process a stream of values through the same
typed plugin point. The ownership checker still requires every iteration to
preserve the outer loop state: a candidate may not consume an outer linear
resource, leave a borrow active, or leave a device event unresolved.

This is deliberately a typed plugin boundary rather than a raw callback API:

- `dynamic slot` requires an explicit parameter interface.
- Every candidate is checked against that slot interface.
- The slot and every candidate must declare the same `interceptor`/`context`
  and `once`/`many` contract; dynamic contexts are currently single-shot.
- The ownership checker evaluates every candidate and rejects alternatives
  that leave different linear, borrow, or device in-flight states.
- Dynamic slots are host-only. Device kernels reject `dynamic slot` and
  `dynamic apply` together with every other continuation effect.

The compiler records this internally as a dynamic-dispatch continuation path,
not as an ordinary static inline `apply`. It is therefore an optimization and
control-flow boundary even when the fallback candidate name is known at build
time.

## Current selection interface

The runtime normalizes a slot name to an environment key:

```text
slot: pipeline       -> LUNA_FRAGMENT_PIPELINE
slot: request-log    -> LUNA_FRAGMENT_REQUEST_LOG
```

This environment interface is intended for tests, feature flags, deployment
configuration, and linked plugin catalogs. It is not a security boundary; a
production host should pass configuration through its own trusted launch
environment.

## External shared-library plugins: Alpha v1

External shared-library loading is available for the deliberately smaller v1
boundary. It is selected without recompiling the Luna program:

```text
LUNA_FRAGMENT_PIPELINE=external_trace \
LUNA_FRAGMENT_PLUGIN=/path/to/libtrace.so \
luna run app.luna
```

Windows 使用对应的 `.dll` 路径，例如
`LUNA_FRAGMENT_PLUGIN=C:/plugins/trace.dll`；运行时会使用原生动态库加载器。

The plugin exports one C symbol, `luna_fragment_plugin_descriptor_v1`. Its
descriptor is validated before registration and contains:

```text
plugin id + version
slot name + slot ABI hash
parameter layout
emission capability (once/many)
effect flags (may-abort, host-only)
fragment entry point
```

The Alpha v1 boundary accepts only host-only, single-shot `interceptor`
plugins. The entry receives a read-only array of pointers to explicit slot
arguments and returns `continue` or a declared `abort`; the host then executes
the statically generated slot continuation. The contract string is generated
from the slot name, interceptor/context kind, cardinality, and parameter type
layout. A mismatch is rejected before entry invocation.

This is intentionally not yet an external `context` ABI. Such a plugin would
need a persistent continuation frame and a proven lifetime rule for
`resume()`. The current static lowering uses a stack-resident frame, so it is
not safe to expose that frame as an arbitrary shared-library callback. External
plugins also cannot capture Luna lexical variables, retain argument pointers,
or provide `many` behavior in v1.

The runtime keeps successfully loaded libraries alive for the process lifetime
and rejects duplicate `(slot, fragment, contract)` registrations. This makes
the plugin path suitable for deployment configuration and tests, but the
environment variable is not a security boundary; production hosts should
load only trusted paths.
