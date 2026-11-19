#include "automation.h"
#include "automation_animation.h"
#include "automation_object.h"
#include "automation_rig.h"

#include "tdoubleparam.h"
#include "toonz/stageunits.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"

#include <QSet>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {

using Channel = TStageObject::Channel;

constexpr int MaxDepth            = 64;
constexpr int MaxBakeFrames       = 10000;
constexpr int MaxVerifiedFrames   = 256;
constexpr double MaxValue         = 1000000;
constexpr double Exact            = 1e-12;
constexpr double PreservedInch    = 1e-6;
constexpr double PreservedLinear  = 1e-9;
constexpr double Orthogonal       = 1e-9;
constexpr double Degenerate       = 1e-9;
constexpr double DegreesPerRadian = 180.0 / 3.14159265358979323846;

struct Named final {
  const char* name;
  Channel native;
};

const Named Placement[] = {{"x", TStageObject::T_X},
                           {"y", TStageObject::T_Y},
                           {"z", TStageObject::T_Z},
                           {"so", TStageObject::T_SO},
                           {"rotation", TStageObject::T_Angle},
                           {"scale_x", TStageObject::T_ScaleX},
                           {"scale_y", TStageObject::T_ScaleY},
                           {"scale", TStageObject::T_Scale},
                           {"shear_x", TStageObject::T_ShearX},
                           {"shear_y", TStageObject::T_ShearY},
                           {"tilt_x", TStageObject::T_RotX},
                           {"tilt_y", TStageObject::T_RotY}};

const Named Baked[] = {{"x", TStageObject::T_X},
                       {"y", TStageObject::T_Y},
                       {"z", TStageObject::T_Z},
                       {"so", TStageObject::T_SO},
                       {"rotation", TStageObject::T_Angle},
                       {"scale_x", TStageObject::T_ScaleX},
                       {"scale_y", TStageObject::T_ScaleY}};

[[noreturn]] void unrepresentable(const QString& message,
                                  const QJsonObject& details) {
  throw Error("unsupported_target", message, details);
}

bool letter(const std::string& handle) {
  return handle.size() == 1 &&
         std::isalpha(static_cast<unsigned char>(handle[0]));
}

bool animated(const TDoubleParam* curve) {
  return curve && curve->getKeyframeCount() > 1;
}

// Why an object's local placement, z and stacking order can change over time,
// or an empty string when they are the same at every frame, subframes
// included.
QString motionReason(const TStageObject& object) {
  if ((object.getStatus() & TStageObject::STATUS_MASK) != TStageObject::XY)
    return "follows a motion path";
  if (!letter(object.getHandle()) || !letter(object.getParentHandle()))
    return "uses a hook handle that follows its drawings";
  for (const auto& channel : Placement)
    if (animated(object.getParam(channel.native)))
      return QString("animates ") + channel.name;
  for (int index = 0; index < object.getOffsetLayerCount(); ++index) {
    const auto& layer = object.getOffsetLayer(index);
    if (animated(layer.m_weight.getPointer())) return "animates a layer weight";
    for (const auto& curve : layer.m_channels)
      if (animated(curve.getPointer())) return "animates an offset layer";
  }
  return {};
}

std::vector<TStageObject*> ancestry(TStageObjectTree& stage,
                                    TStageObject* object) {
  std::vector<TStageObject*> chain;
  while (object && static_cast<int>(chain.size()) <= MaxDepth + 1) {
    chain.push_back(object);
    object = stage.getStageObject(object->getParent(), false);
  }
  return chain;
}

// The objects between an attachment parent and the common ancestor of both
// parents, top-down; the whole chain when the parents share no ancestor.
std::vector<TStageObject*> branch(const std::vector<TStageObject*>& chain,
                                  const std::vector<TStageObject*>& other) {
  const std::set<TStageObject*> shared(other.begin(), other.end());
  std::vector<TStageObject*> result;
  for (TStageObject* object : chain) {
    if (shared.count(object)) break;
    result.push_back(object);
  }
  std::reverse(result.begin(), result.end());
  return result;
}

double channelValue(const TStageObject& object, Channel channel, double frame) {
  return object.getParam(channel)->getValue(frame) +
         object.getOffsetValue(channel, frame);
}

