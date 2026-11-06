#pragma once
#include "toonz/rasterbrushsupport.h"

// Authored current-path/metadata/report cases; no source qualification follows.
void testSourceSupportDiagnostics() {
  using C = SourceCapabilityV1;
  using E = SourceEvaluatorV1;
  using R = SourceDiagnosticReasonV1;
  const auto row = [](const SourceSupportReportV1 &report, C capability)
      -> const SourceCapabilityDiagnosticV1 & {
    for (const auto &item : report.capabilities)
      if (item.capability == capability) return item;
    throw std::runtime_error("Missing derived source capability row");
  };
  const auto reason = [&](const SourceSupportReportV1 &report, C capability, R why) {
    const auto &reasons = row(report, capability).reasons;
    return std::find(reasons.begin(), reasons.end(), why) != reasons.end();
  };
  Record native;
  native.referenceVerified = true;
  native.provenance = {{"sourceReferenceVerified", true}, {"sourceSemanticallySupported", true}};
  const auto initial = sourceSupportReportV1(native);
  require(initial.recognizedProfile && initial.currentSettingsAccepted && initial.nativeProfile &&
              !initial.sourceReferenceEstablished && row(initial, C::Geometry).evaluator == E::Native &&
              row(initial, C::Sensors).evaluator == E::Inactive &&
              row(initial, C::Texture).evaluator == E::Inactive &&
              reason(initial, C::ReferenceEvidence, R::SourceReferenceNotEstablished),
          "Derived source report trusted imported claims or invented active controls");
  native.preset.opacity.mappings.push_back({Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
  auto configured = sourceSupportReportV1(native);
  require(row(configured, C::Sensors).configuredMappings == 1 &&
              row(configured, C::Transfer).configuredMappings == 1 &&
              row(configured, C::Sensors).evaluator == E::Native,
          "Configured native mappings were not reported separately");
  native.sourceSensorsV1 = Detail::preserveKppSourceSensors({
      {"PressureOpacity", "false"}, {"OpacityUseCurve", "true"},
      {"OpacityUseSameCurve", "true"}, {"OpacityValue", "1"},
      {"OpacitySensor", "<params id=\"time\" duration=\"2500\" periodic=\"0\"><curve>0,0;1,1;</curve></params>"},
      {"MaskingBrush/Preset/PressureSize", "true"},
      {"MaskingBrush/Preset/SizeUseCurve", "false"},
      {"MaskingBrush/Preset/SizeSensor", "<params id=\"distance\" length=\"128\" periodic=\"1\"/>"}});
  const auto declared = sourceSupportReportV1(native);
  const auto declaredText = sourceSupportDetailsV1(declared).join("\n");
  require(row(declared, C::Sensors).configuredMappings == 1 &&
              row(declared, C::Sensors).preservedControls == 2 &&
              reason(declared, C::Sensors, R::PreservedSensorsNotEvaluated) &&
              reason(declared, C::Sensors, R::RetainedPressureSwitchOff) &&
              !reason(declared, C::Sensors, R::DeclarationDisabled) &&
              declaredText.contains("Primary source opacity") && declaredText.contains("pressure switch=off") &&
              declaredText.contains("Masked source size") && declaredText.contains("2500 source milliseconds") &&
              declaredText.contains("128 source length units") && declaredText.contains("repeat=off"),
          "Source switch, scope or interval metadata was mistaken for an active evaluator");
  native.preset.opacity.mappings.clear();
  const auto inactive = sourceSupportReportV1(native);
  require(row(inactive, C::Sensors).configuredMappings == 0 &&
              row(inactive, C::Sensors).evaluator == E::Inactive &&
              row(inactive, C::Sensors).hasPreservedDeclarations,
          "Retained declarations activated a mapping or stale report survived editing");

  SourceMaskDefinitionV1 mask;
  mask.enabled = true;
  mask.sourceFieldsSha256 = QString(64, 'a');
  mask.unsupportedFeatures << "unresolved-resource";
  native.sourceMaskDefinitionV1 = mask;
  const auto masked = sourceSupportReportV1(native);
  require(row(masked, C::SecondaryMask).evaluator == E::Inactive &&
              row(masked, C::SecondaryMask).hasPreservedDeclarations &&
              reason(masked, C::SecondaryMask, R::PreservedMaskNotEvaluated) &&
              reason(masked, C::SecondaryMask, R::MaskResourceUnresolved) &&
              !native.preset.secondary.enabled && !native.preset.sourceMasked.enabled,
          "Retained source mask resource declarations became active");
  SourceGeneratedDefinitionV1 generated;
  generated.diameter = 16;
  native.sourceGeneratedV1 = generated;
  require(reason(sourceSupportReportV1(native), C::Geometry, R::PreservedGeneratedNotEvaluated),
          "Retained generated definition was mistaken for an active generator");

  Record source;
  source.preset.profile = "krita-5.3.4.1-hard-pixel-candidate-1";
  const auto candidate = sourceSupportReportV1(source);
  require(candidate.currentSettingsAccepted && !candidate.nativeProfile &&
              row(candidate, C::Geometry).evaluator == E::ExperimentalSource &&
              reason(candidate, C::Geometry, R::ExperimentalPathOnly) &&
              !candidate.sourceReferenceEstablished,
          "Recognized constrained path became general source support");
  source.preset.size.value = 20.5;
  const auto invalid = sourceSupportReportV1(source);
  require(!invalid.currentSettingsAccepted && row(invalid, C::Geometry).evaluator == E::Rejected &&
              reason(invalid, C::Geometry, R::CurrentSettingsRejected),
          "Diagnostic descriptor bypassed the authoritative current-setting guard");
  source.preset.profile = "unknown-private-profile-marker";
  const auto unknown = sourceSupportReportV1(source);
  require(!unknown.recognizedProfile && !unknown.currentSettingsAccepted &&
              reason(unknown, C::Geometry, R::UnknownProfile) &&
              !sourceSupportDetailsV1(unknown).join("\n").contains("unknown-private-profile-marker"),
          "Unknown profile was admitted or its private name leaked through the fixed report");

  // Safe omission categories survive portable reopen, while arbitrary field
  // names, XML, reports and forged support flags do not become public evidence.
  native.provenance["conversionKind"] = "partial-native";
  native.provenance["sourceOmissionsV1"] = QJsonArray{
      "source-transfer-dynamics", "source-secondary-mask", "private-omission-marker"};
  native.provenance["sourceDescriptorTupleV1"] = "private-descriptor-marker";
  native.diagnostics << "private-diagnostic-marker";
  QString error;
  const auto portable = encodePortable(native, ResourcePackaging::PrivateCopy, error);
  require(error.isEmpty() && !portable.isEmpty() &&
              !portable.contains("private-omission-marker") &&
              !portable.contains("private-descriptor-marker") &&
              !portable.contains("private-diagnostic-marker"),
          "Safe capability report portable projection leaked private source text");
  const auto reopened = decode(portable, error);
  const auto restored = sourceSupportReportV1(reopened);
  require(error.isEmpty() && reopened.sourceSensorsV1 && reopened.sourceMaskDefinitionV1 &&
              row(restored, C::Sensors).preservedControls == 2 &&
              row(restored, C::Sensors).evaluator == E::Inactive &&
              reason(restored, C::Transfer, R::TransferDynamicsOmitted) &&
              reason(restored, C::SecondaryMask, R::SecondaryMaskOmitted) &&
              reason(restored, C::Composition, R::UnclassifiedSettingOmitted) &&
              !restored.sourceReferenceEstablished,
          "Portable reopen lost safe omission/declaration diagnostics or activated metadata");
  auto converted = reopened;
  converted.provenance["conversionKind"] = "native-reinterpretation";
  converted.preset.opacity.mappings.push_back({Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
  const auto copy = sourceSupportReportV1(converted);
  require(reason(copy, C::Geometry, R::SourceEquationsReplaced) &&
              row(copy, C::Sensors).configuredMappings == 1 &&
              row(copy, C::Sensors).preservedControls == 2 &&
              !copy.sourceReferenceEstablished,
          "Native conversion confused current edits with original source declarations");

  const auto abr = importBrush(boundAbr(false, false, 6, false, true), "abr", error);
  require(error.isEmpty() && abr.complete && abr.records.size() == 2 &&
              reason(sourceSupportReportV1(abr.records.front()), C::SecondaryMask, R::SecondaryMaskOmitted),
          "ABR dual omission was not classified in the derived per-capability report");
  const auto staticAbr = importBrush(boundAbr(false, false, 6, false, false, false), "abr", error);
  require(error.isEmpty() && staticAbr.complete &&
              safeSourceOmissionsV1(staticAbr.records.front().provenance.value("sourceOmissionsV1")).empty(),
          "Explicitly disabled ABR dynamics produced active omission diagnostics");
}
