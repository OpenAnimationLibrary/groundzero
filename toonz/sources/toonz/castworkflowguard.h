#pragma once

#include "tapp.h"
#include "toonz/tscenehandle.h"

#include <QDialog>
#include <QEvent>
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QVariant>
#include <QWindow>
#include <vector>

// One scope follows the original Cast owner through every nested prompt.
// Stack windows have no QObject parent: destroying a panel in a nested event
// loop must not delete a dialog that will also be destroyed on stack unwind.
class CastWorkflowGuard final : public QObject {
  QPointer<QWidget> m_owner;
  ToonzScene *m_scene;
  bool m_invalid = false;
  std::vector<QPointer<QWidget>> m_windows;

  void invalidate() {
    if (m_invalid) return;
    m_invalid = true;  // Sticky, including hide/show and scene leave/return.
    const auto windows = m_windows;
    for (const auto &window : windows) {
      if (!window) continue;
      // QFileDialog may be inside its own overwrite-confirmation event loop.
      // Reject its nested Qt dialogs too, without transferring or deleting
      // ownership, so closing the Cast cannot leave that prompt stranded.
      std::vector<QPointer<QDialog>> children;
      for (auto *dialog : window->findChildren<QDialog *>())
        children.emplace_back(dialog);
      window->setEnabled(false);
      for (auto it = children.rbegin(); it != children.rend(); ++it)
        if (*it) (*it)->reject();
      if (!window) continue;
      if (auto *progress = qobject_cast<QProgressDialog *>(window.data()))
        progress->cancel();
      else if (auto *dialog = qobject_cast<QDialog *>(window.data()))
        dialog->reject();
      else
        window->close();
    }
  }

protected:
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::Hide || event->type() == QEvent::Close ||
        event->type() == QEvent::Destroy)
      invalidate();
    return false;
  }

public:
  explicit CastWorkflowGuard(QWidget *owner)
      : m_owner(owner),
        m_scene(TApp::instance()->getCurrentScene()->getScene()) {
    for (QWidget *widget = owner; widget; widget = widget->parentWidget()) {
      widget->installEventFilter(this);
      connect(widget, &QObject::destroyed, this, [this]() { invalidate(); });
    }
    connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched,
            this, [this]() { invalidate(); });
  }

  bool valid() const {
    return !m_invalid && m_owner &&
           TApp::instance()->getCurrentScene()->getScene() == m_scene;
  }

  bool canAct(const QDialog &dialog) const {
    return valid() && !dialog.property("CastWorkflowFinished").toBool();
  }

  void cancel() { invalidate(); }

  // The transient relation supplies native placement/modality without QObject
  // ownership. Nested prompts may use their review window as visual owner.
  void watch(QWidget &window, QWidget *visualOwner = nullptr) {
    m_windows.emplace_back(&window);
    if (!valid()) { window.setEnabled(false); return; }
    if (auto *dialog = qobject_cast<QDialog *>(&window)) {
      dialog->setProperty("CastWorkflowFinished", false);
      dialog->setProperty("CastWorkflowRejected", false);
      connect(dialog, &QDialog::finished, dialog, [dialog]() {
        dialog->setProperty("CastWorkflowFinished", true);
      });
      connect(dialog, &QDialog::rejected, dialog, [dialog]() {
        dialog->setProperty("CastWorkflowRejected", true);
      });
    }
    QPointer<QWidget> target = visualOwner ? visualOwner : m_owner.data();
    QPointer<QWidget> top = target ? target->window() : nullptr;
    window.setWindowModality(Qt::WindowModal);
    if (top) {
      top->winId();
      window.winId();
      if (top && top->windowHandle() && window.windowHandle())
        window.windowHandle()->setTransientParent(top->windowHandle());
    }
  }

  int exec(QDialog &dialog, QWidget *visualOwner = nullptr) {
    if (!valid()) return QDialog::Rejected;
    watch(dialog, visualOwner);
    if (!valid()) return QDialog::Rejected;
    const int result = dialog.exec();
    return valid() && !dialog.property("CastWorkflowRejected").toBool()
               ? result : QDialog::Rejected;
  }

  QAction *exec(QMenu &menu, const QPoint &position) {
    if (!valid()) return nullptr;
    watch(menu);
    if (!valid()) return nullptr;
    QAction *action = menu.exec(position);
    return valid() ? action : nullptr;
  }

  QString file(const QString &title, const QString &directory,
               const QString &filter, QFileDialog::FileMode mode,
               QFileDialog::AcceptMode acceptMode = QFileDialog::AcceptOpen,
               QWidget *visualOwner = nullptr) {
    if (!valid()) return {};
    QFileDialog dialog(nullptr, title, directory, filter);
    dialog.setFileMode(mode);
    dialog.setAcceptMode(acceptMode);
    if (mode == QFileDialog::Directory) dialog.setOption(QFileDialog::ShowDirsOnly);
    if (exec(dialog, visualOwner) != QDialog::Accepted) return {};
    const auto files = dialog.selectedFiles();
    return files.empty() ? QString() : files.front();
  }

  QString directory(const QString &title, const QString &initial = {},
                    QWidget *visualOwner = nullptr) {
    return file(title, initial, {}, QFileDialog::Directory,
                QFileDialog::AcceptOpen, visualOwner);
  }
  QString openFile(const QString &title, const QString &initial = {},
                   const QString &filter = {}, QWidget *visualOwner = nullptr) {
    return file(title, initial, filter, QFileDialog::ExistingFile,
                QFileDialog::AcceptOpen, visualOwner);
  }
  QString saveFile(const QString &title, const QString &filter,
                   QWidget *visualOwner = nullptr) {
    return file(title, {}, filter, QFileDialog::AnyFile,
                QFileDialog::AcceptSave, visualOwner);
  }
  QMessageBox::StandardButton message(QMessageBox::Icon icon,
      const QString &title, const QString &text,
      QMessageBox::StandardButtons buttons = QMessageBox::Ok,
      QWidget *visualOwner = nullptr) {
    if (!valid()) return QMessageBox::NoButton;
    QMessageBox dialog(icon, title, text, buttons);
    const int result = exec(dialog, visualOwner);
    return valid() && result != QDialog::Rejected
               ? dialog.standardButton(dialog.clickedButton()) : QMessageBox::NoButton;
  }
  void warning(const QString &title, const QString &text,
               QWidget *visualOwner = nullptr) {
    message(QMessageBox::Warning, title, text, QMessageBox::Ok, visualOwner);
  }
  bool confirm(const QString &title, const QString &text,
               QWidget *visualOwner = nullptr) {
    return message(QMessageBox::Question, title, text,
                   QMessageBox::Yes | QMessageBox::No, visualOwner) == QMessageBox::Yes;
  }
};
