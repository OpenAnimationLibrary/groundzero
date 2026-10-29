"""Exercise saved native SeExpr scenes through the actual batch renderer."""
import argparse
from collections import Counter
from contextlib import ExitStack
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from native_ui_test import windows_mesa_renderer


def run(command, directory, environment, report, label):
    report.append(f"{label}: starting\n")
    try:
        result = subprocess.run(command, cwd=directory, env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace", timeout=120)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        report.append(output)
        raise RuntimeError(f"{label} timed out after 120 seconds") from error
    report.append(result.stdout)
    report.append(f"{label}: status {result.returncode}\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--builder", required=True, type=Path)
    parser.add_argument("--composer", required=True, type=Path)
    parser.add_argument("--stuff", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    report, status = [], 1
    try:
        builder = args.builder.resolve(strict=True)
        composer = args.composer.resolve(strict=True)
        source = args.stuff.resolve(strict=True)
        with tempfile.TemporaryDirectory(prefix="seexpr-native-batch-") as temporary, ExitStack() as runtime:
            directory = Path(temporary).resolve(strict=True)
            (directory / ".seexpr-acceptance").write_text("disposable SeExpr fixture\n", encoding="ascii")
            stuff = directory / "portablestuff"
            shutil.copytree(source, stuff)
            (stuff / "projects").mkdir(exist_ok=True)
            (stuff / "toonzfarm").mkdir(exist_ok=True)
            environment = os.environ.copy()
            for key in ("TMPDIR", "TEMP", "TMP"):
                environment[key] = str(directory.parent)
            for key, leaf in (("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache"),
                              ("XDG_DATA_HOME", "data"), ("APPDATA", "appdata"),
                              ("LOCALAPPDATA", "localappdata")):
                location = directory / leaf
                location.mkdir()
                environment[key] = str(location)
            # Rendering requires the native platform's OpenGL context. The
            # surrounding QWidget tests can still select an offscreen backend.
            environment["QT_QPA_PLATFORM"] = (
                "windows" if os.name == "nt" else "cocoa" if sys.platform == "darwin" else "xcb")
            environment["LIBGL_ALWAYS_SOFTWARE"] = "1"
            mesa_runtime = (Path(environment["SEEXPR_TEST_MESA"])
                            if os.name == "nt" and environment.get("SEEXPR_TEST_MESA") else None)
            environment.update(runtime.enter_context(windows_mesa_renderer(composer, mesa_runtime)))
            report.append(f"Native batch Qt platform: {environment['QT_QPA_PLATFORM']}\n")
            # Binary dependencies are selected by existing platform CI setup;
            # both native fixture and renderer use the same deployed runtime.
            environment["PATH"] = os.pathsep.join(
                [str(composer.parent), str(builder.parent), environment.get("PATH", "")])
            built = run([str(builder), str(directory)], directory, environment, report,
                        "Native scene preparation")
            if built.returncode:
                raise RuntimeError("Native scene preparation failed")
            scenes = stuff / "projects" / "acceptance" / "scenes"
            # Check exact callbacks, including failures after a successful
            # frame and a requested range that starts past frame one. Error
            # animation deliberately renders its GUI fallback before the
            # separate headless failure boundary rejects the job.
            cases = (
                ("valid", "valid", {1, 2}, set(), False, []),
                ("syntax-error", "syntax-error", set(), {1, 2}, True, []),
                ("runtime-error", "runtime-error", set(), {1, 2}, True, []),
                ("runtime-first", "runtime-first", {2, 3}, {1}, True, []),
                ("runtime-middle", "runtime-middle", {1, 3}, {2}, True, []),
                ("runtime-last", "runtime-last", {1, 2}, {3}, True, []),
                ("runtime-range", "runtime-middle", {3}, {2}, True, ["-range", "2", "3"]),
                ("runtime-step", "runtime-middle", {1, 3}, set(), False, ["-step", "2"]),
                ("animation-error", "animation-error", {1, 2}, set(), True, []),
                ("unsupported-version", "unsupported-version", None, None, True, []),
                ("multimedia-syntax", "syntax-error", None, None, True, ["-multimedia", "1"]),
                ("multimedia-version", "unsupported-version", None, None, True, ["-multimedia", "1"]),
                ("surface-valid", "surface-valid", {1, 2, 3}, set(), False, []),
                ("surface-workers", "surface-valid", {1, 2, 3}, set(), False, []),
                ("surface-static", "surface-static", {1, 2, 3}, set(), False, []),
                ("surface-image-plane", "surface-image-plane", {1, 2, 3}, set(), False, []),
                ("surface-runtime-middle", "surface-runtime-middle", {1, 3}, {2}, True, []),
                ("surface-runtime-workers", "surface-runtime-middle", {1, 3}, {2}, True, []),
            )
            for name, scene_name, completed, failed, expects_error, options in cases:
                scene = scenes / f"{scene_name}.tnz"
                if not scene.is_file():
                    raise RuntimeError("Native scene preparation omitted a required fixture")
                threads = "4" if name in ("surface-workers", "surface-runtime-workers") else "single"
                command = [str(composer), str(scene), "-o", str(directory / "output" / f"{name}.png"),
                           "-nthreads", threads, "-TOONZROOT", str(stuff)] + options
                rendered = run(command, directory, environment, report, f"Batch {name}")
                if not expects_error:
                    if rendered.returncode:
                        raise RuntimeError("Valid native SeExpr scene failed batch rendering")
                elif rendered.returncode == 0:
                    raise RuntimeError(f"Invalid {name} scene reported successful batch rendering")
                elif "SeExpr" not in rendered.stdout:
                    raise RuntimeError(f"Invalid {name} scene failed without a SeExpr diagnostic")
                elif os.name != "nt" and rendered.returncode < 0:
                    raise RuntimeError(f"Invalid {name} scene crashed instead of reporting failure")
                elif os.name == "nt" and (rendered.returncode & 0xC0000000) == 0xC0000000 and \
                        (rendered.returncode & 0xFFFFFFFF) not in (0xFFFFFFFE, 0xFFFFFFFF):
                    raise RuntimeError(f"Invalid {name} scene crashed instead of reporting failure")
                if expects_error and "Compositing completed in" in rendered.stdout:
                    raise RuntimeError(f"Invalid {name} scene printed a successful completion summary")
                if name == "animation-error" and "a SeExpr expression could not be evaluated" not in rendered.stdout:
                    raise RuntimeError("Animation fallback did not reach the explicit batch failure boundary")
                if name in ("surface-runtime-middle", "surface-runtime-workers") and \
                        "a SeExpr expression could not be evaluated" not in rendered.stdout:
                    raise RuntimeError("Surface displacement did not reach the explicit batch expression failure counter")
                if completed is not None:
                    pattern = re.compile(rf"[/\\]{re.escape(name)}\.(\d+)\.png (computed|failed)(?=:|\s*$)", re.MULTILINE)
                    events = Counter((int(frame), outcome) for frame, outcome in pattern.findall(rendered.stdout))
                    expected = Counter([(frame, "computed") for frame in completed] +
                                       [(frame, "failed") for frame in failed])
                    if events != expected:
                        raise RuntimeError(f"Batch {name} misreported completed or failed frame numbers")
                    image_pattern = re.compile(rf"{re.escape(name)}\.(\d+)\.png")
                    written = {int(match.group(1)) for image in (directory / "output").iterdir()
                               if (match := image_pattern.fullmatch(image.name))}
                    if written != completed:
                        raise RuntimeError(f"Batch {name} wrote an incorrect set of output frames")
                if name == "valid":
                    checked = run([str(builder), str(directory), "--verify-output"], directory,
                                  environment, report, "Batch image verification")
                    if checked.returncode:
                        raise RuntimeError("Valid native SeExpr image verification failed")
            checked = run([str(builder), str(directory), "--verify-surfaces"], directory,
                          environment, report, "Saved surface image and worker verification")
            if checked.returncode:
                raise RuntimeError("Actual surface batch pixels, geometry, animation or worker verification failed")
            report.append("Saved GLB and Image Plane surfaces: single/multiple-worker pixels, animated attributes, displacement, scheduler inputs and failure counter verified\n")
            report.append("SeExpr native batch acceptance: PASS\n")
            status = 0
    except (OSError, RuntimeError, ValueError) as error:
        report.append(f"SeExpr native batch acceptance: FAIL: {error}\n")
    try:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text("".join(report), encoding="utf-8")
    except OSError as error:
        report.append(f"Could not write native batch report: {type(error).__name__}\n")
        status = 1
    print("".join(report), end="")
    return status


if __name__ == "__main__":
    sys.exit(main())
