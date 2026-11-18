#include "automation_vector_opacity.h"
#include "automation_drawing.h"
#include "automation_drawing_analysis.h"
#include "automation_drawing_inspection.h"
#include "tools/artlayer.h"
#include "tregion.h"
#include "timage_io.h"
#include "tlevel.h"
#include "tlevel_io.h"
#include "tpalette.h"
#include "tstroke.h"
#include "toonz/toonzscene.h"
#include "toonz/txshsimplelevel.h"
#include <QDir>
#include <memory>
#include <cmath>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

QJsonObject target(Context& context, Document& document) {
  const auto created = executeScene(context, "level.create",
                                    {{"document_id", document.id},
                                     {"name", "Opacity"},
                                     {"format", "vector"}})
                           .toObject();
  auto* level =
      document.level(created.value("level_id").toString())->getSimpleLevel();
  TVectorImageP image = new TVectorImage();
  image->setPalette(level->getPalette());
  for (int index = 0; index < 2; ++index) {
    auto* stroke = new TStroke(
        std::vector<TThickPoint>{{-10, 0, 2}, {0, 0, 2}, {10, 0, 2}});
    stroke->setStyle(1);
    image->addStroke(stroke);
  }
  image->group(0, 2);
  level->setFrame(TFrameId(1), image);
  return {{"document_id", document.id},
          {"level_id", created.value("level_id")},
          {"drawing_id", QJsonObject{{"number", 1}, {"suffix", ""}}},
          {"expected_revision", document.revisionText()},
          {"operation_id", "opacity-test"},
          {"sharing", "all_exposures"},
          {"stroke_indices", QJsonArray{1, 0}}};
}

void rejects(Context& context, Document& document, const QString& method,
             const QJsonObject& params, const QString& code) {
  auto* level =
      document.level(params.value("level_id").toString())->getSimpleLevel();
  const auto digest = vectorLevelOpacityDigest(*level);
  context.resetMutationStarted();
  try {
    executeDrawing(context, method, params);
  } catch (const Error& error) {
    require(error.code == code,
            qPrintable("Wrong opacity error: " + error.code));
    require(!context.mutationStarted(),
            "Rejected opacity edit crossed publication");
    require(vectorLevelOpacityDigest(*level) == digest,
            "Rejected opacity edit changed artwork");
    return;
  }
  throw std::runtime_error("Invalid opacity operation succeeded");
}

void selection(Context& context, Document& document, QJsonObject params) {
  params.insert("coordinate_space", "drawing_native");
  params.insert("query", QJsonObject{{"mode", "nearest_centerline"},
                                     {"x", 0},
                                     {"y", 1},
                                     {"max_distance", 1}});
  context.resetMutationStarted();
  auto result = executeDrawing(context, "vector.select", params).toObject();
  require(result.value("stroke_indices").toArray() == QJsonArray{0},
          "Nearest tie did not select lowest index");
  require(result.value("nearest").toObject().value("distance") == 1,
          "Nearest distance changed");
  require(!context.mutationStarted(), "Selection published an edit");
  auto query = params.value("query").toObject();
  query.insert("max_distance", 0);
  params.insert("query", query);
  result = executeDrawing(context, "vector.select", params).toObject();
  require(result.value("nearest").isNull() &&
              result.value("stroke_indices").toArray().isEmpty(),
          "No-hit selection is not empty");
  params.insert("query", QJsonObject{{"mode", "centerline_bounds"},
                                     {"relation", "intersects"},
                                     {"x0", 0},
                                     {"x1", 0},
                                     {"y0", 0},
                                     {"y1", 0}});
  require(executeDrawing(context, "vector.select", params)
                  .toObject()
                  .value("stroke_indices")
                  .toArray()
                  .size() == 2,
          "Degenerate bounds lost boundary contact");
  params.insert("expected_revision", "999");
  rejects(context, document, "vector.select", params, "revision_conflict");
}

QJsonArray profile() {
  return {QJsonObject{{"position", 0}, {"opacity", .2}},
          QJsonObject{{"position", .12345678901234567}, {"opacity", .7}},
          QJsonObject{{"position", .5}, {"opacity", .9}},
          QJsonObject{{"position", .5}, {"opacity", .1}},
          QJsonObject{{"position", 1}, {"opacity", .8}}};
}

