"""Generated sequences and round-trip properties, compared with an independent model.

`mcp_property_model.py` computes what a scene, its cells and its curves must read back after a
sequence of operations; this module replays generated sequences against the worker and compares.
Every sequence is a function of a printed seed and length: `GZ_PROPERTY_SEEDS=5,9` and
`GZ_PROPERTY_LENGTH=12` replay chosen ones, `GZ_PROPERTY_FRESH=3` adds three new random seeds
(printed), and `GZ_PROPERTY_STEPS=<file>` replays a sequence written as JSON. A failing
sequence is cut to the shortest prefix that still fails and then to the fewest steps, and
printed so that it can be pasted into such a file.
"""

from __future__ import annotations

import json
import math
import os
import random
import re
import tempfile
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_property_curves import Key, key_record, sample
from mcp_property_effects import EFFECT_OPERATIONS, FX_CHANNEL, FX_TYPE, FX_UNIT, generate_effects
from mcp_property_model import (
    CELL_HORIZON,
    CHANNEL_UNITS,
    OPERATIONS,
    SAMPLE_FRAMES,
    TOOLS,
    Handles,
    ModelError,
    Scene,
    expected,
    generate,
    new_scene,
)
from mcp_property_output import project
from mcp_w2_cases import _raw

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

# Chosen from seeds 1 to 399 by a greedy cover of what the model's sequences do: every operation
# and every refusal it answers, every retime kind and replace mapping, and a reopened scene that
# holds cells. A test keeps the cover.
DEFAULT_SEEDS = (87, 10, 56, 271, 94, 16, 245, 34, 399, 237, 276, 9, 21, 154, 157, 11, 92, 1, 7)
# The same for effect-channel sequences: every effect operation, an OUT_OF_RANGE refusal, a
# refusal of the curve rules and a reopened scene that holds effect keys.
EFFECT_SEEDS = (13, 45, 183, 121, 90)
DEFAULT_LENGTH = 24
TOLERANCE = 1e-9
LOG_NAME = "property-sequences.ndjson"
VECTOR_LARGEST = 65535  # Native PLI files number drawings from 1 to 65535.
RASTER_LARGEST = 999999
NAME_LIMIT = 128
MAX_SAVED_ROW = 999999
WRITE_BUDGET = 1200  # A worker keeps 2048 receipts; shrinking stops well before that.


def seeds_to_run() -> list[int]:
    """The seeds of this run: `GZ_PROPERTY_SEEDS` when set, else the fixed defaults."""
    text = os.environ.get("GZ_PROPERTY_SEEDS", "")
    seeds = [int(item) for item in text.split(",") if item.strip()] or list(DEFAULT_SEEDS)
    fresh = int(os.environ.get("GZ_PROPERTY_FRESH", "0"))
    return seeds + [random.SystemRandom().randrange(10**6) for _ in range(fresh)]


def length_to_run() -> int:
    """The length of each generated sequence: `GZ_PROPERTY_LENGTH` or the default."""
    return int(os.environ.get("GZ_PROPERTY_LENGTH", DEFAULT_LENGTH))


def differences(want: Any, got: Any, path: str = "", tolerance: float = TOLERANCE) -> list[str]:
    """List where two observations differ; numbers agree within a tiny tolerance."""
    if isinstance(want, dict) and isinstance(got, dict):
        found = []
        for key in sorted(set(want) | set(got), key=str):
            if key not in want or key not in got:
                found.append(
                    f"{path}/{key}: {'missing' if key in want else 'unexpected'} in worker"
                )
            else:
                found.extend(differences(want[key], got[key], f"{path}/{key}", tolerance))
        return found
    if isinstance(want, list) and isinstance(got, list):
        if len(want) != len(got):
            return [f"{path}: {len(want)} items expected, the worker has {len(got)}"]
        found = []
        for index, (left, right) in enumerate(zip(want, got, strict=True)):
            found.extend(differences(left, right, f"{path}[{index}]", tolerance))
        return found
    numbers = all(
        isinstance(item, int | float) and not isinstance(item, bool) for item in (want, got)
    )
    if numbers:
        same = math.isclose(want, got, rel_tol=tolerance, abs_tol=tolerance)
    else:
        same = type(want) is type(got) and want == got
    return [] if same else [f"{path}: expected {want!r}, the worker reads {got!r}"]


def observed_row(key: dict[str, Any]) -> list[Any]:
    """A key as animation.keys reports it, in the row form the model uses."""
    incoming = key["incoming"]
    kinds = {"none": ["none"]}
    if incoming["kind"] == "speed":
        kinds["speed"] = ["speed", incoming["frame_delta"], incoming["value_delta"]]
    if incoming["kind"] == "ease":
        kinds["ease"] = ["ease", incoming["frames"]]
    outgoing = key.get("outgoing")
    if outgoing is not None:
        outgoing = (
            ["speed", outgoing["frame_delta"], outgoing["value_delta"]]
            if "frame_delta" in outgoing
            else ["ease", outgoing["frames"]]
        )
    return [
        key["frame"],
        key["value"],
        key["interpolation"],
        key["step"],
        kinds[incoming["kind"]],
        outgoing,
    ]


RICH_TOLERANCE = 1e-6


