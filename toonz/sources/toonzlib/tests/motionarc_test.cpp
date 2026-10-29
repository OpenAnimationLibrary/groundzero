// Regression test of the motion arcs (toonz/motionarc.h, doc/motion_arcs.md):
// the math on the samples (range window, spacing, speed, labels, colour ramp,
// spacing chart), and the sampling of a scene in memory: keys of the object
// and of its parents, the hooks of the drawings, the camera perspective and
// the placement cache put back to the current frame.

#include "toonz/motionarc.h"

#include "toonz/hook.h"
#include "toonz/preferences.h"
#include "toonz/stage.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/tstageobject.h"
#include "tconvert.h"
#include "tdoubleparam.h"
#include "tenv.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

bool approx(double a, double b, double eps = 1e-9) {
  return std::abs(a - b) < eps;
}

bool approx(const TPointD &a, const TPointD &b, double eps = 1e-9) {
  return approx(a.x, b.x, eps) && approx(a.y, b.y, eps);
}

MotionArc::Sample at(int frame, double x, double y, bool key = false) {
  MotionArc::Sample s;
  s.frame = frame;
  s.pos = s.viewPos = TPointD(x, y);
  s.key             = key;
  return s;
}

//------------------------------------------------------------------
// The math

void testClampRange() {
  int r0 = -5, r1 = 50;
  require(MotionArc::clampRange(r0, r1, 20, 3, 1000) && r0 == 0 && r1 == 19,
          "clamp: not brought into the xsheet");
  r0 = 30, r1 = 40;
  require(!MotionArc::clampRange(r0, r1, 20, 3, 1000),
          "clamp: a range after the xsheet is not empty");
  // A window of 10 frames around frame 50 of [0, 99]
  r0 = 0, r1 = 99;
  require(MotionArc::clampRange(r0, r1, 100, 50, 10) && r0 == 45 && r1 == 54,
          "clamp: wrong window around the current frame");
  // Near the end the window stays inside the range
  r0 = 0, r1 = 99;
  require(MotionArc::clampRange(r0, r1, 100, 98, 10) && r0 == 90 && r1 == 99,
          "clamp: wrong window at the end");
  r0 = 0, r1 = 99;
  require(MotionArc::clampRange(r0, r1, 100, 1, 10) && r0 == 0 && r1 == 9,
          "clamp: wrong window at the start");
}

void testSpacing() {
  // An ease out: 0, 4, 7, 9, 10 along x
  std::vector<MotionArc::Sample> s = {at(0, 0, 0, true), at(1, 4, 0),
                                      at(2, 7, 0), at(3, 9, 0),
                                      at(4, 10, 0, true)};
  const std::vector<double> d      = MotionArc::spacing(s);
  require(d.size() == 5 && d[0] == 0 && approx(d[1], 4) && approx(d[2], 3) &&
              approx(d[3], 2) && approx(d[4], 1),
          "spacing: wrong distances");
  require(approx(MotionArc::length(s), 10), "spacing: wrong length");
  const std::vector<double> v = MotionArc::speeds(s);
  require(approx(v[0], 4) && approx(v[1], 3.5) && approx(v[2], 2.5) &&
              approx(v[3], 1.5) && approx(v[4], 1),
          "speeds: not the mean of the spacing either side");
  const std::vector<double> n = MotionArc::normalized(v);
  require(approx(n[0], 1) && approx(n[4], 0.25), "normalized: wrong values");
  require(MotionArc::normalized({0, 0, 0}) == std::vector<double>({0, 0, 0}),
          "normalized: a hold is not all 0");
  // A diagonal: the distance, not a coordinate
  std::vector<MotionArc::Sample> diagonal = {at(0, 0, 0), at(1, 3, 4)};
  require(approx(MotionArc::spacing(diagonal)[1], 5),
          "spacing: not the euclidean distance");
  require(MotionArc::speeds({}).empty() && MotionArc::length({}) == 0,
          "spacing: no samples");
  require(MotionArc::speeds({at(3, 1, 1)})[0] == 0,
          "speeds: a single sample is not still");
}

