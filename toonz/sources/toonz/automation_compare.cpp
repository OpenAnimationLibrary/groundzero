#include "automation_compare.h"

#include "automation_files.h"
#include "automation_jobs.h"
#include "automation_ownership.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>

namespace GroundzeroAutomation {

namespace {

constexpr int MaxPairs        = 120;
constexpr int MaxImages       = 16;
constexpr int MaxSheetRows    = 16;
constexpr int SheetBound      = 2048;
constexpr int SheetGutter     = 8;
constexpr int MaxDimension    = 2048;
constexpr int RowsPerCheck    = 64;
constexpr int DefaultPageRows = 8;
constexpr qint64 InlineBytes  = 4 * 1024 * 1024;
constexpr qint64 FrameBytes   = 20 * 1024 * 1024;
constexpr qint64 JobBytes     = 64 * 1024 * 1024;
constexpr qint64 WorkerBytes  = 128 * 1024 * 1024;
constexpr qint64 EntryBytes   = 64 * 1024;

const char* const ChannelNames[] = {"r", "g", "b", "a"};

// ---------------------------------------------------------------------------
// Metrics (contract w3-compare §5).

struct UnionFind final {
  std::vector<int> parent;
  int find(int index) {
    while (parent[size_t(index)] != index) {
      parent[size_t(index)] = parent[size_t(parent[size_t(index)])];
      index                 = parent[size_t(index)];
    }
    return index;
  }
  void join(int left, int right) {
    left  = find(left);
    right = find(right);
    if (left != right)
      parent[size_t(std::max(left, right))] = std::min(left, right);
  }
};

struct Component final {
  qint64 area = 0;
  int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
  int max    = 0;
  qint64 sum = 0;
};

QJsonObject box(int x0, int y0, int x1, int y1) {
  return {
      {"x", x0}, {"y", y0}, {"width", x1 - x0 + 1}, {"height", y1 - y0 + 1}};
}

// Cell index of each offset along one axis: cell i spans
// [floor(i * size / cells), floor((i + 1) * size / cells)).
std::vector<int> cellIndex(int size, int cells) {
  std::vector<int> index(size_t(size), 0);
  for (int cell = 0; cell < cells; ++cell) {
    const int from = int(qint64(cell) * size / cells);
    const int to   = int(qint64(cell + 1) * size / cells);
    for (int offset = from; offset < to; ++offset) index[size_t(offset)] = cell;
  }
  return index;
}

QJsonObject componentRows(const std::vector<uchar>& difference,
                          const QRect& region, int threshold, int limit) {
  const int width = region.width(), height = region.height();
  std::vector<int> label(difference.size(), -1);
  UnionFind sets;
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const size_t at = size_t(y) * width + x;
      if (difference[at] <= threshold) continue;
      const int left  = x > 0 ? label[at - 1] : -1;
      const int above = y > 0 ? label[at - size_t(width)] : -1;
      if (left < 0 && above < 0) {
        label[at] = int(sets.parent.size());
        sets.parent.push_back(label[at]);
      } else {
        label[at] = left >= 0 ? left : above;
        if (left >= 0 && above >= 0) sets.join(left, above);
      }
    }
  std::map<int, Component> found;
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const size_t at = size_t(y) * width + x;
      if (label[at] < 0) continue;
      Component& component = found[sets.find(label[at])];
      ++component.area;
      component.x0  = std::min(component.x0, x + region.x());
      component.y0  = std::min(component.y0, y + region.y());
      component.x1  = std::max(component.x1, x + region.x());
      component.y1  = std::max(component.y1, y + region.y());
      component.max = std::max(component.max, int(difference[at]));
      component.sum += difference[at];
    }
  std::vector<Component> ordered;
  for (const auto& entry : found) ordered.push_back(entry.second);
  std::sort(ordered.begin(), ordered.end(),
            [](const Component& left, const Component& right) {
              if (left.area != right.area) return left.area > right.area;
              if (left.y0 != right.y0) return left.y0 < right.y0;
              return left.x0 < right.x0;
            });
  QJsonArray items;
  for (const Component& component : ordered) {
    if (items.size() >= limit) break;
    items.append(QJsonObject{
        {"area", double(component.area)},
        {"box", box(component.x0, component.y0, component.x1, component.y1)},
        {"max", component.max},
        {"sum", double(component.sum)}});
  }
  return {{"total", int(ordered.size())}, {"items", items}};
}

struct Accumulator final {
  std::array<int, 4> maxAbs{};
  std::array<qint64, 4> sumAbs{};
  std::array<quint64, 4> sumSq{};
  std::array<qint64, 256> histogram{};
  qint64 changed = 0;
  int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
};

// Stored code values as the mode sees them: straight, or RGB premultiplied
// by alpha with round half up in integers.
inline int modeValue(const uchar* pixel, int channel, bool premultiplied) {
  if (!premultiplied || channel == 3) return pixel[channel];
  return (pixel[channel] * pixel[3] + 127) / 255;
}

QJsonObject channelMetrics(const Accumulator& sums, int channel,
                           qint64 pixels) {
  return {{"max_abs", sums.maxAbs[size_t(channel)]},
          {"sum_abs", double(sums.sumAbs[size_t(channel)])},
          {"sum_sq", double(sums.sumSq[size_t(channel)])},
          {"mean_abs", double(sums.sumAbs[size_t(channel)]) / double(pixels)},
          {"rmse",
           std::sqrt(double(sums.sumSq[size_t(channel)]) / double(pixels))}};
}

QJsonObject percentiles(const Accumulator& sums, qint64 pixels) {
  QJsonObject result;
  for (int percent : {50, 95, 99, 100}) {
    const qint64 rank = (qint64(percent) * pixels + 99) / 100;
    qint64 seen       = 0;
    int value         = 255;
    for (int bin = 0; bin < 256; ++bin) {
      seen += sums.histogram[size_t(bin)];
      if (seen >= rank) {
        value = bin;
        break;
      }
    }
    result.insert("p" + QString::number(percent), value);
  }
  return result;
}

QJsonObject metricsJson(const CompareInput& input, const Accumulator& sums,
                        qint64 pixels, bool identical) {
  const int considered = input.alpha ? 4 : 3;
  QJsonObject channels;
  quint64 allSquares = 0;
  for (int channel = 0; channel < 4; ++channel) {
    if (channel >= considered) {
      channels.insert(ChannelNames[channel], QJsonValue());
      continue;
    }
    channels.insert(ChannelNames[channel],
                    channelMetrics(sums, channel, pixels));
    allSquares += sums.sumSq[size_t(channel)];
  }
  QJsonArray histogram;
  for (qint64 count : sums.histogram) histogram.append(double(count));
  return {
      {"identical_straight", identical},
      {"pixels", double(pixels)},
      {"channels", channels},
      {"rmse_all", std::sqrt(double(allSquares) / double(pixels * considered))},
      {"histogram", histogram},
      {"percentiles", percentiles(sums, pixels)},
      {"changed_pixels", double(sums.changed)},
      {"changed_ppm", double(sums.changed * 1000000 / pixels)},
      {"changed_bbox",
       sums.x1 < 0 ? QJsonValue()
                   : QJsonValue(box(sums.x0, sums.y0, sums.x1, sums.y1))}};
}

}  // namespace

