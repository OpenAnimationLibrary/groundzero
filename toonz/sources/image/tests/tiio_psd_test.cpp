// SPDX-License-Identifier: BSD-3-Clause

#include "../../common/psdlib/psd.h"
#include "tlevel_io.h"
#include "tnzimage.h"
#include "tenv.h"
#include "tpsdlayerwriter.h"

#include "texception.h"
#include "tfilepath_io.h"
#include "tfiletype.h"
#include "tpixelutils.h"
#include "trasterimage.h"

#include <QCoreApplication>
#include <QDir>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

std::string pixelMessage(const std::string &message, int x, int y) {
  return message + " at (" + std::to_string(x) + ", " + std::to_string(y) + ")";
}

std::vector<unsigned char> readFile(const TFilePath &path) {
  FILE *file = fopen(path, "rb");
  require(file != nullptr, "Cannot open the written PSD file");
  std::vector<unsigned char> data;
  unsigned char buffer[4096];
  size_t count;
  while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    data.insert(data.end(), buffer, buffer + count);
  std::fclose(file);
  return data;
}

// Big-endian reader for checking the file layout independently of TPSDReader,
// which tolerates wrong section lengths that Photoshop would reject.
class ByteReader {
  const std::vector<unsigned char> &m_data;
  std::uint64_t m_pos = 0;

public:
  explicit ByteReader(const std::vector<unsigned char> &data) : m_data(data) {}

  std::uint64_t pos() const { return m_pos; }
  std::uint64_t size() const { return m_data.size(); }
  void seek(std::uint64_t pos) {
    require(pos <= m_data.size(), "PSD section runs past the end of file");
    m_pos = pos;
  }
  void skip(std::uint64_t count) { seek(m_pos + count); }
  unsigned int u8() {
    require(m_pos < m_data.size(), "PSD file is truncated");
    return m_data[m_pos++];
  }
  unsigned int u16() {
    const unsigned int high = u8();
    return (high << 8) | u8();
  }
  std::uint32_t u32() {
    const std::uint32_t high = u16();
    return (high << 16) | u16();
  }
  int s16() { return (std::int16_t)u16(); }
  std::string str(int count) {
    std::string result;
    for (int i = 0; i < count; ++i) result.push_back((char)u8());
    return result;
  }
};

struct PsdLayout {
  std::string pascalName;
  QString unicodeName;
  int compression          = -1;
  std::uint64_t mergedData = 0;  // offset of the merged compression field
};

// Reads the compression field and the RLE row counts of `planes` channels,
// returning the size of the row data that follows.
std::uint64_t channelDataSize(ByteReader &in, int planes, int ly, int rowBytes,
                              int &compression) {
  compression = in.u16();
  if (compression == 0) return (std::uint64_t)planes * ly * rowBytes;
  require(compression == 1, "Unexpected PSD compression");
  std::uint64_t size = 0;
  for (int i = 0; i < planes * ly; ++i) {
    const unsigned int count = in.u16();
    require(count > 0 && count <= 2u * rowBytes, "Bad RLE row byte count");
    size += count;
  }
  return size;
}

PsdLayout checkLayout(const std::vector<unsigned char> &data, int lx, int ly,
                      int depth) {
  const int rowBytes = lx * depth / 8;
  PsdLayout layout;
  ByteReader in(data);

  require(in.str(4) == "8BPS", "PSD signature mismatch");
  require(in.u16() == 1, "PSD version mismatch");
  in.skip(6);
  require(in.u16() == 4, "PSD header channel count mismatch");
  require(in.u32() == (std::uint32_t)ly, "PSD height mismatch");
  require(in.u32() == (std::uint32_t)lx, "PSD width mismatch");
  require(in.u16() == (unsigned int)depth, "PSD depth mismatch");
  require(in.u16() == 3, "PSD color mode is not RGB");
  in.skip(in.u32());  // color mode data
  const std::uint32_t resources = in.u32();
  require(resources % 2 == 0, "Image resources length is odd");
  in.skip(resources);

  const std::uint32_t layerMask = in.u32();
  require(layerMask % 2 == 0, "Layer and mask section length is odd");
  const std::uint64_t layerMaskEnd = in.pos() + layerMask;
  const std::uint32_t layerInfo    = in.u32();
  require(layerInfo % 2 == 0, "Layer info length is odd");
  const std::uint64_t layerInfoEnd = in.pos() + layerInfo;
  require(in.s16() == -1, "Expected one layer and merged alpha");

  require(in.u32() == 0 && in.u32() == 0, "Layer top/left mismatch");
  require(in.u32() == (std::uint32_t)ly, "Layer bottom mismatch");
  require(in.u32() == (std::uint32_t)lx, "Layer right mismatch");
  require(in.u16() == 4, "Layer channel count mismatch");
  const int channelIds[4] = {-1, 0, 1, 2};
  std::uint32_t channelLengths[4];
  for (int i = 0; i < 4; ++i) {
    require(in.s16() == channelIds[i], "Layer channel id mismatch");
    channelLengths[i] = in.u32();
  }
  require(in.str(4) == "8BIM", "Blend mode signature mismatch");
  require(in.str(4) == "norm", "Blend mode is not normal");
  require(in.u8() == 255, "Layer opacity mismatch");
  in.skip(3);
  const std::uint32_t extra    = in.u32();
  const std::uint64_t extraEnd = in.pos() + extra;
  require(in.u32() == 0, "Unexpected layer mask data");
  in.skip(in.u32());  // blending ranges
  const std::uint64_t nameStart = in.pos();
  const unsigned int nameLength = in.u8();
  layout.pascalName             = in.str(nameLength);
  require(nameLength > 0, "Layer has no name");
  in.seek(nameStart + ((1 + nameLength + 3) & ~3u));
  while (in.pos() < extraEnd) {
    require(in.str(4) == "8BIM", "Additional layer info signature mismatch");
    const std::string key        = in.str(4);
    const std::uint32_t length   = in.u32();
    const std::uint64_t blockEnd = in.pos() + length;
    require(length % 2 == 0, "Additional layer info length is odd");
    if (key == "luni") {
      const std::uint32_t count = in.u32();
      require(4 + 2 * (std::uint64_t)count <= length, "Bad luni length");
      for (std::uint32_t i = 0; i < count; ++i)
        layout.unicodeName += QChar((ushort)in.u16());
    }
    in.seek(blockEnd);
  }
  require(in.pos() == extraEnd, "Layer extra data length mismatch");

  for (int i = 0; i < 4; ++i) {
    const std::uint64_t channelStart = in.pos();
    int compression;
    const std::uint64_t size =
        channelDataSize(in, 1, ly, rowBytes, compression);
    require(in.pos() + size == channelStart + channelLengths[i],
            "Layer channel length mismatch");
    require(layout.compression < 0 || layout.compression == compression,
            "Inconsistent compression");
    layout.compression = compression;
    in.seek(channelStart + channelLengths[i]);
  }
  require(in.pos() <= layerInfoEnd && layerInfoEnd - in.pos() < 4,
          "Layer info length mismatch");
  in.seek(layerInfoEnd);
  in.skip(in.u32());  // global layer mask info
  require(in.pos() == layerMaskEnd, "Layer and mask section length mismatch");

  layout.mergedData = in.pos();
  int compression;
  const std::uint64_t size = channelDataSize(in, 4, ly, rowBytes, compression);
  require(compression == layout.compression, "Merged compression mismatch");
  require(in.pos() + size == in.size(), "Merged image data size mismatch");
  return layout;
}

