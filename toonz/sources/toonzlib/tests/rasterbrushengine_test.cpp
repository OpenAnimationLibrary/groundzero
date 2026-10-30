#include "toonz/rasterbrushengine.h"
#include <cmath>
#include <array>
#include <algorithm>
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
void testDisabledSecondaryAndExtremeTransforms() {
  Preset p;
  p.size.value    = 12;
  p.scatter.value = .4;
  p.count         = 3;
  Canvas baseline, disabled;
  line(baseline, p);
  auto image   = std::make_shared<Image>();
  image->width = image->height   = 1;
  image->pixels                  = {Pixel{0, 0, 0, 1}};
  p.secondary.tip.shape          = Shape::Bitmap;
  p.secondary.tip.images         = {image, image};
  p.secondary.tip.randomSequence = true;
  require(!p.secondary.enabled, "secondary must remain disabled");
  line(disabled, p);
  require(same(baseline, disabled),
          "disabled secondary consumed primary stroke random sequence");

  p                    = Preset();
  p.tip.shape          = Shape::Square;
  p.tip.antialiasEdges = true;
  p.size.value         = .25;
  p.aspect.value       = .01;
  Canvas primary;
  Stroke first(primary, p);
  require(first.add(sample(32.5, 32.0625, 0)), first.error().c_str());
  first.finish();
  require(primary.read(32, 32).a > 0,
          "extreme-transform fixture did not cover a primary pixel");
  auto wide    = std::make_shared<Image>();
  wide->width  = 8192;
  wide->height = 1;
  wide->pixels.assign(8192, Pixel{0, 0, 0, 1});
  p.secondary.enabled    = true;
  p.secondary.tip.shape  = Shape::Bitmap;
  p.secondary.tip.images = {wide};
  p.secondary.size.value = .01;
  p.secondary.aspect     = .01;
  require(validate(p).empty(), "legal tiny secondary fixture rejected");
  Canvas masked;
  Stroke second(masked, p);
  require(second.add(sample(32.5, 32.0625, 0)), second.error().c_str());
  second.finish();
  // The sampled secondary coordinate exceeds 32-bit integer range in texels.
  // It is outside the image, so coverage is zero without an unsafe cast.
  for (const auto &pixel : masked.pixels)
    require(pixel.a == 0, "out-of-support secondary bitmap produced coverage");
}

