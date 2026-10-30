"""Qualify an installed application artifact without a build-tree runtime."""

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

import native_ui_test
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "neural-style"))
import installed as neural_installed


def unique(paths, description):
    values = [path for path in paths if path.is_file() and not path.is_symlink()]
    if len(values) != 1:
        raise RuntimeError(f"Expected exactly one {description} in the produced artifact")
    return values[0]


def require_stuff(path):
    if not path.is_dir() or path.is_symlink():
        raise RuntimeError("The application artifact is missing its installed data directory")
    for name in ("config", "profiles", "projects"):
        if not (path / name).is_dir():
            raise RuntimeError("The installed application data is incomplete")
    if not (path / "doc" / "LICENSE" / "LICENSE_seexpr.txt").is_file():
        raise RuntimeError("The installed application is missing its expression-engine notice")
    return path


def linux_package(artifact, installed):
    archive = unique(artifact.rglob("*.tar.gz"), "Linux release tarball")
    unpacked = installed / "release"
    unpacked.mkdir()
    with tarfile.open(archive, "r:gz") as source:
        source.extractall(unpacked, filter="data")
    appimage = unique(unpacked.rglob("*.AppImage"), "packaged AppImage")
    appimage.chmod(appimage.stat().st_mode | 0o111)
    extracted = installed / "appimage"
    extracted.mkdir()
    subprocess.run([str(appimage), "--appimage-extract"], cwd=extracted,
                   stdout=subprocess.DEVNULL, check=True, timeout=180)
    root = extracted / "squashfs-root"
    launcher = root / "AppRun"
    executable = root / "usr" / "bin" / "OpenToonz"
    if not launcher.is_file() or not executable.is_file():
        raise RuntimeError("The extracted AppImage has no native application launcher")
    # Exercise AppRun as shipped, including forwarding the fixture argument.
    # The copied fixture still comes from the artifact's portable installation.
    return launcher, require_stuff(appimage.parent / "portablestuff")


def macos_package(artifact, installed):
    image = unique(artifact.rglob("*.dmg"), "macOS disk image")
    result = subprocess.run(["hdiutil", "attach", "-readonly", "-nobrowse",
                             "-noautoopen", "-plist", str(image)],
                            check=True, stdout=subprocess.PIPE, timeout=120)
    entities = plistlib.loads(result.stdout).get("system-entities", [])
    mounts = [Path(entity["mount-point"]) for entity in entities
              if "mount-point" in entity]
    if len(mounts) != 1:
        for mount in mounts:
            subprocess.run(["hdiutil", "detach", "-quiet", str(mount)],
                           check=False, timeout=60)
        raise RuntimeError("The application disk image has an unexpected volume layout")
    mount = mounts[0]
    try:
        applications = [path for path in mount.glob("*.app")
                        if path.is_dir() and not path.is_symlink()]
        if len(applications) != 1:
            raise RuntimeError("The disk image must contain one installed application bundle")
        application = installed / applications[0].name
        subprocess.run(["ditto", str(applications[0]), str(application)],
                       check=True, timeout=180)
    finally:
        subprocess.run(["hdiutil", "detach", "-quiet", str(mount)],
                       check=True, timeout=60)
    executable = application / "Contents" / "MacOS" / "OpenToonz"
    if not executable.is_file():
        raise RuntimeError("The installed bundle is missing its native application")
    return executable, require_stuff(application / "portablestuff")


def windows_package(artifact):
    executable = unique((path for path in artifact.rglob("*")
                         if path.name.casefold() == "opentoonz.exe"),
                        "packaged Windows application")
    return executable, require_stuff(executable.parent / "portablestuff")