TFilePath saveFrame(const TFilePath &levelPath, const TFrameId &fid,
                    const TRasterP &ras) {
  TRasterImageP image(ras);
  image->setDpi(150, 150);
  TLevelWriterP writer(levelPath);
  require(writer.getPointer() != nullptr, "No PSD level writer");
  TImageWriterP frameWriter = writer->getFrameWriter(fid);
  frameWriter->save(TImageP(image));
  return frameWriter->getFilePath();
}

void checkHeader(TPSDReader &reader, int lx, int ly, int depth) {
  const TPSDHeaderInfo header = reader.getPSDHeaderInfo();
  require(header.cols == lx && header.rows == ly, "PSD size read mismatch");
  require(header.depth == depth, "PSD depth read mismatch");
  require(header.channels == 4 && header.mode == ModeRGBColor,
          "PSD channels read mismatch");
  require(header.layersCount == 1 && header.mergedalpha,
          "PSD layer count read mismatch");
  require(std::fabs(header.hres - 150.0) < 0.01 &&
              std::fabs(header.vres - 150.0) < 0.01,
          "PSD resolution read mismatch");
}

TRaster32P loadLayer(TPSDReader &reader, int layerId, int lx, int ly) {
  TRasterImageP image;
  reader.load(image, layerId);
  require(image.getPointer() != nullptr, "TPSDReader could not load the PSD");
  TRaster32P ras = image->getRaster();
  require(ras && ras->getLx() == lx && ras->getLy() == ly,
          "Loaded PSD raster size mismatch");
  return ras;
}

// Straight-alpha test color for Toonz pixel (x, y): transparent, opaque and
// semi-transparent gradient blocks, so the rows contain both kinds of RLE
// packets, longer than 128 bytes.
TPixel32 straightColor(int x, int y, int opaqueStart, int gradientStart) {
  if (x < opaqueStart) return TPixel32(0, 0, 0, 0);
  if (x < gradientStart) return TPixel32(200, 30 + 10 * y, 90, 255);
  return TPixel32((x * 7 + y * 13) % 256, (x * 3) % 256, 255 - x % 256,
                  32 + (x - gradientStart) % 200);
}

void test8Bit(const TFilePath &dir) {
  const int lx = 400, ly = 7;
  TRaster32P ras(lx, ly);
  for (int y = 0; y < ly; ++y)
    for (int x = 0; x < lx; ++x)
      ras->pixels(y)[x] = premultiply(straightColor(x, y, 150, 200));

  const TFilePath path =
      saveFrame(dir + TFilePath("psdtest..psd"), TFrameId(1), ras);
  require(path == dir + TFilePath("psdtest.0001.psd"),
          "Unexpected PSD frame file name");

  const PsdLayout layout = checkLayout(readFile(path), lx, ly, 8);
  require(layout.compression == 1, "8 bit PSD is not RLE compressed");
  require(layout.pascalName == "psdtest.0001", "Layer name mismatch");
  require(layout.unicodeName == QString("psdtest.0001"),
          "Unicode layer name mismatch");

  TPSDReader reader(path);
  checkHeader(reader, lx, ly, 8);
  require(std::string(reader.getLayerInfo(0)->name) == "psdtest.0001",
          "TPSDReader layer name mismatch");

  // layer id 0 is the merged image, 1 the layer
  for (int layerId = 0; layerId <= 1; ++layerId) {
    TRaster32P loaded = loadLayer(reader, layerId, lx, ly);
    for (int y = 0; y < ly; ++y)
      for (int x = 0; x < lx; ++x) {
        const TPixel32 straight = straightColor(x, y, 150, 200);
        TPixel32 expected       = premultiply(straight);
        if (expected.m != 0) depremult(expected);
        const TPixel32 actual = loaded->pixels(y)[x];
        require(actual == expected, pixelMessage("8 bit pixel mismatch", x, y));
        if (straight.m >= 128)
          require(std::abs(actual.r - straight.r) <= 2 &&
                      std::abs(actual.g - straight.g) <= 2 &&
                      std::abs(actual.b - straight.b) <= 2,
                  pixelMessage("8 bit pixel is not depremultiplied", x, y));
      }
  }
}