void edits(Context& context, Document& document, QJsonObject params) {
  auto* level =
      document.level(params.value("level_id").toString())->getSimpleLevel();
  const TVectorImageP original = level->getFrame(TFrameId(1), false);
  const auto originalDigest    = vectorLevelOpacityDigest(*level);
  const auto originalBytes =
      automationImageBytes(TImageP(original.getPointer()));
  params.insert("opacity_per_mille", 327);
  context.resetMutationStarted();
  auto result =
      executeDrawing(context, "vector.opacity.set", params).toObject();
  require(result.value("changed").toBool() && context.mutationStarted(),
          "Scalar edit was not published");
  require(result.value("stroke_indices").toArray() == QJsonArray({0, 1}),
          "Indices are not normalized");
  require(original->getStroke(0)->getOpacity() == 1,
          "Scalar edit mutated source image");
  params.insert("knots", profile());
  executeDrawing(context, "vector.opacity.profile.set", params);
  TVectorImageP edited = level->getFrame(TFrameId(1), false);
  require(edited->getStroke(0)->getOpacity() == .327, "Profile changed scalar");
  require(edited->getStroke(0)->getId() == original->getStroke(0)->getId(),
          "Opacity changed stroke identity");
  require(edited->getGroupDepth(0) == original->getGroupDepth(0),
          "Opacity changed group depth");
  require(automationImageBytes(edited) - originalBytes == 10 * sizeof(TPointD),
          "Profile storage omitted from asset budget");
  require(vectorLevelOpacityDigest(*level) != originalDigest,
          "Digest missed opacity change");
  context.resetMutationStarted();
  result =
      executeDrawing(context, "vector.opacity.profile.set", params).toObject();
  require(!result.value("changed").toBool() && !context.mutationStarted(),
          "Exact no-op published an edit");
  params.insert("stroke_index", 0);
  params.insert("offset", 1);
  params.insert("limit", 2);
  result = executeDrawing(context, "vector.opacity.inspect", params).toObject();
  require(result.value("next_offset") == 3 &&
              result.value("knots").toArray().size() == 2,
          "Opacity pagination failed");
  params.insert("offset", 5);
  require(executeDrawing(context, "vector.opacity.inspect", params)
              .toObject()
              .value("next_offset")
              .isNull(),
          "End page did not terminate");
  params.insert("offset", 6);
  rejects(context, document, "vector.opacity.inspect", params,
          "invalid_params");
}

void rejectionCases(Context& context, Document& document, QJsonObject params) {
  params.insert("opacity_per_mille", 1001);
  rejects(context, document, "vector.opacity.set", params, "invalid_params");
  params.insert("opacity_per_mille", 100);
  params.insert("stroke_indices", QJsonArray{0, 0});
  rejects(context, document, "vector.opacity.set", params, "invalid_params");
  params.insert("stroke_indices", QJsonArray{2});
  rejects(context, document, "vector.opacity.set", params, "invalid_params");
  params.insert("stroke_indices", QJsonArray{0});
  for (const auto& knots :
       {QJsonArray{QJsonObject{{"position", 0}, {"opacity", 1}}},
        QJsonArray{QJsonObject{{"position", .1}, {"opacity", 1}},
                   QJsonObject{{"position", 1}, {"opacity", 1}}}}) {
    params.insert("knots", knots);
    rejects(context, document, "vector.opacity.profile.set", params,
            "invalid_params");
  }
  auto* level =
      document.level(params.value("level_id").toString())->getSimpleLevel();
  level->getPalette()->setIsLocked(true);
  rejects(context, document, "vector.opacity.set", params, "locked");
  level->getPalette()->setIsLocked(false);
}

void limitsAndEmpty(Context& context, Document& document, QJsonObject params) {
  auto* level =
      document.level(params.value("level_id").toString())->getSimpleLevel();
  const TImageP original = level->getFrame(TFrameId(1), false);
  TVectorImageP empty    = new TVectorImage();
  empty->setPalette(level->getPalette());
  level->setFrame(TFrameId(1), empty);
  params.insert("stroke_index", 0);
  params.insert("offset", 0);
  params.insert("limit", 1);
  rejects(context, document, "vector.opacity.inspect", params,
          "invalid_params");
  params.insert("coordinate_space", "drawing_native");
  params.insert("query", QJsonObject{{"mode", "nearest_centerline"},
                                     {"x", 0},
                                     {"y", 0},
                                     {"max_distance", 0}});
  require(executeDrawing(context, "vector.select", params)
              .toObject()
              .value("nearest")
              .isNull(),
          "Empty drawing produced nearest match");
  level->setFrame(TFrameId(1), original);
  params.insert("stroke_indices", QJsonArray{0});
  QJsonArray knots;
  for (int index = 0; index < 4096; ++index)
    knots.append(
        QJsonObject{{"position", index / 4095.0}, {"opacity", index % 2}});
  params.insert("knots", knots);
  executeDrawing(context, "vector.opacity.profile.set", params);
  knots.insert(1, QJsonObject{{"position", 0}, {"opacity", 1}});
  params.insert("knots", knots);
  rejects(context, document, "vector.opacity.profile.set", params,
          "invalid_params");
  level->setFrame(TFrameId(1), original);
  params.insert("knots", QJsonArray());
  executeDrawing(context, "vector.opacity.profile.set", params);
}

