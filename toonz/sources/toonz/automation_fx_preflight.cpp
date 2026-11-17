#include "automation.h"
#include "automation_fx.h"
#include "automation_fx_profile.h"

#include "tdoubleparam.h"
#include "tfx.h"
#include "tfxattributes.h"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "toonz/fxdag.h"
#include "toonz/scenefx.h"
#include "toonz/stageunits.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {

// The placement of effects that consume the composited Xsheet node.
constexpr int kXsheetPlacement = -1;

// The largest factor by which the affine scales any direction. A host that
// cannot hand an effect its affine renders it at TScale(spectralNorm), and an
// effect that can uses sqrt|det|, which never exceeds the spectral norm.
double spectralNorm(const TAffine& affine) {
  const double squares = affine.a11 * affine.a11 + affine.a12 * affine.a12 +
                         affine.a21 * affine.a21 + affine.a22 * affine.a22;
  const double det     = affine.det();
  return std::sqrt(
      (squares + std::sqrt(std::max(0.0, squares * squares - 4 * det * det))) /
      2);
}

QJsonValue finite(double value) {
  return std::isfinite(value) ? QJsonValue(value) : QJsonValue();
}

// The affines with which the scene builder hands effects their input: the
// camera DPI and placement times the placement of one upstream column.
class RenderGeometry final {
public:
  RenderGeometry(ToonzScene& scene, int width, int height)
      : m_sheet(scene.getXsheet()), m_width(width), m_height(height) {}

  double scale(TFx* fx, int frame) {
    double result = 0;
    for (const TAffine& affine : affines(fx, frame))
      result = std::max(result, spectralNorm(affine));
    return result;
  }

  double minimumDeterminant(TFx* fx, int frame) {
    double result = std::numeric_limits<double>::infinity();
    for (const TAffine& affine : affines(fx, frame))
      result = std::min(result, affine.det());
    return result;
  }

private:
  TXsheet* m_sheet;
  int m_width;
  int m_height;
  std::map<TFx*, std::set<int>> m_placements;

  // Upstream column indexes whose placement fx can inherit; an effect after
  // the Xsheet node inherits none, only the camera.
  const std::set<int>& placements(TFx* fx) {
    const auto found = m_placements.find(fx);
    if (found != m_placements.end()) return found->second;
    std::set<int> result;
    if (auto* column = dynamic_cast<TColumnFx*>(fx)) {
      if (column->getXshColumn() && column->getXshColumn()->isPreviewVisible())
        result.insert(column->getColumnIndex());
    } else if (dynamic_cast<TXsheetFx*>(fx)) {
      result.insert(kXsheetPlacement);
    } else {
      for (int index = 0; index < fx->getInputPortCount(); ++index)
        if (TFx* input = fx->getInputPort(index)->getFx()) {
          const std::set<int>& inherited = placements(input);
          result.insert(inherited.begin(), inherited.end());
        }
    }
    return m_placements[fx] = result;
  }

  TAffine camera(int frame) const {
    TStageObject* object = m_sheet->getStageObject(
        m_sheet->getStageObjectTree()->getCurrentCameraId());
    const TDimensionD size  = object->getCamera()->getSize();
    const TAffine dpi       = TScale(m_width / (Stage::inch * size.lx),
                                     m_height / (Stage::inch * size.ly));
    const TAffine placement = object->getPlacement(frame) *
                              TScale((1000 + object->getZ(frame)) / 1000);
    return dpi * placement.inv();
  }

  std::vector<TAffine> affines(TFx* fx, int frame) {
    const TAffine view = camera(frame);
    std::vector<TAffine> result;
    for (int placement : placements(fx)) {
      TAffine column;
      // A column behind the camera renders nothing through fx.
      if (placement != kXsheetPlacement &&
          !getColumnPlacement(column, m_sheet, frame, placement, false))
        continue;
      result.push_back(view * column);
    }
    return result;
  }
};

// Effects the current output renders, in first-visit order. A disabled
// effect renders only its preferred input, as the scene builder does.
void collectRendered(TFx* fx, FxDag* graph, std::set<TFx*>& seen,
                     std::vector<TFx*>& order) {
  if (!fx || !seen.insert(fx).second) return;
  if (dynamic_cast<TXsheetFx*>(fx)) {
    TFxSet* terminals = graph->getTerminalFxs();
    for (int index = 0; index < terminals->getFxCount(); ++index)
      collectRendered(terminals->getFx(index), graph, seen, order);
    return;
  }
  if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx)) {
    if (wrapper->getXshColumn()->isPreviewVisible() &&
        wrapper->getAttributes()->isEnabled() &&
        wrapper->getZeraryFx()->getAttributes()->isEnabled())
      order.push_back(fx);
    return;
  }
  if (dynamic_cast<TColumnFx*>(fx)) return;
  if (!fx->getAttributes()->isEnabled()) {
    if (fx->getInputPortCount() > 0)
      collectRendered(fx->getInputPort(fx->getPreferredInputPort())->getFx(),
                      graph, seen, order);
    return;
  }
  order.push_back(fx);
  for (int index = 0; index < fx->getInputPortCount(); ++index)
    collectRendered(fx->getInputPort(index)->getFx(), graph, seen, order);
}

