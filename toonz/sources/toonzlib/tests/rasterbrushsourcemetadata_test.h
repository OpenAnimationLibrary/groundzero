#pragma once
// Authored declarations test preservation, not source sensor equations.
void testSourceSensorPreservation() {
  const QString perSensor = "<params id=\"sensorslist\"><ChildSensor id=\"pressure\"><curve>0,0;0.5,0.75;1,1;</curve>"
      "</ChildSensor><ChildSensor id=\"xtilt\"><curve>0,1;1,0;</curve></ChildSensor>"
      "<ChildSensor id=\"time\" duration=\"2500\" periodic=\"1\"><curve>0,0.25;1,1;</curve></ChildSensor>"
      "<ChildSensor id=\"distance\" length=\"128\" periodic=\"0\"/>"
      "<ChildSensor id=\"fuzzy\"/><ChildSensor id=\"fuzzystroke\"/><ChildSensor id=\"speed\"/></params>";
  QMap<QString, QString> fields{{"PressureSize", "false"}, {"SizeUseCurve", "true"},
      {"SizeUseSameCurve", "false"}, {"SizeValue", "0.75"}, {"SizecurveMode", "0"},
      {"SizecommonCurve", "0,1;1,1;"}, {"SizeSensor", perSensor},
      {"FlowUseCurve", "false"}, {"FlowcommonCurve", ""}, {"FlowSensor", "<params id=\"pressure\"/>"},
      {"MaskingBrush/Preset/PressureRotation", "true"}, {"MaskingBrush/Preset/RotationUseCurve", "false"},
      {"MaskingBrush/Preset/RotationSensor", "<params id=\"drawingangle\" lockedAngleMode=\"1\" angleOffset=\"-45\" "
          "fanCornersEnabled=\"0\" fanCornersStep=\"30\"><curve>0,0;1,1;</curve></params>"}};
  const auto metadata = Detail::preserveKppSourceSensors(fields);
  const auto json = Detail::sourceSensorsJson(metadata);
  const auto control = [](const SourceSensorsV1 &value, const QString &target, const QString &scope = "primary") -> const SourceSensorControlV1 & {
    for (const auto &item : value.controls)
      if (item.target == target && item.scope == scope) return item;
    throw std::runtime_error("Missing preserved source sensor control");
  };
  const auto &size = control(metadata, "Size");
  require(size.enabled == std::optional<bool>(false) && size.useCurve == std::optional<bool>(true) &&
              size.useSameCurve == std::optional<bool>(false) && size.combinationCode == std::optional<int>(0) &&
              size.layout == "list" && size.sensors.size() == 7 && size.sensors[0].curve->size() == 3 &&
              size.sensors[2].id == "time" && size.sensors[2].interval == std::optional<double>(2500) &&
              size.sensors[2].periodic == std::optional<bool>(true) &&
              size.sensors[3].interval == std::optional<double>(128) && size.sensors[3].periodic == std::optional<bool>(false) &&
              !size.sensors[3].curve && size.commonCurve->size() == 2,
          "Inactive source list, individual curves, interval/repeat or combiner declaration was lost");
  const auto &flow = control(metadata, "Flow");
  const auto &rotation = control(metadata, "Rotation", "masked");
  require(!flow.enabled && flow.useCurve == std::optional<bool>(false) && flow.commonCurve && flow.commonCurve->empty() && flow.layout == "single" &&
              rotation.enabled == std::optional<bool>(true) && rotation.useCurve == std::optional<bool>(false) &&
              rotation.sensors[0].angleOffset == std::optional<double>(-45) &&
              rotation.sensors[0].lockedAngleMode == std::optional<int>(1) &&
              rotation.sensors[0].fanCornersEnabled == std::optional<bool>(false),
          "Absent switches became false or masked inactive technical controls changed");
  require(Detail::sourceSensorsJson(Detail::sourceSensors(json)) == json &&
              json["curveInterpolation"] == "not-declared" && json["role"] == "preserved-metadata-only",
          "Source curve metadata invented interpolation or active evaluator semantics");
  for (const auto &id : Detail::sourceSensorIdsV1()) {
    QMap<QString, QString> one{{"SizeSensor", "<params id=\"" + id + "\"/>"}};
    const auto projected = Detail::preserveKppSourceSensors(one);
    require(control(projected, "Size").sensors.size() == 1 && control(projected, "Size").sensors[0].id == id,
            "Known serialized source sensor ID was not retained");
  }

  const auto longDuration = Detail::preserveKppSourceSensors({
      {"SizeSensor", "<params id=\"time\" duration=\"2000000000\" periodic=\"0\"/>"}});
  require(control(longDuration, "Size").sensors[0].interval == std::optional<double>(2000000000.) &&
              control(longDuration, "Size").sensors[0].periodic == std::optional<bool>(false),
          "Preserved technical interval was restricted to a native evaluator range");

  Record original;
  original.sourceSensorsV1 = metadata;
  QString error;
  const auto settings = QJsonDocument::fromJson(encode(Record{}, error)).object()["settings"];
  const auto encoded = encode(original, error);
  require(error.isEmpty() && !encoded.isEmpty() &&
              QJsonDocument::fromJson(encoded).object()["settings"] == settings,
          "Preserved declarations changed active native settings");
  const auto same = [&](const Record &record) {
    require(error.isEmpty() && record.sourceSensorsV1 &&
                Detail::sourceSensorsJson(*record.sourceSensorsV1) == json &&
                record.preset.profile == "groundzero-native-1" && !record.referenceVerified &&
                !record.provenance["sourceReferenceVerified"].toBool(),
            "Preserved sensor metadata changed or granted source verification");
  };
  same(decode(encoded, error));
  for (auto packaging : {ResourcePackaging::ManifestOnly, ResourcePackaging::PermittedOnly, ResourcePackaging::PrivateCopy}) {
    const auto bytes = encodePortable(original, packaging, error);
    require(error.isEmpty() && !bytes.isEmpty(), "Preserved sensor portable encoding failed");
    same(decode(bytes, error));
  }
  QTemporaryDir directory;
  require(directory.isValid(), "Cannot create preserved sensor worker fixture");
  const auto wire = encodeWorkerRecord(original, directory.path(), error);
  require(error.isEmpty() && !wire.isEmpty(), "Preserved sensor worker encoding failed");
  same(decodeWorkerRecord(wire, directory.path(), error));

  // Unknown syntax/values retain an explicit unsupported declaration and a
  // technical hash, never an arbitrary sensor ID, XML blob or private text.
  auto unknownFields = fields;
  unknownFields["SizeSensor"] = "<params id=\"private_descriptor_marker\" origin=\"private_origin_marker\"/>";
  unknownFields["PressureFlow"] = "private_switch_marker";
  original.sourceSensorsV1 = Detail::preserveKppSourceSensors(unknownFields);
  const auto &unknown = control(*original.sourceSensorsV1, "Size");
  require(unknown.layout == "unsupported" && unknown.sensors.empty() && unknown.unsupportedFields.contains("sensors") &&
              unknown.sourceFieldsSha256.size() == 64 && control(*original.sourceSensorsV1, "Flow").unsupportedFields.contains("enabled"),
          "Unknown source declarations lost their unsupported state/hash");
  const auto portableUnknown = encodePortable(original, ResourcePackaging::PrivateCopy, error);
  require(error.isEmpty() && !portableUnknown.isEmpty() && !portableUnknown.contains("private_descriptor_marker") &&
              !portableUnknown.contains("private_origin_marker") && !portableUnknown.contains("private_switch_marker") &&
              !portableUnknown.contains("<params"),
          "Portable preserved metadata leaked raw source XML or arbitrary values");

  const auto root = QJsonDocument::fromJson(encoded).object();
  for (int mutation = 0; mutation < 17; ++mutation) {
    auto document = root;
    auto declared = json;
    auto controls = declared["controls"].toArray();
    auto first = controls[0].toObject();
    auto sensors = first["sensors"].toArray();
    auto sensor = sensors[0].toObject();
    if (mutation == 0) declared["version"] = true;
    if (mutation == 1) declared["role"] = "active-evaluator";
    if (mutation == 2) declared["sourceReferenceVerified"] = true;
    if (mutation == 3) declared["curveInterpolation"] = "linear";
    if (mutation == 4) first.remove("enabled");
    if (mutation == 5) first["enabled"] = 0;
    if (mutation == 6) first["combinationCode"] = .5;
    if (mutation == 7) first["target"] = "unknown_control";
    if (mutation == 8) first["sourceFieldsSha256"] = "invalid";
    if (mutation == 9) sensor["id"] = "private_descriptor_marker";
    if (mutation == 10) sensor["periodic"] = 1;
    if (mutation == 11) sensor["intervalUnit"] = "native-seconds";
    if (mutation == 12) sensor["curve"] = QJsonArray{QJsonArray{0, 0}, QJsonArray{0, 1}};
    if (mutation == 13) sensor["sourcePath"] = "private_source_marker";
    if (mutation == 14) first["unsupportedFields"] = QJsonArray{"enabled"};
    if (mutation == 15) first["layout"] = "absent";
    if (mutation == 16) controls.append(first);
    sensors[0] = sensor;
    first["sensors"] = sensors;
    controls[0] = first;
    declared["controls"] = controls;
    document["sourceSensorsV1"] = declared;
    decode(QJsonDocument(document).toJson(), error);
    require(!error.isEmpty(), "Malformed preserved sensor declaration was accepted");
  }

  const auto fixture = [](const QMap<QString, QString> &overrides, const QString &model = QString()) {
    QImage image = QImage::fromData(hardPixelKpp("buildUp", model), "PNG");
    QString xml = image.text("preset");
    for (auto item = overrides.begin(); item != overrides.end(); ++item) {
      const QString opening = "<param name=\"" + item.key() + "\">";
      const int start = xml.indexOf(opening);
      const QString replacement = opening + item.value().toHtmlEscaped() + "</param>";
      if (start < 0) xml.replace("</Preset>", replacement + "</Preset>");
      else {
        const int end = xml.indexOf("</param>", start);
        require(end >= 0, "Authored sensor fixture parameter is incomplete");
        xml.replace(start, end + 8 - start, replacement);
      }
    }
    image.setText("preset", xml);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "Cannot encode authored sensor KPP");
    return bytes;
  };
  for (const QString &model : {QString(), QString("new-dulling")}) {
    const QMap<QString, QString> multi{{"PressureSize", "true"}, {"SizeUseCurve", "true"},
        {"SizeUseSameCurve", "false"}, {"SizeValue", "1"}, {"SizecurveMode", "0"}, {"SizeSensor", perSensor}};
    const auto imported = importBrush(fixture(multi, model), "kpp", error);
    require(error.isEmpty() && imported.complete && imported.records.size() == 1 && imported.records.front().sourceSensorsV1 &&
                control(*imported.records.front().sourceSensorsV1, "Size").sensors.size() == 7 &&
                control(*imported.records.front().sourceSensorsV1, "Size").combinationCode == std::optional<int>(0) &&
                imported.records.front().preset.size.mappings.empty() && imported.records.front().preset.profile == "groundzero-native-1",
            "K/S multi-sensor preservation widened active evaluation or dropped declarations");
    const auto saved = encodePortable(imported.records.front(), ResourcePackaging::PrivateCopy, error);
    const auto restored = decode(saved, error);
    require(error.isEmpty() && restored.sourceSensorsV1 &&
                Detail::sourceSensorsJson(*restored.sourceSensorsV1) == Detail::sourceSensorsJson(*imported.records.front().sourceSensorsV1),
            "K/S source declaration roundtrip lost independent combinations");
  }
  const auto transfer = importBrush(fixture({{"PressureOpacity", "false"}, {"OpacityUseCurve", "true"},
      {"OpacityUseSameCurve", "true"}, {"OpacityValue", "1"}, {"OpacitycurveMode", "0"},
      {"OpacitycommonCurve", "0,0;1,1;"}, {"OpacitySensor", "<params id=\"pressure\"><curve>0,0;1,1;</curve></params>"}}), "kpp", error);
  require(error.isEmpty() && transfer.complete && transfer.records.size() == 1 && transfer.records.front().sourceSensorsV1 &&
              control(*transfer.records.front().sourceSensorsV1, "Opacity").enabled == std::optional<bool>(false) &&
              control(*transfer.records.front().sourceSensorsV1, "Opacity").useCurve == std::optional<bool>(true) &&
              transfer.records.front().preset.opacity.mappings.size() == 1,
          "Preservation overwrote Pixel's raw Pressure flag with inferred effective curve activity");
  for (const QString &id : {QString("time"), QString("distance"), QString("speed")}) {
    const QString attributes = id == "time" ? " duration=\"2500\" periodic=\"1\"" :
                               id == "distance" ? " length=\"128\" periodic=\"1\"" : "";
    const auto imported = importBrush(fixture({{"PressureSize", "true"}, {"SizeUseCurve", "true"},
        {"SizeUseSameCurve", "false"}, {"SizecurveMode", "0"},
        {"SizeSensor", "<params id=\"" + id + "\"" + attributes + "><curve>0,0.25;1,1;</curve></params>"}}), "kpp", error);
    require(error.isEmpty() && imported.complete && imported.records.size() == 1 && imported.records.front().sourceSensorsV1,
            "Authored interval/speed source declaration failed native conversion");
    const auto &record = imported.records.front();
    const auto &preserved = control(*record.sourceSensorsV1, "Size").sensors.front();
    require(preserved.id == id && (id == "speed" || preserved.periodic == std::optional<bool>(true)) &&
                (id != "time" || preserved.interval == std::optional<double>(2500)) &&
                (id != "distance" || (preserved.interval == std::optional<double>(128) &&
                    record.preset.size.mappings.size() == 1 && record.preset.size.mappings[0].scale == 128 &&
                    record.preset.size.mappings[0].periodic)),
            "Preservation changed native units or discarded unsupported source duration/speed");
  }
  // Optional projection must degrade, rather than reject, a bounded source
  // preset whose inactive declarations exceed the shared knot quota.
  QString budgetCurve;
  for (int i = 0; i < 1024; ++i)
    budgetCurve += QString::number(i * .0009, 'f', 4) + ",0;";
  const auto budgetSensorList = [&](int count) {
    const QStringList ids{"pressure", "xtilt", "ytilt", "rotation", "drawingangle"};
    QString xml = "<params id=\"sensorslist\">";
    for (int i = 0; i < count; ++i)
      xml += "<ChildSensor id=\"" + ids[i] + "\"><curve>" + budgetCurve + "</curve></ChildSensor>";
    return xml + "</params>";
  };
  const auto countProjectedKnots = [](const SourceSensorsV1 &value) {
    size_t count = 0;
    for (const auto &item : value.controls) {
      if (item.commonCurve) count += item.commonCurve->size();
      for (const auto &sensor : item.sensors)
        if (sensor.curve) count += sensor.curve->size();
    }
    return count;
  };
  const auto &budgetTargets = Detail::sourceSensorTargetsV1();
  const QString fiveSensors = budgetSensorList(5);
  require(fiveSensors.size() < 65536, "Authored sensor budget fixture exceeded one XML field budget");
  QMap<QString, QString> budgetFields;
  for (int i = 0; i < 13; ++i) {
    const QString target = budgetTargets[i];
    budgetFields[target + "Sensor"] = fiveSensors;
    budgetFields["Pressure" + target] = "false";
    budgetFields[target + "UseCurve"] = "false";
  }
  const auto projectedBudget = Detail::preserveKppSourceSensors(budgetFields);
  const auto &rejectedGroup = control(projectedBudget, budgetTargets[12]);
  require(countProjectedKnots(projectedBudget) == 12u * 5u * 1024u &&
              rejectedGroup.layout == "unsupported" && rejectedGroup.sensors.empty() &&
              rejectedGroup.unsupportedFields.contains("sensors") &&
              rejectedGroup.enabled == std::optional<bool>(false) &&
              rejectedGroup.useCurve == std::optional<bool>(false) &&
              rejectedGroup.sourceFieldsSha256.size() == 64,
          "Oversized optional sensor block rejected import or lost its flags/hash");
  const auto budgetFixture = fixture(budgetFields);
  require(QImage::fromData(budgetFixture, "PNG").text("preset").toUtf8().size() < 1024 * 1024,
          "Authored aggregate sensor fixture exceeded the KPP container limit");
  const auto budgetImport = importBrush(budgetFixture, "kpp", error);
  require(error.isEmpty() && budgetImport.complete && budgetImport.records.size() == 1 &&
              budgetImport.records[0].sourceSensorsV1 &&
              countProjectedKnots(*budgetImport.records[0].sourceSensorsV1) <= 65536 &&
              control(*budgetImport.records[0].sourceSensorsV1, budgetTargets[12]).layout == "unsupported" &&
              budgetImport.records[0].preset.profile == "groundzero-native-1" &&
              budgetImport.records[0].preset.size.mappings.empty(),
          "Optional metadata overflow prevented bounded inactive KPP native fallback");

  QMap<QString, QString> exactBudgetFields;
  const QString fourSensors = budgetSensorList(4);
  for (int i = 0; i < 16; ++i) {
    const QString base = i < budgetTargets.size() ? "" : "MaskingBrush/Preset/";
    exactBudgetFields[base + budgetTargets[i % budgetTargets.size()] + "Sensor"] = fourSensors;
  }
  const auto boundaryMetadata = Detail::preserveKppSourceSensors(exactBudgetFields);
  require(countProjectedKnots(boundaryMetadata) == 65536,
          "Exact aggregate sensor boundary was prematurely degraded");
  auto overflowFields = exactBudgetFields;
  overflowFields["MaskingBrush/Preset/PressureRatio"] = "false";
  overflowFields["MaskingBrush/Preset/RatioUseCurve"] = "false";
  overflowFields["MaskingBrush/Preset/RatioValue"] = ".25";
  overflowFields["MaskingBrush/Preset/RatiocommonCurve"] = "0,0;1,1;";
  overflowFields["MaskingBrush/Preset/RatioSensor"] = "<params id=\"pressure\"><curve>0,0;1,1;</curve></params>";
  const auto overflowMetadata = Detail::preserveKppSourceSensors(overflowFields);
  const auto &overflowControl = control(overflowMetadata, "Ratio", "masked");
  require(countProjectedKnots(overflowMetadata) == 65536 && !overflowControl.commonCurve &&
              overflowControl.layout == "unsupported" && overflowControl.sensors.empty() &&
              overflowControl.unsupportedFields.contains("commonCurve") &&
              overflowControl.unsupportedFields.contains("sensors") &&
              overflowControl.enabled == std::optional<bool>(false) &&
              overflowControl.strength == std::optional<double>(.25),
          "Curve overflow lost flags or silently retained unbounded projected knots");
  overflowFields["MaskingBrush/Preset/RatiocommonCurve"] = "0,1;1,0;";
  const auto changedOverflow = Detail::preserveKppSourceSensors(overflowFields);
  require(control(changedOverflow, "Ratio", "masked").sourceFieldsSha256 != overflowControl.sourceFieldsSha256,
          "Unsupported curve hash no longer identifies the original raw declaration");

  // This recovery policy belongs only to source projection. Programmatic and
  // portable metadata remain strict at the exact same aggregate boundary.
  auto invalidMetadata = boundaryMetadata;
  invalidMetadata.controls.back().commonCurve = std::vector<Knot>{{0, 0}, {1, 1}};
  Record invalidRecord;
  invalidRecord.sourceSensorsV1 = invalidMetadata;
  const auto invalidEncoding = encode(invalidRecord, error);
  require(!error.isEmpty() && invalidEncoding.isEmpty(),
          "Programmatic sensor metadata silently degraded instead of rejecting overflow");
  Record boundaryRecord;
  boundaryRecord.sourceSensorsV1 = boundaryMetadata;
  const auto boundaryPortable = encodePortable(boundaryRecord, ResourcePackaging::ManifestOnly, error);
  require(error.isEmpty() && !boundaryPortable.isEmpty(), "Exact sensor boundary could not be exported");
  auto overflowPortable = QJsonDocument::fromJson(boundaryPortable).object();
  auto overflowPackage = overflowPortable["package"].toObject();
  auto overflowDeclaration = overflowPackage["sourceSensorsV1"].toObject();
  auto overflowControls = overflowDeclaration["controls"].toArray();
  auto overflowLast = overflowControls.last().toObject();
  overflowLast["commonCurve"] = QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}};
  overflowControls[overflowControls.size() - 1] = overflowLast;
  overflowDeclaration["controls"] = overflowControls;
  overflowPackage["sourceSensorsV1"] = overflowDeclaration;
  overflowPortable["package"] = overflowPackage;
  decode(QJsonDocument(overflowPortable).toJson(), error);
  require(!error.isEmpty(), "Portable decoder accepted aggregate source-sensor knot overflow");

  std::optional<std::vector<Knot>> preflightCurve;
  require(!Detail::sourceSensorKnots(QString(65536, ';'), preflightCurve) && !preflightCurve &&
              !Detail::sourceSensorKnots("0," + QString(32768, ',') + ";1,1;", preflightCurve) &&
              !preflightCurve,
          "Sensor curve delimiter overflow reached unbounded row/pair projection");
  size_t readKnots = 65535;
  bool readRejected = false;
  try { Detail::sourceSensorCurve(QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}, readKnots); }
  catch (const std::exception &) { readRejected = true; }
  require(readRejected && readKnots == 65535,
          "Strict reader charged/allocated typed knots before aggregate preflight");
  readKnots = 65534;
  const auto exactRead = Detail::sourceSensorCurve(
      QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}, readKnots);
  require(exactRead && exactRead->size() == 2 && readKnots == 65536,
          "Strict reader rejected the exact aggregate knot boundary");
  auto invalidLabel = metadata;
  invalidLabel.controls[0].target = QString(65536, 'x');
  bool labelRejected = false;
  try { Detail::sourceSensorsJson(invalidLabel); }
  catch (const std::exception &) { labelRejected = true; }
  require(labelRejected, "Unknown oversized sensor target entered an identity projection");

}
