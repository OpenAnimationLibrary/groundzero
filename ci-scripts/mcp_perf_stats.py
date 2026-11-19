"""Rules for reading performance trials: what counts, what is noise, what a peak means.

`scripts/perf.py` (the private qualification tooling) measures; this module decides what a set of
measurements may be said to show, so the rules can be tested without a worker. Nothing here is a
pass or fail gate on speed: a result is a statement with its noise, or a refusal to compare.

- Only trials that finished (`ok`) enter a speed figure; failed, canceled and timed-out trials are
  counted and kept, never averaged in as fast runs.
- A cold start and a warm start are different measurements and are never compared or pooled.
- Two sets of trials are comparable only on one host (same fingerprint) with the same fixtures and
  the same temperature; otherwise the comparison says why it is invalid.
- A difference smaller than the noise of either arm is reported as within noise.
- A memory peak is labelled as what it is: the largest sampled size of one process, the largest
  sampled sum over the whole process tree at one instant (simultaneous), or the sum of the
  individual peaks (which no instant ever reached).
"""

from __future__ import annotations

import math
import statistics
from collections import Counter
from dataclasses import dataclass, field
from typing import Any

OK = "ok"
OUTCOMES = (OK, "failed", "canceled", "timeout")
TEMPERATURES = ("cold", "warm", "not_applicable")
ARMS = ("baseline", "candidate")
MINIMUM_TRIALS = 3
# A median shifts by this many median absolute deviations before it is called a difference.
NOISE_FACTOR = 3.0
# Below this many seconds a clock cannot tell two medians apart on a loaded machine.
RESOLUTION_SECONDS = 0.002
HOST_KEYS = ("model", "cpu", "cores", "memory_bytes", "os_build")


@dataclass
class Trial:
    """One run of one case.

    Attributes:
        case: The case's name.
        arm: `baseline` or `candidate`.
        temperature: `cold`, `warm` or `not_applicable`.
        outcome: One of `OUTCOMES`; only `ok` counts toward a speed figure.
        seconds: Wall time of the measured step.
        load: The machine's one-minute load average when the trial ended.
        detail: Anything else the trial recorded (latency of a cancel, peaks, residue).
    """

    case: str
    arm: str
    temperature: str
    outcome: str
    seconds: float
    load: float
    detail: dict[str, Any] = field(default_factory=dict)


@dataclass
class Summary:
    """What a set of trials of one case, arm and temperature shows."""

    trials: int
    counted: int
    excluded: dict[str, int]
    median: float
    minimum: float
    maximum: float
    noise: float
    maximum_load: float

    def describe(self) -> dict[str, Any]:
        """The summary as plain data."""
        return {
            "trials": self.trials,
            "counted": self.counted,
            "excluded": self.excluded,
            "median_seconds": self.median,
            "minimum_seconds": self.minimum,
            "maximum_seconds": self.maximum,
            "noise_seconds": self.noise,
            "maximum_load": self.maximum_load,
        }


def validate(trial: Trial) -> None:
    """Refuse a trial that names an arm, temperature or outcome this module does not know."""
    if trial.arm not in ARMS:
        raise ValueError(f"arm {trial.arm!r} is not one of {ARMS}")
    if trial.temperature not in TEMPERATURES:
        raise ValueError(f"temperature {trial.temperature!r} is not one of {TEMPERATURES}")
    if trial.outcome not in OUTCOMES:
        raise ValueError(f"outcome {trial.outcome!r} is not one of {OUTCOMES}")
    if trial.seconds < 0 or math.isnan(trial.seconds):
        raise ValueError(f"seconds {trial.seconds!r} is not a duration")


def summarize(trials: list[Trial]) -> Summary:
    """Summarize trials of one case, arm and temperature; only finished trials count."""
    if not trials:
        raise ValueError("no trials to summarize")
    for trial in trials:
        validate(trial)
    keys = {(trial.case, trial.arm, trial.temperature) for trial in trials}
    if len(keys) != 1:
        raise ValueError(f"trials of more than one case, arm or temperature: {sorted(keys)}")
    counted = [trial.seconds for trial in trials if trial.outcome == OK]
    excluded = Counter(trial.outcome for trial in trials if trial.outcome != OK)
    load = max(trial.load for trial in trials)
    if not counted:
        return Summary(len(trials), 0, dict(excluded), math.nan, math.nan, math.nan, math.nan, load)
    median = statistics.median(counted)
    noise = statistics.median(abs(value - median) for value in counted)
    return Summary(
        len(trials), len(counted), dict(excluded), median, min(counted), max(counted), noise, load
    )


