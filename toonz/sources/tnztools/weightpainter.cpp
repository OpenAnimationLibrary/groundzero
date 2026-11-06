#include "weightpainter.h"

// TnzTools includes
#include "tools/tool.h"

// TnzLib includes
#include "toonz/tfxhandle.h"
#include "toonz/tscenehandle.h"

// TnzBase includes
#include "trasterfx.h"
#include "tparamcontainer.h"
#include "tnotanimatableparam.h"

// TnzCore includes
#include "tgl.h"
#include "tundo.h"
#include "historytypes.h"

#include <QObject>

#include <algorithm>
#include <cmath>

namespace {

// Cells on the longer side of a new painting
const int c_cells = 64;
// How far one dab moves the weights toward the target, at the brush center
const double c_strength = 0.35;

std::string paramName(int layer) {
  return std::string(1, char('a' + std::min(std::max(layer, 0), 2))) +
         "_paint";
}

TStringParam *paintParam(TFx *fx, const std::string &name) {
  if (!fx) return nullptr;
  return dynamic_cast<TStringParam *>(fx->getParams()->getParam(name));
}

std::wstring toWide(const std::string &s) {
  return std::wstring(s.begin(), s.end());
}

std::string toNarrow(const std::wstring &s) {
  return std::string(s.begin(), s.end());
}

void notifyFxChanged() {
  TTool::Application *app = TTool::getApplication();
  if (!app) return;
  app->getCurrentFx()->notifyFxChanged();
  app->getCurrentScene()->setDirtyFlag(true);
}

//=============================================================================
// WeightPaintUndo
//-----------------------------------------------------------------------------

class WeightPaintUndo final : public TUndo {
  TFxP m_fx;
  std::string m_paramName;
  std::wstring m_before, m_after;

  void apply(const std::wstring &value) const {
    if (TStringParam *param = paintParam(m_fx.getPointer(), m_paramName))
      param->setValue(value, true);
    notifyFxChanged();
  }

public:
  WeightPaintUndo(TFx *fx, const std::string &paramName,
                  const std::wstring &before, const std::wstring &after)
      : m_fx(fx), m_paramName(paramName), m_before(before), m_after(after) {}

  void undo() const override { apply(m_before); }
  void redo() const override { apply(m_after); }

  int getSize() const override {
    return int(sizeof(*this) +
               (m_before.size() + m_after.size()) * sizeof(wchar_t));
  }

  QString getHistoryString() override {
    return QObject::tr("Paint Deformer Weights");
  }
  int getHistoryType() override { return HistoryType::Fx; }
};

}  // namespace

//=============================================================================

bool WeightPainter::accepts(TFx *fx) {
  return fx && fx->getFxType() == "STD_deformerStackFx";
}

//-----------------------------------------------------------------------------

TDeformerWeightPainting WeightPainter::getPainting(TFx *fx, int layer) {
  TStringParam *param = accepts(fx) ? paintParam(fx, paramName(layer)) : 0;
  if (!param) return TDeformerWeightPainting();
  return TDeformerWeightPainting::fromString(toNarrow(param->getValue()));
}

//-----------------------------------------------------------------------------

bool WeightPainter::begin(TFx *fx, int layer, const TPointD &pos,
                          double radius, bool erase, double frame) {
  m_active = false;
  if (!accepts(fx) || !(radius > 0.0)) return false;
  TStringParam *param = paintParam(fx, paramName(layer));
  if (!param) return false;

  m_fx        = fx;
  m_paramName = paramName(layer);
  m_before    = param->getValue();
  m_painting  = TDeformerWeightPainting::fromString(toNarrow(m_before));
  m_radius    = radius;
  m_erase     = erase;

  if (m_painting.isEmpty()) {
    // Over the Source, with a margin, or a default area
    TRectD rect(-500.0, -500.0, 500.0, 500.0);
    TRasterFx *source =
        fx->getInputPortCount() > 0
            ? dynamic_cast<TRasterFx *>(fx->getInputPort(0)->getFx())
            : nullptr;
    TRectD box;
    if (source && source->getBBox(frame, box, TRenderSettings()) &&
        box != TConsts::infiniteRectD && box.getLx() > 0.0 &&
        box.getLy() > 0.0 && box.getLx() < 1e5 && box.getLy() < 1e5)
      rect = box.enlarge(0.25 * std::max(box.getLx(), box.getLy()));

    const double ratio = rect.getLy() / rect.getLx();
    const int lx = ratio <= 1.0 ? c_cells
                                : std::max(8, int(std::lround(c_cells / ratio)));
    const int ly = ratio <= 1.0 ? std::max(8, int(std::lround(c_cells * ratio)))
                                : c_cells;
    m_painting = TDeformerWeightPainting::make(rect, lx, ly);
    if (m_painting.isEmpty()) return false;
  }

  m_active  = true;
  m_lastPos = pos;
  dab(pos);
  return true;
}

//-----------------------------------------------------------------------------

void WeightPainter::drag(const TPointD &pos) {
  if (!m_active) return;
  // Dabs a quarter of the brush apart
  const double spacing = 0.25 * m_radius;
  const int steps =
      std::min(200, std::max(1, int(std::ceil(norm(pos - m_lastPos) / spacing))));
  for (int k = 1; k <= steps; ++k)
    dab(m_lastPos + (pos - m_lastPos) * (double(k) / steps));
  m_lastPos = pos;
}

//-----------------------------------------------------------------------------

void WeightPainter::end() {
  if (!m_active) return;
  m_active = false;

  const std::wstring after = toWide(m_painting.toString());
  if (after != m_before) {
    store();
    TUndoManager::manager()->add(
        new WeightPaintUndo(m_fx.getPointer(), m_paramName, m_before, after));
    notifyFxChanged();
  }
  m_fx = TFxP();
}

//-----------------------------------------------------------------------------

void WeightPainter::dab(const TPointD &pos) {
  m_painting.paint(pos, m_radius, m_erase ? 0.0 : 1.0, c_strength);
}

//-----------------------------------------------------------------------------

void WeightPainter::store() {
  if (TStringParam *param = paintParam(m_fx.getPointer(), m_paramName))
    param->setValue(toWide(m_painting.toString()));
}

//-----------------------------------------------------------------------------

void WeightPainter::draw(const TDeformerWeightPainting &painting,
                         const TPointD &cursor, double radius,
                         double pixelSize) {
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  if (!painting.isEmpty()) {
    // The weights in red, more opaque where higher
    const TRectD &rect = painting.m_rect;
    const double cw = rect.getLx() / painting.m_lx;
    const double ch = rect.getLy() / painting.m_ly;
    glBegin(GL_QUADS);
    for (int j = 0; j < painting.m_ly; ++j)
      for (int i = 0; i < painting.m_lx; ++i) {
        const int value = painting.m_values[j * painting.m_lx + i];
        if (value == 0) continue;
        glColor4d(1.0, 0.15, 0.1, 0.5 * value / 255.0);
        const double x0 = rect.x0 + i * cw, y0 = rect.y0 + j * ch;
        glVertex2d(x0, y0);
        glVertex2d(x0 + cw, y0);
        glVertex2d(x0 + cw, y0 + ch);
        glVertex2d(x0, y0 + ch);
      }
    glEnd();

    // The painted area
    glColor4d(1.0, 0.15, 0.1, 0.6);
    tglDrawRect(rect);
  }

  if (radius > 0.0) {
    glColor4d(1.0, 0.4, 0.2, 0.9);
    tglDrawCircle(cursor, radius);
    tglDrawCircle(cursor, std::max(0.0, radius - pixelSize));
  }
}
