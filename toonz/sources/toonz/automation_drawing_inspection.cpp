#include "automation_drawing_inspection.h"

#include "tools/artlayer.h"

#include "tpalette.h"
#include "tregion.h"
#include "tstroke.h"

#include <QUuid>

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <set>

namespace GroundzeroAutomation {

namespace {

constexpr double kLassoLimit  = 32768.0;
constexpr int kLassoMinimum   = 3;
constexpr int kLassoMaximum   = 256;
constexpr int kEvictedHandles = 1024;

[[noreturn]] void invalid(const QString& message) {
  throw Error("invalid_params", message);
}

[[noreturn]] void refuseLasso(const QString& reason, const QString& message) {
  throw Error("invalid_params", message, {{"reason", reason}});
}

double coordinate(const QJsonObject& point, const QString& key) {
  const QJsonValue value = point.value(key);
  const double result    = value.toDouble();
  if (!value.isDouble() || !std::isfinite(result) ||
      std::abs(result) > kLassoLimit)
    invalid("Lasso " + key + " must be a finite number within 32768");
  return result;
}

double cross(const TPointD& a, const TPointD& b, const TPointD& c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

int orientation(const TPointD& a, const TPointD& b, const TPointD& c) {
  const double value = cross(a, b, c);
  return value > 0 ? 1 : (value < 0 ? -1 : 0);
}

bool withinBox(const TPointD& a, const TPointD& b, const TPointD& p) {
  return p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) &&
         p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
}

// Whether two closed segments share a point, touching included.
bool segmentsMeet(const TPointD& a, const TPointD& b, const TPointD& c,
                  const TPointD& d) {
  const int abc = orientation(a, b, c), abd = orientation(a, b, d);
  const int cda = orientation(c, d, a), cdb = orientation(c, d, b);
  if (abc != abd && cda != cdb) return true;
  return (abc == 0 && withinBox(a, b, c)) || (abd == 0 && withinBox(a, b, d)) ||
         (cda == 0 && withinBox(c, d, a)) || (cdb == 0 && withinBox(c, d, b));
}

// Edges that share a vertex meet again only by running back over each other.
bool adjacentEdgesOverlap(const TPointD& shared, const TPointD& a,
                          const TPointD& b) {
  if (cross(shared, a, b) != 0) return false;
  return (a.x - shared.x) * (b.x - shared.x) +
             (a.y - shared.y) * (b.y - shared.y) >
         0;
}

bool selfIntersecting(const std::vector<TPointD>& polygon) {
  const int count = int(polygon.size());
  for (int i = 0; i < count; ++i) {
    const TPointD& a = polygon[i];
    const TPointD& b = polygon[(i + 1) % count];
    for (int j = i + 1; j < count; ++j) {
      const TPointD& c = polygon[j];
      const TPointD& d = polygon[(j + 1) % count];
      const bool next  = j == i + 1;
      const bool wraps = i == 0 && j == count - 1;
      if (next && adjacentEdgesOverlap(b, a, d)) return true;
      if (wraps && adjacentEdgesOverlap(a, b, c)) return true;
      if (!next && !wraps && segmentsMeet(a, b, c, d)) return true;
    }
  }
  return false;
}

bool allCollinear(const std::vector<TPointD>& polygon) {
  for (size_t i = 2; i < polygon.size(); ++i)
    if (cross(polygon[0], polygon[1], polygon[i]) != 0) return false;
  return true;
}

bool hasRepeatedNeighbour(const std::vector<TPointD>& polygon) {
  const int count = int(polygon.size());
  for (int i = 0; i < count; ++i)
    if (polygon[i] == polygon[(i + 1) % count]) return true;
  return false;
}

// The closed zero-thickness stroke through the polygon's vertices, with each
// edge's midpoint as the quadratic control point between them.
std::unique_ptr<TStroke> polygonStroke(const std::vector<TPointD>& polygon) {
  std::vector<TThickPoint> points;
  const int count = int(polygon.size());
  for (int i = 0; i < count; ++i) {
    const TPointD& a = polygon[i];
    const TPointD& b = polygon[(i + 1) % count];
    points.emplace_back(a.x, a.y, 0);
    points.emplace_back((a.x + b.x) / 2, (a.y + b.y) / 2, 0);
  }
  points.emplace_back(polygon.front().x, polygon.front().y, 0);
  auto stroke = std::make_unique<TStroke>(points);
  stroke->setSelfLoop(true);
  return stroke;
}

int opacityPerMille(const TStroke& stroke) {
  const double value = stroke.getOpacity();
  if (!std::isfinite(value)) return 1000;
  return std::clamp(int(std::lround(value * 1000)), 0, 1000);
}

}  // namespace

QString roleName(int role) {
  static const char* const names[] = {"line_art", "colour_art", "underlay",
                                      "overlay"};
  return QString::fromLatin1(names[role]);
}

int roleFromName(const QString& name) {
  for (int role = 0; role < ArtLayer::RoleCount; ++role)
    if (roleName(role) == name) return role;
  return -1;
}

DrawingRoles drawingRoles(const TVectorImage& image) {
  DrawingRoles result;
  const ArtLayer::ColourRange range = ArtLayer::colourRange(image);
  ArtLayer::countRoles(image, result.counts);
  const int count = int(image.getStrokeCount());
  result.byStroke.reserve(count);
  for (int index = 0; index < count; ++index)
    result.byStroke.push_back(ArtLayer::roleInStack(
        range, index, ArtLayer::hasZeroThickness(*image.getStroke(index))));
  result.colourFirst = range.first;
  result.colourLast  = range.last;
  result.layered     = range.isLayered();
  return result;
}

QJsonObject roleCountsJson(const DrawingRoles& roles) {
  QJsonObject counts;
  for (int role = 0; role < ArtLayer::RoleCount; ++role)
    counts.insert(roleName(role), roles.counts[role]);
  return counts;
}

void describeStroke(QJsonObject& stroke, const TStroke& native, int role) {
  stroke.insert("role", roleName(role));
  stroke.insert("zero_thickness", ArtLayer::hasZeroThickness(native));
  stroke.insert("opacity_per_mille", opacityPerMille(native));
  stroke.insert("opacity_knots", int(native.getOpacityProfile().size()));
}

std::map<int, int> strokeStyleUsage(const TVectorImage& image) {
  std::map<int, int> usage;
  for (UINT index = 0; index < image.getStrokeCount(); ++index)
    if (const TStroke* stroke = image.getStroke(index))
      ++usage[stroke->getStyle()];
  return usage;
}

QJsonObject inspectionSummary(const TVectorImage& image,
                              const TPalette* palette,
                              const DrawingRoles& roles) {
  QJsonArray styles;
  for (const auto& [style, strokes] : strokeStyleUsage(image))
    styles.append(QJsonObject{
        {"style_id", style},
        {"strokes", strokes},
        {"defined", palette && style >= 0 && style < palette->getStyleCount() &&
                        palette->getStyle(style) != nullptr}});
  const auto position = [](int index) {
    return index < 0 ? QJsonValue() : QJsonValue(index);
  };
  return {{"style_usage", styles},
          {"roles", roleCountsJson(roles)},
          {"colour_range", QJsonObject{{"first", position(roles.colourFirst)},
                                       {"last", position(roles.colourLast)},
                                       {"layered", roles.layered}}},
          {"regions", "not_computed"}};
}

std::vector<TPointD> lassoPolygon(const QJsonObject& query) {
  const QJsonValue value = query.value("points");
  if (!value.isArray()) invalid("Lasso points must be an array");
  const QJsonArray points = value.toArray();
  if (points.size() < kLassoMinimum || points.size() > kLassoMaximum)
    invalid("Lasso points must contain 3 to 256 points");
  std::vector<TPointD> polygon;
  for (const QJsonValue& entry : points) {
    if (!entry.isObject()) invalid("Each lasso point must be an object");
    const QJsonObject point = entry.toObject();
    for (auto it = point.begin(); it != point.end(); ++it)
      if (it.key() != "x" && it.key() != "y")
        invalid("Unknown lasso point field: " + it.key());
    polygon.emplace_back(coordinate(point, "x"), coordinate(point, "y"));
  }
  if (polygon.size() > 1 && polygon.front() == polygon.back())
    polygon.pop_back();
  if (polygon.size() < size_t(kLassoMinimum) || hasRepeatedNeighbour(polygon) ||
      allCollinear(polygon))
    refuseLasso("degenerate",
                "The lasso polygon needs three distinct points that enclose "
                "an area.");
  if (selfIntersecting(polygon))
    refuseLasso("self_intersecting",
                "The lasso polygon crosses or touches itself.");
  return polygon;
}

std::vector<int> lassoSelection(const TVectorImage& image,
                                const std::vector<TPointD>& polygon,
                                bool includeIntersecting,
                                const std::vector<bool>& allowed, int limit) {
  const auto lasso = polygonStroke(polygon);
  TVectorImage detached;
  detached.addStroke(new TStroke(*lasso));
  detached.findRegions();
  if (detached.getRegionCount() == 0)
    refuseLasso("degenerate", "The lasso polygon encloses no region.");
  std::vector<int> selected;
  for (UINT index = 0; index < image.getStrokeCount(); ++index) {
    if (!allowed[index]) continue;
    TStroke* stroke = image.getStroke(index);
    bool hit        = false;
    for (UINT region = 0; region < detached.getRegionCount() && !hit; ++region)
      hit = detached.getRegion(region)->contains(*stroke, true);
    if (!hit && includeIntersecting) {
      std::vector<DoublePair> crossings;
      intersect(lasso.get(), stroke, crossings, false);
      hit = !crossings.empty();
    }
    if (!hit) continue;
    selected.push_back(int(index));
    if (int(selected.size()) > limit)
      throw Error("resource_limit",
                  "Selection exceeds 128 strokes; narrow the query.");
  }
  return selected;
}

struct SelectionRecord final {
  QString id;
  quint64 revision = 0;
  SelectionBinding binding;
};

struct SelectionRegistry final {
  std::deque<SelectionRecord> live;
  std::deque<QString> evicted;
};

namespace {

SelectionRegistry& selectionsOf(Document& document) {
  if (!document.selections)
    document.selections = std::make_shared<SelectionRegistry>();
  return *document.selections;
}

[[noreturn]] void refuseStale(Document& document, const QString& id,
                              const QString& reason) {
  throw Error("stale_selection",
              "The selection no longer applies; select again at the current "
              "revision.",
              {{"reason", reason},
               {"revision", document.revisionText()},
               {"selection_id", id}});
}

}  // namespace

QJsonObject issueSelection(Document& document,
                           const SelectionBinding& binding) {
  SelectionRegistry& registry = selectionsOf(document);
  SelectionRecord record;
  record.id = "selection_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  record.revision = document.revision;
  record.binding  = binding;
  registry.live.push_back(record);
  while (int(registry.live.size()) > SelectionsPerDocument) {
    registry.evicted.push_back(registry.live.front().id);
    registry.live.pop_front();
    if (int(registry.evicted.size()) > kEvictedHandles)
      registry.evicted.pop_front();
  }
  return {{"selection_id", record.id},
          {"rule", binding.rule},
          {"expires",
           QJsonObject{{"on_revision_change", true},
                       {"retained_per_document", SelectionsPerDocument}}}};
}

std::vector<int> resolveSelection(Document& document, const QString& id,
                                  const QString& levelId,
                                  const QJsonObject& drawingId) {
  SelectionRegistry& registry = selectionsOf(document);
  for (const SelectionRecord& record : registry.live) {
    if (record.id != id) continue;
    if (record.revision != document.revision)
      refuseStale(document, id, "revision_changed");
    if (record.binding.levelId != levelId ||
        record.binding.drawingId != drawingId)
      refuseStale(document, id, "other_drawing");
    return record.binding.indices;
  }
  if (std::find(registry.evicted.begin(), registry.evicted.end(), id) !=
      registry.evicted.end())
    refuseStale(document, id, "evicted");
  throw Error("unknown_selection",
              "This worker issued no selection with this id for this "
              "document.",
              {{"selection_id", id}});
}

}  // namespace GroundzeroAutomation
