# Heterogeneous compute (initial ABI)

## 中文说明

异构计算目前提供 CPU simulator、CUDA 和 ROCm 三条路径。`device_buffer`、`launch`
和 `await` 具有独立的所有权与事件生命周期；默认 simulator 方便无 GPU 环境回归。
设备端函数不能使用 host continuation 或 host-only 动态 effect，硬件后端不可用时
必须显式报错，不会静默切换后端。

Luna now has a safe, explicit asynchronous compute surface. The default backend is a CPU simulator so programs run everywhere. CUDA and ROCm backends clone each `kernel fn` into an LLVM device module, emit PTX or a linked HSA code object (HSACO), load it through the CUDA Driver API or HIP Module API, dispatch it, and return a real device event. The ROCm path packages the linked HSACO in the Clang HIP module bundle that `hipModuleLoadData` accepts. The source ABI keeps allocation, scalar transfers, dispatch, and synchronization separate from ordinary host memory.

```luna
kernel fn add_one(index: i32, data: &mut device_buffer<i32>) {
    let value = gpu_load_i32(data, index);
    gpu_store_i32(data, index, value + 1);
}

fn main() -> i32 {
    linear let data = gpu_alloc_i32(8);
    gpu_store_i32(borrow mut data, 0, 41);

    let done = launch add_one[threads: 8](borrow mut data);
    await done;

    print(gpu_load_i32(borrow data, 0)); // 42
    gpu_free(move data);
    return 0;
}
```

`kernel fn` has a fixed initial ABI:

- Its first parameter is explicitly `index: i32`; `launch` supplies one logical index per thread.
- It returns `unit`, is neither generic nor `extern`/`constexpr`, and may only call `gpu_load_i32` and `gpu_store_i32` in this initial ABI.
- Scalars are passed by value. A buffer is passed only as `&device_buffer<T>` or `&mut device_buffer<T>`.
- Kernel bodies form a `DeviceMemory`-only sublanguage: scalar bindings, arithmetic, branches, loops, and the two device built-ins are allowed. `slot`, `apply`, `resume()`, `abort()`, `await`, `launch`, `new`, `free`, FFI, closures, reflection, and ordinary host calls are rejected. This prevents host continuations or resource effects from entering SIMT code.

`device_buffer<T>` and the `event` returned from `launch` are automatically linear. Buffer arguments must be explicit named borrows at the launch site. Launching a buffer produces an in-flight loan that lasts until the corresponding `await`:

- The buffer cannot be borrowed, accessed, moved, or freed while in flight.
- An event cannot be ignored, copied, returned, or allowed to leave its scope unawaited.
- An event may be transferred with `move`; the in-flight loan transfers with it, and the receiving binding must be awaited.
- `gpu_free(move buffer)` is the only release operation for a device buffer.

## Failure behavior

A CUDA or ROCm launch that cannot load a module, resolve a kernel, create an
event, or submit work returns an internal invalid event handle. It is not
treated as a completed dispatch. `await event` validates and synchronizes that
handle; on launch or synchronization failure it prints the selected backend
and the vendor/runtime error, then terminates with a non-zero status. This is
also true for AOT binaries. The simulator's completed event succeeds through
the same `await` ABI.

## Compile targets and runtime backend

The simulator is the default and runs dispatch synchronously under the hood
while preserving the asynchronous ownership contract. Device code generation
and runtime selection are deliberately separate:

- `--gpu-target=sim|cuda[:sm_*]|rocm[:gfx*]` is a compiler option. It determines
  which device artifacts are embedded and never initializes CUDA/HIP.
- `LUNA_GPU_BACKEND=sim|cuda|rocm` is read only by the generated program. It
  selects which embedded representation to execute.

Targets may be comma-separated, for example
`--gpu-target=sim,cuda:sm_86,rocm:gfx1101`. One architecture per vendor is
currently supported. The host simulator form is available for every reachable
kernel; `sim` is the default when no target option is given.

```sh
./build/luna run examples/heterogeneous.luna
# or explicitly:
LUNA_GPU_BACKEND=sim ./build/luna run examples/heterogeneous.luna \
  --gpu-target=sim
```

