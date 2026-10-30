#pragma once

#ifndef VECTORSELECTIONTOOL_H
#define VECTORSELECTIONTOOL_H

#include "selectiontool.h"

// TnzTools includes
#include "tools/strokeselection.h"
#include "tools/levelselection.h"
#include "tools/cursors.h"

// TnzCore includes
#include "tstroke.h"
#include "tregion.h"

// TnzQt includes
#include "toonzqt/selectioncommandids.h"

// Qt includes
#include <QSet>

//============================================================

//    Forward declarations

class VectorSelectionTool;

//============================================================

//=============================================================================
// Constants / Defines
//-----------------------------------------------------------------------------

enum SelectionTarget  //! Possible selection targets in a SelectionTool.
{
  NORMAL_TYPE_IDX,             //!< Normal selection (strokes at current frame).
  SELECTED_FRAMES_TYPE_IDX,    //!< Selection of whole frames.
  ONION_SKIN_FRAMES_TYPE_IDX,  //!< Whole frames shown by the onion skin (and
                               //!  the current one).
  ALL_LEVEL_TYPE_IDX,          //!< Selection of an entire level.
  SAME_STYLE_TYPE_IDX,         //!< Selected styles at current frame.
  STYLE_SELECTED_FRAMES_TYPE_IDX,  //!< Selected styles at selected frames.
  STYLE_LEVEL_TYPE_IDX,  //!< Selected styles at current frame on the whole
                         //! level.
  BOUNDARY_TYPE_IDX,     //!< Boundary strokes on current frame.
  BOUNDARY_SELECTED_FRAMES_TYPE_IDX,  //!< Boundary strokes on selected frames.
  BOUNDARY_LEVEL_TYPE_IDX,            //!< Boundary strokes on the whole level.
};

#define NORMAL_TYPE L"Standard"
#define SELECTED_FRAMES_TYPE L"Selected Frames"
#define ONION_SKIN_FRAMES_TYPE L"Onion Skin Frames"
#define ALL_LEVEL_TYPE L"Whole Level"
#define SAME_STYLE_TYPE L"Same Style"
#define STYLE_SELECTED_FRAMES_TYPE L"Same Style on Selected Frames"
#define STYLE_LEVEL_TYPE L"Same Style on Whole Level"
#define BOUNDARY_TYPE L"Boundary Strokes"
#define BOUNDARY_SELECTED_FRAMES_TYPE L"Boundaries on Selected Frames"
#define BOUNDARY_LEVEL_TYPE L"Boundaries on Whole Level"

#define ROUNDC_WSTR L"round_cap"
#define BUTT_WSTR L"butt_cap"
#define PROJECTING_WSTR L"projecting_cap"
#define ROUNDJ_WSTR L"round_join"
#define BEVEL_WSTR L"bevel_join"
#define MITER_WSTR L"miter_join"

//=============================================================================
// VectorFreeDeformer
//-----------------------------------------------------------------------------

class VectorFreeDeformer final : public FreeDeformer {
  TVectorImageP m_vi;
  std::set<int> m_strokeIndexes;
  std::vector<TStroke *> m_originalStrokes;

  // Envelope mode
  std::vector<TPointD> m_restEnvelope, m_deformedEnvelope;
  std::vector<std::vector<TThickPoint>>
      m_envelopeSources;  //!< Control points of the original strokes, with
                          //!  their chunks cut in pieces for the warp.
  bool m_isEnvelope = false;

  bool m_preserveThickness, m_computeRegion, m_flip;

  TThickPoint deform(TThickPoint point);
  void deformEnvelopeImage();

public:
  VectorFreeDeformer(TVectorImageP vi, std::set<int> strokeIndexes);
  ~VectorFreeDeformer();

  void setPreserveThickness(bool preserveThickness);
  void setComputeRegion(bool computeRegion);
  void setFlip(bool flip);

  /*! Return \b index point, with index from 0 to 3. */
  TPointD getPoint(int index) const { return m_newPoints[index]; }
  /*! Set \b index point to \b p, with index from 0 to 3. */
  void setPoint(int index, const TPointD &p) override;
  /*! Helper function. */
  void setPoints(const TPointD &p0, const TPointD &p1, const TPointD &p2,
                 const TPointD &p3) override;
  /*! Helper function. */
  TVectorImage *getDeformedImage() const { return m_vi.getPointer(); }

  /*! Envelope mode: deformImage() warps the strokes with the mean value
      coordinates of the closed polygon \b rest moved to \b deformed (see
      TEnvelopeDeformer and EnvelopeLayout) instead of the four corners. */
  void setEnvelope(const std::vector<TPointD> &rest,
                   const std::vector<TPointD> &deformed);
  bool isEnvelope() const { return m_isEnvelope; }

