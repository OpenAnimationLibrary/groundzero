#include "automation_intake.h"

#include "automation_cells.h"
#include "automation_drawing.h"
#include "automation_files.h"
#include "automation_jobs.h"
#include "automation_path.h"
#include "automation_scene_shared.h"
#include "automation_shared_parse.h"

#include "tcolorstyles.h"
#include "tiio.h"
#include "tlevel_io.h"
#include "tpalette.h"
#include "tproperty.h"
#include "trasterimage.h"
#include "tregion.h"
#include "tsimplecolorstyles.h"
#include "tstroke.h"
#include "tvectorimage.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toutputproperties.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QXmlStreamReader>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace GroundzeroAutomation {

using namespace SceneShared;

namespace {

constexpr int MaxIntakeFiles    = 4096;
constexpr int InlineFiles       = 64;
constexpr qint64 InlinePixels   = 67108864;
constexpr qint64 MaxSourceBytes = 67108864;
constexpr int MaxListing        = 16384;
constexpr int MaxReported       = 64;
constexpr int MaxSide           = 8192;
constexpr int MaxSequenceNumber = 999999;
constexpr int MaxChunks         = 1024;
constexpr int DefaultPage       = 64;
constexpr double InchesPerMeter = 0.0254;
constexpr int LastRow           = 999999;

}  // namespace

SequenceName parseSequenceName(const QString& name, const QString& prefix,
                               const QString& extension) {
  SequenceName result;
  const QString head = prefix + '.';
  const QString tail = '.' + extension;
  if (!name.startsWith(head) || !name.endsWith(tail) ||
      name.size() <= head.size() + tail.size())
    return result;
  const QString digits =
      name.mid(head.size(), name.size() - head.size() - tail.size());
  if (digits.size() > 6) return result;
  for (const QChar character : digits)
    if (character < '0' || character > '9') return result;
  result.number  = digits.toInt();
  result.digits  = digits.size();
  result.matches = result.number >= 1 && result.number <= MaxSequenceNumber;
  return result;
}

