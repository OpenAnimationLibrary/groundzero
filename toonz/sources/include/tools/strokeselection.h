#pragma once

#ifndef STROKE_SELECTION_H
#define STROKE_SELECTION_H

#include <memory>

// TnzQt includes
#include "toonzqt/selection.h"

// TnzCore includes
#include "tcommon.h"
#include "tvectorimage.h"

// STD includes
#include <set>
#include <utility>
#include <vector>
#include "tools/vectoralignment.h"

#undef DVAPI
#undef DVVAR
#ifdef TNZTOOLS_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//=========================================================

//    Forward declarations

class TGroupCommand;
class TSceneHandle;
class TXshSimpleLevel;
class TFrameId;
namespace WidthProfile {
struct Profile;
}

//=========================================================

//    Undo data structures

struct UndoVectorData {
  int index;
  double offset;
  TStroke *oldStroke;
};

//=========================================================

//****************************************************************************
//    StrokeSelection  declaration
//****************************************************************************

class DVAPI StrokeSelection final : public TSelection {
public:
  typedef std::set<int> IndexesContainer;

public:
  StrokeSelection();
  ~StrokeSelection();

  StrokeSelection(const StrokeSelection &other);
  StrokeSelection &operator=(const StrokeSelection &other);

  TGroupCommand *getGroupCommand() { return m_groupCommand.get(); }

  void setImage(const TVectorImageP &image) {
    if (m_vi != image) m_selectionOrder.clear();
    m_vi = image;
  }
  const TVectorImageP &getImage() const { return m_vi; }

  const IndexesContainer &getSelection() const { return m_indexes; }
  IndexesContainer &getSelection() { return m_indexes; }

  bool isEmpty() const override { return m_indexes.empty(); }
  bool updateSelectionBBox() const { return m_updateSelectionBBox; }
  bool isSelected(int index) const { return (m_indexes.count(index) > 0); }
  void select(int index, bool on);
  void toggle(int index);
  void selectNone() override {
    m_indexes.clear();
    m_selectionOrder.clear();
  }

  void setSceneHandle(TSceneHandle *tsh) { m_sceneHandle = tsh; }

  bool isEditable();

public:
  // Commands

  void changeColorStyle(int styleIndex);
  void deleteStrokes();
  void copy();
  void cut();
  void paste();
  void saveAsCustomVectorBrush();
  void saveVectorSelectionAs();

  void removeEndpoints();
  void sortWithPaletteOrder();

  //! Replaces the selected variable-width strokes by closed zero-thickness
  //! strokes with their outline (see ContourOutline).
  void convertToOutlineShapes();
  void editOpacityProfile();

  //! Width profiles (see tools/widthprofile.h, widthprofilecommands.cpp):
  //! gives the selected strokes the thickness profile along them, keeping
  //! their maximum thickness. \p kind is a WidthProfile::BuiltIn.
  void applyBuiltInWidthProfile(int kind);
  //! Asks which of the saved width profiles to apply to the selection.
  void applySavedWidthProfile();
  //! Saves the width profile of the selected stroke under a name.
  void saveWidthProfile();
  //! Returns false (and changes nothing) if no selected stroke has thickness.
  bool applyWidthProfile(const WidthProfile::Profile &profile,
                         double strength = 1.0, bool multiply = false);
  void enableWidthProfileCommands();

  // Stroke pairing hints for the Inbetween command, stored
  // in versioned scene metadata, scoped to this level and its drawings.
  void pairStrokesForInbetween();
  void clearInbetweenStrokePairs();
  static bool hasInbetweenStrokePairs();
  //! Returns the hinted (first image index, last image index) stroke pairs
  //! that can be found in the specified images, for use with TInbetween.
  static std::vector<std::pair<int, int>> getInbetweenStrokePairs(
      TXshSimpleLevel *level, const TFrameId &first, const TFrameId &last);
  //! Zone hints for the Inbetween command: the first call records the
  //! bounding box of the selection on the first key drawing, the second call
  //! (on the other key drawing) records the target box, saved with the scene.
  void addInbetweenZoneHint();
  //! Returns the hinted (rect in first image, rect in last image) pairs that
  //! can be found in the specified images, for use with TInbetween.
  static std::vector<std::pair<TRectD, TRectD>> getInbetweenZoneHints(
      TXshSimpleLevel *level, const TFrameId &first, const TFrameId &last);

  void enableCommands() override;

  void selectAll();
  void alignStrokes(VectorAlignment::Type type);
  void notifyAlignmentChanged();

private:
  std::vector<int> selectionOrder() const;
  void saveSelectedVectors(bool chooseDestination);

  TVectorImageP m_vi;          //!< Selected vector image.
  IndexesContainer m_indexes;  //!< Selected stroke indexes in m_vi.
  std::vector<int> m_selectionOrder;

  std::unique_ptr<TGroupCommand> m_groupCommand;  //!< Groups commands wrapper.
  TSceneHandle *m_sceneHandle;  //!< Global scene handle. \deprecated  Use
                                //! TApplication instead.

  /*!Set this boolean to true before call tool->notifyImageChanged() when you
want to reset strokes bbox.
N.B. after call tool->notifyImageChanged() set m_updateSelectionBBox to false.*/
  bool m_updateSelectionBBox;
};

#endif  // STROKE_SELECTION_H