  void deformRegions();
  void deformImage() override;
};

//=============================================================================
// DragSelectionTool
//-----------------------------------------------------------------------------

namespace DragSelectionTool {

//=============================================================================
// UndoChangeStrokes
//-----------------------------------------------------------------------------

class UndoChangeStrokes final : public ToolUtils::TToolUndo {
public:
  UndoChangeStrokes(TXshSimpleLevel *level, const TFrameId &frameId,
                    VectorSelectionTool *tool,
                    const StrokeSelection &selection);
  UndoChangeStrokes(TXshSimpleLevel *level, const TFrameId &frameId,
                    VectorSelectionTool *tool, const LevelSelection &selection);
  ~UndoChangeStrokes();

  void registerStrokes(bool beforeModify = false);
  void setFlip(bool value) { m_flip = value; }
  void transform(const std::vector<TStroke *> &strokes, FourPoints bbox,
                 TPointD center, DeformValues deformValue, bool notify = true) const;
  void restoreRegions() const;
  void restoreBefore() const;
  const TVectorImageP &beforeImage() const { return m_beforeImage; }
  void discardReviewImage() { m_beforeImage = TVectorImageP(); }
  TVectorImageP currentImage() const;
  const TFrameId &frameId() const { return m_frameId; }
  void undo() const override;
  void redo() const override;
  int getSize() const override;

private:
  VectorSelectionTool *m_tool;

  std::vector<TStroke *> m_oldStrokes, m_newStrokes;

  std::vector<int> m_indexes;  //!< Selected indexes at current frame.
  std::vector<TFilledRegionInf> m_regionsData;
  std::vector<TFilledRegionInf> m_newRegionsData;
  TVectorImageP m_beforeImage;

  int m_selectionCount;

  FourPoints m_oldBBox, m_newBBox;

  TPointD m_oldCenter, m_newCenter;

  DeformValues m_oldDeformValues, m_newDeformValues;

  bool m_flip;
};

//=============================================================================
// VectorDeformTool
//-----------------------------------------------------------------------------

class VectorDeformTool : public DeformTool {
public:
  VectorDeformTool(VectorSelectionTool *tool);
  ~VectorDeformTool();

  void applyTransform(FourPoints bbox, bool onFastDragging = false) override;
  void addTransformUndo() override;
  void cancel();
  bool isReviewing() const { return m_isReviewing; }

  /*! Transform whole level and add undo. */
  void transformWholeLevel();
  bool isFlip();

  /*! Gives the free deformer of the box \b index (0 is the current frame) its
      new shape. By default, the four corners of \b bbox. */
  virtual void setDeformerShape(VectorFreeDeformer *deformer, int index,
                                const FourPoints &bbox);

protected:
  std::unique_ptr<UndoChangeStrokes> m_undo;
  bool m_cancelled = false;
  bool m_isReviewing = false;

  bool commitChanges(std::vector<std::unique_ptr<UndoChangeStrokes>> &changes);

protected:
  void leftButtonDrag(const TPointD &pos, const TMouseEvent &e) override {}

  void leftButtonUp(const TPointD &pos, const TMouseEvent &e) override;

  void draw() override {}

private:
  struct VFDScopedBlock;
  std::unique_ptr<VFDScopedBlock> m_vfdScopedBlock;
};

//=============================================================================
// VectorRotationTool
//-----------------------------------------------------------------------------

class VectorRotationTool final : public VectorDeformTool {
  std::unique_ptr<Rotation> m_rotation;

public:
  VectorRotationTool(VectorSelectionTool *tool);

  void transform(TAffine aff, double angle) override;
  void leftButtonDrag(const TPointD &pos, const TMouseEvent &e) override;
  void draw() override;
};

//=============================================================================
// VectorFreeDeformTool
//-----------------------------------------------------------------------------

class VectorFreeDeformTool final : public VectorDeformTool {
  std::unique_ptr<FreeDeform> m_freeDeform;

public:
  VectorFreeDeformTool(VectorSelectionTool *tool);

  void leftButtonDrag(const TPointD &pos, const TMouseEvent &e) override;
};

//=============================================================================
// VectorEnvelopeTool
//-----------------------------------------------------------------------------

/*! Drags a handle of the envelope (Envelope mode): the handle moves, in the box
    of every frame the tool edits, and the strokes are warped from the envelope
    they had at the button press to the moved one. */
class VectorEnvelopeTool final : public VectorDeformTool {
  std::vector<std::vector<TPointD>>
      m_restEnvelopes;  //!< Envelope of each box at the button press.

public:
  VectorEnvelopeTool(VectorSelectionTool *tool);
  ~VectorEnvelopeTool();

