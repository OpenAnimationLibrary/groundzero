"""Qualify refusals the worker already enforces: numbers, nesting, keys, suffixes and enums."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

MAX_SAFE_INTEGER = 2**53 - 1


def _create_text(width: str, operation: str = "limit-create") -> str:
    return (
        '{"id":"limit","method":"scene.create","params":'
        f'{{"name":"Limit","operation_id":"{operation}","width_px":{width}}}}}'
    )


def _create_params(members: str) -> str:
    return '{"id":"dup","method":"scene.create","params":{' + members + "}}"


def _send(case: NativeAcceptance, text: str) -> dict[str, Any]:
    return case.worker.exchange(text.encode())


def _no_documents(case: NativeAcceptance) -> None:
    case.assertEqual(case.success("automation.list_documents", {})["documents"], [])
    case.assertEqual(case.success("automation.capabilities", {})["protocol_version"], 2)


def _integer_properties(schema: Any, path: str) -> list[tuple[str, dict[str, Any]]]:
    found: list[tuple[str, dict[str, Any]]] = []
    if isinstance(schema, dict):
        if schema.get("type") == "integer":
            found.append((path, schema))
        for key, value in schema.items():
            found.extend(_integer_properties(value, f"{path}.{key}"))
    elif isinstance(schema, list):
        for index, value in enumerate(schema):
            found.extend(_integer_properties(value, f"{path}[{index}]"))
    return found


def integers_beyond_exact_precision_are_refused(case: NativeAcceptance) -> None:
    """Refuse integers a JSON double cannot hold exactly instead of rounding them.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    for literal in (
        "9007199254740992",
        "9007199254740993",
        "-9007199254740993",
        "18446744073709551616",
        "1e30",
    ):
        response = _send(case, _create_text(literal))
        case.assertEqual(response["id"], "limit", literal)
        case.assertEqual(response["error"]["code"], "invalid_params", literal)
    _no_documents(case)
    tools = case.success("automation.capabilities", {})["tools"]
    integers = [
        found for tool in tools for found in _integer_properties(tool["inputSchema"], tool["name"])
    ]
    case.assertGreater(len(integers), 100)
    for path, schema in integers:
        with case.subTest(path=path):
            case.assertGreaterEqual(schema.get("minimum", -(2**63)), -MAX_SAFE_INTEGER)
            case.assertLessEqual(schema.get("maximum", 2**63), MAX_SAFE_INTEGER)


def non_finite_numbers_are_refused_before_dispatch(case: NativeAcceptance) -> None:
    """Refuse NaN, infinities and overflowing literals as unparsable requests.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    for literal in ("NaN", "Infinity", "-Infinity", "1e999", "-1e999", "1e400"):
        response = _send(case, _create_text(literal))
        case.assertEqual(response["error"]["code"], "parse_error", literal)
        case.assertIsNone(response["id"], literal)
    for value in (32.5, "32", True, None, [32]):
        error = case.failure(
            "scene.create",
            case.write_params(None, {"name": "Kind", "width_px": value}),
        )
        case.assertEqual(error["code"], "invalid_params", repr(value))
    _no_documents(case)


def deep_nesting_is_refused_without_stopping_the_worker(case: NativeAcceptance) -> None:
    """Keep deeply nested values typed: bounded by the parser or by the unknown-field check.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    for depth in (31, 32, 33, 100):
        nested = "[" * depth + "]" * depth
        text = (
            '{"id":"deep","method":"scene.create","params":'
            f'{{"name":"Deep","operation_id":"deep-{depth}","extra":{nested}}}}}'
        )
        response = _send(case, text)
        case.assertEqual(response["id"], "deep", depth)
        case.assertEqual(response["error"]["code"], "invalid_params", depth)
        case.assertIn("Unknown field: extra", response["error"]["message"])
    for depth in (1100, 20000):
        nested = "[" * depth + "]" * depth
        text = f'{{"id":"deep","method":"automation.capabilities","params":{{"pad":{nested}}}}}'
        response = _send(case, text)
        case.assertEqual(response["error"]["code"], "parse_error", depth)
        case.assertIsNone(response["id"])
    _no_documents(case)


