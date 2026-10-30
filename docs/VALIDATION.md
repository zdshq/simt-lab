# 验证记录

本页原表记录阶段 0–10 的 75 项功能验收。后续增量包括 17 项存储硬件检查、6 项 NVML 端到端检查、7 项流水线单元检查、35 项基础时序执行检查、6 项 Tensor 阵列检查、19 项 Tensor 时序执行检查、29 项异步/组合时序检查、9 项 C++/17 项 Python local 与原子检查，以及 4 项 C++/33 项 Python 持久上下文、CP/copy/Graph 检查。软件三种构建均通过 20 套件、257 项检查（214 项 Python、43 项 C++）。具体证据见 [存储单元](HARDWARE_MEMORY.md)、[SM](HARDWARE_SM.md)、[Tensor](HARDWARE_TENSOR.md)、[DMA/事务屏障](HARDWARE_ASYNC.md)、[local/原子](HARDWARE_ATOMIC_LOCAL.md)、[持久设备](HARDWARE_HOST.md) 和 [原版 nvidia-smi](NVIDIA_SMI.md)。另有 30 项独立 RTL 检查：首批 [缓存/依赖/仲裁](RTL.md) 14 项及 [Tensor 数值/阵列](RTL_TENSOR.md) 16 项，见下表。完整 RTL GPU、实机校准和最终硬件验收仍待完成。

最新 Tensor RTL 接入后：默认 Debug 为 21 套件、259 项；RTL Release 与 ASan/UBSan 为 24 套件、318 项（270 Python、48 C++）。其中后端复用的检查不计作新增独立场景，详细执行结果见本页“Tensor RTL 执行后端”及 [RTL_BACKEND.md](RTL_BACKEND.md)。

后续 L1/L2 RTL 接入：默认 Debug 为 22 套件、261 项；缓存+Tensor RTL 构建为 28 套件、421 项（368 Python、53 C++）。缓存增加两项失效检查后，独立 RTL 总数为 32。细节见“L1/L2 RTL 执行后端”和 [RTL_CACHE_BACKEND.md](RTL_CACHE_BACKEND.md)；前面阶段数字保留为历史记录。

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

## 阶段 16：L1/L2 RTL 执行后端

`SIMT_CACHE_RTL=ON` 构建及 `--cache-rtl` / Python `cache_rtl=True` 接入每 SM 64 KiB L1 和共享 256 KiB L2。tag/data/MSHR/响应/失效扫描由实际电路执行；适配器只持有有限请求/响应票据，无影子缓存。读取返回值来自 SRAM，普通写入、local、原子、DMA、Tensor 和主机拷贝复用该层次。DRAM 与跨 SM 一致性协调仍为 C++。

| 验收 | 结果 |
|---|---|
| 默认 Debug | `ctest --test-dir build --output-on-failure -j6`，22 套件 / 261 项通过 |
| 缓存+Tensor RTL Release | `ctest --test-dir build-cache-rtl --output-on-failure -j6`，28 套件 / 421 项通过 |
| RTL ASan/UBSan | 使用 `UBSAN_OPTIONS=halt_on_error=1`，首轮 27 套件通过，最大矩阵超过 60 秒；保留算例并设 180 秒后，`rtl_cache_execution` 的 61 项重跑通过（139.05 秒），全部套件均有通过证据 |
| 接入检查 | 2 项后端选择、5 项 C++ 适配器/层次检查；61 项存储/DMA/Tensor 契约与 35 项主机/Graph 契约在新后端复跑 |
| 独立 RTL | `artifacts/rtl-cache-all/simulation.json` 包含 32 项；缓存 11 项在普通仿真、sanitizer 和实际综合后网表通过；64 KiB 参数也执行相同 11 项 |
| 完整负载 | `examples/hardware_host.py --build build-cache-rtl --tensor-rtl --cache-rtl --out artifacts/rtl-cache-host` 四类负载各重放两次，GEMM 96 输出与 Transformer 32 输出通过原数值阈值 |
| 活设备监控 | 原版 nvidia-smi 经项目后端查询实际 RTL GEMM 会话，显示 64/7/57 MiB，关闭后无设备；系统默认仍显示真实 4070 SUPER |

最终删除冗余 tag/valid 复位后，重新构建 Release 与 sanitizer，并执行 `ctest --test-dir <build> --output-on-failure -R '^(cache_backend_selection|rtl_cache_engine|rtl_cache_execution|rtl_cache_host)$'`，受影响四套件全部通过。sanitizer 使用 `UBSAN_OPTIONS=halt_on_error=1`，最终 61 项执行检查耗时 139.22 秒，无检测报告。此前全套检查与这次受影响复跑分开记录，没有宣称一次完整 sanitizer 执行从未超时。

最终源码的 32 项独立 RTL、11 项缓存 sanitizer、11 项缓存综合网表以及 64 KiB 配置的相同 11 项检查均通过。2 KiB 小配置综合为 55,212 个 generic cells / 22,083 个触发器位；64 KiB 保留存储综合识别出 524,288 个数据位、单读/单写端口，以及 tag 单写端口、valid 双写端口。相关 `simulation.json` / `synthesis.json` / `gate.json` 的源码哈希与当前文件一致；小配置网表哈希也匹配。

256 KiB、8 路 L2 保留存储综合两次超过 600 秒，第二次在上述复位优化后仍停留于 Yosys `OPT_CLEAN`，日志保留在 `artifacts/rtl-l2-integrated-inference/cache/synthesis.log`。未取得 L2 综合通过证据；完整 L2 的功能和时钟验证来自实际 Verilator 集成执行，不能与综合完成混为一谈。

失效检查覆盖精确的一行/拍扫描、非目标 sector 保留、目标 MSHR 拒绝失效、同一行的无关 fill 与失效同拍，以及主机范围清零前的在途访问排斥。新增诊断向量曾触发旧 Verilator 的位依赖误报，拆为标量输出后通过，未关闭组合环检查。

## 阶段 16：组内 LRU 与完整 L2 综合

用 `cache_lru.sv` 的每组独立寄存器/局部比较器替代平坦数组的动态更新。缓存容量、精确 LRU、接口和流水线不变；完整 L2 保留存储综合已通过，解决上一阶段两次 600 秒超时。Yosys 中间网表还直接确认 L1/L2 分别有 512×2 / 2,048×3 位 LRU 状态。

| 验收 | 当前源码的实际结果 |
|---|---|
| 完整 L1/L2 保留存储综合 | `rtl-l1-banked-inference` / `rtl-l2-banked-inference`，均通过 `check -assert`；数据阵列分别 524,288 / 2,097,152 位、单读/单写 |
| 小配置完整综合 | `rtl-cache-banked` 的 2 KiB/4 路：54,770 cells / 22,083 FF 位；`rtl-cache8-banked` 的 4 KiB/8 路：92,501 / 39,528；含层次内全部实例及展开数据存储 |
| 综合后网表行为 | 上述两个小配置分别通过相同 11 项检查，不算 22 项独立测试设计 |
| 完整容量 RTL | `rtl-l1-banked` 的 64 KiB/4 路、`rtl-l2-banked` 的 256 KiB/8 路均通过相同 11 项；360 次随机流在大配置也实际制造跨组冲突和替换 |
| 独立 sanitizer | `rtl-cache-banked-sanitize`、`rtl-l2-banked-sanitize` 的同样 11 项通过，检测保留，旧波形运行库关闭 |
| 联合 RTL Release | `ctest --test-dir build-cache-rtl --output-on-failure -j4`，28 套件 / 421 项全部通过，包括实际原版 nvidia-smi 六项检查，无跳过 |
| 受影响的主机/ISA sanitizer | 四套件中后端选择、C++ 适配器、35 项主机检查通过；执行套件 60 项通过，最大矩阵超过 180 秒。将进程上限改为 420 秒后，单独重跑完整 32×32×256 用例通过，308.877 秒，无 sanitizer 报告；保留 1,024 个输出与 262,144 次有效乘积断言 |
| 网表配置保护 | 实际拒绝用 4 路网表运行 8 路平台；清单同时校验 SETS/WAYS、RTL 源码和网表哈希 |
| 完整主机负载 | `hardware_host.py --build build-cache-rtl --tensor-rtl --cache-rtl --out artifacts/rtl-banked-host` 四类负载各两次通过；与重构前 `rtl-cache-host/validation.json` 全量相等，输出、设备/kernel 周期和 cache/DRAM 计数均一致 |

所有综合仍使用原 `synth -run begin:fine` 和 `check -assert`，没有缩小完整 L2 容量、关闭检查或改用较弱替换策略。小配置通用单元数不是 NAND2 等效门数；完整容量只保留存储描述，仍没有工艺 SRAM/FPGA BRAM 映射或布局布线。

本阶段首次 sanitizer 受影响回归总耗时 318.78 秒，不能算作全部通过；原日志保存在 `artifacts/rtl-banked-sanitize/ctest-before-timeout-change.log`。失败项重跑日志为 `maximum-matrix.log`，使用如下实际命令，默认 Debug 编译与 ASan/UBSan 均保留。其余已通过项未重复跑，合起来覆盖全部受影响检查。

```sh
PYTHONPATH=build-cache-sanitize/generated:python SIMT_RUNNER="$PWD/build-cache-sanitize/simt-run" UBSAN_OPTIONS=halt_on_error=1 python3 tests/test_rtl_cache_execution.py CacheTensorPipeline.test_maximum_descriptor_shape
```

最终核对 11 份本阶段仿真/综合/网表清单中的 RTL 哈希、SETS/WAYS 和两份实际网表哈希，与当前文件一致；摘要保存在 `artifacts/rtl-banked-evidence.json`。Release 日志重新计数为 368 项 Python 与 53 项 C++，没有跳过。此次未重复无改动的默认软件构建及其余独立 RTL 模块，前阶段证据保留各自范围。

## 阶段 17：真实/模拟设备合并与用户级默认命令

原版 `/usr/bin/nvidia-smi` 575.57.08 通过合并 NVML 后端同时查询 RTX 4070 SUPER 和活模拟器。真实设备保持原索引、句柄、元数据和默认进程表；模拟设备追加在后，内存读取实际分配表，正常退出或 SIGKILL 后退出枚举。公开 ABI 同时验证精确字节数、未知指针拒绝、引用计数、重新初始化和双线程真实/模拟查询。

| 实际命令或场景 | 结果与证据 |
|---|---|
| `cmake --build <build> --target simt_nvml test-nvml-api test-nvml-merge-api -j4` | `build`、`build-release`、`build-sanitize` 均成功；所有被修改的 NVML 代码及两个 ABI 辅助程序重新构建 |
| `ctest --test-dir build --output-on-failure -j4` | 23 套件、268 项全部通过，225 Python / 43 C++；日志 `artifacts/nvml-merged/debug-full.log` |
| `ctest --test-dir build-release -R '^(native_nvidia_smi|merged_nvidia_smi)$' --output-on-failure -j2` | 2 套件、13 Python 用例通过；`release-nvml.log` |
| `UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-sanitize -R '^(native_nvidia_smi|merged_nvidia_smi)$' --output-on-failure -j2` | 同样 13 项通过，无 sanitizer 报告；`sanitize-nvml.log`。原有外部程序 libasan 预加载及仅关闭泄漏报告的约定保留 |
| `python3 tools/install_smi.py --build build-release` | 已安装 `/home/zeke/.local/bin/nvidia-smi`；PATH 原本已包含该目录，无系统文件或 shell 配置改动；安装/卸载及拒绝覆盖行为有自动检查 |
| 实际联合 RTL Graph GEMM 后运行普通 `nvidia-smi` | 两次重放各验证 96 个值，最大误差约 2.98e-7；Tensor/cache 均为 RTL。随后 7 MiB 分配与释放正确显示，退出后与原版 `-L` 一致，仅剩实卡 |
| 原版文件核对 | 安装前后 `/usr/bin/nvidia-smi` SHA-256 相同；`installation.json` 保存路径、哈希、GEMM 与硬件统计，`default-command.txt` 保存原版输出 |

检查数来自实际日志，摘要为 `artifacts/nvml-merged/tests.json`。7 项新增合并用例中的 C++ ABI 断言不重复计数。本次没有重跑不受改动影响的全套 RTL/综合验证，之前的 421 项联合 RTL 记录保留原阶段范围；这里的联合 RTL GEMM 属于端到端监控验证。

最初包装原生句柄导致私有进程查询无法识别实卡；改为保留原生句柄，并在公开入口登记和验证身份。ABI 辅助程序还发现关闭死锁：原生 `nvmlShutdown` 内部回调被接管的 `nvmlDeviceGetCount_v2`，重入桥接锁。增加线程局部原生调用状态后，厂商内部调用直接回到原生符号，嵌套关闭、重建与并发测试通过；没有换成递归锁或关闭检测。早期失败保存在 `first-test-failures.log`。

PCI 选择在原程序私有路径中假定所有设备都有 PCI 状态，模拟设备无法满足；启动器因此将 PCI bus ID 选择、compute/accounted-process 查询以及管理/其他模式交给原生后端。数字索引和 UUID 可合并选择，公开 NVML 的 PCI 句柄查询也已验证。没有用虚假 PCI 地址掩盖协议差异。完整命令范围、启动监控的要求和撤销方式见 [NVIDIA_SMI.md](NVIDIA_SMI.md)。

## 阶段 16：Scoreboard RTL 接入实际 ISA 与主机

增加 `SIMT_SCOREBOARD_RTL`、`--scoreboard-rtl` 和 Python `scoreboard_rtl=True`。物理 warp 槽持有实际电路实例，RAW/WAW/WAR、谓词写者、同拍完成/收集/预约由 RTL 状态与组合查询决定。适配器没有软件影子依赖表。每寄存器每种事件单拍最多一次，按流水线阶段处理；4-bank 配置下非空事件上界 13，接口容量 16。详细结构与剩余硬件边界见 [RTL_SCOREBOARD.md](RTL_SCOREBOARD.md)。

