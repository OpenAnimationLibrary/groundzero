// Regression test of the JSON builder of the Texture Atlas export
// (toonz/atlasjson.h): the files parse back, have the documented keys, list the
// sprites of the input, cover the timeline of every layer without gaps or
// overlaps, and keep the values of the matrices where the doc says.

#include "toonz/atlasjson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QString>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

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
  } catch (const std::exception &e) {
    std::cerr << "atlasjson_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "atlasjson_test passed" << std::endl;
  return 0;
}
