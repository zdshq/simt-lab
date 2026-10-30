# 微架构、硬件结构与原版 nvidia-smi 集成

用户目标（2026-10-29）：认真补齐内部硬件结构，包括 L1 等已有公开逆向研究的细节，并让 nvidia-smi 检测到模拟器。阶段 0–10 的功能 ISS 不满足这次目标；本计划不以已有 75 项功能测试代替硬件验收。

## 完成条件与阶段

| 阶段 | 实现内容 | 必须提供的证据 | 当前状态 |
|---|---|---|---|
| 11 | 官方资料、逆向论文、参数来源与不确定性台账 | 每个结构/参数区分公开事实、实测结论和设计假设；架构代际不得混用 | 进行中；已有本机 118 组/590 个 CUDA/PTX 样本与 30 组模型探针对照，见 MEMORY_CALIBRATION.md；NCU 计数和 L2 辨识仍未完成 |
| 12 | 真实数据缓存、访存合并、L1/L2 tag/sector、MSHR、反压、shared banks、DRAM bank/row/总线 | 冷热、冲突、合并、容量、队列满、跨 SM、数据一致性与独立参考测试 | C++ 时序层次和可选 L1/L2 RTL 已接入实际访存及主机，见 RTL_CACHE_BACKEND.md；参数校准与更完整 DRAM/一致性硬件仍待完成 |
| 13 | SM 分区、warp 发射、寄存器 bank/operand collector、scoreboard、执行流水线、多 CTA 资源准入 | 依赖链/独立链、RAW/WAW、结构冲突、occupancy 和退休顺序；逐周期轨迹 | 基础时序通路已实现，scoreboard、寄存器数据与 collector/读请求仲裁已可选接入 RTL；原 7 项 C++/35 项 Python 检查及后续实际指令复跑见 HARDWARE_SM.md / RTL_SCOREBOARD.md，尚非完整 GPU |
| 14 | 分拍 Tensor 数据通路、乘法/累加单元、操作数供给、TMEM bank、异步完成 | 不再一条宿主循环完成 MMA；数值差分、带宽和流水线反压测试 | 分拍阵列/供数/TMEM 已接入并验证，另可选择实际 RTL 数值/阵列后端，见 HARDWARE_TENSOR.md / RTL_BACKEND.md；当前发起 warp 阻塞到完成，PTX 异步提交协议未实现 |
| 15 | 将上述单元接入实际 ISA 执行、CP、拷贝与 Graph，缓存跨 kernel 保留 | 原功能负载在微架构执行模式中运行；同步与失败行为保持正确；无绕过模型的快路径 | 自定义 59 个 opcode 与持久 CP/copy/Graph 已接入；四类完整负载通过主机上传/Graph/下载执行，跨 kernel 缓存与故障排空通过 4 项 C++/33 项 Python 增量检查；见 HARDWARE_HOST.md，完整 NVIDIA 协议不在此阶段完成范围 |
| 16 | 可综合的关键硬件模块与 RTL 对照 | cache tag/controller、bank arbiter、scoreboard、乘加阵列的仿真和综合结果；面积只报告实际工具结果 | 进行中：L1/L2、Tensor、scoreboard、寄存器数据、collector/读仲裁、七执行端口、整数执行、四级浮点执行、谓词存储/三读端口和有限完成存储/写回仲裁已接入。32 槽写回、整数与浮点模块均完成综合和网表验证，见 RTL_WRITEBACK.md / RTL_INTEGER.md / RTL_FLOATING.md。整数仍需细分算术内部流水级；集合执行 SHFL/VOTE/ELECT/ACTIVEMASK 已直连控制端口与写回，见 RTL_COLLECTIVES.md；六类分支控制转换和每 warp 16 个重聚令牌已接入并通过综合/网表验证，见 RTL_WARP_CONTROL.md；PC/lane/等待元数据存储、PC 组选择、YIELD 恢复和 PC 推进现已直连重聚单元并通过综合/网表验证，见 RTL_WARP_FRONTEND.md；CTA 的 16 项到达记录、动态人数/EXIT 计数、有限命令端口与前端唤醒已接入并通过综合/网表验证，见 RTL_CTA_BARRIER.md；异步事务的完整 1,024 条 RAM 表、人数/字节/阶段算术、发布确认、回收扫描及前端等待/唤醒已接入，见 RTL_TRANSACTION_TABLE.md；64 命令/64 片段 DMA 的描述符、分片、地址/掩码、载荷存储和完成资格已接入，完整容量综合及验收见 RTL_DMA.md；跨 warp 候选选择、重试/轮转、在途计数和串行化已接入，每分区每拍一个候选，完整 32 槽综合及验收见 RTL_WARP_SCHEDULER.md；跨分区 Tensor/异步候选轮转、Tensor 持有和 INT/FP/SFU 发射间隔已接入完整八分区 RTL，见 RTL_EXECUTION_ARBITER.md；原始 128 位机器码与操作数译码已接入每分区组合 RTL，见 RTL_INSTRUCTION_DECODER.md；三源/目标/谓词的并行依赖选择与归约已在 scoreboard 中接入，见 RTL_DEPENDENCIES.md；八分区同步指令数据/tag、缺失补充与轮转已接入并通过完整综合/网表验证，见 RTL_INSTRUCTION_FETCH.md；指令补充已接入实际共享 L2/DRAM、只读映射和失败排空，见 INSTRUCTION_MEMORY_HIERARCHY.md；存储传输连接 RTL、前端顶层路由、异步后台与 CTA 回收服务次序、外部内存返回票据选择、SFU/访存执行与其余 RTL 整机连接仍未完成 |
| 17 | 活设备状态服务与 NVML 接入 | 原版 `/usr/bin/nvidia-smi` 实际枚举活模拟设备，内存/状态跟随执行变化，关闭后不留虚假设备；明确启动方式与真实设备共存范围 | 已验证并安装用户级默认命令：真实设备与监控中的模拟器合并显示，分配/释放/退出正确，13 项监控检查通过三种构建；查询和 ABI 范围见 NVIDIA_SMI.md，不代表 CUDA/PCIe 枚举 |
| 18 | 整体数值/时序/RTL/NVML 验收及结构说明 | 可复现实验命令、波形/trace、来源表、限制和阶段 Git 历史；逐项审计本表 | 待完成 |

