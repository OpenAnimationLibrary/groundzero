#pragma once

#ifndef BRUSHSTROKE_HARNESS_H
#define BRUSHSTROKE_HARNESS_H

// Drives the editor's own brush and eraser tools without the application:
// a TApplication made of real handles, a viewer that only reports a pixel
// size and a view matrix, and a clock that follows the recorded gesture.

#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "toonz/tapplication.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/palettecontroller.h"
#include "toonz/tpalettehandle.h"
#include "toonzqt/tselectionhandle.h"
#include "tmetaimage.h"
#include "tpalette.h"
#include "tproperty.h"
#include "traster.h"
#include "tvectorimage.h"

#include <QImage>
#include <QString>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace brushstroke {

inline void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// One recorded pointer sample. The tools take no tilt: TMouseEvent has no
// tilt field and every brush passes a zero tilt to its input manager.
struct Sample {
  double tMs, x, y, pressure;
};
typedef std::vector<Sample> Gesture;

// How a gesture is delivered to the tool.
struct Play {
  bool tablet    = true;   // false: a mouse, the tools ignore its pressure
  bool shift     = false;  // held from press to release
  bool alt       = false;
  bool ctrl      = false;
  double scale   = 1.0;  // applied to the recorded positions, then offset
  TPointD offset = TPointD();
};

class Viewer final : public TToolViewer {
public:
  Viewer() : TToolViewer(nullptr) {}

  double pixelSize = 1.0;
  TAffine viewMatrix;

  double getPixelSize() const override { return pixelSize; }
  TAffine getViewMatrix() const override { return viewMatrix; }
  void invalidateAll() override {}
  void GLInvalidateAll() override {}
  void GLInvalidateRect(const TRectD&) override {}
  void invalidateToolStatus() override {}
  int posToColumnIndex(const TPointD&, double, bool) const override {
    return -1;
  }
  void posToColumnIndexes(const TPointD&, std::vector<int>&, double,
                          bool) const override {}
  int posToRow(const TPointD&, double, bool, bool) const override { return -1; }
  TPointD worldToPos(const TPointD& worldPos) const override {
    return worldPos;
  }
  int pick(const TPointD&) override { return 0; }
  TPointD winToWorld(const TPointD& winPos) const override { return winPos; }
  void pan(const TPointD&) override {}
  void zoom(const TPointD&, double) override {}
  void rotate(const TPointD&, double) override {}
  void rotate3D(double, double) override {}
  bool is3DView() const override { return false; }
  bool getIsFlippedX() const override { return false; }
  bool getIsFlippedY() const override { return false; }
  double projectToZ(const TPointD&) override { return 0; }
  TPointD getDpiScale() const override { return TPointD(1, 1); }
  int getVGuideCount() override { return int(vGuides.size()); }
  int getHGuideCount() override { return int(hGuides.size()); }
  double getHGuide(int index) override { return hGuides[index]; }
  double getVGuide(int index) override { return vGuides[index]; }
  void resetInputMethod() override {}
  void setFocus() override {}
  TRectD getGeometry() const override { return TRectD(-400, -300, 400, 300); }

  std::vector<double> hGuides, vGuides;
};

class Application final : public TApplication {
public:
  Application();
  ~Application() override;

  TFrameHandle* getCurrentFrame() const override { return &frame; }
  TXshLevelHandle* getCurrentLevel() const override { return &level; }
  TXsheetHandle* getCurrentXsheet() const override { return &sheet; }
  TObjectHandle* getCurrentObject() const override { return &object; }
  TColumnHandle* getCurrentColumn() const override { return &column; }
  TSceneHandle* getCurrentScene() const override { return &scene; }
  ToolHandle* getCurrentTool() const override { return &tool; }
  TSelectionHandle* getCurrentSelection() const override { return &selection; }
  TOnionSkinMaskHandle* getCurrentOnionSkin() const override { return nullptr; }
  TPaletteHandle* getCurrentPalette() const override {
    return palette.getCurrentPalette();
  }
  TFxHandle* getCurrentFx() const override { return nullptr; }
  PaletteController* getPaletteController() const override { return &palette; }
  TColorStyle* getCurrentLevelStyle() const override {
    return palette.getCurrentLevelPalette()->getStyle();
  }
  int getCurrentLevelStyleIndex() const override {
    return palette.getCurrentLevelPalette()->getStyleIndex();
  }
  void setCurrentLevelStyleIndex(int index, bool forceUpdate) override {
    palette.getCurrentLevelPalette()->setStyleIndex(index, forceUpdate);
  }

