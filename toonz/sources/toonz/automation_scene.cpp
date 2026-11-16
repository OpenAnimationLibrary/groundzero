#include "automation.h"

#include "tdoubleparam.h"
#include "toutputproperties.h"
#include "tpalette.h"
#include "trasterimage.h"
#include "toonz/fxdag.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxFrame     = 1000000;
constexpr int MaxSpan      = 10000;
constexpr int MaxEntities  = 4096;
constexpr qint64 MaxPixels = 16777216;

QJsonObject textSchema() { return {{"type", "string"}, {"minLength", 1}}; }

QJsonObject numberSchema(double minimum, double maximum) {
  return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject enumSchema(const QJsonArray& values) {
  return {{"type", "string"}, {"enum", values}};
}

QJsonObject arraySchema(const QJsonObject& items) {
  return {{"type", "array"},
          {"items", items},
          {"minItems", 1},
          {"maxItems", MaxSpan}};
}

QJsonObject documentSchema() { return {{"document_id", textSchema()}}; }

double numberParam(const QJsonObject& params, const QString& key,
                   double minimum, double maximum, double fallback) {
  if (!params.contains(key)) return fallback;
  const QJsonValue value = params.value(key);
  const double number    = value.toDouble();
  if (!value.isDouble() || !std::isfinite(number) || number < minimum ||
      number > maximum)
    throw Error("invalid_argument",
                key + " must be a finite number within its stated limits.");
  return number;
}

bool boolParam(const QJsonObject& params, const QString& key, bool fallback) {
  if (!params.contains(key)) return fallback;
  if (!params.value(key).isBool())
    throw Error("invalid_argument", key + " must be a boolean.");
  return params.value(key).toBool();
}

QString nameParam(const QJsonObject& params) {
  const QString name = requiredString(params, "name");
  if (name.size() > 128 || name.contains(QChar::Null))
    throw Error("invalid_argument",
                "name must contain 1 to 128 characters without NUL.");
  return name;
}

Document& getDocument(Context& context, const QJsonObject& params) {
  return context.document(requiredString(params, "document_id"));
}

TStageObjectTree* tree(Document& document) {
  return document.scene->getXsheet()->getStageObjectTree();
}

int columnIndex(Document& document, TXshColumn* column) {
  TXsheet* sheet = document.scene->getXsheet();
  for (int index = 0; index < sheet->getColumnCount(); ++index)
    if (sheet->getColumn(index) == column) return index;
  throw Error("stale_handle", "The column no longer belongs to this document.");
}

void requireEditable(Document& document, TStageObject* object) {
  bool locked = object->isLocked();
  if (object->getId().isColumn()) {
    TXshColumn* column =
        document.scene->getXsheet()->getColumn(object->getId().getIndex());
    locked = locked || (column && column->isLocked());
  }
  if (locked) throw Error("locked", "Unlock the target before modifying it.");
}

QJsonObject drawingId(const TFrameId& frame) {
  return {{"number", frame.getNumber()}, {"suffix", frame.getLetter()}};
}

TFrameId readDrawingId(const QJsonObject& params) {
  if (!params.value("drawing_id").isObject())
    throw Error("invalid_argument",
                "drawing_id must contain number and suffix.");
  const QJsonObject id = params.value("drawing_id").toObject();
  const int number     = integerParam(id, "number", 1, 999999, -1);
  if (number < 1 || !id.value("suffix").isString())
    throw Error("invalid_argument",
                "drawing_id requires a positive number and string suffix.");
  const QString suffix = id.value("suffix").toString();
  if (suffix.size() > 8)
    throw Error("invalid_argument",
                "drawing_id suffix is limited to eight ASCII letters.");
  for (const QChar letter : suffix)
    if (letter < QLatin1Char('a') || letter > QLatin1Char('z'))
      throw Error("invalid_argument",
                  "drawing_id suffix must contain lowercase ASCII letters.");
  return TFrameId(number, suffix);
}

QString levelFormat(int type) {
  if (type == PLI_XSHLEVEL) return "vector";
  if (type == TZP_XSHLEVEL) return "toonzRaster";
  if (type == OVL_XSHLEVEL) return "rgba8";
  return "unsupported";
}

QJsonObject describeLevel(Document& document, TXshLevel* level) {
  std::vector<TFrameId> frames;
  level->getFids(frames);
  QJsonArray drawings;
  for (const TFrameId& frame : frames) drawings.append(drawingId(frame));
  return {{"level_id", document.idForLevel(level)},
          {"name", QString::fromStdWString(level->getName())},
          {"format", levelFormat(level->getType())},
          {"drawings", drawings}};
}

QJsonObject describeObject(Document& document, TStageObject* object) {
  const TStageObjectId id = object->getId();
  QString kind            = "table";
  if (id.isColumn()) kind = "column";
  if (id.isCamera()) kind = "camera";
  if (id.isPegbar()) kind = "pegbar";
  TStageObject* parent =
      tree(document)->getStageObject(object->getParent(), false);
  return {{"object_id", document.idForObject(object)},
          {"kind", kind},
          {"name", QString::fromStdString(object->getName())},
          {"parent_id",
           parent ? QJsonValue(document.idForObject(parent)) : QJsonValue()},
          {"locked", object->isLocked()}};
}

QJsonObject describeColumn(Document& document, TXshColumn* column) {
  const int index = columnIndex(document, column);
  TStageObject* object =
      tree(document)->getStageObject(TStageObjectId::ColumnId(index), false);
  return {{"column_id", document.idForColumn(column)},
          {"object_id", document.idForObject(object)},
          {"position", index},
          {"name", QString::fromStdString(object->getName())},
          {"visible", column->isPreviewVisible()},
          {"locked", column->isLocked()},
          {"opacity", column->getOpacity() / 255.0},
          {"last_frame", column->getMaxFrame()}};
}

QJsonObject describeCamera(Document& document) {
  TStageObjectTree* objects = tree(document);
  TCamera* camera           = objects->getCurrentCamera();
  TStageObject* object =
      objects->getStageObject(objects->getCurrentCameraId(), false);
  return {{"object_id", document.idForObject(object)},
          {"width_px", camera->getRes().lx},
          {"height_px", camera->getRes().ly},
          {"width_in", camera->getSize().lx},
          {"height_in", camera->getSize().ly}};
}

QJsonObject describeScene(Document& document) {
  QJsonArray columns, levels, objects;
  TXsheet* sheet = document.scene->getXsheet();
  for (int index = 0; index < sheet->getColumnCount(); ++index)
    if (TXshColumn* column = sheet->getColumn(index))
      columns.append(describeColumn(document, column));
  TLevelSet* cast = document.scene->getLevelSet();
  for (int index = 0; index < cast->getLevelCount(); ++index)
    levels.append(describeLevel(document, cast->getLevel(index)));
  for (int index = 0; index < tree(document)->getStageObjectCount(); ++index)
    objects.append(
        describeObject(document, tree(document)->getStageObject(index)));
  return {
      {"document_id", document.id},
      {"revision", document.revisionText()},
      {"name", QString::fromStdWString(document.scene->getSceneName())},
      {"frame_rate",
       document.scene->getProperties()->getOutputProperties()->getFrameRate()},
      {"frame_count", document.scene->getFrameCount()},
      {"frame_origin", 0},
      {"camera", describeCamera(document)},
      {"columns", columns},
      {"levels", levels},
      {"objects", objects},
      {"mode", "detached"}};
}

struct Settings {
  int width, height;
  double widthIn, heightIn, fps;
};

Settings readSettings(const QJsonObject& params, ToonzScene& scene) {
  TCamera* camera = scene.getCurrentCamera();
  Settings settings;
  settings.width =
      integerParam(params, "width_px", 1, 8192, camera->getRes().lx);
  settings.height =
      integerParam(params, "height_px", 1, 8192, camera->getRes().ly);
  if (qint64(settings.width) * settings.height > MaxPixels)
    throw Error("resource_limit",
                "Camera resolution exceeds the 16 megapixel limit.");
  settings.widthIn =
      numberParam(params, "width_in", 0.001, 10000, camera->getSize().lx);
  settings.heightIn =
      numberParam(params, "height_in", 0.001, 10000, camera->getSize().ly);
  const double fps =
      scene.getProperties()->getOutputProperties()->getFrameRate();
  settings.fps = numberParam(params, "frame_rate", 1, 240, fps);
  return settings;
}

void applySettings(ToonzScene& scene, const Settings& settings) {
  scene.getCurrentCamera()->setSize(
      TDimensionD(settings.widthIn, settings.heightIn));
  scene.getCurrentCamera()->setRes(TDimension(settings.width, settings.height));
  scene.getProperties()->getOutputProperties()->setFrameRate(settings.fps);
  scene.getProperties()->getPreviewProperties()->setFrameRate(settings.fps);
}

QJsonValue createScene(Context& context, const QJsonObject& params) {
  const QString name = nameParam(params);
  if (name.contains('/') || name.contains('\\') || name == "." || name == "..")
    throw Error("invalid_argument",
                "Scene name must be a name without path separators.");
  auto scene = std::make_unique<ToonzScene>();
  scene->setProject(context.project());
  scene->setScenePath(TFilePath(
      QDir(context.workspace()).filePath(name + ".tnz").toStdWString()));
  applySettings(*scene, {1920, 1080, 16.0, 9.0, 24.0});
  applySettings(*scene, readSettings(params, *scene));
  context.markMutationStarted();
  Document& document = context.addDocument(std::move(scene));
  return describeScene(document);
}

QJsonValue sceneSummary(Context& context, const QJsonObject& params) {
  return describeScene(getDocument(context, params));
}

QJsonValue updateSettings(Context& context, const QJsonObject& params) {
  Document& document      = getDocument(context, params);
  const Settings settings = readSettings(params, *document.scene);
  const Settings current  = readSettings({}, *document.scene);
  if (settings.width == current.width && settings.height == current.height &&
      settings.widthIn == current.widthIn &&
      settings.heightIn == current.heightIn && settings.fps == current.fps)
    return describeScene(document);
  TStageObject* camera = tree(document)->getStageObject(
      tree(document)->getCurrentCameraId(), false);
  requireEditable(document, camera);
  context.markMutationStarted();
  applySettings(*document.scene, settings);
  return describeScene(document);
}

QJsonValue updateCamera(Context& context, const QJsonObject& params) {
  updateSettings(context, params);
  return describeCamera(getDocument(context, params));
}

QJsonValue discardScene(Context& context, const QJsonObject& params) {
  const QString id = getDocument(context, params).id;
  context.markMutationStarted();
  context.removeDocument(id);
  return QJsonObject{{"document_id", id}, {"discarded", true}};
}

QJsonValue inspectProject(Context& context, const QJsonObject&) {
  return QJsonObject{{"mode", "private_workspace"},
                     {"documents", context.documents()},
                     {"arbitrary_scene_open", false},
                     {"live_gui_mutation", false}};
}

TFilePath newLevelPath(Context& context, const QString& extension) {
  const QString directory = context.resolvePath("levels", true);
  if (!QDir().mkpath(directory))
    throw Error("io_error", "Cannot create the workspace level directory.");
  const QString id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString separator = extension == "png" ? ".." : ".";
  return TFilePath(
      QDir(directory).filePath(id + separator + extension).toStdWString());
}

std::unique_ptr<TXshSimpleLevel> prepareLevel(Context& context,
                                              Document& document,
                                              const QJsonObject& params) {
  const QString name = nameParam(params);
  TLevelSet* cast    = document.scene->getLevelSet();
  if (cast->getLevelCount() >= MaxEntities)
    throw Error("resource_limit", "The document already contains 4096 levels.");
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
  return describeLevel(document, result);
}

void validatePngPrecision(const QString& path) {
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly))
    throw Error("io_error", "Cannot read the source PNG file.");
  const QByteArray header    = input.read(29);
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

