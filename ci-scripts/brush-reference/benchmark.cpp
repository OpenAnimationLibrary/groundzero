#include "toonz/rasterbrushengine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using namespace RasterBrush;
namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point begin, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}
void quoted(std::ostream &out, const std::string &value) {
  static const char hex[] = "0123456789abcdef";
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << char(c);
    else if (c < 32) out << "\\u00" << hex[c >> 4] << hex[c & 15];
    else out << char(c);
  }
  out << '"';
}
struct Canvas : Surface {
  int side;
  std::vector<Pixel> pixels;
  explicit Canvas(int n) : side(n), pixels(size_t(n) * n) {
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x)
        pixels[size_t(y) * n + x] =
            (x / 128) % 2 ? Pixel{.2, .4, .1, .5} : Pixel{.1, .2, .8, 1};
  }
  int width() const override { return side; }
  int height() const override { return side; }
  bool prepare(int, int, int, int) override { return true; }
  Pixel read(int x, int y) const override {
    return pixels[size_t(y) * side + x];
  }
  void write(int x, int y, const Pixel &p) override {
    pixels[size_t(y) * side + x] = p;
  }
};
struct Measurement {
  std::string name, profile, setupMode, error, failurePhase;
  int side = 0, rate = 0, seconds = 0, processOrdinal = 0;
  bool firstProfileUse = false, definitionReused = false, expandedWorkload = false;
  bool validated = false, constructorAttempted = false, constructorSucceeded = false;
  bool finishAttempted = false, finishSucceeded = false, resourceReleaseChecked = false;
  bool constructorAllocationObserved = false, finishAllocationObserved = false;
  bool beforeFinishDabsObserved = false, afterFinishDabsObserved = false;
  bool cleanupDabsObserved = false;
  bool complete = false, eventPassed = false, addThroughputPassed = false;
  bool lifecyclePassed = false, passed = false;
  size_t events = 0, attempted = 0, dabsBefore = 0, dabsAfter = 0, peak = 0;
  size_t dabsBeforeCleanup = 0;
  size_t afterConstructor = 0, afterFinish = 0, afterCleanup = 0;
  size_t resourceObjects = 0, resourceBytes = 0, curveKnots = 0;
  std::uint64_t seed = 0;
  double sizeValue = 0, spacing = 0, smudgeRadius = 0;
  double rasterSetup = 0, surfaceSetup = 0, construction = 0, firstAdd = 0;
  double addElapsed = 0, finish = 0, cleanup = 0, lifecycle = 0;
  double p95 = 0, p99 = 0, maximum = 0;
  double p95Limit() const { return side > 2048 ? 33 : 16; }
  double p99Limit() const { return side > 2048 ? 66 : 33; }
};
void inventory(const Preset &p, Measurement &m) {
  std::set<const Image *> images;
  auto image = [&](const std::shared_ptr<const Image> &value) {
    if (value && images.insert(value.get()).second) {
      ++m.resourceObjects;
      m.resourceBytes += value->pixels.size() * sizeof(Pixel);
    }
  };
  for (const auto &value : p.tip.images) image(value);
  for (const auto &value : p.secondary.tip.images) image(value);
  image(p.texture.image);
  for (const auto *control : {&p.size, &p.aspect, &p.angle, &p.opacity, &p.flow,
         &p.scatter, &p.countControl, &p.backgroundMix, &p.hue, &p.saturation,
         &p.brightness, &p.pickupControl, &p.colorRateControl, &p.radiusControl,
         &p.secondary.size, &p.secondary.angle, &p.secondary.opacity,
         &p.sourceMirror.control, &p.sourceHsvV2[0].control,
         &p.sourceHsvV2[1].control, &p.sourceHsvV2[2].control})
    for (const auto &mapping : control->mappings) m.curveKnots += mapping.curve.size();
  if (p.sourceGeneratedV1) m.curveKnots += p.sourceGeneratedV1->softnessCurve.size();
}
Measurement measure(const char *name, int side, int rate, const Preset &p,
                    int seconds, bool repeated, bool expanded, int ordinal,
                    bool firstProfileUse) {
  Measurement m;
  m.name = name; m.profile = p.profile; m.side = side; m.rate = rate; m.seconds = seconds;
  m.processOrdinal = ordinal; m.firstProfileUse = firstProfileUse;
  m.definitionReused = repeated; m.expandedWorkload = expanded; m.seed = p.seed;
  m.sizeValue = p.size.value; m.spacing = p.spacing; m.smudgeRadius = p.smudgeRadius;
  m.setupMode = repeated ? "repeat-workload-same-preset-and-resource-objects"
                         : "first-use-of-workload-in-process";
  inventory(p, m);
  std::shared_ptr<Canvas> canvas;
  std::shared_ptr<MemoryBudget> budget;
  std::unique_ptr<BufferedSurface> surface;
  std::unique_ptr<Stroke> stroke;
  std::vector<double> latency;
  Clock::time_point lifecycleStart{};
  bool lifecycleStarted = false;
  auto checkpoint = [&] { if (budget) m.peak = std::max(m.peak, budget->used()); };
  try {
    m.failurePhase = "preset-validation";
    const auto invalid = validate(p);
    if (!invalid.empty()) throw std::runtime_error(invalid);
    m.validated = true;
    m.failurePhase = "latency-storage-setup";
    latency.reserve(size_t(seconds * rate + 1));
    m.failurePhase = "backing-raster-setup";
    auto start = Clock::now();
    canvas = std::make_shared<Canvas>(side);
    budget = std::make_shared<MemoryBudget>(256u * 1024u * 1024u);
    m.rasterSetup = milliseconds(start, Clock::now());
    m.failurePhase = "buffered-surface-constructor";
    start = Clock::now();
    surface = std::make_unique<BufferedSurface>(canvas, budget);
    m.surfaceSetup = milliseconds(start, Clock::now());
    m.failurePhase = "stroke-constructor";
    m.constructorAttempted = true;
    lifecycleStart = start = Clock::now(); lifecycleStarted = true;
    stroke = std::make_unique<Stroke>(*surface, p, 256u * 1024u * 1024u, budget);
    m.construction = milliseconds(start, Clock::now());
    checkpoint(); m.afterConstructor = budget->used(); m.constructorAllocationObserved = true;
    if (!stroke->error().empty()) throw std::runtime_error(stroke->error());
    m.constructorSucceeded = true;
    m.failurePhase = "stroke-add";
    start = Clock::now();
    for (int i = 0; i <= seconds * rate; ++i) {
      Sample s;
      s.time = i / double(rate);
      s.x = side * (.5 + .35 * std::sin(s.time * .4));
      s.y = side * (.5 + .2 * std::sin(s.time * .7));
      s.pressure = .5 + .5 * std::sin(s.time * .3) * std::sin(s.time * .3);
      s.hasPressure = true;
      ++m.attempted;
      const auto event = Clock::now();
      if (!stroke->add(s)) throw std::runtime_error(stroke->error());
      latency.push_back(milliseconds(event, Clock::now()));
      ++m.events;
      checkpoint();
    }
    // Preserve the original add-loop wall time, including loop/bookkeeping.
    m.addElapsed = milliseconds(start, Clock::now());
    m.firstAdd = latency.front();
    m.dabsBefore = stroke->dabCount(); m.beforeFinishDabsObserved = true;
    m.failurePhase = "stroke-finish";
    m.finishAttempted = true;
    start = Clock::now();
    const bool finished = stroke->finish();
    m.finish = milliseconds(start, Clock::now());
    m.dabsAfter = stroke->dabCount(); m.afterFinishDabsObserved = true;
    if (!finished) throw std::runtime_error(stroke->error());
    m.finishSucceeded = true;
    checkpoint(); m.afterFinish = budget->used(); m.finishAllocationObserved = true;
    m.complete = true;
  } catch (const std::exception &e) {
    m.error = e.what();
  } catch (...) {
    m.error = "Unknown benchmark workload failure";
  }
  // Read actual progress while the stroke is still alive, including after
  // a failed add or a finish that returned false/threw. An unattempted finish
  // has no pre/post-finish observation and must never acquire a numeric zero.
  if (stroke) {
    m.dabsBeforeCleanup = stroke->dabCount(); m.cleanupDabsObserved = true;
    if (m.finishAttempted && !m.afterFinishDabsObserved) {
      m.dabsAfter = m.dabsBeforeCleanup; m.afterFinishDabsObserved = true;
    }
  }
  const auto cleanupStart = Clock::now();
  stroke.reset();
  surface.reset();
  m.cleanup = milliseconds(cleanupStart, Clock::now());
  if (lifecycleStarted) m.lifecycle = milliseconds(lifecycleStart, Clock::now());
  if (budget) {
    m.resourceReleaseChecked = true;
    m.afterCleanup = budget->used();
    if (m.afterCleanup != 0) {
      m.complete = false; m.failurePhase = "allocation-release";
      m.error = "Active stroke allocations did not return to zero";
    }
  }
  if (m.complete) {
    std::sort(latency.begin(), latency.end());
    auto percentile = [&](double q) { return latency[size_t(std::ceil(q * latency.size())) - 1]; };
    m.p95 = percentile(.95); m.p99 = percentile(.99); m.maximum = latency.back();
    m.eventPassed = m.p95 <= m.p95Limit() && m.p99 <= m.p99Limit();
    m.addThroughputPassed = m.addElapsed <= seconds * 1000.;
    // Additional lifecycle accounting includes construction, finish and cleanup;
    // the original add-only percentile and throughput gates remain mandatory.
    m.lifecyclePassed = m.lifecycle <= seconds * 1000.;
    m.passed = m.eventPassed && m.addThroughputPassed && m.lifecyclePassed;
    if (m.passed) m.failurePhase.clear();
    else {
      m.failurePhase = "cpu-budget";
      m.error = "CPU event budget or complete lifecycle real-time throughput exceeded";
    }
  }
  return m;
}
void report(std::ostream &out, const Measurement &m) {
  auto flag = [&](bool value) { out << (value ? "true" : "false"); };
  auto duration = [&](const char *key, double value) {
    out << ','; quoted(out, key); out << ':';
    if (m.complete) out << value; else out << "null";
  };
  out << '{'; quoted(out, "name"); out << ':'; quoted(out, m.name);
  out << ",\"profile\":"; quoted(out, m.profile);
  out << ",\"status\":"; quoted(out, m.complete ? "complete" : "failed");
  out << ",\"failurePhase\":"; quoted(out, m.failurePhase);
  out << ",\"error\":"; quoted(out, m.error);
  out << ",\"setupMode\":"; quoted(out, m.setupMode);
  out << ",\"processStrokeOrdinal\":" << m.processOrdinal << ",\"firstProfileUseInProcess\":"; flag(m.firstProfileUse);
  out << ",\"presetDefinitionReused\":"; flag(m.definitionReused);
  out << ",\"expandedLifecycleWorkload\":"; flag(m.expandedWorkload);
  out << ",\"machineColdCacheClaimed\":false,\"unmeasuredWarmupStrokes\":0"
         ",\"rasterState\":\"fresh backing and BufferedSurface per measured stroke\""
         ",\"resourceOrigin\":\"original in-memory benchmark definitions\""
         ",\"presetResourceCreationTimed\":false,\"resolvedSeed\":"; quoted(out, std::to_string(m.seed));
  out << ",\"canvasSide\":" << m.side << ",\"inputHz\":" << m.rate
      << ",\"traceSeconds\":" << m.seconds << ",\"plannedEvents\":" << m.seconds * m.rate + 1
      << ",\"sizeValue\":" << m.sizeValue << ",\"spacing\":" << m.spacing
      << ",\"smudgeRadius\":" << m.smudgeRadius << ",\"enginePixelBytes\":" << sizeof(Pixel)
      << ",\"attemptedEvents\":" << m.attempted << ",\"events\":" << m.events;
  auto dabCount = [&](const char *key, size_t value, bool observed) {
    out << ','; quoted(out, key); out << ':';
    if (observed) out << value; else out << "null";
  };
  out << ",\"beforeFinishDabCheckpointRecorded\":"; flag(m.beforeFinishDabsObserved);
  out << ",\"afterFinishDabCheckpointRecorded\":"; flag(m.afterFinishDabsObserved);
  out << ",\"cleanupDabCheckpointRecorded\":"; flag(m.cleanupDabsObserved);
  // Preserve the original successful-record alias: dabs means before finish.
  dabCount("dabs", m.dabsBefore, m.beforeFinishDabsObserved);
  dabCount("dabsBeforeFinish", m.dabsBefore, m.beforeFinishDabsObserved);
  dabCount("dabsAfterFinish", m.dabsAfter, m.afterFinishDabsObserved);
  dabCount("dabsBeforeCleanup", m.dabsBeforeCleanup, m.cleanupDabsObserved);
  out << ",\"presetValidated\":"; flag(m.validated);
  out << ",\"constructorAttempted\":"; flag(m.constructorAttempted);
  out << ",\"constructorSucceeded\":"; flag(m.constructorSucceeded);
  out << ",\"finishAttempted\":"; flag(m.finishAttempted);
  out << ",\"finishSucceeded\":"; flag(m.finishSucceeded);
  duration("backingRasterSetupMilliseconds", m.rasterSetup);
  duration("bufferedSurfaceSetupMilliseconds", m.surfaceSetup);
  duration("strokeConstructorMilliseconds", m.construction);
  duration("firstAddMilliseconds", m.firstAdd);
  duration("renderMilliseconds", m.addElapsed);
  duration("eventP95Milliseconds", m.p95);
  duration("eventP99Milliseconds", m.p99);
  duration("eventMaxMilliseconds", m.maximum);
  duration("finishMilliseconds", m.finish);
  duration("cleanupMilliseconds", m.cleanup);
  duration("strokeLifecycleMilliseconds", m.lifecycle);
  out << ",\"peakTrackedStrokeBytes\":" << m.peak
      << ",\"peakTrackedStrokeBytesIsSampled\":true"
         ",\"trackedAllocationSampling\":\"constructor, successful adds, finish, cleanup; transient peaks between checkpoints not observed\""
      << ",\"constructorAllocationCheckpointRecorded\":"; flag(m.constructorAllocationObserved);
  out << ",\"trackedBytesAfterConstructor\":";
  if (m.constructorAllocationObserved) out << m.afterConstructor; else out << "null";
  out << ",\"finishAllocationCheckpointRecorded\":"; flag(m.finishAllocationObserved);
  out << ",\"trackedBytesAfterFinish\":";
  if (m.finishAllocationObserved) out << m.afterFinish; else out << "null";
  out << ",\"backingRasterBytes\":" << size_t(m.side) * m.side * sizeof(Pixel)
      << ",\"inputResourceObjects\":" << m.resourceObjects
      << ",\"inputResourcePixelBytes\":" << m.resourceBytes
      << ",\"presetCurveKnots\":" << m.curveKnots
      << ",\"resourceReleaseChecked\":"; flag(m.resourceReleaseChecked);
  out << ",\"releasedStrokeBytes\":";
  if (m.resourceReleaseChecked) out << m.afterCleanup; else out << "null";
  out << ",\"cpuP95LimitMilliseconds\":" << m.p95Limit()
      << ",\"cpuP99LimitMilliseconds\":" << m.p99Limit()
      << ",\"traceThroughputLimitMilliseconds\":" << m.seconds * 1000.
      << ",\"eventBudgetPassed\":"; flag(m.eventPassed);
  out << ",\"addThroughputPassed\":"; flag(m.addThroughputPassed);
  out << ",\"lifecycleThroughputPassed\":"; flag(m.lifecyclePassed);
  out << ",\"cpuBudgetPassed\":"; flag(m.passed);
  out << ",\"sourceReferenceVerified\":false,\"displayLatencyMeasured\":false} ";
}
Preset generated(bool soft) {
  Preset p;
  p.profile = "krita-5.3.4.1-generated-mask-candidate-1";
  p.size.value = 64; p.spacing = 1; p.tip.antialiasEdges = true;
  p.tip.shape = soft ? Shape::Round : Shape::Square;
  SourceGeneratedDefinitionV1 definition;
  definition.generator = soft ? SourceGeneratedGenerator::Soft : SourceGeneratedGenerator::Gaussian;
  definition.shape = soft ? SourceGeneratedShape::Circle : SourceGeneratedShape::Rectangle;
  definition.diameter = 64; definition.ratio = 1;
  definition.horizontalFade = .25; definition.verticalFade = .75; definition.antialiasEdges = true;
  if (soft) for (int i = 0; i <= 64; ++i) {
    const double x = i / 64.;
    definition.softnessCurve.push_back({x, std::sqrt(x)});
  }
  p.sourceGeneratedV1 = std::move(definition);
  return p;
}
Preset scatter() {
  Preset p;
  p.profile = "krita-5.3.4.1-scatter-candidate-1";
  p.size.value = 8; p.spacing = 4;
  p.sourceScatterV1 = SourceScatterV1{1, true, true};
  return p;
}
Preset randomGih() {
  Preset p;
  p.profile = "krita-5.3.4.1-random-bitmap-candidate-1";
  p.size.value = 8; p.spacing = 4; p.tip.shape = Shape::Bitmap; p.tip.randomSequence = true;
  for (int frame = 0; frame < 3; ++frame) {
    auto image = std::make_shared<Image>();
    image->width = image->height = 8; image->color = true; image->pixels.resize(64);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
      if ((x >= 2 && x < 6) || (y >= 2 && y < 6))
        image->pixels[size_t(y) * 8 + x] = {frame == 0 ? 1. : 0., frame == 1 ? 1. : 0., frame == 2 ? 1. : 0., 1};
    p.tip.images.push_back(image);
  }
  return p;
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 2) throw std::runtime_error("Expected benchmark JSON destination");
    std::ofstream out(argv[1]);
    if (!out) throw std::runtime_error("Cannot create benchmark report");
    out << std::setprecision(12)
        << "{\"schema\":2,\"scope\":\"engine CPU stroke lifecycle and event-to-surface only; hosted runner; no GUI/display/tablet latency claim\","
           "\"displayLatencyMeasured\":false,\"sourceReferenceVerified\":false,\"records\":[";
    bool passed = true, first = true;
    int ordinal = 0, failed = 0;
    std::set<std::string> profiles;
    auto run = [&](const char *name, int side, int rate, const Preset &preset,
                   int seconds = 60, bool repeated = false, bool expanded = false) {
      auto result = measure(name, side, rate, preset, seconds, repeated, expanded,
                            ++ordinal, profiles.insert(preset.profile).second);
      if (!first) out << ',';
      first = false; report(out, result);
      if (!result.passed) {
        ++failed; passed = false;
        std::cerr << "::error::Brush CPU workload " << name << " failed at "
                  << result.failurePhase << ": " << result.error << '\n';
      }
    };
    // Original workloads, settings, event traces and add-only limits are retained.
    Preset p;
    p.size.value = 64; p.color = {.8, .1, .2, 1};
    p.size.mappings.push_back({Sensor::Pressure, {{0, .2}, {1, 1}}, 1});
    run("standard-dry", 2048, 240, p);
    p.tip.antialiasEdges = true;
    run("standard-dry-antialias", 2048, 240, p);
    p.tip.antialiasEdges = false; p.size.value = 256;
    auto texture = std::make_shared<Image>();
    texture->width = texture->height = 2;
    texture->pixels = {{0, 0, 0, .25}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, .5}};
    p.texture.image = texture; p.texture.scale = 4;
    p.secondary.enabled = true; p.secondary.size.value = .8;
    run("heavy-texture-dual", 4096, 120, p);
    p.secondary.enabled = false; p.wet = Wet::Smear;
    run("heavy-smear", 4096, 120, p);
    Preset source;
    source.profile = "krita-5.3.4.1-hard-pixel-candidate-1";
    source.size.value = 64; source.wash = true; source.opacity.value = .5; source.flow.value = .5;
    run("source-candidate-hard-wash", 2048, 240, source);
    source.profile = "krita-5.3.4.1-default-circle-candidate-1";
    source.tip.hardness = .5; source.tip.antialiasEdges = true;
    run("source-candidate-default-circle-aa", 2048, 240, source);
    for (const auto &model : {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"}) {
      source = Preset{};
      source.profile = std::string("krita-5.3.4.1-") + model + "-candidate-1";
      source.size.value = 64; source.smudgeRadius = 0;
      source.wet = std::string(model).find("dulling") != std::string::npos ? Wet::Dulling : Wet::Smear;
      run(model, 2048, 240, source);
    }
    // Fresh surfaces prevent retained paint/tile/reservoir state from being a
    // warm-up. Same-object repeats expose setup reuse without machine-cold claims.
    auto repeated = [&](const char *name, const Preset &preset) {
      const std::string firstName = std::string(name) + "-first";
      const std::string repeatName = std::string(name) + "-repeat";
      run(firstName.c_str(), 2048, 240, preset, 12, false, true);
      run(repeatName.c_str(), 2048, 240, preset, 12, true, true);
    };
    repeated("source-generated-soft65", generated(true));
    repeated("source-generated-gaussian", generated(false));
    repeated("source-scatter", scatter());
    repeated("source-random-gih", randomGih());
    source = Preset{};
    source.profile = "krita-5.3.4.1-new-dulling-candidate-1";
    source.size.value = 64; source.wet = Wet::Dulling; source.smudgeRadius = .5;
    repeated("source-new-dulling-radius-half", source);
    out << "],\"workloads\":" << ordinal << ",\"failedWorkloads\":" << failed
        << ",\"allWorkloadsPassed\":" << (passed ? "true" : "false") << "}\n";
    if (!out) throw std::runtime_error("Cannot finish benchmark report");
    return passed ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "::error::Native brush benchmark failed: " << e.what() << '\n';
    return 1;
  }
}
