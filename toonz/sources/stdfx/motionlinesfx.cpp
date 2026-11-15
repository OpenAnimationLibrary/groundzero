#include "comiclinesfx.h"
#include "tparamuiconcept.h"

// Motion Lines: parallel lines inside a band that runs from Start to End,
// for speed lines, rain and hatching. Lines travel along the band at Speed
// and wrap inside it; Tile repeats the band over the whole frame. The line
// engine is in comiclines.h, the shared parameters in comiclinesfx.h.
class MotionLinesFx final : public ComicLinesFx::Base {
  FX_PLUGIN_DECLARATION(MotionLinesFx)
  TPointParamP m_start, m_end;
  TDoubleParamP m_width;
  TBoolParamP m_tile;

  void layout(ComicLines::Settings &s, double frame) const override {
    const TPointD start = m_start->getValue(frame);
    const TPointD end   = m_end->getValue(frame);
    s.layout            = ComicLines::Layout::Parallel;
    s.originX           = start.x;
    s.originY           = start.y;
    s.directionX        = end.x - start.x;
    s.directionY        = end.y - start.y;
    s.bandLength        = std::hypot(s.directionX, s.directionY);
    s.bandWidth  = ComicLinesFx::bounded(m_width->getValue(frame), 0, 100000);
    s.tile       = m_tile->getValue();
  }
  const char *aliasName() const override { return "motionLines"; }

public:
  MotionLinesFx()
      : ComicLinesFx::Base({60, 300, 0.5, 1, 3, 0, 0.25, 0.25})
      , m_start(TPointD(-200.0, 0.0))
      , m_end(TPointD(200.0, 0.0))
      , m_width(300.0)
      , m_tile(false) {
    for (TPointParamP *point : {&m_start, &m_end}) {
      (*point)->getX()->setMeasureName("fxLength");
      (*point)->getY()->setMeasureName("fxLength");
    }
    m_width->setMeasureName("fxLength");
    m_width->setValueRange(0, 100000);
    bindParam(this, "start", m_start);
    bindParam(this, "end", m_end);
    bindParam(this, "width", m_width);
    bindParam(this, "tile", m_tile);
    bindLineParams();
  }

  void getParamUIs(TParamUIConcept *&concepts, int &length) override {
    concepts = new TParamUIConcept[length = 1];
    concepts[0].m_type  = TParamUIConcept::LINEAR_RANGE;
    concepts[0].m_label = "";
    concepts[0].m_params.push_back(m_start);
    concepts[0].m_params.push_back(m_end);
  }
};
FX_PLUGIN_IDENTIFIER(MotionLinesFx, "motionLinesFx")
