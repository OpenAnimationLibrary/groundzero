"""Author and verify an editable 120-frame native shot using original BSD-3-Clause artwork."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import struct
import sys
import time
import traceback
import zlib
from pathlib import Path
from typing import Any

from mcp_png import SIGNATURE, Pixel, decode_png
from mcp_worker import Worker
from run_harness_tests import annotation

WIDTH, HEIGHT, FRAMES, DPI = 512, 288, 120, 128
PROBE_FRAMES = (0, 30, 60, 90, 119)
DRAWING = {"number": 1, "suffix": ""}
TRANSPARENT: Pixel = (0, 0, 0, 0)
INK: Pixel = (10, 24, 41, 255)
CREAM: Pixel = (246, 230, 186, 255)
GOLD: Pixel = (235, 153, 63, 255)
CYAN: Pixel = (91, 224, 226, 255)


def require(condition: bool, message: str) -> None:
    """Reject incomplete or visually incorrect evidence with an actionable failure."""
    if not condition:
        raise AssertionError(message)


class Canvas:
    """Create original flat-color illustration pixels without external assets or libraries."""

    def __init__(self, width: int, height: int) -> None:
        self.width, self.height = width, height
        self.pixels = [TRANSPARENT] * (width * height)

    def rectangle(self, box: tuple[int, int, int, int], color: Pixel) -> None:
        """Paint a clipped rectangle in top-to-bottom image coordinates."""
        left, top, right, bottom = box
        left, right = max(0, left), min(self.width, right)
        if right <= left:
            return
        for row in range(max(0, top), min(self.height, bottom)):
            self.pixels[row * self.width + left : row * self.width + right] = [color] * (
                right - left
            )

    def ellipse(self, box: tuple[int, int, int, int], color: Pixel) -> None:
        """Paint an ellipse using deterministic horizontal spans."""
        left, top, right, bottom = box
        center_x, center_y = (left + right) / 2, (top + bottom) / 2
        radius_x, radius_y = (right - left) / 2, (bottom - top) / 2
        for row in range(max(0, top), min(self.height, bottom)):
            extent = radius_x * math.sqrt(max(0, 1 - ((row + 0.5 - center_y) / radius_y) ** 2))
            span = (
                math.ceil(center_x - extent - 0.5),
                row,
                math.ceil(center_x + extent - 0.5),
                row + 1,
            )
            self.rectangle(span, color)

    def line(self, start: tuple[int, int], end: tuple[int, int], radius: int, color: Pixel) -> None:
        """Paint a rounded stroke by overlapping circular spans."""
        steps = max(abs(end[0] - start[0]), abs(end[1] - start[1]), 1)
        for step in range(steps + 1):
            x = round(start[0] + (end[0] - start[0]) * step / steps)
            y = round(start[1] + (end[1] - start[1]) * step / steps)
            self.ellipse((x - radius, y - radius, x + radius, y + radius), color)


def harbor_art() -> Canvas:
    """Draw a moonlit harbor with a transparent sky for the native gradient generator."""
    art = Canvas(640, 360)
    art.ellipse((477, 35, 527, 85), CREAM)
    for x, y, radius in ((492, 50, 5), (510, 68, 7), (493, 72, 3)):
        art.ellipse((x - radius, y - radius, x + radius, y + radius), (210, 207, 173, 255))
    for x, y in ((95, 67), (173, 38), (241, 83), (377, 52), (442, 111), (553, 96), (327, 123)):
        art.rectangle((x - 1, y - 3, x + 1, y + 3), CREAM)
        art.rectangle((x - 3, y - 1, x + 3, y + 1), CREAM)
    for index in range(18):
        x, top = index * 38 - 12, 195 + (index * 17) % 49
        art.rectangle((x, top, x + 32, 307), (23, 47, 66, 255))
        art.rectangle((x + 4, top - 5, x + 28, top), (28, 57, 74, 255))
        for row in range(top + 13, 292, 17):
            for column in (x + 7, x + 21):
                art.rectangle((column, row, column + 4, row + 6), (201, 151, 78, 255))
    art.rectangle((0, 287, 640, 360), (13, 34, 52, 255))
    art.rectangle((0, 286, 640, 289), (53, 104, 115, 255))
    for index in range(22):
        x, y = (index * 79) % 600, 302 + (index * 13) % 47
        art.rectangle((x, y, x + 28, y + 2), (35, 69, 85, 255))
    return art


def courier_art() -> Canvas:
    """Draw an original little flying courier with a helmet, parcel, and rocket boots."""
    art = Canvas(160, 160)
    for x in (60, 92):
        art.ellipse((x - 9, 118, x + 9, 155), CYAN)
        art.ellipse((x - 5, 122, x + 5, 145), CREAM)
        art.rectangle((x - 11, 110, x + 11, 125), INK)
    art.rectangle((30, 75, 54, 112), INK)
    art.rectangle((34, 80, 52, 108), (173, 95, 54, 255))
    art.line((55, 85), (30, 104), 8, INK)
    art.line((55, 85), (30, 104), 5, GOLD)
    art.line((105, 85), (132, 68), 8, INK)
    art.line((105, 85), (132, 68), 5, GOLD)
    for x, y in ((29, 105), (132, 67)):
        art.ellipse((x - 8, y - 8, x + 8, y + 8), INK)
        art.ellipse((x - 5, y - 5, x + 5, y + 5), CREAM)
    art.rectangle((51, 72, 110, 116), INK)
    art.rectangle((57, 77, 104, 112), GOLD)
    art.rectangle((59, 108, 103, 115), (32, 73, 91, 255))
    art.rectangle((67, 84, 94, 104), (181, 111, 58, 255))
    art.rectangle((71, 84, 91, 101), CREAM)
    art.rectangle((78, 84, 83, 101), GOLD)
    art.line((81, 17), (89, 3), 3, INK)
    art.ellipse((84, 0, 95, 10), GOLD)
    art.ellipse((34, 12, 127, 81), INK)
    art.ellipse((40, 18, 121, 75), CREAM)
    art.ellipse((47, 30, 115, 64), INK)
    art.ellipse((52, 35, 110, 59), (31, 88, 109, 255))
    art.line((60, 40), (75, 38), 2, CYAN)
    for x in (77, 98):
        art.ellipse((x - 3, 44, x + 3, 50), CREAM)
    art.ellipse((40, 43, 51, 58), GOLD)
    return art


def png_chunk(kind: bytes, content: bytes) -> bytes:
    """Encode one PNG chunk with its independent CRC."""
    return (
        struct.pack(">I", len(content))
        + kind
        + content
        + struct.pack(">I", zlib.crc32(kind + content))
    )


def write_art(path: Path, art: Canvas) -> None:
    """Retain the original authored artwork as an independently readable RGBA PNG."""
    rows = bytearray()
    for row in range(art.height):
        rows.append(0)
        for pixel in art.pixels[row * art.width : (row + 1) * art.width]:
            rows.extend(pixel)
    header = struct.pack(">IIBBBBB", art.width, art.height, 8, 6, 0, 0, 0)
    path.write_bytes(
        SIGNATURE
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(rows))
        + png_chunk(b"IEND", b"")
    )


def pixel_hash(pixels: list[Pixel]) -> str:
    return hashlib.sha256(bytes(component for pixel in pixels for component in pixel)).hexdigest()


class Shot:
    """Track native document revisions and retain verified artifacts from one worker."""

    def __init__(self, worker: Worker, output: Path) -> None:
        self.worker, self.output = worker, output
        self.revisions: dict[str, str] = {}
        self.operations = 0
        self.proof: dict[str, Any] = {"status": "running", "title": "Moonlight courier"}

    def read(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        """Require a successful response from the shipped native process."""
        response = self.worker.call(method, params)
        require("error" not in response, f"{method} failed: {response.get('error')}")
        result = response.get("result")
        if not isinstance(result, dict):
            raise AssertionError(f"{method} returned a nonobject result")
        return result

    def write(self, method: str, document: str | None = None, **params: Any) -> dict[str, Any]:
        """Attach a unique receipt and the actual current document revision."""
        self.operations += 1
        params["operation_id"] = f"shot-{self.operations}"
        if document is not None:
            params.update(document_id=document, expected_revision=self.revisions[document])
        result = self.read(method, params)
        if "document_id" in result:
            self.revisions[result["document_id"]] = result["revision"]
        return result

    def wait(self, job: str) -> dict[str, Any]:
        """Wait for closed output writers and a successful terminal render state."""
        deadline = time.monotonic() + 360
        while time.monotonic() < deadline:
            status = self.read("render.status", {"job_id": job})
            if status["status"] in {"completed", "failed", "canceled"}:
                require(status["status"] == "completed", f"Render failed: {status}")
                require(
                    status["writers_closed"], "Render reported completion before writers closed"
                )
                require(status["failed_frames"] == 0, f"Render omitted frames: {status}")
                return status
            time.sleep(0.1)
        raise TimeoutError(f"Render {job} did not finish within 360 seconds")

    def artifact(self, item: dict[str, Any], path: Path) -> list[Pixel]:
        """Check an opaque PNG artifact's checksum, dimensions, and actual decoded pixels."""
        result = self.read("artifact.read", {"artifact_id": item["artifact_id"]})
        require(result["image"]["mimeType"] == "image/png", "Artifact is not a PNG image")
        data = base64.b64decode(result["image"]["data"], validate=True)
        require(len(data) == item["size_bytes"], "Artifact byte count differs from its receipt")
        require(hashlib.sha256(data).hexdigest() == item["sha256"], "Artifact checksum changed")
        width, height, pixels = decode_png(data)
        require((width, height) == (WIDTH, HEIGHT), "Rendered camera dimensions changed")
        path.write_bytes(data)
        return pixels

    def preview(self, document: str, label: str, frame: int = 60) -> list[Pixel]:
        """Render an independent frame at the final shot resolution and retain its PNG."""
        revision = self.revisions[document]
        job = self.write("render.preview", document, frame=frame, width=WIDTH, height=HEIGHT)
        self.wait(job["job_id"])
        artifacts = self.read("render.artifacts", {"job_id": job["job_id"]})["artifacts"]
        require(len(artifacts) == 1, "A preview must produce exactly one artifact")
        require(artifacts[0]["frame"] == frame, "Preview returned another frame's artifact")
        require(job["source_revision"] == revision, "Preview used an unexpected scene revision")
        require(self.revisions[document] == revision, "Preview changed the editable document")
        return self.artifact(artifacts[0], self.output / "previews" / f"{label}.png")


