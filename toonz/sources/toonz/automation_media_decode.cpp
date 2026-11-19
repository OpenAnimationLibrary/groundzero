#include "automation_media_decode.h"
#include "automation.h"

#include "texception.h"
#include "timage_io.h"
#include "tlevel_io.h"
#include "trasterimage.h"
#include "tsound.h"
#include "tsound_io.h"
#include "ttoonzimage.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <cstdlib>

namespace GroundzeroAutomation {
namespace {

constexpr qint64 MaxManifestBytes = 1024 * 1024;

bool soundFormat(const QString& format) {
  return format == "wav" || format == "aif" || format == "aiff";
}

bool levelFormat(const QString& format) {
  return format == "pli" || format == "tlv";
}

// Width and height of a decoded raster or Toonz raster image; a vector image
// has neither.
void dimensions(const TImageP& image, QJsonObject& row) {
  if (TRasterImageP raster = image) {
    if (TRasterP pixels = raster->getRaster()) {
      row.insert("width", pixels->getLx());
      row.insert("height", pixels->getLy());
    }
  } else if (TToonzImageP toonz = image) {
    row.insert("width", toonz->getSize().lx);
    row.insert("height", toonz->getSize().ly);
  }
}

QJsonObject decoded(int frames, const QString& message) {
  return {{"result", "decoded"},
          {"frames_checked", frames},
          {"width", QJsonValue()},
          {"height", QJsonValue()},
          {"message", message}};
}

QJsonObject corrupt(const QString& message) {
  return {{"result", "corrupt"},
          {"frames_checked", 0},
          {"width", QJsonValue()},
          {"height", QJsonValue()},
          {"message", message.left(512)}};
}

quint32 chunkSize(const QByteArray& bytes, qint64 at, bool bigEndian) {
  const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + at);
  return bigEndian ? qFromBigEndian<quint32>(data)
                   : qFromLittleEndian<quint32>(data);
}

// Whether the RIFF (WAV) or FORM (AIFF) chunks declare more sample bytes than
// the file holds: native readers stop at the end of the file and keep what
// they read, so a truncated stream would otherwise pass.
bool truncatedSound(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return true;
  const QByteArray bytes = file.readAll();
  const bool aiff        = bytes.startsWith("FORM");
  if (!aiff && !bytes.startsWith("RIFF")) return true;
  const QByteArray samples = aiff ? "SSND" : "data";
  for (qint64 at = 12; at + 8 <= bytes.size();) {
    const qint64 size = chunkSize(bytes, at + 4, aiff);
    if (bytes.mid(at, 4) == samples) return at + 8 + size > bytes.size();
    at += 8 + size + (size & 1);
  }
  return true;
}

// A sound: the header, the declared sample stream and every sample.
QJsonObject decodeSound(const TFilePath& path) {
  if (truncatedSound(path.getQString()))
    return corrupt("The sample stream is shorter than its header declares.");
  TSoundTrackP track;
  if (!TSoundTrackReader::load(path, track) || !track)
    return corrupt("The sound decoder read no samples.");
  return decoded(0, QString("%1 samples").arg(track->getSampleCount()));
}

// A level file: its frame list and its first frame.
QJsonObject decodeLevel(const TFilePath& path) {
  TLevelReaderP reader(path);
  if (!reader) return corrupt("No level reader accepts the file.");
  const TLevelP level = reader->loadInfo();
  if (!level || level->getFrameCount() == 0)
    return corrupt("The level lists no frames.");
  const TImageReaderP frame = reader->getFrameReader(level->begin()->first);
  const TImageP image       = frame ? frame->load() : TImageP();
  if (!image) return corrupt("The first frame cannot be decoded.");
  QJsonObject row =
      decoded(1, QString("%1 frames listed").arg(level->getFrameCount()));
  dimensions(image, row);
  return row;
}

// An image file: its header and the full image.
QJsonObject decodeImage(const TFilePath& path) {
  TImageP image;
  if (!TImageReader::load(path, image) || !image)
    return corrupt("The image cannot be decoded.");
  QJsonObject row = decoded(1, QString());
  dimensions(image, row);
  return row;
}

#ifdef BUILD_UI_PORT_TESTS
// Test-only: a file that starts with this marker crashes the helper, standing
// for a native decoder that crashes on corrupt input.
void crashOnMarker(const QString& path) {
  QFile file(path);
  if (file.open(QIODevice::ReadOnly) &&
      file.read(24).startsWith("GROUNDZERO-TEST-CRASH"))
    std::abort();
}
#endif

QJsonObject decode(const QString& path, const QString& format) {
#ifdef BUILD_UI_PORT_TESTS
  crashOnMarker(path);
#endif
  const TFilePath file(path.toStdWString());
  try {
    if (soundFormat(format)) return decodeSound(file);
    if (levelFormat(format)) return decodeLevel(file);
    return decodeImage(file);
  } catch (const TException& error) {
    return corrupt(QString::fromStdWString(error.getMessage()));
  } catch (const std::exception& error) {
    return corrupt(QString::fromUtf8(error.what()));
  } catch (...) {
    return corrupt("Unrecognized native error.");
  }
}

QJsonArray readManifest(Context& context) {
  QFile file(QDir(context.workspace()).filePath("manifest.json"));
  if (!file.open(QIODevice::ReadOnly) || file.size() > MaxManifestBytes)
    return {};
  return QJsonDocument::fromJson(file.readAll()).array();
}

}  // namespace

const QStringList& mediaDecodeFormats() {
  static const QStringList formats{"pli", "tlv", "png",  "tif", "tiff", "tga",
                                   "bmp", "jpg", "jpeg", "wav", "aif",  "aiff"};
  return formats;
}

int runMediaDecodeHelper(Context& context, QFile& output) {
  const QDir directory(context.workspace());
  for (const QJsonValue& value : readManifest(context)) {
    const QJsonObject entry = value.toObject();
    const QString format    = entry.value("format").toString();
    if (!mediaDecodeFormats().contains(format)) return 2;
    QJsonObject row =
        decode(directory.filePath(entry.value("file").toString()), format);
    row.insert("index", entry.value("index"));
    const QByteArray line =
        QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
    if (output.write(line) != line.size() || !output.flush()) return 2;
  }
  return 0;
}

}  // namespace GroundzeroAutomation
