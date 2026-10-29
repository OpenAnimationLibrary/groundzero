#include "toonz/colormatch.h"
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QLocale>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace ColorMatch {
namespace {
double clamp(double x) { return std::max(0.0, std::min(1.0, x)); }
double linear(double x) {
  x = clamp(x);
  return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4);
}
double encoded(double x) {
  return clamp(x <= .0031308 ? 12.92 * x
                            : 1.055 * std::pow(x, 1.0 / 2.4) - .055);
}
Color lab(const Color &c) {
  double r = linear(c[0]), g = linear(c[1]), b = linear(c[2]);
  double l = std::cbrt(.4122214708*r + .5363325363*g + .0514459929*b);
  double m = std::cbrt(.2119034982*r + .6806995451*g + .1073969566*b);
  double s = std::cbrt(.0883024619*r + .2817188376*g + .6299787005*b);
  return {{.2104542553*l + .7936177850*m - .0040720468*s,
           1.9779984951*l - 2.4285922050*m + .4505937099*s,
           .0259040371*l + .7827717662*m - .8086757660*s}};
}
Color rgb(const Color &c) {
  double l = c[0] + .3963377774*c[1] + .2158037573*c[2];
  double m = c[0] - .1055613458*c[1] - .0638541728*c[2];
  double s = c[0] - .0894841775*c[1] - 1.2914855480*c[2];
  l *= l*l; m *= m*m; s *= s*s;
  return {{encoded(4.0767416621*l - 3.3077115913*m + .2309699292*s),
           encoded(-1.2684380046*l + 2.6097574011*m - .3413193965*s),
           encoded(-.0041960863*l - .7034186147*m + 1.7076147010*s)}};
}
QJsonArray array(const Color &c) { return QJsonArray{c[0], c[1], c[2]}; }
QJsonArray rectangle(const QRectF &r) {
  return QJsonArray{r.x(),r.y(),r.width(),r.height()};
}
bool readRectangle(const QJsonValue &value,QRectF &rectangle) {
  if(value.isUndefined()) return true;
  if(!value.isArray() || value.toArray().size()!=4) return false;
  const auto values=value.toArray();
  for(const auto &v:values) if(!v.isDouble() || !std::isfinite(v.toDouble()))return false;
  QRectF next(values[0].toDouble(),values[1].toDouble(),values[2].toDouble(),values[3].toDouble());
  if(next.isEmpty() || !QRectF(0,0,1,1).contains(next))return false;
  rectangle=next;return true;
}
bool read(const QJsonValue &value, Color &c, bool deviation) {
  if (!value.isArray() || value.toArray().size() != 3) return false;
  const QJsonArray values = value.toArray();
  for (int i = 0; i < 3; ++i) {
    const QJsonValue v = values[i];
    if (!v.isDouble() || !std::isfinite(v.toDouble())) return false;
    c[i] = v.toDouble();
    if (c[i] < (deviation ? 0 : -1) || c[i] > 1) return false;
  }
  return true;
}
}

Color transform(const Color &input, const Profile &p) {
  if (p.strength <= 0) return input;
  Color value = lab(input);
  for (int i = p.preserveLightness ? 1 : 0; i < 3; ++i) {
    // Flat source colors use a mean shift; never amplify noise without bound.
    double scale = p.source.deviation[i] < 1e-5 ? 1 :
        std::max(.25, std::min(4.0, p.reference.deviation[i] /
                                      p.source.deviation[i]));
    value[i] = (value[i] - p.source.mean[i]) * scale + p.reference.mean[i];
  }
  value = rgb(value);
  for (int i = 0; i < 3; ++i)
    value[i] = input[i] + clamp(p.strength) * (value[i] - input[i]);
  return value;
}

bool analyze(const QImage &image, const QImage &mask, const QRectF &crop,
             Statistics &result, QString &error, const std::atomic_bool *cancel) {
  if (image.isNull() || image.width() > 1024 || image.height() > 1024 ||
      (!mask.isNull() && mask.size() != image.size()) ||
      !QRectF(0, 0, 1, 1).contains(crop) || crop.isEmpty()) {
    error = QStringLiteral("Invalid image, mask size, or crop."); return false;
  }
  QImage src = image.convertToFormat(QImage::Format_ARGB32);
  QImage coverage = mask.convertToFormat(QImage::Format_ARGB32);
  Statistics next;
  Color m2{{0,0,0}};
  double weight = 0;
  for (int y = 0; y < src.height(); ++y) {
    if (cancel && cancel->load()) { error = QStringLiteral("Cancelled."); return false; }
    if ((y + .5) / src.height() < crop.top() ||
        (y + .5) / src.height() >= crop.bottom()) continue;
    const QRgb *row = reinterpret_cast<const QRgb *>(src.constScanLine(y));
    const QRgb *mr = coverage.isNull() ? nullptr :
        reinterpret_cast<const QRgb *>(coverage.constScanLine(y));
    for (int x = 0; x < src.width(); ++x) {
      if ((x + .5) / src.width() < crop.left() ||
          (x + .5) / src.width() >= crop.right()) continue;
      double w = qAlpha(row[x]) / 255.0;
      if (mr) w *= qAlpha(mr[x]) / 255.0 * qGray(mr[x]) / 255.0;
      if (w <= 0) continue;
      Color c = lab({{qRed(row[x])/255.0, qGreen(row[x])/255.0,
                      qBlue(row[x])/255.0}});
      weight += w;
      for (int i = 0; i < 3; ++i) {
        double delta = c[i] - next.mean[i];
        next.mean[i] += w / weight * delta;
        m2[i] += w * delta * (c[i] - next.mean[i]);
      }
    }
  }
  if (weight < 1e-6) { error = QStringLiteral("The crop/mask contains no visible samples."); return false; }
  for (int i = 0; i < 3; ++i) next.deviation[i] = std::sqrt(std::max(0.0, m2[i]/weight));
  result = next;
  return true;
}