| 最终验收命令或场景 | 实际结果 |
|---|---|
| `ctest --test-dir build --output-on-failure -j4` | 默认 Debug 全部 24 套件、270 项通过（227 Python / 43 C++），2.49 秒 |
| `ctest --test-dir build-cache-rtl --output-on-failure -j4` | 三种 RTL 后端可用的 Release 构建全部 33 套件、564 项通过（507 Python / 57 C++），5.62 秒。套件分别选择各后端，不宣称每个用例都同时开启三个 RTL |
| `UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-scoreboard-sanitize --output-on-failure -j4` | scoreboard RTL + Debug ASan/UBSan 全部 27 套件、404 项通过（357 Python / 47 C++），101.94 秒；该构建的 Tensor 和缓存为 C++ 后端 |
| 新增适配器与接口验证 | 4 项 C++ 适配器和 2 项 Python 选择检查；实际查询无时钟、跨字引脚打包、容量、拒绝、RMW、物理槽隔离与合法等待上限 |
| 实际指令与主机复跑 | 96 项指令/数值/存储/同步检查、34 项主机/Graph 检查，包括独立 stream 故障恢复和状态查询不推进电路 |
| 独立 RTL / sanitizer / 网表 | `tools/rtl.py --module warp_scoreboard` 的相同 4 项全部通过；6,400 个随机事件与独立 C++ 参考对照，另覆盖时钟门、阶段旁路、mask 原子拒绝、复位与饱和 |
| 三个后端同时运行四类负载 | `hardware_host.py --build build-cache-rtl --tensor-rtl --cache-rtl --scoreboard-rtl --out artifacts/rtl-scoreboard-gated-host`；四类负载各两次 Graph 重放通过，全部输出/周期/缓存与 DRAM 计数与此前 `rtl-banked-host` 一致，设备最终 64,500 拍 |
| 最终状态、trace 与监控复核 | 8 份 workload kernel 报告删除新增 scoreboard 观测项后与旧报告完全相等，包含完整状态和逐拍 trace；另 2 份监控 GEMM 报告也核对三个 RTL 后端及有效/输入时钟关系。普通 nvidia-smi 实际显示同存实卡和模拟器，7 MiB 分配、释放及退出均正确，见 `rtl-scoreboard-gated-host/{backend-audit.json,nvidia-smi-status.txt}` |

上述软件日志和计数摘要保存在 `artifacts/rtl-scoreboard-gated/{debug-full,joint-release,scoreboard-sanitize}.log`、`tests.json`。独立 RTL 与当前综合网表使用同一测试平台，不能把三次复跑当成 12 项新设计。原有 32 项独立 RTL 场景没有修改，本轮新增模块单独复跑；工具的完整场景集合现在为 36 项。

最终核对当前两份 RTL 的 SHA-256、实际 gate 网表 SHA-256，以及普通仿真、综合、网表仿真、独立 sanitizer 的四份清单，全部匹配；摘要为 `rtl-scoreboard-gated/evidence-audit.json`。

实际完整综合从 2,741,408 cells 的通用事件链，降为按阶段更新的 763,640，再加入显式时钟门为 763,658；最终均有 14,368 个触发器位，包含 63 个寄存器子实例。阶段版与门控版都通过原 `check -assert` 和网表行为检查。最终源文件、网表及测试记录见 `rtl-scoreboard-gated/`，独立 sanitizer 在 `rtl-scoreboard-gated-sanitize/`。没有 ICG/BRAM 工艺映射、布局布线或频率结论。

时钟门来自 RTL 的 `reset || |command_valid`，控制输入遵守低电平更新契约。适配器仅省略门关闭时确定没有状态变化的求值；常零 R63、常真 P7 和未变化的组合输入也避免重复求值。`clocks` 计输入模型周期，`active_clocks` 单独计有效更新边沿，不把两者混为一谈。每个 kernel 的逐拍行为和输出没有因此改变。

早期失败保留而不改写成成功：

- 首次主机恢复检查使用失败任务所在 stream，按原语义得到 `dependency_failed`；改用独立 stream 验证新 kernel，原失败传播行为保留。
- 首版测试用 `(port*7+5)%63` 生成端口目标，每 9 项重复，违反最终电路同寄存器同类事件单拍一次的约束。改用互素步长生成 16 个不同目标，完整端口/掩码检查保留。
- RTL 改动增加 Verilator 分片后，旧 CMake 文件列表导致缺失符号；显式 `cmake -S . -B <build>` 更新构建依赖后重新编译通过。
- 未门控版本的 sanitizer 主机测试触发固定 30 秒运输等待，实际异常为 `device_transport_timeout`。公开接口增加有限正数 `request_timeout`，默认不变，RTL 主机验收选择 180 秒。
- 同版 sanitizer 的 96 项执行套件达到 600 秒总上限；此前部分原子检查标为 ERROR，但总超时前未输出异常详情，不将其擅自归因于某个具体异常。原始日志在 `rtl-scoreboard-phases/sanitize-before-clock-gating.log`。门控及组合求值优化后，保持原完整算例、120 秒子进程上限、600 秒套件上限和所有检测，96 项全部通过，101.94 秒。

当前只是 scoreboard 依赖控制的电路接入；寄存器数据、bank 仲裁与 operand collector 尚未形成完整 RTL SM。完整目标继续按硬件计划推进。

## 阶段 16：寄存器数据 RTL 接入实际执行

增加 `SIMT_REGISTER_RTL`、`--register-rtl` 和 Python `register_rtl=True`。每 SM 的 65,536 字数据阵列按 1/2/4/8 个分区划分，每分区四个物理 bank；读取返回 RTL 存储或同拍写回旁路的实际值。逐 lane 有效位保证首次使用和 CTA 槽复用时返回零；C++ 历史架构副本不参与操作数读取。结构、端口共享、时钟和剩余边界见 [RTL_REGISTERS.md](RTL_REGISTERS.md)。

| 最终命令或场景 | 实际结果 |
|---|---|
| `ctest --test-dir build --output-on-failure -j4` | 25 套件、272 项运行，229 Python / 43 C++；271 通过，1 项寄存器 RTL 专项因未构建后端而跳过 |
| `ctest --test-dir build-cache-rtl --output-on-failure -j4` | 四种 RTL 后端可用的 Release 构建全部 37 套件、699 项通过（639 Python / 60 C++），5.35 秒；各套件分别选择后端 |
| `UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-register-sanitize --output-on-failure -j4` | 寄存器 RTL + Debug ASan/UBSan 全部 28 套件、405 项通过（359 Python / 46 C++），74.28 秒；Tensor/cache/scoreboard 为 C++ |
| `ctest --test-dir build-register -R 'register' --output-on-failure -j4` | 寄存器单后端 Release 最终重建后，4 套件、135 项通过：选择 2、适配器 3、执行 96、主机/Graph 34；1.74 秒 |
| 独立 RTL 与 sanitizer | `tools/rtl.py --module register_file` 默认 8 槽、64 KiB partition 的相同 4 项通过；含 2,400 次随机 masked write 和独立数组对照 |
| 保留存储综合 | `--synth --infer-only` 默认 8 槽，经层次实例展开共有 128 个一读一写存储数组，合计 524,288 数据位；`check -assert` 通过 |
| 小配置完整综合与网表 | `--rf-slots 2 --synth` 为 427,025 generic cells、135,168 触发器位；`--rf-slots 2 --gate-sim` 同 4 项全部通过。2 槽数据容量为 16 KiB，不能当成完整 SM 面积 |
| 四后端同时运行完整负载 | `hardware_host.py --build build-cache-rtl --register-rtl --scoreboard-rtl --cache-rtl --tensor-rtl --out artifacts/rtl-register-host-final`；四负载各两次重放通过，最终设备 64,500 拍 |
| 完整状态与监控 | 8 份 kernel 报告删除新增寄存器观测项后与 `rtl-scoreboard-gated-host` 完全相等，含状态和 trace；另 2 份监控 GEMM 验证四个 RTL 后端及原版 nvidia-smi 共存、分配/释放/退出 |

全量 CTest 原始日志与实际计数在 `artifacts/rtl-register-file-final/{debug,joint-release,register-sanitize}.log` 和 `tests.json`。独立工具产物在 `rtl-register-file-final`、`rtl-register-file-final-sanitize`、`rtl-register-file-final-full`、`rtl-register-file-final-gates`；五份清单及实际 gate 文件与当前 RTL 哈希匹配，保存为 `evidence-audit.json`。主机报告、状态对比和原版程序输出在 `rtl-register-host-final`。

此前 36 项独立电路场景未改动，没有宣称全部重新执行；新增寄存器 4 项使工具默认集合变为 40 项。普通 RTL、sanitizer 和网表复跑使用同一组场景，不算 12 项新测试。

首轮集成的 96 项指令检查中，1 项 CAS 替换值/WAR 检查因固定四端口几何拒绝 `--register-banks 1`。失败日志保存在 `rtl-register-file-history/first-integration.log`。实现 1/2/4 端口组的 RTL 冲突逻辑后，保留原算例、数值断言与 sanitizer，最终全部通过。没有将不支持的配置悄悄回退到 C++。

寄存器阶段结束时，数据、有效位和旁路已经电路化，collector、请求优先级、调度和谓词值仍是 C++；后续 collector/读仲裁接入见下一节。不宣称循环 bank 仲裁器、整个 SM、FPGA 存储/时钟宏映射或真实 NVIDIA 数据通路已经完成。

## 阶段 16：寄存器直连 collector 与读请求年龄仲裁（2026-10-29）

本阶段从 `063ff61` 接续，新增可选 `--register-rtl --collector-rtl`。实际 RTL 保存每分区 32 个物理条目的源描述符、三个源向量、等待位和相对年龄，读授权直接驱动寄存器堆端口。默认运行额度 4 不等于只综合 4 个条目。实现、数据路径与执行交接边界见 [RTL_COLLECTOR.md](RTL_COLLECTOR.md)。

| 验证 | 实际结果 |
|---|---|
| 默认 Debug 全量 | `ctest --test-dir build --output-on-failure -j6`：26 套件、274 项运行（231 Python / 43 C++），272 通过、2 项因未构建 RTL 而跳过，2.30 秒 |
| 联合 Release 全量 | `ctest --test-dir build-cache-rtl --output-on-failure -j6`：41 套件、834 项（771 Python / 63 C++）全部通过，5.20 秒；各套件选择其测试后端，不把全部检查说成同时开启五个 RTL |
| 寄存器/collector Debug sanitizer | `UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-register-sanitize --output-on-failure -j3`：32 套件、540 项（491 Python / 49 C++）全部通过，133.94 秒；Tensor/cache/scoreboard 为软件实现 |
| 最终适配器空闲断言 | 上述 sanitizer 全量后新增空槽和已收齐条目各 20 拍时钟门检查；重建受影响目标，3 项 C++ 适配器检查在 Release 和 ASan/UBSan 下再次通过；最终联合 Release 全量也包含这批断言 |
| collector 独立 RTL | `python3 tools/rtl.py --module collector --out artifacts/rtl-collector-final`：32 条目、5 项场景通过；相同 5 项在 `rtl-collector-sanitize` 下带 ASan/UBSan 通过 |
| 直连寄存器/collector | `--module operand_unit`：2 项真实顶层连线检查通过；同 2 项在 `rtl-operand-unit-sanitize` 下通过 |
| 完整 collector 综合及网表 | `--module collector --synth --out artifacts/rtl-collector-full-gates`：32 条目，840,085 generic cells / 102,432 触发器位；`check -assert` 通过，随后 `--gate-sim` 同 5 项全部通过 |
| 小 collector 综合与网表 | `--collector-entries 4 --synth`：122,764 generic cells / 12,804 触发器位；随后 `--gate-sim` 同 5 项通过，产物 `rtl-collector-gates` |
| 完整直连顶层存储综合 | `--module operand_unit --synth --infer-only --out artifacts/rtl-operand-unit-full`：8 个 warp 槽 RF + 32 条目 collector；按层次展开为 135 个数组、626,688 存储位，`check -assert` 通过 |
| 五后端完整主机负载 | `python3 examples/hardware_host.py --build build-cache-rtl --register-rtl --collector-rtl --scoreboard-rtl --cache-rtl --tensor-rtl --out artifacts/rtl-collector-host-final`：四类负载各两次 Graph 重放通过，设备最终 64,500 拍 |
| 状态与监控审计 | 10 个 kernel 报告确认五种 RTL 身份、collector 分配/释放/读取与时钟关系；普通 `nvidia-smi` 经桥接层显示实卡和活模拟器、7 MiB 分配、释放到零及退出移除 |

全量 CTest 日志与计数在 `artifacts/rtl-collector-final/{debug,joint-release,register-collector-sanitize}.log` 和 `tests.json`；最后增补的空闲断言结果另存 `adapter-sanitize-final.log`。独立工具每份清单记录源码哈希和参数；本阶段没有把先前 40 项独立电路检查全部重跑，新默认集合为 47 项。普通 RTL、sanitizer 与网表使用相同场景，不重复计算新增场景数。

首轮 sanitizer 的 96 项执行检查只有 `test_maximum_descriptor_shape` 出错：子进程达到原有 120 秒限制，没有数值或 sanitizer 检测报告。日志在 `rtl-collector-history/sanitize-before-idle-elision.log`。适配器改为读取实际 waiting 位，无待处理源时不打开 collector 时钟、不重复空闲求值；RF 无输入变化时也省去引脚清空后的冗余求值。相同 96 项最终 133.94 秒全部通过，最大矩阵形状、全部断言、120 秒子进程与 600 秒套件限制不变。最初直连测试的 C++ 类型推导编译错误也保存在 `first-connected-bench-build.log`，已修正为明确的无符号类型。

完整 32 条目网表第一次 Verilator/C++ 编译达到工具既有 600 秒限制；诊断保存在 `rtl-collector-history/full32-gate-build-timeout.log`。相同 `--module collector --gate-sim --out artifacts/rtl-collector-full-gates` 命令再次运行，复用已完成对象文件后编译与全部 5 项执行通过；没有改网表、缩减条目数或延长限制。最终十份清单的 RTL 源码哈希与两份实际网表哈希核对一致，结果在 `rtl-collector-final/evidence-audit.json`。

主机负载与 `rtl-register-host-final` 对比，删除新增 collector 观测项后架构状态、模型周期、缓存/DRAM 统计一致。Transformer 两次重放的 trace 在同一拍内事件顺序改变，按 `(cycle, instruction, action, sm)` 归组后相等；其余六份 trace 全量相等。没有将归组相等写成原始 trace 逐项相等。证据在 `rtl-collector-host-final/backend-audit.json` 和 `nvidia-smi-status.txt`。

