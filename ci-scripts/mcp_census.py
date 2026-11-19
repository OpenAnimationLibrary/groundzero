"""Generate and check the MCP capability census against the tool catalog, README and tests.

The census data file lists every artist outcome with its classification, the tools that deliver
it and a disposition. Tool, toolset and test fields are derived. `--update` rewrites them, the
README tool index and the census document. The default check needs no worker; `--executable`
adds the comparison with the live catalog.
"""

from __future__ import annotations

import argparse
import ast
import json
import re
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any

from mcp_census_text import (
    render_document,
    render_readme_index,
    replace_region,
)

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "ci-scripts"
CENSUS_PATH = SCRIPTS / "mcp_census.json"
CENSUS_RELATIVE = "ci-scripts/mcp_census.json"
README_PATH = ROOT / "tools" / "mcp" / "README.md"
DOCUMENT_PATH = ROOT / "doc" / "mcp_capability_census.md"
NATIVE_TESTS = ROOT / "toonz" / "sources" / "toonz" / "tests"
ADAPTER_SOURCES = ROOT / "tools" / "mcp" / "src"
ACCEPTANCE = "ci-scripts/mcp_acceptance.py"
SCRIPT_EVIDENCE = ("mcp_shot.py", "mcp_live.py")
NATIVE_ADAPTER_TEST = "tools/mcp/src/native.test.ts"
REVISION_MATRIX_PATH = SCRIPTS / "mcp_revision_matrix.json"

CLASSIFICATIONS = (
    "exposed",
    "compositional",
    "optional",
    "not_exposed",
    "restricted",
    "unsupported",
)
DISPOSITIONS = {
    "exposed": {"implemented"},
    "compositional": {"implemented"},
    "optional": {"implemented", "planned"},
    "not_exposed": {"planned"},
    "restricted": {"restricted"},
    "unsupported": {"unsupported"},
}
OUTCOME_FIELDS = {"id", "domain", "outcome", "classification", "tools", "disposition"}
OUTCOME_OPTIONAL_FIELDS = {"requires", "journey"}
ORIGINS = ("worker", "adapter", "live_bridge")
MILESTONE = re.compile(r"^W(?:[0-9]|1[0-2])$")
UNSUPPORTED_FORMAT_TOKENS = frozenset(
    "TIF TIFF JPG JPEG GIF BMP TGA EXR HDR MOV AVI WEBM MKV MP3 FLAC OGG PSD PDF SVG XDTS SXF OCA "
    "GLB GLTF TLV WEBP".split()
)
TEST_CAPS = {"acceptance": 4, "matrix": 1, "native": 2, "adapter": 1, "script": 1}
TOOL_NAME = re.compile(r"`([a-z][a-z_]*(?:\.[a-z_]+)+)`")
TOOLSET_PROSE = re.compile(r"`([a-z][a-z0-9-]*)` toolset")
TOOLSET_FLAG = re.compile(r"--toolsets\s+([a-z][a-z0-9,-]*)")
FORMAT_LITERAL = re.compile(r'format:\s*"([a-z0-9_]+)"')
EXTENSION = re.compile(r"`\.([a-z0-9]+)`")
FILE_SUFFIXES = frozenset({"json", "xml", "exe", "md", "ts", "js", "py"})
UPPERCASE = re.compile(r"\b[A-Z][A-Z0-9]{2,}\b")


def load_census(path: Path = CENSUS_PATH) -> dict[str, Any]:
    """Read the census data file."""
    return json.loads(path.read_text(encoding="utf-8"))


def dump_census(census: dict[str, Any]) -> str:
    """Serialize the census with a stable layout."""
    return json.dumps(census, indent=1, ensure_ascii=False) + "\n"


def _outcome_problems(outcome: dict[str, Any], tools: dict[str, Any]) -> list[str]:
    name = f"outcome {outcome.get('id', '?')}"
    problems: list[str] = []
    keys = set(outcome)
    if missing := OUTCOME_FIELDS - keys:
        problems.append(f"{name}: missing {sorted(missing)}")
    if extra := keys - OUTCOME_FIELDS - OUTCOME_OPTIONAL_FIELDS:
        problems.append(f"{name}: unknown fields {sorted(extra)}")
    classification = outcome.get("classification")
    if classification not in CLASSIFICATIONS:
        problems.append(f"{name}: unknown classification {classification!r}")
        return problems
    for tool in outcome.get("tools", []):
        if tool not in tools:
            problems.append(f"{name}: names tool {tool} that has no census tool entry")
    return problems + _disposition_problems(name, outcome)


