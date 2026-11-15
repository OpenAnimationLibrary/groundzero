#include "stdfx.h"
#include "tfxparam.h"
#include "tfxattributes.h"
#include "tparamset.h"
#include "tpixelutils.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tdoublekeyframe.h"
#include "tsimplecolorstyles.h"
#include "tstroke.h"
#include "tthreadmessage.h"
#include "tvectorimage.h"
#include "toonz/columncoloreffect.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcolumnfx.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "roughstroke.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Rough Stroke: hand-drawn strokes along the strokes of a vector level. The
// FX reads the drawing's strokes from the level column behind Source, so
// Source must be a vector level, directly or through transforms and the
// nodes the render puts over a column. Any other source passes through
// unchanged. The math is in roughstroke.h.
namespace {
double bounded(double value, double minimum, double maximum) {
  return std::isfinite(value) ? std::max(minimum, std::min(maximum, value))
                              : minimum;
}
int discrete(double value, int minimum, int maximum) {
  return int(std::floor(bounded(value, minimum, maximum) + 0.5));
}
double scaleOf(const TAffine &affine) {
  const double scale = std::sqrt(std::abs(affine.det()));
  return std::isfinite(scale) && scale > 0 ? scale : 0.0;
}
class RasterLock {
  TRasterP m_raster;

public:
  explicit RasterLock(const TRasterP &raster) : m_raster(raster) {
    m_raster->lock();
  }
  ~RasterLock() { m_raster->unlock(); }
};

// The level column behind an FX, and what lies between them
struct Behind {
  TLevelColumnFx *column = nullptr;
  TAffine placement;          // the transforms' placement of the drawing
  bool colourFilter = false;  // the column's colour filter and opacity
  bool colourEffect = false;  // the column's Colour Effect
};

// The level column behind an FX, through enabled transforms and the nodes
// the render puts over a column before the FX that follow it (FxBuilder::
// makePF in toonzlib/scenefx.cpp). Their classes are not exported, so they
// are told by type, as in rendersVectorStrokes() (toonzlib/
// strokedeformerdata.cpp). Multiplane Depth's haze and blur are seen through
// but not reproduced. Any other FX hides the column.
Behind columnBehind(TFx *source, double frame) {
  Behind behind;
  std::set<TFx *> visited;
  while (source) {
    if (!visited.insert(source).second) return Behind();
    if (auto *column = dynamic_cast<TLevelColumnFx *>(source)) {
      behind.column = column;
      return behind;
    }
    if (!source->getAttributes()->isEnabled() ||
        source->getInputPortCount() != 1 ||
        !source->getInputPort(0)->isConnected())
      return Behind();
    const std::string type = source->getFxType();
    auto *geometry         = dynamic_cast<TGeometryFx *>(source);
    if (type == "columnColorFilterFx")
      behind.colourFilter = true;
    else if (type == "columnColorEffectFx")
      behind.colourEffect = true;
    else if (geometry)
      behind.placement = behind.placement * geometry->getPlacement(frame);
    // The depth blur is a Blur FX that the render made, so it has no id
    else if (type != "columnDepthFx" &&
             !(type == "STD_blurFx" && source->getFxId().empty()))
      return Behind();
    source = source->getInputPort(0)->getFx();
  }
  return Behind();
}

// The strokes a render hides (tglDraw in common/tvrender/tglregions.cpp):
// those without opacity, those whose style draws no stroke, and plain colour
// ones without thickness
bool hidden(const TStroke &stroke, const TColorStyle *style) {
  if (!(stroke.getOpacity() > 0)) return true;
  if (style && (!style->isStrokeStyle() || !style->isEnabled())) return true;
  if (style && !dynamic_cast<const TSolidColorStyle *>(style)) return false;
  for (int i = 0; i < stroke.getControlPointCount(); ++i)
    if (stroke.getControlPoint(i).thick != 0) return false;
  return true;
}

// A style's colour at a frame of the palette's animation, without setting
// the frame of the palette, which other renders and the viewer share
TPixel32 colourAt(const TPalette &palette, int styleId, int frame,
                  std::map<int, TPixel32> &colours) {
  const auto found = colours.find(styleId);
  if (found != colours.end()) return found->second;
  const TPixel32 colour = palette.sampleStyle(styleId, frame)->getMainColor();
  colours.emplace(styleId, colour);
  return colour;
}

// The drawing's strokes as paths in output pixels. Returns false when the
// source is not a vector drawing.
bool readPaths(const Behind &behind, double frame, const TAffine &worldToOutput,
               std::vector<RoughStroke::Path> &paths) {
  paths.clear();
  if (!behind.column || !behind.column->getColumn()) return false;
  // The row and the palette frame the column draws (TLevelColumnFx::
  // doCompute)
  const int row       = int(frame);
  const TXshCell cell = behind.column->getColumn()->getRemappedCell(row);
  // Other levels are not loaded only to be passed through
  const TXshSimpleLevel *level =
      cell.isEmpty() ? nullptr : cell.getSimpleLevel();
  if (!level || level->getType() != PLI_XSHLEVEL) return false;
  const TVectorImageP image = cell.getImage(false);
  if (!image) return false;
  const TAffine toOutput = worldToOutput * behind.placement;
  const double scale     = scaleOf(toOutput);
  if (!(scale > 0)) return true;  // a vector drawing with nothing to draw
  const TPalette *palette = image->getPalette();
  std::vector<int> styles;  // of the paths
  {
    TThread::MutexLocker lock(image->getMutex());
    for (UINT i = 0; i < image->getStrokeCount(); ++i) {
      const TStroke *stroke = image->getStroke(i);
      const int styleId     = stroke->getStyle();
      if (hidden(*stroke, palette ? palette->getStyle(styleId) : nullptr))
        continue;
      const double length = stroke->getLength();
      if (!(length > 0) || !std::isfinite(length)) continue;
      // About one point per output pixel
      const double pixels = length * scale;
      if (pixels > double(RoughStroke::maxVertices))
        throw TException("Rough Stroke: a stroke is too long for the raster");
      const int count = std::max(1, int(std::ceil(pixels)));
      RoughStroke::Path path;
      path.points.reserve(size_t(count) + 1);
      for (int k = 0; k <= count; ++k) {
        const double along  = length * k / count;
        const TThickPoint p = stroke->getThickPointAtLength(along);
        const TPointD q     = toOutput * TPointD(p.x, p.y);
        path.points.push_back({q.x, q.y, std::max(0.0, p.thick) * scale,
                               stroke->getOpacityAtLength(along)});
      }
      paths.push_back(std::move(path));
      styles.push_back(styleId);
    }
  }
  // Outside the drawing's lock: the column draws an animated palette under
  // the palette's lock, then takes the drawing's
  std::map<int, TPixel32> colours;
  for (size_t i = 0; i < paths.size(); ++i) {
    const TPixel32 colour =
        palette ? colourAt(*palette, styles[i], row, colours) : TPixel32::Black;
    paths[i].color = {colour.r / 255.0, colour.g / 255.0, colour.b / 255.0,
                      colour.m / 255.0};
  }
  return true;
}

// The colour filter the render gives a column (FxBuilder::makePF): the
// filter's colour, with its alpha scaled by the column's opacity
RoughStroke::Rgba colourFilter(const TXshLevelColumn &column) {
  TPixel32 colour         = TPixel32::Black;  // no filter
  const TXsheet *xsheet   = column.getXsheet();
  const ToonzScene *scene = xsheet ? xsheet->getScene() : nullptr;
  const TSceneProperties *properties =
      scene ? scene->getProperties() : nullptr;
  const int id = column.getColorFilterId();
  if (properties && id >= 0 && id < properties->getColorFilters().size())
    colour = properties->getColorFilterColor(id);
  colour.m = TPixel32::Channel(int(colour.m) * int(column.getOpacity()) /
                               TPixel32::maxChannelValue);
  return {colour.r / 255.0, colour.g / 255.0, colour.b / 255.0,
          colour.m / 255.0};
}

// Does to the strokes what the nodes between the column and the FX do to
// the drawing, in the render's order: the colour filter, then the effect
void applyColumnColours(const Behind &behind, double frame,
                        std::vector<RoughStroke::Rgba> &pixels) {
  if (!behind.colourFilter && !behind.colourEffect) return;
  const RoughStroke::Rgba filter =
      behind.colourFilter ? colourFilter(*behind.column->getColumn())
                          : RoughStroke::Rgba{0, 0, 0, 1};
  const ColumnColorEffect::Transform effect =
      behind.colourEffect ? behind.column->getColorEffect(frame)
                          : ColumnColorEffect::Transform();
  for (RoughStroke::Rgba &pixel : pixels) {
    if (!(pixel.a > 0)) continue;
    if (behind.colourFilter) RoughStroke::filter(pixel, filter);
    if (behind.colourEffect) {
      double rgba[4] = {pixel.r, pixel.g, pixel.b, pixel.a};
      effect.applyPremultiplied(rgba, 1.0);
      pixel = {rgba[0], rgba[1], rgba[2], rgba[3]};
    }
  }
}

template <class Pixel>
void write(const TRasterPT<Pixel> &output,
           const std::vector<RoughStroke::Rgba> &strokes, bool over) {
  RasterLock lock(output);
  const int w = output->getLx();
  for (int y = 0; y < output->getLy(); ++y) {
    Pixel *row = output->pixels(y);
    for (int x = 0; x < w; ++x) {
      const RoughStroke::Rgba &c = strokes[size_t(y) * w + x];
      if (!(c.a > 0)) continue;
      TPixelD result(c.r, c.g, c.b, c.a);
      if (over) {
        const TPixelD under = PixelConverter<TPixelD>::from(row[x]);
        result.r += under.r * (1 - c.a);
        result.g += under.g * (1 - c.a);
        result.b += under.b * (1 - c.a);
        result.m += under.m * (1 - c.a);
      }
      row[x] = PixelConverter<Pixel>::from(result);
    }
  }
}
}  // namespace

class RoughStrokeFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(RoughStrokeFx)
  TRasterFxPort m_source;
  TDoubleParamP m_width, m_thicknessScale, m_inLength, m_outLength, m_copies,
      m_copyOffset, m_copyWidthRandom, m_copyOpacityRandom, m_waveAmplitude,
      m_wavePeriod, m_wavePhase, m_wavePhaseRandom, m_drift, m_driftSize,
      m_colorMode, m_opacity, m_revealStart, m_revealEnd, m_travel, m_speed,
      m_hold, m_seed;
  TPixelParamP m_startColor, m_endColor;
  TBoolParamP m_loop, m_overSource;

  static RoughStroke::Rgba colour(const TPixelParamP &param, double frame) {
    const TPixelD c = param->getValueD(frame);
    return RoughStroke::Rgba{c.r, c.g, c.b, c.m};
  }

  // How far the stretch has moved by frame, as a share of the length: by
  // Speed every frame. An animated Speed is added up frame by frame, so
  // slowing down never moves the stretch back.
  double travelled(double frame) const {
    auto speed = [this](int f) {
      return bounded(m_speed->getValue(f), -1e3, 1e3);
    };
    if (!m_speed->hasKeyframes()) return speed(0) * frame;
    // The speed holds before its first key and after its last unless it
    // cycles, and is taken to hold past the bound (about seven minutes at 24
    // frames a second)
    const double bound = 10000;
    const double start = m_speed->getKeyframe(0).m_frame;
    const double end =
        m_speed->getKeyframe(m_speed->getKeyframeCount() - 1).m_frame;
    const double first = bounded(std::floor(start), -bound, 0);
    const double last =
        m_speed->isCycleEnabled() ? bound : bounded(std::ceil(end), 0, bound);
    return RoughStroke::covered(speed, frame, int(first), int(last));
  }

