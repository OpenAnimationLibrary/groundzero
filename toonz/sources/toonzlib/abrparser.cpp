#include "toonz/abrparser.h"

#include <cstring>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace AbrParser {

namespace {

// Limits of what is accepted as a tip: Photoshop tips stay below 2500 pixels.
const int c_maxTipSide         = 10000;
const long long c_maxTipPixels = 64LL * 1024 * 1024;

struct Budget {
  const Limits &limits;
  size_t decoded = 0, records = 0;
  size_t descriptorNodes = 0, descriptorBytes = 0;
  std::string error;
  explicit Budget(const Limits &value) : limits(value) {}
  bool record() {
    if (records >= limits.maxRecords) {
      error = "The ABR record limit was exceeded.";
      return false;
    }
    ++records;
    return true;
  }
  bool allocate(size_t bytes) {
    if (bytes > limits.maxDecodedBytes - decoded) {
      error = "The ABR decoded image budget was exceeded.";
      return false;
    }
    decoded += bytes;
    return true;
  }
  void descriptorAllocate(size_t bytes) {
    if (bytes > limits.maxDescriptorBytes - descriptorBytes) {
      error = "The ABR descriptor allocation budget was exceeded.";
      throw std::runtime_error(error);
    }
    descriptorBytes += bytes;
  }
  void descriptorReserve(size_t count) {
    if (count > limits.maxDescriptorNodes - descriptorNodes) {
      error = "The ABR descriptor node budget was exceeded.";
      throw std::runtime_error(error);
    }
    // Include bounded container bookkeeping, including duplicate-key checks.
    const size_t perNode = sizeof(DescriptorValue) + 64;
    if (count > (limits.maxDescriptorBytes - descriptorBytes) / perNode) {
      error = "The ABR descriptor allocation budget was exceeded.";
      throw std::runtime_error(error);
    }
    descriptorAllocate(count * perNode);
    descriptorNodes += count;
  }
};

//-----------------------------------------------------------------------------
// Bounds checked reader of big endian values. It never reads at or past its
// end: every read returns false instead.
//-----------------------------------------------------------------------------

class Reader {
  const unsigned char *m_data;
  size_t m_end, m_pos;

public:
  Reader(const unsigned char *data, size_t end, size_t pos)
      : m_data(data), m_end(end), m_pos(pos) {}

  size_t pos() const { return m_pos; }
  size_t end() const { return m_end; }
  size_t remaining() const { return m_end - m_pos; }
  const unsigned char *ptr() const { return m_data + m_pos; }

  //! A reader on [pos(), end), with end clamped to this reader's end.
  Reader sub(size_t end) const {
    if (end > m_end) end = m_end;
    if (end < m_pos) end = m_pos;
    return Reader(m_data, end, m_pos);
  }

  bool skip(size_t n) {
    if (n > remaining()) return false;
    m_pos += n;
    return true;
  }
  bool seek(size_t pos) {
    if (pos > m_end) return false;
    m_pos = pos;
    return true;
  }

  bool u8(unsigned &v) {
    if (remaining() < 1) return false;
    v = m_data[m_pos++];
    return true;
  }
  bool u16(unsigned &v) {
    if (remaining() < 2) return false;
    v = ((unsigned)m_data[m_pos] << 8) | m_data[m_pos + 1];
    m_pos += 2;
    return true;
  }
  bool i16(int &v) {
    unsigned u;
    if (!u16(u)) return false;
    v = u >= 0x8000u ? (int)u - 0x10000 : (int)u;
    return true;
  }
  bool u32(uint32_t &v) {
    if (remaining() < 4) return false;
    v = ((uint32_t)m_data[m_pos] << 24) | ((uint32_t)m_data[m_pos + 1] << 16) |
        ((uint32_t)m_data[m_pos + 2] << 8) | (uint32_t)m_data[m_pos + 3];
    m_pos += 4;
    return true;
  }
  bool i32(int32_t &v) {
    uint32_t u;
    if (!u32(u)) return false;
    v = (int32_t)u;
    return true;
  }
};

//-----------------------------------------------------------------------------

void appendUtf8(std::string &s, unsigned cp) {
  if (cp < 0x80) {
    s += (char)cp;
  } else if (cp < 0x800) {
    s += (char)(0xC0 | (cp >> 6));
    s += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += (char)(0xE0 | (cp >> 12));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  } else {
    s += (char)(0xF0 | (cp >> 18));
    s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F));
    s += (char)(0x80 | (cp & 0x3F));
  }
}

