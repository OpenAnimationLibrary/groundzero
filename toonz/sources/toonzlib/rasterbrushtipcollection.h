#pragma once

#include "rasterbrushresponse.h"
#include "toonz/rasterbrushpreset.h"
#include "toonz/rasterbrushlibrary.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>
#include <cmath>

namespace RasterBrush {
namespace Detail {
inline constexpr const char *abrTipCollectionManifestName = "sampled-tips.json";
inline constexpr qint64 abrTipManifestBudget = 8LL * 1024 * 1024;
inline constexpr qint64 abrTipOutputBudget = 64LL * 1024 * 1024;

inline bool abrTipKeys(const QJsonObject &object, const QStringList &keys) {
  if (object.size() != keys.size()) return false;
  for (const auto &key : keys)
    if (!object.contains(key)) return false;
  return true;
}
inline bool abrTipInteger(const QJsonValue &value, double low, double high) {
  const double n = value.toDouble(-1);
  return value.isDouble() && std::isfinite(n) && n >= low && n <= high &&
         n == std::floor(n);
}
inline bool abrTipDigest(const QJsonValue &value) {
  static const QRegularExpression sha256("^[0-9a-f]{64}$");
  return value.isString() && value.toString().size() == 64 &&
         sha256.match(value.toString()).hasMatch();
}
inline bool abrTipFilename(const QString &name) {
  if (name.isEmpty() || name.startsWith('.') || name.toUtf8().size() > 240 ||
      !name.endsWith(".png", Qt::CaseInsensitive) || name.contains('/') ||
      name.contains('\\') || name.contains(':'))
    return false;
  for (QChar c : name)
    if (c.unicode() < 32) return false;
  return true;
}
inline bool abrTipCancelled(const std::atomic_bool *cancelled, QString &error) {
  if (!cancelled || !cancelled->load(std::memory_order_relaxed)) return false;
  error = "Brush import cancelled";
  return true;
}

// This records parsed sampled images only. The digest detects changed outputs;
// it is not a source-authenticity, preset-behavior or redistribution credential.
inline QJsonObject decodeAbrTipCollectionManifest(const QByteArray &bytes,
                                                  QString &error) {
  error.clear();
  const auto invalid = [&]() -> QJsonObject {
    error = "Invalid sampled-tip collection manifest";
    return {};
  };
  if (bytes.isEmpty() || bytes.size() > abrTipManifestBudget) return invalid();
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(bytes, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) return invalid();
  const auto root = document.object();
  if (!abrTipKeys(root, {"format", "version", "extractionKind", "source", "sampleCount", "samples"}) ||
      root["format"] != "groundzero-abr-sampled-tip-collection" ||
      !abrTipInteger(root["version"], 1, 1) ||
      root["extractionKind"] != "sampled-images-only" ||
      !root["source"].isObject() || !abrTipInteger(root["sampleCount"], 1, 256) ||
      !root["samples"].isArray())
    return invalid();
  const auto source = root["source"].toObject();
  if (!abrTipKeys(source, {"format", "sha256", "version", "subversion"}) ||
      source["format"] != "abr" || !abrTipDigest(source["sha256"]) ||
      !abrTipInteger(source["version"], 1, 10) ||
      !abrTipInteger(source["subversion"], 0, 2))
    return invalid();
  const int version = source["version"].toInt(), subversion = source["subversion"].toInt();
  if (!((version == 1 || version == 2) ? subversion == 0
        : version >= 6 && (subversion == 1 || subversion == 2)))
    return invalid();
  const auto samples = root["samples"].toArray();
  if (samples.size() != root["sampleCount"].toInt()) return invalid();
  QSet<QString> filenames;
  qint64 pixels = 0;
  for (int ordinal = 0; ordinal < samples.size(); ++ordinal) {
    if (!samples[ordinal].isObject()) return invalid();
    const auto sample = samples[ordinal].toObject();
    if (!abrTipKeys(sample, {"ordinal", "name", "sampleId", "width", "height", "sourceDepth", "file", "outputSha256"}) ||
        !abrTipInteger(sample["ordinal"], ordinal, ordinal) ||
        !sample["name"].isString() || sample["name"].toString().size() > 4096 ||
        !sample["sampleId"].isString() || sample["sampleId"].toString().size() > 36 ||
        !abrTipInteger(sample["width"], 1, 8192) ||
        !abrTipInteger(sample["height"], 1, 8192) ||
        !(abrTipInteger(sample["sourceDepth"], 8, 8) || abrTipInteger(sample["sourceDepth"], 16, 16)) ||
        !sample["file"].isString() || !abrTipFilename(sample["file"].toString()) ||
        !abrTipDigest(sample["outputSha256"]))
      return invalid();
    for (QChar c : sample["sampleId"].toString())
      if (c.unicode() > 255) return invalid();  // Parser preserves ID bytes as Latin-1.
    const qint64 area = qint64(sample["width"].toInt()) * sample["height"].toInt();
    pixels += area;
    const auto key = legacyTipLibraryCollisionKey(sample["file"].toString());
    if (area > 2 * 1024 * 1024 || pixels > 8 * 1024 * 1024 || filenames.contains(key))
      return invalid();
    filenames.insert(key);
  }
  return root;
}

// Hash regular, single-link files in bounded chunks without invoking a PNG
// decoder in the parent. The emitted PNG's IHDR must match the parsed geometry.
inline QString abrTipOutputDigest(const RegularResponseDirectory &directory,
                                  const QString &name, int width, int height,
                                  qint64 &remaining, QString &error,
                                  const std::atomic_bool *cancelled) {
  if (abrTipCancelled(cancelled, error)) return {};
  RegularResponseFile file;
  if (!file.open(directory, name, remaining, error)) return {};
  remaining -= file.size();
  QCryptographicHash hash(QCryptographicHash::Sha256);
  QByteArray header;
  qint64 position = 0;
  while (position < file.size()) {
    if (abrTipCancelled(cancelled, error)) return {};
    const auto bytes = file.read(std::min(qint64(65536), file.size() - position), error);
    if (!error.isEmpty()) return {};
    if (header.size() < 24) header.append(bytes.left(24 - header.size()));
    hash.addData(bytes);
    position += bytes.size();
  }
  if (!file.finish(error)) return {};
  if (header.size() != 24 || header.left(8) != QByteArray::fromHex("89504e470d0a1a0a") ||
      qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(header.constData() + 8)) != 13 ||
      header.mid(12, 4) != "IHDR" ||
      qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(header.constData() + 16)) != quint32(width) ||
      qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(header.constData() + 20)) != quint32(height)) {
    error = "Sampled-tip output geometry does not match its manifest";
    return {};
  }
  return QString::fromLatin1(hash.result().toHex());
}

