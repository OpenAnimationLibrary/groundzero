#include "automation_assistance.h"

#include <QCoreApplication>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <cmath>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace AssistanceDetail {
namespace {
constexpr qint64 NeuralManifestLimit  = 16 * 1024;
constexpr qint64 CadmiumManifestLimit = 64 * 1024;
constexpr qint64 NeuralFileLimit      = 64 * 1024 * 1024;
constexpr qint64 CadmiumFileLimit     = 1024LL * 1024 * 1024;
const QString PredictionHash =
    "af6ad4b2e7aeba0675f32636082ab915ced5375229a3f8aff7e714c6213f5ed2";
const QString TransferHash =
    "7a1550643cf034a4d813c0aa276976cd15da4141b4f1ec3631db1d0d9c8e2cd1";

QJsonObject base(const QString& component, bool compiled) {
  return {
      {"component", component},           {"compiled", compiled},
      {"package_source", "app_adjacent"}, {"configuration", "not_observed"},
      {"presence", "not_observed"},       {"verification", "not_requested"},
      {"runtime", "not_observed"},        {"qualification", "not_performed"},
      {"inference_available", false},     {"reasons", QJsonArray()}};
}

QJsonObject withState(QJsonObject result, const QString& state,
                      const QString& reason) {
  result["presence"] = state;
  result["reasons"]  = QJsonArray{reason};
  return result;
}

bool ordinaryFile(const QFileInfo& info) {
  return !info.isSymLink() && info.isFile();
}

bool safeName(const QString& name) {
  if (name.isEmpty() || name.size() > 128 || name == "." || name == "..")
    return false;
  const QString forbidden = QString::fromLatin1("\0/\\:", 4);
  for (const QChar character : forbidden) {
    if (name.contains(character)) return false;
  }
  return true;
}

bool hashDeclaration(const QString& value) {
  const QByteArray bytes = value.toLatin1();
  return bytes.size() == 64 && QByteArray::fromHex(bytes).toHex() == bytes;
}

bool fileDeclaration(const QJsonObject& record, qint64 limit) {
  const double size = record.value("bytes").toDouble(-1);
  if (!std::isfinite(size) || size <= 0 || size > limit ||
      std::floor(size) != size)
    return false;
  return safeName(record.value("file").toString()) &&
         hashDeclaration(record.value("sha256").toString());
}

bool filePresent(const QDir& directory, const QJsonObject& record) {
  const QFileInfo info(directory.filePath(record.value("file").toString()));
  return ordinaryFile(info) &&
         info.size() == qint64(record.value("bytes").toDouble());
}

QString manifestMetadata(const QFileInfo& info, qint64 limit) {
  if (!info.exists() && !info.isSymLink()) return "manifest_missing";
  if (!ordinaryFile(info)) return "manifest_not_regular";
  if (info.size() <= 0 || info.size() > limit) return "manifest_size_invalid";
  return {};
}

QString readManifest(const QString& path, qint64 limit, QJsonObject& manifest) {
  const QFileInfo info(path);
  const QString metadataError = manifestMetadata(info, limit);
  if (!metadataError.isEmpty()) return metadataError;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return "manifest_unreadable";
  const QByteArray bytes = file.read(limit);
  if (file.error() != QFile::NoError || bytes.size() != info.size() ||
      !file.atEnd())
    return "manifest_changed_or_unreadable";
  QJsonParseError error;
  const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !parsed.isObject())
    return "manifest_invalid_json";
  manifest = parsed.object();
  return {};
}

bool neuralVersion(const QJsonObject& manifest) {
  return manifest.value("version").toDouble(-1) == 1 &&
         manifest.value("protocol").toDouble(-1) == 1 &&
         manifest.value("runtimeVersion").toString() == "2.3.0";
}

QString neuralFiles(const QDir& directory, const QJsonObject& manifest) {
  const QStringList roles{"worker", "runtime", "prediction", "transfer"};
  for (const QString& role : roles) {
    const QJsonObject record = manifest.value(role).toObject();
    if (!fileDeclaration(record, NeuralFileLimit))
      return "component_declaration_invalid";
    if (!filePresent(directory, record)) return "component_missing_or_changed";
  }
  if (manifest.value("prediction").toObject().value("sha256").toString() !=
          PredictionHash ||
      manifest.value("transfer").toObject().value("sha256").toString() !=
          TransferHash)
    return "model_revision_unsupported";
  return {};
}

bool missingPath(const QFileInfo& info) {
  return !info.exists() && !info.isSymLink();
}

bool ordinaryDirectory(const QFileInfo& info) {
  return !info.isSymLink() && info.isDir();
}

QJsonObject neural(const QDir& application, bool compiled) {
  QJsonObject result = base("neural_style", compiled);
  if (!compiled) return withState(result, "not_observed", "not_compiled");
  const QFileInfo directory(application.filePath("neural-style"));
  if (missingPath(directory))
    return withState(result, "missing", "component_directory_missing");
  if (!ordinaryDirectory(directory))
    return withState(result, "incomplete", "component_directory_not_regular");
  const QDir package(directory.filePath());
  QJsonObject manifest;
  const QString error = readManifest(package.filePath("package.json"),
                                     NeuralManifestLimit, manifest);
  if (!error.isEmpty()) return withState(result, "incomplete", error);
  if (!neuralVersion(manifest))
    return withState(result, "incomplete", "version_unsupported");
  const QString filesError = neuralFiles(package, manifest);
  if (!filesError.isEmpty()) return withState(result, "incomplete", filesError);
  result["declared_identity"] = QJsonObject{
      {"version", 1}, {"protocol", 1}, {"runtime_version", "2.3.0"}};
  return withState(result, "present_unverified",
                   "integrity_and_runtime_not_verified");
}

