#include "storyboardtiming.h"
#include "tapp.h"
#include "tundo.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "toonz/navigationtags.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tpinnedrangeset.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshsoundcolumn.h"
#include "toonzqt/functiontreeviewer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
struct CurveState {
  TDoubleParamP target, before, after;
};
struct PinState {
  TStageObjectP object;
  TPinnedRangeSet before, after;
};
struct SoundState {
  int column;
  TXshColumnP before, after;
};

void collectCurves(TreeModel::Item *item, std::set<TDoubleParam *> &curves) {
  if (!item) return;
  if (auto *channel = dynamic_cast<FunctionTreeModel::Channel *>(item))
    if (channel->getParam()->hasKeyframes()) curves.insert(channel->getParam());
  for (int i = 0; i < item->getChildCount(); ++i)
    collectCurves(item->getChild(i), curves);
}

// Native cuts preserve full interpolation inside each contiguous run. At a
// shot boundary the last source frame holds until the next shot begins.
bool canCut(TDoubleParam *curve, int frame) {
  if (curve->isKeyframe(frame)) return true;
  int previous = curve->getPrevKeyframe(frame);
  if (previous < 0 || previous == curve->getKeyframeCount() - 1) return true;
  const auto &key = curve->getKeyframe(previous);
  return key.m_step == 1 &&
         (key.m_type == TDoubleKeyframe::Constant || key.m_type == TDoubleKeyframe::Linear);
}
TDoubleKeyframe cutKey(TDoubleParam *curve, int frame) {
  if (curve->isKeyframe(frame)) return curve->getKeyframeAt(frame);
  const double value = curve->getValue(frame);
  if (!std::isfinite(value)) throw std::runtime_error("Nonfinite storyboard boundary");
  TDoubleKeyframe key(frame, value);
  const int previous = curve->getPrevKeyframe(frame);
  key.m_type = previous >= 0 && previous < curve->getKeyframeCount() - 1
                   ? curve->getKeyframe(previous).m_type : TDoubleKeyframe::Constant;
  return key;
}
TDoubleParamP nativeMove(TDoubleParam *curve, int start, const std::vector<int> &source) {
  TDoubleParamP result = new TDoubleParam(*curve);
  // Only copied/moved keys and cut neighbours need fixed effective handles.
  // Keep the destination's untouched keys byte-for-byte in their native state.
  TDoubleParamP frozen = new TDoubleParam(*curve);
  for (int i = 0; i < frozen->getKeyframeCount(); ++i) {
    auto key = curve->getKeyframe(i);
    if (i > 0 && curve->getKeyframe(i - 1).m_type == TDoubleKeyframe::SpeedInOut)
      key.m_speedIn = curve->getSpeedIn(i);
    if (key.m_type == TDoubleKeyframe::SpeedInOut) key.m_speedOut = curve->getSpeedOut(i);
    key.m_linkedHandles = false;
    frozen->setKeyframe(key);
  }
  const int end = start + int(source.size());
  for (int i = result->getKeyframeCount() - 1; i >= 0; --i) {
    const double frame = result->getKeyframe(i).m_frame;
    if (frame >= start && frame < end) result->deleteKeyframe(frame);
  }
  // Scene frame zero has no preceding editable scene frame. Do not create
  // negative keys unless the original curve already contains negative timing.
  if (start > 0 || curve->getKeyframe(0).m_frame < 0) {
    auto key = cutKey(frozen.getPointer(), start - 1);
    key.m_type = TDoubleKeyframe::Constant;
    result->setKeyframe(key);
  }
  result->setKeyframe(cutKey(frozen.getPointer(), end));
  for (int a = 0; a < int(source.size());) {
    int b = a;
    while (b + 1 < int(source.size()) && source[b + 1] == source[b] + 1) ++b;
    const int delta = start + a - source[a];
    auto put = [&](TDoubleKeyframe key) {
      if (key.m_frame == source[b]) key.m_type = TDoubleKeyframe::Constant;
      key.m_frame += delta;
      result->setKeyframe(key);
    };
    put(cutKey(frozen.getPointer(), source[a]));
    for (int i = 0; i < frozen->getKeyframeCount(); ++i) {
      const auto &key = frozen->getKeyframe(i);
      if (key.m_frame > source[a] && key.m_frame < source[b]) put(key);
    }
    put(cutKey(frozen.getPointer(), source[b]));
    a = b + 1;
  }
  return result;
}

