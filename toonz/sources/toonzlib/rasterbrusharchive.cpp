#include "rasterbrusharchive.h"
#include <QSet>
#include <zlib.h>
#include <algorithm>
#include <stdexcept>
#include <vector>
namespace RasterBrush {
namespace {
struct Zip {
  const QByteArray &data;
  unsigned integer(size_t p, int width) const {
    if (p > size_t(data.size()) || size_t(width) > size_t(data.size()) - p)
      throw std::runtime_error("Truncated bundle header");
    unsigned n = 0;
    for (int i = width - 1; i >= 0; --i)
      n = (n << 8) | static_cast<unsigned char>(data[int(p) + i]);
    return n;
  }
  QByteArray bytes(size_t p, size_t n) const {
    if (p > size_t(data.size()) || n > size_t(data.size()) - p)
      throw std::runtime_error("Truncated bundle member");
    return data.mid(int(p), int(n));
  }
};
QString path(const QByteArray &bytes) {
  QString p = QString::fromUtf8(bytes);
  if (p.toUtf8() != bytes || p.size() > 4096 || p.isEmpty() ||
      p.startsWith('/') || p.contains('\\') || p.contains(':') ||
      p.contains(QChar(0)))
    throw std::runtime_error("Unsafe bundle path");
  auto components = p.split('/');
  for (int i = 0; i < components.size(); ++i) {
    const auto &c = components[i];
    if (c == "." || c == ".." || (c.isEmpty() && i + 1 < components.size()))
      throw std::runtime_error("Unsafe bundle path component");
  }
  return p;
}
QByteArray unpack(const QByteArray &input, unsigned outputSize,
                  unsigned method) {
  if (method == 0) {
    if (unsigned(input.size()) != outputSize)
      throw std::runtime_error("Invalid stored bundle member size");
    return input;
  }
  if (method != 8) throw std::runtime_error("Unsupported bundle compression");
  QByteArray out(int(outputSize) + 1, 0);
  z_stream stream{};
  stream.next_in =
      reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
  stream.avail_in  = input.size();
  stream.next_out  = reinterpret_cast<Bytef *>(out.data());
  stream.avail_out = out.size();
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
    throw std::runtime_error("Cannot initialize bundle decoder");
  int code       = inflate(&stream, Z_FINISH);
  auto size      = stream.total_out;
  auto remaining = stream.avail_in;
  inflateEnd(&stream);
  if (code != Z_STREAM_END || size != outputSize || remaining)
    throw std::runtime_error("Invalid compressed bundle member");
  out.resize(outputSize);
  return out;
}
}  // namespace
QMap<QString, QByteArray> brushArchive(const QByteArray &bytes,
                                       QString &error) {
  error.clear();
  try {
    if (bytes.size() < 22 || bytes.size() > 64 * 1024 * 1024)
      throw std::runtime_error("Bundle input budget exceeded");
    Zip zip{bytes};
    int end = -1;
    for (int i = bytes.size() - 22; i >= std::max(0, int(bytes.size()) - 65557);
         --i)
      if (zip.integer(i, 4) == 0x06054b50u &&
          i + 22 + zip.integer(i + 20, 2) == unsigned(bytes.size())) {
        end = i;
        break;
      }
    if (end < 0)
      throw std::runtime_error("Bundle central directory is missing");
    unsigned count       = zip.integer(end + 10, 2),
             centralSize = zip.integer(end + 12, 4),
             central     = zip.integer(end + 16, 4);
    if (zip.integer(end + 4, 2) || zip.integer(end + 6, 2) ||
        zip.integer(end + 8, 2) != count || count > 1024 ||
        central > unsigned(end) || centralSize != unsigned(end) - central)
      throw std::runtime_error("Unsupported bundle directory layout");
    QMap<QString, QByteArray> result;
    QSet<QString> identities;
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t position = central, total = 0;
    for (unsigned item = 0; item < count; ++item) {
      if (position + 46 > size_t(end) ||
          zip.integer(position, 4) != 0x02014b50u)
        throw std::runtime_error("Invalid bundle central entry");
      unsigned flags      = zip.integer(position + 8, 2),
               method     = zip.integer(position + 10, 2),
               crc        = zip.integer(position + 16, 4);
      unsigned packed     = zip.integer(position + 20, 4),
               unpacked   = zip.integer(position + 24, 4);
      unsigned nameLength = zip.integer(position + 28, 2),
               extra      = zip.integer(position + 30, 2),
               comment    = zip.integer(position + 32, 2);
      unsigned attributes = zip.integer(position + 38, 4),
               local      = zip.integer(position + 42, 4);
      if (flags & ~(0x800u | 8u) || zip.integer(position + 34, 2) ||
          packed == 0xffffffffu || unpacked > 64u * 1024u * 1024u ||
          local >= central || ((attributes >> 16) & 0170000) == 0120000)
        throw std::runtime_error("Unsupported bundle member or link");
      if (unpacked > 256u * 1024u * 1024u - total)
        throw std::runtime_error("Bundle expansion budget exceeded");
      total += unpacked;
      size_t next = position + 46 + nameLength + extra + comment;
      if (next > size_t(end))
        throw std::runtime_error("Truncated bundle central entry");
      auto rawName = zip.bytes(position + 46, nameLength);
      QString name = path(rawName);
      QString identity =
          name.normalized(QString::NormalizationForm_C).toCaseFolded();
      if (identities.contains(identity))
        throw std::runtime_error("Duplicate or colliding bundle path");
      identities.insert(identity);
      // ZIP64 sizes/offsets cannot be recovered from extension fields.
      for (size_t ep = position + 46 + nameLength;
           ep < position + 46 + nameLength + extra;) {
        if (ep + 4 > position + 46 + nameLength + extra)
          throw std::runtime_error("Invalid ZIP extra field");
        unsigned tag = zip.integer(ep, 2), length = zip.integer(ep + 2, 2);
        ep += 4;
        if (tag == 1 || ep + length > position + 46 + nameLength + extra)
          throw std::runtime_error("Unsupported ZIP64 or extra field");
        ep += length;
      }
      if (size_t(local) + 30 > central ||
          zip.integer(local, 4) != 0x04034b50u ||
          zip.integer(local + 6, 2) != flags ||
          zip.integer(local + 8, 2) != method)
        throw std::runtime_error("Bundle local header mismatch");
      unsigned localName  = zip.integer(local + 26, 2),
               localExtra = zip.integer(local + 28, 2);
      size_t payload      = size_t(local) + 30 + localName + localExtra;
      if (localName != nameLength ||
          zip.bytes(size_t(local) + 30, localName) != rawName ||
          payload > central || packed > central - payload)
        throw std::runtime_error("Bundle member range mismatch");
      if (!(flags & 8) && (zip.integer(local + 14, 4) != crc ||
                           zip.integer(local + 18, 4) != packed ||
                           zip.integer(local + 22, 4) != unpacked))
        throw std::runtime_error("Bundle size/checksum header mismatch");
      size_t rangeEnd = payload + packed;
      if (flags & 8) {
        size_t descriptor = rangeEnd;
        if (zip.integer(descriptor, 4) == 0x08074b50u) descriptor += 4;
        if (descriptor + 12 > central || zip.integer(descriptor, 4) != crc ||
            zip.integer(descriptor + 4, 4) != packed ||
            zip.integer(descriptor + 8, 4) != unpacked)
          throw std::runtime_error("Invalid bundle data descriptor");
        rangeEnd = descriptor + 12;
      }
      ranges.emplace_back(local, rangeEnd);
      auto value = unpack(zip.bytes(payload, packed), unpacked, method);
      if (crc32(0, reinterpret_cast<const Bytef *>(value.constData()),
                value.size()) != crc)
        throw std::runtime_error("Bundle member checksum mismatch");
      if (!name.endsWith('/'))
        result[name] = value;
      else if (!value.isEmpty())
        throw std::runtime_error("Bundle directory contains data");
      position = next;
    }
    if (position != size_t(end))
      throw std::runtime_error("Unexpected bundle central records");
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i)
      if (ranges[i].first < ranges[i - 1].second)
        throw std::runtime_error("Overlapping bundle members");
    return result;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}
}  // namespace RasterBrush
