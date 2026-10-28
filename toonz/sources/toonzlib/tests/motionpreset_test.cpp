// Regression test of the motion presets (toonz/motionpreset.h): saving and
// loading, the keys that applying a preset at a frame sets (relative to the
// value of the curve there, for the positions, the angles and the scales), the
// offset layers, capturing the keys of an object and applying them to another
// one, and the stock presets shipped in stuff/library/motionpresets, whose
// folder is the first argument of the program.

#include "toonz/motionpreset.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tstageobjecttree.h"

#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfilepath.h"
#include "tundo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

bool nearly(double a, double b) {
  return std::abs(a - b) <= 1e-4 * std::max(1.0, std::abs(b));
}

TFilePath toPath(const QString &path) { return TFilePath(path.toStdWString()); }

//------------------------------------------------------------------

typedef MotionPreset::Key Key;
typedef MotionPreset::Channel Channel;

// A hop: up 3 inches in 6 frames and down again, with horizontal tangents
// at the top
Channel hopChannel() {
  Channel channel("y", MotionPreset::Add);
  Key k0(0, 0, TDoubleKeyframe::SpeedInOut);
  k0.m_speedOut = TPointD(2, 0);
  Key k1(6, 3, TDoubleKeyframe::SpeedInOut);
  k1.m_speedIn  = TPointD(-2, 0);
  k1.m_speedOut = TPointD(2, 0);
  Key k2(12, 0, TDoubleKeyframe::Linear);
  k2.m_speedIn = TPointD(-2, 0);
  channel.m_keys.push_back(k0);
  channel.m_keys.push_back(k1);
  channel.m_keys.push_back(k2);
  return channel;
}

// The scale goes to 1.5 times its value and comes back
Channel pulseChannel(const std::string &name) {
  Channel channel(name, MotionPreset::Multiply);
  channel.m_keys.push_back(Key(0, 1.0));
  channel.m_keys.push_back(Key(6, 1.5));
  channel.m_keys.push_back(Key(12, 1.0));
  return channel;
}

Channel spinChannel() {
  Channel channel("rot", MotionPreset::Add);
  channel.m_keys.push_back(Key(0, 0.0));
  channel.m_keys.push_back(Key(24, 360.0));
  return channel;
}

MotionPreset makePreset() {
  MotionPreset preset;
  preset.setName("Hop \"big\" & high");
  preset.setDescription("Up, then down; with a quote \" and a backslash \\");
  preset.addChannel(hopChannel());
  preset.addChannel(pulseChannel("sc"));
  preset.addChannel(spinChannel());
  return preset;
}

//------------------------------------------------------------------

void requireSame(const MotionPreset &a, const MotionPreset &b,
                 const std::string &what) {
  require(a.getName() == b.getName(), what + ": name");
  require(a.getDescription() == b.getDescription(), what + ": description");
  require(a.getAnchor() == b.getAnchor(), what + ": anchor");
  require(a.getChannelCount() == b.getChannelCount(), what + ": channels");
  for (int i = 0; i < a.getChannelCount(); ++i) {
    const Channel &ca = a.getChannel(i);
    const Channel &cb = b.getChannel(i);
    require(ca.m_name == cb.m_name, what + ": channel name");
    require(ca.m_mode == cb.m_mode, what + ": channel mode " + ca.m_name);
    require(ca.m_keys.size() == cb.m_keys.size(),
            what + ": key count " + ca.m_name);
    for (size_t k = 0; k < ca.m_keys.size(); ++k) {
      const Key &ka           = ca.m_keys[k];
      const Key &kb           = cb.m_keys[k];
      const std::string where = what + ": " + ca.m_name + " key";
      require(nearly(ka.m_frame, kb.m_frame), where + " frame");
      require(nearly(ka.m_value, kb.m_value), where + " value");
      require(ka.m_type == kb.m_type, where + " type");
      require(nearly(ka.m_speedIn.x, kb.m_speedIn.x) &&
                  nearly(ka.m_speedIn.y, kb.m_speedIn.y),
              where + " speed in");
      require(nearly(ka.m_speedOut.x, kb.m_speedOut.x) &&
                  nearly(ka.m_speedOut.y, kb.m_speedOut.y),
              where + " speed out");
      require(ka.m_linkedHandles == kb.m_linkedHandles, where + " linked");
    }
  }
}