struct Attachment final {
  TStageObject* parent;
  std::string handle;
};

struct Relative final {
  TAffine placement;
  double z  = 0;
  double so = 0;
};

// Placement, z and stacking order of an attachment point relative to the
// common ancestor, from static branch objects.
Relative relative(ToonzScene& scene, const std::vector<TStageObject*>& objects,
                  const Attachment& attachment) {
  Relative result;
  for (TStageObject* object : objects) {
    result.placement = result.placement * object->getLocalPlacement(0);
    result.z += channelValue(*object, TStageObject::T_Z, 0);
    result.so += channelValue(*object, TStageObject::T_SO, 0);
  }
  const TPointD handle =
      automationHandlePosition(scene, *attachment.parent, attachment.handle, 0);
  result.placement = result.placement * TTranslation(handle * Stage::inch);
  return result;
}

struct Rewrite final {
  Channel channel;
  const char* name;
  AnimationCurveEdit edit;
};

AnimationCurveEdit currentCurve(const TDoubleParam& curve) {
  AnimationCurveEdit edit;
  edit.defaultValue = curve.getDefaultValue();
  for (int index = 0; index < curve.getKeyframeCount(); ++index)
    edit.keys.push_back(curve.getKeyframe(index));
  return edit;
}

void validateEdit(const TDoubleParam& curve, const AnimationCurveEdit& edit) {
  if (curve.isEditorLocked())
    throw Error("locked", "The animation channel is locked.");
  if (!std::isfinite(edit.defaultValue) ||
      std::abs(edit.defaultValue) > MaxValue)
    throw Error("invalid_params",
                "A rewritten channel default is outside its bounds.");
  TDoubleParamP prospective(new TDoubleParam(curve));
  applyAnimationCurveEdit(*prospective, edit);
  requireSupportedAnimationCurve(*prospective);
}

// Adding a constant keeps every admitted interpolation shape: speed handles
// are relative to their key and ease is a timing.
AnimationCurveEdit shifted(const TDoubleParam& curve, double delta) {
  auto edit = currentCurve(curve);
  edit.defaultValue += delta;
  for (auto& key : edit.keys) key.m_value += delta;
  return edit;
}

AnimationCurveEdit scaled(const TDoubleParam& curve, double factor) {
  auto edit = currentCurve(curve);
  edit.defaultValue *= factor;
  for (auto& key : edit.keys) {
    key.m_value *= factor;
    key.m_speedIn.y *= factor;
    key.m_speedOut.y *= factor;
  }
  return edit;
}

AnimationCurveEdit constant(const TDoubleParam& curve, double value) {
  auto edit = currentCurve(curve);
  if (edit.keys.empty())
    edit.defaultValue = value;
  else
    edit.keys.front().m_value = value;
  return edit;
}

void addRewrite(std::vector<Rewrite>& rewrites, const TStageObject& object,
                const Named& channel, const AnimationCurveEdit& edit) {
  const TDoubleParam& curve = *object.getParam(channel.native);
  validateEdit(curve, edit);
  rewrites.push_back({channel.native, channel.name, edit});
}

const Named& named(Channel channel) {
  for (const auto& entry : Placement)
    if (entry.native == channel) return entry;
  throw Error("native_error", "An unknown placement channel was requested.");
}

bool layersTouch(const TStageObject& object,
                 std::initializer_list<Channel> channels) {
  for (int index = 0; index < object.getOffsetLayerCount(); ++index) {
    const auto& layer = object.getOffsetLayer(index);
    for (const Channel channel : channels) {
      const TDoubleParam* curve = layer.m_channels[channel].getPointer();
      if (curve &&
          (curve->getKeyframeCount() != 0 || curve->getDefaultValue() != 0))
        return true;
    }
  }
  return false;
}

struct Change final {
  TAffine affine;
  double z  = 0;
  double so = 0;
};