void testNativeTextureCoverage() {
  Preset p;
  p.tip.shape  = Shape::Square;
  p.size.value = 8;
  auto image   = std::make_shared<Image>();
  image->width = image->height = 1;
  image->pixels                = {Pixel{0, 0, 0, .25}};
  p.texture.image              = image;
  p.texture.depth              = .4;
  p.texture.contrast           = 2;
  p.texture.brightness         = .1;
  for (bool invert : {false, true}) {
    p.texture.invert = invert;
    for (MaskMode mode : {MaskMode::Multiply, MaskMode::Subtract}) {
      p.texture.mode = mode;
      Canvas c;
      Stroke stroke(c, p);
      require(stroke.add(sample(32.5, 32.5, 0)), stroke.error().c_str());
      stroke.finish();
      // contrast/brightness yield .1; inversion yields .9. Apply depth
      // before the native mask operation, as specified by RCP1.
      const double maskValue = invert ? .9 : .1;
      const double expected =
          mode == MaskMode::Multiply ? .6 + .4 * maskValue : 1 - .4 * maskValue;
      require(std::abs(c.read(32, 32).a - expected) < 1e-12,
              "native texture contrast/invert/depth operation order changed");
    }
  }
  p.texture.depth = 0;
  Canvas off, baseline;
  line(off, p);
  p.texture.image.reset();
  line(baseline, p);
  require(same(off, baseline), "zero texture depth changed native painting");

  image           = std::make_shared<Image>();
  image->width    = 2;
  image->height   = 1;
  image->pixels   = {Pixel{}, Pixel{0, 0, 0, 1}};
  p.texture       = Texture();
  p.texture.image = image;
  for (bool perDab : {false, true}) {
    p.texture.perDab = perDab;
    for (int x : {32, 33}) {
      Canvas c;
      Stroke stroke(c, p);
      require(stroke.add(sample(x + .5, 32.5, 0)), stroke.error().c_str());
      stroke.finish();
      const double expected = perDab ? .5 : double(x % 2);
      require(c.read(x, 32).a == expected,
              "fixed/per-dab texture origin or repeat seam changed");
    }
  }
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
      else if (mode == Wet::Dulling)
        require(vertical.read(64, 64).b > .5,
                "wet pickup ignored tip rotation");
      else {
        Pixel horizontalState, verticalState;
        require(first.nativeWetState(horizontalState, .75, 0) &&
                    second.nativeWetState(verticalState, .75, 0) &&
                    horizontalState.r > .999 && verticalState.b > .999 &&
                    vertical.read(64, 64).r > .999,
                "native Smear averaged away its rotated local field");
      }
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
  p.size.mappings.front().curve = {{0, .25}, {1, .75}};
  FullCanvas minimumPressure;
  Stroke minimumStroke(minimumPressure, p);
  require(minimumStroke.add(sample(64, 64, 0, 0)),
          minimumStroke.error().c_str());
  minimumStroke.finish();
  require(same(pressured, minimumPressure),
          "nonzero source pressure-curve minimum was discarded");
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
void testNativeWetAnalyticalDrift() {
  // A one-pixel square tip with center pickup makes each field cell uniform,
  // so the documented reservoir equations have an independent affine oracle.
  // The canvas still contains color/alpha strips and is revisited in both
  // directions; pickup reads the oracle's evolving raster, never engine output.
  using Color     = std::array<long double, 4>;
  auto components = [](const Pixel &p) -> Color {
    return {p.r, p.g, p.b, p.a};
  };
  auto difference = [&](const Pixel &p, const Color &reference) {
    const auto actual = components(p);
    long double error = 0;
    for (size_t k = 0; k < 4; ++k)
      error = std::max(error, std::abs(actual[k] - reference[k]));
    return error;
  };
  for (Wet mode : {Wet::Dulling, Wet::Smear})
    for (double reload : {0.0, .125, 1.0}) {
      Canvas canvas;
      canvas.w = 16;
      canvas.h = 1;
      canvas.pixels.resize(16);
      std::vector<Color> reference(16), pickups;
      for (int x = 0; x < 16; ++x) {
        const double alpha = (x + 1) / 16.0;
        canvas.pixels[x]   = (x / 4) % 2 ? Pixel{alpha, 0, alpha * .25, alpha}
                                         : Pixel{0, alpha * .5, alpha, alpha};
        reference[x]       = components(canvas.pixels[x]);
      }
      const Canvas initial = canvas;
      Preset p;
      p.wet                    = mode;
      p.tip.shape              = Shape::Square;
      p.tip.hardness           = 1;
      p.size.value             = 1;
      p.spacing                = 1;
      p.smudgeRadius           = 0;
      p.pickup                 = .375;
      p.colorRate              = .25;
      p.reload                 = reload;
      p.color                  = {.15, .3, .075, .75};
      p.opacity.value          = .65;
      p.flow.value             = .7;
      const Color foreground   = components(p.color);
      Color state              = foreground, firstState{};
      const long double uptake = p.pickup, tint = p.colorRate, refill = reload;
      const long double retain   = (1 - refill) * (1 - uptake);
      const long double acquire  = (1 - refill) * uptake;
      const long double oldPaint = (1 - tint) * (1 - uptake);
      const long double newPaint = (1 - tint) * uptake;
      const long double deposit =
          static_cast<long double>(p.opacity.value) * p.flow.value;
      auto budget = std::make_shared<MemoryBudget>(1024 * 1024);
      Stroke stroke(canvas, p, 1024 * 1024, budget);
      Pixel measured;
      require(!stroke.nativeWetState(measured),
              "unstarted stroke exposed wet state");
      const int checkpoints[]   = {1, 10, 100, 1000, 10000};
      size_t checkpoint         = 0;
      long double previousError = 0;
      for (int step = 0; step < 10000; ++step) {
        const int phase = step % 30, x = phase <= 15 ? phase : 30 - phase;
        const Color before = reference[x];
        pickups.push_back(before);
        Color ink{};
        for (size_t k = 0; k < 4; ++k)
          ink[k] =
              oldPaint * state[k] + newPaint * before[k] + tint * foreground[k];
        for (size_t k = 0; k < 4; ++k) {
          reference[x][k] =
              deposit * ink[k] + (1 - deposit * ink[3]) * before[k];
          state[k] =
              retain * state[k] + acquire * before[k] + refill * foreground[k];
        }
        if (step == 0) firstState = state;
        require(stroke.add(sample(x + .5, .5, step / 240.0)),
                stroke.error().c_str());
        require(stroke.dabCount() == size_t(step + 1),
                "analytical wet trace resampled unexpectedly");
        if (step + 1 != checkpoints[checkpoint]) continue;
        // Closed-form geometric history sum checks the hidden state without
        // reproducing the runtime's sequence of mix operations.
        const long double power = std::pow(retain, step + 1);
        Color closed{};
        for (size_t k = 0; k < 4; ++k)
          closed[k] =
              (power + refill * (1 - power) / (1 - retain)) * foreground[k];
        long double weight = acquire;
        for (size_t j = pickups.size(); j > 0; --j) {
          for (size_t k = 0; k < 4; ++k)
            closed[k] += weight * pickups[j - 1][k];
          weight *= retain;
        }
        require(stroke.nativeWetState(measured) &&
                    difference(measured, closed) <= 1e-6L,
                "native wet state drifted from its analytical history");
        long double error = 0;
        for (size_t x = 0; x < reference.size(); ++x) {
          error = std::max(error, difference(canvas.pixels[x], reference[x]));
          const auto actual = components(canvas.pixels[x]);
          for (size_t k = 0; k < 4; ++k)
            require(std::abs(std::llround(actual[k] * 255) -
                             std::llround(reference[x][k] * 255)) <= 1,
                    "native wet raster exceeds one-byte analytical error");
        }
        require(error <= 1e-6L, "native wet raster drifted from affine oracle");
        if (step + 1 == 1000) previousError = error;
        if (step + 1 == 10000)
          require(error <= previousError + 1 / 255.0L,
                  "native wet drift increased at final checkpoint");
        ++checkpoint;
      }
      stroke.finish();
      require(!stroke.nativeWetState(measured) && budget->used() == 0,
              "finished native wet stroke retained observable state or memory");
      {
        Canvas restarted = initial;
        Stroke fresh(restarted, p, 1024 * 1024, budget);
        require(fresh.add(sample(.5, .5, 0)) &&
                    fresh.nativeWetState(measured) &&
                    difference(measured, firstState) <= 1e-6L,
                "new stroke reused previous wet state");
        // Destruction models cancellation's stroke-state teardown. The app
        // transaction suite separately checks raster rollback and undo history.
      }
      require(budget->used() == 0,
              "cancelled native wet stroke retained field memory");
    }
}
void testSourceWetOpacityTransport() {
  // Original K5341 straight RGBA8 captures, two/three separated dabs over
  // opaque blue then half-alpha red. The third dab distinguishes carried
  // opacity-scaled feedback from merely fading the displayed second dab.
  const int captured[3][4][2][4] = {
      {{{255, 0, 0, 128}, {255, 0, 0, 128}},
       {{199, 2, 55, 144}, {246, 2, 7, 130}},
       {{153, 6, 95, 160}, {220, 9, 26, 137}},
       {{116, 13, 126, 176}, {184, 21, 51, 149}}},
      {{{255, 0, 0, 128}, {255, 0, 0, 128}},
       {{193, 7, 55, 146}, {239, 9, 7, 132}},
       {{146, 12, 97, 163}, {210, 18, 27, 140}},
       {{108, 17, 130, 179}, {173, 27, 55, 152}}},
      {{{255, 0, 0, 128}, {255, 0, 0, 128}},
       {{203, 7, 45, 142}, {242, 8, 5, 131}},
       {{162, 13, 80, 157}, {220, 18, 18, 138}},
       {{128, 18, 109, 170}, {192, 27, 36, 146}}}};
  for (const auto &model :
       {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"}) {
    const std::string name = model;
    const int group = name == "new-smear" ? 1 : name == "new-dulling" ? 2 : 0;
    for (int level = 0; level < 4; ++level) {
      for (bool pressure : {false, true}) {
        if (pressure && level != 1 && level != 3) continue;
        Canvas canvas;
        for (int y = 0; y < canvas.h; ++y)
          for (int x = 0; x < canvas.w; ++x)
            canvas.write(x, y,
                         x < 40 ? Pixel{0, 0, 1, 1}
                                : Pixel{128 / 255.0, 0, 0, 128 / 255.0});
        const Canvas original = canvas;
        Preset p;
        p.profile = "krita-5.3.4.1-" + name + "-candidate-1";
        p.wet     = name.find("dulling") != std::string::npos ? Wet::Dulling
                                                              : Wet::Smear;
        p.tip.antialiasEdges = false;
        p.size.value         = 8;
        p.spacing            = 5;
        p.smudgeRadius       = 0;
        p.color              = {0, 1, 0, 1};
        p.opacity.value      = pressure ? 1 : level / 4.0;
        if (pressure)
          p.opacity.mappings = {{Sensor::Pressure, {{0, 0}, {1, 1}}, 1}};
        Stroke stroke(canvas, p);
        require(stroke.add(sample(10, 64, 0, pressure ? level / 4.0 : 1)),
                stroke.error().c_str());
        require(same(canvas, original),
                "translucent source wet first dab painted");
        require(stroke.add(sample(91, 64, 1, pressure ? level / 4.0 : 1)),
                stroke.error().c_str());
        require(stroke.dabCount() == 3,
                "source opacity fixture dab count changed");
        for (int dab = 0; dab < 2; ++dab) {
          const Pixel actual  = canvas.read(50 + dab * 40, 64);
          const int *expected = captured[group][level][dab];
          require(
              actual.a > 0 &&
                  std::abs(actual.a * 255 - expected[3]) <= 1.000001 &&
                  std::abs(actual.r / actual.a * 255 - expected[0]) <=
                      1.000001 &&
                  std::abs(actual.g / actual.a * 255 - expected[1]) <=
                      1.000001 &&
                  std::abs(actual.b / actual.a * 255 - expected[2]) <= 1.000001,
              "source opacity transport differs from captured second/third "
              "dab");
        }
        if (level == 0)
          require(same(canvas, original), "zero source opacity painted");
        stroke.finish();
      }
    }
  }
}
void testSourceWetDisabledControls() {
  const int captured[3][2][2][4] = {
      {{{85, 0, 170, 192}, {153, 0, 102, 160}},
       {{85, 21, 149, 192}, {143, 36, 76, 164}}},
      {{{85, 0, 170, 192}, {153, 0, 102, 160}},
       {{78, 21, 156, 195}, {134, 36, 85, 168}}},
      {{{110, 0, 145, 179}, {185, 0, 70, 148}},
       {{100, 22, 133, 184}, {161, 36, 58, 157}}}};
  for (const std::string model :
       {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"}) {
    const int group = model == "new-smear" ? 1 : model == "new-dulling" ? 2 : 0;
    for (bool colorEnabled : {false, true})
      for (double value : {.25, .75}) {
        Canvas canvas;
        for (int y = 0; y < canvas.h; ++y)
          for (int x = 0; x < canvas.w; ++x)
            canvas.write(x, y,
                         x < 40 ? Pixel{0, 0, 1, 1}
                                : Pixel{128 / 255.0, 0, 0, 128 / 255.0});
        Preset p;
        p.profile = "krita-5.3.4.1-" + model + "-candidate-1";
        p.wet     = model.find("dulling") != std::string::npos ? Wet::Dulling
                                                               : Wet::Smear;
        p.tip.antialiasEdges     = false;
        p.size.value             = 8;
        p.spacing                = 5;
        p.smudgeRadius           = 0;
        p.color                  = {0, 1, 0, 1};
        p.sourceColorRateEnabled = colorEnabled;
        p.colorRate              = colorEnabled || value == .25 ? .25 : 1;
        if (colorEnabled)
          p.flow.mappings = {{Sensor::Pressure, {{0, 0}, {1, 1}}, 1}};
        Stroke stroke(canvas, p);
        require(stroke.add(sample(10, 64, 0, value)) &&
                    stroke.add(sample(91, 64, 1, value)),
                stroke.error().c_str());
        for (int dab = 0; dab < 2; ++dab) {
          const auto pixel    = canvas.read(50 + dab * 40, 64);
          const int *expected = captured[group][colorEnabled ? 1 : 0][dab];
          require(
              pixel.a > 0 &&
                  std::abs(pixel.a * 255 - expected[3]) <= 1.000001 &&
                  std::abs(pixel.r / pixel.a * 255 - expected[0]) <= 1.000001 &&
                  std::abs(pixel.g / pixel.a * 255 - expected[1]) <= 1.000001 &&
                  std::abs(pixel.b / pixel.a * 255 - expected[2]) <= 1.000001,
              "disabled color or inactive flow changed captured source wet "
              "state");
        }
        stroke.finish();
        p.opacity.mappings = {{Sensor::Pressure, {{0, .25}, {1, .75}}, 1}};
        require(!validate(p).empty(), "unmeasured wet opacity curve admitted");
        p.opacity.mappings.clear();
        p.flow.mappings = {{Sensor::TiltX, {{0, 0}, {1, 1}}, 1}};
        require(!validate(p).empty(), "unmeasured wet flow sensor admitted");
      }
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
    p.smudgeRadius = newDulling ? 3.01 : 1.01;
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
void testSourceDullingRadiusImpulses() {
  // Captured independently in the pinned source application: orthogonal
  // impulses distinguish footprint coordinates from color/alpha transport.
  struct Case {
    bool modern, row;
    int radius, offset, value, alpha;
    int diameter = 32;
  };
  const Case captured[] = {{false, false, 50, -8, 6, 255},
                           {false, false, 50, -4, 16, 255},
                           {false, false, 50, 0, 16, 255},
                           {false, false, 50, 4, 16, 255},
                           {false, false, 50, 8, 6, 255},
                           {false, false, 100, -16, 3, 255},
                           {false, false, 100, -8, 10, 255},
                           {false, false, 100, -4, 10, 255},
                           {false, false, 100, 0, 10, 255},
                           {false, false, 100, 4, 10, 255},
                           {false, false, 100, 8, 10, 255},
                           {false, false, 100, 16, 3, 255},
                           {false, true, 50, -16, 0, 255},
                           {false, true, 50, -8, 6, 255},
                           {false, true, 50, -4, 19, 255},
                           {false, true, 50, 0, 16, 255},
                           {false, true, 50, 4, 16, 255},
                           {false, true, 50, 8, 0, 255},
                           {false, true, 50, 16, 0, 255},
                           {false, true, 100, -16, 3, 255},
                           {false, true, 100, -8, 10, 255},
                           {false, true, 100, -4, 6, 255},
                           {false, true, 100, 0, 10, 255},
                           {false, true, 100, 4, 10, 255},
                           {false, true, 100, 8, 10, 255},
                           {false, true, 100, 16, 0, 255},
                           {true, false, 50, -8, 6, 204},
                           {true, false, 50, -4, 16, 204},
                           {true, false, 50, 0, 16, 204},
                           {true, false, 50, 4, 16, 204},
                           {true, false, 50, 8, 6, 204},
                           {true, false, 100, -16, 0, 204},
                           {true, false, 100, -8, 12, 204},
                           {true, false, 100, -4, 12, 204},
                           {true, false, 100, 0, 12, 204},
                           {true, false, 100, 4, 12, 204},
                           {true, false, 100, 8, 12, 204},
                           {true, false, 100, 16, 0, 204},
                           {true, false, 300, -16, 0, 204},
                           {true, false, 300, -8, 12, 204},
                           {true, false, 300, -4, 12, 204},
                           {true, false, 300, 0, 12, 204},
                           {true, false, 300, 4, 12, 204},
                           {true, false, 300, 8, 12, 204},
                           {true, false, 300, 16, 0, 204},
                           {true, true, 50, -16, 0, 204},
                           {true, true, 50, -8, 6, 204},
                           {true, true, 50, -4, 19, 204},
                           {true, true, 50, 0, 16, 204},
                           {true, true, 50, 4, 16, 204},
                           {true, true, 50, 8, 0, 204},
                           {true, true, 50, 16, 0, 204},
                           {true, true, 100, -16, 0, 204},
                           {true, true, 100, -8, 12, 204},
                           {true, true, 100, -4, 4, 204},
                           {true, true, 100, 0, 12, 204},
                           {true, true, 100, 4, 12, 204},
                           {true, true, 100, 8, 8, 204},
                           {true, true, 100, 16, 0, 204},
                           {false, false, 100, -9, 0, 255, 16},
                           {false, false, 100, -8, 6, 255, 16},
                           {false, false, 100, -4, 16, 255, 16},
                           {false, false, 100, 0, 16, 255, 16},
                           {false, false, 100, 4, 16, 255, 16},
                           {false, false, 100, 8, 6, 255, 16},
                           {false, false, 100, 9, 0, 255, 16},
                           {false, true, 100, -9, 0, 255, 16},
                           {false, true, 100, -8, 6, 255, 16},
                           {false, true, 100, -4, 19, 255, 16},
                           {false, true, 100, 0, 16, 255, 16},
                           {false, true, 100, 4, 16, 255, 16},
                           {false, true, 100, 8, 0, 255, 16},
                           {false, true, 100, 9, 0, 255, 16},
                           {false, false, 100, -33, 0, 255, 64},
                           {false, false, 100, -32, 0, 255, 64},
                           {false, false, 100, -16, 3, 255, 64},
                           {false, false, 100, 0, 3, 255, 64},
                           {false, false, 100, 16, 3, 255, 64},
                           {false, false, 100, 32, 0, 255, 64},
                           {false, false, 100, 33, 0, 255, 64},
                           {false, true, 100, -33, 0, 255, 64},
                           {false, true, 100, -32, 3, 255, 64},
                           {false, true, 100, -16, 5, 255, 64},
                           {false, true, 100, 0, 3, 255, 64},
                           {false, true, 100, 16, 3, 255, 64},
                           {false, true, 100, 32, 0, 255, 64},
                           {false, true, 100, 33, 0, 255, 64},
                           {true, false, 100, -9, 0, 204, 16},
                           {true, false, 100, -8, 0, 204, 16},
                           {true, false, 100, -4, 20, 204, 16},
                           {true, false, 100, 0, 20, 204, 16},
                           {true, false, 100, 4, 20, 204, 16},
                           {true, false, 100, 8, 0, 204, 16},
                           {true, false, 100, 9, 0, 204, 16},
                           {true, true, 100, -9, 0, 204, 16},
                           {true, true, 100, -8, 4, 204, 16},
                           {true, true, 100, -4, 24, 204, 16},
                           {true, true, 100, 0, 20, 204, 16},
                           {true, true, 100, 4, 16, 204, 16},
                           {true, true, 100, 8, 0, 204, 16},
                           {true, true, 100, 9, 0, 204, 16},
                           {true, false, 100, -33, 0, 204, 64},
                           {true, false, 100, -32, 0, 204, 64},
                           {true, false, 100, -16, 3, 204, 64},
                           {true, false, 100, 0, 3, 204, 64},
                           {true, false, 100, 16, 3, 204, 64},
                           {true, false, 100, 32, 0, 204, 64},
                           {true, false, 100, 33, 0, 204, 64},
                           {true, true, 100, -33, 0, 204, 64},
                           {true, true, 100, -32, 3, 204, 64},
                           {true, true, 100, -16, 3, 204, 64},
                           {true, true, 100, 0, 3, 204, 64},
                           {true, true, 100, 16, 3, 204, 64},
                           {true, true, 100, 32, 0, 204, 64},
                           {true, true, 100, 33, 0, 204, 64}};
  for (const auto &fixture : captured) {
    Preset p;
    p.profile    = fixture.modern ? "krita-5.3.4.1-new-dulling-candidate-1"
                                  : "krita-5.3.4.1-legacy-dulling-candidate-1";
    p.wet        = Wet::Dulling;
    p.size.value = fixture.diameter;
    p.spacing    = 2;
    p.tip.antialiasEdges = false;
    p.pickup             = 1;
    p.colorRate          = 0;
    p.smudgeRadius       = fixture.radius / 100.0;
    require(validate(p).empty(), "source Dulling fixture rejected");
    struct FullCoverageCanvas : Canvas {
      double coverage(int, int) const override { return 1; }
    } canvas;
    canvas.w = 512;
    canvas.h = 256;
    canvas.pixels.assign(size_t(canvas.w) * canvas.h, Pixel{});
    for (int y = 0; y < canvas.h; ++y)
      for (int x = 0; x < 128 + fixture.diameter; ++x) {
        const double white =
            fixture.row ? y == 128 + fixture.offset : x == 128 + fixture.offset;
        canvas.write(x, y, {white, white, white, 1});
      }
    Stroke stroke(canvas, p);
    require(stroke.add(sample(128, 128, 0)), stroke.error().c_str());
    require(stroke.add(sample(129 + 2 * fixture.diameter, 128, 1)),
            stroke.error().c_str());
    stroke.finish();
    const Pixel pixel = canvas.read(128 + 2 * fixture.diameter, 128);
    require(std::abs(pixel.a * 255 - fixture.alpha) < 1e-9 &&
                std::abs(pixel.r / pixel.a * 255 - fixture.value) < 1e-9 &&
                pixel.r == pixel.g && pixel.g == pixel.b,
            "Dulling sampling differs from original orthogonal impulse");
  }
}
void testSourceWetPressureColorAttenuation() {
  struct WideCanvas : Canvas {
    WideCanvas() {
      w = 512;
      pixels.resize(size_t(w) * h);
    }
    double coverage(int, int) const override { return 1; }
  };
  const int captured[2][2][4] = {{{199, 7, 49, 144}, {241, 9, 6, 132}},
                                 {{116, 17, 121, 176}, {181, 27, 47, 150}}};
  for (const auto &model : {"legacy-smear", "legacy-dulling"})
    for (int i = 0; i < 2; ++i) {
      Preset p;
      p.profile = std::string("krita-5.3.4.1-") + model + "-candidate-1";
      p.wet = std::string(model) == "legacy-smear" ? Wet::Smear : Wet::Dulling;
      p.size.value            = 32;
      p.spacing               = 5;
      p.tip.antialiasEdges    = false;
      p.smudgeRadius          = 0;
      p.pickup                = .5;
      p.colorRate             = .25;
      p.color                 = {0, 1, 0, 1};
      p.pickupControl.minimum = 0;
      p.pickupControl.mappings.push_back(
          {Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
      WideCanvas canvas;
      for (int y = 0; y < canvas.h; ++y)
        for (int x = 0; x < canvas.w; ++x)
          canvas.write(x, y,
                       x < 112 ? Pixel{0, 0, 1, 1}
                               : Pixel{128 / 255.0, 0, 0, 128 / 255.0});
      Stroke stroke(canvas, p);
      const double pressure = i ? .75 : .25;
      require(stroke.add(sample(32, 64, 0, pressure)), stroke.error().c_str());
      require(stroke.add(sample(353, 64, 1, pressure)), stroke.error().c_str());
      stroke.finish();
      for (int dab = 0; dab < 2; ++dab) {
        const Pixel pixel     = canvas.read(192 + 160 * dab, 64);
        const double actual[] = {pixel.r / pixel.a, pixel.g / pixel.a,
                                 pixel.b / pixel.a, pixel.a};
        for (int channel = 0; channel < 4; ++channel)
          require(
              std::abs(actual[channel] * 255 - captured[i][dab][channel]) <= 1,
              "legacy wet pressure changed static color attenuation");
      }
    }
}
void testSourceWashLattice() {
  Preset p;
  p.profile            = "krita-5.3.4.1-hard-pixel-candidate-1";
  p.size.value         = 32;
  p.tip.hardness       = 1;
  p.tip.antialiasEdges = false;
  p.wash               = true;
  p.flow.value         = .25;
  p.spacing            = .1;
  Canvas canvas;
  Stroke stroke(canvas, p);
  require(stroke.add(sample(32, 64, 0)), stroke.error().c_str());
  require(stroke.add(sample(96, 64, 1)), stroke.error().c_str());
  stroke.finish();
  // Source tablet alpha window at (768,1024), translated by (-736,-960).
  // Fixed .1 spacing and quarter flow expose individual overlapping dabs.
  const int captured[][3] = {{16, 64, 0},   {17, 64, 64},  {20, 64, 112},
                             {23, 64, 148}, {26, 64, 175}, {29, 64, 195},
                             {20, 74, 64},  {29, 79, 64}};
  for (const auto &pixel : captured)
    require(std::abs(canvas.read(pixel[0], pixel[1]).a * 255 - pixel[2]) <= 1,
            "source wash dab placement differs from captured tablet alpha");
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
void testSourceSmallHardCircleAA() {
  // Complete original source masks from integer-position single-dab captures.
  // D5/6 are held out for the source application's full-image CI comparison.
  struct Capture {
    int diameter;
    std::vector<int> alpha;
  };
  const Capture captures[] = {
      {1, {78}},
      {2, {115, 194, 37, 70}},
      {3, {69, 98, 10, 210, 240, 74, 133, 174, 29}},
      {4,
       {30, 171, 196, 75, 97, 246, 254, 174, 59, 213, 232, 121, 0, 46, 64, 8}},
      {7, {0,   22,  76,  87,  41,  2,   0,   38,  170, 236, 244, 200, 82,
           0,   140, 247, 255, 255, 254, 193, 28,  195, 255, 255, 255, 255,
           235, 62,  165, 253, 255, 255, 255, 213, 42,  71,  208, 252, 255,
           231, 125, 5,   3,   62,  145, 161, 94,  13,  0}},
      {8, {0,   15,  101, 177, 192, 132, 37,  0,   7,   128, 232, 255, 255,
           246, 174, 32,  54,  218, 255, 255, 255, 255, 243, 113, 88,  246,
           255, 255, 255, 255, 255, 164, 70,  233, 255, 255, 255, 255, 251,
           136, 19,  166, 248, 255, 255, 254, 206, 58,  0,   43,  158, 223,
           233, 187, 82,  3,   0,   0,   12,  49,  59,  24,  0,   0}},
      {9, {0,   1,   32,  76,  85,  48,  6,   0,   0,   4,   86,  192, 239, 246,
           214, 126, 18,  0,   64,  208, 254, 255, 255, 255, 234, 119, 4,   150,
           252, 255, 255, 255, 255, 255, 203, 32,  195, 255, 255, 255, 255, 255,
           255, 236, 60,  170, 255, 255, 255, 255, 255, 255, 219, 44,  95,  231,
           255, 255, 255, 255, 248, 153, 10,  15,  134, 227, 254, 255, 242, 174,
           45,  0,   0,   12,  80,  147, 161, 105, 30,  0,   0}}};
  Preset p;
  p.profile            = "krita-5.3.4.1-default-circle-candidate-1";
  p.tip.antialiasEdges = true;
  for (const auto &capture : captures) {
    p.size.value = capture.diameter;
    Canvas canvas;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(64, 64, 0)), stroke.error().c_str());
    stroke.finish();
    const int left = 64 - capture.diameter / 2;
    const int top  = int(std::floor(64 - capture.diameter / 2.0));
    for (int y = 0; y < canvas.h; ++y)
      for (int x = 0; x < canvas.w; ++x) {
        const int expected =
            x >= left && x < left + capture.diameter && y >= top &&
                    y < top + capture.diameter
                ? capture.alpha[(y - top) * capture.diameter + x - left]
                : 0;
        require(std::abs(canvas.read(x, y).a * 255 - expected) < 1e-9,
                "small hard-AA complete source mask differs");
      }
  }
  for (double size : {4.5, 10.0, 12.0}) {
    p.size.value = size;
    require(!validate(p).empty(), "unmeasured small-AA size accepted");
  }
  p.size.value   = 4;
  p.tip.hardness = .5;
  require(!validate(p).empty(), "unmeasured small-AA fade accepted");
  p.tip.hardness  = 1;
  p.size.mappings = {{Sensor::Pressure, {{0, 0}, {1, 1}}, 1}};
  require(!validate(p).empty(), "dynamic small-AA size accepted");
}
void testSourcePixelBlendCandidates() {
  // Analytic encoded-sRGB Porter-Duff/separable blend examples, not source
  // oracle pixels. Source rounding and accumulation are gated separately by
  // the original pixelblend application captures.
  struct Case {
    Blend mode;
    Pixel half, opaque;
  };
  const Case cases[] = {
      {Blend::Normal, {.4625, .2875, .325, .75}, {.525, .475, .45, 1}},
      {Blend::Multiply, {.3125, .275, .275, .75}, {.225, .45, .35, 1}},
      {Blend::Screen, {.475, .4375, .4, .75}, {.55, .775, .6, 1}},
      {Blend::Overlay, {.3625, .3875, .325, .75}, {.325, .675, .45, 1}}};
  auto equal = [](Pixel a, Pixel b) {
    return std::abs(a.r - b.r) < 1e-12 && std::abs(a.g - b.g) < 1e-12 &&
           std::abs(a.b - b.b) < 1e-12 && std::abs(a.a - b.a) < 1e-12;
  };
  for (const auto &entry : cases) {
    for (double alpha : {0.0, .5, 1.0}) {
      const Pixel base{.25 * alpha, .75 * alpha, .5 * alpha, alpha};
      const Pixel expected = alpha == 0    ? Pixel{.4, .1, .2, .5}
                             : alpha == .5 ? entry.half
                                           : entry.opaque;
      Preset p;
      p.profile       = "krita-5.3.4.1-hard-pixel-candidate-1";
      p.color         = {.8, .2, .4, 1};
      p.size.value    = 16;
      p.spacing       = .125;
      p.opacity.value = .5;
      p.flow.value = 0;  // Fixed source build-up ignores inactive static flow.
      p.blend      = entry.mode;
      Canvas build;
      for (auto &pixel : build.pixels) pixel = base;
      Stroke first(build, p);
      require(first.add(sample(32, 32, 0)), first.error().c_str());
      require(equal(build.read(32, 32), expected),
              "source blend analytic alpha case");
      p.wash       = true;
      p.flow.value = .25;
      Canvas wash;
      for (auto &pixel : wash.pixels) pixel = base;
      Stroke accumulated(wash, p);
      require(accumulated.add(sample(32, 32, 0)), accumulated.error().c_str());
      require(accumulated.add(sample(34.5, 32, 1)),
              accumulated.error().c_str());
      // Two .25-flow stamps: stroke-buffer alpha .4375, then .5 opacity.
      // The blend is applied once against the stroke's original backdrop.
      const Pixel twice{base.r + .4375 * (expected.r - base.r),
                        base.g + .4375 * (expected.g - base.g),
                        base.b + .4375 * (expected.b - base.b),
                        base.a + .4375 * (expected.a - base.a)};
      require(equal(wash.read(32, 32), twice),
              "source wash blended against changing backdrop");
      p.flow.value = 0;
      Canvas empty;
      for (auto &pixel : empty.pixels) pixel = base;
      Stroke zero(empty, p);
      require(zero.add(sample(32, 32, 0)), zero.error().c_str());
      require(equal(empty.read(32, 32), base),
              "zero-flow source wash changed backdrop");
    }
    Preset wet;
    wet.profile = "krita-5.3.4.1-legacy-dulling-candidate-1";
    wet.wet     = Wet::Dulling;
    wet.blend   = entry.mode;
    require(validate(wet).empty() == (entry.mode == Blend::Normal),
            "Pixel blend contract changed source wet eligibility");
  }
}
void testSourceWetWorkBounds() {
  struct GuardSurface : Surface {
    int side;
    size_t prepared      = 0;
    mutable size_t reads = 0;
    size_t writes        = 0;
    explicit GuardSurface(int size) : side(size) {}
    int width() const override { return side; }
    int height() const override { return side; }
    bool prepare(int x0, int y0, int x1, int y1) override {
      require(
          x0 >= 0 && y0 >= 0 && x1 <= side && y1 <= side && x0 < x1 && y0 < y1,
          "source wet preflight used unclipped bounds");
      ++prepared;
      return true;
    }
    Pixel read(int, int) const override {
      ++reads;
      return {0, 0, 0, 1};
    }
    void write(int, int, const Pixel &) override { ++writes; }
  };
  for (const std::string model :
       {"legacy-smear", "new-smear", "legacy-dulling", "new-dulling"}) {
    Preset p;
    p.profile = "krita-5.3.4.1-" + model + "-candidate-1";
    p.wet =
        model.find("dulling") == std::string::npos ? Wet::Smear : Wet::Dulling;
    p.size.value   = 4096;
    p.smudgeRadius = p.wet == Wet::Smear ? 0 : model == "new-dulling" ? 3 : 1;
    require(validate(p).empty(), "extreme source geometry eligibility changed");
    // Smear's unclipped snapshot footprint exceeds the work budget. Dulling's
    // clipped 4096-square footprint exactly consumes it, so even radius-zero
    // center pickup must be charged before any host operation. Nonzero-radius
    // cases additionally cover the real area-scaled physical sample count.
    GuardSurface guard(p.wet == Wet::Smear ? 8192 : 4096);
    auto budget = std::make_shared<MemoryBudget>(1024u * 1024u * 1024u);
    for (double radius : {0.0, p.smudgeRadius}) {
      p.smudgeRadius = radius;
      Stroke denied(guard, p, 1024u * 1024u * 1024u, budget);
      require(!denied.add(sample(guard.side / 2, guard.side / 2, 0)) &&
                  denied.error() == "Stroke input pixel work limit exceeded",
              "source wet bypassed the per-add work budget");
      require(guard.prepared == 0 && guard.reads == 0 && guard.writes == 0 &&
                  budget->used() == 0,
              "source wet work failure touched the host or allocated snapshot");
      require(!denied.add(sample(0, 0, 1)), "failed source wet stroke resumed");
      denied.finish();
      require(budget->used() == 0, "failed source wet stroke leaked budget");
    }
    // Charge clipped raster area rather than a theoretical full brush square.
    // A normal small corner snapshot remains legal and releases its state.
    p.size.value   = 32;
    p.smudgeRadius = p.wet == Wet::Smear ? 0 : .5;
    GuardSurface corner(1);
    Stroke clipped(corner, p, 1024 * 1024, budget);
    require(clipped.add(sample(0, 0, 0)), clipped.error().c_str());
    require(corner.prepared == 1 && budget->used() == sizeof(Pixel),
            "clipped source wet snapshot charged the wrong raster area");
    clipped.finish();
    require(budget->used() == 0, "clipped source wet snapshot leaked budget");
    GuardSurface outside(1);
    Stroke offCanvas(outside, p);
    require(offCanvas.add(sample(10000, 10000, 0)) && outside.prepared == 0 &&
                outside.reads == 0 && outside.writes == 0,
            "empty clipped source wet footprint touched the host");
  }
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
void testIndependentSecondarySpacing() {
  Preset p;
  p.size.value                   = 8;
  p.spacing                      = .25;
  p.secondary.enabled            = true;
  p.secondary.independentSpacing = true;
  p.secondary.size.value         = .5;
  p.secondary.spacing            = 2;
  Canvas whole, split;
  auto budget = std::make_shared<MemoryBudget>(1024 * 1024);
  Stroke stroke(whole, p, 1024 * 1024, budget);
  require(stroke.add(sample(16, 64, 0)), stroke.error().c_str());
  require(stroke.add(sample(48, 64, 1)), stroke.error().c_str());
  require(whole.read(16, 64).a == 1 && whole.read(24, 64).a == 1,
          "secondary origin/tied-position stamp missing");
  require(whole.read(20, 64).a == 0,
          "secondary independently spaced gap filled by primary cadence");
  require(budget->used() > 0, "independent mask not charged to shared budget");
  stroke.finish();
  require(budget->used() == 0, "independent mask retained after finish");
  Stroke partitioned(split, p);
  for (int i = 0; i <= 16; ++i)
    require(partitioned.add(sample(16 + 2 * i, 64, i / 16.0)),
            partitioned.error().c_str());
  partitioned.finish();
  require(same(whole, split),
          "secondary scheduling changed with event partition");
  p.secondary.independentSpacing = false;
  Canvas synchronized;
  line(synchronized, p);
  require(synchronized.read(20, 64).a == 1,
          "legacy synchronized secondary behavior changed");
  p.secondary.independentSpacing = true;
  p.secondary.mode               = MaskMode::Subtract;
  Canvas subtract;
  Stroke inverse(subtract, p);
  require(inverse.add(sample(16, 64, 0)), inverse.error().c_str());
  require(inverse.add(sample(48, 64, 1)), inverse.error().c_str());
  require(subtract.read(16, 64).a == 0 && subtract.read(20, 64).a == 1,
          "independent subtract mask composition order changed");

  // Count unions each stamp's opacity before masking the primary. A stationary
  // airbrush event reuses the mask; it must not emit additional secondary dabs.
  p.secondary.mode          = MaskMode::Multiply;
  p.secondary.count         = 2;
  p.secondary.opacity.value = .5;
  p.airbrushRate            = 1;
  Canvas stationary;
  Stroke clock(stationary, p);
  require(clock.add(sample(32, 64, 0)), clock.error().c_str());
  require(stationary.read(32, 64).a == .75,
          "independent secondary count/opacity union incorrect");
  require(clock.add(sample(32, 64, 1)), clock.error().c_str());
  require(stationary.read(32, 64).a == .9375,
          "stationary primary airbrush rescheduled secondary path");

  // A failed mask allocation happens before publishing the first primary dab.
  Canvas denied;
  auto tiny = std::make_shared<MemoryBudget>(1);
  {
    Stroke bounded(denied, p, 1, tiny);
    require(!bounded.add(sample(32, 64, 0)), "secondary mask exceeded budget");
    require(denied.read(32, 64).a == 0, "denied initial mask published paint");
  }
  require(tiny->used() == 0, "failed secondary mask leaked memory charge");
  p.size.value           = 4096;
  p.secondary.size.value = 2;
  require(!validate(p).empty(), "oversize independent geometry accepted");
}
void testIndependentSecondaryDynamics() {
  Preset p;
  p.size.value                   = 16;
  p.spacing                      = .25;
  p.secondary.enabled            = true;
  p.secondary.independentSpacing = true;
  p.secondary.size.value         = 1;
  p.secondary.size.mappings      = {{Sensor::Pressure, {{0, 0}, {1, 1}}, 1}};
  p.secondary.spacing            = 1;
  p.secondary.opacity.value      = .5;
  p.secondary.scatter            = .125;
  p.secondary.count              = 2;
  p.secondary.angle.mappings     = {{Sensor::RandomDab, {{0, 0}, {1, 1}}, 1}};
  p.secondary.angle.value        = 90;
  auto image                     = std::make_shared<Image>();
  image->width                   = 2;
  image->height                  = 1;
  image->pixels                  = {{0, 0, 0, 1}, {}};
  p.secondary.tip.shape          = Shape::Bitmap;
  p.secondary.tip.images         = {image};
  Canvas whole, split, repeated;
  auto draw = [&](Canvas &canvas, bool subdivide) {
    Stroke stroke(canvas, p);
    require(stroke.add(sample(16, 64, 0, .5)), stroke.error().c_str());
    if (subdivide)
      require(stroke.add(sample(32, 64, .5, .75)), stroke.error().c_str());
    require(stroke.add(sample(48, 64, 1, 1)), stroke.error().c_str());
    stroke.finish();
  };
  draw(whole, false);
  draw(split, true);
  draw(repeated, false);
  require(same(whole, split) && same(whole, repeated),
          "secondary pressure/bitmap/random dynamics depend on event partition "
          "or history");
  p.secondary.flipX = true;
  Canvas flipped;
  draw(flipped, false);
  require(!same(whole, flipped), "independent secondary bitmap flip ignored");
}
void testNativeSpacingAndWorkLimits() {
  Preset p;
  p.size.value = .25;
  p.spacing    = .01;
  Canvas canvas;
  Stroke stroke(canvas, p);
  require(stroke.add(sample(32, 32, 0)), stroke.error().c_str());
  require(stroke.add(sample(32.009, 32, 1)), stroke.error().c_str());
  require(stroke.dabCount() == 4,
          "native spacing used an absolute .01 pixel floor");
  // Off-canvas traces exercise scheduler bounds cheaply without raster work.
  Stroke bounded(canvas, p);
  require(bounded.add(sample(-1000, -1000, 0)), bounded.error().c_str());
  require(!bounded.add(sample(-500, -1000, 1)) &&
              bounded.error() == "Stroke input event work limit exceeded",
          "tiny-spacing huge input event did not report bounded failure");
  p.secondary.enabled = p.secondary.independentSpacing = true;
  Stroke independent(canvas, p);
  require(independent.add(sample(-1000, -1000, 0)),
          independent.error().c_str());
  require(!independent.add(sample(-500, -1000, 1)) &&
              independent.error() == "Stroke input event work limit exceeded",
          "independent scheduler did not bound combined event work");
  struct WorkGuard : Surface {
    bool touched = false;
    int width() const override { return 2048; }
    int height() const override { return 2048; }
    bool prepare(int, int, int, int) override {
      touched = true;
      return true;
    }
    Pixel read(int, int) const override { return {}; }
    void write(int, int, const Pixel &) override { touched = true; }
  } guard;
  p.size.value                   = 4096;
  p.tip.antialiasEdges           = true;
  p.count                        = 64;
  p.secondary.independentSpacing = false;
  p.secondary.count              = 64;
  p.secondary.tip.antialiasEdges = true;
  Stroke extreme(guard, p);
  require(!extreme.add(sample(1024, 1024, 0)) &&
              extreme.error() == "Stroke input pixel work limit exceeded" &&
              !guard.touched,
          "extreme dual/filter work was not rejected before raster access");
  p.secondary.independentSpacing = true;
  Stroke extremeIndependent(guard, p);
  require(!extremeIndependent.add(sample(1024, 1024, 0)) &&
              extremeIndependent.error() ==
                  "Stroke input pixel work limit exceeded" &&
              !guard.touched,
          "extreme independent mask work was not bounded before raster access");
}
}  // namespace
int main() {
  try {
    testFloatTransaction();
    testComposition();
    testAreaCoverage();
    testDeterminismAndMasks();
    testDisabledSecondaryAndExtremeTransforms();
    testNativeTextureCoverage();
    testWashAndInput();
    testResourcesAndWet();
    testWetPressure();
    testWetPickupGeometry();
    testCountFadeAndDirection();
    testSecondaryCountAndMirror();
    testIndependentSecondarySpacing();
    testIndependentSecondaryDynamics();
    testNativeSpacingAndWorkLimits();
    testKritaHardPixelCandidate();
    testStationaryAndBatching();
    testColorDynamics();
    testCubicCurves();
    testSourceDefaultCircle();
    testSourceDullingRadiusImpulses();
    testSourceWetPressureColorAttenuation();
    testSourceWashLattice();
    testSourceSmallCircle();
    testSourceHardCircleAA();
    testSourceWetWorkBounds();
    testSourcePixelBlendCandidates();
    testSourceSmallHardCircleAA();
    testSourceRectangle();
    testSourceWetInitialization();
    testSourceWetOpacityTransport();
    testSourceWetDisabledControls();
    testLongWetTrace();
    testNativeWetAnalyticalDrift();
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  std::cout << "Native raster engine regression passed\n";
}