//! Reads a version 2 name: the number of UTF-16 units (the final null
//! included), then the units, big endian.
bool readUnicodeName(Reader &r, std::string &name, Budget &budget) {
  int32_t count;
  if (!r.i32(count) || count < 0) return false;
  if ((size_t)count > r.remaining() / 2) return false;
  if ((size_t)count > budget.limits.maxNameUnits) {
    budget.error = "The ABR name length limit was exceeded.";
    return false;
  }

  std::vector<unsigned> units(count);
  for (int32_t i = 0; i < count; ++i) r.u16(units[i]);
  while (!units.empty() && units.back() == 0) units.pop_back();

  name.clear();
  for (size_t i = 0; i < units.size(); ++i) {
    unsigned u = units[i];
    if (u >= 0xD800 && u < 0xDC00 && i + 1 < units.size() &&
        units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
      u = 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00);
      ++i;
    } else if (u >= 0xD800 && u < 0xE000) {
      u = 0xFFFD;  // lone surrogate
    }
    appendUtf8(name, u);
  }
  return true;
}

// Independently written reader of Adobe's documented Action Descriptor data
// layout. Decoding a typed value does not interpret its brush-engine meaning.
// https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
class DescriptorReader {
  Reader &r;
  Budget &budget;
  void need(bool valid) {
    if (!valid) throw std::runtime_error("truncated or invalid descriptor");
  }
  uint32_t word() {
    uint32_t value;
    need(r.u32(value));
    return value;
  }
  std::string bytes(size_t count) {
    need(count <= r.remaining());
    budget.descriptorAllocate(count);
    std::string value(reinterpret_cast<const char *>(r.ptr()), count);
    r.skip(count);
    return value;
  }
  std::string id() {
    size_t count = word();
    if (!count) count = 4;
    need(count <= 4096);
    return bytes(count);
  }
  std::string unicode() {
    Reader peek = r;
    uint32_t count;
    need(peek.u32(count));
    if (count > budget.limits.maxNameUnits ||
        count > (budget.limits.maxDescriptorBytes - budget.descriptorBytes) / 8) {
      budget.error = "The ABR descriptor string budget was exceeded.";
      throw std::runtime_error(budget.error);
    }
    budget.descriptorAllocate(size_t(count) * 8);
    std::string value;
    need(readUnicodeName(r, value, budget));
    return value;
  }
  uint64_t wide() {
    const uint64_t high = word();
    return (high << 32) | word();
  }
  double real() {
    uint64_t bits = wide();
    double value;
    static_assert(sizeof(value) == sizeof(bits), "IEEE binary64 required");
    std::memcpy(&value, &bits, sizeof(value));
    need(std::isfinite(value));
    return value;
  }
  void children(DescriptorValue &value, size_t depth, bool keyed) {
    const size_t count = word();
    need(count <= r.remaining() / (keyed ? 12 : 4));
    budget.descriptorReserve(count);
    value.children.reserve(count);
    std::set<std::string> keys;
    for (size_t i = 0; i < count; ++i) {
      std::string key = keyed ? id() : std::string();
      if (keyed) budget.descriptorAllocate(key.size());
      if (keyed && !keys.insert(key).second)
        throw std::runtime_error("duplicate descriptor key");
      auto child = item(depth + 1);
      child.key  = std::move(key);
      value.children.push_back(std::move(child));
    }
  }
  void object(DescriptorValue &value, size_t depth) {
    value.name    = unicode();
    value.classId = id();
    children(value, depth, true);
  }
  DescriptorValue item(size_t depth) {
    if (depth > budget.limits.maxDescriptorDepth) {
      budget.error = "The ABR descriptor nesting limit was exceeded.";
      throw std::runtime_error(budget.error);
    }
    DescriptorValue value;
    value.type = bytes(4);
    if (value.type == "Objc" || value.type == "GlbO") {
      object(value, depth);
    } else if (value.type == "VlLs") {
      children(value, depth, false);
    } else if (value.type == "TEXT") {
      value.text = unicode();
    } else if (value.type == "enum") {
      value.classId = id();
      value.text    = id();
    } else if (value.type == "long") {
      uint32_t n    = word();
      value.integer = n >= 0x80000000u ? int64_t(n) - 0x100000000LL : n;
    } else if (value.type == "comp") {
      uint64_t n    = wide();
      value.integer = n >> 63 ? -int64_t(~n) - 1 : int64_t(n);
    } else if (value.type == "doub" || value.type == "UntF") {
      if (value.type == "UntF") value.unit = bytes(4);
      value.number = real();
    } else if (value.type == "bool") {
      unsigned n;
      need(r.u8(n) && n <= 1);
      value.boolean = n != 0;
    } else if (value.type == "type" || value.type == "GlbC") {
      value.name    = unicode();
      value.classId = id();
    } else if (value.type == "tdta" || value.type == "alis") {
      // Preserve opaque bytes only. Aliases are never opened or resolved.
      value.text = bytes(word());
    } else {
      throw std::runtime_error("unsupported descriptor type: " + value.type);
    }
    return value;
  }

public:
  DescriptorReader(Reader &reader, Budget &b) : r(reader), budget(b) {}
  DescriptorValue read() {
    need(word() == 16);
    budget.descriptorReserve(1);
    DescriptorValue value;
    value.type = "Objc";
    object(value, 0);
    need(r.remaining() <= 3);
    while (r.remaining()) {
      unsigned padding;
      need(r.u8(padding) && padding == 0);
    }
    return value;
  }
};