TPixel64 straightColor64(int x, int y) {
  const TPixel32 color = straightColor(x, y, 96, 128);
  if (color.m == 0) return TPixel64(0, 0, 0, 0);
  const int low = (x * 11 + y * 5) % 256;
  return TPixel64(color.r * 256 + low, color.g * 256 + low, color.b * 256 + low,
                  color.m == 255 ? 65535 : color.m * 256 + (x * 3) % 256);
}

void test16Bit(const TFilePath &dir) {
  const int lx = 257, ly = 5;
  TRaster64P ras(lx, ly);
  for (int y = 0; y < ly; ++y)
    for (int x = 0; x < lx; ++x)
      ras->pixels(y)[x] = premultiply(straightColor64(x, y));

  // Without a frame number the level path itself is written
  const TFilePath path =
      saveFrame(dir + TFilePath("deep.psd"), TFrameId(TFrameId::NO_FRAME), ras);
  require(path == dir + TFilePath("deep.psd"), "Unexpected PSD file name");

  const std::vector<unsigned char> data = readFile(path);
  const PsdLayout layout                = checkLayout(data, lx, ly, 16);
  require(layout.compression == 0, "16 bit PSD is not stored raw");

  // Check the exact 16 bit values of the raw merged image data
  const std::uint64_t planes = layout.mergedData + 2;
  for (int y = 0; y < ly; ++y)
    for (int x = 0; x < lx; ++x) {
      TPixel64 expected = premultiply(straightColor64(x, y));
      if (expected.m != 0) depremult(expected);
      const unsigned int values[4] = {expected.r, expected.g, expected.b,
                                      expected.m};
      const int psdY               = ly - 1 - y;
      for (int c = 0; c < 4; ++c) {
        const std::uint64_t offset =
            planes + (((std::uint64_t)c * ly + psdY) * lx + x) * 2;
        const unsigned int value = (data[offset] << 8) | data[offset + 1];
        require(value == values[c],
                pixelMessage("16 bit merged value mismatch", x, y));
      }
    }

  // TPSDReader converts 16 bit images to 8 bit with dithering
  TPSDReader reader(path);
  checkHeader(reader, lx, ly, 16);
  for (int layerId = 0; layerId <= 1; ++layerId) {
    TRaster32P loaded = loadLayer(reader, layerId, lx, ly);
    for (int y = 0; y < ly; ++y)
      for (int x = 0; x < lx; ++x) {
        TPixel64 expected = premultiply(straightColor64(x, y));
        if (expected.m != 0) depremult(expected);
        const TPixel32 actual   = loaded->pixels(y)[x];
        const int actuals[4]    = {actual.r, actual.g, actual.b, actual.m};
        const double expects[4] = {expected.r / 257.0, expected.g / 257.0,
                                   expected.b / 257.0, expected.m / 257.0};
        for (int c = 0; c < 4; ++c)
          require(std::fabs(actuals[c] - expects[c]) <= 1.01,
                  pixelMessage("16 bit pixel mismatch", x, y));
      }
  }
}

void testUnicodeName(const TFilePath &dir) {
  TRaster32P ras(3, 2);
  ras->fill(TPixel32(10, 20, 30, 255));
  const TFilePath path = saveFrame(
      dir + TFilePath(QString::fromUtf8("psd\xC3\xA9..psd")), TFrameId(3), ras);
  const PsdLayout layout = checkLayout(readFile(path), 3, 2, 8);
  require(layout.pascalName == "psd_.0003", "ASCII layer name mismatch");
  require(layout.unicodeName == QString::fromUtf8("psd\xC3\xA9.0003"),
          "Unicode layer name mismatch");
}

// An image one pixel wide and high made the reader divide by a shrink of 0.
void testSinglePixel(const TFilePath &dir) {
  TRaster32P ras(1, 1);
  ras->fill(TPixel32(10, 20, 30, 255));
  const TFilePath path =
      saveFrame(dir + TFilePath("dot..psd"), TFrameId(1), ras);
  TPSDReader reader(path);
  checkHeader(reader, 1, 1, 8);
  for (int layerId = 0; layerId <= 1; ++layerId) {
    TRaster32P loaded = loadLayer(reader, layerId, 1, 1);
    require(loaded->pixels(0)[0] == TPixel32(10, 20, 30, 255),
            "Single pixel PSD mismatch");
  }
}

void writeFile(const TFilePath &path, const std::vector<unsigned char> &data) {
  FILE *file = fopen(path, "wb");
  require(file != nullptr, "Cannot write the damaged PSD file");
  if (!data.empty()) std::fwrite(data.data(), 1, data.size(), file);
  std::fclose(file);
}

// Independent raw PSD fixtures: no application writer or codec builds the
// pixel data, so reader and writer cannot hide the same decoding defect.
struct NativePsdBytes {
  std::vector<unsigned char> data;
  void u8(unsigned int n) { data.push_back(n & 255); }
  void u16(unsigned int n) {
    u8(n >> 8);
    u8(n);
  }
  void u32(std::uint32_t n) {
    u16(n >> 16);
    u16(n);
  }
  void text(const char *s) {
    while (*s) u8(*s++);
  }
  void append(const std::vector<unsigned char> &bytes) {
    data.insert(data.end(), bytes.begin(), bytes.end());
  }
};

std::vector<unsigned char> storedZip(const std::vector<unsigned char> &data) {
  // A zlib stream containing one uncompressed DEFLATE block. It still drives
  // the PSD ZIP path, including its retained decompressed channel buffers.
  require(data.size() <= 65535, "Native ZIP fixture is too large");
  NativePsdBytes out;
  out.u8(0x78);
  out.u8(0x01);
  out.u8(0x01);
  out.u8(data.size());
  out.u8(data.size() >> 8);
  out.u8(~data.size());
  out.u8((~data.size()) >> 8);
  out.append(data);
  std::uint32_t a = 1, b = 0;
  for (unsigned char byte : data) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  out.u32((b << 16) | a);
  return out.data;
}

