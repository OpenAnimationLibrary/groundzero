#include "stdfx.h"
#include "globalcontrollablefx.h"
#include "tnotanimatableparam.h"
#include "tfxparam.h"
#include "toonz/colormatch.h"
#include <cmath>

namespace {
template <class P> double maskValue(const TRasterPT<P> &mask, int x, int y) {
  if (!mask) return 1;
  auto p = mask->pixels(y)[x];
  // Premultiplied RGB already contains alpha. Black/transparent excludes.
  return std::max(0.0, std::min(1.0,
      (.2126*p.r + .7152*p.g + .0722*p.b) / P::maxChannelValue));
}
template <class P>
bool applyMatch(TRasterPT<P> out, TRasterPT<P> mask, TRasterPT<P> ink,
                const ColorMatch::Profile &profile, const int *cancel) {
  const double maximum = P::maxChannelValue;
  out->lock();
  if (mask) mask->lock();
  if (ink) ink->lock();
  bool complete = true;
  for (int y=0;y<out->getLy();++y) {
    if (cancel && *cancel) { complete = false; break; }
    auto row = out->pixels(y);
    for (int x=0;x<out->getLx();++x) {
      auto &p = row[x];
      if (p.m <= 0 || !std::isfinite(double(p.m)) ||
          !std::isfinite(double(p.r)) || !std::isfinite(double(p.g)) ||
          !std::isfinite(double(p.b))) continue;
      double coverage = maskValue(mask,x,y) * (ink ? 1-maskValue(ink,x,y) : 1);
      if (coverage <= 0) continue;
      ColorMatch::Profile local = profile;
      local.strength *= coverage;
      auto c = ColorMatch::transform({{double(p.r)/p.m,double(p.g)/p.m,
                                      double(p.b)/p.m}}, local);
      const double rounding = maximum == 1 ? 0 : .5;
      p.r = static_cast<typename P::Channel>(std::max(0.0,std::min(1.0,c[0]))*p.m+rounding);
      p.g = static_cast<typename P::Channel>(std::max(0.0,std::min(1.0,c[1]))*p.m+rounding);
      p.b = static_cast<typename P::Channel>(std::max(0.0,std::min(1.0,c[2]))*p.m+rounding);
    }
  }
  if (ink) ink->unlock();
  if (mask) mask->unlock();
  out->unlock();
  return complete;
}
}

class ReferenceColorMatchFx final : public GlobalControllableFx {
  FX_PLUGIN_DECLARATION(ReferenceColorMatchFx)
  TRasterFxPort m_source, m_mask, m_ink;
  TStringParamP m_profile;
public:
  ReferenceColorMatchFx() : m_profile(L"") {
    addInputPort("Source",m_source);
    addInputPort("Mask",m_mask);
    addInputPort("ProtectInk",m_ink);
    bindParam(this,"profile",m_profile);
    enableComputeInFloat(true);
  }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool toBeComputedInLinearColorSpace(bool, bool) const override { return false; }
  bool doGetBBox(double frame, TRectD &box, const TRenderSettings &ri) override {
    if (!m_source.isConnected()) { box=TRectD(); return false; }
    return m_source->doGetBBox(frame,box,ri);
  }
  void doCompute(TTile &tile,double frame,const TRenderSettings &ri) override {
    if (!m_source.isConnected()) { tile.getRaster()->clear(); return; }
    m_source->compute(tile,frame,ri);
    const auto text = QString::fromStdWString(m_profile->getValue());
    if (text.isEmpty() || (ri.m_isCanceled && *ri.m_isCanceled)) return;
    ColorMatch::Profile profile;
    QString error;
    if (!ColorMatch::parse(text,profile,error))
      throw TException((QStringLiteral("Reference Color Match: ")+error).toStdWString());
    if (profile.strength == 0) return;
    auto raster = tile.getRaster();
    if (qint64(raster->getLx())*raster->getLy() > 16777216)
      throw TException("Reference Color Match: render tile exceeds the 16 megapixel working-memory limit.");
    TTile mask, ink;
    if (m_mask.isConnected())
      m_mask->allocateAndCompute(mask,tile.m_pos,raster->getSize(),raster,frame,ri);
    if (m_ink.isConnected())
      m_ink->allocateAndCompute(ink,tile.m_pos,raster->getSize(),raster,frame,ri);
    // Publish a complete tile only. Cancellation leaves the unmodified source.
    TRasterP output = raster->clone();
    bool complete = false;
    if (TRaster32P r = output)
      complete=applyMatch<TPixel32>(r,mask.getRaster(),ink.getRaster(),profile,ri.m_isCanceled);
    else if (TRaster64P r = output)
      complete=applyMatch<TPixel64>(r,mask.getRaster(),ink.getRaster(),profile,ri.m_isCanceled);
    else if (TRasterFP r = output)
      complete=applyMatch<TPixelF>(r,mask.getRaster(),ink.getRaster(),profile,ri.m_isCanceled);
    else throw TException("Reference Color Match: unsupported raster type.");
    if (complete) raster->copy(output);
  }
};
FX_PLUGIN_IDENTIFIER(ReferenceColorMatchFx,"referenceColorMatchFx")