QImage readPng(Context& context, const QJsonObject& params) {
  const QString path =
      context.resolvePath(requiredString(params, "path"), false);
  const QFileInfo info(path);
  if (!info.isFile() || info.size() > 67108864)
    throw Error("invalid_asset",
                "Import requires a regular PNG file no larger than 64 MiB.");
  validatePngPrecision(path);
  QImageReader reader(path, "png");
  reader.setAutoDetectImageFormat(false);
  const QSize size = reader.size();
  if (size.isEmpty() || size.width() > 8192 || size.height() > 8192 ||
      qint64(size.width()) * size.height() > MaxPixels)
    throw Error("resource_limit",
                "PNG dimensions must be 1..8192 per axis and no more than "
                "16 megapixels.");
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
  const QImage image     = readPng(context, params);
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
  return describeLevel(document, result);
}

QJsonValue inspectLevel(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  return describeLevel(document,
                       document.level(requiredString(params, "level_id")));
}

QJsonValue createColumn(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  const QString name = nameParam(params);
  TXsheet* sheet     = document.scene->getXsheet();
  const int index    = sheet->getColumnCount();
  if (index >= MaxEntities)
    throw Error("resource_limit", "The document already has 4096 columns.");
  context.markMutationStarted();
  sheet->insertColumn(index, new TXshLevelColumn());
  TXshColumn* column = sheet->getColumn(index);
  column->setPreviewVisible(true);
  column->setCamstandVisible(true);
  sheet->getStageObject(TStageObjectId::ColumnId(index))
      ->setName(name.toStdString());
  return describeColumn(document, column);
}

