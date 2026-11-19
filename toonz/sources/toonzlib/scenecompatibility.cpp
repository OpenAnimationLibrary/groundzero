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
#include "toonz/sceneproperties.h"
#include "toonz/levelset.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "tvectorimage.h"
#include "tstroke.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "texpression.h"
#include "tfx.h"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "ext/plasticskeletondeformation.h"
#include <QObject>
#include <set>

namespace {
bool hasWorkspaceIdentity(TParam *parameter) {
  std::vector<TParam *> pending{parameter};
  std::set<TParam *> visited;
  while (!pending.empty()) {
    TParam *item = pending.back(); pending.pop_back();
    if (!item || !visited.insert(item).second) continue;
    if (auto *curve = dynamic_cast<TDoubleParam *>(item)) {
      if (!curve->getWorkspaceId().empty()) return true;
    } else if (auto *set = dynamic_cast<TParamSet *>(item)) {
      for (int i = 0; i < set->getParamCount(); ++i)
        pending.push_back(set->getParam(i).getPointer());
    }
  }
  return false;
}

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

namespace {
using SceneChecker::Certainty;
using SceneChecker::Issue;

Issue compatibilityIssue(SceneChecker::Severity severity, const char* code,
                         Certainty certainty, const QString& text) {
  Issue issue;
  issue.kind      = SceneChecker::Compatibility;
  issue.severity  = severity;
  issue.code      = code;
  issue.certainty = certainty;
  issue.text      = text;
  return issue;
}

// The rules over every sheet: navigation tags, stage objects and their
// curves, columns and effects. Records whether any curve carries a workspace
// channel identity, which is reported once for the scene.
void sheetRules(ToonzScene* scene, std::vector<Issue>& result,
                bool& hasChannelIdentity) {
  std::vector<std::pair<TXsheet *, std::wstring>> pending{{scene->getTopXsheet(), L""}};
  std::set<TXsheet*> visited, scheduled{scene->getTopXsheet()};
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
    auto add = [&](const char* code, Certainty certainty, const QString& text,
                   int column = -1, int row = -1) {
      Issue issue =
          compatibilityIssue(SceneChecker::Warning, code, certainty, text);
      issue.xsheet = sheet;
      issue.subXsheet = next.second;
      issue.column = column;
      issue.row       = row;
      result.push_back(issue);
    };
    for (const auto &tag : sheet->getNavigationTags()->getTags())
      if (!tag.m_notes.isEmpty()) {
        add("compat.storyboard_notes", SceneChecker::RefusalRisk,
            QObject::tr("Storyboard notes require a reader with extended "
                        "navigation tags; "
                        "older readers may refuse this scene."),
            -1, tag.m_frame);
        break;
      }
    auto *tree = sheet->getStageObjectTree();
    for (int i = 0; i < tree->getStageObjectCount(); ++i) {
      auto *object = tree->getStageObject(i);
      const QString name = QString::fromStdString(object->getName());
      if (object->getPublishedChannelCount())
        add("compat.published_channels", SceneChecker::RefusalRisk,
            QObject::tr("%1 has published rig channels. Readers without "
                        "published-channel "
                        "support may refuse the scene.")
                .arg(name));
      if (object->getOffsetLayerCount())
        add("compat.offset_layers", SceneChecker::RefusalRisk,
            QObject::tr(
                "%1 has offset animation layers. Readers without offset-layer "
                "support may refuse the scene.")
                .arg(name));
      for (auto channel : {TStageObject::T_RotX, TStageObject::T_RotY,
                           TStageObject::T_FocusZ, TStageObject::T_Aperture}) {
        auto *curve = object->getParam(channel);
        if (curve && (curve->hasKeyframes() || curve->getDefaultValue() != 0.0)) {
          add("compat.camera_depth_channels", SceneChecker::ConfirmedLoss,
              QObject::tr("%1 uses 3D tilt or camera depth controls. Readers "
                          "without those channels cannot reproduce this "
                          "placement or depth blur.")
                  .arg(name));
          break;
        }
      }
      bool expression = false;
      for (int channel = 0; channel < TStageObject::T_ChannelCount; ++channel) {
        TDoubleParam *curve = object->getParam(TStageObject::Channel(channel));
        if (!curve) continue;
        hasChannelIdentity = hasChannelIdentity || hasWorkspaceIdentity(curve);
        expression = expression || hasSeExprAnimation(curve);
      }
      for (int layer = 0; layer < object->getOffsetLayerCount(); ++layer) {
        const auto &offset = object->getOffsetLayer(layer);
        expression = expression || hasSeExprAnimation(offset.m_weight.getPointer());
        hasChannelIdentity = hasChannelIdentity || hasWorkspaceIdentity(offset.m_weight.getPointer());
        for (const auto &curve : offset.m_channels) {
          expression = expression || hasSeExprAnimation(curve.getPointer());
          hasChannelIdentity = hasChannelIdentity || hasWorkspaceIdentity(curve.getPointer());
        }
      }
      if (auto deformation = object->getPlasticSkeletonDeformation()) {
        expression = expression || hasSeExprAnimation(deformation->skeletonIdsParam().getPointer());
        hasChannelIdentity = hasChannelIdentity || hasWorkspaceIdentity(deformation->skeletonIdsParam().getPointer());
        SkD::vd_iterator vertex, end;
        deformation->vertexDeformations(vertex, end);
        for (; vertex != end; ++vertex)
          for (const auto &curve : (*vertex).second->m_params) {
            expression = expression || hasSeExprAnimation(curve.getPointer());
            hasChannelIdentity = hasChannelIdentity || hasWorkspaceIdentity(curve.getPointer());
          }
      }
      if (expression)
        add("compat.seexpr_stage_animation", SceneChecker::ConfirmedLoss,
            QObject::tr(
                "%1 uses SeExpr animation. A build without the native SeExpr "
                "integration cannot evaluate this animation.")
                .arg(name));
    }
    for (int column = 0; column < sheet->getColumnCount(); ++column) {
      TXshColumn *track = sheet->getColumn(column);
      if (!track) continue;
      TXshLevelColumn *level = track->getLevelColumn();
      if (level && level->isTimeRemapUsed())
        add("compat.time_remap", SceneChecker::ConfirmedLoss,
            QObject::tr(
                "Column %1 uses time remapping. Readers without this feature "
                "show the original exposure timing.")
                .arg(column + 1),
            column);
      if (level && level->isColorEffectUsed())
        add("compat.color_effects", SceneChecker::ConfirmedLoss,
            QObject::tr("Column %1 uses color effects. Readers without these "
                        "parameters "
                        "may ignore the color adjustment.")
                .arg(column + 1),
            column);
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
    std::vector<std::pair<TFx *, QString>> pendingEffects;
    for (TFx *fx : effects) pendingEffects.push_back({fx, QString()});
    for (size_t index = 0; index < pendingEffects.size(); ++index) {
      TFx *fx = pendingEffects[index].first;
      const QString macroPath = pendingEffects[index].second;
      if (!fx || !checkedEffects.insert(fx).second) continue;
      for (int parameter = 0; parameter < fx->getParams()->getParamCount(); ++parameter)
        hasChannelIdentity = hasChannelIdentity || hasWorkspaceIdentity(fx->getParams()->getParam(parameter));
      const std::string type = fx->getFxType();
      QString name = QString::fromStdWString(fx->getFxId());
      if (name.isEmpty()) name = QString::fromStdWString(fx->getName());
      if (auto *macro = dynamic_cast<TMacroFx *>(fx)) {
        const QString childPath = macroPath.isEmpty() ? name : macroPath + " / " + name;
        for (const auto &child : macro->getFxs())
          pendingEffects.push_back({child.getPointer(), childPath});
        // Macro parameters alias their member parameters. Report the actual
        // owning effect once, with its macro path, instead of duplicate copies.
        continue;
      }
      if (!macroPath.isEmpty()) name += QObject::tr(" [macro %1]").arg(macroPath);
      // Built-in factories register names with STD_, unlike the short names
      // in FX_PLUGIN_IDENTIFIER declarations.
      const std::string nativeType = type.compare(0, 4, "STD_") == 0 ? type.substr(4) : type;
      if (nativeType.find("seexpr") == 0 || nativeType == "particleCacheFx" ||
          nativeType == "deformerStackFx" || nativeType == "transform3DFx" ||
          nativeType == "lut3DBakeFx" || nativeType == "neuralStyleFx")
        add("compat.groundzero_effect", SceneChecker::UnknownSupport,
            QObject::tr(
                "Effect %1 (%2) requires the matching Groundzero effect. "
                "Older builds may omit it or render differently.")
                .arg(name)
                .arg(QString::fromStdString(type)));
      bool expression = false;
      for (int parameter = 0; parameter < fx->getParams()->getParamCount(); ++parameter)
        expression = expression || hasSeExprAnimation(fx->getParams()->getParam(parameter));
      if (expression)
        add("compat.seexpr_effect_parameters", SceneChecker::ConfirmedLoss,
            QObject::tr(
                "Effect %1 (%2) has SeExpr animation in its parameters. "
                "A build without native SeExpr integration cannot evaluate it.")
                .arg(name)
                .arg(QString::fromStdString(type)));
    }
  }
}

Issue levelIssue(TXshLevel* level, SceneChecker::Severity severity,
                 const char* code, Certainty certainty, const QString& text) {
  Issue issue     = compatibilityIssue(severity, code, certainty, text);
  issue.levelName = level->getName();
  return issue;
}

// A level's organization and authoring metadata, which reads no drawing.
void levelMetadataRules(TXshLevel* level, std::vector<Issue>& result) {
  const QString name = QString::fromStdWString(level->getName());
  if (!level->getCastTags().isEmpty() || level->hasCastCover())
    result.push_back(levelIssue(
        level, SceneChecker::Info, "compat.cast_organization",
        SceneChecker::ConfirmedLoss,
        QObject::tr(
            "%1 has Scene Cast tags or a saved thumbnail cover. "
            "Older readers may discard this organization metadata when saving; "
            "the drawing geometry and exposure timing remain unchanged.")
            .arg(name)));
  if (level->getLibraryProvenance().relationship !=
      TXshLevel::LibraryProvenance::NotRecorded)
    result.push_back(levelIssue(
        level, SceneChecker::Info, "compat.library_provenance",
        SceneChecker::ConfirmedLoss,
        QObject::tr(
            "%1 has recorded Library source attribution. Older readers "
            "may discard its item identity, revision and ownership "
            "relationship when saving; "
            "filesystem location alone cannot reconstruct this information.")
            .arg(name)));
  auto* simple = level->getSimpleLevel();
  if (simple && simple->getType() == PLI_XSHLEVEL &&
      !simple->getMorphHints().empty())
    result.push_back(
        levelIssue(level, SceneChecker::Info, "compat.morph_hints",
                   SceneChecker::ConfirmedLoss,
                   QObject::tr("%1 has saved morph correspondence hints. "
                               "Readers without morph-hint support may discard "
                               "these authoring hints "
                               "when saving; existing drawings remain intact.")
                       .arg(name)));
}

// The rules of the scene as a whole, after every sheet and level.
void sceneRules(ToonzScene* scene, bool hasChannelIdentity,
                std::vector<Issue>& result) {
  if (hasChannelIdentity)
    result.push_back(
        compatibilityIssue(SceneChecker::Info, "compat.channel_identity",
                           SceneChecker::ConfirmedLoss,
                           QObject::tr("Function Editor channel identities are "
                                       "stored with animation parameters. "
                                       "Older readers may discard these "
                                       "optional identities when saving, which "
                                       "can disconnect saved channel sets; "
                                       "animation values remain unchanged.")));
  if (!scene->getProperties()->getCastCollections().isEmpty())
    result.push_back(compatibilityIssue(
        SceneChecker::Warning, "compat.cast_collections",
        SceneChecker::RefusalRisk,
        QObject::tr(
            "The scene contains named Scene Cast collections. Readers without "
            "the castCollections scene-property extension may refuse to open "
            "it. Keep the original "
            "scene before opening or saving with another build.")));
  if (scene->getLevelSet()->getLevelCount() > 0)
    result.push_back(compatibilityIssue(
        SceneChecker::Info, "compat.cast_asset_identities",
        SceneChecker::ConfirmedLoss,
        QObject::tr(
            "Scene Cast asset identities and saved-source states are stored as "
            "level attributes. Older readers may discard them when saving, "
            "disconnecting named "
            "collection membership or losing the distinction between new and "
            "missing assets.")));
  if (!scene->getPoseLibrary()->isEmpty())
    result.push_back(compatibilityIssue(
        SceneChecker::Warning, "compat.pose_library",
        SceneChecker::ConfirmedLoss,
        QObject::tr(
            "The scene contains a pose library. Readers without the poses "
            "extension cannot preserve its base pose and saved poses.")));
  for (const auto& tag : scene->getUnrecognizedSceneTags())
    result.push_back(compatibilityIssue(
        SceneChecker::Warning, "compat.unrecognized_scene_data",
        SceneChecker::ConfirmedLoss,
        QObject::tr("This build skipped scene data '%1'. Saving cannot "
                    "preserve that unknown feature.")
            .arg(QString::fromStdString(tag))));
}
}  // namespace

std::vector<SceneChecker::Issue> SceneChecker::sceneCompatibilityReport(
    ToonzScene* scene) {
  std::vector<Issue> result;
  if (!scene) return result;
  bool hasChannelIdentity = false;
  sheetRules(scene, result, hasChannelIdentity);
  for (int index = 0; index < scene->getLevelSet()->getLevelCount(); ++index)
    if (TXshLevel* level = scene->getLevelSet()->getLevel(index))
      levelMetadataRules(level, result);
  sceneRules(scene, hasChannelIdentity, result);
  return result;
}

int SceneChecker::opacityEnvelopeDrawing(TXshSimpleLevel* level,
                                         const TFrameId& fid) {
  // Loading for inspection does not mark drawings modified or rewrite their
  // native metadata.
  try {
    TVectorImageP drawing = level->getFrame(fid, false);
    if (!drawing) return -1;
    for (unsigned int stroke = 0; stroke < drawing->getStrokeCount(); ++stroke)
      if (!drawing->getStroke(stroke)->getOpacityProfile().empty()) return 1;
    return 0;
  } catch (...) {
    return -1;
  }
}

std::vector<SceneChecker::Issue> SceneChecker::levelDrawingCompatibility(
    TXshLevel* level, int profiledDrawings, bool incomplete) {
  std::vector<Issue> result;
  const QString name = QString::fromStdWString(level->getName());
  if (profiledDrawings)
    result.push_back(levelIssue(
        level, Warning, "compat.opacity_envelopes", RefusalRisk,
        QObject::tr(
            "%1 has opacity envelopes in %2 drawings. "
            "These drawings require PLI 150.1; older PLI readers cannot open "
            "them. "
            "Keep the native files for editing, or render images for viewers "
            "without opacity-envelope support.")
            .arg(name)
            .arg(profiledDrawings)));
  if (incomplete)
    result.push_back(levelIssue(
        level, Warning, "compat.level_not_inspected", UnknownSupport,
        QObject::tr("Some drawings in %1 could not be inspected. "
                    "This compatibility report is incomplete for that level.")
            .arg(name)));
  return result;
}

std::vector<SceneChecker::Issue> SceneChecker::compatibilityReport(
    ToonzScene* scene) {
  std::vector<Issue> result;
  if (!scene) return result;
  bool hasChannelIdentity = false;
  sheetRules(scene, result, hasChannelIdentity);
  // The scene cast also includes spare drawings, which must remain portable
  // even when they are not exposed in a current xsheet.
  for (int index = 0; index < scene->getLevelSet()->getLevelCount(); ++index) {
    TXshLevel *level = scene->getLevelSet()->getLevel(index);
    if (!level) continue;
    levelMetadataRules(level, result);
    auto *simple = level->getSimpleLevel();
    if (!simple || simple->getType() != PLI_XSHLEVEL) continue;
    int profiledDrawings = 0;
    bool incomplete = false;
    for (const auto &fid : simple->getFids()) {
      const int profiled = opacityEnvelopeDrawing(simple, fid);
      if (profiled > 0) ++profiledDrawings;
      if (profiled < 0) incomplete = true;
    }
    const auto drawn =
        levelDrawingCompatibility(level, profiledDrawings, incomplete);
    result.insert(result.end(), drawn.begin(), drawn.end());
  }
  sceneRules(scene, hasChannelIdentity, result);
  return result;
}

QString SceneChecker::certaintyName(Certainty certainty) {
  switch (certainty) {
  case ConfirmedLoss:
    return "confirmed_loss";
  case RefusalRisk:
    return "refusal_risk";
  case UnknownSupport:
    return "unknown_support";
  }
  return "unknown_support";
}