//------------------------------------------------------------------

void testRoundTrip(const QString &tempDir) {
  MotionPreset preset = makePreset();
  preset.setAnchor(MotionPreset::AnchorEnd);

  // A key with every field set
  MotionPreset full;
  full.setName("Full");
  Channel channel("x", MotionPreset::Add);
  Key a(0, 0, TDoubleKeyframe::EaseInOut);
  a.m_speedOut = TPointD(3, 0);
  Key b(8, -2.5, TDoubleKeyframe::SpeedInOut);
  b.m_speedIn       = TPointD(-3, 1.25);
  b.m_speedOut      = TPointD(2.5, -0.75);
  b.m_linkedHandles = true;
  Key c(20, 10, TDoubleKeyframe::Constant);
  channel.m_keys.push_back(a);
  channel.m_keys.push_back(b);
  channel.m_keys.push_back(c);
  full.addChannel(channel);

  const MotionPreset presets[] = {preset, full};
  for (int i = 0; i < 2; ++i) {
    const MotionPreset &original = presets[i];
    std::string error;
    require(original.isValid(&error), "round trip: valid: " + error);

    const TFilePath fp =
        toPath(QDir(tempDir).filePath(QString("sub/round%1.tnzmotion").arg(i)));
    require(original.save(fp, &error), "round trip: save: " + error);

    MotionPreset loaded;
    require(MotionPreset::load(fp, loaded, &error),
            "round trip: load: " + error);
    requireSame(original, loaded, "round trip");
  }

  // Missing and wrong files
  MotionPreset dummy;
  std::string error;
  require(
      !MotionPreset::load(toPath(QDir(tempDir).filePath("missing.tnzmotion")),
                          dummy, &error),
      "load: a missing file");
  QFile bad(QDir(tempDir).filePath("bad.tnzmotion"));
  require(bad.open(QIODevice::WriteOnly), "load: write the bad file");
  bad.write("<something>\n  1 2 3\n</something>\n");
  bad.close();
  require(!MotionPreset::load(toPath(bad.fileName()), dummy, &error),
          "load: a file of another kind");

  // A preset can't be saved if it can't be applied
  MotionPreset invalid;
  require(!invalid.save(toPath(QDir(tempDir).filePath("invalid.tnzmotion"))),
          "save: an empty preset");
}

//------------------------------------------------------------------

// The syntax the stock files are written in, with a tag of a later version
void testHandWritten(const QString &tempDir) {
  QFile file(QDir(tempDir).filePath("hand.tnzmotion"));
  require(file.open(QIODevice::WriteOnly), "hand: write the file");
  file.write(
      "<motionpreset>\n"
      "  <name>\n"
      "    \"Hand Made\"\n"
      "  </name>\n"
      "  <future>\n"
      "    1 2 3\n"
      "  </future>\n"
      "  <anchor>\n"
      "    end\n"
      "  </anchor>\n"
      "  <channel name=\"y\" mode=\"add\">\n"
      "    0 0 L 0 0 0 0 0\n"
      "    10 -5 L 0 0 0 0 0\n"
      "  </channel>\n"
      "  <channel name=\"sy\">\n"
      "    0 1 S 0 0 3 0 0\n"
      "    6 0.5 L -3 0 0 0 0\n"
      "  </channel>\n"
      "</motionpreset>\n");
  file.close();

  MotionPreset preset;
  std::string error;
  require(MotionPreset::load(toPath(file.fileName()), preset, &error),
          "hand: load: " + error);
  require(preset.getName() == "Hand Made", "hand: name");
  require(preset.getAnchor() == MotionPreset::AnchorEnd, "hand: anchor");
  require(preset.getChannelCount() == 2, "hand: channel count");
  require(preset.getChannel(0).m_keys.size() == 2, "hand: key count");
  require(nearly(preset.getChannel(0).m_keys[1].m_value, -5), "hand: value");
  // The mode of a scale is multiply by default
  require(preset.getChannel(1).m_mode == MotionPreset::Multiply, "hand: mode");
  require(preset.getChannel(1).m_keys[0].m_type == TDoubleKeyframe::SpeedInOut,
          "hand: type");
  require(nearly(preset.getChannel(1).m_keys[0].m_speedOut.x, 3),
          "hand: speed");
  require(preset.isValid(&error) && preset.isNormalized(), "hand: valid");
}

