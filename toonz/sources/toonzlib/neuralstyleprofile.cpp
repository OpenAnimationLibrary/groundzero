#include "toonz/neuralstyle.h"
#include "toonz/colormatch.h"
#include <QBuffer>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <cmath>

namespace NeuralStyle {
namespace {
bool validCrop(const QRectF &crop) {
  return std::isfinite(crop.x()) && std::isfinite(crop.y()) &&
         std::isfinite(crop.width()) && std::isfinite(crop.height()) &&
         crop.x() >= 0 && crop.y() >= 0 && crop.width() > 0 && crop.height() > 0 &&
         crop.right() <= 1 && crop.bottom() <= 1;
}
QRect fit(const QSize &size, int side) {
  const QSize raw = size.scaled(side, side, Qt::KeepAspectRatio);
  const QSize scaled(std::max(1,raw.width()),std::max(1,raw.height()));
  return QRect(QPoint((side-scaled.width())/2, (side-scaled.height())/2), scaled);
}
bool nameValid(const QString &name) {
  return name.size() <= 128 && !name.contains('/') && !name.contains('\\') && !name.contains(':');
}
bool digestValid(const QString &digest) {
  if (digest.isEmpty()) return true;
  if (digest.size() != 64) return false;
  for (const auto c : digest) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}
}

bool capture(const QImage &reference, const QImage &mask, const QRectF &crop,
             Profile &profile, QString &error) {
  if (reference.isNull() || reference.width() > 1024 || reference.height() > 1024 ||
      (!mask.isNull() && mask.size() != reference.size()) || !validCrop(crop)) {
    error = "Use a bounded reference, a matching mask and a crop inside the image.";
    return false;
  }
  const QRect pixels(int(std::floor(crop.x()*reference.width())),
                     int(std::floor(crop.y()*reference.height())),
                     std::max(1, int(std::floor(crop.width()*reference.width()))),
                     std::max(1, int(std::floor(crop.height()*reference.height()))));
  const auto rgba = reference.copy(pixels).convertToFormat(QImage::Format_ARGB32);
  const auto selection = mask.isNull() ? QImage() : mask.copy(pixels).convertToFormat(QImage::Format_ARGB32);
  QImage opaque(rgba.size(), QImage::Format_RGB32);
  bool included = false;
  for (int y=0; y<rgba.height(); ++y) {
    const auto row = reinterpret_cast<const QRgb *>(rgba.constScanLine(y));
    const auto selected = selection.isNull() ? nullptr : reinterpret_cast<const QRgb *>(selection.constScanLine(y));
    auto out = reinterpret_cast<QRgb *>(opaque.scanLine(y));
    for (int x=0; x<rgba.width(); ++x) {
      double coverage = qAlpha(row[x])/255.0;
      if (selected) coverage *= qAlpha(selected[x])/255.0 *
          (.2126*qRed(selected[x])+.7152*qGreen(selected[x])+.0722*qBlue(selected[x]))/255.0;
      included = included || coverage > 0;
      const auto channel = [coverage](int value) { return int(255+(value-255)*coverage+.5); };
      out[x] = qRgb(channel(qRed(row[x])),channel(qGreen(row[x])),channel(qBlue(row[x])));
    }
  }
  if (!included) { error = "The reference crop and mask contain no visible pixels."; return false; }
  Profile complete = profile;
  complete.reference = QImage(256, 256, QImage::Format_RGB32);
  complete.reference.fill(Qt::white);
  {
    QPainter painter(&complete.reference);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(fit(opaque.size(), 256), opaque);
  }
  complete.crop = crop;
  complete.sampleDigest = ColorMatch::sampleDigest(reference);
  complete.maskDigest = ColorMatch::sampleDigest(mask);
  profile = complete; error.clear(); return true;
}

QString serialize(const Profile &profile) {
  if (profile.reference.size() != QSize(256,256)) return {};
  QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
  if (!profile.reference.save(&buffer,"PNG")) return {};
  const QJsonObject object{{"version",1},{"model","google-magenta-int8-v1"},
      {"preprocessing","srgb-white-letterbox-v1"},
      {"reference",QString::fromLatin1(png.toBase64())},
      {"crop",QJsonArray{profile.crop.x(),profile.crop.y(),profile.crop.width(),profile.crop.height()}},
      {"sampleName",profile.sampleName},{"sampleDigest",profile.sampleDigest},
      {"maskName",profile.maskName},{"maskDigest",profile.maskDigest}};
  return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}
bool parse(const QString &text, Profile &profile, QString &error) {
  if (text.size() > 400000) { error = "Neural profile exceeds the size limit."; return false; }
  const auto object = QJsonDocument::fromJson(text.toUtf8()).object();
  if (object.value("version").toInt() != 1 || object.value("model").toString() != "google-magenta-int8-v1" ||
      object.value("preprocessing").toString() != "srgb-white-letterbox-v1") {
    error = "Unsupported neural profile version."; return false;
  }
  Profile complete;
  const auto crop = object.value("crop").toArray();
  if (crop.size() != 4) { error = "Invalid neural reference crop."; return false; }
  complete.crop = QRectF(crop.at(0).toDouble(-1),crop.at(1).toDouble(-1),
                         crop.at(2).toDouble(-1),crop.at(3).toDouble(-1));
  complete.sampleName = object.value("sampleName").toString();
  complete.sampleDigest = object.value("sampleDigest").toString();
  complete.maskName = object.value("maskName").toString();
  complete.maskDigest = object.value("maskDigest").toString();
  if (crop.size() != 4 || !validCrop(complete.crop) || !nameValid(complete.sampleName) ||
      !nameValid(complete.maskName) || !digestValid(complete.sampleDigest) || !digestValid(complete.maskDigest)) {
    error = "Invalid neural profile metadata."; return false;
  }
  QByteArray png = QByteArray::fromBase64(object.value("reference").toString().toLatin1());
  QBuffer buffer(&png); buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer,"PNG");
  if (reader.size() != QSize(256,256)) { error = "Invalid captured neural reference dimensions."; return false; }
  complete.reference = reader.read();
  if (complete.reference.isNull()) { error = "Cannot decode the captured neural reference."; return false; }
  complete.reference = complete.reference.convertToFormat(QImage::Format_ARGB32);
  for (int y=0; y<256; ++y) {
    const auto row = reinterpret_cast<const QRgb *>(complete.reference.constScanLine(y));
    for (int x=0; x<256; ++x) if (qAlpha(row[x]) != 255) {
      error = "Captured neural reference must be opaque."; return false;
    }
  }
  complete.reference = complete.reference.convertToFormat(QImage::Format_RGB32);
  profile = complete; error.clear(); return true;
}
QImage prepare(const QImage &source, QRect &area, QString &error) {
  if (source.isNull() || qint64(source.width())*source.height() > 16777216) {
    error = "Neural source exceeds the 16 megapixel input limit."; return {};
  }
  QImage result(384,384,QImage::Format_RGB32); result.fill(Qt::white);
  area = fit(source.size(),384);
  QPainter painter(&result); painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.drawImage(area,source); painter.end();
  error.clear(); return result;
}
QImage restoreSize(const QImage &styled, const QSize &size) {
  if (styled.size()!=QSize(384,384) || size.isEmpty() || qint64(size.width())*size.height()>16777216) return {};
  return styled.copy(fit(size,384)).scaled(size,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
}
}  // namespace NeuralStyle
