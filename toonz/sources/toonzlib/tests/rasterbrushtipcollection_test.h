#pragma once
// Authored sampled identities and PNGs; this does not establish source behavior.
void testAbrTipCollectionIdentity() {
  const QString composed = QString::fromUtf8("Original \xc3\xa9 / ") +
                           QString(140, QChar(0x7b46));
  const QString decomposed = composed.normalized(QString::NormalizationForm_D);
  auto namedSample = [](const QString &name) {
    auto file = sampledAbr(2);
    const auto be = [](QByteArray &out, unsigned value, int bytes) {
      for (int byte = bytes - 1; byte >= 0; --byte) out.append(char(value >> (8 * byte)));
    };
    QByteArray encoded;
    be(encoded, unsigned(name.size() + 1), 4);
    for (QChar unit : name) be(encoded, unit.unicode(), 2);
    be(encoded, 0, 2);
    file.replace(16, 4, encoded);
    QByteArray length;
    be(length, unsigned(file.size() - 10), 4);
    file.replace(6, 4, length);
    return file;
  };
  auto bytes = namedSample(composed);
  bytes[3] = 2;
  bytes += namedSample(decomposed).mid(4);
  QString error;
  const auto imported = importAbrTips(bytes, error);
  require(error.isEmpty() && imported.complete && imported.records.size() == 2,
          "Cannot decode authored collection identity fixture");
  const QString decomposedFilename = QString("tip_") + QChar(0x65) + QChar(0x301) + ".png";
  const QStringList filenames{decomposedFilename, "tip_001 (2).png"};
  auto write = [](const QString &path, const QByteArray &data) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                file.write(data) == data.size() && file.flush(),
            "Cannot write collection identity fixture");
    file.close();
    require(file.error() == QFileDevice::NoError, "Cannot close collection identity fixture");
  };
  auto png = [](const Record &record) {
    const auto &tip = *record.preset.tip.images.front();
    QImage image(tip.width, tip.height, QImage::Format_Grayscale8);
    require(!image.isNull(), "Cannot allocate authored sampled-tip PNG");
    for (int y = 0; y < tip.height; ++y)
      for (int x = 0; x < tip.width; ++x)
        image.scanLine(y)[x] = 255 - int(std::floor(tip.pixels[size_t(y) * tip.width + x].a * 255 + .5));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
            "Cannot encode authored sampled-tip PNG");
    return encoded;
  };
  QTemporaryDir library;
  require(library.isValid(), "Cannot create collection identity fixture library");
  QTemporaryDir stage(library.filePath(".abr-import-XXXXXX"));
  require(stage.isValid(), "Cannot create collection identity fixture stage");
  const QByteArray firstPng = png(imported.records.front());
  for (int i = 0; i < filenames.size(); ++i)
    write(stage.filePath(filenames[i]), png(imported.records[size_t(i)]));
  require(Detail::writeAbrTipCollectionManifest(stage.path(), imported.records, filenames, error) && error.isEmpty(),
          "Sampled collection identity could not be persisted");
  const auto original = Detail::readAbrTipCollectionManifest(stage.path(), error);
  require(error.isEmpty() && !original.isEmpty(), "Sampled collection manifest did not round-trip");
  const auto samples = original["samples"].toArray();
  require(original["extractionKind"] == "sampled-images-only" &&
              original["source"].toObject()["sha256"] == imported.records.front().provenance["sourceSha256"] &&
              samples.size() == 2 && samples[0].toObject()["ordinal"] == 0 && samples[1].toObject()["ordinal"] == 1 &&
              samples[0].toObject()["name"] == composed && samples[1].toObject()["name"] == decomposed &&
              samples[0].toObject()["name"] != samples[1].toObject()["name"] &&
              samples[0].toObject()["file"] == filenames[0] && samples[1].toObject()["file"] == filenames[1] &&
              samples[0].toObject()["sampleId"] == "" && samples[0].toObject()["sourceDepth"] == 8 &&
              samples[0].toObject()["width"] == 2 && samples[0].toObject()["height"] == 1 &&
              samples[0].toObject()["outputSha256"] == QString::fromLatin1(
                  QCryptographicHash::hash(firstPng, QCryptographicHash::Sha256).toHex()),
          "Manifest lost parsed Unicode spelling/order/depth/geometry or output identity");
  // Author an actual NFD filename, then retain its NFC equivalent in JSON.
  // This exercises member lookup on normalization-sensitive Linux as well as
  // supported filesystems which expose one canonical spelling through Qt.
  auto canonicalFilenameManifest = original;
  auto canonicalSamples = samples;
  auto canonicalSample = canonicalSamples[0].toObject();
  canonicalSample["file"] = filenames[0].normalized(QString::NormalizationForm_C);
  canonicalSamples[0] = canonicalSample;
  canonicalFilenameManifest["samples"] = canonicalSamples;
  const QString stagedManifest = stage.filePath(QString::fromLatin1(Detail::abrTipCollectionManifestName));
  write(stagedManifest, QJsonDocument(canonicalFilenameManifest).toJson());
  require(Detail::readAbrTipCollectionManifest(stage.path(), error) == canonicalFilenameManifest && error.isEmpty(),
          "Canonical-equivalent actual Unicode PNG filename lost collection identity");
  write(stagedManifest, QJsonDocument(original).toJson());
