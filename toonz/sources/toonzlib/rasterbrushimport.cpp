#include "toonz/rasterbrushimport.h"
#include "toonz/abrparser.h"
#include "rasterbrushpng.h"
#include <QXmlStreamReader>
#include <QSet>
#include <cmath>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <stdexcept>

namespace RasterBrush {
namespace {
constexpr int fileLimit = 64 * 1024 * 1024;
struct Invalid : std::runtime_error {
  explicit Invalid(const char *s) : std::runtime_error(s) {}
};
struct Reader {
  const QByteArray &bytes;
  size_t pos = 0, total = 0;
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
    if (n > 256u * 1024u * 1024u - total)
      throw Invalid("Aggregate brush budget exceeded");
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
    if (label.back() != 0) throw Invalid("GBR name is not terminated");
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
Record convertKpp(const QByteArray &bytes, QStringList &diagnostics) {
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
      if (found || a.value("type") != QStringLiteral("auto_brush"))
        throw Invalid(
            "KPP conversion requires a generated tip; external tips must be "
            "resolved first");
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
  QStringList unmapped;
  for (auto i = fields.begin(); i != fields.end(); ++i)
    if (!mapped.contains(i.key())) unmapped << i.key();
  diagnostics
      << "Generated tip, geometry and static opacity/flow are converted to "
         "native definitions; source edge and spacing mathematics may differ.";
  if (!unmapped.isEmpty())
    diagnostics << "Source settings not reproduced: " + unmapped.join(", ");
  r.provenance["conversionReport"] = diagnostics.join("\n");
  return r;
}
void check(Record &r) {
  auto error = validate(r.preset);
  if (!error.empty()) throw std::runtime_error(error);
  r.semanticallySupported = true;
}
}  // namespace
ImportResult importBrush(const QByteArray &bytes, const QString &format,
                         QString &error) {
  ImportResult result;
  error.clear();
  try {
    if (bytes.isEmpty() || bytes.size() > fileLimit)
      throw Invalid("Brush input exceeds 64 MiB");
    const QString kind = format.toLower();
    if (kind == "gzbrush") {
      auto r = decode(bytes, error);
      if (!error.isEmpty()) return {};
      result.records.push_back(r);
      result.complete = true;
      return result;
    }
    if (kind == "gbr" || kind == "gih") {
      Reader reader{bytes};
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
      auto r = convertKpp(bytes, result.diagnostics);
      check(r);
      result.records.push_back(r);
    } else if (kind == "abr") {
      AbrParser::Result parsed;
      std::string why;
      AbrParser::Limits limits;
      limits.maxFileBytes    = fileLimit;
      limits.maxDecodedBytes = 8 * 1024 * 1024;
      limits.maxRecords      = 256;
      if (!AbrParser::parseAbr(
              reinterpret_cast<const unsigned char *>(bytes.constData()),
              bytes.size(), parsed, why, limits))
        throw std::runtime_error(why);
      for (const auto &w : parsed.warnings)
        result.diagnostics << QString::fromStdString(w);
      if (parsed.computedBrushes || parsed.skippedSamples ||
          !parsed.warnings.empty())
        result.diagnostics
            << "The ABR contains behavior or records that are not represented "
               "by these native tip conversions.";
      Reader allocator{bytes};
      for (const auto &tip : parsed.tips) {
        auto r                              = converted(bytes, kind);
        r.preset.name                       = tip.name;
        r.provenance["containerVersion"]    = parsed.version;
        r.provenance["containerSubversion"] = parsed.subVersion;
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
        throw Invalid("No decodable sampled tips in this ABR");
    } else
      throw Invalid("Unsupported brush format");
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
ImportResult importBrushFile(const QString &path, QString &error) {
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
  return importBrush(bytes, QFileInfo(path).suffix(), error);
}
}  // namespace RasterBrush