def compare_observations(want: dict[str, Any], got: dict[str, Any]) -> list[str]:
    """Compare what the model expects with what the worker reads, curve by curve.

    A curve with a speed or ease segment is compared to 1e-6, since its value comes from solving
    a Bezier; a curve whose handles cross has no modelled shape, so its samples are not compared.
    """
    found = differences({**want, "curves": {}}, {**got, "curves": {}})
    if set(want["curves"]) != set(got["curves"]):
        return found + [f"/curves: expected {sorted(want['curves'])}, got {sorted(got['curves'])}"]
    for name, curve in want["curves"].items():
        actual = dict(got["curves"][name])
        wanted = {key: value for key, value in curve.items() if key != "rich"}
        if wanted["samples"] is None:
            actual["samples"] = wanted["samples"] = None
        found += differences(
            wanted, actual, f"/curves/{name}", RICH_TOLERANCE if curve["rich"] else TOLERANCE
        )
    return found


@dataclass
class Failure:
    """The first step at which the worker and the model disagree."""

    index: int
    operation: str
    messages: list[str]


class Session:
    """One draft scene driven through a sequence, with the handles its answers named."""

    def __init__(self, case: NativeAcceptance) -> None:
        self.case = case
        self.root = Path(tempfile.mkdtemp(prefix="run-", dir=case.workspace))
        self.model: Scene = new_scene()
        self.handles: Handles = {
            "level": {},
            "column": {},
            "peg": {},
            "column_object": {},
            "camera": {},
            "group": {},
            "fx": {},
        }
        self.document = self._call_create()
        self._learn_cameras()

    def _call_create(self) -> str:
        camera = self.model.camera
        return self.case.mutate(
            "scene.create",
            None,
            name="Property",
            frame_rate=self.model.frame_rate,
            width_px=camera["width_px"],
            height_px=camera["height_px"],
            width_in=camera["width_in"],
            height_in=camera["height_in"],
        )["document_id"]

    def close(self) -> None:
        """Release the scene so that a run never holds more than two."""
        self.case.mutate("scene.discard", self.document)

    def call(self, tool: str, params: dict[str, Any]) -> dict[str, Any]:
        """Call a writing tool on the scene with its current revision."""
        document = None if tool == "scene.open" else self.document
        return self.case.worker.call(tool, self.case.write_params(document, params))

    def read(self, tool: str, params: dict[str, Any]) -> dict[str, Any]:
        """Call a reading tool on the scene."""
        return self.case.success(tool, {"document_id": self.document, **params})

    def summary(self) -> dict[str, Any]:
        """Read the scene summary, which holds a model scene in one page."""
        summary = self.read("scene.summary", {})
        partial = [name for name, page in summary["pages"].items() if page["next_offset"]]
        self.case.assertEqual(partial, [], "the model compared a partial summary")
        return summary

    def _cameras(self) -> list[dict[str, Any]]:
        listing = self.read(
            "camera.list", {"expected_revision": self.case.revisions[self.document]}
        )
        return listing["cameras"]

    def _learn_cameras(self) -> None:
        if "camera.list" in TOOLS:
            self.handles["camera"] = {row["name"]: row["camera_id"] for row in self._cameras()}

    def _observe_cameras(self) -> list[dict[str, Any]]:
        keys = ("name", "width_px", "height_px", "width_in", "height_in")
        return [
            {**{key: row[key] for key in keys}, "roles": sorted(row["roles"])}
            for row in self._cameras()
        ]

    def _observe_properties(self, shape: dict[str, Any]) -> dict[str, Any]:
        revision = self.case.revisions[self.document]
        record = self.read("scene.properties.inspect", {"expected_revision": revision})
        return project(record, shape)

    def _observe_objects(self, summary: dict[str, Any], want: dict[str, Any]) -> dict[str, Any]:
        """Parents and frame 0 positions of pegs and columns, and the schematic groups."""
        named = {c["object_id"]: c["name"] for c in summary["columns"] if "object_id" in c}
        named.update(
            {o["object_id"]: o["name"] for o in summary["objects"] if o["kind"] == "pegbar"}
        )
        parents = {
            named[o["object_id"]]: named.get(o.get("parent_id"))
            for o in summary["objects"]
            if o["object_id"] in named
        }
        found: dict[str, Any] = {"parents": parents, "origins": self._origins(named)}
        if "groups" in want:
            found["groups"] = self._observe_groups(named)
        return found

    def _origins(self, named: dict[str, str]) -> dict[str, list[float]]:
        origins = {}
        for handle, name in named.items():
            values = []
            for channel in ("x", "y"):
                target = {"object_id": handle, "channel": channel, "unit": "inch", "frames": [0]}
                values.append(self.read("animation.sample", target)["values"][0]["value"])
            origins[name] = values
        return origins

    def _observe_groups(self, named: dict[str, str]) -> dict[str, Any]:
        keys = {number: key for key, number in self.handles["group"].items()}
        found = {}
        revision = self.case.revisions[self.document]
        for group in self.read("object.groups", {"expected_revision": revision})["groups"]:
            parent = group["parent_group_number"]
            found[keys.get(group["group_number"], f"?{group['group_number']}")] = {
                "name": group["name"],
                "members": sorted(named[handle] for handle in group["object_ids"]),
                "parent": None if parent is None else keys.get(parent, f"?{parent}"),
            }
        return found

    def _observe_extras(self, summary: dict[str, Any]) -> dict[str, Any]:
        """What the tools beyond the first model's add to the observation."""
        want = expected(self.model)
        found: dict[str, Any] = {}
        if "cameras" in want:
            found["cameras"] = self._observe_cameras()
        if "properties" in want:
            found["properties"] = self._observe_properties(want["properties"])
        if "parents" in want:
            found.update(self._observe_objects(summary, want))
        return found

    def observe(self) -> dict[str, Any]:
        """Read everything the model predicts, with the worker's read tools only."""
        summary = self.summary()
        names = {item["level_id"]: item["name"] for item in summary["levels"]}
        camera = summary["camera"]
        pegs = [item for item in summary["objects"] if item["kind"] == "pegbar"]
        return {
            **self._observe_extras(summary),
            "frame_rate": summary["frame_rate"],
            "camera": {
                key: camera[key] for key in ("width_px", "height_px", "width_in", "height_in")
            },
            "frame_count": summary["frame_count"],
            "levels": {
                item["name"]: sorted((d["number"], d["suffix"]) for d in item["drawings"])
                for item in summary["levels"]
            },
            "columns": [self._column(item, names) for item in summary["columns"]],
            "pegs": sorted(item["name"] for item in pegs),
            "curves": {**self._curves(pegs), **self._effect_curves()},
        }

    def _effects(self) -> dict[str, str]:
        """The modelled effects of the scene, by name, with their handles."""
        graph = self.read("fx.graph", {})
        return {item["name"]: item["fx_id"] for item in graph["effects"] if item["type"] == FX_TYPE}

    def _effect_curves(self) -> dict[str, Any]:
        curves = {}
        for name, effect in self._effects().items():
            target = {"fx_id": effect, "channel": FX_CHANNEL, "unit": FX_UNIT}
            record = self.read("animation.keys", target)
            if not record["keys"]:
                continue
            values = self.read("animation.sample", {**target, "frames": SAMPLE_FRAMES})
            curves[f"{name}/{FX_CHANNEL}"] = {
                "keys": [observed_row(key) for key in record["keys"]],
                "cycle": record["cycle"],
                "samples": [item["value"] for item in values["values"]],
            }
        return curves

    def _column(self, item: dict[str, Any], names: dict[str, str]) -> dict[str, Any]:
        cells = self.read(
            "column.cells",
            {"column_id": item["column_id"], "start_frame": 0, "length": CELL_HORIZON},
        )["cells"]
        rows = [
            None
            if cell["empty"]
            else [
                names[cell["level_id"]],
                cell["drawing_id"]["number"],
                cell["drawing_id"]["suffix"],
            ]
            for cell in cells
        ]
        keys = ("name", "visible", "locked", "opacity")
        return {**{key: item[key] for key in keys}, "rows": rows}

    def _curves(self, pegs: list[dict[str, Any]]) -> dict[str, Any]:
        curves = {}
        for peg in pegs:
            for channel, unit in CHANNEL_UNITS.items():
                target = {"object_id": peg["object_id"], "channel": channel, "unit": unit}
                record = self.read("animation.keys", target)
                keys = record["keys"]
                if not keys:
                    continue
                values = self.read("animation.sample", {**target, "frames": SAMPLE_FRAMES})
                curves[f"{peg['name']}/{channel}"] = {
                    "keys": [observed_row(key) for key in keys],
                    "cycle": record["cycle"],
                    "samples": [item["value"] for item in values["values"]],
                }
        return curves

    def step(self, index: int, step: dict[str, Any]) -> Failure | None:
        """Apply one step to the model and the worker, then compare everything."""
        operation = ALL_OPERATIONS[step["op"]]
        before = self.case.revisions[self.document]
        error = operation.effect(self.model, step["args"])
        tool, params = operation.request(step["args"], self.handles)
        if operation.reopens:
            params = {**params, "directory": str(self.root / params["directory"])}
        try:
            response = self.call(tool, params)
            messages = self._outcome(operation.name, step, response, error, before)
            if not messages and operation.reopens and error is None:
                self._reopen(response["result"])
            messages += compare_observations(expected(self.model), self.observe())
        except (AssertionError, RuntimeError) as problem:
            messages = [f"{type(problem).__name__}: {problem}"]
        return Failure(index, operation.name, messages) if messages else None

    def _outcome(
        self,
        name: str,
        step: dict[str, Any],
        response: dict[str, Any],
        error: str | None,
        before: str,
    ) -> list[str]:
        if error is not None:
            failure = response.get("error", {})
            code = failure.get("code", "success")
            wanted, _, reason = error.partition("/")
            if reason and code == wanted:
                code = f"{code}/{failure.get('details', {}).get('reason')}"
            same = self.case.revisions[self.document] == before
            return [] if code == error and same else [f"{name}: expected error {error}, got {code}"]
        if "error" in response:
            return [f"{name}: expected success, got {response['error']}"]
        result = response["result"]
        if "revision" in result and result.get("document_id") == self.document:
            self.case.revisions[self.document] = result["revision"]
        ALL_OPERATIONS[step["op"]].learn(step["args"], result, self.handles)
        return []

    def _reopen(self, saved: dict[str, Any]) -> None:
        opened = self.case.mutate("scene.open", None, path=saved["package_directory"])
        old, self.document = self.document, opened["document_id"]
        self.case.mutate("scene.discard", old)
        summary = self.summary()
        self.handles["level"] = {item["name"]: item["level_id"] for item in summary["levels"]}
        self.handles["column"] = {item["name"]: item["column_id"] for item in summary["columns"]}
        self.handles["column_object"] = {
            item["name"]: item["object_id"] for item in summary["columns"] if "object_id" in item
        }
        self._learn_cameras()
        self.handles["peg"] = {
            item["name"]: item["object_id"]
            for item in summary["objects"]
            if item["kind"] == "pegbar"
        }
        self.handles["fx"] = self._effects()


