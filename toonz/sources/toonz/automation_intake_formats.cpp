#include "automation_intake.h"

#include "timage_io.h"
#include "timageinfo.h"
#include "trasterimage.h"
#include "trop.h"
#include "turbojpeg.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QtEndian>

#include <map>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxChunks         = 1024;
constexpr double InchesPerMeter = 0.0254;
constexpr int MaxTiffEntries    = 1024;
constexpr int MaxSide           = 8192;
constexpr qint64 MaxPixels      = 16777216;

quint32 bigEndian(const QByteArray& bytes, int at) {
  const auto byte = [&](int index) {
    return quint32(static_cast<unsigned char>(bytes.at(at + index)));
  };
  return byte(0) << 24 | byte(1) << 16 | byte(2) << 8 | byte(3);
}

bool startsWithHex(const QByteArray& bytes, const char* hex) {
  return bytes.startsWith(QByteArray::fromHex(hex));
}

bool looksLikeSvg(const QByteArray& head) {
  const QByteArray text = head.left(1024).trimmed();
  return (text.startsWith("<?xml") || text.startsWith("<svg")) &&
         text.contains("<svg");
}

QString containerFormat(const QByteArray& head) {
  if (startsWithHex(head, "ffd8ff")) return "jpeg";
  for (const char* tiff : {"49492a00", "4d4d002a", "49492b00", "4d4d002b"})
    if (startsWithHex(head, tiff)) return "tiff";
  if (head.startsWith("GIF87a") || head.startsWith("GIF89a")) return "gif";
  if (head.startsWith("BM")) return "bmp";
  if (head.startsWith("RIFF") && head.mid(8, 4) == "WEBP") return "webp";
  if (head.startsWith("RIFF") && head.mid(8, 4) == "AVI ") return "avi";
  if (head.startsWith("8BPS")) return "psd";
  if (startsWithHex(head, "762f3101")) return "exr";
  if (head.mid(4, 4) == "ftyp") return "mp4";
  if (looksLikeSvg(head)) return "svg";
  return {};
}

QString extensionFormat(const QString& name) {
  const QString suffix = QFileInfo(name).suffix().toLower();
  for (const char* known : {"tga", "pli", "tlv", "tpl", "tzp", "tzl"})
    if (suffix == QLatin1String(known)) return suffix;
  return "unknown";
}

bool validDepth(int colorType, int depth) {
  switch (colorType) {
  case 0:
    return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
  case 3:
    return depth == 1 || depth == 2 || depth == 4 || depth == 8;
  case 2:
  case 4:
  case 6:
    return depth == 8 || depth == 16;
  default:
    return false;
  }
}

void readPhysicalDensity(ImageHeader& header, const QByteArray& data) {
  if (data.size() != 9 || static_cast<unsigned char>(data.at(8)) != 1) return;
  const quint32 x = bigEndian(data, 0), y = bigEndian(data, 4);
  if (x == 0 || y == 0) return;
  header.dpiKnown = true;
  header.dpiX     = x * InchesPerMeter;
  header.dpiY     = y * InchesPerMeter;
}

void readChunk(ImageHeader& header, const QByteArray& type,
               const QByteArray& data) {
  if (type == "pHYs") readPhysicalDensity(header, data);
  if (type == "tRNS" && header.alpha == "none")
    header.alpha = "transparency_chunk";
  const std::map<QByteArray, QString> color{{"gAMA", "gamma"},
                                            {"sRGB", "srgb"},
                                            {"iCCP", "icc_profile"},
                                            {"cHRM", "chromaticities"}};
  const auto found = color.find(type);
  if (found != color.end() && !header.colorMetadata.contains(found->second))
    header.colorMetadata.append(found->second);
}

// The chunks after IHDR and before the first IDAT, within the read bytes.
void readAncillaryChunks(ImageHeader& header, const QByteArray& head) {
  int offset = 33;
  for (int count = 0; count < MaxChunks && offset + 8 <= head.size(); ++count) {
    const quint32 length  = bigEndian(head, offset);
    const QByteArray type = head.mid(offset + 4, 4);
    if (type == "IDAT" || type == "IEND" || length > quint32(IntakeHeaderBytes))
      return;
    if (offset + 12 + qint64(length) > head.size()) return;
    readChunk(header, type, head.mid(offset + 8, int(length)));
    offset += 12 + int(length);
  }
}

