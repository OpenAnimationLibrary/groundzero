#include "tools/widthprofilemanager.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QLineEdit>
#include <QPixmap>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <utility>

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

struct DialogStep {
  enum Kind { Input, File, Message } kind;
  const char *description;
  std::function<void(QDialog *)> action;
};

static QString describeDialog(QDialog *dialog) {
  if (!dialog) return QStringLiteral("no active modal");
  auto bounded = [](QString text, int limit) {
    text.replace('\n', ' ');
    text.replace('\r', ' ');
    return text.left(limit);
  };
  QString result = QStringLiteral("class=%1 title=\"%2\" visible=%3")
      .arg(bounded(QString::fromLatin1(dialog->metaObject()->className()), 80))
      .arg(bounded(dialog->windowTitle(), 120)).arg(dialog->isVisible());
  if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
    result += QStringLiteral(" icon=%1 text=\"%2\" informative=\"%3\"")
        .arg(int(message->icon())).arg(bounded(message->text(), 320))
        .arg(bounded(message->informativeText(), 160));
    const auto buttons = message->buttons();
    result += QStringLiteral(" buttons=%1").arg(buttons.size());
    for (int i = 0; i < std::min(6, int(buttons.size())); ++i)
      result += QStringLiteral(" [%1 role=%2 standard=%3 enabled=%4]")
          .arg(bounded(buttons[i]->text(), 80))
          .arg(int(message->buttonRole(buttons[i])))
          .arg(int(message->standardButton(buttons[i])))
          .arg(buttons[i]->isEnabled());
  } else if (auto *file = qobject_cast<QFileDialog *>(dialog)) {
    const auto selected = file->selectedFiles();
    result += QStringLiteral(" acceptMode=%1 fileMode=%2 selected=%3")
        .arg(int(file->acceptMode())).arg(int(file->fileMode()))
        .arg(selected.size());
    for (int i = 0; i < std::min(2, int(selected.size())); ++i)
      result += QStringLiteral(" [%1]").arg(bounded(selected[i], 200));
  }
  return result;
}

// Queue every modal action on its receiver. A QFileDialog's accept() can itself
// open the overwrite question; invoking it inside this repeating timer would
// prevent the timer from driving that nested dialog on Qt 5.
class DialogScript {
  std::vector<DialogStep> m_steps;
  size_t m_next = 0;
  QPointer<QDialog> m_last;
  QTimer m_timer;
  QElapsedTimer m_elapsed;
  const char *m_operation;
public:
  explicit DialogScript(std::vector<DialogStep> steps, const char *operation = nullptr)
      : m_steps(std::move(steps)), m_operation(operation) {
    if (m_operation)
      std::cout << "width-profile-manager: " << m_operation << std::endl;
    m_elapsed.start();
    QObject::connect(&m_timer, &QTimer::timeout, [&] {
      if (m_elapsed.elapsed() > 10000) {
        std::cerr << "width-profile-manager: timed out waiting for "
                  << (m_next < m_steps.size() ? m_steps[m_next].description
                                             : "the operation to return") << '\n';
        std::cerr << "width-profile-manager: operation="
                  << (m_operation ? m_operation : "unspecified")
                  << " completed-steps=" << m_next << '/' << m_steps.size()
                  << ' ' << describeDialog(qobject_cast<QDialog *>(
                         QApplication::activeModalWidget())).toStdString() << '\n';
        std::exit(1);
      }
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog || dialog == m_last || dialog->objectName() == "widthProfileManager") return;
      if (m_operation || m_next >= m_steps.size())
        std::cerr << "width-profile-manager: operation="
                  << (m_operation ? m_operation : "unspecified")
                  << " dialog=" << (m_next + 1) << '/' << m_steps.size()
                  << ' ' << describeDialog(dialog).toStdString() << '\n';
      require(m_next < m_steps.size(), "unexpected additional native dialog");
      const auto step = m_steps[m_next];
      const bool expected = step.kind == DialogStep::Input ? bool(qobject_cast<QInputDialog *>(dialog))
          : step.kind == DialogStep::File ? bool(qobject_cast<QFileDialog *>(dialog))
                                         : bool(qobject_cast<QMessageBox *>(dialog));
      if (!expected) {
        std::cerr << "width-profile-manager: expected " << step.description
                  << ", observed " << describeDialog(dialog).toStdString() << '\n';
        std::exit(1);
      }
      ++m_next;
      m_last = dialog;
      m_elapsed.restart();
      QTimer::singleShot(0, dialog, [step, dialog] { step.action(dialog); });
    });
    m_timer.start(1);
  }
  void finish() {
    m_timer.stop();
    require(m_next == m_steps.size(), "native operation skipped an expected dialog");
  }
};

