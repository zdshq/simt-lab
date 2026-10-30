#!/usr/bin/env python3
"""Portable NVIDIA kernel analysis, with a standard-library-only viewer."""
import argparse
import csv
import hashlib
import json
import math
import os
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse
from uuid import uuid4

from diagnostics import diagnose
from metrics import METRICS, parse_ncu_launches
from nsys_analysis import read_systems_sqlite

HERE = Path(__file__).resolve().parent


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    staging = path.with_name(path.name + ".tmp")
    staging.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    staging.replace(path)


def read_json(path):
    def reject_constant(value):
        raise ValueError(f"JSON 包含非有限数值：{value}")
    return json.loads(Path(path).read_text(encoding="utf-8"), parse_constant=reject_constant)


def sha256(path):
    with Path(path).open("rb") as stream:
        result = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
        return result.hexdigest()


def finite(value, positive=False):
    return (type(value) in (int, float) and math.isfinite(value)
            and (value > 0 if positive else value >= 0))


def argv(value, key):
    if not isinstance(value, list) or not value or not all(isinstance(v, str) and v and "\x00" not in v for v in value):
        raise ValueError(f"{key} 必须是非空 argv 字符串数组")
    return value


def load_config(path):
    path = Path(path).resolve()
    config = read_json(path)
    if config.get("schema_version") != 1:
        raise ValueError("配置 schema_version 必须为 1")
    for key in ("commands", "workload", "implementation", "timing", "build", "profile", "systems", "environment", "compiler_map"):
        if not isinstance(config.get(key, {}), dict):
            raise ValueError(f"{key} 必须是对象")
    for key, value in config.get("commands", {}).items():
        if key not in ("build", "check", "benchmark", "profile", "systems"):
            raise ValueError(f"未知命令类型：{key}")
        argv(value, f"commands.{key}")
    root = (path.parent / config.get("project_root", ".")).resolve()
    if not root.is_dir():
        raise ValueError(f"项目根目录不存在：{root}")
    for key in ("source_files", "binary_files"):
        if not isinstance(config.get(key, []), list) or not all(isinstance(p, str) for p in config.get(key, [])):
            raise ValueError(f"{key} 必须是路径数组")
    for key, value in config.get("environment", {}).items():
        if not isinstance(value, str) or "\x00" in key + value or "=" in key:
            raise ValueError("environment 必须是合法的字符串键值对")
    repeats = config.get("repeats", 1)
    if type(repeats) is not int or not 1 <= repeats <= 100:
        raise ValueError("repeats 必须在 1..100 之间")
    if not finite(config.get("timeout_seconds", 600), positive=True):
        raise ValueError("timeout_seconds 必须为有限正数")
    profile = config.get("profile", {})
    for key in ("set", "kernel_regex", "nvtx_include", "name_base", "replay", "cache", "clock"):
        if key in profile and not isinstance(profile[key], str):
            raise ValueError(f"profile.{key} 必须是字符串")
    if type(profile.get("enabled", False)) is not bool:
        raise ValueError("profile.enabled 必须是布尔值")
    for key, values in (("name_base", ("demangled", "mangled", "function")), ("cache", ("all", "none")), ("clock", ("none", "base", "reset"))):
        if key in profile and profile[key] not in values:
            raise ValueError(f"profile.{key} 取值无效")
    for key, default, low in (("launch_skip", 0, 0), ("launch_count", 1, 1)):
        if type(profile.get(key, default)) is not int or profile.get(key, default) < low:
            raise ValueError(f"profile.{key} 取值无效")
    for key in ("sections", "metrics"):
        if not isinstance(profile.get(key, []), list) or not all(isinstance(v, str) and v for v in profile.get(key, [])):
            raise ValueError(f"profile.{key} 必须是字符串数组")
    systems = config.get("systems", {})
    if type(systems.get("enabled", False)) is not bool:
        raise ValueError("systems.enabled 必须是布尔值")
    for key, default, allowed in (("sample", "none", ("none", "process-tree")),
                                  ("cpuctxsw", "none", ("none", "process-tree")),
                                  ("capture_range", "none", ("none", "cudaProfilerApi", "nvtx"))):
        if systems.get(key, default) not in allowed:
            raise ValueError(f"systems.{key} 取值无效")
    if not isinstance(systems.get("trace", ["cuda", "nvtx", "osrt"]), list) or not all(isinstance(v, str) and v for v in systems.get("trace", ["cuda", "nvtx", "osrt"])):
        raise ValueError("systems.trace 必须是非空字符串数组")
    if not systems.get("trace", ["cuda", "nvtx", "osrt"]):
        raise ValueError("systems.trace 不能为空")
    if type(systems.get("max_events", 12000)) is not int or not 1 <= systems.get("max_events", 12000) <= 100000:
        raise ValueError("systems.max_events 必须在 1..100000 之间")
    if systems.get("capture_range") == "nvtx" and not isinstance(systems.get("nvtx_capture"), str):
        raise ValueError("NVTX capture_range 需要 nvtx_capture 字符串")
    return config, root


