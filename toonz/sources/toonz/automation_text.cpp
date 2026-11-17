#include "automation_text.h"
#include "automation_drawing.h"
#include "typetoolvector.h"

#include "tpalette.h"
#include "tregion.h"
#include "tsimplecolorstyles.h"
#include "tstroke.h"
#include "toonz/stageunits.h"
#include "toonz/toonzscene.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCryptographicHash>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QJsonDocument>
#include <QRawFont>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr char TextSchemas[] = R"json(
[
  {
    "name": "text.fonts",
    "inputSchema": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "family_prefix": {
          "type": "string",
          "maxLength": 128
        },
        "offset": {
          "type": "integer",
          "minimum": 0,
          "maximum": 4096
        },
        "limit": {
          "type": "integer",
          "minimum": 1,
          "maximum": 64
        }
      },
      "required": [
        "family_prefix",
        "offset",
        "limit"
      ]
    },
    "writes": false
  },
  {
    "name": "vector.text.create",
    "inputSchema": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "document_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "expected_revision": {
          "type": "string",
          "minLength": 1,
          "maxLength": 20
        },
        "operation_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "level_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "drawing_id": {
          "type": "object",
          "additionalProperties": false,
          "properties": {
            "number": {
              "type": "integer",
              "minimum": 1,
              "maximum": 65535
            },
            "suffix": {
              "type": "string",
              "pattern": "^[a-z]{0,8}$"
            }
          },
          "required": [
            "number",
            "suffix"
          ]
        },
        "text": {
          "type": "string",
          "minLength": 1,
          "maxLength": 1024
        },
        "font": {
          "type": "object",
          "additionalProperties": false,
          "properties": {
            "family": {
              "type": "string",
              "minLength": 1,
              "maxLength": 256
            },
            "style": {
              "type": "string",
              "minLength": 1,
              "maxLength": 256
            },
            "size_points": {
              "type": "integer",
              "minimum": 1,
              "maximum": 256
            }
          },
          "required": [
            "family",
            "style",
            "size_points"
          ]
        },
        "baseline": {
          "type": "object",
          "additionalProperties": false,
          "properties": {
            "x": {
              "type": "number",
              "minimum": -32768,
              "maximum": 32768
            },
            "y": {
              "type": "number",
              "minimum": -32768,
              "maximum": 32768
            }
          },
          "required": [
            "x",
            "y"
          ]
        },
        "style_id": {
          "type": "integer",
          "minimum": 1,
          "maximum": 4095
        }
      },
      "required": [
        "document_id",
        "expected_revision",
        "operation_id",
        "level_id",
        "drawing_id",
        "text",
        "font",
        "baseline",
        "style_id"
      ]
    },
    "writes": true
  }
]
)json";

constexpr int MaxResponse = 1048576;
constexpr int MaxNames    = 4096;
constexpr int MaxGlyphs   = 4096;
constexpr int MaxRuns     = 1024;
constexpr int MaxPoints   = 32768;

void require(bool condition, const QString& code, const QString& message) {
  if (!condition) throw Error(code, message);
}

void boundedName(const QString& name) {
  require(!name.isEmpty(), "resource_limit",
          "Native font metadata contains an empty name.");
  require(name.size() <= 512, "resource_limit",
          "Native font name exceeds 256 scalars.");
  require(name.toUcs4().size() <= 256, "resource_limit",
          "Native font name exceeds 256 scalars.");
}

QJsonObject boundedResult(const QJsonObject& value) {
  require(QJsonDocument(value).toJson(QJsonDocument::Compact).size() + 1 <=
              MaxResponse,
          "resource_limit",
          "Text result exceeds 1 MiB; shorten the requested text.");
  return value;
}

QStringList sortedNames(QStringList names, int maximum) {
  require(names.size() <= maximum, "resource_limit",
          "Native font enumeration exceeds its bound.");
  for (const auto& name : names) boundedName(name);
  names.removeDuplicates();
  std::sort(names.begin(), names.end());
  return names;
}

