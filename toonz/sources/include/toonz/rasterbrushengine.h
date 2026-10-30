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
  RandomStroke
};
struct Knot {
  double x = 0, y = 0;
};
struct Mapping {
  Sensor sensor = Sensor::Pressure;
  std::vector<Knot> curve{{0, 0}, {1, 1}};
  double scale = 1;
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
  Shape shape     = Shape::Round;
  double hardness = 1;
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
};
struct Preset {
  std::string name = "Native brush", profile = "groundzero-native-1";
  Tip tip;
  Dynamic size{20, 0.01, {}}, aspect{1, 0.01, {}}, angle{0, 0, {}};
  Dynamic opacity{1, 0, {}}, flow{1, 0, {}}, scatter{0, 0, {}};
  double spacing = 0.1, airbrushRate = 0;
  int count = 1;
  bool wash = false, lockAlpha = false, flipX = false, flipY = false;
  Blend blend = Blend::Normal;
  Pixel color{0, 0, 0, 1};
  Texture texture;
  Secondary secondary;
  Wet wet       = Wet::Off;
  double pickup = 0.5, colorRate = 0.25, reload = 0, smudgeRadius = 1;
  std::uint64_t seed = 1;
};
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
  void finish();
  const std::string &error() const;
  size_t dabCount() const;
};
}  // namespace RasterBrush