QString pixelSha256(const uchar* rgba, int width, int height) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData("GZRGBA8\n", 8);
  uchar size[8];
  qToBigEndian<quint32>(quint32(width), size);
  qToBigEndian<quint32>(quint32(height), size + 4);
  hash.addData(reinterpret_cast<const char*>(size), 8);
  hash.addData(reinterpret_cast<const char*>(rgba), width * height * 4);
  return QString::fromLatin1(hash.result().toHex());
}

CompareOutput compareImages(const CompareInput& input) {
  CompareOutput out;
  const QRect region           = input.region;
  const int considered         = input.alpha ? 4 : 3;
  const int columns            = std::min(input.gridColumns, region.width());
  const int rows               = std::min(input.gridRows, region.height());
  const std::vector<int> cellX = cellIndex(region.width(), columns);
  const std::vector<int> cellY = cellIndex(region.height(), rows);
  std::vector<std::array<qint64, 3>> cells(size_t(columns) * rows, {0, 0, 0});
  Accumulator sums;
  out.pixels = qint64(region.width()) * region.height();
  out.difference.assign(size_t(out.pixels), 0);
  out.identical = std::equal(
      input.reference, input.reference + size_t(input.width) * input.height * 4,
      input.candidate);
  for (int dy = 0; dy < region.height(); ++dy) {
    if (dy % RowsPerCheck == 0 && input.cancel && input.cancel->load()) {
      out.canceled = true;
      return out;
    }
    const int y = region.y() + dy;
    for (int dx = 0; dx < region.width(); ++dx) {
      const int x         = region.x() + dx;
      const size_t offset = (size_t(y) * input.width + x) * 4;
      const uchar* left   = input.reference + offset;
      const uchar* right  = input.candidate + offset;
      int largest         = 0;
      for (int channel = 0; channel < considered; ++channel) {
        const int d = std::abs(modeValue(left, channel, input.premultiplied) -
                               modeValue(right, channel, input.premultiplied));
        sums.maxAbs[size_t(channel)] =
            std::max(sums.maxAbs[size_t(channel)], d);
        sums.sumAbs[size_t(channel)] += d;
        sums.sumSq[size_t(channel)] += quint64(d) * quint64(d);
        largest = std::max(largest, d);
      }
      ++sums.histogram[size_t(largest)];
      out.difference[size_t(dy) * region.width() + dx] = uchar(largest);
      std::array<qint64, 3>& cell =
          cells[size_t(cellY[size_t(dy)]) * columns + cellX[size_t(dx)]];
      cell[1] = std::max<qint64>(cell[1], largest);
      cell[2] += largest;
      if (largest <= input.threshold) continue;
      ++sums.changed;
      ++cell[0];
      sums.x0 = std::min(sums.x0, x);
      sums.y0 = std::min(sums.y0, y);
      sums.x1 = std::max(sums.x1, x);
      sums.y1 = std::max(sums.y1, y);
    }
  }
  out.changed     = sums.changed;
  out.maxAbsAlpha = input.alpha ? sums.maxAbs[3] : 0;
  out.metrics     = metricsJson(input, sums, out.pixels, out.identical);
  QJsonArray grid;
  for (const auto& cell : cells)
    grid.append(QJsonArray{double(cell[0]), double(cell[1]), double(cell[2])});
  out.regions = {
      {"grid",
       QJsonObject{{"columns", columns}, {"rows", rows}, {"cells", grid}}},
      {"components", componentRows(out.difference, region, input.threshold,
                                   input.components)}};
  return out;
}

namespace {

// ---------------------------------------------------------------------------
// Requests.

struct Tolerance final {
  QString name;
  QString mode;
  int threshold   = 0;
  qint64 maxPpm   = 0;
  int maxAbsAlpha = -1;

