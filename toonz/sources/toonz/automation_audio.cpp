#include "automation_audio.h"
#include "automation.h"
#include "automation_drawing.h"
#include "automation_files.h"
#include "automation_path.h"

#include "tdoubleparam.h"
#include "tsound_io.h"
#include "toutputproperties.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsoundcolumn.h"
#include "toonz/txshsoundlevel.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <locale>
#include <map>
#include <memory>
#include <sstream>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxSources          = 64;
constexpr int MaxSoundColumns     = 32;
constexpr int MaxColumnClips      = 256;
constexpr int MaxDocumentClips    = 1024;
constexpr int MaxColumns          = 4096;
constexpr int MaxFrame            = 1000000;
constexpr qint64 MaxResultBytes   = 1024 * 1024;
constexpr double MaxSeconds       = 600;
constexpr qint64 MaxMixBytes      = 64LL * 1024 * 1024;
constexpr qint64 MaxMixMemory     = 256LL * 1024 * 1024;
constexpr int MaxMixArtifacts     = 128;
constexpr qint64 MaxArtifactBytes = 256LL * 1024 * 1024;
constexpr qint64 WavHeaderBytes   = 44;
const char* const FrameSampleRule =
    "trunc((frame - start_frame) * sample_rate / frame_rate)";

struct ArtifactBudget final {
  int files    = 0;
  qint64 bytes = 0;
};
ArtifactBudget mixdownArtifacts;

[[noreturn]] void invalid(const QString& message, QJsonObject details = {}) {
  throw Error("invalid_params", message, details);
}

double sceneRate(ToonzScene& scene) {
  return scene.getProperties()->getOutputProperties()->getFrameRate();
}

// The scene rate survives the native save only when its six-significant-digit
// stream form parses back to the same double.
bool stableRate(double fps) {
  if (!std::isfinite(fps) || fps < 1 || fps > 240) return false;
  std::ostringstream text;
  text.imbue(std::locale::classic());
  text << fps;
  std::istringstream input(text.str());
  input.imbue(std::locale::classic());
  double parsed = 0;
  input >> parsed;
  return parsed == fps;
}

void requireStableRate(ToonzScene& scene) {
  const double fps = sceneRate(scene);
  if (!stableRate(fps))
    throw Error("timing_conflict",
                "Audio timing requires a frame rate the native scene file "
                "stores exactly, such as 23.976, 24 or 29.97.",
                {{"frame_rate", fps}});
}

TXshSoundLevel* soundSource(TLevelSet* levels, int index) {
  TXshLevel* level = levels->getLevel(index);
  return level ? level->getSoundLevel() : nullptr;
}

std::vector<std::pair<int, TXshSoundLevel*>> soundSources(ToonzScene& scene) {
  std::vector<std::pair<int, TXshSoundLevel*>> result;
  TLevelSet* levels = scene.getLevelSet();
  for (int index = 0; index < levels->getLevelCount(); ++index)
    if (TXshSoundLevel* level = soundSource(levels, index))
      result.emplace_back(index, level);
  return result;
}

TXshSoundColumn* soundColumnAt(TXsheet* sheet, int index) {
  TXshColumn* column = sheet->getColumn(index);
  return column ? column->getSoundColumn() : nullptr;
}

std::vector<int> soundColumnIndexes(TXsheet* sheet) {
  std::vector<int> result;
  for (int index = 0; index < sheet->getColumnCount(); ++index)
    if (soundColumnAt(sheet, index)) result.push_back(index);
  return result;
}

TStageObject* columnObject(TXsheet* sheet, int index) {
  return sheet->getStageObject(TStageObjectId::ColumnId(index));
}

int columnPosition(TXsheet* sheet, const TXshColumn* column) {
  for (int index = 0; index < sheet->getColumnCount(); ++index)
    if (sheet->getColumn(index) == column) return index;
  throw Error("invalid_handle", "The column is no longer in this document.");
}

QString sampleTypeName(int type) {
  if (type == TSound::UINT) return "unsigned_int";
  if (type == TSound::FLOAT) return "float";
  return "signed_int";
}

QString codecName(int bits, int type) {
  if (type == TSound::FLOAT) return "pcm_f32";
  if (bits == 8) return type == TSound::UINT ? "pcm_u8" : "pcm_s8";
  return bits == 16 ? "pcm_s16" : "pcm_s24";
}

QJsonObject formatJson(const TSoundTrackFormat& format) {
  return {{"bits_per_sample", format.m_bitPerSample},
          {"channels", format.m_channelCount},
          {"codec", codecName(format.m_bitPerSample, format.m_sampleType)},
          {"sample_rate", double(format.m_sampleRate)},
          {"sample_type", sampleTypeName(format.m_sampleType)}};
}

bool admittedFormat(const TSoundTrackFormat& format) {
  const int bits = format.m_bitPerSample;
  const int type = format.m_sampleType;
  const bool known =
      (type == TSound::FLOAT && bits == 32) ||
      (type == TSound::UINT && bits == 8) ||
      (type == TSound::INT && (bits == 8 || bits == 16 || bits == 24));
  return known && format.m_channelCount >= 1 && format.m_channelCount <= 2 &&
         format.m_sampleRate >= 8000 && format.m_sampleRate <= 96000;
}

int volumePermille(const TXshSoundColumn& column) {
  return int(std::lround(column.getVolume() * 1000));
}

QString gainDigest(const TDoubleParam& curve) {
  QJsonArray keys;
  for (int index = 0; index < curve.getKeyframeCount(); ++index) {
    const TDoubleKeyframe& key = curve.getKeyframe(index);
    keys.append(QJsonArray{key.m_frame, key.m_value, int(key.m_type)});
  }
  const QJsonObject value{{"default", curve.getDefaultValue()}, {"keys", keys}};
  return QString::fromLatin1(
      QCryptographicHash::hash(
          QJsonDocument(value).toJson(QJsonDocument::Compact),
          QCryptographicHash::Sha256)
          .toHex());
}

[[noreturn]] void outsideProfile(const QString& message,
                                 QJsonObject details = {}) {
  throw Error("unsupported_level", message, details);
}

void requireSourceProfile(ToonzScene& scene, TXshSoundLevel& level) {
  const TSoundTrackP track = level.getSoundTrack();
  const QJsonObject name{{"name", QString::fromStdWString(level.getName())}};
  if (!track || track->getSampleCount() < 1 ||
      !admittedFormat(track->getFormat()))
    outsideProfile("A sound source has no decoded PCM in the admitted format.",
                   name);
  if (automationSoundBytes(level) > 128LL * 1024 * 1024 ||
      track->getDuration() > MaxSeconds)
    outsideProfile("A sound source exceeds 600 seconds or 128 MiB.", name);
  if (level.getFrameRate() != sceneRate(scene))
    outsideProfile("A sound source uses a frame rate other than the scene's.",
                   name);
}

