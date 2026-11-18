#include "automation.h"
#include "automation_audio.h"
#include "automation_drawing.h"
#include "automation_hook.h"
#include "automation_vector_opacity.h"
#include "tstrokeopacity.h"

#include "tcolorstyles.h"
#include "tgeometry.h"
#include "timage_io.h"
#include "timageinfo.h"
#include "tpalette.h"
#include "trasterimage.h"
#include "tregion.h"
#include "tsimplecolorstyles.h"
#include "tstroke.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <typeinfo>
#include <utility>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int kMaxPixels         = 16777216;
constexpr int kMaxPatch          = 16384;
constexpr int kMaxStrokes        = 4096;
constexpr int kMaxPoints         = 1025;
constexpr int kMaxStyles         = 4096;
constexpr int kMaxPages          = 128;
constexpr int kMaxVectorFrame    = 65535;
constexpr double kMaxCoordinate  = 32768.0;
constexpr double kMaxThickness   = 255.0;
constexpr int kMaxTransformEdges = 65536;

[[noreturn]] void invalid(const QString& message) {
  throw Error("invalid_params", message);
}

void keys(const QJsonObject& object, const QStringList& allowed) {
  for (auto it = object.begin(); it != object.end(); ++it)
    if (!allowed.contains(it.key())) invalid("Unknown field: " + it.key());
}

double number(const QJsonObject& object, const QString& key, double minimum,
              double maximum) {
  const auto value    = object.value(key);
  const double result = value.toDouble();
  if (!value.isDouble() || !std::isfinite(result) || result < minimum ||
      result > maximum)
    invalid(key + " must be a finite number in the advertised range");
  return result;
}

int integer(const QJsonObject& object, const QString& key, int minimum,
            int maximum) {
  const double value = number(object, key, minimum, maximum);
  if (value != std::floor(value)) invalid(key + " must be an integer");
  return static_cast<int>(value);
}

bool boolean(const QJsonObject& object, const QString& key,
             bool fallback = false) {
  if (!object.contains(key)) return fallback;
  if (!object.value(key).isBool()) invalid(key + " must be a boolean");
  return object.value(key).toBool();
}

QJsonArray array(const QJsonObject& object, const QString& key, int minimum,
                 int maximum) {
  const auto value = object.value(key);
  if (!value.isArray()) invalid(key + " must be an array");
  const auto result = value.toArray();
  if (result.size() < minimum || result.size() > maximum)
    invalid(key + " has an unsupported item count");
  return result;
}

TFrameId frameId(const QJsonObject& params, const QString& key = "drawing_id") {
  if (!params.value(key).isObject())
    invalid(key + " must be a drawing identity object");
  const auto value = params.value(key).toObject();
  keys(value, {"number", "suffix"});
  const int frame   = integer(value, "number", 1, 999999);
  const auto suffix = value.value("suffix");
  if (!suffix.isString() ||
      !QRegularExpression(QRegularExpression::anchoredPattern("[a-z]{0,8}"))
           .match(suffix.toString())
           .hasMatch())
    invalid(
        "Drawing suffix must contain zero to eight lowercase ASCII letters");
  return TFrameId(frame, suffix.toString());
}

void validateFrameNumber(TXshSimpleLevel* level, const TFrameId& frame) {
  if (level->getType() == PLI_XSHLEVEL && frame.getNumber() > kMaxVectorFrame)
    throw Error("unsupported_drawing_id",
                "Native PLI drawing numbers must be between 1 and 65535");
}

QJsonObject frameJson(const TFrameId& frame) {
  return {{"number", frame.getNumber()}, {"suffix", frame.getLetter()}};
}

TXshSimpleLevel* simpleLevel(Document& document, const QJsonObject& params) {
  auto* level =
      document.level(requiredString(params, "level_id"))->getSimpleLevel();
  if (!level)
    throw Error("unsupported_type",
                "Drawing operations require a simple level");
  return level;
}

TImageP imageFor(TXshSimpleLevel* level, const TFrameId& frame) {
  if (!level->isFid(frame))
    throw Error("not_found", "Drawing identity does not exist");
  const auto image = level->getFrame(frame, false);
  if (!image)
    throw Error("asset_unavailable", "The native drawing could not be loaded");
  return image;
}

void sharing(const QJsonObject& params) {
  if (requiredString(params, "sharing") != "all_exposures")
    invalid(
        "sharing must be all_exposures; clone the drawing to edit an "
        "independent use");
}

TPalette* paletteFor(TXshSimpleLevel* level, bool write = false) {
  if (level->getType() != PLI_XSHLEVEL && level->getType() != TZP_XSHLEVEL)
    throw Error("unsupported_type",
                "Palette operations require a vector or Toonz raster level");
  auto* palette = level->getPalette();
  if (!palette)
    throw Error("unsupported_type", "This level has no native palette");
  if (write && palette->isLocked())
    throw Error("locked", "The level palette is locked");
  return palette;
}

int styleId(const QJsonObject& params, TPalette* palette,
            const QString& key = "style_id") {
  const int id = integer(params, key, 0,
                         std::min(kMaxStyles, palette->getStyleCount()) - 1);
  if (!palette->getStyle(id)) invalid("The palette style does not exist");
  return id;
}

QString imageFormat(const TImageP& image) {
  if (TVectorImageP(image)) return "vector";
  if (TToonzImageP(image)) return "toonzRaster";
  const TRasterImageP raster = image;
  if (raster && TRaster32P(raster->getRaster())) return "rgba8";
  throw Error("unsupported_type",
              "Only vector, RGBA8 and Toonz raster drawings are admitted");
}

QJsonObject imageSummary(TXshSimpleLevel* level, const TFrameId& frame) {
  const auto image     = imageFor(level, frame);
  const QString format = imageFormat(image);
  QJsonObject result{{"drawing_id", frameJson(frame)},
                     {"format", format},
                     {"sharing", "all_exposures"}};
  const TVectorImageP vector = image;
  if (vector) {
    result.insert("coordinate_space", "drawing_native");
    result.insert("stroke_count", static_cast<int>(vector->getStrokeCount()));
    result.insert("thickness", "native_radius");
    return result;
  }
  double dx = 0.0, dy = 0.0;
  const TRasterImageP raster = image;
  if (raster)
    raster->getDpi(dx, dy);
  else
    TToonzImageP(image)->getDpi(dx, dy);
  result.insert("width", image->raster()->getLx());
  result.insert("height", image->raster()->getLy());
  result.insert("dpi", QJsonArray{dx, dy});
  result.insert("coordinate_space", "raster_pixels_lower_left");
  result.insert("channels",
                format == "rgba8" ? "rgba8_premultiplied" : "ink_paint_tone");
  return result;
}

void publish(Context& context, TXshSimpleLevel* level, const TFrameId& frame,
             const TImageP& image) {
  context.markMutationStarted();
  level->setFrame(frame, image);
  level->touchFrame(frame);
}

void rasterDimensions(int width, int height) {
  if (width < 1 || height < 1 || width > 8192 || height > 8192 ||
      static_cast<qint64>(width) * height > kMaxPixels)
    invalid(
        "Raster dimensions must be at most 8192 per axis and 16777216 pixels");
}

void compatibleRaster(TXshSimpleLevel* level, const TDimension& size,
                      double dpi) {
  if (level->isEmpty()) return;
  const auto* properties = level->getProperties();
  const auto levelDpi =
      properties->getDpiPolicy() == LevelProperties::DP_CustomDpi
          ? properties->getDpi()
          : properties->getImageDpi();
  if (properties->getImageRes() != size || levelDpi != TPointD(dpi, dpi))
    invalid(
        "Raster drawings in this level must have identical dimensions and DPI");
}

void initializeRaster(Context& context, TXshSimpleLevel* level,
                      const TDimension& size, double dpi) {
  if (!level->isEmpty()) return;
  context.markMutationStarted();
  auto* properties = level->getProperties();
  properties->setImageRes(size);
  properties->setImageDpi(TPointD(dpi, dpi));
  properties->setDpi(TPointD(dpi, dpi));
  properties->setDpiPolicy(LevelProperties::DP_ImageDpi);
}

TImageP emptyVector(TXshSimpleLevel* level, const QJsonObject& params) {
  if (level->getType() != PLI_XSHLEVEL)
    invalid("Vector drawings require a vector level");
  if (params.contains("width") || params.contains("height") ||
      params.contains("dpi"))
    invalid("Vector drawings do not accept raster dimensions or DPI");
  requireAssetGrowth(*level->getScene(), AutomationDrawingOverheadBytes, 1);
  return TVectorImageP(new TVectorImage);
}

TImageP emptyImage(Context& context, TXshSimpleLevel* level,
                   const QJsonObject& params) {
  const QString format = requiredString(params, "format");
  if (format == "vector") return emptyVector(level, params);
  if ((format != "rgba8" || level->getType() != OVL_XSHLEVEL) &&
      (format != "toonzRaster" || level->getType() != TZP_XSHLEVEL))
    invalid("Drawing format must match the native level type");
  const int width  = integer(params, "width", 1, 8192);
  const int height = integer(params, "height", 1, 8192);
  const double dpi = number(params, "dpi", 1.0, 2400.0);
  rasterDimensions(width, height);
  compatibleRaster(level, TDimension(width, height), dpi);
  requireAssetGrowth(*level->getScene(), automationRasterBytes(width, height),
                     1);
  TImageP image;
  if (format == "rgba8") {
    TRaster32P raster(width, height);
    raster->clear();
    TRasterImageP result(raster);
    result->setDpi(dpi, dpi);
    result->setSavebox(raster->getBounds());
    image = result;
  } else {
    TRasterCM32P raster(width, height);
    raster->fill(TPixelCM32());
    TToonzImageP result(raster, raster->getBounds());
    result->setDpi(dpi, dpi);
    image = result;
  }
  initializeRaster(context, level, TDimension(width, height), dpi);
  return image;
}

QJsonObject createDrawing(Context& context, Document& document,
                          const QJsonObject& params) {
  auto* level      = simpleLevel(document, params);
  const auto frame = frameId(params);
  validateFrameNumber(level, frame);
  if (level->isFid(frame))
    throw Error("already_exists", "Drawing identity already exists");
  if (level->getFrameCount() >= 4096)
    throw Error("resource_limit", "Level already has 4096 drawings");
  const auto image = emptyImage(context, level, params);
  publish(context, level, frame, image);
  return imageSummary(level, frame);
}

QJsonObject cloneDrawing(Context& context, Document& document,
                         const QJsonObject& params) {
  auto* level            = simpleLevel(document, params);
  const auto source      = frameId(params);
  const auto destination = frameId(params, "destination_drawing_id");
  validateFrameNumber(level, destination);
  if (level->isFid(destination))
    throw Error("already_exists", "Destination drawing exists");
  if (level->getFrameCount() >= 4096)
    throw Error("resource_limit", "Level already has 4096 drawings");
  const auto image = imageFor(level, source);
  imageFormat(image);
  requireAssetGrowth(*document.scene, automationImageBytes(image), 1);
  publish(context, level, destination, TImageP(image->cloneImage()));
  auto result = imageSummary(level, destination);
  result.insert("source_hook_keys_not_copied",
                automationHookKeys(*level, source));
  return result;
}

struct DrawingExposure final {
  TXshCellColumn* column;
  TStageObject* object;
  int row;
  TXshCell cell;
};

struct ExposureScan final {
  TXshSimpleLevel* level;
  std::set<TFrameId> frames;
  bool firstOnly;
  qint64 remaining = 1000000;
  std::set<TXsheet*> visited;
  std::vector<DrawingExposure> exposures;
};

bool referencesInColumn(TXsheet* sheet, int index, ExposureScan& scan);

bool referencesInSheet(TXsheet* sheet, ExposureScan& scan) {
  if (!sheet || !scan.visited.insert(sheet).second) return false;
  if (scan.visited.size() > 128)
    throw Error("resource_limit", "Exposure scan exceeds 128 nested sheets");
  for (int index = 0; index < sheet->getColumnCount(); ++index)
    if (referencesInColumn(sheet, index, scan)) return true;
  return false;
}

bool referencesInCell(TXsheet* sheet, int index, int row, ExposureScan& scan) {
  auto* column     = sheet->getColumn(index)->getCellColumn();
  const auto& cell = column->getCell(row);
  if (cell.getSimpleLevel() == scan.level &&
      scan.frames.count(cell.m_frameId)) {
    auto* object = sheet->getStageObjectTree()->getStageObject(
        TStageObjectId::ColumnId(index), false);
    scan.exposures.push_back({column, object, row, cell});
    if (scan.firstOnly) return true;
  }
  auto* child = cell.getChildLevel();
  return child && referencesInSheet(child->getXsheet(), scan);
}

bool referencesInColumn(TXsheet* sheet, int index, ExposureScan& scan) {
  auto* entry = sheet->getColumn(index);
  // Sound clips expose no drawings, and their getCell() allocates per call.
  if (entry && entry->getSoundColumn()) return false;
  auto* column = entry ? entry->getCellColumn() : nullptr;
  int first = 0, last = -1;
  if (!column || !column->getRange(first, last)) return false;
  scan.remaining -= static_cast<qint64>(last) - first + 1;
  if (scan.remaining < 0)
    throw Error("resource_limit", "Exposure scan exceeds one million cells");
  for (int row = first; row <= last; ++row)
    if (referencesInCell(sheet, index, row, scan)) return true;
  return false;
}

void scanExposures(Document& document, ExposureScan& scan) {
  if (referencesInSheet(document.scene->getXsheet(), scan)) return;
  auto* levels = document.scene->getLevelSet();
  for (int i = 0; i < levels->getLevelCount(); ++i) {
    auto* child = levels->getLevel(i)->getChildLevel();
    if (child && referencesInSheet(child->getXsheet(), scan)) return;
  }
}

