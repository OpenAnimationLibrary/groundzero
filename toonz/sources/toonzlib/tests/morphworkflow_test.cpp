// Native vector correspondence, birth/death policy and saved scene-hint contracts.
#include "tinbetween.h"
#include "toonz/morphhints.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "tpalette.h"
#include "tregion.h"
#include "tstream.h"
#include "tlevel_io.h"
#include "tlevel.h"
#include "tnzimage.h"
#include "tthread.h"
#include "texception.h"
#include "tenv.h"
#include "timagecache.h"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *why) {
  if (!value) throw std::runtime_error(why);
}
TVectorImageP drawing(const std::vector<double> &positions) {
  TVectorImageP image = new TVectorImage;
  image->setPalette(new TPalette);
  for (double x : positions) {
    auto stroke = new TStroke(std::vector<TThickPoint>{
        {x, 0, 2}, {x + 5, 5, 2}, {x + 10, 0, 2}});
    stroke->setStyle(1);
    image->addStroke(stroke);
  }
  return image;
}
void correspondence() {
  auto first = drawing({0, 30, 60});
  auto last = drawing({60, 0});
  TInbetween::MatchOptions options;
  options.m_autoMatch = true;
  auto review = TInbetween::reviewCorrespondence(first, last, {{1,1}, {1,0}, {-1,0}}, options);
  require(review.pairs.size() == 2, "pair count wrong");
  bool manual = false;
  for (const auto &pair : review.pairs)
    if (pair.first == 1) manual = pair.last == 1 && pair.source == TInbetween::ManualMatch;
  require(manual, "automatic matching replaced manual choice");
  require(review.unmatchedFirst.size() == 1 && review.unmatchedLast.empty(),
          "unmatched stroke list wrong");
  TInbetween tween(first, last, {{1,1}}, options);
  auto result = tween.tween(.5);
  require(result->getStrokeCount() == review.pairs.size(), "review/commit assignment diverged");
  auto defaults = TInbetween::reviewCorrespondence(first, last, {}, {});
  require(defaults.pairs[0].first == 0 && defaults.pairs[0].last == 0 &&
          defaults.pairs[1].first == 1 && defaults.pairs[1].last == 1,
          "legacy index order changed");
  options.m_autoMatch = false;
  options.m_zoneHints = {{TRectD(55,-10,75,10), TRectD(55,-10,75,10)}};
  auto zones = TInbetween::reviewCorrespondence(first, last, {}, options);
  bool foundZone = false;
  for (const auto &pair : zones.pairs)
    if (pair.first == 2 && pair.last == 0) foundZone = pair.source == TInbetween::ZoneMatch;
  require(foundZone, "zone assignment lost provenance");
}
void unmatchedAndFills() {
  auto first = drawing({0, 30});
  auto last = drawing({0});
  TInbetween::MatchOptions options;
  options.m_unmatchedPolicy = TInbetween::HoldUnmatched;
  TInbetween hold(first, last, {}, options);
  require(hold.tween(.5)->getStrokeCount() == 2, "hold dropped disappearing stroke");
  require(hold.tween(0)->getStrokeCount() == 2 && hold.tween(1)->getStrokeCount() == 1,
          "endpoints are not exact drawings");
  options.m_unmatchedPolicy = TInbetween::CollapseUnmatched;
  TInbetween collapse(first, last, {}, options);
  auto mid = collapse.tween(.5);
  require(std::abs(mid->getStroke(1)->getControlPoint(0).thick - 1) < 1e-10,
          "collapse width does not follow growth factor");
  TInbetween grow(last, first, {}, options);
  require(grow.tween(.25)->getStrokeCount() == 2 &&
          std::abs(grow.tween(.25)->getStroke(1)->getControlPoint(0).thick - .5) < 1e-10,
          "appearing stroke growth is wrong");
  require(first->getStroke(1)->getControlPoint(0).thick == 2,
          "preview mutated source width");
  // A manual pairing can leave an earlier stroke unmatched. It must retain
  // its original stack position instead of moving above the paired stroke.
  TInbetween preserveStack(first, last, {{1,0}}, options);
  auto ordered = preserveStack.tween(.5);
  require(ordered->getStrokeCount() == 2 &&
          std::abs(ordered->getStroke(0)->getControlPoint(0).thick - 1) < 1e-10,
          "disappearing stroke moved out of first-drawing stack order");

  TVectorImageP closed = new TVectorImage;
  closed->setPalette(first->getPalette());
  auto box = new TStroke(std::vector<TThickPoint>{
      {0,0,1}, {5,0,1}, {10,0,1}, {10,5,1}, {10,10,1},
      {5,10,1}, {0,10,1}, {0,5,1}, {0,0,1}});
  box->setSelfLoop(); box->setStyle(1); closed->addStroke(box);
  closed->findRegions();
  require(closed->getRegionCount() > 0, "fixture has no filled region");
  closed->getRegion(0)->setStyle(1);
  auto empty = drawing({});
  TInbetween shrinkFill(closed, empty, {}, options);
  auto smaller = shrinkFill.tween(.5);
  require(smaller->getRegionCount() > 0 && smaller->getRegion(0)->getStyle() == 1,
          "disappearing closed stroke lost its fill");
  TInbetween growFill(empty, closed, {}, options);
  auto bigger = growFill.tween(.5);
  require(bigger->getRegionCount() > 0 && bigger->getRegion(0)->getStyle() == 1,
          "appearing closed stroke lost its fill");

  options.m_unmatchedPolicy = TInbetween::FadeUnmatched;
  for (bool appearing : {false, true}) {
    bool rejected = false;
    try { TInbetween fadeFill(appearing ? empty : closed,
                             appearing ? closed : empty, {}, options); }
    catch (const TException &e) {
      rejected = e.getMessage().find(L"painted regions") != std::wstring::npos;
    }
    require(rejected && closed->getRegion(0)->getStyle() == 1 &&
                closed->getStroke(0)->getOpacity() == 1.,
            "Fade accepted an unmatched painted boundary or mutated its source");
  }
  auto matched = closed->clone();
  TInbetween matchedFill(closed, matched, {}, options);
  auto middle = matchedFill.tween(.5);
  require(middle->getRegionCount() > 0 && middle->getRegion(0)->getStyle() == 1,
          "Fade rejected or removed a matched fill");
  closed->getRegion(0)->setStyle(0);
  TInbetween unpainted(closed, empty, {}, options);
  require(unpainted.tween(.5)->getStroke(0)->getOpacity() == .5,
          "Fade rejected an unpainted closed stroke");
}
void nestedFadeBoundaries() {
  auto rings = drawing({});
  for (double size : {30.,20.,10.}) {
    auto *ring = new TStroke(std::vector<TThickPoint>{
        {-size,-size,1}, {0,-size,1}, {size,-size,1}, {size,0,1},
        {size,size,1}, {0,size,1}, {-size,size,1}, {-size,0,1}, {-size,-size,1}});
    ring->setStyle(1); ring->setSelfLoop(); rings->addStroke(ring);
  }
  rings->findRegions();
  require(rings->getRegionCount() == 1 &&
              rings->getRegion(0)->getSubregionCount() == 1 &&
              rings->getRegion(0)->getSubregion(0)->getSubregionCount() == 1,
          "nested Fade fixture did not create nested native regions");
  auto *outer = rings->getRegion(0);
  auto *middle = outer->getSubregion(0);
  auto *inner = middle->getSubregion(0);
  auto withoutInner = drawing({});
  withoutInner->addStroke(new TStroke(*rings->getStroke(0)));
  withoutInner->addStroke(new TStroke(*rings->getStroke(1)));
  TInbetween::MatchOptions options; options.m_unmatchedPolicy = TInbetween::FadeUnmatched;
  auto check = [&](bool rejected) {
    const auto review = TInbetween::reviewCorrespondence(rings, withoutInner, {}, options);
    require(review.unmatchedFirst == std::vector<int>{2} &&
                TInbetween::fadeHasPaintedBoundaries(rings, withoutInner, review) == rejected,
            "Fade preflight missed a nested painted region or its hole boundary");
    bool threw = false;
    try { TInbetween tween(rings, withoutInner, {}, options); tween.tween(.5); }
    catch (const TException &) { threw = true; }
    require(threw == rejected, "Fade constructor disagreed with native preflight");
  };
  inner->setStyle(1); check(true);
  inner->setStyle(0); middle->setStyle(1); check(true); // Inner ring bounds a painted hole.
  middle->setStyle(0); outer->setStyle(1); check(false); // Inner ring is inside an unpainted hole.
  require(outer->getStyle() == 1 && middle->getStyle() == 0 && inner->getStyle() == 0,
          "Fade preflight mutated source region colors");
}
void fadeBirthDeath(const QTemporaryDir &dir) {
  require(TInbetween::OmitUnmatched == 0 && TInbetween::HoldUnmatched == 1 &&
              TInbetween::CollapseUnmatched == 2 && TInbetween::FadeUnmatched == 3,
          "saved unmatched policy values changed");
  auto first = drawing({0,30}), last = drawing({0});
  const StrokeOpacity::Profile profile{{0.,.2},{.5,.8},{1.,1.}};
  auto *source = first->getStroke(1);
  source->setOpacity(.6);
  require(source->setOpacityProfile(profile), "fade fixture envelope invalid");
  first->group(1,1);
  TInbetween::MatchOptions options; options.m_unmatchedPolicy = TInbetween::FadeUnmatched;
  TInbetween disappear(first,last,{},options), appear(last,first,{},options);
  auto verify = [&](const TVectorImageP &result, double expected) {
    require(result->getStrokeCount() == 2 && result->getGroupDepth(1) == 1,
            "Fade lost an unmatched stroke or its group");
    const auto *stroke = result->getStroke(1);
    require(std::abs(stroke->getOpacity()-expected) < 1.e-12 &&
                stroke->getOpacityProfile() == profile && stroke->getStyle() == source->getStyle() &&
                stroke->isSelfLoop() == source->isSelfLoop() &&
                stroke->getControlPointCount() == source->getControlPointCount(),
            "Fade changed source appearance beyond the visibility multiplier");
    for (int i = 0; i < stroke->getControlPointCount(); ++i)
      require(stroke->getControlPoint(i) == source->getControlPoint(i),
              "Fade changed unmatched geometry or width");
    require(result->getStroke(0)->getOpacity() == 1., "Fade changed the matched stroke opacity");
  };
  for (double t : {.1,.25,.5,.9}) {
    verify(disappear.tween(t),.6*(1.-t));
    verify(appear.tween(t),.6*t);
  }
  verify(disappear.tween(.5),.3); verify(disappear.tween(.5),.3);
  require(first->getStroke(1)->getOpacity() == .6 && source->getOpacityProfile() == profile &&
              disappear.tween(0)->getStroke(1)->getOpacity() == .6 &&
              disappear.tween(1)->getStrokeCount() == 1 && appear.tween(0)->getStrokeCount() == 1 &&
              appear.tween(1)->getStroke(1)->getOpacity() == .6,
          "Fade mutated its sources or changed endpoint drawings");
  const TFilePath path(dir.filePath("faded-strokes.pli"));
  {
    TLevelWriterP writer(path);
    writer->getFrameWriter(TFrameId(1))->save(disappear.tween(.5));
  }
  TLevelReaderP reader(path); reader->loadInfo();
  verify(reader->getFrameReader(TFrameId(1))->load(),.3);
}
void opacityEnvelopes() {
  auto first = drawing({0}), last = drawing({20});
  const StrokeOpacity::Profile a{{0.,0.},{.5,.2},{.5,.8},{1.,1.}};
  const StrokeOpacity::Profile b{{0.,1.},{.25,.5},{1.,0.}};
  first->getStroke(0)->setOpacity(.2); last->getStroke(0)->setOpacity(.8);
  require(first->getStroke(0)->setOpacityProfile(a) &&
          last->getStroke(0)->setOpacityProfile(b), "opacity fixture profile rejected");
  TInbetween tween(first, last);
  auto middle = tween.tween(.5);
  require(std::abs(middle->getStroke(0)->getOpacity() - .5) < 1e-12,
          "morph did not interpolate overall opacity");
  const auto &profile = middle->getStroke(0)->getOpacityProfile();
  require(profile == StrokeOpacity::blend(a,b,.5), "morph did not preserve union/step knots");
  require(StrokeOpacity::evaluateLeft(profile,.5) < StrokeOpacity::evaluate(profile,.5),
          "morph flattened an opacity discontinuity");
  require(first->getStroke(0)->getOpacityProfile() == a &&
          last->getStroke(0)->getOpacityProfile() == b, "morph mutated opacity endpoints");
  first->getStroke(0)->setOpacityProfile({{0.,.1},{0.,.9},{1.,1.}});
  last->getStroke(0)->setOpacityProfile({});
  TInbetween endpointStep(first,last);
  const auto endpointProfile = endpointStep.tween(.5)->getStroke(0)->getOpacityProfile();
  require(endpointProfile.size() == 3 && endpointProfile[0].x == 0. &&
              endpointProfile[1].x == 0. &&
              std::abs(endpointProfile[0].y - .55) < 1e-12 &&
              std::abs(endpointProfile[1].y - .95) < 1e-12,
          "morph flattened a step at the stroke start");
}
void persistence(const QTemporaryDir &dir) {
  auto first = drawing({0, 30});
  auto last = drawing({50});
  auto anchor = MorphHints::anchor(first, 1);
  QJsonObject record{{"kind", "pair"}, {"firstFrame", MorphHints::frame(TFrameId(1, "A"))},
      {"lastFrame", MorphHints::frame(TFrameId(9))},
      {"first", anchor}, {"last", MorphHints::anchor(last, 0)}};
  std::string text = MorphHints::encode({record});
  TXshSimpleLevelP level = new TXshSimpleLevel(L"drawings");
  level->setType(PLI_XSHLEVEL);
  level->setPath(TFilePath("+drawings/test.pli"), true);
  level->setMorphHints(text);
  const TFilePath sceneData(dir.filePath("level-metadata.xml"));
  {
    TOStream out(sceneData);
    out.openChild("level"); level->saveData(out); out.closeChild();
  }
  TXshSimpleLevelP reopened = new TXshSimpleLevel;
  {
    TIStream in(sceneData);
    std::string tag;
    require(in.matchTag(tag) && tag == "level", "scene metadata tag missing");
    reopened->loadData(in); in.matchEndTag();
  }
  require(reopened->getMorphHints() == text, "real level serializer lost saved hints");
  reopened->clearFrames();
  require(reopened->getMorphHints() == text, "cache reload discarded scene morph hints");
  QJsonArray records;
  require(MorphHints::decode(reopened->getMorphHints(), records) && records.size() == 1,
          "saved hint metadata not readable");
  require(MorphHints::frame(records[0].toObject()["firstFrame"].toObject()) == TFrameId(1, "A"),
          "frame suffix lost");
  // Exercise the real PLI writer/reader: runtime stroke IDs are not file identities.
  const TFilePath pli(dir.filePath("drawings.pli"));
  {
    TLevelWriterP writer(pli);
    writer->setPalette(first->getPalette());
    writer->getFrameWriter(TFrameId(1))->save(first);
  }
  TVectorImageP loaded;
  {
    TLevelReaderP reader(pli);
    reader->loadInfo(); loaded = reader->getFrameReader(TFrameId(1))->load();
  }
  require(MorphHints::resolve(anchor, loaded) == 1, "hint did not survive real PLI reload");
  auto changed = first->clone();
  auto cp = changed->getStroke(1)->getControlPoint(1);
  cp.y += 4; changed->getStroke(1)->setControlPoint(1, cp);
  require(MorphHints::resolve(anchor, changed) == -1, "stale hint rebound to edited stroke");
  auto duplicate = first->clone(); duplicate->addStroke(new TStroke(*first->getStroke(1)));
  require(MorphHints::resolve(anchor, duplicate) == -1, "ambiguous hint silently chose a stroke");
  std::map<TFrameId,TFrameId> table{{TFrameId(1,"A"), TFrameId(20)}, {TFrameId(9),TFrameId(30)}};
  require(MorphHints::decode(MorphHints::renumber(text, table), records) &&
          MorphHints::frame(records[0].toObject()["firstFrame"].toObject()) == TFrameId(20),
          "renumber did not follow drawings");
  require(!MorphHints::decode("{broken", records), "damaged metadata accepted");
  require(!MorphHints::decode("{\"version\":2,\"records\":[]}", records), "future version misread as v1");
  require(!MorphHints::decode("{\"version\":1.5,\"records\":[]}", records), "fractional hint version accepted");
  auto corrupt = record;
  corrupt["firstFrame"] = QJsonObject{{"number", 1.5}, {"letter", "A"}};
  require(!MorphHints::decode(MorphHints::encode({corrupt}), records), "fractional drawing identity accepted");
  corrupt = record;
  auto malformedAnchor = anchor; malformedAnchor.remove("style"); corrupt["first"] = malformedAnchor;
  require(!MorphHints::decode(MorphHints::encode({corrupt}), records), "incomplete stroke identity accepted");
}
}
int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "temporary directory unavailable");
    const QString stuff = dir.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default"), "isolated morph settings");
    TEnv::setStuffDir(TFilePath(stuff));
    TEnv::setApplicationFullName("morphworkflow_test");
    TThread::init(); initImageIo();
    require(QDir().mkpath(dir.filePath("cache")), "isolated morph cache");
    TImageCache::instance()->setRootDir(TFilePath(dir.filePath("cache")));
    correspondence(); unmatchedAndFills(); nestedFadeBoundaries(); fadeBirthDeath(dir);
    opacityEnvelopes(); persistence(dir);
    std::cout << "Native morph workflow contracts passed\n";
    return 0;
  } catch (const TException &e) {
    std::cerr << QString::fromStdWString(e.getMessage()).toStdString() << '\n';
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
  return 1;
}
