#include "cadmiumconsensus.h"
#include "cadmiumregeneration.h"
#include "cadmiumsession.h"
#include "cadmiummemory.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

QByteArray digest(const char *value) {
  return QCryptographicHash::hash(QByteArray(value), QCryptographicHash::Sha256);
}

const QColor red(255, 40, 20, 255);
const QColor blue(20, 60, 230, 255);
const Cadmium::NativeColor nativeRed{1.23456789, 0.156862745, 0.0784313725,
                                    1.0, false, true};
const Cadmium::NativeColor nativeBlue{0.0784313725, 0.2352941176, 0.9019607843,
                                     1.0, false, true};

Cadmium::Frame drawing(const QByteArray &fingerprint) {
  Cadmium::Frame frame;
  frame.kind = Cadmium::Kind::FullColor;
  frame.lines = QImage(4, 3, QImage::Format_ARGB32_Premultiplied);
  frame.lines.fill(Qt::transparent);
  frame.labels = {0, 0, 0, 0, 0, 1, 2, 0, 0, 1, 2, 0};
  frame.regionColors.resize(3);
  frame.regionStyles.fill(-1, 3);
  frame.nativeRegionColors.resize(3);
  frame.fingerprint = fingerprint;
  return frame;
}

Cadmium::Frame reference(const QByteArray &fingerprint) {
  Cadmium::Frame frame = drawing(fingerprint);
  frame.regionColors[1] = red;
  frame.regionColors[2] = blue;
  frame.nativeRegionColors[1] = nativeRed;
  frame.nativeRegionColors[2] = nativeBlue;
  return frame;
}

QJsonArray rgba(const QColor &color) {
  return QJsonArray{color.red(), color.green(), color.blue(), color.alpha()};
}

QJsonObject response(bool secondRegionRed) {
  return QJsonObject{
      {"target_color_ids", QJsonArray{-100, 1, secondRegionRed ? 1 : 2}},
      {"target_colors_rgba", QJsonArray{
          QJsonArray{-100, -100, -100, -100}, rgba(red),
          rgba(secondRegionRed ? red : blue)}}};
}

int choiceFor(const Cadmium::RegionProposal &region,
              const Cadmium::NativeColor &native) {
  for (int choice = 0; choice < region.candidates.size(); ++choice)
    if (region.candidates[choice].nativeColor == native) return choice;
  return -1;
}

void recoveryTransitions() {
  Cadmium::SessionDrawing saved;
  saved.fid = TFrameId(7);
  saved.regions.resize(3);
  saved.regions[1].selected = -2; saved.regions[1].locked = true;
  saved.regions[2].protectedRegion = true;
  Cadmium::SessionRecovery recovery;
  require(!recovery.stage(saved) && recovery.contains(saved.fid),
          "unapplied recovery was not held for full validation");
  const auto refused = Cadmium::planCacheAdmission({{80, false}}, 10, 20, 100);
  require(!refused.allowed && recovery.contains(saved.fid) &&
              recovery.records().value(saved.fid).regions[1].locked &&
              recovery.records().value(saved.fid).regions[2].protectedRegion,
          "failed analysis admission discarded pending review choices");
  // The production inspection/autosave paths consult contains(); no fallback
  // inspection is permitted to overwrite a still-pending saved record.
  const Cadmium::SessionDrawing validated = recovery.finish(saved.fid);
  require(!recovery.contains(saved.fid) && validated.regions[1].selected == -2 &&
              validated.regions[2].protectedRegion,
          "successful validation lost decisions or left recovery pending");
  recovery.stage(saved);
  recovery.discard(saved.fid);
  require(!recovery.contains(saved.fid),
          "an explicit reference change retained stale recovery eligibility");
  recovery.stage(saved); recovery.clear();
  require(!recovery.contains(saved.fid),
          "changed input options retained pending recovery decisions");
  saved.applied = true; saved.regions.clear();
  const bool applied = recovery.stage(saved);
  require(applied && recovery.contains(saved.fid),
          "saved applied history was lost before analysis admission");
  require(!refused.allowed && applied && recovery.records().value(saved.fid).applied,
          "refused analysis made an applied history entry regeneratable");
}

