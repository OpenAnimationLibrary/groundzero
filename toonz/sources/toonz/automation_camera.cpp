#include "automation_scene_shared.h"

#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"

namespace GroundzeroAutomation {
namespace SceneShared {

QJsonObject describeCamera(Document& document) {
  TStageObjectTree* objects = tree(document);
  TCamera* camera           = objects->getCurrentCamera();
  TStageObject* object =
      objects->getStageObject(objects->getCurrentCameraId(), false);
  return {{"object_id", document.idForObject(object)},
          {"width_px", camera->getRes().lx},
          {"height_px", camera->getRes().ly},
          {"width_in", camera->getSize().lx},
          {"height_in", camera->getSize().ly}};
}

namespace {

QJsonValue updateCamera(Context& context, const QJsonObject& params) {
  updateSettings(context, params);
  return describeCamera(getDocument(context, params));
}

QJsonValue inspectCamera(Context& context, const QJsonObject& params) {
  return describeCamera(getDocument(context, params));
}

}  // namespace

void addCameraOperations(QJsonArray& catalog) {
  catalog.append(operation(
      "camera.inspect", "Inspect active camera resolution and size in inches.",
      documentSchema(), {"document_id"}));
  catalog.append(operation(
      "camera.update", "Set camera resolution and physical size in inches.",
      merge(documentSchema(), cameraSchema()), {"document_id"}, true));
}

void addCameraHandlers(SceneHandlers& handlers) {
  handlers.insert(
      {{"camera.inspect", inspectCamera}, {"camera.update", updateCamera}});
}

}  // namespace SceneShared
}  // namespace GroundzeroAutomation