void requireClipProfile(ToonzScene& scene, const ColumnLevel& clip,
                        int previousEnd) {
  TXshSoundLevel* level = clip.getSoundLevel();
  if (!level || !scene.getLevelSet()->hasLevel(level->getName()) ||
      scene.getLevelSet()->getLevel(level->getName()) != level)
    outsideProfile("A sound clip references a source outside the cast.");
  const int start = clip.getVisibleStartFrame();
  if (clip.getStartOffset() < 0 || clip.getEndOffset() < 0 ||
      clip.getVisibleFrameCount() < 1 || start < 0 ||
      clip.getVisibleEndFrame() > MaxFrame || start <= previousEnd)
    outsideProfile("A sound clip lies outside its source or the timeline.",
                   {{"timeline_start_frame", start}});
}

void requireColumnProfile(ToonzScene& scene, TXshSoundColumn& column,
                          int& clips) {
  if (column.getClipCount() > MaxColumnClips)
    outsideProfile("A sound column exceeds 256 clips.");
  clips += column.getClipCount();
  const double volume = column.getVolume();
  if (volume != volumePermille(column) / 1000.0)
    outsideProfile("A sound column volume is not a whole per-mille value.");
  if (column.getVolumeCurve()->hasKeyframes() ||
      column.getVolumeCurve()->getDefaultValue() != 1.0)
    outsideProfile("Animated sound column gain is not in this profile.");
  int previousEnd = -1;
  for (int index = 0; index < column.getClipCount(); ++index) {
    requireClipProfile(scene, *column.getClip(index), previousEnd);
    previousEnd = column.getClip(index)->getVisibleEndFrame();
  }
}

void requireSoundProfile(ToonzScene& scene) {
  const auto sources = soundSources(scene);
  if (int(sources.size()) > MaxSources)
    outsideProfile("The document exceeds 64 sound sources.");
  for (const auto& source : sources)
    requireSourceProfile(scene, *source.second);
  TXsheet* sheet     = scene.getXsheet();
  const auto columns = soundColumnIndexes(sheet);
  if (int(columns.size()) > MaxSoundColumns)
    outsideProfile("The document exceeds 32 sound columns.");
  int clips = 0;
  for (int index : columns)
    requireColumnProfile(scene, *soundColumnAt(sheet, index), clips);
  if (clips > MaxDocumentClips)
    outsideProfile("The document exceeds 1024 sound clips.");
}

[[noreturn]] void unsavable(int position, const QString& message) {
  throw Error("unsupported_column", message, {{"position", position}});
}

// Native scene files omit the status of an empty sound column and every
// trailing column that the native code considers empty.
void requireSavableColumns(ToonzScene& scene) {
  TXsheet* sheet  = scene.getXsheet();
  const int saved = sheet->getFirstFreeColumnIndex();
  for (int index : soundColumnIndexes(sheet)) {
    TXshSoundColumn* column = soundColumnAt(sheet, index);
    const bool clipless     = column->getClipCount() == 0;
    if (clipless && (column->isLocked() || !column->isPreviewVisible() ||
                     !column->isCamstandVisible()))
      unsavable(index,
                "Unlock and unmute the empty sound column, or delete it, "
                "before saving; native scenes do not store its state.");
    if (!clipless && index >= saved)
      unsavable(index,
                "A last sound column whose clips span one frame is not "
                "saved natively; lengthen the clip or add a later column.");
  }
}

QJsonObject clipKey(Document& document, TXshSoundColumn* column,
                    const ColumnLevel& clip) {
  return {{"column_id", document.idForColumn(column)},
          {"timeline_start_frame", clip.getVisibleStartFrame()}};
}

QJsonObject clipJson(Document& document, TXshSoundColumn* column,
                     const ColumnLevel& clip) {
  TXshSoundLevel* level    = clip.getSoundLevel();
  const TSoundTrackP track = level->getSoundTrack();
  const double perFrame    = track->getSampleRate() / level->getFrameRate();
  const int source         = clip.getStartOffset();
  const int count          = clip.getVisibleFrameCount();
  const qint64 last        = qint64((source + count) * perFrame);
  return {{"column_id", document.idForColumn(column)},
          {"frame_count", count},
          {"level_id", document.idForLevel(level)},
          {"native_start_frame", clip.getStartFrame()},
          {"source_sample_end_inclusive",
           double(std::min<qint64>(last, track->getSampleCount() - 1))},
          {"source_sample_start", double(qint64(source * perFrame))},
          {"source_start_frame", source},
          {"timeline_end_frame", clip.getVisibleEndFrame()},
          {"timeline_start_frame", clip.getVisibleStartFrame()}};
}

QJsonObject columnJson(Document& document, TXshSoundColumn* column) {
  TXsheet* sheet       = document.scene->getXsheet();
  const int position   = columnPosition(sheet, column);
  TStageObject* object = columnObject(sheet, position);
  QJsonArray clips;
  for (int index = 0; index < column->getClipCount(); ++index)
    clips.append(clipJson(document, column, *column->getClip(index)));
  return {{"audible", column->isPreviewVisible()},
          {"clips", clips},
          {"column_id", document.idForColumn(column)},
          {"gain_default", 1},
          {"gain_keys", QJsonArray()},
          {"locked", column->isLocked()},
          {"name", QString::fromStdString(object->getName())},
          {"object_id", document.idForObject(object)},
          {"position", position},
          {"volume_permille", volumePermille(*column)}};
}

QByteArray readSoundFile(const QString& path) {
  const QFileInfo info(path);
  if (!info.isFile() || info.isSymLink())
    throw Error("invalid_asset", "The audio source is not a regular file.");
  if (info.size() > AutomationAudioFileBytes)
    throw Error("resource_limit", "Audio source files are limited to 160 MiB.");
  QFile file;
  const OpenResult opened = openApprovedFile(file, path);
  if (opened == OpenResult::NotRegular)
    throw Error("invalid_asset", "The audio source is not a regular file.");
  if (opened != OpenResult::Opened)
    throw Error("io_error", "Cannot open the audio source file.");
  const QByteArray bytes = file.read(AutomationAudioFileBytes + 1);
  if (file.error() != QFileDevice::NoError || bytes.size() != info.size())
    throw Error("io_error", "Cannot read the complete audio source file.");
  return bytes;
}

