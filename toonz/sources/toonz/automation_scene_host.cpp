#include "automation_scene_host.h"
#include "automation_shared_parse.h"

#include "tcolorstyles.h"
#include "tlevel_io.h"
#include "tpalette.h"
#include "tstream.h"
#include "tsystem.h"
#include "toonz/filepathproperties.h"
#include "toonz/fullcolorpalette.h"
#include "toonz/imagestyles.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toutputproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <chrono>
#include <cstdlib>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace GroundzeroAutomation {
namespace {

constexpr int HostMemoryExit = 125;
#ifndef _WIN32
constexpr int HostOrphanExit = 126;
#endif

struct NativeBinding final {
  QString status;
  QString projectId;
  QString projectFile;
  QString cause;
};

QByteArray readBytes(const QString& path, qint64 limit) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return QByteArray();
  return file.read(limit + 1);
}

[[noreturn]] void changed(const QString& path) {
  throw Error("source_changed",
              "A source file changed while the scene was being opened.",
              {{"paths", QJsonArray{path}}, {"count", 1}});
}

bool insideRoots(const HostLoadRequest& request, const QString& path) {
  for (const QString& root : request.readRoots)
    if (path == root || path.startsWith(root.endsWith('/') ? root : root + '/'))
      return true;
  return false;
}

// Authorized means inside a read root both lexically and canonically, with
// no symlink component; anything else is never stat-ed or read.
QString authorized(Context& context, const HostLoadRequest& request,
                   const QString& path) {
  const QString clean = QDir::cleanPath(path);
  if (!QDir::isAbsolutePath(clean) || !insideRoots(request, clean))
    return QString();
  try {
    return context.resolvePath(clean, false);
  } catch (const Error&) {
    return QString();
  }
}

NativeBinding parseScenesXml(const HostLoadRequest& request, QString& text) {
  NativeBinding binding{"unreadable"};
  const QFileInfo info(request.scenesXml);
  if (info.isSymLink() || !info.isFile()) {
    binding.cause = "not_a_file";
    return binding;
  }
  if (info.size() > ScenesXmlLimitBytes) {
    binding.cause = "too_large";
    return binding;
  }
  if (fileRevision(readBytes(request.scenesXml, ScenesXmlLimitBytes)) !=
      request.scenesXmlRevision)
    changed(request.scenesXml);
  std::string type;
  try {
    TIStream is(TFilePath(request.scenesXml.toStdWString()));
    std::string tag;
    if (!is || !is.matchTag(tag)) throw TException("no root tag");
    type = is.getTagAttribute("type");
    TFilePath folder;
    is >> folder;
    is.matchEndTag();
    text = type.empty() ? QString("..") : folder.getQString();
  } catch (...) {
    binding.cause = "rejected";
    return binding;
  }
  if (fileRevision(readBytes(request.scenesXml, ScenesXmlLimitBytes)) !=
      request.scenesXmlRevision)
    changed(request.scenesXml);
  binding.status = type.empty() || type == "projectFolder" ? "" : "name";
  return binding;
}

NativeBinding nativeBinding(Context& context, const HostLoadRequest& request) {
  if (request.scenesXml.isEmpty()) return {"none_within_roots"};
  QString text;
  NativeBinding binding = parseScenesXml(request, text);
  if (binding.status == "unreadable") return binding;
  const QString base   = QFileInfo(request.scenesXml).absolutePath();
  const QString folder = QDir::cleanPath(
      QDir::isAbsolutePath(text) ? text : QDir(base).filePath(text));
  if (binding.status == "name" && !QDir::isAbsolutePath(text) &&
      !text.contains('/'))
    return {"unresolved_name"};
  if (!insideRoots(request, folder)) return {"outside_roots"};
  const QString resolved = authorized(context, request, folder);
  if (resolved.isEmpty()) return {"outside_roots"};
  const QString file =
      TProjectManager::instance()
          ->projectFolderToProjectPath(TFilePath(resolved.toStdWString()))
          .getQString();
  const QFileInfo project(file);
  if (!project.isFile() || project.isSymLink())
    return {"unreadable", QString(), QString(), "no_project_file"};
  const QString canonical = project.canonicalFilePath();
  binding.projectFile     = canonical;
  binding.projectId       = projectIdForFile(canonical);
  binding.status =
      binding.projectId == request.projectId ? "matches" : "differs";
  return binding;
}

