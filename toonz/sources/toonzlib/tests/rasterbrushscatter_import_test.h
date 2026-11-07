#pragma once
// Included beside the existing isolated preset regression groups.
QByteArray sourceScatterKpp(bool enabled, bool axisX, bool axisY,
                           const QString &amount = "1", int diameter = 8,
                           bool useCurve = false, bool unknownActive = false) {
  QImage image = QImage::fromData(RasterBrushTest::hardPixelKpp("buildUp"), "PNG");
  QString xml = image.text("preset");
  const QString before = "<param name=\"PressureScatter\">false</param>";
  require(xml.count(before) == 1, "Original scatter fixture lost its explicit enable flag");
  xml.replace(before, "<param name=\"PressureScatter\">" +
                      QString(enabled ? "true" : "false") + "</param>");
  auto replace = [&](const QString &a, const QString &b) {
    require(xml.count(a.toHtmlEscaped()) == 1, "Original scatter fixture attribute is not unique");
    xml.replace(a.toHtmlEscaped(), b.toHtmlEscaped());
  };
  replace("diameter=\"32\"", "diameter=\"" + QString::number(diameter) + "\"");
  replace("spacing=\"0.1\"", "spacing=\"4\"");
  replace("useAutoSpacing=\"0\"", "useAutoSpacing=\"0\" autoSpacingCoeff=\"1\"");
  QString extra;
  auto param = [&](const QString &name, const QString &value) {
    extra += "<param name=\"" + name + "\">" + value.toHtmlEscaped() + "</param>";
  };
  param("ScatterValue", amount);
  param("ScatterUseCurve", useCurve ? "true" : "false");
  param("ScatterUseSameCurve", "true");
  param("ScattercurveMode", "0");
  param("ScattercommonCurve", "0,1;1,1;");
  param("ScatterSensor", "<params id=\"pressure\"><curve>0,1;1,1;</curve></params>");
  param("Scattering/AxisX", axisX ? "true" : "false");
  param("Scattering/AxisY", axisY ? "true" : "false");
  if (unknownActive) param("Scattering/UnknownActive", "true");
  xml.replace("</Preset>", extra + "</Preset>");
  image.setText("preset", xml);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
          "Cannot encode original scatter source fixture");
  return bytes;
}
void testSourceScatterImportLifecycle() {
  QString error;
  for (int axes = 0; axes < 3; ++axes) {
    const bool x = axes != 1, y = axes != 0;
    const auto imported = importBrush(sourceScatterKpp(true, x, y), "kpp", error);
    require(error.isEmpty() && imported.complete && imported.records.size() == 1,
            "Measured explicit source scatter controls did not import");
    const auto record = imported.records.front();
    const auto same = [&](const Record &restored) {
      require(error.isEmpty() && restored.preset.profile == "krita-5.3.4.1-scatter-candidate-1" &&
                  restored.preset.sourceScatterV1 && restored.preset.sourceScatterV1->amount == 1 &&
                  restored.preset.sourceScatterV1->axisX == x && restored.preset.sourceScatterV1->axisY == y &&
                  restored.preset.scatter.value == 0 && restored.preset.scatter.mappings.empty() &&
                  restored.preset.varySeedPerStroke &&
                  !restored.referenceVerified && !restored.provenance["sourceReferenceVerified"].toBool(),
              "Source scatter lost process state or gained unsupported source/RNG verification");
    };
    same(record);
    const auto raw = record.provenance["sourceScatterControlsV1"].toObject();
    require(raw["PressureScatter"] == "true" && raw["ScatterValue"] == "1" &&
                raw["ScatterUseCurve"] == "false" && raw["ScatterSensor"].toString().contains("pressure"),
            "Original source scatter declarations were dropped during import");
    const auto nativePackage = encode(record, error);
    require(error.isEmpty() && !nativePackage.isEmpty(), "Source scatter native package encoding failed");
    const auto reopened = decode(nativePackage, error);
    same(reopened);
    require(reopened.provenance["sourceScatterControlsV1"] == raw,
            "Private/native source scatter parameter readback was lost");
    const auto portable = encodePortable(record, ResourcePackaging::PrivateCopy, error);
    require(error.isEmpty() && !portable.isEmpty(), "Source scatter portable encoding failed");
    same(decode(portable, error));
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create source scatter worker fixture");
    const auto wire = encodeWorkerRecord(record, directory.path(), error);
    require(error.isEmpty() && !wire.isEmpty(), "Source scatter worker encoding failed");
    same(decodeWorkerRecord(wire, directory.path(), error));
    auto forged = record;
    forged.preset.profile = "groundzero-native-1";
    require(encode(forged, error).isEmpty() && !error.isEmpty(),
            "Active source scatter silently survived native profile reassignment");
    auto root = QJsonDocument::fromJson(nativePackage).object();
    const auto original = root["settings"].toObject();
    for (int mutation = 0; mutation < 6; ++mutation) {
      auto setting = original;
      auto process = setting["sourceScatterV1"].toObject();
      if (mutation == 0) process.remove("axisX");
      if (mutation == 1) process["amount"] = true;
      if (mutation == 2) process["axisY"] = 1;
      if (mutation == 3) process["sourceReferenceVerified"] = true;
      if (mutation == 4) process["amount"] = .5;
      if (mutation == 5) { process["axisX"] = false; process["axisY"] = false; }
      setting["sourceScatterV1"] = process;
      root["settings"] = setting;
      decode(QJsonDocument(root).toJson(), error);
      require(!error.isEmpty(), "Malformed or unmeasured source scatter state decoded");
    }
    const auto disabled = importBrush(sourceScatterKpp(false, x, y), "kpp", error);
    require(error.isEmpty() && disabled.complete && disabled.records.size() == 1 &&
                disabled.records.front().preset.profile == "krita-5.3.4.1-hard-pixel-candidate-1" &&
                !disabled.records.front().preset.sourceScatterV1 &&
                disabled.records.front().provenance["sourceScatterControlsV1"].toObject()["ScatterValue"] == "1",
            "Disabled scatter controls lost metadata or fell back to a different native mask");
  }
  for (const auto &bytes : {
           sourceScatterKpp(true, true, false, ".5"), sourceScatterKpp(true, true, false, "1", 16),
           sourceScatterKpp(true, true, false, "1", 8, true), sourceScatterKpp(true, false, false),
           sourceScatterKpp(true, true, false, "1", 8, false, true),
           sourceScatterKpp(true, true, false, "nan")}) {
    const auto rejected = importBrush(bytes, "kpp", error);
    require(!rejected.complete && rejected.records.empty() && !error.isEmpty(),
            "Unmeasured active source scatter was silently ignored or admitted");
  }
}
