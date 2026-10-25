// SPDX-License-Identifier: BSD-3-Clause

// Robustness test for the PSD reader (common/psdlib, built into tnzcore).
//
// TLevelReaderPsd (image/psd) asks the application preferences for the
// layer name encoding, so this harness drives TPSDReader and TPSDParser the
// way it does instead: the parser for every way a scene imports a PSD (flat,
// one level per layer, the layers as frames of one level, groups as
// levels), a load of every frame it lists, and a shrunk partial load as the
// viewer does. The seeds are written byte by byte here, since the PSD writer
// of the image library is not exported on Windows.
//
// No known issues are listed yet: inputs that crash the reader are reported
// as failures, with the commands that reproduce them.

#include "fuzzdriver.h"

#include "../common/psdlib/psd.h"

// TnzCore includes
#include "tconvert.h"
#include "texception.h"
#include "tfilepath.h"
#include "trasterimage.h"

// Qt includes
#include <QCoreApplication>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using otfuzz::Bytes;
using otfuzz::Seed;
using otfuzz::Writer;

//-----------------------------------------------------------------------------
// Seeds

const int kRaw = 0, kRle = 1;

//! Sample values; the right half of each row is a run, so that the RLE
//! seeds hold both kinds of packet.
uint8_t sampleAt(int channel, int y, int x, int cols) {
  if (x >= cols / 2) return uint8_t(40 * channel + 10 * y + 1);
  return uint8_t(17 * channel + 29 * y + 7 * x + 3);
}

//! One row of one channel; channel id -1 (transparency) uses sample 3.
Bytes rowData(int channelId, int y, int cols, int depth) {
  const int channel = channelId < 0 ? 3 : channelId;
  Bytes row;
  for (int x = 0; x < cols; ++x) {
    uint8_t value = sampleAt(channel, y, x, cols);
    row.push_back(value);
    if (depth == 16) row.push_back(uint8_t(value ^ 0x5a));
  }
  return row;
}

//! PackBits, as Photoshop compresses rows.
Bytes packBits(const Bytes &row) {
  Bytes out;
  size_t i = 0;
  while (i < row.size()) {
    size_t run = 1;
    while (i + run < row.size() && run < 128 && row[i + run] == row[i]) ++run;
    if (run >= 3) {
      out.push_back(uint8_t(257 - run));  // 1 - run as a signed byte
      out.push_back(row[i]);
      i += run;
      continue;
    }
    const size_t start = i;
    while (
        i < row.size() && i - start < 128 &&
        !(i + 2 < row.size() && row[i] == row[i + 1] && row[i] == row[i + 2]))
      ++i;
    out.push_back(uint8_t(i - start - 1));
    out.insert(out.end(), row.begin() + std::ptrdiff_t(start),
               row.begin() + std::ptrdiff_t(i));
  }
  return out;
}

//! Image data of some channels behind one compression field: all channels
//! for the merged image, one for each layer channel.
void writeChannels(Writer &w, const std::vector<int> &channelIds, int rows,
                   int cols, int depth, int compression) {
  w.u16(unsigned(compression));
  if (compression == kRaw) {
    for (int id : channelIds)
      for (int y = 0; y < rows; ++y) {
        Bytes row = rowData(id, y, cols, depth);
        w.bytes(row.data(), row.size());
      }
    return;
  }
  std::vector<Bytes> packed;
  for (int id : channelIds)
    for (int y = 0; y < rows; ++y)
      packed.push_back(packBits(rowData(id, y, cols, depth)));
  for (const Bytes &row : packed) w.u16(unsigned(row.size()), true);
  for (const Bytes &row : packed) w.bytes(row.data(), row.size());
}

struct Layer {
  int m_top, m_left, m_bottom, m_right;
  std::vector<int> m_channelIds;
  const char *m_name;
  int m_layerId;  // written as a "lyid" block when not 0
  int m_section;  // written as a "lsct" block when not 0
};

