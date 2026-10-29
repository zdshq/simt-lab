# 验证记录

本页原表记录阶段 0–10 的 75 项功能验收。后续增量包括 17 项存储硬件检查、6 项 NVML 端到端检查、7 项流水线单元检查、35 项基础时序执行检查、6 项 Tensor 阵列检查、19 项 Tensor 时序执行检查、29 项异步/组合时序检查、9 项 C++/17 项 Python local 与原子检查，以及 4 项 C++/33 项 Python 持久上下文、CP/copy/Graph 检查。软件三种构建均通过 20 套件、257 项检查（214 项 Python、43 项 C++）。具体证据见 [存储单元](HARDWARE_MEMORY.md)、[SM](HARDWARE_SM.md)、[Tensor](HARDWARE_TENSOR.md)、[DMA/事务屏障](HARDWARE_ASYNC.md)、[local/原子](HARDWARE_ATOMIC_LOCAL.md)、[持久设备](HARDWARE_HOST.md) 和 [原版 nvidia-smi](NVIDIA_SMI.md)。另有 30 项独立 RTL 检查：首批 [缓存/依赖/仲裁](RTL.md) 14 项及 [Tensor 数值/阵列](RTL_TENSOR.md) 16 项，见下表。完整 RTL GPU、实机校准和最终硬件验收仍待完成。

最新 Tensor RTL 接入后：默认 Debug 为 21 套件、259 项；RTL Release 与 ASan/UBSan 为 24 套件、318 项（270 Python、48 C++）。其中后端复用的检查不计作新增独立场景，详细执行结果见本页“Tensor RTL 执行后端”及 [RTL_BACKEND.md](RTL_BACKEND.md)。

验收日期：2026-10-29。环境：Linux、GCC 11.4、CMake 3.22、Python 3.13.5。基础构建要求 C++20、CMake 3.20+ 和 Python 3.10+；可选 RTL 构建还需 Verilator，综合需 Yosys/ABC。

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
python3 examples/hardware_async.py
python3 examples/hardware_memory_ops.py
python3 examples/hardware_host.py
python3 examples/host_graph.py --timing --hardware-trace --out artifacts/timed-host-graph
python3 examples/run_workloads.py --timing --out artifacts/timed
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
| 持久时钟与缓存 | `test_hardware_context.cpp`：相邻 sector、在途 fill、所有者排空、local 生命周期；`test_timed_host.py`：跨 kernel 冷热、上传失效、地址复用、双引擎单时钟、有限链路/暂存、CP/Graph 带宽、原子/local 故障排空 |
| 时序主机完整负载 | `test_timed_host.py`：原 20 项主机/Graph 契约、三次 DMA→Tensor 图重放及四类完整负载；`hardware_host.py` 保存四类负载各两次图重放的实际下载字节与硬件报告 |
| 用户代码规范 | 入口只调用流程函数；源码、模板注释相邻英文/中文；函数按职责拆分并保持简短；没有添加样式计数测试 |
| 开发过程 | 独立 Git 仓库按阶段提交，未改写此前历史；构建与示例产物不跟踪 |

相同输入的默认示例输出：vector 257 个值误差 0；reduction 3 个整数和完全一致；GEMM 96 个值最大绝对误差约 2.98e-7；Transformer 32 个值约 1.07e-7。精确数值单元测试和组合数学容差测试分开，不能互相替代。

主机 Graph 示例输出：普通路径 4 个公开提交包，三次图重放各 1 个包，跨 stream capture 启动 1 个包；write/read/completed 均为 8，最终计算及拷贝引擎空闲，trace 未丢弃。每次均与主机独立计算的 65 元素向量结果逐项相等。计数验证提交结构，不代表真实 GPU 的时间或吞吐收益。

## 阶段 16：关键模块 RTL

Verilator 4.038 和 Yosys 0.40 的实际产物、命令及源码哈希保存在 `artifacts/rtl-*`。原软件 Debug 回归本阶段再次通过 20 套件、257 项检查；RTL 独立统计，不把不同配置或网表复跑累计成新测试场景。

