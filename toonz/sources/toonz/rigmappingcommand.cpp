// Rig Mapping: copies the animation of the parts of a rig onto the parts of
// another rig that have the same names. See doc/animate_rig_mapping.md; the
// matching itself is in toonz/rigmapping.h.

// Tnz6 includes
#include "menubarcommandids.h"
#include "cellselection.h"
#include "columnselection.h"
#include "tapp.h"

// TnzQt includes
#include "stageobjectselection.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/tselectionhandle.h"
#include "toonzqt/uitranslations.h"

// TnzLib includes
#include "toonz/rigmapping.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevel.h"

// TnzExt includes
#include "ext/plasticskeletondeformation.h"

// TnzBase includes
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tenv.h"

// TnzCore includes
#include "tundo.h"
#include "historytypes.h"

// Qt includes
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

// STD includes
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

TEnv::IntVar RigMappingRelative("RigMappingRelative", 1);
TEnv::IntVar RigMappingReplace("RigMappingReplace", 1);
TEnv::IntVar RigMappingPlastic("RigMappingPlastic", 1);

namespace {

QWidget *dialogParent() { return TApp::instance()->getMainWindow(); }

//=============================================================================
// The rigs
//-----------------------------------------------------------------------------

// A stage object of a rig.
struct RigPart {
  TStageObjectId m_id;
  QString m_name;           // shown to the user
  std::string m_matchName;  // case folded; empty if the part has no name
  int m_parent;             // index in the rig, -1 for the root
  int m_depth;
};

typedef std::vector<RigPart> RigParts;

// The name of the only level of a column, or an empty string.
QString singleLevelName(TXsheet *xsh, int columnIndex) {
  TXshColumn *column    = xsh->getColumn(columnIndex);
  TXshCellColumn *cells = column ? column->getCellColumn() : nullptr;
  if (!cells) return QString();
  int r0 = 0, r1 = -1;
  cells->getRange(r0, r1);
  TXshLevel *single = nullptr;
  for (int r = r0; r <= r1; r++) {
    const TXshCell &cell = cells->getCell(r);
    if (cell.isEmpty()) continue;
    TXshLevel *level = cell.m_level.getPointer();
    if (!single)
      single = level;
    else if (single != level)
      return QString();
  }
  return single ? QString::fromStdWString(single->getName()) : QString();
}

// The name of a part: its own one or, for a column without one, the name of
// its level if it has a single one (as the column header can show).
void getPartNames(TXsheet *xsh, const TStageObject *object, QString &shown,
                  std::string &matchName) {
  QString name;
  if (object->hasSpecifiedName())
    name = QString::fromStdString(object->getName());
  else if (object->getId().isColumn())
    name = singleLevelName(xsh, object->getId().getIndex());
  shown     = name.isEmpty() ? QString::fromStdString(object->getName()) : name;
  matchName = name.trimmed().toCaseFolded().toStdString();
}

void collectPart(TXsheet *xsh, TStageObject *object, int parent, int depth,
                 RigParts &rig, std::set<TStageObjectId> &seen) {
  if (!seen.insert(object->getId()).second) return;
  RigPart part;
  part.m_id     = object->getId();
  part.m_parent = parent;
  part.m_depth  = depth;
  getPartNames(xsh, object, part.m_name, part.m_matchName);
  const int index = int(rig.size());
  rig.push_back(part);

  // the children in the order of the xsheet: columns left to right, then
  // pegbars
  std::vector<TStageObject *> children(object->getChildren().begin(),
                                       object->getChildren().end());
  std::sort(
      children.begin(), children.end(),
      [](TStageObject *a, TStageObject *b) { return a->getId() < b->getId(); });
  for (TStageObject *child : children)
    if (child) collectPart(xsh, child, index, depth + 1, rig, seen);
}

// The object and all the objects attached to it, directly or not, the
// parents before their children.
void collectRig(TXsheet *xsh, const TStageObjectId &rootId, RigParts &rig) {
  TStageObject *root = xsh->getStageObjectTree()->getStageObject(rootId, false);
  if (!root) return;
  std::set<TStageObjectId> seen;
  collectPart(xsh, root, -1, 0, rig, seen);
}

std::vector<RigMapping::Node> toNodes(const RigParts &rig) {
  std::vector<RigMapping::Node> nodes;
  for (const RigPart &part : rig)
    nodes.push_back(RigMapping::Node(part.m_matchName, part.m_parent));
  return nodes;
}

//=============================================================================
// Copying the keys
//-----------------------------------------------------------------------------

// The animatable channels of a stage object but the position on its motion
// path, with the rule of their values in the relative mode.
const struct {
  TStageObject::Channel m_channel;
  RigMapping::ValueRule m_rule;
} cChannels[] = {
    {TStageObject::T_Angle, RigMapping::ValueRule::Offset},
    {TStageObject::T_X, RigMapping::ValueRule::Offset},
    {TStageObject::T_Y, RigMapping::ValueRule::Offset},
    {TStageObject::T_Z, RigMapping::ValueRule::Offset},
    {TStageObject::T_SO, RigMapping::ValueRule::Offset},
    {TStageObject::T_ScaleX, RigMapping::ValueRule::Scale},
    {TStageObject::T_ScaleY, RigMapping::ValueRule::Scale},
    {TStageObject::T_Scale, RigMapping::ValueRule::Scale},
    {TStageObject::T_ShearX, RigMapping::ValueRule::Offset},
    {TStageObject::T_ShearY, RigMapping::ValueRule::Offset},
    {TStageObject::T_RotX, RigMapping::ValueRule::Offset},
    {TStageObject::T_RotY, RigMapping::ValueRule::Offset},
};

struct Settings {
  bool m_relative;  // else absolute values
  bool m_replace;   // else the other keys of the target are kept
  bool m_plastic;   // also the Plastic vertex curves, by vertex name
};

struct Stats {
  int m_parts;     // pairs that got at least one curve
  int m_channels;  // curves copied
  int m_skipped;   // curves left out for their expression keys

