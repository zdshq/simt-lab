# 寄存器堆直连的操作数收集器与读请求仲裁

构建 `SIMT_REGISTER_RTL=ON` 后，运行 `--timing --register-rtl --collector-rtl`，或使用 Python `Device(timing=True, register_rtl=True, collector_rtl=True)`。此模式实例化 `register_operand_unit.sv`，将实际寄存器堆的读端口直接接到 `operand_collector.sv`。数据不会先经 C++ 缓冲，再回填电路。

本阶段电路负责收集槽分配、源寄存器/掩码、未收集位、年龄排序、bank 读授权、数据锁存、就绪、释放和同拍复用。C++ 仍负责指令解码、warp 调度、执行单元准入与尚未电路化的运算。它不是完整 RTL SM，也不是 NVIDIA operand collector 电路的精确逆向。

## 每个分区保存什么

每个 partition 固定 32 个物理 collector 条目，运行时 `--collectors 1..32` 限制可用额度；默认额度 4。额度变小不缩小已综合的电路。每条目最多三个去重后的普通寄存器源，R63 和空掩码源在上游去除，直接提交这种非法描述符会被电路拒绝。

| 状态 | 默认 8 个 warp 槽/partition 时的每条目宽度 | 用途 |
|---|---:|---|
| occupied | 1 位 | 区分空槽与存活指令 |
| rank | 6 位 | 在存活条目中的相对发射年龄 |
| warp | 3 位 | 本 partition 的物理寄存器槽 |
| count | 2 位 | 实际普通源数量，0–3 |
| registers | 18 位 | 三个 6 位源寄存器编号 |
| masks | 96 位 | 三个 32 lane 源读取掩码 |
| waiting | 3 位 | 哪些源仍需物理读取 |
| values | 3×1,024 位 | 三个完整 warp 向量的锁存值 |

32 个条目的数据共 98,304 位，即 12 KiB；上述元数据共 4,128 位。分区的 warp 容量改变时只有 warp 字段宽度跟随改变。数据不复位，occupied/waiting 和输出掩码阻止未初始化值被消费；释放槽可以保留旧比特，但新描述符必须重新收集各个有效源。

## 请求如何仲裁

每拍在所有 occupied 且 waiting 的源中，针对每个物理端口组选择 rank 最小的条目，同一条目选描述符中较前的源。四端口组时按 `reg % 4`；二组时 bank 0/2 和 1/3 分别共享；单组时每拍只能读取一个向量。不同组能同拍服务同一指令的多个源。

这是电路中的年龄优先仲裁，与原 C++ 按动态指令顺序收集的策略一致。早期独立 `register_arbiter.sv` 是循环优先级，仍保留为另一种策略的基元；本次接入的是 collector 内部年龄仲裁，不能说已经使用那个循环仲裁器。

rank 不采用固定宽度的递增时间戳。释放条目时，每个存活条目减去比自己更老的已释放条目数量；新指令得到释放后存活条目数。这样长期等待执行单元的指令仍保持最老身份，低编号槽复用不会插队，也不会因时间戳回绕改变优先级。

已授权的 `(warp, reg)` 在 RTL 内生成 bank 行地址，寄存器数据沿连线进入对应 collector 数据端口。四个 bank 每拍最多四个向量写入 collector。接口暴露授权条目、源编号和 lane mask，C++ 只据这些真实事件释放 scoreboard 读者并记录 trace，不参与挑选获胜源或计算其值。

## 就绪、执行与同拍复用

collector 保存每个已收集的旧值。较新的指令即使随后覆盖寄存器堆，也不能改变已锁存操作数。最后一个源可以经当前 bank 返回值旁路到执行输出，无须人为增加一拍；若执行单元忙，数据在边沿锁存，随后等待期间保持不变。

执行资源接受指令后，C++ 执行锁存器才读取 collector 输出，之后计算或提交访存。它不会在发射时复制整份寄存器文件来代替物理收集。控制、Tensor 和 DMA 指令同样使用此输出路径。谓词快照、PC 与执行阶段状态仍为 C++，尚未合入本电路。

当前输出是一个组合查询端口，宿主可在同一模型拍的低电平阶段依次读取多个已就绪条目。这是向 C++ 执行层交接数据的边界，不代表已经实现多个执行单元同时接收操作数的物理端口网络；完整 RTL SM 还需要相应的输出仲裁、传输端口和执行锁存器。

释放通过条目位图提交，一拍可释放多个已就绪条目；未就绪释放报错，不清除其状态。分配端口每拍最多接受一条新指令。刚释放的槽可以同拍被新指令复用，但当前拍的旧源授权和查询仍属于旧指令，新描述符在下一边沿接管槽位。

适配器仅保留每个物理 collector 对应的动态指令 ID，用于找到 scoreboard 和 trace；没有第二份 waiting 表、rank 表或源数据存储。实际发射容量由 RTL `allocate_ready` 决定，实际待执行状态由 RTL ready 位决定。

## 时钟与连接边界

