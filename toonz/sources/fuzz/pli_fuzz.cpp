// SPDX-License-Identifier: BSD-3-Clause

// Robustness test for the PLI vector level reader (image/pli). The reader
// classes are not exported from the image library, so their sources are
// compiled into this test (see CMakeLists.txt). The seeds are written by
// TLevelWriterPli, the reader is driven like a level load: loadInfo(), then
// every frame.

#include "fuzzdriver.h"

#include "pli_io.h"
#include "tiio_pli.h"

// TnzCore includes
#include "tcontenthistory.h"
#include "tconvert.h"
#include "texception.h"
#include "tfilepath.h"
#include "timage_io.h"
#include "tlevel.h"
#include "tlevel_io.h"
#include "tpalette.h"
#include "tstroke.h"
#include "tvectorimage.h"

// Qt includes
#include <QCoreApplication>
#include <QString>

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using otfuzz::Bytes;
using otfuzz::Field;
using otfuzz::KnownIssue;
using otfuzz::Seed;

//-----------------------------------------------------------------------------
// Known issues

const KnownIssue kShortTag = {
    "pli-short-tag",
    "PLI: the tag readers in pli_io.cpp do not check the tag length; a tag "
    "shorter than its fixed part, or a count or size larger than the tag, is "
    "read out of bounds (from a null buffer when no earlier tag had data)"};
const KnownIssue kTruncatedTag = {
    "pli-truncated-tag",
    "PLI: a style, bitmap, intersection or (v150) image tag cut short by the "
    "end of the file is parsed from uninitialised buffer memory, including "
    "its sizes"};
const KnownIssue kPaletteTag = {
    "pli-palette-tag",
    "PLI: readPaletteTag() writes one colour past its array when the tag "
    "length is not a multiple of 3 (4 with alpha)"};

