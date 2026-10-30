#include "languagemanager.h"
#include "toonzqt/uitranslations.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTranslator>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <iostream>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (argc != 2 && argc != 3) {
    std::cerr << "Expected the compiled fixture catalog directory\n";
    return 1;
  }
  UiLanguageManager manager;
  manager.setCatalogDirectory(QString::fromLocal8Bit(argv[1]));
  QString error;
  bool ok = manager.setLanguage("English", &error);
  int committed = 0;
  QObject::connect(&manager, &UiLanguageManager::languageChanged,
                   [&]() { ++committed; });
  QWidget window;
  QLabel label(QCoreApplication::translate("Fixture", "Open"), &window);
  QLabel formatted(UiTranslations::text(
      QCoreApplication::translate("Fixture", "Frame %1")).arg(37), &window);
  QLineEdit edited(QStringLiteral("Unsaved name"), &window);
  edited.setCursorPosition(4);
  const QStringList languages{"FixtureA", "FixtureB", "English", "FixtureA"};
  const QStringList labels{QString::fromUtf8("開く"), QString::fromUtf8("Ouvrir"),
                           "Open", QString::fromUtf8("開く")};
  const QStringList frames{QString::fromUtf8("フレーム 37"), "Image 37",
                           "Frame 37", QString::fromUtf8("フレーム 37")};
  for (int i = 0; i < languages.size(); ++i) {
    if (!manager.setLanguage(languages[i], &error)) {
      std::cerr << error.toStdString() << '\n';
      ok = false;
      continue;
    }
    ok &= label.text() == labels[i];
    ok &= formatted.text() == frames[i];
    ok &= edited.text() == "Unsaved name" && edited.cursorPosition() == 4;
  }
  ok &= committed == languages.size();
  const QString kept = label.text();
  ok &= !manager.setLanguage("Missing", &error) && !error.isEmpty();
  ok &= manager.currentLanguage() == "FixtureA" && label.text() == kept;
  ok &= committed == languages.size();
  QTemporaryDir incomplete;
  QDir(incomplete.path()).mkdir("Broken");
  QFile bad(incomplete.path() + "/Broken/toonz.qm");
  if (bad.open(QIODevice::WriteOnly)) bad.write("invalid catalog");
  bad.close();
  manager.setCatalogDirectory(incomplete.path());
  ok &= !manager.setLanguage("Broken", &error);
  ok &= manager.currentLanguage() == "FixtureA" && label.text() == kept;
  ok &= !manager.setLanguage("../FixtureB", &error);
  ok &= manager.setLanguage("English", &error);
  ok &= label.text() == "Open" && formatted.text() == "Frame 37";

  auto expect = [&](bool condition, const char *message) {
    if (!condition) std::cerr << message << '\n';
    ok &= condition;
  };
  int selected = 0;
  QObject::connect(&manager, &UiLanguageManager::languageSelected,
                   [&]() { ++selected; });
  const QString japanese = QString::fromUtf8("開く");
  const QString fixtureCatalog =
      QDir(QString::fromLocal8Bit(argv[1])).filePath("FixtureA/toonz.qm");
  QDir(incomplete.path()).mkdir("Partial");
  QFile::copy(fixtureCatalog, incomplete.path() + "/Partial/toonz.qm");
  expect(manager.setLanguage("Partial", &error) && label.text() == japanese &&
             selected == 1,
         "A language without optional module catalogs was rejected");
  QDir(incomplete.path()).mkdir("Damaged");
  QFile::copy(fixtureCatalog, incomplete.path() + "/Damaged/toonz.qm");
  QFile damaged(incomplete.path() + "/Damaged/toonzqt.qm");
  if (damaged.open(QIODevice::WriteOnly)) damaged.write("invalid catalog");
  damaged.close();
  expect(!manager.setLanguage("Damaged", &error) &&
             error.contains("current language was kept") &&
             manager.currentLanguage() == "Partial" && selected == 1,
         "A damaged module catalog was accepted or changed the language");
  // At startup a saved language that cannot be loaded runs the session in
  // English, says so, and is not reported for saving.
  expect(!manager.startWithLanguage("Broken", &error) &&
             manager.currentLanguage() == "English" && label.text() == "Open",
         "Startup did not fall back to English");
  expect(!error.isEmpty() && !error.contains("kept") && selected == 1,
         "Startup fallback replaced the saved language or misreported it");
  expect(manager.startWithLanguage("Partial", &error) && error.isEmpty() &&
             label.text() == japanese && selected == 1,
         "Startup language was not applied, or was reported for saving");
  expect(manager.setLanguage("English", &error) && selected == 2 &&
             manager.setLanguage("English", &error) && selected == 3,
         "A choice of the user was not reported for saving");
  if (argc == 3) {
    manager.setCatalogDirectory(QString::fromLocal8Bit(argv[2]));
    const QStringList installed = manager.availableLanguages();
    ok &= installed.size() == 11;  // English and all ten packaged languages.
    for (const QString &language : installed) {
      if (!manager.setLanguage(language, &error)) {
        std::cerr << "Packaged catalog failed: " << language.toStdString()
                  << ": " << error.toStdString() << '\n';
        ok = false;
      }
      if (language != "English") {
        QTranslator qtCatalog;
        ok &= qtCatalog.load(QDir(manager.catalogDirectory())
            .filePath(language + "/qtbase.qm"));
      }
      ok &= manager.currentLanguage() == language;
      ok &= edited.text() == "Unsaved name" && edited.cursorPosition() == 4;
    }
    ok &= manager.setLanguage("English", &error);
    ok &= formatted.text() == "Frame 37";
  }
  if (!ok) std::cerr << "Language catalog, rollback or UI-state regression\n";
  return ok ? 0 : 1;
}
