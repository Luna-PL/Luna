# src/runtime/FragmentPluginABI.h

Luna 外部片段插件（Fragment Plugin）的稳定 C ABI 规范头文件，定义了插件描述符、调用参数包、入口点函数指针类型和相关常量。

## 这个文件做什么

- 定义片段插件 ABI 版本宏 `LUNA_FRAGMENT_PLUGIN_ABI_V1`（1）和描述符魔数 `LUNA_FRAGMENT_PLUGIN_DESCRIPTOR_V1`（`"LFP1"`，即 0x4c465031）。
- 定义片段类型枚举：`LUNA_FRAGMENT_KIND_INTERCEPTOR`（拦截器，1）和 `LUNA_FRAGMENT_KIND_CONTEXT`（上下文，2）。
- 定义基数枚举：`LUNA_FRAGMENT_CARDINALITY_ONCE`（单次，1）和 `LUNA_FRAGMENT_CARDINALITY_MANY`（多次，2）。
- 定义副作用标志枚举：`LUNA_FRAGMENT_EFFECT_HOST_ONLY`（仅宿主，位 0）和 `LUNA_FRAGMENT_EFFECT_MAY_ABORT`（可能终止，位 1）。
- 定义插件入口返回值枚举：`LUNA_FRAGMENT_PLUGIN_CONTINUE`（0）、`LUNA_FRAGMENT_PLUGIN_ABORT`（1）、`LUNA_FRAGMENT_PLUGIN_ERROR`（-1）。
- 定义数据结构：`LunaFragmentInvocationV1`（调用参数包）和 `LunaFragmentPluginDescriptorV1`（插件描述符）。
- 定义入口点函数指针类型 `LunaFragmentPluginEntryV1` 和描述符获取函数指针类型 `LunaFragmentPluginDescriptorFnV1`。

ABI 设计原则是"元数据优先"：宿主在调用入口点之前先验证 slot 契约。

## 关键结构体·类·枚举

### `LunaFragmentInvocationV1` —— 调用参数包

```c
typedef struct LunaFragmentInvocationV1 {
    uint32_t abi_version;           // 固定为 LUNA_FRAGMENT_PLUGIN_ABI_V1
    const void* const* args;        // 参数指针数组，每个元素指向一个参数值
    size_t arg_count;               // 参数个数
} LunaFragmentInvocationV1;
```

参数以指针数组的形式传递，而不是扁平化参数块。每个参数值由宿主分配，插件只读访问。

### `LunaFragmentPluginDescriptorV1` —— 插件描述符

```c
typedef struct LunaFragmentPluginDescriptorV1 {
    uint32_t magic;                 // LUNA_FRAGMENT_PLUGIN_DESCRIPTOR_V1
    uint32_t abi_version;           // LUNA_FRAGMENT_PLUGIN_ABI_V1
    uint32_t descriptor_size;       // 结构体大小，用于向前兼容
    const char* plugin_id;          // 插件唯一标识符
    const char* fragment_name;      // 片段名称
    const char* slot_name;          // 插槽名称
    const char* contract_hash;      // 编译器产生的契约哈希，覆盖 kind/cardinality/参数布局
    uint32_t fragment_kind;         // LUNA_FRAGMENT_KIND_INTERCEPTOR 或 _CONTEXT
    uint32_t cardinality;           // LUNA_FRAGMENT_CARDINALITY_ONCE 或 _MANY
    uint32_t effects;               // 副作用标志位组合
    LunaFragmentPluginEntryV1 entry; // 入口点函数指针
} LunaFragmentPluginDescriptorV1;
```

### 枚举汇总

