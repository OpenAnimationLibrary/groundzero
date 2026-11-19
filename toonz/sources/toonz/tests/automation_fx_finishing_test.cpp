#include "automation.h"
#include "automation_dispatch.h"
#include "automation_fx.h"
#include "automation_fx_profile.h"
#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/toonzscene.h"

#include <QRegularExpression>
#include <QTemporaryDir>

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

using namespace GroundzeroAutomation;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QJsonObject call(Context& context, Document& document, const QString& method,
                 QJsonObject params) {
  params["document_id"] = document.id;
  return executeFx(context, method, params).toObject();
}

TFx* create(Context& context, Document& document, const QString& type) {
  return document.fx(call(context, document, "fx.create",
                          {{"type", type}, {"frame_count", 13}})
                         .value("fx_id")
                         .toString());
}

TFx* native(TFx* fx) {
  auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx);
  return wrapper ? wrapper->getZeraryFx() : fx;
}

TParam* param(TFx* fx, const char* name) {
  TParam* found = native(fx)->getParams()->getParam(name);
  require(found, "Finishing parameter is missing");
  return found;
}

TDoubleParam* number(TFx* fx, const char* name) {
  auto* found = dynamic_cast<TDoubleParam*>(param(fx, name));
  require(found, "Finishing parameter is not a number");
  return found;
}

void rejects(Context& context, const std::function<void()>& action,
             const QString& code, QJsonObject* details = nullptr) {
  context.resetMutationStarted();
  try {
    action();
  } catch (const Error& error) {
    require(error.code == code, "Finishing rejection code changed");
    require(!context.mutationStarted(),
            "A finishing rejection began a mutation");
    if (details) *details = error.details;
    return;
  }
  throw std::runtime_error("Finishing invalid state was accepted");
}

void rejectsScene(Context& context, Document& document, const QString& code,
                  const QString& path) {
  rejects(context, [&] { validateAutomationFxGraph(*document.scene); }, code);
  document.scene->saveCopy(TFilePath(path.toStdWString()));
  ToonzScene reopened;
  reopened.setProject(document.scene->getProject());
  reopened.loadTnzFile(TFilePath(path.toStdWString()));
  rejects(context, [&] { validateAutomationFxGraph(reopened); }, code);
}

void rejectsEdit(Context& context, Document& document, TFx* fx,
                 const QString& name, const QJsonValue& value,
                 const QString& code) {
  const auto before = executeFx(
      context, "fx.parameter.get",
      {{"document_id", document.id}, {"fx_id", document.idForFx(fx)}});
  rejects(
      context,
      [&] {
        call(context, document, "fx.parameter.set",
             {{"fx_id", document.idForFx(fx)},
              {"parameter", name},
              {"value", value}});
      },
      code);
  require(executeFx(context, "fx.parameter.get",
                    {{"document_id", document.id},
                     {"fx_id", document.idForFx(fx)}}) == before,
          "A rejected finishing edit changed the effect");
}

void requireWithinNative(TFx* fx) {
  for (int index = 0; index < fx->getParams()->getParamCount(); ++index) {
    const TParamVar* entry = fx->getParams()->getParamVar(index);
    if (entry->isObsolete()) continue;
    const QJsonObject schema =
        automationFxParameterSchema(fx, entry->getParam(), entry->getName());
    require(!schema.isEmpty(), "An admitted parameter has no schema");
    const QJsonObject bounds = schema.value("type") == "array"
                                   ? schema.value("items").toObject()
                                   : schema;
    if (!bounds.contains("minimum")) continue;
    const double minimum = bounds.value("minimum").toDouble();
    const double maximum = bounds.value("maximum").toDouble();
    require(minimum <= maximum && std::fabs(minimum) <= 1e5 &&
                std::fabs(maximum) <= 1e5,
            "Every admitted bound must be finite and at most 1e5");
    auto* curve = dynamic_cast<TDoubleParam*>(entry->getParam());
    double low, high, step;
    if (curve && curve->getValueRange(low, high, step))
      require(low <= minimum && maximum <= high,
              "An admitted range leaves the native range");
  }
}