void requireBinding(const HostLoadRequest& request,
                    const NativeBinding& binding) {
  if (request.binding == "override") return;
  if (binding.status == "matches") return;
  if (request.projectId == "standalone" &&
      binding.status == "none_within_roots")
    return;
  throw Error("project_mismatch",
              "The selected project differs from the scene's native binding; "
              "select the native project or pass project_binding override.",
              {{"native_binding", binding.status},
               {"native_project_id", binding.projectId.isEmpty()
                                         ? QJsonValue()
                                         : QJsonValue(binding.projectId)},
               {"native_project_file", binding.projectFile.isEmpty()
                                           ? QJsonValue()
                                           : QJsonValue(binding.projectFile)},
               {"cause", binding.cause.isEmpty() ? QJsonValue()
                                                 : QJsonValue(binding.cause)}});
}

std::shared_ptr<TProject> loadProject(const HostLoadRequest& request) {
  auto project = std::make_shared<TProject>();
  if (request.projectFile.isEmpty()) return project;
  const QFileInfo info(request.projectFile);
  if (!info.isFile() || info.isSymLink())
    throw Error("invalid_project", "The project file is missing.",
                {{"cause", "missing"}});
  const QString picked = TProjectManager::instance()
                             ->projectFolderToProjectPath(
                                 TFilePath(info.absolutePath().toStdWString()))
                             .getQString();
  if (picked != request.projectFile)
    throw Error("invalid_project",
                "The project folder now selects a different project file.",
                {{"cause", "ambiguous"}, {"native_project_file", picked}});
  try {
    project->load(TFilePath(request.projectFile.toStdWString()));
  } catch (const TException& error) {
    throw Error("invalid_project", "The native project parser rejected it.",
                {{"cause", "rejected"},
                 {"native_message",
                  QString::fromStdWString(error.getMessage()).left(256)}});
  } catch (...) {
    throw Error("invalid_project", "The native project parser rejected it.",
                {{"cause", "rejected"}});
  }
  if (fileRevision(readBytes(request.projectFile, ProjectFileLimitBytes)) !=
      request.projectRevision)
    changed(request.projectFile);
  return project;
}

void applyPathRules(TProject& project) {
  FilePathProperties* rules = project.getFilePathProperties();
  TFilePath::setFilePathProperties(
      rules->useStandard(), rules->acceptNonAlphabetSuffix(),
      rules->letterCountForSuffix(), rules->noSeparatorFormatAllowed());
}

QJsonObject pathRules(TProject& project) {
  FilePathProperties* rules = project.getFilePathProperties();
  return {{"use_standard", rules->useStandard()},
          {"accept_non_alphabet_suffix", rules->acceptNonAlphabetSuffix()},
          {"letter_count_for_suffix", rules->letterCountForSuffix()},
          {"no_separator_format_allowed", rules->noSeparatorFormatAllowed()}};
}

std::unique_ptr<ToonzScene> parseScene(const ReadOnlyScene& source) {
  auto scene = std::make_unique<ToonzScene>();
  scene->setProject(source.project);
  try {
    scene->loadTnzFile(TFilePath(source.sceneCopy.toStdWString()));
  } catch (const TException& error) {
    throw Error("invalid_scene", "The native scene parser rejected the file.",
                {{"cause", "rejected"},
                 {"native_message",
                  QString::fromStdWString(error.getMessage()).left(256)}});
  } catch (const std::exception& error) {
    throw Error(
        "invalid_scene", "The native scene parser rejected the file.",
        {{"cause", "rejected"},
         {"native_message", QString::fromUtf8(error.what()).left(256)}});
  } catch (...) {
    throw Error("invalid_scene", "The native scene parser rejected the file.",
                {{"cause", "rejected"}});
  }
  scene->setScenePath(TFilePath(source.scenePath.toStdWString()), true);
  scene->setProject(source.project);
  scene->getXsheet()->updateFrameCount();
  return scene;
}

// ---------------------------------------------------------------- levels