void exactProfileAndScalar(Context& context, Document& document,
                           QJsonObject params) {
  params.insert("stroke_indices", QJsonArray{0});
  params.insert("knots", profile());
  executeDrawing(context, "vector.opacity.profile.set", params);
  auto knots = profile();
  auto knot  = knots[1].toObject();
  knot.insert("position",
              std::nextafter(knot.value("position").toDouble(), 1.0));
  knots[1] = knot;
  params.insert("knots", knots);
  context.resetMutationStarted();
  require(executeDrawing(context, "vector.opacity.profile.set", params)
              .toObject()
              .value("changed")
              .toBool(),
          "One-ULP profile change was lost to approximate equality");
  require(context.mutationStarted(),
          "One-ULP profile change was not published");
  auto* level =
      document.level(params.value("level_id").toString())->getSimpleLevel();
  TVectorImageP image = level->getFrame(TFrameId(1), false);
  image->getStroke(0)->setOpacity(.3274);
  try {
    nativeOpacityPerMille(*image->getStroke(0));
  } catch (const Error& error) {
    require(error.code == "unsupported_type",
            "Noncanonical scalar has wrong error");
    image->getStroke(0)->setOpacity(.327);
    return;
  }
  throw std::runtime_error("Noncanonical scalar was silently rounded");
}

void aggregateBudgetPurity(Context& context, Document& document,
                           QJsonObject params) {
  auto* level =
      document.level(params.value("level_id").toString())->getSimpleLevel();
  const TImageP saved = level->getFrame(TFrameId(1), false);
  TVectorImageP image = new TVectorImage();
  image->setPalette(level->getPalette());
  QJsonArray indices;
  for (int index = 0; index < 17; ++index) {
    auto* stroke = new TStroke(std::vector<TThickPoint>{
        {-1, double(index), 1}, {0, double(index), 1}, {1, double(index), 1}});
    stroke->setStyle(1);
    image->addStroke(stroke);
    indices.append(index);
  }
  image->group(0, 17);
  level->setFrame(TFrameId(1), image);
  QJsonArray knots;
  for (int index = 0; index < 4096; ++index)
    knots.append(QJsonObject{{"position", index / 4095.0}, {"opacity", .5}});
  params.insert("stroke_indices", indices);
  params.insert("knots", knots);
  rejects(context, document, "vector.opacity.profile.set", params,
          "resource_limit");
  require(image->getStroke(0)->getOpacityProfile().empty(),
          "Aggregate failure mutated retained source");
  require(image->getStroke(16)->getOpacity() == 1,
          "Aggregate failure changed source scalar");
  require(image->getGroupDepth(16) == 1,
          "Aggregate failure changed source group");
  indices.removeLast();
  params.insert("stroke_indices", indices);
  executeDrawing(context, "vector.opacity.profile.set", params);
  const TVectorImageP accepted = level->getFrame(TFrameId(1), false);
  require(accepted->getStroke(15)->getOpacityProfile().size() == 4096,
          "Exact aggregate quota rejected");
  require(accepted->getStroke(16)->getOpacityProfile().empty(),
          "Unselected stroke changed");
  level->setFrame(TFrameId(1), saved);
}

