"""The render compatibility matrix: every admitted FX type, output format and platform.

`mcp_fx_matrix.json` holds one cell for every combination of an admitted FX type (read from the
profile table in `automation_fx_profile.cpp`), an output format and a platform (the census lists
them). A cell says `passed`, `failed`, `unavailable` or `not_tested`, each with its reason or
measurement, never merged into one count. A recorded run carries the identity the worker reported
(`runtime.inspect`: build, operating system, Qt, graphics backend, plugin policy, fonts, media
tools and codec), and each cell names the run that wrote it and the tolerance its comparison used.
`doc/mcp_fx_render_matrix.md` is generated from the data and compared with the stored copy.

    mcp_fx_matrix.py                check the data and the document (no worker)
    mcp_fx_matrix.py --update       add missing cells as not_tested, drop cells of removed types,
                                    rewrite the document; never changes a recorded status
    mcp_fx_matrix.py --document P   write the document to P

Results come only from `mcp_fx_matrix_cases.py`: its acceptance case renders every cell on the
host and compares with what is stored, and writes nothing; `mcp_fx_matrix_cases.py record --reason`
is the one command that stores results.

The check fails when:

- a type, format or platform has no cell, or a cell is for something that no longer exists;
- a cell has no reason or no measurement for its status, or a tolerance above the format's cap
  (a broad tolerance would hide a driver, codec or font difference), or an observed difference
  above its own tolerance;
- a platform's label says it is complete although a cell was not run, or was run by an older run
  than the platform's latest, so a subset run cannot be labelled complete;
- a released tool has no negative test and is not on the list of gaps, or is on that list and now
  has one;
- the generated document differs from the data.
"""

from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any

from mcp_fx_matrix_text import render_document

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "ci-scripts"
DATA_PATH = SCRIPTS / "mcp_fx_matrix.json"
DOCUMENT_PATH = ROOT / "doc" / "mcp_fx_render_matrix.md"
DOCUMENT_RELATIVE = "doc/mcp_fx_render_matrix.md"
CENSUS_PATH = SCRIPTS / "mcp_census.json"
REVISION_MATRIX_PATH = SCRIPTS / "mcp_revision_matrix.json"
PROFILE_SOURCE = ROOT / "toonz" / "sources" / "toonz" / "automation_fx_profile.cpp"
SCHEMA_VERSION = 1
STATUSES = ("passed", "failed", "unavailable", "not_tested")
MINIMUM_REASON = 12
# The widest tolerance a format may state. PNG output is lossless, so any difference between two
# renders on one platform is a defect; MP4 is lossy, so its decoded frame is compared with the PNG
# render of the same scene under a stated error. Widening a cap is a change to this file.
TOLERANCE_CAPS: dict[str, dict[str, float]] = {
    "png_sequence": {"max_color": 0, "max_alpha": 0, "differing_pixels": 0},
    "mp4": {"mean_error": 40.0, "max_error": 220, "decoded_differing_bytes": 0},
}
PROFILE_ROW = re.compile(r'^ {6}\{"([A-Za-z0-9_]+)",\s*\d+,', re.MULTILINE)
NEGATIVE_CALL = re.compile(
    r"fail|reject|refus|error|invalid|denied|violation|bad|conflict", re.IGNORECASE
)
FALLBACK_NOTE = "added to the grid; no run has covered it yet"


def load_data(path: Path = DATA_PATH) -> dict[str, Any]:
    """Read the matrix data."""
    return json.loads(path.read_text(encoding="utf-8"))


def dump_data(data: dict[str, Any]) -> str:
    """Serialize the data with a stable layout."""
    return json.dumps(data, indent=1, ensure_ascii=False, sort_keys=False) + "\n"


def admitted_types(source: Path = PROFILE_SOURCE) -> list[str]:
    """The admitted native FX types in catalog order, read from the profile table."""
    text = source.read_text(encoding="utf-8")
    start = text.index("static const std::vector<TypeProfile> table")
    end = text.index("};", start)
    return PROFILE_ROW.findall(text[start:end])


def census_platforms(census: dict[str, Any]) -> list[str]:
    """Every platform the census names, qualified first."""
    platforms = census["platforms"]
    return [*platforms["qualified"], *platforms["not_recorded"]]


def _call_name(function: ast.expr) -> str:
    if isinstance(function, ast.Name):
        return function.id
    return function.attr if isinstance(function, ast.Attribute) else ""