QJsonObject removeDrawing(Context& context, Document& document,
                          const QJsonObject& params) {
  auto* level      = simpleLevel(document, params);
  const auto frame = frameId(params);
  imageFor(level, frame);
  ExposureScan scan{level, {frame}, true, 1000000, {}, {}};
  scanExposures(document, scan);
  if (!scan.exposures.empty())
    throw Error("in_use", "Remove every exposure before removing this drawing");
  requireAutomationHookRemoval(document, *level, frame);
  const int hookKeys = automationHookKeys(*level, frame);
  context.markMutationStarted();
  level->eraseFrame(frame);
  level->setDirtyFlag(true);
  if (automationHookKeys(*level, frame) != 0)
    throw Error("native_error", "Native drawing removal kept hook keys");
  return {{"removed", frameJson(frame)}, {"hook_keys_removed", hookKeys}};
}

using DrawingMapping = std::map<TFrameId, TFrameId>;

DrawingMapping drawingMapping(TXshSimpleLevel* level,
                              const QJsonObject& params) {
  DrawingMapping mapping;
  for (const auto& item : array(params, "mapping", 1, 4096)) {
    if (!item.isObject()) invalid("Each mapping entry must be an object");
    const auto entry = item.toObject();
    keys(entry, {"drawing_id", "destination_drawing_id"});
    const auto source      = frameId(entry);
    const auto destination = frameId(entry, "destination_drawing_id");
    validateFrameNumber(level, destination);
    if (!level->isFid(source))
      throw Error("not_found", "A source drawing identity does not exist");
    if (!mapping.emplace(source, destination).second)
      invalid("A source drawing appears more than once in mapping");
  }
  return mapping;
}

std::vector<TFrameId> renamedFrames(TXshSimpleLevel* level,
                                    const DrawingMapping& mapping) {
  std::vector<TFrameId> frames = level->getFids();
  std::set<TFrameId> destinations;
  for (auto& frame : frames) {
    const auto renamed = mapping.find(frame);
    if (renamed != mapping.end()) frame = renamed->second;
    if (!destinations.insert(frame).second)
      throw Error("already_exists",
                  "A destination collides with another drawing; include all "
                  "moved drawings in the same mapping");
  }
  return frames;
}

void prepareRenumberExposures(ExposureScan& scan,
                              const DrawingMapping& mapping) {
  for (auto& exposure : scan.exposures) {
    if (exposure.column->isLocked() ||
        (exposure.object && exposure.object->isLocked()))
      throw Error("locked",
                  "An affected drawing exposure is in a locked column");
    exposure.cell.m_frameId = mapping.at(exposure.cell.m_frameId);
    if (!exposure.column->canSetCell(exposure.cell))
      throw Error("incompatible_column",
                  "An exposure cannot accept its renamed drawing");
  }
}

void applyRenumber(Context& context, TXshSimpleLevel* level,
                   const std::vector<TFrameId>& frames, ExposureScan& scan,
                   const AutomationHookRenumber& hooks) {
  std::vector<TImageP> images;
  for (const auto& frame : level->getFids())
    images.push_back(imageFor(level, frame));
  context.markMutationStarted();
  level->renumber(frames);
  verifyAutomationHookRenumber(*level, hooks);
  for (const auto& exposure : scan.exposures)
    if (!exposure.column->setCell(exposure.row, exposure.cell))
      throw Error("native_error",
                  "A preflighted drawing exposure could not be renamed");
  for (size_t index = 0; index < frames.size(); ++index)
    if (imageFor(level, frames[index]).getPointer() !=
        images[index].getPointer())
      throw Error("native_error",
                  "Native renumbering changed a drawing's image binding");
  level->setDirtyFlag(true);
}

QJsonObject renumberDrawing(Context& context, Document& document,
                            const QJsonObject& params) {
  sharing(params);
  auto* level        = simpleLevel(document, params);
  const auto mapping = drawingMapping(level, params);
  const auto frames  = renamedFrames(level, mapping);
  ExposureScan scan{level, {}, false, 1000000, {}, {}};
  QJsonArray resultMapping;
  for (const auto& entry : mapping) {
    resultMapping.append(
        QJsonObject{{"drawing_id", frameJson(entry.first)},
                    {"destination_drawing_id", frameJson(entry.second)}});
    if (entry.first != entry.second) scan.frames.insert(entry.first);
  }
  AutomationHookRenumber hooks;
  if (!scan.frames.empty()) {
    if (level->isReadOnly())
      throw Error("locked", "The drawing level is read-only");
    validateDrawingProfile(level);
    requireAssetGrowth(*document.scene, 0);
    hooks = prepareAutomationHookRenumber(*level, mapping);
    scanExposures(document, scan);
    prepareRenumberExposures(scan, mapping);
    applyRenumber(context, level, frames, scan, hooks);
  }
  return {{"mapping", resultMapping},
          {"drawings_renumbered", static_cast<int>(scan.frames.size())},
          {"exposures_updated", static_cast<int>(scan.exposures.size())},
          {"hook_keys_renumbered", hooks.moved},
          {"sharing", "all_exposures"}};
}

TVectorImageP vectorFor(TXshSimpleLevel* level, const TFrameId& frame) {
  const TVectorImageP vector = imageFor(level, frame);
  if (!vector)
    throw Error("unsupported_type", "This operation requires a vector drawing");
  if (vector->getStrokeCount() > kMaxStrokes)
    throw Error("resource_limit",
                "Vector operation is limited to 4096 strokes per drawing");
  int pointCount = 0;
  for (UINT i = 0; i < vector->getStrokeCount(); ++i) {
    pointCount += vector->getStroke(i)->getControlPointCount();
    if (pointCount > 32768)
      throw Error(
          "resource_limit",
          "Vector operation is limited to 32768 control points per drawing");
  }
  return vector;
}

QJsonObject strokeJson(const TStroke& stroke, int index) {
  QJsonArray points;
  for (int i = 0; i < stroke.getControlPointCount(); ++i) {
    const auto point = stroke.getControlPoint(i);
    points.append(QJsonObject{
        {"x", point.x}, {"y", point.y}, {"thickness", point.thick}});
  }
  return {{"index", index},
          {"native_id", stroke.getId()},
          {"style_id", stroke.getStyle()},
          {"closed", stroke.isSelfLoop()},
          {"points", points}};
}

QJsonObject inspectVector(Context&, Document& document,
                          const QJsonObject& params) {
  auto* level       = simpleLevel(document, params);
  const auto vector = vectorFor(level, frameId(params));
  const int offset  = integerParam(params, "offset", 0, kMaxStrokes, 0);
  const int limit   = integerParam(params, "limit", 1, 128, 32);
  const int total   = static_cast<int>(vector->getStrokeCount());
  const int end     = std::min(offset + limit, total);
  QJsonArray strokes;
  int pointCount = 0;
  for (int index = offset; index < end; ++index) {
    const auto* stroke = vector->getStroke(index);
    pointCount += stroke->getControlPointCount();
    if (pointCount > 32768)
      throw Error("resource_limit",
                  "Reduce the vector inspection limit to return fewer points");
    strokes.append(strokeJson(*stroke, index));
  }
  return {{"strokes", strokes},
          {"total", total},
          {"geometry_revision", document.revisionText()},
          {"coordinate_space", "drawing_native"},
          {"thickness", "native_radius"},
          {"next_offset", end < total ? QJsonValue(end) : QJsonValue()}};
}

std::vector<TThickPoint> controlPoints(const QJsonObject& definition) {
  const auto points = array(definition, "points", 3, kMaxPoints);
  if (points.size() % 2 != 1)
    invalid("Quadratic strokes require 2*n+1 control points");
  std::vector<TThickPoint> result;
  result.reserve(points.size());
  for (const auto value : points) {
    if (!value.isObject()) invalid("Each control point must be an object");
    const auto point = value.toObject();
    keys(point, {"x", "y", "thickness"});
    result.emplace_back(number(point, "x", -kMaxCoordinate, kMaxCoordinate),
                        number(point, "y", -kMaxCoordinate, kMaxCoordinate),
                        number(point, "thickness", 0.0, kMaxThickness));
  }
  if (boolean(definition, "closed") && result.front() != result.back())
    invalid(
        "Closed strokes require identical first and last control points and "
        "thickness");
  return result;
}

struct StrokeEdit final {
  QString action;
  int index = -1;
  std::unique_ptr<TStroke> stroke;
};

int originalStrokeIndex(const QJsonObject& edit, const TVectorImageP& vector) {
  if (edit.value("action") != "add")
    return integer(edit, "index", 0,
                   static_cast<int>(vector->getStrokeCount()) - 1);
  if (edit.contains("index"))
    invalid("An added stroke cannot specify an existing index");
  return -1;
}

StrokeEdit prepareStroke(const QJsonObject& edit, const TVectorImageP& vector,
                         TPalette* palette) {
  keys(edit, {"action", "index", "style_id", "points", "closed"});
  StrokeEdit result;
  result.action = requiredString(edit, "action");
  if (!QStringList{"add", "update", "delete"}.contains(result.action))
    invalid("Stroke action must be add, update or delete");
  result.index = originalStrokeIndex(edit, vector);
  if (result.action == "delete") {
    if (edit.size() != 2) invalid("Delete edits accept only action and index");
    return result;
  }
  const auto points = controlPoints(edit);
  const int style   = styleId(edit, palette);
  result.stroke.reset(result.index < 0
                          ? new TStroke(points)
                          : new TStroke(*vector->getStroke(result.index)));
  if (result.index >= 0) {
    result.stroke->reshape(points.data(), static_cast<int>(points.size()));
    result.stroke->setId(vector->getStroke(result.index)->getId());
  }
  result.stroke->setStyle(style);
  result.stroke->setSelfLoop(boolean(edit, "closed"));
  return result;
}

void uniqueStrokeTarget(const StrokeEdit& edit, std::set<int>& touched) {
  if (edit.index < 0) return;
  if (!touched.insert(edit.index).second)
    invalid("A batch may target each original stroke index only once");
}

int pointCountChange(const StrokeEdit& edit, const TVectorImageP& vector) {
  const int previous =
      edit.index < 0 ? 0
                     : vector->getStroke(edit.index)->getControlPointCount();
  const int next = edit.stroke ? edit.stroke->getControlPointCount() : 0;
  return next - previous;
}

std::vector<StrokeEdit> prepareEdits(const QJsonObject& params,
                                     const TVectorImageP& vector,
                                     TPalette* palette) {
  const auto values = array(params, "edits", 1, 128);
  std::vector<StrokeEdit> edits;
  std::set<int> touched;
  int resultingStrokes = static_cast<int>(vector->getStrokeCount());
  int pointCount       = 0;
  for (UINT i = 0; i < vector->getStrokeCount(); ++i)
    pointCount += vector->getStroke(i)->getControlPointCount();
  for (const auto value : values) {
    if (!value.isObject()) invalid("Each vector edit must be an object");
    auto edit = prepareStroke(value.toObject(), vector, palette);
    uniqueStrokeTarget(edit, touched);
    if (edit.action == "add")
      ++resultingStrokes;
    else if (edit.action == "delete")
      --resultingStrokes;
    pointCount += pointCountChange(edit, vector);
    edits.push_back(std::move(edit));
  }
  if (resultingStrokes > kMaxStrokes || pointCount > 32768)
    throw Error("resource_limit",
                "Geometry batch exceeds the stroke or control-point limit");
  return edits;
}

bool vectorChanged(const std::vector<StrokeEdit>& edits,
                   const TVectorImageP& vector) {
  for (const auto& edit : edits) {
    if (edit.action != "update") return true;
    const auto* previous = vector->getStroke(edit.index);
    if (*previous != *edit.stroke ||
        previous->getStyle() != edit.stroke->getStyle() ||
        previous->isSelfLoop() != edit.stroke->isSelfLoop())
      return true;
  }
  return false;
}

qint64 vectorEditGrowth(const std::vector<StrokeEdit>& edits,
                        const TVectorImageP& vector) {
  qint64 bytes = 0;
  for (const auto& edit : edits) {
    bytes += qint64(pointCountChange(edit, vector)) * 64;
    if (edit.action == "add") bytes += 1024;
    if (edit.action == "delete") bytes -= 1024;
  }
  return bytes;
}

void applyVectorEdits(const TVectorImageP& vector,
                      std::vector<StrokeEdit>& edits) {
  std::vector<int> removed;
  for (auto& edit : edits) {
    if (edit.action == "delete")
      removed.push_back(edit.index);
    else if (edit.action == "update") {
      std::unique_ptr<TStroke> previous(
          new TStroke(*vector->getStroke(edit.index)));
      vector->replaceStroke(edit.index, edit.stroke.release());
      vector->notifyChangedStrokes(edit.index, previous.get());
    }
  }
  std::sort(removed.rbegin(), removed.rend());
  for (int index : removed) vector->deleteStroke(index);
  for (auto& edit : edits)
    if (edit.action == "add") vector->addStroke(edit.stroke.release(), false);
  vector->findRegions();
}

QJsonObject editVector(Context& context, Document& document,
                       const QJsonObject& params) {
  sharing(params);
  if (requiredString(params, "coordinate_space") != "drawing_native")
    invalid(
        "Vector geometry uses drawing_native coordinates and native radius "
        "thickness");
  auto* level         = simpleLevel(document, params);
  const auto frame    = frameId(params);
  const auto original = vectorFor(level, frame);
  auto edits          = prepareEdits(params, original, paletteFor(level));
  if (!vectorChanged(edits, original))
    return {{"stroke_count", static_cast<int>(original->getStrokeCount())},
            {"geometry_indices_invalidated", false},
            {"sharing", "all_exposures"},
            {"changed", false}};
  requireAssetGrowth(*document.scene, vectorEditGrowth(edits, original));
  TVectorImageP vector(original->cloneImage());
  applyVectorEdits(vector, edits);
  publish(context, level, frame, vector);
  return {{"stroke_count", static_cast<int>(vector->getStrokeCount())},
          {"geometry_indices_invalidated", true},
          {"sharing", "all_exposures"}};
}

