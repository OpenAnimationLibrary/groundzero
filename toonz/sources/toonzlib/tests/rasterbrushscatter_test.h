#pragma once
// Included inside the engine regression's existing anonymous namespace.
std::array<double, 2> scatterCentroid(const Canvas &canvas, int left, int right) {
  double weight = 0, x = 0, y = 0;
  for (int j = 0; j < canvas.h; ++j)
    for (int i = left; i < right; ++i) {
      const double alpha = canvas.read(i, j).a;
      weight += alpha;
      x += i * alpha;
      y += j * alpha;
    }
  require(weight > 0, "Source scatter diagnostic lost the authored dab");
  return {x / weight, y / weight};
}
void testSourceScatterProcess() {
  auto preset = [](bool axisX, bool axisY) {
    Preset p;
    p.profile = "krita-5.3.4.1-scatter-candidate-1";
    p.size.value = 8;
    p.spacing = 4;
    p.sourceScatterV1 = SourceScatterV1{1, axisX, axisY};
    return p;
  };
  auto paint = [](Canvas &canvas, const Preset &p) {
    Stroke stroke(canvas, p);
    require(stroke.add(sample(16, 64, 0)) && stroke.add(sample(49, 64, .1)),
            stroke.error().c_str());
    finishStroke(stroke);
    require(stroke.dabCount() == 2,
            "Source scatter changed the measured nominal source cadence");
  };
  Preset control = preset(true, true);
  control.profile = "krita-5.3.4.1-hard-pixel-candidate-1";
  control.sourceScatterV1.reset();
  Canvas baseline;
  paint(baseline, control);
  const auto first = scatterCentroid(baseline, 0, 32),
             later = scatterCentroid(baseline, 32, 80);
  require(first[0] == 16 && first[1] == 63.5 && later[0] == 48 && later[1] == 63.5,
          "Source scatter control lost the independently observed hard-mask lattice");
  int controlPixels = 0;
  for (const auto &pixel : baseline.pixels) controlPixels += pixel.a != 0;
  require(controlPixels == 96, "Disabled source scatter control changed original 48-pixel dabs");
  for (int axes = 0; axes < 3; ++axes) {
    Preset p = preset(axes != 1, axes != 0);
    require(validate(p).empty(), "Measured static source scatter process was rejected");
    double squared = 0, crossSquared = 0;
    bool squareCorner = false;
    Canvas firstRaster;
    for (int replicate = 0; replicate < 100; ++replicate) {
      p.seed = UINT64_C(0x9e3779b97f4a7c15) * std::uint64_t(replicate + 1);
      Canvas canvas;
      paint(canvas, p);
      if (!replicate) firstRaster = canvas;
      const auto a = scatterCentroid(canvas, 0, 32), b = scatterCentroid(canvas, 32, 80);
      const double dx = a[0] - first[0], dy = a[1] - first[1],
                   bx = b[0] - later[0], by = b[1] - later[1];
      require(std::abs(dx) <= 8.3 && std::abs(dy) <= 8.3 &&
                  std::abs(bx) <= 8.3 && std::abs(by) <= 8.3,
              "Source scatter escaped the observed dimensionless displacement support");
      if (axes == 2) {
        squared += bx * bx + by * by;
        squareCorner |= bx * bx + by * by > 64;
      } else {
        // The observed first contact basis points from global origin to (16,64).
        // Later single-axis dabs follow the horizontal authored path instead.
        const double ux = 1 / std::sqrt(17.0), uy = 4 / std::sqrt(17.0);
        const double active = axes == 0 ? dx * ux + dy * uy : -dx * uy + dy * ux;
        const double orthogonal = axes == 0 ? -dx * uy + dy * ux : dx * ux + dy * uy;
        require(std::abs(orthogonal) < .35 &&
                    std::abs(axes == 0 ? by : bx) < .2,
                "Source scatter used path direction for the first dab or randomized its next direction");
        squared += active * active;
        crossSquared += orthogonal * orthogonal;
      }
    }
    // Presence/support tests supplement the retained opaque-source RCP1 gates;
    // they do not presume the source application's private RNG or exact seeds.
    require(squared / 100 > 9 && (axes == 2 || crossSquared / 100 < .1),
            "Enabled source scatter collapsed to a deterministic or wrong-axis process");
    if (axes == 2)
      require(squareCorner, "Two-axis source scatter became a disk-shaped process");
    p.seed = UINT64_C(0x9e3779b97f4a7c15);
    Canvas repeated;
    paint(repeated, p);
    require(same(firstRaster, repeated), "Known native RNG lost exact seed reproducibility in candidate rendering");
  }
  for (int mutation = 0; mutation < 8; ++mutation) {
    auto p = preset(true, false);
    if (mutation == 0) p.sourceScatterV1.reset();
    if (mutation == 1) p.profile = "groundzero-native-1";
    if (mutation == 2) p.sourceScatterV1->amount = .5;
    if (mutation == 3) p.sourceScatterV1->axisX = false;
    if (mutation == 4) p.size.value = 16;
    if (mutation == 5) p.scatter.mappings.push_back(Mapping{});
    if (mutation == 6) p.opacity.value = .5;
    if (mutation == 7) p.wash = true;
    require(!validate(p).empty(), "Unmeasured source scatter settings silently admitted");
  }
}
