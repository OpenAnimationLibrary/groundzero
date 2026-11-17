#include "automation.h"
#include "automation_fx.h"
#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tenv.h"
#include "tfilepath.h"
#include "tfx.h"
#include "tfxattributes.h"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "tstream.h"
#include "toonz/toonzscene.h"
#include "toonz/tcolumnfx.h"
#include "toonz/txshzeraryfxcolumn.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>

DV_IMPORT_API void initStdFx();

namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

TFxP create(const std::string& type) {
  std::unique_ptr<TPersist> owned(TPersist::create(type));
  auto* fx = dynamic_cast<TFx*>(owned.get());
  require(fx && fx->getFxType() == type, "Registered builtin factory mismatch");
  owned.release();
  return TFxP(fx);
}

TDoubleParam* scalar(const TFxP& fx) {
  auto* value = dynamic_cast<TDoubleParam*>(fx->getParams()->getParam("value"));
  require(value != nullptr, "Native blur scalar is absent");
  return value;
}

TDoubleParam* redChannel(TFx* fx) {
  auto* color = dynamic_cast<TPixelParam*>(fx->getParams()->getParam("color"));
  require(color != nullptr, "Native color parameter is absent");
  return dynamic_cast<TDoubleParam*>(color->getParam(0).getPointer());
}

void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly),
          "Cannot create fixture file");
  require(file.write(bytes) == bytes.size(), "Cannot finish fixture file");
}

QByteArray save(const TFxP& fx, const QString& path) {
  {
    TOStream stream(TFilePath(path.toStdWString()));
    require(bool(stream), "Cannot create native preset stream");
    stream.setFullPrecision(true);
    fx->savePreset(stream);
  }
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read native preset stream");
  return file.readAll();
}

void load(const TFxP& fx, const QString& path) {
  TIStream stream(TFilePath(path.toStdWString()));
  require(bool(stream), "Cannot open native preset stream");
  fx->loadPreset(stream);
}

void checkIndependentCopy() {
  const auto source      = create("STD_blurFx");
  const auto destination = create("STD_blurFx");
  scalar(source)->setDefaultValue(1.0 / 3.0);
  scalar(source)->setKeyframe(TDoubleKeyframe(0, 2.0 / 3.0));
  scalar(source)->setKeyframe(TDoubleKeyframe(12, 10.0 / 7.0));
  destination->getParams()->copy(source->getParams());
  require(scalar(destination) != scalar(source),
          "Native copy shared its parameter object");
  require(scalar(destination)->getDefaultValue() == 1.0 / 3.0,
          "Native copy lost its default value");
  require(scalar(destination)->getKeyframeCount() == 2 &&
              scalar(destination)->getValue(6) == scalar(source)->getValue(6),
          "Native copy lost keyed interpolation");
  scalar(destination)->setKeyframe(TDoubleKeyframe(0, 8));
  require(scalar(source)->getValue(0) == 2.0 / 3.0,
          "Editing a copied curve changed its source");
  require(!destination->getInputPort("Source")->isConnected(),
          "Parameter copying unexpectedly created graph connections");
}

void checkNativePresets(const QString& directory) {
  const auto source = create("STD_blurFx");
  scalar(source)->setDefaultValue(1.0 / 3.0);
  scalar(source)->setKeyframe(TDoubleKeyframe(0, 2.0 / 3.0));
  scalar(source)->setKeyframe(TDoubleKeyframe(12, 10.0 / 7.0));
  const auto bytes    = save(source, directory + "/source.preset");
  const auto restored = create("STD_blurFx");
  load(restored, directory + "/source.preset");
  require(save(restored, directory + "/restored.preset") == bytes,
          "Full-precision native preset did not roundtrip exactly");
  require(scalar(restored)->getValue(6) == scalar(source)->getValue(6),
          "Native preset changed interior temporal evaluation");
  auto extra = bytes;
  extra.replace("</params>",
                "<unknownParameter>42</unknownParameter></params>");
  require(extra != bytes,
          "Unknown-parameter fixture did not change native bytes");
  write(directory + "/unknown.preset", extra);
  const auto permissive = create("STD_blurFx");
  load(permissive, directory + "/unknown.preset");
  require(save(permissive, directory + "/permissive.preset") == bytes,
          "Native loader no longer silently ignores unknown preset parameters");
  bool rejected = false;
  try {
    load(create("STD_glowFx"), directory + "/source.preset");
  } catch (const TException&) {
    rejected = true;
  }
  require(rejected, "Native preset loader accepted a different effect type");
}