namespace {

struct SourceFile final {
  QString name;
  int number = 0;
  int digits = 4;
  // regular, missing, link or other.
  QString entry     = "missing";
  qint64 bytes      = 0;
  qint64 modifiedMs = 0;
  ImageHeader header;
  // supported, or the error code the import answers for this file.
  QString status;
  QString reason;
};

struct SourcePlan final {
  QString directory;
  QString selection;
  QString prefix;
  QString extension;
  std::vector<SourceFile> files;
  QList<int> duplicates;
  bool mixedPadding = false;
};

QJsonValue nullable(bool present, const QJsonValue& value) {
  return present ? value : QJsonValue();
}

bool plainName(const QString& name) {
  return !name.isEmpty() && name.size() <= 255 && name != "." && name != ".." &&
         !name.contains('/') && !name.contains('\\') &&
         !name.contains(QChar::Null);
}

QString listedDirectory(Context& context, const QJsonObject& params,
                        QVector<EntryInfo>& entries) {
  const QString directory =
      context.resolvePath(requiredString(params, "source_directory"), false);
  const QFileInfo target(directory);
  if (target.exists() && !target.isSymLink() && !target.isDir())
    throw Error("invalid_path", "source_directory must name a directory.");
  const ListResult listed =
      listApprovedDirectory(directory, entries, MaxListing);
  if (listed == ListResult::TooMany)
    throw Error("resource_limit",
                "The source directory holds more than 4096 entries.",
                {{"limit", "directory_entries"}, {"maximum", MaxListing}});
  if (listed == ListResult::Failed) {
    const QFileInfo info(directory);
    if (!info.exists())
      throw Error("not_found", "The source directory does not exist.");
    if (!info.isDir())
      throw Error("invalid_path", "source_directory must name a directory.");
    throw Error("io_error", "Cannot list the source directory.");
  }
  return directory;
}

void addFileSelection(SourcePlan& plan, const QJsonArray& names) {
  if (names.isEmpty() || names.size() > MaxIntakeFiles)
    throw Error("invalid_params", "file_names holds 1 to 256 names.");
  QSet<QString> seen;
  for (const QJsonValue& value : names) {
    const QString name = value.toString();
    if (!plainName(name) || seen.contains(name))
      throw Error("invalid_params",
                  "file_names holds distinct plain file names.",
                  {{"reason", "file_name"}, {"name", name.left(255)}});
    seen.insert(name);
    SourceFile file;
    file.name   = name;
    file.number = int(plan.files.size()) + 1;
    plan.files.push_back(file);
  }
}

void addSequenceSelection(SourcePlan& plan, const QVector<EntryInfo>& entries) {
  std::map<int, SourceFile> byNumber;
  QSet<int> digitCounts;
  for (const EntryInfo& entry : entries) {
    const SequenceName parsed =
        parseSequenceName(entry.fileName(), plan.prefix, plan.extension);
    if (!parsed.matches) continue;
    if (byNumber.count(parsed.number)) {
      if (!plan.duplicates.contains(parsed.number))
        plan.duplicates.append(parsed.number);
      if (byNumber[parsed.number].name < entry.fileName()) continue;
    }
    SourceFile file;
    file.name   = entry.fileName();
    file.number = parsed.number;
    file.digits = parsed.digits;
    digitCounts.insert(parsed.digits);
    byNumber[parsed.number] = file;
  }
  if (byNumber.empty())
    throw Error("not_found", "No file in the directory matches the sequence.",
                {{"reason", "no_sequence_files"}});
  if (byNumber.size() > size_t(MaxIntakeFiles))
    throw Error("resource_limit", "A sequence holds at most 256 frames.",
                {{"limit", "sequence_frames"},
                 {"maximum", MaxIntakeFiles},
                 {"count", int(byNumber.size())}});
  plan.mixedPadding = digitCounts.size() > 1;
  for (auto& entry : byNumber) plan.files.push_back(entry.second);
  std::sort(plan.duplicates.begin(), plan.duplicates.end());
}

QString sequenceText(const QJsonObject& params, const QString& key,
                     const QString& pattern) {
  const QString value = requiredString(params, key);
  if (!QRegularExpression(pattern).match(value).hasMatch())
    throw Error("invalid_params",
                key + " has characters a sequence name cannot hold.",
                {{"reason", key}});
  return value;
}

// The status of one listed file from its header, before any decoding.
// The status and reason a file's directory entry gives, or empty when the
// entry is a regular file within the byte bound.
std::pair<QString, QString> entryStatus(const SourceFile& file) {
  if (file.entry == "missing") return {"not_found", QString()};
  if (file.entry == "link") return {"path_denied", "symbolic_link"};
  if (file.entry != "regular") return {"invalid_asset", "not_regular"};
  if (file.bytes > MaxSourceBytes) return {"resource_limit", "file_bytes"};
  return {};
}

// The status and reason a file's header gives.
std::pair<QString, QString> headerStatus(const ImageHeader& header) {
  if (decoderExtension(header.format).isEmpty())
    return {"unsupported_format", QString()};
  if (!header.valid) return {"invalid_asset", header.problem};
  if (!header.unsupported.isEmpty())
    return {"unsupported_format", header.unsupported};
  if (header.bitDepth > 16 || (header.format == "jpeg" && header.bitDepth != 8))
    return {"unsupported_precision", QString()};
  if (header.width > MaxSide || header.height > MaxSide ||
      qint64(header.width) * header.height > MaxPixels)
    return {"resource_limit", "pixels"};
  return {"supported", QString()};
}

void classify(SourceFile& file) {
  auto status = entryStatus(file);
  if (status.first.isEmpty()) status = headerStatus(file.header);
  file.status = status.first;
  file.reason = status.second;
}

void readHeader(SourcePlan& plan, SourceFile& file,
                const QHash<QString, EntryInfo>& listing) {
  const auto found = listing.constFind(file.name);
  if (found == listing.constEnd()) return classify(file);
  const EntryInfo& entry = found.value();
  file.entry             = entry.isSymLink() ? "link"
                           : entry.isFile()  ? "regular"
                                             : "other";
  file.bytes             = entry.size();
  file.modifiedMs        = entry.lastModified().toMSecsSinceEpoch();
  if (file.entry == "regular") {
    QFile input;
    if (openApprovedFile(input, plan.directory + '/' + file.name) ==
        OpenResult::Opened)
      file.header = readImageHeader(input, file.name);
    else
      file.entry = "other";
  }
  classify(file);
}

SourcePlan collectSources(Context& context, const QJsonObject& params) {
  SourcePlan plan;
  QVector<EntryInfo> entries;
  plan.directory = listedDirectory(context, params, entries);
  QHash<QString, EntryInfo> listing;
  for (const EntryInfo& entry : entries)
    listing.insert(entry.fileName(), entry);
  const bool files    = params.contains("file_names");
  const bool sequence = params.contains("sequence_prefix");
  if (files == sequence || (sequence && !params.contains("sequence_extension")))
    throw Error("invalid_params",
                "Give either file_names or sequence_prefix with "
                "sequence_extension.",
                {{"reason", "selection"}});
  if (files) {
    plan.selection = "files";
    addFileSelection(plan, params.value("file_names").toArray());
  } else {
    plan.selection = "sequence";
    plan.prefix =
        sequenceText(params, "sequence_prefix", "^[^/\\\\\\x00]{1,128}$");
    plan.extension =
        sequenceText(params, "sequence_extension", "^[A-Za-z0-9]{1,8}$");
    addSequenceSelection(plan, entries);
  }
  for (SourceFile& file : plan.files) readHeader(plan, file, listing);
  return plan;
}

QJsonObject headerJson(const ImageHeader& header) {
  return {{"format", header.format},
          {"width", nullable(header.valid, header.width)},
          {"height", nullable(header.valid, header.height)},
          {"bit_depth", nullable(header.valid, header.bitDepth)},
          {"pixel_format", nullable(header.valid, header.pixelFormat())},
          {"interlaced", nullable(header.valid, header.interlaced)},
          {"alpha", nullable(header.valid, header.alpha)},
          {"dpi_x", nullable(header.dpiKnown, header.dpiX)},
          {"dpi_y", nullable(header.dpiKnown, header.dpiY)},
          {"color_metadata", QJsonArray::fromStringList(header.colorMetadata)}};
}

QJsonObject fileRow(const SourceFile& file) {
  QJsonObject row = headerJson(file.header);
  row.insert("name", file.name);
  row.insert("drawing_id", drawingId(TFrameId(file.number)));
  row.insert("bytes", double(file.bytes));
  row.insert("status", file.status);
  row.insert("reason", nullable(!file.reason.isEmpty(), file.reason));
  return row;
}

QJsonArray gapRanges(const SourcePlan& plan, int& total) {
  QJsonArray ranges;
  total = 0;
  if (plan.selection != "sequence") return ranges;
  for (size_t index = 1; index < plan.files.size(); ++index) {
    const int before = plan.files[index - 1].number;
    const int after  = plan.files[index].number;
    if (after - before <= 1) continue;
    total += after - before - 1;
    if (ranges.size() < MaxReported)
      ranges.append(QJsonObject{{"first", before + 1}, {"last", after - 1}});
  }
  return ranges;
}

QJsonValue sequenceJson(const SourcePlan& plan) {
  if (plan.selection != "sequence") return QJsonValue();
  int missing           = 0;
  const QJsonArray gaps = gapRanges(plan, missing);
  QJsonArray duplicates;
  for (int number : plan.duplicates)
    if (duplicates.size() < MaxReported) duplicates.append(number);
  return QJsonObject{{"prefix", plan.prefix},
                     {"extension", plan.extension},
                     {"first", plan.files.front().number},
                     {"last", plan.files.back().number},
                     {"count", int(plan.files.size())},
                     {"contiguous", missing == 0},
                     {"missing_frames", missing},
                     {"gaps", gaps},
                     {"duplicate_numbers", duplicates},
                     {"mixed_padding", plan.mixedPadding}};
}

struct Uniformity final {
  bool size      = true;
  bool dpi       = true;
  int withoutDpi = 0;
  int withAlpha  = 0;
  qint64 pixels  = 0;
  qint64 bytes   = 0;
  int supported  = 0;
  bool format    = true;
  // Whether every file is an 8-bit PNG the worker decodes itself.
  bool inWorker = true;
  std::optional<ImageHeader> first;
};

Uniformity uniformity(const SourcePlan& plan) {
  Uniformity result;
  for (const SourceFile& file : plan.files) {
    result.bytes += file.bytes;
    if (file.status != "supported") continue;
    const ImageHeader& header = file.header;
    ++result.supported;
    result.pixels += qint64(header.width) * header.height;
    if (header.alpha != "none") ++result.withAlpha;
    if (!header.dpiKnown) ++result.withoutDpi;
    result.inWorker =
        result.inWorker && header.format == "png" && header.bitDepth <= 8;
    if (!result.first) {
      result.first = header;
      continue;
    }
    result.format = result.format && header.format == result.first->format;
    result.size   = result.size && header.width == result.first->width &&
                    header.height == result.first->height;
    result.dpi    = result.dpi && header.dpiKnown == result.first->dpiKnown &&
                    header.dpiX == result.first->dpiX &&
                    header.dpiY == result.first->dpiY;
  }
  return result;
}

// Whether the import runs as one call: few 8-bit PNG files; anything longer
// or decoded by the contained helper runs as a job.
bool runsInline(const SourcePlan& plan, const Uniformity& shape) {
  return shape.inWorker && int(plan.files.size()) <= InlineFiles &&
         shape.pixels <= InlinePixels;
}

QJsonObject summaryJson(const SourcePlan& plan, const Uniformity& shape) {
  QMap<QString, int> formats;
  for (const SourceFile& file : plan.files) ++formats[file.header.format];
  QJsonArray formatRows;
  for (auto it = formats.constBegin(); it != formats.constEnd(); ++it)
    formatRows.append(QJsonObject{{"format", it.key()}, {"count", it.value()}});
  const bool known = shape.first.has_value();
  const bool dpi   = known && shape.dpi && shape.first->dpiKnown;
  return {
      {"files", int(plan.files.size())},
      {"supported", shape.supported},
      {"formats", formatRows},
      {"uniform_size", known && shape.size},
      {"width", nullable(known && shape.size, known ? shape.first->width : 0)},
      {"height",
       nullable(known && shape.size, known ? shape.first->height : 0)},
      {"uniform_dpi", known && shape.dpi},
      {"dpi_x", nullable(dpi, dpi ? shape.first->dpiX : 0)},
      {"dpi_y", nullable(dpi, dpi ? shape.first->dpiY : 0)},
      {"files_without_dpi", shape.withoutDpi},
      {"files_with_alpha", shape.withAlpha},
      {"total_pixels", double(shape.pixels)},
      {"total_bytes", double(shape.bytes)}};
}

QJsonObject frameRateJson(Document& document) {
  return {
      {"source_rate", QJsonValue()},
      {"scene_frame_rate",
       document.scene->getProperties()->getOutputProperties()->getFrameRate()},
      {"handling", "one_row_per_source_frame"}};
}

struct Blockers final {
  QJsonArray rows;
  int total = 0;
  void add(const QString& code, const QString& reason,
           const QString& name = QString()) {
    ++total;
    if (rows.size() < MaxReported)
      rows.append(QJsonObject{{"code", code},
                              {"reason", reason},
                              {"name", nullable(!name.isEmpty(), name)}});
  }
};

void fileBlockers(const SourcePlan& plan, Blockers& blockers) {
  for (const SourceFile& file : plan.files)
    if (file.status != "supported")
      blockers.add(file.status,
                   file.reason.isEmpty() ? file.header.format : file.reason,
                   file.name);
  if (!plan.duplicates.isEmpty())
    blockers.add("invalid_params", "duplicate_numbers");
  if (plan.mixedPadding) blockers.add("invalid_params", "mixed_padding");
}

void capacityBlockers(Document& document, const SourcePlan& plan,
                      const Uniformity& shape, Blockers& blockers) {
  if (!shape.size) blockers.add("invalid_params", "mixed_size");
  if (!shape.dpi) blockers.add("invalid_params", "mixed_dpi");
  if (!shape.format) blockers.add("invalid_params", "mixed_formats");
  try {
    const qint64 bytes =
        shape.first
            ? automationRasterBytes(shape.first->width, shape.first->height) *
                  qint64(plan.files.size())
            : 0;
    requireAssetGrowth(*document.scene, bytes, int(plan.files.size()), 1);
  } catch (const Error& error) {
    blockers.add(error.code, "document_capacity");
  }
}

QByteArray planDigest(const SourcePlan& plan, const QJsonObject& params) {
  QJsonArray files;
  for (const SourceFile& file : plan.files) {
    QJsonObject row = headerJson(file.header);
    row.insert("name", file.name);
    row.insert("number", file.number);
    row.insert("bytes", double(file.bytes));
    row.insert("modified_ms", double(file.modifiedMs));
    files.append(row);
  }
  const QJsonObject key{
      {"directory", plan.directory},  {"selection", plan.selection},
      {"prefix", plan.prefix},        {"extension", plan.extension},
      {"name", params.value("name")}, {"files", files}};
  return QCryptographicHash::hash(
             QJsonDocument(key).toJson(QJsonDocument::Compact),
             QCryptographicHash::Sha256)
      .toHex();
}

// The native level path a linked sequence keeps, or empty when the native
// path cannot name every source file exactly.
TFilePath linkedLevelPath(const SourcePlan& plan) {
  if (plan.selection != "sequence" || plan.mixedPadding) return TFilePath();
  const TFilePath path(
      (plan.directory + '/' + plan.prefix + ".." + plan.extension)
          .toStdWString());
  for (const SourceFile& file : plan.files) {
    const TFrameId frame(file.number, QString(), file.digits);
    if (path.withFrame(frame).getQString() != plan.directory + '/' + file.name)
      return TFilePath();
  }
  return path;
}

QJsonValue existingLevelId(Document& document, const QString& name) {
  TXshLevel* level =
      document.scene->getLevelSet()->getLevel(name.toStdWString());
  return level ? QJsonValue(document.idForLevel(level)) : QJsonValue();
}

QJsonObject destinationJson(Document& document, const QJsonObject& params) {
  if (!params.contains("name")) return {};
  const QString name       = nameParam(params);
  const QJsonValue level   = existingLevelId(document, name);
  const QJsonObject record = level.isString()
                                 ? document.intake.value(level.toString())
                                 : QJsonObject();
  return {{"name", name},
          {"collision", level.isString() ? "level_exists" : "none"},
          {"existing_level_id", level},
          {"existing_storage",
           record.isEmpty() ? QJsonValue() : record.value("storage")}};
}

QJsonArray filePage(const SourcePlan& plan, int offset, int limit) {
  QJsonArray rows;
  for (int index = offset;
       index < int(plan.files.size()) && rows.size() < limit; ++index)
    rows.append(fileRow(plan.files[size_t(index)]));
  return rows;
}

QJsonValue preflight(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  if (params.contains("expected_revision") &&
      params.value("expected_revision").toString() != document.revisionText())
    throw Error("stale_revision", "The document changed since that revision.",
                {{"revision", document.revisionText()}});
  const int offset = integerParam(params, "offset", 0, MaxIntakeFiles, 0);
  const int limit =
      integerParam(params, "limit", 1, MaxIntakeFiles, DefaultPage);
  const SourcePlan plan  = collectSources(context, params);
  const Uniformity shape = uniformity(plan);
  Blockers blockers;
  fileBlockers(plan, blockers);
  capacityBlockers(document, plan, shape, blockers);
  const QJsonArray rows         = filePage(plan, offset, limit);
  const int end                 = offset + rows.size();
  const QJsonObject destination = destinationJson(document, params);
  return QJsonObject{
      {"document_id", document.id},
      {"revision", document.revisionText()},
      {"source_directory", plan.directory},
      {"selection", plan.selection},
      {"files", rows},
      {"offset", offset},
      {"next_offset",
       end < int(plan.files.size()) ? QJsonValue(end) : QJsonValue()},
      {"sequence", sequenceJson(plan)},
      {"summary", summaryJson(plan, shape)},
      {"frame_rate", frameRateJson(document)},
      {"destination", destination.isEmpty() ? QJsonValue() : destination},
      {"link_available", !linkedLevelPath(plan).isEmpty()},
      {"inline_limits",
       QJsonObject{{"files", InlineFiles}, {"pixels", double(InlinePixels)}}},
      {"execution", runsInline(plan, shape) ? "inline" : "job"},
      {"blockers", blockers.rows},
      {"blocker_total", blockers.total},
      {"importable", blockers.total == 0},
      {"plan_digest", QString::fromLatin1(planDigest(plan, params))}};
}

// ---------------------------------------------------------------- import --

struct ImportOptions final {
  QString name;
  QString storage;
  QString onExisting;
  QString dpiPolicy;
  double dpi = 0;
  QString onGap;
  QString exposure;
  int startFrame = 0;
};

QString choiceParam(const QJsonObject& params, const QString& key,
                    const QStringList& values, const QString& fallback) {
  const QString value =
      params.contains(key) ? requiredString(params, key) : fallback;
  if (!values.contains(value))
    throw Error("invalid_params", key + " is not one of the listed choices.");
  return value;
}

ImportOptions readOptions(const QJsonObject& params) {
  ImportOptions options;
  options.name    = nameParam(params);
  options.storage = choiceParam(params, "storage", {"copy", "link"}, {});
  options.onExisting =
      choiceParam(params, "on_existing", {"refuse", "reuse_identical"}, {});
  options.dpiPolicy =
      choiceParam(params, "dpi_policy", {"source", "value"}, {});
  options.onGap =
      choiceParam(params, "on_gap", {"refuse", "hold", "blank"}, "refuse");
  options.exposure =
      choiceParam(params, "exposure", {"none", "new_column"}, {});
  options.startFrame = integerParam(params, "start_frame", 0, LastRow, 0);
  if ((options.dpiPolicy == "value") != params.contains("dpi"))
    throw Error("invalid_params",
                "dpi is required with dpi_policy value and refused otherwise.",
                {{"reason", "dpi"}});
  if (options.dpiPolicy == "value")
    options.dpi = numberParam(params, "dpi", 1, 2400, 0);
  return options;
}

[[noreturn]] void refuseFile(const SourceFile& file) {
  const QString message =
      file.status == "unsupported_format"
          ? "Intake imports PNG only; this file is " + file.header.format + "."
          : "A source file cannot be imported (" + file.status + ").";
  throw Error(file.status, message,
              {{"name", file.name},
               {"format", file.header.format},
               {"reason", nullable(!file.reason.isEmpty(), file.reason)}});
}

void requireFrames(const SourcePlan& plan, const ImportOptions& options) {
  for (const SourceFile& file : plan.files)
    if (file.status != "supported") refuseFile(file);
  if (!plan.duplicates.isEmpty() || plan.mixedPadding)
    throw Error("invalid_params",
                "The sequence names a frame twice or mixes paddings.",
                {{"reason",
                  plan.mixedPadding ? "mixed_padding" : "duplicate_numbers"}});
  int missing           = 0;
  const QJsonArray gaps = gapRanges(plan, missing);
  if (missing > 0 && options.onGap == "refuse")
    throw Error(
        "invalid_params",
        "The sequence has missing frames; choose on_gap hold or blank.",
        {{"reason", "gaps"}, {"gaps", gaps}, {"missing_frames", missing}});
}

void requireShape(const SourcePlan& plan, const ImportOptions& options,
                  const Uniformity& shape) {
  if (!shape.size)
    throw Error("invalid_params", "Every drawing of a level has one size.",
                {{"reason", "mixed_size"}});
  if (options.dpiPolicy == "source" && (!shape.dpi || shape.withoutDpi > 0))
    throw Error(
        "invalid_asset",
        "dpi_policy source needs one pHYs density in every file.",
        {{"reason", shape.withoutDpi > 0 ? "dpi_missing" : "mixed_dpi"}});
  if (!shape.format)
    throw Error("invalid_params", "Every drawing of a level has one format.",
                {{"reason", "mixed_formats"}});
}

struct LoadedFile final {
  QByteArray sha256;
  qint64 bytes = 0;
  TRasterImageP raster;
  QString stagedCopy;
  QString decoder;
  QStringList conversion;
};

QImage decodePng(const QByteArray& bytes, const ImageHeader& header,
                 const QString& name) {
  QByteArray copy = bytes;
  QBuffer buffer(&copy);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer, "png");
  reader.setAutoDetectImageFormat(false);
  const QSize size = reader.size();
  if (size.width() != header.width || size.height() != header.height)
    throw Error("invalid_asset", "The PNG header and decoder disagree.",
                {{"name", name}, {"reason", "header_mismatch"}});
  const QImage image = reader.read();
  if (image.isNull() || image.size() != size)
    throw Error("invalid_asset", "PNG decoding failed: " + reader.errorString(),
                {{"name", name}, {"reason", "decode_failed"}});
  return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

TRasterImageP nativeRaster(const QImage& image, double dpi) {
  TRaster32P raster(image.width(), image.height());
  raster->lock();
  for (int y = 0; y < image.height(); ++y) {
    TPixel32* row      = raster->pixels(image.height() - y - 1);
    const QRgb* source = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x)
      row[x] = TPixel32(qRed(source[x]), qGreen(source[x]), qBlue(source[x]),
                        qAlpha(source[x]));
  }
  raster->unlock();
  TRasterImageP result(raster);
  result->setDpi(dpi, dpi);
  return result;
}

