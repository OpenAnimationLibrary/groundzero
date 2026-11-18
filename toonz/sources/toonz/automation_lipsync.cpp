#include "automation_lipsync.h"
#include "automation.h"
#include "automation_audio.h"

#include "toutputproperties.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>

#include <cmath>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr qint64 MaxFileBytes    = 1024 * 1024;
constexpr int MaxCues            = 20000;
constexpr int MaxAnalyses        = 64;
constexpr int MaxFrame           = 1000000;
constexpr int MaxAppliedFrames   = 100000;
constexpr int MaxDroppedListed   = 100;
constexpr qint64 MaxMicroseconds = 3600LL * 1000000;
constexpr qint64 Microseconds    = 1000000;

struct Cue final {
  qint64 start = 0;   // 1-based frame or microseconds, by timebase
  qint64 end   = -1;  // microseconds; -1 when the file gives no end
  QString shape;
};

struct Analysis final {
  QString format;
  QString shapeSet;
  double frameRate = 0;  // nonzero for a frames timebase
  std::vector<Cue> cues;
  QString fileSha256;
  QString digest;
  QJsonValue link;
};

std::map<QString, Analysis> analyses;

[[noreturn]] void invalid(const QString& message, QJsonObject details = {}) {
  throw Error("invalid_params", message, details);
}

[[noreturn]] void malformed(int line, const QString& message) {
  throw Error("invalid_asset", message, {{"line", line}});
}

const QStringList& shapeSet(const QString& name) {
  static const std::map<QString, QStringList> sets{
      {"preston_blair",
       {"ai", "e", "o", "u", "fv", "l", "mbp", "wq", "etc", "rest"}},
      {"rhubarb_basic", {"A", "B", "C", "D", "E", "F"}},
      {"rhubarb_extended", {"A", "B", "C", "D", "E", "F", "G", "H", "X"}}};
  return sets.at(name);
}

