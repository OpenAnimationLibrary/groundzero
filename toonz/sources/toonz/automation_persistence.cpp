#include "automation_hook.h"
#include "automation_path.h"
#include "automation_rig.h"
#include "automation_vector_opacity.h"
#include "automation.h"
#include "automation_files.h"
#include "automation_jobs.h"
#include "automation_journal.h"
#include "automation_audio.h"
#include "automation_drawing.h"
#include "automation_fx.h"
#include "automation_macro.h"
#include "automation_sheet_record.h"
#include "automation_storyboard.h"

#include "timage.h"
#include "tcolorstyles.h"
#include "tlevel_io.h"
#include "tpalette.h"
#include "trasterimage.h"
#include "ttoonzimage.h"
#include "toonz/levelset.h"
#include "toonz/levelproperties.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "texception.h"
#include "toutputproperties.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxLevels          = 256;
constexpr int MaxDrawings        = 4096;
constexpr int MaxFiles           = 8192;
constexpr qint64 MaxPackageBytes = 256 * 1024 * 1024;
constexpr int MaxPackages        = 128;

struct Package final {
  QJsonObject manifest;
  QByteArray manifestHash;
  QByteArray macroFingerprint;
  // The exact manifest.json bytes, and for a publication the registration
  // its intent carries (empty without a journal).
  QByteArray manifestBytes;
  QJsonObject registration;
};

constexpr qint64 MaxManifestBytes = 2 * 1024 * 1024;

std::map<QString, Package> publishedPackages;

QByteArray digest(const QByteArray& bytes) {
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool isPackageRelativePath(const QString& relative) {
  return !relative.isEmpty() && !QDir::isAbsolutePath(relative) &&
         QDir::cleanPath(relative) == relative && !relative.startsWith("../");
}

bool isOwnedPackageFile(const QFileInfo& info, const QString& path) {
  return info.canonicalFilePath() == QDir::cleanPath(path) && info.isFile() &&
         !info.isSymLink() && info.size() <= MaxPackageBytes;
}

QByteArray readRegularFile(const QString& directory, const QString& relative) {
  const QString path = QDir(directory).absoluteFilePath(relative);
  const QFileInfo info(path);
  if (!isPackageRelativePath(relative) || !isOwnedPackageFile(info, path))
    throw Error("invalid_package",
                "A package file is missing or outside its directory.",
                {{"file", relative}});
  QFile file;
  if (openApprovedFile(file, path) != OpenResult::Opened)
    throw Error("package_read_failed", "Cannot open a package file.",
                {{"file", relative}});
  const QByteArray bytes = file.read(MaxPackageBytes + 1);
  if (file.error() != QFileDevice::NoError || bytes.size() != info.size())
    throw Error("package_read_failed", "Cannot read a complete package file.",
                {{"file", relative}});
  return bytes;
}

// Writes one file of a private copy, creating its folders.
void writePrivateFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  if (!QDir().mkpath(QFileInfo(path).absolutePath()) ||
      !file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      file.write(bytes) != bytes.size())
    throw Error("package_read_failed", "Cannot copy a package file privately.");
}

QJsonArray fileManifest(const QString& directory) {
  QJsonArray result;
  qint64 totalBytes = 0;
  QDirIterator iterator(directory, QDir::Files | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const QString path     = iterator.next();
    const QString relative = QDir(directory).relativeFilePath(path);
    if (result.size() >= MaxFiles)
      throw Error("package_limit", "The package contains too many files.");
    const QByteArray bytes = readRegularFile(directory, relative);
    totalBytes += bytes.size();
    if (totalBytes > MaxPackageBytes)
      throw Error("package_limit", "The package exceeds 256 MiB.");
    result.append(QJsonObject{{"path", relative},
                              {"size_bytes", double(bytes.size())},
                              {"sha256", QString::fromLatin1(digest(bytes))}});
  }
  return result;
}

void verifyFiles(const QString& directory, const Package& package) {
  if (digest(readRegularFile(directory, "manifest.json")) !=
      package.manifestHash)
    throw Error("invalid_package",
                "The package manifest changed after publication.");
  QSet<QString> expected;
  for (const QJsonValue& value : package.manifest.value("files").toArray()) {
    const QJsonObject entry = value.toObject();
    const QString relative  = entry.value("path").toString();
    const QByteArray bytes  = readRegularFile(directory, relative);
    if (bytes.size() != entry.value("size_bytes").toDouble() ||
        QString::fromLatin1(digest(bytes)) != entry.value("sha256").toString())
      throw Error("invalid_package",
                  "A package file changed after publication.",
                  {{"file", relative}});
    expected.insert(relative);
  }
  expected.insert("manifest.json");
  QDirIterator iterator(directory, QDir::Files | QDir::Hidden | QDir::System,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext())
    if (!expected.remove(QDir(directory).relativeFilePath(iterator.next())))
      throw Error("invalid_package",
                  "The package contains an unrecorded file.");
  if (!expected.isEmpty())
    throw Error("invalid_package", "The package is missing a recorded file.");
}