QJsonObject fonts(const QJsonObject& params) {
  const auto prefix = params.value("family_prefix").toString();
  const int offset  = integerParam(params, "offset", 0, MaxNames, 0);
  const int limit   = integerParam(params, "limit", 1, 64, 32);
  const QFontDatabase database;
  const auto all = sortedNames(database.families(), MaxNames);
  QStringList matching;
  for (const auto& family : all)
    if (family.startsWith(prefix, Qt::CaseSensitive)) matching.append(family);
  require(offset <= matching.size(), "invalid_params",
          "Font offset exceeds the matching count.");
  const int end = std::min(offset + limit, matching.size());
  QJsonArray rows;
  for (int index = offset; index < end; ++index) {
    const auto& family = matching[index];
    const auto styles  = sortedNames(database.styles(family), 128);
    rows.append(QJsonObject{{"family", family},
                            {"styles", QJsonArray::fromStringList(styles)}});
  }
  return boundedResult(
      {{"scope", "worker_host"},
       {"families", rows},
       {"next_offset",
        end == matching.size() ? QJsonValue() : QJsonValue(end)}});
}

void validateText(const QString& text) {
  require(!text.isEmpty(), "invalid_params", "Text must not be empty.");
  require(text.size() <= 1024, "resource_limit",
          "Text exceeds 1024 UTF-16 code units.");
  require(text.count('\n') < 32, "resource_limit", "Text exceeds 32 lines.");
  for (int index = 0; index < text.size(); ++index) {
    const QChar character = text[index];
    if (character.isHighSurrogate()) {
      require(index + 1 < text.size() && text[index + 1].isLowSurrogate(),
              "invalid_params", "Text contains an unpaired high surrogate.");
      ++index;
      continue;
    }
    require(!character.isLowSurrogate(), "invalid_params",
            "Text contains an unpaired surrogate.");
    const bool permitted = character == '\n' || character == '\t';
    require(permitted || character.category() != QChar::Other_Control,
            "invalid_params",
            "Text contains a control character other than LF or TAB.");
    require(character != QChar(0x2028) && character != QChar(0x2029),
            "invalid_params", "Use LF for explicit text line breaks.");
  }
}

TypeToolText::TypeToolTextEntry entryFor(const QJsonObject& params) {
  const auto font = params.value("font").toObject();
  TypeToolText::TypeToolTextEntry entry;
  entry.source      = requiredString(params, "text");
  entry.font.family = requiredString(font, "family");
  entry.font.style  = requiredString(font, "style");
  entry.font.size =
      QString::number(integerParam(font, "size_points", 1, 256, 32));
  validateText(entry.source);
  const QFontDatabase database;
  const auto families = sortedNames(database.families(), MaxNames);
  require(families.contains(entry.font.family), "font_not_found",
          "Requested font family is not installed; inspect text.fonts.");
  const auto styles = sortedNames(database.styles(entry.font.family), 128);
  require(styles.contains(entry.font.style), "font_not_found",
          "Requested font style is not installed; inspect text.fonts.");
  return entry;
}

QByteArray fontHash(const QRawFont& font) {
  return QCryptographicHash::hash(font.fontTable("name"),
                                  QCryptographicHash::Sha256)
      .toHex();
}

bool ignorableCluster(const QString& text) {
  for (const auto scalar : text.toUcs4()) {
    if (QChar::isSpace(scalar)) continue;
    if (QChar::category(scalar) == QChar::Other_Format) continue;
    return false;
  }
  return true;
}

bool hasOutline(const QGlyphRun& run) {
  bool visible = false;
  for (const auto glyph : run.glyphIndexes()) {
    require(glyph != 0, "missing_glyph",
            "A visible text cluster contains a missing glyph.");
    visible |= !run.rawFont().pathForGlyph(glyph).isEmpty();
  }
  return visible;
}

