#include "automation.h"
#include "automation_scene_host.h"

#include "tdoubleparam.h"
#include "tfx.h"
#include "tnotanimatableparam.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "trasterfx.h"
#include "tspectrumparam.h"
#include "ttonecurveparam.h"

#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace GroundzeroAutomation {
namespace {

// The admission class of every effect type the application registers. A type
// added to the registry has no row here until someone decides its class, and
// the walk below fails on it with that instruction.
//
// How each class is recognized (automation_scene_content.cpp):
//  builtin_admitted / builtin_other  declared by the application's own code,
//      runs no outside program and reads no file; admitted when the detached
//      render profile covers it.
//  file_resource  declared by the application but reads a file the scene names.
//  external_code  an external program, a declaration made at run time, or an
//      effect that runs a program the scene supplies.
//  xsheet / output / column / macro / unknown  graph structure, not effects.
const std::map<std::string, std::set<std::string>>& deliberateClasses() {
  static const std::map<std::string, std::set<std::string>> classes = {
      {"builtin_admitted",
       {"STD_backlitFx",
        "STD_blurFx",
        "STD_bodyHighLightFx",
        "STD_brightContFx",
        "STD_channelMixerFx",
        "STD_directionalBlurFx",
        "STD_embossFx",
        "STD_erodeDilateFx",
        "STD_gammaFx",
        "STD_glowFx",
        "STD_hsvScaleFx",
        "STD_iwa_AdjustExposureFx",
        "STD_iwa_BloomFx",
        "STD_iwa_LinearGradientFx",
        "STD_lightSpotFx",
        "STD_lightWrapFx",
        "STD_localBlurFx",
        "STD_matteChokeFx",
        "STD_posterizeFx",
        "STD_radialGradientFx",
        "STD_sharpenFx",
        "STD_solarizeFx",
        "STD_targetSpotFx",
        "blendFx",
        "colorCardFx",
        "inFx",
        "invertFx",
        "multFx",
        "outFx",
        "overFx",
        "screenFx"}},
      {"builtin_other",
       {"STD_adjustLevelsFx",
        "STD_alphaDistanceGradientFx",
        "STD_artContourFx",
        "STD_autoMatteFx",
        "STD_autoPatchFx",
        "STD_blendTzFx",
        "STD_boneDeformerFx",
        "STD_burstLinesFx",
        "STD_calligraphicFx",
        "STD_castShadowFx",
        "STD_celBoilFx",
        "STD_celBoundaryBlurFx",
        "STD_celColorExpandFx",
        "STD_celColorKeepFx",
        "STD_celColorKeyFx",
        "STD_celDirectionalBlurFx",
        "STD_celEdgeLinesFx",
        "STD_celRadialBlurFx",
        "STD_celSmoother2Fx",
        "STD_celSmootherFx",
        "STD_cloudsFx",
        "STD_colorEmbossFx",
        "STD_colorRaylitFx",
        "STD_colourOverrideFx",
        "STD_colourScriptFx",
        "STD_cornerPinFx",
        "STD_curveDeformerFx",
        "STD_deformerStackFx",
        "STD_despeckleFx",
        "STD_diamondGradientFx",
        "STD_dissolveFx",
        "STD_dotScreenFx",
        "STD_edgeExtendFx",
        "STD_envelopeDeformerFx",
        "STD_envelopeDeformerPlusFx",
        "STD_externalPaletteFx",
        "STD_fadeFx",
        "STD_flowDisplaceFx",
        "STD_fourPointsGradientFx",
        "STD_freeDistortFx",
        "STD_freeFormDeformerFx",
        "STD_hsvKeyFx",
        "STD_imagePlaneFx",
        "STD_inoAddFx",
        "STD_inoBlurFx",
        "STD_inoChannelSelectorFx",
        "STD_inoColorBurnFx",
        "STD_inoColorDodgeFx",
        "STD_inoCrossDissolveFx",
        "STD_inoDarkenFx",
        "STD_inoDarkerColorFx",
        "STD_inoDensityFx",
        "STD_inoDivideFx",
        "STD_inoFogFx",
        "STD_inoHardLightFx",
        "STD_inoHardMixFx",
        "STD_inoLevelAutoFx",
        "STD_inoLevelMasterFx",
        "STD_inoLevelrgbaFx",
        "STD_inoLightenFx",
        "STD_inoLighterColorFx",
        "STD_inoLineBlurFx",
        "STD_inoLinearBurnFx",
        "STD_inoLinearDodgeFx",
        "STD_inoLinearLightFx",
        "STD_inoMaxMinFx",
        "STD_inoMedianFilterFx",
        "STD_inoMedianFx",
        "STD_inoMotionBlurFx",
        "STD_inoMotionWindFx",
        "STD_inoMultiplyFx",
        "STD_inoNegateFx",
        "STD_inoOverFx",
        "STD_inoOverlayFx",
        "STD_inoPinLightFx",
        "STD_inoRadialBlurFx",
        "STD_inoScreenFx",
        "STD_inoSoftLightFx",
        "STD_inoSpinBlurFx",
        "STD_inoSubtractFx",
        "STD_inoVividLightFx",
        "STD_inoWarphvFx",
        "STD_inohlsAddFx",
        "STD_inohlsAdjustFx",
        "STD_inohlsNoiseFx",
        "STD_inohsvAddFx",
        "STD_inohsvAdjustFx",
        "STD_inohsvNoiseFx",
        "STD_inopnCloudsFx",
        "STD_iwa_BarrelDistortFx",
        "STD_iwa_BokehAdvancedFx",
        "STD_iwa_BokehFx",
        "STD_iwa_BokehRefFx",
        "STD_iwa_CorridorGradientFx",
        "STD_iwa_DirectionalBlurFx",
        "STD_iwa_FloorBumpFx",
        "STD_iwa_FlowBlurFx",
        "STD_iwa_FlowPaintBrushFx",
        "STD_iwa_FractalNoiseFx",
        "STD_iwa_GlareFx",
        "STD_iwa_GradientWarpFx",
        "STD_iwa_MotionBlurCompFx",
        "STD_iwa_MotionFlowFx",
        "STD_iwa_PNPerspectiveFx",
        "STD_iwa_PerspectiveDistortFx",
        "STD_iwa_RainbowFx",
        "STD_iwa_SmootherFx",
        "STD_iwa_SoapBubbleFx",
        "STD_iwa_SpectrumFx",
        "STD_iwa_SpinGradientFx",
        "STD_iwa_TangentFlowFx",
        "STD_iwa_TextFx",
        "STD_iwa_TileFx",
        "STD_iwa_TiledParticlesFx",
        "STD_iwa_TimeCodeFx",
        "STD_kaleidoFx",
        "STD_lightShadingFx",
        "STD_linearGradientFx",
        "STD_linearWaveFx",
        "STD_localTransparencyFx",
        "STD_mosaicFx",
        "STD_motionBlurFx",
        "STD_motionLinesFx",
        "STD_multiLinearGradientFx",
        "STD_multiRadialGradientFx",
        "STD_multiToneFx",
        "STD_naru_LazyBrushFx",
        "STD_noiseFx",
        "STD_nothingFx",
        "STD_outBorderFx",
        "STD_paletteFilterFx",
        "STD_particlesFx",
        "STD_perlinNoiseFx",
        "STD_premultiplyFx",
        "STD_printProcessFx",
        "STD_radialBlurFx",
        "STD_randomWaveFx",
        "STD_raylitFx",
        "STD_rgbKeyFx",
        "STD_rgbmCutFx",
        "STD_rgbmFadeFx",
        "STD_rgbmScaleFx",
        "STD_rippleFx",
        "STD_rotationalBlurFx",
        "STD_roughStrokeFx",
        "STD_saltpepperNoiseFx",
        "STD_seexprAttributeFx",
        "STD_shapeAwareDeformerFx",
        "STD_spiralFx",
        "STD_squareGradientFx",
        "STD_starGlowFx",
        "STD_textureFx",
        "STD_threePointLightFx",
        "STD_tileFx",
        "STD_toneCurveFx",
        "STD_toneHighlightFx",
        "STD_transform3DFx",
        "STD_unmultiplyFx",
        "STD_warpFx",
        "STD_watercolourFx",
        "addFx",
        "affineFx",
        "atopFx",
        "checkBoardFx",
        "colorBurnFx",
        "colorDodgeFx",
        "columnColorEffectFx",
        "columnColorFilterFx",
        "columnDepthFx",
        "maxFx",
        "minFx",
        "naAffineFx",
        "plasticDeformerFx",
        "subFx",
        "timeShuffleFx",
        "trFx"}},
      {"file_resource",
       {"STD_glbModelFx", "STD_lut3DBakeFx", "STD_openExrAovFx",
        "STD_particleCacheFx", "STD_referenceColorMatchFx"}},
      {"external_code",
       {"STD_neuralStyleFx", "STD_seexprColorFx", "STD_seexprGeneratorFx",
        "STD_seexprSurfaceFx", "externalProgramFx"}},
      {"xsheet", {"Toonz_xsheetFx"}},
      {"output", {"Toonz_outputFx"}},
      {"column", {"Toonz_columnFx", "Toonz_paletteColumnFx"}},
      {"macro", {"macroFx"}},
      {"unknown", {"Toonz_zeraryColumnFx"}},
  };
  return classes;
}

// String parameters of built-in effects, each read once: none names a file, a
// program or a script. Style index lists, a name prefix, a drawn text and
// attribute names.
const std::set<std::string>& reviewedStrings() {
  static const std::set<std::string> reviewed = {
      "STD_artContourFx/Color_Index",
      "STD_blendTzFx/Color_Index",
      "STD_calligraphicFx/Color_Index",
      "STD_colourScriptFx/keepPrefix",
      "STD_cornerPinFx/indexes",
      "STD_deformerStackFx/a_paint",
      "STD_deformerStackFx/b_paint",
      "STD_deformerStackFx/c_paint",
      "STD_iwa_TextFx/text",
      "STD_paletteFilterFx/indexes",
      "STD_seexprAttributeFx/name",
      "STD_seexprAttributeFx/namespace",
      "STD_textureFx/indexes"};
  return reviewed;
}

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

TFxP create(const std::string& type) {
  std::unique_ptr<TPersist> object(TPersist::create(type));
  auto* fx = dynamic_cast<TFx*>(object.get());
  require(fx, "The registry type " + type + " does not create an effect.");
  object.release();
  return TFxP(fx);
}

std::string classOf(const TFxP& fx) {
  return fxNodeClass(fx.getPointer()).toStdString();
}

std::map<std::string, std::string> deliberateTable() {
  std::map<std::string, std::string> table;
  for (const auto& [kind, types] : deliberateClasses())
    for (const std::string& type : types) {
      require(table.emplace(type, kind).second,
              type + " has two classes in the table of this test.");
    }
  return table;
}

void checkEveryRegisteredTypeHasADeliberateClass() {
  const std::map<std::string, std::string> deliberate = deliberateTable();
  std::vector<TFxInfo> registered;
  TFx::listFxs(registered);
  std::string problems;
  std::set<std::string> seen;
  for (const TFxInfo& info : registered) {
    seen.insert(info.m_name);
    const std::string actual = classOf(create(info.m_name));
    const auto found         = deliberate.find(info.m_name);
    if (found == deliberate.end())
      problems += "- " + info.m_name + " is registered and classified " +
                  actual +
                  ", but no class is chosen for it. Decide whether it takes a "
                  "program or a file, then add it to the table.\n";
    else if (found->second != actual)
      problems += "- " + info.m_name + " is classified " + actual +
                  ", the table says " + found->second + ".\n";
  }
  for (const auto& [type, kind] : deliberate)
    if (!seen.count(type))
      problems += "- " + type + " is in the table but not registered.\n";
  require(problems.empty(),
          "Effect classes differ from the table:\n" + problems);
}

bool inertKind(TParam* param) {
  return dynamic_cast<TDoubleParam*>(param) ||
         dynamic_cast<TIntParam*>(param) || dynamic_cast<TBoolParam*>(param) ||
         dynamic_cast<TEnumParam*>(param) ||
         dynamic_cast<TNADoubleParam*>(param) ||
         dynamic_cast<TPointParam*>(param) ||
         dynamic_cast<TPixelParam*>(param) ||
         dynamic_cast<TRangeParam*>(param) ||
         dynamic_cast<TSpectrumParam*>(param) ||
         dynamic_cast<TToneCurveParam*>(param) ||
         dynamic_cast<TFontParam*>(param);
}

// A built-in effect holds numbers, colors, choices and reviewed strings only:
// a file path or an unreviewed string would be a way in for outside input.
void checkBuiltInEffectsTakeNoOutsideInput() {
  std::vector<TFxInfo> registered;
  TFx::listFxs(registered);
  std::string problems;
  for (const TFxInfo& info : registered) {
    const TFxP fx          = create(info.m_name);
    const std::string kind = classOf(fx);
    if (kind != "builtin_admitted" && kind != "builtin_other") continue;
    const auto* declaration =
        dynamic_cast<const TFxDeclaration*>(fx->getDeclaration());
    if (!declaration || !declaration->isCompiledIn())
      problems += "- " + info.m_name +
                  " is built in without a compiled-in declaration.\n";
    TParamContainer* params = fx->getParams();
    for (int index = 0; index < params->getParamCount(); ++index) {
      TParam* param          = params->getParam(index);
      const std::string name = info.m_name + "/" + params->getParamName(index);
      if (inertKind(param)) continue;
      if (dynamic_cast<TStringParam*>(param) && reviewedStrings().count(name))
        continue;
      problems += "- " + name +
                  " is a parameter of a built-in effect that is neither a "
                  "number, a color, a choice nor a reviewed string.\n";
    }
  }
  require(problems.empty(),
          "Built-in effects with unreviewed input:\n" + problems);
}

// A probe declared the way a plug-in or a shader effect is: by a TFxDeclaration
// made at run time, not by the application's own code. Its name begins with
// STD_, so a name test would take it for built in.
class RuntimeProbeFx final : public TRasterFx {
  TRasterFxPort m_input;

public:
  RuntimeProbeFx() { addInputPort("Source", m_input); }
  const TPersistDeclaration* getDeclaration() const override;
  std::string getPluginId() const override { return std::string(); }
  bool canHandle(const TRenderSettings&, double) override { return true; }
  bool doGetBBox(double, TRectD&, const TRenderSettings&) override {
    return false;
  }
  void doCompute(TTile&, double, const TRenderSettings&) override {}
};

class RuntimeDeclaration final : public TFxDeclaration {
public:
  explicit RuntimeDeclaration(const char* id)
      : TFxDeclaration(TFxInfo(id, false)) {}
  TPersist* create() const override { return new RuntimeProbeFx; }
};

// Registered on first use, after the registry walk.
const TPersistDeclaration* RuntimeProbeFx::getDeclaration() const {
  static const RuntimeDeclaration declaration("STD_gzRuntimeProbeFx");
  return &declaration;
}

// An effect no registry knows: it has no declaration at all.
class UndeclaredProbeFx final : public TRasterFx {
public:
  const TPersistDeclaration* getDeclaration() const override { return nullptr; }
  std::string getPluginId() const override { return std::string(); }
  bool canHandle(const TRenderSettings&, double) override { return true; }
  bool doGetBBox(double, TRectD&, const TRenderSettings&) override {
    return false;
  }
  void doCompute(TTile&, double, const TRenderSettings&) override {}
};

void checkRunTimeDeclarationsAreExternal() {
  const TFxP runtime(new RuntimeProbeFx);
  const std::string type = runtime->getFxType();
  require(TFx::getFxInfo(type).m_name == type,
          "The probe is registered like any plug-in or shader effect.");
  require(
      classOf(runtime) == "external_code",
      "An effect declared at run time is external code, whatever its name.");
  const TFxP undeclared(new UndeclaredProbeFx);
  require(classOf(undeclared) == "external_code",
          "An effect without a declaration is external code.");
}

}  // namespace

void runAutomationFxAdmissionTests() {
  checkEveryRegisteredTypeHasADeliberateClass();
  checkBuiltInEffectsTakeNoOutsideInput();
  checkRunTimeDeclarationsAreExternal();
  std::cout << "automation-fx-admission-test: registry classes, built-in "
               "inputs and run-time declarations PASS\n";
}

}  // namespace GroundzeroAutomation