struct Loader final {
  Context& context;
  const HostLoadRequest& request;
  ReadOnlyScene& source;
  Document* document;
  ToonzScene& scene;
  int loaded   = 0;
  int failed   = 0;
  int drawings = 0;
  // Sequence frame files by folder, then by native level name.
  QHash<QString, QHash<QString, QStringList>> folders;

  void pin(const QString& path) { addSourcePin(source, path); }

  const QStringList& sequenceFiles(const TFilePath& level) {
    const QString folder = QFileInfo(level.getQString()).absolutePath();
    if (!folders.contains(folder)) {
      QHash<QString, QStringList>& names = folders[folder];
      const QFileInfoList entries        = QDir(folder).entryInfoList(
          QDir::Files | QDir::NoSymLinks, QDir::Name);
      for (const QFileInfo& entry : entries) {
        const TFilePath member(entry.absoluteFilePath().toStdWString());
        names[QString::fromStdWString(member.getLevelNameW())].append(
            entry.absoluteFilePath());
      }
    }
    static const QStringList none;
    const auto& names = folders[folder];
    const auto found =
        names.constFind(QString::fromStdWString(level.getLevelNameW()));
    return found == names.constEnd() ? none : found.value();
  }

  void row(TXshLevel* level, const QString& reason, const QString& detail,
           ContentRow::Rule rule = ContentRow::Rule::Rendered,
           const QString& kind   = "level") {
    if (!document) return;
    ContentRow entry;
    entry.kind    = kind;
    entry.reason  = reason;
    entry.name    = QString::fromStdWString(level->getName());
    entry.detail  = detail;
    entry.levelId = document->idForLevel(level);
    entry.level   = level;
    entry.rule    = rule;
    source.rows.push_back(entry);
  }
};

QString levelKind(TXshLevel* level) {
  switch (level->getType()) {
  case PLI_XSHLEVEL:
    return "vector";
  case TZP_XSHLEVEL:
    return "toonz_raster";
  case OVL_XSHLEVEL:
    return "raster";
  case TZI_XSHLEVEL:
    return "scan_raster";
  case CHILD_XSHLEVEL:
    return "sub_xsheet";
  case SND_XSHLEVEL:
    return "sound";
  case SND_TXT_XSHLEVEL:
    return "sound_text";
  case PLT_XSHLEVEL:
    return "palette";
  case MESH_XSHLEVEL:
    return "mesh";
  case ZERARYFX_XSHLEVEL:
    return "generator";
  default:
    return "unknown";
  }
}

bool renderedFormat(const QString& kind, const QString& type) {
  static const QStringList raster = {"png", "tif", "tiff", "tga",
                                     "bmp", "jpg", "jpeg", "tzi"};
  if (kind == "vector") return type == "pli";
  if (kind == "toonz_raster") return type == "tlv";
  if (kind == "raster" || kind == "scan_raster") return raster.contains(type);
  return false;
}

bool knownFormat(const QString& type) {
  static const QStringList known = {
      "psd", "mp4", "mov",  "avi", "webm", "gif", "exr",  "svg", "pli", "tlv",
      "png", "tif", "tiff", "tga", "bmp",  "jpg", "jpeg", "tzi", "tzp", "tzu"};
  return known.contains(type);
}

void pinLevelFiles(Loader& loader, const TFilePath& decoded) {
  const QString path = decoded.getQString();
  if (decoded.isLevelName()) {
    loader.source.pinnedPaths.insert(path);
    for (const QString& file : loader.sequenceFiles(decoded)) loader.pin(file);
  } else {
    loader.pin(path);
  }
  if (decoded.getType() == "tlv") {
    const QString palette = decoded.withNoFrame().withType("tpl").getQString();
    if (QFileInfo(palette).isFile()) loader.pin(palette);
  }
  const TFilePath hook = TXshSimpleLevel::getExistingHookFile(decoded);
  if (!hook.isEmpty()) loader.pin(hook.getQString());
}

