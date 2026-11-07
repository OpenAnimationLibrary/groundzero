#include "color_metrics.h"
#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <utility>
#include <zlib.h>

using namespace RasterBrush;
namespace {
void require(bool v, const QString &message) {
  if (!v)
    throw std::runtime_error(message.toStdString());
}
Pixel pixel(QRgb q) {
  double a = qAlpha(q) / 255.0;
  return {qRed(q) / 255.0 * a, qGreen(q) / 255.0 * a, qBlue(q) / 255.0 * a, a};
}
QRgb encoded(Pixel p) {
  auto byte = [](double v) {
    return int(std::floor(std::clamp(v, 0.0, 1.0) * 255 + .5));
  };
  return qRgba(p.a > 0 ? byte(p.r / p.a) : 0, p.a > 0 ? byte(p.g / p.a) : 0,
               p.a > 0 ? byte(p.b / p.a) : 0, byte(p.a));
}
struct Canvas : Surface {
  int w, h;
  bool quantizedInput = true;
  std::vector<Pixel> data;
  explicit Canvas(const QImage &image)
      : w(image.width()), h(image.height()), data(size_t(w) * h) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        data[size_t(y) * w + x] = pixel(image.pixel(x, y));
  }
  int width() const override { return w; }
  int height() const override { return h; }
  PixelStorage storage() const override {
    // Initial data comes from straight QImage RGBA8. Renderer writes retain
    // arbitrary associated values, including sub-byte paint from other tracks.
    return quantizedInput ? PixelStorage::QuantizedStraightRgba8
                          : PixelStorage::FloatingPremultiplied;
  }
  bool prepare(int, int, int, int) override { return true; }
  Pixel read(int x, int y) const override { return data[size_t(y) * w + x]; }
  void write(int x, int y, const Pixel &p) override {
    quantizedInput = false;
    data[size_t(y) * w + x] = p;
  }
  QImage image() const {
    QImage result(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        result.setPixel(x, y, encoded(read(x, y)));
    return result;
  }
};
void applyViewRotation(Preset &preset, const QJsonObject &source) {
  if (!source.contains("brushRotationDegrees"))
    return;
  const auto modifier = source["brushRotationDegrees"];
  require(modifier.isDouble() && std::isfinite(modifier.toDouble()) &&
              std::abs(modifier.toDouble()) <= 36000,
          "Invalid source view brush rotation");
  // Imported fixed KPP angles already use raster-coordinate sign. Positive
  // source View rotation is counterclockwise: the original asymmetric8x8
  // resource's right edge becomes its top edge in the90degree capture.
  preset.angle.value -= modifier.toDouble();
}
void testViewRotation() {
  auto tip = std::make_shared<Image>();
  tip->width = tip->height = 3;
  tip->color = true;
  tip->pixels.resize(9);
  tip->pixels[5] = {1, 0, 0,
                    1}; // An off-center mark exposes rotation direction.
  auto render = [&](double serializedAngle, double viewAngle) {
    Preset preset;
    preset.tip.shape = Shape::Bitmap;
    preset.tip.images = {tip};
    preset.size.value = 12;
    preset.angle.value = serializedAngle;
    applyViewRotation(preset, QJsonObject{{"brushRotationDegrees", viewAngle}});
    QImage blank(64, 64, QImage::Format_ARGB32);
    blank.fill(Qt::transparent);
    Canvas canvas(blank);
    Stroke stroke(canvas, preset);
    Sample sample;
    sample.x = sample.y = 32;
    require(stroke.add(sample) && stroke.finish(),
            QString::fromStdString(stroke.error()));
    return canvas.image();
  };
  auto center = [](const QImage &image) {
    double mass = 0, x = 0, y = 0;
    for (int row = 0; row < image.height(); ++row)
      for (int col = 0; col < image.width(); ++col) {
        const double a = qAlpha(image.pixel(col, row));
        mass += a;
        x += (col + .5) * a;
        y += (row + .5) * a;
      }
    require(mass > 0, "Empty view-rotation regression stamp");
    return std::make_pair(x / mass, y / mass);
  };
  const auto plain = render(0, 0), quarter = render(0, 90);
  const auto p = center(plain), q = center(quarter),
             oblique = center(render(0, 40));
  require(p.first > 34 && std::abs(p.second - 32) < .1 && q.second < 30 &&
              std::abs(q.first - 32) < .1 && plain != quarter &&
              oblique.first > 33 && oblique.second < 31,
          "Source view rotation did not rotate asymmetric candidate "
          "counterclockwise");
  require(render(-37, 53) == quarter && render(23, -17) == render(40, 0),
          "View rotation replaced or mis-signed the serialized brush angle");
}
QJsonArray bounds(const QRect &r) {
  return {r.x(), r.y(), r.width(), r.height()};
}
QJsonObject compare(const QImage &base, const QImage &reference,
                    const QImage &candidate, const QString &diffPath) {
  const int w = base.width(), h = base.height();
  require(reference.size() == base.size() && candidate.size() == base.size(),
          "Reference dimensions differ; resizing is prohibited");
  std::vector<unsigned char> region(size_t(w) * h, 0);
  QRect refBounds, candidateBounds;
  QRect refAlphaBounds, candidateAlphaBounds;
  qint64 alphaIntersection = 0, alphaUnion = 0;
  qint64 visibleIntersection = 0, visibleUnion = 0;
  bool transparentBase = true;
  qint64 refCount = 0, candidateCount = 0, intersection = 0, unionCount = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      transparentBase = transparentBase && qAlpha(base.pixel(x, y)) == 0;
      const bool ra = qAlpha(reference.pixel(x, y)) > 0;
      const bool ca = qAlpha(candidate.pixel(x, y)) > 0;
      if (ra)
        refAlphaBounds |= QRect(x, y, 1, 1);
      if (ca)
        candidateAlphaBounds |= QRect(x, y, 1, 1);
      alphaIntersection += ra && ca;
      alphaUnion += ra || ca;
      bool r = reference.pixel(x, y) != base.pixel(x, y);
      bool c = candidate.pixel(x, y) != base.pixel(x, y);
      // Hidden RGB at zero alpha is not a visible change, including on a
      // partially transparent background. Keep the raw union for error
      // sampling.
      const bool rv =
          r && (qAlpha(reference.pixel(x, y)) || qAlpha(base.pixel(x, y)));
      const bool cv =
          c && (qAlpha(candidate.pixel(x, y)) || qAlpha(base.pixel(x, y)));
      visibleIntersection += rv && cv;
      visibleUnion += rv || cv;
      if (r) {
        ++refCount;
        refBounds |= QRect(x, y, 1, 1);
      }
      if (c) {
        ++candidateCount;
        candidateBounds |= QRect(x, y, 1, 1);
      }
      if (r && c)
        ++intersection;
      if (!r && !c)
        continue;
      ++unionCount;
      for (int yy = std::max(0, y - 2); yy <= std::min(h - 1, y + 2); ++yy)
        for (int xx = std::max(0, x - 2); xx <= std::min(w - 1, x + 2); ++xx)
          region[size_t(yy) * w + xx] = 1;
    }
  double rgbError = 0, alphaError = 0, maxRGB = 0, maxAlpha = 0;
  std::vector<double> alphaErrors;
  std::vector<double> perceptual[2];
  double perceptualSum[2] = {0, 0};
  QImage diff(w, h, QImage::Format_ARGB32);
  diff.fill(qRgb(0, 0, 0));
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      Pixel r = pixel(reference.pixel(x, y)), c = pixel(candidate.pixel(x, y));
      double er = std::abs(r.r - c.r), eg = std::abs(r.g - c.g),
             eb = std::abs(r.b - c.b),
             ea = std::abs(qAlpha(reference.pixel(x, y)) -
                           qAlpha(candidate.pixel(x, y))) / 255.0;
      diff.setPixel(x, y,
                    qRgb(int(std::min(1.0, er + ea) * 255 + .5),
                         int(std::min(1.0, eg + ea) * 255 + .5),
                         int(std::min(1.0, eb + ea) * 255 + .5)));
      if (!region[size_t(y) * w + x])
        continue;
      rgbError += (er + eg + eb) / 3;
      alphaError += ea;
      maxRGB = std::max(maxRGB, std::max(er, std::max(eg, eb)));
      maxAlpha = std::max(maxAlpha, ea);
      alphaErrors.push_back(ea);
      for (int background = 0; background <= 1; ++background) {
        auto lab = [background](Pixel p) {
          double paper = background * (1 - p.a);
          return BrushMetrics::srgbLab(p.r + paper, p.g + paper, p.b + paper);
        };
        double difference = BrushMetrics::deltaE00(lab(r), lab(c));
        perceptual[background].push_back(difference);
        perceptualSum[background] += difference;
      }
    }
  require(diff.save(diffPath), "Cannot save full-canvas difference");
  std::sort(alphaErrors.begin(), alphaErrors.end());
  for (auto &errors : perceptual)
    std::sort(errors.begin(), errors.end());
  const double n = double(alphaErrors.size());
  return {
      {"region", "union of changed sets, square dilation radius 2"},
      {"regionPixels", n},
      {"referenceBounds", bounds(refBounds)},
      {"candidateBounds", bounds(candidateBounds)},
      {"referenceAlphaBounds", bounds(refAlphaBounds)},
      {"candidateAlphaBounds", bounds(candidateAlphaBounds)},
      {"transparentBase", transparentBase},
      {"alphaCoverageIoU",
       alphaUnion ? double(alphaIntersection) / alphaUnion : 1},
      {"referenceChangedPixels", double(refCount)},
      {"candidateChangedPixels", double(candidateCount)},
      {"changedPixelIoU", unionCount ? double(intersection) / unionCount : 1},
      {"visibleChangedPixelIoU",
       visibleUnion ? double(visibleIntersection) / visibleUnion : 1},
      {"premultipliedRGBMAE", n ? rgbError / n : 0},
      {"alphaMAE", n ? alphaError / n : 0},
      {"maxRGBError", maxRGB},
      {"maxAlphaError", maxAlpha},
      {"alphaP99", n ? alphaErrors[size_t(std::ceil(.99 * n)) - 1] : 0},
      {"deltaE00BlackMean", n ? perceptualSum[0] / n : 0},
      {"deltaE00WhiteMean", n ? perceptualSum[1] / n : 0},
      {"deltaE00BlackP95",
       n ? perceptual[0][size_t(std::ceil(.95 * n)) - 1] : 0},
      {"deltaE00WhiteP95",
       n ? perceptual[1][size_t(std::ceil(.95 * n)) - 1] : 0},
      {"emptyCandidateFailure",
       transparentBase
           ? !refAlphaBounds.isEmpty() && candidateAlphaBounds.isEmpty()
           : refCount > 0 && candidateCount == 0},
      {"bothEmpty", transparentBase ? refAlphaBounds.isEmpty() &&
                                          candidateAlphaBounds.isEmpty()
                                    : refCount == 0 && candidateCount == 0}};
}
constexpr char spacingAdmissionError[] =
    "KPP spacing is outside the RCP1 supported range 0.01-5";

bool validateSourceAdmission(const QString &group, int index,
                             const QJsonObject &source) {
  if (group != "spacingdynamics") {
    require(!source.contains("sourceAdmission"),
            "Source admission metadata is not allowed for this fixture group");
    return false;
  }
  const QString name = QString("GZ Original dry sensor spacingdynamics %1")
                           .arg(index, 3, 10, QLatin1Char('0'));
  const int spacing = index < 16 ? 1 : index < 64 ? 4 : 8;
  const int plan = index < 16 ? index / 4 : (index - 16) / 4;
  const QStringList pressures = {"quarter", "half", "full", "ramp"};
  const QStringList directions = {"east", "north", "diagonal"};
  require(index >= 0 && index < 112,
          "Source admission fixture index is outside its bounded matrix");
  const QString label = index < 16 ? pressures[plan]
      : "sparse " + QString::number(spacing) + " " + directions[(plan % 12) / 4] +
            " " + pressures[plan % 4];
  const auto sensor = source["sensorProbe"].toObject();
  require(source["preset"] == QJsonValue(name) &&
              source["spacing"] == QJsonValue(spacing) &&
              source["diameter"] == QJsonValue(24) &&
              sensor["target"] == QJsonValue(QStringLiteral("Spacing")) &&
              sensor["sensor"] == QJsonValue(QStringLiteral("pressure")) &&
              sensor["enabled"] == QJsonValue(index % 4 >= 2) &&
              sensor["isotropic"] == QJsonValue(index % 2 == 1) &&
              sensor["label"] == QJsonValue(label),
          "Source admission inputs differ from the authored spacing matrix: " + name);
  if (spacing != 8) {
    require(!source.contains("sourceAdmission"),
            "An admitted source spacing case declares expected rejection: " + name);
    return false;
  }
  const QJsonObject admission = {
      {"version", 1}, {"outcome", "expected-rejection"},
      {"reasonCode", "kpp-spacing-outside-rcp1"}, {"field", "spacing"},
      {"authoredValue", 8}, {"minimum", .01}, {"maximum", 5}};
  require(source["sourceAdmission"].isObject() &&
              source["sourceAdmission"].toObject() == admission,
          "Missing or forged outside-envelope source admission: " + name);
  return true;
}

bool generatedControlGroup(const QString &group) {
  return group == "generated-transfer" || group == "generated-geometry" ||
         group == "generated-sensors";
}
QJsonArray generatedControlXml(const QString &xml, const QString &context) {
  require(xml.size() <= 65536 && !xml.contains("<!"),
          "Unsafe generated control XML: " + context);
  QXmlStreamReader reader(xml);
  QJsonArray events;
  int depth = 0;
  while (!reader.atEnd()) {
    const auto token = reader.readNext();
    if (token == QXmlStreamReader::StartElement) {
      require(++depth <= 32 && events.size() < 4096,
              "Generated control XML exceeds structural bounds: " + context);
      require(reader.namespaceUri().isEmpty() && reader.namespaceDeclarations().isEmpty(),
              "Source settings XML namespace declarations are not authored: " + context);
      QJsonObject attributes;
      for (const auto &attribute : reader.attributes()) {
        require(attribute.namespaceUri().isEmpty(), "Source settings XML attribute namespace changed: " + context);
        attributes[attribute.name().toString()] = attribute.value().toString();
      }
      events.append(QJsonArray{"start", reader.namespaceUri().toString(),
                               reader.name().toString(), attributes});
    } else if (token == QXmlStreamReader::EndElement) {
      --depth;
      events.append(QJsonArray{"end"});
    } else if (token == QXmlStreamReader::Characters) {
      const auto text = reader.text().toString().trimmed();
      if (!text.isEmpty()) events.append(QJsonArray{"text", text});
    } else {
      require(token == QXmlStreamReader::StartDocument ||
                  token == QXmlStreamReader::EndDocument,
              "Unexpected generated control XML token: " + context);
    }
    require(events.size() <= 4096, "Generated control XML event budget: " + context);
  }
  require(!reader.hasError() && depth == 0 && !events.empty(),
          "Malformed generated control XML: " + context);
  return events;
}
QJsonObject sourceEffectivePresetXml(const QByteArray &data, const QString &name, const QString &context) {
  require(!data.isEmpty() && data.size() <= 1024 * 1024,
          "Missing or oversized actual source effective XML: " + context);
  QXmlStreamReader reader(data); QJsonObject fields;
  int roots = 0, depth = 0;
  while (!reader.atEnd()) {
    const auto token = reader.readNext();
    if (token == QXmlStreamReader::StartElement) {
      require(reader.namespaceUri().isEmpty() && reader.namespaceDeclarations().isEmpty(),
              "Source effective XML namespace declaration changed: " + context);
      for (const auto &attribute : reader.attributes())
        require(attribute.namespaceUri().isEmpty(), "Source effective XML attribute namespace changed: " + context);
      if (depth == 0) {
        require(++roots == 1 && reader.name() == QStringLiteral("Preset") &&
                    reader.attributes().value("name") == name &&
                    reader.attributes().value("paintopid") == QStringLiteral("paintbrush"),
                "Source effective preset identity changed: " + context);
        depth = 1;
      } else {
        require(depth == 1 && reader.name() == QStringLiteral("param"),
                "Unexpected actual source effective XML child: " + context);
        const QString key = reader.attributes().value("name").toString();
        require(!key.isEmpty() && key.size() <= 256 && !fields.contains(key) && fields.size() < 2048,
                "Invalid or duplicate source effective parameter: " + context);
        QString text; bool closed = false;
        while (!reader.atEnd()) {
          const auto body = reader.readNext();
          if (body == QXmlStreamReader::Characters) {
            text += reader.text();
            require(text.size() <= 65536, "Oversized source effective parameter: " + context);
          } else if (body == QXmlStreamReader::EndElement && reader.name() == QStringLiteral("param")) {
            closed = true; break;
          } else require(false, "Unexpected token inside actual source effective parameter: " + context);
        }
        require(closed && !reader.hasError(), "Incomplete actual source effective parameter: " + context);
        fields[key] = text;
      }
    } else if (token == QXmlStreamReader::EndElement) {
      require(depth == 1 && reader.name() == QStringLiteral("Preset"),
              "Actual source effective XML depth changed: " + context);
      depth = 0;
    } else if (token == QXmlStreamReader::Characters) {
      require(reader.isWhitespace(), "Actual source effective XML contains undeclared text: " + context);
    } else require(token == QXmlStreamReader::StartDocument || token == QXmlStreamReader::EndDocument,
                   "Unexpected actual source effective XML token: " + context);
  }
  require(!reader.hasError() && roots == 1 && depth == 0 && !fields.isEmpty(),
          "Malformed actual source effective preset XML: " + context);
  return fields;
}
QJsonObject retainedSourceEffectiveFields(const QDir &root, const QJsonObject &source,
                                          const QString &name, const QString &context) {
  const QString filename = QString(name).replace(' ', '_') + ".effective.xml";
  require(source.value("effectivePresetXml") == QJsonValue(filename),
          "Actual source effective preset filename changed: " + context);
  QFile file(root.filePath("output/" + filename));
  require(file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= 1024 * 1024,
          "Missing bounded actual source effective preset: " + context);
  const auto bytes = file.read(1024 * 1024 + 1);
  require(file.atEnd() && file.error() == QFile::NoError && bytes.size() <= 1024 * 1024 &&
              source.value("effectivePresetSha256") == QJsonValue(QString::fromLatin1(
                  QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())),
          "Actual source effective preset digest changed: " + context);
  return sourceEffectivePresetXml(bytes, name, context);
}
bool sourceBrushesEqual(const QString &observed, const QString &expected,
                         const QString &context, const QString &resourceMd5 = {}) {
  const auto actual = generatedControlXml(observed, context), wanted = generatedControlXml(expected, context);
  if (actual.size() != wanted.size()) return false;
  const QStringList numeric = resourceMd5.isEmpty()
      ? QStringList{"spacing", "angle", "randomness", "density", "diameter", "ratio", "hfade", "vfade", "spikes", "antialiasEdges"}
      : QStringList{"scale", "spacing", "angle", "autoSpacingCoeff"};
  auto curve = [&](const QString &text) {
    auto rows = text.split(';');
    if (!rows.empty() && rows.last().isEmpty()) rows.removeLast();
    require(rows.size() >= 2 && rows.size() <= 1024, "Source generated curve cardinality changed: " + context);
    QJsonArray result;
    for (const auto &row : rows) {
      const auto pair = row.split(',');
      require(pair.size() == 2, "Source generated curve knot changed: " + context);
      QJsonArray point;
      for (const auto &coordinate : pair) {
        bool ok = false; const double value = coordinate.toDouble(&ok);
        require(ok && std::isfinite(value), "Source generated curve numeric value changed: " + context);
        point.append(value);
      }
      result.append(point);
    }
    return result;
  };
  for (int i = 0; i < actual.size(); ++i) {
    const auto a = actual[i].toArray(), b = wanted[i].toArray();
    if (a.size() != b.size()) return false;
    if (a.size() != 4 || a[0] != QJsonValue("start")) {
      if (a != b) return false;
      continue;
    }
    if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) return false;
    auto aa = a[3].toObject(); const auto bb = b[3].toObject();
    if (!resourceMd5.isEmpty() && aa.contains("md5sum")) {
      if (aa.value("md5sum") != QJsonValue(resourceMd5)) return false;
      aa.remove("md5sum");
    }
    if (aa.keys() != bb.keys()) return false;
    for (auto entry = bb.begin(); entry != bb.end(); ++entry) {
      const QString value = aa.value(entry.key()).toString(), target = entry.value().toString();
      if (numeric.contains(entry.key())) {
        bool ok = false, targetOk = false;
        const double first = value.toDouble(&ok), second = target.toDouble(&targetOk);
        if (!ok || !targetOk || !std::isfinite(first) || !std::isfinite(second) ||
            std::abs(first - second) > (resourceMd5.isEmpty() ? 1e-7 : 0)) return false;
      } else if (entry.key() == "softness_curve") {
        if (curve(value) != curve(target)) return false;
      } else if (value != target) return false;
    }
  }
  return true;
}

