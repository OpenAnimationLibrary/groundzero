#include "../seexprfx.cpp"
#include "tmacrofx.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <future>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
TDoubleParamP numeric(TFx &fx, const std::string &name) {
  TParam *parameter = fx.getParams()->getParam(name);
  require(parameter && dynamic_cast<TDoubleParam *>(parameter), "numeric fixture parameter is missing");
  return TParamP(parameter);
}
void set(TFx &fx, const std::string &name, double value) { numeric(fx, name)->setDefaultValue(value); }
void choice(TFx &fx, const std::string &name, int value) {
  auto *parameter = dynamic_cast<TIntEnumParam *>(fx.getParams()->getParam(name));
  require(parameter, "choice fixture parameter is missing"); parameter->setValue(value);
}
QJsonObject scalar(const char *id, double value, bool integer = false) {
  return {{"id", id}, {"name", id}, {"type", "scalar"}, {"value", value},
          {"min", -20}, {"max", 20}, {"integer", integer}};
}
void document(TFx &fx, const QString &rgb, const QString &alpha = "1", const QJsonArray &controls = {}) {
  auto *native = dynamic_cast<TFxSeExprSource *>(&fx);
  require(native, "native expression interface is missing");
  const QJsonObject object{{"version", 1}, {"rgb", rgb}, {"alpha", alpha}, {"controls", controls}};
  native->seExprDocumentParam()->setValue(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)).toStdWString());
}
template <typename Pixel> TRasterPT<Pixel> rendered(
    TRasterFx &fx, int width, int height, TPointD position = TPointD(), double frame = 0,
    const TRenderSettings &settings = TRenderSettings()) {
  TRasterPT<Pixel> image(width, height); image->fill(Pixel::White);
  TTile tile; tile.setRaster(image); tile.m_pos = position;
  // The application renderer owns the base-class compute entry point. These
  // focused fixtures call only the native effects' public implementation and
  // retain checked dispatch when persistence/clone APIs return a base pointer.
  if (auto *generator = dynamic_cast<SeExprGeneratorFx *>(&fx))
    generator->doCompute(tile, frame, settings);
  else if (auto *color = dynamic_cast<SeExprColorFx *>(&fx))
    color->doCompute(tile, frame, settings);
  else
    throw std::runtime_error("render fixture requires a native expression effect");
  return image;
}
template <typename Pixel> void expect(const Pixel &pixel, double r, double g, double b, double a) {
  const double maximum = Pixel::maxChannelValue;
  const double epsilon = std::is_same<Pixel, TPixelF>::value ? 0.000003 : 1.5 / maximum;
  require(std::abs(double(pixel.r) / maximum - r) < epsilon &&
              std::abs(double(pixel.g) / maximum - g) < epsilon &&
              std::abs(double(pixel.b) / maximum - b) < epsilon &&
              std::abs(double(pixel.m) / maximum - a) < epsilon,
          "rendered pixel differs from the analytic result");
}
class Source final : public TStandardRasterFx {
  TRasterP m_image;
public:
  explicit Source(const TRasterP &image) : m_image(image) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override { return "seexpr-test-source"; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, m_image->getLx(), m_image->getLy()); return true;
  }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    tile.getRaster()->clear();
    tile.getRaster()->copy(m_image, TPoint(-int(tile.m_pos.x), -int(tile.m_pos.y)));
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override { compute(tile, frame, settings); }
};

template <typename Pixel> void checkGenerator() {
  SeExprGeneratorFx fx;
  set(fx, "width", 8); set(fx, "height", 4);
  document(fx, "[$u,$v,0.25]", "0.5");
  const auto image = rendered<Pixel>(fx, 8, 4, TPointD(-4, -2));
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x)
    expect(image->pixels(y)[x], ((x + .5) / 8) * .5, ((y + .5) / 4) * .5, .125, .5);
  for (int origin = 0; origin < 8; origin += 2) {
    const auto split = rendered<Pixel>(fx, 2, 4, TPointD(-4 + origin, -2));
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 2; ++x)
      require(split->pixels(y)[x] == image->pixels(y)[origin + x], "generator has a tile seam");
  }
  expect(rendered<Pixel>(fx, 1, 1, TPointD(9, 9))->pixels(0)[0], 0, 0, 0, 0);
  TRenderSettings scaled; scaled.m_affine = TScale(2);
  expect(rendered<Pixel>(fx, 1, 1, TPointD(-8, -4), 0, scaled)->pixels(0)[0], .015625, .03125, .125, .5);
  document(fx, "[$time/2,0,0]");
  set(fx, "fps", 12);
  expect(rendered<Pixel>(fx, 1, 1, TPointD(), 12)->pixels(0)[0], .5, 0, 0, 1);
  int stop = 1; TRenderSettings canceledSettings; canceledSettings.m_isCanceled = &stop;
  expect(rendered<Pixel>(fx, 2, 2, TPointD(), 0, canceledSettings)->pixels(0)[0], 0, 0, 0, 0);
}

template <typename Pixel> void checkColorAndSampling() {
  const double maximum = Pixel::maxChannelValue;
  TRasterPT<Pixel> input(3, 1);
  input->pixels(0)[0] = Pixel(maximum * .25, 0, 0, maximum * .5);
  input->pixels(0)[1] = Pixel(0, maximum * .5, 0, maximum * .5);
  input->pixels(0)[2] = Pixel(0, 0, maximum, maximum);
  SeExprColorFx fx; fx.getInputPort("Source")->setFx(new Source(input));
  document(fx, "$src", "$a");
  const auto identity = rendered<Pixel>(fx, 3, 1);
  for (int x = 0; x < 3; ++x) require(identity->pixels(0)[x] == input->pixels(0)[x], "identity changed premultiplied input");
  document(fx, "sample_rgb(1,0)", "sample_alpha(1,0)"); set(fx, "samplingRadius", 1);
  choice(fx, "samplingFilter", 0);
  const auto shift = rendered<Pixel>(fx, 3, 1);
  require(shift->pixels(0)[0] == input->pixels(0)[1], "bounded neighbor sampler did not shift Source");
  expect(shift->pixels(0)[2], 0, 0, 0, 0);
  choice(fx, "edgeMode", 2);
  require(rendered<Pixel>(fx, 3, 1)->pixels(0)[2] == input->pixels(0)[0], "repeat sampling did not wrap");
  document(fx, "sample_rgb(0.5,0)", "sample_alpha(0.5,0)");
  choice(fx, "samplingFilter", 1);
  const auto wrapped = rendered<Pixel>(fx, 3, 1);
  expect(wrapped->pixels(0)[2], double(input->pixels(0)[0].r) / maximum * .5, 0, .5,
         (1 + double(input->pixels(0)[0].m) / maximum) * .5);
  const auto split = rendered<Pixel>(fx, 1, 1, TPointD(2, 0));
  require(split->pixels(0)[0] == wrapped->pixels(0)[2], "repeat bilinear sampling has a tile seam");
  document(fx, "[1,1,1]", "1");
  TRasterPT<Pixel> mask(3, 1); mask->clear(); mask->pixels(0)[1] = Pixel::White;
  fx.getInputPort("Mask")->setFx(new Source(mask));
  set(fx, "mix", .5);
  const auto mixed = rendered<Pixel>(fx, 3, 1);
  require(mixed->pixels(0)[0] == input->pixels(0)[0], "zero mask did not retain Source");
  expect(mixed->pixels(0)[1], .5, .5 + double(input->pixels(0)[1].g) / maximum * .5,
         .5, .5 + double(input->pixels(0)[1].m) / maximum * .5);
  document(fx, "sample_rgb(2,0)", "1");
  bool rejected = false; try { rendered<Pixel>(fx, 1, 1); } catch (const TException &) { rejected = true; }
  require(rejected, "sampler silently exceeded its declared radius");
}

