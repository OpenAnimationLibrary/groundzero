#include "automation_sheet_record.h"

#include "automation.h"

#include "tdoubleparam.h"
#include "toutputproperties.h"
#include "trasterfx.h"
#include "toonz/levelproperties.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <vector>

namespace GroundzeroAutomation {
namespace {

QString sha256(const QJsonValue& value) {
  const QByteArray bytes =
      value.isArray()
          ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
          : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString columnKind(const TXshColumn& column) {
  switch (column.getColumnType()) {
  case TXshColumn::eLevelType:
    return "level";
  case TXshColumn::eSoundType:
    return "sound";
  case TXshColumn::eSoundTextType:
    return "sound_text";
  case TXshColumn::eZeraryFxType:
    return "generator";
  case TXshColumn::ePaletteType:
    return "palette";
  case TXshColumn::eMeshType:
    return "mesh";
  }
  return "unknown";
}

QJsonObject objectReference(const TStageObjectId& id) {
  QString kind = "none";
  if (id.isTable()) kind = "table";
  if (id.isPegbar()) kind = "pegbar";
  if (id.isColumn()) kind = "column";
  if (id.isCamera()) kind = "camera";
  return {{"kind", kind}, {"index", id.isTable() ? 0 : id.getIndex()}};
}

QJsonArray cellRun(int start, int length, const TXshCell& cell) {
  return {start, length, QString::fromStdWString(cell.m_level->getName()),
          cell.m_frameId.getNumber(), cell.m_frameId.getLetter()};
}

// Cell runs of a level column as [start, length, level, number, suffix].
QJsonArray levelRuns(TXsheet& sheet, int index) {
  QJsonArray runs;
  TXshColumn* column = sheet.getColumn(index);
  if (!column->getLevelColumn() || column->isEmpty()) return runs;
  int first = 0;
  int last  = -1;
  column->getRange(first, last);
  int start = first;
  for (int row = first; row <= last + 1; ++row) {
    const bool end      = row > last;
    const TXshCell cell = end ? TXshCell() : sheet.getCell(row, index);
    const TXshCell held = sheet.getCell(start, index);
    if (!end && cell == held) continue;
    if (!held.isEmpty()) runs.append(cellRun(start, row - start, held));
    start = row;
  }
  return runs;
}

QJsonObject columnRecord(TXsheet& sheet, int index) {
  TXshColumn* column = sheet.getColumn(index);
  if (!column) return {{"kind", "empty"}};
  TStageObject* object = sheet.getStageObjectTree()->getStageObject(
      TStageObjectId::ColumnId(index), false);
  const QJsonArray runs = levelRuns(sheet, index);
  return {{"kind", columnKind(*column)},
          {"name", object ? QString::fromStdString(object->getName())
                          : QString("Col%1").arg(index + 1)},
          {"preview_visible", column->isPreviewVisible()},
          {"camstand_visible", column->isCamstandVisible()},
          {"locked", column->isLocked()},
          {"opacity", int(column->getOpacity())},
          {"color_filter_id", column->getColorFilterId()},
          // A column without a stage object gets one parented to the table
          // when the saver or the loader touches it.
          {"parent", objectReference(object ? object->getParent()
                                            : TStageObjectId::TableId)},
          {"cell_runs", runs.size()},
          {"cells_sha256", sha256(runs)}};
}

QJsonObject sheetSection(ToonzScene& scene) {
  TXsheet& sheet = *scene.getXsheet();
  QJsonArray columns;
  for (int index = 0; index < sheet.getFirstFreeColumnIndex(); ++index)
    columns.append(columnRecord(sheet, index));
  return {{"columns", columns}, {"columns_sha256", sha256(columns)}};
}

bool usesHandles(TDoubleKeyframe::Type type) {
  return type == TDoubleKeyframe::SpeedInOut ||
         type == TDoubleKeyframe::EaseInOut ||
         type == TDoubleKeyframe::EaseInOutPercentage;
}

// The fields a key's interpolation reads: handles only around speed and ease
// segments, text only for expression, file and similar-shape segments.
QJsonObject keyRecord(const TDoubleKeyframe& key) {
  QJsonObject record{{"frame", key.m_frame},
                     {"value", key.m_value},
                     {"type", int(key.m_type)},
                     {"step", key.m_step}};
  if (usesHandles(key.m_prevType))
    record.insert("speed_in", QJsonArray{key.m_speedIn.x, key.m_speedIn.y});
  if (usesHandles(key.m_type))
    record.insert("speed_out", QJsonArray{key.m_speedOut.x, key.m_speedOut.y});
  if (!TDoubleKeyframe::isKeyframeBased(key.m_type)) {
    record.insert("expression", QString::fromStdString(key.m_expressionText));
    record.insert("unit", QString::fromStdString(key.m_unitName));
    record.insert("similar_shape_offset", key.m_similarShapeOffset);
  }
  return record;
}

QString curveDigest(TStageObject& object) {
  QJsonArray channels;
  for (int channel = 0; channel < TStageObject::T_ChannelCount; ++channel) {
    const TDoubleParamP curve =
        object.getParam(static_cast<TStageObject::Channel>(channel));
    QJsonArray keys;
    for (int index = 0; index < curve->getKeyframeCount(); ++index)
      keys.append(keyRecord(curve->getKeyframe(index)));
    channels.append(
        QJsonObject{{"default", curve->getDefaultValue()}, {"keys", keys}});
  }
  return sha256(channels);
}

QJsonObject cameraRecord(TStageObjectTree& objects, TStageObject& object) {
  const TStageObjectId id = object.getId();
  TCamera* camera         = object.getCamera();
  const TRect interest    = camera->getInterestRect();
  QJsonArray roles;
  if (objects.getCurrentCameraId() == id) roles.append("output");
  if (objects.getCurrentPreviewCameraId() == id) roles.append("preview");
  return {{"index", id.getIndex()},
          {"name", QString::fromStdString(object.getName())},
          {"width_px", camera->getRes().lx},
          {"height_px", camera->getRes().ly},
          {"width_in", camera->getSize().lx},
          {"height_in", camera->getSize().ly},
          {"interest_rect",
           QJsonArray{interest.x0, interest.y0, interest.x1, interest.y1}},
          {"x_prevalence", camera->isXPrevalence()},
          {"parent", objectReference(object.getParent())},
          {"roles", roles},
          {"curves_sha256", curveDigest(object)}};
}

QJsonObject cameraSection(ToonzScene& scene) {
  TStageObjectTree& objects = *scene.getXsheet()->getStageObjectTree();
  std::vector<TStageObject*> cameras;
  for (int index = 0; index < objects.getStageObjectCount(); ++index)
    if (objects.getStageObject(index)->getId().isCamera())
      cameras.push_back(objects.getStageObject(index));
  std::sort(cameras.begin(), cameras.end(),
            [](TStageObject* left, TStageObject* right) {
              return left->getId().getIndex() < right->getId().getIndex();
            });
  QJsonArray records;
  for (TStageObject* camera : cameras)
    records.append(cameraRecord(objects, *camera));
  return {{"cameras", records},
          {"output_camera", objects.getCurrentCameraId().getIndex()},
          {"preview_camera", objects.getCurrentPreviewCameraId().getIndex()}};
}

QJsonArray colorRecord(const TPixel32& color) {
  return {int(color.r), int(color.g), int(color.b), int(color.m)};
}

QJsonObject outputRecord(TOutputProperties& output) {
  int first = 0;
  int last  = 0;
  int step  = 0;
  output.getRange(first, last, step);
  const TRenderSettings& render = output.getRenderSettings();
  return {{"range", QJsonArray{first, last}},
          {"step", step},
          {"frame_rate", output.getFrameRate()},
          {"shrink", render.m_shrinkX},
          {"bpp", render.m_bpp},
          {"linear_color_space", render.m_linearColorSpace},
          {"aspect_mode", output.getAspectMode()},
          {"aspect_ratio", output.getAspectRatio()},
          {"aspect_color", colorRecord(output.getAspectColor())},
          {"sync_color_settings", output.isColorSettingsSynced()}};
}

QJsonArray guidesRecord(const TSceneProperties::Guides& guides) {
  QJsonArray values;
  for (const double value : guides) values.append(value);
  return values;
}

QJsonObject propertiesSection(ToonzScene& scene) {
  TSceneProperties& properties = *scene.getProperties();
  int distance                 = 0;
  int offset                   = 0;
  int secondary                = 0;
  properties.getMarkers(distance, offset, secondary);
  QJsonArray filters;
  for (const auto& filter : properties.getColorFilters())
    filters.append(QJsonObject{{"name", filter.name},
                               {"color", colorRecord(filter.color)}});
  return {{"output", outputRecord(*properties.getOutputProperties())},
          {"preview", outputRecord(*properties.getPreviewProperties())},
          {"background_color", colorRecord(properties.getBgColor())},
          {"field_guide", QJsonObject{{"size", properties.getFieldGuideSize()},
                                      {"aspect_ratio",
                                       properties.getFieldGuideAspectRatio()}}},
          {"markers", QJsonArray{distance, offset, secondary}},
          {"ruler_guides",
           QJsonObject{{"horizontal", guidesRecord(properties.getHGuides())},
                       {"vertical", guidesRecord(properties.getVGuides())}}},
          {"column_filters_on_render",
           properties.isColumnColorFilterOnRenderEnabled()},
          {"color_filters", filters}};
}

QString valueText(const QJsonValue& value) {
  if (value.isUndefined()) return "absent";
  if (value.isArray())
    return QString::fromUtf8(
        QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
  if (value.isObject())
    return QString::fromUtf8(
        QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
  return QString::fromUtf8(
      QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

// The path of the first leaf where two records differ, or an empty string.
QString firstDifference(const QJsonValue& expected, const QJsonValue& actual,
                        const QString& path) {
  if (expected == actual) return {};
  if (expected.isObject() && actual.isObject()) {
    const QJsonObject left  = expected.toObject();
    const QJsonObject right = actual.toObject();
    QStringList keys        = left.keys() + right.keys();
    keys.removeDuplicates();
    keys.sort();
    for (const QString& key : keys) {
      const QString found =
          firstDifference(left.value(key), right.value(key), path + "." + key);
      if (!found.isEmpty()) return found;
    }
  }
  if (expected.isArray() && actual.isArray() &&
      expected.toArray().size() == actual.toArray().size()) {
    const QJsonArray left  = expected.toArray();
    const QJsonArray right = actual.toArray();
    for (int index = 0; index < left.size(); ++index) {
      const QString found = firstDifference(left.at(index), right.at(index),
                                            path + QString("[%1]").arg(index));
      if (!found.isEmpty()) return found;
    }
  }
  return path;
}

QJsonValue valueAt(const QJsonValue& root, const QString& path) {
  QJsonValue value = root;
  for (const QString& part : path.split('.', Qt::SkipEmptyParts)) {
    const int bracket = part.indexOf('[');
    value = value.toObject().value(bracket < 0 ? part : part.left(bracket));
    for (int open = bracket; open >= 0; open = part.indexOf('[', open + 1))
      value = value.toArray().at(part.mid(open + 1).section(']', 0, 0).toInt());
  }
  return value;
}

}  // namespace

QJsonObject automationSheetRecord(ToonzScene& scene) {
  return {{"sheet", sheetSection(scene)},
          {"cameras", cameraSection(scene)},
          {"scene_properties", propertiesSection(scene)}};
}

QJsonObject automationLevelSettings(TXshSimpleLevel& level) {
  LevelProperties& properties = *level.getProperties();
  QJsonObject settings{{"subsampling", properties.getSubsampling()},
                       {"antialias_softness", properties.antialiasSoftness()},
                       {"premultiply", properties.doPremultiply()},
                       {"white_transparent", properties.whiteTransp()},
                       {"color_space_gamma", properties.colorSpaceGamma()}};
  if (level.getType() == PLI_XSHLEVEL) return settings;
  settings.insert("dpi_policy",
                  properties.getDpiPolicy() == LevelProperties::DP_CustomDpi
                      ? "custom"
                      : "image");
  settings.insert("dpi",
                  QJsonArray{properties.getDpi().x, properties.getDpi().y});
  return settings;
}

void requireEqualRecord(const QString& section, const QJsonValue& expected,
                        const QJsonValue& actual) {
  const QString path = firstDifference(expected, actual, section);
  if (path.isEmpty()) return;
  throw Error(
      "invalid_package",
      "The reopened scene differs from the saved scene at " + path + ".",
      {{"section", section},
       {"path", path},
       {"saved", valueText(valueAt(expected, path.section('.', 1, -1)))},
       {"reopened", valueText(valueAt(actual, path.section('.', 1, -1)))}});
}

void requireAutomationSheetRecord(const QJsonObject& expected,
                                  ToonzScene& reopened) {
  const QJsonObject actual = automationSheetRecord(reopened);
  for (const QString& section : {"sheet", "cameras", "scene_properties"})
    requireEqualRecord(section, expected.value(section), actual.value(section));
}

}  // namespace GroundzeroAutomation
