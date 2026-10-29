#include "seexprfx.h"
#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tseexprfx.h"
#include "tseexprspectrum.h"
#include "tstream.h"
#include "tpixelutils.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

namespace {
constexpr std::size_t maxDocumentBytes = 262144;
constexpr std::size_t maxControls = 128;
constexpr double maxWorkingBytes = 256.0 * 1024 * 1024;
const char *documentName = "seexprDocument";
const char *controlPrefix = "seexprControl_";
const std::set<QString> reservedVariables = {
    "x", "y", "u", "v", "width", "height", "frame", "time", "fps",
    "seed", "src", "r", "g", "b", "a", "sample_rgb", "sample_alpha"};

void demand(bool value, const char *message) {
  if (!value) throw TException(std::string("SeExpr: ") + message);
}
double finite(double value, const char *message) {
  demand(std::isfinite(value), message);
  return value;
}
double unit(double value) { return std::max(0.0, std::min(1.0, value)); }
double workingColor(double value, bool linear, double gamma) {
  // Match the native floating-raster convention for negative scene values.
  return linear && value >= 0 ? std::pow(value, gamma) : value;
}
bool canceled(const int *flag) { return flag && *flag; }
bool identifier(const QString &name) {
  static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,47}$"));
  return pattern.match(name).hasMatch();
}
double number(const QJsonValue &value, double fallback = 0) {
  demand(value.isUndefined() || value.isDouble(), "control value must be numeric");
  return finite(value.isUndefined() ? fallback : value.toDouble(),
                "control value must be finite");
}
std::array<double, 3> triple(const QJsonValue &value) {
  demand(value.isArray(), "vector/color controls need three numeric components");
  const auto values = value.toArray();
  demand(values.size() == 3, "vector/color controls need three numeric components");
  return {{number(values[0]), number(values[1]), number(values[2])}};
}
bool finiteBox(const TRectD &box) {
  return box != TConsts::infiniteRectD && std::isfinite(box.x0) &&
         std::isfinite(box.y0) && std::isfinite(box.x1) && std::isfinite(box.y1);
}
TRectD rasterBox(const TRectD &box) {
  demand(finiteBox(box), "input canvas must be finite");
  return TRectD(std::floor(box.x0), std::floor(box.y0),
                std::ceil(box.x1), std::ceil(box.y1));
}
TDimension rasterSize(const TRectD &box, int bytes) {
  const double w = box.getLx(), h = box.getLy();
  demand(finiteBox(box) && w >= 1 && h >= 1 &&
             w <= std::numeric_limits<int>::max() &&
             h <= std::numeric_limits<int>::max() && bytes > 0 &&
             w * h * bytes <= maxWorkingBytes,
         "sampling request exceeds the 256 MiB working-buffer limit");
  return TDimension(int(w), int(h));
}
class RasterLock {
  TRasterP m_raster;
public:
  explicit RasterLock(const TRasterP &raster) : m_raster(raster) {
    if (m_raster) m_raster->lock();
  }
  ~RasterLock() { if (m_raster) m_raster->unlock(); }
};
TPixelD straight(TPixelD p) {
  demand(std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b) &&
             std::isfinite(p.m), "source contains nonfinite pixels");
  if (p.m <= 0) return TPixelD(0, 0, 0, 0);
  return TPixelD(p.r / p.m, p.g / p.m, p.b / p.m, unit(p.m));
}
template <typename Pixel> TPixelD normalized(const Pixel &p) {
  const double maximum = Pixel::maxChannelValue;
  return TPixelD(p.r / maximum, p.g / maximum, p.b / maximum, p.m / maximum);
}
TPixelD pixelAt(const TRasterP &raster, int x, int y) {
  if (!raster || x < 0 || y < 0 || x >= raster->getLx() || y >= raster->getLy())
    return TPixelD(0, 0, 0, 0);
  if (TRaster32P image = raster) return normalized(image->pixels(y)[x]);
  if (TRaster64P image = raster) return normalized(image->pixels(y)[x]);
  if (TRasterFP image = raster) return normalized(image->pixels(y)[x]);
  throw TException("SeExpr: unsupported source pixel type");
}
TPixelD blendPixels(const TPixelD &a, const TPixelD &b, double t) {
  return TPixelD(a.r * (1 - t) + b.r * t, a.g * (1 - t) + b.g * t,
                 a.b * (1 - t) + b.b * t, a.m * (1 - t) + b.m * t);
}
TPixelD integerPixel(const TPixelD &p) {
  const double alpha = unit(p.m);
  return TPixelD(std::max(0.0, std::min(alpha, p.r)),
                 std::max(0.0, std::min(alpha, p.g)),
                 std::max(0.0, std::min(alpha, p.b)), alpha);
}
TPixel32 converted(const TPixelD &p, TPixel32 *) { return toPixel32(integerPixel(p)); }
TPixel64 converted(const TPixelD &p, TPixel64 *) { return toPixel64(integerPixel(p)); }
TPixelF converted(const TPixelD &p, TPixelF *) {
  const double limit = std::numeric_limits<float>::max();
  return TPixelF(float(std::max(-limit, std::min(limit, p.r))),
                 float(std::max(-limit, std::min(limit, p.g))),
                 float(std::max(-limit, std::min(limit, p.b))), float(unit(p.m)));
}

struct InputSampler {
  TRasterP image;
  TPointD position;
  TRectD canvas;
  TAffine affine;
  TPointD current;
  double radius = 0;
  int edge = 0, filter = 1;

  static double wrapped(double x, double lower, double size, bool mirror) {
    const double period = size * (mirror ? 2 : 1);
    double p = std::fmod(x - lower, period);
    if (p < 0) p += period;
    if (mirror && p >= size) p = std::nextafter(period - p, 0.0);
    return lower + std::min(std::nextafter(size, 0.0), p);
  }
  TPixelD readAt(TPointD global) const {
    if (!image) return TPixelD(0, 0, 0, 0);
    if (edge && !canvas.isEmpty()) {
      if (edge == 1) {
        global.x = std::max(canvas.x0 + .5, std::min(canvas.x1 - .5, global.x));
        global.y = std::max(canvas.y0 + .5, std::min(canvas.y1 - .5, global.y));
      } else {
        global.x = wrapped(global.x, canvas.x0, canvas.getLx(), edge == 3);
        global.y = wrapped(global.y, canvas.y0, canvas.getLy(), edge == 3);
      }
    }
    const double px = global.x - position.x, py = global.y - position.y;
    if (filter == 0) return pixelAt(image, int(std::floor(px)), int(std::floor(py)));
    const double x = px - .5, y = py - .5;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    auto tap = [&](int tx, int ty) {
      if (edge > 1) {
        const double gx = wrapped(position.x + tx + .5, canvas.x0, canvas.getLx(), edge == 3);
        const double gy = wrapped(position.y + ty + .5, canvas.y0, canvas.getLy(), edge == 3);
        tx = int(std::floor(gx - position.x)); ty = int(std::floor(gy - position.y));
      }
      return pixelAt(image, tx, ty);
    };
    return blendPixels(blendPixels(tap(ix, iy), tap(ix + 1, iy), x - ix),
                       blendPixels(tap(ix, iy + 1), tap(ix + 1, iy + 1), x - ix), y - iy);
  }
  TPixelD sample(double dx, double dy) const {
    demand(std::isfinite(dx) && std::isfinite(dy) &&
               std::abs(dx) <= radius && std::abs(dy) <= radius,
           "sample offset exceeds the declared sampling radius");
    return straight(readAt(affine * (current + TPointD(dx, dy))));
  }
};

