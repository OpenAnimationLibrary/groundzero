// SPDX-License-Identifier: BSD-3-Clause
// Particle cache playback is stateless: every sample is a function of scene
// time, cache content and FX parameters, never of preview/render order.
#include "stdfx.h"
#include "tfxparam.h"
#include "tnotanimatableparam.h"
#include "toonz/particlecache.h"
#include "toonz/particlebake.h"
#include "toonz/sceneresources.h"
#include "tfilepath.h"
#include "tparamuiconcept.h"
#include "trop.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <vector>

namespace particlecachefx {
using Color = std::array<double, 4>;
constexpr std::size_t MaxPixels = 16u * 1024u * 1024u;
constexpr double Pi = 3.14159265358979323846;

struct RasterLock {
  TRasterP raster;
  explicit RasterLock(const TRasterP &r) : raster(r) { raster->lock(); }
  ~RasterLock() { raster->unlock(); }
};

template <class PIXEL>
Color readPixel(const TRasterPT<PIXEL> &raster, int x, int y) {
  if (x < 0 || y < 0 || x >= raster->getLx() || y >= raster->getLy())
    return {{0, 0, 0, 0}};
  const auto &p = raster->pixels(y)[x];
  const double k = 1.0 / PIXEL::maxChannelValue;
  return {{p.r * k, p.g * k, p.b * k, p.m * k}};
}

Color readPixel(const TRasterP &raster, int x, int y) {
  if (TRaster32P r = raster) return readPixel<TPixel32>(r, x, y);
  if (TRaster64P r = raster) return readPixel<TPixel64>(r, x, y);
  if (TRasterFP r = raster) return readPixel<TPixelF>(r, x, y);
  throw std::runtime_error("Unsupported artwork pixel type");
}

Color sample(const TRasterP &raster, double x, double y) {
  if (!std::isfinite(x) || !std::isfinite(y) || x < -1 || y < -1 ||
      x >= raster->getLx() || y >= raster->getLy()) return {{0, 0, 0, 0}};
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const double ax = x - ix, ay = y - iy;
  const Color a = readPixel(raster, ix, iy);
  const Color b = readPixel(raster, ix + 1, iy);
  const Color c = readPixel(raster, ix, iy + 1);
  const Color d = readPixel(raster, ix + 1, iy + 1);
  Color result{};
  for (int k = 0; k < 4; ++k)
    result[k] = (a[k] * (1 - ax) + b[k] * ax) * (1 - ay) +
                (c[k] * (1 - ax) + d[k] * ax) * ay;
  return result;
}

template <class PIXEL>
void copyPixels(const TRasterPT<PIXEL> &raster,
                const std::vector<Color> &pixels, double weight) {
  using Channel = typename PIXEL::Channel;
  RasterLock lock(raster);
  for (int y = 0; y < raster->getLy(); ++y) {
    auto *row = raster->pixels(y);
    for (int x = 0; x < raster->getLx(); ++x) {
      Color p = pixels[std::size_t(y) * raster->getLx() + x];
      for (int k = 0; k < 4; ++k) p[k] *= weight;
      p[3] = std::clamp(p[3], 0.0, 1.0);
      for (int k = 0; k < 3; ++k) {
        if (!std::isfinite(p[k])) p[k] = 0;
        if (!std::is_floating_point<Channel>::value)
          p[k] = std::clamp(p[k], 0.0, p[3]);
      }
      const double round = std::is_floating_point<Channel>::value ? 0 : .5;
      row[x] = PIXEL(Channel(p[0] * PIXEL::maxChannelValue + round),
                     Channel(p[1] * PIXEL::maxChannelValue + round),
                     Channel(p[2] * PIXEL::maxChannelValue + round),
                     Channel(p[3] * PIXEL::maxChannelValue + round));
    }
  }
}

void over(Color &destination, Color source) {
  const double remain = 1.0 - std::clamp(source[3], 0.0, 1.0);
  for (int k = 0; k < 4; ++k)
    destination[k] = source[k] + remain * destination[k];
}

struct Artwork {
  TRasterP raster;
  TPointD origin;
  double density = 1;
  TRectD bounds;
};
using ArtworkKey = std::tuple<int, double, double>;
struct ArtworkMap {
  std::map<ArtworkKey, Artwork> images;
  std::size_t bytes = 0;
};
}  // namespace particlecachefx