void testLabels() {
  // Every 4 frames of the xsheet's numbering (frame 3 is shown as 4)
  require(MotionArc::isLabelled(3, false, 4) &&
              MotionArc::isLabelled(7, false, 4) &&
              !MotionArc::isLabelled(4, false, 4) &&
              !MotionArc::isLabelled(0, false, 4),
          "labels: not every 4 frames");
  require(MotionArc::isLabelled(5, true, 4), "labels: a key is not labelled");
  require(!MotionArc::isLabelled(3, true, 0), "labels: labelled when off");
  require(MotionArc::isLabelled(0, false, 1), "labels: not every frame");
}

void testRamp() {
  int r, g, b;
  MotionArc::rampColor(0, r, g, b);
  require(b > r && b > g, "ramp: slow is not blue");
  MotionArc::rampColor(0.5, r, g, b);
  require(g > r && g > b, "ramp: the middle is not green");
  MotionArc::rampColor(1, r, g, b);
  require(r > g && r > b, "ramp: fast is not red");
  int r2, g2, b2;
  MotionArc::rampColor(7, r2, g2, b2);
  require(r2 == r && g2 == g && b2 == b, "ramp: not clamped above 1");
}

void testChart() {
  // Even spacing: ticks at 0, 1/4, 1/2, 3/4, 1
  std::vector<MotionArc::Sample> even;
  for (int f = 0; f <= 4; ++f)
    even.push_back(at(f, 2.0 * f, 0, f == 0 || f == 4));
  std::vector<MotionArc::Tick> ticks = MotionArc::chart(even, 0, 4);
  require(ticks.size() == 5, "chart: not one tick per frame");
  for (int i = 0; i <= 4; ++i)
    require(approx(ticks[i].t, i / 4.0), "chart: even spacing not even");
  require(ticks[0].key && ticks[4].key && !ticks[2].key,
          "chart: the keys are not marked");
  require(approx(ticks[2].spacing, 2), "chart: wrong spacing of a tick");

  // An ease out, from frame 1 to 3 only: 4 -> 7 -> 9, length 5
  std::vector<MotionArc::Sample> ease = {at(0, 0, 0), at(1, 4, 0), at(2, 7, 0),
                                         at(3, 9, 0), at(4, 10, 0)};
  ticks = MotionArc::chart(ease, 3, 1);  // either order
  require(ticks.size() == 3 && ticks[0].frame == 1 && ticks[2].frame == 3,
          "chart: wrong frames");
  require(approx(ticks[0].t, 0) && approx(ticks[1].t, 0.6) && ticks[2].t == 1.0,
          "chart: wrong ticks of an ease out");
  require(ticks[0].spacing == 0, "chart: the first tick has spacing");

  // A hold: every tick at 0
  std::vector<MotionArc::Sample> hold = {at(0, 5, 5), at(1, 5, 5), at(2, 5, 5)};
  for (const MotionArc::Tick &t : MotionArc::chart(hold, 0, 2))
    require(t.t == 0, "chart: a hold is not at 0");
  require(MotionArc::chart(hold, 7, 9).empty(), "chart: frames out of range");
}

void testKeysAround() {
  std::vector<MotionArc::Sample> s;
  for (int f = 0; f <= 20; ++f)
    s.push_back(at(f, f, 0, f == 2 || f == 10 || f == 18));
  int f0, f1;
  require(MotionArc::keysAround(s, 5, f0, f1) && f0 == 2 && f1 == 10,
          "keys around: not the keys either side");
  require(MotionArc::keysAround(s, 10, f0, f1) && f0 == 10 && f1 == 18,
          "keys around: on a key, not the next segment");
  require(MotionArc::keysAround(s, 0, f0, f1) && f0 == 0 && f1 == 2,
          "keys around: before the first key");
  require(MotionArc::keysAround(s, 19, f0, f1) && f0 == 18 && f1 == 20,
          "keys around: after the last key");
  require(!MotionArc::keysAround({}, 0, f0, f1), "keys around: no samples");
}

//------------------------------------------------------------------
// Sampling a scene

void key(TStageObject *obj, TStageObject::Channel channel, int frame,
         double value) {
  obj->getParam(channel)->setValue(frame, value);
}

