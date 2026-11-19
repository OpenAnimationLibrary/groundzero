#include "automation_scene_host.h"
#include "automation_scene_shared.h"

#include "toutputproperties.h"
#include "trasterfx.h"
#include "toonz/sceneproperties.h"
#include "toonz/stage.h"
#include "toonz/toonzscene.h"

#include <QSet>

#include <cmath>
#include <vector>

namespace GroundzeroAutomation {
namespace SceneShared {
namespace {

constexpr int MaxGuides  = 256;
constexpr int MaxFilters = 64;

QJsonObject colorRecord(const TPixel32& color) {
  return {{"r", int(color.r)},
          {"g", int(color.g)},
          {"b", int(color.b)},
          {"a", int(color.m)}};
}

TPixel32 readColor(const QJsonObject& value) {
  return TPixel32(value.value("r").toInt(), value.value("g").toInt(),
                  value.value("b").toInt(), value.value("a").toInt());
}

QJsonObject aspectRecord(const TOutputProperties& output) {
  static const char* modes[] = {"none", "letterbox", "crop"};
  const int mode             = output.getAspectMode();
  return {{"mode", modes[mode >= 0 && mode < 3 ? mode : 0]},
          {"ratio", output.getAspectRatio()},
          {"color", colorRecord(output.getAspectColor())}};
}

QJsonObject outputRecord(TOutputProperties& output, int frames) {
  int first = 0;
  int last  = -1;
  int step  = 1;
  output.getRange(first, last, step);
  const bool all = first == 0 && last == -1;
  QJsonObject range{{"kind", all ? "all" : "frames"}};
  if (!all) {
    range.insert("start_frame", first);
    range.insert("end_frame", last);
  }
  const int end                 = all ? frames - 1 : std::min(last, frames - 1);
  const int start               = all ? 0 : first;
  const TRenderSettings& render = output.getRenderSettings();
  return {{"range", range},
          {"effective_range",
           end >= start ? QJsonValue(QJsonObject{{"start_frame", start},
                                                 {"end_frame", end}})
                        : QJsonValue()},
          {"step", step},
          {"shrink", render.m_shrinkX},
          {"bpp", render.m_bpp},
          {"linear_color_space", render.m_linearColorSpace},
          {"aspect_export", aspectRecord(output)},
          {"frame_rate", output.getFrameRate()},
          {"worker_render_applies", false}};
}

QJsonArray guideList(const TSceneProperties::Guides& guides, bool& truncated) {
  QJsonArray inches;
  for (const double value : guides) {
    if (inches.size() == MaxGuides) {
      truncated = true;
      break;
    }
    inches.append(value / Stage::inch);
  }
  return inches;
}

// Ruler guides are stored in stage units; hGuides hold the x positions of
// vertical lines (viewerdraw.cpp).
QJsonObject guidesRecord(TSceneProperties& properties) {
  bool truncated              = false;
  const QJsonArray vertical   = guideList(properties.getHGuides(), truncated);
  const QJsonArray horizontal = guideList(properties.getVGuides(), truncated);
  return {{"vertical_lines_x_in", vertical},
          {"horizontal_lines_y_in", horizontal},
          {"native_unit", "stage_units_times_device_pixel_ratio"},
          {"display_scale_dependent", true},
          {"truncated", truncated}};
}

QJsonObject markersRecord(const TSceneProperties& properties) {
  int distance = 0;
  int offset   = 0;
  int seconds  = 0;
  properties.getMarkers(distance, offset, seconds);
  return {{"interval", distance}, {"start_frame", offset}};
}

QJsonObject fieldGuideRecord(const TSceneProperties& properties) {
  return {{"size", properties.getFieldGuideSize()},
          {"aspect_ratio", properties.getFieldGuideAspectRatio()}};
}

QJsonObject scopeRow(const char* setting, const char* scope, bool stored,
                     const char* tool, const char* reason) {
  return {{"setting", setting},
          {"scope", scope},
          {"stored_in_scene", stored},
          {"editable_tool", tool ? QJsonValue(tool) : QJsonValue()},
          {"reason", reason ? QJsonValue(reason) : QJsonValue()}};
}

QJsonArray settingsScope() {
  return {
      scopeRow("output.range", "scene", true, "scene.output.set", nullptr),
      scopeRow("output.step", "scene", true, "scene.output.set", nullptr),
      scopeRow("output.shrink", "scene", true, "scene.output.set", nullptr),
      scopeRow("output.bpp", "scene", true, "scene.output.set", nullptr),
      scopeRow("output.aspect_export", "scene", true, "scene.output.set",
               nullptr),
      scopeRow("preview.range", "scene", true, "scene.output.set", nullptr),
      scopeRow("preview.shrink", "scene", true, "scene.output.set", nullptr),
      scopeRow("sync_color_settings", "scene", true, "scene.output.set",
               nullptr),
      scopeRow("column_filters_on_render", "scene", true, "scene.output.set",
               nullptr),
      scopeRow("frame_rate", "scene", true, "scene.settings", nullptr),
      scopeRow("background_color", "scene", true, "scene.settings", nullptr),
      scopeRow("field_guide", "scene", true, "scene.guides.set", nullptr),
      scopeRow("markers", "scene", true, "scene.guides.set", nullptr),
      scopeRow("ruler_guides", "scene", true, "scene.guides.set", nullptr),
      scopeRow("color_filters", "scene", true, nullptr,
               "conversion_production"),
      scopeRow("linear_color_space", "scene", true, nullptr,
               "not_used_by_worker"),
      scopeRow("output.path_and_format", "scene", true, nullptr,
               "render_delivery_w3"),
      scopeRow("output.time_stretch", "scene", true, nullptr, "retime_w6"),
      scopeRow("render_threads_and_tiles", "machine", true, nullptr,
               "machine_tuning"),
      scopeRow("safe_area", "user_view", false, nullptr, "editor_user_state"),
      scopeRow("viewer_aspect_guides", "user_view", false, nullptr,
               "worker_has_no_view_state"),
      scopeRow("camera_box", "user_view", false, nullptr,
               "worker_has_no_view_state"),
      scopeRow("field_guide_display", "user_view", false, nullptr,
               "editor_user_state")};
}

void requireRevision(Document& document, const QJsonObject& params) {
  if (requiredString(params, "expected_revision") != document.revisionText())
    throw Error(
        "stale_revision", "Refresh scene.summary before continuing.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
}

QJsonArray filterRecords(TSceneProperties& properties) {
  QJsonArray filters;
  const auto all = properties.getColorFilters();
  for (int index = 0; index < all.size() && filters.size() < MaxFilters;
       ++index)
    filters.append(QJsonObject{{"color_filter_id", index},
                               {"name", all.at(index).name},
                               {"color", colorRecord(all.at(index).color)}});
  return filters;
}

QJsonValue inspectProperties(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireRevision(document, params);
  TSceneProperties& properties = *document.scene->getProperties();
  const int frames             = document.scene->getFrameCount();
  return QJsonObject{
      {"output", outputRecord(*properties.getOutputProperties(), frames)},
      {"preview", outputRecord(*properties.getPreviewProperties(), frames)},
      {"sync_color_settings",
       properties.getPreviewProperties()->isColorSettingsSynced()},
      {"column_filters_on_render",
       properties.isColumnColorFilterOnRenderEnabled()},
      {"color_filters", filterRecords(properties)},
      {"background_color", colorRecord(properties.getBgColor())},
      {"field_guide", fieldGuideRecord(properties)},
      {"markers", markersRecord(properties)},
      {"ruler_guides", guidesRecord(properties)},
      {"frame_count", frames},
      {"settings_scope", settingsScope()}};
}

// ------------------------------------------------------------ output set

struct OutputBlock {
  int first, last, step, shrink, bpp, aspect;
  double ratio;
  TPixel32 color;
};

OutputBlock readBlock(TOutputProperties& output) {
  OutputBlock block{};
  output.getRange(block.first, block.last, block.step);
  block.shrink = output.getRenderSettings().m_shrinkX;
  block.bpp    = output.getRenderSettings().m_bpp;
  block.aspect = output.getAspectMode();
  block.ratio  = output.getAspectRatio();
  block.color  = output.getAspectColor();
  return block;
}

bool sameBlock(const OutputBlock& left, const OutputBlock& right) {
  return left.first == right.first && left.last == right.last &&
         left.step == right.step && left.shrink == right.shrink &&
         left.bpp == right.bpp && left.aspect == right.aspect &&
         left.ratio == right.ratio && left.color == right.color;
}

void readRange(const QJsonObject& settings, OutputBlock& block) {
  if (!settings.contains("range")) return;
  const QJsonObject range = settings.value("range").toObject();
  if (range.value("kind").toString() == "all") {
    block.first = 0;
    block.last  = -1;
    return;
  }
  block.first = range.value("start_frame").toInt();
  block.last  = range.value("end_frame").toInt();
  if (block.first > block.last)
    throw Error("invalid_params", "start_frame must not follow end_frame.",
                {{"reason", "range"}});
}

void readAspect(const QJsonObject& settings, OutputBlock& block) {
  if (!settings.contains("aspect_export")) return;
  const QJsonObject aspect = settings.value("aspect_export").toObject();
  const QString mode       = aspect.value("mode").toString();
  if (mode == "none") {
    block.aspect = TOutputProperties::NoAspectExport;
    return;
  }
  block.aspect = mode == "crop" ? TOutputProperties::AspectCrop
                                : TOutputProperties::AspectLetterbox;
  block.ratio  = aspect.value("ratio").toDouble();
  if (aspect.contains("color"))
    block.color = readColor(aspect.value("color").toObject());
}

OutputBlock nextBlock(const QJsonObject& settings, OutputBlock block) {
  readRange(settings, block);
  block.step   = settings.value("step").toInt(block.step);
  block.shrink = settings.value("shrink").toInt(block.shrink);
  block.bpp    = settings.value("bpp").toInt(block.bpp);
  readAspect(settings, block);
  return block;
}

void writeBlock(TOutputProperties& output, const OutputBlock& block) {
  output.setRange(block.first, block.last, block.step);
  TRenderSettings render = output.getRenderSettings();
  render.m_shrinkX = render.m_shrinkY = block.shrink;
  render.m_bpp                        = block.bpp;
  output.setRenderSettings(render);
  output.setAspectMode(block.aspect);
  output.setAspectRatio(block.ratio);
  output.setAspectColor(block.color);
}

void requireSomething(const QJsonObject& params) {
  if (params.value("settings").toObject().isEmpty() &&
      !params.contains("sync_color_settings") &&
      !params.contains("column_filters_on_render"))
    throw Error("invalid_params",
                "Give settings, sync_color_settings or "
                "column_filters_on_render.",
                {{"reason", "nothing_to_set"}});
}

QJsonValue setOutput(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireSomething(params);
  TSceneProperties& properties = *document.scene->getProperties();
  const QJsonArray applyTo     = params.value("apply_to").toArray();
  const bool toOutput          = applyTo.contains("output");
  const bool toPreview         = applyTo.contains("preview");
  if (applyTo.size() != int(toOutput) + int(toPreview))
    throw Error("invalid_params", "Name each block once.",
                {{"reason", "apply_to"}});
  if (params.contains("sync_color_settings") && !toPreview)
    throw Error("invalid_params",
                "sync_color_settings is the preview block's flag.",
                {{"reason", "sync_color_settings"}});
  const QJsonObject settings      = params.value("settings").toObject();
  TOutputProperties* output       = properties.getOutputProperties();
  TOutputProperties* preview      = properties.getPreviewProperties();
  const OutputBlock outputBefore  = readBlock(*output);
  const OutputBlock previewBefore = readBlock(*preview);
  const OutputBlock outputAfter =
      toOutput ? nextBlock(settings, outputBefore) : outputBefore;
  const OutputBlock previewAfter =
      toPreview ? nextBlock(settings, previewBefore) : previewBefore;
  const bool sync = params.value("sync_color_settings")
                        .toBool(preview->isColorSettingsSynced());
  const bool filters =
      params.value("column_filters_on_render")
          .toBool(properties.isColumnColorFilterOnRenderEnabled());
  QJsonArray changed;
  if (!sameBlock(outputBefore, outputAfter)) changed.append("output");
  if (!sameBlock(previewBefore, previewAfter)) changed.append("preview");
  if (sync != preview->isColorSettingsSynced())
    changed.append("sync_color_settings");
  if (filters != properties.isColumnColorFilterOnRenderEnabled())
    changed.append("column_filters_on_render");
  if (!changed.isEmpty()) {
    context.markMutationStarted();
    writeBlock(*output, outputAfter);
    writeBlock(*preview, previewAfter);
    preview->syncColorSettings(sync);
    properties.enableColumnColorFilterOnRender(filters);
  }
  const int frames = document.scene->getFrameCount();
  return QJsonObject{{"output", outputRecord(*output, frames)},
                     {"preview", outputRecord(*preview, frames)},
                     {"sync_color_settings", preview->isColorSettingsSynced()},
                     {"column_filters_on_render",
                      properties.isColumnColorFilterOnRenderEnabled()},
                     {"changed_fields", changed}};
}

// ------------------------------------------------------------ guides set

TSceneProperties::Guides readGuides(const QJsonArray& inches,
                                    double& worstError) {
  TSceneProperties::Guides units;
  for (const QJsonValue& value : inches) {
    const double inch   = value.toDouble();
    const double stored = inch * Stage::inch;
    const double error  = std::abs(stored / Stage::inch - inch);
    if (error > 1e-9)
      throw Error("invalid_params",
                  "A guide position does not read back within 1e-9 inch.",
                  {{"reason", "guide_precision"}, {"value", inch}});
    worstError = std::max(worstError, error);
    units.push_back(stored);
  }
  return units;
}

QJsonValue setGuides(Context& context, const QJsonObject& params) {
  Document& document           = getDocument(context, params);
  TSceneProperties& properties = *document.scene->getProperties();
  const QJsonObject field      = params.value("field_guide").toObject();
  const QJsonObject markers    = params.value("markers").toObject();
  if (field.isEmpty() && markers.isEmpty() && !params.contains("ruler_guides"))
    throw Error("invalid_params", "Give field_guide, markers or ruler_guides.",
                {{"reason", "nothing_to_set"}});
  const int size = field.value("size").toInt(properties.getFieldGuideSize());
  const double ratio = field.value("aspect_ratio")
                           .toDouble(properties.getFieldGuideAspectRatio());
  const QJsonObject currentMarkers = markersRecord(properties);
  const int interval =
      markers.value("interval").toInt(currentMarkers.value("interval").toInt());
  const int start = markers.value("start_frame")
                        .toInt(currentMarkers.value("start_frame").toInt());
  double worst    = 0;
  TSceneProperties::Guides vertical   = properties.getHGuides();
  TSceneProperties::Guides horizontal = properties.getVGuides();
  if (params.contains("ruler_guides")) {
    const QJsonObject guides = params.value("ruler_guides").toObject();
    vertical = readGuides(guides.value("vertical_lines_x_in").toArray(), worst);
    horizontal =
        readGuides(guides.value("horizontal_lines_y_in").toArray(), worst);
  }
  QJsonArray changed;
  if (size != properties.getFieldGuideSize() ||
      ratio != properties.getFieldGuideAspectRatio())
    changed.append("field_guide");
  if (interval != currentMarkers.value("interval").toInt() ||
      start != currentMarkers.value("start_frame").toInt())
    changed.append("markers");
  if (vertical != properties.getHGuides() ||
      horizontal != properties.getVGuides())
    changed.append("ruler_guides");
  if (!changed.isEmpty()) {
    context.markMutationStarted();
    properties.setFieldGuideSize(size);
    properties.setFieldGuideAspectRatio(ratio);
    properties.setMarkers(interval, start);
    properties.getHGuides() = vertical;
    properties.getVGuides() = horizontal;
  }
  return QJsonObject{{"field_guide", fieldGuideRecord(properties)},
                     {"markers", markersRecord(properties)},
                     {"ruler_guides", guidesRecord(properties)},
                     {"max_canonicalization_error_in", worst},
                     {"changed_fields", changed}};
}

QJsonObject colorSchema() {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"properties", QJsonObject{{"r", integerSchema(0, 255)},
                                     {"g", integerSchema(0, 255)},
                                     {"b", integerSchema(0, 255)},
                                     {"a", integerSchema(0, 255)}}},
          {"required", QJsonArray{"r", "g", "b", "a"}}};
}

QJsonObject objectSchema(const QJsonObject& properties,
                         const QJsonArray& required) {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"properties", properties},
          {"required", required}};
}