ALL_OPERATIONS = {**OPERATIONS, **EFFECT_OPERATIONS}


def replay(case: NativeAcceptance, steps: list[dict[str, Any]]) -> Failure | None:
    """Run steps on a fresh scene and return the first disagreement, or None."""
    session = Session(case)
    try:
        for index, step in enumerate(steps):
            try:
                failure = session.step(index, step)
            except ModelError as problem:
                return Failure(index, step["op"], [f"not a valid step for the model: {problem}"])
            if failure:
                return failure
        return None
    finally:
        session.close()


def _same_failure(original: Failure, other: Failure) -> bool:
    """Whether a shorter sequence fails at the same operation and in the same place."""
    head = original.messages[0].split(":")[0]
    return other.operation == original.operation and other.messages[0].split(":")[0] == head


def shrink(
    case: NativeAcceptance, steps: list[dict[str, Any]], failure: Failure
) -> tuple[list[dict[str, Any]], Failure]:
    """Cut a failing sequence to its shortest failing prefix, then to as few steps as it needs."""
    kept = steps[: failure.index + 1]
    again = replay(case, kept)
    if again is None:
        return kept, Failure(
            failure.index, failure.operation, ["not reproducible: " + "; ".join(failure.messages)]
        )
    kept, found = kept[: again.index + 1], again
    index = 0
    while index < len(kept) - 1 and case.operations < WRITE_BUDGET:
        trial = kept[:index] + kept[index + 1 :]
        outcome = replay(case, trial)
        if outcome is not None and _same_failure(found, outcome):
            kept, found = trial[: outcome.index + 1], outcome
        else:
            index += 1
    return kept, found


