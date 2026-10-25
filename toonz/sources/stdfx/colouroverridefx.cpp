#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "ttzpimagefx.h"
#include "globalcontrollablefx.h"

#include <string>
#include <utility>

//==============================================================================
//    Colour Override Fx
//
// Changes the color of some styles of the Toonz Vector and Toonz Raster
// levels in the Source without editing their palettes - e.g. for a night-time
// version of a scene, or for one instance of a character in another outfit.
// Each pair maps a style index to a color; index 0 leaves the pair unused. The
// color replaces the style's main color, or - with the Multiply and Add modes
// - tints it. The change is made on a copy of the palette when the column is
// rendered, so it works on the style ids of the drawings. Other kinds of
// levels pass through unchanged.
//==============================================================================

class ColourOverrideFx final : public GlobalControllableFx,
                               public TColourOverrideSource {
  FX_PLUGIN_DECLARATION(ColourOverrideFx)

  enum { PAIR_COUNT = 8 };

  TRasterFxPort m_input;
  TIntParamP m_styleId[PAIR_COUNT];
  TIntEnumParamP m_mode[PAIR_COUNT];
  TPixelParamP m_color[PAIR_COUNT];

public:
  ColourOverrideFx() {
    addInputPort("Source", m_input);

    for (int i = 0; i < PAIR_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_styleId[i] = TIntParamP(0);
      m_styleId[i]->setValueRange(0, 4095);
      bindParam(this, "style" + suffix, m_styleId[i]);

      m_color[i] = TPixelParamP(TPixel32::Red);
      m_color[i]->enableMatte(true);
      bindParam(this, "color" + suffix, m_color[i]);

      // Replace is the default, so that the Fx of older scenes is unchanged
      m_mode[i] = TIntEnumParamP(
          new TIntEnumParam(ColourOverrideFxRenderData::eReplace, "Replace"));
      m_mode[i]->addItem(ColourOverrideFxRenderData::eMultiply, "Multiply");
      m_mode[i]->addItem(ColourOverrideFxRenderData::eAdd, "Add");
      bindParam(this, "mode" + suffix, m_mode[i]);
    }
  }

  // Used by the viewer, which shows the overrides of the Fx that follow a
  // column when the preference for it is on
  bool getColourOverrides(double frame,
                          ColourOverrideFxRenderData &data) const override;

  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override {
    if (m_input.isConnected()) {
      bool ret = m_input->doGetBBox(frame, bBox, info);
      return ret;
    } else {
      bBox = TRectD();
      return false;
    }
  }

  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &info) override;
  void doCompute(TTile &tile, double frame, const TRenderSettings &ri) override;

  bool canHandle(const TRenderSettings &info, double frame) override {
    return true;
  }

  bool allowUserCacheOnPort(int port) override { return false; }

private:
  // Adds the pairs in use at frame to the render data for the columns
  // upstream. Without any pair in use, the Source renders as it is.
  void addOverrides(TRenderSettings &ri, double frame);
};

//-------------------------------------------------------------------

bool ColourOverrideFx::getColourOverrides(
    double frame, ColourOverrideFxRenderData &data) const {
  for (int i = 0; i < PAIR_COUNT; ++i) {
    int styleId = m_styleId[i]->getValue();
    if (styleId <= 0) continue;

    data.m_overrides.push_back(ColourOverrideFxRenderData::Entry(
        styleId, m_color[i]->getValue(frame), m_mode[i]->getValue()));
  }

  return !data.m_overrides.empty();
}

//-------------------------------------------------------------------

void ColourOverrideFx::addOverrides(TRenderSettings &ri, double frame) {
  ColourOverrideFxRenderData *data = new ColourOverrideFxRenderData;
  TRasterFxRenderDataP dataP(data);

  if (!getColourOverrides(frame, *data)) return;

  ri.m_data.push_back(dataP);
  ri.m_userCachable = false;
}

//-------------------------------------------------------------------

void ColourOverrideFx::doDryCompute(TRectD &rect, double frame,
                                    const TRenderSettings &info) {
  if (!m_input.isConnected()) return;

  TRenderSettings ri2(info);
  addOverrides(ri2, frame);
  m_input->dryCompute(rect, frame, ri2);
}

//-------------------------------------------------------------------

void ColourOverrideFx::doCompute(TTile &tile, double frame,
                                 const TRenderSettings &ri) {
  if (!m_input.isConnected()) return;

  TRenderSettings ri2(ri);
  addOverrides(ri2, frame);
  m_input->compute(tile, frame, ri2);
}

FX_PLUGIN_IDENTIFIER(ColourOverrideFx, "colourOverrideFx");
