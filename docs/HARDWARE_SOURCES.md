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

实现 CUDA pointer-chase、合并和 shared bank 微基准，记录原始结果、GPU/驱动/编译器信息、样本分布和计时开销。结果用于判断模型能否解释趋势，不通过调参强行宣称结构唯一可识别。