bool validDimension(quint32 value) { return value > 0 && value <= 0x7fffffff; }

// Whether the 13 IHDR bytes at offset 16 hold a size, depth, colour type,
// compression, filter and interlace method the PNG specification allows.
bool validIhdr(const QByteArray& head) {
  const int colorType = static_cast<unsigned char>(head.at(25));
  const int depth     = static_cast<unsigned char>(head.at(24));
  const bool methods  = head.at(26) == 0 && head.at(27) == 0;
  return validDimension(bigEndian(head, 16)) &&
         validDimension(bigEndian(head, 20)) && validDepth(colorType, depth) &&
         methods && static_cast<unsigned char>(head.at(28)) <= 1;
}

QString pngPixel(int colorType, int depth) {
  static const std::map<int, QString> names{
      {0, "grey"}, {2, "rgb"}, {3, "indexed"}, {4, "grey_alpha"}, {6, "rgba"}};
  return names.at(colorType) + QString::number(depth);
}

ImageHeader readPngHeader(const QByteArray& head) {
  ImageHeader header;
  header.format = "png";
  if (head.size() < 33 || bigEndian(head, 8) != 13 ||
      head.mid(12, 4) != "IHDR") {
    header.problem = "missing_ihdr";
    return header;
  }
  if (!validIhdr(head)) {
    header.problem = "invalid_ihdr";
    return header;
  }
  header.valid      = true;
  header.width      = int(bigEndian(head, 16));
  header.height     = int(bigEndian(head, 20));
  header.bitDepth   = static_cast<unsigned char>(head.at(24));
  header.colorType  = static_cast<unsigned char>(head.at(25));
  header.interlaced = head.at(28) == 1;
  header.pixel      = pngPixel(header.colorType, header.bitDepth);
  if (header.colorType == 4 || header.colorType == 6) header.alpha = "channel";
  readAncillaryChunks(header, head);
  return header;
}

// ---------------------------------------------------------------- TIFF --

struct TiffReader final {
  QIODevice& device;
  bool little = true;

  quint32 number(const QByteArray& bytes, int at, int size) const {
    if (bytes.size() < at + size) return 0;
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + at);
    if (size == 2)
      return little ? qFromLittleEndian<quint16>(data)
                    : qFromBigEndian<quint16>(data);
    return little ? qFromLittleEndian<quint32>(data)
                  : qFromBigEndian<quint32>(data);
  }
  QByteArray at(qint64 offset, int size) {
    if (offset <= 0 || !device.seek(offset)) return {};
    return device.read(size);
  }
};

struct TiffFields final {
  quint32 width = 0, height = 0, bits = 1, samples = 1, photometric = 99;
  quint32 unit = 2, planar = 1, compression = 1;
  double xres = 0, yres = 0;
  bool extra = false;
  QByteArray offsets, counts;
};

double rational(TiffReader& reader, quint32 offset) {
  const QByteArray value    = reader.at(offset, 8);
  const quint32 denominator = reader.number(value, 4, 4);
  return denominator ? double(reader.number(value, 0, 4)) / denominator : 0;
}

// The first value of a SHORT or LONG field, inline or at its offset.
quint32 tiffValue(TiffReader& reader, const QByteArray& entry) {
  const quint32 type  = reader.number(entry, 2, 2);
  const quint32 count = reader.number(entry, 4, 4);
  const int size      = type == 3 ? 2 : 4;
  if (count * quint64(size) <= 4) return reader.number(entry, 8, size);
  return reader.number(reader.at(reader.number(entry, 8, 4), size), 0, size);
}

void readTiffEntry(TiffReader& reader, const QByteArray& entry,
                   TiffFields& fields) {
  const quint32 tag    = reader.number(entry, 0, 2);
  const quint32 offset = reader.number(entry, 8, 4);
  switch (tag) {
  case 256:
    fields.width = tiffValue(reader, entry);
    break;
  case 257:
    fields.height = tiffValue(reader, entry);
    break;
  case 258:
    fields.bits = tiffValue(reader, entry);
    break;
  case 259:
    fields.compression = tiffValue(reader, entry);
    break;
  case 273:
    fields.offsets = entry;
    break;
  case 279:
    fields.counts = entry;
    break;
  case 262:
    fields.photometric = tiffValue(reader, entry);
    break;
  case 277:
    fields.samples = tiffValue(reader, entry);
    break;
  case 282:
    fields.xres = rational(reader, offset);
    break;
  case 283:
    fields.yres = rational(reader, offset);
    break;
  case 284:
    fields.planar = tiffValue(reader, entry);
    break;
  case 296:
    fields.unit = tiffValue(reader, entry);
    break;
  case 338:
    fields.extra = true;
    break;
  default:
    break;
  }
}

