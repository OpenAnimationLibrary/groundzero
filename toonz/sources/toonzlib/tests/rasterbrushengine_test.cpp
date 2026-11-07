#include "toonz/rasterbrushengine.h"
#include "../rasterbrushsampling.h"
#include "../rasterbrushgihselection.h"
#include "rasterbrushbitmapreference.h"
#include <cmath>
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <utility>
using namespace RasterBrush;
namespace {
void require(bool v, const char *s) {
  if (!v) throw std::runtime_error(s);
}
void finishStroke(RasterBrush::Stroke &stroke) {
  const bool success = stroke.finish();
  require(success, stroke.error().c_str());
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
void sourceFixtureMotion(Stroke &stroke, Sample contact) {
  // Original component masks were acquired from a moving line, not a tap.
  contact.x += .5;
  contact.time += .001;
  require(stroke.add(contact), stroke.error().c_str());
}
void line(Canvas &c, Preset p) {
  Stroke s(c, p);
  for (int i = 0; i <= 120; ++i)
    require(s.add(sample(i, 64, i / 240.0)), s.error().c_str());
  finishStroke(s);
}
void testSourceGeneratedMasks() {
  auto preset = [](SourceGeneratedGenerator generator, SourceGeneratedShape shape,
                   double diameter, double horizontal, double vertical,
                   const std::vector<Knot> &curve = {}) {
    Preset p;
    p.profile = "krita-5.3.4.1-generated-mask-candidate-1";
    p.size.value = diameter;
    p.spacing = 1;
    p.tip.shape = shape == SourceGeneratedShape::Circle ? Shape::Round : Shape::Square;
    p.tip.antialiasEdges = true;
    SourceGeneratedDefinitionV1 definition;
    definition.generator = generator;
    definition.shape = shape;
    definition.diameter = diameter;
    definition.horizontalFade = horizontal;
    definition.verticalFade = vertical;
    definition.antialiasEdges = true;
    definition.softnessCurve = curve;
    p.sourceGeneratedV1 = definition;
    return p;
  };
  auto anchors = [&](const Preset &p, double originX,
                     const std::vector<std::array<int, 3>> &expected) {
    require(validate(p).empty(), "Measured generated source candidate rejected");
    Canvas canvas;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(64 + originX, 64, 0)) &&
                stroke.add(sample(65 + originX, 64, .01)), stroke.error().c_str());
    finishStroke(stroke);
    require(stroke.dabCount() == 1, "Generated source short stroke changed dab count");
    for (const auto &point : expected)
      require(std::abs(std::floor(canvas.read(64 + point[0], 64 + point[1]).a * 255 + .5) -
                           point[2]) <= 1,
              "Generated mask differs from independently retained source pixel");
  };
  // Original full-raster source captures, retained before this implementation.
  // The fixed bytes exercise nonuniform cubic curves, off-grid block AA and
  // independent Gaussian axes; tests do not recalculate the model equations.
  anchors(preset(SourceGeneratedGenerator::Soft, SourceGeneratedShape::Circle,
                 16, .5, .5, {{0, 0}, {.25, .75}, {1, 1}}), 0,
          {{0, 0, 3}, {4, 0, 191}, {5, 0, 254}, {7, 0, 251}, {5, 5, 149}});
  anchors(preset(SourceGeneratedGenerator::Soft, SourceGeneratedShape::Rectangle,
                 16, .5, .5, {{0, 1}, {.5, .2}, {1, 0}}), 0,
          {{0, 0, 223}, {1, 0, 168}, {2, 0, 116}, {4, 0, 36}});
  anchors(preset(SourceGeneratedGenerator::Soft, SourceGeneratedShape::Rectangle,
                 16, .5, .5, {{0, 0}, {1, 1}}), .125,
          {{-7, -8, 78}, {2, -8, 12}, {7, -8, 166}, {8, -8, 13}, {7, -7, 124}});
  anchors(preset(SourceGeneratedGenerator::Soft, SourceGeneratedShape::Rectangle,
                 64, .5, .5, {{0, 0}, {1, 1}}), .125,
          {{-31, -32, 102}, {-25, -32, 76}, {-24, -32, 140},
           {25, -32, 149}, {31, -32, 230}, {32, -32, 15}});
  anchors(preset(SourceGeneratedGenerator::Gaussian, SourceGeneratedShape::Circle,
                 16, 0, 0), 0, {{0, 0, 251}, {3, 0, 137}, {0, 7, 3}, {5, 5, 3}});
  anchors(preset(SourceGeneratedGenerator::Gaussian, SourceGeneratedShape::Circle,
                 64, .5, .5), 0,
          {{20, 0, 155}, {21, 0, 132}, {22, 0, 108}, {31, 0, 2}, {22, 22, 1}});
  anchors(preset(SourceGeneratedGenerator::Gaussian, SourceGeneratedShape::Circle,
                 64, .1, .9), 0,
          {{20, 0, 155}, {21, 0, 132}, {22, 0, 108}, {0, 21, 120}, {22, 22, 1}});
  anchors(preset(SourceGeneratedGenerator::Gaussian, SourceGeneratedShape::Rectangle,
                 64, .1, .9), 0,
          {{0, 0, 255}, {12, 0, 201}, {0, 23, 119}, {12, 20, 157}, {-31, 20, 2}, {31, 20, 2}});
  auto measured = preset(SourceGeneratedGenerator::Soft, SourceGeneratedShape::Circle,
                         16, .5, .5, {{0, 0}, {1, 1}});
  for (int mutation = 0; mutation < 8; ++mutation) {
    auto p = measured;
    if (mutation == 0) p.profile = "groundzero-native-1";
    if (mutation == 1) p.sourceGeneratedV1.reset();
    if (mutation == 2) p.sourceGeneratedV1->antialiasEdges = false;
    if (mutation == 3) p.angle.value = 15;
    if (mutation == 4) p.aspect.value = .5;
    if (mutation == 5) p.size.value = p.sourceGeneratedV1->diameter = 32;
    if (mutation == 6) p.blend = Blend::Erase;
    if (mutation == 7) p.size.mappings.push_back({});
    require(!validate(p).empty(), "Unmeasured generated source setting was admitted");
  }
  // These operations consume generated coverage through the same independently
  // established Pixel transfer/compositor, rather than changing mask geometry.
  for (auto blend : {Blend::Normal, Blend::Multiply, Blend::Screen, Blend::Overlay})
    for (bool wash : {false, true}) {
      auto p = measured;
      p.blend = blend;
      p.wash = wash;
      p.opacity.value = .5;
      p.flow.value = .25;
      p.spacing = .25;
      require(validate(p).empty(), "Shared Pixel transfer was blocked for generated coverage");
    }
  for (bool flow : {false, true}) {
    auto p = measured;
    auto &control = flow ? p.flow : p.opacity;
    control.value = .5;
    control.mappings.push_back({Sensor::Pressure, {{0, .25}, {.5, .75}, {1, .5}},
                                1, Interpolation::NaturalCubic});
    require(validate(p).empty(), "Measured Pixel pressure transfer was not shared");
    control.mappings.front().sensor = Sensor::Time;
    require(!validate(p).empty(), "Unmeasured generated Time transfer was admitted");
  }
  Canvas untouched;
  Stroke bounded(untouched, measured, 1);
  require(!bounded.add(sample(64, 64, 0)) && bounded.dabCount() == 0,
          "Generated source snapshot ignored its memory budget");
  // The maximum admitted source curve must reserve its complete temporary
  // solver peak before construction, not merely the coefficients it retains.
  auto maximum = measured;
  auto &knots = maximum.sourceGeneratedV1->softnessCurve;
  knots.clear();
  for (int index = 0; index < 1024; ++index)
    knots.push_back({double(index) / 1023, double(index) / 1023});
  require(validate(maximum).empty(), "legal 1024-knot generated curve rejected");
  struct GeneratedGuard : Canvas {
    int prepares = 0, writes = 0;
    mutable int reads = 0;
    bool prepare(int x0, int y0, int x1, int y1) override {
      ++prepares;
      return Canvas::prepare(x0, y0, x1, y1);
    }
    Pixel read(int x, int y) const override {
      ++reads;
      return Canvas::read(x, y);
    }
    void write(int x, int y, const Pixel &pixel) override {
      ++writes;
      Canvas::write(x, y, pixel);
    }
  } guard;
  auto memory = std::make_shared<MemoryBudget>(1024 * 1024);
  Stroke measurement(guard, maximum, 1024 * 1024, memory);
  const size_t retained = memory->used();
  require(measurement.error().empty() && retained >= 1024 * sizeof(Knot),
          "maximum generated curve escaped its snapshot/cache charge");
  finishStroke(measurement);
  require(memory->used() == 0, "generated coefficient snapshot leaked storage");
  const size_t peak = retained + 1024 * sizeof(double);
  {
    auto oneShort = std::make_shared<MemoryBudget>(peak - 1);
    Stroke denied(guard, maximum, peak - 1, oneShort);
    require(denied.error() == "Generated curve memory limit exceeded" &&
                oneShort->used() == 0 && !denied.add(sample(64, 64, 0)),
            "generated solver did not reject its one-byte-short peak before allocation");
  }
  {
    auto exact = std::make_shared<MemoryBudget>(peak);
    Stroke allowed(guard, maximum, peak, exact);
    require(allowed.error().empty() && exact->used() == retained,
            "generated solver changed its declared complete construction peak");
    finishStroke(allowed);
    require(exact->used() == 0, "completed generated solver retained storage");
  }
  require(guard.prepares == 0 && guard.reads == 0 && guard.writes == 0,
          "generated preprocessing touched the host before input");
}