def _disposition_problems(name: str, outcome: dict[str, Any]) -> list[str]:
    classification = outcome["classification"]
    disposition = outcome.get("disposition")
    if not isinstance(disposition, dict) or not disposition.get("kind"):
        return [f"{name}: has no disposition"]
    kind = disposition["kind"]
    problems: list[str] = []
    if kind not in DISPOSITIONS[classification]:
        problems.append(f"{name}: a {classification} outcome cannot have disposition {kind!r}")
    if kind == "planned" and not MILESTONE.match(str(disposition.get("milestone"))):
        problems.append(f"{name}: a planned outcome needs a milestone label W0 to W12")
    if kind in {"restricted", "unsupported"} and not str(disposition.get("reason", "")).strip():
        problems.append(f"{name}: a {kind} outcome needs a reason")
    return problems + _evidence_problems(name, outcome, kind)


def _evidence_problems(name: str, outcome: dict[str, Any], kind: str) -> list[str]:
    classification = outcome["classification"]
    tools = outcome.get("tools", [])
    problems: list[str] = []
    if kind == "implemented" and not tools:
        problems.append(f"{name}: an implemented outcome needs at least one tool")
    if kind != "implemented" and tools:
        problems.append(f"{name}: only an implemented outcome may name tools")
    if classification == "compositional" and not outcome.get("journey"):
        problems.append(f"{name}: a compositional outcome needs a journey test")
    if classification == "optional" and kind == "implemented" and not outcome.get("requires"):
        problems.append(f"{name}: an implemented optional outcome must say what it requires")
    return problems


def outcome_help(tool: str) -> str:
    """Say which file and entry to add for a tool that no outcome delivers."""
    example = {
        "id": "area.what_the_artist_does",
        "domain": "an existing domain heading",
        "outcome": "One sentence on what the artist can now do.",
        "classification": "exposed",
        "tools": [tool],
        "disposition": {"kind": "implemented"},
    }
    return (
        f'  Add {tool!r} to the "tools" list of an outcome in {CENSUS_RELATIVE}, or add an '
        f'entry to its "outcomes" list, for example:\n  {json.dumps(example)}'
    )


def tool_entry_help(tool: str, toolset: str, writes: bool) -> str:
    """Say which file and entry to add for a catalog tool the census lacks."""
    example = {tool: {"origin": "worker", "toolset": toolset, "writes": writes, "tests": []}}
    return (
        f'  Add this under "tools" in {CENSUS_RELATIVE} (or run mcp_census.py --executable '
        f"APP --update), then list the tool in an outcome:\n  {json.dumps(example)}\n"
        f"{outcome_help(tool)}"
    )


def check_outcomes(census: dict[str, Any]) -> list[str]:
    """Check every outcome's shape, classification, tools and disposition."""
    tools = census["tools"]
    problems: list[str] = []
    seen: set[str] = set()
    for outcome in census["outcomes"]:
        identifier = outcome.get("id")
        if identifier in seen:
            problems.append(f"outcome {identifier}: duplicated")
        seen.add(identifier)
        problems.extend(_outcome_problems(outcome, tools))
    delivered = {tool for outcome in census["outcomes"] for tool in outcome.get("tools", [])}
    problems.extend(
        f"tool {tool}: not delivered by any outcome.\n{outcome_help(tool)}"
        for tool in sorted(set(tools) - delivered)
    )
    return problems


def _test_exists(identifier: str, root: Path) -> bool:
    path, _, name = identifier.partition("::")
    target = root / path
    if not target.is_file():
        return False
    return not name or re.search(rf"\b{re.escape(name)}\b", target.read_text("utf-8")) is not None


