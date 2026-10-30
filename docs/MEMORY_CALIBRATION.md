# 4070 SUPER 实测与存储模型对照

2026-10-30，本机 RTX 4070 SUPER 已运行手写 CUDA C++/内联 PTX 微基准；没有使用 Triton、CUTLASS 或 CUTE。完成 118 组实验、590 个原生计时样本和 30 组正式 C++ 模型探针。所有计算结果校验通过。它们提供了存储行为证据，**还不足以把模型称为 NVIDIA 硬件复刻或准确性能预测器**。

## 证据在哪里

- [审计结果](evidence/memory-4070-20261030/audit.json)：样本数、结果检查及冻结源码/二进制哈希核对。
- [原生原始样本](evidence/memory-4070-20261030/native.json)、[模型原始结果](evidence/memory-4070-20261030/model.json)、[统计结果](evidence/memory-4070-20261030/summary.json)。原生文件逐项保存 argv、设备属性及全部五次样本，没有只保留最好的一次。
- [对照图](evidence/memory-4070-20261030/memory.svg)、[SASS](evidence/memory-4070-20261030/sass.txt)、[编译日志](evidence/memory-4070-20261030/build.log)、[源码/二进制 SHA-256](evidence/memory-4070-20261030/sha256.json)。图中 GPU 与模型使用各自的时钟口径。
- [运行期间的频率和温度](evidence/memory-4070-20261030/gpu-telemetry.csv)、[NVCC 版本](evidence/memory-4070-20261030/nvcc.txt)、[NCU 权限失败](evidence/memory-4070-20261030/ncu-permission.log)。

完整工作目录是 `artifacts/memory-4070-locality/`，保留逐命令日志、命令起始 UTC、退出码、冻结源码和可执行文件。上面的 Git 内证据保留其原始文件名引用；逐命令文件和大二进制仍在工作目录。探索轮 `artifacts/memory-4070-full/` 也保留，但没有与最终轮合并统计。

## 测试的是哪块卡

CUDA 实际查询：CC 8.9、56 SM、L2 50,331,648 B（48 MiB）、每 SM shared 102,400 B、65,536 个寄存器。驱动 575.57.08，CUDA 12.9.86，`nvcc -O3 -std=c++17 -arch=native -lineinfo`。`-arch=native` 选择本机架构，不把 SM89 固定进通用构建工具。

监控使用绝对路径 `/usr/bin/nvidia-smi`，查询真实显卡；没有通过项目 NVML 桥接把模拟设备混入 CUDA 实验。实验时桌面仍在运行，没有锁频或独占显卡。记录中有 P0/P2 状态和显存时钟变化，因此微秒级 CUDA event 差异不能被解释成稳定的性能优势。

## 三种实验如何实现

### 依赖加载与缓存

`benchmarks/cuda/chase.cuh` 只让一个 warp 的 lane 0 执行。按确定性种子打乱 sector 顺序，将每个 32 B sector 中第一个 uint32 写为下一节点的字索引，形成覆盖全部节点的单环。每个 kernel 先完整走一圈，再测 8,192 次依赖加载；另有 0、4,096 和 16,384 次控制组。每次最终索引都与宿主逐步追踪比较。

PTX `.ca` 与 `.cg` 分别选择允许 L1 和绕过 L1 的缓存提示。起始时钟先依赖预热的最后加载值，第一条计时加载的地址再依赖起始时钟与运行时零掩码；结束时钟依赖最终加载值。内联汇编为 volatile，并检查编译后的代码。当前 SASS 包含 `.ca` 对应的 `LDG.E.STRONG.SM`、`.cg` 对应的 `LDG.E.STRONG.GPU`、`CS2R`，停止时钟之前存在消费加载结果的 `ISETP`。这些名称是本次工具链的实际产物，不是跨架构 SASS 编码承诺。

`clock64` 测得的是**依赖循环每步成本**，包括地址计算、循环控制、调度和访存；不是单独 SRAM 的命中时延。用两个非零循环长度的中位数差分估计斜率，单独报告零次循环的成本；零次路径与非零路径不同，不能把它机械扣除后称为纯加载延迟。shared 也采用这个原则。

