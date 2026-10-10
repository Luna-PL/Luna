# Contributing / 贡献指南

Read [the file and responsibility guide](docs/file_guide.md) before changing implementation files. / 修改实现文件前，请先阅读[文件与职责指南](docs/file_guide.zh-CN.md)。

## Commit subjects / 提交标题

Use the repository's bracketed format for new commits: `[Type] short imperative description`. Keep one space after the closing bracket and describe the actual change. / 新提交使用 `[类型] 简短祈使句` 的英文标题，右方括号后留一个空格，准确描述改动。

Choose `Feature`, `Fix`, `Docs`, `Refactor`, `Hardening`, `Test`, `Release`, `Control`, `Benchmark`, `Build`, `CI`, or `Chore`. For example:

```text
[Feature] add owned Result host transfer
[Fix] resolve Windows LLVM library ordering
[Docs] record the JIT relocation probe result
```

The commit check covers commits newly introduced by a push to `main` or by a pull request. Earlier mixed-format commits are historical records. / 检查仅覆盖新推送到 `main` 或新进入拉取请求的提交；历史格式不一的提交仍作为原始记录保留。

## Attribution / 贡献归属

Set the Git author email to an address associated with your GitHub account before committing. Add a `Co-authored-by` trailer only when the named account actually co-authored the change and the attribution is intended. Tool assistance alone does not establish co-authorship. / 提交前请使用已关联 GitHub 账号的作者邮箱。只有在对方确实共同创作且需要署名时，才添加 `Co-authored-by`；仅使用工具不自动构成共同作者。
