#include "cadmiumsession.h"

#include "tsimplecolorstyles.h"
#include "tpixelcm.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace Cadmium {
namespace {
constexpr int version = 1;
constexpr int maximumSessionCandidates = 131072;
constexpr int maximumSuffixLength = 64;

bool fail(QString &error, const char *message) {
  error = QObject::tr(message);
  return false;
}

QString normalizedId(const QString &value) {
  const QUuid id(value);
  return id.isNull() ? QString() : id.toString(QUuid::WithoutBraces);
}

bool frameIdValid(const TFrameId &fid) {
  if (fid.isNoFrame() || fid.isEmptyFrame() ||
      fid.getLetter().size() > maximumSuffixLength) return false;
  for (QChar c : fid.getLetter())
    if (c.isNull() || c.category() == QChar::Other_Control) return false;
  return true;
}

bool integer(const QJsonValue &value, int minimum, int maximum, int &result) {
  if (!value.isDouble()) return false;
  const double number = value.toDouble();
  if (!std::isfinite(number) || number < minimum || number > maximum ||
      std::floor(number) != number) return false;
  result = int(number);
  return true;
}

bool boolean(const QJsonObject &object, const char *key, bool &value) {
  const QJsonValue entry = object.value(QLatin1String(key));
  if (!entry.isBool()) return false;
  value = entry.toBool();
  return true;
}

bool digest(const QJsonValue &value, QByteArray &result, bool emptyAllowed) {
  if (!value.isString()) return false;
  const QByteArray text = value.toString().toLatin1();
  if (text.isEmpty() && emptyAllowed) { result.clear(); return true; }
  if (text.size() != 64) return false;
  for (char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  result = QByteArray::fromHex(text);
  return result.size() == 32;
}

QJsonObject encodeFid(const TFrameId &fid) {
  return {{"number", fid.getNumber()}, {"suffix", fid.getLetter()}};
}

bool decodeFid(const QJsonValue &value, TFrameId &fid) {
  if (!value.isObject()) return false;
  const QJsonObject object = value.toObject();
  int number;
  if (!integer(object.value("number"), std::numeric_limits<int>::min(),
               std::numeric_limits<int>::max(), number) ||
      !object.value("suffix").isString()) return false;
  fid = TFrameId(number, object.value("suffix").toString());
  return frameIdValid(fid);
}

bool optionsValid(const SessionOptions &options) {
  return int(options.source) >= int(RasterSource::TransparentLines) &&
      int(options.source) <= int(RasterSource::LinkedLines) &&
      std::isfinite(options.paperThreshold) &&
      std::isfinite(options.lineThreshold) &&
      options.paperThreshold >= 0.0 && options.paperThreshold <= 1.0 &&
      options.lineThreshold >= 0.0 && options.lineThreshold <= 1.0 &&
      (options.lineSourceKey.isEmpty() || options.lineSourceKey.size() == 32) &&
      (options.lineLevelId.isEmpty() || !normalizedId(options.lineLevelId).isEmpty());
}

bool hasModelContributions(const QVector<RegionProposal> &regions) {
  for (const RegionProposal &region : regions) {
    if (!region.references.isEmpty()) return true;
    for (const ProposalCandidate &candidate : region.candidates)
      if (candidate.referenceIndex >= 0 || !candidate.supportingReferences.isEmpty()) return true;
  }
  return false;
}

bool completeAgreement(const RegionProposal &region, int references) {
  if (region.protectedRegion || region.locked || region.selected < 0) return true;
  if (region.selected >= region.candidates.size()) return false;
  const ProposalCandidate &selected = region.candidates[region.selected];
  if (selected.referenceIndex == -1 && selected.referenceRegion == -1) return true;
  const QVector<int> &supports = selected.supportingReferences;
  if (supports.size() != references || region.references.size() != references) return false;
  for (int index = 0; index < references; ++index)
    if (!supports.contains(index) || !region.references.contains(index)) return false;
  return references > 0;
}

QJsonObject encodeOptions(const SessionOptions &options) {
  return {{"source", int(options.source)},
          {"paperThreshold", options.paperThreshold},
          {"lineThreshold", options.lineThreshold},
          {"extractionApproved", options.extractionApproved},
          {"repaint", options.repaint},
          {"lineSourceKey", QString::fromLatin1(options.lineSourceKey.toHex())},
          {"lineLevelId", options.lineLevelId.isEmpty()
                              ? QString() : normalizedId(options.lineLevelId)}};
}

bool decodeOptions(const QJsonValue &value, SessionOptions &options) {
  if (!value.isObject()) return false;
  const QJsonObject object = value.toObject();
  int source;
  if (!integer(object.value("source"), int(RasterSource::TransparentLines),
               int(RasterSource::LinkedLines), source) ||
      !object.value("paperThreshold").isDouble() ||
      !object.value("lineThreshold").isDouble() ||
      !object.value("lineLevelId").isString() ||
      !digest(object.value("lineSourceKey"), options.lineSourceKey, true) ||
      !boolean(object, "extractionApproved", options.extractionApproved) ||
      !boolean(object, "repaint", options.repaint)) return false;
  options.source = RasterSource(source);
  options.paperThreshold = object.value("paperThreshold").toDouble();
  options.lineThreshold = object.value("lineThreshold").toDouble();
  options.lineLevelId = object.value("lineLevelId").toString();
  if (!options.lineLevelId.isEmpty()) options.lineLevelId = normalizedId(options.lineLevelId);
  return optionsValid(options) &&
      (object.value("lineLevelId").toString().isEmpty() || !options.lineLevelId.isEmpty());
}

QJsonArray encodeIndices(const QVector<int> &indices) {
  QJsonArray result;
  for (int index : indices) result.append(index);
  return result;
}

bool decodeIndices(const QJsonValue &value, QVector<int> &indices) {
  if (!value.isArray()) return false;
  const QJsonArray array = value.toArray();
  if (array.size() > MaximumProposalReferences) return false;
  indices.clear();
  for (const QJsonValue &entry : array) {
    int index;
    if (!integer(entry, 0, MaximumProposalReferences - 1, index) ||
        indices.contains(index)) return false;
    indices.append(index);
  }
  return true;
}

QJsonObject encodeNative(const NativeColor &color) {
  return {{"valid", color.valid}, {"linear", color.linear},
          {"r", color.r}, {"g", color.g}, {"b", color.b}, {"a", color.a}};
}

bool decodeNative(const QJsonValue &value, NativeColor &color) {
  if (!value.isObject()) return false;
  const QJsonObject object = value.toObject();
  if (!boolean(object, "valid", color.valid) ||
      !boolean(object, "linear", color.linear)) return false;
  double *channels[] = {&color.r, &color.g, &color.b, &color.a};
  const char *names[] = {"r", "g", "b", "a"};
  for (int channel = 0; channel < 4; ++channel) {
    const QJsonValue entry = object.value(QLatin1String(names[channel]));
    if (!entry.isDouble() || !std::isfinite(entry.toDouble())) return false;
    *channels[channel] = entry.toDouble();
  }
  return color.a >= 0.0 && color.a <= 1.0;
}

QJsonObject encodeCandidate(const ProposalCandidate &candidate) {
  return {{"rgba", QJsonArray{candidate.color.red(), candidate.color.green(),
                              candidate.color.blue(), candidate.color.alpha()}},
          {"style", candidate.style}, {"native", encodeNative(candidate.nativeColor)},
          {"reference", candidate.referenceIndex}, {"region", candidate.referenceRegion},
          {"supports", encodeIndices(candidate.supportingReferences)}};
}

bool decodeCandidate(const QJsonValue &value, ProposalCandidate &candidate) {
  if (!value.isObject()) return false;
  const QJsonObject object = value.toObject();
  if (!object.value("rgba").isArray()) return false;
  const QJsonArray array = object.value("rgba").toArray();
  if (array.size() != 4) return false;
  int channels[4];
  for (int channel = 0; channel < 4; ++channel)
    if (!integer(array[channel], 0, 255, channels[channel])) return false;
  candidate.color = QColor(channels[0], channels[1], channels[2], channels[3]);
  return integer(object.value("style"), 0, std::numeric_limits<int>::max(), candidate.style) &&
      integer(object.value("reference"), -1, MaximumProposalReferences - 1, candidate.referenceIndex) &&
      integer(object.value("region"), -1, MaximumProposalRegions - 1, candidate.referenceRegion) &&
      decodeNative(object.value("native"), candidate.nativeColor) &&
      decodeIndices(object.value("supports"), candidate.supportingReferences);
}

QJsonObject encodeRegion(const RegionProposal &region) {
  QJsonArray candidates;
  for (const ProposalCandidate &candidate : region.candidates)
    candidates.append(encodeCandidate(candidate));
  return {{"selected", region.selected}, {"locked", region.locked},
          {"protected", region.protectedRegion}, {"references", encodeIndices(region.references)},
          {"candidates", candidates}};
}

bool decodeRegion(const QJsonValue &value, RegionProposal &region, int &candidateCount) {
  if (!value.isObject()) return false;
  const QJsonObject object = value.toObject();
  if (!integer(object.value("selected"), -2, MaximumRegionCandidates - 1, region.selected) ||
      !boolean(object, "locked", region.locked) ||
      !boolean(object, "protected", region.protectedRegion) ||
      !decodeIndices(object.value("references"), region.references) ||
      !object.value("candidates").isArray()) return false;
  const QJsonArray array = object.value("candidates").toArray();
  if (array.size() > MaximumRegionCandidates ||
      candidateCount > maximumSessionCandidates - array.size()) return false;
  candidateCount += array.size();
  region.candidates.clear();
  for (const QJsonValue &entry : array) {
    ProposalCandidate candidate;
    if (!decodeCandidate(entry, candidate)) return false;
    region.candidates.append(candidate);
  }
  return true;
}

QJsonObject encodeDrawing(const SessionDrawing &drawing) {
  QJsonArray references, regions;
  for (const SessionReference &reference : drawing.references)
    references.append(QJsonObject{{"fid", encodeFid(reference.fid)},
        {"fingerprint", QString::fromLatin1(reference.fingerprint.toHex())},
        {"regions", reference.regionCount}});
  for (const RegionProposal &region : drawing.regions) regions.append(encodeRegion(region));
  return {{"fid", encodeFid(drawing.fid)},
          {"fingerprint", QString::fromLatin1(drawing.targetFingerprint.toHex())},
          {"labels", QString::fromLatin1(drawing.labelsFingerprint.toHex())},
          {"width", drawing.canvas.width()}, {"height", drawing.canvas.height()},
          {"references", references}, {"regions", regions},
          {"ready", drawing.ready}, {"reviewed", drawing.reviewed}, {"applied", drawing.applied}};
}

bool decodeDrawing(const QJsonValue &value, SessionDrawing &drawing, int &candidateCount) {
  if (!value.isObject()) return false;
  const QJsonObject object = value.toObject();
  int width, height;
  if (!decodeFid(object.value("fid"), drawing.fid) ||
      !digest(object.value("fingerprint"), drawing.targetFingerprint, true) ||
      !digest(object.value("labels"), drawing.labelsFingerprint, true) ||
      !integer(object.value("width"), 0, std::numeric_limits<int>::max(), width) ||
      !integer(object.value("height"), 0, std::numeric_limits<int>::max(), height) ||
      !boolean(object, "ready", drawing.ready) ||
      !boolean(object, "reviewed", drawing.reviewed) ||
      !boolean(object, "applied", drawing.applied) ||
      !object.value("references").isArray() || !object.value("regions").isArray()) return false;
  drawing.canvas = QSize(width, height);
  const QJsonArray references = object.value("references").toArray();
  const QJsonArray regions = object.value("regions").toArray();
  if (references.size() > MaximumProposalReferences || regions.size() > MaximumProposalRegions)
    return false;
  for (const QJsonValue &entry : references) {
    if (!entry.isObject()) return false;
    const QJsonObject ref = entry.toObject();
    SessionReference reference;
    if (!decodeFid(ref.value("fid"), reference.fid) ||
        !digest(ref.value("fingerprint"), reference.fingerprint, true) ||
        !integer(ref.value("regions"), 0, MaximumProposalRegions, reference.regionCount)) return false;
    drawing.references.append(reference);
  }
  for (const QJsonValue &entry : regions) {
    RegionProposal region;
    if (!decodeRegion(entry, region, candidateCount)) return false;
    drawing.regions.append(region);
  }
  return true;
}

QString canonicalScene(const QString &scenePath) {
  if (scenePath.isEmpty() || !QDir::isAbsolutePath(scenePath)) return QString();
  QFileInfo scene(scenePath);
  QString path = scene.canonicalFilePath();
  if (path.isEmpty()) {
    QString parent = scene.dir().canonicalPath();
    if (parent.isEmpty()) parent = QDir::cleanPath(scene.absolutePath());
    path = QDir(parent).filePath(scene.fileName());
  }
#ifdef Q_OS_WIN
  path = path.toCaseFolded();
#endif
  return path;
}

QString identity(const QString &scenePath, const QString &levelId) {
  const QString path = canonicalScene(scenePath);
  const QString id = normalizedId(levelId);
  if (path.isEmpty() || id.isEmpty()) return QString();
  QByteArray bytes = path.toUtf8();
  bytes.append('\0'); bytes.append(id.toLatin1());
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString aliasIdentity(const QString &scenePath, const QByteArray &sourceKey) {
  const QString path = canonicalScene(scenePath);
  if (path.isEmpty() || sourceKey.size() != 32) return QString();
  QByteArray bytes = path.toUtf8();
  bytes.append('\0'); bytes.append("source"); bytes.append('\0'); bytes.append(sourceKey);
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray aliasChecksum(const QString &alias, const QString &levelId) {
  QByteArray bytes = QByteArray::number(version);
  bytes.append('\0'); bytes.append(alias.toLatin1());
  bytes.append('\0'); bytes.append(levelId.toLatin1());
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

bool readAlias(const QString &path, const QString &expectedIdentity,
               QString &levelId, QString &error) {
  levelId.clear();
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return fail(error, "No readable saved painting session is available for this source.");
  if (file.size() < 2 || file.size() > 4096)
    return fail(error, "The painting session discovery record has an unsupported size.");
  const QByteArray bytes = file.read(4097);
  if (bytes.size() != file.size() || file.error() != QFileDevice::NoError)
    return fail(error, "The painting session discovery record could not be read completely.");
  QJsonParseError parse;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject())
    return fail(error, "The painting session discovery record is malformed.");
  const QJsonObject object = document.object();
  int storedVersion;
  QByteArray checksum;
  const QString id = object.value("level").toString();
  if (!integer(object.value("version"), version, version, storedVersion) ||
      object.value("identity").toString() != expectedIdentity ||
      normalizedId(id).isEmpty() || normalizedId(id) != id ||
      !digest(object.value("checksum"), checksum, false) ||
      checksum != aliasChecksum(expectedIdentity, id))
    return fail(error, "The painting session discovery record failed its identity or integrity check.");
  levelId = id;
  return true;
}

bool writeAlias(const QString &path, const QString &alias, const QString &levelId,
                QString &error) {
  const QJsonObject object{{"version", version}, {"identity", alias}, {"level", levelId},
      {"checksum", QString::fromLatin1(aliasChecksum(alias, levelId).toHex())}};
  const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly))
    return fail(error, "The painting session discovery record could not be saved.");
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  if (file.write(bytes) != bytes.size()) {
    file.cancelWriting();
    return fail(error, "The painting session discovery record could not be written completely.");
  }
  if (!file.commit())
    return fail(error, "The painting session was saved, but its recovery index could not be committed.");
  return true;
}

bool storagePath(const QString &path, bool create, QString &error) {
  if (path.isEmpty()) return fail(error, "A saved scene and stable level identity are required for a painting session.");
  const QFileInfo file(path), directory(file.absolutePath());
  if (directory.isSymLink() || file.isSymLink() || QFileInfo(path + ".lock").isSymLink())
    return fail(error, "The painting session cache contains an unsupported link.");
  if (create && !QDir().mkpath(directory.absoluteFilePath()))
    return fail(error, "The painting session cache could not be created.");
  if (create)
    QFile::setPermissions(directory.absoluteFilePath(), QFileDevice::ReadOwner |
                          QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  return true;
}

QByteArray candidateKey(const ProposalCandidate &candidate, Kind kind) {
  QByteArray result;
  const quint32 rgba = qToBigEndian(quint32(candidate.color.rgba()));
  result.append(reinterpret_cast<const char *>(&rgba), sizeof(rgba));
  if (kind != Kind::FullColor) {
    const quint32 style = qToBigEndian(quint32(candidate.style));
    result.append(reinterpret_cast<const char *>(&style), sizeof(style));
  } else {
    for (double value : {candidate.nativeColor.r, candidate.nativeColor.g,
                         candidate.nativeColor.b, candidate.nativeColor.a}) {
      if (value == 0.0) value = 0.0;
      quint64 bits;
      std::memcpy(&bits, &value, sizeof(bits));
      bits = qToBigEndian(bits);
      result.append(reinterpret_cast<const char *>(&bits), sizeof(bits));
    }
    result.append(candidate.nativeColor.linear ? '\1' : '\0');
  }
  return result;
}

bool candidateFromRegion(const Frame &reference, int region, Kind kind,
                         ProposalCandidate &candidate) {
  if (region < 0 || region >= reference.regionColors.size() ||
      !reference.regionColors[region].isValid()) return false;
  candidate.color = reference.regionColors[region];
  if (kind == Kind::FullColor) {
    if (region >= reference.nativeRegionColors.size() ||
        !reference.nativeRegionColors[region].valid) return false;
    candidate.style = 0;
    candidate.nativeColor = reference.nativeRegionColors[region];
  } else {
    if (region >= reference.regionStyles.size() || reference.regionStyles[region] < 0)
      return false;
    candidate.style = reference.regionStyles[region];
  }
  return true;
}
}  // namespace

QByteArray sessionOptionsDigest(const SessionOptions &options) {
  if (!optionsValid(options)) return QByteArray();
  return QCryptographicHash::hash(QJsonDocument(encodeOptions(options)).toJson(QJsonDocument::Compact),
                                  QCryptographicHash::Sha256);
}

QByteArray sessionLabelsDigest(const Frame &frame) {
  if (frame.lines.isNull() || frame.regionColors.isEmpty() ||
      frame.regionColors.size() > MaximumProposalRegions ||
      qint64(frame.lines.width()) * frame.lines.height() != frame.labels.size()) return QByteArray();
  QCryptographicHash hash(QCryptographicHash::Sha256);
  const quint32 header[] = {qToBigEndian(quint32(frame.kind)),
      qToBigEndian(quint32(frame.lines.width())), qToBigEndian(quint32(frame.lines.height())),
      qToBigEndian(quint32(frame.regionColors.size()))};
  hash.addData(reinterpret_cast<const char *>(header), sizeof(header));
  quint32 block[1024];
  int used = 0;
  for (int label : frame.labels) {
    if (label < 0 || label >= frame.regionColors.size()) return QByteArray();
    block[used++] = qToBigEndian(quint32(label));
    if (used == 1024) {
      hash.addData(reinterpret_cast<const char *>(block), sizeof(block));
      used = 0;
    }
  }
  if (used) hash.addData(reinterpret_cast<const char *>(block), used * sizeof(quint32));
  return hash.result();
}

bool validateSession(const Session &session, QString &error) {
  error.clear();
  if (normalizedId(session.levelId).isEmpty() ||
      int(session.kind) < int(Kind::ToonzRaster) || int(session.kind) > int(Kind::FullColor) ||
      !optionsValid(session.options) || session.drawings.size() > MaximumSessionDrawings ||
      session.selectedDrawing < -1 || session.selectedDrawing >= session.drawings.size())
    return fail(error, "The painting session settings or drawing selection are invalid.");
  std::set<TFrameId> fids;
  qint64 candidateCount = 0;
  for (const SessionDrawing &drawing : session.drawings) {
    if (!frameIdValid(drawing.fid) || !fids.insert(drawing.fid).second ||
        drawing.references.size() > MaximumProposalReferences)
      return fail(error, "The painting session contains invalid or duplicate drawings.");
    const bool snapshot = !drawing.targetFingerprint.isEmpty() ||
        !drawing.labelsFingerprint.isEmpty() || !drawing.regions.isEmpty() ||
        drawing.canvas != QSize(0, 0);
    if (snapshot) {
      if (drawing.targetFingerprint.size() != 32 || drawing.labelsFingerprint.size() != 32 ||
          drawing.canvas.width() <= 0 || drawing.canvas.height() <= 0 ||
          qint64(drawing.canvas.width()) * drawing.canvas.height() > std::numeric_limits<int>::max() ||
          drawing.regions.isEmpty() || drawing.regions.size() > MaximumProposalRegions)
        return fail(error, "The painting session proposal snapshot is incomplete.");
    } else if (drawing.ready || drawing.reviewed)
      return fail(error, "The painting session has reviewed results without a source snapshot.");
    if (drawing.ready && drawing.references.isEmpty())
      return fail(error, "The painting session is missing its painted references.");
    const bool completeReferences = drawing.ready || hasModelContributions(drawing.regions);
    std::set<TFrameId> refs;
    for (const SessionReference &reference : drawing.references) {
      if (!frameIdValid(reference.fid) || reference.fid == drawing.fid ||
          !refs.insert(reference.fid).second || reference.regionCount < 0 ||
          reference.regionCount > MaximumProposalRegions ||
          (reference.fingerprint.isEmpty()
               ? reference.regionCount != 0 || completeReferences
               : reference.fingerprint.size() != 32 || reference.regionCount < 1))
        return fail(error, "The painting session reference metadata is invalid.");
    }
    if (!validateProposals(drawing.regions, session.kind, error)) return false;
    for (const RegionProposal &region : drawing.regions) {
      if (drawing.ready && !completeAgreement(region, drawing.references.size()))
        return fail(error, "The saved automatic fill does not agree across every assigned reference.");
      candidateCount += region.candidates.size();
      if (candidateCount > maximumSessionCandidates)
        return fail(error, "The painting session exceeds its candidate storage limit.");
      for (int reference : region.references)
        if (reference < 0 || reference >= drawing.references.size() ||
            drawing.references[reference].fingerprint.size() != 32)
          return fail(error, "The painting session refers to an unavailable reference.");
      for (const ProposalCandidate &candidate : region.candidates) {
        if (candidate.referenceIndex >= drawing.references.size() ||
            (candidate.referenceIndex >= 0 &&
             candidate.referenceRegion >= drawing.references[candidate.referenceIndex].regionCount))
          return fail(error, "The painting session candidate has invalid reference provenance.");
        for (int reference : candidate.supportingReferences)
          if (reference < 0 || reference >= drawing.references.size() ||
              drawing.references[reference].fingerprint.size() != 32)
            return fail(error, "The painting session candidate has invalid supporting references.");
        const NativeColor &native = candidate.nativeColor;
        if (!std::isfinite(native.r) || !std::isfinite(native.g) ||
            !std::isfinite(native.b) || !std::isfinite(native.a) ||
            native.a < 0.0 || native.a > 1.0)
          return fail(error, "The painting session contains unsupported native color values.");
      }
    }
  }
  return true;
}

bool validateSessionAgainstFrames(const SessionDrawing &drawing, const Frame &target,
                                  const QVector<Frame> &references, Kind kind,
                                  QString &error) {
  error.clear();
  if (!frameIdValid(drawing.fid) || target.kind != kind || drawing.targetFingerprint.size() != 32 ||
      target.fingerprint != drawing.targetFingerprint ||
      drawing.labelsFingerprint.size() != 32 ||
      sessionLabelsDigest(target) != drawing.labelsFingerprint ||
      target.lines.size() != drawing.canvas || drawing.regions.size() != target.regionColors.size() ||
      references.size() != drawing.references.size() ||
      references.size() > MaximumProposalReferences ||
      !validateProposals(drawing.regions, kind, error))
    return fail(error, "The saved painting proposal no longer matches its drawing.");
  const bool completeReferences = drawing.ready || hasModelContributions(drawing.regions);
  std::set<TFrameId> referenceIds;
  for (int index = 0; index < references.size(); ++index) {
    const Frame &reference = references[index];
    const SessionReference &saved = drawing.references[index];
    if (!frameIdValid(saved.fid) || saved.fid == drawing.fid ||
        !referenceIds.insert(saved.fid).second || saved.regionCount < 0 ||
        saved.regionCount > MaximumProposalRegions ||
        (saved.fingerprint.isEmpty() && saved.regionCount != 0) ||
        reference.kind != kind || reference.fingerprint.size() != 32 ||
        (!saved.fingerprint.isEmpty() && saved.fingerprint.size() != 32) ||
        (completeReferences && saved.fingerprint.isEmpty()) ||
        (!saved.fingerprint.isEmpty() && (reference.fingerprint != saved.fingerprint ||
          reference.regionColors.size() != saved.regionCount)))
      return fail(error, "A saved painted reference has changed. Generate a new proposal.");
  }
  // Index only candidate colors already present in the bounded session. This
  // avoids a large duplicate native-palette cache while checking all support.
  QHash<QByteArray, quint32> supported;
  int candidateCount = 0;
  for (const RegionProposal &region : drawing.regions)
    for (const ProposalCandidate &candidate : region.candidates) {
      if (++candidateCount > maximumSessionCandidates)
        return fail(error, "The painting session exceeds its candidate storage limit.");
      supported.insert(candidateKey(candidate, kind), 0);
    }
  for (int index = 0; index < references.size(); ++index) {
    const Frame &reference = references[index];
    for (int region = 0; region < reference.regionColors.size(); ++region) {
      ProposalCandidate native;
      if (!candidateFromRegion(reference, region, kind, native)) continue;
      auto found = supported.find(candidateKey(native, kind));
      if (found != supported.end()) found.value() |= quint32(1) << index;
    }
  }
  for (const RegionProposal &region : drawing.regions)
    if (drawing.ready && !completeAgreement(region, references.size()))
      return fail(error, "The saved automatic fill does not agree across every assigned reference.");
  for (const RegionProposal &region : drawing.regions)
    for (const ProposalCandidate &candidate : region.candidates) {
      if (kind != Kind::FullColor) {
        if (!target.palette || target.palette->isAnimated() || candidate.style < 0 ||
            (kind == Kind::ToonzRaster && candidate.style > TPixelCM32::getMaxPaint()) ||
            candidate.style >= target.palette->getStyleCount() ||
            !dynamic_cast<const TSolidColorStyle *>(target.palette->getStyle(candidate.style)))
          return fail(error, "A saved native palette choice is no longer available.");
        const TPixel32 color = target.palette->getStyle(candidate.style)->getMainColor();
        if (candidate.color != QColor(color.r, color.g, color.b, color.m))
          return fail(error, "A saved palette color has changed. Review the drawing again.");
      }
      if (candidate.referenceIndex >= 0) {
        ProposalCandidate actual;
        if (candidate.referenceIndex >= references.size() ||
            drawing.references[candidate.referenceIndex].fingerprint.size() != 32 ||
            !candidateFromRegion(references[candidate.referenceIndex],
                                 candidate.referenceRegion, kind, actual) ||
            candidateKey(actual, kind) != candidateKey(candidate, kind))
          return fail(error, "A saved fill no longer matches its painted reference.");
      }
      const quint32 matching = supported.value(candidateKey(candidate, kind));
      for (int reference : candidate.supportingReferences)
        if (reference < 0 || reference >= references.size() ||
            drawing.references[reference].fingerprint.size() != 32 ||
            !(matching & (quint32(1) << reference)))
          return fail(error, "The saved fill agreement no longer matches its references.");
    }
  return true;
}

QString sessionFilePath(const QString &cacheRoot, const QString &scenePath,
                        const QString &levelId) {
  const QString key = identity(scenePath, levelId);
  if (cacheRoot.isEmpty() || !QDir::isAbsolutePath(cacheRoot) || key.isEmpty()) return QString();
  return QDir(QDir(cacheRoot).filePath("cadmium-sessions")).filePath(key + ".json");
}

bool saveSession(const QString &cacheRoot, const QString &scenePath,
                 const Session &session, QString &error) {
  if (!validateSession(session, error)) return false;
  const QString path = sessionFilePath(cacheRoot, scenePath, session.levelId);
  if (!storagePath(path, true, error)) return false;
  QJsonArray drawings;
  for (const SessionDrawing &drawing : session.drawings) drawings.append(encodeDrawing(drawing));
  const QJsonObject payload{{"level", normalizedId(session.levelId)}, {"kind", int(session.kind)},
      {"options", encodeOptions(session.options)},
      {"optionsDigest", QString::fromLatin1(sessionOptionsDigest(session.options).toHex())},
      {"selected", session.selectedDrawing}, {"drawings", drawings}};
  const QByteArray bytes = QJsonDocument(payload).toJson(QJsonDocument::Compact);
  const QJsonObject envelope{{"version", version}, {"identity", identity(scenePath, session.levelId)},
      {"checksum", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
      {"payload", payload}};
  const QByteArray document = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
  if (document.size() > MaximumSessionBytes)
    return fail(error, "The painting session exceeds its storage limit. Save a smaller selection.");
  QLockFile lock(path + ".lock");
  lock.setStaleLockTime(0);
  if (!lock.tryLock(0)) return fail(error, "Another operation is saving this painting session.");
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) return fail(error, "The painting session could not be saved.");
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  if (file.write(document) != document.size()) {
    file.cancelWriting();
    return fail(error, "The painting session could not be written completely.");
  }
  if (!file.commit()) return fail(error, "The painting session could not be committed. Its previous save was preserved.");
  error.clear();
  return true;
}

bool loadSession(const QString &cacheRoot, const QString &scenePath,
                 const QString &levelId, Session &session, QString &error) {
  session = Session();
  error.clear();
  const QString path = sessionFilePath(cacheRoot, scenePath, levelId);
  if (!storagePath(path, false, error)) return false;
  if (!QFileInfo::exists(path))
    return fail(error, "No readable saved painting session is available.");
  QLockFile lock(path + ".lock");
  lock.setStaleLockTime(0);
  if (!lock.tryLock(0)) return fail(error, "The painting session is currently being saved.");
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return fail(error, "No readable saved painting session is available.");
  if (file.size() < 2 || file.size() > MaximumSessionBytes)
    return fail(error, "The saved painting session has an unsupported size.");
  const QByteArray bytes = file.read(MaximumSessionBytes + 1);
  if (file.error() != QFileDevice::NoError || bytes.size() != file.size() || bytes.size() > MaximumSessionBytes)
    return fail(error, "The saved painting session could not be read completely.");
  QJsonParseError parse;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject())
    return fail(error, "The saved painting session is not valid session data.");
  const QJsonObject envelope = document.object();
  int storedVersion;
  QByteArray checksum;
  if (!integer(envelope.value("version"), version, version, storedVersion) ||
      envelope.value("identity").toString() != identity(scenePath, levelId) ||
      !digest(envelope.value("checksum"), checksum, false) || !envelope.value("payload").isObject())
    return fail(error, "The saved painting session version or identity does not match this level.");
  const QJsonObject payload = envelope.value("payload").toObject();
  if (QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact),
                             QCryptographicHash::Sha256) != checksum)
    return fail(error, "The saved painting session failed its integrity check.");
  Session loaded;
  int kind;
  QByteArray optionsHash;
  loaded.levelId = payload.value("level").toString();
  if (loaded.levelId != normalizedId(levelId) ||
      !integer(payload.value("kind"), int(Kind::ToonzRaster), int(Kind::FullColor), kind) ||
      !integer(payload.value("selected"), -1, MaximumSessionDrawings - 1, loaded.selectedDrawing) ||
      !decodeOptions(payload.value("options"), loaded.options) ||
      !digest(payload.value("optionsDigest"), optionsHash, false) ||
      optionsHash != sessionOptionsDigest(loaded.options) || !payload.value("drawings").isArray())
    return fail(error, "The saved painting session settings are malformed.");
  loaded.kind = Kind(kind);
  const QJsonArray drawings = payload.value("drawings").toArray();
  if (drawings.size() > MaximumSessionDrawings)
    return fail(error, "The saved painting session contains too many drawings.");
  int candidates = 0;
  for (const QJsonValue &value : drawings) {
    SessionDrawing drawing;
    if (!decodeDrawing(value, drawing, candidates))
      return fail(error, "The saved painting session contains malformed proposal data.");
    loaded.drawings.append(drawing);
  }
  if (!validateSession(loaded, error)) return false;
  session = loaded;
  return true;
}

