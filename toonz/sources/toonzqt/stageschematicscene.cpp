

#include "toonzqt/uitranslations.h"
#include "toonzqt/stageschematicscene.h"

// TnzQt includes
#include "toonzqt/stageschematicnode.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/gutil.h"
#include "toonzqt/schematicgroupeditor.h"
#include "stageobjectselection.h"
#include "toonzqt/icongenerator.h"
#include "toonzqt/functionviewer.h"
#include "toonzqt/menubarcommand.h"

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tframehandle.h"
#include "toonz/txshcolumn.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tstageobjectspline.h"
#include "toonz/tcamera.h"
#include "toonz/tstageobjectcmd.h"
#include "toonz/tproject.h"
#include "toonz/childstack.h"
#include "toonz/fxdag.h"
#include "toonz/tfxhandle.h"
#include "toonz/doubleparamcmd.h"

// TnzCore includes
#include "tconst.h"
#include "tsystem.h"
#include "tstream.h"
#include "tstroke.h"
#include "tenv.h"
#include "tfx.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tunit.h"

// Qt includes
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QTimer>
#include <QMenu>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneMouseEvent>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QGraphicsItem>
#include <QPainter>
#include <QSet>
#include <QStringList>

#include <cctype>
#include <functional>
#include <set>
#include <string>
#include <vector>

TEnv::IntVar ShowLetterOnOutputPortOfStageNode(
    "ShowLetterOnOutputPortOfStageNode", 0);

namespace {

//! Define the method to sort a TreeStageNode container.
//! The sorting follows these rules:
//! \li TableNode < CameraNode < PegbarNode < ColumnNode
//! \li if two nodes are of the same type, they are ordered using indexes
class CompareNodes {
public:
  bool operator()(TreeStageNode *node1, TreeStageNode *node2) {
    TStageObjectId id1 = node1->getNode()->getStageObject()->getId();
    TStageObjectId id2 = node2->getNode()->getStageObject()->getId();

    if (id1.isTable() ||
        (id1.isCamera() && !id2.isTable() && !id2.isCamera()) ||
        (id1.isPegbar() && !id2.isTable() && !id2.isCamera() &&
         !id2.isPegbar()))
      return true;
    if ((id1.isCamera() && id2.isCamera()) ||
        (id1.isTable() && id2.isTable()) ||
        (id1.isPegbar() && id2.isPegbar()) ||
        (id1.isColumn() && id2.isColumn()))
      return id1.getIndex() < id2.getIndex();
    return false;
  }
};

//------------------------------------------------------------------

TStageObject *getRoot(const QList<TStageObject *> &objs,
                      TStageObjectTree *pegTree) {
  int i;
  for (i = 0; i < objs.size(); i++) {
    TStageObject *parent = pegTree->getStageObject(objs[i]->getParent(), false);
    if (objs.contains(parent))
      continue;
    else
      return objs[i];
  }
  return 0;
}

//------------------------------------------------------------------

void keepSubgroup(QMap<int, QList<SchematicNode *>> &editedGroup) {
  QMap<int, QList<SchematicNode *>>::iterator it;
  for (it = editedGroup.begin(); it != editedGroup.end(); it++) {
    QList<SchematicNode *> groupedNodes = it.value();
    int i;
    for (i = 0; i < groupedNodes.size(); i++) {
      StageSchematicNode *node =
          dynamic_cast<StageSchematicNode *>(groupedNodes[i]);
      if (!node) continue;
      TStageObject *obj = node->getStageObject();
      assert(obj);
      QMap<int, QList<SchematicNode *>>::iterator it2;
      for (it2 = editedGroup.begin(); it2 != editedGroup.end(); it2++) {
        if (obj->isContainedInGroup(it2.key()) &&
            !editedGroup[it2.key()].contains(node))
          editedGroup[it2.key()].append(node);
      }
    }
  }
}

bool resizingNodes = false;
bool updatingScene = false;
//==================================================================
//
// Rig view
//
//==================================================================

//! A small label under a node (Rig view). It only has a tool tip, unless it
//! is given a click handler (and a double click one): then it takes the left
//! button. The handlers run once the mouse event is over, as they usually
//! rebuild the scene, and with it this item.
class RigBadgeItem final : public QGraphicsItem {
  QString m_text;
  QColor m_color;
  QRectF m_rect;
  bool m_ignoreRelease = false;
  std::function<void()> m_onClick, m_onDoubleClick;
  std::function<QString()> m_textFn;  //!< Gives the text again, see refresh()
  QString m_portName;  //!< The name of a published channel row, matched by
                       //!< the port filter (empty for the badges)
  bool m_isPortRow = false;

  void fire(const std::function<void()> &handler) {
    if (!handler || !scene()) return;
    QTimer::singleShot(0, scene(), [handler]() { handler(); });
  }

public:
  RigBadgeItem(QGraphicsItem *parent, const QString &text,
               const QString &toolTip, const QColor &color)
      : QGraphicsItem(parent), m_color(color) {
    setText(text);
    setToolTip(toolTip);
    setAcceptedMouseButtons(Qt::NoButton);
    setZValue(1);
  }

  static QFont badgeFont() {
    QFont f;
    f.setPixelSize(9);
    return f;
  }

  void setText(const QString &text) {
    QFontMetrics fm(badgeFont());
    prepareGeometryChange();
    m_text = fm.elidedText(text, Qt::ElideRight, 150);
    m_rect = QRectF(0, 0, fm.horizontalAdvance(m_text) + 8, fm.height() + 2);
    update();
  }

  //! The item makes its text with \b textFn when it is refreshed.
  void setTextFunction(const std::function<QString()> &textFn) {
    m_textFn = textFn;
  }
  void refresh() {
    if (m_textFn) setText(m_textFn());
  }

  //! Makes the item the row of the published channel \b name
  void setPortName(const QString &name) {
    m_portName  = name;
    m_isPortRow = true;
  }
  bool isPortRow() const { return m_isPortRow; }
  const QString &getPortName() const { return m_portName; }

  void setClickHandlers(
      const std::function<void()> &onClick,
      const std::function<void()> &onDoubleClick = std::function<void()>()) {
    m_onClick       = onClick;
    m_onDoubleClick = onDoubleClick;
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(Qt::PointingHandCursor);
  }

  QRectF boundingRect() const override { return m_rect; }

  void paint(QPainter *painter, const QStyleOptionGraphicsItem *,
             QWidget *) override {
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(m_color);
    painter->drawRoundedRect(m_rect, 3, 3);
    painter->setPen(Qt::white);
    painter->setFont(badgeFont());
    painter->drawText(m_rect, Qt::AlignCenter, m_text);
  }

protected:
  void mousePressEvent(QGraphicsSceneMouseEvent *event) override {
    if (event->button() == Qt::LeftButton) {
      m_ignoreRelease = false;
      event->accept();
    } else
      event->ignore();
  }
  void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override {
    if (m_ignoreRelease) {
      m_ignoreRelease = false;
      return;
    }
    if (event->button() == Qt::LeftButton && m_rect.contains(event->pos()))
      fire(m_onClick);
  }
  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override {
    event->accept();
    m_ignoreRelease = true;
    fire(m_onDoubleClick);
  }
};

//------------------------------------------------------------------

//! The channels offered by Publish Channel..., as for the offset layers
const struct {
  TStageObject::Channel m_channel;
  const char *m_label;
} rigChannels[] = {
    {TStageObject::T_X, QT_TR_NOOP("X")},
    {TStageObject::T_Y, QT_TR_NOOP("Y")},
    {TStageObject::T_Z, QT_TR_NOOP("Z")},
    {TStageObject::T_SO, QT_TR_NOOP("SO")},
    {TStageObject::T_Angle, QT_TR_NOOP("Rotation")},
    {TStageObject::T_ScaleX, QT_TR_NOOP("Scale H")},
    {TStageObject::T_ScaleY, QT_TR_NOOP("Scale V")},
    {TStageObject::T_Scale, QT_TR_NOOP("Scale")},
    {TStageObject::T_ShearX, QT_TR_NOOP("Shear H")},
    {TStageObject::T_ShearY, QT_TR_NOOP("Shear V")},
    {TStageObject::T_RotX, QT_TR_NOOP("Rotation X")},
    {TStageObject::T_RotY, QT_TR_NOOP("Rotation Y")},
};

//------------------------------------------------------------------

//! Returns the value of the curve at \b frame in the units shown to the user,
//! and their name in \b unitName.
double getDisplayValue(TDoubleParam *param, double frame,
                       QString *unitName = 0) {
  double value      = param->getValue(frame);
  TMeasure *measure = param->getMeasure();
  const TUnit *unit = measure ? measure->getCurrentUnit() : 0;
  if (unit) {
    value = unit->convertTo(value);
    if (unitName)
      *unitName = QString::fromStdWString(unit->getDefaultExtension());
  }
  return value;
}

//------------------------------------------------------------------

//! Returns a visible Function Editor, or 0.
FunctionViewer *findVisibleFunctionViewer() {
  for (QWidget *widget : QApplication::topLevelWidgets())
    for (FunctionViewer *viewer : widget->findChildren<FunctionViewer *>())
      if (viewer->isVisible()) return viewer;
  return 0;
}

//------------------------------------------------------------------

//! The label of the deformer fx types shown by the Rig view, or an empty
//! string for any other fx.
QString getDeformerLabel(TFx *fx) {
  std::string type = fx->getFxType();
  if (type == "deformerStackFx")
    return StageSchematicScene::tr("Deformer Stack");
  if (type == "envelopeDeformerFx") return StageSchematicScene::tr("Envelope");
  if (type == "freeFormDeformerFx") return StageSchematicScene::tr("Free-Form");
  return QString();
}

//------------------------------------------------------------------

//! Returns the deformer fxs in the chain that starts from the fx of
//! \b column. The chain follows the output connections of the fxs, and
//! stops at the fxs that combine several inputs (they are not the column's
//! own) and at the xsheet fx.
std::vector<TFx *> getColumnDeformers(TXsheet *xsh, TXshColumn *column) {
  std::vector<TFx *> deformers;
  TFx *columnFx = column ? column->getFx() : 0;
  if (!columnFx) return deformers;
  TFx *xsheetFx = xsh->getFxDag()->getXsheetFx();

  std::set<TFx *> visited;
  std::vector<TFx *> toVisit;
  visited.insert(columnFx);
  toVisit.push_back(columnFx);
  while (!toVisit.empty()) {
    TFx *fx = toVisit.back();
    toVisit.pop_back();
    for (int i = 0; i < fx->getOutputConnectionCount(); i++) {
      TFxPort *outPort = fx->getOutputConnection(i);
      TFx *next        = outPort ? outPort->getOwnerFx() : 0;
      if (!next || next == xsheetFx || !visited.insert(next).second) continue;
      int connected = 0;
      for (int p = 0; p < next->getInputPortCount(); p++)
        if (next->getInputPort(p)->isConnected()) connected++;
      if (connected > 1) continue;
      if (!getDeformerLabel(next).isEmpty()) deformers.push_back(next);
      toVisit.push_back(next);
    }
  }
  return deformers;
}

//------------------------------------------------------------------

//! Returns whether \b text, an expression, contains the reference \b ref (for
//! example "peg3.rot") as a whole word.
bool expressionRefersTo(const std::string &text, const std::string &ref) {
  std::string lowerText = text;
  for (char &c : lowerText) c = (char)tolower((unsigned char)c);
  size_t pos = 0;
  while ((pos = lowerText.find(ref, pos)) != std::string::npos) {
    size_t end   = pos + ref.size();
    bool startOk = pos == 0 || !(isalnum((unsigned char)lowerText[pos - 1]) ||
                                 lowerText[pos - 1] == '_');
    bool endOk =
        end >= lowerText.size() ||
        !(isalnum((unsigned char)lowerText[end]) || lowerText[end] == '_');
    if (startOk && endOk) return true;
    pos++;
  }
  return false;
}

//! Returns whether an expression keyframe of \b param refers to \b ref.
bool paramRefersTo(const TDoubleParam *param, const std::string &ref) {
  if (!param) return false;
  for (int i = 0; i < param->getKeyframeCount(); i++) {
    const TDoubleKeyframe &kf = param->getKeyframe(i);
    if (kf.m_type == TDoubleKeyframe::Expression &&
        expressionRefersTo(kf.m_expressionText, ref))
      return true;
  }
  return false;
}

}  // namespace

