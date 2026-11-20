#pragma once

#include "automation.h"
#include "tgeometry.h"
#include "tvectorimage.h"

#include <map>
#include <vector>

class TPalette;
class TStroke;

namespace GroundzeroAutomation {

// The art roles of a drawing's strokes, derived with the templates of
// tools/artlayer.h. Nothing is stored or changed.
struct DrawingRoles final {
  // ArtLayer::Role of every stroke, in stacking order.
  std::vector<int> byStroke;
  // Strokes per role, indexed by ArtLayer::Role.
  int counts[4]   = {0, 0, 0, 0};
  int colourFirst = -1;
  int colourLast  = -1;
  bool layered    = false;
};

DrawingRoles drawingRoles(const TVectorImage& image);
// "line_art", "colour_art", "underlay" or "overlay".
QString roleName(int role);
// The role of a name from a roles filter, or -1.
int roleFromName(const QString& name);
QJsonObject roleCountsJson(const DrawingRoles& roles);
// The role, zero-thickness and opacity scalars of one inspected stroke.
void describeStroke(QJsonObject& stroke, const TStroke& native, int role);

// Number of strokes per style id. SceneChecker::usedStyleIds and the drawing
// analysis job count style usage of a vector drawing with this function.
std::map<int, int> strokeStyleUsage(const TVectorImage& image);

// The whole-drawing part of vector.inspect: style usage, role counts and the
// Colour Art range. Regions are never computed here.
QJsonObject inspectionSummary(const TVectorImage& image,
                              const TPalette* palette,
                              const DrawingRoles& roles);

// A lasso polygon from a vector.select query. Refuses an unusable polygon with
// invalid_params and details.reason self_intersecting or degenerate.
std::vector<TPointD> lassoPolygon(const QJsonObject& query);

// Strokes of `image` that `polygon` selects, in index order, as the editor's
// freehand selection does (VectorSelectionTool::selectRegionVectorImage) on a
// detached image. `allowed` holds one flag per stroke. Refuses more than
// `limit` strokes with resource_limit.
std::vector<int> lassoSelection(const TVectorImage& image,
                                const std::vector<TPointD>& polygon,
                                bool includeIntersecting,
                                const std::vector<bool>& allowed, int limit);

// Selection handles: bound to a document revision, level, drawing and stroke
// indices; at most SelectionsPerDocument stay resolvable.
constexpr int SelectionsPerDocument = 256;

struct SelectionBinding final {
  QString levelId;
  QJsonObject drawingId;
  std::vector<int> indices;
  // Reported with the handle: the rule that produced the indices.
  QJsonObject rule;
};

// Registers a handle for the document's current revision and returns
// {selection_id, rule, expires}.
QJsonObject issueSelection(Document& document, const SelectionBinding& binding);
// The indices of a handle bound to this document, level, drawing and its
// current revision; unknown_selection or stale_selection otherwise.
std::vector<int> resolveSelection(Document& document, const QString& id,
                                  const QString& levelId,
                                  const QJsonObject& drawingId);

}  // namespace GroundzeroAutomation
