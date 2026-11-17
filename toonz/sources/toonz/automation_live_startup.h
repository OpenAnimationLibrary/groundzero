#pragma once

#include "automation_live.h"

#include <QApplication>

class QMainWindow;

namespace GroundzeroAutomation {
namespace Live {

class Application final : public QApplication {
public:
  using QApplication::QApplication;
  bool notify(QObject* receiver, QEvent* event) override;
};

// Compact argv without changing ordinary GUI arguments; disabled mode is empty.
// Throws Error for conflicting launch modes before creating any native
// resources.
Options parseLiveOptions(int& argc, char* argv[]);

class LiveUi final {
public:
  // The window and application must outlive this GUI-thread owner.
  LiveUi(QApplication& application, QMainWindow& window, Options options);
  ~LiveUi();
  LiveUi(const LiveUi&)            = delete;
  LiveUi& operator=(const LiveUi&) = delete;
  void start();
  void stop();
  bool running() const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace Live
}  // namespace GroundzeroAutomation