//==================================================================
//
// TreeStageNode
//
//==================================================================

void TreeStageNode::sortChildren(int startIndex, int lastIndex) {
  if (startIndex == lastIndex) return;
  std::vector<TreeStageNode *>::iterator begin, end;
  begin = m_cildren.begin() + startIndex;
  end   = m_cildren.begin() + lastIndex;
  std::sort(begin, end, CompareNodes());
}

//==================================================================
//
// StageSchematicScene
//
//==================================================================

StageSchematicScene::StageSchematicScene(QWidget *parent)
    : SchematicScene(parent)
    , m_nextNodePos(0, 0)
    , m_xshHandle(0)
    , m_objHandle(0)
    , m_colHandle(0)
    , m_sceneHandle(0)
    , m_frameHandle(0)
    , m_gridDimension(eSmall)
    , m_showLetterOnPortFlag(ShowLetterOnOutputPortOfStageNode != 0)
    , m_viewer() {
  m_viewer = (SchematicViewer *)parent;

  QPointF sceneCenter = sceneRect().center();
  m_firstPos          = TPointD(sceneCenter.x(), sceneCenter.y());

  m_selection = new StageObjectSelection();
  connect(m_selection, SIGNAL(doCollapse(QList<TStageObjectId>)), this,
          SLOT(onCollapse(QList<TStageObjectId>)));
  connect(m_selection, SIGNAL(doExplodeChild(QList<TStageObjectId>)), this,
          SIGNAL(doExplodeChild(QList<TStageObjectId>)));
  connect(this, SIGNAL(selectionChanged()), this, SLOT(onSelectionChanged()));
  m_highlightedLinks.clear();
}

//------------------------------------------------------------------

StageSchematicScene::~StageSchematicScene() {}

//------------------------------------------------------------------

void StageSchematicScene::setXsheetHandle(TXsheetHandle *xshHandle) {
  m_xshHandle = xshHandle;
  m_selection->setXsheetHandle(xshHandle);
}

//------------------------------------------------------------------

void StageSchematicScene::setObjectHandle(TObjectHandle *objHandle) {
  m_objHandle = objHandle;
  m_selection->setObjectHandle(objHandle);
}

//------------------------------------------------------------------

void StageSchematicScene::setColumnHandle(TColumnHandle *colHandle) {
  m_colHandle = colHandle;
  m_selection->setColumnHandle(colHandle);
}

//------------------------------------------------------------------

void StageSchematicScene::setFxHandle(TFxHandle *fxHandle) {
  m_fxHandle = fxHandle;
  m_selection->setFxHandle(fxHandle);
}

//------------------------------------------------------------------

void StageSchematicScene::onSelectionSwitched(TSelection *oldSel,
                                              TSelection *newSel) {
  if (m_selection == oldSel && m_selection != newSel) clearSelection();
}

//------------------------------------------------------------------

void StageSchematicScene::updateScene() {
  if (updatingScene) return;
  updatingScene = true;
  clearAllItems();

  QPointF firstPos = sceneRect().center();
  m_nextNodePos    = TPointD(firstPos.x(), firstPos.y());

  TStageObjectTree *pegTree = m_xshHandle->getXsheet()->getStageObjectTree();

  m_nodeTable.clear();
  m_GroupTable.clear();
  m_groupEditorTable.clear();
  m_gridDimension = pegTree->getDagGridDimension();

  QMap<int, QList<TStageObject *>> groupedObj;
  QMap<int, QList<SchematicNode *>> editedGroup;
  // in order to draw the position-specified nodes first
  QList<int> modifiedNodeIds;
  for (int i = 0; i < pegTree->getStageObjectCount(); i++) {
    TStageObject *pegbar = pegTree->getStageObject(i);
    if (pegbar->getDagNodePos() == TConst::nowhere)
      modifiedNodeIds.push_back(i);
    else
      modifiedNodeIds.push_front(i);
  }

  for (int i = 0; i < modifiedNodeIds.size(); i++) {
    TStageObject *pegbar = pegTree->getStageObject(modifiedNodeIds.at(i));
    if (pegbar->isGrouped() && !pegbar->isGroupEditing()) {
      groupedObj[pegbar->getGroupId()].push_back(pegbar);
      continue;
    }
    StageSchematicNode *node = addStageSchematicNode(pegbar);
    if (node != 0) {
      m_nodeTable[pegbar->getId()] = node;
      if (pegbar->isGrouped())
        editedGroup[pegbar->getEditingGroupId()].append(node);
    }
  }

  // Motion Path
  m_splineTable.clear();
  for (int i = 0; i < pegTree->getSplineCount(); i++) {
    TStageObjectSpline *spline     = pegTree->getSpline(i);
    StageSchematicSplineNode *node = addSchematicSplineNode(spline);
    if (node != 0) {
      m_splineTable[spline] = node;
      connect(node, SIGNAL(currentObjectChanged(const TStageObjectId &, bool)),
              this, SLOT(onCurrentObjectChanged(const TStageObjectId &, bool)));
    }
  }

  QMap<int, QList<TStageObject *>>::iterator it;
  for (it = groupedObj.begin(); it != groupedObj.end(); it++) {
    StageSchematicGroupNode *node = addStageGroupNode(it.value());
    m_GroupTable[it.key()]        = node;
    int editingGroupId            = node->getStageObject()->getEditingGroupId();
    if (editingGroupId != -1) editedGroup[editingGroupId].append(node);
  }

  keepSubgroup(editedGroup);
  updateEditedGroups(editedGroup);

  // update stage links
  for (int i = 0; i < pegTree->getStageObjectCount(); i++)

  {
    TStageObject *pegbar    = pegTree->getStageObject(i);
    TStageObjectId parentId = pegbar->getParent();
    // update link; find StageSchematicNode representing pegbar
    QMap<TStageObjectId, StageSchematicNode *>::iterator it;

    if (!pegbar->isGrouped() || pegbar->isGroupEditing()) {
      it = m_nodeTable.find(pegbar->getId());

      //! note: not every stage object is represented in the scene (e.g. empty
      //! columns are not)
      if (it == m_nodeTable.end()) continue;
      StageSchematicNode *node = it.value();
      // get pegbar parent
      if (parentId != TStageObjectId::NoneId) {
        // find StageSchematicNode representing parentId
        it = m_nodeTable.find(parentId);
        if (it != m_nodeTable.end())  // at least we avoid crash
        {
          StageSchematicNode *parentNode = it.value();
          if (parentNode) {
            StageSchematicNodePort *port1 = parentNode->makeChildPort(
                QString::fromStdString(pegbar->getParentHandle()));
            StageSchematicNodePort *port2 = node->makeParentPort(
                QString::fromStdString(pegbar->getHandle()));
            port2->makeLink(port1);
          }
        }
        TStageObject *parent = pegTree->getStageObject(parentId, false);
        if (parent && parent->isGrouped()) {
          int groupId = parent->getGroupId();
          if (m_GroupTable.contains(groupId)) {
            StageSchematicGroupNode *groupeNode = m_GroupTable[groupId];
            StageSchematicNodePort *port1       = groupeNode->getChildPort(0);
            StageSchematicNodePort *port2       = node->makeParentPort(
                QString::fromStdString(pegbar->getHandle()));
            port2->makeLink(port1);
          }
        }
      }

      // link to motion path
      TStageObjectSpline *spline = pegbar->getSpline();
      if (spline) {
        QMap<TStageObjectSpline *, StageSchematicSplineNode *>::iterator
            splineIt;
        splineIt = m_splineTable.find(spline);
        if (splineIt != m_splineTable.end()) {
          SchematicPort *port1 = splineIt.value()->getPort(-1);
          SchematicPort *port2 = node->getPort(-1);
          port2->makeLink(port1);
        }
      }
    } else {
      int groupId                         = pegbar->getGroupId();
      StageSchematicGroupNode *groupeNode = m_GroupTable[groupId];
      TStageObject *parent = pegTree->getStageObject(parentId, false);
      if (parent && (!parent->isGrouped() || parent->isGroupEditing())) {
        it = m_nodeTable.find(parentId);
        if (it == m_nodeTable.end()) continue;
        StageSchematicNode *parentNode = it.value();
        if (parentNode) {
          StageSchematicNodePort *port1 = parentNode->makeChildPort(
              QString::fromStdString(pegbar->getParentHandle()));
          StageSchematicNodePort *port2 = groupeNode->getParentPort();
          port2->makeLink(port1);
        }
      }
      if (parent && parent->isGrouped()) {
        int parentGroupId = parent->getGroupId();
        if (m_GroupTable.contains(parentGroupId)) {
          StageSchematicGroupNode *groupeParentNode =
              m_GroupTable[parentGroupId];
          StageSchematicNodePort *port1 = groupeParentNode->getChildPort(0);
          StageSchematicNodePort *port2 = groupeNode->getParentPort();
          if (!port1->isLinkedTo(port2) && groupeParentNode != groupeNode)
            port2->makeLink(port1);
        }
      }
      TStageObjectSpline *spline = pegbar->getSpline();
      if (spline) {
        QMap<TStageObjectSpline *, StageSchematicSplineNode *>::iterator
            splineIt;
        splineIt = m_splineTable.find(spline);
        if (splineIt != m_splineTable.end()) {
          SchematicPort *port1 = splineIt.value()->getPort(-1);
          SchematicPort *port2 = groupeNode->getPort(-1);
          port2->makeLink(port1);
        }
      }
    }
  }
  m_nodesToPlace.clear();
  if (m_rigViewEnabled) applyRigView();
  updatingScene = false;
}

