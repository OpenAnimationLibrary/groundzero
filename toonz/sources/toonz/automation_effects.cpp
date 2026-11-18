#include "automation_effects.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>

namespace GroundzeroAutomation {
namespace {

constexpr int maximumErrors = 64;

struct Declaration {
  ToolEffects effects;
  QStringList errors;
};

const ToolEffects kInspect{"read", "reads", {}, "none", {}, false};
const ToolEffects kInspectWorker{"read", "none", {}, "none", {}, false};
const ToolEffects kJobRead{"read", "none", {}, "reads", {}, false};
const ToolEffects kListShared{"read", "none", {"reads_shared_roots"},
                              "none", {},     false};
const ToolEffects kParseShared{"read", "none",           {"reads_shared_roots"},
                               "none", {"parse_helper"}, false};
const ToolEffects kEdit{"document_write", "writes", {}, "none", {}, true};
const ToolEffects kAdd{"document_write", "writes", {}, "none", {}, false};
const ToolEffects kEditFromFile{"document_write", "writes", {"reads_roots"},
                                "none",           {},       true};
const ToolEffects kImport{
    "document_write", "writes", {"reads_roots", "writes_private"},
    "none",           {},       false};
const ToolEffects kExport{"document_write", "reads", {"writes_caller_path"},
                          "none",           {},      false};

// One row per native tool. Its effects say what the tool can change; its
// errors are the codes it returns beyond those common to its class and inputs
// (see commonErrors). A tool registered without a row stops the worker.
const QHash<QString, Declaration>& declarations() {
  static const QHash<QString, Declaration> table{
      {"animation.channels", {kInspect, {}}},
      {"animation.copy_curve",
       {kEdit, {"invalid_handle", "locked", "unsupported_interpolation"}}},
      {"animation.delete_keys", {kEdit, {"invalid_argument", "missing_key"}}},
      {"animation.keys", {kInspect, {}}},
      {"animation.retime_curve", {kEdit, {"locked"}}},
      {"animation.sample", {kInspect, {"invalid_argument", "invalid_handle"}}},
      {"animation.set_keys",
       {kEdit,
        {"invalid_argument", "invalid_unit", "locked", "unsupported_channel"}}},
      {"artifact.read",
       {ToolEffects{"read", "none", {"reads_private"}, "reads", {}, false},
        {"invalid_artifact", "unknown_artifact", "unsupported_artifact_kind"}}},
      {"assistance.inspect",
       {ToolEffects{"read", "none", {"reads_application"}, "none", {}, false},
        {}}},
      {"assistance.neural.preview",
       {ToolEffects{"job_start",
                    "reads",
                    {"writes_private"},
                    "starts",
                    {"neural_helper"},
                    false},
        {}}},
      {"audio.clip.place",
       {kEdit, {"audio_overlap", "incompatible_column", "timing_conflict"}}},
      {"audio.clip.remove", {kEdit, {"locked", "not_found"}}},
      {"audio.clip.update", {kEdit, {}}},
      {"audio.column.create", {kAdd, {}}},
      {"audio.column.set_volume", {kEdit, {"locked"}}},
      {"audio.import", {kImport, {"invalid_asset", "unsupported_format"}}},
      {"audio.inspect", {kInspect, {"revision_conflict"}}},
      {"audio.mixdown",
       {kExport, {"already_exists", "missing_parent", "timing_conflict"}}},
      {"automation.capabilities", {kInspectWorker, {}}},
      {"automation.list_documents", {kInspectWorker, {}}},
      {"camera.inspect", {kInspect, {}}},
      {"camera.update", {kEdit, {}}},
      {"cast.list", {kInspect, {"stale_revision"}}},
      {"cast.usage", {kInspect, {"stale_revision"}}},
      {"column.cells", {kInspect, {"incompatible_column"}}},
      {"column.create", {kAdd, {}}},
      {"column.delete", {kEdit, {"has_dependents", "locked"}}},
      {"column.expose",
       {kEdit, {"incompatible_column", "invalid_handle", "locked"}}},
      {"column.update", {kEdit, {"invalid_handle", "locked"}}},
      {"drawing.clone", {kAdd, {"unsupported_drawing_id"}}},
      {"drawing.create", {kAdd, {}}},
      {"drawing.inspect", {kInspect, {"not_found"}}},
      {"drawing.remove", {kEdit, {"has_dependents", "in_use"}}},
      {"drawing.renumber",
       {kEdit,
        {"already_exists", "invalid_handle", "locked", "not_found",
         "unsupported_drawing_id"}}},
      {"fx.catalog", {kInspectWorker, {}}},
      {"fx.connect", {kEdit, {"GRAPH_CYCLE"}}},
      {"fx.create", {kAdd, {}}},
      {"fx.delete", {kEdit, {"UNSUPPORTED_FX"}}},
      {"fx.disconnect", {kEdit, {}}},
      {"fx.duplicate", {kAdd, {"INVALID_PARAMS", "UNSUPPORTED_FX", "locked"}}},
      {"fx.enabled.set", {kEdit, {"UNSUPPORTED_FX", "locked"}}},
      {"fx.graph", {kInspect, {}}},
      {"fx.macro.create", {kEdit, {"INVALID_MACRO", "invalid_handle"}}},
      {"fx.macro.expand", {kEdit, {"INVALID_MACRO"}}},
      {"fx.output.select", {kEdit, {}}},
      {"fx.parameter.get",
       {kInspect, {"UNSUPPORTED_PARAMETER", "invalid_handle"}}},
      {"fx.parameter.set",
       {kEdit,
        {"INVALID_PARAMS", "OUT_OF_RANGE", "PARAMETER_CONSTRAINT",
         "UNKNOWN_PARAMETER", "UNSUPPORTED_FX"}}},
      {"fx.parameter.set_key", {kEdit, {"OUT_OF_RANGE", "UNKNOWN_PARAMETER"}}},
      {"fx.preset.apply",
       {kEditFromFile,
        {"INCOMPATIBLE_PRESET", "INVALID_PRESET", "PRESET_READ_FAILED",
         "RESOURCE_LIMIT", "UNREGISTERED_PRESET"}}},
      {"fx.preset.save",
       {kExport,
        {"ALREADY_EXISTS", "INVALID_PARAMS", "MISSING_PARENT",
         "PRESET_WRITE_FAILED", "RESOURCE_LIMIT"}}},
      {"fx.recipe.list", {kInspectWorker, {}}},
      {"fx.rename",
       {kEdit,
        {"INVALID_PARAMS", "UNSUPPORTED_FX", "invalid_handle", "locked"}}},
      {"fx.terminal.set", {kEdit, {}}},
      {"hook.delete",
       {kEdit, {"has_dependents", "locked", "missing_key", "not_found"}}},
      {"hook.inspect", {kInspect, {}}},
      {"hook.set", {kEdit, {"locked", "missing_drawing"}}},
      {"level.create", {kAdd, {}}},
      {"level.import", {kImport, {"invalid_asset", "unsupported_precision"}}},
      {"level.inspect", {kInspect, {}}},
      {"library.inspect", {kParseShared, {"invalid_library_item", "io_error"}}},
      {"library.list",
       {kListShared,
        {"resource_limit", "revision_conflict", "root_unavailable"}}},
      {"library.thumbnail",
       {kParseShared, {"asset_unavailable", "not_found", "resource_limit"}}},
      {"lipsync.apply",
       {kEdit,
        {"incompatible_column", "invalid_handle", "locked", "missing_drawing",
         "stale_analysis", "timing_conflict", "unsupported_level"}}},
      {"lipsync.import", {kEditFromFile, {"invalid_asset"}}},
      {"lipsync.inspect", {kInspect, {}}},
      {"motion_path.create", {kAdd, {}}},
      {"motion_path.delete", {kEdit, {}}},
      {"motion_path.duplicate", {kAdd, {}}},
      {"motion_path.inspect",
       {kInspect, {"invalid_handle", "revision_conflict"}}},
      {"motion_path.list", {kInspect, {}}},
      {"motion_path.rename", {kEdit, {}}},
      {"motion_path.update", {kEdit, {"locked"}}},
      {"object.create", {kAdd, {}}},
      {"object.handles.set", {kEdit, {}}},
      {"object.parent", {kEdit, {"cycle", "invalid_parent"}}},
      {"object.path.attach", {kEdit, {}}},
      {"object.path.detach", {kEdit, {}}},
      {"object.pivot.set", {kEdit, {}}},
      {"offset_layer.create", {kAdd, {"name_conflict", "unsupported_target"}}},
      {"offset_layer.delete", {kEdit, {}}},
      {"offset_layer.update", {kEdit, {"invalid_handle", "name_conflict"}}},
      {"palette.clone_style", {kAdd, {}}},
      {"palette.create_style", {kAdd, {}}},
      {"palette.inspect", {kInspect, {}}},
      {"palette.page.create", {kAdd, {}}},
      {"palette.page.move", {kEdit, {}}},
      {"palette.page.remove", {kEdit, {"page_not_empty"}}},
      {"palette.page.rename", {kEdit, {}}},
      {"palette.pages", {kInspect, {}}},
      {"palette.rename_style", {kEdit, {}}},
      {"palette.style.move", {kEdit, {}}},
      {"palette.update_style", {kEdit, {}}},
      {"project.scenes",
       {ToolEffects{"read", "none", {"reads_roots"}, "none", {}, false},
        {"invalid_path", "invalid_project", "io_error", "not_found",
         "path_denied", "resource_limit", "revision_conflict"}}},
      {"project.list",
       {ToolEffects{"read", "none", {"reads_roots"}, "none", {}, false},
        {"invalid_path", "io_error", "not_found", "path_denied",
         "revision_conflict"}}},
      {"project.inspect", {kInspectWorker, {}}},
      {"raster.export_png", {kExport, {"already_exists", "asset_unavailable"}}},
      {"raster.import_png", {kEditFromFile, {"unsupported_precision"}}},
      {"raster.patch", {kEdit, {}}},
      {"raster.read_pixels", {kInspect, {}}},
      {"render.artifacts", {kJobRead, {"artifacts_unavailable"}}},
      {"render.cancel",
       {ToolEffects{"job_control", "none", {}, "controls", {}, true}, {}}},
      {"render.capabilities", {kInspectWorker, {}}},
      {"render.preview",
       {ToolEffects{
            "job_start", "reads", {"writes_private"}, "starts", {}, false},
        {"fx_preflight_failed"}}},
      {"render.status", {kJobRead, {}}},
      {"render.submit",
       {ToolEffects{"job_start",
                    "reads",
                    {"writes_private"},
                    "starts",
                    {"media_tools"},
                    false},
        {"fx_preflight_failed", "render_busy", "render_unavailable",
         "unsupported_render_settings"}}},
      {"rig.evaluate", {kInspect, {}}},
      {"rig.inspect", {kInspect, {}}},
      {"scene.create",
       {ToolEffects{"document_lifecycle", "creates", {}, "none", {}, false},
        {}}},
      {"scene.dependencies",
       {ToolEffects{"read", "reads", {"reads_roots"}, "none", {}, false},
        {"stale_revision"}}},
      {"scene.discard",
       {ToolEffects{"document_lifecycle", "discards", {}, "none", {}, true},
        {}}},
      {"scene.open",
       {ToolEffects{"document_lifecycle",
                    "creates",
                    {"reads_roots"},
                    "none",
                    {},
                    false},
        {"invalid_package", "missing_drawing", "package_limit",
         "package_read_failed", "reader_unavailable", "unregistered_package",
         "unsupported_level"}}},
      {"scene.inventory",
       {kInspect, {"resource_limit", "scene_host_failed", "stale_revision"}}},
      {"scene.open_read_only",
       {ToolEffects{"document_lifecycle", "opens_read_only", {"reads_roots"},
                    "none", {"scene_host"}, false},
        {"invalid_project", "invalid_scene", "io_error", "not_found",
         "project_mismatch", "scene_limit", "source_changed",
         "unsupported_type"}}},
      {"scene.package_info",
       {ToolEffects{"read", "none", {"reads_roots"}, "none", {}, false},
        {"invalid_package", "package_limit", "package_read_failed",
         "unregistered_package"}}},
      {"scene.save_copy",
       {kExport,
        {"already_exists", "empty_level", "invalid_package", "missing_drawing",
         "missing_level", "missing_parent", "package_limit",
         "package_write_failed", "reader_unavailable", "scene_limit",
         "unsupported_column", "unsupported_level", "writer_unavailable"}}},
      {"scene.settings", {kEdit, {"timing_conflict"}}},
      {"scene.source_status",
       {ToolEffects{"read", "reads", {"reads_roots"}, "none", {}, false},
        {"resource_limit", "scene_host_failed", "stale_revision",
         "unsupported_target"}}},
      {"scene.summary", {kInspect, {}}},
      {"scene.validate", {kInspect, {"stale_revision"}}},
      {"shared.roots", {kListShared, {}}},
      {"storyboard.panel.create", {kAdd, {"already_exists"}}},
      {"storyboard.panel.delete", {kEdit, {"not_found"}}},
      {"storyboard.panel.update", {kEdit, {"already_exists", "not_found"}}},
      {"storyboard.panels", {kInspect, {}}},
      {"studio_palette.find", {kParseShared, {}}},
      {"studio_palette.inspect",
       {kParseShared, {"revision_conflict", "unsupported_palette"}}},
      {"studio_palette.list", {kListShared, {}}},
      {"text.fonts", {kInspectWorker, {}}},
      {"vector.edit", {kEdit, {}}},
      {"vector.fill", {kEdit, {"not_found"}}},
      {"vector.group.create",
       {kEdit, {"not_found", "unsupported_grouping", "unsupported_type"}}},
      {"vector.group.remove", {kEdit, {"not_found"}}},
      {"vector.inspect", {kInspect, {}}},
      {"vector.opacity.inspect", {kInspect, {}}},
      {"vector.opacity.profile.set", {kEdit, {}}},
      {"vector.opacity.set", {kEdit, {}}},
      {"vector.order.move", {kEdit, {"unsupported_grouping"}}},
      {"vector.select", {kInspect, {"resource_limit", "revision_conflict"}}},
      {"vector.structure", {kInspect, {}}},
      {"vector.text.create",
       {kAdd,
        {"already_exists", "font_not_found", "missing_glyph",
         "unsupported_outline"}}},
      {"vector.transform",
       {kEdit,
        {"unsupported_geometry", "unsupported_transform", "unsupported_type"}}},
  };
  return table;
}

// Tools whose results have no published outputSchema yet. A tool registered
// without an outputSchema must be listed here, and a listed tool must not have
// one, so each schema that lands removes its name.
const QSet<QString>& pendingOutputSchemas() {
  static const QSet<QString> pending{
      "animation.channels",
      "animation.copy_curve",
      "animation.delete_keys",
      "animation.keys",
      "animation.retime_curve",
      "animation.sample",
      "animation.set_keys",
      "artifact.read",
      "assistance.inspect",
      "assistance.neural.preview",
      "audio.clip.place",
      "audio.clip.remove",
      "audio.clip.update",
      "audio.column.create",
      "audio.column.set_volume",
      "audio.import",
      "audio.inspect",
      "audio.mixdown",
      "camera.inspect",
      "camera.update",
      "cast.list",
      "cast.usage",
      "column.cells",
      "column.create",
      "column.delete",
      "column.expose",
      "column.update",
      "drawing.clone",
      "drawing.create",
      "drawing.inspect",
      "drawing.remove",
      "drawing.renumber",
      "fx.catalog",
      "fx.connect",
      "fx.create",
      "fx.delete",
      "fx.disconnect",
      "fx.duplicate",
      "fx.enabled.set",
      "fx.graph",
      "fx.macro.create",
      "fx.macro.expand",
      "fx.output.select",
      "fx.parameter.get",
      "fx.parameter.set",
      "fx.parameter.set_key",
      "fx.preset.apply",
      "fx.preset.save",
      "fx.recipe.list",
      "fx.rename",
      "fx.terminal.set",
      "hook.delete",
      "hook.inspect",
      "hook.set",
      "level.create",
      "level.import",
      "level.inspect",
      "library.inspect",
      "library.list",
      "library.thumbnail",
      "lipsync.apply",
      "lipsync.import",
      "lipsync.inspect",
      "motion_path.create",
      "motion_path.delete",
      "motion_path.duplicate",
      "motion_path.inspect",
      "motion_path.list",
      "motion_path.rename",
      "motion_path.update",
      "object.create",
      "object.handles.set",
      "object.parent",
      "object.path.attach",
      "object.path.detach",
      "object.pivot.set",
      "offset_layer.create",
      "offset_layer.delete",
      "offset_layer.update",
      "palette.clone_style",
      "palette.create_style",
      "palette.inspect",
      "palette.page.create",
      "palette.page.move",
      "palette.page.remove",
      "palette.page.rename",
      "palette.pages",
      "palette.rename_style",
      "palette.style.move",
      "palette.update_style",
      "project.inspect",
      "project.list",
      "raster.export_png",
      "raster.import_png",
      "raster.patch",
      "raster.read_pixels",
      "render.artifacts",
      "render.cancel",
      "render.capabilities",
      "render.preview",
      "render.status",
      "render.submit",
      "rig.evaluate",
      "rig.inspect",
      "scene.create",
      "scene.dependencies",
      "scene.discard",
      "scene.open",
      "scene.open_read_only",
      "scene.package_info",
      "scene.save_copy",
      "scene.settings",
      "scene.summary",
      "scene.validate",
      "shared.roots",
      "storyboard.panel.create",
      "storyboard.panel.delete",
      "storyboard.panel.update",
      "storyboard.panels",
      "studio_palette.find",
      "studio_palette.inspect",
      "studio_palette.list",
      "text.fonts",
      "vector.edit",
      "vector.fill",
      "vector.group.create",
      "vector.group.remove",
      "vector.inspect",
      "vector.opacity.inspect",
      "vector.opacity.profile.set",
      "vector.opacity.set",
      "vector.order.move",
      "vector.select",
      "vector.structure",
      "vector.text.create",
      "vector.transform",
  };
  return pending;
}

bool isRead(const ToolEffects& effects) { return effects.kind == "read"; }

bool isWrite(const ToolEffects& effects) {
  return effects.kind != "read" && effects.kind != "session";
}

QJsonArray grants(const ToolEffects& effects) {
  QJsonArray result{"inspect"};
  static const QSet<QString> drafting{"writes", "creates", "discards"};
  if (drafting.contains(effects.document)) result.append("draft");
  if (effects.files.contains("writes_caller_path") ||
      effects.files.contains("writes_shared_roots"))
    result.append("file");
  if (effects.job == "starts" || effects.job == "controls")
    result.append("job");
  return result;
}

QJsonObject effectsObject(const ToolEffects& effects) {
  const bool writes = isWrite(effects);
  const bool keepsRevision =
      effects.kind == "document_write" && effects.document == "writes";
  return {
      {"class", effects.kind},
      {"document", effects.document},
      {"files", QJsonArray::fromStringList(effects.files)},
      {"job", effects.job},
      {"processes", QJsonArray::fromStringList(effects.processes)},
      {"overwrites", effects.overwrites},
      {"undo", "none"},
      {"no_op", keepsRevision ? "revision_kept" : "not_applicable"},
      {"after_change_failure", writes ? "partially_applied" : "not_applicable"},
      {"rollback", "never_claimed"},
      {"retry", writes ? "receipt_replay" : "repeat_safe"},
      {"grants", grants(effects)}};
}

QStringList commonErrors(const ToolEffects& effects,
                         const QJsonObject& inputs) {
  QStringList result{"invalid_params", "method_not_found", "worker_busy",
                     "native_error", "response_too_large"};
  if (inputs.contains("document_id")) result.append("document_not_found");
  if (inputs.contains("job_id")) result.append("unknown_job");
  if (inputs.contains("path") || inputs.contains("directory") ||
      inputs.contains("output_path"))
    result << "path_denied" << "invalid_path";
  if (!isWrite(effects)) return result;
  result << "operation_conflict" << "partially_applied" << "resource_limit";
  if (inputs.contains("expected_revision")) result.append("revision_conflict");
  return result;
}

[[noreturn]] void undeclared(const QString& name, const QString& problem) {
  throw Error("internal_error",
              "Tool " + name + " " + problem +
                  " in toolDeclarations (automation_effects.cpp).");
}

void requireConsistent(const QString& name, const ToolEffects& effects,
                       const QJsonObject& inputSchema) {
  const bool receipts = inputSchema.value("required")
                            .toArray()
                            .contains(QJsonValue("operation_id"));
  if (isRead(effects) == receipts)
    undeclared(name, receipts
                         ? "requires operation_id, so it needs a writing class"
                         : "takes no operation_id, so it needs class read");
}

}  // namespace

QJsonObject declaredTool(QJsonObject descriptor, const ToolEffects& effects,
                         const QStringList& errors) {
  const QString name = descriptor.value("name").toString();
  const QJsonObject inputs =
      descriptor.value("inputSchema").toObject().value("properties").toObject();
  requireConsistent(name, effects, descriptor.value("inputSchema").toObject());
  QStringList codes = commonErrors(effects, inputs);
  for (const QString& code : errors)
    if (!codes.contains(code)) codes.append(code);
  if (codes.size() > maximumErrors)
    undeclared(name, "declares more than 64 error codes; trim its row");
  descriptor.remove("mutates_document");
  descriptor.insert("effects", effectsObject(effects));
  descriptor.insert("errors", QJsonArray::fromStringList(codes));
  descriptor.insert(
      "annotations",
      QJsonObject{{"readOnlyHint", isRead(effects)},
                  {"destructiveHint", effects.overwrites && !isRead(effects)},
                  {"idempotentHint", true},
                  {"openWorldHint", false}});
  return descriptor;
}

QJsonObject declaredTool(const QJsonObject& descriptor) {
  const QString name = descriptor.value("name").toString();
  const auto found   = declarations().constFind(name);
  if (found == declarations().constEnd())
    undeclared(name, "has no effect declaration: add a row for it");
  const bool pending = pendingOutputSchemas().contains(name);
  if (descriptor.contains("outputSchema") == pending)
    throw Error("internal_error",
                "Tool " + name +
                    (pending ? " has an outputSchema: remove it from "
                               "pendingOutputSchemas"
                             : " has no outputSchema: give its descriptor one "
                               "with withOutputSchema") +
                    " (automation_effects.cpp).");
  return declaredTool(descriptor, found->effects, found->errors);
}

QStringList declaredToolNames() { return declarations().keys(); }

bool advancesRevision(const QJsonObject& descriptor) {
  static const QSet<QString> advancing{"writes", "creates", "discards"};
  return advancing.contains(
      descriptor.value("effects").toObject().value("document").toString());
}

QJsonObject errorEnvelopeSchema() {
  const QJsonObject error{
      {"type", "object"},
      {"additionalProperties", false},
      {"required", QJsonArray{"code", "message", "details"}},
      {"properties",
       QJsonObject{{"code", QJsonObject{{"type", "string"},
                                        {"minLength", 1},
                                        {"maxLength", 64}}},
                   {"message", QJsonObject{{"type", "string"},
                                           {"minLength", 0},
                                           {"maxLength", 2048}}},
                   {"details", QJsonObject{{"type", "object"}}}}}};
  return {{"type", "object"},
          {"additionalProperties", false},
          {"required", QJsonArray{"error"}},
          {"properties", QJsonObject{{"error", error}}}};
}

namespace {

QJsonObject stringSchema(int minimum, int maximum) {
  return {{"type", "string"}, {"minLength", minimum}, {"maxLength", maximum}};
}

QJsonObject enumSchema(const QStringList& values) {
  return {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}};
}

QJsonObject arraySchema(const QJsonObject& items, int minimum, int maximum) {
  return {{"type", "array"},
          {"items", items},
          {"minItems", minimum},
          {"maxItems", maximum}};
}

QJsonObject strictObject(const QJsonObject& properties,
                         const QStringList& required) {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"properties", properties},
          {"required", QJsonArray::fromStringList(required)}};
}