static DialogStep inputStep(const QString &text, bool accept = true) {
  return {DialogStep::Input, "profile input", [=](QDialog *dialog) {
    auto *input = qobject_cast<QInputDialog *>(dialog);
    input->setTextValue(text);
    if (accept) input->accept(); else input->reject();
  }};
}
static DialogStep fileStep(const QString &path, bool accept = true,
                          const std::function<void()> &afterAccepted = {}) {
  return {DialogStep::File, "profile file chooser", [=](QDialog *dialog) {
    auto *file = qobject_cast<QFileDialog *>(dialog);
    require(file->testOption(QFileDialog::DontUseNativeDialog) ||
                QApplication::testAttribute(Qt::AA_DontUseNativeDialogs),
            "file chooser must expose its real Qt controls to the test");
    if (!path.isEmpty()) {
      const QFileInfo requested(path);
      file->setDirectory(requested.absolutePath());
      auto *filename = file->findChild<QLineEdit *>("fileNameEdit");
      require(filename, "profile file chooser filename control is missing");
      // Qt's selectFile() deliberately leaves a visible, focused filename
      // field unchanged. Enter the exact path through the real control so
      // both ExistingFile and Save keep their ordinary native validation.
      filename->setFocus();
      filename->setText(requested.absoluteFilePath());
      const QStringList selected = file->selectedFiles();
      auto exactPath = [](const QFileInfo &info) {
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty()) return canonical;
        // A new export has no canonical file yet. Resolve its existing
        // parent so platform aliases still identify the same destination.
        const QString parent = info.absoluteDir().canonicalPath();
        return parent.isEmpty() ? QDir::cleanPath(info.absoluteFilePath())
            : QDir(parent).filePath(info.fileName());
      };
      require(selected.size() == 1 &&
                  exactPath(QFileInfo(selected.front())) == exactPath(requested),
              "profile file chooser did not select the exact requested path");
      if (file->acceptMode() == QFileDialog::AcceptOpen)
        require(requested.isFile(), "profile import source file is missing");
    }
    if (afterAccepted)
      QObject::connect(file, &QDialog::accepted, file, afterAccepted);
    if (accept) {
      require(!path.isEmpty(), "accepted profile file chooser needs an exact path");
      auto *buttons = file->findChild<QDialogButtonBox *>();
      const auto role = file->acceptMode() == QFileDialog::AcceptOpen
          ? QDialogButtonBox::Open : QDialogButtonBox::Save;
      auto *commit = buttons ? buttons->button(role) : nullptr;
      require(commit && commit->isEnabled(),
              "profile file chooser Open or Save action is unavailable");
      QTimer::singleShot(0, file, [commit] { commit->click(); });
    } else file->reject();
  }};
}
static DialogStep messageStep(QMessageBox::StandardButton answer,
                              const QString &reason = {}) {
  return {DialogStep::Message, "profile confirmation or error", [=](QDialog *dialog) {
    auto *message = qobject_cast<QMessageBox *>(dialog);
    require(!message->text().isEmpty() &&
                (reason.isEmpty() || message->text().contains(reason, Qt::CaseInsensitive)),
            "native error dialog did not explain the expected failure");
    auto *choice = message->button(answer);
    require(choice, "native message lacks the expected response");
    choice->click();
  }};
}
static void clickWithDialogs(WidthProfile::Manager &manager, const QString &label,
                             std::vector<DialogStep> steps,
                             const char *operation = nullptr) {
  DialogScript script(std::move(steps), operation);
  button(manager, label)->click();
  script.finish();
}
static void expectProfile(const WidthProfile::Profile &actual,
                          const WidthProfile::Profile &expected) {
  require(actual.name == expected.name && actual.points.size() == expected.points.size(),
          "native profile identity or point count changed");
  for (size_t i = 0; i < actual.points.size(); ++i)
    require(actual.points[i].pos == expected.points[i].pos &&
                actual.points[i].ratio == expected.points[i].ratio,
            "native profile changed authored samples");
}
static void expectDraft(WidthProfile::Manager &manager,
                        const std::vector<WidthProfile::Profile> &expected) {
  auto *list = manager.findChild<QListWidget *>("widthProfileList");
  require(list && list->count() == int(expected.size()), "native draft profile count differs");
  const int selected = list->currentRow();
  for (int i = 0; i < list->count(); ++i) {
    list->setCurrentRow(i);
    expectProfile(manager.selectedProfile(), expected[size_t(i)]);
  }
  list->setCurrentRow(selected);
}
static void expectSaved(const QString &path,
                        const std::vector<WidthProfile::Profile> &expected) {
  std::vector<WidthProfile::Profile> actual;
  QByteArray saved;
  QString error;
  require(WidthProfile::readLibrary(path, actual, saved, error) && actual.size() == expected.size(),
          "cannot reopen the native profile output");
  for (size_t i = 0; i < expected.size(); ++i) expectProfile(actual[i], expected[i]);
}

