#pragma once

#ifndef ATLASJSON_H
#define ATLASJSON_H

// JSON files of the Texture Atlas export (see doc/animate_texture_atlas.md),
// in the style of the Texture Atlas of Adobe Animate: "spritemap1.json" (the
// sprites of an atlas page) and "Animation.json" (the layers and frames which
// place the sprites, and the symbols that frames can show). The format is
// reverse engineered, and what is written is documented in the doc file.
//
// Header only and free of OpenToonz: plain structures in, QJsonObject or bytes
// out, so that it can be tested standalone (toonzlib/tests/atlasjson_test.cpp).

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace AtlasJson {

//! A 4x4 matrix, 16 numbers in column-major order, as Animate writes M3D:
//! [a b 0 0  c d 0 0  0 0 1 0  tx ty 0 1].
typedef std::array<double, 16> Matrix3D;

inline Matrix3D identityMatrix() {
  Matrix3D m = {{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0,
                 0.0, 0.0, 0.0, 1.0}};
  return m;
}

//! The matrix of the 2D transform x' = a * x + c * y + tx,
//! y' = b * x + d * y + ty. Its values are at the indices 0 (a), 1 (b),
//! 4 (c), 5 (d), 12 (tx) and 13 (ty).
inline Matrix3D matrixFromAffine(double a, double b, double c, double d,
                                 double tx, double ty) {
  Matrix3D m = {
      {a, b, 0.0, 0.0, c, d, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, tx, ty, 0.0, 1.0}};
  return m;
}

//! How a symbol instance plays the frames of its symbol (the "LP" key). The
//! n-th frame (from 0) of a keyframe with the first frame FF shows the frame
//! (FF + n) modulo the symbol length for Loop ("LP"), FF + n up to the last
//! frame of the symbol, which is then held, for PlayOnce ("PO"), and FF for
//! SingleFrame ("SF").
enum class LoopMode { Loop, PlayOnce, SingleFrame };

//! The "LP" value of a loop mode
inline QString loopModeKey(LoopMode mode) {
  switch (mode) {
  case LoopMode::PlayOnce:
    return QString::fromLatin1("PO");
  case LoopMode::SingleFrame:
    return QString::fromLatin1("SF");
  default:
    return QString::fromLatin1("LP");
  }
}

//-----------------------------------------------------------------------------
// Input
//-----------------------------------------------------------------------------

//! A sprite of an atlas page: the rectangle it takes in the page image.
//! When m_rotated, the sprite is stored turned 90 degrees clockwise and m_w,
//! m_h are the size of the stored (turned) rectangle.
struct Sprite {
  QString m_name;
  int m_x = 0, m_y = 0, m_w = 0, m_h = 0;
  bool m_rotated = false;
};

struct Page {
  QString m_image;  //!< File name of the page image
  int m_width = 0, m_height = 0;
  double m_resolution = 1.0;  //!< Scale of the sprite pixels, 1 for 100%
  std::vector<Sprite> m_sprites;
};

/*!
A keyframe of a layer: from frame m_start (0 based) for m_duration frames it
shows either the sprite m_sprite (an "ASI" element) or an instance of the
symbol m_symbol (an "SI" element), with the transform m_matrix. For a sprite
the matrix places the sprite pixels (origin at the top left corner of the
sprite, y down) in the coordinates of the timeline which holds the layer: the
stage (origin at the top left corner, y down) for the main timeline, the
symbol's own coordinates for the timeline of a symbol. For a symbol instance
it places the coordinates of the symbol in the same way. A frame with neither
sprite nor symbol name is blank; one with both is refused by validate().
*/
struct Frame {
  int m_start    = 0;
  int m_duration = 1;
  QString m_sprite;  //!< The sprite shown, or empty
  QString m_symbol;  //!< The symbol shown, or empty

  // Symbol instances only
  int m_firstFrame = 0;  //!< "FF": frame of the symbol shown at m_start
  LoopMode m_loop  = LoopMode::Loop;  //!< "LP"
  QString m_instanceName;             //!< "IN", usually empty

  Matrix3D m_matrix = identityMatrix();
};

//! The first layer is the top one
struct Layer {
  QString m_name;
  std::vector<Frame> m_frames;
};

//! A symbol of the symbol dictionary ("SD"."S"): a timeline of its own, shown
//! by the symbol instances of the frames of other timelines.
struct Symbol {
  QString m_name;        //!< "SN": unique, and not the main symbol's name
  int m_frameCount = 0;  //!< Length of its timeline; at least up to the end of
                         //!< the last frame of its layers
  std::vector<Layer> m_layers;
};

struct Animation {
  QString m_name;        //!< "AN"."N": name of the animation (the scene)
  QString m_symbolName;  //!< "AN"."SN": name of its symbol
  double m_frameRate = 24.0;
  int m_stageWidth = 0, m_stageHeight = 0;  //!< Pixels, 0 if unknown
  int m_frameCount = 0;  //!< Length of the timeline; at least up to the end of
                         //!< the last frame of the layers
  std::vector<Layer> m_layers;
  std::vector<Symbol> m_symbols;  //!< In the order they are written
};

//-----------------------------------------------------------------------------
// Symbol instances from frames of a sub-timeline
//-----------------------------------------------------------------------------

//! A keyframe of a symbol instance, made by instanceRuns()
struct InstanceRun {
  int m_start = 0, m_duration = 1;  //!< Frames of the timeline
  int m_firstFrame = 0;             //!< "FF"
  LoopMode m_loop  = LoopMode::SingleFrame;
};

/*!
The keyframes of symbol instances which show, at each frame f of a timeline,
the frame \e childFrames[f] of a symbol of \e symbolLength frames, or nothing
where it is negative or not less than symbolLength. A run of frames where the
symbol frame advances by one per frame is one keyframe from the first symbol
frame of the run, PlayOnce, or Loop when it goes from the last frame of the
symbol back to the first one. A run which holds a symbol frame, and a lone
frame, is one SingleFrame keyframe. Two frames whose \e segments differ are
never in the same keyframe (the transform changed, for example); \e segments
may be empty, else it has a value per frame. The runs are made from the first
frame on: each one is as long as it can be.
*/
inline std::vector<InstanceRun> instanceRuns(
    const std::vector<int> &childFrames, int symbolLength,
    const std::vector<int> &segments = std::vector<int>()) {
  std::vector<InstanceRun> runs;
  const bool useSegments = segments.size() == childFrames.size();
  // open: the last run ends at the previous frame; mode of the open run: 0
  // one frame so far, 1 held, 2 playing; last: the symbol frame at the
  // previous frame
  bool open = false;
  int mode = 0, last = 0;
  for (int f = 0; f < (int)childFrames.size(); ++f) {
    const int frame = childFrames[f];
    if (frame < 0 || frame >= symbolLength) {
      open = false;
      continue;
    }
    if (open && (!useSegments || segments[f] == segments[f - 1])) {
      const int next   = (last + 1 < symbolLength) ? last + 1 : 0;
      const bool held  = frame == last;
      const bool plays = !held && frame == next;
      InstanceRun &run = runs.back();
      if ((mode == 0 && (held || plays)) || (mode == 1 && held) ||
          (mode == 2 && plays)) {
        if (mode == 0) {
          mode       = held ? 1 : 2;
          run.m_loop = held ? LoopMode::SingleFrame : LoopMode::PlayOnce;
        }
        if (mode == 2 && frame < last) run.m_loop = LoopMode::Loop;  // wraps
        ++run.m_duration;
        last = frame;
        continue;
      }
    }
    InstanceRun run;
    run.m_start      = f;
    run.m_duration   = 1;
    run.m_firstFrame = frame;
    run.m_loop       = LoopMode::SingleFrame;
    runs.push_back(run);
    open = true;
    mode = 0;
    last = frame;
  }
  return runs;
}

//-----------------------------------------------------------------------------

namespace detail {

inline double finiteOr(double value, double fallback) {
  return std::isfinite(value) ? value : fallback;
}

//-----------------------------------------------------------------------------

//! Length of a timeline: the end of the last frame of any layer, or
//! frameCount if more.
inline int totalFrames(int frameCount, const std::vector<Layer> &layers) {
  int total = std::max(0, frameCount);
  for (const Layer &layer : layers)
    for (const Frame &f : layer.m_frames)
      if (f.m_duration > 0) total = std::max(total, f.m_start + f.m_duration);
  return total;
}

inline int totalFrames(const Animation &animation) {
  return totalFrames(animation.m_frameCount, animation.m_layers);
}

inline int totalFrames(const Symbol &symbol) {
  return totalFrames(symbol.m_frameCount, symbol.m_layers);
}

//-----------------------------------------------------------------------------

//! The length of each symbol, by name
inline QHash<QString, int> symbolLengths(const Animation &animation) {
  QHash<QString, int> lengths;
  for (const Symbol &symbol : animation.m_symbols)
    if (!lengths.contains(symbol.m_name))
      lengths.insert(symbol.m_name, totalFrames(symbol));
  return lengths;
}

//-----------------------------------------------------------------------------

/*!
Returns the frames of \e layer made into a timeline of \e frameCount frames
which starts at frame 0 and has neither gaps nor overlaps: the frames are
sorted by start, a frame which starts inside the previous one cuts it (the
last one wins; one which would be left with no frame is dropped), frames with
no duration are dropped, a part before frame 0 is cut, and the gaps are filled
with blank frames. When the part cut before frame 0 is of a symbol instance
which plays, its first frame moves on by the frames cut (modulo the symbol
length in \e symbolLengths for a loop, up to the last frame of the symbol for
play once).
*/
inline std::vector<Frame> timeline(const Layer &layer, int frameCount,
                                   const QHash<QString, int> &symbolLengths) {
  std::vector<Frame> sorted;
  for (const Frame &frame : layer.m_frames) {
    Frame f = frame;
    if (f.m_start < 0) {
      const int cutFrames = -f.m_start;
      f.m_duration -= cutFrames;
      f.m_start = 0;
      if (f.m_duration > 0 && !f.m_symbol.isEmpty() &&
          f.m_loop != LoopMode::SingleFrame) {
        const int length = symbolLengths.value(f.m_symbol, 0);
        long long first  = (long long)f.m_firstFrame + cutFrames;
        if (length > 0 && f.m_loop == LoopMode::Loop)
          first %= length;
        else if (length > 0)
          first = std::min<long long>(first, length - 1);
        f.m_firstFrame = (int)std::min<long long>(first, 0x7fffffff);
      }
    }
    if (f.m_duration <= 0) continue;
    sorted.push_back(f);
  }
  std::stable_sort(
      sorted.begin(), sorted.end(),
      [](const Frame &a, const Frame &b) { return a.m_start < b.m_start; });

  std::vector<Frame> cut;
  for (const Frame &f : sorted) {
    if (!cut.empty() &&
        f.m_start < cut.back().m_start + cut.back().m_duration) {
      cut.back().m_duration = f.m_start - cut.back().m_start;
      if (cut.back().m_duration <= 0) cut.pop_back();
    }
    cut.push_back(f);
  }

  std::vector<Frame> result;
  int cursor = 0;
  for (const Frame &f : cut) {
    if (f.m_start > cursor) {
      Frame blank;
      blank.m_start    = cursor;
      blank.m_duration = f.m_start - cursor;
      result.push_back(blank);
    }
    result.push_back(f);
    cursor = f.m_start + f.m_duration;
  }
  if (cursor < frameCount) {
    Frame blank;
    blank.m_start    = cursor;
    blank.m_duration = frameCount - cursor;
    result.push_back(blank);
  }
  return result;
}

//-----------------------------------------------------------------------------

//! The "M3D" array of a matrix: the identity if a value is not finite
inline QJsonArray matrixJson(const Matrix3D &matrix) {
  bool finite = true;
  for (double v : matrix)
    if (!std::isfinite(v)) finite = false;
  QJsonArray m3d;
  for (double v : finite ? matrix : identityMatrix()) m3d.append(QJsonValue(v));
  return m3d;
}

//-----------------------------------------------------------------------------

//! The "L" array of a timeline of \e total frames
inline QJsonArray layersJson(const std::vector<Layer> &layers, int total,
                             const QHash<QString, int> &symbolLengths) {
  QJsonArray result;
  for (const Layer &layer : layers) {
    QJsonArray frames;
    for (const Frame &f : timeline(layer, total, symbolLengths)) {
      QJsonArray elements;
      if (!f.m_sprite.isEmpty()) {
        QJsonObject instance;
        instance["N"]   = f.m_sprite;
        instance["M3D"] = matrixJson(f.m_matrix);
        QJsonObject element;
        element["ASI"] = instance;
        elements.append(element);
      } else if (!f.m_symbol.isEmpty()) {
        QJsonObject point;
        point["x"] = 0.0;
        point["y"] = 0.0;
        QJsonObject instance;
        instance["SN"]  = f.m_symbol;
        instance["IN"]  = f.m_instanceName;
        instance["ST"]  = QString::fromLatin1("G");
        instance["FF"]  = std::max(0, f.m_firstFrame);
        instance["LP"]  = loopModeKey(f.m_loop);
        instance["TRP"] = point;
        instance["M3D"] = matrixJson(f.m_matrix);
        QJsonObject element;
        element["SI"] = instance;
        elements.append(element);
      }
      QJsonObject frame;
      frame["I"]  = f.m_start;
      frame["DU"] = f.m_duration;
      frame["E"]  = elements;
      frames.append(frame);
    }
    QJsonObject json;
    json["LN"] = layer.m_name;
    json["FR"] = frames;
    result.append(json);
  }
  return result;
}

//-----------------------------------------------------------------------------

/*!
Checks the layers of a timeline (\e owner is the name of its symbol, empty
for the main timeline): layer names are not empty and unique, frames refer to
existing sprites or symbols, not both, and show a frame of the symbol's
timeline. Adds to \e uses the indices of the symbols shown.
*/
inline QString checkLayers(const std::vector<Layer> &layers,
                           const QString &owner,
                           const QSet<QString> &spriteNames,
                           const QHash<QString, int> &symbolIndices,
                           const std::vector<Symbol> &symbols,
                           std::vector<int> &uses) {
  QString where;
  if (!owner.isEmpty()) where = QString(" of the symbol ") + owner;
  QSet<QString> layerNames;
  for (const Layer &layer : layers) {
    if (layer.m_name.isEmpty()) return "A layer" + where + " has no name.";
    if (layerNames.contains(layer.m_name))
      return "The layer name " + layer.m_name + " is used twice" + where + ".";
    layerNames.insert(layer.m_name);
    for (const Frame &f : layer.m_frames) {
      if (!f.m_sprite.isEmpty() && !f.m_symbol.isEmpty())
        return "A frame of the layer " + layer.m_name + where +
               " shows both a sprite and a symbol.";
      if (!f.m_sprite.isEmpty() && !spriteNames.contains(f.m_sprite))
        return "The layer " + layer.m_name + where +
               " uses the unknown sprite " + f.m_sprite + ".";
      if (f.m_symbol.isEmpty()) continue;
      if (!symbolIndices.contains(f.m_symbol))
        return "The layer " + layer.m_name + where +
               " uses the undefined symbol " + f.m_symbol + ".";
      const int index  = symbolIndices.value(f.m_symbol);
      const int length = totalFrames(symbols[index]);
      if (f.m_firstFrame < 0 || f.m_firstFrame >= length)
        return "The layer " + layer.m_name + where + " shows the frame " +
               QString::number(f.m_firstFrame) + " of the symbol " +
               f.m_symbol + ", which has " + QString::number(length) +
               " frame(s).";
      if (std::find(uses.begin(), uses.end(), index) == uses.end())
        uses.push_back(index);
    }
  }
  return QString();
}

//-----------------------------------------------------------------------------

//! Depth first search from symbol \e i: returns a symbol which contains
//! itself, or -1. \e state: 0 not seen, 1 on the path, 2 done.
inline int findCycle(int i, const std::vector<std::vector<int>> &uses,
                     std::vector<int> &state) {
  state[i] = 1;
  for (int j : uses[i]) {
    if (state[j] == 1) return j;
    if (state[j] == 0) {
      int k = findCycle(j, uses, state);
      if (k >= 0) return k;
    }
  }
  state[i] = 2;
  return -1;
}

}  // namespace detail

//-----------------------------------------------------------------------------
// Checks
//-----------------------------------------------------------------------------

/*!
Checks the input and returns a description of the first problem found, or an
empty string if it can be written: sprite names are not empty and unique over
all the pages, sprites have a size and are inside their page, the frame rate
is positive, symbol names are not empty, unique and not the name of the main
symbol, layer names are unique in each timeline, frames refer to existing
sprites or to defined symbols (not both) and show a frame of the symbol's
timeline, and no symbol contains itself, directly or through other symbols.
*/
inline QString validate(const std::vector<Page> &pages,
                        const Animation &animation) {
  QSet<QString> spriteNames;
  for (const Page &page : pages) {
    if (page.m_image.isEmpty()) return "A page has no image name.";
    if (page.m_width <= 0 || page.m_height <= 0)
      return "The page " + page.m_image + " has no size.";
    for (const Sprite &s : page.m_sprites) {
      if (s.m_name.isEmpty()) return "A sprite has no name.";
      if (spriteNames.contains(s.m_name))
        return "The sprite name " + s.m_name + " is used twice.";
      spriteNames.insert(s.m_name);
      if (s.m_w <= 0 || s.m_h <= 0)
        return "The sprite " + s.m_name + " has no size.";
      if (s.m_x < 0 || s.m_y < 0 || s.m_x + s.m_w > page.m_width ||
          s.m_y + s.m_h > page.m_height)
        return "The sprite " + s.m_name + " is outside its page.";
    }
  }

  if (!std::isfinite(animation.m_frameRate) || animation.m_frameRate <= 0.0)
    return "The frame rate must be positive.";

  const std::vector<Symbol> &symbols = animation.m_symbols;
  QHash<QString, int> symbolIndices;
  for (int i = 0; i < (int)symbols.size(); ++i) {
    const QString &name = symbols[i].m_name;
    if (name.isEmpty()) return "A symbol has no name.";
    if (symbolIndices.contains(name))
      return "The symbol name " + name + " is used twice.";
    if (name == animation.m_symbolName)
      return "The symbol name " + name +
             " is the name of the main timeline's symbol.";
    symbolIndices.insert(name, i);
  }

  std::vector<int> mainUses;
  QString problem =
      detail::checkLayers(animation.m_layers, QString(), spriteNames,
                          symbolIndices, symbols, mainUses);
  if (!problem.isEmpty()) return problem;

  std::vector<std::vector<int>> uses(symbols.size());
  for (int i = 0; i < (int)symbols.size(); ++i) {
    problem = detail::checkLayers(symbols[i].m_layers, symbols[i].m_name,
                                  spriteNames, symbolIndices, symbols, uses[i]);
    if (!problem.isEmpty()) return problem;
  }

  std::vector<int> state(symbols.size(), 0);
  for (int i = 0; i < (int)symbols.size(); ++i) {
    if (state[i] != 0) continue;
    int cycle = detail::findCycle(i, uses, state);
    if (cycle >= 0)
      return "The symbol " + symbols[cycle].m_name + " contains itself.";
  }
  return QString();
}

//-----------------------------------------------------------------------------
// Output
//-----------------------------------------------------------------------------

/*!
The content of a "spritemap<N>.json" file:
\code
{"ATLAS": {"SPRITES": [{"SPRITE": {"name": ..., "x": ..., "y": ..., "w": ...,
                                   "h": ..., "rotated": false}}, ...]},
 "meta": {"app": ..., "version": ..., "image": "spritemap1.png",
          "format": "RGBA8888", "size": {"w": ..., "h": ...},
          "resolution": "1"}}
\endcode
*/
inline QJsonObject spritemapJson(const Page &page, const QString &app,
                                 const QString &version) {
  QJsonArray sprites;
  for (const Sprite &s : page.m_sprites) {
    QJsonObject sprite;
    sprite["name"]    = s.m_name;
    sprite["x"]       = s.m_x;
    sprite["y"]       = s.m_y;
    sprite["w"]       = s.m_w;
    sprite["h"]       = s.m_h;
    sprite["rotated"] = s.m_rotated;
    QJsonObject item;
    item["SPRITE"] = sprite;
    sprites.append(item);
  }
  QJsonObject atlas;
  atlas["SPRITES"] = sprites;

  QJsonObject size;
  size["w"] = page.m_width;
  size["h"] = page.m_height;

  QJsonObject meta;
  meta["app"]       = app;
  meta["version"]   = version;
  meta["image"]     = page.m_image;
  meta["format"]    = QString("RGBA8888");
  meta["size"]      = size;
  double resolution = detail::finiteOr(page.m_resolution, 1.0);
  if (resolution <= 0.0) resolution = 1.0;
  meta["resolution"] = QString::number(resolution, 'g', 6);

  QJsonObject root;
  root["ATLAS"] = atlas;
  root["meta"]  = meta;
  return root;
}

//-----------------------------------------------------------------------------

/*!
The content of "Animation.json":
\code
{"AN": {"N": name, "SN": symbol name,
        "TL": {"L": [{"LN": layer name,
                      "FR": [{"I": start, "DU": duration,
                              "E": [{"ASI": {"N": sprite name,
                                             "M3D": [16 numbers]}}]},
                             {"I": start, "DU": duration,
                              "E": [{"SI": {"SN": symbol name, "IN": "",
                                            "ST": "G", "FF": first frame,
                                            "LP": "LP" | "PO" | "SF",
                                            "TRP": {"x": 0, "y": 0},
                                            "M3D": [16 numbers]}}]},
                             {"I": start, "DU": duration, "E": []}, ...]},
                     ...]}},
 "MD": {"FRT": frame rate, "W": stage width, "H": stage height},
 "SD": {"S": [{"SN": symbol name, "TL": {"L": [layers as above]}}, ...]}}
\endcode
Every layer covers its timeline from frame 0 to its end, without gaps or
overlaps: the frames which show nothing have an empty "E". The layers of a
symbol cover the symbol's timeline. A non finite value is written as 0, and a
matrix with a non finite value as the identity.
*/
inline QJsonObject animationJson(const Animation &animation) {
  const QHash<QString, int> lengths = detail::symbolLengths(animation);

  QJsonObject timelineJson;
  timelineJson["L"] = detail::layersJson(
      animation.m_layers, detail::totalFrames(animation), lengths);

  QJsonObject an;
  an["N"]  = animation.m_name;
  an["SN"] = animation.m_symbolName;
  an["TL"] = timelineJson;

  QJsonObject md;
  md["FRT"] = detail::finiteOr(animation.m_frameRate, 24.0);
  if (animation.m_stageWidth > 0 && animation.m_stageHeight > 0) {
    md["W"] = animation.m_stageWidth;
    md["H"] = animation.m_stageHeight;
  }

  QJsonArray symbols;
  for (const Symbol &symbol : animation.m_symbols) {
    QJsonObject symbolTimeline;
    symbolTimeline["L"] = detail::layersJson(
        symbol.m_layers, detail::totalFrames(symbol), lengths);
    QJsonObject json;
    json["SN"] = symbol.m_name;
    json["TL"] = symbolTimeline;
    symbols.append(json);
  }
  QJsonObject sd;
  sd["S"] = symbols;

  QJsonObject root;
  root["AN"] = an;
  root["MD"] = md;
  root["SD"] = sd;
  return root;
}

//-----------------------------------------------------------------------------

//! The bytes of a JSON file, compact as Animate writes them
inline QByteArray toBytes(const QJsonObject &json) {
  return QJsonDocument(json).toJson(QJsonDocument::Compact);
}

}  // namespace AtlasJson

#endif  // ATLASJSON_H