def _log(case: NativeAcceptance, seed: int | None, steps: list[dict[str, Any]]) -> None:
    line = json.dumps({"seed": seed, "length": len(steps), "steps": steps})
    print(f"property seed={seed} length={len(steps)}")
    with (case.directory / LOG_NAME).open("a", encoding="utf-8") as log:
        log.write(line + "\n")


def check_sequence(case: NativeAcceptance, seed: int | None, steps: list[dict[str, Any]]) -> None:
    """Replay a sequence; on a disagreement fail with the seed and the smallest sequence."""
    _log(case, seed, steps)
    failure = replay(case, steps)
    if failure is None:
        return
    smallest, found = shrink(case, steps, failure)
    report = [
        f"seed={seed} length={len(steps)}: step {failure.index} ({failure.operation}) disagrees",
        *failure.messages,
        f"smallest failing sequence ({len(smallest)} steps; GZ_PROPERTY_STEPS=<file> replays it):",
        json.dumps(smallest),
        *found.messages,
    ]
    case.fail("\n".join(report))


@acceptance_case
def generated_sequences_match_the_independent_model(case: NativeAcceptance) -> None:
    """Replay seeded sequences of scene, level, column, exposure and curve edits and compare.

    After every step the settings, every column's rows, and every curve's keys and samples
    that the worker reads back must equal what the model computed from the steps alone,
    including the refusals the model predicts; some steps save and reopen the scene.
    """
    path = os.environ.get("GZ_PROPERTY_STEPS")
    if path:
        check_sequence(case, None, json.loads(Path(path).read_text(encoding="utf-8")))
        return
    for seed in seeds_to_run():
        with case.subTest(seed=seed):
            check_sequence(case, seed, generate(seed, length_to_run()))


@acceptance_case
def generated_effect_key_sequences_match_the_independent_model(case: NativeAcceptance) -> None:
    """Replay seeded sequences of key edits on effect channels and compare with the model.

    The animation key tools write a blur's value channel as they write a stage channel; after
    every step its keys and samples must equal what the model computed, and every write the
    model finds outside the admitted range must be refused as OUT_OF_RANGE with nothing
    changed. `GZ_PROPERTY_SEEDS` and `GZ_PROPERTY_LENGTH` choose the sequences here too.
    """
    seeds = seeds_to_run() if os.environ.get("GZ_PROPERTY_SEEDS") else list(EFFECT_SEEDS)
    for seed in seeds:
        with case.subTest(seed=seed):
            check_sequence(case, seed, generate_effects(seed, length_to_run()))


def _seeded(case: NativeAcceptance, kind: str) -> list[tuple[int, random.Random]]:
    """The seeds of a round-trip property, printed, each with its own generator."""
    runs = []
    for seed in seeds_to_run()[:3]:
        print(f"property {kind} seed={seed}")
        with (case.directory / LOG_NAME).open("a", encoding="utf-8") as log:
            log.write(json.dumps({"property": kind, "seed": seed}) + "\n")
        runs.append((seed, random.Random(f"{kind}:{seed}")))
    return runs


def _reopened(case: NativeAcceptance, document: str, name: str) -> str:
    saved = case.mutate("scene.save_copy", document, directory=str(case.workspace / name))
    return case.mutate("scene.open", None, path=saved["package_directory"])["document_id"]


def _read_keys(case: NativeAcceptance, document: str, target: dict[str, Any]) -> list[Any]:
    keys = case.success("animation.keys", {"document_id": document, **target})["keys"]
    return [[key["frame"], key["value"]] for key in keys]


def _peg_by_name(case: NativeAcceptance, document: str, name: str) -> str:
    objects = case.success("scene.summary", {"document_id": document})["objects"]
    return next(item["object_id"] for item in objects if item["name"] == name)


def _expose_at(case: NativeAcceptance, document: str, frames: list[int]) -> list[Any]:
    """Expose one drawing at each frame of an otherwise empty column; return the rows by hand."""
    level = case.mutate("level.create", document, name="Frames", format="vector")["level_id"]
    drawings = [{"number": number, "suffix": ""} for number in (1, 2, 3)]
    for drawing in drawings:
        case.mutate("drawing.create", document, level_id=level, drawing_id=drawing, format="vector")
    column = case.mutate("column.create", document, name="Frames")["column_id"]
    rows = []
    for index, frame in enumerate(frames):
        drawing = drawings[index % 3]
        case.mutate(
            "column.expose",
            document,
            column_id=column,
            level_id=level,
            drawing_id=drawing,
            start_frame=frame,
            length=1,
        )
        rows.append([frame, drawing["number"], False])
    case.assertEqual(_exposed_rows(case, document, frames), rows)
    return rows


