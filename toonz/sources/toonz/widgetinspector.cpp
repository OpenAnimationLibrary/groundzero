#include "widgetinspector.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QHeaderView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabletEvent>
#include <QTimer>
#include <QTouchEvent>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <utility>

namespace {
QPoint mousePosition(const QMouseEvent *event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return event->globalPosition().toPoint();
#else
  return event->globalPos();
#endif
}

QWidget *eventWidget(QObject *watched, QEvent *event) {
  if (auto *widget = qobject_cast<QWidget *>(watched)) return widget;
  if (!qobject_cast<QWindow *>(watched)) return nullptr;
  QPoint global;
  switch (event->type()) {
  case QEvent::MouseButtonPress:
  case QEvent::MouseButtonDblClick:
  case QEvent::MouseButtonRelease:
  case QEvent::MouseMove:
    global = mousePosition(static_cast<QMouseEvent *>(event));
    break;
  case QEvent::TabletPress:
  case QEvent::TabletRelease:
  case QEvent::TabletMove:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    global = static_cast<QTabletEvent *>(event)->globalPosition().toPoint();
#else
    global = static_cast<QTabletEvent *>(event)->globalPos();
#endif
    break;
  case QEvent::TouchBegin:
  case QEvent::TouchUpdate:
  case QEvent::TouchEnd: {
    auto *touch = static_cast<QTouchEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (touch->points().isEmpty()) return nullptr;
    global = touch->points().first().globalPosition().toPoint();
#else
    if (touch->touchPoints().isEmpty()) return nullptr;
    global = touch->touchPoints().first().screenPos().toPoint();
#endif
    break;
  }
  case QEvent::Wheel:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    global = static_cast<QWheelEvent *>(event)->globalPosition().toPoint();
#else
    global = static_cast<QWheelEvent *>(event)->globalPos();
#endif
    break;
  default:
    return nullptr;
  }
  return QApplication::widgetAt(global);
}
}  // namespace

// A canceled selection can still have a release in flight. Keep only that
// input sequence captured until its release, even if the inspector is closed.
// This prevents a scene tool from receiving an unmatched editing release.
class WidgetInspectorPickSession final : public QObject {
public:
  explicit WidgetInspectorPickSession(WidgetInspector *owner)
      : QObject(qApp), m_owner(owner) {
    if (s_active) s_active->finish();
    s_active = this;
    qApp->installEventFilter(this);
    QApplication::setOverrideCursor(Qt::CrossCursor);
    m_hasCursor = true;
    connect(qApp, &QCoreApplication::aboutToQuit, this,
            [this]() { finish(); });
  }

  ~WidgetInspectorPickSession() override {
    releaseCursor();
    if (qApp) qApp->removeEventFilter(this);
  }