当前还有 C++ 执行锁存器、发射调度、谓词值和写回请求仲裁。collector 的组合输出查询在宿主低电平阶段可顺序读取多个条目，尚非多执行单元的物理输出端口网络。综合结果只描述列出的模块，未作工艺宏映射、物理时序、整 SM 或整 GPU 门数结论。

## 阶段 16：七个执行输出端口与条目载荷电路（2026-10-29—30）

从 `81bed63` 接续，把 collector 的诊断查询交接改成七条实际执行输出总线。RTL 保存每条目的 pipe 编号，分别选择最老的就绪/准入条目，确认驱动槽位释放；C++ 不选择分区内各端口的获胜条目，也不回填向量。数据存储与旁路通过每条目的 `collector_payload.sv` 实例实现，完整结构和外部控制边界见 [RTL_DISPATCH.md](RTL_DISPATCH.md)。

| 验证 | 最终结果与范围 |
|---|---|
| 默认 Debug | `ctest --test-dir build --output-on-failure -j6`：26 套件、274 项运行（231 Python / 43 C++），272 通过、2 跳过；2.44 秒。后续修改仅涉及可选 RTL 与工具，不重复运行此构建 |
| 联合 Release | `ctest --test-dir build-cache-rtl --output-on-failure -j6`：41 套件、835 项（771 Python / 64 C++）全部通过，7.19 秒；套件按目的选择后端，不是每项都同时启用五种 RTL |
| Debug ASan/UBSan | `UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-register-sanitize --output-on-failure -j3`：32 套件、541 项（491 Python / 50 C++）全部通过，445.41 秒；此构建启用寄存器/collector RTL，其余后端为 C++ |
| 独立选择器 | `tools/rtl.py --module dispatch`：32 条目，3 项通过，包含 5,000 组随机排列；同三项 sanitizer 和综合网表也通过 |
| 完整 collector RTL | `--module collector`：32 条目，7 项通过，含七端口并行数据、1,500 拍分派对照及原 1,200 拍收集生命周期；相同 7 项 sanitizer 通过 |
| 寄存器直连 RTL | `--module operand_unit`：3 项通过，包含实际 RF 同拍写回→最高编号执行端口旁路及跨 pipe 槽复用；相同 3 项 sanitizer 通过 |
| 完整选择器综合 | `--module dispatch --synth`：8,835 generic cells，组合逻辑；`check -assert` 及同 3 项 `--gate-sim` 通过，产物 `rtl-dispatch-final` |
| 4 条目 collector 综合与网表 | `--module collector --collector-entries 4 --synth`：279,821 generic cells / 12,816 FF 位；同 7 项 `--gate-sim` 通过，产物 `rtl-dispatch-collector-gates`；七个 pipe 编号都参与测试，但 4 条目配置不能同时占满七个输出 |
| 完整 32 条目保留存储综合 | `--module collector --synth --infer-only`：7 个元数据数组共 4,192 位、98,336 个已推断 FF 位，其中载荷寄存器 98,304 位、occupied 32 位；`check -assert` 通过，产物 `rtl-dispatch-collector-full` |
| 完整直连顶层保留存储综合 | `--module operand_unit --synth --infer-only`：8 warp 槽 RF + 32 collectors；按实例层次计 135 个数组 / 528,480 存储位，以及 114,720 FF 位；`check -assert` 通过，产物 `rtl-dispatch-unit-full` |
| 实际主机与监控 | `hardware_host.py --build build-cache-rtl --register-rtl --collector-rtl --scoreboard-rtl --cache-rtl --tensor-rtl --out artifacts/rtl-dispatch-host`：四负载各两次 Graph 重放；另两次监控 GEMM。十份报告确认后端、端口确认数、释放数及完整状态/trace；原版 nvidia-smi 经桥接层确认实卡共存、7 MiB 分配、释放与退出 |

独立工具当前共 53 项，本次运行受影响的 13 项（选择器 3 / collector 7 / 直连 3）；其余 40 项独立工具场景未全部重跑。不同构建与网表复跑不重复计为新场景。最终十二份清单的 RTL 哈希与两份实际网表哈希核对一致，见 `artifacts/rtl-dispatch-final/evidence-audit.json`。CTest 原始日志及计数在同目录的 `debug.log`、`joint-release.log`、`register-collector-sanitize.log` 与 `tests.json`。未运行当前版本完整 32 条目 collector 的通用门展开/门级仿真，不能沿用旧版本的 840,085 cells 或旧网表成功记录。

完整 collector 的推断位数按状态种类相加：98,304 个数据 FF + 32 occupied FF + 4,192 元数据数组位 = 102,528 位。直连顶层还包含寄存器堆的 524,288 数据数组位和 16,384 有效位 FF。多端口元数据不是现成 SRAM 宏；层次统计按每个实例展开，不能只计一次共享 `collector_payload` 定义。没有工艺布局布线或真实频率结论。

早期失败和修正均保留在 `rtl-dispatch-history`：适配器首次编译因小整数提升后的窄化转换被 `-Werror` 拒绝，改成明确的无符号转换；初版 4/32 条目综合都达到 600 秒，粗网表显示 24 个 4096→1024 位可变截取，改成固定 bank 选择。随后独立条目准备减少重复逻辑，4 条目成本由 851,890 降至 354,937；但摊平结构的完整综合与 sanitizer 96 项套件仍达到 600 秒，日志完整保留。最终采用可复用载荷子模块、只复制所选条目的选择逻辑，4 条目为 279,821，完整容量推断综合与原 96 项检查通过。没有缩小容量、矩阵形状或断言；120 秒子进程、600 秒套件及综合限制全部保留。诊断后的旧综合作业曾主动终止，记录与超时区分，没有把终止算成通过。

最终网表第一次链接还触发旧 Verilator 的归档残留：已经移除的生成分片留在 `__ALL.a` 中，造成重复符号。工具现在构建前重建该归档，并在复跑开始时移除旧成功清单，防止失败后读取过时结果；相同最终网表随后 7 项全部通过，失败日志保存在 `stale-gate-archive-link-error.log`。

最终十份 kernel 报告删除新增 `dispatch_ports/dispatches/dispatch_waits` 字段后，与 `rtl-collector-host-final` 的完整架构状态、逐拍 trace、周期和其他统计全量相等，主机验证汇总也相等。证据为 `rtl-dispatch-host/backend-audit.json`、`nvidia-smi-status.txt` 及运行脚本 `check_monitor.py`。监控依然是 NVML 管理面接入，不是 CUDA 驱动枚举。

这一阶段完成执行输出端口选择、数据路由和确认，但执行锁存器、跨分区共享单元归属、写回仲裁与完整 SM 控制仍有 C++，完整硬件目标保持未完成。

## 阶段 16：完成存储、写回年龄仲裁与 RF 直连（2026-10-30）

从 `6ef824f` 接续。每分区 32 个完成槽在发射时预约，七类执行端口各每拍接收一个结果；实际载荷、归属、完成位和年龄存入 RTL。四个 RF bank 按 1/2/4 端口组仲裁，另有一个谓词端口；无写入指令仍占预约槽。确认驱动直接 RF 写入、同拍 collector 旁路、scoreboard 释放和容量归还。启用 collector 后端即启用这条通路，见 [RTL_WRITEBACK.md](RTL_WRITEBACK.md)。

| 验证 | 实际结果与范围 |
|---|---|
| 默认 Debug | `ctest --test-dir build --output-on-failure -j4`：26 套件、274 项（231 Python / 43 C++），272 通过、2 跳过，2.52 秒 |
| 联合 Release | `ctest --test-dir build-cache-rtl --output-on-failure -j4`：42 套件、839 项（772 Python / 67 C++）全部通过，7.57 秒 |
| 最终 Debug ASan/UBSan 全量 | `UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-register-sanitize --output-on-failure -j3`：33 套件、545 项（492 Python / 53 C++）全部通过，532.11 秒；软件时序套件 98.64 秒，97 项 RTL 指令套件 532.11 秒；实际配置包含寄存器/collector/写回 RTL，其余为 C++ |
| 首次寄存器/collector/写回 Debug ASan/UBSan | 33 套件中 32 通过，556.12 秒；97 项 RTL 指令套件和三个写回适配器检查通过，原软件 `timed_sm_execution` 达到 90 秒；没有把超时计为成功 |
| 软件时序套件定向复跑 | 仅对上述构建将套件总限时改为 180 秒后，35 项软件时序检查在 95.64 秒全部通过；三个写回适配器检查再通过，复跑总计 96.70 秒；记录 `rtl-writeback-final/sanitize-rerun.log` |
| 完整 32 槽写回 RTL / sanitizer | 相同五项通过：保存/旁路、容量/年龄/复位、拒绝非法输入、七生产端口并行、3,000 拍独立随机参考；产物 `rtl-writeback` / `rtl-writeback-sanitize` |
| 完整 32 槽综合及网表 | `--module writeback --synth`：739,485 generic cells / 34,496 FF 位；`check -assert` 和同五项实际 `--gate-sim` 通过，产物 `rtl-writeback-full-gates` |
| 4 槽综合及网表 | `--collector-entries 4 --synth`：88,869 cells / 4,312 FF 位；同五项网表检查通过，四槽配置并行检查四个生产端口，完整 32 槽检查全部七个 |
| 完整 RF/collector/写回连接 | `--module operand_unit` 普通及 sanitizer 均四项通过；新增检查同时要求同拍旁路和下一拍仍能从 RF 读取结果，产物 `rtl-writeback-connected` / `rtl-writeback-connected-sanitize` |
| 完整组合顶层存储综合 | 8 warp 槽、32 collector、32 完成槽的 `--infer-only` 通过；按实例展开为 141 个数组 / 530,144 数组位，加 147,552 FF 位；没有宣称这一完整组合顶层已经做通用门网表仿真 |
| 主机、Graph 与监控 | `hardware_host.py --build build-cache-rtl --register-rtl --collector-rtl --scoreboard-rtl --cache-rtl --tensor-rtl --out artifacts/rtl-writeback-host`：四负载各两次重放；另两次监控 GEMM。十份报告检查写回 RTL 身份、预约/完成/退休数等于 issued、占用归零以及无隐藏时钟；实际 `nvidia-smi` 确认实卡共存、7 MiB 分配、释放与退出 |

首次联调的组合门控时钟会在写回控制状态更新时撤掉 RF 写入：同拍旁路正确，但下一拍读取错误。改为集成 RF 与写回共用源时钟、每 lane 显式写使能后，原失败套件全部通过。早期日志在 `rtl-writeback-history/before-shared-clock-tests.log`；旧 CMake 生成规则缺少新模块时的构建失败也保留为 `stale-build-graph.log`，显式重新配置后解决。

软件时序套件在 sanitizer 下全量和单独运行均达到原 90 秒；已完成场景没有数值或 sanitizer 报告。只将 `SIMT_REGISTER_RTL && SIMT_SANITIZE` 下此套件的总限时改为 180 秒，保留全部场景及 8 秒子进程限制；其他构建仍为 90 秒，RTL 执行套件仍为 600 秒、单次 120 秒。两次超时记录、后续复跑及最终计数保存在 `rtl-writeback-history` 和 `rtl-writeback-final`。首次调整属性时放在测试注册前的 CMake 错误也保留，已移到注册后。

主机验证保留了实际变化：十份报告的架构状态、总周期与其他统计相等；规约两次重放各增加 3 次完成端口等待，三条与同分区另一条 LDS 同拍返回的 LDS 晚一拍退休。其余八份报告的事件和时钟按同拍归组相等；两份向量报告的原始 trace 相等。先汇集全部结果再仲裁会改变其他同拍事件的记录顺序，因此不能声称所有原始 trace 不变。`backend-audit.json` 和 `rtl-writeback-history/host-state-differences.json` 保留逐条差异。

工具集合由 53 增至 59 个独立场景，本次运行直接受影响的九个（写回 5、组合顶层 4），没有声称重跑全部独立模块。十份当前 RTL 源码清单与两份实际网表 SHA-256 核对一致，见 `rtl-writeback-final/evidence-audit.json`。综合网表检查是同一场景的复跑，不增加独立场景数；generic cells 不等于 NAND2 门数、工艺面积或整 GPU 成本。

目标仍未完成：执行流水线与各生产端口的输入选择、完整谓词值存储、跨分区共享资源仲裁和其他整机 RTL 连接仍有 C++，还需实卡参数校准、PTX 异步矩阵协议与最终要求审计。

## 阶段 16：整数数值、有限执行队列与写回直连（2026-10-30）

从 `09000bb` 接续。16 种整数操作全部进入 32 lane RTL，源值由 collector 总线直接供给，结果经过 32 项 FIFO 到写回端口 0。C++ 只传递元数据并追踪指令身份，既不复制整数源向量，也不计算整数结果或按 `due` 时间完成。队列包含真实存储、倒计数、容量和反压；组合 ALU 没有被描述成内部四级流水乘法器，详见 [RTL_INTEGER.md](RTL_INTEGER.md)。

| 验证 | 实际结果与范围 |
|---|---|
| 默认 Debug | `ctest --test-dir build --output-on-failure -j3`：26 套件、274 项（231 Python / 43 C++），272 通过、2 跳过，3.21 秒 |
| 联合 Release | `ctest --test-dir build-cache-rtl --output-on-failure -j3`：42 套件、841 项（773 Python / 68 C++）全部通过，8.80 秒 |
| Debug ASan/UBSan 全量 | `UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-register-sanitize --output-on-failure -j3`：33 套件、547 项（493 Python / 54 C++）全部通过，598.32 秒；其中实际 collector/整数指令套件 98 项。构建包含寄存器/collector/整数/写回 RTL，其余为 C++；保留原 600 秒套件限制，无 sanitizer 报告 |
| 完整 32 项整数 RTL / sanitizer | 四项独立检查均通过：全部整数操作与数值参考、地址转换/非法命令、容量/保持/复用/复位/1/4/100 拍、6,000 拍随机时间与反压参考；产物 `rtl-integer` / `rtl-integer-sanitize` |
| 完整 32 项整数综合与网表 | `--module integer --synth` 实际得到 314,847 generic cells / 34,005 FF 位；`check -assert` 及相同四项 `--gate-sim` 全部通过，产物 `rtl-integer-synth` |
| 两项小队列 | `--module integer --collector-entries 2` 同四项 RTL 检查通过，验证最小公开工具配置的满队列与首尾回绕；未声称此配置另做门级仿真 |
| 组合顶层与写回 | `--module operand_unit` 普通/sanitizer 各五项，含实际最后源旁路→整数 ALU→写回→后续 RF 读取；`--module writeback` 当前源码普通/sanitizer 各五项，新增完成位观测不改变原行为 |
| 四种分区几何 | C++ 写回适配器新增一项覆盖 1/2/4/8 分区的整数直连、掩码、拒绝后重试、生产端口所有权、无隐藏时钟和持久数据；包含在上述 CTest 总数中 |
| 组合顶层保留存储综合 | 8 warp 槽、32 collector、32 完成槽、32 整数结果槽的 `--infer-only` 和 `check -assert` 通过；按实际层次实例展开为 144 个数组 / 564,096 数组位，加 147,600 FF 位。没有进行整个组合顶层的通用门网表仿真 |
| 主机 Graph | `hardware_host.py --build build-cache-rtl --register-rtl --collector-rtl --scoreboard-rtl --cache-rtl --tensor-rtl --out artifacts/rtl-integer-host`：四负载各两次重放全部通过，完整验证汇总与 `rtl-writeback-host` 相等，设备总计 64,500 模型周期 |
| 实际监控 | 另一个启用联合 RTL 的活设备完成两次 Graph GEMM，每次验证 96 个输出及 114 条整数 RTL 指令；普通 nvidia-smi 同时显示实卡和模拟器，7 MiB→0 MiB→退出移除；系统绝对路径只显示实卡。证据 `rtl-integer-host/monitor/` |