static void creationAndSaveChecks(const QTemporaryDir &temp) {
  std::cout << "width-profile-manager: actual New/capture, cancel, errors and save retry" << std::endl;
  const QString path = temp.filePath("created.txt");
  WidthProfile::Profile captured{"Captured pressure", {{0,.25},{.25,1},{1,.5}}};
  auto uniform = WidthProfile::builtIn(WidthProfile::UNIFORM);
  uniform.name = "Uniform copy";
  auto savedCapture = captured;
  savedCapture.name = "Saved pressure";
  WidthProfile::Manager manager(path, captured, true);
  QTimer::singleShot(0, [&] {
    clickWithDialogs(manager, "New…", {inputStep("Selected stroke", false)});
    expectDraft(manager, {});
    clickWithDialogs(manager, "New…", {inputStep("Selected stroke"), inputStep("Unused", false)});
    expectDraft(manager, {});
    clickWithDialogs(manager, "New…", {inputStep("Selected stroke"), inputStep("#invalid"),
        messageStep(QMessageBox::Ok, "Use a name")});
    expectDraft(manager, {});
    clickWithDialogs(manager, "New…", {inputStep("Selected stroke"), inputStep("Saved pressure")});
    expectDraft(manager, {savedCapture});
    QWidget *preview = nullptr;
    for (auto *widget : manager.findChildren<QWidget *>())
      if (widget->accessibleName() == "Width profile preview") preview = widget;
    require(preview, "native width-profile preview missing");
    const QImage capturedPixels = preview->grab().toImage();
    clickWithDialogs(manager, "New…", {inputStep("Uniform"), inputStep("Uniform copy")});
    expectDraft(manager, {savedCapture, uniform});
    require(!capturedPixels.isNull() && capturedPixels != preview->grab().toImage(),
            "New profile did not change the actual preview");
    require(!QFileInfo::exists(path), "draft creation wrote the library before Save");

    // Exercise the production capture entry used by the selected-stroke action,
    // including replacement cancellation, without changing the original samples.
    {
      DialogScript script({inputStep("Uniform copy"), messageStep(QMessageBox::Cancel)});
      manager.captureNewProfile();
      script.finish();
    }
    expectDraft(manager, {savedCapture, uniform});
    QLockFile otherWindow(path + ".lock");
    require(otherWindow.tryLock(0), "cannot hold the profile-library save lock");
    clickWithDialogs(manager, "Save Library", {messageStep(QMessageBox::Ok, "another window")});
    require(manager.isVisible() && !manager.shouldApply() && !QFileInfo::exists(path),
            "failed save closed the manager or published a partial library");
    expectDraft(manager, {savedCapture, uniform});
    otherWindow.unlock();
    button(manager, "Save Library")->click();
  });
  require(manager.exec() == QDialog::Accepted && !manager.shouldApply(),
          "Save Library retry did not complete without applying artwork");
  expectSaved(path, {savedCapture, uniform});
  expectProfile(captured, WidthProfile::Profile{"Captured pressure", {{0,.25},{.25,1},{1,.5}}});
  WidthProfile::Manager reopened(path, captured, false);
  expectDraft(reopened, {savedCapture, uniform});
}