  void cancel() {
    if (m_finished) return;
    detachOwner();
    m_candidate.clear();
    releaseCursor();
    if (m_sequence == Sequence::None) finish();
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (m_finished) return false;

    const QEvent::Type type = event->type();
    if (type == QEvent::ApplicationDeactivate ||
        (type == QEvent::ApplicationStateChange &&
         QApplication::applicationState() != Qt::ApplicationActive)) {
      finish();
      return false;
    }

    QWidget *widget = eventWidget(watched, event);
    if (m_owner && widget) {
      const bool ownerHidden =
          (type == QEvent::Hide || type == QEvent::Close) &&
          (widget == m_owner || widget->isAncestorOf(m_owner));
      const bool modalShown =
          type == QEvent::Show && widget->isWindow() && widget->isModal() &&
          widget != m_owner && !m_owner->isAncestorOf(widget);
      if (ownerHidden || modalShown) {
        cancel();
        return false;
      }
    }

    // Once canceled, consume only the remainder of the captured gesture.
    // Unrelated keys and interactions resume immediately.
    if (!m_owner) return drainInput(watched, event);

    if (type == QEvent::ShortcutOverride) {
      event->accept();
      return true;
    }
    if (type == QEvent::KeyPress || type == QEvent::KeyRelease) {
      if (type == QEvent::KeyPress &&
          static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape)
        cancel();
      event->accept();
      return true;
    }

    // Capture platform-window input before Qt routes it to a widget and
    // changes keyboard focus. The same filter also handles widget events.
    if (!widget) return false;
    const bool ownControl = widget == m_owner || m_owner->isAncestorOf(widget);
    if (ownControl && m_sequence == Sequence::None) return false;

    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
      auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() == Qt::RightButton && m_sequence != Sequence::None) {
        cancel();
        return consume(event);
      }
      if (m_sequence == Sequence::None) {
        m_sequence = Sequence::Mouse;
        m_mouseButton = mouse->button();
        QWidget *target = QApplication::widgetAt(mousePosition(mouse));
        if (!target) target = widget;
        if (target != m_owner && !m_owner->isAncestorOf(target) &&
            mouse->button() == Qt::LeftButton)
          m_candidate = target;
      }
      return consume(event);
    }
    case QEvent::MouseButtonRelease: {
      auto *mouse = static_cast<QMouseEvent *>(event);
      if (m_sequence == Sequence::Mouse && mouse->button() == m_mouseButton)
        selectCandidate();
      return consume(event);
    }
    case QEvent::TabletPress:
      if (m_sequence == Sequence::None) {
        m_sequence = Sequence::Tablet;
        if (static_cast<QTabletEvent *>(event)->button() != Qt::RightButton)
          m_candidate = widget;
      }
      return consume(event);
    case QEvent::TabletRelease:
      if (m_sequence == Sequence::Tablet) selectCandidate();
      return consume(event);
    case QEvent::TouchBegin:
      if (m_sequence == Sequence::None) {
        m_sequence = Sequence::Touch;
        m_candidate = widget;
      }
      return consume(event);
    case QEvent::TouchEnd:
      if (m_sequence == Sequence::Touch) selectCandidate();
      return consume(event);
    case QEvent::TouchCancel:
      finish();
      return consume(event);
    case QEvent::MouseMove:
    case QEvent::TabletMove:
    case QEvent::TouchUpdate:
    case QEvent::Wheel:
    case QEvent::ContextMenu:
    case QEvent::Gesture:
    case QEvent::GestureOverride:
    case QEvent::NativeGesture:
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::DragLeave:
    case QEvent::Drop:
      return consume(event);
    default:
      return false;
    }
  }

