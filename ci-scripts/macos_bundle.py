"""Deploy and verify the Mach-O dependency closure of a macOS application."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

MACH_MAGIC = {
    b"\xfe\xed\xfa\xce",
    b"\xce\xfa\xed\xfe",
    b"\xfe\xed\xfa\xcf",
    b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf",
    b"\xbf\xba\xfe\xca",
}
LOAD_COMMANDS = {
    "LC_LOAD_DYLIB",
    "LC_LOAD_WEAK_DYLIB",
    "LC_REEXPORT_DYLIB",
    "LC_LOAD_UPWARD_DYLIB",
    "LC_LAZY_LOAD_DYLIB",
}
MAX_IMAGES = 4096
MAX_FILES = 100000


@dataclass(frozen=True)
class Image:
    dependencies: tuple[str, ...]
    rpaths: tuple[str, ...]
    identifier: str | None
    uuids: frozenset[str]


def command(arguments: list[str]) -> str:
    """Run one bounded native inspection or deployment command.

    Args:
        arguments: An executable and its arguments, without shell expansion.
    """
    try:
        result = subprocess.run(
            arguments, check=True, capture_output=True, text=True, timeout=60
        )
    except subprocess.CalledProcessError as error:
        details = (error.stderr or error.stdout or "").strip()
        raise RuntimeError(f"{arguments[0]} failed: {details[:8192]}") from error
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")
    if len(result.stdout) > 8 * 1024 * 1024:
        raise RuntimeError(f"Native tool output exceeds 8 MiB: {arguments[0]}")
    return result.stdout


def _field(block: str, name: str) -> str:
    found = re.search(rf"^\s*{name} (.+?) \(offset \d+\)\s*$", block, re.MULTILINE)
    if not found:
        raise RuntimeError(f"Malformed Mach-O {name} load command")
    return found.group(1)


def _load_command(block: str) -> tuple[str, str] | None:
    match = re.search(r"^\s*cmd (LC_\w+)\s*$", block, re.MULTILINE)
    if not match:
        raise RuntimeError("Malformed Mach-O command kind")
    kind = match.group(1)
    if kind in LOAD_COMMANDS:
        return "dependencies", _field(block, "name")
    if kind == "LC_RPATH":
        return "rpaths", _field(block, "path")
    if kind == "LC_ID_DYLIB":
        return "identifiers", _field(block, "name")
    if kind == "LC_UUID":
        uuid = re.search(r"^\s*uuid ([A-Fa-f0-9-]+)\s*$", block, re.MULTILINE)
        if not uuid:
            raise RuntimeError("Malformed Mach-O UUID")
        return "uuids", uuid.group(1).upper()
    return None


def parse_image(output: str) -> Image:
    """Read dylib names, runpaths and identities from otool load commands.

    Args:
        output: The complete successful output of otool -l for one image.
    """
    blocks = re.split(r"^Load command \d+\s*$", output, flags=re.MULTILINE)[1:]
    if not blocks or len(blocks) > 32768:
        raise RuntimeError("Missing or excessive Mach-O load commands")
    fields: dict[str, list[str]] = {
        key: [] for key in ("dependencies", "rpaths", "identifiers", "uuids")
    }
    for block in blocks:
        entry = _load_command(block)
        if entry is not None:
            fields[entry[0]].append(entry[1])
    identifiers = fields["identifiers"]
    if len(set(identifiers)) > 1:
        raise RuntimeError("Universal Mach-O slices have different install names")
    return Image(
        tuple(dict.fromkeys(fields["dependencies"])),
        tuple(dict.fromkeys(fields["rpaths"])),
        identifiers[0] if identifiers else None,
        frozenset(fields["uuids"]),
    )


def _system(path: str) -> bool:
    normalized = os.path.normpath(path)
    return normalized in ("/System/Library", "/usr/lib") or normalized.startswith(
        ("/System/Library/", "/usr/lib/")
    )


def _inside(path: Path, root: Path) -> bool:
    return path.resolve().is_relative_to(root)


def _macho(path: Path) -> bool:
    if not path.is_file():
        return False
    with path.open("rb") as stream:
        return stream.read(4) in MACH_MAGIC


def _images(application: Path) -> list[Path]:
    images = set()
    for count, path in enumerate((application / "Contents").rglob("*"), 1):
        if count > MAX_FILES:
            raise RuntimeError("Application scan exceeds 100000 entries")
        if path.is_symlink() and not _inside(path, application):
            raise RuntimeError(f"Bundle symlink escapes the application: {path}")
        if _macho(path):
            images.add(path.resolve(strict=True))
        if len(images) > MAX_IMAGES:
            raise RuntimeError("Application exceeds 4096 Mach-O images")
    if not images:
        raise RuntimeError("Application contains no Mach-O images")
    return sorted(images)


def _expand(reference: str, owner: Path, executable: Path) -> Path | None:
    for prefix, directory in (
        ("@loader_path", owner.parent),
        ("@executable_path", executable),
    ):
        if reference == prefix or reference.startswith(prefix + "/"):
            return directory / reference[len(prefix) :].lstrip("/")
    return Path(reference) if reference.startswith("/") else None


def _native_signature(path: Path, *, sign: bool = False) -> None:
    # A main-executable path can still identify its whole bundle to codesign.
    # Use a standalone copy: portablestuff intentionally remains at the app root.
    # This checks binary integrity, not a Gatekeeper or notarized app signature.
    # https://developer.apple.com/library/archive/technotes/tn2206/_index.html
    with tempfile.TemporaryDirectory(prefix="groundzero-macho-") as temporary:
        standalone = Path(temporary) / path.name
        shutil.copy2(path, standalone)
        original_mode = path.stat().st_mode
        if sign:
            standalone.chmod(original_mode | 0o200)
            command(
                [
                    "/usr/bin/codesign",
                    "--force",
                    "--sign",
                    "-",
                    "--timestamp=none",
                    str(standalone),
                ]
            )
        command(["/usr/bin/codesign", "--verify", "--strict", str(standalone)])
        if sign:
            path.chmod(original_mode | 0o200)
            shutil.copy2(standalone, path)
            path.chmod(original_mode)


class _Bundle:
    """Resolve one application without consulting ambient dynamic-loader variables."""

    def __init__(self, application: Path, library_roots: tuple[Path, ...] = ()) -> None:
        self.application = application.resolve(strict=True)
        self.frameworks = self.application / "Contents" / "Frameworks"
        self.main = self.application / "Contents" / "MacOS" / "OpenToonz"
        if not _macho(self.main):
            raise RuntimeError("The application main executable is not Mach-O")
        self.library_roots = tuple(root.resolve(strict=True) for root in library_roots)
        self.metadata: dict[Path, Image] = {}
        self.origins: dict[Path, Path] = {}
        self.architecture_sets: dict[Path, frozenset[str]] = {}
        self.dependencies: dict[frozenset[str], Path] = {}
        for path in _images(self.application):
            self.register_dependency(path)

    def register_dependency(self, path: Path) -> Path:
        """Reuse one loaded dylib path across equivalent versioned aliases."""
        image = self.inspect(path)
        if image.identifier is None or not image.uuids:
            return path
        canonical = self.dependencies.setdefault(image.uuids, path)
        self.require_architectures(canonical)
        return canonical

    def architectures(self, path: Path) -> frozenset[str]:
        """Read the architectures supported by one native image."""
        path = path.resolve(strict=True)
        if path not in self.architecture_sets:
            output = command(["/usr/bin/lipo", "-archs", str(path)]).strip()
            if not re.fullmatch(r"[A-Za-z0-9_]+(?:\s+[A-Za-z0-9_]+){0,31}", output):
                raise RuntimeError(
                    f"Malformed Mach-O architecture list for {path}: {output!r}"
                )
            self.architecture_sets[path] = frozenset(output.split())
        return self.architecture_sets[path]

    def require_architectures(self, path: Path) -> None:
        """Require native dependencies to support every main executable architecture."""
        missing = self.architectures(self.main) - self.architectures(path)
        if missing:
            raise RuntimeError(
                f"Missing application architectures {sorted(missing)} in {path}"
            )

    def inspect(self, path: Path) -> Image:
        """Inspect a native image once during this immutable inspection pass."""
        path = path.resolve(strict=True)
        if path not in self.metadata:
            if len(self.metadata) >= MAX_IMAGES * 2:
                raise RuntimeError("Dependency inspection exceeds 8192 images")
            self.metadata[path] = parse_image(
                command(["/usr/bin/otool", "-l", str(path)])
            )
        return self.metadata[path]

    def executable_directory(self, owner: Path) -> Path:
        """Return the nearest containing app's executable directory."""
        for parent in owner.parents:
            if parent.suffix == ".app" and _inside(parent, self.application):
                return parent / "Contents" / "MacOS"
        return self.main.parent

    def candidates(self, owner: Path, reference: str, *, deployed: bool) -> list[Path]:
        """Resolve declared paths first, then explicitly supplied deployment roots."""
        executable = self.executable_directory(owner)
        direct = _expand(reference, owner, executable)
        if direct is not None:
            candidates = [direct]
            if not deployed and owner in self.origins:
                # A direct reference has already matched an absolute/loader prefix.
                original = _expand(reference, self.origins[owner], executable)
                assert original is not None
                candidates.append(original)
            return candidates
        if not reference.startswith("@rpath/"):
            raise RuntimeError(f"Unsupported dependency {reference!r} in {owner}")
        relative = reference[len("@rpath/") :]
        candidates = self.runpath_candidates(owner, relative)
        if not deployed:
            candidates.extend([self.frameworks / relative, executable / relative])
            candidates.extend(root / relative for root in self.library_roots)
        return candidates

    def runpath_candidates(self, owner: Path, relative: str) -> list[Path]:
        """Find a declared runpath in the owner, its origin or the main image."""
        candidates = []
        images = dict.fromkeys((owner, self.main, self.origins.get(owner, owner)))
        for image in images:
            for rpath in self.inspect(image).rpaths:
                directory = _expand(rpath, image, self.executable_directory(owner))
                if directory is not None:
                    candidates.append(directory / relative)
        return candidates

    def resolve(self, owner: Path, reference: str, *, deployed: bool) -> Path:
        """Resolve a dependency, rejecting external paths in verification mode."""
        found = self.resolved_candidates(owner, reference, deployed=deployed)
        if not found:
            raise RuntimeError(f"Unresolved dependency {reference!r} in {owner}")
        if any(not self.same_image(found[0], candidate) for candidate in found[1:]):
            raise RuntimeError(
                f"Conflicting dependency candidates for {reference!r} in {owner}"
            )
        self.require_architectures(found[0])
        return found[0]

    def resolved_candidates(
        self, owner: Path, reference: str, *, deployed: bool
    ) -> list[Path]:
        """Collect existing unique paths without allowing deployed dependencies to escape."""
        found = []
        for candidate in self.candidates(owner, reference, deployed=deployed):
            if candidate.is_file():
                actual = candidate.resolve(strict=True)
                if deployed and not _inside(actual, self.application):
                    raise RuntimeError(
                        f"Ambient dependency {reference!r} in {owner}: {actual}"
                    )
                if actual not in found:
                    found.append(actual)
        return found

    def same_image(self, first: Path, second: Path) -> bool:
        """Compare identity across install-name edits, rejecting name collisions."""
        first_info, second_info = self.inspect(first), self.inspect(second)
        if first_info.uuids and second_info.uuids:
            return first_info.uuids == second_info.uuids
        with first.open("rb") as first_stream, second.open("rb") as second_stream:
            return (
                hashlib.file_digest(first_stream, "sha256").digest()
                == hashlib.file_digest(second_stream, "sha256").digest()
            )

    def copy_dependency(self, source: Path) -> Path:
        """Copy an actual dependency once, preserving framework layout and symlinks."""
        self.require_architectures(source)
        if _inside(source, self.application):
            return self.register_dependency(source)
        if not _macho(source):
            raise RuntimeError(f"Dependency is not Mach-O: {source}")
        framework = next(
            (parent for parent in source.parents if parent.suffix == ".framework"), None
        )
        destination = self.frameworks / source.name
        if framework is not None:
            destination = (
                self.frameworks / framework.name / source.relative_to(framework)
            )
        if existing := self.existing_dependency(source, destination):
            return existing
        self.frameworks.mkdir(parents=True, exist_ok=True)
        if framework is None:
            shutil.copy2(source, destination)
        else:
            shutil.copytree(framework, self.frameworks / framework.name, symlinks=True)
        destination = destination.resolve(strict=True)
        self.origins[destination] = source
        return self.register_dependency(destination)

    def existing_dependency(self, source: Path, destination: Path) -> Path | None:
        """Check name collisions before reusing a complete native dylib identity."""
        if destination.exists():
            self.require_architectures(destination)
            if not self.same_image(source, destination):
                raise RuntimeError(f"Conflicting bundled dependency: {destination}")
            destination = destination.resolve(strict=True)
            self.origins[destination] = source
            return self.register_dependency(destination)
        image = self.inspect(source)
        if image.identifier is not None:
            return self.dependencies.get(image.uuids)
        return None

    def rewrite(self, owner: Path, links: dict[str, Path]) -> None:
        """Replace loader references and remove non-bundle, non-system search paths."""
        info = self.inspect(owner)
        arguments = ["/usr/bin/install_name_tool"]
        for reference, target in links.items():
            rewritten = "@loader_path/" + os.fsdecode(
                os.path.relpath(target, owner.parent)
            )
            if rewritten != reference:
                arguments.extend(["-change", reference, rewritten])
        if info.identifier is not None and info.identifier.startswith("/"):
            arguments.extend(
                [
                    "-id",
                    "@rpath/" + os.fsdecode(os.path.relpath(owner, self.frameworks)),
                ]
            )
        arguments.extend(self.runpath_changes(owner, info))
        desired = "@executable_path/../Frameworks"
        if owner == self.main and desired not in info.rpaths:
            arguments.extend(["-add_rpath", desired])
        if len(arguments) > 1:
            owner.chmod(owner.stat().st_mode | 0o200)
            command([*arguments, str(owner)])

    def runpath_changes(self, owner: Path, info: Image) -> list[str]:
        """Preserve Apple paths and make every remaining search directory portable."""
        arguments = []
        for rpath in info.rpaths:
            if _system(rpath):
                continue
            expanded = _expand(rpath, owner, self.executable_directory(owner))
            if expanded is None or not _inside(expanded, self.application):
                arguments.extend(["-delete_rpath", rpath])
            elif rpath.startswith("/"):
                relative = "@loader_path/" + os.fsdecode(
                    os.path.relpath(expanded, owner.parent)
                )
                if relative in info.rpaths:
                    arguments.extend(["-delete_rpath", rpath])
                else:
                    arguments.extend(["-rpath", rpath, relative])
        return arguments