//------------------------------------------------------------------

void testApplyAdd() {
  MotionPreset preset;
  preset.addChannel(hopChannel());
  const Channel &hop = preset.getChannel(0);

  // A curve without keys: the keys are relative to its value
  {
    TDoubleParam y(5.0);
    std::string reason;
    require(preset.applyChannel(hop, &y, 10, 0, &reason), "add: " + reason);
    require(y.getKeyframeCount() == 3, "add: key count");
    require(y.keyframeIndexToFrame(0) == 10 &&
                y.keyframeIndexToFrame(1) == 16 &&
                y.keyframeIndexToFrame(2) == 22,
            "add: key frames");
    require(nearly(y.getKeyframe(0).m_value, 5) &&
                nearly(y.getKeyframe(1).m_value, 8) &&
                nearly(y.getKeyframe(2).m_value, 5),
            "add: key values");
    require(y.getKeyframe(0).m_type == TDoubleKeyframe::SpeedInOut &&
                y.getKeyframe(2).m_type == TDoubleKeyframe::Linear,
            "add: key types");
    require(nearly(y.getKeyframe(0).m_speedOut.x, 2) &&
                nearly(y.getKeyframe(1).m_speedIn.x, -2),
            "add: handles");
    require(nearly(y.getValue(0), 5) && nearly(y.getValue(40), 5),
            "add: held outside");
    const double mid = y.getValue(13);
    require(mid > 5 && mid < 8, "add: eased between the keys");
  }

  // A curve with keys: the preset starts from its value at the frame, replaces
  // the keys in its way and leaves the other ones
  {
    TDoubleParam y(0.0);
    TDoubleKeyframe k0(0, 1), k1(14, 100), k2(30, 7);
    for (TDoubleKeyframe *k : {&k0, &k1, &k2}) {
      k->m_type = TDoubleKeyframe::Linear;
      y.setKeyframe(*k);
    }
    const double start = y.getValue(10);
    require(nearly(start, 1 + 99.0 * 10 / 14), "add: the start value");

    std::string reason;
    require(preset.applyChannel(hop, &y, 10, 0, &reason), "add 2: " + reason);
    require(y.getKeyframeCount() == 5, "add 2: key count");
    require(y.keyframeIndexToFrame(0) == 0 && y.keyframeIndexToFrame(1) == 10 &&
                y.keyframeIndexToFrame(2) == 16 &&
                y.keyframeIndexToFrame(3) == 22 &&
                y.keyframeIndexToFrame(4) == 30,
            "add 2: key frames");
    require(nearly(y.getKeyframe(0).m_value, 1) &&
                nearly(y.getKeyframe(4).m_value, 7),
            "add 2: the other keys stay");
    require(nearly(y.getKeyframe(1).m_value, start) &&
                nearly(y.getKeyframe(2).m_value, start + 3) &&
                nearly(y.getKeyframe(3).m_value, start),
            "add 2: key values");
  }

  // Applying it twice at the same frame doesn't pile up
  {
    TDoubleParam y(2.0);
    std::string reason;
    require(preset.applyChannel(hop, &y, 4, 0, &reason), "twice: first");
    require(preset.applyChannel(hop, &y, 4, 0, &reason), "twice: second");
    require(y.getKeyframeCount() == 3, "twice: key count");
    require(nearly(y.getValue(10), 5), "twice: value");
  }

  // A channel that starts later than the preset
  {
    TDoubleParam y(0.0);
    MotionPreset shifted;
    Channel channel = hopChannel();
    for (Key &key : channel.m_keys) key.m_frame += 3;
    std::string reason;
    shifted.addChannel(channel);
    require(shifted.applyChannel(shifted.getChannel(0), &y, 0, 0, &reason),
            "offset: " + reason);
    require(y.keyframeIndexToFrame(0) == 3, "offset: first frame");

    // And it can't start before the first frame
    TDoubleParam z(0.0);
    require(!preset.applyChannel(hop, &z, -5, 0, &reason), "negative: applied");
    require(z.getKeyframeCount() == 0, "negative: curve changed");
  }
}

