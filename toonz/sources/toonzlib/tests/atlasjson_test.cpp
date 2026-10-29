// Regression test of the JSON builder of the Texture Atlas export
// (toonz/atlasjson.h): the files parse back, have the documented keys, list the
// sprites of the input, cover the timeline of every layer without gaps or
// overlaps, and keep the values of the matrices where the doc says. Symbols:
// the symbol dictionary, symbol instances (also nested ones) and their keys,
// the checks of the symbol input (undefined and recursive symbols), and the
// symbol instances made from the frames of a sub-timeline (instanceRuns()).

#include "toonz/atlasjson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QString>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AtlasJson;

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

//------------------------------------------------------------------

QJsonObject parse(const QByteArray &bytes, const char *what) {
  QJsonParseError error;
  QJsonDocument doc = QJsonDocument::fromJson(bytes, &error);
  require(error.error == QJsonParseError::NoError,
          std::string(what) + ": the JSON does not parse");
  require(doc.isObject(), std::string(what) + ": the root is an object");
  return doc.object();
}

//! Every number of a JSON value is finite, and there is no null
void requireFinite(const QJsonValue &value, const char *what) {
  require(!value.isNull() && !value.isUndefined(),
          std::string(what) + ": null or undefined value");
  if (value.isDouble())
    require(std::isfinite(value.toDouble()),
            std::string(what) + ": non finite number");
  else if (value.isArray())
    for (const QJsonValue &v : value.toArray()) requireFinite(v, what);
  else if (value.isObject()) {
    QJsonObject object = value.toObject();
    for (const QString &key : object.keys()) requireFinite(object[key], what);
  }
}

//------------------------------------------------------------------

Page samplePage() {
  Page page;
  page.m_image  = "spritemap1.png";
  page.m_width  = 256;
  page.m_height = 128;
  Sprite a, b, c;
  a.m_name = "A_0001";
  a.m_x = 0, a.m_y = 0, a.m_w = 100, a.m_h = 50;
  b.m_name = "A_0002";
  b.m_x = 102, b.m_y = 0, b.m_w = 64, b.m_h = 64;
  c.m_name       = "B_0001";
  c.m_x          = 0;
  c.m_y          = 52;
  c.m_w          = 30;
  c.m_h          = 70;
  c.m_rotated    = true;
  page.m_sprites = {a, b, c};
  return page;
}

Frame frame(int start, int duration, const char *sprite,
            const Matrix3D &matrix = identityMatrix()) {
  Frame f;
  f.m_start    = start;
  f.m_duration = duration;
  f.m_sprite   = sprite;
  f.m_matrix   = matrix;
  return f;
}

Frame symbolFrame(int start, int duration, const char *symbol, int firstFrame,
                  LoopMode loop, const Matrix3D &matrix = identityMatrix()) {
  Frame f;
  f.m_start      = start;
  f.m_duration   = duration;
  f.m_symbol     = symbol;
  f.m_firstFrame = firstFrame;
  f.m_loop       = loop;
  f.m_matrix     = matrix;
  return f;
}

//! The "L" array of an object with a "TL"
QJsonArray layersOf(const QJsonObject &owner) {
  return owner["TL"].toObject()["L"].toArray();
}

//! The frames of every layer follow each other from 0 to total
void requireCovers(const QJsonArray &layers, int total,
                   const std::string &what) {
  for (const QJsonValue &layer : layers) {
    int cursor = 0;
    for (const QJsonValue &v : layer.toObject()["FR"].toArray()) {
      QJsonObject f = v.toObject();
      require(f.contains("I") && f.contains("DU") && f.contains("E"),
              what + ": frame keys");
      require(f["I"].toInt(-1) == cursor,
              what + ": frames follow each other without gap or overlap");
      require(f["DU"].toInt(-1) >= 1, what + ": duration is positive");
      cursor += f["DU"].toInt();
    }
    require(cursor == total, what + ": every layer covers its timeline");
  }
}

//! The element of a frame entry, which must have one
QJsonObject elementOf(const QJsonValue &frameEntry, const std::string &what) {
  QJsonArray elements = frameEntry.toObject()["E"].toArray();
  require(elements.size() == 1, what + ": one element");
  return elements[0].toObject();
}

void requireMatrix(const QJsonValue &m3d, const double (&expected)[16],
                   const std::string &what) {
  QJsonArray m = m3d.toArray();
  require(m.size() == 16, what + ": 16 numbers");
  for (int i = 0; i < 16; ++i)
    require(m[i].isDouble() && m[i].toDouble() == expected[i],
            what + ": matrix value at index " + std::to_string(i));
}

//------------------------------------------------------------------