void checkControlRangeEdits() {
  SeExprGeneratorFx fx;
  auto amount = scalar("amount", 2.5);
  QJsonObject direction{{"id", "direction"}, {"name", "direction"}, {"type", "vector"},
      {"value", QJsonArray{1, 2, 3}}, {"min", -20}, {"max", 20}};
  document(fx, "[$amount,$direction[0],$direction[1]]", "1", {amount, direction});
  const std::array<std::string, 4> names{{"seexprControl_amount__value",
      "seexprControl_direction__x", "seexprControl_direction__y", "seexprControl_direction__z"}};
  const std::array<double, 4> defaults{{2.5, 1, 2, 3}};
  const std::array<double, 4> values{{12.5, 4, 5, 6}};
  std::array<TDoubleParamP, 4> parameters;
  for (int i = 0; i < 4; ++i) {
    parameters[i] = numeric(fx, names[i]);
    double minimum, maximum, step;
    require(parameters[i]->getValueRange(minimum, maximum, step) && minimum == -20 && maximum == 20,
            "initial native control range differs from its declaration");
    parameters[i]->setValue(1, values[i]);
    parameters[i]->setValue(5, -values[i]);
  }
  amount["min"] = -1; amount["max"] = 1;
  direction["min"] = -2; direction["max"] = 2;
  document(fx, "[$amount,$direction[0],$direction[1]]", "1", {amount, direction});
  for (int i = 0; i < 4; ++i) {
    const auto current = numeric(fx, names[i]);
    double minimum, maximum, step;
    const double limit = i == 0 ? 1 : 2;
    require(current.getPointer() == parameters[i].getPointer(),
            "editing a native control range replaced its parameter object");
    require(current->getValueRange(minimum, maximum, step) && minimum == -limit && maximum == limit,
            "editing scalar/vector metadata did not update the native range");
    require(current->getDefaultValue() == defaults[i] && current->isKeyframe(1) && current->isKeyframe(5) &&
                current->getValue(1) == values[i] && current->getValue(5) == -values[i],
            "editing a native control range changed its defaults or animated values");
  }
}