//------------------------------------------------------------------

void testApplyMultiply() {
  MotionPreset preset;
  preset.addChannel(pulseChannel("sx"));

  TDoubleParam sx(2.0);
  std::string reason;
  require(preset.applyChannel(preset.getChannel(0), &sx, 3, 0, &reason),
          "multiply: " + reason);
  require(sx.getKeyframeCount() == 3, "multiply: key count");
  require(nearly(sx.getKeyframe(0).m_value, 2) &&
              nearly(sx.getKeyframe(1).m_value, 3) &&
              nearly(sx.getKeyframe(2).m_value, 2),
          "multiply: values are ratios of the start");
  require(sx.keyframeIndexToFrame(1) == 9, "multiply: frames");

  // A ratio handle is a ratio too
  MotionPreset eased;
  Channel channel("sc", MotionPreset::Multiply);
  Key k0(0, 1, TDoubleKeyframe::SpeedInOut);
  k0.m_speedOut = TPointD(2, 0.5);
  Key k1(6, 2, TDoubleKeyframe::Linear);
  k1.m_speedIn = TPointD(-2, 0);
  channel.m_keys.push_back(k0);
  channel.m_keys.push_back(k1);
  eased.addChannel(channel);
  TDoubleParam sc(4.0);
  require(eased.applyChannel(eased.getChannel(0), &sc, 0, 0, &reason),
          "multiply: handles");
  require(nearly(sc.getKeyframe(0).m_speedOut.y, 2.0), "multiply: handle");
  require(nearly(sc.getKeyframe(1).m_value, 8), "multiply: end value");
}

//------------------------------------------------------------------

void testAnchorEnd() {
  MotionPreset preset;
  preset.setAnchor(MotionPreset::AnchorEnd);
  Channel channel("x", MotionPreset::Add);
  channel.m_keys.push_back(Key(0, 0.0));
  channel.m_keys.push_back(Key(12, 18.0));
  preset.addChannel(channel);
  // sc goes from 1 to 2: the start is half of the value
  Channel scale("sc", MotionPreset::Multiply);
  scale.m_keys.push_back(Key(0, 1.0));
  scale.m_keys.push_back(Key(10, 2.0));
  preset.addChannel(scale);

  TDoubleParam x(1.0);
  std::string reason;
  require(preset.applyChannel(preset.getChannel(0), &x, 4, 0, &reason),
          "end: " + reason);
  require(x.keyframeIndexToFrame(0) == 4 && x.keyframeIndexToFrame(1) == 16,
          "end: frames");
  require(nearly(x.getKeyframe(0).m_value, -17), "end: starts away");
  require(nearly(x.getKeyframe(1).m_value, 1), "end: ends at the value");

  TDoubleParam sc(3.0);
  require(preset.applyChannel(preset.getChannel(1), &sc, 0, 0, &reason),
          "end: scale");
  require(nearly(sc.getKeyframe(0).m_value, 1.5) &&
              nearly(sc.getKeyframe(1).m_value, 3),
          "end: scale values");
}

//------------------------------------------------------------------