QString sha256(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool sameTrack(const AutomationAudioInfo& info, const TSoundTrackP& track) {
  if (!track) return false;
  const TSoundTrackFormat format = track->getFormat();
  return format.m_sampleRate == info.sampleRate &&
         format.m_channelCount == info.channels &&
         format.m_bitPerSample == info.bitsPerSample &&
         format.m_sampleType == info.sampleType &&
         track->getSampleCount() == info.sampleCount &&
         automationTrackSha256(track) == info.pcmSha256;
}

QJsonObject metadata(const QByteArray& bytes, const AutomationAudioInfo& info) {
  return {{"container", info.container},
          {"size_bytes", double(bytes.size())},
          {"source_sha256", sha256(bytes)},
          {"pcm_sha256", QString::fromLatin1(info.pcmSha256)}};
}

// Verify a source's worker-owned file against its decoded PCM.
QJsonObject verifiedSource(ToonzScene& scene, TXshSoundLevel& level,
                           QByteArray* content = nullptr) {
  const QString path = QString::fromStdWString(
      scene.decodeFilePath(level.getPath()).getWideString());
  const QByteArray bytes         = readSoundFile(path);
  const AutomationAudioInfo info = parseAutomationAudio(bytes);
  if (!sameTrack(info, level.getSoundTrack()))
    throw Error("invalid_asset",
                "A sound source file changed after it was imported.",
                {{"name", QString::fromStdWString(level.getName())}});
  if (content) *content = bytes;
  return metadata(bytes, info);
}

const QJsonObject& sourceMetadata(Document& document, TXshSoundLevel* level) {
  auto found = document.soundSources.find(level);
  if (found == document.soundSources.end())
    found = document.soundSources.insert(
        level, verifiedSource(*document.scene, *level));
  return found.value();
}

int clipCount(ToonzScene& scene, const TXshSoundLevel* level) {
  int count      = 0;
  TXsheet* sheet = scene.getXsheet();
  for (int index : soundColumnIndexes(sheet)) {
    TXshSoundColumn* column = soundColumnAt(sheet, index);
    for (int clip = 0; clip < column->getClipCount(); ++clip)
      if (column->getClip(clip)->getSoundLevel() == level) ++count;
  }
  return count;
}

QJsonObject sourceJson(Document& document, TXshSoundLevel* level) {
  const QJsonObject& meta  = sourceMetadata(document, level);
  const TSoundTrackP track = level->getSoundTrack();
  const double fps         = level->getFrameRate();
  const double perFrame    = track->getSampleRate() / fps;
  const int frames         = level->getFrameCount();
  const qint64 count       = track->getSampleCount();
  const QString container  = meta.value("container").toString();
  return {{"clip_count", clipCount(*document.scene, level)},
          {"container", container},
          {"decoded_by", container == "wav" ? "native_wav" : "native_aiff"},
          {"duration_seconds", double(count) / track->getSampleRate()},
          {"format", formatJson(track->getFormat())},
          {"frame_rate", fps},
          {"level_id", document.idForLevel(level)},
          {"lossy_source", false},
          {"name", QString::fromStdWString(level->getName())},
          {"native_frame_count", frames},
          {"pcm_sha256", meta.value("pcm_sha256")},
          {"sample_count", double(count)},
          {"samples_per_frame", perFrame},
          {"size_bytes", meta.value("size_bytes")},
          {"source_sha256", meta.value("source_sha256")},
          {"unreachable_tail_samples",
           double(std::max<qint64>(0, count - qint64(frames * perFrame)))}};
}

// ---------------------------------------------------------------------------
// Mix planning, as TXsheet::makeSound selects and mixes columns.

struct MixPlan final {
  std::vector<TXshSoundColumn*> columns;
  QJsonArray included;
  QJsonArray excluded;
  TSoundTrackFormat source;
  TSoundTrackFormat output;
};

TSoundTrackFormat promoted(TSoundTrackFormat format) {
  if (format.m_bitPerSample == 8) {
    format.m_bitPerSample = 16;
    format.m_sampleType   = TSound::INT;
  }
  return format;
}

QString exclusion(const MixPlan& plan, TXshSoundColumn* column) {
  if (column->isEmpty()) return "empty";
  if (!column->isPreviewVisible()) return "muted";
  if (!plan.columns.empty() && column->getVolume() == 0) return "zero_volume";
  return QString();
}

MixPlan planMix(Document& document) {
  MixPlan plan;
  TXsheet* sheet = document.scene->getXsheet();
  for (int index : soundColumnIndexes(sheet)) {
    TXshSoundColumn* column = soundColumnAt(sheet, index);
    const QString id        = document.idForColumn(column);
    const QString reason    = exclusion(plan, column);
    // The native mix skips a later zero-volume column itself.
    if (reason.isEmpty() || reason == "zero_volume")
      plan.columns.push_back(column);
    if (reason.isEmpty())
      plan.included.append(id);
    else
      plan.excluded.append(QJsonObject{{"column_id", id}, {"reason", reason}});
  }
  if (!plan.columns.empty()) {
    plan.source = plan.columns.front()
                      ->getClip(0)
                      ->getSoundLevel()
                      ->getSoundTrack()
                      ->getFormat();
    plan.output = promoted(plan.source);
  }
  return plan;
}

QJsonObject mixJson(const MixPlan& plan) {
  return {
      {"audible_columns", plan.included},
      {"excluded_columns", plan.excluded},
      {"format", plan.columns.empty() ? QJsonValue()
                                      : QJsonValue(formatJson(plan.output))},
      {"format_rule", "first_audible_column_first_clip"}};
}

// ---------------------------------------------------------------------------
// Handles and arguments.

Document& currentDocument(Context& context, const QJsonObject& params) {
  Document& document = context.document(requiredString(params, "document_id"));
  if (document.revisionText() != requiredString(params, "expected_revision"))
    throw Error(
        "revision_conflict",
        "The document changed; inspect its current revision first.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
  requireSoundProfile(*document.scene);
  return document;
}

QString nameParam(const QJsonObject& params) {
  const QString name = requiredString(params, "name");
  if (name.contains(QChar::Null))
    invalid("Names cannot contain a null character.");
  return name;
}

TXshSoundColumn* soundColumn(Document& document, const QString& id) {
  TXshSoundColumn* column = document.column(id)->getSoundColumn();
  if (!column)
    throw Error("incompatible_column", "The column is not a sound column.",
                {{"column_id", id}});
  return column;
}

TXshSoundLevel* soundLevel(Document& document, const QString& id) {
  TXshSoundLevel* level = document.level(id)->getSoundLevel();
  if (!level)
    invalid("level_id must name an imported sound source.", {{"level_id", id}});
  return level;
}

void requireEditable(Document& document, TXshSoundColumn* column) {
  TXsheet* sheet = document.scene->getXsheet();
  if (column->isLocked() ||
      columnObject(sheet, columnPosition(sheet, column))->isLocked())
    throw Error("locked", "Unlock the sound column before editing it.",
                {{"column_id", document.idForColumn(column)}});
}

struct Placement final {
  int timeline = 0;
  int source   = 0;
  int count    = 0;
  int end() const { return timeline + count - 1; }
  bool operator==(const Placement& other) const {
    return timeline == other.timeline && source == other.source &&
           count == other.count;
  }
};

Placement readPlacement(const QJsonObject& params) {
  const QJsonObject value = params.value("placement").toObject();
  return {value.value("timeline_start_frame").toInt(),
          value.value("source_start_frame").toInt(),
          value.value("frame_count").toInt()};
}

Placement placementOf(const ColumnLevel& clip) {
  return {clip.getVisibleStartFrame(), clip.getStartOffset(),
          clip.getVisibleFrameCount()};
}

const ColumnLevel* findClip(TXshSoundColumn* column, int timeline) {
  for (int index = 0; index < column->getClipCount(); ++index)
    if (column->getClip(index)->getVisibleStartFrame() == timeline)
      return column->getClip(index);
  return nullptr;
}

struct ClipTarget final {
  TXshSoundColumn* column;
  const ColumnLevel* clip;
};

ClipTarget readClip(Document& document, const QJsonObject& params) {
  const QJsonObject key = params.value("clip").toObject();
  TXshSoundColumn* column =
      soundColumn(document, requiredString(key, "column_id"));
  const int timeline      = key.value("timeline_start_frame").toInt();
  const ColumnLevel* clip = findClip(column, timeline);
  if (!clip)
    throw Error("not_found", "No clip starts at that frame in the column.",
                {{"column_id", key.value("column_id")},
                 {"timeline_start_frame", timeline}});
  return {column, clip};
}

void requirePlacement(TXshSoundLevel* level, const Placement& placement) {
  const int frames = level->getFrameCount();
  if (placement.source + placement.count > frames)
    invalid(
        "source_start_frame + frame_count exceeds the source's native "
        "frame count.",
        {{"native_frame_count", frames}});
  if (qint64(placement.timeline) + placement.count - 1 > MaxFrame)
    invalid("The clip must end at or before frame 1000000.");
}

void requireNoOverlap(Document& document, TXshSoundColumn* column,
                      const Placement& placement, const ColumnLevel* ignored) {
  QJsonArray conflicts;
  for (int index = 0; index < column->getClipCount(); ++index) {
    const ColumnLevel* clip = column->getClip(index);
    if (clip == ignored) continue;
    if (clip->getVisibleStartFrame() <= placement.end() &&
        clip->getVisibleEndFrame() >= placement.timeline)
      conflicts.append(clipKey(document, column, *clip));
  }
  if (!conflicts.isEmpty())
    throw Error("audio_overlap",
                "The clip would overlap other clips in the column.",
                {{"conflicts", conflicts}});
}

// Native cell edits join a clip with an adjacent clip that continues the same
// source; that would silently change clip identities.
void requireNoJoin(Document& document, TXshSoundColumn* column,
                   TXshSoundLevel* level, const Placement& placement,
                   const ColumnLevel* ignored) {
  for (int index = 0; index < column->getClipCount(); ++index) {
    const ColumnLevel* clip = column->getClip(index);
    if (clip == ignored || clip->getSoundLevel() != level ||
        clip->getStartFrame() != placement.timeline - placement.source)
      continue;
    if (clip->getVisibleEndFrame() + 1 == placement.timeline ||
        clip->getVisibleStartFrame() == placement.end() + 1)
      invalid(
          "The clip would continue an adjacent clip of the same source; "
          "update that clip instead.",
          {{"reason", "contiguous_clip"},
           {"clip", clipKey(document, column, *clip)}});
  }
}

void requireClipCapacity(ToonzScene& scene, TXshSoundColumn* column) {
  int total      = 0;
  TXsheet* sheet = scene.getXsheet();
  for (int index : soundColumnIndexes(sheet))
    total += soundColumnAt(sheet, index)->getClipCount();
  if (column->getClipCount() >= MaxColumnClips || total >= MaxDocumentClips)
    throw Error("resource_limit",
                "Sound columns admit 256 clips each and 1024 per document.");
}

void writeCells(TXsheet* sheet, TXshSoundColumn* column, TXshSoundLevel* level,
                const Placement& placement) {
  std::vector<TXshCell> cells;
  cells.reserve(placement.count);
  for (int frame = 0; frame < placement.count; ++frame)
    cells.emplace_back(level, TFrameId(placement.source + frame));
  column->setCells(placement.timeline, placement.count, cells.data());
  sheet->updateFrameCount();
}

const ColumnLevel& requireWritten(TXshSoundColumn* column,
                                  TXshSoundLevel* level,
                                  const Placement& placement) {
  const ColumnLevel* clip = findClip(column, placement.timeline);
  if (!clip || clip->getSoundLevel() != level ||
      !(placementOf(*clip) == placement) ||
      clip->getEndOffset() !=
          level->getFrameCount() - placement.source - placement.count)
    throw Error("native_error",
                "The native sound column did not store the exact clip.");
  return *clip;
}

// ---------------------------------------------------------------------------
// Tools.

QJsonValue inspectAudio(Context& context, const QJsonObject& params) {
  Document& document = currentDocument(context, params);
  ToonzScene& scene  = *document.scene;
  QJsonArray sources, columns;
  for (const auto& source : soundSources(scene))
    sources.append(sourceJson(document, source.second));
  TXsheet* sheet = scene.getXsheet();
  for (int index : soundColumnIndexes(sheet))
    columns.append(columnJson(document, soundColumnAt(sheet, index)));
  const double fps = sceneRate(scene);
  QJsonObject result{{"columns", columns},
                     {"document_id", document.id},
                     {"frame_rate", fps},
                     {"frame_rate_native_stable", stableRate(fps)},
                     {"mix", mixJson(planMix(document))},
                     {"older_reader_compatible", true},
                     {"revision", document.revisionText()},
                     {"scene_frame_count", scene.getFrameCount()},
                     {"sources", sources}};
  if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() >
      MaxResultBytes)
    throw Error("resource_limit", "Audio inspection exceeds 1 MiB.");
  return result;
}

QString privateCopyPath(Context& context, const QString& container) {
  const QString directory =
      context.resolvePath(context.privateDirectory("sounds"), true);
  if (!QDir().mkpath(directory))
    throw Error("io_error", "Cannot create the workspace sound directory.");
  return QDir(directory).filePath(
      QUuid::createUuid().toString(QUuid::WithoutBraces) +
      (container == "wav" ? ".wav" : ".aif"));
}

void writeFile(const QString& path, const QByteArray& bytes) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit())
    throw Error("io_error", "Cannot write a private sound file.");
}