void checkAnimationCurveControls(const QString &directory) {
  TFxP owner = new SeExprGeneratorFx;
  auto &fx = *dynamic_cast<SeExprGeneratorFx *>(owner.getPointer());
  QJsonObject left{{"id", "left"}, {"time", 0}, {"value", 0},
                   {"inTangent", "linear"}, {"outTangent", "linear"},
                   {"foreignKeyMetadata", "retained"}};
  QJsonObject right{{"id", "right"}, {"time", 2}, {"value", 2},
                    {"inTangent", "linear"}, {"outTangent", "linear"}};
  QJsonObject motion{{"id", "motion"}, {"name", "motion"}, {"type", "animcurve"},
                     {"preInfinity", "constant"}, {"postInfinity", "linear"},
                     {"weighted", false}, {"link", "col2.x"},
                     {"foreignControlMetadata", QJsonObject{{"retained", true}}},
                     {"keys", QJsonArray{left, right}}};
  document(fx, "[motion(-1),motion(1),motion(3)]", "1", {motion});
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 0, 1, 3, 1);
  const auto animated = numeric(fx, "seexprControl_motion__kleft_value");
  animated->setValue(0, 0); animated->setValue(5, 4);
  document(fx, "[motion(0),motion(1),motion(2)]", "1", {motion});
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 5)->pixels(0)[0], 4, 3, 2, 1);

  // Move the same owned keys to a different chronological order. Array-index
  // storage would attach the left key's animation to the other key here.
  left["time"] = 2; right["time"] = 0;
  motion["keys"] = QJsonArray{right, left};
  motion["name"] = "movement"; motion["label"] = "Renamed Motion";
  document(fx, "[movement(0),movement(1),movement(2)]", "1", {motion});
  require(numeric(fx, "seexprControl_motion__kleft_value").getPointer() == animated.getPointer() &&
              animated->isKeyframe(0) && animated->isKeyframe(5),
          "reordering or renaming animation keys replaced their native owners");
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 5)->pixels(0)[0], 2, 3, 4, 1);
  const auto before = fx.getAlias(5, TRenderSettings());
  animated->setValue(5, 4.00000001);
  require(before != fx.getAlias(5, TRenderSettings()), "animation-key alias lost numeric precision");
  animated->setValue(5, 4);

  auto *native = dynamic_cast<TFxSeExprSource *>(&fx);
  const auto stored = SeExprFx::parseDocument(native->seExprDocumentParam()->getValue());
  const auto storedControl = stored.json.value("controls").toArray()[0].toObject();
  require(storedControl.value("foreignControlMetadata").toObject().value("retained").toBool() &&
              storedControl.value("keys").toArray()[1].toObject().value("foreignKeyMetadata").toString() == "retained" &&
              storedControl.value("link").toString() == "col2.x",
          "native animation schema lost link or foreign metadata");
  auto draftControl = storedControl;
  auto draftKeys = draftControl.value("keys").toArray();
  auto draftRight = draftKeys[0].toObject(); draftRight["value"] = 7;
  draftKeys[0] = draftRight; draftControl["keys"] = draftKeys;
  auto draft = stored.json; draft["controls"] = QJsonArray{draftControl};
  TSeExpr::Value draftValue; double alpha = 0; std::string error;
  require(native->sampleSeExprDocument(QString::fromUtf8(QJsonDocument(draft).toJson(QJsonDocument::Compact)).toStdWString(),
                                      0, draftValue, alpha, error) &&
              draftValue.number[0] == 7 && draftValue.number[1] == 3.5 && draftValue.number[2] == 0,
          "animation-curve draft preview ignored changed defaults or mutated keyed fields");
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 0)->pixels(0)[0], 2, 1, 0, 1);

  auto removed = motion; removed["keys"] = QJsonArray{right};
  document(fx, "[movement(0),0,0]", "1", {removed});
  require(fx.getParams()->getParamVar("seexprControl_motion__kleft_value")->isHidden() &&
              numeric(fx, "seexprControl_motion__kleft_value").getPointer() == animated.getPointer(),
          "removing an animation key discarded its archived track");
  document(fx, "[movement(0),movement(1),movement(2)]", "1", {motion});
  require(!fx.getParams()->getParamVar("seexprControl_motion__kleft_value")->isHidden() &&
              animated->getValue(5) == 4,
          "restoring an animation key did not restore its native animation");

  QJsonObject ramp{{"id", "ramp"}, {"name", "ramp"}, {"type", "curve"},
                   {"points", QJsonArray{QJsonArray{0, 0, 1}, QJsonArray{1, 1, 1}}}};
  document(fx, "[movement(0),movement(1),movement(2)]", "1", {motion, ramp});
  const auto functions = native->seExprFunctions(5);
  animated->setValue(5, 8);
  auto worker = [functions]() {
    TSeExpr::Program program; TSeExpr::Options options; options.resultType = TSeExpr::Type::Scalar;
    require(program.compile("movement(0)+ramp(0.25)", {}, functions, options),
            "shared immutable curve snapshot did not compile in a worker");
    for (int sample = 0; sample < 64; ++sample) {
      TSeExpr::Value value;
      require(program.evaluate(value) && std::abs(value.number[0] - 2.25) < 1e-10,
              "shared curve snapshots raced or followed later native edits");
    }
  };
  auto first = std::async(std::launch::async, worker);
  auto second = std::async(std::launch::async, worker);
  first.get(); second.get(); animated->setValue(5, 4);

  TFxP copy = fx.clone(false);
  expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(copy.getPointer()), 1, 1, TPointD(), 5)->pixels(0)[0], 2, 3, 4, 1);
  for (bool compressed : {false, true}) {
    const TFilePath path((directory + (compressed ? "/animation-curve-compressed.tnz" : "/animation-curve.tnz")).toStdWString());
    { TOStream stream(path, compressed); stream << owner.getPointer(); }
    TFxP restored; { TIStream stream(path); stream >> restored; }
    require(numeric(*restored, "seexprControl_motion__kleft_value")->isKeyframe(5),
            "animation-curve save/reopen discarded native keys");
    expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(restored.getPointer()), 1, 1, TPointD(), 5)->pixels(0)[0], 2, 3, 4, 1);
  }
  const TFilePath preset((directory + "/animation-curve.preset").toStdWString());
  { TOStream stream(preset); fx.savePreset(stream); }
  SeExprGeneratorFx restoredPreset; { TIStream stream(preset); restoredPreset.loadPreset(stream); }
  expect(rendered<TPixelF>(restoredPreset, 1, 1, TPointD(), 5)->pixels(0)[0], 2, 3, 4, 1);
  SeExprGeneratorFx linked; linked.linkParams(&fx);
  require(numeric(linked, "seexprControl_motion__kleft_value").getPointer() == animated.getPointer(),
          "linked animation controls did not share the native parameter");
  animated->setValue(5, 8);
  expect(rendered<TPixelF>(linked, 1, 1, TPointD(), 5)->pixels(0)[0], 2, 5, 8, 1);
  linked.unlinkParams();
  numeric(linked, "seexprControl_motion__kleft_value")->setValue(5, 9);
  require(animated->getValue(5) == 8, "unlinking an animation curve retained shared key ownership");
  animated->setValue(5, 4);

  auto invalid = motion;
  invalid["keys"] = QJsonArray{left, right};
  auto invalidDocument = stored.json; invalidDocument["controls"] = QJsonArray{invalid};
  require(!native->validateSeExprDocument(QString::fromUtf8(QJsonDocument(invalidDocument).toJson(QJsonDocument::Compact)).toStdWString(),
                                        0, error) && !error.empty(),
          "unsorted animation keys were accepted without diagnostics");
  QJsonArray tooMany;
  for (int key = 0; key < 65; ++key)
    tooMany.append(QJsonObject{{"time", key}, {"value", 0}});
  invalid["keys"] = tooMany; invalidDocument["controls"] = QJsonArray{invalid};
  require(!native->validateSeExprDocument(QString::fromUtf8(QJsonDocument(invalidDocument).toJson(QJsonDocument::Compact)).toStdWString(),
                                        0, error) && error.find("64") != std::string::npos,
          "native animation key bound lacked its explicit diagnostic");
  motion["keys"] = QJsonArray();
  document(fx, "[movement(1),0,0]", "1", {motion});
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 0, 0, 0, 1);
  QJsonObject legacy = motion; legacy["id"] = "legacy"; legacy["name"] = "legacy";
  legacy["keys"] = QJsonArray{QJsonObject{{"time", 0}, {"value", .25}}};
  document(fx, "[legacy(1),0,0]", "1", {legacy});
  require(fx.getParams()->getParam("seexprControl_legacy__kkey_0_value"),
          "legacy animation keys did not acquire stable native IDs");
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], .25, 0, 0, 1);
}

