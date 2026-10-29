#include "particlebakecapture.h"
#include "trasterfx.h"
#include "ttile.h"
#include "tconvert.h"
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
struct Capture {
  const TFx *source;
  ParticleBake::Frame &frame;
  const ParticleBake::Options &options;
  const ParticleBake::Cancel &cancel;
  TAffine inverseEvaluationAffine;
  ParticleBake::DrawSample draw;
  std::uint64_t bytes = 0;
};
thread_local Capture *current = nullptr;
struct CaptureScope {
  Capture *previous;
  explicit CaptureScope(Capture *capture) : previous(current) { current = capture; }
  ~CaptureScope() { current = previous; }
};

void isolateGraphIdentifiers(TFx *source) {
  std::set<TFx *> visited;
  std::vector<TFx *> pending{source};
  while (!pending.empty()) {
    TFx *fx = pending.back();
    pending.pop_back();
    if (!visited.insert(fx).second) continue;
    // Recursive cloning retains IDs. Upstream compute() can hit a cached
    // particle raster before reaching the engine's private-state hook, so the
    // detached graph also needs its own render-cache identities.
    fx->setNewIdentifier();
    for (int port = 0; port < fx->getInputPortCount(); ++port) {
      TFx *input = fx->getInputPort(port)->getFx();
      if (input) pending.push_back(input);
    }
  }
}
}
namespace ParticleBakeCapture {
bool active(const TFx *source) { return current && current->source == source; }
bool isolating() { return current != nullptr; }
void checkCancel(const TFx *) {
  if (isolating() && current->cancel && current->cancel())
    throw std::runtime_error("Particle bake canceled.");
}
int artworkLastFrame(const TFx *source, const TFxTimeRegion &region) {
  if (!isolating()) return region.getLastFrame();
  checkCancel(source);
  if (region.isEmpty())
    throw std::runtime_error("Particle artwork has an empty frame range.");
  int last = region.getLastFrame();
  if (last == std::numeric_limits<int>::max())
    last = current->options.proceduralArtworkLastFrame;
  if (last < 0 || last > 1000000)
    throw std::runtime_error("Particle artwork frame range exceeds bake limits.");
  return last;
}
void checkParticleCount(const TFx *source, double count) {
  if (!isolating()) return;
  checkCancel(source);
  if (!std::isfinite(count) || count < 0 || count > current->options.maxDraws)
    throw std::runtime_error("Particle simulation exceeds bake particle limit.");
}
void checkRasterSize(const TFx *, double width, double height, int pixelSize) {
  if (!isolating()) return;
  if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0 ||
      width > current->options.maxRasterDimension || height > current->options.maxRasterDimension ||
      width * height * pixelSize > current->options.maxArtworkBytes)
    throw std::runtime_error("Particle sampling exceeds bake raster limit.");
}
void checkSimulationRange(const TFx *, int start, int end) {
  if (!isolating()) return;
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
  sample.rasterToWorld = current->inverseEvaluationAffine *
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
                  const Cancel &cancel, TRasterP *evaluatedRaster) {
  error.clear();
  result = Frame();
  if (evaluatedRaster) *evaluatedRaster = TRasterP();
  if (!isSupported(fx)) { error = "Select a Particles or Tiled Particles FX."; return false; }
  const TRectD &d = options.domain;
  const TAffine &a = settings.m_affine;
  const TRectD &viewport = options.pixelDomain;
  const double determinant = a.det();
  if (!std::isfinite(frame) || frame < std::numeric_limits<int>::min() ||
      frame > std::numeric_limits<int>::max() || std::floor(frame) != frame ||
      !std::isfinite(options.framesPerSecond) || options.framesPerSecond <= 0 ||
      !std::isfinite(d.x0) || !std::isfinite(d.x1) ||
      !std::isfinite(d.y0) || !std::isfinite(d.y1) || d.isEmpty() ||
      !std::isfinite(viewport.x0) || !std::isfinite(viewport.x1) ||
      !std::isfinite(viewport.y0) || !std::isfinite(viewport.y1) ||
      !std::isfinite(a.a11) || !std::isfinite(a.a12) || !std::isfinite(a.a13) ||
      !std::isfinite(a.a21) || !std::isfinite(a.a22) || !std::isfinite(a.a23) ||
      !std::isfinite(determinant) || determinant == 0 ||
      options.maxRasterDimension <= 0 || options.maxArtworkBytes == 0 ||
      options.maxDraws == 0 || options.proceduralArtworkLastFrame < 0 ||
      options.proceduralArtworkLastFrame > 1000000 ||
      !std::isfinite(settings.m_colorSpaceGamma) ||
      settings.m_colorSpaceGamma <= 0) {
    error = "Bake requires an integer frame, positive FPS, finite domain/viewport and invertible finite affine.";
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
    isolateGraphIdentifiers(source.getPointer());
    const bool explicitViewport = !viewport.isEmpty();
    const TRectD pixels = explicitViewport ? viewport : a * d;
    // An explicit viewport keeps its pixel-grid origin, including half-pixel
    // camera origins for odd output dimensions. Only inferred world bounds are
    // expanded to an integer grid.
    const double x0 = explicitViewport ? pixels.x0 : std::floor(pixels.x0);
    const double y0 = explicitViewport ? pixels.y0 : std::floor(pixels.y0);
    const double width = explicitViewport ? std::ceil(pixels.getLx()) : std::ceil(pixels.x1) - x0;
    const double height = explicitViewport ? std::ceil(pixels.getLy()) : std::ceil(pixels.y1) - y0;
    if (!std::isfinite(x0) || !std::isfinite(y0) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        width < 1 || height < 1 || width > options.maxRasterDimension ||
        height > options.maxRasterDimension ||
        width * height * (settings.m_bpp / 8) > options.maxArtworkBytes)
      throw std::runtime_error("Bake evaluation domain exceeds memory or size limit.");
    // Match TRasterFx::compute's translation canonicalization. Native tiled
    // particles use the evaluation tile's location to place their initial grid;
    // bypassing this step changes the simulation for ordinary column movement.
    // Keep caller coordinates in Frame metadata while dispatching in the same
    // normalized space that the live renderer supplies to doCompute.
    const double integerX = std::floor(x0), integerY = std::floor(y0);
    const double fractionalX = x0 - integerX, fractionalY = y0 - integerY;
    const double fractionalTranslationX = a.a13 - fractionalX;
    const double fractionalTranslationY = a.a23 - fractionalY;
    const double translateX = std::floor(fractionalTranslationX);
    const double translateY = std::floor(fractionalTranslationY);
    const TPointD evaluationOrigin(integerX - translateX, integerY - translateY);
    const double coordinateLimit = double(std::numeric_limits<int>::max()) - 1;
    if (!std::isfinite(evaluationOrigin.x) || !std::isfinite(evaluationOrigin.y) ||
        std::abs(x0) + width > coordinateLimit || std::abs(y0) + height > coordinateLimit ||
        std::abs(evaluationOrigin.x) + width > coordinateLimit ||
        std::abs(evaluationOrigin.y) + height > coordinateLimit)
      throw std::runtime_error("Particle bake viewport exceeds renderer coordinate limits.");
    TRenderSettings evaluationSettings(settings);
    evaluationSettings.m_affine.a13 = fractionalTranslationX - translateX;
    evaluationSettings.m_affine.a23 = fractionalTranslationY - translateY;
    const TAffine inverseEvaluation = evaluationSettings.m_affine.inv();
    for (double value : {inverseEvaluation.a11, inverseEvaluation.a12, inverseEvaluation.a13,
                         inverseEvaluation.a21, inverseEvaluation.a22, inverseEvaluation.a23})
      if (!std::isfinite(value))
        throw std::runtime_error("Particle bake inverse affine is not finite.");
    TRasterP raster;
    if (settings.m_bpp == 64) raster = TRaster64P(int(width), int(height));
    else raster = TRaster32P(int(width), int(height));
    raster->clear();
    TTile tile(raster, evaluationOrigin);
    Frame captured;
    captured.frame = frame;
    captured.domain = d;
    captured.pixelDomain = TRectD(x0, y0, x0 + width, y0 + height);
    captured.renderAffine = a;
    captured.bitsPerPixel = settings.m_bpp;
    captured.linearColorSpace = settings.m_linearColorSpace;
    captured.colorSpaceGamma = settings.m_colorSpaceGamma;
    Capture state{source.getPointer(), captured, options, cancel, inverseEvaluation};
    CaptureScope scope(&state);
    if (!computeStandardParticlesForBake(source.getPointer(), tile, frame, evaluationSettings) &&
        !computeTiledParticlesForBake(source.getPointer(), tile, frame, evaluationSettings))
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
    if (evaluatedRaster) *evaluatedRaster = tile.getRaster();
    return true;
  } catch (const TException &e) { error = QString::fromStdWString(e.getMessage()); }
    catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
    catch (...) { error = "Particle capture failed."; }
  result = Frame();
  return false;
}
}
