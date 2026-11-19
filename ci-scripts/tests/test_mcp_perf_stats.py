"""Prove the performance rules refuse each way a benchmark could mislead."""

from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mcp_perf_stats as stats

HOST = {"model": "m", "cpu": "c", "cores": 8, "memory_bytes": 1, "os_build": "x"}


def trials(
    values: list[float], arm: str = "baseline", temperature: str = "warm", outcome: str = "ok"
) -> list[stats.Trial]:
    return [stats.Trial("startup", arm, temperature, outcome, value, 1.0) for value in values]


class SummaryTest(unittest.TestCase):
    def test_failed_canceled_and_timed_out_runs_never_enter_the_speed_figure(self) -> None:
        mixed = trials([1.0, 1.0, 1.0]) + [
            stats.Trial("startup", "baseline", "warm", "failed", 0.001, 1.0),
            stats.Trial("startup", "baseline", "warm", "canceled", 0.002, 1.0),
            stats.Trial("startup", "baseline", "warm", "timeout", 0.003, 1.0),
        ]
        summary = stats.summarize(mixed)
        self.assertEqual(summary.median, 1.0)
        self.assertEqual(summary.minimum, 1.0)
        self.assertEqual(summary.counted, 3)
        self.assertEqual(summary.excluded, {"failed": 1, "canceled": 1, "timeout": 1})
        self.assertEqual(summary.trials, 6)

    def test_a_case_with_no_finished_trial_has_no_speed(self) -> None:
        summary = stats.summarize(trials([0.1, 0.1], outcome="failed"))
        self.assertEqual(summary.counted, 0)
        self.assertTrue(math.isnan(summary.median))

    def test_noise_is_the_median_absolute_deviation(self) -> None:
        summary = stats.summarize(trials([1.0, 1.1, 0.9, 1.0, 2.0]))
        self.assertEqual(summary.median, 1.0)
        self.assertAlmostEqual(summary.noise, 0.1)

    def test_cold_and_warm_trials_are_never_pooled(self) -> None:
        with self.assertRaises(ValueError):
            stats.summarize(trials([1.0]) + trials([2.0], temperature="cold"))

    def test_the_two_arms_are_never_pooled(self) -> None:
        with self.assertRaises(ValueError):
            stats.summarize(trials([1.0]) + trials([2.0], arm="candidate"))

    def test_an_unknown_outcome_or_a_negative_duration_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            stats.summarize(trials([1.0], outcome="fast"))
        with self.assertRaises(ValueError):
            stats.summarize(trials([-1.0]))
        with self.assertRaises(ValueError):
            stats.summarize([])


class ComparisonTest(unittest.TestCase):
    def summaries(self, base: list[float], cand: list[float]) -> tuple[stats.Summary, ...]:
        return (
            stats.summarize(trials(base)),
            stats.summarize(trials(cand, arm="candidate")),
        )

    def test_a_clear_slowdown_is_reported(self) -> None:
        base, cand = self.summaries([1.0, 1.01, 0.99, 1.0], [2.0, 2.01, 1.99, 2.0])
        self.assertEqual(stats.compare(base, cand)["verdict"], "slower")

    def test_a_clear_speedup_is_reported(self) -> None:
        base, cand = self.summaries([2.0, 2.01, 1.99, 2.0], [1.0, 1.01, 0.99, 1.0])
        self.assertEqual(stats.compare(base, cand)["verdict"], "faster")

    def test_a_difference_inside_the_noise_is_not_reported_as_one(self) -> None:
        base, cand = self.summaries([1.0, 1.4, 0.6, 1.0], [1.1, 1.5, 0.7, 1.1])
        self.assertEqual(stats.compare(base, cand)["verdict"], "within noise")

    def test_the_same_trials_in_both_arms_are_within_noise(self) -> None:
        base, cand = self.summaries([1.0, 1.2, 0.8, 1.0], [1.0, 1.2, 0.8, 1.0])
        self.assertEqual(stats.compare(base, cand)["verdict"], "within noise")

    def test_too_few_finished_trials_make_the_comparison_invalid(self) -> None:
        base, cand = self.summaries([1.0, 1.0, 1.0], [2.0, 2.0])
        found = stats.compare(base, cand)
        self.assertEqual(found["verdict"], "invalid")
        self.assertIn("fewer than 3", found["reason"])

    def test_failed_runs_do_not_make_a_candidate_look_fast(self) -> None:
        base = stats.summarize(trials([1.0, 1.0, 1.0]))
        cand = stats.summarize(
            trials([1.0, 1.0, 1.0], arm="candidate")
            + trials([0.0001] * 20, arm="candidate", outcome="failed")
        )
        self.assertEqual(stats.compare(base, cand)["verdict"], "within noise")