//------------------------------------------------------------------

StageSchematicNode *StageSchematicScene::addStageSchematicNode(
    TStageObject *pegbar) {
  // create the node according to the type of StageObject
  StageSchematicNode *node = createStageSchematicNode(this, pegbar);
  if (!node) return 0;
  connect(node, SIGNAL(sceneChanged()), this, SLOT(onSceneChanged()));
  connect(node, SIGNAL(xsheetChanged()), this, SLOT(onXsheetChanged()));
  connect(node, SIGNAL(currentObjectChanged(const TStageObjectId &, bool)),
          this, SLOT(onCurrentObjectChanged(const TStageObjectId &, bool)));
  connect(node, SIGNAL(currentColumnChanged(int)), this,
          SLOT(onCurrentColumnChanged(int)));
  connect(node, SIGNAL(editObject()), this, SIGNAL(editObject()));
  connect(node, SIGNAL(nodeChangedSize()), this, SLOT(onNodeChangedSize()));

  // specify the node position
  if (pegbar->getDagNodePos() == TConst::nowhere) {
    if (pegbar->getId().isColumn()) {
      StageSchematicColumnNode *cNode =
          dynamic_cast<StageSchematicColumnNode *>(node);
      assert(node);
      cNode->resize(m_gridDimension == 0);
    }
    placeNode(node);
  } else
    updatePosition(node, pegbar->getDagNodePos());
  return node;
}

//------------------------------------------------------------------

StageSchematicGroupNode *StageSchematicScene::addStageGroupNode(
    QList<TStageObject *> objs) {
  if (objs.isEmpty()) return 0;
  TStageObjectTree *objTree = m_xshHandle->getXsheet()->getStageObjectTree();
  TStageObject *root        = getRoot(objs, objTree);
  StageSchematicGroupNode *node = new StageSchematicGroupNode(this, root, objs);
  if (!node) return 0;
  connect(node, SIGNAL(sceneChanged()), this, SLOT(onSceneChanged()));
  connect(node, SIGNAL(xsheetChanged()), this, SLOT(onXsheetChanged()));
  connect(node, SIGNAL(currentObjectChanged(const TStageObjectId &, bool)),
          this, SLOT(onCurrentObjectChanged(const TStageObjectId &, bool)));
  connect(node, SIGNAL(currentColumnChanged(int)), this,
          SLOT(onCurrentColumnChanged(int)));
  connect(node, SIGNAL(editObject()), this, SIGNAL(editObject()));
  if (root->getDagNodePos() == TConst::nowhere) {
    placeNode(node);
  } else
    updatePosition(node, root->getDagNodePos());
  return node;
}

//------------------------------------------------------------------

StageSchematicGroupEditor *
StageSchematicScene::addEditedGroupedStageSchematicNode(
    int groupId, const QList<SchematicNode *> &groupedObjs) {
  StageSchematicGroupEditor *editorGroup =
      new StageSchematicGroupEditor(groupId, groupedObjs, this);
  m_groupEditorTable[groupId] = editorGroup;
  return editorGroup;
}

//------------------------------------------------------------------

void StageSchematicScene::updatePosition(StageSchematicNode *node,
                                         const TPointD &pos) {
  node->setPos(QPointF(pos.x, pos.y));
  node->getStageObject()->setDagNodePos(pos);
  // this operation may abort
  /*
QVector<SchematicNode*> placedNodes = getPlacedNode(node);
TPointD offset(15,15);
int i;
for(i=0; i<placedNodes.size(); i++)
{
StageSchematicNode *placedNode =
dynamic_cast<StageSchematicNode*>(placedNodes[i]);
assert(placedNode);
TPointD newPos = placedNode->getStageObject()->getDagNodePos()+offset;
updatePosition(placedNode,newPos);
}
*/
}

//------------------------------------------------------------------

void StageSchematicScene::highlightLinks(StageSchematicNode *node, bool value) {
  int i, portCount = node->getChildCount();
  for (i = 0; i < portCount; i++) {
    StageSchematicNodePort *port = node->getChildPort(i);
    int j, linkCount = port->getLinkCount();
    for (j = 0; j < linkCount; j++) {
      SchematicLink *link = port->getLink(j);
      if (!link) continue;
      link->setHighlighted(value);
      link->update();
      m_highlightedLinks.push_back(link);
    }
  }

  StageSchematicNodePort *port = node->getParentPort();
  if (port) {
    int linkCount = port->getLinkCount();
    for (i = 0; i < linkCount; i++) {
      SchematicLink *link = port->getLink(i);
      if (link) {
        link->setHighlighted(value);
        link->update();
        m_highlightedLinks.push_back(link);
      }
    }
  }
}

//------------------------------------------------------------------

void StageSchematicScene::updateEditedGroups(
    const QMap<int, QList<SchematicNode *>> &editedGroup) {
  QMap<int, QList<SchematicNode *>>::const_iterator it;
  for (it = editedGroup.begin(); it != editedGroup.end(); it++) {
    int zValue                                            = 2;
    QMap<int, QList<SchematicNode *>>::const_iterator it2 = editedGroup.begin();
    while (it2 != editedGroup.end()) {
      StageSchematicNode *placedObj =
          dynamic_cast<StageSchematicNode *>(it2.value()[0]);
      StageSchematicNode *obj =
          dynamic_cast<StageSchematicNode *>(it.value()[0]);
      if (!placedObj || !obj) {
        it2++;
        continue;
      }
      int placedGroupedId = placedObj->getStageObject()->getEditingGroupId();
      if (obj->getStageObject()->isContainedInGroup(placedGroupedId) &&
          obj->getStageObject()->getEditingGroupId() != it2.key())
        zValue += 2;
      it2++;
    }
    StageSchematicGroupEditor *node =
        addEditedGroupedStageSchematicNode(it.key(), it.value());
    node->setZValue(zValue);
    node->setGroupedNodeZValue(zValue + 1);
  }
}

//------------------------------------------------------------------

void StageSchematicScene::updateNestedGroupEditors(StageSchematicNode *node,
                                                   const QPointF &newPos) {
  if (!node) return;
  QStack<int> groupIdStack = node->getStageObject()->getGroupIdStack();
  if (groupIdStack.isEmpty()) return;
  int i;
  QRectF rect;
  for (i = 0; i < groupIdStack.size(); i++) {
    if (m_groupEditorTable.contains(groupIdStack[i])) {
      QRectF app = m_groupEditorTable[groupIdStack[i]]->sceneBoundingRect();
      if (rect.isEmpty())
        rect = app;
      else
        rect = rect.united(app);
    }
  }
  node->setPos(newPos);
  for (i = 0; i < groupIdStack.size(); i++) {
    if (!m_groupEditorTable.contains(groupIdStack[i])) continue;
    rect =
        rect.united(m_groupEditorTable[groupIdStack[i]]->sceneBoundingRect());
    QRectF app = m_groupEditorTable[groupIdStack[i]]->boundingSceneRect();
    if (m_groupEditorTable[groupIdStack[i]]->scenePos() != app.topLeft())
      m_groupEditorTable[groupIdStack[i]]->setPos(app.topLeft());
  }
  update(rect);
}

//------------------------------------------------------------------