std::vector<unsigned char> nativePsd(
    int width, int height, int depth, int mode,
    const std::vector<std::vector<unsigned char>> &merged,
    const std::vector<std::vector<unsigned char>> &layer,
    const TRect &layerBounds, bool zipLayer = false) {
  std::vector<std::vector<unsigned char>> encoded = layer;
  if (zipLayer)
    for (auto &plane : encoded) plane = storedZip(plane);
  NativePsdBytes extra;
  extra.u32(0);  // no layer mask
  extra.u32(0);  // no blending ranges
  extra.u8(4);
  extra.text("Gray");
  extra.u8(0);
  extra.u16(0);  // Pascal name padded to 4 bytes
  extra.text("8BIMlnsr");
  extra.u32(4);
  extra.text("test");

  NativePsdBytes layers;
  layers.u16(merged.size() > 1 ? 0xffff : 1);  // merged transparency flag
  layers.u32(layerBounds.y0);
  layers.u32(layerBounds.x0);
  layers.u32(layerBounds.y1 + 1);
  layers.u32(layerBounds.x1 + 1);
  layers.u16(layer.size());
  for (size_t i = 0; i < layer.size(); ++i) {
    layers.u16(i == 0 ? 0 : 0xffff);  // grayscale, transparency
    layers.u32(2 + encoded[i].size());
  }
  layers.text("8BIMnorm");
  layers.u8(255);
  layers.u8(0);
  layers.u8(0);
  layers.u8(0);
  layers.u32(extra.data.size());
  layers.append(extra.data);
  for (const auto &plane : encoded) {
    layers.u16(zipLayer ? 2 : 0);
    layers.append(plane);
  }
  if (layers.data.size() % 2) layers.u8(0);

  NativePsdBytes out;
  out.text("8BPS");
  out.u16(1);
  out.u32(0);
  out.u16(0);
  out.u16(merged.size());
  out.u32(height);
  out.u32(width);
  out.u16(depth);
  out.u16(mode);
  out.u32(0);
  out.u32(0);  // color data and image resources
  out.u32(4 + layers.data.size() + 4);
  out.u32(layers.data.size());
  out.append(layers.data);
  out.u32(0);  // global layer mask
  out.u16(0);  // raw merged image
  for (const auto &plane : merged) out.append(plane);
  return out.data;
}

void testNativeBitmap(const TFilePath &dir) {
  const TFilePath path = dir + TFilePath("native_bitmap.psd");
  const std::vector<unsigned char> plane = {
      0xaa, 0xa8, 0x00, 0x00, 0xff, 0xf8, 0x96, 0x48, 0x42, 0x10};
  const char *rows[] = {"1010101010101", "0000000000000", "1111111111111",
                        "1001011001001", "0100001000010"};
  writeFile(path, nativePsd(13, 5, 1, ModeBitmap, {plane}, {plane},
                            TRect(0, 0, 12, 4)));
  TPSDReader reader(path);
  struct Sample {
    TRect region;
    int sx, sy;
  };
  const Sample samples[] = {
      {TRect(), 1, 1}, {TRect(), 3, 3}, {TRect(3, 1, 11, 4), 2, 3},
      {TRect(7, 2, 7, 2), 8, 8}, {TRect(-2, -1, 5, 3), 1, 2}};
  for (const Sample &sample : samples) {
    const TRect crop = sample.region.isEmpty() ? TRect(0, 0, 12, 4)
                                             : sample.region * TRect(0, 0, 12, 4);
    reader.setRegion(sample.region);
    for (int layerId = 0; layerId <= 1; ++layerId) {
      reader.setShrink(sample.sx, sample.sy);
      TRasterImageP image;
      reader.load(image, layerId);
      require(!!image, "Native bitmap did not load");
      TRasterGR8P raster = image->getRaster();
      require(!!raster, "Native bitmap is not grayscale");
      const int width = (crop.getLx() - 1) / sample.sx + 1;
      const int height = (crop.getLy() - 1) / sample.sy + 1;
      require(raster->getLx() == width && raster->getLy() == height,
              "Native bitmap crop/shrink size mismatch");
      for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
          const int sx = crop.x0 + x * sample.sx;
          const int sy = crop.y0 + y * sample.sy;
          require(raster->pixels(y)[x].value == (rows[4 - sy][sx] == '1' ? 0 : 255),
                  pixelMessage("Native bitmap bit/crop/shrink mismatch", x, y));
        }
    }
  }

  // A cropped, offset layer uses its own row/column origin on the canvas.
  const TFilePath offset = dir + TFilePath("native_bitmap_offset.psd");
  const std::vector<unsigned char> layer = {0x96, 0x80, 0x69, 0x00, 0xaa, 0x80};
  const char *layerRows[] = {"100101101", "011010010", "101010101"};
  writeFile(offset, nativePsd(13, 5, 1, ModeBitmap, {plane}, {layer},
                              TRect(2, 1, 10, 3)));
  TPSDReader offsetReader(offset);
  offsetReader.setRegion(TRect(3, 0, 12, 4));
  offsetReader.setShrink(3, 2);
  TRasterImageP image;
  offsetReader.load(image, 1);
  require(!!image, "Offset bitmap did not load");
  TRasterGR8P raster = image->getRaster();
  require(!!raster && raster->getLx() == 4 && raster->getLy() == 3,
          "Offset bitmap dimensions mismatch");
  require(image->getSavebox() == TRect(0, 1, 2, 1), "Offset bitmap savebox mismatch");
  for (int x = 0; x < 3; ++x)
    require(raster->pixels(1)[x].value == (layerRows[1][1 + 3 * x] == '1' ? 0 : 255),
            "Offset bitmap sampled the wrong source pixel");
}

