#include "automation_assistance.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {
using AssistanceDetail::Build;
using AssistanceDetail::PackagePlatform;
const Build Posix{true, true, PackagePlatform::Posix};
const Build Windows{true, true, PackagePlatform::Windows};

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString& path, const QByteArray& data) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "Create fixture file");
  require(file.write(data) == data.size(), "Write entire fixture");
}

QJsonObject record(const QString& name,
                   const QString& hash = QString(64, '0')) {
  return {{"file", name}, {"bytes", 4}, {"sha256", hash}};
}

QJsonObject neuralManifest(const QDir& directory) {
  QJsonObject manifest{
      {"version", 1}, {"protocol", 1}, {"runtimeVersion", "2.3.0"}};
  for (const QString& role : {"worker", "runtime", "prediction", "transfer"}) {
    writeFile(directory.filePath(role), "test");
    manifest[role] = record(role);
  }
  manifest["prediction"] = record(
      "prediction",
      "af6ad4b2e7aeba0675f32636082ab915ced5375229a3f8aff7e714c6213f5ed2");
  manifest["transfer"] = record(
      "transfer",
      "7a1550643cf034a4d813c0aa276976cd15da4141b4f1ec3631db1d0d9c8e2cd1");
  return manifest;
}

QJsonObject inspect(const QString& root, const Build& build, int component) {
  const QJsonObject result = AssistanceDetail::inspectPackages(root, build);
  const QByteArray bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
  require(bytes.size() <= 16384, "Discovery result is bounded");
  require(!bytes.contains(root.toUtf8()),
          "Discovery must not expose local paths");
  const QJsonObject item =
      result.value("components").toArray().at(component).toObject();
  require(item.value("verification") == "not_requested",
          "Presence must not imply verified");
  require(item.value("runtime") == "not_observed",
          "Presence must not imply running");
  require(item.value("qualification") == "not_performed",
          "Presence must not imply qualified");
  require(!item.value("inference_available").toBool(),
          "Discovery must not enable inference");
  return item;
}

void state(const QJsonObject& item, const QString& presence,
           const QString& reason) {
  require(item.value("presence").toString() == presence,
          "Unexpected component presence");
  require(item.value("reasons").toArray() == QJsonArray{reason},
          "Unexpected component reason");
}

QDir package(const QString& root) {
  require(QDir(root).mkdir("neural-style"), "Create private package directory");
  return QDir(QDir(root).filePath("neural-style"));
}

void saveManifest(const QDir& directory, const QJsonObject& manifest) {
  writeFile(directory.filePath("package.json"),
            QJsonDocument(manifest).toJson());
}

void missingAndBuildState() {
  QTemporaryDir root;
  require(root.isValid(), "Private root exists");
  state(inspect(root.path(), Posix, 0), "missing", "worker_missing");
  state(inspect(root.path(), Posix, 1), "missing",
        "component_directory_missing");
  state(inspect(root.path(), Build{}, 0), "not_observed", "not_compiled");
  state(inspect(root.path(), Build{}, 1), "not_observed", "not_compiled");
  std::cout << "missing and compiled states PASS\n";
}

void neuralPresence() {
  QTemporaryDir root;
  const QDir directory = package(root.path());
  state(inspect(root.path(), Posix, 1), "incomplete", "manifest_missing");
  QJsonObject manifest = neuralManifest(directory);
  saveManifest(directory, manifest);
  state(inspect(root.path(), Posix, 1), "present_unverified",
        "integrity_and_runtime_not_verified");
  writeFile(directory.filePath("worker"), "fake");
  state(inspect(root.path(), Posix, 1), "present_unverified",
        "integrity_and_runtime_not_verified");
  writeFile(directory.filePath("runtime"), "wrong-size");
  state(inspect(root.path(), Posix, 1), "incomplete",
        "component_missing_or_changed");
  std::cout << "presence is not content verification PASS\n";
}

void malformedManifest() {
  QTemporaryDir root;
  const QDir directory = package(root.path());
  writeFile(directory.filePath("package.json"), "{");
  state(inspect(root.path(), Posix, 1), "incomplete", "manifest_invalid_json");
  writeFile(directory.filePath("package.json"), QByteArray(16385, ' '));
  state(inspect(root.path(), Posix, 1), "incomplete", "manifest_size_invalid");
  QJsonObject manifest = neuralManifest(directory);
  QByteArray boundary  = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
  boundary.append(QByteArray(16384 - boundary.size(), ' '));
  writeFile(directory.filePath("package.json"), boundary);
  state(inspect(root.path(), Posix, 1), "present_unverified",
        "integrity_and_runtime_not_verified");
  writeFile(directory.filePath("package.json"), boundary + ' ');
  state(inspect(root.path(), Posix, 1), "incomplete", "manifest_size_invalid");
  manifest["version"] = 1.5;
  saveManifest(directory, manifest);
  state(inspect(root.path(), Posix, 1), "incomplete", "version_unsupported");
  std::cout << "malformed, oversized and unsupported manifest PASS\n";
}