QJsonObject fillVector(Context& context, Document& document,
                       const QJsonObject& params) {
  sharing(params);
  if (requiredString(params, "coordinate_space") != "drawing_native")
    invalid("Vector fill uses drawing_native coordinates");
  auto* level         = simpleLevel(document, params);
  const auto frame    = frameId(params);
  const auto original = vectorFor(level, frame);
  const TPointD point(number(params, "x", -kMaxCoordinate, kMaxCoordinate),
                      number(params, "y", -kMaxCoordinate, kMaxCoordinate));
  const int style = styleId(params, paletteFor(level));
  TVectorImageP vector(original->cloneImage());
  vector->findRegions();
  const int previous = vector->fill(point, style);
  if (previous == style)
    return {{"previous_style_id", previous},
            {"style_id", style},
            {"sharing", "all_exposures"},
            {"changed", false}};
  if (previous < 0)
    throw Error("not_found", "No closed vector region contains the fill point");
  publish(context, level, frame, vector);
  return {{"previous_style_id", previous},
          {"style_id", style},
          {"sharing", "all_exposures"}};
}

TPixel32 color(const QJsonObject& params, bool premultiplied) {
  const int r = integer(params, "r", 0, 255), g = integer(params, "g", 0, 255);
  const int b = integer(params, "b", 0, 255), a = integer(params, "a", 0, 255);
  if (premultiplied && (r > a || g > a || b > a))
    invalid("Premultiplied pixel RGB channels must not exceed alpha");
  return TPixel32(r, g, b, a);
}

QJsonObject colorJson(const TPixel32& pixel) {
  return {{"r", pixel.r}, {"g", pixel.g}, {"b", pixel.b}, {"a", pixel.m}};
}

TPixel32 styleColor(const QJsonObject& params) {
  if (!params.value("color").isObject())
    invalid("color must be an RGBA8 object");
  const auto value = params.value("color").toObject();
  keys(value, {"r", "g", "b", "a"});
  return color(value, false);
}

QJsonObject styleJson(TPalette* palette, int id) {
  auto* style = palette->getStyle(id);
  QJsonArray keyframes;
  QJsonArray animation;
  const auto keys = palette->getStyleAnimation(id);
  if (keys.size() > 4096)
    throw Error("resource_limit", "Style inspection exceeds 4096 keyframes");
  for (const auto& entry : keys) {
    keyframes.append(entry.first);
    QJsonObject key{
        {"frame", entry.first},
        {"timing", static_cast<int>(entry.second.interpolation.timing)},
        {"color_space", static_cast<int>(entry.second.interpolation.space)}};
    if (entry.second.style->hasMainColor())
      key.insert("color", colorJson(entry.second.style->getMainColor()));
    animation.append(key);
  }
  QJsonObject result{{"style_id", id},
                     {"name", QString::fromStdWString(style->getName())},
                     {"native_tag", style->getTagId()},
                     {"keyframes", keyframes},
                     {"animation", animation},
                     {"editable", typeid(*style) == typeid(TSolidColorStyle)}};
  if (style->hasMainColor())
    result.insert("color", colorJson(style->getMainColor()));
  return result;
}

QJsonObject inspectPalette(Context&, Document& document,
                           const QJsonObject& params) {
  auto* palette    = paletteFor(simpleLevel(document, params));
  const int offset = integerParam(params, "offset", 0, kMaxStyles, 0);
  const int limit  = integerParam(params, "limit", 1, 128, 32);
  const int total  = palette->getStyleCount();
  const int end    = std::min(offset + limit, total);
  QJsonArray styles;
  for (int id = offset; id < end; ++id) styles.append(styleJson(palette, id));
  return {{"styles", styles},
          {"total", total},
          {"next_offset", end < total ? QJsonValue(end) : QJsonValue()},
          {"locked", palette->isLocked()},
          {"color_space", "rgba8_straight_native"},
          {"frame_base", 0},
          {"sharing", "whole_level"}};
}

QString styleName(const QJsonObject& params) {
  const QString name = requiredString(params, "name");
  if (name.size() > 128 || name.startsWith('|') || name.contains(QChar::Null))
    invalid(
        "Style names must have at most 128 characters, contain no NUL, and "
        "not begin with the native studio-palette marker |");
  return name;
}

QJsonObject createStyle(Context& context, Document& document,
                        const QJsonObject& params) {
  auto* level        = simpleLevel(document, params);
  auto* palette      = paletteFor(level, true);
  const auto rgba    = styleColor(params);
  const QString name = styleName(params);
  if (palette->getStyleCount() >= kMaxStyles)
    throw Error("resource_limit", "Palette already contains 4096 styles");
  requireAssetGrowth(*document.scene, AutomationStyleBytes);
  std::unique_ptr<TColorStyle> style(new TSolidColorStyle(rgba));
  style->setName(name.toStdWString());
  context.markMutationStarted();
  const int id = palette->addStyle(style.release());
  auto* page   = palette->getPageCount() ? palette->getPage(0)
                                         : palette->addPage(L"Colors");
  page->addStyle(id);
  palette->setDirtyFlag(true);
  level->setDirtyFlag(true);
  return styleJson(palette, id);
}

TColorStyle* solidEditableStyle(TPalette* palette, int id) {
  if (id == 0) invalid("The transparent style at index zero cannot be changed");
  auto* style = palette->getStyle(id);
  if (typeid(*style) != typeid(TSolidColorStyle))
    throw Error("unsupported_style",
                "Only native solid color styles can be edited");
  return style;
}

bool sameStyleColor(TPalette* palette, int id, const QJsonObject& params,
                    const TPixel32& color, int maximumFrame) {
  const auto keys = palette->getStyleAnimation(id);
  if (!params.contains("keyframe"))
    return keys.empty() && palette->getStyle(id)->getMainColor() == color;
  const int frame = integer(params, "keyframe", 0, maximumFrame);
  const auto key  = keys.find(frame);
  if (key == keys.end()) return false;
  const auto* style = key->second.style.getPointer();
  return typeid(*style) == typeid(TSolidColorStyle) &&
         style->getMainColor() == color;
}

void requirePaletteKeyGrowth(ToonzScene& scene, TPalette* palette, int id,
                             const QJsonObject& params, int maximumFrame) {
  if (!params.contains("keyframe")) return;
  const int frame = integer(params, "keyframe", 0, maximumFrame);
  if (!palette->isKeyframe(id, frame))
    requireAssetGrowth(scene, AutomationStyleBytes);
}

QJsonObject updateStyle(Context& context, Document& document,
                        const QJsonObject& params) {
  auto* level     = simpleLevel(document, params);
  auto* palette   = paletteFor(level, true);
  const int id    = styleId(params, palette);
  auto* style     = solidEditableStyle(palette, id);
  const auto rgba = styleColor(params);
  const int maximumFrame =
      level->getType() == PLI_XSHLEVEL ? kMaxVectorFrame : 999999;
  requirePaletteKeyGrowth(*document.scene, palette, id, params, maximumFrame);
  if (sameStyleColor(palette, id, params, rgba, maximumFrame))
    return styleJson(palette, id);
  auto animation = palette->getStyleAnimation(id);
  std::unique_ptr<TColorStyle> updated(style->clone());
  updated->setMainColor(rgba);
  if (params.contains("keyframe")) {
    const int frame = integer(params, "keyframe", 0, maximumFrame);
    auto found      = animation.find(frame);
    if (found == animation.end()) {
      if (animation.size() >= 4096)
        throw Error("resource_limit", "Style has too many keyframes");
      TPalette::StyleKey key;
      key.style = TColorStyleP(updated->clone());
      animation.emplace(frame, std::move(key));
    } else {
      std::unique_ptr<TColorStyle> key(found->second.style->clone());
      const auto* keyStyle = key.get();
      if (typeid(*keyStyle) != typeid(TSolidColorStyle))
        throw Error("unsupported_style",
                    "The selected keyframe is not a solid color style");
      key->setMainColor(rgba);
      found->second.style = TColorStyleP(key.release());
    }
  } else if (!animation.empty()) {
    invalid(
        "An animated style requires an explicit zero-based keyframe target");
  }
  context.markMutationStarted();
  palette->replaceStyleAnimation(id, animation, *updated);
  style->invalidateIcon();
  palette->setDirtyFlag(true);
  level->setDirtyFlag(true);
  return styleJson(palette, id);
}

bool validPageName(const QString& name, bool vectorFirstPage) {
  return !name.isEmpty() && name.size() <= 128 && !name.contains(QChar::Null) &&
         !(vectorFirstPage && name.startsWith("refimage"));
}

QString pageName(const QJsonObject& params, bool vectorFirstPage) {
  const QString name = requiredString(params, "name");
  if (!validPageName(name, vectorFirstPage))
    invalid(
        "Page names must have 1..128 characters and no NUL; vector page "
        "zero cannot begin with the native refimage marker");
  return name;
}

QJsonObject pagesJson(TPalette* palette) {
  QJsonArray pages;
  for (int index = 0; index < palette->getPageCount(); ++index) {
    const auto* page = palette->getPage(index);
    QJsonArray ids;
    for (int position = 0; position < page->getStyleCount(); ++position)
      ids.append(page->getStyleId(position));
    pages.append(QJsonObject{{"page_index", index},
                             {"name", QString::fromStdWString(page->getName())},
                             {"style_ids", ids}});
  }
  return {{"pages", pages},
          {"page_count", palette->getPageCount()},
          {"style_count", palette->getStyleCount()},
          {"sharing", "whole_level"},
          {"indices", "zero_based_at_document_revision"}};
}

QJsonObject inspectPages(Context&, Document& document,
                         const QJsonObject& params) {
  return pagesJson(paletteFor(simpleLevel(document, params)));
}

void publishPalette(Context& context, TXshSimpleLevel* level,
                    const TPaletteP& palette) {
  std::vector<TImageP> drawings;
  for (const auto& frame : level->getFids())
    drawings.push_back(imageFor(level, frame));
  context.markMutationStarted();
  level->setPalette(palette.getPointer());
  for (const auto& drawing : drawings)
    drawing->setPalette(palette.getPointer());
  palette->setDirtyFlag(true);
  level->setDirtyFlag(true);
}

QJsonObject createPage(Context& context, Document& document,
                       const QJsonObject& params) {
  auto* level        = simpleLevel(document, params);
  auto* original     = paletteFor(level, true);
  const QString name = pageName(params, false);
  if (original->getPageCount() >= kMaxPages)
    throw Error("resource_limit", "Palette already contains 128 pages");
  requireAssetGrowth(*document.scene, AutomationPageBytes);
  const TPaletteP palette(original->clone());
  palette->addPage(name.toStdWString());
  publishPalette(context, level, palette);
  return pagesJson(palette.getPointer());
}

QJsonObject renamePage(Context& context, Document& document,
                       const QJsonObject& params) {
  auto* level    = simpleLevel(document, params);
  auto* original = paletteFor(level, true);
  const int index =
      integer(params, "page_index", 0, original->getPageCount() - 1);
  const QString name =
      pageName(params, index == 0 && level->getType() == PLI_XSHLEVEL);
  if (original->getPage(index)->getName() == name.toStdWString())
    return pagesJson(original);
  requireAssetGrowth(*document.scene, 0);
  const TPaletteP palette(original->clone());
  palette->getPage(index)->setName(name.toStdWString());
  publishPalette(context, level, palette);
  return pagesJson(palette.getPointer());
}

QJsonObject movePage(Context& context, Document& document,
                     const QJsonObject& params) {
  auto* level           = simpleLevel(document, params);
  auto* original        = paletteFor(level, true);
  const int last        = original->getPageCount() - 1;
  const int index       = integer(params, "page_index", 1, last);
  const int destination = integer(params, "to_index", 1, last);
  if (index == destination) return pagesJson(original);
  requireAssetGrowth(*document.scene, 0);
  const TPaletteP palette(original->clone());
  palette->movePage(palette->getPage(index), destination);
  publishPalette(context, level, palette);
  return pagesJson(palette.getPointer());
}

QJsonObject removePage(Context& context, Document& document,
                       const QJsonObject& params) {
  auto* level    = simpleLevel(document, params);
  auto* original = paletteFor(level, true);
  const int index =
      integer(params, "page_index", 1, original->getPageCount() - 1);
  if (original->getPage(index)->getStyleCount() != 0)
    throw Error("page_not_empty", "Move the page's styles before removing it");
  requireAssetGrowth(*document.scene, -AutomationPageBytes);
  const TPaletteP palette(original->clone());
  palette->erasePage(index);
  publishPalette(context, level, palette);
  return pagesJson(palette.getPointer());
}

QJsonObject moveStyle(Context& context, Document& document,
                      const QJsonObject& params) {
  auto* level    = simpleLevel(document, params);
  auto* original = paletteFor(level, true);
  const int id   = styleId(params, original);
  if (id == 0) invalid("The transparent style must remain first on page zero");
  const int destination =
      integer(params, "page_index", 0, original->getPageCount() - 1);
  auto* source = original->getStylePage(id);
  if (!source)
    throw Error("unsupported_palette", "The style has no native page");
  const auto* targetPage = original->getPage(destination);
  const int last = targetPage->getStyleCount() - (source == targetPage ? 1 : 0);
  const int position =
      integer(params, "position", destination == 0 ? 1 : 0, last);
  if (source == targetPage && source->search(id) == position)
    return pagesJson(original);
  requireAssetGrowth(*document.scene, 0);
  const TPaletteP palette(original->clone());
  auto* current = palette->getStylePage(id);
  current->removeStyle(current->search(id));
  palette->getPage(destination)->insertStyle(position, id);
  publishPalette(context, level, palette);
  return pagesJson(palette.getPointer());
}

TPalette::StyleAnimation namedAnimation(TPalette* palette, int id,
                                        const std::wstring& name) {
  auto animation = palette->getStyleAnimation(id);
  for (auto& entry : animation) {
    TColorStyleP style(entry.second.style->clone());
    style->setName(name);
    entry.second.style = style;
  }
  return animation;
}

QJsonObject renameStyle(Context& context, Document& document,
                        const QJsonObject& params) {
  auto* level        = simpleLevel(document, params);
  auto* original     = paletteFor(level, true);
  const int id       = styleId(params, original);
  const auto* source = solidEditableStyle(original, id);
  const auto name    = styleName(params).toStdWString();
  if (source->getName() == name) return styleJson(original, id);
  requireAssetGrowth(*document.scene, 0);
  const TPaletteP palette(original->clone());
  const auto animation = namedAnimation(palette.getPointer(), id, name);
  TColorStyleP renamed(source->clone());
  renamed->setName(name);
  palette->replaceStyleAnimation(id, animation, *renamed);
  palette->getStyle(id)->setName(name);
  publishPalette(context, level, palette);
  return styleJson(palette.getPointer(), id);
}