def author_layer(shot: Shot, document: str, name: str, art: Canvas) -> dict[str, Any]:
    """Create and paint an editable native RGBA drawing, then expose all 120 frames."""
    level = shot.write("level.create", document, name=name, format="rgba8")
    target = {"level_id": level["level_id"], "drawing_id": DRAWING}
    shot.write(
        "drawing.create",
        document,
        **target,
        format="rgba8",
        width=art.width,
        height=art.height,
        dpi=DPI,
    )
    rows_per_patch = 16384 // art.width
    for bottom in range(0, art.height, rows_per_patch):
        height = min(rows_per_patch, art.height - bottom)
        pixels = []
        for row in range(bottom, bottom + height):
            start = (art.height - row - 1) * art.width
            for red, green, blue, alpha in art.pixels[start : start + art.width]:
                pixels.append({"r": red, "g": green, "b": blue, "a": alpha})
        shot.write(
            "raster.patch",
            document,
            **target,
            coordinate_space="raster_pixels_lower_left",
            x=0,
            y=bottom,
            width=art.width,
            height=height,
            sharing="all_exposures",
            pixels=pixels,
        )
    column = shot.write("column.create", document, name=name)
    shot.write(
        "column.expose",
        document,
        **target,
        column_id=column["column_id"],
        start_frame=0,
        length=FRAMES,
    )
    exported = shot.write(
        "raster.export_png",
        document,
        **target,
        path=str(shot.output / "workspace" / f"{name}-authored.png"),
    )
    checksum = pixel_hash(decode_png(Path(exported["path"]).read_bytes())[2])
    require(checksum == pixel_hash(art.pixels), f"Native painting changed original {name} pixels")
    shot.proof.setdefault("drawings", {})[name] = {
        "width": art.width,
        "height": art.height,
        "pixels_sha256": checksum,
    }
    return {**target, **column}


