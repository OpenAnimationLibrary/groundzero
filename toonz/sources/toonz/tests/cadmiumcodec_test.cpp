#include "cadmiumcodec.h"

#include <QCoreApplication>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
QImage decode(const QString &uri) {
  const QString prefix = QStringLiteral("data:image/png;base64,");
  require(uri.startsWith(prefix), "PNG data URI prefix");
  return QImage::fromData(QByteArray::fromBase64(uri.mid(prefix.size()).toLatin1()), "PNG")
      .convertToFormat(QImage::Format_RGBA8888);
}
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    Cadmium::Frame frame;
    frame.lines = QImage(257, 256, QImage::Format_RGBA8888);
    frame.lines.fill(Qt::transparent);
    frame.regionColors.resize(65537);
    frame.labels.resize(frame.lines.width() * frame.lines.height());
    for (int i = 0; i < frame.labels.size(); ++i) frame.labels[i] = i % 65537;
    const QImage encoded = decode(Cadmium::labelUri(frame));
    require(encoded.size() == frame.lines.size(), "non-square label dimensions");
    for (int y = 0; y < encoded.height(); ++y) {
      const uchar *row = encoded.constScanLine(y);
      for (int x = 0; x < encoded.width(); ++x) {
        const uchar *rgba = row + 4 * x;
        // Simulate the actual cv2 BGRA decode followed by the serving
        // decoder's unsigned big-endian channel packing.
        const quint32 label = (quint32(rgba[2]) << 24) |
                              (quint32(rgba[1]) << 16) |
                              (quint32(rgba[0]) << 8) | rgba[3];
        require(label == quint32(frame.labels[y * encoded.width() + x]),
                "full integer labels survive PNG and BGRA transport");
      }
    }
    frame.labels[0] = -1;
    require(Cadmium::labelUri(frame).isEmpty(), "negative labels rejected");
    frame.labels[0] = frame.regionColors.size();
    require(Cadmium::labelUri(frame).isEmpty(), "labels outside palette rejected");
    frame.labels.removeLast();
    require(Cadmium::labelUri(frame).isEmpty(), "truncated label map rejected");
    frame.labels.clear(); frame.lines = QImage();
    require(Cadmium::labelUri(frame).isEmpty(), "empty map rejected");
    require(Cadmium::imageUri(QImage()).isEmpty(), "empty line image rejected");
    QImage lines(3, 1, QImage::Format_ARGB32_Premultiplied);
    lines.fill(Qt::transparent);
    lines.setPixel(1, 0, qPremultiply(qRgba(60, 120, 180, 128)));
    lines.setPixel(2, 0, qRgb(0, 0, 0));
    const QImage canonical = decode(Cadmium::lineImageUri(lines));
    const uchar *pixels = canonical.constScanLine(0);
    require(pixels[0] == 255 && pixels[1] == 255 && pixels[2] == 255 && pixels[3] == 0,
            "transparent line background is white with zero coverage");
    require(pixels[7] == 128 && std::abs(int(pixels[4]) - 60) <= 1 &&
                std::abs(int(pixels[5]) - 120) <= 1 &&
                std::abs(int(pixels[6]) - 180) <= 1,
            "line color conversion unpremultiplies while preserving coverage");
    require(pixels[8] == 0 && pixels[9] == 0 && pixels[10] == 0 && pixels[11] == 255,
            "opaque ink remains black");
    require(lines.pixel(0, 0) == 0, "canonicalization does not mutate input");
    require(Cadmium::lineImageUri(QImage()).isEmpty(), "empty canonical line rejected");
    std::cout << "cadmiumcodec passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