void testSpritemap() {
  Page page = samplePage();
  QJsonObject root =
      parse(toBytes(spritemapJson(page, "OpenToonz", "1.7.1")), "spritemap");

  require(root.contains("ATLAS") && root.contains("meta"),
          "spritemap: ATLAS and meta");
  QJsonArray sprites = root["ATLAS"].toObject()["SPRITES"].toArray();
  require((int)sprites.size() == (int)page.m_sprites.size(),
          "spritemap: sprite count");

  QSet<QString> names;
  for (int i = 0; i < sprites.size(); ++i) {
    QJsonObject item = sprites[i].toObject();
    require(item.contains("SPRITE"), "spritemap: SPRITE key");
    QJsonObject s = item["SPRITE"].toObject();
    for (const char *key : {"name", "x", "y", "w", "h", "rotated"})
      require(s.contains(QString::fromLatin1(key)),
              std::string("spritemap: sprite key ") + key);

    const Sprite &in = page.m_sprites[i];
    require(s["name"].toString() == in.m_name, "spritemap: name");
    require(s["x"].toInt(-1) == in.m_x && s["y"].toInt(-1) == in.m_y &&
                s["w"].toInt(-1) == in.m_w && s["h"].toInt(-1) == in.m_h,
            "spritemap: rectangle");
    require(s["rotated"].isBool() && s["rotated"].toBool() == in.m_rotated,
            "spritemap: rotated");
    names.insert(s["name"].toString());
  }
  require(names.size() == sprites.size(), "spritemap: names are unique");

  QJsonObject meta = root["meta"].toObject();
  for (const char *key :
       {"app", "version", "image", "format", "size", "resolution"})
    require(meta.contains(QString::fromLatin1(key)),
            std::string("spritemap: meta key ") + key);
  require(meta["app"].toString() == "OpenToonz", "spritemap: app");
  require(meta["version"].toString() == "1.7.1", "spritemap: version");
  require(meta["image"].toString() == "spritemap1.png", "spritemap: image");
  require(meta["format"].toString() == "RGBA8888", "spritemap: format");
  require(meta["size"].toObject()["w"].toInt(-1) == 256 &&
              meta["size"].toObject()["h"].toInt(-1) == 128,
          "spritemap: size");
  require(meta["resolution"].isString() && meta["resolution"].toString() == "1",
          "spritemap: resolution is the string 1");
  requireFinite(root, "spritemap");

  page.m_resolution = 0.5;
  root = parse(toBytes(spritemapJson(page, "a", "b")), "spritemap 50%");
  require(root["meta"].toObject()["resolution"].toString() == "0.5",
          "spritemap: resolution 0.5");

  // No sprite
  page.m_sprites.clear();
  root = parse(toBytes(spritemapJson(page, "a", "b")), "spritemap empty");
  require(root["ATLAS"].toObject()["SPRITES"].toArray().isEmpty(),
          "spritemap: empty sprite list");
}

//------------------------------------------------------------------

// The sprites of samplePage() on two layers
Animation sampleAnimation() {
  Animation an;
  an.m_name        = "scene";
  an.m_symbolName  = "scene_symbol";
  an.m_frameRate   = 12.0;
  an.m_stageWidth  = 1920;
  an.m_stageHeight = 1080;

  Layer top, bottom;
  top.m_name = "Col2";
  top.m_frames.push_back(frame(
      0, 3, "A_0001", matrixFromAffine(1.5, 0.25, -0.5, 2.0, 10.0, -20.0)));
  top.m_frames.push_back(frame(5, 2, "A_0002"));  // gap of two frames before
  // The layer is shorter than the timeline: blank at the end
  bottom.m_name = "Col1";
  bottom.m_frames.push_back(frame(0, 10, "B_0001"));
  an.m_layers = {top, bottom};
  return an;
}

void testAnimationStructure() {
  Animation an            = sampleAnimation();
  std::vector<Page> pages = {samplePage()};
  require(validate(pages, an).isEmpty(), "animation: valid input");

  QJsonObject root = parse(toBytes(animationJson(an)), "animation");
  require(root.contains("AN") && root.contains("MD") && root.contains("SD"),
          "animation: AN, MD and SD");

  QJsonObject object = root["AN"].toObject();
  require(object["N"].toString() == "scene", "animation: AN.N");
  require(object["SN"].toString() == "scene_symbol", "animation: AN.SN");
  QJsonArray layers = object["TL"].toObject()["L"].toArray();
  require(layers.size() == 2, "animation: two layers");
  require(layers[0].toObject()["LN"].toString() == "Col2" &&
              layers[1].toObject()["LN"].toString() == "Col1",
          "animation: layers keep their order");

  QJsonObject md = root["MD"].toObject();
  require(md["FRT"].toDouble(-1) == 12.0, "animation: MD.FRT");
  require(md["W"].toInt(-1) == 1920 && md["H"].toInt(-1) == 1080,
          "animation: MD.W and MD.H");
  require(root["SD"].toObject()["S"].isArray(), "animation: SD.S");

  // Timelines: from 0 to the same end for all layers, no gap, no overlap
  const int total = 10;
  QSet<QString> layerNames;
  for (int l = 0; l < layers.size(); ++l) {
    QJsonObject layer = layers[l].toObject();
    layerNames.insert(layer["LN"].toString());
    QJsonArray frames = layer["FR"].toArray();
    require(!frames.isEmpty(), "animation: layer has frames");
    int cursor = 0;
    for (int i = 0; i < frames.size(); ++i) {
      QJsonObject f = frames[i].toObject();
      require(f.contains("I") && f.contains("DU") && f.contains("E"),
              "animation: frame keys");
      require(f["I"].toInt(-1) == cursor,
              "animation: frames follow each other without gap or overlap");
      require(f["DU"].toInt(-1) >= 1, "animation: duration is positive");
      cursor += f["DU"].toInt();
      QJsonArray elements = f["E"].toArray();
      require(elements.size() <= 1, "animation: at most one sprite");
      if (!elements.isEmpty())
        require(elements[0].toObject().contains("ASI"), "animation: ASI");
    }
    require(cursor == total, "animation: every layer covers the timeline");
  }
  require(layerNames.size() == layers.size(), "animation: layer names unique");

  // Top layer: A_0001 for 3 frames, 2 blank, A_0002 for 2, 3 blank
  QJsonArray fr = layers[0].toObject()["FR"].toArray();
  require(fr.size() == 4, "animation: top layer frame count");
  const int starts[4]    = {0, 3, 5, 7};
  const int durations[4] = {3, 2, 2, 3};
  const bool blank[4]    = {false, true, false, true};
  for (int i = 0; i < 4; ++i) {
    QJsonObject f = fr[i].toObject();
    require(f["I"].toInt(-1) == starts[i] && f["DU"].toInt(-1) == durations[i],
            "animation: top layer start and duration " + std::to_string(i));
    require(f["E"].toArray().isEmpty() == blank[i],
            "animation: top layer blank frames " + std::to_string(i));
  }
}

