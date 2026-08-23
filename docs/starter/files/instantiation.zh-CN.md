> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/instantiation/ —— 目录逐文件指南

本指南合并了 src/instantiation/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: Instantiator.cpp
source: src/instantiation/Instantiator.cpp
language: C++ (C++17)
audience: Luna 编译器开发者 / 泛型实例化子系统实现者
---

# src/instantiation/Instantiator.cpp

Instantiator.cpp 是 Instantiator 类的完整实现，包含确定性缓存键生成、FNV-1a 稳定哈希、实例符号名生成以及完整的实例化生命周期管理。

## 这个文件做什么

- 实现匿名命名空间中的辅助函数 appendPart() 和 stableHash()：前者用于构建长度前缀的字符串拼接，后者实现 FNV-1a 64 位哈希算法。
- 实现 Instantiator::reset()：清空 mEntries 映射表。
- 实现 Instantiator::begin()：插入或查找条目，设置状态为 InProgress，记录请求站点。
- 实现 Instantiator::complete() 和 Instantiator::fail()：推进状态机。
- 实现 Instantiator::lookup() 和 Instantiator::lookupKey()：只读查找。
- 实现静态方法 keyFor()：生成格式化的缓存键字符串。
- 实现静态方法 instanceIdFor()：生成基于 FNV-1a 哈希的实例符号名。

## 关键结构体·类·枚举

本文件（.cpp）是纯实现文件，**不定义任何新类型**（无 struct / class / enum / union 声明）。它实现的所有类型均声明于同名头文件：

| 类型 | 声明位置 | 说明（见头文件指南） |
|------|----------|----------------------|
| `Request` | `Instantiator.h` | 实例化请求：声明 ID + 三类实参（类型/值/元数据） |
| `State` | `Instantiator.h` | 实例条目状态机：InProgress / Ready / Failed |
| `Entry` | `Instantiator.h` | 缓存条目：key、instanceId、state、failure、requestSites |
| `Instantiator` | `Instantiator.h` | 实例化器类，本文件实现其全部方法 |

> 若想了解这些类型的内存布局或字段，请直接阅读 `src/instantiation/Instantiator.h` 或对应头文件的 zh-CN 指南。

## 关键函数·方法

### 匿名命名空间辅助函数

#### appendPart(std::string& output, const std::string& part)

```cpp
void appendPart(std::string& output, const std::string& part) {
    output += std::to_string(part.size());
    output += ':';
    output += part;
    output += ';';
}
```

以 <长度>:<内容>; 格式追加一段内容到字符串。这种长度前缀格式（类似 Protocol Buffers 的 length-delimited 编码）保证序列化结果无歧义，且可被解析器正确分段。

#### stableHash(const std::string& value) -> uint64_t

```cpp
uint64_t stableHash(const std::string& value) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}
```

实现 FNV-1a（Fowler–Noll–Vo 1a）64 位非加密哈希算法。该算法具有以下特性：
- 确定性：相同输入总是产生相同输出，适合用作缓存键的哈希。
- 快速：仅涉及 XOR 和乘法，无需复杂运算。
- 稳定：不依赖平台特性（如字节序），跨编译运行结果一致。

偏移基数 1469598103934665603ULL 和质数 1099511628211ULL 是 FNV-1a 64 位的标准参数。

### Instantiator::begin(const Request& request) -> const Entry&

```cpp
const Entry& Instantiator::begin(const Request& request) {
    const auto key = keyFor(request);
    auto [iterator, inserted] = mEntries.emplace(key, Entry{});
    auto& entry = iterator->second;
    if (inserted) {
        entry.key = key;
        entry.instanceId = instanceIdFor(request);
        entry.state = State::InProgress;
    }
    if (!request.requestedBy.empty())
        entry.requestSites.push_back(request.requestedBy);
    return entry;
}
```

使用 std::unordered_map::emplace 的返回值 std::pair<iterator, bool>（C++17 结构化绑定）来判断是否是首次插入。如果是新插入，则初始化 key、instanceId 和 state。无论是否首次，都记录请求站点（如果非空）。返回 const Entry& 引用，调用方通过检查返回的 state 是否为 InProgress 来判断是否首次实例化。

### Instantiator::complete(const std::string& key) -> bool

