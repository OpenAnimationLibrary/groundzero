#include "rasterbrushpng.h"
#include <zlib.h>
#include <stdexcept>
namespace RasterBrush {
namespace {
unsigned be(const QByteArray &b, int p) {
  unsigned n = 0;
  for (int i = 0; i < 4; ++i)
    n = (n << 8) | static_cast<unsigned char>(b[p + i]);
  return n;
}
QByteArray inflateText(const QByteArray &in) {
  QByteArray out(1024 * 1024 + 1, 0);
  z_stream stream{};
  stream.next_in =
      reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
  stream.avail_in  = in.size();
  stream.next_out  = reinterpret_cast<Bytef *>(out.data());
  stream.avail_out = out.size();
  if (inflateInit(&stream) != Z_OK)
    throw std::runtime_error("Cannot initialize PNG metadata decoder");
  int status     = inflate(&stream, Z_FINISH);
  auto size      = stream.total_out;
  auto remaining = stream.avail_in;
  inflateEnd(&stream);
  if (status != Z_STREAM_END || size > 1024 * 1024 || remaining)
    throw std::runtime_error("Invalid or oversized PNG metadata");
  out.resize(int(size));
  return out;
}
}  // namespace
PngParts boundedPng(const QByteArray &bytes, QString &error) {
  PngParts result;
  error.clear();
  try {
    if (bytes.size() < 8 || bytes.size() > 64 * 1024 * 1024 ||
        bytes.left(8) != QByteArray("\x89PNG\r\n\x1a\n", 8))
      throw std::runtime_error("Invalid PNG signature or budget");
    result.image = bytes.left(8);
    int pos = 8, chunks = 0, totalText = 0;
    bool end = false, header = false, data = false;
    while (pos < bytes.size()) {
      if (++chunks > 8192 || bytes.size() - pos < 12)
        throw std::runtime_error("Invalid PNG chunk framing");
      unsigned n = be(bytes, pos);
      if (n > unsigned(bytes.size() - pos - 12))
        throw std::runtime_error("Truncated PNG chunk");
      auto type  = bytes.mid(pos + 4, 4);
      auto value = bytes.mid(pos + 8, int(n));
      auto crc =
          crc32(0, reinterpret_cast<const Bytef *>(bytes.constData() + pos + 4),
                n + 4);
      if (crc != be(bytes, pos + 8 + int(n)))
        throw std::runtime_error("PNG checksum mismatch");
      if (!header && type != "IHDR")
        throw std::runtime_error("PNG lacks initial header");
      if (type == "IHDR") {
        if (header || n != 13) throw std::runtime_error("Invalid PNG header");
        header = true;
      }
      bool keep = true;
      if (type == "tEXt" || type == "zTXt" || type == "iTXt" ||
          type == "iCCP") {
        int zero = value.indexOf('\0');
        if (zero < 1 || zero > 79)
          throw std::runtime_error("Invalid PNG metadata key");
        auto key  = value.left(zero);
        auto body = value.mid(zero + 1);
        if (type == "zTXt" || type == "iCCP") {
          if (body.isEmpty() || body.at(0) != 0)
            throw std::runtime_error("Unsupported PNG text compression");
          body = inflateText(body.mid(1));
        } else if (type == "iTXt") {
          if (body.size() < 4 || body.at(1) != 0 ||
              (body.at(0) != 0 && body.at(0) != 1))
            throw std::runtime_error("Invalid PNG international text");
          bool compressed = body.at(0) == 1;
          int language    = body.indexOf('\0', 2);
          if (language < 0) throw std::runtime_error("Invalid PNG language");
          int translated = body.indexOf('\0', language + 1);
          if (translated < 0)
            throw std::runtime_error("Invalid PNG translated key");
          body = body.mid(translated + 1);
          if (compressed) body = inflateText(body);
        }
        totalText += body.size();
        if (body.size() > 1024 * 1024 || totalText > 4 * 1024 * 1024)
          throw std::runtime_error("PNG metadata budget exceeded");
        if (type != "iCCP") {
          QString name = QString::fromLatin1(key);
          if (result.text.contains(name))
            throw std::runtime_error("Duplicate PNG metadata key");
          result.text[name] = body;
          keep              = false;
        }
      }
      if (type == "IDAT") data = true;
      if (keep) result.image.append(bytes.constData() + pos, int(n) + 12);
      pos += int(n) + 12;
      if (type == "IEND") {
        if (n || !data || pos != bytes.size())
          throw std::runtime_error("Invalid PNG end");
        end = true;
        break;
      }
    }
    if (!end) throw std::runtime_error("PNG lacks end marker");
    return result;
  } catch (const std::exception &e) {
    error = QString::fromUtf8(e.what());
    return {};
  }
}
}  // namespace RasterBrush