def set_motion(shot: Shot, document: str, target: str, channels: dict[str, Any]) -> None:
    """Write explicit native linear keys using inches and degrees."""
    for channel, (unit, values) in channels.items():
        keys = [
            {"frame": frame, "value": value, "interpolation": "linear"} for frame, value in values
        ]
        shot.write(
            "animation.set_keys", document, object_id=target, channel=channel, unit=unit, keys=keys
        )


def animate(shot: Shot, document: str, courier: dict[str, Any]) -> None:
    """Give the courier an editable flight peg and the camera a gentle tracking move."""
    peg = shot.write("object.create", document, name="Courier flight", kind="pegbar")
    shot.write(
        "object.parent", document, object_id=courier["object_id"], parent_id=peg["object_id"]
    )
    set_motion(
        shot,
        document,
        peg["object_id"],
        {
            "x": ("inch", [(0, -90 / DPI), (119, 70 / DPI)]),
            "y": (
                "inch",
                [(0, -10 / DPI), (30, 8 / DPI), (60, -8 / DPI), (90, 12 / DPI), (119, -4 / DPI)],
            ),
            "rotation": ("degree", [(0, -5), (60, 3), (119, -3)]),
        },
    )
    camera = shot.read("camera.inspect", {"document_id": document})
    set_motion(shot, document, camera["object_id"], {"x": ("inch", [(0, 0), (119, 12 / DPI)])})


