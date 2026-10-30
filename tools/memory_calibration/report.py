"""Keep observed GPU clocks, model clocks, and inferred structures distinct.
区分实测 GPU 周期、模型周期和推断的结构。
"""
import json
import math
import statistics
from collections import defaultdict

from .collect import write_json


def distribution(values):
    ordered = sorted(values)
    return {"n": len(values), "min": min(values), "median": statistics.median(values),
            "p90": ordered[math.ceil(0.9 * len(values)) - 1], "max": max(values)}


def summarize(case):
    samples = [row for row in case["records"] if row.get("kind") == "sample"]
    excluded = {"repeat", "kind", "cycles", "sm", "final_value", "event_ms", "check_passed"}
    result = {key: value for key, value in samples[0].items() if key not in excluded}
    result.update(log=case["log"], check_passed=all(row["check_passed"] for row in samples),
                  event_ms=distribution([row["event_ms"] for row in samples]))
    if samples[0]["cycles"] is not None:
        result["cycles"] = distribution([row["cycles"] for row in samples])
        result["sm_ids"] = sorted({row["sm"] for row in samples})
        if result["iterations"]:
            result["cycles_per_step"] = result["cycles"]["median"] / result["iterations"]
    return result


def slopes(rows):
    groups = defaultdict(dict)
    for row in rows:
        if "cycles" not in row: continue
        key = (row["experiment"], row.get("stride_words"), row.get("policy"), row.get("bytes"),
               row.get("carveout_preference"), row.get("seed"), row.get("locality_group_bytes"))
        groups[key][row["iterations"]] = row
    result = []
    for key, points in groups.items():
        if not all(count in points for count in (0, 4096, 16384)): continue
        low, high = points[4096]["cycles"]["median"], points[16384]["cycles"]["median"]
        result.append({"experiment": key[0], "stride_words": key[1], "policy": key[2], "bytes": key[3],
                       "control_cycles": points[0]["cycles"], "slope_cycles_per_step": (high-low)/12288,
                       "fitted_intercept_cycles": low-(high-low)/3,
                       "logs": [points[count]["log"] for count in (0, 4096, 16384)]})
    return sorted(result, key=lambda row: (row["experiment"], row.get("stride_words") or 0, row.get("policy") or ""))