QByteArray readSource(const QString& directory, const SourceFile& file) {
  QFile input;
  if (openApprovedFile(input, directory + '/' + file.name) !=
      OpenResult::Opened)
    throw Error("source_changed", "A source file can no longer be opened.",
                {{"name", file.name}});
  const QByteArray bytes = input.read(MaxSourceBytes + 1);
  if (input.error() != QFileDevice::NoError)
    throw Error("io_error", "Cannot read a source file.",
                {{"name", file.name}});
  const ImageHeader reread = readImageHeader(bytes, file.name);
  if (bytes.size() != file.bytes ||
      reread.pixelFormat() != file.header.pixelFormat() ||
      reread.width != file.header.width || reread.height != file.header.height)
    throw Error("source_changed", "A source file changed while it was read.",
                {{"name", file.name}});
  return bytes;
}

double importDpi(const ImportOptions& options, const ImageHeader& header) {
  return options.dpiPolicy == "value" ? options.dpi : header.dpiX;
}

// Whether the worker decodes the file itself (8-bit PNG through Qt) rather
// than a contained helper running the native reader.
bool decodedInWorker(const ImageHeader& header) {
  return header.format == "png" && header.bitDepth <= 8;
}

// Everything one file's decode needs, copied so it can run off the
// application thread.
struct DecodeTask final {
  QString directory;
  SourceFile file;
  double dpi = 0;
  // Empty when no private copy is staged.
  QString stagingDirectory;
  QString scratchDirectory;
  QString childFault;
  // Test-only write fault intake:copy: the copy fails half written, as on
  // a full disk.
  bool copyFault = false;
};

QStringList conversionOf(const ImageHeader& header,
                         bool premultipliedByHelper) {
  QStringList steps;
  if (header.bitDepth == 16) steps.append("depth_16_to_8_rounded");
  const QString pixel = header.pixelFormat();
  if (pixel.startsWith("grey") || pixel.startsWith("indexed") ||
      pixel == "rgb5")
    steps.append("expanded_to_rgba8");
  if (header.alpha == "none") steps.append("opaque_alpha_added");
  if (header.format == "jpeg") steps.append("lossy_source");
  if (pixel == "rgbx8") steps.append("fourth_byte_ignored");
  if (premultipliedByHelper) steps.append("straight_alpha_premultiplied");
  return steps;
}