TSoundTrackP decodeNative(const QString& path) {
  TSoundTrackP track;
  try {
    TSoundTrackReader::load(TFilePath(path.toStdWString()), track);
  } catch (const TException& error) {
    throw Error("native_error", QString::fromStdWString(error.getMessage()));
  }
  return track;
}

struct ImportedCopy final {
  QString path;
  TSoundTrackP track;
  QJsonObject metadata;
};

ImportedCopy importCopy(Context& context, const QJsonObject& params,
                        ToonzScene& scene) {
  const QString source =
      context.resolvePath(requiredString(params, "path"), false);
  const QByteArray bytes         = readSoundFile(source);
  const AutomationAudioInfo info = parseAutomationAudio(bytes);
  requireAssetGrowth(scene, info.pcmBytes, 0, 1);
  ImportedCopy copy{
      privateCopyPath(context, info.container), {}, metadata(bytes, info)};
  writeFile(copy.path, bytes);
  try {
    copy.track = decodeNative(copy.path);
    if (!sameTrack(info, copy.track))
      throw Error("native_error",
                  "The native decoder disagrees with the validated source.");
  } catch (...) {
    QFile::remove(copy.path);
    throw;
  }
  return copy;
}

QJsonValue importAudio(Context& context, const QJsonObject& params) {
  Document& document = currentDocument(context, params);
  ToonzScene& scene  = *document.scene;
  requireStableRate(scene);
  const QString name = nameParam(params);
  if (scene.getLevelSet()->hasLevel(name.toStdWString()))
    throw Error("name_conflict", "A level with that name already exists.");
  if (int(soundSources(scene).size()) >= MaxSources)
    throw Error("resource_limit",
                "The document admits at most 64 sound sources.");
  ImportedCopy copy = importCopy(context, params, scene);
  context.markMutationStarted();
  auto* level = new TXshSoundLevel(name.toStdWString());
  level->setType(SND_XSHLEVEL);
  level->setScene(&scene);
  level->setPath(TFilePath(copy.path.toStdWString()));
  level->setSoundTrack(copy.track);
  if (!scene.getLevelSet()->insertLevel(level))
    throw Error("native_error", "Could not add the sound source to the cast.");
  document.soundSources.insert(level, copy.metadata);
  return QJsonObject{{"changed", true},
                     {"source", sourceJson(document, level)}};
}

