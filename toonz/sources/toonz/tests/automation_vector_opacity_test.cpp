#include "automation_vector_opacity.h"
#include "automation_drawing.h"
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
}
}  // namespace GroundzeroAutomation