bool changesColumnContent(const QJsonObject& params) {
  return params.contains("name") || params.contains("visible") ||
         params.contains("opacity");
}

bool matchesColumn(TXshColumn* column, TStageObject* object,
                   const QJsonObject& value) {
  return value.value("name").toString().toStdString() == object->getName() &&
         value.value("visible").toBool() == column->isPreviewVisible() &&
         value.value("visible").toBool() == column->isCamstandVisible() &&
         value.value("locked").toBool() == column->isLocked() &&
         value.value("alpha").toInt() == column->getOpacity();
}

QJsonValue updateColumn(Context& context, const QJsonObject& params) {
  Document& document   = getDocument(context, params);
  TXshColumn* column   = document.column(requiredString(params, "column_id"));
  TStageObject* object = tree(document)->getStageObject(
      TStageObjectId::ColumnId(columnIndex(document, column)), false);
  const QString name = params.contains("name")
                           ? nameParam(params)
                           : QString::fromStdString(object->getName());
  const bool visible = boolParam(params, "visible", column->isPreviewVisible());
  const bool locked  = boolParam(params, "locked", column->isLocked());
  const double opacity =
      numberParam(params, "opacity", 0, 1, column->getOpacity() / 255.0);
  if (changesColumnContent(params)) requireEditable(document, object);
  const UCHAR alpha = static_cast<UCHAR>(std::lround(opacity * 255));
  const QJsonObject updated{{"name", name},
                            {"visible", visible},
                            {"locked", locked},
                            {"alpha", alpha}};
  if (matchesColumn(column, object, updated))
    return describeColumn(document, column);
  context.markMutationStarted();
  object->setName(name.toStdString());
  column->setPreviewVisible(visible);
  column->setCamstandVisible(visible);
  column->setOpacity(alpha);
  column->lock(locked);
  return describeColumn(document, column);
}

