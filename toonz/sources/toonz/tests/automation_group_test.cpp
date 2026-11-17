#include "automation.h"

#include "tpalette.h"
#include "tstroke.h"
#include "tvectorimage.h"
#include "toonz/levelproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/txshsimplelevel.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QJsonObject makeDrawing(Context& context, Document& document) {
  const auto level = executeScene(context, "level.create",
                                  {{"document_id", document.id},
                                   {"name", "Native group fixture"},
                                   {"format", "vector"}})
                         .toObject();
  const QJsonObject target{
      {"document_id", document.id},
      {"level_id", level.value("level_id")},
      {"drawing_id", QJsonObject{{"number", 1}, {"suffix", ""}}}};
  auto create = target;
  create.insert("format", "vector");
  executeDrawing(context, "drawing.create", create);
  return target;
}

TVectorImageP nativeImage(Document& document, const QJsonObject& target) {
  auto* level =
      document.level(target.value("level_id").toString())->getSimpleLevel();
  return level->getFrame(TFrameId(1), false);
}

void addNativeStrokes(const TVectorImageP& image, int count) {
  for (int index = 0; index < count; ++index) {
    const double x = index * 10;
    auto* stroke   = new TStroke(
        std::vector<TThickPoint>{{x, 0, 1}, {x + 2, 3, 1}, {x + 4, 0, 1}});
    stroke->setStyle(1);
    image->addStroke(stroke);
  }
}

QJsonObject inspect(Context& context, const QJsonObject& target) {
  return executeDrawing(context, "vector.structure", target).toObject();
}

void rejects(Context& context, const QString& code,
             const std::function<void()>& operation) {
  context.resetMutationStarted();
  try {
    operation();
  } catch (const Error& error) {
    require(error.code == code,
            "Group rejection returned the wrong error code");
    require(!context.mutationStarted(),
            "Group rejection crossed the publication boundary");
    return;
  }
  throw std::runtime_error(
      "Invalid native group request unexpectedly succeeded");
}

void rejectEveryWrite(Context& context, const QJsonObject& target) {
  auto params = target;
  params.insert("sharing", "all_exposures");
  params.insert("first_index", 0);
  const auto before = inspect(context, target);
  rejects(context, "unsupported_grouping",
          [&] { executeDrawing(context, "vector.group.remove", params); });
  params.insert("count", 1);
  rejects(context, "unsupported_grouping",
          [&] { executeDrawing(context, "vector.group.create", params); });
  params.insert("before_index", 0);
  rejects(context, "unsupported_grouping",
          [&] { executeDrawing(context, "vector.order.move", params); });
  require(inspect(context, target) == before,
          "Rejected group writes changed native structure");
}

void testNestedInspection(Context& context) {
  auto& document    = context.addDocument(std::make_unique<ToonzScene>());
  const auto target = makeDrawing(context, document);
  const auto image  = nativeImage(document, target);
  addNativeStrokes(image, 5);
  image->group(2, 2);
  image->group(0, 5);
  const auto result = inspect(context, target);
  const QJsonArray expected{QJsonObject{{"index", 0},
                                        {"parent_index", QJsonValue::Null},
                                        {"first_index", 0},
                                        {"count", 5},
                                        {"depth", 1}},
                            QJsonObject{{"index", 1},
                                        {"parent_index", 0},
                                        {"first_index", 3},
                                        {"count", 2},
                                        {"depth", 2}}};
  require(result.value("groups").toArray() == expected,
          "Nested inspection must report exact native parent spans after "
          "native rearrangement");
  require(!result.value("write_admission").toBool(),
          "Nested drawing incorrectly admits writes");
  require(result.value("unsupported_reasons").toArray() ==
              QJsonArray{"nested_groups"},
          "Nested drawing must report the concrete unsupported reason");
  rejectEveryWrite(context, target);
  require(image->enterGroup(0), "Could not enter the native fixture group");
  const int entered        = image->isInsideGroup();
  const auto enteredResult = inspect(context, target);
  require(enteredResult.value("inside_group_depth").toInt() == entered,
          "Structure inspection lost the current native group context");
  require(enteredResult.value("groups").toArray() == expected,
          "Entered context must not change reported absolute group spans");
  rejectEveryWrite(context, target);
  require(image->isInsideGroup() == entered,
          "Rejected writes changed entered group context");
  context.removeDocument(document.id);
}

void testDepthLimit(Context& context) {
  auto& document    = context.addDocument(std::make_unique<ToonzScene>());
  const auto target = makeDrawing(context, document);
  const auto image  = nativeImage(document, target);
  addNativeStrokes(image, 1);
  for (int depth = 0; depth < 16; ++depth) image->group(0, 1);
  require(inspect(context, target).value("groups").toArray().size() == 16,
          "The admitted depth boundary must inspect all native groups");
  image->group(0, 1);
  rejects(context, "resource_limit", [&] { inspect(context, target); });
  require(image->getGroupDepth(0) == 17,
          "Depth rejection changed native grouping");
  context.removeDocument(document.id);
}

void testStructureCountBoundary(Context& context) {
  auto& document    = context.addDocument(std::make_unique<ToonzScene>());
  const auto target = makeDrawing(context, document);
  const auto image  = nativeImage(document, target);
  addNativeStrokes(image, 4096);
  require(inspect(context, target).value("stroke_count").toInt() == 4096,
          "Structure must admit exactly 4096 bounded native strokes");
  addNativeStrokes(image, 1);
  rejects(context, "resource_limit", [&] { inspect(context, target); });
  require(image->getStrokeCount() == 4097,
          "Count rejection changed native geometry");
  context.removeDocument(document.id);
}

void testNativeNoOp(Context& context) {
  auto& document   = context.addDocument(std::make_unique<ToonzScene>());
  auto target      = makeDrawing(context, document);
  const auto image = nativeImage(document, target);
  addNativeStrokes(image, 2);
  image->group(0, 2);
  auto* level =
      document.level(target.value("level_id").toString())->getSimpleLevel();
  level->getProperties()->setDirtyFlag(false);
  const auto before = inspect(context, target);
  target.insert("sharing", "all_exposures");
  target.insert("first_index", 0);
  target.insert("count", 2);
  context.resetMutationStarted();
  const auto result =
      executeDrawing(context, "vector.group.create", target).toObject();
  require(!result.value("changed").toBool(),
          "An exact native group must be a no-op");
  require(!context.mutationStarted(),
          "A group no-op must not mark a native mutation");
  require(!level->getProperties()->getDirtyFlag(),
          "A group no-op dirtied the native level");
  require(nativeImage(document, target) == image,
          "A group no-op replaced the native frame");
  require(inspect(context, target) == before,
          "A group no-op changed native structure");
  context.removeDocument(document.id);
}

}  // namespace

void runAutomationGroupTests(Context& context) {
  testNestedInspection(context);
  testDepthLimit(context);
  testStructureCountBoundary(context);
  testNativeNoOp(context);
}

}  // namespace GroundzeroAutomation