void roundTrip(Context& context, Document& document, QJsonObject params) {
  auto* level =
      document.level(params.value("level_id").toString())->getSimpleLevel();
  const auto before = vectorLevelOpacityDigest(*level);
  const TFilePath path(
      QDir(context.workspace()).filePath("native-opacity.pli").toStdWString());
  {
    TLevelWriterP writer(path);
    writer->setPalette(level->getPalette());
    writer->getFrameWriter(TFrameId(1))
        ->save(level->getFrame(TFrameId(1), false));
  }
  TLevelReaderP reader(path);
  const auto info  = reader->loadInfo();
  const auto image = reader->getFrameReader(TFrameId(1))->load();
  image->setPalette(info->getPalette());
  level->setFrame(TFrameId(1), image);
  require(vectorLevelOpacityDigest(*level) == before,
          "Native PLI changed opacity digest");
  params.insert("knots", QJsonArray());
  executeDrawing(context, "vector.opacity.profile.set", params);
  const TVectorImageP cleared = level->getFrame(TFrameId(1), false);
  require(cleared->getStroke(0)->getOpacityProfile().empty(),
          "Profile clear failed");
  require(cleared->getStroke(0)->getOpacity() == .327,
          "Profile clear changed scalar");
}
struct Segment final {
  double x0, x1, y, thickness;
  int style;
};

TVectorImageP drawingOf(const std::vector<Segment>& segments) {
  TVectorImageP image = new TVectorImage();
  for (const Segment& segment : segments) {
    const double middle = (segment.x0 + segment.x1) / 2;
    auto* stroke        = new TStroke(
        std::vector<TThickPoint>{{segment.x0, segment.y, segment.thickness},
                                 {middle, segment.y, segment.thickness},
                                 {segment.x1, segment.y, segment.thickness}});
    stroke->setStyle(segment.style);
    image->addStroke(stroke);
  }
  return image;
}

QJsonObject drawingTarget(Document& document, const QString& levelId,
                          const TVectorImageP& image, int number) {
  auto* level = document.level(levelId)->getSimpleLevel();
  image->setPalette(level->getPalette());
  level->setFrame(TFrameId(number), TImageP(image.getPointer()));
  return {{"document_id", document.id},
          {"level_id", levelId},
          {"drawing_id", QJsonObject{{"number", number}, {"suffix", ""}}},
          {"expected_revision", document.revisionText()},
          {"coordinate_space", "drawing_native"}};
}

void rolesMatchArtLayer(Context& context, Document& document,
                        const QString& level) {
  struct Fixture {
    const char* name;
    std::vector<Segment> strokes;
    std::vector<int> roles;
  };
  using ArtLayer::COLOUR_ART;
  using ArtLayer::LINE_ART;
  using ArtLayer::OVERLAY;
  using ArtLayer::UNDERLAY;
  const std::vector<Fixture> fixtures{
      {"no colour art",
       {{0, 10, 0, 2, 1}, {0, 10, 5, 2, 1}},
       {LINE_ART, LINE_ART}},
      {"colour block below",
       {{0, 10, 0, 0, 1}, {0, 10, 5, 0, 1}, {0, 10, 9, 2, 1}},
       {COLOUR_ART, COLOUR_ART, LINE_ART}},
      {"layered",
       {{0, 10, 0, 2, 1},
        {0, 10, 5, 0, 1},
        {0, 10, 9, 2, 1},
        {0, 10, 12, 0, 1},
        {0, 10, 15, 2, 1}},
       {UNDERLAY, COLOUR_ART, LINE_ART, COLOUR_ART, OVERLAY}}};
  int number = 20;
  for (const Fixture& fixture : fixtures) {
    const auto image = drawingOf(fixture.strokes);
    const auto roles = drawingRoles(*image);
    require(roles.byStroke == fixture.roles, fixture.name);
    int native[ArtLayer::RoleCount];
    ArtLayer::countRoles(*image, native);
    for (int role = 0; role < ArtLayer::RoleCount; ++role)
      require(roles.counts[role] == native[role], "Role count left ArtLayer");
    auto params = drawingTarget(document, level, image, number++);
    const auto page =
        executeDrawing(context, "vector.inspect", params).toObject();
    const auto strokes = page.value("strokes").toArray();
    for (int index = 0; index < strokes.size(); ++index)
      require(strokes[index].toObject().value("role").toString() ==
                  roleName(fixture.roles[index]),
              "vector.inspect role differs from the art-layer rule");
    const auto summary = page.value("summary").toObject();
    require(summary.value("regions") == "not_computed",
            "Regions were computed");
    require(summary.value("roles").toObject() == roleCountsJson(roles),
            "Summary role counts changed");
    require(image->getRegionCount() == 0, "Inspection built regions");
  }
}

