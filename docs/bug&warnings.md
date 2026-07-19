# bug & warnings

## 异构计算出现的问题

1. setenv和putenv存在的环境影响问题
2. 现在的模式是必定注入GPU异构计算相关逻辑，这也许会导致严重问题。将在下一个版本中强化异构计算能力时解决
3. GPU检查从致命错误降为warning
4. 为最快修复上述问题，JIT模式不再引用rt_gpu_*符号

以上问题根源来源于异构计算的支持极不完善。将在下一轮升级迭代中进行完善
目前的短期修复意味着也许会导致JIT出现问题，异构计算支持出现问题等
报告日期 GMT+8 2026.7.18 18：44


## 两类剩余问题

类别 1：JIT 符号缺失（测试 1, 7, 13）
Symbols not found: [ __main, rt_gpu_initialize, rt_gpu_report_initialization_error ]
CodeGenerator::generateFunctionBody() 第 ~396 行——JIT 生成的 main() 仍嵌入 GPU 初始化调用。
存疑结论：我的 mIsAOT 修复逻辑正确，但CI 二进制未包含此修改（未 push / CI 用了旧缓存）。

类别 2：AOT 链接 CMD 引号解析失败（测试 2-6, 8-10, 14, 16, 17）
The filename, directory name, or volume label syntax is incorrect.
路径已全部 /（generic_string() 生效了）。但 shellQuote 把每个参数单独包双引号：

"clang++.exe" -O0 "file.ll" "libruntime.a" -o "output.exe"
CMD 的 "clang++.exe" 关—开—关引号序列在某些 MSYS2 环境解析异常，把 clang++" -O0 "D: 当成一个命令名：

'clang++" -O0 "D:' is not recognized as an internal or external command
证据：不经过 std::system() 的原生可执行文件测试（11, 12, 15）全部通过。

修改位置

问题	文件	行号	需要改什么
JIT GPU 符号	src/codegen/CodeGenerator.cpp	~396	if (decl->name == "main" && mIsAOT) — 确认此行存在
AOT CMD 引号	src/main.cpp	~378-384	不要每个参数都 shellQuote。只对含空格的参数加引号。或者 Windows 上改用 CreateProcess 替代 std::system()