void testClosedSourceXml(const QDir &root) {
  const QString name = "closed original";
  const QByteArray xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<Preset name=\"closed original\" paintopid=\"paintbrush\">\n"
      "<param name=\"MirrorSensor\"><![CDATA[<params id=\"pressure\"><curve>0,0;1,1;</curve></params>]]></param>\n</Preset>";
  const auto fields = sourceEffectivePresetXml(xml, name, "closed XML regression");
  require(fields.value("MirrorSensor") == QJsonValue("<params id=\"pressure\"><curve>0,0;1,1;</curve></params>"),
          "Actual source XML declaration or Preset CDATA was rejected or altered");
  for (const auto &defect : QStringList{"namespace", "comment", "pi", "param-pi", "param-comment", "dtd", "tail"}) {
    auto changed = xml;
    if (defect == "namespace") changed.replace("<Preset ", "<Preset xmlns:unused=\"urn:unsupported\" ");
    else if (defect == "comment") changed.replace("<Preset ", "<!--unsupported--><Preset ");
    else if (defect == "pi") changed.replace("<Preset ", "<?unsupported payload?><Preset ");
    else if (defect == "param-pi") changed.replace("</param>", "<?unsupported payload?></param>");
    else if (defect == "param-comment") changed.replace("</param>", "<!--unsupported--></param>");
    else if (defect == "dtd") changed.replace("<Preset ", "<!DOCTYPE Preset [<!ENTITY x \"unsupported\">]><Preset ");
    else changed.replace("</param>", "</param>unsupported");
    bool rejected = false;
    try { sourceEffectivePresetXml(changed, name, "closed XML negative regression"); }
    catch (const std::runtime_error &) { rejected = true; }
    require(rejected, "Actual source XML accepted undeclared content: " + defect);
  }
  const QString sensor = "<params id=\"pressure\"><curve>0,0;1,1;</curve></params>";
  for (const auto &changed : QStringList{
      "<params xmlns:unused=\"urn:unsupported\" id=\"pressure\"><curve>0,0;1,1;</curve></params>",
      "<params id=\"pressure\"><curve>0,0;1,1;</curve>unsupported</params>",
      "<!--unsupported-->" + sensor, "<?unsupported payload?>" + sensor}) {
    bool rejected = false;
    try { rejected = generatedControlXml(changed, "inner negative") != generatedControlXml(sensor, "inner expected"); }
    catch (const std::runtime_error &) { rejected = true; }
    require(rejected, "Source curve accepted undeclared namespace, text, comment or PI");
  }
  require(sourceBrushesEqual(
              "<Brush spacing=\"1.0\"><MaskGenerator softness_curve=\"0.0,0.0;1.0,1.0;\" /></Brush>",
              "<Brush spacing=\"1\"><MaskGenerator softness_curve=\"0,0;1,1;\"/></Brush>", "known numeric curve") &&
              !sourceBrushesEqual("<Brush spacing=\"1\">unsupported</Brush>",
                                  "<Brush spacing=\"1\"/>", "unknown brush text"),
          "Source brush lost known numeric/curve formatting or accepted unknown text");
  require(root.mkdir("output"), "Cannot create actual-source binding regression directory");
  const QString filename = "closed_original.effective.xml";
  QFile file(root.filePath("output/" + filename));
  require(file.open(QIODevice::WriteOnly) && file.write(xml) == xml.size(),
          "Cannot retain actual-source XML regression file");
  file.close();
  QJsonObject captured{{"effectivePresetXml", filename}, {"effectivePresetSha256",
      QString::fromLatin1(QCryptographicHash::hash(xml, QCryptographicHash::Sha256).toHex())}};
  require(retainedSourceEffectiveFields(root, captured, name, "actual file binding") == fields,
          "Actual retained source file did not reproduce its bound fields");
  require(file.open(QIODevice::WriteOnly) && file.write(xml + ' ') == xml.size() + 1,
          "Cannot alter actual-source binding regression file");
  file.close();
  bool rejected = false;
  try { retainedSourceEffectiveFields(root, captured, name, "changed actual file"); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "Copied source evidence excused a changed actual preset file");
}

bool generatedTransferExpectedNoop(const QString &group, const QJsonObject &source) {
  if (group != "generated-transfer") return false;
  const auto options = source.value("overrides").toObject();
  // Original source observations establish inactive FlowValue in build-up.
  // A zero scalar is a no-op only for an explicitly authored static Wash row.
  const bool zeroWashFlow = source.value("flow") == QJsonValue(0) &&
                            options.value("PaintOpAction") == QJsonValue("0");
  require(source.value("expectUnchanged") == QJsonValue(zeroWashFlow),
          "Generated transfer no-op expectation must match explicit static zero-flow Wash");
  if (zeroWashFlow) {
    bool scalarOk = false;
    const double effectiveScalar = options.value("FlowValue").toString().toDouble(&scalarOk);
    require(scalarOk && effectiveScalar == 0 && source.value("delivery") == QJsonValue("Node.paintLine") &&
                source.value("background") == QJsonValue("transparent") &&
                !source.contains("sensorProbe") && !source.contains("timedProbe") &&
                (!options.contains("PressureFlow") || options.value("PressureFlow") == QJsonValue("false")) &&
                source.value("generatedControlProbe").toObject().value("objective") == QJsonValue("static flow 0"),
            "Generated zero-flow Wash exception requires the authored inactive-curve Node control");
  }
  return zeroWashFlow;
}

bool generatedTransferRasterMatchesExpectation(bool expectedNoop, const QJsonObject &metrics) {
  if (expectedNoop)
    return metrics.value("transparentBase") == QJsonValue(true) &&
           metrics.value("bothEmpty") == QJsonValue(true) &&
           metrics.value("referenceChangedPixels") == QJsonValue(0) &&
           metrics.value("candidateChangedPixels") == QJsonValue(0);
  return metrics.value("bothEmpty") == QJsonValue(false) &&
         metrics.value("referenceChangedPixels").toDouble() > 0 &&
         metrics.value("candidateChangedPixels").toDouble() > 0 &&
         metrics.value("emptyCandidateFailure") == QJsonValue(false);
}

void validateGeneratedControlSource(const QDir &root, const QString &group,
                                    const QJsonObject &source,
                                    const QJsonObject &authored,
                                    const QString &context) {
  for (auto item = authored.begin(); item != authored.end(); ++item)
    require(source.value(item.key()) == item.value(),
            "Generated source case changed after acquisition: " + context + "/" + item.key());
  const bool candidate = group == "generated-transfer";
  if (candidate) generatedTransferExpectedNoop(group, source);
  const auto evidence = source.value("generatedControlEvidence").toObject();
  require(evidence.value("version") == QJsonValue(1) &&
              evidence.value("implementationStatus") == QJsonValue(candidate
                  ? "shared-pixel-transfer-candidate" : "open-domain-acquisition-only") &&
              evidence.value("candidateExpected") == QJsonValue(candidate) &&
              evidence.value("sourceReferenceVerified") == QJsonValue(false) &&
              evidence.value("sourceSemanticallySupported") == QJsonValue(false) &&
              evidence.value("sourceParityQualified") == QJsonValue(false) &&
              evidence.value("deliveryInterface") == authored.value("delivery") &&
              evidence.value("deliveryInterface") == source.value("deliveryReport").toObject().value("mode"),
          "Missing generated control delivery/status evidence: " + context);
  require(evidence.value("effectiveControls").isObject(),
          "Missing generated effective controls: " + context);
  const auto controls = evidence.value("effectiveControls").toObject();
  const auto expected = authored.value("overrides").toObject();
  const auto fields = retainedSourceEffectiveFields(root, source, authored.value("preset").toString(), context);
  require(sourceBrushesEqual(fields.value("brush_definition").toString(), expected.value("brush_definition").toString(), context) &&
              source.value("generatedTipEvidence").toObject().value("brushDefinition") == fields.value("brush_definition"),
          "Generated mask evidence is not bound to actual retained source brush XML: " + context);
  int expectedCount = 0;
  for (auto item = expected.begin(); item != expected.end(); ++item) {
    if (item.key() == "brush_definition") continue;
    ++expectedCount;
    const auto actual = controls.value(item.key());
    require(actual.isString() && actual.toString().size() <= 65536 && item.value().isString() &&
                fields.value(item.key()) == actual,
            "Generated effective control differs from actual retained XML: " + context + "/" + item.key());
    if (item.key().endsWith("Value") || item.key() == "PaintOpAction") {
      bool okActual = false, okExpected = false;
      const double first = actual.toString().toDouble(&okActual),
                   second = item.value().toString().toDouble(&okExpected);
      require(okActual && okExpected && std::isfinite(first) && std::isfinite(second) &&
                  std::abs(first - second) <= 1e-7,
              "Generated numeric control differs: " + context + "/" + item.key());
    } else if (item.key().endsWith("Sensor")) {
      require(generatedControlXml(actual.toString(), context) ==
                  generatedControlXml(item.value().toString(), context),
              "Generated sensor control differs: " + context + "/" + item.key());
    } else {
      require(actual == item.value(),
              "Generated control differs: " + context + "/" + item.key());
    }
  }
  require(controls.size() == expectedCount,
          "Generated effective controls contain undeclared entries: " + context);
}

#include "scatter_holdout_reference.h"

void validateAcquiredSourceAdmission(const QJsonObject &source,
                                     const QJsonObject &authored,
                                     const QString &context) {
  // A non-const JSON subscript inserts null for an absent field. Absence is
  // meaningful here: ordinary cases must not acquire rejection metadata.
  require(source.contains("sourceAdmission") == authored.contains("sourceAdmission") &&
              source.value("sourceAdmission") == authored.value("sourceAdmission"),
          "Source admission changed after acquisition: " + context);
}