QJsonValue createColumn(Context& context, const QJsonObject& params) {
  Document& document = currentDocument(context, params);
  const QString name = nameParam(params);
  TXsheet* sheet     = document.scene->getXsheet();
  const int index    = sheet->getColumnCount();
  if (int(soundColumnIndexes(sheet).size()) >= MaxSoundColumns ||
      index >= MaxColumns)
    throw Error("resource_limit",
                "The document admits 32 sound columns and 4096 columns.");
  requireAutomationPathObjectGrowth(*document.scene);
  auto column = std::make_unique<TXshSoundColumn>();
  column->setVolume(params.value("volume_permille").toInt() / 1000.0);
  column->setPreviewVisible(true);
  column->setCamstandVisible(true);
  context.markMutationStarted();
  TXshSoundColumn* created = column.release();
  sheet->insertColumn(index, created);
  columnObject(sheet, index)->setName(name.toStdString());
  return QJsonObject{{"changed", true},
                     {"column", columnJson(document, created)}};
}

QJsonValue setVolume(Context& context, const QJsonObject& params) {
  Document& document      = currentDocument(context, params);
  const QString id        = requiredString(params, "column_id");
  TXshSoundColumn* column = soundColumn(document, id);
  requireEditable(document, column);
  const int permille = params.value("volume_permille").toInt();
  const bool changed = permille != volumePermille(*column);
  if (changed) {
    context.markMutationStarted();
    column->setVolume(permille / 1000.0);
  }
  return QJsonObject{{"changed", changed},
                     {"column_id", id},
                     {"volume_permille", volumePermille(*column)}};
}

QJsonValue placeClip(Context& context, const QJsonObject& params) {
  Document& document = currentDocument(context, params);
  requireStableRate(*document.scene);
  TXshSoundColumn* column =
      soundColumn(document, requiredString(params, "column_id"));
  TXshSoundLevel* level =
      soundLevel(document, requiredString(params, "level_id"));
  requireEditable(document, column);
  const Placement placement = readPlacement(params);
  requirePlacement(level, placement);
  requireClipCapacity(*document.scene, column);
  requireNoOverlap(document, column, placement, nullptr);
  requireNoJoin(document, column, level, placement, nullptr);
  context.markMutationStarted();
  writeCells(document.scene->getXsheet(), column, level, placement);
  const ColumnLevel& clip = requireWritten(column, level, placement);
  return QJsonObject{{"changed", true},
                     {"clip", clipJson(document, column, clip)}};
}

QJsonValue updateClip(Context& context, const QJsonObject& params) {
  Document& document = currentDocument(context, params);
  requireStableRate(*document.scene);
  const ClipTarget target   = readClip(document, params);
  TXshSoundLevel* level     = target.clip->getSoundLevel();
  const Placement placement = readPlacement(params);
  requireEditable(document, target.column);
  requirePlacement(level, placement);
  const Placement current = placementOf(*target.clip);
  if (placement == current)
    return QJsonObject{
        {"changed", false},
        {"clip", clipJson(document, target.column, *target.clip)}};
  requireNoOverlap(document, target.column, placement, target.clip);
  requireNoJoin(document, target.column, level, placement, target.clip);
  TXsheet* sheet = document.scene->getXsheet();
  context.markMutationStarted();
  target.column->clearCells(current.timeline, current.count);
  writeCells(sheet, target.column, level, placement);
  const ColumnLevel& clip = requireWritten(target.column, level, placement);
  return QJsonObject{{"changed", true},
                     {"clip", clipJson(document, target.column, clip)}};
}

QJsonValue removeClip(Context& context, const QJsonObject& params) {
  Document& document      = currentDocument(context, params);
  const ClipTarget target = readClip(document, params);
  requireEditable(document, target.column);
  const QJsonObject removed = clipJson(document, target.column, *target.clip);
  const Placement current   = placementOf(*target.clip);
  const int count           = target.column->getClipCount();
  context.markMutationStarted();
  target.column->clearCells(current.timeline, current.count);
  document.scene->getXsheet()->updateFrameCount();
  if (target.column->getClipCount() != count - 1 ||
      findClip(target.column, current.timeline))
    throw Error("native_error",
                "The native sound column did not remove exactly the clip.");
  return QJsonObject{{"changed", true}, {"removed", removed}};
}

// ---------------------------------------------------------------------------
// Mixdown.

qint64 mixSamples(int from, int to, double fps, double rate) {
  const double duration = double(to - from) / fps;
  const double samples  = duration * rate;
  qint64 count          = qint64(samples);
  if (double(count) < samples - 1e-8) ++count;
  return count;
}

