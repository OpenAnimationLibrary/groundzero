#include "toonzqt/gutil.h"

#include <QApplication>
#include <iostream>

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication app(argc, argv);
  ThemeManager &themes = ThemeManager::getInstance();
  themes.parseCustomPropertiesFromStylesheet(
      ":TOONZCOLORS { -icon-base-color: #112233; -icon-close-color: #aa0000; "
      "-icon-base-opacity: 0.6; }\n"
      ":TOONZCOLORS { -icon-base-color: #445566; }");
  if (themes.getIconBaseColor() != QColor("#445566") ||
      themes.getIconCloseColor() != QColor("#aa0000") ||
      themes.getIconBaseOpacity() != 0.6) {
    std::cerr << "additional icon properties did not override the base theme\n";
    return 1;
  }
  themes.parseCustomPropertiesFromStylesheet(
      ":TOONZCOLORS { -icon-base-color: #778899; }");
  if (themes.getIconBaseColor() != QColor("#778899") ||
      themes.getIconCloseColor().isValid() || themes.getIconBaseOpacity() != 0.8 ||
      themes.getCustomProperty("omitted-property", "fallback") != "fallback") {
    std::cerr << "omitted theme properties carried over instead of resetting\n";
    return 1;
  }
  return 0;
}