QJsonValue deleteColumn(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  const QString id   = requiredString(params, "column_id");
  TXshColumn* column = document.column(id);
  if (column->getColumnType() != TXshColumn::eLevelType)
    throw Error("incompatible_column",
                "Use the owning subsystem to delete this column type.");
  const int index = columnIndex(document, column);
  TStageObject* object =
      tree(document)->getStageObject(TStageObjectId::ColumnId(index), false);
  requireEditable(document, object);
  if (object->hasChildren())
    throw Error("has_dependents",
                "Reparent the column's children before deleting it.");
  const QString objectId = document.idForObject(object);
  TFx* fx                = column->getFx();
  context.markMutationStarted();
  for (auto entry = document.effects.begin();
       entry != document.effects.end();) {
    if (entry.value() == fx)
      entry = document.effects.erase(entry);
    else
      ++entry;
  }
  if (fx) document.scene->getXsheet()->getFxDag()->removeFxId(fx);
  document.columns.remove(id);
  document.objects.remove(objectId);
  document.scene->getXsheet()->removeColumn(index);
  return QJsonObject{{"column_id", id}, {"deleted", true}};
}

struct CellRange {
  int start, length;
};

CellRange readRange(const QJsonObject& params) {
  const int start  = integerParam(params, "start_frame", 0, MaxFrame, -1);
  const int length = integerParam(params, "length", 1, MaxSpan, -1);
  if (start < 0 || length < 1 || start > MaxFrame - length + 1)
    throw Error("invalid_argument",
                "The exposure interval exceeds the supported frame range.");
  return {start, length};
}

void validateDrawing(TXshLevel* level, const TFrameId& frame) {
  std::vector<TFrameId> drawings;
  level->getFids(drawings);
  if (std::find(drawings.begin(), drawings.end(), frame) == drawings.end())
    throw Error("missing_drawing",
                "The requested drawing does not exist in the level.");
}

bool preflightCells(TXsheet* sheet, int index, const CellRange& range,
                    const TXshCell& cell, bool overwrite) {
  bool changed = false;
  for (int row = range.start; row < range.start + range.length; ++row) {
    const TXshCell& existing = sheet->getCell(row, index);
    if (!overwrite && !existing.isEmpty())
      throw Error("occupied_cells",
                  "The interval contains cells; set overwrite to replace.");
    if (existing != cell) changed = true;
  }
  return changed;
}

QJsonValue exposeColumn(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  TXshColumn* column = document.column(requiredString(params, "column_id"));
  const int index    = columnIndex(document, column);
  TStageObject* object =
      tree(document)->getStageObject(TStageObjectId::ColumnId(index), false);
  requireEditable(document, object);
  TXshLevel* level     = document.level(requiredString(params, "level_id"));
  const TFrameId frame = readDrawingId(params);
  validateDrawing(level, frame);
  const CellRange range = readRange(params);
  const TXshCell cell(level, frame);
  TXshLevelColumn* target = column->getLevelColumn();
  if (!target || !target->canSetCell(cell))
    throw Error("incompatible_column",
                "This column cannot expose the requested level type.");
  const bool overwrite = boolParam(params, "overwrite", false);
  TXsheet* sheet       = document.scene->getXsheet();
  if (preflightCells(sheet, index, range, cell, overwrite)) {
    const std::vector<TXshCell> cells(range.length, cell);
    const std::string name = object->getName();
    context.markMutationStarted();
    if (!sheet->setCells(range.start, index, range.length, cells.data()))
      throw Error("native_error",
                  "The preflighted native exposure could not be applied.");
    // Native exposure can rename columns according to the GUI preference.
    object->setName(name);
  }
  return QJsonObject{{"column_id", document.idForColumn(column)},
                     {"start_frame", range.start},
                     {"length", range.length},
                     {"level_id", document.idForLevel(level)},
                     {"drawing_id", drawingId(frame)}};
}