void StageSchematicScene::resizeNodes(bool maximizedNode) {
  resizingNodes             = true;
  m_gridDimension           = maximizedNode ? eLarge : eSmall;
  TStageObjectTree *pegTree = m_xshHandle->getXsheet()->getStageObjectTree();
  pegTree->setDagGridDimension(m_gridDimension);
  int i, objCount = pegTree->getStageObjectCount();
  for (i = 0; i < objCount; i++) {
    TStageObject *obj = pegTree->getStageObject(i);
    if (obj && obj->getId().isColumn()) {
      if (!m_nodeTable.contains(obj->getId())) continue;
      StageSchematicColumnNode *node =
          dynamic_cast<StageSchematicColumnNode *>(m_nodeTable[obj->getId()]);
      assert(node);
      node->resize(maximizedNode);
    }
    if (obj && !obj->getId().isCamera())
      updatePositionOnResize(obj, maximizedNode);
  }
  int splineCount = pegTree->getSplineCount();
  for (i = 0; i < splineCount; i++) {
    TStageObjectSpline *spl = pegTree->getSpline(i);
    if (spl) {
      if (!m_splineTable.contains(spl)) continue;
      StageSchematicSplineNode *node =
          dynamic_cast<StageSchematicSplineNode *>(m_splineTable[spl]);
      assert(node);
      node->resize(maximizedNode);
      updateSplinePositionOnResize(spl, maximizedNode);
    }
  }
  QMap<int, StageSchematicGroupNode *>::iterator it;
  for (it = m_GroupTable.begin(); it != m_GroupTable.end(); it++) {
    it.value()->resize(maximizedNode);
    QList<TStageObject *> groupedObjs = it.value()->getGroupedObjects();
    for (int i = 0; i < groupedObjs.size(); i++)
      updatePositionOnResize(groupedObjs[i], maximizedNode);
  }
  QMap<int, StageSchematicGroupEditor *>::iterator it2;
  for (it2 = m_groupEditorTable.begin(); it2 != m_groupEditorTable.end(); it2++)
    it2.value()->resizeNodes(maximizedNode);
  updateScene();
  resizingNodes = false;
}

//------------------------------------------------------------------

void StageSchematicScene::updatePositionOnResize(TStageObject *obj,
                                                 bool maximizedNode) {
  TPointD oldPos = obj->getDagNodePos();
  if (oldPos == TConst::nowhere) return;
  double oldPosY = oldPos.y - 25500;
  double newPosY = maximizedNode ? oldPosY * 2 : oldPosY * 0.5;
  obj->setDagNodePos(TPointD(oldPos.x, newPosY + 25500));
}

//------------------------------------------------------------------

void StageSchematicScene::updateSplinePositionOnResize(TStageObjectSpline *spl,
                                                       bool maximizedNode) {
  TPointD oldPos = spl->getDagNodePos();
  if (oldPos == TConst::nowhere) return;
  double oldPosY = oldPos.y - 25500;
  double newPosY = maximizedNode ? oldPosY * 2 : oldPosY * 0.5;
  spl->setDagNodePos(TPointD(oldPos.x, newPosY + 25500));
}

//------------------------------------------------------------------

StageSchematicSplineNode *StageSchematicScene::addSchematicSplineNode(
    TStageObjectSpline *spline) {
  StageSchematicSplineNode *node = new StageSchematicSplineNode(this, spline);
  connect(node, SIGNAL(sceneChanged()), this, SLOT(onSceneChanged()));
  connect(node, SIGNAL(xsheetChanged()), this, SLOT(onXsheetChanged()));
  if (!node) return 0;
  if (spline->getDagNodePos() == TConst::nowhere) {
    node->resize(m_gridDimension == 0);
    placeSplineNode(node);
  } else
    node->setPos(QPointF(spline->getDagNodePos().x, spline->getDagNodePos().y));
  return node;
}

//------------------------------------------------------------------
/*! create node according to the type of StageObject
 */
StageSchematicNode *StageSchematicScene::createStageSchematicNode(
    StageSchematicScene *scene, TStageObject *pegbar) {
  TStageObjectId id = pegbar->getId();
  if (id.isColumn()) {
    int columnIndex = id.getIndex();
    if (m_xshHandle->getXsheet()->isColumnEmpty(columnIndex)) {
      return 0;
    } else {
      TXshColumn *column = m_xshHandle->getXsheet()->getColumn(columnIndex);
      if (!column || column->getSoundColumn() || column->getSoundTextColumn())
        return 0;
    }
  }
  if (!pegbar || !scene) return 0;
  if (id.isTable())
    return new StageSchematicTableNode(scene, pegbar);
  else if (id.isCamera())
    return new StageSchematicCameraNode(scene, pegbar);
  else if (id.isPegbar())
    return new StageSchematicPegbarNode(scene, pegbar);
  else if (id.isColumn())
    return new StageSchematicColumnNode(scene, pegbar);
  else
    return 0;
}

//------------------------------------------------------------------

QGraphicsItem *StageSchematicScene::getCurrentNode() {
  QList<QGraphicsItem *> allItems = items();

  for (auto const item : allItems) {
    StageSchematicNode *node = dynamic_cast<StageSchematicNode *>(item);
    if (node && node->getStageObject()->getId() == m_objHandle->getObjectId())
      return node;
  }
  return 0;
}

//------------------------------------------------------------------

void StageSchematicScene::reorderScene() { placeNodes(); }

//------------------------------------------------------------------

void StageSchematicScene::placeNodes() {
  // search all possible roots
  std::vector<TreeStageNode *> roots;
  findRoots(roots);

  // sorts the roots container. Sortijg rules are specified by CompareNodes
  // class
  std::sort(roots.begin(), roots.end(), CompareNodes());

  double xFirstPos = m_firstPos.x - 500;
  double yFirstPos = m_firstPos.y + 500;
  double xPos      = xFirstPos;
  double yPos      = yFirstPos;
  double maxXPos;
  double maxYPos = yFirstPos;
  int step       = m_gridDimension == eLarge ? 100 : 50;

  // places the first roots and its children. The first root is always a table!
  assert(roots[0]->getNode()->getStageObject()->getId().isTable());
  roots[0]->getNode()->getStageObject()->setDagNodePos(
      TPointD(xFirstPos, yFirstPos));
  placeChildren(roots[0], xPos, yPos);
  maxXPos = xPos;

  int i;
  // places other roots and root's children.
  for (i = 1; i < (int)roots.size(); i++) {
    TStageObject *pegbar = roots[i]->getNode()->getStageObject();
    xPos                 = xFirstPos;
    yPos                 = maxYPos + (pegbar->getId().isCamera() ? 100 : step);
    pegbar->setDagNodePos(TPointD(xPos, yPos));
    placeChildren(roots[i], xPos, yPos);
    maxXPos = std::max(xPos, maxXPos);
    maxYPos = std::max(yPos, maxYPos);
  }

  // places all spline nodes.
  TStageObjectTree *pegTree = m_xshHandle->getXsheet()->getStageObjectTree();
  for (i = 0; i < pegTree->getSplineCount(); i++) {
    TStageObjectSpline *spline = pegTree->getSpline(i);
    spline->setDagNodePos(TPointD(maxXPos, yFirstPos + step));
    maxXPos += (m_showLetterOnPortFlag) ? 150 : 120;
  }

  // delete the tree
  for (i = 0; i < (int)roots.size(); i++) delete roots[i];
  roots.clear();

  updateScene();
}

//------------------------------------------------------------------

void StageSchematicScene::findRoots(std::vector<TreeStageNode *> &roots) {
  TStageObjectTree *pegTree = m_xshHandle->getXsheet()->getStageObjectTree();
  int i;
  for (i = 0; i < pegTree->getStageObjectCount(); i++) {
    TStageObject *pegbar = pegTree->getStageObject(i);
    // only cameras and pegbars can be roots
    // check if the id is contained in the table in case the object is grouped
    if ((pegbar->getId().isCamera() || pegbar->getId().isTable()) &&
        m_nodeTable.contains(pegbar->getId())) {
      StageSchematicNode *node = m_nodeTable[pegbar->getId()];
      TreeStageNode *treeNode  = new TreeStageNode(node);
      roots.push_back(treeNode);
      // when a root is found, the tree is build.
      makeTree(treeNode);
    }
  }
}

//------------------------------------------------------------------

void StageSchematicScene::makeTree(TreeStageNode *treeNode) {
  int i, portCount = treeNode->getNode()->getChildCount();
  for (i = 0; i < portCount; i++) {
    StageSchematicNodePort *port = treeNode->getNode()->getChildPort(i);
    int firstChild               = treeNode->getChildrenCount();
    int j, linkCount = port->getLinkCount();
    for (j = 0; j < linkCount; j++) {
      // the linked node is taken and the method is iterated for this node
      StageSchematicNode *schematicNode =
          dynamic_cast<StageSchematicNode *>(port->getLinkedNode(j));
      TreeStageNode *treeChild = new TreeStageNode(schematicNode);
      treeNode->addChild(treeChild);
      makeTree(treeChild);
    }
    int lastChild = treeNode->getChildrenCount();
    treeNode->sortChildren(firstChild, lastChild);
  }
}

//------------------------------------------------------------------

void StageSchematicScene::placeChildren(TreeStageNode *treeNode, double &xPos,
                                        double &yPos, bool isCameraTree) {
  int i;
  xPos += (m_showLetterOnPortFlag) ? 150 : 120;
  double xChildPos = xPos;
  double xRefPos   = xPos;
  bool firstChild  = true;
  bool startFromCamera =
      isCameraTree || treeNode->getNode()->getStageObject()->getId().isCamera();
  int step       = m_gridDimension == eLarge ? 100 : 50;
  double yOffset = startFromCamera ? step : -step;
  if (startFromCamera) treeNode->reverseChildren();
  for (i = 0; i < treeNode->getChildrenCount(); i++) {
    TreeStageNode *childNode  = treeNode->getChildTreeNode(i);
    TStageObject *childPegbar = childNode->getNode()->getStageObject();
    if (childPegbar->getId().isCamera()) continue;
    xChildPos = xRefPos;
    // first child must have same yPos of father.
    yPos += firstChild ? 0 : yOffset;
    firstChild = false;
    childPegbar->setDagNodePos(TPointD(xChildPos, yPos));
    placeChildren(childNode, xChildPos, yPos, startFromCamera);
    xPos = std::max(xPos, xChildPos);
  }
}

