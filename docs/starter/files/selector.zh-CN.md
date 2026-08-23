> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/selector/ —— 目录逐文件指南

本指南合并了 src/selector/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

---
title: Selector.cpp
source: src/selector/Selector.cpp
language: C++ (C++17)
audience: Luna 编译器开发者 / 声明选择机制实现者
---

# src/selector/Selector.cpp

Selector.cpp 是 DeclarationView 构造函数与 Engine 核心方法的实现，负责声明视图的不变量验证和选择结果的合法性检查。

## 这个文件做什么

- 实现 DeclarationView::DeclarationView(std::vector<Candidate>)：在构造函数中执行三个关键不变量检查——候选 ID 非空、无重复 ID、同一声明族。
- 实现 DeclarationView::find()：线性搜索候选列表，返回匹配的 Candidate* 指针或 nullptr。类比 C++ 中 std::find_if 的裸指针版本。
- 实现 Engine::validate()：验证选择器返回值，确保恰好返回一个在视图内的合法候选。
- 实现 Engine::planDynamic()：为动态选择生成 DynamicPlan，验证所有候选至少为 Runtime 存活级别。

## 关键结构体·类·枚举

（本文件不定义新类型，所有类型来自 Selector.h。）

## 关键函数·方法

### DeclarationView::DeclarationView(std::vector<Candidate> candidates)

```cpp
DeclarationView::DeclarationView(std::vector<Candidate> candidates)
    : mCandidates(std::move(candidates)) {
    std::unordered_set<std::string> ids;
    for (const auto& candidate : mCandidates) {
        if (candidate.declarationId.empty()) {
            mValid = false;
            mError = "selector candidate has no declaration identity";
            return;
        }
        if (!ids.insert(candidate.declarationId).second) {
            mValid = false;
            mError = "selector view contains duplicate candidate '" +
                     candidate.declarationId + "'";
            return;
        }
        if (mFamilyId.empty()) mFamilyId = candidate.familyId;
        else if (mFamilyId != candidate.familyId) {
            mValid = false;
            mError = "selector view crosses declaration family boundaries";
            return;
        }
    }
}
```

构造函数通过 std::move 获取候选列表（避免拷贝，类似 C++ 中按值传递 + std::move 的惯用法），然后遍历所有候选执行三个检查。使用 std::unordered_set 实现 O(1) 均摊的重复检测。一旦发现任何不变量违例，立即设置 mValid = false、记录错误信息并返回，剩余候选不再检查（fail-fast 策略）。

### DeclarationView::find()

```cpp
const Candidate* DeclarationView::find(const std::string& declarationId) const {
    for (const auto& candidate : mCandidates)
        if (candidate.declarationId == declarationId) return &candidate;
    return nullptr;
}
```

简单的线性搜索，时间复杂度 O(n)。返回裸指针，类比 C++ 中 std::find_if 返回迭代器后取地址。nullptr 表示未找到。

### Engine::validate()

```cpp
Result Engine::validate(const DeclarationView& view,
                        const std::vector<std::string>& returnedIds) const {
    if (!view.valid())
        return {ResultKind::InvalidView, std::nullopt, view.error()};
    if (returnedIds.empty())
        return {ResultKind::NoMatch, std::nullopt,
                "selector returned no legal declaration"};
    if (returnedIds.size() != 1)
        return {ResultKind::Ambiguous, std::nullopt,
                "selector must return exactly one declaration"};
    const auto* selected = view.find(returnedIds.front());
    if (!selected)
        return {ResultKind::InvalidCandidate, std::nullopt,
                "selector returned a declaration outside its supplied candidate view"};
    return {ResultKind::Unique, *selected, {}};
}
```

采用 guard-clause 风格（卫语句），逐层检查失败条件并提前返回。每个 Result 通过聚合初始化（aggregate initialization）构造，语法类似 C++ 的结构体初始化列表。selected 字段用 std::nullopt 表示无值，最后通过 *selected（拷贝 Candidate 值）生成成功结果。

### Engine::planDynamic()