```cpp
bool Instantiator::complete(const std::string& key) {
    auto iterator = mEntries.find(key);
    if (iterator == mEntries.end() || iterator->second.state != State::InProgress)
        return false;
    iterator->second.state = State::Ready;
    return true;
}
```

仅当条目存在且状态为 InProgress 时成功。如果条目不存在（end()）或状态不是 InProgress（如已 Ready 或 Failed），返回 false。这防止了重复完成或已完成条目的误操作。

### Instantiator::fail(const std::string& key, std::string reason) -> bool

```cpp
bool Instantiator::fail(const std::string& key, std::string reason) {
    auto iterator = mEntries.find(key);
    if (iterator == mEntries.end()) return false;
    iterator->second.state = State::Failed;
    iterator->second.failure = std::move(reason);
    return true;
}
```

与 complete() 不同，fail() 允许任何状态（除了不存在）的条目标记为 Failed。失败原因通过 std::move 获取所有权，避免字符串拷贝。

### Instantiator::keyFor(const Request& request) -> std::string（静态）

```cpp
std::string Instantiator::keyFor(const Request& request) {
    std::string key;
    appendPart(key, request.genericDeclarationId);
    key += "T";
    for (const auto& argument : request.typeArguments) appendPart(key, argument);
    key += "V";
    for (const auto& argument : request.valueArguments) appendPart(key, argument);
    key += "M";
    for (const auto& argument : request.metadataArguments) appendPart(key, argument);
    return key;
}
```

以 T、V、M 作为分隔符标记三类实参。例如，对泛型声明 foo、类型参数 [int, string] 的键为：3:foo;T3:int;6:string;V;M。

### Instantiator::instanceIdFor(const Request& request) -> std::string（静态）

```cpp
std::string Instantiator::instanceIdFor(const Request& request) {
    std::ostringstream output;
    output << "__moon_inst_" << std::hex
           << std::setw(16) << std::setfill('0') << stableHash(keyFor(request));
    return output.str();
}
```

生成形如 __moon_inst_<16位十六进制> 的符号名。std::setw(16) 和 std::setfill('0') 保证哈希值始终填充为 16 位十六进制数字（即 64 位）。该符号名在 MoonIR 中用作实例化后类型的唯一标识。

## 与周边文件·阶段的关系

```
Instantiator.cpp -> Instantiator.h（类型定义）
```

- Instantiator.h：Request、Entry、State、Instantiator 的类型定义。
- 本文件是泛型实例化子系统的唯一实现文件。在编译器流水线中，实例化发生在语义分析阶段识别出泛型具体使用后，结果供后续代码生成阶段使用。

## 延伸阅读

- Instantiator.h：核心类型和接口声明
- FNV-1a 哈希算法：https://en.wikipedia.org/wiki/Fowler-Noll-Vo_hash_function
- selector/Selector.h：声明选择器，与实例化器协同处理泛型声明选择
- C++17 结构化绑定与 std::unordered_map::emplace 返回值

---

---
title: Instantiator.h
source: src/instantiation/Instantiator.h
language: C++ (C++17)
audience: Luna 编译器开发者 / 泛型实例化子系统开发者
---

# src/instantiation/Instantiator.h

Instantiator.h 声明了泛型实例化（generic instantiation）子系统的核心类型：实例化请求 Request、实例化条目 Entry、状态枚举 State 以及实例化器 Instantiator。该组件负责泛型声明的按需实例化及其缓存、递归检测和确定性输出。

## 这个文件做什么

- 定义 Request：封装一次实例化请求的全部输入——泛型声明 ID、类型参数、值参数、元数据参数和请求者标识。
- 定义 State 枚举：跟踪实例化条目的生命周期——New（新建）、InProgress（进行中）、Ready（就绪）、Failed（失败）。
- 定义 Entry：记录一次实例化尝试的完整状态，包括缓存键、实例 ID、请求站点链和失败原因。
- 定义 Instantiator 类：提供 begin()、complete()、fail()、lookup() 等操作，以及静态方法 keyFor() 和 instanceIdFor() 用于生成缓存键和实例符号名。

## 关键结构体·类·枚举

### Request