//------------------------------------------------------------------
/*! define the position of nodes which is not specified manually
 */
void StageSchematicScene::placeNode(StageSchematicNode *node) {
  double xFirstPos = m_firstPos.x - 500;
  double yFirstPos = m_firstPos.y + 500;
  double xPos      = xFirstPos;
  double yPos      = yFirstPos;
  int step         = m_gridDimension == eLarge ? 100 : 50;
  int hStep        = (m_showLetterOnPortFlag) ? 150 : 120;

  TStageObjectTree *pegTree = m_xshHandle->getXsheet()->getStageObjectTree();
  QRectF nodeRect           = node->boundingRect();
  TStageObject *pegbar      = node->getStageObject();

  TStageObjectId parentObjId = pegbar->getParent();
  TStageObject *parentObj    = pegTree->getStageObject(parentObjId, false);

  if (pegbar->getId().isCamera())
    yPos = yFirstPos + step;
  else if (pegbar->getId().isPegbar()) {
    if (parentObj) {
      if (parentObj->getDagNodePos() != TConst::nowhere) {
        TPointD pos = parentObj->getDagNodePos();
        yPos        = pos.y;
        xPos        = pos.x + hStep;
      } else {
        m_nodesToPlace[parentObjId].append(node);
        return;
      }

    } else {
      yPos = yFirstPos;
      xPos = xFirstPos + hStep;
    }
  } else if (pegbar->getId().isColumn()) {
    if (parentObj) {
      if (parentObj->getDagNodePos() != TConst::nowhere) {
        TPointD pos = parentObj->getDagNodePos();
        yPos        = pos.y;
        xPos        = pos.x + hStep;
      } else {
        m_nodesToPlace[parentObjId].append(node);
        return;
      }
    } else {
      yPos = yFirstPos;
      xPos = xFirstPos + (hStep * 2);
    }
  }
  QPointF initPos(xPos, yPos);
  nodeRect.moveTopLeft(initPos);

  bool found = false;
  // try to place inside of the visible area
  if (!views().isEmpty() && pegbar->getId().isPegbar()) {
    QGraphicsView *view = views().at(0);
    QRectF visibleRect =
        view->mapToScene(0, 0, view->width(), view->height()).boundingRect();
    while (visibleRect.left() > nodeRect.left()) {
      nodeRect.translate(hStep, 0);
      xPos += hStep;
    }
    while (visibleRect.bottom() < nodeRect.bottom()) {
      nodeRect.translate(0, -step);
      yPos -= step;
    }
    QPointF tmpPos(xPos, yPos);
    while (visibleRect.right() > nodeRect.right()) {
      while (!isAnEmptyZone(nodeRect)) {
        nodeRect.translate(0, -step);
        yPos -= step;
      }
      // if failed to place this column, then move the candidate position to
      // right
      if (visibleRect.contains(nodeRect)) {
        found = true;
        break;
      } else {
        xPos += hStep;
        yPos = tmpPos.y();
        nodeRect.moveTopLeft(QPointF(xPos, yPos));
      }
    }
  }
  // place the node outside of the visible area
  if (!found) {
    nodeRect.moveTopLeft(initPos);
    xPos = initPos.x();
    yPos = initPos.y();
    nodeRect.moveTopLeft(initPos);

    while (!isAnEmptyZone(nodeRect)) {
      if (pegbar->getId().isCamera()) {
        nodeRect.translate(0, step);
        yPos += step;
      } else if (pegbar->getId().isColumn() || pegbar->getId().isPegbar()) {
        nodeRect.translate(0, -step);
        yPos -= step;
      }
    }
  }
  pegbar->setDagNodePos(TPointD(xPos, yPos));
  node->setPos(xPos, yPos);
  if (m_nodesToPlace.contains(pegbar->getId())) {
    QList<StageSchematicNode *> nodes = m_nodesToPlace[pegbar->getId()];
    int i;
    for (i = 0; i < nodes.size(); i++) placeNode(nodes[i]);
  }
}

//------------------------------------------------------------------

void StageSchematicScene::placeSplineNode(
    StageSchematicSplineNode *splineNode) {
  double xFirstPos           = m_firstPos.x - 500;
  double yFirstPos           = m_firstPos.y + 500;
  int hStep                  = (m_showLetterOnPortFlag) ? 150 : 120;
  double xPos                = xFirstPos + (hStep * 2);
  int step                   = m_gridDimension == eLarge ? 100 : 50;
  double yPos                = yFirstPos + step;
  QRectF nodeRect            = splineNode->boundingRect();
  TStageObjectSpline *spline = splineNode->getSpline();
  nodeRect.translate(QPointF(xPos, yPos));

  while (!isAnEmptyZone(nodeRect)) {
    nodeRect.translate(hStep, 0);
    xPos += hStep;
  }
  spline->setDagNodePos(TPointD(xPos, yPos));
  splineNode->setPos(xPos, yPos);
}

//------------------------------------------------------------------

void StageSchematicScene::onPegbarAdded() {
  // if this command called from the context menu, place the node at the
  // cllicked position
  // clickedPos will be null(0,0) if it called from the toolbar
  QPointF clickedPos = qobject_cast<QAction *>(sender())->data().toPointF();

  TStageObjectCmd::addNewPegbar(m_xshHandle, m_objHandle, clickedPos);
}

//------------------------------------------------------------------

void StageSchematicScene::onSplineAdded() {
  // if this command called from the context menu, place the node at the
  // cllicked position
  // clickedPos will be null(0,0) if it called from the toolbar
  QPointF clickedPos = qobject_cast<QAction *>(sender())->data().toPointF();

  TStageObjectCmd::addNewSpline(m_xshHandle, m_objHandle, m_colHandle,
                                clickedPos);
}

//------------------------------------------------------------------

void StageSchematicScene::onCameraAdded() {
  // if this command called from the context menu, place the node at the
  // cllicked position
  // clickedPos will be null(0,0) if it called from the toolbar
  QPointF clickedPos = qobject_cast<QAction *>(sender())->data().toPointF();

  TStageObjectCmd::addNewCamera(m_xshHandle, m_objHandle, clickedPos);
}

//------------------------------------------------------------------

void StageSchematicScene::onSwitchPortModeToggled(bool withLetter) {
  m_showLetterOnPortFlag            = withLetter;
  ShowLetterOnOutputPortOfStageNode = (withLetter) ? 1 : 0;
  updateScene();
}

//------------------------------------------------------------------

void StageSchematicScene::onRigViewToggled(bool enabled) {
  m_rigViewEnabled = enabled;
  // The values of the published channels follow the current frame
  if (m_frameHandle) {
    if (enabled)
      connect(m_frameHandle, SIGNAL(frameSwitched()), this,
              SLOT(onRigFrameSwitched()), Qt::UniqueConnection);
    else
      disconnect(m_frameHandle, SIGNAL(frameSwitched()), this,
                 SLOT(onRigFrameSwitched()));
  }
  // updateScene() rebuilds the nodes, with or without the Rig view items
  if (m_xshHandle) updateScene();
}

//------------------------------------------------------------------

void StageSchematicScene::onRigFilterChanged(const QString &text) {
  m_rigFilter = text;
  if (m_rigViewEnabled && m_xshHandle) applyRigFilter();
}

//------------------------------------------------------------------

void StageSchematicScene::onRigPortFilterChanged(const QString &text) {
  m_rigPortFilter = text;
  if (m_rigViewEnabled && m_xshHandle) applyRigPortFilter();
}

//------------------------------------------------------------------

void StageSchematicScene::onRigFrameSwitched() {
  if (m_rigViewEnabled) refreshRigPorts();
}

//------------------------------------------------------------------

void StageSchematicScene::refreshRigPorts() {
  QMap<TStageObjectId, StageSchematicNode *>::iterator it;
  for (it = m_nodeTable.begin(); it != m_nodeTable.end(); ++it)
    for (QGraphicsItem *child : it.value()->childItems())
      if (RigBadgeItem *item = dynamic_cast<RigBadgeItem *>(child))
        item->refresh();
}

//------------------------------------------------------------------