```cpp
std::optional<DynamicPlan> Engine::planDynamic(
    const DeclarationView& view,
    const std::string& selectorDeclarationId,
    std::string& error) const {
    error.clear();
    if (!view.valid()) { error = view.error(); return std::nullopt; }
    if (selectorDeclarationId.empty()) { error = "dynamic selector has no declaration identity"; return std::nullopt; }
    DynamicPlan plan;
    plan.familyId = view.familyId();
    plan.selectorDeclarationId = selectorDeclarationId;
    for (const auto& candidate : view.candidates()) {
        if (candidate.retention == Retention::CompileTime) {
            error = "dynamic select candidate '" + candidate.declarationId +
                    "' has no runtime descriptor";
            return std::nullopt;
        }
        plan.candidateIds.push_back(candidate.declarationId);
    }
    if (plan.candidateIds.empty()) {
        error = "dynamic select has an empty candidate view";
        return std::nullopt;
    }
    return plan;
}
```

动态选择计划的生成分两步：先验证输入合法性（视图合法、选择器 ID 非空、所有候选不为 CompileTime），再填充 DynamicPlan。失败时通过 std::optional 返回空值，并通过 error 输出参数（非 const 引用）传递错误描述，这是 C++ 中常见的"返回 optional + 出参传错误"模式。

## 与周边文件·阶段的关系

```
Selector.cpp -> Selector.h（类型定义）
```

- Selector.h：所有实现的结构体头文件，包含 Candidate、Result、DynamicPlan 等类型。
- 本文件的选择逻辑在语义分析阶段（Sema）中调用，具体选择时机由 select 表达式在 AST 中的位置决定。

## 延伸阅读

- Selector.h：声明视图和选择引擎的类型定义
- instantiation/Instantiator.h：泛型实例化器，与选择器协同
- C++ 标准库：std::unordered_set、std::optional、std::move

---

---
title: Selector.h
source: src/selector/Selector.h
language: C++ (C++17)
audience: Luna 编译器开发者 / 宏与声明选择机制开发者
---

# src/selector/Selector.h

Selector.h 声明了声明选择（declaration selection）子系统的核心类型：候选描述 Candidate、不可变候选视图 DeclarationView、选择结果 Result、动态选择计划 DynamicPlan 以及选择引擎 Engine。该文件是编译期中“声明族内唯一选择”这一语义约束的落地实现。

## 这个文件做什么

- 定义 Retention 枚举：区分编译期、运行时、动态三种存活策略，控制候选声明在代码生成阶段的保留方式。
- 定义 Metadata 与 MetadataValue：为候选声明附加可反射的元数据键值对，支持编译期检查和运行时查询。
- 定义 Candidate：描述一个可被选择的声明，包含声明标识、符号名、所属族 ID、可调用类型签名以及存活策略。
- 定义 DeclarationView：不可变的候选声明有限视图，在构造时验证成员同一族（same-family）和标识唯一性两个不变量，类似 C++ 的 const std::vector<Candidate> 加上构造时校验的包装器。
- 定义 Result 与 ResultKind：选择操作的结果类型，枚举了唯一命中、无匹配、歧义、无效候选、无效视图五种状态。
- 定义 DynamicPlan：动态选择场景下，记录族 ID、候选 ID 列表和选择器声明 ID，供运行时生成选择代码。
- 定义 Engine：选择引擎，提供 validate() 和 planDynamic() 两个核心方法。

## 关键结构体·类·枚举

### Retention 枚举

```cpp
enum class Retention : uint8_t {
    CompileTime,  // 仅编译期存在，不生成运行时描述
    Runtime,      // 生成运行时描述，可反射
    Dynamic,      // 保留运行时描述，且可在运行时动态选择
};
```

类比 C++ 的 constexpr（CompileTime）与 typeid/动态反射（Runtime/Dynamic）。Dynamic 是 Runtime 的超集，保证候选声明在运行时仍可通过选择器查询。

### MetadataValue

```cpp
using MetadataValue = std::variant<int64_t, double, bool, std::string>;
```

元数据值的类型安全的联合体，类似 C++17 的 std::variant，支持四种基础类型：整数、浮点数、布尔值和字符串。

### Metadata

```cpp
struct Metadata {
    std::string schemaId;
    std::vector<MetadataValue> values;
    Retention retention = Retention::CompileTime;
};
```

每个元数据项通过 schemaId 关联到一个模式定义（schema），values 按位置存储模式中的字段值。类比 C++ 中通过 UUID 与反射系统关联的注解。

### Candidate

```cpp
struct Candidate {
    std::string declarationId;
    std::string symbolName;
    std::string familyId;
    TypePtr callableType;
    Retention retention = Retention::CompileTime;
    std::vector<Metadata> metadata;
};
```

