#include "automation_projects.h"
#include "automation.h"
#include "automation_files.h"
#include "automation_scene_host.h"
#include "automation_shared_parse.h"

#include "texception.h"
#include "toutputproperties.h"
#include "toonz/filepathproperties.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/tproject.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <cmath>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxFolders          = 32;
constexpr int MaxNameLength       = 64;
constexpr double DefaultTolerance = 1e-6;

struct Folder final {
  QString alias;
  QString path;
  bool useScenePath = false;
};

struct Rules final {
  bool useStandard        = true;
  bool acceptNonAlphabet  = false;
  int letterCount         = 1;
  bool noSeparatorAllowed = false;
};

struct Defaults final {
  double frameRate = 24;
  int widthPx      = 1920;
  int heightPx     = 1080;
  double widthIn   = 16;
  double heightIn  = 9;
};

struct ProjectRequest final {
  QString directory;
  QString name;
  std::vector<Folder> folders;
  Rules rules;
  Defaults defaults;
};

[[noreturn]] void invalid(const QString& message) {
  throw Error("invalid_params", message);
}

QString projectName(const QString& directory) {
  static const QRegularExpression forbidden(R"([\\/:*?"<>|\x00-\x1f\x7f])");
  const QString name = QFileInfo(directory).fileName();
  if (name.isEmpty() || name.size() > MaxNameLength || name == "." ||
      name == ".." || name.contains(forbidden))
    invalid(
        "The project name, the last component of directory, needs 1-64 "
        "characters without \\ / : * ? \" < > | or control characters.");
  return name;
}

// The workspace path of the new project folder: absent, with a parent that
// is a folder.
QString newProjectDirectory(Context& context, const QJsonObject& params) {
  const QString directory =
      context.resolvePath(requiredString(params, "directory"), true);
  const QFileInfo info(directory);
  if (info.exists() || info.isSymLink())
    throw Error("already_exists", "The project folder already exists.",
                {{"directory", directory}});
  if (!QFileInfo(info.absolutePath()).isDir())
    invalid("The parent folder of directory does not exist.");
  return directory;
}

const std::vector<Folder>& standardFolders() {
  static const std::vector<Folder> folders = [] {
    std::vector<Folder> result;
    for (const std::string& name :
         {TProject::Inputs, TProject::Drawings, TProject::Scenes,
          TProject::Extras, TProject::Outputs, TProject::Scripts,
          TProject::Palettes})
      result.push_back(
          {QString::fromStdString(name), QString::fromStdString(name), false});
    return result;
  }();
  return folders;
}

Folder requestedFolder(const QJsonObject& record) {
  static const QRegularExpression alias("^[a-z][a-z0-9_]{0,31}$");
  static const QRegularExpression relative(
      R"(^(\$scenefolder(/[A-Za-z0-9 _.-]+)*|[A-Za-z0-9 _.-]+(/[A-Za-z0-9 _.-]+)*)$)");
  Folder folder{record.value("alias").toString(),
                record.value("path").toString(),
                record.value("use_scene_path").toBool(false)};
  if (!alias.match(folder.alias).hasMatch())
    invalid("A folder alias is [a-z][a-z0-9_]{0,31}.");
  if (!relative.match(folder.path).hasMatch() ||
      folder.path.split('/').contains(".."))
    invalid(
        "A folder path is relative to the project folder or to "
        "$scenefolder, without '..'.");
  return folder;
}

std::vector<Folder> requestedFolders(const QJsonObject& params) {
  if (!params.contains("folders")) return standardFolders();
  std::vector<Folder> folders;
  QStringList aliases;
  for (const QJsonValue& value : params.value("folders").toArray()) {
    const Folder folder = requestedFolder(value.toObject());
    if (aliases.contains(folder.alias))
      invalid("Folder aliases must be unique.");
    aliases.append(folder.alias);
    folders.push_back(folder);
  }
  if (folders.empty() || int(folders.size()) > MaxFolders)
    invalid("A project has 1 to 32 folders.");
  return folders;
}

