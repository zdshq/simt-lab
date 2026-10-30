# 实际执行中的寄存器数据阵列

`SIMT_REGISTER_RTL=ON` 构建、`--timing --register-rtl` 或 Python `Device(timing=True, register_rtl=True)` 将普通寄存器数据接入 SystemVerilog 电路。指令源操作数来自电路的实际读取端口；C++ 架构状态中的寄存器副本只用于保存退休结果和转储，不再作为这个后端的操作数来源。

这仍是 CPU 上的混合仿真，不是完整 RTL SM，也不是 NVIDIA 寄存器布局的逆向复刻。单独启用寄存器后端时，操作数收集器和请求排序仍由 C++ 实现；另加 `--collector-rtl` 可启用 [直连收集器和年龄优先读仲裁](RTL_COLLECTOR.md)。独立 `register_arbiter.sv` 的循环优先级不是这个年龄优先策略，仍未接入。

## 容量、地址与端口

每个 SM 物理提供 65,536 个 32 位字，即 256 KiB。按每 warp 64 个寄存器、每寄存器 32 个 lane 分为 32 个物理槽。支持 1、2、4、8 个 partition，各 partition 分别获得 32、16、8、4 个槽；不会因分区数增加而复制整份 SM 存储。

| 项目 | 实际结构 |
|---|---|
| 物理 bank | 每 partition 4 个，`bank = reg % 4` |
| bank 行 | `row = partition_local_slot * 16 + reg / 4` |
| 每行数据 | 32 个 lane，每个 lane 为 32 位；一个向量共 1,024 位 |
| 默认端口 | 每 bank 一个组合读端口、一个时钟边沿写端口 |
| 写使能 | 32 位 lane mask，不参与的 lane 保留旧值 |
| 初始化状态 | 每行每 lane 一个 valid 位，清除 valid 后读取返回零 |
| R63 | 返回零，丢弃写入，不占用端口；RTL 也屏蔽对应存储行 |
| 写回旁路 | 本拍同地址的有效写 lane 直接送往读端口，其余 lane 取原值 |

默认四个端口组分别对应四个 bank。`--register-banks 2` 使 bank 0/2 共用一个端口组、bank 1/3 共用另一个；`--register-banks 1` 使四个存储 bank 共用一个组。组合授权逻辑根据已经保留的端口检查冲突，读写方向独立。底层仍有四个数据 bank，低带宽配置没有改变数据布局，也没有由软件补读额外端口。

大于 65,536 的 SM 寄存器预算、不支持的端口组数或非 2 的幂分区数会明确拒绝。较小的 `--sm-registers` 只限制准入预算，不缩小已综合的数据阵列；`hardware.register_files` 报告实际物理容量。物理槽分配同时受寄存器预算和 `max_warps` 限制。

## 数据存储、清零和时钟

`rtl/register_bank.sv` 将每个 lane 写成独立的 32 位宽存储数组，每数组一个读地址、一个写地址。数据位没有复位赋值；有效位阻止未初始化的数据传播。这个描述具有异步读语义，尚未映射到具体 SRAM 宏、FPGA BRAM 或 DSP，不能把工具识别出的 memory cell 当成已完成的物理实现。

CTA 准入时通过 `clear_slots` 使新分配物理槽的所有有效位失效。组合读取立即返回零，下一有效边沿提交失效；同拍若有写入，只有参与的 lane 重新有效。其他槽不受影响。回收再分配使用同一份电路存储，不为历史 CTA 无限制创建寄存器堆。C++ 保留历史 CTA 的结果用于观测，不能从这份历史结果读操作数。

适配器设置端口引脚、读取组合返回值；这些方法不产生额外时钟。每次整机推进在写回、操作数收集和发射结束后对各 partition 执行一次 `tick()`。写回先于收集，因而同拍写回通过 RTL 旁路被后续收集读取，与原研究流水线一致。适配器明确拒绝在已经收集操作数后继续写入；不会返回一个旧向量，再偷偷改动同拍写回使先前读取失效。

存储时钟门为 `reset || |clear_slots || |write_valid`，输入只在时钟低电平更新。只读和空闲周期没有数据状态变化。构造时复位属于设备建立，不计入 kernel 周期。接口预约由适配器保持到周期结束，不能把同一周期内多次方法调用当成额外 bank 带宽。这一时钟门尚未映射到工艺 ICG 或 FPGA 专用时钟网络。

报告同时记录四个物理 bank 和实际端口组数，分别记录输入周期 `clocks`、实际有效存储边沿 `active_clocks`、被接受的向量读写数和清除的槽数。向量读写计数从真实端口 valid 引脚采样，不按指令数推算。普通状态和 NVML 查询不推进电路。故障后私有寄存器电路停止，存储请求可能继续排空，因而故障快照中的设备时间可以继续增长。

接入谓词存储后，`register_files.active_clocks` 仍统计普通寄存器写入或清槽的有效更新周期；仅谓词写入不增加该项，单独记入 `predicate_files.active_clocks`。组合模块使用共享时钟，因此这些是更新使能计数，不表示独立时钟网络在硅上实际关闭，也不是功耗测量。

## 执行接入

