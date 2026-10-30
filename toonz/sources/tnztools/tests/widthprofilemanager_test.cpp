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
    std::cout << "width-profile-manager: rename then cancel" << std::endl;
    WidthProfile::Manager manager(path, profile, true);
    QTimer::singleShot(0, [&] {
      std::cout << "width-profile-manager: open rename dialog" << std::endl;
      QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        require(dialog, "rename opens native input dialog");
        dialog->setTextValue("Renamed"); dialog->accept();
      });
      button(manager, "Rename…")->click();
      require(manager.selectedProfile().name == "Renamed", "rename updates draft");
      std::cout << "width-profile-manager: reject renamed draft" << std::endl;
      manager.reject();
    });
    require(manager.exec() == QDialog::Rejected, "cancel manager");
    require(bytes(path) == original, "cancel must preserve library bytes");
  }
  {
    std::cout << "width-profile-manager: rename then save/apply" << std::endl;
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
    std::cout << "width-profile-manager: delete final profile then cancel" << std::endl;
    WidthProfile::Manager manager(path, profile, false);
    QTimer::singleShot(0, [&] {
      QTimer::singleShot(0, [] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        require(dialog, "delete asks confirmation");
        dialog->button(QMessageBox::Yes)->click();
      });
      button(manager, "Delete…")->click();
      require(manager.selectedProfile().empty(), "delete draft");
      std::cout << "width-profile-manager: final profile deleted" << std::endl;
      manager.reject();
    });
    manager.exec();
    require(bytes(path) == saved, "cancel delete preserves file");
  }
  {
    std::cout << "width-profile-manager: repeated last-row deletion" << std::endl;
    const QString severalPath = temp.filePath("several.txt");
    auto first = profile, second = profile, third = profile;
    first.name = "First"; second.name = "Second"; third.name = "Third";
    require(WidthProfile::writeLibrary(severalPath, {first,second,third}, error),
            "save multi-profile fixture");
    const QByteArray severalOriginal = bytes(severalPath);
    WidthProfile::Manager manager(severalPath, profile, false);
    QTimer::singleShot(0, [&] {
      auto *list = manager.findChild<QListWidget *>("widthProfileList");
      require(list && list->count() == 3, "native list contains every profile");
      list->setCurrentRow(2);
      for (int remaining = 2; remaining >= 0; --remaining) {
        QTimer::singleShot(0, [] {
          auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
          require(dialog, "repeated delete asks confirmation");
          dialog->button(QMessageBox::Yes)->click();
        });
        button(manager, "Delete…")->click();
        require(list->count() == remaining, "delete rebuilds native list");
        const std::string expected = remaining == 2 ? "Second" : remaining == 1 ? "First" : "";
        require(manager.selectedProfile().name == expected,
                "delete selects the preceding surviving draft");
        // Repaint the actual preview after each deletion, including empty.
        manager.repaint();
        std::cout << "width-profile-manager: profiles remaining=" << remaining << std::endl;
      }
      manager.reject();
    });
    require(manager.exec() == QDialog::Rejected, "cancel repeated deletion");
    require(bytes(severalPath) == severalOriginal, "repeated delete cancel preserves library bytes");
  }
  // Invalid input is rejected wholesale; failed atomic saves preserve data.
  std::cout << "width-profile-manager: malformed import and atomic save errors" << std::endl;
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