void checkNamespacedRenderAttributes() {
  SeExprGeneratorFx fx;
  document(fx, "$shot::gain+$shot::tint+$missing::value");
  auto *native = dynamic_cast<TFxSeExprSource *>(&fx);
  std::string error;
  require(native->validateSeExprDocument(native->seExprDocumentParam()->getValue(), 0, error),
          "missing host attributes did not validate with their zero fallback");
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 0, 0, 0, 1);
  TSeExprAttributes::Snapshot outer;
  outer.attributes = {{"shot::gain", TSeExpr::Value::scalar(.25)},
                       {"shot::tint", TSeExpr::Value::vector(-1, .5, 2)}};
  TSeExprAttributes::Snapshot near;
  near.attributes = {{"shot::gain", TSeExpr::Value::scalar(.5)}};
  near.options = {{"shot::gain", TSeExpr::Value::scalar(.75)}};
  const TRenderSettings empty;
  TRenderSettings one;
  one.m_data.push_back(new TSeExprAttributes::RenderData(outer));
  TRenderSettings two = one;
  two.m_data.push_back(new TSeExprAttributes::RenderData(near));
  const auto compareSettings = [](const TRenderSettings &left,
                                   const TRenderSettings &right, bool equal) {
    require((left == right) == equal && (left != right) != equal,
            "render settings compared an attribute prefix as a complete snapshot");
    require((right == left) == equal && (right != left) != equal,
            "render settings attribute comparison depends on argument order");
  };
  compareSettings(empty, TRenderSettings(), true);
  compareSettings(empty, one, false); compareSettings(empty, two, false);
  compareSettings(one, two, false);
  TRenderSettings sameOne = one;
  sameOne.m_data.front() = new TSeExprAttributes::RenderData(outer);
  compareSettings(one, sameOne, true);
  TRenderSettings sameTwo = two;
  sameTwo.m_data.back() = new TSeExprAttributes::RenderData(near);
  compareSettings(two, sameTwo, true);
  auto changedOuter = outer;
  changedOuter.attributes["shot::gain"] = TSeExpr::Value::scalar(.251);
  sameOne.m_data.front() = new TSeExprAttributes::RenderData(changedOuter);
  compareSettings(one, sameOne, false);
  auto changedNear = near;
  changedNear.options["shot::gain"] = TSeExpr::Value::scalar(.751);
  sameTwo.m_data.back() = new TSeExprAttributes::RenderData(changedNear);
  compareSettings(two, sameTwo, false);
  TRenderSettings settings;
  settings.m_data.push_back(new TSeExprAttributes::RenderData(outer));
  settings.m_data.push_back(new TSeExprAttributes::RenderData(near));
  const auto alias = fx.getAlias(0, settings);
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 0, settings)->pixels(0)[0], -.5, 1, 2.5, 1);
  near.options["shot::gain"] = TSeExpr::Value::scalar(.75000001);
  settings.m_data.back() = new TSeExprAttributes::RenderData(near);
  require(alias != fx.getAlias(0, settings), "render attributes were missing from the full-precision alias");
  near.attributes.clear(); settings.m_data.back() = new TSeExprAttributes::RenderData(near);
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 0, settings)->pixels(0)[0], -.75, .75, 2.25, 1);
  outer.attributes.erase("shot::gain");
  settings.m_data.front() = new TSeExprAttributes::RenderData(outer);
  near.options["shot::gain"] = TSeExpr::Value::scalar(1.25);
  settings.m_data.back() = new TSeExprAttributes::RenderData(near);
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 0, settings)->pixels(0)[0], .25, 1.75, 3.25, 1);
  near.options.clear(); settings.m_data.back() = new TSeExprAttributes::RenderData(near);
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 0, settings)->pixels(0)[0], -1, .5, 2, 1);
  document(fx, "[$shot::gain[0],$shot::gain[1],$shot::gain[2]]");
  outer.attributes["shot::gain"] = TSeExpr::Value::scalar(.5);
  settings.m_data.front() = new TSeExprAttributes::RenderData(outer);
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 0, settings)->pixels(0)[0], .5, .5, .5, 1);
  outer.attributes.erase("shot::gain");
  settings.m_data.front() = new TSeExprAttributes::RenderData(outer);
  near.options["shot::gain"] = TSeExpr::Value::scalar(.75);
  settings.m_data.back() = new TSeExprAttributes::RenderData(near);
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 0, settings)->pixels(0)[0], .75, .75, .75, 1);
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 0, 0, 0, 1);
}

void checkSurfaceHostBoundary() {
  SeExprGeneratorFx fx;
  auto *native = dynamic_cast<TFxSeExprSource *>(&fx);
  require(native && !native->supportsSeExprSurfaceDocument(),
          "an image generator advertised unsupported geometry authoring");
  for (const QString field : {QString("displacement"), QString("opacity")}) {
    QJsonObject value{{"version", 1}, {"rgb", "[1,0,0]"}, {"alpha", "1"},
                       {"controls", QJsonArray()}};
    value[field] = field == "opacity" ? "[1,1,1]" : "0.25";
    const auto source = QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)).toStdWString();
    std::string error;
    require(!native->validateSeExprDocument(source, 0, error) &&
                error.find("Surface") != std::string::npos,
            "an image host validated an inactive surface source");
    native->seExprDocumentParam()->setValue(source);
    bool rejected = false;
    try { rendered<TPixelF>(fx, 1, 1); }
    catch (const TException &) { rejected = true; }
    require(rejected, "a stored surface source was silently ignored by an image renderer");
  }
}