private:
  enum class Sequence { None, Mouse, Tablet, Touch, DoubleClickTail };
  static QPointer<WidgetInspectorPickSession> s_active;
  QPointer<WidgetInspector> m_owner;
  QPointer<QWidget> m_candidate;
  Sequence m_sequence = Sequence::None;
  Qt::MouseButton m_mouseButton = Qt::NoButton;
  bool m_hasCursor = false;
  bool m_finished = false;

  static bool consume(QEvent *event) {
    event->accept();
    return true;
  }

  void releaseCursor() {
    if (!m_hasCursor) return;
    m_hasCursor = false;
    QApplication::restoreOverrideCursor();
  }

  void detachOwner() {
    if (!m_owner) return;
    QPointer<WidgetInspector> owner = m_owner;
    m_owner.clear();
    owner->pickingFinished();
  }

  void finish() {
    if (m_finished) return;
    m_finished = true;
    if (qApp) qApp->removeEventFilter(this);
    releaseCursor();
    detachOwner();
    m_candidate.clear();
    if (s_active == this) s_active.clear();
    deleteLater();
  }

  void selectCandidate() {
    QPointer<WidgetInspector> owner = m_owner;
    QPointer<QWidget> candidate = m_candidate;
    if (m_sequence == Sequence::Mouse && m_mouseButton == Qt::LeftButton) {
      // A second click may arrive as MouseButtonDblClick after the first
      // selection completed. Swallow that tail, while allowing an ordinary
      // new click immediately. Otherwise double-click picking invokes the
      // underlying button or drawing tool on its second release.
      m_sequence = Sequence::DoubleClickTail;
      releaseCursor();
      detachOwner();
      QTimer::singleShot(QApplication::doubleClickInterval(), this,
                        [this]() {
                          if (m_sequence == Sequence::DoubleClickTail) finish();
                        });
    } else {
      finish();
    }
    if (owner && candidate) owner->inspectWidget(candidate);
  }

  bool drainInput(QObject *watched, QEvent *event) {
    const QEvent::Type type = event->type();
    switch (m_sequence) {
    case Sequence::Mouse:
      if (type == QEvent::MouseMove) return consume(event);
      if (type == QEvent::MouseButtonRelease) {
        if (static_cast<QMouseEvent *>(event)->button() == m_mouseButton)
          finish();
        return consume(event);
      }
      if (type == QEvent::MouseButtonPress) finish();
      break;
    case Sequence::Tablet:
      if (type == QEvent::TabletMove) return consume(event);
      if (type == QEvent::TabletRelease) {
        finish();
        return consume(event);
      }
      if (type == QEvent::TabletPress) finish();
      break;
    case Sequence::Touch:
      if (type == QEvent::TouchUpdate) return consume(event);
      if (type == QEvent::TouchEnd || type == QEvent::TouchCancel) {
        finish();
        return consume(event);
      }
      if (type == QEvent::TouchBegin) finish();
      break;
    case Sequence::None:
      finish();
      break;
    case Sequence::DoubleClickTail:
      if (type == QEvent::MouseButtonDblClick) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        QWidget *target = QApplication::widgetAt(mousePosition(mouse));
        if (!target) target = qobject_cast<QWidget *>(watched);
        if (m_candidate && target == m_candidate &&
            mouse->button() == Qt::LeftButton) {
          m_sequence = Sequence::Mouse;
          m_mouseButton = Qt::LeftButton;
          return consume(event);
        }
        finish();
      } else if (type == QEvent::MouseButtonPress) {
        finish();
      }
      break;
    }
    return false;
  }
};

QPointer<WidgetInspectorPickSession> WidgetInspectorPickSession::s_active;

WidgetInspector::WidgetInspector(QWidget *parent)
    : QWidget(parent)
    , m_pickButton(new QPushButton(this))
    , m_refreshButton(new QPushButton(this))
    , m_clearButton(new QPushButton(this))
    , m_status(new QLabel(this))
    , m_properties(new QTreeWidget(this)) {
  setObjectName(QStringLiteral("widgetInspector"));
  m_pickButton->setObjectName(QStringLiteral("widgetInspectorPick"));
  m_refreshButton->setObjectName(QStringLiteral("widgetInspectorRefresh"));
  m_clearButton->setObjectName(QStringLiteral("widgetInspectorClear"));
  m_status->setObjectName(QStringLiteral("widgetInspectorStatus"));
  m_properties->setObjectName(QStringLiteral("widgetInspectorProperties"));
  m_status->setWordWrap(true);
  m_properties->setColumnCount(2);
  m_properties->setAlternatingRowColors(true);
  m_properties->setRootIsDecorated(false);
  m_properties->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_properties->header()->setStretchLastSection(true);

  auto *buttons = new QHBoxLayout;
  buttons->addWidget(m_pickButton);
  buttons->addWidget(m_refreshButton);
  buttons->addWidget(m_clearButton);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->addLayout(buttons);
  layout->addWidget(m_status);
  layout->addWidget(m_properties, 1);

  connect(m_pickButton, &QPushButton::clicked, this, [this]() {
    if (isPicking()) cancelPicking();
    else beginPicking();
  });
  connect(m_refreshButton, &QPushButton::clicked, this,
          &WidgetInspector::refreshInspection);
  connect(m_clearButton, &QPushButton::clicked, this,
          &WidgetInspector::clearInspection);
  updateText();
}