struct PsdSpec {
  int m_channels, m_rows, m_cols, m_depth, m_mode, m_compression;
  bool m_mergedAlpha;
  std::vector<Layer> m_layers;
};

void writeBlock(Writer &w, const char *key, uint32_t value) {
  w.text("8BIM");
  w.text(key);
  w.u32(4, true);
  w.u32(value);
}

Seed makePsd(const char *name, const PsdSpec &spec) {
  Writer w(true);

  // Header
  w.text("8BPS");
  w.u16(1);  // version
  w.zeros(6);
  w.u16(unsigned(spec.m_channels), true);
  w.u32(uint32_t(spec.m_rows), true);
  w.u32(uint32_t(spec.m_cols), true);
  w.u16(unsigned(spec.m_depth), true);
  w.u16(unsigned(spec.m_mode));

  w.u32(0, true);  // color mode data

  // Image resources: the resolution
  const size_t resources = w.pos();
  w.u32(0, true);
  w.text("8BIM");
  w.u16(1005);
  w.u16(0);  // empty name, padded to an even length
  w.u32(16, true);
  for (int i = 0; i < 2; ++i) {
    w.u32(150u << 16);  // 150 dpi as 16.16 fixed point
    w.u16(1);
    w.u16(1);
  }
  w.patchU32(resources, uint32_t(w.pos() - resources - 4));

  // Layer and mask information
  const size_t layerAndMask = w.pos();
  w.u32(0, true);
  const size_t layerInfo = w.pos();
  w.u32(0, true);
  if (!spec.m_layers.empty()) {
    const int count = int(spec.m_layers.size());
    w.u16(unsigned(spec.m_mergedAlpha ? -count : count) & 0xffff, true);

    std::vector<size_t> channelLengths;
    for (const Layer &layer : spec.m_layers) {
      w.u32(uint32_t(layer.m_top), true);
      w.u32(uint32_t(layer.m_left), true);
      w.u32(uint32_t(layer.m_bottom), true);
      w.u32(uint32_t(layer.m_right), true);
      w.u16(unsigned(layer.m_channelIds.size()), true);
      for (int id : layer.m_channelIds) {
        w.u16(unsigned(id) & 0xffff);
        channelLengths.push_back(w.pos());
        w.u32(0, true);
      }
      w.text("8BIM");
      w.text("norm");
      w.u8(255);  // opacity
      w.u8(0);    // clipping
      w.u8(0);    // flags
      w.u8(0);    // filler

      const size_t extra = w.pos();
      w.u32(0, true);
      w.u32(0, true);  // no layer mask
      w.u32(0, true);  // no blending ranges
      // Pascal string padded to a multiple of 4 bytes
      const size_t nameLength = std::strlen(layer.m_name);
      w.u8(unsigned(nameLength), true);
      w.text(layer.m_name);
      w.zeros(((nameLength + 4) & ~size_t(3)) - nameLength - 1);
      if (layer.m_layerId) writeBlock(w, "lyid", uint32_t(layer.m_layerId));
      if (layer.m_section) writeBlock(w, "lsct", uint32_t(layer.m_section));
      w.patchU32(extra, uint32_t(w.pos() - extra - 4));
    }

    size_t field = 0;
    for (const Layer &layer : spec.m_layers) {
      const int rows = layer.m_bottom - layer.m_top;
      const int cols = layer.m_right - layer.m_left;
      for (int id : layer.m_channelIds) {
        const size_t start = w.pos();
        writeChannels(w, std::vector<int>(1, id), rows, cols, spec.m_depth,
                      spec.m_compression);
        w.patchU32(channelLengths[field++], uint32_t(w.pos() - start));
      }
    }
    if ((w.pos() - layerInfo) % 2) w.u8(0);
    w.patchU32(layerInfo, uint32_t(w.pos() - layerInfo - 4));
  }
  w.u32(0, true);  // global layer mask information
  w.patchU32(layerAndMask, uint32_t(w.pos() - layerAndMask - 4));

  // Merged image
  std::vector<int> channelIds;
  for (int ch = 0; ch < spec.m_channels; ++ch) channelIds.push_back(ch);
  writeChannels(w, channelIds, spec.m_rows, spec.m_cols, spec.m_depth,
                spec.m_compression);

  Seed seed;
  seed.m_name   = name;
  seed.m_data   = w.m_data;
  seed.m_fields = w.m_fields;
  return seed;
}

