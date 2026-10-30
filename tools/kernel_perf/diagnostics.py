"""Evidence-based labels, not a hardware 'cache full' detector.

Derived on read from the saved run and its own build. Never interpret missing
counters as zero, local traffic as spill, or throughput as cache occupancy.
"""

from metrics import METRICS, number


RULES = {"version": 1, "high_throughput_pct": 80, "elevated_throughput_pct": 60,
         "low_hit_hint_pct": 50}


def diagnose(summary, native, compiler=None, compiler_source="missing"):
    def value(key):
        return number((summary.get(key) or {}).get("value"))

    def metric(key):
        label, unit, names = METRICS[key]
        saved = summary.get(key) or {}
        return {"label": label, "value": value(key), "unit": unit,
                "metric": saved.get("metric") or names[0], "source": "NCU"}

    def cache_card(key, title):
        throughput, hit = value(f"{key}_pct"), value(f"{key}_hit_pct")
        if throughput is None:
            state, tone, badge = "unknown", "unknown", "吞吐证据缺失"
            description = "尚未拿到吞吐计数器，无法判断压力。请运行 NCU 或检查原始报告。"
        elif throughput >= RULES["high_throughput_pct"]:
            state, tone, badge = "high", "warning", "高吞吐压力"
            description = "该单元吞吐接近峰值，是优先排查方向；尚不能据此确认唯一瓶颈。"
        elif throughput >= RULES["elevated_throughput_pct"]:
            state, tone, badge = "elevated", "attention", "吞吐偏高"
            description = "该单元有一定吞吐压力，需要结合访存量、stall 和耗时判断影响。"
        else:
            state, tone, badge = "not_high", "neutral", "未见高吞吐"
            description = "本次平均吞吐未到提示阈值，不代表缓存一定正常，也不排除访存延迟。"
        notes = ["容量是否不足：未判定。吞吐百分比不是缓存容量占用率。"]
        if hit is None:
            notes.append("命中率缺失；没有把缺失计数器按 0% 处理。")
        elif hit < RULES["low_hit_hint_pct"]:
            alternatives = "流式读取、绕过 L1 等" if key == "l1" else "流式读取、缺少跨 CTA 复用等"
            notes.append(f"命中率低于 50%（经验提示）：检查复用与缓存策略；{alternatives}也会导致低命中率。")
        elif hit >= 90 and state == "high":
            notes.append("命中率虽高，命中的访问仍消耗该级缓存资源；高命中率不等于吞吐压力小。")
        evidence = [metric(f"{key}_pct"), metric(f"{key}_hit_pct")]
        if key == "l1":
            evidence += [metric("shared_pct"), metric("shared_wavefronts_load_pct"),
                         metric("shared_wavefronts_store_pct"), metric("shared_bank_conflicts"),
                         metric("ldgsts_cache_access"), metric("ldgsts_cache_bypass"),
                         metric("shared_bank_load"), metric("shared_bank_store")]
            if value("ldgsts_cache_access") is not None or value("ldgsts_cache_bypass") is not None:
                notes.append("该 launch 包含 LDGSTS 路径计数器；global→shared 路径中；L1 lookup 命中率不代表 Shared Memory 复用。普通 Shared load/store conflicts 与 LDGSTS path counter 已分开显示。")
            else:
                notes.append("L1/TEX 是聚合单元指标；Shared wavefront 与 bank conflicts 才是 Shared 访问的主要证据。")
        else:
            evidence += [metric("dram_pct"), metric("l2_bytes_per_second")]
            dram = value("dram_pct")
            if state == "high" and dram is not None and dram < RULES["elevated_throughput_pct"]:
                notes.append("L2 很忙而 DRAM 未见高吞吐：优先检查片上流量与 local 访问，不能称为显存带宽已满。")
        return {"id": key, "title": title, "state": state, "tone": tone, "badge": badge,
                "description": description, "notes": notes, "evidence": evidence,
                "capacity_state": "undetermined"}

    resources = compiler if isinstance(compiler, dict) else {}

    def compiler_value(key):
        item = resources.get(key)
        return item if type(item) is int and item >= 0 else None

    stores, loads = compiler_value("spill_store_bytes"), compiler_value("spill_load_bytes")
    compiler_regs = compiler_value("registers_per_thread")
    observed_regs = [item for item in (number(native.get("registers_per_thread")),
                                      value("registers_per_thread")) if item is not None]
    mismatch = (compiler_regs is not None and any(item != compiler_regs for item in observed_regs))
    local_loads, local_stores = value("local_load_sectors"), value("local_store_sectors")
    local_observed = any(item is not None and item > 0 for item in (local_loads, local_stores))
    local_zero = local_loads == 0 and local_stores == 0
    notes = []
    if mismatch:
        state, tone, badge = "mismatch", "attention", "编译与运行证据不一致"
        description = "编译日志的寄存器数量与运行记录不符。先核对 ELF / kernel，不据此判定本次 spill。"
    elif stores is not None and loads is not None:
        if stores > 0 or loads > 0:
            state, tone, badge = "confirmed", "warning", "已确认编译期 spill"
            description = "当前目标 kernel 的 ptxas spill 统计非零：编译器生成了寄存器溢出访问。"
        else:
            state, tone, badge = "not_detected", "good", "未检测到编译期 spill"
            description = "当前目标 kernel 的 ptxas spill loads / stores 均为 0；这是编译证据，不是动态寄存器忙碌率。"
    elif local_observed:
        state, tone, badge = "suspected", "attention", "有 local 流量，spill 待确认"
        description = "本次有 local-memory 访问，但缺少有效编译证据；局部数组或栈对象也可能产生这些访问。"
    else:
        state, tone, badge = "unknown", "unknown", "缺少 spill 编译证据"
        description = "不能由寄存器个数、占用率或 local 计数器缺失推断没有 spill。查看编译日志或重新编译。"
    if local_observed:
        notes.append("NCU 已观察到 local 访问；计数包括 spill 和其他 local 对象，不能全部归因于 spill。")
    elif local_zero:
        notes.append("本次 local load/store sectors 均为 0；仅说明本次未观察到这类访问，不否定其他路径的编译期 spill。")
    else:
        notes.append("动态 local 计数器不完整；需要 NCU 才能评估本次运行的 local 流量。")
    notes.append("spill bytes 是编译期静态统计，不是整个 kernel 的动态传输字节数；stack frame 大小单独不能证明 spill。")
    if state == "confirmed":
        notes.append("即使 registers/thread × threads/CTA 刚好等于 65536，也可能 spill：显示的是最终物理分配量。")
    source_labels = {"build_metadata": "构建元数据 · ptxas", "build_log": "历史构建日志 · ptxas",
                     "missing": "未找到 ptxas 证据"}
    source_label = source_labels.get(compiler_source, "ptxas")
    evidence = [{"label": label, "value": compiler_value(key), "unit": unit, "metric": None,
                 "source": source_label} for key, label, unit in (
                     ("spill_store_bytes", "编译期 spill stores", "B"),
                     ("spill_load_bytes", "编译期 spill loads", "B"),
                     ("stack_frame_bytes", "Stack frame", "B"),
                     ("registers_per_thread", "编译分配寄存器 / thread", "reg"))]
    evidence += [metric("local_load_sectors"), metric("local_store_sectors")]
    spill = {"id": "spill", "title": "寄存器 Spill", "state": state, "tone": tone, "badge": badge,
             "description": description, "notes": notes, "evidence": evidence,
             "compiler_source": compiler_source}
    return {"rules": RULES, "cards": [cache_card("l1", "L1/TEX 与 Shared 路径"), cache_card("l2", "L2 Cache"), spill]}