  QJsonObject toJson() const {
    return {{"profile", name.isEmpty() ? QJsonValue() : QJsonValue(name)},
            {"mode", mode},
            {"channel_threshold", threshold},
            {"max_changed_ppm", double(maxPpm)},
            {"max_abs_alpha",
             maxAbsAlpha < 0 ? QJsonValue() : QJsonValue(maxAbsAlpha)}};
  }
};

const std::vector<Tolerance>& toleranceProfiles() {
  static const std::vector<Tolerance> profiles{
      {"exact", "straight", 0, 0, -1},
      {"rounding", "straight", 2, 0, -1},
      {"straight_alpha_roundtrip", "premultiplied", 1, 0, 0},
      {"native_fx_regression", "premultiplied", 3, 2000, -1}};
  return profiles;
}

struct Side final {
  QString origin;
  RenderFrameRef frame;
};

struct PairSpec final {
  int index = 0;
  QString label;
  Side reference;
  Side candidate;
};

struct Settings final {
  QString mode     = "premultiplied";
  QString channels = "rgba";
  std::optional<QRect> region;
  Tolerance tolerance;
  int gridColumns          = 8;
  int gridRows             = 8;
  int components           = 16;
  QString differenceImages = "changed";
  bool sheet               = false;
  int deadlineMs           = 300000;
};

[[noreturn]] void refuse(const QString& code, const QString& message,
                         QJsonObject details = {}) {
  throw Error(code, message, details);
}

Tolerance readTolerance(const QJsonObject& params,
                        const QString& requestedMode) {
  const QJsonValue value = params.value("tolerance");
  Tolerance tolerance;
  if (value.isString()) {
    for (const Tolerance& profile : toleranceProfiles())
      if (profile.name == value.toString()) tolerance = profile;
    if (!requestedMode.isEmpty() && requestedMode != tolerance.mode)
      refuse("profile_conflict",
             "The tolerance profile compares in another mode than the one "
             "requested.",
             {{"profile", tolerance.name},
              {"profile_mode", tolerance.mode},
              {"mode", requestedMode}});
    return tolerance;
  }
  tolerance.mode = requestedMode.isEmpty() ? "premultiplied" : requestedMode;
  if (value.isObject()) {
    const QJsonObject explicitValues = value.toObject();
    tolerance.threshold = explicitValues.value("channel_threshold").toInt();
    tolerance.maxPpm =
        qint64(explicitValues.value("max_changed_ppm").toDouble());
    tolerance.maxAbsAlpha = explicitValues.value("max_abs_alpha").toInt(-1);
    return tolerance;
  }
  const QString name =
      tolerance.mode == "straight" ? "exact" : "native_fx_regression";
  for (const Tolerance& profile : toleranceProfiles())
    if (profile.name == name) return profile;
  return tolerance;
}

Settings readSettings(const QJsonObject& params) {
  Settings settings;
  const QString mode = params.value("mode").toString();
  settings.tolerance = readTolerance(params, mode);
  settings.mode      = settings.tolerance.mode;
  settings.channels  = params.value("channels").toString("rgba");
  if (settings.channels == "rgb" && settings.tolerance.maxAbsAlpha >= 0)
    refuse("profile_conflict",
           "The tolerance bounds alpha but the comparison ignores it.",
           {{"channels", "rgb"}});
  if (params.contains("region")) {
    const QJsonObject region = params.value("region").toObject();
    settings.region =
        QRect(region.value("x").toInt(), region.value("y").toInt(),
              region.value("width").toInt(), region.value("height").toInt());
  }
  const QJsonObject grid = params.value("grid").toObject();
  settings.gridColumns   = grid.value("columns").toInt(8);
  settings.gridRows      = grid.value("rows").toInt(8);
  settings.components    = integerParam(params, "components", 0, 64, 16);
  settings.differenceImages =
      params.value("difference_images").toString("changed");
  settings.sheet = params.value("sheet").toBool(false);
  settings.deadlineMs =
      integerParam(params, "deadline_ms", 1000, 600000, 300000);
  return settings;
}

Side artifactSide(Context& context, const QString& artifactId) {
  const std::optional<RenderFrameRef> frame =
      renderFrameByArtifact(context, artifactId);
  if (!frame)
    refuse("unknown_artifact",
           "No completed render job of this worker owns "
           "this artifact.",
           {{"artifact_id", artifactId}});
  if (frame->profile != "rgba8-png")
    refuse("unsupported_artifact_kind",
           "Only 8-bit PNG frames can be compared.",
           {{"artifact_id", artifactId}, {"profile", frame->profile}});
  return {"artifact", *frame};
}

Side frameSide(Context& context, const QString& jobId, int frame) {
  for (const RenderFrameRef& ref : completedRenderFrames(context, jobId)) {
    if (ref.frame != frame) continue;
    if (ref.profile != "rgba8-png")
      refuse("unsupported_artifact_kind",
             "Only 8-bit PNG frames can be compared.",
             {{"job_id", jobId}, {"profile", ref.profile}});
    return {"job_frame", ref};
  }
  refuse("frame_mismatch", "The job did not render this frame.",
         {{"job_id", jobId}, {"frame", frame}});
}

Side readSide(Context& context, const QJsonObject& side) {
  const QString kind  = side.value("kind").toString();
  const bool artifact = kind == "artifact";
  const bool expected =
      artifact ? side.contains("artifact_id") && !side.contains("job_id") &&
                     !side.contains("frame")
               : side.contains("job_id") && side.contains("frame") &&
                     !side.contains("artifact_id");
  if (!expected)
    refuse("invalid_params",
           "An artifact side takes artifact_id; a job_frame side takes job_id "
           "and frame.");
  if (artifact)
    return artifactSide(context, side.value("artifact_id").toString());
  return frameSide(context, side.value("job_id").toString(),
                   side.value("frame").toInt());
}

QJsonArray framesLacking(const std::vector<RenderFrameRef>& have,
                         const std::vector<RenderFrameRef>& want) {
  QJsonArray missing;
  for (const RenderFrameRef& frame : want) {
    const bool found = std::any_of(
        have.begin(), have.end(),
        [&](const RenderFrameRef& ref) { return ref.frame == frame.frame; });
    if (!found && missing.size() < 64) missing.append(frame.frame);
  }
  return missing;
}

std::vector<RenderFrameRef> windowed(const std::vector<RenderFrameRef>& frames,
                                     const QJsonObject& window) {
  if (window.isEmpty()) return frames;
  const int start = window.value("start").toInt(),
            end   = window.value("end").toInt();
  if (end < start)
    refuse("invalid_params", "frames.end precedes frames.start.");
  std::vector<RenderFrameRef> result;
  for (const RenderFrameRef& frame : frames)
    if (frame.frame >= start && frame.frame <= end) result.push_back(frame);
  return result;
}

std::vector<PairSpec> rangePairs(Context& context, const QJsonObject& range) {
  const QJsonObject window = range.value("frames").toObject();
  const auto reference =
      windowed(completedRenderFrames(
                   context, range.value("reference_job_id").toString()),
               window);
  const auto candidate =
      windowed(completedRenderFrames(
                   context, range.value("candidate_job_id").toString()),
               window);
  const QJsonArray referenceLacks = framesLacking(reference, candidate);
  const QJsonArray candidateLacks = framesLacking(candidate, reference);
  if (!referenceLacks.isEmpty() || !candidateLacks.isEmpty() ||
      reference.empty())
    refuse("frame_mismatch", "The two jobs do not cover the same frames.",
           {{"reference_lacks", referenceLacks},
            {"candidate_lacks", candidateLacks}});
  if (int(reference.size()) > MaxPairs)
    refuse("invalid_params", "A range pairs at most 120 frames.");
  std::vector<PairSpec> pairs;
  for (const RenderFrameRef& left : reference)
    for (const RenderFrameRef& right : candidate) {
      if (left.frame != right.frame) continue;
      if (left.profile != "rgba8-png" || right.profile != "rgba8-png")
        refuse("unsupported_artifact_kind",
               "Only 8-bit PNG frames can be compared.");
      PairSpec pair;
      pair.index     = int(pairs.size());
      pair.label     = "frame " + QString::number(left.frame);
      pair.reference = {"job_frame", left};
      pair.candidate = {"job_frame", right};
      pairs.push_back(pair);
    }
  return pairs;
}

std::vector<PairSpec> readPairs(Context& context, const QJsonObject& params) {
  if (params.contains("pairs") == params.contains("range"))
    refuse("invalid_params", "Give exactly one of pairs and range.");
  if (params.contains("range"))
    return rangePairs(context, params.value("range").toObject());
  std::vector<PairSpec> pairs;
  for (const QJsonValue& value : params.value("pairs").toArray()) {
    const QJsonObject entry = value.toObject();
    PairSpec pair;
    pair.index     = int(pairs.size());
    pair.label     = entry.value("label").toString();
    pair.reference = readSide(context, entry.value("reference").toObject());
    pair.candidate = readSide(context, entry.value("candidate").toObject());
    pairs.push_back(pair);
  }
  return pairs;
}

void requireRegion(const Settings& settings,
                   const std::vector<PairSpec>& pairs) {
  if (!settings.region) return;
  const QRect region = *settings.region;
  for (const PairSpec& pair : pairs)
    for (const Side* side : {&pair.reference, &pair.candidate})
      if (!QRect(0, 0, side->frame.width, side->frame.height).contains(region))
        refuse("invalid_params", "The region lies outside a compared image.",
               {{"reason", "region_outside"},
                {"pair", pair.index},
                {"width", side->frame.width},
                {"height", side->frame.height}});
}

// ---------------------------------------------------------------------------
// Sheet layout: one row per pair (at most 16), columns reference, candidate
// and difference, every tile reduced by the same integer factor k.

struct SheetLayout final {
  int factor = 1;
  int rows   = 0;
  int cell   = 0;
  int width  = 0;
  int height = 0;
  std::vector<int> rowHeights;
};

int reduced(int size, int factor) { return (size + factor - 1) / factor; }

SheetLayout sheetLayout(const std::vector<PairSpec>& pairs,
                        const std::optional<QRect>& region) {
  SheetLayout layout;
  layout.rows = std::min(int(pairs.size()), MaxSheetRows);
  for (int factor = 1;; ++factor) {
    int cell = 0, height = SheetGutter;
    std::vector<int> heights;
    for (int row = 0; row < layout.rows; ++row) {
      const PairSpec& pair      = pairs[size_t(row)];
      const int differenceWidth = region ? region->width()
                                         : std::max(pair.reference.frame.width,
                                                    pair.candidate.frame.width);
      const int differenceHeight = region
                                       ? region->height()
                                       : std::max(pair.reference.frame.height,
                                                  pair.candidate.frame.height);
      cell = std::max({cell, reduced(pair.reference.frame.width, factor),
                       reduced(pair.candidate.frame.width, factor),
                       reduced(differenceWidth, factor)});
      const int rowHeight =
          std::max({reduced(pair.reference.frame.height, factor),
                    reduced(pair.candidate.frame.height, factor),
                    reduced(differenceHeight, factor)});
      heights.push_back(rowHeight);
      height += rowHeight + SheetGutter;
    }
    const int width = 3 * cell + 4 * SheetGutter;
    if (width <= SheetBound && height <= SheetBound) {
      layout.factor     = factor;
      layout.cell       = cell;
      layout.width      = width;
      layout.height     = height;
      layout.rowHeights = heights;
      return layout;
    }
  }
}

// Nearest-neighbour reduction: tile pixel (x, y) is source pixel
// (k x + floor(k / 2), k y + floor(k / 2)), clamped.
QImage reduceTile(const QImage& source, int factor) {
  QImage tile(reduced(source.width(), factor), reduced(source.height(), factor),
              QImage::Format_RGBA8888);
  for (int y = 0; y < tile.height(); ++y) {
    const int sy      = std::min(source.height() - 1, factor * y + factor / 2);
    const uchar* from = source.constScanLine(sy);
    uchar* to         = tile.scanLine(y);
    for (int x = 0; x < tile.width(); ++x) {
      const int sx = std::min(source.width() - 1, factor * x + factor / 2);
      std::copy(from + sx * 4, from + sx * 4 + 4, to + x * 4);
    }
  }
  return tile;
}

// ---------------------------------------------------------------------------
// Pair work, run on one worker thread: nothing here touches native scene
// state or the job registry.

struct Decoded final {
  QImage image;
  QString fileSha256;
  QString pixelSha256;
  QString failure;
  QString detail;
};

Decoded decodeSide(const RenderFrameRef& frame) {
  Decoded decoded;
  QFile file;
  if (openApprovedFile(file, frame.path) != OpenResult::Opened) {
    decoded.failure = "source_missing";
    return decoded;
  }
  const QByteArray data = file.read(FrameBytes + 1);
  decoded.fileSha256    = QString::fromLatin1(
      QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
  if (decoded.fileSha256 != frame.sha256) {
    decoded.failure = "source_changed";
    return decoded;
  }
  if (data.size() < 33 || data[24] != char(8)) {
    decoded.failure = "unsupported_image";
    decoded.detail  = "not_8_bit";
    return decoded;
  }
  QBuffer buffer;
  buffer.setData(data);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer, "png");
  reader.setAutoTransform(false);
  const QImage image = reader.read();
  if (image.isNull()) {
    decoded.failure = "corrupt";
    decoded.detail  = reader.errorString().left(256);
    return decoded;
  }
  decoded.image       = image.convertToFormat(QImage::Format_RGBA8888);
  decoded.pixelSha256 = pixelSha256(
      decoded.image.constBits(), decoded.image.width(), decoded.image.height());
  return decoded;
}

QByteArray encodePng(const QImage& image) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "png");
  return bytes;
}