// Reader pre-check: refuses before native load() can open a modal dialog or
// read a file outside the roots. Returns the load status, or empty to load.
QString precheck(Loader& loader, TXshSimpleLevel* level,
                 const TFilePath& decoded) {
  TLevelReaderP reader(decoded);
  if (!reader) return "failed";
  TLevelP info;
  try {
    info = reader->loadInfo();
  } catch (...) {
    loader.row(level, "level_load_failed", "The level reader rejected it.");
    return "failed";
  }
  if (!info) return "failed";
  if (info->isPartialLoad()) {
    loader.row(level, "partial_level", "Not all frames were found.");
    return "partial";
  }
  if (info->getFrameCount() > 0) {
    const TImageInfo* image = reader->getImageInfo(info->begin()->first);
    if (image && image->m_samplePerPixel >= 5) {
      loader.row(level, "unsupported_samples",
                 "More than four samples per pixel.");
      return "unsupported_samples";
    }
    if (image && (image->m_lx > FirstFrameAxisLimit ||
                  image->m_ly > FirstFrameAxisLimit ||
                  qint64(image->m_lx) * image->m_ly > FirstFramePixelLimit)) {
      loader.row(level, "image_too_large",
                 QString("First frame is %1 by %2 pixels.")
                     .arg(image->m_lx)
                     .arg(image->m_ly));
      return "failed";
    }
  }
  const TPalette* palette   = info->getPalette();
  const TFilePath reference = palette ? palette->getRefImgPath() : TFilePath();
  if (!reference.isEmpty()) {
    const QString allowed =
        authorized(loader.context, loader.request, reference.getQString());
    if (allowed.isEmpty()) {
      loader.row(level, "external_file_reference",
                 "The palette names a reference image outside the read roots.",
                 ContentRow::Rule::Anywhere);
      return "forbidden";
    }
    if (QFileInfo(allowed).isFile()) loader.pin(allowed);
  }
  return QString();
}

QString locate(Loader& loader, TXshSimpleLevel* level, TFilePath& decoded) {
  decoded            = loader.scene.decodeFilePath(level->getPath());
  const QString kind = levelKind(level);
  const QString type = QString::fromStdString(decoded.getType()).toLower();
  if (!renderedFormat(kind, type)) {
    loader.row(level,
               knownFormat(type) ? "unsupported_level_format" : "unknown_level",
               "Format ." + type + " is outside this profile.");
    return "not_applicable";
  }
  if (!decoded.isAbsolute()) {
    loader.row(level, "missing_file", "The level path does not resolve.");
    return "missing";
  }
  if (authorized(loader.context, loader.request, decoded.getQString())
          .isEmpty()) {
    loader.row(level, "forbidden_file",
               "The level file is outside the read roots.");
    return "forbidden";
  }
  if (!TSystem::doesExistFileOrLevel(decoded)) {
    loader.row(level, "missing_file", "The level file does not exist.");
    return "missing";
  }
  const TFilePath scanned = level->getScannedPath();
  if (!scanned.isEmpty()) {
    const TFilePath path = loader.scene.decodeFilePath(scanned);
    if (authorized(loader.context, loader.request, path.getQString())
            .isEmpty()) {
      loader.row(level, "forbidden_file",
                 "The scanned source is outside the read roots.");
      return "forbidden";
    }
    if (TSystem::doesExistFileOrLevel(path)) pinLevelFiles(loader, path);
  }
  return QString();
}

// Texture styles name image files; custom ones decode against the scene.
void classifyStyles(Loader& loader, TXshSimpleLevel* level) {
  TPalette* palette = level->getPalette();
  if (!palette) return;
  for (int id = 0; id < palette->getStyleCount(); ++id) {
    TColorStyle* style = palette->getStyle(id);
    for (int param = 0; style && param < style->getParamCount(); ++param) {
      if (style->getParamType(param) != TColorStyle::FILEPATH) continue;
      const TFilePath path =
          style->getParamValue(TColorStyle::TFilePath_tag(), param);
      if (path.isEmpty()) continue;
      if (path.getParentDir().isEmpty()) {
        loader.row(
            level, "library_resource_unavailable",
            "Style " + QString::number(id) + " uses a shipped library texture.",
            ContentRow::Rule::Rendered, "style");
        continue;
      }
      const QString allowed =
          authorized(loader.context, loader.request,
                     loader.scene.decodeFilePath(path).getQString());
      if (allowed.isEmpty())
        loader.row(level, "external_file_reference",
                   "Style " + QString::number(id) +
                       " names a texture outside the read roots.",
                   ContentRow::Rule::Anywhere, "style");
      else if (QFileInfo(allowed).isFile())
        loader.pin(allowed);
    }
  }
}