def effect(shot: Shot, document: str, effect_type: str, name: str) -> str:
    return shot.write("fx.create", document, type=effect_type, name=name, frame_count=FRAMES)[
        "fx_id"
    ]


def set_parameters(shot: Shot, document: str, target: str, values: dict[str, Any]) -> None:
    """Set only explicitly requested native effect parameters."""
    for parameter, value in values.items():
        shot.write("fx.parameter.set", document, fx_id=target, parameter=parameter, value=value)


def connect(shot: Shot, document: str, target: str, sources: dict[str, str]) -> None:
    """Connect named native ports to already-owned effect handles."""
    for port, source in sources.items():
        shot.write("fx.connect", document, fx_id=target, port=port, source_fx_id=source)


def compose(shot: Shot, document: str, layers: list[dict[str, Any]]) -> str:
    """Compose sky and harbor, applying character Glow before their final composite."""
    sky = effect(shot, document, "STD_iwa_LinearGradientFx", "Sky gradient")
    set_parameters(
        shot,
        document,
        sky,
        {
            "startPoint": [0, 170],
            "endPoint": [0, -170],
            "startColor": [11, 20, 43, 255],
            "endColor": [49, 91, 112, 255],
        },
    )
    graph = shot.read("fx.graph", {"document_id": document})
    columns = {item["column_id"]: item["fx_id"] for item in graph["effects"] if "column_id" in item}
    background, courier = [columns[layer["column_id"]] for layer in layers]
    for target in (background, courier):
        shot.write("fx.terminal.set", document, fx_id=target, enabled=False)
    environment = effect(shot, document, "overFx", "Harbor over sky")
    connect(shot, document, environment, {"Source1": background, "Source2": sky})
    glow = effect(shot, document, "STD_glowFx", "Courier halo")
    set_parameters(
        shot,
        document,
        glow,
        {"value": 7, "brightness": 0, "fade": 100, "color": [82, 214, 255, 255]},
    )
    connect(shot, document, glow, {"Source": courier, "Light": courier})
    final = effect(shot, document, "overFx", "Courier over harbor")
    connect(shot, document, final, {"Source1": glow, "Source2": environment})
    connect(shot, document, graph["current_output_fx_id"], {"source": final})
    return glow


def export_background(shot: Shot, document: str, layer: dict[str, Any], label: str) -> str:
    """Fingerprint the entire background drawing through the native PNG export path."""
    target = {"level_id": layer["level_id"], "drawing_id": DRAWING}
    path = shot.output / "workspace" / f"background-{label}.png"
    result = shot.write("raster.export_png", document, **target, path=str(path))
    return pixel_hash(decode_png(Path(result["path"]).read_bytes())[2])


