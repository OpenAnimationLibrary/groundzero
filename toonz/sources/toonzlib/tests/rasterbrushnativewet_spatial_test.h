#pragma once
// Independent native wet reference: authored spatial stencils, barycentric
// basis functions and tensor transport matrices, using no renderer helpers.
namespace NativeWetSpatialReference {
using Color = std::array<long double, 4>;
using Raster = std::vector<Color>;
struct Tap { int x, y, quarters; };
const std::array<Tap, 7> taps{{{2, 0, 1}, {0, 1, 2}, {1, 1, 4},
                              {1, 2, 3}, {2, 2, 2}, {0, 3, 1}, {3, 3, 4}}};
struct Frame {
  int cells;
  long double x, y, aspect;
  int turn;
  bool flipX, flipY;
};
Color foreground() { return {{3.L / 16, 1.L / 8, 1.L / 4, 3.L / 4}}; }
Color background(int x, int y) {
  return {{1.L / 32 + x / 1024.L + y / 2048.L,
           1.L / 16 + x / 2048.L + y / 1024.L,
           1.L / 8 + x / 4096.L + y / 2048.L,
           1.L / 4 + x / 256.L + y / 512.L}};
}
Pixel pixel(const Color &value) {
  return {double(value[0]), double(value[1]), double(value[2]), double(value[3])};
}
Raster initial(int width, int height, std::array<int, 2> hole) {
  Raster result(size_t(width) * height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      result[size_t(y) * width + x] = x == hole[0] && y == hole[1] ? Color{} : background(x, y);
  return result;
}
void install(Canvas &canvas, const Raster &raster, int width, int height) {
  canvas.w = width; canvas.h = height;
  canvas.pixels.resize(raster.size());
  for (size_t i = 0; i < raster.size(); ++i) canvas.pixels[i] = pixel(raster[i]);
}
std::shared_ptr<const Image> alphaTip() {
  auto image = std::make_shared<Image>();
  image->width = image->height = 4;
  image->pixels.resize(16);
  for (const auto &tap : taps) image->pixels[size_t(tap.y) * 4 + tap.x].a = tap.quarters / 4.0;
  return image;
}
void close(const Pixel &actual, const Color &expected, const char *message) {
  const Color measured{{actual.r, actual.g, actual.b, actual.a}};
  for (size_t component = 0; component < 4; ++component)
    require(std::abs(measured[component] - expected[component]) <= 1e-12L, message);
}
void rasterEquals(const Canvas &actual, const Raster &expected) {
  require(actual.pixels.size() == expected.size(), "Native wet oracle raster geometry changed");
  for (size_t i = 0; i < expected.size(); ++i)
    close(actual.pixels[i], expected[i], "Native wet spatial raster differs from independent reference");
}
long double hat(long double distance) { return std::max(0.L, 1 - std::abs(distance)); }
std::array<long double, 2> local(const Frame &frame, int x, int y) {
  const long double dx = (x + .5L - frame.x) * 2 / frame.cells;
  const long double dy = (y + .5L - frame.y) * 2 / frame.cells;
  long double u = 0, v = 0;
  switch (frame.turn) {
  case 0: u = dx; v = dy; break;
  case 1: u = dy; v = -dx; break;
  case 2: u = -dx; v = -dy; break;
  default: u = -dy; v = dx; break;
  }
  return {{frame.flipX ? -u : u, (frame.flipY ? -v : v) / frame.aspect}};
}
long double coverage(long double u, long double v) {
  long double value = 0;
  for (const auto &tap : taps)
    value += tap.quarters / 4.L * hat((u + 1) * 2 - .5L - tap.x) *
                                    hat((v + 1) * 2 - .5L - tap.y);
  return value;
}
// These are authored sample coordinates, not coordinates computed by the
// renderer's trigonometric/floor loops. The center is always integer +1/8.
std::array<int, 2> point4(const Frame &frame, int radiusCase, int i, int j) {
  const int wide[4][4] = {{12, 12, 12, 12}, {11, 11, 12, 12},
                          {10, 11, 12, 13}, {7, 10, 13, 16}};
  const int narrow[4][4] = {{12, 12, 12, 12}, {11, 12, 12, 12},
                            {11, 11, 12, 12}, {9, 11, 12, 14}};
  if (frame.flipX) i = 3 - i;
  if (frame.flipY) j = 3 - j;
  const auto &xs = wide[radiusCase];
  const auto &ys = frame.aspect == 1 ? wide[radiusCase] : narrow[radiusCase];
  int x = 0, y = 0;
  switch (frame.turn) {
  case 0: x = xs[i]; y = ys[j]; break;
  case 1: x = ys[3 - j]; y = xs[i]; break;
  case 2: x = xs[3 - i]; y = ys[3 - j]; break;
  default: x = ys[j]; y = xs[3 - i]; break;
  }
  return {{x + int(frame.x - 12.125L), y + int(frame.y - 12.125L)}};
}
std::array<int, 2> point8(const Frame &frame, int i, int j) {
  // Eight-pixel, half-aspect, quarter-turn frame. Its pickup covers a
  // duplicated horizontal axis and eight distinct vertical source pixels.
  const int xByRow[8] = {13, 13, 12, 12, 11, 11, 10, 10};
  if (frame.flipX) i = 7 - i;
  if (frame.flipY) j = 7 - j;
  return {{xByRow[j] + int(frame.x - 12.125L), 8 + i + int(frame.y - 12.125L)}};
}
Color weightedPickup(const Raster &raster, int width, const Frame &frame, int radiusCase) {
  Color result{};
  // The alpha stencil has exactly17 quarter units. Transparent taps keep
  // their weight in this denominator and contribute premultiplied zero.
  for (const auto &tap : taps) {
    const auto at = point4(frame, radiusCase, tap.x, tap.y);
    const auto &value = raster[size_t(at[1]) * width + at[0]];
    for (size_t component = 0; component < 4; ++component)
      result[component] += tap.quarters * value[component] / 17;
  }
  return result;
}
Color affine(const Color &a, long double wa, const Color &b, long double wb) {
  Color result{};
  for (size_t k = 0; k < 4; ++k) result[k] = wa * a[k] + wb * b[k];
  return result;
}
template <class Paint>
void deposit(Raster &raster, int width, int height, const Frame &frame, Paint paint) {
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const auto uv = local(frame, x, y);
      const long double weight = coverage(uv[0], uv[1]) * (5.L / 8) * (3.L / 4);
      if (weight == 0) continue;
      const Color ink = paint(uv[0], uv[1]);
      Color &destination = raster[size_t(y) * width + x];
      const long double oldWeight = 1 - weight * ink[3];
      for (size_t k = 0; k < 4; ++k)
        destination[k] = weight * ink[k] + oldWeight * destination[k];
    }
}
std::array<long double, 8> basis(int cells, long double coordinate) {
  std::array<long double, 8> weights{};
  const long double first = -1 + 1.L / cells, last = 1 - 1.L / cells;
  if (coordinate <= first) weights[0] = 1;
  else if (coordinate >= last) weights[cells - 1] = 1;
  else for (int i = 0; i < cells; ++i)
    weights[i] = hat((coordinate - (-1 + (2.L * i + 1) / cells)) * cells / 2);
  return weights;
}
Color evaluate(const Raster &field, int cells, long double u, long double v) {
  const auto horizontal = basis(cells, u), vertical = basis(cells, v);
  Color value{};
  for (int j = 0; j < cells; ++j)
    for (int i = 0; i < cells; ++i)
      for (size_t k = 0; k < 4; ++k)
        value[k] += horizontal[i] * vertical[j] * field[size_t(j) * cells + i][k];
  return value;
}
long double transport(int before, int after, int output, int input) {
  // Cell-centered tensor interpolation, expressed as two authored matrices.
  const int expand[8][4] = {{4, 0, 0, 0}, {3, 1, 0, 0}, {1, 3, 0, 0},
      {0, 3, 1, 0}, {0, 1, 3, 0}, {0, 0, 3, 1}, {0, 0, 1, 3}, {0, 0, 0, 4}};
  if (before == 4 && after == 8) return expand[output][input] / 4.L;
  if (before == 8 && after == 4) return input == 2 * output || input == 2 * output + 1 ? .5L : 0;
  return output == input ? 1 : 0;
}
Raster carry(const Raster &field, int before, int after) {
  Raster result(size_t(after) * after);
  for (int j = 0; j < after; ++j)
    for (int i = 0; i < after; ++i)
      for (int oldY = 0; oldY < before; ++oldY)
        for (int oldX = 0; oldX < before; ++oldX) {
          const long double coefficient = transport(before, after, i, oldX) * transport(before, after, j, oldY);
          for (size_t k = 0; k < 4; ++k)
            result[size_t(j) * after + i][k] += coefficient * field[size_t(oldY) * before + oldX][k];
        }
  return result;
}
void stateEquals(Stroke &stroke, const Raster &field, int cells) {
  Pixel measured;
  for (int j = 0; j < cells; ++j)
    for (int i = 0; i < cells; ++i) {
      const double u = -1 + (2.0 * i + 1) / cells, v = -1 + (2.0 * j + 1) / cells;
      require(stroke.nativeWetState(measured, u, v), "Native transported state unavailable");
      close(measured, field[size_t(j) * cells + i], "Native wet transported cell differs from tensor oracle");
    }
  const std::array<double, 2> probes[] = {{{-1, .5}}, {{0, 0}}, {{1, -.5}}};
  for (const auto &uv : probes) {
    require(stroke.nativeWetState(measured, uv[0], uv[1]), "Native transported edge state unavailable");
    close(measured, evaluate(field, cells, uv[0], uv[1]), "Native wet field interpolation differs from independent basis");
  }
}
Preset makePreset(Wet mode, const std::shared_ptr<const Image> &tip) {
  Preset p;
  p.wet = mode;
  p.tip.shape = Shape::Bitmap; p.tip.images = {tip};
  p.size.value = 4; p.spacing = 5;
  p.opacity.value = 5.0 / 8; p.flow.value = 3.0 / 4;
  p.color = pixel(foreground());
  return p;
}
}  // namespace NativeWetSpatialReference