def execute(command, root, log, timeout=600, environment=None):
    argv(command, "command")
    log = Path(log)
    write_json(log.with_suffix(".command.json"), {"argv": command, "cwd": str(root)})
    with log.open("wb") as output:
        process = subprocess.Popen(command, cwd=root, env=environment, stdout=output,
                                   stderr=subprocess.STDOUT, start_new_session=os.name != "nt")
        try:
            code = process.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            if os.name != "nt":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            process.wait()
            raise
    text = log.read_text(encoding="utf-8", errors="replace")
    if code:
        raise RuntimeError(f"退出码 {code}，日志 {log.name}\n{text[-2000:]}")
    return text


def probe(command, environment=None):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=15, env=environment)
        return {"ok": result.returncode == 0, "text": (result.stdout + result.stderr).strip()}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"ok": False, "text": str(error)}


def doctor(ncu=None, environment=None, nsys=None):
    gpu = probe(["nvidia-smi", "--query-gpu=name,uuid,driver_version", "--format=csv,noheader"], environment)
    devices = []
    if gpu["ok"]:
        devices = [{"name": r[0].strip(), "uuid": r[1].strip(), "driver": r[2].strip()}
                   for r in csv.reader(gpu["text"].splitlines()) if len(r) == 3]
    return {"python": platform.python_version(), "platform": platform.platform(),
            "gpu": devices or None, "gpu_probe": gpu,
            "ncu": probe([ncu or shutil.which("ncu") or "ncu", "--version"], environment),
            "nsys": probe([nsys or shutil.which("nsys") or "nsys", "--version"], environment),
            "ncu_ui": shutil.which("ncu-ui"), "nsys_ui": shutil.which("nsight-sys"),
            "nvcc": probe(["nvcc", "--version"], environment)}


def native_result(text):
    for line in reversed(text.splitlines()):
        for prefix in ("KERNEL_PERF_RESULT ", "TILELAB_RESULT "):
            if line.startswith(prefix):
                result = json.loads(line[len(prefix):])
                validate_native(result)
                return result
    raise ValueError("没有 KERNEL_PERF_RESULT/TILELAB_RESULT JSON；接入原生计时驱动")


def validate_native(result):
    if not isinstance(result, dict) or not finite(result.get("time_ms"), positive=True):
        raise ValueError("原生 time_ms 必须为有限正数")
    if "check_passed" in result and result["check_passed"] is not None and type(result["check_passed"]) is not bool:
        raise ValueError("check_passed 必须是布尔值或 null")
    for key in ("flops", "useful_bytes"):
        if key in result and not finite(result[key]):
            raise ValueError(f"{key} 必须为有限非负数")


def aggregate(samples):
    result = dict(samples[-1])
    times = [sample["time_ms"] for sample in samples]
    checks = [sample.get("check_passed") for sample in samples]
    result.update(time_ms=statistics.mean(times), samples=samples,
                  min_ms=min(times), max_ms=max(times), stdev_ms=statistics.stdev(times) if len(times) > 1 else None,
                  check_passed=False if False in checks else True if all(v is True for v in checks) else None)
    for key in ("flops", "useful_bytes"):
        values = [sample.get(key) for sample in samples]
        if any(v != values[0] for v in values):
            raise ValueError(f"独立样本工作量不一致：{key}")
    if "flops" in result:
        result["tflops"] = result["flops"] / result["time_ms"] / 1e9
    if "useful_bytes" in result:
        result["effective_gb_s"] = result["useful_bytes"] / result["time_ms"] / 1e6
    return result