| 实际执行 | 检查数量 | 结果与证据 |
|---|---:|---|
| 缓存、scoreboard、bank 仲裁 RTL 仿真 | 9 + 3 + 2 = 14 | 全部通过，`artifacts/rtl-final/simulation.json`；VCD 与 C++ 差分日志齐全 |
| 同一 RTL 和对照平台的 ASan/UBSan | 同样 14 项 | 全部通过，无 sanitizer 报告，`artifacts/rtl-sanitize-final/simulation.json`；旧 Verilator 波形库存在非对齐访问，此模式关闭波形并保留 sanitizer 检测 |
| 三模块 Yosys 完整小配置综合与网表仿真 | 同样 14 项 | `check -assert` 通过，`artifacts/rtl-synth-final/gate.json`；网表及 RTL 哈希与综合记录一致 |
| 默认 64 KiB 缓存 RTL 仿真 | 同样 9 项缓存检查 | 全部通过，`artifacts/rtl-full/simulation.json` |
| 默认 64 KiB 缓存保留存储综合 | 不计行为测试数 | `artifacts/rtl-synth-full/synthesis.json`：数据阵列 2,048×256 位、一个读端口、一个写端口 |

缓存测试覆盖数据字节、sector、MSHR/merge/response 容量、锁定 way、LRU、写确认、反压、乱序填充与高地址，并与 C++ 执行 360 个混合请求的差分；scoreboard 有 5,000 条随机转换，bank 仲裁有 4,000 拍端口比较。完整映射只针对 2 KiB 缓存小配置；其 50,627 个 generic cells 不是等价 NAND2 门数或物理 SRAM 成本。其余计数及限制见 [RTL.md](RTL.md)。

本阶段再次实际运行原版 `/usr/bin/nvidia-smi`：系统后端显示 RTX 4070 SUPER；项目启动器显示活模拟设备及 64/7/57 MiB，关闭后返回 `No devices found.`。记录在 `artifacts/rtl-final/nvidia-smi-status.txt`，没有合并系统默认视图。

## 阶段 16：Tensor 数值与完整阵列 RTL

新增 16 项：FMA 4、格式转换 3、PE 3、完整 4×4 阵列 6；`cmake --build build --target rtl-check` 合计 30 项全部通过，当前报告为 `build/rtl/simulation.json`。这些检查另在 ASan/UBSan 和实际综合后网表上通过，详见 [RTL_TENSOR.md](RTL_TENSOR.md)。

| 检查对象 | 主要覆盖 | 实际证据目录 |
|---|---|---|
| 四级 FP32 FMA | 13,824 个特殊值组合、250,000 个随机/抵消输入、12,000 个精确有理数参考、时钟/反压/复位 | `artifacts/rtl-tensor-synth`、`artifacts/rtl-tensor-fma-sanitize` |
| 格式转换 | 全部 FP16/BF16 编码、INT8 字节、FP4 半字节及非法格式 | `artifacts/rtl-tensor-decode-synth`、`artifacts/rtl-tensor-decode-sanitize` |
| 独立 PE | 65,536 种整数乘积、融合/缩放/稀疏语义、最后令牌阻止后继 | `artifacts/rtl-tensor-pe-synth`、`artifacts/rtl-tensor-pe-sanitize` |
| 完整 4×4 阵列 | 100 个混合格式/输入气泡 tile、256 K、有限 FIFO、结果保持、控制拒绝、在途复位；与 C++ 阵列逐位比较 | `artifacts/rtl-tensor-array-synth`、`artifacts/rtl-tensor-array-sanitize` |

完整阵列实际综合为 217,893 个 generic cells，其中 16,636 个触发器位；没有工艺面积或全 GPU 门数结论。首次 FIFO 推断存在未驱动位，明确使用寄存器组后通过严格综合检查和网表验证；复查补上最后令牌退休同拍的准入约束，并重跑受影响 PE/阵列的 RTL、sanitizer、综合和网表检查。

本阶段软件 Debug 回归再次通过全部 20 套件、257 项检查，包括原版 nvidia-smi 集成。当时新增 RTL 尚未接入 ISA/TMEM 或主机设备；后续接入验收见下一节。

## 阶段 16：Tensor RTL 执行后端

新增可选 `SIMT_TENSOR_RTL` 构建、CLI/Python 显式选择、每 SM 独立 Verilator 实例和统一 `TensorEngine` 接口。前端传原始字节，RTL 负责转换、计算和控制；结果经原 TMEM banks 写确认。查询和引脚适配不推进隐藏时钟，工作计数来自实际 RTL 事件。详细实现、命令及边界见 [RTL_BACKEND.md](RTL_BACKEND.md)。