void testNativeGray16(const TFilePath &dir) {
  const unsigned int grays[] = {0, 65535, 16448, 65535, 32896, 8224};
  const unsigned int alphas[] = {0, 65535, 32896, 16448, 0, 65535};
  NativePsdBytes gray, alpha;
  for (unsigned int value : grays) gray.u16(value);
  for (unsigned int value : alphas) alpha.u16(value);
  for (bool layerAlpha : {false, true}) {
    const TFilePath path = dir + TFilePath(layerAlpha ? "native_gray_alpha.psd"
                                                     : "native_gray_opaque.psd");
    std::vector<std::vector<unsigned char>> layer = {gray.data};
    if (layerAlpha) layer.push_back(alpha.data);
    writeFile(path, nativePsd(3, 2, 16, ModeGrayScale, {gray.data, alpha.data},
                              layer, TRect(0, 0, 2, 1), layerAlpha));
    TPSDReader reader(path);
    for (int loadIndex = 0; loadIndex < 4; ++loadIndex) {
      const int layerId = loadIndex % 2;
      TRaster32P loaded = loadLayer(reader, layerId, 3, 2);
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) {
          const int source = (1 - y) * 3 + x;
          const TPixel32 pixel = loaded->pixels(y)[x];
          const double grayValue = grays[source] / 257.0;
          const double alphaValue = (layerId == 0 || layerAlpha)
                                        ? alphas[source] / 257.0 : 255;
          require(std::fabs(pixel.r - grayValue) <= 1.01 &&
                      std::fabs(pixel.g - grayValue) <= 1.01 &&
                      std::fabs(pixel.b - grayValue) <= 1.01 &&
                      std::fabs(pixel.m - alphaValue) <= 1.01,
                  pixelMessage("Native grayscale/alpha mismatch", x, y));
        }
    }
    reader.setRegion(TRect(1, 0, 2, 1));
    reader.setShrink(2, 1);
    TRaster32P cropped = loadLayer(reader, 0, 1, 2);
    require(std::abs(cropped->pixels(0)[0].r - 128) <= 1 &&
                cropped->pixels(0)[0].m == 0 &&
                cropped->pixels(1)[0] == TPixel32(255, 255, 255, 255),
            "Native grayscale crop sampled the wrong pixel");
    // Exercise metadata ownership and reader teardown through public factories.
    for (int repeat = 0; repeat < 16; ++repeat) {
      TLevelReaderP levelReader(path);
      TLevelP first = levelReader->loadInfo();
      TLevelP second = levelReader->loadInfo();
      require(first->getFrameCount() == 1 && second->getFrameCount() == 1,
              "Repeated PSD metadata load changed frame count");
      TImageP frame = levelReader->getFrameReader(TFrameId(1))->load();
      require(!!frame, "PSD frame did not survive metadata parser teardown");
      bool rejected = false;
      try {
        TPSDParser invalid(path.withName(path.getName() + "#Gray#invalid"));
      } catch (const TImageException &) {
        rejected = true;
      }
      require(rejected, "Invalid PSD level mode was accepted");
    }
  }
}


// Loading a damaged file may fail, but it must not crash or hang.
void loadDamaged(const TFilePath &path) {
  try {
    TPSDReader reader(path);
    for (int layerId = 0; layerId <= 1; ++layerId) {
      try {
        TRasterImageP image;
        reader.load(image, layerId);
      } catch (...) {
      }
    }
  } catch (...) {
  }
}

void testDamagedFiles(const TFilePath &dir) {
  TRaster32P ras(40, 5);
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 40; ++x)
      ras->pixels(y)[x] = premultiply(straightColor(x, y, 10, 20));
  const TFilePath path =
      saveFrame(dir + TFilePath("damaged..psd"), TFrameId(1), ras);
  const std::vector<unsigned char> data = readFile(path);
  require(data.size() > 32, "The written PSD file is too short");
  const TFilePath damaged = dir + TFilePath("cut.psd");

  // cut short, in the header area and then in steps
  for (size_t size = 0; size < data.size(); size += size < 160 ? 1 : 13) {
    writeFile(damaged, std::vector<unsigned char>(
                           data.begin(), data.begin() + (std::ptrdiff_t)size));
    loadDamaged(damaged);
  }

  // a size or count field set to an extreme value: the height, the width and
  // the channel count of the header
  const size_t offsets[]      = {14, 18, 12};
  const std::uint32_t sizes[] = {0, 1, 40000, 0x7fffffff, 0xffffffff};
  for (size_t offset : offsets)
    for (std::uint32_t value : sizes) {
      std::vector<unsigned char> copy = data;
      const int bytes                 = offset == 12 ? 2 : 4;
      for (int i = 0; i < bytes; ++i)
        copy[offset + i] = (unsigned char)(value >> (8 * (bytes - 1 - i)));
      writeFile(damaged, copy);
      loadDamaged(damaged);
    }
}

void testUnsupportedImage(const TFilePath &dir) {
  TLevelWriterP writer(dir + TFilePath("empty..psd"));
  bool thrown = false;
  try {
    writer->getFrameWriter(TFrameId(1))->save(TImageP());
  } catch (const TImageException &) {
    thrown = true;
  }
  require(thrown, "Saving a non-raster image did not fail");
  FILE *file = fopen(dir + TFilePath("empty.0001.psd"), "rb");
  if (file) std::fclose(file);
  require(file == nullptr, "A file was written for a non-raster image");
}

//-----------------------------------------------------------------------------
// Layered files (writeLayeredPsd)
//-----------------------------------------------------------------------------

