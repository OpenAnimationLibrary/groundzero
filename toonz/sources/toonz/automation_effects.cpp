#include "automation_effects.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace GroundzeroAutomation {
namespace {

constexpr int maximumErrors = 64;
// The adapter appends its own codes, at most 8, to a described tool's list.
constexpr int maximumNativeErrors = maximumErrors - 8;

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
// Writes that can change the FX graph (see ToolEffects::fxGraph).
const ToolEffects kFxEdit{
    "document_write", "writes", {}, "none", {}, true, true};
const ToolEffects kFxAdd{
    "document_write", "writes", {}, "none", {}, false, true};
const ToolEffects kFxEditFromFile{
    "document_write", "writes", {"reads_roots"}, "none", {}, true, true};
const ToolEffects kImport{
    "document_write", "writes", {"reads_roots", "writes_private"},
    "none",           {},       false};
const ToolEffects kExport{"document_write", "reads", {"writes_caller_path"},
                          "none",           {},      false};

// The animation key writes also address effect channels (fx_id), which
// answer the FX profile's refusals.
QStringList withFxKeyErrors(QStringList errors) {
  errors << "INVALID_PARAMS" << "NATIVE_FAILURE" << "OUT_OF_RANGE"
         << "PARAMETER_CONSTRAINT" << "RESOURCE_LIMIT" << "STALE_ENTITY"
         << "UNSUPPORTED_FX" << "UNSUPPORTED_PARAMETER" << "unsupported_target";
  return errors;
}

// One row per native tool. Its effects say what the tool can change; its
// errors are the codes it returns beyond those common to its class and inputs
// (see commonErrors). A tool registered without a row stops the worker.
const QHash<QString, Declaration>& declarations() {
  static const QHash<QString, Declaration> table{
      {"animation.channels",
       {kInspect,
        {"resource_limit", "unsupported_channel", "unsupported_target"}}},
      {"animation.copy_curve",
       {kFxEdit,
        withFxKeyErrors({"invalid_handle", "invalid_unit", "locked",
                         "unsupported_channel", "unsupported_interpolation"})}},
      {"animation.delete_keys",
       {kFxEdit, withFxKeyErrors({"invalid_argument", "invalid_unit", "locked",
                                  "missing_key", "unsupported_channel",
                                  "unsupported_interpolation"})}},
      {"animation.duplicate_keys",
       {kFxEdit, withFxKeyErrors({"invalid_argument", "invalid_unit", "locked",
                                  "missing_key", "unsupported_channel",
                                  "unsupported_interpolation"})}},
      {"animation.insert_breakdown",
       {kFxEdit,
        withFxKeyErrors({"invalid_argument", "invalid_unit", "locked",
                         "unsupported_channel", "unsupported_interpolation"})}},
      {"animation.keys",
       {kInspect,
        {"invalid_unit", "resource_limit", "unsupported_channel",
         "unsupported_target"}}},
      {"animation.move_keys",
       {kFxEdit, withFxKeyErrors({"invalid_argument", "invalid_unit", "locked",
                                  "missing_key", "unsupported_channel",
                                  "unsupported_interpolation"})}},
      {"animation.retime_curve",
       {kFxEdit,
        withFxKeyErrors({"invalid_unit", "locked", "unsupported_channel",
                         "unsupported_interpolation"})}},
      {"animation.sample",
       {kInspect,
        {"invalid_argument", "invalid_unit", "resource_limit",
         "unsupported_channel", "unsupported_interpolation",
         "unsupported_target"}}},
      {"animation.set_cycle",
       {kEdit,
        {"invalid_argument", "invalid_unit", "locked", "unsupported_channel",
         "unsupported_interpolation"}}},
      {"animation.set_keys",
       {kFxEdit,
        withFxKeyErrors({"invalid_argument", "invalid_unit", "locked",
                         "unsupported_channel", "unsupported_interpolation"})}},
      {"artifact.read",
       {ToolEffects{"read", "none", {"reads_private"}, "reads", {}, false},
        {"invalid_artifact", "path_denied", "unknown_artifact",
         "unsupported_artifact_kind"}}},
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
        {"component_unavailable", "deadline_exceeded", "fx_preflight_failed",
         "invalid_handle", "reference_capture_failed", "render_busy",
         "render_output_failed", "unknown_drawing", "unsafe_component_location",
         "unsupported_containment", "unsupported_reference",
         "unsupported_render_settings"}}},
      {"audio.clip.place",
       {kEdit, {"audio_overlap", "incompatible_column", "timing_conflict"}}},
      {"audio.clip.remove", {kEdit, {"locked", "not_found"}}},
      {"audio.clip.update", {kEdit, {"not_found"}}},
      {"audio.column.create", {kFxAdd, {}}},
      {"audio.column.set_volume", {kEdit, {"locked"}}},
      {"audio.import",
       {kImport, {"invalid_asset", "name_conflict", "unsupported_format"}}},
      {"audio.inspect", {kInspect, {"revision_conflict"}}},
      {"audio.mixdown",
       {kExport,
        {"already_exists", "io_error", "missing_parent",
         "publication_unsupported", "timing_conflict"}}},
      {"automation.capabilities", {kInspectWorker, {}}},
      {"automation.list_documents", {kInspectWorker, {}}},
      {"camera.create", {kAdd, {"name_conflict", "unsupported_target"}}},
      {"camera.duplicate", {kAdd, {"name_conflict", "unsupported_target"}}},
      {"camera.inspect", {kInspect, {"unsupported_target"}}},
      {"camera.list", {kInspect, {"stale_revision", "unsupported_target"}}},
      {"camera.remove",
       {kEdit, {"has_dependents", "in_use", "locked", "unsupported_target"}}},
      {"camera.select", {kEdit, {"unsupported_target"}}},
      {"camera.update",
       {kEdit, {"locked", "name_conflict", "unsupported_target"}}},
      {"cells.clear",
       {kEdit,
        {"incompatible_column", "locked", "scene_limit",
         "unsupported_target"}}},
      {"cells.copy",
       {kEdit,
        {"incompatible_column", "locked", "scene_limit", "unsupported_level",
         "unsupported_target"}}},
      {"cells.insert",
       {kEdit,
        {"incompatible_column", "locked", "scene_limit",
         "unsupported_target"}}},
      {"cells.remove",
       {kEdit,
        {"incompatible_column", "locked", "scene_limit",
         "unsupported_target"}}},
      {"cells.retime",
       {kEdit,
        {"incompatible_column", "locked", "scene_limit",
         "unsupported_target"}}},
      {"cells.replace",
       {kEdit,
        {"incompatible_column", "locked", "missing_drawing", "missing_level",
         "scene_limit", "unsupported_level", "unsupported_target"}}},
      {"cells.set",
       {kEdit,
        {"incompatible_column", "locked", "missing_drawing", "missing_level",
         "occupied_cells", "scene_limit", "unsupported_level",
         "unsupported_target"}}},
      {"cast.list", {kInspect, {"stale_revision"}}},
      {"cast.usage", {kInspect, {"stale_revision"}}},
      {"column.cells", {kInspect, {"incompatible_column"}}},
      {"column.create", {kFxAdd, {"unsupported_target"}}},
      {"column.delete",
       {kFxEdit,
        {"has_dependents", "incompatible_column", "locked",
         "unsupported_target"}}},
      {"column.duplicate",
       {kFxAdd, {"incompatible_column", "unsupported_target"}}},
      {"column.kinds", {kInspectWorker, {}}},
      {"column.move", {kFxEdit, {"unsupported_target"}}},
      {"column.expose",
       {kEdit,
        {"incompatible_column", "locked", "missing_drawing",
         "occupied_cells"}}},
      {"column.update", {kEdit, {"locked"}}},
      {"drawing.analysis.submit",
       {ToolEffects{"job_start", "reads", {}, "starts", {}, false},
        {"analysis_busy", "not_found", "unsupported_type"}}},
      {"drawing.clone", {kAdd, {"already_exists", "unsupported_drawing_id"}}},
      {"drawing.create", {kAdd, {"already_exists", "unsupported_drawing_id"}}},
      {"drawing.inspect", {kInspect, {"not_found", "unsupported_type"}}},
      {"drawing.remove", {kEdit, {"has_dependents", "in_use", "not_found"}}},
      {"drawing.renumber",
       {kEdit,
        {"already_exists", "locked", "not_found", "unsupported_drawing_id"}}},
      {"fx.catalog", {kInspectWorker, {}}},
      {"fx.connect",
       {kFxEdit, {"GRAPH_CYCLE", "NATIVE_FAILURE", "UNKNOWN_PORT"}}},
      {"fx.create", {kFxAdd, {"UNSUPPORTED_FX"}}},
      {"fx.delete", {kFxEdit, {"UNSUPPORTED_FX"}}},
      {"fx.disconnect", {kFxEdit, {"UNKNOWN_PORT"}}},
      {"fx.duplicate",
       {kFxAdd, {"INVALID_PARAMS", "UNSUPPORTED_FX", "locked"}}},
      {"fx.enabled.set", {kFxEdit, {"UNSUPPORTED_FX", "locked"}}},
      {"fx.graph", {kInspect, {"invalid_handle"}}},
      {"fx.graph.delta",
       {kInspect, {"delta_unavailable", "invalid_handle", "stale_revision"}}},
      {"fx.group.create",
       {kFxAdd,
        {"NATIVE_FAILURE", "STALE_ENTITY", "invalid_group", "locked",
         "resource_limit"}}},
      {"fx.group.remove",
       {kFxEdit,
        {"INVALID_GRAPH", "NATIVE_FAILURE", "locked", "unknown_group"}}},
      {"fx.group.rename",
       {kFxEdit,
        {"INVALID_GRAPH", "NATIVE_FAILURE", "locked", "unknown_group"}}},
      {"fx.layout.set",
       {kFxEdit, {"NATIVE_FAILURE", "STALE_ENTITY", "locked"}}},
      {"fx.link.add",
       {kFxEdit,
        {"NATIVE_FAILURE", "STALE_ENTITY", "already_linked",
         "link_incompatible", "locked", "resource_limit", "unsupported_link"}}},
      {"fx.link.remove",
       {kFxEdit, {"NATIVE_FAILURE", "STALE_ENTITY", "locked"}}},
      {"fx.macro.create", {kFxEdit, {"INVALID_MACRO", "invalid_handle"}}},
      {"fx.macro.expand", {kFxEdit, {"INVALID_MACRO"}}},
      {"fx.output.select", {kFxEdit, {"INVALID_OUTPUT"}}},
      {"fx.parameter.get",
       {kInspect, {"UNSUPPORTED_FX", "UNSUPPORTED_PARAMETER"}}},
      {"fx.parameter.set",
       {kFxEdit,
        {"INVALID_PARAMS", "OUT_OF_RANGE", "PARAMETER_CONSTRAINT",
         "UNKNOWN_PARAMETER", "UNSUPPORTED_FX", "locked"}}},
      {"fx.parameter.set_key",
       {kFxEdit,
        {"NOT_ANIMATABLE", "OUT_OF_RANGE", "UNKNOWN_PARAMETER", "locked"}}},
      {"fx.port_group.set",
       {kFxEdit,
        {"GRAPH_CYCLE", "INVALID_CONNECTION", "NATIVE_FAILURE",
         "PORT_TYPE_MISMATCH", "STALE_ENTITY", "UNSUPPORTED_FX", "locked",
         "resource_limit", "unknown_port_group"}}},
      {"fx.preset.apply",
       {kFxEditFromFile,
        {"INCOMPATIBLE_PRESET", "INVALID_PARAMS", "INVALID_PRESET",
         "PRESET_READ_FAILED", "RESOURCE_LIMIT", "UNREGISTERED_PRESET",
         "locked"}}},
      {"fx.preset.save",
       {kExport,
        {"ALREADY_EXISTS", "INVALID_PARAMS", "MISSING_PARENT",
         "PRESET_WRITE_FAILED", "RESOURCE_LIMIT", "publication_unsupported"}}},
      {"fx.recipe.list", {kInspectWorker, {}}},
      {"fx.rename", {kFxEdit, {"INVALID_PARAMS", "UNSUPPORTED_FX", "locked"}}},
      {"fx.terminal.set", {kFxEdit, {}}},
      {"hook.delete",
       {kEdit, {"has_dependents", "locked", "missing_key", "not_found"}}},
      {"hook.inspect", {kInspect, {"unsupported_level"}}},
      {"hook.set", {kEdit, {"locked", "missing_drawing"}}},
      {"journal.status",
       {ToolEffects{"read", "none", {"reads_private"}, "none", {}, false}, {}}},
      {"job.cancel",
       {ToolEffects{"job_control", "none", {}, "controls", {}, true}, {}}},
      {"job.list", {kJobRead, {}}},
      {"job.result",
       {kJobRead,
        {"artifacts_unavailable", "invalid_artifact", "result_unavailable",
         "scene_host_failed"}}},
      {"job.status", {kJobRead, {"scene_host_failed"}}},
      {"level.create", {kAdd, {"name_conflict"}}},
      {"level.import",
       {kImport, {"invalid_asset", "name_conflict", "unsupported_precision"}}},
      {"level.inspect", {kInspect, {}}},
      {"library.inspect",
       {kParseShared,
        {"invalid_library_item", "io_error", "not_found", "path_denied",
         "root_unavailable"}}},
      {"library.list",
       {kListShared,
        {"resource_limit", "revision_conflict", "root_unavailable"}}},
      {"library.thumbnail",
       {kParseShared,
        {"asset_unavailable", "not_found", "resource_limit",
         "root_unavailable"}}},
      {"lipsync.apply",
       {kEdit,
        {"incompatible_column", "locked", "missing_drawing", "stale_analysis",
         "timing_conflict", "unsupported_level"}}},
      {"lipsync.import",
       {ToolEffects{
            "document_write", "reads", {"reads_roots"}, "none", {}, false},
        {"invalid_asset"}}},
      {"lipsync.inspect", {kInspect, {}}},
      {"motion_path.create", {kAdd, {}}},
      {"motion_path.delete", {kEdit, {}}},
      {"motion_path.duplicate", {kAdd, {}}},
      {"motion_path.inspect", {kInspect, {"revision_conflict"}}},
      {"motion_path.list", {kInspect, {"revision_conflict"}}},
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
      {"offset_layer.update", {kEdit, {"name_conflict"}}},
      {"palette.clone_style", {kAdd, {}}},
      {"palette.create_style", {kAdd, {}}},
      {"palette.inspect", {kInspect, {"unsupported_type"}}},
      {"palette.page.create", {kAdd, {}}},
      {"palette.page.move", {kEdit, {}}},
      {"palette.page.remove", {kEdit, {"page_not_empty"}}},
      {"palette.page.rename", {kEdit, {}}},
      {"palette.pages", {kInspect, {"unsupported_type"}}},
      {"palette.rename_style", {kEdit, {}}},
      {"palette.style.move", {kEdit, {}}},
      {"palette.update_style", {kEdit, {}}},
      {"project.inspect", {kInspectWorker, {"not_found", "path_denied"}}},
      {"project.list",
       {ToolEffects{"read", "none", {"reads_roots"}, "none", {}, false},
        {"invalid_path", "io_error", "not_found", "path_denied",
         "revision_conflict"}}},
      {"project.scenes",
       {ToolEffects{"read", "none", {"reads_roots"}, "none", {}, false},
        {"invalid_path", "invalid_project", "io_error", "not_found",
         "path_denied", "resource_limit", "revision_conflict"}}},
      {"operation.list",
       {ToolEffects{"read", "none", {"reads_private"}, "none", {}, false},
        {"journal_unavailable"}}},
      {"operation.status",
       {ToolEffects{"read", "none", {"reads_private"}, "none", {}, false},
        {"journal_unavailable"}}},
      {"raster.export_png",
       {kExport,
        {"already_exists", "asset_unavailable", "io_error",
         "publication_unsupported"}}},
      {"raster.import_png",
       {kEditFromFile,
        {"already_exists", "asset_unavailable", "io_error",
         "unsupported_precision"}}},
      {"raster.patch", {kEdit, {}}},
      {"raster.read_pixels", {kInspect, {"not_found", "unsupported_type"}}},
      {"render.capabilities", {kInspectWorker, {}}},
      {"render.preflight",
       {ToolEffects{"read",
                    "reads",
                    {},
                    "none",
                    {"parse_helper", "media_tools"},
                    false},
        {"scene_host_failed", "stale_revision"}}},
      {"render.preview",
       {ToolEffects{
            "job_start", "reads", {"writes_private"}, "starts", {}, false},
        {"fx_preflight_failed", "invalid_package", "missing_dependency",
         "render_blocked", "render_busy", "render_unavailable",
         "source_changed"}}},
      {"render.submit",
       {ToolEffects{"job_start",
                    "reads",
                    {"writes_private"},
                    "starts",
                    {"media_tools"},
                    false},
        {"fx_preflight_failed", "invalid_package", "package_write_failed",
         "render_busy", "render_limit", "render_output_failed",
         "render_unavailable", "unsupported_render_settings"}}},
      {"rig.evaluate", {kInspect, {"invalid_handle"}}},
      {"rig.inspect", {kInspect, {"revision_conflict"}}},
      {"runtime.inspect",
       {ToolEffects{
            "read", "none", {}, "none", {"parse_helper", "media_tools"}, false},
        {}}},
      {"scene.check.submit",
       {ToolEffects{"job_start", "reads", {"reads_roots"}, "starts", {}, false},
        {"analysis_busy", "invalid_handle", "scene_host_failed",
         "source_changed"}}},
      {"scene.create",
       {ToolEffects{"document_lifecycle", "creates", {}, "none", {}, false},
        {}}},
      {"scene.dependencies",
       {ToolEffects{"read", "reads", {"reads_roots"}, "none", {}, false},
        {"stale_revision"}}},
      {"scene.discard",
       {ToolEffects{"document_lifecycle", "discards", {}, "none", {}, true},
        {"render_busy"}}},
      {"scene.inventory",
       {kInspect,
        {"invalid_handle", "resource_limit", "scene_host_failed",
         "stale_revision"}}},
      {"scene.open",
       {ToolEffects{"document_lifecycle",
                    "creates",
                    {"reads_roots", "reads_private", "writes_private"},
                    "none",
                    {},
                    false},
        {"invalid_package", "missing_drawing", "package_limit",
         "package_read_failed", "reader_unavailable", "unregistered_package",
         "unsupported_level"}}},
      {"scene.open_read_only",
       {ToolEffects{"document_lifecycle",
                    "opens_read_only",
                    {"reads_roots"},
                    "none",
                    {"scene_host"},
                    false},
        {"invalid_project", "invalid_scene", "io_error", "not_found",
         "project_mismatch", "scene_limit", "source_changed",
         "unsupported_type"}}},
      {"scene.package_info",
       {ToolEffects{"read",
                    "none",
                    {"reads_roots", "reads_private"},
                    "none",
                    {},
                    false},
        {"invalid_package", "package_limit", "package_read_failed",
         "unregistered_package"}}},
      {"scene.guides.set", {kEdit, {}}},
      {"scene.output.set", {kEdit, {}}},
      {"scene.properties.inspect", {kInspect, {"stale_revision"}}},
      {"scene.save_copy",
       {kExport,
        {"already_exists", "empty_level", "invalid_package", "missing_drawing",
         "missing_level", "missing_parent", "package_limit",
         "package_write_failed", "publication_unsupported",
         "reader_unavailable", "scene_limit", "unsupported_column",
         "unsupported_level", "writer_unavailable"}}},
      {"scene.settings", {kEdit, {"timing_conflict"}}},
      {"scene.source_status",
       {ToolEffects{"read", "reads", {"reads_roots"}, "none", {}, false},
        {"resource_limit", "scene_host_failed", "stale_revision",
         "unsupported_target"}}},
      {"scene.summary", {kInspect, {"scene_host_failed"}}},
      {"scene.validate", {kInspect, {"stale_revision"}}},
      {"shared.roots", {kListShared, {}}},
      {"storyboard.panel.create", {kAdd, {"already_exists"}}},
      {"storyboard.panel.delete", {kEdit, {"not_found"}}},
      {"storyboard.panel.update", {kEdit, {"already_exists", "not_found"}}},
      {"storyboard.panels", {kInspect, {"revision_conflict"}}},
      {"studio_palette.find", {kParseShared, {"root_unavailable"}}},
      {"studio_palette.inspect",
       {kParseShared,
        {"io_error", "not_found", "revision_conflict", "root_unavailable",
         "unsupported_palette"}}},
      {"studio_palette.list",
       {kListShared,
        {"io_error", "not_found", "path_denied", "root_unavailable"}}},
      {"text.fonts", {kInspectWorker, {}}},
      {"tool.describe", {kInspectWorker, {"unknown_tool"}}},
      {"vector.edit", {kEdit, {}}},
      {"vector.fill", {kEdit, {"not_found"}}},
      {"vector.group.create",
       {kEdit, {"not_found", "unsupported_grouping", "unsupported_type"}}},
      {"vector.group.remove", {kEdit, {"not_found"}}},
      {"vector.inspect",
       {kInspect,
        {"asset_unavailable", "not_found", "resource_limit", "source_changed",
         "stale_revision", "unsupported_type"}}},
      {"vector.opacity.inspect",
       {kInspect, {"not_found", "revision_conflict", "unsupported_type"}}},
      {"vector.opacity.profile.set",
       {kEdit, {"stale_selection", "unknown_selection"}}},
      {"vector.opacity.set", {kEdit, {"stale_selection", "unknown_selection"}}},
      {"vector.order.move", {kEdit, {"unsupported_grouping"}}},
      {"vector.select",
       {kInspect,
        {"not_found", "resource_limit", "revision_conflict",
         "unsupported_type"}}},
      {"vector.structure", {kInspect, {"not_found", "unsupported_type"}}},
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

// Success shapes of the tools whose descriptors do not build their own, keyed
// by tool name. The file is a resource of the executable.
const QJsonObject& successSchemas() {
  static const QJsonObject schemas = [] {
    QFile file(":/automation/automation_output_schemas.json");
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(
        file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
      throw Error("internal_error",
                  "automation_output_schemas.json is missing or malformed.");
    return document.object();
  }();
  return schemas;
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

// Handles other than documents, jobs and artifacts are resolved by lookups
// that refuse an unknown or retired handle with invalid_handle.
bool takesHandle(const QJsonObject& inputs) {
  static const QSet<QString> dedicated{"document_id", "operation_id", "job_id",
                                       "artifact_id"};
  for (const QString& input : inputs.keys())
    if (input.endsWith("_id") && !dedicated.contains(input)) return true;
  return false;
}

QStringList commonErrors(const ToolEffects& effects,
                         const QJsonObject& inputs) {
  QStringList result{"invalid_params", "method_not_found",   "worker_busy",
                     "native_error",   "response_too_large", "grant_denied"};
  if (inputs.contains("document_id"))
    result << "document_not_found" << "read_only_document";
  if (takesHandle(inputs)) result.append("invalid_handle");
  if (inputs.contains("job_id")) result.append("unknown_job");
  if (inputs.contains("path") || inputs.contains("directory") ||
      inputs.contains("output_path"))
    result << "path_denied" << "invalid_path";
  if (!isWrite(effects)) return result;
  result << "operation_conflict" << "partially_applied" << "resource_limit"
         << "operation_in_progress" << "operation_interrupted"
         << "receipt_expired" << "journal_unavailable" << "journal_busy";
  if (inputs.contains("expected_revision")) result.append("revision_conflict");
  return result;
}

[[noreturn]] void undeclared(const QString& name, const QString& problem) {
  throw Error("internal_error",
              "Tool " + name + " " + problem +
                  " in toolDeclarations (automation_effects.cpp).");
}

// A write requires operation_id. A read may name one only to look it up
// (operation.status), and is marked read-only where it was described.
void requireConsistent(const QString& name, const ToolEffects& effects,
                       const QJsonObject& descriptor) {
  const bool receipts = descriptor.value("inputSchema")
                            .toObject()
                            .value("required")
                            .toArray()
                            .contains(QJsonValue("operation_id"));
  const bool readOnly = descriptor.value("annotations")
                            .toObject()
                            .value("readOnlyHint")
                            .toBool(!receipts);
  if (isRead(effects) != readOnly || (!readOnly && !receipts))
    undeclared(name,
               readOnly ? "is described read-only, so it needs class read"
                        : "requires operation_id, so it needs a writing class");
}

// A write replayed from the workspace journal by a later worker carries a
// receipt beside its recorded result, so every write's success shape admits
// one.
QJsonObject receiptSchema() {
  const QJsonObject instance{
      {"type", "string"}, {"minLength", 36}, {"maxLength", 36}};
  return {{"type", "object"},
          {"additionalProperties", false},
          {"required", QJsonArray{"source", "recorded_by_worker",
                                  "recorded_at_ms", "handles_expired"}},
          {"properties",
           QJsonObject{
               {"source", QJsonObject{{"type", "string"},
                                      {"enum", QJsonArray{"journal"}}}},
               {"recorded_by_worker", instance},
               {"recorded_at_ms", QJsonObject{{"type", "integer"},
                                              {"minimum", 0},
                                              {"maximum", 9007199254740991.0}}},
               {"handles_expired", QJsonObject{{"type", "boolean"}}}}}};
}

QJsonObject withReceipt(QJsonObject success) {
  if (success.contains("oneOf")) {
    QJsonArray alternatives;
    for (const QJsonValue& alternative : success.value("oneOf").toArray())
      alternatives.append(withReceipt(alternative.toObject()));
    success.insert("oneOf", alternatives);
    return success;
  }
  if (!success.value("properties").isObject()) return success;
  QJsonObject properties = success.value("properties").toObject();
  properties.insert("receipt", receiptSchema());
  success.insert("properties", properties);
  return success;
}

QJsonObject receiptOutputSchema(QJsonObject descriptor) {
  QJsonObject schema = descriptor.value("outputSchema").toObject();
  QJsonArray shapes  = schema.value("oneOf").toArray();
  if (shapes.isEmpty()) return descriptor;
  shapes.replace(0, withReceipt(shapes.at(0).toObject()));
  schema.insert("oneOf", shapes);
  descriptor.insert("outputSchema", schema);
  return descriptor;
}

}  // namespace

QJsonObject declaredTool(QJsonObject descriptor, const ToolEffects& effects,
                         const QStringList& errors) {
  if (isWrite(effects) && descriptor.contains("outputSchema"))
    descriptor = receiptOutputSchema(descriptor);
  const QString name = descriptor.value("name").toString();
  const QJsonObject inputs =
      descriptor.value("inputSchema").toObject().value("properties").toObject();
  requireConsistent(name, effects, descriptor);
  QStringList codes = commonErrors(effects, inputs);
  for (const QString& code : errors)
    if (!codes.contains(code)) codes.append(code);
  if (codes.size() > maximumNativeErrors)
    undeclared(name, "declares more than 56 error codes; trim its row");
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
  const QJsonValue listed = successSchemas().value(name);
  if (listed.isObject() == descriptor.contains("outputSchema"))
    throw Error("internal_error",
                "Tool " + name +
                    (listed.isObject()
                         ? " has an outputSchema in its descriptor and in "
                           "automation_output_schemas.json; keep one."
                         : " has no outputSchema: add its success shape to "
                           "automation_output_schemas.json."));
  if (listed.isObject())
    return declaredTool(withOutputSchema(descriptor, listed.toObject()),
                        found->effects, found->errors);
  return declaredTool(descriptor, found->effects, found->errors);
}

QStringList declaredToolNames() { return declarations().keys(); }

QStringList outputSchemaNames() { return successSchemas().keys(); }

bool changesFxGraph(const QString& tool) {
  const auto found = declarations().constFind(tool);
  return found != declarations().constEnd() && found->effects.fxGraph;
}

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
  // Effects, errors and outputSchema come only when declarations are asked for.
  QStringList required{"name", "description", "toolset", "inputSchema",
                       "annotations"};
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

// A field of the result runtime.inspect also reports, such as the platform.
QJsonObject runtimeFieldSchema(const QString& field) {
  return successSchemas()
      .value("runtime.inspect")
      .toObject()
      .value("properties")
      .toObject()
      .value(field)
      .toObject();
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

QJsonObject authoritySchema() {
  const QJsonObject path = stringSchema(1, 4096);
  const QJsonObject sharedRoot =
      strictObject({{"role", enumSchema(sharedRootRoles())},
                    {"path", path},
                    {"writable", QJsonObject{{"type", "boolean"}}}});
  return strictObject(
      {{"grants",
        arraySchema(enumSchema({"inspect", "draft", "file", "job"}), 1, 4)},
       {"domains", arraySchema(stringSchema(1, 32), 1, 64)},
       {"workspace", path},
       {"input_roots", arraySchema(path, 0, 16)},
       {"shared_roots", arraySchema(sharedRoot, 0, 3)},
       {"media_tools",
        QJsonObject{
            {"oneOf", QJsonArray{path, QJsonObject{{"type", "null"}}}}}},
       {"digest", stringSchema(64, 64)}});
}

}  // namespace

QJsonObject capabilitiesResultSchema() {
  const QJsonObject toolset =
      strictObject({{"name", stringSchema(1, 32)},
                    {"description", stringSchema(1, 1024)},
                    {"tools", arraySchema(stringSchema(1, 128), 0, 1024)}});
  return strictObject(
      {{"protocol_version", QJsonObject{{"type", "integer"}, {"const", 3}}},
       {"workspace", stringSchema(1, 4096)},
       {"tools", arraySchema(descriptorSchema(), 0, 64)},
       {"tool_count",
        QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 1024}}},
       {"next_offset",
        QJsonObject{{"oneOf", QJsonArray{QJsonObject{{"type", "integer"},
                                                     {"minimum", 1},
                                                     {"maximum", 4096}},
                                         QJsonObject{{"type", "null"}}}}}},
       {"toolsets", arraySchema(toolset, 1, 64)},
       {"media_tools_configured", QJsonObject{{"type", "boolean"}}},
       {"isolation",
        QJsonObject{{"type", "string"}, {"const", "private_process"}}},
       {"platform", runtimeFieldSchema("platform")},
       {"worker", runtimeFieldSchema("worker")},
       {"read_only_scenes", readOnlyScenesSchema()},
       {"journal",
        strictObject(
            {{"available", QJsonObject{{"type", "boolean"}}},
             {"state", enumSchema({"ok", "damaged", "unavailable"})},
             {"format_version",
              QJsonObject{
                  {"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
             {"path", QJsonObject{{"type", "string"},
                                  {"const", ".groundzero/journal"}}}})},
       {"authority", authoritySchema()}});
}

QJsonObject toolDescriptionResultSchema(int maximumTools) {
  const QJsonObject tool = strictObject(
      {{"name", stringSchema(1, 128)},
       {"toolset", stringSchema(1, 32)},
       {"effects", effectsSchema()},
       {"errors", arraySchema(stringSchema(1, 64), 1, maximumErrors)},
       {"outputSchema", QJsonObject{{"type", "object"}}}});
  return strictObject({{"tools", arraySchema(tool, 1, maximumTools)}});
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