struct PairWork final {
  PairSpec pair;
  Settings settings;
  int sheetFactor                 = 0;
  bool wantImage                  = false;
  const std::atomic<bool>* cancel = nullptr;
};

struct PairOutcome final {
  QString outcome = "compared";
  QString detail;
  Decoded reference;
  Decoded candidate;
  CompareOutput result;
  QRect region;
  QImage difference;
  QByteArray differencePng;
  QString differencePixelSha;
  std::array<QImage, 3> tiles;
};

QImage differenceImage(const CompareOutput& result, const QRect& region,
                       int threshold) {
  QImage image(region.width(), region.height(), QImage::Format_RGBA8888);
  for (int y = 0; y < region.height(); ++y) {
    uchar* row = image.scanLine(y);
    for (int x = 0; x < region.width(); ++x) {
      const int d      = result.difference[size_t(y) * region.width() + x];
      const uchar gray = d > threshold ? uchar(d) : 0;
      row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = gray;
      row[x * 4 + 3]                               = 255;
    }
  }
  return image;
}

PairOutcome runPair(const PairWork& work) {
  PairOutcome outcome;
  outcome.reference = decodeSide(work.pair.reference.frame);
  outcome.candidate = decodeSide(work.pair.candidate.frame);
  for (const Decoded* side : {&outcome.reference, &outcome.candidate})
    if (!side->failure.isEmpty()) {
      outcome.outcome = side->failure;
      outcome.detail  = side->detail;
      return outcome;
    }
  const QImage& left  = outcome.reference.image;
  const QImage& right = outcome.candidate.image;
  if (left.size() != right.size()) {
    outcome.outcome = "size_mismatch";
    return outcome;
  }
  outcome.region =
      work.settings.region.value_or(QRect(QPoint(0, 0), left.size()));
  CompareInput input;
  input.reference     = left.constBits();
  input.candidate     = right.constBits();
  input.width         = left.width();
  input.height        = left.height();
  input.region        = outcome.region;
  input.premultiplied = work.settings.mode == "premultiplied";
  input.alpha         = work.settings.channels == "rgba";
  input.threshold     = work.settings.tolerance.threshold;
  input.gridColumns   = work.settings.gridColumns;
  input.gridRows      = work.settings.gridRows;
  input.components    = work.settings.components;
  input.cancel        = work.cancel;
  outcome.result      = compareImages(input);
  if (outcome.result.canceled) return outcome;
  const bool needImage = work.wantImage || work.sheetFactor > 0;
  if (needImage)
    outcome.difference =
        differenceImage(outcome.result, outcome.region, input.threshold);
  if (work.wantImage) {
    outcome.differencePng = encodePng(outcome.difference);
    outcome.differencePixelSha =
        pixelSha256(outcome.difference.constBits(), outcome.difference.width(),
                    outcome.difference.height());
  }
  if (work.sheetFactor > 0) {
    outcome.tiles[0] = reduceTile(left, work.sheetFactor);
    outcome.tiles[1] = reduceTile(right, work.sheetFactor);
    outcome.tiles[2] = reduceTile(outcome.difference, work.sheetFactor);
  }
  outcome.result.difference.clear();
  outcome.difference = QImage();
  return outcome;
}

