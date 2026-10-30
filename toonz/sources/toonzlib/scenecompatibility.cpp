#include "toonz/scenechecker.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
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
#include "toonz/levelset.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "texpression.h"
#include "tfx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "ext/plasticskeletondeformation.h"
#include <QObject>
#include <set>

namespace {
bool hasSeExprAnimation(TParam *parameter) {
  std::vector<TParam *> pending{parameter};
  std::set<TParam *> visited;
  while (!pending.empty()) {
    TParam *item = pending.back();
    pending.pop_back();
    if (!item || !visited.insert(item).second) continue;
    if (auto *curve = dynamic_cast<TDoubleParam *>(item)) {
      for (int key = 0; key < curve->getKeyframeCount(); ++key) {
        const auto &frame = curve->getKeyframe(key);
        if ((frame.m_type == TDoubleKeyframe::Expression ||
             frame.m_type == TDoubleKeyframe::SimilarShape) &&
            TExpression::isSeExprText(frame.m_expressionText)) return true;
      }
    } else if (auto *set = dynamic_cast<TParamSet *>(item)) {
      for (int child = 0; child < set->getParamCount(); ++child)
        pending.push_back(set->getParam(child).getPointer());
    }
  }
  return false;
}
}

std::vector<SceneChecker::Issue>
SceneChecker::compatibilityReport(ToonzScene *scene) {
  std::vector<Issue> result;
  if (!scene) return result;
  std::vector<std::pair<TXsheet *, std::wstring>> pending{{scene->getTopXsheet(), L""}};
  std::set<TXsheet *> visited, scheduled{scene->getTopXsheet()};
  // Full-scene saving also serializes unused child levels in the Scene Cast.
  // Their extensions can prevent an older reader opening the whole file.
  for (int index = 0; index < scene->getLevelSet()->getLevelCount(); ++index) {
    auto *level = scene->getLevelSet()->getLevel(index);
    if (!level) continue;
    if (auto *child = level->getChildLevel())
      if (scheduled.insert(child->getXsheet()).second)
        pending.push_back({child->getXsheet(), child->getName()});
  }
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
      for (int channel = 0; channel < TStageObject::T_ChannelCount; ++channel)
        expression = expression || hasSeExprAnimation(
            object->getParam(TStageObject::Channel(channel)));
      for (int layer = 0; layer < object->getOffsetLayerCount(); ++layer) {
        const auto &offset = object->getOffsetLayer(layer);
        expression = expression || hasSeExprAnimation(offset.m_weight.getPointer());
        for (const auto &curve : offset.m_channels)
          expression = expression || hasSeExprAnimation(curve.getPointer());
      }
      if (auto deformation = object->getPlasticSkeletonDeformation()) {
        expression = expression || hasSeExprAnimation(deformation->skeletonIdsParam().getPointer());
        SkD::vd_iterator vertex, end;
        deformation->vertexDeformations(vertex, end);
        for (; vertex != end; ++vertex)
          for (const auto &curve : (*vertex).second->m_params)
            expression = expression || hasSeExprAnimation(curve.getPointer());
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
      if (auto *track = sheet->getColumn(column)) {
        effects.push_back(track->getFx());
        if (auto *zerary = track->getZeraryFxColumn())
          effects.push_back(zerary->getZeraryColumnFx()->getZeraryFx());
      }
    std::set<TFx *> checkedEffects;
    for (TFx *fx : effects) {
      if (!fx || !checkedEffects.insert(fx).second) continue;
      const std::string type = fx->getFxType();
      // Built-in factories register names with STD_, unlike the short names
      // in FX_PLUGIN_IDENTIFIER declarations.
      const std::string nativeType = type.compare(0, 4, "STD_") == 0 ? type.substr(4) : type;
      if (nativeType.find("seexpr") == 0 || nativeType == "particleCacheFx" ||
          nativeType == "deformerStackFx" || nativeType == "transform3DFx" ||
          nativeType == "lut3DBakeFx" || nativeType == "neuralStyleFx")
        add(QObject::tr("Effect %1 (%2) requires the matching Groundzero effect. "
                        "Older builds may omit it or render differently.")
                .arg(QString::fromStdWString(fx->getFxId()))
                .arg(QString::fromStdString(type)));
      bool expression = false;
      for (int parameter = 0; parameter < fx->getParams()->getParamCount(); ++parameter)
        expression = expression || hasSeExprAnimation(fx->getParams()->getParam(parameter));
      if (expression)
        add(QObject::tr("Effect %1 (%2) has SeExpr animation in its parameters. "
                        "A build without native SeExpr integration cannot evaluate it.")
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