//------------------------------------------------------------------

void testMatrix() {
  Animation an         = sampleAnimation();
  QJsonObject root     = parse(toBytes(animationJson(an)), "matrix");
  QJsonArray layers    = root["AN"].toObject()["TL"].toObject()["L"].toArray();
  QJsonObject first    = layers[0].toObject()["FR"].toArray()[0].toObject();
  QJsonObject instance = first["E"].toArray()[0].toObject()["ASI"].toObject();
  require(instance["N"].toString() == "A_0001", "matrix: sprite name");

  QJsonArray m = instance["M3D"].toArray();
  require(m.size() == 16, "matrix: 16 numbers");
  for (int i = 0; i < 16; ++i)
    require(m[i].isDouble() && std::isfinite(m[i].toDouble()),
            "matrix: finite number");

  // [a b 0 0  c d 0 0  0 0 1 0  tx ty 0 1], column-major
  const double expected[16] = {1.5, 0.25, 0, 0, -0.5, 2.0,   0, 0,
                               0,   0,    1, 0, 10.0, -20.0, 0, 1};
  for (int i = 0; i < 16; ++i)
    require(m[i].toDouble() == expected[i],
            "matrix: value at index " + std::to_string(i));

  // The identity for a frame which has none given
  QJsonObject second = layers[0].toObject()["FR"].toArray()[2].toObject();
  QJsonArray m2 =
      second["E"].toArray()[0].toObject()["ASI"].toObject()["M3D"].toArray();
  Matrix3D id = identityMatrix();
  for (int i = 0; i < 16; ++i)
    require(m2[i].toDouble() == id[i], "matrix: identity");

  // The helper puts its values where the doc says
  Matrix3D mat = matrixFromAffine(1, 2, 3, 4, 5, 6);
  require(mat[0] == 1 && mat[1] == 2 && mat[4] == 3 && mat[5] == 4 &&
              mat[12] == 5 && mat[13] == 6 && mat[10] == 1 && mat[15] == 1 &&
              mat[2] == 0 && mat[3] == 0 && mat[6] == 0 && mat[7] == 0 &&
              mat[8] == 0 && mat[9] == 0 && mat[11] == 0 && mat[14] == 0,
          "matrix: matrixFromAffine layout");
}

//------------------------------------------------------------------

void testTimelineRepair() {
  // Unsorted frames, an overlap and a gap
  Animation an;
  an.m_frameRate = 24;
  Layer l;
  l.m_name = "L";
  l.m_frames.push_back(frame(5, 3, "B"));
  l.m_frames.push_back(frame(0, 4, "A"));
  l.m_frames.push_back(frame(3, 4, "C"));
  l.m_frames.push_back(frame(9, 0, "A"));  // no duration: dropped
  Layer l2;
  l2.m_name = "L2";
  l2.m_frames.push_back(frame(2, 2, "A"));
  an.m_layers = {l, l2};

  QJsonObject root  = parse(toBytes(animationJson(an)), "repair");
  QJsonArray layers = root["AN"].toObject()["TL"].toObject()["L"].toArray();

  // L: A [0,3) cut by C, C [3,5) cut by B, B [5,8)
  QJsonArray fr = layers[0].toObject()["FR"].toArray();
  require(fr.size() == 3, "repair: three frames");
  const int starts[3]    = {0, 3, 5};
  const int durations[3] = {3, 2, 3};
  const char *sprites[3] = {"A", "C", "B"};
  for (int i = 0; i < 3; ++i) {
    QJsonObject f = fr[i].toObject();
    require(f["I"].toInt(-1) == starts[i] && f["DU"].toInt(-1) == durations[i],
            "repair: start and duration " + std::to_string(i));
    require(f["E"].toArray()[0].toObject()["ASI"].toObject()["N"].toString() ==
                sprites[i],
            "repair: sprite " + std::to_string(i));
  }

  // L2: blank, A, blank up to the end of the longest layer (8)
  fr = layers[1].toObject()["FR"].toArray();
  require(fr.size() == 3, "repair: gaps are filled");
  int cursor = 0;
  for (int i = 0; i < fr.size(); ++i) {
    QJsonObject f = fr[i].toObject();
    require(f["I"].toInt(-1) == cursor, "repair: contiguous");
    cursor += f["DU"].toInt();
  }
  require(cursor == 8, "repair: same length as the longest layer");
  require(fr[0].toObject()["E"].toArray().isEmpty() &&
              !fr[1].toObject()["E"].toArray().isEmpty() &&
              fr[2].toObject()["E"].toArray().isEmpty(),
          "repair: blank, sprite, blank");

  // The same start: the last frame wins
  Animation same;
  same.m_frameRate = 24;
  Layer l3;
  l3.m_name = "L";
  l3.m_frames.push_back(frame(0, 4, "A"));
  l3.m_frames.push_back(frame(0, 2, "B"));
  same.m_layers = {l3};
  root          = parse(toBytes(animationJson(same)), "same start");
  fr            = root["AN"]
           .toObject()["TL"]
           .toObject()["L"]
           .toArray()[0]
           .toObject()["FR"]
           .toArray();
  require(fr.size() == 2 && fr[0].toObject()["I"].toInt(-1) == 0 &&
              fr[0].toObject()["DU"].toInt(-1) == 2 &&
              fr[1].toObject()["I"].toInt(-1) == 2 &&
              fr[1].toObject()["DU"].toInt(-1) == 2 &&
              fr[1].toObject()["E"].toArray().isEmpty(),
          "repair: same start keeps the later frame, then a gap");
  require(fr[0].toObject()["E"]
                  .toArray()[0]
                  .toObject()["ASI"]
                  .toObject()["N"]
                  .toString() == "B",
          "repair: same start: the later frame wins");

  // The timeline length can be longer than the frames
  Animation longer    = sampleAnimation();
  longer.m_frameCount = 14;
  root                = parse(toBytes(animationJson(longer)), "frame count");
  layers              = root["AN"].toObject()["TL"].toObject()["L"].toArray();
  for (int l = 0; l < layers.size(); ++l) {
    int end = 0;
    for (const QJsonValue &v : layers[l].toObject()["FR"].toArray()) {
      QJsonObject f = v.toObject();
      end           = f["I"].toInt() + f["DU"].toInt();
    }
    require(end == 14, "frame count: layers extend to the frame count");
  }
}

