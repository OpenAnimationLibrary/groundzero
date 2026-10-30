// Width profile commands of the vector selection: give the selected strokes
// one of the built-in profiles or a saved one, and save the profile of a
// stroke. The arithmetic is in tools/widthprofile.h.

#include "tools/strokeselection.h"
#include "tools/widthprofile.h"
#include "tools/widthprofilemanager.h"
#include "tools/levelselection.h"
#include "tools/artlayer.h"
#include "vectorselectiontool.h"

// TnzTools includes
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "tools/toolutils.h"

// TnzQt includes
#include "toonzqt/selectioncommandids.h"
#include "toonzqt/dvdialog.h"

// TnzLib includes
#include "toonz/txshlevelhandle.h"
#include "toonz/txshsimplelevel.h"

// TnzCore includes
#include "tenv.h"
#include "tstroke.h"
#include "tsystem.h"
#include "tundo.h"
#include "tvectorimage.h"

// Qt includes
#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QInputDialog>
#include <QLineEdit>
#include <QMutexLocker>
#include <QString>
#include <QStringList>

// STD includes
#include <algorithm>
#include <string>
#include <limits>
#include "tregion.h"
#include <vector>

using namespace ToolUtils;

//=============================================================================
namespace {

//! The file of the saved profiles, in the user's config folder: one profile
//! per line (see WidthProfile::toText).
TFilePath savedProfilesPath() {
  return TEnv::getConfigDir() + "width_profiles.txt";
}

//! The profile of a stroke: its thickness sampled along its length.
WidthProfile::Profile profileOfStroke(TStroke *stroke) {
  std::vector<double> lengths, thickness;
  const double total = stroke->getLength();
  if (!(total > 0.0)) {
    lengths.push_back(0.0);
    thickness.push_back(stroke->getControlPoint(0).thick);
  } else {
    const int samples =
        std::max(32, std::min(128, 4 * stroke->getChunkCount()));
    for (int i = 0; i <= samples; ++i) {
      double s = total * double(i) / samples;
      lengths.push_back(s);
      thickness.push_back(stroke->getThickPointAtLength(s).thick);
    }
  }
  return WidthProfile::fromSamples(lengths, thickness);
}

}  // namespace

//=============================================================================
// StrokeSelection

void StrokeSelection::enableWidthProfileCommands() {
  enableCommand(this, MI_WidthProfileUniform,
                &StrokeSelection::applyBuiltInWidthProfile,
                (int)WidthProfile::UNIFORM);
  enableCommand(this, MI_WidthProfileTaperStart,
                &StrokeSelection::applyBuiltInWidthProfile,
                (int)WidthProfile::TAPER_START);
  enableCommand(this, MI_WidthProfileTaperEnd,
                &StrokeSelection::applyBuiltInWidthProfile,
                (int)WidthProfile::TAPER_END);
  enableCommand(this, MI_WidthProfileTaperBoth,
                &StrokeSelection::applyBuiltInWidthProfile,
                (int)WidthProfile::TAPER_BOTH);
  enableCommand(this, MI_WidthProfilePulse,
                &StrokeSelection::applyBuiltInWidthProfile,
                (int)WidthProfile::PULSE);
  enableCommand(this, MI_ApplySavedWidthProfile,
                &StrokeSelection::applySavedWidthProfile);
  enableCommand(this, MI_SaveWidthProfile, &StrokeSelection::saveWidthProfile);
}

//-----------------------------------------------------------------------------

void StrokeSelection::applyBuiltInWidthProfile(int kind) {
  applyWidthProfile(WidthProfile::builtIn(kind));
}

//-----------------------------------------------------------------------------