QString levelPath(int index, int type) {
  const QString prefix = "assets/level-" + QString::number(index);
  if (type == PLI_XSHLEVEL) return prefix + ".pli";
  if (type == TZP_XSHLEVEL) return prefix + ".tlv";
  if (type == OVL_XSHLEVEL) return prefix + "..png";
  throw Error("unsupported_level",
              "Only vector, Toonz raster, and RGBA drawings can be saved.");
}

QJsonArray frameManifest(TXshSimpleLevel& level) {
  QJsonArray result;
  for (const TFrameId& frame : level.getFids())
    result.append(QJsonObject{{"number", frame.getNumber()},
                              {"suffix", frame.getLetter()}});
  return result;
}

QJsonObject styleManifest(const TColorStyle& style) {
  const TPixel32 color = style.getMainColor();
  return {{"name", QString::fromStdWString(style.getName())},
          {"color", QJsonArray{color.r, color.g, color.b, color.m}}};
}

QJsonValue paletteManifest(TXshSimpleLevel& level) {
  TPalette* palette = level.getPalette();
  if (level.getType() == OVL_XSHLEVEL || !palette) return QJsonValue();
  QJsonArray styles;
  for (int index = 0; index < palette->getStyleCount(); ++index) {
    QJsonObject style = styleManifest(*palette->getStyle(index));
    QJsonArray keys;
    for (const auto& key : palette->getStyleAnimation(index)) {
      QJsonObject value = styleManifest(*key.second.style);
      value.insert("frame", key.first);
      value.insert("timing", int(key.second.interpolation.timing));
      value.insert("space", int(key.second.interpolation.space));
      keys.append(value);
    }
    style.insert("keys", keys);
    styles.append(style);
  }
  QJsonArray pages;
  for (int index = 0; index < palette->getPageCount(); ++index) {
    const TPalette::Page* page = palette->getPage(index);
    QJsonArray ids;
    for (int position = 0; position < page->getStyleCount(); ++position)
      ids.append(page->getStyleId(position));
    pages.append(QJsonObject{{"name", QString::fromStdWString(page->getName())},
                             {"styles", ids}});
  }
  return QJsonObject{{"styles", styles}, {"pages", pages}};
}

TPointD frameDpi(TXshSimpleLevel& level) {
  TPointD dpi;
  const TImageP image = level.getFrame(level.getFirstFid(), false);
  if (const TRasterImageP fullcolor = image) fullcolor->getDpi(dpi.x, dpi.y);
  if (const TToonzImageP indexed = image) indexed->getDpi(dpi.x, dpi.y);
  return dpi;
}

QJsonObject rasterManifest(TXshSimpleLevel& level) {
  if (level.getType() == PLI_XSHLEVEL) return {};
  const LevelProperties* properties = level.getProperties();
  const TPointD imageDpi            = properties->getImageDpi();
  const TPointD effectiveDpi =
      properties->getDpiPolicy() == LevelProperties::DP_CustomDpi
          ? properties->getDpi()
          : imageDpi;
  const TDimension size    = properties->getImageRes();
  const TPointD drawingDpi = frameDpi(level);
  return {{"width", size.lx},
          {"height", size.ly},
          {"image_dpi_x", imageDpi.x},
          {"image_dpi_y", imageDpi.y},
          {"frame_dpi_x", drawingDpi.x},
          {"frame_dpi_y", drawingDpi.y},
          {"effective_dpi_x", effectiveDpi.x},
          {"effective_dpi_y", effectiveDpi.y}};
}

void writeLevel(TXshSimpleLevel& level, const QString& path, double frameRate) {
  TLevelWriterP writer(TFilePath(path.toStdWString()));
  if (!writer)
    throw Error("writer_unavailable",
                "The native level writer is unavailable.");
  TPaletteP palette;
  if (level.getPalette()) {
    palette = level.getPalette()->clone();
    writer->setPalette(palette.getPointer());
  }
  writer->setFrameRate(frameRate);
  for (const TFrameId& frame : level.getFids()) {
    const TImageP source = level.getFrame(frame, false);
    if (!source)
      throw Error("missing_drawing", "A drawing cannot be read while saving.",
                  {{"drawing", QString::fromStdString(frame.expand())}});
    const TImageP image(source->cloneImage());
    if (palette) image->setPalette(palette.getPointer());
    const TImageWriterP frameWriter = writer->getFrameWriter(frame);
    if (!frameWriter)
      throw Error("writer_unavailable",
                  "A native frame writer is unavailable.");
    frameWriter->save(image);
  }
  writer->finalize();
}

// The step of a package write in progress and the package-relative file it
// writes, named by any error that step raises.
struct SaveStep final {
  QString stage = "stage";
  QString file;
};

// A test write fault at `point` (package:level, ...): the step fails as a
// native writer would, with an error that does not name its step.
void packageFault(const Context& context, const char* point,
                  const SaveStep& step) {
  if (writeFaultFires(context, point))
    throw Error("package_write_failed", "Cannot write the package at its " +
                                            step.stage +
                                            " step; nothing was published.");
}

