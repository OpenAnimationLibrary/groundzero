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
    testDeterminismAndMasks();
    testWashAndInput();
    testResourcesAndWet();
    testWetPressure();
    testStationaryAndBatching();
    testColorDynamics();
    testCubicCurves();
    testLongWetTrace();
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  std::cout << "Native raster engine regression passed\n";
}