QThreadPool& comparePool() {
  static QThreadPool* pool = []() {
    auto* created = new QThreadPool();
    created->setMaxThreadCount(1);
    return created;
  }();
  return *pool;
}

// ---------------------------------------------------------------------------
// Rows.

QJsonObject sideReport(const Side& side, const Decoded& decoded) {
  const RenderFrameRef& frame = side.frame;
  const QJsonObject effective = frame.effective;
  const QJsonObject color     = effective.value("color").toObject();
  const QJsonObject pipeline  = color.value("pipeline").toObject();
  const QJsonObject camera    = effective.value("camera").toObject();
  return {{"origin", side.origin},
          {"artifact_id", frame.artifactId},
          {"job_id", frame.jobId},
          {"document_id", frame.documentId},
          {"source_revision", frame.revision},
          {"frame", frame.frame},
          {"file_sha256", frame.sha256},
          {"pixel_sha256", decoded.pixelSha256.isEmpty()
                               ? QJsonValue()
                               : QJsonValue(decoded.pixelSha256)},
          {"width", frame.width},
          {"height", frame.height},
          {"bit_depth", 8},
          {"alpha_kind", "straight"},
          {"color_tags", QJsonArray()},
          {"profile", frame.profile},
          {"dpi", camera.value("dpi")},
          {"camera", camera.value("resolution")},
          {"fit_scale", effective.value("fit_scale")},
          {"crop", effective.value("crop")},
          {"aspect_export", effective.value("aspect_export")},
          {"linear", pipeline.value("linear")},
          {"gamma", pipeline.value("gamma")}};
}

QJsonArray conditionsDiffer(const QJsonObject& left, const QJsonObject& right) {
  static const char* fields[] = {
      "width",         "height",      "dpi",
      "camera",        "fit_scale",   "crop",
      "aspect_export", "linear",      "gamma",
      "color_tags",    "alpha_kind",  "bit_depth",
      "profile",       "document_id", "source_revision",
      "frame"};
  QJsonArray differ;
  for (const char* field : fields)
    if (left.value(field) != right.value(field))
      differ.append(QJsonObject{{"field", field},
                                {"reference", left.value(field)},
                                {"candidate", right.value(field)},
                                {"known", true}});
  return differ;
}

QJsonObject verdict(const CompareOutput& result, const Tolerance& tolerance) {
  QJsonArray failed;
  if (result.changed * 1000000 > tolerance.maxPpm * result.pixels)
    failed.append("changed_ppm");
  if (tolerance.maxAbsAlpha >= 0 && result.maxAbsAlpha > tolerance.maxAbsAlpha)
    failed.append("max_abs_alpha");
  return {{"passed", failed.isEmpty()},
          {"failed_checks", failed},
          {"tolerance", tolerance.toJson()}};
}

// ---------------------------------------------------------------------------
// The job.

struct ReviewArtifact final {
  QString id;
  QString role;
  QString path;
  QString sha256;
  int pair     = -1;
  int width    = 0;
  int height   = 0;
  qint64 bytes = 0;
};

enum class State { Running, Completed, Failed, Canceled, DeadlineExceeded };

