import importlib.util
import pathlib
import unittest

SPEC = importlib.util.spec_from_file_location("compare", pathlib.Path(__file__).resolve().parents[2] / "ops/bench/compare.py")
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)

BASELINE_P50_MS = 0.1
BASELINE_P99_MS = 0.2
BASELINE_CPU_MS = 0.15
DIRECT_P50_MS = 0.1
DIRECT_P99_MS = 0.2


def rows(*, added_p50=BASELINE_P50_MS, added_p99=BASELINE_P99_MS, cpu=BASELINE_CPU_MS, errors=0, opened=1, reused=999, concurrency=1):
    summary = lambda p50, p99: {"p50": p50, "p99": p99, "max": p99}
    common = {"scenario": "burst", "concurrency": concurrency, "body_kb": 2}
    return [
        {**common, "label": "direct", "errors": 0, "total_ms": summary(DIRECT_P50_MS, DIRECT_P99_MS)},
        {**common, "label": compare.GATEWAY_LABEL, "errors": errors, "total_ms": summary(DIRECT_P50_MS + added_p50, DIRECT_P99_MS + added_p99),
         "cpu_ms_per_request": cpu, "upstream_opened": opened, "upstream_reused": reused},
    ]


def judged(current_rows):
    baseline = compare.cells(rows())
    return compare.judge(compare.cells(current_rows), baseline)


class BenchCompareTest(unittest.TestCase):
    def test_a_run_equal_to_the_baseline_has_no_findings(self):
        self.assertEqual(judged(rows()), ([], []))

    def test_a_failed_request_is_a_hard_finding(self):
        hard, _ = judged(rows(errors=3))
        self.assertEqual(len(hard), 1)
        self.assertIn("3 failed requests", hard[0])

    def test_a_pool_that_stops_reusing_connections_is_a_hard_finding(self):
        hard, _ = judged(rows(opened=1000, reused=0))
        self.assertTrue(any("reused" in line for line in hard))

    def test_reuse_is_not_required_when_requests_overlap(self):
        hard, _ = judged(rows(opened=1000, reused=0, concurrency=8))
        self.assertEqual(hard, [])

    def test_a_slowdown_beyond_the_tolerance_is_only_advisory(self):
        slow = BASELINE_P50_MS * compare.TOLERANCE_FACTOR + compare.LATENCY_FLOOR_MS + 0.1
        hard, advisory = judged(rows(added_p50=slow))
        self.assertEqual(hard, [])
        self.assertTrue(any("added_p50_ms" in line for line in advisory))

    def test_a_slowdown_inside_the_tolerance_is_quiet(self):
        inside = BASELINE_P50_MS * compare.TOLERANCE_FACTOR
        self.assertEqual(judged(rows(added_p50=inside)), ([], []))

    def test_cpu_per_request_above_the_tolerance_is_advisory(self):
        hungry = BASELINE_CPU_MS * compare.TOLERANCE_FACTOR + compare.CPU_FLOOR_MS + 0.1
        _, advisory = judged(rows(cpu=hungry))
        self.assertTrue(any("cpu_ms_per_request" in line for line in advisory))

    def test_the_noisiest_concurrency_is_left_out(self):
        self.assertEqual(compare.cells(rows(concurrency=compare.NOISY_CONCURRENCY)), {})


if __name__ == "__main__":
    unittest.main()
