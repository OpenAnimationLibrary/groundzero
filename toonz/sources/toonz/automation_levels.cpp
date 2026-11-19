#include "automation_drawing.h"
#include "automation_files.h"
#include "automation_hook.h"
#include "automation_level_lifecycle.h"
#include "automation_scene_host.h"
#include "automation_scene_shared.h"

#include "tpalette.h"
#include "trasterimage.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>
#include <QUuid>

#include <map>
#include <vector>

namespace GroundzeroAutomation {
namespace SceneShared {
namespace {

constexpr qint64 MaxPngBytes = 67108864;

QString levelFormat(int type) {
  if (type == PLI_XSHLEVEL) return "vector";
  if (type == TZP_XSHLEVEL) return "toonzRaster";
  if (type == OVL_XSHLEVEL) return "rgba8";
  if (type == SND_XSHLEVEL) return "sound";
  return "unsupported";
}

}  // namespace

QJsonObject describeLevel(Document& document, TXshLevel* level,
                          bool includeDrawings) {
  QJsonObject result{{"level_id", document.idForLevel(level)},
                     {"name", QString::fromStdWString(level->getName())},
                     {"format", levelFormat(level->getType())}};
  if (includeDrawings) {
    std::vector<TFrameId> frames;
    if (!level->getSoundLevel()) level->getFids(frames);
    QJsonArray drawings;
    for (const TFrameId& frame : frames) drawings.append(drawingId(frame));
    result.insert("drawings", drawings);
  }
  if (document.readOnly) {
    const LevelState state = document.readOnly->levels.value(level);
    result.insert("kind", state.kind.isEmpty() ? "unknown" : state.kind);
    result.insert("bit_depth",
                  state.bitDepth ? QJsonValue(state.bitDepth) : QJsonValue());
  }
  return result;
}

TFilePath newLevelPath(Context& context, const QString& extension) {
  const QString directory =
      context.resolvePath(context.privateDirectory("levels"), true);
  if (!QDir().mkpath(directory))
    throw Error("io_error", "Cannot create the workspace level directory.");
  const QString id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString separator = extension == "png" ? ".." : ".";
  return TFilePath(
      QDir(directory).filePath(id + separator + extension).toStdWString());
}

namespace {

std::unique_ptr<TXshSimpleLevel> prepareLevel(Context& context,
                                              Document& document,
                                              const QJsonObject& params) {
  const QString name = nameParam(params);
  TLevelSet* cast    = document.scene->getLevelSet();
  if (cast->getLevel(name.toStdWString()))
    throw Error("name_conflict", "A level with that name already exists.");
  const QString format = requiredString(params, "format");
  const std::map<QString, std::pair<int, QString>> types = {
      {"vector", {PLI_XSHLEVEL, "pli"}},
      {"rgba8", {OVL_XSHLEVEL, "png"}},
      {"toonzRaster", {TZP_XSHLEVEL, "tlv"}}};
  const auto type = types.find(format);
  if (type == types.end())
    throw Error("unsupported_format", "Unsupported level format.");
  requireAssetGrowth(
      *document.scene,
      format == "rgba8" ? 0 : 2 * AutomationStyleBytes + AutomationPageBytes, 0,
      1);
  auto level = std::make_unique<TXshSimpleLevel>(name.toStdWString());
  level->setScene(document.scene.get());
  level->setType(type->second.first);
  level->setPath(newLevelPath(context, type->second.second), true);
  if (format != "rgba8") level->setPalette(new TPalette());
  return level;
}

QJsonValue createLevel(Context& context, const QJsonObject& params) {
  Document& document      = getDocument(context, params);
  auto level              = prepareLevel(context, document, params);
  TXshSimpleLevel* result = level.get();
  context.markMutationStarted();
  if (!document.scene->getLevelSet()->insertLevel(result))
    throw Error("native_error",
                "Could not register the new level in the scene cast.");
  level.release();
  return describeLevel(document, result, true);
}

QByteArray readSourcePng(const QString& path) {
  const QFileInfo info(path);
  if (!info.isFile() || info.size() > MaxPngBytes)
    throw Error("invalid_asset",
                "Import requires a regular PNG file no larger than 64 MiB.");
  QFile input;
  if (openApprovedFile(input, path) != OpenResult::Opened)
    throw Error("io_error", "Cannot read the source PNG file.");
  const QByteArray bytes = input.read(MaxPngBytes + 1);
  if (input.error() != QFileDevice::NoError || bytes.size() > MaxPngBytes)
    throw Error("invalid_asset",
                "Import requires a regular PNG file no larger than 64 MiB.");
  return bytes;
}

void validatePngPrecision(const QByteArray& bytes) {
  const QByteArray header    = bytes.left(29);
  const QByteArray signature = QByteArray::fromHex("89504e470d0a1a0a");
  if (header.size() != 29 || !header.startsWith(signature) ||
      header.mid(8, 8) != QByteArray::fromHex("0000000d49484452"))
    throw Error("invalid_asset",
                "The source is not a PNG with a valid IHDR header.");
  const unsigned char depth = static_cast<unsigned char>(header.at(24));
  if (depth > 8)
    throw Error("unsupported_precision",
                "PNG import supports at most 8 bits per channel.");
}

QImage readPng(Context& context, Document& document,
               const QJsonObject& params) {
  const QString path =
      context.resolvePath(requiredString(params, "path"), false);
  QByteArray bytes = readSourcePng(path);
  validatePngPrecision(bytes);
  QBuffer source(&bytes);
  source.open(QIODevice::ReadOnly);
  QImageReader reader(&source, "png");
  reader.setAutoDetectImageFormat(false);
  const QSize size = reader.size();
  if (size.isEmpty() || size.width() > 8192 || size.height() > 8192 ||
      qint64(size.width()) * size.height() > MaxPixels)
    throw Error("resource_limit",
                "PNG dimensions must be 1..8192 per axis and no more than "
                "16 megapixels.");
  requireAssetGrowth(*document.scene,
                     automationRasterBytes(size.width(), size.height()), 1, 1);
  const QImage image = reader.read();
  if (image.isNull())
    throw Error("invalid_asset",
                "PNG decoding failed: " + reader.errorString());
  return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

TRasterImageP nativeRaster(const QImage& image, double dpi) {
  TRaster32P raster(image.width(), image.height());
  raster->lock();
  for (int y = 0; y < image.height(); ++y) {
    TPixel32* row      = raster->pixels(image.height() - y - 1);
    const QRgb* source = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x)
      row[x] = TPixel32(qRed(source[x]), qGreen(source[x]), qBlue(source[x]),
                        qAlpha(source[x]));
  }
  raster->unlock();
  TRasterImageP result(raster);
  result->setDpi(dpi, dpi);
  return result;
}

QJsonValue importLevel(Context& context, const QJsonObject& params) {
  Document& document     = getDocument(context, params);
  const double dpi       = numberParam(params, "dpi", 1, 2400, 120);
  const QImage image     = readPng(context, document, params);
  QJsonObject properties = params;
  properties.insert("format", "rgba8");
  auto level = prepareLevel(context, document, properties);
  const TFrameId frame(1);
  const QString path = QString::fromStdWString(
      level->getPath().withFrame(frame).getWideString());
  TRasterImageP raster = nativeRaster(image, dpi);
  QSaveFile output(path);
  context.markMutationStarted();
  if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") ||
      !output.commit())
    throw Error("io_error", "Cannot write the private PNG import copy.");
  level->getProperties()->setDpiPolicy(LevelProperties::DP_CustomDpi);
  level->getProperties()->setDpi(dpi);
  level->getProperties()->setImageRes(
      TDimension(image.width(), image.height()));
  level->setFrame(frame, raster);
  level->setDirtyFlag(true);
  TXshSimpleLevel* result = level.get();
  if (!document.scene->getLevelSet()->insertLevel(result))
    throw Error("native_error",
                "Could not register the imported PNG in the scene cast.");
  level.release();
  return describeLevel(document, result, true);
}

int hookKeys(TXshSimpleLevel& level) {
  int keys = 0;
  for (const TFrameId& frame : level.getFids())
    keys += automationHookKeys(level, frame);
  return keys;
}

QJsonValue inspectLevel(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  TXshLevel* level        = document.level(requiredString(params, "level_id"));
  QJsonObject result      = describeLevel(document, level, true);
  TXshSimpleLevel* simple = level->getSimpleLevel();
  if (!simple) return result;
  const LevelUsage usage = scanLevelUsage(document, level);
  QJsonObject settings   = levelSettingsReport(document, simple, usage);
  settings.insert("hook_keys", hookKeys(*simple));
  result.insert("settings", settings);
  if (boolParam(params, "include_usage", false))
    result.insert("usage", levelUsageJson(document, usage));
  return result;
}

}  // namespace

void addLevelOperations(QJsonArray& catalog) {
  QJsonObject level  = merge(documentSchema(), {{"name", textSchema()}});
  QJsonObject create = level;
  create.insert("format", enumSchema({"vector", "rgba8", "toonzRaster"}));
  catalog.append(
      operation("level.create",
                "Create an empty native level; document limit is 256 levels.",
                create, {"document_id", "name", "format"}, true));
  level.insert("path", textSchema());
  level.insert("dpi", numberSchema(1, 2400));
  catalog.append(operation(
      "level.import",
      "Import one PNG as private drawing 1 within document asset limits.",
      level, {"document_id", "path", "name"}, true));
  catalog.append(operation(
      "level.inspect",
      "Inspect a level's format, exact drawing identifiers and interpretation "
      "settings, each with whether it applies and can be written now; with "
      "include_usage, its exposures in every sheet.",
      merge(documentSchema(),
            {{"level_id", textSchema()},
             {"include_usage", QJsonObject{{"type", "boolean"}}}}),
      {"document_id", "level_id"}));
}

void addLevelHandlers(SceneHandlers& handlers) {
  handlers.insert({{"level.create", createLevel},
                   {"level.import", importLevel},
                   {"level.inspect", inspectLevel}});
}

}  // namespace SceneShared
}  // namespace GroundzeroAutomation