//! Walks the tags of a PLI file the way ParsedPliImp reads them. Returns the
//! known issue the file reaches and, if fields is set, collects the offsets
//! of its length fields.
const KnownIssue *scanPli(const Bytes &data, std::vector<Field> *fields) {
  if (data.size() < 6) return nullptr;
  const uint32_t magic = otfuzz::getU32(data, 0, false);
  bool bigEndian;
  if (magic == 0x4D494C50)
    bigEndian = false;
  else if (magic == 0x504C494D)
    bigEndian = true;
  else
    return nullptr;

  const unsigned major = data[4], minor = data[5];
  auto atLeast = [&](unsigned a, unsigned b) {
    return major > a || (major == a && minor >= b);
  };
  if (major > 150 || (major == 150 && minor > 0)) return nullptr;

  size_t pos    = 6;
  auto addField = [&](int bytes) {
    if (!fields || fields->size() >= 40) return;
    Field field = {pos, bytes, bigEndian};
    fields->push_back(field);
  };
  auto readValue = [&](int bytes, uint32_t &value) {
    if (!otfuzz::has(data, pos, size_t(bytes))) return false;
    value = bytes == 1   ? data[pos]
            : bytes == 2 ? otfuzz::getU16(data, pos, bigEndian)
                         : otfuzz::getU32(data, pos, bigEndian);
    pos += size_t(bytes);
    return true;
  };

  uint32_t value;
  if (atLeast(5, 8)) {  // creator
    addField(2);
    if (!readValue(2, value)) return nullptr;
    pos += value;
  }
  if (major < 5) {
    // The constructor reads all tags of these old files.
    if (!readValue(4, value) || !readValue(2, value) || !readValue(1, value))
      return nullptr;
  } else {
    if (!atLeast(5, 5)) return nullptr;  // TLevelReaderPli stops here
    if (!readValue(4, value)) return nullptr;
    addField(2);  // frame count
    if (!readValue(2, value)) return nullptr;
    if (!atLeast(5, 7) && !readValue(1, value)) return nullptr;
    if (atLeast(6, 5) && !readValue(1, value)) return nullptr;
    if (!readValue(1, value) || !readValue(1, value)) return nullptr;
  }

  unsigned width          = 2;  // bytes of the "dynamic" integers
  const KnownIssue *issue = nullptr;
  for (;;) {
    uint32_t first, type, lengthId, length = 0;
    if (!readValue(1, first)) break;
    if (first == 0xFF) {
      if (!readValue(2, type)) break;
      lengthId = type >> 14;
      type &= 0x3FFF;
    } else {
      lengthId = first >> 6;
      type     = first & 0x3F;
    }
    if (lengthId) {
      int bytes = lengthId == 1 ? 1 : lengthId == 2 ? 2 : 4;
      addField(bytes);
      if (!readValue(bytes, length)) break;
    }
    if (type == PliTag::END_CNTRL) break;

    const size_t start   = pos;
    const bool truncated = !otfuzz::has(data, start, length);
    auto byteAt          = [&](uint64_t offset) -> uint32_t {
      return start + offset < data.size() ? data[start + offset] : 0;
    };
    auto u16At = [&](uint64_t offset) -> uint32_t {
      return bigEndian ? (byteAt(offset) << 8) | byteAt(offset + 1)
                       : (byteAt(offset + 1) << 8) | byteAt(offset);
    };
    auto u32At = [&](uint64_t offset) -> uint32_t {
      return bigEndian ? (u16At(offset) << 16) | u16At(offset + 2)
                       : (u16At(offset + 2) << 16) | u16At(offset);
    };

    bool shortTag = false, sizeFromData = false;
    switch (type) {
    case PliTag::SET_DATA_8_CNTRL:
      width = 1;
      break;
    case PliTag::SET_DATA_16_CNTRL:
      width = 2;
      break;
    case PliTag::SET_DATA_32_CNTRL:
      width = 4;
      break;
    case PliTag::IMAGE_BEGIN_GOBJ:
      width = 2;  // loadFrame() starts every frame with 16-bit data
      break;
    case PliTag::PALETTE:
      if (length % 3 && !issue) issue = &kPaletteTag;
      break;
    case PliTag::PALETTE_WITH_ALPHA:
      if (length % 4 && !issue) issue = &kPaletteTag;
      break;
    case PliTag::THICK_QUADRATIC_CHAIN_GOBJ:
    case PliTag::THICK_QUADRATIC_LOOP_GOBJ:
      shortTag = length < (atLeast(5, 7) ? 2u : 1u) + 2 * width;
      break;
    case PliTag::BITMAP_GOBJ:
      sizeFromData = true;
      shortTag = length < 4 || uint64_t(u16At(0)) * u16At(2) * 4 > length - 4;
      break;
    case PliTag::IMAGE_GOBJ: {
      uint64_t header = 2;
      if (major >= 150) {
        sizeFromData = true;
        header += 4;
        if (length >= 6) header += u32At(2);
      } else if (atLeast(6, 6))
        header += 1;
      shortTag = header > length;
      break;
    }
    case PliTag::COLOR_NGOBJ:
      shortTag = length < 2;
      break;
    case PliTag::GEOMETRIC_TRANSFORMATION_GOBJ:
      shortTag = length < 13 * width;
      break;
    case PliTag::DOUBLEPAIR_OBJ:
      shortTag = length < 4 * width;
      break;
    case PliTag::STYLE_NGOBJ: {
      // readStyleTag() reads 16-bit data and walks the parameters.
      sizeFromData   = true;
      uint64_t off   = atLeast(5, 6) ? 4 : 2;
      shortTag       = off > length;
      int64_t remain = int64_t(int32_t(length)) - int64_t(off);
      while (!shortTag && remain > 0) {
        if (off >= length) {
          shortTag = true;
          break;
        }
        uint32_t paramType = byteAt(off++);
        --remain;
        uint64_t size = 0;
        if (paramType == TStyleParam::SP_BYTE)
          size = 1;
        else if (paramType == TStyleParam::SP_USHORT)
          size = 2;
        else if (paramType == TStyleParam::SP_INT ||
                 paramType == TStyleParam::SP_DOUBLE)
          size = 4;
        else if (paramType == TStyleParam::SP_RASTER)
          size = off + 4 > length
                     ? uint64_t(length) + 1
                     : 4 + uint64_t(u16At(off)) * u16At(off + 2) * 4;
        else if (paramType == TStyleParam::SP_STRING)
          size = off + 2 > length ? uint64_t(length) + 1 : 2 + u16At(off);
        if (off + size > length) shortTag = true;
        off += size;
        remain -= int64_t(size);
      }
      break;
    }
    case PliTag::INTERSECTION_DATA_GOBJ: {
      sizeFromData = true;
      if (length < 4) {
        shortTag = true;
        break;
      }
      const uint64_t count = u32At(0), branch = 3 * width + 2;
      if (count > (length - 4) / (branch + 1)) {
        shortTag = true;
        break;
      }
      uint64_t off = 4;
      for (uint64_t i = 0; i < count && !shortTag; ++i) {
        off += branch;
        if (off >= length)
          shortTag = true;
        else
          off += (byteAt(off) & 0x80) ? 1 : 8;
        if (off > length) shortTag = true;
      }
      break;
    }
    case PliTag::OUTLINE_OPTIONS_GOBJ:
      shortTag = length < 2 + 2 * width;
      break;
    case PliTag::PRECISION_SCALE_GOBJ:
    case PliTag::AUTOCLOSE_TOLERANCE_GOBJ:
      shortTag = length < width;
      break;
    default:
      break;
    }
    if (!issue && truncated && sizeFromData) issue = &kTruncatedTag;
    if (!issue && shortTag) issue = &kShortTag;
    if (issue && !fields) return issue;
    pos = start + length;
    if (pos > data.size()) break;
  }
  return issue;
}

//-----------------------------------------------------------------------------
// Seeds

TStroke *makeStroke(const std::vector<TThickPoint> &points, int styleId) {
  TStroke *stroke = new TStroke(points);
  stroke->setStyle(styleId);
  return stroke;
}

