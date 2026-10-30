#include "toonz/scenechecker.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/navigationtags.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/tcolumnfx.h"
#include "toonz/txshzeraryfxcolumn.h"
#include "toonz/poselibrary.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tfx.h"
#include <QObject>
#include <set>

std::vector<SceneChecker::Issue>
SceneChecker::compatibilityReport(ToonzScene *scene) {
  std::vector<Issue> result;
  if (!scene) return result;
  std::vector<std::pair<TXsheet *, std::wstring>> pending{{scene->getTopXsheet(), L""}};
  std::set<TXsheet *> visited, scheduled{scene->getTopXsheet()};
  while (!pending.empty()) {
    const auto next = pending.back();
    pending.pop_back();
    TXsheet *sheet = next.first;
    if (!sheet || !visited.insert(sheet).second) continue;
    auto add = [&](const QString &text, int column = -1, int row = -1) {
      Issue issue;
      issue.kind = Compatibility;
      issue.severity = Warning;
      issue.xsheet = sheet;
      issue.subXsheet = next.second;
      issue.column = column;
      issue.row = row;
      issue.text = text;
      result.push_back(issue);
    };
    for (const auto &tag : sheet->getNavigationTags()->getTags())
      if (!tag.m_notes.isEmpty()) {
        add(QObject::tr("Storyboard notes require a reader with extended navigation tags; "
                        "older readers may refuse this scene."), -1, tag.m_frame);
        break;
      }
    auto *tree = sheet->getStageObjectTree();
    for (int i = 0; i < tree->getStageObjectCount(); ++i) {
      auto *object = tree->getStageObject(i);
      const QString name = QString::fromStdString(object->getName());
      if (object->getPublishedChannelCount())
        add(QObject::tr("%1 has published rig channels. Readers without published-channel "
                        "support may refuse the scene.").arg(name));
      if (object->getOffsetLayerCount())
        add(QObject::tr("%1 has offset animation layers. Readers without offset-layer "
                        "support may refuse the scene.").arg(name));
      for (auto channel : {TStageObject::T_RotX, TStageObject::T_RotY,
                           TStageObject::T_FocusZ, TStageObject::T_Aperture}) {
        auto *curve = object->getParam(channel);
        if (curve && (curve->hasKeyframes() || curve->getDefaultValue() != 0.0)) {
          add(QObject::tr("%1 uses 3D tilt or camera depth controls. Readers without those channels cannot reproduce this placement or depth blur.").arg(name));
          break;
        }
      }
      bool expression = false;
      for (int channel = 0; channel < TStageObject::T_ChannelCount; ++channel) {
        TDoubleParam *curve = object->getParam(TStageObject::Channel(channel));
        if (!curve) continue;
        for (int key = 0; key < curve->getKeyframeCount(); ++key)
          if (curve->getKeyframe(key).m_expressionText.find("seexpr1:") == 0)
            expression = true;
      }
      if (expression)
        add(QObject::tr("%1 uses SeExpr animation. A build without the native SeExpr "
                        "integration cannot evaluate this animation.").arg(name));
    }
    for (int column = 0; column < sheet->getColumnCount(); ++column) {
      TXshColumn *track = sheet->getColumn(column);
      if (!track) continue;
      TXshLevelColumn *level = track->getLevelColumn();
      if (level && level->isTimeRemapUsed())
        add(QObject::tr("Column %1 uses time remapping. Readers without this feature "
                        "show the original exposure timing.").arg(column + 1), column);
      if (level && level->isColorEffectUsed())
        add(QObject::tr("Column %1 uses color effects. Readers without these parameters "
                        "may ignore the color adjustment.").arg(column + 1), column);
      if (!track->getCellColumn() || track->getSoundColumn()) continue;
      int first, last;
      if (!track->getRange(first, last)) continue;
      for (int row = first; row <= last; ++row)
        if (auto *child = sheet->getCell(row, column).getChildLevel())
          if (scheduled.insert(child->getXsheet()).second)
            pending.push_back({child->getXsheet(), child->getName()});
    }
    std::vector<TFx *> effects;
    sheet->getFxDag()->getFxs(effects);
    for (int column = 0; column < sheet->getColumnCount(); ++column)
      if (auto *track = sheet->getColumn(column))
        if (auto *zerary = track->getZeraryFxColumn())
          effects.push_back(zerary->getZeraryColumnFx()->getZeraryFx());
    std::set<TFx *> checkedEffects;
    for (TFx *fx : effects) {
      if (!fx || !checkedEffects.insert(fx).second) continue;
      const std::string type = fx->getFxType();
      if (type.find("seexpr") == 0 || type == "particleCacheFx" ||
          type == "deformerStackFx")
        add(QObject::tr("Effect %1 (%2) requires the matching Groundzero effect. "
                        "Older builds may omit it or render differently.")
                .arg(QString::fromStdWString(fx->getFxId()))
                .arg(QString::fromStdString(type)));
    }
  }
  if (!scene->getPoseLibrary()->isEmpty()) {
    Issue issue;
    issue.kind = Compatibility;
    issue.severity = Warning;
    issue.text = QObject::tr("The scene contains a pose library. Readers without the poses extension cannot preserve its base pose and saved poses.");
    result.push_back(issue);
  }
  for (const auto &tag : scene->getUnrecognizedSceneTags()) {
    Issue issue;
    issue.kind = Compatibility;
    issue.severity = Warning;
    issue.text = QObject::tr("This build skipped scene data '%1'. Saving cannot preserve that unknown feature.")
                     .arg(QString::fromStdString(tag));
    result.push_back(issue);
  }
  return result;
}
