# 张量和异步事务规范

实现顺序遵循“先确定数学语义，再建立数据通路”。本项目的格式、tile 范围、描述符和同步规则公开定义，不能据此声称兼容真实 Tensor Core 的指令、布局、吞吐或数值细节。

本页主要描述现有**功能模式**及共同数值规则。`--timing` 已将 Tensor/TMEM 接入逐拍乘加阵列、真实内存供数和 TMEM bank 端口，见 [Tensor 硬件通路](HARDWARE_TENSOR.md)。CP_ASYNC/mbarrier 也已接入有限 DMA/事务控制器，见 [异步通路](HARDWARE_ASYNC.md)；尚未完成完整 NVIDIA Tensor Core 电路或 PTX 异步接口。

本项目另有独立可综合 [Tensor RTL](RTL_TENSOR.md)，已实现上述数值约定的浮点/整数电路和 4×4 PE 阵列，并与 C++ 模型进行对照；尚未替换本页 ISA 的执行后端。

## TMEM

每个 CTA 有独立的 128 行 × 256 列 TMEM，每单元 32 位；一列占 512 字节。物理字节地址为 `(column * 128 + row) * 4`。

以上为项目逻辑窗口。时序模式还施加每 SM 共享的物理列预算，并通过 bank 端口执行初始化和读写，详见 HARDWARE_TENSOR.md。

- TMEM_ALLOC rd, imm：恰好一个线程发起，分配 imm 列，数量必须为 8 的正整数倍且不超过 256；rd 返回首列编号。分配区域初始化为零。
- TMEM_FREE ra, imm：恰好一个线程发起，R[ra] 是原始首列，imm 必须等于原始分配大小。
- LDT rd, ra, rb：逐 lane 从 `(R[ra], R[rb])` 行列坐标读取单元。
- STT ra, rb, rc：逐 lane 向上述单元写入 R[rc]。

越界、未分配和不完整释放均报错。MMA 累加器可以跨 K tile 留在 TMEM。此版本提供显式行列访问，不实现作者列举的所有 LDT/STT shape。

## MMA 描述符

MMA ra 由一个线程发起，R[ra] 指向本 CTA shared 中 16 字节对齐的 80 字节描述符。记录由 20 个小端 uint32 构成，Python `MatrixDescriptor.words()` 和 `store_words()` 可构造它。

| word | 字段 | 含义 |
|---|---|---|
| 0 | magic | 0x31414D4D |
| 1 | format | 0 FP32、1 FP16、2 BF16、3 signed INT8、4 FP4 E2M1 |
| 2 | flags | bit0 累加已有 D，bit1 A 为 2:4 稀疏，bit2 块缩放；其余位必须零 |
| 3..5 | M, N, K | 1≤M,N≤32，1≤K≤256 |
| 6..7 | A, B | 源矩阵字节地址 |
| 8..9 | stride_A, stride_B | 行步长，以元素为单位；A 是 M×K，B 是 K×N |
| 10..11 | row_D, column_D | TMEM 中输出 tile 左上角坐标 |
| 12..13 | metadata, stride_metadata | 稀疏元数据基址、每行字节步长 |
| 14..15 | scale_A, scale_B | binary32 缩放矩阵基址 |
| 16 | block_K | 沿 K 的块大小，要求整除 K |
| 17..18 | stride_scale_A, stride_scale_B | 缩放矩阵每行元素步长 |
| 19 | space | 0 从 shared 读取矩阵、元数据及缩放；1 从 global 读取这些数据 |

功能模式在整个结果 tile 完成计算后统一写入 TMEM；时序模式按微 tile 经 bank 端口写入，指令退休表示全部写入完成，不提供整个 tile 的中途原子可见性。普通程序须用 CTA 屏障在生产、MMA 和多 warp drain 之间建立执行同步。

## 数值规则

FP16、BF16 精确提升到 binary32，保留次正规数；FP32 直接使用原始位模式。浮点 dense MMA 按 K 递增顺序执行 binary32 融合乘加，最近偶数舍入；初值为零或已有 D，NaN 输出统一为 `0x7fc00000`。不模拟某一代 NVIDIA Tensor Core 的内部累加精度或 flush-to-zero 模式。