void testSourceAirbrushDispatch() {
  Preset preset;
  preset.profile       = "krita-5.3.4.1-airbrush-candidate-1";
  preset.size.value    = 16;
  preset.spacing       = 1;
  preset.opacity.value = .1;
  preset.airbrushRate  = 5;
  preset.color         = {220 / 255., 40 / 255., 30 / 255., 1};
  require(validate(preset).empty(), "Static airbrush candidate rejected");
  // Original off-boundary stationary source captures: 5Hz/.375s and1.125s,
  // 20Hz/.375s and1.125s. Ticks are explicitly supplied, never count-fitted.
  const int counts[]      = {2, 6, 8, 23};
  const int sourceAlpha[] = {49, 120, 146, 232};
  for (int variant = 0; variant < 4; ++variant) {
    Canvas canvas;
    preset.airbrushRate = variant < 2 ? 5 : 20;
    Stroke stroke(canvas, preset);
    require(stroke.add(sample(64, 64, 10)), stroke.error().c_str());
    for (int tick = 1; tick < counts[variant]; ++tick) {
      Sample event    = sample(100, 100, 10 + tick / preset.airbrushRate);
      event.timerTick = true;
      require(stroke.add(event), stroke.error().c_str());
    }
    finishStroke(stroke);
    int occupied = 0;
    for (int y = 0; y < canvas.h; ++y)
      for (int x = 0; x < canvas.w; ++x) {
        const auto pixel = canvas.read(x, y);
        const bool mask =
            (x + .5 - 64) * (x + .5 - 64) + (y + .5 - 64) * (y + .5 - 64) < 64;
        require((pixel.a > 0) == mask,
                "Airbrush tap lattice differs from source capture");
        if (mask) {
          ++occupied;
          require(
              std::abs(std::floor(pixel.a * 255 + .5) - sourceAlpha[variant]) <=
                  1,
              "Airbrush accumulation differs from stationary source capture");
        }
      }
    require(occupied == 208 && stroke.dabCount() == size_t(counts[variant]),
            "Explicit airbrush tick count or support changed");
  }
  preset.airbrushRate = 5;
  for (bool ignore : {false, true}) {
    Canvas canvas;
    preset.airbrushIgnoreSpacing = ignore;
    Stroke stroke(canvas, preset);
    require(stroke.add(sample(24, 64, 0)) && stroke.add(sample(32, 64, .1)),
            stroke.error().c_str());
    Sample tick    = sample(96, 100, .2);
    tick.timerTick = true;
    require(stroke.add(tick) && stroke.add(sample(40, 64, .3)),
            stroke.error().c_str());
    require(stroke.dabCount() == 2 && canvas.read(32, 64).a > 0 &&
                canvas.read(96, 100).a == 0,
            "Timer did not use latest pointer/reset distance");
    require(stroke.add(sample(48, 64, .4)), stroke.error().c_str());
    require(stroke.dabCount() == size_t(ignore ? 2 : 3),
            "Ignore-spacing did not suppress pointer distance deposits");
    require(stroke.add(sample(48, 64, 10)), stroke.error().c_str());
    require(stroke.dabCount() == size_t(ignore ? 2 : 3),
            "Elapsed time synthesized unobserved airbrush ticks");
    const Canvas before = canvas;
    stroke.cancel();
    require(same(canvas, before) && !stroke.add(tick),
            "Cancelled airbrush resumed or deposited a final dab");
  }
  preset.airbrushIgnoreSpacing = false;
  Canvas overshoot;
  Stroke stroke(overshoot, preset);
  require(stroke.add(sample(24, 64, 0)) && stroke.add(sample(60, 64, .1)),
          stroke.error().c_str());
  require(stroke.dabCount() == 2 && overshoot.read(40, 64).a == 0 &&
              overshoot.read(60, 64).a > 0,
          "Source airbrush interpolated pointer distance deposits");
  Canvas refused;
  refused.refuse = true;
  Stroke failing(refused, preset);
  require(!failing.add(sample(64, 64, 0)) && failing.dabCount() == 0,
          "Airbrush bypassed transaction preparation failure");
  Canvas limited;
  auto budget = std::make_shared<MemoryBudget>(64);
  Stroke exhausted(limited, preset, 64, budget);
  require(!exhausted.add(sample(64, 64, 0)) && exhausted.dabCount() == 0 &&
              budget->used() == 0,
          "Airbrush snapshot allocation bypassed shared memory limit");
  Canvas empty;
  Stroke noContact(empty, preset);
  Sample tick    = sample(64, 64, 0);
  tick.timerTick = true;
  require(!noContact.add(tick) && noContact.dabCount() == 0,
          "Airbrush accepted a timer without pointer contact");
  Preset native  = preset;
  native.profile = "groundzero-native-1";
  Canvas nativePointer, nativeFlagged;
  Stroke a(nativePointer, native), b(nativeFlagged, native);
  for (double time : {0., .25, 1.}) {
    Sample event = sample(64, 64, time);
    require(a.add(event), a.error().c_str());
    event.timerTick = true;
    require(b.add(event), b.error().c_str());
  }
  require(same(nativePointer, nativeFlagged) && a.dabCount() == b.dabCount(),
          "Source tick flag changed native scheduling");
}
bool mockLinearPickupLab(const Pixel &mean, Detail::QuantizedPickupLab &lab) {
  lab.encoded = {{std::uint16_t(std::floor(mean.r * 65535 + .5)), 32768, 32768}};
  lab.physical = {{lab.encoded[0] * 100.0 / 65535, 0, 0}};
  return true;
}
bool mockBoundaryPickupLab(const Pixel &mean, Detail::QuantizedPickupLab &lab) {
  // Exact physical delta3 tests the controller boundary; this is deliberately
  // a mock calibration and does not implement a claimed source color transform.
  lab.encoded = {{std::uint16_t(mean.r > 0 ? 1966 : 0), 32768, 32768}};
  lab.physical = {{mean.r > 0 ? 3.0 : 0.0, 0, 0}};
  return true;
}
bool mockUnavailablePickupLab(const Pixel &, Detail::QuantizedPickupLab &) {
  return false;
}
void testAdaptiveSourcePickupComponent() {
  using namespace Detail;
  const PickupLabCalibration mock{mockLinearPickupLab, "mock component test"};
  require(sourcePickupLabCalibration().available(),
          "Measured source Lab conversion is unavailable");
  AdaptivePickupSampling blocked(80, 256, PickupLabCalibration{},
                                 PickupCountBasis::Attempted);
  Pixel result;
  require(blocked.decision() == PickupDecision::CalibrationUnavailable &&
              blocked.add({0, 0, 0, 1}, true) ==
                  PickupDecision::CalibrationUnavailable &&
              blocked.attempts() == 0 && !blocked.result(result),
          "Uncalibrated source sampling produced a usable approximation");
  AdaptivePickupSampling uniform(80, 256, mock, PickupCountBasis::Attempted);
  for (unsigned i = 0; i < 80; ++i) {
    const auto decision = uniform.add({.25, .25, .25, 1}, true);
    require(decision == (i == 79 ? PickupDecision::Converged
                                : PickupDecision::Collecting),
            "Uniform pickup failed its exact minimum/batch boundary");
  }
  require(uniform.result(result) && result.r == .25 && result.a == 1 &&
              uniform.attempts() == 80 && uniform.accepted() == 80,
          "Uniform adaptive pickup changed cumulative color or count");
  AdaptivePickupSampling cumulative(80, 256, mock, PickupCountBasis::Attempted);
  while (cumulative.decision() == PickupDecision::Collecting) {
    const double value = cumulative.attempts() < 25 ? 1 : 0;
    cumulative.add({value, value, value, 1}, true);
  }
  require(cumulative.decision() == PickupDecision::Converged &&
              cumulative.attempts() == 128 && cumulative.result(result) &&
              result.r == 25.0 / 128 && result.a == 1,
          "Adaptive pickup used a batch-only mean or skipped a16-sample checkpoint");
  const PickupLabCalibration boundary{mockBoundaryPickupLab, "mock exact boundary"};
  AdaptivePickupSampling strict(80, 128, boundary, PickupCountBasis::Attempted);
  for (unsigned i = 0; i < 80; ++i)
    strict.add(i < 64 ? Pixel{0, 0, 0, 1} : Pixel{1, 1, 1, 1}, true);
  require(strict.decision() == PickupDecision::Collecting,
          "Delta E exactly3 incorrectly satisfied a strict below3 boundary");
  for (unsigned i = 0; i < 16; ++i) strict.add({1, 1, 1, 1}, true);
  require(strict.decision() == PickupDecision::Converged &&
              strict.attempts() == 96,
          "Adaptive pickup omitted a subsequent converged batch");
  AdaptivePickupSampling capped(80, 95, mock, PickupCountBasis::Attempted);
  for (unsigned i = 0; i < 95; ++i) {
    const double value = i < 25 ? 1 : 0;
    capped.add({value, value, value, 1}, true);
  }
  require(capped.decision() == PickupDecision::WorkLimit &&
              capped.attempts() == 95 && !capped.result(result),
          "Unconverged pickup escaped its work limit or returned partial paint");
  AdaptivePickupSampling accepted(80, 200, mock, PickupCountBasis::Accepted);
  while (accepted.decision() == PickupDecision::Collecting)
    accepted.add({.25, .25, .25, 1}, accepted.attempts() % 2 == 0);
  require(accepted.decision() == PickupDecision::Converged &&
              accepted.attempts() == 159 && accepted.accepted() == 80,
          "Attempted and accepted pickup counts were conflated");
  const PickupLabCalibration unavailable{mockUnavailablePickupLab, "mock failure"};
  AdaptivePickupSampling failed(80, 128, unavailable, PickupCountBasis::Attempted);
  for (unsigned i = 0; i < 64; ++i) failed.add({0, 0, 0, 1}, true);
  require(failed.decision() == PickupDecision::InvalidCalibration &&
              !failed.result(result),
          "Failed color calibration returned a usable pickup");
  AdaptivePickupSampling invalid(80, 128, mock, PickupCountBasis::Attempted);
  require(invalid.add({1, 0, 0, .5}, true) == PickupDecision::InvalidSample &&
              !invalid.result(result),
          "Non-premultiplied input entered adaptive pickup state");
  AdaptivePickupSampling badConfiguration(79, 128, mock,
                                         PickupCountBasis::Attempted);
  require(badConfiguration.decision() == PickupDecision::InvalidConfiguration,
          "Invalid adaptive pickup configuration was accepted");
}
void testSourcePickupLabPreparation() {
  using namespace Detail;
  const bool ready = sourcePickupLabReadyFlag().load(std::memory_order_acquire);
  const size_t expected = ready ? 0 : SourcePickupLabTransform::initializationWork;
  require(sourcePickupLabReady() == ready &&
              sourcePickupLabPreparationWork() == expected &&
              sourcePickupLabReady() == ready,
          "Source Lab preparation query allocated or changed cache readiness");
  require(SourcePickupLabTransform::initializationWork == 4608384 &&
              SourcePickupLabTransform::initializationWork < 256u * 1024u * 1024u,
          "Source Lab grid construction escaped its fixed operation bound");
  QuantizedPickupLab lab;
  require(sourcePickupLabFromU16({{0, 0, 0}}, lab) && sourcePickupLabReady() &&
              sourcePickupLabPreparationWork() == 0,
          "Constructed source Lab cache still reports uncharged cold work");
}
void testSourcePickupLabCalibration() {
  using namespace Detail;
  // Original public ManagedColor observations, not generated by this helper.
  // These include gray, both chromatic polarities and near-lightness colors.
  struct Observation {
    SourcePickupLabTransform::Code rgb, lab;
  };
  const Observation observations[] = {
      {{{16549, 16549, 16549}}, {{17868, 32896, 32896}}},
      {{{14246, 14246, 14246}}, {{15244, 32896, 32896}}},
      {{{10051, 10051, 10051}}, {{10280, 32896, 32896}}},
      {{{16383, 16383, 16383}}, {{17682, 32896, 32896}}},
      {{{54389, 11145, 0}}, {{31088, 49251, 48629}}},
      {{{10051, 10051, 55483}}, {{19401, 45612, 9839}}},
      {{{4818, 4818, 60716}}, {{18782, 48567, 6435}}},
      {{{5319, 60215, 0}}, {{53389, 13989, 52450}}},
      {{{17808, 17808, 17808}}, {{19274, 32896, 32896}}},
      {{{36704, 15807, 26559}}, {{25181, 42818, 31078}}},
      {{{42350, 42350, 23184}}, {{43526, 30822, 42762}}},
      {{{16886, 27698, 42413}}, {{29164, 32591, 23681}}},
  };
  const auto &transform = sourcePickupLabTransform();
  require(&transform == &sourcePickupLabTransform() &&
              transform.chargedBytes() == SourcePickupLabTransform::tableBytes &&
              transform.chargedBytes() == 215622,
          "Source Lab cache was duplicated or escaped its fixed memory charge");
  for (const auto &observation : observations) {
    QuantizedPickupLab lab;
    require(sourcePickupLabFromU16(observation.rgb, lab) &&
                lab.encoded == observation.lab,
            "Calibrated source ICC conversion differs from an original U16 observation");
    const Pixel input{(observation.rgb[0] + .25) / 65535,
                      (observation.rgb[1] + .25) / 65535,
                      (observation.rgb[2] + .25) / 65535, 1};
    QuantizedPickupLab normalized;
    require(convertSourcePickupLab(input, normalized) &&
                normalized.encoded == observation.lab,
            "Source U16 component writes stopped truncating cumulative RGB means");
    const Pixel associated{input.r / 2, input.g / 2, input.b / 2, .5};
    require(convertSourcePickupLab(associated, normalized) &&
                normalized.encoded == observation.lab,
            "Premultiplied alpha changed straight RGB profile conversion");
  }
  QuantizedPickupLab gray, chromatic;
  sourcePickupLabFromU16({{16549, 16549, 16549}}, gray);
  require(std::abs(gray.physical[0] - 27.2648215293884) < .00001 &&
              gray.physical[1] == 0 && gray.physical[2] == 0,
          "Source physical Lab decode disagrees with public neutral XML");
  sourcePickupLabFromU16({{54389, 11145, 0}}, chromatic);
  require(chromatic.physical[1] ==
              (chromatic.encoded[1] / 257.0 - 128) * (127.0 / 128),
          "Source positive Lab XML channel scaling changed");
  require(!convertSourcePickupLab({1, 0, 0, .5}, gray),
          "Invalid premultiplied input entered source color conversion");
}
void testSourcePickupObservedConvergence() {
  using namespace Detail;
  // Completed source first-25/75 point masks and complements: four endpoint
  // pairs, 16 independently captured RGB outputs. Stopping counts remain
  // predictions of this model; a source internal counter was not observed.
  // Minimum candidate 99 is explicit, not a fitted general size-count rule.
  using RGB = std::array<unsigned, 3>;
  struct Pair {
    Pixel low, high;
    unsigned normal25, complement25, normal75, complement75;
    std::array<RGB, 4> observed;
  };
  const Pair pairs[] = {
      {{0, 0, 0, 1}, {1, 1, 1, 1}, 147, 131, 227, 211,
       {{{{43, 43, 43}}, {{206, 206, 206}}, {{84, 84, 84}}, {{164, 164, 164}}}}},
      {{1, 0, 0, 1}, {0, 1, 0, 1}, 163, 147, 307, 275,
       {{{{216, 39, 0}}, {{43, 212, 0}}, {{193, 62, 0}}, {{70, 185, 0}}}}},
      {{0, 0, 1, 1}, {1, 1, 0, 1}, 195, 163, 355, 307,
       {{{{33, 33, 222}}, {{216, 216, 39}}, {{54, 54, 201}}, {{193, 193, 62}}}}},
      {{192.0 / 255, 32.0 / 255, 64.0 / 255, 1},
       {32.0 / 255, 128.0 / 255, 192.0 / 255, 1}, 147, 115, 243, 211,
       {{{{165, 48, 86}}, {{67, 107, 164}}, {{143, 62, 104}}, {{89, 94, 147}}}}}};
  for (const auto &pair : pairs)
    for (unsigned points : {25u, 75u})
      for (bool complement : {false, true}) {
        AdaptivePickupSampling sampling(99, 1024, sourcePickupLabCalibration(),
                                        PickupCountBasis::Attempted,
                                        PickupMeanStorage::QuantizedStraightRgba8);
        while (sampling.decision() == PickupDecision::Collecting) {
          const bool marked = (sampling.attempts() < points) != complement;
          sampling.add(marked ? pair.high : pair.low, true);
        }
        const unsigned expected = points == 25
            ? (complement ? pair.complement25 : pair.normal25)
            : (complement ? pair.complement75 : pair.normal75);
        Pixel result;
        require(sampling.attempts() == expected && sampling.result(result),
                "Full-color U16 Lab convergence disagrees with a captured source mask");
        const RGB &observed = pair.observed[(points == 75 ? 2 : 0) + complement];
        auto byte = [](double value) { return std::floor(value * 255 + .5); };
        require(byte(result.r) == observed[0] && byte(result.g) == observed[1] &&
                    byte(result.b) == observed[2] && std::abs(result.a - 1) < 1e-12,
                "Adaptive pickup did not return the captured cumulative RGB output");
      }
}
void testSourcePickupExactQuantizedMean() {
  using namespace Detail;
  const PickupLabCalibration uniformCalibration{
      convertSourcePickupLab, "authored exact non-neutral RGBA8 mean",
      [](const std::array<std::uint16_t, 3> &rgb, QuantizedPickupLab &lab) {
        // These authored bytes times 257 must survive every checkpoint,
        // independent of repetition count or uniform quantized alpha.
        if (rgb != std::array<std::uint16_t, 3>{{4369, 23901, 51657}})
          return false;
        return sourcePickupLabFromU16(rgb, lab);
      }};
  for (unsigned minimum : {80u, 147u, 4096u})
    for (unsigned alpha : {255u, 127u, 1u}) {
      const double a = alpha / 255.0;
      const Pixel pixel{(17.0 / 255) * a, (93.0 / 255) * a,
                        (201.0 / 255) * a, a};
      AdaptivePickupSampling sampling(minimum, 8192, uniformCalibration,
                                      PickupCountBasis::Attempted,
                                      PickupMeanStorage::QuantizedStraightRgba8);
      while (sampling.decision() == PickupDecision::Collecting)
        sampling.add(pixel, true);
      Pixel mean;
      require(sampling.attempts() == minimum && sampling.result(mean) &&
                  mean.r == (17.0 * alpha) / (255 * 255) &&
                  mean.g == (93.0 * alpha) / (255 * 255) &&
                  mean.b == (201.0 * alpha) / (255 * 255) && mean.a == a,
              "Uniform quantized non-neutral pickup drifted with repeated accumulation");
    }
  const PickupLabCalibration weightedCalibration{
      convertSourcePickupLab, "authored exact premultiplied mixed-alpha mean",
      [](const std::array<std::uint16_t, 3> &rgb, QuantizedPickupLab &lab) {
        if (rgb != std::array<std::uint16_t, 3>{{49151, 0, 16383}})
          return false;
        return sourcePickupLabFromU16(rgb, lab);
      }};
  for (unsigned minimum : {80u, 144u, 4096u}) {
    AdaptivePickupSampling sampling(minimum, 8192, weightedCalibration,
                                    PickupCountBasis::Attempted,
                                    PickupMeanStorage::QuantizedStraightRgba8);
    while (sampling.decision() == PickupDecision::Collecting)
      sampling.add(sampling.attempts() % 2 == 0
                       ? Pixel{1, 0, 0, 1} : Pixel{0, 0, 85.0 / 255, 85.0 / 255},
                   true);
    Pixel mean;
    require(sampling.attempts() == minimum && sampling.result(mean) &&
                mean.r == .5 && mean.g == 0 && mean.b == 1.0 / 6 &&
                mean.a == 2.0 / 3,
            "Exact source accumulation changed premultiplied mixed-alpha weighting");
  }
}
void testSourcePickupPremultipliedByteStorage() {
  using namespace Detail;
  const PickupLabCalibration calibration{
      convertSourcePickupLab, "authored associated-byte storage contract",
      [](const SourcePickupLabTransform::Code &rgb, QuantizedPickupLab &lab) {
        // Average associated byte channels are 26.5, 46.5, 69 and alpha 127.
        // Unassociation happens after exact accumulation, with truncation.
        if (rgb != SourcePickupLabTransform::Code{{13674, 23995, 35605}})
          return false;
        return sourcePickupLabFromU16(rgb, lab);
      }};
  for (unsigned minimum : {80u, 144u, 4096u}) {
    AdaptivePickupSampling sampling(minimum, 8192, calibration,
        PickupCountBasis::Attempted, PickupMeanStorage::QuantizedPremultipliedRgba8);
    while (sampling.decision() == PickupDecision::Collecting)
      sampling.add(sampling.attempts() % 2 == 0
          ? Pixel{17.0 / 255, 31.0 / 255, 91.0 / 255, 128.0 / 255}
          : Pixel{36.0 / 255, 62.0 / 255, 47.0 / 255, 126.0 / 255}, true);
    Pixel mean;
    require(sampling.attempts() == minimum && sampling.result(mean) &&
                mean.r == 53.0 / 510 && mean.g == 93.0 / 510 &&
                mean.b == 138.0 / 510 && mean.a == 127.0 / 255,
            "Associated-byte pickup re-quantized unassociated color or changed its mean");
  }
}
void testSurfacePickupStorageContract() {
  Canvas generic;
  require(generic.storage() == PixelStorage::FloatingPremultiplied,
          "Generic Surface inferred byte storage for arbitrary floating pixels");
  struct ByteBacking : Canvas {
    PixelStorage storage() const override {
      return PixelStorage::QuantizedPremultipliedRgba8;
    }
  };
  auto backing = std::make_shared<ByteBacking>();
  auto memory = std::make_shared<MemoryBudget>(1024 * 1024);
  BufferedSurface buffered(backing, memory);
  require(buffered.storage() == PixelStorage::FloatingPremultiplied &&
              buffered.prepare(0, 0, 32, 32),
          "Float tile cache inherited the backing raster's byte declaration");
  const Pixel priorTrack{.25e-12, .125e-12, .375e-12, 1e-12};
  buffered.write(10, 10, priorTrack);
  const Pixel retained = buffered.read(10, 10);
  require(retained.r == priorTrack.r && retained.g == priorTrack.g &&
              retained.b == priorTrack.b && retained.a == priorTrack.a &&
              buffered.storage() == PixelStorage::FloatingPremultiplied,
          "Shared float tiles discarded a prior track's sub-byte paint");
  using namespace Detail;
  AdaptivePickupSampling first(80, 128, sourcePickupLabCalibration(),
                               PickupCountBasis::Attempted);
  while (first.decision() == PickupDecision::Collecting)
    first.add(buffered.read(10, 10), true);
  Pixel mean;
  require(first.result(mean) && mean.r == priorTrack.r && mean.g == priorTrack.g &&
              mean.b == priorTrack.b && mean.a == priorTrack.a,
          "First floating-surface pickup was quantized from dab count");
}
void testSourcePickupRetainedFloatingMean() {
  using namespace Detail;
  // First retained Dulling feedback stores its mathematical mean directly.
  // These are authored floating pixels, not claimed RGBA8 source captures.
  const Pixel retained[] = {
      {.25, .125, .375, 1},
      {25.0 / 99, 39.0 / 163, 54.0 / 355, 1},
      {.25e-12, .125e-12, .375e-12, 1e-12}};
  for (const Pixel &pixel : retained)
    for (unsigned minimum : {80u, 147u, 4096u}) {
      AdaptivePickupSampling sampling(minimum, 8192, sourcePickupLabCalibration(),
                                      PickupCountBasis::Attempted,
                                      PickupMeanStorage::FloatingPoint);
      while (sampling.decision() == PickupDecision::Collecting)
        sampling.add(pixel, true);
      Pixel mean;
      require(sampling.attempts() == minimum && sampling.result(mean) &&
                  mean.r == pixel.r && mean.g == pixel.g &&
                  mean.b == pixel.b && mean.a == pixel.a,
              "Retained floating pickup was rounded or lost its uniform tiny alpha");
    }
  const PickupLabCalibration mock{mockLinearPickupLab, "compensated floating mean"};
  AdaptivePickupSampling varied(128, 256, mock, PickupCountBasis::Attempted,
                                PickupMeanStorage::FloatingPoint);
  const double tiny = std::ldexp(1.0, -54);
  while (varied.decision() == PickupDecision::Collecting)
    varied.add(varied.attempts() == 0 ? Pixel{1, 0, .5, 1}
                                    : Pixel{tiny, 2 * tiny, 3 * tiny, 1}, true);
  Pixel mean;
  require(varied.attempts() == 128 && varied.result(mean) &&
              mean.r == (1 + 127 * tiny) / 128 &&
              mean.g == (254 * tiny) / 128 &&
              mean.b == (.5 + 381 * tiny) / 128 && mean.a == 1,
          "Nonuniform retained pickup discarded representable small contributions");
}
void testPeriodicTimeMapping() {
  auto render = [](bool periodic, double origin) {
    Canvas canvas;
    Preset preset;
    preset.size.value = 4;
    preset.spacing    = 4;
    Mapping time;
    time.sensor             = Sensor::Time;
    time.periodic           = periodic;
    time.curve              = {{0, .25}, {1, 1}};
    preset.opacity.mappings = {time};
    Stroke stroke(canvas, preset);
    require(stroke.add(sample(10.5, 10.5, origin)), stroke.error().c_str());
    require(stroke.add(sample(90.5, 10.5, origin + 1.25)),
            stroke.error().c_str());
    finishStroke(stroke);
    return canvas;
  };
  const auto cycling = render(true, 0), shifted = render(true, 100);
  const auto clamped = render(false, 0);
  require(same(cycling, shifted), "Time cycle used absolute device epoch");
  for (double origin : {-100., 1000000., 1000000000.})
    require(same(cycling, render(true, origin)) &&
                same(clamped, render(false, origin)),
            "Time mapping changed with a different device epoch");
  require(std::abs(cycling.read(10, 10).a - .25) < 1e-12 &&
              std::abs(cycling.read(74, 10).a - .25) < 1e-12 &&
              std::abs(clamped.read(74, 10).a - 1) < 1e-12,
          "Time cycle did not restart while nonperiodic time saturated");
  Preset invalid;
  Mapping pressure;
  pressure.periodic     = true;
  invalid.size.mappings = {pressure};
  require(!validate(invalid).empty(),
          "Non-Time sensor admitted a periodic flag");
}
void testPeriodicDistanceMapping() {
  auto render = [](bool periodic, bool dense) {
    Canvas canvas;
    Preset preset;
    preset.size.value = 4;
    preset.spacing = 4;
    Mapping distance;
    distance.sensor = Sensor::Distance;
    distance.scale = 64;
    distance.periodic = periodic;
    distance.curve = {{0, .25}, {1, 1}};
    preset.opacity.mappings = {distance};
    Stroke stroke(canvas, preset);
    require(stroke.add(sample(10.5, 10.5, 0)), stroke.error().c_str());
    if (dense) {
      for (int i = 1; i <= 80; ++i)
        require(stroke.add(sample(10.5 + i, 10.5, i / 80.)),
                stroke.error().c_str());
    } else
      require(stroke.add(sample(90.5, 10.5, 1)), stroke.error().c_str());
    finishStroke(stroke);
    return canvas;
  };
  const auto cycling = render(true, false), clamped = render(false, false);
  require(same(cycling, render(true, true)) &&
              same(clamped, render(false, true)),
          "Distance mapping depends on straight-path delivery partition");
  require(cycling.read(10, 10).a == .25 &&
              cycling.read(74, 10).a == .25 && clamped.read(74, 10).a == 1,
          "Distance interval did not wrap or nonperiodic distance did not saturate");
}
void testSourceFixedBitmapCaptures() {
  for (const auto &capture : RasterBrushBitmapReference::captures()) {
    auto image    = std::make_shared<Image>();
    image->width  = capture.width;
    image->height = capture.height;
    image->color  = capture.pattern >= 6;
    for (int y = 0; y < image->height; ++y)
      for (int x = 0; x < image->width; ++x) {
        int alpha = 0;
        if (capture.pattern == 0)
          alpha =
              x == 0 || y == 0
                  ? 0
                  : std::array<int, 4>{{64, 128, 192, 255}}[(x + 2 * y) % 4];
        else if (capture.pattern == 3)
          alpha = (x * 31 + y * 7) % 256;
        else if (capture.pattern == 5)
          alpha = 255;
        else if (x == (capture.pattern == 2 ? 0 : image->width / 2) &&
                 y == (capture.pattern == 2 ? 0 : image->height / 2))
          alpha = 255;
        if (capture.pattern >= 6) {
          std::array<int, 4> rgba;
          if (capture.pattern == 6) {
            const std::array<std::array<int, 4>, 4> colors = {
                {{{255, 0, 0, 255}},
                 {{0, 255, 0, 128}},
                 {{0, 0, 255, 64}},
                 {{255, 255, 255, 0}}}};
            rgba = colors[(x / 2 + y / 4) % 4];
          } else
            rgba = {{(x * 31 + 17) % 256, (y * 29 + 23) % 256,
                     (x * 13 + y * 19 + 41) % 256,
                     std::array<int, 4>{{0, 64, 128, 255}}[(x + y) % 4]}};
          const double a = rgba[3] / 255.0;
          image->pixels.push_back({rgba[0] / 255.0 * a, rgba[1] / 255.0 * a,
                                   rgba[2] / 255.0 * a, a});
        } else
          image->pixels.push_back({0, 0, 0, alpha / 255.0});
      }
    Preset p;
    p.profile = capture.colorAsMask
                    ? "krita-5.3.4.1-fixed-bitmap-mask-candidate-1"
                    : "krita-5.3.4.1-fixed-bitmap-candidate-1";
    if (capture.colorAsMask) p.color = {220 / 255.0, 40 / 255.0, 30 / 255.0, 1};
    p.tip.shape   = Shape::Bitmap;
    p.tip.images  = {image};
    p.size.value  = std::max(image->width, image->height) * capture.scale;
    p.spacing     = 4;
    p.angle.value = capture.angle;
    require(validate(p).empty(), "original source bitmap fixture rejected");
    Canvas canvas;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(32 + capture.phase, 32 + capture.phase, 0)) &&
                stroke.add(sample(33 + capture.phase, 32 + capture.phase, 1)),
            "source bitmap capture stroke failed");
    finishStroke(stroke);
    std::vector<int> expected(canvas.pixels.size(), 0);
    for (const auto &point : capture.pixels)
      expected[size_t(point.y) * canvas.w + point.x] = point.alpha;
    // Full canvas, including zeros: never crop away an extra edge pixel.
    for (size_t i = 0; i < expected.size(); ++i) {
      const int actual = int(std::floor(canvas.pixels[i].a * 255 + .5));
      require((actual != 0) == (expected[i] != 0) &&
                  std::abs(actual - expected[i]) <= 1,
              "source bitmap full-region support/one-byte gate failed");
    }
    if (image->color)
      for (const auto &point : capture.pixels) {
        const auto &actual =
            canvas.pixels[size_t(point.y) * canvas.w + point.x];
        const double alpha = point.alpha / 255.0;
        require(
            std::abs(actual.r - point.r / 255.0 * alpha) <= 2.0 / 255 + 1e-12 &&
                std::abs(actual.g - point.g / 255.0 * alpha) <=
                    2.0 / 255 + 1e-12 &&
                std::abs(actual.b - point.b / 255.0 * alpha) <=
                    2.0 / 255 + 1e-12,
            "source bitmap premultiplied-color two-byte gate failed");
      }
    if (capture.colorAsMask) {
      p.tip.images.push_back(image);
      require(!validate(p).empty(),
              "unqualified PNG mask frame sequence admitted");
      p.tip.images.pop_back();
      image->color = false;
      require(!validate(p).empty(),
              "PNG mask accepted discarded original color");
      image->color = true;
    }
    p.size.value = std::max(image->width, image->height) * .75;
    require(!validate(p).empty(), "unqualified bitmap downscale admitted");
    p.size.value = std::max(image->width, image->height);
    p.flipX      = true;
    require(!validate(p).empty(), "unqualified bitmap mirror admitted");
  }
}
void testSourceMirrorThreshold() {
  // All 21 original enabled/control source pairs establish this exact
  // reflection and inclusive threshold. Scale1/2 composition has separate
  // source holdouts; no unqualified scale3 geometry is admitted here.
  auto image   = std::make_shared<Image>();
  image->width = image->height = 8;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      const bool ink = ((x == 1 || x == 2) && y >= 1 && y <= 6) ||
                       ((y == 5 || y == 6) && x >= 1 && x <= 6);
      image->pixels.push_back({0, 0, 0, ink ? 1.0 : 0.0});
    }
  for (double scale : {1.0, 2.0}) {
    Preset p;
    p.profile    = "krita-5.3.4.1-fixed-bitmap-candidate-1";
    p.tip.shape  = Shape::Bitmap;
    p.tip.images = {image};
    p.size.value = 8 * scale;
    Canvas original;
    Stroke plain(original, p);
    require(plain.add(sample(32, 32, 0)), plain.error().c_str());
    finishStroke(plain);
    for (int axes = 1; axes <= 3; ++axes)
      for (double pressure : {.1, .25, .49, .5, .51, .75, 1.0}) {
        p.sourceMirror.enabled          = true;
        p.sourceMirror.horizontal       = axes & 1;
        p.sourceMirror.vertical         = axes & 2;
        p.sourceMirror.control.mappings = {Mapping{}};
        Canvas mirrored;
        Stroke stroke(mirrored, p);
        require(stroke.add(sample(32, 32, 0, pressure)),
                stroke.error().c_str());
        finishStroke(stroke);
        for (int y = 16; y < 48; ++y)
          for (int x = 16; x < 48; ++x) {
            const int sx = pressure >= .5 && (axes & 1) ? 64 - x : x;
            const int sy = pressure >= .5 && (axes & 2) ? 63 - y : y;
            require(mirrored.read(x, y).a == original.read(sx, sy).a,
                    "source Mirror threshold/reflection differs from original "
                    "pairs");
          }
      }
    p.angle.value = 90;
    require(!validate(p).empty(),
            "unqualified source Mirror rotation admitted");
    p.angle.value                             = 0;
    p.sourceMirror.control.mappings[0].sensor = Sensor::TiltX;
    require(!validate(p).empty(), "unqualified source Mirror sensor admitted");
    p.profile = "groundzero-native-1";
    require(!validate(p).empty(),
            "source Mirror silently retained on native profile");
  }
}
void testSourceRandomGihCacheMechanism() {
  using namespace SourceGihV1;
  const double below = std::nextafter(16., 0.);
  require(phase(below) == 64 && phase(16.) == 0 && quantized(below) == 16. &&
              quantized(16.) == 16. && phase(16.5) == phase(48.5),
          "GIH cache phase endpoint was normalized or integer translation changed its mask key");
  int resourceA = 0, resourceB = 0;
  GeometryKey key;
  key.resources = &resourceA; key.proposalResource = &resourceA;
  key.resourceCount = 3; key.width = key.height = 8;
  key.diameter = 8; key.phaseX = 32;
  Selection selection;
  int calls = 0;
  auto fresh = [&] { ++calls; return std::size_t(2); };
  require(selection.select(key, 0, fresh) == 2 && calls == 1 && selection.valid() &&
              selection.proposedFrame() == 0 && selection.actualFrame() == 2,
          "Source GIH first proposal did not perform an independent actual selection");
  require(selection.select(key, 0, fresh) == 2 && calls == 1,
          "Source GIH unchanged proposal/key consumed a new actual selection");
  require(selection.select(key, 1, fresh) == 2 && calls == 2,
          "Source GIH proposal change reused a stale frame without independent selection");
  for (int field = 0; field < 12; ++field) {
    GeometryKey altered = key;
    switch (field) {
    case 0: altered.phaseX = 31; break;
    case 1: altered.phaseY = 64; break;
    case 2: altered.diameter = 16; break;
    case 3: altered.aspect = .5; break;
    case 4: altered.rotation = 90; break;
    case 5: altered.mirrorX = true; break;
    case 6: altered.mirrorY = true; break;
    case 7: altered.resources = &resourceB; break;
    case 8: altered.proposalResource = &resourceB; break;
    case 9: altered.width = 16; break;
    case 10: altered.resourceCount = 4; break;
    case 11: altered.colorAsMask = true; break;
    }
    Selection changed;
    changed.select(key, 0, fresh);
    const int before = calls;
    changed.select(altered, 0, fresh);
    require(calls == before + 1, "Source GIH cache omitted an effective geometry/resource invalidation");
    changed.select(altered, 0, fresh);
    require(calls == before + 1, "Source GIH exact cache hit advanced actual frame");
  }
  Selection restarted;
  const int before = calls;
  restarted.select(key, 0, fresh);
  require(calls == before + 1, "Source GIH cache state escaped its stroke");
}
void testSourceRandomGihRenderAndNativeInvariance() {
  Preset p;
  p.tip.shape = Shape::Bitmap; p.tip.antialiasEdges = false;
  p.tip.randomSequence = true; p.size.value = 2; p.spacing = 2; p.seed = 1;
  for (int frame = 0; frame < 3; ++frame) {
    auto image = std::make_shared<Image>();
    image->width = image->height = 2; image->color = true;
    image->pixels.assign(4, Pixel{frame == 0 ? 1. : 0., frame == 1 ? 1. : 0., frame == 2 ? 1. : 0., 1});
    p.tip.images.push_back(image);
  }
  auto render = [&](const Preset &preset, double end) {
    Canvas canvas;
    Stroke stroke(canvas, preset);
    require(stroke.add(sample(16, 64, 0)) && stroke.add(sample(end, 64, 1)), stroke.error().c_str());
    finishStroke(stroke);
    return std::make_pair(canvas, stroke.dabCount());
  };
  auto labels = [&](const Canvas &canvas, int count) {
    std::vector<int> result;
    for (int index = 0; index < count; ++index) {
      const Pixel color = canvas.read(16 + index * 4, 64);
      require(color.a > 0, "GIH sequence regression lost an observable stamp");
      result.push_back(color.r > color.g && color.r > color.b ? 0 : color.g > color.b ? 1 : 2);
    }
    return result;
  };
  const std::vector<int> nativeFrames{2, 2, 1, 2, 1, 2, 1, 0, 0, 1, 1, 2, 1, 2, 2, 1};
  const auto native = render(p, 77);
  require(native.second == 16 && labels(native.first, 16) == nativeFrames,
          "Source GIH work changed the established native uniform-per-dab RNG sequence");
  Preset source = p;
  source.profile = "krita-5.3.4.1-random-bitmap-candidate-1";
  require(validate(source).empty(), "Experimental source random GIH profile rejected original fixed frames");
  const auto acquired = render(source, 77), replay = render(source, 77);
  const std::vector<int> sourceFrames{2, 2, 2, 2, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1};
  require(acquired.second == 16 && labels(acquired.first, 16) == sourceFrames && same(acquired.first, replay.first),
          "Source GIH proposal/cache mechanism was not deterministic or was not used by rendering");
  Preset changedSeed = source;
  ++changedSeed.seed;
  require(!same(acquired.first, render(changedSeed, 77).first), "Source GIH ignored its resolved stroke seed");
  Preset hostPolicy = source;
  hostPolicy.varySeedPerStroke = true;
  require(same(acquired.first, render(hostPolicy, 77).first),
          "Source GIH renderer replaced the resolved seed using host stroke policy");
  const auto exact = render(source, 76);
  require(exact.second == 15 && exact.first.read(76, 64).a == 0 && render(p, 76).second == 16,
          "Source GIH exact endpoint continuation was invented or changed native endpoint behavior");
  require(labels(render(p, 77).first, 16) == nativeFrames,
          "Source GIH stroke cache contaminated a subsequent native stroke");
  Preset invalid = source;
  invalid.tip.randomSequence = false;
  require(!validate(invalid).empty(), "Random source profile accepted an incremental selector");
  invalid = source; invalid.profile = "krita-5.3.4.1-fixed-bitmap-candidate-1";
  require(!validate(invalid).empty(), "Random GIH inherited deterministic bitmap source admission");
}
void testSourceBitmapWorkBudget() {
  struct Guard : Surface {
    bool touched = false;
    int width() const override { return 8192; }
    int height() const override { return 8192; }
    bool prepare(int, int, int, int) override {
      touched = true;
      return true;
    }
    Pixel read(int, int) const override { return {}; }
    void write(int, int, const Pixel &) override { touched = true; }
  } canvas;
  auto image    = std::make_shared<Image>();
  image->width  = 2048;
  image->height = 1;
  image->pixels.assign(2048, Pixel{0, 0, 0, 1});
  Preset p;
  p.profile    = "krita-5.3.4.1-fixed-bitmap-candidate-1";
  p.tip.shape  = Shape::Bitmap;
  p.tip.images = {image};
  p.size.value = 4096;
  require(validate(p).empty(), "bitmap budget fixture rejected");
  auto budget = std::make_shared<MemoryBudget>(256u * 1024u * 1024u);
  Stroke stroke(canvas, p, 256u * 1024u * 1024u, budget);
  require(!stroke.add(sample(4096, 4096, 0)) &&
              stroke.error() == "Stroke input pixel work limit exceeded" &&
              !canvas.touched,
          "two-stage bitmap work not rejected before raster access");
  stroke.cancel();
  require(budget->used() == 0 && !stroke.add(sample(1, 1, 1)) &&
              !stroke.finish() && !canvas.touched,
          "bitmap work failure retained storage or resumed");
}
void testSourceMaskedEventBudget() {
  struct GuardCanvas : Canvas {
    size_t prepares = 0, writes = 0;
    bool prepare(int x0, int y0, int x1, int y1) override {
      ++prepares;
      return Canvas::prepare(x0, y0, x1, y1);
    }
    void write(int x, int y, const Pixel &pixel) override {
      ++writes;
      Canvas::write(x, y, pixel);
    }
  } canvas;
  const Canvas before = canvas;
  Preset p;
  p.profile                    = "krita-5.3.4.1-hard-pixel-candidate-1";
  p.size.value                 = 1;
  p.spacing                    = .02;
  p.wash                       = true;
  p.sourceMasked.enabled       = true;
  p.sourceMasked.useMasterSize = false;
  p.sourceMasked.diameter      = 1;
  p.sourceMasked.spacing       = .01;
  require(validate(p).empty(), "mask event-budget fixture invalid");
  auto budget = std::make_shared<MemoryBudget>(256u * 1024u * 1024u);
  Stroke stroke(canvas, p, 256u * 1024u * 1024u, budget);
  // Keep raster work outside the canvas: combined primary/secondary scheduler
  // work must be bounded even when no destination pixels need allocation.
  require(stroke.add(sample(-2000, -1000, 0)),
          "mask budget initial input failed");
  require(!stroke.add(sample(-1000, -1000, 1)) &&
              stroke.error() == "Stroke input event work limit exceeded",
          "source mask bypassed combined per-input event budget");
  require(canvas.prepares == 0 && canvas.writes == 0 && same(canvas, before),
          "off-canvas mask work failure changed the destination");
  stroke.cancel();
  require(budget->used() == 0, "mask work cancellation retained allocation");
  require(!stroke.add(sample(32, 32, 2)) && !stroke.finish() &&
              canvas.prepares == 0 && canvas.writes == 0 &&
              same(canvas, before),
          "failed masked stroke resumed or committed after cancellation");
}
void testSourceMaskedWash() {
  struct MaskCanvas : Canvas {
    MaskCanvas() {
      w = 384;
      h = 64;
      pixels.assign(size_t(w) * h, {});
    }
    double coverage(int, int) const override { return 1; }
  };
  Preset p;
  p.profile              = "krita-5.3.4.1-hard-pixel-candidate-1";
  p.size.value           = 24;
  p.spacing              = .25;
  p.wash                 = true;
  p.sourceMasked.enabled = true;
  p.color                = {220 / 255., 40 / 255., 30 / 255., 1};
  require(validate(p).empty(), "source mask contract rejected");
  // Original captured full-region histograms, not a mirrored renderer oracle.
  const std::array<std::array<int, 4>, 4> counts = {{{{0, 0, 0, 1590}},
                                                     {{1590, 0, 0, 0}},
                                                     {{72, 1272, 246, 0}},
                                                     {{72, 1272, 246, 0}}}};
  for (int variant = 0; variant < 4; ++variant) {
    p.sourceMasked.opacity = variant == 1 || variant == 3 ? .5 : 1;
    p.sourceMasked.flow    = variant >= 2 ? .5 : 1;
    MaskCanvas canvas;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(32, 32, 0)) && stroke.add(sample(288, 32, 1)),
            "source mask captured stroke failed");
    finishStroke(stroke);
    std::array<int, 256> histogram{};
    for (const Pixel &pixel : canvas.pixels)
      ++histogram[int(std::floor(pixel.a * 255 + .5))];
    const std::array<int, 4> levels =
        variant == 3 ? std::array<int, 4>{{64, 96, 112, 255}}
                     : std::array<int, 4>{{128, 192, 224, 255}};
    for (size_t i = 0; i < levels.size(); ++i)
      require(histogram[levels[i]] == counts[variant][i],
              "source mask captured histogram differs");
    require(histogram[0] == canvas.w * canvas.h - 1590,
            "source mask extent or endpoint differs");
  }
  p.sourceMasked.opacity = p.sourceMasked.flow = 1;
  p.spacing                                    = 2;
  for (MaskMode mode : {MaskMode::Multiply, MaskMode::Subtract}) {
    p.sourceMasked.mode = mode;
    MaskCanvas canvas;
    const Pixel base{.06, .12, .18, .3};
    std::fill(canvas.pixels.begin(), canvas.pixels.end(), base);
    Stroke stroke(canvas, p);
    require(stroke.add(sample(48, 32, 0)), "source mask initial stamp failed");
    const auto before = canvas.read(58, 32);
    require(stroke.add(sample(54, 32, .1)),
            "source mask deferred endpoint failed");
    require(canvas.read(58, 32).a == before.a,
            "final mask endpoint was deposited early");
    require(stroke.add(sample(55, 32, .2)), "source mask dirty update failed");
    const auto after = canvas.read(58, 32);
    require(stroke.dabCount() == 1,
            "secondary-only event unexpectedly deposited primary paint");
    require(
        mode == MaskMode::Multiply ? (before.a == base.a && after.a == 1)
                                   : (before.a == 1 && after.a == base.a),
        "secondary-only dirty update did not recompose retained primary/base");
    finishStroke(stroke);
  }
  p.spacing           = .25;
  p.sourceMasked.mode = MaskMode::Multiply;
  MaskCanvas one, split;
  Stroke a(one, p), b(split, p);
  require(a.add(sample(32, 32, 0)) && a.add(sample(288, 32, 1)),
          "mask whole trace failed");
  for (int i = 0; i <= 256; ++i)
    require(b.add(sample(32 + i, 32, i / 256.)), "mask split trace failed");
  finishStroke(a);
  finishStroke(b);
  require(same(one, split), "mask endpoint deferral depends on event batching");
  auto budget = std::make_shared<MemoryBudget>(24000);
  MaskCanvas limited;
  Stroke fail(limited, p, 24000, budget);
  const bool accepted =
      fail.add(sample(32, 32, 0)) && fail.add(sample(288, 32, 1));
  require(!accepted && !fail.error().empty(),
          "source mask memory budget ignored");
  fail.cancel();
  require(budget->used() == 0, "source mask cancellation retained memory");
  const auto canceled = limited;
  require(
      !fail.add(sample(10, 10, 2)) && !fail.finish() && same(limited, canceled),
      "failed source mask resumed after cancellation");
  p.sourceMasked.flow = 2;
  require(!validate(p).empty(), "invalid mask flow admitted");
  p.sourceMasked.flow = 1;
  p.profile           = "groundzero-native-1";
  require(!validate(p).empty(),
          "source field silently admitted as native mask");
}
void testNativeEraseCoverage() {
  const Pixel base{.12, .3, .48, .6};
  for (Wet wet : {Wet::Dulling, Wet::Smear}) {
    Preset invalid;
    invalid.wet   = wet;
    invalid.blend = Blend::Erase;
    require(!validate(invalid).empty(),
            "direct wet/erase preset bypassed the single-mode contract");
  }
  for (int tipKind : {0, 1, 2})
    for (bool wash : {false, true})
      for (bool locked : {false, true}) {
        Preset p;
        p.blend               = Blend::Erase;
        p.size.value          = 8;
        p.tip.shape           = Shape::Square;
        p.flow.value          = .4;
        p.opacity.value       = .3;
        p.airbrushRate        = 10;
        p.wash                = wash;
        p.lockAlpha           = locked;
        const double coverage = tipKind ? .5 : 1;
        if (tipKind) {
          auto tip   = std::make_shared<Image>();
          tip->width = tip->height = 4;
          tip->color               = tipKind == 2;
          tip->pixels.assign(16, Pixel{.1, .2, .3, .5});
          p.tip.shape  = Shape::Bitmap;
          p.tip.images = {tip};
        }
        Canvas reference;
        bool first = true;
        for (const Pixel color :
             {Pixel{}, Pixel{.1, .2, .05, .25}, Pixel{.8, .1, .4, 1}})
          for (double backgroundMix : {0., .5, 1.}) {
            p.color               = color;
            p.background          = {.15, .05, .1, .2};
            p.backgroundMix.value = backgroundMix;
            Canvas canvas;
            std::fill(canvas.pixels.begin(), canvas.pixels.end(), base);
            Stroke stroke(canvas, p);
            const bool accepted =
                stroke.add(sample(64, 64, 0)) && stroke.add(sample(64, 64, .2));
            require(accepted, stroke.error().c_str());
            const auto count = stroke.dabCount();
            require(count > 1,
                    "eraser accumulation fixture lacked repeat dabs");
            finishStroke(stroke);
            const double factor =
                locked ? 1
                : wash
                    ? 1 - p.opacity.value *
                              (1 - std::pow(1 - coverage * p.flow.value, count))
                    : std::pow(1 - coverage * p.flow.value * p.opacity.value,
                               count);
            const Pixel center = canvas.read(64, 64);
            require(std::abs(center.a - base.a * factor) < 1e-12 &&
                        std::abs(center.r - base.r * factor) < 1e-12 &&
                        std::abs(center.g - base.g * factor) < 1e-12 &&
                        std::abs(center.b - base.b * factor) < 1e-12,
                    "eraser changed coverage, wash, opacity or alpha lock");
            require(canvas.read(110, 64).a == base.a,
                    "eraser changed pixels outside the dab");
            if (first) {
              reference.pixels = canvas.pixels;
              first            = false;
            } else
              require(same(reference, canvas),
                      "eraser strength depends on foreground/background color");
          }
      }
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
    finishStroke(stroke);
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
void testSnapshotAndPrepareBudgets() {
  struct GuardSurface : Surface {
    int side;
    int prepares = 0, reads = 0, writes = 0;
    bool refuse = false;
    explicit GuardSurface(int size) : side(size) {}
    int width() const override { return side; }
    int height() const override { return side; }
    bool prepare(int, int, int, int) override {
      ++prepares;
      return !refuse;
    }
    Pixel read(int, int) const override {
      ++const_cast<GuardSurface *>(this)->reads;
      return {};
    }
    void write(int, int, const Pixel &) override { ++writes; }
  };
  for (int size : {2048, std::numeric_limits<int>::max()}) {
    auto backing = std::make_shared<GuardSurface>(size);
    auto shared  = std::make_shared<MemoryBudget>(65536);
    BufferedSurface buffer(backing, shared);
    require(!buffer.prepare(0, 0, size, size) && backing->prepares == 0 &&
                backing->reads == 0 && backing->writes == 0 &&
                shared->used() == 0,
            "float-cache budget was checked after host undo allocation");
  }
  {
    auto backing = std::make_shared<GuardSurface>(32);
    auto shared  = std::make_shared<MemoryBudget>(65536);
    BufferedSurface buffer(backing, shared);
    backing->refuse = true;
    require(!buffer.prepare(0, 0, 32, 32) && shared->used() == 0,
            "host refusal leaked preflight reservation");
    backing->refuse = false;
    require(buffer.prepare(0, 0, 32, 32), "valid buffered region rejected");
    const size_t retained = shared->used();
    require(retained >= 1024 * sizeof(Pixel) && buffer.prepare(0, 0, 32, 32) &&
                shared->used() == retained,
            "existing float tiles charged twice");
  }
  {
    auto backing = std::make_shared<GuardSurface>(2048);
    auto shared  = std::make_shared<MemoryBudget>(2 * 1024 * 1024);
    BufferedSurface buffer(backing, shared);
    Preset wet;
    wet.wet          = Wet::Smear;
    wet.size.value   = 4096;
    wet.smudgeRadius = 3;
    auto texture     = std::make_shared<Image>();
    texture->width = texture->height = 1;
    texture->pixels                  = {{0, 0, 0, 1}};
    wet.texture.image                = texture;
    Stroke stroke(buffer, wet, 2 * 1024 * 1024, shared);
    require(
        !stroke.add(sample(1024, 1024, 0)) &&
            stroke.error() == "Raster transaction refused the stroke region" &&
            backing->prepares == 0 && backing->reads == 0 &&
            backing->writes == 0,
        "combined wet texture allocated host undo before shared-budget denial");
    require(!stroke.finish(), "failed wet texture unexpectedly finalized");
    require(shared->used() == 0, "failed wet texture retained memory charges");
  }
  Preset p;
  Mapping mapping;
  mapping.curve.clear();
  for (int i = 0; i < 1024; ++i)
    mapping.curve.push_back({double(i) / 1023, double(i) / 1023});
  p.size.mappings.assign(16, mapping);
  require(validate(p).empty(), "legal large linear-curve preset rejected");
  Canvas canvas;
  auto small = std::make_shared<MemoryBudget>(65536);
  Stroke refused(canvas, p, 65536, small);
  require(refused.error() == "Preset snapshot memory limit exceeded" &&
              !refused.add(sample(32, 32, 0)) && small->used() == 0,
          "linear settings snapshot bypassed memory budget");
  auto measured = std::make_shared<MemoryBudget>(1024 * 1024);
  Stroke measurement(canvas, p, 1024 * 1024, measured);
  const size_t snapshot = measured->used();
  require(measurement.error().empty() && snapshot >= 16 * 1024 * sizeof(Knot),
          "copied linear knots were not charged");
  finishStroke(measurement);
  require(measured->used() == 0, "finished settings snapshot retained charge");
  {
    Preset cubic = p;
    for (auto &curve : cubic.size.mappings)
      curve.interpolation = Interpolation::NaturalCubic;
    auto exactSnapshot = std::make_shared<MemoryBudget>(snapshot);
    Stroke deniedCache(canvas, cubic, snapshot, exactSnapshot);
    require(deniedCache.error() == "Curve memory limit exceeded" &&
                exactSnapshot->used() == 0,
            "constructor cache failure retained its copied settings or charge");
  }
  auto shared = std::make_shared<MemoryBudget>(2 * snapshot - 1);
  Stroke first(canvas, p, 1024 * 1024, shared);
  Stroke second(canvas, p, 1024 * 1024, shared);
  require(first.error().empty() && !second.error().empty() &&
              shared->used() == snapshot,
          "replicated tracks bypassed aggregate settings budget");
  finishStroke(first);
  require(shared->used() == 0, "replicated settings snapshot leaked charge");
  // Finished strokes must drop their resource references as well as reporting
  // released memory; the caller may replace the installed preset immediately.
  std::weak_ptr<const Image> lifetime;
  std::unique_ptr<Stroke> retained;
  {
    Preset bitmap;
    auto image   = std::make_shared<Image>();
    image->width = image->height = 1;
    image->pixels                = {{0, 0, 0, 1}};
    lifetime                     = image;
    bitmap.tip.shape             = Shape::Bitmap;
    bitmap.tip.images.push_back(image);
    bitmap.texture.image = image;
    retained.reset(new Stroke(canvas, bitmap));
  }
  require(!lifetime.expired(), "stroke did not retain its input resource");
  finishStroke(*retained);
  require(lifetime.expired(),
          "finished stroke retained snapshot resource storage");
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
  finishStroke(stroke);
  double area = 0;
  for (const auto &pixel : c.pixels) area += pixel.a;
  require(area == 1 && c.read(31, 31).a == .25 && c.read(32, 32).a == .25,
          "one-pixel square area was not conserved across four pixels");
  p.size.value = .25;
  Canvas tiny;
  Stroke dot(tiny, p);
  require(dot.add(sample(32, 32, 0)), dot.error().c_str());
  finishStroke(dot);
  area = 0;
  for (const auto &pixel : tiny.pixels) area += pixel.a;
  require(area == .0625, "subpixel square disappeared or gained coverage");
  p.size.value   = 4;
  p.aspect.value = .5;
  Canvas horizontal, vertical;
  Stroke a(horizontal, p);
  require(a.add(sample(32, 32, 0)), a.error().c_str());
  finishStroke(a);
  p.angle.value = 90;
  Stroke b(vertical, p);
  require(b.add(sample(32, 32, 0)), b.error().c_str());
  finishStroke(b);
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
  finishStroke(first);
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
  finishStroke(second);
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
      finishStroke(stroke);
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
      finishStroke(stroke);
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
void testKritaPressureHsvCandidate() {
  struct WideCanvas : Canvas {
    WideCanvas() {
      w = 320;
      h = 64;
      pixels.assign(size_t(w) * h, {});
    }
    double coverage(int, int) const override { return 1; }
  };
  // Independent Node source captures at eleven dab centers. Full-region
  // reference jobs additionally gate every RGB/alpha pixel and geometry.
  const int colors[2][3]          = {{173, 91, 47}, {59, 137, 191}};
  const int captured[3][2][11][3] = {{{{117, 47, 173},
                                       {160, 47, 173},
                                       {173, 47, 144},
                                       {173, 47, 101},
                                       {173, 47, 59},
                                       {173, 78, 47},
                                       {173, 120, 47},
                                       {173, 163, 47},
                                       {141, 173, 47},
                                       {98, 173, 47},
                                       {56, 173, 47}},
                                      {{110, 191, 59},
                                       {65, 191, 59},
                                       {59, 191, 97},
                                       {59, 191, 142},
                                       {59, 191, 187},
                                       {59, 151, 191},
                                       {59, 106, 191},
                                       {59, 62, 191},
                                       {101, 59, 191},
                                       {145, 59, 191},
                                       {190, 59, 191}}},
                                     {{{173, 140, 122},
                                       {173, 131, 108},
                                       {173, 122, 94},
                                       {173, 112, 80},
                                       {173, 103, 66},
                                       {173, 94, 51},
                                       {173, 84, 36},
                                       {173, 70, 14},
                                       {173, 60, 0},
                                       {173, 60, 0},
                                       {173, 60, 0}},
                                      {{138, 169, 191},
                                       {123, 163, 191},
                                       {108, 157, 191},
                                       {93, 151, 191},
                                       {78, 145, 191},
                                       {64, 139, 191},
                                       {47, 132, 191},
                                       {24, 123, 191},
                                       {0, 113, 191},
                                       {0, 113, 191},
                                       {0, 113, 191}}},
                                     {{{70, 37, 19},
                                       {89, 47, 24},
                                       {109, 57, 29},
                                       {128, 67, 35},
                                       {147, 78, 40},
                                       {167, 88, 45},
                                       {179, 104, 63},
                                       {189, 122, 86},
                                       {198, 141, 110},
                                       {207, 159, 133},
                                       {216, 177, 157}},
                                      {{24, 55, 77},
                                       {30, 71, 98},
                                       {37, 86, 120},
                                       {44, 101, 141},
                                       {50, 117, 163},
                                       {57, 132, 184},
                                       {74, 146, 196},
                                       {96, 159, 203},
                                       {118, 173, 210},
                                       {140, 186, 218},
                                       {162, 199, 225}}}};
  for (int variant = 0; variant < 3; ++variant)
    for (int color = 0; color < 2; ++color) {
      Preset p;
      p.profile         = "krita-5.3.4.1-hard-pixel-candidate-1";
      p.size.value      = 24;
      p.spacing         = 1;
      p.color           = {colors[color][0] / 255., colors[color][1] / 255.,
                           colors[color][2] / 255., 1};
      const int channel = variant;
      p.sourceHsvV2[channel].enabled = true;
      p.sourceHsvV2[channel].control = {
          1, 0, {{Sensor::Pressure, {{0, 0}, {1, 1}}, 1}}};
      WideCanvas canvas;
      Stroke stroke(canvas, p);
      require(stroke.add(sample(32, 32, 0, .2)) &&
                  stroke.add(sample(288, 32, 1, .8)),
              stroke.error().c_str());
      stroke.finish();
      require(stroke.error().empty(), "source HSV pressure finish failed");
      for (int dab = 0; dab < 11; ++dab) {
        const auto pixel = canvas.read(33 + 24 * dab, 32);
        require(
            pixel.a == 1 &&
                int(std::floor(pixel.r * 255 + .5)) ==
                    captured[variant][color][dab][0] &&
                int(std::floor(pixel.g * 255 + .5)) ==
                    captured[variant][color][dab][1] &&
                int(std::floor(pixel.b * 255 + .5)) ==
                    captured[variant][color][dab][2],
            "source H/S/V pressure differs from independently captured dab");
      }
      p.sourceHsvV2[(channel + 1) % 3] = p.sourceHsvV2[channel];
      p.sourceHsvV2[(channel + 1) % 3].control.mappings[0].sensor =
          Sensor::TiltX;
      require(!validate(p).empty(),
              "unqualified simultaneous tilt HSV admitted");
    }
}

void testKritaMixedPressureHsvCandidate() {
  struct WideCanvas : Canvas {
    WideCanvas() {
      w = 320;
      h = 64;
      pixels.assign(size_t(w) * h, {});
    }
    double coverage(int, int) const override { return 1; }
  };
  // Original source captures: mixed/fixed companions and simultaneous ramps.
  // Values are captured RGB bytes, never generated by the candidate model.
  const int captured[14][11][3] = {{{135, 136, 132},
                                    {132, 136, 130},
                                    {127, 136, 128},
                                    {125, 136, 131},
                                    {123, 136, 135},
                                    {121, 130, 136},
                                    {118, 122, 136},
                                    {119, 114, 136},
                                    {126, 109, 136},
                                    {136, 103, 134},
                                    {136, 96, 117}},
                                   {{33, 34, 30},
                                    {49, 54, 47},
                                    {64, 73, 65},
                                    {81, 92, 87},
                                    {98, 111, 110},
                                    {115, 124, 130},
                                    {133, 136, 148},
                                    {155, 152, 164},
                                    {177, 171, 181},
                                    {198, 190, 197},
                                    {214, 209, 212}},
                                   {{33, 34, 34},
                                    {51, 52, 54},
                                    {68, 70, 73},
                                    {84, 88, 92},
                                    {100, 106, 111},
                                    {115, 123, 130},
                                    {131, 139, 148},
                                    {148, 156, 164},
                                    {164, 173, 181},
                                    {182, 190, 198},
                                    {201, 208, 214}},
                                   {{34, 34, 33},
                                    {52, 54, 51},
                                    {68, 73, 69},
                                    {84, 92, 88},
                                    {100, 111, 110},
                                    {115, 125, 130},
                                    {131, 135, 148},
                                    {151, 148, 164},
                                    {175, 164, 181},
                                    {198, 182, 197},
                                    {214, 201, 208}},
                                   {{205, 207, 196},
                                    {200, 207, 196},
                                    {196, 207, 198},
                                    {196, 207, 202},
                                    {196, 207, 207},
                                    {196, 203, 207},
                                    {196, 199, 207},
                                    {199, 196, 207},
                                    {203, 196, 207},
                                    {207, 196, 207},
                                    {207, 196, 202}},
                                   {{206, 207, 206},
                                    {205, 207, 206},
                                    {204, 207, 205},
                                    {203, 207, 204},
                                    {202, 207, 204},
                                    {201, 207, 203},
                                    {200, 207, 202},
                                    {199, 207, 201},
                                    {197, 207, 200},
                                    {194, 207, 198},
                                    {191, 207, 196}},
                                   {{27, 34, 30},
                                    {43, 54, 46},
                                    {58, 73, 62},
                                    {73, 92, 79},
                                    {88, 111, 95},
                                    {104, 130, 112},
                                    {123, 148, 130},
                                    {143, 164, 150},
                                    {164, 181, 169},
                                    {184, 198, 188},
                                    {205, 214, 208}},
                                   {{0, 173, 102},
                                    {0, 173, 29},
                                    {44, 173, 0},
                                    {117, 173, 0},
                                    {173, 158, 15},
                                    {173, 104, 41},
                                    {173, 66, 59},
                                    {173, 77, 112},
                                    {173, 95, 156},
                                    {160, 112, 173},
                                    {146, 130, 173}},
                                   {{202, 234, 221},
                                    {173, 223, 181},
                                    {161, 211, 144},
                                    {172, 200, 115},
                                    {188, 178, 85},
                                    {177, 114, 56},
                                    {156, 49, 42},
                                    {132, 36, 71},
                                    {108, 29, 91},
                                    {71, 23, 83},
                                    {32, 16, 59}},
                                   {{234, 192, 170},
                                    {223, 157, 122},
                                    {211, 128, 83},
                                    {200, 115, 69},
                                    {188, 104, 59},
                                    {177, 94, 50},
                                    {156, 89, 53},
                                    {132, 84, 59},
                                    {108, 76, 59},
                                    {83, 64, 54},
                                    {59, 49, 44}},
                                   {{170, 234, 208},
                                    {122, 223, 139},
                                    {116, 211, 83},
                                    {157, 200, 69},
                                    {188, 176, 59},
                                    {177, 111, 50},
                                    {156, 60, 53},
                                    {132, 59, 85},
                                    {108, 59, 97},
                                    {77, 54, 83},
                                    {50, 44, 59}},
                                   {{136, 222, 187},
                                    {136, 222, 150},
                                    {157, 222, 136},
                                    {194, 222, 136},
                                    {222, 214, 136},
                                    {222, 177, 136},
                                    {222, 141, 136},
                                    {222, 136, 167},
                                    {222, 136, 204},
                                    {204, 136, 222},
                                    {168, 136, 222}},
                                   {{222, 120, 207},
                                    {222, 120, 207},
                                    {222, 126, 208},
                                    {222, 145, 211},
                                    {222, 159, 213},
                                    {222, 169, 214},
                                    {222, 177, 215},
                                    {222, 184, 216},
                                    {222, 191, 218},
                                    {222, 198, 219},
                                    {222, 205, 220}},
                                   {{234, 179, 226},
                                    {223, 137, 210},
                                    {211, 95, 194},
                                    {200, 53, 178},
                                    {188, 11, 162},
                                    {177, 0, 150},
                                    {156, 0, 133},
                                    {132, 0, 112},
                                    {108, 0, 92},
                                    {83, 0, 71},
                                    {59, 0, 50}}};
  const char *channels[7] = {"hs", "hv", "sv", "hsv", "hsv", "hsv", "hsv"};
  const char *dynamic[7]  = {"hs", "hv", "sv", "hsv", "h", "s", "v"};
  const int colors[2][3]  = {{120, 128, 136}, {173, 91, 47}};
  for (int color = 0; color < 2; ++color)
    for (int variant = 0; variant < 7; ++variant) {
      Preset p;
      p.profile             = "krita-5.3.4.1-hard-pixel-candidate-1";
      p.size.value          = 24;
      p.spacing             = 1;
      p.color               = {colors[color][0] / 255., colors[color][1] / 255.,
                               colors[color][2] / 255., 1};
      const double fixed[3] = {.3, .7, .8};
      for (int channel = 0; channel < 3; ++channel) {
        const char name = "hsv"[channel];
        if (std::string(channels[variant]).find(name) == std::string::npos)
          continue;
        p.sourceHsvV2[channel].enabled = true;
        if (std::string(dynamic[variant]).find(name) != std::string::npos)
          p.sourceHsvV2[channel].control = {
              1, 0, {{Sensor::Pressure, {{0, 0}, {1, 1}}, 1}}};
        else
          p.sourceHsvV2[channel].control = {fixed[channel], 0, {}};
      }
      WideCanvas canvas;
      Stroke stroke(canvas, p);
      require(stroke.add(sample(32, 32, 0, color ? .875 : .125)) &&
                  stroke.add(sample(288, 32, 1, color ? .125 : .875)),
              stroke.error().c_str());
      require(stroke.finish(), stroke.error().c_str());
      for (int dab = 0; dab < 11; ++dab) {
        const auto pixel     = canvas.read(33 + 24 * dab, 32);
        const auto &expected = captured[color * 7 + variant][dab];
        require(pixel.a == 1 &&
                    int(std::floor(pixel.r * 255 + .5)) == expected[0] &&
                    int(std::floor(pixel.g * 255 + .5)) == expected[1] &&
                    int(std::floor(pixel.b * 255 + .5)) == expected[2],
                "mixed source HSV differs from independently captured output");
      }
    }
}

void testKritaJointHsvCandidate() {
  const int colors[2][3]    = {{101, 137, 157}, {173, 91, 47}};
  const double values[6][3] = {{.5, .65, .65}, {.5, .85, .85}, {.5, .65, .85},
                               {.5, .85, .65}, {.2, .65, .85}, {.8, .85, .65}};
  // Source outputs from independent noncommuting-channel acquisitions.
  const int expected[2][6][3] = {{{128, 166, 186},
                                  {181, 210, 226},
                                  {201, 217, 226},
                                  {81, 149, 186},
                                  {212, 226, 201},
                                  {186, 81, 170}},
                                 {{198, 113, 67},
                                  {230, 181, 154},
                                  {230, 194, 174},
                                  {198, 81, 19},
                                  {205, 174, 230},
                                  {19, 198, 46}}};
  for (int color = 0; color < 2; ++color)
    for (int variant = 0; variant < 6; ++variant) {
      Preset p;
      p.profile    = "krita-5.3.4.1-hard-pixel-candidate-1";
      p.size.value = 24;
      p.color      = {colors[color][0] / 255., colors[color][1] / 255.,
                      colors[color][2] / 255., 1};
      for (int channel = 0; channel < 3; ++channel) {
        p.sourceHsvV2[channel].enabled       = channel > 0 || variant >= 4;
        p.sourceHsvV2[channel].control.value = values[variant][channel];
      }
      Canvas canvas;
      Stroke stroke(canvas, p);
      require(stroke.add(sample(64, 64, 0)), stroke.error().c_str());
      finishStroke(stroke);
      const auto pixel = canvas.read(64, 64);
      require(pixel.a == 1 &&
                  int(std::floor(pixel.r * 255 + .5)) ==
                      expected[color][variant][0] &&
                  int(std::floor(pixel.g * 255 + .5)) ==
                      expected[color][variant][1] &&
                  int(std::floor(pixel.b * 255 + .5)) ==
                      expected[color][variant][2],
              "source joint chroma model differs from captured output");
    }
}

void testKritaFixedHsvCandidate() {
  // Independently acquired K5341 opaque center pixels, two foreground colors,
  // controls 0,.2,.4,.6,.8,1. No expected values come from candidate equations.
  const int colors[2][3]         = {{173, 91, 47}, {59, 137, 191}};
  const int observed[3][2][6][3] = {{{{47, 129, 173},
                                      {116, 47, 173},
                                      {173, 47, 79},
                                      {173, 167, 47},
                                      {47, 173, 66},
                                      {47, 129, 173}},
                                     {{191, 113, 59},
                                      {111, 191, 59},
                                      {59, 191, 166},
                                      {60, 59, 191},
                                      {191, 59, 163},
                                      {191, 113, 59}}},
                                    {{{173, 173, 173},
                                      {173, 140, 123},
                                      {173, 107, 72},
                                      {173, 68, 12},
                                      {173, 60, 0},
                                      {173, 60, 0}},
                                     {{191, 191, 191},
                                      {138, 169, 191},
                                      {85, 148, 191},
                                      {22, 122, 191},
                                      {0, 113, 191},
                                      {0, 113, 191}}},
                                    {{{0, 0, 0},
                                      {69, 36, 19},
                                      {138, 73, 38},
                                      {189, 124, 89},
                                      {222, 189, 172},
                                      {255, 255, 255}},
                                     {{0, 0, 0},
                                      {24, 55, 76},
                                      {47, 110, 153},
                                      {98, 161, 204},
                                      {177, 208, 229},
                                      {255, 255, 255}}}};
  for (int channel = 0; channel < 3; ++channel)
    for (int color = 0; color < 2; ++color)
      for (int control = 0; control < 6; ++control)
        for (bool enabled : {false, true}) {
          Preset p;
          p.profile    = "krita-5.3.4.1-hard-pixel-candidate-1";
          p.size.value = 24;
          p.color      = {colors[color][0] / 255., colors[color][1] / 255.,
                          colors[color][2] / 255., 1};
          p.sourceHsvChannel =
              enabled ? SourceHsvChannel(channel + 1) : SourceHsvChannel::Off;
          p.sourceHsvValue = control / 5.;
          Canvas canvas;
          Stroke stroke(canvas, p);
          require(stroke.add(sample(64, 64, 0)), stroke.error().c_str());
          finishStroke(stroke);
          const Pixel pixel      = canvas.read(64, 64);
          const double actual[3] = {pixel.r, pixel.g, pixel.b};
          require(pixel.a == 1, "source HSV changed opaque dab alpha");
          for (int c = 0; c < 3; ++c) {
            const int expected = enabled ? observed[channel][color][control][c]
                                         : colors[color][c];
            // Saturation has a measured one-byte residual; it remains a
            // candidate, not byte-exact source verification.
            const int tolerance = enabled && channel == 1 ? 1 : 0;
            require(std::abs(int(std::floor(actual[c] * 255 + .5)) -
                             expected) <= tolerance,
                    "source HSV differs from independently captured RGB");
          }
        }
  // Previously unseen source acquisitions, including the achromatic boundary.
  for (const auto &capture :
       std::vector<std::array<int, 7>>{{211, 37, 149, 125, 211, 168, 196},
                                       {101, 101, 101, 875, 101, 101, 101}}) {
    Preset heldout;
    heldout.profile    = "krita-5.3.4.1-hard-pixel-candidate-1";
    heldout.size.value = 24;
    heldout.color = {capture[0] / 255., capture[1] / 255., capture[2] / 255.,
                     1};
    heldout.sourceHsvChannel = SourceHsvChannel::Saturation;
    heldout.sourceHsvValue   = capture[3] / 1000.;
    Canvas canvas;
    Stroke stroke(canvas, heldout);
    require(stroke.add(sample(64, 64, 0)), stroke.error().c_str());
    finishStroke(stroke);
    const Pixel actual = canvas.read(64, 64);
    require(std::floor(actual.r * 255 + .5) == capture[4] &&
                std::floor(actual.g * 255 + .5) == capture[5] &&
                std::floor(actual.b * 255 + .5) == capture[6] && actual.a == 1,
            "source saturation heldout differs");
  }
  Preset invalid;
  invalid.sourceHsvChannel = SourceHsvChannel::Hue;
  require(!validate(invalid).empty(), "native profile admitted source HSV");
  invalid.profile = "krita-5.3.4.1-hard-pixel-candidate-1";
  invalid.wash    = true;
  require(!validate(invalid).empty(), "unqualified source HSV wash admitted");
  invalid.wash       = false;
  invalid.flow.value = .5;
  require(!validate(invalid).empty(),
          "unqualified source HSV transfer admitted");
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
    sourceFixtureMotion(stroke, sample(64, 64, 0));
    finishStroke(stroke);
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
  finishStroke(a);
  finishStroke(b);
  require(a.dabCount() == 2 && b.dabCount() == 2 && same(sparse, divided),
          "source endpoint scheduling depends on event subdivision");
  p.size.value = 32;
  p.spacing    = .1;
  FullCanvas longCanvas;
  Stroke longStroke(longCanvas, p);
  require(
      longStroke.add(sample(0, 64, 0)) && longStroke.add(sample(1536, 64, 1)),
      "source long decimal-spacing path failed");
  finishStroke(longStroke);
  require(longStroke.dabCount() == 480,
          "decimal spacing emitted a spurious final source dab");
  p.size.minimum =
      0;  // Explicit source curve has no native minimum-size offset.
  p.size.mappings.push_back({Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
  FullCanvas pressured;
  Stroke pressureStroke(pressured, p);
  require(pressureStroke.add(sample(64, 64, 0, .25)),
          pressureStroke.error().c_str());
  sourceFixtureMotion(pressureStroke, sample(64, 64, 0, .25));
  finishStroke(pressureStroke);
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
  finishStroke(risingStroke);
  require(risingStroke.dabCount() == 106,
          "source sparse pressure spacing differs from captured footprints");
  p.size.mappings.front().curve = {{0, .25}, {1, .75}};
  FullCanvas minimumPressure;
  Stroke minimumStroke(minimumPressure, p);
  require(minimumStroke.add(sample(64, 64, 0, 0)),
          minimumStroke.error().c_str());
  sourceFixtureMotion(minimumStroke, sample(64, 64, 0, 0));
  finishStroke(minimumStroke);
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
  require(buildUpStroke.add(sample(64, 64, 0)), buildUpStroke.error().c_str());
  sourceFixtureMotion(buildUpStroke, sample(64, 64, 0));
  finishStroke(buildUpStroke);
  require(buildUp.read(64, 64).a == .5,
          "source build-up incorrectly applied inactive flow");
  p.wash = true;
  FullCanvas wash;
  Stroke washStroke(wash, p);
  require(washStroke.add(sample(64, 64, 0)), washStroke.error().c_str());
  sourceFixtureMotion(washStroke, sample(64, 64, 0));
  finishStroke(washStroke);
  require(wash.read(64, 64).a == 0, "source wash ignored zero flow");
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
void testNativePressureDeliveryBatches() {
  // Batching preserves every authored sample. This does NOT resample a source
  // path or assert invariance under a changed sequence of tablet events.
  Preset p;
  p.size.value    = 24;
  p.spacing       = .1;
  p.size.mappings = {Mapping{}};
  p.opacity.value = .5;
  Canvas direct, batched;
  Stroke a(direct, p), b(batched, p);
  std::vector<Sample> events;
  for (int i = 0; i <= 32; ++i)
    events.push_back(sample(32 + i * .5, 32, i / 32.0, .25 + .5 * i / 32.0));
  for (const auto &event : events) require(a.add(event), a.error().c_str());
  for (size_t offset = 0; offset < events.size(); offset += 7)
    for (size_t i = offset; i < std::min(offset + 7, events.size()); ++i)
      require(b.add(events[i]), b.error().c_str());
  require(same(direct, batched) && a.dabCount() == b.dabCount(),
          "native identical pressure events changed when delivered in batches");
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
    finishStroke(a);
    finishStroke(b);
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
      finishStroke(stroke);
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
void testSourceWetBoundaryMatrix() {
  // Structural oracle for the original wetboundary controls: four models,
  // four edges, entry/exit/reentry, color 0/.25, D32 and 8px input steps.
  // This checks state preservation across host clipping, not source fidelity.
  struct BoundaryCanvas : Canvas {
    BoundaryCanvas(int side) {
      w = h = side;
      pixels.assign(size_t(w) * h, {});
    }
    double coverage(int, int) const override { return 1; }
  };
  for (const std::string model :
       {"legacy-smear", "new-smear", "legacy-dulling", "new-dulling"})
    for (double colorRate : {0.0, .25})
      for (int edge = 0; edge < 4; ++edge)
        for (int trace = 0; trace < 3; ++trace) {
          BoundaryCanvas finite(256), padded(1024);
          constexpr int padding = 384;
          for (int y = 0; y < finite.h; ++y)
            for (int x = 0; x < finite.w; ++x) {
              const Pixel initial = x < 64
                                        ? Pixel{0, 0, 1, 1}
                                        : Pixel{128 / 255.0, 0, 0, 128 / 255.0};
              finite.write(x, y, initial);
              padded.write(x + padding, y + padding, initial);
            }
          Preset p;
          p.profile = "krita-5.3.4.1-" + model + "-candidate-1";
          p.wet = model.find("dulling") != std::string::npos ? Wet::Dulling
                                                            : Wet::Smear;
          p.tip.antialiasEdges = false;
          p.size.value = 32;
          p.spacing = .5;
          p.smudgeRadius = 0;
          p.color = {0, 1, 0, 1};
          p.colorRate = colorRate;
          const std::vector<std::pair<int, int>> paths[] = {
              {{-96, 128}, {128, 128}},
              {{128, 128}, {-96, 128}},
              {{128, 128}, {-96, 128}, {-96, 192}, {128, 192}}};
          auto transform = [edge](std::pair<int, int> point) {
            const int x = point.first, y = point.second;
            return edge == 0 ? std::make_pair(x, y)
                 : edge == 1 ? std::make_pair(256 - x, y)
                 : edge == 2 ? std::make_pair(y, x)
                             : std::make_pair(y, 256 - x);
          };
          auto budget = std::make_shared<MemoryBudget>(8 * 1024 * 1024);
          Stroke clipped(finite, p, 8 * 1024 * 1024, budget), full(padded, p);
          double time = 0;
          for (size_t segment = 1; segment < paths[trace].size(); ++segment) {
            const auto a = transform(paths[trace][segment - 1]);
            const auto b = transform(paths[trace][segment]);
            const int steps = std::max(std::abs(b.first - a.first),
                                       std::abs(b.second - a.second)) / 8;
            for (int index = segment == 1 ? 0 : 1; index <= steps; ++index) {
              const double x = a.first + (b.first - a.first) * double(index) / steps;
              const double y = a.second + (b.second - a.second) * double(index) / steps;
              require(clipped.add(sample(x, y, time)), clipped.error().c_str());
              require(full.add(sample(x + padding, y + padding, time)),
                      full.error().c_str());
              time += 1 / 240.0;
            }
          }
          require(clipped.dabCount() == full.dabCount(),
                  "document edge changed source wet dab cadence");
          for (int y = 0; y < finite.h; ++y)
            for (int x = 0; x < finite.w; ++x) {
              const Pixel a = finite.read(x, y);
              const Pixel b = padded.read(x + padding, y + padding);
              require(std::abs(a.r - b.r) < 1e-12 &&
                          std::abs(a.g - b.g) < 1e-12 &&
                          std::abs(a.b - b.b) < 1e-12 &&
                          std::abs(a.a - b.a) < 1e-12,
                      "wet entry/exit/reentry lost state at a document edge");
            }
          finishStroke(clipped);
          finishStroke(full);
          require(budget->used() == 0,
                  "completed boundary matrix retained transient state");
        }
}
void testSourceWetLengthToggleMeaning() {
  // Original c7 wetopacitycombo RGBA8 centers at half opacity, length off,
  // alpha transport on/off. Retained length=.5 still attenuates legacy color;
  // disabled length selects full transport, never zero or native pickup.
  const int captured[3][2][2][4] = {
      {{{85, 11, 159, 192}, {148, 18, 88, 162}},
       {{85, 11, 159, 192}, {157, 17, 81, 177}}},
      {{{82, 11, 163, 193}, {143, 18, 93, 164}},
       {{82, 11, 163, 193}, {153, 17, 85, 179}}},
      {{{105, 11, 139, 181}, {173, 18, 64, 153}},
       {{110, 5, 141, 179}, {189, 7, 60, 164}}}};
  struct LengthCanvas : Canvas {
    LengthCanvas() {
      w = 640;
      pixels.resize(size_t(w) * h);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          write(x, y, x < 336 ? Pixel{0, 0, 1, 1}
                              : Pixel{128 / 255.0, 0, 0, 128 / 255.0});
    }
    double coverage(int, int) const override { return 1; }
  };
  for (const std::string model :
       {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"})
    for (int alpha = 0; alpha < 2; ++alpha) {
      const int group = model == "new-smear" ? 1 : model == "new-dulling" ? 2 : 0;
      LengthCanvas canvas;
      Preset p;
      p.profile = "krita-5.3.4.1-" + model + "-candidate-1";
      p.wet = model.find("dulling") != std::string::npos ? Wet::Dulling
                                                        : Wet::Smear;
      p.tip.antialiasEdges = false;
      p.size.value = 32;
      p.spacing = 5;
      p.smudgeRadius = 0;
      p.pickup = .5;
      p.colorRate = .25;
      p.color = {0, 1, 0, 1};
      p.opacity.value = .5;
      p.sourceSmudgeLengthEnabled = false;
      p.sourceSmearAlpha = alpha == 0;
      Stroke stroke(canvas, p);
      require(stroke.add(sample(256, 64, 0)) && stroke.add(sample(577, 64, 1)),
              stroke.error().c_str());
      finishStroke(stroke);
      require(stroke.dabCount() == 3,
              "disabled length changed source wet spacing");
      for (int dab = 0; dab < 2; ++dab) {
        const Pixel actual = canvas.read(416 + 160 * dab, 64);
        require(actual.a > 0, "disabled length erased a captured wet deposit");
        const int encoded[] = {int(std::lround(actual.r / actual.a * 255)),
                               int(std::lround(actual.g / actual.a * 255)),
                               int(std::lround(actual.b / actual.a * 255)),
                               int(std::lround(actual.a * 255))};
        for (int channel = 0; channel < 4; ++channel)
          require(std::abs(encoded[channel] - captured[group][alpha][dab][channel]) <= 1,
                  "disabled length/alpha state differs from source center");
      }
    }
}
void testSourceWetOffCanvasState() {
  struct UnselectedCanvas : Canvas {
    double coverage(int, int) const override { return 1; }
  };
  // Cropping the host raster must not alter a continuous stroke's transient
  // off-canvas transport. A larger transparent canvas is an independent
  // boundary oracle; only the finite host writes are compared.
  for (const char *model :
       {"legacy-smear", "new-smear", "legacy-dulling", "new-dulling"})
    for (double colorRate : {0.0, .25})
      for (double pickupRadius : {0.0, 1.0, 3.0}) {
        UnselectedCanvas finite, padded;
        padded.w = 512;
        padded.h = 256;
        padded.pixels.assign(size_t(padded.w) * padded.h, {});
        for (int y = 0; y < finite.h; ++y)
          for (int x = 0; x < finite.w; ++x) {
            Pixel pixel = x < 64 ? Pixel{0, 0, 1, 1}
                                 : Pixel{128 / 255.0, 0, 0, 128 / 255.0};
            finite.write(x, y, pixel);
            padded.write(x + 192, y + 64, pixel);
          }
        Preset p;
        p.profile = std::string("krita-5.3.4.1-") + model + "-candidate-1";
        p.wet     = std::string(model).find("dulling") != std::string::npos
                        ? Wet::Dulling
                        : Wet::Smear;
        if ((p.wet == Wet::Smear && pickupRadius != 0) ||
            (std::string(model) == "legacy-dulling" && pickupRadius > 1))
          continue;
        p.tip.antialiasEdges = false;
        p.size.value         = 16;
        p.spacing            = .5;
        p.smudgeRadius       = pickupRadius;
        p.color              = {0, 1, 0, 1};
        p.colorRate          = colorRate;
        auto shared          = std::make_shared<MemoryBudget>(8 * 1024 * 1024);
        Stroke clipped(finite, p, 8 * 1024 * 1024, shared), full(padded, p);
        const std::pair<int, int> path[] = {
            {64, 48}, {-64, 48}, {-64, 80}, {64, 80}};
        double time = 0;
        for (int segment = 0; segment < 3; ++segment) {
          const auto a = path[segment], b = path[segment + 1];
          const int steps = std::max(std::abs(a.first - b.first),
                                     std::abs(a.second - b.second)) /
                            4;
          for (int i = segment ? 1 : 0; i <= steps; ++i) {
            const double x = a.first + (b.first - a.first) * double(i) / steps,
                         y = a.second +
                             (b.second - a.second) * double(i) / steps;
            require(clipped.add(sample(x, y, time)), clipped.error().c_str());
            require(full.add(sample(x + 192, y + 64, time)),
                    full.error().c_str());
            time += 1 / 240.0;
          }
        }
        for (int y = 0; y < finite.h; ++y)
          for (int x = 0; x < finite.w; ++x) {
            const Pixel a = finite.read(x, y), b = padded.read(x + 192, y + 64);
            require(
                std::abs(a.r - b.r) < 1e-12 && std::abs(a.g - b.g) < 1e-12 &&
                    std::abs(a.b - b.b) < 1e-12 && std::abs(a.a - b.a) < 1e-12,
                "off-canvas wet state differs from transparent padded canvas");
          }
        require(shared->used() > 0,
                "off-canvas state escaped memory accounting");
        finishStroke(clipped);
        finishStroke(full);
        require(shared->used() == 0,
                "finished off-canvas wet state retained memory");
      }
}
void testSourceTextureMasks() {
  auto mask   = std::make_shared<Image>();
  mask->width = mask->height = 16;
  mask->pixels.resize(256);
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x)
      mask->pixels[y * 16 + x] = {0, 0, 0, (x / 4) / 3.0};
  // Original K5341 grayscale-band captures. Soft texturing attenuates the
  // operation; legacy strength instead attenuates the incoming mask.
  const int captured[2][2][4] = {{{0, 43, 85, 128}, {127, 170, 212, 255}},
                                 {{128, 43, 0, 0}, {255, 212, 170, 127}}};
  for (int mode = 0; mode < 2; ++mode)
    for (int soft = 0; soft < 2; ++soft)
      for (bool invert : {false, true}) {
        Canvas canvas;
        Preset p;
        p.profile            = "krita-5.3.4.1-hard-pixel-candidate-1";
        p.tip.antialiasEdges = false;
        p.size.value         = 64;
        p.texture.image      = mask;
        p.texture.depth      = .5;
        p.texture.mode       = mode ? MaskMode::Subtract : MaskMode::Multiply;
        p.texture.invert     = invert;
        p.sourceTextureSoftTexturing = soft;
        Stroke stroke(canvas, p);
        require(stroke.add(sample(64, 64, 0)), stroke.error().c_str());
        sourceFixtureMotion(stroke, sample(64, 64, 0));
        finishStroke(stroke);
        for (int band = 0; band < 4; ++band)
          require(
              std::abs(canvas.read(64 + 4 * band, 64).a * 255 -
                       captured[mode][soft][invert ? 3 - band : band]) < 1e-5,
              "source texture byte-domain strength/polarity changed");
      }
  const int wetCaptured[3][4][4] = {{{255, 0, 0, 128},
                                     {233, 12, 10, 134},
                                     {193, 24, 38, 146},
                                     {143, 36, 76, 164}},
                                    {{255, 0, 0, 128},
                                     {245, 10, 0, 131},
                                     {235, 20, 0, 133},
                                     {225, 30, 0, 136}},
                                    {{255, 0, 0, 128},
                                     {231, 12, 12, 134},
                                     {188, 24, 43, 147},
                                     {134, 36, 85, 168}}};
  const char *models[] = {"legacy-smear", "legacy-dulling", "new-smear"};
  for (int model = 0; model < 3; ++model) {
    Canvas canvas;
    for (int y = 0; y < canvas.h; ++y)
      for (int x = 0; x < canvas.w; ++x)
        canvas.write(
            x, y,
            x < 32 ? Pixel{0, 0, 1, 1} : Pixel{128 / 255.0, 0, 0, 128 / 255.0});
    Preset p;
    p.profile = std::string("krita-5.3.4.1-") + models[model] + "-candidate-1";
    p.wet     = model == 1 ? Wet::Dulling : Wet::Smear;
    p.tip.antialiasEdges = false;
    p.size.value         = 32;
    p.spacing            = 1;
    p.smudgeRadius       = 0;
    p.color              = {0, 1, 0, 1};
    p.texture.image      = mask;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(16, 64, 0)) && stroke.add(sample(81, 64, 1)),
            stroke.error().c_str());
    require(stroke.dabCount() == 3,
            "source textured wet trace spacing changed");
    for (int band = 0; band < 4; ++band) {
      const Pixel actual  = canvas.read(80 + band * 4, 64);
      const int *expected = wetCaptured[model][band];
      require(
          actual.a > 0 && std::abs(actual.a * 255 - expected[3]) <= 1.000001 &&
              std::abs(actual.r / actual.a * 255 - expected[0]) <= 1.000001 &&
              std::abs(actual.g / actual.a * 255 - expected[1]) <= 1.000001 &&
              std::abs(actual.b / actual.a * 255 - expected[2]) <= 1.000001,
          "source texture feedback differs from original third-dab capture");
    }
  }
  Preset wet;
  wet.profile            = "krita-5.3.4.1-new-dulling-candidate-1";
  wet.wet                = Wet::Dulling;
  wet.tip.antialiasEdges = false;
  wet.smudgeRadius       = 0;
  wet.texture.image      = mask;
  require(!validate(wet).empty(), "unmeasured new Dulling texture admitted");
}
void testSourceWetForegroundTransformPlumbing() {
  // Original Pixel RGB captures establish the foreground transform separately
  // from wet transport. Feed the rendered foreground into each existing wet
  // equation and compare with those captured colors injected directly. This
  // qualifies composition plumbing, not unmeasured imported wet HSV support.
  const int captured[3][2][3] = {{{116, 47, 173}, {47, 173, 66}},
                                 {{173, 140, 123}, {173, 60, 0}},
                                 {{69, 36, 19}, {222, 189, 172}}};
  const Pixel original{173 / 255.0, 91 / 255.0, 47 / 255.0, 1};
  for (const char *model :
       {"legacy-smear", "new-smear", "legacy-dulling", "new-dulling"})
    for (int channel = 0; channel < 3; ++channel)
      for (int setting = 0; setting < 2; ++setting)
        for (bool enabled : {false, true})
          for (bool smearAlpha : {false, true}) {
            Preset dry;
            dry.profile    = "krita-5.3.4.1-hard-pixel-candidate-1";
            dry.size.value = 24;
            dry.color      = original;
            dry.sourceHsvChannel =
                enabled ? SourceHsvChannel(channel + 1) : SourceHsvChannel::Off;
            dry.sourceHsvValue = setting ? .8 : .2;
            Canvas swatch;
            Stroke transform(swatch, dry);
            require(transform.add(sample(64, 64, 0)),
                    transform.error().c_str());
            finishStroke(transform);
            Canvas candidate, expected;
            for (int y = 0; y < candidate.h; ++y)
              for (int x = 0; x < candidate.w; ++x) {
                const Pixel background =
                    x < 40 ? Pixel{0, 0, 1, 1}
                           : Pixel{128 / 255.0, 0, 0, 128 / 255.0};
                candidate.write(x, y, background);
                expected.write(x, y, background);
              }
            Preset wet;
            wet.profile =
                std::string("krita-5.3.4.1-") + model + "-candidate-1";
            wet.wet = std::string(model).find("dulling") != std::string::npos
                          ? Wet::Dulling
                          : Wet::Smear;
            wet.size.value         = 8;
            wet.spacing            = 5;
            wet.smudgeRadius       = 0;
            wet.tip.antialiasEdges = false;
            wet.sourceSmearAlpha   = smearAlpha;
            wet.color              = swatch.read(64, 64);
            wet.sourceHsvValue =
                dry.sourceHsvValue;  // Disabled source value is inert.
            Preset direct   = wet;
            const auto &rgb = captured[channel][setting];
            direct.color    = enabled ? Pixel{rgb[0] / 255.0, rgb[1] / 255.0,
                                           rgb[2] / 255.0, 1}
                                      : original;
            Stroke actualStroke(candidate, wet),
                expectedStroke(expected, direct);
            for (const auto &input : {sample(10, 64, 0), sample(91, 64, 1)}) {
              require(actualStroke.add(input), actualStroke.error().c_str());
              require(expectedStroke.add(input),
                      expectedStroke.error().c_str());
            }
            finishStroke(actualStroke);
            finishStroke(expectedStroke);
            const double tolerance =
                enabled && channel == 1 ? 1 / 255.0 + 1e-12 : 1e-12;
            for (size_t i = 0; i < candidate.pixels.size(); ++i) {
              const Pixel a = candidate.pixels[i], b = expected.pixels[i];
              require(std::abs(a.r - b.r) <= tolerance &&
                          std::abs(a.g - b.g) <= tolerance &&
                          std::abs(a.b - b.b) <= tolerance && a.a == b.a,
                      "shared source foreground differs from direct wet color "
                      "injection");
            }
            wet.sourceHsvChannel = SourceHsvChannel(channel + 1);
            require(validate(wet).empty() == smearAlpha,
                    "wet HSV eligibility lost its measured alpha boundary");
          }
}
void testSourceWetJointForegroundComposition() {
  // Red plus a quarter-turn hue and neutral S/V is encoded yellow-green. Compare with
  // direct injection through wet/transfer/texture, not duplicated equations.
  for (const std::string model :
       {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"})
    for (bool pressure : {false, true})
      for (int texture = 0; texture < (model == "new-dulling" ? 1 : 3); ++texture) {
        Preset preset;
        preset.profile = "krita-5.3.4.1-" + model + "-candidate-1";
        preset.wet = model.find("dulling") == std::string::npos ? Wet::Smear : Wet::Dulling;
        preset.size.value = 8;
        preset.spacing = 2;
        preset.tip.antialiasEdges = false;
        preset.smudgeRadius = 0;
        preset.pickup = .5;
        preset.colorRate = .25;
        preset.opacity.value = .5;
        preset.flow.value = .5;
        preset.color = {1, 0, 0, 1};
        for (auto &control : preset.sourceHsvV2) {
          control.enabled = true;
          control.control.value = .5;
        }
        preset.sourceHsvV2[0].control.value = .75;
        if (pressure) {
          Mapping mapping;
          mapping.sensor = Sensor::Pressure;
          mapping.curve = {{0, 0}, {1, 1}};
          preset.sourceHsvV2[0].control = {1, 0, {mapping}};
        }
        if (texture) {
          auto image = std::make_shared<Image>();
          image->width = 2;
          image->height = 1;
          image->pixels = {{64 / 255., 64 / 255., 64 / 255., 1},
                           {192 / 255., 192 / 255., 192 / 255., 1}};
          preset.texture.image = image;
          preset.texture.mode = texture == 1 ? MaskMode::Multiply : MaskMode::Subtract;
          preset.texture.depth = .5;
        }
        require(validate(preset).empty(), "wet HSV composition candidate rejected");
        Preset direct = preset;
        direct.sourceHsvV2 = {};
        direct.color = {128. / 255, 1, 0, 1};
        Canvas actual, expected;
        for (size_t i = 0; i < actual.pixels.size(); ++i)
          actual.pixels[i] = expected.pixels[i] = {0, 0, .5, .5};
        Stroke a(actual, preset), b(expected, direct);
        for (const auto &point : {sample(16, 64, 0, .75), sample(81, 64, 1, .75)}) {
          require(a.add(point), a.error().c_str());
          require(b.add(point), b.error().c_str());
        }
        finishStroke(a);
        finishStroke(b);
        require(same(actual, expected), "joint wet HSV changed injection/texture order");
      }
}

void testSourceWetHsvReferenceInteraction() {
  // Original K5341 captures: two foregrounds, each fixed channel at .2/.8,
  // enabled/disabled, four wet models. At the third dab the blue backdrop
  // meets retained foreground from a preceding transparent dab.
  const int captured[96][4] = {
      {26, 14, 223, 150}, {18, 7, 243, 150},  {26, 14, 223, 150},
      {7, 26, 226, 150},  {9, 21, 245, 150},  {17, 29, 225, 150},
      {9, 21, 245, 150},  {29, 9, 241, 150},  {26, 14, 223, 150},
      {26, 21, 235, 150}, {26, 14, 223, 150}, {26, 9, 216, 150},
      {9, 21, 245, 150},  {21, 26, 245, 150}, {9, 21, 245, 150},
      {0, 17, 245, 150},  {26, 14, 223, 150}, {11, 6, 219, 150},
      {26, 14, 223, 150}, {34, 29, 242, 150}, {9, 21, 245, 150},
      {4, 8, 228, 150},   {9, 21, 245, 150},  {27, 32, 251, 150},
      {26, 14, 223, 150}, {18, 7, 243, 150},  {26, 14, 223, 150},
      {7, 26, 226, 150},  {9, 21, 245, 150},  {17, 29, 225, 150},
      {9, 21, 245, 150},  {29, 9, 241, 150},  {26, 14, 223, 150},
      {26, 21, 235, 150}, {26, 14, 223, 150}, {26, 9, 216, 150},
      {9, 21, 245, 150},  {21, 26, 245, 150}, {9, 21, 245, 150},
      {0, 17, 245, 150},  {26, 14, 223, 150}, {11, 6, 219, 150},
      {26, 14, 223, 150}, {34, 29, 242, 150}, {9, 21, 245, 150},
      {4, 8, 228, 150},   {9, 21, 245, 150},  {27, 32, 251, 150},
      {28, 15, 221, 143}, {19, 8, 242, 143},  {28, 15, 221, 143},
      {8, 28, 224, 143},  {10, 22, 245, 143}, {18, 31, 223, 143},
      {10, 22, 245, 143}, {31, 10, 240, 143}, {28, 15, 221, 143},
      {28, 23, 233, 143}, {28, 15, 221, 143}, {28, 10, 213, 143},
      {10, 22, 245, 143}, {23, 28, 245, 143}, {10, 22, 245, 143},
      {0, 18, 245, 143},  {28, 15, 221, 143}, {11, 6, 216, 143},
      {28, 15, 221, 143}, {36, 31, 241, 143}, {10, 22, 245, 143},
      {4, 9, 226, 143},   {10, 22, 245, 143}, {29, 34, 251, 143},
      {23, 12, 227, 165}, {15, 6, 244, 165},  {23, 12, 227, 165},
      {6, 23, 230, 165},  {8, 18, 247, 165},  {15, 25, 229, 165},
      {8, 18, 247, 165},  {25, 8, 243, 165},  {23, 12, 227, 165},
      {23, 19, 237, 165}, {23, 12, 227, 165}, {23, 8, 221, 165},
      {8, 18, 247, 165},  {18, 22, 247, 165}, {8, 18, 247, 165},
      {0, 15, 247, 165},  {23, 12, 227, 165}, {9, 5, 224, 165},
      {23, 12, 227, 165}, {29, 25, 244, 165}, {8, 18, 247, 165},
      {3, 7, 231, 165},   {8, 18, 247, 165},  {23, 28, 252, 165}};
  const int colors[2][3] = {{173, 91, 47}, {59, 137, 191}};
  size_t fixture         = 0;
  for (const char *model :
       {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"})
    for (int channel = 0; channel < 3; ++channel)
      for (const auto &rgb : colors)
        for (double value : {.2, .8})
          for (bool enabled : {false, true}) {
            Canvas canvas;
            for (int y = 0; y < canvas.h; ++y)
              for (int x = 64; x < canvas.w; ++x)
                canvas.write(x, y, {0, 0, 1, 1});
            Preset p;
            p.profile = std::string("krita-5.3.4.1-") + model + "-candidate-1";
            p.wet     = std::string(model).find("dulling") != std::string::npos
                            ? Wet::Dulling
                            : Wet::Smear;
            p.size.value         = 24;
            p.spacing            = 1;
            p.smudgeRadius       = 0;
            p.pickup             = .5;
            p.colorRate          = .25;
            p.tip.antialiasEdges = false;
            p.sourceSmearAlpha   = true;
            p.color = {rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0, 1};
            p.sourceHsvChannel =
                enabled ? SourceHsvChannel(channel + 1) : SourceHsvChannel::Off;
            p.sourceHsvValue = value;
            Stroke stroke(canvas, p);
            require(
                stroke.add(sample(16, 64, 0)) && stroke.add(sample(112, 64, 1)),
                stroke.error().c_str());
            finishStroke(stroke);
            const Pixel actual   = canvas.read(64, 64);
            const auto &expected = captured[fixture++];
            require(std::abs(actual.a * 255 - expected[3]) < 1e-9,
                    "wet HSV reference alpha changed");
            const double straight[] = {actual.r / actual.a, actual.g / actual.a,
                                       actual.b / actual.a};
            for (int c = 0; c < 3; ++c)
              require(std::abs(straight[c] * 255 - expected[c]) <= 2 + 1e-9,
                      "wet HSV foreground/transport reference differs");
            if (enabled) {
              Preset unsupported       = p;
              unsupported.smudgeRadius = .5;
              require(!validate(unsupported).empty(),
                      "unqualified HSV pickup kernel admitted");
              unsupported                  = p;
              unsupported.sourceSmearAlpha = false;
              require(!validate(unsupported).empty(),
                      "unqualified HSV alpha-off admitted");
            }
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
        finishStroke(stroke);
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
        finishStroke(stroke);
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
    finishStroke(stroke);
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
    finishStroke(oddStroke);
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
      finishStroke(switchedStroke);
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
    finishStroke(three);
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
      finishStroke(sequence);
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
    finishStroke(patchStroke);
    auto widerShared = std::make_shared<MemoryBudget>(1024 * 1024);
    Canvas bounded   = original;
    Stroke settingsOnly(bounded, p, 1024 * 1024, widerShared);
    const size_t settingsBytes = widerShared->used();
    finishStroke(settingsOnly);
    const size_t localBudget = settingsBytes + 4096;
    Stroke localLimit(bounded, p, localBudget, widerShared);
    require(localLimit.add(sample(30, 64, 0)),
            "one source snapshot should fit the local budget");
    require(!localLimit.add(sample(71, 64, 1)),
            "replacement source snapshot ignored retained local memory");
    require(
        widerShared->used() <= localBudget,
        "source stroke exceeded its local limit inside a wider shared budget");
    localLimit.cancel();
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
    finishStroke(stroke);
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
    finishStroke(aaStroke);
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
    finishStroke(stroke);
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
      finishStroke(stroke);
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
  finishStroke(stroke);
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
  finishStroke(stroke);
  // Original source-rendered D7 fade .25 pixels, translated from (256,1024).
  const int captured[][3] = {
      {61, 62, 22}, {64, 62, 222}, {62, 63, 177}, {63, 63, 244}};
  for (const auto &pixel : captured)
    require(std::abs(canvas.read(pixel[0], pixel[1]).a * 255 - pixel[2]) < 1e-9,
            "small-circle fade or scalar quantization differs from source");
}
void testSourceBarrelGeometry() {
  // Original static ellipse masks plus the independently captured positive
  // barrel sign: raw315 degrees matches serialized static+45 degrees.
  struct Capture {
    int diameter;
    double aspect, barrel;
    int x, y, w, h;
    uint64_t hash;
  };
  const Capture captures[] = {
      {16, 0.25, 0, -12, -12, 26, 26, 5110072451055542885ull},
      {16, 0.25, 315, -12, -12, 26, 26, 17554875013171802149ull},
      {16, 0.25, 270, -12, -12, 26, 26, 15888210921076884757ull},
      {16, 0.5, 0, -12, -12, 26, 26, 6985544762035993973ull},
      {16, 0.5, 315, -12, -12, 26, 26, 11519218464571737365ull},
      {16, 0.5, 270, -12, -12, 26, 26, 6572898940333968981ull},
      {16, 1, 0, -12, -12, 26, 26, 15671155680365188181ull},
      {16, 1, 315, -12, -12, 26, 26, 15671155680365188181ull},
      {16, 1, 270, -12, -12, 26, 26, 15671155680365188181ull},
      {64, 0.25, 0, -36, -36, 74, 74, 7286171426856152085ull},
      {64, 0.25, 315, -36, -36, 74, 74, 15881757145890768269ull},
      {64, 0.25, 270, -36, -36, 74, 74, 6522406809041249397ull},
      {64, 0.5, 0, -36, -36, 74, 74, 4417048127223711637ull},
      {64, 0.5, 315, -36, -36, 74, 74, 18082161390857717109ull},
      {64, 0.5, 270, -36, -36, 74, 74, 11746254902137415701ull},
      {64, 1, 0, -36, -36, 74, 74, 4871573068758355349ull},
      {64, 1, 315, -36, -36, 74, 74, 4871573068758355349ull},
      {64, 1, 270, -36, -36, 74, 74, 4871573068758355349ull}};
  for (const auto &capture : captures) {
    Preset p;
    p.profile      = "krita-5.3.4.1-hard-pixel-candidate-1";
    p.size.value   = capture.diameter;
    p.aspect.value = capture.aspect;
    p.angle.value  = 360;
    Mapping mapping;
    mapping.sensor   = Sensor::Rotation;
    p.angle.mappings = {mapping};
    Canvas canvas;
    Stroke stroke(canvas, p);
    auto input        = sample(64, 64, 0);
    input.hasRotation = true;
    input.rotation    = capture.barrel * 3.14159265358979323846 / 180;
    require(stroke.add(input), stroke.error().c_str());
    sourceFixtureMotion(stroke, input);
    finishStroke(stroke);
    uint64_t hash = 14695981039346656037ull;
    for (int y = 0; y < capture.h; ++y)
      for (int x = 0; x < capture.w; ++x)
        hash =
            (hash ^ uint64_t(std::lround(
                        canvas.read(64 + capture.x + x, 64 + capture.y + y).a *
                        255))) *
            1099511628211ull;
    require(hash == capture.hash,
            "source barrel mask disagrees with original ellipse capture");
    p.opacity.value = .5;
    require(!validate(p).empty(),
            "unmeasured barrel/transfer combination admitted");
  }
}
void testSourceSymmetricTiltRotation() {
  // Original off-grid rectangle observations identify these orientations
  // modulo180: both signs of10/20 degree tilt yield60/120 degree rotation.
  // Only symmetric hard ellipses are admitted; asymmetric tips need evidence.
  for (Sensor sensor : {Sensor::TiltX, Sensor::TiltY})
    for (double degrees : {-20., -10., 10., 20.}) {
      Preset p;
      p.profile      = "krita-5.3.4.1-hard-pixel-candidate-1";
      p.size.value   = 24;
      p.aspect.value = .5;
      p.angle.value  = 360;
      Mapping mapping;
      mapping.sensor   = sensor;
      p.angle.mappings = {mapping};
      Canvas tilted, barrel;
      Stroke a(tilted, p);
      auto input    = sample(64, 64, 0);
      input.hasTilt = true;
      input.tiltX   = sensor == Sensor::TiltX ? degrees / 60 : 0;
      input.tiltY   = sensor == Sensor::TiltY ? degrees / 60 : 0;
      require(a.add(input), a.error().c_str());
      sourceFixtureMotion(a, input);
      finishStroke(a);
      p.angle.mappings.front().sensor = Sensor::Rotation;
      Stroke b(barrel, p);
      input.hasRotation = true;
      input.rotation    = std::abs(degrees) * 6 * 3.14159265358979323846 / 180;
      require(b.add(input), b.error().c_str());
      sourceFixtureMotion(b, input);
      finishStroke(b);
      if (!same(tilted, barrel)) {
        size_t mismatches = 0, first = 0;
        for (size_t i = 0; i < tilted.pixels.size(); ++i) {
          const auto &x = tilted.pixels[i], &y = barrel.pixels[i];
          if (x.r != y.r || x.g != y.g || x.b != y.b || x.a != y.a) {
            if (!mismatches) first = i;
            ++mismatches;
          }
        }
        const auto &x = tilted.pixels[first], &y = barrel.pixels[first];
        std::ostringstream diagnostic;
        diagnostic << "source tilt normalization disagrees with measured "
                      "orientation class: sensor="
                   << (sensor == Sensor::TiltX ? "TiltX" : "TiltY")
                   << " degrees=" << degrees
                   << " tiltDabs=" << a.dabCount()
                   << " barrelDabs=" << b.dabCount()
                   << " mismatches=" << mismatches
                   << " first=(" << first % tilted.w << ',' << first / tilted.w
                   << ')' << std::hexfloat
                   << " tiltInput="
                   << (sensor == Sensor::TiltX ? input.tiltX : input.tiltY)
                   << " tiltNormalized="
                   << std::abs(sensor == Sensor::TiltX ? input.tiltX : input.tiltY)
                   << " barrelRadians=" << input.rotation
                   << " barrelNormalized="
                   << input.rotation / (2 * 3.14159265358979323846)
                   << " tiltedRGBA=(" << x.r << ',' << x.g << ',' << x.b << ','
                   << x.a << ") barrelRGBA=(" << y.r << ',' << y.g << ','
                   << y.b << ',' << y.a << ')';
        require(false, diagnostic.str().c_str());
      }
      p.angle.mappings.front().sensor = sensor;
      p.tip.shape                     = Shape::Square;
      require(!validate(p).empty(), "unqualified source tilt shape admitted");
    }
}
void testSourcePixelPressureTransfer() {
  // Original constant-pressure Node observations: wash opacity scales the
  // stroke, wash flow and build-up opacity accumulate per dab, build-up flow
  // is inactive. Source byte rounding is allowed one byte at these probes.
  for (bool wash : {false, true})
    for (bool opacityControl : {false, true})
      for (double pressure : {.25, .75}) {
        Preset p;
        p.profile        = "krita-5.3.4.1-hard-pixel-candidate-1";
        p.size.value     = 32;
        p.spacing        = .25;
        p.wash           = wash;
        Dynamic &control = opacityControl ? p.opacity : p.flow;
        control.value    = .5;
        control.mappings = {Mapping{}};
        Canvas canvas;
        Stroke stroke(canvas, p);
        // The source defers a dab at the final input endpoint. Continue past
        // x48 so the captured x768/x776 probes translated to x32/x40
        // receive two/three interior dabs, as in the original long stroke.
        auto first = sample(32, 32, 0), last = sample(56, 32, 1);
        first.pressure = last.pressure = pressure;
        require(stroke.add(first) && stroke.add(last), stroke.error().c_str());
        require(stroke.dabCount() == 3,
                "pressure transfer fixture must emit three interior dabs");
        const bool inactive      = !wash && !opacityControl;
        const bool strokeOpacity = wash && opacityControl;
        const int two            = inactive        ? 255
                                   : strokeOpacity ? (pressure == .25 ? 32 : 96)
                                                   : (pressure == .25 ? 60 : 156);
        const int three          = inactive        ? 255
                                   : strokeOpacity ? two
                                                   : (pressure == .25 ? 84 : 193);
        const auto actualTwo     = std::lround(canvas.read(32, 32).a * 255);
        const auto actualThree   = std::lround(canvas.read(40, 32).a * 255);
        const std::string diagnostic =
            "source pressure transfer disagrees with original Node probes: "
            "wash=" +
            std::to_string(wash) +
            " opacity=" + std::to_string(opacityControl) +
            " pressure=" + std::to_string(pressure) +
            " expected=" + std::to_string(two) + "," + std::to_string(three) +
            " actual=" + std::to_string(actualTwo) + "," +
            std::to_string(actualThree);
        require(std::abs(actualTwo - two) <= 1 &&
                    std::abs(actualThree - three) <= 1,
                diagnostic.c_str());
        control.mappings.front().sensor = Sensor::TiltX;
        require(!validate(p).empty(),
                "unmeasured Pixel transfer sensor admitted");
      }
}
void testSourceTransferCurves() {
  // Independent natural-spline solution for (0,0),(.5,1),(1,0):
  // on [0,.5], f(x)=3*x-4*x^3; f(.25)=.6875.
  // Original Node shared/false-Pressure-flag controls also establish the
  // two-/three-dab bytes below; full-region gates remain independent.
  for (bool wash : {false, true})
    for (bool opacity : {false, true}) {
      Preset p;
      p.profile     = "krita-5.3.4.1-hard-pixel-candidate-1";
      p.size.value  = 32;
      p.wash        = wash;
      p.spacing     = .25;
      auto &control = opacity ? p.opacity : p.flow;
      control.value = .5;
      Mapping curve;
      curve.interpolation = Interpolation::NaturalCubic;
      curve.curve         = {{0, 0}, {.5, 1}, {1, 0}};
      control.mappings    = {curve};
      Canvas canvas;
      Stroke stroke(canvas, p);
      auto input     = sample(32, 32, 0);
      input.pressure = .25;
      require(stroke.add(input), stroke.error().c_str());
      auto firstMotion = input;
      firstMotion.x += .5;
      firstMotion.time = .01;
      require(stroke.add(firstMotion), stroke.error().c_str());
      const double expected = !wash && !opacity ? 1 : .34375;
      require(std::abs(canvas.read(32, 32).a - expected) <= 1.0 / 255,
              "source candidate cubic transfer composition changed");
      auto last     = sample(56, 32, 1);
      last.pressure = .25;
      require(stroke.add(last), stroke.error().c_str());
      finishStroke(stroke);
      require(stroke.dabCount() == 3,
              "source spline fixture lost interior dabs");
      const int two   = !wash && !opacity ? 255 : wash && opacity ? 88 : 145;
      const int three = !wash && !opacity ? 255 : wash && opacity ? 88 : 183;
      require(
          std::abs(std::lround(canvas.read(32, 32).a * 255) - two) <= 1 &&
              std::abs(std::lround(canvas.read(40, 32).a * 255) - three) <= 1,
          "source spline transfer differs from original overlap bytes");
      control.mappings.front().interpolation = Interpolation::Linear;
      require(!validate(p).empty(),
              "source multi-knot transfer admitted linear interpolation");
      control.mappings.front().interpolation = Interpolation::NaturalCubic;
      p.profile = "krita-5.3.4.1-legacy-smear-candidate-1";
      p.wet     = Wet::Smear;
      require(!validate(p).empty(),
              "Pixel cubic transfer broadened wet qualification");
    }
}
void testSourceSmearRasterOrigin() {
  // Translated original 8px wetgeometry PNG support, observed identically in
  // 516 Legacy/New Smear marks: fractional Y, reverse and vertical controls
  // retain this fixed raster. Node.paintPath partition roundoff is separate.
  const char *rows[] = {"..####..", ".######.", "########", "########",
                        "########", "########", ".######.", "..####.."};
  struct Capture {
    double x0, y0, x1, y1;
    int left, top;
  };
  const Capture captures[] = {
      {16, 64, 33, 64, 28, 60},
      {16, 63.875, 33, 63.875, 28, 59},
      {16, 64.125, 33, 64.125, 28, 60},
      {96, 64, 79, 64, 75, 60},
      {64, 16, 64, 33, 60, 28},
  };
  for (const auto &model : {"legacy-smear", "new-smear"}) {
    Preset p;
    p.profile = std::string("krita-5.3.4.1-") + model + "-candidate-1";
    p.tip.antialiasEdges = false;
    p.size.value = 8;
    p.spacing = 2;
    p.wet = Wet::Smear;
    p.smudgeRadius = 0;
    p.color = {0, 1, 0, 1};
    require(validate(p).empty(), "measured Smear geometry profile rejected");
    for (const auto &capture : captures) {
      Canvas canvas;
      for (int y = 0; y < canvas.h; ++y)
        for (int x = 0; x < canvas.w; ++x)
          canvas.write(x, y, {0, 0, 1, 1});
      Stroke stroke(canvas, p);
      require(stroke.add(sample(capture.x0, capture.y0, 0)) &&
                  stroke.add(sample(capture.x1, capture.y1, 1)),
              stroke.error().c_str());
      finishStroke(stroke);
      require(stroke.dabCount() == 2,
              "Smear raster-origin control lost its independent second dab");
      int occupied = 0;
      for (int y = 0; y < canvas.h; ++y)
        for (int x = 0; x < canvas.w; ++x) {
          const int dx = x - capture.left, dy = y - capture.top;
          const bool expected = dx >= 0 && dx < 8 && dy >= 0 && dy < 8 &&
                                rows[dy][dx] == '#';
          const bool changed = canvas.read(x, y).g > 0;
          require(changed == expected,
                  "Smear fractional/vertical raster differs from source support");
          occupied += changed;
        }
      require(occupied == 52, "Smear source raster support changed");
    }
  }
}
void testSourceSmearRecursiveGeometry() {
  // Original 9111a8970 wetgeometry captures: three constant-axis origins and
  // geometrically identical coarse/subdivided turns distinguish recursive
  // future stepping from direct lerp and coordinate-derived remaining length.
  // These are captured support expectations, never runtime position tables.
  struct GeometryCanvas : Canvas {
    GeometryCanvas() {
      w = 800;
      h = 1000;
      pixels.assign(size_t(w) * h, Pixel{0, 0, 1, 1});
    }
    double coverage(int, int) const override { return 1; }
  };
  const char *rows[] = {"..####..", ".######.", "########", "########",
                        "########", "########", ".######.", "..####.."};
  auto capturedMark = [&](const Canvas &canvas, int cx, int cy, int left,
                          int top) {
    for (int y = cy - 6; y <= cy + 6; ++y)
      for (int x = cx - 6; x <= cx + 6; ++x) {
        const int dx = x - left, dy = y - top;
        const bool expected = dx >= 0 && dx < 8 && dy >= 0 && dy < 8 &&
                              rows[dy][dx] == '#';
        require((canvas.read(x, y).g > 0) == expected,
                "recursive Smear geometry differs from captured raster window");
      }
  };
  for (const auto &model : {"legacy-smear", "new-smear"}) {
    Preset p;
    p.profile = std::string("krita-5.3.4.1-") + model + "-candidate-1";
    p.tip.antialiasEdges = false;
    p.size.value = 8;
    p.spacing = 2;
    p.wet = Wet::Smear;
    p.smudgeRadius = 0;
    p.color = {0, 1, 0, 1};
    for (int y : {40, 328, 968}) {
      GeometryCanvas canvas;
      Stroke stroke(canvas, p);
      require(stroke.add(sample(256, y, 0)) && stroke.add(sample(769, y, 1)),
              stroke.error().c_str());
      finishStroke(stroke);
      require(stroke.dabCount() == 33,
              "recursive geometry changed the captured straight dab cadence");
      for (int index = 1; index <= 32; ++index) {
        const bool shifted =
            (y == 328 && ((index >= 5 && index <= 8) ||
                          (index >= 21 && index <= 24))) ||
            (y == 968 && index >= 22 && index <= 27);
        capturedMark(canvas, 256 + 16 * index, y, 252 + 16 * index,
                     y - 4 - int(shifted));
      }
    }
    const double vertices[][2] = {{256, 328}, {768, 328}, {768, 344},
                                   {256, 344}, {256, 360}, {769, 360}};
    for (bool subdivided : {false, true}) {
      GeometryCanvas canvas;
      Stroke stroke(canvas, p);
      require(stroke.add(sample(vertices[0][0], vertices[0][1], 0)),
              stroke.error().c_str());
      int ordinal = 0;
      for (int segment = 1; segment < 6; ++segment) {
        const double dx = vertices[segment][0] - vertices[segment - 1][0];
        const double dy = vertices[segment][1] - vertices[segment - 1][1];
        const int steps = subdivided ? int(std::ceil(std::hypot(dx, dy) / 16)) : 1;
        for (int step = 1; step <= steps; ++step) {
          const double fraction = double(step) / steps;
          require(stroke.add(sample(vertices[segment - 1][0] + dx * fraction,
                                    vertices[segment - 1][1] + dy * fraction,
                                    ++ordinal / 240.0)),
                  stroke.error().c_str());
        }
      }
      finishStroke(stroke);
      require(stroke.dabCount() == 99,
              "path partition changed the captured Smear dab count");
      for (int index = 1; index <= 98; ++index) {
        int x, y, left;
        if (index <= 32) {
          x = 256 + 16 * index;
          y = 328;
          left = x - 4;
        } else if (index <= 65) {
          x = 768 - 16 * (index - 33);
          y = 344;
          left = x - (index == 65 ? 4 : 5);
        } else {
          x = 256 + 16 * (index - 66);
          y = 360;
          left = x - 4;
        }
        const bool shifted = !subdivided &&
                             ((index >= 20 && index <= 22) ||
                              (index >= 87 && index <= 95));
        capturedMark(canvas, x, y, left, y - 4 - int(shifted));
      }
    }
  }
}
void testSourcePathDirectionGeometry() {
  // Original dry count controls from continuous Node.paintPath. The window
  // includes forward, reverse and reverse-to-downward corner footprints.
  // Negative diagnostic coordinates are deliberately outside this crop.
  struct Capture {
    int count;
    uint64_t hash;
  };
  const Capture captures[] = {
      {1, 5422027869455958821ull},      {10, 4613665443013575461ull},
      {100, 4613665443013575461ull},    {1000, 15843215279964946725ull},
      {10000, 15843215279964946725ull},
  };
  auto point = [](int index) {
    int row = index / 128, column = index % 128;
    return sample(8 + 16 * (row % 2 ? 127 - column : column), 8 + 16 * row,
                  index);
  };
  struct FullCanvas : Canvas {
    double coverage(int, int) const override { return 1; }
  };
  for (const auto &capture : captures) {
    Preset p;
    p.profile       = "krita-5.3.4.1-hard-pixel-candidate-1";
    p.size.value    = 8;
    p.spacing       = 2;
    p.opacity.value = .5;
    FullCanvas canvas;
    Stroke stroke(canvas, p);
    for (int index = 0; index < capture.count; ++index)
      require(stroke.add(point(index)), stroke.error().c_str());
    Sample last = point(capture.count - 1), next = point(capture.count);
    last.x += (next.x - last.x) / 16;
    last.y += (next.y - last.y) / 16;
    last.time += .0625;
    require(stroke.add(last), stroke.error().c_str());
    finishStroke(stroke);
    require(stroke.dabCount() == size_t(capture.count),
            "source outgoing path changed observed deposit count");
    uint64_t hash = 14695981039346656037ull;
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 64; ++x)
        hash = (hash ^ uint64_t(std::lround(canvas.read(x, y).a * 255))) *
               1099511628211ull;
    require(hash == capture.hash,
            "source reverse/corner geometry differs from original path window");
  }
}
void testSourceHalfPixelLookahead() {
  Preset p;
  p.profile       = "krita-5.3.4.1-hard-pixel-candidate-1";
  p.size.value    = 32;
  p.size.minimum  = 0;  // Source pressure has no native minimum-size offset.
  p.size.mappings = {Mapping{}};
  p.spacing       = .1;
  const std::vector<std::vector<double>> partitions = {
      {0, 16}, {0, .8, 16}, {0, 1, 16}, {0,  .5,   1,  1.5,  2,  2.5,  3,  3.5,
                                         4,  4.5,  5,  5.5,  6,  6.5,  7,  7.5,
                                         8,  8.5,  9,  9.5,  10, 10.5, 11, 11.5,
                                         12, 12.5, 13, 13.5, 14, 14.5, 15, 15.5,
                                         16}};
  Canvas original;
  for (size_t index = 0; index < partitions.size(); ++index) {
    Canvas canvas;
    Stroke stroke(canvas, p);
    for (double distance : partitions[index])
      require(stroke.add(sample(32 + distance, 32, distance / 16,
                                .25 + .5 * distance / 16)),
              stroke.error().c_str());
    finishStroke(stroke);
    uint64_t hash = 14695981039346656037ull;
    for (int y = 12; y < 54; ++y)
      for (int x = 12; x < 54; ++x)
        hash = (hash ^ uint64_t(std::lround(canvas.read(x, y).a * 255))) *
               1099511628211ull;
    require(hash == 15336735573899725277ull,
            "half-pixel pressure path differs from original partition capture");
    int x0 = 128, y0 = 128, x1 = 0, y1 = 0;
    for (int y = 0; y < 128; ++y)
      for (int x = 0; x < 128; ++x)
        if (canvas.read(x, y).a > 0) {
          x0 = std::min(x0, x);
          y0 = std::min(y0, y);
          x1 = std::max(x1, x + 1);
          y1 = std::max(y1, y + 1);
        }
    require(x0 == 28 && y0 == 21 && x1 == 57 && y1 == 43,
            "source partition full extent changed outside captured window");
    if (index == 0)
      original = canvas;
    else
      require(same(original, canvas),
              "source linear pressure resampling depends on event partition");
  }
  struct Capture {
    double first, last, spacing;
    int x, width;
    uint64_t hash;
  };
  const Capture captured[] = {
      {0.04, 0.04, 1, 232, 1584, 625310388616192741ull},
      {0.05, 0.05, 1, 232, 1584, 9118570702728184549ull},
      {0.0625, 0.0625, 1, 232, 1584, 2462118175317398245ull},
      {0, 0.05, 1, 232, 1584, 15477144494955960253ull},
      {0.05, 0, 1, 232, 1584, 340152130165620632ull},
      {0.5, 0.5, 1, 232, 1584, 17651779327498978021ull},
      {0.25, 0.75, 1, 232, 1584, 16447721037685110285ull},
      {1, 0.1, 1, 232, 1584, 7401096805165472565ull},
      {0.1, 1, 1, 232, 1584, 17343809668620954918ull},
      {1, 0, 1, 232, 1584, 17550580342204038856ull},
      {0, 1, 1, 232, 1584, 4200416735286428294ull}};
  struct WideCanvas : Canvas {
    WideCanvas() {
      w = 2048;
      pixels.assign(size_t(w) * h, {});
    }
    double coverage(int, int) const override { return 1; }
  };
  for (const auto &capture : captured) {
    p.spacing = capture.spacing;
    WideCanvas canvas;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(256, 64, 0, capture.first)) &&
                stroke.add(sample(1792, 64, 1, capture.last)),
            stroke.error().c_str());
    finishStroke(stroke);
    uint64_t hash = 14695981039346656037ull;
    for (int x = capture.x; x < capture.x + capture.width; ++x)
      hash = (hash ^ uint64_t(canvas.read(x, 63).a > 0)) * 1099511628211ull;
    require(
        hash == capture.hash,
        "source half-pixel pressure spacing differs from original sparse row");
  }
}
void testSourceAbsoluteSpacingFloor() {
  // Original 1536px source traces: rising/falling/constant pressure and
  // three spacings. Extents are captured observations, not runtime output.
  // Sparse full-row hashes remain independently checked above; dense alpha
  // and low-opacity full-region qualification remain source-CI obligations.
  struct Capture {
    double first, last, spacing;
    int left, right;
  };
  const Capture captures[] = {
      {0.0, 1.0, 0.1, 304, 1804},       {0.0, 1.0, 0.25, 304, 1801},
      {0.0, 1.0, 1.0, 305, 1785},       {1.0, 0.0, 0.1, 241, 1743},
      {1.0, 0.0, 0.25, 241, 1743},      {1.0, 0.0, 1.0, 241, 1735},
      {0.1, 1.0, 0.1, 255, 1804},       {0.1, 1.0, 0.25, 255, 1801},
      {0.1, 1.0, 1.0, 255, 1782},       {1.0, 0.1, 0.1, 241, 1792},
      {1.0, 0.1, 0.25, 241, 1793},      {1.0, 0.1, 1.0, 241, 1790},
      {0.25, 0.75, 0.1, 253, 1802},     {0.25, 0.75, 0.25, 253, 1802},
      {0.25, 0.75, 1.0, 253, 1780},     {0.5, 0.5, 0.1, 249, 1798},
      {0.5, 0.5, 0.25, 249, 1795},      {0.5, 0.5, 1.0, 249, 1783},
      {0.04, 0.04, 0.1, 256, 1791},     {0.04, 0.04, 0.25, 256, 1791},
      {0.04, 0.04, 1.0, 256, 1791},     {0.05, 0.05, 0.1, 256, 1792},
      {0.05, 0.05, 0.25, 256, 1792},    {0.05, 0.05, 1.0, 256, 1791},
      {0.0625, 0.0625, 0.1, 256, 1792}, {0.0625, 0.0625, 0.25, 256, 1792},
      {0.0625, 0.0625, 1.0, 256, 1790}, {0.0, 0.05, 0.1, 1216, 1792},
      {0.0, 0.05, 0.25, 1216, 1792},    {0.0, 0.05, 1.0, 1235, 1791},
      {0.05, 0.0, 0.1, 256, 831},       {0.05, 0.0, 0.25, 256, 831},
      {0.05, 0.0, 1.0, 256, 794},
  };
  struct WideCanvas : Canvas {
    WideCanvas() {
      w = 2048;
      pixels.assign(size_t(w) * h, {});
    }
    double coverage(int, int) const override { return 1; }
  };
  for (const auto &capture : captures) {
    Preset p;
    p.profile       = "krita-5.3.4.1-hard-pixel-candidate-1";
    p.size.value    = 32;
    p.size.minimum  = 0;
    p.size.mappings = {Mapping{}};
    p.spacing       = capture.spacing;
    WideCanvas canvas;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(256, 64, 0, capture.first)) &&
                stroke.add(sample(1792, 64, 1, capture.last)),
            stroke.error().c_str());
    finishStroke(stroke);
    int left = canvas.w, right = -1;
    for (int x = 0; x < canvas.w; ++x)
      if (canvas.read(x, 63).a > 0) {
        left  = std::min(left, x);
        right = std::max(right, x);
      }
    require(left == capture.left && right == capture.right,
            "source absolute spacing/available endpoint differs from original "
            "extent");
  }
  // Exact half-pixel availability is a real input sample, not a tolerance.
  // This also exercises retaining the previous endpoint when a subsequent
  // delivery starts exactly at the consumed future sample.
  Preset p;
  p.profile       = "krita-5.3.4.1-hard-pixel-candidate-1";
  p.size.value    = 32;
  p.size.minimum  = 0;
  p.size.mappings = {Mapping{}};
  p.spacing       = .1;
  p.opacity.value = .5;
  Canvas whole, split;
  Stroke a(whole, p), b(split, p);
  require(a.add(sample(32, 32, 0, .05)) && a.add(sample(33, 32, 1, .05)),
          a.error().c_str());
  require(b.add(sample(32, 32, 0, .05)) && b.add(sample(32.499, 32, .499, .05)),
          b.error().c_str());
  require(b.dabCount() == 0,
          "source emitted before future input was available");
  require(b.add(sample(32.5, 32, .5, .05)), b.error().c_str());
  require(b.dabCount() == 1, "source excluded exact future input boundary");
  require(b.add(sample(33, 32, 1, .05)), b.error().c_str());
  require(a.dabCount() == 2 && b.dabCount() == 2,
          "source absolute spacing changed exact-boundary stamp count");
  finishStroke(a);
  finishStroke(b);
  require(same(whole, split),
          "source exact-boundary event split changed pixels");
}
void testSourceSubpixelSpacingFloor() {
  // Original D32, half-opacity, four-pixel Qt traces at all three requested
  // spacings have this same row and six dabs. Preserve source byte tolerance.
  const int expected[] = {0,   0,   0,   0,   0,   192, 240, 248, 252, 252, 252,
                          252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
                          252, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
                          252, 252, 252, 248, 240, 192, 128, 0,   0};
  for (double spacing : {.01, .015625, .02}) {
    Preset p;
    p.profile       = "krita-5.3.4.1-hard-pixel-candidate-1";
    p.size.value    = 32;
    p.size.minimum  = 0;  // Source pressure has no native minimum-size offset.
    p.size.mappings = {Mapping{}};
    p.opacity.value = .5;
    p.spacing       = spacing;
    Canvas canvas;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(32, 32, 0)) && stroke.add(sample(36, 32, 1)),
            stroke.error().c_str());
    finishStroke(stroke);
    require(stroke.dabCount() == 6,
            "source subpixel spacing/terminal rule changed dab count");
    for (size_t x = 0; x < sizeof(expected) / sizeof(*expected); ++x)
      require(std::abs(std::lround(canvas.read(12 + int(x), 32).a * 255) -
                       expected[x]) <= 1,
              "source subpixel spacing differs from captured alpha row");
  }
}
void testSourceTapAndShortMotion() {
  // Exact original Qt press/move/release and press/release-only alpha windows.
  // These are fast authored event traces, not wall-clock latency claims.
  struct Capture {
    bool tap;
    double length, first, last;
    uint64_t hash;
  };
  const Capture captures[] = {
      {false, 0.0, 0.25, 0.75, 6571640615898572149ull},
      {false, 0.0, 0.75, 0.25, 6571640615898572149ull},
      {false, 0.1, 0.25, 0.75, 6571640615898572149ull},
      {false, 0.1, 0.75, 0.25, 6571640615898572149ull},
      {false, 0.4, 0.25, 0.75, 6571640615898572149ull},
      {false, 0.4, 0.75, 0.25, 6571640615898572149ull},
      {false, 0.5, 0.25, 0.75, 7371093129759711509ull},
      {false, 0.5, 0.75, 0.25, 16562168500648688885ull},
      {false, 0.6, 0.25, 0.75, 5117783438066559437ull},
      {false, 0.6, 0.75, 0.25, 16995377932085478973ull},
      {true, 0, 0.25, 0.25, 17528376983619268489ull},
      {true, 0, 0.75, 0.75, 9656716888816939093ull},
  };
  for (const auto &capture : captures) {
    Preset p;
    p.profile       = "krita-5.3.4.1-hard-pixel-candidate-1";
    p.size.value    = 32;
    p.size.minimum  = 0;
    p.size.mappings = {Mapping{}};
    p.spacing       = .1;
    Canvas canvas;
    Stroke stroke(canvas, p);
    require(stroke.add(sample(32, 32, 0, capture.first)),
            stroke.error().c_str());
    if (!capture.tap)
      require(
          stroke.add(sample(32 + capture.length, 32, 1.0 / 240, capture.last)),
          stroke.error().c_str());
    finishStroke(stroke);
    uint64_t hash = 14695981039346656037ull;
    for (int y = 12; y < 54; ++y)
      for (int x = 12; x < 54; ++x)
        hash = (hash ^ uint64_t(std::lround(canvas.read(x, y).a * 255))) *
               1099511628211ull;
    require(hash == capture.hash,
            "source tap/short motion differs from original alpha window");
    require(stroke.dabCount() == size_t(capture.tap || capture.length >= .5),
            "source no-op path finalized as a tap");
  }
}
void testSourceLookaheadLifecycle() {
  Preset p;
  p.profile       = "krita-5.3.4.1-hard-pixel-candidate-1";
  p.size.value    = 32;
  p.size.minimum  = 0;  // Source pressure has no native minimum-size offset.
  p.size.mappings = {Mapping{}};
  Canvas tap, cancelled;
  auto memory = std::make_shared<MemoryBudget>(8 * 1024 * 1024);
  {
    Stroke stroke(tap, p, 8 * 1024 * 1024, memory);
    require(stroke.add(sample(32, 32, 0, .25)) && stroke.dabCount() == 0,
            "source future-dependent tap painted before input was available");
    require(stroke.finish() && stroke.dabCount() == 1,
            "source committed tap did not flush once");
    require(stroke.finish() && stroke.dabCount() == 1,
            "source finish was not idempotent");
  }
  require(memory->used() == 0, "source tap retained buffer after finish");
  {
    Stroke stroke(cancelled, p);
    require(stroke.add(sample(32, 32, 0, .25)), stroke.error().c_str());
    stroke.cancel();
    require(stroke.dabCount() == 0, "cancel flushed a pending source tap");
  }
  for (const auto &pixel : cancelled.pixels)
    require(pixel.a == 0, "cancel modified source surface");
  Canvas refusal;
  refusal.refuse = true;
  {
    Stroke stroke(refusal, p, 8 * 1024 * 1024, memory);
    require(stroke.add(sample(32, 32, 0, .25)), stroke.error().c_str());
    require(!stroke.finish() && !stroke.error().empty(),
            "pending source tap hid finalization failure");
  }
  require(memory->used() == 0, "failed source tap leaked memory");
  {
    auto backing = std::make_shared<Canvas>();
    BufferedSurface surface(backing, memory);
    Stroke stroke(surface, p, 8 * 1024 * 1024, memory);
    require(stroke.add(sample(32, 32, 0, .25)), stroke.error().c_str());
    const size_t obstruction = 8 * 1024 * 1024 - memory->used();
    require(memory->reserve(obstruction),
            "cannot constrain pending-tap cache budget");
    require(
        !stroke.finish() &&
            stroke.error() == "Raster transaction refused the stroke region",
        "pending tap ignored finalization cache budget");
    for (const auto &pixel : backing->pixels)
      require(pixel.a == 0, "failed tap published partial pixels");
    require(memory->used() == obstruction,
            "failed tap retained its path memory");
    memory->release(obstruction);
  }
  {
    Canvas untouched;
    Stroke stroke(untouched, p);
    require(stroke.add(sample(32, 32, 1, .25)), stroke.error().c_str());
    require(!stroke.add(sample(33, 32, 0, .5)) && stroke.dabCount() == 0,
            "source lookahead accepted backwards time");
    require(!stroke.finish(), "invalid source input finalized a tap");
  }
  Canvas dense;
  {
    Stroke stroke(dense, p, 8 * 1024 * 1024, memory);
    require(stroke.add(sample(32, 32, 0, .25)), stroke.error().c_str());
    for (int i = 1; i < 4096; ++i)
      require(stroke.add(sample(32 + i * .00001, 32, i, .25)),
              stroke.error().c_str());
    require(!stroke.add(sample(32 + .04096, 32, 4096, .25)) &&
                stroke.error() == "Source input history limit exceeded" &&
                stroke.dabCount() == 0,
            "unbounded subpixel input history was accepted");
    stroke.cancel();
  }
  require(memory->used() == 0, "source input-history rejection leaked memory");
  for (const auto &pixel : dense.pixels)
    require(pixel.a == 0, "history rejection painted pending data");
}
void testSourceNaturalCubicSize() {
  // Complete original source alpha windows, independent of this evaluator.
  struct Capture {
    bool turning;
    double pressure;
    int x, y, w, h;
    uint64_t hash;
  };
  const Capture captures[] = {
      {true, 0.0, -36, -36, 74, 74, 3428123260092120149ull},
      {true, 0.125, -36, -36, 74, 74, 1679702931769718589ull},
      {true, 0.25, -36, -36, 74, 74, 4812327144733718197ull},
      {true, 0.5, -36, -36, 74, 74, 3476452870747138773ull},
      {true, 0.75, -36, -36, 74, 74, 16804940784029821173ull},
      {true, 0.875, -36, -36, 74, 74, 11913853263255668941ull},
      {true, 1.0, -36, -36, 74, 74, 1786701866695526229ull},
      {false, 0.0, -36, -36, 74, 74, 16804940784029821173ull},
      {false, 0.125, -36, -36, 74, 74, 3187964990055137253ull},
      {false, 0.25, -36, -36, 74, 74, 3428123260092120149ull},
      {false, 0.5, -36, -36, 74, 74, 3476452870747138773ull},
      {false, 0.75, -36, -36, 74, 74, 1786701866695526229ull},
      {false, 0.875, -36, -36, 74, 74, 6334138343680221509ull},
      {false, 1.0, -36, -36, 74, 74, 4812327144733718197ull},
  };
  for (const auto &capture : captures) {
    Preset p;
    p.profile      = "krita-5.3.4.1-hard-pixel-candidate-1";
    p.size.value   = 64;
    p.size.minimum = 0;
    Mapping curve;
    curve.sensor        = Sensor::Pressure;
    curve.interpolation = Interpolation::NaturalCubic;
    curve.curve =
        capture.turning
            ? std::vector<Knot>{{0, .25}, {.25, .875}, {.75, .125}, {1, .75}}
            : std::vector<Knot>{{0, .125}, {.25, .25}, {.75, .75}, {1, .875}};
    p.size.mappings = {curve};
    require(validate(p).empty(), "captured source cubic fixture is invalid");
    Canvas canvas;
    Stroke stroke(canvas, p);
    auto input     = sample(64, 64, 0);
    input.pressure = capture.pressure;
    require(stroke.add(input), stroke.error().c_str());
    sourceFixtureMotion(stroke, input);
    finishStroke(stroke);
    uint64_t hash = 14695981039346656037ull;
    for (int y = 0; y < capture.h; ++y)
      for (int x = 0; x < capture.w; ++x) {
        const auto byte = uint64_t(std::lround(
            canvas.read(64 + capture.x + x, 64 + capture.y + y).a * 255));
        hash            = (hash ^ byte) * 1099511628211ull;
      }
    require(hash == capture.hash,
            "natural cubic size differs from complete source mask");
    p.size.mappings.front().interpolation = Interpolation::Linear;
    require(!validate(p).empty(),
            "multi-knot linear curve admitted as source spline");
  }
}
void testSourceSmallHardCircleAA() {
  // Complete original source masks from integer-position single-dab captures.
  // D5/6 and D11/13/14/15 are held out for full-image source CI comparison.
  struct Capture {
    int diameter;
    std::vector<int> alpha;
  };
  const Capture captures[] = {
      {10, {0,   0,   0,   20,  101, 138, 101, 20,  0,   0,   0,   0,   92,
            201, 255, 255, 255, 201, 92,  0,   0,   70,  216, 255, 255, 255,
            255, 255, 216, 69,  0,   166, 255, 255, 255, 255, 255, 255, 255,
            165, 0,   221, 255, 255, 255, 255, 255, 255, 255, 221, 0,   221,
            255, 255, 255, 255, 255, 255, 255, 221, 0,   166, 255, 255, 255,
            255, 255, 255, 255, 165, 0,   70,  216, 255, 255, 255, 255, 255,
            216, 69,  0,   0,   92,  201, 255, 255, 255, 201, 92,  0,   0,
            0,   0,   20,  101, 138, 101, 20,  0,   0}},
      {12,
       {0,   0,   0,   0,   37,  104, 136, 104, 37,  0,   0,   0,   0,   0,
        0,   130, 218, 255, 255, 255, 217, 130, 0,   0,   0,   0,   146, 252,
        255, 255, 255, 255, 255, 252, 146, 0,   0,   95,  242, 255, 255, 255,
        255, 255, 255, 255, 241, 95,  0,   175, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 174, 0,   224, 255, 255, 255, 255, 255, 255, 255, 255,
        255, 224, 0,   224, 255, 255, 255, 255, 255, 255, 255, 255, 255, 224,
        0,   175, 255, 255, 255, 255, 255, 255, 255, 255, 255, 174, 0,   95,
        242, 255, 255, 255, 255, 255, 255, 255, 241, 95,  0,   0,   146, 252,
        255, 255, 255, 255, 255, 252, 146, 0,   0,   0,   0,   130, 218, 255,
        255, 255, 217, 130, 0,   0,   0,   0,   0,   0,   37,  104, 136, 104,
        37,  0,   0,   0}},
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
    finishStroke(stroke);
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
  for (double size : {4.5, 10.5, 12.5}) {
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
      finishStroke(first);
      require(equal(build.read(32, 32), expected),
              "source blend analytic alpha case");
      p.wash       = true;
      p.flow.value = .25;
      Canvas wash;
      for (auto &pixel : wash.pixels) pixel = base;
      Stroke accumulated(wash, p);
      require(accumulated.add(sample(32, 32, 0)), accumulated.error().c_str());
      require(accumulated.add(sample(35, 32, 1)), accumulated.error().c_str());
      require(accumulated.dabCount() == 2,
              "wash blend fixture must contain two interior source dabs");
      // The second dab at x34 needs its half-pixel future sample available.
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
      finishStroke(zero);
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
void testSourceWetOutgoingGeometry() {
  // Candidate directional transport uses the same physical future path as
  // hard Pixel. These original regressions enforce geometry/partitioning;
  // source wet color windows and full drift gates qualify source agreement.
  for (const std::string model :
       {"legacy-smear", "new-smear", "legacy-dulling", "new-dulling"}) {
    Preset p;
    p.profile = "krita-5.3.4.1-" + model + "-candidate-1";
    p.wet = model.find("dulling") == std::string::npos ? Wet::Smear : Wet::Dulling;
    p.size.value = 8;
    p.spacing = 2;
    p.tip.antialiasEdges = false;
    p.smudgeRadius = 0;
    p.pickup = .5;
    p.colorRate = 1;
    p.color = {1, 0, 0, 1};
    auto render = [&](bool dense) {
      Canvas canvas;
      for (auto &pixel : canvas.pixels) pixel = {0, 0, 1, 1};
      auto budget = std::make_shared<MemoryBudget>(1024 * 1024);
      Stroke stroke(canvas, p, 1024 * 1024, budget);
      require(stroke.add(sample(64, 32, 0)), stroke.error().c_str());
      if (dense) {
        for (int step = 1; step <= 196; ++step)
          require(stroke.add(sample(64 - step * .25, 32, step / 240.0)),
                  stroke.error().c_str());
      } else {
        require(stroke.add(sample(15, 32, 1)), stroke.error().c_str());
      }
      require(stroke.dabCount() == 4, "wet future path changed nominal dab count");
      finishStroke(stroke);
      require(budget->used() == 0, "wet future input history leaked budget");
      return canvas;
    };
    const Canvas coarse = render(false), dense = render(true);
    require(same(coarse, dense), "wet path geometry depends on event partitioning");
    const int left = p.wet == Wet::Dulling ? 44 : 43;
    for (int x = 41; x <= 53; ++x)
      require((coarse.read(x, 31).r > 0) == (x >= left && x <= 50),
              "reverse wet footprint retained a fixed forward offset");
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
    Pixel read(int x, int y) const override {
      require(x >= 0 && y >= 0 && x < side && y < side,
              "source wet read escaped the finite host");
      ++reads;
      return {0, 0, 0, 1};
    }
    void write(int x, int y, const Pixel &) override {
      require(x >= 0 && y >= 0 && x < side && y < side,
              "source wet write escaped the finite host");
      ++writes;
    }
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
    // Every model charges its full moving snapshot, including off-canvas
    // pixels, before any host operation. Large dabs must not bypass the work
    // budget by clipping to a smaller host. Nonzero-radius cases additionally
    // cover the real area-scaled physical sample count.
    GuardSurface guard(p.wet == Wet::Smear ? 8192 : 4096);
    auto budget = std::make_shared<MemoryBudget>(1024u * 1024u * 1024u);
    for (double radius : {0.0, p.smudgeRadius}) {
      p.smudgeRadius = radius;
      Stroke denied(guard, p, 1024u * 1024u * 1024u, budget);
      const size_t settingsBytes = budget->used();
      require(!denied.add(sample(guard.side / 2, guard.side / 2, 0)) &&
                  denied.error() == "Stroke input pixel work limit exceeded",
              "source wet bypassed the per-add work budget");
      require(guard.prepared == 0 && guard.reads == 0 && guard.writes == 0 &&
                  budget->used() == settingsBytes,
              "source wet work failure touched the host or allocated snapshot");
      require(!denied.add(sample(0, 0, 1)), "failed source wet stroke resumed");
      denied.cancel();
      require(budget->used() == 0, "failed source wet stroke leaked budget");
    }
    // The full D32 moving field is 34x34 for Smear and 35x34 for
    // Dulling's half-pixel x lattice. At a 1x1 host corner it also needs
    // four sparse 32x32 outside tiles, each with its charged node allowance.
    // Host clipping must not hide either allocation from the budget.
    p.size.value   = 32;
    p.smudgeRadius = p.wet == Wet::Smear ? 0 : .5;
    GuardSurface corner(1);
    Stroke clipped(corner, p, 1024 * 1024, budget);
    const size_t settingsBytes = budget->used();
    require(clipped.add(sample(0, 0, 0)), clipped.error().c_str());
    const size_t snapshotBytes =
        size_t(p.wet == Wet::Smear ? 34 : 35) * 34 * sizeof(Pixel);
    const size_t outsideTileBytes = 32 * 32 * sizeof(Pixel) + 128;
    require(
        corner.prepared == 1 && budget->used() == settingsBytes +
                                                      snapshotBytes +
                                                      4 * outsideTileBytes,
        "source moving field or sparse outside tiles were not fully charged");
    finishStroke(clipped);
    require(budget->used() == 0,
            "source corner field or outside tiles leaked budget");
    for (bool denySnapshot : {false, true}) {
      const size_t cap = settingsBytes + 4 * outsideTileBytes +
                         (denySnapshot ? snapshotBytes : 0) - 1;
      auto limited = std::make_shared<MemoryBudget>(cap);
      GuardSurface untouched(1);
      Stroke denied(untouched, p, cap, limited);
      require(
          !denied.add(sample(0, 0, 0)) &&
              denied.error() ==
                  (denySnapshot ? "Source wet snapshot memory limit exceeded"
                                : "Source off-canvas memory limit exceeded") &&
              untouched.prepared == 0 && untouched.reads == 0 &&
              untouched.writes == 0,
          "source corner memory cap was not enforced before host access");
      require(limited->used() ==
                  settingsBytes + (denySnapshot ? 4 * outsideTileBytes : 0),
              "failed source allocation escaped accounting");
      denied.cancel();
      require(limited->used() == 0,
              "denied source corner allocation leaked budget");
    }
    GuardSurface outside(1);
    Stroke offCanvas(outside, p, 1024 * 1024, budget);
    const size_t outsideSettings = budget->used();
    require(
        offCanvas.add(sample(10000, 10000, 0)) && outside.prepared == 0 &&
            outside.reads == 0 && outside.writes == 0 &&
            budget->used() ==
                outsideSettings + snapshotBytes + 9 * outsideTileBytes,
        "fully outside wet field touched host or escaped memory accounting");
    finishStroke(offCanvas);
    require(budget->used() == 0, "fully outside wet field leaked budget");
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
  finishStroke(stroke);
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
  finishStroke(stroke);
  require(std::abs(plain.read(64, 56).a * 255 - 255) < 1e-9 &&
              std::abs(plain.read(63, 56).a * 255 - 181) <= 1.000001 &&
              std::abs(plain.read(49, 64).a * 255 - 21) <= 1.000001,
          "non-AA rectangle source axis/fade behavior changed");
  p.tip.antialiasEdges = true;
  Canvas aa;
  Stroke aaStroke(aa, p);
  require(aaStroke.add(sample(64, 64, 0)), aaStroke.error().c_str());
  finishStroke(aaStroke);
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
    finishStroke(stroke);
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
  finishStroke(stroke);
  require(budget->used() == 0, "independent mask retained after finish");
  Stroke partitioned(split, p);
  for (int i = 0; i <= 16; ++i)
    require(partitioned.add(sample(16 + 2 * i, 64, i / 16.0)),
            partitioned.error().c_str());
  finishStroke(partitioned);
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
    finishStroke(stroke);
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
#include "rasterbrushnativewet_spatial_test.h"
#include "rasterbrushscatter_test.h"
}  // namespace
int main() {
  struct Case {
    const char *name;
    void (*run)();
  };
  const Case cases[] = {
      {"testNativeEraseCoverage", testNativeEraseCoverage},
      {"testSourceMaskedWash", testSourceMaskedWash},
      {"testSourceFixedBitmapCaptures", testSourceFixedBitmapCaptures},
      {"testSourceMirrorThreshold", testSourceMirrorThreshold},
      {"testSourceBitmapWorkBudget", testSourceBitmapWorkBudget},
      {"testSourceRandomGihCacheMechanism", testSourceRandomGihCacheMechanism},
      {"testSourceRandomGihRenderAndNativeInvariance", testSourceRandomGihRenderAndNativeInvariance},
      {"testSourceMaskedEventBudget", testSourceMaskedEventBudget},
      {"testFloatTransaction", testFloatTransaction},
      {"testSnapshotAndPrepareBudgets", testSnapshotAndPrepareBudgets},
      {"testComposition", testComposition},
      {"testAreaCoverage", testAreaCoverage},
      {"testDeterminismAndMasks", testDeterminismAndMasks},
      {"testDisabledSecondaryAndExtremeTransforms",
       testDisabledSecondaryAndExtremeTransforms},
      {"testNativeTextureCoverage", testNativeTextureCoverage},
      {"testWashAndInput", testWashAndInput},
      {"testResourcesAndWet", testResourcesAndWet},
      {"testWetPressure", testWetPressure},
      {"testWetPickupGeometry", testWetPickupGeometry},
      {"testCountFadeAndDirection", testCountFadeAndDirection},
      {"testSecondaryCountAndMirror", testSecondaryCountAndMirror},
      {"testIndependentSecondarySpacing", testIndependentSecondarySpacing},
      {"testIndependentSecondaryDynamics", testIndependentSecondaryDynamics},
      {"testNativeSpacingAndWorkLimits", testNativeSpacingAndWorkLimits},
      {"testKritaHardPixelCandidate", testKritaHardPixelCandidate},
      {"testKritaFixedHsvCandidate", testKritaFixedHsvCandidate},
      {"testKritaPressureHsvCandidate", testKritaPressureHsvCandidate},
      {"testKritaJointHsvCandidate", testKritaJointHsvCandidate},
      {"testKritaMixedPressureHsvCandidate",
       testKritaMixedPressureHsvCandidate},
      {"testStationaryAndBatching", testStationaryAndBatching},
      {"testNativePressureDeliveryBatches", testNativePressureDeliveryBatches},
      {"testColorDynamics", testColorDynamics},
      {"testCubicCurves", testCubicCurves},
      {"testAdaptiveSourcePickupComponent", testAdaptiveSourcePickupComponent},
      {"testSourcePickupLabPreparation", testSourcePickupLabPreparation},
      {"testSourcePickupLabCalibration", testSourcePickupLabCalibration},
      {"testSourcePickupObservedConvergence", testSourcePickupObservedConvergence},
      {"testSourcePickupExactQuantizedMean", testSourcePickupExactQuantizedMean},
      {"testSourcePickupPremultipliedByteStorage", testSourcePickupPremultipliedByteStorage},
      {"testSurfacePickupStorageContract", testSurfacePickupStorageContract},
      {"testSourcePickupRetainedFloatingMean", testSourcePickupRetainedFloatingMean},
      {"testPeriodicTimeMapping", testPeriodicTimeMapping},
      {"testPeriodicDistanceMapping", testPeriodicDistanceMapping},
      {"testSourceAirbrushDispatch", testSourceAirbrushDispatch},
      {"testSourceGeneratedMasks", testSourceGeneratedMasks},
      {"testSourceScatterProcess", testSourceScatterProcess},
      {"testSourceDefaultCircle", testSourceDefaultCircle},
      {"testSourceDullingRadiusImpulses", testSourceDullingRadiusImpulses},
      {"testSourceWetPressureColorAttenuation",
       testSourceWetPressureColorAttenuation},
      {"testSourceWashLattice", testSourceWashLattice},
      {"testSourceSmallCircle", testSourceSmallCircle},
      {"testSourceHardCircleAA", testSourceHardCircleAA},
      {"testSourceWetWorkBounds", testSourceWetWorkBounds},
      {"testSourceWetOutgoingGeometry", testSourceWetOutgoingGeometry},
      {"testSourcePixelBlendCandidates", testSourcePixelBlendCandidates},
      {"testSourceSmallHardCircleAA", testSourceSmallHardCircleAA},
      {"testSourceNaturalCubicSize", testSourceNaturalCubicSize},
      {"testSourceSmearRasterOrigin", testSourceSmearRasterOrigin},
      {"testSourceSmearRecursiveGeometry", testSourceSmearRecursiveGeometry},
      {"testSourcePathDirectionGeometry", testSourcePathDirectionGeometry},
      {"testSourceHalfPixelLookahead", testSourceHalfPixelLookahead},
      {"testSourceTapAndShortMotion", testSourceTapAndShortMotion},
      {"testSourceLookaheadLifecycle", testSourceLookaheadLifecycle},
      {"testSourceAbsoluteSpacingFloor", testSourceAbsoluteSpacingFloor},
      {"testSourceSubpixelSpacingFloor", testSourceSubpixelSpacingFloor},
      {"testSourceBarrelGeometry", testSourceBarrelGeometry},
      {"testSourceSymmetricTiltRotation", testSourceSymmetricTiltRotation},
      {"testSourcePixelPressureTransfer", testSourcePixelPressureTransfer},
      {"testSourceTransferCurves", testSourceTransferCurves},
      {"testSourceRectangle", testSourceRectangle},
      {"testSourceWetInitialization", testSourceWetInitialization},
      {"testSourceWetBoundaryMatrix", testSourceWetBoundaryMatrix},
      {"testSourceWetLengthToggleMeaning", testSourceWetLengthToggleMeaning},
      {"testSourceWetOffCanvasState", testSourceWetOffCanvasState},
      {"testSourceTextureMasks", testSourceTextureMasks},
      {"testSourceWetHsvReferenceInteraction",
       testSourceWetHsvReferenceInteraction},
      {"testSourceWetJointForegroundComposition",
       testSourceWetJointForegroundComposition},
      {"testSourceWetForegroundTransformPlumbing",
       testSourceWetForegroundTransformPlumbing},
      {"testSourceWetOpacityTransport", testSourceWetOpacityTransport},
      {"testSourceWetDisabledControls", testSourceWetDisabledControls},
      {"testLongWetTrace", testLongWetTrace},
      {"testNativeWetAnalyticalDrift", testNativeWetAnalyticalDrift},
      {"testNativeWetSpatialDullingOracle", testNativeWetSpatialDullingOracle},
      {"testNativeWetSpatialSmearOracle", testNativeWetSpatialSmearOracle},
      {"testNativeWetZeroWeightPickup", testNativeWetZeroWeightPickup},
      {"testNativeWetNormalAlphaLockOracle", testNativeWetNormalAlphaLockOracle},
  };
  int failures = 0;
  for (const auto &test : cases) {
    try {
      test.run();
      std::cout << "PASS " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
    } catch (...) {
      ++failures;
      std::cerr << "FAIL " << test.name << ": unknown exception\n";
    }
  }
  if (failures) {
    std::cerr << failures << " native raster engine cases failed\n";
    return 1;
  }
  std::cout << "Native raster engine regression passed\n";
  return 0;
}
