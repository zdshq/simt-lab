import csv
import io
import json
import math
import re
import hashlib


METRICS = {
    "compute_pct": ("Compute / SM 吞吐", "%", ["sm__throughput.avg.pct_of_peak_sustained_elapsed"]),
    "memory_pct": ("Memory 总吞吐", "%", ["gpu__compute_memory_throughput.avg.pct_of_peak_sustained_elapsed"]),
    "l1_pct": ("L1 / TEX 吞吐", "%", ["l1tex__throughput.avg.pct_of_peak_sustained_elapsed"]),
    "l2_pct": ("L2 吞吐", "%", ["lts__throughput.avg.pct_of_peak_sustained_elapsed"]),
    "dram_pct": ("DRAM 吞吐", "%", ["gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed", "dram__throughput.avg.pct_of_peak_sustained_elapsed"]),
    "l1_hit_pct": ("L1 / TEX lookup 命中率（LDGSTS 不适用）", "%", ["l1tex__t_sector_hit_rate.pct"]),
    "l2_hit_pct": ("L2 命中率", "%", ["lts__t_sector_hit_rate.pct"]),
    "shared_pct": ("Shared Memory 吞吐", "%", ["l1tex__data_pipe_lsu_wavefronts_mem_shared.sum.pct_of_peak_sustained_elapsed"]),
    "shared_wavefronts_load_pct": ("Shared load wavefronts", "%", ["l1tex__data_pipe_lsu_wavefronts_mem_shared_op_ld.sum.pct_of_peak_sustained_elapsed"]),
    "shared_wavefronts_store_pct": ("Shared store wavefronts", "%", ["l1tex__data_pipe_lsu_wavefronts_mem_shared_op_st.sum.pct_of_peak_sustained_elapsed"]),
    "shared_bank_conflicts": ("LDGSTS 路径 bank-conflict 计数（非普通 Shared load/store）", "count", ["smsp__sass_l1tex_data_bank_conflicts_pipe_lsu_mem_shared_op_ldgsts.sum"]),
    "ldgsts_cache_access": ("LDGSTS cache access", "count", ["sm__sass_l1tex_t_requests_pipe_lsu_mem_global_op_ldgsts_cache_access.sum"]),
    "ldgsts_cache_bypass": ("LDGSTS cache bypass", "count", ["sm__sass_l1tex_t_requests_pipe_lsu_mem_global_op_ldgsts_cache_bypass.sum"]),
    "tensor_pct": ("Tensor pipe（active cycles）", "%", ["sm__pipe_tensor_cycles_active.avg.pct_of_peak_sustained_active", "sm__pipe_tensor_cycles_active_v2.avg.pct_of_peak_sustained_active"]),
    "occupancy_pct": ("实际 Occupancy", "%", ["sm__warps_active.avg.pct_of_peak_sustained_active"]),
    "theoretical_occupancy_pct": ("理论 Occupancy", "%", ["sm__maximum_warps_per_active_cycle_pct"]),
    "registers_per_thread": ("寄存器 / thread", "reg", ["launch__registers_per_thread"]),
    "register_block_limit": ("寄存器限制的 CTA / SM", "block", ["launch__occupancy_limit_registers"]),
    "shared_block_limit": ("Shared 限制的 CTA / SM", "block", ["launch__occupancy_limit_shared_mem"]),
    "warp_block_limit": ("Warp 限制的 CTA / SM", "block", ["launch__occupancy_limit_warps"]),
    "eligible_warps": ("Eligible warps / scheduler", "warp", ["smsp__warps_eligible.avg.per_cycle_active"]),
    "active_warps": ("Active warps / scheduler", "warp", ["smsp__warps_active.avg.per_cycle_active"]),
    "issue_pct": ("Issue active", "%", ["smsp__issue_active.avg.pct_of_peak_sustained_active"]),
    "long_scoreboard_pct": ("Long scoreboard stall", "%", ["smsp__warp_issue_stalled_long_scoreboard_per_warp_active.pct"]),
    "math_throttle_pct": ("Math pipe throttle stall", "%", ["smsp__warp_issue_stalled_math_pipe_throttle_per_warp_active.pct"]),
    "short_scoreboard_pct": ("Short scoreboard stall", "%", ["smsp__warp_issue_stalled_short_scoreboard_per_warp_active.pct"]),
    "barrier_pct": ("Barrier stall", "%", ["smsp__warp_issue_stalled_barrier_per_warp_active.pct"]),
    "long_scoreboard_ratio": ("Long scoreboard / issue", "ratio", ["smsp__average_warps_issue_stalled_long_scoreboard_per_issue_active.ratio"]),
    "math_throttle_ratio": ("Math throttle / issue", "ratio", ["smsp__average_warps_issue_stalled_math_pipe_throttle_per_issue_active.ratio"]),
    "short_scoreboard_ratio": ("Short scoreboard / issue", "ratio", ["smsp__average_warps_issue_stalled_short_scoreboard_per_issue_active.ratio"]),
    "barrier_ratio": ("Barrier / issue", "ratio", ["smsp__average_warps_issue_stalled_barrier_per_issue_active.ratio"]),
    "warp_latency_cycles": ("Warp latency / issued instruction", "cycle", ["smsp__average_warp_latency_per_inst_issued.ratio"]),
    "local_load_sectors": ("Local load sectors", "sector", ["l1tex__t_sectors_pipe_lsu_mem_local_op_ld.sum"]),
    "local_store_sectors": ("Local store sectors", "sector", ["l1tex__t_sectors_pipe_lsu_mem_local_op_st.sum"]),
    "shared_bank_load": ("Shared load bank conflicts", "count", ["l1tex__data_bank_conflicts_pipe_lsu_mem_shared_op_ld.sum"]),
    "shared_bank_store": ("Shared store bank conflicts", "count", ["l1tex__data_bank_conflicts_pipe_lsu_mem_shared_op_st.sum"]),
    "waves_per_sm": ("Waves / SM", "wave", ["launch__waves_per_multiprocessor"]),
    "duration_seconds": ("NCU 单次 kernel 时长", "s", ["gpu__time_duration.sum"]),
    "dram_bytes_per_second": ("DRAM 带宽", "B/s", ["dram__bytes.sum.per_second"]),
    "l2_bytes_per_second": ("L2 扇区带宽（换算）", "B/s", ["lts__t_sectors.sum.per_second"]),
}