class ParticleCacheFx final : public TStandardZeraryFx,
                              public SceneFxResource {
  FX_PLUGIN_DECLARATION(ParticleCacheFx)
  TFxPortDG m_artwork;
  TStringParamP m_cacheFile, m_mappingJson;
  TBoolParamP m_useBakedAppearance;
  TBoolParamP m_manifestMapping;
  TStringParamP m_positionAttribute, m_velocityAttribute, m_idAttribute;
  TStringParamP m_sizeAttribute, m_colorAttribute, m_opacityAttribute;
  TStringParamP m_rotationAttribute, m_ageAttribute, m_lifetimeAttribute;
  TStringParamP m_sourceAttribute, m_drawingAttribute;
  TDoubleParamP m_units, m_defaultSize, m_sceneFps, m_cacheFps;
  TDoubleParamP m_startFrame, m_offset, m_speed;
  TIntEnumParamP m_axes, m_end, m_missing, m_interpolation, m_sizeMode;
  TIntEnumParamP m_artworkAnimation, m_assignment, m_order;
  TBoolParamP m_flipY, m_diameter, m_radians, m_linearTint;
  TDoubleParamP m_positionX, m_positionY, m_scale, m_rotation, m_opacity;
  TDoubleParamP m_artworkStart, m_artworkLength, m_artworkSpeed;
  TIntParamP m_seed, m_trails, m_blurSamples;
  TDoubleParamP m_trailStep, m_trailFade, m_shutter;
  TFilePath m_resolvedPath, m_resolvedFor;

  QString path() const {
    const TFilePath original(m_cacheFile->getValue());
    const TFilePath actual = original == m_resolvedFor && !m_resolvedPath.isEmpty()
                                 ? m_resolvedPath : original;
    return actual.getQString();
  }

  ParticleCache::Sequence sequence() const {
    ParticleCache::Sequence result;
    QString error;
    if (!ParticleCache::openSequence(path(), result, error))
      throw std::runtime_error(error.toStdString());
    if (m_cacheFps->getValue(0) > 0)
      result.metadata.framesPerSecond = m_cacheFps->getValue(0);
    return result;
  }

  ParticleCache::Mapping mapping(const ParticleCache::Sequence &sequence,
                                 double frame) const {
    ParticleCache::Mapping m = sequence.mapping;
    if (!m_manifestMapping->getValue()) {
      auto name = [](const TStringParamP &p) {
        return QString::fromStdWString(p->getValue());
      };
      m.position = name(m_positionAttribute);
      m.velocity = name(m_velocityAttribute);
      m.id = name(m_idAttribute);
      m.size = name(m_sizeAttribute);
      m.color = name(m_colorAttribute);
      m.opacity = name(m_opacityAttribute);
      m.rotation = name(m_rotationAttribute);
      m.age = name(m_ageAttribute);
      m.lifetime = name(m_lifetimeAttribute);
      m.artworkIndex = name(m_sourceAttribute);
      m.artworkFrame = name(m_drawingAttribute);
      m.axes = m_axes->getValue() == 1 ? std::array<int, 3>{{0, 2, 1}}
               : m_axes->getValue() == 2 ? std::array<int, 3>{{1, 2, 0}}
                                        : std::array<int, 3>{{0, 1, 2}};
      m.axisSigns = {{1, m_flipY->getValue() ? -1 : 1, 1}};
      m.units = m_units->getValue(frame);
      m.defaultSize = m_defaultSize->getValue(frame);
      m.sizeIsDiameter = m_diameter->getValue();
      m.rotationInRadians = m_radians->getValue();
    }
    if (m_manifestMapping->getValue() && !m_mappingJson->getValue().empty()) {
      QJsonParseError parse;
      const auto json = QJsonDocument::fromJson(QString::fromStdWString(m_mappingJson->getValue()).toUtf8(), &parse);
      QString error;
      if (parse.error != QJsonParseError::NoError || !json.isObject() ||
          !ParticleCache::mappingFromJson(json.object(), m, error))
        throw std::runtime_error((error.isEmpty() ? QString("Invalid particle attribute mapping") : error).toStdString());
    }
    return m;
  }

  ParticleCache::Playback playback(double frame) const {
    ParticleCache::Playback p;
    p.sceneFramesPerSecond = m_sceneFps->getValue(frame);
    p.startFrame = m_startFrame->getValue(frame);
    p.offset = m_offset->getValue(frame);
    p.speed = m_speed->getValue(frame);
    p.interpolate = m_interpolation->getValue() != 0;
    p.end = static_cast<ParticleCache::EndBehavior>(m_end->getValue());
    p.missing = static_cast<ParticleCache::MissingFrame>(m_missing->getValue());
    return p;
  }

  ParticleCache::SnapshotPtr snapshot(const ParticleCache::Sequence &seq,
                                      double frame) const {
    QString error;
    auto result = ParticleCache::sampleSequence(seq, frame, playback(frame),
                                                mapping(seq, frame), error);
    if (!result) throw std::runtime_error(error.toStdString());
    return result;
  }

  int sourceIndex(const ParticleCache::Particle &p) const {
    if (m_assignment->getValue() == 0) return p.artworkIndex;
    // Hash identities, not array indices: source choice survives reorder/births.
    std::uint64_t h = std::uint64_t(p.id) ^ std::uint64_t(m_seed->getValue());
    h ^= h >> 30; h *= UINT64_C(0xbf58476d1ce4e5b9);
    h ^= h >> 27; h *= UINT64_C(0x94d049bb133111eb);
    h ^= h >> 31;
    std::vector<int> connected;
    for (int i = 0; i < getInputPortCount(); ++i)
      if (getInputPort(i)->isConnected()) connected.push_back(i);
    return connected.empty() ? 0 : connected[std::size_t(h % connected.size())];
  }

  double artworkFrame(const ParticleCache::Particle &p, double frame) const {
    const int mode = m_artworkAnimation->getValue();
    if (mode == 0) return p.artworkFrame;
    if (mode == 1) return m_artworkStart->getValue(frame);
    double time = mode == 2 ? p.age * m_sceneFps->getValue(0) : frame;
    const double length = std::max(1.0, m_artworkLength->getValue(frame));
    double f = std::fmod(std::floor(time * m_artworkSpeed->getValue(frame)), length);
    if (f < 0) f += length;
    return m_artworkStart->getValue(frame) + f;
  }

  TRasterFxPort *sourcePort(int source) const {
    if (source < 0 || source >= getInputPortCount()) return nullptr;
    auto *port = dynamic_cast<TRasterFxPort *>(getInputPort(source));
    return port && port->isConnected() ? port : nullptr;
  }

  particlecachefx::Artwork artwork(int source, double frame, double density,
                                   const TRenderSettings &settings,
                                   particlecachefx::ArtworkMap &cache, bool dry = false) const {
    using namespace particlecachefx;
    density = std::max(1.0 / 64, density);
    // Quantize upward so neighboring particles can share source rasters.
    density = std::ceil(density * 8) / 8;
    const ArtworkKey key(source, frame, density);
    auto existing = cache.images.find(key);
    if (existing != cache.images.end()) return existing->second;
    Artwork result;
    auto *port = sourcePort(source);
    if (!port) return result;
    TRenderSettings input(settings);
    input.m_affine = TAffine();
    (*port)->getBBox(frame, result.bounds, input);
    if (result.bounds.isEmpty()) return result;
    if (result.bounds == TConsts::infiniteRectD)
      throw std::runtime_error("Artwork must have finite bounds; crop the source before connecting it");
    input.m_affine = TScale(density);
    TRectD box = input.m_affine * result.bounds;
    const double x0 = std::floor(box.x0) - 2, y0 = std::floor(box.y0) - 2;
    const double width = std::ceil(box.x1) - x0 + 3;
    const double height = std::ceil(box.y1) - y0 + 3;
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 ||
        width > 16384 || height > 16384 || width * height > double(MaxPixels))
      throw std::runtime_error("Artwork raster exceeds the particle memory limit; reduce scale or source bounds");
    const std::size_t bytes = std::size_t(width) * std::size_t(height) * sizeof(TPixelF);
    if (!dry && cache.bytes + bytes > 256u * 1024u * 1024u) {
      cache.images.clear(); cache.bytes = 0;
    }
    if (dry) {
      input.m_bpp = 128;
      (*port)->dryCompute(TRectD(x0, y0, x0 + width, y0 + height), frame, input);
      cache.images.emplace(key, result);
      return result;
    }
    TTile tile;
    // Ask for float artwork; the FX framework converts sources without float
    // implementations. Do not down-convert HDR/16-bit inputs to 8-bit.
    TRasterFP templateRaster(1, 1);
    input.m_bpp = 128;
    (*port)->allocateAndCompute(tile, TPointD(x0, y0),
                               TDimension(int(width), int(height)),
                               templateRaster, frame, input);
    result.raster = tile.getRaster();
    result.origin = tile.m_pos;
    result.density = density;
    cache.images.emplace(key, result);
    cache.bytes += bytes;
    return result;
  }

  void renderNative(const ParticleCache::Snapshot &snapshot, double frame,
                    TTile &tile, const TRenderSettings &settings) const {
    ParticleBake::Frame captured;
    QString error;
    if (!ParticleBake::loadFrame(snapshot, captured, error))
      throw std::runtime_error(error.toStdString());
    const TAffine instance = TTranslation(m_positionX->getValue(frame), m_positionY->getValue(frame)) *
                             TRotation(m_rotation->getValue(frame)) * TScale(m_scale->getValue(frame) / 100);
    for (const auto &draw : captured.draws) {
      if (!draw.artwork) continue;
      TRasterP artwork = draw.artwork;
      const bool convertColor = captured.linearColorSpace != settings.m_linearColorSpace;
      const double opacity = std::clamp(m_opacity->getValue(frame) / 100, 0.0, 1.0);
      const bool downConvert = TRaster32P(tile.getRaster()) && !TRaster32P(artwork);
      if (convertColor || opacity != 1 || TRasterFP(tile.getRaster()) || downConvert) {
        TRasterFP prepared(artwork->getLx(), artwork->getLy());
        TRop::convert(prepared, artwork);
        if (convertColor) {
          if (settings.m_linearColorSpace) TRop::toLinearRGB(prepared, settings.m_colorSpaceGamma);
          else TRop::tosRGB(prepared, captured.colorSpaceGamma);
        }
        if (opacity != 1) {
          particlecachefx::RasterLock lock(prepared);
          for (int y = 0; y < prepared->getLy(); ++y)
            for (int x = 0; x < prepared->getLx(); ++x) {
              auto &p = prepared->pixels(y)[x];
              p.r *= opacity; p.g *= opacity; p.b *= opacity; p.m *= opacity;
            }
        }
        if (TRasterFP(tile.getRaster())) artwork = prepared;
        else {
          artwork = tile.getRaster()->create(prepared->getLx(), prepared->getLy());
          TRop::convert(artwork, prepared);
        }
      }
      const TAffine affine = TTranslation(-tile.m_pos.x, -tile.m_pos.y) *
                              settings.m_affine * instance * draw.rasterToWorld;
      if (std::abs(affine.det()) < 1e-18) continue;
      TRectD bounds = affine * TRectD(0, 0, artwork->getLx(), artwork->getLy());
      if (!std::isfinite(bounds.getLx()) || !std::isfinite(bounds.getLy()) ||
          bounds.getLx() > 32768 || bounds.getLy() > 32768 ||
          bounds.getLx() * bounds.getLy() > particlecachefx::MaxPixels)
        throw std::runtime_error("Baked artwork transform exceeds the render memory limit");
      if ((bounds * TRectD(0, 0, tile.getRaster()->getLx(), tile.getRaster()->getLy())).isEmpty())
        continue;
      TRop::over(tile.getRaster(), artwork, affine);
    }
  }

  void renderSnapshot(const ParticleCache::Snapshot &data, double frame,
                       double trailOpacity, const TTile &tile,
                       const TRenderSettings &settings,
                       std::vector<particlecachefx::Color> &pixels,
                       particlecachefx::ArtworkMap &artworks) const {
    using namespace particlecachefx;
    if (m_assignment->getValue() == 1 && !data.hasStableIds)
      throw std::runtime_error("Random artwork assignment requires stable particle IDs");
    std::vector<std::size_t> indices(data.particles.size());
    std::iota(indices.begin(), indices.end(), std::size_t(0));
    const int order = m_order->getValue();
    if (order != 3)
      std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        const auto &pa = data.particles[a], &pb = data.particles[b];
        if (order == 0 && pa.position[2] != pb.position[2])
          return pa.position[2] < pb.position[2];
        if (order == 2 && pa.age != pb.age) return pa.age > pb.age;
        return pa.id < pb.id;
      });
    const int width = tile.getRaster()->getLx(), height = tile.getRaster()->getLy();
    const double userScale = m_scale->getValue(frame) / 100;
    const double outputDensity = std::sqrt(std::max(
        settings.m_affine.a11 * settings.m_affine.a11 + settings.m_affine.a21 * settings.m_affine.a21,
        settings.m_affine.a12 * settings.m_affine.a12 + settings.m_affine.a22 * settings.m_affine.a22));
    for (std::size_t index : indices) {
      const auto &p = data.particles[index];
      if (!(p.size > 0) || !(userScale > 0)) continue;
      const double opacity = std::clamp(p.tint[3] * m_opacity->getValue(frame) / 100 * trailOpacity, 0.0, 1.0);
      if (opacity <= 0) continue;
      const int source = sourceIndex(p);
      const double drawingFrame = artworkFrame(p, frame);
      const TPointD position(p.position[0] + m_positionX->getValue(frame),
                             p.position[1] + m_positionY->getValue(frame));
      double size = p.size * userScale;
      Artwork sprite;
      auto *port = sourcePort(source);
      if (!port) {
        for (int i = 0; i < getInputPortCount(); ++i)
          if (getInputPort(i)->isConnected())
            throw std::runtime_error("Particle artwork index selects an unconnected Artwork port");
      }
      if (port) {
        TRenderSettings identity(settings); identity.m_affine = TAffine();
        TRectD bounds;
        (*port)->getBBox(drawingFrame, bounds, identity);
        if (bounds.isEmpty()) continue;
        if (bounds == TConsts::infiniteRectD)
          throw std::runtime_error("Artwork must have finite bounds; crop its source");
        if (m_sizeMode->getValue() == 0) {
          const double radius = .5 * std::max(bounds.getLx(), bounds.getLy());
          if (radius <= 0) continue;
          size /= radius;
        }
        sprite = artwork(source, drawingFrame, size * outputDensity, settings, artworks);
        if (!sprite.raster) continue;
      }
      const TAffine world = TTranslation(position) *
                            TRotation(p.rotation + m_rotation->getValue(frame)) * TScale(size);
      const TAffine transform = settings.m_affine * world;
      if (std::abs(transform.det()) < 1e-18) continue;
      const TAffine inverse = transform.inv();
      const TRectD local = sprite.raster ? sprite.bounds : TRectD(-1, -1, 1, 1);
      const TRectD bounds = (transform * local).enlarge(2);
      if (!std::isfinite(bounds.x0) || !std::isfinite(bounds.y0) ||
          !std::isfinite(bounds.x1) || !std::isfinite(bounds.y1))
        throw std::runtime_error("Particle transform is outside the supported coordinate range");
      const int x0 = int(std::max(0.0, std::min(double(width), std::floor(bounds.x0 - tile.m_pos.x))));
      const int y0 = int(std::max(0.0, std::min(double(height), std::floor(bounds.y0 - tile.m_pos.y))));
      const int x1 = int(std::max(0.0, std::min(double(width), std::ceil(bounds.x1 - tile.m_pos.x + 1))));
      const int y1 = int(std::max(0.0, std::min(double(height), std::ceil(bounds.y1 - tile.m_pos.y + 1))));
      std::unique_ptr<RasterLock> lock;
      if (sprite.raster) lock.reset(new RasterLock(sprite.raster));
      Color tint = p.tint;
      if (settings.m_linearColorSpace && !m_linearTint->getValue())
        for (int k = 0; k < 3; ++k)
          tint[k] = std::pow(std::max(0.0, tint[k]), settings.m_colorSpaceGamma);
      else if (!settings.m_linearColorSpace && m_linearTint->getValue())
        for (int k = 0; k < 3; ++k)
          tint[k] = std::pow(std::max(0.0, tint[k]), 1.0 / settings.m_colorSpaceGamma);
      for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        const TPointD q = inverse * TPointD(tile.m_pos.x + x, tile.m_pos.y + y);
        Color pixel;
        if (sprite.raster)
          pixel = sample(sprite.raster, q.x * sprite.density - sprite.origin.x,
                                        q.y * sprite.density - sprite.origin.y);
        else {
          // Four fixed coverage samples give a display-independent disk default.
          double coverage = 0;
          for (double dy : {-.25, .25}) for (double dx : {-.25, .25}) {
            const TPointD sub = inverse * TPointD(tile.m_pos.x + x + dx, tile.m_pos.y + y + dy);
            if (sub.x * sub.x + sub.y * sub.y <= 1) coverage += .25;
          }
          pixel = {{coverage, coverage, coverage, coverage}};
        }
        for (int k = 0; k < 3; ++k) pixel[k] *= tint[k] * opacity;
        pixel[3] *= opacity;
        over(pixels[std::size_t(y) * width + x], pixel);
      }
    }
  }

