"""Sign relocated bundled code inside out, then the isolated import executable.

Apple TN2206 requires nested code signed before its enclosing bundle. Do not use
--deep signing: the brush helper alone receives its explicit sandbox identity.
Existing dependency entitlements/flags/identity are preserved, not replaced by
helper entitlements. This CI package remains ad-hoc signed, not notarized.
https://developer.apple.com/library/archive/technotes/tn2206/_index.html
"""
import argparse
from pathlib import Path
import plistlib
import subprocess

MAGIC = {bytes.fromhex(s) for s in (
    "feedface", "cefaedfe", "feedfacf", "cffaedfe", "cafebabe", "bebafeca",
    "cafebabf", "bfbafeca")}


def run(*args):
    subprocess.run(args, check=True, timeout=120)


def signing_targets(app):
    leaves, containers = set(), set()
    for directory in (app / "Contents" / "Frameworks", app / "Contents" / "PlugIns"):
        if not directory.exists():
            continue
        for path in directory.rglob("*"):
            if path.is_symlink():
                # Framework aliases are signed through their actual member.
                if not path.resolve().is_relative_to(app):
                    raise RuntimeError("Bundled code alias escapes application")
                continue
            if path.is_dir() and path.suffix in (".framework", ".bundle", ".plugin"):
                containers.add(path)
            elif path.is_file():
                with path.open("rb") as stream:
                    if stream.read(4) in MAGIC:
                        leaves.add(path)
    # Deeper nested code first, enclosing frameworks only after their contents.
    ordered = sorted(leaves | containers, key=lambda p: (-len(p.parts), str(p)))
    return ordered


def has_signature(path):
    result = subprocess.run(["/usr/bin/codesign", "--display", str(path)],
                            capture_output=True, timeout=30)
    if result.returncode == 0:
        return True
    if b"code object is not signed at all" in result.stderr:
        return False
    raise RuntimeError("Cannot establish existing code-signing state: " + str(path))


def verify_app(app):
    if has_signature(app):
        run("/usr/bin/codesign", "--verify", "--strict", "--verbose=2", str(app))
    else:
        print("Enclosing application remains unsigned under existing package policy")


def sign_existing(path, identity):
    signed = has_signature(path)
    command = ["/usr/bin/codesign", "--force", "--sign", identity]
    if signed:
        command += ["--preserve-metadata=identifier,entitlements,flags,runtime"]
    run(*command, str(path))
    run("/usr/bin/codesign", "--verify", "--strict", "--verbose=2", str(path))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("entitlements", type=Path)
    parser.add_argument("--identity", default="-")
    parser.add_argument("--verify-app-only", action="store_true")
    args = parser.parse_args()
    app = args.app.resolve(strict=True)
    if args.verify_app_only:
        verify_app(app)
        return
    # Capture this before re-signing nested code. Qt5 deployment without
    # -codesign defaults to unsigned packaging; do not invent a new app policy.
    app_was_signed = has_signature(app)
    entitlements = args.entitlements.resolve(strict=True)
    if plistlib.loads(entitlements.read_bytes()) != {"com.apple.security.app-sandbox": True}:
        raise RuntimeError("Unexpected brush import entitlement expansion")
    targets = signing_targets(app)
    if not targets:
        raise RuntimeError("No deployed dependencies found")
    for path in targets:
        sign_existing(path, args.identity)
    helper = app / "Contents" / "MacOS" / "rasterbrushimport-helper"
    run("/usr/bin/codesign", "--force", "--sign", args.identity,
        "--identifier", "org.openanimationlibrary.groundzero.brush-import",
        "--entitlements", str(entitlements), str(helper))
    # macdeployqt also relocates the main executable and sibling tools. Repair
    # existing signatures on those tools, without granting helper entitlements.
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    main_name = info["CFBundleExecutable"]
    if not isinstance(main_name, str) or Path(main_name).name != main_name:
        raise RuntimeError("Invalid application executable name")
    for path in sorted((app / "Contents/MacOS").iterdir()):
        if path.is_symlink() or not path.is_file() or path == helper or path.name == main_name:
            continue
        with path.open("rb") as stream:
            macho = stream.read(4) in MAGIC
        if macho and has_signature(path):
            sign_existing(path, args.identity)
    # Signing the app updates its main Mach-O and outer resource seal together,
    # after all nested changes. Preserve app metadata, never helper metadata.
    if app_was_signed:
        sign_existing(app, args.identity)
    verify_app(app)
    # Recheck every leaf after signing enclosing frameworks; none may be stale.
    for path in [*targets, helper]:
        run("/usr/bin/codesign", "--verify", "--strict", "--verbose=2", str(path))
    print("Validated deployed code signatures:", len(targets), "dependencies plus sandbox helper")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("::error::Deployed code signing failed: " + str(error).replace("%", "%25").replace("\n", "%0A").replace("\r", "%0D"), flush=True)
        raise