def check_environment(pixels: list[Pixel], frame: int) -> dict[str, Any]:
    """Check authored opaque landmarks and the sky independently of previous render output."""
    landmarks: tuple[tuple[str, int, int, Pixel], ...] = (
        ("moon", 500, 57, CREAM),
        ("building", 574, 228, (23, 47, 66, 255)),
        ("water", 96, 300, (13, 34, 52, 255)),
    )
    observed = {}
    for name, source_x, source_y, expected in landmarks:
        x = round(source_x - (640 - WIDTH) / 2 - 12 * frame / (FRAMES - 1))
        y = source_y - (360 - HEIGHT) // 2
        actual = pixels[y * WIDTH + x]
        require(actual == expected, f"Frame {frame} lost the authored {name}: {actual}")
        observed[name] = {"position": [x, y], "rgba": list(actual)}
    upper, lower = pixels[64 * WIDTH + 16], pixels[128 * WIDTH + 16]
    for pixel in (upper, lower):
        require(
            11 <= pixel[0] <= 49 and 20 <= pixel[1] <= 91 and 43 <= pixel[2] <= 112,
            f"Frame {frame} sky left the authored gradient color range: {pixel}",
        )
        require(pixel[3] == 255, f"Frame {frame} sky is unexpectedly transparent")
    require(lower[1] >= upper[1] + 4, f"Frame {frame} lost the vertical sky gradient")
    return {"landmarks": observed, "sky": [list(upper), list(lower)]}


def prove_effect_edit(shot: Shot, document: str, glow: str, background: dict[str, Any]) -> str:
    """Require a visible FX change while the background drawing and edge pixels remain unchanged."""
    background_hash = export_background(shot, document, background, "before")
    before = shot.preview(document, "before-glow")
    check_environment(before, 60)
    shot.write(
        "scene.save_copy", document, directory=str(shot.output / "workspace" / "before-edit")
    )
    set_parameters(shot, document, glow, {"brightness": 55})
    after = shot.preview(document, "after-glow")
    shot.proof["environment_oracle"] = check_environment(after, 60)
    changed = sum(first != second for first, second in zip(before, after, strict=True))
    require(
        changed > 1000, f"The Glow edit did not visibly affect the rendered character: {changed}"
    )
    border = [
        index
        for index in range(WIDTH * HEIGHT)
        if index % WIDTH < 48 or index % WIDTH >= WIDTH - 48
    ]
    require(
        all(before[index] == after[index] for index in border),
        "The character FX changed the background borders",
    )
    require(
        export_background(shot, document, background, "after") == background_hash,
        "The FX edit modified the independent background drawing",
    )
    shot.proof["effect_edit"] = {
        "changed_pixels": changed,
        "unchanged_border_pixels": len(border),
        "background_pixels_sha256": background_hash,
        "after_pixels_sha256": pixel_hash(after),
    }
    return pixel_hash(after)


def check_motion(
    shot: Shot, document: str, target: str, channel: str, expected: dict[int, float]
) -> None:
    """Check independently reopened endpoints and interior samples of each authored channel."""
    result = shot.read(
        "animation.sample",
        {
            "document_id": document,
            "object_id": target,
            "channel": channel,
            "unit": "degree" if channel == "rotation" else "inch",
            "frames": list(expected),
        },
    )
    require(
        [item["frame"] for item in result["values"]] == list(expected),
        f"Reopened {channel} motion omitted or reordered requested samples",
    )
    values = [item["value"] for item in result["values"]]
    require(
        all(
            math.isclose(value, sample, abs_tol=1e-9)
            for value, sample in zip(values, expected.values(), strict=True)
        ),
        f"Reopening changed an authored {channel} motion sample",
    )


def check_drawings(shot: Shot, document: str, levels: list[dict[str, Any]]) -> None:
    """Compare every reopened drawing with all of its originally authored native pixels."""
    for level in levels:
        expected = shot.proof["drawings"][level["name"]]
        target = {"level_id": level["level_id"], "drawing_id": DRAWING}
        description = shot.read("drawing.inspect", {"document_id": document, **target})
        require(description["format"] == "rgba8", "Reopening flattened an editable raster level")
        require(description["dpi"] == [DPI, DPI], "Reopening changed artwork DPI")
        require(
            (description["width"], description["height"])
            == (expected["width"], expected["height"]),
            "Reopening resized the original drawing",
        )
        exported = shot.write(
            "raster.export_png",
            document,
            **target,
            path=str(shot.output / "workspace" / f"{level['name']}-reopened.png"),
        )
        require(
            pixel_hash(decode_png(Path(exported["path"]).read_bytes())[2])
            == expected["pixels_sha256"],
            "Independent native reopening changed the editable drawing pixels",
        )