  Stats() : m_parts(0), m_channels(0), m_skipped(0) {}
};

// The keys and the cycle of a curve, before and after the command.
struct CurveChange {
  TDoubleParamP m_param;
  std::vector<TDoubleKeyframe> m_oldKeys, m_newKeys;
  bool m_oldCycle, m_newCycle;
};

void getKeys(const TDoubleParam *param, std::vector<TDoubleKeyframe> &keys) {
  const int count = param->getKeyframeCount();
  keys.reserve(keys.size() + count);
  for (int k = 0; k < count; k++) keys.push_back(param->getKeyframe(k));
}

void setKeys(TDoubleParam *param, const std::vector<TDoubleKeyframe> &keys,
             bool cycle) {
  param->clearKeyframes();
  for (const TDoubleKeyframe &key : keys) param->setKeyframe(key);
  if (param->isCycleEnabled() != cycle) param->enableCycle(cycle);
}

// Keys whose segments are not made of keys: an expression, a file or a
// similar shape. They refer to other curves or files, and are left alone.
bool hasExpressionKeys(const TDoubleParam *param) {
  const int count = param->getKeyframeCount();
  for (int k = 0; k < count; k++) {
    TDoubleKeyframe::Type type = param->getKeyframe(k).m_type;
    if (type == TDoubleKeyframe::Expression || type == TDoubleKeyframe::File ||
        type == TDoubleKeyframe::SimilarShape)
      return true;
  }
  return false;
}

// Copies the keys of src to dst: their frames, interpolations, handles and
// steps, and the cycle. Returns 1 if copied, 0 if there is nothing to copy,
// -1 if left out.
int copyCurve(TDoubleParam *src, TDoubleParam *dst, RigMapping::ValueRule rule,
              const Settings &settings, std::vector<CurveChange> &changes) {
  if (!src || !dst || src == dst || !src->hasKeyframes()) return 0;
  if (hasExpressionKeys(src) || hasExpressionKeys(dst)) return -1;

  CurveChange change;
  change.m_param = dst;
  getKeys(dst, change.m_oldKeys);
  change.m_oldCycle = dst->isCycleEnabled();

  // the keys of the source, in frame order, with the values of the target
  std::vector<TDoubleKeyframe> keys;
  getKeys(src, keys);
  const TDoubleKeyframe first    = keys.front();
  const RigMapping::ValueMap map = RigMapping::retargetMap(
      settings.m_relative ? rule : RigMapping::ValueRule::Absolute,
      first.m_value, dst->getValue(first.m_frame));
  for (TDoubleKeyframe &key : keys) {
    key.m_value = map(key.m_value);
    key.m_speedIn.y *= map.factor;
    key.m_speedOut.y *= map.factor;
    key.m_isKeyframe = true;
  }

  // the keys of the target at the same frames are replaced in any case
  std::map<double, TDoubleKeyframe> merged;
  if (!settings.m_replace)
    for (const TDoubleKeyframe &key : change.m_oldKeys)
      merged[key.m_frame] = key;
  for (const TDoubleKeyframe &key : keys) merged[key.m_frame] = key;
  for (const auto &entry : merged) change.m_newKeys.push_back(entry.second);
  change.m_newCycle = settings.m_replace
                          ? src->isCycleEnabled()
                          : (change.m_oldCycle || src->isCycleEnabled());

  setKeys(dst, change.m_newKeys, change.m_newCycle);
  changes.push_back(change);
  return 1;
}

void countCopy(int result, int &copied, Stats &stats) {
  if (result > 0) {
    copied++;
    stats.m_channels++;
  } else if (result < 0)
    stats.m_skipped++;
}

// The names of the vertices that have a deformation, and those names case
// folded for the matching.
void getVertexNames(const PlasticSkeletonDeformationP &sd,
                    std::vector<QString> &names,
                    std::vector<RigMapping::Node> &nodes) {
  PlasticSkeletonDeformation::vd_iterator vdt, vdEnd;
  sd->vertexDeformations(vdt, vdEnd);
  for (; vdt != vdEnd; ++vdt) {
    const std::pair<const QString *, SkVD *> &vd = *vdt;
    names.push_back(*vd.first);
    nodes.push_back(
        RigMapping::Node(vd.first->trimmed().toCaseFolded().toStdString(), -1));
  }
}

// The curves of the vertices of the Plastic skeleton of src go to the
// vertices of the same names of dst. Returns the number of curves copied.
int copyPlastic(const TStageObject *src, const TStageObject *dst,
                const Settings &settings, std::vector<CurveChange> &changes,
                Stats &stats) {
  const PlasticSkeletonDeformationP srcSd =
      src->getPlasticSkeletonDeformation();
  const PlasticSkeletonDeformationP dstSd =
      dst->getPlasticSkeletonDeformation();
  if (!srcSd || !dstSd || srcSd == dstSd) return 0;

  std::vector<QString> srcNames, dstNames;
  std::vector<RigMapping::Node> srcNodes, dstNodes;
  getVertexNames(srcSd, srcNames, srcNodes);
  getVertexNames(dstSd, dstNames, dstNodes);

  // the names only: a flat list has no tree to give positions
  RigMapping::Options options;
  options.pairSingleRoots = false;
  options.matchByPosition = false;
  const std::vector<RigMapping::Pair> pairs =
      RigMapping::match(srcNodes, dstNodes, options);

  int copied = 0;
  for (const RigMapping::Pair &pair : pairs) {
    SkVD *srcVd = srcSd->vertexDeformation(srcNames[pair.source]);
    SkVD *dstVd = dstSd->vertexDeformation(dstNames[pair.target]);
    if (!srcVd || !dstVd) continue;
    // angle and distance are changes from the rest pose, the stacking order
    // an offset too
    for (int p = 0; p < SkVD::PARAMS_COUNT; p++)
      countCopy(copyCurve(srcVd->m_params[p].getPointer(),
                          dstVd->m_params[p].getPointer(),
                          RigMapping::ValueRule::Offset, settings, changes),
                copied, stats);
  }
  return copied;
}

//=============================================================================
// Undo
//-----------------------------------------------------------------------------

void notifyRigMapped() {
  TApp *app = TApp::instance();
  app->getCurrentScene()->setDirtyFlag(true);
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentObject()->notifyObjectIdChanged(false);
}

// The whole command: the keys of every changed curve before and after it.
class RigMappingUndo final : public TUndo {
  std::vector<CurveChange> m_changes;

public:
  explicit RigMappingUndo(const std::vector<CurveChange> &changes)
      : m_changes(changes) {}