typedef std::vector<unsigned char> Bytes;

struct ParsedLayer {
  std::string pascalName;
  QString unicodeName;
  unsigned int opacity = 0;
  unsigned int flags   = 0;
  Bytes planes[4];  // R, G, B, A, top-down rows of big-endian samples
};

struct ParsedPsd {
  int depth = 0;
  std::vector<ParsedLayer> layers;
  Bytes merged[4];
  int compression = -1;
};

// Decodes the PackBits rows of one plane
Bytes unpackRows(ByteReader &in, const std::vector<unsigned int> &counts,
                 int rowBytes) {
  Bytes result;
  for (unsigned int count : counts) {
    const std::uint64_t end = in.pos() + count;
    const size_t rowStart   = result.size();
    while (in.pos() < end) {
      const unsigned int n = in.u8();
      if (n < 128)
        for (unsigned int i = 0; i <= n; ++i) result.push_back(in.u8());
      else if (n > 128) {
        const unsigned char value = (unsigned char)in.u8();
        for (unsigned int i = 0; i < 257 - n; ++i) result.push_back(value);
      }
    }
    require(in.pos() == end, "RLE row overruns its byte count");
    require(result.size() - rowStart == (size_t)rowBytes,
            "RLE row length mismatch");
  }
  return result;
}

// Reads one layer channel (compression field included), `length` bytes long
Bytes readLayerChannel(ByteReader &in, std::uint64_t length, int ly,
                       int rowBytes) {
  const std::uint64_t start = in.pos();
  const int compression     = in.u16();
  Bytes result;
  if (compression == 0) {
    for (std::uint64_t i = 0; i < (std::uint64_t)ly * rowBytes; ++i)
      result.push_back((unsigned char)in.u8());
  } else {
    require(compression == 1, "Unexpected layer compression");
    std::vector<unsigned int> counts;
    for (int y = 0; y < ly; ++y) counts.push_back(in.u16());
    result = unpackRows(in, counts, rowBytes);
  }
  require(in.pos() == start + length, "Layer channel length mismatch");
  return result;
}

ParsedPsd parseLayeredPsd(const Bytes &data, int lx, int ly, int depth,
                          int layerCount) {
  const int rowBytes = lx * depth / 8;
  ParsedPsd psd;
  psd.depth = depth;
  ByteReader in(data);

  require(in.str(4) == "8BPS", "PSD signature mismatch");
  require(in.u16() == 1, "PSD version mismatch");
  in.skip(6);
  require(in.u16() == 4, "PSD header channel count mismatch");
  require(in.u32() == (std::uint32_t)ly, "PSD height mismatch");
  require(in.u32() == (std::uint32_t)lx, "PSD width mismatch");
  require(in.u16() == (unsigned int)depth, "PSD depth mismatch");
  require(in.u16() == 3, "PSD color mode is not RGB");
  in.skip(in.u32());  // color mode data
  in.skip(in.u32());  // image resources

  const std::uint32_t layerMask = in.u32();
  require(layerMask % 2 == 0, "Layer and mask section length is odd");
  const std::uint64_t layerMaskEnd = in.pos() + layerMask;
  const std::uint32_t layerInfo    = in.u32();
  require(layerInfo % 2 == 0, "Layer info length is odd");
  const std::uint64_t layerInfoEnd = in.pos() + layerInfo;
  require(in.s16() == -layerCount, "Layer count mismatch");

  // Layer records, bottom layer first
  std::vector<std::vector<std::uint32_t>> channelLengths;
  for (int l = 0; l < layerCount; ++l) {
    ParsedLayer layer;
    require(in.u32() == 0 && in.u32() == 0, "Layer top/left mismatch");
    require(in.u32() == (std::uint32_t)ly, "Layer bottom mismatch");
    require(in.u32() == (std::uint32_t)lx, "Layer right mismatch");
    require(in.u16() == 4, "Layer channel count mismatch");
    const int channelIds[4] = {-1, 0, 1, 2};
    std::vector<std::uint32_t> lengths;
    for (int i = 0; i < 4; ++i) {
      require(in.s16() == channelIds[i], "Layer channel id mismatch");
      lengths.push_back(in.u32());
    }
    channelLengths.push_back(lengths);
    require(in.str(4) == "8BIM", "Blend mode signature mismatch");
    require(in.str(4) == "norm", "Blend mode is not normal");
    layer.opacity = in.u8();
    in.skip(1);  // clipping
    layer.flags = in.u8();
    in.skip(1);  // filler
    const std::uint32_t extra    = in.u32();
    const std::uint64_t extraEnd = in.pos() + extra;
    require(in.u32() == 0, "Unexpected layer mask data");
    in.skip(in.u32());  // blending ranges
    const std::uint64_t nameStart = in.pos();
    const unsigned int nameLength = in.u8();
    layer.pascalName              = in.str(nameLength);
    in.seek(nameStart + ((1 + nameLength + 3) & ~3u));
    while (in.pos() < extraEnd) {
      require(in.str(4) == "8BIM", "Additional layer info signature mismatch");
      const std::string key        = in.str(4);
      const std::uint32_t length   = in.u32();
      const std::uint64_t blockEnd = in.pos() + length;
      if (key == "luni") {
        const std::uint32_t count = in.u32();
        for (std::uint32_t i = 0; i < count; ++i)
          layer.unicodeName += QChar((ushort)in.u16());
      }
      in.seek(blockEnd);
    }
    require(in.pos() == extraEnd, "Layer extra data length mismatch");
    psd.layers.push_back(layer);
  }

  // Layer channel image data
  const int planeOfChannel[4] = {3, 0, 1, 2};
  for (int l = 0; l < layerCount; ++l)
    for (int i = 0; i < 4; ++i)
      psd.layers[l].planes[planeOfChannel[i]] =
          readLayerChannel(in, channelLengths[l][i], ly, rowBytes);
  require(in.pos() <= layerInfoEnd && layerInfoEnd - in.pos() < 4,
          "Layer info length mismatch");
  in.seek(layerInfoEnd);
  in.skip(in.u32());  // global layer mask info
  require(in.pos() == layerMaskEnd, "Layer and mask section length mismatch");

  // Merged image data
  psd.compression = in.u16();
  if (psd.compression == 0) {
    for (int c = 0; c < 4; ++c)
      for (std::uint64_t i = 0; i < (std::uint64_t)ly * rowBytes; ++i)
        psd.merged[c].push_back((unsigned char)in.u8());
  } else {
    require(psd.compression == 1, "Unexpected merged compression");
    std::vector<unsigned int> counts;
    for (int i = 0; i < 4 * ly; ++i) counts.push_back(in.u16());
    for (int c = 0; c < 4; ++c)
      psd.merged[c] =
          unpackRows(in,
                     std::vector<unsigned int>(counts.begin() + c * ly,
                                               counts.begin() + (c + 1) * ly),
                     rowBytes);
  }
  require(in.pos() == in.size(), "Merged image data size mismatch");
  return psd;
}

