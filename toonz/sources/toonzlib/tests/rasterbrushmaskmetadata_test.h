#pragma once
// Authored preservation/lifecycle cases; these do not establish mask equations.
void testSourceMaskPreservedResources() {
  QString error;
  const auto fixed = importBrush(RasterBrushTest::maskedPixelKpp(), "kpp", error);
  require(error.isEmpty() && fixed.records.size() == 1 && fixed.records[0].sourceMaskDefinitionV1 &&
              fixed.records[0].sourceMaskDefinitionV1->generated &&
              fixed.records[0].sourceMaskDefinitionV1->generated->diameter == 12 &&
              fixed.records[0].sourceMaskDefinitionV1->controlValues["Opacity"].toDouble() == .5 &&
              fixed.records[0].preset.sourceMasked.enabled,
          "Fixed generated source mask preservation changed measured admission");
  const auto dynamic = importBrush(RasterBrushTest::maskedPixelKpp("dynamic"), "kpp", error);
  require(error.isEmpty() && dynamic.records.size() == 1 && dynamic.records[0].sourceMaskDefinitionV1 &&
              dynamic.records[0].sourceSensorsV1 && !dynamic.records[0].preset.sourceMasked.enabled,
          "Unmeasured source mask became active or lost its declarations");
  bool retainedSize = false;
  for (const auto &control : dynamic.records[0].sourceSensorsV1->controls)
    if (control.scope == "masked" && control.target == "Size") retainedSize = control.enabled.value_or(false);
  require(retainedSize, "Masked control scope was folded into primary controls");

  QImage preset = QImage::fromData(RasterBrushTest::maskedPixelKpp(), "PNG");
  QString xml = preset.text("preset");
  const QString tag = "<param name=\"MaskingBrush/Preset/brush_definition\">";
  const int begin = xml.indexOf(tag), end = xml.indexOf("</param>", begin);
  require(begin >= 0 && end > begin, "Missing authored secondary definition");
  const QString definition = "<Brush type=\"png_brush\" filename=\"mask.png\" brushApplication=\"0\" "
      "scale=\"1.5\" spacing=\".23\" angle=\"17\" useAutoSpacing=\"0\"/>";
  xml.replace(begin + tag.size(), end - begin - tag.size(), definition.toHtmlEscaped());
  preset.setText("preset", xml);
  QByteArray presetBytes;
  QBuffer presetBuffer(&presetBytes); presetBuffer.open(QIODevice::WriteOnly);
  require(preset.save(&presetBuffer, "PNG"), "Cannot encode authored mask preset");
  QImage mask(2, 1, QImage::Format_ARGB32);
  mask.setPixel(0, 0, qRgba(17, 93, 201, 255));
  mask.setPixel(1, 0, qRgba(201, 17, 93, 85));
  QByteArray pixels;
  QBuffer pixelBuffer(&pixels); pixelBuffer.open(QIODevice::WriteOnly);
  require(mask.save(&pixelBuffer, "PNG"), "Cannot encode authored alpha-application resource");
  QByteArray manifest = "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\">"
      "<manifest:file-entry manifest:full-path=\"paintoppresets/masked.kpp\" manifest:media-type=\"paintoppresets\"/>"
      "<manifest:file-entry manifest:full-path=\"brushes/mask.png\" manifest:media-type=\"brushes\"/>"
      "</manifest:manifest>";
  const auto bundle = archive({{"mimetype", "application/x-krita-resourcebundle"},
      {"META-INF/manifest.xml", manifest}, {"paintoppresets/masked.kpp", presetBytes},
      {"brushes/mask.png", pixels}}, true);
  const auto imported = importBrush(bundle, "bundle", error);
  require(error.isEmpty() && imported.complete && imported.records.size() == 1, qPrintable(error));
  const auto original = imported.records.front();
  require(original.sourceMaskDefinitionV1 && original.sourceMaskDefinitionV1->tipImage &&
              original.sourceMaskDefinitionV1->tipKind == "decoded-resource" &&
              original.sourceMaskDefinitionV1->application == "alpha-mask" &&
              original.sourceMaskDefinitionV1->geometry["angle"].toDouble() == 17 &&
              original.sourceMaskDefinitionV1->geometry["spacing"].toDouble() == .23 &&
              original.sourceMaskDefinitionV1->tipImage->color &&
              original.sourceMaskDefinitionV1->resourceEncoding == "canonical-rgba8-srgb" &&
              original.sourceMaskDefinitionV1->unsupportedFeatures.contains("unverified-resource-color-contract") &&
              original.sourceMaskDefinitionV1->sourceResourceSha256 == QString::fromLatin1(
                  QCryptographicHash::hash(pixels, QCryptographicHash::Sha256).toHex()) &&
              original.sourceMaskDefinitionV1->tipImage->pixels[0].r == 17 / 255.0 &&
              !original.preset.sourceMasked.enabled && !original.preset.secondary.enabled,
          "Mask resource was baked, activated, or replaced by native Secondary");
  const auto inventory = resources(original, error);
  require(error.isEmpty() && inventory.size() == 1 && inventory[0].color,
          "Mask-only resource is absent from its content-hash graph");
  const auto native = encode(original, error);
  const auto restored = decode(native, error);
  require(error.isEmpty() && restored.sourceMaskDefinitionV1 && restored.sourceMaskDefinitionV1->tipImage &&
              restored.sourceMaskDefinitionV1->tipImage->pixels.size() == 2 &&
              restored.sourceMaskDefinitionV1->geometry == original.sourceMaskDefinitionV1->geometry,
          "Native codec lost preserved mask resource or geometry");
  QTemporaryDir worker;
  const auto wire = encodeWorkerRecord(original, worker.path(), error);
  const auto transferred = decodeWorkerRecord(wire, worker.path(), error);
  require(error.isEmpty() && transferred.sourceMaskDefinitionV1 && transferred.sourceMaskDefinitionV1->tipImage &&
              transferred.sourceMaskDefinitionV1->tipImage->pixels.size() == 2,
          "Raw worker protocol lost the source-only mask image");
  decodeWorkerRecord(wire, worker.path(), error, 2 * sizeof(Pixel) - 1);
  require(!error.isEmpty(), "Worker mask image escaped its decoded allocation budget");
  const auto portable = encodePortable(original, ResourcePackaging::PrivateCopy, error);
  auto info = inspectPortable(portable, error);
  require(error.isEmpty() && info.resources.size() == 1 && info.resources[0].embedded &&
              !portable.contains("mask.png"), "Portable mask graph lost its resource or leaked resolver names");
  const auto manifestOnly = encodePortable(original, ResourcePackaging::ManifestOnly, error);
  info = inspectPortable(manifestOnly, error);
  require(error.isEmpty() && info.resources.size() == 1 && !info.resources[0].embedded,
          "Manifest-only export embedded the unpermitted mask resource");
  // Manifest-only declarations are checked before any pixel binding/repair.
  const auto validManifest = QJsonDocument::fromJson(manifestOnly).object();
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto conflicting = validManifest;
    auto entries = conflicting["resources"].toObject();
    auto package = conflicting["package"].toObject();
    auto declaration = package["sourceMaskDefinitionV1"].toObject();
    if (mutation == 0) {
      const QString id = entries.keys().front();
      auto entry = entries[id].toObject();
      entry["color"] = false;
      entries[id] = entry;
    }
    if (mutation == 1) declaration["resourceEncoding"] = "alpha-u8";
    if (mutation == 2) {
      auto tags = declaration["unsupportedFeatures"].toArray();
      for (int i = tags.size() - 1; i >= 0; --i)
        if (tags[i] == "unverified-resource-color-contract") tags.removeAt(i);
      declaration["unsupportedFeatures"] = tags;
    }
    package["sourceMaskDefinitionV1"] = declaration;
    conflicting["package"] = package;
    conflicting["resources"] = entries;
    const auto conflictingBytes = QJsonDocument(conflicting).toJson();
    inspectPortable(conflictingBytes, error);
    require(!error.isEmpty(), "Unbound mask color/encoding declarations bypassed portable inspection");
    require(preparePortableRepair(conflictingBytes, error).isEmpty() && !error.isEmpty(),
            "Unbound inconsistent mask declarations became a repair manifest");
    QStringList matches;
    require(relinkPortable(conflictingBytes, {original}, matches, error).isEmpty() && !error.isEmpty(),
            "Pixel relinking bypassed inconsistent mask color/encoding declarations");
  }
  const auto missingRepair = preparePortableRepair(manifestOnly, error);
  const auto missingInfo = inspectPortable(missingRepair, error);
  require(error.isEmpty() && missingInfo.resources.size() == 1 &&
              missingInfo.resources[0].color && !missingInfo.resources[0].embedded,
          "Agreeing missing mask declarations stopped being repairable");
  decode(manifestOnly, error);
  require(!error.isEmpty(), "Missing source mask resource silently became a complete brush");
  QStringList matched;
  const auto repaired = relinkPortable(manifestOnly, {original}, matched, error);
  const auto repairedRecord = decode(repaired, error);
  require(error.isEmpty() && matched.size() == 1 && repairedRecord.sourceMaskDefinitionV1 &&
              repairedRecord.sourceMaskDefinitionV1->tipImage && !repairedRecord.preset.sourceMasked.enabled,
          "Portable repair lost or activated the source-only mask image");
  const auto retry = preparePortableRepair(portable, error);
  const auto retryInfo = inspectPortable(retry, error);
  require(error.isEmpty() && retryInfo.resources.size() == 1 && !retryInfo.resources[0].embedded,
          "Repair retry discarded the mask reference instead of only its embedded pixels");
  Record permitted = original;
  permitted.resourceRights[inventory[0].hash] = QJsonObject{{"redistribution", "permitted"}, {"license", "CC0-1.0"}};
  info = inspectPortable(encodePortable(permitted, ResourcePackaging::PermittedOnly, error), error);
  require(error.isEmpty() && info.resources.size() == 1 && info.resources[0].embedded,
          "Mask resource rights were not applied by permitted-only export");
  auto malformed = QJsonDocument::fromJson(native).object();
  auto badDefinition = malformed["sourceMaskDefinitionV1"].toObject();
  badDefinition["activate"] = true;
  malformed["sourceMaskDefinitionV1"] = badDefinition;
  decode(QJsonDocument(malformed).toJson(), error);
  require(!error.isEmpty(), "Unknown mask activation metadata was accepted");
  malformed = QJsonDocument::fromJson(native).object();
  badDefinition = malformed["sourceMaskDefinitionV1"].toObject();
  badDefinition["image"] = "";
  malformed["sourceMaskDefinitionV1"] = badDefinition;
  decode(QJsonDocument(malformed).toJson(), error);
  require(!error.isEmpty(), "Decoded mask declaration accepted an empty resource binding");
  manifest.replace("</manifest:manifest>",
      "<manifest:file-entry manifest:full-path=\"paintoppresets/second.kpp\" manifest:media-type=\"paintoppresets\"/></manifest:manifest>");
  const auto duplicateBundle = archive({{"mimetype", "application/x-krita-resourcebundle"},
      {"META-INF/manifest.xml", manifest}, {"paintoppresets/masked.kpp", presetBytes},
      {"paintoppresets/second.kpp", presetBytes}, {"brushes/mask.png", pixels}}, true);
  const auto bounded = importBrush(duplicateBundle, "bundle", error, nullptr, 2 * (sizeof(Pixel) + 16));
  require(!error.isEmpty() && !bounded.complete,
          "Repeated source-only mask resources escaped aggregate bundle accounting");
}

