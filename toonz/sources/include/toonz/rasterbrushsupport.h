#pragma once

#include "toonz/rasterbrushpreset.h"
#include "toonz/rasterbrushprofile.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <algorithm>
#include <array>

namespace RasterBrush {
// A derived explanation of the current constrained renderer path. This is not
// source admission, a mapping from preserved declarations, or reference proof.
enum class SourceEvaluatorV1 { Inactive, Native, ExperimentalSource, Rejected };
enum class SourceDiagnosticReasonV1 {
  CurrentSettingsRejected, UnknownProfile, ExperimentalPathOnly,
  SourceReferenceNotEstablished, PreservedSensorsNotEvaluated,
  PreservedMaskNotEvaluated, PreservedGeneratedNotEvaluated,
  DeclarationDisabled, RetainedPressureSwitchOff, DeclarationStateUnknown, UnprojectedDeclaration,
  MaskResourceUnresolved, MaskResourceColorUnverified,
  SourceEquationsReplaced, TipExtractionOnly, StaticNativeConversion,
  PartialNativeConversion, ShapeDynamicsOmitted, ScatterDynamicsOmitted,
  TransferDynamicsOmitted, ColorDynamicsOmitted, TextureOmitted,
  SecondaryMaskOmitted, BrushPoseOmitted, ToolOptionsOmitted,
  TipGeometryOmitted, StrokeEffectsOmitted, UnclassifiedSettingOmitted
};
struct SourceCapabilityDiagnosticV1 {
  SourceCapabilityV1 capability = SourceCapabilityV1::TipResource;
  SourceEvaluatorV1 evaluator = SourceEvaluatorV1::Inactive;
  size_t configuredMappings = 0, preservedControls = 0;
  bool hasPreservedDeclarations = false;
  std::vector<SourceDiagnosticReasonV1> reasons;
};
struct SourceSupportReportV1 {
  unsigned schemaVersion = 1;
  bool recognizedProfile = false, currentSettingsAccepted = false;
  bool nativeProfile = false;
  // Deliberately never inferred from Record flags or imported provenance.
  bool sourceReferenceEstablished = false;
  std::array<SourceCapabilityDiagnosticV1, 11> capabilities;
  QStringList preservedDeclarationDetails; // Derived fixed labels, never XML.
};

// Immutable historical omission categories are portable; a derived support
// verdict is never persisted. Arbitrary source names/XML cannot enter this list.
inline const QStringList &sourceOmissionVocabularyV1() {
  static const QStringList tags{
      "source-shape-dynamics", "source-scatter-dynamics", "source-transfer-dynamics",
      "source-color-dynamics", "source-texture", "source-secondary-mask",
      "source-brush-pose", "source-tool-options", "source-tip-geometry",
      "source-stroke-effects", "source-unclassified-setting"};
  return tags;
}
inline QStringList safeSourceOmissionsV1(const QJsonValue &value) {
  if (value.isUndefined()) return {};
  QStringList result;
  bool unknown = !value.isArray();
  const auto input = value.toArray();
  // A malformed or oversized diagnostic declaration can only add a warning.
  // It cannot alter active settings, semantic flags or source admission.
  if (input.size() > sourceOmissionVocabularyV1().size()) unknown = true;
  for (int index = 0; index < std::min<int>(input.size(), 32); ++index) {
    const auto item = input[index];
    if (item.isString() && sourceOmissionVocabularyV1().contains(item.toString())) {
      if (!result.contains(item.toString())) result.append(item.toString());
    } else unknown = true;
  }
  if (unknown && !result.contains("source-unclassified-setting"))
    result.append("source-unclassified-setting");
  return result;
}
namespace Detail {
inline void sourceSupportReason(SourceCapabilityDiagnosticV1 &row,
                                SourceDiagnosticReasonV1 reason) {
  if (std::find(row.reasons.begin(), row.reasons.end(), reason) == row.reasons.end())
    row.reasons.push_back(reason);
}
inline SourceCapabilityDiagnosticV1 &sourceSupportRow(
    SourceSupportReportV1 &report, SourceCapabilityV1 capability) {
  for (auto &row : report.capabilities) if (row.capability == capability) return row;
  return report.capabilities.back(); // All declared capabilities occur below.
}
inline void sourceSupportOmission(SourceSupportReportV1 &report, const QString &tag) {
  using C = SourceCapabilityV1;
  using R = SourceDiagnosticReasonV1;
  const std::array<C, 11> capabilities{{C::Geometry, C::StochasticProcess,
      C::Transfer, C::Foreground, C::Texture, C::SecondaryMask, C::Sensors,
      C::Composition, C::Geometry, C::Composition, C::Composition}};
  const std::array<R, 11> reasons{{R::ShapeDynamicsOmitted, R::ScatterDynamicsOmitted,
      R::TransferDynamicsOmitted, R::ColorDynamicsOmitted, R::TextureOmitted,
      R::SecondaryMaskOmitted, R::BrushPoseOmitted, R::ToolOptionsOmitted,
      R::TipGeometryOmitted, R::StrokeEffectsOmitted, R::UnclassifiedSettingOmitted}};
  const int index = sourceOmissionVocabularyV1().indexOf(tag);
  if (index >= 0) sourceSupportReason(sourceSupportRow(report, capabilities[size_t(index)]), reasons[size_t(index)]);
}
} // namespace Detail

inline QStringList sourcePreservedDeclarationsV1(const Record &record) {
  auto text = [](const char *value) { return QCoreApplication::translate("RasterBrushSupport", value); };
  auto state = [&](const std::optional<bool> &value) {
    return !value ? text("not declared") : *value ? text("on") : text("off");
  };
  auto label = [&](const QString &value, const QStringList &ids, const char *const *labels,
                   const char *unknown) {
    const int index = ids.indexOf(value);
    return text(index < 0 ? unknown : labels[index]);
  };
  const QStringList targets{"Size", "Ratio", "Rotation", "Spacing", "Scatter", "Opacity", "Flow",
      "SmudgeRate", "ColorRate", "SmudgeRadius", "h", "s", "v", "Mirror", "Texture/Strength/"};
  const char *const targetLabels[]{"size", "aspect", "rotation", "spacing", "scatter", "opacity", "flow",
      "smudge rate", "paint color rate", "smudge radius", "hue", "saturation", "value", "mirror", "texture strength"};
  const QStringList sensors{"pressure", "pressurein", "xtilt", "ytilt", "ascension", "declination", "rotation",
      "drawingangle", "time", "distance", "speed", "fuzzy", "fuzzystroke", "fade", "perspective", "tangentialpressure"};
  const char *const sensorLabels[]{"Pressure", "Pressure input", "X tilt", "Y tilt", "Tilt direction", "Tilt elevation",
      "Barrel rotation", "Drawing angle", "Time", "Distance", "Speed", "Per-dab randomness", "Per-stroke randomness",
      "Fade", "Perspective", "Tangential pressure"};
  QStringList result;
  if (record.sourceSensorsV1) {
    const auto &controls = record.sourceSensorsV1->controls;
    for (size_t index = 0; index < std::min<size_t>(controls.size(), 32); ++index) {
      const auto &control = controls[index];
      const QString scope = control.scope == "masked" ? text("Masked") : control.scope == "primary"
          ? text("Primary") : text("Unrecognized scope");
      result << text("%1 source %2: declared pressure switch=%3, use curve=%4; metadata only.")
          .arg(scope, label(control.target, targets, targetLabels, "unrecognized control"),
               state(control.enabled), state(control.useCurve));
      if (!control.unsupportedFields.empty())
        result << text("Unprojected source fields: %1; original field text is excluded.")
            .arg(control.unsupportedFields.size());
      if (control.commonCurve)
        result << text("Retained common curve: %1 knots. Its source interpolation is not established.")
            .arg(qulonglong(control.commonCurve->size()));
      for (size_t item = 0; item < std::min<size_t>(control.sensors.size(), 16); ++item) {
        const auto &sensor = control.sensors[item];
        const QString name = label(sensor.id, sensors, sensorLabels, "Unrecognized source sensor");
        result << text("Source sensor: %1; retained curve knots: %2.")
            .arg(name).arg(sensor.curve ? qulonglong(sensor.curve->size()) : 0);
        if (sensor.id == "time" || sensor.id == "distance" || sensor.id == "fade") {
          const QString unit = sensor.id == "time" ? text("source milliseconds") : sensor.id == "distance"
              ? text("source length units") : text("source dab count");
          result << text("Declared interval: %1 %2; repeat=%3. These units are not converted to native timing.")
              .arg(sensor.interval ? QString::number(*sensor.interval, 'g', 17) : text("not declared"),
                   unit, state(sensor.periodic));
        }
      }
      if (control.sensors.size() > 16) result << text("Additional source sensor declarations are omitted from this bounded report.");
    }
    if (controls.size() > 32) result << text("Additional source controls are omitted from this bounded report.");
  }
  if (record.sourceMaskDefinitionV1) {
    const auto &mask = *record.sourceMaskDefinitionV1;
    const QString composition = mask.composition == "multiply" ? text("Multiply")
        : mask.composition == "subtract" ? text("Subtract") : mask.composition == "absent"
            ? text("not declared") : text("unprojected");
    const QString resource = mask.tipImage ? text("decoded pixels retained separately")
        : mask.generated ? text("generated definition retained separately")
        : mask.unsupportedFeatures.contains("unresolved-resource") ? text("original resource unresolved")
            : text("no decoded image retained");
    result << text("Source mask: declared enabled=%1, composition=%2, %3; metadata only.")
        .arg(state(mask.enabled), composition, resource);
  }
  return result;
}

inline SourceSupportReportV1 sourceSupportReportV1(const Record &record) {
  using C = SourceCapabilityV1;
  using R = SourceDiagnosticReasonV1;
  SourceSupportReportV1 report;
  const auto &p = record.preset;
  const auto descriptor = profileDescriptorV1(p.profile);
  report.recognizedProfile = descriptor.recognized();
  report.nativeProfile = descriptor.isNative();
  // Reuse the authoritative guard. Neither enum labels nor preserved metadata
  // substitute for numeric/resource/combination validation.
  report.currentSettingsAccepted = validate(p).empty();
  const std::array<C, 11> order{{C::TipResource, C::Geometry, C::Sensors,
      C::Transfer, C::Texture, C::SecondaryMask, C::Foreground, C::Timing,
      C::StochasticProcess, C::Composition, C::ReferenceEvidence}};
  for (size_t index = 0; index < order.size(); ++index)
    report.capabilities[index].capability = order[index];
  auto row = [&](C capability) -> SourceCapabilityDiagnosticV1 & {
    return Detail::sourceSupportRow(report, capability);
  };
  const auto active = !report.currentSettingsAccepted
      ? SourceEvaluatorV1::Rejected : descriptor.isNative()
      ? SourceEvaluatorV1::Native : SourceEvaluatorV1::ExperimentalSource;
  auto evaluate = [&](C capability, bool enabled = true) {
    if (enabled) row(capability).evaluator = active;
  };
  evaluate(C::TipResource, !p.tip.images.empty());
  evaluate(C::Geometry); evaluate(C::Transfer); evaluate(C::Foreground);
  evaluate(C::Timing); evaluate(C::Composition);
  evaluate(C::Texture, bool(p.texture.image));
  evaluate(C::SecondaryMask, p.secondary.enabled || p.sourceMasked.enabled);
  evaluate(C::StochasticProcess, p.sourceScatterV1.has_value() ||
      descriptor.stochastic == StochasticProcessV1::SourceGihProposalCache ||
      (p.tip.randomSequence && p.tip.images.size() > 1) || p.scatter.value != 0 ||
      !p.scatter.mappings.empty());
  auto mappings = [&](const Dynamic &control, C capability) {
    row(capability).configuredMappings += control.mappings.size();
    row(C::Sensors).configuredMappings += control.mappings.size();
  };
  for (const auto *control : {&p.size, &p.aspect, &p.angle}) mappings(*control, C::Geometry);
  for (const auto *control : {&p.opacity, &p.flow}) mappings(*control, C::Transfer);
  mappings(p.countControl, C::Geometry);
  mappings(p.scatter, C::StochasticProcess);
  for (const auto *control : {&p.backgroundMix, &p.hue, &p.saturation, &p.brightness})
    mappings(*control, C::Foreground);
  if (p.wet != Wet::Off)
    for (const auto *control : {&p.pickupControl, &p.colorRateControl, &p.radiusControl})
      mappings(*control, C::Transfer);
  if (p.secondary.enabled)
    for (const auto *control : {&p.secondary.size, &p.secondary.angle, &p.secondary.opacity})
      mappings(*control, C::SecondaryMask);
  if (p.sourceMirror.enabled) mappings(p.sourceMirror.control, C::Geometry);
  for (const auto &channel : p.sourceHsvV2)
    if (channel.enabled) mappings(channel.control, C::Foreground);
  evaluate(C::Sensors, row(C::Sensors).configuredMappings != 0);
  for (auto &item : report.capabilities) {
    if (!descriptor.recognized()) Detail::sourceSupportReason(item, R::UnknownProfile);
    else if (!report.currentSettingsAccepted) Detail::sourceSupportReason(item, R::CurrentSettingsRejected);
    else if (item.evaluator == SourceEvaluatorV1::ExperimentalSource)
      Detail::sourceSupportReason(item, R::ExperimentalPathOnly);
  }
  if (record.sourceSensorsV1) {
    auto &sensors = row(C::Sensors);
    sensors.hasPreservedDeclarations = !record.sourceSensorsV1->controls.empty();
    sensors.preservedControls = record.sourceSensorsV1->controls.size();
    if (sensors.hasPreservedDeclarations)
      Detail::sourceSupportReason(sensors, R::PreservedSensorsNotEvaluated);
    for (const auto &control : record.sourceSensorsV1->controls) {
      if (control.enabled == std::optional<bool>(false))
        Detail::sourceSupportReason(sensors, R::RetainedPressureSwitchOff);
      else if (!control.enabled) Detail::sourceSupportReason(sensors, R::DeclarationStateUnknown);
      if (!control.unsupportedFields.empty())
        Detail::sourceSupportReason(sensors, R::UnprojectedDeclaration);
    }
  }
  if (record.sourceMaskDefinitionV1) {
    const auto &definition = *record.sourceMaskDefinitionV1;
    auto &mask = row(C::SecondaryMask);
    mask.hasPreservedDeclarations = true;
    // The declaration object never drives SourceMasked, even if a separately
    // admitted fixed renderer is active. Do not infer that the two correspond.
    Detail::sourceSupportReason(mask, R::PreservedMaskNotEvaluated);
    if (definition.enabled == std::optional<bool>(false))
      Detail::sourceSupportReason(mask, R::DeclarationDisabled);
    else if (!definition.enabled) Detail::sourceSupportReason(mask, R::DeclarationStateUnknown);
    if (definition.unsupportedFeatures.contains("unresolved-resource"))
      Detail::sourceSupportReason(mask, R::MaskResourceUnresolved);
    if (definition.unsupportedFeatures.contains("unverified-resource-color-contract"))
      Detail::sourceSupportReason(mask, R::MaskResourceColorUnverified);
    for (const QString &tag : {QString("unrecognized-setting"), QString("unrecognized-geometry"),
                              QString("invalid-declaration"), QString("unsupported-tip-kind"),
                              QString("unsupported-tip-application"), QString("unprojectable-generated")})
      if (definition.unsupportedFeatures.contains(tag))
        Detail::sourceSupportReason(mask, R::UnprojectedDeclaration);
  }
  if (record.sourceGeneratedV1) {
    row(C::Geometry).hasPreservedDeclarations = true;
    Detail::sourceSupportReason(row(C::Geometry), R::PreservedGeneratedNotEvaluated);
  }
  for (const auto &tag : safeSourceOmissionsV1(record.provenance.value("sourceOmissionsV1")))
    Detail::sourceSupportOmission(report, tag);
  const auto conversion = record.provenance.value("conversionKind").toString();
  if (descriptor.isNative()) {
    if (conversion == "native-reinterpretation") {
      for (auto &item : report.capabilities)
        if (item.evaluator == SourceEvaluatorV1::Native)
          Detail::sourceSupportReason(item, R::SourceEquationsReplaced);
    } else if (conversion == "partial-native")
      Detail::sourceSupportReason(row(C::Composition), R::PartialNativeConversion);
    else if (conversion == "static-native")
      Detail::sourceSupportReason(row(C::Composition), R::StaticNativeConversion);
    else if (conversion == "tip-only" || conversion == "sampled-tip-extraction")
      Detail::sourceSupportReason(row(C::TipResource), R::TipExtractionOnly);
  }
  Detail::sourceSupportReason(row(C::ReferenceEvidence), R::SourceReferenceNotEstablished);
  report.preservedDeclarationDetails = sourcePreservedDeclarationsV1(record);
  return report;
}
namespace Detail {
inline QString sourceSupportText(const char *text) {
  return QCoreApplication::translate("RasterBrushSupport", text);
}
inline QString sourceCapabilityLabel(SourceCapabilityV1 capability) {
  using C = SourceCapabilityV1;
  switch (capability) {
  case C::TipResource: return sourceSupportText("Image tips");
  case C::Geometry: return sourceSupportText("Tip geometry");
  case C::Sensors: return sourceSupportText("Sensor controls");
  case C::Transfer: return sourceSupportText("Opacity, flow and paint transfer");
  case C::Texture: return sourceSupportText("Texture");
  case C::SecondaryMask: return sourceSupportText("Secondary masks");
  case C::Foreground: return sourceSupportText("Paint color");
  case C::Timing: return sourceSupportText("Dab spacing and timing");
  case C::StochasticProcess: return sourceSupportText("Scatter and random tips");
  case C::Composition: return sourceSupportText("Blending and brush combinations");
  case C::ReferenceEvidence: return sourceSupportText("Matching the original application");
  }
  return sourceSupportText("Brush behavior");
}
inline QString sourceReasonText(SourceDiagnosticReasonV1 reason) {
  using R = SourceDiagnosticReasonV1;
  switch (reason) {
  case R::CurrentSettingsRejected: return sourceSupportText("The current combination is rejected. Restore accepted settings or explicitly convert a copy to native.");
  case R::UnknownProfile: return sourceSupportText("This behavior profile is not recognized. Select a supported preset or a native copy.");
  case R::ExperimentalPathOnly: return sourceSupportText("Only the current constrained experimental path is evaluated; broader source controls and combinations are not established.");
  case R::SourceReferenceNotEstablished: return sourceSupportText("Matching source strokes and physical-device behavior have not been established by this preset.");
  case R::PreservedSensorsNotEvaluated: return sourceSupportText("Retained sensor declarations are metadata only. They do not bind to, replace or establish equivalence with the separately configured mappings.");
  case R::PreservedMaskNotEvaluated: return sourceSupportText("Retained mask definitions and images are metadata only. A separately active mask does not establish that those declarations are evaluated.");
  case R::PreservedGeneratedNotEvaluated: return sourceSupportText("Retained generated-tip geometry is metadata only; it does not enable or establish a matching generator.");
  case R::DeclarationDisabled: return sourceSupportText("Explicitly disabled source declarations are retained without activating them.");
  case R::RetainedPressureSwitchOff: return sourceSupportText("An original pressure switch is false. This alone does not disable a retained curve or determine the active mappings.");
  case R::DeclarationStateUnknown: return sourceSupportText("Some original source switches were absent or unprojectable. Their effective behavior is not inferred.");
  case R::UnprojectedDeclaration: return sourceSupportText("Some original settings could not be read completely. Keep the original preset for those settings.");
  case R::MaskResourceUnresolved: return sourceSupportText("A source mask resource was not resolved. Supply the original resource; relinking alone will not activate its declarations.");
  case R::MaskResourceColorUnverified: return sourceSupportText("Source mask coverage from color is unverified. The original color pixels are retained without applying that coverage rule.");
  case R::SourceEquationsReplaced: return sourceSupportText("This copy uses native equations. Its source declarations do not restore the original behavior.");
  case R::TipExtractionOnly: return sourceSupportText("Only image tips were extracted. Original preset controls were not imported; choose native settings explicitly.");
  case R::StaticNativeConversion: return sourceSupportText("Fixed settings were converted with native equations. Source dynamics and complete appearance remain unverified.");
  case R::PartialNativeConversion: return sourceSupportText("Some active or unknown source settings were omitted. Review the per-capability omissions and keep the original preset.");
  case R::ShapeDynamicsOmitted: return sourceSupportText("Source size, angle or roundness dynamics were not applied. Keep the original or create native dynamics explicitly.");
  case R::ScatterDynamicsOmitted: return sourceSupportText("Source scatter or count dynamics were not applied. Native random offsets do not reproduce the source process.");
  case R::TransferDynamicsOmitted: return sourceSupportText("Source opacity, flow or paint-transfer dynamics were not applied. Configure native controls explicitly if desired.");
  case R::ColorDynamicsOmitted: return sourceSupportText("Source color dynamics were not applied. Native color controls do not establish equivalent source behavior.");
  case R::TextureOmitted: return sourceSupportText("Source texture settings were not applied. Preserve the original pattern and preset; a native texture is a separate choice.");
  case R::SecondaryMaskOmitted: return sourceSupportText("Source dual-brush settings were not applied. A native secondary tip is a separate behavior.");
  case R::BrushPoseOmitted: return sourceSupportText("Source brush-pose controls were not applied. Do not infer matching tilt or rotation behavior.");
  case R::ToolOptionsOmitted: return sourceSupportText("Source tool options were not applied. Review native opacity, flow and tool settings explicitly.");
  case R::TipGeometryOmitted: return sourceSupportText("Some source tip-geometry fields were not applied. The current native geometry does not reproduce those fields.");
  case R::StrokeEffectsOmitted: return sourceSupportText("Source stroke effects were not applied. Keep the original for those effects.");
  case R::UnclassifiedSettingOmitted: return sourceSupportText("Additional source settings were not classified or applied. Keep the original; private field names are excluded from portable reports.");
  }
  return sourceSupportText("Source behavior is not established.");
}
} // namespace Detail

inline QString sourceSupportSummaryV1(const SourceSupportReportV1 &report) {
  if (!report.recognizedProfile)
    return Detail::sourceSupportText("The current behavior profile is unrecognized; rendering is unavailable.");
  if (!report.currentSettingsAccepted)
    return Detail::sourceSupportText("The current settings are rejected; restore accepted settings or convert a copy to native.");
  return report.nativeProfile
      ? Detail::sourceSupportText("The current native path is evaluated. Retained source declarations do not control rendering; source matching remains unverified.")
      : Detail::sourceSupportText("The current constrained experimental path is evaluated. Retained metadata and available controls do not establish general source support.");
}
inline QStringList sourceSupportDetailsV1(const SourceSupportReportV1 &report) {
  QStringList result;
  for (const auto &row : report.capabilities) {
    QString evaluated;
    switch (row.evaluator) {
    case SourceEvaluatorV1::Inactive: evaluated = Detail::sourceSupportText("This component is inactive in the current settings."); break;
    case SourceEvaluatorV1::Native: evaluated = Detail::sourceSupportText("Current native settings are evaluated."); break;
    case SourceEvaluatorV1::ExperimentalSource: evaluated = Detail::sourceSupportText("Only the current constrained experimental settings are evaluated."); break;
    case SourceEvaluatorV1::Rejected: evaluated = Detail::sourceSupportText("Current settings are blocked by validation."); break;
    }
    result << Detail::sourceCapabilityLabel(row.capability) + ": " + evaluated;
    if (row.configuredMappings)
      result << Detail::sourceSupportText("Configured mappings in current settings: %1. These do not imply a binding to preserved source declarations.")
          .arg(qulonglong(row.configuredMappings));
    if (row.preservedControls)
      result << Detail::sourceSupportText("Source controls retained separately as metadata: %1.").arg(qulonglong(row.preservedControls));
    for (const auto &reason : row.reasons) result << Detail::sourceReasonText(reason);
  }
  result.append(report.preservedDeclarationDetails);
  return result;
}
} // namespace RasterBrush
