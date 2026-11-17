#include "automation.h"

#include "tenv.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

using GroundzeroAutomation::Context;
using GroundzeroAutomation::Error;

struct RestoreDirectory final {
  QString previous = QDir::currentPath();
  ~RestoreDirectory() {
    if (!QDir::setCurrent(previous)) {
      std::cerr << "FAIL: cannot restore fixture working directory\n";
      std::abort();
    }
  }
};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void initialize(const QString& root) {
  require(QDir().mkpath(root + "/portablestuff") && QDir::setCurrent(root),
          "Cannot select private native settings");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root);
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, root);
  TEnv::setRootVarName("TOONZROOT");
  TEnv::setSystemVarPrefix("TOONZ");
  require(TEnv::setArgPathValue("TOONZROOT", root.toStdString()),
          "Cannot select private native root");
  for (const auto& entry : TEnv::getSystemPathMap()) {
    const QString path =
        QDir(root).filePath(QString::fromStdString(entry.second));
    require(QDir().mkpath(path),
            "Cannot create private configuration directory");
    require(TEnv::setArgPathValue("TOONZ" + entry.first, path.toStdString()),
            "Cannot select private configuration directory");
  }
}

QString directory(const QString& root, const QString& name) {
  const QString path = QDir(root).filePath(name);
  require(QDir().mkpath(path), "Cannot create path fixture directory");
  return QFileInfo(path).canonicalFilePath();
}

Error rejection(Context& context, const QString& path, bool write) {
  try {
    context.resolvePath(path, write);
  } catch (const Error& error) {
    return error;
  }
  throw std::runtime_error("Path outside authority was accepted");
}

void unchangedAdmission(Context& context, const QString& input) {
  const QString expected = context.workspace() + "/new/future.pli";
  require(context.resolvePath("new/future.pli", true) == expected,
          "Missing workspace descendants stopped working");
  require(context.resolvePath(expected, false) == expected,
          "Absolute workspace paths stopped working");
  require(context.resolvePath(input + "/source.png", false) ==
              input + "/source.png",
          "Explicit input roots stopped working");
  require(rejection(context, input + "/source.png", true).code == "path_denied",
          "Input roots acquired write authority");
  require(rejection(context, "../foreign/file", false).code == "invalid_path",
          "Parent traversal changed admission");
  require(
      rejection(context, context.workspace() + "-sibling/file", false).code ==
          "path_denied",
      "A sibling prefix acquired authority");
}

void foreignObservation(Context& context, const QString& foreign) {
  const auto original = rejection(context, foreign + "/missing", false);
  require(original.code == "path_denied" &&
              original.message == "The path is outside the allowed roots.",
          "Foreign paths did not fail lexical authority first");
#ifndef _WIN32
  const QString link = foreign + "/broken-link";
  require(QFile::link(foreign + "/absent-target", link),
          "Cannot create foreign link fixture");
  const auto linked = rejection(context, link, false);
  require(linked.code == original.code && linked.message == original.message,
          "Foreign link metadata affected rejection before authority");
#endif
}

void workspaceLinks(Context& context, const QString& foreign) {
#ifndef _WIN32
  const QString link = context.workspace() + "/escape";
  require(QFile::link(foreign, link), "Cannot create workspace link fixture");
  require(rejection(context, link + "/new-file", false).code == "path_denied",
          "An authorized lexical path followed a foreign directory link");
  const QString broken = context.workspace() + "/broken";
  require(QFile::link(context.workspace() + "/absent", broken),
          "Cannot create dangling workspace link");
  require(rejection(context, broken, true).code == "path_denied",
          "A dangling workspace link acquired write authority");
#else
  Q_UNUSED(context);
  Q_UNUSED(foreign);
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir temporary;
    RestoreDirectory restore;
    require(temporary.isValid(), "Cannot create private path fixture");
    initialize(temporary.path());
    const QString workspace = directory(temporary.path(), "workspace");
    const QString input     = directory(temporary.path(), "input");
    const QString foreign   = directory(temporary.path(), "foreign");
    Context context(workspace, {input});
    unchangedAdmission(context, input);
    foreignObservation(context, foreign);
    workspaceLinks(context, foreign);
    std::cout << "PASS: worker path authority boundaries\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL path authority: " << error.what() << '\n';
    return 1;
  }
}