void memoryAdmission() {
  // Reproduce a long browsing session with a tiny injectable budget. Only
  // pristine inactive inspection caches can be evicted; a locked decision and
  // the currently visible drawing must survive every admission.
  QVector<Cadmium::RegionProposal> pristine(3);
  pristine[0].selected = -2; pristine[0].protectedRegion = true;
  require(!Cadmium::hasRegionDecisions(pristine),
          "the automatically protected exterior pinned an inspection cache");
  QVector<Cadmium::RegionProposal> decided = pristine;
  decided[1].selected = -2; decided[1].locked = true;
  require(Cadmium::hasRegionDecisions(decided), "an explicit skip was evictable");
  QVector<Cadmium::CacheEntry> entries{{40, !Cadmium::hasRegionDecisions(decided)}};
  constexpr qint64 limit = 100, fixed = 10, preview = 20;
  for (int drawing = 0; drawing < 100; ++drawing) {
    const auto admission = Cadmium::planCacheAdmission(entries, fixed, preview, limit);
    require(admission.allowed, "browsing exhausted the cache instead of evicting old inspections");
    for (int index : admission.evictions) {
      require(index > 0 && entries[index].evictable, "an artist decision or visible cache was evicted");
      entries[index].bytes = 0;
    }
    // Switching the visible row releases the previous inspection's pin.
    for (int index = 1; index < entries.size(); ++index) entries[index].evictable = true;
    entries.append({preview, false});
    qint64 retained = fixed;
    for (const auto &entry : entries) retained += entry.bytes;
    require(retained <= limit && entries[0].bytes == 40 && decided[1].locked && decided[1].selected == -2,
            "browsing exceeded its budget or lost a manual decision");
  }
  const auto rejected = Cadmium::planCacheAdmission(entries, fixed, 40, limit);
  require(!rejected.allowed && rejected.evictions.isEmpty(),
          "a rejected request partially evicted the existing review state");
  QVector<Cadmium::RegionProposal> protectedOnly = pristine;
  protectedOnly[2].protectedRegion = true;
  require(Cadmium::hasRegionDecisions(protectedOnly), "manual region protection was evictable");
  QVector<Cadmium::RegionProposal> generated = pristine;
  generated[1].references.append(0);
  require(Cadmium::hasRegionDecisions(generated), "partial reference consensus was evictable");
  require(!Cadmium::planCacheAdmission({{std::numeric_limits<qint64>::max(), false},
                                       {1, false}}, 0, 0).allowed,
          "overflow admitted an oversized cache");
  require(Cadmium::addMemoryBytes(std::numeric_limits<qint64>::max(), 1) ==
              std::numeric_limits<qint64>::max(), "memory accounting wrapped around");

  Cadmium::Frame frame = drawing(digest("memory-accounting"));
  const qint64 initial = Cadmium::frameMemoryBytes(frame);
  frame.protectionMask = QImage(4, 3, QImage::Format_ARGB32);
  frame.protectionMask.fill(Qt::black);
  frame.nativeRegionIndices.reserve(64);
  frame.nativeRegionColors.reserve(64);
  frame.regionStyles.reserve(64);
  frame.regionColors.reserve(64);
  frame.labels.reserve(64);
  const qint64 withMetadata = Cadmium::frameMemoryBytes(frame);
  require(withMetadata >= initial + frame.protectionMask.sizeInBytes() + 61 * sizeof(Cadmium::NativeColor),
          "mask or reserved region metadata was omitted from cache accounting");
  Cadmium::Session session;
  const qint64 emptySession = Cadmium::sessionMemoryBytes(session);
  Cadmium::SessionDrawing record; record.regions = decided;
  record.regions[1].candidates.resize(8);
  session.drawings.append(record);
  require(Cadmium::sessionMemoryBytes(session) >= emptySession + Cadmium::proposalMemoryBytes(record.regions),
          "retained session decisions were omitted from cache accounting");
}