## 不改变用户目标的实施边界

- 建立有明确内部状态、资源限制和时钟推进的微架构，不给原功能执行器简单乘一个延迟系数。
- 公开研究通常揭示容量、时延、吞吐和局部映射，不能证明 NVIDIA 完整门级设计。未知的组索引、替换算法、端口数等必须标注假设，并提供参数化实现与验证。不得宣称完整复刻某代 NVIDIA 电路。
- 模型默认两个 SM；先建立自洽、可调的研究架构，分别记录 Ada、SM100、SM120 的证据。已有自定义 TMEM/MMA 扩展不能被命名为 Ada 或 SM120 硬件兼容。
- NVML 管理面可见、CUDA 计算接口可用、PCIe 设备枚举是不同验收项。原版 nvidia-smi 必须实际调用模拟器状态，不能用替代脚本打印表格。现已通过用户级默认命令完成合并显示；其他用户和直接执行系统二进制仍使用原生视图。
- 不覆盖 NVIDIA 驱动或系统库。用户级入口可撤销，只给子进程选择桥接后端；保留真实显卡、原生进程表及明确的原生模式回退，不将有限管理接口冒充完整 NVML 或 CUDA 支持。
- 原目标保持 active；本计划及阶段进展不是完成证明。

## 初始环境证据

- 当前 `/usr/bin/nvidia-smi -L` 只返回 RTX 4070 SUPER；驱动 575.57.08。
- CUDA 编译器和 NVML 官方头文件存在，可对接原程序并进行本机微基准。
- 初始 `verilator`、`iverilog`、`yosys` 不在 PATH；阶段 16 已在项目 `.tools/rtl` 准备固定版本工具，复现方式见 RTL.md，没有修改系统库。
- 初始仓库干净，基线提交 `9b52311`，75 项功能测试。
