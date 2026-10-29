// Included after the shared native authoring implementation. The surface node
// inherits its complete schema, animation, undo, clone and persistence behavior.
#include "t3dsource.h"
#include "tseexprattributes.h"
#include <type_traits>

namespace {
class SeExprSurfaceEvaluator final : public otglb::SurfaceEvaluator {
  std::array<TSeExpr::Program, 4> m_programs;
  int m_coordinates = 0;
  double m_limit = 0;

  void sample(const otglb::SurfaceSample &value) {
    const auto &position = m_coordinates == 1 ? value.worldPosition
        : m_coordinates == 2 ? value.cameraPosition : value.objectPosition;
    const auto &normal = m_coordinates == 1 ? value.worldNormal
        : m_coordinates == 2 ? value.cameraNormal : value.objectNormal;
    for (auto &program : m_programs) {
      auto scalar = [&](const char *name, double component) {
        if (!program.setScalar(name, component)) throw std::runtime_error(program.errorMessage());
      };
      auto vector = [&](const char *name, const std::array<double, 3> &components) {
        if (!program.setVector(name, components[0], components[1], components[2]))
          throw std::runtime_error(program.errorMessage());
      };
      vector("P", position); vector("N", normal);
      vector("Po", value.objectPosition); vector("Pw", value.worldPosition);
      vector("Pc", value.cameraPosition); vector("No", value.objectNormal);
      vector("Nw", value.worldNormal); vector("Nc", value.cameraNormal);
      const std::array<double, 3> color{{value.baseColor[0], value.baseColor[1], value.baseColor[2]}};
      vector("Cs", color); vector("src", color);
      vector("Os", {{value.baseColor[3], value.baseColor[3], value.baseColor[3]}});
      scalar("x", position[0]); scalar("y", position[1]); scalar("z", position[2]);
      scalar("u", value.uv[0]); scalar("v", value.uv[1]);
      scalar("r", color[0]); scalar("g", color[1]); scalar("b", color[2]);
      scalar("a", value.baseColor[3]); scalar("frame", value.frame);
      scalar("time", value.time);
    }
  }

public:
  SeExprSurfaceEvaluator(const std::array<std::string, 4> &sources,
      const std::vector<TSeExpr::Variable> &variables,
      const std::vector<TSeExpr::Function> &functions, TSeExpr::Backend backend,
      int coordinates, double limit, const int *canceled)
      : m_coordinates(coordinates), m_limit(limit) {
    for (std::size_t channel = 0; channel < m_programs.size(); ++channel) {
      TSeExpr::Options options;
      options.backend = backend;
      options.resultType = channel == 0 || channel == 3 ? TSeExpr::Type::Vector : TSeExpr::Type::Scalar;
      options.cancelled = [canceled]() { return canceled && *canceled; };
      if (!m_programs[channel].compile(sources[channel], variables, functions, options))
        throw std::runtime_error(std::string(channel == 0 ? "Surface RGB: "
            : channel == 1 ? "Surface alpha: "
            : channel == 2 ? "Displacement: " : "Opacity RGB: ") +
            m_programs[channel].errorMessage());
    }
  }
  double displace(const otglb::SurfaceSample &value) override {
    sample(value);
    TSeExpr::Value result;
    if (!m_programs[2].evaluate(result))
      throw std::runtime_error("Displacement: " + m_programs[2].errorMessage());
    if (std::abs(result.number[0]) > m_limit)
      throw std::runtime_error("Displacement exceeds its declared object-unit limit");
    return result.number[0];
  }
  otglb::SurfaceColor shade(const otglb::SurfaceSample &value) override {
    sample(value);
    TSeExpr::Value rgb, alpha, opacity;
    if (!m_programs[0].evaluate(rgb))
      throw std::runtime_error("Surface RGB: " + m_programs[0].errorMessage());
    if (!m_programs[1].evaluate(alpha))
      throw std::runtime_error("Surface alpha: " + m_programs[1].errorMessage());
    if (!m_programs[3].evaluate(opacity))
      throw std::runtime_error("Opacity RGB: " + m_programs[3].errorMessage());
    otglb::SurfaceColor result;
    result.color = {{rgb.number[0], rgb.number[1], rgb.number[2]}};
    result.opacity = unit(alpha.number[0]);
    result.coloredOpacity = {{unit(opacity.number[0]), unit(opacity.number[1]), unit(opacity.number[2])}};
    return result;
  }
};
}  // namespace

