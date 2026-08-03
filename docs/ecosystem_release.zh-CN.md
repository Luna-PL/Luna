# 生态发布快照

`ecosystem.lock.json` 是连接 Luna、LunaToolchain 与 Lunax 三个独立仓库的权威候选
快照。Luna 组件由“包含该 lock 文件的 commit”标识；子组件使用精确 Git commit。
语言版本、诊断协议和分析协议与各组件 package 版本分别记录。

当 `release.publish` 为 false 时，该快照仍是 candidate。升级为可发布状态前，必须通过
根仓库平台门禁、toolchain 强制真实编译器集成、Lunax 事务安装集成，以及所有发布产物
的 checksum 验证。release tag 本身永远不允许用不同内容替换已有产物。

在相邻本地 checkout 中，构建 Luna 后可验证完整快照：

```sh
cmake -DLUNA_SOURCE_DIR="$PWD" \
  -DLUNA_EXECUTABLE="$PWD/build-stage-a-strict/luna" \
  -P tools/verify_ecosystem_lock.cmake
```

验证器检查子仓库 commit 与 clean worktree、组件版本与兼容声明，以及编译器自报源码
commit；它不执行网络访问或修改操作。