QJsonArray writeLevels(const Context& context, ToonzScene& scene,
                       const QString& directory, SaveStep& step) {
  QJsonArray entries;
  TLevelSet* levels = scene.getLevelSet();
  const double frameRate =
      scene.getProperties()->getOutputProperties()->getFrameRate();
  for (int index = 0; index < levels->getLevelCount(); ++index) {
    TXshSimpleLevel* level = levels->getLevel(index)->getSimpleLevel();
    if (!level) continue;  // Sound sources are copied by writeAutomationSounds.
    const QString relative = levelPath(index, level->getType());
    const QString file     = QDir(directory).absoluteFilePath(relative);
    step                   = {"level", relative};
    writeLevel(*level, file, frameRate);
    packageFault(context, "package:level", step);
    const QJsonValue hooks = writeAutomationHooks(*level, file);
    if (level->getType() == TZP_XSHLEVEL)
      readRegularFile(directory, relative.left(relative.size() - 3) + "tpl");
    entries.append(
        QJsonObject{{"name", QString::fromStdWString(level->getName())},
                    {"type", level->getType()},
                    {"path", relative},
                    {"stroke_opacity_sha256",
                     level->getType() == PLI_XSHLEVEL
                         ? QJsonValue(vectorLevelOpacityDigest(*level))
                         : QJsonValue()},
                    {"palette", paletteManifest(*level)},
                    {"raster", rasterManifest(*level)},
                    {"hooks_sha256", hooks},
                    {"drawings", frameManifest(*level)}});
  }
  return entries;
}

class PathRestore final {
  struct Entry final {
    TXshSimpleLevel* level;
    int index;
    TFilePath path;
    LevelProperties properties;
  };

public:
  explicit PathRestore(ToonzScene& scene) {
    TLevelSet* levels = scene.getLevelSet();
    for (int index = 0; index < levels->getLevelCount(); ++index) {
      TXshSimpleLevel* level = levels->getLevel(index)->getSimpleLevel();
      if (level)
        m_paths.push_back(
            {level, index, level->getPath(), *level->getProperties()});
    }
  }
  ~PathRestore() {
    for (const Entry& entry : m_paths) {
      entry.level->setPath(entry.path, true);
      *entry.level->getProperties() = entry.properties;
    }
  }
  void rebase() {
    for (const Entry& entry : m_paths) {
      TXshSimpleLevel* level = entry.level;
      const QString path =
          "$scenefolder/" + levelPath(entry.index, level->getType());
      level->setPath(TFilePath(path.toStdWString()), true);
      *level->getProperties() = entry.properties;
      if (level->getType() != PLI_XSHLEVEL) {
        const TPointD dpi =
            entry.properties.getDpiPolicy() == LevelProperties::DP_CustomDpi
                ? entry.properties.getDpi()
                : entry.properties.getImageDpi();
        level->getProperties()->setDpi(dpi);
        level->getProperties()->setDpiPolicy(LevelProperties::DP_CustomDpi);
      }
    }
  }

private:
  std::vector<Entry> m_paths;
};

// Level settings by name, read after the package paths and DPI are applied:
// what the saver writes, which differs from the document in DPI policy.
QJsonObject levelSettings(ToonzScene& scene) {
  QJsonObject settings;
  TLevelSet* cast = scene.getLevelSet();
  for (int index = 0; index < cast->getLevelCount(); ++index)
    if (TXshSimpleLevel* level = cast->getLevel(index)->getSimpleLevel())
      settings.insert(QString::fromStdWString(level->getName()),
                      automationLevelSettings(*level));
  return settings;
}

QJsonObject writeScene(ToonzScene& scene, const QString& directory) {
  PathRestore restore(scene);
  restore.rebase();
  const QJsonObject settings = levelSettings(scene);
  AutomationSoundPaths sounds(scene);
  scene.saveCopy(
      TFilePath(QDir(directory).absoluteFilePath("scene.tnz").toStdWString()));
  return settings;
}

QJsonArray withLevelSettings(const QJsonArray& levels,
                             const QJsonObject& settings) {
  QJsonArray result;
  for (const QJsonValue& value : levels) {
    QJsonObject entry = value.toObject();
    entry.insert("settings", settings.value(entry.value("name").toString()));
    result.append(entry);
  }
  return result;
}