//! Gives every selected stroke that has some thickness the shape of the
//! profile along its length, keeping its maximum thickness. Strokes without
//! thickness (hidden fill boundaries) are left alone. One undo for all.
bool StrokeSelection::applyWidthProfile(const WidthProfile::Profile &profile,
                                       double strength, bool multiply) {
  if (!m_vi || !isEditable()) return false;
  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  auto *selectionTool = dynamic_cast<VectorSelectionTool *>(tool);
  TXshSimpleLevel *level =
      TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  if (!tool || !level || strength <= 0.0) return false;

  class DrawingUndo final : public TToolUndo {
    TVectorImageP m_before, m_after;
    void restore(const TVectorImageP &image) const {
      m_level->setFrame(m_frameId, image->cloneWithGroupContext());
      notifyImageChanged();
    }
  public:
    DrawingUndo(TXshSimpleLevel *level, const TFrameId &fid,
                const TVectorImageP &before, const TVectorImageP &after)
        : TToolUndo(level, fid), m_before(before), m_after(after) {}
    void undo() const override { restore(m_before); }
    void redo() const override { restore(m_after); }
    int getSize() const override {
      size_t bytes = sizeof(*this);
      for (const auto &image : {m_before, m_after}) {
        bytes += sizeof(TVectorImage);
        for (UINT i = 0; i < image->getStrokeCount(); ++i) {
          const auto *stroke = image->getStroke(i);
          bytes += sizeof(TStroke) + size_t(stroke->getControlPointCount()) * sizeof(TThickPoint) +
                   stroke->getOpacityProfile().size() * sizeof(TPointD);
        }
        std::vector<const TRegion *> regions;
        for (UINT i = 0; i < image->getRegionCount(); ++i) regions.push_back(image->getRegion(i));
        while (!regions.empty()) {
          const auto *region = regions.back(); regions.pop_back();
          bytes += sizeof(TRegion) + size_t(region->getEdgeCount()) * sizeof(TEdge);
          for (UINT i = 0; i < region->getSubregionCount(); ++i) regions.push_back(region->getSubregion(i));
        }
      }
      return int(std::min(bytes, size_t(std::numeric_limits<int>::max())));
    }
    QString getToolName() override { return QObject::tr("Apply Width Profile"); }
  };
  struct Drawing {
    TFrameId fid;
    TVectorImageP before, after;
  };
  std::vector<TFrameId> frames;
  const bool acrossFrames = selectionTool &&
      (selectionTool->isLevelType() || selectionTool->isSelectedFramesType());
  if (acrossFrames) {
    level->getFids(frames);
    if (selectionTool->isSelectedFramesType()) {
      const auto &selected = selectionTool->getTargetFrames();
      frames.erase(std::remove_if(frames.begin(), frames.end(),
          [&](const TFrameId &fid) { return !selected.count(fid); }), frames.end());
    }
  } else frames.push_back(tool->getCurrentFid());

  // Prepare every drawing before editing any of them. Snapshots retain fills,
  // groups and native stroke identities; frame IDs deduplicate exposures.
  std::vector<Drawing> edits;
  for (const auto &fid : frames) {
    if (level->isFrameReadOnly(fid)) {
      DVGui::warning(QObject::tr("A selected drawing is read-only. No width profiles were applied."));
      return false;
    }
    TVectorImageP image = level->getFrame(fid, false);
    if (!image) continue;
    QMutexLocker lock(image->getMutex());
    std::vector<int> indices;
    if (acrossFrames)
      indices = getSelectedStrokes(*image, selectionTool->levelSelection());
    else indices.assign(m_indexes.begin(), m_indexes.end());
    ArtLayer::Filter filter(*image);
    indices.erase(std::remove_if(indices.begin(), indices.end(),
        [&](int index) {
          return index < 0 || index >= int(image->getStrokeCount()) ||
                 !image->isEnteredGroupStroke(index) || !filter.isEditable(index);
        }), indices.end());
    TVectorImageP before = image->cloneWithGroupContext();
    TVectorImageP after = image->cloneWithGroupContext();
    bool changed = false;
    for (int index : indices) {
      if (index < 0 || index >= int(after->getStrokeCount())) continue;
      TStroke *stroke = after->getStroke(index);
      const int count = stroke->getControlPointCount();
      std::vector<double> lengths(count), widths(count);
      for (int i = 0; i < count; ++i) {
        lengths[i] = stroke->getLengthAtControlPoint(i);
        widths[i] = stroke->getControlPoint(i).thick;
      }
      if (widths.empty() || *std::max_element(widths.begin(), widths.end()) <= 0) continue;
      const auto result = WidthProfile::applyBlended(profile, lengths, widths, strength, multiply);
      if (result == widths) continue;
      TStroke old(*stroke);
      for (int i = 0; i < count; ++i) {
        auto point = stroke->getControlPoint(i); point.thick = result[i];
        stroke->setControlPoint(i, point);
      }
      stroke->invalidate();
      after->notifyChangedStrokes(index, &old);
      changed = true;
    }
    if (changed) edits.push_back({fid, before, after});
  }
  if (edits.empty()) return false;
  TUndoManager::manager()->beginBlock();
  for (const auto &edit : edits) {
    auto *undo = new DrawingUndo(level, edit.fid, edit.before, edit.after);
    undo->redo();
    TUndoManager::manager()->add(undo);
  }
  TUndoManager::manager()->endBlock();
  tool->notifyImageChanged();
  return true;
}

//-----------------------------------------------------------------------------

void StrokeSelection::applySavedWidthProfile() {
  WidthProfile::Profile captured;
  if (m_vi && m_indexes.size() == 1) {
    QMutexLocker lock(m_vi->getMutex());
    const int index = *m_indexes.begin();
    if (index >= 0 && index < int(m_vi->getStrokeCount()))
      captured = profileOfStroke(m_vi->getStroke(index));
  }
  auto *tool = dynamic_cast<VectorSelectionTool *>(TTool::getApplication()->getCurrentTool()->getTool());
  const bool hasTargets = !m_indexes.empty() || (tool && (tool->isLevelType() || tool->isSelectedFramesType()));
  WidthProfile::Manager manager(savedProfilesPath().getQString(), captured,
      m_vi && hasTargets && isEditable(), QApplication::activeWindow());
  if (manager.exec() == QDialog::Accepted && manager.shouldApply())
    applyWidthProfile(manager.selectedProfile(), manager.strength(), manager.multiply());
}

//-----------------------------------------------------------------------------

void StrokeSelection::saveWidthProfile() {
  if (!m_vi) return;

  if (m_indexes.size() != 1) {
    DVGui::warning(QObject::tr("Select one stroke to save its width profile."));
    return;
  }

  WidthProfile::Profile profile;
  {
    QMutexLocker lock(m_vi->getMutex());
    int index = *m_indexes.begin();
    if (index < 0 || index >= (int)m_vi->getStrokeCount()) return;
    profile = profileOfStroke(m_vi->getStroke(index));
  }
  if (profile.empty()) {
    DVGui::warning(
        QObject::tr("The selected stroke has no thickness, so it has no "
                    "width profile to save."));
    return;
  }

  profile.name = "From selected stroke";
  WidthProfile::Manager manager(savedProfilesPath().getQString(), profile,
      isEditable(), QApplication::activeWindow());
  manager.captureNewProfile();
  if (manager.exec() == QDialog::Accepted && manager.shouldApply())
    applyWidthProfile(manager.selectedProfile(), manager.strength(), manager.multiply());
}