  mutable TFrameHandle frame;
  mutable TXshLevelHandle level;
  mutable TXsheetHandle sheet;
  mutable TObjectHandle object;
  mutable TColumnHandle column;
  mutable TSceneHandle scene;
  mutable ToolHandle tool;
  mutable TSelectionHandle selection;
  mutable PaletteController palette;
};

// What one case produced: text lines, and the rasters they refer to.
struct Record {
  std::vector<std::string> lines;
  std::vector<QImage> images;
};

class Fixture {
public:
  Fixture(Application& app, const QString& gestureDir);
  ~Fixture();

  // Starts a case: an empty scene, no undo history, the viewer at its
  // defaults.
  void begin();

  // Each makes the level current at row 0 of a new column with one drawing.
  TXshSimpleLevel* newVectorLevel();
  TXshSimpleLevel* newToonzRasterLevel();
  TXshSimpleLevel* newFullColorLevel();
  // Adds one more drawing of the current level at the next row.
  void addDrawing();
  void goToRow(int row);
  void setStyle(int styleId);
  // A column of assistants and replicators shown at every row.
  void addMetaColumn(const std::vector<TMetaObjectP>& objects);
  // Writes a brush tip image into the library of the private profile and
  // returns its value in the raster Brush's "Tip:" option. The Brush lists
  // the tips when it is activated.
  std::wstring addBrushTip();

  // Makes the named tool current for the image at the current cell.
  TTool* useTool(const char* name);
  TTool* tool() const { return m_tool; }

  // Each sets a tool option as the options bar does: the value, then the
  // tool's onPropertyChanged(). Numbers and pairs take the option's own type.
  void setBool(const char* property, bool value);
  void setNumber(const char* property, double value);
  void setEnum(const char* property, int index);
  void setEnumValue(const char* property, const std::wstring& value);
  void setPair(const char* property, double first, double second);

  // Replays a recorded gesture as the viewer delivers it: hover, press,
  // drags, release, with the clock following the recorded times.
  void play(const char* gesture, const Play& how = Play());

  TImageP image(int row = -1) const;
  TXshSimpleLevel* level() const { return m_level.getPointer(); }

  Viewer viewer;
  Application& app;

private:
  TXshSimpleLevel* newLevel(int type, const wchar_t* name);
  void place(const TImageP& image);
  TProperty* property(const char* name) const;
  void changed(const char* name);
  const Gesture& gesture(const char* name);

  QString m_gestureDir;
  std::vector<std::pair<std::string, Gesture>> m_gestures;
  TXshSimpleLevelP m_level;
  TTool* m_tool     = nullptr;
  int m_levelNumber = 0;
  int m_column      = -1;
  int m_rows        = 0;
};

// The palette every fixture level uses: styles 1 to 4 are solid colours in
// page order 3, 1, 2, 4, so that Palette Order differs from the style ids.
TPaletteP makePalette();

// Appends the strokes, groups and fills of a vector drawing.
void recordVector(Record& record, const std::string& label,
                  const TVectorImageP& image);
// Appends size, savebox, painted pixel count and a digest of a raster drawing,
// and its pixels.
void recordRaster(Record& record, const std::string& label,
                  const TImageP& image);
// Every drawing of the fixture level, in the form the golden data uses.
Record snapshot(Fixture& fixture);
// Appends the drawings of `after`, each raster with the number of pixels that
// differ from `before`.
void recordChange(Record& record, const Record& before, const Record& after);
// Appends the undo history length and whether undoing every step restores
// the drawings of `before` and redoing them gives the current drawings again.
// Where one does not, the drawings it left are appended too.
void recordUndo(Record& record, Fixture& fixture, const Record& before);

// Sets the private profile every case runs in.
void initializeProfile(const QString& root, const char* executable);

}  // namespace brushstroke

#endif
