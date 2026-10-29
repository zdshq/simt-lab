# 开发记录

## 阶段 0：建立工程边界（2026-10-29）

- 根据本地 curryGPU 文章，采用先功能、后硬件的路线，本次实现功能级 ISS。
- 在 `simt-lab/` 建立独立仓库，保留上级已有资料。
- 默认目标为两个 SM、32 lane/warp；执行器使用 C++20，编码生成和参考测试使用 Python 标准库。
- 当前环境具备 GCC 11.4、CMake 3.22、Ninja 和 Python 3.13，无需先安装依赖。
- 第一阶段先验证编码与基础执行，后续按计划扩大机器状态和指令语义。

## 阶段 1：编码与可运行执行器（2026-10-29）

- 实现 25 条基础指令，统一 JSON 描述生成 C++ 解码表与 Python 编码器；128 位小端编码，保留位严格校验。
- 执行器支持可配置 SM 数、CTA 派发、多 warp 和部分有效 warp；默认两个 SM，每 SM 一个驻留 CTA。
- 实现整数、谓词、基础浮点与融合乘加；结构化 JSON 结果包含实际 SM 派发和架构状态。
- 收到用户 `$coding-standards` 要求后，先将执行、调度、转储、CLI、生成器拆成短函数，补齐相邻中英文注释；项目 AGENTS.md 固化约定。
- 验证：`cmake --build build` 成功；`ctest --test-dir build -V` 的 12 项测试通过，包括 1,000 次编码往返、80 个逐 lane 整数差分程序、融合舍入反例和错误诊断。
- 示例：`python3 examples/identifiers.py` 后运行 `./build/simt-run build/identifiers.bin --sms 2 --blocks 5 --threads 37`，5 个 CTA 完成，SM 分布为 0、1、0、1、0。
- 此阶段仍无跳转重聚、内存或 MMA，下一阶段验证分歧与线程集合的正确性。

## 阶段 2：独立线程调度与重聚（2026-10-29）

- 新增 BRA、BSSY、BSYNC、BREAK、YIELD 和四种 warp 集合操作，总计 34 个 opcode。
- 重聚令牌分别保存参与和到达集合；EXIT 清理全部参与关系，BREAK 仅清理指定令牌，支持嵌套分支及逐 lane 退出循环。
- 明确集合操作的成员约束和 YIELD 的功能语义，拒绝未初始化令牌、活动令牌覆盖、错误汇合 PC 和缺席成员。
- 控制、集合、逐 lane 算术保持独立模块，按短函数及相邻中英文注释规范实现。
- 验证：`cmake --build build && ctest --test-dir build --output-on-failure` 通过两个测试套件，共 21 项测试；编码随机往返随 ISA 扩展为 1,360 次。新增 9 项行为测试覆盖三种调度策略下的嵌套重聚、31 次逐 lane 循环、提前退出、集合操作和死锁。

## 阶段 3：内存、原子与多 warp 同步（2026-10-29）

- 新增 15 个 opcode，共 49 个；4 KiB 稀疏字节页，明确 global/shared/local/constant 归属、通用窗口和子字扩展。
- 接入宿主二进制镜像与规范化非零字节快照；覆盖接近 4 GiB 的地址，未申请扁平 4 GiB 数组。
- 实现十种整数原子模式，按 lane 顺序完整读改写；CTA 屏障记录逐线程到达和可复用阶段，动态和静态计数严格区分。
- 新增 warp 选择策略和具体死锁位置。生产者/消费者测试验证默认轮转在自旋存在时仍能前进。
- 验证：`cmake --build build && ctest --test-dir build --output-on-failure` 三个套件共 32 项测试全部通过；新增 11 项测试包括 5 CTA × 37 线程规约、185 个唯一原子旧值、空间隔离、分歧到达、重复到达及早退负例。ISA 随机往返为 1,960 次。

## 阶段 4：张量数据通路和异步事务（2026-10-29）

- 新增 10 个 opcode，共 59 个。实现 per-CTA TMEM 列分配、坐标读写、80 字节共享描述符和完整 tile 提交。
- 实现 FP32/FP16/BF16/INT8/FP4 E2M1，2:4 真实逻辑 K 还原、沿 K 块缩放和保留 D 累加。明确不宣称完整 MX/NVFP4 编码或硬件精度兼容。
- 数值参考独立用 Python Fraction 计算精确乘加并实现最近偶数 binary32 舍入，单独检验中点、次正规数和溢出。随机矩阵使用非紧密步长，比较量化后的输入实际字节。
- 异步 global→shared 搬运通过事件队列延迟提交；mbarrier 保存到达与字节计数，反映到受保护的 shared 记录。CTA 全阻塞或已 EXIT 时，已发起事件仍会完成。
- 验证：五个套件共 48 项测试，包括 15 个随机 dense 矩阵、全部六种 2:4 位置、稀疏与非平凡缩放舍入组合、跨 warp DMA→MMA，以及丢失事务死锁、记录覆盖和未来令牌负例。ISA 往返为 2,360 次。

## 阶段 5：组合 kernel 与可复现示例（2026-10-29）

- 实现向量加、两层规约、三个 CTA 的 FP16 GEMM，以及含 LayerNorm、单头注意力、softmax、FFN 和两条残差路径的小型 Transformer block。
- 示例发射真实指令，全部中间张量在 C++ 模拟器内计算；独立参考使用普通标量数学。Transformer 使用 8 次有效 MMA，两 warp 的指令组计数为 16，文档明确区分。
- 每次示例保存二进制、输入、汇编清单、启动参数、完整内存结果和验证摘要。新增架构图与阅读路线。
- 验证：`ctest --test-dir build --output-on-failure` 六个套件共 52 项测试通过；工作负载在 rr/min/max 的 PC 与 warp 策略下输出一致。`python3 examples/run_workloads.py` 全部通过，向量/整数规约误差 0，GEMM 最大绝对误差 2.980232238769531e-7，Transformer 为 1.0707641351359598e-7。
- Python AST 检查用于本次人工审查：当前最长函数 22 行，没有添加样式计数测试。

## 阶段 6：最终验收（2026-10-29）

- Debug、Release、ASan+UBSan 三套独立构建的六个测试套件全部通过，每套 52 项 unittest；sanitizer 未报告错误。
- 审查地址范围、稀疏索引、舍入、异步完成时的 CTA 释放、谓词空执行和计数语义。实现范围与未支持项在 VALIDATION.md 中逐项列明。
- 检查源码及生成模板的解释性注释，未发现缺失对应中文的英文注释；C++ 最长函数 24 行，Python 最长 22 行。未新增针对行数或注释格式的测试。
- 根据实际类型注解要求，将 Python 最低版本明确为 3.10。保存构建、测试、组合示例命令及需求证据映射。
- 完成阶段 0 至 6 的交付，保留独立仓库历史，不推送远程。生成产物保存在被忽略的 build*/ 和 artifacts/ 中。

## 阶段 7：持久设备与 CP 后端（2026-10-29）

- 根据用户继续完成主机链路与 Graph 的请求，新增 HOST_PLAN.md。保持自定义功能接口边界，不宣称 CUDA ABI 或 PCIe 设备兼容。
- `simt-device` 持有持久 global 内存、模块、环形提交队列、doorbell、任务状态和完成通知。原 Machine 支持逐步推进及共享内存，原单次 CLI 保持可用。
- 一个计算引擎使用设备配置的 SM，一个拷贝引擎独立按字节块推进；stream 与显式依赖等待完成状态，不把出队误认为完成。
- 加入资源保留、释放时忙碌检查、错误传播、有界等待与跟踪；后端同时支持经 DAG 验证的图实例、内部节点展开和参数更新。
- 验证：原 52 项行为测试保持通过；新增三个不依赖 Python 主机封装的协议测试，覆盖发布/取走/完成区别、失败请求无部分提交、单包图展开。

## 阶段 8：主机接口与 stream/event（2026-10-29）

- Python 主机接口连接同一个持久 C++ 进程，提供内存、模块、kernel 参数、H2D/D2H/D2D、memset、stream/event 和完成查询。
- 指针句柄保存设备归属，提交自动声明缓冲区引用；待执行资源不能释放，释放后的句柄即使地址复用也不能再次使用。
- 区分源 bytes 快照、异步提交、显式推进和完成读回。跨 stream 依赖等待完成；独立 copy 与 compute 引擎可交错推进。
- 新增九项主机行为测试，覆盖队列满与环绕、两个 SM 上多次 kernel 的持久内存、资源生命周期、重叠执行、跨 stream 事件、错误传播、超时恢复和设备归属。
- 验证：Debug 构建的主机接口与原生协议套件全部通过，共 12 项新增链路测试；原 52 项测试保持通过。

## 阶段 9：Graph 与 stream capture（2026-10-29）

- 分离图模板、实例化快照和启动句柄；六种节点由 C++ CP 根据完成依赖调度，kernel 仍执行自定义机器码。
- 一张图启动占一个公开队列包；支持 fork/join、非拓扑编号、同实例跨 stream 串行重放、同类型节点参数更新和逐节点结果。
- stream capture 将提交及事件关系记录成图，捕获时无后端执行；跨 stream 分支须汇入捕获起点。拒绝捕获外部事件、同步、嵌套捕获等未支持行为。
- 新增 11 项图行为测试，覆盖快照、三次重放、更新互斥、分支重叠、环路和无效边、失败传播、独立分支完成、单/多 stream 捕获及陈旧句柄。
- `python3 examples/host_graph.py` 实际运行两个 SM、三个 CTA 的 65 元素向量加：普通提交 4 个包，三次图重放各 1 个包，另一次跨 stream 捕获启动，全部数值一致。保存机器码、汇编、trace 和验证 JSON。
- 验证：Debug、Release、ASan+UBSan 的九个套件均通过，每套 75 项测试；没有测量或宣称真实 CUDA 性能收益。

