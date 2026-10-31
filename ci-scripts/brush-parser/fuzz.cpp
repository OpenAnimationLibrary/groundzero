#include "../../toonz/sources/toonzlib/rasterbrushresponse.h"
#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  static int argc = 1;
  static char name[] = "brush-import-fuzz";
  static char *argv[] = {name, nullptr};
  static QCoreApplication application(argc, argv);
  return 0;
}
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, size_t size) {
  if (size < 2 || size > 65536)
    return 0;
  static const char *formats[] = {"gzbrush", "kpp",    "gbr",
                                  "gih",     "bundle", "abr"};
  QString error;
  const QByteArray payload(reinterpret_cast<const char *>(data + 1),
                           int(size - 1));
  const unsigned kind = data[0] % 9;
  if (kind >= 6) {
    QTemporaryDir directory;
    if (!directory.isValid())
      std::abort();
    RasterBrush::Record original;
    auto image = std::make_shared<RasterBrush::Image>();
    image->width = image->height = 1;
    image->pixels = {{0, 0, 0, 1}};
    original.preset.tip.shape = RasterBrush::Shape::Bitmap;
    original.preset.tip.images.push_back(image);
    auto metadata =
        RasterBrush::encodeWorkerRecord(original, directory.path(), error);
    if (!error.isEmpty())
      std::abort();
    if (kind == 8) {
      RasterBrush::Detail::RegularResponseDirectory anchored;
      if (!anchored.open(directory.path(), error))
        std::abort();
      auto write = [&](const QString &name, const QByteArray &bytes) {
        QFile file(directory.filePath(name));
        return file.open(QIODevice::WriteOnly) &&
               file.write(bytes) == bytes.size();
      };
      if (!write("0.gzbrush", metadata) || !write("result.json", payload))
        std::abort();
      const auto result = RasterBrush::readWorkerResponse(anchored, true, 0,
                                                          error, 1024 * 1024);
      if (!error.isEmpty() && !result.records.empty())
        std::abort();
      for (const auto &record : result.records)
        if (record.referenceVerified ||
            !RasterBrush::validate(record.preset).empty())
          std::abort();
      return 0;
    }
    if (kind == 6) {
      metadata = payload;
    } else {
      // Hostile raw sidecar bytes enter the exact parent return decoder,
      // including headers that advertise much larger allocations than their
      // body.
      auto root = QJsonDocument::fromJson(metadata).object();
      const QString identity = QString::fromLatin1(
          QCryptographicHash::hash(payload + "mask", QCryptographicHash::Sha256)
              .toHex());
      QFile file(directory.filePath(identity + ".rgba8"));
      if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
          file.write(payload) != payload.size())
        std::abort();
      file.close();
      root["assets"] = QJsonObject{
          {identity, QJsonObject{{"rgba8", identity}, {"color", false}}}};
      auto settings = root["settings"].toObject();
      auto tip = settings["tip"].toObject();
      tip["images"] = QJsonArray{identity};
      settings["tip"] = tip;
      root["settings"] = settings;
      metadata = QJsonDocument(root).toJson();
    }
    const auto record = RasterBrush::decodeWorkerRecord(
        metadata, directory.path(), error, 1024 * 1024);
    if (error.isEmpty() && (record.referenceVerified ||
                            !RasterBrush::validate(record.preset).empty()))
      std::abort();
    return 0;
  }
  auto result = RasterBrush::importBrush(
      payload, QString::fromLatin1(formats[kind]), error);
  if (!error.isEmpty() && !result.records.empty())
    std::abort();
  for (const auto &record : result.records) {
    if (record.referenceVerified ||
        !RasterBrush::validate(record.preset).empty())
      std::abort();
    auto bytes = RasterBrush::encode(record, error);
    if (error.isEmpty()) {
      auto restored = RasterBrush::decode(bytes, error);
      if (!error.isEmpty() || !restored.semanticallySupported ||
          restored.referenceVerified)
        std::abort();
    }
  }
  return 0;
}