class SeExprSurfaceFx final : public SeExprNativeFx<TStandardRasterFx>,
                              public T3DRenderSource {
  FX_PLUGIN_DECLARATION(SeExprSurfaceFx)
  using Base = SeExprNativeFx<TStandardRasterFx>;
  T3DSourcePort m_geometry;
  TIntEnumParamP m_coordinates;
  TDoubleParamP m_displacementLimit;

  static QString displacement(const SeExprFx::Document &document) {
    demand(document.json.value("displacement").isString(),
           "surface displacement source must be a string");
    return document.json.value("displacement").toString();
  }
  static QString opacity(const SeExprFx::Document &document) {
    demand(!document.json.contains("opacity") || document.json.value("opacity").isString(),
           "surface RGB opacity source must be a string");
    return document.json.value("opacity").toString("[1,1,1]");
  }

protected:
  std::vector<TSeExpr::Variable> variablesFor(const SeExprFx::Document &document,
      double frame, bool linear = false, double gamma = 2.2) const override {
    auto variables = Base::variablesFor(document, frame, linear, gamma);
    const std::set<std::string> vectors{"P", "N", "Po", "Pw", "Pc", "No", "Nw", "Nc", "Cs", "Os"};
    for (const auto &variable : variables)
      demand(!vectors.count(variable.name) && variable.name != "z",
             "surface controls cannot replace a position, normal or material binding");
    for (const auto &name : vectors) {
      const bool color = name == "Cs" || name == "Os";
      variables.push_back({name, color ? TSeExpr::Value::vector(1, 1, 1)
          : TSeExpr::Value::vector(0, 0, name[0] == 'N' ? 1 : 0)});
    }
    for (auto &variable : variables) {
      if (variable.name == "a" || variable.name == "r" || variable.name == "g" || variable.name == "b")
        variable.value = TSeExpr::Value::scalar(1);
      else if (variable.name == "src") variable.value = TSeExpr::Value::vector(1, 1, 1);
    }
    variables.push_back({"z", TSeExpr::Value::scalar(0)});
    TSeExprAttributes::appendVariables(variables, {},
        {document.rgb.toStdString(), document.alpha.toStdString(), displacement(document).toStdString(), opacity(document).toStdString()});
    return variables;
  }
  std::vector<TSeExpr::Function> functionsFor(const SeExprFx::Document &document,
      double frame, InputSampler *, bool linear = false, double gamma = 2.2) const override {
    auto functions = Base::functionsFor(document, frame, nullptr, linear, gamma);
    functions.erase(std::remove_if(functions.begin(), functions.end(),
        [](const TSeExpr::Function &function) {
          return function.name == "sample_rgb" || function.name == "sample_alpha";
        }), functions.end());
    return functions;
  }

  std::shared_ptr<const otglb::SurfaceRequest> request(
      double frame, const TRenderSettings &settings) const {
    demand(m_schemaError.empty(), m_schemaError.c_str());
    const auto document = SeExprFx::parseDocument(m_document->getValue());
    const std::array<std::string, 4> sources{{document.rgb.toStdString(),
        document.alpha.toStdString(), displacement(document).toStdString(), opacity(document).toStdString()}};
    auto variables = variablesFor(document, frame);
    const auto attributes = TSeExprAttributes::snapshot(settings);
    TSeExprAttributes::appendVariables(variables, attributes,
                                      {sources[0], sources[1], sources[2], sources[3]});
    std::size_t bindingBytes = 0;
    for (const auto &variable : variables)
      bindingBytes += variable.name.size() + variable.value.text.size() +
                      variable.value.number.size() * sizeof(double);
    demand(bindingBytes <= 1024 * 1024, "surface bindings exceed the one MiB snapshot limit");
    const auto functions = functionsFor(document, frame, nullptr);
    const auto backend = document.backend == "llvm" ? TSeExpr::Backend::LLVM
                                                    : TSeExpr::Backend::Interpreter;
    const int coordinates = m_coordinates->getValue();
    demand(coordinates >= 0 && coordinates <= 2, "unknown surface coordinate space");
    const double limit = finite(m_displacementLimit->getValue(frame), "displacement limit must be finite");
    demand(limit >= 0 && limit <= 1000000000.0, "displacement limit is outside zero to one billion object units");
    std::string identity = Base::getAlias(frame, settings) + attributes.identity();
    struct Entry { std::string identity; std::shared_ptr<const otglb::SurfaceRequest> value; };
    static thread_local std::list<Entry> cache;
    const auto found = std::find_if(cache.begin(), cache.end(),
        [&](const Entry &entry) { return entry.identity == identity; });
    if (found != cache.end()) {
      cache.splice(cache.begin(), cache, found);
      return cache.front().value;
    }
    // Preparation validates every expression before any geometry or pixels can
    // be cached. The renderer creates fresh evaluators per preparation/worker.
    SeExprSurfaceEvaluator validated(sources, variables, functions, backend,
                                     coordinates, limit, settings.m_isCanceled);
    auto result = std::make_shared<otglb::SurfaceRequest>();
    result->identity = std::move(identity);
    result->frame = frame + 1;
    const double fps = m_fps->getValue(frame);
    demand(std::isfinite(fps) && fps > 0, "surface time rate must be positive and finite");
    result->time = frame / fps;
    result->createEvaluator = [sources, variables, functions, backend, coordinates, limit](const int *canceled) {
      return std::unique_ptr<otglb::SurfaceEvaluator>(new SeExprSurfaceEvaluator(
          sources, variables, functions, backend, coordinates, limit, canceled));
    };
    cache.push_front({result->identity, result});
    if (cache.size() > 8) cache.pop_back();
    return result;
  }

  bool previewAtOrigin(const std::wstring &text, double frame,
      TSeExpr::Value &rgb, double &alpha, double &distance,
      std::array<double, 3> &opacityResult, std::string &error) const {
    try {
      if (!validateSeExprDocument(text, frame, error)) return false;
      const auto document = SeExprFx::parseDocument(text);
      const auto backend = document.backend == "llvm" ? TSeExpr::Backend::LLVM : TSeExpr::Backend::Interpreter;
      const std::array<std::string, 4> sources{{document.rgb.toStdString(),
          document.alpha.toStdString(), displacement(document).toStdString(), opacity(document).toStdString()}};
      SeExprSurfaceEvaluator evaluator(sources, variablesFor(document, frame),
          functionsFor(document, frame, nullptr), backend, m_coordinates->getValue(),
          m_displacementLimit->getValue(frame), nullptr);
      otglb::SurfaceSample sample;
      sample.objectNormal = sample.worldNormal = sample.cameraNormal = {{0, 0, 1}};
      sample.baseColor = {{1, 1, 1, 1}}; sample.uv = {{.5, .5}};
      sample.frame = frame + 1; sample.time = frame / m_fps->getValue(frame);
      distance = evaluator.displace(sample);
      const auto color = evaluator.shade(sample);
      rgb = TSeExpr::Value::vector(color.color[0], color.color[1], color.color[2]);
      opacityResult = color.coloredOpacity;
      alpha = color.opacity * std::max({opacityResult[0], opacityResult[1], opacityResult[2]});
      error.clear(); return true;
    } catch (const TException &failure) {
      error = QString::fromStdWString(failure.getMessage()).toStdString(); return false;
    } catch (const std::exception &failure) { error = failure.what(); return false; }
  }

public:
  bool hasSurface3D() const override { return true; }
  bool supportsSeExprSurfaceDocument() const override { return true; }
  SeExprSurfaceFx() : Base(true), m_coordinates(0, "Object"),
                      m_displacementLimit(1000000.0) {
    addInputPort("Source", m_geometry);
    bindParam(this, "coordinates", m_coordinates);
    bindParam(this, "displacementLimit", m_displacementLimit);
    m_coordinates->addItem(1, "World"); m_coordinates->addItem(2, "Camera");
    m_displacementLimit->setValueRange(0, 1000000000.0);
    QJsonObject document = SeExprFx::parseDocument(m_document->getValue()).json;
    document.insert("rgb", "$Cs"); document.insert("alpha", "$a");
    document.insert("displacement", "0");
    document.insert("opacity", "[1,1,1]");
    m_document->setValue(QString::fromUtf8(QJsonDocument(document).toJson(QJsonDocument::Compact)).toStdWString());
    for (const char *name : {"width", "height", "mix", "samplingRadius", "edgeMode", "samplingFilter", "maskChannel"})
      getParams()->getParamVar(name)->setIsHidden(true);
  }
  ~SeExprSurfaceFx() override {
    // ParamVar stores the address of each smart-pointer member. Detach these
    // derived bindings while those members are alive; the base destructor's
    // already-detached fast path must not read an ended member lifetime.
    for (const char *name : {"coordinates", "displacementLimit"})
      if (auto *parameter = getParams()->getParamVar(name))
        parameter->setParamObserver(nullptr);
  }
  bool validateSeExprDocument(const std::wstring &text, double frame,
      std::string &error, std::vector<TSeExpr::Error> *diagnostics = nullptr) const override {
    try {
      if (!Base::validateSeExprDocument(text, frame, error, diagnostics)) return false;
      const auto document = SeExprFx::parseDocument(text);
      for (bool geometry : {true, false}) {
        TSeExpr::Program program; TSeExpr::Options options;
        options.backend = document.backend == "llvm" ? TSeExpr::Backend::LLVM : TSeExpr::Backend::Interpreter;
        options.resultType = geometry ? TSeExpr::Type::Scalar : TSeExpr::Type::Vector;
        if (!program.compile((geometry ? displacement(document) : opacity(document)).toStdString(), variablesFor(document, frame),
                             functionsFor(document, frame, nullptr), options)) {
          error = std::string(geometry ? "Displacement: " : "Opacity RGB: ") + program.errorMessage();
          if (diagnostics) *diagnostics = program.errors();
          return false;
        }
      }
      error.clear(); if (diagnostics) diagnostics->clear(); return true;
    } catch (const TException &failure) {
      error = QString::fromStdWString(failure.getMessage()).toStdString(); return false;
    } catch (const std::exception &failure) { error = failure.what(); return false; }
  }
  bool sampleSeExprDocument(const std::wstring &text, double frame,
      TSeExpr::Value &rgb, double &alpha, std::string &error) const override {
    double distance = 0;
    std::array<double, 3> opacityResult{};
    return previewAtOrigin(text, frame, rgb, alpha, distance, opacityResult, error);
  }
  bool previewSeExprDocument(const std::wstring &text, double frame,
      std::string &value, std::string &error) const override {
    TSeExpr::Value rgb; double alpha = 0, distance = 0;
    std::array<double, 3> opacityResult{};
    if (!previewAtOrigin(text, frame, rgb, alpha, distance, opacityResult, error)) return false;
    std::ostringstream output; output.imbue(std::locale::classic());
    output << std::setprecision(8) << "At origin, normal +Z: RGB ["
           << rgb.number[0] << ", " << rgb.number[1] << ", " << rgb.number[2]
           << "], alpha " << alpha << ", displacement " << distance
           << ", opacity RGB [" << opacityResult[0] << ", " << opacityResult[1]
           << ", " << opacityResult[2] << ']';
    value = output.str(); return true;
  }
  std::shared_ptr<const otglb::RenderScene> get3DRenderScene(
      double frame, const int *canceled, const otglb::LightingRig *lighting = nullptr,
      const std::vector<otglb::ModelTransform> *transforms = nullptr,
      const TRenderSettings *settings = nullptr,
      const std::shared_ptr<const otglb::SurfaceRequest> &downstream = {}) const override {
    if (!m_geometry.isConnected() || (canceled && *canceled)) return {};
    if (downstream)
      throw std::runtime_error("A 3D branch supports one Expression Surface binding; combine its RGB, alpha, opacity and displacement expressions in one node");
    const auto surface = request(frame, settings ? *settings : TRenderSettings());
    return m_geometry.source()->get3DRenderScene(frame, canceled, lighting,
                                                transforms, settings, surface);
  }
  std::shared_ptr<const otglb::RenderScene> get3DRenderGeometry(
      double frame, const int *canceled,
      const std::vector<otglb::ModelTransform> *transforms = nullptr,
      const TRenderSettings *settings = nullptr,
      const std::shared_ptr<const otglb::SurfaceRequest> &downstream = {}) const override {
    if (!m_geometry.isConnected() || (canceled && *canceled)) return {};
    if (downstream)
      throw std::runtime_error("A 3D branch supports one Expression Surface binding; combine its RGB, alpha, opacity and displacement expressions in one node");
    const auto surface = request(frame, settings ? *settings : TRenderSettings());
    return m_geometry.source()->get3DRenderGeometry(frame, canceled, transforms, settings, surface);
  }
  bool doGetBBox(double frame, TRectD &box, const TRenderSettings &settings) override {
    box = TRectD();
    if (!m_geometry.isConnected()) return false;
    try {
      const auto geometry = get3DRenderGeometry(frame, settings.m_isCanceled, nullptr, &settings);
      if (!geometry || geometry->triangles.empty()) return false;
      const auto &bounds = geometry->bounds;
      box = TRectD(bounds[0], bounds[1], bounds[2], bounds[3]);
      return true;
    } catch (const TException &) { box = TRectD(-500, -500, 500, 500); return true; }
      catch (const std::exception &) { box = TRectD(-500, -500, 500, 500); return true; }
  }
  void dryCompute3D(double frame, const TRenderSettings &settings) const override {
    if (m_geometry.isConnected()) m_geometry.source()->dryCompute3D(frame, settings);
  }
  void doDryCompute(TRectD &, double frame, const TRenderSettings &settings) override {
    dryCompute3D(frame, settings);
  }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool toBeComputedInLinearColorSpace(bool, bool) const override { return false; }
  int getMemoryRequirement(const TRectD &rect, double, const TRenderSettings &) override {
    if (rect.isEmpty()) return 0;
    const double bytes = std::ceil(rect.getLx()) * std::ceil(rect.getLy()) * 16.0;
    return int(std::min(double(std::numeric_limits<int>::max()),
        256.0 + std::ceil(bytes / (1024 * 1024))));
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    tile.getRaster()->clear();
    if (!m_geometry.isConnected() || canceled(settings.m_isCanceled)) return;
    try {
      const auto geometry = get3DRenderScene(frame, settings.m_isCanceled, nullptr, nullptr, &settings);
      if (!geometry || geometry->triangles.empty()) return;
      otglb::RenderTile request;
      request.width = tile.getRaster()->getLx(); request.height = tile.getRaster()->getLy();
      request.x = tile.m_pos.x; request.y = tile.m_pos.y;
      const auto &a = settings.m_affine;
      request.affine = {{a.a11, a.a12, a.a13, a.a21, a.a22, a.a23}};
      if (!request.width || !request.height) return;
      const int height = request.height;
      const int band = std::max(1, std::min(32, 512 * 1024 / request.width));
      for (int offset = 0; offset < height; offset += band) {
        request.height = std::min(band, height - offset);
        request.y = tile.m_pos.y + offset;
        const auto pixels = otglb::renderTile(*geometry, request, settings.m_isCanceled);
        if (pixels.empty()) { tile.getRaster()->clear(); return; }
        auto copy = [&](auto raster) {
          using Pixel = typename std::remove_reference<decltype(*raster->pixels(0))>::type;
          RasterLock lock(raster);
          for (int y = 0; y < request.height; ++y)
            for (int x = 0; x < request.width; ++x) {
              const auto &p = pixels[std::size_t(y) * request.width + x];
              raster->pixels(y + offset)[x] = converted(TPixelD(p.r, p.g, p.b, p.alpha), static_cast<Pixel *>(nullptr));
            }
        };
        if (TRasterFP output = tile.getRaster()) copy(output);
        else if (TRaster64P output = tile.getRaster()) copy(output);
        else if (TRaster32P output = tile.getRaster()) copy(output);
        else throw std::runtime_error("Unsupported surface output pixel type");
      }
    } catch (const std::exception &failure) {
      tile.getRaster()->clear();
      throw TException("Expression Surface: " + std::string(failure.what()));
    } catch (const TException &) { tile.getRaster()->clear(); throw; }
  }
};

FX_PLUGIN_IDENTIFIER(SeExprSurfaceFx, "seexprSurfaceFx")