## 阶段 10：主机链路与 Graph 验收（2026-10-29）

- 完成三套独立构建回归，原 ISA、控制流、内存、张量及 Transformer 测试保持通过；新增链路和图测试全部通过，sanitizer 未报告错误。
- 审查队列、依赖、图资源保留、参数更新和失败分支；核对功能步数、完成计数及图内部节点计数含义，避免将软件服务机会描述成硬件周期。
- 复核短函数和中英文逐行配对注释。按含声明与结束行统计，最长 C++ 函数 25 行，Python 22 行；没有增加样式测试。
- README、HOST.md、HOST_PLAN.md 与 VALIDATION.md 已包含运行方法、支持项、未实现的 CUDA 兼容部分和证据映射。
- 保留阶段 7、8、9 的独立 Git 提交与本阶段验收记录；构建产物和实际 trace/验证 JSON 留在本地忽略目录中。

## 阶段 11–12 开始：微架构来源与存储单元（2026-10-29）

- 新目标要求内部硬件细节和原版 nvidia-smi 可见。新增 HARDWARE_PLAN.md/HARDWARE_SOURCES.md，保留完整剩余目标，不将功能 ISS 的完成状态沿用到硬件目标。
- 阅读官方 Ada/Blackwell 与 CUDA 访存文档、Turing/Hopper/Blackwell 逆向论文。明确已知行为与设计假设；记录逆向资料的代际混淆和数值不一致，避免直接抄作硬件事实。
- 实现可组合的真实数据 sector cache、有限 MSHR/等待者/返回容量、预约 way、LRU、bank 端口与 fill 端口、write-through，配合 bank/row/总线时序 DRAM 控制器。
- 实现 warp 合并器、逐拍 shared bank/广播，以及两个 SM 独立 L1/共享 L2 的数据层次结构；全局写采用明确但保守的排空/失效协议。
- 验证：17 项 C++ 硬件单元/组合检查，包含 320 请求随机字节差分；三种构建的十个 CTest 套件全部通过，即原 75 项加新 17 项检查。ASan+UBSan 无错误。
- 当前硬件单元尚未接入 Machine/CP，且 RTL、Tensor 流水线和 NVML 仍未完成。详细结构和可复现实验见 HARDWARE_MEMORY.md。

## 阶段 17 第一部分：原版 nvidia-smi 与活会话 NVML（2026-10-29）

- 在等待检测方式偏好时，按已说明的专用启动命令推进。实际使用未修改的 `/usr/bin/nvidia-smi`，不创建仿制表格脚本，不修改系统驱动或库。
- 首次纯公开 NVML 探针被 575.57.08 拒绝；动态加载日志定位到私有 `nvmlInternalGetExportTable` 握手。当前仅将该握手转发给匹配的已安装 NVIDIA 库，公开设备接口由模拟器后端提供。
- `Device(monitor_dir=...)` 发布实际分配表和 CP 状态；状态快照锁与生命周期锁分离。正常退出清理，异常死亡即使残留文件也不被枚举；旧句柄返回 GPU_IS_LOST。
- 原版 `-L`、默认表格和 CSV 查询均成功；7 MiB 分配显示 64/7/57 MiB，释放回到 64/0/64 MiB。温度、功耗、频率、CUDA、PCIe 等未实现属性保持 N/A。
- 新增六项端到端检查及 C++ ABI 辅助程序，覆盖多会话、退出、SIGKILL、坏快照、结构体版本和精确字节数。Debug/Release/ASan+UBSan 的 11 套件全部通过；当前共 98 项检查。
- sanitizer 对项目 monitor/NVML 插桩，外部原版程序预加载 libasan 并关闭其泄漏报告，地址/UB 检查仍启用。未修改系统 `nvidia-smi -L` 仍只返回 RTX 4070 SUPER。
- 文档和双终端运行方法见 NVIDIA_SMI.md。系统默认视图合并、CUDA 执行兼容、SM/Tensor 流水线与 RTL 尚未完成，完整目标保持进行中。

## 阶段 13 与 15 第一部分：基础 ISA 的 SM 时序通路（2026-10-29）

- 新增 `simt-run --timing`，基础算术/控制/集合/屏障及 global/shared/constant 访存经过真实寄存器 bank、逐 lane scoreboard、有限收集器、INT/FP/SFU 执行与写回资源；指令结果到写回时才更新架构寄存器。
- 分别处理 RAW/WAW 与操作数实际收集前的 WAR；相同源只占一次读端口，读写端口独立，多条流水线的完成可竞争写回 bank。每 warp 一个在途访存，但独立 ALU 可以与 load 重叠。
- CTA 按 warp/固定寄存器/shared 预算准入，分配真实槽位，完成后复用。每个 SM 的 shared bank 仲裁器服务多个 CTA 的隔离存储；global 路径直接使用此前 sector cache/MSHR/DRAM 单元，返回值不绕过缓存。
- 复用控制、集合、屏障和标量数值语义，保留错误位置；新增周期限制、可调参数、有限逐拍 trace 和硬件统计。明确拒绝尚未接入的 local/原子/异步/Tensor 指令，不自动回退瞬时功能执行。
- 验证：新增 7 项 C++ 单元和 35 项 Python 时序检查，包含原 80 个整数独立参考程序及 12 个新增 120 指令随机依赖程序；Debug、Release、ASan+UBSan 的 13 套件全部通过，共 140 项检查。`git diff --check` 通过。
- 复用屏障负例时发现旧例依赖顺序执行：时序发射延迟允许另一 warp 在两次到达之间前进，因而输入可以合法完成。新负例显式延迟另一 warp，验证同代重复到达被拒绝，未修改屏障语义迎合旧调度假设。
- `python3 examples/hardware_pipeline.py` 生成机器码、完整状态、命令和轨迹。依赖/独立 13 指令实验分别为 63/19 模型周期；shared 连续/冲突/广播为 19/50/19。冷 load 等待期间独立 MOV 在 cycle 7 写回，而 load 在 cycle 65 写回。均未宣称真实 GPU 周期。
- 文档见 HARDWARE_SM.md，参数来源新增明确假设条目。主机 CP/Graph 仍走功能模式，缓存尚未跨 kernel 保留；Tensor 数据通路、RTL 和参数校准仍待完成，完整目标保持 active。

## 阶段 14 与 15 第二部分：逐拍 Tensor 阵列与真实供数（2026-10-29）

- 继续目标前核对工作区干净、上阶段提交 `931da4a` 及 HARDWARE_PLAN.md；上次属于有代码、测试和提交的实际进展。没有将阶段完成视为整个硬件目标完成。
- 新增参数化 1–4 行/列阵列，默认 4×4。A/B 错拍注入并逐拍传向相邻 PE；每 PE 保持累加器、块点积、待完成结果。有限输入 FIFO 和未取响应占用均形成反压，不在单次宿主循环中计算整矩阵。
- Tensor 前端逐拍读取 shared 描述符、经真实 global/cache 或 shared banks 获取 A/B/稀疏 metadata/scale；复用纯数值解码，禁止读取底层内存旁路。输出按微 tile 经 TMEM 端口写入，全部确认后退休。
- TMEM 分配采用每 SM 共享列预算，按 bank 清零；显式释放及 CTA 退休归还容量。LDT/STT、已有 D 装载与结果存放共享 TMEM 仲裁。默认 16 banks、2 拍返回，参数均明确标为设计假设。
- 支持五种格式、2:4 不同行的位置选择、跳过缺失 NaN 乘积、binary32 块缩放三段及已有 D 累加；原功能数值规范不变。Tensor 单元独立推进，但发起 warp 仍阻塞到完成，没有宣称 PTX 非阻塞 MMA/commit 兼容。
- 复核 NVIDIA PTX 9.4 的 TMEM 和异步语义并记录差异，不用本研究阵列猜测 NVIDIA 原电路。新增 HARDWARE_TENSOR.md，更新 SM/内存/张量规范及来源台账。
- 验证：6 项 C++ 阵列检查覆盖精确波前时序、有限队列/响应、输入气泡、尺度三段和数值边界；19 项 Python 检查包括独立有理数参考、bank 冲突/容量、其他 warp 前进、原完整 GEMM 和最大 32×32×256 描述符。三种构建全部通过 15 套件、165 项检查，sanitizer 无错误。
- `python3 examples/run_workloads.py --workload gemm --timing --out artifacts/timed` 实际完成两个 SM、3 CTA、96 输出，最大绝对误差约 2.98e-7，共 1,536 次乘加。4×4/2×2 阵列分别为 2,245/3,811 模型周期，命令/输入/完整轨迹保存在 artifacts/timed/gemm。
- 仍未完成 local、原子、CP_ASYNC/mbarrier 的时序通路、CP/copy/Graph 统一时钟与跨 kernel 缓存、RTL 和校准；系统默认 nvidia-smi 视图合并也尚未实现。目标保持 active。

## 阶段 15 第三部分：真实异步搬运与事务屏障（2026-10-29）