WidgetInspector::~WidgetInspector() { cancelPicking(); }

void WidgetInspector::setPanelTypeResolver(PanelTypeResolver resolver) {
  m_panelTypeResolver = std::move(resolver);
  if (m_target) refreshInspection();
}

QWidget *WidgetInspector::inspectedWidget() const { return m_target.data(); }

bool WidgetInspector::isPicking() const { return !m_picker.isNull(); }

void WidgetInspector::beginPicking() {
  if (isPicking() || !isVisible() || QApplication::activeModalWidget()) return;
  m_picker = new WidgetInspectorPickSession(this);
  updateText();
}

void WidgetInspector::cancelPicking() {
  if (m_picker) m_picker->cancel();
}

void WidgetInspector::pickingFinished() {
  m_picker.clear();
  updateText();
}

void WidgetInspector::inspectWidget(QWidget *widget) {
  if (!widget || widget == this || isAncestorOf(widget)) return;
  cancelPicking();
  QObject::disconnect(m_destroyedConnection);
  m_target = widget;
  m_destroyedConnection = connect(widget, &QObject::destroyed, this,
                                 &WidgetInspector::clearInspection);
  refreshInspection();
}

void WidgetInspector::refreshInspection() {
  if (!m_target) {
    clearInspection();
    return;
  }
  m_className = QString::fromLatin1(m_target->metaObject()->className());
  m_objectName = m_target->objectName();
  m_geometry = m_target->geometry();
  m_visible = m_target->isVisible();
  m_enabled = m_target->isEnabled();
  m_panelType = m_panelTypeResolver ? m_panelTypeResolver(m_target) : QString();
  showSnapshot();
  updateText();
}

void WidgetInspector::clearInspection() {
  cancelPicking();
  QObject::disconnect(m_destroyedConnection);
  m_target.clear();
  m_properties->clear();
  m_className.clear();
  m_objectName.clear();
  m_panelType.clear();
  updateText();
}

void WidgetInspector::showSnapshot() {
  m_properties->clear();
  if (!m_target) return;
  const auto row = [this](const QString &name, const QString &value) {
    new QTreeWidgetItem(m_properties, QStringList{name, value});
  };
  row(tr("Class"), m_className);
  row(tr("Object Identifier"), m_objectName);
  row(tr("Local Geometry"),
      tr("%1 x %2 at (%3, %4)")
          .arg(m_geometry.width()).arg(m_geometry.height())
          .arg(m_geometry.x()).arg(m_geometry.y()));
  row(tr("Visible"), m_visible ? tr("Yes") : tr("No"));
  row(tr("Enabled"), m_enabled ? tr("Yes") : tr("No"));
  if (!m_panelType.isEmpty()) row(tr("Panel Type"), m_panelType);
}

void WidgetInspector::updateText() {
  setWindowTitle(tr("Widget Inspector"));
  m_pickButton->setText(isPicking() ? tr("Cancel Picking") : tr("Pick Widget"));
  m_refreshButton->setText(tr("Refresh"));
  m_clearButton->setText(tr("Clear"));
  m_properties->setHeaderLabels({tr("Property"), tr("Value")});
  m_refreshButton->setEnabled(m_target && !isPicking());
  m_clearButton->setEnabled(m_target || isPicking());
  m_status->setText(isPicking()
      ? tr("Select a widget. Escape or right-click cancels.")
      : m_target ? tr("Read-only snapshot. Refresh to update the values.")
                 : tr("Pick a widget to inspect its metadata."));
}

void WidgetInspector::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::LanguageChange) {
    updateText();
    showSnapshot();
  }
}

void WidgetInspector::hideEvent(QHideEvent *event) {
  cancelPicking();
  QWidget::hideEvent(event);
}

void WidgetInspector::closeEvent(QCloseEvent *event) {
  cancelPicking();
  QWidget::closeEvent(event);
}