QJsonValue inspectCells(Context& context, const QJsonObject& params) {
  Document& document    = getDocument(context, params);
  TXshColumn* column    = document.column(requiredString(params, "column_id"));
  const int index       = columnIndex(document, column);
  const CellRange range = readRange(params);
  QJsonArray cells;
  for (int row = range.start; row < range.start + range.length; ++row) {
    const TXshCell& cell = document.scene->getXsheet()->getCell(row, index);
    QJsonObject value{{"frame", row}, {"empty", cell.isEmpty()}};
    if (!cell.isEmpty()) {
      value.insert("level_id", document.idForLevel(cell.m_level.getPointer()));
      value.insert("drawing_id", drawingId(cell.m_frameId));
    }
    cells.append(value);
  }
  return QJsonObject{{"column_id", document.idForColumn(column)},
                     {"cells", cells}};
}

QJsonValue inspectCamera(Context& context, const QJsonObject& params) {
  return describeCamera(getDocument(context, params));
}

QJsonValue createObject(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  const QString name = nameParam(params);
  if (requiredString(params, "kind") != "pegbar")
    throw Error("unsupported_kind",
                "Only pegbar creation is currently supported.");
  TStageObjectTree* objects = tree(document);
  if (objects->getStageObjectCount() >= MaxEntities)
    throw Error("resource_limit",
                "The document already contains 4096 stage objects.");
  int index = 0;
  while (objects->getStageObject(TStageObjectId::PegbarId(index), false))
    ++index;
  context.markMutationStarted();
  TStageObject* object =
      objects->getStageObject(TStageObjectId::PegbarId(index));
  object->setName(name.toStdString());
  return describeObject(document, object);
}

void checkParentCycle(Document& document, TStageObject* object,
                      TStageObject* parent) {
  TStageObject* ancestor = parent;
  for (int depth = 0; ancestor && depth <= MaxEntities; ++depth) {
    if (ancestor == object)
      throw Error("cycle", "The requested parent would create a cycle.");
    ancestor = tree(document)->getStageObject(ancestor->getParent(), false);
  }
  if (ancestor) throw Error("cycle", "The existing parent chain is cyclic.");
}

void checkParent(Document& document, TStageObject* object,
                 TStageObject* parent) {
  if (object->getId().isTable())
    throw Error("invalid_parent", "The table cannot be reparented.");
  if (object->getId().isPegbar() && parent->getId().isColumn())
    throw Error("invalid_parent", "A pegbar cannot be parented to a column.");
  if (parent->getId().isColumn()) {
    TXshColumn* column =
        document.scene->getXsheet()->getColumn(parent->getId().getIndex());
    if (!column || !column->canBeParent())
      throw Error("invalid_parent", "This column cannot parent stage objects.");
  }
  checkParentCycle(document, object, parent);
}

QJsonValue parentObject(Context& context, const QJsonObject& params) {
  Document& document   = getDocument(context, params);
  TStageObject* object = document.object(requiredString(params, "object_id"));
  TStageObject* parent = document.object(requiredString(params, "parent_id"));
  requireEditable(document, object);
  checkParent(document, object, parent);
  if (object->getParent() == parent->getId())
    return describeObject(document, object);
  context.markMutationStarted();
  object->setParent(parent->getId());
  return describeObject(document, object);
}

struct Channel {
  TStageObject::Channel native;
  QString unit;
};

Channel readChannel(const QJsonObject& params) {
  static const std::map<QString, Channel> channels = {
      {"x", {TStageObject::T_X, "inch"}},
      {"y", {TStageObject::T_Y, "inch"}},
      {"z", {TStageObject::T_Z, "native_z"}},
      {"rotation", {TStageObject::T_Angle, "degree"}},
      {"scale_x", {TStageObject::T_ScaleX, "scalar"}},
      {"scale_y", {TStageObject::T_ScaleY, "scalar"}},
      {"scale", {TStageObject::T_Scale, "scalar"}}};
  const auto channel = channels.find(requiredString(params, "channel"));
  if (channel == channels.end())
    throw Error("unsupported_channel", "Unsupported animation channel.");
  if (requiredString(params, "unit") != channel->second.unit)
    throw Error("invalid_unit",
                "This channel requires unit " + channel->second.unit + ".");
  return channel->second;
}

QJsonArray readArray(const QJsonObject& params, const QString& name) {
  if (!params.value(name).isArray())
    throw Error("invalid_argument", name + " must be an array.");
  const QJsonArray values = params.value(name).toArray();
  if (values.isEmpty() || values.size() > MaxSpan)
    throw Error("resource_limit",
                name + " must contain between 1 and 10000 items.");
  return values;
}