Package writePackage(const Context& context, Document& document,
                     const QString& directory, SaveStep& step) {
  if (!QDir(directory).mkdir("assets"))
    throw Error("package_write_failed",
                "Cannot create the package assets directory.");
  const QByteArray macroFingerprint = automationMacroFingerprint(*document.scene, directory);
  const QJsonObject record = automationSheetRecord(*document.scene);
  const QJsonArray levels =
      writeLevels(context, *document.scene, directory, step);
  step                     = {"sounds", QString()};
  const QJsonObject sounds = writeAutomationSounds(*document.scene, directory);
  packageFault(context, "package:sounds", step);
  step                       = {"scene", "scene.tnz"};
  const QJsonObject settings = writeScene(*document.scene, directory);
  packageFault(context, "package:scene", step);
  step = {"manifest", "manifest.json"};
  Package package;
  package.macroFingerprint = macroFingerprint;
  package.manifest         = {{"version", 1},
                              {"profile", "worker-generated-v1"},
                              {"scene_file", "scene.tnz"},
                              {"source_revision", document.revisionText()},
                              {"levels", withLevelSettings(levels, settings)}};
  for (auto entry = record.begin(); entry != record.end(); ++entry)
    package.manifest.insert(entry.key(), entry.value());
  for (auto entry = sounds.begin(); entry != sounds.end(); ++entry)
    package.manifest.insert(entry.key(), entry.value());
  const QString tags = automationTagsDigest(*document.scene);
  if (!tags.isEmpty()) package.manifest.insert("tags_sha256", tags);
  package.manifest.insert("files", fileManifest(directory));
  const QByteArray bytes =
      QJsonDocument(package.manifest).toJson(QJsonDocument::Indented);
  QSaveFile file(QDir(directory).absoluteFilePath("manifest.json"));
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit())
    throw Error("package_write_failed", "Cannot publish the package manifest.");
  package.manifestHash  = digest(bytes);
  package.manifestBytes = bytes;
  packageFault(context, "package:manifest", step);
  return package;
}

// An error raised while a package is staged, with the step that raised it.
Error stepError(const QString& code, const QString& message,
                QJsonObject details, const SaveStep& step) {
  if (details.contains("stage")) return Error(code, message, details);
  details.insert("stage", step.stage);
  details.insert("file", step.file.isEmpty() ? QJsonValue() : step.file);
  details.insert("published", false);
  return Error(code, message, details);
}

std::vector<TFrameId> expectedFrames(const QJsonObject& entry) {
  std::vector<TFrameId> frames;
  for (const QJsonValue& value : entry.value("drawings").toArray()) {
    const QJsonObject drawing = value.toObject();
    frames.emplace_back(drawing.value("number").toInt(),
                        drawing.value("suffix").toString());
  }
  return frames;
}

void restoreRasterProperties(TXshSimpleLevel& level, const TImageInfo& info,
                             const QJsonObject& entry) {
  const QJsonObject raster = entry.value("raster").toObject();
  if (info.m_lx != raster.value("width").toInt() ||
      info.m_ly != raster.value("height").toInt())
    throw Error("invalid_package",
                "The saved raster dimensions differ from their source.");
  LevelProperties* properties = level.getProperties();
  properties->setImageRes(TDimension(info.m_lx, info.m_ly));
  properties->setImageDpi(TPointD(raster.value("image_dpi_x").toDouble(),
                                  raster.value("image_dpi_y").toDouble()));
  properties->setDpi(TPointD(raster.value("effective_dpi_x").toDouble(),
                             raster.value("effective_dpi_y").toDouble()));
  properties->setDpiPolicy(LevelProperties::DP_CustomDpi);
  properties->setBpp(info.m_bitsPerSample * info.m_samplePerPixel);
}

void restoreImageDpi(const TImageP& image, const QJsonObject& entry) {
  const QJsonObject raster = entry.value("raster").toObject();
  const double x           = raster.value("frame_dpi_x").toDouble();
  const double y           = raster.value("frame_dpi_y").toDouble();
  if (const TRasterImageP fullcolor = image) fullcolor->setDpi(x, y);
  if (const TToonzImageP indexed = image) indexed->setDpi(x, y);
}

TXshSimpleLevel* findSavedLevel(ToonzScene& scene, const QJsonObject& entry) {
  TXshLevel* native = scene.getLevelSet()->getLevel(
      entry.value("name").toString().toStdWString());
  TXshSimpleLevel* level = native ? native->getSimpleLevel() : nullptr;
  const TFilePath coded(
      ("$scenefolder/" + entry.value("path").toString()).toStdWString());
  if (!level || level->getType() != entry.value("type").toInt() ||
      level->getPath() != coded)
    throw Error("invalid_package",
                "The native level does not match the package manifest.");
  return level;
}

void restoreSavedRaster(TXshSimpleLevel& level, TLevelReaderP& reader,
                        const std::vector<TFrameId>& frames,
                        const QJsonObject& entry) {
  if (level.getType() == PLI_XSHLEVEL || frames.empty()) return;
  const TImageInfo* imageInfo = reader->getImageInfo(frames.front());
  if (!imageInfo)
    throw Error("invalid_package",
                "The saved raster level has no image metadata.");
  restoreRasterProperties(level, *imageInfo, entry);
}

void restoreSavedFrames(TXshSimpleLevel& level, TLevelReaderP& reader,
                        const TLevelP& info, const QJsonObject& entry) {
  for (const TFrameId& frame : expectedFrames(entry)) {
    if (info->getTable()->find(frame) == info->getTable()->end())
      throw Error("missing_drawing",
                  "A saved drawing ID is missing from its level.");
    const TImageReaderP frameReader = reader->getFrameReader(frame);
    if (!frameReader)
      throw Error("missing_drawing", "A saved drawing reader is unavailable.");
    const TImageP image = frameReader->load();
    if (!image)
      throw Error("missing_drawing",
                  "A saved drawing cannot be independently decoded.");
    restoreImageDpi(image, entry);
    level.setFrame(frame, image);
  }
}