static void importChecks(const QTemporaryDir &temp) {
  std::cout << "width-profile-manager: actual import cancellation, collisions and malformed files" << std::endl;
  const QString path = temp.filePath("import-library.txt");
  const QString incomingPath = temp.filePath("incoming.txt");
  const QString badPath = temp.filePath("malformed-import.txt");
  const QString emptyPath = temp.filePath("empty-import.txt");
  const WidthProfile::Profile a{"A", {{0,1},{1,1}}}, b{"B", {{0,0},{1,1}}};
  const WidthProfile::Profile incomingA{"A", {{0,.5},{1,.5}}};
  const WidthProfile::Profile incomingB{"B", {{0,.25},{1,.25}}};
  const WidthProfile::Profile added{"C", {{0,0},{.5,1},{1,0}}};
  QString error;
  require(WidthProfile::writeLibrary(path, {a,b}, error) &&
              WidthProfile::writeLibrary(incomingPath, {added,incomingA,incomingB}, error),
          "cannot prepare import fixtures");
  { QFile file(badPath); require(file.open(QIODevice::WriteOnly), "cannot prepare malformed import");
    file.write("Valid\t0:1,1:1\nBroken\t0:junk\n"); }
  { QFile file(emptyPath); require(file.open(QIODevice::WriteOnly), "cannot prepare empty import");
    file.write("# No profiles\n"); }
  const QByteArray original = bytes(path), incomingBytes = bytes(incomingPath);
  {
    WidthProfile::Manager manager(path, {}, false);
    QTimer::singleShot(0, [&] {
      clickWithDialogs(manager, "Import…", {fileStep(incomingPath, false)});
      expectDraft(manager, {a,b});
      // A new entry and the first accepted replacement must also roll back
      // when the second replacement is canceled.
      clickWithDialogs(manager, "Import…", {fileStep(incomingPath),
          messageStep(QMessageBox::Yes, "“A”"), messageStep(QMessageBox::Cancel, "“B”")});
      expectDraft(manager, {a,b});
      clickWithDialogs(manager, "Import…", {fileStep(badPath), messageStep(QMessageBox::Ok, "line 2")});
      expectDraft(manager, {a,b});
      clickWithDialogs(manager, "Import…", {fileStep(emptyPath), messageStep(QMessageBox::Ok, "no profiles")});
      expectDraft(manager, {a,b});
      require(bytes(path) == original, "canceled or failed import wrote the library");
      clickWithDialogs(manager, "Import…", {fileStep(incomingPath),
          messageStep(QMessageBox::Yes, "“A”"), messageStep(QMessageBox::Yes, "“B”")});
      expectDraft(manager, {incomingA,incomingB,added});
      manager.reject();
    });
    require(manager.exec() == QDialog::Rejected && bytes(path) == original,
            "canceling the imported draft changed library bytes");
  }
  {
    WidthProfile::Manager manager(path, {}, false);
    QTimer::singleShot(0, [&] {
      clickWithDialogs(manager, "Import…", {fileStep(incomingPath),
          messageStep(QMessageBox::Yes), messageStep(QMessageBox::Yes)});
      button(manager, "Save Library")->click();
    });
    require(manager.exec() == QDialog::Accepted, "accepted import did not save");
  }
  expectSaved(path, {incomingA,incomingB,added});
  require(bytes(incomingPath) == incomingBytes, "import changed its source file");
  WidthProfile::Manager reopened(path, {}, false);
  expectDraft(reopened, {incomingA,incomingB,added});
}