std::map<std::string, double> spectrumValues(const QJsonObject &object) {
  const TSeExprSpectrum::Parameters p;
  const auto flow = object.value("flowDirection").toArray();
  auto read = [&](const char *name, double fallback) { return number(object.value(name), fallback); };
  return {{"resolution", read("resolution", p.resolution)}, {"tileSize", read("tileSize", p.tileSize)},
          {"lengthCutoff", read("lengthCutoff", p.lengthCutoff)}, {"amplitude", read("amplitude", p.amplitude)},
          {"windAngle", read("windAngle", p.windAngle)}, {"windSpeed", read("windSpeed", p.windSpeed)},
          {"directionalFactorExponent", read("directionalFactorExponent", p.directionalFactorExponent)},
          {"directionalReflectionDamping", read("directionalReflectionDamping", p.directionalReflectionDamping)},
          {"flowX", flow.size() > 0 ? number(flow[0]) : 0}, {"flowY", flow.size() > 1 ? number(flow[1]) : 0},
          {"flowZ", flow.size() > 2 ? number(flow[2]) : 0}, {"sharpen", read("sharpen", p.sharpen)},
          {"time", read("time", p.time)}, {"filterWidth", read("filterWidth", p.filterWidth)}};
}
TSeExprSpectrum::Parameters spectrumParameters(const std::map<std::string, double> &v) {
  TSeExprSpectrum::Parameters p;
  p.resolution = int(std::round(v.at("resolution"))); p.tileSize = v.at("tileSize");
  p.lengthCutoff = v.at("lengthCutoff"); p.amplitude = v.at("amplitude");
  p.windAngle = v.at("windAngle"); p.windSpeed = v.at("windSpeed");
  p.directionalFactorExponent = v.at("directionalFactorExponent");
  p.directionalReflectionDamping = v.at("directionalReflectionDamping");
  p.flowDirection = {{v.at("flowX"), v.at("flowY"), v.at("flowZ")}};
  p.sharpen = v.at("sharpen"); p.time = v.at("time"); p.filterWidth = v.at("filterWidth");
  demand(TSeExprSpectrum::valid(p), "spectrum parameters are outside their valid ranges");
  return p;
}

struct NativeControl {
  SeExprFx::Control spec;
  TDoubleParamP scalar;
  std::array<TDoubleParamP, 3> vector;
  TPixelParamP color;
  TStringParamP text;
  std::array<TDoubleParamP, 64> positions, values;
  std::array<TPixelParamP, 64> colors;
  int allocatedKnots = 0;
  std::map<std::string, TDoubleParamP> spectrum;
};

}  // namespace

namespace SeExprFx {
namespace {
// Storage-only parsing never authorizes evaluation. It recognizes the native
// control declarations of an unavailable engine/document so scene and preset
// loaders can retain their separately serialized animation parameters.
Document parseDocumentContents(const std::wstring &text, bool checkIdentity) {
  const QByteArray bytes = QString::fromStdWString(text).toUtf8();
  demand(std::size_t(bytes.size()) <= maxDocumentBytes, "expression document is too large");
  QJsonParseError error;
  const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &error);
  demand(error.error == QJsonParseError::NoError && parsed.isObject(),
         "expression document is not a JSON object");
  Document result;
  result.json = parsed.object();
  if (checkIdentity)
    demand(result.json.value("version").toInt(-1) == 1, "unsupported expression document version");
  demand(result.json.value("rgb").isString() && result.json.value("alpha").isString(),
         "RGB and alpha source must be strings");
  result.rgb = result.json.value("rgb").toString();
  result.alpha = result.json.value("alpha").toString();
  result.backend = result.json.value("backend").toString("interpreter");
  if (checkIdentity) {
    demand(result.backend == "interpreter" || result.backend == "llvm", "unknown evaluation backend");
    demand(!result.json.contains("engine") || result.json.value("engine").toString().toStdString() == TSeExpr::Program::engineVersion(),
           "saved expression engine version is unavailable");
    demand(!result.json.contains("functionPolicy") || result.json.value("functionPolicy").toInt(-1) == int(TSeExpr::Program::functionPolicyVersion()),
           "saved expression function policy is unsupported");
  }
  std::map<QString, Control> table;
  std::set<QString> names;
  for (const QString group : {QString("controls"), QString("archive")}) {
    const auto value = result.json.value(group);
    demand(value.isUndefined() || value.isArray(), "control schema must be an array");
    for (const auto item : value.toArray()) {
      demand(item.isObject(), "control declaration must be an object");
      const QJsonObject object = item.toObject();
      Control control;
      control.id = object.value("id").toString();
      control.name = object.value("name").toString();
      control.label = object.value("label").toString(control.name);
      control.type = object.value("type").toString("scalar");
      control.definition = object;
      control.active = group == "controls";
      demand(identifier(control.id) && identifier(control.name), "control IDs and variables must be identifiers");
      demand(table.count(control.id) == 0, "duplicate control ID");
      demand(!control.active || (!reservedVariables.count(control.name) && names.insert(control.name).second),
             "duplicate or reserved control variable");
      demand(control.type == "scalar" || control.type == "vector" || control.type == "color" ||
                 control.type == "string" || control.type == "curve" || control.type == "colorcurve",
             "unsupported control type");
      const double minimum = number(object.value("min"), -1000000);
      const double maximum = number(object.value("max"), 1000000);
      demand(minimum <= maximum, "control range is reversed");
      if (control.type == "scalar") {
        number(object.value("value"));
        demand(!object.value("integer").toBool() || std::ceil(minimum) <= std::floor(maximum),
               "integer control range contains no integer");
      }
      else if (control.type == "vector" || control.type == "color") triple(object.value("value"));
      else if (control.type == "string")
        demand(object.value("value").isString(), "string control needs a string value");
      else {
        const QJsonArray points = object.value("points").toArray();
        demand(points.size() >= 2 && points.size() <= 64, "curves need two to 64 points");
        double previous = -std::numeric_limits<double>::infinity();
        for (const auto point : points) {
          const auto pair = point.toArray();
          demand(pair.size() == 2 || pair.size() == 3, "curve points need position, value and optional interpolation");
          if (pair.size() == 3) { const double mode = number(pair[2]);
            demand(mode >= 0 && mode <= 4 && mode == std::floor(mode), "curve interpolation must be zero to four"); }
          const double position = number(pair[0]);
          demand(position > previous && position >= 0 && position <= 1,
                 "curve positions must increase within zero to one");
          previous = position;
          if (control.type == "curve") number(pair[1]); else triple(pair[1]);
        }
      }
      if (object.value("presentation").toString() == "spectrum") {
        demand(control.type == "curve", "spectrum presentation needs a scalar curve");
        spectrumParameters(spectrumValues(object.value("spectrum").toObject()));
      }
      table.emplace(control.id, control);
    }
  }
  demand(table.size() <= maxControls, "too many expression controls");
  std::set<QString> ordered;
  for (const auto idValue : result.json.value("registry").toArray()) {
    const QString id = idValue.toString();
    demand(table.count(id) && ordered.insert(id).second, "invalid saved control registry");
    result.controls.push_back(table.at(id));
  }
  // JSON array order is stable for new documents; registry preserves native
  // parameter order across reordering, removal, cloning and save/reopen.
  for (const QString group : {QString("controls"), QString("archive")})
    for (const auto item : result.json.value(group).toArray()) {
      const QString id = item.toObject().value("id").toString();
      if (ordered.insert(id).second) result.controls.push_back(table.at(id));
    }
  return result;
}
}  // namespace

Document parseDocument(const std::wstring &text) {
  return parseDocumentContents(text, true);
}
std::wstring initialDocument(bool generator) {
  QJsonObject object{{"version", 1}, {"rgb", generator ? "[$u,$v,0.5]" : "$src"},
                     {"alpha", generator ? "1" : "$a"}, {"controls", QJsonArray()}};
  return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)).toStdWString();
}
}  // namespace SeExprFx

