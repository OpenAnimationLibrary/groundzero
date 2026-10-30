#include "toonz/rasterbrushengine.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace RasterBrush {
namespace {
constexpr double pi = 3.14159265358979323846;
bool sourceWetProfile(const std::string &profile) {
  return profile == "krita-5.3.4.1-legacy-smear-candidate-1" ||
         profile == "krita-5.3.4.1-legacy-dulling-candidate-1" ||
         profile == "krita-5.3.4.1-new-smear-candidate-1";
}
bool sourceProfile(const std::string &profile) {
  return profile == "krita-5.3.4.1-hard-pixel-candidate-1" ||
         sourceWetProfile(profile);
}
double unit(double v) { return std::max(0.0, std::min(1.0, v)); }
Pixel clean(Pixel p) {
  p.a = unit(p.a);
  p.r = std::max(0.0, std::min(p.a, p.r));
  p.g = std::max(0.0, std::min(p.a, p.g));
  p.b = std::max(0.0, std::min(p.a, p.b));
  return p;
}
Pixel mul(Pixel p, double a) { return {p.r * a, p.g * a, p.b * a, p.a * a}; }
Pixel mix(Pixel p, Pixel q, double t) {
  return {p.r + (q.r - p.r) * t, p.g + (q.g - p.g) * t, p.b + (q.b - p.b) * t,
          p.a + (q.a - p.a) * t};
}
double wrap(double x) { return x - std::floor(x); }
Pixel hsv(Pixel p, double degrees, double saturation, double brightness) {
  if (p.a == 0) return {};
  if (degrees == 0 && saturation == 1 && brightness == 1) return p;
  const double r = p.r / p.a, g = p.g / p.a, b = p.b / p.a;
  const double maximum = std::max(r, std::max(g, b));
  const double delta   = maximum - std::min(r, std::min(g, b));
  double h             = 0;
  if (delta > 0) {
    if (maximum == r)
      h = (g - b) / delta;
    else if (maximum == g)
      h = 2 + (b - r) / delta;
    else
      h = 4 + (r - g) / delta;
  }
  h              = 6 * wrap(h / 6 + degrees / 360);
  const double v = unit(maximum * brightness);
  const double s = maximum > 0 ? unit(delta / maximum * saturation) : 0;
  const double c = v * s, x = c * (1 - std::abs(std::fmod(h, 2.0) - 1)),
               m = v - c;
  Pixel out;
  switch (int(h)) {
  case 0:
    out = {c, x, 0, 1};
    break;
  case 1:
    out = {x, c, 0, 1};
    break;
  case 2:
    out = {0, c, x, 1};
    break;
  case 3:
    out = {0, x, c, 1};
    break;
  case 4:
    out = {x, 0, c, 1};
    break;
  default:
    out = {c, 0, x, 1};
    break;
  }
  return {(out.r + m) * p.a, (out.g + m) * p.a, (out.b + m) * p.a, p.a};
}
double arc(double x) { return std::remainder(x, 2 * pi); }
double mask(double a, double b, MaskMode mode) {
  return mode == MaskMode::Multiply ? a * b : std::max(0.0, a - b);
}
Pixel texel(const Image &im, double x, double y, bool repeat) {
  if (repeat) {
    x = wrap(x);
    y = wrap(y);
  }
  const double fx = x * im.width - 0.5, fy = y * im.height - 0.5;
  const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
  auto at = [&](int a, int b) -> Pixel {
    if (repeat) {
      a = (a % im.width + im.width) % im.width;
      b = (b % im.height + im.height) % im.height;
    }
    if (a < 0 || b < 0 || a >= im.width || b >= im.height) return {};
    return im.pixels[size_t(b) * im.width + a];
  };
  return mix(mix(at(ix, iy), at(ix + 1, iy), fx - ix),
             mix(at(ix, iy + 1), at(ix + 1, iy + 1), fx - ix), fy - iy);
}
Pixel tipPixel(const Tip &tip, size_t index, double x, double y) {
  if (tip.shape == Shape::Bitmap) {
    const auto &image   = *tip.images[index % tip.images.size()];
    const double extent = std::max(image.width, image.height);
    return texel(image, x * extent / image.width * .5 + .5,
                 y * extent / image.height * .5 + .5, false);
  }
  double d = tip.shape == Shape::Square ? std::max(std::abs(x), std::abs(y))
                                        : std::hypot(x, y);
  if (d >= 1) return {};
  double a = d <= tip.hardness ? 1 : (1 - d) / std::max(1e-9, 1 - tip.hardness);
  if (tip.shape == Shape::Gaussian)
    a = (std::exp(-4 * d * d) - std::exp(-4.0)) / (1 - std::exp(-4.0));
  return {0, 0, 0, unit(a)};
}
bool finitePixel(const Pixel &p) {
  return std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b) &&
         std::isfinite(p.a);
}
// Integrate procedural coverage on a fixed 8x8 pixel grid in tip coordinates.
// The Jacobian includes rotation/aspect/secondary transforms. Bitmap sampling
// keeps its separately defined bilinear rule. Fully opaque/outside footprints
// avoid supersampling, so large hard dabs pay this cost only at their edges.
Pixel filteredTip(const Tip &tip, size_t index, double u, double v, double ux,
                  double uy, double vx, double vy) {
  if (!tip.antialiasEdges || tip.shape == Shape::Bitmap)
    return tipPixel(tip, index, u, v);
  const double hu = .5 * (std::abs(ux) + std::abs(uy));
  const double hv = .5 * (std::abs(vx) + std::abs(vy));
  auto distance   = [&](double x, double y) {
    return tip.shape == Shape::Square ? std::max(x, y) : std::hypot(x, y);
  };
  if (distance(std::max(0.0, std::abs(u) - hu),
               std::max(0.0, std::abs(v) - hv)) >= 1)
    return {};
  if (tip.shape != Shape::Gaussian &&
      distance(std::abs(u) + hu, std::abs(v) + hv) <= tip.hardness)
    return {0, 0, 0, 1};
  double coverage = 0;
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      double dx = (x + .5) / 8 - .5, dy = (y + .5) / 8 - .5;
      coverage +=
          tipPixel(tip, index, u + ux * dx + uy * dy, v + vx * dx + vy * dy).a;
    }
  return {0, 0, 0, coverage / 64};
}
Sample interpolate(const Sample &a, const Sample &b, double t) {
  Sample s   = b;
  s.x        = a.x + (b.x - a.x) * t;
  s.y        = a.y + (b.y - a.y) * t;
  s.pressure = a.pressure + (b.pressure - a.pressure) * t;
  s.tiltX    = a.tiltX + (b.tiltX - a.tiltX) * t;
  s.tiltY    = a.tiltY + (b.tiltY - a.tiltY) * t;
  s.rotation = a.rotation + arc(b.rotation - a.rotation) * t;
  s.time     = a.time + (b.time - a.time) * t;
  return s;
}
}  // namespace