def ptxas_functions(text):
    """Extract exact function names without assuming a kernel family."""
    result = {}
    blocks = re.split(r"ptxas info\s*:\s*Function properties for ", text)[1:]
    for block in blocks:
        name, _, body = block.partition("\n")
        stack = re.search(r"(\d+) bytes stack frame, (\d+) bytes spill stores, (\d+) bytes spill loads", body)
        registers = re.search(r"ptxas info\s*:\s*Used (\d+) registers", body)
        if not stack or not registers:
            continue
        resources = dict(zip(("stack_frame_bytes", "spill_store_bytes", "spill_load_bytes"), map(int, stack.groups())))
        resources["registers_per_thread"] = int(registers[1])
        if name.strip() in result and result[name.strip()] != resources:
            # Multiple architecture images may report different resources.
            result[name.strip()] = None
        else:
            result[name.strip()] = resources
    return result


def decorate(run, functions=None, mapping=None):
    functions, mapping = functions or {}, mapping or {}
    if not all(isinstance(k, str) and isinstance(v, str) for k, v in mapping.items()):
        raise ValueError("compiler_map 必须是报告函数名到 ptxas 函数名的字符串映射")
    for launch in run["launches"]:
        symbol = mapping.get(launch["kernel"], launch["kernel"])
        compiler = functions.get(symbol)
        launch["compiler_evidence"] = {"function": symbol if compiler else None, "resources": compiler,
                                        "source": "build_log" if compiler else "missing"}
        # Operator-level native resources must not be assigned to all launches.
        launch["diagnostics"] = diagnose(launch["summary"], {}, compiler, "build_log" if compiler else "missing")


def snapshot(config, root, key):
    return {name: sha256(root / name) for name in config.get(key, [])}


def ncu_tool(config):
    return config.get("ncu") or shutil.which("ncu", path=config.get("environment", {}).get("PATH", os.environ.get("PATH"))) or "ncu"


def nsys_tool(config):
    return config.get("nsys") or shutil.which("nsys", path=config.get("environment", {}).get("PATH", os.environ.get("PATH"))) or "nsys"


def new_run(data, config=None, label=None):
    identifier = uuid4().hex
    directory = Path(data).resolve() / "runs" / identifier
    directory.mkdir(parents=True)
    return directory, {"schema_version": 1, "id": identifier, "label": label or (config or {}).get("label", "Kernel 实验"),
                       "created_at": datetime.now(timezone.utc).isoformat(), "status": "running",
                       "config": config or {}, "native": {}, "launches": [], "systems": None,
                       "analyses": {}, "environment": {}, "build": {}, "artifacts": {}}


def ncu_command(config, directory, commands):
    settings = config.get("profile", {})
    if not settings.get("kernel_regex") and not settings.get("nvtx_include"):
        raise ValueError("采集需要 kernel_regex 或 nvtx_include 来明确目标")
    command = [ncu_tool(config), "--config-file", "off", "--target-processes", "all",
               "--export", str(directory / "profile"), "--replay-mode", settings.get("replay", "kernel"),
               "--cache-control", settings.get("cache", "all"), "--clock-control", settings.get("clock", "none"),
               "--launch-skip", str(settings.get("launch_skip", 0)), "--launch-count", str(settings.get("launch_count", 1))]
    if settings.get("set"):
        command += ["--set", settings["set"]]
    for section in settings.get("sections", []):
        command += ["--section", section]
    if settings.get("metrics"):
        command += ["--metrics", ",".join(settings["metrics"])]
    if settings.get("kernel_regex"):
        command += ["--kernel-name-base", settings.get("name_base", "demangled"), "--kernel-name", "regex:" + settings["kernel_regex"]]
    if settings.get("nvtx_include"):
        command += ["--nvtx", "--nvtx-include", settings["nvtx_include"]]
    return command + commands.get("profile", commands["benchmark"])


def export_csv(report, ncu, directory, timeout=600):
    text = execute([ncu or shutil.which("ncu") or "ncu", "--config-file", "off", "--import", str(report), "--page", "raw", "--csv", "--print-units", "base"],
                   directory, directory / "export.log", timeout)
    (directory / "metrics.csv").write_text(text, encoding="utf-8")
    return parse_ncu_launches(text)


