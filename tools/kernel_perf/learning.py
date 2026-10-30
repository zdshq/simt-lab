"""Beginner lessons: a mechanism, a worked example, and a falsifiable next step.

Examples are hypothetical unless the UI explicitly labels a saved experiment.
Hardware-specific examples state their own assumptions; they are not target defaults.
"""

CONCEPTS = {
    "execution": {"name": "线程 → Warp → CTA → SM", "meaning": "线程（thread）是你写代码时的一份执行身份，有自己的变量。NVIDIA 将线程分成 32 个一组的 warp；多个 warp 组成 block，也叫 CTA。SM 是 GPU 上执行这些工作的硬件单元，不是一个线程，也不是一个 CUDA core。",
                  "example": "这个实验中，一个 256-thread block 包含 8 个 warp。一个 block 的线程在同一个 SM 上协作；整张矩阵会分给很多 block。"},
    "memory": {"name": "谁的存储，与存在哪里，是两个问题", "meaning": "寄存器通常放线程自己的临时数；shared memory 让同一 CTA 内的线程协作；local memory 也是线程私有，但通常由设备内存提供后备存储。L1、L2 是缓存，保存访问过的数据副本；DRAM 是显存。",
               "example": "Local 的意思是“这个线程私有”，不是“离计算单元很近”。Local 数据可能在缓存命中，也可能需要访问显存。"},
    "sector": {"name": "Byte、sector、访问合并", "meaning": "Byte（B，字节）是数据大小单位；一个 float 占 4 B。这里的 sector 是硬件计数用的 32 B 小块。一个 warp 的多线程访存可能合并，所以一次 warp 指令、一个线程的操作、一个 sector 是三种不同的计数对象。",
               "example": "理想对齐、连续访问下，32 个线程各访问 4 B，总共 128 B，覆盖 4 个 sector。不能按“32 个线程＝32 个 sector”计算。"},
    "rate": {"name": "容量、访问量、吞吐、延迟", "meaning": "容量问“最多能放多少”；访问量问“这段时间搬了多少”；吞吐问“每秒搬多少/做多少”；延迟问“一次操作要等多久”。同一份数据反复搬，访问量能远大于容量。",
             "example": "一个 128 B 的缓冲反复读 1000 次，占用空间仍是 128 B，但有用读取量累计为 128000 B。等待很久，也不一定意味着每秒搬得很多。"},
    "register": {"name": "寄存器与存活变量", "meaning": "可以把寄存器想成线程手边放临时数的小格子。只要一个数后面还会使用，它就需要被保留；不同生命周期的变量可能复用同一个寄存器。warp 内线程不会因为同属一组，就把各自寄存器变成共享变量。",
                 "example": "线程 0 的 x 和线程 1 的 x 是各自的值。用 shuffle 等指令可以传值，但不是两个线程直接读写同一个私有寄存器变量。"},
    "spill": {"name": "Spill、编译期与运行期", "meaning": "编译器需要保留的数放不进可用寄存器时，可能把一部分暂存到 local memory，之后再读回，这叫寄存器 spill。编译期报告说明生成了什么；NCU 运行期计数说明这次执行发生了多少访问。",
              "example": "编译期生成了一条溢出写指令，如果它在循环里执行很多次，动态写访问会很多。栈空间和编译期 bytes 不能直接当成整个 kernel 的流量。"},
    "schedule": {"name": "驻留 → 准备好 → 发射", "meaning": "驻留（active/resident）表示 warp 已经分到 SM 的资源。准备好（eligible）表示它下一条指令的数据和执行条件已满足。调度器（scheduler）再选择可执行的 warp，把指令交给执行单元，这叫发射（issue）。",
                 "example": "8 个 warp 都驻留，其中 7 个等数据，只有 1 个准备好：active 可以是 8，eligible 只有 1。人数多，不等于每个人都能立刻干活。"},
    "dependency": {"name": "依赖、scoreboard 与 stall", "meaning": "如果先做 x=加载数据，再做 y=x+1，第二条必须等第一条得到 x。硬件记录哪些结果还没准备好，这种依赖跟踪称为 scoreboard。warp 因等待而暂时无法发射时，NCU 会记录 stall 原因。",
                   "example": "等 x 的 warp 可以暂停，调度器尝试运行另一个准备好的 warp；这叫隐藏延迟，不是把读取 x 本身变快。"},
    "pipeline": {"name": "流水线与 stages", "meaning": "流水线尝试同时推进不同批次的工作：计算当前批数据时，为下一批加载数据。stage 是为在途批次安排的一层缓冲/处理位置，不是某个线程要计算多少行。",
                 "example": "餐厅可以炒第 1 份菜，同时准备第 2 份食材。多准备几份可能减少等待，但要更多台面；类比只帮助理解重叠，不代表 GPU 的具体时序。"},
    "gemm": {"name": "GEMM 的 M、N、K", "meaning": "矩阵乘法 A[M,K] × B[K,N] 得到 D[M,N]。一个输出 D[m,n] 要把 A 的一行与 B 的一列逐项相乘，再沿 K 把结果加起来。沿 K 合并部分和，叫归约。",
             "example": "M=2、N=3、K=4：输出有 2×3=6 个数，每个数需要 4 次乘法及求和。M/N 决定输出平面，K 决定每个输出的计算长度。"},
    "tile": {"name": "CTA tile、warp tile、指令 tile", "meaning": "大矩阵先切成 CTA 的任务块，再让 CTA 中的多个 warp 分工。Tensor Core 指令还会处理更小的形状。这三个层级不是同一个东西，也不能把 warp tile 当成一个线程的矩阵。",
             "example": "当前 K 方向不拆 warp 的 GEMM：CTA128×128、warp32×32，M/N 各分 4 份，一共 4×4=16 个 warp。"},
    "peak": {"name": "百分比、峰值与 active/elapsed", "meaning": "“相对峰值百分比”是某种工作速率相对于其峰值。“命中率”是命中请求占请求总数。“occupancy”是驻留 warp 相对驻留上限。它们都用 %，但分母不同。名称里的 active 按相应单元的活跃周期计算，elapsed 按完整测量区间计算；活跃不等于所有执行通路都满载。",
             "example": "20/100 次请求命中是 20%；8/48 个 warp 驻留是约 16.7%。不能把两个百分比加起来说 GPU 使用了 36.7%。"},
    "measurement": {"name": "Kernel launch、benchmark 与 replay", "meaning": "Kernel 是在 GPU 上执行的计算函数；launch 是启动它一次。Benchmark 是为了测正常运行时间而重复执行。NCU 为采集不同计数器，可能把同一个 launch 重放多轮，叫 replay；分析器的总耗时包含这些额外工作。",
                    "example": "NCU 为一个 1 ms kernel 做多轮 replay，整个分析过程可能花很久。这不表示正常运行的 kernel 变成了几秒。"},
    "bank": {"name": "Shared memory 的 bank", "meaning": "Shared memory 划分为多个可并行访问的小存储通道，叫 bank。不同线程的地址如果落到同一 bank 的不同位置，可能需要分批服务。它不是缓存查找失败。",
             "example": "在简化的 32-bit 访问例子中，连续 32 个 float 可分散到 32 个 bank；隔 32 个 float 访问可能集中到同一 bank。访问同一个字还有广播规则，不能一概算冲突。"},
    "units": {"name": "ms、B、KiB、GB/s 与 TFLOP/s", "meaning": "1 ms=0.001 s；1 KiB=1024 B；1 GB=10⁹ B；1 GiB=2³⁰ B。B 是字节，bit 是位，8 bit=1 B。TFLOP/s 是每秒万亿次浮点运算，不是字节数。",
              "example": "FP16 每个元素 2 B、FP32 每个元素 4 B。16384 个 FP16 元素是 32768 B，也就是 32 KiB，而不是 16 KiB。"},
    "tools": {"name": "ptxas、NCU、Source / SASS", "meaning": "ptxas 是 CUDA 编译过程中的工具，它把中间代码变成 GPU 可执行的机器代码，并能报告寄存器和 spill。NCU 是 Nsight Compute 的命令行工具，采集运行时指标。SASS 是 GPU 机器指令；Nsight 的 Source 页面用于把指令或源码位置与计数、等待位置联系起来。",
              "example": "编译日志告诉你“生成了 spill 读写”；NCU 告诉你“这次运行有多少 local 访问”；再到 Source/SASS 找发生访问的指令。三者回答的是不同问题。"},
}