八份完整负载报告除新增 `integer_units` 字段外，架构状态、原有统计和原始逐拍 trace 均与上一阶段全量相等。每次 vector/reduction/GEMM/Transformer 重放分别有 100/510/114/874 条整数指令，实际电路的 accepted/completed 与 trace 中的整数执行数逐一相等，所有结果队列最终为空；证据 `rtl-integer-host/backend-audit.json`。没有用“数值相等”代替后端身份和实际握手检查。

首次连接的 RTL lint 指出了整数接收→collector 门控时钟的反馈路径。组合顶层改用 collector 的 `SHARED_CLOCK=1`，保留独立模块配置和各数据存储写使能，严格 lint 后通过；不是屏蔽 UNOPT 告警。日志为 `rtl-integer-history/collector-clock-lint.log`。组合测试驱动首次还出现同一 `auto` 声明推导为不同整数类型的编译错误，改成明确的 `uint32_t` 后复跑通过，原日志保留为 `connected-build-type-failure.log`。

独立工具集合由 59 增至 64 项，本次运行受影响的 14 项：整数 4、组合顶层 5、写回 5。不同构建、队列大小和网表复跑不重复增加独立场景。当前源码与网表 SHA-256 审计见 `rtl-integer-final/evidence-audit.json`；CTest 原始输出及计数见同目录。上述 generic cells 不是 NAND2 等效门数，也没有给出 FPGA/ASIC 布局布线或频率结论。

完整目标仍未完成：整数算术内部流水级、浮点/SFU/控制执行、完整谓词值存储、跨分区共享单元仲裁、其余 GPU RTL 连接、实卡参数校准和 PTX 异步矩阵协议仍需继续。

## 阶段 16：普通浮点四级电路、转换比较与结果预约（2026-10-30）

从 `eff0684` 接续。FADD/FMUL/FFMA 直接使用每分区 32 份四级 FMA，FSETP/I2F/F2I 用位电路及四级对齐寄存器；结果发射前预约 32 项有界存储。collector 端口 1 到算术，再到写回端口 1 和 RF 的数值通路全部直连，C++ 不复制源向量、不调用浮点数值函数、不按 `due` 时间产生结果。默认六拍保留，四拍是物理下限，额外延迟由 RTL 倒计数推进；小于四拍明确拒绝，见 [RTL_FLOATING.md](RTL_FLOATING.md)。

| 验证 | 实际结果与范围 |
|---|---|
| 默认 Debug | `ctest --test-dir build --output-on-failure -j3`：26 套件、274 项（231 Python / 43 C++），272 通过、2 跳过，3.20 秒 |
| 联合 Release | `ctest --test-dir build-cache-rtl --output-on-failure -j3`：最终 45 套件、847 项（778 Python / 69 C++）全部通过，8.56 秒；分组增加两个套件，没有增加或遗漏原有场景 |
| 寄存器联合 RTL Debug ASan/UBSan | `ctest --test-dir build-register-sanitize --output-on-failure -j3`：最终 36 套件、553 项（498 Python / 55 C++）全部通过，600.32 秒；包含寄存器、collector、整数、浮点及写回 RTL，Tensor/cache/scoreboard 为 C++ 后端，独立 Tensor RTL sanitizer 另见下方 |
| 完整 32 槽浮点 RTL / sanitizer | 相同五项独立检查通过，包括六种操作、NaN/带符号零、融合舍入、随机整数与舍入边界、越界转换、掩码、有限容量、同拍复用、在途复位和 6,000 拍随机流；产物 `rtl-floating` / `rtl-floating-sanitize` |
| 完整 32 槽浮点综合与网表 | `--module floating --synth`：588,501 generic cells / 53,809 FF 位，`check -assert` 及相同五项实际 `--gate-sim` 通过，产物 `rtl-floating-synth` |
| 两项小队列 | `--module floating --collector-entries 2` 同五项 RTL 检查通过，包括物理流水级仍在推进时队列满、出队复用和绕回；未对小配置另做门级仿真 |
| 组合顶层 | `--module operand_unit` 普通/sanitizer 各六项通过，新增最后源同拍送入浮点、四级算术加两拍等待、真实 RF 写回及后续读取；产物 `rtl-floating-connected` / `rtl-floating-connected-sanitize` |
| 四种分区几何 | 写回适配器新增检查 1/2/4/8 分区下 masked fused cancellation、低延迟拒绝后重试、完成端口归属、无隐藏时钟和持久 RF；已包含在 CTest 总数中 |
| FMA / PE / Tensor 回归 | 因新增 FMA 各级有效写使能，三模块普通 RTL、sanitizer、重新综合及同一实际网表分别通过四/三/六项。FMA 保留 250,000 随机/抵消、13,824 边界组合及 12,000 有理数参考；完整 Tensor 网格和输入/输出反压也复跑 |
| FMA / PE / Tensor 实际成本 | 新 generic cells 分别 11,504 / 13,099 / 218,054，FF 位保持 478 / 794 / 16,636；产物 `rtl-floating-fma` / `rtl-floating-pe` / `rtl-floating-tensor`，旧阶段成本记录保留其当时范围 |
| 组合顶层保留存储综合 | RF 8 warp 槽 + collector 32 项 + 整数 32 项 + 浮点 32 项 + 写回 32 项的 `--infer-only` 和 `check -assert` 通过；层次展开 147 个数组 / 598,048 数组位，另 167,612 FF 位，没有宣称整个组合顶层完成通用门仿真 |
| 主机 Graph | 四负载各重放两次，全部数值、原有统计、模型周期及原始 trace 与 `rtl-integer-host` 全量相等，新增 `floating_units` 单独审计；主机仍为 64,500 模型周期，证据 `rtl-floating-host/backend-audit.json` |
| 原版 nvidia-smi | 另一个联合 RTL 活设备完成两次 Transformer Graph，每次普通浮点 RTL 接收/完成均为 70、Tensor 后端为 RTL，验证 32 个输出；实卡共存、7 MiB→0 MiB→退出移除通过，绝对系统路径只显示实卡。脚本与证据 `rtl-floating-host/check_monitor.py` / `monitor/` |

主机 vector 每次包含 10 条普通浮点指令，Transformer 每次 70 条；对应实际电路 accepted/completed 与 trace 逐条匹配，所有结果队列最终为空。reduction 和 Tensor GEMM 的普通浮点计数为零，不能用它们冒充新增普通浮点通路覆盖。新 ISA 浮点套件包含五项测试，另覆盖 4/6/100 拍依赖链、lane 转换边界、40 条独立浮点指令填满 32 个完成槽、异常和低于物理流水级数的配置拒绝。

独立工具集合由 64 增至 70 个场景，本阶段运行受影响的 24 项：浮点 5、组合顶层 6、FMA 4、PE 3、Tensor 6。不同构建和综合网表复跑不重复计数；没有宣称重跑全部独立模块。20 份当前源码清单和四份实际网表 SHA-256 已核对，见 `rtl-floating-final/evidence-audit.json`。完整浮点单元的门数不等于整个 SM，更不等于两个 SM 或整张显卡。

长任务期间增加两行 FMA 解释注释，使首次普通浮点 simulation/synthesis 清单与最终源码哈希不同；数值逻辑没有变化，仍重新执行当前源码的仿真、综合和网表检查。`tools/rtl.py` 现在在调用前保存源码哈希，结束后若有变化则拒绝发布成功清单；重新综合开始前也移除旧 netlist 清单。原诊断见 `rtl-floating-history/source-snapshot-recheck.json`，最终清单只接受源码稳定后的复跑结果。

首次全量 sanitizer 运行 34 套件，其中 33 通过，原单个 `rtl_collector_execution` 在 600.02 秒达到总限时；98 个场景中 85 个已输出通过，没有数值或 sanitizer 报告，但该次运行不算全部通过。现在将这 98 项按 core 30 / memory+async 49 / Tensor 19 分成三个 CTest 套件，保留原每套件 600 秒及每子进程 120 秒限制。实际 unittest 发现集合与三个分组的并集全等且无重复，证据 `rtl-floating-history/collector-suite-partition.json`；原完整失败输出和分析分别为 `sanitize-before-suite-split.log`、`sanitize-timeout-analysis.json`。直接运行原测试脚本不指定分组仍执行全部 98 项。

最终 sanitizer 三组分别在 252.14、222.68、120.83 秒通过。Release 和 sanitizer 的实际输出均逐项核对全部 98 个测试名称，与原发现集合完全相等。最终完整明细及计数保存在 `rtl-floating-final/{joint-release.log,register-sanitize.log,tests.json,collector-execution-audit.json}`；600.32 秒是整轮并行 CTest 的总耗时，不能与单套件 600 秒限时混淆。

分组后的首次 Release 全量运行通过，但后续 `ctest --show-only=json-v1` 覆盖了 CTest 的明细日志；那份被覆盖的日志不作场景计数证据。已完整重跑并保存 `rtl-floating-final/joint-release.log`，逐项核对 98 个 collector 场景与分组清单全等，再统计上表的最终结果。

SFU、控制/访存执行电路、完整谓词值存储、整数内部算术流水级、跨分区共享单元仲裁和其余 GPU RTL 仍需完成。实卡参数校准、PTX 异步矩阵协议及最终整体验收也未完成，目标保持 active。

## 阶段 16：谓词值存储、双读端口与直连退休（2026-10-30）

从 `fbbbddf` 接续。`predicate_file.sv` 保存每 warp 七个 32 位字及每字有效位，P7 为常量；guard 的取反/PC 组筛选与 VOTE 源来自两个实际读多路器。寄存器后端不再从架构谓词镜像取得执行输入。collector 模式的写回第五端口直接驱动物理谓词写入，包含 mask、同拍旁路、年龄优先和清槽复用；见 [RTL_PREDICATES.md](RTL_PREDICATES.md)。

| 验证 | 实际结果与范围 |
|---|---|
| 默认 Debug | `ctest --test-dir build --output-on-failure -j3`：26 套件、274 项（231 Python / 43 C++），272 通过、2 跳过，3.10 秒 |
| 联合 Release | `ctest --test-dir build-cache-rtl --output-on-failure -j3`：最终 46 套件、852 项（781 Python / 71 C++）全部通过，8.93 秒；首次同范围运行 9.85 秒，探测求值优化后再次全量验证 |
| 寄存器联合 RTL Debug ASan/UBSan | `ctest --test-dir build-register-sanitize --output-on-failure -j3`：最终 37 套件、558 项（501 Python / 57 C++）全部通过，513.88 秒；包含普通/谓词寄存器、collector、整数、浮点及写回 RTL，Tensor/cache/scoreboard 为 C++ 后端 |
| 谓词独立 RTL | 四项检查在 4/8/32 槽通过：双端口、P7、guard/group、边沿、掩码、清槽/写入同拍、隔离、错误拒绝及 12,000 拍独立随机参考；8 槽同样通过 ASan/UBSan |
| 谓词综合与实际网表 | 8 槽 13,733 cells / 1,848 FF 位；32 槽 53,573 cells / 7,392 FF 位。两种配置均通过 `check -assert` 和同四项 `--gate-sim`，产物 `rtl-predicate-synth` / `rtl-predicate-full` |
| 寄存器 / 写回 / 组合顶层 | 普通 RTL 和 sanitizer 分别通过 5/5/7 项；包含新增谓词存储连接、实际退休总线与保留值读取。写回完整 32 槽另重新综合及同五项实际网表验证，740,745 generic cells / 34,496 FF 位 |
| 组合顶层保留存储综合 | 8 warp 槽配置 `--infer-only` / `check -assert` 通过：147 数组、598,048 数组位、169,460 FF 位；FF 较上一阶段增加 1,848，正好等于该分区谓词存储。不宣称整个组合顶层完成门级验证 |
| 实际 ISA 与适配器 | 三项 Python 用例覆盖两种寄存器后端、1/2/4/8 分区、部分写入/VOTE/ELECT、FSETP 退休到发射同拍旁路、37 线程与多 CTA 槽复用。两个新增 C++ 用例覆盖四种几何、读取捕获、写回保留与年龄、外部写端口冲突；均包含在 CTest 总数中 |
| 主机 Graph | 四负载各两次重放；除新增 `predicate_files` 外，全部状态、旧统计、模型周期和原始 trace 与 `rtl-floating-host` 全量相等。每次 vector/reduction/GEMM/Transformer 的实际谓词写为 20/45/6/56，与汇编中的谓词指令 PC 及退休 trace 独立计数一致 |
| 原版 nvidia-smi | 启用全部已接入 RTL 的活设备完成两次 Transformer Graph，每次验证 32 个数值、70 条普通浮点及 56 次谓词写入；实卡共存、7 MiB→0 MiB→退出移除通过，系统绝对路径仍仅显示实卡。证据 `rtl-predicate-host/monitor/` 与复现脚本 `check_monitor.py` |

