// An external-program effect starts a program only when external programs are
// allowed, and only the program of a declaration installed next to the
// application. A scene names the declaration; a program path or arguments
// stored in the scene are never started.

#include "texternfx.h"
#include "tconvert.h"
#include "texception.h"
#include "tstream.h"
#include "tsystem.h"
#include "ttile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUuid>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// The harmless program: this test binary, which only creates a marker file.
int worker(const QStringList& arguments) {
  if (arguments.size() != 3 || arguments[1] != "--marker") return 20;
  QFile marker(arguments[2]);
  return marker.open(QIODevice::WriteOnly) ? 0 : 21;
}

std::string markerArguments(const QString& marker) {
  return "--marker \"" + marker.toStdString() + "\"";
}

std::string uniqueName() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

// Computes one frame and returns its error, empty when the frame succeeded.
std::string compute(TExternalProgramFx& effect) {
  TRaster32P raster(1, 1);
  TTile tile(raster);
  try {
    effect.doCompute(tile, 0, TRenderSettings());
  } catch (const TException& error) {
    return QString::fromStdWString(error.getMessage()).toStdString();
  }
  return std::string();
}

// Writes an external-effect node as a scene stores it, then loads it.
void loadNode(TExternalProgramFx& effect, const QString& file,
              const std::string& name, const QString& program,
              const std::string& arguments) {
  const TFilePath path(file);
  {
    TOStream stream(path);
    stream.child("name") << name;
    stream.child("path") << TFilePath(program);
    stream.child("args") << arguments;
    stream.openChild("ports");
    stream.openCloseChild("outport", {{"name", "out"}, {"ext", "png"}});
    stream.closeChild();
  }
  TIStream stream(path);
  effect.loadData(stream);
}

// A declaration file in a folder below the application's folder, removed
// with its folders (when empty) at the end of the case.
class Declaration {
  QString m_file;
  QStringList m_folders;

public:
  Declaration(const QStringList& folders, const std::string& name,
              const QString& program, const std::string& arguments) {
    QDir directory(TSystem::getBinDir().getQString());
    for (const QString& folder : folders) {
      directory.setPath(directory.filePath(folder));
      m_folders.prepend(directory.path());
    }
    require(QDir().mkpath(directory.path()),
            "Cannot create the folder for an external-effect declaration");
    m_file = directory.filePath(QString::fromStdString(name) + ".xml");
    TOStream stream{TFilePath(m_file)};
    stream.openChild("externFx");
    stream.openCloseChild(
        "executable", {{"path", program.toStdString()}, {"args", arguments}});
    stream.openCloseChild("outport", {{"name", "out"}, {"ext", "png"}});
    stream.closeChild();
  }
  ~Declaration() {
    QFile::remove(m_file);
    for (const QString& folder : m_folders) QDir().rmdir(folder);
  }
};

// Refused by default, naming the node and the program; started once allowed.
void launchNeedsApproval(const QString& program, const QDir& directory) {
  const QString marker = directory.filePath("approval.txt");
  TExternalProgramFx effect;
  effect.setName(L"Gate node");
  effect.setExecutable(TFilePath(program), markerArguments(marker));
  effect.addPort("out", "png", false);
  require(!TExternalProgramPolicy::isAllowed(),
          "External programs are allowed before anything allowed them");
  const std::string refusal = compute(effect);
  require(!QFile::exists(marker),
          "An external program started without approval");
  for (const std::string& part :
       {std::string("\"Gate node\""), ::to_string(TFilePath(program)),
        std::string("turned off")})
    require(refusal.find(part) != std::string::npos,
            "The refusal does not name " + part + ": " + refusal);

  TExternalProgramPolicy::setAllowed(true);
  const std::string error = compute(effect);
  TExternalProgramPolicy::setAllowed(false);
  require(QFile::exists(marker),
          "An allowed external program did not start: " + error);
}

void scenePathIsNotStarted(const QString& program, const QDir& directory) {
  const QString marker = directory.filePath("scene path.txt");
  TExternalProgramFx effect;
  loadNode(effect, directory.filePath("not installed.xml"), uniqueName(),
           program, markerArguments(marker));
  const std::string error = compute(effect);
  require(!QFile::exists(marker),
          "A program named by the scene was started: " + error);
  require(
      error.find("no program is installed") != std::string::npos,
      "A scene without an installed declaration was not reported: " + error);
}

void declarationOutsideFolderIsNotRead(const QString& program,
                                       const QDir& directory) {
  const QString marker   = directory.filePath("outside.txt");
  const std::string name = uniqueName();
  const Declaration outside({"plugins"}, name, program,
                            markerArguments(marker));
  TExternalProgramFx loaded;
  loadNode(loaded, directory.filePath("outside.xml"), "../" + name, program,
           markerArguments(marker));
  std::unique_ptr<TExternFx> created(TExternFx::create("../" + name));
  auto* clone = dynamic_cast<TExternalProgramFx*>(created.get());
  require(clone != nullptr, "TExternFx::create made no external effect");
  for (TExternalProgramFx* effect : {&loaded, clone}) {
    const std::string error = compute(*effect);
    require(!QFile::exists(marker),
            "A declaration outside plugins/externFxs was started: " + error);
  }
}

void installedDeclarationRuns(const QString& program, const QDir& directory) {
  const QString installed = directory.filePath("installed.txt");
  const QString scene     = directory.filePath("scene arguments.txt");
  const std::string name  = uniqueName();
  const Declaration declaration({"plugins", "externFxs"}, name, program,
                                markerArguments(installed));
  TExternalProgramFx effect;
  loadNode(effect, directory.filePath("installed.xml"), name, program,
           markerArguments(scene));
  const std::string error = compute(effect);
  require(QFile::exists(installed),
          "The installed declaration's program did not run: " + error);
  require(!QFile::exists(scene),
          "The scene's arguments replaced the installed declaration's");
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  const QStringList arguments = application.arguments();
  if (arguments.size() > 1) return worker(arguments);
  try {
    const QString program = QCoreApplication::applicationFilePath();
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create a temporary folder");
    const QDir directory(temporary.path());
    launchNeedsApproval(program, directory);
    // Allowed from here: what a scene names must still not start.
    TExternalProgramPolicy::setAllowed(true);
    scenePathIsNotStarted(program, directory);
    declarationOutsideFolderIsNotRead(program, directory);
    installedDeclarationRuns(program, directory);
    std::cout << "External program gate: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
