# src/sema/SemanticContext.cpp — 语义分析主流程（多趟调度）与转发

> 一句话定位：实现 `SemanticContext::analyze` 的多趟分析管线（链接名分配 → 声明收集 → impl 校验 → 体分析 → 推断收尾），以及把各组件接口转发给已绑定实现的「接线代码」。

## 这个文件做什么

这是 Sema 的「总指挥」。`analyze(Program*)` 按依赖顺序驱动五组件，保证前向引用与声明顺序无关：

1. 初始化/清洗所有状态（const 作用域、slot/apply 作用域、各注册表）。
2. 内置契约注册：`Drop`（编译器保留）、`From`（luna.compiler）。
3. 包 use 别名表（`mPackageAliases`）。
4. **链接名分配**：遍历所有声明，为每个声明生成 `generatedSymbolName`（`main` 特殊处理，重名/带元数据用 `isolatedLinkageName`），并查包级重名。
5. `declareMeta`（元数据 schema 先注册）。
6. `declareConstraint`（约束名先注册，where 子句顺序无关）。
7. 名义类型预绑定：`Type::makeStruct/makeEnum` 写入 `mDeclaredTypes`/`mSymTable`（支持前向引用）。
8. `declareTrait` → `declareStruct/declareEnum`（含 `validateMetadata`）→ `declareFunction/declareFragment/declareImpl`。
9. `analyzeTrait` → `analyzeImpl`（先于普通体，保证 Drop 契约不依赖源码顺序）→ `analyzeFunction/Struct/Enum/Meta/Constraint`。
10. 推断收尾：`defaultUnconstrainedNumeric()` → 逐声明 `checkUnresolved` → `mInferenceRoots` → `materializeInferredTypes`。

后半部分是大量转发方法：`SemanticContext` 上的 `declareXxx`/`analyzeXxx`/`resolveTypeAST`/`constrain`/`evaluateConstXxx` 等一律转发给 `mXxxAnalysis` 指针（已在构造函数绑定）。

C++ 类比：一个编译单元的「翻译单元级上下文 + 调度器」：先登记所有声明（像 C++ 的先声明后使用），再逐类做检查，最后统一推断收尾。

## 关键结构体·类·枚举

无新类型（结构在 .h）；这里包含：
- 文件内 lambda/局部状态：`declaredNames`/`linkageNameCounts`（重名统计）、`rootEntryCount`（唯一 main 校验）。
- 方法内部借用的 `SemanticContext` 私有成员（见 .h）。

## 关键函数·方法

- `SemanticContext()`：注册内置类型与 `print`。
- `analyze(Program*)`：如上多趟管线；返回 `mErrors.empty()`。
- 链接名分配细节：`metadataDeclarationName(name, decl)` 生成基础链接名；`isRootEntry`（`main`）→ `"main"`；重复或 `main` 冲突 → `isolatedLinkageName(familyKey + "::" + sourceLinkage, sourceLinkage)`；并做 `Duplicate package declaration`/`more than one main` 诊断。
- `resolveTraitRef(TraitRef&, useSite)`：`Drop`/`From` 特判（编译器保留 id），否则查 `mTraits` 并 `recordDeclarationReference`。
- `satisfiesTrait(traitId, type)`：`mImpls` + `mTraitMethods` 双查。
- `recordDeclarationReference`/`recordResolvedReference`：写 `mDeclarationReferences`（IDE 跳转），带有效性过滤。
- `error(msg, line, col)`：按消息关键词选 hint（undefined name/FFI/selector/not callable/integer/const/constraint/type_ 等），用 `diagnostic::format("semantic", ...)` 入队。
- `setDiagnosticLocation`/`setDeclarationContext`：维护当前诊断位置与包/模块上下文。
- `sourceDeclarationKey(name, diagnoseVisibility)`：处理 `::` 限定名、包别名、模块路径，做可见性诊断。
- `lookupSymbol`/`lookupDeclaredType`：词法绑定/内置优先，其次限定键查表。
- 其余全是转发：`declareFunction`→`mDeclarationAnalysis->declareFunction`、`analyzeFunction`→`mBodyAnalysis`、`analyzeSlotDecl`→`mControlAnalysis`、`resolveTypeAST`→`mTypeAnalysis`、`evaluateConstExpr`→`mCompileTimeAnalysis` 等。