QString sha256(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// ---------------------------------------------------------------------------
// Strict, bounded parsing of user-authored timing files.

QStringList textLines(const QByteArray& bytes) {
  const QString text = QString::fromUtf8(bytes);
  if (text.toUtf8() != bytes || text.contains(QChar::Null))
    throw Error("invalid_asset",
                "Timing files must be UTF-8 text without NUL.");
  QStringList lines = text.split('\n');
  for (QString& line : lines)
    if (line.endsWith('\r')) line.chop(1);
  return lines;
}

void addCue(std::vector<Cue>& cues, const Cue& cue, int line) {
  if (!cues.empty() && cue.start <= cues.back().start)
    malformed(line, "Cue times must strictly increase.");
  if (int(cues.size()) >= MaxCues)
    throw Error("resource_limit", "Timing files are limited to 20000 cues.");
  cues.push_back(cue);
}

QString prestonShape(const QString& name, int line) {
  const QString shape = name.toLower() == "other" ? "etc" : name.toLower();
  if (!shapeSet("preston_blair").contains(shape))
    malformed(line, "Unknown Preston Blair shape: " + name);
  return shape;
}

std::vector<Cue> parseDat(const QByteArray& bytes) {
  static const QRegularExpression key("^([1-9][0-9]{0,6})[ \\t]+([A-Za-z]+)$");
  std::vector<Cue> cues;
  bool header      = false;
  const auto lines = textLines(bytes);
  for (int index = 0; index < lines.size(); ++index) {
    const QString& line = lines[index];
    if (line.isEmpty()) continue;
    if (!header) {
      if (line != "MohoSwitch1")
        malformed(index + 1, "The first line must be MohoSwitch1.");
      header = true;
      continue;
    }
    const auto match = key.match(line);
    if (!match.hasMatch() || match.captured(1).toLongLong() > MaxFrame + 1)
      malformed(index + 1, "Expected '<frame> <shape>' with a frame from 1.");
    addCue(cues,
           {match.captured(1).toLongLong(), -1,
            prestonShape(match.captured(2), index + 1)},
           index + 1);
  }
  return cues;
}

qint64 decimalMicroseconds(const QString& seconds, const QString& fraction) {
  return seconds.toLongLong() * Microseconds +
         fraction.leftJustified(6, '0').toLongLong();
}

std::vector<Cue> parseTsv(const QByteArray& bytes, const QStringList& shapes) {
  static const QRegularExpression cue(
      "^(0|[1-9][0-9]{0,3})(?:\\.([0-9]{1,6}))?\\t([A-HX])$");
  std::vector<Cue> cues;
  const auto lines = textLines(bytes);
  for (int index = 0; index < lines.size(); ++index) {
    if (lines[index].isEmpty()) continue;
    const auto match = cue.match(lines[index]);
    const qint64 start =
        match.hasMatch()
            ? decimalMicroseconds(match.captured(1), match.captured(2))
            : -1;
    if (start < 0 || start > MaxMicroseconds ||
        !shapes.contains(match.captured(3)))
      malformed(index + 1,
                "Expected '<seconds>\\t<shape>' with at most six decimals and "
                "a shape of the chosen set.");
    addCue(cues, {start, -1, match.captured(3)}, index + 1);
  }
  return cues;
}

qint64 jsonMicroseconds(const QJsonValue& value, int cue) {
  const double seconds = value.toDouble(-1);
  if (!value.isDouble() || !(seconds >= 0) || seconds > 3600)
    malformed(cue, "Cue times must be seconds from 0 through 3600.");
  const qint64 micro = std::llround(seconds * Microseconds);
  if (double(micro) / Microseconds != seconds)
    malformed(cue, "Cue times allow at most six decimal places.");
  return micro;
}

void requireKnownKeys(const QJsonObject& object, const QStringList& keys,
                      const QString& name) {
  for (const QString& key : object.keys())
    if (!keys.contains(key))
      throw Error("invalid_asset", name + " has an unknown field: " + key);
}

QJsonArray jsonCues(const QByteArray& bytes) {
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    throw Error("invalid_asset", "Rhubarb JSON must be one object.",
                {{"offset", error.offset}});
  const QJsonObject root = document.object();
  requireKnownKeys(root, {"metadata", "mouthCues"}, "Rhubarb JSON");
  const QJsonObject metadata = root.value("metadata").toObject();
  requireKnownKeys(metadata, {"soundFile", "duration"}, "metadata");
  if (!root.value("metadata").isObject() ||
      !root.value("mouthCues").isArray() ||
      (metadata.contains("soundFile") &&
       !metadata.value("soundFile").isString()) ||
      (metadata.contains("duration") && !metadata.value("duration").isDouble()))
    throw Error("invalid_asset",
                "Rhubarb JSON needs metadata and a mouthCues array.");
  return root.value("mouthCues").toArray();
}

std::vector<Cue> parseJson(const QByteArray& bytes, const QStringList& shapes) {
  std::vector<Cue> cues;
  const QJsonArray values = jsonCues(bytes);
  for (int index = 0; index < values.size(); ++index) {
    const QJsonObject value = values.at(index).toObject();
    if (!values.at(index).isObject())
      malformed(index + 1, "Each mouth cue must be an object.");
    requireKnownKeys(value, {"start", "end", "value"}, "A mouth cue");
    const Cue cue{jsonMicroseconds(value.value("start"), index + 1),
                  jsonMicroseconds(value.value("end"), index + 1),
                  value.value("value").toString()};
    if (cue.end <= cue.start || !shapes.contains(cue.shape) ||
        (!cues.empty() && cue.start != cues.back().end))
      malformed(index + 1,
                "Mouth cues must be contiguous, nonempty and use the chosen "
                "shape set.");
    addCue(cues, cue, index + 1);
  }
  return cues;
}

// ---------------------------------------------------------------------------
// Import.

void requireFormat(const QJsonObject& params) {
  const QString format = params.value("format").toString();
  const QString set    = params.value("shape_set").toString();
  const bool dat       = format == "papagayo_dat";
  if (dat != params.contains("frame_rate"))
    invalid(dat ? "papagayo_dat requires frame_rate."
                : "frame_rate applies only to papagayo_dat.");
  if (dat != (set == "preston_blair"))
    invalid(dat ? "papagayo_dat uses the preston_blair shape set."
                : "Rhubarb files use a rhubarb shape set.");
}

QByteArray readTimingFile(Context& context, const QString& path) {
  const QString resolved = context.resolvePath(path, false);
  const QFileInfo info(resolved);
  if (!info.isFile() || info.isSymLink())
    throw Error("invalid_asset", "The timing file is not a regular file.");
  if (info.size() > MaxFileBytes)
    throw Error("resource_limit", "Timing files are limited to 1 MiB.");
  QFile file(resolved);
  if (!file.open(QIODevice::ReadOnly))
    throw Error("io_error", "Cannot open the timing file.");
  const QByteArray bytes = file.read(MaxFileBytes + 1);
  if (file.error() != QFileDevice::NoError || bytes.size() != info.size())
    throw Error("io_error", "Cannot read the complete timing file.");
  return bytes;
}

std::vector<Cue> parseCues(const QByteArray& bytes, const QString& format,
                           const QString& set) {
  std::vector<Cue> cues = format == "papagayo_dat" ? parseDat(bytes)
                          : format == "rhubarb_tsv"
                              ? parseTsv(bytes, shapeSet(set))
                              : parseJson(bytes, shapeSet(set));
  if (cues.empty())
    throw Error("invalid_asset", "The timing file contains no cues.");
  return cues;
}

double sceneRate(ToonzScene& scene) {
  return scene.getProperties()->getOutputProperties()->getFrameRate();
}

QJsonValue readLink(Context& context, const QJsonObject& params) {
  if (!params.contains("source_link")) return QJsonValue();
  const QJsonObject link = params.value("source_link").toObject();
  Document& document = context.document(requiredString(link, "document_id"));
  if (document.revisionText() != requiredString(link, "expected_revision"))
    throw Error(
        "revision_conflict",
        "The linked document changed; inspect its current revision.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
  TXshLevel* level = document.level(requiredString(link, "level_id"));
  return QJsonObject{{"document_id", document.id},
                     {"level_id", link.value("level_id")},
                     {"pcm_sha256", automationSoundPcmSha256(document, level)},
                     {"frame_rate", sceneRate(*document.scene)}};
}

QJsonValue timebase(const Analysis& analysis) {
  if (analysis.frameRate > 0)
    return QJsonObject{{"kind", "frames"},
                       {"frame_rate", analysis.frameRate},
                       {"first_frame_number", 1}};
  return QJsonObject{{"kind", "microseconds"}};
}

QJsonArray cueRecords(const Analysis& analysis, int offset, int limit) {
  QJsonArray result;
  for (int index = offset;
       index < int(analysis.cues.size()) && index < offset + limit; ++index) {
    const Cue& cue = analysis.cues[index];
    result.append(QJsonObject{
        {"index", index},
        {"start", double(cue.start)},
        {"end", cue.end < 0 ? QJsonValue() : QJsonValue(double(cue.end))},
        {"shape", cue.shape}});
  }
  return result;
}

QString analysisDigest(const Analysis& analysis) {
  const QJsonObject canonical{
      {"format", analysis.format},
      {"shape_set", analysis.shapeSet},
      {"timebase", timebase(analysis)},
      {"cues", cueRecords(analysis, 0, int(analysis.cues.size()))},
      {"linked_source", analysis.link}};
  return sha256(QJsonDocument(canonical).toJson(QJsonDocument::Compact));
}

QJsonArray shapesUsed(const Analysis& analysis) {
  std::set<QString> used;
  for (const Cue& cue : analysis.cues) used.insert(cue.shape);
  QJsonArray result;
  for (const QString& shape : shapeSet(analysis.shapeSet))
    if (used.count(shape)) result.append(shape);
  return result;
}

QJsonObject analysisJson(const QString& id, const Analysis& analysis) {
  return {{"analysis_id", id},
          {"cue_count", int(analysis.cues.size())},
          {"digest", analysis.digest},
          {"format", analysis.format},
          {"linked_source", analysis.link},
          {"recognizer", QJsonValue()},
          {"shape_set", analysis.shapeSet},
          {"timebase", timebase(analysis)}};
}

QJsonValue importTiming(Context& context, const QJsonObject& params) {
  requireFormat(params);
  if (int(analyses.size()) >= MaxAnalyses)
    throw Error("resource_limit",
                "This worker has reached its 64 lip-sync analysis limit.");
  Analysis analysis;
  analysis.format    = params.value("format").toString();
  analysis.shapeSet  = params.value("shape_set").toString();
  analysis.frameRate = params.value("frame_rate").toDouble(0);
  analysis.link      = readLink(context, params);
  const QByteArray bytes =
      readTimingFile(context, requiredString(params, "path"));
  analysis.cues       = parseCues(bytes, analysis.format, analysis.shapeSet);
  analysis.fileSha256 = sha256(bytes);
  analysis.digest     = analysisDigest(analysis);
  const QString id =
      "analysis_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  context.markMutationStarted();
  analyses.emplace(id, analysis);
  QJsonObject result = analysisJson(id, analysis);
  result.insert("shapes_used", shapesUsed(analysis));
  result.insert("source_file_sha256", analysis.fileSha256);
  return result;
}

const Analysis& findAnalysis(const QString& id) {
  const auto found = analyses.find(id);
  if (found == analyses.end())
    throw Error("invalid_handle",
                "The analysis is unknown or belongs to another worker.");
  return found->second;
}

QJsonValue inspectTiming(Context&, const QJsonObject& params) {
  const QString id         = requiredString(params, "analysis_id");
  const Analysis& analysis = findAnalysis(id);
  const int offset         = params.value("offset").toInt();
  const int limit          = params.value("limit").toInt();
  const int count          = int(analysis.cues.size());
  QJsonObject result       = analysisJson(id, analysis);
  result.insert("cues", cueRecords(analysis, offset, limit));
  result.insert("offset", offset);
  result.insert("limit", limit);
  result.insert("next_offset", offset + limit < count
                                   ? QJsonValue(offset + limit)
                                   : QJsonValue());
  return result;
}

// ---------------------------------------------------------------------------
// Application.

struct Rate final {
  qint64 numerator   = 0;
  qint64 denominator = 1;
};

// The exact decimal the native scene file stores for a native-stable rate.
Rate exactRate(double fps) {
  std::ostringstream text;
  text.imbue(std::locale::classic());
  text << fps;
  std::istringstream check(text.str());
  check.imbue(std::locale::classic());
  double parsed = 0;
  check >> parsed;
  const QString decimal = QString::fromStdString(text.str());
  static const QRegularExpression form("^([0-9]+)(?:\\.([0-9]+))?$");
  const auto match = form.match(decimal);
  if (parsed != fps || !match.hasMatch())
    throw Error("timing_conflict",
                "Lip-sync timing requires a frame rate the native scene file "
                "stores exactly, such as 23.976, 24 or 29.97.",
                {{"frame_rate", fps}});
  Rate rate;
  rate.numerator = (match.captured(1) + match.captured(2)).toLongLong();
  for (int digit = 0; digit < match.captured(2).size(); ++digit)
    rate.denominator *= 10;
  return rate;
}

struct Timing final {
  int start   = 0;
  bool frames = false;
  Rate rate;
  qint64 hold = -1;  // inclusive last frame, or -1
};

qint64 frameOf(const Timing& timing, qint64 value) {
  if (timing.frames) return timing.start + value - 1;
  return timing.start + value * timing.rate.numerator /
                            (timing.rate.denominator * Microseconds);
}

struct Span final {
  qint64 first;
  qint64 last;
  int cue;
};

qint64 lastEnd(const Timing& timing, const Cue& last) {
  if (timing.hold >= 0) {
    if (timing.hold < frameOf(timing, last.start))
      invalid("hold_until_frame must not precede the last cue.");
    return timing.hold + 1;
  }
  if (!timing.frames && last.end >= 0) return frameOf(timing, last.end);
  return frameOf(timing, last.start) + 1;
}

struct Plan final {
  std::vector<Span> spans;
  QJsonArray dropped;
  int droppedCount = 0;
};

Plan planSpans(const Analysis& analysis, const Timing& timing) {
  Plan plan;
  const auto& cues = analysis.cues;
  for (int index = 0; index < int(cues.size()); ++index) {
    const qint64 first = frameOf(timing, cues[index].start);
    const qint64 next  = index + 1 < int(cues.size())
                             ? frameOf(timing, cues[index + 1].start)
                             : lastEnd(timing, cues[index]);
    if (next > first) {
      plan.spans.push_back({first, next - 1, index});
      continue;
    }
    ++plan.droppedCount;
    if (plan.dropped.size() < MaxDroppedListed)
      plan.dropped.append(
          QJsonObject{{"index", index}, {"shape", cues[index].shape}});
  }
  if (plan.spans.empty())
    invalid("The analysis covers no whole frame at this frame rate.");
  const qint64 count = plan.spans.back().last - plan.spans.front().first + 1;
  if (plan.spans.back().last > MaxFrame || count > MaxAppliedFrames)
    throw Error("resource_limit",
                "Lip-sync application is limited to 100000 frames ending at "
                "or before frame 1000000.",
                {{"last_frame", double(plan.spans.back().last)}});
  return plan;
}

TFrameId readDrawing(const QJsonObject& value) {
  const QString suffix = value.value("suffix").toString();
  static const QRegularExpression letters("^[a-z]{0,8}$");
  if (!letters.match(suffix).hasMatch())
    invalid("drawing_id suffix must contain up to eight lowercase letters.");
  return TFrameId(value.value("number").toInt(), suffix);
}

TFrameId requireDrawing(TXshSimpleLevel* level, const QJsonObject& value) {
  const TFrameId frame = readDrawing(value);
  if (!level->isFid(frame))
    throw Error("missing_drawing", "The mapped drawing is not in the level.",
                {{"drawing_id", value}});
  return frame;
}

std::map<QString, TFrameId> readMapping(const QJsonObject& params,
                                        const Analysis& analysis,
                                        TXshSimpleLevel* level) {
  std::map<QString, TFrameId> mapping;
  for (const QJsonValue& value : params.value("mapping").toArray()) {
    const QJsonObject entry = value.toObject();
    const QString shape     = entry.value("shape").toString();
    if (!shapeSet(analysis.shapeSet).contains(shape))
      invalid("A mapped shape is not in the analysis shape set.",
              {{"shape", shape}});
    if (mapping.count(shape))
      invalid("Each shape may be mapped once.", {{"shape", shape}});
    mapping.emplace(
        shape, requireDrawing(level, entry.value("drawing_id").toObject()));
  }
  return mapping;
}

std::vector<TFrameId> spanDrawings(const QJsonObject& params,
                                   const Analysis& analysis,
                                   TXshSimpleLevel* level, const Plan& plan) {
  const auto mapping       = readMapping(params, analysis, level);
  const QJsonObject policy = params.value("unmapped_policy").toObject();
  const bool fallback      = policy.value("kind") == "use_drawing";
  const TFrameId spare =
      fallback ? requireDrawing(level, policy.value("drawing_id").toObject())
               : TFrameId();
  std::vector<TFrameId> drawings;
  std::set<QString> unmapped;
  for (const Span& span : plan.spans) {
    const QString& shape = analysis.cues[span.cue].shape;
    const auto found     = mapping.find(shape);
    if (found == mapping.end() && !fallback) unmapped.insert(shape);
    drawings.push_back(found == mapping.end() ? spare : found->second);
  }
  if (!unmapped.empty()) {
    QJsonArray shapes;
    for (const QString& shape : unmapped) shapes.append(shape);
    invalid("Map every used shape or choose use_drawing.",
            {{"unmapped_shapes", shapes}});
  }
  return drawings;
}

struct Target final {
  TXshColumn* column;
  int index;
  TXshSimpleLevel* level;
};

Target readTarget(Document& document, const QJsonObject& params) {
  TXshColumn* column = document.column(requiredString(params, "column_id"));
  TXsheet* sheet     = document.scene->getXsheet();
  int index          = 0;
  while (sheet->getColumn(index) != column) ++index;
  if (!column->getLevelColumn())
    throw Error("incompatible_column", "Lip sync needs a level column.");
  TStageObject* object = sheet->getStageObject(TStageObjectId::ColumnId(index));
  if (column->isLocked() || object->isLocked())
    throw Error("locked", "Unlock the target column first.");
  TXshSimpleLevel* level =
      document.level(requiredString(params, "level_id"))->getSimpleLevel();
  const int type = level ? level->getType() : UNKNOWN_XSHLEVEL;
  if (type != PLI_XSHLEVEL && type != TZP_XSHLEVEL && type != OVL_XSHLEVEL)
    throw Error("unsupported_level",
                "Lip sync needs a vector, Toonz raster or RGBA level.");
  return {column, index, level};
}

void requireFreshLink(Context& context, const Analysis& analysis) {
  if (analysis.link.isNull()) return;
  const QJsonObject link = analysis.link.toObject();
  try {
    Document& document = context.document(link.value("document_id").toString());
    TXshLevel* level   = document.level(link.value("level_id").toString());
    if (automationSoundPcmSha256(document, level) ==
        link.value("pcm_sha256").toString())
      return;
  } catch (const Error& error) {
    if (error.code != "document_not_found" && error.code != "invalid_handle")
      throw;
  }
  throw Error("stale_analysis",
              "The linked sound source is gone or its audio changed.");
}

Timing readTiming(Document& document, const Analysis& analysis,
                  const QJsonObject& params) {
  const double fps = sceneRate(*document.scene);
  if (params.value("expected_frame_rate").toDouble() != fps)
    throw Error("timing_conflict",
                "expected_frame_rate differs from the scene frame rate.",
                {{"frame_rate", fps}});
  if (analysis.frameRate > 0 && analysis.frameRate != fps)
    throw Error("timing_conflict",
                "The analysis frame numbers use another frame rate.",
                {{"analysis_frame_rate", analysis.frameRate}});
  Timing timing;
  timing.start          = params.value("start_frame").toInt();
  timing.frames         = analysis.frameRate > 0;
  timing.rate           = exactRate(fps);
  const QJsonObject end = params.value("end_policy").toObject();
  if (end.value("kind") == "hold_until_frame")
    timing.hold = end.value("frame").toInt();
  return timing;
}

std::vector<TXshCell> plannedCells(TXshSimpleLevel* level, const Plan& plan,
                                   const std::vector<TFrameId>& drawings) {
  std::vector<TXshCell> cells;
  for (std::size_t index = 0; index < plan.spans.size(); ++index)
    for (qint64 frame = plan.spans[index].first;
         frame <= plan.spans[index].last; ++frame)
      cells.emplace_back(level, drawings[index]);
  return cells;
}

QJsonArray runs(qint64 first, const std::vector<TXshCell>& cells) {
  QJsonArray result;
  for (std::size_t index = 0; index < cells.size();) {
    std::size_t end = index;
    while (end + 1 < cells.size() && cells[end + 1] == cells[index]) ++end;
    result.append(QJsonArray{double(first + qint64(index)),
                             double(first + qint64(end)),
                             cells[index].m_frameId.getNumber(),
                             cells[index].m_frameId.getLetter()});
    index = end + 1;
  }
  return result;
}

bool writeCells(Context& context, Document& document, const Target& target,
                int first, const std::vector<TXshCell>& cells) {
  TXsheet* sheet = document.scene->getXsheet();
  bool changed   = false;
  for (std::size_t index = 0; index < cells.size() && !changed; ++index)
    changed = sheet->getCell(first + int(index), target.index) != cells[index];
  if (!changed) return false;
  TStageObject* object =
      sheet->getStageObject(TStageObjectId::ColumnId(target.index));
  const std::string name = object->getName();
  context.markMutationStarted();
  if (!sheet->setCells(first, target.index, int(cells.size()), cells.data()))
    throw Error("native_error",
                "The native column refused the lip-sync cells.");
  // Native exposure can rename columns according to the GUI preference.
  object->setName(name);
  return true;
}

QJsonValue applyTiming(Context& context, const QJsonObject& params) {
  Document& document = context.document(requiredString(params, "document_id"));
  const QString id   = requiredString(params, "analysis_id");
  const Analysis& analysis = findAnalysis(id);
  const Timing timing      = readTiming(document, analysis, params);
  requireFreshLink(context, analysis);
  const Target target = readTarget(document, params);
  const Plan plan     = planSpans(analysis, timing);
  const auto cells    = plannedCells(
      target.level, plan, spanDrawings(params, analysis, target.level, plan));
  const int first          = int(plan.spans.front().first);
  const QJsonArray written = runs(first, cells);
  const bool changed = writeCells(context, document, target, first, cells);
  return QJsonObject{
      {"analysis_id", id},
      {"cells_written", int(cells.size())},
      {"changed", changed},
      {"column_id", params.value("column_id")},
      {"dropped_cue_count", plan.droppedCount},
      {"dropped_cues", plan.dropped},
      {"first_frame", first},
      {"frame_rate", sceneRate(*document.scene)},
      {"last_frame", int(plan.spans.back().last)},
      {"level_id", params.value("level_id")},
      {"runs_count", written.size()},
      {"runs_sha256",
       sha256(QJsonDocument(written).toJson(QJsonDocument::Compact))}};
}

// ---------------------------------------------------------------------------
// Catalog.

QJsonObject stringSchema(int maximum) {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
}

QJsonObject revisionSchema() {
  QJsonObject schema = stringSchema(20);
  schema.insert("pattern", "^(0|[1-9][0-9]*)$");
  return schema;
}

QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject strictObject(const QJsonObject& properties,
                         const QJsonArray& required) {
  return {{"type", "object"},
          {"properties", properties},
          {"required", required},
          {"additionalProperties", false}};
}

QJsonObject constSchema(const QString& value) { return {{"const", value}}; }

QJsonObject drawingSchema() {
  return strictObject({{"number", integerSchema(1, 999999)},
                       {"suffix", QJsonObject{{"type", "string"},
                                              {"pattern", "^[a-z]{0,8}$"}}}},
                      {"number", "suffix"});
}

QJsonObject shapeSchema() {
  return {{"type", "string"},
          {"enum",
           QJsonArray{"ai", "e", "o", "u", "fv", "l", "mbp", "wq", "etc",
                      "rest", "A", "B", "C", "D", "E", "F", "G", "H", "X"}}};
}

QJsonObject rateSchema() {
  return {{"type", "number"}, {"minimum", 1}, {"maximum", 240}};
}

QJsonObject tool(const QString& name, const QString& description,
                 const QJsonObject& fields, const QJsonArray& required,
                 bool writes) {
  QJsonObject descriptor = operation(name, description, {}, {}, writes);
  descriptor.insert("inputSchema", strictObject(fields, required));
  return descriptor;
}

QJsonObject importTool() {
  const QJsonObject link =
      strictObject({{"document_id", stringSchema(128)},
                    {"expected_revision", revisionSchema()},
                    {"level_id", stringSchema(128)}},
                   {"document_id", "expected_revision", "level_id"});
  QJsonObject descriptor = tool(
      "lipsync.import",
      "Import a Papagayo DAT or Rhubarb TSV/JSON timing file as an immutable "
      "analysis after strict parsing.",
      {{"operation_id", stringSchema(128)},
       {"path", stringSchema(4096)},
       {"format", QJsonObject{{"type", "string"},
                              {"enum", QJsonArray{"papagayo_dat", "rhubarb_tsv",
                                                  "rhubarb_json"}}}},
       {"shape_set",
        QJsonObject{{"type", "string"},
                    {"enum", QJsonArray{"preston_blair", "rhubarb_basic",
                                        "rhubarb_extended"}}}},
       {"frame_rate", rateSchema()},
       {"source_link", link}},
      {"operation_id", "path", "format", "shape_set"}, true);
  descriptor.insert("mutates_document", false);
  return descriptor;
}

QJsonObject applyTool() {
  const QJsonObject mapping{
      {"type", "array"},
      {"minItems", 1},
      {"maxItems", 19},
      {"items",
       strictObject({{"shape", shapeSchema()}, {"drawing_id", drawingSchema()}},
                    {"shape", "drawing_id"})}};
  const QJsonObject unmapped{
      {"oneOf",
       QJsonArray{strictObject({{"kind", constSchema("reject")}}, {"kind"}),
                  strictObject({{"kind", constSchema("use_drawing")},
                                {"drawing_id", drawingSchema()}},
                               {"kind", "drawing_id"})}}};
  const QJsonObject end{
      {"oneOf",
       QJsonArray{
           strictObject({{"kind", constSchema("last_cue_end")}}, {"kind"}),
           strictObject({{"kind", constSchema("hold_until_frame")},
                         {"frame", integerSchema(0, MaxFrame)}},
                        {"kind", "frame"})}}};
  return tool(
      "lipsync.apply",
      "Replace one level column's cells with an analysis mapped to drawings, "
      "at an exact scene frame rate.",
      {{"document_id", stringSchema(128)},
       {"expected_revision", revisionSchema()},
       {"operation_id", stringSchema(128)},
       {"analysis_id", stringSchema(128)},
       {"column_id", stringSchema(128)},
       {"level_id", stringSchema(128)},
       {"start_frame", integerSchema(0, MaxFrame)},
       {"expected_frame_rate", rateSchema()},
       {"mapping", mapping},
       {"unmapped_policy", unmapped},
       {"end_policy", end},
       {"overwrite", constSchema("replace_range")}},
      {"document_id", "expected_revision", "operation_id", "analysis_id",
       "column_id", "level_id", "start_frame", "expected_frame_rate", "mapping",
       "unmapped_policy", "end_policy", "overwrite"},
      true);
}

}  // namespace

QJsonArray lipsyncOperations() {
  return {importTool(),
          tool("lipsync.inspect",
               "Read a page of an analysis's cues and its metadata.",
               {{"analysis_id", stringSchema(128)},
                {"offset", integerSchema(0, MaxCues)},
                {"limit", integerSchema(1, 1000)}},
               {"analysis_id", "offset", "limit"}, false),
          applyTool()};
}

QJsonValue executeLipsync(Context& context, const QString& method,
                          const QJsonObject& params) {
  static const std::map<QString, QJsonValue (*)(Context&, const QJsonObject&)>
      handlers{{"lipsync.import", importTiming},
               {"lipsync.inspect", inspectTiming},
               {"lipsync.apply", applyTiming}};
  const auto found = handlers.find(method);
  if (found == handlers.end())
    throw Error("unknown_method", "Unknown detached lip-sync operation.");
  return found->second(context, params);
}

}  // namespace GroundzeroAutomation