void catalogProfile(Context& context) {
  const QJsonObject catalog = executeFx(context, "fx.catalog", {}).toObject();
  require(catalog.value("profile") == "builtin-2d-v2",
          "The tightened profile must be builtin-2d-v2");
  const QJsonArray effects = catalog.value("effects").toArray();
  require(effects.size() == 31, "The profile admits exactly 31 effects");
  for (const QJsonValue& value : effects) {
    const QJsonObject entry = value.toObject();
    const QString type      = entry.value("type").toString();
    require(entry.value("available").toBool(), "Admitted effect missing");
    const int version =
        type == "STD_iwa_BloomFx" || type == "STD_iwa_AdjustExposureFx" ? 3 : 1;
    require(entry.value("native_fx_version").toInt() == version,
            "Admitted effect has an unexpected native version");
    for (const char* field : {"seed_parameters", "constraints",
                              "extent_parameters", "required_inputs"})
      require(entry.value(field).isArray(), "Catalog profile field missing");
    require(entry.value("replay_class") == "stateless",
            "S1 effects are stateless");
    TFxP fx = makeAutomationEffect(type);
    requireWithinNative(fx.getPointer());
    for (const QJsonValue& extent : entry.value("extent_parameters").toArray())
      require(fx->getParams()->getParam(
                  extent.toObject().value("name").toString().toStdString()),
              "An extent names a missing parameter");
    for (const QJsonValue& port : entry.value("required_inputs").toArray())
      require(fx->getInputPort(port.toString().toStdString()),
              "A required input names a missing port");
  }
}

void legacyValues(Context& context, const QString& directory) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  TFx* blur      = create(context, document, "STD_blurFx");
  number(blur, "value")->setDefaultValue(200.5);
  rejectsScene(context, document, "OUT_OF_RANGE", directory + "/blur.tnz");
  number(blur, "value")->setDefaultValue(200);
  TFx* gradient = create(context, document, "STD_radialGradientFx");
  number(gradient, "period")->setDefaultValue(0);
  rejectsScene(context, document, "OUT_OF_RANGE", directory + "/radial.tnz");
  number(gradient, "period")->setDefaultValue(0.01);
  TFx* highlight = create(context, document, "STD_bodyHighLightFx");
  auto* color    = dynamic_cast<TPixelParam*>(param(highlight, "color"));
  color->setDefaultValue(TPixel32(255, 255, 255, 128));
  rejectsScene(context, document, "OUT_OF_RANGE", directory + "/matte.tnz");
  color->setDefaultValue(TPixel32::White);
  auto* point = dynamic_cast<TPointParam*>(param(highlight, "point"));
  TDoubleKeyframe key(6, 100.5);
  key.m_isKeyframe = true;
  point->getX()->setKeyframe(key);
  rejectsScene(context, document, "OUT_OF_RANGE", directory + "/point.tnz");
  point->getX()->clearKeyframes();
  TFx* wrap = create(context, document, "STD_lightWrapFx");
  number(wrap, "blend")->setDefaultValue(0.5);
  rejects(
      context, [&] { validateAutomationFxGraph(*document.scene); },
      "INVALID_PARAMS");
  number(wrap, "blend")->setDefaultValue(1);
  TFx* bloom = create(context, document, "STD_iwa_BloomFx");
  require(bloom->getFxVersion() == 3, "Bloom must be created at version 3");
  // Version 2 with the default gamma upgrades itself to 3; version 1 stays.
  bloom->setFxVersion(1);
  rejects(
      context, [&] { validateAutomationFxGraph(*document.scene); },
      "UNSUPPORTED_FX");
  bloom->setFxVersion(3);
  validateAutomationFxGraph(*document.scene);
  context.removeDocument(document.id);
}