def _verify_runpaths(bundle: _Bundle, owner: Path, info: Image) -> None:
    for rpath in info.rpaths:
        expanded = _expand(rpath, owner, bundle.executable_directory(owner))
        if not _system(rpath) and (
            rpath.startswith("/")
            or expanded is None
            or not _inside(expanded, bundle.application)
        ):
            raise RuntimeError(f"Ambient runpath {rpath!r} in {owner}")


def _verify_load_path(
    bundle: _Bundle, target: Path, loaded: dict[frozenset[str], Path]
) -> None:
    info = bundle.inspect(target)
    if info.identifier is None or not info.uuids:
        return
    first = loaded.setdefault(info.uuids, target)
    if first != target:
        raise RuntimeError(f"Duplicate dylib load paths: {first} and {target}")


def verify_bundle(application: Path) -> int:
    """Reject dependencies and runpaths that can escape a shipped application.

    Args:
        application: The existing application bundle, inspected without mutation.
    """
    bundle = _Bundle(application)
    images = _images(bundle.application)
    loaded: dict[frozenset[str], Path] = {}
    for owner in images:
        bundle.require_architectures(owner)
        info = bundle.inspect(owner)
        _verify_runpaths(bundle, owner, info)
        for reference in info.dependencies:
            if not _system(reference):
                if reference.startswith("/"):
                    raise RuntimeError(
                        f"Nonportable absolute dependency {reference!r} in {owner}"
                    )
                target = bundle.resolve(owner, reference, deployed=True)
                _verify_load_path(bundle, target, loaded)
        _native_signature(owner)
    if "@executable_path/../Frameworks" not in bundle.inspect(bundle.main).rpaths:
        raise RuntimeError("Main executable lacks its bundle Frameworks runpath")
    return len(images)


