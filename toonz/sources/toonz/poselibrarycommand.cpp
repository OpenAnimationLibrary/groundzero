#include "poselibrarycommand.h"
#include "posespane.h"
#include "tapp.h"
#include "tundo.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"

#include <algorithm>
#include <limits>

namespace {
bool equalEntries(const std::vector<PoseLibrary::Entry> &a,
                  const std::vector<PoseLibrary::Entry> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].m_objectId != b[i].m_objectId ||
        a[i].m_channel != b[i].m_channel || a[i].m_value != b[i].m_value)
      return false;
  return true;
}
bool equal(const PoseLibrary &a, const PoseLibrary &b) {
  if (!equalEntries(a.getBase(), b.getBase()) ||
      a.getPoseCount() != b.getPoseCount()) return false;
  for (int i = 0; i < a.getPoseCount(); ++i)
    if (a.getPose(i).m_name != b.getPose(i).m_name ||
        !equalEntries(a.getPose(i).m_entries, b.getPose(i).m_entries))
      return false;
  return true;
}
size_t bytes(const PoseLibrary &library) {
  size_t result = sizeof(library);
  auto count = [&](const std::vector<PoseLibrary::Entry> &entries) {
    result += entries.size() * sizeof(PoseLibrary::Entry);
    for (const auto &entry : entries) result += entry.m_channel.size();
  };
  count(library.getBase());
  for (int i = 0; i < library.getPoseCount(); ++i) {
    result += sizeof(PoseLibrary::Pose) + library.getPose(i).m_name.size() * sizeof(QChar);
    count(library.getPose(i).m_entries);
  }
  return result;
}
class PoseLibraryUndo final : public TUndo {
  ToonzScene *m_scene;
  PoseLibrary m_before, m_after;
  QString m_label;
  void apply(const PoseLibrary &state) const {
    auto *handle = TApp::instance()->getCurrentScene();
    if (handle->getScene() != m_scene) return;
    *m_scene->getPoseLibrary() = state;
    handle->setDirtyFlag(true);
    PoseLibraryNotifier::instance()->notifyChanged();
  }
public:
  PoseLibraryUndo(ToonzScene *scene, const PoseLibrary &after, const QString &label)
      : m_scene(scene), m_before(*scene->getPoseLibrary()),
        m_after(after), m_label(label) {}
  void undo() const override { apply(m_before); }
  void redo() const override { apply(m_after); }
  int getSize() const override {
    return int(std::min<size_t>(std::numeric_limits<int>::max(),
        sizeof(*this) + bytes(m_before) + bytes(m_after)));
  }
  QString getHistoryString() override { return m_label; }
};
}

bool replacePoseLibrary(ToonzScene *scene, const PoseLibrary &library,
                        const QString &label) {
  if (!scene || scene != TApp::instance()->getCurrentScene()->getScene() ||
      equal(*scene->getPoseLibrary(), library)) return false;
  auto *undo = new PoseLibraryUndo(scene, library, label);
  TUndoManager::manager()->add(undo);
  undo->redo();
  return true;
}
