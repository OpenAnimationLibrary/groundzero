

#include "tbasefx.h"
#include "trop.h"
#include "tdoubleparam.h"
#include "tnotanimatableparam.h"
#include "trasterfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "texception.h"

// TnzLib includes (header only)
#include "toonz/columncoloreffect.h"
#include "toonz/multiplanedepth.h"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

// #define ALLOW_SHEAR

//==============================================================================

TGeometryFx::TGeometryFx() {
  setName(L"Geometry");
  enableComputeInFloat(true);
}

//---------------------------------------------------------------

void TGeometryFx::doCompute(TTile &tile, double frame,
                            const TRenderSettings &ri) {
  TRasterFxPort *input = dynamic_cast<TRasterFxPort *>(getInputPort(0));
  assert(input);

  if (!input->isConnected()) return;

  if (!getActiveTimeRegion().contains(frame)) {
    TRasterFxP(input->getFx())->compute(tile, frame, ri);
    return;
  }

  if (!TRaster32P(tile.getRaster()) && !TRaster64P(tile.getRaster()) &&
      !TRasterFP(tile.getRaster()))
    throw TException("AffineFx unsupported pixel type");

  TAffine aff1 = getPlacement(frame);
  TRenderSettings ri2(ri);
  ri2.m_affine = ri2.m_affine * aff1;

  TRasterFxP src = getInputPort("source")->getFx();
  src->compute(tile, frame, ri2);
  return;
}

//--------------------------------------------------

bool TGeometryFx::doGetBBox(double frame, TRectD &bBox,
                            const TRenderSettings &info) {
  TRasterFxPort *input = dynamic_cast<TRasterFxPort *>(getInputPort(0));
  assert(input);

  if (input->isConnected()) {
    TRasterFxP fx = input->getFx();
    assert(fx);
    bool ret = fx->doGetBBox(frame, bBox, info);
    if (getActiveTimeRegion().contains(frame))
      bBox = getPlacement(frame) * bBox;
    return ret;
  } else {
    bBox = TRectD();
    return false;
  }
  return true;
};

//--------------------------------------------------

std::string TGeometryFx::getAlias(double frame,
                                  const TRenderSettings &info) const {
  TGeometryFx *tthis = const_cast<TGeometryFx *>(this);
  TAffine affine     = tthis->getPlacement(frame);

  std::string alias = getFxType();
  alias += "[";

  // alias degli effetti connessi alle porte di input separati da virgole
  // una porta non connessa da luogo a un alias vuoto (stringa vuota)

  for (int i = 0; i < getInputPortCount(); ++i) {
    TFxPort *port = getInputPort(i);
    if (port->isConnected()) {
      TRasterFxP ifx = port->getFx();
      assert(ifx);
      alias += ifx->getAlias(frame, info);
    }
    alias += ",";
  }

  return alias +
         (areAlmostEqual(affine.a11, 0) ? "0" : ::to_string(affine.a11, 5)) +
         "," +
         (areAlmostEqual(affine.a12, 0) ? "0" : ::to_string(affine.a12, 5)) +
         "," +
         (areAlmostEqual(affine.a13, 0) ? "0" : ::to_string(affine.a13, 5)) +
         "," +
         (areAlmostEqual(affine.a21, 0) ? "0" : ::to_string(affine.a21, 5)) +
         "," +
         (areAlmostEqual(affine.a22, 0) ? "0" : ::to_string(affine.a22, 5)) +
         "," +
         (areAlmostEqual(affine.a23, 0) ? "0" : ::to_string(affine.a23, 5)) +
         "]";
}

//--------------------------------------------------

void TGeometryFx::transform(double frame, int port, const TRectD &rectOnOutput,
                            const TRenderSettings &infoOnOutput,
                            TRectD &rectOnInput, TRenderSettings &infoOnInput) {
  rectOnInput = rectOnOutput;
  TAffine aff = getPlacement(frame);

  infoOnInput          = infoOnOutput;
  infoOnInput.m_affine = infoOnInput.m_affine * aff;
}

//--------------------------------------------------

bool TGeometryFx::toBeComputedInLinearColorSpace(bool settingsIsLinear,
                                                 bool tileIsLinear) const {
  return tileIsLinear;
}

//==================================================

NaAffineFx::NaAffineFx(bool isDpiAffine)
    : m_aff(TAffine()), m_isDpiAffine(isDpiAffine) {
  addInputPort("source", m_port);
  setName(L"Geometry-NaAffineFx");
}

//--------------------------------------------------

TFx *NaAffineFx::clone(bool recursive) const {
  NaAffineFx *clonedFx = dynamic_cast<NaAffineFx *>(TFx::clone(recursive));
  assert(clonedFx);
  clonedFx->m_aff         = m_aff;
  clonedFx->m_isDpiAffine = m_isDpiAffine;
  return clonedFx;
}