def host_problems(baseline: dict[str, Any], candidate: dict[str, Any]) -> list[str]:
    """Every way two host fingerprints differ on what decides speed."""
    return [
        f"host {key} differs: {baseline.get(key)!r} against {candidate.get(key)!r}"
        for key in HOST_KEYS
        if baseline.get(key) != candidate.get(key)
    ]


def comparison_problems(
    baseline: dict[str, Any], candidate: dict[str, Any], temperatures: tuple[str, str]
) -> list[str]:
    """Why two runs may not be compared at all: hosts, fixtures or temperature differ."""
    found = host_problems(baseline["host"], candidate["host"])
    if baseline["fixtures"] != candidate["fixtures"]:
        found.append("the fixtures differ (their hashes are not equal)")
    if temperatures[0] != temperatures[1]:
        found.append(f"a {temperatures[0]} run cannot be compared with a {temperatures[1]} run")
    return found


def compare(base: Summary, cand: Summary) -> dict[str, Any]:
    """Say whether the candidate is faster, slower or within noise of the baseline.

    The verdict is a reading of the trials, never a gate. It is `invalid` when either arm has too
    few finished trials to say anything.
    """
    if min(base.counted, cand.counted) < MINIMUM_TRIALS:
        return {
            "verdict": "invalid",
            "reason": f"fewer than {MINIMUM_TRIALS} finished trials in an arm "
            f"({base.counted} and {cand.counted})",
        }
    band = max(NOISE_FACTOR * max(base.noise, cand.noise), RESOLUTION_SECONDS)
    change = cand.median - base.median
    verdict = "within noise"
    if change > band:
        verdict = "slower"
    elif change < -band:
        verdict = "faster"
    return {
        "verdict": verdict,
        "change_seconds": change,
        "relative_change": change / base.median if base.median else math.nan,
        "noise_band_seconds": band,
    }


def attribute(samples: list[list[dict[str, Any]]]) -> dict[str, Any]:
    """Attribute sampled memory to the worker and to its descendants, and label each peak.

    Args:
        samples: One list per sampling instant; each entry has `pid`, `role` (`worker` or
            `descendant`) and `rss` bytes of one process.
    Returns:
        The largest single-process sampled size per role, the largest sum over the whole tree at
        one instant, the sum of every process's own peak, and the sampling count. The last two
        differ whenever the processes did not peak together; only the first of them was ever
        reached by the machine.
    """
    if not samples:
        raise ValueError("no samples to attribute")
    peaks: dict[int, int] = {}
    roles: dict[int, str] = {}
    simultaneous = 0
    for instant in samples:
        simultaneous = max(simultaneous, sum(entry["rss"] for entry in instant))
        for entry in instant:
            peaks[entry["pid"]] = max(peaks.get(entry["pid"], 0), entry["rss"])
            roles[entry["pid"]] = entry["role"]
    return {
        "samples": len(samples),
        "worker_peak_bytes": max([peaks[p] for p in peaks if roles[p] == "worker"], default=0),
        "largest_descendant_peak_bytes": max(
            [peaks[p] for p in peaks if roles[p] == "descendant"], default=0
        ),
        "descendant_count": sum(1 for role in roles.values() if role == "descendant"),
        "tree_peak_simultaneous_bytes": simultaneous,
        "sum_of_individual_peaks_bytes": sum(peaks.values()),
        "note": (
            "sampled: a peak shorter than the sampling interval can be missed; the simultaneous "
            "figure is the largest sum seen at one instant, the sum of peaks is never labelled "
            "as memory in use at once"
        ),
    }


def growth(values: list[float]) -> dict[str, float]:
    """The least-squares slope of a series over its index, and how well a line fits it."""
    count = len(values)
    if count < MINIMUM_TRIALS:
        raise ValueError(f"{count} points do not show a trend")
    mean_x, mean_y = (count - 1) / 2, statistics.fmean(values)
    spread = sum((index - mean_x) ** 2 for index in range(count))
    slope = sum((i - mean_x) * (v - mean_y) for i, v in enumerate(values)) / spread
    residual = sum((v - (mean_y + slope * (i - mean_x))) ** 2 for i, v in enumerate(values))
    total = sum((v - mean_y) ** 2 for v in values)
    return {"slope_per_step": slope, "r_squared": 1 - residual / total if total else 0.0}


def propose_budget(summary: Summary) -> dict[str, Any]:
    """A proposed limit from a baseline run: never in force, only for the owner to agree to."""
    if summary.counted < MINIMUM_TRIALS:
        raise ValueError("a budget cannot be proposed from fewer than three finished trials")
    return {
        "proposed_seconds": round(max(3 * summary.median, summary.maximum + 3 * summary.noise), 3),
        "basis": "three times the median, or the slowest trial plus three noise units",
        "agreed": False,
    }