bool cadmiumVersion(const QJsonObject& manifest) {
  return manifest.value("format").toDouble(-1) == 1 &&
         manifest.value("source").toString() ==
             "d817cae470a1499df4985be460e0fdc721233987" &&
         manifest.value("platform").toString() == "windows-x64";
}

QString cadmiumFiles(const QDir& directory, const QJsonObject& manifest) {
  QSet<QString> expected{"cadmium-sidecar.exe", "onnxruntime.dll",
                         "onnxruntime_providers_shared.dll", "DirectML.dll"};
  const QJsonArray records = manifest.value("files").toArray();
  if (records.size() != expected.size()) return "component_set_invalid";
  for (const QJsonValue& value : records) {
    const QJsonObject record = value.toObject();
    if (!fileDeclaration(record, CadmiumFileLimit))
      return "component_declaration_invalid";
    if (!expected.remove(record.value("file").toString()))
      return "component_set_invalid";
    if (!filePresent(directory, record)) return "component_missing_or_changed";
  }
  return {};
}

QJsonObject cadmiumPackage(QJsonObject result, const QDir& application,
                           bool windows) {
  QJsonObject manifest;
  const QString error =
      readManifest(application.filePath("runtime-manifest.json"),
                   CadmiumManifestLimit, manifest);
  if (!windows && error == "manifest_missing")
    return withState(result, "present_unverified",
                     "unpackaged_worker_models_not_observed");
  if (!error.isEmpty()) return withState(result, "incomplete", error);
  if (!windows || !cadmiumVersion(manifest))
    return withState(result, "incomplete", "version_unsupported");
  const QString filesError = cadmiumFiles(application, manifest);
  if (!filesError.isEmpty()) return withState(result, "incomplete", filesError);
  result["declared_identity"] =
      QJsonObject{{"format", 1},
                  {"platform", "windows-x64"},
                  {"source", "d817cae470a1499df4985be460e0fdc721233987"}};
  return withState(result, "present_unverified",
                   "runtime_and_models_not_verified");
}

QJsonObject cadmium(const QDir& application, const Build& build) {
  QJsonObject result = base("cadmium", build.cadmium);
  if (!build.cadmium) return withState(result, "not_observed", "not_compiled");
  const bool windows = build.platform == PackagePlatform::Windows;
  const QFileInfo worker(application.filePath(windows ? "cadmium-sidecar.exe"
                                                      : "cadmium-sidecar"));
  if (missingPath(worker))
    return withState(result, "missing", "worker_missing");
  if (!ordinaryFile(worker))
    return withState(result, "incomplete", "worker_not_regular");
  if (!windows && !worker.isExecutable())
    return withState(result, "incomplete", "worker_not_executable");
  return cadmiumPackage(result, application, windows);
}

void requireApplicationRoot(const QString& directory) {
  const QFileInfo root(directory);
  if (!root.isAbsolute() || !root.isDir() || root.isSymLink() ||
      root.canonicalFilePath() != QDir::cleanPath(directory))
    throw Error(
        "invalid_application_root",
        "Assistance inspection requires a canonical application directory.");
}

}  // namespace

QJsonObject inspectPackages(const QString& applicationDirectory,
                            const Build& build, const QString& component) {
  if (component != "all" && component != "cadmium" &&
      component != "neural_style")
    throw Error("invalid_params",
                "component must be all, cadmium or neural_style.");
  requireApplicationRoot(applicationDirectory);
  const QDir application(applicationDirectory);
  QJsonArray components;
  if (component != "neural_style")
    components.append(cadmium(application, build));
  if (component != "cadmium")
    components.append(neural(application, build.neuralStyle));
  const QJsonObject result{{"format", 1}, {"components", components}};
  if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > 16 * 1024)
    throw Error("resource_limit",
                "Assistance inspection exceeds its response budget.");
  return result;
}
}  // namespace AssistanceDetail

QJsonArray assistanceOperations() {
  const QJsonObject component{
      {"type", "string"},
      {"enum", QJsonArray{"all", "cadmium", "neural_style"}},
      {"default", "all"}};
  return {operation(
      "assistance.inspect",
      "Inspect bounded app-adjacent component metadata; this read does not "
      "establish inference availability or verify component integrity.",
      {{"component", component}}, {})};
}

QJsonValue executeAssistance(const QString& method, const QJsonObject& params) {
  if (method != "assistance.inspect")
    throw Error("unknown_operation", "Unknown assistance operation.");
  for (auto item = params.begin(); item != params.end(); ++item)
    if (item.key() != "component")
      throw Error("invalid_params",
                  "Assistance inspection accepts only component.");
  const QString component = params.contains("component")
                                ? requiredString(params, "component")
                                : "all";
  // Both native implementations are unconditional application sources in CMake.
  AssistanceDetail::Build build{true, true};
#ifdef _WIN32
  build.platform = AssistanceDetail::PackagePlatform::Windows;
#endif
  const QString directory =
      QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath();
  return AssistanceDetail::inspectPackages(directory, build, component);
}
}  // namespace GroundzeroAutomation
