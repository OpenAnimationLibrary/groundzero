#include "externalapplications.h"

#include "toonz/preferences.h"
#include <QCoreApplication>

namespace {
PreferencesItemId pathPreference(
    ExternalApplications::Application application) {
  return application == ExternalApplications::Ztoryc ? ztorycExecutablePath
                                                     : ot3dExecutablePath;
}
}  // namespace

ExternalApplications &NativeExternalApplications::instance() {
  static ExternalApplications applications(
      QCoreApplication::applicationDirPath(),
      QCoreApplication::applicationFilePath(),
      {[](ExternalApplications::Application application) {
         return Preferences::instance()->getStringValue(
             pathPreference(application));
       },
       [](ExternalApplications::Application application, const QString &path) {
         Preferences::instance()->setValue(pathPreference(application), path);
       }});
  return applications;
}