Rules requestedRules(const QJsonObject& params) {
  Rules rules;
  if (!params.contains("file_path_rules")) return rules;
  const QJsonObject value = params.value("file_path_rules").toObject();
  rules.useStandard = value.value("use_standard").toBool(rules.useStandard);
  rules.acceptNonAlphabet =
      value.value("accept_non_alphabet_suffix").toBool(rules.acceptNonAlphabet);
  rules.letterCount =
      value.value("letter_count_for_suffix").toInt(rules.letterCount);
  rules.noSeparatorAllowed = value.value("no_separator_format_allowed")
                                 .toBool(rules.noSeparatorAllowed);
  return rules;
}

Defaults requestedDefaults(const QJsonObject& params) {
  Defaults defaults;
  if (!params.contains("scene_defaults")) return defaults;
  const QJsonObject value = params.value("scene_defaults").toObject();
  defaults.frameRate = value.value("frame_rate").toDouble(defaults.frameRate);
  defaults.widthPx   = value.value("camera_width_px").toInt(defaults.widthPx);
  defaults.heightPx  = value.value("camera_height_px").toInt(defaults.heightPx);
  defaults.widthIn = value.value("camera_width_in").toDouble(defaults.widthIn);
  defaults.heightIn =
      value.value("camera_height_in").toDouble(defaults.heightIn);
  return defaults;
}

void requireUnusedName(Context& context, const QString& name) {
  if (projectNameRegistered(context, name))
    throw Error("name_conflict",
                "A project listed or created by this worker already has this "
                "name.",
                {{"name", name}});
}

ProjectRequest createRequest(Context& context, const QJsonObject& params) {
  ProjectRequest request;
  request.directory = newProjectDirectory(context, params);
  request.name      = projectName(request.directory);
  requireUnusedName(context, request.name);
  request.folders  = requestedFolders(params);
  request.rules    = requestedRules(params);
  request.defaults = requestedDefaults(params);
  return request;
}

// The definition from the request alone: never the worker's or the editor's
// current project, whose scene properties native creation copies.
void buildProject(TProject& project, const ProjectRequest& request) {
  for (const Folder& folder : request.folders) {
    const std::string alias = folder.alias.toStdString();
    project.setFolder(alias, TFilePath(folder.path.toStdWString()));
    project.setUseScenePath(alias, folder.useScenePath);
  }
  FilePathProperties* rules = project.getFilePathProperties();
  rules->setUseStandard(request.rules.useStandard);
  rules->setAcceptNonAlphabetSuffix(request.rules.acceptNonAlphabet);
  rules->setLetterCountForSuffix(request.rules.letterCount);
  rules->setNoSeparatorFormatAllowed(request.rules.noSeparatorAllowed);
  TSceneProperties properties;
  auto* camera = new TCamera();
  camera->setRes(
      TDimension(request.defaults.widthPx, request.defaults.heightPx));
  camera->setSize(
      TDimensionD(request.defaults.widthIn, request.defaults.heightIn));
  properties.getCameras().push_back(camera);
  properties.getOutputProperties()->setFrameRate(request.defaults.frameRate);
  properties.getPreviewProperties()->setFrameRate(request.defaults.frameRate);
  project.setSceneProperties(properties);
}

QString stagedProjectFile(const PrivateDirectory& staging,
                          const ProjectRequest& request) {
  return staging.filePath(request.name + "_otprj.xml");
}

void saveStaged(const PrivateDirectory& staging,
                const ProjectRequest& request) {
  TProject project;
  buildProject(project, request);
  try {
    if (project.save(
            TFilePath(stagedProjectFile(staging, request).toStdWString())))
      return;
  } catch (const TException& error) {
    throw stagedWriteError("io_error",
                           QString::fromStdWString(error.getMessage()), "stage",
                           QString(), false);
  }
  throw stagedWriteError("io_error", "The native project writer failed.",
                         "stage", QString(), false);
}

