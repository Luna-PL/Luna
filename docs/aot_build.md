# AOT 构建与运行时库

`build` 先发射 LLVM IR，再调用 C++ 链接器生成本机可执行文件：

```sh
./build/luna build examples/operators.luna -O2
```

默认情况下，开发构建会使用编译器自身构建目录中的 `libruntime.a` 与 `clang++`。为部署、打包或交叉环境提供可复现边界，可显式指定：

```sh
./build/luna build app.luna \
  --runtime-lib /opt/luna/lib/libruntime.a \
  --cc /usr/bin/clang++ \
  --link m
```

也可设置 `LUNA_RUNTIME_LIB` 和 `LUNA_CXX` 作为默认值；命令行参数优先。`--link` 可重复使用，接受库路径或库名。

找不到运行时库时，驱动会报 `DRV0001`，链接器失败会报 `DRV0002`，并保留完整链接命令以便复现。AOT 可执行文件自身也会在进入 Luna `main` 时初始化所选 GPU 后端；初始化失败会打印具体后端和动态加载错误，再以状态码 `1` 退出，不会静默退回模拟器。