def duplicate_keys_are_refused_before_dispatch(case: NativeAcceptance) -> None:
    """Refuse a repeated key at any depth, however it is spelled, before it can act.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    requests = {
        "id": '{"id":"dup","id":"dup","method":"automation.capabilities","params":{}}',
        "method": '{"id":"dup","method":"scene.create","method":"scene.create","params":{}}',
        "params": '{"id":"dup","method":"scene.create","params":{"operation_id":"d0"},"params":{}}',
        "argument": _create_params('"operation_id":"d1","name":"A","name":"B"'),
        "escaped": _create_params('"operation_id":"d2","name":"A","\\u006eame":"B"'),
        "nested": _create_params('"operation_id":"d3","e":[{"k":1},{"j":1,"j":2}]'),
        "width": _create_params('"operation_id":"d4","width_px":1,"width_px":2'),
    }
    for label, text in requests.items():
        response = _send(case, text)
        case.assertEqual(response["error"]["code"], "invalid_params", label)
        case.assertIn("Duplicate JSON object keys", response["error"]["message"], label)
        case.assertIsNone(response["id"], label)
    _no_documents(case)
    accepted = _send(case, _create_params('"operation_id":"d5","name":"name","camera":"Name"'))
    case.assertEqual(accepted["error"]["code"], "invalid_params")
    case.assertIn("Unknown field", accepted["error"]["message"])
    created = _send(case, _create_params('"operation_id":"d6","name":"Distinct keys"'))
    case.assertEqual(created["result"]["name"], "Distinct keys")


def non_lowercase_drawing_suffixes_are_refused(case: NativeAcceptance) -> None:
    """Refuse every suffix outside zero to eight lowercase ASCII letters, changing nothing.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Suffixes")
    level = case.mutate("level.create", document, name="Vector", format="vector")
    column = case.mutate("column.create", document, name="Column")
    case.mutate(
        "drawing.create",
        document,
        level_id=level["level_id"],
        format="vector",
        drawing_id={"number": 1, "suffix": "a"},
    )
    before = case.success("scene.summary", {"document_id": document})
    suffixes = ("A", "aB", "\u00e9", "\uff41", "a1", "a-", "a b", " a", "a\x00", "\u202ea")
    suffixes += ("abcdefghi", "a" * 100)
    for suffix in suffixes:
        drawing = {"number": 1, "suffix": suffix}
        target = {"level_id": level["level_id"], "drawing_id": drawing}
        label = repr(suffix)
        error = case.failure("drawing.inspect", {"document_id": document, **target})
        case.assertEqual(error["code"], "invalid_params", label)
        error = case.failure(
            "drawing.create", case.write_params(document, {**target, "format": "vector"})
        )
        case.assertEqual(error["code"], "invalid_params", label)
        error = case.failure(
            "column.expose",
            case.write_params(
                document,
                {**target, "column_id": column["column_id"], "start_frame": 0, "length": 1},
            ),
        )
        case.assertEqual(error["code"], "invalid_argument", label)
        case.assertEqual(case.success("scene.summary", {"document_id": document}), before, label)


def _set_curve(case: NativeAcceptance, document: str, effect: str, value: Any) -> dict[str, Any]:
    params = {"fx_id": effect, "parameter": "curveType", "value": value}
    return case.failure("fx.parameter.set", case.write_params(document, params))


def _curve_value(case: NativeAcceptance, document: str, effect: str) -> int:
    result = case.success("fx.parameter.get", {"document_id": document, "fx_id": effect})
    return next(item for item in result["parameters"] if item["name"] == "curveType")["value"]


def enum_parameters_accept_only_native_integers(case: NativeAcceptance) -> None:
    """Refuse menu captions, aliases and near-integers for an enumerated FX parameter.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    document = case.create("Enumerations")
    effect = case.mutate("fx.create", document, type="STD_radialGradientFx", frame_count=2)
    described = case.success(
        "fx.parameter.get", {"document_id": document, "fx_id": effect["fx_id"]}
    )
    captions = next(
        item["schema"]["captions"]
        for item in described["parameters"]
        if item["name"] == "curveType"
    )
    case.assertEqual(captions, ["Ease In-Out", "Linear", "Ease In", "Ease Out"])
    original = _curve_value(case, document, effect["fx_id"])
    revision = case.revisions[document]
    labels = [*captions, *(caption.lower() for caption in captions), "1", "01", "linear "]
    for value in (*labels, 1.5, True, None, [1], {"value": 1}):
        error = _set_curve(case, document, effect["fx_id"], value)
        case.assertEqual(error["code"], "INVALID_PARAMS", repr(value))
    for value in (-1, 4, 1e9):
        error = _set_curve(case, document, effect["fx_id"], value)
        case.assertEqual(error["code"], "OUT_OF_RANGE", repr(value))
    case.assertEqual(case.revisions[document], revision)
    case.assertEqual(_curve_value(case, document, effect["fx_id"]), original)
    changed = case.mutate(
        "fx.parameter.set", document, fx_id=effect["fx_id"], parameter="curveType", value=2
    )
    case.assertEqual(changed["value"], 2)
    case.assertEqual(captions[changed["value"]], "Ease In")


def hidden_fx_parameters_are_neither_listed_nor_writable(case: NativeAcceptance) -> None:
    """Keep a hidden native parameter (the blur SSE switch) out of reads and writes.

    Args:
        case: A fresh native-worker acceptance fixture.
    """
    catalog = case.success("fx.catalog", {})
    entry = next(item for item in catalog["effects"] if item["type"] == "STD_blurFx")
    case.assertEqual([item["name"] for item in entry["parameters"]], ["value"])
    document = case.create("Hidden parameters")
    blur = case.mutate("fx.create", document, type="STD_blurFx")
    listed = case.success("fx.parameter.get", {"document_id": document, "fx_id": blur["fx_id"]})
    case.assertEqual([item["name"] for item in listed["parameters"]], ["value"])
    revision = case.revisions[document]
    for name in ("useSSE", "usesse", "USESSE"):
        for method, extra in (("fx.parameter.set", {}), ("fx.parameter.set_key", {"frame": 0})):
            params = {"fx_id": blur["fx_id"], "parameter": name, "value": False, **extra}
            error = case.failure(method, case.write_params(document, params))
            case.assertEqual(error["code"], "UNKNOWN_PARAMETER", f"{method} {name}")
    case.assertEqual(case.revisions[document], revision)