void StageSchematicScene::applyRigView() {
  TXsheet *xsh = getXsheet();
  if (!xsh) return;
  TStageObjectTree *pegTree = xsh->getStageObjectTree();
  m_rigSearchText.clear();

  // The master controllers are the pegbars named "ctrl_*" (see the Master
  // Controllers panel). Their value is referred to as "peg<N>.rot".
  struct Controller {
    QString m_name;
    std::string m_ref;
  };
  std::vector<Controller> controllers;
  for (int i = 0; i < pegTree->getStageObjectCount(); i++) {
    TStageObject *obj = pegTree->getStageObject(i);
    if (!obj || !obj->getId().isPegbar()) continue;
    QString name = QString::fromStdString(obj->getName());
    if (!name.startsWith("ctrl_", Qt::CaseInsensitive)) continue;
    Controller controller;
    controller.m_name = name;
    controller.m_ref =
        "peg" + std::to_string(obj->getId().getIndex() + 1) + ".rot";
    controllers.push_back(controller);
  }

  const QColor deformerColor(140, 80, 190, 210);
  const QColor offsetColor(40, 140, 140, 210);
  const QColor controllerColor(190, 120, 30, 210);
  const QColor portColor(50, 100, 170, 220);

  QMap<TStageObjectId, StageSchematicNode *>::iterator it;
  for (it = m_nodeTable.begin(); it != m_nodeTable.end(); ++it) {
    StageSchematicNode *node = it.value();
    if (!node) continue;
    TStageObject *obj   = node->getStageObject();
    TStageObjectId id   = obj->getId();
    QStringList &search = m_rigSearchText[it.key()];

    // The badges and the port rows are stacked under the node
    qreal y = node->boundingRect().bottom() + 1;

    // One badge per kind, for the column nodes. The badge can be clicked: it
    // opens the editor of what it lists.
    // The names listed are what the filter looks for.
    auto addBadge = [&](const QString &title, const QStringList &entries,
                        const QStringList &names, const QColor &color,
                        const std::function<void()> &open) {
      if (entries.isEmpty()) return;
      QString full        = title + ": " + entries.join(", ");
      RigBadgeItem *badge = new RigBadgeItem(node, full, full, color);
      badge->setClickHandlers(open);
      badge->setPos(0, y);
      y += badge->boundingRect().height() + 1;
      search.append(names);
    };

    if (id.isColumn()) {
      TXshColumn *column = xsh->getColumn(id.getIndex());

      // Deformer fxs of the column's fx chain
      std::vector<TFx *> deformerFxs = getColumnDeformers(xsh, column);
      QStringList deformers;
      std::vector<TFxP> deformerPtrs;
      for (TFx *fx : deformerFxs) {
        deformers.append(getDeformerLabel(fx));
        deformerPtrs.push_back(TFxP(fx));
      }
      addBadge(tr("Deformers"), deformers, deformers, deformerColor,
               [this, deformerPtrs]() { openRigDeformer(deformerPtrs); });

      // Offset layers of the column
      QStringList layers, layerNames;
      for (int i = 0; i < obj->getOffsetLayerCount(); i++) {
        const TStageObject::OffsetLayer &layer = obj->getOffsetLayer(i);
        QString name = QString::fromStdWString(layer.m_name);
        layerNames.append(name);
        layers.append(layer.m_enabled ? name : name + tr(" (off)"));
      }
      addBadge(tr("Offsets"), layers, layerNames, offsetColor,
               [this, id]() { openRigPanel(id, "MI_OpenOffsetLayers"); });

      // Master controllers driving a channel or an offset layer weight
      QStringList bound;
      for (const Controller &controller : controllers) {
        bool found = false;
        for (int ch = 0; ch < TStageObject::T_ChannelCount && !found; ch++) {
          if (!TStageObject::isOffsetChannel((TStageObject::Channel)ch))
            continue;
          found = paramRefersTo(obj->getParam((TStageObject::Channel)ch),
                                controller.m_ref);
        }
        for (int i = 0; i < obj->getOffsetLayerCount() && !found; i++)
          found = paramRefersTo(obj->getOffsetLayer(i).m_weight.getPointer(),
                                controller.m_ref);
        if (found) bound.append(controller.m_name);
      }
      addBadge(tr("Controllers"), bound, bound, controllerColor,
               [this, id]() { openRigPanel(id, "MI_OpenMasterControllers"); });
    }

    // The published channels of any stage object, one row each: its name and
    // its value at the current frame. A click selects the channel in the
    // Function Editor, a double click edits the value.
    for (int i = 0; i < obj->getPublishedChannelCount(); i++) {
      const TStageObject::PublishedChannel &port = obj->getPublishedChannel(i);
      QString name = QString::fromStdWString(port.m_name);
      QString channelName;
      if (port.isLayerWeight())
        channelName =
            QString::fromStdWString(port.m_layer) + "/" + tr("Weight");
      else
        for (const auto &info : rigChannels)
          if (info.m_channel == port.m_channel) channelName = tr(info.m_label);
      QString toolTip = UiTranslations::text(tr("%1 (%2): click to select the channel in the "
                           "Function Editor, double click to set its value "
                           "at the current frame"))
                            .arg(name)
                            .arg(channelName);
      RigBadgeItem *row = new RigBadgeItem(node, getPublishedChannelText(id, i),
                                           toolTip, portColor);
      row->setTextFunction(
          [this, id, i]() { return getPublishedChannelText(id, i); });
      row->setClickHandlers([this, id, i]() { selectPublishedChannel(id, i); },
                            [this, id, i]() { editPublishedChannel(id, i); });
      row->setPortName(name);
      row->setPos(0, y);
      y += row->boundingRect().height() + 1;
      search.append(name);
    }
  }

  applyRigFilter();
  applyRigPortFilter();
}

//------------------------------------------------------------------

void StageSchematicScene::applyRigPortFilter() {
  QString filter = m_rigPortFilter.trimmed();
  QMap<TStageObjectId, StageSchematicNode *>::iterator it;
  for (it = m_nodeTable.begin(); it != m_nodeTable.end(); ++it) {
    StageSchematicNode *node = it.value();
    if (!node) continue;
    // The badges and rows of the node are stacked again from the place of the
    // first one (which never moves), in the order they were made, so that the
    // hidden rows leave no gap. The badges are never hidden.
    bool first = true;
    qreal y    = 0;
    for (QGraphicsItem *child : node->childItems()) {
      RigBadgeItem *item = dynamic_cast<RigBadgeItem *>(child);
      if (!item) continue;
      if (first) {
        y     = item->pos().y();
        first = false;
      }
      bool shown = !item->isPortRow() || filter.isEmpty() ||
                   item->getPortName().contains(filter, Qt::CaseInsensitive);
      item->setVisible(shown);
      if (!shown) continue;
      item->setPos(item->pos().x(), y);
      y += item->boundingRect().height() + 1;
    }
  }
}

//------------------------------------------------------------------

void StageSchematicScene::applyRigFilter() {
  // Start from all the nodes and links visible
  QList<StageSchematicNode *> nodes = m_nodeTable.values();
  for (StageSchematicGroupNode *groupNode : m_GroupTable)
    if (groupNode) nodes.append(groupNode);
  QList<QGraphicsItem *> links;
  for (QGraphicsItem *item : items())
    if (dynamic_cast<SchematicLink *>(item)) links.append(item);

  for (StageSchematicNode *node : nodes) node->setVisible(true);
  for (QGraphicsItem *link : links) link->setVisible(true);

  QString filter = m_rigFilter.trimmed();
  if (filter.isEmpty()) return;

  // The nodes having a badge or a published channel that matches
  QSet<SchematicNode *> matching;
  QMap<TStageObjectId, StageSchematicNode *>::iterator it;
  for (it = m_nodeTable.begin(); it != m_nodeTable.end(); ++it)
    for (const QString &text : m_rigSearchText.value(it.key()))
      if (text.contains(filter, Qt::CaseInsensitive)) {
        matching.insert(it.value());
        break;
      }

  QSet<SchematicNode *> hidden;
  for (StageSchematicNode *node : nodes) {
    if (node->getName().contains(filter, Qt::CaseInsensitive) ||
        matching.contains(node))
      continue;
    node->setSelected(false);
    node->setVisible(false);
    hidden.insert(node);
  }
  // A link is hidden with any of its two nodes
  for (QGraphicsItem *item : links) {
    SchematicLink *link  = static_cast<SchematicLink *>(item);
    SchematicPort *start = link->getStartPort();
    SchematicPort *end   = link->getEndPort();
    if ((start && hidden.contains(start->getNode())) ||
        (end && hidden.contains(end->getNode())))
      item->setVisible(false);
  }
}

//------------------------------------------------------------------

QString StageSchematicScene::getPublishedChannelText(const TStageObjectId &id,
                                                     int index) {
  TXsheet *xsh = m_xshHandle ? m_xshHandle->getXsheet() : 0;
  TStageObject *obj =
      xsh ? xsh->getStageObjectTree()->getStageObject(id, false) : 0;
  if (!obj || index < 0 || index >= obj->getPublishedChannelCount())
    return QString();

  QString text =
      QString::fromStdWString(obj->getPublishedChannel(index).m_name);
  TDoubleParam *param = obj->getPublishedParam(index);
  if (!param) return text + ": -";

  int frame = (m_frameHandle &&
               m_frameHandle->getFrameType() == TFrameHandle::SceneFrame)
                  ? m_frameHandle->getFrame()
                  : 0;
  QString unitName;
  double value = getDisplayValue(param, frame, &unitName);
  text += ": " + QString::number(value, 'f', 2);
  if (!unitName.isEmpty()) text += " " + unitName;
  return text;
}

//------------------------------------------------------------------

void StageSchematicScene::selectPublishedChannel(const TStageObjectId &id,
                                                 int index) {
  TXsheet *xsh = m_xshHandle ? m_xshHandle->getXsheet() : 0;
  TStageObject *obj =
      xsh ? xsh->getStageObjectTree()->getStageObject(id, false) : 0;
  TDoubleParam *param = obj ? obj->getPublishedParam(index) : 0;
  if (!param) return;

  // The Function Editor is opened if there is none shown
  FunctionViewer *viewer = findVisibleFunctionViewer();
  if (!viewer) {
    QAction *open =
        CommandManager::instance()->getAction("MI_OpenFunctionEditor");
    if (open) open->trigger();
    viewer = findVisibleFunctionViewer();
  }
  if (viewer) viewer->selectStageObjectCurve(param);
}

//------------------------------------------------------------------

