# FX render references

Reference images for `fxrender_test` (`stdfx/tests/fxrender_test.cpp`), one
PNG per render case. The test renders each case and compares it with the PNG
of the same name, allowing small floating point differences.

When a case is added, or a change is meant to alter a render, take the new
images from the `fxrender-output` artifact of the Linux CI run (or from
`stdfx/fxrender-output` in a local build folder), check them, and copy them
here.

The PNG files hold the renderer's premultiplied pixel values stored as
straight RGBA, so semi-transparent areas look slightly darker in an image
viewer than they render in OpenToonz.

Cases without a reference image are reported as skipped, not failed, as
long as every other case matches. The output image of each such case is
still written, ready to be checked and copied here.