- 在 `fa6556d` 后接入五条自定义异步指令。CP_ASYNC 经寄存器端口读地址，验证范围/预算，发布 issued 字节预约后入队并退休；载荷通过真实 global/coalescer/L1/L2/DRAM 读请求及 shared bank 写请求推进，没有源数据快照或功能内存旁路。
- 每 SM 默认 8 个命令、4 个有限片段、128 B 片段上限，支持非对齐 byte 路径及尾部。控制器与普通/Tensor shared 请求竞争端口；复制完成先于新提交申请控制器，避免队列满时阻塞额度归还。
- 事务表容量跨 CTA 共享，私有下一状态经 shared 记录写确认后才发布。到达和载荷都完成才切换 phase；WAIT 同址广播查询、不同地址串行。提前预约记录保护，并拒绝在途 DMA/普通 store 与新记录相交。
- 指令退休不代表 DMA 完成；全部 lane 等待/退出后后台仍推进，CTA 资源回收等待载荷完成与完成记录发布。原功能模式快照规则保持不变，明确无同步源修改与时序模式可能不同。
- 新增 29 项 Python 检查，包括原六项事务/四项组合负载、19 项时序/容量/错误检查；18 组边界载荷和 20 组固定种子随机片段逐字节比较，验证两类 store/init 竞争、多 CTA 隔离、队列饱和、复用 L1、EXIT 排空、多个 WAIT 地址及阶段先后条件。
- `cmake --build build{,-release,-sanitize}` 与三套 `ctest --output-on-failure` 均通过 16 套件、194 项检查，ASan+UBSan 无错误。最初编译因 BarrierJob 默认成员初始化被 Werror 拒绝，修正后全部构建通过；新增测试中两处编码器调用错误也在验收前修正，没有改变模拟语义来适配它们。
- `python3 examples/hardware_async.py` 保留机器码、输入、命令、完整 trace 和逐字节校验：4 KiB 搬运在片段池 1/4/8 时为 3356/2147/2147 模型周期；四次 256 B 拷贝在队列 1/8 时为 764/617 周期，容量不足时仍正常完成。
- `python3 examples/run_workloads.py --timing --out artifacts/timed` 原四类负载全部运行。Transformer 实际搬运 2176 B，执行 8 个有效 MMA、2304 次乘积，32 个输出最大误差约 1.07e-7，5637 模型周期。谓词关闭指令仍计入 op_counts，不能将其中 2 次 CP/16 次 MMA 误报为有效命令数。
- 新增 HARDWARE_ASYNC.md，核对官方 PTX 9.4 的 cp.async 大小和完成规则，所有队列/控制延迟保持“设计假设”标注。短函数与英中相邻注释约定保持，入口仅调用流程函数，没有增加样式测试。
- 原子/local 时序通路、持久 CP/copy/Graph 时钟与缓存、RTL、校准和最终审计仍待完成；专用启动器下原版 nvidia-smi 可见能力保持通过，系统默认视图合并未实现。完整目标仍 active。

## 阶段 15 第四部分：Local 物理映射与分阶段整数原子（2026-10-29）

- 开始前核对工作区干净及上一阶段 `b894e96` 提交，确认前一轮有实现/测试/文档进展。继续完整硬件目标，没有把阶段通过等同于最终完成。
- 将底层存储接口与稀疏 Memory 分离，增加实际 local 地址解码器。16 KiB 线程窗口按字交错映射到 global ISA 窗口以外，经过原有 coalescer/L1/L2/DRAM；DRAM 完成直接更新实际线程存储，不维护影子快照。部分 warp 填充读取和 CTA 生命周期均保留归属。
- 物理 sector/coalescer 支持 64 位地址；逻辑 global 仍限定 32 位，原高地址负例改为分别验证物理保留高位与逻辑越界。generic LD/ST 可在同一指令中拆分 global/shared/local 请求。
- 新增 global 原子 bank 阵列，每 lane 经 L2 读、整数 ALU 等待、4 B byte-mask 写穿、确认后返回旧值。有限请求队列包含未取响应；同 bank 锁至写确认，连续原子请求可共同使用阵列，普通读写在保守一致性边界排空。
- shared 原子复用真实 shared banks，读/ALU/写期间独占相应 bank，与普通访存/DMA/Tensor/事务记录竞争。没有同址原子广播，CTA 存储隔离保持。纯整数模式语义提取复用，执行器没有借此绕过内存单元。
- 自定义 59 个 opcode 均有 CLI 时序路径；新增 atomic/local 提交 trace 和 global/shared 原子统计、bank/queue/ALU 参数。该覆盖不等于原生 PTX/CUDA 或全部 NVIDIA 硬件。
- 验证新增 9 项 C++/17 项 Python 检查：精确 shared 时钟、普通 store 排斥、跨 SM 冷/热缓存、240 个混合请求差分、185 个唯一原子票号、6×48 条随机原子序列、local 冷热/分散/边界/CTA 槽复用和混合 generic 空间。新增 C++ 随机测试首次编译发现同一 auto 声明推断不同整数类型，拆开声明后通过，未改变算法来适配结果。
- Debug、Release、ASan+UBSan 均通过 18 套件、220 项检查（181 项 Python、39 项 C++），sanitizer 无错误；`git diff --check` 通过。既有数值、异步、Graph 和原版 nvidia-smi 套件保持通过。
- `python3 examples/hardware_memory_ops.py` 保存机器码/命令/完整状态与验证：同偏移/分散 local 两次读取为 133/606 模型周期、4/32 个 DRAM sector；global 原子同址/分 bank 为 2815/1081，shared 为 179/24。计时只描述当前设计，写穿策略的 DRAM 开销不是 NVIDIA 实测参数。
- 新增 HARDWARE_ATOMIC_LOCAL.md，更新整体范围与来源台账。核对官方 local 和 atomic 语义；具体物理映射、bank 锁与延迟均保持设计假设标注。按职责保留短函数，入口仅调流程函数，英中解释性注释相邻，没有添加样式测试。
- 持久设备 CP/copy/Graph 的统一时钟与跨 kernel 缓存、可综合 RTL、实卡校准和最终审计仍待完成。专用启动器可见模拟设备，系统默认 nvidia-smi 合并仍未实现；目标保持 active。

## 阶段 15 第五部分：持久设备统一时钟与实际主机拷贝（2026-10-29）

- `HardwareContext` 提升到设备生命周期，持有实际 L1/L2、DRAM、local 地址映射和单一时钟。kernel 使用外部时钟，独立 CLI 保留私有上下文。跨 kernel 保留缓存及开行状态，local 为每次启动预约不重复的物理 tag，并在退出后解除底层指针映射。
- `Device(timing=True)` 和原生 `--timing` 接入已有自定义 59 opcode 的全部时序执行路径；功能默认模式保持兼容。CP 增加 16 个活包槽、每拍一次取包/调度与共享 Graph 解码端口。所有节点先预约句柄，再逐拍解码，保留非拓扑编号和完成依赖。
- 新增单槽 128 B 主机拷贝引擎：H2D/D2H 经明确的链路等待，所有设备端访问经实际 L2/DRAM、写确认与 L1 失效；D2D 使用正向/反向有限块提供无竞争时的 memmove 结果。没有直接读取底层 Memory 作为数据返回的旁路。
- 分配/释放清零前只失效相交 sector，保留无关在途 fill。给层次请求增加所有者，kernel 出错后停止发射、保留存储/资源 pin，排空已接受 global/local/atomic 请求后才发布错误，不消费另一引擎的完成响应，不回滚已接受写入。
- 增加 per-job 硬件快照、绝对时钟/相对周期及累计统计范围。完成快照不随后续任务变化；CP/copy 和 kernel trace 均有上限。主机查询不推进时钟，两个引擎并行时设备时钟仍每 tick 仅增加一次。
- 新增 4 项 C++ 生命周期检查和 33 项 Python 检查（复用 20 项主机/Graph 契约，新增 13 项持久硬件场景）。覆盖相邻 sector、在途 fill、owner 排空、local 代际、冷热缓存、上传失效、重分配、链路/暂存、有限 CP/图展开、随机重叠搬运、原子/local 错误恢复、三次 DMA/Tensor 图重放和原四类完整负载。
- 实际验收：`cmake --build build -j6`、`cmake --build build-release -j6`、`cmake --build build-sanitize -j6`；三套 `ctest --test-dir <build> --output-on-failure -j6` 全部通过 20 套件、257 项检查（214 Python、43 C++），sanitizer 无错误。更新时序 NVML 用例后补跑 Debug 的 `native_nvidia_smi` 通过；`git diff --check` 通过。
- `python3 examples/hardware_host.py` 保存四类原负载各两次主机 Graph 重放的镜像、机器码、下载字节和硬件轨迹；仅增加 global 基址参数，没有替代算法。冷/热只读 kernel 为 62/17 模型周期，第二次不增加 DRAM 读。GEMM 96 输出误差约 2.98e-7，Transformer 32 输出约 1.07e-7，后者每次实际 DMA 2176 B、乘加 2304 次。
- `python3 examples/host_graph.py --timing --hardware-trace --out artifacts/timed-host-graph` 通过普通提交、三次图更新/重放和跨 stream capture，8 个公开任务全部完成，65 个输出逐项一致。原版 nvidia-smi 对实际时序设备重新实测，返回模拟器名称和 64/7/57 MiB；记录保存在 artifacts/hardware-host/nvidia-smi.txt，系统默认仍只显示真实 4070 SUPER。
- 新增 HARDWARE_HOST.md，更新整体计划、来源假设和结构边界。所有 CP 容量、链路参数、拷贝暂存与故障协议均标为研究设计；不能解释为已逆向 NVIDIA 内部实现。保持短函数和相邻中英文注释，未添加样式测试。
- 阶段 15 的自定义 ISA/CP/copy/Graph 接入已验收；完整目标仍 active。可综合 RTL、实机参数校准、PTX 异步矩阵协议、最终逐项硬件审计和系统默认 nvidia-smi 视图合并仍未完成。

## 阶段 16 第一部分：实际缓存、依赖状态和端口仲裁 RTL（2026-10-29）

