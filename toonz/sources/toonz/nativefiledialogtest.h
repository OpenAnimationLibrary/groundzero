#pragma once

#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QListView>
#include <iostream>

namespace NativeUiTest {
// Qt 5 selectFile(absoluteDirectory) changes to its parent and can skip the
// filename update while the visible filename edit has focus. Choose the root
// directly, clear old selection/text, and verify its identity before accepting.
// See qtbase/src/widgets/dialogs/qfiledialog.cpp: selectFile/selectedFiles.
inline bool acceptDirectory(QFileDialog *dialog, const QString &requested) {
  dialog->setDirectory(requested);
  auto *edit = dialog->findChild<QLineEdit *>("fileNameEdit");
  auto *view = dialog->findChild<QListView *>("listView");
  if (!edit || !view || !view->selectionModel()) {
    std::cerr << "native-file-dialog: FAIL: widget directory controls unavailable" << std::endl;
    dialog->reject();
    return false;
  }
  // setDirectory can return early when the requested root is already current.
  view->selectionModel()->clear();
  edit->clear();
  const QStringList selected = dialog->selectedFiles();
  const QFileInfo expected(requested);
  const QString canonical = expected.canonicalFilePath();
  std::cerr << "native-file-dialog: requested directory=" << requested.toStdString()
            << "; current=" << dialog->directory().absolutePath().toStdString()
            << "; selected=" << selected.join(" | ").toStdString() << std::endl;
  if (!expected.isDir() || canonical.isEmpty() || selected.size() != 1 ||
      QFileInfo(selected.front()).canonicalFilePath() != canonical) {
    // Reject rather than throwing through Qt's timer event handler. Existing
    // workflow assertions then fail with the exact selection diagnostic above.
    std::cerr << "native-file-dialog: FAIL: selected directory identity differs from requested directory" << std::endl;
    dialog->reject();
    return false;
  }
  static_cast<QDialog *>(dialog)->accept();
  return true;
}
}  // namespace NativeUiTest
