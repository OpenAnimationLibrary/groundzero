#include "automation_intake.h"

#include "tcolorstyles.h"
#include "tiio.h"
#include "tlevel_io.h"
#include "tpalette.h"
#include "tproperty.h"
#include "tsimplecolorstyles.h"
#include "tstroke.h"
#include "tvectorimage.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <random>
#include <stdexcept>
#include <string>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error("intake: " + message);
}

quint32 crc32(const QByteArray& bytes) {
  quint32 crc = 0xffffffffu;
  for (const char byte : bytes) {
    crc ^= static_cast<unsigned char>(byte);
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

QByteArray bigEndian(quint32 value) {
  QByteArray bytes(4, '\0');
  for (int index = 0; index < 4; ++index)
    bytes[index] = char((value >> (24 - 8 * index)) & 0xff);
  return bytes;
}

QByteArray chunk(const QByteArray& type, const QByteArray& data) {
  return bigEndian(quint32(data.size())) + type + data +
         bigEndian(crc32(type + data));
}

QByteArray encoded(const QImage& image) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  require(image.save(&buffer, "PNG"), "Qt cannot encode a fixture PNG");
  return bytes;
}

// Put a chunk right after IHDR (offset 33), where ancillary chunks may go.
QByteArray withChunk(const QByteArray& png, const QByteArray& type,
                     const QByteArray& data) {
  return png.left(33) + chunk(type, data) + png.mid(33);
}

QImage decoded(const QByteArray& png) {
  QImage image;
  require(image.loadFromData(png, "PNG"), "Qt cannot decode a fixture PNG");
  return image.convertToFormat(QImage::Format_ARGB32);
}

QImage gradient(QImage::Format format) {
  QImage image(5, 3, QImage::Format_ARGB32);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 5; ++x)
      image.setPixel(x, y, qRgba(40 * x, 80 * y, 200, 255 - 50 * x));
  return image.convertToFormat(format);
}

void testPixelFormats() {
  const struct {
    QImage::Format format;
    const char* pixel;
    const char* alpha;
  } cases[] = {{QImage::Format_ARGB32, "rgba8", "channel"},
               {QImage::Format_RGB32, "rgb8", "none"},
               {QImage::Format_Grayscale8, "grey8", "none"},
               {QImage::Format_Grayscale16, "grey16", "none"},
               {QImage::Format_RGBA64, "rgba16", "channel"},
               {QImage::Format_Indexed8, "indexed8", "none"}};
  for (const auto& test : cases) {
    QImage image = gradient(test.format);
    if (test.format == QImage::Format_Indexed8)
      image =
          gradient(QImage::Format_RGB32)
              .convertToFormat(QImage::Format_Indexed8, Qt::ThresholdDither);
    const ImageHeader header = readImageHeader(encoded(image), "a.png");
    require(header.valid && header.width == 5 && header.height == 3,
            std::string("size of ") + test.pixel);
    require(header.pixelFormat() == test.pixel,
            std::string("pixel format ") + test.pixel + " read as " +
                header.pixelFormat().toStdString());
    require(header.alpha == test.alpha || QString(test.pixel) == "indexed8",
            std::string("alpha of ") + test.pixel);
  }
}

void testAncillaryChunks() {
  QImage image = gradient(QImage::Format_RGB32);
  image.setDotsPerMeterX(11811);
  image.setDotsPerMeterY(5906);
  const QByteArray png     = encoded(image);
  const ImageHeader header = readImageHeader(png, "a.png");
  require(header.dpiKnown && qAbs(header.dpiX - 299.9994) < 1e-9 &&
              qAbs(header.dpiY - 150.0124) < 1e-9,
          "pHYs density in dots per inch");
  const QByteArray gamma = withChunk(png, "gAMA", bigEndian(100000));
  const ImageHeader tagged =
      readImageHeader(withChunk(gamma, "tRNS", QByteArray(6, '\0')), "a.png");
  require(tagged.colorMetadata == QStringList{"gamma"}, "gAMA reported");
  require(tagged.alpha == "transparency_chunk", "tRNS reported as alpha");
  // Probe IP-2: the decoder applies no gamma, so the pixels are the file's.
  require(decoded(gamma) == decoded(png), "gAMA changes no decoded pixel");
  // Probe IP-2: a tRNS key colour decodes as transparent.
  QImage black(2, 1, QImage::Format_RGB32);
  black.fill(Qt::black);
  black.setPixel(1, 0, qRgb(255, 255, 255));
  const QImage keyed =
      decoded(withChunk(encoded(black), "tRNS", QByteArray(6, '\0')));
  require(qAlpha(keyed.pixel(0, 0)) == 0 && qAlpha(keyed.pixel(1, 0)) == 255,
          "tRNS key colour decodes as transparent");
}

void testMalformedHeaders() {
  const QByteArray png = encoded(gradient(QImage::Format_ARGB32));
  require(readImageHeader(png.left(20), "a.png").problem == "missing_ihdr",
          "truncated header");
  QByteArray depth = png;
  depth[24]        = 3;
  require(readImageHeader(depth, "a.png").problem == "invalid_ihdr",
          "depth 3 is not a PNG depth");
  std::mt19937 random(20261119);
  std::uniform_int_distribution<int> offset(0, 64), byte(0, 255);
  for (int round = 0; round < 4000; ++round) {
    QByteArray mutated = png;
    for (int count = 0; count < 3; ++count)
      mutated[offset(random) % mutated.size()] = char(byte(random));
    const ImageHeader header =
        readImageHeader(mutated.left(offset(random) + 8), "a.png");
    require(!header.valid || (header.width > 0 && header.height > 0 &&
                              !header.pixelFormat().isEmpty()),
            "a mutated header is either refused or well formed");
  }
}