void StageSchematicScene::editPublishedChannel(const TStageObjectId &id,
                                               int index) {
  TXsheet *xsh = m_xshHandle ? m_xshHandle->getXsheet() : 0;
  TStageObject *obj =
      xsh ? xsh->getStageObjectTree()->getStageObject(id, false) : 0;
  TDoubleParam *param = obj ? obj->getPublishedParam(index) : 0;
  if (!param || !m_frameHandle ||
      m_frameHandle->getFrameType() != TFrameHandle::SceneFrame)
    return;

  int frame                           = m_frameHandle->getFrame();
  TStageObject::PublishedChannel port = obj->getPublishedChannel(index);
  QString name                        = QString::fromStdWString(port.m_name);
  QString unitName;
  double value = getDisplayValue(param, frame, &unitName);

  bool ok         = false;
  QWidget *parent = views().isEmpty() ? 0 : views()[0];
  double newValue = QInputDialog::getDouble(
      parent, tr("Published Channel"),
      UiTranslations::text(tr("%1 at frame %2%3:"))
          .arg(name)
          .arg(frame + 1)
          .arg(unitName.isEmpty() ? QString() : " (" + unitName + ")"),
      value, -1000000.0, 1000000.0, 3, &ok);
  if (!ok) return;

  // Back to the internal units. The scene may have changed while the dialog
  // was open, so the channel is looked for again.
  xsh = m_xshHandle->getXsheet();
  obj = xsh ? xsh->getStageObjectTree()->getStageObject(id, false) : 0;
  if (!obj || index >= obj->getPublishedChannelCount() ||
      obj->getPublishedParam(index) != param)
    return;
  TMeasure *measure = param->getMeasure();
  const TUnit *unit = measure ? measure->getCurrentUnit() : 0;
  if (unit) newValue = unit->convertFrom(newValue);

  if (port.isLayerWeight())
    // The weight of a layer is keyed only if it is animated, as in the
    // Offset Layers panel
    TStageObjectCmd::setOffsetLayerWeight(
        id, obj->findOffsetLayer(port.m_layer), newValue, frame, m_xshHandle);
  else {
    KeyframeSetter::setValue(param, frame, newValue);
    m_xshHandle->notifyXsheetChanged();
  }
}

//------------------------------------------------------------------

void StageSchematicScene::addRigMenuActions(QMenu &menu,
                                            const TStageObjectId &id) {
  if (!m_rigViewEnabled) return;
  TXsheet *xsh = m_xshHandle ? m_xshHandle->getXsheet() : 0;
  TStageObject *obj =
      xsh ? xsh->getStageObjectTree()->getStageObject(id, false) : 0;
  if (!obj) return;

  // The dialogs are opened once the menu is closed
  menu.addSeparator();
  QAction *publish = menu.addAction(tr("Publish Channel..."));
  connect(publish, &QAction::triggered, this, [this, id]() {
    QTimer::singleShot(0, this, [this, id]() { publishChannelDialog(id); });
  });
  if (obj->hasPublishedChannels()) {
    QAction *unpublish = menu.addAction(tr("Unpublish Channel..."));
    connect(unpublish, &QAction::triggered, this, [this, id]() {
      QTimer::singleShot(0, this, [this, id]() { unpublishChannelDialog(id); });
    });
  }
}

//------------------------------------------------------------------

void StageSchematicScene::publishChannelDialog(const TStageObjectId &id) {
  TXsheet *xsh = m_xshHandle ? m_xshHandle->getXsheet() : 0;
  TStageObject *obj =
      xsh ? xsh->getStageObjectTree()->getStageObject(id, false) : 0;
  if (!obj) return;

  QDialog dialog(views().isEmpty() ? 0 : views()[0]);
  dialog.setWindowTitle(tr("Publish Channel"));
  QFormLayout *layout = new QFormLayout(&dialog);

  // The animatable channels of the object, and the weights of its offset
  // layers. The user data is the channel (T_ChannelCount for a weight) and
  // the name of the layer.
  QComboBox *channelCombo = new QComboBox(&dialog);
  for (const auto &info : rigChannels) {
    // Cameras ignore the 3D tilt channels
    if (id.isCamera() && (info.m_channel == TStageObject::T_RotX ||
                          info.m_channel == TStageObject::T_RotY))
      continue;
    channelCombo->addItem(tr(info.m_label), (int)info.m_channel);
  }
  for (int i = 0; i < obj->getOffsetLayerCount(); i++) {
    QString layerName = QString::fromStdWString(obj->getOffsetLayer(i).m_name);
    channelCombo->addItem(layerName + "/" + tr("Weight"),
                          (int)TStageObject::T_ChannelCount);
    channelCombo->setItemData(channelCombo->count() - 1, layerName,
                              Qt::UserRole + 1);
  }
  layout->addRow(tr("Channel:"), channelCombo);

  // The name proposed is the one of the channel, until the user edits it
  QLineEdit *nameEdit  = new QLineEdit(channelCombo->currentText(), &dialog);
  QString proposedName = channelCombo->currentText();
  connect(channelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          &dialog, [&](int) {
            if (nameEdit->text() == proposedName) {
              proposedName = channelCombo->currentText();
              nameEdit->setText(proposedName);
            }
          });
  layout->addRow(tr("Name:"), nameEdit);

  QDialogButtonBox *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addRow(buttons);

  if (dialog.exec() != QDialog::Accepted) return;

  int index = TStageObjectCmd::publishChannel(
      id, nameEdit->text().trimmed().toStdWString(),
      channelCombo->currentData().toInt(),
      channelCombo->currentData(Qt::UserRole + 1).toString().toStdWString(),
      m_xshHandle);
  if (index < 0)
    DVGui::warning(
        tr("The channel was not published: the name is empty or "
           "already used by another published channel."));
}

//------------------------------------------------------------------

void StageSchematicScene::unpublishChannelDialog(const TStageObjectId &id) {
  TXsheet *xsh = m_xshHandle ? m_xshHandle->getXsheet() : 0;
  TStageObject *obj =
      xsh ? xsh->getStageObjectTree()->getStageObject(id, false) : 0;
  if (!obj || !obj->hasPublishedChannels()) return;

  QStringList names;
  for (int i = 0; i < obj->getPublishedChannelCount(); i++)
    names.append(QString::fromStdWString(obj->getPublishedChannel(i).m_name));

  bool ok         = false;
  QWidget *parent = views().isEmpty() ? 0 : views()[0];
  QString chosen  = QInputDialog::getItem(parent, tr("Unpublish Channel"),
                                          tr("Channel:"), names, 0, false, &ok);
  if (!ok) return;
  TStageObjectCmd::unpublishChannel(id, names.indexOf(chosen), m_xshHandle);
}

//------------------------------------------------------------------

void StageSchematicScene::openRigDeformer(const std::vector<TFxP> &fxs) {
  if (!m_fxHandle || fxs.empty()) return;

  TFx *fx = fxs[0].getPointer();
  if (fxs.size() > 1) {
    QMenu menu(views().isEmpty() ? 0 : views()[0]);
    for (size_t i = 0; i < fxs.size(); i++) {
      QAction *action =
          menu.addAction(getDeformerLabel(fxs[i].getPointer()) + " (" +
                         QString::fromStdWString(fxs[i]->getFxId()) + ")");
      action->setData((int)i);
    }
    QAction *chosen = menu.exec(QCursor::pos());
    if (!chosen) return;
    fx = fxs[chosen->data().toInt()].getPointer();
  }

  m_fxHandle->setFx(fx);
  QAction *fxSettings =
      CommandManager::instance()->getAction("MI_FxParamEditor");
  if (fxSettings) fxSettings->trigger();
}

//------------------------------------------------------------------

void StageSchematicScene::openRigPanel(const TStageObjectId &id,
                                       const char *commandId) {
  // The panel of the offset layers shows the layers of the current object
  if (m_objHandle && m_objHandle->getObjectId() != id) {
    onCurrentObjectChanged(id, false);
    if (id.isColumn()) onCurrentColumnChanged(id.getIndex());
  }
  QAction *action = CommandManager::instance()->getAction(commandId);
  if (action) action->trigger();
}

//------------------------------------------------------------------

void StageSchematicScene::onResetCenter() {
  TStageObjectId id = m_objHandle->getObjectId();
  TStageObjectCmd::resetCenterAndOffset(id, m_xshHandle);
}

//------------------------------------------------------------------

void StageSchematicScene::onCameraActivate() {
  TStageObjectCmd::setAsActiveCamera(m_xshHandle, m_objHandle);
}

//------------------------------------------------------------------

void StageSchematicScene::onRemoveSpline() { m_selection->deleteSelection(); }

//------------------------------------------------------------------

void StageSchematicScene::onSaveSpline() {
  TFilePath projectFolder =
      m_sceneHandle->getScene()->getProject()->getProjectFolder();
  QString fileNameStr = QFileDialog::getSaveFileName(
      this->views()[0], QObject::tr("Save Motion Path"),
      QString::fromStdWString(projectFolder.getWideString()),
      QObject::tr("Motion Path files (*.mpath)"));
  if (fileNameStr == "") return;
  TFilePath fp(fileNameStr.toStdWString());
  if (fp.getType() == "") fp = fp.withType("mpath");
  try {
    assert(m_objHandle->isSpline());
    TStageObjectId id        = m_objHandle->getObjectId();
    TXsheet *xsh             = m_xshHandle->getXsheet();
    TStageObject *currentObj = xsh->getStageObjectTree()->getStageObject(id);

    if (currentObj == 0) throw "no currentObj";
    TStageObjectSpline *spline = currentObj->getSpline();
    if (spline == 0) throw "no spline";
    TOStream os(fp);

    // Only points are saved
    const TStroke *stroke = spline->getStroke();
    int n                 = stroke ? stroke->getControlPointCount() : 0;
    for (int i = 0; i < n; i++) {
      TThickPoint p = stroke->getControlPoint(i);
      os << p.x << p.y << p.thick;
    }
  } catch (...) {
    DVGui::warning(QObject::tr("It is not possible to save the motion path."));
  }
}

//------------------------------------------------------------------