void reopenLevel(ToonzScene& scene, const QJsonObject& entry) {
  TXshSimpleLevel* level             = findSavedLevel(scene, entry);
  const std::vector<TFrameId> frames = expectedFrames(entry);
  TLevelReaderP reader(scene.decodeFilePath(level->getPath()));
  if (!reader)
    throw Error("reader_unavailable", "A saved level reader is unavailable.");
  const TLevelP info = reader->loadInfo();
  if (!info || info->isPartialLoad() ||
      info->getFrameCount() != int(frames.size()))
    throw Error("invalid_package",
                "Saved drawing count differs from the source level.");
  level->setPalette(info->getPalette());
  restoreSavedRaster(*level, reader, frames, entry);
  restoreSavedFrames(*level, reader, info, entry);
  if (level->getFids() != frames)
    throw Error("invalid_package",
                "The reopened drawing IDs differ from their source.");
  readAutomationHooks(scene, *level, entry.value("hooks_sha256"));
  validateDrawingProfile(level);
  if (level->getType() == PLI_XSHLEVEL &&
      vectorLevelOpacityDigest(*level) !=
          entry.value("stroke_opacity_sha256").toString())
    throw Error("invalid_package",
                "Reopened stroke opacity differs from the source level.");
  if (paletteManifest(*level) != entry.value("palette"))
    throw Error("invalid_package",
                "The reopened palette differs from the source palette.");
}

void requireLevelSettings(const QJsonArray& entries, ToonzScene& scene) {
  QJsonObject saved;
  for (const QJsonValue& entry : entries)
    saved.insert(entry.toObject().value("name").toString(),
                 entry.toObject().value("settings"));
  requireEqualRecord("level_settings", saved, levelSettings(scene));
}

std::unique_ptr<ToonzScene> reopenPackage(Context& context,
                                          const QString& directory,
                                          const Package& package) {
  verifyFiles(directory, package);
  auto scene = std::make_unique<ToonzScene>();
  scene->setProject(context.project());
  scene->loadTnzFile(
      TFilePath(QDir(directory).absoluteFilePath("scene.tnz").toStdWString()));
  const QJsonArray entries = package.manifest.value("levels").toArray();
  const int sounds         = package.manifest.value("sounds").toArray().size();
  if (scene->hasUnrecognizedSceneData() ||
      scene->getLevelSet()->getLevelCount() != entries.size() + sounds)
    throw Error("invalid_package",
                "The reopened scene differs from the published scene.");
  for (const QJsonValue& entry : entries) reopenLevel(*scene, entry.toObject());
  reopenAutomationSounds(*scene, package.manifest);
  if (automationTagsDigest(*scene) !=
      package.manifest.value("tags_sha256").toString())
    throw Error("invalid_package",
                "Reopened storyboard panels differ from the published scene.");
  validateAutomationScene(*scene);
  requireAutomationSheetRecord(package.manifest, *scene);
  requireLevelSettings(entries, *scene);
  indexAutomationMacroMembers(*scene);
  if (automationMacroFingerprint(*scene, context.scratchDirectory()) !=
      package.macroFingerprint)
    throw Error("invalid_package", "The reopened macro graph differs from its published source.");
  verifyFiles(directory, package);
  return scene;
}

// What the journal registers for a published package: its id (the SHA-256 of
// the exact manifest bytes), the manifest as a blob and the macro fingerprint
// the manifest lacks. Empty outside a journalled publication.
QJsonObject registrationFields(Context& context, const Package& package) {
  Journal* journal = workspaceJournal(context);
  if (!journal || !journal->available() || context.writeClass != "publication")
    return {};
  const QString blob = journal->storeBlob(package.manifestBytes);
  if (blob.isEmpty()) return {};
  return {
      {"package_id", "package:" + QString::fromLatin1(package.manifestHash)},
      {"manifest_blob", blob},
      {"manifest_bytes", double(package.manifestBytes.size())},
      {"macro_fingerprint",
       QString::fromLatin1(package.macroFingerprint.toHex())}};
}

Package publishPackage(Context& context, Document& document,
                       const QString& destination) {
  const QString path = context.resolvePath(destination, true);
  const QFileInfo target(path);
  if (target.exists() || target.isSymLink())
    throw Error(
        "already_exists",
        "Choose a new package directory; existing paths are not overwritten.");
  if (!QDir(target.absolutePath()).exists())
    throw Error("missing_parent", "Create the package parent directory first.");
  if (!QFileInfo(target.absolutePath()).isWritable())
    throw Error("package_write_failed",
                "The package parent directory is not writable.");
  validateAutomationScene(*document.scene);
  PrivateDirectory staging(context.runtimeDirectory + "/package-XXXXXX");
  if (!staging.isValid())
    throw stagedWriteError("package_write_failed",
                           "Cannot create a private package staging directory.",
                           "stage", QString(), false);
  SaveStep step;
  Package package;
  try {
    packageFault(context, "package:staged", step);
    package = writePackage(context, document, staging.path(), step);
    step    = {"verify", QString()};
    reopenPackage(context, staging.path(), package);
    packageFault(context, "package:verify", step);
  } catch (const Error& error) {
    throw stepError(error.code, error.message, error.details, step);
  } catch (const TException& error) {
    throw stepError("native_error", QString::fromStdWString(error.getMessage()),
                    {}, step);
  } catch (const std::exception& error) {
    throw stepError("native_error", QString::fromUtf8(error.what()), {}, step);
  }
  package.registration = registrationFields(context, package);
  publishStaged(context, staging.path(), path, "package_write_failed",
                "Cannot publish the verified package directory.",
                package.registration.isEmpty()
                    ? QJsonObject()
                    : QJsonObject{{"package", package.registration}});
  staging.setAutoRemove(false);
  return package;
}