//------------------------------------------------------------------

void testNoFrames() {
  Animation an;
  an.m_name = "empty";
  Layer l;
  l.m_name          = "L";
  an.m_layers       = {l};
  QJsonObject root  = parse(toBytes(animationJson(an)), "no frames");
  QJsonArray layers = root["AN"].toObject()["TL"].toObject()["L"].toArray();
  require(layers.size() == 1 && layers[0].toObject()["FR"].toArray().isEmpty(),
          "no frames: an empty frame list");
  require(!root["MD"].toObject().contains("W"),
          "no frames: no stage size when unknown");

  Animation none;
  root = parse(toBytes(animationJson(none)), "no layers");
  require(root["AN"].toObject()["TL"].toObject()["L"].toArray().isEmpty(),
          "no layers: an empty layer list");
}

//------------------------------------------------------------------

void testNumbersAreFinite() {
  Animation an = sampleAnimation();
  Matrix3D bad = identityMatrix();
  bad[12]      = std::numeric_limits<double>::quiet_NaN();
  bad[13]      = std::numeric_limits<double>::infinity();
  an.m_layers[0].m_frames.push_back(frame(8, 1, "B_0001", bad));

  QJsonObject root = parse(toBytes(animationJson(an)), "finite");
  requireFinite(root, "finite");

  QJsonArray layers = root["AN"].toObject()["TL"].toObject()["L"].toArray();
  QJsonArray frames = layers[0].toObject()["FR"].toArray();
  // The frame with the bad matrix (8, 1) is followed by a blank one (9, 1)
  QJsonObject badFrame = frames[frames.size() - 2].toObject();
  require(badFrame["I"].toInt(-1) == 8, "finite: the frame is there");
  QJsonArray m =
      badFrame["E"].toArray()[0].toObject()["ASI"].toObject()["M3D"].toArray();
  Matrix3D id = identityMatrix();
  for (int i = 0; i < 16; ++i)
    require(m[i].toDouble() == id[i], "finite: identity instead of NaN");

  Animation fps   = sampleAnimation();
  fps.m_frameRate = std::numeric_limits<double>::quiet_NaN();
  root            = parse(toBytes(animationJson(fps)), "finite fps");
  requireFinite(root, "finite fps");
}

//------------------------------------------------------------------

void testValidate() {
  std::vector<Page> pages = {samplePage()};
  Animation an            = sampleAnimation();
  require(validate(pages, an).isEmpty(), "validate: good input");

  // Sprite names must be unique, over all the pages
  std::vector<Page> twice = pages;
  twice.push_back(samplePage());
  twice[1].m_image = "spritemap2.png";
  require(!validate(twice, an).isEmpty(), "validate: duplicate sprite names");
  for (Sprite &s : twice[1].m_sprites) s.m_name += "_p2";
  require(validate(twice, an).isEmpty(), "validate: unique over two pages");

  // Unknown sprite
  Animation unknown = an;
  unknown.m_layers[0].m_frames.push_back(frame(9, 1, "nope"));
  require(!validate(pages, unknown).isEmpty(), "validate: unknown sprite");

  // Duplicate layer names
  Animation sameLayer          = an;
  sameLayer.m_layers[1].m_name = sameLayer.m_layers[0].m_name;
  require(!validate(pages, sameLayer).isEmpty(), "validate: duplicate layers");

  // A sprite outside its page
  std::vector<Page> outside   = pages;
  outside[0].m_sprites[0].m_w = 1000;
  require(!validate(outside, an).isEmpty(), "validate: sprite outside page");

  // A sprite without name or size
  std::vector<Page> noName      = pages;
  noName[0].m_sprites[1].m_name = QString();
  require(!validate(noName, an).isEmpty(), "validate: empty sprite name");
  std::vector<Page> noSize   = pages;
  noSize[0].m_sprites[1].m_h = 0;
  require(!validate(noSize, an).isEmpty(), "validate: empty sprite size");

  // Frame rate
  Animation fps   = an;
  fps.m_frameRate = 0.0;
  require(!validate(pages, fps).isEmpty(), "validate: zero frame rate");
  fps.m_frameRate = std::numeric_limits<double>::infinity();
  require(!validate(pages, fps).isEmpty(), "validate: infinite frame rate");

  // Blank frames (no sprite name) are fine
  Animation blank = an;
  blank.m_layers[0].m_frames.push_back(frame(9, 1, ""));
  require(validate(pages, blank).isEmpty(), "validate: blank frame");
}

//------------------------------------------------------------------
// Symbols
//------------------------------------------------------------------