class ReviewJob final : public Job,
                        public std::enable_shared_from_this<ReviewJob> {
public:
  Context* context = nullptr;
  JobCommon common;
  Settings settings;
  std::vector<PairSpec> pairs;
  SheetLayout layout;
  QString directory;
  QString workspace;
  OwnershipClaim ownership{true};
  State state = State::Running;
  QStringList reasons;
  std::vector<QJsonObject> rows;
  std::vector<ReviewArtifact> artifacts;
  std::vector<std::array<QImage, 3>> tiles;
  std::shared_ptr<std::atomic<bool>> cancelFlag =
      std::make_shared<std::atomic<bool>>(false);
  qint64 storageBytes  = 0;
  qint64 pixelsScanned = 0;
  qint64 bytesRead     = 0;
  int imagesWritten    = 0;
  int passed = 0, failedPairs = 0, notCompared = 0;
  qint64 worstPpm = -1;
  bool inFlight   = false;
  QJsonValue sheet;

  QString id() const override { return common.id; }
  JobLane lane() const override { return JobLane::Analysis; }
  QString documentId() const override { return common.documentId; }
  bool terminal() const override { return state != State::Running; }
  qint64 reservedBytes() const override { return JobBytes; }

  qint64 now() const { return jobRegistry(*context).elapsedMs(); }

  QString statusText() const {
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
    if (common.cancelRequested) return "cancellation_requested";
    return "running";
  }

  QJsonValue complete() const {
    if (state == State::Running || state == State::Failed) return QJsonValue();
    return state == State::Completed && reasons.isEmpty();
  }

  QJsonObject analysisRecord() const {
    const qint64 elapsed =
        common.startedMs < 0
            ? 0
            : (common.endedMs >= 0 ? common.endedMs : now()) - common.startedMs;
    return {{"check", "image_review"},
            {"cost", QJsonObject{{"elapsed_ms", double(elapsed)},
                                 {"pixels_scanned", double(pixelsScanned)},
                                 {"files_checked", 2 * int(rows.size())},
                                 {"bytes_read", double(bytesRead)},
                                 {"helpers_started", 0}}},
            {"budget",
             QJsonObject{{"max_pixels", double(qint64(MaxPairs) * MaxDimension *
                                               MaxDimension)},
                         {"max_files", 2 * MaxPairs},
                         {"max_bytes", double(JobBytes)}}},
            {"complete", complete()},
            {"reasons", QJsonArray::fromStringList(reasons)}};
  }

  QJsonObject record() const override {
    const QJsonObject limits{{"deadline_ms", settings.deadlineMs},
                             {"storage_reserved_bytes", double(JobBytes)},
                             {"cancel_grace_ms", CancelGraceMs}};
    const QJsonObject progress{{"unit", "frames"},
                               {"done", int(rows.size())},
                               {"total", int(pairs.size())}};
    const QJsonObject outputs{
        {"directory", QDir(workspace).relativeFilePath(directory)},
        {"artifact_count", int(artifacts.size())},
        {"result_rows", int(rows.size()) + (terminal() ? 1 : 0)}};
    QJsonObject result = commonRecord(*context, common, statusText(),
                                      terminal(), limits, progress, outputs);
    result.insert("analysis", analysisRecord());
    return result;
  }

  void cancel() override {
    if (!common.cancelRequested) {
      common.cancelRequested = true;
      common.cancelMs        = now();
    }
    cancelFlag->store(true);
    if (!terminal() && !inFlight) finish(State::Canceled);
  }

  void interrupt(qint64) override { cancelFlag->store(true); }

  QJsonValue result(Context&, const QJsonObject& params) override;

  void start() {
    common.startedMs = now();
    if (settings.sheet) tiles.resize(size_t(layout.rows));
    const auto self = shared_from_this();
    QTimer::singleShot(settings.deadlineMs, QCoreApplication::instance(),
                       [self]() {
                         if (self->terminal()) return;
                         self->cancelFlag->store(true);
                         if (!self->inFlight)
                           self->finish(State::DeadlineExceeded);
                         else
                           self->deadlinePending = true;
                       });
    next();
  }

  bool deadlinePending = false;

  void next() {
    if (terminal()) return;
    if (cancelFlag->load())
      return finish(deadlinePending ? State::DeadlineExceeded
                                    : State::Canceled);
    if (rows.size() == pairs.size()) return finishSheet();
    PairWork work;
    work.pair     = pairs[rows.size()];
    work.settings = settings;
    work.sheetFactor =
        settings.sheet && int(rows.size()) < layout.rows ? layout.factor : 0;
    work.wantImage  = wantsImage();
    work.cancel     = cancelFlag.get();
    inFlight        = true;
    const auto self = shared_from_this();
    const auto flag = cancelFlag;
    comparePool().start(QRunnable::create([self, work, flag]() {
      auto outcome = std::make_shared<PairOutcome>(runPair(work));
      QTimer::singleShot(0, QCoreApplication::instance(),
                         [self, outcome]() { self->paired(*outcome); });
    }));
  }

  bool wantsImage() const { return settings.differenceImages != "none"; }

  void paired(PairOutcome& outcome) {
    inFlight = false;
    if (terminal()) return;
    if (outcome.result.canceled || cancelFlag->load())
      return finish(deadlinePending ? State::DeadlineExceeded
                                    : State::Canceled);
    rows.push_back(pairRow(outcome));
    jobRegistry(*context).publish();
    next();
  }

  QJsonObject pairRow(PairOutcome& outcome);
  QJsonObject writeDifference(PairOutcome& outcome, int pair);
  std::optional<ReviewArtifact> publishImage(const QByteArray& png,
                                             const QString& role, int pair,
                                             int width, int height);
  void finishSheet();

  void finish(State end) {
    if (terminal()) return;
    if (end == State::Canceled) reasons.append("canceled");
    if (end == State::DeadlineExceeded) reasons.append("deadline");
    tiles.clear();
    state          = end;
    common.endedMs = now();
    ownership.release();
    jobRegistry(*context).publish();
  }

  void failJob(const QString& code, const QString& message) {
    common.failure = QJsonObject{
        {"code", code}, {"phase", "analyze"}, {"message", message.left(2048)}};
    rows.clear();
    finish(State::Failed);
    try {
      removeApprovedTree(directory);
    } catch (const Error&) {
    }
  }

  QJsonObject summary() const {
    return {{"kind", "summary"},
            {"compared", passed + failedPairs},
            {"passed", passed},
            {"failed", failedPairs},
            {"not_compared", notCompared},
            {"worst_changed_ppm",
             worstPpm < 0 ? QJsonValue() : QJsonValue(double(worstPpm))},
            {"images_written", imagesWritten},
            {"storage_bytes", double(storageBytes)},
            {"sheet", sheet},
            {"complete", complete()},
            {"evidence", QJsonObject{{"kind", "visual"},
                                     {"basis", "decoded_pixels"},
                                     {"structural_claims", "none"}}}};
  }
};

std::optional<ReviewArtifact> ReviewJob::publishImage(const QByteArray& png,
                                                      const QString& role,
                                                      int pair, int width,
                                                      int height) {
  if (storageBytes + png.size() + EntryBytes > JobBytes) return std::nullopt;
  ReviewArtifact artifact;
  artifact.id   = jobRegistry(*context).newArtifactId();
  artifact.role = role;
  artifact.pair = pair;
  artifact.path =
      directory + "/" +
      (role == "sheet"
           ? QString("sheet.png")
           : QString("difference.%1.png").arg(pair, 4, 10, QChar('0')));
  const QString staged =
      directory + "/.staged-" + QFileInfo(artifact.path).fileName();
  QFile file(staged);
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      file.write(png) != png.size() || !file.flush())
    throw Error("io_error", "A review image could not be written.");
  file.close();
  if (publishApproved(staged, artifact.path) != PublishResult::Published) {
    QFile::remove(staged);
    throw Error("io_error", "A review image could not be published.");
  }
  artifact.sha256 = QString::fromLatin1(
      QCryptographicHash::hash(png, QCryptographicHash::Sha256).toHex());
  artifact.width  = width;
  artifact.height = height;
  artifact.bytes  = png.size();
  storageBytes += png.size() + EntryBytes;
  artifacts.push_back(artifact);
  return artifact;
}

QJsonObject ReviewJob::writeDifference(PairOutcome& outcome, int pair) {
  QJsonObject image{{"status", "not_requested"},
                    {"artifact_id", QJsonValue()},
                    {"width", outcome.region.width()},
                    {"height", outcome.region.height()}};
  if (settings.differenceImages == "none") return image;
  if (settings.differenceImages == "changed" && outcome.result.changed == 0) {
    image.insert("status", "no_change");
    return image;
  }
  if (imagesWritten >= MaxImages) {
    image.insert("status", "omitted_budget");
    return image;
  }
  const auto artifact =
      publishImage(outcome.differencePng, "difference", pair,
                   outcome.region.width(), outcome.region.height());
  if (!artifact) {
    image.insert("status", "omitted_storage");
    return image;
  }
  ++imagesWritten;
  image.insert("status", "written");
  image.insert("artifact_id", artifact->id);
  image.insert("size_bytes", double(artifact->bytes));
  image.insert("sha256", artifact->sha256);
  image.insert("pixel_sha256", outcome.differencePixelSha);
  return image;
}