// A package admitted for scene.open or scene.package_info.
struct Admission final {
  QString directory;
  Package package;
  QString kind;
  QString packageId;
  bool relocated = false;
  QJsonValue publishedBy;
};

[[noreturn]] void refuseUnregistered(const QString& reason) {
  throw Error("unregistered_package",
              "Open only a scene package a worker of this workspace published "
              "and registered; open other native scenes with "
              "scene.open_read_only and import them with "
              "scene.import_editable.",
              {{"reason", reason},
               {"next", "scene.import_editable"},
               {"hint", "scene.open_read_only"}});
}

QString workspaceRelative(Context& context, const QString& path) {
  const QString workspace = context.workspace();
  return path.startsWith(workspace + '/') ? path.mid(workspace.size() + 1)
                                          : path;
}

// Why a manifest this registry does not hold is refused: registered once and
// expired, another manifest registered at this place, or never registered.
QString unregisteredReason(const Journal& journal, const QString& id,
                           const QString& relative) {
  if (const QJsonObject* record = journal.package(id))
    if (record->value("expired").toBool()) return "registry_expired";
  for (const QString& other : journal.packageIds()) {
    const QJsonObject* record = journal.package(other);
    if (!record->value("expired").toBool() &&
        record->value("path").toString() == relative)
      return "manifest_changed";
  }
  return "not_registered";
}

Admission registryAdmission(Context& context, const QString& directory) {
  Journal* journal = workspaceJournal(context);
  if (!journal || !journal->readable()) refuseUnregistered("not_registered");
  journal->refresh();
  const QFileInfo manifest(directory + "/manifest.json");
  if (!manifest.isFile() || manifest.size() > MaxManifestBytes)
    refuseUnregistered("not_registered");
  const QByteArray bytes    = readRegularFile(directory, "manifest.json");
  const QString id          = "package:" + QString::fromLatin1(digest(bytes));
  const QString relative    = workspaceRelative(context, directory);
  const QJsonObject* record = journal->package(id);
  if (!record || record->value("expired").toBool())
    refuseUnregistered(unregisteredReason(*journal, id, relative));
  if (journal->readBlob(record->value("manifest_blob").toString(),
                        bytes.size()) != bytes)
    throw Error("invalid_package",
                "The registered manifest of this package is missing from the "
                "workspace journal.",
                {{"package_id", id}});
  Admission admission;
  admission.directory                = directory;
  admission.kind                     = "workspace_registry";
  admission.packageId                = id;
  admission.package.manifest         = QJsonDocument::fromJson(bytes).object();
  admission.package.manifestHash     = digest(bytes);
  admission.package.manifestBytes    = bytes;
  admission.package.macroFingerprint = QByteArray::fromHex(
      record->value("macro_fingerprint").toString().toLatin1());
  admission.relocated = record->value("path").toString() != relative;
  const QString writer =
      record->value("published_by").toString(record->value("w").toString());
  admission.publishedBy =
      QJsonObject{{"worker_instance_id", writer},
                  {"current", writer == journal->instance()}};
  return admission;
}

// Admitted by this worker's own publication, or by the workspace registry
// wherever the package now lies inside a read root.
Admission admitPackage(Context& context, const QJsonObject& params) {
  QString path = context.resolvePath(requiredString(params, "path"), false);
  if (QFileInfo(path).fileName() == "manifest.json")
    path = QFileInfo(path).absolutePath();
  const QString directory = QFileInfo(path).canonicalFilePath();
  if (directory.isEmpty() || !QFileInfo(directory).isDir())
    refuseUnregistered("not_registered");
  const auto own = publishedPackages.find(directory);
  if (own == publishedPackages.end())
    return registryAdmission(context, directory);
  Admission admission;
  admission.directory = directory;
  admission.package   = own->second;
  admission.kind      = "this_worker";
  admission.packageId =
      "package:" + QString::fromLatin1(own->second.manifestHash);
  admission.publishedBy =
      QJsonObject{{"worker_instance_id", jobRegistry(context).instance()},
                  {"current", true}};
  return admission;
}