void checkNamedControlsAndPersistence(const QString &directory) {
  TFxP owner = new SeExprGeneratorFx;
  auto &fx = *dynamic_cast<SeExprGeneratorFx *>(owner.getPointer());
  QJsonObject tint{{"id", "tint"}, {"name", "tint"}, {"type", "color"}, {"value", QJsonArray{3.5, -.25, 1.123456789}}};
  document(fx, "$tint", "1", {tint});
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 3.5, -.25, 1.123456789, 1);
  QJsonObject colors{{"id", "colors"}, {"name", "colors"}, {"type", "colorcurve"},
      {"points", QJsonArray{QJsonArray{0, QJsonArray{4, -.5, .123456789}, 1},
                             QJsonArray{1, QJsonArray{2, .5, .987654321}, 1}}}};
  document(fx, "colors(0.25)", "1", {colors});
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 3.5, -.25,
         .123456789 * .75 + .987654321 * .25, 1);
  TRenderSettings linear; linear.m_linearColorSpace = true; linear.m_colorSpaceGamma = 2;
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 0, linear)->pixels(0)[0], 13, -.3125,
         .123456789 * .123456789 * .75 + .987654321 * .987654321 * .25, 1);
  QJsonObject spectrum{{"id", "ocean"}, {"name", "ocean"}, {"type", "curve"},
      {"presentation", "spectrum"}, {"spectrum", QJsonObject()},
      {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}};
  document(fx, "[ocean(0.5),0,0]", "1", {spectrum});
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], TSeExprSpectrum::value(TSeExprSpectrum::Parameters(), .5), 0, 0, 1);
  set(fx, "seexprControl_ocean__s_amplitude", 0);
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 0, 0, 0, 1);
  QJsonObject steps = scalar("steps", 2.7, true);
  document(fx, "[$steps,0,0]", "1", {steps});
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 3, 0, 0, 1);
  numeric(fx, "seexprControl_steps__value")->setValue(1, 4.4);
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 1)->pixels(0)[0], 4, 0, 0, 1);
  const auto before = fx.getAlias(1, TRenderSettings());
  numeric(fx, "seexprControl_steps__value")->setValue(1, 4.40000001);
  require(before != fx.getAlias(1, TRenderSettings()), "cache key lost control precision");
  auto renamed = steps; renamed["label"] = "Renamed Steps";
  document(fx, "[$steps,0,0]", "1", {renamed});
  require(numeric(fx, "seexprControl_steps__value")->hasKeyframes(), "display rename deleted animation");
  QJsonObject curve{{"id", "ramp"}, {"name", "ramp"}, {"type", "curve"},
                    {"points", QJsonArray{QJsonArray{0, 0, 0}, QJsonArray{1, 1, 1}}}};
  document(fx, "[ramp(0.5),$steps,0]", "1", {renamed, curve});
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 1)->pixels(0)[0], 0, 4, 0, 1);
  curve["points"] = QJsonArray{QJsonArray{0, 0, 1}, QJsonArray{.5, 1, 1}, QJsonArray{1, 0, 1}};
  document(fx, "[ramp(0.25),$steps,0]", "1", {curve, renamed});
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 1)->pixels(0)[0], .5, 4, 0, 1);
  TFxP copy = fx.clone(false);
  require(copy->getParams()->getParamCount() == fx.getParams()->getParamCount(), "cloning lost archived controls or curve stops");
  expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(copy.getPointer()), 1, 1, TPointD(), 1)->pixels(0)[0], .5, 4, 0, 1);
  for (bool compressed : {false, true}) {
    const TFilePath path((directory + (compressed ? "/fx-compressed.tnz" : "/fx.tnz")).toStdWString());
    { TOStream stream(path, compressed); stream << owner.getPointer(); }
    TFxP restored; { TIStream stream(path); stream >> restored; }
    require(restored && restored->getParams()->getParam("seexprControl_steps__value"), "scene loading looked up controls before their schema");
    expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(restored.getPointer()), 1, 1, TPointD(), 1)->pixels(0)[0], .5, 4, 0, 1);
  }
  const TFilePath preset((directory + "/formula.preset").toStdWString());
  { TOStream stream(preset); fx.savePreset(stream); }
  SeExprGeneratorFx presetFx; { TIStream stream(preset); presetFx.loadPreset(stream); }
  expect(rendered<TPixelF>(presetFx, 1, 1, TPointD(), 1)->pixels(0)[0], .5, 4, 0, 1);
  owner->setFxId(L"Expression1");
  TFxP colorOwner = new SeExprColorFx;
  colorOwner->setFxId(L"ExpressionColor1");
  colorOwner->connect("Source", owner.getPointer());
  // Native macros require at least two connected effects with one output.
  // Exercise a real Generator -> Color graph, including the Color's exposed
  // Mask input, instead of an ineligible single-effect selection.
  TFxP macroOwner = TMacroFx::create({owner, colorOwner});
  auto *macro = dynamic_cast<TMacroFx *>(macroOwner.getPointer());
  require(macro, "native expression macro creation failed");
  macro->setParamExposed(owner.getPointer(), "seexprControl_steps__value", true);
  auto checkMacro = [](TMacroFx *candidate) {
    require(candidate, "native expression macro is missing");
    auto *generator = dynamic_cast<SeExprGeneratorFx *>(candidate->getFxById(L"Expression1"));
    auto *color = dynamic_cast<SeExprColorFx *>(candidate->getFxById(L"ExpressionColor1"));
    require(generator && color && candidate->getRoot() == color &&
                color->getInputPort("Source")->getFx() == generator,
            "native expression macro lost its internal Generator to Color graph");
    require(candidate->isParamExposed(generator, "seexprControl_steps__value") &&
                numeric(*generator, "seexprControl_steps__value")->hasKeyframes(),
            "native expression macro lost its exposed animated control");
    expect(rendered<TPixelF>(*generator, 1, 1, TPointD(), 1)->pixels(0)[0], .5, 4, 0, 1);
    return generator;
  };
  checkMacro(macro);
  const TFilePath macroPath((directory + "/formula.macro").toStdWString());
  { TOStream stream(macroPath); stream << macroOwner.getPointer(); }
  TFxP loadedMacro; { TIStream stream(macroPath); stream >> loadedMacro; }
  auto *restoredMacro = dynamic_cast<TMacroFx *>(loadedMacro.getPointer());
  auto *restoredGenerator = checkMacro(restoredMacro);
  TFxP macroCopy = restoredMacro->clone(false);
  auto *clonedGenerator = checkMacro(dynamic_cast<TMacroFx *>(macroCopy.getPointer()));
  numeric(*clonedGenerator, "seexprControl_steps__value")->setValue(1, 8);
  expect(rendered<TPixelF>(*clonedGenerator, 1, 1, TPointD(), 1)->pixels(0)[0], .5, 8, 0, 1);
  expect(rendered<TPixelF>(*restoredGenerator, 1, 1, TPointD(), 1)->pixels(0)[0], .5, 4, 0, 1);
  SeExprGeneratorFx linked;
  document(linked, "[$extra,0,0]", "1", {scalar("extra", .2)});
  linked.linkParams(&fx);
  numeric(fx, "seexprControl_steps__value")->setValue(1, 6);
  expect(rendered<TPixelF>(linked, 1, 1, TPointD(), 1)->pixels(0)[0], .5, 6, 0, 1);
  auto expanded = scalar("another", .75);
  document(fx, "[$another,$steps,0]", "1", {renamed, expanded});
  require(linked.getParams()->getParam("seexprControl_another__value") == fx.getParams()->getParam("seexprControl_another__value"),
          "growing a linked schema created independent controls");
  linked.unlinkParams();
  document(linked, "[$solo,0,0]", "1", {scalar("solo", .125)});
  expect(rendered<TPixelF>(linked, 1, 1)->pixels(0)[0], .125, 0, 0, 1);
  require(!fx.getParams()->getParam("seexprControl_solo__value"), "unlinked schema edits changed the former source");
  document(fx, "[0,0,0]");
  require(fx.getParams()->getParam("seexprControl_steps__value") &&
              numeric(fx, "seexprControl_steps__value")->hasKeyframes(), "removing a control deleted its archived animation");
}

void checkFailuresAndWorkers() {
  SeExprGeneratorFx fx;
  document(fx, "[broken,");
  bool failed = false; try { rendered<TPixelF>(fx, 1, 1); } catch (const TException &) { failed = true; }
  require(failed, "invalid formula rendered a success-looking blank image");
  document(fx, "[1/0,0,0]"); failed = false;
  try { rendered<TPixelF>(fx, 1, 1); } catch (const TException &) { failed = true; }
  require(failed, "nonfinite output was accepted");
  auto worker = [] {
    SeExprGeneratorFx effect; set(effect, "width", 16); set(effect, "height", 16);
    document(effect, "def FLOAT square(FLOAT a){a*a} [square($u),square($v),noise([$x,$y,$seed])]");
    return rendered<TPixelF>(effect, 16, 16, TPointD(-8,-8));
  };
  auto a = std::async(std::launch::async, worker), b = std::async(std::launch::async, worker);
  const auto first = a.get(), second = b.get();
  for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x)
    require(first->pixels(y)[x] == second->pixels(y)[x], "parallel workers shared mutable interpreter state");
}

