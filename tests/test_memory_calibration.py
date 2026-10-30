"""Check probe behavior and prevent native/model evidence from being conflated.
检查探针行为，防止真实 GPU 与模型证据混淆。
"""
import json
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from memory_calibration import report


def probe(*arguments):
    result = subprocess.run([sys.argv[1], *map(str, arguments)], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def sample(iterations, cycles, policy="ca"):
    return {"experiment": "chase", "policy": policy, "bytes": 4096, "seed": 123,
            "carveout_preference": 0, "locality_group_bytes": 0, "iterations": iterations,
            "cycles": {"median": cycles}, "log": f"{policy}-{iterations}"}


class CalibrationTest(unittest.TestCase):
    def test_coalescer_cli_matches_sector_coverage(self):
        for stride, offset in ((0, 7), (1, 0), (1, 1), (2, 0), (8, 7)):
            row = probe("coalesce", stride, offset)
            expected = {(lane*stride+offset)*4//32*32 for lane in range(32)}
            self.assertEqual(set(row["addresses"]), expected)
            self.assertEqual(row["sectors"], len(expected))

    def test_shared_dependency_chain_with_odd_length(self):
        for stride, clocks in ((0, 3), (1, 3), (32, 34), (33, 3)):
            row = probe("shared", stride, 17)
            self.assertTrue(row["check_passed"])
            self.assertEqual(row["model_cycles"], 17*clocks)

    def test_chase_policy_changes_actual_model_path(self):
        cached = probe("chase", "ca", 4096, 73, 123)
        bypass = probe("chase", "cg", 4096, 73, 123)
        self.assertEqual(cached["final_value"], bypass["final_value"])
        self.assertEqual((cached["l1_reads"], cached["l1_hits"]), (73, 73))
        self.assertEqual((bypass["l1_reads"], bypass["l2_hits"]), (0, 73))

    def test_slopes_exclude_fixed_cost_and_do_not_merge_policies(self):
        rows = [sample(0, 80), sample(4096, 4096*31+200), sample(16384, 16384*31+200),
                sample(4096, 10000000, "cg")]
        slopes = report.slopes(rows)
        self.assertEqual(len(slopes), 1)
        self.assertEqual(slopes[0]["slope_cycles_per_step"], 31)
        self.assertEqual(slopes[0]["control_cycles"]["median"], 80)
        self.assertEqual(slopes[0]["fitted_intercept_cycles"], 200)

    def test_model_sectors_do_not_become_hardware_measurements(self):
        cases = [{"records": [probe("coalesce", 1, 0)]}]
        row = report.compare_model(cases)[0]
        self.assertEqual(row["expected_sectors"], 4)
        self.assertIsNone(row["hardware_sectors"])
        cases[0]["records"][0]["addresses"] = [0]
        with self.assertRaises(ValueError): report.compare_model(cases)


def main():
    unittest.main(argv=[sys.argv[0]])


if __name__ == "__main__":
    main()