独立工具集合从 70 增至 76 个场景；本轮运行受影响的 21 项：谓词 4、寄存器 5、写回 5、组合顶层 7。不同几何、构建和网表复跑不重复计数，没有宣称重跑全部独立电路。17 份当前 RTL 源码清单和三份实际网表 SHA-256 已核对，记录在 `rtl-predicate-final/evidence-audit.json`；完整 CTest 明细和计数保存于同目录。主机验证、旧字段对照和退休计数证据为 `rtl-predicate-host/backend-audit.json`。

首次全量 ASan/UBSan 为 37 套件、36 通过，`rtl_collector_memory_async` 在 600.02 秒达到总限时；该次不能记作全部通过。失败明细保存在 `rtl-predicate-history/sanitize-before-probe-cache.log`。检查发现谓词候选查询无论引脚是否改变都会重新求值整个组合模块，给等待访存的周期增加了大量宿主开销。适配器现只在查询引脚变化时额外求值；写入、退休和时钟操作仍正常求值，返回的是当前电路输出。未调整任何测试分组、600 秒套件限时或 120 秒子进程限时。

保留优化前 sanitizer 可执行文件，交替顺序运行相同输入并比较完整 JSON 输出：随机原子场景为 13.73/13.81 秒对 5.40/5.27 秒，模型均为 6,513 拍；六 CTA 有限原子队列场景为 77.13/77.75 秒对 2.38/2.40 秒，模型均为 16,740 拍。完整输出逐项相等；这些是宿主仿真耗时，不是 GPU 性能。复现脚本、二进制哈希和两轮原始观测为 `rtl-predicate-history/{compare_probe.py,sanitize-timeout-analysis.json,probe-evaluation-comparison.json}`。再次完整回归中，原超时的 49 项存储/异步组在 246.59 秒通过。

最终 Release 和 sanitizer 输出均逐项核对 30/49/19 个 collector 场景，与上一阶段完整 98 项发现集合完全相等、无重复。证据为 `rtl-predicate-final/collector-execution-audit.json`，最终全量明细为同目录 `joint-release.log`、`register-sanitize.log` 和 `tests.json`。优化后的实际主机 Graph 与监控也重新执行，未只复用旧版二进制的输出。

谓词存储和读写通路完成本阶段验证不等于完整 SM：调度、解码、PC 与集合运算仍为 C++，SFU、控制/访存电路、跨分区共享单元、整数内部算术流水级、整机 RTL、实卡校准和 PTX 异步矩阵协议仍须继续，完整目标保持 active。

## 阶段 11/12：真实 4070 SUPER 存储实验（2026-10-30）

从 `1ef8cfc` 接续，新增 CUDA/PTX 微基准与调用正式 `simt_hardware` 的 C++ 探针，没有修改现有 RTL 或模型参数。方法与解释边界见 [MEMORY_CALIBRATION.md](MEMORY_CALIBRATION.md)，Git 内原始证据见 `docs/evidence/memory-4070-20261030/`。

```sh
python3 tools/calibrate_memory.py --mode full --out artifacts/memory-4070-locality --repeats 5
python3 tools/calibrate_memory.py --mode check --out artifacts/memory-4070-check
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
python3 tools/kernel_perf/perf.py run --config tools/kernel_perf/configs/memory-coalescing.json --benchmark-only
python3 tools/kernel_perf/perf.py run --config tools/kernel_perf/configs/memory-coalescing.json
```

- 最终原生实验：118 组/590 个样本，全部逐次验证数值；包括两种 seed、carveout 偏好、局部性分组和随机化实验顺序。模型探针 30 组检查通过，6 组 coalescer 与独立地址枚举比较，8 组 shared 和 16 组 cache 读出真实数据。
- 探索轮 `memory-4070-full` 的 106 组原生点和最终轮分开存储，不将重复实验计为独立覆盖。两个非零迭代长度的差分斜率与零迭代分布单独保存；原生 event 和 profiler event 不混用。
- CUDA Compute Sanitizer：10 项 memcheck 均零错误，shared racecheck 零 errors/零 warnings。包含分组指针链、零次加载、513 次奇数循环、broadcast、32 路冲突、stride 33 和错位/最大合法地址组合。
- 默认 Debug：27 套件全部通过，9.34 秒；279 项中 277 通过、2 项因未构建 RTL 跳过。新增 5 项验证探针的真实路径、数值和报告口径。没有为了注释、函数行数或画图格式写测试。
- `kernel_perf` 的 doctor 与原生 benchmark-only 成功。最终原生记录 `f9e644432ed4419f88bc21fa31b6992f` 含编译输出与二进制哈希；先前 `a7fca69319a5438590e356592a7f376e` 原生成功但没有向外层 build.log 透传 ptxas 输出，已保留并修复该日志交接。
- NCU 记录 `c38553e449d04715bda7ef3843aca4d9` 明确失败于 `ERR_NVGPUCTRPERM`，其原生 check 和 benchmark 成功。hardware sectors 为未知；没有声称指标名称已由本卡成功查询或采到 counter。没有改变驱动设置或提升采集权限。
- 最终工具复跑 `memory-4070-final-smoke`：46 组/92 个原生样本及 30 组模型探针通过；另检查超时保留部分日志、缺少样本明确失败。它不与主实验的 590 样本合并统计。
- 结果审计核对了冻结源码和两个可执行文件的 SHA-256，原始样本/模型结果和本次实际编译代码一致。后续增加测试注册、文档、工具日志透传和采集结果完整性检查，没有改动已测 CUDA/C++ kernel 源码。完整硬件目标保持 active。

## 阶段 16：跨 lane 集合执行与第三谓词读口（2026-10-30）

从 `a61020f` 接续，SHFL/VOTE/ELECT/ACTIVEMASK 在 collector 模式下改由真实 RTL 计算，控制端口与写回直连。没有回退到 C++ 求值。结构、命令和精确边界见 [RTL_COLLECTIVES.md](RTL_COLLECTIVES.md)。

- 默认 Debug：27 套件/279 项，277 通过、2 跳过，9.42 秒。联合 Release 的 Tensor/cache/scoreboard/register 四项构建选项全开：48 套件/862 项全部通过（790 Python、72 C++），25.68 秒。执行时是否使用每个后端仍由各套件决定，不声称每项测试都同时启用全部后端。
- 受影响独立电路共 21 项（集合 4、谓词 4、RF 5、直连顶层 8），RTL 和 Debug ASan/UBSan 都通过。独立场景总集合由原 76 变为 81，本次没有复跑所有未受影响模块。未重跑完整联合 C++ 后端 sanitizer 套件，不能把独立电路 sanitizer 冒充该项证据。
- 集合四项中包含 3,000 组功能 ISA 差分、5,000 拍独立反压参考、非法成员/索引、空参与、重置和同拍替换。谓词 12,000 拍随机参考扩展为同时核对第三独立读口；直连顶层新增一项将发射与执行地址置于不同 warp。
- 集合单元实际综合 55,423 generic cells/1,030 FF 位，32 槽三读口谓词文件 68,814 cells/7,392 FF 位；各自的相同 4 项测试在实际综合网表通过。完整联合顶层没有重新做门级综合，不声称已得到整 SM 面积。
- 初次 CMake 接线把执行谓词端口也传给了 writeback 实例，编译明确拒绝；修正连接后通过。第一次独立工具调用同时给出 --synth/--gate-sim，被参数检查拒绝；随后按规定分开执行。
- 首轮联合测试有两项新夹具错误：VOTE 期望误用了递增向量辅助函数，另一测试向 VOTE/ELECT 传了不适用的编码字段。按广播语义和 ISA 字段修正测试，保留原失败日志后，全 48 套件重跑通过；没有为通过测试修改运算或错误语义。
- `tools/rtl.py` 原聚合清单会被同一输出目录的下一模块覆盖。新增各模块独立清单，并在该模块复跑前删除旧清单。为补齐被覆盖的原证据，保留已有记录并复跑对应的谓词/RF/集合小模块。最终 12 份清单的 RTL 源码及两份实际网表哈希均核对一致，见 `artifacts/rtl-collective-final/evidence-audit.json`。
- 五个可选后端同时运行四类主机 Graph 各两次，数值独立校验通过，设备累计 64,500 模型拍。每次规约 75 条、小 Transformer 32 条集合指令来自真实 RTL 接收计数；向量/GEMM 不含这四类指令。结果在 `artifacts/rtl-collective-host/`。
- 监控会话两次规约 Graph 通过；原版 nvidia-smi 经用户级入口与桥接层同时显示实卡及模拟器，实际分配 7 MiB、释放 0 MiB、退出消失。证据 `artifacts/rtl-collective-final/{allocated,freed,exited}.csv`；未变成 CUDA 或 PCIe 枚举。
- 完整目标仍 active：SFU、分支/重聚/CTA 屏障控制、其余访存电路、整机 RTL 接线、参数辨识与最终硬件审计未完成。

## 阶段 16：分支控制与 RTL 重聚令牌（2026-10-30）

本阶段从 `ee5067c` 接续。实际六类控制指令读取并更新每物理 warp 的 16 个 RTL 令牌；lane PC/状态仍是显式宿主输入和输出，未实现完整硬件调度器。结构、存储和接口边界见 [RTL_WARP_CONTROL.md](RTL_WARP_CONTROL.md)。

实际验收命令：

```sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
cmake -S . -B build-cache-rtl -DCMAKE_BUILD_TYPE=Release \
  -DSIMT_TENSOR_RTL=ON -DSIMT_CACHE_RTL=ON \
  -DSIMT_SCOREBOARD_RTL=ON -DSIMT_REGISTER_RTL=ON
cmake --build build-cache-rtl -j4
ctest --test-dir build-cache-rtl --output-on-failure
python3 tools/rtl.py --build build-release --out artifacts/rtl-warp-control --module warp_control
python3 tools/rtl.py --build build-release --out artifacts/rtl-warp-control --module warp_control --synth
python3 tools/rtl.py --build build-release --out artifacts/rtl-warp-control --module warp_control --gate-sim
UBSAN_OPTIONS=halt_on_error=1 python3 tools/rtl.py --build build-sanitize \
  --out artifacts/rtl-warp-control-sanitize --module warp_control
python3 examples/hardware_host.py --build build-cache-rtl --tensor-rtl --cache-rtl \
  --scoreboard-rtl --register-rtl --collector-rtl --out artifacts/rtl-control-host
```

- 默认 Debug：27 套件，236 Python + 43 C++ = 279 项，277 通过、2 项未构建 RTL 而跳过，9.75 秒。联合 Release：50 套件，804 Python + 76 C++ = 880 项全部通过，27.47 秒。原始输出为 `artifacts/rtl-control-final/{default,joint}-{ctest,tests}.log`。
- 新控制模块四项独立场景分别通过 RTL、ASan/UBSan 与实际综合网表。包含 200 轮全 16 令牌随机到达（9,600 次事务）、3,000 次 BRA/YIELD 随机向量、嵌套/EXIT/BREAK 唤醒、错误命令原子性、清零与解除状态。参考复用功能 ISA 实现，不使用 RTL 计算预期值。
- 四项新适配器测试覆盖空闲时钟门控、只在 tick 提交、待提交命令保护、清槽、两实例隔离、16 项存储、修改观察快照不影响硬件、错误撤销引脚及 96 位 lane 状态打包。ISA 新套件复跑九项既有控制测试并新增三项；两项持久主机检查验证 16 令牌/9 CTA/37 线程的两次 Graph 重放、333 个内存输出及失败 kernel 隔离。
- 首版组合过程逐令牌改写同一 lane 向量，Yosys 长时间停留在 `PROC_DLATCH`，人工停止该次工具进程。日志保存在 `initial-synthesis-latch-pass.log`；启动器历史日志包含被停止进程的 `-15` 返回。重写为完成向量和按等待编号查询后，最终综合成功且无残留 memory/process；所有受影响验证在最终 RTL 上重新通过。没有获得首版有效综合成本。
- 最终单 warp 模块为 68,030 generic cells、1,568 FF 位。`netlist.json` 关联最终源码与 `gates.v` 哈希，四份模块清单（RTL、sanitizer、综合、网表）全部核对一致。工具新增独立场景总数由 81 增为 85；本阶段重跑的是新增四项，不把历史模块算成本次重新验收。
- 五个后端同时启用的四类 Graph 各两次通过数学参考；额外两次带监控的规约也通过。十份报告和 trace 删除新增 `warp_controls` 后与上一阶段完全一致。四类原负载只有 EXIT 使用新单元；重聚运算由新增 ISA、适配器和持久主机测试覆盖。
- 普通 `nvidia-smi` 实测显示真实 4070 SUPER 和活模拟器；模拟内存 7 MiB→0 MiB，退出后移除。绝对路径 `/usr/bin/nvidia-smi` 在模拟器存活时仍只看见实卡。列表、表格、分配/释放/退出和监控负载记录保存在 `artifacts/rtl-control-final/`；未提供 CUDA/PCIe 设备兼容。
- 最终 `evidence-audit.json` 冻结 273 个源码文件与三个联合二进制，核对四份清单、一个实际网表和十份主机报告。完整联合 sanitizer、其他未修改电路的综合和完整 GPU 门级连接没有在本阶段重新运行。
- 整体目标仍 active；PC/lane 存储、发射/YIELD 恢复、CTA 屏障、SFU、其余访存电路、整机 RTL、实测校准、PTX 异步矩阵协议与最终硬件审计未完成。

## 阶段 16：Warp 状态存储与 PC 选择前端（2026-10-30）

从 `4462d34` 接续，将 PC/lane/等待元数据/轮转游标的持久状态及 PC 组选择接入真实 RTL，并直接连接重聚子模块。跨 warp 仲裁和 CTA/异步计数仍有 C++；完整接线和状态更新顺序见 [RTL_WARP_FRONTEND.md](RTL_WARP_FRONTEND.md)。

实际验收命令：

```sh
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
cmake -S . -B build-cache-rtl -DCMAKE_BUILD_TYPE=Release \
  -DSIMT_TENSOR_RTL=ON -DSIMT_CACHE_RTL=ON \
  -DSIMT_SCOREBOARD_RTL=ON -DSIMT_REGISTER_RTL=ON
cmake --build build-cache-rtl -j4
ctest --test-dir build-cache-rtl --output-on-failure
python3 tools/rtl.py --build build-release --out artifacts/rtl-warp-frontend --module warp_frontend
python3 tools/rtl.py --build build-release --out artifacts/rtl-warp-frontend --module warp_frontend --synth
python3 tools/rtl.py --build build-release --out artifacts/rtl-warp-frontend --module warp_frontend --gate-sim
UBSAN_OPTIONS=halt_on_error=1 python3 tools/rtl.py --build build-sanitize \
  --out artifacts/rtl-warp-frontend-sanitize --module warp_frontend
python3 examples/hardware_host.py --build build-cache-rtl --tensor-rtl --cache-rtl \
  --scoreboard-rtl --register-rtl --collector-rtl --out artifacts/rtl-frontend-host
```