void visibleCluster(const QTextLayout& layout, int first, int end,
                    const QRawFont& selected) {
  const auto runs = layout.glyphRuns(first, end - first);
  bool outline    = false;
  for (const auto& run : runs) {
    const auto font = run.rawFont();
    require(font.familyName() == selected.familyName() &&
                font.styleName() == selected.styleName() &&
                fontHash(font) == fontHash(selected),
            "font_fallback",
            "A visible text cluster uses a different font face.");
    outline |= hasOutline(run);
  }
  require(outline, "unsupported_outline",
          "A visible text cluster has no vector outline.");
}

void inspectClusters(const QTextLayout& layout, const QString& text,
                     const QRawFont& selected) {
  int first = 0;
  for (int end = 1; end <= text.size(); ++end) {
    if (end != text.size() && !layout.isValidCursorPosition(end)) continue;
    if (!ignorableCluster(text.mid(first, end - first)))
      visibleCluster(layout, first, end, selected);
    first = end;
  }
}

struct FontInspection final {
  QRawFont selected;
  QJsonArray runs;
  int glyphs = 0;

  explicit FontInspection(const TypeToolText::TypeToolFontSettings& settings)
      : selected(QRawFont::fromFont(QFontDatabase().font(
            settings.family, settings.style, settings.size.toInt()))) {
    require(selected.isValid(), "font_not_found",
            "Native selected font face is unavailable.");
  }

  void append(const QGlyphRun& run, int block) {
    const auto indexes = run.glyphIndexes();
    if (indexes.isEmpty()) return;
    require(runs.size() < MaxRuns, "resource_limit",
            "Text exceeds 1024 native font runs.");
    require(indexes.size() <= MaxGlyphs - glyphs, "resource_limit",
            "Text exceeds 4096 native glyphs.");
    glyphs += indexes.size();
    const auto font = run.rawFont();
    boundedName(font.familyName());
    boundedName(font.styleName());
    require(std::isfinite(font.pixelSize()) && font.pixelSize() > 0 &&
                font.pixelSize() <= 1024,
            "resource_limit",
            "Native text pixel size exceeds the admitted bounds.");
    QJsonArray ids;
    for (const auto id : indexes) {
      require(id != 0, "missing_glyph",
              "Native text run contains a missing glyph.");
      ids.append(static_cast<double>(id));
    }
    runs.append(QJsonObject{
        {"block", block},
        {"family", font.familyName()},
        {"style", font.styleName()},
        {"pixel_size", font.pixelSize()},
        {"right_to_left", run.isRightToLeft()},
        {"glyph_ids", ids},
        {"name_table_sha256", QString::fromLatin1(fontHash(font))}});
  }

  void inspect(const QTextDocument& document) {
    int blockIndex = 0;
    for (auto block = document.begin(); block.isValid();
         block      = block.next(), ++blockIndex) {
      const auto* layout = block.layout();
      inspectClusters(*layout, block.text(), selected);
      for (int line = 0; line < layout->lineCount(); ++line)
        for (const auto& run : layout->lineAt(line).glyphRuns())
          append(run, blockIndex);
    }
  }
};

TPointD baselineFor(const QJsonObject& params) {
  const auto baseline = params.value("baseline").toObject();
  const TPointD point(baseline.value("x").toDouble(),
                      baseline.value("y").toDouble());
  require(baseline.value("x").isDouble() && baseline.value("y").isDouble(),
          "invalid_params", "Text baseline requires numeric x and y.");
  require(std::isfinite(point.x) && std::isfinite(point.y), "invalid_params",
          "Text baseline coordinates must be finite.");
  require(std::abs(point.x) <= 32768 && std::abs(point.y) <= 32768,
          "invalid_params", "Text baseline coordinates exceed native bounds.");
  return point;
}

void boundedCoordinate(double coordinate) {
  require(std::isfinite(coordinate) && std::abs(coordinate) <= 32768,
          "resource_limit",
          "Text outline exceeds native coordinate bounds; reduce size or "
          "baseline.");
}