def verify_test_artifacts_absent(directory):
    forbidden = {"opengl32.dll", "libgallium_wgl.dll", "opentoonz.exe.local",
                 "tcomposer.exe.local"}
    forbidden.update(name + suffix for name in ("seexprsdkfixture", "seexprinvalidfixture")
                     for suffix in (".dll", ".so", ".dylib"))
    drivers = {"seexpr_plugin_test", "seexpr_animation_test", "seexpr_fx_test",
               "seexpr_fx_ui_test", "seexpr_surface_fx_test", "seexpr_native_animcurve_test",
               "glbsurface_test",
               "seexpr_editor_test", "seexpr_controls_test", "seexpr_range_test",
               "seexpr_batch_fixture", "neuralstyle_host_test", "neuralstyle_test", "neuralstyle_editor_test"}
    suffixes = ("", ".exe", ".lib", ".exp", ".pdb", ".ilk", ".ipdb", ".iobj",
                ".exe.manifest")
    forbidden.update(driver + suffix for driver in drivers for suffix in suffixes)
    for path in directory.rglob("*"):
        if path.name.casefold() in forbidden:
            raise RuntimeError("A test-only native renderer, driver or SDK fixture is present in the installed package")


@contextmanager
def packaged_environment(platform, executable):
    # The harness interpreter is already running. Native children must resolve
    # their libraries/plugins from the package and normal OS runtime paths,
    # without inheriting setup-python or a developer Qt environment.
    original = os.environ.copy()
    try:
        for name in list(os.environ):
            if name.startswith(("DYLD_", "LD_")) or name in {
                    "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_FONTDIR",
                    "QT_OPENGL_DLL", "QTDIR", "QT_PATH", "CMAKE_PREFIX_PATH",
                    "PYTHONHOME", "PYTHONPATH", "APPDIR", "APPIMAGE", "ARGV0",
                    "SEEXPR_LINUX_CORE_DIRECTORY"}:
                os.environ.pop(name, None)
        if platform == "linux":
            os.environ["APPDIR"] = str(executable.parent)
        if platform == "windows":
            entries = []
            for entry in os.environ.get("PATH", "").split(os.pathsep):
                candidate = Path(entry)
                if entry and any((candidate / name).is_file()
                                 for name in ("Qt5Core.dll", "Qt6Core.dll")):
                    continue
                entries.append(entry)
            os.environ["PATH"] = os.pathsep.join(entries)
        yield
    finally:
        os.environ.clear()
        os.environ.update(original)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("linux", "macos", "windows"), required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    expected = "windows" if os.name == "nt" else "macos" if sys.platform == "darwin" else "linux"
    if args.platform != expected:
        raise RuntimeError("The package qualification platform does not match this fresh runner")
    artifact = args.artifact.resolve(strict=True)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    mesa = os.environ.get("SEEXPR_TEST_MESA")
    if args.platform == "windows" and not mesa:
        raise RuntimeError("Windows installed-package qualification requires the verified CI Mesa runtime")
    with tempfile.TemporaryDirectory(prefix="groundzero-seexpr-installed-") as temporary:
        installed = Path(temporary)
        if args.platform == "linux":
            executable, stuff = linux_package(artifact, installed)
        elif args.platform == "macos":
            executable, stuff = macos_package(artifact, installed)
        else:
            executable, stuff = windows_package(artifact)
        package = executable.parent if args.platform != "linux" else executable.parent / "usr"
        verify_test_artifacts_absent(package)
        component = package / "neural-style" if args.platform != "linux" else package / "bin" / "neural-style"
        neural_error = None
        try:
            neural_installed.check(component)
        except Exception as error:
            neural_error = str(error)
            print("::error title=Installed neural component::" + neural_error.replace("\n", " | "), flush=True)
        original_arguments = sys.argv
        try:
            arguments = [str(Path(native_ui_test.__file__)), str(executable), str(stuff),
                         "--output", str(output)]
            if mesa and args.platform == "windows":
                arguments.extend(["--windows-mesa", mesa])
            with packaged_environment(args.platform, executable):
                sys.argv = arguments
                status = native_ui_test.main()
        finally:
            sys.argv = original_arguments
            verify_test_artifacts_absent(package)
        if status != 0 or not output.is_file() or "seexpr-native-integration: PASS" not in output.read_text(
                encoding="utf-8", errors="replace").splitlines():
            raise RuntimeError("The installed application did not complete the native lifecycle fixture")
        print("seexpr-installed-package: PASS", flush=True)
        if neural_error:
            raise RuntimeError("Installed neural component failed: " + neural_error)
    return 0


if __name__ == "__main__":
    sys.exit(main())