Pixel composite(Pixel dst, Pixel src, Blend mode, bool lockAlpha) {
  dst = clean(dst);
  src = clean(src);
  if (mode == Blend::Erase) return lockAlpha ? dst : mul(dst, 1 - src.a);
  auto channel = [&](double d, double s) {
    const double cb = dst.a > 0 ? d / dst.a : 0, cs = src.a > 0 ? s / src.a : 0;
    double b = cs;
    if (mode == Blend::Multiply) b = cb * cs;
    if (mode == Blend::Screen) b = 1 - (1 - cb) * (1 - cs);
    if (mode == Blend::Overlay)
      b = cb <= 0.5 ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    if (lockAlpha) return dst.a * ((1 - src.a) * cb + src.a * b);
    return src.a * (1 - dst.a) * cs + src.a * dst.a * b + (1 - src.a) * d;
  };
  return clean({channel(dst.r, src.r), channel(dst.g, src.g),
                channel(dst.b, src.b),
                lockAlpha ? dst.a : src.a + dst.a * (1 - src.a)});
}

std::string validate(const Preset &p) {
  const bool sourceHard = p.profile == "krita-5.3.4.1-hard-pixel-candidate-1";
  const bool sourceWet  = sourceWetProfile(p.profile);
  if (p.profile != "groundzero-native-1" && !sourceHard && !sourceWet)
    return "Unimplemented behavior profile: " + p.profile;
  if (sourceHard || sourceWet) {
    auto fixed = [](const Dynamic &value, double expected) {
      return value.value == expected && value.mappings.empty();
    };
    if (p.tip.shape != Shape::Round || p.tip.hardness != 1 ||
        p.tip.antialiasEdges || !p.tip.images.empty() ||
        !p.size.mappings.empty() || p.size.value != std::floor(p.size.value) ||
        !fixed(p.aspect, 1) || !fixed(p.angle, 0) || !fixed(p.opacity, 1) ||
        !fixed(p.flow, 1) || !fixed(p.scatter, 0) ||
        !fixed(p.countControl, 1) || !fixed(p.backgroundMix, 0) ||
        !fixed(p.hue, 0) || !fixed(p.saturation, 1) ||
        !fixed(p.brightness, 1) || p.count != 1 || p.airbrushRate != 0 ||
        p.wash || p.lockAlpha || p.flipX || p.flipY ||
        p.blend != Blend::Normal || (sourceHard && p.wet != Wet::Off) ||
        p.texture.image || p.secondary.enabled || p.color.a != 1)
      return "Unimplemented setting in the experimental Krita hard-pixel "
             "profile";
  }
  if (sourceWet &&
      (p.wet != (p.profile == "krita-5.3.4.1-legacy-dulling-candidate-1"
                     ? Wet::Dulling
                     : Wet::Smear) ||
       p.smudgeRadius != 0 || p.reload != 0 || p.pickupControl.value != 1 ||
       !p.pickupControl.mappings.empty() || p.colorRateControl.value != 1 ||
       !p.colorRateControl.mappings.empty() || p.radiusControl.value != 1 ||
       !p.radiusControl.mappings.empty()))
    return "Unimplemented setting in the experimental Krita wet profile";
  auto range = [](double v, double lo, double hi) {
    return std::isfinite(v) && v >= lo && v <= hi;
  };
  if (int(p.blend) < 0 || int(p.blend) > int(Blend::Erase) || int(p.wet) < 0 ||
      int(p.wet) > int(Wet::Smear) || int(p.texture.mode) < 0 ||
      int(p.texture.mode) > int(MaskMode::Subtract) ||
      int(p.secondary.mode) < 0 ||
      int(p.secondary.mode) > int(MaskMode::Subtract))
    return "Invalid brush mode";
  if (!finitePixel(p.color) || p.color.a < 0 || p.color.a > 1 ||
      p.color.r < 0 || p.color.g < 0 || p.color.b < 0 ||
      p.color.r > p.color.a || p.color.g > p.color.a || p.color.b > p.color.a)
    return "Invalid premultiplied brush color";
  if (!finitePixel(p.background) || p.background.a < 0 || p.background.a > 1 ||
      p.background.r < 0 || p.background.g < 0 || p.background.b < 0 ||
      p.background.r > p.background.a || p.background.g > p.background.a ||
      p.background.b > p.background.a)
    return "Invalid premultiplied background color";
  auto dynamic = [&](const Dynamic &d, double lo, double hi) {
    if (!range(d.value, lo, hi) || !range(d.minimum, 0, 1) ||
        d.mappings.size() > 16)
      return false;
    for (const auto &m : d.mappings) {
      if (int(m.sensor) < 0 || int(m.sensor) > int(Sensor::DabIndex) ||
          !range(m.scale, 1e-9, 1e9) || m.curve.empty() ||
          m.curve.size() > 1024 || int(m.interpolation) < 0 ||
          int(m.interpolation) > int(Interpolation::NaturalCubic))
        return false;
      double last = -1;
      for (const auto &k : m.curve) {
        if (!range(k.x, 0, 1) || !range(k.y, 0, 1) || k.x <= last) return false;
        if (m.interpolation == Interpolation::NaturalCubic && last >= 0 &&
            k.x - last < 1e-9)
          return false;
        last = k.x;
      }
    }
    return true;
  };
  size_t bytes = 0;
  auto image   = [&](const std::shared_ptr<const Image> &im) {
    if (!im) return false;
    if (im->width < 1 || im->height < 1 || im->width > 8192 ||
        im->height > 8192)
      return false;
    if (im->pixels.size() != size_t(im->width) * im->height) return false;
    size_t n = im->pixels.size() * sizeof(Pixel);
    if (n > 64u * 1024u * 1024u || bytes > 256u * 1024u * 1024u - n)
      return false;
    bytes += n;
    for (const auto &v : im->pixels)
      if (!finitePixel(v) || v.a < 0 || v.a > 1 || v.r < 0 || v.g < 0 ||
          v.b < 0 || v.r > v.a || v.g > v.a || v.b > v.a)
        return false;
    return true;
  };
  auto tip = [&](const Tip &t) {
    if (int(t.shape) < 0 || int(t.shape) > int(Shape::Bitmap) ||
        !range(t.hardness, 0, 1) || t.images.size() > 256)
      return false;
    if (t.shape == Shape::Bitmap && t.images.empty()) return false;
    for (const auto &im : t.images)
      if (!image(im)) return false;
    return true;
  };
  if (!tip(p.tip) || !dynamic(p.size, .25, 4096) ||
      !dynamic(p.aspect, .01, 1) || !dynamic(p.angle, -36000, 36000) ||
      !dynamic(p.opacity, 0, 1) || !dynamic(p.flow, 0, 1) ||
      !dynamic(p.scatter, 0, 5) || !dynamic(p.countControl, 0, 1) ||
      !range(p.spacing, .01, 5) || !range(p.airbrushRate, 0, 120) ||
      p.count < 1 || p.count > 64 || !finitePixel(p.color))
    return "Invalid brush geometry, dynamics, image, or resource budget";
  const auto &t = p.texture;
  if (t.image && (!image(t.image) || !range(t.scale, .01, 4096) ||
                  !range(t.angle, -36000, 36000) || !range(t.depth, 0, 1) ||
                  !range(t.contrast, 0, 10) || !range(t.brightness, -1, 1) ||
                  !range(t.offsetX, -1e9, 1e9) || !range(t.offsetY, -1e9, 1e9)))
    return "Invalid texture";
  const auto &s = p.secondary;
  if ((!tip(s.tip) || !dynamic(s.size, .01, 10) ||
       !dynamic(s.angle, -36000, 36000) || !dynamic(s.opacity, 0, 1) ||
       !range(s.aspect, .01, 1) || !range(s.scatter, 0, 5) || s.count < 1 ||
       s.count > 64))
    return "Invalid secondary tip";
  if (!dynamic(p.pickupControl, 0, 1) || !dynamic(p.colorRateControl, 0, 1) ||
      !dynamic(p.radiusControl, 0, 1))
    return "Invalid wet sensor dynamics";
  if (!dynamic(p.backgroundMix, 0, 1) || !dynamic(p.hue, -36000, 36000) ||
      !dynamic(p.saturation, 0, 2) || !dynamic(p.brightness, 0, 2))
    return "Invalid color dynamics";
  if (!range(p.pickup, 0, 1) || !range(p.colorRate, 0, 1) ||
      !range(p.reload, 0, 1) || !range(p.smudgeRadius, 0, 3))
    return "Invalid wet brush settings";
  if (p.wet != Wet::Off && (p.wash || s.enabled || p.blend != Blend::Normal))
    return "Wet brushes require normal build-up painting and a single tip";
  if (p.wet != Wet::Off &&
      (p.tip.images.size() > 1 ||
       std::any_of(p.tip.images.begin(), p.tip.images.end(),
                   [](const std::shared_ptr<const Image> &image) {
                     return image->color;
                   })))
    return "Wet brushes require a single alpha-mask tip";
  return {};
}