bool near(const QJsonObject& reread, const QString& key, double value) {
  return std::abs(reread.value(key).toDouble(NAN) - value) <= DefaultTolerance;
}

bool sameDefaults(const QJsonObject& reread, const Defaults& defaults) {
  return near(reread, "frame_rate", defaults.frameRate) &&
         reread.value("camera_width_px").toInt() == defaults.widthPx &&
         reread.value("camera_height_px").toInt() == defaults.heightPx &&
         near(reread, "camera_width_in", defaults.widthIn) &&
         near(reread, "camera_height_in", defaults.heightIn);
}

bool sameRules(const QJsonObject& reread, const Rules& rules) {
  return reread.value("use_standard").toBool() == rules.useStandard &&
         reread.value("accept_non_alphabet_suffix").toBool() ==
             rules.acceptNonAlphabet &&
         reread.value("letter_count_for_suffix").toInt() == rules.letterCount &&
         reread.value("no_separator_format_allowed").toBool() ==
             rules.noSeparatorAllowed;
}

bool sameFolders(const QJsonArray& reread, const std::vector<Folder>& folders) {
  for (const Folder& folder : folders) {
    bool found = false;
    for (const QJsonValue& value : reread) {
      const QJsonObject row = value.toObject();
      if (row.value("alias") != folder.alias) continue;
      found = row.value("coded_path") == folder.path &&
              row.value("use_scene_path").toBool() == folder.useScenePath;
    }
    if (!found) return false;
  }
  return true;
}

// Read the staged definition back through the parse helper, as any project
// is read, and require what was asked for.
void verifyStaged(Context& context, const PrivateDirectory& staging,
                  const ProjectRequest& request) {
  const QString file = stagedProjectFile(staging, request);
  QJsonObject reread;
  try {
    reread =
        parseSharedFile(context, "project", file.toUtf8(), "invalid_project");
  } catch (const Error& error) {
    throw stagedWriteError("native_error", error.message, "verify",
                           QFileInfo(file).fileName(), false);
  }
  if (!sameFolders(reread.value("folders").toArray(), request.folders) ||
      !sameRules(reread.value("file_path_rules").toObject(), request.rules) ||
      !sameDefaults(reread.value("scene_defaults").toObject(),
                    request.defaults))
    throw stagedWriteError("native_error",
                           "The reread project differs from the request.",
                           "verify", QFileInfo(file).fileName(), false);
}

QJsonObject writeProject(Context& context, const ProjectRequest& request) {
  if (context.runtimeDirectory.isEmpty())
    throw Error("io_error", "The worker has no private runtime directory.");
  PrivateDirectory staging(context.runtimeDirectory + "/project-XXXXXX");
  if (!staging.isValid())
    throw stagedWriteError("io_error",
                           "Cannot create a private project staging folder.",
                           "stage", QString(), false);
  saveStaged(staging, request);
  // Test-only write fault project:staged: the staged definition comes out
  // with another frame rate, as a writer that lost a field would leave it.
  if (writeFaultFires(context, "project:staged")) {
    ProjectRequest other = request;
    other.defaults.frameRate += 1;
    saveStaged(staging, other);
  }
  verifyStaged(context, staging, request);
  publishStaged(context, staging.path(), request.directory, "io_error",
                "Cannot publish the verified project folder.");
  staging.setAutoRemove(false);
  const QString file =
      QFileInfo(QDir(request.directory).filePath(request.name + "_otprj.xml"))
          .canonicalFilePath();
  const QString id = registerWrittenProject(context, file);
  return inspectGrantedProject(context, id).toObject();
}

QJsonValue createProject(Context& context, const QJsonObject& params) {
  const ProjectRequest request = createRequest(context, params);
  return writeProject(context, request);
}

// A folder of the copy that still names a place outside the new project
// folder: an absolute path or one that climbs out of it.
bool sharedFolder(const QString& coded) {
  return QDir::isAbsolutePath(coded) || coded.split('/').contains("..");
}

