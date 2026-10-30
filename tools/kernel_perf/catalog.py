"""The UI consumes this catalog for labels, units, help, search and deep links."""

from metrics import METRICS
from learning import CONCEPTS, LEARNING_PATH, LESSONS

PROFILING = "https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html"
CUDA = "https://docs.nvidia.com/cuda/archive/12.9.1/cuda-c-programming-guide/index.html"
NVCC = "https://docs.nvidia.com/cuda/archive/12.9.1/cuda-compiler-driver-nvcc/index.html"
CLI = "https://docs.nvidia.com/nsight-compute/NsightComputeCli/index.html"
GROUPS = {"memory": "存储与缓存", "resources": "资源与驻留", "scheduler": "调度与等待", "compute": "计算与耗时", "inputs": "输入与实验参数", "tiles": "Tile 与流水线"}


def entry(identifier, group, short, scope, explanation, example, caution, related=(), experiment="固定输入，一次只改一个参数，对比原生耗时。", source=PROFILING):
    label, unit, aliases = METRICS.get(identifier, (identifier, "", []))
    return {"id": identifier, "name": label, "unit": unit, "aliases": aliases, "group": group,
            "short": short, "scope": scope, "explanation": explanation, "example": example,
            "caution": caution, "related": list(related), "experiment": experiment,
            "source": source, "refs": [f"summary.{identifier}.value"], "value_source": "NCU"}


