#include "stdfx.h"
#include "tfxparam.h"
#include "tparamcontainer.h"
#include "tnotanimatableparam.h"
#include "ttzpimagefx.h"

#include <string>

//==============================================================================
//    Colour Script Fx
//
// A scene-wide colour shift for the colour script of a film: dusk, night or a
// flashback, keyed over the cuts. Placed between the Xsheet node and the
// Output (or after any columns), it shifts the hue, saturation and lightness
// of the styles of the Toonz Vector and Toonz Raster levels upstream, on a
// copy of their palettes when they are rendered, so the lines and fills stay
// clean; full-colour levels are shifted by their pixels. Styles whose name
// starts with the Keep Styles prefix, and columns set to Keep Colours, are
// left as they are. The math is in toonz/palettetransform.h.
//==============================================================================

class ColourScriptFx final : public TStandardRasterFx,
                             public TColourOverrideSource {
  FX_PLUGIN_DECLARATION(ColourScriptFx)

  TRasterFxPort m_input;
  TDoubleParamP m_hue, m_saturation, m_lightness;
  TStringParamP m_keepPrefix;

public:
  ColourScriptFx()
      : m_hue(0.0)
      , m_saturation(0.0)
      , m_lightness(0.0)
      , m_keepPrefix(new TStringParam(L"")) {
    addInputPort("Source", m_input);
    m_hue->setValueRange(-180.0, 180.0);
    m_hue->setMeasureName("angle");
    m_saturation->setValueRange(-100.0, 100.0);
    m_lightness->setValueRange(-100.0, 100.0);
    bindParam(this, "hue", m_hue);
    bindParam(this, "saturation", m_saturation);
    bindParam(this, "lightness", m_lightness);
    bindParam(this, "keepPrefix", m_keepPrefix);
    enableComputeInFloat(true);
  }

  PaletteTransform::Shift shift(double frame) const {
    PaletteTransform::Shift s;
    s.hue        = m_hue->getValue(frame);
    s.saturation = m_saturation->getValue(frame) / 100.0;
    s.lightness  = m_lightness->getValue(frame) / 100.0;
    s.clamp();
    return s;
  }

  // Used by the viewer, which shows the shift of a Colour Script after the
  // Xsheet node or after a column when the preference for it is on
  bool getColourOverrides(double frame,
                          ColourOverrideFxRenderData &data) const override {
    const PaletteTransform::Shift s = shift(frame);
    if (s.isIdentity()) return false;
    // An outer shift applies after an inner one
    if (data.m_shift.isIdentity()) {
      data.m_shift      = s;
      data.m_keepPrefix = m_keepPrefix->getValue();
    }
    return true;
  }

  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override {
    if (m_input.isConnected()) return m_input->doGetBBox(frame, bBox, info);
    bBox = TRectD();
    return false;
  }

  bool canHandle(const TRenderSettings &info, double frame) override {
    return true;
  }

  bool allowUserCacheOnPort(int port) override { return false; }

  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &info) override {
    if (!m_input.isConnected()) return;
    TRenderSettings ri(info);
    addShift(ri, frame);
    m_input->dryCompute(rect, frame, ri);
  }

  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &info) override {
    if (!m_input.isConnected()) return;
    TRenderSettings ri(info);
    addShift(ri, frame);
    m_input->compute(tile, frame, ri);
  }

private:
  // Adds the shift at frame to the render data for the columns upstream
  void addShift(TRenderSettings &ri, double frame) const {
    const PaletteTransform::Shift s = shift(frame);
    if (s.isIdentity()) return;
    ColourOverrideFxRenderData *data = new ColourOverrideFxRenderData;
    TRasterFxRenderDataP dataP(data);
    data->m_shift      = s;
    data->m_keepPrefix = m_keepPrefix->getValue();
    ri.m_data.push_back(dataP);
    ri.m_userCachable = false;
  }
};

FX_PLUGIN_IDENTIFIER(ColourScriptFx, "colourScriptFx");
