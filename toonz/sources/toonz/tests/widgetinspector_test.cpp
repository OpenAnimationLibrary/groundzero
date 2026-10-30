#include "widgetinspector.h"

#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QTreeWidget>
#include <QWindow>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QPointingDevice>
#else
#include <QTouchDevice>
#endif

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void mouse(QWidget *widget, QEvent::Type type, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
  const QPoint local = widget->rect().center();
  QMouseEvent event(type, QPointF(local),
                    QPointF(widget->mapTo(widget->window(), local)),
                    QPointF(widget->mapToGlobal(local)), button, buttons,
                    Qt::NoModifier);
  QApplication::sendEvent(widget, &event);
}

void click(QWidget *widget, Qt::MouseButton button = Qt::LeftButton) {
  mouse(widget, QEvent::MouseButtonPress, button, button);
  mouse(widget, QEvent::MouseButtonRelease, button, Qt::NoButton);
}

void nativeMouse(QWidget *target, QEvent::Type type, Qt::MouseButton button,
                 Qt::MouseButtons buttons) {
  QWidget *window = target->window();
  const QPoint local = target->mapTo(window, target->rect().center());
  QMouseEvent event(type, QPointF(local), QPointF(local),
                    QPointF(window->mapToGlobal(local)), button, buttons,
                    Qt::NoModifier);
  QApplication::sendEvent(window->windowHandle(), &event);
}

QString value(QTreeWidget *tree, const QString &name) {
  for (int i = 0; i < tree->topLevelItemCount(); ++i) {
    QTreeWidgetItem *item = tree->topLevelItem(i);
    if (item->text(0) == name) return item->text(1);
  }
  return QString();
}

class EditingWidget final : public QWidget {
public:
  explicit EditingWidget(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_AcceptTouchEvents);
    setAttribute(Qt::WA_TabletTracking);
  }

  int edits = 0;

protected:
  bool event(QEvent *event) override {
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::KeyPress:
      ++edits;
      event->accept();
      return true;
    default:
      return QWidget::event(event);
    }
  }
};

void tablet(QWidget *widget, QEvent::Type type) {
  const QPointF local(widget->rect().center());
  const QPointF global(widget->mapToGlobal(local.toPoint()));
  const bool press = type == QEvent::TabletPress;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  static QPointingDevice device(
      QStringLiteral("test-stylus"), 1, QInputDevice::DeviceType::Stylus,
      QPointingDevice::PointerType::Pen,
      QInputDevice::Capability::Position | QInputDevice::Capability::Pressure,
      1, 1);
  QTabletEvent event(type, &device, local, global, press ? 1.0 : 0.0,
                     0, 0, 0, 0, 0, Qt::NoModifier, Qt::LeftButton,
                     press ? Qt::LeftButton : Qt::NoButton);
#else
  QTabletEvent event(type, local, global, QTabletEvent::Stylus, QTabletEvent::Pen,
                     press ? 1.0 : 0.0, 0, 0, 0, 0, 0, Qt::NoModifier, 1,
                     Qt::LeftButton, press ? Qt::LeftButton : Qt::NoButton);
#endif
  QApplication::sendEvent(widget, &event);
}

