#include "toonz/rasterbrushimport.h"
#include "toonz/abrparser.h"
#include "rasterbrushpng.h"
#include "rasterbrusharchive.h"
#include <QXmlStreamReader>
#include <QSet>
#include <cmath>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
#include <stdexcept>

namespace RasterBrush {
namespace {
constexpr int fileLimit = 64 * 1024 * 1024;
struct Invalid : std::runtime_error {
  explicit Invalid(const char *s) : std::runtime_error(s) {}
};
struct Reader {
  const QByteArray &bytes;
  size_t pos = 0, total = 0, limit = 256u * 1024u * 1024u;
  unsigned read() {
    if (pos >= size_t(bytes.size())) throw Invalid("Truncated brush resource");
    return static_cast<unsigned char>(bytes[int(pos++)]);
  }
  unsigned word() {
    unsigned n = 0;
    for (int i = 0; i < 4; ++i) n = (n << 8) | read();
    return n;
  }
  QByteArray take(size_t n) {
    if (n > size_t(bytes.size()) - pos)
      throw Invalid("Truncated brush resource");
    auto b = bytes.mid(int(pos), int(n));
    pos += n;
    return b;
  }
  std::shared_ptr<Image> allocate(unsigned w, unsigned h, bool color) {
    if (!w || !h || w > 8192 || h > 8192 || size_t(w) * h > 2 * 1024 * 1024)
      throw Invalid("Brush image budget exceeded");
    size_t n = size_t(w) * h * sizeof(Pixel);
    if (n > limit - total) throw Invalid("Aggregate brush budget exceeded");
    total += n;
    auto im    = std::make_shared<Image>();
    im->width  = int(w);
    im->height = int(h);
    im->color  = color;
    im->pixels.resize(size_t(w) * h);
    return im;
  }
  std::shared_ptr<Image> gbr(QString &name, double &spacing) {
    size_t start    = pos;
    unsigned header = word(), version = word(), w = word(), h = word(),
             channels = word();
    if (version != 1 && version != 2)
      throw Invalid("Unsupported GBR version (requires 1 or 2)");
    unsigned fixed = version == 1 ? 20 : 28;
    if (header < fixed + 1 || header > fixed + 4096)
      throw Invalid("Invalid GBR name length");
    if (channels != 1 && channels != 4)
      throw Invalid("Unsupported GBR channel format");
    if (version == 2) {
      if (word() != 0x47494d50u) throw Invalid("Invalid GBR signature");
      spacing = word() / 100.0;
    } else
      spacing = .1;
    auto label = take(header - fixed);
    if (label.at(label.size() - 1) != '\0')
      throw Invalid("GBR name is not terminated");
    name = QString::fromUtf8(label.constData(), label.size() - 1);
    if (size_t(w) * h * channels > size_t(bytes.size()) - pos)
      throw Invalid("Truncated GBR pixels");
    auto im = allocate(w, h, channels == 4);
    for (auto &p : im->pixels) {
      if (channels == 1)
        p = {0, 0, 0, read() / 255.0};
      else {
        double r = read() / 255.0, g = read() / 255.0, b = read() / 255.0,
               a = read() / 255.0;
        p        = {r * a, g * a, b * a, a};
      }
    }
    if (pos <= start) throw Invalid("Invalid GBR record");
    return im;
  }
  QByteArray line() {
    size_t start = pos;
    while (pos < size_t(bytes.size()) && bytes[int(pos)] != '\n') {
      if (pos - start >= 4096) throw Invalid("GIH header line budget exceeded");
      ++pos;
    }
    if (pos == size_t(bytes.size())) throw Invalid("Truncated GIH header");
    auto result = bytes.mid(int(start), int(pos - start)).trimmed();
    ++pos;
    return result;
  }
};
void geometry(Preset &p, const Image &im, double spacing) {
  // Bitmap sampling retains image aspect inside its longest-axis diameter.
  p.size.value   = std::max(im.width, im.height);
  p.aspect.value = 1;
  p.angle.value  = 0;
  p.spacing      = spacing;
}
Record converted(const QByteArray &bytes, const QString &format) {
  Record r;
  r.provenance = {
      {"adapter", "groundzero-resource-1"},
      {"format", format},
      {"sourceSha256",
       QString::fromLatin1(
           QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
               .toHex())},
      {"conversion", "native brush; source engine behavior is not reproduced"},
      {"sourceSemanticallySupported", false},
      {"sourceReferenceVerified", false}};
  r.decoded = true;
  r.diagnostics << "Converted native settings; source engine semantics and "
                   "appearance are not verified.";
  return r;
}
QMap<QString, QString> kppParameters(const QByteArray &xml, QString &name,
                                     QString &engine) {
  if (xml.size() > 1024 * 1024) throw Invalid("KPP XML budget exceeded");
  QXmlStreamReader reader(xml);
  QMap<QString, QString> fields;
  bool root = false;
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isDTD() || reader.isEntityReference())
      throw Invalid("KPP entities are unsupported");
    if (!reader.isStartElement()) continue;
    if (!root) {
      if (reader.name() != QStringLiteral("Preset"))
        throw Invalid("Invalid KPP preset root");
      root   = true;
      name   = reader.attributes().value("name").toString();
      engine = reader.attributes().value("paintopid").toString();
    } else {
      if (reader.name() != QStringLiteral("param"))
        throw Invalid("Unknown KPP structure");
      QString key = reader.attributes().value("name").toString();
      if (key.isEmpty() || key.size() > 256 || fields.contains(key) ||
          fields.size() >= 1024)
        throw Invalid("Invalid KPP parameter table");
      fields[key] =
          reader.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement);
    }
  }
  if (reader.hasError() || !root || name.size() > 4096)
    throw Invalid("Invalid KPP XML");
  if (fields.contains("paintop") && fields["paintop"] != engine)
    throw Invalid("Conflicting KPP engine identifiers");
  return fields;
}
bool convertCurve(const QMap<QString, QString> &fields, const QString &prefix,
                  Dynamic &target, QSet<QString> &mapped,
                  QStringList &diagnostics, bool enabledByDefault = false) {
  bool enabled = fields.value("Pressure" + prefix,
                              enabledByDefault ? "true" : "false") == "true";
  if (!enabled) return false;
  if (fields.value(prefix + "UseCurve") != "true") return false;
  if (fields.value(prefix + "curveMode", "0") != "0") return false;
  QXmlStreamReader xml(fields.value(prefix + "Sensor"));
  QString sensor, curve;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isEntityReference()) return false;
    if (xml.isDTD() && xml.text().toString().trimmed() != "<!DOCTYPE params>")
      return false;
    if (!xml.isStartElement()) continue;
    if (xml.name() == QStringLiteral("params")) {
      if (!sensor.isEmpty()) return false;
      sensor = xml.attributes().value("id").toString();
      // Non-default source-specific sensor options need their own adapter.
      for (auto a : xml.attributes())
        if (a.name() != QStringLiteral("id")) return false;
    } else if (xml.name() == QStringLiteral("curve"))
      curve = xml.readElementText();
    else
      return false;
  }
  if (xml.hasError()) return false;
  const QMap<QString, Sensor> names = {{"pressure", Sensor::Pressure},
                                       {"xtilt", Sensor::TiltX},
                                       {"ytilt", Sensor::TiltY},
                                       {"ascension", Sensor::TiltDirection},
                                       {"declination", Sensor::TiltElevation},
                                       {"rotation", Sensor::Rotation},
                                       {"drawingangle", Sensor::Direction}};
  if (!names.contains(sensor)) return false;
  if (fields.value(prefix + "UseSameCurve") == "true")
    curve = fields.value(prefix + "commonCurve", curve);
  auto points = curve.split(';', Qt::SkipEmptyParts);
  if (points.size() < 2 || points.size() > 1024) return false;
  Mapping mapping;
  mapping.sensor = names[sensor];
  mapping.curve.clear();
  double previous = -1;
  for (auto point : points) {
    auto pair = point.split(',');
    if (pair.size() != 2) return false;
    bool okx = false, oky = false;
    double x = pair[0].toDouble(&okx), y = pair[1].toDouble(&oky);
    if (!okx || !oky || !std::isfinite(x) || !std::isfinite(y) ||
        x <= previous || x < 0 || x > 1 || y < 0 || y > 1)
      return false;
    mapping.curve.push_back({x, y});
    previous = x;
  }
  target.minimum = 0;
  target.mappings.push_back(mapping);
  for (auto suffix :
       {"Sensor", "UseCurve", "UseSameCurve", "commonCurve", "curveMode"})
    mapped.insert(prefix + suffix);
  mapped.insert("Pressure" + prefix);
  diagnostics << prefix +
                     ": sensor curve converted to native linear interpolation "
                     "(source spline behavior is unverified).";
  return true;
}
void convertTipToMask(Tip &tip, bool legacyTransparentAlpha = false) {
  for (auto &image : tip.images) {
    if (!image->color) continue;
    // Resources here are freshly decoded and not yet published. Transform in
    // place so explicit mask selection cannot double the decoded image budget.
    if (image.use_count() != 1)
      throw Invalid("Unexpected shared image during tip conversion");
    auto writable    = std::const_pointer_cast<Image>(image);
    bool transparent = false;
    if (legacyTransparentAlpha)
      for (const auto &pixel : writable->pixels)
        if (pixel.a < 1) {
          transparent = true;
          break;
        }
    for (auto &pixel : writable->pixels) {
      double alpha =
          transparent
              ? pixel.a
              : std::clamp(pixel.a - (.2126 * pixel.r + .7152 * pixel.g +
                                      .0722 * pixel.b),
                           0.0, 1.0);
      pixel = {0, 0, 0, alpha};
    }
    writable->color = false;
  }
}
Tip resolveTip(const QMap<QString, QByteArray> *resources,
               const QString &filename, const QString &md5,
               QStringList &diagnostics, size_t maximumDecodedBytes,
               int application) {
  if (!resources)
    throw Invalid(
        "KPP tip resource is missing; import a bundle containing its "
        "resources");
  QStringList matches;
  for (auto i = resources->begin(); i != resources->end(); ++i) {
    if (!md5.isEmpty()) {
      if (QString::fromLatin1(
              QCryptographicHash::hash(i.value(), QCryptographicHash::Md5)
                  .toHex()) == md5.toLower())
        matches << i.key();
    } else if (i.key() == filename || QFileInfo(i.key()).fileName() == filename)
      matches << i.key();
  }
  if (matches.size() != 1)
    throw Invalid("Missing or ambiguous KPP tip dependency");
  const QString key = matches[0], suffix = QFileInfo(key).suffix().toLower();
  if (suffix == "png") {
    QString error;
    auto image =
        decodeImage(resources->value(key), true, error, maximumDecodedBytes);
    if (!image) throw std::runtime_error(error.toStdString());
    Tip tip;
    tip.shape = Shape::Bitmap;
    tip.images.push_back(std::move(image));
    if (application != 1) convertTipToMask(tip, application < 0);
    diagnostics
        << (application == 1
                ? "Embedded PNG color image retained for native stamping."
                : "PNG converted to a native coverage mask; source mask color "
                  "math remains unverified.");
    return tip;
  }
  if (suffix != "gbr" && suffix != "gih")
    throw Invalid("Unsupported KPP tip resource format");
  QString error;
  auto imported = importBrush(resources->value(key), suffix, error, nullptr,
                              maximumDecodedBytes);
  if (!error.isEmpty() || imported.records.size() != 1)
    throw Invalid("Cannot decode KPP tip dependency");
  diagnostics.append(imported.diagnostics);
  auto tip = std::move(imported.records[0].preset.tip);
  if (application == 0) convertTipToMask(tip);
  return tip;
}
bool eligibleFixedSource(const QMap<QString, QString> &fields,
                         const QString &engine, const Preset &preset) {
  if (engine != "paintbrush" && engine != "colorsmudge") return false;
  // Explicit eligibility, not inference from a partially decoded preset.
  // Missing defaults and unknown settings stay native conversions.
  QMap<QString, QString> required = {
      {"paintop", engine},
      {"CompositeOp", "normal"},
      {"EraserMode", "false"},
      {"OpacityValue", "1"},
      {"FlowValue", "1"},
      {"OpacityUseCurve", "false"},
      {"FlowUseCurve", "false"},
      {"PressureSize", "false"},
      {"PressureRotation", "false"},
      {"PressureRatio", "false"},
      {"PressureScatter", "false"},
      {"PressureSpacing", "false"},
      {"Texture/Pattern/Enabled", "false"},
      {"MergedPaint", "false"},
      {"PaintingMode", "1"},
      {"PaintOpSettings/isAirbrushing", "false"},
      {"PaintOpSettings/ignoreSpacing", "false"},
      {"PaintOpSettings/updateSpacingBetweenDabs", "false"},
      {"KisPrecisionOption/AutoPrecisionEnabled", "false"},
      {"KisPrecisionOption/precisionLevel", "5"}};
  if (engine == "paintbrush") {
    for (const QString &key : {"OpacityValue", "FlowValue"}) {
      if (!fields.contains(key)) return false;
      required[key] = fields[key];  // Finite 0..1 values checked by validate().
    }
    if (fields.value("PaintingMode") != "0" &&
        fields.value("PaintingMode") != "1")
      return false;
    required["PaintingMode"] = fields["PaintingMode"];
  }
  if (engine == "paintbrush" && fields.value("PressureSize") == "true") {
    if (preset.size.mappings.size() != 1) return false;
    required["PressureSize"]     = "true";
    required["SizeUseCurve"]     = "true";
    required["SizeUseSameCurve"] = "true";
    required["SizecommonCurve"]  = "0,0;1,1;";
    required["SizeSensor"]       = fields.value("SizeSensor");
    if (fields.contains("SizecurveMode")) required["SizecurveMode"] = "0";
  }
  if (engine == "colorsmudge") {
    for (const QString &key : {"PressureColorRate", "PressureSmudgeRate",
                               "PressureSmudgeRadius", "SmudgeRateSmearAlpha"})
      required[key] = "true";
    for (const QString &key :
         {"ColorRateUseCurve", "SmudgeRateUseCurve", "SmudgeRadiusUseCurve"})
      required[key] = "false";
    required["SmudgeRadiusValue"] = "0";
    for (const QString &key : {"SmudgeRateValue", "ColorRateValue",
                               "SmudgeRateMode", "SmudgeRateUseNewEngine"})
      if (!fields.contains(key)) return false;
  }
  for (auto i = required.begin(); i != required.end(); ++i)
    if (!fields.contains(i.key()) || fields.value(i.key()) != i.value())
      return false;
  // These serialized properties belong exclusively to Color Smudge. They
  // are inert in Pixel; no wildcard exemptions for other unknown settings.
  const QSet<QString> inactive = {
      "PressureColorRate", "PressureSmudgeRate",     "PressureSmudgeRadius",
      "ColorRateUseCurve", "SmudgeRateUseCurve",     "SmudgeRadiusUseCurve",
      "SmudgeRateMode",    "SmudgeRateUseNewEngine", "SmudgeRateSmearAlpha",
      "SmudgeRateValue",   "ColorRateValue",         "SmudgeRadiusValue"};
  for (auto i = fields.begin(); i != fields.end(); ++i)
    if (!required.contains(i.key()) && !inactive.contains(i.key()) &&
        i.key() != "brush_definition")
      return false;
  QXmlStreamReader xml(fields.value("brush_definition"));
  int depth = 0, brushes = 0, masks = 0;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isDTD() || xml.isEntityReference()) return false;
    if (xml.isEndElement()) {
      --depth;
      continue;
    }
    if (!xml.isStartElement()) continue;
    ++depth;
    const bool brush = xml.name() == QStringLiteral("Brush") && depth == 1;
    const bool mask =
        xml.name() == QStringLiteral("MaskGenerator") && depth == 2;
    if (!brush && !mask) return false;
    if (brush)
      ++brushes;
    else
      ++masks;
    const QMap<QString, QString> exact =
        brush
            ? QMap<QString, QString>{{"type", "auto_brush"},
                                     {"BrushVersion", "2"},
                                     {"angle", "0"},
                                     {"randomness", "0"},
                                     {"density", "1"},
                                     {"useAutoSpacing", "0"}}
            : QMap<QString, QString>{{"id", "default"}, {"type", "circle"},
                                     {"ratio", "1"},    {"hfade", "1"},
                                     {"vfade", "1"},    {"antialiasEdges", "0"},
                                     {"spikes", "2"}};
    const auto attributes = xml.attributes();
    for (auto i = exact.begin(); i != exact.end(); ++i)
      if (!attributes.hasAttribute(i.key()) ||
          attributes.value(i.key()) != i.value())
        return false;
    const QString variable = brush ? "spacing" : "diameter";
    if (!attributes.hasAttribute(variable)) return false;
    for (const auto &attribute : attributes)
      if (!exact.contains(attribute.name().toString()) &&
          attribute.name() != variable)
        return false;
  }
  return !xml.hasError() && brushes == 1 && masks == 1 &&
         validate(preset).empty();
}
Record convertKpp(const QByteArray &bytes, QStringList &diagnostics,
                  const QMap<QString, QByteArray> *resources = nullptr,
                  size_t maximumDecodedBytes = 256u * 1024u * 1024u) {
  const int diagnosticStart = diagnostics.size();
  QString error;
  auto png = boundedPng(bytes, error);
  if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
  if (png.text.value("version") != "2.2" && png.text.value("version") != "5.0")
    throw Invalid("Unsupported KPP container version");
  QString name, engine;
  auto fields = kppParameters(png.text.value("preset"), name, engine);
  if (engine != "paintbrush" && engine != "colorsmudge")
    throw Invalid("Only Pixel and Color Smudge KPP conversion is available");
  Record r                         = converted(bytes, "kpp");
  r.preset.name                    = name.toStdString();
  r.provenance["containerVersion"] = QString::fromLatin1(png.text["version"]);
  r.provenance["sourceEngine"]     = engine;
  QXmlStreamReader brush(fields.value("brush_definition"));
  bool found = false, maskFound = false;
  auto numeric = [](const QString &v) {
    bool ok  = false;
    double n = v.toDouble(&ok);
    if (!ok || !std::isfinite(n)) throw Invalid("Invalid KPP number");
    return n;
  };
  while (!brush.atEnd()) {
    brush.readNext();
    if (brush.isDTD() || brush.isEntityReference())
      throw Invalid("KPP brush entities are unsupported");
    if (!brush.isStartElement()) continue;
    auto a = brush.attributes();
    if (brush.name() == QStringLiteral("Brush")) {
      if (found) throw Invalid("Duplicate KPP brush definition");
      QString type = a.value("type").toString();
      if (type != "auto_brush") {
        if (type != "png_brush" && type != "gbr_brush" && type != "gih_brush")
          throw Invalid("Unsupported KPP tip type");
        // Public enumBrushApplication values and serialized preset metadata:
        // 0 is alpha mask, 1 color image; lightness/gradient modes are outside
        // this adapter. The modern field overrides the older ColorAsMask flag.
        int application = -1;
        if (a.hasAttribute("brushApplication")) {
          auto mode = a.value("brushApplication").toString();
          if (mode != "0" && mode != "1")
            throw Invalid(
                "Unsupported KPP lightness, gradient or unknown tip "
                "application");
          application = mode == "1" ? 1 : 0;
        } else if (a.hasAttribute("ColorAsMask")) {
          auto mode = a.value("ColorAsMask").toString();
          if (mode != "0" && mode != "1")
            throw Invalid("Invalid KPP color-as-mask flag");
          application = mode == "0" ? 1 : 0;
        }
        r.provenance["sourceTipApplication"] =
            application < 0    ? "legacy-unspecified"
            : application == 1 ? "color-image"
                               : "alpha-mask";
        r.preset.tip = resolveTip(resources, a.value("filename").toString(),
                                  a.value("md5sum").toString(), diagnostics,
                                  maximumDecodedBytes, application);
        r.preset.size.value = std::max(r.preset.tip.images[0]->width,
                                       r.preset.tip.images[0]->height) *
                              numeric(a.value("scale").toString());
        maskFound = true;
      }
      found            = true;
      r.preset.spacing = numeric(a.value("spacing").toString());
      r.preset.angle.value =
          numeric(a.value("angle").toString()) * 180 / 3.14159265358979323846;
      if (a.value("useAutoSpacing") == QStringLiteral("1"))
        diagnostics << "Automatic source spacing is replaced by the stored "
                       "manual spacing.";
      if (a.value("randomness").toDouble() != 0 ||
          a.value("density").toDouble() != 1)
        diagnostics << "Source tip randomness/density is not reproduced.";
    } else if (brush.name() == QStringLiteral("MaskGenerator")) {
      if (maskFound) throw Invalid("Duplicate KPP mask generator");
      maskFound         = true;
      QString shape     = a.value("type").toString(),
              generator = a.value("id").toString();
      if (shape != "circle" && shape != "rect")
        throw Invalid("Unsupported KPP generated-tip shape");
      if (generator != "default" && generator != "gauss")
        throw Invalid("Unsupported KPP mask generator");
      r.preset.tip.shape    = generator == "gauss" ? Shape::Gaussian
                              : shape == "rect"    ? Shape::Square
                                                   : Shape::Round;
      r.preset.size.value   = numeric(a.value("diameter").toString());
      r.preset.aspect.value = numeric(a.value("ratio").toString());
      r.preset.tip.hardness = std::min(numeric(a.value("hfade").toString()),
                                       numeric(a.value("vfade").toString()));
      if (a.hasAttribute("antialiasEdges")) {
        auto value = a.value("antialiasEdges").toString();
        if (value != "0" && value != "1")
          throw Invalid("Invalid KPP antialias flag");
        r.preset.tip.antialiasEdges = value == "1";
        diagnostics << "Generated-tip antialiasing uses native 8x8 area "
                       "sampling; source edge coverage remains unverified.";
      }
      if (a.value("spikes").toString() != "2")
        throw Invalid(
            "KPP star-shaped mask requires a separate geometry profile");
    } else
      throw Invalid("Unknown KPP brush definition element");
  }
  if (brush.hasError() || !found || !maskFound)
    throw Invalid("Incomplete KPP generated brush definition");
  QSet<QString> mapped = {"paintop",   "brush_definition", "OpacityValue",
                          "FlowValue", "CompositeOp",      "EraserMode"};
  if (fields.contains("OpacityValue"))
    r.preset.opacity.value = numeric(fields["OpacityValue"]);
  if (fields.contains("FlowValue"))
    r.preset.flow.value = numeric(fields["FlowValue"]);
  auto blend = fields.value("CompositeOp", "normal");
  if (blend == "normal")
    r.preset.blend = Blend::Normal;
  else if (blend == "multiply")
    r.preset.blend = Blend::Multiply;
  else if (blend == "screen")
    r.preset.blend = Blend::Screen;
  else if (blend == "overlay")
    r.preset.blend = Blend::Overlay;
  else
    throw Invalid("Unsupported KPP color blending mode");
  if (fields.value("EraserMode") == "true") r.preset.blend = Blend::Erase;
  if (engine == "colorsmudge") {
    if (!fields.contains("SmudgeRateUseNewEngine") ||
        !fields.contains("SmudgeRateMode"))
      throw Invalid("KPP smudge model is unidentified");
    QString mode  = fields["SmudgeRateMode"],
            model = fields["SmudgeRateUseNewEngine"];
    if ((mode != "0" && mode != "1") || (model != "true" && model != "false"))
      throw Invalid("Invalid KPP smudge model");
    r.provenance["sourceSmudgeModel"] =
        (model == "true" ? "new-" : "legacy-") +
        QString(mode == "0" ? "smear" : "dulling");
    r.preset.wet   = mode == "0" ? Wet::Smear : Wet::Dulling;
    r.preset.blend = Blend::Normal;
    if (fields.contains("SmudgeRateValue"))
      r.preset.pickup = numeric(fields["SmudgeRateValue"]);
    if (fields.contains("ColorRateValue"))
      r.preset.colorRate = numeric(fields["ColorRateValue"]);
    diagnostics << "Krita wet behavior is replaced by native wet painting. "
                   "Old/new smudge equations, smear-alpha and sensor "
                   "relationships are NOT reproduced.";
  }
  convertCurve(fields, "SmudgeRate", r.preset.pickupControl, mapped,
               diagnostics);
  convertCurve(fields, "ColorRate", r.preset.colorRateControl, mapped,
               diagnostics);
  convertCurve(fields, "SmudgeRadius", r.preset.radiusControl, mapped,
               diagnostics);
  convertCurve(fields, "Size", r.preset.size, mapped, diagnostics);
  convertCurve(fields, "Opacity", r.preset.opacity, mapped, diagnostics, true);
  convertCurve(fields, "Flow", r.preset.flow, mapped, diagnostics, true);
  convertCurve(fields, "Ratio", r.preset.aspect, mapped, diagnostics);
  QStringList unmapped;
  for (auto i = fields.begin(); i != fields.end(); ++i)
    if (!mapped.contains(i.key())) unmapped << i.key();
  diagnostics
      << "Generated tip, geometry and static opacity/flow are converted to "
         "native definitions; source edge and spacing mathematics may differ.";
  if (!unmapped.isEmpty())
    diagnostics << "Source settings not reproduced: " + unmapped.join(", ");
  Preset candidate = r.preset;
  candidate.profile =
      engine == "paintbrush"
          ? "krita-5.3.4.1-hard-pixel-candidate-1"
          : "krita-5.3.4.1-" +
                r.provenance["sourceSmudgeModel"].toString().toStdString() +
                "-candidate-1";
  if (engine == "colorsmudge")
    candidate.smudgeRadius = 0;
  else
    candidate.wash = fields.value("PaintingMode") == "1";
  if (eligibleFixedSource(fields, engine, candidate)) {
    r.preset = std::move(candidate);
    while (diagnostics.size() > diagnosticStart) diagnostics.removeLast();
    r.diagnostics.clear();
    diagnostics
        << "Strict fixed-tip source candidate selected. Component fixtures "
           "are tested separately; arbitrary source presets and complete "
           "source engine parity are not verified.";
    r.provenance["conversion"] = "strict source-profile candidate";
    r.provenance["candidateEligibility"] =
        "k5341-fixed-tip-explicit-settings-1";
  }
  r.provenance["conversionReport"] = diagnostics.join("\n");
  r.diagnostics.append(diagnostics);
  return r;
}
QStringList bundlePresets(const QMap<QString, QByteArray> &members,
                          QMap<QString, QByteArray> &resources) {
  if (members.value("mimetype").trimmed() !=
      "application/x-krita-resourcebundle")
    throw Invalid("Invalid Krita bundle MIME type");
  auto xml = members.value("META-INF/manifest.xml");
  if (xml.isEmpty() || xml.size() > 1024 * 1024)
    throw Invalid("Missing or oversized bundle manifest");
  QXmlStreamReader reader(xml);
  QStringList presets;
  QSet<QString> paths;
  bool root        = false;
  const QString ns = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0";
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isDTD() || reader.isEntityReference())
      throw Invalid("Bundle manifest entities are unsupported");
    if (!reader.isStartElement()) continue;
    if (!root) {
      if (reader.name() != QStringLiteral("manifest") ||
          reader.namespaceUri() != ns ||
          reader.attributes().value(ns, "version") != QStringLiteral("1.2"))
        throw Invalid("Unsupported bundle manifest version");
      root = true;
      continue;
    }
    if (reader.name() == QStringLiteral("file-entry")) {
      auto a       = reader.attributes();
      QString path = a.value(ns, "full-path").toString(),
              type = a.value(ns, "media-type").toString();
      if (paths.contains(path)) throw Invalid("Duplicate bundle manifest path");
      paths.insert(path);
      if (path == "/") {
        if (type != "application/x-krita-resourcebundle")
          throw Invalid("Invalid bundle root declaration");
        continue;
      }
      if (!members.contains(path))
        throw Invalid("Bundle manifest references a missing member");
      QString md5 = a.value(ns, "md5sum").toString();
      if (!md5.isEmpty() &&
          QString::fromLatin1(
              QCryptographicHash::hash(members[path], QCryptographicHash::Md5)
                  .toHex()) != md5.toLower())
        throw Invalid("Bundle manifest checksum mismatch");
      if (type == "brushes" || type == "patterns")
        resources[path] = members[path];
      if (type == "paintoppresets") {
        if (!path.endsWith(".kpp", Qt::CaseInsensitive))
          throw Invalid("Bundle preset type mismatch");
        presets << path;
      }
    } else if (reader.name() != QStringLiteral("tags") &&
               reader.name() != QStringLiteral("tag"))
      throw Invalid("Unknown bundle manifest element");
  }
  if (reader.hasError() || !root || presets.isEmpty() || presets.size() > 256)
    throw Invalid("Invalid or empty bundle preset manifest");
  return presets;
}
void check(Record &r) {
  auto error = validate(r.preset);
  if (!error.empty()) throw std::runtime_error(error);
  r.semanticallySupported = true;
}
}  // namespace
ImportResult importBrush(const QByteArray &bytes, const QString &format,
                         QString &error, const std::atomic_bool *cancelled,
                         size_t maximumDecodedBytes) {
  maximumDecodedBytes =
      std::min(maximumDecodedBytes, size_t(256u * 1024u * 1024u));
  ImportResult result;
  error.clear();
  auto checkCancelled = [&]() {
    if (cancelled && cancelled->load(std::memory_order_relaxed))
      throw Invalid("Brush import cancelled");
  };
  try {
    checkCancelled();
    if (bytes.isEmpty() || bytes.size() > fileLimit)
      throw Invalid("Brush input exceeds 64 MiB");
    const QString kind = format.toLower();
    if (kind == "gzbrush") {
      auto r = decode(bytes, error, maximumDecodedBytes);
      checkCancelled();
      if (!error.isEmpty()) return {};
      result.records.push_back(r);
      result.complete = true;
      return result;
    }
    if (kind == "bundle") {
      auto members = brushArchive(bytes, error);
      if (!error.isEmpty()) return {};
      QMap<QString, QByteArray> resources;
      auto presets   = bundlePresets(members, resources);
      size_t decoded = 0;
      for (const auto &path : presets) {
        checkCancelled();
        QStringList diagnostics;
        auto r = convertKpp(members[path], diagnostics, &resources,
                            maximumDecodedBytes - decoded);
        check(r);
        for (const auto &image : r.preset.tip.images) {
          size_t n = image->pixels.size() * sizeof(Pixel);
          if (n > maximumDecodedBytes - decoded)
            throw Invalid("Bundle decoded image budget exceeded");
          decoded += n;
        }
        r.provenance["bundleSha256"] = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                .toHex());
        r.provenance["bundleMember"] = path;
        result.records.push_back(r);
        result.diagnostics << path + ": " + diagnostics.join("\n");
      }
    } else if (kind == "gbr" || kind == "gih") {
      Reader reader{bytes};
      reader.limit   = maximumDecodedBytes;
      auto r         = converted(bytes, kind);
      int count      = 1;
      double spacing = .1;
      QMap<QString, QString> fields;
      if (kind == "gih") {
        r.preset.name = QString::fromUtf8(reader.line()).toStdString();
        auto tokens =
            QString::fromLatin1(reader.line())
                .split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        bool ok = false;
        count   = tokens.value(0).toInt(&ok);
        if (!ok || count < 1 || count > 256)
          throw Invalid("Invalid GIH frame count");
        const QStringList allowed = {
            "ncells",     "step",  "dim",  "cols",      "rows",     "cellwidth",
            "cellheight", "rank0", "sel0", "selection", "placement"};
        for (int i = 1; i < tokens.size(); ++i) {
          int colon = tokens[i].indexOf(':');
          if (colon < 1) throw Invalid("Invalid GIH parameter");
          auto k = tokens[i].left(colon), v = tokens[i].mid(colon + 1);
          if (!allowed.contains(k) || fields.contains(k))
            throw Invalid("Unsupported or duplicate GIH parameter");
          fields[k] = v;
        }
        if (fields.value("dim") != "1" ||
            fields.value("rank0").toInt() != count ||
            fields.value("ncells").toInt() != count)
          throw Invalid("GIH requires a single complete selection dimension");
        auto mode = fields.value("sel0", fields.value("selection"));
        if (mode != "random" && mode != "incremental")
          throw Invalid("Unsupported GIH selection mode");
        if (fields.contains("sel0") && fields.contains("selection") &&
            fields["sel0"] != fields["selection"])
          throw Invalid("Conflicting GIH selectors");
        if (fields.contains("placement") && fields["placement"] != "constant")
          throw Invalid("Unsupported GIH placement");
        r.preset.tip.randomSequence = mode == "random";
        spacing = fields.value("step").toDouble(&ok) / 100.0;
        if (!ok) throw Invalid("Invalid GIH spacing");
      }
      r.preset.tip.shape = Shape::Bitmap;
      for (int i = 0; i < count; ++i) {
        checkCancelled();
        QString name;
        double frameSpacing;
        auto im = reader.gbr(name, frameSpacing);
        if (i == 0) {
          if (kind == "gbr") {
            r.preset.name = name.toStdString();
            spacing       = frameSpacing;
          }
          geometry(r.preset, *im, spacing);
        } else if (im->width != r.preset.tip.images[0]->width ||
                   im->height != r.preset.tip.images[0]->height)
          throw Invalid(
              "Variable-size GIH frames need a source-specific geometry "
              "profile");
        r.preset.tip.images.push_back(im);
      }
      if (reader.pos != size_t(bytes.size()))
        throw Invalid("Unexpected bytes after brush records");
      check(r);
      result.records.push_back(r);
    } else if (kind == "kpp") {
      auto r =
          convertKpp(bytes, result.diagnostics, nullptr, maximumDecodedBytes);
      check(r);
      result.records.push_back(r);
    } else if (kind == "abr") {
      AbrParser::Result parsed;
      std::string why;
      AbrParser::Limits limits;
      limits.maxFileBytes    = fileLimit;
      limits.maxDecodedBytes = std::min(
          size_t(8 * 1024 * 1024), maximumDecodedBytes / (sizeof(Pixel) + 1));
      limits.maxRecords = 256;
      if (!AbrParser::parseAbr(
              reinterpret_cast<const unsigned char *>(bytes.constData()),
              bytes.size(), parsed, why, limits))
        throw std::runtime_error(why);
      for (const auto &w : parsed.warnings)
        result.diagnostics << QString::fromStdString(w);
      if (parsed.computedBrushes != int(parsed.computed.size()) ||
          parsed.skippedSamples || !parsed.warnings.empty())
        result.diagnostics
            << "The ABR contains behavior or records that are not represented "
               "by these native tip conversions.";
      Reader allocator{bytes};
      size_t grayBytes = 0;
      for (const auto &tip : parsed.tips) grayBytes += tip.pixels.size();
      // The parser's byte masks remain alive while native float tips are made.
      // Reserve both representations, not just the final resource graph.
      allocator.limit = maximumDecodedBytes - grayBytes;
      for (const auto &shape : parsed.computed) {
        checkCancelled();
        auto r                           = converted(bytes, kind);
        r.preset.name                    = shape.name;
        r.preset.tip.shape               = Shape::Round;
        r.preset.tip.hardness            = shape.hardness / 100.0;
        r.preset.tip.antialiasEdges      = true;
        r.preset.size.value              = shape.diameter;
        r.preset.aspect.value            = shape.roundness / 100.0;
        r.preset.angle.value             = shape.angle;
        r.preset.spacing                 = shape.spacing / 100.0;
        r.provenance["containerVersion"] = parsed.version;
        r.provenance["sourceTipType"]    = "computed-ellipse";
        r.diagnostics.append(result.diagnostics);
        r.diagnostics << "Computed ellipse converted to native geometry. "
                         "Source hardness, angle and spacing behavior is "
                         "not reference-verified; zero spacing is unsupported.";
        check(r);
        result.records.push_back(r);
      }
      for (const auto &tip : parsed.tips) {
        checkCancelled();
        auto r        = converted(bytes, kind);
        r.preset.name = tip.name;
        r.diagnostics.append(result.diagnostics);
        r.provenance["containerVersion"]       = parsed.version;
        r.provenance["containerSubversion"]    = parsed.subVersion;
        r.provenance["decodedDescriptorCount"] = int(parsed.descriptors.size());
        if (!tip.sampleId.empty())
          r.provenance["sourceSampleId"] = QString::fromLatin1(
              tip.sampleId.data(), int(tip.sampleId.size()));
        auto im = allocator.allocate(tip.width, tip.height, false);
        for (size_t i = 0; i < tip.pixels.size(); ++i)
          im->pixels[i] = {0, 0, 0, tip.pixels[i] / 255.0};
        r.preset.tip.shape = Shape::Bitmap;
        r.preset.tip.images.push_back(im);
        geometry(r.preset, *im, tip.spacing < 0 ? .1 : tip.spacing / 100.0);
        check(r);
        result.records.push_back(r);
      }
      if (result.records.empty())
        throw Invalid("No decodable sampled or computed tips in this ABR");
    } else
      throw Invalid("Unsupported brush format");
    checkCancelled();
    result.diagnostics << "Converted to native painting. Source brush dynamics "
                          "and appearance are not reference-verified. Embedded "
                          "assets retain their original license.";
    result.complete = true;
    return result;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}
ImportResult importBrushFile(const QString &path, QString &error,
                             const std::atomic_bool *cancelled) {
  if (cancelled && cancelled->load(std::memory_order_relaxed)) {
    error = "Brush import cancelled";
    return {};
  }
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    error = f.errorString();
    return {};
  }
  if (f.size() > fileLimit) {
    error = "Brush input exceeds 64 MiB";
    return {};
  }
  auto bytes = f.read(fileLimit + 1);
  if (!f.atEnd() || f.error() != QFile::NoError) {
    error = "Cannot read brush file";
    return {};
  }
  return importBrush(bytes, QFileInfo(path).suffix(), error, cancelled);
}
}  // namespace RasterBrush