TVectorImageP makeDrawing(TPalette *palette, bool rich) {
  TVectorImageP image(new TVectorImage());
  image->setPalette(palette);
  image->addStroke(makeStroke(
      {TThickPoint(0, 0, 2), TThickPoint(10, 5, 3), TThickPoint(20, 0, 2),
       TThickPoint(30, -5, 1), TThickPoint(40, 0, 2)},
      2));
  if (!rich) return image;

  TStroke *loop = makeStroke(
      {TThickPoint(0, 0, 1), TThickPoint(50, 0, 1), TThickPoint(100, 0, 1),
       TThickPoint(100, 50, 1), TThickPoint(100, 100, 1),
       TThickPoint(50, 100, 1), TThickPoint(0, 100, 1), TThickPoint(0, 50, 1),
       TThickPoint(0, 0, 1)},
      1);
  loop->setSelfLoop();
  image->addStroke(loop);

  TStroke *mitered = makeStroke(
      {TThickPoint(10, 10, 4), TThickPoint(20, 30, 4), TThickPoint(30, 10, 4)},
      3);
  mitered->outlineOptions() =
      TStroke::OutlineOptions(TStroke::OutlineOptions::BUTT_CAP,
                              TStroke::OutlineOptions::MITER_JOIN, 0.5, 3.0);
  image->addStroke(mitered);

  image->group(1, 2);
  image->findRegions();
  image->fill(TPointD(50, 50), 2);
  image->setAutocloseTolerance(2.0);
  return image;
}

void writePli(const QString &path, bool rich, bool multipleSuffixes) {
  TPaletteP palette(new TPalette());
  int red = palette->addStyle(TPixel32(255, 0, 0));
  palette->getPage(0)->addStyle(red);
  int blue = palette->addStyle(TPixel32(0, 0, 255, 128));
  palette->addPage(L"second page")->addStyle(blue);
  if (rich) palette->setRefImgPath(TFilePath("reference.png"));

  // Projects that allow multiple suffixes save version 150 files.
  if (multipleSuffixes)
    TFilePath::setFilePathProperties(false, false, 1, false);
  try {
    TSmartPointerT<TLevelWriter> writer(
        new TLevelWriterPli(TFilePath(path), nullptr));
    writer->setCreator("otfuzz");
    TContentHistory *history = new TContentHistory(true);
    history->deserialize("| #    DATE:       Time:   MACHINE:    USER: |\n");
    writer->setContentHistory(history);
    TVectorImageP first = makeDrawing(palette.getPointer(), rich);
    writer->getFrameWriter(TFrameId(1))->save(TImageP(first.getPointer()));
    if (rich) {
      TVectorImageP second = makeDrawing(palette.getPointer(), false);
      TFrameId fid = multipleSuffixes ? TFrameId(2, QString("a")) : TFrameId(2);
      writer->getFrameWriter(fid)->save(TImageP(second.getPointer()));
    }
    // The writer saves the file when it is destroyed here.
  } catch (...) {
    TFilePath::setFilePathProperties(true, false, 1, false);
    throw;
  }
  TFilePath::setFilePathProperties(true, false, 1, false);
}

std::vector<Seed> makeSeeds(const QString &dir) {
  struct Spec {
    const char *m_name;
    bool m_rich, m_multipleSuffixes;
  };
  static const Spec specs[] = {{"one-stroke", false, false},
                               {"two-frames", true, false},
                               {"suffixes-v150", true, true}};
  std::vector<Seed> seeds;
  for (const Spec &spec : specs) {
    const QString path = dir + "/" + spec.m_name + ".pli";
    writePli(path, spec.m_rich, spec.m_multipleSuffixes);
    Seed seed;
    seed.m_name = spec.m_name;
    if (!otfuzz::readFile(path, seed.m_data) || seed.m_data.empty())
      throw std::runtime_error("cannot write the seed " + seed.m_name);
    scanPli(seed.m_data, &seed.m_fields);
    seeds.push_back(seed);
  }
  return seeds;
}

//-----------------------------------------------------------------------------
// Loading

void load(const QString &path) {
  TSmartPointerT<TLevelReader> reader(new TLevelReaderPli(TFilePath(path)));
  TLevelP level = reader->loadInfo();
  reader->getCreator();
  if (!level) return;

  std::string error;
  int count = 0;
  for (TLevel::Iterator it = level->begin(); it != level->end() && count < 16;
       ++it, ++count) {
    try {
      TImageReaderP frameReader = reader->getFrameReader(it->first);
      TVectorImageP image       = frameReader->load();
      if (image) {
        image->getStrokeCount();
        image->getRegionCount();
        image->getBBox();
      }
    } catch (const TException &e) {
      error = ::to_string(e.getMessage());
    } catch (const std::exception &e) {
      error = e.what();
    }
  }
  if (!error.empty()) throw std::runtime_error(error);
}

// The issues above are fixed in the reader (see doc/loader_known_issues.md),
// so no input is skipped any more; scanPli() only finds the length fields.
const otfuzz::Harness kHarness = {"pli",     "pli", false,
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