// Rigid motion with uniform scale, absorbed exactly into the object's own
// static position, rotation and global scale. Any other change of attachment
// frame would need shear or new keys.
void similarityRewrite(const TStageObject& object, const Change& change,
                       std::vector<Rewrite>& rewrites) {
  const TAffine& a   = change.affine;
  const double size  = std::hypot(a.a11, a.a21);
  const bool similar = std::abs(a.a11 - a.a22) <= Exact * std::max(1.0, size) &&
                       std::abs(a.a12 + a.a21) <= Exact * std::max(1.0, size) &&
                       size > 0;
  if (!similar)
    unrepresentable("The new attachment frame shears or mirrors the object.",
                    {{"reason", "attachment_shear_or_mirror"}});
  const QString moving =
      animated(object.getParam(TStageObject::T_X)) ||
              animated(object.getParam(TStageObject::T_Y))
          ? "x/y keys"
      : layersTouch(object, {TStageObject::T_X, TStageObject::T_Y})
          ? "x/y offset layers"
          : QString();
  if (!moving.isEmpty())
    unrepresentable(
        "A rotated or scaled attachment frame needs the object's "
        "position to be static; it has " +
            moving + ".",
        {{"reason", "animated_position_in_rotated_frame"}});
  const bool resized = std::abs(size - 1) > Exact;
  if (resized &&
      layersTouch(object, {TStageObject::T_Scale, TStageObject::T_ScaleX,
                           TStageObject::T_ScaleY}))
    unrepresentable("Scale offset layers cannot follow a rescaled parent.",
                    {{"reason", "scale_offset_layers"}});
  TPointD center, offset;
  object.getCenterAndOffset(center, offset);
  const TPointD position(channelValue(object, TStageObject::T_X, 0),
                         channelValue(object, TStageObject::T_Y, 0));
  const TPointD moved =
      a * ((offset + position) * Stage::inch) * (1.0 / Stage::inch) - offset;
  for (const Channel channel : {TStageObject::T_X, TStageObject::T_Y}) {
    const double target = channel == TStageObject::T_X ? moved.x : moved.y;
    addRewrite(rewrites, object, named(channel),
               constant(*object.getParam(channel),
                        target - object.getOffsetValue(channel, 0)));
  }
  const double angle = std::atan2(a.a21, a.a11) * DegreesPerRadian;
  if (angle != 0)
    addRewrite(rewrites, object, named(TStageObject::T_Angle),
               shifted(*object.getParam(TStageObject::T_Angle), angle));
  if (resized)
    addRewrite(rewrites, object, named(TStageObject::T_Scale),
               scaled(*object.getParam(TStageObject::T_Scale), size));
}

std::vector<Rewrite> exactRewrites(const TStageObject& object,
                                   const Change& change) {
  std::vector<Rewrite> rewrites;
  const TAffine& a       = change.affine;
  const bool translation = std::abs(a.a11 - 1) <= Exact &&
                           std::abs(a.a22 - 1) <= Exact &&
                           std::abs(a.a12) <= Exact && std::abs(a.a21) <= Exact;
  if (translation) {
    if (a.a13 != 0)
      addRewrite(
          rewrites, object, named(TStageObject::T_X),
          shifted(*object.getParam(TStageObject::T_X), a.a13 / Stage::inch));
    if (a.a23 != 0)
      addRewrite(
          rewrites, object, named(TStageObject::T_Y),
          shifted(*object.getParam(TStageObject::T_Y), a.a23 / Stage::inch));
  } else {
    similarityRewrite(object, change, rewrites);
  }
  for (const Channel channel : {TStageObject::T_Z, TStageObject::T_SO}) {
    const double delta = channel == TStageObject::T_Z ? change.z : change.so;
    if (delta != 0)
      addRewrite(rewrites, object, named(channel),
                 shifted(*object.getParam(channel), delta));
  }
  return rewrites;
}

struct Request final {
  ToonzScene* scene;
  TStageObject* object;
  Attachment from;
  Attachment to;
};

// Frames up to which any curve that moves the object, either parent chain or
// an exposure can still change.
int horizon(ToonzScene& scene, const std::vector<TStageObject*>& objects) {
  int last = std::max(0, scene.getFrameCount() - 1);
  for (const TStageObject* object : objects) {
    for (const auto& channel : Placement) {
      const TDoubleParam* curve = object->getParam(channel.native);
      if (curve && curve->getKeyframeCount() > 0)
        last = std::max(last, static_cast<int>(curve->keyframeIndexToFrame(
                                  curve->getKeyframeCount() - 1)));
    }
  }
  return last;
}