def _loop_constants(tree: ast.AST) -> dict[str, set[str]]:
    """Names bound by `for name in ("a", "b")` loops, with the strings they range over."""
    found: dict[str, set[str]] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.For) and isinstance(node.target, ast.Name):
            if isinstance(node.iter, ast.Tuple | ast.List):
                values = {e.value for e in node.iter.elts if isinstance(e, ast.Constant)}
                found.setdefault(node.target.id, set()).update(
                    v for v in values if isinstance(v, str)
                )
    return found


def _negative_names(path: Path, tools: set[str]) -> set[str]:
    """Tools named, directly or by a loop over literals, as an argument of a refusal call."""
    tree = ast.parse(path.read_text(encoding="utf-8"))
    loops = _loop_constants(tree)
    found: set[str] = set()
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call) and NEGATIVE_CALL.search(_call_name(node.func))):
            continue
        for argument in [*node.args, *(keyword.value for keyword in node.keywords)]:
            for leaf in ast.walk(argument):
                if isinstance(leaf, ast.Constant) and isinstance(leaf.value, str):
                    if leaf.value in tools:
                        found.add(leaf.value)
                elif isinstance(leaf, ast.Name):
                    found |= loops.get(leaf.id, set()) & tools
    return found


def tools_with_negative_tests(tools: set[str], scripts: Path = SCRIPTS) -> set[str]:
    """Tools some acceptance case calls expecting a refusal, or that the revision table rejects."""
    found: set[str] = set()
    for path in sorted(scripts.glob("mcp_*.py")):
        found |= _negative_names(path, tools)
    revision = json.loads((scripts / "mcp_revision_matrix.json").read_text(encoding="utf-8"))
    for key, entry in revision["tools"].items():
        if entry.get("reject"):
            found.add(entry.get("tool", key))
    return found & tools


def _cell_problems(
    where: str, cell: dict[str, Any], caps: dict[str, float], runs: set[str]
) -> list[str]:
    status = cell.get("status")
    if status not in STATUSES:
        return [f"{where}: status {status!r} is not one of {list(STATUSES)}"]
    found: list[str] = []
    if status != "passed" and len(cell.get("reason", "").strip()) < MINIMUM_REASON:
        found.append(f"{where}: a {status} cell needs its reason")
    if status in {"passed", "failed"}:
        found += _measured_problems(where, cell, caps)
        if cell.get("run") not in runs:
            found.append(f"{where}: names run {cell.get('run')!r}, which is not recorded")
    elif "run" in cell or "observed" in cell:
        found.append(f"{where}: a {status} cell carries a measurement it did not make")
    return found


def _measured_problems(where: str, cell: dict[str, Any], caps: dict[str, float]) -> list[str]:
    tolerance, observed = cell.get("tolerance"), cell.get("observed")
    if not isinstance(tolerance, dict) or not isinstance(observed, dict):
        return [f"{where}: a cell that ran needs its tolerance and its observed difference"]
    found = []
    if set(tolerance) - {"why"} != set(caps):
        found.append(f"{where}: tolerance must state exactly {sorted(caps)} (and `why`)")
        return found
    for name, cap in caps.items():
        if tolerance[name] > cap:
            found.append(
                f"{where}: tolerance {name} {tolerance[name]} is above the cap {cap} for the "
                "format; a broad tolerance could hide a driver, codec or font difference"
            )
        if cell["status"] == "passed" and observed.get(name, float("inf")) > tolerance[name]:
            found.append(
                f"{where}: passed, but {name} {observed.get(name)} is above its tolerance "
                f"{tolerance[name]}"
            )
    if any(tolerance[name] for name in caps) and not tolerance.get("why", "").strip():
        found.append(f"{where}: a tolerance above zero needs its reason in `why`")
    return found


def expected_cells(data: dict[str, Any], types: list[str]) -> set[tuple[str, str, str]]:
    """Every (type, format, platform) the grid must hold."""
    formats = [entry["id"] for entry in data["formats"]]
    return {
        (kind, form, platform)
        for kind in types
        for form in formats
        for platform in data["platforms"]
    }


def present_cells(data: dict[str, Any]) -> dict[tuple[str, str, str], dict[str, Any]]:
    """The cells the data holds, by (type, format, platform)."""
    return {
        (kind, form, platform): cell
        for kind, forms in data["cells"].items()
        for form, platforms in forms.items()
        for platform, cell in platforms.items()
    }