QStringList helperArguments(const QString& directory, const QString& extension,
                            const QString& childFault) {
  QStringList arguments{"--automation-worker", "--workspace", directory,
                        "--parse-helper", "image_decode:" + extension};
#ifdef BUILD_UI_PORT_TESTS
  if (!childFault.isEmpty()) arguments << "--test-child-fault" << childFault;
#else
  Q_UNUSED(childFault);
#endif
  return arguments;
}

// Run the helper to its end, killing it on cancellation, at the deadline or
// past the memory bound; returns its one reply line.
QByteArray superviseDecode(QProcess& helper, const std::atomic<bool>& stop,
                           const QString& name) {
  QElapsedTimer timer;
  timer.start();
  helper.start();
  QString cause = helper.waitForStarted(SharedParseDeadlineMs) ? "" : "crashed";
  QByteArray output;
  while (cause.isEmpty() && helper.state() != QProcess::NotRunning) {
    helper.waitForFinished(20);
    output += helper.readAllStandardOutput();
    if (stop.load()) cause = "canceled";
    if (timer.elapsed() > SharedParseDeadlineMs) cause = "timeout";
    if (processMemoryBytes(helper.processId()) > SharedParseMemoryBytes)
      cause = "memory";
  }
  if (!cause.isEmpty()) {
    helper.kill();
    helper.waitForFinished(3000);
  }
  output += helper.readAllStandardOutput();
  if (cause.isEmpty() &&
      (helper.exitStatus() != QProcess::NormalExit || helper.exitCode() != 0))
    cause = "crashed";
  if (cause == "canceled") throw Error("canceled", "The import was canceled.");
  if (!cause.isEmpty())
    throw Error("invalid_asset", "The contained decoder did not finish.",
                {{"name", name}, {"reason", "decoder_" + cause}});
  return output;
}

QJsonObject decoderSummary(const QByteArray& output, const QString& name) {
  const QJsonObject reply = QJsonDocument::fromJson(output.trimmed()).object();
  const QString status    = reply.value("status").toString();
  if (status == "parsed") return reply.value("summary").toObject();
  const QString message = reply.value("message").toString();
  const QString reason  = status == "limit" ? "pixels"
                          : message.startsWith("corrupt_data")
                              ? "corrupt_data"
                              : "decoder_rejected";
  const QJsonObject details{
      {"name", name},
      {"reason", reason},
      {"native_message", reply.value("message").toString().left(256)}};
  if (status == "limit")
    throw Error("resource_limit", "The image is outside the bounds.", details);
  throw Error("invalid_asset", "The native reader refused the file.", details);
}

