> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/version/ —— 目录逐文件指南

本指南合并了 src/version/ 下全部源码文件的五段式导读，方便你一次读完整个目录。

> Document category: implementation note
> Applies to: Luna 0.3.0 development
> Status: Implemented Experimental
> Normative status: non-normative
> 语言：中文权威版（英文版为占位）

# src/Version.h —— 编译器版本号

## 这个文件做什么

定义当前编译器的版本宏。CMake 构建与此引用，用于命令行 --version 输出。

## 关键结构体·宏

- `LUNA_VERSION_STRING`：定型为 "0.3.0"。

## 关键函数

无函数；仅一个预处理器宏。

## 与周边文件·阶段的关系

- 被 src/driver/Driver.cpp（--version 分支）等引用。

## 延伸阅读

- 版本化 ABI：docs/versioning.md。



---