// Only a private transaction stage calls this writer; the caller retires the
// entire unpublished stage after any failure or cancellation. Never overwrite.
inline bool writeAbrTipCollectionManifest(const QString &directory,
                                          const std::vector<Record> &records,
                                          const QStringList &filenames,
                                          QString &error,
                                          const std::atomic_bool *cancelled = nullptr) {
  error.clear();
  if (abrTipCancelled(cancelled, error)) return false;
  if (records.empty() || records.size() > 256 || records.size() != size_t(filenames.size())) {
    error = "Incomplete sampled-tip collection identity";
    return false;
  }
  RegularResponseDirectory anchored;
  if (!anchored.open(directory, error)) return false;
  const auto &provenance = records.front().provenance;
  const QJsonObject source{{"format", "abr"}, {"sha256", provenance["sourceSha256"]},
                          {"version", provenance["containerVersion"]},
                          {"subversion", provenance["containerSubversion"]}};
  QJsonArray samples;
  qint64 remaining = abrTipOutputBudget;
  for (size_t ordinal = 0; ordinal < records.size(); ++ordinal) {
    if (abrTipCancelled(cancelled, error)) return false;
    const auto &record = records[ordinal];
    const auto &metadata = record.provenance;
    if (metadata["conversionKind"] != "sampled-tip-extraction" ||
        metadata["sampledTipOrdinal"] != QJsonValue(int(ordinal)) ||
        metadata["sampledTipCount"] != QJsonValue(int(records.size())) ||
        metadata["sourceSha256"] != source["sha256"] ||
        metadata["containerVersion"] != source["version"] ||
        metadata["containerSubversion"] != source["subversion"] ||
        record.preset.tip.shape != Shape::Bitmap || record.preset.tip.images.size() != 1 ||
        !record.preset.tip.images.front() || record.preset.tip.images.front()->color ||
        (metadata.contains("sourceSampleId") && !metadata["sourceSampleId"].isString())) {
      error = "Inconsistent sampled-tip collection identity";
      return false;
    }
    const auto &image = *record.preset.tip.images.front();
    if (image.width < 1 || image.height < 1 ||
        size_t(image.width) * size_t(image.height) != image.pixels.size() ||
        !abrTipFilename(filenames[int(ordinal)])) {
      error = "Invalid sampled-tip collection output";
      return false;
    }
    const auto digest = abrTipOutputDigest(anchored, filenames[int(ordinal)],
                                         image.width, image.height, remaining,
                                         error, cancelled);
    if (!error.isEmpty()) return false;
    samples.append(QJsonObject{{"ordinal", int(ordinal)},
        {"name", QString::fromStdString(record.preset.name)},
        {"sampleId", metadata.contains("sourceSampleId") ? metadata["sourceSampleId"] : QJsonValue("")},
        {"width", image.width}, {"height", image.height},
        {"sourceDepth", metadata["sourceSampleDepth"]},
        {"file", filenames[int(ordinal)]}, {"outputSha256", digest}});
  }
  const QByteArray bytes = QJsonDocument(QJsonObject{
      {"format", "groundzero-abr-sampled-tip-collection"}, {"version", 1},
      {"extractionKind", "sampled-images-only"}, {"source", source},
      {"sampleCount", int(records.size())}, {"samples", samples}}).toJson();
  if (decodeAbrTipCollectionManifest(bytes, error).isEmpty()) return false;
  if (abrTipCancelled(cancelled, error)) return false;
  QFile file(QDir(directory).filePath(QString::fromLatin1(abrTipCollectionManifestName)));
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      file.write(bytes) != bytes.size() || !file.flush()) {
    error = "Cannot write sampled-tip collection manifest";
    return false;
  }
  file.close();
  if (file.error() != QFileDevice::NoError) {
    error = "Cannot close sampled-tip collection manifest";
    return false;
  }
  return !abrTipCancelled(cancelled, error);
}

