// SPDX-License-Identifier: BSD-3-Clause

#include "../psd/tiio_psd.h"

#include "texception.h"
#include "tfilepath_io.h"
#include "tfiletype.h"
#include "tpixelutils.h"
#include "trasterimage.h"

#include <QCoreApplication>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
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
}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  try {
    // OpenToonz normally registers these in initImageIo()
    TFileType::declare("psd", TFileType::RASTER_LEVEL);
    TLevelWriter::define("psd", TLevelWriterPsd::create, false);

    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create PSD test directory");
    const TFilePath dir(directory.path());
    test8Bit(dir);
    test16Bit(dir);
    testUnicodeName(dir);
    testUnsupportedImage(dir);
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
