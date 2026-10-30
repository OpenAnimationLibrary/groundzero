#include "toonz/castworkflow.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshexrlevel.h"
#include "toonz/txshsoundlevel.h"
#include "toonz/txshpalettelevel.h"
#include "toonz/txshleveltypes.h"
#include "tenv.h"
#include "tstream.h"
#include "tvectorimage.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
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
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    QString stuff = QDir(dir.path()).filePath("stuff");
    QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuff));
    queryAndTags(); occurrencePaths(); tagsRoundTrip(dir.path()); soundReplacementDuration();
    std::cout << "PASS: Cast queries, instance navigation, limits and native tag persistence\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  } catch (...) { std::cerr << "Unexpected Cast workflow exception\n"; }
  return 1;
}