static void exportChecks(const QTemporaryDir &temp) {
  std::cout << "width-profile-manager: actual export, overwrite, write failure and retry" << std::endl;
  const QString path = temp.filePath("export-library.txt");
  const QString output = temp.filePath("exported.txt");
  const QString canceledOutput = temp.filePath("canceled-export.txt");
  const QString blockedParent = temp.filePath("export-parent");
  const QString blockedOutput = QDir(blockedParent).filePath("failed.txt");
  const WidthProfile::Profile first{"First", {{0,1},{1,1}}};
  const WidthProfile::Profile selected{"Selected", {{0,.25},{.5,1},{1,.5}}};
  QString error;
  require(WidthProfile::writeLibrary(path, {first,selected}, error), "cannot prepare export library");
  { QFile file(output); require(file.open(QIODevice::WriteOnly), "cannot prepare overwrite target");
    file.write("Preserve this existing file until overwrite is confirmed.\n"); }
  const QByteArray original = bytes(path), oldOutput = bytes(output);
  WidthProfile::Manager manager(path, {}, false);
  QTimer::singleShot(0, [&] {
    manager.findChild<QListWidget *>("widthProfileList")->setCurrentRow(1);
    clickWithDialogs(manager, "Export…", {fileStep(canceledOutput, false)},
                     "export: cancel new destination");
    require(!QFileInfo::exists(canceledOutput), "canceled export created a file");
    clickWithDialogs(manager, "Export…", {fileStep(output), messageStep(QMessageBox::No), fileStep({}, false)},
                     "export: decline overwrite then cancel chooser");
    require(bytes(output) == oldOutput, "declining overwrite changed the destination");
    clickWithDialogs(manager, "Export…", {fileStep(output), messageStep(QMessageBox::Yes)},
                     "export: confirm overwrite");
    expectSaved(output, {selected});
    clickWithDialogs(manager, "Export…", {fileStep(path), messageStep(QMessageBox::Yes),
        messageStep(QMessageBox::Ok, "Use Save Library")},
                     "export: reject overwriting the manager library");
    require(bytes(path) == original, "Export overwrote the manager's own library");

    require(QDir().mkpath(blockedParent), "cannot prepare export-failure directory");
    // Make the accepted destination a nonempty directory only after the real
    // chooser accepts a valid path. This reaches the writer error on all three
    // platforms, without depending on permission bits or deleting a directory
    // currently watched by the chooser.
    clickWithDialogs(manager, "Export…", {fileStep(blockedOutput, true, [&] {
      require(QDir().mkpath(blockedOutput), "cannot block the export destination");
      QFile blocker(QDir(blockedOutput).filePath("keep.txt"));
      require(blocker.open(QIODevice::WriteOnly), "cannot block the export parent");
      blocker.write("Unrelated existing data\n");
    }), messageStep(QMessageBox::Ok)}, "export: accepted destination writer failure");
    require(manager.isVisible() && QFileInfo(blockedOutput).isDir() &&
                bytes(QDir(blockedOutput).filePath("keep.txt")) == "Unrelated existing data\n",
            "failed export closed the manager or damaged existing data");
    expectDraft(manager, {first,selected});
    require(QFile::remove(QDir(blockedOutput).filePath("keep.txt")) && QDir().rmdir(blockedOutput),
            "cannot restore the export destination for retry");
    clickWithDialogs(manager, "Export…", {fileStep(blockedOutput)},
                     "export: retry restored destination");
    expectSaved(blockedOutput, {selected});
    require(bytes(path) == original, "export or export retry changed the library");
    manager.reject();
  });
  require(manager.exec() == QDialog::Rejected && bytes(path) == original,
          "closing after export changed the library");
  expectSaved(output, {selected});
}

int main(int argc, char **argv) {
  // Keep the platform's real Qt file chooser and overwrite path observable.
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  std::cout << "width-profile-manager: qt=" << qVersion()
            << " platform=" << QApplication::platformName().toStdString()
            << " native-interface=" << bool(QApplication::platformNativeInterface()) << std::endl;
  QTemporaryDir temp;
  require(temp.isValid(), "temporary directory");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.filePath("settings"));
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
        std::cout << "width-profile-manager: delete confirmation driver entered" << std::endl;
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        require(dialog, "delete asks confirmation");
        auto *confirm = dialog->button(QMessageBox::Yes);
        require(confirm, "delete confirmation exposes its Yes action");
        std::cout << "width-profile-manager: accepting delete confirmation" << std::endl;
        confirm->click();
      });
      std::cout << "width-profile-manager: opening delete confirmation" << std::endl;
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
  creationAndSaveChecks(temp);
  importChecks(temp);
  exportChecks(temp);
  std::cout << "Native width-profile draft, save, cancel and storage checks passed\n";
}
