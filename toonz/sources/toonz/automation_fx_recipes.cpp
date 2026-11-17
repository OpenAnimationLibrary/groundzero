#include "automation.h"
#include "automation_fx.h"
#include "automation_fx_profile.h"

#include <QJsonDocument>

namespace GroundzeroAutomation {
namespace {

// Static recipe documents. Steps are atomic worker tool calls; an argument
// whose whole value is "${input.<name>}" or "${steps.<id>.<field>}" takes that
// input's or earlier result's JSON value. Check frames use the input defaults.
const char* const recipeText = R"json([
 {"id": "light-wrap-composite", "version": 1, "title": "Light wrap composite",
  "outcome": "Background light spills over the foreground edges.",
  "category": "light", "required_types": ["STD_lightWrapFx"], "required_sources": [],
  "inputs": [
   {"name": "document_id", "schema": {"type": "string", "minLength": 1}},
   {"name": "foreground_fx_id", "schema": {"type": "string", "minLength": 1}},
   {"name": "background_fx_id", "schema": {"type": "string", "minLength": 1}}],
  "steps": [
   {"id": "graph", "tool": "fx.graph", "arguments": {"document_id": "${input.document_id}"}},
   {"id": "wrap", "tool": "fx.create", "arguments": {"document_id": "${input.document_id}",
    "type": "STD_lightWrapFx", "name": "Light wrap"}},
   {"id": "source", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.wrap.fx_id}", "port": "Source",
    "source_fx_id": "${input.foreground_fx_id}"}},
   {"id": "background", "tool": "fx.connect", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.wrap.fx_id}",
    "port": "Background", "source_fx_id": "${input.background_fx_id}"}},
   {"id": "width", "tool": "fx.parameter.set", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.wrap.fx_id}",
    "parameter": "width", "value": 12}},
   {"id": "output", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.graph.current_output_fx_id}", "port": "source",
    "source_fx_id": "${steps.wrap.fx_id}"}}],
  "controls": [{"name": "wrap_width", "step": "wrap", "parameter": "width"},
   {"name": "softness", "step": "wrap", "parameter": "softness"},
   {"name": "intensity", "step": "wrap", "parameter": "intensity"}],
  "seeds": [],
  "checks": [{"kind": "diagnostics_clean"}, {"kind": "render_alpha_nonempty", "frames": [0]},
   {"kind": "reopen_pixels_equal", "frames": [0]}]},
 {"id": "backlit-rim-light", "version": 1, "title": "Backlit rim light",
  "outcome": "A light source glows through the subject's silhouette edges.",
  "category": "light", "required_types": ["STD_backlitFx"], "required_sources": [],
  "inputs": [
   {"name": "document_id", "schema": {"type": "string", "minLength": 1}},
   {"name": "light_fx_id", "schema": {"type": "string", "minLength": 1}},
   {"name": "subject_fx_id", "schema": {"type": "string", "minLength": 1}}],
  "steps": [
   {"id": "graph", "tool": "fx.graph", "arguments": {"document_id": "${input.document_id}"}},
   {"id": "rim", "tool": "fx.create", "arguments": {"document_id": "${input.document_id}",
    "type": "STD_backlitFx", "name": "Rim light"}},
   {"id": "light", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.rim.fx_id}", "port": "Light", "source_fx_id": "${input.light_fx_id}"}},
   {"id": "subject", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.rim.fx_id}", "port": "Source", "source_fx_id": "${input.subject_fx_id}"}},
   {"id": "width", "tool": "fx.parameter.set", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.rim.fx_id}",
    "parameter": "value", "value": 8}},
   {"id": "output", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.graph.current_output_fx_id}", "port": "source",
    "source_fx_id": "${steps.rim.fx_id}"}}],
  "controls": [{"name": "rim_width", "step": "rim", "parameter": "value"},
   {"name": "rim_color", "step": "rim", "parameter": "color"},
   {"name": "fade", "step": "rim", "parameter": "fade"}],
  "seeds": [],
  "checks": [{"kind": "diagnostics_clean"}, {"kind": "render_alpha_nonempty", "frames": [0]},
   {"kind": "reopen_pixels_equal", "frames": [0]}]},
 {"id": "exposure-grade", "version": 1, "title": "Exposure grade",
  "outcome": "The source is re-exposed in stops, then its gamma is adjusted.",
  "category": "light", "required_types": ["STD_iwa_AdjustExposureFx", "STD_gammaFx"],
  "required_sources": [],
  "inputs": [
   {"name": "document_id", "schema": {"type": "string", "minLength": 1}},
   {"name": "source_fx_id", "schema": {"type": "string", "minLength": 1}}],
  "steps": [
   {"id": "graph", "tool": "fx.graph", "arguments": {"document_id": "${input.document_id}"}},
   {"id": "exposure", "tool": "fx.create", "arguments": {
    "document_id": "${input.document_id}", "type": "STD_iwa_AdjustExposureFx",
    "name": "Exposure"}},
   {"id": "source", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.exposure.fx_id}", "port": "Source",
    "source_fx_id": "${input.source_fx_id}"}},
   {"id": "stops", "tool": "fx.parameter.set", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.exposure.fx_id}",
    "parameter": "scale", "value": 0.5}},
   {"id": "gamma", "tool": "fx.create", "arguments": {"document_id": "${input.document_id}",
    "type": "STD_gammaFx", "name": "Grade gamma"}},
   {"id": "graded", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.gamma.fx_id}", "port": "Source",
    "source_fx_id": "${steps.exposure.fx_id}"}},
   {"id": "curve", "tool": "fx.parameter.set", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.gamma.fx_id}",
    "parameter": "value", "value": 1.4}},
   {"id": "output", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.graph.current_output_fx_id}", "port": "source",
    "source_fx_id": "${steps.gamma.fx_id}"}}],
  "controls": [{"name": "exposure", "step": "exposure", "parameter": "scale"},
   {"name": "offset", "step": "exposure", "parameter": "offset"},
   {"name": "gamma", "step": "gamma", "parameter": "value"}],
  "seeds": [],
  "checks": [{"kind": "diagnostics_clean"}, {"kind": "render_alpha_nonempty", "frames": [0]},
   {"kind": "reopen_pixels_equal", "frames": [0]}]},
 {"id": "cross-dissolve-transition", "version": 1, "title": "Cross dissolve",
  "outcome": "The from source dissolves into the to source between two frames.",
  "category": "transition", "required_types": ["blendFx"], "required_sources": [],
  "inputs": [
   {"name": "document_id", "schema": {"type": "string", "minLength": 1}},
   {"name": "from_fx_id", "schema": {"type": "string", "minLength": 1}},
   {"name": "to_fx_id", "schema": {"type": "string", "minLength": 1}},
   {"name": "start_frame",
    "schema": {"type": "integer", "minimum": 0, "maximum": 1000000, "default": 0}},
   {"name": "end_frame",
    "schema": {"type": "integer", "minimum": 0, "maximum": 1000000, "default": 12}}],
  "steps": [
   {"id": "graph", "tool": "fx.graph", "arguments": {"document_id": "${input.document_id}"}},
   {"id": "dissolve", "tool": "fx.create", "arguments": {
    "document_id": "${input.document_id}", "type": "blendFx", "name": "Cross dissolve"}},
   {"id": "to", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.dissolve.fx_id}", "port": "Source1",
    "source_fx_id": "${input.to_fx_id}"}},
   {"id": "from", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.dissolve.fx_id}", "port": "Source2",
    "source_fx_id": "${input.from_fx_id}"}},
   {"id": "start", "tool": "fx.parameter.set_key", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.dissolve.fx_id}",
    "parameter": "value", "frame": "${input.start_frame}", "value": 0}},
   {"id": "end", "tool": "fx.parameter.set_key", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.dissolve.fx_id}",
    "parameter": "value", "frame": "${input.end_frame}", "value": 100}},
   {"id": "output", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.graph.current_output_fx_id}", "port": "source",
    "source_fx_id": "${steps.dissolve.fx_id}"}}],
  "controls": [{"name": "mix", "step": "dissolve", "parameter": "value"}],
  "seeds": [],
  "checks": [{"kind": "diagnostics_clean"}, {"kind": "frames_differ", "frames": [0, 6, 12]},
   {"kind": "reopen_pixels_equal", "frames": [0, 6, 12]}]},
 {"id": "iris-wipe-transition", "version": 1, "title": "Iris wipe",
  "outcome": "A soft circular matte opens from the center to reveal the subject.",
  "category": "transition", "required_types": ["STD_radialGradientFx", "inFx"],
  "required_sources": [],
  "inputs": [
   {"name": "document_id", "schema": {"type": "string", "minLength": 1}},
   {"name": "subject_fx_id", "schema": {"type": "string", "minLength": 1}},
   {"name": "start_frame",
    "schema": {"type": "integer", "minimum": 0, "maximum": 1000000, "default": 0}},
   {"name": "end_frame",
    "schema": {"type": "integer", "minimum": 0, "maximum": 1000000, "default": 12}},
   {"name": "frame_count",
    "schema": {"type": "integer", "minimum": 1, "maximum": 10000, "default": 13}},
   {"name": "open_radius",
    "schema": {"type": "number", "minimum": 0.01, "maximum": 100000, "default": 20}}],
  "steps": [
   {"id": "graph", "tool": "fx.graph", "arguments": {"document_id": "${input.document_id}"}},
   {"id": "iris", "tool": "fx.create", "arguments": {"document_id": "${input.document_id}",
    "type": "STD_radialGradientFx", "name": "Iris", "frame_count": "${input.frame_count}"}},
   {"id": "inside", "tool": "fx.parameter.set", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.iris.fx_id}",
    "parameter": "color1", "value": [255, 255, 255, 255]}},
   {"id": "outside", "tool": "fx.parameter.set", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.iris.fx_id}",
    "parameter": "color2", "value": [0, 0, 0, 0]}},
   {"id": "closed", "tool": "fx.parameter.set_key", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.iris.fx_id}",
    "parameter": "period", "frame": "${input.start_frame}", "value": 0.01}},
   {"id": "open", "tool": "fx.parameter.set_key", "arguments": {
    "document_id": "${input.document_id}", "fx_id": "${steps.iris.fx_id}",
    "parameter": "period", "frame": "${input.end_frame}", "value": "${input.open_radius}"}},
   {"id": "matte", "tool": "fx.create", "arguments": {"document_id": "${input.document_id}",
    "type": "inFx", "name": "Iris matte"}},
   {"id": "subject", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.matte.fx_id}", "port": "Source", "source_fx_id": "${input.subject_fx_id}"}},
   {"id": "shape", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.matte.fx_id}", "port": "Matte", "source_fx_id": "${steps.iris.fx_id}"}},
   {"id": "output", "tool": "fx.connect", "arguments": {"document_id": "${input.document_id}",
    "fx_id": "${steps.graph.current_output_fx_id}", "port": "source",
    "source_fx_id": "${steps.matte.fx_id}"}}],
  "controls": [{"name": "radius", "step": "iris", "parameter": "period"},
   {"name": "solid_radius", "step": "iris", "parameter": "innerperiod"}],
  "seeds": [],
  "checks": [{"kind": "diagnostics_clean"}, {"kind": "frames_differ", "frames": [0, 6, 12]},
   {"kind": "render_alpha_nonempty", "frames": [12]},
   {"kind": "reopen_pixels_equal", "frames": [6]}]}
])json";