public:
  // Distances are entered in scene units and used in output pixels
  RoughStroke::Settings settings(double frame, double scale) const {
    RoughStroke::Settings s;
    s.width = bounded(m_width->getValue(frame), 0, 10000) * scale;
    s.thicknessScale = bounded(m_thicknessScale->getValue(frame), 0, 100);
    s.inLength       = bounded(m_inLength->getValue(frame), 0, 1);
    s.outLength      = bounded(m_outLength->getValue(frame), 0, 1);
    s.copies =
        discrete(m_copies->getValue(frame), 1, RoughStroke::maxCopies);
    s.copyOffset = bounded(m_copyOffset->getValue(frame), 0, 10000) * scale;
    s.copyWidthRandom   = bounded(m_copyWidthRandom->getValue(frame), 0, 1);
    s.copyOpacityRandom = bounded(m_copyOpacityRandom->getValue(frame), 0, 1);
    s.waveAmplitude =
        bounded(m_waveAmplitude->getValue(frame), 0, 10000) * scale;
    s.wavePeriod =
        std::max(1.0, bounded(m_wavePeriod->getValue(frame), 0.1, 100000) *
                          scale);
    s.wavePhase = bounded(m_wavePhase->getValue(frame), -1e6, 1e6) *
                  RoughStroke::pi / 180.0;
    s.wavePhaseRandom = bounded(m_wavePhaseRandom->getValue(frame), 0, 1);
    s.drift = bounded(m_drift->getValue(frame), 0, 10000) * scale;
    s.driftSize =
        std::max(1.0, bounded(m_driftSize->getValue(frame), 0.1, 100000) *
                          scale);
    s.useStrokeColor = discrete(m_colorMode->getValue(frame), 0, 1) == 0;
    s.startColor     = colour(m_startColor, frame);
    s.endColor       = colour(m_endColor, frame);
    s.opacity        = bounded(m_opacity->getValue(frame), 0, 1);
    s.revealStart    = bounded(m_revealStart->getValue(frame), 0, 1);
    s.revealEnd      = bounded(m_revealEnd->getValue(frame), 0, 1);
    // The stretch moves by Speed every frame without any key
    s.travel = bounded(m_travel->getValue(frame), -1e6, 1e6) + travelled(frame);
    s.loop   = m_loop->getValue();
    s.seed   = RoughStroke::frameSeed(
        std::uint32_t(discrete(m_seed->getValue(frame), 0, 100000)), frame,
        discrete(m_hold->getValue(frame), 0, 1000));
    return s;
  }

  RoughStrokeFx()
      : m_width(2.0)
      , m_thicknessScale(1.0)
      , m_inLength(0.15)
      , m_outLength(0.15)
      , m_copies(1.0)
      , m_copyOffset(3.0)
      , m_copyWidthRandom(0.3)
      , m_copyOpacityRandom(0.0)
      , m_waveAmplitude(0.0)
      , m_wavePeriod(40.0)
      , m_wavePhase(0.0)
      , m_wavePhaseRandom(1.0)
      , m_drift(1.0)
      , m_driftSize(30.0)
      , m_colorMode(0.0)
      , m_opacity(1.0)
      , m_revealStart(0.0)
      , m_revealEnd(1.0)
      , m_travel(0.0)
      , m_speed(0.0)
      , m_hold(2.0)
      , m_seed(1.0)
      , m_startColor(TPixel32::Black)
      , m_endColor(TPixel32::Black)
      , m_loop(false)
      , m_overSource(false) {
    addInputPort("Source", m_source);
    for (TDoubleParamP *distance :
         {&m_width, &m_copyOffset, &m_waveAmplitude, &m_wavePeriod, &m_drift,
          &m_driftSize})
      (*distance)->setMeasureName("fxLength");
    m_width->setValueRange(0, 10000);
    m_thicknessScale->setValueRange(0, 100);
    m_thicknessScale->setUISliderRange(0, 4);
    m_copies->setValueRange(1, RoughStroke::maxCopies);
    m_copies->setUISliderRange(1, 12);
    m_copyOffset->setValueRange(0, 10000);
    m_waveAmplitude->setValueRange(0, 10000);
    m_wavePeriod->setValueRange(0.1, 100000);
    m_wavePhase->setMeasureName("angle");
    m_drift->setValueRange(0, 10000);
    m_driftSize->setValueRange(0.1, 100000);
    m_colorMode->setValueRange(0, 1);
    m_colorMode->setUIChoices({"Stroke Colour", "Gradient"});
    for (TDoubleParamP *share :
         {&m_inLength, &m_outLength, &m_copyWidthRandom, &m_copyOpacityRandom,
          &m_wavePhaseRandom, &m_opacity, &m_revealStart, &m_revealEnd})
      (*share)->setValueRange(0, 1);
    m_travel->setValueRange(-1e6, 1e6);
    m_travel->setUISliderRange(-1, 1);
    m_speed->setValueRange(-1e3, 1e3);
    m_speed->setUISliderRange(-0.2, 0.2);
    m_hold->setValueRange(0, 1000);
    m_hold->setUISliderRange(0, 12);
    m_seed->setValueRange(0, 100000);
    m_seed->setUISliderRange(0, 1000);
    bindParam(this, "width", m_width);
    bindParam(this, "thicknessScale", m_thicknessScale);
    bindParam(this, "inLength", m_inLength);
    bindParam(this, "outLength", m_outLength);
    bindParam(this, "copies", m_copies);
    bindParam(this, "copyOffset", m_copyOffset);
    bindParam(this, "copyWidthRandom", m_copyWidthRandom);
    bindParam(this, "copyOpacityRandom", m_copyOpacityRandom);
    bindParam(this, "waveAmplitude", m_waveAmplitude);
    bindParam(this, "wavePeriod", m_wavePeriod);
    bindParam(this, "wavePhase", m_wavePhase);
    bindParam(this, "wavePhaseRandom", m_wavePhaseRandom);
    bindParam(this, "drift", m_drift);
    bindParam(this, "driftSize", m_driftSize);
    bindParam(this, "colorMode", m_colorMode);
    bindParam(this, "startColor", m_startColor);
    bindParam(this, "endColor", m_endColor);
    bindParam(this, "opacity", m_opacity);
    bindParam(this, "revealStart", m_revealStart);
    bindParam(this, "revealEnd", m_revealEnd);
    bindParam(this, "travel", m_travel);
    bindParam(this, "speed", m_speed);
    bindParam(this, "loop", m_loop);
    bindParam(this, "hold", m_hold);
    bindParam(this, "seed", m_seed);
    bindParam(this, "overSource", m_overSource);
    enableComputeInFloat(true);
  }
  bool requiresFullPrecisionSerialization() const override { return true; }
  // Rotation and an even scale are drawn; the render does a stretch, after
  // drawing at the larger of its scales
  bool canHandle(const TRenderSettings &info, double) override {
    return info.m_affine.isIsotropic(0.001);
  }
  bool toBeComputedInLinearColorSpace(bool, bool) const override {
    return false;
  }
  // The strokes as doubles, and the coverage and colour of the window of a
  // copy, up to the whole tile
  int getMemoryRequirement(const TRectD &rect, double,
                           const TRenderSettings &) override {
    if (!m_source.isConnected()) return 0;
    const double perPixel =
        sizeof(RoughStroke::Rgba) + sizeof(double) + sizeof(RoughStroke::Rgba);
    const double megabytes =
        std::ceil(rect.getLx() * rect.getLy() * perPixel / (1024.0 * 1024.0));
    return int(std::min(double(std::numeric_limits<int>::max()),
                        std::max(0.0, megabytes)));
  }
  bool doGetBBox(double frame, TRectD &box,
                 const TRenderSettings &info) override {
    box = TRectD();
    if (!m_source.isConnected()) return false;
    std::vector<RoughStroke::Path> paths;
    try {
      const Behind behind = columnBehind(m_source.getFx(), frame);
      if (!readPaths(behind, frame, TAffine(), paths))
        return m_source->doGetBBox(frame, box, info);  // passed through
    } catch (const TException &) {
      return m_source->doGetBBox(frame, box, info);
    }
    // The strokes in scene units, grown by how far a copy can reach
    const RoughStroke::Settings s = settings(frame, 1.0);
    const double reach            = RoughStroke::reach(s);
    for (const RoughStroke::Path &path : paths)
      for (const RoughStroke::Point &p : path.points) {
        const double r = p.thickness * s.thicknessScale + reach;
        box += TRectD(p.x - r, p.y - r, p.x + r, p.y + r);
      }
    if (m_overSource->getValue()) {
      TRectD source;
      if (m_source->doGetBBox(frame, source, info)) box += source;
    }
    return !box.isEmpty();
  }
  // The stretch travels and the seed holds without any parameter changing
  std::string getAlias(double frame,
                       const TRenderSettings &info) const override {
    const RoughStroke::Settings s = settings(frame, 1.0);
    std::ostringstream alias;
    alias.imbue(std::locale::classic());
    alias << TRasterFx::getAlias(frame, info) << "[roughStroke:1:"
          << std::setprecision(std::numeric_limits<double>::max_digits10)
          << s.travel << ',' << s.seed << ']';
    return alias.str();
  }
  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &info) override {
    tile.getRaster()->clear();
    if (!m_source.isConnected() || (info.m_isCanceled && *info.m_isCanceled))
      return;
    const Behind behind = columnBehind(m_source.getFx(), frame);
    std::vector<RoughStroke::Path> paths;
    const bool vector = readPaths(behind, frame, info.m_affine, paths);
    // Not a vector drawing: pass it through. Over Source: draw on top of it.
    if (!vector || m_overSource->getValue()) {
      m_source->compute(tile, frame, info);
      if (info.m_isCanceled && *info.m_isCanceled) {
        tile.getRaster()->clear();
        return;
      }
      if (!vector) return;
    }
    const int w = tile.getRaster()->getLx(), h = tile.getRaster()->getLy();
    std::vector<RoughStroke::Rgba> strokes(size_t(w) * size_t(h));
    try {
      const RoughStroke::Settings s =
          settings(frame, scaleOf(info.m_affine));
      for (size_t i = 0; i < paths.size(); ++i) {
        if (info.m_isCanceled && *info.m_isCanceled) {
          tile.getRaster()->clear();
          return;
        }
        for (const RoughStroke::Copy &copy :
             RoughStroke::build(paths[i], int(i), s))
          RoughStroke::draw(copy, w, h, tile.m_pos.x, tile.m_pos.y, strokes);
      }
    } catch (const std::exception &error) {
      throw TException(error.what());
    }
    applyColumnColours(behind, frame, strokes);
    const bool over = m_overSource->getValue();
    if (TRaster32P output = tile.getRaster())
      write<TPixel32>(output, strokes, over);
    else if (TRaster64P output = tile.getRaster())
      write<TPixel64>(output, strokes, over);
    else if (TRasterFP output = tile.getRaster())
      write<TPixelF>(output, strokes, over);
    else
      throw TException("Rough Stroke: unsupported pixel type");
  }
};
FX_PLUGIN_IDENTIFIER(RoughStrokeFx, "roughStrokeFx")