def _exposed_rows(case: NativeAcceptance, document: str, frames: list[int]) -> list[Any]:
    column = case.success("scene.summary", {"document_id": document})["columns"][0]["column_id"]
    rows = []
    for frame in frames:
        read = {"document_id": document, "column_id": column, "start_frame": frame, "length": 1}
        cell = case.success("column.cells", read)["cells"][0]
        rows.append([cell["frame"], cell["drawing_id"]["number"], cell["empty"]])
    return rows


@acceptance_case
def frames_round_trip_through_keys_exposures_and_reopen(case: NativeAcceptance) -> None:
    """Keys and exposures at random frames, and at the first and last frame, read back exact."""
    for seed, rng in _seeded(case, "frames"):
        with case.subTest(seed=seed):
            frames = sorted({0, 1, 999999, 1000000, *(rng.randint(0, 1000000) for _ in range(30))})
            document = case.create(f"Frames {seed}")
            peg = case.mutate("object.create", document, name="Peg", kind="pegbar")["object_id"]
            target = {"object_id": peg, "channel": "x", "unit": "inch"}
            keys = [
                {"frame": f, "value": rng.randint(-400, 400) / 8, "interpolation": "linear"}
                for f in frames
            ]
            case.mutate("animation.set_keys", document, **target, keys=keys)
            want = [[key["frame"], key["value"]] for key in keys]
            case.assertEqual(_read_keys(case, document, target), want)
            sampled = case.success(
                "animation.sample", {"document_id": document, **target, "frames": frames}
            )
            case.assertEqual([v["value"] for v in sampled["values"]], [key[1] for key in want])
            exposed = _expose_at(case, document, [f for f in frames if f <= MAX_SAVED_ROW])
            reopened = _reopened(case, document, f"frames-{seed}")
            moved = {**target, "object_id": _peg_by_name(case, reopened, "Peg")}
            case.assertEqual(_read_keys(case, reopened, moved), want)
            case.assertEqual(_exposed_rows(case, reopened, [r[0] for r in exposed]), exposed)
            case.mutate("scene.discard", reopened)
            case.mutate("scene.discard", document)


def _channel_keys(rng: random.Random, descriptor: dict[str, Any]) -> list[dict[str, Any]]:
    """Keys at three frames: both bounds of the channel and a value of full precision."""
    low, high = descriptor["value_bounds"]
    low, high = max(low, -1e4), min(high, 1e4)
    values = [low, high, rng.uniform(low, high)]
    return [
        {"frame": frame, "value": value, "interpolation": "linear"}
        for frame, value in zip((0, 7, 20), values, strict=True)
    ]


def _check_channels(
    case: NativeAcceptance, document: str, peg: str, wanted: dict[str, Any]
) -> None:
    for channel, (unit, keys) in wanted.items():
        target = {"object_id": peg, "channel": channel, "unit": unit}
        got = case.success("animation.keys", {"document_id": document, **target})["keys"]
        case.assertEqual([k["frame"] for k in got], [k["frame"] for k in keys], channel)
        for key, read in zip(keys, got, strict=True):
            case.assertEqual(differences(key["value"], read["value"]), [], (channel, unit))


@acceptance_case
def values_round_trip_in_every_channel_unit_through_keys_and_reopen(case: NativeAcceptance) -> None:
    """Keys at the bounds of each channel and in between read back in their unit, and reopened."""
    for seed, rng in _seeded(case, "units"):
        with case.subTest(seed=seed):
            document = case.create(f"Units {seed}")
            peg = case.mutate("object.create", document, name="Peg", kind="pegbar")["object_id"]
            channels = case.success(
                "animation.channels", {"document_id": document, "object_id": peg}
            )
            wanted = {}
            for descriptor in channels["channels"]:
                if not descriptor["writable"]:
                    continue
                keys = _channel_keys(rng, descriptor)
                target = {
                    "object_id": peg,
                    "channel": descriptor["channel"],
                    "unit": descriptor["unit"],
                }
                case.mutate("animation.set_keys", document, **target, keys=keys)
                wanted[descriptor["channel"]] = (descriptor["unit"], keys)
            case.assertGreaterEqual(len(wanted), 7)
            _check_channels(case, document, peg, wanted)
            reopened = _reopened(case, document, f"units-{seed}")
            _check_channels(case, reopened, _peg_by_name(case, reopened, "Peg"), wanted)
            case.mutate("scene.discard", reopened)
            case.mutate("scene.discard", document)


NAME_CHARACTERS = "abcXYZ019 _-.<>&\"'%$+#()[]{}@!~,;=中é \U0001f680́\t"
SAFE_CHARACTERS = "abcXYZ019 _-.+#()[]{}@!~,;=%&'中é\U0001f680́"


def _composed(text: str) -> str:
    """The text with every name composed, which is how a macOS worker reports stored names."""
    return unicodedata.normalize("NFC", text)


def _random_text(rng: random.Random, alphabet: str, longest: int) -> str:
    return "".join(rng.choice(alphabet) for _ in range(rng.randint(1, longest)))