  void undo() const override {
    for (auto it = m_changes.rbegin(); it != m_changes.rend(); ++it)
      setKeys(it->m_param.getPointer(), it->m_oldKeys, it->m_oldCycle);
    notifyRigMapped();
  }
  void redo() const override {
    for (const CurveChange &change : m_changes)
      setKeys(change.m_param.getPointer(), change.m_newKeys, change.m_newCycle);
    notifyRigMapped();
  }
  int getSize() const override {
    std::size_t size = sizeof(*this);
    for (const CurveChange &change : m_changes)
      size += sizeof(change) +
              sizeof(TDoubleKeyframe) *
                  (change.m_oldKeys.size() + change.m_newKeys.size());
    return int(size);
  }
  QString getHistoryString() override { return QObject::tr("Rig Mapping"); }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//-----------------------------------------------------------------------------

struct PartPair {
  int m_source, m_target;  // indices in the rigs
};

// Copies the keys of the source parts to the target parts, as one undo.
Stats applyRigMapping(TXsheet *xsh, const RigParts &source,
                      const RigParts &target,
                      const std::vector<PartPair> &pairs,
                      const Settings &settings) {
  Stats stats;
  TStageObjectTree *tree = xsh->getStageObjectTree();
  std::vector<CurveChange> changes;
  for (const PartPair &pair : pairs) {
    TStageObject *src = tree->getStageObject(source[pair.m_source].m_id, false);
    TStageObject *dst = tree->getStageObject(target[pair.m_target].m_id, false);
    if (!src || !dst || src == dst) continue;
    int copied = 0;
    for (const auto &channel : cChannels)
      countCopy(copyCurve(src->getParam(channel.m_channel),
                          dst->getParam(channel.m_channel), channel.m_rule,
                          settings, changes),
                copied, stats);
    if (settings.m_plastic)
      copied += copyPlastic(src, dst, settings, changes, stats);
    if (copied > 0) stats.m_parts++;
  }
  if (!changes.empty()) {
    TUndoManager::manager()->add(new RigMappingUndo(changes));
    notifyRigMapped();
  }
  return stats;
}

//-----------------------------------------------------------------------------

// The number of curves of a part that have keys, Plastic ones included.
int animatedCurveCount(TXsheet *xsh, const TStageObjectId &id) {
  const TStageObject *object =
      xsh->getStageObjectTree()->getStageObject(id, false);
  if (!object) return 0;
  int count = 0;
  for (const auto &channel : cChannels) {
    const TDoubleParam *param = object->getParam(channel.m_channel);
    if (param && param->hasKeyframes()) count++;
  }
  const PlasticSkeletonDeformationP sd =
      object->getPlasticSkeletonDeformation();
  if (sd) {
    PlasticSkeletonDeformation::vd_iterator vdt, vdEnd;
    sd->vertexDeformations(vdt, vdEnd);
    for (; vdt != vdEnd; ++vdt) {
      const std::pair<const QString *, SkVD *> &vd = *vdt;
      for (int p = 0; p < SkVD::PARAMS_COUNT; p++)
        if (vd.second->m_params[p] && vd.second->m_params[p]->hasKeyframes())
          count++;
    }
  }
  return count;
}

//=============================================================================
// The dialog
//-----------------------------------------------------------------------------

// The pairs found by name or position, which the user can change.
class RigMappingDialog final : public QDialog {
  Q_DECLARE_TR_FUNCTIONS(RigMappingDialog)

