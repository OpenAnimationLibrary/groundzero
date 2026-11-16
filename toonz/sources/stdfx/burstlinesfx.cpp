#include "comiclinesfx.h"
#include "tparamuiconcept.h"

// Burst Lines: lines that run from an outer radius towards a centre, for
// focus lines, impact flashes and zooms. Nothing is drawn inside the inner
// radius. Twist curls the lines around the centre, and Falloff thins them
// towards it. The line engine is in comiclines.h, the shared parameters in
// comiclinesfx.h.
class BurstLinesFx final : public ComicLinesFx::Base {
  FX_PLUGIN_DECLARATION(BurstLinesFx)
  TPointParamP m_center;
  TDoubleParamP m_innerRadius, m_outerRadius, m_twist, m_falloff;

  void layout(ComicLines::Settings &s, double frame) const override {
    const TPointD center = m_center->getValue(frame);
    s.layout             = ComicLines::Layout::Radial;
    s.originX            = center.x;
    s.originY            = center.y;
    s.innerRadius =
        ComicLinesFx::bounded(m_innerRadius->getValue(frame), 0, 100000);
    s.outerRadius =
        ComicLinesFx::bounded(m_outerRadius->getValue(frame), 0, 100000);
    s.twist = ComicLinesFx::bounded(m_twist->getValue(frame), -3600, 3600) *
              ComicLines::pi / 180.0;
    s.falloff = ComicLinesFx::bounded(m_falloff->getValue(frame), 0, 8);
  }
  const char *aliasName() const override { return "burstLines"; }

public:
  BurstLinesFx()
      : ComicLinesFx::Base({120, 600, 0.6, 0, 12, 1, 0, 0})
      , m_center(TPointD(0.0, 0.0))
      , m_innerRadius(120.0)
      , m_outerRadius(700.0)
      , m_twist(0.0)
      , m_falloff(0.0) {
    m_center->getX()->setMeasureName("fxLength");
    m_center->getY()->setMeasureName("fxLength");
    for (TDoubleParamP *radius : {&m_innerRadius, &m_outerRadius}) {
      (*radius)->setMeasureName("fxLength");
      (*radius)->setValueRange(0, 100000);
    }
    m_twist->setMeasureName("angle");
    m_twist->setValueRange(-3600, 3600);
    m_twist->setUISliderRange(-180, 180);
    m_falloff->setValueRange(0, 8);
    m_falloff->setUISliderRange(0, 3);
    bindParam(this, "center", m_center);
    bindParam(this, "innerRadius", m_innerRadius);
    bindParam(this, "outerRadius", m_outerRadius);
    bindParam(this, "twist", m_twist);
    bindParam(this, "falloff", m_falloff);
    bindLineParams();
  }

  void getParamUIs(TParamUIConcept *&concepts, int &length) override {
    concepts = new TParamUIConcept[length = 3];
    concepts[0].m_type  = TParamUIConcept::POINT;
    concepts[0].m_label = "Center";
    concepts[0].m_params.push_back(m_center);
    concepts[1].m_type  = TParamUIConcept::RADIUS;
    concepts[1].m_label = "Inner Radius";
    concepts[1].m_params.push_back(m_innerRadius);
    concepts[1].m_params.push_back(m_center);
    concepts[2].m_type  = TParamUIConcept::RADIUS;
    concepts[2].m_label = "Outer Radius";
    concepts[2].m_params.push_back(m_outerRadius);
    concepts[2].m_params.push_back(m_center);
  }
};
FX_PLUGIN_IDENTIFIER(BurstLinesFx, "burstLinesFx")