void testNativeWetSpatialDullingOracle() {
  using namespace NativeWetSpatialReference;
  const auto tip = alphaTip();
  const Frame geometries[] = {{4, 12.125L, 12.125L, 1, 0, false, false},
      {4, 12.125L, 12.125L, .5L, 1, false, false}, {4, 12.125L, 12.125L, 1, 2, true, false},
      {4, 12.125L, 12.125L, .5L, 3, false, true}, {4, 12.125L, 12.125L, .5L, 0, true, true}};
  const double radii[] = {0, .5, 1, 3};
  for (const Frame &frame : geometries)
    for (int radiusCase = 0; radiusCase < 4; ++radiusCase)
      for (double pickup : {0., .375, 1.})
        for (double color : {0., .25, 1.})
          for (double reload : {0., .125, 1.}) {
            const auto hole = point4(frame, radiusCase, 3, 3);
            const auto seed = initial(32, 32, hole);
            Canvas canvas; install(canvas, seed, 32, 32);
            Raster expected = seed;
            Preset p = makePreset(Wet::Dulling, tip);
            p.aspect.value = double(frame.aspect); p.angle.value = frame.turn * 90;
            p.flipX = frame.flipX; p.flipY = frame.flipY;
            p.smudgeRadius = radii[radiusCase];
            p.pickup = pickup; p.colorRate = color; p.reload = reload;
            const Color q = weightedPickup(expected, 32, frame, radiusCase);
            const Color ink = affine(foreground(), 1 - (1 - color) * pickup, q, (1 - color) * pickup);
            const Color state = affine(foreground(), 1 - (1 - reload) * pickup, q, (1 - reload) * pickup);
            deposit(expected, 32, 32, frame, [&](long double, long double) { return ink; });
            auto budget = std::make_shared<MemoryBudget>(1024 * 1024);
            Stroke stroke(canvas, p, 1024 * 1024, budget);
            require(stroke.add(sample(12.125, 12.125, 0)), stroke.error().c_str());
            require(stroke.dabCount() == 1, "Spatial Dulling oracle received extra dabs");
            Pixel measured;
            require(stroke.nativeWetState(measured), "Spatial Dulling state unavailable");
            close(measured, state, "Normalized alpha-weight pickup differs from independent stencil");
            rasterEquals(canvas, expected);
            if (radiusCase % 2) stroke.cancel();
            else require(stroke.finish(), stroke.error().c_str());
            require(!stroke.nativeWetState(measured) && budget->used() == 0,
                    "Native Dulling finish/cancel retained state or storage");
            rasterEquals(canvas, expected);
            if (pickup == .375 && color == .25 && reload == .125) {
              Canvas fresh; install(fresh, seed, 32, 32);
              Stroke restarted(fresh, p, 1024 * 1024, budget);
              require(restarted.add(sample(12.125, 12.125, 0)) && restarted.nativeWetState(measured),
                      "Native Dulling restart failed");
              close(measured, state, "Native Dulling restart reused old reservoir");
              rasterEquals(fresh, expected);
              restarted.cancel();
              require(budget->used() == 0, "Native Dulling restart leaked shared storage");
            }
          }
}