std::vector<TStageObject*> movingObjects(const Request& request) {
  auto& stage  = *request.scene->getXsheet()->getStageObjectTree();
  auto objects = ancestry(stage, request.from.parent);
  for (TStageObject* object : ancestry(stage, request.to.parent))
    objects.push_back(object);
  objects.push_back(request.object);
  return objects;
}

void requireOwnMotion(const TStageObject& object) {
  if ((object.getStatus() & TStageObject::STATUS_MASK) != TStageObject::XY)
    unrepresentable("Objects that follow a motion path keep preserve_local.",
                    {{"reason", "path_consumer"}});
}

void requireBakeable(const TStageObject& object) {
  requireOwnMotion(object);
  if (object.getOffsetLayerCount() != 0)
    unrepresentable(
        "A bake rewrites base channels; remove or bake the "
        "object's offset layers first.",
        {{"reason", "offset_layers"}});
  if (object.isCycleEnabled())
    unrepresentable("A cycled object cannot be baked.", {{"reason", "cycle"}});
  for (const Channel channel : {TStageObject::T_ShearX, TStageObject::T_ShearY,
                                TStageObject::T_RotX, TStageObject::T_RotY}) {
    const TDoubleParam* curve = object.getParam(channel);
    if (curve->getKeyframeCount() != 0 || curve->getDefaultValue() != 0)
      unrepresentable("A bake keeps shear and tilt at zero.",
                      {{"reason", "shear_or_tilt"}});
  }
}

QJsonObject bakeProposal(const Request& request, const QString& reason) {
  try {
    requireBakeable(*request.object);
  } catch (const Error& error) {
    return {{"reason", reason}, {"bake_unavailable", error.message}};
  }
  const int last = horizon(*request.scene, movingObjects(request));
  const int end  = std::min(last, MaxBakeFrames - 1);
  QJsonArray channels;
  for (const auto& channel : Baked) channels.append(channel.name);
  return {
      {"reason", reason},
      {"bake_proposal", QJsonObject{{"kind", "bake"},
                                    {"start_frame", 0},
                                    {"end_frame", end},
                                    {"new_key_interpolation", "linear"},
                                    {"channels", channels},
                                    {"covers_all_animation", last == end}}}};
}

// The change of attachment frame, or a refusal with a bake proposal when it
// is not one fixed transform at every frame.
Change attachmentChange(const Request& request) {
  automationRefreshPlacements(*request.scene);
  auto& stage          = *request.scene->getXsheet()->getStageObjectTree();
  const auto old       = ancestry(stage, request.from.parent);
  const auto now       = ancestry(stage, request.to.parent);
  const auto oldBranch = branch(old, now);
  const auto newBranch = branch(now, old);
  for (const auto* objects : {&oldBranch, &newBranch})
    for (TStageObject* object : *objects) {
      const QString reason = motionReason(*object);
      if (!reason.isEmpty())
        unrepresentable(
            "World placement cannot be kept exactly: " +
                QString::fromStdString(object->getName()) + " " + reason +
                ". Use world_policy bake or placement_policy preserve_local.",
            bakeProposal(request, "parent_branch_moves"));
    }
  for (const auto* attachment : {&request.from, &request.to})
    if (!letter(attachment->handle))
      unrepresentable("A hook parent handle follows its drawings.",
                      bakeProposal(request, "hook_parent_handle"));
  const Relative before = relative(*request.scene, oldBranch, request.from);
  const Relative after  = relative(*request.scene, newBranch, request.to);
  if (std::abs(after.placement.det()) <= Degenerate)
    unrepresentable("The new parent collapses stage space.",
                    {{"reason", "singular_parent"}});
  return {after.placement.inv() * before.placement, before.z - after.z,
          before.so - after.so};
}

struct Deviation final {
  double translation = 0;
  double linear      = 0;
  double z           = 0;
  int frames         = 0;
};

void measure(Deviation& deviation, const TAffine& expected,
             const TAffine& actual, double zExpected, double zActual) {
  deviation.translation = std::max(
      deviation.translation,
      std::hypot(actual.a13 - expected.a13, actual.a23 - expected.a23) /
          Stage::inch);
  deviation.z      = std::max(deviation.z, std::abs(zActual - zExpected));
  deviation.linear = std::max(
      {deviation.linear, std::abs(actual.a11 - expected.a11),
       std::abs(actual.a12 - expected.a12), std::abs(actual.a21 - expected.a21),
       std::abs(actual.a22 - expected.a22)});
  ++deviation.frames;
}