void macroMembers(Context& context) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  TFx* blur      = create(context, document, "STD_blurFx");
  TFx* glow      = create(context, document, "STD_glowFx");
  glow->getInputPort("Light")->setFx(blur);
  const QJsonObject created = call(
      context, document, "fx.macro.create",
      {{"name", "Finishing macro"},
       {"fx_ids", QJsonArray{document.idForFx(blur), document.idForFx(glow)}}});
  // Creation retires the selected effects; edit the macro's own member.
  auto* macro = dynamic_cast<TMacroFx*>(
      document.fx(created.value("node").toObject().value("fx_id").toString()));
  require(macro, "Macro creation returned no macro");
  TFx* member = nullptr;
  for (const auto& candidate : macro->getFxs())
    if (candidate->getFxType() == "STD_blurFx") member = candidate.getPointer();
  number(member, "value")->setDefaultValue(500);
  rejects(
      context, [&] { validateAutomationFxGraph(*document.scene); },
      "OUT_OF_RANGE");
  number(member, "value")->setDefaultValue(10);
  validateAutomationFxGraph(*document.scene);
  context.removeDocument(document.id);
}

void editBounds(Context& context) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  TFx* highlight = create(context, document, "STD_bodyHighLightFx");
  rejectsEdit(context, document, highlight, "color",
              QJsonArray{255, 255, 255, 128}, "OUT_OF_RANGE");
  rejectsEdit(context, document, highlight, "point", QJsonArray{0, 100.5},
              "OUT_OF_RANGE");
  call(context, document, "fx.parameter.set",
       {{"fx_id", document.idForFx(highlight)},
        {"parameter", "point"},
        {"value", QJsonArray{-100, 100}}});
  TFx* wrap = create(context, document, "STD_lightWrapFx");
  rejectsEdit(context, document, wrap, "blend", 0.5, "INVALID_PARAMS");
  rejectsEdit(context, document, wrap, "softness", 20.001, "OUT_OF_RANGE");
  TFx* solarize = create(context, document, "STD_solarizeFx");
  rejectsEdit(context, document, solarize, "peak_edge", 0.5, "OUT_OF_RANGE");
  context.removeDocument(document.id);
}

QJsonObject pointEdit(Document& document, TFx* fx, const char* name, double x,
                      double y, int frame = -1) {
  QJsonObject edit{{"fx_id", document.idForFx(fx)},
                   {"parameter", name},
                   {"value", QJsonArray{x, y}}};
  if (frame >= 0) edit["frame"] = frame;
  return edit;
}

void requireSeparationFailure(Context& context, Document& document, TFx* fx,
                              const QString& method, const QJsonObject& edit,
                              double frame) {
  QJsonObject details;
  rejects(
      context, [&] { call(context, document, method, edit); },
      "PARAMETER_CONSTRAINT", &details);
  require(details.value("constraint") == "linear_gradient_separation" &&
              details.value("fx_id") == document.idForFx(fx) &&
              details.value("parameters") ==
                  QJsonArray({"startPoint", "endPoint"}) &&
              std::fabs(details.value("frame").toDouble() - frame) < 1e-6,
          "Separation failure details changed");
}