void touch(QWidget *widget, QEvent::Type type) {
  const QPointF local(widget->rect().center());
  const QPointF global(widget->mapToGlobal(local.toPoint()));
  const bool press = type == QEvent::TouchBegin;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  static QPointingDevice device(
      QStringLiteral("test-touch"), 2, QInputDevice::DeviceType::TouchScreen,
      QPointingDevice::PointerType::Finger,
      QInputDevice::Capability::Position, 1, 0);
  const QEventPoint point(0, press ? QEventPoint::State::Pressed
                                 : QEventPoint::State::Released,
                          local, global);
  QTouchEvent event(type, &device, Qt::NoModifier, {point});
#else
  static QTouchDevice device;
  device.setType(QTouchDevice::TouchScreen);
  device.setCapabilities(QTouchDevice::Position);
  device.setMaximumTouchPoints(1);
  const auto state = press ? Qt::TouchPointPressed : Qt::TouchPointReleased;
  QTouchEvent::TouchPoint point(0);
  point.setState(state);
  point.setPos(local);
  point.setScreenPos(global);
  QTouchEvent event(type, &device, Qt::NoModifier, state, {point});
#endif
  QApplication::sendEvent(widget, &event);
}
}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication app(argc, argv);
  try {
    QWidget host;
    host.resize(1000, 600);
    WidgetInspector inspector(&host);
    inspector.setGeometry(480, 20, 500, 430);
    QPushButton button(QStringLiteral("Synthetic editable text"), &host);
    button.setObjectName(QStringLiteral("sampleAction"));
    button.setGeometry(20, 20, 220, 40);
    button.setProperty("unrelatedData", "Synthetic private value");
    EditingWidget canvas(&host);
    canvas.setObjectName(QStringLiteral("sampleCanvas"));
    canvas.setGeometry(20, 100, 400, 350);
    int activations = 0;
    QObject::connect(&button, &QPushButton::clicked,
                     [&activations]() { ++activations; });
    host.show();
    app.processEvents();

    auto *tree = inspector.findChild<QTreeWidget *>("widgetInspectorProperties");
    auto *pick = inspector.findChild<QPushButton *>("widgetInspectorPick");
    auto *refresh = inspector.findChild<QPushButton *>("widgetInspectorRefresh");
    auto *clear = inspector.findChild<QPushButton *>("widgetInspectorClear");
    require(tree && pick && refresh && clear, "inspector controls are missing");
    require(!inspector.isPicking() && !refresh->isEnabled() &&
                tree->topLevelItemCount() == 0,
            "inspector did not start with an inactive empty snapshot");

    QApplication::setOverrideCursor(Qt::WaitCursor);
    pick->click();
    require(inspector.isPicking() && QApplication::overrideCursor() &&
                QApplication::overrideCursor()->shape() == Qt::CrossCursor,
            "explicit picking did not install its cursor");
    click(&button);
    require(!inspector.isPicking() && inspector.inspectedWidget() == &button &&
                activations == 0,
            "picking a command button invoked its action");
    require(QApplication::overrideCursor() &&
                QApplication::overrideCursor()->shape() == Qt::WaitCursor,
            "selection did not restore the previous cursor");
    QApplication::restoreOverrideCursor();
    mouse(&button, QEvent::MouseButtonDblClick, Qt::LeftButton, Qt::LeftButton);
    mouse(&button, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    require(activations == 0,
            "double-click picking invoked the underlying command on its tail");
    require(value(tree, "Class") == "QPushButton" &&
                value(tree, "Object Identifier") == "sampleAction" &&
                value(tree, "Enabled") == "Yes" &&
                tree->topLevelItemCount() == 5,
            "selected widget metadata was not displayed");
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
      const QString text = tree->topLevelItem(i)->text(1);
      require(!text.contains("Synthetic"),
              "inspector exposed widget text or arbitrary properties");
      require(!(tree->topLevelItem(i)->flags() & Qt::ItemIsEditable),
              "metadata rows became editable");
    }
    click(&button);
    require(activations == 1, "normal command input did not resume after picking");

    pick->setFocus();
    QWidget *focusBeforePick = QApplication::focusWidget();
    inspector.beginPicking();
    nativeMouse(&button, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    nativeMouse(&button, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    require(!inspector.isPicking() && activations == 1 &&
                inspector.inspectedWidget() == &button &&
                QApplication::focusWidget() == focusBeforePick,
            "platform-window selection changed focus or invoked a command");

    button.setEnabled(false);
    require(value(tree, "Enabled") == "Yes", "snapshot refreshed unexpectedly");
    refresh->click();
    require(value(tree, "Enabled") == "No", "Refresh did not read current metadata");
    button.setEnabled(true);
    inspector.setPanelTypeResolver([](QWidget *) { return QStringLiteral("SamplePanel"); });
    require(value(tree, "Panel Type") == "SamplePanel",
            "native panel type resolver was not reflected in the snapshot");
    inspector.inspectWidget(&inspector);
    inspector.inspectWidget(pick);
    require(inspector.inspectedWidget() == &button,
            "inspector accepted one of its own controls");
    clear->click();
    require(!inspector.inspectedWidget() && tree->topLevelItemCount() == 0 &&
                !refresh->isEnabled(), "Clear retained a target or snapshot");

    inspector.beginPicking();
    QKeyEvent typing(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
    QApplication::sendEvent(&canvas, &typing);
    click(&canvas);
    require(canvas.edits == 0 && inspector.inspectedWidget() == &canvas,
            "picking the viewer changed its contents or keyboard state");
    click(&canvas);
    require(canvas.edits == 2, "drawing input did not resume after selection");

    const int previousEdits = canvas.edits;
    inspector.beginPicking();
    mouse(&canvas, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &escape);
    require(!inspector.isPicking() && !QApplication::overrideCursor(),
            "Escape did not cancel picking and restore the cursor");
    mouse(&canvas, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    require(canvas.edits == previousEdits,
            "canceled picking delivered an unmatched release to a scene tool");
    click(&canvas);
    require(canvas.edits == previousEdits + 2,
            "canceled capture kept intercepting normal editing input");

    inspector.beginPicking();
    click(&canvas, Qt::RightButton);
    require(!inspector.isPicking() && canvas.edits == previousEdits + 2,
            "right-click cancellation was delivered to the viewer");

    inspector.beginPicking();
    tablet(&canvas, QEvent::TabletPress);
    tablet(&canvas, QEvent::TabletRelease);
    require(!inspector.isPicking() && inspector.inspectedWidget() == &canvas &&
                canvas.edits == previousEdits + 2,
            "tablet picking drew on the viewer or failed to finish");

    inspector.beginPicking();
    touch(&canvas, QEvent::TouchBegin);
    touch(&canvas, QEvent::TouchEnd);
    require(!inspector.isPicking() && inspector.inspectedWidget() == &canvas &&
                canvas.edits == previousEdits + 2,
            "touch picking changed the viewer or failed to finish");

    inspector.beginPicking();
    inspector.hide();
    require(!inspector.isPicking() && !QApplication::overrideCursor(),
            "hiding the inspector retained picking state");
    inspector.show();
    inspector.beginPicking();
    host.hide();
    require(!inspector.isPicking(), "hiding an ancestor retained the picker");
    host.show();
    app.processEvents();

    inspector.beginPicking();
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QApplication::sendEvent(&app, &deactivate);
    require(!inspector.isPicking() && !QApplication::overrideCursor(),
            "application deactivation retained the input filter");

    inspector.beginPicking();
    QDialog dialog(&host);
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.show();
    require(!inspector.isPicking(), "opening modal UI retained picking state");
    dialog.close();
    app.processEvents();

    auto *temporary = new QWidget(&host);
    temporary->setObjectName(QStringLiteral("temporaryTarget"));
    inspector.inspectWidget(temporary);
    delete temporary;
    inspector.refreshInspection();
    require(!inspector.inspectedWidget() && tree->topLevelItemCount() == 0 &&
                !refresh->isEnabled(), "destroying a target left a stale snapshot");

    // A closed panel must still swallow an in-flight selection release.
    auto *temporaryInspector = new WidgetInspector(&host);
    temporaryInspector->setGeometry(480, 20, 500, 430);
    temporaryInspector->show();
    temporaryInspector->beginPicking();
    mouse(&canvas, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    const int editsBeforeClose = canvas.edits;
    delete temporaryInspector;
    mouse(&canvas, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    require(canvas.edits == editsBeforeClose && !QApplication::overrideCursor(),
            "closing the inspector leaked selection input or a cursor override");
    click(&canvas);
    require(canvas.edits == editsBeforeClose + 2,
            "destroyed inspector left a global input filter active");

    std::cout << "Widget Inspector input and lifetime checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