def build_catalog():
    items = {}
    def add(identifier, *args, **kwargs):
        items[identifier] = entry(identifier, *args, **kwargs)
        return items[identifier]

    for key, name in (("l1_pct", "L1/TEX"), ("l2_pct", "L2"), ("dram_pct", "DRAM"),
                      ("compute_pct", "SM 计算"), ("memory_pct", "存储系统")):
        add(key, "compute" if key == "compute_pct" else "memory", f"{name} 的相对峰值吞吐压力。",
            "本次被分析 launch；基于 elapsed cycles 的相对峰值指标。",
            "吞吐聚合指标取其组成计数器的最大百分比，不是各指标之和，也不是空间占用率。需展开子指标定位哪条路径忙。",
            f"{name} 吞吐 90%，表示某条相关吞吐路径接近其峰值，不表示容量用了 90%。",
            "高吞吐提示排查方向，不保证它是唯一瓶颈；低平均吞吐也不排除访存/依赖延迟。",
            ["l1_hit_pct", "l2_hit_pct", "eligible_warps", "native_time_ms"])
    for key, name in (("l1_hit_pct", "L1/TEX lookup"), ("l2_hit_pct", "L2")):
        add(key, "memory", f"在 {name} 中命中的 sector 比例。", "选中 launch 的缓存请求；不是整个进程/所有实验的平均。",
            "命中率说明请求是否在该级找到数据。数据复用、缓存策略、工作集和访问模式都会影响它。对于 cp.async/LDGSTS，L1 lookup 命中率不等于 Shared Memory 复用。",
            "命中率 99% 与吞吐 91% 可以同时出现：命中的访问仍消耗缓存资源。",
            "低命中率不能直接证明容量不足；流式读取、缓存绕过或 global→shared 的 LDGSTS 路径都可能没有普通 L1 lookup hit。", ["l1_pct", "l2_pct", "dram_pct"])
    for key, operation in (("local_load_sectors", "读"), ("local_store_sectors", "写")):
        add(key, "memory", f"线程私有 local memory 的{operation}访问 sector 计数。",
            "本次 launch；L1/TEX 统计位置，跨相关硬件单元求和。不是每个 thread 的值。",
            "Local 表示线程私有，不等于 shared，也不保证存储在片上。一个 sector 是 32 B；重复访问同一地址会再次计数。",
            "1,000,000 sectors × 32 B = 32 MB 的 sector 访问量折算。不是占了 32 MB 空间，也不代表 DRAM 流量正好是 32 MB。",
            "不是指令数，不是唯一地址数，也不直接等于 spill。局部数组/栈对象也会访问 local。NCU replay 的多个 pass 不应在这里相加。",
            ["spill_store_bytes", "spill_load_bytes", "l2_pct", "dram_pct"],
            "结合目标 kernel 的 ptxas spill 证据，再比较 local sectors、L2 吞吐和原生耗时。")
    for key in ("shared_bank_load", "shared_bank_store"):
        add(key, "memory", "Shared memory 访问的 bank 冲突计数。", "选中 launch 的 shared 访问统计；动态计数。",
            "多个线程访问同一 bank 中不同位置时，访问可能需要拆开处理，增加服务代价。",
            "相同输入下，调整 shared layout 后冲突计数和耗时一起下降，才是有价值的改善证据。",
            "不是缓存 miss，也不等于 L1 容量不足。不同工作量的原始冲突总数不能直接排名。",
            ["short_scoreboard_ratio", "l1_pct", "native_time_ms"])
    add("shared_bank_conflicts", "memory", "LDGSTS global→shared 路径的专用 bank-conflict 计数。",
        "选中 launch 的 LDGSTS 统计；不等同普通 Shared load/store conflicts。",
        "该计数器只描述 global→shared 异步拷贝路径；普通 Shared load/store 冲突应分别看 Shared load conflicts 和 Shared store conflicts。",
        "2fbc58 中该计数器非零，但普通 Shared load/store conflicts 为 0，不能据此判定 Tensor Core 的 shared layout 有问题。",
        "不要把它与 l1tex__data_bank_conflicts_pipe_lsu_mem_shared.sum 或普通 Shared load/store 计数直接混加。",
        ["shared_bank_load", "shared_bank_store", "ldgsts_cache_access", "ldgsts_cache_bypass"])
    for key, name in (("dram_bytes_per_second", "DRAM"), ("l2_bytes_per_second", "L2")):
        add(key, "memory", f"{name} 每秒访问字节量。", "选中 launch；B/s。页面可用十进制 GB/s 显示。",
            "L2 值由 sector/s × 32 B 换算；DRAM 值来自 DRAM bytes/s 计数器。两者位于不同统计位置。",
            "1,000,000,000 B/s = 1 GB/s，不是 1 GiB/s。",
            "不能把 L2 带宽与显存带宽当成同一个数；缓存命中会改变下游流量。", ["l2_pct", "dram_pct", "l2_hit_pct"])
    add("registers_per_thread", "resources", "编译后每个线程实际分配的 32-bit 寄存器数。", "静态分配；单位 reg/thread。",
        "CUDA numRegs 与 NCU launch register 指标描述物理分配量，不是变量在每个周期的活跃数。",
        "64 regs/thread × 1024 threads = 65536 个名义寄存器，仍可能有变量被 spill。",
        "少用寄存器不自动更快；强行限制寄存器可能引入 spill。", ["spill_store_bytes", "spill_load_bytes", "register_capacity_pct_nominal"], source=NVCC)
    items["registers_per_thread"]["refs"].append("native.registers_per_thread")
    add("occupancy_pct", "resources", "实际活跃/驻留 warp 数相对硬件最大 warp 数的比例。", "本次 launch；SM active cycles 下的平均驻留比例。",
        "衡量有多少 warp 驻留，不表示所有 warp 都在执行指令。", "驻留 32 个 warp、上限 48 个时，占比约 66.7%。",
        "Occupancy 越高不一定越快。还要看 eligible、issue 与计算/访存效率。", ["theoretical_occupancy_pct", "eligible_warps", "issue_pct"])
    add("theoretical_occupancy_pct", "resources", "该配置在资源限制下能达到的理论驻留上限。", "理论值，不是实测 GPU 忙碌率。",
        "线程数、寄存器、shared 等会限制每 SM 可驻留 CTA 数。", "每 SM 最多 1 个 256-thread CTA 时，驻留是 8 个 warp。",
        "理论上限高不意味着 grid 足够，也不意味着调度器有可发射指令。", ["occupancy_pct", "register_block_limit", "shared_block_limit"])
    items["theoretical_occupancy_pct"]["refs"].append("native.theoretical_occupancy_pct")
    for key, resource in (("register_block_limit", "寄存器"), ("shared_block_limit", "shared memory"), ("warp_block_limit", "warp/线程")):
        add(key, "resources", f"单独考虑 {resource} 约束时，每 SM 最多能驻留多少 CTA。", "理论资源约束；block/SM。",
            "实际可驻留 CTA 数需要同时满足所有限制。", "寄存器限制是 1、shared 限制是 2 时，不能据此驻留 2 个 CTA。",
            "不是已经实际运行了多少 CTA；grid 和其他限制也有影响。", ["active_blocks_per_sm", "occupancy_pct"])
    add("active_warps", "scheduler", "每个 scheduler 平均驻留、尚未完成的 warp 数。", "warp/scheduler；按 active cycles 归一化。",
        "包括正在等待依赖、数据或同步的 warp。", "Active=8、Eligible=0.2：人很多，但大多还没准备好执行。",
        "不能把 active 当成当前可发射。", ["eligible_warps", "issue_pct"])
    add("eligible_warps", "scheduler", "每个 scheduler 平均准备好发射下一条指令的 warp 数。", "warp/scheduler；按 active cycles 归一化。",
        "数据和执行条件满足后，warp 才有资格被调度器选中。", "Eligible=0.1 表示平均很少有 warp 可被选中，不是只有 0.1 个物理 warp。",
        "不是百分比；低值要结合 stall 原因分析。", ["active_warps", "issue_pct", "long_scoreboard_ratio"])
    add("issue_pct", "scheduler", "调度器发射指令的活跃程度。", "相对峰值 issue 活跃指标；active cycles 口径。",
        "表示是否有指令被发射，而不是多少 Tensor Core FLOP 已完成。", "发射率低时，先看 eligible warps 和等待原因。",
        "不同指令每条完成的工作量不同，不能单凭 issue 判断计算效率。", ["eligible_warps", "tensor_pct"])
    waits = {"long_scoreboard": "等待与 L1/TEX 操作相关的数据依赖，不一定是在等 DRAM。",
             "short_scoreboard": "等待 MIO 操作的数据依赖，常见于 shared，但并不限于 shared。",
             "math_throttle": "所需数学流水线暂时不能接收更多工作。",
             "barrier": "等待同一 CTA 的其他线程/warp 到达同步点。"}
    for name, description in waits.items():
        for suffix in ("ratio", "pct"):
            key = f"{name}_{suffix}"
            ratio = suffix == "ratio"
            add(key, "scheduler", description, "每次 issue 归一化的比值。" if ratio else "按活跃 warp 归一化的 stall 百分比。",
                "必须结合 eligible、issue 和 Source/SASS 中的具体位置判断影响。",
                "页面值 24（ratio）不能读成耗时 24%。" if ratio else "某种 stall 占比高，不等于消除它后能等比例提速。",
                "不同分母的 ratio、pct 和 cycles 不能混加；关注无法发射时的关键等待。", ["eligible_warps", "issue_pct", "native_time_ms"])
    add("warp_latency_cycles", "scheduler", "warp 两次指令发射之间的平均周期数。", "cycles/issued instruction；不是 kernel 总周期数。",
        "等待与发射间隔共同影响这个平均值。", "间隔较长时，需要看哪些依赖/资源限制了发射。", "不能乘一个随意的指令数就估算整个 GPU 耗时。", ["eligible_warps", "long_scoreboard_ratio"])
    add("waves_per_sm", "resources", "Grid 相对于设备可同时驻留 CTA 容量的波次数。", "理论 grid 分解；不是 occupancy。",
        "反映需要多少轮驻留容量才能覆盖 grid。", "Waves 很少时，可能只有少量 SM 有工作；很多 waves 也不保证计算流水线繁忙。",
        "不要与 shared wavefront 或 warp 混淆。", ["grid_blocks", "active_blocks_per_sm", "occupancy_pct"])
    add("tensor_pct", "compute", "Tensor pipeline 在 SM active cycles 中的活跃程度。", "active cycles 分母；与 elapsed 口径的 Compute 不同。",
        "专门观察 Tensor 运算相关流水线，而不是 FP32 CUDA core 的峰值利用率。", "Tensor=50% 与 Compute=45% 可以同时成立，因为口径不同。",
        "不是整张卡理论 TFLOP/s 的简单百分比。", ["compute_pct", "tflops", "native_time_ms"])
    add("duration_seconds", "compute", "NCU 报告中的单次被分析 kernel 时长。", "单位 s；页面可能换成 ms。",
        "与 benchmark 原生计时分开保存。", "报告 duration=0.008 s 是 8 ms，不是 profiler 所有 replay 的进程耗时。",
        "不能把 profiler 进程总时间用于 TFLOP/s；多 kernel duration 之和也不自动等于算子端到端耗时。", ["native_time_ms", "cache"])

    def extra(key, label, unit, group, refs, short, example, caution, related=(), source=CUDA):
        result = add(key, group, short, " / ".join(refs) + "；参见数据来源。", short, example, caution, related, source=source)
        result.update(name=label, unit=unit, refs=refs, value_source="编译 / CUDA / 实验快照")

    extra("native_time_ms", "原生 Kernel / 算子耗时", "ms", "compute", ["native.time_ms"],
          "未在 NCU replay 下测得的 CUDA event 计时；当前 GEMM 为多次迭代均值。", "同条件 A=2 ms、B=1 ms，B 的测时速度是 A 的 2 倍。", "应重复实验并检查波动；不能把一次最快值当稳定性能。", ["iterations", "warmup", "duration_seconds"])
    extra("tflops", "计算性能 TFLOP/s", "TFLOP/s", "compute", ["native.tflops"],
          "当前 GEMM 按 2×M×N×K 除以原生秒数再除以 10¹²。", "FMA 按一次乘法加一次加法计为 2 FLOP。", "不是所有 kernel 都适合这个公式；接入新算子需声明工作量定义。", ["native_time_ms", "tensor_pct"])
    extras = [
        ("shared_bytes_per_block", "Shared memory / CTA", "B", "一个 CTA 分配的 shared memory。", "73728 B = 72 KiB。", "不是 L1 实时占用率；可能由流水或 epilogue 的较大者决定。"),
        ("local_bytes_per_thread", "Local bytes / thread", "B", "CUDA 函数属性报告的线程 local 存储大小。", "120 B 是存储大小，不是一次运行总共读写了 120 B。", "不能仅凭此值证明 spill。"),
        ("threads_per_block", "Threads / CTA", "thread", "一个 block 的线程数。", "32 warps × 32 lanes = 1024 threads。", "线程越多不保证更快。"),
        ("warps_per_block", "Warps / CTA", "warp", "一个 block 的 warp 数。", "CTA256×128、warp32×32 的 M/N 分工是 8×4=32 warps。", "这个 tile 除法依赖当前映射，不是所有 kernel 的通用公式。"),
        ("active_blocks_per_sm", "理论 active blocks / SM", "block", "CUDA occupancy API 计算的每 SM 可驻留 CTA 上限。", "每 SM 上限是 1 个 CTA，不代表实际每个 SM 都有 CTA。", "静态资源上限，不是实际驻留数量的计数器。"),
        ("grid_blocks", "Grid blocks", "block", "本次 launch 的 CTA 总数。", "GEMM grid 为 ceil(M/CTA_M) × ceil(N/CTA_N)。", "不同实现的 grid 定义可能不同。"),
        ("register_capacity_pct_nominal", "寄存器名义容量占比", "%", "寄存器/thread × threads/CTA × active CTA/SM / registers/SM。", "64×1024×1/65536=100%。", "忽略分配粒度，不是寄存器忙碌率，也不能排除 spill。")]
    for key, name, unit, short, example, caution in extras:
        extra(key, name, unit, "resources", [f"native.{key}"], short, example, caution, ["registers_per_thread", "occupancy_pct"])
    for key, label in (("spill_store_bytes", "编译期 spill stores"), ("spill_load_bytes", "编译期 spill loads"), ("stack_frame_bytes", "Stack frame")):
        extra(key, label, "B", "resources", [f"compiler_evidence.resources.{key}"],
              "目标 GEMM 的 ptxas 编译期资源统计。", "Spill loads/stores 非零说明生成了 spill；Stack frame 是另一项栈空间统计。",
              "编译期 bytes 不是动态流量；stack frame 非零单独不能证明 spill。", ["local_load_sectors", "local_store_sectors", "registers_per_thread"], source=NVCC)
    for prefix, label in (("cta", "CTA"), ("warp", "Warp")):
        for axis in ("m", "n", "k"):
            extra(f"{prefix}_{axis}", f"{label} tile {axis.upper()}", "element", "tiles", [f"shape.{prefix}_{axis}"],
                  f"{label} 层级 GEMM tile 的 {axis.upper()} 维度。M/N 是输出平面，K 是归约维度。",
                  "CTA128×256×32 表示输出 tile 为128×256，主循环每步处理 K=32。", "Warp tile 不是一个线程的输出矩阵；不等于 Tensor 指令形状。", ["threads_per_block", "stages"])
    extra("stages", "Pipeline stages", "stage", "tiles", ["shape.stages"],
          "多级流水缓冲数量，用于重叠加载与计算。", "FP16 GEMM 的 A/B shared 载荷约为 (CTA_M+CTA_N)×CTA_K×2×stages。",
          "不是总共执行几次 K 循环；增加 stages 也会增加资源压力。", ["shared_bytes_per_block", "cta_k"])
    input_help = {
        "m": ("输入 M", "输出行数；A 的行数。", "A[M,K] × B[K,N] → D[M,N]。"),
        "n": ("输入 N", "输出列数；B 的列数。", "更改 N 会改变工作量，不能只凭耗时判断 tile 更好。"),
        "k": ("输入 K", "矩阵乘法的归约长度。", "K=4096、CTA_K=32，完整 tile 路径有128步主循环。"),
        "warmup": ("Warmup", "正式计时之前的预热次数。", "Warmup 不会覆盖 NCU cache=all 的 replay 清缓存行为。"),
        "iterations": ("Iterations", "原生计时的迭代次数。", "当前输出是这些迭代的均值，不是独立样本的 p50。"),
        "seed": ("随机 Seed", "确定输入矩阵的伪随机种子。", "同一 seed、形状和生成规则可复现输入。"),
        "input_range": ("输入数值范围", "控制伪随机输入上下界，当前按0.25步长生成。", "范围不是数据类型，也不改变矩阵维度。"),
        "alpha": ("Alpha", "D = alpha×A×B + beta×C 的乘积系数。", "alpha=1 保留矩阵乘积。"),
        "beta": ("Beta", "D = alpha×A×B + beta×C 的 C 系数。", "beta=0 与非零可能使用不同 epilogue 读路径。"),
        "check": ("正确性检查", "是否为本次完整输入比较 reference。", "构建的小矩阵检查通过，不等于当前大输入也检查过。"),
        "preset": ("NCU preset", "选择 detailed 或 full 指标集。", "full 通常需要更多 replay；耗时不用于原生性能排名。"),
        "cache": ("NCU cache control", "all 在 replay 前清缓存；none 不做这个清理。", "cache=none 不等于禁用硬件缓存。")}
    for key, (name, short, example) in input_help.items():
        extra(key, name, "", "inputs", [f"inputs.{key}"], short, example,
              "比较实验时要明确输入、生成规则和测量条件；缺失值不是零。", ["native_time_ms", "duration_seconds"], source=CLI)
    if set(items) != set(LESSONS):
        raise ValueError(f"词条与入门课不匹配：{set(items) ^ set(LESSONS)}")
    for key, item in items.items():
        item["lesson"] = LESSONS[key]
    return {"version": 2, "groups": GROUPS, "entries": list(items.values()),
            "concepts": CONCEPTS, "learning_path": LEARNING_PATH}


CATALOG = build_catalog()