| 路径 | 实现 |
|---|---|
| 工厂与能力检查 | `include/simt/register_file.hpp`、`src/hardware/register_file.cpp` |
| 引脚/时钟适配 | `src/rtl/register_file.cpp`，不保存第二份寄存器数据阵列 |
| 物理数据与有效位 | `rtl/register_file.sv`、`rtl/register_bank.sv` |
| 操作数传送 | `src/timing/registers.cpp` 从电路输出复制被选 lane 到当前 C++ collector |
| 写回 | 标量、load、atomic、TMEM、异步返回、集合指令全部通过同一数据端口 |
| 生命周期 | `src/timing/scheduler.cpp` 分配槽时清有效位，每模型拍统一提交 |

谓词值现已由同一寄存器后端接入 [谓词存储 RTL](RTL_PREDICATES.md)，guard/VOTE 从实际读多路器取值；本页早期综合统计保持其当时范围。PC、控制流状态和部分执行锁存仍在 C++。控制类集合指令的结果经寄存器写端口或直连写回端口提交；它们的普通寄存器输入同样来自已收集的 RTL 输出。可同时启用 scoreboard、缓存和 Tensor RTL，各单元保持自己的明确时钟归属。

## 复现

```sh
cmake -S . -B build-register -G Ninja -DCMAKE_BUILD_TYPE=Release -DSIMT_REGISTER_RTL=ON
cmake --build build-register -j4
ctest --test-dir build-register --output-on-failure -j4

cmake -S . -B build-cache-rtl -DSIMT_REGISTER_RTL=ON -DSIMT_SCOREBOARD_RTL=ON -DSIMT_CACHE_RTL=ON -DSIMT_TENSOR_RTL=ON
cmake --build build-cache-rtl -j4
python3 examples/hardware_host.py --build build-cache-rtl --register-rtl --scoreboard-rtl --cache-rtl --tensor-rtl --out artifacts/rtl-register-host

python3 tools/rtl.py --module register_file --out artifacts/rtl-register-file-final
python3 tools/rtl.py --module register_file --synth --infer-only --out artifacts/rtl-register-file-final-full
python3 tools/rtl.py --module register_file --rf-slots 2 --synth --out artifacts/rtl-register-file-final-gates
python3 tools/rtl.py --module register_file --rf-slots 2 --gate-sim --out artifacts/rtl-register-file-final-gates
```

4 项独立 RTL 场景覆盖逐位写回旁路、边沿归属、R63、复位、槽隔离/复用、端口组冲突，以及 2,400 次随机掩码写入与独立数据数组对照。3 项适配器场景覆盖全部四种物理分区几何、跨 32 位地址打包、有限端口、查询不产生时钟和非法调用隔离。实际指令与主机/Graph 复跑范围及最终工具记录见 [VALIDATION.md](VALIDATION.md)。复跑不能重复算成新的独立测试设计。

最终默认 Debug 运行 272 项，其中 271 项通过、1 项因未构建 RTL 跳过；四后端可用的联合 Release 构建通过 37 套件、699 项；寄存器 RTL 的 Debug ASan/UBSan 构建通过 28 套件、405 项。后者仅启用寄存器 RTL，其他硬件为 C++，不能称为四后端联合 sanitizer 验收。

默认 8 槽 partition 的保留存储综合按实例层次展开计数，包含 128 个 `32×128` 存储数组，每数组一读一写，共 524,288 个数据位，即 64 KiB。四 partition 合计 256 KiB/SM；有效位另占 8 KiB/SM。2 槽、16 KiB 小配置实际映射得到 427,025 个 generic cells / 135,168 个触发器位（131,072 数据位 + 4,096 有效位），通过 `check -assert` 和相同 4 项网表测试。完整容量没有展开成通用门网表，也没有宏映射、布局布线或频率结论。

最终普通 RTL、独立 sanitizer、完整 partition 存储推断、小配置综合和小配置网表仿真的五份清单与当前源码 SHA-256 匹配，实际 gate 文件也匹配综合记录。核对摘要位于 `artifacts/rtl-register-file-final/evidence-audit.json`。

四种 RTL 同时运行的向量、规约、GEMM 和小型 Transformer 各完成两次 Graph 重放。删除新增 `register_files` 观测项后，8 份完整 kernel 状态和逐拍 trace 与上一阶段完全相等；工作负载汇总除新增后端属性外也完全相等，设备仍为 64,500 模型拍。另有一次联合 RTL GEMM 监控会话，普通 `nvidia-smi` 显示真实 4070 SUPER 和活模拟器，7 MiB 分配、释放到零及退出移除均通过，见 `artifacts/rtl-register-host-final/`。

首轮 96 项指令复跑中，原 CAS 替换操作数检查使用单 bank 端口配置，首次固定四 bank 接口拒绝了该配置。保留原测试和失败日志，增加真实 RTL 端口共享授权后继续验证，没有删除 WAR/旧替换值检查。

后续 [写回 RTL](RTL_WRITEBACK.md) 已在 collector 后端中接入完成存储与写回年龄仲裁。集成 RF 使用 `SHARED_CLOCK=1` 和逐 lane 写使能，保证内部写回信号在源时钟边沿被可靠采样；独立 RF 保留原宿主引脚门控方式。本页早期综合清单对应当时源码，新组合顶层的当前综合范围见写回文档。

完整目标仍按 [HARDWARE_PLAN.md](HARDWARE_PLAN.md) 推进：执行流水线和整体 SM 连接、其余存储/控制单元、实卡参数校准、PTX 异步矩阵协议和最终硬件审计尚未完成。