def number(value):
    try:
        result = float(str(value).replace(",", "").replace("%", "").strip())
        return result if math.isfinite(result) else None
    except (ValueError, TypeError):
        return None


def parse_ncu_launches(text):
    lines = text.splitlines()
    start = next((index for index, line in enumerate(lines)
                  if ('"Metric Name"' in line and '"Metric Value"' in line)
                  or (line.startswith('"ID","Process ID"') and '"Kernel Name"' in line)), None)
    if start is None:
        raise ValueError("NCU CSV 没有指标表头，可能没有采集到目标 kernel")
    rows = []
    reader = csv.DictReader(io.StringIO("\n".join(lines[start:])))
    if "Metric Name" in reader.fieldnames:
        for row in reader:
            name = row.get("Metric Name")
            if name:
                rows.append({"name": name, "unit": row.get("Metric Unit", ""),
                             "value": number(row.get("Metric Value")), "raw": row.get("Metric Value", ""),
                             "kernel": row.get("Kernel Name", ""), "kernel_id": row.get("ID", ""),
                             "section": row.get("Section Name", ""),
                             "context": {key: row.get(key, "") for key in ("Process ID", "Device", "Context", "Stream")}})
    else:
        units = next(reader, {})
        for row in reader:
            if not row.get("ID"):
                continue
            for name in reader.fieldnames:
                if "__" in name:
                    rows.append({"name": name, "unit": units.get(name, ""),
                                 "value": number(row.get(name)), "raw": row.get(name, ""),
                                 "kernel": row.get("Kernel Name", ""), "kernel_id": row["ID"], "section": "raw",
                                 "context": {key: row.get(key, "") for key in ("Process ID", "Device", "Context", "Stream")}})
    if not rows:
        raise ValueError("NCU report 不包含指标")
    groups = {}
    for row in rows:
        identity = json.dumps([row["context"], row["kernel_id"]], sort_keys=True)
        groups.setdefault(identity, []).append(row)
    result = []
    for identity, entries in groups.items():
        if len({entry["kernel"] for entry in entries}) != 1:
            raise ValueError("同一个 launch ID 对应多个 kernel 名称，拒绝混合指标")
        unique = {}
        for row in entries:
            previous = unique.get(row["name"])
            if previous and (previous["value"], previous["unit"]) != (row["value"], row["unit"]):
                raise ValueError("同一 launch 的重复 metric 值或单位不一致")
            unique[row["name"]] = row
        entries = list(unique.values())
        result.append({"id": hashlib.sha256(identity.encode()).hexdigest()[:20], "kernel": entries[0]["kernel"],
                       "context": {**entries[0]["context"], "ID": entries[0]["kernel_id"]},
                       "metrics": entries, "summary": summarize_ncu(entries)})
    return result