QString tiffPixel(const TiffFields& fields) {
  const QString bits = QString::number(fields.bits);
  if (fields.photometric <= 1)
    return (fields.samples >= 2 ? "grey_alpha" : "grey") + bits;
  if (fields.photometric == 3) return "indexed" + bits;
  if (fields.photometric == 2 || fields.photometric == 6)
    return (fields.samples >= 4 ? "rgba" : "rgb") + bits;
  return {};
}

// The values of a SHORT or LONG array field, at most 65536.
QList<quint32> tiffArray(TiffReader& reader, const QByteArray& entry) {
  const quint32 type  = reader.number(entry, 2, 2);
  const quint32 count = reader.number(entry, 4, 4);
  const int size      = type == 3 ? 2 : 4;
  QList<quint32> values;
  if (entry.isEmpty() || count == 0 || count > 65536) return values;
  const QByteArray data =
      count * quint64(size) <= 4
          ? entry.mid(8, 4)
          : reader.at(reader.number(entry, 8, 4), int(count) * size);
  for (quint32 index = 0; index < count; ++index)
    values.append(reader.number(data, int(index) * size, size));
  return values;
}

// Whether every uncompressed strip lies inside the file and the strips hold
// every pixel: the native reader fills what is missing instead of failing.
bool stripsComplete(TiffReader& reader, const TiffFields& fields) {
  const QList<quint32> offsets = tiffArray(reader, fields.offsets);
  const QList<quint32> counts  = tiffArray(reader, fields.counts);
  if (offsets.isEmpty() || offsets.size() != counts.size()) return false;
  qint64 total = 0;
  for (int index = 0; index < offsets.size(); ++index) {
    if (qint64(offsets[index]) + counts[index] > reader.device.size())
      return false;
    total += counts[index];
  }
  const qint64 rowBytes =
      (qint64(fields.width) * fields.samples * fields.bits + 7) / 8;
  return total >= rowBytes * fields.height;
}

void describeTiff(ImageHeader& header, const TiffFields& fields,
                  bool morePages) {
  header.width       = int(fields.width);
  header.height      = int(fields.height);
  header.bitDepth    = int(fields.bits);
  header.pixel       = tiffPixel(fields);
  header.alpha       = fields.extra ? "channel" : "none";
  const double scale = fields.unit == 3 ? 2.54 : 1.0;
  header.dpiKnown    = fields.unit != 1 && fields.xres > 0 && fields.yres > 0;
  header.dpiX        = fields.xres * scale;
  header.dpiY        = fields.yres * scale;
  if (morePages) header.unsupported = "multi_page";
  if (fields.photometric == 5) header.unsupported = "cmyk";
  if (header.pixel.isEmpty() && header.unsupported.isEmpty())
    header.unsupported = "photometric";
  if (fields.planar != 1) header.unsupported = "planar";
}

ImageHeader readTiffHeader(QIODevice& device, const QByteArray& head) {
  ImageHeader header;
  header.format = "tiff";
  TiffReader reader{device, head.startsWith("II")};
  if (reader.number(head, 2, 2) == 43) {
    header.problem = "bigtiff";
    return header;
  }
  const quint32 offset = reader.number(head, 4, 4);
  const quint32 count  = reader.number(reader.at(offset, 2), 0, 2);
  if (count == 0 || count > MaxTiffEntries) {
    header.problem = "invalid_ifd";
    return header;
  }
  const QByteArray entries = reader.at(offset + 2, int(count) * 12 + 4);
  if (entries.size() != int(count) * 12 + 4) {
    header.problem = "invalid_ifd";
    return header;
  }
  TiffFields fields;
  for (quint32 index = 0; index < count; ++index)
    readTiffEntry(reader, entries.mid(int(index) * 12, 12), fields);
  header.valid = fields.width > 0 && fields.height > 0 &&
                 fields.width < 0x7fffffff && fields.height < 0x7fffffff &&
                 fields.bits > 0 && fields.bits <= 32;
  if (!header.valid) {
    header.problem = "invalid_ifd";
    return header;
  }
  describeTiff(header, fields, reader.number(entries, int(count) * 12, 4));
  if (fields.compression != 1) {
    header.unsupported = "tiff_compression";
  } else if (!stripsComplete(reader, fields)) {
    header.problem = "truncated_pixels";
    header.valid   = false;
  }
  return header;
}

