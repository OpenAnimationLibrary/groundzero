#include "automation.h"

#include "toonz/boardsettings.h"
#include "toonz/movierenderer.h"
#include "toonz/preferences.h"
#include "toonz/scenefx.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toutputproperties.h"
#include "tproperty.h"
#include "tiio.h"
#include "texception.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <functional>
#include <set>
#include <utility>
#include <vector>

namespace GroundzeroAutomation {

namespace RenderDetail {

constexpr int kMaxFrames         = 120;
constexpr int kMaxDimension      = 2048;
constexpr int kPreviewDimension  = 1024;
constexpr int kMaxJobs           = 32;
constexpr qint64 kInlineBytes    = 4 * 1024 * 1024;
constexpr qint64 kFrameBytes     = 20 * 1024 * 1024;
constexpr qint64 kWorkspaceBytes = 128 * 1024 * 1024;

QString newId(const QString& prefix) {
  return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject stringSchema() { return {{"type", "string"}, {"minLength", 1}}; }

QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

struct Request final {
  int width  = 0;
  int height = 0;
  int start  = 0;
  int end    = 0;
  int step   = 1;

  int count() const { return (end - start) / step + 1; }
  qint64 reserveBytes() const {
    return (qint64(width) * height * 4 + 65536) * count();
  }
};

TDimension readDimensions(const QJsonObject& params, ToonzScene& scene,
                          bool preview) {
  const int bound             = preview ? kPreviewDimension : kMaxDimension;
  const TDimension resolution = scene.getCurrentCamera()->getRes();
  if (resolution.lx <= 0 || resolution.ly <= 0)
    throw Error("invalid_camera",
                "The scene camera must have positive dimensions.");
  if (params.contains("width") != params.contains("height"))
    throw Error("invalid_params",
                "Specify both width and height when resizing the output.");
  const double scale =
      preview ? std::min(1.0,
                         double(bound) / std::max(resolution.lx, resolution.ly))
              : 1.0;
  const int width  = integerParam(params, "width", 1, bound,
                                  std::max(1, int(resolution.lx * scale)));
  const int height = integerParam(params, "height", 1, bound,
                                  std::max(1, int(resolution.ly * scale)));
  if (width > bound || height > bound)
    throw Error("render_limit",
                "Specify width and height within the render limit.");
  return TDimension(width, height);
}

Request readRequest(const QJsonObject& params, ToonzScene& scene,
                    bool preview) {
  Request request;
  const TDimension dimensions = readDimensions(params, scene, preview);
  request.width               = dimensions.lx;
  request.height              = dimensions.ly;
  if (preview) {
    request.start = integerParam(params, "frame", 0, 1000000, 0);
    request.end   = request.start;
  } else {
    request.start = integerParam(params, "start_frame", 0, 1000000, 0);
    request.end   = integerParam(params, "end_frame", request.start, 1000000,
                                 request.start);
    request.step  = integerParam(params, "step", 1, 1000000, 1);
  }
  if (request.count() > kMaxFrames || request.reserveBytes() > kWorkspaceBytes)
    throw Error("render_limit", "Reduce the frame count or output dimensions.");
  return request;
}

struct Artifact final {
  QString id;
  QString path;
  QString sha256;
  int frame    = 0;
  int width    = 0;
  int height   = 0;
  qint64 bytes = 0;

  QJsonObject metadata() const {
    return {{"artifact_id", id},        {"frame", frame},
            {"width", width},           {"height", height},
            {"mime_type", "image/png"}, {"size_bytes", double(bytes)},
            {"sha256", sha256},         {"alpha", "straight"}};
  }
};

QByteArray readOutput(const QString& path, qint64 limit) {
  QFileInfo info(path);
  if (info.canonicalFilePath() != QDir::cleanPath(path))
    throw Error("invalid_artifact",
                "The artifact path is no longer an owned regular file.");
  if (!info.isFile() || info.isSymLink() || info.size() <= 0 ||
      info.size() > limit)
    throw Error("invalid_artifact",
                "The rendered file is missing or exceeds its size limit.");
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    throw Error("artifact_read_failed", "The rendered file cannot be opened.");
  const QByteArray data = file.read(limit + 1);
  if (file.error() != QFileDevice::NoError || data.size() != info.size())
    throw Error("artifact_read_failed",
                "The rendered file could not be read completely.");
  return data;
}

Artifact verifyOutput(const QString& path, int frame, const Request& request) {
  const QByteArray data = readOutput(path, kFrameBytes);
  QBuffer buffer;
  buffer.setData(data);
  if (!buffer.open(QIODevice::ReadOnly))
    throw Error("artifact_read_failed",
                "The PNG decoder buffer could not be opened.");
  QImageReader reader(&buffer, "png");
  if (reader.size() != QSize(request.width, request.height))
    throw Error("invalid_artifact",
                "The rendered PNG dimensions differ from the request.");
  const QImage decoded = reader.read();
  if (decoded.isNull() || !decoded.hasAlphaChannel())
    throw Error("invalid_artifact",
                "The rendered PNG cannot be decoded as an alpha image.");
  Artifact artifact;
  artifact.id     = newId("artifact_");
  artifact.path   = path;
  artifact.frame  = frame;
  artifact.width  = decoded.width();
  artifact.height = decoded.height();
  artifact.bytes  = data.size();
  artifact.sha256 = QString::fromLatin1(
      QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
  return artifact;
}

enum class JobStatus {
  Preparing,
  Running,
  Canceling,
  Finalizing,
  Completed,
  Failed,
  Canceled
};

QString statusText(JobStatus status) {
  switch (status) {
  case JobStatus::Preparing:
    return "preparing";
  case JobStatus::Running:
    return "running";
  case JobStatus::Canceling:
    return "cancellation_requested";
  case JobStatus::Finalizing:
    return "finalizing";
  case JobStatus::Completed:
    return "completed";
  case JobStatus::Failed:
    return "failed";
  case JobStatus::Canceled:
    return "canceled";
  }
  throw Error("internal_error", "Unknown render job state.");
}

class RenderJob final : public MovieRenderer::Listener,
                        public std::enable_shared_from_this<RenderJob> {
public:
  QString id;
  QString documentId;
  QString revision;
  QString directory;
  QString error;
  Request request;
  std::unique_ptr<ToonzScene> snapshot;
  std::unique_ptr<MovieRenderer> renderer;
  std::function<void()> nativeCancel;
  std::atomic<bool> cancellation{false};
  JobStatus status = JobStatus::Preparing;
  std::set<int> completed;
  std::set<int> failed;
  std::vector<Artifact> artifacts;
  int nextFrame        = 0;
  int nextVerification = 0;
  qint64 bytes         = 0;
  bool writersClosed   = false;
  double frameRate     = 0;

  bool terminal() const {
    return status == JobStatus::Completed || status == JobStatus::Failed ||
           status == JobStatus::Canceled;
  }

  QJsonObject describe() const {
    return {{"job_id", id},
            {"document_id", documentId},
            {"source_revision", revision},
            {"status", statusText(status)},
            {"frame_count", request.count()},
            {"completed_frames", int(completed.size())},
            {"failed_frames", int(failed.size())},
            {"writers_closed", writersClosed},
            {"width", request.width},
            {"height", request.height},
            {"start_frame", request.start},
            {"end_frame", request.end},
            {"step", request.step},
            {"frame_rate", frameRate},
            {"profile", "rgba8-png"},
            {"color_space", "nonlinear"},
            {"color_space_gamma", 2.2},
            {"alpha", "straight"},
            {"error", error}};
  }

  TFilePath outputPath() const {
    return TFilePath((directory + "/frames/frame..png").toStdWString());
  }

  void releaseNativeState() {
    Q_ASSERT(writersClosed);
    nativeCancel = {};
    renderer.reset();
    snapshot.reset();
  }

  void fail(const QString& message) {
    error = message.left(2048);
    artifacts.clear();
    status = JobStatus::Failed;
    releaseNativeState();
  }

  void scheduleBuild() {
    const auto self = shared_from_this();
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [self]() { self->buildFrame(); });
  }

  void buildFrame() {
    if (cancellation) {
      writersClosed = true;
      status        = JobStatus::Canceled;
      releaseNativeState();
      return;
    }
    try {
      TFxPair pair;
      const TRenderSettings& settings =
          snapshot->getProperties()->getOutputProperties()->getRenderSettings();
      pair.m_frameA = buildSceneFx(snapshot.get(), nextFrame, 1, false,
                                   &settings.m_activeChecks);
      if (!pair.m_frameA)
        throw Error("render_failed",
                    "The requested frame has no renderable output.");
      renderer->addFrame(nextFrame, pair);
      nextFrame += request.step;
      if (nextFrame <= request.end) {
        scheduleBuild();
      } else {
        status = JobStatus::Running;
        renderer->start();
      }
    } catch (const Error& exception) {
      writersClosed = true;
      fail(exception.message);
    } catch (const TException& exception) {
      writersClosed = true;
      fail(QString::fromStdWString(exception.getMessage()));
    } catch (const std::exception& exception) {
      writersClosed = true;
      fail(QString::fromUtf8(exception.what()));
    } catch (...) {
      writersClosed = true;
      fail("The native frame graph could not be prepared.");
    }
  }

  void cancel() {
    if (terminal()) return;
    if (cancellation.exchange(true)) return;
    status = JobStatus::Canceling;
    cancelUntilClosed();
  }

  void cancelUntilClosed() {
    if (terminal() || writersClosed) return;
    if (nativeCancel) nativeCancel();
    // Native activation is queued: an initial stop can precede registration
    // of the render instance. Keep the request active until writers close.
    const auto self = shared_from_this();
    QTimer::singleShot(10, QCoreApplication::instance(),
                       [self]() { self->cancelUntilClosed(); });
  }

  bool onFrameCompleted(int frame) override {
    const auto self = shared_from_this();
    QTimer::singleShot(0, QCoreApplication::instance(), [self, frame]() {
      if (self->terminal()) return;
      self->completed.insert(frame);
      if (!self->cancellation &&
          int(self->completed.size()) == self->request.count())
        self->status = JobStatus::Finalizing;
    });
    return !cancellation;
  }

  bool onFrameFailed(int frame, TException& exception) override {
    const auto self = shared_from_this();
    const QString message =
        QString::fromStdWString(exception.getMessage()).left(2048);
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [self, frame, message]() {
                         if (self->terminal()) return;
                         self->failed.insert(frame);
                         self->error = message;
                       });
    return !cancellation;
  }

  void onSequenceCompleted(const TFilePath&) override {}

  void onSequenceFinished(const TFilePath&,
                          MovieRenderer::CompletionStatus result,
                          const std::wstring& message) override {
    const auto self          = shared_from_this();
    const QString diagnostic = QString::fromStdWString(message).left(2048);
    QTimer::singleShot(
        0, QCoreApplication::instance(),
        [self, result, diagnostic]() { self->finish(result, diagnostic); });
  }

  void finish(MovieRenderer::CompletionStatus result,
              const QString& diagnostic) {
    writersClosed = true;
    // The native terminal callback and render-end cleanup run on this thread.
    // This queued continuation runs after they return. Drop graphs before the
    // snapshot scene; artifact verification only needs the closed output files.
    releaseNativeState();
    if (cancellation || result == MovieRenderer::CompletionStatus::Canceled) {
      status = JobStatus::Canceled;
      return;
    }
    if (result == MovieRenderer::CompletionStatus::Failed) {
      fail(diagnostic.isEmpty() ? "Native rendering failed." : diagnostic);
      return;
    }
    if (int(completed.size()) != request.count() || !failed.empty()) {
      fail("Rendering ended without completing every requested frame.");
      return;
    }
    status           = JobStatus::Finalizing;
    nextVerification = request.start;
    scheduleVerification();
  }

  void scheduleVerification() {
    const auto self = shared_from_this();
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [self]() { self->verifyFrame(); });
  }