struct World final {
  std::vector<int> frames;
  std::vector<TAffine> placements;
  std::vector<double> z;
};

World sampleWorld(ToonzScene& scene, TStageObject& object,
                  const std::vector<int>& frames) {
  World world{frames, automationPlacements(scene, object, frames), {}};
  for (const int frame : frames) world.z.push_back(object.getZ(frame));
  return world;
}

Deviation compareWorld(ToonzScene& scene, TStageObject& object,
                       const World& before) {
  const World after = sampleWorld(scene, object, before.frames);
  Deviation deviation;
  for (std::size_t index = 0; index < before.frames.size(); ++index)
    measure(deviation, before.placements[index], after.placements[index],
            before.z[index], after.z[index]);
  return deviation;
}

std::vector<int> exactFrames(const Request& request,
                             const QJsonObject& params) {
  std::set<int> frames{0};
  for (const int frame : automationReportFrames(params)) frames.insert(frame);
  const int last = horizon(*request.scene, movingObjects(request));
  for (int step = 0; step <= 8; ++step)
    frames.insert(static_cast<int>(static_cast<qint64>(last) * step / 8));
  for (const auto& channel : Placement) {
    const TDoubleParam* curve = request.object->getParam(channel.native);
    for (int index = 0; curve && index < curve->getKeyframeCount() &&
                        static_cast<int>(frames.size()) < MaxVerifiedFrames;
         ++index) {
      const int key = static_cast<int>(curve->keyframeIndexToFrame(index));
      frames.insert(key);
      frames.insert(key + 1);
    }
  }
  return {frames.begin(), frames.end()};
}

void attach(const Request& request) {
  request.object->setParent(request.to.parent->getId());
  request.object->setParentHandle(request.to.handle);
}

int keyCount(const std::vector<Rewrite>& rewrites) {
  int keys = 0;
  for (const auto& rewrite : rewrites)
    keys += static_cast<int>(rewrite.edit.keys.size());
  return keys;
}

QJsonObject outcome(const QString& method, const QString& exactAt,
                    const std::vector<Rewrite>& rewrites, int keys,
                    const Deviation& deviation) {
  QJsonArray channels;
  for (const auto& rewrite : rewrites) channels.append(rewrite.name);
  return {{"method", method},
          {"exact_at", exactAt},
          {"channels_rewritten", channels},
          {"keys_rewritten", keys},
          {"verified_frames", deviation.frames},
          {"max_translation_deviation_inch", deviation.translation},
          {"max_linear_deviation", deviation.linear},
          {"max_z_deviation", deviation.z}};
}

void requireVerified(const Deviation& deviation) {
  if (deviation.translation > PreservedInch ||
      deviation.linear > PreservedLinear || deviation.z > PreservedInch)
    throw Error("native_error",
                "The rewritten world placement differs from the original.",
                {{"max_translation_deviation_inch", deviation.translation},
                 {"max_linear_deviation", deviation.linear},
                 {"max_z_deviation", deviation.z}});
}

QJsonObject preserveExactly(Context& context, const Request& request,
                            const QJsonObject& params) {
  requireOwnMotion(*request.object);
  const Change change = attachmentChange(request);
  std::vector<Rewrite> rewrites;
  try {
    rewrites = exactRewrites(*request.object, change);
  } catch (const Error& error) {
    if (error.code != "unsupported_target") throw;
    throw Error(
        error.code,
        error.message +
            " Use world_policy bake or placement_policy "
            "preserve_local.",
        bakeProposal(request, error.details.value("reason").toString()));
  }
  const World before = sampleWorld(*request.scene, *request.object,
                                   exactFrames(request, params));
  context.markMutationStarted();
  attach(request);
  for (const auto& rewrite : rewrites)
    applyAnimationCurveEdit(*request.object->getParam(rewrite.channel),
                            rewrite.edit);
  const Deviation deviation =
      compareWorld(*request.scene, *request.object, before);
  requireVerified(deviation);
  return outcome("exact_rewrite", "all_frames", rewrites, keyCount(rewrites),
                 deviation);
}