def parse_ncu(text):
    launches = parse_ncu_launches(text)
    if len(launches) != 1:
        raise ValueError("预期一个 kernel，报告包含多个 kernel；拒绝混合指标")
    return launches[0]["metrics"], launches[0]["summary"]


TIME_UNITS = {"nsecond": 1e-9, "usecond": 1e-6, "msecond": 1e-3, "second": 1.0,
              "ns": 1e-9, "us": 1e-6, "µs": 1e-6, "μs": 1e-6, "ms": 1e-3, "s": 1.0}
PREFIXES = {"": 1, "k": 1e3, "K": 1e3, "M": 1e6, "G": 1e9, "T": 1e12, "P": 1e15,
            "Ki": 1024, "Mi": 1024**2, "Gi": 1024**3, "Ti": 1024**4}


def unit_scale(unit, bases, rate=False):
    """Normalize NCU scaled units such as Gbyte/s and sector/ns."""
    numerator, separator, denominator = unit.strip().partition("/")
    if rate:
        if not separator or denominator not in TIME_UNITS:
            return None
        divisor = TIME_UNITS[denominator]
    else:
        if separator:
            return None
        divisor = 1
    for prefix, scale in PREFIXES.items():
        if any(numerator == prefix + base for base in bases):
            return scale / divisor
    return None


def summarize_ncu(rows):
    indexed = {row["name"]: row for row in rows}
    summary = {}
    for key, (label, unit, candidates) in METRICS.items():
        found = next((indexed[name] for name in candidates
                      if name in indexed and indexed[name]["value"] is not None), None)
        value = found["value"] if found else None
        if found and key == "duration_seconds":
            scale = TIME_UNITS.get(found["unit"])
            value = value * scale if scale is not None else None
        if found and key == "l2_bytes_per_second":
            scale = unit_scale(found["unit"], ("sector",), rate=True)
            value = value * scale * 32 if scale is not None else None
        if found and key == "dram_bytes_per_second":
            scale = unit_scale(found["unit"], ("byte", "B"), rate=True)
            value = value * scale if scale is not None else None
        if found and unit in ("sector", "count"):
            scale = unit_scale(found["unit"], ("sector", "count", "conflict", ""))
            value = value * scale if scale is not None else None
        summary[key] = {"label": label, "unit": unit, "value": value,
                        "metric": found["name"] if found else candidates[0],
                        "source_unit": found["unit"] if found else None}
    return summary