def deploy_bundle(application: Path, library_roots: tuple[Path, ...]) -> int:
    """Copy the dependency closure, rewrite load paths and verify the result.

    Args:
        application: The writable bundle after Qt deployment.
        library_roots: Explicit native library search directories from the build host.
    """
    bundle = _Bundle(application, library_roots)
    rewrites: dict[Path, dict[str, Path]] = {}
    while pending := [
        path for path in _images(bundle.application) if path not in rewrites
    ]:
        while pending:
            owner = pending.pop()
            if owner in rewrites:
                continue
            if len(rewrites) >= MAX_IMAGES:
                raise RuntimeError("Dependency closure exceeds 4096 images")
            bundle.require_architectures(owner)
            links = _bundle_links(bundle, owner)
            pending.extend(links.values())
            rewrites[owner] = links
    for owner, links in rewrites.items():
        bundle.rewrite(owner, links)
        _native_signature(owner, sign=True)
    return verify_bundle(bundle.application)


def _bundle_links(bundle: _Bundle, owner: Path) -> dict[str, Path]:
    """Copy and resolve the direct declared dependencies of one bundled image."""
    links = {}
    for reference in bundle.inspect(owner).dependencies:
        if not _system(reference):
            source = bundle.resolve(owner, reference, deployed=False)
            links[reference] = bundle.copy_dependency(source)
    return links


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("application", type=Path)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--library-root", type=Path, action="append", default=[])
    args = parser.parse_args()
    if args.verify and args.library_root:
        parser.error("Verification cannot use build-host library roots")
    count = (
        verify_bundle(args.application)
        if args.verify
        else deploy_bundle(args.application, tuple(args.library_root))
    )
    print(f"macos-bundle-closure: PASS ({count} Mach-O images)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as failure:
        print(
            "::error title=macOS bundle closure::" + str(failure).replace("\n", " | "),
            file=sys.stderr,
        )
        raise SystemExit(1) from failure