// ---------------------------------------------------------------- JPEG --

bool frameMarker(int marker) {
  return marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 &&
         marker != 0xcc;
}

void readJfifDensity(ImageHeader& header, const QByteArray& segment) {
  if (!segment.startsWith(QByteArray("JFIF\0", 5)) || segment.size() < 12)
    return;
  const int unit   = static_cast<uchar>(segment.at(7));
  const auto* data = reinterpret_cast<const uchar*>(segment.constData());
  const double x   = qFromBigEndian<quint16>(data + 8);
  const double y   = qFromBigEndian<quint16>(data + 10);
  if (unit == 0 || x == 0 || y == 0) return;
  header.dpiKnown = true;
  header.dpiX     = unit == 2 ? x * 2.54 : x;
  header.dpiY     = unit == 2 ? y * 2.54 : y;
}

void readJpegFrame(ImageHeader& header, const QByteArray& segment) {
  if (segment.size() < 6) return;
  const auto* data     = reinterpret_cast<const uchar*>(segment.constData());
  header.bitDepth      = data[0];
  header.height        = qFromBigEndian<quint16>(data + 1);
  header.width         = qFromBigEndian<quint16>(data + 3);
  const int components = data[5];
  header.valid         = header.width > 0 && header.height > 0;
  header.pixel         = components == 1 ? "grey" : "rgb";
  header.pixel += QString::number(header.bitDepth);
  if (components != 1 && components != 3) header.unsupported = "cmyk";
}

ImageHeader readJpegHeader(QIODevice& device, const QByteArray& head) {
  ImageHeader header;
  header.format  = "jpeg";
  header.problem = "missing_frame";
  int offset     = 2;
  while (offset + 4 <= head.size()) {
    if (static_cast<uchar>(head.at(offset)) != 0xff) break;
    const int marker = static_cast<uchar>(head.at(offset + 1));
    const int length = qFromBigEndian<quint16>(
        reinterpret_cast<const uchar*>(head.constData() + offset + 2));
    const QByteArray segment = head.mid(offset + 4, length - 2);
    if (marker == 0xe0) readJfifDensity(header, segment);
    if (frameMarker(marker)) {
      readJpegFrame(header, segment);
      if (header.valid) header.problem.clear();
      // A JPEG cut short still decodes natively, with filled pixels; only
      // the end marker shows it is whole.
      if (header.valid && (!device.seek(device.size() - 2) ||
                           device.read(2) != QByteArray("\xff\xd9", 2))) {
        header.valid   = false;
        header.problem = "truncated_data";
      }
      return header;
    }
    if (marker == 0xda || length < 2) break;
    offset += 2 + length;
  }
  return header;
}

// ----------------------------------------------------------- BMP and TGA --

qint32 littleSigned(const QByteArray& head, int at) {
  return qFromLittleEndian<qint32>(
      reinterpret_cast<const uchar*>(head.constData() + at));
}

quint16 littleShort(const QByteArray& head, int at) {
  return qFromLittleEndian<quint16>(
      reinterpret_cast<const uchar*>(head.constData() + at));
}

ImageHeader readBmpHeader(const QByteArray& head, qint64 fileSize) {
  ImageHeader header;
  header.format = "bmp";
  if (head.size() < 54 || littleSigned(head, 14) < 40) {
    header.problem = "unsupported_header";
    return header;
  }
  const qint32 width = littleSigned(head, 18), height = littleSigned(head, 22);
  const int bits = littleShort(head, 28);
  const QList<int> depths{1, 4, 8, 16, 24, 32};
  header.valid = width > 0 && height != 0 && height > -0x7fffffff &&
                 depths.contains(bits) && littleShort(head, 26) == 1;
  if (!header.valid) {
    header.problem = "invalid_header";
    return header;
  }
  header.width      = width;
  header.height     = height < 0 ? -height : height;
  header.bitDepth   = bits <= 8 ? bits : 8;
  header.pixel      = bits <= 8    ? "indexed" + QString::number(bits)
                      : bits == 16 ? QString("rgb5")
                      : bits == 24 ? QString("rgb8")
                                   : QString("rgbx8");
  const qint32 xppm = littleSigned(head, 38), yppm = littleSigned(head, 42);
  header.dpiKnown = xppm > 0 && yppm > 0;
  header.dpiX     = xppm * InchesPerMeter;
  header.dpiY     = yppm * InchesPerMeter;
  // Uncompressed pixel rows must all be in the file: the native reader fills
  // what is missing instead of failing.
  const qint64 stride = (qint64(width) * bits / 8 + 3) & ~qint64(3);
  const qint64 offset = qFromLittleEndian<quint32>(
      reinterpret_cast<const uchar*>(head.constData() + 10));
  if (littleSigned(head, 30) == 0 && offset + stride * header.height > fileSize)
    header.problem = "truncated_pixels";
  header.valid = header.problem.isEmpty();
  return header;
}