void testNativeWetSpatialSmearOracle() {
  using namespace NativeWetSpatialReference;
  const auto tip = alphaTip();
  for (int mirror = 0; mirror < 3; ++mirror)
    for (double pickup : {0., .375, 1.})
      for (double color : {0., .25, 1.})
        for (double reload : {0., .125, 1.}) {
          const bool flipX = mirror == 1, flipY = mirror == 2;
          const Frame frames[] = {{4, 12.125L, 12.125L, 1, 0, flipX, flipY},
              {8, 32.125L, 12.125L, .5L, 1, flipX, flipY},
              {4, 72.125L, 12.125L, 1, 0, flipX, flipY}};
          const auto seed = initial(96, 24, point4(frames[0], 2, 3, 3));
          Canvas canvas; install(canvas, seed, 96, 24);
          Raster expected = seed, state, firstState, firstRaster;
          Preset p = makePreset(Wet::Smear, tip);
          p.size.value = 8;
          // Zero authored minima make pressure .5/1/.5 exactly4/8/4 and
          // aspect1/.5/1. Native minima otherwise interpolate the full range.
          p.size.minimum = p.aspect.minimum = 0;
          p.size.mappings = {{Sensor::Pressure, {{0, 0}, {1, 1}}, 1}};
          p.aspect.mappings = {{Sensor::Pressure, {{0, 1}, {.5, 1}, {1, .5}}, 1}};
          p.angle.value = 180;
          p.angle.mappings = {{Sensor::Pressure, {{0, 0}, {.5, 0}, {1, .5}}, 1}};
          p.flipX = flipX; p.flipY = flipY;
          p.pickup = pickup; p.colorRate = color; p.reload = reload;
          auto budget = std::make_shared<MemoryBudget>(1024 * 1024);
          Stroke stroke(canvas, p, 1024 * 1024, budget);
          int previousCells = 0;
          for (int phase = 0; phase < 3; ++phase) {
            const Frame &frame = frames[phase];
            Raster prior = state.empty() ? Raster(size_t(frame.cells) * frame.cells, foreground())
                                         : carry(state, previousCells, frame.cells);
            Raster beforeReload(prior.size()); state.resize(prior.size());
            for (int j = 0; j < frame.cells; ++j)
              for (int i = 0; i < frame.cells; ++i) {
                const auto at = frame.cells == 4 ? point4(frame, 2, i, j) : point8(frame, i, j);
                const Color q = expected[size_t(at[1]) * 96 + at[0]];
                const size_t index = size_t(j) * frame.cells + i;
                beforeReload[index] = affine(prior[index], 1 - pickup, q, pickup);
                state[index] = affine(prior[index], (1 - reload) * (1 - pickup), q, (1 - reload) * pickup);
                for (size_t k = 0; k < 4; ++k) state[index][k] += reload * foreground()[k];
              }
            deposit(expected, 96, 24, frame, [&](long double u, long double v) {
              return affine(evaluate(beforeReload, frame.cells, u, v), 1 - color, foreground(), color);
            });
            require(stroke.add(sample(double(frame.x), double(frame.y), phase, phase == 1 ? 1 : .5)),
                    stroke.error().c_str());
            require(stroke.dabCount() == size_t(phase + 1), "Smear geometry trace did not emit one authored dab");
            stateEquals(stroke, state, frame.cells);
            rasterEquals(canvas, expected);
            if (!phase) { firstState = state; firstRaster = expected; }
            previousCells = frame.cells;
          }
          Pixel measured;
          if (mirror % 2) stroke.cancel();
          else require(stroke.finish(), stroke.error().c_str());
          require(!stroke.nativeWetState(measured) && budget->used() == 0,
                  "Native Smear finish/cancel retained transported state or storage");
          rasterEquals(canvas, expected);
          if (pickup == .375 && color == .25 && reload == .125) {
            Canvas fresh; install(fresh, seed, 96, 24);
            Stroke restarted(fresh, p, 1024 * 1024, budget);
            require(restarted.add(sample(12.125, 12.125, 0, .5)), "Native Smear restart failed");
            stateEquals(restarted, firstState, 4);
            rasterEquals(fresh, firstRaster);
            restarted.cancel();
            require(budget->used() == 0, "Native Smear restart retained shared field storage");
          }
        }
}


