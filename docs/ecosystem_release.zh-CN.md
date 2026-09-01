# 生态发布快照

`ecosystem.lock.json` 是连接 Luna、LunaToolchain 与 Lunax 三个独立仓库的权威快照。
当前入库的快照故意冻结在 0.2.1 baseline；它是该发布线的证据，不表示当前
0.3 编译器生态已就绪。Luna 组件由“包含该 lock 文件的 commit”标识；子组件使用
精确 Git commit。
语言版本、诊断协议和分析协议与各组件 package 版本分别记录。
对于已经发布的子组件，`commit` 跟踪当前验证源码，`published_release.commit` 则记录公开
制品所对应的不可变 commit。快照同时保留 release URL、发布时间、checksum manifest 摘要
以及每个制品的摘要作为证据。只有干净 runner 下载、校验、解包并运行所有受支持平台的
package 后，consumer verification 才会从 pending 升级。

当 `release.publish` 为 false 时，该快照不可发布。升级前必须设置
`status: release-ready`，lock 中语言版本及所有 Luna 兼容 tag 必须精确等于根仓
`VERSION`，并通过根仓平台门禁、toolchain 强制真实编译器集成、Lunax 事务安装集成，
以及所有发布产物的 checksum 与 attestation 验证。release tag 本身永远不允许用不同
内容替换已有产物。

0.3 使用两阶段提升，避免 Luna tag 与子组件 release 互相等待：

1. 先提交完整的 Luna 源码、测试和发布工作流，得到候选 commit，但不创建 `v0.3.0` tag；
2. 分别提交并创建 toolchain/Lunax 的 release tag，通过手动 release workflow 把该 Luna
   候选 commit 作为 `luna_ref`；工作流解析并发布实际验证的 40 位 commit，不能只记录分支；
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

## 发布交接决策登记表（2026-08-31）

2026-08-31 复核重新打开了 Slot/Fragment 的 `TBD-SF007`–`TBD-SF010`：
已完成的是 static lexical slice，不是完整 runtime model。因此候选提交已暂停，
先解决阻断项，再处理下表的产物授权、发布范围与延后项：

| ID | 需要确认的内容 | 当前已编码默认 | 建议 | 是否阻断 0.3 发布 |
|---|---|---|---|---|
| `RLS001` | 候选提交拓扑 | 三个独立工作树均未提交 | 每个仓库创建一个经完整验证的 candidate commit；根仓稍后另建一个仅包含 lock/状态的 promotion commit，不拆出未单独测试的中间语义提交 | 是；必须在 push/tag 前确认 |
| `RLS002` | GitHub release 可见性 | 根 `v0.3.0` 与 Lunax `v0.2.0` 为 prerelease；Toolchain `v0.2.0` 为普通 release | 保持当前三个 workflow 的等级；如需统一，必须在 candidate commit 前修改并重跑门禁 | 是 |
| `RLS003` | 外部写操作授权 | 未 commit、push、tag 或 publish | 按两阶段顺序一次授权：candidate commits → push/CI → 子组件 tags/releases → lock promotion → Luna tag/release | 是 |
| `RLS004` | 真实 CUDA/ROCm 性能证据是否为发布门 | release workflow 用 `-LE hardware` 明确排除硬件测试；simulator/AOT 门已通过 | 保持为非阻断的独立性能证据，不将某块 GPU 变成 0.3 发布前置条件 | 否 |
| `RLS005` | VS Code test selection、workspace status 和 cache report 是否进入 0.3 | Luna/Lunax 尚无对应所有者协议，editor 不猜测 | 显式延后到 0.3 之后；0.3 只发布已有编译器语义的 check/build/run task | 否 |
| `RLS006` | 总体设计文档状态 | 仍标记 `Draft`，且 `TBD-SF007`–`TBD-SF010` 已重新登记 | 阻断项关闭且 candidate 被接受时改为 `Accepted release candidate`；最终 tag 后再改为已发布状态 | 是 |
| `RLS007` | attestation 服务临时失败时的策略 | 每项有限重试 5 次，仍失败则 fail closed | 等待/重跑 GitHub/Sigstore 服务，不允许手工绕过或仅依赖 checksum | 是，直到联网门禁通过 |
| `RLS008` | Slot/Fragment 设计收口 | static slice 已实现，runtime scope、同 slot 嵌套/重入与 descriptor 承诺未冻结 | 决定并实现 `TBD-SF007`–`TBD-SF009`；若选 static-only，将 `TBD-SF010` 明确延后 | 是 |

收口 Slot/Fragment 决策后，发布执行顺序为：

1. 关闭 `TBD-SF007`–`TBD-SF009`，按决定补齐实现、负例与规范，并处理 `TBD-SF010`；
2. 最后审查三个 diff，创建并 push 三个 candidate commit，等待远程 CI；
3. 以精确 Luna candidate SHA 分别发布 Toolchain/Lunax，不使用可变分支名；
4. 下载每个资产并通过 consumer、checksum、source-commit 和 attestation 门；
5. 一次性替换 lock 中两个子组件的 version、commit、URL、时间、资产摘要与
   `verified_luna_source_commit`，设置 `status: release-ready` 和 `release.publish: true`；
6. 通过 strict readiness 与联网 evidence 门，提交 lock promotion，最后创建
   `v0.3.0` 并触发根仓 prerelease。