std::vector<int> readFrames(const QJsonObject& params) {
  std::vector<int> frames;
  QSet<int> seen;
  for (const QJsonValue& value : readArray(params, "frames")) {
    const int frame =
        integerParam({{"frame", value}}, "frame", 0, MaxFrame, -1);
    if (seen.contains(frame))
      throw Error("invalid_argument", "frames must not contain duplicates.");
    seen.insert(frame);
    frames.push_back(frame);
  }
  return frames;
}

TDoubleKeyframe readKey(const QJsonValue& value) {
  if (!value.isObject())
    throw Error("invalid_argument", "Each key must be an object.");
  const QJsonObject item = value.toObject();
  const int frame        = integerParam(item, "frame", 0, MaxFrame, -1);
  if (frame < 0 || !item.contains("value"))
    throw Error("invalid_argument",
                "Each key needs a frame and numeric value.");
  const double number = numberParam(item, "value", -1000000, 1000000, 0);
  const QString interpolation = requiredString(item, "interpolation");
  if (interpolation != "linear" && interpolation != "constant")
    throw Error("unsupported_interpolation",
                "Use explicit linear or constant interpolation.");
  TDoubleKeyframe key(frame, number);
  key.m_isKeyframe = true;
  key.m_type       = interpolation == "linear" ? TDoubleKeyframe::Linear
                                               : TDoubleKeyframe::Constant;
  key.m_prevType   = key.m_type;
  key.m_step       = 1;
  return key;
}

