# SIMT Lab

按 curryGPU 公开文章的分层思路，从指令集开始实现的可验证 GPU 功能模拟器。

项目使用 C++20 实现执行核心，Python 生成 ISA 编码接口、构造 kernel 和独立参考结果。默认设备含两个 SM，每个 warp 有 32 个逻辑线程。它运行在 CPU 上，模拟的是自定义机器的行为，不预测真实 GPU 周期、面积或功耗。

这里使用自己的指令编码与数值规范，借鉴作者公开的设计方法，不宣称兼容 curryGPU 的私有二进制、NVIDIA SASS 或完整 CUDA。第一条交付路线是可运行的 ISS；RTL、PTX 编译器和 FPGA 实现属于后续项目。

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

最终验证覆盖 Debug、Release 和 ASan/UBSan 三种构建，每种构建的九个套件、75 项测试全部通过。命令、证据映射与未实现范围见 [验证记录](docs/VALIDATION.md)。

## 路线依据

- [ISA](https://zhuanlan.zhihu.com/p/2044711939746566963)：先定义机器状态与指令语义。
- [ISS](https://zhuanlan.zhihu.com/p/2046857919606661244)：编码表、C++ 执行核、Python 构造器与分层验收。
- [独立线程调度](https://zhuanlan.zhihu.com/p/2047985048213780000)：per-lane PC、按 PC 分组与重聚。
- [内存同步](https://zhuanlan.zhihu.com/p/2048501401848903199)：多 warp 调度、分离地址空间和屏障。
- [MMA](https://zhuanlan.zhihu.com/p/2056027770405270973)：矩阵语义、TMEM、稀疏元数据与块缩放。

本目录是独立 Git 仓库。上级目录保存的网页和讨论文档不加入此仓库。

## 进行中的硬件模型扩展

用户要求进一步实现内部硬件结构和原版 nvidia-smi 集成，完整目标与剩余验收见 [硬件计划](docs/HARDWARE_PLAN.md)。已新增 [逐周期存储单元](docs/HARDWARE_MEMORY.md)：真实 sector cache 数据、MSHR/反压、shared banks、DRAM bank/row 时序及双 SM 存储层次，三种构建下 17 项新增检查通过。

这些单元目前由独立测试驱动，尚未接入上面的功能执行器；SM/Tensor 流水线和 RTL 仍在后续工作中。这里的阶段进展不等于已经完成硬件复刻。

已验证 [原版 nvidia-smi 接入](docs/NVIDIA_SMI.md)：终端一运行 `python3 examples/nvml_device.py`，终端二运行 `python3 tools/simt_smi.py`，可查看实际模拟设备及其分配内存。专用 NVML 后端只作用于该子进程，系统默认命令仍查询真实显卡；没有 CUDA 或 PCIe 兼容。

当前三种构建均通过 11 个 CTest 套件，共 81 项 Python 检查和 17 项硬件 C++ 检查。完整硬件目标仍在推进，剩余工作以硬件计划为准。
