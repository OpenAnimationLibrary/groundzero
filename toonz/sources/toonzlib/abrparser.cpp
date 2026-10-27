#include "toonz/abrparser.h"

#include <cstring>

namespace AbrParser {

namespace {

// Limits of what is accepted as a tip: Photoshop tips stay below 2500 pixels.
const int c_maxTipSide         = 10000;
const long long c_maxTipPixels = 64LL * 1024 * 1024;

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
bool readUnicodeName(Reader &r, std::string &name) {
  int32_t count;
  if (!r.i32(count) || count < 0) return false;
  if ((size_t)count > r.remaining() / 2) return false;

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
bool readBoundsAndImage(Reader &r, Tip &tip, std::string &why) {
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
bool readSampleV12(Reader &r, int version, Tip &tip, std::string &why) {
  int32_t misc;
  int spacing;
  if (!r.i32(misc) || !r.i16(spacing)) {
    why = "truncated header";
    return false;
  }
  tip.spacing = spacing;
  if (version == 2) {
    std::string name;
    if (!readUnicodeName(r, name)) {
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
  return readBoundsAndImage(r, tip, why);
}

void parseV12(const unsigned char *data, size_t size, int version, int count,
              Result &out) {
  Reader r(data, size, 4);
  for (int i = 0; i < count; ++i) {
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
      if (readSampleV12(sample, version, tip, why)) {
        out.tips.push_back(tip);
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
bool readSampleV6(Reader &r, int subVersion, Tip &tip, std::string &why) {
  // The id of the brush (a pascal string of 36 characters) and some fields
  // which are not used: 47 bytes for sub version 1, 301 for the others.
  if (!r.skip(subVersion == 1 ? 47 : 301)) {
    why = "truncated header";
    return false;
  }
  return readBoundsAndImage(r, tip, why);
}

//! \b section holds the samples, each one preceded by its length and padded
//! to a multiple of 4 bytes.
void parseSamples(Reader &section, int subVersion, Result &out) {
  int index = 0;
  while (section.remaining() >= 4) {
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
    if (readSampleV6(sample, subVersion, tip, why)) {
      out.tips.push_back(tip);
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
             Result &out) {
  Reader r(data, size, 4);
  bool sawSamples = false;
  while (r.remaining() >= 12) {
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
      parseSamples(section, subVersion, out);
    }
    // 'desc' (names, spacing and dynamics), 'patt' and the others are skipped
    r.seek(section.end());
  }
  if (!sawSamples)
    out.warnings.push_back("The file has no sampled brushes section.");
}

}  // namespace

//=============================================================================

bool parseAbr(const unsigned char *data, size_t size, Result &out,
              std::string &error) {
  out = Result();
  error.clear();

  Reader header(data, size, 0);
  unsigned version, second;
  if (!data || !header.u16(version) || !header.u16(second)) {
    error = "The file is too short to be an ABR file.";
    return false;
  }

  out.version = (int)version;
  if (version == 1 || version == 2) {
    parseV12(data, size, (int)version, (int)second, out);
  } else if (version >= 6 && version <= 10) {
    out.subVersion = (int)second;
    parseV6(data, size, (int)second, out);
  } else {
    error = "ABR version " + std::to_string(version) +
            " is not supported (versions 1, 2 and 6 to 10 are).";
    return false;
  }
  return true;
}

}  // namespace AbrParser