std::vector<Seed> makeSeeds(const QString &) {
  const std::vector<int> rgba = {-1, 0, 1, 2};
  std::vector<Seed> seeds;

  // Two RGBA layers, the second with an explicit layer id.
  PsdSpec layers = {4, 6, 8, 8, ModeRGBColor, kRaw, true, {}};
  layers.m_layers.push_back({0, 0, 6, 8, rgba, "paper", 0, 0});
  layers.m_layers.push_back({1, 2, 5, 7, {0, 1, 2}, "line", 7, 0});
  seeds.push_back(makePsd("rgba-layers-raw", layers));

  // A group, stored bottom to top: end marker, content, group header.
  PsdSpec group = {3, 5, 6, 8, ModeRGBColor, kRle, false, {}};
  group.m_layers.push_back({0, 0, 0, 0, rgba, "</Layer group>", 11, 3});
  group.m_layers.push_back({0, 1, 5, 6, rgba, "inside", 12, 0});
  group.m_layers.push_back({0, 0, 0, 0, rgba, "group", 13, 1});
  seeds.push_back(makePsd("rgb-group-rle", group));

  // A flat 16-bit grayscale image without layers.
  PsdSpec flat = {1, 4, 5, 16, ModeGrayScale, kRaw, false, {}};
  seeds.push_back(makePsd("gray16-flat", flat));
  return seeds;
}

//-----------------------------------------------------------------------------
// Loading

const int kMaxLevels = 8, kMaxFrames = 8, kMaxLoads = 16;

//! How a scene names a PSD to import it: flat, one level per layer, the
//! layers as frames of one level, groups as levels.
const char *const kImportModes[] = {"", "#1", "#1#frames", "#1#group"};

void importAs(const QString &path, const char *mode) {
  // The reader removes the "#..." part again to find the file.
  const QFileInfo info(path);
  const TFilePath levelPath(info.path() + "/" + info.completeBaseName() +
                            QString::fromLatin1(mode) + "." + info.suffix());
  TPSDParser parser(levelPath);
  int loads            = 0;
  const int levelCount = std::min(parser.getLevelsCount(), kMaxLevels);
  for (int i = 0; i < levelCount; ++i) {
    const int levelId = parser.getLevelId(i);
    parser.getLevelNameWithCounter(levelId);
    const int frameCount = std::min(parser.getFramesCount(levelId), kMaxFrames);
    for (int f = 0; f < frameCount && loads < kMaxLoads; ++f, ++loads) {
      TRasterImageP image;
      parser.load(image, parser.getFrameId(levelId, f));
    }
  }
}

void loadPart(const QString &path) {
  // As the viewer loads a level at a reduced size.
  TPSDReader reader{TFilePath(path)};
  reader.setShrink(2, 2);
  reader.setRegion(TRect(0, 0, 2, 2));
  TRasterImageP image;
  reader.load(image, 0);
}

void load(const QString &path) {
  std::string error;
  for (int i = 0; i <= 4; ++i) {
    try {
      if (i < 4)
        importAs(path, kImportModes[i]);
      else
        loadPart(path);
    } catch (const TException &e) {
      error = ::to_string(e.getMessage());
    } catch (const std::exception &e) {
      error = e.what();
    } catch (...) {
      error = "exception";
    }
  }
  if (!error.empty()) throw std::runtime_error(error);
}

const otfuzz::Harness kHarness = {"psd",     "psd", false,
                                  makeSeeds, load,  nullptr};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  return otfuzz::fuzzOne(kHarness, data, size);
}

#ifndef OT_LIBFUZZER
int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  return otfuzz::runDriver(kHarness, argc, argv);
}
#endif