/*
sampleAnimation() with, on top, a layer "Col3" showing the symbol "Outer",
which holds a sprite and an instance of the symbol "Inner":
  Inner (4 frames): Col1: A_0001 [0,2), A_0002 [2,4)
  Outer (6 frames): Col2: Inner [0,6) FF 1 loop, scaled by 0.5, moved (4, 8)
                    Col1: B_0001 [1,4)
  main Col3: Outer [0,4) FF 2 play once, moved (100, 50); Outer [6,8) FF 5
             single frame
*/
Animation symbolAnimation() {
  Animation an = sampleAnimation();

  Symbol inner;
  inner.m_name       = "Inner";
  inner.m_frameCount = 4;
  Layer innerLayer;
  innerLayer.m_name = "Col1";
  innerLayer.m_frames.push_back(frame(0, 2, "A_0001"));
  innerLayer.m_frames.push_back(frame(2, 2, "A_0002"));
  inner.m_layers = {innerLayer};

  Symbol outer;
  outer.m_name       = "Outer";
  outer.m_frameCount = 6;
  Layer outerTop, outerBottom;
  outerTop.m_name = "Col2";
  outerTop.m_frames.push_back(
      symbolFrame(0, 6, "Inner", 1, LoopMode::Loop,
                  matrixFromAffine(0.5, 0, 0, 0.5, 4, 8)));
  // The same layer name as in Inner and in the main timeline: layer names
  // are unique in each timeline only
  outerBottom.m_name = "Col1";
  outerBottom.m_frames.push_back(frame(1, 3, "B_0001"));
  outer.m_layers = {outerTop, outerBottom};

  Layer top;
  top.m_name = "Col3";
  top.m_frames.push_back(symbolFrame(0, 4, "Outer", 2, LoopMode::PlayOnce,
                                     matrixFromAffine(1, 0, 0, 1, 100, 50)));
  top.m_frames.push_back(symbolFrame(6, 2, "Outer", 5, LoopMode::SingleFrame));
  an.m_layers.insert(an.m_layers.begin(), top);

  an.m_symbols = {inner, outer};
  return an;
}

void testSymbols() {
  Animation an            = symbolAnimation();
  std::vector<Page> pages = {samplePage()};
  require(validate(pages, an).isEmpty(), "symbols: valid input");

  QJsonObject root = parse(toBytes(animationJson(an)), "symbols");
  requireFinite(root, "symbols");

  // The symbol dictionary, in the order of the input
  QJsonArray symbols = root["SD"].toObject()["S"].toArray();
  require(symbols.size() == 2, "symbols: two symbols in SD.S");
  QJsonObject inner = symbols[0].toObject();
  QJsonObject outer = symbols[1].toObject();
  require(
      inner["SN"].toString() == "Inner" && outer["SN"].toString() == "Outer",
      "symbols: SN of the symbols");
  require(inner.contains("TL") && outer.contains("TL"), "symbols: TL");

  // Inner: one layer of two sprites, over its 4 frames
  QJsonArray innerLayers = layersOf(inner);
  require(innerLayers.size() == 1 &&
              innerLayers[0].toObject()["LN"].toString() == "Col1",
          "symbols: the layer of Inner");
  requireCovers(innerLayers, 4, "symbols: Inner");
  QJsonArray fr = innerLayers[0].toObject()["FR"].toArray();
  require(fr.size() == 2, "symbols: Inner has two frames");
  require(elementOf(fr[0], "symbols: Inner frame 0")["ASI"]
                      .toObject()["N"]
                      .toString() == "A_0001" &&
              elementOf(fr[1], "symbols: Inner frame 1")["ASI"]
                      .toObject()["N"]
                      .toString() == "A_0002",
          "symbols: the sprites of Inner");

  // Outer: the nested instance of Inner on top, a sprite below, over its 6
  // frames
  QJsonArray outerLayers = layersOf(outer);
  require(outerLayers.size() == 2 &&
              outerLayers[0].toObject()["LN"].toString() == "Col2" &&
              outerLayers[1].toObject()["LN"].toString() == "Col1",
          "symbols: the layers of Outer, in order");
  requireCovers(outerLayers, 6, "symbols: Outer");
  fr = outerLayers[0].toObject()["FR"].toArray();
  require(fr.size() == 1 && fr[0].toObject()["I"].toInt(-1) == 0 &&
              fr[0].toObject()["DU"].toInt(-1) == 6,
          "symbols: the nested instance covers Outer");
  QJsonObject element = elementOf(fr[0], "symbols: nested instance");
  require(element.contains("SI") && !element.contains("ASI"),
          "symbols: a symbol instance is an SI element");
  QJsonObject si = element["SI"].toObject();
  for (const char *key : {"SN", "IN", "ST", "FF", "LP", "TRP", "M3D"})
    require(si.contains(QString::fromLatin1(key)),
            std::string("symbols: SI key ") + key);
  require(si["SN"].toString() == "Inner", "symbols: nested SN");
  require(si["IN"].isString() && si["IN"].toString().isEmpty(),
          "symbols: IN is an empty string");
  require(si["ST"].toString() == "G", "symbols: ST is G (graphic)");
  require(si["FF"].toInt(-1) == 1, "symbols: nested FF");
  require(si["LP"].toString() == "LP", "symbols: nested loop mode LP");
  QJsonObject trp = si["TRP"].toObject();
  require(trp["x"].isDouble() && trp["x"].toDouble(-1) == 0.0 &&
              trp["y"].isDouble() && trp["y"].toDouble(-1) == 0.0,
          "symbols: TRP is (0, 0)");
  const double nested[16] = {0.5, 0, 0, 0, 0, 0.5, 0, 0,
                             0,   0, 1, 0, 4, 8,   0, 1};
  requireMatrix(si["M3D"], nested, "symbols: nested M3D");

  fr = outerLayers[1].toObject()["FR"].toArray();
  require(fr.size() == 3 && fr[0].toObject()["E"].toArray().isEmpty() &&
              fr[2].toObject()["E"].toArray().isEmpty(),
          "symbols: blank, sprite, blank in Outer");
  require(elementOf(fr[1], "symbols: Outer sprite")["ASI"]
                  .toObject()["N"]
                  .toString() == "B_0001",
          "symbols: the sprite of Outer");

  // The main timeline: the instances of Outer on the top layer
  QJsonArray layers = layersOf(root["AN"].toObject());
  require(layers.size() == 3 && layers[0].toObject()["LN"].toString() == "Col3",
          "symbols: the instance layer is on top");
  requireCovers(layers, 10, "symbols: main timeline");
  fr = layers[0].toObject()["FR"].toArray();
  require(fr.size() == 4, "symbols: four frames on the top layer");
  const int starts[4]    = {0, 4, 6, 8};
  const int durations[4] = {4, 2, 2, 2};
  for (int i = 0; i < 4; ++i)
    require(fr[i].toObject()["I"].toInt(-1) == starts[i] &&
                fr[i].toObject()["DU"].toInt(-1) == durations[i],
            "symbols: top layer start and duration " + std::to_string(i));
  require(fr[1].toObject()["E"].toArray().isEmpty() &&
              fr[3].toObject()["E"].toArray().isEmpty(),
          "symbols: blank frames between the instances");

  si = elementOf(fr[0], "symbols: first instance")["SI"].toObject();
  require(si["SN"].toString() == "Outer" && si["FF"].toInt(-1) == 2 &&
              si["LP"].toString() == "PO" && si["ST"].toString() == "G",
          "symbols: first instance fields");
  const double moved[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100, 50, 0, 1};
  requireMatrix(si["M3D"], moved, "symbols: first instance M3D");

  si = elementOf(fr[2], "symbols: second instance")["SI"].toObject();
  require(si["SN"].toString() == "Outer" && si["FF"].toInt(-1) == 5 &&
              si["LP"].toString() == "SF",
          "symbols: second instance fields");
  const double identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  requireMatrix(si["M3D"], identity, "symbols: second instance M3D");

  // The other layers are unchanged
  require(layers[1].toObject()["LN"].toString() == "Col2" &&
              layers[2].toObject()["LN"].toString() == "Col1",
          "symbols: the sprite layers follow");

  // A symbol is as long as its frames, if more than its frame count
  Animation longer                 = an;
  longer.m_symbols[0].m_frameCount = 0;
  longer.m_symbols[0].m_layers[0].m_frames.push_back(frame(4, 3, "A_0001"));
  root = parse(toBytes(animationJson(longer)), "symbol length");
  requireCovers(layersOf(root["SD"].toObject()["S"].toArray()[0].toObject()), 7,
                "symbol length");
}

