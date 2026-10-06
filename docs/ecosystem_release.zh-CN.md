# 生态发布快照

`ecosystem.lock.json` 是连接 Luna、LunaToolchain 与 Lunax 三个独立仓库的权威快照。
当前入库的快照声明为已提升的 0.3.0 生态发布候选，仍须通过下述提交谱系与
readiness 检查。Luna 组件由“包含该 lock 文件的
commit”标识；子组件使用精确 Git commit。
语言版本、诊断协议和分析协议与各组件 package 版本分别记录。
对于已经发布的子组件，`commit` 跟踪当前验证源码，`published_release.commit` 则记录公开
制品所对应的不可变 commit。快照同时保留 release URL、发布时间、checksum manifest 摘要
以及每个制品的摘要作为证据。当前 lock 已记录子组件 consumer verification 与
attestation 通过；这些记录对应子组件 release 固定的 Luna 源码候选。

当 `release.publish` 为 false 时，该快照不可发布。升级前必须设置
`status: release-ready`，lock 中语言版本及所有 Luna 兼容 tag 必须精确等于根仓
`VERSION`，并通过根仓平台门禁、toolchain 强制真实编译器集成、Lunax 事务安装集成，
以及所有发布产物的 checksum 与 attestation 验证。release tag 本身永远不允许用不同
内容替换已有产物。

0.3 使用两阶段提升，避免 Luna tag 与子组件 release 互相等待：

1. 先提交完整的 Luna 源码、测试和发布工作流，得到候选 commit，但不创建 `v0.3.0` tag；
2. 分别提交并创建 toolchain/Lunax 的 release tag；各自 compatibility manifest 用
   `source_commit` 固定该 Luna 候选，release workflow 读取并发布这个 40 位 commit，
   不把可变分支名保留为证据；
3. 完成子组件 consumer、checksum 和 attestation 验证，把同一个 commit 写入两组件的
   `verified_luna_source_commit`，并录入不可变 release 证据；
4. 提交 lock 提升，再创建最终 Luna tag。候选 commit 之后只允许修改
   `ecosystem.lock.json`、`CHANGELOG.md` 和本文/0.3 设计状态文档；发布门会验证 ancestry
   和该 allowlist，任何 compiler、runtime、stdlib、测试或 workflow 变化都要求重新生成
   子组件证据。

因此 Luna 最终 tag 无需预先存在，子组件也不会针对一个未来会被移动的 tag 构建。
toolchain 的 `SHA256SUMS` 与 Lunax 的逐项 checksum 都包含 `LUNA-SOURCE-COMMIT`，使 lock
中的候选身份可以由已证明的公开制品复核。

在相邻本地 checkout 中，可以不探测当前 0.3 编译器，直接验证冻结的子仓快照：

```sh
cmake -DLUNA_SOURCE_DIR="$PWD" \
  -P tools/verify_ecosystem_lock.cmake
```

验证器检查子仓库 commit 与 clean worktree、组件版本与兼容声明；它不执行网络
访问或修改操作。`LUNA_EXECUTABLE` 只是可选的附加探测，且该二进制必须属于
快照记录的语言版本；不得把当前 0.3 二进制与冻结的 0.2.1 baseline 比较。

本地发布策略门禁单独运行，因为历史快照可以是有效的，却不适用于当前编译器：

```sh
cmake -DLUNA_SOURCE_DIR="$PWD" \
  -P tools/verify_release_readiness.cmake
```

该命令会报告发布被阻断的原因，并在 fail-closed 策略正常生效时成功。strict
模式还会核对 0.3 snapshot 名、Toolchain/Lunax 0.2.0 version、tag、URL、source/published
commit 一致性、consumer/attestation 状态，以及两个 workflow 会产生的精确资产名和
SHA-256 形状，因而只修改 `status` 而保留 0.1.2 证据不会被误报为 ready。
prebuilt-release workflow 使用同一脚本的 `-DREQUIRE_READY=ON` 模式，因此在快照
显式升级且三个组件全部指向当前 Luna 版本前，tag 不能开始打包。

