#pragma once

#ifndef T_COLOR_FUNCTIONS_INCLUDED
#define T_COLOR_FUNCTIONS_INCLUDED

#include "tpixel.h"

#include <memory>

#undef DVAPI
#undef DVVAR
#ifdef TCOLOR_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//-----------------------------------------------------------------------------

class DVAPI TColorFunction {
public:
  virtual TPixel32 operator()(
      const TPixel32 &color) const = 0;  // {return color;};

  struct Parameters {  // outX = tcrop(inX * m_mX + m_cX, 0, 1); 0<=inX<=1
    double m_mR, m_mG, m_mB, m_mM;
    double m_cR, m_cG, m_cB, m_cM;
    Parameters()
        : m_mR(1)
        , m_mG(1)
        , m_mB(1)
        , m_mM(1)
        , m_cR(0)
        , m_cG(0)
        , m_cB(0)
        , m_cM(0) {}
  };

  virtual TColorFunction *clone() const = 0;
  virtual bool getParameters(
      Parameters &p) const = 0;  //{ p = Parameters(); return true; }

  virtual ~TColorFunction() {}
};

//-----------------------------------------------------------------------------

class DVAPI TGenericColorFunction final : public TColorFunction {
  double m_m[4], m_c[4];

public:
  TGenericColorFunction(const double m[4], const double c[4]);

  TColorFunction *clone() const override {
    return new TGenericColorFunction(m_m, m_c);
  }

  TPixel32 operator()(const TPixel32 &color) const override;
  bool getParameters(Parameters &p) const override;
};

//-----------------------------------------------------------------------------

class DVAPI TColorFader final : public TColorFunction {
  TPixel32 m_color;
  double m_fade;

public:
  TColorFader() : m_color(), m_fade(0.5) {}
  TColorFader(const TPixel32 &color, double fade)
      : m_color(color), m_fade(fade) {}

  TColorFunction *clone() const override {
    return new TColorFader(m_color, m_fade);
  }

  TPixel32 operator()(const TPixel32 &color) const override;
  bool getParameters(Parameters &p) const override;
};

//-----------------------------------------------------------------------------

class DVAPI TOnionFader final : public TColorFunction {
  TPixel32 m_color;
  double m_fade;

public:
  TOnionFader() : m_color(), m_fade(0.5) {}
  TOnionFader(const TPixel32 &color, double fade)
      : m_color(color), m_fade(fade) {}

  TColorFunction *clone() const override {
    return new TOnionFader(m_color, m_fade);
  }

  TPixel32 operator()(const TPixel32 &color) const override;
  bool getParameters(Parameters &p) const override;
};

class DVAPI TTranspFader final : public TColorFunction {
  double m_transp;

public:
  TTranspFader() : m_transp(0.5) {}
  TTranspFader(double transp) : m_transp(transp) {}

  TColorFunction *clone() const override { return new TTranspFader(m_transp); }

  TPixel32 operator()(const TPixel32 &color) const override;
  bool getParameters(Parameters &p) const override;
};

//-----------------------------------------------------------------------------

class DVAPI TColumnColorFilterFunction final : public TColorFunction {
  TPixel32 m_colorScale;

public:
  TColumnColorFilterFunction() : m_colorScale() {}
  TColumnColorFilterFunction(const TPixel32 &color) : m_colorScale(color) {}

  TColorFunction *clone() const override {
    return new TColumnColorFilterFunction(m_colorScale);
  }

  TPixel32 operator()(const TPixel32 &color) const override;
  bool getParameters(Parameters &p) const override;
};

//-----------------------------------------------------------------------------

//! Multiplies the alpha of the colors by an opacity, after having applied an
//! optional \p inner color function (which is cloned). Used to draw a stroke
//! with its own opacity (TStroke::getOpacity()). Fully inline.
class TStrokeOpacityFunction final : public TColorFunction {
  std::unique_ptr<TColorFunction> m_inner;
  double m_opacity;

public:
  TStrokeOpacityFunction(const TColorFunction *inner, double opacity)
      : m_inner(inner ? inner->clone() : nullptr), m_opacity(opacity) {}

  TColorFunction *clone() const override {
    return new TStrokeOpacityFunction(m_inner.get(), m_opacity);
  }

  TPixel32 operator()(const TPixel32 &color) const override {
    TPixel32 c = m_inner ? (*m_inner)(color) : color;
    c.m        = (UCHAR)(c.m * m_opacity + 0.5);
    return c;
  }

  bool getParameters(Parameters &p) const override {
    if (m_inner) {
      if (!m_inner->getParameters(p)) return false;
    } else
      p = Parameters();
    p.m_mM *= m_opacity;
    p.m_cM *= m_opacity;
    return true;
  }
};

#endif