def nsys_command(config, directory, commands):
    settings = config.get("systems", {})
    command = [nsys_tool(config), "profile", "--output=" + str(directory / "profile"),
               "--force-overwrite=false", "--trace=" + ",".join(settings.get("trace", ["cuda", "nvtx", "osrt"])),
               "--sample=" + settings.get("sample", "none"), "--cpuctxsw=" + settings.get("cpuctxsw", "none"),
               "--capture-range=" + settings.get("capture_range", "none")]
    if settings.get("capture_range", "none") != "none":
        command += ["--capture-range-end=stop", "--kill=none"]
    if settings.get("capture_range") == "nvtx":
        command += ["--nvtx-capture=" + settings["nvtx_capture"]]
    return command + commands.get("systems", commands.get("profile", commands["benchmark"]))


def export_systems(report, nsys, directory, timeout=600, max_events=12000):
    sqlite = directory / "systems.sqlite"
    execute([nsys or shutil.which("nsys") or "nsys", "export", "--type=sqlite",
             "--output=" + str(sqlite), "--force-overwrite=false", str(report)],
            directory, directory / "systems-export.log", timeout)
    return read_systems_sqlite(sqlite, max_events)


def run_experiment(args):
    config, root = load_config(args.config)
    commands = config.get("commands", {})
    if not commands.get("benchmark") or not config.get("workload") or not config.get("timing"):
        raise ValueError("运行需要 benchmark 命令、workload 和 timing；配置模板还未接入")
    data = Path(args.data).resolve() if args.data else root / config.get("data_dir", ".kernel-perf")
    directory, run = new_run(data, config, args.label)
    timeout = config.get("timeout_seconds", 600)
    environment = {**os.environ, **config.get("environment", {})}
    run["environment"] = doctor(ncu_tool(config), environment, nsys_tool(config))
    try:
        run["build"] = {**config.get("build", {}), "source_sha256": snapshot(config, root, "source_files")}
        if commands.get("build") and not args.skip_build:
            execute(commands["build"], root, directory / "build.log", timeout, environment)
        run["build"]["binary_sha256"] = snapshot(config, root, "binary_files")
        if commands.get("check"):
            execute(commands["check"], root, directory / "check.log", timeout, environment)
            run["check_command_passed"] = True
        samples = []
        for index in range(config.get("repeats", 1)):
            sample = native_result(execute(commands["benchmark"], root, directory / f"benchmark-{index}.log", timeout, environment))
            samples.append(sample)
            run["native"] = aggregate(samples)
            write_json(directory / "run.json", run)
            if sample.get("check_passed") is False:
                raise ValueError("原生结果正确性检查失败；停止采集")
        errors = []
        for backend, setting in (("ncu", "profile"), ("nsys", "systems")):
            if not config.get(setting, {}).get("enabled") or args.benchmark_only:
                continue
            run["analyses"][backend] = {"status": "running"}
            try:
                if backend == "ncu":
                    execute(ncu_command(config, directory, commands), root, directory / "profile.log", timeout, environment)
                    report = directory / "profile.ncu-rep"
                    if not report.is_file():
                        raise ValueError("NCU 未生成 profile.ncu-rep")
                    run["launches"] = export_csv(report, ncu_tool(config), directory, timeout)
                else:
                    execute(nsys_command(config, directory, commands), root, directory / "systems-profile.log", timeout, environment)
                    report = directory / "profile.nsys-rep"
                    if not report.is_file():
                        raise ValueError("NSYS 未生成 profile.nsys-rep")
                    run["systems"] = export_systems(report, nsys_tool(config), directory, timeout, config.get("systems", {}).get("max_events", 12000))
                run["analyses"][backend] = {"status": "succeeded"}
            except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
                run["analyses"][backend] = {"status": "failed", "error": str(error)}
                errors.append(backend + ": " + str(error))
            write_json(directory / "run.json", run)
        functions = ptxas_functions((directory / "build.log").read_text(errors="replace")) if (directory / "build.log").exists() else {}
        decorate(run, functions, config.get("compiler_map"))
        run["status"] = "failed" if errors else "succeeded"
        if errors:
            run["error"] = "\n".join(errors)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        run.update(status="failed", error=str(error) or type(error).__name__)
    finally:
        save_run(directory, run)
    print(json.dumps({"id": run["id"], "status": run["status"], "directory": str(directory), "error": run.get("error")}, ensure_ascii=False))
    return 0 if run["status"] == "succeeded" else 1