//-----------------------------------------------------------------------------

//! Decodes a PackBits run sequence of \b srcLen bytes into exactly
//! \b dstLen bytes. Returns false if the data is malformed or does not fill
//! the destination.
bool unpackBits(const unsigned char *src, size_t srcLen, unsigned char *dst,
                size_t dstLen) {
  size_t i = 0, o = 0;
  while (i < srcLen && o < dstLen) {
    int n = (signed char)src[i++];
    if (n >= 0) {
      size_t count = (size_t)n + 1;
      if (count > srcLen - i || count > dstLen - o) return false;
      std::memcpy(dst + o, src + i, count);
      i += count;
      o += count;
    } else if (n != -128) {
      size_t count = (size_t)(1 - n);
      if (i >= srcLen || count > dstLen - o) return false;
      std::memset(dst + o, src[i], count);
      i += 1;
      o += count;
    }
    // -128 is a no-op
  }
  return o == dstLen;
}

//-----------------------------------------------------------------------------

//! Reads the compression flag and the image data which follow the depth
//! field. 16 bit images keep the high byte of each pixel.
bool decodeImage(Reader &r, int width, int height, int depth,
                 std::vector<unsigned char> &out, std::string &why) {
  const size_t bpp      = depth / 8;
  const size_t rowBytes = (size_t)width * bpp;

  unsigned compression;
  if (!r.u8(compression)) {
    why = "truncated before the image data";
    return false;
  }

  std::vector<unsigned> lengths;
  if (compression == 0) {
    if (rowBytes * (size_t)height > r.remaining()) {
      why = "truncated image data";
      return false;
    }
  } else if (compression == 1) {
    // each row needs at least two bytes per 128 bytes of the row
    size_t minRow = 2 * ((rowBytes + 127) / 128);
    if ((size_t)height * (2 + minRow) > r.remaining()) {
      why = "truncated image data";
      return false;
    }
    lengths.resize(height);
    for (int y = 0; y < height; ++y) r.u16(lengths[y]);
  } else {
    why = "unknown compression " + std::to_string(compression);
    return false;
  }

  out.assign((size_t)width * height, 0);
  std::vector<unsigned char> row(compression ? rowBytes : 0);
  for (int y = 0; y < height; ++y) {
    const unsigned char *src;
    if (compression == 0) {
      src = r.ptr();
      r.skip(rowBytes);
    } else {
      size_t len = lengths[y];
      if (len > r.remaining()) {
        why = "truncated image data";
        return false;
      }
      if (!unpackBits(r.ptr(), len, row.data(), rowBytes)) {
        why = "malformed PackBits row " + std::to_string(y);
        return false;
      }
      r.skip(len);
      src = row.data();
    }
    unsigned char *dst = &out[(size_t)y * width];
    for (int x = 0; x < width; ++x) dst[x] = src[(size_t)x * bpp];
  }
  return true;
}

//-----------------------------------------------------------------------------