  void setDeformerShape(VectorFreeDeformer *deformer, int index,
                        const FourPoints &bbox) override;

  void leftButtonDrag(const TPointD &pos, const TMouseEvent &e) override;
  void leftButtonUp(const TPointD &pos, const TMouseEvent &e) override;
};

//=============================================================================
// VectorMoveSelectionTool
//-----------------------------------------------------------------------------

class VectorMoveSelectionTool final : public VectorDeformTool {
  std::unique_ptr<MoveSelection> m_moveSelection;

public:
  VectorMoveSelectionTool(VectorSelectionTool *tool);

  void transform(TAffine aff) override;
  void leftButtonDown(const TPointD &pos, const TMouseEvent &e) override;
  void leftButtonDrag(const TPointD &pos, const TMouseEvent &e) override;
};

//=============================================================================
// VectorScaleTool
//-----------------------------------------------------------------------------

class VectorScaleTool final : public VectorDeformTool {
  std::unique_ptr<Scale> m_scale;

public:
  VectorScaleTool(VectorSelectionTool *tool, ScaleType type);

  TPointD transform(int index, TPointD newPos, bool onFastDragging = false)
      override;  //!< Returns scale value.

  void leftButtonDown(const TPointD &pos, const TMouseEvent &e) override;
  void leftButtonDrag(const TPointD &pos, const TMouseEvent &e) override;
};

//=============================================================================
// VectorChangeThicknessTool
//-----------------------------------------------------------------------------

class VectorChangeThicknessTool final : public DragTool {
  TPointD m_curPos, m_firstPos;
  std::map<int, std::vector<double>> m_strokesThickness;
  double m_thicknessChange;
  bool m_isIncrementalMode;// Or Fixed Mode
  std::unique_ptr<UndoChangeStrokes> m_undo;

public:
  VectorChangeThicknessTool(VectorSelectionTool *tool);
  ~VectorChangeThicknessTool();

  void setStrokesThickness(TVectorImage &vi);
  void setThicknessChange(double value) { m_thicknessChange = value; }

  void changeImageThickness(TVectorImage &vi, double newThickness);
  void setImageThickness(TVectorImage &vi, double newThickness);

  void addUndo();

  void leftButtonDown(const TPointD &pos, const TMouseEvent &e) override;
  void leftButtonDrag(const TPointD &pos, const TMouseEvent &e) override;
  void leftButtonUp(const TPointD &pos, const TMouseEvent &e) override;
  void draw() override {}
};

}  // namespace DragSelectionTool

//=============================================================================
// VectorSelectionTool
//-----------------------------------------------------------------------------

/*!
  \brief    Selection tool for Toonz's vector images.
*/

class VectorSelectionTool final : public SelectionTool {
  Q_DECLARE_TR_FUNCTIONS(VectorSelectionTool)

public:
  VectorSelectionTool(int targetType);

  void setNewFreeDeformer() override;

  void setCanEnterGroup(bool value) { m_canEnterGroup = value; }

  bool isConstantThickness() const override {
    return m_constantThickness.getValue();
  }
  bool isEnvelopeMode() const override;
  bool isLevelType() const override;
  /*! True when the tool works on a set of frames: the selected ones (frame
          strip) or the ones shown by the onion skin. The set is returned by
          getTargetFrames(). */
  bool isSelectedFramesType() const override;
  bool isOnionSkinFramesType() const;

  /*! The frames the tool works on in the modes where isSelectedFramesType()
          is true. The current frame is always in the set in the onion skin
          mode. */
  const std::set<TFrameId> &getTargetFrames() const;
  bool isSameStyleType() const override;
  bool isModifiableSelectionType() const override;

  const std::set<int> &selectedStyles() const {
    return m_levelSelection.styles();
  }
  std::set<int> &selectedStyles() { return m_levelSelection.styles(); }

  int getSelectionCount() const { return m_selectionCount; }

  void selectionOutlineStyle(int &capStyle, int &joinStyle);

  const StrokeSelection &strokeSelection() const { return m_strokeSelection; }

  const LevelSelection &levelSelection() const { return m_levelSelection; }

  TSelection *getSelection() override;
  bool isSelectionEmpty() override;

  void computeBBox() override;
  void notifyImageChanged() override;

  TPropertyGroup *getProperties(int targetType) override;

  bool m_resetCenter;