// Decode one file with its native reader in a contained helper process; the
// helper writes 8-bit premultiplied pixels as a PNG this worker then reads.
QImage decodeInHelper(const DecodeTask& task, const QByteArray& bytes,
                      const std::atomic<bool>& stop, bool& premultiplied) {
  const SourceFile& file  = task.file;
  const QString extension = decoderExtension(file.header.format);
  QTemporaryDir directory(task.scratchDirectory + "/intake-decode-XXXXXX");
  QFile input(directory.filePath("input." + extension));
  if (!directory.isValid() || !input.open(QIODevice::WriteOnly) ||
      input.write(bytes) != bytes.size() || !input.flush())
    throw Error("io_error", "Cannot write the private decode copy.",
                {{"name", file.name}});
  input.close();
  QProcess helper;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.remove("GROUNDZERO_AUTOMATION_CONTROL_FD");
  helper.setProcessEnvironment(environment);
  helper.setWorkingDirectory(directory.path());
  helper.setStandardInputFile(QProcess::nullDevice());
  helper.setStandardErrorFile(QProcess::nullDevice());
  helper.setProgram(QCoreApplication::applicationFilePath());
  helper.setArguments(
      helperArguments(directory.path(), extension, task.childFault));
  const QJsonObject summary =
      decoderSummary(superviseDecode(helper, stop, file.name), file.name);
  premultiplied = summary.value("alpha_premultiplied_by_helper").toBool();
  if (summary.value("width").toInt() != file.header.width ||
      summary.value("height").toInt() != file.header.height)
    throw Error("invalid_asset", "The header and the decoded image disagree.",
                {{"name", file.name}, {"reason", "header_mismatch"}});
  QImageReader reader(directory.filePath("decoded.png"), "png");
  const QImage image = reader.read();
  if (image.size() != QSize(file.header.width, file.header.height))
    throw Error("invalid_asset", "The decoded pixels cannot be read back.",
                {{"name", file.name}, {"reason", "decode_failed"}});
  return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QString stagedCopyPath(const DecodeTask& task) {
  return QDir(task.stagingDirectory)
      .filePath(QString::number(task.file.number) + '.' +
                decoderExtension(task.file.header.format));
}

// Read, hash, decode and stage one source file. Runs on any thread.
LoadedFile decodeFile(const DecodeTask& task, const std::atomic<bool>& stop) {
  const QByteArray bytes = readSource(task.directory, task.file);
  LoadedFile entry;
  entry.bytes = bytes.size();
  entry.sha256 =
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
  bool premultiplied  = false;
  const bool inWorker = decodedInWorker(task.file.header);
  const QImage image  = inWorker
                            ? decodePng(bytes, task.file.header, task.file.name)
                            : decodeInHelper(task, bytes, stop, premultiplied);
  entry.decoder    = inWorker ? QString("qt_png_in_worker")
                              : "native_" + task.file.header.format + "_helper";
  entry.conversion = conversionOf(task.file.header, premultiplied);
  entry.raster     = nativeRaster(image, task.dpi);
  if (task.stagingDirectory.isEmpty()) return entry;
  entry.stagedCopy = stagedCopyPath(task);
  QSaveFile output(entry.stagedCopy);
  const QByteArray written =
      task.copyFault ? bytes.left(bytes.size() / 2) : bytes;
  if (!output.open(QIODevice::WriteOnly) ||
      output.write(written) != bytes.size() || !output.commit())
    throw Error("io_error", "Cannot stage a private copy of a source file.",
                {{"name", task.file.name}});
  return entry;
}

DecodeTask decodeTask(Context& context, const SourcePlan& plan,
                      const ImportOptions& options, const SourceFile& file,
                      const QString& staging) {
  DecodeTask task;
  task.directory        = plan.directory;
  task.file             = file;
  task.dpi              = importDpi(options, file.header);
  task.stagingDirectory = staging;
  task.scratchDirectory = context.scratchDirectory();
  task.copyFault        = context.writeFaultArmed("intake:copy");
#ifdef BUILD_UI_PORT_TESTS
  task.childFault = context.childFault;
#endif
  return task;
}

std::vector<LoadedFile> loadSources(Context& context, const SourcePlan& plan,
                                    const ImportOptions& options,
                                    PrivateDirectory* staging) {
  const std::atomic<bool> never{false};
  std::vector<LoadedFile> loaded;
  for (const SourceFile& file : plan.files)
    loaded.push_back(
        decodeFile(decodeTask(context, plan, options, file,
                              staging ? staging->path() : QString()),
                   never));
  return loaded;
}

QJsonArray sourceRecords(const SourcePlan& plan,
                         const std::vector<LoadedFile>& loaded) {
  QJsonArray sources;
  for (size_t index = 0; index < plan.files.size(); ++index) {
    const SourceFile& file = plan.files[index];
    sources.append(QJsonObject{
        {"drawing_id", drawingId(TFrameId(file.number))},
        {"name", file.name},
        {"bytes", double(loaded[index].bytes)},
        {"modified_ms", double(file.modifiedMs)},
        {"sha256", QString::fromLatin1(loaded[index].sha256)},
        {"width", file.header.width},
        {"height", file.header.height},
        {"pixel_format", file.header.pixelFormat()},
        {"alpha", file.header.alpha},
        {"stored_as", "rgba8_premultiplied"},
        {"decoder", loaded[index].decoder},
        {"conversion", QJsonArray::fromStringList(loaded[index].conversion)},
        {"color_metadata",
         QJsonArray::fromStringList(file.header.colorMetadata)}});
  }
  return sources;
}

bool sameSources(const QJsonArray& recorded, const QJsonArray& current) {
  if (recorded.size() != current.size()) return false;
  for (int index = 0; index < recorded.size(); ++index) {
    const QJsonObject a = recorded[index].toObject();
    const QJsonObject b = current[index].toObject();
    if (a.value("drawing_id") != b.value("drawing_id") ||
        a.value("sha256") != b.value("sha256"))
      return false;
  }
  return true;
}

// The existing level's record when it is the same import, else name_conflict.
QJsonObject reusable(Document& document, const QString& levelId,
                     const QJsonObject& wanted) {
  const QJsonObject record = document.intake.value(levelId);
  QStringList differences;
  if (record.isEmpty()) differences.append("no_intake_record");
  for (const char* key : {"storage", "dpi_x", "dpi_y"})
    if (!record.isEmpty() && record.value(key) != wanted.value(key))
      differences.append(key);
  if (!record.isEmpty() && !sameSources(record.value("sources").toArray(),
                                        wanted.value("sources").toArray()))
    differences.append("sources");
  if (!differences.isEmpty())
    throw Error("name_conflict", "A different level already has that name.",
                {{"reason", "not_identical"},
                 {"level_id", levelId},
                 {"differences", QJsonArray::fromStringList(differences)}});
  return record;
}

QString uniqueLevelBase() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

TFrameId frameFor(const SourceFile& file, const QString& storage) {
  return storage == "link" ? TFrameId(file.number, QString(), file.digits)
                           : TFrameId(file.number);
}

// Move the staged copies into the level store under the native frame names
// of `path`; on failure remove the moved ones, so nothing is left.
QJsonArray publishCopies(const SourcePlan& plan,
                         const std::vector<LoadedFile>& loaded,
                         const TFilePath& path, bool interrupted) {
  QJsonArray copies;
  QStringList moved;
  for (size_t index = 0; index < plan.files.size(); ++index) {
    const QString target =
        path.withFrame(TFrameId(plan.files[index].number)).getQString();
    // Test-only write fault intake:publish stops the moves halfway.
    const bool stopped = interrupted && index == plan.files.size() / 2;
    if (stopped || publishApproved(loaded[index].stagedCopy, target) !=
                       PublishResult::Published) {
      for (const QString& name : moved) QFile::remove(name);
      throw Error("io_error", "Cannot place a private copy in the level store.",
                  {{"name", plan.files[index].name}});
    }
    moved.append(target);
    copies.append(QJsonObject{
        {"drawing_id", drawingId(TFrameId(plan.files[index].number))},
        {"source_name", plan.files[index].name},
        {"copy_name", QFileInfo(target).fileName()},
        {"bytes", double(loaded[index].bytes)},
        {"sha256", QString::fromLatin1(loaded[index].sha256)}});
  }
  return copies;
}

TFilePath privateLevelPath(Context& context, const QString& extension) {
  const QString directory =
      context.resolvePath(context.privateDirectory("levels"), true);
  if (!QDir().mkpath(directory))
    throw Error("io_error", "Cannot create the workspace level directory.");
  return TFilePath(QDir(directory)
                       .filePath(uniqueLevelBase() + ".." + extension)
                       .toStdWString());
}

TXshSimpleLevel* insertLevel(Document& document, const SourcePlan& plan,
                             const ImportOptions& options,
                             std::vector<LoadedFile>& loaded,
                             const TFilePath& path) {
  auto level = std::make_unique<TXshSimpleLevel>(options.name.toStdWString());
  level->setScene(document.scene.get());
  level->setType(OVL_XSHLEVEL);
  level->setPath(path, true);
  const ImageHeader& first = plan.files.front().header;
  const double dpi         = importDpi(options, first);
  level->getProperties()->setDpiPolicy(LevelProperties::DP_CustomDpi);
  level->getProperties()->setDpi(dpi);
  level->getProperties()->setImageRes(TDimension(first.width, first.height));
  for (size_t index = 0; index < plan.files.size(); ++index)
    level->setFrame(frameFor(plan.files[index], options.storage),
                    loaded[index].raster);
  level->setDirtyFlag(true);
  TXshSimpleLevel* result = level.get();
  if (!document.scene->getLevelSet()->insertLevel(result))
    throw Error("native_error", "Could not register the imported level.");
  level.release();
  return result;
}

std::vector<TXshCell> exposureCells(const SourcePlan& plan,
                                    TXshSimpleLevel* level,
                                    const QString& onGap) {
  std::vector<TXshCell> cells;
  const bool sequence                = plan.selection == "sequence";
  const std::vector<TFrameId> frames = level->getFids();
  for (size_t index = 0; index < plan.files.size(); ++index) {
    const TXshCell cell(level, frames[index]);
    if (sequence && index > 0) {
      const int missing =
          plan.files[index].number - plan.files[index - 1].number - 1;
      const TXshCell filler = onGap == "hold" ? cells.back() : TXshCell();
      cells.insert(cells.end(), size_t(std::max(0, missing)), filler);
    }
    cells.push_back(cell);
  }
  return cells;
}

int exposureLength(const SourcePlan& plan) {
  if (plan.selection != "sequence") return int(plan.files.size());
  return plan.files.back().number - plan.files.front().number + 1;
}

void requireExposureRoom(Document& document, const SourcePlan& plan,
                         const ImportOptions& options) {
  if (options.exposure != "new_column") return;
  requireAutomationPathObjectGrowth(*document.scene);
  if (document.scene->getXsheet()->getColumnCount() >= MaxEntities)
    throw Error("resource_limit", "The document already has 4096 columns.");
  if (options.startFrame + exposureLength(plan) - 1 > LastRow)
    throw Error("invalid_params",
                "The exposure ends past frame 999999, the last native row.",
                {{"reason", "frame_range"}});
}

QJsonValue exposeLevel(Document& document, const SourcePlan& plan,
                       const ImportOptions& options, TXshSimpleLevel* level) {
  if (options.exposure != "new_column") return QJsonValue();
  TXsheet* sheet  = document.scene->getXsheet();
  const int index = sheet->getColumnCount();
  sheet->insertColumn(index, new TXshLevelColumn());
  TXshColumn* column = sheet->getColumn(index);
  column->setPreviewVisible(true);
  column->setCamstandVisible(true);
  sheet->getStageObject(TStageObjectId::ColumnId(index))
      ->setName(options.name.toStdString());
  const std::vector<TXshCell> cells = exposureCells(plan, level, options.onGap);
  CellPlan cellPlan;
  cellPlan.column   = column;
  cellPlan.index    = index;
  cellPlan.columnId = document.idForColumn(column);
  cellPlan.before   = CellModel::read(*sheet, index);
  cellPlan.ops.push_back(
      {CellOp::Write, options.startFrame, int(cells.size()), cells, {}, {}});
  modelCellPlan(cellPlan);
  std::vector<CellPlan> plans{cellPlan};
  applyCellPlans(document, plans);
  int missing = 0;
  gapRanges(plan, missing);
  return QJsonObject{{"column_id", cellPlan.columnId},
                     {"start_frame", options.startFrame},
                     {"length", int(cells.size())},
                     {"gap_rows", missing},
                     {"on_gap", options.onGap}};
}

QJsonObject wantedRecord(const SourcePlan& plan, const ImportOptions& options,
                         const std::vector<LoadedFile>& loaded) {
  const double dpi = importDpi(options, plan.files.front().header);
  return {
      {"storage", options.storage},
      {"source_directory", plan.directory},
      {"selection", plan.selection},
      {"sequence_prefix", nullable(plan.selection == "sequence", plan.prefix)},
      {"sequence_extension",
       nullable(plan.selection == "sequence", plan.extension)},
      {"dpi_policy", options.dpiPolicy},
      {"dpi_x", dpi},
      {"dpi_y", dpi},
      {"sources", sourceRecords(plan, loaded)}};
}

QJsonObject importResult(Document& document, TXshLevel* level,
                         const QJsonObject& record, const QString& outcome) {
  QJsonArray drawings;
  for (const QJsonValue& value : record.value("sources").toArray()) {
    QJsonObject row = value.toObject();
    row.remove("modified_ms");
    drawings.append(row);
  }
  return {{"outcome", outcome},
          {"level", describeLevel(document, level, true)},
          {"storage", record.value("storage")},
          {"source_directory", record.value("source_directory")},
          {"dpi_policy", record.value("dpi_policy")},
          {"dpi_x", record.value("dpi_x")},
          {"dpi_y", record.value("dpi_y")},
          {"drawings", drawings},
          {"frame_rate", frameRateJson(document)}};
}

QJsonObject reuseExisting(Document& document, const QString& name,
                          const QJsonObject& wanted) {
  TXshLevel* level =
      document.scene->getLevelSet()->getLevel(name.toStdWString());
  const QString levelId = document.idForLevel(level);
  QJsonObject result    = importResult(
      document, level, reusable(document, levelId, wanted), "reused");
  result.insert("copied_files", QJsonArray());
  result.insert("exposure", QJsonValue());
  return result;
}

void requirePlanDigest(const SourcePlan& plan, const QJsonObject& params) {
  if (!params.contains("plan_digest")) return;
  const QByteArray current = planDigest(plan, params);
  if (params.value("plan_digest").toString().toLatin1() != current)
    throw Error("source_changed",
                "The sources or destination changed since the preflight.",
                {{"plan_digest", QString::fromLatin1(current)}});
}

TFilePath levelPathFor(Context& context, const SourcePlan& plan,
                       const ImportOptions& options) {
  if (options.storage == "copy")
    return privateLevelPath(context,
                            decoderExtension(plan.files.front().header.format));
  const TFilePath linked = linkedLevelPath(plan);
  if (linked.isEmpty())
    throw Error("invalid_params",
                "storage link needs a sequence whose names the native level "
                "path reads back exactly.",
                {{"reason", "link_unavailable"}});
  return linked;
}

// An import that passed every check that needs no decoding.
struct ImportPlan final {
  SourcePlan sources;
  ImportOptions options;
  Uniformity shape;
  TFilePath path;
  bool exists       = false;
  bool copy         = false;
  bool publishFault = false;
};

ImportPlan planImport(Context& context, Document& document,
                      const QJsonObject& params) {
  ImportPlan plan;
  plan.options = readOptions(params);
  plan.sources = collectSources(context, params);
  plan.shape   = uniformity(plan.sources);
  requireFrames(plan.sources, plan.options);
  requireShape(plan.sources, plan.options, plan.shape);
  requirePlanDigest(plan.sources, params);
  plan.exists = existingLevelId(document, plan.options.name).isString();
  if (plan.exists && plan.options.onExisting == "refuse")
    throw Error("name_conflict", "A level with that name already exists.",
                {{"reason", "exists"}});
  plan.path         = levelPathFor(context, plan.sources, plan.options);
  plan.copy         = plan.options.storage == "copy" && !plan.exists;
  plan.publishFault = context.writeFaultArmed("intake:publish");
  if (plan.exists) return plan;
  const ImageHeader& first = *plan.shape.first;
  requireAssetGrowth(*document.scene,
                     automationRasterBytes(first.width, first.height) *
                         qint64(plan.sources.files.size()),
                     int(plan.sources.files.size()), 1);
  requireExposureRoom(document, plan.sources, plan.options);
  return plan;
}

// Put decoded files into the document. `changed` runs once the cast has
// changed; before that a refusal leaves the document and the level store as
// they were.
QJsonObject applyImport(Document& document, const ImportPlan& plan,
                        std::vector<LoadedFile>& loaded,
                        const std::function<void()>& changed) {
  const QJsonObject wanted = wantedRecord(plan.sources, plan.options, loaded);
  if (plan.exists) return reuseExisting(document, plan.options.name, wanted);
  const QJsonArray copies =
      plan.copy
          ? publishCopies(plan.sources, loaded, plan.path, plan.publishFault)
          : QJsonArray();
  TXshSimpleLevel* level = nullptr;
  try {
    level =
        insertLevel(document, plan.sources, plan.options, loaded, plan.path);
  } catch (const Error&) {
    const QDir store(plan.path.getParentDir().getQString());
    for (const QJsonValue& copied : copies)
      QFile::remove(
          store.filePath(copied.toObject().value("copy_name").toString()));
    throw;
  }
  changed();
  QJsonObject record = wanted;
  record.insert("copies", copies);
  document.intake.insert(document.idForLevel(level), record);
  const QJsonValue exposure =
      exposeLevel(document, plan.sources, plan.options, level);
  QJsonObject result = importResult(document, level, record, "created");
  result.insert("copied_files", copies);
  result.insert("exposure", exposure);
  return result;
}

// ------------------------------------------------------------ import job --

constexpr int ImportDeadlineMs      = 600000;
constexpr qint64 MaxJobStorageBytes = 536870912;

// One decode at a time: each holds a source file and its pixels in memory.
QThreadPool& decodePool() {
  static QThreadPool* pool = []() {
    auto* created = new QThreadPool();
    created->setMaxThreadCount(1);
    return created;
  }();
  return *pool;
}

struct DecodeOutcome final {
  LoadedFile file;
  QString code;
  QString message;
  QJsonObject details;
};

class IntakeJob final : public Job,
                        public std::enable_shared_from_this<IntakeJob> {
public:
  enum class State { Running, Completed, Failed, Canceled, DeadlineExceeded };

  Context* context = nullptr;
  JobCommon common;
  ImportPlan plan;
  std::shared_ptr<PrivateDirectory> staging;
  std::vector<LoadedFile> loaded;
  std::shared_ptr<std::atomic<bool>> stop =
      std::make_shared<std::atomic<bool>>(false);
  std::optional<std::future<DecodeOutcome>> pending;
  State state    = State::Running;
  size_t decoded = 0;
  QJsonObject outcome;
  std::atomic<qint64> interruptMs{-1};

  QString id() const override { return common.id; }
  JobLane lane() const override { return JobLane::Analysis; }
  QString documentId() const override { return common.documentId; }
  bool terminal() const override { return state != State::Running; }
  qint64 reservedBytes() const override {
    return plan.copy ? qint64(plan.shape.bytes) : 0;
  }
  QJsonObject record() const override;
  QJsonValue result(Context& owner, const QJsonObject& params) override;

  void cancel() override {
    if (common.cancelRequested) return;
    const qint64 asked     = interruptMs.load();
    common.cancelRequested = true;
    common.cancelMs        = asked >= 0 ? asked : now();
    if (!terminal()) finish(State::Canceled);
  }
  void interrupt(qint64 requestedMs) override {
    qint64 unset = -1;
    interruptMs.compare_exchange_strong(unset, requestedMs);
    stop->store(true);
  }

  qint64 now() const { return jobRegistry(*context).elapsedMs(); }
  void schedule(int delayMs) {
    QTimer::singleShot(delayMs, QCoreApplication::instance(),
                       [self = shared_from_this()]() { self->tick(); });
  }

private:
  void tick() {
    const int delay = step();
    jobRegistry(*context).publish();
    if (delay >= 0) schedule(delay);
  }
  int step();
  bool collect();
  void startDecode();
  void apply();
  void finish(State end);
  void fail(const QString& code, const QString& message,
            const QJsonObject& details, const QString& phase);
  QString statusText() const;
};

QString IntakeJob::statusText() const {
  switch (state) {
  case State::Completed:
    return "completed";
  case State::Failed:
    return "failed";
  case State::Canceled:
    return "canceled";
  case State::DeadlineExceeded:
    return "deadline_exceeded";
  case State::Running:
    break;
  }
  return common.cancelRequested ? "cancellation_requested" : "running";
}

// Ends the job; the decoded pixels and the staged copies go with it (a decode
// still running keeps the staging folder until it returns, then removes it).
void IntakeJob::finish(State end) {
  stop->store(true);
  pending.reset();
  loaded.clear();
  staging.reset();
  state          = end;
  common.endedMs = now();
}

void IntakeJob::fail(const QString& code, const QString& message,
                     const QJsonObject& details, const QString& phase) {
  const QString name   = details.value("name").toString();
  const QString reason = details.value("reason").toString();
  QString text         = message;
  if (!name.isEmpty()) text += " File: " + name + '.';
  if (!reason.isEmpty()) text += " Reason: " + reason + '.';
  common.failure = QJsonObject{
      {"code", code}, {"phase", phase}, {"message", text.left(2048)}};
  finish(State::Failed);
}

void IntakeJob::startDecode() {
  const SourceFile& file = plan.sources.files[loaded.size()];
  const DecodeTask task = decodeTask(*context, plan.sources, plan.options, file,
                                     plan.copy ? staging->path() : "");
  auto promise          = std::make_shared<std::promise<DecodeOutcome>>();
  pending               = promise->get_future();
  decodePool().start([task, promise, flag = stop, folder = staging]() {
    DecodeOutcome result;
    try {
      result.file = decodeFile(task, *flag);
    } catch (const Error& error) {
      result.code    = error.code;
      result.message = error.message;
      result.details = error.details;
    } catch (const std::exception& error) {
      result.code    = "native_error";
      result.message = QString::fromUtf8(error.what());
    }
    promise->set_value(result);
  });
}

// Take a finished decode; false when the job failed on it.
bool IntakeJob::collect() {
  DecodeOutcome result = pending->get();
  pending.reset();
  if (!result.code.isEmpty()) {
    fail(result.code, result.message, result.details, "decode");
    return false;
  }
  loaded.push_back(result.file);
  ++decoded;
  return true;
}

void IntakeJob::apply() {
  try {
    Document& document = context->document(common.documentId);
    if (document.revisionText() != common.revision)
      throw Error("revision_changed",
                  "The document changed after the import began.",
                  {{"revision", document.revisionText()}});
    bool changed = false;
    outcome =
        applyImport(document, plan, loaded, [&changed]() { changed = true; });
    if (changed) ++document.revision;
    outcome.insert("document_id", document.id);
    outcome.insert("revision", document.revisionText());
    finish(State::Completed);
  } catch (const Error& error) {
    fail(error.code, error.message, error.details, "apply");
  }
}

// One slice: start or collect a decode, or apply at the end. Returns the
// delay before the next slice, or -1 when the job has ended.
int IntakeJob::step() {
  if (terminal()) return -1;
  if (common.startedMs < 0) common.startedMs = now();
  if (common.cancelRequested || stop->load()) {
    if (!common.cancelRequested) cancel();
    return -1;
  }
  if (now() - common.startedMs > ImportDeadlineMs) {
    finish(State::DeadlineExceeded);
    return -1;
  }
  if (pending) {
    if (pending->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return 5;
    if (!collect()) return -1;
  }
  if (loaded.size() < plan.sources.files.size()) {
    startDecode();
    return 5;
  }
  apply();
  return -1;
}

QJsonObject IntakeJob::record() const {
  const QJsonObject limits{{"deadline_ms", ImportDeadlineMs},
                           {"storage_reserved_bytes", double(reservedBytes())},
                           {"cancel_grace_ms", CancelGraceMs}};
  const QJsonObject progress{{"unit", "files"},
                             {"done", double(decoded)},
                             {"total", double(plan.sources.files.size())}};
  const int rows = outcome.value("drawings").toArray().size();
  const QJsonObject outputs{{"directory", QJsonValue()},
                            {"artifact_count", 0},
                            {"result_rows", rows}};
  QJsonObject result = commonRecord(*context, common, statusText(), terminal(),
                                    limits, progress, outputs);
  result.insert("intake", QJsonObject{{"files", int(plan.sources.files.size())},
                                      {"storage", plan.options.storage},
                                      {"level_name", plan.options.name}});
  return result;
}

QJsonValue IntakeJob::result(Context&, const QJsonObject& params) {
  if (state != State::Completed)
    throw Error("result_unavailable",
                "The import has no result; poll job.status.",
                {{"status", statusText()}, {"failure", common.failure}});
  const int offset          = integerParam(params, "offset", 0, 65536, 0);
  const int limit           = integerParam(params, "limit", 1, 256, 64);
  const QJsonArray drawings = outcome.value("drawings").toArray();
  const QJsonArray copies   = outcome.value("copied_files").toArray();
  QJsonArray rows;
  for (int index = offset; index < drawings.size() && rows.size() < limit;
       ++index) {
    QJsonObject row = drawings[index].toObject();
    row.insert("copy_name", index < copies.size()
                                ? copies[index].toObject().value("copy_name")
                                : QJsonValue());
    rows.append(row);
  }
  QJsonObject summary = outcome;
  summary.remove("drawings");
  summary.remove("copied_files");
  summary.insert("copied_file_count", copies.size());
  const int end = offset + rows.size();
  return QJsonObject{
      {"job_id", common.id},
      {"kind", common.kind},
      {"status", statusText()},
      {"summary", summary},
      {"rows", rows},
      {"total_rows", drawings.size()},
      {"offset", offset},
      {"next_offset", end < drawings.size() ? QJsonValue(end) : QJsonValue()},
      {"source_revision", common.revision}};
}

QJsonObject submitImportJob(Context& context, Document& document,
                            const ImportPlan& plan, const QJsonObject& params) {
  if (plan.copy && plan.shape.bytes > MaxJobStorageBytes)
    throw Error(
        "resource_limit",
        "The private copies of one import job hold at most 512 MiB.",
        {{"limit", "job_storage"}, {"maximum", double(MaxJobStorageBytes)}});
  JobRegistry& registry = jobRegistry(context);
  registry.admitAnalysis({}, 0);
  auto job     = std::make_shared<IntakeJob>();
  job->context = &context;
  job->plan    = plan;
  job->staging = std::make_shared<PrivateDirectory>(context.scratchDirectory() +
                                                    "/intake-job-XXXXXX");
  if (!job->staging->isValid())
    throw Error("io_error", "Cannot create the import job's staging folder.");
  job->common.id          = registry.newJobId();
  job->common.kind        = "intake_import";
  job->common.lane        = JobLane::Analysis;
  job->common.documentId  = document.id;
  job->common.revision    = document.revisionText();
  job->common.request     = params;
  job->common.submittedMs = registry.elapsedMs();
  job->common.request.remove("operation_id");
  registry.add(job);
  job->schedule(0);
  return {{"outcome", "job_started"}, {"job", job->record()}};
}

QJsonValue importIntake(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  ImportPlan plan    = planImport(context, document, params);
  if (!runsInline(plan.sources, plan.shape))
    return submitImportJob(context, document, plan, params);
  PrivateDirectory staging(context.scratchDirectory() + "/intake-XXXXXX");
  if (!staging.isValid())
    throw Error("io_error", "Cannot create a private staging directory.");
  std::vector<LoadedFile> loaded = loadSources(
      context, plan.sources, plan.options, plan.copy ? &staging : nullptr);
  return applyImport(document, plan, loaded,
                     [&context]() { context.markMutationStarted(); });
}

// ------------------------------------------------------------ provenance --

QString levelPathKind(Context& context, Document& document,
                      TXshSimpleLevel* level, const QJsonObject& record) {
  const QString path = QString::fromStdWString(
      document.scene->decodeFilePath(level->getPath()).getWideString());
  if (QFileInfo(path).absolutePath() == context.privateDirectory("levels"))
    return "private_store";
  if (record.value("storage") == "link" &&
      QFileInfo(path).absolutePath() ==
          record.value("source_directory").toString())
    return "linked_source";
  return "package";
}

QJsonArray sourceStatus(Context& context, const QJsonObject& record,
                        QJsonObject& counts) {
  QVector<EntryInfo> entries;
  QHash<QString, EntryInfo> listing;
  bool listed = false;
  try {
    const QString directory =
        context.resolvePath(record.value("source_directory").toString(), false);
    listed = listApprovedDirectory(directory, entries, MaxListing) ==
             ListResult::Listed;
  } catch (const Error&) {
    listed = false;
  }
  for (const EntryInfo& entry : entries)
    listing.insert(entry.fileName(), entry);
  QJsonArray rows;
  QMap<QString, int> totals;
  for (const QJsonValue& value : record.value("sources").toArray()) {
    QJsonObject row  = value.toObject();
    const auto found = listing.constFind(row.value("name").toString());
    QString status   = "missing";
    if (!listed)
      status = "unreachable";
    else if (found != listing.constEnd())
      status = found->isFile() &&
                       double(found->size()) == row.value("bytes").toDouble() &&
                       double(found->lastModified().toMSecsSinceEpoch()) ==
                           row.value("modified_ms").toDouble()
                   ? "unchanged"
                   : "modified";
    ++totals[status];
    row.insert("source_status", status);
    rows.append(row);
  }
  counts = {{"unchanged", totals.value("unchanged")},
            {"modified", totals.value("modified")},
            {"missing", totals.value("missing")},
            {"unreachable", totals.value("unreachable")}};
  return rows;
}

QJsonValue provenance(Context& context, const QJsonObject& params) {
  Document& document    = getDocument(context, params);
  const QString levelId = requiredString(params, "level_id");
  TXshLevel* level      = document.level(levelId);
  const int offset      = integerParam(params, "offset", 0, MaxIntakeFiles, 0);
  const int limit =
      integerParam(params, "limit", 1, MaxIntakeFiles, DefaultPage);
  const QJsonObject record = document.intake.value(levelId);
  TXshSimpleLevel* simple  = level->getSimpleLevel();
  QJsonObject result{{"document_id", document.id},
                     {"revision", document.revisionText()},
                     {"level_id", levelId},
                     {"name", QString::fromStdWString(level->getName())},
                     {"recorded", !record.isEmpty()},
                     {"offset", offset},
                     {"next_offset", QJsonValue()}};
  if (record.isEmpty() || !simple) return result;
  QJsonObject counts;
  const QJsonArray sources = sourceStatus(context, record, counts);
  QJsonArray page;
  for (int index = offset; index < sources.size() && page.size() < limit;
       ++index)
    page.append(sources[index]);
  const int end = offset + page.size();
  for (const char* key :
       {"storage", "source_directory", "selection", "sequence_prefix",
        "sequence_extension", "dpi_policy", "dpi_x", "dpi_y"})
    result.insert(key, record.value(key));
  result.insert("level_path", levelPathKind(context, document, simple, record));
  result.insert(
      "source_check",
      QJsonObject{{"method", "size_and_modified_time"}, {"counts", counts}});
  result.insert("sources", page);
  result.insert("copies", record.value("copies").toArray().size());
  result.insert("total_sources", sources.size());
  result.insert("offset", offset);
  result.insert("next_offset",
                end < sources.size() ? QJsonValue(end) : QJsonValue());
  return result;
}

// --------------------------------------------------------------- schemas --

QJsonObject stringArray(int maximum) {
  return {
      {"type", "array"},
      {"minItems", 1},
      {"maxItems", maximum},
      {"items",
       QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 255}}}};
}

