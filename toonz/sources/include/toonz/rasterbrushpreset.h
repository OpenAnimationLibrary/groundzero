#pragma once
#include "qtmsvccompat.h"
#include "toonz/rasterbrushengine.h"
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <atomic>

namespace RasterBrush {
struct Record {
  Preset preset;
  QJsonObject provenance;
  // Declarations are scoped to stable pixel hashes, never inferred from a pack
  // name.
  QJsonObject resourceRights;
  QStringList diagnostics;
  bool decoded               = false;
  bool semanticallySupported = false;
  // Verification is never granted by a field in an imported file.
  bool referenceVerified = false;
};
// Bounded local regular-file read without following links or opening FIFOs.
// Empty success is distinguished from failure by error.isEmpty().
RASTERBRUSH_API QByteArray readBoundedRegularFile(const QString &path,
                                                  qint64 maximumBytes,
                                                  QString &error);
enum class ResourcePackaging { ManifestOnly, PermittedOnly, PrivateCopy };
struct ResourceInfo {
  QString hash, redistribution = "unknown", license;
  int width = 0, height = 0;
  bool color = false, embedded = false;
};
struct PortableInfo {
  bool portable = false;
  QString name;
  std::vector<ResourceInfo> resources;
};
// Metadata inspection never invokes an image decoder. A manifest with missing
// resources cannot become a selectable brush until every hash is resolved.
RASTERBRUSH_API std::vector<ResourceInfo> resources(const Record &,
                                                    QString &error);
RASTERBRUSH_API PortableInfo inspectPortable(const QByteArray &,
                                             QString &error);
// Discard embedded copies after failed worker validation, retaining identities.
RASTERBRUSH_API QByteArray preparePortableRepair(const QByteArray &,
                                                 QString &error);
RASTERBRUSH_API QByteArray encodePortable(const Record &, ResourcePackaging,
                                          QString &error);
RASTERBRUSH_API bool savePortable(const QString &, const Record &,
                                  ResourcePackaging, QString &error);
RASTERBRUSH_API QByteArray relinkPortable(const QByteArray &,
                                          const std::vector<Record> &copies,
                                          QStringList &matchedHashes,
                                          QString &error);
RASTERBRUSH_API QByteArray encode(const Record &record, QString &error);
RASTERBRUSH_API Record decode(const QByteArray &bytes, QString &error,
                              size_t maximumDecodedBytes = 256u * 1024u *
                                                           1024u);
// Private worker wire format: bounded metadata plus fixed-size RGBA8 sidecars.
// Parent decoding does not invoke PNG, archive, XML or foreign brush codecs.
RASTERBRUSH_API QByteArray encodeWorkerRecord(const Record &record,
                                              const QString &directory,
                                              QString &error);
RASTERBRUSH_API Record decodeWorkerRecord(
    const QByteArray &bytes, const QString &directory, QString &error,
    size_t maximumDecodedBytes = 256u * 1024u * 1024u);
RASTERBRUSH_API Record load(const QString &path, QString &error,
                            size_t maximumDecodedBytes = 256u * 1024u * 1024u);
RASTERBRUSH_API bool save(const QString &path, const Record &record,
                          QString &error);
RASTERBRUSH_API QString install(const QString &directory, const Record &record,
                                QString &error);
// Stage a complete collection, then publish one directory atomically. Existing
// collections are verified and reused; cancellation never publishes a subset.
RASTERBRUSH_API QStringList
installCollection(const QString &directory, const std::vector<Record> &records,
                  QString &error, const std::atomic_bool *cancelled = nullptr);
// Converts a bounded bitmap into a native mask or colored stamp. PNG resources
// inside a package have no filenames and cannot escape their resource graph.
RASTERBRUSH_API std::shared_ptr<const Image> decodeImage(
    const QByteArray &bytes, bool color, QString &error,
    size_t maximumDecodedBytes = 256u * 1024u * 1024u);
}  // namespace RasterBrush