void testSkips() {
  MotionPreset preset;
  preset.addChannel(hopChannel());
  const Channel &hop = preset.getChannel(0);
  std::string reason;

  // Cycled
  {
    TDoubleParam y(0.0);
    TDoubleKeyframe k0(0, 0), k1(10, 5);
    y.setKeyframe(k0);
    y.setKeyframe(k1);
    y.enableCycle(true);
    require(!preset.applyChannel(hop, &y, 20, 0, &reason), "cycle: applied");
    require(!reason.empty(), "cycle: no reason");
    require(y.getKeyframeCount() == 2, "cycle: curve changed");
  }

  // An expression segment where the keys go
  {
    TDoubleParam y(0.0);
    TDoubleKeyframe k0(5, 0), k1(30, 5);
    k0.m_type           = TDoubleKeyframe::Expression;
    k0.m_expressionText = "1";
    y.setKeyframe(k0);
    y.setKeyframe(k1);
    reason.clear();
    require(!preset.applyChannel(hop, &y, 10, 0, &reason),
            "expression: applied");
    require(!reason.empty(), "expression: no reason");
    require(y.getKeyframeCount() == 2 && y.keyframeIndexToFrame(0) == 5 &&
                y.keyframeIndexToFrame(1) == 30 &&
                y.getKeyframe(0).m_type == TDoubleKeyframe::Expression,
            "expression: curve changed");
  }

  // Invalid presets are not applied
  {
    MotionPreset bad;
    Channel channel("y", MotionPreset::Add);
    channel.m_keys.push_back(Key(0, 0.0));
    channel.m_keys.push_back(Key(0, 3.0));
    bad.addChannel(channel);
    TDoubleParam y(0.0);
    require(!bad.isValid(&reason), "invalid: same frame");
    require(!bad.applyChannel(bad.getChannel(0), &y, 0, 0, &reason),
            "invalid: applied");
    require(y.getKeyframeCount() == 0, "invalid: curve changed");

    MotionPreset unknown;
    Channel other("nope", MotionPreset::Add);
    other.m_keys.push_back(Key(0, 0.0));
    other.m_keys.push_back(Key(4, 1.0));
    unknown.addChannel(other);
    require(!unknown.isValid(&reason), "invalid: unknown channel");
  }
}

//------------------------------------------------------------------

void testCapture() {
  // y: a speed in/out segment and a linear one, and keys out of the range
  TDoubleParam y(0.0);
  {
    TDoubleKeyframe out0(0, 9);
    out0.m_type = TDoubleKeyframe::Linear;
    TDoubleKeyframe k0(3, 2);
    k0.m_type          = TDoubleKeyframe::SpeedInOut;
    k0.m_speedIn       = TPointD(-5, 5);
    k0.m_speedOut      = TPointD(2, 1);
    k0.m_linkedHandles = false;
    TDoubleKeyframe k1(9, 8);
    k1.m_type          = TDoubleKeyframe::Linear;
    k1.m_speedIn       = TPointD(-2, -1);
    k1.m_speedOut      = TPointD(2, 0);
    k1.m_linkedHandles = false;
    TDoubleKeyframe k2(15, 2);
    k2.m_type = TDoubleKeyframe::SpeedInOut;
    TDoubleKeyframe out1(20, 4);
    out1.m_type = TDoubleKeyframe::Linear;
    y.setKeyframe(out0);
    y.setKeyframe(k0);
    y.setKeyframe(k1);
    y.setKeyframe(k2);
    y.setKeyframe(out1);
  }

  Channel channel;
  std::string reason;
  require(MotionPreset::captureChannel(&y, "y", 3, 15, 3, channel, &reason) ==
              MotionPreset::CaptureDone,
          "capture: " + reason);
  require(channel.m_mode == MotionPreset::Add && channel.m_keys.size() == 3,
          "capture: keys");
  require(nearly(channel.m_keys[0].m_frame, 0) &&
              nearly(channel.m_keys[1].m_frame, 6) &&
              nearly(channel.m_keys[2].m_frame, 12),
          "capture: frames relative to the start");
  require(nearly(channel.m_keys[0].m_value, 0) &&
              nearly(channel.m_keys[1].m_value, 6) &&
              nearly(channel.m_keys[2].m_value, 0),
          "capture: values relative to the first");
  require(channel.m_keys[0].m_type == TDoubleKeyframe::SpeedInOut &&
              channel.m_keys[2].m_type == TDoubleKeyframe::Linear,
          "capture: types");
  require(
      channel.m_keys[0].m_speedIn.x == 0 && channel.m_keys[0].m_speedIn.y == 0,
      "capture: the first key has no incoming handle");
  require(nearly(channel.m_keys[0].m_speedOut.x, 2) &&
              nearly(channel.m_keys[0].m_speedOut.y, 1),
          "capture: speed out");
  require(nearly(channel.m_keys[1].m_speedIn.x, -2) &&
              nearly(channel.m_keys[1].m_speedIn.y, -1),
          "capture: speed in");

  // Nothing to store
  require(MotionPreset::captureChannel(&y, "y", 4, 8, 4, channel, &reason) ==
              MotionPreset::CaptureNothing,
          "capture: a range without keys");

  // Apply the capture to a curve elsewhere: the same deltas
  MotionPreset preset;
  preset.addChannel(channel);
  TDoubleParam other(1.0);
  require(preset.applyChannel(preset.getChannel(0), &other, 20, 0, &reason),
          "capture: apply " + reason);
  for (int f = 0; f <= 12; ++f)
    require(nearly(y.getValue(3 + f) - 2.0, other.getValue(20 + f) - 1.0),
            "capture: the curves differ");

  // A scale is stored as ratios
  TDoubleParam sc(1.0);
  TDoubleKeyframe s0(0, 2.0), s1(10, 3.0);
  sc.setKeyframe(s0);
  sc.setKeyframe(s1);
  Channel scale;
  require(MotionPreset::captureChannel(&sc, "sc", 0, 10, 0, scale, &reason) ==
              MotionPreset::CaptureDone,
          "capture: scale");
  require(scale.m_mode == MotionPreset::Multiply &&
              nearly(scale.m_keys[0].m_value, 1) &&
              nearly(scale.m_keys[1].m_value, 1.5),
          "capture: scale ratios");
  MotionPreset scalePreset;
  scalePreset.addChannel(scale);
  TDoubleParam target(4.0);
  require(scalePreset.applyChannel(scalePreset.getChannel(0), &target, 0, 0,
                                   &reason),
          "capture: apply scale");
  require(nearly(target.getKeyframe(1).m_value, 6), "capture: scale applied");

  // An expression is not stored
  TDoubleParam expr(0.0);
  TDoubleKeyframe e0(0, 0), e1(10, 1);
  e0.m_type           = TDoubleKeyframe::Expression;
  e0.m_expressionText = "1";
  expr.setKeyframe(e0);
  expr.setKeyframe(e1);
  require(MotionPreset::captureChannel(&expr, "x", 0, 10, 0, channel,
                                       &reason) == MotionPreset::CaptureSkipped,
          "capture: expression");
}