void gradientSeparation(Context& context) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  TFx* gradient  = create(context, document, "STD_iwa_LinearGradientFx");
  requireSeparationFailure(
      context, document, gradient, "fx.parameter.set",
      pointEdit(document, gradient, "endPoint", -50.0005, 0), 0);
  call(context, document, "fx.parameter.set",
       pointEdit(document, gradient, "endPoint", -50, 0));
  call(context, document, "fx.parameter.set",
       pointEdit(document, gradient, "endPoint", 5, 0.0011));
  call(context, document, "fx.parameter.set_key",
       pointEdit(document, gradient, "startPoint", 0, 0, 0));
  call(context, document, "fx.parameter.set_key",
       pointEdit(document, gradient, "startPoint", 10, 0, 10));
  for (double y : {0.0009, 0.0})
    requireSeparationFailure(context, document, gradient, "fx.parameter.set",
                             pointEdit(document, gradient, "endPoint", 5, y),
                             5);
  requireSeparationFailure(
      context, document, gradient, "fx.parameter.set_key",
      pointEdit(document, gradient, "startPoint", 4, 0.0011, 4), 5);
  auto* end = dynamic_cast<TPointParam*>(param(gradient, "endPoint"));
  end->getY()->setDefaultValue(0.0005);
  rejects(
      context, [&] { validateAutomationFxGraph(*document.scene); },
      "PARAMETER_CONSTRAINT");
  end->getY()->setDefaultValue(0.0011);
  validateAutomationFxGraph(*document.scene);
  context.removeDocument(document.id);
}

void requiredInputs(Context& context) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  TFx* card      = create(context, document, "colorCardFx");
  const std::map<QString, QString> ports{{"STD_backlitFx", "Light"},
                                         {"STD_lightWrapFx", "Background"},
                                         {"STD_localBlurFx", "Reference"}};
  for (const auto& [type, port] : ports) {
    const QJsonObject created =
        call(context, document, "fx.create", {{"type", type}});
    TFx* fx                = document.fx(created.value("fx_id").toString());
    const QJsonArray found = created.value("diagnostics").toArray();
    require(found.size() == 1 &&
                found[0].toObject().value("code") == "REQUIRED_INPUT_MISSING" &&
                found[0].toObject().value("port") == port,
            "A missing effect input must be diagnosed");
    fx->getInputPort(port.toStdString())->setFx(card);
    require(automationFxProfileDiagnostics(fx).isEmpty(),
            "A connected effect input must not be diagnosed");
  }
  context.removeDocument(document.id);
}

struct Catalog {
  std::map<QString, QJsonObject> tools;
  QSet<QString> toolsets;
};

Catalog liveCatalog(Context& context) {
  Dispatcher dispatcher(context);
  Catalog catalog;
  QJsonValue offset = 0;
  while (!offset.isNull()) {
    const QJsonObject response =
        dispatcher.dispatch({{"id", "tools"},
                             {"method", "automation.capabilities"},
                             {"params", QJsonObject{{"offset", offset},
                                                    {"limit", 64},
                                                    {"declarations", true}}}});
    const QJsonObject page = response.value("result").toObject();
    for (const QJsonValue& tool : page.value("tools").toArray())
      catalog.tools[tool.toObject().value("name").toString()] = tool.toObject();
    for (const QJsonValue& toolset : page.value("toolsets").toArray())
      catalog.toolsets.insert(toolset.toObject().value("name").toString());
    offset = page.value("next_offset");
  }
  return catalog;
}

// Every placeholder names a declared input or an earlier step, then dotted
// field names and array indexes.
void requirePlaceholders(const QJsonValue& value, const QSet<QString>& inputs,
                         const QSet<QString>& steps) {
  static const QRegularExpression pattern(
      R"(^\$\{(input|steps)\.([a-z_]+)((\.[a-z_0-9]+)*)\}$)");
  if (value.isArray()) {
    for (const QJsonValue& item : value.toArray())
      requirePlaceholders(item, inputs, steps);
    return;
  }
  if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (auto item = object.begin(); item != object.end(); ++item)
      requirePlaceholders(item.value(), inputs, steps);
    return;
  }
  const QString text = value.toString();
  if (!text.contains("${")) return;
  const auto match = pattern.match(text);
  require(match.hasMatch(), "A recipe placeholder is malformed");
  require(match.captured(1) == "input" ? inputs.contains(match.captured(2)) &&
                                             match.captured(3).isEmpty()
                                       : steps.contains(match.captured(2)),
          "A recipe placeholder names an unknown input or later step");
}

