#pragma once

#ifndef ATLASJSON_H
#define ATLASJSON_H

// JSON files of the Texture Atlas export (see doc/animate_texture_atlas.md),
// in the style of the Texture Atlas of Adobe Animate: "spritemap1.json" (the
// sprites of an atlas page) and "Animation.json" (the layers and frames which
// place the sprites). The format is reverse engineered, and what is written is
// documented in the doc file.
//
// Header only and free of OpenToonz: plain structures in, QJsonObject or bytes
// out, so that it can be tested standalone (toonzlib/tests/atlasjson_test.cpp).

#include <QByteArray>
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

//! A keyframe of a layer: from frame m_start (0 based) for m_duration frames
//! the sprite m_sprite is shown with the transform m_matrix, which places the
//! sprite pixels (origin at the top left corner of the sprite, y down) on the
//! stage (origin at the top left corner, y down). A frame without sprite name
//! is blank.
struct Frame {
  int m_start    = 0;
  int m_duration = 1;
  QString m_sprite;
  Matrix3D m_matrix = identityMatrix();
};

//! The first layer is the top one
struct Layer {
  QString m_name;
  std::vector<Frame> m_frames;
};

struct Animation {
  QString m_name;        //!< "AN"."N": name of the animation (the scene)
  QString m_symbolName;  //!< "AN"."SN": name of its symbol
  double m_frameRate = 24.0;
  int m_stageWidth = 0, m_stageHeight = 0;  //!< Pixels, 0 if unknown
  int m_frameCount = 0;  //!< Length of the timeline; at least up to the end of
                         //!< the last frame of the layers
  std::vector<Layer> m_layers;
};

//-----------------------------------------------------------------------------

namespace detail {

inline double finiteOr(double value, double fallback) {
  return std::isfinite(value) ? value : fallback;
}

//-----------------------------------------------------------------------------

//! A frame of the written timeline: m_sprite is empty for a blank frame
struct TimelineFrame {
  int m_start, m_duration;
  QString m_sprite;
  Matrix3D m_matrix;
};

/*!
Returns the frames of \e layer made into a timeline of \e frameCount frames
which starts at frame 0 and has neither gaps nor overlaps: the frames are
sorted by start, a frame which starts inside the previous one cuts it (the
last one wins; one which would be left with no frame is dropped), frames with
no duration are dropped, a part before frame 0 is cut, and the gaps are filled
with blank frames.
*/
inline std::vector<TimelineFrame> timeline(const Layer &layer, int frameCount) {
  std::vector<TimelineFrame> sorted;
  for (const Frame &f : layer.m_frames) {
    int start = f.m_start, duration = f.m_duration;
    if (start < 0) duration += start, start = 0;
    if (duration <= 0) continue;
    sorted.push_back({start, duration, f.m_sprite, f.m_matrix});
  }
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const TimelineFrame &a, const TimelineFrame &b) {
                     return a.m_start < b.m_start;
                   });

  std::vector<TimelineFrame> cut;
  for (const TimelineFrame &f : sorted) {
    if (!cut.empty() &&
        f.m_start < cut.back().m_start + cut.back().m_duration) {
      cut.back().m_duration = f.m_start - cut.back().m_start;
      if (cut.back().m_duration <= 0) cut.pop_back();
    }
    cut.push_back(f);
  }

  std::vector<TimelineFrame> result;
  int cursor = 0;
  for (const TimelineFrame &f : cut) {
    if (f.m_start > cursor)
      result.push_back(
          {cursor, f.m_start - cursor, QString(), identityMatrix()});
    result.push_back(f);
    cursor = f.m_start + f.m_duration;
  }
  if (cursor < frameCount)
    result.push_back(
        {cursor, frameCount - cursor, QString(), identityMatrix()});
  return result;
}

//! Length of the timeline of \e animation: the end of the last frame of any
//! layer, or m_frameCount if more.
inline int totalFrames(const Animation &animation) {
  int total = std::max(0, animation.m_frameCount);
  for (const Layer &layer : animation.m_layers)
    for (const Frame &f : layer.m_frames)
      if (f.m_duration > 0) total = std::max(total, f.m_start + f.m_duration);
  return total;
}

}  // namespace detail

//-----------------------------------------------------------------------------
// Checks
//-----------------------------------------------------------------------------

/*!
Checks the input and returns a description of the first problem found, or an
empty string if it can be written: sprite names are not empty and unique over
all the pages, sprites have a size and are inside their page, frames refer to
existing sprites, layer names are unique, the frame rate is positive.
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

  QSet<QString> layerNames;
  for (const Layer &layer : animation.m_layers) {
    if (layer.m_name.isEmpty()) return "A layer has no name.";
    if (layerNames.contains(layer.m_name))
      return "The layer name " + layer.m_name + " is used twice.";
    layerNames.insert(layer.m_name);
    for (const Frame &f : layer.m_frames)
      if (!f.m_sprite.isEmpty() && !spriteNames.contains(f.m_sprite))
        return "The layer " + layer.m_name + " uses the unknown sprite " +
               f.m_sprite + ".";
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
                             {"I": start, "DU": duration, "E": []}, ...]},
                     ...]}},
 "MD": {"FRT": frame rate, "W": stage width, "H": stage height},
 "SD": {"S": []}}
\endcode
Every layer covers the timeline from frame 0 to its end, without gaps or
overlaps: the frames which show nothing have an empty "E". A non finite value
is written as 0, and a matrix with a non finite value as the identity.
*/
inline QJsonObject animationJson(const Animation &animation) {
  const int total = detail::totalFrames(animation);

  QJsonArray layers;
  for (const Layer &layer : animation.m_layers) {
    QJsonArray frames;
    for (const detail::TimelineFrame &f : detail::timeline(layer, total)) {
      QJsonArray elements;
      if (!f.m_sprite.isEmpty()) {
        bool finite = true;
        for (double v : f.m_matrix)
          if (!std::isfinite(v)) finite = false;
        const Matrix3D matrix = finite ? f.m_matrix : identityMatrix();

        QJsonArray m3d;
        for (double v : matrix) m3d.append(QJsonValue(v));

        QJsonObject instance;
        instance["N"]   = f.m_sprite;
        instance["M3D"] = m3d;
        QJsonObject element;
        element["ASI"] = instance;
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
    layers.append(json);
  }

  QJsonObject timelineJson;
  timelineJson["L"] = layers;

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

  // No symbol is exported: the dictionary is empty
  QJsonObject sd;
  sd["S"] = QJsonArray();

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