class ValidityTest(unittest.TestCase):
    def run_of(self, **changes: object) -> dict[str, object]:
        host = {**HOST, **{k: v for k, v in changes.items() if k in HOST}}
        return {"host": host, "fixtures": changes.get("fixtures", {"scene": "a"})}

    def test_runs_on_different_hardware_cannot_be_compared(self) -> None:
        found = stats.comparison_problems(self.run_of(), self.run_of(model="other"), ("warm",) * 2)
        self.assertEqual(found, ["host model differs: 'm' against 'other'"])

    def test_runs_on_different_fixtures_cannot_be_compared(self) -> None:
        found = stats.comparison_problems(
            self.run_of(), self.run_of(fixtures={"scene": "b"}), ("warm",) * 2
        )
        self.assertEqual(found, ["the fixtures differ (their hashes are not equal)"])

    def test_a_cold_run_cannot_be_compared_with_a_warm_one(self) -> None:
        found = stats.comparison_problems(self.run_of(), self.run_of(), ("cold", "warm"))
        self.assertIn("a cold run cannot be compared with a warm run", found)

    def test_matching_runs_have_no_problem(self) -> None:
        self.assertEqual(
            stats.comparison_problems(self.run_of(), self.run_of(), ("warm", "warm")), []
        )


class AttributionTest(unittest.TestCase):
    def test_simultaneous_peak_and_sum_of_peaks_are_kept_apart(self) -> None:
        samples = [
            [
                {"pid": 1, "role": "worker", "rss": 100},
                {"pid": 2, "role": "descendant", "rss": 10},
            ],
            [
                {"pid": 1, "role": "worker", "rss": 40},
                {"pid": 2, "role": "descendant", "rss": 90},
            ],
        ]
        found = stats.attribute(samples)
        self.assertEqual(found["tree_peak_simultaneous_bytes"], 130)
        self.assertEqual(found["sum_of_individual_peaks_bytes"], 190)
        self.assertEqual(found["worker_peak_bytes"], 100)
        self.assertEqual(found["largest_descendant_peak_bytes"], 90)
        self.assertEqual(found["descendant_count"], 1)
        self.assertIn("never labelled as memory in use at once", found["note"])

    def test_descendants_are_not_folded_into_the_worker(self) -> None:
        samples = [
            [
                {"pid": 1, "role": "worker", "rss": 5},
                {"pid": 2, "role": "descendant", "rss": 500},
            ]
        ]
        self.assertEqual(stats.attribute(samples)["worker_peak_bytes"], 5)

    def test_no_samples_is_an_error_not_a_zero(self) -> None:
        with self.assertRaises(ValueError):
            stats.attribute([])


class GrowthTest(unittest.TestCase):
    def test_a_steady_rise_is_a_trend(self) -> None:
        found = stats.growth([10.0, 12.0, 14.0, 16.0, 18.0])
        self.assertAlmostEqual(found["slope_per_step"], 2.0)
        self.assertAlmostEqual(found["r_squared"], 1.0)

    def test_a_plateau_has_no_slope(self) -> None:
        self.assertEqual(stats.growth([5.0, 5.0, 5.0, 5.0])["slope_per_step"], 0.0)

    def test_too_few_points_are_refused(self) -> None:
        with self.assertRaises(ValueError):
            stats.growth([1.0, 2.0])


class BudgetTest(unittest.TestCase):
    def test_a_proposed_budget_is_never_in_force(self) -> None:
        summary = stats.summarize(trials([1.0, 1.1, 0.9, 1.0]))
        proposal = stats.propose_budget(summary)
        self.assertIs(proposal["agreed"], False)
        self.assertGreaterEqual(proposal["proposed_seconds"], 3.0)

    def test_a_budget_needs_three_finished_trials(self) -> None:
        with self.assertRaises(ValueError):
            stats.propose_budget(stats.summarize(trials([1.0, 1.0])))


if __name__ == "__main__":
    unittest.main()
