"""Mark the functions of the case modules that the acceptance suite runs.

`mcp_acceptance.py` adds every marked function of the `mcp_*_cases.py` modules beside it to
`NativeAcceptance` as `test_<name>`, so a new case needs no registration elsewhere.
"""

from __future__ import annotations

from collections.abc import Callable
from types import FunctionType
from typing import Any, TypeVar, overload

Case = TypeVar("Case", bound=FunctionType)
MARK = "acceptance_mark"


@overload
def acceptance_case(function: Case, /) -> Case: ...


@overload
def acceptance_case(*, name: str = "", worker_only: str = "") -> Callable[[Case], Case]: ...


def acceptance_case(
    function: FunctionType | None = None, /, *, name: str = "", worker_only: str = ""
) -> Any:
    """Mark a function taking the acceptance fixture as a case of the suite.

    Args:
        function: The case, when the decorator is used without arguments.
        name: The test name after `test_`, when it differs from the function's name.
        worker_only: Why no MCP client can express the case; through the SDK client it is
            then skipped with this reason.
    """

    def mark(case: FunctionType) -> FunctionType:
        setattr(case, MARK, {"name": name or case.__name__, "worker_only": worker_only})
        return case

    return mark if function is None else mark(function)