def _random_drawings(rng: random.Random, count: int, largest: int) -> list[dict[str, Any]]:
    drawings: dict[tuple[int, str], dict[str, Any]] = {}
    while len(drawings) < count:
        suffix = "".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(rng.randint(0, 8)))
        number = rng.choice([1, 2, largest, largest - 1, rng.randint(1, largest)])
        drawings[(number, suffix)] = {"number": number, "suffix": suffix}
    return list(drawings.values())


def _ids(items: list[dict[str, Any]]) -> list[tuple[int, str]]:
    return sorted((item["number"], item["suffix"]) for item in items)


@acceptance_case
def identifiers_and_names_round_trip_through_exposures_and_reopen(case: NativeAcceptance) -> None:
    """Drawing numbers and suffixes, and level, column and peg names, survive a save and reopen."""
    for seed, rng in _seeded(case, "identifiers"):
        with case.subTest(seed=seed):
            document = case.create(_random_text(rng, NAME_CHARACTERS, 40))
            level_name = _random_text(rng, NAME_CHARACTERS, 60)
            column_name = _random_text(rng, NAME_CHARACTERS, 60)
            peg_name = _random_text(rng, NAME_CHARACTERS, 60)
            level = case.mutate("level.create", document, name=level_name, format="vector")
            drawings = _random_drawings(rng, 12, VECTOR_LARGEST)
            for drawing in drawings:
                case.mutate(
                    "drawing.create",
                    document,
                    level_id=level["level_id"],
                    drawing_id=drawing,
                    format="vector",
                )
            column = case.mutate("column.create", document, name=column_name)
            for frame, drawing in enumerate(drawings):
                case.mutate(
                    "column.expose",
                    document,
                    column_id=column["column_id"],
                    level_id=level["level_id"],
                    drawing_id=drawing,
                    start_frame=frame,
                    length=1,
                )
            case.mutate("object.create", document, name=peg_name, kind="pegbar")
            reopened = _reopened(case, document, f"ids-{seed}")
            summary = case.success("scene.summary", {"document_id": reopened})
            case.assertEqual([item["name"] for item in summary["levels"]], [level_name])
            case.assertEqual(_ids(summary["levels"][0]["drawings"]), _ids(drawings))
            case.assertEqual([item["name"] for item in summary["columns"]], [column_name])
            names = [item["name"] for item in summary["objects"] if item["kind"] == "pegbar"]
            case.assertEqual(names, [peg_name])
            read = {"document_id": reopened, "column_id": summary["columns"][0]["column_id"]}
            cells = case.success("column.cells", {**read, "start_frame": 0, "length": 12})["cells"]
            case.assertEqual(_ids([c["drawing_id"] for c in cells]), _ids(drawings))
            case.assertEqual([c["drawing_id"] for c in cells], drawings)
            case.mutate("scene.discard", reopened)
            case.mutate("scene.discard", document)


@acceptance_case
def package_paths_round_trip_through_save_and_open(case: NativeAcceptance) -> None:
    """Save into folders whose names are awkward and open the package from the path it reported."""
    for seed, rng in _seeded(case, "paths"):
        with case.subTest(seed=seed):
            document, _level, _column = case.populated_scene()
            parts = [_random_text(rng, SAFE_CHARACTERS, 24).strip() or "x" for _ in range(3)]
            target = case.workspace.joinpath(f"{seed}", *parts)
            target.parent.mkdir(parents=True)
            saved = case.mutate("scene.save_copy", document, directory=str(target))
            reported = saved["package_directory"]
            case.assertEqual(_composed(reported), _composed(str(target)))
            case.assertTrue(os.path.samefile(reported, target), reported)
            case.assertTrue((target / "scene.tnz").is_file())
            reopened = case.mutate("scene.open", None, path=reported)
            case.assertEqual(reopened["package_directory"], reported)
            summary = case.success("scene.summary", {"document_id": reopened["document_id"]})
            case.assertEqual([item["name"] for item in summary["levels"]], ["Quadrants"])
            case.assertEqual(summary["frame_count"], 25)
            case.mutate("scene.discard", reopened["document_id"])
            case.mutate("scene.discard", document)


@acceptance_case
def drawing_numbers_reach_the_limit_of_their_format_and_no_further(case: NativeAcceptance) -> None:
    """Vector drawings stop at 65535, raster drawings at 999999; both limits save and reopen."""
    document = case.create()
    vector = case.mutate("level.create", document, name="Vector", format="vector")["level_id"]
    raster = case.mutate("level.create", document, name="Raster", format="rgba8")["level_id"]
    limits = {"vector": (vector, VECTOR_LARGEST), "rgba8": (raster, RASTER_LARGEST)}
    size = {"width": 2, "height": 2, "dpi": 120}
    for image_format, (level, largest) in limits.items():
        extra = {} if image_format == "vector" else size
        for number, suffix in ((1, ""), (largest, "abcdefgh")):
            drawing = {"level_id": level, "drawing_id": {"number": number, "suffix": suffix}}
            case.mutate("drawing.create", document, **drawing, format=image_format, **extra)
    before = case.success("scene.summary", {"document_id": document})
    refused = {"level_id": vector, "drawing_id": {"number": VECTOR_LARGEST + 1, "suffix": ""}}
    error = case.failure(
        "drawing.create", case.write_params(document, {**refused, "format": "vector"})
    )
    case.assertEqual(error["code"], "unsupported_drawing_id", error)
    case.assertEqual(case.success("scene.summary", {"document_id": document}), before)
    over = {"level_id": raster, "drawing_id": {"number": RASTER_LARGEST + 1, "suffix": ""}}
    case.schema_failure("drawing.create", case.write_params(document, {**over, "format": "rgba8"}))
    reopened = _reopened(case, document, "limits")
    levels = {
        item["name"]: _ids(item["drawings"])
        for item in case.success("scene.summary", {"document_id": reopened})["levels"]
    }
    case.assertEqual(levels["Vector"], [(1, ""), (VECTOR_LARGEST, "abcdefgh")])
    case.assertEqual(levels["Raster"], [(1, ""), (RASTER_LARGEST, "abcdefgh")])


