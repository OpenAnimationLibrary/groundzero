#include "themeapplication.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void write(const QString &path, const QByteArray &contents) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "could not write theme fixture");
  require(file.write(contents) == contents.size(), "short fixture write");
}

QAction *themeAction(QMenu &menu, const QString &name) {
  for (QAction *action : menu.actions())
    if (action->data().toString() == name) return action;
  throw std::runtime_error("installed theme action missing");
}

void requireChecked(QMenu &menu, const QString &expected) {
  int checked = 0;
  for (QAction *action : menu.actions()) {
    if (!action->isChecked()) continue;
    ++checked;
    require(action->data().toString() == expected, "menu checked the wrong theme");
  }
  require(checked == 1, "menu did not have one exclusive checked theme");
}
}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication app(argc, argv);
  try {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary theme directory unavailable");
    const QString root = directory.path();
    write(root + "/Dark/Dark.qss", "QWidget { color: #dddddd; background: #222222; }\n"
          "/* url('not-an-asset.png') is a comment. */");
    write(root + "/Light/Light.qss", "QWidget { color: #222222; background: #eeeeee; }");
    write(root + "/Broken/Broken.qss", "QWidget { color: #ffffff;");
    write(root + "/MissingAsset/MissingAsset.qss", "QWidget { image: url('missing.svg'); }");
    QDir().mkpath(root + "/NotATheme");

    const QString settingsPath = root + "/settings.ini";
    QSettings settings(settingsPath, QSettings::IniFormat);
    settings.setValue("theme", "Dark");
    settings.setValue("extra", "QLabel#custom { color: #7f1248; }");
    int saves = 0, extraSaves = 0, iconRefreshes = 0, notifications = 0;
    ThemeApplication::Settings adapter{
        [&]() { return settings.value("theme").toString(); },
        [&]() { return settings.value("extra").toString(); },
        [&](const QString &name) { ++saves; settings.setValue("theme", name); settings.sync(); },
        [&](const QString &style) { ++extraSaves; settings.setValue("extra", style); settings.sync(); },
        [&](const QString &) { ++iconRefreshes; }};
    ThemeApplication themes(root, adapter);
    QObject::connect(&themes, &ThemeApplication::themeChanged,
                     [&]() { ++notifications; });

    QLabel mainLabel("main"), floatingLabel("floating"), customLabel("custom");
    customLabel.setObjectName("custom");
    mainLabel.show(); floatingLabel.show(); customLabel.show();
    QString error;
    require(themes.reload(&error), "initial theme could not be loaded");
    require(error.isEmpty(), "successful application left an error");
    app.processEvents();
    require(mainLabel.palette().color(QPalette::WindowText) == QColor("#dddddd"),
            "main widget did not receive the theme");
    require(customLabel.palette().color(QPalette::WindowText) == QColor("#7f1248"),
            "additional stylesheet lost precedence");
    require(!themes.availableThemes().contains("NotATheme"), "non-theme directory was listed");

    std::unique_ptr<QMenu> first(createQuickThemeMenu(themes, nullptr));
    std::unique_ptr<QMenu> second(createQuickThemeMenu(themes, nullptr));
    QComboBox preferences;
    for (int i = 0; i < 5; ++i) themes.bindComboBox(&preferences);
    QObject::connect(&preferences, QOverload<int>::of(&QComboBox::currentIndexChanged),
        [&](int) {
          if (!themes.applyTheme(preferences.currentData().toString(), &error))
            themes.synchronizeComboBox(&preferences);
        });
    requireChecked(*first, "Dark");
    requireChecked(*second, "Dark");

    const int initialRefreshes = iconRefreshes;
    const int initialNotifications = notifications;
    themeAction(*first, "Light")->trigger();
    app.processEvents();
    require(themes.currentTheme() == "Light", "menu did not persist selection");
    require(saves == 1 && iconRefreshes == initialRefreshes + 1 &&
            notifications == initialNotifications + 1, "one choice applied more than once");
    requireChecked(*first, "Light");
    requireChecked(*second, "Light");
    require(preferences.currentData().toString() == "Light", "Preferences was not synchronized");
    require(floatingLabel.palette().color(QPalette::WindowText) == QColor("#222222"),
            "floating widget did not refresh");
    require(customLabel.palette().color(QPalette::WindowText) == QColor("#7f1248"),
            "switching themes lost user overrides");
    themeAction(*second, "Light")->trigger();
    require(saves == 1 && iconRefreshes == initialRefreshes + 1,
            "already selected theme reapplied unnecessarily");

    preferences.setCurrentIndex(preferences.findData("Dark"));
    requireChecked(*first, "Dark");
    requireChecked(*second, "Dark");
    const QString before = app.styleSheet();
    const int beforeRefreshes = iconRefreshes, beforeSaves = saves;
    for (const QString &bad : {QString("Gone"), QString("../Dark"), QString("Broken"),
                              QString("MissingAsset")}) {
      require(!themes.applyTheme(bad, &error) && !error.isEmpty(), "invalid theme accepted");
      require(themes.currentTheme() == "Dark" && app.styleSheet() == before,
              "failed theme changed the live selection or stylesheet");
    }
    require(iconRefreshes == beforeRefreshes && saves == beforeSaves,
            "failed theme caused side effects");
    preferences.setCurrentIndex(preferences.findData("Broken"));
    require(preferences.currentData().toString() == "Dark",
            "failed Preferences choice was not restored");
    QTimer::singleShot(0, []() {
      for (QWidget *window : QApplication::topLevelWidgets())
        if (QMessageBox *message = qobject_cast<QMessageBox *>(window)) message->reject();
    });
    themeAction(*first, "Broken")->trigger();
    requireChecked(*first, "Dark");
    requireChecked(*second, "Dark");
    require(themes.currentTheme() == "Dark" && app.styleSheet() == before,
            "failed menu choice changed the current theme");
    require(!themes.applyAdditionalStyle("QLabel { color: 'broken", &error),
            "unterminated override accepted");
    require(extraSaves == 0 && app.styleSheet() == before,
            "failed override changed settings or appearance");
    require(themes.applyAdditionalStyle("QLabel#custom { color: #123456; }", &error),
            "valid override rejected");
    app.processEvents();
    require(extraSaves == 1 && customLabel.palette().color(QPalette::WindowText) == QColor("#123456"),
            "valid override was not applied and saved");

    write(root + "/New/New.qss", "QWidget { color: #338844; }");
    QMetaObject::invokeMethod(first.get(), "aboutToShow", Qt::DirectConnection);
    require(themeAction(*first, "New"), "newly installed theme not discovered");
    themes.bindComboBox(&preferences);
    require(preferences.findData("New") >= 0, "Preferences missed newly installed theme");
    themeAction(*first, "New")->trigger();
    QMetaObject::invokeMethod(second.get(), "aboutToShow", Qt::DirectConnection);
    requireChecked(*second, "New");
    const int actionCount = first->actions().size();
    for (int i = 0; i < 5; ++i)
      QMetaObject::invokeMethod(first.get(), "aboutToShow", Qt::DirectConnection);
    require(first->actions().size() == actionCount, "menu accumulated duplicate actions");

    write(root + "/New/New.qss", "QWidget { color: #884433; }");
    require(themes.reload(&error), "edited stylesheet did not reload");
    app.processEvents();
    require(floatingLabel.palette().color(QPalette::WindowText) == QColor("#884433"),
            "Reload QSS did not refresh floating widgets");
    require(themes.applyAdditionalStyle("/* only a comment */", &error),
            "comment-only override rejected");
    require(themes.applyAdditionalStyle("", &error), "clearing overrides failed");

    QSettings reopened(settingsPath, QSettings::IniFormat);
    require(reopened.value("theme").toString() == "New" &&
            reopened.value("extra").toString().isEmpty(), "settings did not survive reopening");
    ThemeApplication restored(root, adapter);
    std::unique_ptr<QMenu> restoredMenu(createQuickThemeMenu(restored, nullptr));
    requireChecked(*restoredMenu, "New");
    require(restored.reload(&error), "saved theme did not restore");

    // CI also supplies the real installed-theme tree. This exercises its QSS
    // and inherited image resources rather than relying only on tiny fixtures.
    if (argc > 1) {
      const QString installedRoot = QString::fromLocal8Bit(argv[1]);
      QString selected = QStringLiteral("Default");
      const QString overrideStyle = QStringLiteral("QLabel#custom { color: #124578; }");
      int overrideWrites = 0;
      ThemeApplication installed(installedRoot,
          {[&]() { return selected; },
           [&]() { return overrideStyle; },
           [&](const QString &name) { selected = name; },
           [&](const QString &) { ++overrideWrites; },
           [](const QString &) {}});
      const QStringList shipped = installed.availableThemes();
      for (const QString &required : {QString("Animate"), QString("Blue"),
           QString("Clay"), QString("Dark"), QString("Darker"),
           QString("Default-Green"), QString("Default"), QString("Light"),
           QString("Neutral"), QString("Premiere"), QString("ToonBoom"),
           QString("Synthwave")})
        require(shipped.contains(required), "a shipped theme is missing its QSS asset");
      std::unique_ptr<QMenu> roomA(createQuickThemeMenu(installed, nullptr));
      std::unique_ptr<QMenu> roomB(createQuickThemeMenu(installed, nullptr));
      QComboBox nativePreferences;
      installed.bindComboBox(&nativePreferences);
      require(installed.reload(&error), "default installed theme failed to load");
      for (const QString &name : shipped) {
        require(installed.applyTheme(name, &error), "shipped theme or referenced image failed to load");
        app.processEvents();
        requireChecked(*roomA, name);
        requireChecked(*roomB, name);
        require(nativePreferences.currentData().toString() == name,
                "installed theme did not synchronize Preferences");
        require(floatingLabel.isVisible() && floatingLabel.text() == "floating" &&
                customLabel.text() == "custom", "theme switch changed an existing window's contents");
        require(customLabel.palette().color(QPalette::WindowText) == QColor("#124578"),
                "shipped theme replaced the custom override");
        require(overrideWrites == 0, "theme switch rewrote the user stylesheet");
      }
      require(installed.applyTheme("Default", &error), "original installed theme failed to restore");
      requireChecked(*roomA, "Default");
      requireChecked(*roomB, "Default");
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
