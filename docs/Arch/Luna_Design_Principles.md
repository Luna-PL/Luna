# Luna Design Principles

> 本文件提炼 Luna 当前架构讨论中已经形成的最高级设计原则。

## 1. Static First

能在编译期完成的工作，优先在编译期完成。

## 2. Pay for What You Use

Runtime、Reflection、Registry、Dynamic、热替换等能力必须显式启用，并具有可解释成本。

## 3. Structural Type First

结构决定默认兼容性。名字默认只用于候选发现、诊断、文档和显式名义约束。

## 4. Compiler Minimal

编译器负责语言正确性，不硬编码可由 Core 或标准库表达的策略。

## 5. Core Stable

Core 只提供长期稳定的语言抽象和协议。

## 6. Standard Library Owns Policy

SemVer、latest、Channel、Compatibility 等策略应由标准库实现。

## 7. Runtime Lightweight

Runtime 只保存执行、发现、选择和生命周期管理所必需的信息。

## 8. Dynamic Explicit

Dynamic 是 Runtime 的超集，只在需要完整运行时反射、Replace、Inspect 或 Runtime Weaving 时启用。

## 9. Metadata as Extension Mechanism

版本、权限、Registry、插件发现、导出与保留策略尽量统一到 Metadata。

## 10. Lowest Capable Layer

任何能力应放在能够完成它的最低层级。

## 11. Specification / Implementation Separation

规范定义行为，实现定义机制。两者不能长期混写。

## 12. Explicit Lifecycle

Runtime Select、Plugin Load、Plugin Unload、Replace 与 Re-select 必须具有明确生命周期和失效规则。
