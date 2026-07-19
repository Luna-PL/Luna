# 包与显式导出

## 中文说明

包由同一目录中的多个 `.luna` 文件组成，文件按名字排序后合并分析。只有写出
`export` 的声明进入公共接口；未导出的函数、类型或片段仍属于包内部。Alpha 暂不
提供跨包依赖、锁文件和远程仓库解析。

一个包是同一目录下所有 `.luna` 文件的集合。目录会按文件名排序后加载；每个文件应使用相同的 `package` 名称。目录没有显式 `package` 声明时，目录名就是包名。

```text
math_demo/
  01_math.luna
  02_main.luna
```

```luna
// 01_math.luna
package math_demo;

export fn add(left: i32, right: i32) -> i32 {
    return left + right;
}

fn helper() -> i32 {
    return 1;
}
```

```luna
// 02_main.luna
package math_demo;

fn main() -> i32 {
    return add(20, 22);
}
```

使用目录作为输入：

```sh
./build/luna run math_demo -O2
./build/luna build math_demo -O2
```

`export` 是 ABI 承诺，而不是仅用于解析的标记。未导出的包级函数在生成的 LLVM IR 中保持内部链接；`export` 的函数使用外部符号。函数、结构体、枚举、trait、`interceptor` 与 `context` 均可声明为导出。`extern` 函数不能同时导出。

同一包内声明身份必须唯一；版本化声明以名称、tag 与完整版本号共同组成身份，例如 `greet@stable(1.0.0)`。包加载会报告包名不匹配、重复导出和跨文件语法错误，并带有源文件位置。

当前包系统不包含依赖清单、远程仓库、锁文件或跨包导入语法；这些属于 Beta 前的工具链与生态工作。公开 API 的版本选择规则见 [versioning.md](versioning.md)。
