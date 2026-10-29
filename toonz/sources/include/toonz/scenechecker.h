#pragma once

#ifndef SCENECHECKER_H
#define SCENECHECKER_H

#include "tfilepath.h"

#include <QString>

#include <functional>
#include <set>
#include <string>
#include <vector>

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

class ToonzScene;
class TXsheet;
class TXshSimpleLevel;

//=============================================================================
// The scene checker (doc/scene_checker.md): a list of what is wrong, or worth
// a look, in a scene, before it is rendered or handed on.
//
// check() only reads the scene; it changes nothing in it. The top xsheet and
// every sub-xsheet used in it are walked once each. An issue inside a
// sub-xsheet has its column and row in that sub-xsheet (the issue's xsheet),
// and the sub-xsheet's level name in subXsheet.
//
// Tested by toonzlib/tests/scenechecker_test.cpp.
//=============================================================================

namespace SceneChecker {

enum Severity { Info = 0, Warning, Error };

enum Kind {
  MissingDrawing = 0,  //!< a cell shows a drawing its level does not have
  MissingFile,         //!< a level's file is not on disk
  UnsavedLevel,        //!< a level has never been saved
  EmptyColumn,         //!< a column without cells
  UnusedLevel,         //!< a level of the cast used in no xsheet
  UnusedStyle,         //!< a palette style no drawing uses (an option)
  DuplicateStyle,      //!< a style that looks the same as another one
  LevelDpi,            //!< a raster level's DPI differs from the camera's
  FrameRate,           //!< a sound or movie runs at another frame rate
  Resolution,          //!< output or preview size against the camera
  KindCount
};

struct Issue {
  Severity severity = Info;
  Kind kind         = MissingDrawing;
  int column        = -1;  //!< in the issue's xsheet, -1 for none
  int row           = -1;  //!< the first row, -1 for none
  int rowCount      = 0;   //!< rows from row with the same problem
  std::wstring levelName;  //!< empty for none
  int styleId = -1;        //!< -1 for none
  std::wstring subXsheet;  //!< the sub-xsheet's level name, empty for the top
  TXsheet *xsheet = nullptr;  //!< the xsheet of column and row, if any
  QString text;               //!< a sentence for the user
};

struct Options {
  //! Look for palette styles that no drawing uses. It loads every drawing of
  //! the levels in the xsheets, so it is slow on large scenes.
  bool unusedStyles = false;

  //! The frame rate of a movie file, 0 when unknown. The default reads it
  //! from the file; tests replace it.
  std::function<double(const TFilePath &)> movieFrameRate;
};

//! Everything found, sorted by severity (errors first), then kind.
DVAPI std::vector<Issue> check(ToonzScene *scene,
                               const Options &options = Options());

//! Stable names, used by the scripts: "error", "warning", "info" and
//! "missingDrawing", "missingFile", ...
DVAPI QString severityName(Severity severity);
DVAPI QString kindName(Kind kind);
//! A title for the user, such as "Missing Drawings"
DVAPI QString kindTitle(Kind kind);

//! The ids of the styles the level's drawings use (ink and paint of a Toonz
//! raster drawing, strokes and regions of a vector drawing). It loads every
//! drawing of the level.
DVAPI std::set<int> usedStyleIds(TXshSimpleLevel *level);

//! What the Scene Check panel shows over the issues
struct Statistics {
  int drawings         = 0;   //!< drawings of the levels used in the xsheets
  int levels           = 0;   //!< levels of the cast
  int columns          = 0;   //!< non-empty columns of the top xsheet
  int frames           = 0;   //!< the scene's frame count
  int styles           = 0;   //!< styles in the pages of the levels' palettes
  qint64 sceneFileSize = -1;  //!< bytes, -1 when the scene is not saved
};

DVAPI Statistics statistics(ToonzScene *scene);

}  // namespace SceneChecker

#endif