## 与周边文件·阶段的关系

- `SemanticAnalyzer::analyze` 转发到这里；组件实现在各自 .cpp。
- `SemanticContextAccess.h/.cpp` 提供组件访问面。
- `SemanticAnalysisSupport.h` 提供身份/链接名/键工具（`nominalDeclarationIdentity`/`isolatedLinkageName`/`qualifiedDeclarationKey` 等）。
- 输出被 MoonIR 生成与后续独立检查器消费。

## 延伸阅读

- `SemanticContext.h`（接口与字段）。
- `DeclarationCollector.cpp`（声明收集细节）、`BodyAnalyzer.cpp`（体分析）、`TypeResolver.cpp`（推断收尾）。
- `SemanticAnalysisSupport.h`（链接名/身份工具）。


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticContext.h
lang: zh-CN
audience: 学过 C/C++、想了解 Luna 语义分析核心状态的读者
---

# src/sema/SemanticContext.h — 语义分析的「状态中心」与五大分析接口

> 一句话定位：`SemanticContext` 是所有语义分析共享状态的宿主（符号表、impls、trait、推断器、诊断、片段/槽表等），同时声明五个分析接口（Body/Type/CompileTime/Declaration/Control Analysis）与 `analyze(Program*)` 主入口。

## 这个文件做什么

这是 Sema 最核心的头文件。它：

1. 声明 `SemanticContext` 类——语义分析运行期的「全局状态」：所有组件共享的字段都在这（`mSymTable`、`mImpls`、`mTraits`、`mConstraints`、`mErrors`、`mDeclarationReferences`、const/selector 相关状态、slot/apply 状态等）。
2. 声明五个纯虚分析接口（`BodyAnalysis`、`TypeAnalysis`、`CompileTimeAnalysis`、`DeclarationAnalysis`、`ControlAnalysis`）——语义分析被拆成五个可独立实现的组件，`SemanticContext` 只依赖接口。
3. 声明 `SemanticContext` 的 `analyze` 主流程与大量转发方法：每个组件在 `SemanticContext` 上都有一个同名转发方法，把调用路由到已绑定的组件。
4. 声明多个内部数据结构：`FromConversion`、`FromIteratorImplementation`、`SlotInfo`。

C++ 类比：`SemanticContext` ≈ 编译器的「编译单元上下文（TranslationUnit Context）」，五接口 ≈ 五个职责单一的子系统（类型、体、编译期、声明、控制流），上下文用依赖注入把它们连起来。

## 关键结构体·类·枚举

五大接口（全部纯虚，供对应组件实现）：

- `class BodyAnalysis`：`analyzeFunction/Struct/Enum/Trait/Impl`、`analyzeStmt/Block/Expr/Call/MemberCall/IteratorCall/Launch/Select`、`statementAlwaysReturns`/`blockAlwaysReturns`。
- `class TypeAnalysis`：`findMatchingImpl`/`monomorphize`/`resolveTypeAST`/`instantiateNominal`/`declaredType`/`resolved`/`constrain`/`requireBool/Numeric/Integer`/`checkUnresolved`/`typeToAST`/`materializeInferredTypes`。
- `class CompileTimeAnalysis`：`analyzeReflectionCall`/`analyzeDeclarationReflectionCall`/const 作用域与查找/`evaluateConstExpr`/`evaluateConstFunction`/`evaluateConstBlock`/constraint 与 selector 求值。
- `class DeclarationAnalysis`：`declareFunction/Meta/Constraint/Struct/Enum/Trait/Impl/Fragment`、`analyzeConstraint/Meta`、`validateMetadata`、`isFFIType`/`validateFFIFunction`。
- `class ControlAnalysis`：`analyzeSlotDecl/SlotInvoke/Apply`、`analyzeFragmentForSlot`、`enter/exitSlotScope`、`selectFragment`。

