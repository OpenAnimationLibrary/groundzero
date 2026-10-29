// Width profile commands of the vector selection: give the selected strokes
// one of the built-in profiles or a saved one, and save the profile of a
// stroke. The arithmetic is in tools/widthprofile.h.

#include "tools/strokeselection.h"
#include "tools/widthprofile.h"

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
#include <vector>

using namespace ToolUtils;

//=============================================================================
namespace {

//! The file of the saved profiles, in the user's config folder: one profile
//! per line (see WidthProfile::toText).
TFilePath savedProfilesPath() {
  return TEnv::getConfigDir() + "width_profiles.txt";
}

std::vector<WidthProfile::Profile> loadSavedProfiles() {
  QFile file(savedProfilesPath().getQString());
  if (!file.open(QIODevice::ReadOnly))
    return std::vector<WidthProfile::Profile>();
  QByteArray data = file.readAll();
  return WidthProfile::parseProfiles(
      std::string(data.constData(), (size_t)data.size()));
}

bool storeSavedProfiles(const std::vector<WidthProfile::Profile> &profiles) {
  TFilePath path = savedProfilesPath();
  TSystem::touchParentDir(path);
  QFile file(path.getQString());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  std::string text =
      "# Width profiles: name, a tab, then position:ratio pairs.\n" +
      WidthProfile::formatProfiles(profiles);
  return file.write(text.c_str(), (qint64)text.size()) == (qint64)text.size();
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
bool StrokeSelection::applyWidthProfile(const WidthProfile::Profile &profile) {
  if (!m_vi || m_indexes.empty()) return false;

  if (!isEditable()) {
    DVGui::error(
        QObject::tr("The selection cannot be updated. It is not editable."));
    return false;
  }

  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  TXshSimpleLevel *level =
      TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  if (!tool || !level) return false;

  const TFrameId fid = tool->getCurrentFid();

  std::vector<int> changed;
  std::vector<TStroke *> oldStrokes;
  std::vector<UndoModifyStroke *> undos;

  {
    QMutexLocker lock(m_vi->getMutex());
    for (int index : m_indexes) {
      if (index < 0 || index >= (int)m_vi->getStrokeCount()) continue;
      TStroke *stroke = m_vi->getStroke(index);

      // The maximum thickness of the stroke, over all its control points
      const int count = stroke->getControlPointCount();
      double maxThick = 0.0;
      for (int i = 0; i < count; ++i)
        maxThick = std::max(maxThick, stroke->getControlPoint(i).thick);
      if (!(maxThick > 0.0)) continue;

      std::vector<double> lengths(count);
      for (int i = 0; i < count; ++i)
        lengths[i] = stroke->getLengthAtControlPoint(i);
      const std::vector<double> thickness =
          WidthProfile::mapToThickness(profile, lengths, maxThick);

      // The undo records the stroke before it is changed
      undos.push_back(new UndoModifyStroke(level, fid, index));
      oldStrokes.push_back(new TStroke(*stroke));
      changed.push_back(index);

      for (int i = 0; i < count; ++i) {
        TThickPoint cp = stroke->getControlPoint(i);
        stroke->setControlPoint(i, TThickPoint(cp.x, cp.y, thickness[i]));
      }
      stroke->invalidate();
    }
  }

  if (changed.empty()) {
    DVGui::warning(
        QObject::tr("None of the selected strokes has thickness. A width "
                    "profile can only be applied to brush strokes."));
    return false;
  }

  // One undo step for all the strokes
  TUndoManager::manager()->beginBlock();
  for (UndoModifyStroke *undo : undos) TUndoManager::manager()->add(undo);
  TUndoManager::manager()->endBlock();

  {
    QMutexLocker lock(m_vi->getMutex());
    m_vi->notifyChangedStrokes(changed, oldStrokes);
  }
  for (TStroke *old : oldStrokes) delete old;

  m_updateSelectionBBox = true;
  tool->notifyImageChanged();
  m_updateSelectionBBox = false;
  return true;
}

//-----------------------------------------------------------------------------

void StrokeSelection::applySavedWidthProfile() {
  if (!m_vi || m_indexes.empty()) return;

  std::vector<WidthProfile::Profile> profiles = loadSavedProfiles();
  if (profiles.empty()) {
    DVGui::info(
        QObject::tr("There is no saved width profile yet. Select a stroke "
                    "and use Save Width Profile From Selected Stroke."));
    return;
  }

  QStringList names;
  for (const WidthProfile::Profile &profile : profiles)
    names << QString::fromUtf8(profile.name.c_str());

  bool accepted = false;
  QString name  = QInputDialog::getItem(
      QApplication::activeWindow(), QObject::tr("Apply Saved Width Profile"),
      QObject::tr("Width profile:"), names, 0, false, &accepted);
  if (!accepted) return;

  int row = names.indexOf(name);
  if (row < 0 || row >= (int)profiles.size()) return;
  applyWidthProfile(profiles[row]);
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

  QWidget *parent = QApplication::activeWindow();
  QString name;
  while (name.isEmpty()) {
    bool accepted = false;
    QString input = QInputDialog::getText(
        parent, QObject::tr("Save Width Profile"), QObject::tr("Profile name:"),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted) return;
    // A tab separates the fields of a saved line, and a line starting with
    // '#' is read as a comment: neither may come from the name
    name = input.trimmed().replace('\t', ' ');
    if (name.isEmpty())
      DVGui::warning(QObject::tr("Please enter a profile name."));
    else if (name.startsWith('#')) {
      DVGui::warning(QObject::tr("A profile name cannot start with \"#\"."));
      name.clear();
    }
  }
  if (name.size() > 64) name.truncate(64);
  profile.name = name.toUtf8().toStdString();

  std::vector<WidthProfile::Profile> profiles = loadSavedProfiles();
  WidthProfile::addOrReplace(profiles, profile);
  if (!storeSavedProfiles(profiles))
    DVGui::error(QObject::tr("The width profile could not be saved."));
}