void testSampling() {
  ToonzScene scene;
  TXsheet *xsh = scene.getXsheet();

  // Column 1 with a drawing on 0..19, parented to a pegbar moving on x from
  // frame 0 to 10; the column itself moves on y from frame 4 to 8
  TXshLevel *level = scene.createNewLevel(PLI_XSHLEVEL, L"A");
  require(level && level->getSimpleLevel(), "sampling: no level");
  for (int r = 0; r < 20; ++r) xsh->setCell(r, 0, TXshCell(level, TFrameId(1)));
  const TStageObjectId colId = TStageObjectId::ColumnId(0);
  const TStageObjectId pegId = TStageObjectId::PegbarId(0);
  TStageObject *column       = xsh->getStageObject(colId);
  TStageObject *pegbar       = xsh->getStageObject(pegId);
  column->setParent(pegId);
  key(pegbar, TStageObject::T_X, 0, 0);
  key(pegbar, TStageObject::T_X, 10, 10);
  key(column, TStageObject::T_Y, 4, 0);
  key(column, TStageObject::T_Y, 8, 2);

  // The range: the column's own keys for the Animate tool, its parent's too
  // for the arc
  int r0, r1;
  require(
      MotionArc::keyRange(xsh, colId, 0, 1000, r0, r1) && r0 == 4 && r1 == 8,
      "sampling: the key range is not the column's keys");
  MotionArc::Track center;
  require(MotionArc::arcRange(xsh, colId, center, 0, 1000, r0, r1) && r0 == 0 &&
              r1 == 10,
          "sampling: the arc range is not the keys of the chain");
  require(!MotionArc::arcRange(xsh, TStageObjectId::ColumnId(3), center, 0,
                               1000, r0, r1),
          "sampling: a still column has a range");

  // The placements before sampling, frame by frame on a fresh evaluation
  std::vector<TAffine> before;
  for (int f = 0; f <= 10; ++f) before.push_back(xsh->getPlacement(colId, f));
  const int current              = 6;
  const TAffine currentPlacement = xsh->getPlacement(colId, current);

  std::vector<MotionArc::Sample> samples = MotionArc::sample(
      xsh, colId, center, 0, 10, MotionArc::Camera(), current);
  require(samples.size() == 11, "sampling: not one sample per frame");
  for (int f = 0; f <= 10; ++f) {
    const MotionArc::Sample &s = samples[f];
    require(s.frame == f, "sampling: wrong frame");
    // The centre is the origin of the column: x from the pegbar (inches to
    // stage units), y from the column
    const double y = f <= 4 ? 0.0 : f >= 8 ? 2.0 : 0.5 * (f - 4);
    require(approx(s.pos, TPointD(f * Stage::inch, y * Stage::inch), 1e-6),
            "sampling: wrong position");
    require(approx(s.pos, before[f] * (Stage::inch * xsh->getCenter(colId, f)),
                   1e-9),
            "sampling: not the Animate tool's centre");
    // At Z 0 with the camera at 0 the view is the world
    require(approx(s.viewPos, s.pos, 1e-9), "sampling: the view moved");
    const bool isKey = f == 0 || f == 4 || f == 8 || f == 10;
    require(s.key == isKey, "sampling: wrong keys of the chain");
  }
  require(approx(MotionArc::worldPos(xsh, colId, center, 3), samples[3].pos),
          "sampling: worldPos differs from the sample");

  // The cache put back: the column and its parent read at the current frame
  // as before, and a sibling under the same parent too. (The cache is
  // invalidated tree-wide when another frame is read, so the restore keeps
  // it warm for the current frame rather than changing what is read.)
  require(xsh->getPlacement(colId, current) == currentPlacement,
          "sampling: the current placement changed");
  const TStageObjectId siblingId = TStageObjectId::ColumnId(1);
  xsh->getStageObject(siblingId)->setParent(pegId);
  require(approx(xsh->getPlacement(siblingId, current) * TPointD(),
                 TPointD(current * Stage::inch, 0), 1e-6),
          "sampling: a sibling reads a wrong placement");

  // A point of the column's local space follows the column
  MotionArc::Track point;
  point.kind  = MotionArc::Track::Point;
  point.point = TPointD(10, -20);
  samples =
      MotionArc::sample(xsh, colId, point, 0, 10, MotionArc::Camera(), current);
  require(approx(samples[2].pos, before[2] * TPointD(10, -20), 1e-9),
          "sampling: the picked point does not follow the column");

  // The camera perspective: a column pushed back by Z is seen smaller,
  // towards the camera centre
  key(column, TStageObject::T_Z, 0, -1000);  // twice the distance: half size
  samples = MotionArc::sample(xsh, colId, center, 10, 10, MotionArc::Camera(),
                              current);
  require(approx(samples[0].viewPos, samples[0].pos * 0.5, 1e-6),
          "sampling: no perspective of the Z");
  column->getParam(TStageObject::T_Z)->deleteKeyframe(0);
  column->getParam(TStageObject::T_Z)->setDefaultValue(0);
}