#ifdef __linux__
  const QString alias = stage.filePath(filenames[0].normalized(QString::NormalizationForm_C));
  QFile colliding(alias);
  require(colliding.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
              colliding.write(firstPng) == firstPng.size() && colliding.flush(),
          "Cannot author distinct canonical-equivalent Linux filename members");
  colliding.close();
  require(Detail::readAbrTipCollectionManifest(stage.path(), error).isEmpty() && error.contains("colliding"),
          "Canonical-equivalent actual collection members were accepted twice");
  require(QFile::remove(alias), "Cannot retire canonical collision fixture");
#endif
  require(publishedLibraryFiles(library.path(), {"*.png"}).isEmpty(),
          "Unpublished manifest collection leaked into the selector");
  const QString destination = library.filePath("published");
  require(Detail::publishLibraryEntryNoReplace(stage.path(), destination, error) == Detail::LibraryPublication::Published,
          "Manifest and PNG collection could not publish atomically");
  stage.setAutoRemove(false);
  require(Detail::readAbrTipCollectionManifest(destination, error) == original && error.isEmpty() &&
              publishedLibraryFiles(library.path(), {"*.png"}).size() == 2,
          "Published identity changed or manifest became a selectable PNG");
  const QString manifestPath = QDir(destination).filePath(QString::fromLatin1(Detail::abrTipCollectionManifestName));
  const QByteArray manifestBytes = QJsonDocument(original).toJson();
  require(!Detail::writeAbrTipCollectionManifest(destination, imported.records, filenames, error) &&
              !error.isEmpty() && Detail::readAbrTipCollectionManifest(destination, error) == original,
          "Writing identity replaced an existing manifest");

  for (int mutation = 0; mutation < 18; ++mutation) {
    auto root = original;
    auto items = root["samples"].toArray();
    auto first = items[0].toObject();
    if (mutation == 0) first.remove("name");
    if (mutation == 1) first.remove("sourceDepth");
    if (mutation == 2) first["sourceDepth"] = true;
    if (mutation == 3) first["file"] = "../escape.png";
    if (mutation == 4) first["file"] = "folder\\escape.png";
    if (mutation == 5) first["outputSha256"] = "invalid";
    if (mutation == 6) first["width"] = 0;
    if (mutation == 7) first["sampleId"] = false;
    if (mutation == 8) first["name"] = QString(4097, 'x');
    if (mutation == 9) first["sourceReferenceVerified"] = true;
    if (mutation == 10) first["ordinal"] = 1;
    if (mutation == 11) root["sampleCount"] = 1;
    if (mutation == 12) root["version"] = true;
    if (mutation == 13) root["sourcePath"] = "untrusted source location";
    if (mutation == 14) {
      auto source = root["source"].toObject();
      source["version"] = 3;
      root["source"] = source;
    }
    if (mutation == 15) {
      auto second = items[1].toObject();
      second["file"] = filenames[0].toUpper();
      items[1] = second;
    }
    if (mutation == 16) root["extractionKind"] = "descriptor-preset-import";
    if (mutation == 17) first["outputSha256"] = first["outputSha256"].toString() + "\n";
    items[0] = first;
    root["samples"] = items;
    require(Detail::decodeAbrTipCollectionManifest(QJsonDocument(root).toJson(), error).isEmpty() && !error.isEmpty(),
            "Malformed collection identity or source claim decoded");
  }
  auto tampered = original;
  auto items = tampered["samples"].toArray();
  auto first = items[0].toObject();
  first["outputSha256"] = QString(64, '0');
  items[0] = first;
  tampered["samples"] = items;
  write(manifestPath, QJsonDocument(tampered).toJson());
  require(Detail::readAbrTipCollectionManifest(destination, error).isEmpty() && error.contains("digest"),
          "Manifest accepted a different PNG digest");
  first["outputSha256"] = samples[0].toObject()["outputSha256"];
  first["width"] = 3;
  items[0] = first;
  tampered["samples"] = items;
  write(manifestPath, QJsonDocument(tampered).toJson());
  require(Detail::readAbrTipCollectionManifest(destination, error).isEmpty() && error.contains("geometry"),
          "Manifest accepted dimensions different from the emitted PNG");
  write(manifestPath, manifestBytes);
  write(QDir(destination).filePath(filenames[0]), firstPng + "tampered");
  require(Detail::readAbrTipCollectionManifest(destination, error).isEmpty() && error.contains("digest"),
          "Collection identity accepted modified output bytes");
  write(QDir(destination).filePath(filenames[0]), firstPng);
  const QString extra = QDir(destination).filePath("unexpected.txt");
  write(extra, "unexpected");
  require(Detail::readAbrTipCollectionManifest(destination, error).isEmpty() && error.contains("members"),
          "Collection identity accepted an unlisted member");
  require(QFile::remove(extra), "Cannot retire extra collection fixture member");
  require(Detail::readAbrTipCollectionManifest(destination, error) == original && error.isEmpty(),
          "Collection identity did not recover after restoring authored outputs");

  // Source IDs are byte-preserving Latin-1; source 16-bit depth remains distinct
  // from the emitted 8-bit grayscale representation.
  for (int variant = 0; variant < 2; ++variant) {
    auto input = boundAbr();
    if (variant == 1) {
      input = sampledAbr(1);
      input[input.size() - 4] = 16;
      input.replace(input.size() - 2, 2, QByteArray::fromHex("ffff0080"));
      input[9] = char(static_cast<unsigned char>(input[9]) + 2);
    }
    const auto source = importAbrTips(input, error);
    require(error.isEmpty() && source.complete && source.records.size() == 1,
            "Cannot decode sampled ID/depth manifest fixture");
    QTemporaryDir output;
    require(output.isValid(), "Cannot create source ID/depth collection");
    write(output.filePath("sample.png"), png(source.records.front()));
    require(Detail::writeAbrTipCollectionManifest(output.path(), source.records, {"sample.png"}, error),
            "Source ID/depth identity could not be written");
    const auto identity = Detail::readAbrTipCollectionManifest(output.path(), error);
    require(error.isEmpty() && identity["samples"].toArray().size() == 1,
            "Cannot read source ID/depth collection identity");
    const auto sample = identity["samples"].toArray().at(0).toObject();
    require(error.isEmpty() && sample["sourceDepth"] == (variant == 1 ? 16 : 8) &&
                sample["sampleId"] == source.records.front().provenance.value("sourceSampleId").toString() &&
                (variant != 0 || !sample["sampleId"].toString().isEmpty()),
            "Source sample ID or 16-bit depth disappeared on manifest readback");
  }
  std::atomic_bool cancelled(true);
  QTemporaryDir cancelledStage;
  require(cancelledStage.isValid(), "Cannot create cancelled collection stage");
  for (int i = 0; i < filenames.size(); ++i)
    write(cancelledStage.filePath(filenames[i]), png(imported.records[size_t(i)]));
  require(!Detail::writeAbrTipCollectionManifest(cancelledStage.path(), imported.records, filenames, error, &cancelled) &&
              error.contains("cancelled") &&
              !QFileInfo::exists(cancelledStage.filePath(QString::fromLatin1(Detail::abrTipCollectionManifestName))),
          "Cancelled collection wrote persistent identity");
}
