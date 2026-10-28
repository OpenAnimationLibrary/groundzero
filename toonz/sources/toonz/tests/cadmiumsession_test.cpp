#include "cadmiumsession.h"

#include "tpalette.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QTemporaryDir>
#include <QUuid>

#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
QByteArray hash(const char *value) {
  return QCryptographicHash::hash(value, QCryptographicHash::Sha256);
}
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QByteArray read(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "cannot inspect saved session");
  return file.readAll();
}
void write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
          "cannot create session fixture");
}
void edit(const QString &path, const std::function<void(QJsonObject &)> &change,
          bool rehash = true) {
  QJsonObject envelope = QJsonDocument::fromJson(read(path)).object();
  change(envelope);
  if (rehash) {
    const QByteArray payload = QJsonDocument(envelope.value("payload").toObject())
                                   .toJson(QJsonDocument::Compact);
    envelope["checksum"] = QString::fromLatin1(
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
  }
  write(path, QJsonDocument(envelope).toJson(QJsonDocument::Compact));
}

struct Fixture {
  Cadmium::Session session;
  Cadmium::Frame target, reference;
  Fixture() {
    target.kind = reference.kind = Cadmium::Kind::FullColor;
    target.lines = reference.lines = QImage(3, 2, QImage::Format_ARGB32_Premultiplied);
    target.lines.fill(Qt::transparent);
    reference.lines.fill(Qt::transparent);
    target.labels = reference.labels = {0, 1, 2, 0, 1, 2};
    target.regionColors = {QColor(), QColor(), QColor()};
    reference.regionColors = {QColor(), QColor(255, 80, 0, 117), QColor(255, 80, 0, 117)};
    reference.nativeRegionColors = {{},
        {2.345678901234567, 0.12345678901234567, -0.12512345678901234, 0.4567890123456789, true, true},
        {2.345678901234568, 0.12345678901234568, -0.12512345678901233, 0.4567890123456789, true, true}};
    target.fingerprint = hash("target");
    reference.fingerprint = hash("reference");
    session.levelId = uuid();
    session.kind = Cadmium::Kind::FullColor;
    session.options.source = Cadmium::RasterSource::LinkedLines;
    session.options.lineLevelId = uuid();
    session.options.lineSourceKey = hash("clean line resource and cast identity");
    session.options.extractionApproved = true;
    session.options.paperThreshold = 0.9123456789012345;
    session.selectedDrawing = 0;
    Cadmium::SessionDrawing drawing;
    drawing.fid = TFrameId(1, QStringLiteral("A"));
    drawing.targetFingerprint = target.fingerprint;
    drawing.labelsFingerprint = Cadmium::sessionLabelsDigest(target);
    drawing.canvas = target.lines.size();
    drawing.references.append({TFrameId(7, QStringLiteral("B")), reference.fingerprint, 3});
    drawing.regions.resize(3);
    for (Cadmium::RegionProposal &region : drawing.regions) region.references.append(0);
    drawing.regions[0].selected = -2;
    drawing.regions[0].protectedRegion = true;
    drawing.regions[0].locked = true;
    for (int region : {1, 2}) {
      Cadmium::ProposalCandidate candidate;
      candidate.color = reference.regionColors[region];
      candidate.style = 0;
      candidate.nativeColor = reference.nativeRegionColors[region];
      candidate.referenceIndex = 0;
      candidate.referenceRegion = region;
      candidate.supportingReferences.append(0);
      drawing.regions[1].candidates.append(candidate);
    }
    drawing.regions[1].selected = 1;
    drawing.regions[1].locked = true;
    drawing.regions[2].selected = -2;
    drawing.regions[2].protectedRegion = true;
    drawing.regions[2].locked = true;
    drawing.ready = true;
    drawing.reviewed = true;
    session.drawings.append(drawing);
  }
};

void roundTrip(const QString &root, const QString &scene) {
  Fixture fixture;
  QString error;
  require(Cadmium::validateSession(fixture.session, error), "valid session fixture was rejected");
  require(Cadmium::validateSessionAgainstFrames(fixture.session.drawings[0],
              fixture.target, {fixture.reference}, fixture.session.kind, error),
          "unchanged native frames did not validate");
  require(Cadmium::saveSession(root, scene, fixture.session, error), "cannot save valid session");
  const QString path = Cadmium::sessionFilePath(root, scene, fixture.session.levelId);
  const QByteArray original = read(path);
  require(!original.contains(scene.toUtf8()) && !original.contains(root.toUtf8()),
          "session data persisted machine-specific cache or scene paths");
  Cadmium::Session loaded;
  require(Cadmium::loadSession(root, scene, fixture.session.levelId, loaded, error),
          "cannot restore valid session");
  require(loaded.levelId == fixture.session.levelId && loaded.drawings.size() == 1 &&
              loaded.selectedDrawing == 0 && loaded.kind == Cadmium::Kind::FullColor,
          "session identity or selection changed");
  const Cadmium::SessionDrawing &drawing = loaded.drawings[0];
  require(drawing.fid == TFrameId(1, QStringLiteral("A")) &&
              drawing.references[0].fid == TFrameId(7, QStringLiteral("B")) &&
              drawing.ready && drawing.reviewed && !drawing.applied &&
              drawing.regions[1].locked && drawing.regions[1].selected == 1 &&
              drawing.regions[2].protectedRegion && drawing.regions[2].selected == -2,
          "session lost drawing suffixes, review, protection or selected choices");
  require(drawing.regions[1].candidates.size() == 2 &&
              drawing.regions[1].candidates[0].nativeColor == fixture.reference.nativeRegionColors[1] &&
              drawing.regions[1].candidates[1].nativeColor == fixture.reference.nativeRegionColors[2] &&
              drawing.regions[1].candidates[0].nativeColor != drawing.regions[1].candidates[1].nativeColor,
          "JSON roundtrip reduced native HDR or sub-byte precision");
  require(Cadmium::sessionOptionsDigest(loaded.options) ==
              Cadmium::sessionOptionsDigest(fixture.session.options) &&
              loaded.options.lineSourceKey == fixture.session.options.lineSourceKey,
          "session input options changed");
  require(Cadmium::validateSessionAgainstFrames(drawing, fixture.target,
              {fixture.reference}, fixture.session.kind, error),
          "restored native choices failed their unchanged source validation");

  Cadmium::Frame changed = fixture.target;
  changed.labels[0] = 2;
  require(!Cadmium::validateSessionAgainstFrames(drawing, changed,
              {fixture.reference}, fixture.session.kind, error),
          "changed target labels retained a stale saved proposal");
  changed = fixture.reference;
  changed.fingerprint = hash("changed reference");
  require(!Cadmium::validateSessionAgainstFrames(drawing, fixture.target,
              {changed}, fixture.session.kind, error),
          "changed reference pixels retained a stale saved proposal");
  changed = fixture.reference;
  changed.nativeRegionColors[2].r += 0.000000000000001;
  require(!Cadmium::validateSessionAgainstFrames(drawing, fixture.target,
              {changed}, fixture.session.kind, error),
          "native reference provenance was trusted without checking its exact fill");

  Cadmium::Session invalid = fixture.session;
  invalid.drawings[0].regions[1].selected = 8;
  require(!Cadmium::saveSession(root, scene, invalid, error) && read(path) == original,
          "invalid replacement changed the previous complete session");
  invalid = fixture.session;
  invalid.drawings[0].regions[1].candidates[0].nativeColor.r =
      std::numeric_limits<double>::quiet_NaN();
  require(!Cadmium::saveSession(root, scene, invalid, error) && read(path) == original,
          "nonfinite native values changed the previous complete session");
  invalid = fixture.session;
  invalid.options.lineSourceKey = "incomplete";
  require(!Cadmium::saveSession(root, scene, invalid, error) && read(path) == original,
          "an incomplete clean-line identity replaced the complete session");
  QLockFile lock(path + ".lock");
  lock.setStaleLockTime(0);
  require(lock.tryLock(0), "cannot create lock fixture");
  require(!Cadmium::saveSession(root, scene, fixture.session, error) && read(path) == original,
          "a second writer ignored the active session lock");
  require(!Cadmium::loadSession(root, scene, fixture.session.levelId, loaded, error) &&
              loaded.drawings.isEmpty() && loaded.levelId.isEmpty(),
          "a locked load exposed a previous output object");
  require(!Cadmium::removeSession(root, scene, fixture.session.levelId, error),
          "session removal ignored its active writer");
  lock.unlock();

  const QString renamed = QDir(QFileInfo(scene).absolutePath()).filePath("renamed.tnz");
  require(Cadmium::copySession(root, scene, renamed, fixture.session.levelId, error) &&
              Cadmium::loadSession(root, renamed, fixture.session.levelId, loaded, error) &&
              read(path) == original,
          "Save As session copy changed the original session or lost its data");
  const QString renamedPath = Cadmium::sessionFilePath(root, renamed, fixture.session.levelId);
  require(renamedPath != path, "different scene identities share one cache entry");
  write(renamed, "renamed scene fixture");
  require(Cadmium::sessionFilePath(root, renamed, fixture.session.levelId) == renamedPath &&
              Cadmium::loadSession(root, renamed, fixture.session.levelId, loaded, error),
          "creating the Save As scene changed its canonical session identity");
  write(renamedPath, original);
  require(!Cadmium::loadSession(root, renamed, fixture.session.levelId, loaded, error),
          "a session from another scene identity was accepted");
  require(Cadmium::removeSession(root, renamed, fixture.session.levelId, error),
          "cannot remove a saved session");
}

void malformed(const QString &root, const QString &scene) {
  Fixture fixture;
  QString error;
  require(Cadmium::saveSession(root, scene, fixture.session, error), "cannot save malformed-test baseline");
  const QString path = Cadmium::sessionFilePath(root, scene, fixture.session.levelId);
  const QByteArray valid = read(path);
  Cadmium::Session output;
  const auto reject = [&] {
    output = fixture.session;
    require(!Cadmium::loadSession(root, scene, fixture.session.levelId, output, error) &&
                !error.isEmpty() && output.drawings.isEmpty() && output.levelId.isEmpty(),
            "malformed session was accepted or exposed stale output");
    write(path, valid);
  };
  write(path, QByteArray("{unfinished")); reject();
  write(path, valid.left(valid.size() / 2)); reject();
  edit(path, [](QJsonObject &envelope) { envelope["version"] = 999; }); reject();
  edit(path, [](QJsonObject &envelope) {
    QJsonObject payload = envelope["payload"].toObject();
    payload["selected"] = 400;
    envelope["payload"] = payload;
  }, false); reject();
  edit(path, [](QJsonObject &envelope) {
    QJsonObject payload = envelope["payload"].toObject();
    QJsonArray drawings = payload["drawings"].toArray();
    drawings.append(drawings[0]); payload["drawings"] = drawings;
    envelope["payload"] = payload;
  }); reject();
  edit(path, [](QJsonObject &envelope) {
    QJsonObject payload = envelope["payload"].toObject();
    QJsonArray drawings = payload["drawings"].toArray();
    QJsonObject drawing = drawings[0].toObject();
    drawing["labels"] = "00";
    drawings[0] = drawing; payload["drawings"] = drawings;
    envelope["payload"] = payload;
  }); reject();
  edit(path, [](QJsonObject &envelope) {
    QJsonObject payload = envelope["payload"].toObject();
    QJsonArray drawings = payload["drawings"].toArray();
    QJsonObject drawing = drawings[0].toObject();
    QJsonArray regions = drawing["regions"].toArray();
    QJsonObject region = regions[1].toObject();
    QJsonArray candidates = region["candidates"].toArray();
    QJsonObject candidate = candidates[0].toObject();
    candidate["reference"] = 12;
    candidates[0] = candidate; region["candidates"] = candidates;
    regions[1] = region; drawing["regions"] = regions;
    drawings[0] = drawing; payload["drawings"] = drawings;
    envelope["payload"] = payload;
  }); reject();
  write(path, QByteArray(int(Cadmium::MaximumSessionBytes + 1), ' ')); reject();
  write(path + ".tmp", valid);
  QFile::remove(path);
  reject();
  require(Cadmium::sessionFilePath(root, QString(), fixture.session.levelId).isEmpty() &&
              Cadmium::sessionFilePath(root, scene, "invalid").isEmpty(),
          "missing saved identity selected an unbounded session path");
}

void queued(const QString &root, const QString &scene) {
  Cadmium::Session session;
  session.levelId = uuid();
  session.selectedDrawing = 0;
  Cadmium::SessionDrawing drawing;
  drawing.fid = TFrameId(3);
  drawing.references.append({TFrameId(9), {}, 0});
  session.drawings.append(drawing);
  QString error;
  require(Cadmium::saveSession(root, scene, session, error), "queued assignments were not persisted");
  Cadmium::Session loaded;
  require(Cadmium::loadSession(root, scene, session.levelId, loaded, error) &&
              !loaded.drawings[0].ready && loaded.drawings[0].regions.isEmpty() &&
              loaded.drawings[0].references[0].fid == TFrameId(9),
          "queued assignments became a ready proposal on restore");
  Cadmium::Frame invalid;
  require(Cadmium::sessionLabelsDigest(invalid).isEmpty(), "invalid topology produced a usable digest");
}

void pendingInspection(const QString &root, const QString &scene) {
  Fixture fixture;
  Cadmium::SessionDrawing &drawing = fixture.session.drawings[0];
  drawing.ready = false;
  drawing.reviewed = false;
  for (Cadmium::SessionReference &reference : drawing.references) {
    reference.fingerprint.clear(); reference.regionCount = 0;
  }
  for (Cadmium::RegionProposal &region : drawing.regions) {
    region.references.clear();
    for (Cadmium::ProposalCandidate &candidate : region.candidates) {
      candidate.referenceIndex = candidate.referenceRegion = -1;
      candidate.supportingReferences.clear();
    }
  }
  QString error;
  require(Cadmium::saveSession(root, scene, fixture.session, error),
          "pre-generation native choices and protection could not be saved");
  Cadmium::Session loaded;
  require(Cadmium::loadSession(root, scene, fixture.session.levelId, loaded, error) &&
              Cadmium::validateSessionAgainstFrames(loaded.drawings[0], fixture.target,
                  {fixture.reference}, fixture.session.kind, error) &&
              !loaded.drawings[0].ready && loaded.drawings[0].regions[1].locked &&
              loaded.drawings[0].regions[2].protectedRegion,
          "pre-generation inspection lost its protected or manual native decisions");
  drawing.ready = true;
  require(!Cadmium::validateSession(fixture.session, error),
          "pending reference metadata was accepted as a completed inference");

  Fixture partial;
  partial.session.drawings[0].ready = false;
  partial.session.drawings[0].reviewed = false;
  partial.session.drawings[0].references.append({TFrameId(8), {}, 0});
  require(!Cadmium::validateSession(partial.session, error),
          "partial model votes omitted an assigned reference fingerprint");
  partial.session.drawings[0].references[1].fingerprint = hash("second reference");
  partial.session.drawings[0].references[1].regionCount = 3;
  partial.session.drawings[0].ready = true;
  partial.session.drawings[0].regions[1].locked = false;
  require(!Cadmium::validateSession(partial.session, error),
          "one processed reference became automatic agreement across two assigned references");
  partial.session.drawings[0].regions[1].locked = true;
  require(Cadmium::validateSession(partial.session, error),
          "an explicit artist choice was rejected because references disagree");
}

void aliases(const QString &root, const QString &scene) {
  Fixture fixture;
  const QByteArray source = hash("resource and cast identity");
  const QByteArray differentSource = hash("another resource and cast identity");
  QString error;
  require(Cadmium::saveSessionWithAlias(root, scene, source, fixture.session, error),
          "first-use session recovery index was not saved");
  const QString primary = Cadmium::sessionFilePath(root, scene, fixture.session.levelId);
  const QString alias = Cadmium::sessionAliasPath(root, scene, source);
  const QByteArray primaryBytes = read(primary), aliasBytes = read(alias);
  const QString currentId = uuid();
  const QString currentPath = Cadmium::sessionFilePath(root, scene, currentId);
  Cadmium::Session recovered;
  require(Cadmium::loadSessionWithAlias(root, scene, currentId, source, recovered, error) &&
              recovered.levelId == currentId && recovered.drawings.size() == 1 &&
              Cadmium::validateSessionAgainstFrames(recovered.drawings[0], fixture.target,
                  {fixture.reference}, fixture.session.kind, error),
          "a changed runtime level identity lost an otherwise valid first-use session");
  Cadmium::SessionOptions reopenedOptions = fixture.session.options;
  reopenedOptions.lineLevelId = uuid();
  recovered.options.lineLevelId = reopenedOptions.lineLevelId;
  require(recovered.options.lineSourceKey == reopenedOptions.lineSourceKey &&
              Cadmium::sessionOptionsDigest(recovered.options) ==
                  Cadmium::sessionOptionsDigest(reopenedOptions) &&
              Cadmium::validateSessionAgainstFrames(recovered.drawings[0], fixture.target,
                  {fixture.reference}, fixture.session.kind, error),
          "clean-line discovery could not retain exact artwork after runtime identity rekeying");
  Cadmium::Frame changed = fixture.target;
  changed.fingerprint = hash("different artwork");
  require(!Cadmium::validateSessionAgainstFrames(recovered.drawings[0], changed,
              {fixture.reference}, fixture.session.kind, error),
          "discovery bypassed the artwork fingerprint check");
  require(!Cadmium::loadSessionWithAlias(root, scene, currentId, differentSource,
              recovered, error) && recovered.levelId.isEmpty(),
          "a different source discovered an unrelated session by scanning the cache");

  write(currentPath, "{corrupt primary");
  require(!Cadmium::loadSessionWithAlias(root, scene, currentId, source, recovered, error),
          "a corrupt primary silently fell back to a different session");
  require(QFile::remove(currentPath), "cannot remove corrupt primary fixture");
  QLockFile primaryLock(currentPath + ".lock");
  primaryLock.setStaleLockTime(0);
  require(primaryLock.tryLock(0), "cannot lock missing primary fixture");
  require(!Cadmium::loadSessionWithAlias(root, scene, currentId, source, recovered, error),
          "an active primary writer was bypassed by recovery discovery");
  primaryLock.unlock();

  write(alias, "{unfinished recovery index");
  require(!Cadmium::loadSessionWithAlias(root, scene, currentId, source, recovered, error),
          "a corrupt recovery index was accepted");
  require(Cadmium::loadSessionWithAlias(root, scene, fixture.session.levelId, source,
              recovered, error),
          "an unnecessary recovery index overrode a valid primary");
  write(alias, aliasBytes);
  QJsonObject wrong = QJsonDocument::fromJson(aliasBytes).object();
  wrong["level"] = uuid();
  write(alias, QJsonDocument(wrong).toJson(QJsonDocument::Compact));
  require(!Cadmium::loadSessionWithAlias(root, scene, currentId, source, recovered, error),
          "an altered recovery pointer passed its integrity check");
  wrong = QJsonDocument::fromJson(aliasBytes).object();
  wrong["version"] = 999;
  write(alias, QJsonDocument(wrong).toJson(QJsonDocument::Compact));
  require(!Cadmium::loadSessionWithAlias(root, scene, currentId, source, recovered, error),
          "an unsupported recovery version was accepted");
  write(alias, QByteArray(4097, ' '));
  require(!Cadmium::loadSessionWithAlias(root, scene, currentId, source, recovered, error),
          "an oversized recovery index was accepted");
  write(alias, aliasBytes);
  const QString otherAlias = Cadmium::sessionAliasPath(root, scene, differentSource);
  write(otherAlias, aliasBytes);
  require(!Cadmium::loadSessionWithAlias(root, scene, currentId, differentSource,
              recovered, error),
          "a copied index was accepted for a different resource identity");

  QLockFile indexLock(alias + ".lock");
  indexLock.setStaleLockTime(0);
  require(indexLock.tryLock(0), "cannot lock recovery index fixture");
  Cadmium::Session updated = fixture.session;
  updated.drawings[0].regions[1].selected = 0;
  require(!Cadmium::saveSessionWithAlias(root, scene, source, updated, error) &&
              read(primary) == primaryBytes && read(alias) == aliasBytes,
          "an active recovery writer did not protect the previous complete session");
  require(!Cadmium::removeSessionWithAlias(root, scene, currentId, source, error),
          "clear ignored the recovery transaction lock");
  indexLock.unlock();
  require(Cadmium::removeSessionWithAlias(root, scene, currentId, source, error) &&
              !QFileInfo::exists(alias) && !QFileInfo::exists(primary) &&
              QFileInfo::exists(otherAlias),
          "clear left a discoverable old session or touched an unrelated cache entry");
  require(!Cadmium::loadSessionWithAlias(root, scene, currentId, source, recovered, error),
          "a cleared first-use session was rediscovered");
}
}

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir temporary;
    require(temporary.isValid(), "cannot create session test storage");
    const QString scene = QDir(temporary.path()).filePath("scene.tnz");
    write(scene, "scene fixture");
    const QString root = QDir(temporary.path()).filePath("cache");
    roundTrip(root, scene);
    malformed(root, scene);
    queued(root, scene);
    pendingInspection(root, scene);
    aliases(root, scene);
    std::cout << "Cadmium session persistence and recovery checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