class StoryboardTimingUndo final : public TUndo {
  TXsheetP m_sheet;
  int m_start;
  std::vector<int> m_columns;
  std::vector<std::vector<TXshCell>> m_before, m_after;
  std::vector<SoundState> m_sounds;
  std::vector<CurveState> m_curves;
  std::vector<PinState> m_pins;
  std::vector<TStageObjectP> m_cycledObjects;
  std::vector<NavigationTags::Tag> m_tagsBefore, m_tagsAfter;

  void apply(bool after) const {
    auto *sheet = m_sheet.getPointer();
    const auto &cells = after ? m_after : m_before;
    for (size_t i = 0; i < cells.size(); ++i)
      sheet->setCells(m_start, m_columns[i], int(cells[i].size()), cells[i].data());
    for (const auto &sound : m_sounds) {
      auto *column = sheet->getColumn(sound.column)->getSoundColumn();
      column->assignLevels((after ? sound.after : sound.before)->getSoundColumn());
    }
    for (const auto &curve : m_curves)
      curve.target->copy((after ? curve.after : curve.before).getPointer());
    for (const auto &pin : m_pins) {
      *pin.object->getPinnedRangeSet() = after ? pin.after : pin.before;
      pin.object->invalidate();
    }
    for (const auto &object : m_cycledObjects) {
      object->enableCycle(!after);
      object->invalidate();
    }
    sheet->getNavigationTags()->m_tags = after ? m_tagsAfter : m_tagsBefore;
    sheet->invalidateSound();
    sheet->updateFrameCount();
    TApp::instance()->getCurrentScene()->setDirtyFlag(true);
    TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
  }

public:
  StoryboardTimingUndo(TXsheet *sheet, int start, const std::vector<int> &source,
                       const std::set<TDoubleParam *> &curves,
                       int sampleStart, int sampleEnd,
                       const std::set<TDoubleParam *> &baked,
                       const std::map<TDoubleParam *, TStageObject *> &cycleOwners,
                       const std::map<TDoubleParam *, TXshLevelColumn *> &remapped,
                       const std::vector<TStageObjectP> &cycledObjects)
      : m_sheet(sheet), m_start(start), m_cycledObjects(cycledObjects) {
    const int count = int(source.size());
    std::vector<int> destination(count);
    for (int row = 0; row < count; ++row) destination[source[row] - start] = start + row;
    // Read every curve against the original graph before any track changes.
    for (auto *parameter : curves) {
      CurveState state;
      state.target = parameter;
      state.before = new TDoubleParam(*parameter);
      if (baked.count(parameter)) state.after = new TDoubleParam(*parameter);
      else state.after = nativeMove(parameter, start, source);
      if (baked.count(parameter)) {
        state.after->clearKeyframes();
        state.after->enableCycle(false);
      }
      for (int row = sampleStart; baked.count(parameter) && row <= sampleEnd; ++row) {
        const int original = row >= start && row < start + count
                                 ? source[row - start] : row;
        double sample = original;
        auto owner = cycleOwners.find(parameter);
        if (owner != cycleOwners.end()) sample = owner->second->paramsTime(original);
        double value = parameter->getValue(sample);
        auto remap = remapped.find(parameter);
        if (remap != remapped.end()) {
          // Keep exposure editing and remapped playback in sync: translate
          // each looked-up source row through the same exposure permutation.
          int sourceRow = remap->second->getRemappedRow(original);
          if (sourceRow >= start && sourceRow < start + count)
            sourceRow = destination[sourceRow - start];
          value = sourceRow + 1.0;
        }
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite storyboard sample");
        TDoubleKeyframe key(row, value);
        key.m_type = key.m_prevType = TDoubleKeyframe::Linear;
        state.after->setKeyframe(key);
      }
      m_curves.push_back(state);
    }
    for (int c = 0; c < sheet->getColumnCount(); ++c) {
      auto *column = sheet->getColumn(c);
      if (!column || !column->getCellColumn()) continue;
      std::vector<TXshCell> before(count), after(count);
      sheet->getCells(start, c, count, before.data());
      for (int row = 0; row < count; ++row) after[row] = before[source[row] - start];
      if (auto *sound = column->getSoundColumn()) {
        // Snapshot clips rather than reconstructing the undo from cells:
        // crossings split on the cut and retain their original source offsets.
        SoundState state{c, sound->clone(), sound->clone()};
        auto *moved = state.after->getSoundColumn();
        moved->clearCells(start, count);
        moved->setCells(start, count, after.data());
        m_sounds.push_back(state);
      } else {
        m_columns.push_back(c);
        m_before.push_back(before);
        m_after.push_back(after);
      }
    }
    auto *tree = sheet->getStageObjectTree();
    for (int i = 0; i < tree->getStageObjectCount(); ++i) {
      auto *object = tree->getStageObject(i);
      auto *pins = object->getPinnedRangeSet();
      if (!pins || !pins->getRangeCount()) continue;
      PinState state{object, *pins, *pins};
      state.after.removeRange(start, start + count - 1);
      for (int row = 0; row < count;) {
        if (!pins->isPinned(source[row])) { ++row; continue; }
        int end = row;
        while (end + 1 < count && pins->isPinned(source[end + 1])) ++end;
        state.after.setRange(start + row, start + end);
        row = end + 1;
      }
      m_pins.push_back(state);
    }
    m_tagsBefore = sheet->getNavigationTags()->m_tags;
    m_tagsAfter = m_tagsBefore;
    for (auto &tag : m_tagsAfter)
      if (tag.m_frame >= start && tag.m_frame < start + count)
        tag.m_frame = destination[tag.m_frame - start];
    std::sort(m_tagsAfter.begin(), m_tagsAfter.end());
  }
  void undo() const override { apply(false); }
  void redo() const override { apply(true); }
  QString getHistoryString() override { return QObject::tr("Move Storyboard Timing"); }
  int getSize() const override {
    size_t size = sizeof(*this);
    for (const auto &cells : m_before) size += 2 * cells.size() * sizeof(TXshCell);
    for (const auto &curve : m_curves)
      size += (curve.before->getKeyframeCount() + curve.after->getKeyframeCount()) * sizeof(TDoubleKeyframe);
    for (const auto &pin : m_pins)
      size += sizeof(PinState) + (pin.before.getRangeCount() + pin.after.getRangeCount()) * sizeof(TPinnedRangeSet::Range);
    for (const auto &sound : m_sounds)
      size += (sound.before->getSoundColumn()->getRowCount() +
               sound.after->getSoundColumn()->getRowCount()) * sizeof(TXshCell);
    return int(std::min<size_t>(size, std::numeric_limits<int>::max()));
  }
};
}