  enum { SourceColumn, TargetColumn, MatchColumn, KeysColumn, ColumnCount };

  TXsheet *m_xsh;
  RigParts m_source, m_target;

  QLabel *m_sourceLabel, *m_targetLabel;
  QTableWidget *m_table;
  std::vector<QComboBox *> m_combos;  // by source part
  std::vector<int> m_proposed;        // the target proposed, by source part
  std::vector<RigMapping::Confidence> m_confidence;
  QRadioButton *m_relative, *m_absolute, *m_replace, *m_keep;
  QCheckBox *m_plastic;

public:
  RigMappingDialog(QWidget *parent, TXsheet *xsh, const RigParts &source,
                   const RigParts &target)
      : QDialog(parent), m_xsh(xsh), m_source(source), m_target(target) {
    setWindowTitle(tr("Rig Mapping"));

    m_sourceLabel = new QLabel(this);
    m_targetLabel = new QLabel(this);
    QPushButton *swapButton =
        new QPushButton(tr("Swap Source and Target"), this);
    QLabel *hint = new QLabel(
        tr("The animation of each ticked source part is copied to the "
           "target part of its row. Pick another target, or none, in the "
           "Target Part column. Pairs found only by their position, "
           "between parts that have different names of their own, start "
           "unticked."),
        this);
    hint->setWordWrap(true);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels(QStringList()
                                       << tr("Source Part") << tr("Target Part")
                                       << tr("Match") << tr("Animated Curves"));
    m_table->verticalHeader()->hide();
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    QHeaderView *header = m_table->horizontalHeader();
    header->setSectionResizeMode(SourceColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(TargetColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(MatchColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(KeysColumn, QHeaderView::ResizeToContents);

    m_relative = new QRadioButton(
        tr("Copy keys relative to the target's rest values"), this);
    m_relative->setToolTip(
        tr("Each target curve starts from its own value at the first key "
           "and gets the changes of the source curve: added for positions "
           "and angles, multiplied for scales."));
    m_absolute = new QRadioButton(tr("Copy absolute values"), this);
    m_absolute->setToolTip(tr("The target gets the values of the source."));
    QGroupBox *valuesBox      = new QGroupBox(tr("Values"), this);
    QVBoxLayout *valuesLayout = new QVBoxLayout(valuesBox);
    valuesLayout->addWidget(m_relative);
    valuesLayout->addWidget(m_absolute);

    m_replace = new QRadioButton(
        tr("Replace existing keys of the target channels"), this);
    m_replace->setToolTip(
        tr("The keys of a target curve are removed before the keys of the "
           "source are copied."));
    m_keep = new QRadioButton(tr("Keep other keys"), this);
    m_keep->setToolTip(
        tr("The keys of a target curve at other frames than the keys of "
           "the source are kept."));
    QGroupBox *keysBox      = new QGroupBox(tr("Existing Keys"), this);
    QVBoxLayout *keysLayout = new QVBoxLayout(keysBox);
    keysLayout->addWidget(m_replace);
    keysLayout->addWidget(m_keep);

    m_plastic = new QCheckBox(
        tr("Also map Plastic vertex animation (by vertex name)"), this);

    (RigMappingRelative != 0 ? m_relative : m_absolute)->setChecked(true);
    (RigMappingReplace != 0 ? m_replace : m_keep)->setChecked(true);
    m_plastic->setChecked(RigMappingPlastic != 0);

    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    // A custom AcceptRole button keeps the Apply caption when Qt refreshes
    // its standard button translations during a language change.
    buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    QObject::connect(buttons, &QDialogButtonBox::accepted, this,
                     [this]() { onApply(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                     &QDialog::reject);
    QObject::connect(swapButton, &QPushButton::clicked, this, [this]() {
      std::swap(m_source, m_target);
      refresh();
    });

    QHBoxLayout *rigsLayout   = new QHBoxLayout();
    QVBoxLayout *labelsLayout = new QVBoxLayout();
    labelsLayout->addWidget(m_sourceLabel);
    labelsLayout->addWidget(m_targetLabel);
    rigsLayout->addLayout(labelsLayout, 1);
    rigsLayout->addWidget(swapButton, 0, Qt::AlignTop);

    QHBoxLayout *optionsLayout = new QHBoxLayout();
    optionsLayout->addWidget(valuesBox);
    optionsLayout->addWidget(keysBox);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(rigsLayout);
    layout->addWidget(hint);
    layout->addWidget(m_table, 1);
    layout->addLayout(optionsLayout);
    layout->addWidget(m_plastic);
    layout->addWidget(buttons);

    resize(720, 520);
    refresh();
    UiTranslations::onLanguageChanged(this, [this]() { retranslateUi(); });
  }

  const RigParts &source() const { return m_source; }
  const RigParts &target() const { return m_target; }

  // The ticked rows with a target.
  std::vector<PartPair> pairs() const {
    std::vector<PartPair> result;
    for (int row = 0; row < int(m_combos.size()); row++) {
      QTableWidgetItem *item = m_table->item(row, SourceColumn);
      if (!item || item->checkState() != Qt::Checked) continue;
      const int target = m_combos[row]->currentData().toInt();
      if (target < 0) continue;
      PartPair pair;
      pair.m_source = row;
      pair.m_target = target;
      result.push_back(pair);
    }
    return result;
  }

  Settings settings() const {
    Settings settings;
    settings.m_relative = m_relative->isChecked();
    settings.m_replace  = m_replace->isChecked();
    settings.m_plastic  = m_plastic->isChecked();
    return settings;
  }

private:
  static QString confidenceText(RigMapping::Confidence confidence) {
    switch (confidence) {
    case RigMapping::Exact:
      return tr("Same name");
    case RigMapping::Normalized:
      return tr("Same name (case or separators differ)");
    case RigMapping::IgnoringNumber:
      return tr("Same name (number ignored)");
    default:
      return tr("By position");
    }
  }

  static QString indented(const RigPart &part) {
    return QString(part.m_depth * 3, QChar(' ')) + part.m_name;
  }

  void updateRigLabels() {
    m_sourceLabel->setText(
        tr("Source rig (the first selected, its animation is copied): "
           "<b>%1</b>")
            .arg(m_source.empty() ? QString()
                                  : m_source.front().m_name.toHtmlEscaped()));
    m_targetLabel->setText(
        tr("Target rig (receives the animation): <b>%1</b>")
            .arg(m_target.empty() ? QString()
                                  : m_target.front().m_name.toHtmlEscaped()));
  }

  void retranslateUi() {
    updateRigLabels();
    const QSignalBlocker tableSignals(m_table);
    const QSignalBlocker modelSignals(m_table->model());
    m_table->setHorizontalHeaderLabels(
        {tr("Source Part"), tr("Target Part"), tr("Match"), tr("Animated Curves")});
    for (int row = 0; row < int(m_combos.size()); ++row) {
      QComboBox *combo = m_combos[row];
      const QSignalBlocker comboSignals(combo);
      const QSignalBlocker comboModelSignals(combo->model());
      const int none = combo->findData(-1);
      if (none >= 0) combo->setItemText(none, tr("(None)"));
      combo->update();
      combo->view()->viewport()->update();
      // The user's chosen target and check state remain untouched. In
      // particular, do not call refresh(), which rematches and rebuilds rows.
      updateMatchText(row);
    }
    m_table->viewport()->update();
    m_table->horizontalHeader()->viewport()->update();
  }

  void refresh() {
    updateRigLabels();

    const std::vector<RigMapping::Pair> found =
        RigMapping::match(toNodes(m_source), toNodes(m_target));
    m_proposed.assign(m_source.size(), -1);
    m_confidence.assign(m_source.size(), RigMapping::Position);
    for (const RigMapping::Pair &pair : found) {
      m_proposed[pair.source]   = pair.target;
      m_confidence[pair.source] = pair.confidence;
    }

    // the rows and their combo boxes go
    m_table->setRowCount(0);
    m_combos.clear();
    m_table->setRowCount(int(m_source.size()));
    for (int row = 0; row < int(m_source.size()); row++) {
      const RigPart &part = m_source[row];
      const int proposed  = m_proposed[row];

      // pairs only by position between parts with names that differ start
      // unticked: the names say they may not go together
      bool ticked = proposed >= 0;
      if (ticked && m_confidence[row] == RigMapping::Position &&
          !RigMapping::isDefaultName(part.m_matchName) &&
          !RigMapping::isDefaultName(m_target[proposed].m_matchName))
        ticked = false;

      QTableWidgetItem *sourceItem = new QTableWidgetItem(indented(part));
      sourceItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
      sourceItem->setCheckState(ticked ? Qt::Checked : Qt::Unchecked);
      m_table->setItem(row, SourceColumn, sourceItem);

      QComboBox *combo = new QComboBox(m_table);
      combo->addItem(tr("(None)"), -1);
      for (int t = 0; t < int(m_target.size()); t++)
        combo->addItem(indented(m_target[t]), t);
      combo->setCurrentIndex(proposed + 1);
      m_table->setCellWidget(row, TargetColumn, combo);
      m_combos.push_back(combo);

      QTableWidgetItem *matchItem = new QTableWidgetItem();
      matchItem->setFlags(Qt::ItemIsEnabled);
      m_table->setItem(row, MatchColumn, matchItem);
      updateMatchText(row);

      QTableWidgetItem *keysItem = new QTableWidgetItem(
          QString::number(animatedCurveCount(m_xsh, part.m_id)));
      keysItem->setFlags(Qt::ItemIsEnabled);
      keysItem->setTextAlignment(Qt::AlignCenter);
      m_table->setItem(row, KeysColumn, keysItem);

      QObject::connect(
          combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this, row](int index) {
            QTableWidgetItem *item = m_table->item(row, SourceColumn);
            if (item)
              item->setCheckState(index > 0 ? Qt::Checked : Qt::Unchecked);
            updateMatchText(row);
          });
    }
  }

  void updateMatchText(int row) {
    QTableWidgetItem *item = m_table->item(row, MatchColumn);
    if (!item || row >= int(m_combos.size())) return;
    const int target = m_combos[row]->currentData().toInt();
    if (target < 0)
      item->setText(tr("No match"));
    else if (target == m_proposed[row])
      item->setText(confidenceText(m_confidence[row]));
    else
      item->setText(tr("Chosen by hand"));
  }

  void onApply() {
    const std::vector<PartPair> chosen = pairs();
    if (chosen.empty()) {
      QMessageBox::information(this, tr("Rig Mapping"),
                               tr("No row is ticked with a target part."));
      return;
    }
    // a target part can't get the keys of two source parts
    std::set<int> targets;
    for (const PartPair &pair : chosen)
      if (!targets.insert(pair.m_target).second) {
        QMessageBox::warning(
            this, tr("Rig Mapping"),
            tr("The target part \"%1\" is chosen in more than one row. "
               "Choose another target, or untick a row.")
                .arg(m_target[pair.m_target].m_name));
        return;
      }
    RigMappingRelative = m_relative->isChecked() ? 1 : 0;
    RigMappingReplace  = m_replace->isChecked() ? 1 : 0;
    RigMappingPlastic  = m_plastic->isChecked() ? 1 : 0;
    accept();
  }
};

//=============================================================================
// The command
//-----------------------------------------------------------------------------

bool isRigRoot(const TStageObjectId &id) {
  return id.isColumn() || id.isPegbar();
}

// The selected columns and pegbars, from left to right in the Xsheet and the
// Timeline. Returns whether they were selected by clicking them (nodes of the
// Stage Schematic, column headers), rather than as cells.
bool getSelectedObjects(TXsheet *xsh, std::vector<TStageObjectId> &ids) {
  bool byClicks = false;
  TSelection *selection =
      TApp::instance()->getCurrentSelection()->getSelection();
  if (StageObjectSelection *objectSelection =
          dynamic_cast<StageObjectSelection *>(selection)) {
    byClicks = true;
    for (const TStageObjectId &id : objectSelection->getObjects())
      if (isRigRoot(id)) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
  } else if (TColumnSelection *columnSelection =
                 dynamic_cast<TColumnSelection *>(selection)) {
    byClicks = true;
    for (int index : columnSelection->getIndices())
      if (index >= 0) ids.push_back(TStageObjectId::ColumnId(index));
  } else if (TCellSelection *cellSelection =
                 dynamic_cast<TCellSelection *>(selection)) {
    if (!cellSelection->isEmpty()) {
      int r0, c0, r1, c1;
      cellSelection->getSelectedCells(r0, c0, r1, c1);
      for (int c = std::max(0, std::min(c0, c1)); c <= std::max(c0, c1); c++)
        ids.push_back(TStageObjectId::ColumnId(c));
    }
  }
  // the same object once, and only existing ones
  std::vector<TStageObjectId> unique;
  for (const TStageObjectId &id : ids)
    if (std::find(unique.begin(), unique.end(), id) == unique.end() &&
        xsh->getStageObjectTree()->getStageObject(id, false))
      unique.push_back(id);
  ids.swap(unique);
  return byClicks;
}

void executeRigMapping() {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh) return;

  std::vector<TStageObjectId> ids;
  const bool byClicks = getSelectedObjects(xsh, ids);
  if (ids.size() != 2) {
    QMessageBox::information(
        dialogParent(), RigMappingDialog::tr("Rig Mapping"),
        RigMappingDialog::tr(
            "Select exactly two columns or pegbars: the root of the rig "
            "whose animation is copied first (the source rig), then the "
            "root of the rig that receives it (the target rig).\n"
            "The parts attached to them, directly or not, are mapped by "
            "name."));
    return;
  }
  // The first selected is the source. The selections do not keep the order
  // of the clicks, but each click makes the object clicked the current one:
  // the current object is the last selected, the target.
  if (byClicks && app->getCurrentObject()->getObjectId() == ids[0])
    std::swap(ids[0], ids[1]);

  RigParts source, target;
  collectRig(xsh, ids[0], source);
  collectRig(xsh, ids[1], target);
  std::set<TStageObjectId> sourceIds;
  for (const RigPart &part : source) sourceIds.insert(part.m_id);
  for (const RigPart &part : target)
    if (sourceIds.count(part.m_id)) {
      QMessageBox::information(
          dialogParent(), RigMappingDialog::tr("Rig Mapping"),
          RigMappingDialog::tr(
              "One of the selected objects is attached to the other one. "
              "Select the roots of two separate rigs."));
      return;
    }

  RigMappingDialog dialog(dialogParent(), xsh, source, target);
  if (dialog.exec() != QDialog::Accepted) return;

  const Stats stats = applyRigMapping(xsh, dialog.source(), dialog.target(),
                                      dialog.pairs(), dialog.settings());
  if (stats.m_channels == 0 && stats.m_skipped == 0)
    QMessageBox::information(
        dialogParent(), RigMappingDialog::tr("Rig Mapping"),
        RigMappingDialog::tr("The ticked source parts have no keys to copy."));
  else if (stats.m_skipped > 0)
    QMessageBox::information(
        dialogParent(), RigMappingDialog::tr("Rig Mapping"),
        RigMappingDialog::tr(
            "%1 curves were copied to %2 parts. %3 curves were left out "
            "because the source or the target has expression, file or "
            "similar shape keys.")
            .arg(stats.m_channels)
            .arg(stats.m_parts)
            .arg(stats.m_skipped));
}

class RigMappingCommand final : public MenuItemHandler {
public:
  RigMappingCommand() : MenuItemHandler(MI_RigMapping) {}
  void execute() override { executeRigMapping(); }
} rigMappingCommand;

}  // namespace