void inspectPaths(const TypeToolText::OutlineLayout& layout, double scale,
                  const TPointD& baseline) {
  int elements = 0;
  for (const auto& path : layout.paths) {
    require(path.elementCount() <= MaxPoints - elements, "resource_limit",
            "Text layout exceeds 32768 outline elements.");
    elements += path.elementCount();
    for (int index = 0; index < path.elementCount(); ++index) {
      const auto element = path.elementAt(index);
      boundedCoordinate(baseline.x + scale * element.x);
      boundedCoordinate(baseline.y - scale * element.y);
    }
  }
  require(elements != 0, "unsupported_outline",
          "Text contains no drawable vector outlines.");
}

void inspectStrokes(const TVectorImageP& image) {
  const auto count = image->getStrokeCount();
  require(count != 0, "unsupported_outline",
          "Native text conversion produced no strokes.");
  require(count <= 4096, "resource_limit",
          "Native text conversion exceeds 4096 strokes.");
  int points = 0;
  for (UINT index = 0; index < count; ++index) {
    const auto* stroke = image->getStroke(index);
    const int size     = stroke->getControlPointCount();
    require(size <= 1025 && size <= MaxPoints - points, "resource_limit",
            "Native text conversion exceeds control point bounds.");
    points += size;
    for (int point = 0; point < size; ++point) {
      const auto control = stroke->getControlPoint(point);
      boundedCoordinate(control.x);
      boundedCoordinate(control.y);
    }
  }
}

void inspectRegions(const TVectorImageP& image) {
  require(image->getRegionCount() <= 4096, "resource_limit",
          "Text exceeds 4096 native regions.");
  std::vector<std::pair<TRegion*, int>> pending;
  for (UINT index = 0; index < image->getRegionCount(); ++index)
    pending.emplace_back(image->getRegion(index), 0);
  int edges = 0;
  for (size_t index = 0; index < pending.size(); ++index) {
    auto* region    = pending[index].first;
    const int depth = pending[index].second;
    require(depth <= 16, "resource_limit",
            "Text region nesting exceeds depth 16.");
    require(region->getEdgeCount() <= UINT(65536 - edges), "resource_limit",
            "Text exceeds 65536 native region edges.");
    edges += region->getEdgeCount();
    TPointD point;
    require(region->getInternalPoint(point), "unsupported_outline",
            "Native outline classification failed to find a region interior "
            "point.");
    require(pending.size() + region->getSubregionCount() <= 4096,
            "resource_limit",
            "Text exceeds 4096 native regions including children.");
    for (UINT child = 0; child < region->getSubregionCount(); ++child)
      pending.emplace_back(region->getSubregion(child), depth + 1);
  }
}

TXshSimpleLevel* targetLevel(Document& document, const QJsonObject& params) {
  auto* level =
      document.level(requiredString(params, "level_id"))->getSimpleLevel();
  require(level && level->getType() == PLI_XSHLEVEL, "unsupported_type",
          "Text creation requires an owned vector level.");
  require(!level->isReadOnly(), "locked", "Text level is read-only.");
  require(level->getPalette(), "unsupported_type",
          "Text level has no native palette.");
  require(!level->getPalette()->isLocked(), "locked",
          "Text level palette is locked.");
  require(level->getFrameCount() < 4096, "resource_limit",
          "Text level already has 4096 drawings.");
  validateDrawingProfile(level);
  return level;
}

TFrameId newFrame(TXshSimpleLevel* level, const QJsonObject& params) {
  const auto drawing = params.value("drawing_id").toObject();
  const int number   = integerParam(drawing, "number", 1, 65535, 1);
  const auto suffix  = drawing.value("suffix").toString();
  const QRegularExpression pattern(QStringLiteral("^[a-z]{0,8}$"));
  require(pattern.match(suffix).hasMatch(), "invalid_params",
          "Text drawing suffix is invalid.");
  const TFrameId frame(number, suffix);
  require(!level->isFid(frame), "already_exists",
          "Text drawing already exists; choose a new ID.");
  return frame;
}

int selectedStyle(TXshSimpleLevel* level, const QJsonObject& params) {
  const int id        = integerParam(params, "style_id", 1, 4095, 1);
  const auto* palette = level->getPalette();
  require(id < palette->getStyleCount(), "invalid_params",
          "Text style index does not exist.");
  const auto* style = palette->getStyle(id);
  require(style && typeid(*style) == typeid(TSolidColorStyle),
          "unsupported_style",
          "Text creation requires an existing solid palette style.");
  return id;
}