LevelState loadSimpleLevel(Loader& loader, TXshSimpleLevel* level) {
  LevelState state{levelKind(level), QString(), 0};
  state.firstPin = int(loader.source.pins.size());
  state.endPin   = state.firstPin;
  TFilePath decoded;
  state.loadStatus = locate(loader, level, decoded);
  if (state.loadStatus.isEmpty())
    state.loadStatus = precheck(loader, level, decoded);
  if (!state.loadStatus.isEmpty()) return state;
  pinLevelFiles(loader, decoded);
  state.endPin = int(loader.source.pins.size());
  try {
    level->load();
  } catch (...) {
    loader.row(level, "level_load_failed", "The native level load failed.");
    state.loadStatus = "failed";
    return state;
  }
  if (level->getProperties()->isForbidden()) {
    loader.row(level, "creator_forbidden",
               "The file was written by an incompatible creator.");
    state.loadStatus = "creator_forbidden";
    return state;
  }
  state.loadStatus = "loaded";
  classifyStyles(loader, level);
  state.bitDepth = level->isFloatChannelLevel()   ? 32
                   : level->is16BitChannelLevel() ? 16
                                                  : 8;
  loader.drawings += level->getFrameCount();
  if (loader.drawings > DrawingLimit)
    throw Error("scene_limit", "The scene exceeds 1,000,000 drawings.",
                {{"limit", "drawings"}, {"value", loader.drawings}});
  return state;
}

LevelState loadLevel(Loader& loader, TXshLevel* level) {
  LevelState state{levelKind(level), "not_applicable", 0};
  if (state.kind == "mesh")
    loader.row(level, "unsupported_level_format",
               "Mesh levels are outside this profile.");
  if (state.kind == "sound") {
    const TFilePath path = loader.scene.decodeFilePath(level->getPath());
    const QString allowed =
        authorized(loader.context, loader.request, path.getQString());
    if (!allowed.isEmpty() && QFileInfo(allowed).isFile()) loader.pin(allowed);
    return state;
  }
  TXshSimpleLevel* simple = level->getSimpleLevel();
  if (!simple || state.kind == "mesh") return state;
  return loadSimpleLevel(loader, simple);
}

void primeRasterPalette(Loader& loader) {
  auto project = loader.scene.getProject();
  if (!project || project->getFolderIndex("palettes") < 0) {
    FullColorPalette::instance()->getPalette(nullptr);
    return;
  }
  QStringList candidates;
  for (const char* name : {"+palettes/Raster_Drawing_Palette.tpl",
                           "+palettes/fullcolorPalette.tpl"})
    candidates.append(
        loader.scene.decodeFilePath(TFilePath(name)).getQString());
  for (const QString& candidate : candidates)
    if (authorized(loader.context, loader.request, candidate).isEmpty()) {
      FullColorPalette::instance()->getPalette(nullptr);
      return;
    }
  FullColorPalette::instance()->getPalette(&loader.scene);
  for (const QString& candidate : candidates)
    if (QFileInfo(candidate).isFile()) loader.pin(candidate);
}

void loadLevels(Loader& loader) {
  TLevelSet* set = loader.scene.getLevelSet();
  if (set->getLevelCount() > LevelLimit)
    throw Error("scene_limit", "The scene exceeds 4096 levels.",
                {{"limit", "levels"}, {"value", set->getLevelCount()}});
  loader.scene.setIsLoading(true);
  primeRasterPalette(loader);
  for (int index = 0; index < set->getLevelCount(); ++index) {
    TXshLevel* level       = set->getLevel(index);
    const LevelState state = loadLevel(loader, level);
    if (state.loadStatus == "loaded") ++loader.loaded;
    if (state.loadStatus != "loaded" && state.loadStatus != "not_applicable")
      ++loader.failed;
    loader.source.levels.insert(level, state);
  }
  loader.scene.setIsLoading(false);
  loader.scene.getXsheet()->updateFrameCount();
}

// ---------------------------------------------------------------- report