QJsonObject outputSettingsSchema() {
  const QJsonObject frames = integerSchema(0, 999999);
  const QJsonObject range{
      {"oneOf",
       QJsonArray{
           objectSchema({{"kind", QJsonObject{{"const", "all"}}}}, {"kind"}),
           objectSchema({{"kind", QJsonObject{{"const", "frames"}}},
                         {"start_frame", frames},
                         {"end_frame", frames}},
                        {"kind", "start_frame", "end_frame"})}}};
  const QJsonObject aspect{
      {"oneOf",
       QJsonArray{
           objectSchema({{"mode", QJsonObject{{"const", "none"}}}}, {"mode"}),
           objectSchema({{"mode", enumSchema({"letterbox", "crop"})},
                         {"ratio", numberSchema(0.1, 10)},
                         {"color", colorSchema()}},
                        {"mode", "ratio"})}}};
  return objectSchema(
      {{"range", range},
       {"step", integerSchema(1, 1000000)},
       {"shrink", integerSchema(1, 64)},
       {"bpp", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{32, 64}}}},
       {"aspect_export", aspect}},
      {});
}

}  // namespace

QJsonObject backgroundColorSchema() { return colorSchema(); }

void applyBackgroundColor(ToonzScene& scene, const QJsonObject& params) {
  if (params.contains("background_color"))
    scene.getProperties()->setBgColor(
        readColor(params.value("background_color").toObject()));
}

