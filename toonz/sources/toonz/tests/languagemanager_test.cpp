#include "languagemanager.h"
#include "toonzqt/uitranslations.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <iostream>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (argc != 2) {
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
  if (!ok) std::cerr << "Language catalog, rollback or UI-state regression\n";
  return ok ? 0 : 1;
}
