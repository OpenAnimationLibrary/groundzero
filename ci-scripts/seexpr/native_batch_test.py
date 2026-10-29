"""Exercise saved native SeExpr scenes through the actual batch renderer."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


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
        with tempfile.TemporaryDirectory(prefix="seexpr-native-batch-") as temporary:
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
            environment["QT_QPA_PLATFORM"] = "offscreen"
            environment["LIBGL_ALWAYS_SOFTWARE"] = "1"
            # Binary dependencies are selected by existing platform CI setup;
            # both native fixture and renderer use the same deployed runtime.
            environment["PATH"] = os.pathsep.join(
                [str(composer.parent), str(builder.parent), environment.get("PATH", "")])
            built = run([str(builder), str(directory)], directory, environment, report,
                        "Native scene preparation")
            if built.returncode:
                raise RuntimeError("Native scene preparation failed")
            scenes = stuff / "projects" / "acceptance" / "scenes"
            for name in ("valid", "syntax-error", "runtime-error", "animation-error", "unsupported-version"):
                scene = scenes / f"{name}.tnz"
                if not scene.is_file():
                    raise RuntimeError("Native scene preparation omitted a required fixture")
                command = [str(composer), str(scene), "-o", str(directory / "output" / f"{name}.png"),
                           "-nthreads", "single", "-TOONZROOT", str(stuff)]
                rendered = run(command, directory, environment, report, f"Batch {name}")
                if name == "valid":
                    if rendered.returncode:
                        raise RuntimeError("Valid native SeExpr scene failed batch rendering")
                elif rendered.returncode == 0:
                    raise RuntimeError(f"Invalid {name} scene reported successful batch rendering")
                elif "SeExpr" not in rendered.stdout:
                    raise RuntimeError(f"Invalid {name} scene failed without a SeExpr diagnostic")
                if name == "animation-error" and "a SeExpr expression could not be evaluated" not in rendered.stdout:
                    raise RuntimeError("Animation fallback did not reach the explicit batch failure boundary")
            checked = run([str(builder), str(directory), "--verify-output"], directory,
                          environment, report, "Batch image verification")
            if checked.returncode:
                raise RuntimeError("Valid native SeExpr image verification failed")
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