bool removeSession(const QString &cacheRoot, const QString &scenePath,
                   const QString &levelId, QString &error) {
  error.clear();
  const QString path = sessionFilePath(cacheRoot, scenePath, levelId);
  if (!storagePath(path, false, error)) return false;
  if (!QFileInfo::exists(QFileInfo(path).absolutePath())) return true;
  QLockFile lock(path + ".lock");
  lock.setStaleLockTime(0);
  if (!lock.tryLock(0)) return fail(error, "The painting session is currently in use.");
  if (!QFileInfo::exists(path)) return true;
  if (!QFile::remove(path)) return fail(error, "The saved painting session could not be removed.");
  return true;
}

bool copySession(const QString &cacheRoot, const QString &oldScenePath,
                 const QString &newScenePath, const QString &levelId,
                 QString &error) {
  Session session;
  return loadSession(cacheRoot, oldScenePath, levelId, session, error) &&
      saveSession(cacheRoot, newScenePath, session, error);
}

QString sessionAliasPath(const QString &cacheRoot, const QString &scenePath,
                         const QByteArray &sourceKey) {
  const QString alias = aliasIdentity(scenePath, sourceKey);
  if (cacheRoot.isEmpty() || !QDir::isAbsolutePath(cacheRoot) || alias.isEmpty()) return QString();
  return QDir(QDir(cacheRoot).filePath("cadmium-sessions")).filePath("alias-" + alias + ".json");
}