Candidate 描述一个可供选择的声明。familyId 是约束一组声明属于同一族的键：选择器只能从同一族内选择一个声明。callableType 指向 Luna 类型系统中的 Type 对象（通过 std::shared_ptr<Type> 共享所有权，详见 core/TypeIdentity.h）。

### DeclarationView

```cpp
class DeclarationView {
public:
    explicit DeclarationView(std::vector<Candidate> candidates = {});
    const std::vector<Candidate>& candidates() const;
    const Candidate* find(const std::string& declarationId) const;
    const std::string& familyId() const;
    bool valid() const;
    const std::string& error() const;
private:
    std::vector<Candidate> mCandidates;
    std::string mFamilyId;
    bool mValid = true;
    std::string mError;
};
```

DeclarationView 是选择器的输入。它在构造函数中执行三个不变量检查（见 Selector.cpp 实现细节）：
1. 每个候选必须有非空的 declarationId。
2. 候选 ID 不能重复（通过 std::unordered_set 去重）。
3. 所有候选必须属于同一个声明族（familyId 一致）。

如果任何检查失败，valid() 返回 false，error() 返回错误描述。find() 提供 O(n) 的 ID 查找，等价于 C++ 的 std::find_if。

### ResultKind 枚举

```cpp
enum class ResultKind {
    Unique,
    NoMatch,
    Ambiguous,
    InvalidCandidate,
    InvalidView,
};
```

### Result

```cpp
struct Result {
    ResultKind kind = ResultKind::NoMatch;
    std::optional<Candidate> selected;
    std::string message;
    bool success() const { return kind == ResultKind::Unique && selected.has_value(); }
};
```

选择操作的结果，类比 C++ 中返回 std::optional<T> 加上错误码的模式。success() 是便捷方法，等价于检查 kind == Unique && selected.has_value()。

### DynamicPlan

```cpp
struct DynamicPlan {
    std::string familyId;
    std::vector<std::string> candidateIds;
    std::string selectorDeclarationId;
};
```

动态选择计划，供代码生成阶段输出运行时选择代码。

### Engine

```cpp
class Engine {
public:
    Result validate(const DeclarationView& view,
                    const std::vector<std::string>& returnedIds) const;
    std::optional<DynamicPlan> planDynamic(
        const DeclarationView& view,
        const std::string& selectorDeclarationId,
        std::string& error) const;
};
```

选择引擎是一个无状态（stateless）的验证器，没有成员变量。validate() 检查选择器返回的 ID 集合是否合法；planDynamic() 为动态选择场景生成运行时计划。

## 关键函数·方法

### Engine::validate()

```cpp
Result Engine::validate(const DeclarationView& view,
                        const std::vector<std::string>& returnedIds) const;
```

验证选择器表达式的返回值。检查顺序：
1. 视图是否合法（否则返回 InvalidView）。
2. 返回的 ID 列表是否非空（否则 NoMatch）。
3. 返回的 ID 列表是否恰好一个元素（否则 Ambiguous）。
4. 返回的 ID 是否在视图中存在（否则 InvalidCandidate）。
5. 通过则返回 Unique + selected。

### Engine::planDynamic()

```cpp
std::optional<DynamicPlan> Engine::planDynamic(
    const DeclarationView& view,
    const std::string& selectorDeclarationId,
    std::string& error) const;
```

为动态选择场景生成 DynamicPlan。除视图合法性检查外，还要求所有候选的 retention 不为 CompileTime（即必须至少是 Runtime），且候选列表非空。

## 与周边文件·阶段的关系

```
Selector.h -> core/TypeSystem.h (TypePtr)
          -> core/TypeIdentity.h (Type 类型定义)
          -> Selector.cpp (实现)
```

- core/TypeIdentity.h：定义了 TypePtr = std::shared_ptr<Type> 和 Type 的前向声明，Candidate::callableType 的类型来源。
- Selector.cpp：DeclarationView 构造函数的不变量验证和 Engine::validate()/planDynamic() 的完整实现。
- 选择阶段在编译器流水线中的位置：语义分析（Sema）完成声明解析后，选择器子系统负责从同一声明族中选出唯一声明，然后进入代码生成。

## 延伸阅读

- Selector.cpp：声明视图构造与引擎验证的实现细节
- core/TypeIdentity.h：TypePtr 与类型身份系统
- core/TypeSystem.h：Luna 类型系统核心定义
- instantiation/Instantiator.h：泛型实例化器，与选择器协同处理泛型声明的选择

---