def check_tests(census: dict[str, Any], root: Path = ROOT) -> list[str]:
    """Require every listed test and journey to exist."""
    problems: list[str] = []
    for tool, entry in census["tools"].items():
        problems.extend(
            f"tool {tool}: listed test {test} does not exist"
            for test in entry["tests"]
            if not _test_exists(test, root)
        )
    for outcome in census["outcomes"]:
        problems.extend(
            f"outcome {outcome['id']}: journey {test} does not exist"
            for test in outcome.get("journey", [])
            if not _test_exists(test, root)
        )
    return problems


def _quoted_names(text: str, pattern: str) -> set[str]:
    return set(re.findall(pattern, text))


def check_adapter_sources(census: dict[str, Any], root: Path = ROOT) -> list[str]:
    """Compare adapter-owned and live-bridge tool names with their source files."""
    sources = {
        "adapter": (
            root / "tools/mcp/src/toolsets.ts",
            r'name: "(toolsets\.[a-z_]+)"',
        ),
        "live_bridge": (
            root / "tools/mcp/src/live-protocol.ts",
            r'^\s+"(live\.[a-z_.]+)",$',
        ),
    }
    problems: list[str] = []
    for origin, (path, pattern) in sources.items():
        text = path.read_text(encoding="utf-8")
        actual = set(re.findall(pattern, text, re.MULTILINE))
        listed = {name for name, entry in census["tools"].items() if entry["origin"] == origin}
        problems.extend(
            f"{origin} tool {name}: in {path.name} but not in the census"
            for name in sorted(actual - listed)
        )
        problems.extend(
            f"{origin} tool {name}: in the census but not in {path.name}"
            for name in sorted(listed - actual)
        )
    return problems


def _readme_tool_mentions(census: dict[str, Any], readme: str) -> set[str]:
    suffixes = _format_tokens(census) | {t.lower() for t in UNSUPPORTED_FORMAT_TOKENS}
    suffixes |= FILE_SUFFIXES
    ignored = set(census["non_tool_tokens"])
    return {
        token
        for token in TOOL_NAME.findall(readme)
        if token.rsplit(".", 1)[-1] not in suffixes and token not in ignored
    }


def _format_tokens(census: dict[str, Any]) -> set[str]:
    return {token.lower() for entry in census["formats"] for token in entry["tokens"]}


def readme_toolsets(readme: str) -> set[str]:
    """Return every toolset the README names."""
    names = set(TOOLSET_PROSE.findall(readme))
    for flag in TOOLSET_FLAG.findall(readme):
        names.update(name for name in flag.split(",") if name and name != "all")
    return names


def readme_format_tokens(census: dict[str, Any], readme: str) -> set[str]:
    """Return the file-format tokens the README claims, in upper case."""
    known = {token.upper() for entry in census["formats"] for token in entry["tokens"]}
    universe = known | UNSUPPORTED_FORMAT_TOKENS
    found = {word for word in UPPERCASE.findall(readme) if word in universe}
    for literal in FORMAT_LITERAL.findall(readme):
        found.update(token for token in _literal_tokens(census, literal) if token in universe)
    for extension in EXTENSION.findall(readme):
        found.update({extension.upper()} & universe)
    for token in TOOL_NAME.findall(readme):
        found.update({token.rsplit(".", 1)[-1].upper()} & universe)
    return found


def _literal_tokens(census: dict[str, Any], literal: str) -> set[str]:
    tokens = {literal.upper()}
    for entry in census["formats"]:
        if literal == entry["id"]:
            tokens.update(token.upper() for token in entry["tokens"])
    return tokens


def check_readme(census: dict[str, Any], readme: str) -> list[str]:
    """Compare README claims with the census, which the live check ties to the catalog."""
    problems: list[str] = []
    tools = set(census["tools"])
    mentioned = _readme_tool_mentions(census, readme)
    problems.extend(
        f"README names tool {name}, which the census lacks" for name in sorted(mentioned - tools)
    )
    problems.extend(f"README omits tool {name}" for name in sorted(tools - mentioned))
    toolsets = {
        entry["toolset"] for entry in census["tools"].values() if entry["origin"] == "worker"
    }
    problems.extend(
        f"README names toolset {name}, which the census lacks"
        for name in sorted(readme_toolsets(readme) - toolsets)
    )
    claimed = readme_format_tokens(census, readme)
    known = {token.upper() for entry in census["formats"] for token in entry["tokens"]}
    problems.extend(
        f"README names file format {name}, which the census lacks"
        for name in sorted(claimed - known)
    )
    return problems