void testSourceAdmissionReadback() {
  QJsonObject source = QJsonDocument::fromJson("{\"preset\":\"original\"}").object();
  const auto authored = source;
  validateAcquiredSourceAdmission(source, authored, "ordinary captured case");
  require(source == authored && !source.contains("sourceAdmission"),
          "Admission readback inserted metadata into an ordinary case");
  QJsonObject nullMetadata = source;
  nullMetadata["sourceAdmission"] = QJsonValue(QJsonValue::Null);
  for (bool changedSource : {false, true}) {
    bool rejected = false;
    try {
      validateAcquiredSourceAdmission(changedSource ? nullMetadata : source,
                                      changedSource ? authored : nullMetadata,
                                      "forged null metadata");
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    require(rejected, "Admission readback equated missing and null metadata");
  }
  source["sourceAdmission"] = QJsonObject{{"version", 1}, {"authoredValue", 8}};
  const auto captured = QJsonDocument::fromJson(QJsonDocument(source).toJson()).object();
  validateAcquiredSourceAdmission(captured, source, "rejection captured case");
  auto altered = captured;
  altered["sourceAdmission"] = QJsonObject{{"version", 1}, {"authoredValue", 4}};
  bool rejected = false;
  try {
    validateAcquiredSourceAdmission(altered, source, "changed rejection input");
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  require(rejected, "Admission readback accepted changed rejection inputs");
}

QJsonObject sourceRejectionOutcome(const QJsonObject &source,
                                   const ImportResult &imported,
                                   const QString &error,
                                   bool acquisitionInputsValidated) {
  require(!imported.complete && imported.records.empty() &&
              error == QString::fromLatin1(spacingAdmissionError),
          "Expected exact source-envelope rejection for " +
              source["preset"].toString() + "; actual complete=" +
              QString::number(imported.complete) + " records=" +
              QString::number(int(imported.records.size())) + " reason=" + error);
  QJsonObject outcome;
  // No candidate raster exists. Null metrics cannot be mistaken for zero
  // error, matching coverage, an empty stroke or measured runtime.
  for (const QString &field : {
           QString("region"), QString("regionPixels"), QString("referenceBounds"),
           QString("candidateBounds"), QString("referenceAlphaBounds"),
           QString("candidateAlphaBounds"), QString("transparentBase"),
           QString("alphaCoverageIoU"), QString("referenceChangedPixels"),
           QString("candidateChangedPixels"), QString("changedPixelIoU"),
           QString("visibleChangedPixelIoU"), QString("premultipliedRGBMAE"),
           QString("alphaMAE"), QString("maxRGBError"), QString("maxAlphaError"),
           QString("alphaP99"), QString("deltaE00BlackMean"),
           QString("deltaE00WhiteMean"), QString("deltaE00BlackP95"),
           QString("deltaE00WhiteP95"), QString("emptyCandidateFailure"),
           QString("bothEmpty"), QString("boundsError"), QString("candidateProfile"),
           QString("candidateSeed"), QString("renderMilliseconds"), QString("dabs")})
    outcome[field] = QJsonValue(QJsonValue::Null);
  outcome["preset"] = source["preset"];
  outcome["inputCase"] = source;
  outcome["comparisonStatus"] = "expected-rejection";
  outcome["sourceAdmission"] = source["sourceAdmission"];
  outcome["admissionReason"] = error;
  outcome["candidateImportComplete"] = imported.complete;
  outcome["candidateImportRecords"] = int(imported.records.size());
  outcome["sourceSemanticallySupported"] = false;
  outcome["sourceReferenceVerified"] = false;
  outcome["sourceParityQualified"] = false;
  outcome["sensorSourceParityQualified"] = false;
  outcome["sensorAcquisitionInputsValidated"] = acquisitionInputsValidated;
  outcome["diagnostics"] = QJsonArray{error};
  return outcome;
}

QJsonObject gihCorrelationPlan(const QString &configuration) {
  const QStringList configurations = {"line-exact", "line-tail", "path-tail",
      "dabs-tail", "halves-tail", "west-tail", "north-tail", "translated-tail", "corners-tail"};
  const auto phaseMatch = QRegularExpression("^phase-([xy])-g(002|004|008|016|032|064|128)-(coarse|dabs)$").match(configuration);
  require(configurations.contains(configuration) || phaseMatch.hasMatch(), "Unknown bounded GIH correlation configuration");
  const double offset = phaseMatch.hasMatch() ? 1. / (2 * phaseMatch.captured(2).toInt()) : 0;
  QJsonValue phaseBoundary(QJsonValue::Null);
  if (phaseMatch.hasMatch()) phaseBoundary = QJsonObject{
      {"axis", phaseMatch.captured(1)}, {"gridBoundary", phaseMatch.captured(2).toInt()},
      {"offset", offset}, {"partition", phaseMatch.captured(3)}, {"sourceGridEstablished", false}};
  QJsonArray centers;
  int left = 2048, top = 2048, right = 0, bottom = 0;
  for (int i = 0; i < 32; ++i) {
    double x = 256 + i * 32, y = 1024;
    if (phaseMatch.hasMatch()) {
      if (phaseMatch.captured(1) == "x") x += offset;
      else y += offset;
    }
    if (configuration == "west-tail") x = 256 + (31 - i) * 32;
    else if (configuration == "north-tail") { x = 1024; y = 1536 - i * 32; }
    else if (configuration == "translated-tail") { x += 13; y += 19; }
    else if (configuration == "corners-tail") { x = 512 + ((i + 1) / 2) * 32; y = 512 + (i / 2) * 32; }
    centers.append(QJsonArray{x, y});
    left = std::min(left, int(std::floor(x)) - 8); top = std::min(top, int(std::floor(y)) - 8);
    right = std::max(right, int(std::ceil(x)) + 9); bottom = std::max(bottom, int(std::ceil(y)) + 9);
  }
  const int continuation = configuration == "line-exact" ? 0 : 1;
  const auto previous = centers[30].toArray(), last = centers[31].toArray();
  const QJsonArray tail{last[0].toDouble() + (last[0].toDouble() - previous[0].toDouble()) / 32 * continuation,
                        last[1].toDouble() + (last[1].toDouble() - previous[1].toDouble()) / 32 * continuation};
  QJsonArray vertices{centers[0], tail};
  if (configuration == "dabs-tail" || configuration == "corners-tail" ||
      (phaseMatch.hasMatch() && phaseMatch.captured(3) == "dabs")) {
    vertices = centers; vertices.append(tail);
  } else if (configuration == "halves-tail") {
    vertices = {};
    for (int i = 0; i <= 62; ++i) vertices.append(QJsonArray{256 + i * 16, 1024});
    vertices.append(tail);
  }
  return {{"configuration", configuration}, {"centers", centers}, {"vertices", vertices},
          {"mode", configuration.startsWith("line-") ? "Node.paintLine" : "Node.paintPath"},
          {"terminalContinuation", continuation}, {"rect", QJsonArray{left, top, right - left, bottom - top}},
          {"phaseBoundary", phaseBoundary}};
}
QJsonObject validateGihCorrelationCase(const QJsonObject &source, const QString &group) {
  require(group.startsWith("gih-correlation-"), "GIH correlation case outside focused matrix");
  const QString configuration = group.mid(QString("gih-correlation-").size());
  const auto plan = gihCorrelationPlan(configuration);
  const auto spec = source["gihCorrelationProbe"].toObject(), resource = source["resourceProbe"].toObject();
  const bool random = resource["kind"].toString() == "random";
  const QString kind = random ? "random" : "incremental";
  const int count = random ? 100 : 2;
  const double replicate = spec["replicate"].toDouble(-1);
  const auto vertices = plan["vertices"].toArray();
  const QString context = group + "/" + source["preset"].toString();
  require(plan["phaseBoundary"].isObject()
              ? source["phaseBoundaryProbe"] == plan["phaseBoundary"]
              : !source.contains("phaseBoundaryProbe"),
          "GIH fractional phase heldout identity or unqualified-state flag differs: " + context);
  require(spec["replicate"].isDouble() && std::isfinite(replicate) && replicate >= 0 &&
              replicate < count && replicate == std::floor(replicate),
          "Invalid GIH correlation replicate: " + context);
  const QString suffix = QString("%1").arg(int(replicate), 3, 10, QLatin1Char('0'));
  require(source["preset"].toString() == "GZ Original dry " + group + " " + kind + " " + suffix &&
              source["candidatePackage"].toString() == "output/resource-packages/gih-correlation-" + kind + "-" + suffix + ".bundle",
          "GIH correlation independent preset/package identity differs: " + context);
  require(resource["kind"].toString() == kind && spec["configuration"] == QJsonValue(configuration) &&
              spec["stochastic"] == QJsonValue(random) && spec["replicates"] == QJsonValue(count) &&
              spec["slots"] == QJsonValue(32) && spec["terminalContinuation"] == plan["terminalContinuation"] &&
              spec["replicate"].isDouble() && replicate >= 0 && replicate < count && replicate == std::floor(replicate) &&
              resource["replicate"] == spec["replicate"] && resource["expectedSlots"] == QJsonValue(32) &&
              source["sourcePath"].toArray() == vertices && source["correlationCenters"] == plan["centers"] &&
              source["start"] == vertices[0] && source["end"] == vertices.last() && source["delivery"] == plan["mode"] &&
              source["variant"].toString() == (random ? "random acquisition" : "baseline") &&
              source["sourceEngine"] == QJsonValue(QStringLiteral("paintbrush")) &&
              source["background"] == QJsonValue(QStringLiteral("transparent")) &&
              source["diameter"] == QJsonValue(8) && source["spacing"] == QJsonValue(4) &&
              source["pressure"].toArray() == QJsonArray{1, 1} && source["opacity"] == QJsonValue(1) &&
              source["flow"] == QJsonValue(1) && source["brushRotationDegrees"] == QJsonValue(0) &&
              source["sourcePressureFlag"] == QJsonValue(false) &&
              source["foregroundRGBA8"].toArray() == QJsonArray{173, 91, 47, 255} &&
              resource["filename"].toString() == "gz-original-" + kind + ".gih" &&
              resource["type"] == QJsonValue(QStringLiteral("gbr_brush")) &&
              resource["originalDimensions"].toArray() == QJsonArray{8, 8} && resource["rect"] == plan["rect"] &&
              resource["bytes"] == QJsonValue(random ? 1057 : 1067) &&
              resource["sha256"].toString() == (random
                  ? "d5c1b2ac401d1021cb214df3c338b61ea7bff5754f370ff83a31fc24fef24913"
                  : "5cbaff0356727e33710a5569ae8b2dc69b41924f0be78091d1ad464bc970734b") &&
              resource["md5"].toString() == (random ? "0e8c9a8ba59d70531adb13f092e7ca6e" : "6cf1224fec489a17eaa51347159f8984"),
          "GIH correlation authored contract/resource differs: " + context);
  return plan;
}
void validateGihCorrelationSource(const QDir &root, const QJsonObject &source, const QJsonObject &authored,
                                  const QString &group, const QString &context) {
  const auto plan = validateGihCorrelationCase(source, group);
  for (auto item = authored.constBegin(); item != authored.constEnd(); ++item)
    require(source.value(item.key()) == item.value(), "GIH source changed authored field " + item.key() + ": " + context);
  const auto evidence = source["gihCorrelationEvidence"].toObject();
  const auto delivery = source["deliveryReport"].toObject(), resource = source["resourceProbe"].toObject();
  const auto resolution = source["resourceResolution"].toObject();
  const auto fields = retainedSourceEffectiveFields(root, source, authored.value("preset").toString(), context);
  auto requested = authored.value("overrides").toObject();
  const auto brushDefinition = requested.take("brush_definition").toString();
  for (const auto &entry : std::initializer_list<std::pair<QString, QString>>{
      {"paintop", "paintbrush"}, {"CompositeOp", "normal"}, {"EraserMode", "false"},
      {"PaintOpAction", "1"}, {"OpacityUseCurve", "false"}, {"FlowUseCurve", "false"},
      {"PressureRotation", "false"}, {"PressureRatio", "false"}, {"PressureScatter", "false"}, {"PressureSpacing", "false"},
      {"Texture/Pattern/Enabled", "false"}, {"MergedPaint", "false"},
      {"PaintOpSettings/isAirbrushing", "false"}, {"PaintOpSettings/ignoreSpacing", "false"},
      {"PaintOpSettings/updateSpacingBetweenDabs", "false"}, {"KisPrecisionOption/AutoPrecisionEnabled", "false"},
      {"KisPrecisionOption/precisionLevel", "5"}, {"OpacityValue", "1"}, {"FlowValue", "1"}})
    requested[entry.first] = entry.second;
  const auto controls = evidence.value("effectiveControls").toObject();
  require(controls.size() == requested.size() &&
              evidence.value("effectiveBrushDefinition") == fields.value("brush_definition") &&
              sourceBrushesEqual(fields.value("brush_definition").toString(), brushDefinition,
                                 context, resource.value("md5").toString()),
          "GIH correlation actual retained brush/control set changed: " + context);
  for (auto field = requested.begin(); field != requested.end(); ++field) {
    const auto actual = fields.value(field.key());
    require(actual.isString() && controls.value(field.key()) == actual,
            "GIH correlation effective control is not bound to actual XML: " + context + "/" + field.key());
    if (field.key() == "MirrorSensor")
      require(generatedControlXml(actual.toString(), context) == generatedControlXml(field.value().toString(), context),
              "GIH correlation actual Mirror sensor changed: " + context);
    else if (field.key() == "OpacityValue" || field.key() == "FlowValue") {
      bool ok = false; const double value = actual.toString().toDouble(&ok);
      require(ok && value == 1, "GIH correlation actual transfer scalar changed: " + context);
    } else require(actual == field.value(), "GIH correlation actual control changed: " + context + "/" + field.key());
  }
  require(evidence["configuration"] == plan["configuration"] && evidence["rect"] == plan["rect"] &&
              evidence["replicate"] == source["gihCorrelationProbe"].toObject()["replicate"] &&
              evidence["stochastic"] == source["gihCorrelationProbe"].toObject()["stochastic"] &&
              evidence["actualPaintedTipReadback"] == QJsonValue(true) &&
              evidence["sourceSelectionModelVerified"] == QJsonValue(false) && evidence["pixelParityClaimed"] == QJsonValue(false) &&
              evidence["phaseGridEstablished"] == QJsonValue(false) &&
              !evidence["effectiveControls"].toObject().empty() && !evidence["effectiveBrushDefinition"].toString().isEmpty() &&
              evidence["sha256OriginalFile"] == resource["sha256"] && resolution["sha256OriginalFile"] == resource["sha256"] &&
              resolution["filename"] == resource["filename"] && source["resourceReadback"].toObject() == evidence &&
              evidence["deliveredPath"].toObject() == delivery && delivery["mode"] == plan["mode"] &&
              source["input"] == plan["mode"] && delivery["physicalTabletClaimed"] == QJsonValue(false),
          "Missing exact GIH correlation source controls/resource/delivery evidence: " + context);
  if (plan["mode"].toString() == "Node.paintLine") {
    const QJsonArray events{QJsonObject{{"type", "line-start"}, {"position", source["start"]}, {"pressure", 1}},
                            QJsonObject{{"type", "line-end"}, {"position", source["end"]}, {"pressure", 1}}};
    require(delivery["events"].toArray() == events, "GIH line endpoints differ from actual delivery: " + context);
  } else require(delivery["vertices"] == plan["vertices"] && delivery["singleContinuousStroke"] == QJsonValue(true),
                 "GIH continuous path differs from actual delivery: " + context);
  const auto labels = evidence["frameSelections"].toArray();
  int missing = 0;
  require(labels.size() == 32, "Incomplete GIH correlation frame labels: " + context);
  for (const auto &label : labels) {
    if (label.isNull()) ++missing;
    else require(label.isDouble() && label.toDouble() >= 0 && label.toDouble() <= 2 &&
                     label.toDouble() == std::floor(label.toDouble()), "Invalid GIH frame label: " + context);
  }
  require(missing < 32 && evidence["missingAuthoredSlots"] == QJsonValue(missing),
          "Missing/forged GIH source label readback: " + context);
}
QJsonArray gihCorrelationLabels(const QImage &image, const QJsonArray &centers, const QString &context) {
  QJsonArray labels;
  for (const auto &value : centers) {
    const auto center = value.toArray();
    const int cx = int(std::floor(center[0].toDouble())), cy = int(std::floor(center[1].toDouble()));
    quint64 sums[3] = {0, 0, 0};
    for (int y = cy - 6; y <= cy + 6; ++y)
      for (int x = cx - 6; x <= cx + 6; ++x) {
        const auto color = image.pixel(x, y);
        sums[0] += qRed(color) * qAlpha(color); sums[1] += qGreen(color) * qAlpha(color); sums[2] += qBlue(color) * qAlpha(color);
      }
    const int winner = int(std::max_element(sums, sums + 3) - sums);
    if (!sums[winner]) labels.append(QJsonValue(QJsonValue::Null));
    else {
      for (int channel = 0; channel < 3; ++channel)
        require(channel == winner || sums[channel] == 0, "GIH correlation cell mixes frame colors: " + context);
      labels.append(winner);
    }
  }
  return labels;
}

void validateFixtureMatrix(const QJsonObject &fixtures) {
  require(fixtures["version"].toString() == "5.3.4.1",
          "Fixture source version changed");
  const auto expected = fixtures["presets"].toArray();
  const auto cases = fixtures["cases"].toArray();
  require(!expected.empty() && expected.size() == cases.size(),
          "Incomplete fixture preset/case manifest");
  QStringList names;
  for (int i = 0; i < expected.size(); ++i) {
    const QString name = expected[i].toString();
    require(!name.isEmpty() && !names.contains(name) &&
                cases[i].toObject()["preset"].toString() == name,
            "Duplicate or mismatched fixture preset identity");
    names.append(name);
  }
  const QString group = fixtures["group"].toString();
  if (group.startsWith("scatterholdout-")) {
    require(isScatterHoldoutGroup(group) && expected.size() == 102 &&
                fixtures["license"] == QJsonValue("CC0-1.0") &&
                fixtures["stochasticSamplePolicy"] == QJsonValue(
                    "100 independent enabled strokes; two independently repeated disabled controls; randomized execution bound to canonical identities") &&
                fixtures["width"] == QJsonValue(2048) && fixtures["height"] == QJsonValue(2048) &&
                fixtures["randomizeExecution"] == QJsonValue(true) &&
                fixtures["developmentAcquisition"] == QJsonValue(true) &&
                fixtures["candidateExpected"] == QJsonValue(scatterHoldoutCandidate(group)) &&
                fixtures["implementationStatus"].toString() ==
                    (scatterHoldoutCandidate(group) ? "measured-static-scatter-candidate"
                                                    : "open-scatter-domain-acquisition-only") &&
                fixtures["sourceProfileVerified"] == QJsonValue(false) &&
                fixtures["pixelParityClaimed"] == QJsonValue(false),
            "Incomplete or falsely qualified focused scatter fixture matrix");
    for (int index = 0; index < cases.size(); ++index)
      validateScatterHoldoutCase(group, index, cases[index].toObject());
  } else {
    for (const auto &value : cases)
      require(!value.toObject().contains("scatterHoldoutProbe"),
              "Focused scatter controls cannot grant an unrelated matrix a repeat exemption");
  }
  for (int i = 0; i < cases.size(); ++i)
    validateSourceAdmission(group, i, cases[i].toObject());
  if (group == "autospacing")
    require(expected.size() == 192, "Incomplete automatic spacing fixture matrix");
  if (group.startsWith("scatter-")) {
    const QStringList groups = {"scatter-x-east", "scatter-x-north",
        "scatter-y-east", "scatter-y-north", "scatter-xy-east", "scatter-xy-north"};
    require(groups.contains(group) && expected.size() == 102,
            "Incomplete independent scatter fixture matrix");
    std::set<int> active, controls;
    for (const auto &value : cases) {
      const auto spec = value.toObject()["scatterProbe"].toObject();
      require(spec["enabled"].isBool() && spec["replicate"].isDouble() &&
                  spec["replicate"].toDouble() == spec["replicate"].toInt(),
              "Invalid scatter replicate declaration");
      const int replicate = spec["replicate"].toInt();
      if (spec["enabled"].toBool())
        require(replicate >= 0 && replicate < 100 && active.insert(replicate).second,
                "Duplicate or missing active scatter replicate");
      else
        require(replicate >= 0 && replicate < 2 && controls.insert(replicate).second,
                "Duplicate or missing deterministic scatter control");
    }
    require(active.size() == 100 && controls.size() == 2,
            "Incomplete scatter sample/control policy");
  }
  if (group.startsWith("gih-correlation-")) {
    require(expected.size() == 102 && fixtures["width"] == QJsonValue(2048) &&
                fixtures["height"] == QJsonValue(2048) && fixtures["randomizeExecution"] == QJsonValue(true),
            "Incomplete bounded GIH correlation fixture matrix");
    std::set<int> random, controls;
    for (const auto &value : cases) {
      const auto source = value.toObject();
      validateGihCorrelationCase(source, group);
      const int replicate = source["gihCorrelationProbe"].toObject()["replicate"].toInt();
      auto &observed = source["resourceProbe"].toObject()["kind"].toString() == "random" ? random : controls;
      require(observed.insert(replicate).second, "Duplicate GIH correlation replicate");
    }
    require(random.size() == 100 && controls.size() == 2, "Incomplete independent GIH correlation strokes/controls");
  }
  if (group == "wetgeometry")
    require(expected.size() == 30, "Incomplete wet geometry diagnostic matrix");
  if (group == "generated-transfer" || group == "generated-geometry" ||
      group == "generated-sensors") {
    const int count = group == "generated-transfer" ? 216
                      : group == "generated-geometry" ? 52 : 84;
    require(expected.size() == count, "Incomplete generated control fixture matrix");
    const bool candidate = group == "generated-transfer";
    for (int i = 0; i < cases.size(); ++i) {
      const auto source = cases[i].toObject();
      if (candidate) generatedTransferExpectedNoop(group, source);
      const auto declaration = source["generatedControlProbe"].toObject();
      const QString name = "GZ Original " + group + " " +
          QString("%1").arg(i, 3, 10, QLatin1Char('0'));
      require(source["preset"] == QJsonValue(name) &&
                  declaration["candidateExpected"] == QJsonValue(candidate) &&
                  declaration["sourceParityQualified"] == QJsonValue(false) &&
                  declaration["scope"].toString() ==
                      (candidate ? "shared Pixel transfer holdout" : "open generated domain obligation") &&
                  declaration["objective"].isString() &&
                  !declaration["objective"].toString().isEmpty() &&
                  declaration["objective"].toString().size() <= 128 &&
                  !source.contains("sourceAdmission"),
              "Forged generated control identity or implementation declaration");
    }
  }
  if (fixtures["group"].toString() == "generatedcurves")
    require(expected.size() == 124, "Incomplete generated curve diagnostic matrix");
  if (fixtures["group"].toString() == "generatedtips")
    require(expected.size() == 32, "Incomplete generated tip fixture matrix");
  if (fixtures["group"].toString() == "tabletsensors")
    require(expected.size() == 188,
            "Incomplete tablet sensor fixture matrix");
  if (fixtures["group"].toString() == "spacingdynamics")
    require(expected.size() == 112,
            "Incomplete spacing sensor fixture matrix");
  if (fixtures["group"].toString() == "mirrortips")
    require(expected.size() == 70, "Incomplete mirror fixture matrix");
  if (fixtures["group"].toString() == "lab-conversion")
    require(expected.size() == 1, "Incomplete public Lab dry control");
  if (fixtures["group"].toString() == "wethsv")
    require(expected.size() == 144, "Incomplete wet HSV acquisition matrix");
  if (fixtures["group"].toString() == "hsvpressure")
    require(expected.size() == 24, "Incomplete pressure HSV heldout matrix");
  if (fixtures["group"].toString() == "hsvchroma")
    require(expected.size() == 36,
            "Incomplete low-chroma HSV heldout matrix");
  if (fixtures["group"].toString() == "hsvorder")
    require(expected.size() == 24,
            "Incomplete order-discriminating HSV matrix");
  if (fixtures["group"].toString() == "hsvjoint")
    require(expected.size() == 64, "Incomplete joint HSV acquisition matrix");
  if (fixtures["group"].toString() == "hsvheldout")
    require(expected.size() == 54,
            "Incomplete heldout HSV acquisition matrix");
  if (fixtures["group"].toString() == "hsvdynamics")
    require(expected.size() == 72, "Incomplete HSV acquisition matrix");
  if (fixtures["group"].toString() == "distancesensors")
    require(expected.size() == 32, "Incomplete distance acquisition matrix");
  if (fixtures["group"].toString() == "gihcadence")
    require(expected.size() == 168,
            "Incomplete GIH cadence acquisition matrix");
  if (fixtures["group"].toString() == "maskwash" ||
      fixtures["group"].toString() == "maskbuildup")
    require(expected.size() == 156,
            "Incomplete masked-brush acquisition matrix");
  if (fixtures["group"].toString() == "airbrushtimed")
    require(expected.size() == 56, "Incomplete paced airbrush workload");
  if (fixtures["group"].toString() == "timedsensors")
    require(expected.size() == 40, "Incomplete paced sensor workload");
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    if (app.arguments().size() == 2 &&
        app.arguments().at(1) == "--test-metrics") {
      testViewRotation();
      testSourceAdmissionReadback();
      QTemporaryDir directory;
      require(directory.isValid(), "Cannot create metric fixture directory");
      testClosedSourceXml(QDir(directory.path()));
      QImage base(2, 2, QImage::Format_ARGB32);
      base.fill(Qt::transparent);
      QImage reference = base, candidate = base;
      const QJsonObject zeroFlowCase{{"flow", 0}, {"expectUnchanged", true},
          {"delivery", "Node.paintLine"}, {"background", "transparent"},
          {"overrides", QJsonObject{{"FlowValue", "0"}, {"PaintOpAction", "0"}}},
          {"generatedControlProbe", QJsonObject{{"objective", "static flow 0"}}}};
      require(generatedTransferExpectedNoop("generated-transfer", zeroFlowCase),
              "Explicit static zero-flow Wash control lost its no-op declaration");
      auto zeroBuildUpCase = zeroFlowCase;
      zeroBuildUpCase["overrides"] = QJsonObject{{"FlowValue", "0"}, {"PaintOpAction", "1"}};
      zeroBuildUpCase["expectUnchanged"] = false;
      require(!generatedTransferExpectedNoop("generated-transfer", zeroBuildUpCase),
              "Measured build-up FlowValue0 was incorrectly classified as a no-op");
      for (bool dynamic : {false, true}) {
        auto forged = zeroFlowCase;
        if (dynamic) forged["sensorProbe"] = QJsonObject{{"target", "Flow"}};
        else forged["expectUnchanged"] = false;
        bool rejected = false;
        try { generatedTransferExpectedNoop("generated-transfer", forged); }
        catch (const std::runtime_error &) { rejected = true; }
        require(rejected, "Generated no-op exception accepted undeclared or dynamic zero flow");
      }
      // Real decoded/candidate images participate in explicit Wash no-op
      // comparisons; missing images still fail ordinary decoding. Build-up
      // FlowValue0 belongs to the ordinary nonempty comparison below.
      const auto noPaint = compare(base, reference, candidate, directory.filePath("generated-zero-flow.png"));
      require(generatedTransferRasterMatchesExpectation(true, noPaint) &&
                  !generatedTransferRasterMatchesExpectation(false, noPaint),
              "Generated zero-flow Wash lost its exact no-op gate or excused empty build-up paint");
      candidate.setPixel(0, 0, qRgba(0, 0, 255, 1));
      const auto strayNoop = compare(base, reference, candidate, directory.filePath("generated-zero-flow-stray.png"));
      require(!generatedTransferRasterMatchesExpectation(true, strayNoop),
              "Generated zero flow excused unexpected candidate paint");
      bool missingSourceRejected = false;
      try { compare(base, QImage(), base, directory.filePath("generated-zero-flow-missing.png")); }
      catch (const std::runtime_error &) { missingSourceRejected = true; }
      require(missingSourceRejected, "Generated no-op comparison fabricated metrics for a missing source image");
      candidate = base;
      reference.setPixel(0, 0, qRgba(255, 0, 0, 0));
      reference.setPixel(1, 0, qRgba(0, 0, 255, 255));
      candidate.setPixel(1, 0, qRgba(0, 0, 255, 255));
      auto measured = compare(base, reference, candidate,
                              directory.filePath("hidden-rgb.png"));
      require(measured["alphaCoverageIoU"].toDouble() == 1 &&
                  measured["changedPixelIoU"].toDouble() == .5 &&
                  measured["visibleChangedPixelIoU"].toDouble() == 1 &&
                  measured["maxRGBError"].toDouble() == 0 &&
                  measured["maxAlphaError"].toDouble() == 0,
              "Undefined transparent RGB was counted as painted coverage");
      candidate.setPixel(0, 1, qRgba(255, 0, 0, 255));
      measured = compare(base, reference, candidate,
                         directory.filePath("stray-visible.png"));
      require(measured["alphaCoverageIoU"].toDouble() == .5 &&
                  measured["maxAlphaError"].toDouble() == 1,
              "Stray visible candidate pixels escaped the union comparison");
      base.setPixel(0, 1, qRgba(255, 0, 0, 128));
      reference = base;
      candidate = base;
      reference.setPixel(0, 0, qRgba(255, 0, 0, 0));
      reference.setPixel(1, 0, qRgba(0, 0, 255, 96));
      candidate.setPixel(1, 0, qRgba(0, 0, 255, 96));
      measured = compare(base, reference, candidate,
                         directory.filePath("mixed-alpha-hidden-rgb.png"));
      require(!measured["transparentBase"].toBool() &&
                  measured["changedPixelIoU"].toDouble() == .5 &&
                  measured["visibleChangedPixelIoU"].toDouble() == 1 &&
                  measured["maxRGBError"].toDouble() == 0 &&
                  measured["maxAlphaError"].toDouble() == 0,
              "Hidden RGB contaminated mixed-background coverage");
      // RGBA8 alpha differences are integer code differences. Subtracting
      // normalized alpha first can put exactly one byte above 1/255 at some
      // code values and below it at others.
      QImage alphaBase(255, 1, QImage::Format_ARGB32);
      alphaBase.fill(Qt::transparent);
      QImage alphaReference = alphaBase, alphaCandidate = alphaBase;
      for (int value = 0; value < 255; ++value) {
        alphaReference.setPixel(value, 0, qRgba(0, 0, 0, value));
        alphaCandidate.setPixel(value, 0, qRgba(0, 0, 0, value + 1));
      }
      measured = compare(alphaBase, alphaReference, alphaCandidate,
                         directory.filePath("one-byte-alpha.png"));
      require(measured["maxAlphaError"].toDouble() == 1 / 255.0 &&
                  measured["alphaP99"].toDouble() == 1 / 255.0,
              "One-byte alpha threshold depends on the alpha code value");
      alphaCandidate.setPixel(100, 0, qRgba(0, 0, 0, 102));
      measured = compare(alphaBase, alphaReference, alphaCandidate,
                         directory.filePath("two-byte-alpha.png"));
      require(measured["maxAlphaError"].toDouble() == 2 / 255.0,
              "Two-byte alpha error was rounded into the one-byte gate");
      // Published numerical pairs, including neutral and hue-wrap boundaries.
      // https://www.ece.rochester.edu/~gsharma/ciede2000/
      const double pairs[][7] = {
          {50, 2.6772, -79.7751, 50, 0, -82.7485, 2.0425},
          {50, 3.1571, -77.2803, 50, 0, -82.7485, 2.8615},
          {50, 0, 0, 50, -1, 2, 2.3669},
          {50, 2.49, -.001, 50, -2.49, .0009, 7.1792},
          {50, 2.49, -.001, 50, -2.49, .0011, 7.2195},
          {50, -.001, 2.49, 50, .0011, -2.49, 4.7461}};
      for (const auto &p : pairs) {
        BrushMetrics::Lab a{p[0], p[1], p[2]}, b{p[3], p[4], p[5]};
        require(std::abs(BrushMetrics::deltaE00(a, b) - p[6]) < .0001 &&
                    std::abs(BrushMetrics::deltaE00(b, a) - p[6]) < .0001,
                "CIEDE2000 calibration failed");
        require(BrushMetrics::deltaE00(a, a) == 0,
                "Identical colors have nonzero difference");
      }
      auto black = BrushMetrics::srgbLab(0, 0, 0);
      auto white = BrushMetrics::srgbLab(1, 1, 1);
      require(std::abs(black.l) < 1e-8 && std::abs(white.l - 100) < 1e-4 &&
                  std::abs(white.a) < 1e-4 && std::abs(white.b) < 1e-4,
              "sRGB/D65 neutral calibration failed");
      return 0;
    }
    const bool fixturesOnly = argc == 3 &&
        app.arguments().at(1) == "--validate-fixtures";
    const bool admissionOnly = argc == 3 &&
        app.arguments().at(1) == "--validate-source-admission";
    require(argc == 2 || fixturesOnly || admissionOnly,
            "Expected reference run directory");
    QDir root(app.arguments().at(fixturesOnly || admissionOnly ? 2 : 1));
    if (fixturesOnly || admissionOnly) {
      QFile manifest(root.filePath("output/fixtures.json"));
      require(manifest.open(QIODevice::ReadOnly), "Fixture manifest missing");
      const auto fixtures = QJsonDocument::fromJson(manifest.readAll()).object();
      validateFixtureMatrix(fixtures);
      if (fixturesOnly)
        std::cout << "Fixture matrix accepted before source acquisition.\n";
      else {
        require(fixtures["group"].toString() == "spacingdynamics",
                "Source admission import regression requires its spacing matrix");
        QJsonArray rejections;
        const auto cases = fixtures["cases"].toArray();
        for (int i = 0; i < cases.size(); ++i) {
          const auto source = cases[i].toObject();
          if (!validateSourceAdmission("spacingdynamics", i, source)) continue;
          QString error;
          const auto imported = importBrushFile(root.filePath(
              "data/krita/paintoppresets/" + source["preset"].toString() + ".kpp"), error);
          rejections.append(sourceRejectionOutcome(source, imported, error, false));
        }
        require(rejections.size() == 48, "Incomplete source admission import regression");
        std::cout << QJsonDocument(QJsonObject{{"records", rejections}})
                         .toJson(QJsonDocument::Compact).constData() << '\n';
      }
      return 0;
    }
    QFile status(root.filePath("output/oracle-status.json"));
    require(status.open(QIODevice::ReadOnly),
            "Source reference status missing");
    auto oracle = QJsonDocument::fromJson(status.readAll()).object();
    require(oracle["status"].toString() == "complete" &&
                oracle["version"].toString() == "5.3.4.1",
            "Source reference incomplete or wrong version");
    QJsonArray results;
    std::set<int> stochasticReplicates;
    QStringList failedHardCases;
    QFile manifest(root.filePath("output/fixtures.json"));
    require(manifest.open(QIODevice::ReadOnly), "Fixture manifest missing");
    auto fixtures = QJsonDocument::fromJson(manifest.readAll()).object();
    if (fixtures["group"].toString() == "gihstatistics" ||
        fixtures["group"].toString() == "gihlabels") {
      const auto statistics = oracle["resourceStatistics"].toObject();
      require(statistics["strokes"].toInt() == 100 &&
                  statistics["sourceSequences"].toArray().size() == 100 &&
                  statistics["observedDabs"].toInt() > 0 &&
                  statistics["seedParityClaimed"].isBool() &&
                  !statistics["seedParityClaimed"].toBool(),
              "Incomplete or mislabeled random GIH acquisition");
    }
    auto expected = fixtures["presets"].toArray();
    require(!expected.empty(), "Empty fixture manifest");
    validateFixtureMatrix(fixtures);
    if (fixtures["group"].toString().startsWith("gih-correlation-")) {
      const auto planned = oracle["plannedExecutionOrder"].toArray();
      const auto rows = oracle["records"].toArray();
      require(oracle["intendedPresetOrder"].toArray() == expected && planned.size() == expected.size() &&
                  oracle["actualExecutionOrder"].toArray() == planned && planned != expected && rows.size() == expected.size(),
              "Missing actual randomized GIH correlation execution report");
      std::set<QString> names;
      for (int index = 0; index < planned.size(); ++index) {
        const QString name = planned[index].toString();
        require(!name.isEmpty() && expected.contains(name) && names.insert(name).second,
                "GIH correlation planned execution contains a missing/duplicate stroke");
        const auto row = rows[index].toObject();
        int ordinal = -1;
        for (int delivered = 0; delivered < planned.size(); ++delivered)
          if (planned[delivered] == row["preset"]) ordinal = delivered;
        require(row["preset"] == expected[index] && row["intendedOrdinal"] == QJsonValue(index) &&
                    row["executionOrdinal"] == QJsonValue(ordinal),
                "GIH correlation actual execution ordinal differs from its canonical stroke identity");
      }
    }
    if (isScatterHoldoutGroup(fixtures["group"].toString())) {
      const QString group = fixtures["group"].toString();
      const auto planned = oracle["plannedExecutionOrder"].toArray();
      const auto actual = oracle["actualExecutionOrder"].toArray();
      const auto rows = oracle["records"].toArray();
      require(expected.size() == 102 && oracle["intendedPresetOrder"].toArray() == expected &&
                  planned.size() == 102 && actual == planned && rows.size() == 102,
              "Missing complete focused scatter intended/planned/actual execution binding");
      std::set<QString> deliveredNames;
      QJsonArray enabledOrder, canonicalEnabledOrder;
      for (int index = 0; index < 100; ++index) canonicalEnabledOrder.append(expected[index]);
      for (int index = 0; index < planned.size(); ++index) {
        const QString name = planned[index].toString();
        int authoredIndex = -1;
        for (int candidate = 0; candidate < expected.size(); ++candidate)
          if (expected[candidate] == planned[index]) authoredIndex = candidate;
        require(authoredIndex >= 0 && deliveredNames.insert(name).second,
                "Focused scatter execution has an unknown or repeated stroke identity");
        if (authoredIndex < 100) enabledOrder.append(planned[index]);
        const auto row = rows[index].toObject();
        int ordinal = -1;
        for (int delivered = 0; delivered < planned.size(); ++delivered)
          if (planned[delivered] == expected[index]) ordinal = delivered;
        require(row["preset"] == expected[index] && row["intendedOrdinal"] == QJsonValue(index) &&
                    row["executionOrdinal"] == QJsonValue(ordinal),
                "Focused scatter records were not restored to their canonical source identities");
      }
      require(enabledOrder.size() == 100 && enabledOrder != canonicalEnabledOrder,
              "Focused scatter enabled strokes were not randomized independently of canonical reporting");
      if (!scatterHoldoutCandidate(group)) {
        require(!QFile::exists(root.filePath("output/comparison.json")),
                "Pending scatter source-only acquisition contains a native comparison");
        const auto cases = fixtures["cases"].toArray();
        for (int index = 0; index < rows.size(); ++index)
          validateScatterHoldoutSource(root, group, rows[index].toObject(), cases[index].toObject(),
                                       group + "/" + expected[index].toString());
        const QJsonObject pending{{"status", "open-scatter-domain-acquisition-only"}, {"group", group},
            {"sourceCases", 102}, {"independentEnabledStrokes", 100},
            {"independentlyRepeatedDisabledControls", 2}, {"developmentAcquisition", true},
            {"candidateImplemented", false}, {"candidateCompared", false},
            {"sourceReferenceVerified", false}, {"sourceSemanticallySupported", false},
            {"sourceParityQualified", false}, {"sourceRngLawIdentified", false},
            {"pixelParityClaimed", false}, {"physicalTabletClaimed", false},
            {"remainingWork", "not-admitted-scaling-or-dynamics"}};
        QFile pendingFile(root.filePath("output/open-scatter-domain.json"));
        require(pendingFile.open(QIODevice::WriteOnly), "Cannot retain pending scatter source-only report");
        const QByteArray encodedPending = QJsonDocument(pending).toJson(QJsonDocument::Indented);
        require(pendingFile.write(encodedPending) == encodedPending.size(),
                "Cannot write complete pending scatter source-only report");
        return 0;
      }
    }
    QStringList remaining;
    for (const auto &name : expected)
      remaining.append(name.toString());
    const QStringList declaredNames = remaining;
    int expectedRejections = 0;
    QImage initial(2048, 2048, QImage::Format_ARGB32);
    const QRgb colors[] = {qRgba(0, 0, 255, 255), qRgba(0, 255, 0, 128),
                           qRgba(255, 0, 0, 255), qRgba(0, 0, 0, 0)};
    for (int y = 0; y < initial.height(); ++y)
      for (int x = 0; x < initial.width(); ++x)
        initial.setPixel(x, y, colors[(x / 128) % 4]);
    require(initial.save(root.filePath("output/initial.png")),
            "Cannot save initial raster");
    const QImage stripInitial = initial;
    for (const auto &value : oracle["records"].toArray()) {
      auto source = value.toObject();
      const QString caseName = source["preset"].toString();
      const QString caseContext = fixtures["group"].toString() + "/" + caseName;
      const int fixtureIndex = declaredNames.indexOf(caseName);
      require(fixtureIndex >= 0, "Unexpected source case: " + caseContext);
      const bool expectedRejection = validateSourceAdmission(
          fixtures["group"].toString(), fixtureIndex, source);
      const auto authoredCase = fixtures["cases"].toArray()[fixtureIndex].toObject();
      if (isScatterHoldoutGroup(fixtures["group"].toString()))
        validateScatterHoldoutSource(root, fixtures["group"].toString(), source,
                                     authoredCase, caseContext);
      validateAcquiredSourceAdmission(source, authoredCase, caseContext);
      if (generatedControlGroup(fixtures["group"].toString()))
        validateGeneratedControlSource(root, fixtures["group"].toString(), source,
                                       authoredCase, caseContext);
      if (fixtures["group"].toString().startsWith("gih-correlation-"))
        validateGihCorrelationSource(root, source, authoredCase, fixtures["group"].toString(), caseContext);
      if (fixtures["group"].toString() == "hsvjoint" ||
          fixtures["group"].toString() == "hsvorder" ||
          fixtures["group"].toString() == "hsvchroma") {
        const auto evidence = source["sensorEvidence"].toObject();
        require(evidence["deliveryValuesMatchAuthored"].toBool() &&
                    evidence["deliveryInterface"].toString() ==
                        "Node.paintLine" &&
                    evidence["deliveredContactEvents"].toArray().size() == 2 &&
                    source["jointHsvProbe"]
                        .toObject()["operationOrderAssumed"]
                        .isBool() &&
                    !source["jointHsvProbe"]
                         .toObject()["operationOrderAssumed"]
                         .toBool(),
                "Missing independent joint HSV acquisition evidence");
        const auto controls = evidence["effectiveControls"].toObject();
        const auto requested = source["overrides"].toObject();
        for (const QString &channel :
             {QString("h"), QString("s"), QString("v")})
          for (const QString &suffix :
               {QString("UseCurve"), QString("UseSameCurve"), QString("Value"),
                QString("curveMode"), QString("commonCurve")})
            require(controls[channel + suffix] == requested[channel + suffix],
                    "Joint HSV control readback differs");
        const auto foreground =
            evidence["effectiveForegroundDisplayRGBA8"].toArray();
        const auto authored = source["foregroundRGBA8"].toArray();
        require(foreground.size() == 4 && authored.size() == 4,
                "Missing joint HSV foreground readback");
        for (int c = 0; c < 4; ++c)
          require(std::abs(foreground[c].toInt() - authored[c].toInt()) <= 1,
                  "Joint HSV foreground readback differs");
      }
      if (fixtures["group"].toString() == "hsvpressure") {
        const auto evidence = source["sensorEvidence"].toObject();
        const auto controls = evidence["effectiveControls"].toObject();
        const QString target =
            source["sensorProbe"].toObject()["target"].toString();
        require((target == "h" || target == "s" || target == "v") &&
                    evidence["deliveryValuesMatchAuthored"].toBool() &&
                    evidence["deliveryInterface"].toString() ==
                        "Node.paintLine" &&
                    controls[target + "commonCurve"].toString() == "0,0;1,1;",
                "Missing dynamic HSV source input/readback");
        const auto foreground =
            evidence["effectiveForegroundDisplayRGBA8"].toArray();
        const auto authored = source["foregroundRGBA8"].toArray();
        require(foreground.size() == 4 && authored.size() == 4,
                "Missing dynamic HSV foreground readback");
        for (int c = 0; c < 4; ++c)
          require(std::abs(foreground[c].toInt() - authored[c].toInt()) <= 1,
                  "Dynamic HSV foreground differs");
      }
      const bool wetHsvCase = fixtures["group"].toString() == "wethsv";
      if (wetHsvCase) {
        const auto evidence = source["sensorEvidence"].toObject();
        const auto controls = evidence["effectiveControls"].toObject();
        const auto spec = source["wetHsvProbe"].toObject();
        const auto requested = source["overrides"].toObject();
        require(evidence["deliveryValuesMatchAuthored"].toBool() &&
                    evidence["deliveryInterface"].toString() ==
                        "Node.paintLine" &&
                    evidence["deliveredContactEvents"].toArray().size() == 2 &&
                    spec["colorRate"].toDouble() == .25 &&
                    requested["ColorRateValue"].toString() == "0.25",
                "Missing wet HSV input/control evidence");
        const QString target = spec["channel"].toString();
        require(target == "h" || target == "s" || target == "v",
                "Invalid wet HSV channel");
        for (const QString &channel :
             {QString("h"), QString("s"), QString("v")})
          require(controls["Pressure" + channel] ==
                      requested["Pressure" + channel],
                  "Wet HSV enabled state differs");
        require(controls[target + "commonCurve"] ==
                    requested[target + "commonCurve"],
                "Wet HSV fixed curve differs");
        if (spec.contains("composition"))
          for (const QString &channel : {QString("h"), QString("s"), QString("v")})
            require(controls[channel + "commonCurve"] == requested[channel + "commonCurve"] &&
                        controls[channel + "Sensor"] == requested[channel + "Sensor"],
                    "Wet HSV composition control differs");
        const auto foreground =
            evidence["effectiveForegroundDisplayRGBA8"].toArray();
        const auto authored = source["foregroundRGBA8"].toArray();
        require(foreground.size() == 4 && authored.size() == 4,
                "Missing wet HSV foreground readback");
        for (int c = 0; c < 4; ++c)
          require(std::abs(foreground[c].toInt() - authored[c].toInt()) <= 1,
                  "Wet HSV foreground readback differs");
      }
      const bool maskCase = fixtures["group"].toString() == "maskwash" ||
                            fixtures["group"].toString() == "maskbuildup";
      if (maskCase) {
        const auto evidence = source["maskEvidence"].toObject();
        require(
            evidence["nestedMetadataVerified"].toBool() &&
                evidence["deliveryVerified"].toBool() &&
                !evidence["effectiveControls"].toObject().empty() &&
                evidence["externalResourceDependencies"].isArray() &&
                evidence["externalResourceDependencies"].toArray().empty() &&
                evidence["sourceParityQualified"].isBool() &&
                !evidence["sourceParityQualified"].toBool(),
            "Missing qualified nested masking metadata or line evidence");
        const auto controls = evidence["effectiveControls"].toObject();
        const auto requested = source["overrides"].toObject();
        for (auto it = requested.begin(); it != requested.end(); ++it)
          if (!it.key().endsWith("brush_definition"))
            require(controls[it.key()] == it.value(),
                    "Masked acquisition effective setting differs");
        require(!evidence["data"].toString().isEmpty() &&
                    evidence["rect"].toArray().size() == 4,
                "Missing masked acquisition region");
        if (source["maskProbe"].toObject()["mode"].toString() == "off")
          require(evidence["nonzeroAlphaPixels"].toInt() > 0,
                  "Unmasked paired source control did not paint");
      }
      const bool sensorCase =
          fixtures["group"].toString() == "tabletsensors" ||
          fixtures["group"].toString() == "spacingdynamics" ||
          fixtures["group"].toString() == "mirrortips" ||
          fixtures["group"].toString() == "hsvdynamics" ||
          fixtures["group"].toString() == "hsvheldout" ||
          fixtures["group"].toString() == "distancesensors" ||
          (fixtures["group"].toString() == "generated-transfer" &&
           source.value("sensorProbe").isObject());
      QJsonArray deliveredSensorEvents;
      if (sensorCase) {
        const auto evidence = source["sensorEvidence"].toObject();
        require(evidence["deliveryValuesMatchAuthored"].isBool() &&
                    evidence["deliveryValuesMatchAuthored"].toBool() &&
                    evidence["effectiveControls"].isObject() &&
                    !evidence["effectiveControls"].toObject().empty(),
                "Missing effective sensor controls or delivered values");
        if (fixtures["group"].toString() == "mirrortips") {
          const auto controls = evidence["effectiveControls"].toObject();
          const auto mirror = source["mirrorProbe"].toObject();
          require(evidence["mirrorAxisValuesVerified"].toBool() &&
                      controls["HorizontalMirrorEnabled"].toString() ==
                          (mirror["horizontal"].toBool() ? "true" : "false") &&
                      controls["VerticalMirrorEnabled"].toString() ==
                          (mirror["vertical"].toBool() ? "true" : "false"),
                  "Missing effective Mirror-axis evidence");
        }
        if (fixtures["group"].toString() == "hsvdynamics" ||
            fixtures["group"].toString() == "hsvheldout" ||
            fixtures["group"].toString() == "distancesensors") {
          const auto foreground =
              evidence["effectiveForegroundDisplayRGBA8"].toArray();
          const auto requested = source["foregroundRGBA8"].toArray();
          require(foreground.size() == 4 && requested.size() == 4,
                  "Missing source foreground readback");
          for (int c = 0; c < 4; ++c)
            require(std::abs(foreground[c].toInt() - requested[c].toInt()) <= 1,
                    "Source foreground readback differs");
        }
        const auto authored = source["events"].toArray();
        deliveredSensorEvents = evidence["deliveredContactEvents"].toArray();
        require(authored.size() == 33 &&
                    deliveredSensorEvents.size() == authored.size(),
                "Incomplete sensor contact trace");
        const double timeBase =
            deliveredSensorEvents[0].toObject()["timeMs"].toDouble();
        for (int i = 0; i < authored.size(); ++i) {
          const auto wanted = authored[i].toObject();
          const auto actual = deliveredSensorEvents[i].toObject();
          const auto image = actual["image"].toArray(),
                     tilt = actual["tilt"].toArray();
          require(image.size() == 2 && tilt.size() == 2,
                  "Incomplete delivered tablet vector");
          for (const auto &pair :
               {std::make_pair(image[0].toDouble(), wanted["x"].toDouble()),
                std::make_pair(image[1].toDouble(), wanted["y"].toDouble()),
                std::make_pair(tilt[0].toDouble(), wanted["tiltX"].toDouble()),
                std::make_pair(tilt[1].toDouble(), wanted["tiltY"].toDouble()),
                std::make_pair(actual["pressure"].toDouble(),
                               wanted["pressure"].toDouble()),
                std::make_pair(actual["rotation"].toDouble(),
                               wanted["rotation"].toDouble())})
            require(std::isfinite(pair.first) &&
                        std::abs(pair.first - pair.second) <= 1e-7,
                    "Candidate/source sensor inputs differ");
          require(
              std::abs((actual["timeMs"].toDouble() - timeBase) -
                       wanted["timeUs"].toDouble() / 1000) <= .500001,
              "Delivered sensor clock differs beyond millisecond quantization");
        }
        const auto spec = source["sensorProbe"].toObject();
        bool zeroEndpoint = spec["target"].toString() == "Ratio" &&
                            spec["sensor"].toString() == "declination" &&
                            spec["enabled"].toBool() &&
                            source["diameter"].toDouble() == 24 &&
                            source["spacing"].toDouble() == 1;
        for (const auto &value : deliveredSensorEvents) {
          const auto event = value.toObject();
          const auto tilt = event["tilt"].toArray();
          zeroEndpoint = zeroEndpoint && tilt[0].toDouble() == 60 &&
                         tilt[1].toDouble() == 0 &&
                         event["pressure"].toDouble() == 1;
        }
        require(evidence["knownZeroRatioEndpoint"].isBool() &&
                    evidence["knownZeroRatioEndpoint"].toBool() ==
                        zeroEndpoint &&
                    source["expectUnchanged"].toBool() == zeroEndpoint &&
                    (zeroEndpoint ? evidence["nonzeroAlphaPixels"].toInt() == 0
                                  : evidence["nonzeroAlphaPixels"].toInt() > 0),
                "Sensor raster disagrees with qualified zero endpoint or "
                "nonempty requirement");
      }
      initial = stripInitial;
      if (source["background"].toString() == "wet-drift")
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x) {
            const bool red = (x / 64) % 2 == 0;
            initial.setPixel(x, y, qRgba(red ? 255 : 0, 0, red ? 0 : 255,
                                        std::min(255, y * 255 / 1280)));
          }
      if (source["background"].toString() == "transparent")
        initial.fill(Qt::transparent);
      if (source["background"].toString() == "blend-stripes") {
        const QRgb swatches[] = {
            qRgba(32, 160, 224, 255),  qRgba(192, 64, 96, 128),
            qRgba(128, 128, 128, 255), qRgba(224, 192, 32, 64),
            qRgba(64, 224, 160, 255),  qRgba(160, 32, 224, 192),
            qRgba(0, 0, 0, 0),         qRgba(255, 255, 255, 255)};
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(x, y, swatches[(x / 8) % 8]);
      }
      if (source["background"].toString() == "wet-halves")
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(x, y,
                             x < source["wetBoundary"].toDouble(336)
                                 ? qRgba(0, 0, 255, 255)
                                 : qRgba(255, 0, 0, 128));
      if (source["wetPattern"].toString() == "two-pixel-red-yellow")
        for (int y = 0; y < initial.height(); ++y)
          for (int x = int(std::ceil(source["wetBoundary"].toDouble(336)));
               x < initial.width(); ++x)
            initial.setPixel(x, y,
                             (x / 2) % 2 == 0 ? qRgba(255, 0, 0, 128)
                                              : qRgba(255, 255, 0, 255));
      if (source["wetPattern"].toString() == "opaque-black-white-column")
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(x, y,
                             x >= source["wetBoundary"].toDouble(416)
                                 ? qRgba(0, 0, 0, 0)
                             : x == 256 + source["wetKernelOffset"].toInt()
                                 ? qRgba(255, 255, 255, 255)
                                 : qRgba(0, 0, 0, 255));
      if (source["wetPattern"].toString() == "opaque-black-white-row")
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(x, y,
                             x >= source["wetBoundary"].toDouble(384)
                                 ? qRgba(0, 0, 0, 0)
                             : y == 1024 + source["wetKernelOffset"].toInt()
                                 ? qRgba(255, 255, 255, 255)
                                 : qRgba(0, 0, 0, 255));
      if (source["wetPattern"].toString() == "opaque-black-white-point") {
        const int pointX = 256 + source["wetKernelOffset"].toInt(),
                  pointY = 1024 + source["wetKernelOffsetY"].toInt();
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(
                x, y,
                x >= source["wetBoundary"].toDouble() ? qRgba(0, 0, 0, 0)
                : x == pointX && y == pointY ? qRgba(255, 255, 255, 255)
                                             : qRgba(0, 0, 0, 255));
      }
      if (source["wetPattern"].toString() == "opaque-black-white-points") {
        const int base = source["wetKernelComplement"].toBool() ? 255 : 0;
        for (int y = 0; y < initial.height(); ++y)
          for (int x = 0; x < initial.width(); ++x)
            initial.setPixel(x, y,
                             x >= source["wetBoundary"].toDouble()
                                 ? qRgba(0, 0, 0, 0)
                                 : qRgba(base, base, base, 255));
        for (const auto &value : source["wetKernelPoints"].toArray()) {
          const auto point = value.toArray();
          initial.setPixel(256 + point[0].toInt(), 1024 + point[1].toInt(),
                           qRgba(255 - base, 255 - base, 255 - base, 255));
        }
      }
      if (source.contains("wetThirdBoundary")) {
        const auto c = source["wetThirdRGBA8"].toArray();
        for (int y = 0; y < initial.height(); ++y)
          for (int x = source["wetThirdBoundary"].toInt(); x < initial.width();
               ++x)
            initial.setPixel(
                x, y,
                qRgba(c[0].toInt(), c[1].toInt(), c[2].toInt(), c[3].toInt()));
      }
      QString name = caseName, error;
      require(remaining.removeOne(name), "Unexpected or duplicate source case: " + caseContext);
      const bool resourceCase =
          fixtures["group"].toString() == "bitmaptips" ||
          fixtures["group"].toString() == "gihstatistics" ||
          fixtures["group"].toString() == "gihlabels" ||
          fixtures["group"].toString() == "gihcadence" ||
          fixtures["group"].toString().startsWith("gih-correlation-") ||
          (sensorCase && source.value("resourceProbe").isObject());
      QString package = "data/krita/paintoppresets/" + name + ".kpp";
      if (resourceCase) {
        package = source["candidatePackage"].toString();
        const bool correlation = fixtures["group"].toString().startsWith("gih-correlation-");
        require(QRegularExpression(correlation
                    ? "^output/resource-packages/gih-correlation-(random|incremental)-[0-9]{3}\\.bundle$"
                    : "^output/resource-packages/resource-[0-9]{3}\\.bundle$")
                    .match(package)
                    .hasMatch(),
                "Invalid original resource package path");
        require(source["resourceResolution"].isObject() &&
                    source["resourceReadback"].isObject(),
                "Missing actual resource resolution/readback evidence");
      }
      auto imported = importBrushFile(root.filePath(package), error);
      if (expectedRejection) {
        // Acquisition controls and actual delivered tablet values were checked
        // above. Preserve the source image without inventing a candidate raster.
        QImage reference(root.filePath("output/" + source["image"].toString()));
        require(!reference.isNull() && reference.size() == initial.size(),
                "Missing retained source image for expected rejection: " + caseContext);
        results.append(sourceRejectionOutcome(source, imported, error, true));
        ++expectedRejections;
        continue;
      }
      require(error.isEmpty() && imported.complete && imported.records.size() == 1,
              "Candidate import failed [" + caseContext + "]: " + error +
                  " (complete=" + QString::number(imported.complete) +
                  ", records=" + QString::number(int(imported.records.size())) + ")");
      Preset p = imported.records.front().preset;
      const bool textureGate = fixtures["group"].toString() == "texturemasks";
      const bool sourceHsv = fixtures["group"].toString() == "hsvdynamics" ||
                             fixtures["group"].toString() == "hsvheldout" ||
                             fixtures["group"].toString() == "hsvpressure" ||
                             fixtures["group"].toString() == "hsvjoint" ||
                             fixtures["group"].toString() == "hsvorder" ||
                             fixtures["group"].toString() == "hsvchroma";
      const auto generatedProbe = source.value("generatedTipProbe").toObject();
      const QString generatedId = generatedProbe.value("id").toString();
      const bool sourceGenerated =
          p.profile == "krita-5.3.4.1-generated-mask-candidate-1";
      if (generatedId == "soft" || generatedId == "gauss")
        require(sourceGenerated,
                "Generated mask importer did not select its measured candidate: " + caseContext);
      if (sourceGenerated) {
        require(p.sourceGeneratedV1.has_value() &&
                    imported.records.front().sourceGeneratedV1.has_value(),
                "Generated mask candidate lost its active or preserved source definition: " + caseContext);
        const auto &definition = *p.sourceGeneratedV1;
        require(definition.generator != SourceGeneratedGenerator::Default,
                "Default generator was silently relabelled as the new mask model: " + caseContext);
        if (!generatedProbe.isEmpty()) {
          require(generatedId == (definition.generator == SourceGeneratedGenerator::Soft ? "soft" : "gauss") &&
                      generatedProbe.value("type").toString() ==
                          (definition.shape == SourceGeneratedShape::Circle ? "circle" : "rect"),
                  "Generated candidate changed source family or shape: " + caseContext);
          for (const auto &item : std::initializer_list<std::pair<QString, double>>{
                   {"diameter", definition.diameter}, {"ratio", definition.ratio},
                   {"hfade", definition.horizontalFade}, {"vfade", definition.verticalFade},
                   {"spikes", double(definition.spikes)}}) {
            bool numeric = false;
            const double expected = generatedProbe.value(item.first).toString().toDouble(&numeric);
            require(numeric && std::isfinite(expected) && expected == item.second,
                    "Generated candidate lost exact source " + item.first + ": " + caseContext);
          }
          require(definition.antialiasEdges.has_value() &&
                      generatedProbe.value("antialiasEdges").toString() ==
                          (*definition.antialiasEdges ? "1" : "0"),
                  "Generated candidate lost source antialias state: " + caseContext);
          if (definition.generator == SourceGeneratedGenerator::Soft) {
            const auto points = generatedProbe.value("softness_curve").toString().split(';', Qt::SkipEmptyParts);
            require(size_t(points.size()) == definition.softnessCurve.size(),
                    "Generated candidate changed source curve cardinality: " + caseContext);
            for (int i = 0; i < points.size(); ++i) {
              const auto pair = points[i].split(',');
              bool okX = false, okY = false;
              const double x = pair.value(0).toDouble(&okX), y = pair.value(1).toDouble(&okY);
              require(pair.size() == 2 && okX && okY &&
                          x == definition.softnessCurve[size_t(i)].x &&
                          y == definition.softnessCurve[size_t(i)].y,
                      "Generated candidate changed source curve knots: " + caseContext);
            }
          }
        }
      }
      const bool disabledScatterControl =
          source.value("scatterProbe").toObject().value("enabled") == QJsonValue(false) ||
          source.value("scatterHoldoutProbe").toObject().value("enabled") == QJsonValue(false);
      const bool sourceHard = !sourceGenerated && (
          disabledScatterControl ||
          fixtures["group"].toString() == "lab-conversion" || maskCase || source.value("sourceBarrelCandidate").toBool(false) ||
          sourceHsv || (textureGate && name.contains("dry legacy")) ||
          fixtures["group"].toString() == "hardpixel" ||
          fixtures["group"].toString() == "pixelopacity" ||
          fixtures["group"].toString() == "pixelopacitytablet" ||
          fixtures["group"].toString() == "pixelcontrols" ||
          fixtures["group"].toString() == "pixelpressure" ||
          fixtures["group"].toString() == "pixelsmallpressure" ||
          fixtures["group"].toString() == "pixelshape" ||
          fixtures["group"].toString() == "pixelprecision" ||
          fixtures["group"].toString() == "pixelcurves" ||
          (fixtures["group"].toString() == "pixeltransfer" &&
           source["variant"].toString().endsWith("Node control")) ||
          (fixtures["group"].toString() == "pixelcurveknots" &&
           !source["variant"].toString().contains("disabled")) ||
          (fixtures["group"].toString() == "pixelblend" &&
           p.tip.shape == Shape::Round && p.tip.hardness == 1 &&
           !p.tip.antialiasEdges) ||
          (fixtures["group"].toString() == "geometry" &&
           name.endsWith("antialias 0")));
      if (sourceHard)
        require(p.profile == "krita-5.3.4.1-hard-pixel-candidate-1",
                "Strict source hard-pixel eligibility did not select the "
                "candidate");
      if (maskCase) {
        const auto settings = source["overrides"].toObject();
        const bool active =
            fixtures["group"].toString() == "maskwash" &&
            settings["MaskingBrush/Enabled"].toString() == "true";
        require(p.sourceMasked.enabled == active &&
                    p.sourceMasked.flow ==
                        settings["MaskingBrush/Preset/FlowValue"]
                            .toString()
                            .toDouble() &&
                    p.sourceMasked.opacity ==
                        settings["MaskingBrush/Preset/OpacityValue"]
                            .toString()
                            .toDouble(),
                "Source mask importer lost separate flow/opacity or Wash "
                "eligibility");
      }
      if (sourceHsv) {
        const auto fields = source["overrides"].toObject();
        for (int channel = 0; channel < 3; ++channel) {
          const QString prefix = QString("hsv").mid(channel, 1);
          const bool enabled = fields["Pressure" + prefix].toString() == "true";
          const bool legacy =
              p.sourceHsvChannel == SourceHsvChannel(channel + 1);
          require(enabled == (legacy || p.sourceHsvV2[channel].enabled),
                  "Source HSV import lost enabled channel");
          if (!enabled)
            continue;
          const auto points = fields[prefix + "commonCurve"].toString().split(
              ';', Qt::SkipEmptyParts);
          require(points.size() == 2, "Unsupported source HSV fixture curve");
          bool ok0 = false, ok1 = false;
          const double first = points[0].section(',', 1, 1).toDouble(&ok0),
                       last = points[1].section(',', 1, 1).toDouble(&ok1);
          require(ok0 && ok1, "Invalid source HSV fixture curve");
          const auto &control = p.sourceHsvV2[channel].control;
          if (first == last)
            require(legacy ? p.sourceHsvValue == first
                           : control.mappings.empty() && control.value == first,
                    "Source HSV import lost fixed control");
          else
            require(!legacy && first == 0 && last == 1 && control.value == 1 &&
                        control.mappings.size() == 1 &&
                        control.mappings[0].sensor == Sensor::Pressure,
                    "Source HSV import lost measured pressure control");
        }
      }
      const bool sourceSoft = !sourceGenerated && (
          generatedId == "default" ||
          (fixtures["group"].toString() == "pixelblend" &&
           (p.tip.shape == Shape::Square || p.tip.hardness < 1 ||
            p.tip.antialiasEdges)) ||
          ((fixtures["group"].toString() == "pixelsoft" ||
            fixtures["group"].toString() == "pixelsoftsize" ||
            fixtures["group"].toString() == "pixelrectsize") &&
           (name.contains("soft rect ") ||
            (name.contains("soft circle ") &&
             (p.tip.hardness < 1 || p.tip.antialiasEdges)))) ||
          ((fixtures["group"].toString() == "geometry" ||
            fixtures["group"].toString() == "pixelaarange") &&
           p.tip.antialiasEdges));
      if (sourceSoft)
        require(p.profile == (p.tip.shape == Shape::Square
                                  ? "krita-5.3.4.1-default-rect-candidate-1"
                                  : "krita-5.3.4.1-default-circle-candidate-1"),
                "Strict source default-mask eligibility did not select "
                "candidate: " +
                    name);
      const bool kernelCount = fixtures["group"].toString() == "wetkernelcount";
      const double kernelDiameter = source["diameter"].toDouble();
      const double kernelRadius = source["overrides"]
                                      .toObject()["SmudgeRadiusValue"]
                                      .toString()
                                      .toDouble();
      const bool kernelCountGate =
          kernelCount && kernelDiameter == std::floor(kernelDiameter) &&
          (name.contains("dulling new") || kernelRadius <= 100);
      if (kernelCount)
        require(
            source["sourceCandidateGate"].isBool() &&
                source["sourceCandidateGate"].toBool() == kernelCountGate,
            "Kernel probe qualification metadata disagrees with eligibility");
      if (textureGate && name.contains("dulling new"))
        require(p.profile == "groundzero-native-1",
                "Unmeasured new Dulling texture gained source eligibility");
      const bool sourceWet =
          (textureGate && !name.contains("dry legacy") &&
           !name.contains("dulling new")) ||
          (fixtures["group"].toString() == "wetsteps" ||
           fixtures["group"].toString() == "wethold" ||
           fixtures["group"].toString() == "wetstate" ||
           fixtures["group"].toString() == "wetswitches" ||
           fixtures["group"].toString() == "wetcombined" ||
           fixtures["group"].toString() == "wetpressure" ||
           fixtures["group"].toString() == "wetopacitycombo" ||
           fixtures["group"].toString() == "wetboundary" || kernelCountGate ||
           fixtures["group"].toString() == "wettransfer" ||
           ((fixtures["group"].toString() == "wetkernel" ||
             fixtures["group"].toString() == "wetkernelscale" ||
             fixtures["group"].toString() == "wettexture" ||
             fixtures["group"].toString() == "wettexture3") &&
            (name.contains(" smear ") ||
             source["overrides"].toObject()["SmudgeRadiusValue"].toString() ==
                 "0" ||
             name.contains("dulling new") ||
             (name.contains("dulling legacy") &&
              source["overrides"]
                      .toObject()["SmudgeRadiusValue"]
                      .toString()
                      .toDouble() <= 100))));
      if (sourceWet) {
        const QString model =
            imported.records.front().provenance["sourceSmudgeModel"].toString();
        require(p.profile == "krita-5.3.4.1-" + model.toStdString() +
                                 "-candidate-1" &&
                    p.smudgeRadius ==
                        ((model == "legacy-dulling" || model == "new-dulling")
                             ? source["overrides"]
                                       .toObject()["SmudgeRadiusValue"]
                                       .toString()
                                       .toDouble() /
                                   100
                             : 0) &&
                    p.reload == 0,
                "Strict source wet eligibility did not select the matching "
                "candidate");
      }
      applyViewRotation(p, source);
      p.size.value = source["diameter"].toDouble();
      p.color = {220 / 255.0, 40 / 255.0, 30 / 255.0, 1};
      if (source.contains("foregroundRGBA8")) {
        auto color = source["foregroundRGBA8"].toArray();
        require(color.size() == 4, "Invalid fixture foreground");
        p.color = pixel(qRgba(color[0].toInt(), color[1].toInt(),
                              color[2].toInt(), color[3].toInt()));
      }
      if (source.contains("autoSpacingProbe") || source.contains("scatterProbe")) {
        const bool scatter = source.contains("scatterProbe");
        const auto evidence = source[scatter ? "scatterEvidence" : "autoSpacingEvidence"].toObject();
        require(!evidence["effectiveControls"].toObject().empty() &&
                    !evidence["effectiveBrushDefinition"].toString().isEmpty() &&
                    evidence["deliveryInterface"].toString() == "Node.paintLine" &&
                    evidence["deliveredContactEvents"].toArray().size() == 2 &&
                    evidence["sourceProfileVerified"] == QJsonValue(false) &&
                    evidence["sourceRngLawIdentified"] == QJsonValue(false),
                "Missing spacing/scatter effective-control and delivery evidence");
        if (scatter) {
          const auto spec = source["scatterProbe"].toObject();
          const int replicate = spec["replicate"].toInt(-1);
          require(replicate >= 0 && replicate < (spec["enabled"].toBool() ? 100 : 2),
                  "Invalid scatter candidate replicate");
          const QString axes = spec.value("axes").toString();
          require(axes == "x" || axes == "y" || axes == "xy",
                  "Unknown source scatter axes: " + caseContext);
          if (spec.value("enabled") == QJsonValue(true)) {
            require(p.profile == "krita-5.3.4.1-scatter-candidate-1" &&
                        p.sourceScatterV1.has_value() && p.varySeedPerStroke &&
                        p.sourceScatterV1->amount == spec.value("amount").toDouble() &&
                        p.sourceScatterV1->axisX == (axes != "y") &&
                        p.sourceScatterV1->axisY == (axes != "x"),
                    "Source scatter importer lost its distinct model or controls: " + caseContext);
          } else {
            require(spec.value("enabled") == QJsonValue(false) &&
                        p.profile == "krita-5.3.4.1-hard-pixel-candidate-1" &&
                        !p.sourceScatterV1.has_value(),
                    "Disabled source scatter failed to retain the hard-pixel control: " + caseContext);
          }
          // Independent native streams, without presuming source RNG identity.
          p.seed = UINT64_C(0x9e3779b97f4a7c15) * std::uint64_t(replicate + 1);
        }
      }
      if (source.contains("scatterHoldoutProbe")) {
        require(scatterHoldoutCandidate(fixtures["group"].toString()),
                "Unimplemented scatter domain cannot enter native comparison: " + caseContext);
        const auto spec = source["scatterHoldoutProbe"].toObject();
        const int replicate = spec["replicate"].toInt(-1);
        const QString axes = spec["axes"].toString();
        require(replicate >= 0 && replicate < (spec["enabled"].toBool() ? 100 : 2),
                "Invalid focused scatter candidate replicate: " + caseContext);
        if (spec["enabled"] == QJsonValue(true)) {
          require(p.profile == "krita-5.3.4.1-scatter-candidate-1" &&
                      p.sourceScatterV1.has_value() && p.varySeedPerStroke &&
                      p.sourceScatterV1->amount == 1 &&
                      p.sourceScatterV1->axisX == (axes != "y") &&
                      p.sourceScatterV1->axisY == (axes != "x"),
                  "Focused scatter importer lost its measured static component: " + caseContext);
        } else {
          require(spec["enabled"] == QJsonValue(false) &&
                      p.profile == "krita-5.3.4.1-hard-pixel-candidate-1" &&
                      !p.sourceScatterV1.has_value(),
                  "Focused scatter disabled control lost its existing hard-pixel gate: " + caseContext);
        }
        p.seed = UINT64_C(0x9e3779b97f4a7c15) * std::uint64_t(replicate + 1);
      }
      p.opacity.value = source["opacity"].toDouble();
      p.flow.value = source["flow"].toDouble();
      if (fixtures["group"].toString() == "gihstatistics" ||
          fixtures["group"].toString() == "gihlabels" ||
          (fixtures["group"].toString().startsWith("gih-correlation-") &&
           source["resourceProbe"].toObject()["kind"].toString() == "random")) {
        const auto probe = source["resourceProbe"].toObject();
        const double replicate = probe["replicate"].toDouble(-1);
        require(replicate >= 0 && replicate < 100 &&
                    replicate == std::floor(replicate) &&
                    stochasticReplicates.insert(int(replicate)).second &&
                    p.tip.randomSequence && p.tip.images.size() == 3,
                "Invalid stochastic candidate replicate or sequence");
        // Distinct reproducible native streams, not 100 replays of seed 1.
        // This does not presume any correspondence to the source RNG/seed.
        p.seed =
            UINT64_C(0x9e3779b97f4a7c15) * std::uint64_t(int(replicate) + 1);
      }
      if (fixtures["group"].toString() == "gihcadence") {
        const auto spec = source["cadenceProbe"].toObject();
        const int spacing = spec["spacing"].toInt(-1);
        require((spacing == 2 || spacing == 3 || spacing == 4) &&
                    spec["slots"].toInt() == 32 &&
                    source["diameter"].toDouble() == 8 &&
                    p.spacing == spacing && spec["replicate"].toInt(-1) >= 0 &&
                    spec["replicate"].toInt(-1) < 4 && p.tip.images.size() == 3,
                "Invalid bounded GIH cadence candidate settings: " + caseContext);
        p.seed = UINT64_C(0x9e3779b97f4a7c15) *
                 std::uint64_t(spec["replicate"].toInt() + 1);
      }
      bool bitmapViewRequiresNativeConversion = false;
      if (p.profile == "krita-5.3.4.1-fixed-bitmap-candidate-1" &&
          !validate(p).empty()) {
        // Authored view transforms can leave the imported source domain.
        // Compare that case explicitly as native conversion, never as parity.
        bitmapViewRequiresNativeConversion = true;
        p.profile = "groundzero-native-1";
      }
      const bool sourceRandomBitmap = p.profile == "krita-5.3.4.1-random-bitmap-candidate-1";
      const bool sourceBitmap =
          p.profile == "krita-5.3.4.1-fixed-bitmap-candidate-1" ||
          p.profile == "krita-5.3.4.1-fixed-bitmap-mask-candidate-1";
      if (fixtures["group"].toString().startsWith("gih-correlation-")) {
        const bool incremental = source["resourceProbe"].toObject()["kind"].toString() == "incremental";
        require(p.tip.images.size() == 3 && p.tip.randomSequence == !incremental &&
                    p.spacing == 4 && sourceBitmap == incremental && sourceRandomBitmap == !incremental,
                "GIH correlation importer changed original sequence/source-admission behavior: " + caseContext);
      }
      if (fixtures["group"].toString() == "bitmaptips") {
        const auto probe = source["resourceProbe"].toObject();
        const auto dimensions = probe["originalDimensions"].toArray();
        const QString kind = probe["kind"].toString();
        require(dimensions.size() == 2, "Missing original bitmap dimensions");
        const int extent =
            std::max(dimensions[0].toInt(), dimensions[1].toInt());
        const bool expected =
            (kind == "alpha" || kind == "color-image" || kind == "color-mask" ||
             kind == "incremental" || kind.startsWith("filter-")) &&
            (p.size.value >= extent && p.size.value <= extent * 2);
        require(sourceBitmap == expected,
                "Bitmap importer failed to enforce the measured source domain");
      }
      if (fixtures["group"].toString() == "mirrortips") {
        const bool expected = source["diameter"].toDouble() <= 16;
        require(
            sourceBitmap == expected,
            "Mirror importer failed to enforce measured bitmap scale domain");
        if (expected) {
          const auto spec = source["sensorProbe"].toObject();
          require(p.sourceMirror.enabled == spec["enabled"].toBool(),
                  "Mirror importer dropped source activation");
        }
      }
      if (source.contains("bitmapPathProbe")) {
        const auto delivery = source["deliveryReport"].toObject();
        const auto start = source["start"].toArray();
        const auto end = source["end"].toArray();
        require(source["bitmapPathProbe"].isBool() &&
                    source["bitmapPathProbe"].toBool() && resourceCase &&
                    source["resourceProbe"].toObject()["kind"].toString().startsWith("filter-") &&
                    start.size() == 2 && end.size() == 2 &&
                    end[0].toDouble() == start[0].toDouble() + 1 &&
                    end[1].toDouble() == start[1].toDouble() &&
                    delivery["mode"].toString() == "Node.paintPath" &&
                    delivery["vertices"].toArray() == QJsonArray{start, end} &&
                    delivery["singleContinuousStroke"].toBool(),
                "Missing exact fractional bitmap path delivery evidence");
      }
      Canvas canvas(initial);
      Stroke stroke(canvas, p);
      Sample first, last;
      first.x = source["start"].toArray()[0].toDouble();
      first.y = source["start"].toArray()[1].toDouble();
      first.hasPressure = true;
      first.pressure = source["pressure"].toArray()[0].toDouble();
      last = first;
      last.x = source["end"].toArray()[0].toDouble();
      last.y = source["end"].toArray()[1].toDouble();
      last.pressure = source["pressure"].toArray()[1].toDouble();
      last.time = 1;
      QElapsedTimer timer;
      timer.start();
      if (fixtures["group"].toString() == "gihcadence") {
        const auto spec = source["cadenceProbe"].toObject();
        const QString mode = spec["mode"].toString();
        const int step = 8 * spec["spacing"].toInt();
        const int end = 16 + 31 * step + 1;
        QJsonArray expectedPaths, expectedCenters;
        for (int i = 0; i < 32; ++i)
          expectedCenters.append(mode == "path-corners"
              ? QJsonArray{768 + ((i + 1) / 2) * step, 768 + (i / 2) * step}
              : QJsonArray{16 + i * step, 1024});
        require(source["cadenceCenters"].toArray() == expectedCenters,
                "GIH readback centers differ from exact path contract: " + caseContext);
        if (mode == "path-corners") {
          require(step == 16, "Invalid corner cadence spacing: " + caseContext);
          QJsonArray path = expectedCenters;
          const auto endPoint = path.last().toArray();
          path.append(QJsonArray{endPoint[0].toInt() + 1, endPoint[1]});
          expectedPaths.append(path);
        } else if (mode == "discrete") {
          for (int i = 0; i < 32; ++i)
            expectedPaths.append(QJsonArray{QJsonArray{16 + i * step, 1024},
                                            QJsonArray{17 + i * step, 1024}});
        } else {
          require(mode == "line" || mode == "path-endpoints" ||
                      mode == "path-dabs" || mode == "path-halves" ||
                      mode == "path-offgrid" || mode == "qt-dabs" || mode == "qt-halves",
                  "Unknown GIH cadence mode: " + caseContext);
          const int increment = (mode == "path-dabs" || mode == "qt-dabs") ? step
                                : (mode == "path-halves" || mode == "qt-halves") ? step / 2
                                : mode == "path-offgrid" ? 17
                                                         : end;
          QJsonArray path;
          for (int x = 16; x < end; x += increment)
            path.append(QJsonArray{x, 1024});
          path.append(QJsonArray{end, 1024});
          expectedPaths.append(path);
        }
        const auto delivered = source["deliveryReport"].toObject();
        require(source["cadencePaths"].toArray() == expectedPaths &&
                    delivered["cadencePaths"].toArray() == expectedPaths &&
                    delivered["strokeCount"].toInt() == expectedPaths.size() &&
                    delivered["mode"].toString() ==
                        (mode.startsWith("qt-") ? "Qt tablet events"
                         : mode == "line" || mode == "discrete"
                             ? "Node.paintLine" : "Node.paintPath"),
                "GIH source cadence delivery differs from authored paths: " + caseContext);
        if (mode.startsWith("qt-")) {
          require(step == 16, "Qt cadence requires bounded spacing two: " + caseContext);
          QJsonArray contacts;
          for (const auto &event : delivered["events"].toArray()) {
            const auto item = event.toObject();
            if ((item["type"].toInt() == 92 || item["type"].toInt() == 87) &&
                item["pressure"].toDouble() > 0) contacts.append(item);
          }
          const auto path = expectedPaths[0].toArray();
          require(contacts.size() == path.size(), "Incomplete Qt GIH contacts: " + caseContext);
          const double timeBase = contacts[0].toObject()["timeMs"].toDouble();
          for (int i = 0; i < contacts.size(); ++i) {
            const auto contact = contacts[i].toObject();
            require(contact["image"].toArray() == path[i].toArray() &&
                        contact["pressure"].toDouble() == 1 &&
                        contact["tilt"].toArray() == QJsonArray{0, 0} &&
                        contact["rotation"].toDouble() == 0 &&
                        std::abs(contact["timeMs"].toDouble() - timeBase - i * 4.167) <= .500001,
                    "Actual Qt GIH inputs differ from causal control: " + caseContext);
          }
        }
        const auto labels =
            source["resourceReadback"].toObject()["frameSelections"].toArray();
        require(labels.size() == 32,
                "Missing bounded source cadence frame labels: " + caseContext);
        for (const auto &label : labels)
          require(label.isNull() ||
                      (label.isDouble() && label.toDouble() >= 0 &&
                       label.toDouble() <= 2 &&
                       label.toDouble() == std::floor(label.toDouble())),
                  "Invalid source cadence label: " + caseContext);
        for (int i = 0; i < expectedPaths.size(); ++i) {
          const auto path = expectedPaths[i].toArray();
          Preset separate = p;
          separate.seed ^= UINT64_C(0xd1b54a32d192ed03) * std::uint64_t(i + 1);
          Stroke dab(canvas, separate);
          Stroke &target = mode == "discrete" ? dab : stroke;
          for (int j = 0; j < path.size(); ++j) {
            const auto point = path[j].toArray();
            Sample sample = first;
            sample.x = point[0].toDouble();
            sample.y = point[1].toDouble();
            sample.time = j / 240.;
            require(target.add(sample), "GIH cadence stroke failed [" + caseContext +
                        "] mode=" + mode + ": " + QString::fromStdString(target.error()));
          }
          if (mode == "discrete")
            require(dab.finish(), "GIH cadence stroke finish failed [" + caseContext +
                        "]: " + QString::fromStdString(dab.error()));
        }
      } else if (fixtures["group"].toString().startsWith("gih-correlation-")) {
        const auto plan = validateGihCorrelationCase(source, fixtures["group"].toString());
        const auto vertices = plan["vertices"].toArray();
        for (int index = 0; index < vertices.size(); ++index) {
          const auto point = vertices[index].toArray();
          Sample sample = first;
          sample.x = point[0].toDouble(); sample.y = point[1].toDouble();
          // These fixed geometry controls have no time dynamics. Public Node
          // APIs expose vertices, not internal dab times; keep native time explicit.
          sample.time = plan["mode"].toString() == "Node.paintLine" ? double(index) : index / 240.;
          require(stroke.add(sample), "GIH correlation vertex replay failed [" + caseContext +
                      "] vertex=" + QString::number(index) + ": " + QString::fromStdString(stroke.error()));
        }
      } else if (source["generatedCurvePath"].toBool()) {
        const auto points = source["sourcePath"].toArray();
        const auto delivery = source["deliveryReport"].toObject();
        require(fixtures["group"].toString() == "generatedcurves" &&
                    points.size() == 2 &&
                    delivery["mode"].toString() == "Node.paintPath" &&
                    delivery["singleContinuousStroke"] == QJsonValue(true) &&
                    delivery["physicalTabletClaimed"] == QJsonValue(false) &&
                    delivery["vertices"].toArray() == points,
                "Missing generated curve fractional delivery evidence");
        for (int i = 0; i < points.size(); ++i) {
          const auto point = points[i].toArray();
          require(point.size() == 2 && point[0].isDouble() && point[1].isDouble() &&
                      std::isfinite(point[0].toDouble()) && std::isfinite(point[1].toDouble()) &&
                      point[0].toDouble() >= 1023.875 && point[0].toDouble() <= 1025.5 &&
                      point[1].toDouble() >= 1023.875 && point[1].toDouble() <= 1024.5,
                  "Generated curve fractional path leaves its bounded origin controls");
          Sample event = first;
          event.x = point[0].toDouble();
          event.y = point[1].toDouble();
          event.time = double(i) / 240;
          require(stroke.add(event), QString::fromStdString(stroke.error()));
        }
      } else if (fixtures["group"].toString() == "wetgeometry") {
        const auto path = source["sourcePath"].toArray();
        const auto delivery = source["deliveryReport"].toObject();
        const auto evidence = source["wetGeometryEvidence"].toObject();
        require(path.size() >= 2 && path.size() <= 129 &&
                    delivery["mode"].toString() == "Node.paintPath" &&
                    delivery["singleContinuousStroke"].toBool() &&
                    delivery["vertices"].toArray() == path &&
                    !evidence["windows"].toArray().empty() &&
                    evidence["model"] == source["wetGeometryProbe"].toObject()["model"] &&
                    evidence["pathId"] == source["wetGeometryProbe"].toObject()["pathId"] &&
                    evidence["sourceParityQualified"] == QJsonValue(false),
                "Missing wet geometry diagnostic path/readback");
        for (int i = 0; i < path.size(); ++i) {
          const auto point = path[i].toArray();
          require(point.size() == 2, "Invalid wet geometry diagnostic vertex");
          Sample event = first;
          event.x = point[0].toDouble();
          event.y = point[1].toDouble();
          event.time = double(i) / 240;
          require(stroke.add(event), QString::fromStdString(stroke.error()));
        }
      } else if (fixtures["group"].toString() == "wetdrift") {
        const auto spec = source["driftProbe"].toObject();
        const int count = spec["nominalDabs"].toInt();
        require(count == 1 || count == 10 || count == 100 || count == 1000 || count == 10000,
                "Invalid drift checkpoint");
        auto center = [](int index) {
          const int row = index / 128, column = index % 128;
          return QPoint(8 + 16 * (row % 2 ? 127 - column : column), 8 + 16 * row);
        };
        std::vector<QPoint> points{center(0)};
        for (int i = 1; i < count; ++i) {
          if (i > 1 && center(i) - center(i - 1) == center(i - 1) - center(i - 2))
            points.back() = center(i);
          else points.push_back(center(i));
        }
        points.push_back(center(count - 1) + (center(count) - center(count - 1)) / 16);
        QJsonArray expectedPath;
        for (const auto &point : points) expectedPath.append(QJsonArray{point.x(), point.y()});
        const auto delivery = source["deliveryReport"].toObject();
        const auto evidence = source["driftEvidence"].toObject();
        require(source["sourcePath"].toArray() == expectedPath &&
                    delivery["vertices"].toArray() == expectedPath &&
                    delivery["mode"].toString() == "Node.paintPath" &&
                    delivery["singleContinuousStroke"].toBool() &&
                    evidence["observedDepositCells"].toInt() == count - (spec["calibration"].toBool() ? 0 : 1) &&
                    evidence["maximumCellOffset"].toInt() <= 5,
                "Missing continuous drift geometry/count evidence");
        const QString model = spec["model"].toString();
        require(p.profile == ("krita-5.3.4.1-" +
                    (spec["calibration"].toBool() ? QString("hard-pixel") : model) + "-candidate-1").toStdString(),
                "Drift imported a native conversion instead of source candidate");
        for (size_t i = 0; i < points.size(); ++i) {
          Sample event = first;
          event.x = points[i].x(); event.y = points[i].y();
          event.time = double(i) / 240;
          require(stroke.add(event), QString::fromStdString(stroke.error()));
        }
      } else if (fixtures["group"].toString() == "gihlabels") {
        const auto path = source["sourcePath"].toArray();
        const auto delivery = source["deliveryReport"].toObject();
        require(path.size() == 96 && delivery["vertices"].toArray() == path &&
                    delivery["mode"].toString() == "Node.paintPath" &&
                    delivery["singleContinuousStroke"].toBool(),
                "Missing bounded continuous source path evidence");
        for (int i = 0; i < 96; ++i) {
          const int row = i / 2;
          const int x = ((row % 2 == 0) == (i % 2 == 0)) ? 16 : 2032;
          const int y = 16 + row * 32;
          const auto point = path[i].toArray();
          require(point.size() == 2 && point[0].toInt() == x &&
                      point[1].toInt() == y,
                  "Source path geometry differs from fixed label fixture");
          Sample sample = first;
          sample.x = x;
          sample.y = y;
          sample.time = i / 240.0;
          require(stroke.add(sample), QString::fromStdString(stroke.error()));
        }
      } else if (source.contains("events")) {
        auto events = source["events"].toArray();
        if (sensorCase) {
          events = {};
          const double timeBase =
              deliveredSensorEvents[0].toObject()["timeMs"].toDouble();
          for (const auto &value : deliveredSensorEvents) {
            const auto delivered = value.toObject();
            const auto image = delivered["image"].toArray(),
                       tilt = delivered["tilt"].toArray();
            events.append(QJsonObject{
                {"x", image[0]},
                {"y", image[1]},
                {"pressure", delivered["pressure"]},
                {"tiltX", tilt[0]},
                {"tiltY", tilt[1]},
                {"rotation", delivered["rotation"]},
                {"timeUs",
                 (delivered["timeMs"].toDouble() - timeBase) * 1000}});
          }
        }
        if (source.contains("timedProbe")) {
          const auto evidence = source["timedEvidence"].toObject();
          require(evidence["actualTimingVerified"].toBool() &&
                      evidence["sourceParityQualified"].isBool() &&
                      !evidence["sourceParityQualified"].toBool(),
                  "Missing measured paced source timing");
          auto contacts = evidence["deliveredContacts"].toArray();
          require(contacts.size() == source["events"].toArray().size() &&
                      (contacts.size() == 1 || contacts.size() == 17 ||
                       contacts.size() == 33 || contacts.size() == 65),
                  "Incomplete paced source contacts");
          const auto spec = source["timedProbe"].toObject();
          const double duration = spec["durationUs"].toDouble();
          const double maxLateness = spec["maxLatenessUs"].toDouble();
          const bool strictTiming =
              duration == 375000 || duration == 1125000 ||
              duration == 1250000 || duration == 2250000 ||
              spec["target"].toString().startsWith("speed-cadence-");
          require(maxLateness == (strictTiming ? 20000 : 250000),
                  "Unexpected paced lateness contract");
          require(duration == 250000 || duration == 375000 ||
                      duration == 1000000 || duration == 1125000 ||
                      duration == 1250000 || duration == 2250000,
                  "Unexpected paced duration");
          const auto discarded = evidence["discardedTimingAttempts"].toArray();
          require(evidence["discardedTimingAttempts"].isArray() &&
                      discarded.size() <= 1,
                  "Too many discarded timing acquisitions");
          for (const auto &item : discarded) {
            const auto attempt = item.toObject();
            const auto detail = attempt["detail"].toObject();
            const QString phase = detail["phase"].toString();
            const int index = detail["index"].toInt(-1);
            const auto authoredEvents = source["events"].toArray();
            require(attempt["attempt"].toInt() == 1 &&
                        attempt["reason"].toString() == "timing-only" &&
                        attempt["cleanup"].toString() == "release-and-leave" &&
                        (phase == "contact" || phase == "release") &&
                        index >= 0 && index <= authoredEvents.size() &&
                        (phase != "contact" || index < authoredEvents.size()) &&
                        (phase != "release" || index == authoredEvents.size()),
                    "Invalid timing-only discarded acquisition");
            const double planned =
                phase == "release"
                    ? duration
                    : authoredEvents[index].toObject()["timeUs"].toDouble();
            const double elapsed = detail["elapsedUs"].toDouble();
            const double late = detail["latenessUs"].toDouble();
            require(std::isfinite(elapsed) && elapsed == std::floor(elapsed) &&
                        detail["authoredUs"].toDouble() == planned &&
                        detail["boundUs"].toDouble() == maxLateness &&
                        late == elapsed - planned &&
                        (late > maxLateness || late < -2000),
                    "Discarded acquisition was inside the timing contract");
          }
          const double origin =
              contacts[0].toObject()["monotonicUs"].toDouble();
          const auto clock = evidence["deviceClock"].toObject();
          const int clockScale = spec["deviceTimestampScale"].toInt(1);
          const int clockOffset = spec["deviceTimestampOffsetMs"].toInt(0);
          require(((clockScale == 1 && clockOffset == 0) ||
                   (clockScale == 2 && (clockOffset == 0 || clockOffset == 1000000))) &&
                      (clockScale == 1 || spec["target"].toString().startsWith("time-clock-")) &&
                      clock["scale"].toInt() == clockScale &&
                      clock["offsetMs"].toInt(-1) == clockOffset &&
                      std::isfinite(clock["baseMs"].toDouble(-1)) &&
                      clock["baseMs"].toDouble(-1) >= 0 &&
                      std::isfinite(clock["originNs"].toDouble(-1)) &&
                      clock["originNs"].toDouble(-1) >= 0,
                  "Invalid authored/delivered device clock discriminator");
          auto clockMatches = [&](const QJsonObject &event) {
            const double clockMs = std::max(0.0, std::floor(
                (event["monotonicUs"].toDouble() * 1000 -
                 clock["originNs"].toDouble()) / 1000000));
            return event["timeMs"].toDouble(-1) ==
                   clock["baseMs"].toDouble() + clockMs * clockScale;
          };
          const auto authored = source["events"].toArray();
          for (int i = 0; i < contacts.size(); ++i) {
            const auto contact = contacts[i].toObject();
            const double elapsed = contact["monotonicUs"].toDouble() - origin;
            const double planned = authored[i].toObject()["timeUs"].toDouble();
            require(std::isfinite(elapsed) && elapsed >= planned - 2000 &&
                        elapsed <= planned + maxLateness &&
                        contact["pressure"].toDouble() == 1 && clockMatches(contact),
                    "Paced contact failed elapsed/device-clock bounds");
          }
          auto release = evidence["release"].toObject();
          const double held = release["monotonicUs"].toDouble() - origin;
          require(std::isfinite(held) && held >= duration - 2000 &&
                      held <= duration + maxLateness && clockMatches(release),
                  "Paced release failed elapsed/device-clock bounds");
          release["pressure"] = 1;
          contacts.append(release);
          events = QJsonArray();
          for (const auto &value : contacts) {
            const auto delivered = value.toObject();
            const auto image = delivered["image"].toArray();
            require(image.size() == 2, "Incomplete paced source coordinates");
            events.append(QJsonObject{
                {"x", image[0]},
                {"y", image[1]},
                {"pressure", delivered["pressure"]},
                {"tiltX", 0},
                {"tiltY", 0},
                {"rotation", 0},
                {"timeUs", delivered["monotonicUs"].toDouble() - origin}});
          }
        }
        require(!events.empty() && events.size() <= 4096,
                "Invalid bounded event trace");
        for (const auto &value : events) {
          auto event = value.toObject();
          Sample sample;
          sample.x = event["x"].toDouble();
          sample.y = event["y"].toDouble();
          sample.pressure = event["pressure"].toDouble();
          sample.tiltX = event["tiltX"].toDouble() / 60;
          sample.tiltY = event["tiltY"].toDouble() / 60;
          sample.rotation =
              event["rotation"].toDouble() * 3.14159265358979323846 / 180;
          sample.time = event["timeUs"].toDouble() / 1000000;
          sample.hasPressure = sample.hasTilt = sample.hasRotation = true;
          require(stroke.add(sample), QString::fromStdString(stroke.error()));
        }
      } else {
        require(stroke.add(first) && stroke.add(last),
                QString::fromStdString(stroke.error()));
      }
      if (!stroke.finish())
        require(false, "Candidate stroke finish failed [" + caseContext + "]: " +
                           QString::fromStdString(stroke.error()));
      double elapsed = timer.nsecsElapsed() / 1e6;
      QImage candidate = canvas.image();
      QString basename = source["image"].toString();
      QImage reference(root.filePath("output/" + basename));
      require(!reference.isNull(), "Cannot decode source image");
      require(candidate.save(root.filePath("output/native-" + basename)),
              "Cannot save candidate image");
      auto metrics =
          compare(initial, reference.convertToFormat(QImage::Format_ARGB32),
                  candidate, root.filePath("output/diff-" + basename));
      metrics["preset"] = name;
      metrics["inputCase"] = source;
      metrics["comparisonStatus"] = sourceGenerated ? "measured-source-candidate"
                                                    : "measured-conversion";
      if (source.contains("timedProbe")) {
        metrics["timedAcquisitionInputsValidated"] = true;
        metrics["sourceParityQualified"] = false;
        metrics["candidateTimingConvention"] =
            "Actual monotonic source contacts plus held final position until "
            "measured release; native conversion only";
      }
      metrics["candidateProfile"] = QString::fromStdString(p.profile);
      if (sourceRandomBitmap) {
        metrics["randomBitmapSourceMechanism"] = "proposal-geometry-cache-v1";
        metrics["randomBitmapSourceQualified"] = false;
        metrics["sourcePhaseGridEstablished"] = false;
        metrics["sourceSeedParityClaimed"] = false;
      }
      metrics["bitmapViewRequiresNativeConversion"] =
          bitmapViewRequiresNativeConversion;
      if (sourceBitmap) {
        const bool passed =
            metrics["maxAlphaError"].toDouble() <= 1.0 / 255 + 1e-12 &&
            metrics["maxRGBError"].toDouble() <= 2.0 / 255 + 1e-12 &&
            metrics["alphaCoverageIoU"].toDouble() == 1 &&
            !metrics["emptyCandidateFailure"].toBool();
        metrics["bitmapSourceCandidatePassed"] = passed;
        if (!passed)
          failedHardCases.append(name);
      }
      if (source.contains("scatterHoldoutProbe")) {
        require(scatterHoldoutCandidate(fixtures["group"].toString()),
                "Unimplemented scatter domain cannot yield native metrics");
        const auto rect = source["scatterHoldoutProbe"].toObject()["rect"].toArray();
        require(rect.size() == 4, "Missing focused scatter candidate region");
        const int left = rect[0].toInt(), top = rect[1].toInt(),
                  width = rect[2].toInt(), height = rect[3].toInt();
        require(left >= 0 && top >= 0 && width == 1280 && height == 256 &&
                    left + width <= candidate.width() && top + height <= candidate.height(),
                "Focused static scatter candidate escaped its authored raster bounds");
        QByteArray alpha;
        alpha.reserve(width * height);
        for (int y = 0; y < candidate.height(); ++y) {
          const auto *line = reinterpret_cast<const QRgb *>(candidate.constScanLine(y));
          for (int x = 0; x < candidate.width(); ++x) {
            const int a = qAlpha(line[x]);
            if (x >= left && x < left + width && y >= top && y < top + height)
              alpha.append(char(a));
            else require(a == 0, "Focused scatter candidate alpha escaped the fixed source region");
          }
        }
        metrics["stochasticAlpha"] = QJsonObject{
            {"rect", rect}, {"encoding", "zlib-base64-alpha8-row-major"},
            {"data", QString::fromLatin1(qCompress(alpha).mid(4).toBase64())}};
        metrics["scatterSourceProfileQualified"] = false;
        metrics["scatterHoldoutSourceParityQualified"] = false;
      }
      if (source.contains("scatterProbe")) {
        const auto rect = source["scatterProbe"].toObject()["rect"].toArray();
        require(rect.size() == 4, "Missing bounded scatter candidate region");
        const int left = rect[0].toInt(), top = rect[1].toInt(),
                  width = rect[2].toInt(), height = rect[3].toInt();
        require(left >= 0 && top >= 0 && width > 0 && width <= 1280 &&
                    height > 0 && height <= 1280 && width * height <= 1280 * 256 &&
                    left <= candidate.width() - width &&
                    top <= candidate.height() - height,
                "Invalid bounded scatter candidate region");
        QByteArray alpha;
        alpha.reserve(width * height);
        for (int y = 0; y < candidate.height(); ++y)
          for (int x = 0; x < candidate.width(); ++x) {
            const auto a = qAlpha(candidate.pixel(x, y));
            if (x >= left && x < left + width && y >= top && y < top + height)
              alpha.append(char(a));
            else require(a == 0, "Candidate scatter stroke escaped fixed region");
          }
        metrics["stochasticAlpha"] = QJsonObject{
            {"rect", rect}, {"encoding", "zlib-base64-alpha8-row-major"},
            {"data", QString::fromLatin1(qCompress(alpha).mid(4).toBase64())}};
        metrics["scatterSourceProfileQualified"] = false;
      }
      if (fixtures["group"].toString() == "gihstatistics") {
        const auto rect = source["resourceProbe"].toObject()["rect"].toArray();
        require(rect.size() == 4, "Missing stochastic region");
        const int left = rect[0].toInt(), top = rect[1].toInt(),
                  width = rect[2].toInt(), height = rect[3].toInt();
        require(left >= 0 && top >= 0 && width > 0 && width <= 2048 &&
                    height > 0 && height <= 128 &&
                    left + width <= candidate.width() &&
                    top + height <= candidate.height(),
                "Invalid bounded stochastic region");
        QByteArray alpha;
        alpha.reserve(width * height);
        for (int y = 0; y < candidate.height(); ++y)
          for (int x = 0; x < candidate.width(); ++x) {
            const auto a = qAlpha(candidate.pixel(x, y));
            if (x >= left && x < left + width && y >= top && y < top + height)
              alpha.append(char(a));
            else
              require(a == 0, "Candidate random stroke escaped fixed region");
          }
        const int slotCount =
            source["resourceProbe"].toObject()["expectedSlots"].toInt();
        const int diameter = source["diameter"].toInt();
        require(slotCount == 32 && diameter == 8,
                "Unexpected stochastic frame-label fixture geometry");
        QJsonArray selections;
        const int radius = diameter / 2 + 2;
        for (int slot = 0; slot < slotCount; ++slot) {
          const int cx = 256 + slot * diameter * 4;
          quint64 sums[3] = {0, 0, 0};
          for (int y = 1024 - radius; y <= 1024 + radius; ++y)
            for (int x = cx - radius; x <= cx + radius; ++x) {
              const auto color = candidate.pixel(x, y);
              const auto alphaValue = qAlpha(color);
              sums[0] += qRed(color) * alphaValue;
              sums[1] += qGreen(color) * alphaValue;
              sums[2] += qBlue(color) * alphaValue;
            }
          const int winner = int(std::max_element(sums, sums + 3) - sums);
          if (!sums[winner]) {
            selections.append(QJsonValue(QJsonValue::Null));
          } else {
            for (int channel = 0; channel < 3; ++channel)
              require(
                  channel == winner || sums[channel] == 0,
                  "Candidate GIH frame is not an original primary-color tip");
            selections.append(winner);
          }
        }
        metrics["stochasticFrameSelections"] = selections;
        metrics["stochasticAlpha"] = QJsonObject{
            {"rect", rect},
            {"encoding", "zlib-base64-alpha8-row-major"},
            {"data", QString::fromLatin1(qCompress(alpha).mid(4).toBase64())}};
      }
      if (fixtures["group"].toString() == "gihlabels") {
        QJsonArray labels;
        for (int row = 0; row < 48; ++row)
          for (int col = 0; col < 64; ++col) {
            const int cx = 16 + 32 * (row % 2 == 0 ? col : 63 - col);
            const int cy = 16 + 32 * row;
            quint64 sums[3] = {0, 0, 0};
            for (int y = cy - 6; y <= cy + 6; ++y)
              for (int x = cx - 6; x <= cx + 6; ++x) {
                const auto c = candidate.pixel(x, y);
                sums[0] += qRed(c) * qAlpha(c);
                sums[1] += qGreen(c) * qAlpha(c);
                sums[2] += qBlue(c) * qAlpha(c);
              }
            const int winner = int(std::max_element(sums, sums + 3) - sums);
            if (!sums[winner])
              labels.append(QJsonValue(QJsonValue::Null));
            else {
              for (int c = 0; c < 3; ++c)
                require(c == winner || sums[c] == 0,
                        "Long candidate label cell mixes primary colors");
              labels.append(winner);
            }
          }
        metrics["stochasticFrameSelections"] = labels;
      }
      if (fixtures["group"].toString() == "gihcadence") {
        QJsonArray labels;
        for (int slot = 0; slot < 32; ++slot) {
          const auto center = source["cadenceCenters"].toArray()[slot].toArray();
          const int cx = center[0].toInt(), cy = center[1].toInt();
          quint64 sums[3] = {0, 0, 0};
          for (int y = cy - 6; y <= cy + 6; ++y)
            for (int x = cx - 6; x <= cx + 6; ++x) {
              const auto color = candidate.pixel(x, y);
              sums[0] += qRed(color) * qAlpha(color);
              sums[1] += qGreen(color) * qAlpha(color);
              sums[2] += qBlue(color) * qAlpha(color);
            }
          const int winner = int(std::max_element(sums, sums + 3) - sums);
          if (!sums[winner])
            labels.append(QJsonValue(QJsonValue::Null));
          else {
            for (int channel = 0; channel < 3; ++channel)
              require(channel == winner || sums[channel] == 0,
                      "Candidate cadence cell mixes frame colors");
            labels.append(winner);
          }
        }
        metrics["candidateFrameSelections"] = labels;
        const bool incremental =
            source["resourceProbe"].toObject()["kind"].toString() == "incremental";
        require(sourceBitmap == incremental && sourceRandomBitmap == !incremental,
                "GIH importer did not separate deterministic and experimental random source behavior");
        if (incremental) {
          const auto referenceLabels =
              source["resourceReadback"].toObject()["frameSelections"].toArray();
          const bool discrete =
              source["cadenceProbe"].toObject()["mode"].toString() == "discrete";
          bool ordered = labels.size() == 32 && referenceLabels.size() == 32;
          for (int slot = 0; slot < 32 && ordered; ++slot) {
            const int expected = discrete ? 0 : slot % 3;
            ordered = !labels[slot].isNull() && !referenceLabels[slot].isNull() &&
                      labels[slot].toInt(-1) == expected &&
                      referenceLabels[slot].toInt(-1) == expected;
          }
          metrics["incrementalFrameSequencePassed"] = ordered;
          if (!ordered) failedHardCases.append(name + " frame advancement");
        }
        metrics["cadenceQualification"] = incremental
            ? "Incremental source candidate: exact frame/reset and full-region pixel gates"
            : "Versioned proposal/geometry cache candidate; independent selections on misses; source seed parity unclaimed";
      }
      if (fixtures["group"].toString().startsWith("gih-correlation-")) {
        const auto plan = validateGihCorrelationCase(source, fixtures["group"].toString());
        const auto centers = plan["centers"].toArray();
        const auto labels = gihCorrelationLabels(candidate, centers, caseContext + "/candidate");
        const auto observed = gihCorrelationLabels(reference, centers, caseContext + "/source-PNG");
        require(observed == source["gihCorrelationEvidence"].toObject()["frameSelections"].toArray(),
                "GIH correlation retained source PNG disagrees with recorded labels: " + caseContext);
        const auto rect = plan["rect"].toArray();
        const int left = rect[0].toInt(), top = rect[1].toInt(), width = rect[2].toInt(), height = rect[3].toInt();
        QByteArray alpha;
        alpha.reserve(width * height);
        for (int y = 0; y < candidate.height(); ++y)
          for (int x = 0; x < candidate.width(); ++x) {
            const int value = qAlpha(candidate.pixel(x, y));
            if (x >= left && x < left + width && y >= top && y < top + height) alpha.append(char(value));
            else require(value == 0, "GIH correlation native coverage escaped retained bounds: " + caseContext);
          }
        metrics["candidateFrameSelections"] = labels;
        metrics["gihCorrelationConfiguration"] = plan["configuration"];
        metrics["gihCorrelationReplicate"] = source["gihCorrelationProbe"].toObject()["replicate"];
        metrics["stochasticAlpha"] = QJsonObject{{"rect", rect}, {"encoding", "zlib-base64-alpha8-row-major"},
            {"data", QString::fromLatin1(qCompress(alpha).mid(4).toBase64())}};
        metrics["gihCorrelationSourceModelVerified"] = false;
        metrics["gihCorrelationSourceParityQualified"] = false;
        metrics["gihCorrelationCandidateInputConvention"] =
            "One continuous native stroke with every actual public API vertex in order; fixed pressure/transfer; no inferred source dab times or RNG correspondence";
      }
      metrics["candidateSeed"] = QString::number(qulonglong(p.seed));
      metrics["sourceSemanticallySupported"] = false;
      metrics["sourceReferenceVerified"] = false;
      if (sourceHard || sourceSoft || sourceGenerated) {
        const bool alphaCoverage = metrics["transparentBase"].toBool();
        auto rb =
            metrics[alphaCoverage ? "referenceAlphaBounds" : "referenceBounds"]
                .toArray();
        auto cb =
            metrics[alphaCoverage ? "candidateAlphaBounds" : "candidateBounds"]
                .toArray();
        const double boundsError =
            std::max({std::abs(rb[0].toDouble() - cb[0].toDouble()),
                      std::abs(rb[1].toDouble() - cb[1].toDouble()),
                      std::abs(rb[0].toDouble() + rb[2].toDouble() -
                               cb[0].toDouble() - cb[2].toDouble()),
                      std::abs(rb[1].toDouble() + rb[3].toDouble() -
                               cb[1].toDouble() - cb[3].toDouble())});
        bool passed =
            metrics["premultipliedRGBMAE"].toDouble() <= 2 / 255.0 &&
            metrics["alphaMAE"].toDouble() <= 1 / 255.0 &&
            metrics["alphaP99"].toDouble() <= 4 / 255.0 &&
            metrics[alphaCoverage ? "alphaCoverageIoU"
                                  : "visibleChangedPixelIoU"]
                    .toDouble() >= .98 &&
            boundsError <= std::max(1.0, source["diameter"].toDouble() * .02) &&
            !metrics["emptyCandidateFailure"].toBool();
        if (fixtures["group"].toString() == "geometry")
          passed = passed && metrics["maxRGBError"].toDouble() == 0 &&
                   metrics["maxAlphaError"].toDouble() == 0;
        if (sourceGenerated) {
          // Every pixel participates through the full union region. Preserve
          // coverage, endpoints and ordinary profile limits, then enforce the
          // tighter measured generated-mask per-pixel code bounds as well.
          passed = passed && metrics["maxAlphaError"].toDouble() <= 1 / 255.0 &&
                   metrics["maxRGBError"].toDouble() <= 2 / 255.0;
          if (fixtures["group"].toString() == "generated-transfer") {
            const bool expectedNoop = generatedTransferExpectedNoop(fixtures["group"].toString(), authoredCase);
            passed = passed && generatedTransferRasterMatchesExpectation(expectedNoop, metrics);
            metrics["generatedTransferExpectedNoop"] = expectedNoop;
          }
          metrics["generatedMaskCandidatePassed"] = passed;
          metrics["generatedMaskSourceParityQualified"] = false;
          metrics["generatedMaskSourceDefinitionRetained"] = true;
          metrics["generatedMaskQualification"] =
              "Experimental independently measured mask model; complete RCP1 geometry, dynamics and combination qualification remains separate";
        } else {
          metrics["hardPixelCandidatePassed"] = passed;
        }
        metrics["boundsError"] = boundsError;
        if (!passed)
          failedHardCases.append(name);
      }
      if (sourceHsv) {
        // Full-canvas union-region metrics, including alpha and missing paint;
        // no center-pixel-only qualification or dilution by empty canvas.
        const double colorTolerance =
            p.sourceHsvChannel == SourceHsvChannel::Saturation ? 1 / 255.0 : 0;
        // Both hard masks are binary-alpha and RGB8. Compare integer code
        // differences, avoiding floating subtraction placing exactly one byte
        // microscopically above 1/255. Alpha and geometry gates are unchanged.
        const int byteError =
            int(std::floor(metrics["maxRGBError"].toDouble() * 255 + .5));
        const bool passed =
            metrics["hardPixelCandidatePassed"].toBool() &&
            byteError <=
                (p.sourceHsvChannel == SourceHsvChannel::Saturation ? 1 : 0) &&
            metrics["maxAlphaError"].toDouble() == 0;
        metrics["fixedHsvCandidatePassed"] = passed;
        metrics["fixedHsvMaxRGBByteError"] = byteError;
        metrics["fixedHsvMaxRGBTolerance"] = colorTolerance;
        if (!passed && !failedHardCases.contains(name))
          failedHardCases.append(name);
      }
      if (wetHsvCase) {
        const auto spec = source["wetHsvProbe"].toObject();
        const QString channel = spec["channel"].toString();
        const auto expectedChannel =
            !spec["enabled"].toBool() ? SourceHsvChannel::Off
            : channel == "h"          ? SourceHsvChannel::Hue
            : channel == "s"          ? SourceHsvChannel::Saturation
                                      : SourceHsvChannel::Value;
        bool eligible =
            p.profile ==
                ("krita-5.3.4.1-" + spec["model"].toString() + "-candidate-1")
                    .toStdString() &&
            p.sourceHsvChannel == expectedChannel &&
            (expectedChannel == SourceHsvChannel::Off ||
             p.sourceHsvValue == spec["value"].toDouble());
        if (spec.contains("composition")) {
          const auto composition = spec["composition"].toArray();
          eligible = composition.size() == 3 &&
              p.profile == ("krita-5.3.4.1-" + spec["model"].toString() + "-candidate-1").toStdString() &&
              p.sourceHsvChannel == SourceHsvChannel::Off &&
              p.opacity.value == spec["transfer"].toDouble() &&
              p.flow.value == spec["transfer"].toDouble();
          for (int c = 0; c < composition.size() && c < 3; ++c) {
            const auto &control = p.sourceHsvV2[c];
            eligible = eligible && control.enabled;
            if (composition[c].isString()) {
              eligible = eligible && composition[c].toString() == "pressure" &&
                  control.control.value == 1 && control.control.mappings.size() == 1;
              if (control.control.mappings.size() == 1) {
                const auto &mapping = control.control.mappings[0];
                eligible = eligible && mapping.sensor == Sensor::Pressure &&
                    mapping.curve.size() == 2 && mapping.curve[0].x == 0 &&
                    mapping.curve[0].y == 0 && mapping.curve[1].x == 1 && mapping.curve[1].y == 1;
              }
            } else {
              eligible = eligible && control.control.mappings.empty() &&
                  control.control.value == composition[c].toDouble();
            }
          }
          if (spec["textureMode"].isNull()) eligible = eligible && !p.texture.image;
          else eligible = eligible && p.texture.image && p.texture.depth == .5 &&
              p.texture.mode == (spec["textureMode"].toInt() == 0 ? MaskMode::Multiply : MaskMode::Subtract);
        }
        const bool passed =
            eligible && metrics["maxRGBError"].toDouble() <= 2 / 255.0 &&
            metrics["maxAlphaError"].toDouble() <= 1 / 255.0 &&
            metrics["visibleChangedPixelIoU"].toDouble() >= .98 &&
            !metrics["emptyCandidateFailure"].toBool();
        metrics["wetHsvSourceEligible"] = eligible;
        metrics["wetHsvCandidatePassed"] = passed;
        metrics["wetHsvQualification"] =
            "Full union-region RGBA and source eligibility; native conversion "
            "cannot pass";
        if (!passed && !failedHardCases.contains(name))
          failedHardCases.append(name);
      }
      if (sourceWet) {
        const bool passed =
            metrics["maxRGBError"].toDouble() <= 2 / 255.0 &&
            metrics["maxAlphaError"].toDouble() <= 1 / 255.0 &&
            metrics["visibleChangedPixelIoU"].toDouble() >= .98 &&
            !metrics["emptyCandidateFailure"].toBool();
        metrics["wetCandidatePassed"] = passed;
        if (!passed)
          failedHardCases.append(name);
      }
      if (maskCase) {
        metrics["maskedAcquisitionInputsValidated"] = true;
        metrics["sourceParityQualified"] = false;
      }
      if (sensorCase) {
        metrics["sensorAcquisitionInputsValidated"] = true;
        metrics["candidateInputConvention"] =
            "delivered Qt tilt degrees / 60; barrel degrees to radians; "
            "delivered relative milliseconds to seconds (native application "
            "convention)";
        metrics["sensorSourceParityQualified"] = false;
      }
      if (fixtures["group"].toString() == "wetdrift") {
        const auto spec = source["driftProbe"].toObject();
        const int count = spec["nominalDabs"].toInt();
        bool passed = stroke.dabCount() == size_t(count);
        if (spec["calibration"].toBool())
          passed = passed && metrics["maxAlphaError"].toDouble() == 0 &&
                   metrics["maxRGBError"].toDouble() == 0;
        else
          passed = passed && metrics["premultipliedRGBMAE"].toDouble() <= 3 / 255.0 &&
                   metrics["alphaMAE"].toDouble() <= 2 / 255.0 &&
                   metrics["deltaE00BlackMean"].toDouble() <= 3 &&
                   metrics["deltaE00WhiteMean"].toDouble() <= 3 &&
                   metrics["deltaE00BlackP95"].toDouble() <= 6 &&
                   metrics["deltaE00WhiteP95"].toDouble() <= 6;
        metrics["driftCheckpoint"] = count;
        metrics["driftModel"] = spec["model"];
        metrics["driftCheckpointPassed"] = passed;
        metrics["sourceCountQualification"] =
            "Observable deposits and dry path controls; internal source dab counter unavailable";
        if (!passed && !failedHardCases.contains(name)) failedHardCases.append(name);
      }
      metrics["renderMilliseconds"] = elapsed;
      metrics["dabs"] = double(stroke.dabCount());
      metrics["diagnostics"] =
          QJsonArray::fromStringList(imported.records.front().diagnostics);
      results.append(metrics);
    }
    require(remaining.empty() && results.size() == expected.size(),
            "Incomplete comparison workload");
    require((fixtures["group"].toString() != "gihstatistics" &&
             fixtures["group"].toString() != "gihlabels" &&
             !fixtures["group"].toString().startsWith("gih-correlation-")) ||
                stochasticReplicates.size() == 100,
            "Incomplete independent stochastic candidate workload");
    if (fixtures["group"].toString() == "wetdrift") {
      for (const QString &model : {QString("legacy-smear"), QString("legacy-dulling"),
                                   QString("new-smear"), QString("new-dulling")}) {
        double thousand = -1, tenThousand = -1;
        for (const auto &value : results) {
          const auto row = value.toObject();
          if (row["driftModel"].toString() != model) continue;
          if (row["driftCheckpoint"].toInt() == 1000) thousand = row["premultipliedRGBMAE"].toDouble();
          if (row["driftCheckpoint"].toInt() == 10000) tenThousand = row["premultipliedRGBMAE"].toDouble();
        }
        if (thousand < 0 || tenThousand < 0 || tenThousand > thousand + 1 / 255.0)
          failedHardCases.append("Source drift growth: " + model);
      }
    }
    QFile report(root.filePath("output/comparison.json"));
    require(report.open(QIODevice::WriteOnly),
            "Cannot write comparison report");
    report.write(
        QJsonDocument(
            QJsonObject{{"status", "measured-conversions-not-qualified"},
                        {"expectedSourceEnvelopeRejections", expectedRejections},
                        {"records", results}})
            .toJson());
    std::cout << "Recorded " << results.size() << " candidate outcomes ("
              << expectedRejections << " explicit source-envelope rejections); "
              << "source parity is not granted.\n";
    require(failedHardCases.empty(),
            "Experimental K5341 source-candidate fidelity gates failed: " +
                failedHardCases.join(", "));
  } catch (const std::exception &e) {
    std::cerr << "::error::Brush comparison failed: " << e.what() << '\n';
    return 1;
  }
}