- 默认 Debug：27 套件，236 Python + 43 C++ = 279 项，277 通过、2 跳过，9.72 秒。联合 Release：51 套件，804 Python + 80 C++ = 884 项全通过，28.25 秒。后续仅补充枚举编码 static_assert 和注释，已重新编译联合构建。原始记录位于 `artifacts/rtl-frontend-final/{default,joint}-{ctest,tests}.log`。
- 前端四项独立场景通过 RTL、ASan/UBSan 和实际综合网表。包含 6,000 拍随机前后写/边界推进/选择/发射、三种 PC 策略、32 位 PC 回绕、完整等待地址与阶段、初始化旁路、部分有效 lane、连接令牌后的 EXIT 唤醒、同拍发射、YIELD 与复用/错误/复位。PC 选择参考使用有序容器，控制语义参考使用独立功能 ISA，不复用 RTL 归约树。
- 四项适配器检查验证观察副本不会改写 PC/令牌、无隐藏时钟、前后写口及发射顺序、96 位跨字字段、YIELD 在未发射时的恢复、物理槽复用和错误调用阶段。这四项另以 ASan/UBSan 直接编译适配器与新前端，全部通过；精确编译/执行命令、环境、源码和二进制哈希见 `artifacts/rtl-frontend-adapter-sanitize/manifest.json`，原始日志为同目录 `compile.log`、`tests.log`。没有把该局部 sanitizer 当作完整联合 sanitizer。
- 首轮 CMake/Verilator 配置因共享树数组上的 UNOPTFLAT 报警终止。改为独立生成节点连线，未屏蔽 lint；原始记录 `initial-selector-lint.log`。最终四份模块清单的源码哈希、实际 `gates.v` 哈希与适配器清单均匹配。
- Yosys/ABC 完整单 warp 组合前端为 103,263 generic cells / 4,801 FF 位，没有残留 memory/process。组选择子模块为 21,594 个组合单元。包括令牌子模块的门级行为检查通过，但不代表整 GPU 已连接或完成物理实现。独立 RTL 场景总数由 85 增为 89；本阶段运行新增四项，不重计历史模块为本次复跑。
- 五后端同时执行四类 Graph 各两次，加上监控规约两次，十份报告/trace 删除新增 `warp_frontends` 后与上一阶段完全一致；所有前端 issued 合计等于实际发射数。每次规约 768 个、小 Transformer 2,272 个外部 lane 写事件，向量/GEMM 为零。专用 16 令牌 Graph 及 333 个输出、错误 kernel 隔离和控制/YIELD 用例继续通过。
- 原版 nvidia-smi 经用户级 NVML 入口显示真实卡与活模拟器，模拟内存从 7 MiB 释放为 0 MiB，退出后移除；存活时的 `/usr/bin/nvidia-smi -L` 仍只列实卡。列表、默认表格、CSV 和原生列表保存到 `artifacts/rtl-frontend-final/`。没有新增 CUDA/PCIe 设备兼容。
- 最终审计清单冻结 280 个源码文件、三个联合二进制、四份模块证据、一份适配器 sanitizer 证据、一个实际综合网表和十份报告。完整联合 sanitizer、其他未修改电路的综合及完整 GPU 门级实现未在本阶段重新验证。
- 目标仍 active；跨 warp 发射仲裁、CTA/异步计数、SFU/其余访存电路、整机 RTL、参数辨识、PTX 异步矩阵协议与最终硬件审计未完成。

## 明确边界

- 功能模式和时序模式都在 CPU 上执行；不会利用 RTX 4070 SUPER 来获得模拟 GPU 的实际吞吐。
- 两个 SM 已有基础时序资源模型；缓存、Tensor、scoreboard、普通/谓词寄存器数据、collector/读请求年龄仲裁、七执行端口、整数/普通浮点/集合数值、重聚令牌/控制转换、warp PC/lane 存储与 PC 组选择、完成存储/写回仲裁 RTL 已接入实际执行，早期循环 bank 仲裁 RTL 仍独立，尚未连接成两份完整 RTL SM。仅报告实际模块的 generic-cell 统计，没有整卡门数、工艺频率、面积或功耗结论。
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


## 阶段 16：CTA 到达表、有限端口与硬件唤醒（2026-10-30）

从 `3823448` 接续，将实际 BAR_SYNC/BAR_ARRIVE/EXIT 的 CTA 到达状态移入 RTL，并为 warp 前端增加原始完成信号输入。每 CTA 有 16 条记录、最多 1,024 个线程；C++ 保留物理槽映射、跨分区提交次序和信号路由，异步事务屏障仍由软件推进。字段、计数不变量与接口边界见 [RTL_CTA_BARRIER.md](RTL_CTA_BARRIER.md)。

| 本轮实际运行 | 结果与范围 |
|---|---|
| `cmake --build build` / `ctest --test-dir build --output-on-failure` | 默认 Debug 27 套件/279 项运行：236 Python、43 C++；277 通过、2 项因未构建可选后端跳过，9.81 秒 |
| `cmake --build build-cache-rtl -j4` / `ctest --test-dir build-cache-rtl --output-on-failure` | 联合 Release 53 套件/897 项全部通过：812 Python、85 C++，29.14 秒；各套件按目的选择后端，并非全部检查同时打开五种 RTL |
| CTA 表独立 RTL / sanitizer / 网表 | 同四项全部通过：16 个编号、1/37/257/1,024 线程、代数复用、已到达 EXIT、全部 16 路完成、原子错误拒绝和 3,200 次随机事件 |
| Warp 前端独立 RTL / sanitizer / 网表 | 同五项全部通过：原四项加专用 CTA 阻塞/释放、同拍唤醒到发射及异步状态隔离 |
| 两个实际 C++ 适配器 | CTA 四项、前端五项在联合 Release 与独立 ASan/UBSan 中均通过；含两个 RTL 模型间的原始完成位传递、槽复用、无隐藏时钟和错误事务保留 |
| 新增 ISA/主机套件 | 八项通过，包括四分区端口等待、16 屏障、9 CTA/37 线程、1/2/4/8 分区、错误 kernel 后两次 Graph 重放，每次验证 333 个输出 |
| 实际综合 | 完整 CTA 表 234,669 generic cells / 18,999 FF 位；带 CTA 端口的完整 warp 前端 106,383 cells / 4,801 FF 位；`check -assert` 与实际网表执行通过 |
| 完整主机与监控 | `python3 examples/hardware_host.py --build build-cache-rtl --register-rtl --collector-rtl --scoreboard-rtl --cache-rtl --tensor-rtl --out artifacts/rtl-cta-host`：四负载各两次 Graph 重放；另两次监控规约，输出均通过独立参考 |

十份主机报告逐条对照实际退休 trace 与汇编，确认 CTA 命令数、启动次数和退出人数；下载字节与上一阶段相同。规约每次有 9 次端口等待、3 次屏障完成、480 个 lane 阻塞与唤醒；GEMM 为 9 次完成、288 个 lane；小 Transformer 为 33 次完成、2,112 个 lane，另保留 128 个外部异步等待字段写事件。向量没有屏障到达但 EXIT 经过表。本组总模型周期与前阶段一致；没有声称 trace 或所有调度行为完全相同。

首轮 53 套件中 52 通过；失败来自直接复用功能模拟的重复到达负例，它依赖连续选择同一 warp。多分区时序可能完成两代，使原程序合法。保留 `artifacts/rtl-cta-final/initial-joint-{ctest,tests}.log`，改用既有时序案例显式延迟另一 warp，仍断言同一重复到达错误和出错 PC。没有修改硬件语义或放宽错误断言。

sanitizer 工具的 `--module` 是单值选项，最初同时填写两次只运行了前端；随后单独执行 CTA 表并核验两份模块清单。没有将未运行的部分算作成功。最终受影响的九项独立电路均在普通 RTL、ASan/UBSan 与网表上复跑，其他独立模块未全部重新执行；完整联合 sanitizer 与整 GPU 综合也未在本轮执行。

原版 nvidia-smi 经已安装入口实测显示 RTX 4070 SUPER 和活模拟器，实际分配 7 MiB、释放归零、关闭后移除；直接系统二进制只见实卡，仍属于 NVML 管理接口。结果在 `artifacts/rtl-cta-final/{list,table,native-list}.txt` 与 `{allocated,freed,exited}.csv`。

八份模块清单、两个实际网表哈希、两适配器 sanitizer 清单、十份主机报告、290 个源码文件和四个联合二进制已核对，记录在 `artifacts/rtl-cta-final/evidence-audit.json`。同目录 `audit.py` 可复查，保留默认/联合完整日志；适配器精确编译命令在 `artifacts/rtl-cta-adapter-sanitize/manifest.json`。这轮完成 CTA 屏障局部电路，不代表整机 RTL、实卡校准或 PTX 异步矩阵协议已经完成。


## 阶段 16：同步 RAM 事务表、发布确认与异步阶段唤醒（2026-10-30）

从 `9cd618a` 接续，将异步事务的权威状态、串行地址查询、人数/字节算术、发布确认和 CTA 清理接入实际 RTL；前端新增原始阶段查询与完成总线。DMA 数据分片、shared bank 服务、等待地址分组和跨实例路由仍有 C++，详见 [RTL_TRANSACTION_TABLE.md](RTL_TRANSACTION_TABLE.md)。

| 本轮实际运行 | 结果与范围 |
|---|---|
| `cmake --build build -j4` / `ctest --test-dir build --output-on-failure` | 默认 Debug 27 套件/279 项运行，236 Python / 43 C++；277 通过、2 跳过，10.03 秒 |
| `cmake --build build-cache-rtl -j4` / `ctest --test-dir build-cache-rtl --output-on-failure` | 最终联合 Release 55 套件/930 项全部通过，840 Python / 90 C++，32.33 秒；各套件选择其目标后端 |
| 完整 1,024 条事务表 RTL 与 ASan/UBSan | 相同四项全部通过；覆盖全部物理槽、最高槽位、CTA 地址别名、发布/响应反压、回收原子性、2,400 条随机命令 |
| 事务算术 RTL / sanitizer / 实际网表 | 相同两项通过，含 20,000 组宽整数参考及 phase/count/byte 边界；3,192 组合 generic cells |
| 修改后的前端 RTL / sanitizer / 实际网表 | 相同六项通过，含新阶段等待、完整地址、等待类型隔离、同拍释放和未来令牌整查询原子拒绝；125,662 generic cells / 4,801 FF 位 |
| 完整表存储推断 | `--module transaction_table --synth --infer-only`：1,024×224 位单同步读/单写 RAM，229,376 数据位；RAM 外 1,410 FF 位，`check -assert` 通过 |
| 16 条小配置完整门映射与网表 | `--transaction-slots 16 --synth` 后单独 `--gate-sim`，四项通过；12,533 generic cells / 4,192 FF 位。不把此数当作完整表面积 |
| 两实际 C++ 适配器 | 事务表四项/前端六项在联合 Release 与独立 ASan/UBSan 中通过，含真实表→前端阶段总线与发布确认前不唤醒 |
| 新增 ISA/主机套件 | 28 项通过：25 项既有事务/DMA 场景，加全部记录/尾 warp/7 CTA 复用、容量/退出错误，以及失败后 Graph 重放 |

四类完整主机负载各两次 Graph 重放通过，另外运行两次带监控的 Transformer。十份报告将实际 `barrier_publish/copy_complete`、查询和已初始化 CTA 与硬件命令/发布/响应数对照，所有硬件表最终占用归零，无错误；前端外部等待字段写计数均为零。下载字节与上一阶段独立数学结果一致。

Transformer 每次为 8 条表命令、54 次搜索读、5 次发布、一次阶段完成，64 个 lane 检查/阻塞/唤醒。完整工作负载的两次 kernel 为 5,730/5,738 模型周期，上阶段 5,614/5,734；其他三个负载的周期保持相同。不宣称完整 trace 相同，也不将模型周期当作实卡性能。产物在 `artifacts/rtl-transaction-host/`。

监控会话实际完成 Transformer 图重放，再验证普通 `nvidia-smi` 中真实显卡与模拟器共存、7 MiB 分配、释放归零和退出移除；直接 `/usr/bin/nvidia-smi` 只显示真实设备。证据保存在 `artifacts/rtl-transaction-final/{list,table,native-list}.txt` 和 `{allocated,freed,exited}.csv`，这仍是 NVML 管理接口接入。

最初新增适配器测试因修改观察副本后没有读取该局部变量，被 `-Werror=unused-but-set-variable` 拒绝编译。保留 `initial-adapter-build-failure.log`，补充副本与实际输出不相等的断言后通过，没有关闭告警。后续前端检查加强为任一未来令牌使整个地址查询在 RTL 内拒绝，普通仿真、sanitizer、综合和网表均使用最终版本重新运行。

本轮共复跑受影响的 12 项独立电路场景，其余独立模块未全部重跑；完整表没有做 1,024 条通用门展开/网表仿真，完整联合 sanitizer 与整 GPU 综合也未执行。13 份模块清单、三份实际网表哈希、完整 RAM 几何、两个适配器 sanitizer 结果、十份主机报告、301 个源码文件与四个联合二进制审计见 `artifacts/rtl-transaction-final/evidence-audit.json`；同目录保留 `audit.py`、全量 CTest 日志和适配器构建脚本。

最后只修正异步退休函数的英中注释，使其描述已经验证的 RTL 等待顺序，不改变执行代码。重新编译后四个已验收可执行文件的 SHA-256 完全相同，记录在 `comment-rebuild.json`，因此没有重复运行同一批测试。整个硬件目标仍未完成：DMA 片段与返回、跨 warp/跨分区仲裁、SFU/访存电路、整机 RTL、实机参数辨识和 PTX 异步矩阵协议继续按总计划推进。

## 阶段 16：完整 DMA 命令/片段池与返回载荷电路（2026-10-30）

