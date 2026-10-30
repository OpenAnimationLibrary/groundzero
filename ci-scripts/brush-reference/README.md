# Original brush reference fixtures

The `Raster Brush Reference` workflow runs the official pinned Krita 5.3.4.1
Linux application in an isolated CI profile. It generates five original presets:
Pixel, legacy Smear, legacy Dulling, new Smear and new Dulling. The generated
presets and input strip images are dedicated to CC0-1.0. No downloaded brush
packs or third-party implementation source are included.

The workflow retains the exact generated presets, application binary hash,
reported version, image pixel hashes, runtime log and PNG results. It fails on
missing presets, version mismatch, unchanged strokes or incomplete output. These
are source execution fixtures, **not passing compatibility comparisons**. The
public `Node.paintLine` API supplies pressure endpoints but does not reproduce
physical tablet delivery, rotation, tilt or timed airbrush. Those require separate
fixtures under the contract in `doc/raster_brush_compatibility.md`.

The comparison executable renders the same strip canvas through the imported
native conversions and retains candidate images, full-canvas differences and
`comparison.json`. Metrics include premultiplied RGB and alpha errors, alpha
p99, changed-pixel IoU and both extents, using the union of changed pixels dilated
by two pixels. Missing output fails the workload. Numerical differences are
reported without a parity pass: the imports remain explicitly converted and
unverified. This five-fixture workload is initial measurement, not the full
diameter/spacing/pressure/long-stroke qualification matrix.

The application is fetched from the official versioned HTTPS download location;
its recorded hash is evidence of the executed binary, not a prevalidated hash
allowlist. CI publishes the original fixtures and outputs, not the application.

References:

- [Current official download](https://krita.org/en/download/)
- [Public Node API](https://api.kde.org/legacy/krita/html/classNode.html)
- [Public View API](https://api.kde.org/legacy/krita/html/classView.html)
- [Python plugin registration](https://docs.krita.org/en/user_manual/python_scripting/krita_python_plugin_howto.html)

Run through the repository CI workflow; project instructions prohibit local
builds and tests. A successful workflow only establishes the five listed source
fixtures. It never promotes imported presets to reference-verified status.