QJsonObject cloneStyle(Context& context, Document& document,
                       const QJsonObject& params) {
  auto* level        = simpleLevel(document, params);
  auto* original     = paletteFor(level, true);
  const int sourceId = styleId(params, original);
  const auto* source = solidEditableStyle(original, sourceId);
  const auto name    = styleName(params).toStdWString();
  const int page =
      integer(params, "page_index", 0, original->getPageCount() - 1);
  if (original->getStyleCount() >= kMaxStyles)
    throw Error("resource_limit", "Palette already contains 4096 styles");
  const qint64 snapshots = qint64(original->getKeyframeCount(sourceId)) + 1;
  requireAssetGrowth(*document.scene, snapshots * AutomationStyleBytes);
  const TPaletteP palette(original->clone());
  const auto animation = namedAnimation(original, sourceId, name);
  std::unique_ptr<TColorStyle> copied(source->clone());
  copied->setName(name);
  const int id = palette->addStyle(copied.release());
  if (id < 0)
    throw Error("resource_limit", "Native palette refused the copied style");
  palette->getPage(page)->addStyle(id);
  TColorStyleP unkeyed(palette->getStyle(id)->clone());
  palette->replaceStyleAnimation(id, animation, *unkeyed);
  palette->getStyle(id)->setName(name);
  publishPalette(context, level, palette);
  return styleJson(palette.getPointer(), id);
}

struct PixelRect final {
  int x, y, width, height;
};

class RasterLock final {
  TRasterP m_raster;

public:
  explicit RasterLock(TRasterP raster) : m_raster(raster) { m_raster->lock(); }
  ~RasterLock() { m_raster->unlock(); }
  RasterLock(const RasterLock&)            = delete;
  RasterLock& operator=(const RasterLock&) = delete;
};

PixelRect pixelRect(const QJsonObject& params, const TImageP& image) {
  if (requiredString(params, "coordinate_space") != "raster_pixels_lower_left")
    invalid("Pixels use raster_pixels_lower_left coordinates");
  imageFormat(image);
  if (!image->raster()) invalid("Pixel operations require a raster drawing");
  const int width = image->raster()->getLx(), height = image->raster()->getLy();
  PixelRect result{
      integer(params, "x", 0, width - 1), integer(params, "y", 0, height - 1),
      integer(params, "width", 1, width), integer(params, "height", 1, height)};
  if (result.width > width - result.x || result.height > height - result.y ||
      static_cast<qint64>(result.width) * result.height > kMaxPatch)
    invalid(
        "Pixel rectangle must fit the drawing and contain at most 16384 "
        "pixels");
  return result;
}

QJsonObject readPixels(Context&, Document& document,
                       const QJsonObject& params) {
  auto* level                = simpleLevel(document, params);
  const auto image           = imageFor(level, frameId(params));
  const auto rect            = pixelRect(params, image);
  const TRaster32P rgba      = image->raster();
  const TRasterCM32P indexed = image->raster();
  RasterLock lock(image->raster());
  QJsonArray pixels;
  for (int y = rect.y; y < rect.y + rect.height; ++y) {
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
      if (rgba)
        pixels.append(colorJson(rgba->pixels(y)[x]));
      else {
        const auto pixel = indexed->pixels(y)[x];
        pixels.append(QJsonObject{{"ink", pixel.getInk()},
                                  {"paint", pixel.getPaint()},
                                  {"tone", pixel.getTone()}});
      }
    }
  }
  return {{"pixels", pixels},
          {"row_order", "bottom_to_top"},
          {"channels", rgba ? "rgba8_premultiplied" : "ink_paint_tone"}};
}

void validatePixels(const QJsonArray& pixels, bool rgba, TPalette* palette) {
  for (const auto value : pixels) {
    if (!value.isObject()) invalid("Each pixel must be a typed pixel object");
    const auto pixel = value.toObject();
    if (rgba) {
      keys(pixel, {"r", "g", "b", "a"});
      color(pixel, true);
    } else {
      keys(pixel, {"ink", "paint", "tone"});
      styleId(pixel, palette, "ink");
      styleId(pixel, palette, "paint");
      integer(pixel, "tone", 0, 255);
    }
  }
}

bool pixelsMatch(const TImageP& image, const PixelRect& rect,
                 const QJsonArray& pixels) {
  const TRaster32P rgba      = image->raster();
  const TRasterCM32P indexed = image->raster();
  RasterLock lock(image->raster());
  int offset = 0;
  for (int y = rect.y; y < rect.y + rect.height; ++y) {
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
      const auto current =
          rgba ? colorJson(rgba->pixels(y)[x])
               : QJsonObject{{"ink", indexed->pixels(y)[x].getInk()},
                             {"paint", indexed->pixels(y)[x].getPaint()},
                             {"tone", indexed->pixels(y)[x].getTone()}};
      if (current != pixels.at(offset++).toObject()) return false;
    }
  }
  return true;
}

QJsonObject patchPixels(Context& context, Document& document,
                        const QJsonObject& params) {
  sharing(params);
  auto* level          = simpleLevel(document, params);
  const auto frame     = frameId(params);
  const auto original  = imageFor(level, frame);
  const auto rect      = pixelRect(params, original);
  const auto pixels    = array(params, "pixels", rect.width * rect.height,
                               rect.width * rect.height);
  const bool fullcolor = bool(TRaster32P(original->raster()));
  validatePixels(pixels, fullcolor, fullcolor ? nullptr : paletteFor(level));
  if (pixelsMatch(original, rect, pixels))
    return {{"pixels_changed", 0},
            {"sharing", "all_exposures"},
            {"changed", false}};
  const TImageP image(original->cloneImage());
  const TRaster32P rgba      = image->raster();
  const TRasterCM32P indexed = image->raster();
  RasterLock lock(image->raster());
  int offset = 0;
  for (int y = rect.y; y < rect.y + rect.height; ++y) {
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
      const auto pixel = pixels.at(offset++).toObject();
      if (rgba)
        rgba->pixels(y)[x] = color(pixel, true);
      else
        indexed->pixels(y)[x] =
            TPixelCM32(pixel.value("ink").toInt(), pixel.value("paint").toInt(),
                       pixel.value("tone").toInt());
    }
  }
  const TRasterImageP rasterImage = image;
  if (rasterImage)
    rasterImage->setSavebox(image->raster()->getBounds());
  else
    TToonzImageP(image)->setSavebox(image->raster()->getBounds());
  publish(context, level, frame, image);
  return {{"pixels_changed", pixels.size()}, {"sharing", "all_exposures"}};
}

QString pngPath(Context& context, const QJsonObject& params, bool write) {
  const QString path =
      context.resolvePath(requiredString(params, "path"), write);
  if (QFileInfo(path).suffix().toLower() != "png")
    invalid("Only PNG artifacts are supported");
  if (write && QFileInfo::exists(path))
    throw Error("already_exists", "PNG destination exists");
  return path;
}

QByteArray readPngHeader(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    throw Error("asset_unavailable",
                "Cannot open PNG input: " + file.errorString());
  if (file.size() > 256 * 1024 * 1024)
    throw Error("resource_limit", "PNG input exceeds 256 MiB");
  const auto header = file.read(33);
  if (header.size() != 33 ||
      header.left(8) != QByteArray::fromHex("89504e470d0a1a0a") ||
      header.mid(12, 4) != "IHDR")
    invalid("Input does not have a valid PNG signature and IHDR header");
  return header;
}

void validatePngHeader(const QString& path) {
  const auto header = readPngHeader(path);
  const auto* bytes = reinterpret_cast<const uchar*>(header.constData());
  const auto width  = qFromBigEndian<quint32>(bytes + 16);
  const auto height = qFromBigEndian<quint32>(bytes + 20);
  if (width > 8192 || height > 8192)
    invalid("PNG dimensions exceed 8192 pixels per axis");
  rasterDimensions(static_cast<int>(width), static_cast<int>(height));
  if (bytes[24] > 8)
    throw Error("unsupported_precision",
                "PNG import rejects source channels deeper than 8 bits");
}

QJsonObject importPng(Context& context, Document& document,
                      const QJsonObject& params) {
  auto* level = simpleLevel(document, params);
  if (level->getType() != OVL_XSHLEVEL)
    invalid("PNG import requires an RGBA8 level");
  const auto frame = frameId(params);
  if (level->isFid(frame))
    throw Error("already_exists", "Drawing identity already exists");
  if (level->getFrameCount() >= 4096)
    throw Error("resource_limit", "Level already has 4096 drawings");
  const QString path = pngPath(context, params, false);
  validatePngHeader(path);
  TImageReaderP reader(TFilePath(path.toStdWString()));
  const auto* info = reader->getImageInfo();
  if (!info || info->m_bitsPerSample > 8)
    throw Error("unsupported_precision",
                "PNG import accepts only source precision of 8 bits or less");
  rasterDimensions(info->m_lx, info->m_ly);
  const double dpi = number(params, "dpi", 1.0, 2400.0);
  compatibleRaster(level, TDimension(info->m_lx, info->m_ly), dpi);
  requireAssetGrowth(*document.scene,
                     automationRasterBytes(info->m_lx, info->m_ly), 1);
  const TRasterImageP image = reader->load();
  if (!image || !TRaster32P(image->getRaster()))
    throw Error("unsupported_type",
                "PNG decoder did not produce an RGBA8 drawing");
  image->setDpi(dpi, dpi);
  image->setSavebox(image->getRaster()->getBounds());
  initializeRaster(context, level, image->getRaster()->getSize(), dpi);
  publish(context, level, frame, TImageP(image.getPointer()));
  return imageSummary(level, frame);
}

QJsonObject exportPng(Context& context, Document& document,
                      const QJsonObject& params) {
  auto* level      = simpleLevel(document, params);
  const auto image = imageFor(level, frameId(params));
  if (imageFormat(image) != "rgba8")
    invalid("PNG export requires an RGBA8 drawing");
  const QString path = pngPath(context, params, true);
  QTemporaryFile temporary(QFileInfo(path).absolutePath() +
                           "/.automation-XXXXXX.png");
  if (!temporary.open())
    throw Error("io_error",
                "Cannot stage PNG output: " + temporary.errorString());
  const QString temporaryPath = temporary.fileName();
  temporary.close();
  {
    TImageWriterP writer(TFilePath(temporaryPath.toStdWString()));
    writer->save(image);
  }
  context.markMutationStarted();
  if (!QFile::rename(temporaryPath, path))
    throw Error("io_error",
                "Cannot publish PNG output; destination may already exist");
  return {{"path", path},
          {"format", "png"},
          {"source_precision", "rgba8"},
          {"alpha_conversion", "native_png_codec"}};
}

QJsonObject typed(const QString& type) { return {{"type", type}}; }

QJsonObject range(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject choice(const QStringList& values) {
  QJsonArray items;
  for (const auto& value : values) items.append(value);
  return {{"type", "string"}, {"enum", items}};
}

QJsonObject drawingIdentity(int maximum = 999999) {
  return {{"type", "object"},
          {"description",
           "Drawing number: 1..65535 for vector PLI, 1..999999 for raster; "
           "suffixes are zero to eight lowercase ASCII letters"},
          {"additionalProperties", false},
          {"properties",
           QJsonObject{{"number", range(1, maximum)},
                       {"suffix", QJsonObject{{"type", "string"},
                                              {"pattern", "^[a-z]{0,8}$"}}}}},
          {"required", QJsonArray{"number", "suffix"}}};
}

QJsonObject target(bool drawing = true) {
  QJsonObject result{{"document_id", typed("string")},
                     {"level_id", typed("string")}};
  if (drawing) result.insert("drawing_id", drawingIdentity());
  return result;
}

QJsonObject rgbaSchema() {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"properties", QJsonObject{{"r", range(0, 255)},
                                     {"g", range(0, 255)},
                                     {"b", range(0, 255)},
                                     {"a", range(0, 255)}}},
          {"required", QJsonArray{"r", "g", "b", "a"}}};
}

QJsonObject boundedArray(const QJsonObject& items, int minimum, int maximum) {
  return {{"type", "array"},
          {"items", items},
          {"minItems", minimum},
          {"maxItems", maximum}};
}

QJsonObject editSchema() {
  QJsonObject coordinate{{"type", "number"},
                         {"minimum", -kMaxCoordinate},
                         {"maximum", kMaxCoordinate}};
  QJsonObject thickness{
      {"type", "number"}, {"minimum", 0}, {"maximum", kMaxThickness}};
  QJsonObject point{{"type", "object"},
                    {"additionalProperties", false},
                    {"properties", QJsonObject{{"x", coordinate},
                                               {"y", coordinate},
                                               {"thickness", thickness}}},
                    {"required", QJsonArray{"x", "y", "thickness"}}};
  const QJsonObject properties{{"index", range(0, kMaxStrokes - 1)},
                               {"style_id", range(0, kMaxStyles - 1)},
                               {"closed", typed("boolean")},
                               {"points", boundedArray(point, 3, kMaxPoints)}};
  QJsonArray alternatives;
  for (const auto& action : QStringList{"add", "update", "delete"}) {
    auto fields = properties;
    fields.insert("action", choice({action}));
    QJsonArray required{"action"};
    if (action == "add")
      fields.remove("index");
    else
      required.append("index");
    if (action == "delete") {
      fields.remove("points");
      fields.remove("style_id");
      fields.remove("closed");
    } else {
      required.append("points");
      required.append("style_id");
    }
    alternatives.append(QJsonObject{{"type", "object"},
                                    {"additionalProperties", false},
                                    {"properties", fields},
                                    {"required", required}});
  }
  return {{"type", "object"}, {"oneOf", alternatives}};
}