`SemanticContext` 内部结构：

- `struct FromConversion`：`From` trait 的一对一转换记录（source/target/`FunctionDecl*`/symbol）。
- `struct FromIteratorImplementation`：`FromIterator` 的 builder 协议（item/builder/target + begin/push/finish）。
- `struct SlotInfo`：槽位声明摘要（参数类型/契约/默认片段/种类/基数/动态性等）。
- 字段群：`mSymTable`、`mMetadataSchemas`、`mConcepts`、`mFunctionFamilies`、`mQualifiedDeclarations`、`mPackageAliases`、`mImpls`、`mFromConversions`、`mFromIteratorImplementations`、`mTraitTypeParams`/`mTraitMethods`/`mTraitOwners`/`mTraits`、`mDeclaredTypes`、`mGeneratedInstances`/`mInstantiator`/`mInstantiatedFunctions`、`mErrors`、`mDeclarationReferences`、当前包/模块/诊断位置、`mCurrentReturnType`/`mInFunction`/`mInKernel`/`mSawReturn`/`mConstraints`/`mInferenceRoots`/`mConstScopes`/`mConstexprFunctions`/`mConstEvaluationDepth`、`mSlotScopes`/`mApplyScopes`/`mDynamicApplyScopes`/`mFragments`/`mCurrentFragmentSlot`/`mCurrentFragmentDecl`、`mIteratorStateCounter`、`mActiveSelectorView`。

## 关键函数·方法

- 构造 `SemanticContext()`：注册内置类型（i32/i64/f32/f64/bool/string）与 `print` 内置函数。
- `analyze(Program*)`：主入口，多趟驱动（详见 .cpp 指南）。
- `bodyAccess()`/`compileTimeAccess()`/`controlAccess()`/`declarationAccess()`/`typeAccess()`：返回各组件专属的 access 引用（friend 类 `BodyContextAccess` 等）。
- `bindBodyAnalysis` 等五个 `bind*` 方法：注入组件实现。
- `errors()`/`symTable()`/`declarationReferences()`：对外访问器。
- 大量转发方法：`declareXxx`→`mDeclarationAnalysis`、`analyzeXxx`→`mBodyAnalysis`/`mControlAnalysis`、`resolveTypeAST`/`constrain`/`requireXxx`→`mTypeAnalysis`、`evaluateConstXxx`/`enterConstScope`→`mCompileTimeAnalysis` 等。
- 私有工具：`resolveTraitRef`/`typeIdentity`/`traitIdentity`/`satisfiesTrait`/`findMatchingImpl`/`monomorphize`/`error`/`setDiagnosticLocation`/`setDeclarationContext`/`sourceDeclarationKey`/`lookupSymbol`/`lookupDeclaredType`/`recordDeclarationReference`/`recordResolvedReference`。

## 与周边文件·阶段的关系

- `SemanticAnalyzer.cpp` 装配：构造 `SemanticContext` + 五组件并互相绑定。
- `SemanticContextAccess.h/.cpp`：给组件暴露字段/方法的「访问面」。
- 各组件实现：`BodyAnalyzer`（BodyAnalysis）、`TypeResolver`（TypeAnalysis）、`CompileTimeEvaluator`（CompileTimeAnalysis）、`DeclarationCollector`（DeclarationAnalysis）、`ControlAnalyzer`（ControlAnalysis）。
- `analyze` 的多趟调度（声明收集 → impl 校验 → 体分析 → 推断收尾）把整条 Sema 主线串起来。

## 延伸阅读

- `SemanticContext.cpp`（analyze 主流程）、`SemanticContextAccess.h`（访问封装）、`SemanticAnalyzer.h/.cpp`（门面）。
- 五个组件各自的 .h/.cpp。
- 总览：`docs/starter/sema.zh-CN.md`。


---

---
kind: source-file-guide
module: sema
source: src/sema/SemanticContextAccess.cpp
lang: zh-CN
audience: 学过 C/C++、想读 Sema 组件接线实现的读者
---
