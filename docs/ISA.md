# 功能 ISA 规范

本项目使用自定义 ISA。编码唯一来源是 `isa/isa.json`，CMake 在构建目录生成 C++ 解码器和 Python 编码器。

## 指令与编码

每条指令 16 字节，四个 little-endian 32 位字。程序文件是原始指令数组，PC 使用指令索引，不使用字节地址。机器加载时解码一次。未知 opcode、保留位或未使用字段非零都被拒绝。

| 字 | 位段 |
|---|---|
| 0 | opcode[7:0], rd[15:8], ra[23:16], rb[31:24] |
| 1 | rc[7:0], guard[11:8], aux[19:12], mode[27:20]，其余保留 |
| 2 | imm[31:0] |
| 3 | control[31:11]，其余保留 |

21 位 control 保留在解码结果中，功能层忽略其时序效果。`guard` 低三位指定 P0..P7，高位表示取反。P7 永远为真，因此默认 guard=7，guard=15 永远不执行。每条指令参与集合为当前 PC group 与 guard 的交集；guard 为假的 lane 仍推进 PC，但没有操作副作用。

## 机器状态与启动

- 一个 warp 固定 32 lane。最后一个 warp 可以不足 32 个有效线程。
- R0..R62 是 32 位普通寄存器，R63 恒为零，写入丢弃。P0..P6 为逐 lane 谓词，P7 只读。
- 未初始化通用寄存器与 P0..P6 在启动时为零。不能依赖真实 GPU 也采取此初始化方式。
- `--sms` 默认 2；grid 为一维，`--blocks` 指 CTA 数，`--threads` 指每 CTA 线程数。
- 当前每个 SM 同时驻留一个 CTA，CTA 结束后派发下一项；多个 SM 和多个 warp 交错执行，不是将整个 CTA 串行运行到底。
- 外层 SM 和 warp 使用轮转调度。PC group 可选 `rr`、`min`、`max`；这不是硬件周期调度策略。
- `instruction_steps` 计每次 group 指令执行，包含空谓词指令；`lane_instructions` 计真正参与的 lane。
- kernel 必须让所有有效线程执行 EXIT；落出代码范围报错。

## 基础指令

完整 opcode 和操作数字段见编码表。整数运算使用 32 位模运算；移位量只取低 5 位。ISETP mode 为 eq、ne、unsigned lt、unsigned le、signed lt、signed le，依次编号 0..5。FSETP mode 为 eq、ne、lt、le，编号 0..3；NaN 比较遵循宿主 IEEE 比较规则。

S2R 的 imm 取值：0=lane ID，1=CTA 内 thread ID，2=warp ID，3=block ID，4=SM ID，5=block 线程数，6=grid block 数。

MOVI 写入原始位模式；Python 的 `f32(value)` 用于构造浮点位模式。FADD/FMUL 使用 binary32，FFMA 使用融合的 binary32 乘加，舍入为最近偶数；NaN 结果规范化为 `0x7fc00000`。FRCP、FRSQRT 按所列运算步骤执行；FEXP2 采用宿主数学库，不能承诺跨平台逐位相同。I2F 按有符号整数转换；F2I 向零截断，对非有限或越界结果报错，不调用未定义的 C++ 转换。

指令从执行前的 warp 状态读取源操作数，计算完成后统一提交寄存器状态，避免遍历 lane 顺序形成隐式通信。

## 分支与独立线程调度

每个 lane 保存自己的 PC 和 active/reconvergence/yielded/exited 状态。调度器只合并当前 PC 相同的活跃线程，不把“处于同一个 warp”视为自动同步。

- BRA imm：guard 为真的 lane 跳转到绝对指令索引。
- BSSY rd, imm：初始化 0..15 号 warp 内重聚令牌，记录当前 PC group 的成员；imm 必须指向同一令牌的 BSYNC。
- BSYNC rd：到达成员阻塞，所有剩余成员到齐后一起推进到 BSYNC 的下一条指令。BSSY/BSYNC 必须不带谓词。活动令牌不能被覆盖。
- BREAK rd：guard 为真的线程退出指定令牌的参与集合；不自动分支。全部成员 BREAK 后令牌变成 dissolved，此时原目标 BSYNC 可以直接通过。
- EXIT：退出线程同时从全部重聚令牌移除，可能释放正在等待的其他线程。
- YIELD：只提供调度提示，在本 warp 内最多让出一次选择机会；不提供内存同步。这里是本项目定义的功能语义，不承诺模拟硬件调度细节。

令牌属于 warp，不能代替跨 warp 的 CTA 屏障。没有任何线程可运行时报告死锁；仍有线程循环执行时由 max-steps 限制终止。

## Warp 集合操作

ELECT/VOTE/SHFL 的 imm 为成员掩码；非空执行时，必须非零且是当前 guard 为真线程集合的子集。所有指定成员必须同时执行这一动态指令，否则报错，不隐式等待分支另一侧。结果只写成员线程，其他线程正常推进 PC。

| 指令 | 语义 |
|---|---|
| ELECT rd, imm | 将成员中编号最小的 lane 的目标谓词设为真，其他成员设为假 |
| VOTE rd, ra, imm, mode | ra 是源谓词编号；mode 0=ballot、1=any、2=all、3=所有成员投票相同；结果写普通寄存器 |
| SHFL rd, ra, rb, imm | 每个成员从自身 rb 取得源 lane 索引，从该成员的 ra 读取执行前值；源 lane 必须属于成员集合 |
| ACTIVEMASK rd | 写入谓词筛选前的当前 PC group 掩码，不能把它当作未来执行点的成员保证 |

## 当前边界

阶段 2 尚不包含访存、CTA barrier 和 MMA；这些按实施计划继续加入。程序不是 PTX，不能直接装载 CUDA cubin。执行步数不表示延迟，两个模拟 SM 不意味着宿主 CPU 必须启动两个 OS 线程。
