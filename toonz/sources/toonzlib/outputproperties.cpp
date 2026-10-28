

#include "toutputproperties.h"

// TnzLib includes
#include "toonz/boardsettings.h"
#include "toonz/cameraguide.h"

// TnzBase includes
#include "trasterfx.h"

// TnzCore includes
#include "tiio.h"
#include "tproperty.h"
#include "tpixelutils.h"

#include <algorithm>
#include <vector>

//**********************************************************************************
//    Local namespace  stuff
//**********************************************************************************

namespace {

inline void deleteValue(const std::pair<std::string, TPropertyGroup *> &p) {
  delete p.second;
}

// Fills the part of ras outside keep, in the pixel type of the raster; color
// is converted to linear for a raster in linear color space.
void fillOutside(const TRasterP &ras, const TRect &keep, const TPixel32 &color,
                 double linearGamma) {
  const TRect bounds = ras->getBounds();
  std::vector<TRect> bars;
  if (keep.y1 < bounds.y1)
    bars.push_back(TRect(bounds.x0, keep.y1 + 1, bounds.x1, bounds.y1));
  if (bounds.y0 < keep.y0)
    bars.push_back(TRect(bounds.x0, bounds.y0, bounds.x1, keep.y0 - 1));
  if (bounds.x0 < keep.x0)
    bars.push_back(TRect(bounds.x0, keep.y0, keep.x0 - 1, keep.y1));
  if (keep.x1 < bounds.x1)
    bars.push_back(TRect(keep.x1 + 1, keep.y0, bounds.x1, keep.y1));

  const bool linear  = ras->isLinear();
  const TPixel32 pix = premultiply(color);
  for (TRect bar : bars) {
    TRasterP sub = ras->extract(bar);
    if (TRaster32P ras32 = sub)
      ras32->fill(linear ? toLinear(pix, linearGamma) : pix);
    else if (TRaster64P ras64 = sub) {
      const TPixel64 pix64 = toPixel64(pix);
      ras64->fill(linear ? toLinear(pix64, linearGamma) : pix64);
    } else if (TRasterFP rasF = sub) {
      const TPixelF pixF = toPixelF(pix);
      rasF->fill(linear ? toLinear(pixF, linearGamma) : pixF);
    }
  }
}

}  // namespace

//**********************************************************************************
//    TOutputProperties  implementation
//**********************************************************************************

TOutputProperties::TOutputProperties()
    : m_path(TFilePath("+outputs") + ".tif")
    , m_renderSettings()
    , m_frameRate(24)
    , m_from(0)
    , m_to(-1)
    , m_offset(0)
    , m_step(1)
    , m_whichLevels(false)
    , m_multimediaRendering(0)
    , m_maxTileSizeIndex(0)
    , m_threadIndex(2)
    , m_subcameraPreview(false)
    , m_boardSettings(new BoardSettings())
    , m_formatTemplateFId()
    , m_syncColorSettings(true)
    , m_aspectMode(NoAspectExport)
    , m_aspectRatio(1.85)
    , m_aspectColor(TPixel32::Black) {
  m_renderSettings = new TRenderSettings();
  m_nonlinearBpp   = m_renderSettings->m_bpp;
}

//-------------------------------------------------------------------

TOutputProperties::TOutputProperties(const TOutputProperties &src)
    : m_path(src.m_path)
    , m_formatProperties(src.m_formatProperties)
    , m_renderSettings(new TRenderSettings(*src.m_renderSettings))
    , m_frameRate(src.m_frameRate)
    , m_from(src.m_from)
    , m_to(src.m_to)
    , m_whichLevels(src.m_whichLevels)
    , m_offset(src.m_offset)
    , m_step(src.m_step)
    , m_multimediaRendering(src.m_multimediaRendering)
    , m_maxTileSizeIndex(src.m_maxTileSizeIndex)
    , m_threadIndex(src.m_threadIndex)
    , m_subcameraPreview(src.m_subcameraPreview)
    , m_boardSettings(new BoardSettings(*src.m_boardSettings))
    , m_formatTemplateFId(src.m_formatTemplateFId)
    , m_syncColorSettings(src.m_syncColorSettings)
    , m_nonlinearBpp(src.m_nonlinearBpp)
    , m_aspectMode(src.m_aspectMode)
    , m_aspectRatio(src.m_aspectRatio)
    , m_aspectColor(src.m_aspectColor) {
  std::map<std::string, TPropertyGroup *>::iterator ft,
      fEnd = m_formatProperties.end();
  for (ft = m_formatProperties.begin(); ft != fEnd; ++ft) {
    if (ft->second) ft->second = ft->second->clone();
  }
}

//-------------------------------------------------------------------

TOutputProperties::~TOutputProperties() {
  delete m_renderSettings;

  std::for_each(m_formatProperties.begin(), m_formatProperties.end(),
                ::deleteValue);
}

//-------------------------------------------------------------------