qint64 blockBytes(const TSoundTrackFormat& format) {
  return qint64(format.m_channelCount) * format.m_bitPerSample / 8;
}

qint64 conversionBytes(const MixPlan& plan) {
  qint64 largest = 0;
  for (TXshSoundColumn* column : plan.columns)
    for (int index = 0; index < column->getClipCount(); ++index) {
      const TSoundTrackP track =
          column->getClip(index)->getSoundLevel()->getSoundTrack();
      if (track->getFormat() == plan.source) continue;
      const double samples =
          track->getSampleCount() *
          (plan.source.m_sampleRate / double(track->getSampleRate()));
      largest = std::max(largest, qint64(samples) * blockBytes(plan.source));
    }
  return largest;
}

struct MixRequest final {
  int start      = 0;
  int end        = 0;
  double fps     = 0;
  qint64 samples = 0;
  QString output;
};

void requireMixBudget(const MixPlan& plan, const MixRequest& request) {
  if ((request.end + 1.0 - request.start) / request.fps > MaxSeconds)
    throw Error("resource_limit", "Mixdowns are limited to 600 seconds.");
  const qint64 bytes = request.samples * blockBytes(plan.output);
  if (bytes > MaxMixBytes)
    throw Error("resource_limit", "Mixdown PCM is limited to 64 MiB.",
                {{"pcm_bytes", double(bytes)}});
  const qint64 working = request.samples * blockBytes(plan.source);
  const qint64 memory =
      std::max(3 * working + 2 * conversionBytes(plan) + bytes, 3 * bytes);
  if (memory > MaxMixMemory)
    throw Error("resource_limit",
                "The mixdown's estimated peak memory exceeds 256 MiB.",
                {{"estimate_bytes", double(memory)}});
  if (mixdownArtifacts.files >= MaxMixArtifacts ||
      mixdownArtifacts.bytes + bytes + WavHeaderBytes > MaxArtifactBytes)
    throw Error("resource_limit",
                "This worker has reached its 128-file, 256 MiB mixdown limit.");
}

QString mixOutputPath(Context& context, const QJsonObject& params) {
  const QString path =
      context.resolvePath(requiredString(params, "output_path"), true);
  const QFileInfo target(path);
  if (target.suffix() != "wav")
    throw Error("invalid_path", "Mixdown output must be a new .wav file.");
  if (target.exists() || target.isSymLink())
    throw Error(
        "already_exists",
        "Choose a new output path; existing files are not overwritten.");
  if (!QDir(target.absolutePath()).exists())
    throw Error("missing_parent", "Create the output directory first.");
  if (!QFileInfo(target.absolutePath()).isWritable())
    throw Error("io_error", "The output directory is not writable.");
  return path;
}

struct MixLevels final {
  double peak      = 0;
  qint64 fullScale = 0;
};

MixLevels measure(const TSoundTrackP& track) {
  MixLevels levels;
  const qint64 values =
      qint64(track->getSampleCount()) * track->getChannelCount();
  const UCHAR* raw = track->getRawData();
  const int bits   = track->getBitPerSample();
  for (qint64 index = 0; index < values; ++index) {
    double value = 0;
    bool full    = false;
    if (track->getSampleType() == TSound::FLOAT) {
      float sample = 0;
      std::memcpy(&sample, raw + index * 4, 4);
      value = std::fabs(sample);
      full  = value >= 1.0;
    } else if (bits == 16) {
      qint16 sample = 0;
      std::memcpy(&sample, raw + index * 2, 2);
      value = std::abs(int(sample)) / 32768.0;
      full  = sample == -32768 || sample == 32767;
    } else {
      const UCHAR* bytes = raw + index * 3;
      int sample         = bytes[0] | bytes[1] << 8 | bytes[2] << 16;
      if (sample & 0x800000) sample -= 0x1000000;
      value = std::abs(sample) / 8388608.0;
      full  = sample == -8388608 || sample == 8388607;
    }
    levels.peak = std::max(levels.peak, value);
    if (full) ++levels.fullScale;
  }
  return levels;
}

// Write through the native WAV writer, then admit the file only when the
// bounded parser and the native reader both return the in-memory PCM.
QByteArray stageMix(const TSoundTrackP& mix, const QString& staging) {
  try {
    TSoundTrackWriter::save(TFilePath(staging.toStdWString()), mix);
  } catch (const TException& error) {
    throw Error("io_error", QString::fromStdWString(error.getMessage()));
  }
  const QByteArray bytes         = readSoundFile(staging);
  const AutomationAudioInfo info = parseAutomationAudio(bytes);
  if (info.container != "wav" || !sameTrack(info, mix) ||
      !sameTrack(info, decodeNative(staging)))
    throw Error("io_error",
                "The written mixdown does not decode to the computed mix.");
  return bytes;
}

MixRequest readMixRequest(Context& context, Document& document,
                          const QJsonObject& params, const MixPlan& plan) {
  MixRequest request;
  request.start = params.value("start_frame").toInt();
  request.end   = params.value("end_frame").toInt();
  request.fps   = sceneRate(*document.scene);
  if (request.end < request.start)
    invalid("end_frame must be at least start_frame.");
  if (plan.included.isEmpty())
    invalid("No sound column is audible.", {{"audible_columns", 0}});
  request.samples = mixSamples(request.start, request.end + 1, request.fps,
                               plan.source.m_sampleRate);
  requireMixBudget(plan, request);
  request.output = mixOutputPath(context, params);
  return request;
}

TSoundTrackP computeMix(const MixPlan& plan, const MixRequest& request) {
  const TSoundTrackP mix = plan.columns.front()->mixingTogetherOffline(
      plan.columns, request.start, request.end + 1, request.fps);
  if (!mix || !(mix->getFormat() == plan.output) ||
      mix->getSampleCount() != request.samples)
    throw Error("native_error",
                "The native offline mix differs from its planned format.");
  return mix;
}

QJsonObject mixResult(const MixPlan& plan, const MixRequest& request,
                      const TSoundTrackP& mix, const QByteArray& bytes) {
  const MixLevels levels = measure(mix);
  return {{"container", "wav"},
          {"end_frame", request.end},
          {"excluded_columns", plan.excluded},
          {"format", formatJson(plan.output)},
          {"format_rule", "first_audible_column_first_clip"},
          {"frame_rate", request.fps},
          {"frame_sample_rule", FrameSampleRule},
          {"full_scale_sample_count", double(levels.fullScale)},
          {"included_columns", plan.included},
          {"output_path", request.output},
          {"pcm_sha256", QString::fromLatin1(automationTrackSha256(mix))},
          {"peak", levels.peak},
          {"sample_count", double(mix->getSampleCount())},
          {"sha256", sha256(bytes)},
          {"silent", levels.peak == 0},
          {"size_bytes", double(bytes.size())},
          {"start_frame", request.start}};
}