NAME_TOOLS = {
    "level.create": {"format": "vector"},
    "column.create": {},
    "object.create": {"kind": "pegbar"},
}


@acceptance_case
def names_beyond_the_length_limit_are_refused_by_the_schema(case: NativeAcceptance) -> None:
    """Name a level, column and peg with 128 characters, then with 129 and with NUL."""
    document = case.create()
    for tool, extra in NAME_TOOLS.items():
        with case.subTest(tool=tool):
            case.mutate(tool, document, name="n" * NAME_LIMIT, **extra)
            case.mutate(tool, document, name="\U0001f680" * 100 + "é" * 28, **extra)
            for name in ("m" * (NAME_LIMIT + 1), "\U0001f680" * 129, "a\x00b"):
                request = case.write_params(document, {"name": name, **extra})
                case.schema_failure(tool, request)


def _example_string(schema: dict[str, Any]) -> str:
    """The first sample string that its schema's pattern accepts whole."""
    if "pattern" not in schema:
        return "ab"
    for candidate in ("a", "0", "fxgroup_00000000-0000-0000-0000-000000000000"):
        if re.fullmatch(schema["pattern"], candidate):
            return candidate
    return "a"


def _example(schema: dict[str, Any]) -> Any:
    """A value that satisfies the part of a schema these tools use."""
    kind = schema.get("type")
    if "const" in schema:
        return schema["const"]
    if "enum" in schema:
        return schema["enum"][0]
    if "oneOf" in schema:
        return _example(schema["oneOf"][0])
    if kind == "object":
        required = schema.get("required", [])
        return {key: _example(schema["properties"][key]) for key in required}
    if kind == "array":
        return [_example(schema["items"])] * schema.get("minItems", 0)
    if kind == "string":
        return _example_string(schema)
    if kind in {"integer", "number"}:
        return schema.get("minimum", 1)
    return False


def _name_properties(schema: Any, path: str = "") -> list[tuple[str, dict[str, Any]]]:
    found: list[tuple[str, dict[str, Any]]] = []
    if not isinstance(schema, dict):
        return found
    for key, value in schema.get("properties", {}).items():
        named = key == "name" or key.endswith("_name") or key == "name_contains"
        if named and value.get("type") == "string":
            found.append((f"{path}{key}", value))
        found.extend(_name_properties(value, f"{path}{key}."))
    found.extend(_name_properties(schema.get("items"), f"{path}[]."))
    return found


@acceptance_case
def every_name_in_every_tool_schema_states_its_limit_and_refuses_nul(
    case: NativeAcceptance,
) -> None:
    """Find each name-taking tool in the catalog; its schema bounds the name and bars NUL.

    A name is a string property called name, name_contains or ending in _name. A new tool that
    takes one fails here until its schema gives a maximum length and a pattern without NUL (and
    the dispatcher refuses a longer or NUL name before the handler, as invalid_params).
    """
    tools = {tool["name"]: tool for tool in case.capabilities()["tools"]}
    named = {
        name: found
        for name, tool in tools.items()
        if (found := _name_properties(tool["inputSchema"]))
    }
    case.assertGreaterEqual(len(named), 20, "the catalog walk found too few name-taking tools")
    document = case.create()
    for tool, properties in named.items():
        for path, schema in properties:
            with case.subTest(tool=tool, name=path):
                limit = schema.get("maxLength")
                case.assertIsInstance(limit, int, f"{tool} {path} states no maxLength")
                assert isinstance(limit, int)
                case.assertIn("pattern", schema, f"{tool} {path} states no character rule")
                case.assertIsNone(re.fullmatch(schema["pattern"], "a\x00b"), "NUL is allowed")
                case.assertIsNotNone(re.fullmatch(schema["pattern"], "a b\u00e9"))
                if "." in path:
                    continue
                _refused_name(case, tools[tool], path, document, ["x" * (limit + 1), "a\x00b"])


def _refused_name(
    case: NativeAcceptance, tool: dict[str, Any], path: str, document: str, names: list[str]
) -> None:
    for name in names:
        params = _example(tool["inputSchema"])
        params[path] = name
        if "document_id" in params:
            params["document_id"] = document
        if "expected_revision" in params:
            params["expected_revision"] = case.revisions[document]
        error = case.failure(tool["name"], params)
        case.assertEqual(error["code"], case.schema_code(), (tool["name"], path, error))
        text = error["message"] + json.dumps(error["details"])
        case.assertIn(path, text, (tool["name"], error))


@acceptance_case
def camera_size_in_inches_keeps_its_value_through_save_and_reopen(case: NativeAcceptance) -> None:
    """Create a camera 32 pixels across at 120 pixels per inch; the reopened scene reads it back."""
    exact = 32 / 120
    document = case.mutate(
        "scene.create",
        None,
        name="Precision",
        width_px=32,
        height_px=32,
        width_in=exact,
        height_in=exact,
        frame_rate=24,
    )["document_id"]
    case.mutate("column.create", document, name="Held")
    reopened = _held_scene(case, document)
    read = case.success("camera.inspect", {"document_id": reopened})
    drift = abs(read["width_in"] - exact)
    observed = "exact" if drift < TOLERANCE else f"{read['width_in']:.6g}"
    case.assertEqual(observed, "exact")
    case.assertEqual((read["width_px"], read["height_px"]), (32, 32))