QStringList strings(const QJsonValue& value) {
  QStringList result;
  for (const QJsonValue& item : value.toArray()) result.append(item.toString());
  return result;
}

QStringList sorted(QSet<QString> set) {
  QStringList result(set.begin(), set.end());
  result.sort();
  return result;
}

// The capabilities a recipe lists are exactly what its steps use, and every
// one exists in the live catalog.
void requireCapabilities(const QJsonObject& recipe, const Catalog& catalog) {
  QSet<QString> tools, toolsets, grants;
  for (const QJsonValue& value : recipe.value("steps").toArray()) {
    const QString name = value.toObject().value("tool").toString();
    const auto found   = catalog.tools.find(name);
    require(found != catalog.tools.end(),
            "A recipe step names an unknown tool");
    tools.insert(name);
    toolsets.insert(found->second.value("toolset").toString());
    for (const QJsonValue& grant :
         found->second.value("effects").toObject().value("grants").toArray())
      grants.insert(grant.toString());
  }
  const QJsonObject listed = recipe.value("required_capabilities").toObject();
  require(listed.keys() == QStringList({"grants", "tools", "toolsets"}),
          "Required capabilities have the wrong fields");
  require(strings(listed.value("tools")) == sorted(tools),
          "A recipe lists tools its steps do not use, or omits some");
  require(strings(listed.value("toolsets")) == sorted(toolsets),
          "A recipe lists toolsets its tools are not in, or omits some");
  require(strings(listed.value("grants")) == sorted(grants),
          "A recipe lists grants its tools do not need, or omits some");
  for (const QString& toolset : strings(listed.value("toolsets")))
    require(catalog.toolsets.contains(toolset),
            "A recipe names a toolset the server lacks");
  for (const QString& grant : strings(listed.value("grants")))
    require(QStringList({"inspect", "draft", "file", "job"}).contains(grant),
            "A recipe names a grant the server lacks");
}

QString stepReference(const QString& from) {
  static const QRegularExpression pattern(
      R"(^steps\.([a-z_]+)(\.[a-z_0-9]+)+$)");
  const auto match = pattern.match(from);
  return match.hasMatch() ? match.captured(1) : QString();
}

void requireOutputs(const QJsonObject& recipe, const QSet<QString>& steps,
                    const QSet<QString>& inputs) {
  const QJsonArray deliverables = recipe.value("deliverables").toArray();
  require(!deliverables.isEmpty() && deliverables.size() <= 8,
          "A recipe needs 1 to 8 deliverables");
  QSet<QString> ids;
  for (const QJsonValue& value : deliverables) {
    const QJsonObject item = value.toObject();
    require(item.keys() == QStringList({"from", "id", "label", "note"}) &&
                !ids.contains(item.value("id").toString()),
            "A deliverable has the wrong fields or repeats an id");
    ids.insert(item.value("id").toString());
    // externally_generated is not a label: this server has no such output.
    require(
        QStringList({"editable", "approximated", "baked"})
            .contains(item.value("label").toString()),
        "A deliverable carries a label outside editable, approximated, baked");
    require(steps.contains(stepReference(item.value("from").toString())),
            "A deliverable does not point at a step result");
  }
  const QJsonObject ownership = recipe.value("ownership").toObject();
  require(ownership.keys() == QStringList({"creates", "edits", "leaves_alone"}),
          "Ownership has the wrong fields");
  for (const QString& id : strings(ownership.value("creates")))
    require(steps.contains(id), "Ownership creates a step that does not exist");
  for (const QString& name : strings(ownership.value("edits")))
    require(inputs.contains(name),
            "Ownership edits an input that does not exist");
  const QJsonObject units = recipe.value("units").toObject();
  require(units.keys() == QStringList({"frame_origin", "frame_rate", "frames",
                                       "lengths"}) &&
              units.value("frame_origin").toInt(-1) == 0,
          "Units must state a zero frame origin, frames, lengths and rate");
  const QJsonArray preconditions = recipe.value("preconditions").toArray();
  require(!preconditions.isEmpty() && preconditions.size() <= 8,
          "A recipe needs 1 to 8 preconditions");
  const int brief = recipe.value("brief").toString().size();
  require(brief >= 1 && brief <= 600, "A brief is 1 to 600 characters");
}