public:
  ParticleCacheFx()
      : m_artwork("Artwork"), m_cacheFile(L""), m_mappingJson(L""), m_useBakedAppearance(true), m_manifestMapping(true),
        m_positionAttribute(L"position"), m_velocityAttribute(L"velocity"), m_idAttribute(L"id"),
        m_sizeAttribute(L"pscale"), m_colorAttribute(L"Cd"), m_opacityAttribute(L"opacity"),
        m_rotationAttribute(L"rotation"), m_ageAttribute(L"age"), m_lifetimeAttribute(L"life"),
        m_sourceAttribute(L"ot_artwork"), m_drawingAttribute(L"ot_frame"),
        m_units(1), m_defaultSize(1), m_sceneFps(24), m_cacheFps(0),
        m_startFrame(0), m_offset(0), m_speed(1),
        m_axes(new TIntEnumParam(0, "XY")),
        m_end(new TIntEnumParam(0, "Hold")), m_missing(new TIntEnumParam(0, "Error")),
        m_interpolation(new TIntEnumParam(0, "Hold Samples")),
        m_sizeMode(new TIntEnumParam(0, "Particle Radius")),
        m_artworkAnimation(new TIntEnumParam(0, "Cache Drawing Frame")),
        m_assignment(new TIntEnumParam(0, "Cache Artwork Index")),
        m_order(new TIntEnumParam(0, "Depth")),
        m_flipY(false), m_diameter(false), m_radians(false), m_linearTint(true),
        m_positionX(0), m_positionY(0), m_scale(100), m_rotation(0), m_opacity(100),
        m_artworkStart(0), m_artworkLength(1), m_artworkSpeed(1),
        m_seed(1), m_trails(0), m_blurSamples(1), m_trailStep(1), m_trailFade(.5), m_shutter(0) {
    addInputPort("Artwork1", new TRasterFxPort, 0);
#define BIND(name, member) bindParam(this, name, member)
    BIND("cacheFile", m_cacheFile); BIND("mappingJson", m_mappingJson);
    BIND("useBakedAppearance", m_useBakedAppearance); BIND("useManifestMapping", m_manifestMapping);
    BIND("positionAttribute", m_positionAttribute); BIND("velocityAttribute", m_velocityAttribute);
    BIND("idAttribute", m_idAttribute); BIND("sizeAttribute", m_sizeAttribute);
    BIND("colorAttribute", m_colorAttribute); BIND("opacityAttribute", m_opacityAttribute);
    BIND("rotationAttribute", m_rotationAttribute); BIND("ageAttribute", m_ageAttribute);
    BIND("lifetimeAttribute", m_lifetimeAttribute); BIND("sourceAttribute", m_sourceAttribute);
    BIND("drawingAttribute", m_drawingAttribute); BIND("units", m_units);
    BIND("defaultSize", m_defaultSize); BIND("axes", m_axes); BIND("flipY", m_flipY);
    BIND("sizeIsDiameter", m_diameter); BIND("rotationInRadians", m_radians);
    BIND("sceneFps", m_sceneFps); BIND("cacheFps", m_cacheFps);
    BIND("startFrame", m_startFrame); BIND("timeOffset", m_offset); BIND("speed", m_speed);
    BIND("endBehavior", m_end); BIND("missingFrame", m_missing); BIND("interpolation", m_interpolation);
    BIND("sizeMode", m_sizeMode); BIND("artworkAnimation", m_artworkAnimation);
    BIND("artworkAssignment", m_assignment); BIND("drawOrder", m_order);
    BIND("linearTint", m_linearTint); BIND("positionX", m_positionX); BIND("positionY", m_positionY);
    BIND("scale", m_scale); BIND("rotation", m_rotation); BIND("opacity", m_opacity);
    BIND("artworkStart", m_artworkStart); BIND("artworkLength", m_artworkLength);
    BIND("artworkSpeed", m_artworkSpeed); BIND("seed", m_seed);
    BIND("trailSamples", m_trails); BIND("trailStep", m_trailStep); BIND("trailFade", m_trailFade);
    BIND("motionBlurSamples", m_blurSamples); BIND("shutter", m_shutter);
#undef BIND
    m_axes->addItem(1, "XZ"); m_axes->addItem(2, "YZ");
    m_end->addItem(1, "Loop"); m_end->addItem(2, "Transparent");
    m_missing->addItem(1, "Hold Previous"); m_missing->addItem(2, "Transparent");
    m_interpolation->addItem(1, "Interpolate Stable IDs");
    m_sizeMode->addItem(1, "Artwork Scale");
    m_artworkAnimation->addItem(1, "Fixed Drawing");
    m_artworkAnimation->addItem(2, "Cycle by Particle Age");
    m_artworkAnimation->addItem(3, "Cycle by Scene Frame");
    m_assignment->addItem(1, "Random by Stable ID");
    m_order->addItem(1, "Particle ID"); m_order->addItem(2, "Oldest First");
    m_order->addItem(3, "File Order");
    m_units->setValueRange(.000001, 1000000); m_defaultSize->setValueRange(0, 1000000);
    m_sceneFps->setValueRange(.001, 1000); m_cacheFps->setValueRange(0, 1000);
    m_scale->setValueRange(0, 100000); m_opacity->setValueRange(0, 100);
    m_artworkLength->setValueRange(1, 1000000);
    m_trails->setValueRange(0, 32); m_trailStep->setValueRange(.01, 1000);
    m_trailFade->setValueRange(0, 1); m_blurSamples->setValueRange(1, 16);
    m_shutter->setValueRange(0, 4);
    enableComputeInFloat(true);
  }

  bool isZerary() const override { return true; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  int dynamicPortGroupsCount() const override { return 1; }
  const TFxPortDG *dynamicPortGroup(int group) const override {
    return group == 0 ? &m_artwork : nullptr;
  }
  TFxTimeRegion getTimeRegion() const override { return TFxTimeRegion::createUnlimited(); }

  TFilePath resourcePath() const override { return TFilePath(m_cacheFile->getValue()); }
  void setResourcePath(const TFilePath &path) override {
    m_cacheFile->setValue(path.getWideString()); m_resolvedPath = TFilePath();
  }
  void setResolvedResourcePath(const TFilePath &path) override {
    m_resolvedFor = resourcePath(); m_resolvedPath = path;
  }
  TFx *clone(bool recursive = true) const override {
    auto *copy = static_cast<ParticleCacheFx *>(TStandardZeraryFx::clone(recursive));
    copy->m_resolvedPath = m_resolvedPath; copy->m_resolvedFor = m_resolvedFor;
    return copy;
  }

  std::string getAlias(double frame, const TRenderSettings &settings) const override {
    QString identity = path();
    if (!m_cacheFile->getValue().empty()) {
      try {
        const auto seq = sequence();
        if (!seq.manifestPath.isEmpty()) {
          QFile manifest(seq.manifestPath);
          QCryptographicHash hash(QCryptographicHash::Sha256);
          if (!manifest.open(QIODevice::ReadOnly) || !hash.addData(&manifest))
            throw std::runtime_error("Cannot read particle cache manifest");
          identity += QString::fromLatin1(hash.result().toHex());
        }
        const auto current = snapshot(seq, frame);
        if (m_useBakedAppearance->getValue() && current->extra.value("native").toBool()) {
          QString error;
          const QString nativeRevision = ParticleBake::frameRevision(*current, error);
          if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
          identity += current->revision + nativeRevision;
        } else {
          // Include only sampled files (and interpolation endpoints through the
          // snapshot revision), never every file in a potentially long sequence.
          // Artwork time can differ from scene time and must be in the alias.
          std::map<std::pair<int, double>, bool> used;
          const int samples = m_shutter->getValue(frame) > 0 ? m_blurSamples->getValue() : 1;
          for (int sample = 0; sample < samples; ++sample) {
            const double time = frame + (samples == 1 ? 0 :
                ((sample + .5) / samples - .5) * m_shutter->getValue(frame));
            for (int trail = m_trails->getValue(); trail >= 0; --trail) {
              const double t = time - trail * m_trailStep->getValue(frame);
              const auto data = t == frame ? current : snapshot(seq, t);
              identity += data->revision + QString::fromStdString(TRasterFx::getAlias(t, settings));
              for (const auto &p : data->particles)
                used[{sourceIndex(p), artworkFrame(p, t)}] = true;
            }
          }
          for (const auto &entry : used)
            if (auto *port = sourcePort(entry.first.first))
              identity += QString::fromStdString((*port)->getAlias(entry.first.second, settings));
        }
      } catch (const std::exception &e) {
        identity += ":error:" + QString::fromUtf8(e.what());
      }
    }
    return TRasterFx::getAlias(frame, settings) + "[particlecache-v1:" + identity.toStdString() + "]";
  }

  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = m_cacheFile->getValue().empty() ? TRectD() : TConsts::infiniteRectD;
    return !box.isEmpty();
  }

  void doDryCompute(TRectD &, double frame, const TRenderSettings &settings) override {
    if (m_cacheFile->getValue().empty()) return;
    try {
      const auto seq = sequence();
      const auto current = snapshot(seq, frame);
      if (m_useBakedAppearance->getValue() && current->extra.value("native").toBool()) return;
      const int samples = m_shutter->getValue(frame) > 0 ? m_blurSamples->getValue() : 1;
      const double outputDensity = std::sqrt(std::max(
          settings.m_affine.a11 * settings.m_affine.a11 + settings.m_affine.a21 * settings.m_affine.a21,
          settings.m_affine.a12 * settings.m_affine.a12 + settings.m_affine.a22 * settings.m_affine.a22));
      particlecachefx::ArtworkMap cache;
      for (int sample = 0; sample < samples; ++sample) {
        const double time = frame + (samples == 1 ? 0 :
            ((sample + .5) / samples - .5) * m_shutter->getValue(frame));
        for (int trail = m_trails->getValue(); trail >= 0; --trail) {
          const double t = time - trail * m_trailStep->getValue(frame);
          const auto data = snapshot(seq, t);
          for (const auto &p : data->particles) {
            auto *port = sourcePort(sourceIndex(p));
            if (!port || p.size <= 0) continue;
            const double drawing = artworkFrame(p, t);
            double size = p.size * m_scale->getValue(t) / 100;
            if (size <= 0) continue;
            TRenderSettings identity(settings); identity.m_affine = TAffine();
            TRectD bounds; (*port)->getBBox(drawing, bounds, identity);
            if (bounds.isEmpty()) continue;
            if (bounds == TConsts::infiniteRectD)
              throw std::runtime_error("Artwork must have finite bounds; crop its source");
            if (m_sizeMode->getValue() == 0) {
              const double radius = .5 * std::max(bounds.getLx(), bounds.getLy());
              if (radius <= 0) continue;
              size /= radius;
            }
            artwork(sourceIndex(p), drawing, size * outputDensity, settings, cache, true);
          }
        }
      }
    } catch (const std::exception &e) {
      throw TException(QString("Particle Cache: %1 (%2)").arg(QString::fromUtf8(e.what()), path()).toStdWString());
    }
  }

  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    using namespace particlecachefx;
    tile.getRaster()->clear();
    if (m_cacheFile->getValue().empty()) return;
    try {
      const std::size_t count = std::size_t(tile.getRaster()->getLx()) * tile.getRaster()->getLy();
      if (count > MaxPixels) throw std::runtime_error("Output tile exceeds particle memory limit");
      const auto seq = sequence();
      const auto current = snapshot(seq, frame);
      if (m_useBakedAppearance->getValue() && current->extra.value("native").toBool()) {
        renderNative(*current, frame, tile, settings);
        return;
      }
      const int blurSamples = m_shutter->getValue(frame) > 0 ? m_blurSamples->getValue() : 1;
      std::vector<Color> accumulated(count, Color{{0, 0, 0, 0}});
      ArtworkMap artworks;
      for (int blur = 0; blur < blurSamples; ++blur) {
        const double time = frame + (blurSamples == 1 ? 0 :
            ((blur + .5) / blurSamples - .5) * m_shutter->getValue(frame));
        std::vector<Color> layer(count, Color{{0, 0, 0, 0}});
        for (int trail = m_trails->getValue(); trail >= 0; --trail) {
          const double sampleTime = time - trail * m_trailStep->getValue(frame);
          const auto data = snapshot(seq, sampleTime);
          renderSnapshot(*data, sampleTime, std::pow(m_trailFade->getValue(frame), trail),
                         tile, settings, layer, artworks);
        }
        for (std::size_t i = 0; i < count; ++i)
          for (int k = 0; k < 4; ++k) accumulated[i][k] += layer[i][k];
      }
      const double weight = 1.0 / blurSamples;
      if (TRaster32P r = tile.getRaster()) copyPixels<TPixel32>(r, accumulated, weight);
      else if (TRaster64P r = tile.getRaster()) copyPixels<TPixel64>(r, accumulated, weight);
      else if (TRasterFP r = tile.getRaster()) copyPixels<TPixelF>(r, accumulated, weight);
      else throw std::runtime_error("Unsupported output pixel type");
    } catch (const std::exception &e) {
      throw TException(QString("Particle Cache: %1 (%2)").arg(QString::fromUtf8(e.what()), path()).toStdWString());
    }
  }
};

FX_PLUGIN_IDENTIFIER(ParticleCacheFx, "particleCacheFx")