  void setResetCenter(bool update) { m_resetCenter = update; }
  bool canResetCenter() { return m_resetCenter; }

  bool isSelectionEditable() override { return m_strokeSelection.isEditable(); }
  bool reviewFills() const { return m_reviewFills.getValue(); }
  bool keyDown(QKeyEvent *event) override;
  bool isEventAcceptable(QEvent *event) override;

protected:
  void onActivate() override;
  void onDeactivate() override;

  void leftButtonDrag(const TPointD &pos, const TMouseEvent &) override;
  void leftButtonDown(const TPointD &pos, const TMouseEvent &) override;
  void leftButtonUp(const TPointD &pos, const TMouseEvent &) override;
  void leftButtonDoubleClick(const TPointD &, const TMouseEvent &e) override;
  void addContextMenuItems(QMenu *menu) override;

  void draw() override;

  void updateAction(TPointD pos, const TMouseEvent &e) override;
  void onSelectedFramesChanged() override;

  bool onPropertyChanged(std::string propertyName) override;
  void onImageChanged() override;

  int getCursorId() const override {
    if (m_viewer && m_viewer->getGuidedStrokePickerMode())
      return m_viewer->getGuidedStrokePickerCursor();
    return m_cursorId;
  }

  bool isDragging() const override;

private:
  class AttachedLevelSelection final : public LevelSelection {
    StrokeSelection &m_strokeSelection;  //!< Selection of strokes to be seen at
                                         //! current frame.

  public:
    AttachedLevelSelection(StrokeSelection &strokeSelection)
        : m_strokeSelection(strokeSelection) {}
    
    // Commands
    void selectNone() override {
      LevelSelection::selectNone(), m_strokeSelection.selectNone();
    }
    void enableCommands() override {
      m_strokeSelection.enableCommands();
      enableCommand(this, MI_SortWithPaletteOrder,
                    &AttachedLevelSelection::sortWithPaletteOrder);
    };
    void sortWithPaletteOrder();
  };

private:
  TEnumProperty m_selectionTarget;  //!< Selected target content (strokes, whole
                                    //! image, styles...).
  TBoolProperty m_includeIntersection;
  TBoolProperty m_constantThickness;
  TBoolProperty m_envelope;  //!< Box handles bend the strokes (Envelope).
  TBoolProperty m_reviewFills;

  StrokeSelection m_strokeSelection;        //!< Standard selection of a set of
                                            //! strokes in current image.
  AttachedLevelSelection m_levelSelection;  //!< Selection across multiple
                                            //! frames and specific styles.

  TEnumProperty m_capStyle;       //!< Style of stroke caps.
  TEnumProperty m_joinStyle;      //!< Style of stroke joins.
  TIntProperty m_miterJoinLimit;  //!< Stroke joins threshold value.
  TDoubleProperty m_opacity;  //!< Opacity (percent) of the selected strokes.

  TPropertyGroup m_outlineProps;

  int m_selectionCount;  //!< \deprecated  This is \a not related to stroke
                         //! selections;
  //!               but rather to bboxes count - and even that is deprecated.
  bool m_canEnterGroup;  //!< \deprecated  Used in Manga and Kids

  std::set<TFrameId> m_onionFrames;  //!< Frames of the onion skin mode. Taken
                                     //!  again each time the selection is
                                     //!  made consistent, so that the bboxes
                                     //!  and the free deformers agree.

private:
  bool refreshOnionFrames();  //!< Updates m_onionFrames. True if they changed.
  void modifySelectionOnClick(TImageP image, const TPointD &pos,
                              const TMouseEvent &e) override;
  bool selectStroke(int index, bool toggle);

  void doOnActivate() override;
  void doOnDeactivate() override;

  void selectRegionVectorImage(bool includeIntersect);

  void updateTranslation() override;

  /*! \details  The updateSelectionTarget() function reads the selection
          target (styles, frames, etc) selected by the user, and ensures
          that the associated internal selection is configured and
          \a current.

          The selected strokes set is \a may be cleared appropriately
          as a result of a change in target.                                  */

  void updateSelectionTarget();  //!< Reads widget fields and updates the
                                 //! selection target accordingly.
  void
  clearSelectedStrokes();    //!< Clears strokes visible in current selection.
  void finalizeSelection();  //!< Makes selection consistent, and invokes all
                             //! related cached data
  //!  updates needed for visualization.
  void drawInLevelType(const TVectorImage &vi);
  void drawSelectedStrokes(const TVectorImage &vi);
  void drawGroup(const TVectorImage &vi);
};

#endif  // VECTORSELECTIONTOOL_H
