#include "workflowlifetimeintegrationtest.h"

#include "mainwindow.h"
#include "menubar.h"
#include "toonz/preferences.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void waitForWorkflow(MainWindow& window, const QString& profile) {
  QElapsedTimer deadline;
  deadline.start();
  while (window.currentRoomSet() != profile && deadline.elapsed() < 10000)
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
  require(window.currentRoomSet() == profile,
          "pending workflow did not activate before the fixture deadline");
}

void switchTo(MainWindow& window, const QString& profile) {
  window.requestRoomSetChange(profile);
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
  waitForWorkflow(window, profile);
}

class SavedWorkflow final {
  MainWindow& m_window;
  const QString m_profile;
  const QString m_roomName;
  const bool m_lazy;

public:
  explicit SavedWorkflow(MainWindow& window)
      : m_window(window)
      , m_profile(window.currentRoomSet())
      , m_roomName(window.getCurrentRoom()->getName())
      , m_lazy(Preferences::instance()->isLazyLoadRoomsEnabled()) {}

  void restore() const {
    Preferences::instance()->setValue(lazyLoadRooms, m_lazy);
    switchTo(m_window, m_profile);
    if (!m_lazy)
      for (int i = 0; i < m_window.getRoomCount(); ++i)
        require(!m_window.getRoom(i)->notInitialized(),
                "workflow fixture left an eager profile with unloaded rooms");
    auto* tabs = m_window.findChild<RoomTabWidget*>();
    require(tabs,
            "workflow fixture lost the room tab widget during restoration");
    for (int i = 0; i < m_window.getRoomCount(); ++i) {
      if (m_window.getRoom(i)->getName() != m_roomName) continue;
      tabs->setCurrentIndex(i);
      require(m_window.getCurrentRoom() == m_window.getRoom(i),
              "workflow fixture did not restore the original selected room");
      return;
    }
    throw std::runtime_error("workflow fixture lost the original room name");
  }
};

int uninitializedRoom(MainWindow& window) {
  for (int i = 0; i < window.getRoomCount(); ++i)
    if (window.getRoom(i)->notInitialized()) return i;
  throw std::runtime_error("workflow fixture has no uninitialized lazy room");
}

void exercisePendingSwitch(MainWindow& window) {
  Preferences::instance()->setValue(lazyLoadRooms, true);
  const QString original  = window.currentRoomSet();
  const QString initial   = original == "Default" ? "Tradigital" : "Default";
  const QString requested = original == "Cutout" ? "Tradigital" : "Cutout";
  switchTo(window, initial);
  auto* tabs = window.findChild<RoomTabWidget*>();
  require(tabs, "workflow fixture has no native room tabs");
  const int lazyIndex = uninitializedRoom(window);
  QPointer<Room> room = window.getRoom(lazyIndex);
  QObject callbacks;
  QStringList switchedProfiles;
  bool initializing      = false;
  bool nestedCallbackRan = false;
  bool nestedRoomAlive   = false;
  QObject::connect(&window, &MainWindow::roomSetChanged, &callbacks,
                   [&](const QString& profile) {
                     if (profile != initial)
                       switchedProfiles.push_back(profile);
                   });
  window.requestRoomSetChange(original);
  window.requestRoomSetChange(requested);
  QTimer::singleShot(0, &callbacks, [&]() {
    nestedCallbackRan = initializing;
    nestedRoomAlive   = room && window.currentRoomSet() == initial;
  });
  initializing = true;
  tabs->setCurrentIndex(lazyIndex);
  initializing = false;
  require(
      nestedCallbackRan && nestedRoomAlive,
      "lazy initialization did not retain its room through queued callbacks");
  require(room && !room->notInitialized(),
          "a pending workflow retired the room before lazy initialization "
          "returned");
  require(window.getCurrentRoom() == room.data(),
          "lazy initialization did not activate its selected room");
  require(window.currentRoomSet() == initial && switchedProfiles.isEmpty(),
          "a pending workflow switched before lazy initialization returned");
  waitForWorkflow(window, requested);
  require(room.isNull() && switchedProfiles == QStringList{requested},
          "lazy initialization lost or duplicated the latest pending workflow "
          "request");
}

}  // namespace

void exerciseWorkflowRoomLifetime(QMainWindow* window) {
  auto* main = qobject_cast<MainWindow*>(window);
  require(
      main && main->getCurrentRoom(),
      "workflow lifetime fixture requires a native window with a current room");
  const SavedWorkflow saved(*main);
  try {
    exercisePendingSwitch(*main);
  } catch (...) {
    saved.restore();
    throw;
  }
  saved.restore();
  std::cerr << "native-ui-integration: checkpoint: lazy room pending workflow "
               "lifetime passed"
            << std::endl;
}
