# SIMT Lab

按 curryGPU 公开文章的分层思路，从指令集开始实现的可验证 GPU 功能模拟器。

项目使用 C++20 实现执行核心，Python 生成 ISA 编码接口、构造 kernel 和独立参考结果。默认设备含两个 SM，每个 warp 有 32 个逻辑线程。它运行在 CPU 上，模拟的是自定义机器的行为，不预测真实 GPU 周期、面积或功耗。

这里使用自己的指令编码与数值规范，借鉴作者公开的设计方法，不宣称兼容 curryGPU 的私有二进制、NVIDIA SASS 或完整 CUDA。ISS 和时序模型已运行；缓存、scoreboard、寄存器 bank 仲裁及 Tensor 数值/乘加阵列已有验证的 RTL。L1/L2、Tensor、scoreboard、寄存器数据、操作数收集器、整数/浮点/集合执行、重聚令牌、warp PC/状态/分组选择、跨 warp 调度、跨分区执行仲裁、CTA/异步事务屏障、DMA 与写回 RTL 已可选接入 ISA 和主机 Graph；完整 RTL GPU、PTX 编译器和 FPGA 实现仍未完成。

开发阶段、验收条件和当前进展见 [实施计划](docs/PLAN.md)、[主机与 Graph 扩展计划](docs/HOST_PLAN.md) 和 [开发记录](docs/DEVELOPMENT.md)。每个通过验收的阶段单独 Git 提交。

## 构建与运行