const QJsonArray& recipeDocuments() {
  static const QJsonArray documents = [] {
    QJsonParseError error;
    const QJsonDocument parsed =
        QJsonDocument::fromJson(QByteArray(recipeText), &error);
    if (error.error != QJsonParseError::NoError || !parsed.isArray())
      throw Error("internal_error", "The embedded FX recipes are malformed.");
    return parsed.array();
  }();
  return documents;
}

QJsonObject withAvailability(QJsonObject recipe) {
  QJsonArray missing;
  for (const QJsonValue& type : recipe.value("required_types").toArray())
    if (!isAutomationFxTypeAvailable(type.toString())) missing.append(type);
  recipe.insert("available", missing.isEmpty());
  recipe.insert("missing_types", missing);
  return recipe;
}

}  // namespace

QJsonArray automationFxRecipes() { return recipeDocuments(); }

QJsonValue listAutomationFxRecipes(const QJsonObject& params) {
  QJsonArray recipes;
  for (const QJsonValue& value : recipeDocuments()) {
    const QJsonObject recipe = value.toObject();
    if (params.contains("recipe_id") &&
        recipe.value("id") != params.value("recipe_id"))
      continue;
    if (params.contains("category") &&
        recipe.value("category") != params.value("category"))
      continue;
    recipes.append(withAvailability(recipe));
  }
  return QJsonObject{{"profile", automationFxProfileName()},
                     {"recipes", recipes}};
}

}  // namespace GroundzeroAutomation
