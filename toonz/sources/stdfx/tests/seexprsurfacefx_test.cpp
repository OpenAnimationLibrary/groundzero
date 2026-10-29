#include "../seexprfx.cpp"
#include "../seexprattributefx.cpp"
#include "../glbmodelfx.cpp"
#include "../transform3dfx.cpp"
#include "../imageplanefx.cpp"
#include "../threepointlightfx.cpp"
#include "../../glb/tests/glbfixture.h"
#include "tfilepath.h"
#include "tstream.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <future>
#include <iostream>

class SurfaceRasterSource final : public TStandardRasterFx {
  TRaster64P m_image;
public:
  int computes = 0;
  int planned = 0;
  TRectD plannedBounds;
  SurfaceRasterSource() : m_image(8, 8) {
    m_image->fill(TPixel64(13107, 6554, 0, 32768));
  }
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override { return "surface-raster-fixture"; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, 8, 8); return true;
  }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    ++computes; tile.getRaster()->clear();
    tile.getRaster()->copy(m_image, TPoint(-int(tile.m_pos.x), -int(tile.m_pos.y)));
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override { compute(tile, frame, settings); }
  void dryCompute(TRectD &rect, double, const TRenderSettings &settings) override {
    ++planned; plannedBounds = rect;
    if (settings.m_affine != TAffine() || settings.m_bpp != 64)
      throw TException("Image-plane preflight did not preserve its texture context");
  }
};