void StageSchematicScene::onLoadSpline() {
  TFilePath projectFolder =
      m_sceneHandle->getScene()->getProject()->getProjectFolder();
  QString fileNameStr = QFileDialog::getOpenFileName(
      this->views()[0], QObject::tr("Load Motion Path"),
      QString::fromStdWString(projectFolder.getWideString()),
      QObject::tr("Motion Path files (*.mpath)"));
  if (fileNameStr == "") return;
  TFilePath fp(fileNameStr.toStdWString());
  if (fp.getType() == "") fp = fp.withType("mpath");
  try {
    if (!TFileStatus(fp).doesExist()) {
      QString msg;
      msg = "Motion path " + toQString(fp) + " doesn't exists.";
      DVGui::info(msg);
      return;
    }
    assert(m_objHandle->isSpline());
    // ObjectID of the parent node to which the spline is connected
    TStageObjectId id = m_objHandle->getObjectId();
    TXsheet *xsh      = m_xshHandle->getXsheet();

    TStageObjectSpline *spline =
        xsh->getStageObjectTree()->getStageObject(id, false)->getSpline();
    if (!spline) return;

    TIStream is(fp);
    if (is) {
      spline->loadData(is);
      m_objHandle->setSplineObject(spline);
      m_objHandle->commitSplineChanges();
      IconGenerator::instance()->invalidate(spline);
    }
  } catch (...) {
    DVGui::warning(QObject::tr("It is not possible to load the motion path."));
  }
}

//--------------------------------------------------------

void StageSchematicScene::onPathToggled(int state) {
  TStageObjectId objId = m_objHandle->getObjectId();
  TStageObject *obj    = m_xshHandle->getXsheet()->getStageObject(objId);
  TStageObjectCmd::enableSplineAim(obj, state, m_xshHandle);
  onSceneChanged();
  update();
}

//--------------------------------------------------------

void StageSchematicScene::onCpToggled(bool toggled) {
  TStageObjectId objId = m_objHandle->getObjectId();
  TStageObject *obj    = m_xshHandle->getXsheet()->getStageObject(objId);
  TStageObjectCmd::enableSplineUppk(obj, toggled, m_xshHandle);
  update();
}

//------------------------------------------------------------------

TXsheet *StageSchematicScene::getXsheet() { return m_xshHandle->getXsheet(); }

//------------------------------------------------------------------

void StageSchematicScene::onXsheetChanged() {
  m_xshHandle->notifyXsheetChanged();
}

//------------------------------------------------------------------

void StageSchematicScene::onSceneChanged() {
  m_sceneHandle->notifySceneChanged();
}

//------------------------------------------------------------------

void StageSchematicScene::onCurrentObjectChanged(const TStageObjectId &id,
                                                 bool isSpline) {
  // revert the pre-selected node to normal text color
  QMap<TStageObjectId, StageSchematicNode *>::iterator it;
  it = m_nodeTable.find(getCurrentObject());
  if (it != m_nodeTable.end()) {
    it.value()->update();
  }

  m_objHandle->setObjectId(id);
  if (m_frameHandle->isEditingLevel()) return;
  m_objHandle->setIsSpline(isSpline);
}

//------------------------------------------------------------------

void StageSchematicScene::onCurrentColumnChanged(int index) {
  m_colHandle->setColumnIndex(index);
}

//------------------------------------------------------------------

void StageSchematicScene::contextMenuEvent(
    QGraphicsSceneContextMenuEvent *cme) {
  QPointF scenePos                = cme->scenePos();
  QList<QGraphicsItem *> itemList = items(scenePos);
  if (!itemList.isEmpty()) {
    SchematicScene::contextMenuEvent(cme);
    return;
  }

  QMenu menu(views()[0]);

  // AddPegbar
  QAction *addPegbar = new QAction(tr("&New Pegbar"), &menu);
  connect(addPegbar, SIGNAL(triggered()), this, SLOT(onPegbarAdded()));

  // AddSpline
  QAction *addSpline = new QAction(tr("&New Motion Path"), &menu);
  connect(addSpline, SIGNAL(triggered()), this, SLOT(onSplineAdded()));

  // AddCamera
  QAction *addCamera = new QAction(tr("&New Camera"), &menu);
  connect(addCamera, SIGNAL(triggered()), this, SLOT(onCameraAdded()));

  QAction *paste = CommandManager::instance()->getAction("MI_Paste");

  // this is to place the node at the clicked position
  addPegbar->setData(cme->scenePos());
  addSpline->setData(cme->scenePos());
  addCamera->setData(cme->scenePos());

  menu.addAction(addPegbar);
  menu.addAction(addCamera);
  menu.addAction(addSpline);

  // Close sub xsheet and move to parent sheet
  ToonzScene *scene      = m_sceneHandle->getScene();
  ChildStack *childStack = scene->getChildStack();
  if (childStack && childStack->getAncestorCount() > 0) {
    menu.addSeparator();
    menu.addAction(CommandManager::instance()->getAction("MI_CloseChild"));
  }

  menu.addSeparator();
  menu.addAction(paste);
  m_selection->setPastePosition(TPointD(scenePos.x(), scenePos.y()));
  menu.exec(cme->screenPos());
}

//------------------------------------------------------------------

void StageSchematicScene::mousePressEvent(QGraphicsSceneMouseEvent *me) {
  QList<QGraphicsItem *> items = selectedItems();
  SchematicScene::mousePressEvent(me);
  if (me->button() == Qt::MiddleButton) {
    int i;
    for (i = 0; i < items.size(); i++) items[i]->setSelected(true);
  }
}

//------------------------------------------------------------------

SchematicNode *StageSchematicScene::getNodeFromPosition(const QPointF &pos) {
  QList<QGraphicsItem *> pickedItems = items(pos);
  for (int i = 0; i < pickedItems.size(); i++) {
    SchematicNode *node = dynamic_cast<SchematicNode *>(pickedItems.at(i));
    if (node) return node;
  }
  return 0;
}

//------------------------------------------------------------------

void StageSchematicScene::onSelectionChanged() {
  m_selection->selectNone();
  int i, size = m_highlightedLinks.size();
  for (i = 0; i < size; i++) {
    SchematicLink *link = m_highlightedLinks[i];
    link->setHighlighted(false);
    link->update();
  }
  m_highlightedLinks.clear();
  QList<QGraphicsItem *> slcItems = selectedItems();
  QList<QGraphicsItem *>::iterator it;
  for (it = slcItems.begin(); it != slcItems.end(); it++) {
    StageSchematicGroupNode *groupNode =
        dynamic_cast<StageSchematicGroupNode *>(*it);
    StageSchematicNode *node = dynamic_cast<StageSchematicNode *>(*it);
    if (groupNode) {
      QList<TStageObject *> objs = groupNode->getGroupedObjects();
      for (i = 0; i < objs.size(); i++) m_selection->select(objs[i]->getId());
      highlightLinks(groupNode, true);
    } else if (node) {
      m_selection->select(node->getStageObject()->getId());
      highlightLinks(node, true);
    }
    StageSchematicSplineNode *spline =
        dynamic_cast<StageSchematicSplineNode *>(*it);
    if (spline) {
      m_selection->select(spline->getSpline()->getId());
      StageSchematicSplinePort *port = spline->getParentPort();
      if (port) {
        int i, linkCount = port->getLinkCount();
        for (i = 0; i < linkCount; i++) {
          SchematicLink *link = port->getLink(i);
          link->setHighlighted(true);
          m_highlightedLinks.push_back(link);
        }
      }
    }

    SchematicLink *link = dynamic_cast<SchematicLink *>(*it);
    if (link) m_selection->select(link);
  }
  if (!m_selection->isEmpty()) m_selection->makeCurrent();
}

//------------------------------------------------------------------

void StageSchematicScene::onCollapse(QList<TStageObjectId> objects) {
  emit doCollapse(objects);
}

//------------------------------------------------------------------

void StageSchematicScene::onEditGroup() {
  if (m_selection->isEmpty()) return;
  TStageObjectTree *pegTree  = m_xshHandle->getXsheet()->getStageObjectTree();
  QList<TStageObjectId> objs = m_selection->getObjects();
  int i;
  for (i = 0; i < objs.size(); i++) {
    TStageObject *obj = pegTree->getStageObject(objs[i], false);
    if (obj && obj->isGrouped() && !obj->isGroupEditing()) obj->editGroup();
  }
  updateScene();
}

//------------------------------------------------------------------

TStageObjectId StageSchematicScene::getCurrentObject() {
  return m_objHandle->getObjectId();
}

//------------------------------------------------------------------

void StageSchematicScene::onNodeChangedSize() {
  if (resizingNodes) return;
  updateScene();
}

//------------------------------------------------------------------
// snap to neighbor nodes on dragging
void StageSchematicScene::updateSnapTarget(QGraphicsItem *item) {
  clearSnapTargets();
  StageSchematicNode *node = dynamic_cast<StageSchematicNode *>(item);
  if (!node) return;

  int portCount = node->getChildCount();
  for (int i = 0; i < portCount; i++) {
    StageSchematicNodePort *port = node->getChildPort(i);
    int j, linkCount = port->getLinkCount();
    for (j = 0; j < linkCount; j++) {
      SchematicLink *link = port->getLink(j);
      if (!link) continue;
      SchematicNode *otherNode = link->getOtherNode(node);
      if (otherNode && !otherNode->isSelected()) {
        QPointF targetPos =
            otherNode->scenePos() -
            QPointF(node->boundingRect().width() + SchematicScene::snapHSpacing,
                    0);
        addSnapTarget(targetPos, node->boundingRect(),
                      link->getOtherPort(port)->getLinkEndPoint(),
                      port->getLinkEndPoint() - node->scenePos());
      }
    }
  }

  StageSchematicNodePort *port = node->getParentPort();
  if (port) {
    int linkCount = port->getLinkCount();
    for (int i = 0; i < linkCount; i++) {
      SchematicLink *link = port->getLink(i);
      if (!link) continue;
      SchematicNode *otherNode = link->getOtherNode(node);
      if (otherNode && !otherNode->isSelected()) {
        QPointF targetPos =
            otherNode->scenePos() + QPointF(otherNode->boundingRect().width() +
                                                SchematicScene::snapHSpacing,
                                            0);
        addSnapTarget(targetPos, node->boundingRect(),
                      link->getOtherPort(port)->getLinkEndPoint(),
                      port->getLinkEndPoint() - node->scenePos());
      }
    }
  }
}