def check_numeric_claims(census: dict[str, Any], readme: str, root: Path = ROOT) -> list[str]:
    """Compare the README's protocol, tool-limit and FX profile claims with their sources."""
    problems: list[str] = []
    claims = census["claims"]
    protocol = re.search(r"native automation protocol version (\d+)", readme)
    if protocol is None or int(protocol.group(1)) != claims["protocol_version"]:
        problems.append("README protocol version differs from the census")
    limit = re.search(r"at most (\d+) native tools", readme)
    source = (root / "tools/mcp/src/toolsets.ts").read_text(encoding="utf-8")
    exposed = re.search(r"MAX_EXPOSED_TOOLS = (\d+)", source)
    if limit is None or exposed is None or limit.group(1) != exposed.group(1):
        problems.append("README exposed-tool limit differs from the adapter source")
    fx = re.search(r"profile `([a-z0-9-]+)`, admits (\d+) builtins", readme)
    if fx is None or (fx.group(1), int(fx.group(2))) != (
        claims["fx_profile"],
        claims["fx_builtins"],
    ):
        problems.append("README FX profile or builtin count differs from the census")
    return problems


def check_readme_index(census: dict[str, Any], readme: str) -> list[str]:
    """Require the README tool index to match the census."""
    if replace_region(readme, render_readme_index(census)) != readme:
        return ["README tool index is stale; run mcp_census.py --update"]
    return []


def check_document(census: dict[str, Any], document: str) -> list[str]:
    """Require the committed census document to match the data file byte for byte."""
    if document != render_document(census, report(census)):
        return ["census document is stale; run mcp_census.py --update"]
    return []


def report(census: dict[str, Any]) -> dict[str, Any]:
    """Count inventoried, implemented and tested outcomes and tools."""
    tools = census["tools"]
    by_class = Counter(outcome["classification"] for outcome in census["outcomes"])
    implemented = [o for o in census["outcomes"] if o["disposition"]["kind"] == "implemented"]
    tested = [o for o in implemented if _outcome_tested(o, tools)]
    untested = sorted(name for name, entry in tools.items() if not entry["tests"])
    return {
        "inventoried": len(census["outcomes"]),
        "by_classification": {name: by_class.get(name, 0) for name in CLASSIFICATIONS},
        "implemented": len(implemented),
        "tested": len(tested),
        "tools": len(tools),
        "tools_without_tests": untested,
        "platforms": {"summary": _platform_summary(census["platforms"], len(tested))},
    }


def _platform_summary(platforms: dict[str, Any], tested: int) -> str:
    qualified = " and ".join(platforms["qualified"])
    others = ", ".join(platforms["not_recorded"])
    return f"{qualified} only so far ({tested} tested outcomes); not recorded for {others}"


def _outcome_tested(outcome: dict[str, Any], tools: dict[str, Any]) -> bool:
    return all(tools[tool]["tests"] for tool in outcome["tools"])


def format_report(counts: dict[str, Any]) -> str:
    """Print the three measures separately."""
    classes = ", ".join(f"{name} {count}" for name, count in counts["by_classification"].items())
    platforms = counts["platforms"]
    return "\n".join(
        [
            f"inventoried outcomes: {counts['inventoried']} ({classes})",
            f"implemented outcomes: {counts['implemented']}",
            f"tested outcomes: {counts['tested']} of {counts['implemented']} implemented",
            f"tools: {counts['tools']}; without a listed test: "
            f"{len(counts['tools_without_tests'])}",
            f"platform-qualified outcomes: {platforms['summary']}",
        ]
    )


def static_problems(
    census: dict[str, Any],
    readme: str,
    document: str,
    root: Path = ROOT,
) -> list[str]:
    """Run every check that needs no worker."""
    problems = check_outcomes(census)
    problems += check_tests(census, root)
    problems += check_adapter_sources(census, root)
    problems += check_readme(census, readme)
    problems += check_numeric_claims(census, readme, root)
    problems += check_readme_index(census, readme)
    if not check_outcomes(census):
        problems += check_document(census, document)
    return problems


