"""Cell interval operations for the property model, written from the editor's definitions.

A column is a list of rows; a row is a cell `(level, number, suffix)` or None for a blank. Each
operation takes the rows of one column and returns its rows afterwards. Rows after an interval
move with it when the operation changes the interval's length, as when time is inserted or
deleted; operations that keep it leave them where they are.

Retime kinds, with an interval `s` of rows:

- reverse: the rows in the opposite order.
- swing: `s`, then the rows of `s` backwards without its last row repeated: a b c is a b c b a.
- step n: every row n times. each n: every n-th row from the first (a b c d e, each 2: a c e).
- reframe: the distinct neighbours of the interval, each shown for `step` rows.
- increase_step, decrease_step: every drawing's hold one row longer, or one shorter (at least
  one); blanks are left as they are. reset_step: every hold, blanks too, one row.
- roll_up: the first row moves to the end; roll_down: the last row moves to the start.
- repeat: the interval's rows over and over until the row `until_frame`, inserted.
- time_stretch: the interval re-sampled to `new_length` rows, row i showing source row
  floor(i * length / new_length).
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

Cell = tuple[str, int, str] | None
Rows = list[Cell]
LAST_ROW = 999999


def padded(rows: Rows, length: int) -> Rows:
    """The rows, with blanks added to reach a length."""
    return rows + [None] * (length - len(rows))


def trimmed(rows: Rows) -> Rows:
    """The rows without the blanks at the end."""
    end = len(rows)
    while end and rows[end - 1] is None:
        end -= 1
    return rows[:end]


def to_rows(cells: dict[int, Any]) -> Rows:
    """A column's cells as rows."""
    if not cells:
        return []
    return [cells.get(row) for row in range(max(cells) + 1)]


def to_cells(rows: Rows) -> dict[int, Any]:
    """Rows as a column's cells."""
    return {row: cell for row, cell in enumerate(rows) if cell is not None}


def write_runs(rows: Rows, start: int, runs: list[dict[str, Any]]) -> Rows:
    """Rows after writing runs one after another from a row; a run is cells or blanks."""
    rows = padded(list(rows), start)
    position = start
    for run in runs:
        cell = None if run.get("empty") else (run["level"], run["number"], run["suffix"])
        rows = padded(rows, position + run["length"])
        for row in range(position, position + run["length"]):
            rows[row] = cell
        position += run["length"]
    return trimmed(rows)


def occupied(rows: Rows, start: int, runs: list[dict[str, Any]]) -> bool:
    """Whether a write of runs from a row would land on a cell."""
    end = start + sum(run["length"] for run in runs)
    return any(cell is not None for cell in rows[start:end])


def clear(rows: Rows, start: int, length: int) -> Rows:
    """Rows with an interval blanked and nothing moved."""
    rows = padded(list(rows), start + length)
    rows[start : start + length] = [None] * length
    return trimmed(rows)


def remove(rows: Rows, start: int, length: int) -> Rows:
    """Rows with an interval deleted and the later rows moved up."""
    return trimmed(rows[:start] + rows[start + length :])


def insert(rows: Rows, start: int, length: int) -> Rows:
    """Rows with blanks inserted; a column that has nothing at or after the row is unchanged."""
    if start >= len(rows):
        return rows
    return rows[:start] + [None] * length + rows[start:]


def copy_block(destination: Rows, source: Rows, start: int, placement: str) -> Rows:
    """The destination rows after a block is put in at a row, inserted or replacing."""
    destination = padded(list(destination), start)
    if placement == "insert":
        return trimmed(destination[:start] + source + destination[start:])
    destination = padded(destination, start + len(source))
    destination[start : start + len(source)] = source
    return trimmed(destination)


def replace_level(rows: Rows, start: int, length: int, old: str, new: str) -> Rows:
    """Rows where the interval's cells of one level show the same drawing of another."""
    out = list(rows)
    for row in range(start, min(start + length, len(out))):
        cell = out[row]
        if cell is not None and cell[0] == old:
            out[row] = (new, cell[1], cell[2])
    return out


def replace_pairs(rows: Rows, start: int, length: int, pairs: list[tuple[Cell, Cell]]) -> Rows:
    """Rows where the interval's cells that equal a pair's first show its second."""
    lookup = dict(pairs)
    out = list(rows)
    for row in range(start, min(start + length, len(out))):
        if out[row] in lookup:
            out[row] = lookup[out[row]]
    return out


def _runs(segment: Rows) -> list[tuple[Cell, int]]:
    runs: list[tuple[Cell, int]] = []
    for cell in segment:
        if runs and runs[-1][0] == cell:
            runs[-1] = (cell, runs[-1][1] + 1)
        else:
            runs.append((cell, 1))
    return runs


def _each(segment: Rows, factor: int) -> Rows:
    if len(segment) < factor:
        return segment
    return segment[::factor]


def _stretch(segment: Rows, new_length: int) -> Rows:
    return [segment[index * len(segment) // new_length] for index in range(new_length)]


def _repeat(segment: Rows, copies: int) -> Rows:
    return [segment[index % len(segment)] for index in range(copies)]


RETIMES: dict[str, Callable[[Rows, dict[str, Any]], Rows]] = {
    "reverse": lambda segment, op: segment[::-1],
    "swing": lambda segment, op: segment + segment[-2::-1],
    "step": lambda segment, op: [cell for cell in segment for _ in range(op["factor"])],
    "each": lambda segment, op: _each(segment, op["factor"]),
    "reframe": lambda segment, op: [cell for cell, _ in _runs(segment) for _ in range(op["step"])],
    "increase_step": lambda segment, op: [
        cell for cell, size in _runs(segment) for _ in range(size if cell is None else size + 1)
    ],
    "decrease_step": lambda segment, op: [
        cell
        for cell, size in _runs(segment)
        for _ in range(size if cell is None else max(1, size - 1))
    ],
    "reset_step": lambda segment, op: [cell for cell, _ in _runs(segment)],
    "roll_up": lambda segment, op: segment[1:] + segment[:1],
    "roll_down": lambda segment, op: segment[-1:] + segment[:-1],
    "repeat": lambda segment, op: _repeat(segment, op["until_frame"] - op["start"] + 1),
    "time_stretch": lambda segment, op: _stretch(segment, op["new_length"]),
}


def retimed(segment: Rows, operation: dict[str, Any]) -> Rows:
    """The rows of an interval after a retime operation."""
    return RETIMES[operation["kind"]](segment, operation)


def retime(rows: Rows, start: int, length: int, operation: dict[str, Any]) -> Rows:
    """Rows after a retime of an interval; the rows after it follow its new length."""
    rows = padded(list(rows), start + length)
    segment = retimed(rows[start : start + length], {**operation, "start": start})
    return trimmed(rows[:start] + segment + rows[start + length :])


def retime_refusal(length: int, start: int, operation: dict[str, Any]) -> str | None:
    """The refusal a correct worker gives a retime that its own arguments make impossible."""
    last = start + length - 1
    if operation["kind"] == "repeat" and operation["until_frame"] <= last:
        return "invalid_params/until_frame"
    if operation["kind"] == "swing" and length < 2:
        return "invalid_params/swing_length"
    return None
