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
// The admitted number schema of one channel of a parameter: component is the
// index of a point or color channel, or -1 for a scalar. Color channels are
// native fractions 0..1. Empty when the parameter is not editable.
QJsonObject automationFxChannelSchema(TFx* fx, TParam* param,
                                      const std::string& name, int component);
// True when a constraint of the type reads the parameter, so its curves keep
// linear keys with step 1.
bool automationFxLinearOnly(TFx* fx, const std::string& name);
// The SC-4 descriptor fields of one parameter, beside its schema: kind,
// default (native units; colors as 0..255 components), bounds {admitted,
// native, slider, source}, measure, unit (as animation.channels names it),
// and for menus their choices {id, label} with label_is_identity false.
QJsonObject automationFxParameterFacts(TFx* fx, TParam* param,
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
// How many ports a dynamic port group of an admitted type may hold through
// fx.port_group.set; 0 when the worker does not edit that group.
int automationFxMaxGroupPorts(const std::string& type);
QJsonObject automationFxProvenance(TFx* fx);

}  // namespace GroundzeroAutomation