def _evidence_problem(entry: dict[str, Any], tools: dict[str, dict[str, Any]]) -> str | None:
    evidence = entry["evidence"]
    tool = tools.get(evidence["tool"])
    if tool is None:
        return f"format {entry['id']}: evidence tool {evidence['tool']} is not in the catalog"
    if evidence["kind"] == "tool":
        return None
    if evidence["kind"] == "description":
        found = evidence["text"] in tool["description"]
    else:
        schema = tool["inputSchema"]["properties"].get(evidence["property"], {})
        found = evidence["value"] in schema.get("enum", [])
    return None if found else f"format {entry['id']}: the catalog does not show {evidence}"


def check_live(
    census: dict[str, Any],
    capabilities: dict[str, Any],
    fx_catalog: dict[str, Any],
    readme: str | None,
) -> list[str]:
    """Compare the census and README with the catalog a worker reports.

    Args:
        census: The census data.
        capabilities: The worker's `automation.capabilities` result.
        fx_catalog: The worker's `fx.catalog` result.
        readme: The README text, or None where the checkout has no README; the README
            comparisons are then left to the static check.
    Returns:
        One message per difference.
    """
    tools = {tool["name"]: tool for tool in capabilities["tools"]}
    listed = {name: e for name, e in census["tools"].items() if e["origin"] == "worker"}
    problems = [
        f"tool {name}: in the catalog but not in the census.\n"
        + tool_entry_help(
            name, tools[name]["toolset"], not tools[name]["annotations"]["readOnlyHint"]
        )
        for name in sorted(set(tools) - set(listed))
    ]
    problems += [
        f"tool {name}: in the census but not in the catalog"
        for name in sorted(set(listed) - set(tools))
    ]
    for name in sorted(set(tools) & set(listed)):
        problems.extend(_tool_problems(name, tools[name], listed[name]))
    problems += _toolset_problems(census, capabilities, readme or "")
    if readme is not None:
        problems += _readme_catalog_problems(census, tools, readme)
    for entry in census["formats"]:
        if entry["state"] == "supported" and (message := _evidence_problem(entry, tools)):
            problems.append(message)
    problems += _catalog_claim_problems(census, capabilities, fx_catalog)
    return problems


def _tool_problems(name: str, tool: dict[str, Any], entry: dict[str, Any]) -> list[str]:
    problems = []
    if tool["toolset"] != entry["toolset"]:
        problems.append(f"tool {name}: toolset {tool['toolset']} differs from the census")
    writes = not tool["annotations"]["readOnlyHint"]
    if writes != entry["writes"]:
        problems.append(f"tool {name}: write annotation differs from the census")
    return problems


def _readme_catalog_problems(
    census: dict[str, Any], tools: dict[str, dict[str, Any]], readme: str
) -> list[str]:
    outside = {name for name, entry in census["tools"].items() if entry["origin"] != "worker"}
    mentioned = _readme_tool_mentions(census, readme)
    problems = [
        f"README names tool {name}, which the catalog lacks"
        for name in sorted(mentioned - set(tools) - outside)
    ]
    problems += [
        f"README omits tool {name}, which the catalog has"
        for name in sorted(set(tools) - mentioned)
    ]
    return problems


def _toolset_problems(
    census: dict[str, Any], capabilities: dict[str, Any], readme: str
) -> list[str]:
    catalog = {toolset["name"] for toolset in capabilities["toolsets"]}
    listed = {entry["toolset"] for entry in census["tools"].values() if entry["origin"] == "worker"}
    problems = [
        f"toolset {name}: in the catalog but not in the census" for name in sorted(catalog - listed)
    ]
    problems += [
        f"toolset {name}: in the census but not in the catalog" for name in sorted(listed - catalog)
    ]
    problems += [
        f"README names toolset {name}, which the catalog lacks"
        for name in sorted(readme_toolsets(readme) - catalog)
    ]
    return problems