void testHooks() {
  ToonzScene scene;
  TXsheet *xsh = scene.getXsheet();

  // Three drawings with hook 1 moving, on frames 0..5 (on twos), no keys
  TXshLevel *level    = scene.createNewLevel(PLI_XSHLEVEL, L"B");
  TXshSimpleLevel *sl = level->getSimpleLevel();
  require(sl, "hooks: no level");
  Hook *hook = sl->getHookSet()->addHook();
  require(hook, "hooks: no hook");
  const TPointD pos[3] = {TPointD(0, 0), TPointD(30, 10), TPointD(60, 0)};
  for (int i = 0; i < 3; ++i) hook->setAPos(TFrameId(i + 1), pos[i]);
  for (int r = 0; r < 6; ++r)
    xsh->setCell(r, 0, TXshCell(level, TFrameId(r / 2 + 1)));
  const TStageObjectId colId = TStageObjectId::ColumnId(0);

  MotionArc::Track center, hook1, hook2;
  hook1.kind = hook2.kind = MotionArc::Track::Hook;
  hook2.hook              = 2;
  int r0, r1;
  require(!MotionArc::arcRange(xsh, colId, center, 0, 1000, r0, r1),
          "hooks: the centre of a still column has a range");
  require(MotionArc::arcRange(xsh, colId, hook1, 0, 1000, r0, r1) && r0 == 0 &&
              r1 == 5,
          "hooks: the range is not the cells");

  std::vector<MotionArc::Sample> samples =
      MotionArc::sample(xsh, colId, hook1, r0, r1, MotionArc::Camera(), 0);
  require(samples.size() == 6, "hooks: not one sample per cell");
  for (int r = 0; r < 6; ++r)
    require(approx(samples[r].pos, pos[r / 2], 1e-6),
            "hooks: the hook is not followed across the drawings");
  // On twos: no motion within a drawing, then the step
  const std::vector<double> d = MotionArc::spacing(samples);
  require(d[1] == 0 && approx(d[2], std::sqrt(30.0 * 30 + 10 * 10), 1e-6),
          "hooks: wrong spacing on twos");

  // A missing hook gives the drawing's origin, not garbage
  require(approx(MotionArc::localPos(xsh, colId, hook2, 2), TPointD()),
          "hooks: a missing hook is not at the origin");
  // A hook of a pegbar is its centre
  const TStageObjectId pegId = TStageObjectId::PegbarId(0);
  require(approx(MotionArc::localPos(xsh, pegId, hook1, 0),
                 MotionArc::localPos(xsh, pegId, center, 0)),
          "hooks: a pegbar hook is not its centre");
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  try {
    testClampRange();
    testSpacing();
    testLabels();
    testRamp();
    testChart();
    testKeysAround();

    // The scenes need a stuff folder (for the sandbox project and the
    // preferences): use an empty one, so that the defaults are used
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    QString stuffPath = QDir(dir.path()).filePath("stuff");
    QDir().mkpath(QDir(stuffPath).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuffPath.toStdWString()));
    Preferences::instance();

    testSampling();
    testHooks();
  } catch (const TException &e) {
    std::cerr << "motionarc_test: " << ::to_string(e.getMessage()) << std::endl;
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "motionarc_test: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "ok: motion arcs" << std::endl;
  return 0;
}