bool saveSessionWithAlias(const QString &cacheRoot, const QString &scenePath,
                          const QByteArray &sourceKey, const Session &session,
                          QString &error) {
  error.clear();
  if (!validateSession(session, error)) return false;
  const QString path = sessionAliasPath(cacheRoot, scenePath, sourceKey);
  if (!storagePath(path, true, error)) return false;
  QLockFile lock(path + ".lock");
  lock.setStaleLockTime(0);
  if (!lock.tryLock(0)) return fail(error, "Another operation is saving this source's painting session.");
  // Publish the discovery pointer only after its complete primary record exists.
  return saveSession(cacheRoot, scenePath, session, error) &&
      writeAlias(path, aliasIdentity(scenePath, sourceKey), normalizedId(session.levelId), error);
}

bool loadSessionWithAlias(const QString &cacheRoot, const QString &scenePath,
                          const QString &currentLevelId, const QByteArray &sourceKey,
                          Session &session, QString &error) {
  session = Session();
  error.clear();
  const QString primary = sessionFilePath(cacheRoot, scenePath, currentLevelId);
  const QString path = sessionAliasPath(cacheRoot, scenePath, sourceKey);
  if (!storagePath(primary, false, error)) return false;
  if (QFileInfo::exists(primary) || QFileInfo(primary).isSymLink())
    return loadSession(cacheRoot, scenePath, currentLevelId, session, error);
  if (!storagePath(path, false, error)) return false;
  if (!QFileInfo::exists(QFileInfo(path).absolutePath()))
    return fail(error, "No readable saved painting session is available for this source.");
  QLockFile lock(path + ".lock");
  lock.setStaleLockTime(0);
  if (!lock.tryLock(0)) return fail(error, "The painting session recovery index is currently in use.");
  // A writer may have completed while the index lock was being acquired.
  if (QFileInfo::exists(primary))
    return loadSession(cacheRoot, scenePath, currentLevelId, session, error);
  {
    QLockFile primaryLock(primary + ".lock");
    primaryLock.setStaleLockTime(0);
    if (!primaryLock.tryLock(0))
      return fail(error, "The current painting session is being saved. Try restoring again after it finishes.");
  }
  QString recoveredId;
  if (!readAlias(path, aliasIdentity(scenePath, sourceKey), recoveredId, error)) return false;
  Session recovered;
  if (!loadSession(cacheRoot, scenePath, recoveredId, recovered, error)) return false;
  recovered.levelId = normalizedId(currentLevelId);
  session = recovered;
  return true;
}