struct BufferedSurface::Impl {
  using Tile = std::array<Pixel, 1024>;
  std::shared_ptr<Surface> backing;
  std::shared_ptr<MemoryBudget> budget;
  std::map<std::pair<int, int>, std::unique_ptr<Tile>> tiles;
  size_t reserved = 0;
  Impl(std::shared_ptr<Surface> b, std::shared_ptr<MemoryBudget> mem)
      : backing(std::move(b)), budget(std::move(mem)) {}
  ~Impl() { budget->release(reserved); }
};
BufferedSurface::BufferedSurface(std::shared_ptr<Surface> b,
                                 std::shared_ptr<MemoryBudget> mem)
    : m(new Impl(std::move(b), std::move(mem))) {}
BufferedSurface::~BufferedSurface() = default;
int BufferedSurface::width() const { return m->backing->width(); }
int BufferedSurface::height() const { return m->backing->height(); }
bool BufferedSurface::prepare(int x0, int y0, int x1, int y1) {
  if (x0 < 0 || y0 < 0 || x1 > width() || y1 > height() || x0 >= x1 || y0 >= y1)
    return false;
  const int left = x0 / 32, top = y0 / 32, right = (x1 - 1) / 32,
            bottom = (y1 - 1) / 32;
  if (!m->backing->prepare(left * 32, top * 32,
                           std::min(width(), (right + 1) * 32),
                           std::min(height(), (bottom + 1) * 32)))
    return false;
  try {
    for (int ty = top; ty <= bottom; ++ty)
      for (int tx = left; tx <= right; ++tx) {
        auto key = std::make_pair(tx, ty);
        if (m->tiles.count(key)) continue;
        constexpr size_t charge = sizeof(Impl::Tile) + 128;
        if (!m->budget->reserve(charge)) return false;
        m->reserved += charge;
        auto tile = std::make_unique<Impl::Tile>();
        for (int y = 0; y < 32 && ty * 32 + y < height(); ++y)
          for (int x = 0; x < 32 && tx * 32 + x < width(); ++x)
            (*tile)[y * 32 + x] = m->backing->read(tx * 32 + x, ty * 32 + y);
        m->tiles.emplace(key, std::move(tile));
      }
  } catch (const std::bad_alloc &) {
    return false;
  }
  return true;
}
Pixel BufferedSurface::read(int x, int y) const {
  return m->tiles.at({x / 32, y / 32})->at((y % 32) * 32 + x % 32);
}
void BufferedSurface::write(int x, int y, const Pixel &p) {
  m->tiles.at({x / 32, y / 32})->at((y % 32) * 32 + x % 32) = p;
  m->backing->write(x, y, p);
}
double BufferedSurface::coverage(int x, int y) const {
  return m->backing->coverage(x, y);
}