QJsonObject folderRow(Context& context, const HostLoadRequest& request,
                      ToonzScene& scene, TProject& project, int index) {
  const std::string name = project.getFolderName(index);
  const TFilePath decoded =
      scene.decodeFilePath(TFilePath("+" + name) + TFilePath("x"))
          .getParentDir();
  QJsonValue resolved, exists;
  QString authority = "unresolved";
  if (decoded.isAbsolute()) {
    const QString allowed = authorized(context, request, decoded.getQString());
    authority             = "outside_roots";
    if (!allowed.isEmpty()) {
      const QString workspace = request.workerWorkspace;
      authority = allowed == workspace || allowed.startsWith(workspace + "/")
                      ? "workspace"
                      : "input_root";
      resolved  = allowed;
      exists    = QFileInfo(allowed).exists();
    }
  }
  return {{"alias", QString::fromStdString(name).left(64)},
          {"coded_path", project.getFolder(index).getQString().left(4096)},
          {"scene_dependent", !project.isConstantFolder(index)},
          {"use_scene_path", project.getUseScenePath(name)},
          {"resolved_path", resolved},
          {"authority", authority},
          {"exists", exists}};
}

QJsonObject projectReport(Context& context, const HostLoadRequest& request,
                          const NativeBinding& binding, ToonzScene& scene) {
  TProject& project = *scene.getProject();
  QJsonArray folders;
  for (int index = 0; index < project.getFolderCount() && index < 64; ++index)
    folders.append(folderRow(context, request, scene, project, index));
  const bool standalone = request.projectId == "standalone";
  QString mode          = standalone ? "standalone" : "native";
  if (request.binding == "override") mode = "override";
  const auto optional = [](const QString& value) {
    return value.isEmpty() ? QJsonValue() : QJsonValue(value);
  };
  return {{"project_id", request.projectId},
          {"name", standalone ? QJsonValue()
                              : optional(QString::fromStdWString(
                                             project.getName().getWideString())
                                             .left(256))},
          {"project_file", optional(request.projectFile)},
          {"project_file_revision", optional(request.projectRevision)},
          {"binding", mode},
          {"native_binding",
           QJsonObject{{"status", binding.status},
                       {"project_id", optional(binding.projectId)},
                       {"scenes_xml", optional(request.scenesXml)}}},
          {"file_path_rules", pathRules(project)},
          {"folders", folders}};
}

QJsonObject sourceReport(ReadOnlyScene& source, ToonzScene& scene) {
  QJsonArray tags;
  const auto& unrecognized = scene.getUnrecognizedSceneTags();
  for (const std::string& tag : unrecognized)
    if (tags.size() < 32) tags.append(QString::fromStdString(tag).left(128));
  const auto version = scene.getVersionNumber();
  return {{"scene_path", source.scenePath},
          {"scene_file_revision", source.sceneRevision},
          {"size_bytes", double(QFileInfo(source.sceneCopy).size())},
          {"scene_version",
           QString("%1.%2").arg(version.first).arg(version.second)},
          {"unrecognized_scene_tags", tags},
          {"unrecognized_tag_count", int(unrecognized.size())},
          {"source_fingerprint", source.fingerprint},
          {"pinned_file_count", int(source.pins.size())}};
}

QJsonObject contentReport(const ReadOnlyScene& source) {
  QJsonArray rows;
  for (const ContentRow& row : source.rows)
    if (rows.size() < UnsupportedRowLimit) rows.append(row.unsupportedRow());
  return {{"unsupported", rows},
          {"unsupported_count", int(source.rows.size())},
          {"truncated", int(source.rows.size()) > UnsupportedRowLimit}};
}

void pinSources(Loader& loader, const HostLoadRequest& request) {
  loader.pin(request.scenePath);
  if (!request.projectFile.isEmpty()) loader.pin(request.projectFile);
  if (!request.scenesXml.isEmpty() && QFileInfo(request.scenesXml).isFile())
    loader.pin(request.scenesXml);
}

void requireSceneRevision(const HostLoadRequest& request) {
  if (fileRevision(readBytes(request.scenePath, SceneFileLimitBytes)) !=
      request.sceneRevision)
    changed(request.scenePath);
}