inline QJsonObject readAbrTipCollectionManifest(const QString &directory,
                                                QString &error,
                                                const std::atomic_bool *cancelled = nullptr) {
  error.clear();
  if (abrTipCancelled(cancelled, error)) return {};
  RegularResponseDirectory anchored;
  if (!anchored.open(directory, error)) return {};
  // Filesystem/Qt boundaries may expose a canonical-equivalent filename.
  // Preserve parsed sample names exactly, but bind member identity through the
  // same portable normalization/case rule used while choosing output names.
  QMap<QString, QString> members;
  QDirIterator entries(directory, QDir::AllEntries | QDir::Hidden | QDir::System |
                                      QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
  while (entries.hasNext()) {
    const auto name = QFileInfo(entries.next()).fileName();
    const auto key = legacyTipLibraryCollisionKey(name);
    if (members.size() >= 257 || members.contains(key)) {
      error = "Sampled-tip collection has excessive or colliding members";
      return {};
    }
    members.insert(key, name);
  }
  const auto manifestKey = legacyTipLibraryCollisionKey(
      QString::fromLatin1(abrTipCollectionManifestName));
  if (!members.contains(manifestKey)) {
    error = "Sampled-tip collection manifest is missing";
    return {};
  }
  const auto bytes = readResponseFile(anchored, members.value(manifestKey), abrTipManifestBudget, error);
  if (!error.isEmpty()) return {};
  const auto root = decodeAbrTipCollectionManifest(bytes, error);
  if (!error.isEmpty()) return {};
  QSet<QString> expected{manifestKey};
  qint64 remaining = abrTipOutputBudget;
  for (const auto &value : root["samples"].toArray()) {
    const auto sample = value.toObject();
    const auto key = legacyTipLibraryCollisionKey(sample["file"].toString());
    if (!members.contains(key)) {
      error = "Sampled-tip collection members do not match its manifest";
      return {};
    }
    expected.insert(key);
    const auto digest = abrTipOutputDigest(anchored, members.value(key), sample["width"].toInt(),
                                         sample["height"].toInt(), remaining, error, cancelled);
    if (!error.isEmpty()) return {};
    if (digest != sample["outputSha256"].toString()) {
      error = "Sampled-tip output digest does not match its manifest";
      return {};
    }
  }
  if (members.size() != expected.size()) {
    error = "Sampled-tip collection members do not match its manifest";
    return {};
  }
  if (abrTipCancelled(cancelled, error)) return {};
  return root;
}
}  // namespace Detail
}  // namespace RasterBrush