| 枚举组 | 值 | 语义 |
|---|---|---|
| 片段类型 | `LUNA_FRAGMENT_KIND_INTERCEPTOR` (1) | 拦截器：宿主调用点被替换为插件实现 |
|  | `LUNA_FRAGMENT_KIND_CONTEXT` (2) | 上下文：提供运行时上下文值 |
| 基数 | `LUNA_FRAGMENT_CARDINALITY_ONCE` (1) | 单次：插件只被调用一次 |
|  | `LUNA_FRAGMENT_CARDINALITY_MANY` (2) | 多次：插件可被多次调用 |
| 副作用 | `LUNA_FRAGMENT_EFFECT_HOST_ONLY` (1) | 仅在宿主端执行，不涉及 Luna 栈 |
|  | `LUNA_FRAGMENT_EFFECT_MAY_ABORT` (2) | 可能终止进程 |
| 返回值 | `LUNA_FRAGMENT_PLUGIN_CONTINUE` (0) | 继续执行 |
|  | `LUNA_FRAGMENT_PLUGIN_ABORT` (1) | 中止当前片段 |
|  | `LUNA_FRAGMENT_PLUGIN_ERROR` (-1) | 发生错误 |

### 函数指针类型

| 类型 | 签名 | 用途 |
|---|---|---|
| `LunaFragmentPluginEntryV1` | `int(*)(const LunaFragmentInvocationV1*)` | 插件入口点，接收调用参数包，返回 `CONTINUE`/ `ABORT`/ `ERROR` |
| `LunaFragmentPluginDescriptorFnV1` | `const LunaFragmentPluginDescriptorV1*(*)(void)` | 共享库中的导出函数，返回描述符指针。插件应导出一个名为 `luna_fragment_plugin_descriptor_v1` 的该类型函数 |

## 关键函数·方法

本文件是纯 ABI 头文件，不包含任何函数实现，仅定义两个函数指针类型和三个结构体。

### 插件侧要求

一个合法的片段插件共享库必须：
1. 导出一个名为 `luna_fragment_plugin_descriptor_v1` 的 `LunaFragmentPluginDescriptorFnV1` 类型函数。
2. 该函数返回一个进程生命周期的 `LunaFragmentPluginDescriptorV1` 常量指针。
3. 描述符中的 `entry` 字段指向一个有效的 `LunaFragmentPluginEntryV1` 函数。

### 宿主侧对应函数

宿主（`Runtime.cpp`）通过以下函数加载和调用插件：
- `rt_fragment_plugin_load` —— 加载共享库，查找 `luna_fragment_plugin_descriptor_v1` 符号，验证描述符。
- `rt_fragment_plugin_is_registered` —— 按 slot_name + fragment_name + contract_hash 三元组查询。
- `rt_fragment_plugin_invoke` —— 查找并调用匹配插件的 `entry` 函数。

## 与周边文件·阶段的关系

- **Runtime.h** —— 声明片段插件相关的 C ABI 入口函数（`rt_fragment_plugin_load`、`rt_fragment_plugin_invoke` 等），这些函数使用本文件定义的类型。
- **Runtime.cpp** —— 实现插件加载与调用逻辑，使用本文件的结构体进行描述符验证。
- **RuntimeABI.h** —— 提供 `LunaRuntimeErrorSnapshotV1` 等错误类型，插件加载错误会写入对应错误域。
- 本文件是独立的 ABI 规范，不依赖任何其他 Runtime 头文件，仅依赖标准 `<stddef.h>` 和 `<stdint.h>`。
- 片段插件开发者应直接包含本文件来构造插件描述符。

## 延伸阅读

- `Runtime.h` 中 `rt_fragment_plugin_load`、`rt_fragment_plugin_invoke` 等函数的声明
- `Runtime.cpp` 中片段插件加载与验证的实现细节
- `RuntimeABI.h` 中 `LUNA_RUNTIME_ERROR_DOMAIN_FRAGMENT_PLUGIN` 错误域的定义
- 动态链接加载：POSIX `dlopen`/ `dlsym` 与 Windows `LoadLibrary`/ `GetProcAddress`


---

---
title: Runtime.cpp
source: src/runtime/Runtime.cpp
language: zh-CN
audience: Luna 运行时实现者 / 嵌入宿主
---