void invalidDeclarations() {
  QTemporaryDir root;
  const QDir directory    = package(root.path());
  const QJsonObject valid = neuralManifest(directory);
  const QStringList names{"../outside", "/absolute", "a:b", "a\\b",
                          ".",          "..",        ""};
  for (const QString& name : names) {
    QJsonObject manifest = valid;
    manifest["worker"]   = record(name);
    saveManifest(directory, manifest);
    state(inspect(root.path(), Posix, 1), "incomplete",
          "component_declaration_invalid");
  }
  for (double bytes : {0.0, -1.0, 1.5, 67108865.0, 1e100}) {
    QJsonObject manifest = valid;
    QJsonObject value    = record("worker");
    value["bytes"]       = bytes;
    manifest["worker"]   = value;
    saveManifest(directory, manifest);
    state(inspect(root.path(), Posix, 1), "incomplete",
          "component_declaration_invalid");
  }
  std::cout << "unsafe filenames and invalid size declarations PASS\n";
}

void payloadsRemainUnread() {
  QTemporaryDir root;
  const QDir directory       = package(root.path());
  const QJsonObject manifest = neuralManifest(directory);
  saveManifest(directory, manifest);
  for (const QString& role : {"worker", "runtime", "prediction", "transfer"})
    require(
        QFile::setPermissions(directory.filePath(role), QFile::Permissions()),
        "Deny payload access for metadata-only inspection");
  state(inspect(root.path(), Posix, 1), "present_unverified",
        "integrity_and_runtime_not_verified");
  std::cout << "metadata discovery does not read executable/runtime/model "
               "payloads PASS\n";
}

void modelIdentity() {
  QTemporaryDir root;
  const QDir directory   = package(root.path());
  QJsonObject manifest   = neuralManifest(directory);
  manifest["prediction"] = record("prediction");
  saveManifest(directory, manifest);
  state(inspect(root.path(), Posix, 1), "incomplete",
        "model_revision_unsupported");
  manifest["worker"] = record("worker", QString(64, 'z'));
  saveManifest(directory, manifest);
  state(inspect(root.path(), Posix, 1), "incomplete",
        "component_declaration_invalid");
  std::cout
      << "unsupported model identity and malformed hash declaration PASS\n";
}

void symlinks() {
  QTemporaryDir root;
  QTemporaryDir external;
  const QDir directory       = package(root.path());
  const QJsonObject manifest = neuralManifest(directory);
  saveManifest(directory, manifest);
  require(QFile::remove(directory.filePath("worker")), "Remove fixture worker");
  writeFile(external.filePath("worker"), "test");
  require(
      QFile::link(external.filePath("worker"), directory.filePath("worker")),
      "Link worker");
  state(inspect(root.path(), Posix, 1), "incomplete",
        "component_missing_or_changed");
  require(QFile::remove(directory.filePath("package.json")),
          "Remove fixture manifest");
  writeFile(external.filePath("manifest"), QJsonDocument(manifest).toJson());
  require(QFile::link(external.filePath("manifest"),
                      directory.filePath("package.json")),
          "Link manifest");
  state(inspect(root.path(), Posix, 1), "incomplete", "manifest_not_regular");
  QTemporaryDir other;
  require(QFile::link(directory.path(), other.filePath("neural-style")),
          "Link package directory");
  state(inspect(other.path(), Posix, 1), "incomplete",
        "component_directory_not_regular");
  std::cout << "component, manifest and package symlink boundaries PASS\n";
}