QJsonObject command(GroundzeroAutomation::Context& context,
                    const QString& document, const QString& method,
                    QJsonObject params) {
  params["document_id"] = document;
  return GroundzeroAutomation::executeFx(context, method, params).toObject();
}

void checkDetachedHandlers(GroundzeroAutomation::Context& context) {
  using namespace GroundzeroAutomation;
  auto& document       = context.addDocument(std::make_unique<ToonzScene>());
  const auto created   = command(context, document.id, "fx.create",
                                 {{"type", "colorCardFx"}, {"frame_count", 2}});
  const QString source = created.value("fx_id").toString();
  command(context, document.id, "fx.rename",
          {{"fx_id", source}, {"name", "Prototype card"}});
  context.resetMutationStarted();
  const auto unchanged =
      command(context, document.id, "fx.rename",
              {{"fx_id", source}, {"name", "Prototype card"}});
  require(!context.mutationStarted() && unchanged.value("changed") == false,
          "Renaming to the same name must be a no-op");
  command(context, document.id, "fx.enabled.set",
          {{"fx_id", source}, {"enabled", false}});
  auto* original = dynamic_cast<TZeraryColumnFx*>(document.fx(source));
  require(original != nullptr, "Source generator wrapper is absent");
  auto* sourceRed = redChannel(original->getZeraryFx());
  sourceRed->setDefaultValue(17 / 255.0);
  sourceRed->setKeyframe(TDoubleKeyframe(0, 24 / 255.0));
  sourceRed->setKeyframe(TDoubleKeyframe(12, 200 / 255.0));
  const auto copy = command(context, document.id, "fx.duplicate",
                            {{"fx_id", source}, {"frame_count", 7}});
  auto* duplicate = dynamic_cast<TZeraryColumnFx*>(
      document.fx(copy.value("fx_id").toString()));
  require(original && duplicate && original != duplicate,
          "Generator copy did not create independent wrapper ownership");
  require(
      original->getColumn()->getRowCount() == 2 &&
          duplicate->getColumn()->getRowCount() == 7,
      "Generator copy changed source exposures or omitted requested exposures");
  require(!duplicate->getAttributes()->isEnabled() &&
              !duplicate->getZeraryFx()->getAttributes()->isEnabled(),
          "Generator copy did not preserve the effective disabled state");
  require(original->getZeraryFx()->getParams()->getParam("color") !=
              duplicate->getZeraryFx()->getParams()->getParam("color"),
          "Generator copy shared its color parameter");
  require(
      original->getZeraryFx()->getFxId() != duplicate->getZeraryFx()->getFxId(),
      "Generator copy reused the native persistent identity");
  auto* copiedRed = redChannel(duplicate->getZeraryFx());
  require(copiedRed->getDefaultValue() == 17 / 255.0 &&
              std::fabs(copiedRed->getValue(6) - 112 / 255.0) < 1e-12 &&
              copiedRed->getKeyframeCount() == 2,
          "Generator copy lost its default or keyed interpolation");
  copiedRed->setKeyframe(TDoubleKeyframe(0, 99 / 255.0));
  require(sourceRed->getValue(0) == 24 / 255.0,
          "Editing copied animation changed its source");
  validateAutomationFxGraph(*document.scene);
  context.removeDocument(document.id);
}