LEARNING_PATH = [
    {"title": "先知道谁在干活", "why": "把软件线程与硬件 SM 分开，才不会把“线程多”当成“算得快”。", "concepts": ["execution", "tile", "gemm"], "metrics": ["threads_per_block", "warps_per_block", "grid_blocks"]},
    {"title": "再知道数据放在哪", "why": "先分清私有/共享和缓存/显存，再读 Local、L1、L2。", "concepts": ["memory", "register", "sector", "rate"], "metrics": ["local_store_sectors", "spill_store_bytes", "l2_hit_pct", "l2_pct"]},
    {"title": "理解为什么有人在等", "why": "驻留的 warp 不一定准备好；等待时间也不等于带宽耗尽。", "concepts": ["schedule", "dependency", "peak"], "metrics": ["occupancy_pct", "eligible_warps", "long_scoreboard_ratio"]},
    {"title": "最后做一次有依据的对照", "why": "一次只改一个参数，同时观察耗时和相关证据，不追求所有百分比变绿。", "concepts": ["measurement", "pipeline", "units"], "metrics": ["native_time_ms", "stages", "cache"]},
]


def lesson(question, intuition, concepts, steps, worked, decisions, takeaway, quiz, simulator=None):
    return {"question": question, "intuition": intuition, "concepts": concepts,
            "steps": [{"title": title, "text": text} for title, text in steps],
            "worked": {"assumptions": worked[0], "lines": worked[1], "conclusion": worked[2]},
            "decisions": [{"when": when, "meaning": meaning, "next": next_step} for when, meaning, next_step in decisions],
            "takeaway": takeaway, "quiz": {"question": quiz[0], "answer": quiz[1]}, "simulator": simulator}


