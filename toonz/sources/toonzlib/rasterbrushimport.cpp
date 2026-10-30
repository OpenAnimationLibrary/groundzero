#include "toonz/rasterbrushimport.h"
#include "toonz/abrparser.h"
#include "rasterbrushpng.h"
#include "rasterbrusharchive.h"
#include "rasterbrushbundlemetadata.h"
#include <QXmlStreamReader>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
#include <map>
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
// Observed in independently inspected author-exported ABR 6.2 records.
// Resource association and static native conversion are not source parity.
const AbrParser::DescriptorValue *abrField(
    const AbrParser::DescriptorValue &object, const char *key) {
  for (const auto &value : object.children)
    if (value.key == key) return &value;
  return nullptr;
}
const AbrParser::DescriptorValue &abrRequired(
    const AbrParser::DescriptorValue &object, const char *key,
    const char *type) {
  auto value = abrField(object, key);
  if (!value || value->type != type)
    throw Invalid("Invalid ABR 6.2 preset field or type");
  return *value;
}
double abrUnit(const AbrParser::DescriptorValue &object, const char *key,
               const char *unit) {
  const auto &value = abrRequired(object, key, "UntF");
  if (value.unit != unit) throw Invalid("Invalid ABR static setting unit");
  return value.number;
}
// Preserve typed source settings, including opaque values; never resolve paths.
// Each node is [key,type,name,classId,unit,text,number,integer,bool,children].
QJsonArray abrMetadata(const AbrParser::DescriptorValue &value,
                       size_t &budget) {
  const size_t bytes = value.key.size() + value.type.size() +
                       value.name.size() + value.classId.size() +
                       value.unit.size() + value.text.size();
  if (budget < 128 || bytes > budget - 128)
    throw Invalid("ABR preset metadata exceeds portable budget");
  budget -= bytes + 128;
  QJsonArray children;
  for (const auto &child : value.children)
    children.append(abrMetadata(child, budget));
  auto text = [](const std::string &v) { return QString::fromStdString(v); };
  // Opaque bytes use hex; int64 uses decimal text to avoid JSON double loss.
  const QString content =
      value.type == "tdta" || value.type == "alis"
          ? QString::fromLatin1(
                QByteArray(value.text.data(), int(value.text.size())).toHex())
          : text(value.text);
  return {text(value.key),  text(value.type),
          text(value.name), text(value.classId),
          text(value.unit), content,
          value.number,     QString::number(qlonglong(value.integer)),
          value.boolean,    children};
}
void abrStaticPreset(Record &record, const AbrParser::DescriptorValue &preset,
                     int ordinal) {
  const auto &brush          = abrRequired(preset, "Brsh", "Objc");
  record.preset.name         = abrRequired(preset, "Nm  ", "TEXT").text;
  record.preset.size.value   = abrUnit(brush, "Dmtr", "#Pxl");
  record.preset.angle.value  = abrUnit(brush, "Angl", "#Ang");
  record.preset.aspect.value = abrUnit(brush, "Rndn", "#Prc") / 100.0;
  record.preset.spacing      = abrUnit(brush, "Spcn", "#Prc") / 100.0;
  if (!abrRequired(brush, "Intr", "bool").boolean)
    throw Invalid("ABR continuous/non-spaced tip conversion is unsupported");
  record.preset.flipX          = abrRequired(brush, "flipX", "bool").boolean;
  record.preset.flipY          = abrRequired(brush, "flipY", "bool").boolean;
  record.provenance["adapter"] = "groundzero-abr6.2-static-1";
  record.provenance["conversionKind"] = "static-native";
  record.diagnostics.removeAll(
      "Tip extraction only: this record does not import the source brush "
      "preset.");
  record.provenance["sourcePresetOrdinal"]  = ordinal;
  record.provenance["sourceSampleBinding"]  = "abr6.2-brushPreset-sampledData";
  record.provenance["sourceFormatEvidence"] = "author-exported-6.2-corpus-1";
  size_t budget                             = 24000;
  record.provenance["sourceDescriptorTupleV1"] =
      QString::fromUtf8(QJsonDocument(abrMetadata(preset, budget))
                            .toJson(QJsonDocument::Compact));
  if (QJsonDocument(record.provenance).toJson().size() > 16384)
    throw Invalid("ABR preset metadata exceeds portable budget");
  // Static-native describes a complete static settings conversion, not a
  // dynamic preset with its active settings silently omitted.
  QStringList omitted;
  auto report = [&](const std::string &key) {
    omitted << QString::fromStdString(key);
  };
  auto disabled = [&](const char *key) {
    const auto *value = abrField(preset, key);
    return value && value->type == "bool" && !value->boolean;
  };
  const QSet<QString> staticKeys = {"Nm  ", "Brsh"};
  const QSet<QString> switches   = {
      "useTipDynamics",   "useScatter",  "useTexture", "usePaintDynamics",
      "useColorDynamics", "Wtdg",        "Nose",       "Rpt ",
      "useBrushPose",     "useBrushSize"};
  const QSet<QString> shapeKeys    = {"flipX",
                                      "flipY",
                                      "brushProjection",
                                      "minimumDiameter",
                                      "minimumRoundness",
                                      "tiltScale",
                                      "szVr",
                                      "angleDynamics",
                                      "roundnessDynamics"};
  const QSet<QString> scatterKeys  = {"Cnt ", "bothAxes", "countDynamics",
                                      "scatterDynamics"};
  const QSet<QString> transferKeys = {"prVr", "opVr", "wtVr", "mxVr"};
  for (const auto &value : preset.children) {
    const QString key = QString::fromStdString(value.key);
    if (staticKeys.contains(key)) continue;
    if (switches.contains(key)) {
      if (value.type != "bool" || value.boolean) report(value.key);
    } else if ((shapeKeys.contains(key) && disabled("useTipDynamics")) ||
               (scatterKeys.contains(key) && disabled("useScatter")) ||
               (transferKeys.contains(key) && disabled("usePaintDynamics"))) {
      // Explicitly disabled groups may retain dormant editor values.
    } else if (key == "dualBrush" || key == "brushGroup") {
      const auto *enabled = abrField(
          value, key == "dualBrush" ? "useDualBrush" : "useBrushGroup");
      if (value.type != "Objc" || value.classId != value.key || !enabled ||
          enabled->type != "bool" || enabled->boolean)
        report(value.key);
    } else {
      // Includes toolOptions, unknown fields and orphan dynamics whose enabling
      // switch is absent. Names and plausible defaults are not semantic proof.
      report(value.key);
    }
  }
  const QSet<QString> tipKeys = {"Dmtr",  "Angl",  "Rndn", "Spcn",       "Intr",
                                 "flipX", "flipY", "Nm  ", "sampledData"};
  for (const auto &value : brush.children)
    if (!tipKeys.contains(QString::fromStdString(value.key)))
      report("Brsh/" + value.key);
  if (!omitted.empty()) {
    record.provenance["conversionKind"] = "partial-native";
    record.diagnostics << "Partial native conversion: active or unknown source "
                          "settings were not applied. This is not a complete "
                          "preset conversion.";
    // The typed descriptor retains the full bounded list; keep diagnostic text
    // small even for hostile descriptors with many arbitrary keys.
    record.diagnostics << "Unapplied settings: " +
                              omitted.mid(0, 32).join(", ").left(4096);
    if (omitted.size() > 32)
      record.diagnostics
          << "Additional unapplied fields remain in internal source metadata.";
    if (omitted.contains("dualBrush"))
      record.diagnostics
          << "ABR dual brush is not applied; its original mode and "
             "settings are preserved without substituting a mask mode.";
  }
  record.diagnostics
      << "ABR 6.2 preset/sample binding decoded. Static geometry "
         "uses native angle, aspect, flips and spacing semantics; "
         "source dynamics and rendering remain unverified.";
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
// Bundle dependency lookup follows manifest resource kinds. Identical PNG
// bytes registered as a brush do not become a source pattern (or vice versa).
struct BundleResources {
  QMap<QString, QByteArray> brushes, patterns;
};
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
// Source qualification cannot inherit the native converter's tolerance for
// unknown curve attributes or duplicate curve nodes. Those may encode active
// source behavior even when the final decoded point list looks familiar.
bool eligiblePressureSensor(const QString &sensorXml) {
  QXmlStreamReader xml(sensorXml);
  int depth = 0, roots = 0, curves = 0;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isEntityReference()) return false;
    if (xml.isDTD() && xml.text().toString().trimmed() != "<!DOCTYPE params>")
      return false;
    if (xml.isStartElement()) {
      ++depth;
      if (!xml.namespaceUri().isEmpty() || !xml.namespaceDeclarations().empty())
        return false;
      if (depth == 1 && xml.name() == QStringLiteral("params")) {
        if (++roots != 1 || xml.attributes().size() != 1 ||
            xml.attributes().value("id") != QStringLiteral("pressure"))
          return false;
      } else if (depth == 2 && xml.name() == QStringLiteral("curve")) {
        if (++curves != 1 || !xml.attributes().empty()) return false;
      } else
        return false;
    } else if (xml.isEndElement()) {
      --depth;
    } else if (xml.isCharacters() && !xml.isWhitespace() && depth != 2) {
      return false;
    }
  }
  return !xml.hasError() && depth == 0 && roots == 1 && curves == 1;
}
// Versioned source HSV admission: independently measured, one constant
// pressure curve at a time. Unknown sensors, active combinations and varying
// curves remain explicit native conversions rather than source candidates.
bool sourceHsvSettings(const QMap<QString, QString> &fields,
                       SourceHsvChannel &channel, double &value,
                       QMap<QString, QString> &accepted) {
  channel = SourceHsvChannel::Off;
  value   = .5;
  const QStringList prefixes{"h", "s", "v"};
  const QStringList suffixes{"UseCurve",  "UseSameCurve", "Value",
                             "curveMode", "commonCurve",  "Sensor"};
  bool present = false;
  for (const auto &prefix : prefixes) {
    present = present || fields.contains("Pressure" + prefix);
    for (const auto &suffix : suffixes)
      present = present || fields.contains(prefix + suffix);
  }
  if (!present) return true;
  int payloads = 0;
  for (int index = 0; index < prefixes.size(); ++index) {
    const QString prefix = prefixes[index], flag = "Pressure" + prefix;
    if (fields.value(flag) != "false" && fields.value(flag) != "true")
      return false;
    accepted[flag] = fields[flag];
    bool payload   = false;
    for (const auto &suffix : suffixes)
      payload = payload || fields.contains(prefix + suffix);
    if (!payload) {
      if (fields[flag] == "true") return false;
      continue;
    }
    if (++payloads > 1 || fields.value(prefix + "UseCurve") != "true" ||
        fields.value(prefix + "UseSameCurve") != "true" ||
        fields.value(prefix + "Value") != "1" ||
        fields.value(prefix + "curveMode") != "0" ||
        !eligiblePressureSensor(fields.value(prefix + "Sensor")))
      return false;
    auto active  = fields;
    active[flag] = "true";
    Dynamic curve{1, 0, {}};
    QSet<QString> mapped;
    QStringList diagnostics;
    if (!convertCurve(active, prefix, curve, mapped, diagnostics) ||
        curve.mappings.size() != 1)
      return false;
    const auto &knots = curve.mappings.front().curve;
    if (knots.size() != 2 || knots[0].x != 0 || knots[1].x != 1 ||
        knots[0].y != knots[1].y)
      return false;
    for (const auto &suffix : suffixes) {
      if (!fields.contains(prefix + suffix)) return false;
      accepted[prefix + suffix] = fields[prefix + suffix];
    }
    if (fields[flag] == "true") {
      channel = SourceHsvChannel(index + 1);
      value   = knots[0].y;
    }
  }
  return true;
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
      {"PaintOpAction", "1"},
      {"PaintOpSettings/isAirbrushing", "false"},
      {"PaintOpSettings/ignoreSpacing", "false"},
      {"PaintOpSettings/updateSpacingBetweenDabs", "false"},
      {"KisPrecisionOption/AutoPrecisionEnabled", "false"},
      {"KisPrecisionOption/precisionLevel", "5"}};
  SourceHsvChannel hsvChannel;
  double hsvValue;
  QMap<QString, QString> hsvFields;
  if (!sourceHsvSettings(fields, hsvChannel, hsvValue, hsvFields) ||
      hsvChannel != preset.sourceHsvChannel ||
      hsvValue != preset.sourceHsvValue ||
      (!hsvFields.empty() && engine != "paintbrush"))
    return false;
  for (auto i = hsvFields.begin(); i != hsvFields.end(); ++i)
    required[i.key()] = i.value();
  if (fields.contains("Spacing/Isotropic"))
    required["Spacing/Isotropic"] = "false";
  if (fields.value("Texture/Pattern/Enabled") == "true") {
    if (!preset.texture.image) return false;
    const QMap<QString, QString> textureFixed = {
        {"Texture/Pattern/Enabled", "true"},
        {"Texture/Pattern/Scale", "1"},
        {"Texture/Pattern/Brightness", "0"},
        {"Texture/Pattern/Contrast", "1"},
        {"Texture/Pattern/NeutralPoint", ".5"},
        {"Texture/Pattern/OffsetX", "0"},
        {"Texture/Pattern/OffsetY", "0"},
        {"Texture/Pattern/isRandomOffsetX", "false"},
        {"Texture/Pattern/isRandomOffsetY", "false"},
        {"Texture/Pattern/CutoffPolicy", "0"},
        {"Texture/Pattern/CutoffLeft", "0"},
        {"Texture/Pattern/CutoffRight", "255"},
        {"Texture/Pattern/AutoInvertOnErase", "false"},
        {"PressureTexture/Strength/", "true"},
        {"Texture/Strength/UseCurve", "false"}};
    for (auto i = textureFixed.begin(); i != textureFixed.end(); ++i) {
      const QString actual = fields.value(i.key());
      bool actualNumeric = false, expectedNumeric = false;
      const double a = actual.toDouble(&actualNumeric),
                   b = i.value().toDouble(&expectedNumeric);
      if (!fields.contains(i.key()) ||
          (expectedNumeric ? (!actualNumeric || !std::isfinite(a) || a != b)
                           : actual != i.value()))
        return false;
      required[i.key()] = actual;
    }
    for (const QString &key :
         {"Texture/Pattern/Invert", "Texture/Pattern/UseSoftTexturing"}) {
      if (fields.value(key) != "false" && fields.value(key) != "true")
        return false;
      required[key] = fields[key];
    }
    if (fields.value("Texture/Pattern/TexturingMode") != "0" &&
        fields.value("Texture/Pattern/TexturingMode") != "1")
      return false;
    required["Texture/Pattern/TexturingMode"] =
        fields["Texture/Pattern/TexturingMode"];
    if (!fields.contains("Texture/Strength/Value")) return false;
    required["Texture/Strength/Value"] = fields["Texture/Strength/Value"];
    for (const QString &key :
         {"Texture/Pattern/Name", "Texture/Pattern/PatternFileName",
          "Texture/Pattern/PatternMD5Sum", "Texture/Pattern/PatternMD5",
          "Texture/Pattern/Pattern"})
      if (fields.contains(key)) required[key] = fields[key];
  }
  // With FlowUseCurve=false the source's static FlowValue is inactive in
  // build-up Pixel and all four measured Color Smudge models. Preserve it.
  if (!fields.contains("FlowValue") || !fields.contains("OpacityValue"))
    return false;
  required["FlowValue"]    = fields["FlowValue"];
  required["OpacityValue"] = fields["OpacityValue"];
  if (engine == "paintbrush") {
    const QString blend = fields.value("CompositeOp");
    if (blend != "normal" && blend != "multiply" && blend != "screen" &&
        blend != "overlay")
      return false;
    required["CompositeOp"] = blend;
    for (const QString &key : {"OpacityValue", "FlowValue"}) {
      if (!fields.contains(key)) return false;
      required[key] = fields[key];  // Finite 0..1 values checked by validate().
    }
    if (fields.value("PaintOpAction") != "0" &&
        fields.value("PaintOpAction") != "1" &&
        fields.value("PaintOpAction") != "2")
      return false;
    required["PaintOpAction"] = fields["PaintOpAction"];
  }
  if (fields.contains("PressureFlow")) {
    if (fields.value("PressureFlow") != "true" &&
        fields.value("PressureFlow") != "false")
      return false;
    // With FlowUseCurve=false both flag states have identical measured static
    // behavior.
    required["PressureFlow"] = fields["PressureFlow"];
  }
  if (engine == "paintbrush" && fields.value("PressureSize") == "true") {
    if (preset.size.mappings.size() != 1) return false;
    required["PressureSize"] = "true";
    required["SizeUseCurve"] = "true";
    const QString sameCurve  = fields.value("SizeUseSameCurve");
    if (sameCurve != "true" && sameCurve != "false") return false;
    required["SizeUseSameCurve"] = sameCurve;
    if (sameCurve == "true" && !fields.contains("SizecommonCurve"))
      return false;
    if (fields.contains("SizecommonCurve"))
      required["SizecommonCurve"] = fields["SizecommonCurve"];
    required["SizeSensor"] = fields.value("SizeSensor");
    // convertCurve resolves either the shared curve or the sensor-local
    // curve. The candidate uses measured natural cubic interpolation for
    // multiple knots; native conversion retains its explicit linear mapping.
    if (fields.contains("SizecurveMode")) required["SizecurveMode"] = "0";
  }
  if (engine == "paintbrush") {
    // Original paired Node traces establish pressure multiplication for hard
    // Pixel opacity and wash flow. Build-up flow remains inactive. Do not
    // infer these equations from Qt events whose source pressure consumption
    // has not yet been established.
    for (const QString prefix : {"Opacity", "Flow"}) {
      if (fields.value(prefix + "UseCurve") != "true") continue;
      const QString enabled = fields.value("Pressure" + prefix);
      if (enabled != "true" && enabled != "false") return false;
      auto active                 = fields;
      active["Pressure" + prefix] = "true";
      Dynamic retained{1, 0, {}};
      QSet<QString> mapped;
      QStringList diagnostics;
      if (!convertCurve(active, prefix, retained, mapped, diagnostics) ||
          retained.mappings.size() != 1)
        return false;
      const auto &mapping = retained.mappings.front();
      if (mapping.sensor != Sensor::Pressure || mapping.curve.size() != 2 ||
          mapping.curve[0].x != 0 || mapping.curve[0].y != 0 ||
          mapping.curve[1].x != 1 || mapping.curve[1].y != 1)
        return false;
      required["Pressure" + prefix]     = enabled;
      required[prefix + "UseCurve"]     = "true";
      required[prefix + "UseSameCurve"] = "true";
      required[prefix + "commonCurve"]  = "0,0;1,1;";
      required[prefix + "Sensor"]       = fields.value(prefix + "Sensor");
      if (fields.contains(prefix + "curveMode"))
        required[prefix + "curveMode"] = "0";
    }
  }
  if (engine == "colorsmudge") {
    for (const QString &key : {"PressureColorRate", "PressureSmudgeRate",
                               "PressureSmudgeRadius", "SmudgeRateSmearAlpha"})
      required[key] = "true";
    for (const QString &key :
         {"PressureSmudgeRate", "SmudgeRateSmearAlpha", "PressureColorRate"}) {
      if (fields.value(key) != "true" && fields.value(key) != "false")
        return false;
      required[key] = fields[key];
    }
    for (const QString &key :
         {"ColorRateUseCurve", "SmudgeRateUseCurve", "SmudgeRadiusUseCurve"})
      required[key] = "false";
    for (const QString &prefix :
         {"SmudgeRate", "ColorRate", "Opacity", "Flow"}) {
      if (fields.value(prefix + "UseCurve") != "true") continue;
      const Dynamic &control = prefix == "SmudgeRate"  ? preset.pickupControl
                               : prefix == "ColorRate" ? preset.colorRateControl
                               : prefix == "Opacity"   ? preset.opacity
                                                       : preset.flow;
      const QString enabled  = fields.value("Pressure" + prefix);
      if (enabled != "true" && !(prefix == "Flow" && enabled == "false"))
        return false;
      if (enabled == "false") {
        // Flow is inactive for this source engine, including a disabled
        // pressure checkbox with its curve retained. Parse that retained
        // identity curve instead of accepting arbitrary unknown sensor XML.
        auto activeFields            = fields;
        activeFields["PressureFlow"] = "true";
        Dynamic retained{1, 0, {}};
        QSet<QString> mapped;
        QStringList diagnostics;
        if (!convertCurve(activeFields, prefix, retained, mapped,
                          diagnostics) ||
            retained.mappings.size() != 1 ||
            retained.mappings.front().sensor != Sensor::Pressure)
          return false;
      } else if (control.mappings.size() != 1)
        return false;
      required["Pressure" + prefix]     = enabled;
      required[prefix + "UseCurve"]     = "true";
      required[prefix + "UseSameCurve"] = "true";
      required[prefix + "commonCurve"]  = "0,0;1,1;";
      required[prefix + "Sensor"]       = fields.value(prefix + "Sensor");
      if (fields.contains(prefix + "curveMode"))
        required[prefix + "curveMode"] = "0";
    }
    required["SmudgeRadiusValue"] = "0";
    if (preset.wet == Wet::Dulling) {
      bool ok             = false;
      const double radius = fields.value("SmudgeRadiusValue").toDouble(&ok);
      const double maximum =
          fields.value("SmudgeRateUseNewEngine") == "true" ? 300 : 100;
      if (!ok || !std::isfinite(radius) || radius < 0 || radius > maximum)
        return false;
      required["SmudgeRadiusValue"] = fields["SmudgeRadiusValue"];
    }
    if (preset.wet == Wet::Smear) {
      // The public K5341 manual defines radius as Dulling-only. Retain the
      // serialized inactive value as provenance; never apply native radius.
      bool ok             = false;
      const double radius = fields.value("SmudgeRadiusValue").toDouble(&ok);
      if (!ok || !std::isfinite(radius) || radius < 0 || radius > 300)
        return false;
      required["SmudgeRadiusValue"] = fields["SmudgeRadiusValue"];
    }
    for (const QString &key : {"SmudgeRateValue", "ColorRateValue",
                               "SmudgeRateMode", "SmudgeRateUseNewEngine"})
      if (!fields.contains(key)) return false;
  }
  for (auto i = required.begin(); i != required.end(); ++i)
    if (!fields.contains(i.key()) || fields.value(i.key()) != i.value())
      return false;
  for (auto i = required.begin(); i != required.end(); ++i)
    if (i.key().endsWith("Sensor") && !eligiblePressureSensor(i.value()))
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
    QMap<QString, QString> exact =
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
    QStringList variable  = {brush ? "spacing" : "diameter"};
    if (engine == "paintbrush") {
      const QString geometry = brush ? "angle" : "ratio";
      exact.remove(geometry);
      variable << geometry;
    }
    if (!brush && engine == "paintbrush" && preset.tip.shape == Shape::Square)
      exact["type"] = "rect";
    if (!brush && engine == "paintbrush" && preset.tip.antialiasEdges) {
      if (attributes.value("antialiasEdges") != QStringLiteral("1"))
        return false;
      exact["antialiasEdges"] = "1";
    }
    if (!brush && engine == "paintbrush" && preset.tip.hardness < 1) {
      bool horizontalOk = false, verticalOk = false;
      const double horizontal =
          attributes.value("hfade").toDouble(&horizontalOk);
      const double vertical = attributes.value("vfade").toDouble(&verticalOk);
      if (!horizontalOk || !verticalOk || horizontal != vertical ||
          horizontal != preset.tip.hardness)
        return false;
      exact.remove("hfade");
      exact.remove("vfade");
      variable << "hfade" << "vfade";
    }
    const QSet<QString> numericAttributes = {
        "angle", "ratio", "hfade", "vfade", "randomness", "density", "spikes"};
    for (auto i = exact.begin(); i != exact.end(); ++i) {
      if (!attributes.hasAttribute(i.key())) return false;
      if (numericAttributes.contains(i.key())) {
        bool ok            = false;
        const double value = attributes.value(i.key()).toDouble(&ok);
        if (!ok || !std::isfinite(value) || value != i.value().toDouble())
          return false;
      } else if (attributes.value(i.key()) != i.value())
        return false;
    }
    for (const QString &name : variable)
      if (!attributes.hasAttribute(name)) return false;
    for (const auto &attribute : attributes)
      if (!exact.contains(attribute.name().toString()) &&
          !variable.contains(attribute.name().toString()))
        return false;
  }
  return !xml.hasError() && brushes == 1 && masks == 1 &&
         validate(preset).empty();
}
Record convertKpp(const QByteArray &bytes, QStringList &diagnostics,
                  const BundleResources *resources = nullptr,
                  size_t maximumDecodedBytes       = 256u * 1024u * 1024u) {
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
        r.preset.tip = resolveTip(resources ? &resources->brushes : nullptr,
                                  a.value("filename").toString(),
                                  a.value("md5sum").toString(), diagnostics,
                                  maximumDecodedBytes, application);
        r.preset.size.value = std::max(r.preset.tip.images[0]->width,
                                       r.preset.tip.images[0]->height) *
                              numeric(a.value("scale").toString());
        maskFound = true;
      }
      found            = true;
      r.preset.spacing = numeric(a.value("spacing").toString());
      // KPP positive rotation is counterclockwise in raster coordinates.
      // Original 16/64px ellipse masks at 45 degrees establish this sign.
      r.preset.angle.value =
          -numeric(a.value("angle").toString()) * 180 / 3.14159265358979323846;
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
    if (r.preset.blend != Blend::Normal)
      throw Invalid(
          "KPP Color Smudge supports only Normal blending without "
          "eraser mode; the requested mode cannot be preserved");
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
    r.preset.wet = mode == "0" ? Wet::Smear : Wet::Dulling;
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
          ? (candidate.tip.shape == Shape::Square
                 ? "krita-5.3.4.1-default-rect-candidate-1"
             : candidate.tip.hardness < 1 || candidate.tip.antialiasEdges
                 ? "krita-5.3.4.1-default-circle-candidate-1"
                 : "krita-5.3.4.1-hard-pixel-candidate-1")
          : "krita-5.3.4.1-" +
                r.provenance["sourceSmudgeModel"].toString().toStdString() +
                "-candidate-1";
  if (engine == "colorsmudge") {
    candidate.smudgeRadius =
        candidate.wet == Wet::Dulling
            ? fields.value("SmudgeRadiusValue").toDouble() / 100
            : 0;
    candidate.sourceSmudgeLengthEnabled =
        fields.value("PressureSmudgeRate") == "true";
    candidate.sourceSmearAlpha = fields.value("SmudgeRateSmearAlpha") == "true";
    candidate.sourceColorRateEnabled =
        fields.value("PressureColorRate") == "true";
  } else
    candidate.wash = fields.value("PaintOpAction") != "1";
  if (fields.value("Texture/Pattern/Enabled") == "true" &&
      (fields.value("Texture/Pattern/TexturingMode") == "0" ||
       fields.value("Texture/Pattern/TexturingMode") == "1")) {
    QByteArray pattern;
    const QByteArray encoded =
        fields.value("Texture/Pattern/Pattern").toLatin1();
    if (!encoded.isEmpty()) {
      pattern = QByteArray::fromBase64(encoded);
      if (pattern.isEmpty() || pattern.toBase64() != encoded)
        throw Invalid("Noncanonical or invalid embedded texture encoding");
    } else if (resources) {
      const QString filename = fields.value("Texture/Pattern/PatternFileName");
      const QString checksum =
          fields.value("Texture/Pattern/PatternMD5Sum").toLower();
      const QByteArray binaryChecksum =
          fields.value("Texture/Pattern/PatternMD5").toLatin1();
      QStringList matches;
      for (auto i = resources->patterns.begin(); i != resources->patterns.end();
           ++i) {
        const QByteArray hash =
            QCryptographicHash::hash(i.value(), QCryptographicHash::Md5);
        const bool match = !checksum.isEmpty()
                               ? QString::fromLatin1(hash.toHex()) == checksum
                           : !binaryChecksum.isEmpty()
                               ? hash.toBase64() == binaryChecksum
                               : (!filename.isEmpty() &&
                                  (i.key() == filename ||
                                   QFileInfo(i.key()).fileName() == filename));
        if (match) matches << i.key();
      }
      if (matches.size() != 1)
        throw Invalid("Missing or ambiguous KPP texture dependency");
      pattern = resources->patterns.value(matches.front());
    } else
      throw Invalid("KPP texture resource missing; import its resource bundle");
    if (!pattern.startsWith(QByteArray::fromHex("89504e470d0a1a0a")))
      throw Invalid("Source texture candidate requires a PNG pattern");
    const QString declaredHash =
        fields.value("Texture/Pattern/PatternMD5Sum").toLower();
    if (!declaredHash.isEmpty() &&
        declaredHash !=
            QString::fromLatin1(
                QCryptographicHash::hash(pattern, QCryptographicHash::Md5)
                    .toHex()))
      throw Invalid("KPP texture checksum mismatch");
    const QByteArray binaryHash =
        fields.value("Texture/Pattern/PatternMD5").toLatin1();
    if (!binaryHash.isEmpty() &&
        binaryHash != QCryptographicHash::hash(pattern, QCryptographicHash::Md5)
                          .toBase64())
      throw Invalid("KPP embedded texture checksum mismatch");
    size_t used = 0;
    for (const auto &image : candidate.tip.images) {
      const size_t bytes = image->pixels.size() * sizeof(Pixel);
      if (bytes > maximumDecodedBytes - used)
        throw Invalid("KPP texture aggregate budget exceeded");
      used += bytes;
    }
    auto image = decodeImage(pattern, true, error, maximumDecodedBytes - used);
    if (!image) throw std::runtime_error(error.toStdString());
    const bool gray = std::all_of(
        image->pixels.begin(), image->pixels.end(), [](Pixel pixel) {
          return pixel.a == 1 && pixel.r == pixel.g && pixel.g == pixel.b;
        });
    if (gray) {
      // Retain source grayscale as mask data, not its uniformly opaque PNG
      // alpha. Original resource bytes remain identified by provenance.
      auto mask = std::const_pointer_cast<Image>(image);
      for (auto &pixel : mask->pixels) pixel = {0, 0, 0, pixel.r};
      mask->color             = false;
      candidate.texture.image = std::move(image);
      candidate.texture.mode =
          fields.value("Texture/Pattern/TexturingMode") == "0"
              ? MaskMode::Multiply
              : MaskMode::Subtract;
      candidate.texture.depth = numeric(fields.value("Texture/Strength/Value"));
      candidate.texture.invert =
          fields.value("Texture/Pattern/Invert") == "true";
      candidate.sourceTextureSoftTexturing =
          fields.value("Texture/Pattern/UseSoftTexturing") == "true";
      r.provenance["sourceTextureSha256"] = QString::fromLatin1(
          QCryptographicHash::hash(pattern, QCryptographicHash::Sha256)
              .toHex());
    } else
      diagnostics << "Source texture candidate requires opaque grayscale; "
                     "color/alpha pattern interpretation is unverified.";
  }
  if (engine == "paintbrush" && candidate.size.mappings.size() == 1 &&
      candidate.size.mappings.front().curve.size() > 2)
    candidate.size.mappings.front().interpolation = Interpolation::NaturalCubic;
  QMap<QString, QString> hsvFields;
  sourceHsvSettings(fields, candidate.sourceHsvChannel,
                    candidate.sourceHsvValue, hsvFields);
  if (eligibleFixedSource(fields, engine, candidate)) {
    r.preset = std::move(candidate);
    if (engine == "colorsmudge" && fields.value("FlowUseCurve") == "true") {
      r.provenance["sourceInactiveFlowCurve"] = "identity-pressure";
      r.provenance["sourceInactiveFlowPressureEnabled"] =
          fields.value("PressureFlow") == "true";
    }
    if (r.preset.wet == Wet::Smear)
      r.provenance["inactiveSourceSmudgeRadiusPercent"] =
          fields.value("SmudgeRadiusValue").toDouble();
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
                          BundleResources &resources,
                          QMap<QString, QJsonArray> &tags,
                          QJsonArray &resourceTags) {
  if (members.value("mimetype").trimmed() !=
      "application/x-krita-resourcebundle")
    throw Invalid("Invalid Krita bundle MIME type");
  auto xml = members.value("META-INF/manifest.xml");
  if (xml.isEmpty() || xml.size() > 1024 * 1024)
    throw Invalid("Missing or oversized bundle manifest");
  QXmlStreamReader reader(xml);
  QStringList presets;
  QSet<QString> paths;
  bool root = false, tagsSeen = false;
  QStringList stack;
  QString currentPath, currentType;
  const QString ns = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0";
  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isDTD() || reader.isEntityReference())
      throw Invalid("Bundle manifest entities are unsupported");
    if (reader.isEndElement()) {
      if (reader.name() == QStringLiteral("file-entry") &&
          !currentPath.isEmpty()) {
        if (!tags.value(currentPath).isEmpty())
          resourceTags.append(QJsonObject{{"member", currentPath},
                                          {"mediaType", currentType},
                                          {"tags", tags.value(currentPath)}});
        currentPath.clear();
      }
      if (!stack.isEmpty()) stack.removeLast();
      continue;
    }
    if (reader.isCharacters() && !reader.isWhitespace())
      throw Invalid("Unsupported text outside bundle tag");
    if (!reader.isStartElement()) continue;
    if (!root) {
      if (reader.name() != QStringLiteral("manifest") ||
          reader.namespaceUri() != ns ||
          reader.attributes().value(ns, "version") != QStringLiteral("1.2"))
        throw Invalid("Unsupported bundle manifest version");
      root = true;
      stack << "manifest";
      continue;
    }
    if (reader.namespaceUri() != ns)
      throw Invalid("Unsupported bundle tag namespace");
    if (reader.name() == QStringLiteral("file-entry")) {
      if (stack != QStringList{"manifest"})
        throw Invalid("Unsupported nested bundle resource");
      stack << "file-entry";
      tagsSeen     = false;
      auto a       = reader.attributes();
      QString path = a.value(ns, "full-path").toString(),
              type = a.value(ns, "media-type").toString();
      if (paths.contains(path)) throw Invalid("Duplicate bundle manifest path");
      paths.insert(path);
      currentPath = path;
      currentType = type;
      if (path == "/") {
        if (type != "application/x-krita-resourcebundle")
          throw Invalid("Invalid bundle root declaration");
        currentPath.clear();
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
      if (type == "brushes") resources.brushes[path] = members[path];
      if (type == "patterns") resources.patterns[path] = members[path];
      if (type == "paintoppresets") {
        if (!path.endsWith(".kpp", Qt::CaseInsensitive))
          throw Invalid("Bundle preset type mismatch");
        presets << path;
      }
    } else if (reader.name() == QStringLiteral("tags")) {
      if (stack != QStringList{"manifest", "file-entry"} || tagsSeen ||
          currentPath.isEmpty() || !reader.attributes().isEmpty())
        throw Invalid("Unsupported bundle tags structure");
      tagsSeen = true;
      stack << "tags";
    } else if (reader.name() == QStringLiteral("tag")) {
      if (stack != QStringList{"manifest", "file-entry", "tags"} ||
          !reader.attributes().isEmpty())
        throw Invalid("Unsupported bundle tag structure");
      // Krita's manifest writer emits text-only manifest:tags/manifest:tag.
      // Preserve order, duplicates, whitespace and Unicode; labels are opaque.
      const QString label =
          reader.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement);
      if (reader.hasError() || label.size() > 256 ||
          tags[currentPath].size() >= 64)
        throw Invalid("Unsupported or oversized bundle tag");
      tags[currentPath].append(label);
    } else
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
    if (kind == "png") {
      auto image = decodeImage(bytes, true, error, maximumDecodedBytes);
      if (!image || !error.isEmpty()) return {};
      Record record;
      record.decoded          = true;
      record.preset.name      = "Imported PNG tip";
      record.preset.tip.shape = Shape::Bitmap;
      record.preset.tip.images.push_back(image);
      geometry(record.preset, *image, .1);
      record.preset.size.value = std::min(4096.0, record.preset.size.value);
      record.provenance        = {{"format", "png"}, {"resourceOnly", true}};
      check(record);
      result.records.push_back(std::move(record));
      result.complete = true;
      return result;
    }
    if (kind == "bundle") {
      auto members = brushArchive(bytes, error);
      if (!error.isEmpty()) return {};
      BundleResources resources;
      QMap<QString, QJsonArray> tags;
      QJsonArray resourceTags;
      auto presets   = bundlePresets(members, resources, tags, resourceTags);
      size_t decoded = 0;
      for (const auto &path : presets) {
        checkCancelled();
        QStringList diagnostics;
        auto r = convertKpp(members[path], diagnostics, &resources,
                            maximumDecodedBytes - decoded);
        check(r);
        // Retained textures and secondary tips consume the same bundle budget
        // as primary tips. Charge them before the next preset is decoded, and
        // count a shared image only once within this record.
        QSet<const Image *> retained;
        auto account = [&](const std::shared_ptr<const Image> &image) {
          if (!image || retained.contains(image.get())) return;
          retained.insert(image.get());
          if (image->pixels.size() >
              (maximumDecodedBytes - decoded) / sizeof(Pixel))
            throw Invalid("Bundle decoded image budget exceeded");
          decoded += image->pixels.size() * sizeof(Pixel);
        };
        for (const auto &image : r.preset.tip.images) account(image);
        for (const auto &image : r.preset.secondary.tip.images) account(image);
        account(r.preset.texture.image);
        r.provenance["bundleSha256"] = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                .toHex());
        r.provenance["bundleMember"] = path;
        QJsonObject metadata{{"version", 1},
                             {"manifestVersion", "1.2"},
                             {"presetTags", tags.value(path)},
                             {"resourceTags", resourceTags}};
        const QString metadataError = Detail::bundleMetadataError(metadata);
        if (!metadataError.isEmpty())
          throw std::runtime_error(metadataError.toStdString());
        r.provenance["bundleMetadata"] = metadata;
        if (QJsonDocument(r.provenance).toJson().size() > 16384)
          throw Invalid(
              "Bundle tags and provenance exceed the preset metadata budget");
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
      for (const auto &pattern : parsed.patterns) {
        grayBytes += pattern.palette.size();
        for (const auto &channel : pattern.channels)
          grayBytes += channel.pixels.size();
      }
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
        r.provenance["conversionKind"]   = "static-native";
        r.diagnostics.append(result.diagnostics);
        r.diagnostics << "Computed ellipse converted to native geometry. "
                         "Source hardness, angle and spacing behavior is "
                         "not reference-verified; zero spacing is unsupported.";
        check(r);
        result.records.push_back(r);
      }
      std::vector<Record> sampledRecords;
      for (const auto &tip : parsed.tips) {
        checkCancelled();
        auto r        = converted(bytes, kind);
        r.preset.name = tip.name;
        r.diagnostics.append(result.diagnostics);
        r.provenance["containerVersion"]    = parsed.version;
        r.provenance["containerSubversion"] = parsed.subVersion;
        r.provenance["conversionKind"]      = "tip-only";
        r.diagnostics << "Tip extraction only: this record does not import the "
                         "source brush preset.";
        r.provenance["decodedDescriptorCount"] = int(parsed.descriptors.size());
        r.provenance["decodedPatternCount"]    = int(parsed.patterns.size());
        r.provenance["patternDataComplete"]    = parsed.patternDataComplete;
        r.provenance["sourceTextureSemanticallySupported"] = false;
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
        sampledRecords.push_back(r);
      }
      if (parsed.version == 6 && parsed.subVersion == 2 &&
          !parsed.descriptors.empty()) {
        std::map<std::string, size_t> samples;
        for (size_t i = 0; i < parsed.tips.size(); ++i)
          if (parsed.tips[i].sampleId.empty() ||
              !samples.emplace(parsed.tips[i].sampleId, i).second)
            throw Invalid("Ambiguous ABR sample identifiers");
        std::vector<bool> used(parsed.tips.size(), false);
        int ordinal = 0;
        for (const auto &root : parsed.descriptors) {
          const auto *list = abrField(root, "Brsh");
          if (root.classId != "null" || !list || list->type != "VlLs") {
            result.diagnostics << "Unrecognized ABR descriptor root; tips "
                                  "remain native conversions.";
            continue;
          }
          for (const auto &preset : list->children) {
            checkCancelled();
            if (++ordinal > 256)
              throw Invalid("ABR preset count budget exceeded");
            const auto *brush = abrField(preset, "Brsh");
            if (preset.type != "Objc" || preset.classId != "brushPreset" ||
                !brush || brush->type != "Objc" ||
                brush->classId != "sampledBrush") {
              result.diagnostics
                  << "Unsupported ABR descriptor preset/tip class.";
              continue;
            }
            const auto &id = abrRequired(*brush, "sampledData", "TEXT").text;
            auto found     = samples.find(id);
            if (found == samples.end())
              throw Invalid("ABR preset references a missing sample");
            auto record = sampledRecords[found->second];
            abrStaticPreset(record, preset, ordinal);
            check(record);
            result.records.push_back(std::move(record));
            used[found->second] = true;
          }
        }
        for (size_t i = 0; i < sampledRecords.size(); ++i)
          if (!used[i]) result.records.push_back(std::move(sampledRecords[i]));
      } else {
        for (auto &record : sampledRecords)
          result.records.push_back(std::move(record));
      }
      if (result.records.size() > 256)
        throw Invalid("ABR record count budget exceeded");
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