// The straight-alpha planes (R, G, B, A, top-down) the file must hold
void expectedPlanes(const TRasterP &ras, Bytes planes[4]) {
  const int lx = ras->getLx(), ly = ras->getLy();
  TRaster32P ras32 = ras;
  TRaster64P ras64 = ras;
  for (int y = 0; y < ly; ++y) {
    const int rasY = ly - 1 - y;
    for (int x = 0; x < lx; ++x) {
      if (ras32) {
        TPixel32 p = ras32->pixels(rasY)[x];
        if (p.m != 0) depremult(p);
        const unsigned int values[4] = {p.r, p.g, p.b, p.m};
        for (int c = 0; c < 4; ++c)
          planes[c].push_back((unsigned char)values[c]);
      } else {
        TPixel64 p = ras64->pixels(rasY)[x];
        if (p.m != 0) depremult(p);
        const unsigned int values[4] = {p.r, p.g, p.b, p.m};
        for (int c = 0; c < 4; ++c) {
          planes[c].push_back((unsigned char)(values[c] >> 8));
          planes[c].push_back((unsigned char)(values[c] & 0xff));
        }
      }
    }
  }
}

void checkPlanes(const Bytes actual[4], const TRasterP &ras,
                 const std::string &what) {
  Bytes expected[4];
  expectedPlanes(ras, expected);
  for (int c = 0; c < 4; ++c)
    require(actual[c] == expected[c], what + " plane " + std::to_string(c) +
                                          " does not match the raster");
}

TRaster32P makeLayer32(int lx, int ly, int opaqueStart, int gradientStart) {
  TRaster32P ras(lx, ly);
  for (int y = 0; y < ly; ++y)
    for (int x = 0; x < lx; ++x)
      ras->pixels(y)[x] =
          premultiply(straightColor(x, y, opaqueStart, gradientStart));
  return ras;
}

void testLayered8Bit(const TFilePath &dir) {
  const int lx = 300, ly = 4;
  TRaster32P bottom = makeLayer32(lx, ly, 20, 100);
  TRaster32P middle = makeLayer32(lx, ly, 150, 210);
  TRaster32P top(lx, ly);  // fully transparent
  top->fill(TPixel32(0, 0, 0, 0));
  TRaster32P merged = makeLayer32(lx, ly, 60, 180);

  std::vector<TLayeredPsdLayer> layers(3);
  layers[0].m_name    = L"Background";
  layers[0].m_raster  = bottom;
  layers[1].m_name    = L"Aé col";
  layers[1].m_raster  = middle;
  layers[1].m_opacity = 128;
  layers[2].m_name    = L"Hidden";
  layers[2].m_raster  = top;
  layers[2].m_visible = false;

  const TFilePath path = dir + TFilePath("layered8.psd");
  writeLayeredPsd(path, merged, layers, 150.0, 150.0);

  const ParsedPsd psd = parseLayeredPsd(readFile(path), lx, ly, 8, 3);
  require(psd.compression == 1, "8 bit layered PSD is not RLE compressed");
  require(psd.layers[0].pascalName == "Background", "Layer 0 name mismatch");
  require(psd.layers[1].pascalName == "A_ col", "Layer 1 ASCII name mismatch");
  require(psd.layers[1].unicodeName == QString::fromUtf8("A\xC3\xA9 col"),
          "Layer 1 Unicode name mismatch");
  require(psd.layers[2].pascalName == "Hidden", "Layer 2 name mismatch");
  require(psd.layers[0].opacity == 255 && psd.layers[1].opacity == 128 &&
              psd.layers[2].opacity == 255,
          "Layer opacity mismatch");
  require((psd.layers[0].flags & 2) == 0 && (psd.layers[1].flags & 2) == 0 &&
              (psd.layers[2].flags & 2) != 0,
          "Layer visibility flag mismatch");
  checkPlanes(psd.layers[0].planes, bottom, "Layer 0");
  checkPlanes(psd.layers[1].planes, middle, "Layer 1");
  checkPlanes(psd.layers[2].planes, top, "Layer 2");
  checkPlanes(psd.merged, merged, "Merged image");

  // TPSDReader sees the layers (the ASCII names, in any order)
  TPSDReader reader(path);
  const TPSDHeaderInfo header = reader.getPSDHeaderInfo();
  require(header.layersCount == 3 && header.mergedalpha,
          "TPSDReader layer count mismatch");
  require(header.cols == lx && header.rows == ly, "TPSDReader size mismatch");
  int found = 0;
  for (int i = 0; i < header.layersCount; ++i) {
    const std::string name = reader.getLayerInfo(i)->name;
    if (name == "Background" || name == "Hidden") ++found;
  }
  require(found == 2, "TPSDReader layer names mismatch");

  // A layer too large to load (the bottom of the first record set beyond the
  // limit) is not loaded, and the next layer is still read from its own data
  Bytes big = readFile(path);
  {
    ByteReader in(big);
    in.skip(26);         // header
    in.skip(in.u32());   // color mode data
    in.skip(in.u32());   // image resources
    in.skip(4 + 4 + 2);  // section lengths, layer count
    const std::uint64_t bottom   = in.pos() + 8;
    const std::uint32_t tooLarge = 400000;
    for (int i = 0; i < 4; ++i)
      big[bottom + i] = (unsigned char)(tooLarge >> (8 * (3 - i)));
  }
  const TFilePath bigPath = dir + TFilePath("layered8_big.psd");
  writeFile(bigPath, big);
  TPSDReader bigReader(bigPath);
  require(bigReader.getPSDHeaderInfo().layersCount == 3,
          "TPSDReader layer count mismatch with a layer too large");
  TRasterImageP skipped;
  bigReader.load(skipped, bigReader.getLayerInfo(0)->layerId);
  require(!skipped, "A layer too large to load was loaded");
  // The layer after it loads exactly as it does from the unmodified file
  TRaster32P reference =
      loadLayer(reader, reader.getLayerInfo(1)->layerId, lx, ly);
  // (its opaque pixels are those written, so it is the right layer)
  for (int y = 0; y < ly; ++y)
    for (int x = 150; x < 210; ++x)
      require(reference->pixels(y)[x] == middle->pixels(y)[x],
              pixelMessage("Layer 1 opaque pixel mismatch", x, y));
  TRaster32P loaded =
      loadLayer(bigReader, bigReader.getLayerInfo(1)->layerId, lx, ly);
  for (int y = 0; y < ly; ++y)
    for (int x = 0; x < lx; ++x) {
      require(loaded->pixels(y)[x] == reference->pixels(y)[x],
              pixelMessage("Layer after a layer too large mismatch", x, y));
    }
}

