#pragma once

#include "tfx.h"
#include <QJsonArray>
#include <QJsonObject>

#include <functional>

class ToonzScene;
class QString;

namespace GroundzeroAutomation {

struct Document;
TFxP makeAutomationEffect(const QString& type);
using EffectFactory = std::function<TFxP(const QString&)>;
// fx.catalog built with the given factory; a type whose factory throws
// UNAVAILABLE_FX is reported as available:false. The worker uses the real
// factory.
QJsonValue automationFxCatalog(const EffectFactory& factory);
QJsonObject describeAutomationFx(Document& document, TFx* fx);
QByteArray automationFxPresetBytes(TFx* fx, const QString& path);
QString automationFxName(const QJsonObject& params);

// The effect a handle names, checked to still belong to the document's graph.
TFx* automationOwnedFx(Document& document, const QString& id);
// Throws locked when fx is a column effect whose column or stage object is
// locked.
void automationRequireColumnEditable(Document& document, TFx* fx);
// Throws locked (reason curve_locked) when a curve of fx's parameters is
// locked in the editor.
void automationRequireUnlockedCurves(Document& document, TFx* fx);
// The effect that owns the parameters: a generator column wraps its zerary
// effect.
TFx* automationParameterFx(TFx* fx);

bool isAutomationFxTypeSupported(const QString& type);
// True when the admitted type is also registered in this worker.
bool isAutomationFxTypeAvailable(const QString& type);

// The static recipe documents behind fx.recipe.list.
QJsonArray automationFxRecipes();
QJsonValue listAutomationFxRecipes(const QJsonObject& params);

// Upper bound, in pixels per stage unit, of the scale of the render affine
// that fx receives at frame when the camera renders width by height pixels.
double automationFxRenderScale(ToonzScene& scene, TFx* fx, int width,
                               int height, int frame);
// Checks the pixel extent of every effect the current output renders, for
// each requested frame, before a render starts. Returns their provenance.
// Throws fx_preflight_failed with the failing effect, frame and reason.
QJsonArray preflightAutomationRender(Document& document, int width, int height,
                                     int start, int end, int step);

// Validate builtin admission, graph ownership, and acyclicity before
// serialization or rendering. Throws Error with an actionable diagnostic when
// the native graph is unsupported or malformed.
void validateAutomationFxGraph(ToonzScene& scene);

}  // namespace GroundzeroAutomation