std::shared_ptr<ReadOnlyScene> newSource(const HostLoadRequest& request,
                                         std::shared_ptr<TProject> project) {
  auto source             = std::make_shared<ReadOnlyScene>();
  source->projectId       = request.projectId;
  source->scenePath       = request.scenePath;
  source->sceneCopy       = request.sceneCopy;
  source->sceneRevision   = request.sceneRevision;
  source->readRoots       = request.readRoots;
  source->workerWorkspace = request.workerWorkspace;
  source->project         = std::move(project);
  source->revisions = QStringList{"scene " + request.sceneRevision,
                                  "project " + request.projectRevision,
                                  "scenes_xml " + request.scenesXmlRevision};
  return source;
}

HostLoadRequest storedRequest(const ReadOnlyScene& source) {
  HostLoadRequest request;
  request.scenePath       = source.scenePath;
  request.sceneCopy       = source.sceneCopy;
  request.readRoots       = source.readRoots;
  request.workerWorkspace = source.workerWorkspace;
  return request;
}

QJsonObject openReport(Context& context, const HostLoadRequest& request,
                       const NativeBinding& binding, Document& document,
                       const Loader& loader) {
  ReadOnlyScene& source = *document.readOnly;
  QJsonObject counts    = readOnlyCounts(document);
  counts.insert("drawings", loader.drawings);
  return {
      {"document_id", document.id},
      {"revision", "0"},
      {"access", "read_only"},
      {"profile", readOnlyProfileName()},
      {"source", sourceReport(source, *document.scene)},
      {"project", projectReport(context, request, binding, *document.scene)},
      {"counts", counts},
      {"content", contentReport(source)},
      {"render", renderReport(document)},
      {"host_state", "ready"},
      {"native_project_id", binding.projectId},
      {"native_project_file", binding.projectFile}};
}

QJsonValue projectVersion(const QString& projectFile) {
  try {
    TIStream is(TFilePath(projectFile.toStdWString()));
    std::string tag;
    if (!is || !is.matchTag(tag) || tag != "project") return QJsonValue();
    while (is.matchTag(tag)) {
      if (tag != "version") {
        is.skipCurrentTag();
        continue;
      }
      int major = 0, minor = 0;
      is >> major >> minor;
      return QString("%1.%2").arg(major).arg(minor);
    }
  } catch (...) {
  }
  return QJsonValue();
}

QJsonValue sceneDefaults(TProject& project) {
  const TSceneProperties& properties = project.getSceneProperties();
  if (properties.getCameras().empty()) return QJsonValue();
  const TCamera* camera = properties.getCameras().front();
  return QJsonObject{
      {"camera_width_px", camera->getRes().lx},
      {"camera_height_px", camera->getRes().ly},
      {"camera_width_in", camera->getSize().lx},
      {"camera_height_in", camera->getSize().ly},
      {"frame_rate", properties.getOutputProperties()->getFrameRate()}};
}

QJsonArray projectFolders(TProject& project) {
  QJsonArray folders;
  for (int index = 0; index < project.getFolderCount() && index < 64; ++index) {
    const std::string name = project.getFolderName(index);
    const bool dependent   = !project.isConstantFolder(index);
    const TFilePath coded  = project.getFolder(index);
    folders.append(QJsonObject{
        {"alias", QString::fromStdString(name).left(64)},
        {"coded_path", coded.getQString().left(4096)},
        {"scene_dependent", dependent},
        {"use_scene_path", project.getUseScenePath(name)},
        {"decoded",
         dependent ? QString() : project.decode(coded).getQString()}});
  }
  return folders;
}

}  // namespace

QJsonObject nativeProjectSummary(const QString& projectFile) {
  TProject project;
  project.load(TFilePath(projectFile.toStdWString()));
  return {
      {"name",
       QString::fromStdWString(project.getName().getWideString()).left(256)},
      {"version", projectVersion(projectFile)},
      {"file_path_rules", pathRules(project)},
      {"folders", projectFolders(project)},
      {"folder_count", project.getFolderCount()},
      {"scene_defaults", sceneDefaults(project)}};
}

