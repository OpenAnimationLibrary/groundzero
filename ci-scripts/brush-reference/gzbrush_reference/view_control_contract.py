"""Pure source view-rotation readback contract; no application imports.

Source UI serialization can wrap a negative angle into [0, 360). Compare the
same orientation circularly without changing the measured acquisition bounds.
"""
import math


def rotation_matches(observed, requested, *, resource=False):
    """Accept finite numeric angles within the existing source readback guard."""
    if (type(resource) is not bool or type(observed) not in (int, float) or
            type(requested) not in (int, float)):
        return False
    try:
        if not math.isfinite(observed) or not math.isfinite(requested):
            return False
        # Reduce each operand before subtraction so two finite very large
        # angles cannot overflow their difference to infinity.
        error = abs(math.remainder(math.remainder(observed, 360.0) -
                                   math.remainder(requested, 360.0), 360.0))
    except (OverflowError, ValueError, TypeError):
        return False
    return error <= (.001 if resource else 1e-5)