//------------------------------------------------------------------

void testSymbolValidate() {
  std::vector<Page> pages = {samplePage()};
  const Animation an      = symbolAnimation();
  require(validate(pages, an).isEmpty(), "symbol validate: good input");

  // An undefined symbol, in the main timeline or in a symbol
  Animation undefined = an;
  undefined.m_layers[0].m_frames.push_back(
      symbolFrame(9, 1, "Nope", 0, LoopMode::Loop));
  require(!validate(pages, undefined).isEmpty(),
          "symbol validate: undefined symbol in the main timeline");
  undefined                                               = an;
  undefined.m_symbols[1].m_layers[0].m_frames[0].m_symbol = "Nope";
  require(!validate(pages, undefined).isEmpty(),
          "symbol validate: undefined symbol in a symbol");
  undefined = an;
  undefined.m_symbols.erase(undefined.m_symbols.begin());  // Inner
  require(!validate(pages, undefined).isEmpty(),
          "symbol validate: nested symbol not defined");

  // Recursion: a symbol which shows itself, directly or through another
  Animation recursive = an;
  recursive.m_symbols[0].m_layers.push_back(Layer());
  recursive.m_symbols[0].m_layers.back().m_name = "Self";
  recursive.m_symbols[0].m_layers.back().m_frames.push_back(
      symbolFrame(0, 1, "Inner", 0, LoopMode::Loop));
  require(!validate(pages, recursive).isEmpty(),
          "symbol validate: a symbol inside itself");
  recursive = an;
  recursive.m_symbols[0].m_layers.push_back(Layer());
  recursive.m_symbols[0].m_layers.back().m_name = "Back";
  recursive.m_symbols[0].m_layers.back().m_frames.push_back(
      symbolFrame(0, 1, "Outer", 0, LoopMode::SingleFrame));
  require(!validate(pages, recursive).isEmpty(),
          "symbol validate: Inner inside Outer inside Inner");
  Animation third = an;
  Symbol extra;
  extra.m_name = "Extra";
  extra.m_layers.push_back(Layer());
  extra.m_layers.back().m_name = "L";
  extra.m_layers.back().m_frames.push_back(
      symbolFrame(0, 2, "Outer", 0, LoopMode::Loop));
  third.m_symbols.push_back(extra);
  require(validate(pages, third).isEmpty(),
          "symbol validate: a symbol used by two symbols is no recursion");
  third.m_symbols[0].m_layers[0].m_frames.push_back(
      symbolFrame(5, 1, "Extra", 0, LoopMode::Loop));
  require(!validate(pages, third).isEmpty(),
          "symbol validate: a cycle of three symbols");

  // Names
  Animation names           = an;
  names.m_symbols[1].m_name = "Inner";
  require(!validate(pages, names).isEmpty(),
          "symbol validate: duplicate symbol names");
  names                     = an;
  names.m_symbols[1].m_name = QString();
  require(!validate(pages, names).isEmpty(),
          "symbol validate: empty symbol name");
  names                     = an;
  names.m_symbols[0].m_name = an.m_symbolName;
  require(!validate(pages, names).isEmpty(),
          "symbol validate: a symbol named as the main symbol");
  names                                 = an;
  names.m_symbols[1].m_layers[1].m_name = "Col2";
  require(!validate(pages, names).isEmpty(),
          "symbol validate: duplicate layer names in a symbol");

  // First frames inside the symbol's timeline
  Animation first                            = an;
  first.m_layers[0].m_frames[0].m_firstFrame = 6;  // Outer has 6 frames
  require(!validate(pages, first).isEmpty(),
          "symbol validate: first frame after the symbol's end");
  first.m_layers[0].m_frames[0].m_firstFrame = -1;
  require(!validate(pages, first).isEmpty(),
          "symbol validate: negative first frame");
  first.m_layers[0].m_frames[0].m_firstFrame = 5;
  require(validate(pages, first).isEmpty(),
          "symbol validate: the last frame of the symbol");

  // A frame shows a sprite or a symbol, not both
  Animation both                        = an;
  both.m_layers[0].m_frames[0].m_sprite = "A_0001";
  require(!validate(pages, both).isEmpty(),
          "symbol validate: sprite and symbol on one frame");

  // Unknown sprite in a symbol
  Animation sprite                                     = an;
  sprite.m_symbols[0].m_layers[0].m_frames[0].m_sprite = "nope";
  require(!validate(pages, sprite).isEmpty(),
          "symbol validate: unknown sprite in a symbol");
}

