# 微架构证据台账

检索日期：2026-10-29。参数进入代码时必须保留来源级别；研究推测不能升级为已知硬件事实。

| 来源 | 可支持的结论 | 使用限制 |
|---|---|---|
| [NVIDIA Ada Tuning Guide](https://docs.nvidia.com/cuda/ada-tuning-guide/) §1.4.1/1.4.2 | 128 KiB unified L1/shared；shared carveout；64K 个 32 位寄存器和最多 48 warp/SM | 不给出完整 L1 tag/索引/替换/端口电路，不用 AD102 L2 容量冒充本机 AD104 |
| [CUDA Best Practices Guide](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html) §10.2.1/10.2.3 | 32 B 访存合并粒度；shared 32 banks，连续 32 位字映射连续 bank，同址读取广播 | 指令级可见行为，不等于完整 SRAM 实现 |
| [Jia et al., Turing T4 microbenchmarking](https://arxiv.org/abs/1903.07486) | 使用指针追逐、冲突模式、指令微基准研究存储与执行单元的方法 | Turing 测量不能直接当作 Ada/Blackwell 参数 |
| [Luo et al., Hopper multi-level analysis](https://arxiv.org/html/2501.12084v2) §4 | RTX 4090/A100/H800 的指针追逐和带宽实验、L2 分区观察 | RTX 4090 不是本机 4070 SUPER；文中 RTX 4090 L2 正文与表格数值有差异，不能当精确常数 |
| [Jarmusch et al., Blackwell microbenchmarks](https://arxiv.org/html/2507.10789v1) | RTX 5080/H100 的访存与指令测试，提供跨代比较方法 | 部分架构描述混合消费级与数据中心特性；不能据此将 tcgen05/TMEM 赋予所有 Blackwell。须以 ISA target 要求核对 |
| [NVIDIA Blackwell Tuning Guide](https://docs.nvidia.com/cuda/blackwell-tuning-guide/) | Blackwell 各 compute capability 的资源及功能说明 | 必须区分 SM100 与 SM120；后续 Tensor 接入需核对相应 PTX 目标 |
| [NVIDIA NVML](https://developer.nvidia.com/management-library-nvml) / 本机 `nvml.h` | 原版 nvidia-smi 底层管理接口及公开 ABI | 管理接口返回设备不代表 NVIDIA 内核驱动或 CUDA 执行兼容 |
| [NVML 初始化/清理](https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlInitializationAndCleanup.html) / [设备查询](https://docs.nvidia.com/deploy/nvml-api/latest/api/group__nvmlDeviceQueries.html)，2026-10-29 核对 | 初始化引用计数、设备数及句柄查询的公开契约 | 本项目追加模拟设备和用户级入口是自定义接入；私有 export-table 仅依据本机匹配驱动的实际验证，不由这些公开文档保证 |
| [PTX ISA 9.4 Tensor Memory / tcgen05](https://docs.nvidia.com/cuda/parallel-thread-execution/#tensor-memory) §9.7.18.1/6，2026-10-29 核对 | 特定 SM100 目标的 TMEM 组织、同步/异步分类、完成与 proxy 顺序要求 | 不是乘加阵列内部电路图；本项目的 128×256 窗口、阻塞式 MMA、分配粒度及失败规则不能称为 PTX 兼容 |
| [PTX ISA 9.4 cp.async](https://docs.nvidia.com/cuda/parallel-thread-execution/#data-movement-and-conversion-instructions-cp-async)，2026-10-29 核对 | 非阻塞 global→shared、4/8/16 B 大小及指定完成机制；`.ca/.cg` 缓存提示 | 本项目 1..4096 B 命令、32 B 事务记录、有限 DMA 池是自定义设计；未实现 async-group/TMA/proxy 的精确接口 |
| [CUDA SIMT kernels / local memory](https://docs.nvidia.com/cuda/cuda-programming-guide/02-basics/writing-cuda-kernels.html)，2026-10-29 核对 | local 的线程私有逻辑归属、位于设备内存、同 warp 同偏移访问可合并 | 本项目 16 KiB 窗口、4 GiB 以上物理布局及 CTA/warp 编号公式均非 NVIDIA 逆向地址映射 |
| [CUDA C++ language extensions / atomics](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/cpp-language-extensions.html)，2026-10-29 核对 | 原子读改写与对应的可见语义 | 不支持推断具体 bank/端口/锁粒度；本项目仅有自定义 32 位整数操作和保守一致性顺序 |

## 首版存储单元中的设计参数

本表的设计假设会实现为真实状态和资源限制，并有单元级验证；尚未标为本机测量或硬件复刻。

| 参数/策略 | 初始选择 | 依据级别 |
|---|---|---|
| warp/合并粒度 | 32 lane / 32 B sector | 官方可见行为 |
| L1 data capacity | 64 KiB/SM，配合 64 KiB shared 预算 | 自定义 carveout 选择；总预算参考 Ada 128 KiB |
| cache line、组索引、相联度 | 128 B line、modulo set、4-way L1/8-way L2 | 可替换模型假设；未知 NVIDIA 地址 hash |
| 替换、写策略 | LRU；write-through/no-write-allocate | 本项目明确选择，不声称 NVIDIA 采用完全相同算法 |
| sector valid/data、MSHR 合并 | 每 sector 独立有效位与真实字节；相同 sector 未命中合并 | 非阻塞缓存实现机制；资源数量是模型配置 |
| L2 | 256 KiB 研究配置，所有模拟 SM 共享 | 两 SM 小型目标的设计选择，不对应整张真实显卡 |
| DRAM | bank/row 分解、open-page、FCFS、显式 tRCD/tCL/tRP/tBURST、共享数据总线 | 教学/研究控制器；不冒充 GDDR6X 的全部时序约束 |
| 各级周期延迟 | 配置值，测试使用便于推导的小值 | 模拟时钟；完成本机校准前不得预测真实性能 |

## 参数校准待办

SM 时序模式新增的 4 分区、4 寄存器 banks、每 bank 向量读写端口、4 个 collectors、INT/FP/SFU 延迟 4/6/16 与发射间隔 1/1/4，均属于**本项目模型假设**。48 warp/SM 和 64K 寄存器预算可与上述 Ada 官方资源上限对照，但固定每线程预留 64 寄存器、128 B shared 分配粒度、槽位 modulo 分区、控制指令排空及每 warp 单个在途访存均为本项目选择。不能从资源上限相似推导内部流水线相同。具体实现与可复现实验见 [HARDWARE_SM.md](HARDWARE_SM.md)。

Tensor 模式的 4×4 输出驻留阵列、相邻 PE 每拍转发、4 拍乘加、深度 2 的输入 FIFO、每 SM 256 列预算、16 个 TMEM banks 和 modulo bank 映射同样是模型假设。块缩放复用 PE 三段执行、稀疏按逻辑 K 传输并禁用缺失项是本项目选择；不得宣称它们是 NVIDIA 的实际 datapath、稀疏吞吐或隐藏延迟。来源、差异、可观测结果见 [HARDWARE_TENSOR.md](HARDWARE_TENSOR.md)。

异步模式的每 SM 8 个命令、4 个片段、每片段至多 128 B、16 个事务条目、单控制端口及 2 拍处理延迟全部是模型假设。shared 记录按端口发布、逻辑表等待发布确认、完成优先仲裁和未对齐 byte 路径是本项目选择，不是逆向得到的 NVIDIA 实现。对照实验与协议差异见 [HARDWARE_ASYNC.md](HARDWARE_ASYNC.md)。

local 的具体字交错物理布局、global 原子 8 banks/8 请求/4 拍 ALU、bank 锁直到写穿确认、连续原子批次前后排空普通请求，以及 shared 原子独占 bank 等待 ALU 均为模型选择。没有将写穿导致的 DRAM 反复访问当成 NVIDIA 原子延迟；证据及限制见 [HARDWARE_ATOMIC_LOCAL.md](HARDWARE_ATOMIC_LOCAL.md)。

持久主机模式的 16 个 CP 解码槽、每拍一次取包/图解码/任务启动、128 B 拷贝暂存、每块 8 拍建立加 16 B/clock 链路服务，全部是模型设计选择。主机 DMA 绕过 L1、写入失效全部 L1、按所有者故障排空及不重复 local tag 是本项目一致性与生命周期协议；没有据此声称 NVIDIA 的 PCIe、GPFIFO、copy engine 或 fault/replay 微架构。对应证据见 [HARDWARE_HOST.md](HARDWARE_HOST.md)。

独立 RTL 的 128 sets/4 ways、256 位同步 SRAM 读写端口、4 个 MSHR/8 个响应槽、4 个合并等待者、有限 LRU rank、单 warp 单命令 scoreboard 及 bank 循环仲裁均是本项目电路选择。它们与 C++ 参考模型比较功能事务和资源约束，不构成 NVIDIA 门级逆向；单入口 RTL 缓存也不是 C++ 四 bank 端口的逐拍等价替换。Verilator/Yosys 验证和实际 generic-cell 计数见 [RTL.md](RTL.md)，计数不能替代工艺面积或整 GPU 门数估计。

L1/L2 RTL 现已接入 ISA 和主机链路；L2 选择 256 sets/8 ways、256 KiB，同样使用单请求端口与 4 个 MSHR。整体/范围失效每拍扫描一条 tag，L1/L2 分别 512/2048 拍；64 项维护队列和 C++ 全局写顺序同样是设计假设。没有将其描述为 NVIDIA 缓存失效延迟、bank hash 或一致性协议的逆向结果，见 [RTL_CACHE_BACKEND.md](RTL_CACHE_BACKEND.md)。

LRU 的每组独立 rank 寄存器、局部更新 mux 和组写使能是本项目为完整 L2 综合实现的电路组织，保留原精确 LRU 策略。L1/L2 的 1,024/6,144 个 rank 状态位已由 Yosys 中间网表核对，不能据此声称 NVIDIA 使用同样的替换算法或布局。数据 SRAM 的单读/单写推断已验证；多读口 tag 等元数据尚未映射到具体 SRAM/BRAM。

Tensor RTL 的四级 FP32 FMA、77 位对齐通路、完整次正规数和最近偶数舍入、独立 INT8 流水线、两项 FIFO、16 个 PE 及反馈旁路都是项目自己的电路设计。它们实现既定数值约定，不是 NVIDIA 内部电路、精度模式或延迟的逆向结论。当前已通过可选 [RTL 后端](RTL_BACKEND.md) 接入 ISA/TMEM/主机 Graph，前端仍有 C++，存储层次可选 L1/L2 RTL。加入 FMA 逐级有效写使能后，完整阵列为 218,054 个 generic cells，来自实际 Yosys/ABC 层次统计，不能用于声称整 SM 或显卡门数，见 [RTL_TENSOR.md](RTL_TENSOR.md)。

2026-10-30 已实现 CUDA/PTX pointer-chase、合并和 shared bank 微基准，并完成本机 118 组/590 个样本与 30 组正式模型探针对照，见 [MEMORY_CALIBRATION.md](MEMORY_CALIBRATION.md)。缓存访问顺序、carveout 偏好、两个种子、计时控制、SASS 和环境均有记录；L2 容量边界仍未由延迟曲线确认，NCU sector 计数因权限未取得。模型与实卡的 shared 相对代价不同；没有直接把循环斜率填进单级硬件延迟配置。参数辨识与校准继续进行，结果不证明结构唯一可识别。

接入 ISA 的 scoreboard RTL 按物理 warp 槽保存 63×32 个写者位、63×32 个 6 位读者计数和 8×32 个谓词位；16 组事件及逐寄存器阶段顺序来自本项目的发射/端口约束。事件时钟门控要求控制信号在低电平阶段稳定，当前没有工艺 ICG 映射、时序收敛或功耗数据。这些结构与数目都是项目设计，不能从 NVIDIA 的可见依赖行为推断出相同电路，详见 [RTL_SCOREBOARD.md](RTL_SCOREBOARD.md)。

寄存器数据后端的四个物理 bank、32 个固定 64-register warp 槽、逐 lane 有效位、异步读/边沿写、写回旁路和 1/2/4 端口共享组也是本项目设计选择。并未用公开微基准证明 NVIDIA 使用相同存储组织或清零方式。RTL 存储推断、门级小配置、控制输入低电平更新约定与执行接入边界见 [RTL_REGISTERS.md](RTL_REGISTERS.md)。

谓词存储的每 warp 七个 32 位字、每字一个有效位、两个组合读端口、一个掩码写端口和清槽后零值语义，是针对本项目 ISA 的电路选择。32 槽实际综合为 7,168 个数据触发器位与 224 个有效位；不据此声称 NVIDIA 使用相同的物理寄存器、复位或读取网络。guard/VOTE 的来源、直连写回和仍由 C++ 实现的调度边界见 [RTL_PREDICATES.md](RTL_PREDICATES.md)。

操作数收集器的 32 个物理条目、三个去重源、32 lane 锁存、年龄优先读仲裁、同拍最后源旁路与释放复用均为本项目设计选择。相对年龄压缩解决有限时间戳回绕；该实现不是公开研究已确认的 NVIDIA collector 拓扑。默认四条目额度与物理 32 条目容量分开报告，具体状态位和验证见 [RTL_COLLECTOR.md](RTL_COLLECTOR.md)。

七类执行端口、每条目 3 位 pipe 字段、年龄优先选择及候选/确认协议是本项目设计选择，详见 [RTL_DISPATCH.md](RTL_DISPATCH.md)。不从 NVIDIA 指令类别推断其物理端口数量，也不把本项目综合成本当作 NVIDIA 芯片数据。

整数执行的 32 lane 组合 ALU、32 项结果 FIFO、逐项倒计数和 collector→整数→写回直连也是项目设计，见 [RTL_INTEGER.md](RTL_INTEGER.md)。默认四拍是模型配置，不代表乘法器内部已经划分四级，也没有根据公开微基准确认 NVIDIA 使用相同队列或数据通路。S2R/CVTA 实现的是自定义 ISA 的上下文和地址窗口，不是 NVIDIA 特殊寄存器、MMU 或 TLB 的完整实现。

普通浮点执行实例化 32 份四级 FMA，转换/比较使用组合电路和四级对齐寄存器，32 项预约队列保存结果和额外延迟，见 [RTL_FLOATING.md](RTL_FLOATING.md)。四级最短时延与默认六拍均为项目设计；没有声称 NVIDIA 采用相同加乘复用、队列、端口数、转换流水划分或舍入精度实现。


集合执行的 32 lane 跨 lane 多路选择、最低成员编码、布尔归约、单项结果寄存器及控制完成端口优先级是本项目硬件设计。谓词文件新增独立执行读口，三个端口共享同一数据和有效位。集合单元 55,423 cells/1,030 FF 位、32 槽谓词文件 68,814 cells/7,392 FF 位来自本次 Yosys/ABC 实际综合；不代表 NVIDIA 网络拓扑、真实延迟或具体工艺面积。实际 ISA 和主机 Graph 接入见 [RTL_COLLECTIVES.md](RTL_COLLECTIVES.md)。

分支重聚的每 warp 16 个 98 位令牌记录、并行参与/到达更新、16 位完成向量和按 lane 等待编号选择目标都是项目自定义电路，不是 NVIDIA 逆向得到的结构。实际综合为 68,030 个 generic cells/1,568 个触发器位，令牌存储未映射为 SRAM；独立子模块的 lane 输入在实际执行中现由下述前端存储提供，指令目标检查仍由宿主完成。数据来源、时钟边界和验证见 [RTL_WARP_CONTROL.md](RTL_WARP_CONTROL.md)。

Warp 前端的 3,233 位 PC/lane/等待元数据/游标存储、五层最小/最大/后继树、两个有序外部等待字段写端口及同拍退休到发射旁路属于本项目设计。完整连接令牌并加入 CTA/异步阶段端口后的前端实际综合为 125,662 generic cells / 4,801 FF 位，没有 SRAM 或工艺映射结论；不能根据 NVIDIA 的线程调度语义推断其使用同一拓扑。跨 warp 候选选择现已接入独立 RTL；取指译码、依赖查询组合、异步地址分组与 DMA 外部票据桥仍有 C++，见 [RTL_WARP_FRONTEND.md](RTL_WARP_FRONTEND.md)。

CTA 的每块 16 个屏障、逐线程到达/存活位图、缓存动态目标人数、64 位代数、单命令端口和前端完成信号匹配均为本项目设计；不是 NVIDIA 公开的内部电路。完整 1,024 线程表实际综合为 234,669 generic cells / 18,999 FF 位，没有 SRAM 宏或工艺布局。EXIT 保留已到达贡献、仅扣除未到达退出者的规则来自本项目 ISA；C++ 仍负责跨分区提交次序与实例间引脚路由，见 [RTL_CTA_BARRIER.md](RTL_CTA_BARRIER.md)。

异步事务表的 1,024×224 位同步 RAM、独立有效位、两拍串行搜索、发布确认后提交、两遍 CTA 回收及完整地址/阶段唤醒都是本项目设计，未通过 NVIDIA 逆向实验证明。完整表已推断为 229,376 位单读单写 RAM，RAM 外 1,410 FF 位；16 条配置的完整通用门映射为 12,533 cells / 4,192 FF 位，不能代表 1,024 条表的门数或物理 SRAM 面积。自定义 CP_ASYNC 与 PTX 的协议差异保留，见 [RTL_TRANSACTION_TABLE.md](RTL_TRANSACTION_TABLE.md)。

DMA 的 64×259 位命令寄存器、64×1,108 位片段寄存器、四个独立轮转游标、33 位地址、读/写各一条返回总线、请求重选气泡及领取/释放分离协议均为项目设计。完整物理配置实际综合为 400,338 generic cells / 87,547 FF 位，其中 65,536 位是无复位载荷触发器；没有 SRAM 宏、工艺面积或 NVIDIA 拓扑结论。片段与载荷控制现已接入 RTL，外部票据返回选择、shared bank 服务和内存互联仍有 C++，见 [RTL_DMA.md](RTL_DMA.md)。

跨 warp 调度器的每分区 32 个物理槽、325 位状态、单候选检查端口、轮转/固定槽优先级及拒绝重试集合均为本项目设计，未通过 NVIDIA 逆向实验证明。完整实例实际综合为 7,625 generic cells / 325 FF 位，无 SRAM；这一局部数字不含取指译码、scoreboard、寄存器或其他执行资源。实际边界与计数口径见 [RTL_WARP_SCHEDULER.md](RTL_WARP_SCHEDULER.md)。