// A preview review names the render, then the steps that read its result.
void requireReview(const QJsonObject& recipe, const QJsonArray& steps) {
  const QJsonObject review = recipe.value("review").toObject();
  const QString kind       = review.value("kind").toString();
  if (kind == "checks") {
    require(review.keys() == QStringList({"kind"}) &&
                !recipe.value("checks").toArray().isEmpty(),
            "A checks review needs checks and no other field");
    return;
  }
  require(kind == "preview" &&
              review.keys() ==
                  QStringList({"frames", "kind", "preview_step", "read_steps",
                               "required_before_success"}) &&
              review.value("required_before_success").toBool(),
          "A review is checks or a preview that is required before success");
  QStringList order;
  QMap<QString, QString> tools;
  for (const QJsonValue& value : steps) {
    order.append(value.toObject().value("id").toString());
    tools[order.last()] = value.toObject().value("tool").toString();
  }
  const QString preview = review.value("preview_step").toString();
  require(tools.value(preview) == "render.preview",
          "The reviewed step is not a preview render");
  const QStringList reads = strings(review.value("read_steps"));
  require(reads.size() >= 2 && tools.value(reads.last()) == "artifact.read",
          "A review ends by reading the preview artifact");
  QSet<QString> readTools;
  int position = order.indexOf(preview);
  for (const QString& id : reads) {
    require(order.indexOf(id) > position, "A review read precedes its preview");
    position = order.indexOf(id);
    readTools.insert(tools.value(id));
  }
  require(readTools.contains("job.result") && readTools.contains("job.status"),
          "A review must wait for the job and read its result");
  require(!review.value("frames").toArray().isEmpty(),
          "A review names the frames it covers");
}

void requireRecipe(const QJsonObject& recipe, const Catalog& catalog,
                   const std::map<QString, QSet<QString>>& parameters) {
  QSet<QString> inputs, steps;
  for (const QJsonValue& input : recipe.value("inputs").toArray())
    inputs.insert(input.toObject().value("name").toString());
  std::map<QString, QString> created;
  const QJsonArray required = recipe.value("required_types").toArray();
  require(recipe.value("steps").toArray().size() <= 32,
          "A recipe has at most 32 steps");
  for (const QJsonValue& value : recipe.value("steps").toArray()) {
    const QJsonObject step = value.toObject();
    require(catalog.tools.count(step.value("tool").toString()) == 1,
            "A recipe step names an unknown tool");
    requirePlaceholders(step.value("arguments"), inputs, steps);
    const QString id = step.value("id").toString();
    require(!steps.contains(id), "Recipe step ids must be unique");
    steps.insert(id);
    if (step.contains("poll")) {
      const QJsonObject poll = step.value("poll").toObject();
      require(step.value("tool") == "job.status" &&
                  poll.keys() == QStringList({"field", "max_attempts",
                                              "success", "terminal"}) &&
                  !strings(poll.value("success")).isEmpty(),
              "Only job.status is polled, with a field, limits and successes");
      for (const QString& state : strings(poll.value("success")))
        require(strings(poll.value("terminal")).contains(state),
                "A polled success is not terminal");
    }
    if (step.value("tool") == "fx.create") {
      const QString type =
          step.value("arguments").toObject().value("type").toString();
      require(required.contains(type), "A created type is not required");
      created[id] = type;
    }
  }
  for (const QJsonValue& value : recipe.value("controls").toArray()) {
    const QJsonObject control = value.toObject();
    if (control.contains("input")) {
      require(steps.contains(control.value("step").toString()) &&
                  inputs.contains(control.value("input").toString()),
              "A recipe control names a missing step or input");
      continue;
    }
    const auto step = created.find(control.value("step").toString());
    require(step != created.end() &&
                parameters.at(step->second)
                    .contains(control.value("parameter").toString()),
            "A recipe control names a missing step parameter");
  }
  requireCapabilities(recipe, catalog);
  requireOutputs(recipe, steps, inputs);
  requireReview(recipe, recipe.value("steps").toArray());
}