//------------------------------------------------------------------

void testStageObject() {
  TStageObjectTree tree;

  // The base curves
  {
    TStageObject *obj = tree.getStageObject(TStageObjectId::PegbarId(1));
    obj->getParam(TStageObject::T_Y)->setDefaultValue(2.0);
    obj->getParam(TStageObject::T_Scale)->setDefaultValue(0.5);

    MotionPreset preset                    = makePreset();
    const MotionPreset::ApplyResult result = preset.apply(obj, 10, -1, false);
    require(result.m_channelCount == 3 && result.m_skipped.empty(),
            "object: channels applied");
    require(result.m_keyCount == 8, "object: keys set");

    TDoubleParam *y = obj->getParam(TStageObject::T_Y);
    require(y->getKeyframeCount() == 3 && nearly(y->getValue(16), 5),
            "object: y");
    TDoubleParam *sc = obj->getParam(TStageObject::T_Scale);
    require(nearly(sc->getValue(16), 0.75) && nearly(sc->getValue(10), 0.5),
            "object: scale");
    TDoubleParam *rot = obj->getParam(TStageObject::T_Angle);
    require(nearly(rot->getValue(34), 360) && nearly(rot->getValue(10), 0),
            "object: angle");
    require(obj->getParam(TStageObject::T_X)->getKeyframeCount() == 0,
            "object: x untouched");
  }

  // One undo for the whole apply
  {
    TStageObject *obj   = tree.getStageObject(TStageObjectId::PegbarId(2));
    MotionPreset preset = makePreset();
    TUndoManager::manager()->reset();
    TUndoManager::manager()->beginBlock();
    const MotionPreset::ApplyResult result = preset.apply(obj, 5, -1, true);
    TUndoManager::manager()->endBlock();
    require(result.m_channelCount == 3, "undo: applied");

    TDoubleParam *y   = obj->getParam(TStageObject::T_Y);
    TDoubleParam *sc  = obj->getParam(TStageObject::T_Scale);
    TDoubleParam *rot = obj->getParam(TStageObject::T_Angle);
    require(y->getKeyframeCount() == 3, "undo: keys set");

    require(TUndoManager::manager()->undo(), "undo: nothing to undo");
    require(y->getKeyframeCount() == 0 && sc->getKeyframeCount() == 0 &&
                rot->getKeyframeCount() == 0,
            "undo: keys are still there");
    require(TUndoManager::manager()->redo(), "undo: nothing to redo");
    require(y->getKeyframeCount() == 3 && sc->getKeyframeCount() == 3 &&
                rot->getKeyframeCount() == 2 && nearly(y->getValue(11), 3),
            "undo: redo");
    TUndoManager::manager()->reset();
  }

  // An offset layer: the base curves are not touched, the layer holds
  // the differences, also for the scales
  {
    TStageObject *obj = tree.getStageObject(TStageObjectId::PegbarId(3));
    obj->getParam(TStageObject::T_Scale)->setDefaultValue(2.0);
    obj->insertOffsetLayer(0, obj->createOffsetLayer(L"Motion"));
    require(obj->getOffsetLayerCount() == 1, "layer: added");

    MotionPreset preset;
    preset.addChannel(hopChannel());
    preset.addChannel(pulseChannel("sc"));
    const MotionPreset::ApplyResult result = preset.apply(obj, 5, 0, false);
    require(result.m_channelCount == 2 && result.m_skipped.empty(),
            "layer: channels applied");
    require(!obj->getParam(TStageObject::T_Y)->hasKeyframes() &&
                !obj->getParam(TStageObject::T_Scale)->hasKeyframes(),
            "layer: base curves changed");

    TDoubleParam *y = obj->getOffsetParam(0, TStageObject::T_Y);
    require(y->getKeyframeCount() == 3 && nearly(y->getValue(11), 3) &&
                nearly(y->getValue(5), 0),
            "layer: y offsets");
    // 2 * (1.5 - 1)
    TDoubleParam *sc = obj->getOffsetParam(0, TStageObject::T_Scale);
    require(nearly(sc->getValue(11), 1.0) && nearly(sc->getValue(5), 0),
            "layer: scale offsets");
    require(nearly(obj->getOffsetValue(TStageObject::T_Scale, 11), 1.0),
            "layer: effective offset");
    require(preset.apply(obj, 5, 1, false).m_channelCount == 0,
            "layer: a layer that does not exist");
  }

  // Capture the keys of an object, apply them to another one
  {
    TStageObject *source = tree.getStageObject(TStageObjectId::PegbarId(4));
    MotionPreset hop     = makePreset();
    hop.apply(source, 3, -1, false);
    // x has a single key in the range, and others out of it
    TDoubleParam *sourceX = source->getParam(TStageObject::T_X);
    sourceX->setKeyframe(TDoubleKeyframe(0, 0));
    sourceX->setKeyframe(TDoubleKeyframe(10, 5));
    sourceX->setKeyframe(TDoubleKeyframe(50, 100));

    MotionPreset captured;
    std::vector<std::string> skipped;
    require(MotionPreset::captureFromKeys(source, 3, 27, captured, &skipped),
            "capture object: nothing captured");
    require(skipped.empty(), "capture object: skipped");
    require(captured.isNormalized(), "capture object: not normalized");
    require(captured.findChannel("y") && captured.findChannel("sc") &&
                captured.findChannel("rot"),
            "capture object: channels");
    require(!captured.findChannel("x"), "capture object: x");
    require(captured.findChannel("sc")->m_mode == MotionPreset::Multiply,
            "capture object: scale mode");

    TStageObject *target = tree.getStageObject(TStageObjectId::PegbarId(5));
    target->getParam(TStageObject::T_Y)->setDefaultValue(-1.0);
    require(captured.apply(target, 20, -1, false).m_channelCount == 3,
            "capture object: apply");
    for (int f = 0; f <= 24; ++f) {
      const double a = source->getParam(TStageObject::T_Y)->getValue(3 + f);
      const double b = target->getParam(TStageObject::T_Y)->getValue(20 + f);
      require(nearly(a - 0.0, b + 1.0), "capture object: y differs");
      const double sa =
          source->getParam(TStageObject::T_Scale)->getValue(3 + f);
      const double sb =
          target->getParam(TStageObject::T_Scale)->getValue(20 + f);
      require(nearly(sa, sb), "capture object: scale differs");
    }
  }
}