ImageHeader readTgaHeader(const QByteArray& head, qint64 fileSize) {
  ImageHeader header;
  header.format = "tga";
  if (head.size() < 18) {
    header.problem = "truncated_header";
    return header;
  }
  const int type  = static_cast<uchar>(head.at(2));
  const int depth = static_cast<uchar>(head.at(16));
  const QList<int> types{1, 2, 3, 9, 10, 11};
  const QList<int> depths{8, 15, 16, 24, 32};
  header.width  = littleShort(head, 12);
  header.height = littleShort(head, 14);
  header.valid  = types.contains(type) && depths.contains(depth) &&
                  header.width > 0 && header.height > 0;
  if (!header.valid) {
    header.problem = "invalid_header";
    return header;
  }
  const bool alpha      = (static_cast<uchar>(head.at(17)) & 0x0f) != 0;
  header.alpha          = alpha ? "channel" : "none";
  header.bitDepth       = 8;
  const int kind        = type & 7;
  header.pixel          = kind == 1   ? QString("indexed8")
                          : kind == 3 ? QString("grey8")
                          : alpha     ? QString("rgba8")
                                      : QString("rgb8");
  const qint64 colorMap = static_cast<uchar>(head.at(1)) == 1
                              ? qint64(littleShort(head, 5)) *
                                    ((static_cast<uchar>(head.at(7)) + 7) / 8)
                              : 0;
  const qint64 pixels =
      qint64(header.width) * header.height * ((depth + 7) / 8);
  if (type < 9 &&
      18 + static_cast<uchar>(head.at(0)) + colorMap + pixels > fileSize)
    header.problem = "truncated_pixels";
  header.valid = header.problem.isEmpty();
  return header;
}

// The native JPEG reader ignores libjpeg's warnings ("Corrupt JPEG data",
// "Premature end of data segment") and returns filled pixels. A second decode
// with the turbojpeg library the application already links, stopping on the
// first warning, tells a whole stream from a damaged one.
void requireWholeJpeg(const QString& path, int width, int height) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    throw Error("rejected", "The private JPEG copy cannot be read.");
  const QByteArray bytes = file.readAll();
  tjhandle decoder       = tjInitDecompress();
  if (!decoder) throw Error("rejected", "The JPEG verifier cannot start.");
  std::vector<unsigned char> pixels(size_t(width) * height * 4);
  const int result = tjDecompress2(
      decoder, reinterpret_cast<const unsigned char*>(bytes.constData()),
      static_cast<unsigned long>(bytes.size()), pixels.data(), width, 0, height,
      TJPF_RGBA, TJFLAG_STOPONWARNING);
  const QString message = QString::fromUtf8(tjGetErrorStr2(decoder));
  tjDestroy(decoder);
  if (result != 0)
    throw Error("rejected", "corrupt_data: " + message.left(200));
}

bool premultiplied(const TRaster32P& raster) {
  raster->lock();
  bool valid = true;
  for (int y = 0; valid && y < raster->getLy(); ++y) {
    const TPixel32* row = raster->pixels(y);
    for (int x = 0; x < raster->getLx(); ++x)
      valid = valid && row[x].r <= row[x].m && row[x].g <= row[x].m &&
              row[x].b <= row[x].m;
  }
  raster->unlock();
  return valid;
}

}  // namespace

QString ImageHeader::pixelFormat() const { return valid ? pixel : QString(); }

