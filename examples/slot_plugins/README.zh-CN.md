# Loop-local slot plugins

这个示例展示类型安全的运行时插件织入点：

```text
dynamic slot context hook(value: i32);
dynamic apply hook(log, audit) { ... }
```

`hook` 位于 `while` 循环体内，每次迭代都会重新调用。`log` 和 `audit`
是静态链接进程序的候选 context，运行时通过环境变量选择：

```sh
LUNA_GPU_BACKEND=sim ./build/luna run \
  examples/slot_plugins/loop_plugins.luna

LUNA_GPU_BACKEND=sim LUNA_FRAGMENT_HOOK=audit \
  ./build/luna run examples/slot_plugins/loop_plugins.luna
```

默认候选是 `log`。两个插件都使用相同的 `context(value: i32)` 接口和
`resume()`，因此每次迭代会先输出插件事件，再输出槽续体中的 `10 + i`。

当前动态插件边界是“已链接候选 + 运行时选择”，还不是 `.so/.dll` 热加载。
循环内 slot 可以重复调用，但插件不能在循环中消费外层线性资源；这类
状态变化会被 ownership checker 拒绝。