QJsonObject ReviewJob::pairRow(PairOutcome& outcome) {
  const PairSpec& pair        = pairs[rows.size()];
  const QJsonObject reference = sideReport(pair.reference, outcome.reference);
  const QJsonObject candidate = sideReport(pair.candidate, outcome.candidate);
  bytesRead += pair.reference.frame.bytes + pair.candidate.frame.bytes;
  QJsonObject row{
      {"kind", "pair"},
      {"index", pair.index},
      {"label", pair.label},
      {"outcome", outcome.outcome},
      {"outcome_detail",
       outcome.detail.isEmpty() ? QJsonValue() : QJsonValue(outcome.detail)},
      {"reference", reference},
      {"candidate", candidate},
      {"conditions_differ", conditionsDiffer(reference, candidate)},
      {"metrics", QJsonValue()},
      {"verdict", QJsonValue()},
      {"regions", QJsonValue()},
      {"difference_image", QJsonValue()}};
  if (outcome.outcome != "compared") {
    ++notCompared;
    return row;
  }
  pixelsScanned += outcome.result.pixels;
  const QJsonObject decision = verdict(outcome.result, settings.tolerance);
  if (decision.value("passed").toBool())
    ++passed;
  else
    ++failedPairs;
  worstPpm = std::max(worstPpm,
                      outcome.result.changed * 1000000 / outcome.result.pixels);
  row.insert("metrics", outcome.result.metrics);
  row.insert("verdict", decision);
  row.insert("regions", outcome.result.regions);
  row.insert("difference_image", writeDifference(outcome, pair.index));
  if (pair.index < int(tiles.size())) tiles[size_t(pair.index)] = outcome.tiles;
  return row;
}

void ReviewJob::finishSheet() {
  if (!settings.sheet) return finish(State::Completed);
  QImage canvas(layout.width, layout.height, QImage::Format_RGBA8888);
  canvas.fill(Qt::transparent);
  QJsonArray map;
  int y = SheetGutter;
  for (int row = 0; row < layout.rows; ++row) {
    for (int column = 0; column < 3; ++column) {
      const QImage& tile = tiles[size_t(row)][size_t(column)];
      if (tile.isNull()) continue;
      const int x = SheetGutter + column * (layout.cell + SheetGutter);
      for (int line = 0; line < tile.height(); ++line)
        std::copy(tile.constScanLine(line),
                  tile.constScanLine(line) + tile.width() * 4,
                  canvas.scanLine(y + line) + x * 4);
      map.append(QJsonObject{{"pair", row},
                             {"column", QJsonArray{"reference", "candidate",
                                                   "difference"}[column]},
                             {"x", x},
                             {"y", y},
                             {"width", tile.width()},
                             {"height", tile.height()}});
    }
    y += layout.rowHeights[size_t(row)] + SheetGutter;
  }
  const auto artifact = publishImage(encodePng(canvas), "sheet", -1,
                                     canvas.width(), canvas.height());
  sheet               = QJsonObject{
      {"status", artifact ? "written" : "omitted_storage"},
      {"artifact_id", artifact ? QJsonValue(artifact->id) : QJsonValue()},
      {"width", layout.width},
      {"height", layout.height},
      {"factor", layout.factor},
      {"pixel_sha256",
       pixelSha256(canvas.constBits(), canvas.width(), canvas.height())},
      {"tiles", map}};
  finish(State::Completed);
}

QJsonValue ReviewJob::result(Context&, const QJsonObject& params) {
  if (!terminal())
    throw Error("result_unavailable",
                "The job has not finished; poll job.status.",
                {{"status", statusText()}});
  if (state == State::Failed)
    throw Error("result_unavailable", "The job failed and has no rows.",
                {{"status", "failed"}, {"failure", common.failure}});
  std::vector<QJsonObject> all = rows;
  all.push_back(summary());
  const int offset = integerParam(params, "offset", 0, 65536, 0);
  const int limit  = integerParam(params, "limit", 1, 256, DefaultPageRows);
  QJsonArray page;
  for (int index = offset; index < int(all.size()) && page.size() < limit;
       ++index)
    page.append(all[size_t(index)]);
  const int end = offset + page.size();
  return QJsonObject{
      {"job_id", common.id},
      {"kind", common.kind},
      {"check", "image_review"},
      {"status", statusText()},
      {"complete", complete()},
      {"reasons", QJsonArray::fromStringList(reasons)},
      {"summary", summary()},
      {"rows", page},
      {"total_rows", int(all.size())},
      {"offset", offset},
      {"next_offset", end < int(all.size()) ? QJsonValue(end) : QJsonValue()},
      {"source_revision", common.revision}};
}

qint64 reviewReservations(Context& context) {
  qint64 total = 0;
  for (const auto& job : jobRegistry(context).jobs())
    if (dynamic_cast<const ReviewJob*>(job.get()))
      total += job->reservedBytes();
  return total;
}

QJsonValue submitReview(Context& context, const QJsonObject& params) {
  Document& document = context.document(requiredString(params, "document_id"));
  const Settings settings           = readSettings(params);
  const std::vector<PairSpec> pairs = readPairs(context, params);
  if (pairs.empty() || int(pairs.size()) > MaxPairs)
    refuse("invalid_params", "Compare between 1 and 120 pairs.");
  requireRegion(settings, pairs);
  JobRegistry& registry = jobRegistry(context);
  registry.admitAnalysis({}, 0);
  if (reviewReservations(context) + JobBytes > WorkerBytes)
    refuse("resource_limit", "This worker's review storage is full.",
           {{"limit", "review_storage"}, {"max_bytes", double(WorkerBytes)}});
  auto job               = std::make_shared<ReviewJob>();
  job->context           = &context;
  job->settings          = settings;
  job->pairs             = pairs;
  job->layout            = sheetLayout(pairs, settings.region);
  job->common.id         = registry.newJobId();
  job->common.kind       = "image_review";
  job->common.lane       = JobLane::Analysis;
  job->common.documentId = document.id;
  job->common.revision   = document.revisionText();
  QJsonObject request    = params;
  request.remove("operation_id");
  request.remove("expected_revision");
  job->common.request     = request;
  job->common.submittedMs = registry.elapsedMs();
  job->workspace          = context.workspace();
  try {
    job->directory = context.resolvePath(
        context.workspace() + "/reviews/" + job->common.id, true);
  } catch (const Error&) {
    refuse("io_error", "The worker's private review folder is unavailable.");
  }
  context.markMutationStarted();
  if (QFileInfo::exists(job->directory) || !QDir().mkpath(job->directory) ||
      !job->ownership.acquire(job->directory)) {
    context.resetMutationStarted();
    refuse("io_error", "A private review folder could not be created.");
  }
  registry.add(job);
  job->start();
  return job->record();
}