def import_experiment(args):
    config = load_config(args.config)[0] if args.config else {}
    directory, run = new_run(args.data or ".kernel-perf", config, args.label)
    run["origin"] = "import"
    try:
        if args.environment:
            shutil.copy2(args.environment, directory / "original-environment.json")
            run["environment"] = read_json(args.environment)
        if args.csv:
            shutil.copy2(args.csv, directory / "metrics.csv")
            run["launches"] = parse_ncu_launches((directory / "metrics.csv").read_text(encoding="utf-8-sig"))
            run["analyses"]["ncu"] = {"status": "succeeded"}
        elif args.nsys_report or (args.report and str(args.report).endswith(".nsys-rep")):
            report = directory / "profile.nsys-rep"
            shutil.copy2(args.nsys_report or args.report, report)
            run["systems"] = export_systems(report, args.nsys, directory, max_events=config.get("systems", {}).get("max_events", 12000))
            run["analyses"]["nsys"] = {"status": "succeeded"}
            run["export_environment"] = {"nsys": probe([args.nsys or shutil.which("nsys") or "nsys", "--version"])}
        elif args.nsys_sqlite:
            sqlite = directory / "systems.sqlite"
            shutil.copy2(args.nsys_sqlite, sqlite)
            run["systems"] = read_systems_sqlite(sqlite, config.get("systems", {}).get("max_events", 12000))
            run["analyses"]["nsys"] = {"status": "succeeded"}
        else:
            report = directory / "profile.ncu-rep"
            shutil.copy2(args.report, report)
            run["launches"] = export_csv(report, args.ncu, directory)
            run["analyses"]["ncu"] = {"status": "succeeded"}
            run["export_environment"] = {"ncu": probe([args.ncu or shutil.which("ncu") or "ncu", "--version"])}
        if args.native:
            shutil.copy2(args.native, directory / "native.json")
            run["native"] = read_json(args.native)
            validate_native(run["native"])
            if "flops" in run["native"]:
                run["native"]["tflops"] = run["native"]["flops"] / run["native"]["time_ms"] / 1e9
            if "useful_bytes" in run["native"]:
                run["native"]["effective_gb_s"] = run["native"]["useful_bytes"] / run["native"]["time_ms"] / 1e6
        run["build"] = config.get("build", {})
        functions = {}
        if args.build_log:
            shutil.copy2(args.build_log, directory / "build.log")
            functions = ptxas_functions((directory / "build.log").read_text(errors="replace"))
        if args.compiler_map:
            shutil.copy2(args.compiler_map, directory / "compiler-map.json")
        decorate(run, functions, read_json(args.compiler_map) if args.compiler_map else config.get("compiler_map"))
        if run["native"].get("check_passed") is False:
            raise ValueError("导入结果记录了正确性检查失败")
        run["status"] = "succeeded"
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        run.update(status="failed", error=str(error))
    finally:
        save_run(directory, run)
    print(json.dumps({"id": run["id"], "status": run["status"], "directory": str(directory), "error": run.get("error")}, ensure_ascii=False))
    return 0 if run["status"] == "succeeded" else 1


def portable_catalog():
    from catalog import CATALOG
    keys = set(METRICS) | {"native_time_ms", "tflops", "registers_per_thread", "shared_bytes_per_block", "local_bytes_per_thread",
                            "threads_per_block", "warps_per_block", "active_blocks_per_sm", "grid_blocks",
                            "register_capacity_pct_nominal", "spill_store_bytes", "spill_load_bytes", "stack_frame_bytes"}
    catalog = json.loads(json.dumps(CATALOG))
    catalog["entries"] = [item for item in catalog["entries"] if item["id"] in keys]
    for item in catalog["entries"]:
        item["related"] = [key for key in item["related"] if key in keys]
        item["refs"] = [ref for ref in item["refs"] if not ref.startswith("shape.")]
        if item["id"] == "tflops":
            item.update(short="当前算子的已声明 FLOP 数除以独立原生秒数。", explanation="由适配器定义工作量。没有 FLOP 定义时保持缺失。",
                        caution="GEMM 的 2MNK 仅是示例，不能套到 reduction、attention 或其他算子。")
        if item["id"].startswith("spill_") or item["id"] == "stack_frame_bytes":
            item["short"] = "与选中函数匹配的 ptxas 编译期资源统计。"
            item["refs"] = ["compiler_evidence.resources." + item["id"]]
    for step in catalog["learning_path"]:
        step["metrics"] = [key for key in step["metrics"] if key in keys]
    existing = {item["id"] for item in catalog["entries"]}
    for key, (label, unit, names) in METRICS.items():
        if key not in existing:
            catalog["entries"].append({"id": key, "name": label, "unit": unit, "aliases": names,
                                        "group": "memory", "short": "选中 launch 的 NCU 计数器。", "scope": "单个 launch",
                                        "explanation": "保留原始指标、单位和采集范围。缺失不是零。", "caution": "需结合原生耗时和其他指标判断。",
                                        "related": [], "source": "https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html"})
    return catalog