最终轮增加两种访问顺序：全范围乱序，以及 4 KiB 分组之间乱序、组内再次乱序。两者都走完整个数据集，后者减少连续访问跨越远地址的频率。**4 KiB 是实验的局部性分组大小，不是测出的 GPU 页大小。** 两个种子、三种 shared carveout 偏好和固定种子打乱的实验顺序都写入原始记录。

### Shared memory 的 bank 与广播

`shared.cuh` 使用 16 KiB shared 数组。每个字保存 `index ^ 2048`，因此每 lane 的下一地址由上次 `ld.shared.u32` 的结果决定，往返两处不同地址。两处的 bank 模式相同。32 个 lane 都执行，最终结果逐 lane 检查；时钟样本取 lane 0。

stride 0 表示全部 lane 读同字，stride 1/33 分散到 32 个 bank，stride 2/4/8/16/32 逐步增加同 bank 不同字的数量。先有单独预热 kernel，计时 kernel 内重新初始化 shared 并同步；`clock64` 只覆盖依赖循环，CUDA event 包含初始化和写结果。这两个计时区间没有混为一谈。

### Global 访存合并

`coalesce.cuh` 使用 64 个 CTA，每个 32 线程，遍历各自的 16 行、每行 2 KiB，执行 128 次 `.cg` 加载。每 lane 累加并写回，宿主检查全部输出，防止无用加载被删除。采用 broadcast、连续对齐、偏移一个字及 stride 2/4/8 六种模式。

模型 `coalesce()` 的结果与独立地址集合枚举一致：分别为 1/4/5/8/16/32 个 32 B sector。**硬件 sector 计数本次没有获得。** CUDA event 中位数约 20.7–22.9 μs；这个小规模程序不是带宽饱和实验，不能用这些相近耗时证明六种模式的硬件事务相同，也不能把逻辑读取字节数当成 DRAM 实际流量。

## 本次实际发现

`.ca` 全范围乱序、carveout 偏好 0% 时，两个种子在 4–80 KiB 的中位数均为 39.007 GPU cycles/step，4 KiB 差分斜率为 39.000；96 KiB 升为约 132.6–134.6，128 KiB 约 239.4–239.5。偏好 64% 时，48 KiB 仍为约 39，64 KiB 升为 163.1–171.2；偏好 100% 时，16 KiB 约 39，24 KiB 升为 180.2–203.5。这支持“局部缓存行为受 carveout 偏好影响”，不证明实际分区容量、相联度或某种替换策略。

