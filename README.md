# SIMT Lab

按 curryGPU 公开文章的分层思路，从指令集开始实现的可验证 GPU 功能模拟器。

项目使用 C++20 实现执行核心，Python 生成 ISA 编码接口、构造 kernel 和独立参考结果。默认设备含两个 SM，每个 warp 有 32 个逻辑线程。它运行在 CPU 上，模拟的是自定义机器的行为，不预测真实 GPU 周期、面积或功耗。

这里使用自己的指令编码与数值规范，借鉴作者公开的设计方法，不宣称兼容 curryGPU 的私有二进制、NVIDIA SASS 或完整 CUDA。第一条交付路线是可运行的 ISS；RTL、PTX 编译器和 FPGA 实现属于后续项目。

开发阶段、验收条件和当前进展见 [实施计划](docs/PLAN.md) 与 [开发记录](docs/DEVELOPMENT.md)。每个通过验收的阶段单独 Git 提交。

## 构建与运行

依赖 C++20 编译器、CMake、Python 3；不需要 CUDA 或 Python 第三方包。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
python3 examples/identifiers.py
./build/simt-run build/identifiers.bin --sms 2 --blocks 5 --threads 37
python3 examples/run_workloads.py
```

加 `--dump-state` 可以查看每个 CTA/warp 的寄存器和谓词。程序输出 JSON，失败返回退出码 2，并报告 block、SM、warp 与 PC。编码规则和当前实现边界见 [ISA 规范](docs/ISA.md)。

已实现独立线程调度、分支重聚、warp 集合操作、双 SM 的多 CTA/warp 内存与屏障，以及 TMEM、五种数据格式的 MMA、2:4 稀疏、块缩放和异步搬运事务。提供向量、规约、分块 GEMM 和小型 Transformer block 的机器码示例与独立参考。

先看 [从示例读懂模拟器](docs/WALKTHROUGH.md)，再查 [内存规范](docs/MEMORY.md) 和 [张量规范](docs/TENSOR.md)。示例产物写入 `artifacts/examples/`，其中包含可阅读汇编、机器码、启动参数及验证结果。

## 路线依据

- [ISA](https://zhuanlan.zhihu.com/p/2044711939746566963)：先定义机器状态与指令语义。
- [ISS](https://zhuanlan.zhihu.com/p/2046857919606661244)：编码表、C++ 执行核、Python 构造器与分层验收。
- [独立线程调度](https://zhuanlan.zhihu.com/p/2047985048213780000)：per-lane PC、按 PC 分组与重聚。
- [内存同步](https://zhuanlan.zhihu.com/p/2048501401848903199)：多 warp 调度、分离地址空间和屏障。
- [MMA](https://zhuanlan.zhihu.com/p/2056027770405270973)：矩阵语义、TMEM、稀疏元数据与块缩放。

本目录是独立 Git 仓库。上级目录保存的网页和讨论文档不加入此仓库。
