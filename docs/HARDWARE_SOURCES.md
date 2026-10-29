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

实现 CUDA pointer-chase、合并和 shared bank 微基准，记录原始结果、GPU/驱动/编译器信息、样本分布和计时开销。结果用于判断模型能否解释趋势，不通过调参强行宣称结构唯一可识别。