void expectUnsupportedDocument(TFx &fx, const std::wstring &source,
                               const char *diagnostic) {
  auto *native = dynamic_cast<TFxSeExprSource *>(&fx);
  require(native && native->seExprDocumentParam()->getValue() == source,
          "unsupported document source was changed or discarded");
  std::string error;
  require(!native->validateSeExprDocument(source, 0, error) &&
              error.find(diagnostic) != std::string::npos,
          "unsupported document validation lost its diagnostic");
  std::string preview;
  require(!native->previewSeExprDocument(source, 0, preview, error) &&
              error.find(diagnostic) != std::string::npos,
          "unsupported document preview silently used a different engine");
  bool rejected = false;
  try {
    rendered<TPixelF>(*dynamic_cast<TRasterFx *>(&fx), 1, 1);
  } catch (const TException &failure) {
    rejected = QString::fromStdWString(failure.getMessage()).toStdString().find(diagnostic) != std::string::npos;
  }
  require(rejected, "unsupported document rendered without the expected failure");
  require(native->seExprDocumentParam()->getValue() == source,
          "validation or rendering rewrote an unsupported document");
}

void expectPreservedUnsupportedAnimation(TFx &fx) {
  const auto gain = numeric(fx, "seexprControl_gain__value");
  const auto stop = numeric(fx, "seexprControl_ramp__v1");
  require(gain->isKeyframe(1) && gain->isKeyframe(3) &&
              gain->getValue(1) == .875 && gain->getValue(3) == .9375 &&
              stop->isKeyframe(1) && stop->isKeyframe(3) &&
              stop->getValue(1) == 2 && stop->getValue(3) == 3,
          "unsupported document persistence lost native control animation");
}