从 `680a038` 接续，把 DMA 的权威描述符、游标、分片、地址/掩码、请求保持、返回载荷、未完成数和完成资格接入 RTL。完整物理几何始终为每 SM 64 命令槽与 64 片段槽；运行额度保持 1～64，不使用小配置冒充完整验收。外部内存票据选择、shared 服务和互联仍有 C++，详细边界见 [RTL_DMA.md](RTL_DMA.md)。

| 实际命令/范围 | 结果 |
|---|---|
| `cmake --build build -j4` / `ctest --test-dir build --output-on-failure` | 默认 Debug 27 套件、279 项运行：236 Python / 43 C++，277 通过、2 跳过；9.86 秒 |
| `cmake --build build-cache-rtl -j4` / `ctest --test-dir build-cache-rtl --output-on-failure` | 联合 Release 56 套件、936 项全部通过：842 Python / 94 C++；32.77 秒 |
| `python3 tools/rtl.py --build build-release --module dma_engine --out artifacts/rtl-dma-final` | 完整 64/64 RTL 六项通过：满槽/最高槽、逆序响应、反压保持、尾部/chunk/地址进位、错误/复位、360 条随机描述符及长命令公平性 |
| 同模块 `--build build-sanitize --out artifacts/rtl-dma-sanitize` | 同六项 ASan/UBSan 通过，无地址或未定义行为报告 |
| 同模块 `--synth --out artifacts/rtl-dma-final` | 完整 64/64 配置 `check -assert` 通过；400,338 generic cells / 87,547 FF 位，无推断 SRAM。包含 64×1,024 位无复位载荷触发器 |
| 同模块 `--gate-sim --out artifacts/rtl-dma-final` | 使用实际综合网表和同一逐字节参考，六项全部通过；透明包装及 `split_var` 解决旧工具的总线分析问题，没有屏蔽环路告警 |
| `python3 artifacts/rtl-dma-final/build_adapter.py` | 实际 DMA 适配器和完整电路四项独立 ASan/UBSan 通过，覆盖显式时钟、观察副本、原始返回位、33 位地址、事件数、独立实例、容量和拒绝后恢复 |
| `rtl_transaction_execution` | 扩展为 30 项；每个成功 kernel 对照 DMA 硬件容量、时钟、请求/返回/确认字节/释放及占用归零。新增实际访存占满 64 片段，以及活查询和在途 DMA 失败后重新执行 |

首轮适配器/既有内存异步/事务执行的三个目标套件也已通过，随后补充容量和失败恢复测试，再运行上表的完整回归。新增四项 C++ 适配器与两项 Python 检查使联合构建从 55 套件/930 项增至 56 套件/936 项；六项独立 RTL 场景另计，不把 RTL、sanitizer 与网表复跑重复算作新场景。

五种后端同时启用运行向量、规约、GEMM、Transformer，各两次 Graph；另外运行两次监控 Transformer。十份 kernel 报告逐份核对真实后端、DMA 事件与动态 trace、最终占用和下载字节。与 `rtl-transaction-host` 对照，全部下载内容相同。Transformer 每次分配 17 个片段，读写确认各 2176 B，kernel 周期从 5730/5738 变为 5735/5741；其他三个负载周期保持相同。没有宣称完整 trace 相同或真实 GPU 性能变化。

监控会话完成 Transformer 后，普通 `nvidia-smi` 实际显示真实 4070 SUPER 与模拟设备；7 MiB 分配、释放至零、退出移除均通过。直接 `/usr/bin/nvidia-smi` 始终只列实卡。证据在 `artifacts/rtl-dma-final/{list,table,native-list}.txt` 与 `{allocated,freed,exited}.csv`，仍属于 NVML 管理面接入。

本轮保留的失败与修复：

- 初始 RTL lint 报八个宽度告警，改为显式位宽转换后严格 lint 通过；日志 `initial-lint.log`。
- 首轮独立驱动把旧 Verilator 的原生 C 数组当作有 `.begin()` 的容器；改为 `std::begin` 后通过，原日志 `rtl-run.log`。
- 初始适配器 sanitizer 构建把项目的 `-Werror` 同时用于旧版厂商 runtime，触发其自身 `sign-compare`。随后单独编译厂商 runtime，仍对项目适配器和测试启用 `-Wall -Wextra -Werror`，全部代码仍接受 sanitizer 插桩；原日志 `adapter-sanitize-run.log`。
- 第一次网表编译把共享地址位生成其他 lane 进位的逻辑视作宽输出总线反馈，报 UNOPT/UNOPTFLAT。直接对顶层 public 端口添加 `split_var` 又被 SPLITVAR 拒绝；日志 `gate-run.log`、`gate-split-lint.log`。最终工具生成仅直接连线的顶层包装，让实际网表的地址总线作为内部信号分位分析，严格 lint 与全部六项行为通过。原始 `gates.v` 未改动，未降低容量或关闭这些告警。

`artifacts/rtl-dma-final/audit.py` 核对四份独立工具清单、完整几何、实际网表和透明包装哈希、一份适配器 sanitizer 清单、十份主机报告、311 个源码文件和三个联合二进制；结果保存在 `evidence-audit.json`。本轮仅复跑新增 DMA 的六项独立场景；其余独立模块、完整联合 sanitizer 和整 GPU 综合未重跑。跨 warp/跨分区仲裁、SFU/剩余访存、外部内存返回选择、整机 RTL、实卡校准和 PTX 异步矩阵协议仍未完成，目标保持 active。

## 阶段 16：跨 warp 候选端口、在途计数和串行化 RTL（2026-10-30）

从 `74d77dd` 接续。collector 后端现在每分区实例化完整 32 槽调度器，由 RTL 保存轮转、重试、live、在途数和边界/访存状态。每拍最多检查一个候选，实际退休进入同拍旁路，CTA 资源等释放边沿后归还。取指译码、依赖查询组合与跨分区共享资源仲裁仍有 C++，详细字段、时钟与策略边界见 [RTL_WARP_SCHEDULER.md](RTL_WARP_SCHEDULER.md)。

| 实际命令/范围 | 最终结果 |
|---|---|
| `cmake --build build -j4` / `ctest --test-dir build --output-on-failure` | 默认 Debug 27 套件、279 项运行：236 Python / 43 C++；277 通过、2 跳过，9.85 秒 |
| `cmake --build build-cache-rtl -j4` / `ctest --test-dir build-cache-rtl --output-on-failure` | 联合 Release 58 套件、945 项全部通过：847 Python / 98 C++，35.21 秒；各套件按目的选择后端 |
| `python3 tools/rtl.py --build build-release --module warp_scheduler --out artifacts/rtl-scheduler-final` | 完整 32 槽五项通过：容量、三种策略与反压、串行化/退休旁路、错误原子性/复用，以及 12,000 拍随机参考 |
| 同模块 `--build build-sanitize --out artifacts/rtl-scheduler-sanitize` | 相同五项 ASan/UBSan 通过，包括最高槽 31 和全部槽分别 32 个在途计数 |
| 同模块 `--synth --out artifacts/rtl-scheduler-final` | 完整容量 `check -assert` 通过，7,625 generic cells / 325 FF 位，32 份槽状态子模块，无 SRAM |
| 同模块 `--build build-release --gate-sim --out artifacts/rtl-scheduler-final` | 最终实际综合网表通过同五项行为检查，不使用缩小容量或修改后的网表 |
| `python3 artifacts/rtl-scheduler-final/build_adapter.py` | 实际适配器和完整 RTL 的四项独立 ASan/UBSan 通过：显式时钟、观察副本、事件阶段、独立实例和拒绝恢复 |
| `rtl_warp_scheduler_execution` | 新增五项检查通过，覆盖 32 warp、1/2/4/8 分区、被阻塞的最高优先级 warp、访存/FENCE 顺序、7 CTA/37 线程槽复用，以及主机查询和错误恢复 |

默认回归之后的最后两处修复仅涉及可选 RTL：逐槽计数连线，以及适配器在决定后拒绝生命周期事件。最终联合回归、独立 RTL/sanitizer、实际综合/网表和适配器 sanitizer 均包含这些修复。没有把默认构建的两个跳过项计作通过，也没有把同一场景的多种构建重复计为新检查。

五后端同时启用执行向量、规约、GEMM、Transformer，各两次 Graph 重放；另外两次监控 Transformer。实际命令为：

```sh
python3 examples/hardware_host.py --build build-cache-rtl \
  --register-rtl --collector-rtl --scoreboard-rtl --cache-rtl --tensor-rtl \
  --out artifacts/rtl-scheduler-host
python3 artifacts/rtl-scheduler-final/check_monitor.py
python3 artifacts/rtl-scheduler-final/audit.py
```

十份报告逐一检查后端身份、硬件容量、时钟、每拍最多一次候选、发射/退休与 trace 一致、分配/释放一致及最终占用归零。实际写回容量限制每分区总在途数不超过 32；独立电路满计数测试的 1,024 总和仅用于验证位宽。下载内容与前一 DMA 阶段逐字节相同。

| 两次 Graph kernel 的模型周期 | 前一 DMA 阶段 | 当前调度器 |
|---|---:|---:|
| vector_add | 4831 / 4850 | 4832 / 4850 |
| reduction | 2008 / 2240 | 2021 / 2240 |
| gemm | 3867 / 3416 | 3868 / 3416 |
| tiny_transformer | 5735 / 5741 | 5736 / 5741 |

变化来自有限候选机会和 CTA 释放阶段。停顿计数按实际硬件事件汇总，不沿用软件多候选扫描的计数口径；未宣称逐拍轨迹相同或实卡性能变化。

监控会话完成实际 Transformer 后，已安装的普通 `nvidia-smi` 入口显示 RTX 4070 SUPER 和活模拟器；7 MiB 分配、释放到零、退出移除均通过。直接 `/usr/bin/nvidia-smi -L` 仍只显示实卡。列表、表格与 CSV 位于 `artifacts/rtl-scheduler-final/`，此功能仍属于 NVML 管理面，没有增加 CUDA/PCIe 兼容。

保留的失败与修复：

- `initial-lint.log`：候选扫描临时整数的高位未使用；改为明确五位槽号和位宽转换，没有屏蔽告警。
- `rtl-run.log`、`rtl-run-2.log`：测试最初缺少 `<optional>`，随后同一 `auto` 声明混用不同 Verilator 输出类型；分别补齐头文件和显式整数类型。
- `sanitize-run.log`、`adapter-sanitize-run.log`：旧版 Verilator 对最高槽的动态 packed 切片生成了越界 C++ 访问。改为逐槽六位连线数组，并静态连接观察总线；随后重新执行 RTL、sanitizer、综合、网表、适配器和联合回归。最终单元数为 7,625，早期未修复版本的综合数字不作为验收结果。
- 人工复查补充了适配器阶段约束：决定后禁止新的 probe 或分配/释放/退休，避免先前接受结果被后续引脚变化撤销。负例已加入实际适配器检查；这项修复来自审查，没有虚构失败日志。

审计脚本核对四份模块清单、完整容量与实际网表、一份适配器 sanitizer 清单、十份主机报告、320 个源码文件及三个联合二进制，结果保存在 `evidence-audit.json`。最终网表 SHA-256 为 `52bdd1e149078421899e565f42fbce11d6fc6028e276435b2812af2be4aea6c3`。本阶段只复跑新增调度电路的五项独立场景，未重跑全部历史独立模块、完整联合 sanitizer 或整 GPU 综合。

完整目标保持 active：取指译码、依赖查询组合、跨分区仲裁、SFU/剩余访存、外部内存返回选择、整机 RTL、实卡参数辨识、PTX 异步矩阵协议与最终硬件审计仍须继续。

## 阶段 16：跨分区执行轮转、Tensor 归属与发射间隔 RTL（2026-10-30）

从 `de300e3` 接续。每 SM 新增完整八分区、七类候选端口的执行仲裁器，实际 collector 输出提供请求，实际接收提供确认。Tensor/异步候选轮转、Tensor 持有到退休及三类算术间隔移入电路；异步 busy 输入、后台完成优先服务与 CTA 回收编排仍有 C++。细节见 [RTL_EXECUTION_ARBITER.md](RTL_EXECUTION_ARBITER.md)。

| 实际命令/范围 | 最终结果 |
|---|---|
| `cmake --build build -j4` / `ctest --test-dir build --output-on-failure` | 默认 Debug 27 套件、279 项运行：236 Python / 43 C++；277 通过、2 跳过，9.92 秒 |
| `cmake --build build-cache-rtl -j4` / `ctest --test-dir build-cache-rtl --output-on-failure` | 联合 Release 60 套件、954 项全部通过：852 Python / 102 C++，36.46 秒；各套件选择目标后端 |
| `python3 tools/rtl.py --build build-release --module execution_arbiter --out artifacts/rtl-execution-final` | 完整八分区五项通过：私有端口与 32 位间隔、共享轮转/下游拒绝、异步空操作、错误原子性/复位及 12,000 拍随机并发 |
| 同模块 `--build build-sanitize --out artifacts/rtl-execution-sanitize` | 同五项 ASan/UBSan 通过，包括最高分区、完整 56 位矩阵和 UINT32_MAX 间隔 |
| 同模块 `--synth --out artifacts/rtl-execution-final` | 完整容量 `check -assert` 通过，6,360 generic cells / 778 FF 位，无 SRAM |
| 同模块 `--build build-release --gate-sim --out artifacts/rtl-execution-final` | 使用实际完整综合网表复跑同五项，全部通过 |
| `python3 artifacts/rtl-execution-final/build_adapter.py` | 实际适配器与完整电路四项独立 ASan/UBSan 通过：快照/时钟、归属交接、间隔/空操作/拒绝及输入阶段与独立实例 |
| `rtl_execution_arbitration` | 五项执行/主机检查通过：1/2/4/8 分区的 3/5/7 发射间隔、八分区 Tensor 互斥、单命令 DMA 池、异步空操作及活 Tensor 归属观察和失败恢复 |

独立随机参考使用绝对截止时间与两个独立优先队列，按原始握手更新，未复用硬件计数器算法。实际 Tensor 互斥用 execute/retire trace 验证持有区间没有重叠；全八分区每个 warp 均完成分配、三次 TMEM 读取和释放。单命令/单片段 DMA 测试验证八份实际载荷，所有分区的 40 条异步指令完成，八条拷贝命令均归还额度。