void summaryNamesUndefinedStyles(Context& context, Document& document,
                                 const QString& level) {
  const auto image = drawingOf({{0, 10, 0, 2, 1}, {0, 10, 5, 2, 900}});
  auto params      = drawingTarget(document, level, image, 30);
  const auto page =
      executeDrawing(context, "vector.inspect", params).toObject();
  const auto usage =
      page.value("summary").toObject().value("style_usage").toArray();
  require(usage.size() == 2 && usage[0].toObject().value("defined").toBool() &&
              !usage[1].toObject().value("defined").toBool() &&
              usage[1].toObject().value("style_id").toInt() == 900,
          "An undefined style was reported as defined");
  require(strokeStyleUsage(*image).at(900) == 1, "Style usage miscounted");
  params.insert("expected_revision", "999");
  try {
    executeDrawing(context, "vector.inspect", params);
  } catch (const Error& error) {
    require(error.code == "stale_revision" &&
                error.details.value("revision") == document.revisionText(),
            "Stale inspection reported the wrong error");
    return;
  }
  throw std::runtime_error("A stale inspection succeeded");
}

TStroke* editorLasso(const std::vector<TPointD>& polygon) {
  std::vector<TThickPoint> points;
  for (size_t index = 0; index < polygon.size(); ++index) {
    const TPointD a = polygon[index];
    const TPointD b = polygon[(index + 1) % polygon.size()];
    points.push_back(TThickPoint(a.x, a.y, 0));
    points.push_back(TThickPoint((a.x + b.x) / 2, (a.y + b.y) / 2, 0));
  }
  points.push_back(TThickPoint(polygon[0].x, polygon[0].y, 0));
  auto* stroke = new TStroke(points);
  stroke->setSelfLoop(true);
  return stroke;
}

// VectorSelectionTool::selectRegionVectorImage on a detached lasso image.
std::vector<int> editorSelection(const TVectorImageP& image,
                                 const std::vector<TPointD>& polygon,
                                 bool includeIntersect) {
  std::unique_ptr<TStroke> lasso(editorLasso(polygon));
  TVectorImage selectImg;
  selectImg.addStroke(new TStroke(*lasso));
  selectImg.findRegions();
  std::vector<int> selected;
  for (UINT s = 0; s < image->getStrokeCount(); ++s) {
    TStroke* current = image->getStroke(s);
    bool hit         = false;
    for (UINT r = 0; r < selectImg.getRegionCount(); ++r)
      hit = selectImg.getRegion(r)->contains(*current, true) || hit;
    if (includeIntersect) {
      std::vector<DoublePair> crossings;
      intersect(lasso.get(), current, crossings, false);
      hit = !crossings.empty() || hit;
    }
    if (hit) selected.push_back(int(s));
  }
  return selected;
}

void lassoMatchesEditor(Context& context, Document& document,
                        const QString& level) {
  std::vector<Segment> segments;
  unsigned seed   = 12345;
  const auto next = [&seed](double span) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8 & 0xffff) / 65535.0 * span;
  };
  for (int index = 0; index < 60; ++index) {
    const double x = next(160) - 80, y = next(160) - 80;
    segments.push_back({x, x + 4 + next(40), y, next(6), 1});
  }
  const auto image  = drawingOf(segments);
  const auto params = drawingTarget(document, level, image, 40);
  const std::vector<std::vector<TPointD>> lassos{
      {{-50, -50}, {60, -40}, {50, 55}, {-45, 40}},
      {{-70, -70}, {70, -70}, {70, 70}, {0, 0}, {-70, 70}},
      {{0, -60}, {60, 0}, {0, 60}, {-60, 0}}};
  for (const auto& polygon : lassos) {
    for (const bool intersects : {false, true}) {
      auto request = params;
      QJsonArray points;
      for (const TPointD& point : polygon)
        points.append(QJsonObject{{"x", point.x}, {"y", point.y}});
      request.insert(
          "query",
          QJsonObject{{"mode", "lasso"},
                      {"relation", intersects ? "intersects" : "contained"},
                      {"points", points}});
      const auto got =
          executeDrawing(context, "vector.select", request).toObject();
      std::vector<int> found;
      for (const auto& index : got.value("stroke_indices").toArray())
        found.push_back(index.toInt());
      require(found == editorSelection(image, polygon, intersects),
              "Lasso selection differs from the editor routine");
      require(!found.empty() || !intersects,
              "The lasso fixture selected nothing");
    }
  }
  require(image->getRegionCount() == 0, "Lasso built regions on the document");
}