void checkUnsupportedDocumentPersistence(const QString &directory) {
  struct Unsupported {
    const char *field;
    QJsonValue value;
    const char *diagnostic;
  };
  const Unsupported cases[] = {
      {"version", 999, "unsupported expression document version"},
      {"engine", QStringLiteral("unavailable-engine"), "saved expression engine version is unavailable"},
      {"functionPolicy", 999, "saved expression function policy is unsupported"}};
  for (const auto &test : cases) {
    TFxP owner = new SeExprGeneratorFx;
    auto *native = dynamic_cast<TFxSeExprSource *>(owner.getPointer());
    const QJsonObject ramp{{"id", "ramp"}, {"name", "ramp"}, {"type", "curve"},
        {"points", QJsonArray{QJsonArray{0, 0, 1}, QJsonArray{1, 1, 1}}}};
    document(*owner, "[$gain,ramp(0.5),0]", "1", {scalar("gain", .75), ramp});
    numeric(*owner, "seexprControl_gain__value")->setValue(1, .875);
    numeric(*owner, "seexprControl_gain__value")->setValue(3, .9375);
    numeric(*owner, "seexprControl_ramp__v1")->setValue(1, 2);
    numeric(*owner, "seexprControl_ramp__v1")->setValue(3, 3);
    const auto repair = native->seExprDocumentParam()->getValue();
    QJsonObject unsupported = QJsonDocument::fromJson(QString::fromStdWString(repair).toUtf8()).object();
    unsupported[test.field] = test.value;
    unsupported["futureMetadata"] = QJsonObject{{"retained", true}};
    // Preserve formatting and unknown fields too, rather than canonicalizing
    // an unsupported format into a success-looking current declaration.
    const auto source = QString::fromUtf8(QJsonDocument(unsupported).toJson(QJsonDocument::Indented)).toStdWString();
    native->seExprDocumentParam()->setValue(source);
    expectUnsupportedDocument(*owner, source, test.diagnostic);
    expectPreservedUnsupportedAnimation(*owner);

    TFxP copy = owner->clone(false);
    expectUnsupportedDocument(*copy, source, test.diagnostic);
    require(copy->getParams()->getParamCount() == owner->getParams()->getParamCount(),
            "cloning an unsupported document lost its registered controls");
    expectPreservedUnsupportedAnimation(*copy);

    for (bool compressed : {false, true}) {
      const QString filename = QString("/unsupported-%1-%2.tnz").arg(test.field).arg(compressed ? "compressed" : "plain");
      const TFilePath path((directory + filename).toStdWString());
      { TOStream stream(path, compressed); stream << owner.getPointer(); }
      TFxP restored; { TIStream stream(path); stream >> restored; }
      require(restored, "loading an unsupported document discarded the effect");
      expectUnsupportedDocument(*restored, source, test.diagnostic);
      expectPreservedUnsupportedAnimation(*restored);
      TFxP restoredCopy = restored->clone(false);
      expectUnsupportedDocument(*restoredCopy, source, test.diagnostic);
      expectPreservedUnsupportedAnimation(*restoredCopy);
      const TFilePath resaved((directory + filename + ".resaved").toStdWString());
      { TOStream stream(resaved, compressed); stream << restored.getPointer(); }
      TFxP reopened; { TIStream stream(resaved); stream >> reopened; }
      expectUnsupportedDocument(*reopened, source, test.diagnostic);
      expectPreservedUnsupportedAnimation(*reopened);
      dynamic_cast<TFxSeExprSource *>(restored.getPointer())->seExprDocumentParam()->setValue(repair);
      expectPreservedUnsupportedAnimation(*restored);
      expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(restored.getPointer()), 1, 1, TPointD(), 1)->pixels(0)[0], .875, 1, 0, 1);
      dynamic_cast<TFxSeExprSource *>(reopened.getPointer())->seExprDocumentParam()->setValue(repair);
      expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(reopened.getPointer()), 1, 1, TPointD(), 3)->pixels(0)[0], .9375, 1.5, 0, 1);
    }

    TFxP first = owner->clone(false), second = owner->clone(false);
    second->linkParams(first.getPointer());
    for (bool compressed : {false, true}) {
      const QString filename = QString("/unsupported-linked-%1-%2.tnz").arg(test.field).arg(compressed ? "compressed" : "plain");
      const TFilePath path((directory + filename).toStdWString());
      { TOStream stream(path, compressed); stream << first.getPointer(); stream << second.getPointer(); }
      TFxP restoredFirst, restoredSecond;
      { TIStream stream(path); stream >> restoredFirst; stream >> restoredSecond; }
      require(restoredFirst && restoredSecond &&
                  restoredFirst->getLinkedFx() == restoredSecond.getPointer() &&
                  restoredSecond->getLinkedFx() == restoredFirst.getPointer(),
              "unsupported linked scene did not restore its linked pair");
      require(restoredFirst->getParams()->getParam("seexprControl_gain__value") ==
                  restoredSecond->getParams()->getParam("seexprControl_gain__value") &&
                  restoredFirst->getParams()->getParam("seexprControl_ramp__v1") ==
                  restoredSecond->getParams()->getParam("seexprControl_ramp__v1"),
              "unsupported linked scene created independent native animation");
      expectUnsupportedDocument(*restoredFirst, source, test.diagnostic);
      expectUnsupportedDocument(*restoredSecond, source, test.diagnostic);
      expectPreservedUnsupportedAnimation(*restoredFirst);
      expectPreservedUnsupportedAnimation(*restoredSecond);
      dynamic_cast<TFxSeExprSource *>(restoredFirst.getPointer())->seExprDocumentParam()->setValue(repair);
      for (TFx *effect : {restoredFirst.getPointer(), restoredSecond.getPointer()}) {
        std::string error;
        auto *nativeEffect = dynamic_cast<TFxSeExprSource *>(effect);
        require(nativeEffect->validateSeExprDocument(nativeEffect->seExprDocumentParam()->getValue(), 1, error),
                "repairing a linked document did not repair every shared peer");
        expectPreservedUnsupportedAnimation(*effect);
        expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(effect), 1, 1, TPointD(), 1)->pixels(0)[0], .875, 1, 0, 1);
      }
      numeric(*restoredSecond, "seexprControl_gain__value")->setValue(1, .8125);
      expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(restoredFirst.getPointer()), 1, 1, TPointD(), 1)->pixels(0)[0], .8125, 1, 0, 1);
    }

    TFxP incompatible = owner->clone(false);
    auto different = unsupported;
    different["futureMetadata"] = QJsonObject{{"retained", false}};
    const auto differentSource = QString::fromUtf8(QJsonDocument(different).toJson(QJsonDocument::Indented)).toStdWString();
    dynamic_cast<TFxSeExprSource *>(incompatible.getPointer())->seExprDocumentParam()->setValue(differentSource);
    bool linkRejected = false;
    try { incompatible->linkParams(owner.getPointer()); } catch (const TException &) { linkRejected = true; }
    require(linkRejected && incompatible->getLinkedFx() == incompatible.getPointer(),
            "incompatible unsupported documents were linked implicitly");
    expectUnsupportedDocument(*incompatible, differentSource, test.diagnostic);

    TFxP incompatibleStorage = new SeExprGeneratorFx;
    const QJsonObject colorGain{{"id", "gain"}, {"name", "gain"}, {"type", "color"},
                               {"value", QJsonArray{.75, .75, .75}}};
    document(*incompatibleStorage, "$gain", "1", {colorGain, ramp});
    dynamic_cast<TFxSeExprSource *>(incompatibleStorage.getPointer())->seExprDocumentParam()->setValue(source);
    linkRejected = false;
    try { incompatibleStorage->linkParams(owner.getPointer()); } catch (const TException &) { linkRejected = true; }
    require(linkRejected && incompatibleStorage->getLinkedFx() == incompatibleStorage.getPointer(),
            "identical unsupported source bypassed native parameter type checks");
    expectUnsupportedDocument(*incompatibleStorage, source, test.diagnostic);

    const TFilePath preset((directory + QString("/unsupported-%1.preset").arg(test.field)).toStdWString());
    { TOStream stream(preset); owner->savePreset(stream); }
    SeExprGeneratorFx presetFx; { TIStream stream(preset); presetFx.loadPreset(stream); }
    expectUnsupportedDocument(presetFx, source, test.diagnostic);
    expectPreservedUnsupportedAnimation(presetFx);
    presetFx.seExprDocumentParam()->setValue(repair);
    expect(rendered<TPixelF>(presetFx, 1, 1, TPointD(), 1)->pixels(0)[0], .875, 1, 0, 1);

    dynamic_cast<TFxSeExprSource *>(copy.getPointer())->seExprDocumentParam()->setValue(repair);
    expectPreservedUnsupportedAnimation(*copy);
    expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(copy.getPointer()), 1, 1, TPointD(), 1)->pixels(0)[0], .875, 1, 0, 1);
    // Repairing the clone must not rewrite the still unsupported original.
    expectUnsupportedDocument(*owner, source, test.diagnostic);
  }

  // A future declaration with an unrecognizable control schema still retains
  // its source, but must not invent native storage or authorize evaluation.
  TFxP future = new SeExprGeneratorFx;
  const QJsonObject unknown{{"version", 999}, {"rgb", "[1,0,0]"}, {"alpha", "1"},
      {"controls", QJsonArray{QJsonObject{{"id", "future"}, {"name", "future"},
          {"type", "unrecognized-control"}, {"value", QJsonObject{{"data", 7}}}}}}};
  const auto source = QString::fromUtf8(QJsonDocument(unknown).toJson(QJsonDocument::Indented)).toStdWString();
  dynamic_cast<TFxSeExprSource *>(future.getPointer())->seExprDocumentParam()->setValue(source);
  const TFilePath path((directory + "/unsupported-control-schema.tnz").toStdWString());
  { TOStream stream(path); stream << future.getPointer(); }
  TFxP restored; { TIStream stream(path); stream >> restored; }
  expectUnsupportedDocument(*restored, source, "unsupported expression document version");
  require(!restored->getParams()->getParam("seexprControl_future__value"),
          "an unrecognizable future control acquired invented native storage");
  TFxP copy = restored->clone(false);
  expectUnsupportedDocument(*copy, source, "unsupported expression document version");
}

std::uint64_t checksum(const TRasterFP &image, int rowStart, int fullWidth) {
  std::uint64_t result = 0;
  for (int y = 0; y < image->getLy(); ++y) for (int x = 0; x < image->getLx(); ++x) {
    const TPixelF &p = image->pixels(y)[x];
    const float channels[] = {p.r, p.g, p.b, p.m};
    for (int c = 0; c < 4; ++c) {
      std::uint32_t bits = 0; std::memcpy(&bits, channels + c, sizeof(bits));
      std::uint64_t h = std::uint64_t(bits) ^
          ((std::uint64_t(rowStart + y) * fullWidth + x) * 4 + c) * 0x9e3779b97f4a7c15ULL;
      h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
      h ^= h >> 27; h *= 0x94d049bb133111ebULL;
      result += h ^ (h >> 31);
    }
  }
  return result;
}
std::uint64_t peakMemory() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  require(GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0,
          "peak working-set measurement failed");
  return counters.PeakWorkingSetSize;
#else
  struct rusage usage{};
  require(getrusage(RUSAGE_SELF, &usage) == 0, "peak resident-memory measurement failed");