`Release evidence` workflow 是对应的联网门禁。它通过 `gh` 获取 lock 指定的每个 release，
核对 release URL、发布时间、状态和 tag commit，要求资产名称集合完全一致，将 GitHub 提供
的资产摘要与 lock 对比，并使用下载的 checksum 文件复核所记录的制品。任何不一致都会
阻止候选升级。它还要求每项资产的 GitHub/Sigstore attestation 均由对应组件的 release
workflow 在 GitHub 托管 runner 上签发；该门禁通过前，不得把 release 证据人工复制进
lock。

根仓 prebuilt-release workflow 不只信任 `status` 字段：readiness 通过后，它会在任何平台
打包前重新下载两组件的完整公开资产集合，解析轻量或 annotated tag 到最终 commit，复核
checksum、`LUNA-SOURCE-COMMIT` 和 GitHub/Sigstore attestation。独立 Release evidence
workflow 与最终 tag 发布使用同一个验证脚本，避免两套门禁随时间漂移。

## 当前提交谱系与下一发布门（2026-10-06）

入库的 lock 已录入 Toolchains 与 Lunax `v0.2.0` release，并将快照标为
`release-ready`、`release.publish: true`。两项子组件 release 已发布，但它们共同
验证的 Luna 源码提交 `41ce85e` 不是 Luna `main` 上实施提交
`9af653f` 的祖先。
本地 `verify_release_readiness.cmake` 明确报告此阻断，严格的
`REQUIRE_READY=ON` 模式失败；非严格模式退出成功只表示阻断策略生效，不表示
允许发布。根仓 `v0.3.0` tag 尚不存在。本地核对没有重跑
联网制品／attestation 验证。

