#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <string>
#include <vector>

class TFx;
class TParam;

namespace GroundzeroAutomation {

// A parameter whose value, scaled by the render, pads the effect's input.
struct AutomationFxExtent final {
  std::string parameter;
  double coefficient;
  double maximumPixels;
};

QString automationFxProfileName();
// Admitted native types in catalog order.
const QStringList& automationFxProfileTypes();

// The admitted JSON schema of one parameter, or an empty object when the
// parameter kind is not editable.
QJsonObject automationFxParameterSchema(TFx* fx, TParam* param,
                                        const std::string& name);
// Throws INVALID_PARAMS or OUT_OF_RANGE when a number leaves its schema.
void validateAutomationFxNumber(const QJsonValue& value,
                                const QJsonObject& schema);
// Validates the native type, version, every parameter default and key, and
// the cross-parameter constraints. Throws the first violation as an Error.
void validateAutomationFxProfile(TFx* fx);
// For a stored effect of an admitted type at its profile version, lists the
// parameters (and constraints) outside their authoring ranges and returns
// true; returns false when the type or version is not admitted.
bool automationFxProfileViolations(TFx* fx, QStringList& violations);
// Moves every stored number of an effect of an admitted type and version into
// its authoring range: defaults and key values of scalar and point curves, and
// integer parameters. Animation shapes and enumerations are left unchanged.
void clampAutomationFxValues(TFx* fx);

// Per-type profile facts for fx.catalog, fx.graph and render preflight.
QJsonObject automationFxProfileDescription(const QString& type);
QJsonArray automationFxProfileDiagnostics(TFx* fx);
std::vector<AutomationFxExtent> automationFxExtents(const std::string& type);
bool automationFxNeedsPositiveDeterminant(const std::string& type);
QJsonObject automationFxProvenance(TFx* fx);

}  // namespace GroundzeroAutomation
