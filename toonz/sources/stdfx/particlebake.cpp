#include "particlebakecapture.h"
#include "trasterfx.h"
#include "ttile.h"
#include "tconvert.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
struct Capture {
  const TFx *source;
  ParticleBake::Frame &frame;
  const ParticleBake::Options &options;
  const ParticleBake::Cancel &cancel;
  ParticleBake::DrawSample draw;
  std::uint64_t bytes = 0;
};
thread_local Capture *current = nullptr;
struct CaptureScope {
  Capture *previous;
  explicit CaptureScope(Capture *capture) : previous(current) { current = capture; }
  ~CaptureScope() { current = previous; }
};
}
namespace ParticleBakeCapture {
bool active(const TFx *source) { return current && current->source == source; }
void checkCancel(const TFx *source) {
  if (active(source) && current->cancel && current->cancel())
    throw std::runtime_error("Particle bake canceled.");
}
void checkParticleCount(const TFx *source, double count) {
  if (!active(source)) return;
  checkCancel(source);
  if (!std::isfinite(count) || count < 0 || count > current->options.maxDraws)
    throw std::runtime_error("Particle simulation exceeds bake particle limit.");
}
void checkRasterSize(const TFx *source, double width, double height, int pixelSize) {
  if (!active(source)) return;
  if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0 ||
      width > current->options.maxRasterDimension || height > current->options.maxRasterDimension ||
      width * height * pixelSize > current->options.maxArtworkBytes)
    throw std::runtime_error("Particle sampling exceeds bake raster limit.");
}
void checkSimulationRange(const TFx *source, int start, int end) {
  if (!active(source)) return;
  if (start == std::numeric_limits<int>::min() || end == std::numeric_limits<int>::max() ||
      double(end) - start > 1000000)
    throw std::runtime_error("Particle bake preroll exceeds simulation frame limit.");
}
void beginDraw(const TFx *source, const ParticleBake::DrawSample &sample) {
  if (active(source)) { checkCancel(source); current->draw = sample; }
}
void particle(const TFx *source, const ParticleBake::DrawSample &sample) {
  if (!active(source)) return;
  checkParticleCount(source, current->frame.particles.size() + 1);
  if (sizeof(sample) > current->options.maxArtworkBytes - current->bytes)
    throw std::runtime_error("Particle state exceeds bake memory limit.");
  current->frame.particles.push_back(sample);
  current->bytes += sizeof(sample);
}
void appearance(const TFx *source, double opacity, const TPixel32 &tint,
                double tintMix) {
  if (!active(source)) return;
  current->draw.opacity = opacity;
  current->draw.tint = tint;
  current->draw.tintMix = tintMix;
}
void record(const TFx *source, const TRasterP &artwork, const TAffine &affine,
            const TTile *tile) {
  if (!active(source) || !artwork) return;
  if (artwork->getLx() > current->options.maxRasterDimension ||
      artwork->getLy() > current->options.maxRasterDimension)
    throw std::runtime_error("Particle artwork exceeds bake size limit.");
  const std::uint64_t bytes = std::uint64_t(artwork->getLx()) *
      artwork->getLy() * artwork->getPixelSize();
  if (current->frame.draws.size() >= current->options.maxDraws ||
      bytes > current->options.maxArtworkBytes - current->bytes)
    throw std::runtime_error("Particle bake exceeds draw or artwork memory limit.");
  ParticleBake::DrawSample sample = current->draw;
  sample.artwork = artwork->clone();
  sample.rasterToWorld = current->frame.renderAffine.inv() *
      TTranslation(tile->m_pos) * affine;
  current->frame.draws.push_back(std::move(sample));
  current->bytes += bytes;
}
void background(const TFx *source, const TTile *tile) {
  if (!active(source)) return;
  ParticleBake::DrawSample sample;
  sample.background = true;
  sample.source = -1;
  sample.id = (std::numeric_limits<std::uint64_t>::max)();
  beginDraw(source, sample);
  record(source, tile->getRaster(), TAffine(), tile);
}
}
namespace ParticleBake {
bool isSupported(const TFx *fx) {
  if (!fx) return false;
  const std::string type = fx->getFxType();
  return type == "STD_particlesFx" || type == "STD_iwa_TiledParticlesFx";
}
bool captureFrame(TRasterFx *fx, double frame, const TRenderSettings &settings,
                  const Options &options, Frame &result, QString &error,
                  const Cancel &cancel) {
  error.clear();
  result = Frame();
  if (!isSupported(fx)) { error = "Select a Particles or Tiled Particles FX."; return false; }
  const TRectD &d = options.domain;
  const TAffine &a = settings.m_affine;
  if (!std::isfinite(frame) || frame < std::numeric_limits<int>::min() ||
      frame > std::numeric_limits<int>::max() || std::floor(frame) != frame ||
      !std::isfinite(options.framesPerSecond) || options.framesPerSecond <= 0 ||
      !std::isfinite(d.x0) || !std::isfinite(d.x1) ||
      !std::isfinite(d.y0) || !std::isfinite(d.y1) || d.isEmpty() ||
      !std::isfinite(a.a11) || !std::isfinite(a.a22) ||
      !std::isfinite(a.a13) || !std::isfinite(a.a23) ||
      a.a11 <= 0 || a.a22 != a.a11 || a.a12 != 0 || a.a21 != 0 ||
      options.maxRasterDimension <= 0 || options.maxArtworkBytes == 0 ||
      options.maxDraws == 0 || !std::isfinite(settings.m_colorSpaceGamma) ||
      settings.m_colorSpaceGamma <= 0) {
    error = "Bake requires an integer frame, positive FPS, finite domain and uniform positive sampling scale.";
    return false;
  }
  if ((settings.m_bpp != 32 && settings.m_bpp != 64) || settings.m_linearColorSpace) {
    error = "Native particle bake supports nonlinear 8-bit or 16-bit RGBA sampling.";
    return false;
  }
  try {
    if (cancel && cancel()) { error = "Particle bake canceled."; return false; }
    // Clone parameters and upstream graph: colour picking mutates particle-local
    // state and upstream computations must not edit the live schematic graph.
    TRasterFxP source = dynamic_cast<TRasterFx *>(fx->clone(true));
    if (!source) throw std::runtime_error("Unable to clone particle effect.");
    for (int port = 0; port < source->getInputPortCount(); ++port) {
      const std::string name = source->getInputPortName(port);
      TFxPort *input = source->getInputPort(port);
      if (name.empty() || name[0] != 'T' || !input || !input->isConnected()) continue;
      const double last = input->getFx()->getTimeRegion().getLastFrame();
      if (!std::isfinite(last) || last < 0 || last > 1000000)
        throw std::runtime_error("Particle artwork must have a finite, nonempty frame range for baking.");
    }
    const TRectD pixels = a * d;
    const double x0 = std::floor(pixels.x0), y0 = std::floor(pixels.y0);
    const double width = std::ceil(pixels.x1) - x0;
    const double height = std::ceil(pixels.y1) - y0;
    if (!std::isfinite(x0) || !std::isfinite(y0) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        width < 1 || height < 1 || width > options.maxRasterDimension ||
        height > options.maxRasterDimension ||
        width * height * (settings.m_bpp / 8) > options.maxArtworkBytes)
      throw std::runtime_error("Bake evaluation domain exceeds memory or size limit.");
    TRasterP raster;
    if (settings.m_bpp == 64) raster = TRaster64P(int(width), int(height));
    else raster = TRaster32P(int(width), int(height));
    raster->clear();
    TTile tile(raster, TPointD(x0, y0));
    Frame captured;
    captured.frame = frame;
    captured.domain = d;
    captured.renderAffine = a;
    captured.bitsPerPixel = settings.m_bpp;
    captured.linearColorSpace = settings.m_linearColorSpace;
    captured.colorSpaceGamma = settings.m_colorSpaceGamma;
    Capture state{source.getPointer(), captured, options, cancel};
    CaptureScope scope(&state);
    if (!computeStandardParticlesForBake(source.getPointer(), tile, frame, settings) &&
        !computeTiledParticlesForBake(source.getPointer(), tile, frame, settings))
      throw std::runtime_error("Unsupported particle effect implementation.");
    if (cancel && cancel()) throw std::runtime_error("Particle bake canceled.");
    // Velocity in interchange is displacement per second; native steps and
    // control-driven drift are already reflected in captured history.
    for (auto &draw : captured.draws) {
      if (!draw.background)
        draw.velocity = draw.velocity * options.framesPerSecond;
    }
    for (auto &particle : captured.particles)
      particle.velocity = particle.velocity * options.framesPerSecond;
    result = std::move(captured);
    return true;
  } catch (const TException &e) { error = QString::fromStdWString(e.getMessage()); }
    catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
    catch (...) { error = "Particle capture failed."; }
  result = Frame();
  return false;
}
}
