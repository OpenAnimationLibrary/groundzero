"""The parts of the property model's scene that more than one operation module touches."""

from __future__ import annotations

from dataclasses import dataclass, field

Drawing = tuple[int, str]
OPACITY_LEVELS = 255


class ModelError(ValueError):
    """A step that cannot be applied to the model's state, as in a hand-edited sequence."""


@dataclass
class Column:
    """A level column: its settings and its cells, keyed by frame."""

    name: str
    visible: bool = True
    locked: bool = False
    opacity: int = OPACITY_LEVELS
    cells: dict[int, tuple[str, Drawing]] = field(default_factory=dict)


@dataclass
class Camera:
    """A camera of the sheet; the first one is unnamed in the editor and shows as Camera1."""

    name: str
    size: dict[str, float]