QJsonObject sourceProperties() {
  return {
      {"document_id", textSchema()},
      {"source_directory", textSchema()},
      {"file_names", stringArray(MaxIntakeFiles)},
      {"sequence_prefix",
       QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
      {"sequence_extension",
       QJsonObject{{"type", "string"}, {"pattern", "^[A-Za-z0-9]{1,8}$"}}},
      {"name", textSchema()}};
}

QJsonObject choiceSchema(const QStringList& values) {
  return {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}};
}

QJsonObject importProperties() {
  QJsonObject properties = sourceProperties();
  properties.insert("storage", choiceSchema({"copy", "link"}));
  properties.insert("on_existing", choiceSchema({"refuse", "reuse_identical"}));
  properties.insert("dpi_policy", choiceSchema({"source", "value"}));
  properties.insert("dpi", numberSchema(1, 2400));
  properties.insert("on_gap", choiceSchema({"refuse", "hold", "blank"}));
  properties.insert("exposure", choiceSchema({"none", "new_column"}));
  properties.insert("start_frame", integerSchema(0, LastRow));
  properties.insert("plan_digest", QJsonObject{{"type", "string"},
                                               {"pattern", "^[0-9a-f]{64}$"}});
  return properties;
}

}  // namespace

QJsonArray intakeOperations() {
  QJsonObject preflightProperties = sourceProperties();
  preflightProperties.insert("expected_revision", textSchema());
  preflightProperties.insert("offset", integerSchema(0, MaxIntakeFiles));
  preflightProperties.insert("limit", integerSchema(1, MaxIntakeFiles));
  return {
      operation("intake.preflight",
                "Check PNG files or a dotted sequence in one granted directory "
                "before import: formats, sizes, frames and gaps, alpha, DPI, "
                "frame rate and name collisions; reads headers only.",
                preflightProperties, {"document_id", "source_directory"}),
      operation("intake.import",
                "Import up to 64 PNG files or sequence frames as one RGBA8 "
                "level with explicit copy or link, DPI, gap, collision and "
                "exposure choices; returns the copied-file manifest.",
                importProperties(),
                {"document_id", "source_directory", "name", "storage",
                 "on_existing", "dpi_policy", "exposure"},
                true),
      operation("intake.provenance",
                "Read how a level was imported (copy or link, source digests) "
                "and whether each source file still matches.",
                merge(documentSchema(),
                      {{"level_id", textSchema()},
                       {"offset", integerSchema(0, MaxIntakeFiles)},
                       {"limit", integerSchema(1, MaxIntakeFiles)}}),
                {"document_id", "level_id"})};
}