QJsonObject strictObject(const QJsonObject& properties) {
  return strictObject(properties, properties.keys());
}

QJsonObject effectsSchema() {
  const QStringList files{"reads_roots",        "reads_shared_roots",
                          "reads_application",  "reads_private",
                          "writes_private",     "writes_caller_path",
                          "writes_shared_roots"};
  const QStringList processes{"parse_helper", "media_tools", "neural_helper",
                              "scene_host"};
  const QStringList grants{"inspect", "draft",        "file",
                           "job",     "live_inspect", "live_edit"};
  return strictObject(
      {{"class", enumSchema({"read", "document_write", "document_lifecycle",
                             "job_start", "job_control", "session"})},
       {"document", enumSchema({"none", "reads", "writes", "creates",
                                "opens_read_only", "discards"})},
       {"files", arraySchema(enumSchema(files), 0, 7)},
       {"job", enumSchema({"none", "starts", "controls", "reads"})},
       {"processes", arraySchema(enumSchema(processes), 0, 4)},
       {"overwrites", QJsonObject{{"type", "boolean"}}},
       {"undo", enumSchema({"none", "one_native_undo_entry"})},
       {"no_op", enumSchema({"revision_kept", "not_applicable"})},
       {"after_change_failure",
        enumSchema({"partially_applied", "not_applicable"})},
       {"rollback",
        QJsonObject{{"type", "string"}, {"const", "never_claimed"}}},
       {"retry", enumSchema({"receipt_replay", "repeat_safe"})},
       {"grants", arraySchema(enumSchema(grants), 1, 4)}});
}