| 验收项 | 实际结果 |
|---|---|
| 默认 Debug `ctest --test-dir build --output-on-failure -j6` | 全部 21 套件、259 项通过 |
| RTL Release `ctest --test-dir build-rtl --output-on-failure -j6` | 全部 24 套件、318 项通过，270 Python / 48 C++ |
| RTL Debug + ASan/UBSan，`UBSAN_OPTIONS=halt_on_error=1` | 首次 23 套件通过，最大矩阵超过 8 秒进程时限；保留原用例、将 RTL 时限改为 60 秒后，重跑 `rtl_tensor_execution` 的 19 项全部通过，耗时 65.46 秒；合计全部 24 套件通过，无 sanitizer 报告 |
| 新增/复用检查 | 2 项显式选择；5 项 C++ 适配器；19 项复用数值/时序；35 项主机检查中 33 项复用，2 项验证查询不推进 RTL 和在途 MMA 失败后再次启动 |
| `cmake --build build --target rtl-check` | 当前源码全部 30 项独立 RTL 检查通过 |
| PE/阵列观测端口改动 | 同样 3+6 项在 ASan/UBSan 及重新综合的网表上通过；源码/网表哈希匹配，见 `artifacts/rtl-{pe,array}-integrated*` |
| 四类 CLI 与主机 Graph 负载 | 原算法全部通过，每个 Graph 两次重放；GEMM 96 输出最大误差约 2.98e-7，Transformer 32 输出约 1.07e-7 |
| 原版 nvidia-smi 查询完成 RTL GEMM 的会话 | 专用后端显示实际 7 MiB 分配，关闭后消失；系统默认仍为真实显卡，见 `artifacts/rtl-integrated/nvidia-smi-status.txt` |

失败测试还验证了时钟统计范围：故障后私有计算单元冻结，存储请求继续排空，因此失败快照的阵列拍数可以小于 kernel 生命周期。正常执行/完成时两者相等。未将测试中的错误时钟假设改写成虚假的电路计数。

事件端口加入后的当前综合统计为 PE 13,090 / 794、完整阵列 217,928 / 16,636（generic cells / 触发器位），替代上一小节历史数值。没有整 GPU 面积或工艺频率结论。

## 明确边界

- 功能模式和时序模式都在 CPU 上执行；不会利用 RTX 4070 SUPER 来获得模拟 GPU 的实际吞吐。
- 两个 SM 已有基础时序资源模型；缓存、scoreboard、bank 仲裁有独立 RTL，但未连接成两份 RTL SM。仅报告实际小模块的 generic-cell 统计，没有整卡门数、工艺频率、面积或功耗结论。
- 自定义编码不兼容 CUDA/PTX/SASS/curryGPU 私有二进制；Python Program 是指令构造接口，不是 CUDA 编译器。
- 主机接口及 CUDA Graph 风格功能通过 Python/C++ 进程协议运行，不实现 NVIDIA `cudaGraph*` API/ABI、驱动、PCIe/MMIO 或系统可枚举的 CUDA 设备。
- Graph 支持范围见 [主机接口规范](HOST.md)：无 child graph、host callback、条件/内存分配节点、device-side launch 或完整 capture mode；单计算引擎不支持并发 kernel。
- 主机提交通过显式推进执行；完成历史保留到进程退出。kernel 的原始 global 访存不自动检查分配归属，硬编码地址需显式声明资源引用。
- 默认功能模式不启用 cache、bank conflict、scoreboard 和时序限制；`--timing` 已为自定义 59 个 opcode 接入相应结构及多 CTA 同驻留，但仍未实现真实弱内存全结果或 NVIDIA 原生 ISA。
- TMEM 使用本项目显式坐标接口；MMA tile 范围、浮点累加与异步事件规则是公开功能约定，非 NVIDIA 精确硬件复刻。
- FP4 数据与 binary32 块系数已实现；完整 MXFP4/NVFP4 scale 编码、tensor 级 scale、多维 TMA 和异步 MMA 未实现。
- 顺序敏感原子和无同步竞争程序不能要求跨所有调度顺序逐线程结果一致；没有实现自动 race checker。
- 测试覆盖所声明的功能案例和负例，并非形式验证或所有输入穷举。

阶段 0–10 的功能 ISS、持久主机链路与任务图已完成其原有验收。后续 CP/copy/Graph 时序、持久缓存和可选 Tensor RTL 后端也已接入。完整硬件目标仍未完成，剩余 PTX 异步矩阵提交、其余 RTL 单元接入、整体 RTL 连接、参数校准和最终审计以 HARDWARE_PLAN.md 为准。