void rejects(const std::string& code, const std::function<void()>& action) {
  try {
    action();
  } catch (const GroundzeroAutomation::Error& error) {
    require(error.code.toStdString() == code,
            "Unexpected native rejection code");
    return;
  }
  throw std::runtime_error("Expected rejection: " + code);
}

void checkPresetGuards(GroundzeroAutomation::Context& context,
                       GroundzeroAutomation::Document& document,
                       const QJsonObject& target, const QString& directory) {
  const QString id  = target.value("fx_id").toString();
  auto* native      = document.fx(id);
  const int version = native->getFxVersion();
  native->setFxVersion(version + 1);
  context.resetMutationStarted();
  rejects("INCOMPATIBLE_PRESET", [&] {
    command(context, document.id, "fx.preset.apply",
            {{"fx_id", id}, {"directory", directory}});
  });
  require(!context.mutationStarted(), "Wrong-version preset marked a mutation");
  native->setFxVersion(version);
  QFile manifest(directory + "/manifest.json");
  require(manifest.open(QIODevice::Append) && manifest.write(" ") == 1,
          "Cannot alter fixture manifest bytes");
  manifest.close();
  rejects("INVALID_PRESET", [&] {
    command(context, document.id, "fx.preset.apply",
            {{"fx_id", id}, {"directory", directory}});
  });
  require(!context.mutationStarted(), "Altered preset marked a mutation");
}

void checkPresetHandlers(GroundzeroAutomation::Context& context,
                         const QString& directory) {
  using namespace GroundzeroAutomation;
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  const auto source =
      command(context, document.id, "fx.create", {{"type", "STD_blurFx"}});
  const auto target =
      command(context, document.id, "fx.create", {{"type", "STD_blurFx"}});
  const QString sourceId = source.value("fx_id").toString();
  const QString targetId = target.value("fx_id").toString();
  const TFxP sourceFx(document.fx(sourceId)), targetFx(document.fx(targetId));
  scalar(sourceFx)->setDefaultValue(1.0 / 3.0);
  scalar(sourceFx)->setKeyframe(TDoubleKeyframe(0, 2.0 / 3.0));
  scalar(sourceFx)->setKeyframe(TDoubleKeyframe(12, 10.0 / 7.0));
  const auto published =
      command(context, document.id, "fx.preset.save",
              {{"fx_id", sourceId}, {"directory", directory}});
  require(document.revision == 0 && context.mutationStarted(),
          "File publication changed the document revision or omitted the "
          "mutation marker");
  require(
      published.value("manifest").toObject().value("source_revision") == "0",
      "Preset manifest lost source revision");
  command(context, document.id, "fx.preset.apply",
          {{"fx_id", targetId}, {"directory", directory}});
  require(scalar(targetFx)->getDefaultValue() == 1.0 / 3.0 &&
              scalar(targetFx)->getValue(6) == scalar(sourceFx)->getValue(6),
          "Applying a native preset lost defaults or keyed interpolation");
  context.resetMutationStarted();
  const auto unchanged =
      command(context, document.id, "fx.preset.apply",
              {{"fx_id", targetId}, {"directory", directory}});
  require(!unchanged.value("changed").toBool() && !context.mutationStarted(),
          "Applying identical native parameters must be a no-op");
  scalar(targetFx)->setKeyframe(TDoubleKeyframe(0, 42));
  require(scalar(sourceFx)->getValue(0) == 2.0 / 3.0,
          "Preset destination shared its source parameter");
  checkPresetGuards(context, document, target, directory);
  context.removeDocument(document.id);
}

