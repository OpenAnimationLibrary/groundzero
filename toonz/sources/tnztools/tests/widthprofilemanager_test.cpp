#include "tools/widthprofilemanager.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdlib>
#include <iostream>

static void require(bool ok, const char *message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static QPushButton *button(QWidget &widget, const QString &text) {
  for (auto *b : widget.findChildren<QPushButton *>())
    if (b->text() == text) return b;
  require(false, "native manager button missing");
  return nullptr;
}
static QByteArray bytes(const QString &path) {
  QFile f(path); require(f.open(QIODevice::ReadOnly), "open saved library");
  return f.readAll();
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QTemporaryDir temp;
  require(temp.isValid(), "temporary directory");
  const QString path = temp.filePath("profiles.txt");
  QString error;
  auto profile = WidthProfile::builtIn(WidthProfile::TAPER_BOTH);
  require(WidthProfile::writeLibrary(path, {profile}, error), "initial save");
  const QByteArray original = bytes(path);
  {
    WidthProfile::Manager manager(path, profile, true);
    QTimer::singleShot(0, [&] {
      QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        require(dialog, "rename opens native input dialog");
        dialog->setTextValue("Renamed"); dialog->accept();
      });
      button(manager, "Rename…")->click();
      require(manager.selectedProfile().name == "Renamed", "rename updates draft");
      manager.reject();
    });
    require(manager.exec() == QDialog::Rejected, "cancel manager");
    require(bytes(path) == original, "cancel must preserve library bytes");
  }
  {
    WidthProfile::Manager manager(path, profile, true);
    QTimer::singleShot(0, [&] {
      QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        require(dialog, "rename dialog");
        dialog->setTextValue("Renamed"); dialog->accept();
      });
      button(manager, "Rename…")->click();
      button(manager, "Save and Apply")->click();
    });
    require(manager.exec() == QDialog::Accepted && manager.shouldApply(), "save/apply result");
    require(manager.selectedProfile().name == "Renamed", "selected draft retained");
  }
  std::vector<WidthProfile::Profile> reopened;
  QByteArray saved;
  require(WidthProfile::readLibrary(path, reopened, saved, error) &&
              reopened.size() == 1 && reopened[0].name == "Renamed", "reopen renamed profile");
  require(WidthProfile::evaluate(reopened[0], .5) == 1.0, "shape retained");
  {
    WidthProfile::Manager manager(path, profile, false);
    QTimer::singleShot(0, [&] {
      QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        require(dialog, "delete asks confirmation");
        dialog->button(QMessageBox::Yes)->click();
      });
      button(manager, "Delete…")->click();
      require(manager.selectedProfile().empty(), "delete draft");
      manager.reject();
    });
    manager.exec();
    require(bytes(path) == saved, "cancel delete preserves file");
  }
  // Invalid input is rejected wholesale; failed atomic saves preserve data.
  const QString bad = temp.filePath("bad.txt");
  { QFile f(bad); require(f.open(QIODevice::WriteOnly), "write invalid fixture");
    f.write("Good\t0:1,1:1\nBad\t0:0junk\n"); }
  QByteArray unused;
  require(!WidthProfile::readLibrary(bad, reopened, unused, error) && reopened.empty(),
          "malformed import must not partly succeed");
  require(!WidthProfile::writeLibrary(temp.path(), {profile}, error), "directory target fails");
  require(bytes(path) == saved, "unrelated failed save preserves library");
  std::cout << "Native width-profile draft, save, cancel and storage checks passed\n";
}