ImageHeader readImageHeader(QIODevice& device, const QString& name) {
  const QByteArray head = device.read(IntakeHeaderBytes);
  if (startsWithHex(head, "89504e470d0a1a0a")) return readPngHeader(head);
  const QString container = containerFormat(head);
  if (container == "tiff") return readTiffHeader(device, head);
  if (container == "jpeg") return readJpegHeader(device, head);
  if (container == "bmp") return readBmpHeader(head, device.size());
  const QString byName = extensionFormat(name);
  if (container.isEmpty() && byName == "tga")
    return readTgaHeader(head, device.size());
  ImageHeader header;
  header.format = container.isEmpty() ? byName : container;
  return header;
}

ImageHeader readImageHeader(const QByteArray& bytes, const QString& name) {
  QByteArray copy = bytes;
  QBuffer buffer(&copy);
  buffer.open(QIODevice::ReadOnly);
  return readImageHeader(buffer, name);
}

QString decoderExtension(const QString& format) {
  static const std::map<QString, QString> extensions{{"png", "png"},
                                                     {"tiff", "tif"},
                                                     {"jpeg", "jpg"},
                                                     {"bmp", "bmp"},
                                                     {"tga", "tga"}};
  const auto found = extensions.find(format);
  return found == extensions.end() ? QString() : found->second;
}

// Helper side of a contained decode: read <workspace>/input.<ext> with the
// native reader, write the pixels as 8-bit RGBA to decoded.png beside it.
QJsonObject intakeDecodeSummary(Context& context, const QString& extension) {
  const QStringList known{"png", "tif", "jpg", "bmp", "tga"};
  if (!known.contains(extension))
    throw Error("rejected", "Unknown image decode kind.");
  const QDir directory(context.workspace());
  TImageReaderP reader(
      TFilePath(directory.filePath("input." + extension).toStdWString()));
  const TImageInfo* info = reader ? reader->getImageInfo() : nullptr;
  if (!info) throw Error("rejected", "The native reader found no image.");
  if (info->m_lx <= 0 || info->m_ly <= 0 || info->m_lx > MaxSide ||
      info->m_ly > MaxSide || qint64(info->m_lx) * info->m_ly > MaxPixels)
    throw Error("resource_limit", "The image is outside the admitted size.");
  if (extension == "jpg")
    requireWholeJpeg(directory.filePath("input.jpg"), info->m_lx, info->m_ly);
  const TRasterImageP image = reader->load();
  if (!image || !image->getRaster())
    throw Error("rejected", "The native reader decoded no raster.");
  const TRasterP raster = image->getRaster();
  TRaster32P rgba       = raster;
  if (!rgba) {
    rgba = TRaster32P(raster->getLx(), raster->getLy());
    TRop::convert(rgba, raster);
  }
  // Probe IP-4: these native readers return straight alpha where every other
  // one returns premultiplied pixels.
  const bool straight = (extension == "tga" && info->m_samplePerPixel == 4) ||
                        (extension == "tif" && info->m_bitsPerSample == 16 &&
                         info->m_samplePerPixel == 4);
  if (straight) TRop::premultiply(rgba);
  if (!premultiplied(rgba))
    throw Error("rejected", "The decoded alpha is not premultiplied.");
  QImage output(rgba->getLx(), rgba->getLy(),
                QImage::Format_ARGB32_Premultiplied);
  rgba->lock();
  for (int y = 0; y < rgba->getLy(); ++y) {
    const TPixel32* row = rgba->pixels(rgba->getLy() - 1 - y);
    auto* target        = reinterpret_cast<QRgb*>(output.scanLine(y));
    for (int x = 0; x < rgba->getLx(); ++x)
      target[x] = qRgba(row[x].r, row[x].g, row[x].b, row[x].m);
  }
  rgba->unlock();
  if (!output.save(directory.filePath("decoded.png"), "PNG"))
    throw Error("rejected", "The helper cannot write the decoded pixels.");
  double dpiX = 0, dpiY = 0;
  image->getDpi(dpiX, dpiY);
  return {{"width", rgba->getLx()},
          {"height", rgba->getLy()},
          {"bits_per_sample", info->m_bitsPerSample},
          {"samples_per_pixel", info->m_samplePerPixel},
          {"raster_bytes_per_pixel", raster->getPixelSize()},
          {"alpha_premultiplied_by_helper", straight},
          {"dpi_x", dpiX},
          {"dpi_y", dpiY}};
}

}  // namespace GroundzeroAutomation