void drawingCatalog(QJsonArray& tools) {
  auto properties = target();
  properties.insert("format", choice({"vector", "rgba8", "toonzRaster"}));
  properties.insert("width", range(1, 8192));
  properties.insert("height", range(1, 8192));
  properties.insert(
      "dpi",
      QJsonObject{{"type", "number"}, {"minimum", 1}, {"maximum", 2400}});
  auto createOperation = operation(
      "drawing.create", "Create an empty native drawing in an existing level",
      properties, {"document_id", "level_id", "drawing_id", "format"}, true);
  auto schema       = createOperation.value("inputSchema").toObject();
  auto vectorFields = schema.value("properties").toObject();
  auto rasterFields = vectorFields;
  vectorFields.insert("format", choice({"vector"}));
  vectorFields.insert("drawing_id", drawingIdentity(kMaxVectorFrame));
  vectorFields.remove("width");
  vectorFields.remove("height");
  vectorFields.remove("dpi");
  rasterFields.insert("format", choice({"rgba8", "toonzRaster"}));
  auto rasterRequired = schema.value("required").toArray();
  for (const auto& key : QStringList{"width", "height", "dpi"})
    rasterRequired.append(key);
  schema.insert("oneOf",
                QJsonArray{QJsonObject{{"type", "object"},
                                       {"additionalProperties", false},
                                       {"properties", vectorFields},
                                       {"required", schema.value("required")}},
                           QJsonObject{{"type", "object"},
                                       {"additionalProperties", false},
                                       {"properties", rasterFields},
                                       {"required", rasterRequired}}});
  createOperation.insert("inputSchema", schema);
  tools.append(createOperation);
  tools.append(operation("drawing.inspect",
                         "Load and describe one native drawing", target(),
                         {"document_id", "level_id", "drawing_id"}));
  properties = target();
  properties.insert("destination_drawing_id", drawingIdentity());
  tools.append(operation(
      "drawing.clone", "Clone a drawing within its level without exposing it",
      properties,
      {"document_id", "level_id", "drawing_id", "destination_drawing_id"},
      true));
  tools.append(operation(
      "drawing.remove", "Remove an unexposed drawing; reject any remaining use",
      target(), {"document_id", "level_id", "drawing_id"}, true));
}

void renumberCatalog(QJsonArray& tools) {
  const QJsonObject entry{
      {"type", "object"},
      {"additionalProperties", false},
      {"properties",
       QJsonObject{{"drawing_id", drawingIdentity()},
                   {"destination_drawing_id", drawingIdentity()}}},
      {"required", QJsonArray{"drawing_id", "destination_drawing_id"}}};
  auto properties = target(false);
  properties.insert("mapping", boundedArray(entry, 1, 4096));
  properties.insert("sharing", choice({"all_exposures"}));
  tools.append(operation(
      "drawing.renumber",
      "Rename drawings simultaneously, including swaps and cycles, and update "
      "all held exposures; locked uses and final identity collisions reject",
      properties, {"document_id", "level_id", "mapping", "sharing"}, true));
}

void vectorTransformCatalog(QJsonArray& tools) {
  auto properties = target();
  properties.insert("coordinate_space", choice({"drawing_native"}));
  properties.insert("sharing", choice({"all_exposures"}));
  properties.insert("thickness_mode",
                    choice({"preserve", "scale_geometric_mean"}));
  const QJsonObject coefficient{{"type", "number"},
                                {"minimum", -kMaxCoordinate},
                                {"maximum", kMaxCoordinate}};
  QJsonObject fields;
  const QJsonArray names{"a11", "a12", "a13", "a21", "a22", "a23"};
  for (const auto& name : names) fields.insert(name.toString(), coefficient);
  properties.insert("matrix", QJsonObject{{"type", "object"},
                                          {"additionalProperties", false},
                                          {"properties", fields},
                                          {"required", names}});
  tools.append(operation(
      "vector.transform",
      "Transform the whole drawing: x'=a11*x+a12*y+a13, "
      "y'=a21*x+a22*y+a23. Require abs(det)>=0.001; preserve native radii "
      "or scale them by sqrt(abs(det)). Retain stroke IDs and painted "
      "topology; "
      "admit at most 65536 region edges and native coordinate/radius bounds.",
      properties,
      {"document_id", "level_id", "drawing_id", "coordinate_space", "sharing",
       "matrix", "thickness_mode"},
      true));
}

void vectorCatalog(QJsonArray& tools) {
  auto properties = target();
  properties.insert("offset", range(0, kMaxStrokes));
  properties.insert("limit", range(1, 128));
  tools.append(operation(
      "vector.inspect", "Read editable quadratic control points and styles",
      properties, {"document_id", "level_id", "drawing_id"}));
  properties = target();
  properties.insert("coordinate_space", choice({"drawing_native"}));
  properties.insert("sharing", choice({"all_exposures"}));
  properties.insert("edits", boundedArray(editSchema(), 1, 128));
  tools.append(operation(
      "vector.edit",
      "Atomically add, reshape or delete original stroke indices; "
      "native thickness is radius, and geometry indices expire after mutation",
      properties,
      {"document_id", "level_id", "drawing_id", "coordinate_space", "sharing",
       "edits"},
      true));
  properties.remove("edits");
  properties.insert("x", QJsonObject{{"type", "number"},
                                     {"minimum", -kMaxCoordinate},
                                     {"maximum", kMaxCoordinate}});
  properties.insert("y", properties.value("x"));
  properties.insert("style_id", range(0, kMaxStyles - 1));
  tools.append(
      operation("vector.fill",
                "Assign a style to the native closed region containing a point",
                properties,
                {"document_id", "level_id", "drawing_id", "coordinate_space",
                 "sharing", "x", "y", "style_id"},
                true));
  vectorTransformCatalog(tools);
}

QJsonObject groupOperation(const QString& name, const QString& description,
                           QJsonObject properties, QJsonArray required,
                           bool writes) {
  properties.insert("drawing_id", drawingIdentity(kMaxVectorFrame));
  auto result = operation(name, description, properties, required, writes);
  auto schema = result.value("inputSchema").toObject();
  properties  = schema.value("properties").toObject();
  for (const auto& key : QStringList{"document_id", "level_id", "operation_id",
                                     "expected_revision"}) {
    if (!properties.contains(key)) continue;
    properties.insert(
        key, QJsonObject{{"type", "string"},
                         {"minLength", 1},
                         {"maxLength", key == "expected_revision" ? 20 : 128}});
  }
  schema.insert("properties", properties);
  result.insert("inputSchema", schema);
  return result;
}

void vectorGroupCatalog(QJsonArray& tools) {
  auto properties = target();
  QJsonArray required{"document_id", "level_id", "drawing_id"};
  tools.append(groupOperation(
      "vector.structure",
      "Inspect revision-bound native stroke order and group spans, including "
      "nested "
      "groups; at most 4096 strokes/groups, depth 16 and 1 MiB of metadata",
      properties, required, false));
  properties.insert("sharing", choice({"all_exposures"}));
  properties.insert("first_index", range(0, kMaxStrokes - 1));
  required.append("sharing");
  required.append("first_index");
  tools.append(groupOperation(
      "vector.group.remove",
      "Ungroup one complete flat group at its first stroke; reject nested or "
      "entered "
      "content and any native result that changes painted boundaries",
      properties, required, true));
  properties.insert("count", range(1, kMaxStrokes));
  required.append("count");
  tools.append(groupOperation("vector.group.create",
                              "Group an adjacent ungrouped range without "
                              "reordering; an exactly matching flat "
                              "group is a no-op. Reject nested/entered content "
                              "and changed painted boundaries",
                              properties, required, true));
  properties.insert("before_index", range(0, kMaxStrokes));
  required.append("before_index");
  tools.append(groupOperation("vector.order.move",
                              "Move one complete flat group or adjacent "
                              "ungrouped strokes before a pre-edit "
                              "index; stroke_count means front/end. "
                              "Destinations within the range are no-ops. "
                              "Reject split groups, nested/entered content and "
                              "changed painted boundaries",
                              properties, required, true));
}

void paletteCatalog(QJsonArray& tools) {
  auto properties = target(false);
  properties.insert("offset", range(0, kMaxStyles));
  properties.insert("limit", range(1, 128));
  tools.append(
      operation("palette.inspect",
                "Read level palette styles and zero-based animation keys",
                properties, {"document_id", "level_id"}));
  properties = target(false);
  properties.insert("color", rgbaSchema());
  properties.insert(
      "name", QJsonObject{{"type", "string"},
                          {"minLength", 1},
                          {"maxLength", 128},
                          {"pattern", "^[^|\\x00][^\\x00]*$"},
                          {"description",
                           "Unicode style name without NUL or a leading |"}});
  tools.append(operation("palette.create_style",
                         "Add a solid RGBA8 straight-alpha style to page zero",
                         properties,
                         {"document_id", "level_id", "color", "name"}, true));
  properties.remove("name");
  properties.insert("style_id", range(1, kMaxStyles - 1));
  auto keyframe = range(0, 999999);
  keyframe.insert("description",
                  "Zero-based palette frame: 0..65535 for vector PLI, "
                  "0..999999 for Toonz raster");
  properties.insert("keyframe", keyframe);
  tools.append(operation(
      "palette.update_style",
      "Edit a solid style for the whole level; animated "
      "styles require a zero-based keyframe and preserve other "
      "keys/interpolation",
      properties, {"document_id", "level_id", "style_id", "color"}, true));
}

void palettePageCatalog(QJsonArray& tools) {
  auto properties = target(false);
  const QJsonArray targetRequired{"document_id", "level_id"};
  tools.append(
      operation("palette.pages",
                "Inspect page order and membership; style IDs remain stable",
                properties, targetRequired));
  properties.insert("name", QJsonObject{{"type", "string"},
                                        {"minLength", 1},
                                        {"maxLength", 128},
                                        {"pattern", "^[^\\x00]+$"}});
  tools.append(operation("palette.page.create",
                         "Append an empty native palette page", properties,
                         {"document_id", "level_id", "name"}, true));
  properties.insert("page_index", range(0, kMaxPages - 1));
  tools.append(operation(
      "palette.page.rename",
      "Rename a page; vector page zero cannot begin with refimage", properties,
      {"document_id", "level_id", "page_index", "name"}, true));
  properties.remove("name");
  properties.insert("page_index", range(1, kMaxPages - 1));
  tools.append(operation(
      "palette.page.remove", "Remove an empty page other than pinned page zero",
      properties, {"document_id", "level_id", "page_index"}, true));
  properties.insert("to_index", range(1, kMaxPages - 1));
  tools.append(operation(
      "palette.page.move", "Move a page; page zero stays first", properties,
      {"document_id", "level_id", "page_index", "to_index"}, true));
  properties.remove("to_index");
  properties.insert("page_index", range(0, kMaxPages - 1));
  properties.insert("style_id", range(1, kMaxStyles - 1));
  properties.insert("position", range(0, kMaxStyles - 1));
  tools.append(operation(
      "palette.style.move",
      "Move a style without changing its ID or keys; position is its final "
      "zero-based position and transparent style zero stays first on page zero",
      properties,
      {"document_id", "level_id", "style_id", "page_index", "position"}, true));
}

void paletteStyleCatalog(QJsonArray& tools) {
  auto properties = target(false);
  properties.insert("style_id", range(1, kMaxStyles - 1));
  properties.insert("name", QJsonObject{{"type", "string"},
                                        {"minLength", 1},
                                        {"maxLength", 128},
                                        {"pattern", "^[^|\\x00][^\\x00]*$"}});
  tools.append(operation(
      "palette.rename_style",
      "Rename a solid style and every animation key snapshot", properties,
      {"document_id", "level_id", "style_id", "name"}, true));
  properties.insert("page_index", range(0, kMaxPages - 1));
  tools.append(operation(
      "palette.clone_style",
      "Append an independent solid style with all animation keys to a page; "
      "existing style IDs and artwork references stay unchanged",
      properties, {"document_id", "level_id", "style_id", "name", "page_index"},
      true));
}

void rasterCatalog(QJsonArray& tools) {
  auto properties = target();
  properties.insert("coordinate_space", choice({"raster_pixels_lower_left"}));
  properties.insert("x", range(0, 8191));
  properties.insert("y", range(0, 8191));
  properties.insert("width", range(1, 8192));
  properties.insert("height", range(1, 8192));
  QJsonArray required{
      "document_id", "level_id", "drawing_id", "coordinate_space",
      "x",           "y",        "width",      "height"};
  tools.append(
      operation("raster.read_pixels",
                "Read up to 16384 native pixels in bottom-to-top row order",
                properties, required));
  QJsonObject indexed{{"type", "object"},
                      {"additionalProperties", false},
                      {"properties", QJsonObject{{"ink", range(0, 4095)},
                                                 {"paint", range(0, 4095)},
                                                 {"tone", range(0, 255)}}},
                      {"required", QJsonArray{"ink", "paint", "tone"}}};
  properties.insert(
      "pixels",
      boundedArray(QJsonObject{{"oneOf", QJsonArray{rgbaSchema(), indexed}}}, 1,
                   kMaxPatch));
  properties.insert("sharing", choice({"all_exposures"}));
  required.append("pixels");
  required.append("sharing");
  tools.append(
      operation("raster.patch",
                "Replace bounded native pixels atomically: premultiplied "
                "RGBA8 or palette ink/paint/tone; rows run bottom-to-top",
                properties, required, true));
  properties = target();
  properties.insert("path", typed("string"));
  properties.insert(
      "dpi",
      QJsonObject{{"type", "number"}, {"minimum", 1}, {"maximum", 2400}});
  tools.append(operation(
      "raster.import_png", "Import an at-most-8-bit PNG as a new RGBA8 drawing",
      properties, {"document_id", "level_id", "drawing_id", "path", "dpi"},
      true));
  properties.remove("dpi");
  auto exportOperation = operation(
      "raster.export_png",
      "Write an RGBA8 drawing to a new workspace PNG artifact", properties,
      {"document_id", "level_id", "drawing_id", "path"}, true);
  exportOperation.insert("mutates_document", false);
  tools.append(exportOperation);
}

QJsonObject inspectDrawing(Context&, Document& document,
                           const QJsonObject& params) {
  return imageSummary(simpleLevel(document, params), frameId(params));
}

void validateStyleProfile(const TColorStyle* style) {
  if (!style || typeid(*style) != typeid(TSolidColorStyle) ||
      !style->getGlobalName().empty() || !style->getOriginalName().empty())
    throw Error("unsupported_style",
                "Persistence admits only unlinked native solid styles");
}