QJsonObject descriptorSchema() {
  const QJsonObject boolean{{"type", "boolean"}};
  const QJsonObject object{{"type", "object"}};
  QStringList required{"name",        "description", "toolset", "inputSchema",
                       "annotations", "effects",     "errors"};
  return strictObject(
      {{"name", stringSchema(1, 128)},
       {"description", stringSchema(1, 8192)},
       {"toolset", stringSchema(1, 32)},
       {"inputSchema", object},
       {"outputSchema", object},
       {"annotations", strictObject({{"readOnlyHint", boolean},
                                     {"destructiveHint", boolean},
                                     {"idempotentHint", boolean},
                                     {"openWorldHint", boolean}})},
       {"effects", effectsSchema()},
       {"errors", arraySchema(stringSchema(1, 64), 1, maximumErrors)}},
      required);
}

QJsonObject readOnlyScenesSchema() {
  const QJsonObject count{
      {"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991.0}};
  return strictObject({{"available", QJsonObject{{"type", "boolean"}}},
                       {"max_documents", count},
                       {"host_memory_bytes", count},
                       {"open_deadline_ms", count},
                       {"read_deadline_ms", count},
                       {"profile", stringSchema(1, 64)}});
}

}  // namespace

QJsonObject capabilitiesResultSchema() {
  const QJsonObject toolset =
      strictObject({{"name", stringSchema(1, 32)},
                    {"description", stringSchema(1, 1024)},
                    {"tools", arraySchema(stringSchema(1, 128), 0, 1024)}});
  return strictObject(
      {{"protocol_version", QJsonObject{{"type", "integer"}, {"const", 2}}},
       {"workspace", stringSchema(1, 4096)},
       {"tools", arraySchema(descriptorSchema(), 0, 1024)},
       {"toolsets", arraySchema(toolset, 1, 64)},
       {"media_tools_configured", QJsonObject{{"type", "boolean"}}},
       {"isolation",
        QJsonObject{{"type", "string"}, {"const", "private_process"}}},
       {"read_only_scenes", readOnlyScenesSchema()}});
}

QJsonObject documentListResultSchema() {
  const QJsonObject document = strictObject(
      {{"document_id", stringSchema(1, 128)},
       {"revision", stringSchema(1, 80)},
       {"access", enumSchema({"editable", "read_only"})},
       {"host_state",
        QJsonObject{{"oneOf", QJsonArray{enumSchema({"ready", "lost"}),
                                         QJsonObject{{"type", "null"}}}}}}});
  return strictObject({{"documents", arraySchema(document, 0, 256)}});
}

QJsonObject outputSchema(const QJsonObject& success) {
  return {{"type", "object"},
          {"oneOf", QJsonArray{success, errorEnvelopeSchema()}}};
}

QJsonObject withOutputSchema(QJsonObject descriptor,
                             const QJsonObject& success) {
  descriptor.insert("outputSchema", outputSchema(success));
  return descriptor;
}

}  // namespace GroundzeroAutomation
