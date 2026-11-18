#include "automation_hook.h"
#include "automation_path.h"
#include "automation_rig.h"
#include "automation_vector_opacity.h"
#include "automation.h"
#include "automation_audio.h"
#include "automation_drawing.h"
#include "automation_fx.h"
#include "automation_macro.h"
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
};

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
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    throw Error("package_read_failed", "Cannot open a package file.",
                {{"file", relative}});
  const QByteArray bytes = file.read(MaxPackageBytes + 1);
  if (file.error() != QFileDevice::NoError || bytes.size() != info.size())
    throw Error("package_read_failed", "Cannot read a complete package file.",
                {{"file", relative}});
  return bytes;
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

QJsonArray writeLevels(ToonzScene& scene, const QString& directory) {
  QJsonArray entries;
  TLevelSet* levels = scene.getLevelSet();
  const double frameRate =
      scene.getProperties()->getOutputProperties()->getFrameRate();
  for (int index = 0; index < levels->getLevelCount(); ++index) {
    TXshSimpleLevel* level = levels->getLevel(index)->getSimpleLevel();
    if (!level) continue;  // Sound sources are copied by writeAutomationSounds.
    const QString relative = levelPath(index, level->getType());
    const QString file     = QDir(directory).absoluteFilePath(relative);
    writeLevel(*level, file, frameRate);
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

void writeScene(ToonzScene& scene, const QString& directory) {
  PathRestore restore(scene);
  restore.rebase();
  AutomationSoundPaths sounds(scene);
  scene.saveCopy(
      TFilePath(QDir(directory).absoluteFilePath("scene.tnz").toStdWString()));
}

Package writePackage(Document& document, const QString& directory) {
  validateAutomationScene(*document.scene);
  if (!QDir(directory).mkdir("assets"))
    throw Error("package_write_failed",
                "Cannot create the package assets directory.");
  const QByteArray macroFingerprint = automationMacroFingerprint(*document.scene, directory);
  const QJsonArray levels = writeLevels(*document.scene, directory);
  const QJsonObject sounds = writeAutomationSounds(*document.scene, directory);
  writeScene(*document.scene, directory);
  Package package;
  package.macroFingerprint = macroFingerprint;
  package.manifest         = {{"version", 1},
                              {"profile", "worker-generated-v1"},
                              {"scene_file", "scene.tnz"},
                              {"source_revision", document.revisionText()},
                              {"levels", levels}};
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
  package.manifestHash = digest(bytes);
  return package;
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
  indexAutomationMacroMembers(*scene);
  if (automationMacroFingerprint(*scene, context.workspace()) != package.macroFingerprint)
    throw Error("invalid_package", "The reopened macro graph differs from its published source.");
  verifyFiles(directory, package);
  return scene;
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
  QTemporaryDir staging(target.absolutePath() + "/.mcp-package-XXXXXX");
  if (!staging.isValid())
    throw Error("package_write_failed",
                "Cannot create a private package staging directory.");
  const Package package = writePackage(document, staging.path());
  reopenPackage(context, staging.path(), package);
  context.markMutationStarted();
  if (QFileInfo(path).exists() || !QDir().rename(staging.path(), path))
    throw Error("package_write_failed",
                "Cannot publish the verified package directory.");
  staging.setAutoRemove(false);
  return package;
}

QString admittedDirectory(Context& context, const QJsonObject& params) {
  QString path = context.resolvePath(requiredString(params, "path"), false);
  if (QFileInfo(path).fileName() == "manifest.json")
    path = QFileInfo(path).absolutePath();
  const QString directory = QFileInfo(path).canonicalFilePath();
  if (!publishedPackages.count(directory))
    throw Error("unregistered_package",
                "Open only a scene package published by this worker; open "
                "other native scenes with scene.open_read_only.",
                {{"hint", "scene.open_read_only"}});
  return directory;
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
  int drawings = 0;
  for (const QJsonValue& entry : package.manifest.value("levels").toArray())
    drawings += entry.toObject().value("drawings").toArray().size();
  return {{"package_directory", directory},
          {"scene_path", directory + "/scene.tnz"},
          {"manifest_path", directory + "/manifest.json"},
          {"verified_drawings", drawings},
          {"document_id", document.id},
          {"revision", document.revisionText()}};
}

QJsonObject openPackage(Context& context, const QJsonObject& params) {
  context.requireDocumentCapacity();
  const QString directory = admittedDirectory(context, params);
  auto scene =
      reopenPackage(context, directory, publishedPackages.at(directory));
  const auto sounds = adoptAutomationSounds(context, *scene);
  context.markMutationStarted();
  Document& document    = context.addDocument(std::move(scene));
  document.soundSources = sounds;
  initializeAutomationRig(document);
  return {{"document_id", document.id},
          {"revision", document.revisionText()},
          {"package_directory", directory}};
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
  if (first < 0 || last > 1000000)
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
                                                 const QString& directory) {
  const Package package = publishPackage(context, document, directory);
  return reopenPackage(context, directory, package);
}

QJsonArray persistenceOperations() {
  QJsonObject save = operation(
      "scene.save_copy",
      "Save a new native scene package and independently decode every drawing.",
      {{"document_id", textSchema()}, {"directory", textSchema()}},
      {"document_id", "directory"}, true);
  save.insert("mutates_document", false);
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
    const QString directory = admittedDirectory(context, params);
    const Package& package  = publishedPackages.at(directory);
    verifyFiles(directory, package);
    return package.manifest;
  }
  throw Error("method_not_found",
              "The persistence operation is not available.");
}

}  // namespace GroundzeroAutomation
