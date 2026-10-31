#include "toonz/rasterbrushengine.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <stdexcept>

namespace RasterBrush {
namespace {
constexpr double pi = 3.14159265358979323846;
bool sourceWetProfile(const std::string &profile) {
  return profile == "krita-5.3.4.1-legacy-smear-candidate-1" ||
         profile == "krita-5.3.4.1-legacy-dulling-candidate-1" ||
         profile == "krita-5.3.4.1-new-smear-candidate-1" ||
         profile == "krita-5.3.4.1-new-dulling-candidate-1";
}
bool sourceDefaultProfile(const std::string &profile) {
  return profile == "krita-5.3.4.1-default-circle-candidate-1" ||
         profile == "krita-5.3.4.1-default-rect-candidate-1";
}
bool sourcePixelProfile(const std::string &profile) {
  return profile == "krita-5.3.4.1-hard-pixel-candidate-1" ||
         sourceDefaultProfile(profile);
}
bool sourceProfile(const std::string &profile) {
  return sourcePixelProfile(profile) || sourceWetProfile(profile);
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
Pixel fieldSample(const std::vector<Pixel> &field, int side, double u,
                  double v) {
  const double x =
      std::max(0.0, std::min(double(side - 1), (u * .5 + .5) * side - .5));
  const double y =
      std::max(0.0, std::min(double(side - 1), (v * .5 + .5) * side - .5));
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const int nx = std::min(ix + 1, side - 1), ny = std::min(iy + 1, side - 1);
  return mix(
      mix(field[size_t(iy) * side + ix], field[size_t(iy) * side + nx], x - ix),
      mix(field[size_t(ny) * side + ix], field[size_t(ny) * side + nx], x - ix),
      y - iy);
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
Pixel sourceHsvTransform(Pixel color, const std::array<bool, 3> &enabled,
                         const std::array<double, 3> &values) {
  if (!enabled[0] && !enabled[1] && !enabled[2]) return color;
  if (enabled[0]) color = hsv(color, 360 * (values[0] - .5), 1, 1);
  auto byte = [](double c) { return std::floor(unit(c) * 255 + .5) / 255; };
  if (!enabled[1]) {
    // Preserve direct affine RGB math for H/V-only byte ties.
    if (enabled[2]) {
      const double d = 2 * values[2] - 1;
      auto adjust    = [d](double c) {
        return d <= 0 ? c * (1 + d) : c + d * (1 - c);
      };
      color = {adjust(color.r), adjust(color.g), adjust(color.b), 1};
    }
    return {byte(color.r), byte(color.g), byte(color.b), 1};
  }
  // Independently measured chroma-domain adjustment, not native HSV scaling.
  // Expand original chroma, cap its intermediate range at one encoded unit,
  // apply Value to maximum/chroma, then cap final chroma to final maximum.
  // This preserves hue without clamping individual negative intermediates.
  const double r = color.r * 255, g = color.g * 255, b = color.b * 255;
  const double originalHigh = std::max({r, g, b}), low = std::min({r, g, b});
  const double originalChroma = originalHigh - low;
  const double d              = 2 * values[1] - 1;
  double chroma =
      std::min(255.0, originalChroma * (d <= 0 ? 1 + d : 1 + d + 2 * d * d));
  double high = originalHigh;
  if (enabled[2]) {
    const double v = 2 * values[2] - 1;
    high           = v <= 0 ? high * (1 + v) : high + v * (255 - high);
    chroma *= v <= 0 ? 1 + v : 1 - v;
  }
  chroma              = std::min(high, chroma);
  const double factor = originalChroma > 0 ? chroma / originalChroma : 0;
  auto adjust         = [high, originalHigh, factor](double c) {
    return std::floor(std::max(0.0, high - (originalHigh - c) * factor) + .5) /
           255;
  };
  return {adjust(r), adjust(g), adjust(b), 1};
}
Pixel sourceHsvColor(Pixel color, SourceHsvChannel channel, double value) {
  std::array<bool, 3> enabled{};
  std::array<double, 3> values{.5, .5, .5};
  if (channel != SourceHsvChannel::Off) {
    enabled[int(channel) - 1] = true;
    values[int(channel) - 1]  = value;
  }
  return sourceHsvTransform(color, enabled, values);
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
  // A tiny/anisotropic secondary tip can map a valid canvas position far
  // outside the bitmap. Reject outside bilinear support before converting to
  // integers; those coordinates need not fit the platform's integer range.
  if (!repeat && (fx <= -1 || fy <= -1 || fx >= im.width || fy >= im.height))
    return {};
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

namespace {
// Candidate Pixel blending is a separate profile boundary. These encoded-sRGB
// separable equations follow the public blend definitions; source byte rounding
// and wash/build-up equivalence require the original pixelblend reference
// gates. Keeping this separate allows measured source arithmetic to evolve
// without changing native painting or the four Normal-only Color Smudge
// profiles.
Pixel sourcePixelComposite(Pixel dst, Pixel src, Blend mode) {
  if (mode == Blend::Normal) return composite(dst, src, Blend::Normal);
  dst                  = clean(dst);
  src                  = clean(src);
  const double overlap = src.a * dst.a;
  auto component       = [&](double d, double s) {
    const double backdrop   = dst.a > 0 ? d / dst.a : 0;
    const double foreground = src.a > 0 ? s / src.a : 0;
    double blend            = 0;
    switch (mode) {
    case Blend::Multiply:
      blend = backdrop * foreground;
      break;
    case Blend::Screen:
      blend = backdrop + foreground - backdrop * foreground;
      break;
    case Blend::Overlay:
      blend = backdrop <= .5 ? 2 * backdrop * foreground
                                   : 1 - 2 * (1 - backdrop) * (1 - foreground);
      break;
    default:
      return 0.0;  // validate rejects non-Pixel modes before rendering.
    }
    return (1 - src.a) * d + (1 - dst.a) * s + overlap * blend;
  };
  return clean({component(dst.r, src.r), component(dst.g, src.g),
                component(dst.b, src.b), src.a + dst.a * (1 - src.a)});
}
}  // namespace

std::string validate(const Preset &p) {
  const bool sourceHard = sourcePixelProfile(p.profile);
  const bool sourceSoft = sourceDefaultProfile(p.profile);
  const bool sourceRect = p.profile == "krita-5.3.4.1-default-rect-candidate-1";
  const bool sourceWet  = sourceWetProfile(p.profile);
  if (p.profile != "groundzero-native-1" && !sourceHard && !sourceWet)
    return "Unimplemented behavior profile: " + p.profile;
  if (p.varySeedPerStroke && p.profile != "groundzero-native-1")
    return "Per-stroke seed variation requires the native profile";
  auto linearPressure = [](const Dynamic &value) {
    if (value.minimum != 0 || value.mappings.size() != 1) return false;
    const auto &mapping = value.mappings.front();
    return mapping.sensor == Sensor::Pressure && mapping.scale == 1 &&
           mapping.interpolation == Interpolation::Linear &&
           mapping.curve.size() == 2 && mapping.curve[0].x == 0 &&
           mapping.curve[0].y == 0 && mapping.curve[1].x == 1 &&
           mapping.curve[1].y == 1;
  };
  // Independently captured nonuniform monotone/turning pressure-size masks
  // establish natural cubic interpolation beyond the affine two-knot case.
  // Wet pressure qualification remains the separate identity-only predicate.
  auto sourcePressureCurve = [](const Dynamic &value) {
    if (value.minimum != 0 || value.mappings.size() != 1) return false;
    const auto &mapping = value.mappings.front();
    return mapping.sensor == Sensor::Pressure && mapping.scale == 1 &&
           mapping.curve.size() >= 2 && mapping.curve.front().x == 0 &&
           mapping.curve.back().x == 1 &&
           ((mapping.curve.size() == 2 &&
             mapping.interpolation == Interpolation::Linear) ||
            (mapping.curve.size() > 2 &&
             mapping.interpolation == Interpolation::NaturalCubic));
  };
  const bool sourcePressureSize = sourceHard && sourcePressureCurve(p.size);
  const bool sourceBarrel =
      sourceHard && !sourceSoft && p.angle.value == 360 &&
      p.angle.minimum == 0 && p.angle.mappings.size() == 1 &&
      (p.angle.mappings.front().sensor == Sensor::Rotation ||
       p.angle.mappings.front().sensor == Sensor::TiltX ||
       p.angle.mappings.front().sensor == Sensor::TiltY) &&
      p.angle.mappings.front().scale == 1 &&
      p.angle.mappings.front().interpolation == Interpolation::Linear &&
      p.angle.mappings.front().curve.size() == 2 &&
      p.angle.mappings.front().curve[0].x == 0 &&
      p.angle.mappings.front().curve[0].y == 0 &&
      p.angle.mappings.front().curve[1].x == 1 &&
      p.angle.mappings.front().curve[1].y == 1;
  if (sourceBarrel && (p.wash || p.blend != Blend::Normal || p.texture.image ||
                       !p.size.mappings.empty() || p.opacity.value != 1 ||
                       !p.opacity.mappings.empty() || p.flow.value != 1 ||
                       !p.flow.mappings.empty()))
    return "Unimplemented source barrel rotation combination";
  const bool extendedTransfer =
      sourceHard &&
      ((!p.opacity.mappings.empty() && !linearPressure(p.opacity)) ||
       (!p.flow.mappings.empty() && !linearPressure(p.flow)));
  if (extendedTransfer &&
      (p.blend != Blend::Normal || p.texture.image ||
       !p.size.mappings.empty() || !p.angle.mappings.empty() ||
       (!p.opacity.mappings.empty() &&
        (p.flow.value != 1 || !p.flow.mappings.empty())) ||
       (!p.flow.mappings.empty() && p.opacity.value != 1)))
    return "Unimplemented source transfer curve combination";
  if (sourceHard || sourceWet) {
    auto fixed = [](const Dynamic &value, double expected) {
      return value.value == expected && value.mappings.empty();
    };
    if (p.tip.shape != (sourceRect ? Shape::Square : Shape::Round) ||
        (!sourceSoft && p.tip.hardness != 1) ||
        (sourceSoft &&
         ((!sourceRect && !p.tip.antialiasEdges && p.tip.hardness >= 1) ||
          (p.tip.antialiasEdges && p.size.value < 16 &&
           !(!sourceRect && p.tip.hardness == 1)) ||
          !p.size.mappings.empty() || !fixed(p.aspect, 1) ||
          !fixed(p.angle, 0))) ||
        (!sourceSoft && p.tip.antialiasEdges) || !p.tip.images.empty() ||
        (!p.size.mappings.empty() && !sourcePressureSize) ||
        p.size.value != std::floor(p.size.value) ||
        !p.aspect.mappings.empty() ||
        (!p.angle.mappings.empty() && !sourceBarrel) ||
        (sourceWet && (!fixed(p.aspect, 1) || !fixed(p.angle, 0))) ||
        (!p.opacity.mappings.empty() &&
         !((sourceWet && linearPressure(p.opacity)) ||
           (sourceHard && !sourceSoft && sourcePressureCurve(p.opacity)))) ||
        (!p.flow.mappings.empty() &&
         !((sourceWet && linearPressure(p.flow)) ||
           (sourceHard && !sourceSoft && sourcePressureCurve(p.flow)))) ||
        !fixed(p.scatter, 0) || !fixed(p.countControl, 1) ||
        !fixed(p.backgroundMix, 0) || !fixed(p.hue, 0) ||
        !fixed(p.saturation, 1) || !fixed(p.brightness, 1) || p.count != 1 ||
        p.airbrushRate != 0 || (sourceWet && p.wash) || p.lockAlpha ||
        p.flipX || p.flipY ||
        (sourceWet ? p.blend != Blend::Normal : p.blend == Blend::Erase) ||
        (sourceHard && p.wet != Wet::Off) || p.secondary.enabled ||
        p.color.a != 1)
      return "Unimplemented setting in the experimental Krita source profile";
  }
  const auto &maskSettings = p.sourceMasked;
  for (double value : {maskSettings.diameter, maskSettings.masterSizeCoeff,
                       maskSettings.aspect, maskSettings.spacing,
                       maskSettings.opacity, maskSettings.flow})
    if (!std::isfinite(value)) return "Invalid source mask scalar";
  if (maskSettings.diameter < 1 || maskSettings.diameter > 4096 ||
      maskSettings.masterSizeCoeff <= 0 || maskSettings.masterSizeCoeff > 16 ||
      maskSettings.aspect <= 0 || maskSettings.aspect > 1 ||
      maskSettings.spacing < .01 || maskSettings.spacing > 5 ||
      maskSettings.opacity < 0 || maskSettings.opacity > 1 ||
      maskSettings.flow < 0 || maskSettings.flow > 1 ||
      (maskSettings.mode != MaskMode::Multiply &&
       maskSettings.mode != MaskMode::Subtract))
    return "Invalid source mask settings";
  if (maskSettings.enabled) {
    const double diameter = maskSettings.useMasterSize
                                ? p.size.value * maskSettings.masterSizeCoeff
                                : maskSettings.diameter;
    if (p.profile != "krita-5.3.4.1-hard-pixel-candidate-1" || !p.wash ||
        p.tip.shape != Shape::Round || p.tip.hardness != 1 ||
        p.tip.antialiasEdges || !p.size.mappings.empty() ||
        p.aspect.value != 1 || p.angle.value != 0 ||
        !p.angle.mappings.empty() || !p.opacity.mappings.empty() ||
        p.flow.value != 1 || !p.flow.mappings.empty() || p.texture.image ||
        p.blend != Blend::Normal || p.secondary.enabled ||
        p.sourceHsvChannel != SourceHsvChannel::Off ||
        p.sourceHsvV2[0].enabled || p.sourceHsvV2[1].enabled ||
        p.sourceHsvV2[2].enabled || diameter < 1 || diameter > 4096 ||
        diameter != std::floor(diameter))
      return "Unimplemented source masked brush combination";
  }
  if (int(p.sourceHsvChannel) < 0 || int(p.sourceHsvChannel) > 3 ||
      !std::isfinite(p.sourceHsvValue) || p.sourceHsvValue < 0 ||
      p.sourceHsvValue > 1)
    return "Invalid source HSV setting";
  int hsvV2Active = 0, hsvV2Dynamic = 0;
  for (size_t channel = 0; channel < p.sourceHsvV2.size(); ++channel) {
    const auto &setting = p.sourceHsvV2[channel];
    if (setting.enabled) {
      ++hsvV2Active;
      if (setting.control.mappings.empty()) {
        if (!std::isfinite(setting.control.value) ||
            setting.control.value < 0 || setting.control.value > 1 ||
            setting.control.minimum != 0)
          return "Invalid fixed source HSV V2 control";
      } else {
        ++hsvV2Dynamic;
        if (setting.control.value != 1 || !linearPressure(setting.control))
          return "Unimplemented source HSV V2 dynamic control";
      }
    } else if (setting.control.value != .5 || setting.control.minimum != 0 ||
               !setting.control.mappings.empty())
      return "Inactive source HSV V2 control must be neutral";
  }
  if ((hsvV2Dynamic && hsvV2Active > 1) ||
      (hsvV2Active &&
       (p.sourceHsvChannel != SourceHsvChannel::Off || p.sourceHsvValue != .5)))
    return "Conflicting source HSV versions or unimplemented composition";
  const bool staticWetHsv =
      sourceWet && !hsvV2Active && p.sourceSmearAlpha && p.smudgeRadius == 0 &&
      p.pickupControl.mappings.empty() && p.colorRateControl.mappings.empty();
  if ((p.sourceHsvChannel != SourceHsvChannel::Off || hsvV2Active) &&
      ((p.profile != "krita-5.3.4.1-hard-pixel-candidate-1" && !staticWetHsv) ||
       p.texture.image || p.wash || p.tip.shape != Shape::Round ||
       p.tip.hardness != 1 || p.tip.antialiasEdges ||
       p.blend != Blend::Normal || p.opacity.value != 1 ||
       !p.opacity.mappings.empty() || p.flow.value != 1 ||
       !p.flow.mappings.empty() || !p.size.mappings.empty() ||
       p.aspect.value != 1 || p.angle.value != 0))
    return "Unimplemented source HSV setting combination";
  if ((sourceHard || sourceWet) && p.texture.image &&
      (p.tip.antialiasEdges || p.tip.hardness != 1 ||
       p.tip.shape != Shape::Round || p.blend != Blend::Normal || p.wash ||
       p.texture.scale != 1 || p.texture.angle != 0 || p.texture.offsetX != 0 ||
       p.texture.offsetY != 0 || p.texture.perDab ||
       p.texture.brightness != 0 || p.texture.contrast != 1 ||
       p.texture.image->color ||
       (sourceWet && (!p.sourceSmearAlpha || p.smudgeRadius != 0 ||
                      p.profile == "krita-5.3.4.1-new-dulling-candidate-1"))))
    return "Unimplemented source texture geometry or wet-state combination";
  if (sourceWet &&
      (p.wet != ((p.profile == "krita-5.3.4.1-legacy-dulling-candidate-1" ||
                  p.profile == "krita-5.3.4.1-new-dulling-candidate-1")
                     ? Wet::Dulling
                     : Wet::Smear) ||
       (p.profile == "krita-5.3.4.1-legacy-dulling-candidate-1"
            ? (p.smudgeRadius < 0 || p.smudgeRadius > 1)
            : (p.profile == "krita-5.3.4.1-new-dulling-candidate-1"
                   ? (p.smudgeRadius < 0 || p.smudgeRadius > 3)
                   : p.smudgeRadius != 0)) ||
       p.reload != 0 || p.pickupControl.value != 1 ||
       (!p.pickupControl.mappings.empty() &&
        !linearPressure(p.pickupControl)) ||
       p.colorRateControl.value != 1 ||
       (!p.colorRateControl.mappings.empty() &&
        !linearPressure(p.colorRateControl)) ||
       p.radiusControl.value != 1 || !p.radiusControl.mappings.empty()))
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
       !range(s.aspect, .01, 1) || !range(s.scatter, 0, 5) ||
       !range(s.spacing, .01, 5) || s.count < 1 || s.count > 64))
    return "Invalid secondary tip";
  if (s.enabled && s.independentSpacing &&
      (p.profile != "groundzero-native-1" ||
       p.size.value * s.size.value > 4096))
    return "Independent secondary masks require native geometry up to 4096 "
           "pixels";
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
  ~Impl() {
    tiles.clear();
    budget->release(reserved);
  }
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
            bottom        = (y1 - 1) / 32;
  constexpr size_t charge = sizeof(Impl::Tile) + 128;
  // Reserve every missing float tile before the host allocates undo or reads
  // the enlarged wet-feedback region. Count the rectangle arithmetically so
  // an enormous request cannot itself create an unbounded preflight loop.
  std::uint64_t missing =
      std::uint64_t(right - left + 1) * std::uint64_t(bottom - top + 1);
  if (missing <= m->tiles.size()) {
    // Ordinary small dabs must not scan the whole accumulated stroke cache.
    for (int ty = top; ty <= bottom; ++ty)
      for (int tx = left; tx <= right; ++tx)
        if (m->tiles.count({tx, ty})) --missing;
  } else {
    for (const auto &tile : m->tiles)
      if (tile.first.first >= left && tile.first.first <= right &&
          tile.first.second >= top && tile.first.second <= bottom)
        --missing;
  }
  if (missing > std::numeric_limits<size_t>::max() / charge) return false;
  size_t pending = size_t(missing) * charge;
  if (!m->budget->reserve(pending)) return false;
  try {
    // Avoid overflow when rounding a canvas near INT_MAX to a tile edge.
    const int endX = right * 32 + std::min(32, width() - right * 32);
    const int endY = bottom * 32 + std::min(32, height() - bottom * 32);
    if (!m->backing->prepare(left * 32, top * 32, endX, endY)) {
      m->budget->release(pending);
      return false;
    }
    for (int ty = top; ty <= bottom; ++ty)
      for (int tx = left; tx <= right; ++tx) {
        auto key = std::make_pair(tx, ty);
        if (m->tiles.count(key)) continue;
        auto tile = std::make_unique<Impl::Tile>();
        for (int y = 0; y < 32 && ty * 32 + y < height(); ++y)
          for (int x = 0; x < 32 && tx * 32 + x < width(); ++x)
            (*tile)[y * 32 + x] = m->backing->read(tx * 32 + x, ty * 32 + y);
        m->tiles.emplace(key, std::move(tile));
        m->reserved += charge;
        pending -= charge;
      }
  } catch (const std::bad_alloc &) {
    m->budget->release(pending);
    return false;
  } catch (...) {
    m->budget->release(pending);
    throw;
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
  Preset p;
  size_t budget, dabs = 0;
  std::shared_ptr<MemoryBudget> shared;
  size_t reserved = 0, inputPixelWork = 0;
  std::string failure;
  Sample previous, first, previousSourceDab;
  bool started = false, finished = false;
  bool pendingSourceEndpoint = false;
  bool pendingMaskPrimary = false, pendingMaskSecondary = false;
  struct SourcePoint {
    Sample sample;
    double distance;
  };
  std::vector<SourcePoint> sourcePath;
  double sourceInputDistance = 0, sourceNextDab = 0;
  bool sourcePressureLookahead = false;
  double sourceFuturePressure  = 1;
  bool sourceFutureHasPressure = false;
  bool hasInitialDirection     = false;
  double travel = 0, sinceDab = 0, sinceTime = 0, spacing = 1, direction = 0,
         speed = 0, initialDirection = 0;
  std::uint64_t rng, secondaryRng;
  size_t secondaryDabs      = 0;
  bool secondaryEvaluation  = false;
  double secondaryRandomDab = 0, sinceSecondary = 0, secondarySpacing = 1;
  using MaskTile = std::array<double, 1024>;
  std::map<std::pair<int, int>, std::unique_ptr<MaskTile>> secondaryMask;
  double randomStroke = 0, randomDab = 0;
  Pixel reservoir;
  std::vector<Pixel> smear, sourceField;
  using SourceTile = std::array<Pixel, 1024>;
  std::map<std::pair<int, int>, std::unique_ptr<SourceTile>> sourceOutside;
  int sourceX = 0, sourceY = 0, sourceW = 0, sourceH = 0;
  int smearSide = 0;
  std::map<const Mapping *, std::vector<double>> cubic;
  struct Tile {
    std::array<Pixel, 1024> base{}, paint{};
  };
  std::map<std::pair<int, int>, std::unique_ptr<Tile>> tiles;
  Impl(Surface &s, const Preset &v, size_t limit,
       std::shared_ptr<MemoryBudget> memory)
      : surface(s)
      , budget(limit)
      , shared(memory ? std::move(memory)
                      : std::make_shared<MemoryBudget>(limit))
      , rng(v.seed ? v.seed : 1)
      , secondaryRng((v.seed ^ UINT64_C(0x9e3779b97f4a7c15))
                         ? (v.seed ^ UINT64_C(0x9e3779b97f4a7c15))
                         : 1)
      , reservoir(v.color) {
    failure      = validate(v);
    randomStroke = random();
    if (!failure.empty()) return;
    try {
      // The stroke owns a stable settings snapshot. Charge its copied strings,
      // mapping arrays, knots and resource-reference arrays before copying;
      // image pixel storage itself stays shared with the installed preset.
      size_t snapshot = sizeof(Impl);
      auto account    = [&](size_t count, size_t element) {
        if (count > (budget - std::min(budget, snapshot)) / element ||
            snapshot > budget || budget - snapshot - count * element < 128)
          return false;
        snapshot += count * element + 128;
        return true;
      };
      bool fits =
          account(v.name.size() + 1, 1) && account(v.profile.size() + 1, 1) &&
          account(v.tip.images.size(), sizeof(std::shared_ptr<const Image>)) &&
          account(v.secondary.tip.images.size(),
                  sizeof(std::shared_ptr<const Image>));
      for (const Dynamic *dynamic : {&v.size,
                                     &v.aspect,
                                     &v.angle,
                                     &v.opacity,
                                     &v.flow,
                                     &v.scatter,
                                     &v.countControl,
                                     &v.pickupControl,
                                     &v.colorRateControl,
                                     &v.radiusControl,
                                     &v.backgroundMix,
                                     &v.hue,
                                     &v.saturation,
                                     &v.brightness,
                                     &v.secondary.size,
                                     &v.secondary.angle,
                                     &v.secondary.opacity,
                                     &v.sourceHsvV2[0].control,
                                     &v.sourceHsvV2[1].control,
                                     &v.sourceHsvV2[2].control}) {
        if (!dynamic->mappings.empty())
          fits = fits && account(dynamic->mappings.size(), sizeof(Mapping));
        for (const auto &mapping : dynamic->mappings)
          fits = fits && account(mapping.curve.size(), sizeof(Knot));
      }
      if (!fits || snapshot > budget || !shared->reserve(snapshot)) {
        failure = "Preset snapshot memory limit exceeded";
        return;
      }
      reserved += snapshot;
      p = v;
      for (const Dynamic *dynamic : {&p.size,
                                     &p.aspect,
                                     &p.angle,
                                     &p.opacity,
                                     &p.flow,
                                     &p.scatter,
                                     &p.countControl,
                                     &p.pickupControl,
                                     &p.colorRateControl,
                                     &p.radiusControl,
                                     &p.backgroundMix,
                                     &p.hue,
                                     &p.saturation,
                                     &p.brightness,
                                     &p.secondary.size,
                                     &p.secondary.angle,
                                     &p.secondary.opacity,
                                     &p.sourceHsvV2[0].control,
                                     &p.sourceHsvV2[1].control,
                                     &p.sourceHsvV2[2].control}) {
        for (const auto &mapping : dynamic->mappings) {
          if (mapping.interpolation != Interpolation::NaturalCubic) continue;
          size_t n      = mapping.curve.size(),
                 charge = 2 * n * sizeof(double) + 128;
          if (charge > budget - reserved || !shared->reserve(charge)) {
            failure = "Curve memory limit exceeded";
            releaseStorage();
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
        }
      }
    } catch (const std::bad_alloc &) {
      failure = "Preset or curve allocation failed";
      releaseStorage();
    }
  }
  void releasePresetStorage() {
    for (Dynamic *dynamic : {&p.size,
                             &p.aspect,
                             &p.angle,
                             &p.opacity,
                             &p.flow,
                             &p.scatter,
                             &p.countControl,
                             &p.pickupControl,
                             &p.colorRateControl,
                             &p.radiusControl,
                             &p.backgroundMix,
                             &p.hue,
                             &p.saturation,
                             &p.brightness,
                             &p.secondary.size,
                             &p.secondary.angle,
                             &p.secondary.opacity,
                             &p.sourceHsvV2[0].control,
                             &p.sourceHsvV2[1].control,
                             &p.sourceHsvV2[2].control})
      std::vector<Mapping>().swap(dynamic->mappings);
    std::vector<std::shared_ptr<const Image>>().swap(p.tip.images);
    std::vector<std::shared_ptr<const Image>>().swap(p.secondary.tip.images);
    p.texture.image.reset();
    std::string().swap(p.name);
    std::string().swap(p.profile);
  }
  void releaseStorage() {
    tiles.clear();
    secondaryMask.clear();
    sourceOutside.clear();
    std::vector<SourcePoint>().swap(sourcePath);
    cubic.clear();
    releasePresetStorage();
    std::vector<Pixel>().swap(smear);
    std::vector<Pixel>().swap(sourceField);
    shared->release(reserved);
    reserved = 0;
  }
  ~Impl() { releaseStorage(); }
  double random() {
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return double((rng * UINT64_C(2685821657736338717)) >> 11) *
           (1.0 / 9007199254740992.0);
  }
  double secondaryRandom() {
    secondaryRng ^= secondaryRng >> 12;
    secondaryRng ^= secondaryRng << 25;
    secondaryRng ^= secondaryRng >> 27;
    return double((secondaryRng * UINT64_C(2685821657736338717)) >> 11) *
           (1.0 / 9007199254740992.0);
  }
  bool independentSecondary() const {
    return p.sourceMasked.enabled ||
           (p.secondary.enabled && p.secondary.independentSpacing);
  }
  bool needsSourcePressureLookahead() const {
    return !p.size.mappings.empty() ||
           std::any_of(p.sourceHsvV2.begin(), p.sourceHsvV2.end(),
                       [](const SourceHsvControl &control) {
                         return control.enabled &&
                                !control.control.mappings.empty();
                       });
  }
  Sample sourcePressureSample(const Sample &geometric) const {
    Sample value = geometric;
    if (sourcePressureLookahead) {
      value.pressure    = sourceFuturePressure;
      value.hasPressure = sourceFutureHasPressure;
    }
    return value;
  }
  bool appendSourcePoint(const Sample &s, double distance) {
    // Dense device input can place arbitrarily many samples in half a pixel.
    // Bound retained history and charge reallocation peak before allocating.
    if (sourcePath.size() >= 4096) {
      failure = "Source input history limit exceeded";
      return false;
    }
    if (sourcePath.size() == sourcePath.capacity()) {
      const size_t capacity = std::min(
          size_t(4096), std::max(size_t(16), sourcePath.capacity() * 2));
      const size_t charge = capacity * sizeof(SourcePoint);
      if (charge > budget - reserved || !shared->reserve(charge)) {
        failure = "Source input history memory limit exceeded";
        return false;
      }
      const size_t old = sourcePath.capacity() * sizeof(SourcePoint);
      try {
        sourcePath.reserve(capacity);
      } catch (const std::bad_alloc &) {
        shared->release(charge);
        throw;
      }
      shared->release(old);
      reserved += charge - old;
    }
    sourcePath.push_back({s, distance});
    return true;
  }
  Sample sourceSampleAt(double distance) const {
    auto upper =
        std::lower_bound(sourcePath.begin(), sourcePath.end(), distance,
                         [](const SourcePoint &point, double value) {
                           return point.distance < value;
                         });
    if (upper == sourcePath.begin()) return upper->sample;
    if (upper == sourcePath.end()) return sourcePath.back().sample;
    const auto &lower = *(upper - 1);
    return interpolate(
        lower.sample, upper->sample,
        (distance - lower.distance) / (upper->distance - lower.distance));
  }
  bool addSourcePixel(const Sample &s) {
    if (!started) {
      if (!appendSourcePoint(s, 0)) return false;
      first = previous = s;
      started          = true;
      // Size and the admitted HSV V2 channels have measured future-pressure
      // semantics. Transfer retains its geometric-position sample.
      if (!needsSourcePressureLookahead()) {
        if (!dab(s)) return false;
        sourceNextDab = spacing;
      }
      return true;
    }
    if (s.time < previous.time) {
      failure = "Tablet timestamps moved backwards";
      return false;
    }
    const double distance = std::hypot(s.x - previous.x, s.y - previous.y);
    if (distance == 0) {
      sourcePath.back().sample = s;
      previous                 = s;
      return true;
    }
    sourceInputDistance += distance;
    if (!appendSourcePoint(s, sourceInputDistance)) return false;
    size_t events = 0;
    // The future sample is available at equality too. Excluding that exact
    // half-pixel boundary drops the final stamp in small-tip source traces.
    while (sourceNextDab + .5 <= sourceInputDistance) {
      if (++events > 65536) {
        failure = "Stroke input event work limit exceeded";
        return false;
      }
      Sample geometric        = sourceSampleAt(sourceNextDab);
      const Sample future     = sourceSampleAt(sourceNextDab + .5);
      sourcePressureLookahead = true;
      sourceFuturePressure    = future.pressure;
      sourceFutureHasPressure = future.hasPressure;
      travel                  = sourceNextDab;
      if (!dab(geometric)) return false;
      sourcePressureLookahead = false;
      sourceNextDab += spacing;
    }
    // Preserve the endpoint preceding the next geometric sample and the
    // unconsumed half-pixel tail; do not resample away authored corner data.
    auto keep =
        std::upper_bound(sourcePath.begin(), sourcePath.end(), sourceNextDab,
                         [](double value, const SourcePoint &point) {
                           return value < point.distance;
                         });
    if (keep != sourcePath.begin()) --keep;
    sourcePath.erase(sourcePath.begin(), keep);
    previous = s;
    return true;
  }
  double sensor(const Mapping &m, const Sample &s) const {
    double v = 0;
    switch (m.sensor) {
    case Sensor::Pressure:
      v = s.hasPressure ? s.pressure : 1;
      break;
    case Sensor::TiltX:
      v = sourcePixelProfile(p.profile) ? (s.hasTilt ? std::abs(s.tiltX) : 0)
                                        : (s.hasTilt ? (s.tiltX + 1) * .5 : .5);
      break;
    case Sensor::TiltY:
      v = sourcePixelProfile(p.profile) ? (s.hasTilt ? std::abs(s.tiltY) : 0)
                                        : (s.hasTilt ? (s.tiltY + 1) * .5 : .5);
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
      v = secondaryEvaluation ? secondaryRandomDab : randomDab;
      break;
    case Sensor::RandomStroke:
      v = randomStroke;
      break;
    case Sensor::InitialDirection:
      v = wrap(initialDirection / (2 * pi));
      break;
    case Sensor::DabIndex:
      v = double(secondaryEvaluation ? secondaryDabs : dabs);
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
    if (charge > budget - reserved || !shared->reserve(charge)) {
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
    return ptr;
  }
  static size_t tipWork(const Tip &tip) {
    return tip.shape == Shape::Bitmap ? 4 : tip.antialiasEdges ? 64 : 1;
  }
  bool accountPixelWork(int width, int height, size_t samples = 1) {
    constexpr size_t limit = 256u * 1024u * 1024u;
    const size_t pixels    = size_t(width) * size_t(height);
    if (samples > limit || pixels > (limit - inputPixelWork) / samples) {
      failure = "Stroke input pixel work limit exceeded";
      return false;
    }
    inputPixelWork += pixels * samples;
    return true;
  }
  void recomposeSourceMask(int x, int y) {
    const auto key     = std::make_pair(x / 32, y / 32);
    const auto primary = tiles.find(key);
    if (primary == tiles.end()) return;
    const size_t k       = size_t(y % 32) * 32 + x % 32;
    const auto secondary = secondaryMask.find(key);
    const double retained =
        secondary == secondaryMask.end() ? 0 : (*secondary->second)[k];
    double coverage =
        std::floor(retained * p.sourceMasked.opacity * 255 + .5) / 255;
    if (p.sourceMasked.mode == MaskMode::Subtract) coverage = 1 - coverage;
    const auto &tile = *primary->second;
    surface.write(
        x, y,
        sourcePixelComposite(tile.base[k],
                             mul(tile.paint[k], coverage * p.opacity.value),
                             Blend::Normal));
  }
  bool sourceMaskDab(const Sample &s) {
    if (secondaryDabs >= 1000000) {
      failure = "Secondary stroke dab limit exceeded";
      return false;
    }
    const auto &settings  = p.sourceMasked;
    const double diameter = settings.useMasterSize
                                ? p.size.value * settings.masterSizeCoeff
                                : settings.diameter;
    const double radius = diameter * .5, halfHeight = radius * settings.aspect;
    secondarySpacing = diameter * settings.spacing;
    const int x0     = std::max(0, int(std::ceil(s.x - radius))),
              x1 = std::min(surface.width(), int(std::floor(s.x + radius)) + 1),
              y0 = std::max(0, int(std::ceil(s.y - halfHeight - .5))),
              y1 = std::min(surface.height(),
                            int(std::floor(s.y + halfHeight - .5)) + 1);
    if (x0 < x1 && y0 < y1) {
      // Both field updates and dirty recomposition are charged before writes.
      if (!accountPixelWork(x1 - x0, y1 - y0, 2)) return false;
      if (!surface.prepare(x0, y0, x1, y1)) {
        failure = "Raster transaction refused the source mask region";
        return false;
      }
      const double flow = std::floor(settings.flow * 255 + .5);
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
          const auto key = std::make_pair(x / 32, y / 32);
          auto found     = secondaryMask.find(key);
          if (found == secondaryMask.end()) {
            constexpr size_t charge = sizeof(MaskTile) + 128;
            if (charge > budget - reserved || !shared->reserve(charge)) {
              failure = "Source mask memory limit exceeded";
              return false;
            }
            reserved += charge;
            found =
                secondaryMask.emplace(key, std::make_unique<MaskTile>()).first;
          }
          double &alpha    = (*found->second)[size_t(y % 32) * 32 + x % 32];
          const double old = std::floor(alpha * 255 + .5);
          alpha = (old + std::floor((255 - old) * flow / 255 + .5)) / 255;
          recomposeSourceMask(x, y);
        }
    }
    ++secondaryDabs;
    return true;
  }
  bool secondaryDab(const Sample &s) {
    if (p.sourceMasked.enabled) return sourceMaskDab(s);
    if (secondaryDabs >= 1000000) {
      failure = "Secondary stroke dab limit exceeded";
      return false;
    }
    struct EvaluationScope {
      bool &flag;
      explicit EvaluationScope(bool &f) : flag(f) { flag = true; }
      ~EvaluationScope() { flag = false; }
    } evaluating(secondaryEvaluation);
    const auto &sec              = p.secondary;
    secondaryRandomDab           = secondaryRandom();
    const double primaryDiameter = std::max(.25, eval(p.size, s));
    const double relativeSize    = std::max(.01, eval(sec.size, s));
    const double diameter = std::max(.25, primaryDiameter * relativeSize);
    secondarySpacing      = diameter * sec.spacing;
    const double radius   = diameter * .5;
    const double aspect   = std::max(.01, eval(p.aspect, s));
    const double angle    = eval(p.angle, s) * pi / 180;
    const double c = std::cos(angle), sn = std::sin(angle);
    const double secondAngle = eval(sec.angle, s) * pi / 180;
    const double sc = std::cos(secondAngle), ss = std::sin(secondAngle);
    const double fu = p.flipX ? -1 : 1, fv = p.flipY ? -1 : 1;
    const double fx = sec.flipX ? -1 : 1, fy = sec.flipY ? -1 : 1;
    const double ux = fu * c / radius, uy = fu * sn / radius;
    const double vx = -fv * sn / (radius * aspect),
                 vy = fv * c / (radius * aspect);
    const double ax = fx * (ux * sc + vx * ss), ay = fx * (uy * sc + vy * ss),
                 bx      = fy * (-ux * ss + vx * sc) / sec.aspect,
                 by      = fy * (-uy * ss + vy * sc) / sec.aspect;
    const double opacity = unit(eval(sec.opacity, s));
    const size_t tipIndex =
        sec.tip.images.empty() ? 0
        : sec.tip.randomSequence
            ? size_t(secondaryRandom() * sec.tip.images.size()) %
                  sec.tip.images.size()
            : secondaryDabs % sec.tip.images.size();
    // Preflight the whole secondary batch, including filtering, before any
    // mask allocation or sampling. Scattered bounds are conservative.
    const double extent =
        std::ceil(radius * 1.415 + 1) + sec.scatter * diameter + 1;
    const int left   = std::max(0, int(std::floor(s.x - extent))),
              top    = std::max(0, int(std::floor(s.y - extent))),
              right  = std::min(surface.width(), int(std::ceil(s.x + extent))),
              bottom = std::min(surface.height(), int(std::ceil(s.y + extent)));
    if (right > left && bottom > top &&
        !accountPixelWork(right - left, bottom - top,
                          size_t(sec.count) * tipWork(sec.tip)))
      return false;
    for (int stamp = 0; stamp < sec.count; ++stamp) {
      const double cx =
          s.x + (secondaryRandom() * 2 - 1) * sec.scatter * diameter;
      const double cy =
          s.y + (secondaryRandom() * 2 - 1) * sec.scatter * diameter;
      const int reach = int(std::ceil(radius * 1.415 + 1));
      const int x0    = std::max(0, int(std::floor(cx)) - reach),
                y0    = std::max(0, int(std::floor(cy)) - reach),
                x1    = std::min(surface.width(), int(std::ceil(cx)) + reach),
                y1    = std::min(surface.height(), int(std::ceil(cy)) + reach);
      if (x0 >= x1 || y0 >= y1) continue;
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
          const double dx = x + .5 - cx, dy = y + .5 - cy;
          const double coverage =
              filteredTip(sec.tip, tipIndex, ax * dx + ay * dy,
                          bx * dx + by * dy, ax, ay, bx, by)
                  .a *
              opacity;
          if (coverage <= 0) continue;
          const auto key = std::make_pair(x / 32, y / 32);
          auto found     = secondaryMask.find(key);
          if (found == secondaryMask.end()) {
            constexpr size_t charge = sizeof(MaskTile) + 128;
            if (charge > budget - reserved || !shared->reserve(charge)) {
              failure = "Secondary mask memory limit exceeded";
              return false;
            }
            reserved += charge;
            found =
                secondaryMask.emplace(key, std::make_unique<MaskTile>()).first;
          }
          double &alpha = (*found->second)[(y % 32) * 32 + x % 32];
          alpha += (1 - alpha) * coverage;
        }
    }
    ++secondaryDabs;
    return true;
  }
  bool addIndependent(const Sample &from, const Sample &s, double distance,
                      double dt) {
    double f      = 0;
    size_t events = 0;
    while (f < 1) {
      const double primaryDistance =
          distance > 0 ? (spacing - sinceDab) / distance : 2;
      const double primaryTime = p.airbrushRate > 0 && dt > 0
                                     ? (1 / p.airbrushRate - sinceTime) / dt
                                     : 2;
      const double secondaryDistance =
          distance > 0 ? (secondarySpacing - sinceSecondary) / distance : 2;
      const double primary = std::min(primaryDistance, primaryTime);
      const double step = std::max(0.0, std::min(primary, secondaryDistance));
      if (f + step > 1 + 1e-12) break;
      const double advance = std::min(step, 1 - f);
      travel += advance * distance;
      sinceDab += advance * distance;
      sinceTime += advance * dt;
      sinceSecondary += advance * distance;
      f += advance;
      const Sample at = interpolate(from, s, f);
      if (p.sourceMasked.enabled && std::abs(f - 1) <= 1e-12) {
        pendingMaskSecondary = secondaryDistance <= step + 1e-12;
        pendingMaskPrimary   = primary <= step + 1e-12;
        if (pendingMaskSecondary) sinceSecondary = 0;
        if (pendingMaskPrimary) sinceDab = sinceTime = 0;
        break;
      }
      // Equal-position secondary stamps are visible to the primary dab.
      if (secondaryDistance <= step + 1e-12) {
        if (++events > 65536) {
          failure = "Stroke input event work limit exceeded";
          return false;
        }
        if (!secondaryDab(at)) return false;
        sinceSecondary = 0;
      }
      if (primary <= step + 1e-12) {
        if (++events > 65536) {
          failure = "Stroke input event work limit exceeded";
          return false;
        }
        if (!dab(at)) return false;
        sinceDab = sinceTime = 0;
      }
    }
    travel += (1 - f) * distance;
    sinceDab += (1 - f) * distance;
    sinceTime += (1 - f) * dt;
    sinceSecondary += (1 - f) * distance;
    previous = s;
    return true;
  }
  double sourceTextureCoverage(int x, int y, double coverage) const {
    const auto &t = p.texture;
    if (!t.image) return coverage;
    const int width = t.image->width, height = t.image->height;
    const int tx = (x % width + width) % width,
              ty = (y % height + height) % height;
    double gray  = t.image->pixels[size_t(ty) * width + tx].a;
    if (t.invert) gray = 1 - gray;
    const double alpha = std::floor(unit(coverage) * 255 + .5);
    const double value = std::floor(unit(gray) * 255 + .5);
    // Original grayscale step probes establish byte-domain texture coverage.
    // Soft texturing attenuates removed coverage; hard texturing attenuates
    // the incoming mask before the selected operation. Native math differs.
    double result;
    if (p.sourceTextureSoftTexturing)
      result = alpha - std::floor((t.mode == MaskMode::Multiply
                                       ? alpha * (255 - value) / 255
                                       : value) *
                                      t.depth +
                                  .5);
    else if (t.mode == MaskMode::Multiply)
      result = std::floor(alpha * value / 255 * t.depth + .5);
    else
      result = std::floor(alpha * t.depth + .5) - value;
    return unit(result / 255);
  }
  bool sourceInside(int x, int y) const {
    return x >= 0 && y >= 0 && x < surface.width() && y < surface.height();
  }
  static int sourceTileCoordinate(int x) {
    return x < 0 ? (x - 31) / 32 : x / 32;
  }
  Pixel sourceRead(int x, int y) const {
    if (sourceInside(x, y)) return surface.read(x, y);
    const int tx = sourceTileCoordinate(x), ty = sourceTileCoordinate(y);
    const auto found = sourceOutside.find({tx, ty});
    return found == sourceOutside.end()
               ? Pixel{}
               : (*found->second)[size_t(y - ty * 32) * 32 + (x - tx * 32)];
  }
  bool prepareSourceOutside(int x0, int y0, int x1, int y1) {
    const int tx0 = sourceTileCoordinate(x0), ty0 = sourceTileCoordinate(y0),
              tx1     = sourceTileCoordinate(x1 - 1),
              ty1     = sourceTileCoordinate(y1 - 1);
    const auto needed = [&](int tx, int ty) {
      return (tx * 32 < 0 || ty * 32 < 0 || (tx + 1) * 32 > surface.width() ||
              (ty + 1) * 32 > surface.height()) &&
             !sourceOutside.count({tx, ty});
    };
    constexpr size_t charge = sizeof(SourceTile) + 128;
    size_t count            = 0;
    for (int ty = ty0; ty <= ty1; ++ty)
      for (int tx = tx0; tx <= tx1; ++tx)
        if (needed(tx, ty)) ++count;
    if (count > (budget - reserved) / charge ||
        !shared->reserve(count * charge)) {
      failure = "Source off-canvas memory limit exceeded";
      return false;
    }
    reserved += count * charge;
    // All pixels and map nodes are charged before allocation or host writes.
    // On allocation failure the owning stroke retains the charge until its
    // partially allocated map is freed by finish/destruction.
    for (int ty = ty0; ty <= ty1; ++ty)
      for (int tx = tx0; tx <= tx1; ++tx)
        if (needed(tx, ty))
          sourceOutside[{tx, ty}] =
              std::unique_ptr<SourceTile>(new SourceTile{});
    return true;
  }
  void sourceWrite(int x, int y, Pixel pixel) {
    if (sourceInside(x, y)) {
      surface.write(x, y, pixel);
      return;
    }
    const int tx = sourceTileCoordinate(x), ty = sourceTileCoordinate(y);
    (*sourceOutside.at({tx, ty}))[size_t(y - ty * 32) * 32 + (x - tx * 32)] =
        pixel;
  }
  bool sourceWetDab(const Sample &s) {
    // Independent candidate inferred from original first/second-dab fixtures.
    // Model-specific equations are intentionally separate from native wet
    // paint.
    // Shared source foreground adjustment precedes the model-specific color
    // injection. Original four-model captures qualify static single-channel
    // controls; validation keeps other wet HSV combinations unsupported.
    // Off is an exact identity, preserving existing wet rounding and alpha.
    const Pixel foreground =
        sourceHsvColor(p.color, p.sourceHsvChannel, p.sourceHsvValue);
    const double radius = p.size.value * .5;
    spacing             = std::max(.01, p.size.value * p.spacing);
    // Measured model distinction: Dulling retains the hard Pixel lattice;
    // Smear transports a pixel-centered footprint instead.
    // Exact odd-diameter source masks are symmetric about pixel centers:
    // Smear's 7px rows are 3/5/7/7/7/5/3; even masks retain their half-pixel
    // center. Dulling uses the independently measured Pixel lattice instead.
    const double oddOffset = (int(p.size.value) & 1) ? .5 : 0;
    const double cx        = s.x + (p.wet == Wet::Dulling ? .5 : oddOffset),
                 cy        = s.y - (p.wet == Wet::Smear ? oddOffset : 0);
    const bool modern = p.profile == "krita-5.3.4.1-new-smear-candidate-1" ||
                        p.profile == "krita-5.3.4.1-new-dulling-candidate-1";
    const double pickupExtent =
        radius * (modern ? std::min(1.0, p.smudgeRadius) : p.smudgeRadius);
    // Keep the full moving field even outside the host raster. Original
    // continuous entry/exit/reentry captures show paint accumulating there
    // and influencing later in-canvas dabs. It is transient stroke state.
    // Retain the complete pickup rectangle, including untouched backdrop
    // outside the paint footprint. Original 3/4-dab large-radius captures
    // distinguish that halo from transparent padding or a uniform reservoir.
    // Eligibility remains independently constrained by sampler qualification.
    const double fieldExtent =
        p.wet == Wet::Dulling ? std::max(radius, pickupExtent) : radius;
    const int reach = int(std::ceil(fieldExtent + 1));
    const int x0    = int(std::floor(cx)) - reach,
              y0 = int(std::floor(cy)) - reach, x1 = int(std::ceil(cx)) + reach,
              y1              = int(std::ceil(cy)) + reach;
    const double deltaX       = previousSourceDab.x - s.x,
                 deltaY       = previousSourceDab.y - s.y;
    const double pickupWidth  = 2 * pickupExtent;
    const double pickupHeight = std::max(0.0, pickupWidth - 1);
    const unsigned pickupSamples =
        p.smudgeRadius == 0
            ? 1
            : std::max(
                  80u,
                  unsigned(std::floor(.02 * pickupWidth * pickupHeight + 20)));
    // Source wet bypasses native dab deposition. Charge its own worst-case
    // clipped snapshot/feedback/deposition work before prepare, allocation or
    // host reads: initialization/store, four-tap transport, tip/selection,
    // backdrop/feedback reads, quantization and publishing. The first dab uses
    // the same conservative ceiling; no geometry/semantic setting is reduced.
    if (!accountPixelWork(x1 - x0, y1 - y0, p.texture.image ? 17 : 16))
      return false;
    if (p.wet == Wet::Dulling) {
      size_t sampleWork = 1;
      if (p.smudgeRadius != 0) {
        // Clipping the canvas does not shorten the physical sample sequence.
        // Bound both radical-inverse digit loops plus coordinate, tip and
        // read/accumulate work for every attempted sample, including rejects.
        sampleWork = 4;
        for (unsigned n = pickupSamples; n; n /= 2) ++sampleWork;
        for (unsigned n = pickupSamples; n; n /= 3) ++sampleWork;
      }
      if (!accountPixelWork(int(pickupSamples), 1, sampleWork)) return false;
    }
    if (!prepareSourceOutside(x0, y0, x1, y1)) return false;
    const size_t bytes = size_t(x1 - x0) * (y1 - y0) * sizeof(Pixel);
    if (bytes > budget - reserved || !shared->reserve(bytes)) {
      failure = "Source wet snapshot memory limit exceeded";
      return false;
    }
    reserved += bytes;
    const int clipX0 = std::max(0, x0), clipY0 = std::max(0, y0),
              clipX1 = std::min(surface.width(), x1),
              clipY1 = std::min(surface.height(), y1);
    if (clipX0 < clipX1 && clipY0 < clipY1 &&
        !surface.prepare(clipX0, clipY0, clipX1, clipY1)) {
      failure = "Raster transaction refused the source wet region";
      return false;
    }
    // Snapshot feedback before destination writes. Smear-alpha uses arithmetic
    // interpolation of premultiplied channels, not source-over opacity. These
    // coincide for an opaque initial pickup but diverge on later dabs.
    std::vector<Pixel> feedback(size_t(x1 - x0) * (y1 - y0));
    auto at = [&](int x, int y) -> Pixel {
      return x >= sourceX && y >= sourceY && x < sourceX + sourceW &&
                     y < sourceY + sourceH
                 ? sourceField[size_t(y - sourceY) * sourceW + x - sourceX]
                 : Pixel{};
    };
    // Independent orthogonal impulses identify a base-2/base-3 sequence
    // with an area-scaled sample count and an 80-sample minimum. Its final y
    // coordinate is one pixel below the mathematical bottom edge. New Dulling
    // clips sampling to the tip and caps its extent at one tip radius; legacy
    // sampling is rectangular.
    auto pickupAt = [&](auto read, double centerX, double centerY) {
      if (p.smudgeRadius == 0)
        return read(int(std::floor(centerX)), int(std::floor(centerY)));
      auto radicalInverse = [](unsigned value, unsigned base) {
        double result = 0, factor = 1;
        while (value) {
          factor /= base;
          result += factor * (value % base);
          value /= base;
        }
        return result;
      };
      Pixel sum{};
      const double extent = pickupExtent;
      unsigned count      = 0;
      for (unsigned i = 1; i <= pickupSamples; ++i) {
        const int x = int(
            std::floor(centerX + (2 * radicalInverse(i, 2) - 1) * extent + .5));
        const int y = int(std::floor(
            centerY - extent +
            radicalInverse(i, 3) * std::max(0.0, 2 * extent - 1) + .5));
        if (modern) {
          const double dx = (x - centerX) / radius,
                       dy = (y + .5 - centerY) / radius;
          if (tipPixel(p.tip, 0, dx, dy).a == 0) continue;
        }
        ++count;
        const Pixel sample = read(x, y);
        sum.r += sample.r;
        sum.g += sample.g;
        sum.b += sample.b;
        sum.a += sample.a;
      }
      return count ? mul(sum, 1.0 / count) : Pixel{};
    };
    auto surfaceAt = [&](int x, int y) { return sourceRead(x, y); };
    // Radius-zero Dulling carries the previous center pickup uniformly.
    // Smear carries a translated field. Heterogeneous third-dab fixtures
    // distinguish these states even when their first two dabs coincide.
    const Pixel dullingPickup =
        p.wet != Wet::Dulling ? Pixel{}
        : !dabs               ? pickupAt(surfaceAt, s.x, s.y)
                : pickupAt(at, previousSourceDab.x, previousSourceDab.y);
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const double fx = x + deltaX, fy = y + deltaY;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        feedback[size_t(y - y0) * (x1 - x0) + x - x0] =
            !dabs ? (p.wet == Wet::Dulling ? dullingPickup : sourceRead(x, y))
            : p.wet == Wet::Dulling
                ? dullingPickup
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
    // New Dulling attenuation measured across 7/32/128-pixel tips and
    // independent strengths; radius changes pickup sampling, not this scale.
    const double pickup    = p.pickup * eval(p.pickupControl, s);
    const double colorRate = p.sourceColorRateEnabled
                                 ? p.colorRate * eval(p.colorRateControl, s)
                                 : 0;
    // Independent second/third-dab opacity captures distinguish transport and
    // injection scaling from a final opacity blend. Both feed retained state.
    const double opacity  = unit(eval(p.opacity, s));
    const double strength = (p.sourceSmudgeLengthEnabled ? pickup : 1) *
                            opacity *
                            (modern && p.wet == Wet::Dulling ? .8 : 1);
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        double dx = (x + .5 - cx) / radius, dy = (y + .5 - cy) / radius;
        double coverage =
            tipPixel(p.tip, 0, dx, dy).a *
            (sourceInside(x, y) ? unit(surface.coverage(x, y)) : 1);
        coverage           = sourceTextureCoverage(x, y, coverage);
        const size_t index = size_t(y - y0) * (x1 - x0) + x - x0;
        Pixel paint        = feedback[index];
        if (!dabs) {
          feedback[index] = paint;
          continue;
        }
        if (modern && p.wet == Wet::Dulling && !p.sourceSmearAlpha) {
          paint = mix(
              sourceRead(x, y),
              composite(paint, mul(foreground, colorRate * colorRate * opacity),
                        Blend::Normal),
              strength);
        } else if (modern) {
          paint = composite(mix(sourceRead(x, y), paint, strength),
                            mul(foreground, colorRate * colorRate * opacity),
                            Blend::Normal);
        } else {
          // Legacy foreground attenuation uses the serialized length setting,
          // while pressure scales transport strength. Original independent
          // pickup/color-pressure probes distinguish these two quantities.
          paint = mix(
              sourceRead(x, y),
              composite(paint,
                        mul(foreground,
                            colorRate * std::max(.2, 1 - p.pickup) * opacity),
                        Blend::Normal),
              strength);
        }
        // Alpha transport disabled retains source-over feedback separately
        // from the displayed arithmetic mix. First-dab initialization above
        // remains unchanged. Three-dab fixtures distinguish the two states.
        feedback[index] = p.texture.image
                              ? quantize(mix(sourceRead(x, y), paint, coverage))
                          : coverage == 0 && p.smudgeRadius > 0
                              ? sourceRead(x, y)
                              : quantize(p.sourceSmearAlpha
                                             ? paint
                                             : composite(sourceRead(x, y),
                                                         paint, Blend::Normal));
        if (coverage > 0)
          sourceWrite(x, y, quantize(mix(sourceRead(x, y), paint, coverage)));
      }
    const size_t oldBytes = sourceField.size() * sizeof(Pixel);
    sourceField.swap(feedback);
    std::vector<Pixel>().swap(feedback);
    shared->release(oldBytes);
    reserved -= oldBytes;
    sourceX           = x0;
    sourceY           = y0;
    sourceW           = x1 - x0;
    sourceH           = y1 - y0;
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
    randomDab        = random();
    Pixel foreground = sourceHsvColor(
        hsv(mix(p.color, p.background, eval(p.backgroundMix, s)),
            eval(p.hue, s), eval(p.saturation, s), eval(p.brightness, s)),
        p.sourceHsvChannel, p.sourceHsvValue);
    std::array<bool, 3> hsvEnabled{};
    std::array<double, 3> hsvValues{.5, .5, .5};
    for (size_t channel = 0; channel < 3; ++channel) {
      hsvEnabled[channel] = p.sourceHsvV2[channel].enabled;
      if (hsvEnabled[channel])
        hsvValues[channel] =
            eval(p.sourceHsvV2[channel].control, sourcePressureSample(s));
    }
    foreground = sourceHsvTransform(foreground, hsvEnabled, hsvValues);
    if (!dabs) reservoir = foreground;
    const Sample sizeSample = sourcePressureSample(s);
    const bool sourceHard = p.profile == "krita-5.3.4.1-hard-pixel-candidate-1";
    const double diameter =
                     std::max(sourceHard ? .5 : .25, eval(p.size, sizeSample)),
                 radius = diameter * .5;
    spacing = sourceHard ? std::max(.5, diameter * std::max(.02, p.spacing))
              : sourceProfile(p.profile) ? std::max(.01, diameter * p.spacing)
                                         : diameter * p.spacing;
    const double aspect       = std::max(.01, eval(p.aspect, s));
    const double angleDegrees = eval(p.angle, s);
    // The measured barrel mapping is periodic; evaluate equivalent angles in
    // a common range so 315 and -45 degrees share the same mask boundary math.
    const double angle =
                     (sourcePixelProfile(p.profile) && !p.angle.mappings.empty()
                          ? std::remainder(angleDegrees, 360.0)
                          : angleDegrees) *
                     pi / 180,
                 c = std::cos(angle), sn = std::sin(angle);
    const double opacity = unit(eval(p.opacity, s));
    // K5341 build-up uses opacity as dab transparency; static flow is
    // inactive. Original zero/quarter/half-flow fixtures establish this
    // distinction from native multiplication and source wash mode.
    const double flow =
        sourcePixelProfile(p.profile) && !p.wash ? 1 : unit(eval(p.flow, s));
    const double pickup       = p.pickup * eval(p.pickupControl, s),
                 colorRate    = p.colorRate * eval(p.colorRateControl, s);
    const double smudgeRadius = p.smudgeRadius * eval(p.radiusControl, s);
    const size_t primaryIndex = index(p.tip),
                 secondIndex  = p.secondary.enabled && !independentSecondary()
                                    ? index(p.secondary.tip)
                                    : 0;
    const double scatter      = eval(p.scatter, s) * diameter;
    const int stampCount =
        int(std::floor(p.count * unit(eval(p.countControl, s)) + .5));
    for (int stamp = 0; stamp < stampCount; ++stamp) {
      const double scatterX = (random() * 2 - 1) * scatter,
                   scatterY = (random() * 2 - 1) * scatter;
      // Independently measured K5341 hard-circle lattice: integer x and
      // half-integer y distances from an integral API stroke position. The
      // native profile retains its pixel-center convention unchanged.
      const double sourceOffset = sourcePixelProfile(p.profile) ? .5 : 0;
      const double cx           = s.x + sourceOffset +
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
      const size_t samples =
          tipWork(p.tip) + (p.sourceMasked.enabled ? 1 : 0) +
          (p.texture.image ? 4 : 0) + (p.wet == Wet::Smear ? 4 : 0) +
          (p.secondary.enabled && !p.secondary.independentSpacing
               ? size_t(p.secondary.count) * tipWork(p.secondary.tip)
               : 0);
      if (!accountPixelWork(x1 - x0, y1 - y0, samples)) return false;
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
        side = std::min(256, std::max(2, int(std::ceil(diameter))));
        if (!accountPixelWork(side, side,
                              p.wet == Wet::Smear ? 4 : tipWork(p.tip)))
          return false;
        const size_t bytes =
            p.wet == Wet::Smear ? size_t(side) * side * sizeof(Pixel) : 0;
        if (bytes > budget - reserved || !shared->reserve(bytes)) {
          failure = "Wet brush memory limit exceeded";
          return false;
        }
        reserved += bytes;
        if (p.wet == Wet::Smear) nextSmear.resize(size_t(side) * side);
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
            if (p.wet == Wet::Smear) {
              // Each transported cell is a reservoir. Pickup and foreground
              // mixing happen once, rather than blending a global reservoir
              // with a second raw-canvas history. Resample retained state in
              // the normalized tip frame when pressure changes grid size.
              const Pixel prior               = smear.empty()
                                                    ? foreground
                                                    : fieldSample(smear, smearSide, u, v);
              nextSmear[size_t(j) * side + i] = mix(prior, q, pickup);
              continue;
            }
            double w = filteredTip(p.tip, primaryIndex, u, v, 2.0 / side, 0, 0,
                                   2.0 / side)
                           .a;
            avg.r += q.r * w;
            avg.g += q.g * w;
            avg.b += q.b * w;
            avg.a += q.a * w;
            total += w;
          }
        if (p.wet == Wet::Dulling) {
          if (total > 0) avg = mul(avg, 1 / total);
          reservoir = mix(reservoir, avg, pickup);
          paint     = mix(reservoir, foreground, colorRate);
        }
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
      if (sec.enabled && !sec.independentSpacing)
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
          Pixel tip;
          double a = 0;
          if (sourceDefaultProfile(p.profile)) {
            // Independently recovered from complete alpha masks at five fade
            // values. Default circles fade in squared distance, with a
            // one-pixel inner radius at zero fade, rather than native linear
            // falloff.
            const double inner = std::min(
                1.0, p.tip.hardness == 0 ? 1 / radius : p.tip.hardness);
            const double d2 = u * u + v * v;
            double fade2    = inner * inner;
            if (p.tip.antialiasEdges) {
              // Independently measured pixel-axis AA, not a radial blur.
              // Exact on all five 32px fade masks and 32/64/128px hard masks.
              // Smaller tips use a separate source sampling regime.
              const double x = std::abs(u) * radius, y = std::abs(v) * radius;
              fade2 *=
                  (x * x + y * y) / ((x + 1) * (x + 1) + (y + 1) * (y + 1));
            }
            double coverage =
                fade2 == 1 ? (d2 < 1 ? 1 : 0) : unit((1 - d2) / (1 - fade2));
            if (p.profile == "krita-5.3.4.1-default-rect-candidate-1") {
              const double x = std::abs(u), y = std::abs(v);
              if (p.tip.antialiasEdges) {
                const double fx = inner * x * radius / (x * radius + 1),
                             fy = inner * y * radius / (y * radius + 1);
                coverage =
                    unit(std::min((1 - x) / (1 - fx), (1 - y) / (1 - fy)));
              } else {
                // Captured non-AA default rectangles include boundary pixels
                // and an opaque axis when either coordinate is exactly zero.
                coverage =
                    x > 1 || y > 1
                        ? 0
                        : (inner == 1 || x == 0 || y == 0
                               ? 1
                               : unit((1 - std::max(x, y)) / (1 - inner)));
              }
            }
            // Small scalar masks truncate; larger captured masks round to
            // nearest. Fade zero alone uses the one-pixel fallback radius.
            double rounding = !p.tip.antialiasEdges && diameter < 16 ? 0 : .5;
            if (p.tip.antialiasEdges && diameter >= 10 && diameter < 16 &&
                p.profile == "krita-5.3.4.1-default-circle-candidate-1") {
              // Original D10/D12 complete masks identify round-to-nearest
              // full eight-pixel row blocks and a truncating remainder.
              // The tail starts relative to the generated stamp, not canvas
              // alignment. Fractional/dynamic sizes remain ineligible.
              const int left = int(std::ceil(cx - .5 - radius));
              if (x >= left + (int(diameter) / 8) * 8) rounding = 0;
            }
            a = std::floor(coverage * 255 + rounding) / 255;
            if (p.tip.antialiasEdges && diameter < 10 &&
                p.profile == "krita-5.3.4.1-default-circle-candidate-1") {
              // Original D1/2/3/4/7/8/9 complete masks establish this scalar
              // regime: a forward 3x3 grid, upward byte quantization at each
              // sample and again after averaging, clipped to the generated
              // integer-size stamp. Do not extend this to dynamic/fractional
              // sizes, soft small masks or rectangles without evidence.
              const int left = int(std::ceil(cx - .5 - radius)),
                        top  = int(std::floor(cy - radius));
              unsigned sum   = 0;
              if (x >= left && x < left + int(diameter) && y >= top &&
                  y < top + int(diameter)) {
                for (int sy = 0; sy < 3; ++sy)
                  for (int sx = 0; sx < 3; ++sx) {
                    const double px        = x + .5 - cx + sx / 3.0,
                                 py        = y + .5 - cy + sy / 3.0;
                    const double distance2 = px * px + py * py;
                    const double fade =
                        distance2 / ((std::abs(px) + 1) * (std::abs(px) + 1) +
                                     (std::abs(py) + 1) * (std::abs(py) + 1));
                    const double sub =
                        unit((1 - distance2 / (radius * radius)) / (1 - fade));
                    sum += unsigned(std::ceil(sub * 255));
                  }
              }
              a = ((sum + 8) / 9) / 255.0;
            }
          } else {
            tip = filteredTip(p.tip, primaryIndex, u, v, ux, uy, vx, vy);
            a   = tip.a;
          }
          if (a <= 0) continue;
          if (t.image && sourcePixelProfile(p.profile))
            a = sourceTextureCoverage(x, y, a);
          else if (t.image) {
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
          if (independentSecondary() && !p.sourceMasked.enabled) {
            const auto found = secondaryMask.find({x / 32, y / 32});
            const double b   = found == secondaryMask.end()
                                   ? 0
                                   : (*found->second)[(y % 32) * 32 + x % 32];
            a                = mask(a, b, sec.mode);
          } else if (sec.enabled) {
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
          if (p.wet == Wet::Smear)
            ink =
                mix(fieldSample(nextSmear, side, u, v), foreground, colorRate);
          // Erasing deposits coverage, not pigment. Tip alpha already lives
          // in a; foreground/background and wet pickup alpha must not attenuate
          // it again. The compositor still enforces the stored alpha lock.
          if (p.blend == Blend::Erase) ink = {0, 0, 0, 1};
          const auto deposit = [&](Pixel dst, Pixel src, Blend mode) {
            return sourcePixelProfile(p.profile)
                       ? sourcePixelComposite(dst, src, mode)
                       : composite(dst, src, mode, p.lockAlpha);
          };
          if (p.wash) {
            Tile *buf = tile(x, y);
            if (!buf) return false;
            size_t k = size_t(y % 32) * 32 + x % 32;
            buf->paint[k] =
                composite(buf->paint[k], mul(ink, a * flow), Blend::Normal);
            if (p.sourceMasked.enabled)
              recomposeSourceMask(x, y);
            else
              surface.write(
                  x, y,
                  deposit(buf->base[k], mul(buf->paint[k], opacity), p.blend));
          } else
            surface.write(x, y,
                          deposit(surface.read(x, y),
                                  mul(ink, a * flow * opacity), p.blend));
        }
      if (p.wet != Wet::Off) {
        if (p.wet == Wet::Dulling)
          reservoir = mix(reservoir, foreground, p.reload);
        else
          for (Pixel &state : nextSmear)
            state = mix(state, foreground, p.reload);
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
    m->inputPixelWork = 0;
    if (!m->p.sourceMasked.enabled &&
        m->p.profile == "krita-5.3.4.1-hard-pixel-candidate-1")
      return m->addSourcePixel(s);
    if (!m->started) {
      m->first = m->previous = s;
      m->started             = true;
      if (m->independentSecondary() && !m->secondaryDab(s)) return false;
      return m->dab(s);
    }
    if (s.time < m->previous.time) {
      m->failure = "Tablet timestamps moved backwards";
      return false;
    }
    const Sample from = m->previous;
    double distance   = std::hypot(s.x - from.x, s.y - from.y),
           dt         = s.time - from.time;
    if (m->p.sourceMasked.enabled && distance > 0) {
      if (m->pendingMaskSecondary && !m->secondaryDab(from)) return false;
      if (m->pendingMaskPrimary && !m->dab(from)) return false;
      m->pendingMaskSecondary = m->pendingMaskPrimary = false;
    }
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
    if (m->independentSecondary())
      return m->addIndependent(from, s, distance, dt);
    double f      = 0;
    size_t events = 0;
    while (f < 1) {
      double fd = distance > 0 ? (m->spacing - m->sinceDab) / distance : 2;
      double ft = m->p.airbrushRate > 0 && dt > 0
                      ? (1 / m->p.airbrushRate - m->sinceTime) / dt
                      : 2;
      double step =
          std::max(sourceProfile(m->p.profile) ? 1e-9 : 0.0, std::min(fd, ft));
      const double end = f + step;
      const bool sourceEndpoint =
          sourceProfile(m->p.profile) && std::abs(end - 1) <= 1e-12;
      if (end > 1 && !sourceEndpoint) break;
      if (++events > 65536) {
        m->failure = "Stroke input event work limit exceeded";
        return false;
      }
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
      Sample dabSample = interpolate(from, s, f);
      if (!m->dab(dabSample)) return false;
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
bool Stroke::finish() {
  if (!m->finished && m->failure.empty() && m->started && !m->dabs &&
      !m->p.sourceMasked.enabled &&
      m->p.profile == "krita-5.3.4.1-hard-pixel-candidate-1") {
    try {
      m->inputPixelWork          = 0;
      m->sourcePressureLookahead = false;
      m->dab(m->first);  // A committed tap has no future motion to sample.
    } catch (const std::bad_alloc &) {
      m->failure = "Brush finalization allocation failed";
    }
  }
  const bool success = m->failure.empty();
  cancel();
  return success;
}
void Stroke::cancel() {
  m->finished              = true;
  m->pendingSourceEndpoint = false;
  m->releaseStorage();
}
const std::string &Stroke::error() const { return m->failure; }
size_t Stroke::dabCount() const { return m->dabs; }
bool Stroke::nativeWetState(Pixel &state, double u, double v) const {
  if (!m->started || m->finished || !m->failure.empty() ||
      m->p.profile != "groundzero-native-1" || m->p.wet == Wet::Off ||
      !std::isfinite(u) || !std::isfinite(v) || std::abs(u) > 1 ||
      std::abs(v) > 1)
    return false;
  if (m->p.wet == Wet::Dulling)
    state = m->reservoir;
  else {
    if (m->smear.empty()) return false;
    state = fieldSample(m->smear, m->smearSide, u, v);
  }
  return true;
}
}  // namespace RasterBrush
