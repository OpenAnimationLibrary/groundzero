"""Exercise the shipped native worker with real scenes, assets, and rendered pixels."""

from __future__ import annotations

import argparse
import base64
import functools
import hashlib
import importlib
import inspect
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any

import mcp_declaration_cases
import mcp_file_project_cases
import mcp_movie_eof
import mcp_renumber_cases
import mcp_revision_cases
import mcp_shards
from mcp_case import MARK
from mcp_png import decode_png, fixture_pixel, write_fixture
from mcp_worker import FULL_AUTHORITY, Worker, joined_capabilities
from run_harness_tests import Tee, annotation

REQUIRED_TOOLS = {
    "automation.capabilities",
    "automation.list_documents",
    "scene.create",
    "scene.summary",
    "scene.settings",
    "scene.save_copy",
    "scene.open",
    "scene.discard",
    "level.import",
    "column.create",
    "column.expose",
    "animation.set_keys",
    "animation.sample",
    "camera.inspect",
    "camera.update",
    "render.preview",
    "render.submit",
    "job.status",
    "job.cancel",
    "job.result",
    "job.list",
    "scene.check.submit",
    "artifact.read",
}
SDK_CLIENT = "native-client.mjs"
SCHEMA_REJECTION = "INVALID_ARGUMENTS"
CaseMethod = Callable[["NativeAcceptance"], None]


def worker_only(reason: str) -> Callable[[CaseMethod], CaseMethod]:
    """Mark a case that qualifies the native worker process itself rather than its tools.

    No MCP client can express such a case, so through the SDK client it is skipped and the
    skip records the reason.
    """

    def mark(test: CaseMethod) -> CaseMethod:
        @functools.wraps(test)
        def run(case: NativeAcceptance) -> None:
            if case.through_sdk:
                case.skipTest(f"worker-only: {reason}")
            test(case)

        return run

    return mark