//--------------------------------------------------

void NaAffineFx::doDryCompute(TRectD &rect, double frame,
                              const TRenderSettings &info) {
  // keep the level column dpi affine for an fx which need to be applied to the
  // full sampled image (see Iwa_SmootherFx)
  if (getName() == L"LevelColumn AffineFx") {
    for (auto data : info.m_data) {
      LevelColumnAffineFxRenderData *levelColumnData =
          dynamic_cast<LevelColumnAffineFxRenderData *>(data.getPointer());
      if (levelColumnData && levelColumnData->m_isSet == false) {
        levelColumnData->m_aff   = getPlacement(frame);
        levelColumnData->m_isSet = true;
      }
    }
  }
  TRasterFx::doDryCompute(rect, frame, info);
}

//--------------------------------------------------

FX_IDENTIFIER_IS_HIDDEN(NaAffineFx, "naAffineFx")

//==================================================================
// LevelColumnAffineFxRenderData implementation
//==================================================================

bool LevelColumnAffineFxRenderData::operator==(
    const TRasterFxRenderData &data) const {
  const LevelColumnAffineFxRenderData *theData =
      dynamic_cast<const LevelColumnAffineFxRenderData *>(&data);
  return theData->m_aff == m_aff && theData->m_isSet == m_isSet;
}

//==================================================================
//  ColumnColorFilterFx
//==================================================================

ColumnColorFilterFx::ColumnColorFilterFx() : m_colorFilter(TPixel::Black) {
  setName(L"ColumnColorFilterFx");
  addInputPort("source", m_port);
}

bool ColumnColorFilterFx::doGetBBox(double frame, TRectD &bBox,
                                    const TRenderSettings &info) {
  if (!m_port.isConnected()) return false;
  TRasterFxP fx = m_port.getFx();
  assert(fx);
  bool ret = fx->doGetBBox(frame, bBox, info);
  return ret;
}

void ColumnColorFilterFx::doCompute(TTile &tile, double frame,
                                    const TRenderSettings &ri) {
  if (!m_port.isConnected()) return;

  if (!TRaster32P(tile.getRaster()) && !TRaster64P(tile.getRaster()))
    throw TException("AffineFx unsupported pixel type");

  TRasterFxP src = m_port.getFx();
  src->compute(tile, frame, ri);

  TRop::applyColorScale(tile.getRaster(), m_colorFilter);
}

std::string ColumnColorFilterFx::getAlias(double frame,
                                          const TRenderSettings &info) const {
  std::string alias = getFxType();
  alias += "[";
  if (m_port.isConnected()) {
    TRasterFxP ifx = m_port.getFx();
    assert(ifx);
    alias += ifx->getAlias(frame, info);
  }
  alias += ",";

  return alias + std::to_string(m_colorFilter.r) + "," +
         std::to_string(m_colorFilter.g) + "," +
         std::to_string(m_colorFilter.b) + "," +
         std::to_string(m_colorFilter.m) + "]";
}

//--------------------------------------------------

FX_IDENTIFIER_IS_HIDDEN(ColumnColorFilterFx, "columnColorFilterFx")

//==================================================================
//  ColumnColorEffectFx
//==================================================================

namespace {

// Applies the effect to the premultiplied pixels of a 32 or 64 bit raster
template <typename PIXEL>
void applyColumnColorEffect(const TRasterPT<PIXEL> &ras,
                            const ColumnColorEffect::Transform &effect) {
  typedef typename PIXEL::Channel Channel;
  const double maxValue = (double)PIXEL::maxChannelValue;

  ras->lock();
  for (int y = 0; y < ras->getLy(); ++y) {
    PIXEL *pix = ras->pixels(y), *endPix = pix + ras->getLx();
    for (; pix != endPix; ++pix) {
      if (pix->m == 0) continue;
      double rgba[4] = {(double)pix->r, (double)pix->g, (double)pix->b,
                        (double)pix->m};
      effect.applyPremultiplied(rgba, maxValue);
      pix->r = (Channel)(rgba[0] + 0.5);
      pix->g = (Channel)(rgba[1] + 0.5);
      pix->b = (Channel)(rgba[2] + 0.5);
      pix->m = (Channel)(rgba[3] + 0.5);
    }
  }
  ras->unlock();
}

}  // namespace

//--------------------------------------------------

ColumnColorEffectFx::ColumnColorEffectFx() {
  for (int i = 0; i < 4; ++i) m_m[i] = 1.0, m_c[i] = 0.0;
  setName(L"ColumnColorEffectFx");
  addInputPort("source", m_port);
}

//--------------------------------------------------

TFx *ColumnColorEffectFx::clone(bool recursive) const {
  ColumnColorEffectFx *clonedFx =
      dynamic_cast<ColumnColorEffectFx *>(TFx::clone(recursive));
  assert(clonedFx);
  clonedFx->setColorEffect(m_m, m_c);
  return clonedFx;
}

