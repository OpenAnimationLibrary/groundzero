#pragma once

#ifndef STAGESCHEMATIC_H
#define STAGESCHEMATIC_H

#include "schematicviewer.h"
#include "tgeometry.h"
#include <QMap>
#include <QString>
#include <QStringList>
#include <vector>

// forward declaration
class QMenu;
class TStageObject;
class TFxHandle;
class TXsheetHandle;
class TObjectHandle;
class TColumnHandle;
class TSceneHandle;
class TFrameHandle;
class TXsheet;
class StageSchematicNode;
class StageSchematicSplineNode;
class TStageObjectSpline;
class StageObjectSelection;
class SchematicLink;
class StageSchematicGroupNode;
class StageSchematicGroupEditor;

//==================================================================
//
// TreeStageNode
//
//==================================================================
//! Provide a structure representing a node af a tree.
//! This class maintains information about a node of the stage schematic and its
//! children.
//! The tree is generated using the TStageObjectTree.
//! The generated tree is used to oreder stage schematic nodes in the window.
class TreeStageNode {
  //! The node to replace in the window
  StageSchematicNode *m_node;
  //! The childre of the node
  std::vector<TreeStageNode *> m_cildren;

public:
  TreeStageNode(StageSchematicNode *node) : m_node(node) {}

  ~TreeStageNode() {
    int i;
    for (i = 0; i < (int)m_cildren.size(); i++) {
      delete m_cildren[i];
    }
  }
  //--------------------------------------

  void setSchematicNode(StageSchematicNode *node) { m_node = node; }
  void addChild(TreeStageNode *child) { m_cildren.push_back(child); }

  StageSchematicNode *getNode() { return m_node; }
  int getChildrenCount() { return m_cildren.size(); }
  TreeStageNode *getChildTreeNode(int i) { return m_cildren[i]; }
  //! Sort the children container.
  //! The sorting follows this rules:\n
  //! \li TableNode < CameraNode < PegbarNode < ColumnNode
  //! \li if two nodes are of the same type, they are ordered using indexes
  void sortChildren(int startIndex, int lastIndex);
  void reverseChildren() { std::reverse(m_cildren.begin(), m_cildren.end()); }
};

//==================================================================
//
// StageSchematic
//
//==================================================================

//! It is the implementation of the SchematicScene representig the
//! QGraphicsScene for the Stage Schematic.
//! All StageSchematicNode,  StageSchematicSplineNode and link are pleced and
//! handled in this scene.
//! The scene maintains two mapping:
//! one for StageSchematicNode and one for StageSchematicSplineNode. These
//! mapping can be used to retrieve
//! nodes and spline.\n
//! To regenearte the scene use the updateScene().\n
//! To oreder nodes in the scene use the reorderScene().

class StageSchematicScene final : public SchematicScene {
  Q_OBJECT
  TXsheetHandle *m_xshHandle;
  TObjectHandle *m_objHandle;
  TColumnHandle *m_colHandle;
  TSceneHandle *m_sceneHandle;
  TFrameHandle *m_frameHandle;
  TFxHandle *m_fxHandle = nullptr;
  TPointD m_nextNodePos;
  TPointD m_firstPos;
  QMap<TStageObjectId, StageSchematicNode *> m_nodeTable;
  QMap<int, StageSchematicGroupNode *> m_GroupTable;
  QMap<int, StageSchematicGroupEditor *> m_groupEditorTable;
  QMap<TStageObjectSpline *, StageSchematicSplineNode *> m_splineTable;
  StageObjectSelection *m_selection;
  int m_gridDimension;

  QMap<TStageObjectId, QList<StageSchematicNode *>> m_nodesToPlace;

  bool m_showLetterOnPortFlag;

  // Rig view: display only, off by default and not saved
  bool m_rigViewEnabled = false;
  QString m_rigFilter;
  //! The texts of the Rig view items of each node, matched by the filter
  QMap<TStageObjectId, QStringList> m_rigSearchText;

  SchematicViewer *m_viewer;

public:
  StageSchematicScene(QWidget *parent);
  ~StageSchematicScene();

  //! Clear all item an regenerate the Stage Schematic graph.
  void updateScene() override;

  //! Returns the current node.
  QGraphicsItem *getCurrentNode() override;

  //! Replace all nodes in the scene.
  void reorderScene() override;
  TXsheet *getXsheet();
  TXsheetHandle *getXsheetHandle() { return m_xshHandle; }

  void setXsheetHandle(TXsheetHandle *xshHandle);
  void setObjectHandle(TObjectHandle *objHandle);
  void setColumnHandle(TColumnHandle *colHandle);
  void setFxHandle(TFxHandle *fxHandle);
  void setSceneHandle(TSceneHandle *sceneHandle) {
    m_sceneHandle = sceneHandle;
  }
  void setFrameHandle(TFrameHandle *frameHandle) {
    m_frameHandle = frameHandle;
  }
  TStageObjectId getCurrentObject();
  TFrameHandle *getFrameHandle() { return m_frameHandle; }
  TColumnHandle *getColumnHandle() { return m_colHandle; }
  void updateNestedGroupEditors(StageSchematicNode *node,
                                const QPointF &newPos);
  void resizeNodes(bool maximizedNode);

  bool isShowLetterOnPortFlagEnabled() { return m_showLetterOnPortFlag; }
  bool isRigViewEnabled() const { return m_rigViewEnabled; }

  //! Adds the Rig view commands of the object (Publish Channel...,
  //! Unpublish Channel...) to the context menu of its node. It does nothing
  //! when the Rig view is off.
  void addRigMenuActions(QMenu &menu, const TStageObjectId &id);

