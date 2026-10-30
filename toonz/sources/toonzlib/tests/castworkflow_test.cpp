#include "toonz/castworkflow.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/sceneproperties.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshexrlevel.h"
#include "toonz/txshsoundlevel.h"
#include "toonz/txshpalettelevel.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshsoundtextlevel.h"
#include "toonz/txshleveltypes.h"
#include "tenv.h"
#include "tstream.h"
#include "tvectorimage.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void queryAndTags() {
  auto tags = CastWorkflow::normalizeTags(QString::fromUtf8(" hero, Keep, HERO, café, , <&\" "));
  require(tags.size() == 4, "tag normalization lost unicode or retained duplicates");
  require(CastWorkflow::matches("HERO café", "run", "/drawings/run.pli", tags), "tag/name search did not match");
  require(CastWorkflow::matches("drawings RUN", "run", "/drawings/run.pli", {}), "path/name tokens did not match");
  require(!CastWorkflow::matches("run missing", "run", "/drawings/run.pli", {}), "search used OR instead of AND");
}
void occurrencePaths() {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  auto *art = scene.createNewLevel(PLI_XSHLEVEL, L"Art");
  auto *child = scene.createNewLevel(CHILD_XSHLEVEL, L"Shared")->getChildLevel();
  TXsheet *sheet = child->getXsheet();
  sheet->setCell(3, 2, TXshCell(art, TFrameId(1)));
  sheet->setCell(4, 2, TXshCell(art, TFrameId(2)));
  auto *top = scene.getTopXsheet();
  top->setCell(10, 0, TXshCell(child, TFrameId(4)));
  top->setCell(11, 0, TXshCell(child, TFrameId(5)));
  top->setCell(20, 0, TXshCell(child, TFrameId(4)));
  top->setCell(0, 1, TXshCell(child, TFrameId(4)));
  const auto result = CastWorkflow::index(top, {art});
  require(result.complete && !result.cycle, "valid scene index incomplete");
  require(result.occurrences.size() == 3, "shared instances were collapsed or every drawing became an instance");
  require(result.used.count(art) && result.used.count(child), "used identity set incomplete");
  CastWorkflow::Limits summary;
  summary.recordOccurrences = false; summary.expandInstances = false;
  const auto unique = CastWorkflow::index(top, {}, {}, summary);
  require(unique.complete && unique.occurrences.empty() && unique.used == result.used && unique.cells < result.cells,
          "usage-only indexing repeated shared sheet instances or allocated occurrence rows");
  for (const auto &use : result.occurrences) {
    require(use.first == 3 && use.last == 4 && use.column == 2, "local exposure range incorrect");
    require(use.path.size() == 1 && CastWorkflow::validPath(top, use), "nested path cannot navigate");
  }
  top->setCell(10, 0, TXshCell());
  require(!CastWorkflow::validPath(top, result.occurrences.front()), "stale path still accepted");
  CastWorkflow::Limits limits; limits.cells = 1;
  require(!CastWorkflow::index(top, {art}, {}, limits).complete, "cell budget ignored");
  require(!CastWorkflow::index(top, {art}, []() { return true; }).complete, "cancellation ignored");
  sheet->setCell(8, 0, TXshCell(child, TFrameId(1)));
  require(CastWorkflow::index(top, {art}).cycle, "cycle was not detected");
  sheet->setCell(8, 0, TXshCell());
  auto *simple = art->getSimpleLevel();
  simple->setFrame(TFrameId(1), new TVectorImage);
  simple->setFrame(TFrameId(2), new TVectorImage);
  simple->setCastCover(TFrameId(2));
  simple->renumber({TFrameId(4), TFrameId(6)});
  require(simple->getCastCover() == TFrameId(6), "drawing renumber lost cover identity");
  simple->renumber({TFrameId(1), TFrameId(2)});
  require(simple->getCastCover() == TFrameId(2), "reverse renumber did not restore cover identity");
}
void soundReplacementDuration() {
  TXshSoundLevelP sound = new TXshSoundLevel;
  sound->setFrameRate(24);
  sound->setSoundTrack(TSoundTrack::create(8000, 16, 1, 8000, TSound::INT));
  require(sound->getFrameCount() == 24, "initial sound duration is stale");
  sound->setSoundTrack(TSoundTrack::create(8000, 16, 1, 16000, TSound::INT));
  require(sound->getFrameCount() == 48, "replacement sound duration is stale");
  sound->setSoundTrack(TSoundTrackP());
  require(sound->getFrameCount() == 1, "cleared soundtrack retained duration");
}
void tagsRoundTrip(const QString &directory) {
  const QStringList tags{QString::fromUtf8("café"), "A&B", "quote\"", "<hero>", "keep"};
  for (int type : {PLI_XSHLEVEL, OVL_XSHLEVEL, SND_XSHLEVEL, PLT_XSHLEVEL}) {
    TXshLevelP source;
    if (type == PLI_XSHLEVEL) { source = new TXshSimpleLevel(L"Art"); source->setType(type); }
    if (type == OVL_XSHLEVEL) source = new TXshExrLevel(L"Layered EXR");
    if (type == SND_XSHLEVEL) source = new TXshSoundLevel(L"Audio");
    if (type == PLT_XSHLEVEL) source = new TXshPaletteLevel(L"Palette");
    source->setCastTags(tags);
    source->setCastCover(TFrameId(12, QString::fromUtf8("a")));
    const TFilePath path(QDir(directory).filePath(QString::number(type) + ".xml"));
    { TOStream os(path); os << source.getPointer(); }
    TPersist *loaded = nullptr;
    { TIStream is(path); is >> loaded; }
    TXshLevelP restored = dynamic_cast<TXshLevel *>(loaded);
    require(restored && restored->getCastTags() == tags && restored->hasCastCover() &&
            restored->getCastCover() == TFrameId(12, QString::fromUtf8("a")),
            "tags or cover did not survive native persistence");
    TXshSimpleLevel metadataCopy;
    metadataCopy.copyCastMetadataFrom(restored.getPointer());
    require(metadataCopy.hasCastCover() && metadataCopy.getCastCover() == restored->getCastCover() &&
            metadataCopy.getCastTags() == tags, "native metadata copy dropped cover or tags");
    source->setCastTags({});
    source->clearCastCover();
    { TOStream os(path); os << source.getPointer(); }
    QFile file(path.getQString()); require(file.open(QIODevice::ReadOnly), "cannot inspect plain output");
    require(!file.readAll().contains("castTags"), "untagged asset added new serialization");
  }
}
void collectionsRoundTrip(const QString &directory) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  auto *a = scene.createNewLevel(PLI_XSHLEVEL, L"Collection A");
  auto *b = scene.createNewLevel(PLI_XSHLEVEL, L"Collection B");
  const QString aId = a->getCastId(), bId = b->getCastId();
  require(!aId.isEmpty() && aId != bId, "new levels reused Cast identity");
  TSceneProperties::CastCollection collection;
  collection.id = "collection-fixture";
  collection.name = QString::fromUtf8("café <&\" collection");
  collection.members = QStringList{aId, bId};
  scene.getProperties()->setCastCollections({collection});
  TXshSimpleLevel clone;
  clone.copyCastMetadataFrom(a);
  require(clone.getCastId() != aId && clone.getCastId() != bId, "metadata clone copied membership identity");
  const TFilePath path(QDir(directory).filePath("collections.tnz"));
  scene.save(path, nullptr, false);
  ToonzScene restored;
  restored.loadTnzFile(path);
  require(restored.getProperties()->getCastCollections() == QList<TSceneProperties::CastCollection>{collection},
          "named collection metadata lost Unicode or exact membership on native scene reload");
  require(restored.getLevelSet()->getLevel(L"Collection A")->getCastId() == aId &&
          restored.getLevelSet()->getLevel(L"Collection B")->getCastId() == bId,
          "scene reload did not preserve member asset identity");
  require(!restored.getLevelSet()->getLevel(L"Collection A")->sourceExpectedOnDisk(),
          "unsaved source was relabeled missing after native reload");
  TSceneProperties copied;
  copied.assign(restored.getProperties());
  require(copied.getCastCollections() == restored.getProperties()->getCastCollections(),
          "scene property copy omitted named collections");
  copied.setCastCollections({});
  require(!restored.getProperties()->getCastCollections().empty(), "collection copies share mutable membership");
  const TFilePath defaults(QDir(directory).filePath("collection-project-defaults.xml"));
  { TOStream os(defaults); os.openChild("properties"); scene.getProperties()->saveData(os); os.closeChild(); }
  { TIStream is(defaults); std::string tag; require(is.matchTag(tag), "missing project-properties fixture"); copied.loadData(is, true); }
  require(copied.getCastCollections().empty(), "scene-specific asset UUIDs leaked into project defaults");
}
void ownershipRoundTrip(const QString &directory) {
  using Origin = TXshLevel::LibraryProvenance;
  Origin origin;
  origin.itemId = "fixture-library-item";
  origin.itemPath = QString::fromUtf8("/Library/templates/café & rig.tnz");
  origin.kind = "rig";
  origin.sourceAssetPath = "/Library/templates/rig/asset.pli";
  origin.revision = 7;
  origin.relationship = Origin::LocalCopy;
  for (int type : {PLI_XSHLEVEL, OVL_XSHLEVEL, SND_XSHLEVEL, PLT_XSHLEVEL,
                   CHILD_XSHLEVEL, SND_TXT_XSHLEVEL}) {
    TXshLevelP source;
    switch (type) {
    case PLI_XSHLEVEL: source = new TXshSimpleLevel(L"Provenance"); source->setType(type); break;
    case OVL_XSHLEVEL: source = new TXshExrLevel(L"Provenance EXR"); break;
    case SND_XSHLEVEL: source = new TXshSoundLevel(L"Provenance audio"); break;
    case PLT_XSHLEVEL: source = new TXshPaletteLevel(L"Provenance palette"); break;
    case CHILD_XSHLEVEL: source = new TXshChildLevel(L"Provenance snapshot"); break;
    case SND_TXT_XSHLEVEL: source = new TXshSoundTextLevel(L"Provenance text"); break;
    }
    source->setLibraryProvenance(origin);
    const QString identity = source->getCastId();
    require(!identity.isEmpty() && !source->sourceExpectedOnDisk(), "new asset identity/save state is invalid");
    for (bool expected : {false, true}) {
      source->setSourceExpectedOnDisk(expected);
      const TFilePath path(QDir(directory).filePath(QString("ownership-%1.xml").arg(type)));
      { TOStream os(path); os << source.getPointer(); }
      TPersist *loaded = nullptr;
      { TIStream is(path); is >> loaded; }
      TXshLevelP restored = dynamic_cast<TXshLevel *>(loaded);
      require(restored && restored->getCastId() == identity &&
                  restored->getLibraryProvenance() == origin && restored->sourceExpectedOnDisk() == expected,
              "native ownership metadata did not round-trip on every Cast level type");
      TXshSimpleLevel cloned;
      const QString cloneId = cloned.getCastId();
      cloned.copyCastMetadataFrom(restored.getPointer());
      require(cloned.getCastId() == cloneId && cloneId != identity &&
                  !cloned.sourceExpectedOnDisk() && cloned.getLibraryProvenance() == origin,
              "metadata clone copied UUID/save state or lost Library attribution");
    }
  }
  // Invalid metadata cannot invent an identity or a Library relationship.
  const TFilePath invalid(QDir(directory).filePath("invalid-ownership.xml"));
  QJsonObject forged{{"id", "item"}, {"item", "/external/file.tnz"}, {"kind", "rig"},
                     {"revision", -1}, {"relationship", int(Origin::Linked)}};
  { TOStream os(invalid);
    os.openChild("path", {{"castId", "not-a-uuid"},
                           {"libraryOrigin", QJsonDocument(forged).toJson(QJsonDocument::Compact).toStdString()}});
    os << TFilePath("asset.pli"); os.closeChild(); }
  TXshSimpleLevel recipient;
  const QString identity = recipient.getCastId();
  { TIStream is(invalid); std::string tag; require(is.matchTag(tag), "malformed metadata fixture missing");
    recipient.loadCastTags(is); }
  require(recipient.getCastId() == identity && recipient.getLibraryRelationship() == Origin::NotRecorded &&
              recipient.sourceExpectedOnDisk(),
          "invalid metadata overrode fresh identity, origin or legacy expected-source state");
  origin.relationship = Origin::Linked;
  recipient.setLibraryProvenance(origin);
  recipient.setPath(TFilePath(origin.sourceAssetPath), true);
  require(recipient.getLibraryRelationship() == Origin::Linked, "recorded source path lost its link");
  recipient.setPath(TFilePath("/elsewhere/replaced.pli"), true);
  require(recipient.getLibraryRelationship() == Origin::Replaced, "changed path still claimed Library file ownership");
  TXshSimpleLevel replacedClone;
  replacedClone.copyCastMetadataFrom(&recipient);
  replacedClone.detachLibrarySource(Origin::LocalCopy);
  require(replacedClone.getLibraryRelationship() == Origin::Replaced &&
              replacedClone.getLibraryProvenance().itemId == origin.itemId,
          "copying a replaced source falsely restored Library ownership or discarded attribution");
}
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    QString stuff = QDir(dir.path()).filePath("stuff");
    QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuff));
    queryAndTags(); occurrencePaths(); tagsRoundTrip(dir.path()); ownershipRoundTrip(dir.path()); soundReplacementDuration();
    collectionsRoundTrip(dir.path());
    std::cout << "PASS: Cast queries, instance navigation, limits and native tag persistence\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  } catch (...) { std::cerr << "Unexpected Cast workflow exception\n"; }
  return 1;
}
