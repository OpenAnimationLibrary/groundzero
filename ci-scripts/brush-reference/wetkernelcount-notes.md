# Dulling count and normalization discriminators

The first 185 cases retain their names and order. The next 42 cases are original
two-dab masks, appended to the same bounded acquisition group. They do not change
candidate eligibility, comparison thresholds, or the sampling implementation.

Existing captures identify missing endpoint samples at extent 28 (nominal index
81) and extent 32 (nominal index 100). Both neighboring half-rounding locations
of index 100 are absent. The 80th sample at extent 16 remains present. However,
subtracting one sample globally contradicts captures at extents 40 and 48.
Single-pixel impulses alone have poor denominator resolution in 8-bit output.

The new masks use extents 28, 32, 40 and 48 with the same source diameter/radius
combinations as the earlier evidence. At each extent, three explicit point sets
contain the nominal first quarter, just under half, and three quarters of the
sample coordinates. Each set is paired with its exact complement over the opaque
pickup backdrop. The points are stored explicitly; their construction is a
hypothesis for selecting informative pixels, not a source-engine assertion.

- Broad gray ratios distinguish dividing by the attempted count from dividing by
  the accepted count, where isolated impulses round to the same byte.
- Complement pairs distinguish channel rounding from an omitted endpoint or
  non-unit normalization. They also expose any asymmetric black/white handling.
- Adjacent isolated endpoint coordinates, nominal indices N-2 through N+2,
  distinguish a changed stopping count from a shifted window. Previously acquired
  identical points are reused, rather than duplicated.
- The existing row/column masks constrain geometry independently. A proposed
  rule must explain those masks, both polarities, endpoint probes, and earlier
  scale captures together. Passing one extent does not justify a size exception.

All masks keep alpha opaque in the pickup region and transparent beyond its
existing boundary. Source and candidate render the same explicit pixels. The
source capture remains deterministic and repeated; no random mask generation or
external brush assets are involved. Extents beyond current source eligibility
remain acquisition-only until a general equation is independently qualified.