def _reference_problems(data: dict[str, Any]) -> list[str]:
    """A passed cell needs its stored reference; a movie must be compared with the PNG's."""
    found = []
    for (kind, form, platform), cell in sorted(present_cells(data).items()):
        if cell["status"] != "passed":
            continue
        where = f"{kind} / {form} / {platform}"
        if not re.fullmatch(r"[0-9a-f]{64}", cell.get("reference", "")):
            found.append(f"{where}: passed without the SHA-256 of its reference render")
        if form == "mp4" and not re.fullmatch(r"[0-9a-f]{64}", cell.get("decoded_reference", "")):
            found.append(f"{where}: passed without the SHA-256 of its reference decode")
        png = data["cells"][kind].get("png_sequence", {}).get(platform, {})
        if form != "png_sequence" and png.get("reference") != cell.get("reference"):
            found.append(f"{where}: was not compared with the PNG render recorded for the type")
    return found


def _grid_problems(data: dict[str, Any], types: list[str]) -> list[str]:
    wanted, present = expected_cells(data, types), present_cells(data)
    found = [
        f"no cell for {' / '.join(key)}; run mcp_fx_matrix.py --update to add it as not_tested"
        for key in sorted(wanted - set(present))
    ]
    found += [
        f"cell {' / '.join(key)} is for something that no longer exists; "
        "run mcp_fx_matrix.py --update to drop it"
        for key in sorted(set(present) - wanted)
    ]
    return found


def _axis_problems(data: dict[str, Any], census: dict[str, Any]) -> list[str]:
    found = []
    named = census_platforms(census)
    if sorted(data["platforms"]) != sorted(named):
        found.append(
            f"platforms {sorted(data['platforms'])} differ from the census's {sorted(named)}"
        )
    formats = {entry["id"]: entry for entry in census["formats"]}
    for entry in data["formats"]:
        listed = formats.get(entry["census_format"])
        if listed is None or listed["state"] != "supported":
            found.append(f"format {entry['id']}: census has no supported {entry['census_format']}")
        if entry["id"] not in TOLERANCE_CAPS:
            found.append(f"format {entry['id']}: add its tolerance cap to TOLERANCE_CAPS")
    return found


def platform_summary(data: dict[str, Any], platform: str, types: list[str]) -> dict[str, Any]:
    """Counts per status, and whether the platform's rows all came from its latest run."""
    cells = [
        cell
        for (kind, _form, name), cell in present_cells(data).items()
        if name == platform and kind in types
    ]
    counts = Counter(cell["status"] for cell in cells)
    latest = data["platforms"][platform].get("latest_run")
    stale = sum(
        1 for cell in cells if cell["status"] in {"passed", "failed"} and cell["run"] != latest
    )
    complete = bool(cells) and counts["not_tested"] == 0 and stale == 0
    return {"counts": counts, "total": len(cells), "stale": stale, "complete": complete}


def _label_problems(data: dict[str, Any], types: list[str]) -> list[str]:
    found = []
    for platform, entry in data["platforms"].items():
        summary = platform_summary(data, platform, types)
        stated = entry.get("complete")
        if stated is not None and stated != summary["complete"]:
            found.append(
                f"platform {platform}: labelled complete={stated}, but its cells say "
                f"complete={summary['complete']} ({summary['counts']['not_tested']} not tested, "
                f"{summary['stale']} from an older run); a subset run cannot be labelled complete"
            )
        latest = entry.get("latest_run")
        if latest is not None and latest not in data["runs"]:
            found.append(f"platform {platform}: latest_run {latest!r} is not recorded")
    return found


def _run_problems(data: dict[str, Any]) -> list[str]:
    found = []
    for name, run in data["runs"].items():
        if run["platform"] not in data["platforms"]:
            found.append(f"run {name}: unknown platform {run['platform']}")
        if len(run.get("reason", "").strip()) < MINIMUM_REASON:
            found.append(f"run {name}: the reason is missing or too short")
        if not run.get("identity", {}).get("build"):
            found.append(f"run {name}: no build identity from runtime.inspect")
    return found