void lassoRefusals(Context& context, Document& document, const QString& level) {
  const auto image  = drawingOf({{0, 10, 0, 2, 1}});
  const auto params = drawingTarget(document, level, image, 41);
  const std::pair<std::vector<TPointD>, const char*> cases[]{
      {{{0, 0}, {100, 100}, {100, 0}, {0, 100}}, "self_intersecting"},
      {{{0, 0}, {50, 50}, {100, 100}}, "degenerate"},
      {{{0, 0}, {0, 0}, {10, 10}, {0, 10}}, "degenerate"},
      {{{0, 0}, {10, 0}, {10, 10}, {5, -5}}, "self_intersecting"}};
  for (const auto& [polygon, reason] : cases) {
    QJsonArray points;
    for (const TPointD& point : polygon)
      points.append(QJsonObject{{"x", point.x}, {"y", point.y}});
    auto request = params;
    request.insert("query", QJsonObject{{"mode", "lasso"},
                                        {"relation", "contained"},
                                        {"points", points}});
    try {
      executeDrawing(context, "vector.select", request);
    } catch (const Error& error) {
      require(error.code == "invalid_params" &&
                  error.details.value("reason").toString() == reason,
              reason);
      continue;
    }
    throw std::runtime_error(std::string("Lasso accepted: ") + reason);
  }
}

void handleLifetime(Context& context, Document& document,
                    const QString& level) {
  const auto image = drawingOf({{0, 10, 0, 2, 1}, {0, 10, 5, 2, 1}});
  auto params      = drawingTarget(document, level, image, 42);
  params.insert("query", QJsonObject{{"mode", "indices"},
                                     {"stroke_indices", QJsonArray{1}}});
  const auto first =
      executeDrawing(context, "vector.select", params).toObject();
  const QString id          = first.value("selection_id").toString();
  const QJsonObject drawing = params.value("drawing_id").toObject();
  const QString levelId     = params.value("level_id").toString();
  require(
      resolveSelection(document, id, levelId, drawing) == std::vector<int>{1},
      "A fresh handle did not resolve");
  const auto expectCode = [&](const QString& handle, const QString& code,
                              const QString& reason) {
    try {
      resolveSelection(document, handle, levelId, drawing);
    } catch (const Error& error) {
      require(error.code == code &&
                  error.details.value("reason").toString() == reason,
              qPrintable("Wrong handle error: " + error.code));
      return;
    }
    throw std::runtime_error("A refused handle resolved");
  };
  expectCode("selection_unknown", "unknown_selection", "");
  for (int index = 0; index < SelectionsPerDocument; ++index)
    executeDrawing(context, "vector.select", params);
  expectCode(id, "stale_selection", "evicted");
  const auto fresh =
      executeDrawing(context, "vector.select", params).toObject();
  const QString current = fresh.value("selection_id").toString();
  QJsonObject elsewhere = drawing;
  elsewhere.insert("number", 43);
  try {
    resolveSelection(document, current, levelId, elsewhere);
    throw std::runtime_error("A handle resolved for another drawing");
  } catch (const Error& error) {
    require(error.code == "stale_selection", "Wrong error for another drawing");
  }
  ++document.revision;
  expectCode(current, "stale_selection", "revision_changed");
}

void inspectionAndSelection(Context& context, Document& document) {
  const auto level = executeScene(context, "level.create",
                                  {{"document_id", document.id},
                                   {"name", "Inspection"},
                                   {"format", "vector"}})
                         .toObject()
                         .value("level_id")
                         .toString();
  rolesMatchArtLayer(context, document, level);
  summaryNamesUndefinedStyles(context, document, level);
  lassoMatchesEditor(context, document, level);
  lassoRefusals(context, document, level);
  handleLifetime(context, document, level);
}
}  // namespace

void runAutomationVectorOpacityTests(Context& context) {
  auto& document    = context.addDocument(std::make_unique<ToonzScene>());
  const auto params = target(context, document);
  selection(context, document, params);
  edits(context, document, params);
  rejectionCases(context, document, params);
  limitsAndEmpty(context, document, params);
  exactProfileAndScalar(context, document, params);
  aggregateBudgetPurity(context, document, params);
  roundTrip(context, document, params);
  context.removeDocument(document.id);
  auto& inspected = context.addDocument(std::make_unique<ToonzScene>());
  inspectionAndSelection(context, inspected);
  context.removeDocument(inspected.id);
  runAutomationDrawingAnalysisTests(context);
}
}  // namespace GroundzeroAutomation
