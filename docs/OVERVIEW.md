# SIMT Lab 项目结构概览

SIMT Lab 是一个自定义 GPU 的 **C++ / RTL 混合模拟器**，按 curryGPU 公开文章的分层思路从指令集开始逐层实现。它不兼容 NVIDIA SASS、完整 CUDA 或 PCIe，也不等同于 4070 SUPER 或 SM100 的真实电路。默认设备为 2 个 SM，每个 warp 32 个线程。

## 1. 分层总览

```mermaid
flowchart TB
    subgraph Host["主机层 (Python)"]
        PY[python/simt_lab<br/>Program 构造器 / Device / Graph]
    end
    subgraph Dev["设备进程 simt-device"]
        CP[CP：门铃 + 解码槽<br/>可选 RTL]
        ENG[计算引擎 / 拷贝引擎<br/>stream · event · Graph]
    end
    subgraph Core["执行核心 (C++20)"]
        ISS[ISS 功能模拟<br/>src/*.cpp]
        TIM[微架构时序模型<br/>src/timing + src/hardware]
    end
    subgraph RTL["可综合 RTL (SystemVerilog)"]
        R[rtl/*.sv<br/>Verilator 仿真 · Yosys 综合]
    end
    NV[NVML 桥接<br/>src/nvml]

    PY -->|命令包| CP --> ENG
    ENG --> ISS
    ENG --> TIM
    TIM <-->|可选接入| R
    ENG -.状态.-> NV
    NV --> SMI[原版 nvidia-smi]
```

| 层 | 目录 | 作用 |
|---|---|---|
| ISA 定义 | `isa/isa.json`, `tools/` | 唯一的指令编码来源，生成 Python 编码器与 C++ 解码器 |
| ISS 功能模拟 | `src/*.cpp`, `include/simt/` | 正确性基准：线程调度、重聚、内存、原子、屏障、MMA、异步事务 |
| 微架构时序 | `src/timing`, `src/hardware` | 逐周期模型：缓存、scoreboard、流水线、DMA、Tensor 阵列 |
| RTL | `rtl/`, `src/rtl/`, `tests/rtl/` | 关键模块的可综合实现，通过 C++ 适配层替换对应时序模块 |
| 主机与设备 | `python/`, `src/device*.cpp` | 持久设备进程、命令环、CP、拷贝、Graph |
| 监控 | `src/nvml`, `src/monitor.cpp` | 让原版 `nvidia-smi` 看到模拟器 |

## 2. 一条指令的路径

```mermaid
flowchart LR
    ISA[isa.json] --> GEN[gen_isa.py]
    GEN --> ENC[Python 编码器]
    GEN --> DEC[C++ 解码器]
    K[kernel 构造器] --> ENC --> BIN[128 位机器码]
    BIN --> DEC --> M[Machine<br/>SM → CTA → warp → PC 组]
    M --> OUT[寄存器 / 内存快照]
    OUT --> REF[Python 独立参考比较]
```

功能模式（ISS）和 `--timing` 微架构模式共用同一份机器码与同一套验收负载（向量、规约、分块 GEMM、小型 Transformer）。

## 3. SM 微架构

```mermaid
flowchart LR
    IF[指令取指<br/>I-cache] --> DEC[译码] --> WF[Warp 前端<br/>PC/状态/重聚令牌]
    WF --> SCH[Warp 调度器] --> SB[Scoreboard<br/>依赖检查]
    SB --> OC[Operand Collector<br/>+ 寄存器 bank 仲裁]
    OC --> ARB[执行仲裁]
    ARB --> INT[整数]
    ARB --> FP[浮点 FMA]
    ARB --> SFU[SFU]
    ARB --> COL[Warp 集合<br/>SHFL/VOTE]
    ARB --> TEN[Tensor 阵列<br/>4×4 乘加]
    ARB --> LSU[访存 / 原子]
    ARB --> DMA[异步 DMA]
    INT & FP & SFU & COL & TEN & LSU & DMA --> WB[写回仲裁]
    WB --> OC
    BAR[CTA 屏障 / 事务表] <--> WF
    RES[CTA 资源准入与退休] --> SCH
```

## 4. 存储层次

```mermaid
flowchart LR
    LSU[LSU / 合并器] --> L1[L1 sector cache<br/>MSHR + 反压]
    LSU --> SH[Shared Memory<br/>32 bank + 原子]
    TC[Tensor 阵列] <--> TMEM[TMEM<br/>128 KiB / SM]
    DMA[DMA] --> SH
    L1 --> L2[共享 L2]
    IFC[指令缓存] --> L2
    L2 --> DRAM[DRAM<br/>bank / row / 总线时序]
    DMA --> L2
```

## 5. 主机路径与 nvidia-smi

```mermaid
sequenceDiagram
    participant P as Python Device
    participant R as 命令环 / doorbell
    participant CP as 软件 CP
    participant E as 计算/拷贝引擎
    participant S as SM 模型
    participant N as NVML 桥接
    participant X as nvidia-smi
    P->>R: 提交命令包 (或 Graph 重放)
    R->>CP: doorbell
    CP->>E: 调度到 stream
    E->>S: 启动 kernel / 拷贝数据
    S-->>E: 完成状态
    E-->>P: event / 完成
    E-->>N: 内存、进程、状态
    X->>N: 查询
    N-->>X: 真实 GPU + 模拟设备
```

`nvidia-smi` 只通过用户级入口走 NVML 桥接，不修改系统二进制或驱动。CUDA / PyTorch 不会识别出这个设备。

## 6. 实现状态

| 模块 | 状态 |
|---|---|
| ISA、ISS、重聚、屏障、MMA、异步事务 | 完成 |
| 主机、Graph、持久设备 | 功能完成 |
| 缓存、scoreboard、寄存器、collector、整数/浮点/集合执行、warp 调度、CTA 屏障、DMA、事务表、shared、CTA 资源 | 有 RTL，可选接入 |
| Tensor 4×4 乘加阵列 | 自研 RTL，非 NVIDIA 精确复刻 |
| TMEM | RTL：每 SM 128 KiB 共享存储和页映射，可选接入 |
| 命令处理器（门铃、取包、解码槽、依赖、引擎占用） | RTL，可选接入；链路和拷贝数据通路仍为 C++ |
| DRAM、SFU、部分互联 | 仍为 C++ 模型 |
| 完整 GPU 顶层、FPGA 实现、PTX 编译器、CUDA/PCIe 兼容 | 未完成 |

## 7. 常用入口

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
python3 examples/run_workloads.py --timing --out artifacts/timed   # 四类负载，微架构模式
python3 examples/nvml_device.py --timing --seconds 120             # 配合另一终端 nvidia-smi -L
```

各结构的实际容量、bank 数、端口和流水线深度见 [PHYSICAL.md](PHYSICAL.md)。

延伸阅读：`WALKTHROUGH.md`（示例）、`HARDWARE_PLAN.md`（阶段与剩余工作）、`RTL.md`（RTL 工具链）、`VALIDATION.md`（验证记录）。