- 新增 `sector_sram.sv` 和 `sector_cache.sv`：默认 64 KiB、128 sets、4 ways、32 B sector 的实际数据阵列；单同步读/写端口、tag/valid、有限 LRU rank、4 个 MSHR、最多 4 个合并等待者及 8 个响应槽。请求和返回使用稳定 valid-ready 握手，way 在事务完成前锁定，支持乱序 fill、byte mask、写穿/不写分配和排空后失效。
- 数据 SRAM 不复位，由有效位保护未初始化内容。Yosys 在 64 KiB 配置下实际识别出 2,048×256 = 524,288 位、一个读端口和一个写端口。它还没有映射到 FPGA BRAM 或工艺 SRAM macro；单入口端口时序不等于原 C++ 四 bank 模型。
- 新增单 warp 的逐 lane scoreboard：writer/predicate 位和 3 位有界 reader 计数，保留 RAW/WAW/WAR、掩码及常零语义；计数饱和反压，非法命令无部分修改。新增 4 请求者/4 bank 的独立读写循环仲裁，验证持续竞争下的公平性；尚无寄存器数据阵列或整个 SM 发射连接。
- 工具准备使用项目私有 `.tools/rtl`，无 sudo/系统库改写。`python3 tools/bootstrap_rtl.py` 实际通过；保留固定 Ubuntu 软件包及 SHA-256、Verilator 4.038、Yosys 0.40 commit `a1bb0255d654ffd0d8503577274f24b349a42995`。发行版 Yosys 0.9 无法解析使用的 SV cast，因此从官方固定版本构建新工具，而非改用宿主伪模型替代综合。
- 首次行为验证暴露 LRU victim 比较中负 sentinel 与无符号 rank 的比较问题，修正显式空候选判断。首次综合暴露 LRU rank 的存储资源共享形成组合环，改成明确触发器数组后通过 `check -assert`。门级导出清理无用别名并拆开内部位网，消除旧 Verilator 的向量级依赖误报，未关闭组合环或未驱动检查。
- `cmake --build build --target rtl-check` 及 `python3 tools/rtl.py --out artifacts/rtl-final` 均通过 14 项检查：cache 9、scoreboard 3、bank 仲裁 2。包括 360 个混合缓存事务、5,000 条 scoreboard 命令和 4,000 拍端口授权的 C++ 对照，验证精确 hit 流水线、高地址、锁定 way、反压稳定性和真实缓存字节。
- `python3 tools/rtl.py --module cache --sets 128 --out artifacts/rtl-full` 通过同样 9 项缓存检查；`--synth --infer-only --module cache --sets 128 --out artifacts/rtl-synth-full` 通过保留存储的综合。大配置没有全部展开成门，不把重复配置运行计作新的行为场景。
- `python3 tools/rtl.py --synth --out artifacts/rtl-synth-final` 三模块均通过综合检查；`--gate-sim --out artifacts/rtl-synth-final` 对实际生成网表再次通过同样 14 项检查。2 KiB 缓存、scoreboard 和仲裁分别为 50,627/31,015/1,391 个 generic cells，包含调试状态和展开存储，不能称为 NAND2 等效门数或芯片面积。网表与 RTL 哈希保留并在门级检查前核对。
- `python3 tools/rtl.py --build build-sanitize --out artifacts/rtl-sanitize-final` 最终 14 项检查全部通过，无 sanitizer 报告。最初开波形运行虽退出码为零，但 UBSan 报告旧 Verilator VCD 库的非对齐访问，不能算作通过；最终只关闭此模式的波形，保留 ASan/UBSan 并启用 `halt_on_error`。普通 RTL 和门级仿真继续生成 VCD。
- `ctest --test-dir build --output-on-failure -j6` 本阶段再次通过 20 套件、257 项软件检查。复核最终 RTL/网表/sanitizer/64 KiB 产物中的源码哈希均与当前文件一致。再次实测原版 nvidia-smi：系统视图为 RTX 4070 SUPER，专用后端为活模拟设备 64/7/57 MiB，关闭后不再显示；记录在 `artifacts/rtl-final/nvidia-smi-status.txt`。
- 新增 `docs/RTL.md`，同步 README、来源、计划、验证及相关硬件文档，明确运行中的 `simt-device` 仍使用 C++ 后端。Tensor 目前为逐拍研究阵列，浮点乘加仍调用宿主 `std::fma`；Tensor 数值/乘加 RTL、整体 RTL 连接、实卡参数校准、PTX 异步矩阵协议和最终硬件审计仍未完成。阶段 16 和完整目标保持 active。

## 阶段 16 第二部分：Tensor 数值电路与完整 4×4 阵列（2026-10-29）

- 开始前核对仓库干净及上一阶段 `12d12ad`，将上一轮分类为已提交且有验证证据的进展。继续 coding-standards 的按职责拆分、入口只调用函数、相邻英中注释要求。
- 新增四级 `fp32_fma.sv`：实际 24×24 位尾数乘法、指数对齐/77 位带 sticky 通路、有符号幅度加减、前导位检测与规格化/最近偶数舍入。保留次正规数、带符号零、溢出无穷与统一 NaN；零操作数不参与最大指数选择。RTL 不调用宿主浮点或 DPI，输出反压冻结在途流水线。
- 新增 `int8_mac.sv` 的有符号 8×8 乘法和模 2^32 累加，以及 `tensor_decode.sv` 的 FP16/BF16/FP4/INT8 转换。全部 65,536 个 FP16 与 BF16 编码逐项验证，全部 65,536 个 INT8 乘积在实际 PE 中验证；FP4 高/低半字节与负零均覆盖。
- `tensor_pe.sv` 具有输出累加器、块内部分和、元数据与 DOT/SCALE/ACCUMULATE 状态，复用同一浮点流水线执行三段块缩放。稀疏缺失项保留原值而不计算传入 NaN；scale 乘法使用同号零保持原有符号零语义。上一项退休与下一项准入同拍时，反馈旁路提供最新累加值。
- `tensor_array.sv` 实例化 16 个 PE、8 个格式转换器、两项各 526 位 FIFO、边缘错拍与相邻传输寄存器。有限输入队列、last 归属、全部 PE 完成及结果反压均有实际状态，支持 1..4 行列、五种格式、显式稀疏使能和块缩放。原 C++ 对照接口更抽象，RTL 首个波多两个前端时钟；定向验证 2×3/两波 14 拍、1×1/两缩放块 27 拍、4×4/256 波 1,033 拍。
- 数值验证没有只使用普通 GEMM：FMA 包含 13,824 个边界/特殊值组合、250,000 个随机/抵消输入，以及独立 `numeric_reference.py` 的 12,000 组 Fraction 参考；还验证四级延迟、输入气泡、反压和复位。阵列与原 `simt::hw::TensorArray` 对照 100 个混合格式/输入气泡 tile、最大 K、模式复用、稀疏 NaN、结果保持和失败后重试。
- 首次阵列综合在 FIFO 的存储输出寄存器合并后报告 260 个未驱动位，改为明确两项寄存器 FIFO 后通过 `check -assert`；失败日志保留在 `artifacts/rtl-tensor-array-synth/tensor/synthesis-before-fifo-fix.log`。复查发现 PE 最后令牌退休同拍的反馈准入边界，增加 `!saved_last` 约束和专门测试，再次执行受影响模块的仿真、综合、网表和 sanitizer 检查。
- 最终 `cmake --build build --target rtl-check` 全部通过 30 项（原 14 + 新增 FMA 4/转换 3/PE 3/阵列 6），当前报告 `build/rtl/simulation.json`。测试驱动加入 `simt_core` 静态库依赖以使用实际 Tensor 参考；大随机测试不生成巨量波形，短场景仍保留 VCD，sanitizer 继续保留全部检测并关闭旧波形运行库。
- 实际执行 `tools/rtl.py --synth` 和 `--gate-sim` 的 fma/decode/pe/tensor 四个模块，分别记录在 `artifacts/rtl-tensor-synth`、`rtl-tensor-decode-synth`、`rtl-tensor-pe-synth`、`rtl-tensor-array-synth`。最终 16 项网表检查全部通过；对应四个 `--build build-sanitize` 目录也全部通过，无 sanitizer 报告。
- Yosys 0.40/ABC 实际统计：FMA 为 11,498 个 generic cells/478 个触发器位；转换器 546/0；PE 13,088/794；完整 4×4 阵列 217,893/16,636。阵列统计包含全部实例，不是只计模块定义。没有工艺库、P&R 或 DSP 映射，不能解释成 NAND2 等效门数、工艺面积、Fmax 或整 SM 成本。
- `ctest --test-dir build --output-on-failure -j6` 本阶段再次通过 20 套件、257 项软件检查，包括原版 nvidia-smi；C++ 数值执行方式没有改动。本阶段主要说明新增于 `RTL_TENSOR.md`，同步计划、Tensor 数值规范、来源、README 和验证记录。
- 完整目标保持 active。当前 `simt-run`/`simt-device` 仍使用 C++ 后端，下一步须接入 RTL 数值/阵列、ISA/TMEM 和整体 RTL 层次；实卡参数校准、PTX 异步矩阵协议、最终逐项审计及系统默认 nvidia-smi 视图合并仍未完成。

## 阶段 16 第三部分：Tensor RTL 接入实际执行路径（2026-10-29）