def _catalog_claim_problems(
    census: dict[str, Any], capabilities: dict[str, Any], fx_catalog: dict[str, Any]
) -> list[str]:
    claims = census["claims"]
    problems = []
    if capabilities["protocol_version"] != claims["protocol_version"]:
        problems.append("catalog protocol version differs from the census")
    if (fx_catalog["profile"], len(fx_catalog["effects"])) != (
        claims["fx_profile"],
        claims["fx_builtins"],
    ):
        problems.append("catalog FX profile or builtin count differs from the census")
    return problems


def _function_index(tree: ast.AST) -> dict[str, ast.FunctionDef]:
    return {n.name: n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef)}


def _load_python_sources() -> tuple[dict[str, ast.Module], set[str]]:
    modules = {
        path.stem: ast.parse(path.read_text(encoding="utf-8"))
        for path in sorted(SCRIPTS.glob("mcp_*.py"))
        if path.stem not in {"mcp_census", "mcp_census_text"}
    }
    return modules, set(modules)


def _callees(node: ast.AST, module: str, names: set[str]) -> list[tuple[str, str]]:
    found = []
    for call in ast.walk(node):
        if not isinstance(call, ast.Call):
            continue
        function = call.func
        if isinstance(function, ast.Name):
            found.append((module, function.id))
        elif isinstance(function, ast.Attribute) and isinstance(function.value, ast.Name):
            owner = function.value.id
            if owner in names:
                found.append((owner, function.attr))
            elif owner in {"self", "case"}:
                found.append(("mcp_acceptance", function.attr))
    return found


def _reachable(
    start: tuple[str, ast.FunctionDef],
    index: dict[str, dict[str, ast.FunctionDef]],
    names: set[str],
) -> list[ast.FunctionDef]:
    visited: set[tuple[str, str]] = set()
    order: list[ast.FunctionDef] = []
    pending = [(start[0], start[1].name)]
    while pending:
        key = pending.pop()
        function = index.get(key[0], {}).get(key[1])
        if key in visited or function is None:
            continue
        visited.add(key)
        order.append(function)
        pending.extend(_callees(function, key[0], names))
    return order


def _tool_counts(nodes: list[ast.FunctionDef], tools: set[str]) -> Counter[str]:
    counts: Counter[str] = Counter()
    for function in nodes:
        for node in ast.walk(function):
            if isinstance(node, ast.Constant) and isinstance(node.value, str):
                if node.value in tools:
                    counts[node.value] += 1
    return counts


def acceptance_coverage(tools: set[str]) -> dict[str, Counter[str]]:
    """Map each acceptance test to the tool names its call graph mentions."""
    modules, names = _load_python_sources()
    index = {name: _function_index(tree) for name, tree in modules.items()}
    coverage = {}
    for name, function in index["mcp_acceptance"].items():
        if name.startswith("test_"):
            reached = _reachable(("mcp_acceptance", function), index, names)
            coverage[f"{ACCEPTANCE}::{name}"] = _tool_counts(reached, tools)
    return coverage


def _literal_counts(path: Path, tools: set[str]) -> Counter[str]:
    text = path.read_text(encoding="utf-8")
    counts: Counter[str] = Counter()
    for match in re.finditer(r'"([a-z_]+(?:\.[a-z_]+)+)"', text):
        if match.group(1) in tools:
            counts[match.group(1)] += 1
    return counts


def _matrix_candidates(tool_names: set[str]) -> dict[str, Counter[str]]:
    """Map each revision-matrix acceptance test to the writing tools its group covers."""
    matrix = json.loads(REVISION_MATRIX_PATH.read_text(encoding="utf-8"))
    coverage: dict[str, Counter[str]] = {}
    for key, entry in matrix["tools"].items():
        tool = entry.get("tool", key)
        test = f"{ACCEPTANCE}::test_revision_matrix_{entry['group']}"
        if tool in tool_names:
            coverage.setdefault(test, Counter())[tool] += 1
    return coverage