QJsonObject loadReadOnlyDocument(Context& context, const QString& documentId,
                                 const HostLoadRequest& request) {
  QElapsedTimer timer;
  timer.start();
  const NativeBinding binding = nativeBinding(context, request);
  requireBinding(request, binding);
  auto project = loadProject(request);
  applyPathRules(*project);
  requireSceneRevision(request);
  auto source = newSource(request, project);
  auto scene  = parseScene(*source);
  requireReadOnlyStructure(*scene);
  Document& document =
      context.addReadOnlyDocument(std::move(scene), documentId, source);
  try {
    Loader loader{context, request, *source, &document, *document.scene};
    loadLevels(loader);
    classifyReadOnlyContent(document);
    pinSources(loader, request);
    source->fingerprint = sourceFingerprint(source->revisions, source->pins);
    QJsonObject report =
        openReport(context, request, binding, document, loader);
    report.insert(
        "load",
        QJsonObject{
            {"levels_loaded", loader.loaded},
            {"levels_failed", loader.failed},
            {"elapsed_ms", std::min<qint64>(timer.elapsed(), 60000)},
            {"host_memory_bytes",
             double(std::max<qint64>(
                 0, processMemoryBytes(QCoreApplication::applicationPid())))}});
    return report;
  } catch (...) {
    context.removeDocument(documentId);
    throw;
  }
}

std::unique_ptr<ToonzScene> loadReadOnlySnapshot(Context& context,
                                                 Document& document) {
  ReadOnlyScene& stored = *document.readOnly;
  ReadOnlyScene copy;
  copy.scenePath                = stored.scenePath;
  copy.sceneCopy                = stored.sceneCopy;
  copy.project                  = stored.project;
  const HostLoadRequest request = storedRequest(stored);
  auto scene                    = parseScene(copy);
  TImageStyle::setCurrentScene(document.scene.get());
  Loader loader{context, request, copy, nullptr, *scene};
  loadLevels(loader);
  return scene;
}

QJsonValue executeSceneHost(Context& context, const QString& method,
                            const QJsonObject& params) {
  if (method == "scene_host.load") {
    HostLoadRequest request;
    request.scenePath         = params.value("scene_path").toString();
    request.sceneCopy         = params.value("scene_copy").toString();
    request.sceneRevision     = params.value("scene_revision").toString();
    request.projectId         = params.value("project_id").toString();
    request.projectFile       = params.value("project_file").toString();
    request.projectRevision   = params.value("project_revision").toString();
    request.scenesXml         = params.value("scenes_xml").toString();
    request.scenesXmlRevision = params.value("scenes_xml_revision").toString();
    request.binding           = params.value("binding").toString();
    request.workerWorkspace   = params.value("worker_workspace").toString();
    for (const QJsonValue& root : params.value("read_roots").toArray())
      request.readRoots.append(root.toString());
#ifdef BUILD_UI_PORT_TESTS
    injectChildFault(context.childFault);
#endif
    return loadReadOnlyDocument(context, context.sceneHostDocument, request);
  }
  Document& document = context.document(context.sceneHostDocument);
  if (method == "scene_host.check_sources") {
    requireUnchangedSources(document.readOnly->pins);
    return QJsonObject{{"state", "unchanged"},
                       {"source_fingerprint", document.readOnly->fingerprint}};
  }
  if (method == "scene_host.close") {
    const QString job = activeRenderJob(context);
    if (!job.isEmpty())
      throw Error("render_busy",
                  "Wait for the document's render job to finish or cancel it.",
                  {{"job_id", job}});
    return QJsonObject{{"closing", true}};
  }
  throw Error("method_not_found", "Unknown scene host operation.");
}

void startSceneHostWatchdog() {
  const qint64 process = QCoreApplication::applicationPid();
#ifdef _WIN32
  // The worker holds the host in a job that ends it with the worker.
  std::thread([process] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (processMemoryBytes(process) > SceneHostMemoryBytes)
        std::_Exit(HostMemoryExit);
    }
  }).detach();
#else
  // The worker names itself: a worker that died before this point has
  // already left the host with another parent.
  bool named         = false;
  const pid_t worker = pid_t(
      qEnvironmentVariableIntValue("GROUNDZERO_SCENE_HOST_PARENT", &named));
  const pid_t parent = named ? worker : getppid();
  std::thread([process, parent] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      if (processMemoryBytes(process) > SceneHostMemoryBytes)
        std::_Exit(HostMemoryExit);
      // A killed worker cannot close our input while a native load blocks
      // the event loop.
      if (getppid() != parent) std::_Exit(HostOrphanExit);
    }
  }).detach();
#endif
}

}  // namespace GroundzeroAutomation
