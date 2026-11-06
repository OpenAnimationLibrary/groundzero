#include "activechecks.h"

#include "tapp.h"
#include "toutputproperties.h"
#include "trasterfx.h"
#include "toonz/activechecks.h"
#include "toonz/childstack.h"
#include "toonz/preferences.h"
#include "toonz/stage2.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"

void captureActiveRenderChecks(ToonzScene *scene,
                               TOutputProperties *properties) {
  if (!scene || !properties) return;
  TRenderSettings settings = properties->getRenderSettings();
  if (!settings.m_activeChecks.m_enabled) return;

  TRenderCheckSettings checks;
  checks.m_enabled         = true;
  ToonzCheck *viewChecks   = ToonzCheck::instance();
  Preferences *preferences = Preferences::instance();
  checks.m_checks =
      viewChecks->getChecks() & TRenderCheckSettings::SupportedChecks;
  checks.m_colorIndex      = viewChecks->getColorIndex();
  checks.m_fillOnlySavebox = preferences->getFillOnlySavebox();
  checks.m_inkColor        = preferences->getInkCheckColor();
  checks.m_ink1Color       = preferences->getInk1CheckColor();
  checks.m_paintColor      = preferences->getPaintCheckColor();
  preferences->getTranspCheckData(checks.m_transparencyInkOnBlack,
                                  checks.m_transparencyInkOnWhite,
                                  checks.m_transparencyPaint);

  TApp *app               = TApp::instance();
  ChildStack *stack       = scene->getChildStack();
  const int depth         = stack->getAncestorCount();
  const int currentColumn = app->getCurrentColumn()->getColumnIndex();
  if (app->getCurrentScene()->getScene() == scene && currentColumn >= 0 &&
      depth <= 32) {
    bool validPath = true;
    for (int i = 0; i < depth; ++i) {
      const AncestorNode *ancestor = stack->getAncestorInfo(i);
      if (!ancestor || ancestor->m_col < 0 || ancestor->m_row < 0) {
        validPath = false;
        break;
      }
      checks.m_columnPath.push_back(ancestor->m_col);
      checks.m_columnPath.push_back(ancestor->m_row);
    }
    if (validPath) {
      checks.m_columnPath.push_back(currentColumn);
      resolveActiveRenderChecks(scene, checks);
    }
    if (!checks.m_targetFxId) checks.m_columnPath.clear();
  }

  settings.m_activeChecks = checks;
  properties->setRenderSettings(settings);
}