def compare_model(cases):
    result = [row for case in cases for row in case["records"]]
    for row in result:
        if row["experiment"] == "coalesce":
            expected = sorted({((lane*row["stride_words"]+row["offset_words"])*4)//32*32 for lane in range(32)})
            if sorted(row["addresses"]) != expected or row["sectors"] != len(expected):
                raise ValueError("production coalescer disagrees with independent sector enumeration")
            row.update(check_passed=True, expected_sectors=len(expected), hardware_sectors=None)
        else:
            if not row["check_passed"]: raise ValueError("model result mismatch")
            row["model_cycles_per_step"] = row["model_cycles"] / row["iterations"]
    return result


def markdown(summary):
    device = summary["device"]
    lines = ["# Memory experiments", "", f"Device: {device['name']}, CC {device['compute_capability']}; "
             f"CUDA reports {device['l2_bytes']} L2 bytes.", "",
             "Dependent-loop cycles include address calculation and control instructions. These are not isolated load latencies.",
             "Model clocks use the project's configured controller delays. Hardware sector counters are unavailable in this native run.", "",
             "## Shared-memory dependency chains", "", "| Stride (words) | GPU slope (cycles/step) | Model (clocks/step) | Zero-loop median |",
             "|---|---:|---:|---:|"]
    shared = {row["stride_words"]: row for row in summary["model"] if row["experiment"] == "shared"}
    for row in summary["slopes"]:
        if row["experiment"] == "shared":
            stride = row["stride_words"]
            lines.append(f"| {stride} | {row['slope_cycles_per_step']:.3f} | {shared[stride]['model_cycles_per_step']:.3f} | {row['control_cycles']['median']} |")
    lines.extend(["", "## Pointer chasing", "", "| Policy | Bytes | Shared preference (%) | Locality group (B) | Seed | Steps | Median cycles/step | Min/max cycles |",
                  "|---|---:|---:|---:|---:|---:|---:|---|"])
    for row in summary["native"]:
        if row["experiment"] == "chase" and row["iterations"]:
            lines.append(f"| {row['policy']} | {row['bytes']} | {row['carveout_preference']} | {row.get('locality_group_bytes', 0)} | {row['seed']} | {row['iterations']} | "
                         f"{row['cycles_per_step']:.3f} | {row['cycles']['min']} / {row['cycles']['max']} |")
    lines.extend(["", "Coalescing event timings, distributions, model counters, raw file references and measurement controls: `summary.json`.",
                  "No NVIDIA cache indexing, associativity, replacement policy or RTL is inferred uniquely from these curves.", ""])
    return "\n".join(lines)


def plots(out, summary):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return {"status": "unavailable", "reason": "matplotlib is not installed"}
    figure, axes = plt.subplots(1, 3, figsize=(15, 4), constrained_layout=True)
    groups = defaultdict(list)
    for row in summary["native"]:
        if row["experiment"] == "chase" and row["iterations"] == 8192:
            groups[(row["policy"], row["carveout_preference"], row["seed"], row.get("locality_group_bytes", 0))].append(row)
    for (policy, carveout, seed, locality), rows in sorted(groups.items()):
        rows.sort(key=lambda row: row["bytes"])
        axes[0].plot([row["bytes"]/1024 for row in rows], [row["cycles_per_step"] for row in rows], ".-",
                     label=f"{policy}, pref {carveout}%, group {locality}, seed {seed}", alpha=0.75)
    axes[0].set(xscale="log", xlabel="Working set (KiB)", ylabel="GPU cycles / dependent step", title="Native GPU chains")
    axes[0].legend(fontsize=6)
    draw_model(axes[1], summary)
    draw_shared(axes[2], summary)
    figure.savefig(out / "memory.png", dpi=160); figure.savefig(out / "memory.svg"); plt.close(figure)
    return {"status": "passed", "files": ["memory.png", "memory.svg"]}


def draw_model(axis, summary):
    for policy in ("ca", "cg"):
        rows = [row for row in summary["model"] if row["experiment"] == "chase" and row["policy"] == policy]
        axis.plot([row["bytes"]/1024 for row in rows], [row["model_cycles_per_step"] for row in rows], ".-", label=policy)
    axis.set(xscale="log", xlabel="Working set (KiB)", ylabel="Model clocks / memory request", title="Production C++ model")
    axis.legend()


def draw_shared(axis, summary):
    native = [row for row in summary["slopes"] if row["experiment"] == "shared"]
    model = [row for row in summary["model"] if row["experiment"] == "shared"]
    for rows, key, label in ((native, "slope_cycles_per_step", "GPU"), (model, "model_cycles_per_step", "Model")):
        base = next(row[key] for row in rows if row["stride_words"] == 1)
        axis.plot(range(len(rows)), [row[key]/base for row in rows], ".-", label=label)
    axis.set(xticks=range(len(native)), xticklabels=[row["stride_words"] for row in native],
             xlabel="Stride (32-bit words)", ylabel="Relative cost (stride 1 = 1)", title="Shared-memory trends")
    axis.legend()


def generate(out):
    cases = json.loads((out / "native.json").read_text())
    rows = [summarize(case) for case in cases]
    summary = {"device": cases[0]["records"][0], "native": rows, "slopes": slopes(rows),
               "model": compare_model(json.loads((out / "model.json").read_text())),
               "hardware_counter_status": "not_collected_by_native_runner"}
    summary["plots"] = plots(out, summary)
    write_json(out / "summary.json", summary)
    (out / "report.md").write_text(markdown(summary))
