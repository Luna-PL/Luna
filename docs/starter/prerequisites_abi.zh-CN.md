> Document category: implementation note / tutorial
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative（教学向导；AOT 边界契约以 docs/runtime_abi.md 与 src/runtime/RuntimeABI.h 为准）
> 语言：中文权威版（英文版为占位）

# 前置课 B：ABI、内存布局与调用约定（面向 C/C++ 开发者）

读 Luna 的 src/runtime/RuntimeABI.h、src/core/TypeLayout.h 和 src/codegen/ 时，"ABI"、布局、对齐、tag 会频繁出现。本文把这几块讲透：ABI 是什么、struct 在内存里怎么摆、函数调用怎么传参、Luna 的 tag / 句柄是什么。全程用 C/C++ 类比。

## 1. ABI 是什么（一句话 + C 类比）

ABI = 编译出来的二进制之间如何协作的约定：参数放寄存器还是栈、struct 每个字段偏移多少、返回值放哪。

> 类比：源码层你只关心签名 `int f(int a, int b)`；链接/运行层，编译器必须和调用者约定 a 放哪个寄存器、结果放哪。这种约定就是 ABI。同一 C 函数用 gcc 和 clang 编译出来可互通，是因为两者遵守同一套系统 ABI。

Luna 的关键：运行时边界必须有一份固定、版本化、C 兼容的 ABI。`src/runtime/RuntimeABI.h` 里：
```c
#define LUNA_RUNTIME_ABI_V1 1u
#define LUNA_HOST_SERVICES_MAGIC_V1 0x4c485331u /* "LHS1" */
```
生成的代码之后可被 JIT 执行、AOT 链接、插件加载，它们必须对同一套内存/调用布局达成一致——这就是这份 ABI 的存在意义。

## 2. 内存布局基础：对齐（alignment）

CPU 读内存通常要求地址能被某数整除（对齐）。double 需 8 字节对齐、int 需 4。C++11 起的 alignas 就是显式对齐。Luna 的 TypeLayout.h 用带版本号的常量与 alignTo：
```cpp
inline constexpr uint64_t InlineTagStorageSize = 8;     // tag 槽大小
inline constexpr uint64_t InlinePayloadAlignment = 8;   // 内联载荷对齐
uint64_t alignTo(uint64_t value, uint64_t alignment);
```
`valueAlignment(type)` / `valueSize(type)` 返回该类型在 MoonIR ABI 的大小与对齐。

## 3. 结构体布局：字节级别怎么排

C 结构体字段顺序+padding 决定 sizeof。Luna 的"产品类型"同样有布局，但有关键取舍：

> 真实 Limuna TypeLayout.h 注释：在编译器/MoonIR ABI 里，nominal 产品类型通常是指针表示（pointer-represented），值是一个指向堆上 payload 的指针；而数组、slice、Result、枚举（和/或）是内联值。

```cpp
uint64_t valueSize(const TypePtr& type);            // 类型"值"的盒子大小
uint64_t productStorageSize(const TypePtr& type);  // 堆上 payload（存储体）大小
uint64_t productFieldOffset(const TypePtr& type, size_t fieldIndex); // 字段偏移
```

当你看到"struct 值大小只有 8 字节，但字段要 64 字节堆"这种不一致时，别慌：前者是句柄的盒子，后者是 payload 的大小。这是按使用付费的核心——默认按指针传、低开销，只有真要存储才分配。

## 4. 枚举 / tag：内联标签（inline ADT tag）

sum 类型（枚举/Result）需要 tag + 有效载荷。C++ 里常手写 tagged union：
```cpp
struct Result { int tag; union { int ok; const char* err; } payload; };
```

Luna 的枚举/Result 是内联值：tag 与 payload 放进同一个（如 8 字节）槽里，免去堆分配。TypeLayout.h 有专门入口：
```cpp
uint64_t enumPayloadSize(const TypePtr& type);
uint64_t variantPayloadSize(const TypeVariant& variant);
```

（代码里是函数，用到 InlineTagStorageSize 这些常量。）

> 所以一个 Result<i32, Error> 在 ABI 里就是 tag+payload 内联。codegen 会用 CreateGEP + load/store 去读写 tag 与 payload 字段。

## 5. 表示策略汇总：谁按值、谁按指针

| 形态 | 表示 | Luna 的设计理由 |
|---|---|---|
| struct / record | 内联值（体积已知） | 免堆、可按值传/return） |
| enum / Result | 内联 tag+payload | 无堆分配 |
| array（定长） | 内联值 | 体积可由 type 得出 |
| slice | 内联（ptr + len） | 视图复用 |
| nominal 命名产品 | 指针（句柄） | 按指针传低成本、利于所有权边界 |

给 C++ 读者的直接后果：看到某个类型 valueSize 只有 8 字节时，先问"它是句柄还是内联体"。

## 6. 所有权在布局中的角色

Luna 是静态所有权语言。src/core/Ownership.h 区分 Relation（Owned/SharedBorrow/MutableBorrow）与 Usage（Copy/Affine/Linear）。需要 drop 的类型，编译器在生命周期终点生成 cleanup（生成 null + drop 调用），类似 C++ RAII 析构，但 Luna 在编译期静态证明归属，而不是整体运行时引用计数（除非选 Arc/Rc）。

## 7. Runtime ABI：生成代码与宿主握手

src/runtime/RuntimeABI.h 定义错误/能力/分配器接口，全部 extern "C"（符号 C 可链接）：
```c
enum LunaRuntimeStatusV1 { LUNA_RUNTIME_STATUS_OK = 0, ... };
typedef struct LunaRuntimeErrorSnapshotV1 {
  uint32_t abi_version; uint32_t struct_size;
  uint32_t domain; int32_t code; uint64_t message_size;
} LunaRuntimeErrorSnapshotV1;
```
宿主能力是位标志（allocator、console、filesystem、可选的 executable memory）；allocator 是 C 函数指针表（LunaAllocatorV1：allocate/reallocate/deallocate + context）。

> 记忆点："ABI 稳定" = 只要都遵守 LUNA_RUNTIME_ABI_V1，不同语言/编译器编出的二进制就能互操作。

## 8. 给 C++ 读者新概念清单

- **ABI**：二进制层的协作约定。
- **对齐/填充**：CPU 对齐要求导致的 padding；Luna 用 alignTo 与对齐常量管理。
- **tag / 内联枚举**：tag+payload 塞进同一槽，避免堆分配。
- **句柄 vs 内联**：nominal 产品指针、枚举/slice/小值内联。
- **版本化 ABI**：LUNA_RUNTIME_ABI_V1，边界永远携带版本。

## 9. 继续阅读

- 类型布局： src/core/TypeLayout.h 、[类型系统导读](./core_tooling_rest.zh-CN.md)
- 运行时 ABI 权威契约： docs/runtime_abi.md 、src/runtime/RuntimeABI.h
- codegen 怎么用布局：[codegen 导读](./codegen.zh-CN.md)