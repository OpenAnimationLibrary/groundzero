// SPDX-License-Identifier: BSD-3-Clause

// Robustness test for TIStream, the XML-like parser behind scenes (.tnz),
// palettes (.tpl) and fx parameters, including its LZ4 compressed variant.
//
// Loading a whole scene (ToonzScene::loadTnzFile) needs the project, folder
// and fx registries of the application, so this harness drives the parser
// the way those loaders do instead:
//  - a generic walk over tags, attributes and text tokens, like the scene
//    loaders that skip what they do not know;
//  - TPalette::loadData, as TLevelReaderTzl and the studio palette read .tpl
//    files;
//  - the TPersist factory ("is >> TPersist *"), which scenes use for fx
//    parameters and other objects.

#include "fuzzdriver.h"

// TnzBase includes
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"

// TnzCore includes
#include "tcolorstyles.h"
#include "tconvert.h"
#include "texception.h"
#include "tfilepath.h"
#include "tpalette.h"
#include "tpersist.h"
#include "tpixel.h"
#include "tstream.h"

#if defined(LZ4_STATIC)
#include "lz4frame_static.h"
#else
#include "lz4frame.h"
#endif

// Qt includes
#include <QCoreApplication>
#include <QString>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using otfuzz::Bytes;
using otfuzz::KnownIssue;
using otfuzz::Seed;

TFilePath toPath(const QString &path) { return TFilePath(path); }

//-----------------------------------------------------------------------------
// Seeds

TPaletteP makePalette() {
  TPaletteP palette(new TPalette());
  palette->setPaletteName(L"fuzz palette");

  int red = palette->addStyle(TPixel32(255, 0, 0));
  palette->getPage(0)->addStyle(red);
  palette->getStyle(red)->setName(L"skin \"tone\" 1");
  palette->getStyle(red)->setPickedPosition(TPoint(3, 4));

  int blue = palette->addStyle(TPixel32(0, 0, 255, 128));
  palette->addPage(L"second page")->addStyle(blue);

  palette->setKeyframe(red, 0);
  palette->setKeyframe(red, 12);
  palette->setShortcutValue('1', red);
  palette->setRefImgPath(TFilePath("reference.png"));
  palette->setIsLocked(true);
  return palette;
}

TDoubleParamP makeParam() {
  TDoubleParamP param(new TDoubleParam(1.0));
  TDoubleKeyframe linear(0, 1.0);
  linear.m_type = TDoubleKeyframe::Linear;
  param->setKeyframe(linear);
  TDoubleKeyframe speed(10, 5.0);
  speed.m_type     = TDoubleKeyframe::SpeedInOut;
  speed.m_speedIn  = TPointD(-2, 0);
  speed.m_speedOut = TPointD(2, 1);
  param->setKeyframe(speed);
  TDoubleKeyframe ease(20, -3.0);
  ease.m_type = TDoubleKeyframe::EaseInOut;
  param->setKeyframe(ease);
  TDoubleKeyframe last(30, 2.0);
  last.m_type = TDoubleKeyframe::Exponential;
  param->setKeyframe(last);
  return param;
}

const char kGenericText[] =
    "<root version='71 0' name=\"a \\\"quoted\\\" name\">\n"
    "  <!-- a comment -- with dashes -->\n"
    "  <child id='3'/>\n"
    "  <text>word \"quoted \\\\ \\\" string\" 12 -7 3.5 dir/file.png</text>\n"
    "  <nested><a><b><c>1 2 3 4</c></b></a></nested>\n"
    "  <color>255 128 0 255</color>\n"
    "</root>\n";

Seed readSeed(const QString &path, const char *name) {
  Seed seed;
  seed.m_name = name;
  if (!otfuzz::readFile(path, seed.m_data) || seed.m_data.empty())
    throw std::runtime_error("cannot write the seed " + seed.m_name);
  return seed;
}