std::vector<TDoubleKeyframe> readKeys(const QJsonObject& params) {
  std::vector<TDoubleKeyframe> keys;
  QSet<int> seen;
  for (const QJsonValue& value : readArray(params, "keys")) {
    const TDoubleKeyframe key = readKey(value);
    const int frame           = static_cast<int>(key.m_frame);
    if (seen.contains(frame))
      throw Error("invalid_argument", "Key frames must be unique.");
    seen.insert(frame);
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

QJsonValue inspectKeys(Context& context, const QJsonObject& params) {
  Document& document    = getDocument(context, params);
  TStageObject* object  = document.object(requiredString(params, "object_id"));
  const Channel channel = readChannel(params);
  TDoubleParam* curve   = object->getParam(channel.native);
  QJsonArray keys;
  for (int index = 0; index < curve->getKeyframeCount(); ++index) {
    const TDoubleKeyframe& key = curve->getKeyframe(index);
    QString interpolation      = "unsupported";
    if (key.m_type == TDoubleKeyframe::Constant) interpolation = "constant";
    if (key.m_type == TDoubleKeyframe::Linear) interpolation = "linear";
    keys.append(QJsonObject{{"frame", key.m_frame},
                            {"value", key.m_value},
                            {"interpolation", interpolation}});
  }
  return QJsonObject{{"object_id", document.idForObject(object)},
                     {"channel", params.value("channel")},
                     {"unit", channel.unit},
                     {"keys", keys}};
}

bool preflightKeys(TDoubleParam* curve,
                   const std::vector<TDoubleKeyframe>& keys) {
  int count    = curve->getKeyframeCount();
  bool changed = false;
  for (const TDoubleKeyframe& key : keys) {
    if (!curve->isKeyframe(key.m_frame)) {
      ++count;
      changed = true;
      continue;
    }
    const TDoubleKeyframe& current = curve->getKeyframeAt(key.m_frame);
    changed = changed || key.m_type != current.m_type ||
              key.m_value != current.m_value || current.m_step != 1;
  }
  if (count > MaxSpan)
    throw Error("resource_limit",
                "An animation channel may contain at most 10000 keys.");
  return changed;
}

QJsonValue setKeys(Context& context, const QJsonObject& params) {
  Document& document   = getDocument(context, params);
  TStageObject* object = document.object(requiredString(params, "object_id"));
  requireEditable(document, object);
  const Channel channel                   = readChannel(params);
  const std::vector<TDoubleKeyframe> keys = readKeys(params);
  TDoubleParam* curve                     = object->getParam(channel.native);
  if (curve->isEditorLocked())
    throw Error("locked", "The animation channel is locked.");
  if (!preflightKeys(curve, keys)) return inspectKeys(context, params);
  context.markMutationStarted();
  for (const TDoubleKeyframe& key : keys) curve->setKeyframe(key);
  return inspectKeys(context, params);
}

QJsonValue deleteKeys(Context& context, const QJsonObject& params) {
  Document& document   = getDocument(context, params);
  TStageObject* object = document.object(requiredString(params, "object_id"));
  requireEditable(document, object);
  const Channel channel         = readChannel(params);
  const std::vector<int> frames = readFrames(params);
  TDoubleParam* curve           = object->getParam(channel.native);
  if (curve->isEditorLocked())
    throw Error("locked", "The animation channel is locked.");
  for (const int frame : frames)
    if (!curve->isKeyframe(frame))
      throw Error("missing_key", "Every requested key must exist.");
  context.markMutationStarted();
  for (const int frame : frames) curve->deleteKeyframe(frame);
  return inspectKeys(context, params);
}

QJsonValue sampleAnimation(Context& context, const QJsonObject& params) {
  Document& document    = getDocument(context, params);
  TStageObject* object  = document.object(requiredString(params, "object_id"));
  const Channel channel = readChannel(params);
  const std::vector<int> frames = readFrames(params);
  QJsonArray values;
  for (const int frame : frames)
    values.append(QJsonObject{
        {"frame", frame}, {"value", object->getParam(channel.native, frame)}});
  return QJsonObject{{"object_id", document.idForObject(object)},
                     {"channel", params.value("channel")},
                     {"unit", channel.unit},
                     {"values", values}};
}

QJsonObject cameraSchema() {
  return {{"width_px", integerSchema(1, 8192)},
          {"height_px", integerSchema(1, 8192)},
          {"width_in", numberSchema(0.001, 10000)},
          {"height_in", numberSchema(0.001, 10000)}};
}

QJsonObject merge(QJsonObject target, const QJsonObject& source) {
  for (auto item = source.begin(); item != source.end(); ++item)
    target.insert(item.key(), item.value());
  return target;
}

void addSceneOperations(QJsonArray& catalog) {
  QJsonObject settings = cameraSchema();
  settings.insert("frame_rate", numberSchema(1, 240));
  catalog.append(
      operation("project.inspect",
                "Inspect the worker's private project and documents.", {}, {}));
  catalog.append(
      operation("scene.create",
                "Create an isolated native scene. Timeline frames start at 0.",
                merge(settings, {{"name", textSchema()}}), {"name"}, true));
  catalog.append(
      operation("scene.summary",
                "Inspect native scene, levels, columns and stage objects.",
                documentSchema(), {"document_id"}));
  catalog.append(operation(
      "scene.settings", "Set explicit frame rate, pixels and physical inches.",
      merge(documentSchema(), settings), {"document_id"}, true));
  catalog.append(
      operation("scene.discard",
                "Discard a detached document and invalidate its handles.",
                documentSchema(), {"document_id"}, true));
  catalog.append(operation(
      "camera.inspect", "Inspect active camera resolution and size in inches.",
      documentSchema(), {"document_id"}));
  catalog.append(operation(
      "camera.update", "Set camera resolution and physical size in inches.",
      merge(documentSchema(), cameraSchema()), {"document_id"}, true));
}

void addLevelOperations(QJsonArray& catalog) {
  QJsonObject level  = merge(documentSchema(), {{"name", textSchema()}});
  QJsonObject create = level;
  create.insert("format", enumSchema({"vector", "rgba8", "toonzRaster"}));
  catalog.append(operation(
      "level.create", "Create an empty native level owned by this workspace.",
      create, {"document_id", "name", "format"}, true));
  level.insert("path", textSchema());
  level.insert("dpi", numberSchema(1, 2400));
  catalog.append(operation(
      "level.import", "Import one PNG as a private copy; drawing number is 1.",
      level, {"document_id", "path", "name"}, true));
  catalog.append(
      operation("level.inspect",
                "Inspect a level's format and exact drawing identifiers.",
                merge(documentSchema(), {{"level_id", textSchema()}}),
                {"document_id", "level_id"}));
}

void addColumnOperations(QJsonArray& catalog) {
  QJsonObject column = merge(documentSchema(), {{"column_id", textSchema()}});
  catalog.append(operation("column.create",
                           "Append an empty level column with a stable handle.",
                           merge(documentSchema(), {{"name", textSchema()}}),
                           {"document_id", "name"}, true));
  QJsonObject update =
      merge(column, {{"name", textSchema()},
                     {"visible", QJsonObject{{"type", "boolean"}}},
                     {"locked", QJsonObject{{"type", "boolean"}}},
                     {"opacity", numberSchema(0, 1)}});
  catalog.append(operation("column.update",
                           "Set column name, visibility, lock or opacity.",
                           update, {"document_id", "column_id"}, true));
  catalog.append(operation("column.delete",
                           "Delete an unlocked column without stage children.",
                           column, {"document_id", "column_id"}, true));
  QJsonObject range =
      merge(column, {{"start_frame", integerSchema(0, MaxFrame)},
                     {"length", integerSchema(1, MaxSpan)}});
  catalog.append(operation(
      "column.cells", "Read an interval of zero-based timeline cells.", range,
      {"document_id", "column_id", "start_frame", "length"}));
  const QJsonObject drawing{
      {"type", "object"},
      {"additionalProperties", false},
      {"properties",
       QJsonObject{{"number", integerSchema(1, 999999)},
                   {"suffix", QJsonObject{{"type", "string"},
                                          {"pattern", "^[a-z]{0,8}$"}}}}},
      {"required", QJsonArray{"number", "suffix"}}};
  range.insert("drawing_id", drawing);
  range.insert("level_id", textSchema());
  range.insert("overwrite", QJsonObject{{"type", "boolean"}});
  catalog.append(operation(
      "column.expose",
      "Expose an existing drawing as a hold; frames start at 0.", range,
      {"document_id", "column_id", "level_id", "drawing_id", "start_frame",
       "length"},
      true));
}

void addAnimationOperations(QJsonArray& catalog) {
  QJsonObject object = merge(documentSchema(), {{"object_id", textSchema()}});
  catalog.append(operation(
      "object.create", "Create a native pegbar with a stable object handle.",
      merge(documentSchema(),
            {{"name", textSchema()}, {"kind", enumSchema({"pegbar"})}}),
      {"document_id", "name", "kind"}, true));
  catalog.append(operation(
      "object.parent", "Reparent an object, preserving its local animation.",
      merge(object, {{"parent_id", textSchema()}}),
      {"document_id", "object_id", "parent_id"}, true));
  QJsonObject animation = merge(
      object, {{"channel", enumSchema({"x", "y", "z", "rotation", "scale_x",
                                       "scale_y", "scale"})},
               {"unit", enumSchema({"inch", "degree", "scalar", "native_z"})}});
  const QJsonArray required{"document_id", "object_id", "channel", "unit"};
  catalog.append(operation("animation.keys",
                           "Read native curve keys with explicit units.",
                           animation, required));
  QJsonArray withFrames = required;
  withFrames.append("frames");
  const QJsonObject frames =
      merge(animation, {{"frames", arraySchema(integerSchema(0, MaxFrame))}});
  catalog.append(operation(
      "animation.sample", "Sample effective native channel values at frame 0+.",
      frames, withFrames));
  catalog.append(operation("animation.delete_keys",
                           "Delete existing keys after complete validation.",
                           frames, withFrames, true));
  const QJsonObject key{
      {"type", "object"},
      {"additionalProperties", false},
      {"properties",
       QJsonObject{{"frame", integerSchema(0, MaxFrame)},
                   {"value", numberSchema(-1000000, 1000000)},
                   {"interpolation", enumSchema({"linear", "constant"})}}},
      {"required", QJsonArray{"frame", "value", "interpolation"}}};
  QJsonArray withKeys = required;
  withKeys.append("keys");
  catalog.append(operation(
      "animation.set_keys",
      "Insert or replace keys with explicit interpolation.",
      merge(animation, {{"keys", arraySchema(key)}}), withKeys, true));
}

}  // namespace

/// Return the typed catalog for detached scene and animation operations.
QJsonArray sceneOperations() {
  QJsonArray catalog;
  addSceneOperations(catalog);
  addLevelOperations(catalog);
  addColumnOperations(catalog);
  addAnimationOperations(catalog);
  return catalog;
}

/// Execute one scene operation against its explicitly addressed document.
///
/// Args:
///   context: Worker-owned project, document registry and path authority.
///   method: Catalog operation name.
///   params: Validated JSON arguments including document-scoped handles.
///
/// Returns:
///   Native inspection data or a typed mutation result.
///
/// Throws:
///   Error: A preflight constraint or native operation fails.
QJsonValue executeScene(Context& context, const QString& method,
                        const QJsonObject& params) {
  using Handler = QJsonValue (*)(Context&, const QJsonObject&);
  static const std::map<QString, Handler> handlers = {
      {"project.inspect", inspectProject},
      {"scene.create", createScene},
      {"scene.summary", sceneSummary},
      {"scene.settings", updateSettings},
      {"scene.discard", discardScene},
      {"level.create", createLevel},
      {"level.import", importLevel},
      {"level.inspect", inspectLevel},
      {"column.create", createColumn},
      {"column.update", updateColumn},
      {"column.delete", deleteColumn},
      {"column.expose", exposeColumn},
      {"column.cells", inspectCells},
      {"camera.inspect", inspectCamera},
      {"camera.update", updateCamera},
      {"object.create", createObject},
      {"object.parent", parentObject},
      {"animation.keys", inspectKeys},
      {"animation.set_keys", setKeys},
      {"animation.delete_keys", deleteKeys},
      {"animation.sample", sampleAnimation}};
  const auto handler = handlers.find(method);
  if (handler == handlers.end())
    throw Error("unknown_method", "Unknown scene operation: " + method);
  return handler->second(context, params);
}

}  // namespace GroundzeroAutomation