def reopen(shot: Shot, document: str, expected_pixels: str) -> str:
    """Discard the source, independently reopen its collected package, and verify editability."""
    saved = shot.write(
        "scene.save_copy", document, directory=str(shot.output / "workspace" / "shot")
    )
    shot.proof["native_package"] = str(Path(saved["package_directory"]).relative_to(shot.output))
    shot.write("scene.discard", document)
    opened = shot.write("scene.open", path=saved["package_directory"])["document_id"]
    require(opened != document, "Reopen reused the source document identity")
    summary = shot.read("scene.summary", {"document_id": opened})
    require(summary["frame_count"] == FRAMES, "The collected native shot lost timeline frames")
    require(
        len(summary["levels"]) == 2, "Background and character must remain separate editable levels"
    )
    check_drawings(shot, opened, summary["levels"])
    peg = next(item for item in summary["objects"] if item["name"] == "Courier flight")
    courier = next(item for item in summary["columns"] if item["name"] == "Courier")
    object_state = next(
        item for item in summary["objects"] if item["object_id"] == courier["object_id"]
    )
    require(
        object_state["parent_id"] == peg["object_id"], "The courier lost its editable parent peg"
    )
    check_motion(
        shot,
        opened,
        peg["object_id"],
        "x",
        {frame: (-90 + 160 * frame / 119) / DPI for frame in PROBE_FRAMES},
    )
    check_motion(
        shot,
        opened,
        peg["object_id"],
        "y",
        {
            frame: value / DPI
            for frame, value in zip(PROBE_FRAMES, (-10, 8, -8, 12, -4), strict=True)
        },
    )
    check_motion(
        shot,
        opened,
        peg["object_id"],
        "rotation",
        {0: -5, 30: -1, 60: 3, 90: 3 - 6 * 30 / 59, 119: -3},
    )
    check_motion(
        shot,
        opened,
        summary["camera"]["object_id"],
        "x",
        {frame: 12 * frame / 119 / DPI for frame in PROBE_FRAMES},
    )
    for column in summary["columns"]:
        require(column["last_frame"] == FRAMES - 1, "An exposed layer does not span the whole shot")
    require(
        pixel_hash(shot.preview(opened, "reopened")) == expected_pixels,
        "Independent native reopening changed the edited preview pixels",
    )
    shot.proof["reopened_scene"] = summary
    shot.proof["reopened_graph"] = shot.read("fx.graph", {"document_id": opened})
    return opened


def render_sequence(shot: Shot, document: str, expected_frames: dict[int, str]) -> None:
    """Render every frame and independently verify all 120 retained PNG artifacts."""
    job = shot.write(
        "render.submit", document, start_frame=0, end_frame=FRAMES - 1, width=WIDTH, height=HEIGHT
    )
    status = shot.wait(job["job_id"])
    require(status["completed_frames"] == FRAMES, "The render did not complete every shot frame")
    artifacts = shot.read("render.artifacts", {"job_id": job["job_id"]})["artifacts"]
    require(
        [item["frame"] for item in artifacts] == list(range(FRAMES)),
        "Frame artifact coverage is incomplete",
    )
    checks = []
    shot.proof["render"] = {
        "width": WIDTH,
        "height": HEIGHT,
        "frame_count": FRAMES,
        "frame_rate": 24,
        "source_revision": job["source_revision"],
        "artifacts": checks,
    }
    for item in artifacts:
        filename = f"frame.{item['frame'] + 1:04d}.png"
        pixels = shot.artifact(item, shot.output / "frames" / filename)
        check_environment(pixels, item["frame"])
        require(
            all(pixel[3] == 255 for pixel in pixels),
            f"The opaque shot has missing pixels: {filename}",
        )
        checksum = pixel_hash(pixels)
        if item["frame"] in expected_frames:
            require(
                checksum == expected_frames[item["frame"]],
                f"Sequence frame {item['frame']} differs from its independent source preview",
            )
        checks.append(
            {
                "frame": item["frame"],
                "path": f"frames/{filename}",
                "sha256": item["sha256"],
                "pixels_sha256": checksum,
            }
        )
    require(
        len({item["pixels_sha256"] for item in checks}) == FRAMES,
        "The continuously moving shot repeated a rendered frame",
    )