Ada 官方说明 unified L1/shared/texture 总容量为 128 KiB，并提供 shared carveout 选择；这不意味着任何本实验的延迟拐点必须恰好等于 128 KiB，也不意味着请求的偏好被执行为某个已知字节分区。[NVIDIA Ada Tuning Guide](https://docs.nvidia.com/cuda/ada-tuning-guide/index.html)

`.cg` 的 8–64 MiB 组在本次环境中波动较大，4 KiB 分组对照也没有产生清晰、可重复的 48 MiB 边界。**L2 的 48 MiB 数值来自 CUDA 属性查询，没有被这些延迟曲线独立测出来。** 地址转换、缓存分区与映射、其他显存使用、调度和时钟状态都尚未分离；目前不能把高延迟归因于其中某一个原因。

Shared 的差分结果如下。第三列来自直接调用正式 `BankedShared` 的请求/响应探针，没有模拟真实 GPU 的循环指令；两列不是逐拍等价的执行环境。

| stride（32 位字） | GPU 依赖循环斜率，cycles/step | 模型请求成本，clocks/request |
|---:|---:|---:|
| 0（广播） | 28.000 | 3 |
| 1 | 28.000 | 3 |
| 2 | 28.000 | 4 |
| 4 | 29.999 | 6 |
| 8 | 38.031 | 10 |
| 16 | 54.000 | 18 |
| 32 | 86.024 | 34 |
| 33 | 28.000 | 3 |

GPU 的 32 路冲突循环约为 stride 1 的 3.07 倍；直接模型请求为 11.33 倍。广播和高冲突趋势相符，绝对成本与归一化曲线不同。尤其不能由 stride 2 的循环斜率相同，推断底层没有 bank 冲突：共同的指令和依赖开销可能掩盖服务差异。没有据此把模型的 shared 延迟直接改成 28 拍。

模型默认 L1/L2 仍是 64/256 KiB。实际模型探针的 `.ca` 4–64 KiB 成本为 5 clocks/request，96 KiB 为 20.797，128–256 KiB 为 21；384/512 KiB 约 73。它能展示分层容量和绕过 L1 的影响，但这些配置显然不等于 4070 SUPER。源码 `CacheConfig` 的 4 拍命中延迟与探针观测到的 5 拍也要区分，后者包括请求服务过程。

## 复现与检查

在仓库根目录执行；非 build 模式拒绝覆盖已有结果目录：

```sh
python3 tools/calibrate_memory.py --mode quick --out artifacts/memory-quick-new
python3 tools/calibrate_memory.py --mode full --out artifacts/memory-full-new --repeats 5
python3 tools/calibrate_memory.py --mode check --out artifacts/memory-check-new
```

需要 CUDA 编译器及真实 CUDA 设备；check 还需要 Compute Sanitizer。绘图使用可选 matplotlib，未安装时仍保存 JSON 和 Markdown。默认 CMake 构建仅新增无 CUDA 依赖的 `simt-memory-probe`：

```sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
build/simt-memory-probe coalesce 1 1
build/simt-memory-probe shared 32 17
build/simt-memory-probe chase ca 4096 73 123
```

本次 27 个默认套件全部通过；新增 5 项行为检查覆盖 CLI 的真实模型路径、奇数长度依赖链、独立 sector 枚举、计时斜率和“缺失硬件计数不能变成零”。10 个 CUDA memcheck 场景零错误，shared racecheck 零 hazards；包含零次迭代、奇数长度、广播、32 路冲突、stride 33、非整除行迭代和末端偏移。硬件源码未变，本阶段没有重跑全部 RTL 综合。

## Nsight 接入与未完成项

已通过 `kernel-performance-port` 接入可移植采集工具。`tools/kernel_perf` 的来源和许可证单独保留；它是 Python 报告工具，没有把其原项目的 CUTLASS kernel 或头文件接入本实验。实际工作负载配置为 `tools/kernel_perf/configs/memory-coalescing.json`：

```sh
python3 tools/kernel_perf/perf.py doctor
python3 tools/kernel_perf/perf.py run --config tools/kernel_perf/configs/memory-coalescing.json --benchmark-only
python3 tools/kernel_perf/perf.py run --config tools/kernel_perf/configs/memory-coalescing.json
```

配置独立测 CUDA event 原生耗时；NCU 只选择 `coalesce_kernel`，跳过一次预热并采一次 kernel，关闭时钟控制，明确采用 replay 的缓存清理。它请求 global-load sectors 和 requests 两项计数。NCU 实际运行返回 `ERR_NVGPUCTRPERM`；报告保留失败和此前成功的原生样本，不把采集时 kernel 耗时拿来比较。`useful_bytes` 仅为 lane 逻辑加载量。[NVIDIA 对该错误的说明](https://developer.nvidia.com/ERR_NVGPUCTRPERM)

还需获得硬件计数并验证请求/sector 关系，设计能区分 TLB、cache set/partition 和替换行为的实验，以及将执行循环与模型指令通路的计时边界匹配后再校准参数。本阶段未完成这些工作，也未据此修改当前 RTL 的结构或延迟。

实验方法参考 [Jia 等的 Turing 微基准论文](https://arxiv.org/abs/1903.07486)，只采用依赖链和保留反汇编的方法，没有移用 Turing 时延。32 B 合并、shared banks 与广播的公开行为依据 [CUDA Best Practices](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html)；缓存提示与时钟寄存器依据 [PTX ISA](https://docs.nvidia.com/cuda/parallel-thread-execution/index.html)。本机实验不能外推成 SM100/SM120 的硬件细节。