void checkPresetKeyBoundary(GroundzeroAutomation::Context& context,
                            const QString& directory) {
  using namespace GroundzeroAutomation;
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  const auto source =
      command(context, document.id, "fx.create", {{"type", "STD_blurFx"}});
  const QString id = source.value("fx_id").toString();
  const TFxP fx(document.fx(id));
  for (int frame = 0; frame < 4096; ++frame)
    scalar(fx)->setKeyframe(TDoubleKeyframe(frame, frame % 8));
  command(context, document.id, "fx.preset.save",
          {{"fx_id", id}, {"directory", directory + "/at-limit"}});
  scalar(fx)->setKeyframe(TDoubleKeyframe(4096, 4));
  context.resetMutationStarted();
  rejects("RESOURCE_LIMIT", [&] {
    command(context, document.id, "fx.preset.save",
            {{"fx_id", id}, {"directory", directory + "/above-limit"}});
  });
  require(
      !context.mutationStarted() && !QDir(directory + "/above-limit").exists(),
      "Over-limit preset changed the document or published a directory");
  context.removeDocument(document.id);
}

void checkMacroEligibility() {
  const auto card = create("colorCardFx");
  const auto blur = create("STD_blurFx");
  const auto glow = create("STD_glowFx");
  require(!TMacroFx::analyze({blur, glow}),
          "Disconnected macro selection was accepted");
  blur->getInputPort("Source")->setFx(card.getPointer());
  glow->getInputPort("Source")->setFx(blur.getPointer());
  glow->getInputPort("Light")->setFx(card.getPointer());
  require(TMacroFx::analyze({blur, glow}),
          "Connected single-root macro was rejected");
  const TFxP owned(TMacroFx::create({blur, glow}));
  const auto* macro = dynamic_cast<TMacroFx*>(owned.getPointer());
  require(macro && macro->getRoot() == glow.getPointer() &&
              macro->getFxs().size() == 2,
          "Native macro lost its root or internal members");
  require(macro->getInputPortCount() == 2,
          "Native macro lost boundary input ports");
  for (int index = 0; index < macro->getInputPortCount(); ++index)
    require(macro->getInputPort(index)->getFx() == card.getPointer(),
            "Native macro changed a boundary source");
}

}  // namespace

namespace GroundzeroAutomation {

void runAutomationFxAuthoringTests(Context& context) {
  QTemporaryDir temporary(context.workspace() + "/.fx-authoring-XXXXXX");
  require(temporary.isValid(),
          "Cannot create native authoring fixture directory");
  checkIndependentCopy();
  checkNativePresets(temporary.path());
  checkMacroEligibility();
  checkDetachedHandlers(context);
  checkPresetHandlers(context, temporary.path() + "/published");
  checkPresetKeyBoundary(context, temporary.path());
}

}  // namespace GroundzeroAutomation

#ifdef AUTOMATION_FX_AUTHORING_STANDALONE
int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir temporary;
    require(temporary.isValid(),
            "Cannot create native authoring fixture directory");
    TEnv::setRootVarName("TOONZROOT");
    TEnv::setSystemVarPrefix("TOONZ");
    TEnv::setArgPathValue("TOONZROOT", temporary.path().toStdString());
    for (const auto& entry : TEnv::getSystemPathMap()) {
      const QString path =
          QDir(temporary.path()).filePath(QString::fromStdString(entry.second));
      require(QDir().mkpath(path),
              "Cannot create isolated native runtime directory");
      TEnv::setArgPathValue("TOONZ" + entry.first, path.toStdString());
    }
    initStdFx();
    GroundzeroAutomation::Context context(temporary.path(), {});
    GroundzeroAutomation::runAutomationFxAuthoringTests(context);
    const QJsonObject report{
        {"status", "passed"},
        {"native_parameter_independence", true},
        {"detached_authoring_handlers", true},
        {"native_preset_handlers_and_key_limits", true},
        {"full_precision_preset_roundtrip", true},
        {"unknown_preset_fields_ignored_by_native_loader", true},
        {"macro_boundary_inputs_preserved", true}};
    std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Native FX authoring probe failed: %s\n",
                 error.what());
    return 1;
  }
}
#endif