- 接续 `b75bec3`，新增 `TensorEngine` 接口和原始字节输入结构，保留原 C++ 阵列作为默认参考模型。`SIMT_TENSOR_RTL=ON` 构建通过 Verilator 官方 CMake 支持生成独立静态库，私有工具环境仅传给工具子进程；没有修改系统安装。
- 每个 SM 单独持有 RTL 实例；MMA 前端继续经 shared/global 请求读取描述符、稀疏元数据、原始 A/B 和 scale。FP16/BF16/FP4 解码移至所选后端，RTL 路径不使用宿主转换或矩阵算术。结果仍经过 TMEM bank 写确认才退休。
- 适配器只在 `tick()` 产生一个上升沿，首次复位也占真实一拍。start/input/result 的 valid-ready 保留归属和有限容量；结果尚未确认时的新启动等待旧结果释放，不重复交付。增加 RTL 观测端口，乘积/跳过/缩放计数来自实际 PE 发射，注入次数来自阵列注入事件。
- CLI 增加 `--tensor-rtl`，Python `Device` 增加 `tensor_rtl=True`；两者要求 timing，未编译后端时明确失败。物理级数和 FIFO 固定为 4/2，不支持的参数明确拒绝。设备属性和逐 SM 快照分别报告可用能力、实际后端与阵列拍数；示例支持相同显式选择并验证实际后端。
- 新增 2 项选择检查和 5 项适配器 C++ 检查；在 RTL 后端复用 19 项独立数值/时序和 33 项主机/Graph 契约，另加 2 项实际运行中查询不推进时钟、未完成 MMA 故障后再次启动的检查。最大矩阵保留 32×32×256 和 262,144 次乘积断言，没有缩小算例。
- 首次失败恢复用例错误地假定故障排空期间阵列仍运行，实际私有计算状态已冻结、持久存储仍推进。修正测试以区分正常执行和故障排空拍数，保留实际计数；验证失败 tile 未写回、后继 DMA/MMA 输出仍为 31。不是通过改变电路统计来掩盖时钟差异。
- `ctest --test-dir build --output-on-failure -j6` 通过默认 Debug 的 21 套件、259 项。`ctest --test-dir build-rtl --output-on-failure -j6` 通过 RTL Release 的 24 套件、318 项（270 Python、48 C++）；原始日志核对计数，没有把独立 RTL 重跑并入这些数字。
- `cmake -S . -B build-rtl-sanitize -DCMAKE_BUILD_TYPE=Debug -DSIMT_SANITIZE=ON -DSIMT_TENSOR_RTL=ON` 并构建后，以 `UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-rtl-sanitize --output-on-failure -j4` 执行。首次其余 23 套件通过，最大矩阵超出既有 8 秒上限。将 RTL 子进程上限设为 60 秒，保留原算例与检测，再执行 `-R '^rtl_tensor_execution$'`，19 项全部通过，整套耗时 65.46 秒。所有套件均验证通过，无 sanitizer 报告；生成电路代码也接受插桩。
- 当前源码 `cmake --build build --target rtl-check` 再次通过全部 30 项。PE/阵列因增加观测端口，分别执行 `tools/rtl.py --synth` 与 `--gate-sim`，保存在 `artifacts/rtl-pe-integrated` / `rtl-array-integrated`；同样 3+6 项检查均通过。两模块还在 `--build build-rtl-sanitize` 的独立仿真下通过，记录在对应 `-sanitize` 目录。
- 新综合统计：PE 13,090 个 generic cells / 794 个触发器位，阵列 217,928 / 16,636，严格综合检查和当前源码哈希验证通过。变化来自观测输出的组合逻辑；没有工艺面积或整 GPU 门数结论。
- `python3 examples/run_workloads.py --build build-rtl --timing --tensor-rtl --out artifacts/rtl-integrated` 四类完整负载通过；GEMM 2253 模型周期、1536 次 RTL 有效乘积，Transformer 5671 周期、2304 次。`python3 examples/hardware_host.py --build build-rtl --tensor-rtl --out artifacts/rtl-host` 每类两次 Graph 重放通过，GEMM/Transformer kernel 为 2241/5655 周期，输出误差约 2.98e-7/1.07e-7；二者初始缓存状态不同。
- 实际完成 RTL Graph GEMM 的设备保持存活时，原版 `/usr/bin/nvidia-smi` 经专用启动器显示名称和实际 64/7/57 MiB，退出后无设备；系统默认仍为 RTX 4070 SUPER。输出在 `artifacts/rtl-integrated/nvidia-smi-status.txt`，未宣称 CUDA/PCIe 枚举或默认视图合并。
- 新增 `RTL_BACKEND.md`，同步 README、硬件来源、计划、Tensor/主机/RTL/验证文档。保持短函数、入口只调函数、相邻英中注释，没有样式计数测试。完整目标仍 active：其余 RTL 单元与整个 SM/存储层次连接、真实显卡参数校准、PTX 异步矩阵协议、系统默认 SMI 合并及最终逐项审计尚未完成。

## 阶段 16 第四部分：L1/L2 RTL 接入持久执行层次（2026-10-29）

- 接续干净的 `c744553`，继续 coding-standards。新增 `CachePort` 与 `SIMT_CACHE_RTL`，每 SM 实例化 64 KiB、4 路 L1，全设备共享 256 KiB、8 路 L2。CLI `--cache-rtl` 与 Python `cache_rtl=True` 要求 timing；不支持的构建和非默认几何配置明确拒绝，不回退或静默忽略配置。
- 缓存 tag/data、MSHR、合并返回和替换实际经过 `sector_cache.sv` / `sector_sram.sv`。适配器只保留一个入口、四个下游票据、一个 fill 与八个未消费响应；ID 在用户取回响应后复用，没有 C++ 影子 tag/data。`tick()` 含首次复位也只推进一个边沿，提交、取回和查询无隐藏时钟。
- 新增 RTL 范围失效控制器：首尾 sector、模式、游标、busy/done，每拍扫描一行；L1/L2 分别 512/2048 拍。目标在途 MSHR 阻止失效，无关 fill 可继续；同一行的非目标 sector 同拍 fill 与目标失效有专门验证。C++ 维护队列上限 64，各缓存先统一预检，避免部分修改分配状态。
- 主机上传下载、普通访问、local、原子、DMA 与 Tensor 供数复用所选真实缓存层次。跨 SM 写入依然由 C++ 一致性协议排空较早读取、预约 L1 失效、绕过 L1 写穿 L2，并阻止较新读取越过维护。DRAM、维护队列、原子 ALU 和整个一致性控制器尚非 RTL。
- 内核快照新增实际后端、绝对缓存时钟、几何和端口容量；hit/miss/merge/eviction 来自硬件寄存器，line miss/fill bytes 来自电路事件。适配器统计反压，不依据算例规模伪造缓存事件。缓存时钟与持久设备时钟逐拍一致。
- 新增 2 项后端选择、5 项 C++ 桥接/层次检查；在新后端复用 61 项存储/DMA/Tensor 和 35 项主机/Graph 契约。定向覆盖八个未消费响应、80 次 ID 复用、无隐藏时钟、真实 SRAM 命中、维护队列、两 SM 一致性、160 次独立字节参考混合访问。原最大 32×32×256 矩阵与全部数值断言保留。
- 实际构建默认 Debug 与 `build-cache-rtl` Release（同时启用 Tensor/Cache RTL）。`ctest --test-dir build --output-on-failure -j6` 通过 22 套件 / 261 项；`ctest --test-dir build-cache-rtl --output-on-failure -j6` 通过 28 套件 / 421 项（368 Python、53 C++）。这些数字包括后端复跑，不等于新增独立场景数。
- `build-cache-sanitize` 对适配器和所有生成电路插桩。`UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-cache-sanitize --output-on-failure -j3` 首次其余 27 套件通过，最大矩阵超出 60 秒进程限制；仅把缓存 RTL 执行限时设为 180 秒后，61 项通过，139.05 秒。最终复位优化后重建两种 RTL 构建，并以 `-R '^(cache_backend_selection|rtl_cache_engine|rtl_cache_execution|rtl_cache_host)$'` 重跑受影响四套件，全部通过；sanitizer 最终执行套件 139.22 秒，无检测报告。
- 新诊断输出的向量位依赖触发旧 Verilator 组合环误报，改为独立标量输出后通过，未禁用警告或 `check -assert`。完整 L2 复位循环需提高 Verilator `--unroll-stmts` 到 100000，实际两套构建成功。删除冗余 tag/valid 复位，以已有 present 位保护旧元数据，新分配仍初始化；带旧数据的复位行为重新验证。
- `python3 tools/rtl.py --out artifacts/rtl-cache-all` 通过当前全部 32 项独立 RTL 检查；缓存新增范围维护两项后共 11 项。`--module cache` 的 RTL、`--synth`、`--gate-sim` 在 `artifacts/rtl-cache-integrated` 均通过；`--build build-cache-sanitize --module cache --out artifacts/rtl-cache-integrated-sanitize` 同样 11 项通过，无 sanitizer 报告。64 KiB 参数的同样 11 项也通过。
- 小配置综合实际为 55,212 generic cells / 22,083 触发器位，比去除冗余复位前少 96 个组合单元；不是等效 NAND2 门数。`--module cache --sets 128 --synth --infer-only --out artifacts/rtl-cache-integrated-full` 通过，识别 524,288 个 SRAM 数据位、单读/单写；tag 写端口降至 1、valid 为 2。最终七份仿真/综合/网表清单的源码哈希匹配当前文件，小配置网表哈希匹配。
- 256 KiB L2 的 `--sets 256 --ways 8 --synth --infer-only` 首次超过 600 秒；修改复位后第二次仍超过限制，最后在 Yosys `OPT_CLEAN`。工具改为实时保存输出，第二次超时前的日志位于 `artifacts/rtl-l2-integrated-inference/cache/synthesis.log`。没有把超时当作成功或用已通过的 L2 Verilator 执行替代综合结果；L2 综合性能问题仍待解决。
- 最终 `python3 examples/hardware_host.py --build build-cache-rtl --tensor-rtl --cache-rtl --out artifacts/rtl-cache-host` 四类负载各两次 Graph 重放通过。冷/热只读分别 40/18 模型周期且热读取无新增 DRAM 请求；GEMM 96 输出最大误差约 2.98e-7，Transformer 32 输出约 1.07e-7。两种后端身份、机器码、下载值和统计均保存，不作为 4070 SUPER 性能预测。
- 实际完成两级缓存和 Tensor RTL GEMM 的活会话经原版 nvidia-smi 显示模拟器及 64/7/57 MiB，关闭后无设备；系统默认仍显示真实 4070 SUPER。记录在 `artifacts/rtl-cache-host/nvidia-smi-status.txt`，没有 CUDA/PCIe 枚举或默认合并结论。
- 新增 `RTL_CACHE_BACKEND.md` 并同步架构、来源、计划及验证文档。当前是 CPU 上运行的混合仿真，非 NVIDIA 内部电路复刻。L2 综合、其余 SM 单元的 RTL 接入、整机连接、实卡校准、PTX 异步矩阵协议、系统默认 SMI 视图合并及最终审计仍未完成，完整目标继续 active。