bool moveStoryboardTiming(TXsheet *sheet, int first, int last, int insertion,
                          QString *error, bool allowBake) {
  if (error) error->clear();
  auto reject = [error](const QString &text) { if (error) *error = text; return false; };
  if (!sheet || sheet != TApp::instance()->getCurrentXsheet()->getXsheet() ||
      first < 0 || last < first || last >= sheet->getFrameCount() ||
      insertion < 0 || insertion > sheet->getFrameCount())
    return reject(QObject::tr("The storyboard range is no longer valid. Refresh the panel and try again."));
  if (insertion >= first && insertion <= last + 1) return false;
  std::map<TDoubleParam *, TXshLevelColumn *> remapped;
  for (int c = 0; c < sheet->getColumnCount(); ++c) {
    auto *column = sheet->getColumn(c);
    if (!column) continue;
    if (column->isLocked())
      return reject(QObject::tr("Unlock column %1 before moving all storyboard tracks.").arg(c + 1));
    auto *level = column->getLevelColumn();
    if (level && level->isTimeRemapActive()) {
      if (!allowBake)
        return reject(QObject::tr("Column %1 uses source time remapping. Choose Bake Unsupported Timing to sample and rebase its playback curve together with the source exposures.").arg(c + 1));
      remapped[level->getTimeRemapParam()] = level;
    }
  }
  auto *objects = sheet->getStageObjectTree();
  std::vector<TStageObjectP> cycledObjects;
  std::map<TDoubleParam *, TStageObject *> cycleOwners;
  for (int i = 0; i < objects->getStageObjectCount(); ++i) {
    auto *object = objects->getStageObject(i);
    if (!object->isCycleEnabled()) continue;
    if (!allowBake)
      return reject(QObject::tr("A stage object is cycling. Choose Bake Unsupported Timing to preserve its scene-frame output as finite keys."));
    cycledObjects.push_back(object);
    for (int channel = 0; channel < TStageObject::T_ChannelCount; ++channel)
      if (auto *curve = object->getParam(TStageObject::Channel(channel)))
        cycleOwners[curve] = object;
    for (int layer = 0; layer < object->getOffsetLayerCount(); ++layer) {
      const auto &offset = object->getOffsetLayer(layer);
      cycleOwners[offset.m_weight.getPointer()] = object;
      for (int channel = 0; channel < TStageObject::T_ChannelCount; ++channel)
        if (offset.m_channels[channel]) cycleOwners[offset.m_channels[channel].getPointer()] = object;
    }
  }

  const int start = std::min(first, insertion);
  std::vector<int> source;
  if (insertion > last + 1) {
    for (int row = last + 1; row < insertion; ++row) source.push_back(row);
    for (int row = first; row <= last; ++row) source.push_back(row);
  } else {
    for (int row = first; row <= last; ++row) source.push_back(row);
    for (int row = insertion; row < first; ++row) source.push_back(row);
  }
  if (static_cast<long long>(source.size()) * sheet->getColumnCount() > 2000000)
    return reject(QObject::tr("This move needs more than two million exposure snapshots. Split the scene into smaller sub-xsheets first."));
  FunctionTreeModel model;
  model.refreshReferenceData(sheet);
  std::set<TDoubleParam *> curves;
  for (int i = 0; i < model.getStageObjectsChannelCount(); ++i) {
    auto *group = model.getStageObjectChannel(i);
    collectCurves(group, curves);
    auto *stage = dynamic_cast<StageObjectChannelGroup *>(group);
    if (stage && stage->getStageObject()->isCycleEnabled()) {
      std::set<TDoubleParam *> local;
      collectCurves(group, local);
      for (auto *curve : local) cycleOwners[curve] = stage->getStageObject();
      // Audio gain lives in the same UI group but does not use stage cycling.
      const auto id = stage->getStageObject()->getId();
      auto *column = id.isColumn() ? sheet->getColumn(id.getIndex()) : nullptr;
      if (column && column->getSoundColumn())
        cycleOwners.erase(column->getSoundColumn()->getVolumeCurve());
    }
  }
  for (int i = 0; i < model.getFxsChannelCount(); ++i)
    collectCurves(model.getFxChannel(i), curves);
  // This command edits every animated track, including camera, offset, Plastic
  // and FX channels that have no column lock. Validate the complete set before
  // preparing samples or publishing any exposure/curve undo transaction.
  for (const auto *curve : curves)
    if (curve->isEditorLocked())
      return reject(QObject::tr("A Function Editor channel is locked. Unlock all animated channels in this xsheet before moving storyboard timing."));
  std::set<TDoubleParam *> baked;
  std::set<int> cuts{start + int(source.size())};
  cuts.insert(start - 1);
  for (int row = 0; row < int(source.size()); ++row)
    if (row == 0 || row + 1 == int(source.size()) ||
        source[row] != source[row - 1] + 1 || source[row + 1] != source[row] + 1)
      cuts.insert(source[row]);
  int sampleStart = 0, sampleEnd = sheet->getFrameCount() - 1;
  for (auto *curve : curves) {
    bool representable = !curve->isCycleEnabled() && !cycleOwners.count(curve) && !remapped.count(curve);
    for (int i = 0; i < curve->getKeyframeCount(); ++i)
      if (!TDoubleKeyframe::isKeyframeBased(curve->getKeyframe(i).m_type) ||
          curve->getKeyframe(i).m_step != 1) representable = false;
    for (int frame : cuts) if (!canCut(curve, frame)) representable = false;
    if (!representable) {
      if (!allowBake)
        return reject(QObject::tr("A channel crosses a panel boundary with interpolation or an expression that cannot be moved exactly. Choose Bake Unsupported Timing to sample it, or add boundary keys first."));
      baked.insert(curve);
    }
    const double a = std::floor(curve->getKeyframe(0).m_frame);
    const double b = std::ceil(curve->getKeyframe(curve->getKeyframeCount() - 1).m_frame);
    if (!std::isfinite(a) || !std::isfinite(b) || a < -1000000 || b > 1000000)
      return reject(QObject::tr("A function has keyframes outside the supported storyboard sampling range."));
    sampleStart = std::min(sampleStart, int(a));
    sampleEnd = std::max(sampleEnd, int(b));
  }
  // A bounded, inspectable allocation instead of an unresponsive large bake.
  if (static_cast<long long>(sampleEnd - sampleStart + 1) * baked.size() > 2000000)
    return reject(QObject::tr("This move would create more than two million sampled keys. Split the scene into smaller sub-xsheets first."));
  StoryboardTimingUndo *undo;
  try {
    undo = new StoryboardTimingUndo(sheet, start, source, curves, sampleStart, sampleEnd, baked, cycleOwners, remapped, cycledObjects);
  } catch (...) {
    return reject(QObject::tr("A timing sample could not be prepared. Resolve invalid expressions or reduce the scene size, then retry. No tracks were moved."));
  }
  TUndoManager::manager()->add(undo);
  undo->redo();
  return true;
}