2026-10-03 本地 CLANG64 非硬件测试以四个 worker 通过 76／77 项；
`luna.repl-smoke` 在并行负载下于进程树清理阶段超时，单独重跑通过。随后按 JIT
准备过程调整其时间上限，保留清理断言。2026-10-04 完整测试以四个 worker
通过 77／77 项。
2026-10-06 重建当前工作树后，同一套本地测试以四个 worker 在 71.18 秒内通过
77／77 项，包括 REPL 和 Native artifact 门禁。只读 release-readiness 检查再次报告
`41ce85e` 不是当时的 `8fae950` 的祖先；退出成功只说明 fail-closed 策略正确
阻断发布。该次测试早于提交 `a0bf2b5`，属于工作树证据，不是远程 CI。
隔离的 WSL Arch Linux Clang／LLVM 22.1.8 构建完成全部目标，并以四个 worker
在 67.15 秒内通过本地 Linux 完整 CTest 76／76 项。首次运行通过 75／76 项：
`luna.ecosystem-frozen-baseline` 因 WSL Git 未继承 Windows 系统的
`core.autocrlf=true` 设置，把 Windows 检出文件的 CRLF 误判为修改。在该设置下
两个子工作树均为干净状态；通过仅对测试进程设置 Git 配置，失败项和整套复测均通过。
这不能替代发布候选所需的平台 CI。
实施工作树增加实验性的并行 Native v2 `i32()` 入口 profile，同时保留 v1 proof／
导出兼容性。Windows CLANG64 和 WSL Arch Linux 的 Native artifact、canonical
聚焦测试均通过。后续完整本地测试分别通过 Windows 76／77、Linux 75／76；
两端唯一失败都是文件指南清单遗漏新增的辅助头文件。补齐清单后，两端该项复测
均通过。这些是本地工作树结果，不构成不可变发布候选或远程 CI 的证据。
下一版工作树把已验证的 v2 profile 贯穿 Native generation binding 和类型化固定
调用，并检查 load-once／切换期间的 profile 稳定性。重新封装的仅 v1 query 变体
可走旧加载路径，但不能满足类型化 requirement。重新构建全部目标后，本地 CLANG64
完整测试以 66.69 秒通过 77／77，WSL Arch Linux 以 90.60 秒通过 76／76。
这些结果仍不构成不可变候选或远程平台证据。
随后独立编译纯 v1 C 库并生成其自身的 proof／trust。独立 proof oracle、v1 调用、
无 profile generation 与类型化 lookup 拒绝在两端 Native artifact 聚焦 CTest
中通过；前述完整测试计数早于这一夹具。
v2 候选现固定 64 位 C 记录偏移及 SHA-256 行编码；独立重新封装的未知
profile 制品会被拒绝。加入这些检查后，两端 Native artifact、MoonRuntime 与
runtime-ABI 聚焦测试均通过。宿主 ABI 审阅仍待完成。
发布包清单与 CI 工作流覆盖 64 位 Linux、Windows 和 macOS runner 架构，
没有列出 32 位发布包。macOS 工作流已包含 Native artifact CTest。
GNU/Linux、Windows GNU、Darwin 目标的 32 位
freestanding C 布局探针现已接入 Clang CTest；本地 Windows CLANG64 与 WSL
Arch Linux 的三项探针和文件清单测试均通过；接入后的完整非硬件 CTest
分别通过 80／80 和 79／79。提交 `a0bf2b5` 包含这些实现和测试证据，
现已位于远端 `main`；只读就绪检查仍报告子组件验证的 `41ce85e` 不在其祖先链上，
严格模式按预期失败。探针不验证 32 位加载器或制品。
其后的提交 `9af653f` 只增加私有三节点分叉 owner 清理证明，覆盖 `?` Err、注入的
body 后失败与宿主 Drop；独立封闭的四节点分叉在私有 JIT 物化前被有界形状
门禁拒绝。Windows CLANG64 与 WSL Arch Linux 的普通及 ASAN
canonical 聚焦测试均通过。在 `9af653f` 重新构建全部目标后，Windows CLANG64
和 WSL Arch Linux 的完整非硬件测试分别通过 80／80、79／79。两个提交均已推送至
远端 `main`，触发了 [Linux CI](https://github.com/Luna-PL/Luna/actions/runs/37459306824)、
[Windows CI](https://github.com/Luna-PL/Luna/actions/runs/37459306797) 和
[macOS CI](https://github.com/Luna-PL/Luna/actions/runs/37459306874)。
Linux 与 Windows 已通过。macOS 仅 `luna.native-artifact` CTest 失败：独立 Python
Native 消费者返回非零，但测试脚本未打印退出码。
[诊断复跑](https://github.com/Luna-PL/Luna/actions/runs/37461051981)
确定退出码 24，位于通过改写已链接动态库字节构造的“仅 v1 query”合成制品分支。
现已删除该重复夹具；独立编译且封装的纯 v1 C 库仍验证 v1 加载、generation 与
类型化拒绝。修正后的测试尚需 macOS CI 证据。下一道 ABI 门是在不可变提交上完成平台 CI，
并取得宿主对并行 query／版本规则的审阅；增加 32 位支持
需要独立的运行时门禁。
下一步建立包含预期源码、测试与工作流的全新不可变 Luna 候选。子组件要
针对这个精确提交重新生成并验证发布证据，不能移动既有 tag 或复用其源码提交声明。
随后将匹配的证据写入 lock，通过根仓平台 CI、严格 readiness 与联网 Release evidence
门禁，最后才创建 Luna tag。只修改 lock 状态不能修复当前候选谱系。

## 发布交接决策登记表（2026-09-15）

2026-09-15 复核时让 Slot/Fragment 的 `TBD-SF007`–`TBD-SF010` 保持开放：
那一检查点仅完成 static lexical slice。因此 Slot/Fragment 被排除在 0.3 核心冻结之外，
而不是继续阻断核心冻结。下表保留当时的候选状态、产物授权、发布范围与明确延后项，
并非 2026-09-25 runtime 实施状态的实时报告：

| ID | 需要确认的内容 | 当前已编码默认 | 建议 | 是否阻断 0.3 发布 |
|---|---|---|---|---|
| `RLS001` | 候选提交拓扑 | 本地候选已建立：Luna `41ce85e`、Toolchains `63c8fe1`、Lunax `42285e1`；本次纯状态更新前三个工作树均清洁 | 保留 Luna 语义候选；根仓稍后另建仅包含 lock/状态的 promotion commit | 否；本地已完成 |
| `RLS002` | GitHub release 可见性 | 根 `v0.3.0` 与 Lunax `v0.2.0` 为 prerelease；Toolchains `v0.2.0` 为普通 release | 保持当前三个 workflow 的等级；如需统一，必须在子组件 tag 前修改并重跑门禁 | 是；tag 前确认 |
| `RLS003` | 外部写操作授权 | 本地 commit 已建立；尚未 push、tag 或 publish | 明确授权剩余顺序：push/CI → 子组件 tags/releases → lock promotion → Luna tag/release | 是 |
| `RLS004` | 真实 CUDA/ROCm 性能证据是否为发布门 | release workflow 用 `-LE hardware` 明确排除硬件测试；simulator/AOT 门已通过 | 保持为非阻断的独立性能证据，不将某块 GPU 变成 0.3 发布前置条件 | 否 |
| `RLS005` | VS Code test selection、workspace status 和 cache report 是否进入 0.3 | Luna/Lunax 尚无对应所有者协议，editor 不猜测 | 显式延后到 0.3 之后；0.3 只发布已有编译器语义的 check/build/run task | 否 |
| `RLS006` | 总体设计文档状态 | Slot/Fragment 尚开放，因此总体仍为 `Draft`；单独记录的核心冻结已有本地候选 | 保持总体设计为 `Draft`，独立提升核心快照；仅在 Slot/Fragment 收口后修改总体状态 | 对核心候选否；对宣称整体设计稳定是 |
| `RLS007` | attestation 服务临时失败时的策略 | 每项有限重试 5 次，仍失败则 fail closed | 等待/重跑 GitHub/Sigstore 服务，不允许手工绕过或仅依赖 checksum | 是，直到联网门禁通过 |
| `RLS008` | Slot/Fragment 设计收口 | static slice 已实现；runtime scope、同 slot 嵌套/重入与 descriptor 承诺保持开放 | 让 `TBD-SF007`–`TBD-SF010` 保持开放并排除在核心冻结契约之外；发布稳定 Slot/Fragment 语义前再收口 | 对核心/alpha 发布否；对稳定 Slot/Fragment 是 |

Slot/Fragment 已明确排除在核心冻结之外，发布执行顺序为：

1. 在当时检查点，保持 `TBD-SF007`–`TBD-SF010` 为开放的 Slot/Fragment 工作，不扩张已冻结的核心候选；
2. push 三个现有 candidate commit，等待远程 CI；
3. 按 compatibility manifest 记录的精确 Luna candidate SHA 分别发布 Toolchain/Lunax，
   不使用可变分支名；
4. 下载每个资产并通过 consumer、checksum、source-commit 和 attestation 门；
5. 一次性替换 lock 中两个子组件的 version、commit、URL、时间、资产摘要与
   `verified_luna_source_commit`，设置 `status: release-ready` 和 `release.publish: true`；
6. 通过 strict readiness 与联网 evidence 门，提交 lock promotion，最后创建
   `v0.3.0` 并触发根仓 prerelease。

### Slot/Fragment 状态修订（2026-09-25）

SFR001 与[运行时注入计划](slot_fragment_runtime_plan.zh-CN.md)取代了历史 SF006 语法，
并解决 `TBD-SF007` 和 `TBD-SF009` 的 runtime 范围及保留规则选择。
`TBD-SF010` 的有界首版 ABI 已实现：已验证的 runtime Fragment 引用、固定 generation 的
候选快照、宿主选定的 BindingSet、safe-point 激活、显式 execution context 与跨包动态
Slot dispatch。`TBD-SF008` 已有宿主定序 chain 与局部 override，但同 Fragment 重入尚未
冻结为稳定语言承诺。带 context 的间接调用和非 Copy 的 exported Slot 契约不在首版 ABI 内。
本次修订不扩大 0.3 核心冻结，也不单独授权稳定版 Slot/Fragment 发布；跨平台 CI 与独立
性能/稳定性证据仍是另外的门禁。

[SF008 有界规则（2026-09-26）](slot_fragment_contract.zh-CN.md)现已集中记录静态环拒绝、
有限同 Slot 续体嵌套、独立 runtime activation、内层 override／None 隔离，以及 v1
published handler 不接收 context 的边界，并对应可执行回归证据。`TBD-SF008` 对有界规则
的稳定承诺接受及任何 handler context／重入扩展仍保持开放。已实现有界行为不等于
稳定版发布授权，也不重新扩大核心冻结边界。