bool sameBackgroundColor(ToonzScene& scene, const QJsonObject& params) {
  return !params.contains("background_color") ||
         readColor(params.value("background_color").toObject()) ==
             scene.getProperties()->getBgColor();
}

void addOutputOperations(QJsonArray& catalog) {
  catalog.append(operation(
      "scene.properties.inspect",
      "Inspect the scene's output and preview settings, background, guides, "
      "markers and color filters, with where each setting is stored and the "
      "tool that edits it.",
      merge(documentSchema(), {{"expected_revision", textSchema()}}),
      {"document_id", "expected_revision"}));
  catalog.append(operation(
      "scene.output.set",
      "Set the stored output or preview settings named in apply_to: range, "
      "step, shrink, bpp and aspect export. Worker renders take their own "
      "explicit settings and do not read these.",
      merge(documentSchema(),
            {{"apply_to",
              QJsonObject{{"type", "array"},
                          {"items", enumSchema({"output", "preview"})},
                          {"minItems", 1},
                          {"maxItems", 2}}},
             {"settings", outputSettingsSchema()},
             {"sync_color_settings", QJsonObject{{"type", "boolean"}}},
             {"column_filters_on_render", QJsonObject{{"type", "boolean"}}}}),
      {"document_id", "apply_to"}, true));
  const QJsonObject positions{{"type", "array"},
                              {"items", numberSchema(-1000, 1000)},
                              {"minItems", 0},
                              {"maxItems", MaxGuides}};
  catalog.append(operation(
      "scene.guides.set",
      "Set the field guide, xsheet markers or the ruler guides (positions in "
      "inches; each list replaces the stored one). Renders do not change.",
      merge(
          documentSchema(),
          {{"field_guide",
            objectSchema({{"size", integerSchema(1, 100)},
                          {"aspect_ratio", numberSchema(0.01, 100)}},
                         {})},
           {"markers", objectSchema({{"interval", integerSchema(0, 1000)},
                                     {"start_frame", integerSchema(0, 999999)}},
                                    {})},
           {"ruler_guides",
            objectSchema({{"vertical_lines_x_in", positions},
                          {"horizontal_lines_y_in", positions}},
                         {"vertical_lines_x_in", "horizontal_lines_y_in"})}}),
      {"document_id"}, true));
}

void addOutputHandlers(SceneHandlers& handlers) {
  handlers.insert({{"scene.properties.inspect", inspectProperties},
                   {"scene.output.set", setOutput},
                   {"scene.guides.set", setGuides}});
}

}  // namespace SceneShared
}  // namespace GroundzeroAutomation
