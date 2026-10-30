# 异步事务服务归属与 CTA 回收仲裁

`--timing --register-rtl --collector-rtl` 为每个 SM 启用一个 `async_service`。事务表由三种来源共享：前台 MB/CP_ASYNC 指令、DMA 完成发布、CTA 记录回收。当前所有者、等待计数与回收轮转游标由 RTL 保存；C++ 不再用 `transaction_retiring`、`transaction_released` 或任务到期时间决定 RTL 路径的准入和延迟。

这是项目自定义的单控制端口，不是 NVIDIA 内部服务调度器的逆向结果。它连接已有事务表和 DMA 电路，但没有把全部 shared bank、访存互联和 CTA 资源管理写成硬件。

## 状态和选择规则

| 寄存器 | 位数 | 含义 |
|---|---:|---|
| kind | 2 | 空闲、前台指令、DMA 完成、CTA 回收 |
| tag | 64 | 指令或 DMA 完整编号 |
| cta | 32 | 逻辑 CTA 编号 |
| slot | 6 | 回收所对应的物理 CTA 槽 |
| countdown | 32 | 当前控制请求的剩余等待 |
| retired | 1 | 回收完成但尚未确认 |
| cursor | 6 | 下次回收轮转起点 |
| 合计 | **143** | 同步复位、带写使能的触发器 |

实际综合为 **5,892 generic cells / 143 FF 位**。这是独立控制模块的工具计数，不包含事务 RAM、DMA 载荷、SM 或缓存，也不是 NAND2 等效门数、FPGA LUT 数或工艺面积。未执行布局布线与时序回标。

空闲或同拍释放所有者时，先选择已完成的 DMA，再选择待回收 CTA，最后允许一条前台指令进入。DMA 完成优先有助于释放拷贝命令额度。回收输入是 64 位请求掩码和 64 个完整 CTA 编号；在运行额度内从 cursor 起轮转，只有实际接受回收后才推进游标。物理支持 64 槽不表示其他资源允许同时驻留 64 个 CTA。

回收要求事务表 ready；若存在回收请求而表尚不可接收，则暂缓新前台指令。DMA 和前台指令可以先取得归属，等待配置的控制延迟后再请求表端口。它们从接受后的显式边沿开始计数：延迟 L 装载 L−1，此后每拍递减。只有归属已提交且计数为零，才能服务请求。一个 MB_WAIT 有多个不同地址时，保留同一个所有者并重新装载延迟，而不是重复取得归属。

优先级不是跨三类来源的全局公平保证。回收候选之间轮转；持续 DMA 完成可以推迟回收，持续回收也可以推迟新指令。现有有限任务与端到端队列压力测试验证了完成行为，没有宣称该策略对无限流量提供有界等待。

## 完成、回收与显式时钟

前台或 DMA 完成可以与下一来源的接受在同拍交接；完成计数仍属于旧所有者。CTA 回收完成只设置完成状态，继续持有归属，直到资源回收端显式确认，不能提前丢失逻辑 CTA 与物理槽的对应关系。

零额度、超过 64 的额度、零延迟、额度外回收位、过早完成、非法重试、错误确认与冲突事件均由电路拒绝。错误批次不修改所有者、游标或等待计数。复位可排空在途归属。

C++ 适配器提供请求连线、读出授权并路由事务表命令；查询、offer 和事件提交不产生隐藏时钟。接受后请求引脚保持到下一显式边沿，避免宿主稍后刷新输入时改变已选中的 DMA 或回收请求。这里的引脚保持不是第二份软件所有权状态。

整机在后台服务阶段及拍末 CTA 回收阶段刷新可服务请求。拍末接受的事件保留至下一拍提交，不能被下一轮 offer 覆盖。每 SM 每模型拍只调用一次控制器 tick。纯空闲时可停止状态时钟；未完成所有者和剩余计数仍按拍推进。

## 仍由 C++ 处理的部分

- 根据 CTA 的线程结束、在途指令、DMA 生命周期和已有记录生成可回收掩码。
- 保存指令操作数与错误/trace 描述符，分组 MB_WAIT 中的不同屏障地址。
- 连接事务表发布与 shared bank 请求，以及 DMA 外部读写票据和返回选择。
- 管理 CTA 的实际资源归还、跨模块信号调用、故障取消与排空。

因此本阶段完成的是事务服务的归属、延迟和来源仲裁，尚未形成完整 SM 顶层。C++ 默认后端继续使用原模型。完整硬件、参数校准和 PTX 异步矩阵协议的剩余目标见 [硬件计划](HARDWARE_PLAN.md)。

## 观察和验证

`hardware.async[].service` 报告真实所有者、CTA/槽、计数、游标、完成/等待状态、时钟，以及三类接受/完成事件。数组依次为前台、DMA、回收。成功执行后，各类接受等于完成，回收完成等于确认，所有者为空；事务表命令数等于三类接受总数加 MB_WAIT 重试数。这些关系已加入原异步执行测试。

```sh
cmake --build build-cache-rtl -j4
ctest --test-dir build-cache-rtl --output-on-failure
python3 tools/rtl.py --module async_service --out artifacts/rtl-async-service-final
python3 tools/rtl.py --module async_service --synth --out artifacts/rtl-async-service-final
python3 tools/rtl.py --module async_service --gate-sim --out artifacts/rtl-async-service-final
python3 tools/rtl.py --build build-cache-sanitize --module async_service --out artifacts/rtl-async-service-sanitize
python3 artifacts/rtl-async-service-final/build_adapter.py
```

四项独立电路检查在 RTL、sanitizer 与实际综合网表上均通过，覆盖全部 64 槽、两轮回收、延迟 1/3/17 与完整 32 位计数、反压保持、同拍交接、非法批次和在途复位。2,000 个随机任务使用独立队列和绝对截止时间作为参考。实际适配器另有三项 sanitizer 检查，验证显式时钟、快照、接受后引脚保持、错误撤回、运行额度和实例隔离。

本阶段全量回归、主机 Graph/NVML 和源码证据见 [验证记录](VALIDATION.md)。局部模块的综合和 sanitizer 不等于整机综合或整机 sanitizer。
