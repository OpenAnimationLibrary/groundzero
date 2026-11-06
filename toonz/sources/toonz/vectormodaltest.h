#pragma once

#include <QApplication>
#include <QEventLoop>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <iostream>
#include <stdexcept>
#include <string>

namespace VectorModalTest {

// QApplication::setActiveWindow only updates Qt's bookkeeping in response to
// a platform event; it does not request native focus. Wait for the shown test
// owner to receive real activation before a command captures activeWindow().
// https://doc.qt.io/archives/qt-5.15/qapplication.html#setActiveWindow
inline void activateOwner(QWidget &owner, const char *workflow) {
  owner.setObjectName(QStringLiteral("vectorModalTestOwner"));
  owner.resize(320, 200);
  owner.show();
  QPointer<QWidget> live(&owner);
  QEventLoop loop;
  QTimer poll, request, deadline;
  deadline.setSingleShot(true);
  bool ready = false;
  auto activate = [&] {
    if (live && !QApplication::activeModalWidget() &&
        !QApplication::activePopupWidget()) {
      live->raise();
      live->activateWindow();
    }
  };
  auto isReady = [&] {
    QWindow *window = live ? live->windowHandle() : nullptr;
    return live && live->isVisible() && window && window->isExposed() &&
           window->isActive() && QApplication::activeWindow() == live &&
           !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
  };
  auto inspect = [&] {
    if (isReady()) {
      ready = true;
      loop.quit();
    }
  };
  QObject::connect(&poll, &QTimer::timeout, &loop, inspect);
  QObject::connect(&request, &QTimer::timeout, &loop, activate);
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  activate();
  inspect();
  if (!ready) {
    poll.start(10); request.start(100); deadline.start(5000);
    loop.exec();
  }
  poll.stop(); request.stop(); deadline.stop();
  ready = ready && isReady();
  if (!ready) {
    auto kind = [](QObject *object) {
      return object ? object->metaObject()->className() : "none";
    };
    QWindow *window = live ? live->windowHandle() : nullptr;
    std::cerr << "vector-owner-native: " << workflow
              << " visible=" << bool(live && live->isVisible())
              << " exposed=" << bool(window && window->isExposed())
              << " native_active=" << bool(window && window->isActive())
              << " active=" << kind(QApplication::activeWindow())
              << " modal=" << kind(QApplication::activeModalWidget())
              << " popup=" << kind(QApplication::activePopupWidget()) << std::endl;
    throw std::runtime_error(std::string(workflow) +
                             " fixture could not activate its native owner");
  }
  std::cout << "vector-owner-native: " << workflow << " exposed and active" << std::endl;
}

inline bool hasReviewOwner(const QWidget *review, const QWidget *owner) {
  return review && owner && !review->parentWidget() && review->windowHandle() &&
         owner->windowHandle() &&
         review->windowHandle()->transientParent() == owner->windowHandle();
}

}  // namespace VectorModalTest