  void verifyFrame() {
    if (cancellation) {
      artifacts.clear();
      status = JobStatus::Canceled;
      return;
    }
    try {
      const TFilePath path =
          outputPath().withFrame(TFrameId(nextVerification + 1));
      Artifact artifact =
          verifyOutput(QString::fromStdWString(path.getWideString()),
                       nextVerification, request);
      bytes += artifact.bytes;
      if (bytes > request.reserveBytes())
        throw Error("render_limit",
                    "The output exceeded the reserved storage budget.");
      artifacts.push_back(std::move(artifact));
      nextVerification += request.step;
      if (nextVerification <= request.end)
        scheduleVerification();
      else
        status = JobStatus::Completed;
    } catch (const Error& exception) {
      fail(exception.message);
    } catch (const std::exception& exception) {
      fail(QString::fromUtf8(exception.what()));
    } catch (...) {
      fail("The rendered PNG could not be verified.");
    }
  }
};

void configureSnapshot(RenderJob& job) {
  job.snapshot->getCurrentCamera()->setRes(
      TDimension(job.request.width, job.request.height));
  TOutputProperties* output =
      job.snapshot->getProperties()->getOutputProperties();
  job.frameRate = output->getFrameRate();
  TRenderSettings settings;
  settings.m_bpp              = 32;
  settings.m_linearColorSpace = false;
  settings.m_colorSpaceGamma  = 2.2;
  settings.m_gamma            = 1.0;
  settings.m_timeStretchFrom  = 1.0;
  settings.m_timeStretchTo    = 1.0;
  settings.m_shrinkX = settings.m_shrinkY = 1;
  settings.m_fieldPrevalence              = TRenderSettings::NoField;
  settings.m_stereoscopic                 = false;
  settings.m_maxTileSize                  = TOutputProperties::SmallVal;
  output->setRenderSettings(settings);
  output->setRange(job.request.start, job.request.end, job.request.step);
  output->setOffset(0);
  output->setAspectMode(TOutputProperties::NoAspectExport);
  output->setMultimediaRendering(0);
  output->getBoardSettings()->setActive(false);
  output->setPath(job.outputPath());
  output->formatTemplateFId() = TFrameId(1);
  TPropertyGroup* properties  = output->getFileFormatProperties("png");
  if (!properties)
    throw Error("render_unavailable", "The native PNG writer is unavailable.");
  TProperty* alpha    = properties->getProperty("Alpha Channel");
  auto* alphaProperty = dynamic_cast<TBoolProperty*>(alpha);
  if (!alphaProperty)
    throw Error("render_unavailable",
                "The native PNG writer lacks its alpha option.");
  alphaProperty->setValue(true);
  job.renderer.reset(
      new MovieRenderer(job.snapshot.get(), job.outputPath(), 1, false));
  job.renderer->setRenderSettings(settings);
  const TPointD dpi = job.snapshot->getCurrentCamera()->getDpi();
  job.renderer->setDpi(dpi.x, dpi.y);
  job.renderer->enablePrecomputing(false);
  job.renderer->addListener(&job);
  job.nativeCancel = job.renderer->getCancelCallback();
  job.nextFrame    = job.request.start;
}

}  // namespace RenderDetail