def reference_frames(shot: Shot, document: str, edited_preview: str) -> dict[int, str]:
    """Retain independent pre-save renders at the endpoints and interior motion keys."""
    expected = {60: edited_preview}
    for frame in PROBE_FRAMES:
        if frame == 60:
            continue
        pixels = shot.preview(document, f"source-frame-{frame}", frame)
        check_environment(pixels, frame)
        expected[frame] = pixel_hash(pixels)
    shot.proof["independent_frame_hashes"] = expected
    return expected


def create_shot(shot: Shot) -> None:
    """Author the native shot, then verify its persistence and every rendered frame."""
    document = shot.write(
        "scene.create",
        name="Moonlight courier",
        width_px=WIDTH,
        height_px=HEIGHT,
        width_in=WIDTH / DPI,
        height_in=HEIGHT / DPI,
        frame_rate=24,
    )["document_id"]
    background, courier = harbor_art(), courier_art()
    write_art(shot.output / "sources" / "harbor.png", background)
    write_art(shot.output / "sources" / "courier.png", courier)
    layers = [
        author_layer(shot, document, "Harbor", background),
        author_layer(shot, document, "Courier", courier),
    ]
    animate(shot, document, layers[1])
    glow = compose(shot, document, layers)
    expected_pixels = prove_effect_edit(shot, document, glow, layers[0])
    expected_frames = reference_frames(shot, document, expected_pixels)
    opened = reopen(shot, document, expected_pixels)
    render_sequence(shot, opened, expected_frames)
    shot.proof.update(
        status="passed",
        artwork_license="BSD-3-Clause",
        artwork_origin="Original procedural artwork authored through native drawing tools",
        render_reservation_bytes=(WIDTH * HEIGHT * 4 + 65536) * FRAMES,
        retained_job_reservation_bytes=(WIDTH * HEIGHT * 4 + 65536) * (FRAMES + 7),
    )


def record_failure(shot: Shot, error: Exception) -> None:
    """Keep the actual failure and traceback beside unfinished creative artifacts."""
    message = f"{type(error).__name__}: {error}"
    shot.proof.update(status="failed", error=message)
    with (shot.output / "failure.txt").open("a", encoding="utf-8") as report:
        report.write(traceback.format_exc() + "\n")
    annotation("Native 120-frame shot", message)


def run(executable: Path, output: Path) -> int:
    """Retain proof and native diagnostics even when a creative milestone fails."""
    for name in ("workspace", "inputs", "sources", "previews", "frames"):
        (output / name).mkdir()
    license_path = Path(__file__).resolve().parents[1] / "LICENSE.txt"
    (output / "artwork-LICENSE.txt").write_text(
        license_path.read_text(encoding="utf-8"), encoding="utf-8"
    )
    worker = Worker(
        executable, output / "workspace", output / "inputs", output / "worker.stderr.log"
    )
    shot = Shot(worker, output)
    try:
        create_shot(shot)
    except Exception as error:
        record_failure(shot, error)
    finally:
        try:
            worker.close()
        except Exception as error:
            record_failure(shot, error)
        (output / "proof.json").write_text(
            json.dumps(shot.proof, indent=2) + "\n", encoding="utf-8"
        )
    print(json.dumps({"status": shot.proof["status"], "proof": str(output / "proof.json")}))
    return int(shot.proof["status"] != "passed")


def main() -> int:
    """Run the native creative journey in a fresh artifact directory."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    try:
        return run(args.executable.resolve(strict=True), output)
    except (OSError, ValueError) as error:
        annotation("Native 120-frame shot setup", f"{type(error).__name__}: {error}")
        (output / "proof.json").write_text(
            json.dumps({"status": "failed", "error": str(error)}), encoding="utf-8"
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