INT8 解释为有符号 8 位，乘积精确，累加按 2^32 取模，D 保留整数位模式。INT8 路径不接受块缩放。

FP4 的低三位映射幅度 `[0, 0.5, 1, 1.5, 2, 3, 4, 6]`，高位表示符号；每字节先低半字节，再高半字节。Python 输入打包器最近偶数舍入并饱和到最大有限幅度，拒绝非有限输入。

## 2:4 与块缩放

稀疏 A 的逻辑 K 必须是 4 的倍数，每组仅存两个保留值，A 行步长针对压缩后的 K/2 元素。每组一个 metadata 字节：bits1:0 为 pos0、bits3:2 为 pos1，要求 `0≤pos0<pos1≤3`，高四位为零。实际 B 索引为 `group*4+pos`，不能简单使用前 K/2 行 B。省略元素不参与乘加，因此含 Inf/NaN 的隐式零乘积不等同于显式 dense 零乘积。

缩放矩阵逻辑形状为 `scale_A[M, K/block_K]`、`scale_B[K/block_K, N]`。每个系数为有限、非负 binary32。每块先从零进行按 K 排序的融合点积，再舍入 `scale_A * scale_B` 到 binary32，最后一次融合乘加 `D = fma(block_dot, scale_product, D)`。稀疏与缩放可组合，要求 block_K 也是 4 的倍数。

这里实现 FP4 数据与通用 binary32 块系数，尚未实现完整 MXFP4/NVFP4 的 E8M0/E4M3 缩放编码及独立 tensor 级 scale；不能用这些测试声称完整支持它们。

## 异步搬运与 mbarrier

MB_INIT ra, imm 在 R[ra] 指向的 shared 地址初始化 32 字节记录，地址须 16 字节对齐，imm 是每阶段到达次数 1..1024。该区域成为不透明记录，普通 store、atomic、DMA 目的区域都不能覆盖它。它独立于 BSSY 重聚和 BAR_SYNC 命名屏障。

- MB_EXPECT ra, imm：增加本阶段预期搬运字节，须在到达可能完成阶段之前执行。
- MB_ARRIVE rd, ra：消耗一次到达，rd 返回到达前的阶段编号。
- CP_ASYNC ra, rb, rc, imm：从 global 的 R[ra] 搬 imm 字节到 shared 的 R[rb]，关联 R[rc] 的事务屏障；大小 1..4096 且不能超出尚未发起的预期字节。
- MB_WAIT ra, rb：R[ra] 是屏障地址，R[rb] 是阶段令牌；该阶段尚未完成时阻塞，未来令牌报错。

管理和搬运命令要求一个参与线程，WAIT 可由多个线程执行。功能模式每次搬运在发起时捕获源数据，经过三次调度循环服务机会后提交目标字节，再完成事务字节计数。这是可复现的功能事件延迟，不是三个硬件周期。时序模式通过实际 global/cache 请求取得载荷，再经 shared banks 分片写入；无同步源覆盖不保证与功能快照一致。两种模式中，即使所有线程暂时阻塞，事件仍会推进；退出后的 CTA 也会等待已发起搬运完成。

当到达次数和预期字节都归零，阶段编号递增，到达计数重置，等待旧阶段的线程被释放。重复阶段需传播新的令牌，例如用 SHFL 或 shared + BAR_SYNC。仅完成线程到达不会掩盖丢失的事务字节。

记录中的八个 word 为 magic `0x3152424D`、phase、expected arrivals、remaining arrivals、remaining bytes、issued outstanding bytes、0、0，可通过 shared 快照审查。

功能模式 MMA 在一条功能指令中同步计算；时序模式的逐拍 Tensor 单元对发起 warp 仍提供阻塞完成语义。异步搬运现已有功能和时序实现，但不声称实现 TMA descriptor、多维 DMA 或 PTX 的异步 MMA/commit 协议。