QString serialize(const Profile &p) {
  QJsonObject o{{"version", 1}, {"space", "srgb-oklab"},
                {"sourceMean", array(p.source.mean)},
                {"sourceDeviation", array(p.source.deviation)},
                {"referenceMean", array(p.reference.mean)},
                {"referenceDeviation", array(p.reference.deviation)},
                {"sourceCrop", rectangle(p.sourceCrop)},
                {"referenceCrop", rectangle(p.referenceCrop)},
                {"preserveLightness", p.preserveLightness}, {"strength", p.strength}};
  return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
bool parse(const QString &text, Profile &p, QString &error) {
  if (text.size() > 4096) { error = QStringLiteral("Profile exceeds 4 KiB."); return false; }
  auto d = QJsonDocument::fromJson(text.toUtf8());
  auto o = d.object();
  Profile next;
  if (!d.isObject() || o.value("version").toDouble(-1) != 1 ||
      o.value("space").toString() != "srgb-oklab" ||
      !read(o.value("sourceMean"), next.source.mean, false) ||
      !read(o.value("sourceDeviation"), next.source.deviation, true) ||
      !read(o.value("referenceMean"), next.reference.mean, false) ||
      !read(o.value("referenceDeviation"), next.reference.deviation, true) ||
      !readRectangle(o.value("sourceCrop"), next.sourceCrop) ||
      !readRectangle(o.value("referenceCrop"), next.referenceCrop) ||
      !o.value("preserveLightness").isBool() ||
      !o.value("strength").isDouble() ||
      !std::isfinite(o.value("strength").toDouble()) ||
      o.value("strength").toDouble() < 0 || o.value("strength").toDouble() > 1) {
    error = QStringLiteral("Invalid or unsupported reference-color profile."); return false;
  }
  next.strength = o.value("strength").toDouble();
  next.preserveLightness = o.value("preserveLightness").toBool();
  p = next;
  return true;
}
QImage preview(const QImage &image, const Profile &p, const std::atomic_bool *cancel) {
  if (image.width() > 1024 || image.height() > 1024) return {};
  QImage output = image.convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < output.height(); ++y) {
    if (cancel && cancel->load()) return {};
    QRgb *row = reinterpret_cast<QRgb *>(output.scanLine(y));
    for (int x = 0; x < output.width(); ++x) {
      const auto c = row[x];
      if (qAlpha(c) == 0 || p.strength == 0) continue;
      auto v = transform({{qRed(c)/255.0, qGreen(c)/255.0, qBlue(c)/255.0}}, p);
      row[x] = qRgba(int(v[0]*255+.5), int(v[1]*255+.5), int(v[2]*255+.5), qAlpha(c));
    }
  }
  return output;
}
QImage loadSample(const QString &path, QString &error) {
  QImageReader reader(path);
  reader.setAutoTransform(true);
  QSize size = reader.size();
  if (!size.isValid() || qint64(size.width()) * size.height() > 16777216) {
    error = QStringLiteral("Use a local raster image of at most 16 megapixels."); return {};
  }
  reader.setScaledSize(size.scaled(1024, 1024, Qt::KeepAspectRatio));
  auto result = reader.read();
  if (result.isNull()) error = reader.errorString();
  if (result.width() > 1024 || result.height() > 1024)
    result = result.scaled(1024,1024,Qt::KeepAspectRatio,Qt::SmoothTransformation);
  return result;
}
bool exportCube(const QString &path, const Profile &p, QString &error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) { error=file.errorString(); return false; }
  QTextStream stream(&file);
  stream.setLocale(QLocale::c());
  stream << "TITLE \"Groundzero Reference Color Match SDR sRGB\"\nLUT_3D_SIZE 33\n";
  for (int b=0;b<33;++b) for(int g=0;g<33;++g) for(int r=0;r<33;++r) {
    auto v=transform({{r/32.0,g/32.0,b/32.0}},p);
    stream << v[0] << ' ' << v[1] << ' ' << v[2] << '\n';
  }
  stream.flush();
  if (stream.status()!=QTextStream::Ok || !file.commit()) { error=file.errorString(); return false; }
  return true;
}
} // namespace ColorMatch