void workflow(const QDir &directory) {
  const Cadmium::Frame target = drawing(digest("target"));
  const Cadmium::Frame first = reference(digest("first-reference"));
  const Cadmium::Frame second = reference(digest("second-reference"));
  const QVector<Cadmium::Frame> references{first, second};
  QVector<Cadmium::RegionProposal> firstPass, secondPass, combined;
  QString error;
  require(Cadmium::parseReferenceProposals(response(false), first, target, 0,
                                          firstPass, error),
          "first reference predictions were not decoded");
  require(Cadmium::parseReferenceProposals(response(true), second, target, 1,
                                          secondPass, error),
          "second reference predictions were not decoded");
  require(Cadmium::mergeReferenceProposals(combined, firstPass, target.kind, error) &&
              Cadmium::mergeReferenceProposals(combined, secondPass, target.kind, error),
          "reference proposals could not be combined");
  require(combined.size() == 3 && combined[0].protectedRegion &&
              combined[1].selected >= 0 && combined[2].selected == -1,
          "unanimous and conflicting predictions were not distinguished");
  require(combined[1].candidates[combined[1].selected].nativeColor == nativeRed,
          "the unanimous native fill lost its exact samples");
  const int blueChoice = choiceFor(combined[2], nativeBlue);
  require(blueChoice >= 0 && Cadmium::chooseProposal(combined[2], blueChoice),
          "the conflicting region could not receive a manual choice");
  Cadmium::protectProposal(combined[1], true);

  Cadmium::Session session;
  session.levelId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  session.kind = target.kind;
  session.selectedDrawing = 0;
  Cadmium::SessionDrawing saved;
  saved.fid = TFrameId(1, QStringLiteral("A"));
  saved.targetFingerprint = target.fingerprint;
  saved.labelsFingerprint = Cadmium::sessionLabelsDigest(target);
  saved.canvas = target.lines.size();
  saved.references = {
      {TFrameId(2), first.fingerprint, first.regionColors.size()},
      {TFrameId(3), second.fingerprint, second.regionColors.size()}};
  saved.regions = combined;
  saved.ready = true;
  saved.reviewed = true;
  session.drawings.append(saved);
  require(Cadmium::validateSessionAgainstFrames(saved, target, references,
                                               target.kind, error),
          "the new proposal did not match its native source frames");

  const QString scene = directory.filePath("painting.tnz");
  QFile sceneFile(scene);
  require(sceneFile.open(QIODevice::WriteOnly) && sceneFile.write("scene") == 5,
          "cannot create workflow scene fixture");
  sceneFile.close();
  const QString cache = directory.filePath("sessions");
  require(Cadmium::saveSession(cache, scene, session, error),
          "review decisions could not be saved");
  Cadmium::Session recovered;
  require(Cadmium::loadSession(cache, scene, session.levelId, recovered, error) &&
              recovered.drawings.size() == 1,
          "review decisions could not be recovered");
  const Cadmium::SessionDrawing &restored = recovered.drawings[0];
  require(restored.reviewed && restored.ready && restored.fid == saved.fid &&
              restored.regions[1].protectedRegion && restored.regions[2].locked &&
              restored.regions[2].candidates[restored.regions[2].selected].nativeColor == nativeBlue,
          "recovery lost the reviewed, protected, locked or exact native state");
  require(Cadmium::validateSessionAgainstFrames(restored, target, references,
                                               target.kind, error),
          "recovered decisions failed fresh source validation");

  Cadmium::Frame renumbered = target;
  for (int &label : renumbered.labels)
    if (label > 0) label = 3 - label;
  renumbered.fingerprint = digest("renumbered-labels");
  const auto moved = Cadmium::regenerationPlan(
      target, renumbered, restored.regions, {}, Cadmium::RegenerationMode::All);
  require(moved.valid() && moved.previousToCurrent.value(1, -1) == 2 &&
              moved.previousToCurrent.value(2, -1) == 1 && moved.requested.isEmpty() &&
              moved.regions[2].protectedRegion && moved.regions[1].locked &&
              moved.regions[1].candidates[moved.regions[1].selected].nativeColor == nativeBlue,
          "region number changes redirected protected or locked decisions");

  QVector<Cadmium::RegionProposal> unlocked = restored.regions;
  Cadmium::protectProposal(unlocked[1], false);
  const auto selective = Cadmium::regenerationPlan(
      target, target, unlocked, QSet<int>{1},
      Cadmium::RegenerationMode::SelectedRegions);
  require(selective.valid() && selective.requested.size() == 1 &&
              selective.requested.contains(1) && selective.invalidateReview &&
              selective.regions[2].locked,
          "selective regeneration did not retain the other manual decision");
  QVector<Cadmium::RegionProposal> regenerated = selective.regions;
  require(Cadmium::mergeReferenceProposals(regenerated, firstPass, target.kind, error) &&
              Cadmium::mergeReferenceProposals(regenerated, secondPass, target.kind, error),
          "selected-region predictions could not be combined again");
  require(regenerated[1].selected >= 0 && regenerated[2].locked &&
              regenerated[2].candidates[regenerated[2].selected].nativeColor == nativeBlue,
          "regeneration replaced the locked correction with a conflicting prediction");

  Cadmium::Frame changed = target;
  changed.labels[5] = 2;
  changed.fingerprint = digest("changed-geometry");
  const auto staleSelection = Cadmium::regenerationPlan(
      target, changed, unlocked, QSet<int>{1},
      Cadmium::RegenerationMode::SelectedRegions);
  require(!staleSelection.valid() && staleSelection.requested.isEmpty(),
          "a changed selected mask was reused by region number");
  require(!Cadmium::validateSessionAgainstFrames(restored, changed, references,
                                                target.kind, error),
          "recovery accepted changed target geometry");
  QVector<Cadmium::Frame> reordered{second, first};
  require(!Cadmium::validateSessionAgainstFrames(restored, target, reordered,
                                                target.kind, error),
          "reordered reference identities redirected recovered decisions");
  Cadmium::SessionDrawing tampered = restored;
  tampered.regions[2].candidates[tampered.regions[2].selected].nativeColor.r += 0.000001;
  require(!Cadmium::validateSessionAgainstFrames(tampered, target, references,
                                                target.kind, error),
          "a native fill detached from its reference was restored");

  Cadmium::Session invalid = recovered;
  invalid.drawings[0].regions[2].candidates[blueChoice].nativeColor.r =
      std::numeric_limits<double>::quiet_NaN();
  require(!Cadmium::saveSession(cache, scene, invalid, error),
          "invalid recovered native color was saved");
  Cadmium::Session previous;
  require(Cadmium::loadSession(cache, scene, session.levelId, previous, error) &&
              previous.drawings[0].regions[2].candidates[blueChoice].nativeColor == nativeBlue,
          "a failed save replaced the previous complete review session");
  const QByteArray options = Cadmium::sessionOptionsDigest(session.options);
  session.options.lineThreshold = 0.3;
  require(options != Cadmium::sessionOptionsDigest(session.options),
          "changed extraction settings retained the same session identity");
  require(Cadmium::removeSession(cache, scene, session.levelId, error),
          "the saved workflow session could not be cleared");
}
}

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir temporary;
    require(temporary.isValid(), "cannot create workflow test storage");
    memoryAdmission();
    recoveryTransitions();
    workflow(QDir(temporary.path()));
    std::cout << "Cadmium reference, review, recovery and regeneration workflow passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
