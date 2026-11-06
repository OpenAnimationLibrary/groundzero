#pragma once

#include <QDialog>
#include <QEvent>
#include <QPointer>
#include <QVariant>
#include <QWindow>
#include <vector>

// A native review owns its stack windows. Its original visual owner may close
// or be destroyed inside exec(), but must never delete those stack objects or
// let a queued Apply revive the cancelled operation.
class VectorModalGuard final : public QObject {
  QPointer<QWidget> m_owner;
  bool m_hadOwner = false, m_invalid = false;
  std::vector<QPointer<QDialog>> m_dialogs;

protected:
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::Hide || event->type() == QEvent::Close ||
        event->type() == QEvent::Destroy)
      cancel();
    return false;
  }

public:
  explicit VectorModalGuard(QWidget *owner)
      : m_owner(owner), m_hadOwner(owner != nullptr) {
    for (QWidget *widget = owner; widget; widget = widget->parentWidget()) {
      widget->installEventFilter(this);
      connect(widget, &QObject::destroyed, this, [this] { cancel(); });
    }
  }
  bool valid() const { return !m_invalid && (!m_hadOwner || m_owner); }
  bool canAct(const QDialog &dialog) const {
    return valid() && !dialog.property("VectorModalFinished").toBool();
  }
  bool accepted(const QDialog &dialog) const {
    return valid() && !dialog.property("VectorModalRejected").toBool();
  }
  void cancel() {
    if (m_invalid) return;
    m_invalid = true;
    const auto dialogs = m_dialogs;
    for (const auto &dialog : dialogs) {
      if (!dialog) continue;
      // Native QFileDialog can be inside its own overwrite confirmation.
      // Reject nested loops without deleting windows still on another stack.
      std::vector<QPointer<QDialog>> children;
      for (auto *child : dialog->findChildren<QDialog *>()) children.emplace_back(child);
      dialog->setEnabled(false);
      for (auto it = children.rbegin(); it != children.rend(); ++it)
        if (*it) (*it)->reject();
      if (dialog) dialog->reject();
    }
  }
  void watch(QDialog &dialog) {
    m_dialogs.emplace_back(&dialog);
    dialog.setProperty("VectorModalFinished", false);
    dialog.setProperty("VectorModalRejected", false);
    connect(&dialog, &QDialog::finished, this,
            [&dialog] { dialog.setProperty("VectorModalFinished", true); });
    connect(&dialog, &QDialog::rejected, this,
            [&dialog] { dialog.setProperty("VectorModalRejected", true); });
    if (!valid()) { dialog.setEnabled(false); return; }
    dialog.setEnabled(true);  // Reusable heap reviews may have been cancelled earlier.
    // Native placement/modality without QObject ownership of a stack window.
    QPointer<QWidget> top = m_owner ? m_owner->window() : nullptr;
    dialog.setWindowModality(Qt::WindowModal);
    if (top) {
      top->winId();
      dialog.winId();
      if (top && top->windowHandle() && dialog.windowHandle())
        dialog.windowHandle()->setTransientParent(top->windowHandle());
    }
  }
  int exec(QDialog &dialog) {
    watch(dialog);
    if (!valid()) return QDialog::Rejected;
    QPointer<QDialog> live(&dialog);
    const int result = dialog.exec();
    return live && accepted(*live) ? result : QDialog::Rejected;
  }
};
