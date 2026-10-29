# 验证记录

本页原表记录阶段 0–10 的 75 项功能验收。后续硬件目标仍在进行中：目前额外通过 17 项存储硬件检查、6 项 NVML 端到端检查、7 项流水线单元检查、35 项基础时序执行检查，以及 6 项 Tensor 阵列检查和 19 项 Tensor 时序执行检查。三种构建均通过 15 套件、165 项检查（135 项 Python、30 项 C++）。具体证据见 [存储单元](HARDWARE_MEMORY.md)、[SM 时序执行](HARDWARE_SM.md)、[Tensor 时序执行](HARDWARE_TENSOR.md) 和 [原版 nvidia-smi](NVIDIA_SMI.md)。这些增量不证明整个微架构已经接入。

验收日期：2026-10-29。环境：Linux、GCC 11.4、CMake 3.22、Python 3.13.5。构建要求 C++20、CMake 3.20+ 和 Python 3.10+，无第三方运行时依赖。

## 阶段 0–10 实际执行结果

| 构建 | CTest 套件 | unittest 项数 | 结果 |
|---|---:|---:|---|
| Debug | 9 | 75 | 全部通过 |
| Release | 9 | 75 | 全部通过 |
| Debug + AddressSanitizer + UndefinedBehaviorSanitizer | 9 | 75 | 全部通过，未报告 sanitizer 错误 |

当前结果包括原 ISA/执行器 52 项、原生主机协议 3 项、主机接口 9 项和 Graph/capture 11 项；开发记录保留各阶段当时的测试数量。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure

cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSIMT_SANITIZE=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure

python3 examples/run_workloads.py
python3 examples/host_graph.py
```

## 需求与证据映射

| 能力 | 主要证据 |
|---|---|
| 声明式 128 位 ISA 与实际解码执行 | `test_simulator.py`：59 opcode × 40 次编码往返，固定编码布局、保留位和未知 opcode 负例 |
| 整数、谓词、标量浮点 | `test_simulator.py`：80 个整数差分程序、32 lane 逐项比较、融合舍入抵消、转换边界 |
| 双 SM、多 CTA、多 warp、部分 warp | 5 CTA × 37 线程标识测试；向量、规约、GEMM 实际运行；默认一 SM 同时驻留一 CTA |
| ITS 与显式重聚 | `test_control.py`：嵌套菱形、逐 lane 循环、EXIT/BREAK、令牌复用，比较 rr/min/max |
| warp 集合操作 | ELECT、VOTE 模式、原地 SHFL、部分成员、缺席成员及非法来源负例 |
| 稀疏内存与空间隔离 | `test_memory.py`：接近 4 GiB 地址、清零规范化、子字扩展、CTA shared 和逐线程 local 隔离、CVTA |
| 原子与 CTA 屏障 | 185 个唯一原子票号、十种原子模式、37 线程规约、分歧到达、重复到达、动态/静态计数区别 |
| 公平前进与死锁 | 自旋消费者和生产者在轮转调度下完成；重聚、CTA、事务屏障死锁报告 |
| TMEM 与 dense MMA | `test_tensor.py`：分配/释放/清零，15 个随机带步长矩阵，五种数据格式，逐位有理数参考 |
| 数值精度与累加 | 独立 Fraction 舍入边界、FP16/BF16/FP32 次正规数、融合抵消、INT8 绕回、多条 MMA 保留 D |
| 2:4 与块缩放 | 全部六种保留位置、错误元数据、FP4 行/块/列缩放、稀疏与缩放舍入组合 |
| 异步数据通路 | `test_async.py`：延迟可见性、阶段复用、EXIT 后完成、多个 warp 的 global→shared→MMA→TMEM 路径 |
| 异步错误诊断 | 丢失搬运字节、未初始化记录、未来令牌、超出字节预算、覆盖屏障记录 |
| 组合负载 | `test_workloads.py`：向量尾部、规约、分块 GEMM、完整小型 Transformer；三种 PC/warp 策略下结果一致 |
| 易复现与解释 | `examples/run_workloads.py` 生成二进制、镜像、汇编、参数、状态和验证摘要；WALKTHROUGH.md 提供架构图 |
| 主机与 CP | `test_protocol.py`：发布/取走/完成不同步、无效请求无部分提交、单包图展开；`test_host.py`：有界环绕队列、超时后继续等待 |
| 持久资源与 stream/event | 两个 SM 的三 CTA kernel 连续读写同一分配；引用保留、释放与地址复用、H2D 快照、D2D 重叠、设备归属、跨 stream 完成依赖 |
| 计算/拷贝引擎 | 独立 stream 或图分支中同时观察到运行中的计算与拷贝；join 的开始时刻不早于所有前驱完成 |
| Graph 生命周期 | `test_graph.py`：模板快照、三次单包重放、参数更新、运行中更新拒绝、同实例跨 stream 串行化、非拓扑编号、空图和环路拒绝 |
| 捕获与错误 | 单/多 stream capture 不执行，事件边及重新汇合、捕获错误恢复；失败节点取消后继而独立分支完成，资源最终释放 |
| 主机组合示例 | `examples/host_graph.py`：普通提交、三次更新/重放、跨 stream capture，逐元素验证 65 个值并保存指令及 trace |
| 用户代码规范 | 入口只调用流程函数；源码、模板注释相邻英文/中文；函数保持简短，按含声明与结束行统计最长 C++ 函数 25 行、Python 22 行；没有添加样式计数测试 |
| 开发过程 | 独立 Git 仓库按阶段提交，未改写此前历史；构建与示例产物不跟踪 |

相同输入的默认示例输出：vector 257 个值误差 0；reduction 3 个整数和完全一致；GEMM 96 个值最大绝对误差约 2.98e-7；Transformer 32 个值约 1.07e-7。精确数值单元测试和组合数学容差测试分开，不能互相替代。

主机 Graph 示例输出：普通路径 4 个公开提交包，三次图重放各 1 个包，跨 stream capture 启动 1 个包；write/read/completed 均为 8，最终计算及拷贝引擎空闲，trace 未丢弃。每次均与主机独立计算的 65 元素向量结果逐项相等。计数验证提交结构，不代表真实 GPU 的时间或吞吐收益。

## 明确边界

- 这是 CPU 上执行的功能模拟器；不会利用 RTX 4070 SUPER 来获得模拟 GPU 的实际吞吐。
- 两个 SM 已有基础时序资源模型，但还不是两份 RTL，也不代表已经估算逻辑门、频率、面积或功耗。
- 自定义编码不兼容 CUDA/PTX/SASS/curryGPU 私有二进制；Python Program 是指令构造接口，不是 CUDA 编译器。
- 主机接口及 CUDA Graph 风格功能通过 Python/C++ 进程协议运行，不实现 NVIDIA `cudaGraph*` API/ABI、驱动、PCIe/MMIO 或系统可枚举的 CUDA 设备。
- Graph 支持范围见 [主机接口规范](HOST.md)：无 child graph、host callback、条件/内存分配节点、device-side launch 或完整 capture mode；单计算引擎不支持并发 kernel。
- 主机提交通过显式推进执行；完成历史保留到进程退出。kernel 的原始 global 访存不自动检查分配归属，硬编码地址需显式声明资源引用。
- 默认功能模式不启用 cache、bank conflict、scoreboard 和时序限制；`--timing` 已接入这些结构及多 CTA 同驻留，但仅支持 HARDWARE_SM.md 列出的基础指令，仍未实现真实弱内存全结果。
- TMEM 使用本项目显式坐标接口；MMA tile 范围、浮点累加与异步事件规则是公开功能约定，非 NVIDIA 精确硬件复刻。
- FP4 数据与 binary32 块系数已实现；完整 MXFP4/NVFP4 scale 编码、tensor 级 scale、多维 TMA 和异步 MMA 未实现。
- 顺序敏感原子和无同步竞争程序不能要求跨所有调度顺序逐线程结果一致；没有实现自动 race checker。
- 测试覆盖所声明的功能案例和负例，并非形式验证或所有输入穷举。

阶段 0–10 的功能 ISS、持久主机链路与任务图已完成其原有验收。新的完整硬件目标仍未完成，剩余异步/原子/local 通路、PTX 异步矩阵提交、CP/Graph 集成、RTL 和参数校准以 HARDWARE_PLAN.md 为准。