struct RenderState final {
  std::map<QString, std::shared_ptr<RenderDetail::RenderJob>> jobs;

  std::shared_ptr<RenderDetail::RenderJob> job(const QString& id) const {
    const auto found = jobs.find(id);
    if (found == jobs.end())
      throw Error("unknown_job",
                  "The render job is unknown or belongs to an expired worker.");
    return found->second;
  }

  bool idle() const {
    for (const auto& entry : jobs)
      if (!entry.second->terminal()) return false;
    return true;
  }

  void admit(const RenderDetail::Request& request) const {
    if (!idle())
      throw Error("render_busy",
                  "Wait for the active render job to finish or cancel it.");
    if (int(jobs.size()) >= RenderDetail::kMaxJobs)
      throw Error("render_limit",
                  "This worker has reached its render job limit.");
    qint64 reserved = request.reserveBytes();
    for (const auto& entry : jobs)
      reserved += entry.second->request.reserveBytes();
    if (reserved > RenderDetail::kWorkspaceBytes)
      throw Error("render_limit",
                  "This worker has reached its render output storage limit.");
  }
};

namespace RenderDetail {

RenderState& renderState(Context& context) {
  if (!context.renderState)
    context.renderState = std::make_shared<RenderState>();
  return *context.renderState;
}

QJsonValue submit(Context& context, const QJsonObject& params, bool preview) {
  Document& document = context.document(requiredString(params, "document_id"));
  const Request request = readRequest(params, *document.scene, preview);
  RenderState& state    = renderState(context);
  state.admit(request);
  std::unique_ptr<Tiio::Writer> writer(Tiio::makeWriter("png"));
  if (!writer)
    throw Error("render_unavailable", "The native PNG writer is unavailable.");
  if (Preferences::instance()->isSceneNumberingEnabled())
    throw Error("unsupported_render_settings",
                "Disable scene numbering before automated rendering.");
  auto job        = std::make_shared<RenderJob>();
  job->id         = newId("job_");
  job->documentId = document.id;
  job->revision   = document.revisionText();
  job->request    = request;
  job->directory =
      context.resolvePath(context.workspace() + "/renders/" + job->id, true);
  context.markMutationStarted();
  if (QFileInfo::exists(job->directory) ||
      !QDir().mkpath(job->directory + "/frames"))
    throw Error("render_output_failed",
                "A new private render output directory could not be created.");
  job->snapshot =
      createRenderSnapshot(context, document, job->directory + "/snapshot");
  configureSnapshot(*job);
  state.jobs.emplace(job->id, job);
  job->scheduleBuild();
  return job->describe();
}

QJsonValue artifactList(const RenderJob& job) {
  if (job.status != JobStatus::Completed)
    throw Error("artifacts_unavailable",
                "Artifacts are available only after successful finalization.",
                {{"status", statusText(job.status)}});
  QJsonArray artifacts;
  for (const Artifact& artifact : job.artifacts)
    artifacts.append(artifact.metadata());
  QJsonObject result = job.describe();
  result.insert("artifacts", artifacts);
  return result;
}

QJsonValue artifactRead(Context& context, const QString& id) {
  for (const auto& entry : renderState(context).jobs) {
    const RenderJob& job = *entry.second;
    if (job.status != JobStatus::Completed) continue;
    for (const Artifact& artifact : job.artifacts) {
      if (artifact.id != id) continue;
      const QString path = context.resolvePath(artifact.path, false);
      if (path != artifact.path)
        throw Error("invalid_artifact",
                    "The artifact path no longer matches its owned output.");
      const QByteArray data = readOutput(path, kInlineBytes);
      const QString digest  = QString::fromLatin1(
          QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
      if (digest != artifact.sha256)
        throw Error("invalid_artifact",
                    "The artifact was modified after finalization.");
      QJsonObject result = artifact.metadata();
      result.insert("job_id", job.id);
      result.insert("document_id", job.documentId);
      result.insert("source_revision", job.revision);
      result.insert(
          "image", QJsonObject{{"mimeType", "image/png"},
                               {"data", QString::fromLatin1(data.toBase64())}});
      return result;
    }
  }
  throw Error("unknown_artifact",
              "The artifact is unknown or belongs to an expired worker.");
}

QJsonObject renderCapabilities() {
  std::unique_ptr<Tiio::Writer> writer(Tiio::makeWriter("png"));
  return {{"profiles", QJsonArray{"rgba8-png"}},
          {"png_writer_available", bool(writer)},
          {"max_frames", kMaxFrames},
          {"max_dimension", kMaxDimension},
          {"max_preview_dimension", kPreviewDimension},
          {"max_active_jobs", 1},
          {"max_retained_jobs", kMaxJobs},
          {"inline_image_bytes", double(kInlineBytes)},
          {"storage_budget_bytes", double(kWorkspaceBytes)},
          {"frame_origin", 0},
          {"snapshot_inputs", "qualified-collected-native-scene"},
          {"graphics_backend", "requires-native-compatible-OpenGL"},
          {"graphics_backend_verified", false},
          {"job_lifetime", "worker-session"}};
}

QJsonObject jobOperation(const QString& name, const QString& description,
                         QJsonObject properties, QJsonArray required) {
  QJsonObject descriptor =
      operation(name, description, properties, required, true);
  descriptor.insert("mutates_document", false);
  return descriptor;
}

QJsonValue executeJob(Context& context, const QString& method,
                      const QJsonObject& params) {
  const auto job = renderState(context).job(requiredString(params, "job_id"));
  if (method == "render.status") return job->describe();
  if (method == "render.artifacts") return artifactList(*job);
  if (method != "render.cancel")
    throw Error("unknown_operation", "Unknown native render operation.");
  if (!job->terminal()) context.markMutationStarted();
  job->cancel();
  return job->describe();
}

}  // namespace RenderDetail

QJsonArray renderOperations() {
  using RenderDetail::integerSchema;
  using RenderDetail::jobOperation;
  using RenderDetail::stringSchema;
  const QJsonObject document{{"document_id", stringSchema()}};
  const QJsonObject job{{"job_id", stringSchema()}};
  QJsonObject preview = document;
  preview.insert("frame", integerSchema(0, 1000000));
  preview.insert("width", integerSchema(1, RenderDetail::kPreviewDimension));
  preview.insert("height", integerSchema(1, RenderDetail::kPreviewDimension));
  QJsonObject sequence = document;
  sequence.insert("start_frame", integerSchema(0, 1000000));
  sequence.insert("end_frame", integerSchema(0, 1000000));
  sequence.insert("step", integerSchema(1, 1000000));
  sequence.insert("width", integerSchema(1, RenderDetail::kMaxDimension));
  sequence.insert("height", integerSchema(1, RenderDetail::kMaxDimension));
  return {
      operation("render.capabilities",
                "Inspect the bounded native PNG render profile.", {}, {}),
      jobOperation("render.preview",
                   "Start an immutable one-frame native PNG preview job.",
                   preview, {"document_id", "frame"}),
      jobOperation("render.submit",
                   "Start an immutable bounded native PNG sequence job.",
                   sequence, {"document_id", "start_frame", "end_frame"}),
      operation("render.status",
                "Inspect render progress and writer finalization.", job,
                {"job_id"}),
      jobOperation("render.cancel",
                   "Request cancellation; poll status until terminal.", job,
                   {"job_id"}),
      operation("render.artifacts",
                "List verified artifacts after complete writer finalization.",
                job, {"job_id"}),
      operation("artifact.read",
                "Read an owned PNG artifact as a bounded inline image.",
                {{"artifact_id", stringSchema()}}, {"artifact_id"})};
}

QJsonValue executeRender(Context& context, const QString& method,
                         const QJsonObject& params) {
  if (QThread::currentThread() != QCoreApplication::instance()->thread())
    throw Error("wrong_thread",
                "Native render commands require the application thread.");
  if (method == "render.capabilities")
    return RenderDetail::renderCapabilities();
  if (method == "render.preview")
    return RenderDetail::submit(context, params, true);
  if (method == "render.submit")
    return RenderDetail::submit(context, params, false);
  if (method == "artifact.read")
    return RenderDetail::artifactRead(context,
                                      requiredString(params, "artifact_id"));
  return RenderDetail::executeJob(context, method, params);
}

void shutdownRender(Context& context) {
  if (!context.renderState) return;
  for (const auto& entry : context.renderState->jobs) entry.second->cancel();
  if (context.renderState->idle()) return;
  QEventLoop drain;
  QTimer poll;
  QObject::connect(&poll, &QTimer::timeout, &drain, [&context, &drain]() {
    if (context.renderState->idle()) drain.quit();
  });
  poll.start(10);
  drain.exec(QEventLoop::ExcludeUserInputEvents);
}

}  // namespace GroundzeroAutomation
