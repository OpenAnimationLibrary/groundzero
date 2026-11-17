#pragma once

#include "tfx.h"
#include <QJsonObject>

class ToonzScene;
class QString;

namespace GroundzeroAutomation {

struct Document;
TFxP makeAutomationEffect(const QString& type);
QJsonObject describeAutomationFx(Document& document, TFx* fx);
QByteArray automationFxPresetBytes(TFx* fx, const QString& path);
QString automationFxName(const QJsonObject& params);

bool isAutomationFxTypeSupported(const QString& type);

// Validate builtin admission, graph ownership, and acyclicity before
// serialization or rendering. Throws Error with an actionable diagnostic when
// the native graph is unsupported or malformed.
void validateAutomationFxGraph(ToonzScene& scene);

}  // namespace GroundzeroAutomation