//------------------------------------------------------------------

void testStock(const QString &stockDir, const QString &tempDir) {
  QDir dir(stockDir);
  require(dir.exists(), "stock: no folder " + stockDir.toStdString());
  const QStringList names =
      dir.entryList(QStringList() << "*.tnzmotion", QDir::Files, QDir::Name);
  require(names.size() >= 8, "stock: fewer than 8 presets");

  std::vector<QString> presetNames;
  for (const QString &fileName : names) {
    const std::string what = fileName.toStdString();
    MotionPreset preset;
    std::string error;
    require(MotionPreset::load(toPath(dir.filePath(fileName)), preset, &error),
            what + ": load: " + error);
    require(preset.isValid(&error), what + ": valid: " + error);
    require(preset.isNormalized(), what + ": first key not at 0");
    require(!preset.getName().isEmpty(), what + ": no name");
    require(!preset.getDescription().isEmpty(), what + ": no description");
    require(std::find(presetNames.begin(), presetNames.end(),
                      preset.getName()) == presetNames.end(),
            what + ": name used twice");
    presetNames.push_back(preset.getName());

    const double duration = preset.getDuration();
    require(duration >= 8 && duration <= 40, what + ": duration");

    bool animated = false;
    for (const Channel &channel : preset.getChannels()) {
      if (MotionPreset::isAnimated(channel)) animated = true;
      const double neutral = channel.m_mode == MotionPreset::Multiply ? 1 : 0;
      require(nearly(channel.m_keys.front().m_value, neutral),
              what + ": first key of " + channel.m_name);

      // Applied to a curve: the first key (or the last one, if the preset ends
      // where the object is) has its value
      const double start = channel.m_mode == MotionPreset::Multiply ? 2.0 : 1.5;
      TDoubleParam param(start);
      std::string reason;
      require(preset.applyChannel(channel, &param, 7, 0, &reason),
              what + ": apply " + channel.m_name + ": " + reason);
      require(param.getKeyframeCount() == (int)channel.m_keys.size(),
              what + ": key count of " + channel.m_name);
      const int anchored = preset.getAnchor() == MotionPreset::AnchorEnd
                               ? param.getKeyframeCount() - 1
                               : 0;
      require(nearly(param.getKeyframe(anchored).m_value, start),
              what + ": anchor value of " + channel.m_name);
      require(
          param.keyframeIndexToFrame(0) == 7 + channel.m_keys.front().m_frame,
          what + ": first frame of " + channel.m_name);
      for (int f = 0; f < 60; ++f)
        require(std::isfinite(param.getValue(f)),
                what + ": value of " + channel.m_name);
    }
    require(animated, what + ": no animated channel");

    // Saving and loading it again changes nothing
    MotionPreset again;
    const TFilePath fp = toPath(QDir(tempDir).filePath("stock_" + fileName));
    require(preset.save(fp, &error), what + ": save: " + error);
    require(MotionPreset::load(fp, again, &error), what + ": reload: " + error);
    requireSame(preset, again, what + " reloaded");
  }
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  if (argc < 2) {
    std::cerr << "usage: motionpreset_test <stuff/library/motionpresets>"
              << std::endl;
    return 1;
  }

  try {
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary folder");
    testRoundTrip(dir.path());
    testHandWritten(dir.path());
    testApplyAdd();
    testApplyMultiply();
    testAnchorEnd();
    testSkips();
    testCapture();
    testStageObject();
    testStock(QString::fromLocal8Bit(argv[1]), dir.path());
  } catch (const std::exception &e) {
    std::cerr << "motionpreset_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "motionpreset_test passed" << std::endl;
  return 0;
}
