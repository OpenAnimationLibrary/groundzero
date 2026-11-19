"""Render a scene with the native batch renderer and compare images: the shared oracle piece.

Two pieces of qualification need the same two things. The old-scene corpus compares the worker's
render of a scene with a stored render of it; the fidelity oracle compares a worker job with the
native renderer's output for the same scene. Both therefore use this module:

- `render_with_tcomposer` runs `tcomposer`, the batch renderer of the same build, on a scene that
  sits in a project, on one thread, in a private stuff folder, and returns the frames it wrote;
- `compare_images` compares two decoded 8-bit images under a stated `Policy`. The default policy is
  exact. A tolerance is a statement (how many code values, on which channels, for how many pixels,
  and why), is bounded here so that it cannot be wide enough to hide a shape, and never covers
  alpha unless the policy says so.
"""

from __future__ import annotations

import os
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from mcp_png import Pixel, decode_png

MAX_COLOR_TOLERANCE = 4
MAX_ALPHA_TOLERANCE = 2
STUFF_DIRECTORIES = (
    "profiles/layouts",
    "config",
    "fxs",
    "library",
    "projects",
    "studiopalette",
    "cache",
)


class OracleError(RuntimeError):
    """The native renderer could not be run or produced no image."""


@dataclass(frozen=True)
class Policy:
    """How closely two images must agree.

    Attributes:
        color: Most a red, green or blue code value may differ, where alpha is equal.
        alpha: Most an alpha code value may differ; 0 means alpha must be equal.
        pixels: Most pixels, as a fraction of the image, that may differ at all.
        why: What the tolerance is for; required whenever any bound is above zero.
    """

    color: int = 0
    alpha: int = 0
    pixels: float = 0.0
    why: str = ""

    def problems(self) -> list[str]:
        """What is wrong with the policy itself."""
        found = []
        if self.color > MAX_COLOR_TOLERANCE or self.alpha > MAX_ALPHA_TOLERANCE:
            found.append(
                f"a tolerance above {MAX_COLOR_TOLERANCE} color or {MAX_ALPHA_TOLERANCE} alpha "
                "code values could hide a shape or an edge"
            )
        if not 0 <= self.pixels <= 0.05:
            found.append("the fraction of differing pixels must be between 0 and 0.05")
        if (self.color or self.alpha or self.pixels) and not self.why.strip():
            found.append("a tolerance needs its reason in `why`")
        if min(self.color, self.alpha) < 0:
            found.append("a tolerance cannot be negative")
        return found

    @property
    def exact(self) -> bool:
        """Whether the policy allows no difference at all."""
        return not (self.color or self.alpha or self.pixels)


@dataclass
class Difference:
    """What differs between two images."""

    width: int
    height: int
    differing: int = 0
    max_color: int = 0
    max_alpha: int = 0
    first: tuple[int, int] | None = None
    box: tuple[int, int, int, int] | None = None
    problems: list[str] = field(default_factory=list)

    @property
    def equal(self) -> bool:
        """Whether the images agree under the policy."""
        return not self.problems


def decode_file(path: Path) -> tuple[int, int, list[Pixel]]:
    """Decode an 8-bit PNG file; a missing or unreadable file is an error, never an empty image."""
    try:
        data = path.read_bytes()
    except OSError as error:
        raise OracleError(f"cannot read the image {path}: {error}") from error
    try:
        return decode_png(data)
    except (ValueError, OSError) as error:
        raise OracleError(f"cannot decode the image {path}: {error}") from error


def _channel_differences(left: Pixel, right: Pixel) -> tuple[int, int]:
    color = max(abs(left[index] - right[index]) for index in range(3))
    return color, abs(left[3] - right[3])