def _file_candidates(tool_names: set[str]) -> dict[str, dict[str, Counter[str]]]:
    kinds: dict[str, dict[str, Counter[str]]] = {"native": {}, "adapter": {}, "script": {}}
    for path in sorted(NATIVE_TESTS.glob("automation_*_test.cpp")):
        kinds["native"][str(path.relative_to(ROOT))] = _literal_counts(path, tool_names)
    for path in sorted(ADAPTER_SOURCES.glob("*.test.ts")):
        kinds["adapter"][str(path.relative_to(ROOT))] = _literal_counts(path, tool_names)
    for name in SCRIPT_EVIDENCE:
        path = SCRIPTS / name
        kinds["script"][str(path.relative_to(ROOT))] = _literal_counts(path, tool_names)
    return kinds


def derive_tests(census: dict[str, Any]) -> dict[str, list[str]]:
    """List, for each tool, the tests that call it most, from the repository sources."""
    names = set(census["tools"])
    candidates = _file_candidates(names)
    candidates["acceptance"] = acceptance_coverage(names)
    candidates["matrix"] = _matrix_candidates(names)
    result: dict[str, list[str]] = {}
    for tool, entry in census["tools"].items():
        chosen: list[str] = []
        for kind, cap in TEST_CAPS.items():
            ranked = sorted(
                ((counts[tool], test) for test, counts in candidates[kind].items() if counts[tool]),
                key=lambda pair: (-pair[0], pair[1]),
            )
            chosen.extend(
                test for _count, test in ranked[:cap] if _adapter_allowed(kind, test, entry)
            )
        result[tool] = chosen
    return result


def _adapter_allowed(kind: str, test: str, entry: dict[str, Any]) -> bool:
    if kind != "adapter" or entry["origin"] != "worker":
        return True
    return test == NATIVE_ADAPTER_TEST


def refresh_tools(census: dict[str, Any], capabilities: dict[str, Any] | None) -> None:
    """Rewrite derived tool fields: the catalog's toolset and write flag, then tests."""
    if capabilities is not None:
        for tool in capabilities["tools"]:
            entry = census["tools"].setdefault(
                tool["name"], {"origin": "worker", "toolset": "", "writes": False, "tests": []}
            )
            entry["toolset"] = tool["toolset"]
            entry["writes"] = not tool["annotations"]["readOnlyHint"]
    census["tools"] = dict(sorted(census["tools"].items()))
    for tool, tests in derive_tests(census).items():
        census["tools"][tool]["tests"] = tests


def fetch_catalog(executable: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    """Start a worker and read its tool catalog and FX catalog."""
    from mcp_worker import Worker, joined_capabilities

    with tempfile.TemporaryDirectory(prefix="census-") as scratch:
        base = Path(scratch)
        for name in ("workspace", "inputs"):
            (base / name).mkdir()
        worker = Worker(executable, base / "workspace", base / "inputs", base / "worker.log")
        try:
            capabilities = joined_capabilities(
                lambda params: worker.call("automation.capabilities", params)["result"]
            )
            fx_catalog = worker.call("fx.catalog", {})["result"]
        finally:
            worker.close()
    return capabilities, fx_catalog


def update(census: dict[str, Any], capabilities: dict[str, Any] | None) -> None:
    """Rewrite the census data file, the README tool index and the census document."""
    refresh_tools(census, capabilities)
    CENSUS_PATH.write_text(dump_census(census), encoding="utf-8")
    readme = README_PATH.read_text(encoding="utf-8")
    README_PATH.write_text(replace_region(readme, render_readme_index(census)), encoding="utf-8")
    DOCUMENT_PATH.write_text(render_document(census, report(census)), encoding="utf-8")


def main() -> int:
    """Check the census, optionally against a live worker, or rewrite its derived files."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, help="Compare with this worker's catalog")
    parser.add_argument("--update", action="store_true", help="Rewrite derived files")
    args = parser.parse_args()
    census = load_census()
    catalog = fetch_catalog(args.executable.resolve(strict=True)) if args.executable else None
    if args.update:
        update(census, catalog[0] if catalog else None)
        census = load_census()
    readme = README_PATH.read_text(encoding="utf-8")
    problems = static_problems(census, readme, DOCUMENT_PATH.read_text(encoding="utf-8"))
    if catalog:
        problems += check_live(census, catalog[0], catalog[1], readme)
    print(format_report(report(census)))
    for problem in problems:
        print(f"census: {problem}")
    return int(bool(problems))


if __name__ == "__main__":
    sys.exit(main())