def _held_scene(case: NativeAcceptance, document: str) -> str:
    """Give the scene's column a cell so it is saved, and return the reopened scene."""
    level = case.mutate("level.create", document, name="Held", format="vector")["level_id"]
    drawing = {"number": 1, "suffix": ""}
    case.mutate("drawing.create", document, level_id=level, drawing_id=drawing, format="vector")
    column = case.success("scene.summary", {"document_id": document})["columns"][0]["column_id"]
    case.mutate(
        "column.expose",
        document,
        column_id=column,
        level_id=level,
        drawing_id=drawing,
        start_frame=0,
        length=2,
    )
    return _reopened(case, document, "held")


@acceptance_case
def the_frame_count_follows_a_deleted_column(case: NativeAcceptance) -> None:
    """Delete the only column that holds cells; the scene is then empty."""
    document = case.create()
    level = case.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    drawing = {"number": 1, "suffix": ""}
    case.mutate("drawing.create", document, level_id=level, drawing_id=drawing, format="vector")
    column = case.mutate("column.create", document, name="Rows")["column_id"]
    case.mutate(
        "column.expose",
        document,
        column_id=column,
        level_id=level,
        drawing_id=drawing,
        start_frame=0,
        length=22,
    )
    case.assertEqual(case.success("scene.summary", {"document_id": document})["frame_count"], 22)
    case.mutate("column.delete", document, column_id=column)
    summary = case.success("scene.summary", {"document_id": document})
    case.assertEqual(summary["columns"], [])
    case.assertEqual(summary["frame_count"], 0)


@acceptance_case
def an_exposure_at_the_last_row_can_be_saved_with_one_at_the_first(case: NativeAcceptance) -> None:
    """Expose rows 0 and 999999, the first and the last the tools admit, then save the scene."""
    document = case.create()
    level = case.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    drawing = {"number": 1, "suffix": ""}
    case.mutate("drawing.create", document, level_id=level, drawing_id=drawing, format="vector")
    column = case.mutate("column.create", document, name="Rows")["column_id"]
    for frame in (0, MAX_SAVED_ROW):
        case.mutate(
            "column.expose",
            document,
            column_id=column,
            level_id=level,
            drawing_id=drawing,
            start_frame=frame,
            length=1,
        )
    request = case.write_params(document, {"directory": str(case.workspace / "last-row")})
    response = _raw(case, "scene.save_copy", request)
    observed = response.get("error", {}).get("code", "saved")
    case.assertEqual(observed, "saved")


@acceptance_case
def trailing_empty_columns_are_not_saved_and_inner_ones_are(case: NativeAcceptance) -> None:
    """A save drops a column without cells at the end of the sheet and keeps one before a cell."""
    document = case.create()
    level = case.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    drawing = {"number": 1, "suffix": ""}
    case.mutate("drawing.create", document, level_id=level, drawing_id=drawing, format="vector")
    names = ["Empty first", "Held", "Empty last"]
    columns = [case.mutate("column.create", document, name=name)["column_id"] for name in names]
    case.mutate("column.update", document, column_id=columns[0], opacity=0.5, locked=True)
    case.mutate(
        "column.expose",
        document,
        column_id=columns[1],
        level_id=level,
        drawing_id=drawing,
        start_frame=2,
        length=1,
    )
    reopened = _reopened(case, document, "trailing")
    saved = case.success("scene.summary", {"document_id": reopened})["columns"]
    case.assertEqual([column["name"] for column in saved], names[:2])
    case.assertEqual((saved[0]["locked"], saved[0]["opacity"]), (True, 128 / 255))


@acceptance_case
def a_speed_segment_follows_its_bezier_whatever_its_handles(case: NativeAcceptance) -> None:
    """Sample speed segments with handles of every length against the Bezier, by hand.

    The last two rows have a frame axis that is a quadratic in the Bezier parameter (three
    times the outgoing length minus the incoming length is twice the span).
    """
    document = case.create()
    peg = case.mutate("object.create", document, name="Peg", kind="pegbar")["object_id"]
    frames = [6, 7, 8, 9, 10, 11, 11.5, 11.9, 12]
    # Handles as long as the span make the frame axis stationary at frame 9: every parameter
    # near 0.5 reaches it, so the editor's closed form and the model's bisection pick values of
    # the parameter 4e-6 apart and the curve differs by about 3e-4 there.
    for outgoing, incoming, tolerance in (
        (4, -1, 1e-5),
        (0, -3, 1e-5),
        (6, -6, 1e-3),
        (4, -0.001, 1e-5),
        (4, 0, 1e-5),
        (0, -4, 1e-5),
    ):
        target = {"object_id": peg, "channel": "x", "unit": "inch"}
        keys = [
            Key(6, -70.5, "speed_in_out", 1, ("none",), ("speed", outgoing, 32.75)),
            Key(12, -5.75, "linear", 1, ("speed", incoming, 34.5), None, False),
        ]
        case.mutate("animation.set_keys", document, **target, keys=[key_record(k) for k in keys])
        got = case.success(
            "animation.sample", {"document_id": document, **target, "frames": frames}
        )
        exact = [sample(keys, frame) for frame in frames]
        close = all(
            abs(a["value"] - b) < tolerance for a, b in zip(got["values"], exact, strict=True)
        )
        case.assertTrue(close, (outgoing, incoming))
        case.mutate("animation.delete_keys", document, **target, frames=[6, 12])