struct Stroke::Impl {
  Surface &surface;
  const Preset p;
  size_t budget, used = 0, dabs = 0;
  std::shared_ptr<MemoryBudget> shared;
  size_t reserved = 0;
  std::string failure;
  Sample previous, first, previousSourceDab;
  bool started = false, finished = false;
  bool pendingSourceEndpoint = false;
  bool hasInitialDirection   = false;
  double travel = 0, sinceDab = 0, sinceTime = 0, spacing = 1, direction = 0,
         speed = 0, initialDirection = 0;
  std::uint64_t rng;
  double randomStroke = 0, randomDab = 0;
  Pixel reservoir;
  std::vector<Pixel> smear;
  int smearSide = 0;
  std::map<const Mapping *, std::vector<double>> cubic;
  struct Tile {
    std::array<Pixel, 1024> base{}, paint{};
  };
  std::map<std::pair<int, int>, std::unique_ptr<Tile>> tiles;
  Impl(Surface &s, const Preset &v, size_t limit,
       std::shared_ptr<MemoryBudget> memory)
      : surface(s)
      , p(v)
      , budget(limit)
      , shared(memory ? std::move(memory)
                      : std::make_shared<MemoryBudget>(limit))
      , rng(v.seed ? v.seed : 1)
      , reservoir(v.color) {
    failure      = validate(v);
    randomStroke = random();
    if (!failure.empty()) return;
    try {
      for (const Dynamic *dynamic :
           {&p.size, &p.aspect, &p.angle, &p.opacity, &p.flow, &p.scatter,
            &p.countControl, &p.pickupControl, &p.colorRateControl,
            &p.radiusControl, &p.backgroundMix, &p.hue, &p.saturation,
            &p.brightness, &p.secondary.size, &p.secondary.angle,
            &p.secondary.opacity}) {
        for (const auto &mapping : dynamic->mappings) {
          if (mapping.interpolation != Interpolation::NaturalCubic) continue;
          size_t n      = mapping.curve.size(),
                 charge = 2 * n * sizeof(double) + 128;
          if (charge > budget - used || !shared->reserve(charge)) {
            failure = "Curve memory limit exceeded";
            return;
          }
          reserved += charge;
          std::vector<double> second(n, 0), rhs(n, 0);
          for (size_t i = 1; i + 1 < n; ++i) {
            const auto &a = mapping.curve[i - 1], &b = mapping.curve[i],
                       &c = mapping.curve[i + 1];
            double span = c.x - a.x, fraction = (b.x - a.x) / span;
            double denominator = fraction * second[i - 1] + 2;
            second[i]          = (fraction - 1) / denominator;
            double slopeChange =
                (c.y - b.y) / (c.x - b.x) - (b.y - a.y) / (b.x - a.x);
            rhs[i] =
                (6 * slopeChange / span - fraction * rhs[i - 1]) / denominator;
          }
          for (size_t i = n - 1; i > 0; --i)
            second[i - 1] = second[i - 1] * second[i] + rhs[i - 1];
          cubic.emplace(&mapping, std::move(second));
          std::vector<double>().swap(rhs);
          shared->release(n * sizeof(double));
          reserved -= n * sizeof(double);
          used += charge - n * sizeof(double);
        }
      }
    } catch (const std::bad_alloc &) {
      failure = "Curve allocation failed";
    }
  }
  ~Impl() { shared->release(reserved); }
  double random() {
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return double((rng * UINT64_C(2685821657736338717)) >> 11) *
           (1.0 / 9007199254740992.0);
  }
  double sensor(const Mapping &m, const Sample &s) const {
    double v = 0;
    switch (m.sensor) {
    case Sensor::Pressure:
      v = s.hasPressure ? s.pressure : 1;
      break;
    case Sensor::TiltX:
      v = s.hasTilt ? (s.tiltX + 1) * .5 : .5;
      break;
    case Sensor::TiltY:
      v = s.hasTilt ? (s.tiltY + 1) * .5 : .5;
      break;
    case Sensor::TiltElevation:
      v = s.hasTilt ? unit(std::hypot(s.tiltX, s.tiltY)) : 0;
      break;
    case Sensor::TiltDirection:
      v = s.hasTilt ? wrap(std::atan2(s.tiltY, s.tiltX) / (2 * pi)) : 0;
      break;
    case Sensor::Rotation:
      v = s.hasRotation ? wrap(s.rotation / (2 * pi)) : 0;
      break;
    case Sensor::Direction:
      v = wrap(direction / (2 * pi));
      break;
    case Sensor::Distance:
      v = travel;
      break;
    case Sensor::Time:
      v = s.time - first.time;
      break;
    case Sensor::Speed:
      v = speed;
      break;
    case Sensor::RandomDab:
      v = randomDab;
      break;
    case Sensor::RandomStroke:
      v = randomStroke;
      break;
    case Sensor::InitialDirection:
      v = wrap(initialDirection / (2 * pi));
      break;
    case Sensor::DabIndex:
      v = double(dabs);
      break;
    }
    return unit(v / m.scale);
  }
  double eval(const Dynamic &d, const Sample &s) const {
    double factor = 1;
    for (const auto &m : d.mappings) {
      double x = sensor(m, s), v = m.curve.back().y;
      if (x <= m.curve.front().x)
        v = m.curve.front().y;
      else
        for (size_t i = 1; i < m.curve.size(); ++i)
          if (x <= m.curve[i].x) {
            const auto &a = m.curve[i - 1];
            const auto &b = m.curve[i];
            v             = a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
            if (m.interpolation == Interpolation::NaturalCubic) {
              const auto &second = cubic.at(&m);
              double span = b.x - a.x, left = (b.x - x) / span,
                     right = (x - a.x) / span;
              v += ((left * left * left - left) * second[i - 1] +
                    (right * right * right - right) * second[i]) *
                   span * span / 6;
              v = unit(v);
            }
            break;
          }
      factor *= v;
    }
    return d.value * (d.minimum + (1 - d.minimum) * factor);
  }
  size_t index(const Tip &t) {
    return t.images.empty() ? 0
           : t.randomSequence
               ? size_t(random() * t.images.size()) % t.images.size()
               : dabs % t.images.size();
  }
  Tile *tile(int x, int y) {
    const auto key = std::make_pair(x / 32, y / 32);
    auto found     = tiles.find(key);
    if (found != tiles.end()) return found->second.get();
    constexpr size_t charge = sizeof(Tile) + 128;
    if (charge > budget - used || !shared->reserve(charge)) {
      failure = "Stroke memory limit exceeded";
      return nullptr;
    }
    reserved += charge;
    const int x0 = key.first * 32, y0 = key.second * 32;
    if (!surface.prepare(x0, y0, std::min(x0 + 32, surface.width()),
                         std::min(y0 + 32, surface.height()))) {
      failure = "Raster transaction refused the stroke region";
      return nullptr;
    }
    std::unique_ptr<Tile> value(new Tile);
    for (int j = 0; j < 32 && j + y0 < surface.height(); ++j)
      for (int i = 0; i < 32 && i + x0 < surface.width(); ++i)
        value->base[j * 32 + i] = surface.read(x0 + i, y0 + j);
    Tile *ptr = value.get();
    tiles.emplace(key, std::move(value));
    used += charge;
    return ptr;
  }
  bool sourceWetDab(const Sample &s) {
    // Independent candidate inferred from original first/second-dab fixtures.
    // Model-specific equations are intentionally separate from native wet
    // paint.
    const double radius = p.size.value * .5;
    spacing             = std::max(.01, p.size.value * p.spacing);
    if (!dabs) {
      previousSourceDab = s;
      ++dabs;
      return true;
    }
    const double cx = s.x + .5, cy = s.y;
    const int reach = int(std::ceil(radius + 1));
    const int x0    = std::max(0, int(std::floor(cx)) - reach),
              y0    = std::max(0, int(std::floor(cy)) - reach),
              x1    = std::min(surface.width(), int(std::ceil(cx)) + reach),
              y1    = std::min(surface.height(), int(std::ceil(cy)) + reach);
    if (x0 >= x1 || y0 >= y1) {
      previousSourceDab = s;
      ++dabs;
      return true;
    }
    const double deltaX = previousSourceDab.x - s.x,
                 deltaY = previousSourceDab.y - s.y;
    int sx0             = std::max(0, int(std::floor(x0 + deltaX))),
        sy0             = std::max(0, int(std::floor(y0 + deltaY))),
        sx1 = std::min(surface.width(), int(std::ceil(x1 + deltaX)) + 1),
        sy1 = std::min(surface.height(), int(std::ceil(y1 + deltaY)) + 1);
    if (!surface.prepare(x0, y0, x1, y1) ||
        (sx0 < sx1 && sy0 < sy1 && !surface.prepare(sx0, sy0, sx1, sy1))) {
      failure = "Raster transaction refused the source wet region";
      return false;
    }
    const int centerX = int(std::floor(previousSourceDab.x)),
              centerY = int(std::floor(previousSourceDab.y));
    if (p.wet == Wet::Dulling && centerX >= 0 && centerY >= 0 &&
        centerX < surface.width() && centerY < surface.height() &&
        !surface.prepare(centerX, centerY, centerX + 1, centerY + 1)) {
      failure = "Raster transaction refused the source wet sample";
      return false;
    }
    const size_t bytes = size_t(x1 - x0) * (y1 - y0) * sizeof(Pixel);
    if (bytes > budget - used || !shared->reserve(bytes)) {
      failure = "Source wet snapshot memory limit exceeded";
      return false;
    }
    reserved += bytes;
    // Snapshot before writes, including overlapping dabs. The next dab reads
    // the deposited surface, not the native pre-deposition reservoir.
    std::vector<Pixel> feedback(size_t(x1 - x0) * (y1 - y0));
    auto at = [&](int x, int y) -> Pixel {
      return x >= 0 && y >= 0 && x < surface.width() && y < surface.height()
                 ? surface.read(x, y)
                 : Pixel{};
    };
    const Pixel center = p.wet == Wet::Dulling ? at(centerX, centerY) : Pixel{};
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const double fx = x + deltaX, fy = y + deltaY;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        feedback[size_t(y - y0) * (x1 - x0) + x - x0] =
            p.wet == Wet::Dulling
                ? center
                : mix(mix(at(ix, iy), at(ix + 1, iy), fx - ix),
                      mix(at(ix, iy + 1), at(ix + 1, iy + 1), fx - ix),
                      fy - iy);
      }
    auto quantize = [](Pixel v) {
      if (v.a <= 0) return Pixel{};
      const double a = std::floor(unit(v.a) * 255 + .5) / 255;
      return Pixel{std::floor(unit(v.r / v.a) * 255 + .5) / 255 * a,
                   std::floor(unit(v.g / v.a) * 255 + .5) / 255 * a,
                   std::floor(unit(v.b / v.a) * 255 + .5) / 255 * a, a};
    };
    const bool modern = p.profile == "krita-5.3.4.1-new-smear-candidate-1";
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        double dx = (x + .5 - cx) / radius, dy = (y + .5 - cy) / radius;
        double coverage =
            tipPixel(p.tip, 0, dx, dy).a * unit(surface.coverage(x, y));
        if (coverage <= 0) continue;
        Pixel paint = feedback[size_t(y - y0) * (x1 - x0) + x - x0];
        Pixel out   = surface.read(x, y);
        if (modern) {
          out = composite(out, mul(paint, p.pickup * coverage), Blend::Normal);
          out =
              composite(out, mul(p.color, p.colorRate * p.colorRate * coverage),
                        Blend::Normal);
        } else {
          paint = composite(
              paint, mul(p.color, p.colorRate * std::max(.2, 1 - p.pickup)),
              Blend::Normal);
          out = composite(out, mul(paint, p.pickup * coverage), Blend::Normal);
        }
        surface.write(x, y, quantize(out));
      }
    shared->release(bytes);
    reserved -= bytes;
    previousSourceDab = s;
    ++dabs;
    return true;
  }
  bool dab(const Sample &s) {
    if (dabs >= 1000000) {
      failure = "Stroke dab limit exceeded";
      return false;
    }
    if (sourceWetProfile(p.profile)) return sourceWetDab(s);
    randomDab = random();
    const Pixel foreground =
        hsv(mix(p.color, p.background, eval(p.backgroundMix, s)),
            eval(p.hue, s), eval(p.saturation, s), eval(p.brightness, s));
    if (!dabs) reservoir = foreground;
    const double diameter = std::max(.25, eval(p.size, s)),
                 radius   = diameter * .5;
    spacing               = std::max(.01, diameter * p.spacing);
    const double aspect   = std::max(.01, eval(p.aspect, s));
    const double angle = eval(p.angle, s) * pi / 180, c = std::cos(angle),
                 sn           = std::sin(angle);
    const double opacity      = unit(eval(p.opacity, s)),
                 flow         = unit(eval(p.flow, s));
    const double pickup       = p.pickup * eval(p.pickupControl, s),
                 colorRate    = p.colorRate * eval(p.colorRateControl, s);
    const double smudgeRadius = p.smudgeRadius * eval(p.radiusControl, s);
    const size_t primaryIndex = index(p.tip),
                 secondIndex  = index(p.secondary.tip);
    const double scatter      = eval(p.scatter, s) * diameter;
    const int stampCount =
        int(std::floor(p.count * unit(eval(p.countControl, s)) + .5));
    for (int stamp = 0; stamp < stampCount; ++stamp) {
      const double scatterX = (random() * 2 - 1) * scatter,
                   scatterY = (random() * 2 - 1) * scatter;
      // Independently measured K5341 hard-circle lattice: integer x and
      // half-integer y distances from an integral API stroke position. The
      // native profile retains its pixel-center convention unchanged.
      const double sourceOffset =
          p.profile == "krita-5.3.4.1-hard-pixel-candidate-1" ? .5 : 0;
      const double cx = s.x + sourceOffset +
                        (p.scatterBothAxes ? scatterX
                                           : -std::sin(direction) * scatterX),
                   cy = s.y + (p.scatterBothAxes
                                   ? scatterY
                                   : std::cos(direction) * scatterX);
      const int reach = int(std::ceil(radius * 1.415 + 1));
      int x0          = std::max(0, int(std::floor(cx)) - reach),
          y0          = std::max(0, int(std::floor(cy)) - reach);
      int x1          = std::min(surface.width(), int(std::ceil(cx)) + reach),
          y1          = std::min(surface.height(), int(std::ceil(cy)) + reach);
      if (x0 >= x1 || y0 >= y1) continue;
      int halo = p.wet == Wet::Off ? 0 : int(std::ceil(radius * smudgeRadius));
      if (!surface.prepare(std::max(0, x0 - halo), std::max(0, y0 - halo),
                           std::min(surface.width(), x1 + halo),
                           std::min(surface.height(), y1 + halo))) {
        failure = "Raster transaction refused the stroke region";
        return false;
      }
      Pixel paint = foreground;
      std::vector<Pixel> nextSmear;
      int side = 0;
      if (p.wet != Wet::Off) {
        // Snapshot all feedback before modifying any destination pixel.
        side         = std::min(256, std::max(2, int(std::ceil(diameter))));
        size_t bytes = size_t(side) * side * sizeof(Pixel);
        if (bytes * 2 > budget - used || !shared->reserve(bytes)) {
          failure = "Wet brush memory limit exceeded";
          return false;
        }
        reserved += bytes;
        nextSmear.resize(size_t(side) * side);
        Pixel avg;
        double total = 0;
        for (int j = 0; j < side; ++j)
          for (int i = 0; i < side; ++i) {
            double u = (i + .5) / side * 2 - 1, v = (j + .5) / side * 2 - 1;
            // Reservoir samples use the same local tip frame as deposition.
            // Keeping this grid in canvas axes makes narrow/rotated tips pick
            // up pixels outside their footprint and twists transported fields.
            const double dx = (p.flipX ? -u : u) * radius * smudgeRadius,
                         dy = (p.flipY ? -v : v) * radius * aspect *
                              smudgeRadius;
            int xx  = int(std::floor(cx + dx * c - dy * sn));
            int yy  = int(std::floor(cy + dx * sn + dy * c));
            Pixel q = (xx >= 0 && yy >= 0 && xx < surface.width() &&
                       yy < surface.height())
                          ? surface.read(xx, yy)
                          : Pixel{};
            nextSmear[size_t(j) * side + i] = q;
            double w = filteredTip(p.tip, primaryIndex, u, v, 2.0 / side, 0, 0,
                                   2.0 / side)
                           .a;
            avg.r += q.r * w;
            avg.g += q.g * w;
            avg.b += q.b * w;
            avg.a += q.a * w;
            total += w;
          }
        if (total > 0) avg = mul(avg, 1 / total);
        reservoir = mix(reservoir, avg, pickup);
        paint     = mix(reservoir, foreground, colorRate);
      }
      const auto &t   = p.texture;
      const auto &sec = p.secondary;
      const double tc = std::cos(t.angle * pi / 180),
                   ts = std::sin(t.angle * pi / 180);
      const double secondSize =
          sec.enabled ? std::max(.01, eval(sec.size, s)) : 1;
      const double secondAngle =
          sec.enabled ? eval(sec.angle, s) * pi / 180 : 0;
      const double sc = std::cos(secondAngle), ss = std::sin(secondAngle);
      std::array<std::pair<double, double>, 64> secondaryOffsets{};
      if (sec.enabled)
        for (int i = 0; i < sec.count; ++i)
          secondaryOffsets[i] = {(random() * 2 - 1) * sec.scatter,
                                 (random() * 2 - 1) * sec.scatter};
      const double secondOpacity = sec.enabled ? unit(eval(sec.opacity, s)) : 1;
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
          double dx = (x + .5 - cx) / radius, dy = (y + .5 - cy) / radius;
          double u = (dx * c + dy * sn), v = (-dx * sn + dy * c) / aspect;
          if (p.flipX) u = -u;
          if (p.flipY) v = -v;
          const double fu = p.flipX ? -1 : 1, fv = p.flipY ? -1 : 1;
          const double ux = fu * c / radius, uy = fu * sn / radius,
                       vx = -fv * sn / (radius * aspect),
                       vy = fv * c / (radius * aspect);
          Pixel tip = filteredTip(p.tip, primaryIndex, u, v, ux, uy, vx, vy);
          double a  = tip.a;
          if (a <= 0) continue;
          if (t.image) {
            double tx = (t.perDab ? x + .5 - cx : x + .5) + t.offsetX,
                   ty = (t.perDab ? y + .5 - cy : y + .5) + t.offsetY;
            double b =
                texel(*t.image,
                      (tx * tc + ty * ts) / (t.image->width * t.scale),
                      (-tx * ts + ty * tc) / (t.image->height * t.scale), true)
                    .a;
            b = unit((b - .5) * t.contrast + .5 + t.brightness);
            if (t.invert) b = 1 - b;
            a = mask(a,
                     t.mode == MaskMode::Multiply ? 1 - t.depth + t.depth * b
                                                  : t.depth * b,
                     t.mode);
          }
          if (sec.enabled) {
            double b        = 0;
            const double fx = sec.flipX ? -1 : 1, fy = sec.flipY ? -1 : 1;
            // Compose secondary coverage before applying the chosen mask
            // operation once. Count one preserves the original native result.
            for (int i = 0; i < sec.count; ++i) {
              double su = (u - secondaryOffsets[i].first) / secondSize,
                     sv = (v - secondaryOffsets[i].second) / secondSize;
              double coverage =
                  filteredTip(
                      sec.tip, secondIndex, fx * (su * sc + sv * ss),
                      fy * (-su * ss + sv * sc) / sec.aspect,
                      fx * (ux * sc + vx * ss) / secondSize,
                      fx * (uy * sc + vy * ss) / secondSize,
                      fy * (-ux * ss + vx * sc) / (secondSize * sec.aspect),
                      fy * (-uy * ss + vy * sc) / (secondSize * sec.aspect))
                      .a *
                  secondOpacity;
              b += (1 - b) * coverage;
            }
            a = mask(a, b, sec.mode);
          }
          a *= unit(surface.coverage(x, y));
          if (a <= 0) continue;
          Pixel ink = paint;
          bool colorTip =
              p.tip.shape == Shape::Bitmap && p.tip.images[primaryIndex]->color;
          if (colorTip) ink = tip.a > 0 ? mul(tip, 1 / tip.a) : Pixel{};
          if (p.wet == Wet::Smear && !smear.empty()) {
            int ix = std::max(
                0, std::min(smearSide - 1, int((u * .5 + .5) * smearSide)));
            int iy = std::max(
                0, std::min(smearSide - 1, int((v * .5 + .5) * smearSide)));
            ink = mix(mix(paint, smear[size_t(iy) * smearSide + ix], pickup),
                      foreground, colorRate);
          }
          if (p.wash) {
            Tile *buf = tile(x, y);
            if (!buf) return false;
            size_t k = size_t(y % 32) * 32 + x % 32;
            buf->paint[k] =
                composite(buf->paint[k], mul(ink, a * flow), Blend::Normal);
            surface.write(x, y,
                          composite(buf->base[k], mul(buf->paint[k], opacity),
                                    p.blend, p.lockAlpha));
          } else
            surface.write(
                x, y,
                composite(surface.read(x, y), mul(ink, a * flow * opacity),
                          p.blend, p.lockAlpha));
        }
      if (p.wet != Wet::Off) {
        reservoir            = mix(reservoir, foreground, p.reload);
        size_t previousBytes = smear.size() * sizeof(Pixel);
        shared->release(previousBytes);
        reserved -= previousBytes;
        smear     = std::move(nextSmear);
        smearSide = side;
      }
    }
    ++dabs;
    return true;
  }
};

