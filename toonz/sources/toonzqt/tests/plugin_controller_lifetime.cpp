#include "tests/plugin_controller_lifetime.h"

#include "pluginhost.h"
#include "toonz/tapplication.h"
#include "toonz/tfxhandle.h"
#include "toonzqt/addfxcontextmenu.h"
#include "toonzqt/pluginloader.h"

#include <QAction>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QMenu>
#include <QPointer>

#include <memory>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void deliverEvents() {
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

struct ControllerRecord {
  QPointer<PluginLoadController> controller;
  bool workerDestroyed = false;
  bool threadFinished  = false;
};

class ControllerObserver;
ControllerObserver* activeObserver = nullptr;

class ControllerObserver final : public QObject {
  ControllerObserver* m_previous;
  std::vector<std::shared_ptr<ControllerRecord>> m_records;

public:
  ControllerObserver() : m_previous(activeObserver) { activeObserver = this; }
  ~ControllerObserver() override { activeObserver = m_previous; }

  void watch(PluginLoadController* controller, Loader* loader,
             QThread* thread) {
    auto record        = std::make_shared<ControllerRecord>();
    record->controller = controller;
    m_records.push_back(record);
    connect(loader, &QObject::destroyed, this,
            [record]() { record->workerDestroyed = true; });
    connect(thread, &QThread::finished, this,
            [record]() { record->threadFinished = true; });
  }

  size_t size() const { return m_records.size(); }

  bool retired(size_t first) const {
    for (size_t index = first; index < m_records.size(); ++index) {
      const auto& record = m_records[index];
      if (record->controller || !record->workerDestroyed ||
          !record->threadFinished)
        return false;
    }
    return true;
  }

  void waitForRetirement(size_t first) {
    QElapsedTimer timer;
    timer.start();
    while (!retired(first)) {
      require(timer.elapsed() < 3000,
              "Plugin controller, worker or thread did not retire");
      deliverEvents();
    }
  }
};

class MenuApplication final : public TApplication {
  std::unique_ptr<TFxHandle> m_fx = std::make_unique<TFxHandle>();

public:
  TFrameHandle* getCurrentFrame() const override { return nullptr; }
  TXshLevelHandle* getCurrentLevel() const override { return nullptr; }
  TXsheetHandle* getCurrentXsheet() const override { return nullptr; }
  TObjectHandle* getCurrentObject() const override { return nullptr; }
  TColumnHandle* getCurrentColumn() const override { return nullptr; }
  TSceneHandle* getCurrentScene() const override { return nullptr; }
  ToolHandle* getCurrentTool() const override { return nullptr; }
  TSelectionHandle* getCurrentSelection() const override { return nullptr; }
  TOnionSkinMaskHandle* getCurrentOnionSkin() const override { return nullptr; }
  TPaletteHandle* getCurrentPalette() const override { return nullptr; }
  TFxHandle* getCurrentFx() const override { return m_fx.get(); }
  PaletteController* getPaletteController() const override { return nullptr; }
  TColorStyle* getCurrentLevelStyle() const override { return nullptr; }
  int getCurrentLevelStyleIndex() const override { return 0; }
  void setCurrentLevelStyleIndex(int, bool) override {
    throw std::logic_error("FX menu unexpectedly changed the palette");
  }
};

void checkRegistry() {
  std::unique_ptr<TFx> host(
      PluginLoader::create_host("_plg_fixture_single_probe"));
  require(host && host->getPluginId() == "fixture_single_probe",
          "Closing an FX menu removed a registered plugin");
  std::unique_ptr<TFx> restored(TFx::create("fixture_single_probe"));
  require(restored && restored->getPluginId() == "fixture_single_probe",
          "Closing an FX menu invalidated its persistent declaration");
}

int probeEntries(QMenu* menu) {
  int count = 0;
  for (QAction* action : menu->findChildren<QAction*>()) {
    if (action->data().toString() == "_plg_fixture_single_probe") ++count;
  }
  return count;
}

std::vector<QPointer<QObject>> menuObjects(AddFxContextMenu& menu) {
  std::vector<QPointer<QObject>> objects;
  for (QMenu* root :
       {menu.getInsertMenu(), menu.getAddMenu(), menu.getReplaceMenu()}) {
    require(root->parent() == nullptr,
            "FX menu ownership changed unexpectedly");
    objects.push_back(root);
    for (QObject* child : root->findChildren<QObject*>())
      objects.push_back(child);
  }
  return objects;
}

std::vector<QPointer<QObject>> submenuObjects(AddFxContextMenu& menu) {
  std::vector<QPointer<QObject>> objects;
  for (QMenu* root :
       {menu.getInsertMenu(), menu.getAddMenu(), menu.getReplaceMenu()}) {
    for (QMenu* child :
         root->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)) {
      objects.push_back(child);
      for (QObject* object : child->findChildren<QObject*>())
        objects.push_back(object);
    }
  }
  return objects;
}

