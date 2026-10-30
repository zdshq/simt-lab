"""Read Nsight Systems SQLite exports without requiring a GPU or NVIDIA tools."""
import sqlite3
from collections import defaultdict
from pathlib import Path


TABLES = (
    ("CUPTI_ACTIVITY_KIND_KERNEL", "kernel"),
    ("CUPTI_ACTIVITY_KIND_MEMCPY", "memcpy"),
    ("CUPTI_ACTIVITY_KIND_MEMSET", "memset"),
    ("CUPTI_ACTIVITY_KIND_RUNTIME", "cuda_api"),
    ("CUPTI_ACTIVITY_KIND_DRIVER", "cuda_api"),
    ("NVTX_EVENTS", "nvtx"),
    ("OSRT_API", "osrt"),
)
GPU_KINDS = {"kernel", "memcpy", "memset"}


def columns(db, table):
    return {row[1] for row in db.execute(f'PRAGMA table_info("{table}")')}


def read_systems_sqlite(path, max_events=12000):
    path = Path(path).resolve()
    if not path.is_file():
        raise ValueError(f"Nsight Systems SQLite 不存在：{path}")
    if type(max_events) is not int or not 1 <= max_events <= 100000:
        raise ValueError("max_events 必须在 1..100000 之间")
    try:
        with sqlite3.connect(path.as_uri() + "?mode=ro", uri=True) as db:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA query_only=ON")
            db.execute("PRAGMA trusted_schema=OFF")
            return analyze(db, max_events)
    except sqlite3.DatabaseError as error:
        raise ValueError(f"无法解析 Nsight Systems SQLite：{error}") from error