//------------------------------------------------------------------

void testSymbolNumbers() {
  // A symbol instance with a non finite matrix is written with the identity
  Animation an = symbolAnimation();
  an.m_layers[0].m_frames[0].m_matrix[12] =
      std::numeric_limits<double>::quiet_NaN();
  an.m_symbols[1].m_layers[0].m_frames[0].m_matrix[0] =
      std::numeric_limits<double>::infinity();
  QJsonObject root = parse(toBytes(animationJson(an)), "symbol finite");
  requireFinite(root, "symbol finite");
  const double identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  QJsonObject si =
      elementOf(
          layersOf(root["AN"].toObject())[0].toObject()["FR"].toArray()[0],
          "symbol finite: main")["SI"]
          .toObject();
  requireMatrix(si["M3D"], identity, "symbol finite: main instance");
  si = elementOf(layersOf(root["SD"].toObject()["S"].toArray()[1].toObject())[0]
                     .toObject()["FR"]
                     .toArray()[0],
                 "symbol finite: nested")["SI"]
           .toObject();
  requireMatrix(si["M3D"], identity, "symbol finite: nested instance");

  // A part before frame 0 is cut: a playing instance moves on its first frame
  Animation cut = symbolAnimation();
  cut.m_layers[0].m_frames.clear();
  cut.m_layers[0].m_frames.push_back(
      symbolFrame(-3, 5, "Inner", 2, LoopMode::Loop));  // Inner: 4 frames
  cut.m_layers[1].m_frames.clear();
  cut.m_layers[1].m_frames.push_back(
      symbolFrame(-2, 4, "Outer", 3, LoopMode::PlayOnce));  // Outer: 6 frames
  cut.m_layers[2].m_frames.clear();
  cut.m_layers[2].m_frames.push_back(
      symbolFrame(-4, 6, "Outer", 1, LoopMode::SingleFrame));
  std::vector<Page> pages = {samplePage()};
  require(validate(pages, cut).isEmpty(), "cut: valid input");
  root                     = parse(toBytes(animationJson(cut)), "cut");
  QJsonArray layers        = layersOf(root["AN"].toObject());
  const int firstFrames[3] = {1, 5, 1};  // (2 + 3) % 4, min(3 + 2, 5), 1
  const int lengths[3]     = {2, 2, 2};
  for (int l = 0; l < 3; ++l) {
    QJsonObject f = layers[l].toObject()["FR"].toArray()[0].toObject();
    require(f["I"].toInt(-1) == 0 && f["DU"].toInt(-1) == lengths[l],
            "cut: the part before frame 0 is cut " + std::to_string(l));
    require(
        elementOf(f, "cut")["SI"].toObject()["FF"].toInt(-1) == firstFrames[l],
        "cut: first frame " + std::to_string(l));
  }
}

//------------------------------------------------------------------
// instanceRuns()

// The frame of the symbol that a player shows at the n-th frame of an
// instance, as the doc describes the loop modes
int playedFrame(const InstanceRun &run, int n, int length) {
  switch (run.m_loop) {
  case LoopMode::Loop:
    return (run.m_firstFrame + n) % length;
  case LoopMode::PlayOnce:
    return std::min(run.m_firstFrame + n, length - 1);
  default:
    return run.m_firstFrame;
  }
}

// The runs play back the child frames exactly, are sorted, do not overlap,
// and stay in their segment
void requirePlaysBack(const std::vector<int> &childFrames, int length,
                      const std::vector<InstanceRun> &runs,
                      const std::vector<int> &segments,
                      const std::string &what) {
  std::vector<int> played(childFrames.size(), -1);
  int end = 0;
  for (const InstanceRun &run : runs) {
    require(run.m_start >= end && run.m_duration >= 1,
            what + ": runs sorted, not overlapping");
    require(run.m_start + run.m_duration <= (int)childFrames.size(),
            what + ": runs inside the timeline");
    require(run.m_firstFrame >= 0 && run.m_firstFrame < length,
            what + ": first frame inside the symbol");
    for (int n = 0; n < run.m_duration; ++n) {
      played[run.m_start + n] = playedFrame(run, n, length);
      if (!segments.empty())
        require(segments[run.m_start + n] == segments[run.m_start],
                what + ": a run stays in its segment");
    }
    end = run.m_start + run.m_duration;
  }
  for (size_t f = 0; f < childFrames.size(); ++f) {
    int expected =
        (childFrames[f] >= 0 && childFrames[f] < length) ? childFrames[f] : -1;
    require(played[f] == expected,
            what + ": frame " + std::to_string(f) + " plays back");
  }
}

void requireRun(const InstanceRun &run, int start, int duration, int firstFrame,
                LoopMode loop, const std::string &what) {
  require(run.m_start == start && run.m_duration == duration,
          what + ": start and duration");
  require(run.m_firstFrame == firstFrame, what + ": first frame");
  require(run.m_loop == loop, what + ": loop mode");
}

std::vector<InstanceRun> checkedRuns(
    const std::vector<int> &childFrames, int length, const std::string &what,
    const std::vector<int> &segments = std::vector<int>()) {
  std::vector<InstanceRun> runs = instanceRuns(childFrames, length, segments);
  requirePlaysBack(childFrames, length, runs, segments, what);
  return runs;
}