struct Range final {
  int start;
  int end;
  TDoubleKeyframe::Type type;
};

Range bakeRange(const QJsonObject& policy) {
  const int start = policy.value("start_frame").toInt();
  const int end   = policy.value("end_frame").toInt();
  if (end < start || end - start + 1 > MaxBakeFrames)
    throw Error("invalid_params",
                "A bake covers 1 through 10000 frames from start_frame to "
                "end_frame.");
  return {start, end,
          policy.value("new_key_interpolation") == "constant"
              ? TDoubleKeyframe::Constant
              : TDoubleKeyframe::Linear};
}

using Samples = std::vector<std::vector<double>>;

struct Frame final {
  TAffine local;
  TPointD center;
  double scale;
};

double unwrap(double angle, double near) {
  return angle + 360.0 * std::round((near - angle) / 360.0);
}

// Solve the new local x, y, rotation and scale factors from a target local
// placement without shear.
std::vector<double> solveFrame(const Frame& frame, const TPointD& offset,
                               double& angle, int row) {
  const TAffine& q   = frame.local;
  const double width = std::hypot(q.a11, q.a21);
  if (width <= Degenerate || std::abs(frame.scale) <= Degenerate)
    unrepresentable("The world placement is degenerate at a baked frame.",
                    {{"reason", "degenerate"}, {"frame", row}});
  angle = unwrap(std::atan2(q.a21, q.a11) * DegreesPerRadian, angle);
  const double radians = angle / DegreesPerRadian;
  const double c = std::cos(radians), s = std::sin(radians);
  const double height = -s * q.a12 + c * q.a22;
  const double skew   = c * q.a12 + s * q.a22;
  if (std::abs(skew) > Orthogonal * std::max(1.0, std::hypot(q.a12, q.a22)))
    unrepresentable("Keeping the world placement would need shear.",
                    {{"reason", "shear_required"}, {"frame", row}});
  const TAffine linear(q.a11, q.a12, 0, q.a21, q.a22, 0);
  const TPointD position =
      TPointD(q.a13, q.a23) + linear * frame.center - offset * Stage::inch;
  return {position.x / Stage::inch, position.y / Stage::inch, 0, 0, angle,
          width / frame.scale,      height / frame.scale};
}

Samples bakeSamples(const Request& request, const Range& range,
                    const World& before) {
  TStageObject& object = *request.object;
  TStageObject& parent = *request.to.parent;
  TPointD center, offset;
  object.getCenterAndOffset(center, offset);
  double angle = object.getParam(TStageObject::T_Angle)->getValue(range.start);
  Samples samples(std::size(Baked));
  automationRefreshPlacements(*request.scene);
  for (int row = range.start; row <= range.end; ++row) {
    const int index      = row - range.start;
    const TPointD handle = automationHandlePosition(*request.scene, parent,
                                                    request.to.handle, row);
    const TAffine attachment =
        parent.getPlacement(row) * TTranslation(handle * Stage::inch);
    if (std::abs(attachment.det()) <= Degenerate)
      unrepresentable("The new parent collapses stage space at a frame.",
                      {{"reason", "singular_parent"}, {"frame", row}});
    const TPointD own = automationHandlePosition(*request.scene, object,
                                                 object.getHandle(), row);
    const Frame frame{attachment.inv() * before.placements[index],
                      (center + own) * Stage::inch,
                      object.getParam(TStageObject::T_Scale)->getValue(row)};
    auto values = solveFrame(frame, offset, angle, row);
    values[2]   = before.z[index] - parent.getZ(row);
    values[3]   = object.getSO(row) - parent.getSO(row);
    for (std::size_t channel = 0; channel < values.size(); ++channel)
      samples[channel].push_back(values[channel]);
  }
  return samples;
}

AnimationCurveEdit bakedCurve(const TDoubleParam& curve, const Range& range,
                              const std::vector<double>& values) {
  AnimationCurveEdit edit;
  edit.defaultValue = curve.getDefaultValue();
  for (int index = 0; index < curve.getKeyframeCount(); ++index) {
    const auto& key = curve.getKeyframe(index);
    if (key.m_frame < range.start || key.m_frame > range.end)
      edit.keys.push_back(key);
  }
  for (int row = range.start; row <= range.end; ++row) {
    TDoubleKeyframe key(row, values[row - range.start]);
    key.m_type          = range.type;
    key.m_isKeyframe    = true;
    key.m_linkedHandles = true;
    edit.keys.push_back(key);
  }
  std::sort(edit.keys.begin(), edit.keys.end());
  return edit;
}