bool removeSessionWithAlias(const QString &cacheRoot, const QString &scenePath,
                            const QString &currentLevelId, const QByteArray &sourceKey,
                            QString &error) {
  error.clear();
  const QString primary = sessionFilePath(cacheRoot, scenePath, currentLevelId);
  const QString path = sessionAliasPath(cacheRoot, scenePath, sourceKey);
  if (!storagePath(primary, false, error) || !storagePath(path, false, error)) return false;
  if (!QFileInfo::exists(QFileInfo(path).absolutePath())) return true;
  QLockFile lock(path + ".lock");
  lock.setStaleLockTime(0);
  if (!lock.tryLock(0)) return fail(error, "The painting session recovery index is currently in use.");
  QString recoveredId, ignored;
  if (QFileInfo::exists(path)) readAlias(path, aliasIdentity(scenePath, sourceKey), recoveredId, ignored);
  if (!removeSession(cacheRoot, scenePath, currentLevelId, error)) return false;
  if (!recoveredId.isEmpty() && recoveredId != normalizedId(currentLevelId) &&
      !removeSession(cacheRoot, scenePath, recoveredId, error)) return false;
  // A corrupt index can be cleared by its exact scoped path; it never supplies
  // an unchecked identity or causes a search through unrelated cache entries.
  if (QFileInfo::exists(path) && !QFile::remove(path))
    return fail(error, "The painting session recovery index could not be removed.");
  return true;
}

}  // namespace Cadmium
