#include "toonz/rasterbrushengine.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace RasterBrush;
namespace {
void require(bool v, const char *s) {
  if (!v) throw std::runtime_error(s);
}
struct Canvas : Surface {
  int w = 128, h = 128;
  bool refuse = false;
  std::vector<Pixel> pixels;
  Canvas() : pixels(size_t(w) * h) {}
  int width() const override { return w; }
  int height() const override { return h; }
  bool prepare(int x0, int y0, int x1, int y1) override {
    require(x0 >= 0 && y0 >= 0 && x1 <= w && y1 <= h && x0 < x1 && y0 < y1,
            "unclipped transaction");
    return !refuse;
  }
  Pixel read(int x, int y) const override {
    return pixels.at(size_t(y) * w + x);
  }
  void write(int x, int y, const Pixel &p) override {
    pixels.at(size_t(y) * w + x) = p;
  }
  double coverage(int x, int) const override { return x < 100 ? 1 : 0; }
};
bool same(const Canvas &a, const Canvas &b) {
  for (size_t i = 0; i < a.pixels.size(); ++i) {
    const auto &x = a.pixels[i], &y = b.pixels[i];
    if (x.r != y.r || x.g != y.g || x.b != y.b || x.a != y.a) return false;
  }
  return true;
}
Sample sample(double x, double y, double t, double pressure = 1) {
  Sample s;
  s.x           = x;
  s.y           = y;
  s.time        = t;
  s.pressure    = pressure;
  s.hasPressure = true;
  return s;
}
void line(Canvas &c, Preset p) {
  Stroke s(c, p);
  for (int i = 0; i <= 120; ++i)
    require(s.add(sample(i, 64, i / 240.0)), s.error().c_str());
  s.finish();
}
void testFloatTransaction() {
  struct Quantized : Canvas {
    void write(int x, int y, const Pixel &p) override {
      auto q = [](double v) { return std::floor(v * 255 + .5) / 255; };
      Canvas::write(x, y, {q(p.r), q(p.g), q(p.b), q(p.a)});
    }
  };
  auto canvas = std::make_shared<Quantized>();
  auto budget = std::make_shared<MemoryBudget>(2 * 1024 * 1024);
  {
    BufferedSurface surface(canvas, budget);
    Preset p;
    p.flow.value   = .001;
    p.airbrushRate = 120;
    p.size.value   = 4;
    Stroke stroke(surface, p, 2 * 1024 * 1024, budget);
    for (int i = 0; i < 240; ++i)
      require(stroke.add(sample(32, 32, i / 240.0)), stroke.error().c_str());
    require(canvas->read(32, 32).a > .05,
            "sub-byte deposits lost to host quantization");
    stroke.finish();
    require(budget->used() > 0, "shared surface unaccounted");
  }
  require(budget->used() == 0, "shared memory leaked");
  auto tiny = std::make_shared<MemoryBudget>(1);
  {
    BufferedSurface denied(canvas, tiny);
    require(!denied.prepare(0, 0, 1, 1), "float tile budget ignored");
  }
  require(tiny->used() == 0, "denied buffer leaked budget");
}
void testComposition() {
  Pixel a{.2, .1, 0, .5}, b{.1, .3, .2, .5};
  Pixel n = composite(a, b, Blend::Normal);
  require(std::abs(n.a - .75) < 1e-12 && std::abs(n.r - .2) < 1e-12,
          "premultiplied source-over");
  require(composite(a, b, Blend::Erase).a == .25, "erase alpha");
  require(composite(a, b, Blend::Screen, true).a == a.a, "lock alpha");
  Pixel z = composite({}, {}, Blend::Overlay);
  require(z.a == 0 && z.r == 0, "transparent color");
}
void testAreaCoverage() {
  Preset p;
  p.tip.shape          = Shape::Square;
  p.tip.antialiasEdges = true;
  p.size.value         = 1;
  Canvas c;
  Stroke stroke(c, p);
  require(stroke.add(sample(32, 32, 0)), stroke.error().c_str());
  stroke.finish();
  double area = 0;
  for (const auto &pixel : c.pixels) area += pixel.a;
  require(area == 1 && c.read(31, 31).a == .25 && c.read(32, 32).a == .25,
          "one-pixel square area was not conserved across four pixels");
  p.size.value = .25;
  Canvas tiny;
  Stroke dot(tiny, p);
  require(dot.add(sample(32, 32, 0)), dot.error().c_str());
  dot.finish();
  area = 0;
  for (const auto &pixel : tiny.pixels) area += pixel.a;
  require(area == .0625, "subpixel square disappeared or gained coverage");
  p.size.value   = 4;
  p.aspect.value = .5;
  Canvas horizontal, vertical;
  Stroke a(horizontal, p);
  require(a.add(sample(32, 32, 0)), a.error().c_str());
  a.finish();
  p.angle.value = 90;
  Stroke b(vertical, p);
  require(b.add(sample(32, 32, 0)), b.error().c_str());
  b.finish();
  for (int y = 28; y < 36; ++y)
    for (int x = 28; x < 36; ++x)
      require(horizontal.read(x, y).a == vertical.read(y, x).a,
              "antialias footprint did not rotate with anisotropic tip");
}
void testDeterminismAndMasks() {
  Preset p;
  p.size.value    = 12;
  p.scatter.value = .2;
  p.count         = 3;
  p.color         = {.8, .2, .1, 1};
  Canvas a, b, c;
  line(a, p);
  line(b, p);
  require(same(a, b), "random stroke not deterministic");
  ++p.seed;
  line(c, p);
  require(!same(a, c), "different seed ignored");
  for (int y = 0; y < c.h; ++y)
    for (int x = 100; x < c.w; ++x)
      require(c.read(x, y).a == 0, "selection escaped");
  p.scatter.value          = 0;
  p.count                  = 1;
  p.secondary.enabled      = true;
  p.secondary.mode         = MaskMode::Subtract;
  p.secondary.size.value   = 1;
  p.secondary.tip.hardness = 1;
  Canvas d;
  line(d, p);
  for (const auto &v : d.pixels) require(v.a == 0, "subtract mask ignored");
}
void testWashAndInput() {
  Preset p;
  p.wash          = true;
  p.opacity.value = .3;
  p.size.value    = 20;
  p.color         = {1, 0, 0, 1};
  Canvas c;
  line(c, p);
  double peak = 0;
  for (auto v : c.pixels) peak = std::max(peak, v.a);
  require(std::abs(peak - .3) < 1e-10, "wash opacity accumulated per dab");
  p.wash = false;
  Canvas d;
  line(d, p);
  require(d.read(64, 64).a > .3, "build-up did not accumulate");
  p.flow.mappings.push_back({Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
  Canvas e;
  Stroke s(e, p);
  require(s.add(sample(20, 20, 0, 0)), "zero pressure sample rejected");
  for (auto v : e.pixels) require(v.a == 0, "zero pressure painted");
  Sample invalid = sample(20, 20, -1);
  require(!s.add(invalid), "backward timestamp accepted");
  Canvas f;
  f.refuse = true;
  Stroke denied(f, p);
  require(!denied.add(sample(20, 20, 0)), "transaction denial ignored");
}
void testWetPressure() {
  Preset p;
  p.wet       = Wet::Dulling;
  p.pickup    = 1;
  p.colorRate = 0;
  p.color     = {1, 0, 0, 1};
  p.pickupControl.mappings.push_back({Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
  Canvas low, high;
  for (auto &v : low.pixels) v = {0, 0, 1, 1};
  high.pixels = low.pixels;
  Stroke a(low, p), b(high, p);
  require(a.add(sample(64, 64, 0, 0)), "low pressure wet failed");
  require(b.add(sample(64, 64, 0, 1)), "high pressure wet failed");
  require(low.read(64, 64).r > .9 && high.read(64, 64).b > .9,
          "wet pressure pickup was ignored");
}
void testWetPickupGeometry() {
  for (auto mode : {Wet::Dulling, Wet::Smear}) {
    for (double samplingRadius : {0.0, 1.0}) {
      Canvas horizontal, vertical;
      for (int y = 0; y < horizontal.h; ++y)
        for (int x = 0; x < horizontal.w; ++x) {
          Pixel color =
              y >= 60 && y < 68 ? Pixel{1, 0, 0, 1} : Pixel{0, 0, 1, 1};
          horizontal.write(x, y, color);
          vertical.write(x, y, color);
        }
      Preset p;
      p.wet          = mode;
      p.size.value   = 32;
      p.aspect.value = .125;
      p.pickup       = 1;
      p.colorRate    = 0;
      p.smudgeRadius = samplingRadius;
      Stroke first(horizontal, p);
      p.angle.value = 90;
      Stroke second(vertical, p);
      require(first.add(sample(64.25, 64.25, 0)) &&
                  second.add(sample(64.25, 64.25, 0)),
              "rotated wet pickup failed");
      require(horizontal.read(64, 64).r > .999,
              "narrow wet tip sampled outside its footprint");
      if (samplingRadius == 0)
        require(vertical.read(64, 64).r > .999,
                "zero pickup radius did not sample the dab center");
      else
        require(vertical.read(64, 64).b > .5,
                "wet pickup ignored tip rotation");
    }
    Canvas original, mirrored;
    for (int y = 0; y < original.h; ++y)
      for (int x = 0; x < original.w; ++x) {
        Pixel color = x < 64 ? Pixel{1, 0, 0, 1} : Pixel{0, 0, 1, 1};
        original.write(x, y, color);
        mirrored.write(x, y, color);
      }
    auto mask    = std::make_shared<Image>();
    mask->width  = 2;
    mask->height = 1;
    mask->pixels = {{0, 0, 0, 1}, {0, 0, 0, 0}};
    Preset p;
    p.wet        = mode;
    p.tip.shape  = Shape::Bitmap;
    p.tip.images = {mask};
    p.size.value = 32;
    p.pickup     = 1;
    p.colorRate  = 0;
    Stroke first(original, p);
    p.flipX = true;
    Stroke second(mirrored, p);
    require(first.add(sample(64, 64, 0)) && second.add(sample(64, 64, 0)),
            "mirrored wet pickup failed");
    // Bilinear filtering feathers the half-mask: symmetry, rather than an
    // assumed hard edge, determines the expected channel relationship.
    require(std::abs(original.read(60, 64).r - mirrored.read(67, 64).b) < 1e-12,
            "wet pickup did not mirror with its asymmetric tip");
  }
}
void testCountFadeAndDirection() {
  Preset p;
  p.size.value            = 4;
  p.color                 = {1, 0, 0, 1};
  p.count                 = 4;
  p.flow.value            = .25;
  p.countControl.mappings = {{Sensor::Pressure, {{0, 0}, {1, 1}}, 1}};
  Canvas zero, half;
  Stroke suppressed(zero, p), twoStamps(half, p);
  require(suppressed.add(sample(32, 32, 0, 0)) &&
              twoStamps.add(sample(32, 32, 0, .5)),
          "count dynamics failed");
  require(zero.read(32, 32).a == 0 && half.read(32, 32).a == .4375,
          "count multiplier did not select zero and two deposits");

  p.count            = 1;
  p.countControl     = Dynamic{1, 0, {}};
  p.flow.value       = 1;
  p.spacing          = 2;
  p.opacity.mappings = {{Sensor::DabIndex, {{0, 1}, {1, 0}}, 2}};
  Canvas fading;
  Stroke fade(fading, p);
  require(fade.add(sample(32, 32, 0)) && fade.add(sample(48, 32, 1)),
          "dab-index fade failed");
  require(fading.read(32, 32).a == 1 && fading.read(40, 32).a == .5 &&
              fading.read(48, 32).a == 0,
          "dab fade did not advance once per scheduled step");

  p.spacing          = 1;
  p.opacity.mappings = {
      {Sensor::InitialDirection, {{0, 1}, {.25, 0}, {1, 0}}, 1}};
  Canvas initial, current;
  Stroke held(initial, p);
  p.opacity.mappings.front().sensor = Sensor::Direction;
  Stroke following(current, p);
  for (auto point : {sample(32, 32, 0), sample(64, 32, 1), sample(64, 80, 2)})
    require(held.add(point) && following.add(point),
            "direction dynamics failed");
  require(initial.read(64, 80).a == 1 && current.read(64, 80).a == 0,
          "initial direction changed after a turn");

  p.opacity.mappings.clear();
  p.count           = 64;
  p.scatter.value   = 5;
  p.scatterBothAxes = false;
  Canvas scattered;
  Stroke perpendicular(scattered, p);
  require(perpendicular.add(sample(32, 64, 0)), "one-axis scatter failed");
  bool outsideCenter = false;
  for (int y = 0; y < scattered.h; ++y)
    for (int x = 0; x < scattered.w; ++x) {
      if (scattered.read(x, y).a == 0) continue;
      require(x >= 30 && x < 34,
              "perpendicular scatter moved along the stroke");
      outsideCenter = outsideCenter || y < 54 || y > 74;
    }
  require(outsideCenter, "one-axis scatter did not disperse stamps");
}
void testKritaHardPixelCandidate() {
  struct FullCanvas : Canvas {
    double coverage(int, int) const override { return 1; }
  };
  Preset p;
  p.profile = "krita-5.3.4.1-hard-pixel-candidate-1";
  // Alpha counts measured from original K5341 fixed-precision-5 fixtures,
  // not generated by this implementation. See geometry-9736e151 evidence.
  const int diameters[] = {1, 7, 8, 32, 64, 128};
  const int counts[]    = {0, 38, 48, 800, 3208, 12844};
  for (int i = 0; i < 6; ++i) {
    FullCanvas canvas;
    p.size.value = diameters[i];
    Stroke stroke(canvas, p);
    require(stroke.add(sample(64, 64, 0)), stroke.error().c_str());
    int count = 0;
    for (const auto &pixel : canvas.pixels) {
      require(pixel.a == 0 || pixel.a == 1,
              "source hard mask gained soft edges");
      count += pixel.a == 1;
    }
    require(count == counts[i], "K5341 measured hard-dab area differs");
  }
  p.size.value = 8;
  p.spacing    = 1;
  FullCanvas sparse, divided;
  Stroke a(sparse, p), b(divided, p);
  require(a.add(sample(32, 32, 0)) && a.add(sample(48, 32, 1)),
          "source sparse path failed");
  require(b.add(sample(32, 32, 0)) && b.add(sample(40, 32, .5)) &&
              b.add(sample(40, 32, .5)) && b.add(sample(48, 32, 1)),
          "source subdivided path failed");
  a.finish();
  b.finish();
  require(a.dabCount() == 2 && b.dabCount() == 2 && same(sparse, divided),
          "source endpoint scheduling depends on event subdivision");
  p.size.value = 32;
  p.spacing    = .1;
  FullCanvas longCanvas;
  Stroke longStroke(longCanvas, p);
  require(
      longStroke.add(sample(0, 64, 0)) && longStroke.add(sample(1536, 64, 1)),
      "source long decimal-spacing path failed");
  longStroke.finish();
  require(longStroke.dabCount() == 480,
          "decimal spacing emitted a spurious final source dab");
  p.size.minimum =
      0;  // Explicit source curve has no native minimum-size offset.
  p.size.mappings.push_back({Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
  FullCanvas pressured;
  Stroke pressureStroke(pressured, p);
  require(pressureStroke.add(sample(64, 64, 0, .25)),
          pressureStroke.error().c_str());
  int pressureCount = 0;
  for (const auto &pixel : pressured.pixels) pressureCount += pixel.a == 1;
  require(pressureCount == 48,
          "linear source pressure changed measured eight-pixel geometry");
  p.spacing = 1;
  FullCanvas rising;
  Stroke risingStroke(rising, p);
  require(risingStroke.add(sample(0, 64, 0, .25)) &&
              risingStroke.add(sample(1536, 64, 1, .75)),
          risingStroke.error().c_str());
  risingStroke.finish();
  require(risingStroke.dabCount() == 106,
          "source sparse pressure spacing differs from captured footprints");
  p.size.mappings.front().curve.insert(
      p.size.mappings.front().curve.begin() + 1, {.5, .25});
  require(!validate(p).empty(),
          "unqualified nonlinear source pressure curve accepted");
  p.size.mappings.clear();
  p.opacity.value = .5;
  p.flow.value    = 0;
  p.wash          = false;
  FullCanvas buildUp;
  Stroke buildUpStroke(buildUp, p);
  require(buildUpStroke.add(sample(64, 64, 0)) && buildUp.read(64, 64).a == .5,
          "source build-up incorrectly applied inactive flow");
  p.wash = true;
  FullCanvas wash;
  Stroke washStroke(wash, p);
  require(washStroke.add(sample(64, 64, 0)) && wash.read(64, 64).a == 0,
          "source wash ignored zero flow");
  p.tip.antialiasEdges = true;
  require(!validate(p).empty(), "unimplemented source edge model accepted");
}
void testSecondaryCountAndMirror() {
  Preset p;
  p.size.value              = 16;
  p.tip.shape               = Shape::Square;
  p.secondary.enabled       = true;
  p.secondary.tip.shape     = Shape::Square;
  p.secondary.opacity.value = .5;
  p.secondary.count         = 2;
  Canvas canvas;
  Stroke repeated(canvas, p);
  require(repeated.add(sample(32, 32, 0)), repeated.error().c_str());
  require(canvas.read(32, 32).a == .75,
          "secondary stamps did not compose coverage before masking");
  p.secondary.mode = MaskMode::Subtract;
  Canvas subtracted;
  Stroke cut(subtracted, p);
  require(cut.add(sample(32, 32, 0)), cut.error().c_str());
  require(subtracted.read(32, 32).a == .25,
          "secondary subtraction was applied separately per stamp");
  auto image   = std::make_shared<Image>();
  image->width = image->height = 2;
  image->pixels                = {{0, 0, 0, 1}, {}, {}, {}};
  p.secondary.tip.shape        = Shape::Bitmap;
  p.secondary.tip.images       = {image};
  p.secondary.mode             = MaskMode::Multiply;
  p.secondary.count            = 1;
  p.secondary.opacity.value    = 1;
  Canvas original, mirrored;
  Stroke a(original, p);
  require(a.add(sample(32.5, 32.5, 0)), a.error().c_str());
  p.secondary.flipX = p.secondary.flipY = true;
  Stroke b(mirrored, p);
  require(b.add(sample(32.5, 32.5, 0)), b.error().c_str());
  require(original.read(28, 28).a == 1 && mirrored.read(28, 28).a == 0 &&
              mirrored.read(36, 36).a == 1 && original.read(36, 36).a == 0,
          "secondary image axes did not mirror independently of primary");
  p.secondary.count = 65;
  require(!validate(p).empty(), "unbounded secondary stamp count accepted");
}
void testResourcesAndWet() {
  Preset p;
  p.wet   = Wet::Dulling;
  p.color = {1, 0, 0, 1};
  Canvas c;
  for (auto &v : c.pixels) v = {0, 0, 1, 1};
  line(c, p);
  Pixel middle = c.read(64, 64);
  require(middle.r > 0 && middle.b > 0,
          "wet brush did not mix pickup and paint");
  p.wet = Wet::Smear;
  Canvas a, b;
  for (auto &v : a.pixels) v = {0, 0, 1, 1};
  b.pixels = a.pixels;
  line(a, p);
  line(b, p);
  require(same(a, b), "smear not deterministic");
  p.wet  = Wet::Off;
  p.wash = true;
  Canvas low;
  Stroke limited(low, p, 1);
  require(!limited.add(sample(20, 20, 0)), "stroke memory budget ignored");
  p.profile = "krita-unverified";
  require(!validate(p).empty(), "unknown profile silently approximated");
  p.profile   = "groundzero-native-1";
  p.tip.shape = Shape::Bitmap;
  require(!validate(p).empty(), "missing bitmap accepted");
  p.tip.shape  = Shape::Round;
  p.size.value = std::numeric_limits<double>::infinity();
  require(!validate(p).empty(), "non-finite setting accepted");
}
void testStationaryAndBatching() {
  Preset p;
  p.airbrushRate = 10;
  p.size.value   = 4;
  Canvas a, b;
  Stroke sa(a, p), sb(b, p);
  std::vector<Sample> events;
  for (int i = 0; i <= 240; ++i) events.push_back(sample(32, 32, i / 240.0));
  for (auto e : events) require(sa.add(e), "stationary event failed");
  for (size_t batch = 0; batch < events.size(); batch += 16)
    for (size_t i = batch; i < std::min(batch + 16, events.size()); ++i)
      require(sb.add(events[i]), "batched event failed");
  require(same(a, b) && sa.dabCount() == sb.dabCount(),
          "delivery batching changed stroke");
  require(sa.dabCount() >= 10 && sa.dabCount() <= 11,
          "airbrush timer did not schedule stationary dabs");
}
void testColorDynamics() {
  auto deposit = [](Preset p, double pressure) {
    Canvas canvas;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(32, 32, 0, pressure)), stroke.error().c_str());
    return canvas.read(32, 32);
  };
  Preset p;
  p.color     = {1, 0, 0, 1};
  p.hue.value = 120;
  Pixel green = deposit(p, 1);
  require(green.r == 0 && green.g == 1 && green.b == 0 && green.a == 1,
          "HSV hue did not rotate red to green");
  p.hue.mappings.push_back({Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
  Pixel yellow = deposit(p, .5);
  require(yellow.r == 1 && yellow.g == 1 && yellow.b == 0,
          "pressure color curve not evaluated at dab");
  p.hue.value        = 0;
  p.saturation.value = 0;
  p.brightness.value = .5;
  Pixel gray         = deposit(p, 1);
  require(gray.r == .5 && gray.g == .5 && gray.b == .5 && gray.a == 1,
          "HSV saturation/value changed alpha");
  p.saturation.value = p.brightness.value = 1;
  p.color                                 = {.5, 0, 0, .5};
  p.background                            = {0, 0, 0, 0};
  p.backgroundMix.value                   = .5;
  Pixel transparent                       = deposit(p, 1);
  require(transparent.r == .25 && transparent.a == .25 && transparent.g == 0,
          "background mix did not preserve premultiplied alpha");
  p.color        = {1, 0, 0, 1};
  p.hue.value    = 360;
  p.hue.mappings = {{Sensor::RandomDab, {{0, 0}, {1, 1}}, 1}};
  Canvas a, b;
  line(a, p);
  line(b, p);
  require(same(a, b), "seeded color variation is not repeatable");
  p.background.r = 1;
  require(!validate(p).empty(), "non-premultiplied background accepted");
}
void testLongWetTrace() {
  // Original color/alpha strips and a long reversing path exercise reservoir
  // accumulation, transparency, deterministic sampling and lifetime accounting.
  // This proves native repeatability, not agreement with another application.
  for (auto mode : {Wet::Dulling, Wet::Smear}) {
    Canvas first, repeated;
    for (int y = 0; y < first.h; ++y)
      for (int x = 0; x < first.w; ++x) {
        double alpha = (x % 16) / 15.0;
        first.pixels[size_t(y) * first.w + x] =
            (x / 16) % 2 ? Pixel{alpha, 0, 0, alpha}
                         : Pixel{0, alpha * .5, alpha, alpha};
      }
    repeated.pixels = first.pixels;
    Preset p;
    p.wet        = mode;
    p.size.value = 4;
    p.spacing    = .25;
    p.pickup     = .75;
    p.colorRate  = .125;
    p.reload     = .015625;
    p.color      = {.125, .25, 0, .5};
    auto budget  = std::make_shared<MemoryBudget>(1024 * 1024);
    Stroke a(first, p, 1024 * 1024, budget), b(repeated, p);
    const size_t checkpoints[] = {1, 10, 100, 1000, 10000};
    size_t checkpoint          = 0;
    for (int i = 0; checkpoint < 5 && i < 50000; ++i) {
      const int phase = i % 512;
      const double x  = 16 + (phase <= 256 ? phase : 512 - phase) * .25;
      auto event      = sample(x, 64, i / 256.0);
      require(a.add(event) && b.add(event), "long native wet trace failed");
      if (a.dabCount() < checkpoints[checkpoint]) continue;
      require(a.dabCount() == checkpoints[checkpoint],
              "long wet trace skipped a dab checkpoint");
      require(same(first, repeated), "native wet drift is not repeatable");
      for (const auto &pixel : first.pixels)
        require(std::isfinite(pixel.a) && std::isfinite(pixel.r) &&
                    std::isfinite(pixel.g) && std::isfinite(pixel.b) &&
                    pixel.a >= 0 && pixel.a <= 1 && pixel.r >= 0 &&
                    pixel.g >= 0 && pixel.b >= 0 &&
                    pixel.r <= pixel.a + 1e-12 && pixel.g <= pixel.a + 1e-12 &&
                    pixel.b <= pixel.a + 1e-12,
                "long wet trace left the premultiplied color domain");
      ++checkpoint;
    }
    require(checkpoint == 5, "long wet trace did not reach 10000 dabs");
    a.finish();
    b.finish();
    require(budget->used() == 0, "long wet trace retained reservoir memory");
  }
}
void testSourceWetInitialization() {
  for (const auto &model :
       {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"}) {
    Preset p;
    p.profile = std::string("krita-5.3.4.1-") + model + "-candidate-1";
    p.tip.antialiasEdges = false;
    p.size.value         = 8;
    p.spacing            = 5;
    p.wet          = std::string(model).find("dulling") != std::string::npos
                         ? Wet::Dulling
                         : Wet::Smear;
    p.smudgeRadius = 0;
    p.color        = {0, 1, 0, 1};
    Canvas canvas;
    for (int y = 0; y < canvas.h; ++y)
      for (int x = 0; x < canvas.w; ++x)
        canvas.write(
            x, y,
            x < 50 ? Pixel{0, 0, 1, 1} : Pixel{128 / 255.0, 0, 0, 128 / 255.0});
    Canvas original = canvas;
    auto budget     = std::make_shared<MemoryBudget>(1024 * 1024);
    Stroke stroke(canvas, p, 1024 * 1024, budget);
    require(stroke.add(sample(30, 64, 0)), stroke.error().c_str());
    require(same(canvas, original), "source wet first dab deposited paint");
    require(stroke.add(sample(71, 64, 1)), stroke.error().c_str());
    Pixel center          = canvas.read(70, 64);
    const bool modern     = std::string(model).find("new-") == 0;
    const bool newDulling = std::string(model) == "new-dulling";
    const double a        = (newDulling ? 184 : modern ? 195 : 192) / 255.0;
    require(std::abs(center.a - a) < 1e-12, "source wet alpha equation");
    require(
        std::abs(center.g / center.a - (newDulling ? 22 : 21) / 255.0) < 1e-12,
        "source wet color equation");
    stroke.finish();
    require(budget->used() == 0, "source wet snapshot retained memory");
    // Original 7px second-dab footprints, translated from captured BGRA.
    const char *smearRows[]   = {"..###..", ".#####.", "#######", "#######",
                                 "#######", ".#####.", "..###..", "......."};
    const char *dullingRows[] = {".......", ".#####.", "#######", "#######",
                                 "#######", "#######", ".#####.", "......."};
    Canvas odd                = original;
    p.size.value              = 7;
    Stroke oddStroke(odd, p);
    require(
        oddStroke.add(sample(30, 64, 0)) && oddStroke.add(sample(66, 64, 1)),
        oddStroke.error().c_str());
    oddStroke.finish();
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 7; ++x) {
        const bool changed = odd.read(x + 62, y + 60).b != 0;
        const char expected =
            (p.wet == Wet::Smear ? smearRows : dullingRows)[y][x];
        require(changed == (expected == '#'),
                "odd wet mask differs from source footprint");
      }
    p.size.value = 8;
    for (int control = 0; control < 2; ++control) {
      Preset switches                    = p;
      switches.sourceSmearAlpha          = control != 0;
      switches.sourceSmudgeLengthEnabled = control != 1;
      Canvas switched                    = original;
      Stroke switchedStroke(switched, switches);
      require(switchedStroke.add(sample(10, 64, 0)) &&
                  switchedStroke.add(sample(91, 64, 1)),
              switchedStroke.error().c_str());
      switchedStroke.finish();
      const Pixel result       = switched.read(90, 64);
      const int alphaOff[3][4] = {
          {152, 33, 70, 178}, {143, 33, 79, 182}, {186, 14, 56, 165}};
      const int lengthOff[3][4] = {
          {0, 60, 195, 255}, {0, 31, 224, 255}, {50, 33, 171, 213}};
      const int *expected = (control == 0 ? alphaOff : lengthOff)[newDulling ? 2
                                                                  : modern   ? 1
                                                                           : 0];
      require(
          std::abs(result.a * 255 - expected[3]) <= 1.000001 && result.a > 0 &&
              std::abs(result.r / result.a * 255 - expected[0]) <= 2.000001 &&
              std::abs(result.g / result.a * 255 - expected[1]) <= 2.000001 &&
              std::abs(result.b / result.a * 255 - expected[2]) <= 2.000001,
          "source wet switch third-dab state differs from captured fixture");
    }
    Canvas carried = original;
    p.colorRate    = 0;
    Stroke three(carried, p);
    require(three.add(sample(10, 64, 0)) && three.add(sample(91, 64, 1)),
            three.error().c_str());
    Pixel third = carried.read(90, 64);
    require(std::abs(third.a - (newDulling ? 148 : 160) / 255.0) < 1e-12 &&
                std::abs(third.b / third.a - (newDulling ? 70 : 102) / 255.0) <
                    2 / 255.0,
            "source arithmetic feedback differs on repeated destinations");
    three.finish();
    // Independently captured third-dab values with a changed destination
    // distinguish arithmetic feedback from carried-ink source-over models.
    for (bool transparent : {true, false}) {
      Canvas varied = original;
      for (int y = 0; y < varied.h; ++y)
        for (int x = 80; x < varied.w; ++x)
          varied.write(x, y, transparent ? Pixel{} : Pixel{1, 1, 0, 1});
      Stroke sequence(varied, p);
      require(
          sequence.add(sample(10, 64, 0)) && sequence.add(sample(91, 64, 1)),
          sequence.error().c_str());
      const Pixel result = varied.read(90, 64);
      const double expectedAlpha =
          (transparent ? (newDulling ? 72 : 96) : (newDulling ? 225 : 223)) /
          255.0;
      const double expectedBlue =
          (transparent ? (newDulling ? 145 : 170) : (newDulling ? 46 : 73)) /
          255.0;
      require(std::abs(result.a - expectedAlpha) <= 1 / 255.0 &&
                  std::abs(result.b / result.a - expectedBlue) <= 2 / 255.0,
              "source feedback differs on a changed third destination");
      sequence.finish();
    }
    Canvas patch = original;
    for (int y = 0; y < patch.h; ++y)
      for (int x = 50; x < patch.w; ++x)
        if (((x - 50) / 2) % 2) patch.write(x, y, {1, 1, 0, 1});
    Stroke patchStroke(patch, p);
    require(patchStroke.add(sample(10, 64, 0)) &&
                patchStroke.add(sample(91, 64, 1)),
            patchStroke.error().c_str());
    const Pixel yellow = patch.read(92, 64);
    const bool dulling = p.wet == Wet::Dulling;
    const double expectedAlpha =
        (dulling ? (newDulling ? 225 : 223) : 255) / 255.0;
    const double expectedGreen =
        (dulling ? (newDulling ? 174 : 146) : 191) / 255.0;
    require(std::abs(yellow.a - expectedAlpha) <= 1 / 255.0 &&
                std::abs(yellow.g / yellow.a - expectedGreen) <= 2 / 255.0,
            "Dulling center pickup was confused with Smear field feedback");
    patchStroke.finish();
    auto widerShared = std::make_shared<MemoryBudget>(1024 * 1024);
    Canvas bounded   = original;
    Stroke localLimit(bounded, p, 4096, widerShared);
    require(localLimit.add(sample(30, 64, 0)),
            "one source snapshot should fit the local budget");
    require(!localLimit.add(sample(71, 64, 1)),
            "replacement source snapshot ignored retained local memory");
    require(
        widerShared->used() <= 4096,
        "source stroke exceeded its local limit inside a wider shared budget");
    localLimit.finish();
    require(widerShared->used() == 0, "failed source stroke retained memory");
    Stroke denied(original, p, 1);
    require(!denied.add(sample(30, 64, 0)),
            "source initial snapshot bypassed budget");
    p.smudgeRadius = 1;
    require(!validate(p).empty(), "unmeasured source wet radius accepted");
  }
}
void testSourceDefaultCircle() {
  // Pixel samples from original complete K5341 alpha fixtures, not values
  // generated by the implementation under test. Origin translated to fit.
  const int measured[4][4] = {{31, 200, 255, 0},
                              {33, 212, 255, 0},
                              {41, 255, 255, 0},
                              {70, 255, 255, 0}};
  const int xy[4][2]       = {{49, 64}, {64, 56}, {64, 64}, {48, 64}};
  for (int i = 0; i < 4; ++i) {
    Preset p;
    p.profile            = "krita-5.3.4.1-default-circle-candidate-1";
    p.size.value         = 32;
    p.tip.hardness       = i * .25;
    p.tip.antialiasEdges = false;
    Canvas canvas;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(64, 64, 0)), stroke.error().c_str());
    stroke.finish();
    for (int j = 0; j < 4; ++j)
      require(std::abs(canvas.read(xy[j][0], xy[j][1]).a * 255 -
                       measured[i][j]) < 1e-9,
              "default-circle sample differs from captured source alpha");
    p.tip.antialiasEdges       = true;
    const int measuredAA[4][4] = {{31, 200, 255, 0},
                                  {32, 209, 255, 0},
                                  {39, 246, 255, 0},
                                  {60, 255, 255, 0}};
    Canvas aa;
    Stroke aaStroke(aa, p);
    require(aaStroke.add(sample(64, 64, 0)), aaStroke.error().c_str());
    aaStroke.finish();
    for (int j = 0; j < 4; ++j)
      require(std::abs(aa.read(xy[j][0], xy[j][1]).a * 255 - measuredAA[i][j]) <
                  1e-9,
              "default-circle AA differs from captured alpha");
    p.size.value = 8;
    require(!validate(p).empty(), "unqualified small-tip AA sampling accepted");
  }
}
void testSourceSmallCircle() {
  Preset p;
  p.profile            = "krita-5.3.4.1-default-circle-candidate-1";
  p.size.value         = 7;
  p.tip.hardness       = .25;
  p.tip.antialiasEdges = false;
  Canvas canvas;
  Stroke stroke(canvas, p);
  require(stroke.add(sample(64, 64, 0)), stroke.error().c_str());
  stroke.finish();
  // Original source-rendered D7 fade .25 pixels, translated from (256,1024).
  const int captured[][3] = {
      {61, 62, 22}, {64, 62, 222}, {62, 63, 177}, {63, 63, 244}};
  for (const auto &pixel : captured)
    require(std::abs(canvas.read(pixel[0], pixel[1]).a * 255 - pixel[2]) < 1e-9,
            "small-circle fade or scalar quantization differs from source");
}
void testSourceHardCircleAA() {
  Preset p;
  p.profile            = "krita-5.3.4.1-default-circle-candidate-1";
  p.size.value         = 32;
  p.tip.hardness       = 1;
  p.tip.antialiasEdges = true;
  Canvas canvas;
  Stroke stroke(canvas, p);
  require(stroke.add(sample(64, 64, 0)), stroke.error().c_str());
  stroke.finish();
  require(std::abs(canvas.read(79, 64).a * 255 - 240) < 1e-9 &&
              std::abs(canvas.read(77, 56).a * 255 - 191) < 1e-9 &&
              std::abs(canvas.read(78, 56).a * 255 - 25) < 1e-9,
          "hard-circle AA lost its pixel-axis edge behavior");
}
void testSourceRectangle() {
  Preset p;
  p.profile      = "krita-5.3.4.1-default-rect-candidate-1";
  p.tip.shape    = Shape::Square;
  p.size.value   = 32;
  p.tip.hardness = .25;
  Canvas plain;
  Stroke stroke(plain, p);
  require(stroke.add(sample(64, 64, 0)), stroke.error().c_str());
  stroke.finish();
  require(std::abs(plain.read(64, 56).a * 255 - 255) < 1e-9 &&
              std::abs(plain.read(63, 56).a * 255 - 181) <= 1.000001 &&
              std::abs(plain.read(49, 64).a * 255 - 21) <= 1.000001,
          "non-AA rectangle source axis/fade behavior changed");
  p.tip.antialiasEdges = true;
  Canvas aa;
  Stroke aaStroke(aa, p);
  require(aaStroke.add(sample(64, 64, 0)), aaStroke.error().c_str());
  aaStroke.finish();
  require(std::abs(aa.read(64, 56).a * 255 - 174) < 1e-9 &&
              std::abs(aa.read(49, 64).a * 255 - 21) < 1e-9,
          "AA rectangle source axis/fade behavior changed");
}
void testCubicCurves() {
  Preset p;
  p.opacity.mappings.push_back({Sensor::Pressure,
                                {{0, 0}, {.5, 1}, {1, 0}},
                                1,
                                Interpolation::NaturalCubic});
  Canvas canvas;
  auto budget = std::make_shared<MemoryBudget>(1024 * 1024);
  {
    Stroke stroke(canvas, p, 1024 * 1024, budget);
    require(stroke.add(sample(32, 32, 0, .25)), stroke.error().c_str());
    require(std::abs(canvas.read(32, 32).a - .6875) < 1e-12,
            "natural cubic curve used linear interpolation");
    require(budget->used() > 0, "curve cache not budgeted");
    stroke.finish();
    require(budget->used() == 0, "curve cache not released at stroke end");
  }
  Stroke denied(canvas, p, 1);
  require(!denied.add(sample(32, 32, 0)), "curve cache exceeded memory budget");
  p.opacity.mappings.front().curve = {{0, 0}, {1e-12, 1}, {1, 0}};
  require(!validate(p).empty(), "numerically unstable cubic knots accepted");
}
}  // namespace
int main() {
  try {
    testFloatTransaction();
    testComposition();
    testAreaCoverage();
    testDeterminismAndMasks();
    testWashAndInput();
    testResourcesAndWet();
    testWetPressure();
    testWetPickupGeometry();
    testCountFadeAndDirection();
    testSecondaryCountAndMirror();
    testKritaHardPixelCandidate();
    testStationaryAndBatching();
    testColorDynamics();
    testCubicCurves();
    testSourceDefaultCircle();
    testSourceSmallCircle();
    testSourceHardCircleAA();
    testSourceRectangle();
    testSourceWetInitialization();
    testLongWetTrace();
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  std::cout << "Native raster engine regression passed\n";
}