// A channel is rewritten only when the bake changes one of its values, so a
// new parent at the same depth adds no z or stacking-order keys.
bool changes(const TDoubleParam& curve, const Range& range,
             const std::vector<double>& values) {
  for (int row = range.start; row <= range.end; ++row)
    if (curve.getValue(row) != values[row - range.start]) return true;
  return false;
}

QJsonObject preserveByBaking(Context& context, const Request& request,
                             const QJsonObject& policy) {
  const Range range = bakeRange(policy);
  requireBakeable(*request.object);
  std::vector<int> frames;
  for (int row = range.start; row <= range.end; ++row) frames.push_back(row);
  const World before    = sampleWorld(*request.scene, *request.object, frames);
  const Samples samples = bakeSamples(request, range, before);
  std::vector<Rewrite> rewrites;
  for (std::size_t index = 0; index < std::size(Baked); ++index) {
    const auto& curve = *request.object->getParam(Baked[index].native);
    if (!changes(curve, range, samples[index])) continue;
    addRewrite(rewrites, *request.object, Baked[index],
               bakedCurve(curve, range, samples[index]));
  }
  context.markMutationStarted();
  attach(request);
  for (const auto& rewrite : rewrites)
    applyAnimationCurveEdit(*request.object->getParam(rewrite.channel),
                            rewrite.edit);
  const Deviation deviation =
      compareWorld(*request.scene, *request.object, before);
  requireVerified(deviation);
  auto result =
      outcome("baked", "integer_frames_in_range", rewrites,
              static_cast<int>(frames.size() * rewrites.size()), deviation);
  result.insert("range", QJsonObject{{"start_frame", range.start},
                                     {"end_frame", range.end}});
  return result;
}

}  // namespace

QJsonObject automationWorldPolicySchema() {
  const QJsonObject frame{
      {"type", "integer"}, {"minimum", 0}, {"maximum", 1000000}};
  const QJsonObject exact = automationStrictSchema(
      {{"kind", QJsonObject{{"const", "exact_or_reject"}}}}, {"kind"});
  const QJsonObject bake = automationStrictSchema(
      {{"kind", QJsonObject{{"const", "bake"}}},
       {"start_frame", frame},
       {"end_frame", frame},
       {"new_key_interpolation",
        QJsonObject{{"type", "string"},
                    {"enum", QJsonArray{"linear", "constant"}}}}},
      {"kind", "start_frame", "end_frame", "new_key_interpolation"});
  return {{"oneOf", QJsonArray{exact, bake}}};
}

QJsonObject automationApplyParent(Context& context, Document& document,
                                  TStageObject& object, TStageObject& parent,
                                  const std::string& handle,
                                  const QJsonObject& params) {
  const bool world = params.value("placement_policy") == "preserve_world";
  if (!world && params.contains("world_policy"))
    throw Error("invalid_params",
                "world_policy applies only to placement_policy "
                "preserve_world.");
  const bool changed = object.getParent() != parent.getId() ||
                       object.getParentHandle() != handle;
  if (!changed)
    return world ? QJsonObject{{"method", "unchanged"}} : QJsonObject{};
  if (!world) {
    context.markMutationStarted();
    object.setParent(parent.getId());
    object.setParentHandle(handle);
    return {};
  }
  auto& stage = *document.scene->getXsheet()->getStageObjectTree();
  const Request request{document.scene.get(),
                        &object,
                        {stage.getStageObject(object.getParent(), false),
                         object.getParentHandle()},
                        {&parent, handle}};
  if (!request.from.parent)
    unrepresentable("The object has no parent whose space it leaves.",
                    {{"reason", "no_parent"}});
  const auto policy = params.value("world_policy").toObject();
  if (policy.value("kind") == "bake")
    return preserveByBaking(context, request, policy);
  return preserveExactly(context, request, params);
}

}  // namespace GroundzeroAutomation