void testNativeWetZeroWeightPickup() {
  using namespace NativeWetSpatialReference;
  // Zero weights differ from nonzero weights sampling transparent pixels:
  // there is no denominator to normalize. The native policy defines Q=zero.
  struct WriteCounter final : Canvas {
    size_t writes = 0;
    void write(int x, int y, const Pixel &value) override {
      ++writes;
      Canvas::write(x, y, value);
    }
  };
  auto emptyTip = std::make_shared<Image>();
  emptyTip->width = emptyTip->height = 4;
  emptyTip->pixels.resize(16);
  const auto seed = initial(64, 24, std::array<int, 2>{{-1, -1}});
  WriteCounter canvas;
  install(canvas, seed, 64, 24);
  Preset p = makePreset(Wet::Dulling, emptyTip);
  p.pickup = .5; p.colorRate = .25; p.reload = .125; p.smudgeRadius = 3;
  auto budget = std::make_shared<MemoryBudget>(1024 * 1024);
  Stroke stroke(canvas, p, 1024 * 1024, budget);
  // With Q=zero, R'=7/16*R+1/8*F. Starting at F gives these
  // independent closed-form factors after the first and second dabs.
  const long double factors[] = {9.L / 16, 95.L / 256};
  for (int phase = 0; phase < 2; ++phase) {
    require(stroke.add(sample(12.125 + 20 * phase, 12.125, phase)), stroke.error().c_str());
    require(stroke.dabCount() == size_t(phase + 1), "Zero-weight pickup received an unintended dab");
    Pixel state;
    require(stroke.nativeWetState(state), "Zero-weight native reservoir unavailable");
    close(state, affine(foreground(), factors[phase], Color{}, 0),
          "Zero-total-weight native pickup was not transparent");
    require(canvas.writes == 0, "Zero-alpha tip deposited pixels");
    rasterEquals(canvas, seed);
  }
  finishStroke(stroke);
  Pixel state;
  require(!stroke.nativeWetState(state) && budget->used() == 0 && canvas.writes == 0,
          "Zero-weight pickup retained state or painted while finishing");
}

