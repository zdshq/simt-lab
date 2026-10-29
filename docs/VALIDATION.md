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