//--------------------------------------------------

void ColumnColorEffectFx::setColorEffect(const double m[4], const double c[4]) {
  for (int i = 0; i < 4; ++i) m_m[i] = m[i], m_c[i] = c[i];
}

//--------------------------------------------------

bool ColumnColorEffectFx::doGetBBox(double frame, TRectD &bBox,
                                    const TRenderSettings &info) {
  if (!m_port.isConnected()) return false;
  TRasterFxP fx = m_port.getFx();
  assert(fx);
  return fx->doGetBBox(frame, bBox, info);
}

//--------------------------------------------------

void ColumnColorEffectFx::doCompute(TTile &tile, double frame,
                                    const TRenderSettings &ri) {
  if (!m_port.isConnected()) return;

  // Not computed in float: the render hands 32 or 64 bit tiles, in the
  // (non linear) color space of the colors chosen in the column settings
  TRaster32P ras32 = tile.getRaster();
  TRaster64P ras64 = tile.getRaster();
  if (!ras32 && !ras64)
    throw TException("ColumnColorEffectFx unsupported pixel type");

  TRasterFxP src = m_port.getFx();
  src->compute(tile, frame, ri);

  // The tile is premultiplied: the effect is applied to its straight colors
  ColumnColorEffect::Transform effect;
  for (int i = 0; i < 4; ++i) effect.m[i] = m_m[i], effect.c[i] = m_c[i];
  if (ras32)
    applyColumnColorEffect<TPixel32>(ras32, effect);
  else
    applyColumnColorEffect<TPixel64>(ras64, effect);
}

//--------------------------------------------------

std::string ColumnColorEffectFx::getAlias(double frame,
                                          const TRenderSettings &info) const {
  std::string alias = getFxType();
  alias += "[";
  if (m_port.isConnected()) {
    TRasterFxP ifx = m_port.getFx();
    assert(ifx);
    alias += ifx->getAlias(frame, info);
  }

  // The values at the frame, so that each frame is cached on its own
  for (int i = 0; i < 4; ++i)
    alias += "," + std::to_string(m_m[i]) + "," + std::to_string(m_c[i]);
  return alias + "]";
}

//--------------------------------------------------

FX_IDENTIFIER_IS_HIDDEN(ColumnColorEffectFx, "columnColorEffectFx")

//==================================================================
//  ColumnDepthFx
//==================================================================

namespace {

template <typename PIXEL>
void applyColumnDepth(const TRasterPT<PIXEL> &ras,
                      const MultiplaneDepth::Column &depth,
                      const double color[3]) {
  typedef typename PIXEL::Channel Channel;
  const double maxValue = (double)PIXEL::maxChannelValue;

  ras->lock();
  for (int y = 0; y < ras->getLy(); ++y) {
    PIXEL *pix = ras->pixels(y), *endPix = pix + ras->getLx();
    for (; pix != endPix; ++pix) {
      if (pix->m == 0) continue;
      double rgb[3] = {pix->r / maxValue, pix->g / maxValue, pix->b / maxValue};
      const double a = pix->m / maxValue;
      MultiplaneDepth::applyPremultiplied(depth, color, rgb, a);
      // Premultiplied: no channel above the alpha
      for (double &v : rgb) v = std::min(std::max(v, 0.0), a);
      pix->r = (Channel)(rgb[0] * maxValue + 0.5);
      pix->g = (Channel)(rgb[1] * maxValue + 0.5);
      pix->b = (Channel)(rgb[2] * maxValue + 0.5);
    }
  }
  ras->unlock();
}

void applyColumnDepth(const TRasterFP &ras,
                      const MultiplaneDepth::Column &depth,
                      const double color[3]) {
  ras->lock();
  for (int y = 0; y < ras->getLy(); ++y) {
    TPixelF *pix = ras->pixels(y), *endPix = pix + ras->getLx();
    for (; pix != endPix; ++pix) {
      if (pix->m <= 0.f) continue;
      double rgb[3] = {pix->r, pix->g, pix->b};
      MultiplaneDepth::applyPremultiplied(depth, color, rgb, pix->m);
      pix->r = (float)rgb[0];
      pix->g = (float)rgb[1];
      pix->b = (float)rgb[2];
    }
  }
  ras->unlock();
}

}  // namespace

//--------------------------------------------------

ColumnDepthFx::ColumnDepthFx() : m_haze(0.0), m_desaturation(0.0) {
  m_color[0] = m_color[1] = m_color[2] = 1.0;
  setName(L"ColumnDepthFx");
  addInputPort("source", m_port);
  enableComputeInFloat(true);
}

//--------------------------------------------------

