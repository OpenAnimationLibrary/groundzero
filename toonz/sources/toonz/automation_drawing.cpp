#include "automation.h"
#include "automation_drawing.h"

#include "tcolorstyles.h"
#include "timage_io.h"
#include "timageinfo.h"
#include "tpalette.h"
#include "trasterimage.h"
#include "tsimplecolorstyles.h"
#include "tstroke.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <typeinfo>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int kMaxPixels  = 16777216;
constexpr int kMaxPatch   = 16384;
constexpr int kMaxStrokes = 4096;
constexpr int kMaxPoints  = 1025;
constexpr int kMaxStyles  = 4096;

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
      !QRegularExpression("^[a-z]{0,8}$").match(suffix.toString()).hasMatch())
    invalid(
        "Drawing suffix must contain zero to eight lowercase ASCII letters");
  return TFrameId(frame, suffix.toString());
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

TImageP emptyImage(Context& context, TXshSimpleLevel* level,
                   const QJsonObject& params) {
  const QString format = requiredString(params, "format");
  if (format == "vector") {
    if (level->getType() != PLI_XSHLEVEL)
      invalid("Vector drawings require a vector level");
    if (params.contains("width") || params.contains("height") ||
        params.contains("dpi"))
      invalid("Vector drawings do not accept raster dimensions or DPI");
    return TVectorImageP(new TVectorImage);
  }
  if ((format != "rgba8" || level->getType() != OVL_XSHLEVEL) &&
      (format != "toonzRaster" || level->getType() != TZP_XSHLEVEL))
    invalid("Drawing format must match the native level type");
  const int width  = integer(params, "width", 1, 8192);
  const int height = integer(params, "height", 1, 8192);
  const double dpi = number(params, "dpi", 1.0, 2400.0);
  rasterDimensions(width, height);
  compatibleRaster(level, TDimension(width, height), dpi);
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
  if (level->isFid(destination))
    throw Error("already_exists", "Destination drawing exists");
  if (level->getFrameCount() >= 4096)
    throw Error("resource_limit", "Level already has 4096 drawings");
  const auto image = imageFor(level, source);
  imageFormat(image);
  publish(context, level, destination, TImageP(image->cloneImage()));
  return imageSummary(level, destination);
}

struct ExposureScan final {
  TXshSimpleLevel* level;
  TFrameId frame;
  qint64 remaining = 1000000;
  std::set<TXsheet*> visited;
};

bool referencesInColumn(TXshCellColumn* column, ExposureScan& scan);

bool referencesInSheet(TXsheet* sheet, ExposureScan& scan) {
  if (!sheet || !scan.visited.insert(sheet).second) return false;
  if (scan.visited.size() > 128)
    throw Error("resource_limit", "Exposure scan exceeds 128 nested sheets");
  for (int index = 0; index < sheet->getColumnCount(); ++index) {
    auto* column = sheet->getColumn(index);
    if (column && referencesInColumn(column->getCellColumn(), scan))
      return true;
  }
  return false;
}

bool referencesInColumn(TXshCellColumn* column, ExposureScan& scan) {
  int first = 0, last = -1;
  if (!column || !column->getRange(first, last)) return false;
  scan.remaining -= static_cast<qint64>(last) - first + 1;
  if (scan.remaining < 0)
    throw Error("resource_limit", "Exposure scan exceeds one million cells");
  for (int row = first; row <= last; ++row) {
    const auto& cell = column->getCell(row);
    if (cell.getSimpleLevel() == scan.level && cell.m_frameId == scan.frame)
      return true;
    auto* child = cell.getChildLevel();
    if (child && referencesInSheet(child->getXsheet(), scan)) return true;
  }
  return false;
}

bool referencedDrawing(Document& document, TXshSimpleLevel* level,
                       const TFrameId& frame) {
  ExposureScan scan{level, frame, 1000000, {}};
  if (referencesInSheet(document.scene->getXsheet(), scan)) return true;
  auto* levels = document.scene->getLevelSet();
  for (int i = 0; i < levels->getLevelCount(); ++i) {
    auto* child = levels->getLevel(i)->getChildLevel();
    if (child && referencesInSheet(child->getXsheet(), scan)) return true;
  }
  return false;
}

QJsonObject removeDrawing(Context& context, Document& document,
                          const QJsonObject& params) {
  auto* level      = simpleLevel(document, params);
  const auto frame = frameId(params);
  imageFor(level, frame);
  if (referencedDrawing(document, level, frame))
    throw Error("in_use", "Remove every exposure before removing this drawing");
  context.markMutationStarted();
  level->eraseFrame(frame);
  level->setDirtyFlag(true);
  return {{"removed", frameJson(frame)}};
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
  const int end =
      std::min(offset + limit, static_cast<int>(vector->getStrokeCount()));
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
          {"total", static_cast<int>(vector->getStrokeCount())},
          {"geometry_revision", document.revisionText()},
          {"coordinate_space", "drawing_native"},
          {"thickness", "native_radius"},
          {"next_offset", end}};
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
    result.emplace_back(number(point, "x", -1000000.0, 1000000.0),
                        number(point, "y", -1000000.0, 1000000.0),
                        number(point, "thickness", 0.0, 10000.0));
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
  TVectorImageP vector(original->cloneImage());
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
  const TPointD point(number(params, "x", -1000000.0, 1000000.0),
                      number(params, "y", -1000000.0, 1000000.0));
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
  const int end    = std::min(offset + limit, palette->getStyleCount());
  QJsonArray styles;
  for (int id = offset; id < end; ++id) styles.append(styleJson(palette, id));
  return {{"styles", styles},
          {"total", palette->getStyleCount()},
          {"next_offset", end},
          {"locked", palette->isLocked()},
          {"color_space", "rgba8_straight_native"},
          {"frame_base", 0},
          {"sharing", "whole_level"}};
}