void validateAnimatedStyleProfile(TPalette* palette, int id, int maximumFrame) {
  validateStyleProfile(palette->getStyle(id));
  if (palette->getKeyframeCount(id) > 4096)
    throw Error("resource_limit", "A style has more than 4096 keyframes");
  const auto keys = palette->getStyleAnimation(id);
  for (const auto& key : keys) {
    if (key.first < 0 || key.first > maximumFrame)
      throw Error("unsupported_palette",
                  "A palette keyframe exceeds its native level format range");
    validateStyleProfile(key.second.style.getPointer());
  }
}

void validatePageStyles(TPalette* palette, const TPalette::Page* page,
                        std::set<int>& seen) {
  for (int index = 0; index < page->getStyleCount(); ++index) {
    const int id = page->getStyleId(index);
    if (id < 0 || id >= palette->getStyleCount() || !seen.insert(id).second ||
        palette->getStylePage(id) != page)
      throw Error("unsupported_palette",
                  "Palette page membership is inconsistent");
  }
}

void validatePalettePages(TPalette* palette, bool vectorPalette) {
  if (palette->getPageCount() < 1 || palette->getPageCount() > kMaxPages)
    throw Error("unsupported_palette",
                "Persistence admits 1..128 palette pages");
  if (palette->getPage(0)->getStyleId(0) != 0)
    throw Error("unsupported_palette",
                "Transparent style zero must be first on page zero");
  std::set<int> seen;
  for (int index = 0; index < palette->getPageCount(); ++index) {
    const auto* page = palette->getPage(index);
    if (!validPageName(QString::fromStdWString(page->getName()),
                       vectorPalette && index == 0))
      throw Error("unsupported_palette",
                  "A palette page name cannot be preserved natively");
    validatePageStyles(palette, page, seen);
  }
  if (int(seen.size()) != palette->getStyleCount())
    throw Error("unsupported_palette",
                "Unpaged styles are outside the persistence profile");
}

void validatePaletteProfile(TPalette* palette, int maximumFrame) {
  if (!palette) return;
  if (palette->getStyleCount() > kMaxStyles || palette->getRefImg() ||
      !palette->getRefImgPath().isEmpty() || !palette->getGlobalName().empty())
    throw Error("unsupported_palette",
                "Palette exceeds limits or refers to an external asset");
  validatePalettePages(palette, maximumFrame == kMaxVectorFrame);
  for (int id = 0; id < palette->getStyleCount(); ++id)
    validateAnimatedStyleProfile(palette, id, maximumFrame);
}

void validateNativePoint(const TThickPoint& point) {
  if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
      std::abs(point.x) > kMaxCoordinate || std::abs(point.y) > kMaxCoordinate)
    throw Error("unsupported_geometry",
                "Vector coordinates exceed the admitted native PLI range");
  if (!std::isfinite(point.thick) || point.thick < 0.0 ||
      point.thick > kMaxThickness)
    throw Error("unsupported_geometry",
                "Vector radius must be finite and between zero and 255");
}

void validateNativeStroke(const TStroke& stroke, TPalette* palette) {
  const int count = stroke.getControlPointCount();
  if (count < 3 || count % 2 != 1)
    throw Error("unsupported_geometry",
                "Native quadratic strokes require 2*n+1 control points");
  if (stroke.getStyle() < 0 || stroke.getStyle() >= palette->getStyleCount())
    throw Error("unsupported_style",
                "A vector stroke refers to an absent palette style");
  for (int index = 0; index < count; ++index)
    validateNativePoint(stroke.getControlPoint(index));
}

TAffine vectorAffine(const QJsonObject& params) {
  if (!params.value("matrix").isObject()) invalid("matrix must be an object");
  const QJsonObject matrix = params.value("matrix").toObject();
  keys(matrix, {"a11", "a12", "a13", "a21", "a22", "a23"});
  const TAffine affine(number(matrix, "a11", -kMaxCoordinate, kMaxCoordinate),
                       number(matrix, "a12", -kMaxCoordinate, kMaxCoordinate),
                       number(matrix, "a13", -kMaxCoordinate, kMaxCoordinate),
                       number(matrix, "a21", -kMaxCoordinate, kMaxCoordinate),
                       number(matrix, "a22", -kMaxCoordinate, kMaxCoordinate),
                       number(matrix, "a23", -kMaxCoordinate, kMaxCoordinate));
  if (std::abs(affine.det()) < 0.001)
    throw Error("unsupported_transform",
                "Vector transforms require abs(determinant) >= 0.001");
  return affine;
}

TThickPoint transformedPoint(const TThickPoint& point, const TAffine& affine,
                             double thicknessScale) {
  validateNativePoint(point);
  const TThickPoint result(affine * TPointD(point.x, point.y),
                           point.thick * thicknessScale);
  validateNativePoint(result);
  return result;
}

void preflightStrokeTransform(const TStroke& stroke, const TAffine& affine,
                              double thicknessScale) {
  for (int index = 0; index < stroke.getControlPointCount(); ++index)
    transformedPoint(stroke.getControlPoint(index), affine, thicknessScale);
}

QString transformedEdge(const TEdge& edge, const TAffine& affine,
                        bool nativeIds) {
  const bool reflected = affine.det() < 0.0;
  const double first   = reflected ? edge.m_w1 : edge.m_w0;
  const double last    = reflected ? edge.m_w0 : edge.m_w1;
  QJsonArray signature{edge.getStyle()};
  if (edge.m_index >= 0) {
    signature.append(nativeIds ? edge.m_s->getId() : edge.m_index);
    signature.append(first);
    signature.append(last);
  } else {
    preflightStrokeTransform(*edge.m_s, affine, 1.0);
    signature.append("autoclose");
    signature.append(edge.m_s->getChunkCount());
    for (double parameter : {first, (first + last) * 0.5, last}) {
      const TPointD point = affine * edge.m_s->getPoint(parameter);
      signature.append(QJsonArray{point.x, point.y});
    }
  }
  return QString::fromUtf8(
      QJsonDocument(signature).toJson(QJsonDocument::Compact));
}

QJsonArray regionEdges(const TRegion& region, const TAffine& affine,
                       bool nativeIds) {
  QStringList edges;
  for (UINT index = 0; index < region.getEdgeCount(); ++index) {
    const TEdge* edge = region.getEdge(index);
    if (!edge || !edge->m_s)
      throw Error("unsupported_geometry", "A vector region has no native edge");
    edges.append(transformedEdge(*edge, affine, nativeIds));
  }
  if (edges.isEmpty())
    throw Error("unsupported_geometry",
                "A vector region has no native boundary");
  std::sort(edges.begin(), edges.end());
  return QJsonArray::fromStringList(edges);
}

QJsonArray vectorTopology(const TVectorImageP& vector, const TAffine& affine,
                          bool nativeIds = false) {
  if (vector->getRegionCount() > kMaxTransformEdges)
    throw Error("resource_limit", "Vector transform has too many regions");
  std::vector<std::pair<TRegion*, QString>> pending;
  for (UINT index = 0; index < vector->getRegionCount(); ++index)
    pending.emplace_back(vector->getRegion(index), QString());
  QStringList result;
  qint64 edgeCount = 0;
  for (size_t index = 0; index < pending.size(); ++index) {
    const auto* region = pending[index].first;
    edgeCount += region->getEdgeCount();
    if (edgeCount > kMaxTransformEdges ||
        pending.size() + region->getSubregionCount() >
            size_t(kMaxTransformEdges))
      throw Error("resource_limit",
                  "Vector transform is limited to 65536 region edges");
    const int style  = region->getStyle();
    const auto edges = regionEdges(*region, affine, nativeIds);
    const QJsonObject record{
        {"parent", pending[index].second},
        {"style", style},
        {"children", static_cast<int>(region->getSubregionCount())},
        {"edges", edges}};
    result.append(QString::fromUtf8(
        QJsonDocument(record).toJson(QJsonDocument::Compact)));
    for (UINT child = 0; child < region->getSubregionCount(); ++child)
      pending.emplace_back(region->getSubregion(child),
                           edges.first().toString());
  }
  std::sort(result.begin(), result.end());
  return QJsonArray::fromStringList(result);
}

void rebuildReflectedRegions(const TVectorImageP& before,
                             const TVectorImageP& after,
                             const TAffine& affine) {
  if (affine.det() >= 0.0) return;
  std::vector<int> indices;
  std::vector<std::unique_ptr<TStroke>> retained;
  std::vector<TStroke*> previous;
  for (UINT index = 0; index < before->getStrokeCount(); ++index) {
    indices.push_back(static_cast<int>(index));
    retained.emplace_back(std::make_unique<TStroke>(*before->getStroke(index)));
    previous.push_back(retained.back().get());
  }
  // Native color transfer needs original strokes and the reversed winding.
  // An empty previous-stroke list permits a whole-group shortcut that skips it.
  if (!indices.empty()) after->notifyChangedStrokes(indices, previous, true);
}

bool verifyStrokeTransform(const TStroke& before, const TStroke& after,
                           const TAffine& affine, double thicknessScale) {
  if (before.getId() != after.getId() ||
      before.getStyle() != after.getStyle() ||
      before.isSelfLoop() != after.isSelfLoop() ||
      before.getControlPointCount() != after.getControlPointCount())
    throw Error("native_error", "Native transform changed stroke identity");
  bool changed = false;
  for (int index = 0; index < before.getControlPointCount(); ++index) {
    const auto original = before.getControlPoint(index);
    const auto actual   = after.getControlPoint(index);
    const auto expected = transformedPoint(original, affine, thicknessScale);
    validateNativePoint(actual);
    if (actual != expected)
      throw Error("native_error", "Native transform did not apply the matrix");
    changed |= actual != original;
  }
  return changed;
}

bool verifyVectorTransform(const TVectorImageP& before,
                           const TVectorImageP& after, const TAffine& affine,
                           double thicknessScale) {
  if (before->getStrokeCount() != after->getStrokeCount())
    throw Error("native_error", "Native transform changed the stroke count");
  bool changed = false;
  for (UINT index = 0; index < before->getStrokeCount(); ++index) {
    if (before->getGroupDepth(index) != after->getGroupDepth(index))
      throw Error("native_error", "Native transform changed a stroke group");
    changed |=
        verifyStrokeTransform(*before->getStroke(index),
                              *after->getStroke(index), affine, thicknessScale);
  }
  return changed;
}

QJsonObject transformVector(Context& context, Document& document,
                            const QJsonObject& params) {
  sharing(params);
  if (requiredString(params, "coordinate_space") != "drawing_native")
    invalid("Vector transforms use drawing_native coordinates");
  const QString mode = requiredString(params, "thickness_mode");
  if (mode != "preserve" && mode != "scale_geometric_mean")
    invalid("Unsupported vector thickness_mode");
  const TAffine affine = vectorAffine(params);
  const double thicknessScale =
      mode == "preserve" ? 1.0 : std::sqrt(std::abs(affine.det()));
  auto* level         = simpleLevel(document, params);
  const auto frame    = frameId(params);
  const auto original = vectorFor(level, frame);
  for (UINT index = 0; index < original->getStrokeCount(); ++index) {
    validateNativeStroke(*original->getStroke(index), paletteFor(level));
    preflightStrokeTransform(*original->getStroke(index), affine,
                             thicknessScale);
  }
  requireAssetGrowth(*document.scene, 0);
  TVectorImageP vector(original->cloneImage());
  vector->findRegions();
  const QJsonArray topology = vectorTopology(vector, affine);
  vector->transform(affine, mode == "scale_geometric_mean");
  rebuildReflectedRegions(original, vector, affine);
  if (vectorTopology(vector, TAffine()) != topology)
    throw Error("native_error", "Native transform changed painted topology");
  const bool changed =
      verifyVectorTransform(original, vector, affine, thicknessScale);
  if (changed) publish(context, level, frame, vector);
  return {{"stroke_count", static_cast<int>(vector->getStrokeCount())},
          {"geometry_indices_invalidated", changed},
          {"sharing", "all_exposures"},
          {"thickness_mode", mode},
          {"changed", changed}};
}

struct GroupSpan final {
  int first;
  int count;
  int parent;
  int depth;
};

struct GroupStructure final {
  std::vector<GroupSpan> groups;
  std::vector<int> membership;
  int entered = 0;
  bool nested = false;
};

void appendGroupStroke(GroupStructure& result, std::vector<int>& stack,
                       int index, int depth, int common) {
  stack.resize(common);
  while (int(stack.size()) < depth) {
    if (result.groups.size() >= size_t(kMaxStrokes))
      throw Error("resource_limit",
                  "Vector structure is limited to 4096 groups");
    const int parent = stack.empty() ? -1 : stack.back();
    stack.push_back(static_cast<int>(result.groups.size()));
    result.groups.push_back({index, 0, parent, static_cast<int>(stack.size())});
  }
  for (int group : stack) ++result.groups[group].count;
  result.membership.push_back(stack.empty() ? -1 : stack.back());
  result.nested |= depth > 1;
}

GroupStructure groupStructure(const TVectorImageP& image) {
  GroupStructure result;
  result.entered = image->isInsideGroup();
  if (result.entered < 0 || result.entered > 16)
    throw Error("resource_limit", "Vector structure admits group depths 0..16");
  std::vector<int> stack;
  std::set<int> ids;
  for (UINT index = 0; index < image->getStrokeCount(); ++index) {
    const int depth = image->getGroupDepth(index);
    if (depth < 0 || depth > 16)
      throw Error("resource_limit",
                  "Vector structure admits group depths 0..16");
    if (!ids.insert(image->getStroke(index)->getId()).second)
      throw Error("unsupported_geometry",
                  "Native stroke IDs must be unique within a drawing");
    const int common =
        index == 0 ? 0
                   : std::min({depth, int(stack.size()),
                               image->getCommonGroupDepth(index - 1, index)});
    appendGroupStroke(result, stack, index, depth, common);
  }
  return result;
}

QJsonValue nullableIndex(int index) {
  return index < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(index);
}

