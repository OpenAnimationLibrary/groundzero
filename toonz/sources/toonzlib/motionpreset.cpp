#include "toonz/motionpreset.h"

// TnzLib includes
#include "toonz/tstageobject.h"

// TnzBase includes
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"

// TnzCore includes
#include "tconvert.h"
#include "texception.h"
#include "tstream.h"
#include "tundo.h"
#include "historytypes.h"

// Qt includes
#include <QDir>
#include <QObject>

// STD includes
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace {

//-----------------------------------------------------------------------------

struct ChannelInfo {
  const char *m_name;
  int m_channel;  // TStageObject::Channel
  MotionPreset::Mode m_mode;
};

const ChannelInfo channelTable[] = {
    {"x", TStageObject::T_X, MotionPreset::Add},
    {"y", TStageObject::T_Y, MotionPreset::Add},
    {"z", TStageObject::T_Z, MotionPreset::Add},
    {"so", TStageObject::T_SO, MotionPreset::Add},
    {"rot", TStageObject::T_Angle, MotionPreset::Add},
    {"rotx", TStageObject::T_RotX, MotionPreset::Add},
    {"roty", TStageObject::T_RotY, MotionPreset::Add},
    {"sx", TStageObject::T_ScaleX, MotionPreset::Multiply},
    {"sy", TStageObject::T_ScaleY, MotionPreset::Multiply},
    {"sc", TStageObject::T_Scale, MotionPreset::Multiply},
    {"shx", TStageObject::T_ShearX, MotionPreset::Add},
    {"shy", TStageObject::T_ShearY, MotionPreset::Add},
};

//-----------------------------------------------------------------------------

// The codes of the interpolations, those of the scene files
struct TypeCode {
  TDoubleKeyframe::Type m_type;
  const char *m_code;
};

const TypeCode typeCodes[] = {
    {TDoubleKeyframe::None, "n"},
    {TDoubleKeyframe::Constant, "C"},
    {TDoubleKeyframe::Linear, "L"},
    {TDoubleKeyframe::SpeedInOut, "S"},
    {TDoubleKeyframe::EaseInOut, "E"},
    {TDoubleKeyframe::EaseInOutPercentage, "Ep"},
    {TDoubleKeyframe::Exponential, "Exp"},
};

//-----------------------------------------------------------------------------

std::string typeToCode(TDoubleKeyframe::Type type) {
  for (const TypeCode &tc : typeCodes)
    if (tc.m_type == type) return tc.m_code;
  return "L";
}

bool codeToType(const std::string &code, TDoubleKeyframe::Type &type) {
  for (const TypeCode &tc : typeCodes)
    if (code == tc.m_code) {
      type = tc.m_type;
      return true;
    }
  return false;
}

//-----------------------------------------------------------------------------

// The interpolations that can't be stored in a preset, and that don't let a
// preset replace them: they depend on something else than the keys
bool isNonKeyType(TDoubleKeyframe::Type type) {
  return type == TDoubleKeyframe::Expression || type == TDoubleKeyframe::File ||
         type == TDoubleKeyframe::SimilarShape;
}

//! The interpolations that use the speed handles
bool usesHandles(TDoubleKeyframe::Type type) {
  return type == TDoubleKeyframe::SpeedInOut ||
         type == TDoubleKeyframe::EaseInOut ||
         type == TDoubleKeyframe::EaseInOutPercentage;
}

TDoubleKeyframe::Type presetType(TDoubleKeyframe::Type type) {
  return type == TDoubleKeyframe::None ? TDoubleKeyframe::Linear : type;
}

//-----------------------------------------------------------------------------

bool fail(std::string *reason, const std::string &text) {
  if (reason) *reason = text;
  return false;
}

//! Checks the keys of a channel
bool checkChannel(const MotionPreset::Channel &channel, bool anchorEnd,
                  std::string *reason) {
  const std::string name = channel.m_name;
  if (MotionPreset::getStageObjectChannel(name) < 0)
    return fail(reason, "unknown channel \"" + name + "\"");

  const std::vector<MotionPreset::Key> &keys = channel.m_keys;
  if (keys.size() < 2)
    return fail(reason, name + ": a channel needs at least two keys");

  for (size_t i = 0; i < keys.size(); ++i) {
    const MotionPreset::Key &key = keys[i];
    if (!std::isfinite(key.m_frame) || !std::isfinite(key.m_value) ||
        !std::isfinite(key.m_speedIn.x) || !std::isfinite(key.m_speedIn.y) ||
        !std::isfinite(key.m_speedOut.x) || !std::isfinite(key.m_speedOut.y))
      return fail(reason, name + ": a number is not finite");
    if (key.m_frame < 0) return fail(reason, name + ": a frame is negative");
    if (i > 0 && key.m_frame < keys[i - 1].m_frame + 1e-6)
      return fail(reason, name + ": the frames are not increasing");
    if (isNonKeyType(key.m_type))
      return fail(reason, name + ": unsupported interpolation");
  }

  if (channel.m_mode == MotionPreset::Multiply) {
    if (std::abs(keys.front().m_value) < 1e-9 ||
        (anchorEnd && std::abs(keys.back().m_value) < 1e-9))
      return fail(reason, name + ": a scale ratio is null");
  }
  return true;
}

//-----------------------------------------------------------------------------

void snapshotKeys(const TDoubleParam *param,
                  std::vector<TDoubleKeyframe> &keys) {
  keys.clear();
  for (int i = 0; i < param->getKeyframeCount(); ++i)
    keys.push_back(param->getKeyframe(i));
}

//=============================================================================
// MotionPresetUndo
//-----------------------------------------------------------------------------
/*! Restores all the keys of a curve. The keys after the apply are read when
    the undo is created.                                                    */

class MotionPresetUndo final : public TUndo {
  TDoubleParamP m_param;
  std::vector<TDoubleKeyframe> m_oldKeys, m_newKeys;

  static void setKeys(TDoubleParam *param,
                      const std::vector<TDoubleKeyframe> &keys) {
    param->clearKeyframes();
    for (const TDoubleKeyframe &key : keys) param->setKeyframe(key);
  }

public:
  MotionPresetUndo(TDoubleParam *param,
                   const std::vector<TDoubleKeyframe> &oldKeys)
      : m_param(param), m_oldKeys(oldKeys) {
    snapshotKeys(param, m_newKeys);
  }

  void undo() const override { setKeys(m_param.getPointer(), m_oldKeys); }
  void redo() const override { setKeys(m_param.getPointer(), m_newKeys); }
  int getSize() const override {
    return (int)(sizeof(*this) + (m_oldKeys.size() + m_newKeys.size()) *
                                     sizeof(TDoubleKeyframe));
  }

  QString getHistoryString() override {
    return QObject::tr("Apply Motion Preset");
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

}  // namespace

//=============================================================================
// MotionPreset
//-----------------------------------------------------------------------------

int MotionPreset::getStageObjectChannel(const std::string &name) {
  for (const ChannelInfo &info : channelTable)
    if (name == info.m_name) return info.m_channel;
  return -1;
}

//-----------------------------------------------------------------------------

std::string MotionPreset::getChannelName(int stageObjectChannel) {
  for (const ChannelInfo &info : channelTable)
    if (stageObjectChannel == info.m_channel) return info.m_name;
  return std::string();
}

//-----------------------------------------------------------------------------

MotionPreset::Mode MotionPreset::getDefaultMode(const std::string &name) {
  for (const ChannelInfo &info : channelTable)
    if (name == info.m_name) return info.m_mode;
  return Add;
}

//-----------------------------------------------------------------------------

std::vector<std::string> MotionPreset::getChannelNames() {
  std::vector<std::string> names;
  for (const ChannelInfo &info : channelTable) names.push_back(info.m_name);
  return names;
}

//-----------------------------------------------------------------------------

const MotionPreset::Channel *MotionPreset::findChannel(
    const std::string &name) const {
  for (const Channel &channel : m_channels)
    if (channel.m_name == name) return &channel;
  return 0;
}

//-----------------------------------------------------------------------------

double MotionPreset::getDuration() const {
  double duration = 0;
  for (const Channel &channel : m_channels)
    if (!channel.m_keys.empty())
      duration = std::max(duration, channel.m_keys.back().m_frame);
  return duration;
}

//-----------------------------------------------------------------------------

bool MotionPreset::isValid(std::string *reason) const {
  if (m_channels.empty()) return fail(reason, "the preset has no channel");
  for (size_t i = 0; i < m_channels.size(); ++i) {
    for (size_t j = 0; j < i; ++j)
      if (m_channels[i].m_name == m_channels[j].m_name)
        return fail(reason,
                    "the channel \"" + m_channels[i].m_name + "\" is repeated");
    if (!checkChannel(m_channels[i], m_anchor == AnchorEnd, reason))
      return false;
  }
  return true;
}

//-----------------------------------------------------------------------------

bool MotionPreset::isNormalized() const {
  if (m_channels.empty()) return false;
  double earliest = std::numeric_limits<double>::max();
  for (const Channel &channel : m_channels) {
    if (channel.m_keys.empty()) return false;
    const Key &first     = channel.m_keys.front();
    const double neutral = channel.m_mode == Multiply ? 1.0 : 0.0;
    if (std::abs(first.m_value - neutral) > 1e-6 || first.m_frame < 0)
      return false;
    earliest = std::min(earliest, first.m_frame);
  }
  return std::abs(earliest) < 1e-6;
}

//-----------------------------------------------------------------------------

bool MotionPreset::isAnimated(const Channel &channel) {
  if (channel.m_keys.size() < 2) return false;
  for (const Key &key : channel.m_keys)
    if (std::abs(key.m_value - channel.m_keys.front().m_value) > 1e-9)
      return true;
  return false;
}

//-----------------------------------------------------------------------------
// Files
//-----------------------------------------------------------------------------

void MotionPreset::saveData(TOStream &os) const {
  os.child("name") << m_name.toStdString();
  if (!m_description.isEmpty())
    os.child("description") << m_description.toStdString();
  if (m_anchor == AnchorEnd) os.child("anchor") << std::string("end");

  for (const Channel &channel : m_channels) {
    std::map<std::string, std::string> attributes;
    attributes["name"] = channel.m_name;
    attributes["mode"] = channel.m_mode == Multiply ? "multiply" : "add";
    os.openChild("channel", attributes);
    for (const Key &key : channel.m_keys) {
      os << key.m_frame << key.m_value << typeToCode(key.m_type)
         << key.m_speedIn.x << key.m_speedIn.y << key.m_speedOut.x
         << key.m_speedOut.y << (int)(key.m_linkedHandles ? 1 : 0);
      os.cr();
    }
    os.closeChild();
  }
}

//-----------------------------------------------------------------------------

void MotionPreset::loadData(TIStream &is) {
  m_name        = QString();
  m_description = QString();
  m_anchor      = AnchorStart;
  m_channels.clear();

  std::string tagName;
  while (is.matchTag(tagName)) {
    if (tagName == "name") {
      std::string text;
      is >> text;
      m_name = QString::fromStdString(text);
    } else if (tagName == "description") {
      std::string text;
      is >> text;
      m_description = QString::fromStdString(text);
    } else if (tagName == "anchor") {
      std::string text;
      is >> text;
      m_anchor = text == "end" ? AnchorEnd : AnchorStart;
    } else if (tagName == "channel") {
      Channel channel;
      std::string text;
      if (is.getTagParam("name", text)) channel.m_name = text;
      channel.m_mode = getDefaultMode(channel.m_name);
      if (is.getTagParam("mode", text))
        channel.m_mode = text == "multiply" ? Multiply : Add;

      while (!is.eos()) {
        Key key;
        std::string code;
        int linked = 0;
        is >> key.m_frame >> key.m_value >> code >> key.m_speedIn.x >>
            key.m_speedIn.y >> key.m_speedOut.x >> key.m_speedOut.y >> linked;
        if (!codeToType(code, key.m_type))
          throw TException("unknown interpolation \"" + code + "\"");
        key.m_linkedHandles = linked != 0;
        channel.m_keys.push_back(key);
      }
      m_channels.push_back(channel);
    } else {
      // Ignore data written by a newer version
      is.skipCurrentTag();
      continue;
    }
    if (!is.matchEndTag()) throw TException(tagName + " : missing end tag");
  }
}

//-----------------------------------------------------------------------------

bool MotionPreset::save(const TFilePath &fp, std::string *error) const {
  std::string reason;
  if (!isValid(&reason)) return fail(error, reason);

  try {
    QDir().mkpath(fp.getParentDir().getQString());
    TOStream os(fp);
    if (!os) return fail(error, "the file can't be written");
    os.openChild("motionpreset");
    saveData(os);
    os.closeChild();
    if (!os.checkStatus()) return fail(error, "the file can't be written");
  } catch (const TException &e) {
    return fail(error, ::to_string(e.getMessage()));
  } catch (...) {
    return fail(error, "the file can't be written");
  }
  return true;
}

//-----------------------------------------------------------------------------

bool MotionPreset::load(const TFilePath &fp, MotionPreset &preset,
                        std::string *error) {
  try {
    TIStream is(fp);
    if (!is) return fail(error, "the file can't be read");

    std::string tagName;
    if (!is.matchTag(tagName) || tagName != "motionpreset")
      return fail(error, "this is not a motion preset");

    MotionPreset loaded;
    loaded.loadData(is);
    if (!is.matchEndTag()) return fail(error, "missing end tag");
    preset = loaded;
  } catch (const TException &e) {
    return fail(error, ::to_string(e.getMessage()));
  } catch (...) {
    return fail(error, "the file can't be read");
  }
  return true;
}

//-----------------------------------------------------------------------------
// Capture
//-----------------------------------------------------------------------------

MotionPreset::CaptureResult MotionPreset::captureChannel(
    const TDoubleParam *param, const std::string &name, double firstFrame,
    double lastFrame, double startFrame, Channel &channel,
    std::string *reason) {
  channel = Channel(name, getDefaultMode(name));
  if (!param) return CaptureNothing;

  // The keys in the range
  int i0 = -1, i1 = -1;
  for (int i = 0; i < param->getKeyframeCount(); ++i) {
    const double frame = param->getKeyframe(i).m_frame;
    if (frame < firstFrame || frame > lastFrame) continue;
    if (i0 < 0) i0 = i;
    i1 = i;
  }
  if (i0 < 0 || i1 <= i0) return CaptureNothing;

  for (int i = i0; i < i1; ++i)
    if (isNonKeyType(param->getKeyframe(i).m_type)) {
      fail(reason, "the curve has an expression, a file or a similar shape");
      return CaptureSkipped;
    }

  // A scale that starts from 0 can't be a ratio
  const double v0 = param->getKeyframe(i0).m_value;
  if (channel.m_mode == Multiply && std::abs(v0) < 1e-6) channel.m_mode = Add;
  const bool add       = channel.m_mode == Add;
  const double yFactor = add ? 1.0 : 1.0 / v0;

  for (int i = i0; i <= i1; ++i) {
    const TDoubleKeyframe kf = param->getKeyframe(i);
    Key key;
    key.m_frame = kf.m_frame - startFrame;
    key.m_value = add ? kf.m_value - v0 : kf.m_value / v0;
    key.m_type  = i == i1 ? TDoubleKeyframe::Linear : presetType(kf.m_type);
    key.m_linkedHandles = kf.m_linkedHandles;

    // The handles as the curve uses them. The first key has no incoming
    // segment and the last one no outgoing segment.
    if (i > i0) {
      const TDoubleKeyframe::Type prevType = param->getKeyframe(i - 1).m_type;
      if (prevType == TDoubleKeyframe::SpeedInOut) {
        key.m_speedIn       = param->getSpeedIn(i);
        key.m_linkedHandles = false;
      } else if (usesHandles(prevType))
        key.m_speedIn = kf.m_speedIn;
    }
    if (i < i1) {
      if (kf.m_type == TDoubleKeyframe::SpeedInOut) {
        key.m_speedOut      = param->getSpeedOut(i);
        key.m_linkedHandles = false;
      } else if (usesHandles(kf.m_type))
        key.m_speedOut = kf.m_speedOut;
    }
    key.m_speedIn.y *= yFactor;
    key.m_speedOut.y *= yFactor;
    channel.m_keys.push_back(key);
  }
  return CaptureDone;
}

//-----------------------------------------------------------------------------

bool MotionPreset::captureFromKeys(const TStageObject *obj, int firstFrame,
                                   int lastFrame, MotionPreset &preset,
                                   std::vector<std::string> *skipped) {
  preset.clearChannels();
  if (!obj || lastFrame <= firstFrame) return false;

  for (const ChannelInfo &info : channelTable) {
    const TDoubleParam *param =
        obj->getParam((TStageObject::Channel)info.m_channel);
    Channel channel;
    std::string reason;
    CaptureResult result =
        captureChannel(param, info.m_name, firstFrame, lastFrame, firstFrame,
                       channel, &reason);
    if (result == CaptureDone) {
      // A channel that only holds its value is no motion
      if (isAnimated(channel)) preset.addChannel(channel);
    } else if (result == CaptureSkipped && skipped)
      skipped->push_back(std::string(info.m_name) + ": " + reason);
  }
  if (preset.m_channels.empty()) return false;

  // The preset starts at its earliest key
  double earliest = std::numeric_limits<double>::max();
  for (const Channel &channel : preset.m_channels)
    earliest = std::min(earliest, channel.m_keys.front().m_frame);
  if (earliest != 0)
    for (Channel &channel : preset.m_channels)
      for (Key &key : channel.m_keys) key.m_frame -= earliest;
  return true;
}

//-----------------------------------------------------------------------------
// Apply
//-----------------------------------------------------------------------------

bool MotionPreset::applyChannel(const Channel &channel, TDoubleParam *param,
                                double frame, const TDoubleParam *base,
                                std::string *reason) const {
  const bool anchorEnd = m_anchor == AnchorEnd;
  if (!checkChannel(channel, anchorEnd, reason)) return false;
  if (!param) return fail(reason, "the curve does not exist");
  if (param->isCycleEnabled()) return fail(reason, "the curve is cycled");

  const std::vector<Key> &keys = channel.m_keys;
  const Key &first             = keys.front();
  const Key &last              = keys.back();
  const double f0              = frame + first.m_frame;
  const double f1              = frame + last.m_frame;
  if (f0 < 0) return fail(reason, "the animation would start before frame 1");

  // The segments of the curve where the keys go must be made of keys
  const int count = param->getKeyframeCount();
  for (int i = 0; i + 1 < count; ++i) {
    const TDoubleKeyframe &kf = param->getKeyframe(i);
    if (kf.m_frame > f1) break;
    if (param->getKeyframe(i + 1).m_frame < f0) continue;
    if (isNonKeyType(kf.m_type))
      return fail(reason, "the curve has an expression or a file there");
  }

  // Compute every new key before changing the curve
  const double start = param->getValue(f0);
  const double ref   = base ? base->getValue(f0) : start;

  std::vector<TDoubleKeyframe> newKeys;
  for (const Key &key : keys) {
    double value, yFactor;
    if (channel.m_mode == Add) {
      const double shift = anchorEnd ? first.m_value - last.m_value : 0.0;
      value              = start + shift + (key.m_value - first.m_value);
      yFactor            = 1.0;
    } else {
      // The ratios are relative to the first key, or to the last one
      const double divisor = anchorEnd ? last.m_value : first.m_value;
      value                = start + ref * (key.m_value / divisor - 1.0);
      yFactor              = ref / divisor;
    }
    TDoubleKeyframe kf(frame + key.m_frame, value);
    kf.m_type          = presetType(key.m_type);
    kf.m_speedIn       = TPointD(key.m_speedIn.x, key.m_speedIn.y * yFactor);
    kf.m_speedOut      = TPointD(key.m_speedOut.x, key.m_speedOut.y * yFactor);
    kf.m_linkedHandles = key.m_linkedHandles;
    kf.m_step          = 1;
    kf.m_isKeyframe    = true;
    newKeys.push_back(kf);
  }

  // The keys of the curve in the way are replaced
  std::vector<double> oldFrames;
  for (int i = 0; i < count; ++i) {
    const double f = param->getKeyframe(i).m_frame;
    if (f >= f0 && f <= f1) oldFrames.push_back(f);
  }
  for (double f : oldFrames) param->deleteKeyframe(f);
  for (const TDoubleKeyframe &kf : newKeys) param->setKeyframe(kf);
  return true;
}

//-----------------------------------------------------------------------------

MotionPreset::ApplyResult MotionPreset::apply(TStageObject *obj, double frame,
                                              int layerIndex,
                                              bool addUndo) const {
  ApplyResult result;
  std::string reason;
  if (!obj) {
    result.m_skipped.push_back("no object");
    return result;
  }
  if (!isValid(&reason)) {
    result.m_skipped.push_back(reason);
    return result;
  }
  if (layerIndex >= obj->getOffsetLayerCount()) {
    result.m_skipped.push_back("the offset layer does not exist");
    return result;
  }

  for (const Channel &channel : m_channels) {
    const TStageObject::Channel soChannel =
        (TStageObject::Channel)getStageObjectChannel(channel.m_name);
    TDoubleParam *base = obj->getParam(soChannel);
    TDoubleParam *param =
        layerIndex < 0 ? base : obj->getOffsetParam(layerIndex, soChannel);
    if (!param) {
      result.m_skipped.push_back(channel.m_name + ": the curve does not exist");
      continue;
    }

    std::vector<TDoubleKeyframe> oldKeys;
    snapshotKeys(param, oldKeys);

    std::string why;
    if (!applyChannel(channel, param, frame, layerIndex < 0 ? 0 : base, &why)) {
      result.m_skipped.push_back(channel.m_name + ": " + why);
      continue;
    }
    ++result.m_channelCount;
    result.m_keyCount += (int)channel.m_keys.size();
    if (addUndo)
      TUndoManager::manager()->add(new MotionPresetUndo(param, oldKeys));
  }
  return result;
}
