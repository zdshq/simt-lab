# 内存与同步规范

这里采用确定调度下的串行提交模型。它能检查功能、通信和同步错误，不建模 cache、合并访问、bank conflict、访存延迟或弱内存允许的全部结果。

## 地址空间

| 空间 | 归属 | 范围 | 指令 |
|---|---|---|---|
| global | 全 grid 共用 | 4 GiB | LDG/STG |
| shared | 每 CTA 独立 | 64 KiB | LDS/STS |
| local | 每线程独立 | 16 KiB | LDL/STL |
| constant | 全 grid 共用、设备只读 | 64 KiB | LDC |

按 4 KiB 页稀疏分配；未写字节读取为零且不分配页。`--dump-memory` 输出地址升序排列的 `[地址, 非零字节]`，写零后的快照与未写相同。保留已完成 CTA 的 shared/local 快照供检查。

`--global FILE` 和 `--constant FILE` 从地址 0 装载原始二进制镜像。每份镜像最多 64 MiB，并受所在空间上限约束；这项宿主输入限制不缩小 global 的稀疏地址范围。

## Load/store 和地址转换

地址为 `R[ra] + signed32(imm)`，中间计算溢出 32 位地址范围时报告错误。mode 为 0=U8、1=S8、2=U16、3=S16、4=U32；store 仅采用对应宽度的低位，load 完整扩展为 32 位。要求自然对齐，按小端排列。load 写 rd，store 从 rb 取值。

LD/ST 按窗口选择空间：`0xF0000000 .. +64KiB` 映射当前 CTA shared，`0xF1000000 .. +16KiB` 映射当前线程 local；其他地址映射 global。直达指令始终使用指定空间，绕过通用窗口。

CVTA 的 mode 0/1 将 shared/local offset 转成通用地址，2/3 做反向转换；超出窗口范围报错。转换本身不访问内存。

普通指令从执行前寄存器读取全部源；store 先校验所有地址，再按 lane 编号升序提交。同一指令同址普通写入由最高 lane 覆盖，这是确定执行约定，不能据此把有数据竞争的程序视为正确同步的程序。

## 整数原子

ATOM 的 rd 接收旧值，RED 不返回值。ra+imm 为地址，rb 为值或 CAS 期望值，rc 为 CAS 替换值；aux=0 选择 global，aux=1 选择 shared。固定 32 位且自然对齐。

mode：0 add、1 exchange、2 compare-exchange、3 and、4 or、5 xor、6 unsigned min、7 unsigned max、8 signed min、9 signed max。按 lane 编号升序逐个完成整个读改写，不能让所有 lane 先读同一个旧值。

原子加最终和可跨调度顺序比较；旧值分配和 exchange/CAS 的逐线程结果可能依赖调度。票号测试检查旧值集合唯一完整，不错误要求每线程票号跨调度一致。

## CTA 命名屏障

BAR_SYNC rd, imm 表示到达并等待；BAR_ARRIVE rd, imm 表示到达后继续。rd 取 0..15，独立于 warp 重聚令牌。屏障按线程 ID 记录到达，同一个 warp 可从不同 PC 到达。

- imm=0 为动态整个 CTA 屏障；退出且未到达的线程从期望集合排除，已到达后退出的线程保留本次贡献。
- imm>0 为显式静态线程计数，不因 EXIT 减少；同一阶段必须使用相同计数。
- 同一线程同阶段重复到达、数量溢出、不同计数混用都报错。
- 完成后释放在此屏障等待的线程，保留各自的后续 PC，然后推进阶段，允许复用屏障编号。

FENCE 在此顺序一致功能模型中不改变状态；mode=0/1/2 表示 CTA/device/system 范围，aux=0/1/2 表示 acquire/release/acq_rel。它不能替代 BAR_SYNC 的执行等待。

`--warp-policy rr|min|max` 独立于 PC-group 的 `--policy`。rr 每次只执行一个 group 指令后轮换 warp，保障同 CTA 的自旋消费者不会独占执行器。min/max 是固定优先级验证策略，不保证活跃自旋中的公平性。无可运行线程的错误报告包含 block、SM、warp、lane、PC、等待类型和令牌。