QJsonValue executeIntake(Context& context, const QString& method,
                         const QJsonObject& params) {
  if (method == "intake.preflight") return preflight(context, params);
  if (method == "intake.import") return importIntake(context, params);
  if (method == "intake.provenance") return provenance(context, params);
  throw Error("method_not_found", "The intake operation is not available.");
}

// ------------------------------------------------------------ SVG export --

namespace {

// The paths the native writer emits for a region: one when it has edges and a
// style, then its subregions (tiio_svg.cpp writeRegion).
int writtenRegions(TRegion* region) {
  if (region->getEdgeCount() == 0 || !region->getStyle()) return 0;
  int count = 1;
  for (int index = 0; index < int(region->getSubregionCount()); ++index)
    count += writtenRegions(region->getSubregion(index));
  return count;
}

bool solidStyle(TPalette* palette, int id) {
  const TColorStyle* style = palette ? palette->getStyle(id) : nullptr;
  return style && typeid(*style) == typeid(TSolidColorStyle);
}

struct SvgContent final {
  int strokes       = 0;
  int zeroThickness = 0;
  int regions       = 0;
};

// Refuse anything outside the verified subset before writing.
SvgContent verifiedContent(const TVectorImageP& image) {
  TPalette* palette = image->getPalette();
  SvgContent content;
  for (UINT index = 0; index < image->getStrokeCount(); ++index) {
    TStroke* stroke = image->getStroke(index);
    if (!stroke->getOpacityProfile().empty())
      throw Error("unsupported_content",
                  "Strokes with an opacity envelope are outside the verified "
                  "SVG subset; the native writer rasterizes them.",
                  {{"reason", "opacity_envelope"}, {"stroke", int(index)}});
    if (!solidStyle(palette, stroke->getStyle()))
      throw Error("unsupported_content",
                  "Only solid styles are in the verified SVG subset.",
                  {{"reason", "style"}, {"stroke", int(index)}});
    if (stroke->getChunkCount() == 0 || stroke->getMaxThickness() == 0)
      ++content.zeroThickness;
    else
      ++content.strokes;
  }
  for (UINT index = 0; index < image->getRegionCount(); ++index) {
    TRegion* region = image->getRegion(index);
    if (region->getStyle() && !solidStyle(palette, region->getStyle()))
      throw Error("unsupported_content",
                  "Only solid fills are in the verified SVG subset.",
                  {{"reason", "fill_style"}});
    content.regions += writtenRegions(region);
  }
  if (content.strokes == 0)
    throw Error("unsupported_content",
                "The drawing has no stroke the SVG writer would write.",
                {{"reason", "empty_drawing"}});
  return content;
}

std::unique_ptr<TPropertyGroup> svgProperties(const QString& mode) {
  std::unique_ptr<TPropertyGroup> properties(Tiio::makeWriterProperties("svg"));
  auto* stroke =
      dynamic_cast<TEnumProperty*>(properties->getProperty("Stroke Mode"));
  auto* quality =
      dynamic_cast<TEnumProperty*>(properties->getProperty("Outline Quality"));
  if (!stroke || !quality)
    throw Error("writer_unavailable", "The native SVG writer is unavailable.");
  stroke->setValue(mode == "outline" ? L"Outline" : L"Centerline");
  quality->setValue(L"High");
  return properties;
}

// The elements the written file holds, read back with an XML reader.
std::pair<int, int> writtenElements(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    throw Error("io_error", "Cannot read back the staged SVG file.");
  QXmlStreamReader xml(&file);
  int paths = 0, images = 0;
  while (!xml.atEnd()) {
    if (xml.readNext() != QXmlStreamReader::StartElement) continue;
    if (xml.name() == QLatin1String("path")) ++paths;
    if (xml.name() == QLatin1String("image")) ++images;
  }
  if (xml.hasError())
    throw Error("native_error", "The native SVG writer wrote invalid XML.",
                {{"reason", xml.errorString().left(256)}});
  return {paths, images};
}

QJsonObject verifiedSubset(const QString& mode) {
  QJsonArray losses{"coordinates_rounded_to_0.1_units",
                    "zero_thickness_strokes_omitted"};
  if (mode == "centerline") losses.append("stroke_thickness_averaged");
  if (mode == "outline") losses.append("strokes_written_as_filled_outlines");
  return {{"styles", "solid"},
          {"opacity_envelopes", "refused"},
          {"regions", "filled_with_style_color"},
          {"raster_content", "none"},
          {"losses", losses}};
}

QJsonValue exportSvg(Context& context, const QJsonObject& params) {
  Document& document     = getDocument(context, params);
  TXshLevel* native      = document.level(requiredString(params, "level_id"));
  TXshSimpleLevel* level = native->getSimpleLevel();
  if (!level || level->getType() != PLI_XSHLEVEL)
    throw Error("unsupported_type", "SVG export needs a vector level.");
  const TFrameId frame = readDrawingId(params);
  if (!level->isFid(frame))
    throw Error("not_found", "Drawing identity does not exist.");
  const TImageP picture     = level->getFrame(frame, false);
  const TVectorImageP image = picture;
  if (!image) throw Error("asset_unavailable", "The drawing cannot be loaded.");
  const QString mode =
      choiceParam(params, "stroke_mode", {"outline", "centerline"}, {});
  const QString path =
      context.resolvePath(requiredString(params, "path"), true);
  if (QFileInfo(path).suffix().toLower() != "svg")
    throw Error("invalid_params", "The SVG path must end in .svg.");
  if (QFileInfo::exists(path) || QFileInfo(path).isSymLink())
    throw Error("already_exists", "The SVG destination exists.");
  const QFileInfo parent(QFileInfo(path).absolutePath());
  if (!parent.isDir() || !parent.isWritable())
    throw Error("io_error",
                "Cannot stage SVG output: the destination folder is missing "
                "or not writable.");
  const SvgContent content = verifiedContent(image);
  PrivateDirectory staging(context.scratchDirectory() + "/svg-XXXXXX");
  if (!staging.isValid())
    throw Error("io_error", "Cannot create a private staging directory.");
  const QString staged = staging.filePath("drawing.svg");
  {
    const auto properties = svgProperties(mode);
    TLevelWriterP writer(TFilePath(staged.toStdWString()), properties.get());
    writer->getFrameWriter(TFrameId(TFrameId::NO_FRAME))->save(picture);
  }
  const auto elements = writtenElements(staged);
  if (elements.first != content.strokes + content.regions || elements.second)
    throw Error("native_error", "The SVG file differs from the drawing.",
                {{"paths_written", elements.first},
                 {"paths_expected", content.strokes + content.regions}});
  QFile written(staged);
  if (!written.open(QIODevice::ReadOnly))
    throw Error("io_error", "Cannot read back the staged SVG file.");
  const QByteArray bytes = written.readAll();
  written.close();
  publishStaged(context, staged, path, "io_error",
                "Cannot publish the SVG file; the destination may exist.");
  return QJsonObject{
      {"path", path},
      {"format", "svg"},
      {"bytes", double(bytes.size())},
      {"sha256", QString::fromLatin1(
                     QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                         .toHex())},
      {"stroke_mode", mode},
      {"strokes_written", content.strokes},
      {"regions_written", content.regions},
      {"strokes_omitted", content.zeroThickness},
      {"visual_only", false},
      {"verified_subset", verifiedSubset(mode)}};
}

}  // namespace