def html_report(payload):
    data = json.dumps(payload, ensure_ascii=False, allow_nan=False).replace("<", "\\u003c").replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
    return (HERE / "static" / "dashboard.html").read_text(encoding="utf-8").replace("__PERF_DATA__", data)


def save_run(directory, run):
    run["artifacts"] = {item.name: {"sha256": sha256(item), "bytes": item.stat().st_size}
                        for item in directory.iterdir() if item.is_file() and item.name not in ("run.json", "report.html", "run.json.tmp")}
    write_json(directory / "run.json", run)
    (directory / "report.html").write_text(html_report({"runs": [run], "catalog": portable_catalog(), "live": False}), encoding="utf-8")


def load_run(data, identifier):
    if not re.fullmatch(r"[0-9a-f]{32}", identifier):
        raise ValueError("无效实验 ID")
    return read_json(Path(data) / "runs" / identifier / "run.json")


def run_index(data):
    result = []
    for file in sorted((Path(data) / "runs").glob("*/run.json"), key=lambda p: p.stat().st_mtime, reverse=True):
        run = read_json(file)
        result.append({key: run.get(key) for key in ("id", "label", "created_at", "status", "native", "error")})
    return result


def compare_runs(left, right):
    blockers, differences, warnings = [], [], []
    def compare(key, a, b, required=True):
        if required and (a is None or b is None or a == {} or b == {}):
            blockers.append("缺少 " + key)
        if a != b:
            differences.append({"key": key, "left": a, "right": b})
            if required:
                blockers.append(key + " 不同")
    for key in ("workload", "timing"):
        compare(key, left.get("config", {}).get(key), right.get("config", {}).get(key))
    compare("process_environment", left.get("config", {}).get("environment", {}), right.get("config", {}).get("environment", {}), False)
    if left.get("config", {}).get("environment", {}) != right.get("config", {}).get("environment", {}):
        blockers.append("显式进程环境不同")
    compare("repeats", left.get("config", {}).get("repeats", 1), right.get("config", {}).get("repeats", 1))
    compare("GPU/driver", left.get("environment", {}).get("gpu"), right.get("environment", {}).get("gpu"))
    for key in ("type", "arch"):
        compare("build." + key, left.get("build", {}).get(key), right.get("build", {}).get(key))
    for key in ("implementation",):
        compare(key, left.get("config", {}).get(key), right.get("config", {}).get(key), False)
    compare("source_sha256", left.get("build", {}).get("source_sha256"), right.get("build", {}).get("source_sha256"), False)
    for run in (left, right):
        if run.get("status") != "succeeded":
            blockers.append("实验未成功：" + run["id"][:8])
        if run.get("native", {}).get("check_passed") is False:
            blockers.append("正确性检查失败：" + run["id"][:8])
        elif run.get("native", {}).get("check_passed") is not True:
            warnings.append("未记录完整输入正确性通过：" + run["id"][:8])
        if run.get("build", {}).get("type") != "Release":
            blockers.append("性能对比要求 Release 构建")
    a, b = left.get("native", {}).get("time_ms"), right.get("native", {}).get("time_ms")
    if not finite(a, True) or not finite(b, True):
        blockers.append("缺少有效的独立原生耗时")
    warnings.append("单轮或细小时间差需要独立重复样本验证。NCU 百分比不代表优化收益。")
    return {"left": left["id"], "right": right["id"], "comparable": not blockers,
            "speedup": a / b if not blockers else None, "blockers": list(dict.fromkeys(blockers)),
            "differences": differences, "warnings": warnings,
            "ncu_note": "NCU 指标逐 launch 查看；当前不对多 launch 做自动角色匹配或百分比聚合。"}