Select CUDA with a functioning NVIDIA driver. No CUDA Toolkit or NVRTC is
needed at build time because the Driver API is loaded dynamically:

```sh
LUNA_GPU_BACKEND=cuda ./build/luna run examples/heterogeneous.luna \
  --gpu-target=cuda:sm_52
```

CUDA emits one-dimensional grids with 256 threads per block and uses the
default CUDA stream. If the Driver API cannot initialize, the generated entry
point fails before entering the user `main`; it never silently falls back to
the simulator.

Select ROCm for an AMD GPU. Luna loads `libamdhip64.so` dynamically, so its
headers and compiler are not build dependencies, but a working HIP runtime and
kernel driver are required to execute on the device:

```sh
LUNA_GPU_BACKEND=rocm ./build/luna run examples/heterogeneous.luna \
  --gpu-target=rocm:gfx1101
```

ROCm uses the same one-dimensional, 256-thread launch geometry. The default
ROCm target is `gfx1101`, suitable for Navi 32 / RX 7700 XT and RX 7800 XT.
Select another GPU explicitly, for example `--gpu-target=rocm:gfx1030`.
AMDGPU emission also requires the matching LLVM `ld.lld` in `PATH`.

On an ROCm-equipped AMD machine, enable the opt-in hardware smoke test:

```sh
cmake -S . -B build -DLUNA_ENABLE_ROCM_SMOKE=ON \
  -DLUNA_ROCM_SMOKE_ARCH=gfx1101
cmake --build build
ctest --test-dir build -L rocm --output-on-failure
```

Build machines without a GPU can emit a device artifact and still execute the
simulator, because no runtime backend is selected:

```sh
./build/luna run examples/heterogeneous.luna --gpu-target=cuda:sm_52
./build/luna run examples/heterogeneous.luna --gpu-target=rocm:gfx1101
```

For deployment, embed the target while building and select it only while
running:

```sh
./build/luna build examples/full_showcase/app --gpu-target=cuda:sm_52
LUNA_GPU_BACKEND=cuda ./examples/full_showcase/app/build/native/app

./build/luna build examples/full_showcase/app --gpu-target=rocm:gfx1101
LUNA_GPU_BACKEND=rocm ./examples/full_showcase/app/build/native/app
```

An AOT binary does not synthesize a missing target at runtime. Selecting ROCm
for an artifact built without HSACO, or CUDA for one built without PTX,
produces a clear launch error and a non-zero exit. Future Moon containers may
retain a verified kernel recipe for MoonRuntime JIT; ordinary native AOT
artifacts do not.

Kernel declarations may carry ordinary Metadata, for example `@version(1, 0, 0) kernel fn bump(...)`. A named `launch bump[threads: n](...)` currently requires that `bump` resolve to one kernel declaration; an ambiguous metadata family is rejected. Runtime kernel-family binding will be an explicit dynamic operation rather than postfix version syntax.

Kernels are code-generated only when referenced by a reachable lowered launch. Merely declaring a kernel emits a deferred MoonIR recipe and no `rt_gpu_*` host symbol. `--reserve-kernel-runtime` explicitly retains kernel code and runtime initialization when a host needs future dynamic kernel capability.

Initial i32 device operations are:

- `gpu_alloc_i32(count) -> device_buffer<i32>`
- `gpu_load_i32(borrow buffer, index) -> i32`
- `gpu_store_i32(borrow mut buffer, index, value)`
- `gpu_free(move buffer)`

Bulk host/device transfer is available through an explicit low-level ABI:

- `gpu_copy_from_host_i32(borrow mut buffer, borrow host, count)`
- `gpu_copy_to_host_i32(borrow mut host, borrow buffer, count)`

`host` has type `raw<i32>` and is commonly returned by an FFI allocator. The
upload requires a mutable device borrow; the download requires a mutable host
borrow. This preserves the borrow checker at both endpoints while the language
does not yet provide a safe host array or slice type. A negative count is
rejected for literals and fails at runtime for dynamic values.

The valid and invalid examples are in `examples/heterogeneous*.luna`.
Benchmark methodology and the JIT/AOT sampling script are in
[performance benchmark guide](benchmarks.md).