// Copies every file the registered manifest lists into private storage,
// checking each size and SHA-256 while copying, and refuses a package with
// a changed, missing or unlisted file. The native load reads only the copy.
QString copyIn(Context& context, const Admission& admission) {
  PrivateDirectory copy(context.runtimeDirectory + "/opened-XXXXXX");
  if (!copy.isValid())
    throw Error("package_read_failed", "Cannot create a private package copy.");
  QSet<QString> listed{"manifest.json"};
  for (const QJsonValue& value :
       admission.package.manifest.value("files").toArray()) {
    const QJsonObject entry = value.toObject();
    const QString relative  = entry.value("path").toString();
    const QByteArray bytes  = readRegularFile(admission.directory, relative);
    if (bytes.size() != entry.value("size_bytes").toDouble() ||
        QString::fromLatin1(digest(bytes)) != entry.value("sha256").toString())
      throw Error("invalid_package",
                  "A package file changed after publication.",
                  {{"file", relative}});
    writePrivateFile(copy.filePath(relative), bytes);
    listed.insert(relative);
    writeFaultFires(context, "reopen:copy");
  }
  writePrivateFile(copy.filePath("manifest.json"),
                   admission.package.manifestBytes);
  QDirIterator files(admission.directory,
                     QDir::Files | QDir::Hidden | QDir::System,
                     QDirIterator::Subdirectories);
  while (files.hasNext())
    if (!listed.contains(
            QDir(admission.directory).relativeFilePath(files.next())))
      throw Error("invalid_package",
                  "The package contains an unrecorded file.");
  copy.setAutoRemove(false);
  return copy.path();
}

// Removes a private copy that no document took.
struct CopyGuard final {
  QString path;
  bool keep = false;
  ~CopyGuard() {
    if (!keep && !path.isEmpty()) removeApprovedTree(path);
  }
};

QJsonObject admissionReport(const Admission& admission) {
  return {
      {"admission", admission.kind},
      {"package_id", admission.packageId},
      {"relocated", admission.relocated},
      {"published_by", admission.publishedBy},
      {"provenance", admission.package.manifest.value("provenance").isObject()
                         ? admission.package.manifest.value("provenance")
                         : QJsonValue()}};
}

// Handles of the empty columns after the last used one: the native saver
// writes no column, object or effect for them.
QJsonArray droppedColumns(Document& document) {
  TXsheet* sheet = document.scene->getXsheet();
  QJsonArray handles;
  for (int index = sheet->getFirstFreeColumnIndex();
       index < sheet->getColumnCount(); ++index)
    if (TXshColumn* column = sheet->getColumn(index))
      handles.append(document.idForColumn(column));
  return handles;
}

QJsonObject saveCopy(Context& context, const QJsonObject& params) {
  if (publishedPackages.size() >= MaxPackages)
    throw Error("package_limit",
                "This worker has reached its 128 published package limit.");
  Document& document = context.document(requiredString(params, "document_id"));
  const QString directory =
      context.resolvePath(requiredString(params, "directory"), true);
  const Package package = publishPackage(context, document, directory);
  publishedPackages.emplace(directory, package);
  Journal* journal      = workspaceJournal(context);
  const bool registered = journal && !package.registration.isEmpty();
  if (registered) {
    QJsonObject fields = package.registration;
    fields.insert("operation_id", context.writeOperation);
    fields.insert("path", workspaceRelative(context, directory));
    fields.insert("published_by", journal->instance());
    journal->registerPackage(fields);
  }
  int drawings = 0;
  for (const QJsonValue& entry : package.manifest.value("levels").toArray())
    drawings += entry.toObject().value("drawings").toArray().size();
  return {
      {"package_directory", directory},
      {"trailing_empty_columns_dropped", droppedColumns(document)},
      {"scene_path", directory + "/scene.tnz"},
      {"manifest_path", directory + "/manifest.json"},
      {"verified_drawings", drawings},
      {"package_id", "package:" + QString::fromLatin1(package.manifestHash)},
      {"registered", registered},
      {"document_id", document.id},
      {"revision", document.revisionText()}};
}

QJsonObject openPackage(Context& context, const QJsonObject& params) {
  context.requireDocumentCapacity();
  const Admission admission = admitPackage(context, params);
  const QString directory   = admission.directory;
  // A registry package loads from a private copy checked while copied; this
  // worker's own publication loads where it published it.
  const QString copy =
      admission.kind == "this_worker" ? QString() : copyIn(context, admission);
  CopyGuard guard{copy};
  auto scene        = reopenPackage(context, copy.isEmpty() ? directory : copy,
                                    admission.package);
  const auto sounds = adoptAutomationSounds(context, *scene);
  context.markMutationStarted();
  Document& document    = context.addDocument(std::move(scene));
  document.soundSources = sounds;
  if (!copy.isEmpty()) document.privateCopies.append(copy);
  guard.keep = true;
  initializeAutomationRig(document);
  QJsonObject result = admissionReport(admission);
  result.insert("document_id", document.id);
  result.insert("revision", document.revisionText());
  result.insert("package_directory", directory);
  return result;
}

using DrawingIndex = std::map<TXshSimpleLevel*, std::set<TFrameId>>;