```cpp
struct Request {
    std::string genericDeclarationId;
    std::vector<std::string> typeArguments;
    std::vector<std::string> valueArguments;
    std::vector<std::string> metadataArguments;
    std::string requestedBy;
};
```

封装一次实例化请求的全部输入。类似 C++ 模板实例化时编译器内部的"模板实参列表"，但 Luna 泛型分离了类型、值和元数据三类实参，且支持运行时反射。

### State 枚举

```cpp
enum class State {
    New,
    InProgress,
    Ready,
    Failed,
};
```

InProgress 状态用于递归检测：如果 begin() 发现已存在的条目且状态为 InProgress，说明发生了递归实例化（泛型展开自身），编译器可以在此处报告错误。

### Entry

```cpp
struct Entry {
    std::string key;
    std::string instanceId;
    State state = State::New;
    std::vector<std::string> requestSites;
    std::string failure;
};
```

每个 Entry 对应一个唯一的实参组合。requestSites 是一个向量，记录所有请求该实例化的位置，类似 C++ 编译器中模板实例化栈的调用链。

### Instantiator

```cpp
class Instantiator {
public:
    void reset();
    const Entry& begin(const Request& request);
    bool complete(const std::string& key);
    bool fail(const std::string& key, std::string reason);
    const Entry* lookup(const Request& request) const;
    const Entry* lookupKey(const std::string& key) const;
    static std::string keyFor(const Request& request);
    static std::string instanceIdFor(const Request& request);
private:
    std::unordered_map<std::string, Entry> mEntries;
};
```

Instantiator 是一个无外部依赖的缓存管理器，类比 C++ 编译器中模板特化的"实例化缓存"（instantiation cache）。内部使用 std::unordered_map 以缓存键为索引存储所有 Entry。静态方法 keyFor() 和 instanceIdFor() 不依赖实例状态，可独立调用。

## 关键函数·方法

### Instantiator::reset()

清空所有条目，重置状态。用于编译单元之间的缓存清理。

### Instantiator::begin(const Request& request) -> const Entry&

尝试开始一次实例化。如果给定 request 的缓存键不存在，则创建新 Entry 并设置状态为 InProgress。如果已存在，则追加请求站点到 requestSites。返回 const Entry& 引用——调用方应检查返回的 state 是否为 InProgress 来判断是否首次实例化。

### Instantiator::complete(const std::string& key) -> bool

将指定键的条目标记为 Ready。仅当条目存在且状态为 InProgress 时成功。返回 bool 表示操作是否合法。

### Instantiator::fail(const std::string& key, std::string reason) -> bool

将指定键的条目标记为 Failed，并记录失败原因。reason 通过 std::move 获取所有权。

### Instantiator::lookup() / lookupKey() -> const Entry*

只读查找，返回裸指针。lookup() 通过 Request 查找，内部调用 lookupKey(keyFor(request))。nullptr 表示未找到。

### Instantiator::keyFor(const Request&) -> std::string（静态）

生成确定性缓存键。格式为：<len>:<genericDeclarationId>;T<len>:<typeArg>;...V<len>:<valueArg>;...M<len>:<metadataArg>;...。其中 T、V、M 分别标记类型、值、元数据参数段。这种格式保证相同实参产生相同字符串，且不同实参产生不同字符串。

### Instantiator::instanceIdFor(const Request&) -> std::string（静态）

生成实例在 IR 中的符号名。格式为 __moon_inst_<16位十六进制哈希>，其中哈希值通过对 keyFor() 的结果执行 FNV-1a 64 位哈希得到。

## 与周边文件·阶段的关系

```
Instantiator.h -> Instantiator.cpp（实现）
```

- Instantiator.cpp：keyFor()、instanceIdFor() 和所有方法的完整实现，包含匿名命名空间中的辅助函数 appendPart() 和 stableHash()。
- 实例化阶段在语义分析（Sema）之后，代码生成之前。当编译器遇到泛型声明的具体使用（如调用泛型函数、构造泛型结构体）时，通过 Instantiator 管理实例化的生命周期。

## 延伸阅读

- Instantiator.cpp：缓存键生成、FNV-1a 哈希与实例化生命周期管理实现
- selector/Selector.h：声明选择器，与实例化器协同处理泛型声明选择
- FNV-1a 哈希算法：Instantiator.cpp 中 stableHash 的实现基准

---
