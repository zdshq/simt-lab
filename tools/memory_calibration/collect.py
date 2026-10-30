"""Build and record experiments without mixing profiler and native timing.
构建并记录实验，将 profiler 与原生计时分开。
"""
import hashlib
import json
import os
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def execute(command, path, required=True, timeout=180):
    argv = [str(value) for value in command]
    record = {"argv": argv, "utc": datetime.now(timezone.utc).isoformat()}
    try:
        result = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
        path.write_text(result.stdout + result.stderr)
        record.update(returncode=result.returncode, log=path.name)
    except subprocess.TimeoutExpired as error:
        partial = b"".join(value.encode() if isinstance(value, str) else value or b""
                           for value in (error.stdout, error.stderr))
        path.write_text(partial.decode(errors="replace") + "\n" + str(error))
        record.update(returncode=None, error=str(error))
    except OSError as error:
        record.update(returncode=None, error=str(error))
        path.write_text(str(error))
    write_json(path.with_suffix(path.suffix + ".command.json"), record)
    if required and record["returncode"] != 0:
        raise RuntimeError(f"command failed; see {path}")
    return record


def build_cuda(out):
    out.mkdir(parents=True, exist_ok=True)
    execute(["nvcc", "-std=c++17", "-O3", "-arch=native", "-lineinfo", "-Xptxas=-v",
             "benchmarks/cuda/memory.cu", "-o", out / "memory-bench"], out / "build.log")
    execute(["cuobjdump", "--dump-sass", out / "memory-bench"], out / "sass.txt")
    execute(["cuobjdump", "--dump-resource-usage", out / "memory-bench"], out / "resources.txt")
    return out / "memory-bench"


def build_model(out):
    build = out / "model-build"
    execute(["cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release"], out / "cmake.log")
    execute(["cmake", "--build", build, "--target", "simt-memory-probe", "-j", "4"], out / "model-build.log")
    return build / "simt-memory-probe"


def environment(out, phase):
    fields = "timestamp,name,uuid,driver_version,pstate,clocks.sm,clocks.mem,temperature.gpu,power.draw,utilization.gpu,memory.used"
    # The absolute binary queries the real card, independently of the simulator's NVML wrapper.
    # 绝对路径直接查询真实显卡，不受模拟器 NVML 包装入口影响。
    return execute(["/usr/bin/nvidia-smi", "--query-gpu=" + fields, "--format=csv"],
                   out / f"gpu-{phase}.csv", required=False)


def snapshot(out, binaries):
    paths = [ROOT / name for name in ("CMakeLists.txt", "src/memory.cpp", "src/atomic_numeric.cpp")]
    for directory in ("benchmarks", "src/hardware", "include/simt", "tools/memory_calibration"):
        paths.extend(path for path in (ROOT / directory).rglob("*") if path.suffix in (".cu", ".cuh", ".cpp", ".hpp", ".py"))
    paths.append(ROOT / "tools/calibrate_memory.py")
    hashes = {}
    for path in sorted(set(paths)):
        relative = path.relative_to(ROOT)
        target = out / "sources" / relative; target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        hashes[str(relative)] = hashlib.sha256(path.read_bytes()).hexdigest()
    hashes.update({str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest() for path in binaries})
    write_json(out / "sha256.json", hashes)


def versions(out):
    for name, command in (("nvcc", ["nvcc", "--version"]), ("ncu", ["ncu", "--version"]),
                          ("git", ["git", "rev-parse", "HEAD"]), ("host", ["uname", "-a"])):
        execute(command, out / f"{name}.txt", required=False)
    write_json(out / "cuda-environment.json", {name: os.environ.get(name) for name in
               ("CUDA_VISIBLE_DEVICES", "CUDA_DEVICE_ORDER", "CUDA_MODULE_LOADING")})


def monitor(out):
    command = ["/usr/bin/nvidia-smi", "--query-gpu=timestamp,uuid,pstate,clocks.sm,clocks.mem,temperature.gpu,utilization.gpu",
               "--format=csv", "--loop-ms=1000"]
    log = (out / "gpu-telemetry.csv").open("w")
    try:
        return subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT), log
    except OSError as error:
        log.write(str(error)); log.close()
        return None, None


def checked_records(path, arguments, device):
    records = [json.loads(line) for line in path.read_text().splitlines() if line.startswith("{")]
    samples = [row for row in records if row.get("kind") != "device"]
    if not samples or any(row.get("check_passed") is False for row in samples):
        raise RuntimeError(f"missing or failed samples in {path}")
    if device is not None:
        repeats = arguments[{"chase": 4, "shared": 3, "coalesce": 6}[arguments[0]]]
        devices = [row for row in records if row.get("kind") == "device"]
        if len(devices) != 1 or devices[0]["device_ordinal"] != device:
            raise RuntimeError(f"unexpected device metadata in {path}")
        if len(samples) != repeats or any(row.get("check_passed") is not True for row in samples):
            raise RuntimeError(f"incomplete validated samples in {path}")
    return records


def run_cases(out, binary, cases, device=None, prefix="native"):
    combined = []
    for index, arguments in enumerate(cases):
        argv = [binary, *arguments] + ([] if device is None else [device])
        path = out / f"{prefix}-{index:03}.jsonl"
        execute(argv, path)
        records = checked_records(path, arguments, device)
        combined.append({"argv": [str(arg) for arg in argv], "log": path.name, "records": records})
        write_json(out / f"{prefix}.json", combined)
        print(f"{prefix} {index+1}: {' '.join(map(str, arguments))}", flush=True)
    return combined