void checkRefresh(ControllerObserver& observer, AddFxContextMenu& menu,
                  TFxHandle& handle, bool hasPlugin) {
  QObject actionOwner;
  auto* sharedAction       = new QAction(&actionOwner);
  QPointer<QAction> shared = sharedAction;
  for (int refresh = 0; refresh < 2; ++refresh) {
    const auto previous = submenuObjects(menu);
    for (QMenu* root :
         {menu.getInsertMenu(), menu.getAddMenu(), menu.getReplaceMenu()}) {
      root->addAction(sharedAction);
    }
    const size_t first = observer.size();
    if (refresh == 0)
      handle.notifyFxPresetSaved();
    else
      handle.notifyFxPresetRemoved();
    require(observer.size() == first + 1,
            "Preset refresh did not rebuild its menus");
    for (const auto& object : previous)
      require(object.isNull(),
              "Preset refresh retained an obsolete submenu object");
    require(shared && shared->parent() == &actionOwner,
            "Preset refresh deleted an externally owned action");
    observer.waitForRetirement(first);
    for (QMenu* root :
         {menu.getInsertMenu(), menu.getAddMenu(), menu.getReplaceMenu()})
      require(probeEntries(root) == (hasPlugin ? 1 : 0),
              "Preset refresh lost a plugin entry");
  }
}

void checkRapidRefresh(ControllerObserver& observer, AddFxContextMenu& menu,
                       TFxHandle& handle, bool hasPlugin) {
  const size_t first = observer.size();
  handle.notifyFxPresetSaved();
  handle.notifyFxPresetRemoved();
  require(observer.size() == first + 2,
          "Rapid preset changes did not create two menu requests");
  std::vector<QPointer<QMenu>> unrelated;
  for (QMenu* root :
       {menu.getInsertMenu(), menu.getAddMenu(), menu.getReplaceMenu()}) {
    auto* group = new QMenu("Other FX", root);
    group->addAction("Keep this action");
    root->addMenu(group);
    unrelated.push_back(group);
  }
  observer.waitForRetirement(first);
  for (const auto& group : unrelated)
    require(group && group->actions().size() == 1,
            "Plugin publication removed an unrelated owned menu");
  for (QMenu* root :
       {menu.getInsertMenu(), menu.getAddMenu(), menu.getReplaceMenu()})
    require(probeEntries(root) == (hasPlugin ? 1 : 0),
            "Rapid preset refresh duplicated plugin entries");
}

void checkMenuCycle(ControllerObserver& observer, bool hasPlugin,
                    bool closeEarly) {
  MenuApplication application;
  std::unique_ptr<TFx> activeHost;
  if (hasPlugin)
    activeHost.reset(PluginLoader::create_host("_plg_fixture_single_probe"));
  const size_t first = observer.size();
  auto menu          = std::make_unique<AddFxContextMenu>();
  menu->setApplication(&application);
  application.getCurrentFx()->setPreviousActionString("A fixture_single_probe");
  QPointer<QAction> repeat = menu->getAgainCommand(AddFxContextMenu::Add);
  require(repeat && repeat->parent() == menu.get(),
          "Repeat FX action has no owner");
  require(observer.size() == first + 1,
          "FX menu did not create its loader controller");
  if (!closeEarly) {
    observer.waitForRetirement(first);
    for (QMenu* root :
         {menu->getInsertMenu(), menu->getAddMenu(), menu->getReplaceMenu()})
      require(probeEntries(root) == (hasPlugin ? 1 : 0),
              "FX menu publication did not precede controller retirement");
    checkRefresh(observer, *menu, *application.getCurrentFx(), hasPlugin);
    checkRapidRefresh(observer, *menu, *application.getCurrentFx(), hasPlugin);
    require(menu->getAgainCommand(AddFxContextMenu::Add) == repeat,
            "Preset refresh replaced a live repeat action");
  }
  const auto objects = menuObjects(*menu);
  menu.reset();
  require(repeat.isNull(), "Closing an FX menu retained its repeat action");
  for (const auto& object : objects)
    require(object.isNull(),
            "FX menu destruction retained an owned menu object");
  observer.waitForRetirement(first);
  if (hasPlugin) {
    checkRegistry();
    activeHost.reset();
    checkRegistry();
  }
}

void checkStartup(ControllerObserver& observer, const QString& directory,
                  bool hasPlugin) {
  QElapsedTimer timer;
  timer.start();
  while (!PluginLoader::load_entries(directory.toStdString())) {
    require(timer.elapsed() < 10000, "Plugin discovery did not finish");
    deliverEvents();
  }
  require(observer.size() == 1,
          "Startup did not create exactly one plugin controller");
  observer.waitForRetirement(0);
  if (hasPlugin) checkRegistry();
}

}  // namespace

void observePluginControllerForTest(PluginLoadController* controller,
                                    Loader* loader, QThread* thread) {
  if (activeObserver) activeObserver->watch(controller, loader, thread);
}

bool verifyPluginControllerLifetime(const QString& directory, bool hasPlugin,
                                    QString& failure) {
  try {
    ControllerObserver observer;
    checkStartup(observer, directory, hasPlugin);
    for (int cycle = 0; cycle < 4; ++cycle) {
      checkMenuCycle(observer, hasPlugin, false);
      checkMenuCycle(observer, hasPlugin, true);
    }
    return true;
  } catch (const std::exception& error) {
    failure = QString::fromUtf8(error.what());
    return false;
  }
}
