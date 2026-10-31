#pragma once

#include "tcommon.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef TOONZLIB_EXPORTS
#define RASTERBRUSH_API DV_EXPORT_API
#else
#define RASTERBRUSH_API DV_IMPORT_API
#endif

// Independently implemented CPU brush primitives. This API does not imply
// equivalence with a foreign application's engine or proprietary presets.
namespace RasterBrush {

struct Pixel {
  double r = 0, g = 0, b = 0, a = 0;  // encoded-sRGB, premultiplied
};
enum class Blend { Normal, Multiply, Screen, Overlay, Erase };
enum class Shape { Round, Square, Gaussian, Bitmap };
enum class MaskMode { Multiply, Subtract };
enum class Wet { Off, Dulling, Smear };
// Version 1: one fixed source HSV channel; dynamic/multiple channels require
// separate acquisition. Native hue/saturation/brightness retain their
// semantics.
enum class SourceHsvChannel { Off, Hue, Saturation, Value };
enum class Sensor {
  Pressure,
  TiltX,
  TiltY,
  TiltElevation,
  TiltDirection,
  Rotation,
  Direction,
  Distance,
  Time,
  Speed,
  RandomDab,
  RandomStroke,
  InitialDirection,
  DabIndex
};
struct Knot {
  double x = 0, y = 0;
};
enum class Interpolation { Linear, NaturalCubic };
struct Mapping {
  Sensor sensor = Sensor::Pressure;
  std::vector<Knot> curve{{0, 0}, {1, 1}};
  double scale                = 1;
  Interpolation interpolation = Interpolation::Linear;
  bool periodic =
      false;  // Explicit Time cycle; other sensors reject this flag.
};
struct Dynamic {
  double value = 1, minimum = 0;
  std::vector<Mapping> mappings;
};
struct Image {
  int width = 0, height = 0;
  std::vector<Pixel> pixels;
  bool color = false;  // false: alpha is coverage, RGB is ignored
};
struct Tip {
  Shape shape         = Shape::Round;
  double hardness     = 1;
  bool antialiasEdges = false;
  std::vector<std::shared_ptr<const Image>> images;
  bool randomSequence = false;
};
struct Texture {
  std::shared_ptr<const Image> image;
  MaskMode mode = MaskMode::Multiply;
  double scale = 1, angle = 0, offsetX = 0, offsetY = 0;
  double depth = 1, contrast = 1, brightness = 0;
  bool invert = false, perDab = false;
};
struct Secondary {
  bool enabled = false;
  Tip tip;
  MaskMode mode = MaskMode::Multiply;
  Dynamic size{1, 0, {}}, angle{0, 0, {}}, opacity{1, 0, {}};
  double aspect = 1, scatter = 0;
  // Native accumulated stroke-mask scheduling; false preserves per-primary
  // dabs.
  bool independentSpacing = false;
  double spacing = 0.1;  // fractions of the evaluated secondary diameter
  int count      = 1;
  bool flipX = false, flipY = false;
};
struct SourceHsvControl {
  bool enabled = false;
  Dynamic control{.5, 0, {}};
};
// Independently measured K5341 generated secondary field, separate from native
// Secondary. Flow accumulates at mask dabs; opacity scales the retained field.
struct SourceMasked {
  bool enabled = false, useMasterSize = true;
  double diameter = 12, masterSizeCoeff = .5, aspect = .5, spacing = .5;
  double opacity = 1, flow = 1;
  MaskMode mode = MaskMode::Multiply;
};
struct Preset {
  std::string name = "Native brush", profile = "groundzero-native-1";
  Tip tip;
  Dynamic size{20, 0.01, {}}, aspect{1, 0.01, {}}, angle{0, 0, {}};
  Dynamic opacity{1, 0, {}}, flow{1, 0, {}}, scatter{0, 0, {}};
  Dynamic countControl{1, 0, {}};
  double spacing = 0.1, airbrushRate = 0;
  int count            = 1;
  bool scatterBothAxes = true;
  bool wash = false, lockAlpha = false, flipX = false, flipY = false;
  Blend blend = Blend::Normal;
  Pixel color{0, 0, 0, 1};
  Pixel background{1, 1, 1, 1};
  // Mix premultiplied foreground/background, then rotate HSV hue (degrees)
  // and multiply saturation/value in straight encoded sRGB. Alpha is retained.
  Dynamic backgroundMix{0, 0, {}}, hue{0, 0, {}}, saturation{1, 0, {}},
      brightness{1, 0, {}};
  Texture texture;
  Secondary secondary;
  SourceMasked sourceMasked;
  Wet wet       = Wet::Off;
  double pickup = 0.5, colorRate = 0.25, reload = 0, smudgeRadius = 1;
  // Versioned source-wet state switches; native wet equations do not use them.
  bool sourceSmudgeLengthEnabled = true, sourceSmearAlpha = true,
       sourceColorRateEnabled       = true;
  bool sourceTextureSoftTexturing   = false;
  SourceHsvChannel sourceHsvChannel = SourceHsvChannel::Off;
  double sourceHsvValue             = .5;
  // V2 per-channel controls (H,S,V). Admission determines measured
  // combinations; storage order does not establish a source composition order.
  std::array<SourceHsvControl, 3> sourceHsvV2;
  Dynamic pickupControl{1, 0, {}}, colorRateControl{1, 0, {}},
      radiusControl{1, 0, {}};
  // Host policy only: Stroke always renders the explicit resolved seed.
  bool varySeedPerStroke = false;
  std::uint64_t seed     = 1;
};
// Canvas coordinates: origin at upper-left, +y down; positive rotation is
// clockwise.
struct Sample {
  double x = 0, y = 0, pressure = 1, tiltX = 0, tiltY = 0;
  double rotation = 0, time = 0;  // radians and monotonic seconds
  bool hasPressure = false, hasTilt = false, hasRotation = false;
};
class Surface {
public:
  virtual ~Surface()         = default;
  virtual int width() const  = 0;
  virtual int height() const = 0;
  // prepare must make the whole read/write region available and record undo
  // BEFORE any read. Inclusive-exclusive bounds; clipped to the canvas.
  virtual bool prepare(int x0, int y0, int x1, int y1) = 0;
  virtual Pixel read(int x, int y) const               = 0;
  virtual void write(int x, int y, const Pixel &pixel) = 0;
  virtual double coverage(int, int) const { return 1; }
};
// Shared by replicated tracks and their float raster cache. UI-thread only.
class RASTERBRUSH_API MemoryBudget {
  size_t limit_, used_ = 0;

public:
  explicit MemoryBudget(size_t limit) : limit_(limit) {}
  bool reserve(size_t bytes) {
    if (bytes > limit_ - used_) return false;
    used_ += bytes;
    return true;
  }
  void release(size_t bytes) { used_ -= bytes; }
  size_t used() const { return used_; }
};
// Lazy float tiles preserve sub-byte deposits while publishing quantized pixels
// to a host surface for display. Share one instance across replicated tracks.
class RASTERBRUSH_API BufferedSurface final : public Surface {
  struct Impl;
  std::unique_ptr<Impl> m;

public:
  BufferedSurface(std::shared_ptr<Surface> backing,
                  std::shared_ptr<MemoryBudget> budget);
  ~BufferedSurface();
  int width() const override;
  int height() const override;
  bool prepare(int x0, int y0, int x1, int y1) override;
  Pixel read(int x, int y) const override;
  void write(int x, int y, const Pixel &pixel) override;
  double coverage(int x, int y) const override;
};
RASTERBRUSH_API Pixel composite(Pixel dst, Pixel src, Blend mode,
                                bool lockAlpha = false);
RASTERBRUSH_API std::string validate(const Preset &preset);

// One instance per stroke/replicated track. Samples/settings never reference
// UI state; randomness and wet reservoirs reset for each instance.
class RASTERBRUSH_API Stroke {
  struct Impl;
  std::unique_ptr<Impl> m;

public:
  Stroke(Surface &surface, const Preset &preset,
         size_t memoryLimit                         = 256u * 1024u * 1024u,
         std::shared_ptr<MemoryBudget> sharedBudget = {});
  ~Stroke();
  Stroke(const Stroke &)            = delete;
  Stroke &operator=(const Stroke &) = delete;
  bool add(const Sample &sample);
  // Commit a pending source tap; failure requires transaction rollback.
  bool finish();
  // Discard buffered input without painting. Destruction also never flushes.
  void cancel();
  const std::string &error() const;
  size_t dabCount() const;
  // Read-only qualification/diagnostic state after reload. Smear coordinates
  // are in the normalized tip frame [-1,1]; Dulling has one uniform reservoir.
  // Foreign profiles, inactive/finished/failed strokes expose no native state.
  bool nativeWetState(Pixel &state, double u = 0, double v = 0) const;
};
}  // namespace RasterBrush