QJsonObject resultFor(Document& document, const QJsonObject& params,
                      const TVectorImageP& image, const QJsonArray& runs) {
  const auto bounds = image->getBBox();
  QJsonArray indices;
  for (UINT index = 0; index < image->getStrokeCount(); ++index)
    indices.append(int(index));
  return {{"changed", true},
          {"document_id", document.id},
          {"operation_id", params.value("operation_id")},
          {"revision", QString::number(document.revision + 1)},
          {"level_id", params.value("level_id")},
          {"drawing_id", params.value("drawing_id")},
          {"geometry_revision", QString::number(document.revision + 1)},
          {"geometry_units", "native_vector"},
          {"native_units_per_qt_pixel", Stage::inch / Stage::standardDpi},
          {"baseline", params.value("baseline")},
          {"bounds", QJsonObject{{"x0", bounds.x0},
                                 {"y0", bounds.y0},
                                 {"x1", bounds.x1},
                                 {"y1", bounds.y1}}},
          {"stroke_indices", indices},
          {"group_index", indices.size() == 1 ? QJsonValue() : QJsonValue(0)},
          {"style_id", params.value("style_id")},
          {"source_recipe", QJsonObject{{"text", params.value("text")},
                                        {"font", params.value("font")}}},
          {"font_runs", runs},
          {"editable_as", "native_vector_strokes"},
          {"semantic_text_persisted", false},
          {"undo_available", false}};
}

QJsonObject createText(Context& context, const QJsonObject& params) {
  auto& document      = context.document(requiredString(params, "document_id"));
  auto* level         = targetLevel(document, params);
  const auto frame    = newFrame(level, params);
  const int style     = selectedStyle(level, params);
  const auto entry    = entryFor(params);
  const auto baseline = baselineFor(params);
  FontInspection inspection(entry.font);
  const auto layout = TypeToolText::inspectOutlineLayout(
      entry, [&](const QTextDocument& value) { inspection.inspect(value); });
  const double scale = Stage::inch / Stage::standardDpi;
  inspectPaths(layout, scale, baseline);
  auto image = TypeToolText::vectorizeOutlineLayout(
      layout, scale, style, [](const TVectorImageP& value) {
        inspectStrokes(value);
        inspectRegions(value);
      });
  image->setPalette(level->getPalette());
  image->transform(TTranslation(baseline));
  inspectStrokes(image);
  inspectRegions(image);
  requireAssetGrowth(*document.scene, automationImageBytes(image), 1);
  const auto result =
      boundedResult(resultFor(document, params, image, inspection.runs));
  context.markMutationStarted();
  level->setFrame(frame, image);
  level->touchFrame(frame);
  return result;
}

}  // namespace

QJsonArray textOperations() {
  const auto schemas = QJsonDocument::fromJson(TextSchemas).array();
  const QStringList descriptions{
      "List bounded installed native font families/styles on this worker host.",
      "Create a new editable vector drawing from native plain-text outlines; "
      "reject fallback, missing glyphs and unclassifiable regions before "
      "publication."};
  QJsonArray result;
  for (int index = 0; index < schemas.size(); ++index) {
    const auto definition = schemas[index].toObject();
    const auto schema     = definition.value("inputSchema").toObject();
    auto descriptor =
        operation(definition.value("name").toString(), descriptions[index],
                  schema.value("properties").toObject(),
                  schema.value("required").toArray(),
                  definition.value("writes").toBool());
    descriptor.insert("inputSchema", schema);
    result.append(descriptor);
  }
  return result;
}

QJsonValue executeText(Context& context, const QString& method,
                       const QJsonObject& params) {
  if (method == "text.fonts") return fonts(params);
  if (method == "vector.text.create") return createText(context, params);
  throw Error("method_not_found", "Unknown text operation: " + method);
}

}  // namespace GroundzeroAutomation