//! Reads the 32 bit bounds, the depth and the image: the part which versions
//! 1, 2 and 6 and up have in common.
bool readBoundsAndImage(Reader &r, Tip &tip, std::string &why, Budget &budget) {
  int32_t top, left, bottom, right;
  int depth;
  if (!r.i32(top) || !r.i32(left) || !r.i32(bottom) || !r.i32(right) ||
      !r.i16(depth)) {
    why = "truncated header";
    return false;
  }
  long long width  = (long long)right - left;
  long long height = (long long)bottom - top;
  if (width < 1 || height < 1 || width > c_maxTipSide ||
      height > c_maxTipSide || width * height > c_maxTipPixels) {
    why = "unsupported size " + std::to_string(width) + "x" +
          std::to_string(height);
    return false;
  }
  if (depth != 8 && depth != 16) {
    why = "unsupported depth " + std::to_string(depth);
    return false;
  }
  if (!budget.allocate((size_t)(width * height))) {
    why = budget.error;
    return false;
  }
  tip.width  = (int)width;
  tip.height = (int)height;
  tip.depth  = depth;
  return decodeImage(r, tip.width, tip.height, depth, tip.pixels, why);
}

//-----------------------------------------------------------------------------

std::string defaultName(int index) {
  std::string s = std::to_string(index);
  while (s.size() < 3) s = "0" + s;
  return "tip_" + s;
}

//-----------------------------------------------------------------------------
// Versions 1 and 2

//! \b r ends where the brush ends.
bool readSampleV12(Reader &r, int version, Tip &tip, std::string &why,
                   Budget &budget) {
  int32_t misc;
  int spacing;
  if (!r.i32(misc) || !r.i16(spacing)) {
    why = "truncated header";
    return false;
  }
  tip.spacing = spacing;
  if (version == 2) {
    std::string name;
    if (!readUnicodeName(r, name, budget)) {
      why = "bad name";
      return false;
    }
    if (!name.empty()) tip.name = name;
  }
  unsigned antialiasing;
  // the bounds as 16 bit values, then again as 32 bit values (read below)
  if (!r.u8(antialiasing) || !r.skip(8)) {
    why = "truncated header";
    return false;
  }
  return readBoundsAndImage(r, tip, why, budget);
}

void parseV12(const unsigned char *data, size_t size, int version, int count,
              Result &out, Budget &budget) {
  Reader r(data, size, 4);
  for (int i = 0; i < count; ++i) {
    if (!budget.record()) return;
    int type;
    int32_t length;
    if (!r.i16(type) || !r.i32(length)) {
      out.warnings.push_back("The file ends after " + std::to_string(i) +
                             " of " + std::to_string(count) + " brushes.");
      return;
    }
    if (length < 0 || (size_t)length > r.remaining()) {
      out.warnings.push_back("Brush " + std::to_string(i + 1) +
                             " is truncated.");
      return;
    }
    size_t next = r.pos() + (size_t)length;
    if (type == 1) {
      ++out.computedBrushes;
    } else if (type == 2) {
      Tip tip;
      tip.name      = defaultName(i + 1);
      Reader sample = r.sub(next);
      std::string why;
      if (readSampleV12(sample, version, tip, why, budget)) {
        out.tips.push_back(std::move(tip));
      } else {
        ++out.skippedSamples;
        out.warnings.push_back("Brush " + std::to_string(i + 1) +
                               " skipped: " + why + ".");
      }
    } else {
      out.warnings.push_back("Brush " + std::to_string(i + 1) +
                             " has the unknown type " + std::to_string(type) +
                             ".");
    }
    r.seek(next);
  }
}

//-----------------------------------------------------------------------------
// Versions 6 and up

//! \b r ends where the sample ends.
bool readSampleV6(Reader &r, int subVersion, Tip &tip, std::string &why,
                  Budget &budget) {
  // The id of the brush (a pascal string of 36 characters) and some fields
  // which are not used: 47 bytes for sub version 1, 301 for the others.
  const size_t headerSize = subVersion == 1 ? 47 : 301;
  if (r.remaining() < headerSize) {
    why = "truncated header";
    return false;
  }
  const unsigned idSize = r.ptr()[0];
  if (idSize > 36) {
    why = "invalid sample identifier";
    return false;
  }
  tip.sampleId.assign(reinterpret_cast<const char *>(r.ptr() + 1), idSize);
  r.skip(headerSize);
  return readBoundsAndImage(r, tip, why, budget);
}