QJsonArray groupSpans(const GroupStructure& structure) {
  QJsonArray result;
  for (size_t index = 0; index < structure.groups.size(); ++index) {
    const auto& group = structure.groups[index];
    result.append(QJsonObject{{"index", static_cast<int>(index)},
                              {"parent_index", nullableIndex(group.parent)},
                              {"first_index", group.first},
                              {"count", group.count},
                              {"depth", group.depth}});
  }
  return result;
}

QJsonObject boundedGroupResult(const QJsonObject& result) {
  if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > 1024 * 1024)
    throw Error("resource_limit",
                "Vector structure results are limited to 1 MiB");
  return result;
}

QJsonObject inspectVectorStructure(Context&, Document& document,
                                   const QJsonObject& params) {
  auto* level      = simpleLevel(document, params);
  const auto frame = frameId(params);
  validateFrameNumber(level, frame);
  const auto image     = vectorFor(level, frame);
  const auto structure = groupStructure(image);
  QJsonArray strokes;
  for (UINT index = 0; index < image->getStrokeCount(); ++index)
    strokes.append(QJsonObject{
        {"index", static_cast<int>(index)},
        {"native_id", image->getStroke(index)->getId()},
        {"group_depth", image->getGroupDepth(index)},
        {"innermost_group_index", nullableIndex(structure.membership[index])}});
  QJsonArray reasons;
  if (structure.entered != 0) reasons.append("entered_group_context");
  if (structure.nested) reasons.append("nested_groups");
  return boundedGroupResult(
      {{"document_id", document.id},
       {"level_id", requiredString(params, "level_id")},
       {"drawing_id", frameJson(frame)},
       {"geometry_revision", document.revisionText()},
       {"scope", "detached"},
       {"order", "bottom_to_top"},
       {"inside_group_depth", structure.entered},
       {"stroke_count", static_cast<int>(image->getStrokeCount())},
       {"strokes", strokes},
       {"groups", groupSpans(structure)},
       {"write_admission", reasons.isEmpty()},
       {"unsupported_reasons", reasons}});
}

enum class GroupAction { Create, Remove, Move };

struct GroupEdit final {
  GroupAction action;
  int first;
  int count;
  int before   = 0;
  bool changed = true;
  std::vector<int> order;
  std::vector<int> membership;
};

void requireFlatGroups(const GroupStructure& structure) {
  if (structure.entered != 0)
    throw Error("unsupported_grouping",
                "Exit the native entered group before editing structure");
  if (structure.nested)
    throw Error("unsupported_grouping",
                "Group writes admit only drawings with flat root groups");
}

void requireGroupRange(const GroupEdit& edit) {
  if (edit.first > int(edit.membership.size()) - edit.count)
    invalid("The requested stroke range exceeds the drawing");
}

bool completeGroup(const GroupEdit& edit, const GroupStructure& structure) {
  const int group = edit.membership[edit.first];
  if (group < 0) return false;
  const auto& span = structure.groups[group];
  return span.first == edit.first && span.count == edit.count;
}

void requireUngroupedRange(const GroupEdit& edit) {
  for (int index = edit.first; index < edit.first + edit.count; ++index)
    if (edit.membership[index] >= 0)
      throw Error("unsupported_grouping",
                  "The range must be ungrouped or one complete flat group");
}

void prepareCreateGroup(GroupEdit& edit, const GroupStructure& structure) {
  if (completeGroup(edit, structure)) {
    edit.changed = false;
    return;
  }
  requireUngroupedRange(edit);
  std::fill(edit.membership.begin() + edit.first,
            edit.membership.begin() + edit.first + edit.count, kMaxStrokes);
}

void prepareRemoveGroup(GroupEdit& edit, const GroupStructure& structure) {
  const int group = edit.membership[edit.first];
  if (group < 0) throw Error("not_found", "There is no group at first_index");
  const auto& span = structure.groups[group];
  if (span.first != edit.first)
    invalid("first_index must address the first stroke of a complete group");
  edit.count = span.count;
  std::fill(edit.membership.begin() + edit.first,
            edit.membership.begin() + edit.first + edit.count, -1);
}

bool destinationSplitsGroup(const GroupEdit& edit) {
  if (edit.before == 0 || edit.before == int(edit.order.size())) return false;
  return edit.membership[edit.before] >= 0 &&
         edit.membership[edit.before] == edit.membership[edit.before - 1];
}

void prepareMoveGroup(GroupEdit& edit, const GroupStructure& structure) {
  if (!completeGroup(edit, structure)) requireUngroupedRange(edit);
  if (edit.before > int(edit.order.size()))
    invalid("before_index exceeds the drawing");
  if (edit.before >= edit.first && edit.before <= edit.first + edit.count) {
    edit.changed = false;
    return;
  }
  if (destinationSplitsGroup(edit))
    throw Error("unsupported_grouping",
                "before_index cannot split another group");
  auto begin = edit.order.begin();
  if (edit.before < edit.first)
    std::rotate(begin + edit.before, begin + edit.first,
                begin + edit.first + edit.count);
  else
    std::rotate(begin + edit.first, begin + edit.first + edit.count,
                begin + edit.before);
}

GroupEdit prepareGroupEdit(GroupAction action, const GroupStructure& structure,
                           const QJsonObject& params) {
  requireFlatGroups(structure);
  GroupEdit edit{action, integer(params, "first_index", 0, kMaxStrokes - 1), 1};
  edit.membership = structure.membership;
  for (size_t index = 0; index < edit.membership.size(); ++index)
    edit.order.push_back(static_cast<int>(index));
  if (action != GroupAction::Remove)
    edit.count = integer(params, "count", 1, kMaxStrokes);
  requireGroupRange(edit);
  switch (action) {
  case GroupAction::Create:
    prepareCreateGroup(edit, structure);
    break;
  case GroupAction::Remove:
    prepareRemoveGroup(edit, structure);
    break;
  case GroupAction::Move:
    edit.before = integer(params, "before_index", 0, kMaxStrokes);
    prepareMoveGroup(edit, structure);
    break;
  }
  return edit;
}

void applyGroupEdit(const TVectorImageP& candidate, const GroupEdit& edit) {
  switch (edit.action) {
  case GroupAction::Create:
    candidate->group(edit.first, edit.count);
    break;
  case GroupAction::Remove:
    if (candidate->ungroup(edit.first) != edit.count)
      throw Error("native_error",
                  "Native ungroup changed an unexpected stroke range");
    break;
  case GroupAction::Move:
    if (!candidate->canMoveStrokes(edit.first, edit.count, edit.before))
      throw Error("unsupported_grouping",
                  "The native grouping model rejects this move");
    candidate->moveStrokes(edit.first, edit.count, edit.before);
    break;
  }
}

void verifyGroupMembership(const GroupEdit& edit,
                           const GroupStructure& actual) {
  for (size_t index = 0; index < edit.order.size(); ++index) {
    const int expected = edit.membership[edit.order[index]];
    if ((expected < 0) != (actual.membership[index] < 0))
      throw Error("native_error",
                  "Native grouping changed unexpected stroke membership");
    if (index == 0) continue;
    const bool sameExpected =
        expected >= 0 && expected == edit.membership[edit.order[index - 1]];
    const bool sameActual =
        actual.membership[index] >= 0 &&
        actual.membership[index] == actual.membership[index - 1];
    if (sameExpected != sameActual)
      throw Error("native_error",
                  "Native grouping split or combined an unexpected group");
  }
}

void verifyGroupGeometry(const TVectorImageP& original,
                         const TVectorImageP& candidate,
                         const GroupEdit& edit) {
  if (candidate->getStrokeCount() != original->getStrokeCount())
    throw Error("native_error", "Native grouping changed the stroke count");
  for (size_t index = 0; index < edit.order.size(); ++index) {
    const auto& before = *original->getStroke(edit.order[index]);
    const auto& after  = *candidate->getStroke(index);
    verifyStrokeTransform(before, after, TAffine(), 1.0);
    if (before.getOpacity() != after.getOpacity() ||
        before.getOpacityProfile() != after.getOpacityProfile())
      throw Error("native_error", "Native grouping changed stroke opacity");
  }
  const auto structure = groupStructure(candidate);
  requireFlatGroups(structure);
  verifyGroupMembership(edit, structure);
}

QJsonObject groupEditResult(const TVectorImageP& original,
                            const TVectorImageP& candidate,
                            const GroupEdit& edit, const QJsonObject& params) {
  std::vector<int> positions(edit.order.size());
  for (size_t index = 0; index < edit.order.size(); ++index)
    positions[edit.order[index]] = static_cast<int>(index);
  QJsonArray mapping;
  for (size_t index = 0; index < positions.size(); ++index)
    mapping.append(
        QJsonObject{{"native_id", original->getStroke(index)->getId()},
                    {"previous_index", static_cast<int>(index)},
                    {"index", positions[index]}});
  return boundedGroupResult(
      {{"level_id", requiredString(params, "level_id")},
       {"drawing_id", frameJson(frameId(params))},
       {"scope", "detached"},
       {"sharing", "all_exposures"},
       {"changed", edit.changed},
       {"geometry_indices_invalidated", edit.changed},
       {"undo_available", false},
       {"stroke_count", static_cast<int>(candidate->getStrokeCount())},
       {"groups", groupSpans(groupStructure(candidate))},
       {"order_mapping", mapping}});
}

QJsonObject editVectorGroups(Context& context, Document& document,
                             const QJsonObject& params, GroupAction action) {
  sharing(params);
  auto* level      = simpleLevel(document, params);
  const auto frame = frameId(params);
  validateFrameNumber(level, frame);
  const auto original = vectorFor(level, frame);
  const auto edit = prepareGroupEdit(action, groupStructure(original), params);
  for (UINT index = 0; index < original->getStrokeCount(); ++index)
    validateNativeStroke(*original->getStroke(index), paletteFor(level));
  requireAssetGrowth(*document.scene, 0);
  if (!edit.changed) return groupEditResult(original, original, edit, params);
  auto candidate = original->clone();
  candidate->findRegions();
  const auto topology = vectorTopology(candidate, TAffine(), true);
  applyGroupEdit(candidate, edit);
  candidate->findRegions();
  verifyGroupGeometry(original, candidate, edit);
  if (vectorTopology(candidate, TAffine(), true) != topology)
    throw Error(
        "unsupported_grouping",
        "Grouping would change painted boundaries; "
        "select complete boundaries or keep the current group structure");
  const auto result = groupEditResult(original, candidate, edit, params);
  publish(context, level, frame, candidate);
  return result;
}

QJsonObject createVectorGroup(Context& context, Document& document,
                              const QJsonObject& params) {
  return editVectorGroups(context, document, params, GroupAction::Create);
}

QJsonObject removeVectorGroup(Context& context, Document& document,
                              const QJsonObject& params) {
  return editVectorGroups(context, document, params, GroupAction::Remove);
}

QJsonObject moveVectorOrder(Context& context, Document& document,
                            const QJsonObject& params) {
  return editVectorGroups(context, document, params, GroupAction::Move);
}

QJsonObject opacityIdentity(Document& document, const QJsonObject& params) {
  if (requiredString(params, "expected_revision") != document.revisionText())
    throw Error("revision_conflict",
                "Drawing indices expired; inspect the current revision.");
  return {{"document_id", document.id},
          {"level_id", requiredString(params, "level_id")},
          {"drawing_id", frameJson(frameId(params))},
          {"geometry_revision", document.revisionText()}};
}

std::vector<int> opacityIndices(const QJsonObject& params, int count) {
  std::set<int> unique;
  for (const auto& value : array(params, "stroke_indices", 1, 128)) {
    const QJsonObject entry{{"index", value}};
    const int index = integer(entry, "index", 0, count - 1);
    if (!unique.insert(index).second) invalid("Stroke indices must be unique");
  }
  return {unique.begin(), unique.end()};
}

QJsonArray opacityIndexJson(const std::vector<int>& indices) {
  QJsonArray result;
  for (int index : indices) result.append(index);
  return result;
}

TVectorImageP opacityImage(TXshSimpleLevel* level, const TFrameId& frame) {
  validateFrameNumber(level, frame);
  const auto image = vectorFor(level, frame);
  for (UINT index = 0; index < image->getStrokeCount(); ++index)
    validateNativeStroke(*image->getStroke(index), paletteFor(level));
  validateVectorOpacity(image);
  groupStructure(image);
  return image;
}

QJsonObject nearestOpacityStroke(const TVectorImageP& image,
                                 const QJsonObject& query) {
  const TPointD point(number(query, "x", -kMaxCoordinate, kMaxCoordinate),
                      number(query, "y", -kMaxCoordinate, kMaxCoordinate));
  const double maximum = number(query, "max_distance", 0, kMaxCoordinate);
  double bestDistance  = maximum * maximum;
  QJsonObject result;
  for (UINT index = 0; index < image->getStrokeCount(); ++index) {
    double parameter = 0, distance = 0;
    if (!image->getStroke(index)->getNearestW(point, parameter, distance,
                                              false))
      throw Error("unsupported_geometry",
                  "Cannot locate native stroke centerline.");
    if (distance > bestDistance ||
        (!result.isEmpty() && distance == bestDistance))
      continue;
    bestDistance = distance;
    result       = {{"stroke_index", static_cast<int>(index)},
                    {"parameter", parameter},
                    {"distance", std::sqrt(distance)}};
  }
  return result;
}

TRectD opacityQueryBounds(const QJsonObject& query) {
  const double x0 = number(query, "x0", -kMaxCoordinate, kMaxCoordinate);
  const double y0 = number(query, "y0", -kMaxCoordinate, kMaxCoordinate);
  const double x1 = number(query, "x1", -kMaxCoordinate, kMaxCoordinate);
  const double y1 = number(query, "y1", -kMaxCoordinate, kMaxCoordinate);
  if (x1 < x0 || y1 < y0) invalid("Selection bounds must be ordered");
  return TRectD(x0, y0, x1, y1);
}