// Sound columns are admitted by clip enumeration in validateAutomationAudio;
// their getCell() allocates a cell per call.
std::pair<int, int> drawingColumnRange(TXshColumn* column) {
  if (!column || column->isEmpty() || column->getZeraryFxColumn() ||
      column->getSoundColumn())
    return {0, -1};
  if (!column->getLevelColumn())
    throw Error("unsupported_column",
                "This package profile supports drawing and generator columns.");
  int first = 0;
  int last  = 0;
  column->getRange(first, last);
  // Rows 0 to 999999, as TXsheet::maxRowCount admits on load.
  if (first < 0 || last > 999999)
    throw Error("scene_limit",
                "The scene exceeds the supported exposure limit.");
  return {first, last};
}

void validateExposure(ToonzScene& scene, const TXshCell& cell,
                      DrawingIndex& drawings) {
  if (cell.isEmpty()) return;
  TXshSimpleLevel* level = cell.getSimpleLevel();
  if (!level || !scene.getLevelSet()->hasLevel(level->getName()))
    throw Error("missing_level",
                "An exposure references an unsupported or missing level.");
  if (!drawings.count(level)) {
    const std::vector<TFrameId> frames = level->getFids();
    drawings.emplace(level, std::set<TFrameId>(frames.begin(), frames.end()));
  }
  if (!drawings.at(level).count(cell.m_frameId))
    throw Error("missing_drawing", "An exposure references a missing drawing.");
}

void validateCells(ToonzScene& scene) {
  TXsheet* sheet = scene.getXsheet();
  if (sheet->getColumnCount() > 4096)
    throw Error("scene_limit", "The scene exceeds the supported column limit.");
  qint64 cells = 0;
  DrawingIndex drawings;
  for (int index = 0; index < sheet->getColumnCount(); ++index) {
    const auto range = drawingColumnRange(sheet->getColumn(index));
    cells += range.second - range.first + 1;
    if (cells > 1000000)
      throw Error("scene_limit",
                  "The scene exceeds the supported exposure limit.");
    for (int row = range.first; row <= range.second; ++row)
      validateExposure(scene, sheet->getCell(row, index), drawings);
  }
}

QJsonObject textSchema() { return {{"type", "string"}, {"minLength", 1}}; }

}  // namespace

void validateAutomationScene(ToonzScene& scene) {
  validateAutomationFxGraph(scene);
  validateAutomationPaths(scene);
  validateAutomationRig(scene);
  validateAutomationStoryboard(scene);
  TLevelSet* levels = scene.getLevelSet();
  if (levels->getLevelCount() > MaxLevels)
    throw Error("scene_limit", "The scene exceeds 256 levels.");
  int drawings = 0;
  for (int index = 0; index < levels->getLevelCount(); ++index) {
    if (levels->getLevel(index)->getSoundLevel()) continue;
    TXshSimpleLevel* level = levels->getLevel(index)->getSimpleLevel();
    if (!level)
      throw Error("unsupported_level",
                  "The scene contains an unsupported level type.");
    validateDrawingProfile(level);
    if (level->getFrameCount() == 0)
      throw Error("empty_level",
                  "Create a drawing in each level before saving or rendering.");
    drawings += level->getFrameCount();
    if (drawings > MaxDrawings)
      throw Error("scene_limit", "The scene exceeds 4096 drawings.");
  }
  validateAutomationAudio(scene);
  requireAssetGrowth(scene, 0);
  validateCells(scene);
  validateAutomationHooks(scene);
}

std::unique_ptr<ToonzScene> createRenderSnapshot(Context& context,
                                                 Document& document,
                                                 const QString& directory,
                                                 QString* manifestSha256) {
  const Package package = publishPackage(context, document, directory);
  if (manifestSha256)
    *manifestSha256 = QString::fromLatin1(package.manifestHash);
  return reopenPackage(context, directory, package);
}

QJsonArray persistenceOperations() {
  const QJsonObject save = operation(
      "scene.save_copy",
      "Save a new native scene package and independently decode every drawing.",
      {{"document_id", textSchema()}, {"directory", textSchema()}},
      {"document_id", "directory"}, true);
  return {
      save,
      operation("scene.open",
                "Open an unchanged package published by this running worker.",
                {{"path", textSchema()}}, {"path"}, true),
      operation("scene.package_info",
                "Inspect a verified package published by this worker.",
                {{"path", textSchema()}}, {"path"})};
}

QJsonValue executePersistence(Context& context, const QString& method,
                              const QJsonObject& params) {
  if (method == "scene.save_copy") return saveCopy(context, params);
  if (method == "scene.open") return openPackage(context, params);
  if (method == "scene.package_info") {
    const Admission admission = admitPackage(context, params);
    verifyFiles(admission.directory, admission.package);
    QJsonObject result       = admission.package.manifest;
    const QJsonObject report = admissionReport(admission);
    for (auto it = report.begin(); it != report.end(); ++it)
      result.insert(it.key(), it.value());
    return result;
  }
  throw Error("method_not_found",
              "The persistence operation is not available.");
}

}  // namespace GroundzeroAutomation