def lessons():
    result = {}
    for key, action, verb in (("local_store_sectors", "写入", "store"), ("local_load_sectors", "读取", "load")):
        result[key] = lesson(
            f"这次运行{'向' if verb == 'store' else '从'} local memory {action}数据，累计访问了多少个 32 B 小块？",
            "想象你有一个很小的临时本子。同一页反复翻写会产生很多访问，但本子并不会因此变大。这个指标数的是访问过的小块，不是本子的页数。",
            ["execution", "memory", "sector", "rate", "spill"],
            [("先解释 Local：属于谁，而不是离得多近", "线程自己的变量优先尝试放进寄存器，但数组、栈对象或寄存器溢出的数可能放进 local memory。它虽然是线程私有，却可能经过缓存甚至显存；不要与 CTA 共享的 shared memory 混淆。"),
             (f"再解释 {verb}：数据的方向", f"Store 是把数写出去，load 是把数取回来。这里限定为 local 地址空间的{action}。矩阵 A/B 的一般 global 访问不算进这个 local 专用计数器；其他访存有各自的计数。"),
             ("最后解释 sectors：按 32 B 小块累计", "硬件可以合并一个 warp 中多个线程的访存。计数器按覆盖的 sector 累计，而不是看源码写了几次赋值。相同小块被反复访问会重复计数；这是整个被分析 launch 的总量，不是每线程的值。"),
             ("为什么它可能让计算变慢", "如果计算中间经常存出去、又读回来，就增加了数据搬运和等待。是否真的拖慢，要看原生耗时、L2 等访问压力以及编译期 spill 证据，不能只看到一个大数字就定罪。")],
            ("教学假设：一个完整 warp 的 32 个线程各访问同一个相对位置的 float；这次访存在统计位置连续、按 32 B 对齐、可完全合并；每轮只算这一条 local 访问。不是保证你的代码会生成这种指令。",
             ["1 个 float = 4 B", "每轮：32 threads × 4 B = 128 B", "每轮覆盖：128 B ÷ 32 B/sector = 4 sectors", "反复访问相同位置 1000 轮：4 × 1000 = 4000 sectors", "访问量折算：4000 × 32 B = 128000 B ≈ 125 KiB"],
             "这只是对相同 128 B 数据反复访问的例子：不是占用了 125 KiB，也不是 4000 条 warp 指令，更不保证有 125 KiB 到达 DRAM。"),
            [("数值很大", "说明这一统计位置出现较多 local 访问，不能单凭总数判断相对效率。", "固定 M/N/K 后对比；检查 ptxas spill loads/stores 与 L2、原生耗时是否一起变化。"),
             ("数值为 0", "这次采集未观察到这类 local 访问，不等于所有执行路径都永远没有 spill。", "与编译证据一起读；确认是正确的 kernel launch。"),
             ("显示 —", "没有可用测量，不是零，也不是正常。", "先确认 NCU 是否采到这个计数器。")],
            "Local 说归属，store/load 说方向，sector 说计数粒度；访问量不是占用空间。",
            ("同一个 32 B 小块被访问 100 次，计数会是 1 还是 100？", "在每次都产生一次被统计 sector 访问的假设下是 100；空间仍可只有 32 B。缓存会影响下游流量，不能把此值直接当显存传输。"), "sectors")

    for key, tier, companion in (("l1_pct", "L1/TEX", "L1 命中率、shared 访问和 bank conflicts"),
                                 ("l2_pct", "L2", "L2 命中率、local sectors 和 DRAM 吞吐"),
                                 ("dram_pct", "DRAM 显存", "DRAM 字节量、L2 命中率和计算量"),
                                 ("memory_pct", "存储系统", "L1/TEX、L2、DRAM 各自的吞吐"),
                                 ("compute_pct", "SM 计算", "Tensor 流水线、eligible warps 和原生耗时")):
        result[key] = lesson(
            f"{tier} 处理工作的速度，离相应峰值还有多远？",
            "把它当成柜台的处理速度，不是仓库的装满程度。柜台一分钟处理了很多单据，仓库可能很空；柜台很闲，也可能是一直在等一张迟迟不到的单据。",
            ["rate", "peak", "memory" if key != "compute_pct" else "schedule"],
            [("先确定它数的是哪种资源", f"这个指标针对 {tier}。L1/TEX、L2、DRAM 是不同统计位置；Compute 也不是所有 CUDA core 随时在干活的直接百分比。要先看名称，再看数值。"),
             ("百分比的分母是相应峰值", "Nsight 的 throughput 聚合指标会取相关组成计数器中最大的百分比，帮助找到可能最忙的路径。90% 不能读成缓存空间用了90%，也不能把各级的90%相加。"),
             ("高和低都需要第二份证据", f"优先一起看 {companion}。当线程在等依赖、同步或数据时，吞吐也可能低；反过来，大量无效重复访问也能制造高吞吐。")],
            ("下面是假设计数器，不是你的 GPU 峰值规格。", ["组成路径 A：相对峰值 91%", "组成路径 B：相对峰值 30%", "聚合 throughput = max(91%, 30%) = 91%", "不是 91% + 30% = 121%，也不是容量占用 91%"],
             "下一步应该展开组成路径，而不是直接认定所有相关单元都已满载。"),
            [("接近峰值", "某条相关吞吐路径很忙，是值得排查的候选。", f"结合 {companion}，再看改动能否降低耗时。"),
             ("很低但运行仍慢", "可能是延迟、依赖、并行工作不足，而不是没有优化空间。", "看 eligible warps 和 stall；不要直接把 tile 一味调大。")],
            "吞吐看速度，不看容量；高吞吐既可能来自有效工作，也可能来自额外搬运。",
            (f"{tier} throughput=90%，能说存储空间还剩10%吗？", "不能。这里没有测容量剩余；它把相关工作速率与相应峰值作比较。"))

    for key, tier in (("l1_hit_pct", "L1"), ("l2_hit_pct", "L2")):
        result[key] = lesson(
            f"请求到 {tier} 找数据时，有多少能直接找到？",
            "像查一本常用笔记：要找的内容已经记在里面就是命中，没有就要去别处找。找到得多不代表翻笔记不花时间，也不代表查找次数少。",
            ["memory", "sector", "rate", "peak"],
            [("命中和 miss 是什么", f"当一次请求在 {tier} 找到有效数据，就叫 hit；找不到是 miss。这个比例按相应 sector 请求统计，不是“你的矩阵有多少比例放在缓存里”。"),
             ("低命中率为什么不必然是坏事", "第一次读取、不打算再用的数据、流式处理、某些缓存绕过策略，都可能减少命中。如果算法本来就只读一次，强行追求高命中率未必有意义。"),
             ("高命中率为什么也可能慢", "一亿次请求即使99%命中，命中的9900万次仍要由缓存服务。反复读写 spill 产生的local数据，就可能使L2命中高、L2吞吐也高。")],
            ("假设本次统计1000个sector请求，其中990个命中。", ["命中率 = 命中请求 ÷ 总请求 × 100%", "990 ÷ 1000 × 100% = 99%", "若请求总数变成一亿、仍99%命中：命中请求就是9900万"],
             "百分比相同，不代表处理的总访问量相同，更不能据此推算存了多少字节。"),
            [("命中率低", "先问数据是否应该被重复使用，再考虑容量/冲突问题。", "固定输入，检查加载策略以及下游访问量，别只凭50%阈值改tile。"),
             ("命中率高、吞吐也高", "缓存可能在处理大量重复请求。", "看local sectors、总字节量和原生耗时，排查不必要的访问。")],
            "命中率说“找没找到”，吞吐说“处理多快”，容量说“能存多少”。",
            ("99%命中和90%吞吐能同时出现吗？", "能。命中请求照样消耗缓存资源；两个百分比的分母也不相同。"))

    for key in ("registers_per_thread", "register_capacity_pct_nominal"):
        result[key] = lesson(
            "一个线程最终分到多少寄存器？一个 CTA 名义上要占多少？",
            "把寄存器看成每个线程手边的小格子。整个SM的格子总量有限；但程序里有多少个变量名，并不等于最终需要多少格子。",
            ["execution", "register", "spill", "units"],
            [("变量数与寄存器数为什么不相等", "有些变量会被消掉，有些生命周期不重叠可以复用格子；向量、地址、中间计算也会需要格子。最终分配由编译器和具体指令决定，不能只数源码中的float个数。"),
             ("线程私有与SM资源池并不矛盾", "每个线程的普通寄存器值是私有的，但所有驻留线程的分配加起来要受SM总资源约束。warp内线程可以通过特定指令传值，不是直接共享同一个寄存器变量。"),
             ("为什么正好65536也能spill", "显示的是最终已经分配成功的数量。如果编译器原先需要保留更多数，可以把一部分安排到local后才做到64寄存器/thread；因此64×1024恰好达到上限，并不能证明没有溢出。")],
            ("本实验的 SM89 示例：65536个32-bit寄存器/SM；这是名义计算，未计分配粒度。", ["每线程64个 × 每CTA1024线程 = 65536个寄存器/CTA", "65536 × 4 B = 256 KiB 的名义寄存器存储量", "1 CTA/SM 时名义占比 = 65536 ÷ 65536 = 100%", "物理分配没有超限，不代表所有存活变量都留在寄存器里"],
             "要判断spill，读ptxas溢出统计；要判断速度，比较原生耗时。名义寄存器占比不是寄存器执行单元的忙碌率。"),
            [("寄存器/thread 很高", "可能限制驻留CTA，也可能是为了提高数据复用和计算效率。", "一起看理论驻留上限、spill与原生耗时，不要仅按数值大小排名。"),
             ("人为限制后寄存器变少", "减少的数可能去了local，而不是凭空不需要了。", "重新检查spill、local访问和时间；不要仅凭occupancy提高宣告成功。")],
            "这里测的是编译后的资源分配量，不是变量个数，也不是每个时刻寄存器的忙碌比例。",
            ("把每线程寄存器从90限制为64，一定会更快吗？", "不一定。可能多驻留一些warp，也可能引入spill；必须一起比较访存和耗时。"))

    for key in ("spill_store_bytes", "spill_load_bytes", "stack_frame_bytes", "local_bytes_per_thread"):
        result[key] = lesson(
            "编译器有没有把临时数放到线程的栈/local 存储？这是空间还是流量？",
            "桌面不够时，你把文件暂时放进抽屉，之后再取出。抽屉有多大、说明书写了哪些取放动作、一天实际取放多少次，是三个不同问题。",
            ["register", "spill", "memory", "measurement", "tools"],
            [("先认清四种字段", "Spill stores/loads 是ptxas给出的编译期溢出访问统计；stack frame是函数栈框架的大小；CUDA local bytes/thread是函数属性里的线程local存储大小。它们不是同一个字段。"),
             ("什么才算直接的spill证据", "目标kernel的ptxas spill stores或loads非零，说明编译器生成了spill。只看到stack frame或local bytes非零不够，因为局部数组、函数栈等也可能需要空间。"),
             ("编译期bytes为什么不能拿来算总流量", "编译器报告并不知道你本次循环执行多少次、哪些分支真正走到。动态访问应由NCU的local计数器观察；一个有spill的路径也可能本次没走到。"),
             ("为什么必须对应到同一个kernel", "程序可能还启动reference、初始化或merge kernel；它们的资源不属于主GEMM。这里优先读这次构建绑定的目标kernel日志，证据缺失时不能用别的kernel代替。")],
            ("以已有构建日志的字段形式说明含义；不是把编译期值当运行流量。", ["Used 64 registers → 最终每线程分配64个寄存器", "120 bytes stack frame → 栈空间统计", "144 bytes spill stores → 生成了编译期spill写访问", "120 bytes spill loads → 生成了编译期spill读访问", "动态local访问 → 还要看这次NCU采集，不能算成144+120 B总流量"],
             "寄存器64与spill非零可以同时成立。不要把stack frame本身当spill，也不要把spill bytes乘线程数冒充实测流量。"),
            [("Spill loads/stores 非零", "已确认生成spill，但还没量化它对本次耗时的影响。", "对照local sectors、L2吞吐和原生耗时。"),
             ("两项spill都是0，local访问非零", "local对象也能有访问，这两份证据并不必然冲突。", "查看数组/栈对象及Source/SASS中的访问位置。"),
             ("只有stack/local大小，没有spill日志", "是否spill仍未知。", "补充本次构建的ptxas输出，不把缺失数据解释成0。")],
            "编译期证据确认“生成了什么”，运行期计数说明“这次发生了多少”。",
            ("Stack frame=120 B 能单独证明寄存器spill吗？", "不能。必须看目标kernel的spill loads/stores或进一步的指令证据；栈还有其他用途。"))

    for key in ("occupancy_pct", "theoretical_occupancy_pct", "active_blocks_per_sm"):
        result[key] = lesson(
            "一个 SM 能同时容纳多少 warp？实际容纳了多少？",
            "像车间里有多少工位被小组占着。坐在工位上不代表手头有活；有人可能一直在等材料。occupancy描述驻留，不直接描述计算速度。",
            ["execution", "schedule", "register", "peak"],
            [("先区分CTA、warp与硬件上限", "一个CTA分成若干warp。寄存器、shared、线程数等限制一个SM能同时留住几个CTA；不是grid里有多少CTA，就能全部同时挤进来。"),
             ("理论occupancy如何得到", "先估算或由CUDA API计算每SM最多驻留几个CTA，再换成warp数，与该架构的warp驻留上限相除。实际分配还有粒度约束，不能只看某一种资源。"),
             ("实际occupancy又是什么", "NCU观察运行中活跃周期的平均驻留情况。grid不足、收尾阶段和任务不均衡会影响结果；它不是“Tensor Core已经用满多少”的读数。"),
             ("为什么更高不保证更快", "多留住一些warp可能掩盖等待，但也可能改变tile复用、指令开销和spill。关键是这些warp里有多少准备好发射，以及总时间有没有下降。")],
            ("SM89/4070 SUPER 的示例：每SM最多驻留48个warp。假设这个kernel资源只允许1个256-thread CTA/SM。",
             ["256 threads ÷ 32 = 8 warps/CTA", "1 CTA/SM × 8 = 8 resident warps/SM", "理论occupancy = 8 ÷ 48 × 100% ≈ 16.7%", "若换配置能驻留32个warp：32 ÷ 48 ≈ 66.7%"],
             "这些百分比只描述驻留。32个warp如果全等同一个瓶颈，仍可能比8个高效工作的warp慢。"),
            [("理论上限就低", "通常有某类资源限制了驻留。", "查register/shared/warp block limit，再决定哪种tile改变值得试。"),
             ("实际远低于理论", "可能grid不足或调度分布不均。", "查grid blocks、waves与任务形状，而不是先强压寄存器。"),
             ("occupancy高但慢", "驻留的warp不一定能发射。", "看eligible、issue和stall，排查真正的等待。")],
            "Occupancy是“住了多少”，不是“干得多快”；active blocks/SM是理论驻留CTA上限。",
            ("Occupancy从16.7%变66.7%，性能应当正好4倍吗？", "不能这样推算。它只把驻留warp比例提高了4倍，没有保证有效指令或数据供应也提高4倍。"))

    for key in ("active_warps", "eligible_warps", "issue_pct"):
        result[key] = lesson(
            "已经在 SM 上的 warp，有几个真正准备好执行下一条指令？",
            "小组都到了车间（active），拿到材料才可开工（eligible），管理员安排某组把一项工作交给机器（issue）。三步不是同一件事。",
            ["execution", "schedule", "dependency", "peak"],
            [("Active 不代表正在做计算", "一个驻留warp即使等数据或同步，仍可以算active。页面Active / scheduler是每个调度器在相应活跃周期中的平均驻留数，不是全卡warp总数。"),
             ("Eligible 是眼下能被选中的候选", "下一条指令需要的输入与执行条件满足后，warp才eligible。不是有了更多驻留warp就必然有更多eligible；它们可能都在等待。"),
             ("Issue 才是发出指令", "调度器从准备好的warp中选择发射。发射率不等于FLOP完成率：一条地址计算指令和一条Tensor指令完成的数学工作量不同。"),
             ("平均数为什么能小于1", "0.1不是把一个物理warp切成十分之一，而是跨时间/调度器平均的结果。看数值前应先确认它是warp/scheduler、百分比，还是cycles/issued instruction。")],
            ("为说明平均数，只看一个假想调度器的100个周期；该例不用于推算具体GPU峰值。", ["100个周期中都有8个warp驻留 → 平均active=8", "其中90个周期没有eligible，10个周期各有1个eligible", "平均eligible = (90×0 + 10×1) ÷ 100 = 0.1", "因此 active=8 与 eligible=0.1 完全可能同时出现"],
             "平均有8个warp不代表每周期有8个可执行。此时值得先查等待原因，而不是继续机械增加线程。"),
            [("Active多、Eligible少", "大量驻留warp正因某种条件不能发射。", "查long/short scoreboard、barrier、math throttle的具体位置。"),
             ("Eligible少、吞吐也低", "可能缺乏能隐藏等待的工作，而非带宽已满。", "结合grid和流水重叠分析，再试tile/stages。"),
             ("发射率改善", "可能减少空闲发射周期，但指令效率也重要。", "最后确认原生时间有没有缩短。")],
            "Active是驻留，eligible是准备好，issue是把指令发出去；平均0.1不是0.1个物理warp。",
            ("Active=8、Eligible=0.1，能说有8个warp同时在算吗？", "不能。它说明平均8个warp已驻留，但可发射候选很少；还要看实际issue。"))

    result["warp_latency_cycles"] = lesson(
        "一个 warp 平均隔多少个周期，才发出下一条指令？",
        "想象只跟踪车间里的一个小组：做完一项安排后，它要等多久才拿到下一次安排？别的小组可能在这段时间工作，所以这个间隔不是整个车间的停工时间。",
        ["execution", "schedule", "dependency", "measurement"],
        [("周期不是毫秒", "Cycle是硬件时钟推进一步的时间单位。这里报告平均每条已发射指令对应的warp周期数；它不是kernel持续了多少毫秒。"),
         ("间隔里可能包含什么", "等待加载结果、计算依赖、执行通路或调度机会，都可能影响这个间隔。这个总指标不负责单独告诉你是哪一种原因。"),
         ("为什么不能当作指令本身的计算延迟", "一条指令发出到结果返回的时间，与同一warp多久能发出下一条指令，是不同问题；如果下一条不依赖该结果，有机会继续推进。"),
         ("为什么不能乘全卡指令数算耗时", "多个warp、多个SM并行工作，许多间隔互相重叠。平均warp间隔不能直接相加成GPU墙钟时间。")],
        ("简化为只观察一个warp的4个发射间隔，演示平均数；实际NCU还有跨warp/调度器聚合。",
         ["4个间隔分别是：4、8、12、16 cycles", "合计：4+8+12+16=40 cycles", "平均发射间隔：40/4=10 cycles", "另一warp可能在这些间隔内发射指令"],
         "10 cycles是平均间隔，不是10%的耗时，也不能说全SM每10个周期才工作一次。"),
        [("平均间隔变长", "同一warp的指令推进更稀疏，但还不知道具体原因。", "一起看eligible、issue，以及各类stall。"),
         ("间隔降低但总耗时增加", "可能总指令数增加，或其他阶段/数据路径变慢。", "比较相同输入下的总指令、访问量与原生时间，不能只追求小间隔。")],
        "它看单个warp指令推进的平均间隔，不是整张卡停工多久。",
        ("这个指标=10 cycles，是否说明一条Tensor指令恰好要10个周期才算完？", "不能。它观察的是warp发射间隔，而不是单独测量那条Tensor指令的结果延迟。"))

    reasons = {
        "long_scoreboard": ("加载的数还没准备好", "例如从global/local加载x，随后执行y=x+1。x尚未返回时，依赖它的指令不能继续。这一类跟踪的是L1/TEX相关操作，并不是直接测量DRAM等待时间。", "检查是哪个加载产生了被等待的结果，再看local流量、缓存和访问方式。"),
        "short_scoreboard": ("MIO 相关结果还没准备好", "MIO可先理解为一组内存输入输出及相关操作通路。常见场景是从shared读x后马上使用x，结果没就绪；特殊数学等操作也可能有关，不能看到short就认定bank conflict。", "先在Source确认操作类型；如果是shared，再查bank conflicts和访问布局。"),
        "math_throttle": ("所需数学执行通路暂时接不下新工作", "下一条计算指令需要某类数学流水线，但它暂时不能接收更多指令。不是说整张GPU所有计算单元都达到峰值；可能只是当前指令集中使用一条通路。", "检查是哪种数学指令和流水线，再看Tensor/Compute及指令混合情况。"),
        "barrier": ("同一CTA里的同伴还没到同步点", "一个warp已到同步点，但其他warp还在完成之前的工作。它必须等，避免读取尚未写好的shared数据。同步本身可能是保证正确性所必需的。", "先找同步前各warp为何到达不齐；不能为了消除等待直接删掉同步。"),
    }
    for prefix, (reason, mechanism, action) in reasons.items():
        for suffix in ("ratio", "pct"):
            key = f"{prefix}_{suffix}"
            ratio = suffix == "ratio"
            result[key] = lesson(
                f"Warp 为什么没法发射下一条指令？这里关注：{reason}。",
                "别把stall理解成“GPU坏了”。它是一张等待原因清单，帮助你知道接下来查数据供应、计算通路，还是同伴同步。",
                ["schedule", "dependency", "peak"] + (["bank"] if prefix == "short_scoreboard" else []),
                [("这类等待在代码中如何产生", mechanism),
                 ("为什么等待不必然拖住整个SM", "等着的warp可以暂时让出发射机会。若另一个warp准备好了，调度器可以发它的指令。因此要把stall与eligible、issue一起看，尤其关注没有可发射工作的情况。"),
                 ("先看分母，别把ratio当百分比", "本页 / issue（ratio）字段按发射活动归一化，不能读成总耗时占比。" if ratio else "本页是按活跃warp口径归一化的百分比，不等于整个kernel墙钟时间有这么多都完全空转。"),
                 ("不要从等待比例直接推算加速", "执行存在重叠，改变一处也可能让另一处成为限制。消除某种等待不保证等比例提速，更不能把几种不同口径的等待值加成总耗时。")],
                ("下面只演示分母的差异，不替代NCU在硬件上的完整聚合计算。", ["假设某等待计数=2400，发射活动计数=100", "按issue归一化：2400 ÷ 100 = 24（ratio）", "若另一个指标用10000作分母：2400 ÷ 10000 ×100% = 24%", "数字都叫24，不代表同一个含义"],
                 "所以看到 / issue=24，应读作该归一化比值为24，不能说总时间的24%被浪费。"),
                [("该等待较多、Eligible也少", "它可能是限制可发射工作的线索。", action),
                 ("等待较多，但原生时间已经更短", "新配置可能用更少的总指令/访问完成了工作。", "不要只优化某一个等待数字；同时比较工作量和最终耗时。")],
                "先弄清在等什么，再确认分母；stall是定位线索，不是直接的提速百分比。",
                ("Math throttle高就说明整张卡算力吃满了吗？" if prefix == "math_throttle" else "某等待ratio=24，可以说它占了总时间24%吗？",
                 "不能，可能只是一条数学通路承压，要结合具体pipeline和SM吞吐。" if prefix == "math_throttle" else "不能。ratio和总时间百分比的分母不同，还存在多warp并行与等待重叠。"))

    for key, action in (("shared_bank_load", "读"), ("shared_bank_store", "写")):
        result[key] = lesson(
            f"一个 warp {action} shared memory 时，地址是否挤在同一条存储通道？",
            "像排队到32个窗口办理业务：分散到不同窗口可并行，很多人挤在同一窗口的不同业务就要分批。这里不是说仓库空间不够。",
            ["execution", "memory", "bank", "sector", "tools"],
            [("Shared为什么能快", "同一CTA的线程可以把复用的数据放在shared中协作访问，它由多个bank提供并行服务。是否分散到不同bank，取决于地址布局，而不只是总字节数。"),
             ("冲突如何产生", "对于简单32-bit访问，可以用字地址对32取模来帮助理解bank分布。不同线程访问同一bank的不同字，会需要额外服务；同一字读取还可能广播，不应一律当冲突。"),
             ("为什么不能拿源码一眼推断复杂kernel", "ldmatrix、向量访问和CUTLASS的swizzle布局更复杂；教学例子不是实际Tensor加载的完整事务模型。要看生成的指令和NCU统计。")],
            ("仅作32-bit shared访问教学：一个warp的lane从0到31；这里观察bank分布，不冒充实测bank-conflict计数。", ["连续：shared[lane] → bank = lane mod32 → 分布到32个bank", "跨距32：shared[lane×32] → bank=(lane×32) mod32=0", "第二种访问落在同一个bank的不同地址，通常需要分批服务", "读同一个shared[0]是不同情况，可能广播"],
             "问题在地址映射，不在“shared或L1装满”。实际复杂指令仍需单独分析。"),
            [("冲突计数高", "可能有shared服务开销，但也可能只是总访问次数多。", "在相同工作量下比较，定位Source/SASS后再改layout。"),
             ("冲突降低但耗时没降", "原先可能不是关键限制，或新布局引入了其他开销。", "检查short scoreboard、指令数量和原生耗时，不保证冲突为0就最快。")],
            "Bank conflict是访问通道冲突，不是缓存miss，更不是容量溢出。",
            ("把shared分配得更大，一定能减少bank conflict吗？", "不能。关键通常是地址到bank的映射；增大分配本身并没有改变这种映射。"))

    result["shared_bank_conflicts"] = lesson(
        "为什么 LDGSTS 的冲突计数不能直接当成普通 Shared load/store 冲突？",
        "global→shared 的异步拷贝和 Tensor Core 后续读取是两条不同的观测路径。NCU 的 LDGSTS 专用计数器只对应前者，不能替代普通 Shared 读写计数。",
        ["memory", "bank", "pipeline", "tools"],
        [("先看数据方向", "cp.async/LDGSTS 从 global memory 把数据写入 shared memory；目标地址在 shared，源地址在 global。它不是普通的 shared load 指令。"),
         ("再分开读计数器", "Shared load/store conflicts 观察普通 Shared 访问；LDGSTS path counter 观察 global→shared 异步拷贝路径。两个计数器不能直接相加。"),
         ("最后结合 SASS 和耗时", "如果 SASS 使用 LDGSTS，先确认它的 cache access/bypass 形式，再在相同工作量下比较计数、stall 和原生耗时。单独一个 LDGSTS 计数不能证明 Tensor Core layout 有问题。")],
        ("本实验中，2fbc58 的普通 Shared load/store conflict 为0，而 LDGSTS 专用计数器非零。", ["普通 Shared load conflict = 0", "普通 Shared store conflict = 0", "LDGSTS path counter = 非零", "因此不能把最后一项标成普通 Shared bank conflict"],
         "先区分预取路径与 Tensor Core Shared 读取，再判断是否存在真正的 bank-conflict 性能问题。"),
        [("普通 Shared load/store 为0", "没有证据表明普通 Shared 读写存在冲突。", "检查 LDGSTS 计数器和 SASS，不要直接修改 Shared layout。"),
         ("LDGSTS 专用计数器非零", "说明异步 global→shared 路径有对应计数，不自动等于 Tensor Core 读取冲突。", "固定输入，比较 .cg/.ca、原生耗时和相关 stall。"),
         ("计数降低但耗时不变", "该计数可能不是当前主要限制。", "以原生耗时和端到端吞吐为最终判断。")],
        "先按指令路径拆分 bank-conflict 计数；普通 Shared 冲突与 LDGSTS 路径计数不能混为一谈。",
        ("LDGSTS 专用计数器非零，能否直接说明 Tensor Core 的 shared layout 有 bank conflict？", "不能。它首先说明 LDGSTS 路径有对应计数；还需要结合普通 Shared load/store 指标、SASS 和耗时判断。"))

    for key, tier in (("l2_bytes_per_second", "L2"), ("dram_bytes_per_second", "DRAM")):
        result[key] = lesson(
            f"{tier} 平均每秒处理多少字节？",
            "交通流量可以统计这段时间一共通过多少货物，也可以除以时间得到每秒通过多少。这里读的是速率，不是仓库大小。",
            ["rate", "units", "memory"],
            [("先认清统计位置", "L2扇区带宽由该级sector/s乘32 B换算；DRAM带宽是显存侧的字节速率。缓存命中使L2和DRAM看到的流量可能非常不同。"),
             ("再把字节和时间单位统一", "字节数要除以秒，不是直接除以毫秒后就称B/s。界面的GB/s用10⁹，不是GiB/s的2³⁰。"),
             ("有效工作与访问量分开", "带宽高可能是复用不足或spill制造了更多流量。更少的访问量使算子更快时，测得带宽未必更高。")],
            ("假设统计位置累计1 GB访问，kernel持续2 ms；只演示单位换算。", ["1 GB = 10⁹ B", "2 ms = 0.002 s", "带宽 = 10⁹ B ÷ 0.002 s = 5×10¹¹ B/s", "= 500 GB/s"],
             "500 GB/s是这个位置的工作速率。不能把L2的这个数与显存标称带宽不加区分地比较。"),
            [("带宽大且耗时长", "可能搬运了大量数据，需要看总量和必要性。", "检查复用、local流量及缓存命中，再做同输入对照。"),
             ("带宽小但耗时长", "可能是延迟或并行不足，而非带宽饱和。", "看eligible、依赖和grid，先定位等待。")],
            "带宽 = 访问量 / 时间，但必须是在同一个统计位置、同一种单位。",
            ("L2带宽很高、DRAM很低，是否一定测错？", "不是。请求可能大量命中L2，不需要同样多的流量到达DRAM。"))

    for key in ("cta_m", "cta_n", "cta_k", "warp_m", "warp_n", "warp_k", "warps_per_block", "threads_per_block"):
        result[key] = lesson(
            "一块输出交给一个 CTA 后，它内部的 warp 怎样分工？",
            "把输出矩阵看成要完成的一大片方格纸。CTA拿一大块，warp各拿其中一小块；沿K则要不断取下一批输入，累加到手上的输出。",
            ["gemm", "execution", "tile", "register"],
            [("M/N负责哪一片输出", "CTA_M/CTA_N决定一个block的输出区域；warp_M/warp_N决定一个warp的协作区域。线程再依照指令布局持有其中的片段，不是一线程简单负责一整行。"),
             ("K不是第三个输出方向", "K是每个输出需要累加的长度。CTA_K=32表示主循环按32个K元素推进，不表示输入K只能等于32，也不等于把K拆给多个warp独立求和。"),
             ("当前实现如何数warp和线程", "这个实验没有warp级K分区，warp_K=CTA_K，因此warp数=(CTA_M/warp_M)×(CTA_N/warp_N)，线程数=warp数×32。这是当前映射约定，不能套到任意kernel。"),
             ("为什么小warp tile也可能更慢", "保持CTA不变而缩小warp tile，会增加warp/线程数，改变协作开销与资源分配。保持warp不变而加大CTA，又可能增加每CTA的工作与shared压力；都要实测。")],
            ("当前GEMM布局的精确整除示例；K方向不分成独立warp。", ["CTA = 128×128×32", "warp = 32×32×32", "M方向：128/32=4；N方向：128/32=4", "warps/CTA = 4×4=16", "threads/CTA = 16×32=512"],
             "warp是32个lane协作处理32×32输出，不是每个线程各自保存整个32×32矩阵。"),
            [("加大CTA", "每CTA可能复用更多数据，但block数和资源驻留也会变。", "一起看grid、shared、regs、spill及耗时。"),
             ("缩小warp tile", "当前映射下warp数可能增加，线程更多不保证更快。", "先算线程数和限制，再通过编译与正确性测试验证，最后测时间。")],
            "先说明是CTA、warp还是指令层级，再谈M/N/K；别把tile大小当每线程工作量。",
            ("CTA128×128、warp64×64，在这个不拆K的映射里是多少线程？", "(128/64)×(128/64)=4个warp，共4×32=128线程。"))

    for key in ("stages", "shared_bytes_per_block"):
        result[key] = lesson(
            "为了让计算与搬运重叠，需要预留多少份 A/B 缓冲？",
            "像流水厨房：正在炒一份，旁边提前切好下一份食材。提前准备可能省等待，但台面有限；把台面全占满可能就容不下另一组人。",
            ["gemm", "pipeline", "tile", "memory", "units"],
            [("为什么需要多个stage", "GEMM沿K循环。加载数据需要时间，如果等加载完才计算、算完才加载，容易出现空档。多级缓冲用于让不同批次的加载和计算重叠。"),
             ("stage数不是循环次数", "输入K=4096、CTA_K=32有128个K步骤。stages=3描述缓冲组织，不是只算3步，也不是把K循环次数变为3。缓冲可以轮流复用。"),
             ("怎样估算A/B占用", "一个stage保存CTA_M×CTA_K个A元素和CTA_K×CTA_N个B元素。FP16每元素2 B。乘stages得到流水载荷估算；实际还可能有padding、布局或epilogue等约束。"),
             ("更多stage为何不一定好", "如果2或3层已足够隐藏等待，再增加可能只增加shared和寄存器压力，减少可驻留CTA。资源占用与等待改善需要一起衡量。")],
            ("FP16、CTA128×256×32、stages=3，仅估算A/B流水载荷。", ["A每stage：128×32×2 B = 8192 B", "B每stage：32×256×2 B = 16384 B", "合计：(8192+16384)×3 = 73728 B", "73728/1024 = 72 KiB"],
             "实际shared/CTA以编译后资源查询为准；stage降到2时载荷估算48 KiB，但不保证实际储存和性能按比例变化。"),
            [("shared较大、驻留上限低", "较多缓冲可能限制CTA驻留。", "固定tile只改stages，检查真实shared、spill和原生耗时。"),
             ("减少stage后更慢", "减少缓冲可能使加载与计算重叠不足。", "看等待是否增加；不要只为了更低shared或更高occupancy选配置。")],
            "Stages控制流水缓冲组织，不是K循环总次数；空间换重叠是否划算由实测决定。",
            ("K=4096、CTA_K=32、stages=3，一共只算3个K块吗？", "不是。这里仍有4096/32=128个K步骤，3描述循环中复用的流水缓冲层数。"))

    for key, resource in (("register_block_limit", "寄存器"), ("shared_block_limit", "shared memory"), ("warp_block_limit", "warp/线程")):
        result[key] = lesson(
            f"如果只考虑{resource}，一个 SM 最多能同时留下几个 CTA？",
            "一个活动既受座位数限制，也受桌子数和材料份数限制。某项资源能容纳4组，不代表所有资源合起来也能容纳4组；最紧的一项会卡住。",
            ["execution", "register", "memory"],
            [("这是某一项资源的上限", f"{resource} block limit是“单独考虑这一约束时”的CTA/SM上限。它不是实测同时运行的CTA数，也不是使用百分比。"),
             ("要与其他限制一起取约束", "寄存器、shared、warp/线程以及架构每SM block上限都需要满足。实际驻留能力还涉及分配粒度，应以CUDA API/NCU结果验证。"),
             ("解除一个限制可能还不够", "寄存器和shared可能同时卡在1个CTA。即使把寄存器压力降低，shared仍可能不允许第2个CTA进来。")],
            ("以下是独立资源上限的教学数字。", ["寄存器最多1 CTA/SM", "Shared最多2 CTA/SM", "Warp/线程最多6 CTA/SM", "其他约束假设不更紧 → 综合最多 min(1,2,6)=1 CTA/SM"],
             "不能把1+2+6相加，也不能只选最大的6。"),
            [("某一项=1", "这一项单独看只能允许一个CTA。", "检查其他项是否同样紧，再设计减少资源需求的实验。"),
             ("上限提高但实际occupancy没提高", "grid或其他约束可能还在限制。", "检查综合驻留上限和grid，不把单字段改善当最终收益。")],
            "这是单项资源允许的CTA数；综合能力受最紧的约束限制。",
            ("寄存器limit=1、shared limit=1，只改寄存器一定能驻留2个CTA吗？", "不能。Shared仍然可能把驻留CTA上限卡在1。"))

    for key in ("grid_blocks", "waves_per_sm"):
        result[key] = lesson(
            "总共有多少个 CTA 任务？设备一轮能容纳多少？",
            "像把一大批任务分给多个车间：任务总数决定能发给多少车间，同时每个车间能接几份任务也有限。Wave是这批任务相对同时容纳能力的规模，不是强制齐步走的一轮。",
            ["execution", "gemm", "tile", "schedule"],
            [("Grid blocks是什么", "当前GEMM把M/N输出平面切块。一个CTA负责一个块；边缘即使不满也需要CTA，所以用向上取整。K通常在CTA内部循环，不乘进grid。"),
             ("Waves怎样帮助看并行度", "用grid CTA总数与全设备可同时驻留CTA数比较，能粗略看有多少轮工作。SM完成一个CTA后可接新任务，不需要等待全卡完成整轮。"),
             ("更多block不是免费收益", "缩小CTA可能产生更多block，让更多SM有事做；也可能减少复用、增加加载和调度开销。任务量够大后不一定还需要继续加block。")],
            ("4070 SUPER有56个SM；假设该配置每SM最多驻留1个CTA，M=N=4096、CTA256×128。", ["M方向：ceil(4096/256)=16", "N方向：ceil(4096/128)=32", "Grid = 16×32=512 CTA", "同时驻留容量 = 56×1=56 CTA", "理论waves ≈ 512/56 ≈ 9.14"],
             "9.14不是9.14个warp，也不是实际occupancy。它不能保证每个周期每个SM都忙。"),
            [("grid很小", "可能有些SM没有分到任务。", "在保持语义输入的前提下尝试更细的任务划分，比较开销与收益。"),
             ("waves已很多但仍慢", "可能不是总任务数不足。", "看每个SM内的eligible、计算/访存吞吐和spill。")],
            "Grid是总任务数，驻留上限是同时容纳量，waves是两者的规模比较。",
            ("512个CTA表示每个SM同时运行512个CTA吗？", "不是。512是全次launch的总数；每SM同时能留住多少还受资源约束。"))

    for key in ("tensor_pct", "tflops"):
        result[key] = lesson(
            "数学工作完成得有多快？Tensor 流水线在观察期间有多活跃？",
            "一台机器可以忙着加工，也可以最终产出很多合格零件。忙碌程度与有效产量相关，但不是同一个读数；指令的工作大小也不相同。",
            ["gemm", "execution", "peak", "units", "measurement"],
            [("Tensor Core在这里做什么", "当前FP16 GEMM使用Tensor运算指令协作计算小矩阵片段，FP32累加结果。它不是把每个输出都用普通FP32标量乘加逐个执行，因此普通FP32峰值提示不一定适合衡量这个kernel。"),
             ("Tensor百分比的分母", "本页Tensor指标按SM active cycles口径报告活跃程度；Compute吞吐使用elapsed口径且是聚合指标。两者数值不同不说明采集错误，不能直接相减算浪费。"),
             ("TFLOP/s怎样从问题与时间得到", "这个GEMM采用2MNK近似运算量：一次乘加按2次浮点操作计。除以未受NCU replay干扰的原生秒数，再除10¹²。它是这一约定下的有效工作速率。")],
            ("假设M=N=K=4096，原生平均时间为2 ms；仅演示公式，非当前实验测量。", ["工作量≈2×4096³=137438953472 FLOP", "2 ms=0.002 s", "TFLOP/s≈137438953472/0.002/10¹²", "≈68.72 TFLOP/s"],
             "必须说明数据类型、累加方式和FLOP约定。新算子不能照抄2MNK；也不能把NCU整段进程耗时放进分母。"),
            [("Tensor较低", "可能没充分供应计算，也可能工作短/其他步骤占比高。", "看eligible、数据搬运和原生耗时，再判断是否该调tile。"),
             ("TFLOP/s提升", "在相同数学输入下表示测得的完成速度更快。", "确认正确性、数据类型、输入及计时条件相同，并重复测量波动。")],
            "Tensor活跃、Compute吞吐和有效TFLOP/s是三个有关系但不能互换的指标。",
            ("用NCU耗时几秒来算普通GEMM TFLOP/s，结果很低能代表真实性能吗？", "不能。NCU整个过程包含多次重放等开销；应使用独立原生benchmark时间。"))

    for key in ("native_time_ms", "duration_seconds", "warmup", "iterations", "preset", "cache"):
        questions = {"native_time_ms": "正常运行这个算子平均要多久？", "duration_seconds": "NCU报告里的某一个kernel用了多久？",
                     "warmup": "正式计时前为什么先跑几次？", "iterations": "为什么要多跑几次再求平均？", "preset": "为什么full分析比detailed花更久？", "cache": "NCU每次重放前是否主动清缓存？"}
        specifics = {
            "native_time_ms": ("本页的原生耗时", "当前GEMM使用CUDA events围住多次执行，给出平均时间。它不混入后面的NCU采集总时长；仍可能受到系统负载、频率和温度影响。"),
            "duration_seconds": ("本页的NCU单kernel耗时", "这是被选中launch的报告时间。后台按秒归一化，界面可能换成ms。多个kernel的报告时间不能随意加成端到端算子时间，因为还可能存在重叠或中间开销。"),
            "warmup": ("预热有什么作用", "GPU启动、首次调用、缓存和时钟状态会影响早期测量。预热让后面更接近日常重复执行，但不能保证所有状态稳定，更不能覆盖NCU自己的缓存控制。"),
            "iterations": ("平均数能解决什么、不能解决什么", "多次执行可以减少一次计时误差的相对影响。当前输出是迭代平均，不是独立运行样本的中位数或波动区间；要判断小差异还应做多轮独立实验。"),
            "preset": ("选择采集内容，不是调kernel", "Detailed/full决定要哪些指标。硬件不一定能同时采完，需要多轮replay。Full更慢是采集成本，不代表换了一个更慢的GEMM实现。"),
            "cache": ("all与none到底控制什么", "all让NCU在replay前清缓存，none不做该清理。none不是禁用缓存，all也不是“使用所有缓存”。原生warmup不保证NCU重放时仍是热缓存。")}
        result[key] = lesson(
            questions[key], "像给跑步计时：热身、正式跑圈、带测量设备反复录像，是不同阶段。需要先说清自己在看哪一段时间，再比较两个配置。",
            ["measurement", "units", "rate"],
            [specifics[key], ("分清三段时间", "编译和初始化不应混成kernel正常耗时；原生benchmark用于主要性能比较；NCU单launch时间用于理解分析条件；NCU进程总时长包含重放与工具开销。"),
             ("公平对照要保持哪些条件", "至少记录相同的输入、数据类型、正确性、预热/迭代设置、GPU环境和缓存策略。两个数非常接近时先重复测试，不要立即把噪声解释成优化。")],
            ("假设原生计时区内20次执行合计40 ms，不含之前的预热。", ["平均原生时间=40 ms/20=2 ms", "NCU为某个launch采集多组计数器，可能需要多轮重放", "NCU进程总时长可能远大于2 ms", "cache=all时每轮replay前的缓存状态也可能与原生热身后的执行不同"],
             "原生2 ms、NCU单launch时间和NCU进程总时长都可以不同，不能把它们当作同一种测量互相替换。"),
            [("不同配置只差一点点", "单轮平均不足以证明稳定优势。", "重复多轮并保持相同条件，先看差异是否稳定。"),
             ("NCU里程序打印的时间特别大", "可能包含profiling重放影响，不适合作正常性能。", "用独立原生结果比较，再读NCU的单launch和计数器。")],
            "先说明测的是哪一段、用什么条件，再讨论快慢；分析器总时间不是kernel真实性能。",
            ("warmup=10并且NCU cache=all，能保证每次replay都继承预热后的热缓存吗？", "不能。all会在replay前清缓存；要明确NCU采集与原生热运行的条件不同。"))

    for key in ("m", "n", "k"):
        result[key] = lesson(
            {"m": "输出矩阵有多少行？", "n": "输出矩阵有多少列？", "k": "一个输出要把多少对输入相乘再相加？"}[key],
            "不要先背M/N/K。先盯住一个输出：它来自A的一行和B的一列做配对相乘求和；所有行列组合构成输出矩阵。",
            ["gemm", "tile", "units"],
            [("把矩阵形状写完整", "A有M行K列，B有K行N列，得到M行N列的D。两边共同的K被求和掉，所以输出形状没有K这一维。"),
             ("输入维度不是tile维度", "输入K=4096而CTA_K=32表示把4096长的归约切成32一批处理；不表示只计算32，也不是丢掉其他元素。M/N同样会被很多CTA覆盖。"),
             ("为什么换输入后不能只比较时间", "输入变大，计算量和访存、并行度都可能改变。1ms的小问题与2ms的大问题不能直接说明前一种tile好；应固定同一个数学任务做调参。")],
            ("教学矩阵 A[2,4]、B[4,3]，不受当前实验器8倍数对齐限制。", ["D的形状是[2,3]，共6个输出", "D[0,0]=A[0,0]B[0,0]+A[0,1]B[1,0]+A[0,2]B[2,0]+A[0,3]B[3,0]", "每个输出沿K=4求和", "按GEMM惯用2MNK计数，工作量≈2×2×3×4=48 FLOP"],
             "代码实验为保证当前向量对齐要求M/N/K是8的倍数；这个小例子只是为了看清数学含义。"),
            [("把M/N加大后更容易忙起来", "可能是产生了更多输出CTA，并不是tile自动更高效。", "分别记录输入规模和tile，对同输入做A/B。"),
             ("把K加大", "通常增加每个输出的累加工作，当前实现不因此按K增加grid。", "查看主循环步数、复用和时间，别把它与grid并行度混淆。")],
            "M/N决定输出平面，K决定求和长度；输入shape与CTA/warp tile分开看。",
            ("A[8,16]乘B[16,24]，输出是什么形状？", "D[8,24]；中间共同的16是归约长度K，不是输出的第三个维度。"))

    for key in ("seed", "input_range", "alpha", "beta", "check"):
        definitions = {
            "seed": ("怎样确保两次实验用的是同一批随机数？", "种子是伪随机数生成器的起点。同样的生成规则、shape和seed才可复现输入；它不改变tile，也不是数据范围。", ["保持shape和生成规则一致", "seed相同 → 可复现相同输入", "seed不同 → 输入数值可能不同，调参对照要记录"]),
            "input_range": ("输入数值允许多大？它与矩阵大小一样吗？", "这里控制每个元素的取值上下界，当前按0.25步长生成。它不是矩阵维度，不是FP16/FP32的选择；范围变化会影响数值误差和溢出风险。", ["矩阵shape可以仍为512×512", "把范围从1改为2，改的是元素取值而不是行列数", "数值范围更大时，更要验证累加与输出精度"]),
            "alpha": ("矩阵乘积算完后，要乘多大的系数？", "当前语义是D=alpha×(A×B)+beta×C。Alpha缩放矩阵乘积，不改变矩阵形状；它不是学习率之类固定意义的名称。", ["假设某位置的A×B=10，C=4", "alpha=2，beta=0.5", "D=2×10+0.5×4=22"]),
            "beta": ("旧矩阵C要参与输出多少？", "Beta控制C的贡献。Beta=0时数学上不需要C的值，实现可能走不读取C的输出处理路径；与非零beta比较时访存条件可能不同。", ["假设某位置的A×B=10，C=4", "alpha=1，beta=0 → D=10", "alpha=1，beta=0.5 → D=12"]),
            "check": ("跑得快之前，怎么算对了？", "Check决定是否为本次完整输入与reference结果比较。构建阶段另有小矩阵检查，但那个PASS不代表当前所有输入都验证过。Reference是用于对照的实现，不是性能目标。", ["编译小矩阵PASS → 说明该小测试通过", "本次大输入check=false → 本次完整输入未检查", "本次check=true且PASS → 通过本次reference比较"]),
        }
        question, mechanism, lines = definitions[key]
        quizzes = {
            "seed": ("Seed相同，但矩阵shape不同，可以说两次输入完全一样吗？", "不能。Seed只确定伪随机生成起点；复现完整输入还需要相同shape和生成规则，矩阵尺寸不同就已经不是同一个完整输入。"),
            "input_range": ("把输入范围从1改成2，矩阵的行数也会变成两倍吗？", "不会。这个参数改变元素允许的取值范围；行列数仍由M/N/K决定，数据类型也没有因此改变。"),
            "alpha": ("某位置A×B=10、C=4，alpha=2、beta=0.5，D是多少？", "D=2×10+0.5×4=22。Alpha只缩放矩阵乘积这一项，Beta缩放C这一项，不是把整个结果一起乘Alpha。"),
            "beta": ("Beta=0，输出一定全是0吗？", "不是。此时D=alpha×A×B，消失的是beta×C这一项；矩阵乘积仍然保留，还要看Alpha和A/B的值。"),
            "check": ("构建时小矩阵PASS，本次check=false，可以写“当前大输入已验证正确”吗？", "不能。构建检查与本次完整输入检查是不同证据，页面应明确区分。"),
        }
        result[key] = lesson(
            question, "一场公平的调参实验，既要固定任务大小，也要说明给了什么数、要求算什么结果。否则两次更快的原因可能不是tile。",
            ["gemm", "measurement", "units"],
            [("这个参数改变什么", mechanism), ("和硬件调参分开记录", "CTA、warp、stages改变工作组织；seed、范围、alpha/beta和check描述输入/语义/验证条件。它们都影响实验解释，但不应该混称为tile。"),
             ("为什么先验证，再比较速度", "不同输入可能暴露边界或精度问题。若输出不正确，耗时再短也不能作为正确实现的性能胜利。对照页会提示未完成正确性检查。")],
            ("以下是说明参数语义的小例子，不是当前实验数值。", lines, "把输入条件和检查状态一同保存，才知道两次实验究竟比较了什么。"),
            [("为了公平调tile", "不应同时随意改动输入和输出语义。", "先复制一条实验，只改一个tile参数，保留这些输入设置。"),
             ("出现误差或失败", "可能与数据、边界或实现有关，不能忽略。", "先固定seed复现，检查reference和数值范围，再继续谈性能。")],
            "先确定做的是同一件正确的数学工作，再比较如何分工更快。",
            quizzes[key])
    return result


LESSONS = lessons()
