#!/usr/bin/env python3
"""Collect CUDA/PTX memory experiments and compare the production C++ model.
采集 CUDA/PTX 访存实验，并与正式 C++ 模型比较。
"""
import argparse
import random
from datetime import datetime, timezone
from pathlib import Path

from memory_calibration import collect, plan, report


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("build", "check", "quick", "full"), default="quick")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.repeats <= 99 or args.device < 0:
        parser.error("repeats must be 1..99 and device nonnegative")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    args.out = (args.out or collect.ROOT / "artifacts" / f"memory-{stamp}").resolve()
    return args


def check_cases(out, binary, device):
    for index, case in enumerate(plan.sanitizer_cases()):
        collect.execute(["compute-sanitizer", "--tool", "memcheck", "--error-exitcode", "3",
                         binary, *case, device], out / f"memcheck-{index}.log")
    collect.execute(["compute-sanitizer", "--tool", "racecheck", "--error-exitcode", "3",
                     binary, "shared", 32, 513, 1, device], out / "racecheck-shared.log")


def measure(args, binary):
    model = collect.build_model(args.out)
    collect.snapshot(args.out, [binary, model])
    cases = list(plan.native_cases(args.mode == "full", args.repeats))
    random.Random(20261030).shuffle(cases)
    process, log = collect.monitor(args.out)
    try:
        collect.run_cases(args.out, binary, cases, args.device)
    finally:
        if process is not None:
            process.terminate(); process.wait(timeout=10); log.close()
    collect.run_cases(args.out, model, plan.model_cases(), prefix="model")
    report.generate(args.out)


def run(args):
    if args.mode != "build" and args.out.exists():
        raise RuntimeError("measurement directory already exists; select a new --out")
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = {"mode": args.mode, "device": args.device, "repeats": args.repeats, "status": "running"}
    collect.write_json(args.out / "manifest.json", manifest)
    try:
        collect.versions(args.out); collect.environment(args.out, "before")
        binary = collect.build_cuda(args.out)
        if args.mode == "build": print((args.out / "build.log").read_text(), end="")
        collect.snapshot(args.out, [binary])
        if args.mode == "check": check_cases(args.out, binary, args.device)
        elif args.mode != "build": measure(args, binary)
        manifest["status"] = "passed"
    except BaseException as error:
        manifest.update(status="failed", error=str(error)); raise
    finally:
        collect.environment(args.out, "after")
        collect.write_json(args.out / "manifest.json", manifest)
    print(args.out)


def main():
    run(arguments())


if __name__ == "__main__":
    main()