void recipeCatalog(Context& context) {
  const QJsonObject listed = listAutomationFxRecipes({}).toObject();
  require(listed.value("profile") == "builtin-2d-v2",
          "Recipes report the profile");
  const QJsonArray recipes = listed.value("recipes").toArray();
  require(recipes.size() == 8 && recipes.size() <= 64,
          "Eight recipes ship: five FX and three scene recipes");
  std::map<QString, QSet<QString>> parameters;
  for (const QJsonValue& value : executeFx(context, "fx.catalog", {})
                                     .toObject()
                                     .value("effects")
                                     .toArray())
    for (const QJsonValue& parameter :
         value.toObject().value("parameters").toArray())
      parameters[value.toObject().value("type").toString()].insert(
          parameter.toObject().value("name").toString());
  const Catalog catalog = liveCatalog(context);
  QSet<QString> ids;
  for (const QJsonValue& value : recipes) {
    const QJsonObject recipe = value.toObject();
    require(recipe.value("available").toBool() &&
                recipe.value("missing_types").toArray().isEmpty(),
            "Every recipe uses only available effects");
    require(!ids.contains(recipe.value("id").toString()), "Duplicate recipe");
    require(recipe.keys() == QStringList({"available",
                                          "brief",
                                          "category",
                                          "checks",
                                          "controls",
                                          "deliverables",
                                          "id",
                                          "inputs",
                                          "limits",
                                          "missing_types",
                                          "outcome",
                                          "ownership",
                                          "preconditions",
                                          "required_capabilities",
                                          "required_sources",
                                          "required_types",
                                          "review",
                                          "seeds",
                                          "steps",
                                          "title",
                                          "units",
                                          "version"}),
            "A recipe document has fields outside its contract");
    require(recipe.value("version").toInt() >= 1 &&
                recipe.value("id").toString().size() <= 64,
            "A recipe has a version and an id of at most 64 characters");
    require(recipe.value("seeds").toArray().isEmpty(),
            "No recipe draws random numbers, so none lists a seed");
    ids.insert(recipe.value("id").toString());
    requireRecipe(recipe, catalog, parameters);
  }
  const std::map<QString, int> categories{{"transition", 2},
                                          {"light", 3},
                                          {"shot", 1},
                                          {"rig", 1},
                                          {"delivery", 1}};
  for (const auto& [category, count] : categories)
    require(listAutomationFxRecipes({{"category", category}})
                    .toObject()
                    .value("recipes")
                    .toArray()
                    .size() == count,
            "Recipe category filter changed");
  require(listAutomationFxRecipes({{"recipe_id", "no-such-recipe"}})
              .toObject()
              .value("recipes")
              .toArray()
              .isEmpty(),
          "An unknown recipe id must list nothing");
}
}  // namespace

namespace GroundzeroAutomation {
void runAutomationFxFinishingTests(Context& context) {
  QTemporaryDir directory(context.workspace() + "/finishing-tests-XXXXXX");
  require(directory.isValid(), "Cannot create finishing fixture directory");
  catalogProfile(context);
  legacyValues(context, directory.path());
  macroMembers(context);
  editBounds(context);
  gradientSeparation(context);
  requiredInputs(context);
  recipeCatalog(context);
}
}  // namespace GroundzeroAutomation
