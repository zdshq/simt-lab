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
