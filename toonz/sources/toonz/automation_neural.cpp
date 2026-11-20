#include "automation_neural.h"

#include "trasterimage.h"
#include "toonz/txshlevel.h"
#include "toonz/txshsimplelevel.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <cmath>

namespace GroundzeroAutomation {
namespace {
using Execution              = NeuralStyle::WorkerExecution;
constexpr qint64 ImageBudget = 96 * 1024 * 1024;
// The native global checkpoint cache can remain populated from earlier jobs.
constexpr qint64 CheckpointCache = 128LL * 384 * 384 * 4;
// One serialized compute: source/output/writer RGBA8 images plus bounded model
// staging.
constexpr qint64 CameraImages = 3LL * 1024 * 1024 * 4;
constexpr qint64 ModelStaging = 12 * 1024 * 1024;
static_assert(CheckpointCache + CameraImages + ModelStaging <= ImageBudget,
              "Neural preview exceeds the image/cache reservation");

QJsonObject objectSchema(const QJsonObject& properties,
                         const QJsonArray& required) {
  return {{"type", "object"},
          {"properties", properties},
          {"required", required},
          {"additionalProperties", false}};
}

QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject referenceSchema() {
  const QJsonObject number{{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
  auto positive = number;
  positive.insert("exclusiveMinimum", 0);
  const auto crop = objectSchema(
      {{"x", number}, {"y", number}, {"width", positive}, {"height", positive}},
      {"x", "y", "width", "height"});
  const auto drawing = objectSchema(
      {{"number", integerSchema(1, 999999)},
       {"suffix",
        QJsonObject{{"type", "string"}, {"pattern", "^[a-z]{0,8}$"}}}},
      {"number", "suffix"});
  return objectSchema(
      {{"level_id", QJsonObject{{"type", "string"}, {"minLength", 1}}},
       {"drawing_id", drawing},
       {"crop", crop}},
      {"level_id", "drawing_id", "crop"});
}

bool beneath(const QString& path, const QString& root) {
#ifdef _WIN32
  const auto sensitivity = Qt::CaseInsensitive;
#else
  const auto sensitivity = Qt::CaseSensitive;
#endif
  const QString prefix = root.endsWith('/') ? root : root + '/';
  return path.compare(root, sensitivity) == 0 ||
         path.startsWith(prefix, sensitivity);
}

QString packageDirectory(const Context& context) {
  const QString application =
      QFileInfo(QCoreApplication::applicationDirPath()).canonicalFilePath();
  const QString directory = QDir(application).filePath("neural-style");
  const QString canonical = QFileInfo(directory).canonicalFilePath();
  const QString checked   = canonical.isEmpty() ? directory : canonical;
  if (beneath(checked, context.workspace()) ||
      beneath(context.workspace(), checked))
    throw Error(
        "unsafe_component_location",
        "Neural components must be outside the worker's writable workspace.");
  return directory;
}

bool validCrop(const QRectF& crop) {
  return crop.x() >= 0 && crop.y() >= 0 && crop.width() > 0 &&
         crop.height() > 0 && crop.right() <= 1 && crop.bottom() <= 1;
}

QRectF cropRect(const QJsonObject& reference) {
  const auto crop = reference.value("crop").toObject();
  double values[4];
  int index = 0;
  for (const QString& name : {"x", "y", "width", "height"}) {
    const auto value = crop.value(name);
    values[index]    = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(values[index]))
      throw Error("invalid_params",
                  "Reference crop coordinates must be finite numbers.");
    ++index;
  }
  const QRectF result(values[0], values[1], values[2], values[3]);
  if (!validCrop(result))
    throw Error("invalid_params",
                "Reference crop must be positive and inside the image.");
  return result;
}

TRaster32P referenceRaster(Document& document, const QJsonObject& reference) {
  const auto drawing = reference.value("drawing_id").toObject();
  const int number   = integerParam(drawing, "number", 1, 999999, -1);
  const auto suffix  = drawing.value("suffix");
  if (!suffix.isString() ||
      !QRegularExpression(QRegularExpression::anchoredPattern("[a-z]{0,8}"))
           .match(suffix.toString())
           .hasMatch())
    throw Error("invalid_params",
                "Drawing suffix must contain up to eight lowercase letters.");
  auto* level =
      document.level(requiredString(reference, "level_id"))->getSimpleLevel();
  const TFrameId frame(number, suffix.toString());
  if (!level || !level->isFid(frame))
    throw Error("unknown_drawing",
                "Reference must name an existing same-document drawing.");
  const TRasterImageP image = level->getFrame(frame, false);
  const TRaster32P raster   = image ? image->getRaster() : TRasterP();
  if (!raster)
    throw Error("unsupported_reference",
                "Neural preview references require RGBA8 drawings.");
  return raster;
}

void requireReferenceSize(qint64 pixels) {
  if (pixels <= 0 ||
      pixels > (ImageBudget - CheckpointCache - 4 * 1024 * 1024) / 16)
    throw Error("resource_limit",
                "Reference capture exceeds the 96 MiB image/cache budget.");
}

QImage referenceImage(const TRaster32P& raster,
                      Execution::Clock::time_point deadline) {
  const qint64 pixels = qint64(raster->getLx()) * raster->getLy();
  requireReferenceSize(pixels);
  QImage result(raster->getLx(), raster->getLy(),
                QImage::Format_ARGB32_Premultiplied);
  if (result.isNull())
    throw Error("resource_limit", "Cannot allocate the reference capture.");
  raster->lock();
  try {
    for (int y = 0; y < result.height(); ++y) {
      checkNeuralDeadline(deadline);
      const auto* source = raster->pixels(result.height() - 1 - y);
      auto* destination  = reinterpret_cast<QRgb*>(result.scanLine(y));
      for (int x = 0; x < result.width(); ++x)
        destination[x] =
            qRgba(source[x].r, source[x].g, source[x].b, source[x].m);
    }
  } catch (...) {
    raster->unlock();
    throw;
  }
  raster->unlock();
  if (result.width() > 1024 || result.height() > 1024)
    result = result.scaled(1024, 1024, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation);
  return result;
}

QJsonObject metadata(const NeuralPreview& preview, const QJsonObject& reference,
                     const QString& revision) {
  const QByteArray profile = NeuralStyle::serialize(preview.profile).toUtf8();
  if (profile.isEmpty())
    throw Error("reference_capture_failed",
                "Cannot encode captured reference.");
  QJsonObject result{
      {"source_revision", revision},
      {"source_frame", preview.frame},
      {"reference", reference},
      {"profile_sha256",
       QString::fromLatin1(
           QCryptographicHash::hash(profile, QCryptographicHash::Sha256)
               .toHex())},
      {"package_manifest_sha256", preview.execution->manifestSha256},
      {"strength", preview.strength},
      {"temporal", 0},
      {"reference_alpha", "composited_over_white"},
      {"deadline_milliseconds", 120000}};
  for (const QString& role : {"worker", "runtime", "prediction", "transfer"})
    result.insert(
        role + "_sha256",
        preview.execution->files.value(role).toObject().value("sha256"));
  return result;
}
}  // namespace

void checkNeuralDeadline(Execution::Clock::time_point deadline) {
  if (Execution::Clock::now() >= deadline)
    throw Error("deadline_exceeded",
                "Neural preview exceeded its total work budget.");
}

QJsonObject neuralPreviewOperation() {
  const QJsonObject text{{"type", "string"}, {"minLength", 1}};
  auto descriptor =
      operation("assistance.neural.preview",
                "Render one immutable native Neural Style preview using a "
                "captured same-document reference.",
                {{"document_id", text},
                 {"frame", integerSchema(0, 999999)},
                 {"reference", referenceSchema()},
                 {"strength", QJsonObject{{"type", "number"},
                                          {"minimum", 1},
                                          {"maximum", 100},
                                          {"default", 35}}}},
                {"document_id", "frame", "reference"}, true);
  auto schema     = descriptor.value("inputSchema").toObject();
  auto properties = schema.value("properties").toObject();
  properties.insert("expected_revision",
                    QJsonObject{{"type", "string"}, {"pattern", "^[0-9]+$"}});
  properties.insert(
      "operation_id",
      QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}});
  schema.insert("properties", properties);
  schema.insert("required", QJsonArray{"document_id", "expected_revision",
                                       "operation_id", "frame", "reference"});
  descriptor.insert("inputSchema", schema);
  return descriptor;
}

NeuralPreview prepareNeuralPreview(Context& context, Document& document,
                                   const QJsonObject& params,
                                   Execution::Clock::time_point deadline) {
  checkNeuralDeadline(deadline);
  NeuralPreview preview;
  preview.frame    = integerParam(params, "frame", 0, 999999, -1);
  preview.strength = params.value("strength").toDouble(35);
  if (!std::isfinite(preview.strength) || preview.strength < 1 ||
      preview.strength > 100)
    throw Error("invalid_params", "Neural strength must be between 1 and 100.");
  const auto reference = params.value("reference").toObject();
  const QRectF crop    = cropRect(reference);
  const auto image =
      referenceImage(referenceRaster(document, reference), deadline);
  QString error;
  if (!NeuralStyle::capture(image, {}, crop, preview.profile, error))
    throw Error("reference_capture_failed", error);
  checkNeuralDeadline(deadline);
  preview.execution = NeuralStyle::captureWorkerExecution(
      packageDirectory(context), deadline, error);
  if (!preview.execution) throw Error("component_unavailable", error);
  preview.metadata = metadata(preview, reference, document.revisionText());
  checkNeuralDeadline(deadline);
  return preview;
}
}  // namespace GroundzeroAutomation