## 阶段 16 第五部分：组内 LRU 与完整 L2 综合（2026-10-29）

- 继续前核对干净仓库和上一阶段提交 `c2bcafd`，上一轮属于有实现、验证与提交的实际进展。确认先前 L2 综合进程已经终止，日志最后为 `OPT_CLEAN`；本轮在改变电路后启动新综合，没有因观察超时重复启动仍存活任务。
- 平坦 `mem2reg` rank 数组的动态读写生成大量跨全缓存的选择逻辑。新增 `cache_lru.sv`，按组实例化排名寄存器、局部选中 rank mux、比较和写使能；输出只选择所查询组。保持精确 LRU、无效行优先、锁定 way 过滤及原流水线。读取和命中行写入准入时更新，合并、写未命中、fill 和未接受请求不更新。
- L1 为 128×4×2 = 1,024 位 rank，L2 为 256×8×3 = 6,144 位；除从源码推导，还核对 Yosys 中间网表对应 512/2,048 个 2/3 位寄存器。数据、tag、MSHR 和维护接口未改为简化模型，没有缩小 L2 或换用随机替换。
- `tools/rtl.py` 的实际仿真和网表检查支持 `--ways 8`。既有 11 项缓存检查参数化容量/扫描长度，锁定替换用例在 8 路下也填满全部 way；360 次随机请求跨多组反复制造相联冲突，大容量下也检查真实替换。所有访问过的 sector 最终逐字节与独立 backing 比较。检查数量未因配置复跑增加。
- 综合和仿真清单增加 SETS/WAYS；门级检查除源码和网表哈希外还检查几何。实际负例验证 4 路网表无法配 8 路平台。新模块接入两份 CMake 后端；旧 Ninja 图在自动重配之前仍用原源文件列表，首次构建提示缺少 cache_lru，显式执行 `cmake -S . -B <build>` 后两套构建均成功。
- `python3 tools/rtl.py --module cache --sets 256 --ways 8 --synth --infer-only --out artifacts/rtl-l2-banked-inference` 通过原完整 coarse 流程和 `check -assert`，数据阵列 8,192×256 = 2,097,152 位、单读/单写。相应 `--sets 128` 的 L1 推断在 `rtl-l1-banked-inference` 通过，524,288 位、单读/单写。没有改动 600 秒工具上限或跳过优化来获得成功。
- `--module cache --out artifacts/rtl-cache-banked`、`--sets 128 --out artifacts/rtl-l1-banked`、`--sets 256 --ways 8 --out artifacts/rtl-l2-banked` 三种容量均通过相同 11 项 RTL 检查。用 `--build build-cache-sanitize` 在 `rtl-cache-banked-sanitize` 与 `rtl-l2-banked-sanitize` 重跑 4/8 路，全部通过；保留 ASan/UBSan，按既有规则只关闭旧 VCD 运行库。
- 2 KiB/4 路 `rtl-cache-banked` 和 4 KiB/8 路 `rtl-cache8-banked` 均执行 `--synth`、`--gate-sim`，严格综合及同样 11 项网表检查通过。层次总计分别为 54,770 / 22,083 和 92,501 / 39,528（generic cells / FF 位）；2 KiB 比上版少 442 个组合单元。大容量只保留存储推断，未声称物理 SRAM、FPGA 映射、面积或全 GPU 门数。
- 联合 RTL Release 的 `ctest --test-dir build-cache-rtl --output-on-failure -j4` 再次通过全部 28 套件 / 421 项，日志重新核对为 368 Python、53 C++，包括原版 nvidia-smi 六项，无跳过。默认软件和其他独立 RTL 模块无行为改动，本轮未无故复跑全部历史检查。
- `UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-cache-sanitize --output-on-failure -j2 -R '^(cache_backend_selection|rtl_cache_engine|rtl_cache_execution|rtl_cache_host)$'` 首次三套件及执行套件 60 项通过，完整最大矩阵超过 180 秒。组内显式寄存器增加未优化插桩代码求值开销，将缓存执行进程上限改为 420 秒，保留 Debug 编译、原算例、全部断言和检测。单独重跑 `CacheTensorPipeline.test_maximum_descriptor_shape` 通过，308.877 秒，1,024 个输出和 262,144 次有效乘积正确，无 sanitizer 报告。失败与重跑日志分别在 `artifacts/rtl-banked-sanitize/ctest-before-timeout-change.log` / `maximum-matrix.log`。
- `python3 examples/hardware_host.py --build build-cache-rtl --tensor-rtl --cache-rtl --out artifacts/rtl-banked-host` 四类负载各两次 Graph 重放通过。新旧 `validation.json` 全量比较相等，包括输出误差、所有 kernel/设备周期、缓存与 DRAM 累计计数；冷/热仍为 40/18 模型周期，设备最终 64,500 拍。这是重构前后可观察行为对照，不是实卡性能校准。
- 最终 11 份本轮仿真/综合/网表清单的源码、参数和两份网表哈希核对通过，摘要保留于 `artifacts/rtl-banked-evidence.json`。更新 RTL、缓存后端、来源、验证、README 和整体计划，保留历史超时而不覆盖成成功记录。完整目标继续 active：其余 SM 电路接入、整机连接、实卡校准、PTX 异步矩阵协议、系统默认 SMI 合并与最终审计仍待完成。

## 阶段 17 第二部分：合并 NVML 与用户级默认入口（2026-10-29）

- 从 `fc0aa9b` 继续，实现真实 NVML 与活模拟快照的合并枚举。物理设备保留原索引，模拟设备追加；按 UUID/索引查找、分配字节、生命周期和旧句柄返回分开路由。精确官方签名的单设备只读接口转发原生库，其他未实现接口返回不支持。默认直接加载库仍为仅模拟模式，`--merge` 显式启用合并。
- 原生私有进程查询不接受包装后的物理句柄，最终保留原生句柄并验证登记身份。ABI 辅助程序暴露关闭死锁：原生 `nvmlShutdown` 回调桥接 `nvmlDeviceGetCount_v2`，重复进入注册表 mutex。用线程局部原生调用范围和各公开入口的回调分支修复；没有使用递归锁、deepbind 或关闭 sanitizer。
- NVIDIA 私有 PCI 选择扫描无法处理没有 PCI 身份的模拟设备。`tools/simt_smi.py` 将 PCI-BDF 选择、compute/accounted-process 专用查询、管理命令及其他模式送回原生后端；默认表格、列表和设备查询进入合并后端。未伪造 PCI 地址或宣称完整 NVML/CLI 兼容。
- 新增安装器，将可撤销的入口写入用户 bin，拒绝替换无关命令、符号链接和用户修改过的安装。实际执行 `python3 tools/install_smi.py --build build-release`，`command -v nvidia-smi` 指向 `/home/zeke/.local/bin/nvidia-smi`，原版 `/usr/bin/nvidia-smi` 安装前后 SHA-256 相同。未修改系统驱动、库或 shell 配置。
- 新增 7 项合并检查，复用原 6 项模拟监控检查，覆盖真实元数据对照、分配/释放、多会话、选择器、退出/SIGKILL、ABI、引用计数、并发查询、安装/撤销及拒绝覆盖。Debug 全量 23 套件、268 项通过（225 Python / 43 C++），Release 与 ASan/UBSan 本次各运行受影响的 2 套件、13 项且全部通过。原始日志与计数保存在 `artifacts/nvml-merged/`；没有把嵌入的 C++ 断言重复计数。
- 使用 `Device('build-cache-rtl/simt-device', timing=True, tensor_rtl=True, cache_rtl=True, monitor_dir=...)` 完成两次 Graph GEMM，每次 96 个值通过，最大误差约 2.98e-7。随后普通 `nvidia-smi` 同时显示实卡和模拟器、保留真实进程表；7 MiB 分配、释放到 0 和退出后移除均验证。`installation.json`、`default-command.txt` 和 `gemm/` 保存原始证据。
- 同步 NVIDIA_SMI、计划、验证、来源、README 和两份 RTL 后端文档。普通 `Device()` 仍须显式启用监控；原生二进制直接执行及其他用户的环境保持原生视图。完整目标保持 active：Tensor/缓存电路已接入，但其余 SM 电路接入、整体 RTL、实卡校准、PTX 异步矩阵协议与最终审计仍未完成。

## 阶段 16 第六部分：物理 warp scoreboard 接入（2026-10-29）