std::vector<int> opacityBounds(const TVectorImageP& image,
                               const QJsonObject& query) {
  const QString relation = requiredString(query, "relation");
  if (relation != "intersects" && relation != "contained")
    invalid("Unknown bounds relation");
  const auto box = opacityQueryBounds(query);
  std::vector<int> result;
  for (UINT index = 0; index < image->getStrokeCount(); ++index) {
    const auto bounds = image->getStroke(index)->getCenterlineBBox();
    if (relation == "contained" ? box.contains(bounds) : box.overlaps(bounds))
      result.push_back(index);
    if (result.size() > 128)
      throw Error("resource_limit",
                  "Selection exceeds 128 strokes; narrow the query.");
  }
  return result;
}

QJsonObject selectVector(Context&, Document& document,
                         const QJsonObject& params) {
  auto result = opacityIdentity(document, params);
  if (requiredString(params, "coordinate_space") != "drawing_native")
    invalid("Selection requires drawing_native coordinates");
  const auto image =
      opacityImage(simpleLevel(document, params), frameId(params));
  const auto query   = params.value("query").toObject();
  const QString mode = requiredString(query, "mode");
  std::vector<int> indices;
  QJsonValue nearest(QJsonValue::Null);
  if (mode == "indices") {
    indices = opacityIndices(query, image->getStrokeCount());
  } else if (mode == "centerline_bounds") {
    indices = opacityBounds(image, query);
  } else if (mode == "nearest_centerline") {
    const auto match = nearestOpacityStroke(image, query);
    if (!match.isEmpty()) {
      nearest = match;
      indices.push_back(match.value("stroke_index").toInt());
    }
  } else
    invalid("Unknown vector selection mode");
  result.insert("coordinate_space", "drawing_native");
  result.insert("stroke_indices", opacityIndexJson(indices));
  result.insert("nearest", nearest);
  return boundedGroupResult(result);
}

QJsonObject inspectVectorOpacity(Context&, Document& document,
                                 const QJsonObject& params) {
  auto result = opacityIdentity(document, params);
  const auto image =
      opacityImage(simpleLevel(document, params), frameId(params));
  const int index =
      integer(params, "stroke_index", 0, int(image->getStrokeCount()) - 1);
  const auto& stroke  = *image->getStroke(index);
  const auto& profile = stroke.getOpacityProfile();
  const int offset    = integer(params, "offset", 0, profile.size());
  const int limit     = integer(params, "limit", 1, 128);
  const int end       = std::min(int(profile.size()), offset + limit);
  QJsonArray knots;
  for (int i = offset; i < end; ++i)
    knots.append(
        QJsonObject{{"position", profile[i].x}, {"opacity", profile[i].y}});
  result.insert("stroke_index", index);
  result.insert("opacity_per_mille", nativeOpacityPerMille(stroke));
  result.insert("knot_count", int(profile.size()));
  result.insert("knots", knots);
  result.insert("next_offset", end < int(profile.size())
                                   ? QJsonValue(end)
                                   : QJsonValue(QJsonValue::Null));
  return boundedGroupResult(result);
}

std::vector<TPointD> opacityProfile(const QJsonObject& params) {
  std::vector<TPointD> knots;
  for (const auto& value : array(params, "knots", 0, 4096)) {
    const auto knot = value.toObject();
    knots.emplace_back(number(knot, "position", 0, 1),
                       number(knot, "opacity", 0, 1));
  }
  if (!StrokeOpacity::valid(knots))
    invalid("Invalid native opacity envelope endpoints or ordering");
  return knots;
}

bool equalOpacityProfile(const std::vector<TPointD>& a,
                         const std::vector<TPointD>& b) {
  if (a.size() != b.size()) return false;
  for (size_t index = 0; index < a.size(); ++index)
    if (a[index].x != b[index].x || a[index].y != b[index].y) return false;
  return true;
}

bool applyOpacity(TStroke& stroke, const QJsonObject& params,
                  const std::vector<TPointD>& knots, bool profile) {
  if (profile) {
    if (equalOpacityProfile(stroke.getOpacityProfile(), knots)) return false;
    if (!stroke.setOpacityProfile(knots))
      invalid("Native opacity envelope was rejected");
  } else {
    const int units = integer(params, "opacity_per_mille", 0, 1000);
    if (nativeOpacityPerMille(stroke) == units) return false;
    stroke.setOpacity(units / 1000.0);
  }
  return true;
}

TVectorImageP writableOpacityImage(TXshSimpleLevel* level,
                                   const TFrameId& frame) {
  if (level->isReadOnly() || level->isFrameReadOnly(frame))
    throw Error("locked", "The opacity target drawing is read-only.");
  paletteFor(level, true);
  const auto original  = opacityImage(level, frame);
  const auto structure = groupStructure(original);
  if (structure.nested || structure.entered)
    throw Error("unsupported_type",
                "Opacity writes require flat groups at root context.");
  return original;
}

QJsonObject writeVectorOpacity(Context& context, Document& document,
                               const QJsonObject& params, bool profile) {
  auto result = opacityIdentity(document, params);
  sharing(params);
  auto* level         = simpleLevel(document, params);
  const auto frame    = frameId(params);
  const auto original = writableOpacityImage(level, frame);
  const auto indices  = opacityIndices(params, original->getStrokeCount());
  const auto knots = profile ? opacityProfile(params) : std::vector<TPointD>();
  auto candidate   = original->cloneWithGroupContext();
  bool changed     = false;
  for (int index : indices)
    changed =
        applyOpacity(*candidate->getStroke(index), params, knots, profile) ||
        changed;
  validateVectorOpacity(candidate);
  requireAssetGrowth(*document.scene,
                     automationImageBytes(candidate) -
                         automationImageBytes(TImageP(original.getPointer())));
  result.insert("changed", changed);
  result.insert("sharing", "all_exposures");
  result.insert("stroke_indices", opacityIndexJson(indices));
  result.insert("undo_available", false);
  if (changed)
    result.insert("geometry_revision", QString::number(document.revision + 1));
  result = boundedGroupResult(result);
  if (changed) publish(context, level, frame, candidate);
  return result;
}

QJsonObject setVectorOpacity(Context& context, Document& document,
                             const QJsonObject& params) {
  return writeVectorOpacity(context, document, params, false);
}

QJsonObject setVectorOpacityProfile(Context& context, Document& document,
                                    const QJsonObject& params) {
  return writeVectorOpacity(context, document, params, true);
}

void validateVectorProfile(TXshSimpleLevel* level, const TFrameId& frame) {
  const auto vector = vectorFor(level, frame);
  validateVectorOpacity(vector);
  for (UINT index = 0; index < vector->getStrokeCount(); ++index)
    validateNativeStroke(*vector->getStroke(index), paletteFor(level));
}

void validateFrameProfile(TXshSimpleLevel* level, const TFrameId& frame) {
  if (frame.getNumber() < 1 || frame.getNumber() > 999999 ||
      !QRegularExpression(QRegularExpression::anchoredPattern("[a-z]{0,8}"))
           .match(frame.getLetter())
           .hasMatch())
    throw Error("unsupported_drawing_id",
                "Persistence requires drawing numbers 1..999999 and lowercase "
                "ASCII suffixes");
  validateFrameNumber(level, frame);
  const auto image  = imageFor(level, frame);
  const auto format = imageFormat(image);
  static const std::map<int, QString> formats{{PLI_XSHLEVEL, "vector"},
                                              {TZP_XSHLEVEL, "toonzRaster"},
                                              {OVL_XSHLEVEL, "rgba8"}};
  if (formats.at(level->getType()) != format)
    throw Error("unsupported_type",
                "Drawing image type does not match its level type");
  if (image->raster())
    rasterDimensions(image->raster()->getLx(), image->raster()->getLy());
  else
    validateVectorProfile(level, frame);
}

TXshSimpleLevel* budgetLevel(TLevelSet* levels, int index) {
  auto* level = levels->getLevel(index)->getSimpleLevel();
  if (!level)
    throw Error("unsupported_type",
                "Asset accounting requires native simple drawing levels");
  return level;
}

qint64 paletteAssetBytes(TPalette* palette) {
  if (!palette) return 0;
  const int styles = palette->getStyleCount();
  if (styles > kMaxStyles)
    throw Error("resource_limit", "Palette exceeds the 4096-style limit");
  qint64 snapshots = styles;
  for (int id = 0; id < styles; ++id) {
    const int keys = palette->getKeyframeCount(id);
    if (keys > 4096)
      throw Error("resource_limit", "A style has more than 4096 keyframes");
    snapshots += keys;
  }
  if (palette->getPageCount() > kMaxPages)
    throw Error("resource_limit", "Palette exceeds the 128-page limit");
  return snapshots * AutomationStyleBytes +
         qint64(palette->getPageCount()) * AutomationPageBytes;
}

void requireAssetCounts(TLevelSet* levels, int additionalDrawings,
                        int additionalLevels) {
  const qint64 levelCount = qint64(levels->getLevelCount()) + additionalLevels;
  if (levelCount < 0 || levelCount > 256)
    throw Error("resource_limit", "The document admits at most 256 levels");
  qint64 drawings = additionalDrawings;
  for (int index = 0; index < levels->getLevelCount(); ++index)
    if (!levels->getLevel(index)->getSoundLevel())
      drawings += budgetLevel(levels, index)->getFrameCount();
  if (drawings < 0 || drawings > 4096)
    throw Error("resource_limit", "The document admits at most 4096 drawings");
}

qint64 sceneAssetBytes(TLevelSet* levels) {
  qint64 bytes = 0;
  for (int index = 0; index < levels->getLevelCount(); ++index) {
    if (auto* sound = levels->getLevel(index)->getSoundLevel()) {
      bytes += automationSoundBytes(*sound);
      continue;
    }
    auto* level = budgetLevel(levels, index);
    bytes += paletteAssetBytes(level->getPalette());
    for (const auto& frame : level->getFids())
      bytes += automationImageBytes(imageFor(level, frame));
  }
  return bytes;
}

}  // namespace

qint64 automationRasterBytes(int width, int height) {
  rasterDimensions(width, height);
  return qint64(width) * height * 4 + AutomationDrawingOverheadBytes;
}

qint64 automationImageBytes(const TImageP& image) {
  if (!image)
    throw Error("asset_unavailable",
                "Cannot estimate a missing native drawing");
  imageFormat(image);
  if (const TRasterP raster = image->raster())
    return automationRasterBytes(raster->getLx(), raster->getLy());
  const TVectorImageP vector = image;
  qint64 bytes               = AutomationDrawingOverheadBytes;
  for (UINT index = 0; index < vector->getStrokeCount(); ++index)
    bytes += qint64(vector->getStroke(index)->getControlPointCount()) * 64 +
             1024 +
             qint64(vector->getStroke(index)->getOpacityProfile().size()) *
                 sizeof(TPointD);
  return bytes;
}

void requireAssetGrowth(ToonzScene& scene, qint64 additionalBytes,
                        int additionalDrawings, int additionalLevels) {
  auto* levels = scene.getLevelSet();
  requireAssetCounts(levels, additionalDrawings, additionalLevels);
  const qint64 bytes = sceneAssetBytes(levels);
  if (additionalBytes < -bytes ||
      additionalBytes > AutomationAssetLimitBytes - bytes)
    throw Error("resource_limit",
                "The document exceeds the 256 MiB native asset estimate; "
                "reduce drawing dimensions, geometry, or palette keys");
}

QJsonArray drawingOperations() {
  QJsonArray tools;
  drawingCatalog(tools);
  renumberCatalog(tools);
  vectorCatalog(tools);
  for (const auto& tool : vectorOpacityOperations()) tools.append(tool);
  vectorGroupCatalog(tools);
  paletteCatalog(tools);
  palettePageCatalog(tools);
  paletteStyleCatalog(tools);
  rasterCatalog(tools);
  return tools;
}

void validateDrawingProfile(TXshSimpleLevel* level) {
  if (level->getType() != PLI_XSHLEVEL && level->getType() != TZP_XSHLEVEL &&
      level->getType() != OVL_XSHLEVEL)
    throw Error(
        "unsupported_type",
        "Persistence admits only vector, Toonz raster and RGBA8 levels");
  if (level->getFrameCount() > 4096)
    throw Error("resource_limit",
                "Persistence admits at most 4096 drawings per level");
  if (level->getType() != OVL_XSHLEVEL)
    validatePaletteProfile(paletteFor(level), level->getType() == PLI_XSHLEVEL
                                                  ? kMaxVectorFrame
                                                  : 999999);
  for (const auto& frame : level->getFids()) validateFrameProfile(level, frame);
}

QJsonValue executeDrawing(Context& context, const QString& method,
                          const QJsonObject& params) {
  auto& document = context.document(requiredString(params, "document_id"));
  using Handler  = QJsonObject (*)(Context&, Document&, const QJsonObject&);
  static const std::map<QString, Handler> handlers{
      {"drawing.create", createDrawing},
      {"drawing.inspect", inspectDrawing},
      {"drawing.clone", cloneDrawing},
      {"drawing.remove", removeDrawing},
      {"drawing.renumber", renumberDrawing},
      {"vector.inspect", inspectVector},
      {"vector.select", selectVector},
      {"vector.opacity.inspect", inspectVectorOpacity},
      {"vector.opacity.set", setVectorOpacity},
      {"vector.opacity.profile.set", setVectorOpacityProfile},
      {"vector.edit", editVector},
      {"vector.fill", fillVector},
      {"vector.transform", transformVector},
      {"vector.structure", inspectVectorStructure},
      {"vector.group.create", createVectorGroup},
      {"vector.group.remove", removeVectorGroup},
      {"vector.order.move", moveVectorOrder},
      {"palette.inspect", inspectPalette},
      {"palette.create_style", createStyle},
      {"palette.update_style", updateStyle},
      {"palette.pages", inspectPages},
      {"palette.page.create", createPage},
      {"palette.page.rename", renamePage},
      {"palette.page.move", movePage},
      {"palette.page.remove", removePage},
      {"palette.style.move", moveStyle},
      {"palette.rename_style", renameStyle},
      {"palette.clone_style", cloneStyle},
      {"raster.read_pixels", readPixels},
      {"raster.patch", patchPixels},
      {"raster.import_png", importPng},
      {"raster.export_png", exportPng}};
  const auto handler = handlers.find(method);
  if (handler == handlers.end())
    throw Error("method_not_found", "Unknown drawing operation: " + method);
  return handler->second(context, document, params);
}

}  // namespace GroundzeroAutomation
