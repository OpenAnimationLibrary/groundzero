#include <QTimer>
#include <QThread>
#include <QCoreApplication>

#include "toonz/tscenehandle.h"

#include "toonz/toonzscene.h"
#include "toonz/tproject.h"

//=============================================================================
// TSceneHandle
//-----------------------------------------------------------------------------

TSceneHandle::TSceneHandle() : m_scene(0), m_dirtyFlag(false) {}

//-----------------------------------------------------------------------------

TSceneHandle::~TSceneHandle() {}

struct TSceneHandle::RetainedScene {
  ToonzScene *scene;
  bool current = true;
  bool retired = false;
  unsigned long long generation = 0;
  explicit RetainedScene(ToonzScene *value) : scene(value) {}
  ~RetainedScene() { if (retired && !current) delete scene; }
};

std::shared_ptr<TSceneHandle::RetainedScene>
TSceneHandle::retainedScene(ToonzScene *scene) {
  if (!scene) return {};
  Q_ASSERT(!QCoreApplication::instance() ||
           QThread::currentThread() == QCoreApplication::instance()->thread());
  // Weak entries do not own current scenes, nor extend retired scene lifetime.
  static std::map<ToonzScene *, std::weak_ptr<RetainedScene>> records;
  for (auto it = records.begin(); it != records.end();) {
    if (it->second.expired()) it = records.erase(it);
    else ++it;
  }
  auto state = records[scene].lock();
  if (!state) {
    state = std::make_shared<RetainedScene>(scene);
    records[scene] = state;
  }
  return state;
}

std::shared_ptr<void> TSceneHandle::retainScene() {
  return retainScene(m_scene);
}

std::shared_ptr<void> TSceneHandle::retainScene(ToonzScene *scene) {
  auto state = retainedScene(scene);
  if (!state) return {};
  // Keep the record's strong reference alive until GUI-thread destruction,
  // including the interval between a worker's final release and DeferredDelete.
  // A re-pin/reinstatement in that interval must find the same record.
  struct Lease final : QObject {
    std::shared_ptr<RetainedScene> state;
    explicit Lease(std::shared_ptr<RetainedScene> value) : state(std::move(value)) {}
  };
  return std::shared_ptr<void>(new Lease(std::move(state)), [](void *value) {
    auto *lease = static_cast<Lease *>(value);
    if (QThread::currentThread() == lease->thread()) delete lease;
    else lease->deleteLater();
  });
}

//-----------------------------------------------------------------------------

ToonzScene *TSceneHandle::getScene() const { return m_scene; }

//-----------------------------------------------------------------------------

void TSceneHandle::setScene(ToonzScene *scene) {
  if (m_scene == scene) return;
  auto old = retainedScene(m_scene);
  auto next = retainedScene(scene);
  if (next) {
    next->current = true;
    next->retired = false;
    ++next->generation;
  }
  unsigned long long generation = 0;
  if (old) {
    old->current = false;
    generation = ++old->generation;
  }
  m_scene = scene;
  if (m_scene) emit sceneSwitched();

  // Prevent memory corruption caused by delayed signals writing into the
  // discarded old scene while that memory was freed.
  // That made OT had a chance of crashing when project or scene changed rapidly.
  // Note: This is not the best solution but "it just works"
  if (old) {
    QTimer *delayedTimer = new QTimer(this);
    delayedTimer->setObjectName("sceneRetirement");
    delayedTimer->setSingleShot(true);
    // The timer owns the retirement record, and readers own independent pins.
    // Reinstating a scene invalidates its earlier retirement generation.
    connect(delayedTimer, &QObject::destroyed, [old, generation]() {
      if (old->generation == generation && !old->current) old->retired = true;
    });
    connect(delayedTimer, &QTimer::timeout, [delayedTimer]() {
      delayedTimer->deleteLater();
    });
    delayedTimer->start(3000);  // 1 sec was enough, but... dunno about toasters
  }
}