QJsonValue mixdown(Context& context, const QJsonObject& params) {
  Document& document = currentDocument(context, params);
  requireStableRate(*document.scene);
  const MixPlan plan       = planMix(document);
  const MixRequest request = readMixRequest(context, document, params, plan);
  const TSoundTrackP mix   = computeMix(plan, request);
  PrivateDirectory staging(context.runtimeDirectory + "/mixdown-XXXXXX");
  if (!staging.isValid())
    throw Error("io_error", "Cannot create private mixdown staging storage.");
  const QString staged   = staging.filePath("mix.wav");
  const QByteArray bytes = stageMix(mix, staged);
  publishStaged(context, staged, request.output, "io_error",
                "Cannot publish the verified mixdown file.");
  ++mixdownArtifacts.files;
  mixdownArtifacts.bytes += bytes.size();
  return mixResult(plan, request, mix, bytes);
}

// ---------------------------------------------------------------------------
// Catalog.

QJsonObject stringSchema(int maximum) {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
}

QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject strictObject(const QJsonObject& properties) {
  QJsonArray required;
  for (auto field = properties.begin(); field != properties.end(); ++field)
    required.append(field.key());
  return {{"type", "object"},
          {"properties", properties},
          {"required", required},
          {"additionalProperties", false}};
}

QJsonObject placementSchema() {
  return strictObject({{"frame_count", integerSchema(1, 144000)},
                       {"source_start_frame", integerSchema(0, 143999)},
                       {"timeline_start_frame", integerSchema(0, MaxFrame)}});
}

QJsonObject clipKeySchema() {
  return strictObject({{"column_id", stringSchema(128)},
                       {"timeline_start_frame", integerSchema(0, MaxFrame)}});
}

QJsonObject tool(const QString& name, const QString& description,
                 QJsonObject fields, bool writes) {
  fields.insert("document_id", stringSchema(128));
  QJsonObject revision = stringSchema(20);
  revision.insert("pattern", "^(0|[1-9][0-9]*)$");
  fields.insert("expected_revision", revision);
  if (writes) fields.insert("operation_id", stringSchema(128));
  QJsonObject descriptor = operation(name, description, {}, {}, writes);
  descriptor.insert("inputSchema", strictObject(fields));
  return descriptor;
}

void addEditTools(QJsonArray& catalog) {
  const QJsonObject reject{{"const", "reject"}};
  catalog.append(tool(
      "audio.clip.place",
      "Place a source range on a sound column; overlaps are rejected, never "
      "trimmed.",
      {{"column_id", stringSchema(128)},
       {"level_id", stringSchema(128)},
       {"placement", placementSchema()},
       {"overlap_policy", reject}},
      true));
  catalog.append(tool(
      "audio.clip.update",
      "Replace a clip's complete placement; the clip is addressed by column "
      "and timeline start at this revision.",
      {{"clip", clipKeySchema()},
       {"placement", placementSchema()},
       {"overlap_policy", reject}},
      true));
  catalog.append(tool("audio.clip.remove",
                      "Remove one clip; its source stays in the cast.",
                      {{"clip", clipKeySchema()}}, true));
}

}  // namespace

QString automationSoundPcmSha256(Document& document, TXshLevel* level) {
  TXshSoundLevel* sound = level->getSoundLevel();
  if (!sound) invalid("level_id must name an imported sound source.");
  return sourceMetadata(document, sound).value("pcm_sha256").toString();
}

qint64 automationSoundBytes(TXshSoundLevel& level) {
  const TSoundTrackP track = level.getSoundTrack();
  return track ? qint64(track->getSampleCount()) * track->getSampleSize() : 0;
}

void validateAutomationAudio(ToonzScene& scene) {
  requireSoundProfile(scene);
  if (!soundSources(scene).empty()) requireStableRate(scene);
  requireSavableColumns(scene);
}

bool automationSceneHasSoundClips(ToonzScene& scene) {
  TXsheet* sheet = scene.getXsheet();
  for (int index : soundColumnIndexes(sheet))
    if (soundColumnAt(sheet, index)->getClipCount() > 0) return true;
  return false;
}

void setAutomationSoundFrameRate(ToonzScene& scene, double fps) {
  for (const auto& source : soundSources(scene))
    source.second->setFrameRate(fps);
}

void muteAutomationSound(ToonzScene& scene) {
  TXsheet* sheet = scene.getXsheet();
  for (int index : soundColumnIndexes(sheet)) {
    soundColumnAt(sheet, index)->setPreviewVisible(false);
    soundColumnAt(sheet, index)->setCamstandVisible(false);
  }
}

// ---------------------------------------------------------------------------
// Packages.

namespace {

QString assetPath(int index, const TXshSoundLevel& level) {
  return "assets/sound-" + QString::number(index) + "." +
         QString::fromStdString(level.getPath().getType());
}

QJsonObject soundEntry(ToonzScene& scene, int index, TXshSoundLevel& level,
                       const QString& directory) {
  QByteArray bytes;
  const QJsonObject meta = verifiedSource(scene, level, &bytes);
  const QString relative = assetPath(index, level);
  writeFile(QDir(directory).absoluteFilePath(relative), bytes);
  const TSoundTrackP track = level.getSoundTrack();
  return {{"name", QString::fromStdWString(level.getName())},
          {"path", relative},
          {"container", meta.value("container")},
          {"size_bytes", meta.value("size_bytes")},
          {"source_sha256", meta.value("source_sha256")},
          {"pcm_sha256", meta.value("pcm_sha256")},
          {"format", formatJson(track->getFormat())},
          {"sample_count", double(track->getSampleCount())}};
}

QJsonObject savedColumn(TXsheet* sheet, int position) {
  TXshSoundColumn* column = soundColumnAt(sheet, position);
  QJsonArray clips;
  for (int index = 0; index < column->getClipCount(); ++index) {
    const ColumnLevel* clip = column->getClip(index);
    clips.append(QJsonObject{
        {"source", QString::fromStdWString(clip->getSoundLevel()->getName())},
        {"timeline_start_frame", clip->getVisibleStartFrame()},
        {"source_start_frame", clip->getStartOffset()},
        {"frame_count", clip->getVisibleFrameCount()}});
  }
  return {{"position", position},
          {"name",
           QString::fromStdString(columnObject(sheet, position)->getName())},
          {"volume_permille", volumePermille(*column)},
          {"audible", column->isPreviewVisible()},
          {"locked", column->isLocked()},
          {"gain_sha256", gainDigest(*column->getVolumeCurve())},
          {"clips", clips}};
}

QJsonArray savedColumns(ToonzScene& scene) {
  QJsonArray result;
  TXsheet* sheet  = scene.getXsheet();
  const int saved = sheet->getFirstFreeColumnIndex();
  for (int index : soundColumnIndexes(sheet))
    if (index < saved) result.append(savedColumn(sheet, index));
  return result;
}

[[noreturn]] void packageDiffers(const QString& message,
                                 QJsonObject details = {}) {
  throw Error("invalid_package", message, details);
}

TXshSoundLevel* savedSource(ToonzScene& scene, const QJsonObject& entry) {
  TXshLevel* native = scene.getLevelSet()->getLevel(
      entry.value("name").toString().toStdWString());
  TXshSoundLevel* level = native ? native->getSoundLevel() : nullptr;
  const TFilePath coded(
      ("$scenefolder/" + entry.value("path").toString()).toStdWString());
  if (!level || level->getPath() != coded)
    packageDiffers("A reopened sound source differs from the package manifest.",
                   {{"name", entry.value("name")}});
  return level;
}

void reopenSource(ToonzScene& scene, const QJsonObject& entry) {
  TXshSoundLevel* level = savedSource(scene, entry);
  const QString path    = QString::fromStdWString(
      scene.decodeFilePath(level->getPath()).getWideString());
  const QByteArray bytes         = readSoundFile(path);
  const AutomationAudioInfo info = parseAutomationAudio(bytes);
  const TSoundTrackP track       = decodeNative(path);
  if (info.container != entry.value("container").toString() ||
      QString::fromLatin1(info.pcmSha256) !=
          entry.value("pcm_sha256").toString() ||
      sha256(bytes) != entry.value("source_sha256").toString() ||
      !sameTrack(info, track) ||
      formatJson(track->getFormat()) != entry.value("format").toObject())
    packageDiffers("A reopened sound source does not decode to its saved PCM.",
                   {{"name", entry.value("name")}});
  level->setScene(&scene);
  level->setSoundTrack(track);
}

}  // namespace