- 从干净的 `0679d4c` 继续；上一阶段实际实现、验证并安装用户级 nvidia-smi 合并入口，属于已完成的阶段进展。完整硬件目标仍 active，本轮处理尚由 C++ 保存的依赖状态。
- 新增 `scoreboard_cell.sv`、`warp_scoreboard.sv`、统一 `ScoreboardEngine` 与实际 Verilator 适配器。CLI、设备进程、Python、主机启动和两个示例显式传递 `scoreboard_rtl`；能力缺失、不启用 timing、超过 4 banks 均明确失败。普通模式保留独立 C++ 参考。
- 按 SM 物理 warp 槽持有电路，首次使用时建立宿主实例并跨 CTA 保留；不为历史 CTA 无限新增硬件槽。实际依赖、源收集释放、写回和谓词均调用电路。每拍末统一提交；查询和引脚打包无额外时钟，无软件影子写者表或读者计数。
- 首版 16 个任意更新串联整个读者计数，实际综合 2,741,408 generic cells。根据每 bank 一读一写、源去重、每 partition 单发射，改为逐寄存器释放写者→收集读者→预约读者→预约写者；每种事件最多一次。保持 16 个输入、6 位计数和逐 lane 检查，阶段版降为 763,640 cells。合法实际路径每拍最多 13 个非空事件；更宽 bank 配置明确拒绝。
- 新增 4 项 RTL 对照及 4 项 C++ 适配器检查，另有 2 项公开后端选择、96 项实际指令/数值/存储/同步复跑和 34 项主机/Graph 检查。原语测试与 6,400 个随机事件覆盖阶段旁路、部分非法 mask 原子拒绝、容量、复位和状态归属；引脚测试独立打包，覆盖跨 32 位字边界。
- 早期测试修正有明确原因：失败 stream 再启动按现有语义得到 dependency_failed，改用独立 stream；测试目标生成式与 63 不互素导致第 10 个端口重复同一寄存器，改成不同目标；Verilator 新分片未进入旧 CMake 列表，显式重配后重新构建。相关失败记录保存在 `rtl-scoreboard-integrated/first-integration.log` 和 `rtl-scoreboard-phases/release-before-adapter-test-fix.log`。
- 未门控版 Release 全部指令检查通过，但 Debug sanitizer 主机请求超过固定 30 秒，96 项执行套件还触发 600 秒总上限。保留错误日志；公开接口增加默认 30 秒、必须有限且为正的 `request_timeout`，测试可选择 180 秒。没有减少算例或关闭检测。
- 新增实际 RTL 事件时钟门，适配器省略门关闭的无状态转换求值，常量 R63/P7 与未变化的组合查询不重复计算。分别报告输入模型拍数和有效更新边沿；低电平控制契约、尚无 ICG/FPGA 时钟映射和工艺时序结果在文档中明确。最终综合 763,658 cells / 14,368 FF 位，RTL、ASan/UBSan、重新综合后的网表均通过相同 4 项，记录在 `rtl-scoreboard-gated*`。
- 最终默认 Debug 全量 24 套件 / 270 项通过；联合 Release 全量 33 套件 / 564 项通过；scoreboard RTL 的 Debug ASan/UBSan 全量 27 套件 / 404 项通过，96 项执行套件 101.94 秒。实际命令和计数见 VALIDATION；联合构建套件分别选择后端，不声称全部测试同时开启三个 RTL。
- 最终 `hardware_host.py --build build-cache-rtl --tensor-rtl --cache-rtl --scoreboard-rtl --out artifacts/rtl-scoreboard-gated-host` 同时启用三类电路，四类负载各两次 Graph 重放通过。除新增后端身份外，完整输出、周期和缓存/DRAM 统计与 `rtl-banked-host` 相同，设备最终 64,500 拍。阶段文档详述源码、状态位、同拍协议、成本、复现和未完成部分。
- 最终进一步逐份比较 8 个 kernel 报告，删除新增 scoreboard 观测字段后，完整状态和逐拍 trace 与旧后端全量相等。另启动联合 RTL GEMM 会话，由普通 nvidia-smi 验证实卡/模拟器共存、7 MiB 分配、释放和退出；10 份 kernel 报告的后端与门控时钟关系核对通过。当前 RTL 与 gate 哈希匹配四份最终工具清单，分别保存在 `backend-audit.json`、`nvidia-smi-status.txt` 和 `evidence-audit.json`。
- 完整目标保持 active。依赖控制电路已经接入，但寄存器数据阵列、bank 仲裁、operand collector、其余 RTL 整机连接、实卡参数校准、PTX 异步矩阵协议及最终审计仍需继续。

## 阶段 16 第七部分：寄存器数据阵列接入（2026-10-29）

- 从干净的 `f09b110` 继续；上一阶段完成了 scoreboard 电路接入与阶段提交，属于实际进展。当前补齐仍由 C++ 提供的源操作数数据，完整硬件目标保持 active。
- 新增 `register_bank.sv`、`register_file.sv`、统一 `RegisterFile` 接口和 Verilator 适配器。每 SM 固定 65,536 字，按 1/2/4/8 partition 分配 32/16/8/4 个物理 warp 槽；每分区四个数据 bank。逐 lane 有效位实现初始零值和槽复用清零，数据位无复位，masked write 与同拍旁路均由实际电路产生。
- CLI、持久 C++ 设备、Python、主机启动、两个示例及硬件快照加入显式选择和能力检查。实际普通源读取不再访问架构镜像；全部产生普通寄存器结果的标量、访存、原子、Tensor/async 返回与集合指令走同一个 RTL 写端口。无后端、非 timing 或不支持几何明确失败。
- 端口引脚在时钟低电平保持；方法调用不增加时钟，读写分别每 bank 每拍一次。适配器拒绝读取后再追加写回，以保持流水线阶段契约。报告物理容量、端口组数、输入/有效时钟、读写和槽清除事件。collector 和请求排序仍为 C++，未将独立循环仲裁模块冒充已接入。
- 首次复跑 96 项指令时，原单端口 CAS/WAR 用例被四端口限定拒绝。新增真实 RTL 的 1/2/4 端口共享冲突判断，保留原测试重新通过；没有跳过旧替换值验证。失败记录保存在 `rtl-register-file-history/first-integration.log`。
- 新增 4 项独立电路场景、3 项适配器检查和 2 项后端/槽复用检查。独立差分包含 2,400 次随机掩码写、清槽、读旁路；适配器覆盖四种分区几何和跨 32 位的行地址、物理端口容量、错误与查询时钟隔离。
- 默认 Debug 全量 25 套件、272 项运行，271 通过、1 项未构建 RTL 而跳过；四后端可用的联合 Release 全量 37 套件、699 项通过，5.35 秒；寄存器 RTL 的 Debug ASan/UBSan 全量 28 套件、405 项通过，74.28 秒。寄存器单后端 Release 最终重建后另复跑受影响的四套件、135 项，1.74 秒。命令、计数与范围见 VALIDATION。
- 最终相同 4 项独立 RTL、sanitizer、综合网表均通过。完整 8 槽 partition 保留存储综合识别 128 个一读一写数组，合计 524,288 数据位；2 槽、16 KiB 小配置完整综合 427,025 generic cells / 135,168 FF 位，`check -assert` 通过。没有完整容量通用门展开、工艺宏映射或频率结论。
- 最终四后端同时运行向量、规约、GEMM、Transformer，每种两次 Graph 重放。除新增寄存器观测项外，8 个完整 kernel 状态、trace、周期、缓存与 DRAM 统计和上一阶段全量相等，设备最终 64,500 拍。另一次联合 GEMM 会话由普通 nvidia-smi 查询，真实卡/模拟器共存、7 MiB 分配、释放到零、退出移除均正确。
- 当前 RTL 哈希与五份最终工具清单及实际 gate 文件核对一致；10 个实际 kernel 报告核对电路后端和时钟关系。记录位于 `rtl-register-file-final/evidence-audit.json` 与 `rtl-register-host-final/{backend-audit.json,nvidia-smi-status.txt}`。
- 寄存器数据接入属于新增进展，不能据此完成总目标。operand collector、请求仲裁电路、其余 RTL 整机连接、实卡参数校准、PTX 异步矩阵协议与最终硬件审计仍待完成。

## 阶段 16 第八部分：寄存器直连 collector 与年龄仲裁（2026-10-29）

