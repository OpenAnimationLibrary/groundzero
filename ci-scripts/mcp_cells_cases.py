"""Cell edit cases: explicit runs, clear versus delete-time, insert, copy and move, and
replacement, each all or nothing over up to 64 columns and read back after a reopen."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_sheet_record_cases import save_and_reopen

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance

ONE = {"number": 1, "suffix": ""}
ONE_A = {"number": 1, "suffix": "a"}
TWO = {"number": 2, "suffix": ""}


def _fixture(case: NativeAcceptance, columns: int = 2) -> tuple[str, str, list[str]]:
    document = case.create("Cells")
    level = case.mutate("level.create", document, name="Ink", format="vector")["level_id"]
    for drawing in (ONE, ONE_A, TWO):
        case.mutate("drawing.create", document, level_id=level, drawing_id=drawing, format="vector")
    ids = [
        case.mutate("column.create", document, name=f"C{index}")["column_id"]
        for index in range(columns)
    ]
    return document, level, ids


def _run(level: str, drawing: dict[str, Any], length: int) -> dict[str, Any]:
    return {"level_id": level, "drawing_id": drawing, "length": length}


def _blank(length: int) -> dict[str, Any]:
    return {"empty": True, "length": length}


def _rows(case: NativeAcceptance, document: str, column: str, length: int) -> list:
    cells = case.success(
        "column.cells",
        {"document_id": document, "column_id": column, "start_frame": 0, "length": length},
    )["cells"]
    return [
        None if cell["empty"] else (cell["drawing_id"]["number"], cell["drawing_id"]["suffix"])
        for cell in cells
    ]


def _twos(level: str) -> list[dict[str, Any]]:
    return [
        _run(level, ONE, 2),
        _run(level, ONE_A, 2),
        _blank(2),
        _run(level, TWO, 2),
    ]


def _counting(case: NativeAcceptance, document: str, level: str, column: str) -> None:
    runs = [_run(level, (ONE, ONE_A, TWO)[row % 3], 1) for row in range(10)]
    case.mutate(
        "cells.set",
        document,
        columns=[{"column_id": column, "start_frame": 0, "runs": runs}],
        overwrite=False,
    )


def _numbers(rows: list) -> list:
    return [None if row is None else row[0] * 10 + len(row[1]) for row in rows]


@acceptance_case
def cells_set_writes_runs_on_twos_and_reopens(case: NativeAcceptance) -> None:
    """Drawings 1, 1a and 2 on twos with a blank, in two columns at once, read back row for
    row after a save and a reopen through both loaders (SCN-07 #1)."""
    document, level, (first, second) = _fixture(case)
    result = case.mutate(
        "cells.set",
        document,
        columns=[
            {"column_id": first, "start_frame": 0, "runs": _twos(level)},
            {"column_id": second, "start_frame": 4, "runs": _twos(level)},
        ],
        overwrite=False,
    )
    case.assertEqual(result["cells_changed"], 12)
    case.assertEqual(
        [row["cell_range_after"] for row in result["columns"]],
        [{"first_frame": 0, "last_frame": 7}, {"first_frame": 4, "last_frame": 11}],
    )
    case.assertFalse(result["curves_moved"])
    expected = [(1, ""), (1, ""), (1, "a"), (1, "a"), None, None, (2, ""), (2, "")]
    case.assertEqual(_rows(case, document, first, 8), expected)
    case.assertEqual(_rows(case, document, second, 12), [None] * 4 + expected)
    _saved, reopened = save_and_reopen(case, document, "twos")
    columns = case.success("scene.summary", {"document_id": reopened})["columns"]
    case.assertEqual(_rows(case, reopened, columns[0]["column_id"], 8), expected)


@acceptance_case
def clear_keeps_frame_numbers_and_remove_shifts_them(case: NativeAcceptance) -> None:
    """Clearing a middle range keeps later frame numbers; removing the same range moves the
    later cells up; inserting moves them down; other columns never move (SCN-07 #2)."""
    document, level, (cleared, removed, other) = _fixture(case, 3)
    for column in (cleared, removed, other):
        _counting(case, document, level, column)
    before = _numbers(_rows(case, document, other, 12))
    case.mutate("cells.clear", document, column_ids=[cleared], start_frame=3, length=2)
    case.assertEqual(
        _numbers(_rows(case, document, cleared, 12)), before[:3] + [None, None] + before[5:]
    )
    result = case.mutate("cells.remove", document, column_ids=[removed], start_frame=3, length=2)
    case.assertEqual(result["columns"][0]["rows_delta"], -2)
    case.assertEqual(
        _numbers(_rows(case, document, removed, 12)), before[:3] + before[5:] + [None] * 2
    )
    case.assertEqual(_numbers(_rows(case, document, other, 12)), before)
    inserted = case.mutate("cells.insert", document, column_ids=[removed], start_frame=3, length=2)
    case.assertEqual(inserted["columns"][0]["rows_delta"], 2)
    case.assertEqual(
        _numbers(_rows(case, document, removed, 12)), before[:3] + [None, None] + before[5:]
    )
    save_and_reopen(case, document, "clear-remove")


def _refused(case: NativeAcceptance, method: str, document: str, code: str, **params: Any) -> dict:
    revision = case.revisions[document]
    error = case.handler_failure(method, case.write_params(document, params), code)
    case.assertEqual(case.success("scene.summary", {"document_id": document})["revision"], revision)
    return error


@acceptance_case
def mixed_cell_batches_change_nothing(case: NativeAcceptance) -> None:
    """A batch with one locked, one sound or one occupied column is refused whole and changes
    no column; exact no-ops keep the revision (SCN-07 #3, F4)."""
    document, level, (free, locked) = _fixture(case)
    _counting(case, document, level, free)
    case.mutate("column.update", document, column_id=locked, locked=True)
    batch = [
        {"column_id": free, "start_frame": 20, "runs": [_run(level, TWO, 3)]},
        {"column_id": locked, "start_frame": 20, "runs": [_run(level, TWO, 3)]},
    ]
    error = _refused(case, "cells.set", document, "locked", columns=batch, overwrite=False)
    case.assertEqual(error["details"]["column_ids"], [locked])
    created = case.mutate("audio.column.create", document, name="Voice", volume_permille=1000)
    sound = created["column"]["column_id"]
    error = _refused(
        case,
        "cells.clear",
        document,
        "incompatible_column",
        column_ids=[free, sound],
        start_frame=0,
        length=4,
    )
    case.assertEqual(error["details"]["kind"], "sound")
    case.assertEqual(error["details"]["owner_tool"], "audio.clip.place")
    occupied = [{"column_id": free, "start_frame": 8, "runs": [_run(level, TWO, 3)]}]
    error = _refused(
        case, "cells.set", document, "occupied_cells", columns=occupied, overwrite=False
    )
    case.assertEqual(error["details"]["rows"][0], {"column_id": free, "frame": 8})
    revision = case.revisions[document]
    case.mutate("cells.clear", document, column_ids=[free], start_frame=40, length=5)
    case.mutate("cells.insert", document, column_ids=[free], start_frame=40, length=5)
    case.assertEqual(case.revisions[document], revision)
    same = [{"column_id": free, "start_frame": 0, "runs": [_run(level, ONE, 1)]}]
    result = case.mutate("cells.set", document, columns=same, overwrite=True)
    case.assertEqual((result["cells_changed"], case.revisions[document]), (0, revision))


@acceptance_case
def cell_bounds_are_refused_before_any_change(case: NativeAcceptance) -> None:
    """Rows past 999999, empty or oversized lengths, repeated columns and nested sheets are
    refused with the document unchanged (SCN-07 F1 for the cell tools)."""
    document, level, (column, high) = _fixture(case)
    _counting(case, document, level, column)
    near_end = [{"column_id": high, "start_frame": 999990, "runs": [_run(level, ONE, 5)]}]
    case.mutate("cells.set", document, columns=near_end, overwrite=False)
    late = [{"column_id": column, "start_frame": 999999, "runs": [_run(level, ONE, 2)]}]
    error = _refused(case, "cells.set", document, "invalid_params", columns=late, overwrite=True)
    case.assertEqual(error["details"]["reason"], "frame_range")
    error = _refused(
        case,
        "cells.insert",
        document,
        "invalid_params",
        column_ids=[high],
        start_frame=999990,
        length=10,
    )
    case.assertEqual(error["details"]["reason"], "frame_range")
    _refused(
        case,
        "cells.clear",
        document,
        "invalid_params",
        column_ids=[column, column],
        start_frame=0,
        length=1,
    )
    nested = [{"column_id": column, "level_id": level}]
    error = _refused(
        case,
        "cells.remove",
        document,
        "unsupported_target",
        column_ids=[column],
        start_frame=0,
        length=1,
        sheet_path=nested,
    )
    case.assertEqual(error["details"]["reason"], "nested_sheet_editing")
    for length in (0, 100001):
        case.schema_failure(
            "cells.clear",
            case.write_params(
                document, {"column_ids": [column], "start_frame": 0, "length": length}
            ),
        )
    case.schema_failure(
        "cells.remove",
        case.write_params(document, {"column_ids": [column], "start_frame": -1, "length": 1}),
    )


@acceptance_case
def cell_copies_share_drawings_and_moves_clear_their_source(case: NativeAcceptance) -> None:
    """Copy references a block (the same drawings, not new ones), insert shifts the target,
    a move clears its source, and an inserting move onto its own column is refused."""
    document, level, (source, target) = _fixture(case)
    _counting(case, document, level, source)
    _counting(case, document, level, target)
    original = _numbers(_rows(case, document, source, 12))
    copy = {"column_ids": [source], "start_frame": 0, "length": 3}
    case.mutate(
        "cells.copy",
        document,
        source=copy,
        destination={"column_ids": [target], "start_frame": 5},
        placement="replace",
        source_after="keep",
    )
    case.assertEqual(
        _numbers(_rows(case, document, target, 12)), original[:5] + original[:3] + original[8:]
    )
    result = case.mutate(
        "cells.copy",
        document,
        source=copy,
        destination={"column_ids": [target], "start_frame": 0},
        placement="insert",
        source_after="clear",
    )
    case.assertEqual(result["source_cells_cleared"], 3)
    case.assertEqual(_numbers(_rows(case, document, source, 4)), [None] * 3 + original[3:4])
    case.assertEqual(_numbers(_rows(case, document, target, 3)), original[:3])
    summary = case.success("scene.summary", {"document_id": document})
    case.assertEqual(len(summary["levels"][0]["drawings"]), 3)
    error = _refused(
        case,
        "cells.copy",
        document,
        "invalid_params",
        source={"column_ids": [target], "start_frame": 0, "length": 2},
        destination={"column_ids": [target], "start_frame": 4},
        placement="insert",
        source_after="clear",
    )
    case.assertEqual(error["details"]["reason"], "overlapping_move_insert")
    save_and_reopen(case, document, "copy")


@acceptance_case
def cell_replacement_keeps_timing(case: NativeAcceptance) -> None:
    """Replace a level by another and drawings by pairs inside an interval; timing and cells
    outside it stay; a target without the exposed drawing is refused."""
    document, level, (column, _other) = _fixture(case)
    _counting(case, document, level, column)
    other = case.mutate("level.create", document, name="Paint", format="vector")["level_id"]
    case.mutate("drawing.create", document, level_id=other, drawing_id=ONE, format="vector")
    error = _refused(
        case,
        "cells.replace",
        document,
        "missing_drawing",
        column_ids=[column],
        start_frame=0,
        length=3,
        mapping={"kind": "level", "from_level_id": level, "to_level_id": other},
    )
    case.assertIn(ONE_A, error["details"]["drawings"])
    case.mutate(
        "cells.replace",
        document,
        column_ids=[column],
        start_frame=0,
        length=10,
        mapping={
            "kind": "drawings",
            "pairs": [
                {
                    "from": {"level_id": level, "drawing_id": TWO},
                    "to": {"level_id": other, "drawing_id": ONE},
                },
            ],
        },
    )
    cells = case.success(
        "column.cells",
        {"document_id": document, "column_id": column, "start_frame": 0, "length": 10},
    )["cells"]
    levels = [cell["level_id"] for cell in cells]
    case.assertEqual([index for index, value in enumerate(levels) if value == other], [2, 5, 8])
    case.assertEqual(levels.count(level), 7)
    save_and_reopen(case, document, "replace")