QJsonObject createStyle(Context& context, Document& document,
                        const QJsonObject& params) {
  auto* level        = simpleLevel(document, params);
  auto* palette      = paletteFor(level, true);
  const auto rgba    = styleColor(params);
  const QString name = requiredString(params, "name");
  if (name.size() > 128) invalid("Style name must have at most 128 characters");
  if (palette->getStyleCount() >= kMaxStyles)
    throw Error("resource_limit", "Palette already contains 4096 styles");
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
                    const TPixel32& color) {
  const auto keys = palette->getStyleAnimation(id);
  if (!params.contains("keyframe"))
    return keys.empty() && palette->getStyle(id)->getMainColor() == color;
  const int frame = integer(params, "keyframe", 0, 999999);
  const auto key  = keys.find(frame);
  if (key == keys.end()) return false;
  return typeid(*key->second.style) == typeid(TSolidColorStyle) &&
         key->second.style->getMainColor() == color;
}

QJsonObject updateStyle(Context& context, Document& document,
                        const QJsonObject& params) {
  auto* level     = simpleLevel(document, params);
  auto* palette   = paletteFor(level, true);
  const int id    = styleId(params, palette);
  auto* style     = solidEditableStyle(palette, id);
  const auto rgba = styleColor(params);
  if (sameStyleColor(palette, id, params, rgba)) return styleJson(palette, id);
  auto animation = palette->getStyleAnimation(id);
  std::unique_ptr<TColorStyle> updated(style->clone());
  updated->setMainColor(rgba);
  if (params.contains("keyframe")) {
    const int frame = integer(params, "keyframe", 0, 999999);
    auto found      = animation.find(frame);
    if (found == animation.end()) {
      if (animation.size() >= 4096)
        throw Error("resource_limit", "Style has too many keyframes");
      TPalette::StyleKey key;
      key.style = TColorStyleP(updated->clone());
      animation.emplace(frame, std::move(key));
    } else {
      std::unique_ptr<TColorStyle> key(found->second.style->clone());
      if (typeid(*key) != typeid(TSolidColorStyle))
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

void validatePngHeader(const QString& path) {
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

QJsonObject drawingIdentity() {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"properties",
           QJsonObject{{"number", range(1, 999999)},
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
  QJsonObject coordinate{
      {"type", "number"}, {"minimum", -1000000}, {"maximum", 1000000}};
  QJsonObject thickness{{"type", "number"}, {"minimum", 0}, {"maximum", 10000}};
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
                                     {"minimum", -1000000},
                                     {"maximum", 1000000}});
  properties.insert("y", properties.value("x"));
  properties.insert("style_id", range(0, kMaxStyles - 1));
  tools.append(
      operation("vector.fill",
                "Assign a style to the native closed region containing a point",
                properties,
                {"document_id", "level_id", "drawing_id", "coordinate_space",
                 "sharing", "x", "y", "style_id"},
                true));
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
      "name",
      QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}});
  tools.append(operation("palette.create_style",
                         "Add a solid RGBA8 straight-alpha style to page zero",
                         properties,
                         {"document_id", "level_id", "color", "name"}, true));
  properties.remove("name");
  properties.insert("style_id", range(1, kMaxStyles - 1));
  properties.insert("keyframe", range(0, 999999));
  tools.append(operation(
      "palette.update_style",
      "Edit a solid style for the whole level; animated "
      "styles require a zero-based keyframe and preserve other "
      "keys/interpolation",
      properties, {"document_id", "level_id", "style_id", "color"}, true));
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

void validatePaletteProfile(TPalette* palette) {
  if (!palette) return;
  if (palette->getStyleCount() > kMaxStyles || palette->getRefImg() ||
      !palette->getRefImgPath().isEmpty() || !palette->getGlobalName().empty())
    throw Error("unsupported_palette",
                "Palette exceeds limits or refers to an external asset");
  for (int id = 0; id < palette->getStyleCount(); ++id) {
    validateStyleProfile(palette->getStyle(id));
    const auto keys = palette->getStyleAnimation(id);
    if (keys.size() > 4096)
      throw Error("resource_limit", "A style has more than 4096 keyframes");
    for (const auto& key : keys)
      validateStyleProfile(key.second.style.getPointer());
  }
}

void validateFrameProfile(TXshSimpleLevel* level, const TFrameId& frame) {
  if (frame.getNumber() < 1 || frame.getNumber() > 999999 ||
      !QRegularExpression("^[a-z]{0,8}$").match(frame.getLetter()).hasMatch())
    throw Error("unsupported_drawing_id",
                "Persistence requires drawing numbers 1..999999 and lowercase "
                "ASCII suffixes");
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
}

}  // namespace

QJsonArray drawingOperations() {
  QJsonArray tools;
  drawingCatalog(tools);
  vectorCatalog(tools);
  paletteCatalog(tools);
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
    validatePaletteProfile(paletteFor(level));
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
      {"vector.inspect", inspectVector},
      {"vector.edit", editVector},
      {"vector.fill", fillVector},
      {"palette.inspect", inspectPalette},
      {"palette.create_style", createStyle},
      {"palette.update_style", updateStyle},
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