def analyze(db, max_events):
    names = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    available = [(table, kind) for table, kind in TABLES if table in names]
    if not available:
        raise ValueError("数据库不包含受支持的 Nsight Systems CUDA/NVTX/OSRT 事件表")
    strings = {row[0]: row[1] for row in db.execute("SELECT id,value FROM StringIds")} if "StringIds" in names else {}
    copy_names = {}
    if "ENUM_CUDA_MEMCPY_OPER" in names:
        enum_columns = columns(db, "ENUM_CUDA_MEMCPY_OPER")
        label = "label" if "label" in enum_columns else "name" if "name" in enum_columns else None
        if label and "id" in enum_columns:
            copy_names = {row[0]: row[1] for row in db.execute(f'SELECT id,"{label}" FROM ENUM_CUDA_MEMCPY_OPER')}
    notes, events, counts, totals, groups = [], [], defaultdict(int), defaultdict(int), {}
    minimum, maximum, invalid = None, None, 0
    budget = max(1, max_events // len(available))
    gpu_queries = []
    for table, kind in available:
        fields = columns(db, table)
        if not {"start", "end"} <= fields:
            notes.append(f"{table} 缺少 start/end 列，未计算这部分事件。")
            continue
        if kind in GPU_KINDS and "deviceId" in fields:
            vm = '"vmId"' if "vmId" in fields else "NULL"
            gpu_queries.append(f'SELECT start,end,deviceId,{vm} AS vmId FROM "{table}" WHERE end >= start')
        stored = 0
        for row in db.execute(f'SELECT * FROM "{table}" ORDER BY start'):
            row = dict(row)
            start, end = row.get("start"), row.get("end")
            if type(start) is not int or type(end) is not int or end < start:
                invalid += 1
                continue
            if kind == "kernel":
                name_id = next((row.get(key) for key in ("demangledName", "shortName", "mangledName", "nameId") if row.get(key) is not None), None)
                name = strings.get(name_id, row.get("name") or f"Kernel #{name_id}")
            elif kind == "memcpy":
                copy = row.get("copyKind")
                name = "Memcpy " + str(copy_names.get(copy, f"kind={copy}"))
            elif kind == "memset":
                name = "Memset"
            elif kind == "nvtx":
                name = row.get("text") or strings.get(row.get("textId")) or strings.get(row.get("nameId")) or "NVTX range"
            else:
                name = strings.get(row.get("nameId"), row.get("name") or f"API #{row.get('nameId')}")
            name = str(name)
            duration = end - start
            counts[kind] += 1
            totals[kind] += duration
            minimum = start if minimum is None else min(minimum, start)
            maximum = end if maximum is None else max(maximum, end)
            group_key = kind, name, row.get("vmId") if kind in GPU_KINDS else None, row.get("deviceId") if kind in GPU_KINDS else None
            group = groups.setdefault(group_key, {"kind": kind, "name": name, "vm": group_key[2], "device": group_key[3], "count": 0,
                                                   "total_ns": 0, "min_ns": duration, "max_ns": duration, "bytes": None})
            group["count"] += 1
            group["total_ns"] += duration
            group["min_ns"], group["max_ns"] = min(group["min_ns"], duration), max(group["max_ns"], duration)
            if type(row.get("bytes")) is int and row["bytes"] >= 0:
                group["bytes"] = (group["bytes"] or 0) + row["bytes"]
            if stored < budget and len(events) < max_events:
                event = {"kind": kind, "name": name, "start_ns": start, "end_ns": end, "duration_ns": duration,
                         "device": row.get("deviceId"), "vm": row.get("vmId"), "context": row.get("contextId"),
                         "stream": row.get("streamId"), "pid": row.get("globalPid"), "tid": row.get("globalTid"),
                         "correlation_id": row.get("correlationId"), "bytes": row.get("bytes")}
                if kind == "kernel":
                    event["grid"] = [row.get("grid" + axis) for axis in "XYZ"]
                    event["block"] = [row.get("block" + axis) for axis in "XYZ"]
                events.append(event)
                stored += 1
    # Merge intervals across all GPU activity tables, per device. Overlapping
    # streams/processes count once; duration sums remain separate evidence.
    coverage = {}
    if gpu_queries:
        query = "SELECT * FROM (" + " UNION ALL ".join(gpu_queries) + ") ORDER BY start,end"
        for row in db.execute(query):
            start, end, device, vm = row
            if type(start) is not int or type(end) is not int:
                continue
            state = coverage.setdefault((vm, device), {"device": device, "vm": vm, "start_ns": start, "end_ns": end,
                                                       "cursor_end": start, "observed_union_ns": 0, "summed_duration_ns": 0,
                                                       "largest_gap_ns": 0})
            state["summed_duration_ns"] += end - start
            state["end_ns"] = max(state["end_ns"], end)
            if start > state["cursor_end"]:
                state["largest_gap_ns"] = max(state["largest_gap_ns"], start - state["cursor_end"])
            state["observed_union_ns"] += max(0, end - max(start, state["cursor_end"]))
            state["cursor_end"] = max(state["cursor_end"], end)
        for state in coverage.values():
            state.pop("cursor_end")
            state["span_ns"] = state["end_ns"] - state["start_ns"]
            state["unobserved_ns"] = state["span_ns"] - state["observed_union_ns"]
            state["observed_coverage_pct"] = 100 * state["observed_union_ns"] / state["span_ns"] if state["span_ns"] else None
    summary = defaultdict(list)
    for group in sorted(groups.values(), key=lambda item: item["total_ns"], reverse=True):
        group["average_ns"] = group["total_ns"] / group["count"]
        group["share_of_category_duration_pct"] = 100 * group["total_ns"] / totals[group["kind"]] if totals[group["kind"]] else None
        summary[group["kind"]].append(group)
    if not counts.get("kernel"):
        notes.append("没有可用 CUDA kernel 事件；可能未启用 CUDA trace，或捕获区间内没有启动。")
    if not counts.get("nvtx"):
        notes.append("没有已闭合的 NVTX range；不能从 kernel 名称推测业务阶段。")
    if invalid:
        notes.append(f"{invalid} 条时间戳缺失/无效或未闭合的事件未计入持续时间统计。")
    notes += ["GPU 覆盖率只描述每台设备首尾被跟踪 GPU 活动之间的区间并集，不是 GPU utilization、occupancy 或算子端到端时间。",
              "未观察区间与最大间隙仅指没有所选 CUDA kernel/memcpy/memset 事件；不据此认定硬件空闲。",
              "CUDA API/OSRT 时长是主机调用时长；同类累计占比不是程序墙钟占比。NVTX 时长是范围本身的时长。",
              "NCU 与 NSYS 各自独立运行，correlation ID 不能跨报告直接匹配。"]
    return {"schema_version": 1, "source": "Nsight Systems SQLite", "origin_ns": minimum, "end_ns": maximum,
            "span_ns": maximum - minimum if minimum is not None else None,
            "counts": dict(counts), "category_total_ns": dict(totals), "gpu_activity": list(coverage.values()),
            "summary": dict(summary), "events": sorted(events, key=lambda e: e["start_ns"]),
            "timeline_sampled": len(events) < sum(counts.values()), "timeline_limit": max_events,
            "notes": notes, "tables": [table for table, _ in available]}