- 从干净的 `063ff61` 继续，按 coding-standards 保持短职责函数和相邻英中注释。新增 `operand_collector.sv`、`register_operand_unit.sv`、统一接口及适配器；同一个 RTL 顶层将实际 RF 向量连到 collector，C++ 不回填源数据。
- 每分区 32 个物理条目，运行额度 1–32；每条目最多三个源、32 lane mask、向量锁存与相对年龄。释放时压缩年龄，避免时间戳回绕；四个读端口组按最老请求仲裁，支持 1/2/4 共享配置、最后源旁路、同时释放和分配。当前输出查询仍是向 C++ 执行层交接的边界，没有冒充多执行单元物理传输网络。
- 指令容量、源收集、scoreboard 读者释放及执行准入接入真实电路；C++ 仅映射动态指令 ID。CLI、持久主机、Python、Graph、示例及报告传递 `collector_rtl`，要求同时启用寄存器 RTL，错误配置明确拒绝。
- collector 使用输入控制的时钟门，RF 与 collector 同拍推进；报告各自有效时钟，查询不推进时间。初次依赖内部输出的门控出现 Verilator 组合反馈警告，改为明确低电平输入协议，没有关闭警告；没有工艺 ICG 或实际功耗结论。
- 默认 Debug 26 套件 / 274 项运行，其中 272 通过、2 跳过；联合 Release 41 套件 / 834 项全部通过；寄存器/collector RTL Debug ASan/UBSan 32 套件 / 540 项全部通过。最后新增空闲时钟断言的 3 项适配器检查在两种构建重跑，联合 Release 全量亦包含最终断言。计数、命令及后端范围见 VALIDATION。
- 首轮 sanitizer 最大矩阵超出原有 120 秒子进程上限。保留失败日志后，按电路 waiting 位省略无变化的空闲求值；同样 96 项最终 133.94 秒通过，原矩阵规模、120 秒单用例/600 秒套件上限及检测项全部保留。
- 新增 5 项独立 collector 场景，含填满全部条目、长期复用年龄压缩及 1,200 拍独立参考；另有 2 项 RF 直连场景。两组均通过 RTL 与 ASan/UBSan，4 条目综合网表通过相同 5 项。此前 40 项独立工具场景未全部重跑，不重复计算复跑为新场景。
- 完整 32 条目 collector 综合为 840,085 generic cells / 102,432 FF 位；小配置 4 条目为 122,764 / 12,804。直连 8 warp 槽 RF 与 32 条目 collector 保留存储综合通过，按层次实例展开为 135 个数组、626,688 位；多端口元数据数组不等于实际 SRAM 宏。均不是整 SM 门数或工艺面积。
- 五后端同时执行四类负载各两次 Graph 重放，最终设备 64,500 拍。8 份架构状态、周期和存储统计与上一寄存器阶段一致；Transformer 的同拍 trace 顺序变化，按周期/指令归组后相等。另有两次实际 GEMM 与原版 nvidia-smi 的实卡共存、分配、释放、退出审计，监控仍是 NVML 桥接而非 CUDA 驱动枚举。
- 完整 32 条目综合网表首次编译触发 600 秒上限，保留原日志；相同命令复用已有对象文件续跑后，5 项门级检查全部通过。最终十份清单的源码哈希与两份实际网表哈希核对一致，证据在 `rtl-collector-final/evidence-audit.json`。
- 完整目标保持 active。写回请求仲裁、warp 发射和其余执行电路、collector 输出端口网络、完整 RTL 整机连接、实卡参数校准、PTX 异步矩阵协议及最终硬件审计仍需继续。

## 阶段 16 第九部分：七执行端口与条目载荷子模块（2026-10-29—30）

- 从干净的 `81bed63` 接续，上一阶段为有提交与验证的实际进展。本阶段补齐诊断查询口被宿主反复复用的执行交接边界，保持 coding-standards 的短职责函数、入口仅调用和相邻英中注释。
- 新增 `collector_dispatch.sv`、`collector_payload.sv`；每条目 3 位 pipe 字段，每个执行端口按实际 rank 选择最老的 ready/eligible 条目，七组各三个 warp 向量的物理输出。确认回到 RTL 释放槽位，同一端口不能同拍连续消费两条。候选允许逐拍重选，条目与操作数始终保留到确认，未冒充 AXI 的固定事务反压协议。
- 实际 ISA 的准入、取值与报告接入新端口，旧诊断查询口不再提供执行操作数。C++ 仍提供外部异步资源准入、跨分区 Tensor/异步归属和后续执行；没有把这些控制称为 RTL。分配每拍一条、默认额度四条和 RF 四读端口仍限制持续吞吐。
- 独立选择器新增 3 项，collector 新增 2 项、直连新增 1 项，工具集合由 47 变为 53；本次实际运行受影响的 13 项。选择器 5,000 组排列、collector 1,500 拍分派和原 1,200 拍收集使用独立年龄参考；C++ 另增三源/七端口并行、字段跨字、重复确认与阶段错误检查。
- 默认 Debug 274 项运行、272 通过/2 跳过；最终联合 Release 41 套件/835 项通过，7.19 秒；寄存器/collector Debug ASan/UBSan 32 套件/541 项通过，445.41 秒。原 120 秒单用例、600 秒套件上限与最大矩阵保留；各构建后端范围和命令见 VALIDATION。
- 初版宽可变截取令 4/32 条目综合触发 600 秒上限；固定 bank 选择后小配置通过。共享条目内准备使 4 条目从 851,890 降到 354,937 cells，但摊平结构的完整综合与 sanitizer 仍超时。最终用层次化载荷子模块和直接复制所选条目，完整容量推断综合及全部原检查通过；4 条目最终 279,821 cells / 12,816 FF 位。失败、主动终止和最终成功分开记录在 `rtl-dispatch-history` 与最终清单中。
- 完整 32 条目选择器为 8,835 组合 cells，RTL/sanitizer/门级相同 3 项通过；完整 collector RTL/sanitizer 7 项通过，4 条目综合网表同 7 项通过；直连 RTL/sanitizer 同 3 项通过。完整 32 条目 collector 和完整 RF 直连顶层保留存储综合通过，实际位数见 VALIDATION；没有宣称当前完整 32 条目数据通路已做门级仿真。
- 修复工具旧静态归档保留已移除 Verilator 分片导致重复符号的问题，构建前重建归档；复跑开始移除旧成功清单，避免失败时误读过时结果。原门级链接错误日志保留，相同最终网表重新通过。
- 五后端同时执行四负载各两次 Graph 重放，另有两次监控 GEMM。十份完整状态/trace 删除三个新增观测字段后与上一阶段全量相等；分配、端口确认和释放数量一致。原版 nvidia-smi 经桥接层验证实卡共存、分配、释放和退出。最终十二份源码清单及两份实际网表哈希核对一致。
- 阶段说明为 RTL_DISPATCH.md，当前代码与成本均区分项目设计和 NVIDIA 事实。目标保持 active；下一步仍需写回仲裁、执行锁存器和后续执行/控制电路、跨分区共享资源仲裁、完整 RTL 连接、实卡校准、PTX 异步矩阵协议和最终审计。

## 阶段 16 第十部分：有限完成存储与写回直连（2026-10-30）

- 从干净的 `6ef824f` 继续。前一轮重新验证实际 Graph GEMM 与 NVML 查询，但没有补齐硬件；本阶段落实执行结果至 RF 的存储、年龄仲裁和数据路径。继续使用 coding-standards 的短职责函数和相邻英中注释，没有子代理参与。
- 新增 `writeback_unit.sv`、`writeback_payload.sv`、`Writeback` 接口与适配器。每分区发射时预留 32 个完成槽，保存目的类型、物理 warp 槽、lane mask、生产端口和相对年龄；七类结果端口各每拍接收一次，载荷保存到退休确认。四个 RF bank 支持 1/2/4 共享端口组，谓词有单独输出，无写入指令不占 RF 端口。
- 电路输出直接驱动 RF 地址、mask 和数据。C++ 接收实际退休总线来更新转储；成功提交后清除 C++ 结果寄存器副本，不能绕过 RTL 回填寄存器。集合指令数值在执行时计算、退休时提交；软件后端保留原来的查询/退休顺序。完成槽满阻止发射，新增 `completion_stalls` 与逐分区实际握手计数。
- 首次集成揭示组合门控时钟问题：新值通过旁路可见，但写回状态同拍改变使存储未可靠写入。集成 RF 改为共用源时钟、逐 lane 写使能；定向测试要求下一拍仍能读回新值。四个失败套件恢复，早期日志保留在 `rtl-writeback-history`。新模块的旧构建规则先报缺失模块，重新配置解决；一次测试时限属性放在测试注册前的 CMake 错误也已修复并保留记录。
- 写回独立五场景涵盖七端口、容量、相对年龄、复位、拒绝非法/重复完成、3,000 拍独立发射编号参考；普通、sanitizer、4 槽和完整 32 槽实际综合网表全部通过。32 槽为 739,485 generic cells / 34,496 FF 位，4 槽为 88,869 / 4,312。没有把宽数据选择网络的成本、工艺频率或整卡门数混为一谈。
- 新增三个 C++ 适配器场景、一个实际长延迟 40 指令容量测试和一个组合 RTL 存储/旁路场景。工具独立场景由 53 增为 59，本次运行受影响的九项。完整 RF/collector/写回顶层推断为 141 个数组 / 530,144 数组位和 147,552 FF 位；仅写回单元做了完整 32 槽门级仿真，组合顶层没有。
- 默认 Debug 26 套件/274 项运行，272 通过、2 跳过；联合 Release 42 套件/839 项通过。sanitizer 首轮 33 套件中 32 通过，软件时序套件两次达到 90 秒；仅对此构建/套件调整总限时为 180 秒，原 35 项在 95.64 秒通过。最后在 `UBSAN_OPTIONS=halt_on_error=1` 下完整重跑，33 套件/545 项全部通过，532.11 秒。单用例 8 秒、RTL 120 秒及 600 秒套件限制保持；所有失败和最终验证范围见 VALIDATION.md。
- 四种主机负载各两次 Graph 重放，另两次监控 GEMM；十份报告的写回预约/完成/退休数量匹配 issued，全部占用归零。两次规约各有三条同分区 LDS 因单个 memory 完成端口晚一拍退休；最终架构状态、总周期与其他统计相同。其余同拍事件有顺序变化，不声称原始 trace 全量相等。原版 nvidia-smi 经 NVML 桥接确认实卡共存、7 MiB→0 与退出移除。
- 十份 RTL 清单与两份实际网表哈希核对通过，产物在 `rtl-writeback-final`、`rtl-writeback-host` 和相应单模块目录。阶段结构说明为 RTL_WRITEBACK.md；源时钟/有效工作周期与工艺时钟门控明确区分。
- 完整目标保持 active。执行流水线及各生产端口的输入选择仍有 C++；完整谓词寄存器存储、跨分区共享单元仲裁、其余 RTL 整机连接、实卡校准、PTX 异步矩阵协议和最终要求审计仍需继续。