//! \b section holds the samples, each one preceded by its length and padded
//! to a multiple of 4 bytes.
void parseSamples(Reader &section, int subVersion, Result &out,
                  Budget &budget) {
  int index = 0;
  while (section.remaining() >= 4) {
    if (!budget.error.empty() || !budget.record()) return;
    uint32_t length;
    section.u32(length);
    ++index;
    if (length > section.remaining()) {
      out.warnings.push_back("Sample " + std::to_string(index) +
                             " is truncated.");
      return;
    }
    size_t start = section.pos();
    size_t next  = start + ((size_t)length + 3) / 4 * 4;
    if (next > section.end()) next = section.end();

    Tip tip;
    tip.name      = defaultName(index);
    Reader sample = section.sub(start + length);
    std::string why;
    if (readSampleV6(sample, subVersion, tip, why, budget)) {
      out.tips.push_back(std::move(tip));
    } else {
      ++out.skippedSamples;
      out.warnings.push_back("Sample " + std::to_string(index) +
                             " skipped: " + why + ".");
    }
    section.seek(next);
  }
}

bool isSignatureAt(const Reader &r, size_t offset) {
  return r.remaining() >= offset + 4 &&
         std::memcmp(r.ptr() + offset, "8BIM", 4) == 0;
}

void parseV6(const unsigned char *data, size_t size, int subVersion,
             Result &out, Budget &budget) {
  Reader r(data, size, 4);
  bool sawSamples = false;
  while (r.remaining() >= 12) {
    if (!budget.error.empty() || !budget.record()) return;
    if (!isSignatureAt(r, 0)) {
      // tolerate up to 3 bytes of padding between sections
      size_t pad = 1;
      while (pad < 4 && !isSignatureAt(r, pad)) ++pad;
      if (pad == 4) {
        out.warnings.push_back("Unrecognized data after the sections.");
        break;
      }
      r.skip(pad);
      if (r.remaining() < 12) break;
    }
    r.skip(4);
    char tag[4];
    std::memcpy(tag, r.ptr(), 4);
    r.skip(4);
    uint32_t length;
    r.u32(length);
    if (length > r.remaining()) {
      out.warnings.push_back("A section is truncated.");
      length = (uint32_t)r.remaining();
    }
    Reader section = r.sub(r.pos() + length);
    if (std::memcmp(tag, "samp", 4) == 0) {
      sawSamples = true;
      parseSamples(section, subVersion, out, budget);
    }
    // A decoded sample is not a decoded preset. Report the skipped behavioral
    // data even if all tip pixels decoded successfully.
    else if (std::memcmp(tag, "desc", 4) == 0) {
      try {
        out.descriptors.push_back(DescriptorReader(section, budget).read());
      } catch (const std::exception &error) {
        out.warnings.push_back(std::string("Preset descriptors not decoded: ") +
                               error.what() + ".");
      }
      out.warnings.push_back(
          "Preset descriptors are not interpreted: names, spacing and dynamics "
          "are not applied to these sampled tips.");
    } else if (std::memcmp(tag, "patt", 4) == 0) {
      out.warnings.push_back(
          "Pattern resources were not imported: textured brush behavior "
          "is not reproduced by these sampled tips.");
    }
    r.seek(section.end());
  }
  if (!sawSamples)
    out.warnings.push_back("The file has no sampled brushes section.");
}

}  // namespace

//=============================================================================

bool parseAbr(const unsigned char *data, size_t size, Result &out,
              std::string &error) {
  return parseAbr(data, size, out, error, Limits());
}

bool parseAbr(const unsigned char *data, size_t size, Result &out,
              std::string &error, const Limits &limits) {
  out = Result();
  error.clear();

  if (size > limits.maxFileBytes) {
    error = "The ABR file size limit was exceeded.";
    return false;
  }
  Budget budget(limits);
  Reader header(data, size, 0);
  unsigned version, second;
  if (!data || !header.u16(version) || !header.u16(second)) {
    error = "The file is too short to be an ABR file.";
    return false;
  }

  out.version = (int)version;
  if (version == 1 || version == 2) {
    parseV12(data, size, (int)version, (int)second, out, budget);
  } else if (version >= 6 && version <= 10) {
    if (second != 1 && second != 2) {
      error = "Unsupported ABR subversion " + std::to_string(second) + ".";
      return false;
    }
    out.subVersion = (int)second;
    parseV6(data, size, (int)second, out, budget);
  } else {
    error = "ABR version " + std::to_string(version) +
            " is not supported (versions 1, 2 and 6 to 10 are).";
    return false;
  }
  if (!budget.error.empty()) {
    error = budget.error;
    out   = Result();
    return false;
  }
  return true;
}

}  // namespace AbrParser