def compare_images(
    expected: tuple[int, int, list[Pixel]],
    actual: tuple[int, int, list[Pixel]],
    policy: Policy = Policy(),
) -> Difference:
    """Compare two decoded images; the result lists every way they disagree under the policy."""
    width, height, wanted = expected
    difference = Difference(width, height)
    difference.problems.extend(policy.problems())
    if (width, height) != actual[:2]:
        difference.problems.append(f"size {actual[0]}x{actual[1]} instead of {width}x{height}")
        return difference
    columns: list[int] = []
    rows: list[int] = []
    for index, (left, right) in enumerate(zip(wanted, actual[2], strict=True)):
        color, alpha = _channel_differences(left, right)
        if not (color or alpha):
            continue
        difference.differing += 1
        difference.max_color = max(difference.max_color, color)
        difference.max_alpha = max(difference.max_alpha, alpha)
        columns.append(index % width)
        rows.append(index // width)
        if difference.first is None:
            difference.first = (index % width, index // width)
    if columns:
        difference.box = (min(columns), min(rows), max(columns), max(rows))
    _judge(difference, policy)
    return difference


def _judge(difference: Difference, policy: Policy) -> None:
    allowed = int(policy.pixels * difference.width * difference.height)
    if difference.max_alpha > policy.alpha:
        difference.problems.append(
            f"alpha differs by {difference.max_alpha}, more than {policy.alpha}, "
            f"first at {difference.first}, within {difference.box}"
        )
    if difference.max_color > policy.color:
        difference.problems.append(
            f"color differs by {difference.max_color}, more than {policy.color}, "
            f"first at {difference.first}, within {difference.box}"
        )
    if difference.differing > allowed:
        difference.problems.append(
            f"{difference.differing} pixels differ, more than the {allowed} allowed"
        )


def compare_files(expected: Path, actual: Path, policy: Policy = Policy()) -> Difference:
    """Compare two PNG files; either file missing is an error, not a pass."""
    return compare_images(decode_file(expected), decode_file(actual), policy)


def find_tcomposer(app_directory: Path) -> Path | None:
    """The batch renderer built beside an application executable, or None."""
    candidate = app_directory / "tcomposer"
    return candidate if candidate.is_file() and os.access(candidate, os.X_OK) else None


def _run(
    command: list[str], output: Path, environment: dict[str, str], timeout: float, scene: Path
) -> subprocess.CompletedProcess[str]:
    for stale in output.glob("frame.*.png"):
        stale.unlink()
    try:
        return subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout,
            env=environment,
            cwd=output,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise OracleError(f"tcomposer did not finish {scene.name} in {timeout} s") from error


def render_with_tcomposer(
    tcomposer: Path,
    scene: Path,
    output: Path,
    frames: range,
    *,
    stuff: Path,
    timeout: float = 300,
) -> list[Path]:
    """Render frames of a scene with the batch renderer and return the PNG files, in order.

    Args:
        tcomposer: The renderer of the build under test.
        scene: A .tnz inside a project (a `scenes.xml` binds it), whose output camera is the size
            to render.
        output: An existing folder that receives `frame.NNNN.png`.
        frames: The scene's own frame numbers, from zero.
        stuff: A private folder for the renderer's settings, created if missing.
        timeout: Seconds before the render is abandoned.
    Raises:
        OracleError: When the renderer fails, hangs or writes a different set of frames.
    """
    for name in STUFF_DIRECTORIES:
        (stuff / name).mkdir(parents=True, exist_ok=True)
    command = [
        str(tcomposer),
        str(scene),
        "-o",
        str(output / "frame..png"),
        "-range",
        str(frames.start + 1),
        str(frames.stop),
        "-nthreads",
        "1",
        "-TOONZROOT",
        str(stuff),
    ]
    # A vector drawing renders through OpenGL, which only the window-system platform offers.
    environment = {
        **os.environ,
        "DYLD_LIBRARY_PATH": str(tcomposer.parent),
        "QT_QPA_PLATFORM": "cocoa" if sys.platform == "darwin" else "xcb",
    }
    done = _run(command, output, environment, timeout, scene)
    written = sorted(output.glob("frame.*.png"))
    if done.returncode != 0 or len(written) != len(frames):
        tail = (done.stdout + done.stderr)[-600:]
        raise OracleError(
            f"tcomposer exit {done.returncode}, {len(written)} of {len(frames)} frames for "
            f"{scene.name}: {tail}"
        )
    return written
