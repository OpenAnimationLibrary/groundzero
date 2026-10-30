#include "toonz/rasterbrushengine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace RasterBrush;
namespace {
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
void measure(std::ostream &out, const char *name, int side, int rate,
             Preset p) {
  auto canvas = std::make_shared<Canvas>(side);
  auto budget = std::make_shared<MemoryBudget>(256u * 1024u * 1024u);
  std::vector<double> latency;
  size_t peak = 0, dabs = 0;
  double elapsed = 0;
  {
    BufferedSurface surface(canvas, budget);
    Stroke stroke(surface, p, 256u * 1024u * 1024u, budget);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i <= 60 * rate; ++i) {
      Sample s;
      s.time = i / double(rate);
      s.x = side * (.5 + .35 * std::sin(s.time * .4));
      s.y = side * (.5 + .2 * std::sin(s.time * .7));
      s.pressure = .5 + .5 * std::sin(s.time * .3) * std::sin(s.time * .3);
      s.hasPressure = true;
      auto event = std::chrono::steady_clock::now();
      if (!stroke.add(s))
        throw std::runtime_error(stroke.error());
      latency.push_back(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - event)
                            .count());
      peak = std::max(peak, budget->used());
    }
    elapsed = std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    dabs = stroke.dabCount();
    stroke.finish();
  }
  if (budget->used() != 0)
    throw std::runtime_error(
        "Active stroke allocations did not return to zero");
  std::sort(latency.begin(), latency.end());
  auto percentile = [&](double q) {
    return latency[size_t(std::ceil(q * latency.size())) - 1];
  };
  out << "{\"name\":\"" << name << "\",\"canvasSide\":" << side
      << ",\"inputHz\":" << rate
      << ",\"traceSeconds\":60,\"events\":" << latency.size()
      << ",\"dabs\":" << dabs << ",\"renderMilliseconds\":" << elapsed
      << ",\"eventP95Milliseconds\":" << percentile(.95)
      << ",\"eventP99Milliseconds\":" << percentile(.99)
      << ",\"eventMaxMilliseconds\":" << latency.back()
      << ",\"peakTrackedStrokeBytes\":" << peak
      << ",\"backingRasterBytes\":" << size_t(side) * side * sizeof(Pixel)
      << ",\"releasedStrokeBytes\":0}";
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("Expected benchmark JSON destination");
    std::ofstream out(argv[1]);
    if (!out)
      throw std::runtime_error("Cannot create benchmark report");
    out << "{\"scope\":\"native CPU event-to-surface only; hosted runner, no "
           "display/tablet latency claim\",\"records\":[";
    Preset p;
    p.size.value = 64;
    p.color = {.8, .1, .2, 1};
    p.size.mappings.push_back({Sensor::Pressure, {{0, .2}, {1, 1}}, 1});
    measure(out, "standard-dry", 2048, 240, p);
    out << ',';
    p.tip.antialiasEdges = true;
    measure(out, "standard-dry-antialias", 2048, 240, p);
    out << ',';
    p.tip.antialiasEdges = false;
    p.size.value = 256;
    auto texture = std::make_shared<Image>();
    texture->width = texture->height = 2;
    texture->pixels = {
        {0, 0, 0, .25}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, .5}};
    p.texture.image = texture;
    p.texture.scale = 4;
    p.secondary.enabled = true;
    p.secondary.size.value = .8;
    measure(out, "heavy-texture-dual", 4096, 120, p);
    out << ',';
    p.secondary.enabled = false;
    p.wet = Wet::Smear;
    measure(out, "heavy-smear", 4096, 120, p);
    Preset source;
    source.profile = "krita-5.3.4.1-hard-pixel-candidate-1";
    source.size.value = 64;
    source.wash = true;
    source.opacity.value = .5;
    source.flow.value = .5;
    out << ',';
    measure(out, "source-candidate-hard-wash", 2048, 240, source);
    for (const auto &model :
         {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"}) {
      source = Preset{};
      source.profile = std::string("krita-5.3.4.1-") + model + "-candidate-1";
      source.size.value = 64;
      source.smudgeRadius = 0;
      source.wet = std::string(model).find("dulling") != std::string::npos
                       ? Wet::Dulling
                       : Wet::Smear;
      out << ',';
      measure(out, model, 2048, 240, source);
    }
    out << "]}\n";
    if (!out)
      throw std::runtime_error("Cannot finish benchmark report");
  } catch (const std::exception &e) {
    std::cerr << "::error::Native brush benchmark failed: " << e.what() << '\n';
    return 1;
  }
}