std::vector<Seed> makeSeeds(const QString &dir) {
  TPaletteP palette = makePalette();
  {
    // As StudioPalette saves a palette.
    TOStream os(toPath(dir + "/studio.tpl"));
    std::map<std::string, std::string> attributes;
    attributes["name"] = "fuzzpalette";
    os.openChild("palette", attributes);
    palette->saveData(os);
    os.closeChild();
  }
  {
    // As TLevelWriterTzl saves a level palette.
    TOStream os(toPath(dir + "/level.tpl"));
    os << static_cast<TPersist *>(palette.getPointer());
  }
  {
    TOStream os(toPath(dir + "/compressed.tpl"), true);
    os << static_cast<TPersist *>(palette.getPointer());
  }
  {
    TDoubleParamP param = makeParam();
    TOStream os(toPath(dir + "/param.xml"));
    os << static_cast<TPersist *>(param.getPointer());
  }

  std::vector<Seed> seeds;
  seeds.push_back(readSeed(dir + "/studio.tpl", "studio-palette"));
  seeds.push_back(readSeed(dir + "/level.tpl", "level-palette"));
  seeds.push_back(readSeed(dir + "/compressed.tpl", "compressed-palette"));
  seeds.push_back(readSeed(dir + "/param.xml", "double-param"));

  // TOStream writes the header fields of a compressed file in host order.
  Seed &compressed = seeds[2];
  for (size_t offset = 4; offset <= 12; offset += 4) {
    otfuzz::Field field = {offset, 4, false};
    compressed.m_fields.push_back(field);
  }

  Seed generic;
  generic.m_name = "generic";
  generic.m_data.assign(kGenericText, kGenericText + sizeof(kGenericText) - 1);
  seeds.push_back(generic);
  return seeds;
}

//-----------------------------------------------------------------------------
// Loading

void visitTag(TIStream &is, int depth, int &budget);

//! Reads the content of the tag just opened, up to its end tag.
void walkContent(TIStream &is, int depth, int &budget) {
  while (budget-- > 0) {
    if (is.matchEndTag()) return;
    std::string tagName;
    if (is.matchTag(tagName)) {
      visitTag(is, depth + 1, budget);
      continue;
    }
    if (!is) return;
    switch (budget % 4) {
    case 0: {
      std::string value;
      is >> value;
      break;
    }
    case 1: {
      QString value;
      is >> value;
      break;
    }
    case 2: {
      TFilePath value;
      is >> value;
      break;
    }
    default:
      is.getString();
      break;
    }
  }
}

//! Called after matchTag() returned a begin or begin/end tag.
void visitTag(TIStream &is, int depth, int &budget) {
  std::string value;
  is.getTagParam("id", value);
  is.getTagParam("name", value);
  if (is.isBeginEndTag()) return;
  if (depth > 64) {
    is.skipCurrentTag();
    return;
  }
  walkContent(is, depth, budget);
}

void walkTags(const TFilePath &fp) {
  TIStream is(fp);
  if (!is) return;
  int budget = 200000;
  std::string tagName;
  while (budget > 0 && is.matchTag(tagName)) visitTag(is, 0, budget);
}

void loadPalette(const TFilePath &fp) {
  // As TLevelReaderTzl::readPalette() reads the palette of a level.
  TIStream is(fp);
  if (!is) return;
  std::string tagName;
  if (!is.matchTag(tagName) || tagName != "palette") return;
  std::string name;
  is.getTagParam("name", name);
  TPaletteP palette(new TPalette());
  palette->loadData(is);
  palette->setGlobalName(::to_wstring(name));
  is.matchEndTag();

  // What the level and the palette viewer do next.
  TPaletteP copy(palette->clone());
  int styleCount = std::min(copy->getStyleCount(), 4096);
  for (int i = 0; i < styleCount; ++i) copy->getStyle(i)->getMainColor();
  for (int i = 0; i < copy->getPageCount(); ++i)
    copy->getPage(i)->getStyleCount();
}

void loadPersist(const TFilePath &fp) {
  // As scene loading reads fx parameters and other persistent objects.
  TIStream is(fp);
  if (!is) return;
  TPersist *object = nullptr;
  try {
    is >> object;
  } catch (...) {
    delete object;
    throw;
  }
  delete object;
}

void load(const QString &path) {
  typedef void (*Stage)(const TFilePath &);
  static const Stage stages[] = {walkTags, loadPalette, loadPersist};
  const TFilePath fp          = toPath(path);
  std::string error;
  for (Stage stage : stages) {
    try {
      stage(fp);
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

//-----------------------------------------------------------------------------
// Known issues: none. The compressed stream used to have four (a size near
// SIZE_MAX wrapping the buffer size, an unbounded size, a size too small making
// the decompression loop forever, data cut short), all fixed in tstream.cpp,
// so those inputs are run again.

const otfuzz::Harness kHarness = {"tstream", "tpl", true,
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
