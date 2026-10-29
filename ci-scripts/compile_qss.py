#!/usr/bin/env python3
"""Compile every theme under stuff/config/qss/Default/less/themes to its QSS.

Each theme file starts with a header such as

    // out: "../../../../Blue/Blue.qss"

which names the compiled stylesheet relative to the theme file (the same
convention the Easy LESS VSCode extension uses). This script reads that header
so adding a theme needs no change here.

Usage:
    python3 ci-scripts/compile_qss.py            # write every QSS
    python3 ci-scripts/compile_qss.py --check    # fail if any QSS is stale
    python3 ci-scripts/compile_qss.py Animate    # only themes whose name matches

`lessc` is taken from PATH; when it is missing, `npx --package less lessc` is
used instead (needs Node.js).
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LESS_DIR = ROOT / "stuff" / "config" / "qss" / "Default" / "less"
THEMES_DIR = LESS_DIR / "themes"
OUT_RE = re.compile(r'^//\s*out:\s*"([^"]+)"', re.MULTILINE)


def lessc_command():
    if shutil.which("lessc"):
        return ["lessc"]
    if shutil.which("npx"):
        return ["npx", "--yes", "--package", "less@4", "lessc"]
    sys.exit("compile_qss: neither lessc nor npx found on PATH")


def themes(filter_names):
    found = []
    for less in sorted(THEMES_DIR.rglob("*-theme.less")):
        match = OUT_RE.search(less.read_text(encoding="utf-8"))
        if not match:
            sys.exit(f"compile_qss: {less} has no '// out:' header")
        out = (less.parent / match.group(1)).resolve()
        if filter_names and not any(
            name.lower() in out.stem.lower() for name in filter_names
        ):
            continue
        found.append((less, out))
    return found


def compile_theme(cmd, less, out):
    result = subprocess.run(
        cmd + [str(less)], capture_output=True, text=True, cwd=str(LESS_DIR)
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        sys.exit(f"compile_qss: lessc failed on {less.relative_to(ROOT)}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="compare with the committed QSS instead of writing it",
    )
    parser.add_argument("names", nargs="*", help="only themes matching these names")
    args = parser.parse_args()

    cmd = lessc_command()
    stale = []
    for less, out in themes(args.names):
        compiled = compile_theme(cmd, less, out)
        rel = out.relative_to(ROOT)
        if args.check:
            current = out.read_text(encoding="utf-8") if out.exists() else None
            if current != compiled:
                stale.append(rel)
                print(f"STALE   {rel}")
            else:
                print(f"ok      {rel}")
        else:
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(compiled, encoding="utf-8", newline="\n")
            print(f"wrote   {rel}")

    if stale:
        names = ", ".join(str(path) for path in stale)
        sys.exit(
            f"compile_qss: {len(stale)} stylesheet(s) differ from their LESS "
            f"sources ({names}); run ci-scripts/compile_qss.py to regenerate"
        )


if __name__ == "__main__":
    main()