void testNativeWetNormalAlphaLockOracle() {
  using namespace NativeWetSpatialReference;
  const auto tip = alphaTip();
  const Frame frame{4, 12.125L, 12.125L, .5L, 1, true, false};
  const auto hole = point4(frame, 2, 3, 3);
  const auto seed = initial(32, 32, hole);
  const auto holeUv = local(frame, hole[0], hole[1]);
  require(coverage(holeUv[0], holeUv[1]) > 0,
          "Alpha-lock transparent control is outside the authored tip support");
  for (Wet mode : {Wet::Dulling, Wet::Smear}) {
    Canvas canvas;
    install(canvas, seed, 32, 32);
    Raster expected = seed, inkGrid(16), stateGrid(16);
    const Color average = weightedPickup(seed, 32, frame, 2);
    for (int j = 0; j < 4; ++j)
      for (int i = 0; i < 4; ++i) {
        const auto at = point4(frame, 2, i, j);
        const Color q = mode == Wet::Dulling ? average : seed[size_t(at[1]) * 32 + at[0]];
        // Expand the native formula at p=3/8,c=1/4,d=1/8, independently
        // of the compositor: P=23/32*F+9/32*Q; R'=43/64*F+21/64*Q.
        const size_t index = size_t(j) * 4 + i;
        inkGrid[index] = affine(foreground(), 23.L / 32, q, 9.L / 32);
        stateGrid[index] = affine(foreground(), 43.L / 64, q, 21.L / 64);
      }
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x) {
        const auto uv = local(frame, x, y);
        const long double weight = coverage(uv[0], uv[1]) * (15.L / 32);
        if (weight == 0) continue;
        const Color ink = mode == Wet::Dulling ? inkGrid[0] : evaluate(inkGrid, 4, uv[0], uv[1]);
        const size_t index = size_t(y) * 32 + x;
        const Color before = seed[index];
        // Locked Normal composition conserves A exactly. Its pigment term
        // is confined by destination A; this is not unlocked source-over
        // followed by overwriting the resulting alpha.
        for (size_t channel = 0; channel < 3; ++channel)
          expected[index][channel] = (1 - weight * ink[3]) * before[channel] +
                                     before[3] * weight * ink[channel];
      }
    Preset p = makePreset(mode, tip);
    p.aspect.value = .5; p.angle.value = 90; p.flipX = true;
    p.pickup = .375; p.colorRate = .25; p.reload = .125; p.lockAlpha = true;
    auto budget = std::make_shared<MemoryBudget>(1024 * 1024);
    Stroke stroke(canvas, p, 1024 * 1024, budget);
    require(stroke.add(sample(12.125, 12.125, 0)), stroke.error().c_str());
    require(stroke.dabCount() == 1, "Wet alpha-lock oracle received extra dabs");
    rasterEquals(canvas, expected);
    bool pigmentChanged = false;
    for (size_t index = 0; index < seed.size(); ++index) {
      const Pixel before = pixel(seed[index]);
      const Pixel actual = canvas.pixels[index];
      require(actual.a == before.a, "Wet Normal alpha lock changed destination alpha");
      pigmentChanged = pigmentChanged || actual.r != before.r || actual.g != before.g || actual.b != before.b;
    }
    const Pixel transparent = canvas.read(hole[0], hole[1]);
    require(pigmentChanged && transparent.a == 0 && transparent.r == 0 && transparent.g == 0 && transparent.b == 0,
            "Wet alpha lock failed its pigment/transparent destination controls");
    if (mode == Wet::Smear) stateEquals(stroke, stateGrid, 4);
    else {
      Pixel state;
      require(stroke.nativeWetState(state), "Locked native Dulling reservoir unavailable");
      close(state, stateGrid[0], "Alpha locking changed pre-deposit wet pickup/reload");
    }
    finishStroke(stroke);
    require(budget->used() == 0, "Wet alpha-lock oracle retained stroke storage");
    rasterEquals(canvas, expected);
  }
}
