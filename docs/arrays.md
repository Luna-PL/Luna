# 安全数组（第一阶段）

`array<T, N>` 是定长、内联存储的值类型，`N` 必须是编译期非负整数：

```luna
let values: array<i32, 4> = [10, 20, 30, 40];
values[1] = values[0] + 5;
print(values[1]);
```

数组字面量会推导元素类型和长度；所有元素必须具有同一类型。`a[i]` 只接受整数索引。显而易见的常量越界会在编译时报错，动态索引会在运行时检查，越界后报告错误并终止，绝不生成未定义的越界访问。

## 借用切片

`values[start..end]` 创建只读 `slice<T>`，等价于 `slice(borrow values, start, end)`：

```luna
let values = [10, 20, 30, 40];
let middle = values[1..3];
print(middle[0]); // 20
```

切片是 `{data, length}` 的非拥有借用视图。它存活期间来源数组不能被写入；切片索引同样执行运行时边界检查。`start` 和 `end` 均允许等于数组长度仅作为半开区间的 `end`，且必须满足 `start <= end`。当前稳定表面只提供只读 `slice<T>`；`slice_mut<T>` 会在可变子借用和重叠区间证明完成后开放。

`raw<T>` 与 `device_buffer<T>` 不会隐式转换为数组或切片。堆拥有的 `vec<T>` 必须等待通用 Drop/析构协议完成后才会作为安全容器开放。