Stroke::Stroke(Surface &s, const Preset &p, size_t limit,
               std::shared_ptr<MemoryBudget> shared)
    : m(new Impl(s, p, limit, std::move(shared))) {}
Stroke::~Stroke() = default;
bool Stroke::add(const Sample &s) {
  if (m->finished || !m->failure.empty()) return false;
  if (!std::isfinite(s.x) || !std::isfinite(s.y) || std::abs(s.x) > 1e8 ||
      std::abs(s.y) > 1e8 || !std::isfinite(s.time) ||
      !std::isfinite(s.pressure) || !std::isfinite(s.tiltX) ||
      !std::isfinite(s.tiltY) || !std::isfinite(s.rotation)) {
    m->failure = "Invalid tablet sample";
    return false;
  }
  try {
    if (!m->started) {
      m->first = m->previous = s;
      m->started             = true;
      return m->dab(s);
    }
    if (s.time < m->previous.time) {
      m->failure = "Tablet timestamps moved backwards";
      return false;
    }
    const Sample from = m->previous;
    double distance   = std::hypot(s.x - from.x, s.y - from.y),
           dt         = s.time - from.time;
    if (m->pendingSourceEndpoint && distance > 0) {
      if (!m->dab(from)) return false;
      m->pendingSourceEndpoint = false;
    }
    if (distance > 0) {
      m->direction = std::atan2(s.y - from.y, s.x - from.x);
      if (!m->hasInitialDirection) {
        m->initialDirection    = m->direction;
        m->hasInitialDirection = true;
      }
    }
    m->speed = dt > 0 ? distance / dt : 0;
    double f = 0;
    while (f < 1) {
      double fd   = distance > 0 ? (m->spacing - m->sinceDab) / distance : 2;
      double ft   = m->p.airbrushRate > 0 && dt > 0
                        ? (1 / m->p.airbrushRate - m->sinceTime) / dt
                        : 2;
      double step = std::max(1e-9, std::min(fd, ft));
      const double end = f + step;
      const bool sourceEndpoint =
          sourceProfile(m->p.profile) && std::abs(end - 1) <= 1e-12;
      if (end > 1 && !sourceEndpoint) break;
      m->travel += (sourceEndpoint ? 1 - f : step) * distance;
      f = sourceEndpoint ? 1 : end;
      if (sourceEndpoint) {
        // The measured source omits a dab exactly at the final endpoint.
        // Defer it until further movement so splitting the same path into
        // input events does not lose interior spacing-boundary dabs.
        // Decimal spacing accumulates roundoff over hundreds of dabs. Treat
        // a normalized endpoint within 1e-12 as the exact boundary, without
        // altering the native scheduler or snapping interior dab positions.
        m->pendingSourceEndpoint = true;
        m->sinceDab = m->sinceTime = 0;
        break;
      }
      if (!m->dab(interpolate(from, s, f))) return false;
      m->sinceDab  = 0;
      m->sinceTime = 0;
    }
    m->travel += (1 - f) * distance;
    m->sinceDab += (1 - f) * distance;
    m->sinceTime += (1 - f) * dt;
    m->previous = s;
    return true;
  } catch (const std::bad_alloc &) {
    m->failure = "Brush allocation failed";
    return false;
  }
}
void Stroke::finish() {
  m->finished              = true;
  m->pendingSourceEndpoint = false;
  m->tiles.clear();
  m->cubic.clear();
  std::vector<Pixel>().swap(m->smear);
  m->shared->release(m->reserved);
  m->reserved = 0;
}
const std::string &Stroke::error() const { return m->failure; }
size_t Stroke::dabCount() const { return m->dabs; }
}  // namespace RasterBrush