void testSignatures() {
  const struct {
    QByteArray head;
    const char* name;
    const char* format;
  } cases[] = {
      {QByteArray::fromHex("ffd8ffe000104a464946"), "a.jpg", "jpeg"},
      {QByteArray::fromHex("49492a0008000000"), "a.tif", "tiff"},
      {QByteArray::fromHex("4d4d002a00000008"), "a.tif", "tiff"},
      {"GIF89a", "a.gif", "gif"},
      {"BMF", "a.bmp", "bmp"},
      {QByteArray::fromHex("524946461000000057454250"), "a.webp", "webp"},
      {"8BPS", "a.psd", "psd"},
      {QByteArray::fromHex("762f310102000000"), "a.exr", "exr"},
      {QByteArray::fromHex("0000001c667479706d703432"), "a.mp4", "mp4"},
      {"<?xml version=\"1.0\"?>\n<svg xmlns=\"x\"/>", "a.svg", "svg"},
      {"anything", "a.tga", "tga"},
      {"anything", "a.dat", "unknown"}};
  for (const auto& test : cases)
    require(readImageHeader(test.head, test.name).format == test.format,
            std::string("signature of ") + test.format);
}

void testSequenceNames() {
  require(parseSequenceName("shot.0007.png", "shot", "png").number == 7,
          "padded frame");
  require(parseSequenceName("shot.0007.png", "shot", "png").digits == 4,
          "padding width");
  require(parseSequenceName("shot.12.png", "shot", "png").matches,
          "short frame");
  for (const char* refused :
       {"shot.png", "shot..png", "shot.0000.png", "shot.1234567.png",
        "shot.12a.png", "shot.0001.PNG", "shots.0001.png", "shot_0001.png"})
    require(!parseSequenceName(refused, "shot", "png").matches,
            std::string("not a member: ") + refused);
  require(parseSequenceName("a.b.0003.png", "a.b", "png").number == 3,
          "a dotted prefix");
}

// Probe IP-1: which paddings the native dotted level path names exactly.
void testNativeSequencePath() {
  const TFilePath level(L"/sources/shot..png");
  const struct {
    int number;
    int padding;
    const char* name;
  } cases[] = {{7, 4, "shot.0007.png"},
               {12, 4, "shot.0012.png"},
               {12345, 4, "shot.12345.png"},
               {7, 3, "shot.007.png"},
               {7, 6, "shot.000007.png"}};
  for (const auto& test : cases) {
    const QString name = QString::fromStdWString(
        level.withFrame(TFrameId(test.number, QString(), test.padding))
            .withoutParentDir()
            .getWideString());
    require(name == test.name, std::string("native frame name ") + test.name +
                                   " came out as " + name.toStdString());
  }
}

// Probe IP-3: the native SVG writer on a solid-style drawing writes vector
// paths only, without an OpenGL context.
void testSvgWriter() {
  TPaletteP palette = new TPalette();
  const int style   = palette->getPage(0)->addStyle(TPixel32(10, 20, 30, 255));
  TVectorImageP image = new TVectorImage();
  image->setPalette(palette.getPointer());
  std::vector<TThickPoint> points{TThickPoint(0, 0, 2), TThickPoint(5, 5, 2),
                                  TThickPoint(10, 0.25, 2)};
  TStroke* stroke = new TStroke(points);
  stroke->setStyle(style);
  image->addStroke(stroke);
  QTemporaryDir directory;
  require(directory.isValid(), "temporary directory");
  const QString path = directory.filePath("probe.svg");
  {
    std::unique_ptr<TPropertyGroup> properties(
        Tiio::makeWriterProperties("svg"));
    dynamic_cast<TEnumProperty*>(properties->getProperty("Stroke Mode"))
        ->setValue(L"Outline");
    TLevelWriterP writer(TFilePath(path.toStdWString()), properties.get());
    writer->getFrameWriter(TFrameId(TFrameId::NO_FRAME))->save(TImageP(image));
  }
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "the SVG writer wrote no file");
  const QByteArray text = file.readAll();
  require(text.contains("<path") && !text.contains("<image"),
          "solid strokes are written as vector paths");
  require(text.contains("fill:rgb(10,20,30)"), "the style colour is kept");
  const QString data = QString::fromUtf8(text.mid(text.indexOf("d=\"")));
  require(data.contains(QRegularExpression("\\d\\.\\d")) &&
              !data.contains(QRegularExpression("\\.\\d\\d")),
          "path coordinates are written with one decimal");
}

}  // namespace

void runAutomationIntakeTests(Context&) {
  testPixelFormats();
  testAncillaryChunks();
  testMalformedHeaders();
  testSignatures();
  testSequenceNames();
  testNativeSequencePath();
  testSvgWriter();
}

}  // namespace GroundzeroAutomation