QJsonValue duplicateProject(Context& context, const QJsonObject& params) {
  const QString sourceId = requiredString(params, "project_id");
  const QJsonObject source =
      inspectGrantedProject(context, sourceId).toObject();
  ProjectRequest request;
  request.directory = newProjectDirectory(context, params);
  request.name      = projectName(request.directory);
  requireUnusedName(context, request.name);
  QJsonArray shared;
  for (const QJsonValue& value : source.value("folders").toArray()) {
    const QJsonObject row = value.toObject();
    const QString coded   = row.value("coded_path").toString();
    if (coded.isEmpty()) continue;
    request.folders.push_back({row.value("alias").toString(), coded,
                               row.value("use_scene_path").toBool()});
    if (sharedFolder(coded)) shared.append(row.value("alias"));
  }
  request.rules =
      requestedRules({{"file_path_rules", source.value("file_path_rules")}});
  if (source.value("scene_defaults").isObject())
    request.defaults =
        requestedDefaults({{"scene_defaults", source.value("scene_defaults")}});
  QJsonObject result = writeProject(context, request);
  result.insert("source_project_id", sourceId);
  result.insert("assets_copied", false);
  result.insert("shared_folders", shared);
  return result;
}

QJsonObject integer(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject number(double minimum, double maximum) {
  return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject strict(const QJsonObject& properties, const QJsonArray& required) {
  return {{"type", "object"},
          {"properties", properties},
          {"required", required},
          {"additionalProperties", false}};
}

QJsonObject createProperties() {
  const QJsonObject boolean{{"type", "boolean"}};
  const QJsonObject folder = strict(
      {{"alias",
        QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 32}}},
       {"path",
        QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
       {"use_scene_path", boolean}},
      {"alias", "path"});
  return {
      {"directory",
       QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
      {"folders", QJsonObject{{"type", "array"},
                              {"items", folder},
                              {"minItems", 1},
                              {"maxItems", MaxFolders}}},
      {"file_path_rules", strict({{"use_standard", boolean},
                                  {"accept_non_alphabet_suffix", boolean},
                                  {"letter_count_for_suffix", integer(0, 8)},
                                  {"no_separator_format_allowed", boolean}},
                                 {})},
      {"scene_defaults", strict({{"frame_rate", number(1, 240)},
                                 {"camera_width_px", integer(1, 8192)},
                                 {"camera_height_px", integer(1, 8192)},
                                 {"camera_width_in", number(0.001, 10000)},
                                 {"camera_height_in", number(0.001, 10000)}},
                                {})}};
}

}  // namespace

QJsonArray projectWriteOperations() {
  const QJsonObject directory{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 4096}};
  return {
      operation(
          "project.create",
          "Create a native project definition in a new workspace folder from "
          "explicit folders, file-path rules and scene defaults (never from "
          "the editor's current project), verify it by reading it back, "
          "publish it without replacing anything and return its project_id.",
          createProperties(), {"directory"}, true),
      operation(
          "project.duplicate",
          "Copy a listed or created project's definition (folders, rules and "
          "scene defaults) to a new workspace folder. No scene, level or "
          "other asset is copied; folders that point outside the new project "
          "are listed as shared_folders.",
          {{"project_id",
            QJsonObject{
                {"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
           {"directory", directory}},
          {"project_id", "directory"}, true)};
}

QJsonValue executeProjectWrite(Context& context, const QString& method,
                               const QJsonObject& params) {
  if (method == "project.create") return createProject(context, params);
  if (method == "project.duplicate") return duplicateProject(context, params);
  throw Error("unknown_method", "Unknown project write operation.");
}

DraftProject draftProject(Context& context, const QString& projectId) {
  const QJsonObject record =
      inspectGrantedProject(context, projectId).toObject();
  return {QJsonObject{{"project_id", projectId},
                      {"name", record.value("name")},
                      {"file_revision", record.value("file_revision")},
                      {"file_path_rules_applied", false}},
          record.value("scene_defaults")};
}

}  // namespace GroundzeroAutomation
