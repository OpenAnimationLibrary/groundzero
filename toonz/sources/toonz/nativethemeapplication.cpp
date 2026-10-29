#include "themeapplication.h"

#include "tenv.h"
#include "tfilepath.h"
#include "toonz/preferences.h"
#include "toonzqt/gutil.h"

ThemeApplication &NativeTheme::instance() {
  static ThemeApplication themes(
      (TEnv::getConfigDir() + TFilePath("qss")).getQString(),
      {[]() { return Preferences::instance()->getStringValue(CurrentStyleSheetName); },
       []() { return Preferences::instance()->getAdditionalStyleSheet(); },
       [](const QString &name) { Preferences::instance()->setValue(CurrentStyleSheetName, name); },
       [](const QString &style) { Preferences::instance()->setValue(additionalStyleSheet, style); },
       [](const QString &style) {
         ThemeManager::getInstance().parseCustomPropertiesFromStylesheet(style);
       }});
  return themes;
}