std::vector<TFx*> renderedEffects(ToonzScene& scene) {
  FxDag* graph = scene.getXsheet()->getFxDag();
  TFx* output  = graph->getCurrentOutputFx();
  std::set<TFx*> seen;
  std::vector<TFx*> order;
  if (output && output->getInputPortCount() == 1)
    collectRendered(output->getInputPort(0)->getFx(), graph, seen, order);
  return order;
}

// The effects whose parameters a rendered node evaluates: a macro's members,
// a generator column's effect, or the node itself.
std::vector<TFx*> evaluatedEffects(TFx* node) {
  if (auto* macro = dynamic_cast<TMacroFx*>(node)) {
    std::vector<TFx*> members;
    for (const auto& member : macro->getFxs())
      members.push_back(member.getPointer());
    return members;
  }
  if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(node))
    return {wrapper->getZeraryFx()};
  return {node};
}

double extentValue(TFx* fx, const std::string& name, int frame) {
  TParam* param = fx->getParams()->getParam(name);
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return std::fabs(number->getValue(frame));
  if (auto* point = dynamic_cast<TPointParam*>(param)) {
    const TPointD value = point->getValue(frame);
    return std::max(std::fabs(value.x), std::fabs(value.y));
  }
  throw Error("internal_error",
              "An extent parameter is neither a number nor a point.");
}

struct Frames final {
  int start;
  int end;
  int step;
};

class Preflight final {
public:
  Preflight(Document& document, int width, int height, const Frames& frames)
      : m_document(document)
      , m_geometry(*document.scene, width, height)
      , m_frames(frames) {}

  QJsonArray run() {
    QJsonArray provenance;
    for (TFx* node : renderedEffects(*m_document.scene))
      for (TFx* effect : evaluatedEffects(node)) {
        const QJsonObject identity = identify(node, effect);
        check(node, effect, identity);
        QJsonObject entry = automationFxProvenance(effect);
        for (auto field = identity.begin(); field != identity.end(); ++field)
          entry.insert(field.key(), field.value());
        provenance.append(entry);
      }
    return provenance;
  }

private:
  Document& m_document;
  RenderGeometry m_geometry;
  Frames m_frames;

  QJsonObject identify(TFx* node, TFx* effect) {
    QJsonObject identity{{"fx_id", m_document.idForFx(node)}};
    if (dynamic_cast<TMacroFx*>(node))
      identity.insert("member_key", QString::fromStdWString(effect->getFxId()));
    return identity;
  }

  void check(TFx* node, TFx* effect, const QJsonObject& identity) {
    const std::string type = effect->getFxType();
    const auto extents     = automationFxExtents(type);
    const bool determinant = automationFxNeedsPositiveDeterminant(type);
    if (extents.empty() && !determinant) return;
    for (int frame = m_frames.start; frame <= m_frames.end;
         frame += m_frames.step) {
      const double scale = m_geometry.scale(node, frame);
      for (const AutomationFxExtent& extent : extents)
        requireExtent(effect, extent, frame, scale, identity);
      if (determinant) requirePositiveDeterminant(node, frame, identity);
    }
  }

  void requireExtent(TFx* effect, const AutomationFxExtent& extent, int frame,
                     double scale, QJsonObject details) {
    const double pixels = extent.coefficient *
                          extentValue(effect, extent.parameter, frame) * scale;
    if (pixels <= extent.maximumPixels) return;
    details.insert("reason", "extent");
    details.insert("parameter", QString::fromStdString(extent.parameter));
    details.insert("frame", frame);
    details.insert("extent_px", finite(pixels));
    details.insert("max_px", extent.maximumPixels);
    throw Error("fx_preflight_failed",
                "An effect would pad its input beyond the pixel extent limit; "
                "lower the parameter, the output size or the camera zoom.",
                details);
  }

  void requirePositiveDeterminant(TFx* node, int frame, QJsonObject details) {
    const double determinant = m_geometry.minimumDeterminant(node, frame);
    if (determinant > 0) return;
    details.insert("reason", "positive_determinant");
    details.insert("frame", frame);
    details.insert("determinant", finite(determinant));
    throw Error("fx_preflight_failed",
                "This effect requires an unmirrored, nonsingular placement.",
                details);
  }
};

}  // namespace

double automationFxRenderScale(ToonzScene& scene, TFx* fx, int width,
                               int height, int frame) {
  return RenderGeometry(scene, width, height).scale(fx, frame);
}

QJsonArray preflightAutomationRender(Document& document, int width, int height,
                                     int start, int end, int step) {
  return Preflight(document, width, height, {start, end, step}).run();
}

}  // namespace GroundzeroAutomation