  SchematicViewer *getSchematicViewer() { return m_viewer; }

  StageObjectSelection *getStageSelection() const { return m_selection; }

private:
  StageSchematicNode *addStageSchematicNode(TStageObject *pegbar);
  StageSchematicGroupNode *addStageGroupNode(QList<TStageObject *> objs);
  StageSchematicSplineNode *addSchematicSplineNode(TStageObjectSpline *spline);
  StageSchematicGroupEditor *addEditedGroupedStageSchematicNode(
      int groupId, const QList<SchematicNode *> &groupedObjs);
  StageSchematicNode *createStageSchematicNode(StageSchematicScene *scene,
                                               TStageObject *pegbar);

  //! Places all nodes in the window
  void placeNodes();

  //! Adds the Rig view badges to the column nodes and applies the filter.
  void applyRigView();
  //! Shows only the nodes whose name, or the text of one of their Rig view
  //! items, contains the Rig view filter text.
  void applyRigFilter();
  //! Refreshes the values of the published channel rows (Rig view).
  void refreshRigPorts();

  //! Returns the text of a published channel row: its name and the value of
  //! the channel at the current frame.
  QString getPublishedChannelText(const TStageObjectId &id, int index);
  //! Makes the channel the current one of the Function Editor, which is
  //! opened if needed, so that it can be keyed there.
  void selectPublishedChannel(const TStageObjectId &id, int index);
  //! Asks a value for the channel, and keys it at the current frame (or sets
  //! the weight of the layer) as a single undo.
  void editPublishedChannel(const TStageObjectId &id, int index);
  //! Dialogs of the commands Publish Channel... and Unpublish Channel...
  void publishChannelDialog(const TStageObjectId &id);
  void unpublishChannelDialog(const TStageObjectId &id);
  //! Rig view badge clicks: selects a deformer fx and opens the FX settings,
  //! or opens the panel of the offset layers or of the master controllers.
  void openRigDeformer(const std::vector<TFxP> &fxs);
  void openRigPanel(const TStageObjectId &id, const char *commandId);

  //! Search nodes in the TStageObjectTree that are roots of a tree.
  //! Roots are placed in the container \b roots
  void findRoots(std::vector<TreeStageNode *> &roots);

  //! Build the tree starting from \b treenode.
  //! The tree is build recursively on each children of \b treeNode.
  void makeTree(TreeStageNode *treeNode);

  //! Give the right position to the \b treeNode children.
  //! All nodes of the stage schematic are replaced recursively on each children
  //! starting from a root.
  //! \b xPos and \b yPos are update from the method.
  //! \sa findRoots(vector<TreeNode*> &roots)
  void placeChildren(TreeStageNode *treeNode, double &xPos, double &yPos,
                     bool isCameraTree = false);

  //! Place a StageSchematicNode in the window.
  //! It is usually used to place new nodes that have not position.
  void placeNode(StageSchematicNode *node);

  //! Place a StageSchematicSplineNode in the window.
  //! It is usually used to place new splines that have not position.
  void placeSplineNode(StageSchematicSplineNode *splineNode);

  SchematicNode *getNodeFromPosition(const QPointF &pos);

  void updatePosition(StageSchematicNode *node, const TPointD &pos);
  void highlightLinks(StageSchematicNode *node, bool value);
  void updateEditedGroups(const QMap<int, QList<SchematicNode *>> &editedGroup);
  void updatePositionOnResize(TStageObject *obj, bool maximizedNode);
  void updateSplinePositionOnResize(TStageObjectSpline *spl,
                                    bool maximizedNode);

  // snap to neighbor nodes on dragging
  void updateSnapTarget(QGraphicsItem *item) override;

protected:
  void contextMenuEvent(QGraphicsSceneContextMenuEvent *cme) override;
  void mousePressEvent(QGraphicsSceneMouseEvent *me) override;

signals:
  void editObject();
  void doCollapse(QList<TStageObjectId>);
  void doExplodeChild(QList<TStageObjectId>);

public slots:
  // Public wrapper slots for modern Qt5 connection syntax
  void onResetCenterPublic() { onResetCenter(); }
  void onEditGroupPublic() { onEditGroup(); }
  void onCameraActivatePublic() { onCameraActivate(); }
  void onRemoveSplinePublic() { onRemoveSpline(); }
  void onSaveSplinePublic() { onSaveSpline(); }
  void onLoadSplinePublic() { onLoadSpline(); }
  void onPathToggledPublic(int state) { onPathToggled(state); }
  void onCpToggledPublic(bool toggled) { onCpToggled(toggled); }

protected slots:
  void onSelectionSwitched(TSelection *oldSel, TSelection *newSel) override;

  void onPegbarAdded();
  void onSplineAdded();
  void onCameraAdded();
  void onResetCenter();
  void onCameraActivate();
  void onRemoveSpline();
  void onSaveSpline();
  void onLoadSpline();

  void onPathToggled(int state);
  void onCpToggled(bool toggled);

  void onXsheetChanged();
  void onSceneChanged();
  void onCurrentObjectChanged(const TStageObjectId &, bool);
  void onCurrentColumnChanged(int);
  void onSelectionChanged();
  void onCollapse(QList<TStageObjectId>);
  void onEditGroup();
  void onNodeChangedSize();

  void onSwitchPortModeToggled(bool withLetter);

  //! Shows or hides the rig badges of the column nodes (Rig view).
  void onRigViewToggled(bool enabled);
  //! Hides the nodes that do not match \b text (Rig view only).
  void onRigFilterChanged(const QString &text);
  //! Refreshes the published channel values on the frame change (Rig view).
  void onRigFrameSwitched();
};

#endif  // STAGESCHEMATIC_H