void testSourceMaskProjectionBounds() {
  const QString prefix = "MaskingBrush/Preset/";
  QMap<QString, QString> fields{{"MaskingBrush/Enabled", "true"},
      {prefix + "brush_definition", "<Brush type=\"auto_brush\" spacing=\".25\">"
          "<MaskGenerator id=\"default\" type=\"rect\" diameter=\"12\" ratio=\".5\" "
          "hfade=\"1\" vfade=\"1\" spikes=\"2\" spacing=\".91\"/></Brush>"}};
  QString name, md5;
  auto declaration = Detail::preserveKppSourceMask(fields, name, md5);
  require(declaration && declaration->geometry["spacing"].toDouble() == .25 &&
              declaration->unsupportedFeatures.contains("unrecognized-geometry"),
          "Misplaced generator attributes overwrote independent brush geometry");
  fields[prefix + "brush_definition"] =
      "<Brush type=\"auto_brush\" spacing=\".25\"><MaskGenerator id=\"soft\" type=\"rect\" "
      "diameter=\"12\" ratio=\".5\" hfade=\"1\" vfade=\"1\" spikes=\"2\" softness_curve=\"" +
      QString(65536, ';') + "\"/></Brush>";
  declaration = Detail::preserveKppSourceMask(fields, name, md5);
  require(declaration && !declaration->generated &&
              declaration->unsupportedFeatures.contains("unprojectable-generated"),
          "Oversized token count entered a preserved source Soft curve");
  QString error;
  const auto imported = importBrush(RasterBrushTest::maskedPixelKpp(), "kpp", error);
  require(error.isEmpty() && imported.records.size() == 1 && imported.records[0].sourceMaskDefinitionV1 &&
              imported.records[0].sourceSensorsV1, "Missing authored mask consistency baseline");
  const Record baseline = imported.records.front();
  const auto encodedBaseline = encode(baseline, error);
  require(error.isEmpty() && !encodedBaseline.isEmpty(), "Consistent duplicated mask declarations rejected");
  const auto baselineJson = QJsonDocument::fromJson(encodedBaseline).object();
  const QJsonObject contradictoryGeometry{{"generator", "gauss"}, {"shape", "circle"},
      {"diameter", 13}, {"ratio", .75}, {"hfade", .5}, {"vfade", .5}, {"spikes", 3},
      {"antialiasEdges", true}, {"tipType", "png_brush"}};
  for (auto field = contradictoryGeometry.begin(); field != contradictoryGeometry.end(); ++field) {
    Record contradictory = baseline;
    contradictory.sourceMaskDefinitionV1->geometry[field.key()] = field.value();
    const auto refused = encode(contradictory, error);
    require(!error.isEmpty() && refused.isEmpty(), "Programmatic mask accepted contradictory generated geometry");
    auto document = baselineJson;
    auto preserved = document["sourceMaskDefinitionV1"].toObject();
    auto geometry = preserved["geometry"].toObject();
    geometry[field.key()] = field.value();
    preserved["geometry"] = geometry;
    document["sourceMaskDefinitionV1"] = preserved;
    decode(QJsonDocument(document).toJson(), error);
    require(!error.isEmpty(), "Native mask decoder accepted contradictory generated geometry");
  }
  Record missingAntialias = baseline;
  missingAntialias.sourceMaskDefinitionV1->generated->antialiasEdges.reset();
  encode(missingAntialias, error);
  require(!error.isEmpty(), "Explicit geometry antialias flag acquired an invented typed default");

  Record sparse;
  sparse.sourceMaskDefinitionV1 = *baseline.sourceMaskDefinitionV1;
  sparse.sourceMaskDefinitionV1->generated = SourceGeneratedDefinitionV1{};
  sparse.sourceMaskDefinitionV1->geometry = QJsonObject{{"angle", 17}, {"spacing", .23}, {"scale", 1.5}};
  const auto sparseCopy = decode(encode(sparse, error), error);
  require(error.isEmpty() && sparseCopy.sourceMaskDefinitionV1 &&
              sparseCopy.sourceMaskDefinitionV1->geometry == sparse.sourceMaskDefinitionV1->geometry &&
              !sparseCopy.sourceMaskDefinitionV1->geometry.contains("diameter") &&
              !sparseCopy.sourceMaskDefinitionV1->geometry.contains("antialiasEdges") &&
              !sparseCopy.sourceMaskDefinitionV1->generated->antialiasEdges &&
              !sparseCopy.preset.sourceMasked.enabled && !sparseCopy.preset.secondary.enabled,
          "Typed generated defaults fabricated missing source geometry or activated runtime");
  Record unsupported;
  unsupported.sourceMaskDefinitionV1 = *baseline.sourceMaskDefinitionV1;
  unsupported.sourceMaskDefinitionV1->tipKind = "unsupported";
  unsupported.sourceMaskDefinitionV1->generated.reset();
  unsupported.sourceMaskDefinitionV1->unsupportedFeatures << "unprojectable-generated";
  const auto unsupportedCopy = decode(encode(unsupported, error), error);
  require(error.isEmpty() && unsupportedCopy.sourceMaskDefinitionV1 &&
              unsupportedCopy.sourceMaskDefinitionV1->tipKind == "unsupported" &&
              !unsupportedCopy.sourceMaskDefinitionV1->generated,
          "Honest unsupported source geometry gained a fabricated generated projection");

  Record resource;
  resource.sourceMaskDefinitionV1 = *baseline.sourceMaskDefinitionV1;
  resource.sourceMaskDefinitionV1->generated.reset();
  resource.sourceMaskDefinitionV1->tipKind = "decoded-resource";
  resource.sourceMaskDefinitionV1->resourceEncoding = "alpha-u8";
  resource.sourceMaskDefinitionV1->geometry = QJsonObject{{"tipType", "png_brush"}};
  auto image = std::make_shared<Image>();
  image->width = image->height = 1;
  image->pixels = {{0, 0, 0, 1}};
  resource.sourceMaskDefinitionV1->tipImage = image;
  require(!encode(resource, error).isEmpty() && error.isEmpty(), "Consistent single mask resource rejected");
  for (const QString type : {QString("auto_brush"), QString("gih_brush")}) {
    auto contradictory = resource;
    contradictory.sourceMaskDefinitionV1->geometry["tipType"] = type;
    encode(contradictory, error);
    require(!error.isEmpty(), "Decoded mask resource accepted contradictory tip type");
  }

  const auto maskedOpacity = [](Record &record) -> SourceSensorControlV1 & {
    for (auto &control : record.sourceSensorsV1->controls)
      if (control.scope == "masked" && control.target == "Opacity") return control;
    throw std::runtime_error("Missing authored masked opacity declaration");
  };
  Record inconsistentValues = baseline;
  inconsistentValues.sourceMaskDefinitionV1->controlValues["Opacity"] = .75;
  require(encode(inconsistentValues, error).isEmpty() && !error.isEmpty(),
          "Programmatic metadata accepted disagreeing masked targetValue projections");
  auto conflictingNative = baselineJson;
  auto conflictingMask = conflictingNative["sourceMaskDefinitionV1"].toObject();
  auto values = conflictingMask["controlValues"].toObject();
  values["Opacity"] = .75;
  conflictingMask["controlValues"] = values;
  conflictingNative["sourceMaskDefinitionV1"] = conflictingMask;
  decode(QJsonDocument(conflictingNative).toJson(), error);
  require(!error.isEmpty(), "Native decoder accepted disagreeing masked targetValue projections");
  QTemporaryDir worker;
  const auto wire = encodeWorkerRecord(baseline, worker.path(), error);
  require(error.isEmpty(), "Consistent mask worker metadata rejected");
  auto conflictingWorker = QJsonDocument::fromJson(wire).object();
  conflictingWorker["sourceMaskDefinitionV1"] = conflictingMask;
  decodeWorkerRecord(QJsonDocument(conflictingWorker).toJson(), worker.path(), error);
  require(!error.isEmpty(), "Worker decoder accepted disagreeing masked targetValue projections");
  const auto portable = encodePortable(baseline, ResourcePackaging::ManifestOnly, error);
  require(error.isEmpty(), "Consistent mask portable metadata rejected");
  auto conflictingPortable = QJsonDocument::fromJson(portable).object();
  auto package = conflictingPortable["package"].toObject();
  package["sourceMaskDefinitionV1"] = conflictingMask;
  conflictingPortable["package"] = package;
  const auto conflictingBytes = QJsonDocument(conflictingPortable).toJson();
  inspectPortable(conflictingBytes, error);
  require(!error.isEmpty(), "Portable inspection accepted disagreeing masked targetValue projections");
  decode(conflictingBytes, error);
  require(!error.isEmpty(), "Portable decode accepted disagreeing masked targetValue projections");
  require(preparePortableRepair(conflictingBytes, error).isEmpty() && !error.isEmpty(),
          "Portable repair bypassed masked metadata consistency");

  Record missingValue = baseline;
  missingValue.sourceMaskDefinitionV1->controlValues.remove("Opacity");
  auto missingCopy = decode(encode(missingValue, error), error);
  require(error.isEmpty() && !missingCopy.sourceMaskDefinitionV1->controlValues.contains("Opacity") &&
              maskedOpacity(missingCopy).strength == std::optional<double>(.5),
          "Mask scalar absence was filled from separate sensor declarations");
  Record missingStrength = baseline;
  maskedOpacity(missingStrength).strength.reset();
  missingCopy = decode(encode(missingStrength, error), error);
  require(error.isEmpty() && !maskedOpacity(missingCopy).strength &&
              missingCopy.sourceMaskDefinitionV1->controlValues["Opacity"].toDouble() == .5,
          "Sensor scalar absence was filled from separate mask declarations");
  Record primaryIndependent = baseline;
  for (auto &control : primaryIndependent.sourceSensorsV1->controls)
    if (control.scope == "primary" && control.target == "Opacity") control.strength = .75;
  require(!encode(primaryIndependent, error).isEmpty() && error.isEmpty(),
          "Primary controls were incorrectly bound to masked scalar declarations");

}