void testLayered16Bit(const TFilePath &dir) {
  const int lx = 129, ly = 3;
  TRaster64P first(lx, ly), second(lx, ly), merged(lx, ly);
  for (int y = 0; y < ly; ++y)
    for (int x = 0; x < lx; ++x) {
      first->pixels(y)[x]  = premultiply(straightColor64(x, y));
      second->pixels(y)[x] = premultiply(straightColor64(lx - 1 - x, y + 1));
      merged->pixels(y)[x] = premultiply(straightColor64(x + 5, y));
    }

  std::vector<TLayeredPsdLayer> layers(2);
  layers[0].m_name   = L"One";
  layers[0].m_raster = first;
  layers[1].m_name   = L"Two";
  layers[1].m_raster = second;

  const TFilePath path = dir + TFilePath("layered16.psd");
  writeLayeredPsd(path, merged, layers, 72.0, 72.0);

  const ParsedPsd psd = parseLayeredPsd(readFile(path), lx, ly, 16, 2);
  require(psd.compression == 0, "16 bit layered PSD is not stored raw");
  checkPlanes(psd.layers[0].planes, first, "16 bit layer 0");
  checkPlanes(psd.layers[1].planes, second, "16 bit layer 1");
  checkPlanes(psd.merged, merged, "16 bit merged image");
}

void testLayeredErrors(const TFilePath &dir) {
  TRaster32P small(4, 4), big(5, 5);
  small->fill(TPixel32(1, 2, 3, 255));
  big->fill(TPixel32(1, 2, 3, 255));
  TRaster64P deep(4, 4);
  deep->fill(TPixel64(1, 2, 3, 65535));

  struct Case {
    const char *message;
    TRasterP merged;
    TRasterP layer;
    bool noLayers;
  } cases[] = {
      {"A layer of another size was accepted", small, big, false},
      {"A layer of another bit depth was accepted", small, deep, false},
      {"A null layer raster was accepted", small, TRasterP(), false},
      {"No layers were accepted", small, small, true}};
  for (const Case &c : cases) {
    std::vector<TLayeredPsdLayer> layers;
    if (!c.noLayers) {
      TLayeredPsdLayer layer;
      layer.m_name   = L"L";
      layer.m_raster = c.layer;
      layers.push_back(layer);
    }
    bool thrown = false;
    try {
      writeLayeredPsd(dir + TFilePath("layered_bad.psd"), c.merged, layers,
                      72.0, 72.0);
    } catch (const TImageException &) {
      thrown = true;
    }
    require(thrown, c.message);
  }
  FILE *file = fopen(dir + TFilePath("layered_bad.psd"), "rb");
  if (file) std::fclose(file);
  require(file == nullptr, "A file was written for invalid layers");
}
}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create PSD test directory");
    const TFilePath dir(directory.path());
    // Public factory initialization reads preferences. Keep that state inside
    // the test directory, with a valid default room for debug builds.
    const QString stuff = QDir(directory.path()).filePath("stuff");
    require(QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default")),
            "Cannot create PSD test settings");
    TEnv::setStuffDir(TFilePath(stuff));
    TEnv::setApplicationFullName("tiio_psd_test");
    initImageIo();
    test8Bit(dir);
    test16Bit(dir);
    testUnicodeName(dir);
    testSinglePixel(dir);
    testNativeBitmap(dir);
    testNativeGray16(dir);
    testDamagedFiles(dir);
    testUnsupportedImage(dir);
    testLayered8Bit(dir);
    testLayered16Bit(dir);
    testLayeredErrors(dir);
    std::cout << "PSD writer tests passed\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << "PSD writer test failed: "
              << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << "PSD writer test failed: " << error.what() << '\n';
  }
  return 1;
}
