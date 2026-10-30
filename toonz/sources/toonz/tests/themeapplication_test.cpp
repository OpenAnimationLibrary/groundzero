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

void checkpoint(const char *message) {
  // Flush before entering Qt so a platform crash identifies the last operation
  // in CTest's failure report even when no exception reaches the test harness.
  std::cerr << "themeapplication: " << message << std::endl;
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
#ifdef Q_OS_WIN
  // Qt 5.15.2's QMessageBox::showEvent reads the Windows system menu through
  // platformNativeInterface(), which the offscreen plugin does not provide.
  // Exercise the real warning dialog on the same platform as the application.
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("windows"));
#else
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
#endif
  QApplication app(argc, argv);
  try {
    checkpoint("creating isolated themes and widgets");
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
    checkpoint("applying initial fixture theme");
    require(themes.reload(&error), "initial theme could not be loaded");
    require(error.isEmpty(), "successful application left an error");
    app.processEvents();
    require(mainLabel.palette().color(QPalette::WindowText) == QColor("#dddddd"),
            "main widget did not receive the theme");
    require(customLabel.palette().color(QPalette::WindowText) == QColor("#7f1248"),
            "additional stylesheet lost precedence");
    require(!themes.availableThemes().contains("NotATheme"), "non-theme directory was listed");

    checkpoint("creating synchronized menus and Preferences");
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
    checkpoint("switching fixture themes from menus and Preferences");
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
    checkpoint("rejecting invalid themes without mutation");
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
    checkpoint("opening invalid-theme warning and dismissing it");
    themeAction(*first, "Broken")->trigger();
    checkpoint("invalid-theme warning returned");
    requireChecked(*first, "Dark");
    requireChecked(*second, "Dark");
    require(themes.currentTheme() == "Dark" && app.styleSheet() == before,
            "failed menu choice changed the current theme");
    checkpoint("applying additional stylesheet overrides");
    require(!themes.applyAdditionalStyle("QLabel { color: 'broken", &error),
            "unterminated override accepted");
    require(extraSaves == 0 && app.styleSheet() == before,
            "failed override changed settings or appearance");
    require(themes.applyAdditionalStyle("QLabel#custom { color: #123456; }", &error),
            "valid override rejected");
    app.processEvents();
    require(extraSaves == 1 && customLabel.palette().color(QPalette::WindowText) == QColor("#123456"),
            "valid override was not applied and saved");

    checkpoint("discovering a newly installed theme");
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

    checkpoint("reloading edited stylesheet and clearing overrides");
    write(root + "/New/New.qss", "QWidget { color: #884433; }");
    require(themes.reload(&error), "edited stylesheet did not reload");
    app.processEvents();
    require(floatingLabel.palette().color(QPalette::WindowText) == QColor("#884433"),
            "Reload QSS did not refresh floating widgets");
    require(themes.applyAdditionalStyle("/* only a comment */", &error),
            "comment-only override rejected");
    require(themes.applyAdditionalStyle("", &error), "clearing overrides failed");

    checkpoint("restoring persisted theme in another application helper");
    QSettings reopened(settingsPath, QSettings::IniFormat);
    require(reopened.value("theme").toString() == "New" &&
            reopened.value("extra").toString().isEmpty(), "settings did not survive reopening");
    ThemeApplication restored(root, adapter);
    std::unique_ptr<QMenu> restoredMenu(createQuickThemeMenu(restored, nullptr));
    requireChecked(*restoredMenu, "New");
    require(restored.reload(&error), "saved theme did not restore");

    checkpoint(
        "falling back at startup from saved styles that fail the checks");
    write(root + "/Default/Default.qss", "QWidget { color: #445566; }");
    QSettings startupSettings(root + "/startup.ini", QSettings::IniFormat);
    int startupSaves = 0;
    ThemeApplication::Settings startupAdapter{
        [&]() { return startupSettings.value("theme").toString(); },
        [&]() { return startupSettings.value("extra").toString(); },
        [&](const QString &name) {
          ++startupSaves;
          startupSettings.setValue("theme", name);
        },
        [&](const QString &style) {
          ++startupSaves;
          startupSettings.setValue("extra", style);
        },
        [](const QString &) {}};
    {
      // A theme with a missing image: the Default theme keeps the overrides.
      startupSettings.setValue("theme", "MissingAsset");
      startupSettings.setValue("extra", "QLabel#custom { color: #7f1248; }");
      ThemeApplication startup(root, startupAdapter);
      require(startup.startup(&error) && !error.isEmpty(),
              "startup left a failing theme unstyled or unreported");
      app.processEvents();
      require(
          floatingLabel.palette().color(QPalette::WindowText) ==
                  QColor("#445566") &&
              customLabel.palette().color(QPalette::WindowText) ==
                  QColor("#7f1248"),
          "startup did not fall back to the Default theme with the overrides");
      require(startupSaves == 0 && startup.currentTheme() == "MissingAsset",
              "startup fallback replaced the saved theme");
      const QString fallbackStyle = app.styleSheet();
      require(!startup.applyTheme("MissingAsset", &error) &&
                  !startup.reload(&error) &&
                  app.styleSheet() == fallbackStyle && startupSaves == 0,
              "an interactive change accepted a theme that fails the checks");
    }
    {
      // A failing additional sheet: the saved theme is kept without it.
      startupSettings.setValue("theme", "Light");
      startupSettings.setValue("extra", "QLabel { color: 'broken");
      ThemeApplication startup(root, startupAdapter);
      require(startup.startup(&error) && !error.isEmpty(),
              "startup left a failing additional sheet unstyled or unreported");
      app.processEvents();
      require(floatingLabel.palette().color(QPalette::WindowText) ==
                  QColor("#222222"),
              "startup did not keep the saved theme without the failing sheet");
      require(startupSaves == 0 && startupSettings.value("extra").toString() ==
                                       "QLabel { color: 'broken",
              "startup fallback replaced the saved additional sheet");
    }

    // CI also supplies the real installed-theme tree. This exercises its QSS
    // and inherited image resources rather than relying only on tiny fixtures.
    if (argc > 1) {
      checkpoint("loading shipped theme resources");
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
      checkpoint("applying installed Default theme");
      require(installed.reload(&error), "default installed theme failed to load");
      for (const QString &name : shipped) {
        std::cerr << "themeapplication: applying installed "
                  << name.toStdString() << std::endl;
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
      checkpoint("restoring installed Default theme");
      require(installed.applyTheme("Default", &error), "original installed theme failed to restore");
      requireChecked(*roomA, "Default");
      requireChecked(*roomB, "Default");
      checkpoint("destroying installed-theme controls");
    }
    checkpoint("destroying fixture controls");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  checkpoint("fixture cleanup complete");
  return 0;
}