TOutputProperties &TOutputProperties::operator=(const TOutputProperties &src) {
  m_path        = src.m_path;
  m_from        = src.m_from;
  m_to          = src.m_to;
  m_frameRate   = src.m_frameRate;
  m_whichLevels = src.m_whichLevels;
  m_offset      = src.m_offset;
  m_step        = src.m_step;

  m_multimediaRendering = src.m_multimediaRendering;
  m_maxTileSizeIndex    = src.m_maxTileSizeIndex;
  m_threadIndex         = src.m_threadIndex;
  m_subcameraPreview    = src.m_subcameraPreview;

  delete m_renderSettings;
  m_renderSettings = new TRenderSettings(*src.m_renderSettings);

  std::for_each(m_formatProperties.begin(), m_formatProperties.end(),
                ::deleteValue);

  std::map<std::string, TPropertyGroup *>::const_iterator sft,
      sfEnd = src.m_formatProperties.end();
  for (sft = src.m_formatProperties.begin(); sft != sfEnd; ++sft)
    m_formatProperties[sft->first] = sft->second->clone();

  delete m_boardSettings;
  m_boardSettings = new BoardSettings(*src.m_boardSettings);

  m_formatTemplateFId = src.m_formatTemplateFId;

  m_aspectMode  = src.m_aspectMode;
  m_aspectRatio = src.m_aspectRatio;
  m_aspectColor = src.m_aspectColor;

  return *this;
}

//-------------------------------------------------------------------

void TOutputProperties::setAspectMode(int mode) {
  m_aspectMode =
      mode == AspectLetterbox || mode == AspectCrop ? mode : NoAspectExport;
}

//-------------------------------------------------------------------

void TOutputProperties::setAspectRatio(double ratio) {
  if (std::isfinite(ratio) && ratio > 0.0) m_aspectRatio = ratio;
}

//-------------------------------------------------------------------

TRasterP TOutputProperties::fitToAspectRatio(const TRasterP &ras,
                                             double linearGamma) const {
  if (m_aspectMode == NoAspectExport || !ras) return ras;
  const bool crop = m_aspectMode == AspectCrop;
  TRect keep =
      CameraGuide::aspectPixelRect(ras->getSize(), m_aspectRatio, crop);
  if (keep == ras->getBounds()) return ras;
  if (!crop) {
    fillOutside(ras, keep, m_aspectColor, linearGamma);
    return ras;
  }
  TRasterP cropped = ras->extract(keep)->clone();
  cropped->setLinear(ras->isLinear());
  return cropped;
}

//-------------------------------------------------------------------

TDimension TOutputProperties::getOutputSize(
    const TDimension &renderSize) const {
  if (m_aspectMode != AspectCrop) return renderSize;
  return CameraGuide::aspectPixelRect(renderSize, m_aspectRatio, true)
      .getSize();
}

//-------------------------------------------------------------------

TFilePath TOutputProperties::getPath() const { return m_path; }

//-------------------------------------------------------------------

void TOutputProperties::setPath(const TFilePath &fp) { m_path = fp; }

//-------------------------------------------------------------------

void TOutputProperties::setOffset(int off) { m_offset = off; }

//-----------------------------------------------------------------------------

bool TOutputProperties::getRange(int &r0, int &r1, int &step) const {
  step = m_step;
  if (m_from > m_to) {
    r0 = 0;
    r1 = -1;
    return false;
  } else {
    r0 = m_from;
    r1 = m_to;
    return true;
  }
}

//-----------------------------------------------------------------------------

void TOutputProperties::setRange(int r0, int r1, int step) {
  assert(0 <= r0 && r0 <= r1 || r0 == 0 && r1 == -1);
  m_from = r0;
  m_to   = r1;
  m_step = step;
}

//-----------------------------------------------------------------------------

void TOutputProperties::setFrameRate(double fps) { m_frameRate = fps; }

//-------------------------------------------------------------------

TPropertyGroup *TOutputProperties::getFileFormatProperties(std::string ext) {
  std::map<std::string, TPropertyGroup *>::const_iterator it;
  it = m_formatProperties.find(ext);
  if (it == m_formatProperties.end()) {
    TPropertyGroup *ret     = Tiio::makeWriterProperties(ext);
    m_formatProperties[ext] = ret;
    return ret;
  } else if (ext == "mov" || ext == "3gp") {
    return it->second;
  } else {
    // Try to merge settings instead of overriding them
    TPropertyGroup *ret = Tiio::makeWriterProperties(ext);
    ret->setProperties(it->second);
    m_formatProperties[ext] = ret;
    return ret;
  }
}

//-------------------------------------------------------------------

void TOutputProperties::getFileFormatPropertiesExtensions(
    std::vector<std::string> &v) const {
  v.reserve(m_formatProperties.size());
  std::map<std::string, TPropertyGroup *>::const_iterator it;
  for (it = m_formatProperties.begin(); it != m_formatProperties.end(); ++it)
    v.push_back(it->first);
}

//-------------------------------------------------------------------

void TOutputProperties::setRenderSettings(
    const TRenderSettings &renderSettings) {
  assert(renderSettings.m_bpp == 32 || renderSettings.m_bpp == 64 ||
         renderSettings.m_bpp == 128);
  assert(renderSettings.m_gamma > 0);
  assert(renderSettings.m_quality == TRenderSettings::StandardResampleQuality ||
         renderSettings.m_quality == TRenderSettings::ImprovedResampleQuality ||
         renderSettings.m_quality == TRenderSettings::HighResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Triangle_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Mitchell_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Cubic5_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Cubic75_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Cubic1_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Hann2_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Hann3_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Hamming2_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Hamming3_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Lanczos2_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Lanczos3_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Gauss_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::ClosestPixel_FilterResampleQuality ||
         renderSettings.m_quality ==
             TRenderSettings::Bilinear_FilterResampleQuality);

  *m_renderSettings = renderSettings;
}