void testInstanceRuns() {
  std::vector<InstanceRun> runs;

  runs = checkedRuns({0, 1, 2, 3}, 10, "runs: playing");
  require(runs.size() == 1, "runs: playing is one run");
  requireRun(runs[0], 0, 4, 0, LoopMode::PlayOnce, "runs: playing");

  runs = checkedRuns({3, 3, 3}, 10, "runs: held");
  require(runs.size() == 1, "runs: held is one run");
  requireRun(runs[0], 0, 3, 3, LoopMode::SingleFrame, "runs: held");

  runs = checkedRuns({8, 9, 0, 1}, 10, "runs: wrapping");
  require(runs.size() == 1, "runs: wrapping is one run");
  requireRun(runs[0], 0, 4, 8, LoopMode::Loop, "runs: wrapping");

  runs = checkedRuns({0, 1, 0, 1, 0}, 2, "runs: looping");
  require(runs.size() == 1, "runs: looping is one run");
  requireRun(runs[0], 0, 5, 0, LoopMode::Loop, "runs: looping");

  runs = checkedRuns({5}, 10, "runs: lone frame");
  require(runs.size() == 1, "runs: lone frame is one run");
  requireRun(runs[0], 0, 1, 5, LoopMode::SingleFrame, "runs: lone frame");

  runs = checkedRuns({-1, 2, 3, -1, -1, 7, 7}, 10, "runs: blanks");
  require(runs.size() == 2, "runs: blanks split");
  requireRun(runs[0], 1, 2, 2, LoopMode::PlayOnce, "runs: blanks 0");
  requireRun(runs[1], 5, 2, 7, LoopMode::SingleFrame, "runs: blanks 1");

  runs = checkedRuns({0, 1, 5, 6}, 10, "runs: jump");
  require(runs.size() == 2, "runs: a jump splits");
  requireRun(runs[0], 0, 2, 0, LoopMode::PlayOnce, "runs: jump 0");
  requireRun(runs[1], 2, 2, 5, LoopMode::PlayOnce, "runs: jump 1");

  runs = checkedRuns({2, 2, 3, 4}, 10, "runs: held then playing");
  require(runs.size() == 2, "runs: held then playing splits");
  requireRun(runs[0], 0, 2, 2, LoopMode::SingleFrame, "runs: held first");
  requireRun(runs[1], 2, 2, 3, LoopMode::PlayOnce, "runs: playing next");

  runs = checkedRuns({0, 1, 1}, 10, "runs: playing then held");
  require(runs.size() == 2, "runs: playing then held splits");
  requireRun(runs[0], 0, 2, 0, LoopMode::PlayOnce, "runs: playing first");
  requireRun(runs[1], 2, 1, 1, LoopMode::SingleFrame, "runs: held next");

  runs = checkedRuns({3, 2, 1}, 10, "runs: backwards");
  require(runs.size() == 3, "runs: backwards is single frames");
  for (const InstanceRun &run : runs)
    require(run.m_loop == LoopMode::SingleFrame && run.m_duration == 1,
            "runs: backwards single frames");

  runs = checkedRuns({0, 1, 2, 3}, 10, "runs: segments", {0, 0, 1, 1});
  require(runs.size() == 2, "runs: a segment change splits");
  requireRun(runs[0], 0, 2, 0, LoopMode::PlayOnce, "runs: segment 0");
  requireRun(runs[1], 2, 2, 2, LoopMode::PlayOnce, "runs: segment 1");

  runs = checkedRuns({0, 10, 11, 9}, 10, "runs: outside the symbol");
  require(runs.size() == 2, "runs: frames outside the symbol are blank");
  requireRun(runs[0], 0, 1, 0, LoopMode::SingleFrame, "runs: outside 0");
  requireRun(runs[1], 3, 1, 9, LoopMode::SingleFrame, "runs: outside 1");

  runs = checkedRuns({0, 0, 0}, 1, "runs: one frame symbol");
  require(runs.size() == 1, "runs: one frame symbol is one run");
  requireRun(runs[0], 0, 3, 0, LoopMode::SingleFrame, "runs: one frame");

  require(instanceRuns({}, 10).empty(), "runs: no frame");
  require(instanceRuns({0, 1}, 0).empty(), "runs: empty symbol");

  // A long mix of playing, held, jumping, wrapping and blank frames, with
  // segments
  unsigned seed   = 12345u;
  auto nextRandom = [&seed](int n) {
    seed = seed * 1103515245u + 12345u;
    return (int)((seed >> 16) % (unsigned)n);
  };
  const int length = 7;
  std::vector<int> frames, segments;
  int current = 0, segment = 0;
  for (int i = 0; i < 400; ++i) {
    // 0-3: plays (and wraps), 4-5: holds, 6-7: jumps, 8: holds, 9: blank
    int r = nextRandom(10);
    if (r < 4)
      current = (current + 1) % length;
    else if (r == 6 || r == 7)
      current = nextRandom(length);
    if (nextRandom(12) == 0) ++segment;
    frames.push_back(r < 9 ? current : -1);
    segments.push_back(segment);
  }
  checkedRuns(frames, length, "runs: mix", segments);
  checkedRuns(frames, length, "runs: mix without segments");
}

}  // namespace

int main() {
  try {
    testSpritemap();
    testAnimationStructure();
    testMatrix();
    testTimelineRepair();
    testNoFrames();
    testNumbersAreFinite();
    testValidate();
    testSymbols();
    testSymbolValidate();
    testSymbolNumbers();
    testInstanceRuns();
  } catch (const std::exception &e) {
    std::cerr << "atlasjson_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "atlasjson_test passed" << std::endl;
  return 0;
}