QJsonArray svgExportOperations() {
  QJsonObject properties = merge(
      documentSchema(), {{"level_id", textSchema()}, {"path", textSchema()}});
  properties.insert(
      "drawing_id",
      QJsonObject{
          {"type", "object"},
          {"additionalProperties", false},
          {"properties",
           QJsonObject{{"number", integerSchema(1, 65535)},
                       {"suffix", QJsonObject{{"type", "string"},
                                              {"pattern", "^[a-z]{0,8}$"}}}}},
          {"required", QJsonArray{"number", "suffix"}}});
  properties.insert("stroke_mode", choiceSchema({"outline", "centerline"}));
  return {operation(
      "vector.export_svg",
      "Write one vector drawing to a new SVG file through the native writer, "
      "refusing content outside the verified subset (solid styles, no "
      "opacity envelopes); the result states the subset and its losses.",
      properties,
      {"document_id", "level_id", "drawing_id", "path", "stroke_mode"}, true)};
}

QJsonValue executeSvgExport(Context& context, const QString& method,
                            const QJsonObject& params) {
  if (method == "vector.export_svg") return exportSvg(context, params);
  throw Error("method_not_found", "The SVG operation is not available.");
}

QJsonObject intakeManifest(Document& document) {
  QJsonObject records;
  TLevelSet* cast = document.scene->getLevelSet();
  for (auto it = document.intake.constBegin(); it != document.intake.constEnd();
       ++it) {
    TXshLevel* level = document.levels.value(it.key());
    if (!level || !cast->hasLevel(level->getName())) continue;
    records.insert(QString::fromStdWString(level->getName()), it.value());
  }
  return records;
}

void adoptIntakeManifest(Document& document, const QJsonObject& manifest) {
  TLevelSet* cast = document.scene->getLevelSet();
  for (auto it = manifest.constBegin(); it != manifest.constEnd(); ++it) {
    TXshLevel* level = cast->getLevel(it.key().toStdWString());
    if (level)
      document.intake.insert(document.idForLevel(level), it.value().toObject());
  }
}

}  // namespace GroundzeroAutomation