初始完整回归 60 套件中 57 通过，保留 `initial-joint-{ctest,tests}.log`。既有两个异步套件发现 `command_stalls` 为零：请求改在 collector 准入处过滤后，不再经过旧软件发射函数的计数点。修复为统计实际已就绪且被 DMA 容量阻塞的 collector 条目，保留原有“必须发生容量停顿”的断言。共享控制忙计数由 RTL 事件提供，相关统计口径在结构文档中说明。

另外，新测试最初向 MOVI 传入无效源寄存器字段，并调用主机接口未支持的 `max_cycles` 参数。修正为合法 ISA 操作数，并使用第二个 warp 的循环耗尽 `max_steps`，在真实 Tensor 持有期间触发故障。报告代码随后因误把 SM 索引当对象而编译失败，记录在 `joint-build-2.log`，修正实际实例访问后编译通过。三个受影响套件先单独通过，再执行上表完整最终回归。未关闭告警或放宽功能断言。

五后端同时运行四类主机负载各两次 Graph，另运行两次监控 Transformer：

```sh
python3 examples/hardware_host.py --build build-cache-rtl \
  --register-rtl --collector-rtl --scoreboard-rtl --cache-rtl --tensor-rtl \
  --out artifacts/rtl-execution-host
python3 artifacts/rtl-execution-final/check_monitor.py
python3 artifacts/rtl-execution-final/audit.py
```

十份下载结果与前一调度器阶段逐字节相同；这组负载的 kernel 模型周期也相同：向量 4832/4850、规约 2021/2240、GEMM 3868/3416、Transformer 5736/5741。每次实际 execute 数分别为 150、735、204、1470，与对应 SM 的仲裁接受数一致。审计还按汇编 PC 和 opcode 分类逐一核对七类接受计数、各类拒绝恒等式、Tensor 持有区间与释放次数；未宣称完整 trace 或停顿统计相同。

普通 `nvidia-smi` 经已安装 NVML 入口再次验证真实卡和模拟器共存、7 MiB 分配、释放归零与退出移除；直接 `/usr/bin/nvidia-smi -L` 只显示真实卡。输出在 `artifacts/rtl-execution-final/{list,table,native-list}.txt` 和 `{allocated,freed,exited}.csv`，未增加 CUDA/PCIe 兼容。

最终审计包含四份模块清单、完整综合网表、一份实际适配器 sanitizer 清单、十份主机报告、329 个源码文件和三个联合二进制。网表 SHA-256 为 `d01708f6be84aae9a611700d17034612bf0dfe46576ec4b100c6d602a129473c`，完整清单在 `artifacts/rtl-execution-final/evidence-audit.json`。本阶段复跑新增电路的五项独立场景，未重跑全部历史独立模块、完整联合 sanitizer 或整 GPU 综合。

完整目标仍 active；取指译码/依赖查询组合、异步后台与 CTA 提交/回收服务次序、SFU/剩余访存电路、外部内存返回选择、整机 RTL、实卡校准、PTX 异步矩阵协议和最终硬件审计仍未完成。


## 阶段 16：原始机器码与操作数译码 RTL（2026-10-30）

从 `38139d7` 接续。每分区新增组合译码器，实际执行字段来自原始 128 位映像的 RTL 输出。规范生成字段/合法位电路，手写操作数电路产生三个有界源请求、重复源合并、R63 消除、跨 lane 掩码与结构错误。PC 越界、指令存储、部分静态目标验证、依赖查询组合与执行专用检查仍有 C++，见 [RTL_INSTRUCTION_DECODER.md](RTL_INSTRUCTION_DECODER.md)。

| 实际命令/范围 | 最终结果 |
|---|---|
| `cmake --build build -j4` / `ctest --test-dir build --output-on-failure` | 默认 Debug 27 套件、279 项运行：236 Python / 43 C++，277 通过、2 跳过，10.23 秒 |
| `cmake --build build-cache-rtl -j4` / `ctest --test-dir build-cache-rtl --output-on-failure` | 联合 Release 63 套件、966 项全部通过：857 Python / 109 C++，38.01 秒 |
| `python3 tools/rtl.py --build build-release --module instruction_decoder --out artifacts/rtl-decoder-final` | 五项独立电路检查通过，含全部 59 个 opcode、197 个未知 opcode、保留位扫描与 70,800 组随机字段/footprint 差分 |
| 同模块 `--build build-sanitize --out artifacts/rtl-decoder-sanitize` | 同五项 ASan/UBSan 通过；参考库来自本轮重新构建的默认 sanitizer |
| 同模块 `--build build-release --synth --out artifacts/rtl-decoder-final` | `check -assert` 通过，完整 1,941 generic cells、零 FF、零运行时存储；中间 64×114 位常量 ROM 已映射为组合逻辑 |
| 同模块 `--build build-release --gate-sim --out artifacts/rtl-decoder-final` | 实际综合网表复跑同五项，全部通过 |
| `python3 artifacts/rtl-decoder-final/build_adapter.py` | 实际适配器与完整组合电路四项独立 ASan/UBSan 通过 |
| `rtl_instruction_decoder_path` / `rtl_instruction_decoding` | 三项 C++ 原始映像权威性检查、五项 Python 实际 ISA/主机检查通过；包含 1/2/4/8 分区、尾 warp、多 CTA、分歧、错误与 Graph 两次重放 |

适配器与实际路径的七项 C++ 已计入联合 109 项，不将 sanitizer 复跑再算作新场景。五项独立电路场景另计。本轮没有执行完整联合 sanitizer 或整 GPU 综合；默认 Release/sanitizer 构建用于生成文件和独立参考库，没有据此宣称它们的全量 CTest 已运行。

原始路径检查故意把宿主预解码的 MOVI 改成 FADD、改变 guard/目标/源/立即数及 STG 的源字段，保持原始映像时执行结果和完整报告仍相同。改变原始立即数后，真正写入的内存随之改变。额外直接修改原始未知 opcode/保留位并清空映像，验证没有回退到宿主预解码。这些负例没有改动实际生产执行来通过测试。

首次完整回归为 62/63 套件通过，失败集中于新增 Python 套件的两处预期，日志保留为 `initial-joint-{ctest,tests}.log`：失败 kernel 后在同一 stream 重放会正确报告 dependency_failed，因此改到独立 stream；空掩码的非法普通源虽不在译码阶段读取，下游整数描述符仍拒绝，因此保留该错误并另测合法源的空掩码。随后单套件验证暴露 CLI 错误报告没有 hardware 字段，移除不成立的字段访问；译码错误计数仍由独立适配器和持久主机测试检查，日志在 `targeted-failed-tests.log`。最终完整回归全部通过，没有改变原有失败传播或描述符校验。

五后端联合运行四类完整主机负载各两次 Graph，另运行两次监控 Transformer：

```sh
python3 examples/hardware_host.py --build build-cache-rtl \
  --register-rtl --collector-rtl --scoreboard-rtl --cache-rtl --tensor-rtl \
  --out artifacts/rtl-decoder-host
python3 artifacts/rtl-decoder-final/check_monitor.py
python3 artifacts/rtl-decoder-final/audit.py
```

十份报告的译码计数与调度 probes/guard stalls/issued 逐一匹配，下载数据与上阶段逐字节相同；去掉新增 instruction_decoders 字段后，完整 hardware 报告（含 trace）也相同。四类 kernel 模型周期为向量 4832/4850、规约 2021/2240、GEMM 3868/3416、Transformer 5736/5741。审计最初误把独立监控会话与已运行其他负载的持久会话比较，因缓存历史不同失败；改为同场景的上阶段监控会话后完整比较通过，保留 `initial-audit-run.log`。没有忽略差异或用同周期代替完整比较。

原版 nvidia-smi 经已安装的用户级 NVML 入口实际显示 RTX 4070 SUPER 与活模拟器，内存从 7 MiB 到释放后的 0 MiB，再随会话退出移除。直接 `/usr/bin/nvidia-smi -L` 仍只显示实卡。这是管理面可见性，不是 PCIe 或 CUDA 设备枚举；证据在本阶段 `list.txt`、`table.txt`、`native-list.txt` 与三份 CSV。

审计核对四份模块清单、生成 RTL 和驱动源码哈希、综合网表、一份适配器 sanitizer 清单、十份主机报告、343 个源码/模板/规范文件、四个构建的生成译码文件及四个联合二进制。实际网表 SHA-256 为 `407e82041949b33c1e9d6b37e6032163ce662379c439b7d0cd8d51eef4407018`；完整映射见 `artifacts/rtl-decoder-final/evidence-audit.json`。

完整目标保持 active。指令存储/取指与依赖查询直连、异步后台/CTA 服务次序、SFU/剩余访存、外部返回选择、整机 RTL、实卡校准、PTX 异步矩阵协议和最终硬件审计仍未完成。


## 阶段 16：三源、目标与谓词的并行依赖查询（2026-10-30）

从 `94fd4fb` 接续。原 scoreboard 的状态和阶段旁路已是 RTL，但实际指令仍在 C++ 中逐个调用 readable/writable/predicate_ready 并短路合并。本阶段将这部分改为固定三源、普通目标、两个谓词端口的组合电路，直接连接原同拍旁路状态；实际发射读取 RTL ready。guard 继续先通过独立谓词查询，实例间路由仍有 C++。结构及状态归属见 [RTL_DEPENDENCIES.md](RTL_DEPENDENCIES.md)。

| 实际命令/范围 | 最终结果 |
|---|---|
| `cmake --build build -j4` / `ctest --test-dir build --output-on-failure` | 默认 Debug 27 套件、279 项运行：236 Python / 43 C++，277 通过、2 跳过，10.26 秒 |
| `cmake -S . -B build-cache-rtl` / `cmake --build build-cache-rtl -j4` / `ctest --test-dir build-cache-rtl --output-on-failure` | 保留原联合选项；联合 Release 64 套件、974 项全部通过：862 Python / 112 C++，38.58 秒 |
| `python3 tools/rtl.py --build build-release --module dependency_query --out artifacts/rtl-dependencies-final` | 三项独立电路检查通过：全部 128 种冲突组合、常量/空 mask/不相交 lane、16,000 组随机完整状态图 |
| 同模块 `--synth` / `--gate-sim` | 完整 11,061 个组合单元、零 FF、无 SRAM；实际综合网表同三项通过 |
| `python3 tools/rtl.py --build build-release --module warp_scoreboard --out artifacts/rtl-dependencies-final` | 完整 32 lane、63 普通寄存器、8 谓词五项检查通过，包含原有 6,400 事件差分与新增多端口/同拍旁路/撤回/RMW 分类 |
| 同模块 `--synth` / `--gate-sim` | 完整 764,130 generic cells / 14,368 FF 位，无 SRAM，check -assert 和实际综合网表同五项通过 |
| 两模块分别 `--build build-sanitize --out artifacts/rtl-dependencies-sanitize` | 三项查询/五项完整 scoreboard 的 ASan/UBSan 全部通过 |
| `python3 artifacts/rtl-dependencies-final/build_adapter.py` | 实际适配器、完整 scoreboard 与 C++ 参考七项独立 ASan/UBSan 通过 |
| `rtl_parallel_dependencies` | 五项实际 ISA/主机检查通过：第三源/最高 lane/多 CTA、WAW 与未收集读者 WAR、谓词目标/guard、合法及非法 SHFL 成员、失败后 Graph 重放与查询不推进状态 |

独立模块共八项包含五项完整 scoreboard 场景的复跑，不称为新增八个不同场景。适配器七项已计入联合 C++ 数量，sanitizer 复跑不重复计数。默认 Release 与 sanitizer 参考库本轮重新构建，但没有运行它们的全量 CTest，也没有运行完整联合 sanitizer 或整 GPU 综合。

首个增量构建先尝试旧 Ninja/Verilator 规则，未找到新 dependency_query 源文件，记录在 initial-build.log；显式重新配置使新源列表生效，之后联合构建通过。首轮完整回归 63/64 套件通过，新增 SHFL 测试因指定成员未全部参与动态指令而正确返回 membermask_not_subset。按既有 ISA 改为合法全 warp 跨 lane 读取，并将原程序保留为显式负例；没有放宽硬件执行校验。首次结果保留在 initial-joint-{ctest,tests}.log，最终结果在 joint-{ctest,tests}.log。

两套电路清单均匹配当前 RTL 和测试驱动，综合网表没有使用小容量替代。完整 scoreboard 的触发器位数未变；报告中的查询次数/分类计数是宿主接口观测，不能算成新增硬件状态。原单查询接口同时改为选取旁路掩码后归约，故独立查询模块的 11,061 单元不能直接当作完整 scoreboard 相对旧版本的面积增量。

五后端共同执行四类主机负载各两次 Graph，另运行两次监控 Transformer：

```sh
python3 examples/hardware_host.py --build build-cache-rtl \
  --register-rtl --collector-rtl --scoreboard-rtl --cache-rtl --tensor-rtl \
  --out artifacts/rtl-dependencies-host
python3 artifacts/rtl-dependencies-final/check_monitor.py
python3 artifacts/rtl-dependencies-final/audit.py
```

十份真实报告中，全部 scoreboard 的 dependency_queries 总和逐项等于译码器 operands 总和，分类冲突不超过实际被阻塞查询数。下载与上一译码阶段逐字节相同；删除新增的八个 scoreboard 查询观测字段后，完整 hardware/trace 也相同。模型周期仍为向量 4832/4850、规约 2021/2240、GEMM 3868/3416、Transformer 5736/5741。

普通 nvidia-smi 经已安装用户入口再验证 RTX 4070 SUPER 与活模拟器共存、7 MiB 分配、释放为零和退出移除；直接系统绝对路径仍只见实卡。此证据仍仅对应 NVML 管理面，未新增 CUDA/PCIe 计算设备兼容。

审计核对八份模块清单、两份实际综合网表、一份适配器 sanitizer 清单、十份报告、346 个源码/模板/规范文件以及三个联合二进制，保存在 artifacts/rtl-dependencies-final/evidence-audit.json。查询网表 SHA-256 为 `057a1bc94438adeb2ed42406dc4d5faaa87705169861282c4708469d9628fe64`，完整 scoreboard 为 `da8798a04e0bf1e6245cf3463c682b350ac73688fbaa21acf1ff66848bfa5114`。

完整目标继续 active。指令存储/取指、前端顶层连接、异步后台/CTA 服务次序、SFU/剩余访存、外部返回选择、整机 RTL、实卡参数校准、PTX 异步矩阵协议和最终硬件审计仍未完成。