依赖 C++20 编译器、CMake 3.20+、Python 3.10+；不需要 CUDA 或 Python 第三方包。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
python3 examples/identifiers.py
./build/simt-run build/identifiers.bin --sms 2 --blocks 5 --threads 37
python3 examples/run_workloads.py
python3 examples/host_graph.py
```

加 `--dump-state` 可以查看每个 CTA/warp 的寄存器和谓词。程序输出 JSON，失败返回退出码 2，并报告 block、SM、warp 与 PC。编码规则和当前实现边界见 [ISA 规范](docs/ISA.md)。

已实现独立线程调度、分支重聚、warp 集合操作、双 SM 的多 CTA/warp 内存与屏障，以及 TMEM、五种数据格式的 MMA、2:4 稀疏、块缩放和异步搬运事务。提供向量、规约、分块 GEMM 和小型 Transformer block 的机器码示例与独立参考。

先看 [从示例读懂模拟器](docs/WALKTHROUGH.md)，再查 [内存规范](docs/MEMORY.md) 和 [张量规范](docs/TENSOR.md)。示例产物写入 `artifacts/examples/`，其中包含可阅读汇编、机器码、启动参数及验证结果。

新增持久设备进程 `simt-device`、Python 主机接口、命令环与 doorbell、软件 CP、独立计算/拷贝引擎、stream/event 和完成状态。支持 CUDA Graph 风格的显式建图、实例化、重放、节点参数更新和跨 stream capture，详见 [主机、命令处理器与 Graph](docs/HOST.md)。这套自定义接口不能直接运行标准 CUDA 程序；原版 nvidia-smi 查询模拟器须使用下述 NVML 接入方式。

`host_graph.py` 验证普通提交的 4 个命令包与图重放的 1 个命令包均得到正确向量结果，并输出 `artifacts/host-graph/trace.json` 与 `validation.json`；包数量不是实际 CUDA 加速比。

阶段 0–10 验收覆盖 Debug、Release 和 ASan/UBSan 三种构建，当时每种构建的九个套件、75 项测试全部通过。后续硬件增量、命令、证据映射与未实现范围见 [验证记录](docs/VALIDATION.md)。

## 路线依据

- [ISA](https://zhuanlan.zhihu.com/p/2044711939746566963)：先定义机器状态与指令语义。
- [ISS](https://zhuanlan.zhihu.com/p/2046857919606661244)：编码表、C++ 执行核、Python 构造器与分层验收。
- [独立线程调度](https://zhuanlan.zhihu.com/p/2047985048213780000)：per-lane PC、按 PC 分组与重聚。
- [内存同步](https://zhuanlan.zhihu.com/p/2048501401848903199)：多 warp 调度、分离地址空间和屏障。
- [MMA](https://zhuanlan.zhihu.com/p/2056027770405270973)：矩阵语义、TMEM、稀疏元数据与块缩放。

本目录是独立 Git 仓库。上级目录保存的网页和讨论文档不加入此仓库。

## 进行中的硬件模型扩展

用户要求进一步实现内部硬件结构和原版 nvidia-smi 集成，完整目标与剩余验收见 [硬件计划](docs/HARDWARE_PLAN.md)。已新增 [逐周期存储单元](docs/HARDWARE_MEMORY.md)：真实 sector cache 数据、MSHR/反压、shared banks、DRAM bank/row 时序及双 SM 存储层次，三种构建下 17 项新增检查通过。

新增 [SM 时序执行](docs/HARDWARE_SM.md)：`simt-run --timing` 已将基础算术、控制、屏障和 load/store 接入寄存器 banks、scoreboard、有限 operand collectors、执行/写回流水线以及真实缓存；支持按资源限制多 CTA 同驻留。`python3 examples/hardware_pipeline.py` 可生成依赖链、缓存及 shared bank 实验和逐拍轨迹。

[Tensor 时序通路](docs/HARDWARE_TENSOR.md) 已接入逐拍乘加阵列、有限操作数 FIFO、真实内存供数和 TMEM bank。[异步搬运通路](docs/HARDWARE_ASYNC.md) 已接入有限 DMA 命令/片段池、真实 global→shared 数据流和事务屏障。运行 `python3 examples/run_workloads.py --timing --out artifacts/timed` 可验证向量、规约、GEMM 与小型 Transformer；`python3 examples/hardware_async.py` 可查看容量与完成顺序实验。

已验证 [原版 nvidia-smi 接入](docs/NVIDIA_SMI.md)：本机已安装用户级命令入口，终端一运行 `python3 examples/nvml_device.py --timing`，终端二直接运行 `nvidia-smi -L` 或 `nvidia-smi`，可同时查看真实 4070 SUPER 和活模拟器。分配、释放和退出会更新原版程序的输出；系统二进制及驱动未改动。自己的 `Device` 会话须显式设置 `monitor_dir`；这不提供 CUDA 或 PCIe 兼容。安装、撤销及查询范围见链接文档。

[Local 与原子通路](docs/HARDWARE_ATOMIC_LOCAL.md) 已接入字交错物理映射、同一缓存/DRAM 层次，以及 global/shared 原子的 bank 预约、读取、ALU 延迟和写确认。`python3 examples/hardware_memory_ops.py` 可生成合并、缓存和原子竞争实验。自定义 59 个 opcode 均有时序路径，仍不表示支持 NVIDIA 的全部硬件。

[持久设备时序](docs/HARDWARE_HOST.md) 已将主机 CP、拷贝和 Graph 接入同一时钟，L1/L2 跨 kernel 保留。使用 `Device(timing=True)` 或 `python3 examples/hardware_host.py` 可执行带上传/下载的四类完整负载及 Graph 重放，并检查缓存、链路、故障排空和逐拍轨迹。

[关键模块 RTL](docs/RTL.md) 已实现 64 KiB sector 缓存的数据阵列和控制器、逐 lane scoreboard、4 bank 读写仲裁。原 14 项检查通过 RTL、sanitizer 和网表验证；缓存新增范围失效扫描后，11 项缓存检查在上述三种模式和 64 KiB 配置下通过。

[Tensor RTL](docs/RTL_TENSOR.md) 新增四级 FP32 融合乘加、INT8 乘加、FP16/BF16/FP4 转换及完整 4×4 PE 网格，包含实际规格化/舍入电路、两项 FIFO、稀疏跳过和三阶段块缩放。新增 16 项 RTL、sanitizer 和综合后网表检查，加上缓存扫描增量，该阶段共 32 项；后续 scoreboard 接入另增 4 项、寄存器数据再增 4 项独立电路检查。

[RTL 执行后端](docs/RTL_BACKEND.md) 已将该阵列接入实际 ISA/TMEM 与主机 Graph。以 `-DSIMT_TENSOR_RTL=ON` 构建后，运行 `python3 examples/hardware_host.py --build build-rtl --tensor-rtl`；每个 SM 拥有独立 RTL 实例，格式转换和乘加结果由电路产生，其余系统仍由 C++ 模拟。没有构建 RTL 却请求它会明确报错。

[L1/L2 RTL 后端](docs/RTL_CACHE_BACKEND.md) 已接入每 SM 64 KiB L1 和共享 256 KiB L2。额外以 `-DSIMT_CACHE_RTL=ON` 构建，通过 `--cache-rtl` 或 `Device(timing=True, cache_rtl=True)` 启用，可与 Tensor RTL 同时使用。实际 tag/data、缺失合并和返回来自电路，DRAM、外部请求/返回路由及一致性协调仍有 C++。

缓存 LRU 已按组实现为排名寄存器和局部比较电路，完整 256 KiB、8 路 L2 的保留存储综合、RTL 和 sanitizer 检查均通过。2/4 KiB 小配置另通过完整综合及相同网表行为检查；这些结果不等于具体 FPGA/ASIC 的存储宏映射或整 GPU 面积。

[Scoreboard RTL 后端](docs/RTL_SCOREBOARD.md) 已将逐 lane 写者、读者计数和谓词依赖接入实际指令与主机 Graph。用 `-DSIMT_SCOREBOARD_RTL=ON` 构建、`--scoreboard-rtl` 运行；可与 Tensor/L1/L2 RTL 同时启用。包含有限同拍事件、阶段旁路和事件时钟门控，报告输入时钟与有效更新边沿。寄存器数据还可选择下述 RTL 后端，collector 和读请求仲裁另可选择下述直连电路。

[寄存器数据 RTL 后端](docs/RTL_REGISTERS.md) 用 `-DSIMT_REGISTER_RTL=ON` 构建、`--register-rtl` 或 Python `register_rtl=True` 启用。每 SM 256 KiB 数据按 partition 与四个物理 bank 划分，包含逐 lane 有效位、masked write、写回旁路和槽复用清零；源操作数来自实际电路输出，可与其他三个 RTL 后端联合运行。

[操作数收集器 RTL](docs/RTL_COLLECTOR.md) 通过 `--register-rtl --collector-rtl` 或 Python `register_rtl=True, collector_rtl=True` 启用。每分区 32 个物理条目保存三个源向量、待收集位和相对年龄；读请求由电路仲裁，寄存器数据直接连入收集器。现已有 [七个物理执行输出端口](docs/RTL_DISPATCH.md)，由电路按年龄选择候选，同端口每拍最多确认一次；整数和浮点端直接连接下面的 RTL，其余端口仍由 C++ 执行锁存器接收。

[写回 RTL](docs/RTL_WRITEBACK.md) 现随 collector 后端启用。每分区 32 个完成槽在发射时预约，接收七类执行结果，按年龄竞争有限写端口，直接驱动寄存器堆。满槽阻止继续发射，等待结果由电路保存；执行流水线和部分控制仍由 C++ 提供。完整 32 槽写回模块的综合和五项网表检查通过。

[整数执行 RTL](docs/RTL_INTEGER.md) 同样随 collector 后端启用。16 种整数操作的源数据直接来自 collector，32 lane 电路执行算术、比较、特殊寄存器和地址转换，结果经过 32 项 FIFO 直接进入写回端口 0。延迟由 RTL 倒计数推进，已验证 1/4/100 拍；这是组合 ALU 加有界结果队列，没有宣称乘法器已切成对应数量的内部流水级。完整 32 项整数模块已通过实际综合和四项网表行为检查。

[浮点执行 RTL](docs/RTL_FLOATING.md) 随 collector 后端启用，FADD/FMUL/FFMA 经过 32 份四级 FMA，FSETP/I2F/F2I 使用位电路和对齐寄存器。结果在发射前预约 32 项有限存储，再直接送入写回端口 1。默认六拍保留，额外延迟由硬件计数；配置小于四拍明确拒绝。完整模块已通过实际综合网表检查，SFU 和部分控制仍为 C++。

[谓词存储 RTL](docs/RTL_PREDICATES.md) 随寄存器后端启用，每 warp 七个 32 位字、三个读取多路器和一个掩码写端口；P7 为常量。发射 guard 和 VOTE 读取实际电路输出，collector 模式的比较/ELECT 结果经写回总线直接进入存储，包含同拍旁路与槽复用清零。8/32 槽完成综合与实际网表验证；集合运算、warp 内 PC/状态和跨 warp 候选选择现有下述 RTL 通路，指令后备存储仍有 C++。

新增 [跨 lane 集合 RTL](docs/RTL_COLLECTIVES.md)：SHFL/VOTE/ELECT/ACTIVEMASK 通过 collector 控制端口、独立谓词读口、选择/归约电路和单项结果寄存器直接进入写回。集合单元已通过 RTL、sanitizer 和综合网表验证；CTA 屏障与跨 warp 候选选择也已接入各自 RTL，实例间路由仍有 C++。

新增 [分支与重聚令牌 RTL](docs/RTL_WARP_CONTROL.md)：每个实际使用的物理 warp 槽拥有 16 个令牌、1,568 位真实存储；EXIT/BRA/BSSY/BSYNC/BREAK/YIELD 的状态转换由电路产生。宿主令牌数组只供观察，实际 lane 状态现由下述前端直接提供。完整令牌子模块已通过综合和网表验证。

新增 [Warp 状态与选择前端 RTL](docs/RTL_WARP_FRONTEND.md)：每物理 warp 用 3,233 位存储持有 PC、运行状态、等待元数据和轮转游标，直接连接重聚单元。五层比较树选择最小/最大/轮转 PC 组，YIELD 恢复与普通发射 PC 推进由电路完成；CTA/异步等待通过专用表的原始完成信号唤醒。完整组合前端包含 4,801 FF 位，已通过综合网表验证，指令后备存储及部分后台控制仍有 C++。

新增 [4070 SUPER 访存实测](docs/MEMORY_CALIBRATION.md)：手写 CUDA/PTX 完成缓存依赖链、carveout、合并和 shared bank 实验，保存 118 组/590 个原始样本及 30 组实际模型探针。广播与冲突趋势有实测依据，但 shared 相对代价与模型不同；L2 容量辨识和 NCU sector 计数尚未完成，没有把这些曲线冒充完整硬件逆向。

新增 [DMA RTL](docs/RTL_DMA.md)：每 SM 64 命令槽、64 片段槽和 8 KiB 返回载荷存储，分片、地址/掩码、请求选择与完成资格由电路产生。片段在 shared 写确认后释放，命令在事务发布后释放；外部票据桥、返回响应选择和 shared bank 服务仍有 C++。完整容量实际综合为 400,338 generic cells / 87,547 FF 位。

新增 [跨 warp 调度 RTL](docs/RTL_WARP_SCHEDULER.md)：每分区 32 个物理槽、325 位持久状态，每拍最多检查一个候选。轮转/固定优先级、拒绝重试、在途数及边界/访存串行化由电路决定，退休和 CTA 释放经过实际时钟；指令后备存储与前端实例间信号路由仍有 C++，跨分区执行准入现由下述仲裁电路提供。完整实例综合为 7,625 generic cells / 325 FF 位，五项独立 RTL、sanitizer 与实际网表检查通过。

新增 [跨分区执行仲裁 RTL](docs/RTL_EXECUTION_ARBITER.md)：每 SM 八个物理分区、七类端口，使用硬件轮转选择 Tensor/异步候选，并保存 Tensor 归属与 24 个发射间隔计数器。完整实例为 6,360 generic cells / 778 FF 位；异步后台服务次序、指令后备存储和部分执行/访存仍有 C++。

新增 [原始机器码与操作数译码 RTL](docs/RTL_INSTRUCTION_DECODER.md)：每分区由原始 128 位指令驱动字段、合法位、执行类别与最多三个源请求，重复源合并和跨 lane 掩码由电路产生。实际执行不读取宿主预解码字段，完整综合为 1,941 个组合单元、零触发器；指令后备存储和前端实例间信号路由仍有 C++。

新增 [并行依赖查询 RTL](docs/RTL_DEPENDENCIES.md)：三个源、一个普通目标和两个谓词端口直接读取 scoreboard 同拍旁路状态，RAW/WAW/WAR 与谓词冲突的选择、归约和 ready 全部来自电路；C++ 不再逐个查询后短路合并。完整 scoreboard 为 764,130 generic cells / 14,368 FF 位，单独查询模块无持久状态。

新增 [指令缓存与取指 RTL](docs/RTL_INSTRUCTION_FETCH.md)：每 SM 八个独立 4 KiB 指令缓存，实际数据/tag 同步读取，四字整行补充和单通道轮转均由电路完成；发射使用缓存返回的原始机器字。完整实例为 564,062 generic cells / 276,530 FF 位，普通 RTL、sanitizer 和综合网表验证通过。后备指令存储仍为独立八拍模型，缓存目前不跨 kernel 保留；尚未接入数据 L2/DRAM 或完整 SM 顶层。

当前默认 Debug 运行 27 套件、279 项（277 通过，2 项未构建 RTL 而跳过）。当前联合 Release 构建通过 64 套件、974 项；本阶段另通过并行查询/完整 scoreboard 两模块共八项和实际适配器七项独立 ASan/UBSan 检查，未重跑完整联合 sanitizer 或整机综合。构建范围、实际结果与各阶段失败记录见 [验证记录](docs/VALIDATION.md)。不同构建包含既有场景复跑，独立 RTL 检查另计，各套件按其目的选择后端，并非全部同时启用所有 RTL。完整 RTL 连接、参数校准及最终硬件审计仍未完成，完整剩余目标以硬件计划为准。
