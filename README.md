# SIMT Lab

按 curryGPU 公开文章的分层思路，从指令集开始实现的可验证 GPU 功能模拟器。

项目使用 C++20 实现执行核心，Python 生成 ISA 编码接口、构造 kernel 和独立参考结果。默认设备含两个 SM，每个 warp 有 32 个逻辑线程。它运行在 CPU 上，模拟的是自定义机器的行为，不预测真实 GPU 周期、面积或功耗。

这里使用自己的指令编码与数值规范，借鉴作者公开的设计方法，不宣称兼容 curryGPU 的私有二进制、NVIDIA SASS 或完整 CUDA。ISS 和时序模型已运行；缓存、scoreboard、寄存器 bank 仲裁及 Tensor 数值/乘加阵列已有验证的 RTL。Tensor RTL 已可选接入 ISA/TMEM 和主机 Graph；完整 RTL GPU、PTX 编译器和 FPGA 实现仍未完成。

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

已验证 [原版 nvidia-smi 接入](docs/NVIDIA_SMI.md)：终端一运行 `python3 examples/nvml_device.py`，终端二运行 `python3 tools/simt_smi.py`，可查看实际模拟设备及其分配内存。专用 NVML 后端只作用于该子进程，系统默认命令仍查询真实显卡；没有 CUDA 或 PCIe 兼容。

[Local 与原子通路](docs/HARDWARE_ATOMIC_LOCAL.md) 已接入字交错物理映射、同一缓存/DRAM 层次，以及 global/shared 原子的 bank 预约、读取、ALU 延迟和写确认。`python3 examples/hardware_memory_ops.py` 可生成合并、缓存和原子竞争实验。自定义 59 个 opcode 均有时序路径，仍不表示支持 NVIDIA 的全部硬件。

[持久设备时序](docs/HARDWARE_HOST.md) 已将主机 CP、拷贝和 Graph 接入同一时钟，L1/L2 跨 kernel 保留。使用 `Device(timing=True)` 或 `python3 examples/hardware_host.py` 可执行带上传/下载的四类完整负载及 Graph 重放，并检查缓存、链路、故障排空和逐拍轨迹。

[关键模块 RTL](docs/RTL.md) 已实现 64 KiB sector 缓存的数据阵列和控制器、逐 lane scoreboard、4 bank 读写仲裁。原 14 项检查通过普通 RTL 仿真、ASan/UBSan 对照平台和综合后网表仿真；64 KiB 配置另外通过同样 9 项缓存检查及存储推断。

[Tensor RTL](docs/RTL_TENSOR.md) 新增四级 FP32 融合乘加、INT8 乘加、FP16/BF16/FP4 转换及完整 4×4 PE 网格，包含实际规格化/舍入电路、两项 FIFO、稀疏跳过和三阶段块缩放。新增 16 项 RTL、sanitizer 和综合后网表检查，当前 `cmake --build build --target rtl-check` 共 30 项。

[RTL 执行后端](docs/RTL_BACKEND.md) 已将该阵列接入实际 ISA/TMEM 与主机 Graph。以 `-DSIMT_TENSOR_RTL=ON` 构建后，运行 `python3 examples/hardware_host.py --build build-rtl --tensor-rtl`；每个 SM 拥有独立 RTL 实例，格式转换和乘加结果由电路产生，其余系统仍由 C++ 模拟。没有构建 RTL 却请求它会明确报错。

当前默认 Debug 通过 21 个 CTest 套件、259 项检查；RTL Release 与 ASan/UBSan 通过 24 套件、318 项检查（270 项 Python、48 项 C++），独立 RTL 检查另计。完整 RTL 连接、参数校准及最终硬件审计仍未完成，完整剩余目标以硬件计划为准。