寄存器堆和收集器由同一个顶层 `clk` 驱动。提交端口、组合查询、收集与执行接收都不插入额外时钟；整机每拍末推进一次边沿。读期间先完成写回引脚设置，因此现有 RTL 写回旁路可以服务同拍收集。运行时设置 `collect_enable` 后不允许继续追加寄存器写回。

collector 状态时钟门由输入 `reset || allocate_valid || |release_entries || collect_enable` 驱动，控制信号只在时钟低电平更新。使用输入控制避免状态反馈形成门控时钟环。实际输出依赖的状态门控在初次 Verilator 检查中被拒绝，随后改为这个明确输入契约，没有关闭警告检查。适配器读取电路 waiting 位；没有待处理源时不拉高 `collect_enable`，不执行无状态变化的重复求值。已收齐但等待执行的条目仍保持数据，普通查询不打开时钟门。`active_clocks` 如实记录有效边沿，不将它解释为真实芯片功耗。

没有工艺 ICG、SRAM/BRAM 映射或布局布线结论。collector 的多写口数据数组和元数据多端口不是商品 SRAM 宏，完整综合和小配置成本只报告工具实际结果。

`hardware.collectors[]` 报告物理条目数、可用额度、当前占用、输入/有效时钟、分配/释放次数、真实向量读取数和未获授权的源请求数。普通状态查询和 NVML 查询不会推进电路。失败 kernel 的私有 collector 停止，后续存储排空可能继续推进设备时间。

## 运行与验证

```sh
cmake -S . -B build-register -DSIMT_REGISTER_RTL=ON
cmake --build build-register -j4
ctest --test-dir build-register --output-on-failure -j4

python3 tools/rtl.py --module collector --out artifacts/rtl-collector-final
python3 tools/rtl.py --module collector --collector-entries 4 --synth --out artifacts/rtl-collector-gates
python3 tools/rtl.py --module collector --collector-entries 4 --gate-sim --out artifacts/rtl-collector-gates
python3 tools/rtl.py --module operand_unit --out artifacts/rtl-operand-unit

python3 examples/hardware_host.py --build build-cache-rtl --register-rtl --collector-rtl --scoreboard-rtl --cache-rtl --tensor-rtl --out artifacts/rtl-collector-host-final
```

五项独立 collector 场景覆盖旧值保持、lane mask、边沿归属、非法描述符、容量、同拍复用、全部物理槽填满/年龄压缩，以及 1,200 拍独立随机生命周期对照。参考用不回绕的软件年龄和独立向量，不复用 RTL 的 rank 算法。两项连接场景直接驱动完整顶层，验证寄存器覆写后旧源保持、寄存器槽清除与 collector 隔离、同拍写回旁路及槽复用。

三项 C++ 适配器场景覆盖四种 partition 几何、端口冲突、容量、非法调用、查询无时钟，以及空槽/已收齐等待条目的时钟门关闭。既有 96 项指令以及 34 项主机/Graph 检查在此后端复跑。实际完整构建、sanitizer、综合、网表与端到端记录见 [VALIDATION.md](VALIDATION.md)，不将重复运行算成新增独立场景。

最终联合 Release 通过 41 套件、834 项；寄存器/collector RTL 的 Debug ASan/UBSan 通过 32 套件、540 项。首轮 sanitizer 的最大矩阵场景超出既有 120 秒子进程限制；根据实际 waiting 位省去空闲求值后，相同 96 项执行检查在 133.94 秒内完成。矩阵规模、数值断言、120 秒单用例限制和地址/未定义行为检测都保留。

Yosys 完整展开 32 条目 collector 得到 840,085 个 generic cells，其中 102,432 个触发器位；4 条目配置为 122,764 个 generic cells、12,804 个触发器位。两种配置的综合网表均通过相同 5 项行为检查。它们只包含 collector，不含寄存器堆、执行单元或整个 SM，也不是等效 NAND2 门数。32 条目仲裁/多端口实现成本较大，尚无物理实现优化和工艺面积结论。

8 个 warp 槽寄存器堆与 32 条目 collector 的直连顶层完成保留存储综合。按层次实例展开共有 135 个存储数组、626,688 个存储位：寄存器数据 524,288 位、collector 数组 102,400 位；collector 的 32 个 occupied 位等独立触发器不在这个存储数组合计中。不能将这些多端口数组都解释为可直接映射的 SRAM 宏。

五种 RTL 后端同时运行向量、规约、GEMM 和 Transformer，每种两次 Graph 重放通过。与上一寄存器阶段相比，架构状态、模型周期和存储统计一致；Transformer 的同一周期内 trace 事件排列变化，按周期和指令归组后相等。原版 `nvidia-smi` 经 NVML 桥接层还能观察实际会话的分配、释放和退出，详见 [监控边界](NVIDIA_SMI.md)。

完整目标仍包括写回请求仲裁、warp 发射/执行电路、其余 RTL 整体连接、真实 GPU 参数校准、PTX 异步矩阵协议和最终审计，见 [HARDWARE_PLAN.md](HARDWARE_PLAN.md)。