class NativeAcceptance(unittest.TestCase):
    """Qualify each automation milestone against the native executable."""

    executable: Path
    output: Path
    media_tools: Path | None = None

    def test_vector_renumber_preserves_exposures_and_native_roundtrip(self) -> None:
        mcp_renumber_cases.check_renumber_roundtrip(self, "vector")

    def test_rgba_renumber_preserves_exposures_and_native_roundtrip(self) -> None:
        mcp_renumber_cases.check_renumber_roundtrip(self, "rgba8")

    def test_toonz_renumber_preserves_exposures_and_native_roundtrip(self) -> None:
        mcp_renumber_cases.check_renumber_roundtrip(self, "toonzRaster")

    def test_movie_eof_interrupts_encoder_finalization(self) -> None:
        mcp_movie_eof.movie_eof_interrupts_encoder_finalization(self)

    @worker_only("needs the --test-path-swap test hook, which the adapter never passes")
    def test_project_listings_stay_in_the_folder_they_pinned(self) -> None:
        mcp_file_project_cases.project_list_stays_in_the_folder_it_pinned(self)
        mcp_file_project_cases.project_scenes_stays_in_the_folder_it_pinned(self)

    def setUp(self) -> None:
        self.directory = Path(tempfile.mkdtemp(prefix=self._testMethodName + "-", dir=self.output))
        self.workspace = self.directory / "workspace"
        self.inputs = self.directory / "inputs"
        self.workspace.mkdir()
        self.inputs.mkdir()
        self.fixture = self.inputs / "quadrants.png"
        write_fixture(self.fixture)
        self.worker = Worker(
            self.executable, self.workspace, self.inputs, self.directory / "worker.stderr.log"
        )
        self.addCleanup(self.worker.close)
        self.revisions: dict[str, str] = {}
        self.operations = 0

    def success(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """Require a successful, object-shaped domain response."""
        response = self.worker.call(method, params)
        self.assertNotIn("error", response, f"{method}: {response}")
        result = response["result"]
        self.assertIsInstance(result, dict)
        return result

    def capabilities(self) -> dict[str, Any]:
        """Return automation.capabilities with every page of tool descriptors joined."""
        return joined_capabilities(lambda params: self.success("automation.capabilities", params))

    def failure(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """Require a structured actionable failure, never process termination."""
        response = self.worker.call(method, params)
        self.assertNotIn("result", response, f"{method} unexpectedly succeeded: {response}")
        error = response["error"]
        self.assertIsInstance(error.get("code"), str)
        self.assertTrue(error.get("message"), response)
        self.assertIsInstance(error.get("details"), dict)
        return error

    @property
    def through_sdk(self) -> bool:
        """Whether each worker is the MCP SDK client fixture driving the adapter."""
        return self.executable.name == SDK_CLIENT

    @property
    def native_executable(self) -> Path:
        """The OpenToonz executable that serves every call, directly or behind the adapter."""
        if not self.through_sdk:
            return self.executable
        return Path(os.environ["GROUNDZERO_NATIVE_EXECUTABLE"])

    def schema_code(self, native_code: str = "invalid_params") -> str:
        """Return the code for arguments outside a tool's advertised input schema.

        The adapter validates every call against the advertised schema and never forwards a
        violation, so through the SDK client the code is SCHEMA_REJECTION; the raw worker
        reports the violation with its own native_code.
        """
        return SCHEMA_REJECTION if self.through_sdk else native_code

    def schema_failure(
        self,
        method: str,
        params: dict[str, Any],
        native_code: str = "invalid_params",
        worker: Worker | None = None,
    ) -> dict[str, Any]:
        """Require the schema rejection of the client in use and an unchanged document.

        Args:
            method: The tool to call.
            params: Arguments that violate the tool's advertised input schema.
            native_code: The code the raw worker reports for this violation.
            worker: The worker to call instead of the case's own.
        Returns:
            The structured error.
        """
        worker = worker or self.worker
        document = params.get("document_id")
        tracked = isinstance(document, str) and document in self.revisions
        before = self._summary(worker, document) if tracked else None
        response = worker.call(method, params)
        self.assertNotIn("result", response, f"{method} unexpectedly succeeded: {response}")
        error = response["error"]
        self.assertEqual(error["code"], self.schema_code(native_code), error)
        self.assertTrue(error["message"], error)
        self.assertIsInstance(error["details"], dict)
        if self.through_sdk:
            self.assertTrue(error["details"]["issues"], error)
        if before is not None:
            self.assertEqual(self._summary(worker, document), before)
        return error

    def handler_failure(self, method: str, params: dict[str, Any], code: str) -> dict[str, Any]:
        """Require a refusal by the tool's own checks, with its own code, and no change.

        Unlike a schema violation, the same code comes back through the worker and the SDK
        client, since the advertised schema admits the arguments.
        """
        document = params["document_id"]
        before = self._summary(self.worker, document)
        error = self.failure(method, params)
        self.assertEqual(error["code"], code, error)
        self.assertEqual(self._summary(self.worker, document), before)
        return error

    def _summary(self, worker: Worker, document: Any) -> dict[str, Any]:
        response = worker.call("scene.summary", {"document_id": document})
        self.assertNotIn("error", response, response)
        return response["result"]

    def launch_refused(self, grants: list[str], name: str) -> str:
        """Require a launch with invalid grants to stop before the protocol starts.

        The raw worker reports `automation-worker: invalid_argument` and exits 2. Through the
        SDK client the adapter refuses the same grants itself and the client exits 1.

        Args:
            grants: Arguments after the workspace grant.
            name: Distinct name for the retained diagnostics.
        Returns:
            The diagnostics, for checks on how the refusal names its argument.
        """
        workspace = ["--workspace", str(self.workspace)]
        result = subprocess.run(
            [str(self.executable), "--automation-worker", *workspace, *grants, *FULL_AUTHORITY],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=60,
            check=False,
        )
        (self.directory / f"{name}.stderr.log").write_bytes(result.stderr)
        diagnostics = result.stderr.decode("utf-8", errors="replace")
        code, marker = (
            (1, "Groundzero MCP startup failed:")
            if self.through_sdk
            else (2, "automation-worker: invalid_argument:")
        )
        self.assertEqual(result.returncode, code, (grants, diagnostics))
        self.assertEqual(result.stdout, b"", "A refused launch must not start the protocol")
        self.assertIn(marker, diagnostics)
        return diagnostics

    def write_params(self, document: str | None, params: dict[str, Any]) -> dict[str, Any]:
        """Attach the current revision and a unique mutation receipt identifier."""
        self.operations += 1
        result = {**params, "operation_id": f"operation-{self.operations}"}
        if document is not None:
            result.update(document_id=document, expected_revision=self.revisions[document])
        return result

    def mutate(self, method: str, document: str | None, **params: Any) -> dict[str, Any]:
        """Issue a mutation and retain the revision returned by the real host."""
        result = self.success(method, self.write_params(document, params))
        if "document_id" in result:
            self.assertIsInstance(result["revision"], str)
            self.revisions[result["document_id"]] = result["revision"]
        return result

    def create(self, name: str = "Acceptance") -> str:
        """Create a small scene with one source pixel per rendered camera pixel."""
        result = self.mutate(
            "scene.create",
            None,
            name=name,
            width_px=32,
            height_px=32,
            width_in=32 / 120,
            height_in=32 / 120,
            frame_rate=24,
        )
        return result["document_id"]

    def populated_scene(self) -> tuple[str, dict[str, Any], dict[str, Any]]:
        """Import and expose an actual PNG in a real Xsheet column."""
        document = self.create()
        level = self.mutate(
            "level.import", document, path=str(self.fixture), name="Quadrants", dpi=120
        )
        column = self.mutate("column.create", document, name="Artwork")
        self.mutate(
            "column.expose",
            document,
            column_id=column["column_id"],
            level_id=level["level_id"],
            drawing_id={"number": 1, "suffix": ""},
            start_frame=0,
            length=25,
        )
        return document, level, column

    def wait_job(self, job: str) -> dict[str, Any]:
        """Wait for an explicit terminal job state within a fixed CI deadline."""
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            result = self.success("job.status", {"job_id": job})
            if result["status"] in {"completed", "failed", "canceled", "deadline_exceeded"}:
                return result
            self.assertIn(
                result["status"],
                {
                    "queued",
                    "preparing",
                    "running",
                    "cancellation_requested",
                    "finalizing",
                },
            )
            time.sleep(0.05)
        self.fail(f"Render job {job} did not reach a terminal state within 90 seconds")

    def preview(self, document: str) -> bytes:
        """Render and retrieve a verified artifact through its opaque identifier."""
        revision = self.revisions[document]
        job = self.mutate("render.preview", document, frame=0, width=32, height=32)
        self.assertEqual(job["source_revision"], revision)
        self.assertEqual(self.revisions[document], revision)
        status = self.wait_job(job["job_id"])
        self.assertEqual(status["status"], "completed", status)
        artifacts = self.success("job.result", {"job_id": job["job_id"]})["artifacts"]
        self.assertEqual(len(artifacts), 1, artifacts)
        artifact = artifacts[0]
        result = self.success("artifact.read", {"artifact_id": artifact["artifact_id"]})
        self.assertEqual(result["image"]["mimeType"], "image/png")
        data = base64.b64decode(result["image"]["data"], validate=True)
        self.assertEqual(hashlib.sha256(data).hexdigest(), artifact["sha256"])
        self.assertEqual(len(data), artifact["size_bytes"])
        self.assertEqual(result["source_revision"], revision)
        return data

    def test_revision_matrix_animation(self) -> None:
        mcp_revision_cases.check_group(self, "animation")

    def test_revision_matrix_drawing(self) -> None:
        mcp_revision_cases.check_group(self, "drawing")

    def test_revision_matrix_fx(self) -> None:
        mcp_revision_cases.check_group(self, "fx")

    def test_revision_matrix_lifecycle(self) -> None:
        mcp_revision_cases.check_group(self, "lifecycle")

    def test_revision_matrix_production(self) -> None:
        mcp_revision_cases.check_group(self, "production")

    def test_revision_matrix_render(self) -> None:
        mcp_revision_cases.check_group(self, "render")

    def test_revision_matrix_rigging(self) -> None:
        mcp_revision_cases.check_group(self, "rigging")

    def test_revision_matrix_scene(self) -> None:
        mcp_revision_cases.check_group(self, "scene")

    def test_capabilities_are_strict_and_implemented(self) -> None:
        capabilities = self.capabilities()
        self.assertEqual(capabilities["protocol_version"], 5)
        tools = capabilities["tools"]
        names = [tool["name"] for tool in tools]
        self.assertEqual(len(names), len(set(names)), "Duplicate advertised tool names")
        self.assertTrue(REQUIRED_TOOLS.issubset(names), REQUIRED_TOOLS.difference(names))
        listed = [name for toolset in capabilities["toolsets"] for name in toolset["tools"]]
        self.assertEqual(sorted(listed), sorted(names), "Toolsets must partition the catalog")
        described = {
            toolset["name"] for toolset in capabilities["toolsets"] if toolset["description"]
        }
        for tool in tools:
            with self.subTest(tool=tool["name"]):
                self.assertTrue(tool["description"])
                self.assertIn(tool["toolset"], described)
                schema = tool["inputSchema"]
                self.assertEqual(schema["type"], "object")
                self.assertIs(schema["additionalProperties"], False)
                readonly = tool["annotations"]["readOnlyHint"]
                self.assertIsInstance(readonly, bool)
                if not readonly:
                    self.assertIn("operation_id", schema["required"])
                    if "document_id" in schema["properties"]:
                        self.assertIn("expected_revision", schema["required"])
                self.failure(tool["name"], {"unrecognized_acceptance_argument": True})
        mcp_declaration_cases.descriptors_publish_declarations(self, tools)

    @worker_only("runs the native domain tests, which only the worker's --test-domains flag starts")
    def test_native_domain_regressions(self) -> None:
        domain_workspace = self.directory / "domain-workspace"
        domain_workspace.mkdir()
        with (self.directory / "native-domain-tests.log").open("wb") as log:
            result = subprocess.run(
                [
                    str(self.executable),
                    "--automation-worker",
                    "--workspace",
                    str(domain_workspace),
                    "--input-root",
                    str(self.inputs),
                    *FULL_AUTHORITY,
                    "--test-domains",
                ],
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=300,
                check=False,
            )
        diagnostics = (self.directory / "native-domain-tests.log").read_bytes()[-4096:]
        self.assertEqual(result.returncode, 0, diagnostics.decode("utf-8", errors="replace"))
        reports = json.loads((domain_workspace / "media-verifier-report.json").read_text())
        self.assertTrue(reports, "Native movie verifier cases did not run")
        self.assertIn("production-deadline", {report["case"] for report in reports})
        for report in reports:
            with self.subTest(native_case=report["case"]):
                self.assertEqual(report["status"], "passed", report)

    def test_vector_edits_validate_the_whole_batch(self) -> None:
        document = self.create()
        level = self.mutate("level.create", document, name="Vector", format="vector")
        target = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": "a"}}
        self.mutate("drawing.create", document, **target, format="vector")
        points = [{"x": x, "y": 0, "thickness": 1} for x in (-10, 0, 10)]
        edit = {"action": "add", "style_id": 1, "closed": False, "points": points}
        edits = {**target, "coordinate_space": "drawing_native", "sharing": "all_exposures"}
        self.mutate("vector.edit", document, **edits, edits=[edit])
        inspected = self.success("vector.inspect", {"document_id": document, **target})
        self.assertEqual(inspected["total"], 1)
        self.assertEqual(inspected["strokes"][0]["points"], points)
        revision = self.revisions[document]
        self.failure(
            "vector.edit",
            self.write_params(
                document,
                {
                    **edits,
                    "edits": [edit, {"action": "delete", "index": 99}],
                },
            ),
        )
        self.assertEqual(
            self.success("vector.inspect", {"document_id": document, **target}), inspected
        )
        self.assertEqual(
            self.success("scene.summary", {"document_id": document})["revision"], revision
        )
        points[1]["y"] = 8
        self.mutate(
            "vector.edit",
            document,
            **edits,
            edits=[{**edit, "action": "update", "index": 0, "points": points}],
        )
        updated = self.success("vector.inspect", {"document_id": document, **target})
        self.assertEqual(updated["strokes"][0]["points"], points)
        self.mutate("vector.edit", document, **edits, edits=[{"action": "delete", "index": 0}])
        self.assertEqual(
            self.success("vector.inspect", {"document_id": document, **target})["total"], 0
        )

    def test_palette_updates_preserve_other_animation_keys(self) -> None:
        document = self.create()
        level = self.mutate("level.create", document, name="Palette", format="vector")
        target = {"level_id": level["level_id"]}
        red = {"r": 255, "g": 0, "b": 0, "a": 255}
        blue = {"r": 0, "g": 0, "b": 255, "a": 255}
        green = {"r": 0, "g": 255, "b": 0, "a": 255}
        style = self.mutate("palette.create_style", document, **target, name="Animated", color=red)
        target["style_id"] = style["style_id"]
        self.mutate("palette.update_style", document, **target, color=red, keyframe=0)
        before = self.mutate("palette.update_style", document, **target, color=blue, keyframe=24)
        after = self.mutate("palette.update_style", document, **target, color=green, keyframe=0)
        self.assertEqual(after["keyframes"], [0, 24])
        self.assertEqual(after["animation"][0]["color"], green)
        self.assertEqual(after["animation"][1], before["animation"][1])
        self.failure("palette.update_style", self.write_params(document, {**target, "color": red}))
        palette = self.success(
            "palette.inspect",
            {
                "document_id": document,
                "level_id": level["level_id"],
            },
        )
        current = next(item for item in palette["styles"] if item["style_id"] == style["style_id"])
        self.assertEqual(current["animation"], after["animation"])

    def test_vector_fill_and_quadratic_geometry_validation(self) -> None:
        document = self.create()
        level = self.mutate("level.create", document, name="Closed", format="vector")
        target = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": ""}}
        self.mutate("drawing.create", document, **target, format="vector")
        edits = {**target, "coordinate_space": "drawing_native", "sharing": "all_exposures"}
        corners = ((0, 0), (5, 0), (10, 0), (10, 5), (10, 10), (5, 10), (0, 10), (0, 5), (0, 0))
        points = [{"x": x, "y": y, "thickness": 0.5} for x, y in corners]
        stroke = {"action": "add", "style_id": 1, "closed": True, "points": points}
        self.failure(
            "vector.edit",
            self.write_params(
                document,
                {
                    **edits,
                    "edits": [{**stroke, "points": points[:-1]}],
                },
            ),
        )
        self.failure(
            "vector.edit",
            self.write_params(
                document,
                {
                    **edits,
                    "sharing": "unacknowledged",
                    "edits": [stroke],
                },
            ),
        )
        self.mutate("vector.edit", document, **edits, edits=[stroke])
        filled = self.mutate("vector.fill", document, **edits, x=5, y=5, style_id=1)
        self.assertEqual(filled["style_id"], 1)
        self.assertGreaterEqual(filled["previous_style_id"], 0)
        self.failure(
            "vector.fill", self.write_params(document, {**edits, "x": 50, "y": 50, "style_id": 1})
        )

    def test_raster_png_roundtrip_and_source_precision_admission(self) -> None:
        document, level, _column = self.populated_scene()
        target = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": ""}}
        exported = self.mutate(
            "raster.export_png", document, **target, path=str(self.workspace / "exported.png")
        )
        self.assertEqual(
            decode_png(Path(exported["path"]).read_bytes()), decode_png(self.fixture.read_bytes())
        )
        copied = self.inputs / "roundtrip.png"
        shutil.copyfile(exported["path"], copied)
        destination = {"level_id": level["level_id"], "drawing_id": {"number": 2, "suffix": "a"}}
        self.mutate("raster.import_png", document, **destination, path=str(copied), dpi=120)
        rectangle = {
            "coordinate_space": "raster_pixels_lower_left",
            "x": 0,
            "y": 0,
            "width": 32,
            "height": 32,
        }
        self.assertEqual(
            self.success(
                "raster.read_pixels",
                {
                    "document_id": document,
                    **destination,
                    **rectangle,
                },
            ),
            self.success("raster.read_pixels", {"document_id": document, **target, **rectangle}),
        )
        precise = self.inputs / "sixteen-bit.png"
        write_fixture(precise, depth=16)
        self.failure(
            "raster.import_png",
            self.write_params(
                document,
                {
                    **destination,
                    "drawing_id": {"number": 3, "suffix": ""},
                    "path": str(precise),
                    "dpi": 120,
                },
            ),
        )
        self.failure(
            "level.import",
            self.write_params(
                document,
                {
                    "name": "Sixteen bit",
                    "path": str(precise),
                    "dpi": 120,
                },
            ),
        )

    def test_glow_without_light_preserves_source_pixels(self) -> None:
        document, _level, _column = self.populated_scene()
        baseline = decode_png(self.preview(document))
        graph = self.success("fx.graph", {"document_id": document})
        glow = self.mutate("fx.create", document, type="STD_glowFx")
        self.mutate(
            "fx.connect",
            document,
            fx_id=glow["fx_id"],
            port="Source",
            source_fx_id=graph["xsheet_fx_id"],
        )
        self.mutate(
            "fx.connect",
            document,
            fx_id=graph["current_output_fx_id"],
            port="source",
            source_fx_id=glow["fx_id"],
        )
        graph = self.success("fx.graph", {"document_id": document})
        node = next(effect for effect in graph["effects"] if effect["fx_id"] == glow["fx_id"])
        self.assertIn("GLOW_WITHOUT_LIGHT", [item["code"] for item in node["diagnostics"]])
        self.assertEqual(decode_png(self.preview(document)), baseline)

    def test_native_raster_pixels_and_atomic_patch_rejection(self) -> None:
        for image_format in ("rgba8", "toonzRaster"):
            with self.subTest(format=image_format):
                document = self.create(image_format)
                level = self.mutate(
                    "level.create", document, name=image_format, format=image_format
                )
                target = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": ""}}
                self.mutate(
                    "drawing.create",
                    document,
                    **target,
                    format=image_format,
                    width=2,
                    height=2,
                    dpi=120,
                )
                rectangle = {
                    **target,
                    "coordinate_space": "raster_pixels_lower_left",
                    "x": 0,
                    "y": 0,
                    "width": 2,
                    "height": 1,
                }
                if image_format == "rgba8":
                    pixels = [
                        {"r": 64, "g": 32, "b": 0, "a": 128},
                        {"r": 0, "g": 0, "b": 0, "a": 0},
                    ]
                    invalid = {"r": 255, "g": 0, "b": 0, "a": 1}
                else:
                    pixels = [
                        {"ink": 1, "paint": 0, "tone": 20},
                        {"ink": 0, "paint": 1, "tone": 255},
                    ]
                    invalid = {"ink": 4095, "paint": 0, "tone": 0}
                self.mutate(
                    "raster.patch", document, **rectangle, sharing="all_exposures", pixels=pixels
                )
                observed = self.success(
                    "raster.read_pixels", {"document_id": document, **rectangle}
                )
                self.assertEqual(observed["pixels"], pixels)
                self.assertEqual(observed["row_order"], "bottom_to_top")
                self.failure(
                    "raster.patch",
                    self.write_params(
                        document,
                        {
                            **rectangle,
                            "sharing": "all_exposures",
                            "pixels": [pixels[1], invalid],
                        },
                    ),
                )
                self.assertEqual(
                    self.success(
                        "raster.read_pixels",
                        {
                            "document_id": document,
                            **rectangle,
                        },
                    ),
                    observed,
                )

    def test_held_drawing_clone_is_independent_and_preserves_suffix(self) -> None:
        document, level, _column = self.populated_scene()
        target = {"level_id": level["level_id"], "drawing_id": {"number": 1, "suffix": ""}}
        self.failure("drawing.remove", self.write_params(document, target))
        cloned = self.mutate(
            "drawing.clone", document, **target, destination_drawing_id={"number": 1, "suffix": "b"}
        )
        self.assertEqual(cloned["drawing_id"], {"number": 1, "suffix": "b"})
        rectangle = {
            **target,
            "coordinate_space": "raster_pixels_lower_left",
            "x": 0,
            "y": 0,
            "width": 1,
            "height": 1,
        }
        original = self.success("raster.read_pixels", {"document_id": document, **rectangle})
        destination = {**rectangle, "drawing_id": cloned["drawing_id"]}
        self.mutate(
            "raster.patch",
            document,
            **destination,
            sharing="all_exposures",
            pixels=[{"r": 255, "g": 0, "b": 255, "a": 255}],
        )
        self.assertEqual(
            self.success("raster.read_pixels", {"document_id": document, **rectangle}), original
        )
        self.mutate(
            "drawing.remove", document, level_id=level["level_id"], drawing_id=cloned["drawing_id"]
        )
        self.failure(
            "drawing.inspect",
            {
                "document_id": document,
                "level_id": level["level_id"],
                "drawing_id": cloned["drawing_id"],
            },
        )

    @worker_only("writes malformed and oversize frames to the worker's own NDJSON protocol")
    def test_protocol_rejects_malformed_and_oversize_requests(self) -> None:
        self.assertEqual(self.worker.exchange(b"{malformed json")["error"]["code"], "parse_error")
        invalid_utf8 = b'{"id":"invalid-\xff","method":"automation.capabilities","params":{}}'
        self.assertIn("error", self.worker.exchange(invalid_utf8))
        concatenated = b'{"id":"first","method":"automation.capabilities","params":{}}{}'
        self.assertIn("error", self.worker.exchange(concatenated))
        missing_id = self.worker.exchange(b'{"method":"automation.capabilities","params":{}}')
        self.assertIn("error", missing_id)
        self.assertIsNone(missing_id["id"])
        self.assertEqual(self.success("automation.capabilities", {})["protocol_version"], 5)
        self.assertIn(
            "error",
            self.worker.exchange(
                json.dumps(
                    {
                        "id": "extra",
                        "method": "automation.capabilities",
                        "params": {},
                        "extra": 1,
                    }
                ).encode()
            ),
        )
        self.failure("acceptance.nonexistent", {})
        self.failure("scene.create", {"name": "Missing receipt"})
        self.failure(
            "scene.create",
            self.write_params(
                None,
                {
                    "name": "Unknown argument",
                    "unrecognized_acceptance_argument": True,
                },
            ),
        )
        self.failure("scene.create", self.write_params(None, {"name": "Bad width", "width_px": -1}))
        self.assertEqual(self.success("automation.list_documents", {})["documents"], [])
        oversized = b'{"id":"oversized","method":"automation.capabilities","params":{"pad":"'
        oversized += b"x" * (8 * 1024 * 1024) + b'"}}'
        self.assertEqual(self.worker.exchange(oversized)["error"]["code"], "request_too_large")
        self.assertEqual(self.worker.process.wait(timeout=30), 2)

    def test_revision_conflicts_noops_and_receipt_replay(self) -> None:
        document = self.create()
        original = self.revisions[document]
        params = self.write_params(document, {"width_px": 64})
        first = self.success("scene.settings", params)
        self.assertNotEqual(first["revision"], original)
        self.assertEqual(self.success("scene.settings", params), first)
        self.failure("scene.settings", {**params, "width_px": 128})
        self.revisions[document] = first["revision"]
        noop = self.mutate("scene.settings", document, width_px=64)
        self.assertEqual(noop["revision"], first["revision"])
        stale = self.write_params(document, {"width_px": 128})
        self.failure("scene.settings", {**stale, "expected_revision": original})
        self.assertEqual(
            self.success("scene.summary", {"document_id": document})["revision"], first["revision"]
        )

    def test_import_admission_rejects_missing_external_and_symlink_assets(self) -> None:
        document = self.create()
        external = self.directory / "unapproved.png"
        shutil.copyfile(self.fixture, external)
        link = self.inputs / "escaped.png"
        link.symlink_to(external)
        for path in (self.inputs / "missing.png", external, link):
            with self.subTest(path=path.name):
                self.failure(
                    "level.import",
                    self.write_params(
                        document,
                        {
                            "path": str(path),
                            "name": "Rejected",
                            "dpi": 120,
                        },
                    ),
                )
        summary = self.success("scene.summary", {"document_id": document})
        self.assertEqual(summary["revision"], self.revisions[document])
        self.assertEqual(summary["levels"], [])
        self.failure("scene.open", self.write_params(None, {"path": str(self.fixture)}))

    def test_native_decoder_failure_leaves_document_usable(self) -> None:
        document = self.create()
        level = self.mutate("level.create", document, name="Broken input", format="rgba8")
        truncated = self.inputs / "truncated.png"
        truncated.write_bytes(self.fixture.read_bytes()[:33])
        before = self.success("scene.summary", {"document_id": document})
        self.failure(
            "raster.import_png",
            self.write_params(
                document,
                {
                    "level_id": level["level_id"],
                    "drawing_id": {"number": 1, "suffix": ""},
                    "path": str(truncated),
                    "dpi": 120,
                },
            ),
        )
        self.assertEqual(self.success("scene.summary", {"document_id": document}), before)
        self.failure(
            "level.import",
            self.write_params(
                document,
                {
                    "name": "Invalid decoded asset",
                    "path": str(truncated),
                    "dpi": 120,
                },
            ),
        )
        self.assertEqual(self.success("scene.summary", {"document_id": document}), before)
        self.assertEqual(self.success("automation.capabilities", {})["protocol_version"], 5)

    def test_exposures_animation_and_document_isolation(self) -> None:
        document, level, column = self.populated_scene()
        other = self.create("Isolated")
        before = self.revisions[document]
        self.failure(
            "column.expose",
            self.write_params(
                other,
                {
                    "column_id": column["column_id"],
                    "level_id": level["level_id"],
                    "drawing_id": {"number": 1, "suffix": ""},
                    "start_frame": 0,
                    "length": 1,
                },
            ),
        )
        self.mutate(
            "animation.set_keys",
            document,
            object_id=column["object_id"],
            channel="x",
            unit="inch",
            keys=[
                {"frame": 0, "value": 0, "interpolation": "linear"},
                {"frame": 24, "value": 2, "interpolation": "linear"},
            ],
        )
        samples = self.success(
            "animation.sample",
            {
                "document_id": document,
                "object_id": column["object_id"],
                "channel": "x",
                "unit": "inch",
                "frames": [0, 12, 24],
            },
        )
        self.assertEqual([sample["value"] for sample in samples["values"]], [0, 1, 2])
        self.assertNotEqual(self.revisions[document], before)
        self.assertEqual(self.success("scene.summary", {"document_id": other})["levels"], [])

    def test_scene_edit_failures_leave_native_state_unchanged(self) -> None:
        document, level, column = self.populated_scene()
        self.mutate("column.update", document, column_id=column["column_id"], locked=True)
        before = self.success("scene.summary", {"document_id": document})
        self.failure(
            "column.expose",
            self.write_params(
                document,
                {
                    "column_id": column["column_id"],
                    "level_id": level["level_id"],
                    "drawing_id": {"number": 1, "suffix": ""},
                    "start_frame": 40,
                    "length": 1,
                },
            ),
        )
        self.assertEqual(self.success("scene.summary", {"document_id": document}), before)
        animation = {"object_id": column["object_id"], "channel": "x", "unit": "inch"}
        keys_before = self.success("animation.keys", {"document_id": document, **animation})
        self.failure(
            "animation.set_keys",
            self.write_params(
                document,
                {
                    **animation,
                    "keys": [
                        {"frame": 0, "value": 5, "interpolation": "linear"},
                        {"frame": 24, "value": 10, "interpolation": "invalid"},
                    ],
                },
            ),
        )
        self.assertEqual(
            self.success("animation.keys", {"document_id": document, **animation}), keys_before
        )
        first = self.mutate("object.create", document, name="First", kind="pegbar")
        second = self.mutate("object.create", document, name="Second", kind="pegbar")
        self.mutate(
            "object.parent",
            document,
            object_id=first["object_id"],
            parent_id=second["object_id"],
            placement_policy="preserve_local",
        )
        before = self.success("scene.summary", {"document_id": document})
        self.failure(
            "object.parent",
            self.write_params(
                document,
                {
                    "object_id": second["object_id"],
                    "parent_id": first["object_id"],
                    "placement_policy": "preserve_local",
                },
            ),
        )
        self.assertEqual(self.success("scene.summary", {"document_id": document}), before)

    def test_save_reopen_and_native_preview_pixels(self) -> None:
        document, _level, _column = self.populated_scene()
        preview = self.preview(document)
        width, height, pixels = decode_png(preview)
        self.assertEqual((width, height), (32, 32))
        for x, y in ((8, 8), (24, 8), (8, 24), (24, 24)):
            self.assertEqual(pixels[y * width + x], fixture_pixel(x, y, width), (x, y))
        saved = self.mutate("scene.save_copy", document, directory=str(self.workspace / "saved"))
        self.assertTrue(Path(saved["scene_path"]).is_file())
        self.assertTrue(Path(saved["manifest_path"]).is_file())
        self.assertTrue(saved["verified_drawings"])
        reopened = self.mutate("scene.open", None, path=saved["package_directory"])
        self.assertNotEqual(reopened["document_id"], document)
        self.assertEqual(decode_png(self.preview(reopened["document_id"])), (width, height, pixels))
        self.mutate("scene.discard", document)
        self.failure("scene.summary", {"document_id": document})
        self.assertEqual(decode_png(self.preview(reopened["document_id"])), (width, height, pixels))

    def test_missing_saved_assets_are_not_admitted(self) -> None:
        document, _level, _column = self.populated_scene()
        saved = self.mutate("scene.save_copy", document, directory=str(self.workspace / "missing"))
        package = Path(saved["package_directory"])
        assets = list(package.rglob("*.png"))
        self.assertTrue(assets, "Saved package must contain the imported raster drawing")
        assets[0].unlink()
        before = self.success("automation.list_documents", {})
        self.failure("scene.open", self.write_params(None, {"path": str(package)}))
        self.assertEqual(self.success("automation.list_documents", {}), before)

    def test_multiletter_drawing_suffix_survives_native_save_and_exposure(self) -> None:
        for image_format in ("vector", "rgba8", "toonzRaster"):
            with self.subTest(format=image_format):
                document = self.create(image_format)
                level = self.mutate("level.create", document, name="Suffix", format=image_format)
                drawing = {"number": 1, "suffix": "abcdefgh"}
                target = {"level_id": level["level_id"], "drawing_id": drawing}
                dimensions = (
                    {} if image_format == "vector" else {"width": 2, "height": 2, "dpi": 120}
                )
                self.mutate("drawing.create", document, **target, format=image_format, **dimensions)
                other_drawing = {"number": 1, "suffix": "z"}
                self.mutate(
                    "drawing.create",
                    document,
                    level_id=level["level_id"],
                    drawing_id=other_drawing,
                    format=image_format,
                    **dimensions,
                )
                self.failure(
                    "drawing.create",
                    self.write_params(
                        document,
                        {
                            **target,
                            "drawing_id": {"number": 2, "suffix": "abcdefghi"},
                            "format": image_format,
                            **dimensions,
                        },
                    ),
                )
                column = self.mutate("column.create", document, name="Suffix")
                self.mutate(
                    "column.expose",
                    document,
                    **target,
                    column_id=column["column_id"],
                    start_frame=0,
                    length=3,
                )
                self.mutate(
                    "column.expose",
                    document,
                    level_id=level["level_id"],
                    drawing_id=other_drawing,
                    column_id=column["column_id"],
                    start_frame=3,
                    length=3,
                )
                saved = self.mutate(
                    "scene.save_copy",
                    document,
                    directory=str(self.workspace / f"suffix-{image_format}"),
                )
                reopened = self.mutate("scene.open", None, path=saved["package_directory"])
                other = reopened["document_id"]
                summary = self.success("scene.summary", {"document_id": other})
                inspected = self.success(
                    "level.inspect",
                    {
                        "document_id": other,
                        "level_id": summary["levels"][0]["level_id"],
                    },
                )
                self.assertIn(drawing, inspected["drawings"])
                self.assertIn(other_drawing, inspected["drawings"])
                cells = self.success(
                    "column.cells",
                    {
                        "document_id": other,
                        "column_id": summary["columns"][0]["column_id"],
                        "start_frame": 0,
                        "length": 6,
                    },
                )
                self.assertEqual(
                    [cell["drawing_id"] for cell in cells["cells"]],
                    [drawing] * 3 + [other_drawing] * 3,
                )
                self.mutate(
                    "drawing.create",
                    other,
                    level_id=summary["levels"][0]["level_id"],
                    drawing_id={"number": 2, "suffix": "a"},
                    format=image_format,
                    **dimensions,
                )

    def test_unregistered_and_altered_packages_are_not_admitted(self) -> None:
        document, _level, _column = self.populated_scene()
        saved = self.mutate("scene.save_copy", document, directory=str(self.workspace / "original"))
        copy = self.workspace / "unregistered"
        shutil.copytree(saved["package_directory"], copy)
        (copy / "manifest.json").write_bytes((copy / "manifest.json").read_bytes() + b"\n")
        before = self.success("automation.list_documents", {})
        error = self.failure("scene.open", self.write_params(None, {"path": str(copy)}))
        self.assertEqual(
            (error["code"], error["details"]["reason"]), ("unregistered_package", "not_registered")
        )
        manifest = Path(saved["manifest_path"])
        manifest.write_bytes(manifest.read_bytes() + b"\n")
        self.failure("scene.open", self.write_params(None, {"path": saved["package_directory"]}))
        self.assertEqual(self.success("automation.list_documents", {}), before)

    def test_cancellation_and_worker_termination_are_bounded(self) -> None:
        document, _level, _column = self.populated_scene()
        job = self.mutate(
            "render.submit", document, start_frame=0, end_frame=119, step=1, width=256, height=256
        )
        canceled = self.success("job.cancel", self.write_params(None, {"job_id": job["job_id"]}))
        self.assertIn(canceled["status"], {"cancellation_requested", "canceled"})
        self.assertEqual(self.wait_job(job["job_id"])["status"], "canceled")
        self.failure("job.result", {"job_id": job["job_id"]})
        self.mutate(
            "render.submit", document, start_frame=0, end_frame=119, step=1, width=256, height=256
        )
        self.worker.process.terminate()
        self.worker.process.wait(timeout=10)
        self.assertIsNotNone(self.worker.process.returncode)
        self.worker = Worker(
            self.executable, self.workspace, self.inputs, self.directory / "replacement.stderr.log"
        )
        self.addCleanup(self.worker.close)
        self.assertEqual(self.success("automation.list_documents", {})["documents"], [])
        self.failure("scene.summary", {"document_id": document})


def _method(function: CaseMethod, reason: str) -> CaseMethod:
    # No docstring: unittest would print it in place of the test's name.
    def run(case: NativeAcceptance) -> None:
        if reason and case.through_sdk:
            case.skipTest(f"worker-only: {reason}")
        function(case)

    return run


def register_cases() -> None:
    """Add every marked function of the case modules beside this file as a test method."""
    for path in sorted(Path(__file__).parent.glob("mcp_*_cases.py")):
        module = importlib.import_module(path.stem)
        for function in list(vars(module).values()):
            mark = getattr(function, MARK, None)
            if not inspect.isfunction(function) or mark is None:
                continue
            if function.__module__ != module.__name__:
                continue
            name = f"test_{mark['name']}"
            if name in vars(NativeAcceptance):
                raise RuntimeError(f"{path.name}: {name} is already a test of the suite")
            setattr(NativeAcceptance, name, _method(function, mark["worker_only"]))


register_cases()


class TimedResult(unittest.TextTestResult):
    """A text result that also records how many seconds each test took."""

    seconds: dict[str, float]
    _started = 0.0

    def startTest(self, test: unittest.TestCase) -> None:
        self._started = time.monotonic()
        super().startTest(test)

    def stopTest(self, test: unittest.TestCase) -> None:
        super().stopTest(test)
        name = test.id().rsplit(".", 1)[-1]
        self.seconds[name] = round(time.monotonic() - self._started, 2)


class TimedRunner(unittest.TextTestRunner):
    """A text runner whose result records each test's seconds."""

    def _makeResult(self) -> TimedResult:
        return TimedResult(self.stream, self.descriptions, self.verbosity)


def _suite(only: Path | None) -> unittest.TestSuite:
    loader = unittest.defaultTestLoader
    if only is None:
        return loader.loadTestsFromTestCase(NativeAcceptance)
    names = [line for line in only.read_text(encoding="utf-8").splitlines() if line]
    return unittest.TestSuite(NativeAcceptance(name) for name in names)


def _run(only: Path | None) -> int:
    report_path = NativeAcceptance.output / "acceptance.txt"
    TimedResult.seconds = {}
    with report_path.open("w", encoding="utf-8") as report:
        result = TimedRunner(stream=Tee(sys.stdout, report), verbosity=2).run(_suite(only))
    (NativeAcceptance.output / mcp_shards.DURATIONS_NAME).write_text(
        json.dumps(TimedResult.seconds, indent=1, sort_keys=True) + "\n"
    )
    for test, traceback in result.errors + result.failures:
        annotation(test.id(), traceback)
    return 0 if result.wasSuccessful() else 1


def main() -> int:
    """Run the real-worker suite and retain both text output and native diagnostics."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--media-tools", type=Path)
    parser.add_argument(
        "--shards", type=mcp_shards.shard_count, default=1, help="a number, or auto (one per CPU)"
    )
    parser.add_argument("--only", type=Path, help="a file naming the tests to run, one per line")
    args = parser.parse_args()
    NativeAcceptance.executable = args.executable.resolve(strict=True)
    NativeAcceptance.output = args.output.resolve()
    NativeAcceptance.media_tools = (
        args.media_tools.resolve(strict=True) if args.media_tools else None
    )
    NativeAcceptance.output.mkdir(parents=True, exist_ok=True)
    if args.shards == 1 or args.only:
        return _run(args.only)
    command = [sys.executable, str(Path(__file__).resolve()), "--executable", str(args.executable)]
    if args.media_tools:
        command += ["--media-tools", str(args.media_tools)]
    names = unittest.defaultTestLoader.getTestCaseNames(NativeAcceptance)
    return mcp_shards.run(list(names), args.shards, NativeAcceptance.output, command)


if __name__ == "__main__":
    sys.exit(main())