namespace {
template <typename Base> class SeExprNativeFx : public Base, public TFxSeExprSource {
protected:
  TStringParamP m_document;
  TDoubleParamP m_width, m_height, m_fps, m_seed, m_mix, m_radius;
  TIntEnumParamP m_edge, m_filter, m_maskChannel;
  TRasterFxPort m_source, m_mask;
  bool m_generator, m_syncing = false, m_loading = false;
  std::string m_schemaError;
  std::vector<std::unique_ptr<NativeControl>> m_controls;
  std::map<QString, NativeControl *> m_controlMap;

  template <typename Param> void registerControl(const std::string &key, Param &parameter,
                                                 const std::string &label) {
    TFx *linked = this->getLinkedFx();
    if (linked && linked != this) {
      if (TParam *existing = linked->getParams()->getParam(key)) parameter = TParamP(existing);
    }
    parameter->setUILabel(label);
    auto inheritGrammar = [&](auto &&self, TParam *value) -> void {
      if (auto *number = dynamic_cast<TDoubleParam *>(value)) number->setGrammar(m_fps->getGrammar());
      else if (auto *group = dynamic_cast<TParamSet *>(value))
        for (int i = 0; i < group->getParamCount(); ++i) self(self, group->getParam(i).getPointer());
    };
    inheritGrammar(inheritGrammar, parameter.getPointer());
    bindParam(this, key, parameter);
  }

  void createControl(NativeControl &control) {
    const auto &spec = control.spec;
    const QJsonObject object = spec.definition;
    const std::string key = std::string(controlPrefix) + spec.id.toStdString() + "__";
    const std::string label = spec.label.toStdString();
    const double minimum = number(object.value("min"), -1000000);
    const double maximum = number(object.value("max"), 1000000);
    if (spec.type == "scalar") {
      control.scalar = new TDoubleParam(number(object.value("value")));
      control.scalar->setValueRange(minimum, maximum);
      registerControl(key + "value", control.scalar, label);
    } else if (spec.type == "vector") {
      const auto value = triple(object.value("value"));
      for (int index = 0; index < 3; ++index) {
        control.vector[index] = new TDoubleParam(value[index]);
        control.vector[index]->setValueRange(minimum, maximum);
        registerControl(key + "xyz"[index], control.vector[index],
                        label + " " + "XYZ"[index]);
      }
    } else if (spec.type == "color") {
      const auto value = triple(object.value("value"));
      control.color = new TPixelParam;
      control.color->getRed()->setDefaultValue(value[0]);
      control.color->getGreen()->setDefaultValue(value[1]);
      control.color->getBlue()->setDefaultValue(value[2]);
      control.color->getMatte()->setDefaultValue(1);
      control.color->enableMatte(false);
      registerControl(key + "value", control.color, label);
    } else if (spec.type == "string") {
      control.text = new TStringParam(object.value("value").toString().toStdWString());
      registerControl(key + "value", control.text, label);
    } else if (spec.definition.value("presentation").toString() == "spectrum") {
      const auto values = spectrumValues(spec.definition.value("spectrum").toObject());
      for (const auto &value : values) {
        auto inserted = control.spectrum.emplace(value.first, new TDoubleParam(value.second));
        if (value.first == "resolution") inserted.first->second->setValueRange(1, 20);
        registerControl(key + "s_" + value.first, inserted.first->second,
                        label + " " + value.first);
      }
    } else {
      allocateKnots(control);
    }
  }

  void allocateKnots(NativeControl &control) {
    const auto points = control.spec.definition.value("points").toArray();
    const int count = std::max(int(points.size()), control.spec.definition.value("nativeKnots").toInt());
    demand(count <= 64, "saved native curve has too many stops");
    const std::string key = std::string(controlPrefix) + control.spec.id.toStdString() + "__";
    for (int index = control.allocatedKnots; index < count; ++index) {
      const auto point = points[std::min(index, int(points.size()) - 1)].toArray();
      control.positions[index] = new TDoubleParam(number(point[0]));
      control.positions[index]->setValueRange(0, 1);
      registerControl(key + "p" + std::to_string(index), control.positions[index],
                      control.spec.label.toStdString() + " Position " + std::to_string(index + 1));
      if (control.spec.type == "curve") {
        control.values[index] = new TDoubleParam(number(point[1]));
        registerControl(key + "v" + std::to_string(index), control.values[index],
                        control.spec.label.toStdString() + " Value " + std::to_string(index + 1));
      } else {
        const auto v = triple(point[1]);
        control.colors[index] = new TPixelParam;
        control.colors[index]->getRed()->setDefaultValue(v[0]);
        control.colors[index]->getGreen()->setDefaultValue(v[1]);
        control.colors[index]->getBlue()->setDefaultValue(v[2]);
        control.colors[index]->getMatte()->setDefaultValue(1);
        control.colors[index]->enableMatte(false);
        registerControl(key + "v" + std::to_string(index), control.colors[index],
                        control.spec.label.toStdString() + " Color " + std::to_string(index + 1));
      }
      ++control.allocatedKnots;
    }
  }

  void synchronizeDocument(bool strict = false) {
    if (m_syncing) return;
    m_syncing = true;
    struct Reset { bool &flag; ~Reset() { flag = false; } } reset{m_syncing};
    std::string identityError;
    try {
      const int oldCount = this->getParams()->getParamCount();
      SeExprFx::Document document;
      try {
        document = SeExprFx::parseDocument(m_document->getValue());
      } catch (const TException &error) {
        identityError = QString::fromStdWString(error.getMessage()).toStdString();
        if (strict) throw;
        document = SeExprFx::parseDocumentContents(m_document->getValue(), false);
      }
      std::set<QString> present;
      for (const auto &spec : document.controls) {
        const auto known = m_controlMap.find(spec.id);
        demand(known == m_controlMap.end() || known->second->spec.type == spec.type,
               "control type changes require a new stable ID");
        demand(known == m_controlMap.end() ||
                   (known->second->spec.definition.value("presentation").toString() == "spectrum") ==
                   (spec.definition.value("presentation").toString() == "spectrum"),
               "spectrum/curve conversion requires a new stable ID");
      }
      for (const auto &spec : document.controls) {
        present.insert(spec.id);
        auto found = m_controlMap.find(spec.id);
        if (found != m_controlMap.end()) {
          NativeControl &native = *found->second;
          const QJsonObject previous = native.spec.definition;
          native.spec = spec;
          if (spec.definition.value("presentation").toString() == "spectrum") {
            const auto values = spectrumValues(spec.definition.value("spectrum").toObject());
            const auto old = spectrumValues(previous.value("spectrum").toObject());
            for (const auto &value : values)
              if (value.second != old.at(value.first)) native.spectrum.at(value.first)->setDefaultValue(value.second);
          } else if (spec.type == "curve" || spec.type == "colorcurve") allocateKnots(native);
          if (previous.value("value") != spec.definition.value("value")) {
            if (spec.type == "scalar") native.scalar->setDefaultValue(number(spec.definition.value("value")));
            else if (spec.type == "vector") { const auto value = triple(spec.definition.value("value"));
              for (int i = 0; i < 3; ++i) native.vector[i]->setDefaultValue(value[i]); }
            else if (spec.type == "color") { const auto value = triple(spec.definition.value("value"));
              native.color->getRed()->setDefaultValue(value[0]);
              native.color->getGreen()->setDefaultValue(value[1]);
              native.color->getBlue()->setDefaultValue(value[2]); }
            else if (spec.type == "string") native.text->setValue(spec.definition.value("value").toString().toStdWString(), false);
          }
          if (spec.definition.value("presentation").toString() != "spectrum" &&
              previous.value("points") != spec.definition.value("points")) {
            const auto points = spec.definition.value("points").toArray();
            for (int i = 0; i < points.size(); ++i) {
              const auto pair = points[i].toArray(); native.positions[i]->setDefaultValue(number(pair[0]));
              if (spec.type == "curve") native.values[i]->setDefaultValue(number(pair[1]));
              else { const auto value = triple(pair[1]);
                native.colors[i]->getRed()->setDefaultValue(value[0]);
                native.colors[i]->getGreen()->setDefaultValue(value[1]);
                native.colors[i]->getBlue()->setDefaultValue(value[2]); }
            }
          }
        } else {
          std::unique_ptr<NativeControl> control(new NativeControl);
          control->spec = spec;
          createControl(*control);
          m_controlMap.emplace(spec.id, control.get());
          m_controls.push_back(std::move(control));
        }
      }
      QJsonArray archive, registry, active;
      for (auto &control : m_controls) {
        registry.append(control->spec.id);
        if (!present.count(control->spec.id)) control->spec.active = false;
        if (control->allocatedKnots) control->spec.definition.insert("nativeKnots", control->allocatedKnots);
        if (!control->spec.active) archive.append(control->spec.definition);
        const std::string key = std::string(controlPrefix) + control->spec.id.toStdString() + "__";
        for (int i = 0; i < this->getParams()->getParamCount(); ++i) {
          const std::string name = this->getParams()->getParamName(i);
          if (name.compare(0, key.size(), key) != 0) continue;
          bool hidden = !control->spec.active;
          const std::string suffix = name.substr(key.size());
          if (control->allocatedKnots && suffix.size() > 1) {
            const int index = std::stoi(suffix.substr(1));
            hidden = hidden || index >= control->spec.definition.value("points").toArray().size();
          }
          this->getParams()->getParamVar(name)->setIsHidden(hidden);
          this->getParams()->getParam(i)->setUILabel(control->spec.label.toStdString() +
              (suffix == "value" ? "" : " " + suffix));
        }
      }
      if (identityError.empty()) {
        for (const auto item : document.json.value("controls").toArray())
          active.append(m_controlMap.at(item.toObject().value("id").toString())->spec.definition);
        document.json.insert("controls", active);
        document.json.insert("archive", archive);
        document.json.insert("registry", registry);
        document.json.insert("engine", QString::fromStdString(TSeExpr::Program::engineVersion()));
        document.json.insert("functionPolicy", int(TSeExpr::Program::functionPolicyVersion()));
        m_document->setValue(QString::fromUtf8(QJsonDocument(document.json).toJson(QJsonDocument::Compact)).toStdWString(), false);
      }
      // Native storage can be recognized without granting permission to run
      // its source. Never replace an unsupported document's identity or JSON.
      m_schemaError = identityError;
      if (this->getParams()->getParamCount() != oldCount) this->notify(TFxParamAdded(this));
    } catch (const TException &error) {
      m_schemaError = identityError.empty()
                          ? QString::fromStdWString(error.getMessage()).toStdString()
                          : identityError;
      // Loading must retain an unsupported declaration verbatim so it can be
      // inspected and repaired. The recorded error still prevents rendering;
      // only internally constructed declarations require an immediate throw.
      if (strict) throw;
    }
  }

  std::vector<TSeExpr::Variable> variablesFor(const SeExprFx::Document &document, double frame,
                                                bool linear = false, double gamma = 2.2) const {
    std::vector<TSeExpr::Variable> result;
    auto scalar = [&](const std::string &name, double value) { result.push_back({name, TSeExpr::Value::scalar(value)}); };
    const double fps = finite(m_fps->getValue(frame), "frame rate must be finite");
    demand(fps > 0, "frame rate must be positive");
    scalar("x", 0); scalar("y", 0); scalar("u", 0); scalar("v", 0);
    scalar("width", m_width->getValue(frame)); scalar("height", m_height->getValue(frame));
    scalar("frame", frame + 1); scalar("time", frame / fps); scalar("fps", fps);
    scalar("seed", m_seed->getValue(frame));
    scalar("r", 0); scalar("g", 0); scalar("b", 0); scalar("a", 0);
    result.push_back({"src", TSeExpr::Value::vector(0, 0, 0)});
    for (const auto &spec : document.controls) {
      if (!spec.active || spec.type == "curve" || spec.type == "colorcurve") continue;
      const auto found = m_controlMap.find(spec.id);
      const NativeControl *control = found == m_controlMap.end() || found->second->spec.type != spec.type ? nullptr : found->second;
      const bool changedDefault = control && control->spec.definition.value("value") != spec.definition.value("value");
      if (spec.type == "scalar") {
        double value = control && (!changedDefault || control->scalar->hasKeyframes()) ? control->scalar->getValue(frame) : number(spec.definition.value("value"));
        if (spec.definition.value("integer").toBool())
          value = std::clamp(std::round(value), std::ceil(number(spec.definition.value("min"), -1000000)),
                             std::floor(number(spec.definition.value("max"), 1000000)));
        scalar(spec.name.toStdString(), value);
      }
      else if (spec.type == "string") result.push_back({spec.name.toStdString(), TSeExpr::Value::string(
          control && !changedDefault ? QString::fromStdWString(control->text->getValue()).toStdString() : spec.definition.value("value").toString().toStdString())});
      else {
        auto value = triple(spec.definition.value("value"));
        if (control && spec.type == "vector") for (int i = 0; i < 3; ++i)
          if (!changedDefault || control->vector[i]->hasKeyframes()) value[i] = control->vector[i]->getValue(frame);
        if (control && spec.type == "color" && (!changedDefault || control->color->hasKeyframes())) {
          const auto p = control->color->getValueD(frame);
          value = {{p.r, p.g, p.b}};
        }
        if (spec.type == "color") for (double &component : value) component = workingColor(component, linear, gamma);
        result.push_back({spec.name.toStdString(), TSeExpr::Value::vector(value[0], value[1], value[2])});
      }
    }
    return result;
  }

  std::vector<TSeExpr::Function> functionsFor(const SeExprFx::Document &document,
                                            double frame, InputSampler *sampler,
                                            bool linear = false, double gamma = 2.2) const {
    std::vector<TSeExpr::Function> result;
    TSeExpr::Function rgb, alpha;
    rgb.name = "sample_rgb"; rgb.documentation = "sample_rgb(dx,dy): straight source RGB at a local-pixel offset within Sampling Radius";
    rgb.argumentTypes = {TSeExpr::Type::Scalar, TSeExpr::Type::Scalar};
    rgb.resultType = TSeExpr::Type::Vector;
    rgb.evaluate = [sampler](const std::vector<TSeExpr::Value> &arguments) {
      const auto p = sampler ? sampler->sample(arguments[0].number[0], arguments[1].number[0]) : TPixelD(0,0,0,0);
      return TSeExpr::Value::vector(p.r, p.g, p.b);
    };
    alpha = rgb; alpha.name = "sample_alpha"; alpha.documentation = "sample_alpha(dx,dy): source alpha at a bounded local-pixel offset";
    alpha.resultType = TSeExpr::Type::Scalar;
    alpha.evaluate = [sampler](const std::vector<TSeExpr::Value> &arguments) {
      return TSeExpr::Value::scalar(sampler ? sampler->sample(arguments[0].number[0], arguments[1].number[0]).m : 0);
    };
    result.push_back(rgb); result.push_back(alpha);
    for (const auto &spec : document.controls) {
      if (!spec.active || (spec.type != "curve" && spec.type != "colorcurve")) continue;
      TSeExpr::Function function;
      function.name = spec.name.toStdString(); function.argumentTypes = {TSeExpr::Type::Scalar};
      function.documentation = "Native animatable curve: " + spec.label.toStdString();
      const auto found = m_controlMap.find(spec.id);
      const NativeControl *native = found != m_controlMap.end() && found->second->spec.type == spec.type ? found->second : nullptr;
      if (spec.definition.value("presentation").toString() == "spectrum") {
        auto values = spectrumValues(spec.definition.value("spectrum").toObject());
        if (native) {
          const auto old = spectrumValues(native->spec.definition.value("spectrum").toObject());
          for (auto &value : values) {
            const auto parameter = native->spectrum.at(value.first);
            if (old.at(value.first) == value.second || parameter->hasKeyframes()) value.second = parameter->getValue(frame);
          }
        }
        const auto parameters = spectrumParameters(values);
        function.resultType = TSeExpr::Type::Scalar;
        function.evaluate = [parameters](const std::vector<TSeExpr::Value> &args) {
          return TSeExpr::Value::scalar(TSeExprSpectrum::value(parameters, args[0].number[0]));
        };
        result.push_back(std::move(function)); continue;
      }
      const bool colorCurve = spec.type == "colorcurve";
      function.resultType = colorCurve ? TSeExpr::Type::Vector : TSeExpr::Type::Scalar;
      std::ostringstream source;
      source.imbue(std::locale::classic()); source << std::setprecision(17);
      source << (colorCurve ? "ccurve($t" : "curve($t");
      const auto points = spec.definition.value("points").toArray();
      for (int index = 0; index < points.size(); ++index) {
        const auto point = points[index].toArray();
        const bool available = native && index < native->allocatedKnots;
        const auto previous = available ? native->spec.definition.value("points").toArray() : QJsonArray();
        const auto prior = index < previous.size() ? previous[index].toArray() : QJsonArray();
        const bool samePosition = !prior.isEmpty() && prior[0] == point[0];
        const bool sameValue = prior.size() > 1 && prior[1] == point[1];
        const double position = available && (samePosition || native->positions[index]->hasKeyframes()) ? native->positions[index]->getValue(frame) : number(point[0]);
        source << ',' << finite(position, "curve position is not finite") << ',';
        if (colorCurve) {
          auto v = triple(point[1]);
          if (available && (sameValue || native->colors[index]->hasKeyframes())) { const auto p = native->colors[index]->getValueD(frame); v = {{p.r, p.g, p.b}}; }
          source << '[' << workingColor(v[0], linear, gamma) << ',' << workingColor(v[1], linear, gamma)
                 << ',' << workingColor(v[2], linear, gamma) << ']';
        } else source << finite(available && (sameValue || native->values[index]->hasKeyframes()) ? native->values[index]->getValue(frame) : number(point[1]), "curve value must be finite");
        source << ',' << (point.size() == 3 ? int(number(point[2])) : 1);
      }
      source << ')';
      auto program = std::make_shared<TSeExpr::Program>();
      TSeExpr::Options options; options.resultType = function.resultType;
      options.backend = document.backend == "llvm" ? TSeExpr::Backend::LLVM : TSeExpr::Backend::Interpreter;
      if (!program->compile(source.str(), {{"t", TSeExpr::Value::scalar(0)}}, {}, options))
        throw TException("SeExpr curve: " + program->errorMessage());
      function.evaluate = [program](const std::vector<TSeExpr::Value> &args) {
        TSeExpr::Value value;
        if (!program->setScalar("t", args[0].number[0])) throw TException("SeExpr curve: " + program->errorMessage());
        if (!program->evaluate(value)) throw TException("SeExpr curve: " + program->errorMessage());
        return value;
      };
      result.push_back(std::move(function));
    }
    for (auto &function : result) {
      auto evaluate = function.evaluate;
      function.evaluate = [evaluate](const std::vector<TSeExpr::Value> &arguments) {
        try { return evaluate(arguments); }
        catch (const TException &error) {
          throw std::runtime_error(QString::fromStdWString(error.getMessage()).toStdString());
        }
      };
    }
    return result;
  }

  TRectD localCanvas(double frame, const TRenderSettings &settings) const {
    if (!m_generator && m_source.isConnected()) {
      TRenderSettings identity = settings; identity.m_affine = TAffine();
      TRectD box;
      if (static_cast<TRasterFx *>(m_source.getFx())->getBBox(frame, box, identity) &&
          finiteBox(box) && !box.isEmpty()) return box;
    }
    const double width = finite(m_width->getValue(frame), "canvas width must be finite");
    const double height = finite(m_height->getValue(frame), "canvas height must be finite");
    demand(width >= 1 && height >= 1 && width <= 65536 && height <= 65536, "canvas dimensions must be within one to 65536 pixels");
    return TRectD(-width * .5, -height * .5, width * .5, height * .5);
  }
  double support(double frame, const TRenderSettings &settings) const {
    const double radius = finite(m_radius->getValue(frame), "sampling radius must be finite");
    demand(radius >= 0 && radius <= 4096, "sampling radius must be within zero to 4096 pixels");
    const auto document = SeExprFx::parseDocument(m_document->getValue());
    // Conservative token scan: comments between a function name and '(' must
    // not shrink its required halo. Mentions in comments may over-request.
    static const QRegularExpression calls(QStringLiteral("\\bsample_(rgb|alpha)\\b"));
    const bool sampling = calls.match(document.rgb).hasMatch() || calls.match(document.alpha).hasMatch();
    return (sampling ? radius : 0) * std::max(std::abs(settings.m_affine.a11) + std::abs(settings.m_affine.a12),
                              std::abs(settings.m_affine.a21) + std::abs(settings.m_affine.a22)) + 2;
  }
  TRectD inputRegion(const TRectD &requested, double frame, const TRenderSettings &settings) const {
    if (m_edge->getValue() == 0) return rasterBox(requested.enlarge(support(frame, settings)));
    TRectD box;
    if (!static_cast<TRasterFx *>(m_source.getFx())->getBBox(frame, box, settings) || box.isEmpty())
      return TRectD();
    demand(finiteBox(box), "clamp/repeat/mirror sampling needs a finite Source canvas");
    return rasterBox(box);
  }

  template <typename Pixel>
  void render(const TRasterPT<Pixel> &output, const TPointD &position, double frame,
              const TRenderSettings &settings, InputSampler &sampler, const TTile &mask) {
    const auto document = SeExprFx::parseDocument(m_document->getValue());
    auto variables = variablesFor(document, frame, settings.m_linearColorSpace, settings.m_colorSpaceGamma);
    struct Prepared {
      TSeExpr::Program rgb, alpha;
      InputSampler sampler;
      const int *cancelFlag = nullptr;
    };
    struct CacheEntry { std::string key; std::unique_ptr<Prepared> prepared; };
    static thread_local std::list<CacheEntry> cache;
    std::ostringstream identity;
    identity.imbue(std::locale::classic()); identity << std::setprecision(17);
    identity << QString::fromStdWString(m_document->getValue()).toUtf8().constData()
             << ',' << settings.m_linearColorSpace << ',' << settings.m_colorSpaceGamma;
    // Scalar/vector values are updated without recompiling. Curves contain
    // frame-dependent native stop constants and therefore qualify the cache.
    for (const auto &control : m_controls) {
      if (!control->spec.active || (control->spec.type != "curve" && control->spec.type != "colorcurve")) continue;
      const std::string prefix = std::string(controlPrefix) + control->spec.id.toStdString() + "__";
      for (int i = 0; i < this->getParams()->getParamCount(); ++i) {
        TParam *parameter = this->getParams()->getParam(i);
        if (parameter->getName().compare(0, prefix.size(), prefix) == 0)
          identity << ',' << parameter->getName() << '=' << parameter->getValueAlias(frame, 17);
      }
    }
    const std::string key = identity.str();
    auto found = std::find_if(cache.begin(), cache.end(), [&](const CacheEntry &entry) { return entry.key == key; });
    if (found == cache.end()) {
      std::unique_ptr<Prepared> entry(new Prepared);
      entry->sampler = sampler; entry->cancelFlag = settings.m_isCanceled;
      const auto functions = functionsFor(document, frame, &entry->sampler,
                                           settings.m_linearColorSpace, settings.m_colorSpaceGamma);
      TSeExpr::Options options;
      options.cancelled = [owned = entry.get()]() { return canceled(owned->cancelFlag); };
      options.backend = document.backend == "llvm" ? TSeExpr::Backend::LLVM : TSeExpr::Backend::Interpreter;
      if (!entry->rgb.compile(document.rgb.toStdString(), variables, functions, options))
        throw TException("SeExpr RGB: " + entry->rgb.errorMessage());
      options.resultType = TSeExpr::Type::Scalar;
      if (!entry->alpha.compile(document.alpha.toStdString(), variables, functions, options))
        throw TException("SeExpr alpha: " + entry->alpha.errorMessage());
      cache.push_front({key, std::move(entry)});
      if (cache.size() > 8) cache.pop_back();
    } else cache.splice(cache.begin(), cache, found);
    Prepared &prepared = *cache.front().prepared;
    prepared.sampler = sampler; prepared.cancelFlag = settings.m_isCanceled;
    struct ReleaseInput {
      Prepared &value;
      ~ReleaseInput() { value.sampler.image = TRasterP(); value.cancelFlag = nullptr; }
    } release{prepared};
    auto &rgb = prepared.rgb; auto &alpha = prepared.alpha;
    for (const auto &variable : variables) {
      if (!rgb.setVariable(variable.name, variable.value)) throw TException("SeExpr RGB: " + rgb.errorMessage());
      if (!alpha.setVariable(variable.name, variable.value)) throw TException("SeExpr alpha: " + alpha.errorMessage());
    }
    const TRectD canvas = localCanvas(frame, settings);
    const TAffine inverse = settings.m_affine.inv();
    const double mix = unit(finite(m_mix->getValue(frame), "mix must be finite"));
    auto set = [&](const char *name, double value) {
      if (!rgb.setScalar(name, value)) throw TException("SeExpr RGB: " + rgb.errorMessage());
      if (!alpha.setScalar(name, value)) throw TException("SeExpr alpha: " + alpha.errorMessage());
    };
    set("width", canvas.getLx()); set("height", canvas.getLy());
    RasterLock outputLock(output), sourceLock(sampler.image), maskLock(mask.getRaster());
    for (int y = 0; y < output->getLy(); ++y) {
      if (canceled(settings.m_isCanceled)) { output->clear(); return; }
      Pixel *row = output->pixels(y);
      for (int x = 0; x < output->getLx(); ++x) {
        if ((x & 63) == 0 && canceled(settings.m_isCanceled)) { output->clear(); return; }
        const TPointD global = position + TPointD(x + .5, y + .5);
        const TPointD point = inverse * global;
        if (m_generator && (point.x < canvas.x0 || point.y < canvas.y0 || point.x >= canvas.x1 || point.y >= canvas.y1)) continue;
        prepared.sampler.current = point;
        const TPixelD original = m_generator ? TPixelD(0,0,0,0) : sampler.readAt(global);
        const TPixelD source = straight(original);
        set("x", point.x); set("y", point.y);
        set("u", (point.x - canvas.x0) / canvas.getLx()); set("v", (point.y - canvas.y0) / canvas.getLy());
        set("r", source.r); set("g", source.g); set("b", source.b); set("a", source.m);
        if (!rgb.setVector("src", source.r, source.g, source.b)) throw TException("SeExpr RGB: " + rgb.errorMessage());
        if (!alpha.setVector("src", source.r, source.g, source.b)) throw TException("SeExpr alpha: " + alpha.errorMessage());
        TSeExpr::Value colorValue, alphaValue;
        if (!rgb.evaluate(colorValue)) throw TException("SeExpr RGB: " + rgb.errorMessage());
        if (!alpha.evaluate(alphaValue)) throw TException("SeExpr alpha: " + alpha.errorMessage());
        const double a = unit(alphaValue.number[0]);
        const TPixelD processed(colorValue.number[0] * a, colorValue.number[1] * a, colorValue.number[2] * a, a);
        double weight = mix;
        if (mask.getRaster()) {
          const auto p = pixelAt(mask.getRaster(), x, y);
          demand(std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b) && std::isfinite(p.m),
                 "Mask contains nonfinite pixels");
          weight *= m_maskChannel->getValue() == 0 ? unit(p.m) : unit(.2126 * p.r + .7152 * p.g + .0722 * p.b);
        }
        const TPixelD result = m_generator ? processed : blendPixels(original, processed, weight);
        row[x] = converted(result, static_cast<Pixel *>(nullptr));
      }
    }
  }

public:
  explicit SeExprNativeFx(bool generator)
      : m_document(SeExprFx::initialDocument(generator)), m_width(1920.0), m_height(1080.0),
        m_fps(24.0), m_seed(0.0), m_mix(1.0), m_radius(32.0), m_edge(0, "Transparent"), m_filter(1, "Bilinear"),
        m_maskChannel(0, "Alpha"), m_generator(generator) {
    // The declaration is saved first, so onParamLoaded can construct all
    // dynamic parameters before either scene or native preset value loading.
    this->getParams()->setParamObserver(this);
    m_document->setMultiLineEnabled(true);
    bindParam(this, documentName, m_document);
    bindParam(this, "width", m_width); bindParam(this, "height", m_height);
    bindParam(this, "fps", m_fps); bindParam(this, "seed", m_seed);
    bindParam(this, "mix", m_mix); bindParam(this, "samplingRadius", m_radius);
    bindParam(this, "edgeMode", m_edge); bindParam(this, "samplingFilter", m_filter);
    bindParam(this, "maskChannel", m_maskChannel);
    m_width->setValueRange(1, 65536); m_height->setValueRange(1, 65536);
    m_fps->setValueRange(.001, 10000); m_mix->setValueRange(0, 1); m_radius->setValueRange(0, 4096);
    m_edge->addItem(1, "Clamp"); m_edge->addItem(2, "Repeat"); m_edge->addItem(3, "Mirror");
    m_filter->addItem(0, "Nearest");
    m_maskChannel->addItem(1, "Luminance");
    if (!generator) { this->addInputPort("Source", m_source); this->addInputPort("Mask", m_mask); }
    this->enableComputeInFloat(true);
    synchronizeDocument(true);
  }
  ~SeExprNativeFx() override {
    for (int i = 0; i < this->getParams()->getParamCount(); ++i)
      this->getParams()->getParamVar(this->getParams()->getParamName(i))->setParamObserver(nullptr);
  }
  TStringParam *seExprDocumentParam() const override { return m_document.getPointer(); }
  bool validateSeExprDocument(const std::wstring &text, double frame, std::string &error,
                              std::vector<TSeExpr::Error> *diagnostics = nullptr) const override {
    try {
      const auto document = SeExprFx::parseDocument(text);
      for (const auto &control : document.controls) {
        const auto existing = m_controlMap.find(control.id);
        demand(existing == m_controlMap.end() ||
                   (existing->second->spec.type == control.type &&
                    (existing->second->spec.definition.value("presentation").toString() == "spectrum") ==
                    (control.definition.value("presentation").toString() == "spectrum")),
               "changing a control type or spectrum kind requires a new stable ID");
      }
      const auto variables = variablesFor(document, frame);
      const auto functions = functionsFor(document, frame, nullptr);
      for (int channel = 0; channel < 2; ++channel) {
        TSeExpr::Program program; TSeExpr::Options options;
        options.backend = document.backend == "llvm" ? TSeExpr::Backend::LLVM : TSeExpr::Backend::Interpreter;
        options.resultType = channel == 0 ? TSeExpr::Type::Vector : TSeExpr::Type::Scalar;
        if (!program.compile((channel ? document.alpha : document.rgb).toStdString(), variables, functions, options)) {
          error = std::string(channel ? "Alpha: " : "RGB: ") + program.errorMessage();
          if (diagnostics) *diagnostics = program.errors();
          return false;
        }
      }
      error.clear(); if (diagnostics) diagnostics->clear(); return true;
    } catch (const TException &failure) { error = QString::fromStdWString(failure.getMessage()).toStdString(); return false; }
  }
  std::vector<TSeExpr::Variable> seExprVariables(double frame) const override {
    return variablesFor(SeExprFx::parseDocument(m_document->getValue()), frame);
  }
  std::vector<TSeExpr::Function> seExprFunctions(double frame) const override {
    return functionsFor(SeExprFx::parseDocument(m_document->getValue()), frame, nullptr);
  }
  bool sampleSeExprDocument(const std::wstring &text, double frame,
                            TSeExpr::Value &rgbValue, double &alphaResult,
                            std::string &error) const override {
    if (!validateSeExprDocument(text, frame, error)) return false;
    try {
      const auto document = SeExprFx::parseDocument(text);
      auto variables = variablesFor(document, frame);
      for (auto &variable : variables)
        if (variable.name == "u" || variable.name == "v") variable.value = TSeExpr::Value::scalar(.5);
      const auto functions = functionsFor(document, frame, nullptr);
      TSeExpr::Value alphaValue;
      for (int channel = 0; channel < 2; ++channel) {
        TSeExpr::Program program; TSeExpr::Options options;
        options.backend = document.backend == "llvm" ? TSeExpr::Backend::LLVM : TSeExpr::Backend::Interpreter;
        options.resultType = channel ? TSeExpr::Type::Scalar : TSeExpr::Type::Vector;
        if (!program.compile((channel ? document.alpha : document.rgb).toStdString(), variables, functions, options) ||
            !program.evaluate(channel ? alphaValue : rgbValue)) { error = program.errorMessage(); return false; }
      }
      alphaResult = unit(alphaValue.number[0]); error.clear(); return true;
    } catch (const TException &failure) { error = QString::fromStdWString(failure.getMessage()).toStdString(); return false; }
  }
  bool previewSeExprDocument(const std::wstring &text, double frame,
                             std::string &value, std::string &error) const override {
    TSeExpr::Value color; double alpha = 0;
    if (!sampleSeExprDocument(text, frame, color, alpha, error)) return false;
    std::ostringstream output; output.imbue(std::locale::classic());
    output << std::setprecision(8) << "RGB [" << color.number[0] << ", " << color.number[1]
           << ", " << color.number[2] << "], alpha " << alpha;
    value = output.str(); return true;
  }
  void onChange(const TParamChange &change) override {
    if (m_syncing) return;
    synchronizeDocument();
    Base::onChange(change);
  }
  void onParamLoaded(const std::string &name) override {
    if (name == documentName) synchronizeDocument();
  }
  void loadData(TIStream &stream) override {
    const bool previous = m_loading;
    m_loading = true;
    struct Reset {
      bool &flag;
      bool previous;
      ~Reset() { flag = previous; }
    } reset{m_loading, previous};
    Base::loadData(stream);
  }
  TFx *clone(bool recursive = true) const override {
    std::unique_ptr<TFx> copy(TFx::create(this->getFxType()));
    auto *native = dynamic_cast<SeExprNativeFx *>(copy.get());
    demand(native != nullptr, "effect factory did not preserve its native type");
    // An invalid declaration can follow an otherwise valid, animated schema.
    // Recreate the already registered native controls independently of that
    // declaration, then retain the original source and its diagnostic. This
    // also keeps the native parameter permutation required by TFx::clone.
    for (const auto &source : m_controls) {
      std::unique_ptr<NativeControl> control(new NativeControl);
      control->spec = source->spec;
      if (source->allocatedKnots)
        control->spec.definition.insert("nativeKnots", source->allocatedKnots);
      native->createControl(*control);
      native->m_controlMap.emplace(control->spec.id, control.get());
      native->m_controls.push_back(std::move(control));
    }
    native->m_document->setValue(m_document->getValue(), false);
    native->synchronizeDocument();
    std::vector<std::string> order;
    for (int i = 0; i < this->getParams()->getParamCount(); ++i) {
      const std::string name = this->getParams()->getParamName(i);
      order.push_back(name);
      native->getParams()->getParamVar(name)->setIsHidden(this->getParams()->isParamHidden(i));
    }
    native->getParams()->reorder(order);
    TFx *result = this->TFx::clone(copy.get(), recursive);
    copy.release(); return result;
  }
  void linkParams(TFx *other) override {
    if (other == this) return;
    auto *source = dynamic_cast<SeExprNativeFx *>(other);
    demand(source && source->getFxType() == this->getFxType(), "linked expressions must have the same effect type");
    SeExprFx::Document merged;
    try {
      merged = SeExprFx::parseDocument(source->m_document->getValue());
    } catch (const TException &error) {
      const auto declaration = source->m_document->getValue();
      // A linked peer is serialized with paramsLinkedTo instead of its own
      // parameter declaration. Only that fresh load may adopt unsupported
      // source verbatim; explicit links require an identical declaration.
      demand(m_document->getValue() == declaration || (m_loading && m_controls.empty()),
             "unsupported linked expressions need identical documents");
      if (m_document->getValue() != declaration) m_document->setValue(declaration);
      synchronizeDocument();
      demand(this->getParams()->getParamCount() == source->getParams()->getParamCount(),
             "unsupported linked expressions have incompatible native layouts");
      std::vector<std::string> order;
      for (int i = 0; i < source->getParams()->getParamCount(); ++i) {
        const std::string name = source->getParams()->getParamName(i);
        const TParam *ours = this->getParams()->getParam(name);
        const TParam *theirs = source->getParams()->getParam(i);
        demand(ours && typeid(*ours) == typeid(*theirs),
               "unsupported linked expressions have incompatible native parameter types");
        order.push_back(name);
      }
      this->getParams()->reorder(order);
      const auto diagnostic = QString::fromStdWString(error.getMessage()).toStdString();
      m_schemaError = source->m_schemaError = diagnostic;
      Base::linkParams(other);
      return;
    }
    std::set<QString> known;
    for (const auto &control : merged.controls) known.insert(control.id);
    QJsonArray archive = merged.json.value("archive").toArray();
    for (const auto &control : m_controls) {
      if (known.insert(control->spec.id).second) archive.append(control->spec.definition);
      else {
        const auto &original = source->m_controlMap.at(control->spec.id)->spec;
        demand(original.type == control->spec.type &&
                   (original.definition.value("presentation").toString() == "spectrum") ==
                   (control->spec.definition.value("presentation").toString() == "spectrum"),
               "linking would change a stable control type");
      }
    }
    merged.json.insert("archive", archive);
    for (const QString group : {QString("controls"), QString("archive")}) {
      QJsonArray entries;
      for (const auto item : merged.json.value(group).toArray()) {
        auto spec = item.toObject(); const auto ours = m_controlMap.find(spec.value("id").toString());
        if (ours != m_controlMap.end() && ours->second->allocatedKnots)
          spec.insert("nativeKnots", std::max(spec.value("nativeKnots").toInt(), ours->second->allocatedKnots));
        entries.append(spec);
      }
      merged.json.insert(group, entries);
    }
    // Notify every previously linked peer so a growing schema is constructed
    // before native parameter pointers are shared by the normal link path.
    source->m_document->setValue(QString::fromUtf8(QJsonDocument(merged.json).toJson(QJsonDocument::Compact)).toStdWString());
    m_document->setValue(source->m_document->getValue(), false);
    synchronizeDocument(true);
    std::vector<std::string> order;
    for (int i = 0; i < source->getParams()->getParamCount(); ++i) order.push_back(source->getParams()->getParamName(i));
    this->getParams()->reorder(order);
    Base::linkParams(other);
  }
  bool requiresFullPrecisionSerialization() const override { return true; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool toBeComputedInLinearColorSpace(bool settingsIsLinear, bool) const override {
    return settingsIsLinear;
  }
  bool doGetBBox(double frame, TRectD &box, const TRenderSettings &settings) override {
    if (m_generator) { box = localCanvas(frame, settings); return !box.isEmpty(); }
    if (!m_source.isConnected()) { box = TRectD(); return false; }
    TRenderSettings identity = settings; identity.m_affine = TAffine();
    return m_source->getBBox(frame, box, identity);
  }
  void transform(double frame, int port, const TRectD &rect, const TRenderSettings &settings,
                  TRectD &input, TRenderSettings &inputSettings) override {
    inputSettings = settings;
    input = !m_generator && port == 0 && m_source.isConnected() ? inputRegion(rect, frame, settings) : rect;
  }
  void doDryCompute(TRectD &rect, double frame, const TRenderSettings &settings) override {
    if (m_generator || !m_source.isConnected()) return;
    TRectD input = inputRegion(rect, frame, settings);
    if (!input.isEmpty()) m_source->dryCompute(input, frame, settings);
    if (m_mask.isConnected()) { TRectD mask = rect; m_mask->dryCompute(mask, frame, settings); }
  }
  int getMemoryRequirement(const TRectD &rect, double frame, const TRenderSettings &settings) override {
    if (m_generator || !m_source.isConnected()) return 0;
    const TRectD input = inputRegion(rect, frame, settings);
    if (input.isEmpty()) return 0;
    const double bytes = double(std::max(4, settings.m_bpp / 8));
    double total = input.getLx() * input.getLy() * bytes;
    if (m_mask.isConnected()) total += rect.getLx() * rect.getLy() * bytes;
    demand(std::isfinite(total) && total <= maxWorkingBytes,
           "predicted Source and Mask exceed the working-buffer limit");
    return int(std::ceil(total / (1024 * 1024)));
  }
  std::string getAlias(double frame, const TRenderSettings &settings) const override {
    std::ostringstream alias;
    alias.imbue(std::locale::classic()); alias << std::setprecision(17);
    alias << this->getFxType() << "[seexpr-fx-v1," << TSeExpr::Program::engineVersion() << ','
          << TSeExpr::Program::functionPolicyVersion() << ',' << frame << ','
          << settings.m_linearColorSpace << ',' << settings.m_colorSpaceGamma;
    for (int index = 0; index < this->getInputPortCount(); ++index) {
      const TFxPort *port = this->getInputPort(index);
      if (port->isConnected()) alias << ',' << TRasterFxP(port->getFx())->getAlias(frame, settings);
    }
    for (int index = 0; index < this->getParams()->getParamCount(); ++index) {
      TParam *param = this->getParams()->getParam(index);
      if (param == m_document.getPointer()) {
        const auto bytes = QString::fromStdWString(m_document->getValue()).toUtf8();
        alias << ",document=" << QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().constData();
      } else alias << ',' << param->getName() << '=' << param->getValueAlias(frame, 17);
    }
    for (double value : {settings.m_affine.a11, settings.m_affine.a12, settings.m_affine.a13,
                         settings.m_affine.a21, settings.m_affine.a22, settings.m_affine.a23}) alias << ',' << value;
    alias << ']'; return alias.str();
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    tile.getRaster()->clear();
    if (canceled(settings.m_isCanceled)) return;
    demand(m_schemaError.empty(), m_schemaError.c_str());
    demand(std::isfinite(settings.m_affine.det()) && std::abs(settings.m_affine.det()) > 1e-12,
           "cannot evaluate a singular coordinate transform");
    demand(!settings.m_linearColorSpace ||
               (std::isfinite(settings.m_colorSpaceGamma) && settings.m_colorSpaceGamma > 0),
           "working color-space gamma must be finite and positive");
    const TRectD requested(tile.m_pos, TDimensionD(tile.getRaster()->getLx(), tile.getRaster()->getLy()));
    InputSampler sampler; sampler.affine = settings.m_affine;
    sampler.radius = m_radius->getValue(frame); sampler.edge = m_edge->getValue(); sampler.filter = m_filter->getValue();
    TTile source, mask;
    if (!m_generator) {
      demand(m_source.isConnected(), "Expression Color needs a connected Source");
      const TRectD region = inputRegion(requested, frame, settings);
      if (!region.isEmpty()) {
        const auto size = rasterSize(region, tile.getRaster()->getPixelSize());
        source.setRaster(tile.getRaster()->create(size.lx, size.ly)); source.m_pos = region.getP00();
        source.getRaster()->setLinear(settings.m_linearColorSpace);
        source.getRaster()->clear(); m_source->compute(source, frame, settings);
        sampler.image = source.getRaster(); sampler.position = source.m_pos;
      }
      TRectD canvas;
      if (m_source->getBBox(frame, canvas, settings) && finiteBox(canvas)) sampler.canvas = rasterBox(canvas);
      if (m_mask.isConnected()) {
        const double inputBytes = sampler.image ? double(sampler.image->getLx()) * sampler.image->getLy() * sampler.image->getPixelSize() : 0;
        demand(inputBytes + double(tile.getRaster()->getLx()) * tile.getRaster()->getLy() * tile.getRaster()->getPixelSize() <= maxWorkingBytes,
               "Source and Mask exceed the 256 MiB working-buffer limit");
        mask.setRaster(tile.getRaster()->create(tile.getRaster()->getLx(), tile.getRaster()->getLy())); mask.m_pos = tile.m_pos;
        mask.getRaster()->setLinear(settings.m_linearColorSpace);
        mask.getRaster()->clear(); m_mask->compute(mask, frame, settings);
      }
    }
    if (canceled(settings.m_isCanceled)) return;
    if (TRaster32P output = tile.getRaster()) render(output, tile.m_pos, frame, settings, sampler, mask);
    else if (TRaster64P output = tile.getRaster()) render(output, tile.m_pos, frame, settings, sampler, mask);
    else if (TRasterFP output = tile.getRaster()) render(output, tile.m_pos, frame, settings, sampler, mask);
    else throw TException("SeExpr: unsupported output pixel type");
  }
};
}  // namespace

class SeExprGeneratorFx final : public SeExprNativeFx<TStandardZeraryFx> {
  FX_PLUGIN_DECLARATION(SeExprGeneratorFx)
public:
  SeExprGeneratorFx() : SeExprNativeFx(true) {}
};
class SeExprColorFx final : public SeExprNativeFx<TStandardRasterFx> {
  FX_PLUGIN_DECLARATION(SeExprColorFx)
public:
  SeExprColorFx() : SeExprNativeFx(false) {}
};
FX_PLUGIN_IDENTIFIER(SeExprGeneratorFx, "seexprGeneratorFx")
FX_PLUGIN_IDENTIFIER(SeExprColorFx, "seexprColorFx")