namespace {
void surfaceCheck(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
template<class Param> Param *surfaceParam(TFx &fx, const char *name) {
  auto *parameter = dynamic_cast<Param *>(fx.getParams()->getParam(name));
  surfaceCheck(parameter != nullptr, "Surface fixture native parameter is missing");
  return parameter;
}
void surfaceDocument(SeExprSurfaceFx &fx, const QString &rgb, const QString &alpha,
                      const QString &displacement, const QJsonArray &controls = {},
                      const QString &opacity = QString()) {
  QJsonObject document{{"version", 1}, {"rgb", rgb}, {"alpha", alpha},
                       {"displacement", displacement}, {"controls", controls}};
  if (!opacity.isNull()) document.insert("opacity", opacity);
  fx.seExprDocumentParam()->setValue(QString::fromUtf8(
      QJsonDocument(document).toJson(QJsonDocument::Compact)).toStdWString());
}
TRenderSettings surfaceSettings(double tint) {
  TRenderSettings settings;
  settings.m_bpp = 128; settings.m_affine = TScale(.1);
  TSeExprAttributes::Snapshot attributes;
  attributes.attributes["user::tint"] = TSeExpr::Value::scalar(tint);
  settings.m_data.push_back(new TSeExprAttributes::RenderData(std::move(attributes)));
  return settings;
}
TRasterFP surfacePixels(SeExprSurfaceFx &fx, double frame, const TRenderSettings &settings,
                        int width = 32, TPointD position = TPointD(-16, -16)) {
  TRasterFP raster(width, 32); raster->fill(TPixelF::White);
  TTile tile(raster, position);
  fx.doCompute(tile, frame, settings);
  return raster;
}
void surfaceAttributes() {
  TRenderSettings settings;
  TSeExprAttributes::Snapshot outer, inner;
  outer.attributes["user::gain"] = TSeExpr::Value::scalar(.1);
  outer.options["user::gain"] = TSeExpr::Value::scalar(.75);
  inner.attributes["user::gain"] = TSeExpr::Value::scalar(.2);
  inner.attributes["user::vector"] = TSeExpr::Value::vector(.25, .5, .75);
  settings.m_data.push_back(new TSeExprAttributes::RenderData(outer));
  settings.m_data.push_back(new TSeExprAttributes::RenderData(inner));
  const auto snapshot = TSeExprAttributes::snapshot(settings);
  inner.attributes["user::vector"] = TSeExpr::Value::vector(9, 9, 9);
  std::vector<TSeExpr::Variable> variables;
  TSeExprAttributes::appendVariables(variables, snapshot,
      {"$label='$quoted::ignore'; $other=\"$quoted::alsoIgnore\"; $user::gain+$user::vector[1]+$missing::value[2] # $ignored::comment\n"});
  surfaceCheck(variables.size() == 3, "Attribute scanner bound a comment or lost a namespace");
  TSeExpr::Program program; TSeExpr::Options options; options.resultType = TSeExpr::Type::Scalar;
  surfaceCheck(program.compile("$user::gain[0]+$user::vector[1]+$missing::value[2]", variables, {}, options),
               "Namespaced attribute program did not compile");
  TSeExpr::Value result;
  surfaceCheck(program.evaluate(result) && std::abs(result.number[0] - .7) < 1e-12,
               "Attribute precedence, vector binding, missing-zero or immutable snapshot failed");
  {
    const std::string source = "[$user::gain[0],$user::gain[1],$user::gain[2]]";
    TSeExpr::Program indexed; std::vector<TSeExpr::Variable> bindings;
    TSeExprAttributes::appendVariables(bindings, snapshot, {source});
    surfaceCheck(indexed.compile(source, bindings) && indexed.evaluate(result) &&
        result.number == std::vector<double>({.2,.2,.2}),
        "A scalar renderer attribute was not promoted to all three components");
    auto optionOnly = snapshot; optionOnly.attributes.erase("user::gain");
    bindings.clear(); TSeExprAttributes::appendVariables(bindings, optionOnly, {source});
    surfaceCheck(indexed.compile(source, bindings) && indexed.evaluate(result) &&
        result.number == std::vector<double>({.75,.75,.75}),
        "A scalar renderer option was not promoted to all three components");
  }
  std::vector<TSeExpr::Variable> dotted;
  TSeExprAttributes::appendVariables(dotted, {}, {"$shot.render::light.gain"});
  surfaceCheck(dotted.size() == 1 && dotted[0].name == "shot.render::light.gain" &&
      dotted[0].value.number == std::vector<double>({0,0,0}),
      "Dotted namespaced fallback was truncated");
  const auto identity = snapshot.identity();
  const auto explicitSnapshot = TSeExprAttributes::snapshot(settings,
      {{"user::gain", TSeExpr::Value::scalar(.9)}});
  surfaceCheck(explicitSnapshot.options.at("user::gain").number[0] == .9 &&
                   identity != explicitSnapshot.identity(), "Explicit option value lost its identity");

  SeExprAttributeFx provider;
  surfaceCheck(provider.getFxType() == "STD_seexprAttributeFx" &&
      provider.getParams()->getParamCount() == 8, "Native attribute authoring is incomplete");
  surfaceParam<TDoubleParam>(provider, "value")->setValue(0, .2);
  surfaceParam<TDoubleParam>(provider, "value")->setValue(8, .8);
  std::unique_ptr<TFx> copy(provider.clone(false));
  surfaceCheck(surfaceParam<TDoubleParam>(*copy, "value")->isKeyframe(0) &&
      surfaceParam<TDoubleParam>(*copy, "value")->getValue(8) == .8,
      "Attribute provider clone lost native animation");
}

void surfaceRender(const QString &directory) {
  const QString path = directory + "/surface.glb";
  const auto bytes = triangleGlb(0, true);
  QFile file(path);
  surfaceCheck(file.open(QIODevice::WriteOnly), "Cannot write surface geometry fixture");
  surfaceCheck(file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) == qint64(bytes.size()),
               "Surface geometry fixture write was incomplete");
  file.close();
  TFxP modelOwner = new GlbModelFx;
  auto &model = *static_cast<GlbModelFx *>(modelOwner.getPointer());
  surfaceParam<TStringParam>(model, "modelFile")->setValue(path.toStdWString());
  surfaceParam<TIntEnumParam>(model, "colorMode")->setValue(1);
  TFxP surfaceOwner = new SeExprSurfaceFx;
  auto &fx = *static_cast<SeExprSurfaceFx *>(surfaceOwner.getPointer());
  fx.getInputPort("Source")->setFx(&model);
  surfaceCheck(!fx.isZerary() && fx.getInputPortCount() == 1 &&
      fx.supportsSeExprSurfaceDocument(),
               "Surface effect does not expose its native 3D source");
  const QJsonObject amount{{"id", "amount"}, {"name", "amount"}, {"type", "scalar"},
      {"value", .25}, {"min", 0}, {"max", 1}};
  surfaceDocument(fx, "[($Po[0]+1)/2,$user::tint[0],$amount]", "0.5", "0", {amount}, "[1,1,1]");
  auto *control = surfaceParam<TDoubleParam>(fx, "seexprControl_amount__value");
  control->setValue(0, .25); control->setValue(6, .75);
  const auto settings = surfaceSettings(.4);
  const auto scene = fx.get3DRenderScene(0, nullptr, nullptr, nullptr, &settings);
  surfaceCheck(scene && scene->surface && !scene->triangles.empty(),
               "Surface expression did not stay in the unprojected renderer path");
  surfaceCheck(fx.get3DRenderScene(0, nullptr, nullptr, nullptr, &settings) == scene,
               "Identical surface request did not reuse its geometry cache");
  {
    Transform3DFx transform;
    transform.getInputPort("Source")->setFx(&fx);
    surfaceParam<TDoubleParam>(transform, "positionX")->setDefaultValue(.5);
    const auto transformed = transform.get3DRenderGeometry(0, nullptr, nullptr, &settings);
    surfaceCheck(transformed && transformed->surface && transform.hasSurface3D() &&
        transformed->bounds[0] > scene->bounds[0] + 49,
        "3D Transformer dropped its procedural geometry request");
    surfaceCheck(transform.getMemoryRequirement(TRectD(0,0,8,8), 0, settings) >= 256,
        "3D Transformer understated procedural worker memory");
    ThreePointLightFx light;
    light.getInputPort("GLB")->setFx(&fx);
    surfaceCheck(light.getMemoryRequirement(TRectD(0,0,8,8), 0, settings) >= 256,
        "Lighting understated procedural worker memory");
  }
  {
    SeExprSurfaceFx nested;
    nested.getInputPort("Source")->setFx(&fx);
    bool rejected = false;
    try { nested.get3DRenderScene(0, nullptr, nullptr, nullptr, &settings); }
    catch (const std::exception &error) { rejected = std::string(error.what()).find("one Expression Surface") != std::string::npos; }
    surfaceCheck(rejected, "A second surface binding silently discarded an upstream shader");
  }
  const auto changedSettings = surfaceSettings(.7);
  surfaceCheck(fx.getAlias(0, settings) != fx.getAlias(0, changedSettings) &&
      fx.get3DRenderScene(0, nullptr, nullptr, nullptr, &changedSettings) != scene,
      "Attribute changes reused an incompatible evaluator or projected scene");
  SeExprAttributeFx provider;
  provider.getInputPort("Source")->setFx(&fx);
  surfaceParam<TStringParam>(provider, "name")->setValue(L"tint");
  surfaceParam<TDoubleParam>(provider, "value")->setDefaultValue(.9);
  const auto scoped = provider.get3DRenderScene(0, nullptr, nullptr, nullptr, &settings);
  auto scopedWorker = scoped->surface->createEvaluator(nullptr);
  otglb::SurfaceSample scopedSample;
  surfaceCheck(std::abs(scopedWorker->shade(scopedSample).color[1] - .9) < 1e-12,
      "Native attribute provider failed to pass its nearest binding into a 3D worker");
  {
    SeExprSurfaceFx indexed;
    indexed.getInputPort("Source")->setFx(&model);
    surfaceDocument(indexed, "[$user::tint[0],$user::tint[1],$user::tint[2]]", "1", "0");
    const auto indexedScene = indexed.get3DRenderScene(0, nullptr, nullptr, nullptr, &settings);
    auto worker = indexedScene->surface->createEvaluator(nullptr);
    const auto color = worker->shade(otglb::SurfaceSample());
    surfaceCheck(color.color == std::array<double,3>{{.4,.4,.4}},
        "Actual 3D worker scalar-attribute indexing differs from upstream tuple promotion");
    surfaceCheck(color.coloredOpacity == std::array<double,3>{{1,1,1}},
        "A legacy surface document without RGB opacity did not default to unity");
  }
  {
    SeExprSurfaceFx colored;
    colored.getInputPort("Source")->setFx(&model);
    surfaceDocument(colored, "[0.8,0.6,0.4]", "0.5", "0", {}, "[$user::tint[0],1,0]");
    const auto coloredScene = colored.get3DRenderScene(0, nullptr, nullptr, nullptr, &settings);
    auto worker = coloredScene->surface->createEvaluator(nullptr);
    const auto value = worker->shade(otglb::SurfaceSample());
    surfaceCheck(value.coloredOpacity == std::array<double,3>{{.4,1,0}} &&
        value.opacity == .5, "RGB opacity lost its namespaced binding or scalar-alpha multiplier");
    const auto pixels = surfacePixels(colored, 0, settings);
    int count = 0;
    for (int y = 0; y < pixels->getLy(); ++y) for (int x = 0; x < pixels->getLx(); ++x) {
      const auto &pixel = pixels->pixels(y)[x];
      if (std::abs(pixel.m - .5f) > .00001) continue;
      ++count;
      surfaceCheck(std::abs(pixel.r - .16f) < .00001 &&
          std::abs(pixel.g - .3f) < .00001 && std::abs(pixel.b) < .00001,
          "Actual native surface pixels did not apply RGB opacity and alpha exactly once");
    }
    surfaceCheck(count > 20, "RGB opacity fixture did not cover enough interior surface samples");
    const auto previousAlias = colored.getAlias(0, settings);
    surfaceDocument(colored, "[0.8,0.6,0.4]", "0.5", "0", {}, "[0,0,1]");
    surfaceCheck(previousAlias != colored.getAlias(0, settings) &&
        colored.get3DRenderScene(0, nullptr, nullptr, nullptr, &settings) != coloredScene,
        "Changing only RGB opacity reused an incompatible prepared geometry/evaluator");
    const auto changed = surfacePixels(colored, 0, settings);
    const auto center = changed->pixels(16)[16];
    surfaceCheck(std::abs(center.r) < .00001 && std::abs(center.g) < .00001 &&
        std::abs(center.b - .2f) < .00001 && std::abs(center.m - .5f) < .00001,
        "An RGB-opacity source change did not update actual rendered pixels");
    surfaceDocument(colored, "[0.8,0.6,0.4]", "0.5", "0", {}, "[-1,2,0.25]");
    const auto clamped = colored.get3DRenderScene(0, nullptr, nullptr, nullptr, &settings)
        ->surface->createEvaluator(nullptr)->shade(otglb::SurfaceSample());
    surfaceCheck(clamped.coloredOpacity == std::array<double,3>{{0,1,.25}},
        "RGB opacity components were not clamped independently");
  }
  const auto image = surfacePixels(fx, 0, settings);
  double minimum = 10, maximum = -10; int covered = 0;
  for (int y = 0; y < image->getLy(); ++y) for (int x = 0; x < image->getLx(); ++x) {
    const auto &pixel = image->pixels(y)[x];
    if (std::abs(pixel.m - .5f) > .00001) continue;
    ++covered; minimum = std::min(minimum, double(pixel.r)); maximum = std::max(maximum, double(pixel.r));
    surfaceCheck(std::abs(pixel.g - .2f) < .00001 && std::abs(pixel.b - .125f) < .00001,
                 "Per-sample color, native control or opacity result is incorrect");
  }
  surfaceCheck(covered > 20 && maximum - minimum > .2,
               "Surface was recolored at a centroid instead of evaluated per sample");
  for (int offset = 0; offset < 32; offset += 8) {
    const auto tile = surfacePixels(fx, 0, settings, 8, TPointD(-16 + offset, -16));
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 8; ++x)
      surfaceCheck(tile->pixels(y)[x] == image->pixels(y)[x + offset], "Surface has a tile seam");
  }
  std::vector<std::future<TRasterFP>> workers;
  for (int index = 0; index < 4; ++index)
    workers.push_back(std::async(std::launch::async, [&]() { return surfacePixels(fx, 0, settings); }));
  for (auto &worker : workers) {
    const auto result = worker.get();
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x)
      surfaceCheck(result->pixels(y)[x] == image->pixels(y)[x], "Workers shared mutable surface evaluation state");
  }
  std::unique_ptr<TFx> clone(fx.clone(false));
  auto *cloned = dynamic_cast<SeExprSurfaceFx *>(clone.get());
  surfaceCheck(cloned && surfaceParam<TDoubleParam>(*cloned, "seexprControl_amount__value")->getValue(6) == .75,
               "Surface clone lost dynamic native control keys");
  const QString saved = directory + "/surface.tnz";
  { TOStream stream(TFilePath(saved.toStdWString())); stream << &fx; }
  TIStream stream(TFilePath(saved.toStdWString())); TPersist *persisted = nullptr; stream >> persisted;
  std::unique_ptr<TPersist> owner(persisted);
  auto *restored = dynamic_cast<SeExprSurfaceFx *>(persisted);
  surfaceCheck(restored && restored->seExprDocumentParam()->getValue() == fx.seExprDocumentParam()->getValue() &&
      surfaceParam<TDoubleParam>(*restored, "seexprControl_amount__value")->getValue(6) == .75,
      "Surface save/reopen lost source or native control animation");

  surfaceParam<TIntEnumParam>(model, "projection")->setValue(1);
  surfaceDocument(fx, "$Cs", "$a", "0");
  const auto before = fx.get3DRenderGeometry(0, nullptr, nullptr, &settings);
  surfaceDocument(fx, "$Cs", "$a", "0.5");
  const auto after = fx.get3DRenderGeometry(0, nullptr, nullptr, &settings);
  surfaceCheck(after && before && after->bounds[0] < before->bounds[0] &&
      after->bounds[2] > before->bounds[2], "Displacement did not change geometry before projection/bounds");
  int stop = 1; auto canceledSettings = settings; canceledSettings.m_isCanceled = &stop;
  const auto blank = surfacePixels(fx, 0, canceledSettings);
  surfaceCheck(blank->pixels(16)[16].m == 0, "Canceled surface exposed stale output");
  surfaceDocument(fx, "$Cs", "$a", "2000000");
  bool failed = false;
  try { surfacePixels(fx, 0, settings); } catch (const TException &) { failed = true; }
  surfaceCheck(failed, "Displacement resource failure became a successful blank frame");
  surfaceDocument(fx, "$Cs", "$a", "missing_function()");
  std::string error;
  surfaceCheck(!fx.validateSeExprDocument(fx.seExprDocumentParam()->getValue(), 0, error) &&
      error.find("Displacement") != std::string::npos, "Editor accepted an invalid displacement program");
  surfaceDocument(fx, "$Cs", "$a", "1/0");
  std::string preview;
  surfaceCheck(!fx.previewSeExprDocument(fx.seExprDocumentParam()->getValue(), 0, preview, error),
      "Editor preview silently skipped a displacement runtime error");
  surfaceDocument(fx, "$Cs", "0.5", "0", {}, "[0.25,0.5,1]");
  surfaceCheck(fx.previewSeExprDocument(fx.seExprDocumentParam()->getValue(), 0, preview, error) &&
      preview.find("opacity RGB [0.25, 0.5, 1]") != std::string::npos,
      "Surface editor preview omitted its evaluated RGB opacity");
  surfaceDocument(fx, "$Cs", "1", "0", {}, "missing_function()");
  std::vector<TSeExpr::Error> diagnostics;
  surfaceCheck(!fx.validateSeExprDocument(fx.seExprDocumentParam()->getValue(), 0, error, &diagnostics) &&
      error.find("Opacity RGB") != std::string::npos && !diagnostics.empty(),
      "Surface editor accepted an invalid RGB-opacity program or lost its source diagnostic");
  surfaceDocument(fx, "$Cs", "1", "0", {}, "[1/0,1,1]");
  surfaceCheck(!fx.previewSeExprDocument(fx.seExprDocumentParam()->getValue(), 0, preview, error) &&
      error.find("Opacity RGB") != std::string::npos,
      "Surface editor preview silently skipped an RGB-opacity runtime error");
  failed = false;
  try { surfacePixels(fx, 0, settings); } catch (const TException &) { failed = true; }
  surfaceCheck(failed, "An RGB-opacity evaluation failure became a successful blank native render");
  auto malformed = SeExprFx::parseDocument(fx.seExprDocumentParam()->getValue()).json;
  malformed.insert("opacity", true);
  const auto malformedText = QString::fromUtf8(QJsonDocument(malformed).toJson(QJsonDocument::Compact)).toStdWString();
  surfaceCheck(!fx.validateSeExprDocument(malformedText, 0, error),
      "A non-string RGB-opacity source was silently replaced with its default");
}