def serve(data, port):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            host = self.headers.get("Host", "").split(":")[0]
            if host not in ("127.0.0.1", "localhost"):
                return self.send_error(403, "Local host only")
            parsed = urlparse(self.path)
            try:
                if parsed.path == "/":
                    body, mime = html_report({"runs": [], "catalog": portable_catalog(), "live": True}).encode(), "text/html; charset=utf-8"
                elif parsed.path == "/api/runs":
                    body, mime = json.dumps(run_index(data), ensure_ascii=False).encode(), "application/json"
                elif parsed.path.startswith("/api/runs/"):
                    body, mime = json.dumps(load_run(data, parsed.path.split("/")[-1]), ensure_ascii=False).encode(), "application/json"
                elif parsed.path == "/api/compare":
                    query = parse_qs(parsed.query)
                    body = json.dumps(compare_runs(load_run(data, query["a"][0]), load_run(data, query["b"][0])), ensure_ascii=False).encode()
                    mime = "application/json"
                else:
                    return self.send_error(404)
            except (OSError, ValueError, KeyError, IndexError) as error:
                return self.send_error(400, str(error))
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(body)
        def log_message(self, *args):
            pass
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"http://127.0.0.1:{server.server_port}  数据：{Path(data).resolve()}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


def open_report(data, identifier, tool):
    run = load_run(data, identifier)
    suffix = "nsys-rep" if tool == "nsys" else "ncu-rep"
    gui = shutil.which("nsight-sys" if tool == "nsys" else "ncu-ui")
    report = Path(data).resolve() / "runs" / run["id"] / ("profile." + suffix)
    if not report.is_file():
        raise ValueError(f"实验没有原始 .{suffix} 报告；SQLite/CSV 仍可在本工具中查看")
    if not gui:
        raise ValueError(f"找不到 {tool} GUI；原始报告位置：{report}")
    process = subprocess.Popen([gui, str(report)])
    print(json.dumps({"viewer": gui, "report": str(report), "pid": process.pid}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("doctor", help="检查 GPU 与工具，不采集")
    check.add_argument("--ncu")
    check.add_argument("--nsys")
    run = commands.add_parser("run", help="独立 benchmark + 可选 NCU/NSYS")
    run.add_argument("--config", required=True)
    run.add_argument("--benchmark-only", action="store_true")
    run.add_argument("--skip-build", action="store_true")
    run.add_argument("--label")
    run.add_argument("--data")
    imp = commands.add_parser("import", help="只导入结果，不运行配置命令")
    reports = imp.add_mutually_exclusive_group(required=True)
    reports.add_argument("--csv")
    reports.add_argument("--report")
    reports.add_argument("--nsys-report")
    reports.add_argument("--nsys-sqlite")
    for key in ("config", "native", "build-log", "compiler-map", "environment", "label", "data", "ncu", "nsys"):
        imp.add_argument("--" + key)
    view = commands.add_parser("serve", help="本机只读网页")
    view.add_argument("--data", default=".kernel-perf")
    view.add_argument("--port", type=int, default=8765)
    compare = commands.add_parser("compare", help="有证据的原生 A/B 对比")
    compare.add_argument("left")
    compare.add_argument("right")
    compare.add_argument("--data", default=".kernel-perf")
    gui = commands.add_parser("open", help="在本机 Nsight GUI 打开原始报告")
    gui.add_argument("identifier")
    gui.add_argument("--tool", choices=("ncu", "nsys"), required=True)
    gui.add_argument("--data", default=".kernel-perf")
    args = parser.parse_args()
    try:
        if args.command == "doctor":
            print(json.dumps(doctor(args.ncu, nsys=args.nsys), ensure_ascii=False, indent=2))
        elif args.command == "run":
            return run_experiment(args)
        elif args.command == "import":
            return import_experiment(args)
        elif args.command == "serve":
            serve(args.data, args.port)
        elif args.command == "compare":
            print(json.dumps(compare_runs(load_run(args.data, args.left), load_run(args.data, args.right)), ensure_ascii=False, indent=2))
        elif args.command == "open":
            open_report(args.data, args.identifier, args.tool)
    except (ValueError, OSError, RuntimeError) as error:
        parser.exit(1, str(error) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
