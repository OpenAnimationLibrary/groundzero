"""Compare the capability census and README with the catalog of the running worker."""

from __future__ import annotations

from typing import TYPE_CHECKING

import mcp_census

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def census_matches_live_catalog(case: NativeAcceptance) -> None:
    """Fail on any drift between the census, the README and the catalog this worker reports."""
    census = mcp_census.load_census()
    path = mcp_census.README_PATH
    readme = path.read_text(encoding="utf-8") if path.is_file() else None
    capabilities = case.success("automation.capabilities", {})
    effects = case.success("fx.catalog", {})
    problems = mcp_census.check_live(census, capabilities, effects, readme)
    case.assertEqual(problems, [], "\n".join(problems))
