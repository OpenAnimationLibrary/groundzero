#pragma once

#include "toonz/particlebake.h"
#include "toonz/scenefx.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tcolumnfx.h"
#include "toonz/txsheet.h"
#include "toonz/txshzeraryfxcolumn.h"
#include "tbasefx.h"
#include "tzeraryfx.h"

#include <QCoreApplication>

#include <cmath>
#include <vector>

// Shared by the dialog and its scene regression. Column depth remains outside
// the recorded native recipe and is applied by the same column during playback.
namespace ParticleBakeScene {
struct Source {
  TRasterFxP native;
  TAffine placement;
  std::vector<TRasterFxP> columnEffects;
};

inline bool validAffine(const TAffine &a) {
  return std::isfinite(a.a11) && std::isfinite(a.a12) &&
         std::isfinite(a.a13) && std::isfinite(a.a21) &&
         std::isfinite(a.a22) && std::isfinite(a.a23) &&
         std::isfinite(a.det()) && a.det() != 0;
}

inline bool prepare(ToonzScene *scene, TXsheet *xsheet, const TFxP &selected,
                    int frame, Source &result, QString &error) {
  result = Source();
  TFxP root = selected;
  TZeraryColumnFx *column = nullptr;
  if (auto zerary = dynamic_cast<TZeraryFx *>(selected.getPointer()))
    if ((column = zerary->getColumnFx())) root = column;
  TFxP built = buildSceneFx(scene, frame, xsheet, root, BSFX_DEFAULT_TR, false);
  while (auto affine = dynamic_cast<NaAffineFx *>(built.getPointer())) {
    result.placement = result.placement * affine->getPlacement(frame);
    built = affine->getInputPort(0)->getFx();
  }
  if (!built) return true;  // Empty exposure or a column behind the camera.

  const bool hasColumnDepth = column && column->getColumn() &&
      !column->getColumn()->isDepthExcluded() && xsheet == scene->getTopXsheet() &&
      scene->getProperties()->getMultiplaneDepth().enabled;
  if (hasColumnDepth) {
    if (auto color = dynamic_cast<ColumnDepthFx *>(built.getPointer())) {
      result.columnEffects.push_back(TRasterFxP(color));
      built = color->getInputPort(0)->getFx();
    }
    if (built && built->getFxType() == "STD_blurFx" &&
        built->getInputPortCount() == 1) {
      result.columnEffects.push_back(TRasterFxP(dynamic_cast<TRasterFx *>(built.getPointer())));
      built = built->getInputPort(0)->getFx();
    }
  }
  result.native = dynamic_cast<TRasterFx *>(built.getPointer());
  // Only accept the selected native clone behind the two generated depth
  // wrappers; never follow arbitrary effect inputs to find a generator.
  if (!result.native || !ParticleBake::isSupported(built.getPointer()) ||
      built->getFxType() != selected->getFxType() ||
      built->getIdentifier() != selected->getIdentifier()) {
    error = QCoreApplication::translate("ParticleCache",
        "The selected particle graph could not be prepared for baking.");
    return false;
  }
  if (!validAffine(result.placement)) {
    error = QCoreApplication::translate("ParticleCache",
        "The particle or camera placement is singular or invalid at frame %1.")
                .arg(frame + 1);
    return false;
  }
  return true;
}

inline bool samplingRequest(const Source &source, int frame,
                            const TRenderSettings &requestedSettings,
                            const TRectD &requestedPixels,
                            TRenderSettings &nativeSettings,
                            TRectD &nativePixels, QString &error) {
  nativeSettings = requestedSettings;
  nativePixels = requestedPixels;
  for (const TRasterFxP &effect : source.columnEffects) {
    TRectD actualPixels, inputPixels;
    TRenderSettings actualSettings(nativeSettings), inputSettings(nativeSettings);
    if (!effect->prepareRenderRequest(frame, nativePixels, nativeSettings,
                                      actualPixels, actualSettings)) {
      error = QCoreApplication::translate("ParticleCache",
          "The column depth effect has no valid particle sampling area.");
      return false;
    }
    effect->transform(frame, 0, actualPixels, actualSettings,
                       inputPixels, inputSettings);
    nativePixels = inputPixels;
    nativeSettings = inputSettings;
  }
  if (nativePixels.isEmpty() || !validAffine(nativeSettings.m_affine)) {
    error = QCoreApplication::translate("ParticleCache",
        "The column depth effect has an invalid particle sampling transform.");
    return false;
  }
  return true;
}
}  // namespace ParticleBakeScene