TFx *ColumnDepthFx::clone(bool recursive) const {
  ColumnDepthFx *clonedFx =
      dynamic_cast<ColumnDepthFx *>(TFx::clone(recursive));
  assert(clonedFx);
  clonedFx->setDepth(m_haze, m_desaturation, m_color);
  return clonedFx;
}

//--------------------------------------------------

void ColumnDepthFx::setDepth(double haze, double desaturation,
                             const double color[3]) {
  m_haze         = haze;
  m_desaturation = desaturation;
  for (int i = 0; i < 3; ++i) m_color[i] = color[i];
}

//--------------------------------------------------

bool ColumnDepthFx::doGetBBox(double frame, TRectD &bBox,
                              const TRenderSettings &info) {
  if (!m_port.isConnected()) return false;
  TRasterFxP fx = m_port.getFx();
  assert(fx);
  return fx->doGetBBox(frame, bBox, info);
}

//--------------------------------------------------

void ColumnDepthFx::doCompute(TTile &tile, double frame,
                              const TRenderSettings &ri) {
  if (!m_port.isConnected()) return;

  TRaster32P ras32 = tile.getRaster();
  TRaster64P ras64 = tile.getRaster();
  TRasterFP rasF   = tile.getRaster();
  if (!ras32 && !ras64 && !rasF)
    throw TException("ColumnDepthFx unsupported pixel type");

  TRasterFxP src = m_port.getFx();
  src->compute(tile, frame, ri);

  MultiplaneDepth::Column depth;
  depth.haze         = m_haze;
  depth.desaturation = m_desaturation;
  if (ras32)
    applyColumnDepth<TPixel32>(ras32, depth, m_color);
  else if (ras64)
    applyColumnDepth<TPixel64>(ras64, depth, m_color);
  else
    applyColumnDepth(rasF, depth, m_color);
}

//--------------------------------------------------

std::string ColumnDepthFx::getAlias(double frame,
                                    const TRenderSettings &info) const {
  std::ostringstream alias;
  alias.imbue(std::locale::classic());
  alias << std::setprecision(std::numeric_limits<double>::max_digits10)
        << getFxType() << "[";
  if (m_port.isConnected()) {
    TRasterFxP ifx = m_port.getFx();
    assert(ifx);
    alias << ifx->getAlias(frame, info);
  }
  alias << "," << m_haze << "," << m_desaturation << "," << m_color[0] << ","
        << m_color[1] << "," << m_color[2] << "]";
  return alias.str();
}

//--------------------------------------------------

FX_IDENTIFIER_IS_HIDDEN(ColumnDepthFx, "columnDepthFx")

//==================================================================
//  Geometric Fx
//==================================================================

//==================================================================

//==================================================================

//------------------------------------------------------------------------------

class InvertFx final : public TBaseRasterFx {
  FX_DECLARATION(InvertFx)
  TRasterFxPort m_input;
  TBoolParamP m_redChan, m_greenChan, m_blueChan, m_alphaChan;

public:
  InvertFx()
      : m_redChan(true)
      , m_greenChan(true)
      , m_blueChan(true)
      , m_alphaChan(false) {
    addInputPort("source", m_input);
    //   bindParam(this, "red_channel"  , m_redChan);
    //   bindParam(this, "green_channel", m_greenChan);
    //   bindParam(this, "blue_channel" , m_blueChan);
    //   bindParam(this, "alpha_channel", m_alphaChan);
    setName(L"InvertFx");
    enableComputeInFloat(true);
  };

  ~InvertFx() {};

  bool canHandle(const TRenderSettings &info, double frame) override {
    return true;
  }

  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override {
    if (m_input.isConnected()) {
      bool ret = m_input->doGetBBox(frame, bBox, info);
      return ret;
    } else {
      bBox = TRectD();
      return false;
    }
  };

  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &ri) override {
    if (!m_input.isConnected()) return;

    m_input->compute(tile, frame, ri);

    TRop::invert(tile.getRaster(), m_redChan->getValue(),
                 m_greenChan->getValue(), m_blueChan->getValue(),
                 m_alphaChan->getValue());
  }
};

//==================================================================
//  Video Fx
//==================================================================

//==================================================================

//===

// Geometric
// FX_IDENTIFIER(ScaleFx,       "scaleFx")
// FX_IDENTIFIER(MoveFx,        "moveFx")
// FX_IDENTIFIER(AffineFx,      "affineFx")
// FX_IDENTIFIER(CropFx,        "cropFx")

// Color
FX_IDENTIFIER(InvertFx, "invertFx")

// Video
// FX_IDENTIFIER(FieldFx,       "fieldFx")
// FX_IDENTIFIER(SwapFieldsFx,  "swapFieldsFx")
// FX_IDENTIFIER(DeInterlaceFx, "deInterlaceFx")
// FX_IDENTIFIER(InterlaceFx,   "interlaceFx")
