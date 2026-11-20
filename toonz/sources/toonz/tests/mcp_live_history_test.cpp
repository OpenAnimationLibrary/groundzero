#include "tests/mcp_live_history_test.h"

#include <QJsonArray>
#include <QString>
#include <functional>
#include <stdexcept>

#include "automation_live.h"
#include "tapp.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tparamcontainer.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "tundo.h"

namespace {

using GroundzeroAutomation::Error;
namespace Live = GroundzeroAutomation::Live;

const QString instance = "00000000-0000-4000-8000-0000000000b1";

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QString operation(int value) {
  return "00000000-0000-4000-8000-" +
         QString::number(value, 16).rightJustified(12, '0');
}

TXsheet* sheet() {
  return TApp::instance()->getCurrentScene()->getScene()->getXsheet();
}

TDoubleParam* cameraX() {
  auto* tree = sheet()->getStageObjectTree();
  return tree->getStageObject(tree->getCurrentCameraId(), false)
      ->getParam(TStageObject::T_X);
}

TFx* namedFx(const std::wstring& name) {
  auto* effects = sheet()->getFxDag()->getInternalFxs();
  for (int index = 0; index < effects->getFxCount(); ++index)
    if (effects->getFx(index)->getName() == name) return effects->getFx(index);
  throw std::runtime_error("The live fixture Blur is missing");
}

struct History final {
  int count;
  int index;
  bool operator==(const History& other) const {
    return count == other.count && index == other.index;
  }
};

History history() {
  auto* manager = TUndoManager::manager();
  return {manager->getHistoryCount(), manager->getCurrentHistoryIndex()};
}

class Session final {
public:
  Session() : m_state(Live::createSceneState(instance)) {}

  QJsonObject inspect() {
    const QString epoch =
        Live::refreshScene(*m_state).value("scene_epoch").toString();
    return Live::inspectScene(*m_state, {{"scene_epoch", epoch}});
  }

  QJsonObject target(const QString& field, const QString& id) {
    for (const QJsonValue& item :
         inspect().value("editable_targets").toArray()) {
      const QJsonObject target = item.toObject();
      if (target.value(field) == id &&
          (field != "object_id" || target.value("channel") == "x"))
        return target;
    }
    throw std::runtime_error("The live fixture target is not editable");
  }

  QJsonObject camera() {
    for (const QJsonValue& item : inspect().value("stage_objects").toArray())
      if (item.toObject().value("kind") == "camera")
        return target("object_id",
                      item.toObject().value("object_id").toString());
    throw std::runtime_error("The live fixture camera is missing");
  }

  QJsonObject blur() {
    for (const QJsonValue& item : inspect().value("fx").toArray())
      if (item.toObject().value("name") == "Live default blur")
        return target("fx_id", item.toObject().value("fx_id").toString());
    throw std::runtime_error("The live fixture Blur is not inspected");
  }

  QJsonObject edit(const QJsonObject& target, const QString& fingerprint,
                   double value) {
    QJsonObject params{
        {"instance_id", instance},
        {"scene_epoch",
         Live::refreshScene(*m_state).value("scene_epoch").toString()},
        {"operation_id", operation(++m_operations)},
        {"expected_fingerprint", fingerprint},
        {"value", value},
        {"unit", target.value("unit")}};
    if (target.contains("object_id")) {
      params.insert("object_id", target.value("object_id"));
      params.insert("channel", "x");
      params.insert("frame", 10);
      return Live::editStage(*m_state, params);
    }
    params.insert("fx_id", target.value("fx_id"));
    params.insert("parameter", "value");
    params.insert("location", "default");
    return Live::editFx(*m_state, params);
  }

  // Requires a stale_target refusal that changes no value and no history.
  QJsonObject stale(const QJsonObject& target, const QString& fingerprint,
                    const std::function<double()>& observe) {
    const double before   = observe();
    const History entries = history();
    try {
      edit(target, fingerprint, before + 7);
    } catch (const Error& error) {
      require(error.code == "stale_target",
              "A stale live edit was refused with another code");
      require(observe() == before && history() == entries,
              "A refused stale live edit changed the scene or history");
      return error.details;
    }
    throw std::runtime_error("A stale live edit was applied");
  }

private:
  std::shared_ptr<Live::SceneState> m_state;
  int m_operations = 0;
};

void requireReason(const QJsonObject& details, const QString& reason,
                   const QString& current) {
  require(details.value("reason") == reason,
          "A stale live edit reported the wrong reason");
  require(details.keys() == QStringList({"current_fingerprint", "reason"}) &&
              details.value("current_fingerprint") == current,
          "A stale live edit did not report the current fingerprint");
}

double cameraValue() { return cameraX()->getValue(10); }

// An edit prepared before an undo or redo is refused; the reason says whether
// the content differs or only the history moved, and the current fingerprint
// lets the caller retry after re-checking the value.
void undoAndRedo() {
  Session session;
  const QJsonObject original = session.camera();
  const QString beforeEdit  = original.value("expected_fingerprint").toString();
  const QJsonObject applied = session.edit(original, beforeEdit, 3);
  const QString afterEdit   = applied.value("after_fingerprint").toString();
  require(applied.value("changed") == true && cameraValue() == 3,
          "The live history fixture edit was not applied");
  TUndoManager::manager()->undo();
  require(cameraValue() == 1, "Native undo did not restore the camera key");
  const QString undone =
      session.camera().value("expected_fingerprint").toString();
  require(undone != beforeEdit,
          "Undo restored a fingerprint the history had already passed");
  requireReason(session.stale(original, beforeEdit, cameraValue),
                "history_changed", undone);
  requireReason(session.stale(original, afterEdit, cameraValue),
                "content_changed", undone);
  TUndoManager::manager()->redo();
  require(cameraValue() == 3, "Native redo did not reapply the camera key");
  const QString redone =
      session.camera().value("expected_fingerprint").toString();
  require(redone != afterEdit,
          "Redo restored a fingerprint the history had already passed");
  requireReason(session.stale(original, afterEdit, cameraValue),
                "history_changed", redone);
  const QJsonObject refused = session.stale(original, undone, cameraValue);
  requireReason(refused, "content_changed", redone);
  const QJsonObject retried = session.edit(
      original, refused.value("current_fingerprint").toString(), 5);
  require(retried.value("changed") == true && cameraValue() == 5,
          "A retry with the reported current fingerprint was refused");
}

void unknownFingerprint() {
  Session session;
  const QJsonObject target = session.camera();
  const QString current    = target.value("expected_fingerprint").toString();
  requireReason(session.stale(target, QString(64, 'f'), cameraValue),
                "content_changed", current);
}

void replacedTarget() {
  Session session;
  TFx* effect          = namedFx(L"Live default blur");
  const auto blurValue = [effect]() {
    return dynamic_cast<TDoubleParam*>(effect->getParams()->getParam("value"))
        ->getDefaultValue();
  };
  const QJsonObject target = session.blur();
  const QString before     = target.value("expected_fingerprint").toString();
  effect->getParams()->unlink();
  const QString current =
      session.blur().value("expected_fingerprint").toString();
  requireReason(session.stale(target, before, blurValue), "target_replaced",
                current);
}

}  // namespace

void runNativeLiveHistoryTests() {
  undoAndRedo();
  unknownFingerprint();
  replacedTarget();
}