void posixCadmium() {
  QTemporaryDir root;
  const QString worker = root.filePath("cadmium-sidecar");
  writeFile(worker, "test");
  state(inspect(root.path(), Posix, 0), "incomplete", "worker_not_executable");
  require(QFile::setPermissions(
              worker, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
          "Set fixture executable metadata without executing it");
  state(inspect(root.path(), Posix, 0), "present_unverified",
        "unpackaged_worker_models_not_observed");
  writeFile(root.filePath("runtime-manifest.json"), "{}");
  state(inspect(root.path(), Posix, 0), "incomplete", "version_unsupported");
  std::cout
      << "POSIX Cadmium presence does not launch or inspect models PASS\n";
}

void windowsCadmiumMetadata() {
  QTemporaryDir root;
  QJsonArray files;
  for (const QString& name :
       {"cadmium-sidecar.exe", "onnxruntime.dll",
        "onnxruntime_providers_shared.dll", "DirectML.dll"}) {
    writeFile(root.filePath(name), "test");
    files.append(record(name));
  }
  QJsonObject manifest{{"format", 1},
                       {"platform", "windows-x64"},
                       {"source", "d817cae470a1499df4985be460e0fdc721233987"},
                       {"files", files}};
  const QString path = root.filePath("runtime-manifest.json");
  writeFile(path, QJsonDocument(manifest).toJson());
  state(inspect(root.path(), Windows, 0), "present_unverified",
        "runtime_and_models_not_verified");
  files[1]          = files[0];
  manifest["files"] = files;
  writeFile(path, QJsonDocument(manifest).toJson());
  state(inspect(root.path(), Windows, 0), "incomplete",
        "component_set_invalid");
  writeFile(path, QByteArray(65537, ' '));
  state(inspect(root.path(), Windows, 0), "incomplete",
        "manifest_size_invalid");
  std::cout
      << "Windows package metadata only (no Windows runtime claim) PASS\n";
}

void invalidRoot() {
  bool refused = false;
  try {
    AssistanceDetail::inspectPackages("relative", Posix);
  } catch (const Error&) {
    refused = true;
  }
  require(refused, "Relative application root must reject");
  std::cout << "trusted canonical application root requirement PASS\n";
}
void selectionAndArguments() {
  QTemporaryDir root;
  const auto both       = AssistanceDetail::inspectPackages(root.path(), Posix);
  const auto components = both.value("components").toArray();
  for (int index = 0; index < components.size(); ++index) {
    const auto name =
        components.at(index).toObject().value("component").toString();
    const auto selected =
        AssistanceDetail::inspectPackages(root.path(), Posix, name);
    require(selected.value("components").toArray() ==
                QJsonArray{components.at(index)},
            "Selecting one component must preserve its observation");
  }
  for (const QJsonObject& params :
       {QJsonObject{{"component", "other"}}, QJsonObject{{"component", 1}},
        QJsonObject{{"component", ""}}, QJsonObject{{"path", root.path()}}}) {
    bool refused = false;
    try {
      executeAssistance("assistance.inspect", params);
    } catch (const Error& error) {
      refused = error.code == "invalid_params";
    }
    require(refused,
            "Invalid inspection parameters must fail before observation");
  }
  bool refused = false;
  try {
    executeAssistance("assistance.unknown", {});
  } catch (const Error& error) {
    refused = error.code == "unknown_operation";
  }
  require(refused, "Unknown assistance method must fail");
  const auto descriptor = assistanceOperations().first().toObject();
  require(
      descriptor.value("annotations").toObject().value("readOnlyHint").toBool(),
      "Passive inspection must be advertised read-only");
  std::cout << "component selection and rejected arguments PASS\n";
}

void declarationAndFilesystemEdges() {
  QTemporaryDir root;
  const QDir directory = package(root.path());
  const auto valid     = neuralManifest(directory);
  for (const QString& name : {QString(129, 'x'), QString("nul") + QChar(0)}) {
    auto manifest      = valid;
    manifest["worker"] = record(name);
    saveManifest(directory, manifest);
    state(inspect(root.path(), Posix, 1), "incomplete",
          "component_declaration_invalid");
  }
  for (const QByteArray& text : {QByteArray(), QByteArray("[]")}) {
    writeFile(directory.filePath("package.json"), text);
    state(inspect(root.path(), Posix, 1), "incomplete",
          text.isEmpty() ? "manifest_size_invalid" : "manifest_invalid_json");
  }
  saveManifest(directory, valid);
  require(QFile::setPermissions(directory.filePath("package.json"),
                                QFile::Permissions()),
          "Deny manifest reads");
  state(inspect(root.path(), Posix, 1), "incomplete", "manifest_unreadable");
  require(QFile::setPermissions(directory.filePath("package.json"),
                                QFile::ReadOwner | QFile::WriteOwner),
          "Restore manifest");
  require(QFile::remove(directory.filePath("package.json")), "Remove manifest");
  require(QDir().mkdir(directory.filePath("package.json")),
          "Create manifest directory");
  state(inspect(root.path(), Posix, 1), "incomplete", "manifest_not_regular");
  std::cout << "declaration and filesystem edge cases PASS\n";
}
}  // namespace

void runAutomationAssistanceTests() {
  missingAndBuildState();
  neuralPresence();
  malformedManifest();
  invalidDeclarations();
  payloadsRemainUnread();
  modelIdentity();
  symlinks();
  posixCadmium();
  windowsCadmiumMetadata();
  invalidRoot();
  selectionAndArguments();
  declarationAndFilesystemEdges();
}
}  // namespace GroundzeroAutomation