def negative_gap_problems(data: dict[str, Any], census: dict[str, Any], scripts: Path) -> list[str]:
    """Released tools without a negative test must be listed, and listed tools must lack one."""
    tools = set(census["tools"])
    lacking = tools - tools_with_negative_tests(tools, scripts)
    listed = data["negative_test_gaps"]
    found = [
        f"released tool {tool} has no negative test: write one (a call that must be refused, "
        "through a failure helper) or list it under negative_test_gaps with the reason"
        for tool in sorted(lacking - set(listed))
    ]
    found += [
        f"{tool} is listed under negative_test_gaps but now has a negative test; delete the entry"
        for tool in sorted(set(listed) & (tools - lacking))
    ]
    found += [
        f"{tool} is listed under negative_test_gaps but is not a released tool"
        for tool in sorted(set(listed) - tools)
    ]
    found += [
        f"negative_test_gaps {tool}: the reason is missing"
        for tool, reason in sorted(listed.items())
        if len(reason.strip()) < MINIMUM_REASON
    ]
    return found


def check_data(
    data: dict[str, Any], census: dict[str, Any], types: list[str], scripts: Path = SCRIPTS
) -> list[str]:
    """Every way the data does not describe the grid, or states more than its cells show."""
    found = [] if data.get("schema_version") == SCHEMA_VERSION else ["schema_version differs"]
    found += _axis_problems(data, census)
    found += _grid_problems(data, types)
    runs = set(data["runs"])
    formats = {entry["id"]: TOLERANCE_CAPS.get(entry["id"], {}) for entry in data["formats"]}
    for (kind, form, platform), cell in sorted(present_cells(data).items()):
        found += _cell_problems(f"{kind} / {form} / {platform}", cell, formats.get(form, {}), runs)
    found += _reference_problems(data) + _run_problems(data) + _label_problems(data, types)
    found += negative_gap_problems(data, census, scripts)
    if len(types) != census["claims"]["fx_builtins"]:
        found.append(
            f"the profile table lists {len(types)} types, the census claims "
            f"{census['claims']['fx_builtins']}"
        )
    return found


def summaries_of(data: dict[str, Any], types: list[str]) -> dict[str, dict[str, Any]]:
    """The summary of every platform."""
    return {platform: platform_summary(data, platform, types) for platform in data["platforms"]}


def document_of(data: dict[str, Any], types: list[str]) -> str:
    """The generated document for the data."""
    return render_document(data, types, summaries_of(data, types))


def check_document(data: dict[str, Any], types: list[str], path: Path = DOCUMENT_PATH) -> list[str]:
    """The stored document must equal the one generated from the data."""
    expected = document_of(data, types)
    if not path.is_file():
        return [f"{DOCUMENT_RELATIVE} does not exist; run mcp_fx_matrix.py --update"]
    if path.read_text(encoding="utf-8") != expected:
        return [f"{DOCUMENT_RELATIVE} is stale; run mcp_fx_matrix.py --update"]
    return []


def update_grid(data: dict[str, Any], types: list[str]) -> None:
    """Add every missing cell as not_tested and drop cells of types that are no longer admitted."""
    formats = [entry["id"] for entry in data["formats"]]
    cells: dict[str, Any] = {}
    for kind in types:
        for form in formats:
            for platform in data["platforms"]:
                old = data["cells"].get(kind, {}).get(form, {}).get(platform)
                new = old or {"status": "not_tested", "reason": FALLBACK_NOTE}
                cells.setdefault(kind, {}).setdefault(form, {})[platform] = new
    data["cells"] = cells


def main() -> int:
    """Check the matrix, or update its grid and document."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--document", type=Path)
    arguments = parser.parse_args()
    data = load_data()
    types = admitted_types()
    if arguments.update:
        update_grid(data, types)
        DATA_PATH.write_text(dump_data(data), encoding="utf-8")
        DOCUMENT_PATH.parent.mkdir(parents=True, exist_ok=True)
        DOCUMENT_PATH.write_text(document_of(data, types), encoding="utf-8")
    if arguments.document:
        arguments.document.write_text(document_of(data, types), encoding="utf-8")
    census = json.loads(CENSUS_PATH.read_text(encoding="utf-8"))
    problems = check_data(data, census, types) + check_document(data, types)
    for problem in problems:
        print(f"fx matrix: {problem}")
    if not problems:
        cells = len(present_cells(data))
        print(f"fx matrix: {len(types)} types, {cells} cells, {len(data['runs'])} runs")
    return int(bool(problems))


if __name__ == "__main__":
    sys.exit(main())