QJsonArray missingAutomationSounds(ToonzScene& scene) {
  QJsonArray missing;
  for (const auto& source : soundSources(scene)) {
    try {
      verifiedSource(scene, *source.second);
    } catch (const Error& error) {
      if (missing.size() < 64)
        missing.append(QJsonObject{
            {"kind", "sound"},
            {"name", QString::fromStdWString(source.second->getName())},
            {"code", error.code}});
    }
  }
  return missing;
}

QJsonObject writeAutomationSounds(ToonzScene& scene, const QString& directory) {
  QJsonArray sounds;
  for (const auto& source : soundSources(scene))
    sounds.append(soundEntry(scene, source.first, *source.second, directory));
  const QJsonArray columns = savedColumns(scene);
  if (sounds.isEmpty() && columns.isEmpty()) return {};
  return {{"sounds", sounds},
          {"sound_columns", columns},
          {"frame_rate", sceneRate(scene)}};
}

void reopenAutomationSounds(ToonzScene& scene, const QJsonObject& manifest) {
  const QJsonArray sounds = manifest.value("sounds").toArray();
  if (int(soundSources(scene).size()) != sounds.size())
    packageDiffers("The reopened scene has a different set of sound sources.");
  if (manifest.contains("frame_rate") &&
      manifest.value("frame_rate").toDouble() != sceneRate(scene))
    packageDiffers("The reopened scene frame rate differs from its source.");
  for (const QJsonValue& entry : sounds) reopenSource(scene, entry.toObject());
  // Clip spans depend on the decoded sources, which the scene file omits.
  scene.getXsheet()->updateFrameCount();
  if (savedColumns(scene) != manifest.value("sound_columns").toArray())
    packageDiffers("Reopened sound columns differ from their saved layout.");
}

QHash<TXshLevel*, QJsonObject> adoptAutomationSounds(Context& context,
                                                     ToonzScene& scene) {
  QHash<TXshLevel*, QJsonObject> result;
  for (const auto& source : soundSources(scene)) {
    QByteArray bytes;
    const QJsonObject meta = verifiedSource(scene, *source.second, &bytes);
    const QString path =
        privateCopyPath(context, meta.value("container").toString());
    writeFile(path, bytes);
    source.second->setPath(TFilePath(path.toStdWString()));
    result.insert(source.second, meta);
  }
  return result;
}

AutomationSoundPaths::AutomationSoundPaths(ToonzScene& scene) {
  for (const auto& source : soundSources(scene)) {
    TXshSoundLevel* level = source.second;
    m_paths.emplace_back(level, level->getPath());
    level->setPath(TFilePath(
        ("$scenefolder/" + assetPath(source.first, *level)).toStdWString()));
  }
}

AutomationSoundPaths::~AutomationSoundPaths() {
  for (const auto& entry : m_paths) entry.first->setPath(entry.second);
}

QJsonArray audioOperations() {
  QJsonArray catalog;
  catalog.append(tool(
      "audio.import",
      "Import a WAV or AIFF file as a sound source after bounded validation.",
      {{"path", stringSchema(4096)}, {"name", boundedNameSchema(128)}}, true));
  catalog.append(tool("audio.inspect",
                      "Inspect sound sources, columns, clips and the mix rule.",
                      {}, false));
  catalog.append(tool("audio.column.create",
                      "Append an audible, unlocked sound column with an "
                      "explicit volume.",
                      {{"name", boundedNameSchema(128)},
                       {"volume_permille", integerSchema(0, 1000)}},
                      true));
  catalog.append(tool("audio.column.set_volume",
                      "Set a sound column's volume in per-mille.",
                      {{"column_id", stringSchema(128)},
                       {"volume_permille", integerSchema(0, 1000)}},
                      true));
  addEditTools(catalog);
  catalog.append(
      tool("audio.mixdown",
           "Write the native offline mix of a frame range to a new WAV file.",
           {{"start_frame", integerSchema(0, MaxFrame)},
            {"end_frame", integerSchema(0, MaxFrame)},
            {"output_path", stringSchema(4096)},
            {"format_policy", QJsonObject{{"const", "native_mix"}}}},
           true));
  return catalog;
}

QJsonValue executeAudio(Context& context, const QString& method,
                        const QJsonObject& params) {
  static const std::map<QString, QJsonValue (*)(Context&, const QJsonObject&)>
      handlers{{"audio.import", importAudio},
               {"audio.inspect", inspectAudio},
               {"audio.column.create", createColumn},
               {"audio.column.set_volume", setVolume},
               {"audio.clip.place", placeClip},
               {"audio.clip.update", updateClip},
               {"audio.clip.remove", removeClip},
               {"audio.mixdown", mixdown}};
  const auto found = handlers.find(method);
  if (found == handlers.end())
    throw Error("unknown_method", "Unknown detached audio operation.");
  return found->second(context, params);
}

}  // namespace GroundzeroAutomation