void surfaceImagePlane() {
  TFxP sourceOwner = new SurfaceRasterSource;
  auto &source = *static_cast<SurfaceRasterSource *>(sourceOwner.getPointer());
  TFxP planeOwner = new ImagePlaneFx;
  auto &plane = *static_cast<ImagePlaneFx *>(planeOwner.getPointer());
  plane.getInputPort("Source")->setFx(&source);
  SeExprSurfaceFx surface;
  surface.getInputPort("Source")->setFx(&plane);
  surfaceDocument(surface, "$Cs", "$a", "0");
  TRenderSettings settings; settings.m_bpp = 128;
  const auto bounds = surface.get3DRenderGeometry(0, nullptr, nullptr, &settings);
  surfaceCheck(bounds && !bounds->triangles.empty() && source.computes == 0,
      "Surface bounds evaluated an Image Plane raster outside its render session");
  TRectD requested(-1, -1, 1, 1);
  surface.doDryCompute(requested, 0, settings);
  surfaceCheck(source.planned == 1 && source.plannedBounds == TRectD(0, 0, 8, 8) &&
      source.computes == 0, "Predictive rendering did not plan the complete image-plane source");
  surfaceCheck(surface.getMemoryRequirement(requested, 0, settings) >= 256,
      "Surface understated its bounded transparent-fragment workspace");
  TRasterFP output(8, 8); TTile tile(output, TPointD(-4, -4));
  surface.doCompute(tile, 0, settings);
  const auto original = output->pixels(3)[3];
  surfaceCheck(source.computes == 1 && std::abs(original.r - .2) < .00003 &&
      std::abs(original.g - .1) < .00003 && std::abs(original.m - .5) < .00003,
      "Image Plane surface did not preserve straight texture RGB/alpha exactly once");
  surfaceDocument(surface, "$Cs", "1", "0");
  surface.doCompute(tile, 0, settings);
  const auto opaque = output->pixels(3)[3];
  surfaceCheck(std::abs(opaque.r - .4) < .00003 && std::abs(opaque.m - 1) < .00001,
      "Surface opacity was multiplied by input alpha a second time");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory;
  try {
    surfaceCheck(directory.isValid(), "Surface test directory is unavailable");
    surfaceAttributes(); surfaceRender(directory.path()); surfaceImagePlane();
    std::cout << "SeExpr native surfaces and attributes: PASS\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << "SeExpr native surfaces: FAIL: " << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) {
    std::cerr << "SeExpr native surfaces: FAIL: " << error.what() << '\n';
  }
  return 1;
}