QJsonObject sideSchema() {
  const QJsonObject text{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
  return {
      {"type", "object"},
      {"additionalProperties", false},
      {"properties",
       QJsonObject{
           {"kind", QJsonObject{{"type", "string"},
                                {"enum", QJsonArray{"artifact", "job_frame"}}}},
           {"artifact_id", text},
           {"job_id", text},
           {"frame",
            QJsonObject{
                {"type", "integer"}, {"minimum", 0}, {"maximum", 1000000}}}}},
      {"required", QJsonArray{"kind"}}};
}

QJsonObject bounded(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject strictObject(const QJsonObject& properties,
                         const QJsonArray& required) {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"properties", properties},
          {"required", required}};
}

QJsonObject toleranceSchema() {
  QJsonArray names;
  for (const Tolerance& profile : toleranceProfiles())
    names.append(profile.name);
  return {{"oneOf",
           QJsonArray{QJsonObject{{"type", "string"}, {"enum", names}},
                      strictObject({{"channel_threshold", bounded(0, 255)},
                                    {"max_changed_ppm", bounded(0, 1000000)},
                                    {"max_abs_alpha", bounded(0, 255)}},
                                   {"channel_threshold", "max_changed_ppm"})}}};
}

}  // namespace

QJsonArray compareOperations() {
  const QJsonObject text{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
  const QJsonObject pair = strictObject(
      {{"reference", sideSchema()},
       {"candidate", sideSchema()},
       {"label",
        QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}}},
      {"reference", "candidate"});
  const QJsonObject range =
      strictObject({{"reference_job_id", text},
                    {"candidate_job_id", text},
                    {"frames", strictObject({{"start", bounded(0, 1000000)},
                                             {"end", bounded(0, 1000000)}},
                                            {"start", "end"})}},
                   {"reference_job_id", "candidate_job_id"});
  const QJsonObject properties{
      {"document_id", text},
      {"pairs", QJsonObject{{"type", "array"},
                            {"items", pair},
                            {"minItems", 1},
                            {"maxItems", MaxPairs}}},
      {"range", range},
      {"mode", QJsonObject{{"type", "string"},
                           {"enum", QJsonArray{"premultiplied", "straight"}}}},
      {"channels",
       QJsonObject{{"type", "string"}, {"enum", QJsonArray{"rgba", "rgb"}}}},
      {"region", strictObject({{"x", bounded(0, MaxDimension - 1)},
                               {"y", bounded(0, MaxDimension - 1)},
                               {"width", bounded(1, MaxDimension)},
                               {"height", bounded(1, MaxDimension)}},
                              {"x", "y", "width", "height"})},
      {"tolerance", toleranceSchema()},
      {"grid",
       strictObject({{"columns", bounded(1, 32)}, {"rows", bounded(1, 32)}},
                    {"columns", "rows"})},
      {"components", bounded(0, 64)},
      {"difference_images",
       QJsonObject{{"type", "string"},
                   {"enum", QJsonArray{"changed", "all", "none"}}}},
      {"sheet", QJsonObject{{"type", "boolean"}}},
      {"deadline_ms", bounded(1000, 600000)}};
  QJsonObject descriptor = operation(
      "review.compare.submit",
      "Start a job that compares pairs of 8-bit PNG render frames (artifacts, "
      "or frames of two completed jobs) by fixed integer metrics: per-channel "
      "differences, changed pixels against a tolerance, a grid and connected "
      "regions, with every difference in DPI, camera, crop, colour, alpha and "
      "frame disclosed. Optionally writes difference images and one review "
      "sheet as artifacts. The result is visual evidence only. Read the rows "
      "with job.result.",
      properties, {"document_id"}, true);
  descriptor.insert("mutates_document", false);
  descriptor.insert("annotations", QJsonObject{{"readOnlyHint", false},
                                               {"destructiveHint", false},
                                               {"idempotentHint", true},
                                               {"openWorldHint", false}});
  return {descriptor};
}

QJsonValue executeCompare(Context& context, const QString& method,
                          const QJsonObject& params) {
  if (method == "review.compare.submit") return submitReview(context, params);
  throw Error("unknown_operation", "Unknown native review operation.");
}

std::optional<QJsonValue> readReviewArtifact(Context& context,
                                             const QString& artifactId) {
  for (const auto& entry : jobRegistry(context).jobs()) {
    const auto* job = dynamic_cast<const ReviewJob*>(entry.get());
    if (!job) continue;
    for (const ReviewArtifact& artifact : job->artifacts) {
      if (artifact.id != artifactId) continue;
      const QString path = context.resolvePath(artifact.path, false);
      QFile file;
      if (path != artifact.path ||
          openApprovedFile(file, path) != OpenResult::Opened)
        throw Error("invalid_artifact",
                    "The review artifact is no longer owned.");
      if (artifact.bytes > InlineBytes)
        throw Error("invalid_artifact",
                    "The review image exceeds the inline read limit; use its "
                    "digest from job.result.",
                    {{"size_bytes", double(artifact.bytes)}});
      const QByteArray data = file.read(InlineBytes + 1);
      const QString digest  = QString::fromLatin1(
          QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
      if (digest != artifact.sha256)
        throw Error("invalid_artifact",
                    "The review artifact was modified after the job wrote it.");
      return QJsonObject{
          {"kind",
           artifact.role == "sheet" ? "review_sheet" : "review_difference"},
          {"artifact_id", artifact.id},
          {"job_id", job->common.id},
          {"document_id", job->common.documentId},
          {"source_revision", job->common.revision},
          {"width", artifact.width},
          {"height", artifact.height},
          {"mime_type", "image/png"},
          {"size_bytes", double(artifact.bytes)},
          {"sha256", artifact.sha256},
          {"alpha", "straight"},
          {"image",
           QJsonObject{{"mimeType", "image/png"},
                       {"data", QString::fromLatin1(data.toBase64())}}}};
    }
  }
  return std::nullopt;
}

void sweepAbandonedReviews(Context& context) {
  OwnershipClaim::removeAbandoned(context.workspace() + "/reviews", {"job_*"});
}

}  // namespace GroundzeroAutomation