#ifdef __APPLE__
  return std::uint64_t(usage.ru_maxrss);
#else
  return std::uint64_t(usage.ru_maxrss) * 1024;
#endif
#endif
}

void benchmarkResidentNodes() {
  // Keep a population of independently animated native effects alive while
  // source revisions evict the worker cache. This measures resident-node and
  // editing workloads; it does not simulate a connected production scene.
  constexpr int nodeCount = 64;
  constexpr int revisions = 8;
  const auto began = std::chrono::steady_clock::now();
  const auto peakBefore = peakMemory();
  std::vector<std::unique_ptr<SeExprGeneratorFx>> effects;
  effects.reserve(nodeCount);
  for (int node = 0; node < nodeCount; ++node) {
    std::unique_ptr<SeExprGeneratorFx> effect(new SeExprGeneratorFx);
    set(*effect, "width", 8); set(*effect, "height", 8);
    document(*effect, "[$amount+$u,0,$v]", "1", {scalar("amount", double(node) / nodeCount)});
    const auto amount = numeric(*effect, "seexprControl_amount__value");
    amount->setValue(1, double(node) / nodeCount);
    amount->setValue(3, -double(node) / nodeCount);
    effects.push_back(std::move(effect));
  }
  const auto peakAllocated = peakMemory();
  double coldMilliseconds = 0, warmMilliseconds = 0;
  std::uint64_t aggregate = 0;
  for (int revision = 0; revision < revisions; ++revision) {
    for (int node = 0; node < nodeCount; ++node) {
      auto &effect = *effects[node];
      const auto amount = numeric(effect, "seexprControl_amount__value");
      const double green = double(revision) / revisions;
      const QString source = QString("[$amount+$u,%1,$v] # node %2")
                                 .arg(green, 0, 'g', 17).arg(node);
      document(effect, source, "1", {scalar("amount", double(node) / nodeCount)});
      require(numeric(effect, "seexprControl_amount__value").getPointer() == amount.getPointer() &&
                  amount->isKeyframe(1) && amount->isKeyframe(3),
              "source editing replaced a resident node's animated parameter");
      for (int pass = 0; pass < 2; ++pass) {
        const double frame = pass ? 3 : 1;
        const auto renderBegan = std::chrono::steady_clock::now();
        const auto image = rendered<TPixelF>(effect, 8, 8, TPointD(-4, -4), frame);
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - renderBegan).count();
        if (pass) warmMilliseconds += elapsed;
        else coldMilliseconds += elapsed;
        const double value = (pass ? -1 : 1) * double(node) / nodeCount;
        expect(image->pixels(0)[0], value + .0625, green, .0625, 1);
        expect(image->pixels(7)[7], value + .9375, green, .9375, 1);
        aggregate += checksum(image, 0, 8);
      }
    }
  }
  const auto peakEdited = peakMemory();
  effects.clear();
  std::cout << "seexpr-resident-nodes: nodes=" << nodeCount << " revisions=" << revisions
            << " rendered_frames=" << nodeCount * revisions * 2
            << " cold_render_total_ms=" << coldMilliseconds
            << " warm_render_total_ms=" << warmMilliseconds
            << " total_wall_ms=" << std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - began).count()
            << " checksum=" << aggregate
            << " peak_before_bytes=" << peakBefore
            << " peak_after_allocation_bytes=" << peakAllocated
            << " peak_after_edits_bytes=" << peakEdited
            << " peak_after_release_bytes=" << peakMemory() << std::endl;
}

void benchmark() {
  benchmarkResidentNodes();
  struct Work { double cold = 0, warm = 0; std::uint64_t first = 0, second = 0; };
  for (const auto dimensions : {TDimension(1920,1080), TDimension(3840,2160)}) {
    std::uint64_t referenceFirst = 0, referenceSecond = 0;
    for (int workers : {1, 4}) {
      std::vector<std::future<Work>> tasks;
      const auto wall = std::chrono::steady_clock::now();
      for (int worker = 0; worker < workers; ++worker) {
        const int start = dimensions.ly * worker / workers;
        const int end = dimensions.ly * (worker + 1) / workers;
        tasks.push_back(std::async(std::launch::async, [=]() {
          SeExprGeneratorFx effect;
          set(effect, "width", dimensions.lx); set(effect, "height", dimensions.ly);
          document(effect, "[$u,$v,0.5+0.25*noise([$x/64,$y/64,$time])]");
          Work result;
          for (int frame = 0; frame < 2; ++frame) {
            const auto began = std::chrono::steady_clock::now();
            const auto image = rendered<TPixelF>(effect, dimensions.lx, end - start,
                TPointD(-dimensions.lx * .5, start - dimensions.ly * .5), frame);
            const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            const auto hash = checksum(image, start, dimensions.lx);
            if (frame == 0) { result.cold = elapsed; result.first = hash; }
            else { result.warm = elapsed; result.second = hash; }
          }
          return result;
        }));
      }
      Work total;
      for (auto &task : tasks) {
        const auto result = task.get();
        total.cold = std::max(total.cold, result.cold); total.warm = std::max(total.warm, result.warm);
        total.first += result.first; total.second += result.second;
      }
      if (workers == 1) { referenceFirst = total.first; referenceSecond = total.second; }
      else require(referenceFirst == total.first && referenceSecond == total.second,
                   "full-resolution worker-count checksums differ");
      std::cout << "seexpr-benchmark: width=" << dimensions.lx << " height=" << dimensions.ly
                << " workers=" << workers << " cold_max_worker_ms=" << total.cold
                << " warm_max_worker_ms=" << total.warm
                << " total_wall_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall).count()
                << " checksum_frame0=" << total.first << " checksum_frame1=" << total.second
                << " peak_resident_bytes=" << peakMemory() << std::endl;
    }
  }
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    if (argc == 2 && std::string(argv[1]) == "--benchmark") { benchmark(); return 0; }
    QTemporaryDir temporary; require(temporary.isValid(), "temporary fixture directory is unavailable");
    checkGenerator<TPixel32>(); checkGenerator<TPixel64>(); checkGenerator<TPixelF>();
    checkColorAndSampling<TPixel32>(); checkColorAndSampling<TPixel64>(); checkColorAndSampling<TPixelF>();
    checkControlRangeEdits(); checkAnimationCurveControls(temporary.path()); checkNamespacedRenderAttributes();
    checkSurfaceHostBoundary();
    checkNamedControlsAndPersistence(temporary.path());
    checkUnsupportedDocumentPersistence(temporary.path()); checkFailuresAndWorkers();
    std::cout << "seexpr-fx: PASS\n"; return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  return 1;
}
