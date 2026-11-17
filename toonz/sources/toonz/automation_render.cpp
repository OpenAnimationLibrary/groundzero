#include "automation.h"
#include "automation_media.h"
#include "automation_neural.h"
#include "stdfx/neuralstyleworker.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/txsheet.h"
#include "toonz/tcolumnfx.h"

#include "toonz/boardsettings.h"
#include "toonz/movierenderer.h"
#include "toonz/preferences.h"
#include "toonz/scenefx.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toutputproperties.h"
#include "tlevel_io.h"
#include "tproperty.h"
#include "tiio.h"
#include "texception.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
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

static QString newId(const QString& prefix) {
  return prefix + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

static QJsonObject stringSchema() {
  return {{"type", "string"}, {"minLength", 1}};
}

static QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

enum class Format { Png, Mp4 };

static QString formatText(Format format) {
  return format == Format::Mp4 ? "mp4" : "png";
}

static Format readFormat(const QJsonObject& params) {
  const QString format =
      params.contains("format") ? requiredString(params, "format") : "png";
  if (format == "png") return Format::Png;
  if (format == "mp4") return Format::Mp4;
  throw Error("invalid_params", "The render format must be png or mp4.");
}

static bool movieWriterRegistered() {
  QStringList formats;
  TLevelWriter::getSupportedFormats(formats, true);
  return formats.contains("mp4");
}

struct Request final {
  Format format = Format::Png;
  int width     = 0;
  int height    = 0;
  int start     = 0;
  int end       = 0;
  int step      = 1;

  int count() const { return (end - start) / step + 1; }
  qint64 reserveBytes() const {
    return (qint64(width) * height * 4 + 65536) * count();
  }
};

static TDimension readDimensions(const QJsonObject& params, ToonzScene& scene,
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

static void validateMovieRequest(const Request& request) {
  if (request.format != Format::Mp4) return;
  if (request.step != 1 || request.width % 2 != 0 || request.height % 2 != 0)
    throw Error("unsupported_render_settings",
                "MP4 requires step 1 and even output width and height.");
}

static Request readRequest(const QJsonObject& params, ToonzScene& scene,
                           bool preview) {
  Request request;
  const TDimension dimensions = readDimensions(params, scene, preview);
  request.width               = dimensions.lx;
  request.height              = dimensions.ly;
  if (preview) {
    request.start = integerParam(params, "frame", 0, 1000000, 0);
    request.end   = request.start;
  } else {
    request.format = readFormat(params);
    request.start  = integerParam(params, "start_frame", 0, 1000000, 0);
    request.end    = integerParam(params, "end_frame", request.start, 1000000,
                                  request.start);
    request.step   = integerParam(params, "step", 1, 1000000, 1);
  }
  validateMovieRequest(request);
  if (request.count() > kMaxFrames || request.reserveBytes() > kWorkspaceBytes)
    throw Error("render_limit", "Reduce the frame count or output dimensions.");
  return request;
}

struct Artifact final {
  Format format = Format::Png;
  QJsonObject movieMetadata;
  QDateTime lastModified;
  QString id;
  QString path;
  QString sha256;
  int frame    = 0;
  int width    = 0;
  int height   = 0;
  qint64 bytes = 0;

  QJsonObject metadata() const {
    if (format == Format::Mp4) {
      QJsonObject result = movieMetadata;
      result.insert("artifact_id", id);
      result.insert("kind", "movie");
      result.insert("mime_type", "video/mp4");
      return result;
    }
    return {{"kind", "image"},
            {"artifact_id", id},
            {"frame", frame},
            {"width", width},
            {"height", height},
            {"mime_type", "image/png"},
            {"size_bytes", double(bytes)},
            {"sha256", sha256},
            {"alpha", "straight"}};
  }
};

static void validateOutputFile(const QFileInfo& info, const QString& path,
                               qint64 limit) {
  if (info.canonicalFilePath() != QDir::cleanPath(path))
    throw Error("invalid_artifact",
                "The artifact path is no longer an owned regular file.");
  if (!info.isFile() || info.isSymLink() || info.size() <= 0 ||
      info.size() > limit)
    throw Error("invalid_artifact",
                "The rendered file is missing or exceeds its size limit.");
}

static QByteArray readOutput(const QString& path, qint64 limit) {
  QFileInfo info(path);
  validateOutputFile(info, path, limit);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    throw Error("artifact_read_failed", "The rendered file cannot be opened.");
  const QByteArray data = file.read(limit + 1);
  if (file.error() != QFileDevice::NoError || data.size() != info.size())
    throw Error("artifact_read_failed",
                "The rendered file could not be read completely.");
  return data;
}

static Artifact verifyOutput(const QString& path, int frame,
                             const Request& request) {
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

static QString statusText(JobStatus status) {
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
  QString workspace;
  QString mediaToolsDirectory;
  QString error;
  Request request;
  QJsonObject assistance;
  std::shared_ptr<const NeuralStyle::WorkerExecution> neuralExecution;
  std::unique_ptr<ToonzScene> snapshot;
  std::unique_ptr<MovieRenderer> renderer;
  std::shared_ptr<MovieVerification> verification;
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
    QJsonObject result{{"job_id", id},
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
            {"format", formatText(request.format)},
            {"profile",
             request.format == Format::Mp4 ? "opaque8-mp4" : "rgba8-png"},
            {"color_space", "nonlinear"},
            {"color_space_gamma", 2.2},
            {"alpha", request.format == Format::Mp4 ? "opaque" : "straight"},
            {"error", error}};
    if (!assistance.isEmpty()) result.insert("assistance", assistance);
    return result;
  }

  TFilePath outputPath() const {
    const QString suffix =
        request.format == Format::Mp4 ? "/movie.mp4" : "/frames/frame..png";
    return TFilePath((directory + suffix).toStdWString());
  }

  void releaseNativeState() {
    Q_ASSERT(writersClosed);
    nativeCancel = {};
    renderer.reset();
    if (neuralExecution && !neuralExecution->helpersClosed())
      neuralExecution->closureFailed();
    snapshot.reset();
  }

  void fail(const QString& message) {
    error = message.left(2048);
    artifacts.clear();
    status = JobStatus::Failed;
    releaseNativeState();
  }

  void expireNeural() {
    if (neuralExecution && neuralExecution->expired()) cancellation = true;
  }

  void checkNeuralPublication() const {
    if (!neuralExecution) return;
    QString diagnostic;
    if (!neuralExecution->verify(diagnostic))
      throw Error("neural_execution_changed", diagnostic);
    checkNeuralDeadline(neuralExecution->deadline);
  }

  void observeNeuralDeadline() {
    if (!neuralExecution || terminal()) return;
    if (neuralExecution->expired()) {
      cancel();
      return;
    }
    const auto self = shared_from_this();
    QTimer::singleShot(20, QCoreApplication::instance(),
                       [self]() { self->observeNeuralDeadline(); });
  }

  void scheduleBuild() {
    const auto self = shared_from_this();
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [self]() { self->buildFrame(); });
  }

  void buildFrame() {
    expireNeural();
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
    if (verification) verification->cancel();
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
    expireNeural();
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
    status = JobStatus::Finalizing;
    if (request.format == Format::Mp4) return verifyMovie();
    nextVerification = request.start;
    scheduleVerification();
  }

  void verifyMovie() {
    MovieVerificationConfig config;
    config.mediaToolsDirectory = mediaToolsDirectory;
    config.moviePath    = QString::fromStdWString(outputPath().getWideString());
    config.width        = request.width;
    config.height       = request.height;
    config.frameCount   = request.count();
    config.frameRate    = frameRate;
    config.maximumBytes = request.reserveBytes();
    const auto self     = shared_from_this();
    verification        = startMovieVerification(
        config, [self](QJsonObject metadata, QString failure) {
          QTimer::singleShot(0, QCoreApplication::instance(),
                             [self, metadata, failure]() {
                               self->movieVerified(metadata, failure);
                             });
        });
  }

  void movieVerified(QJsonObject metadata, const QString& failure) {
    verification.reset();
    if (cancellation) {
      status = JobStatus::Canceled;
      return;
    }
    if (!failure.isEmpty()) return fail(failure);
    Artifact artifact;
    artifact.format = Format::Mp4;
    artifact.id     = newId("artifact_");
    artifact.path   = QString::fromStdWString(outputPath().getWideString());
    artifact.lastModified = QFileInfo(artifact.path).lastModified();
    artifact.bytes        = qint64(metadata.value("size_bytes").toDouble());
    artifact.sha256       = metadata.value("sha256").toString();
    metadata.insert("workspace_relative_path",
                    QDir(workspace).relativeFilePath(artifact.path));
    metadata.insert("job_id", id);
    metadata.insert("document_id", documentId);
    metadata.insert("source_revision", revision);
    metadata.insert("source_frame_rate", frameRate);
    metadata.insert("start_frame", request.start);
    metadata.insert("end_frame", request.end);
    metadata.insert("step", request.step);
    artifact.movieMetadata = std::move(metadata);
    bytes                  = artifact.bytes;
    artifacts.push_back(std::move(artifact));
    status = JobStatus::Completed;
  }

  void scheduleVerification() {
    const auto self = shared_from_this();
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [self]() { self->verifyFrame(); });
  }

  void verifyFrame() {
    expireNeural();
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
      checkNeuralPublication();
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

static void configurePng(TOutputProperties& output) {
  TPropertyGroup* properties = output.getFileFormatProperties("png");
  if (!properties)
    throw Error("render_unavailable", "The native PNG writer is unavailable.");
  auto* alpha =
      dynamic_cast<TBoolProperty*>(properties->getProperty("Alpha Channel"));
  if (!alpha)
    throw Error("render_unavailable",
                "The native PNG writer lacks its alpha option.");
  alpha->setValue(true);
}

static void configureMovie(RenderJob& job, TOutputProperties& output) {
  TPropertyGroup* properties = output.getFileFormatProperties("mp4");
  if (!properties)
    throw Error("render_unavailable", "The native MP4 writer is unavailable.");
  auto* quality =
      dynamic_cast<TIntProperty*>(properties->getProperty("Quality"));
  auto* scale = dynamic_cast<TIntProperty*>(properties->getProperty("Scale"));
  if (!quality || !scale)
    throw Error(
        "render_unavailable",
        "The native MP4 writer lacks its fixed quality or scale option.");
  quality->setValue(90);
  scale->setValue(100);
  job.snapshot->getProperties()->setBgColor(TPixel32::Black);
}

static void configureSnapshot(RenderJob& job) {
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
  if (job.request.format == Format::Mp4)
    configureMovie(job, *output);
  else
    configurePng(*output);
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

static RenderState& renderState(Context& context) {
  if (!context.renderState)
    context.renderState = std::make_shared<RenderState>();
  return *context.renderState;
}

static void validateWriter(const Context& context, Format format) {
  if (format == Format::Mp4) {
    if (context.mediaToolsDirectory.isEmpty())
      throw Error(
          "render_unavailable",
          "Start the worker with an explicit --media-tools directory for MP4.");
    if (!movieWriterRegistered())
      throw Error(
          "render_unavailable",
          "The configured media tools did not register the native MP4 writer.");
    return;
  }
  std::unique_ptr<Tiio::Writer> writer(Tiio::makeWriter("png"));
  if (!writer)
    throw Error("render_unavailable", "The native PNG writer is unavailable.");
}

static QJsonValue submit(Context& context, const QJsonObject& params,
                         bool preview) {
  Document& document = context.document(requiredString(params, "document_id"));
  const Request request = readRequest(params, *document.scene, preview);
  RenderState& state    = renderState(context);
  state.admit(request);
  validateWriter(context, request.format);
  if (Preferences::instance()->isSceneNumberingEnabled())
    throw Error("unsupported_render_settings",
                "Disable scene numbering before automated rendering.");
  auto job                 = std::make_shared<RenderJob>();
  job->id                  = newId("job_");
  job->documentId          = document.id;
  job->revision            = document.revisionText();
  job->request             = request;
  job->workspace           = context.workspace();
  job->mediaToolsDirectory = context.mediaToolsDirectory;
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

static Request neuralRequest(ToonzScene& scene, const QJsonObject& params) {
  const auto resolution = scene.getCurrentCamera()->getRes();
  if (resolution.lx <= 0 || resolution.ly <= 0 || resolution.lx > 1024 || resolution.ly > 1024)
    throw Error("render_limit", "Neural preview requires a camera at most 1024 by 1024 pixels.");
  Request request;
  request.width = resolution.lx;
  request.height = resolution.ly;
  request.start = integerParam(params, "frame", 0, 999999, -1);
  request.end = request.start;
  return request;
}

static void attachNeuralPreview(RenderJob& job, const NeuralPreview& preview) {
  auto* graph = job.snapshot->getXsheet()->getFxDag();
  auto* output = graph->getCurrentOutputFx();
  if (!output || output->getInputPortCount() != 1 || !output->getInputPort(0)->getFx())
    throw Error("render_failed", "The scene has no selected renderable output.");
  TFxP neural = NeuralStyle::createWorkerPreviewFx(
      preview.profile, preview.strength, preview.frame, preview.execution);
  neural->getInputPort("Source")->setFx(output->getInputPort(0)->getFx());
  graph->getInternalFxs()->addFx(neural.getPointer());
  output->getInputPort(0)->setFx(neural.getPointer());
  job.neuralExecution = preview.execution;
  job.assistance = preview.metadata;
}

static QJsonValue submitNeural(Context& context, const QJsonObject& params) {
  const auto deadline = NeuralStyle::WorkerExecution::Clock::now() + std::chrono::seconds(120);
  if (!context.helpersContained())
    throw Error("unsupported_containment",
                "Neural preview requires the supported MCP adapter's contained worker launch.");
  auto& document = context.document(requiredString(params, "document_id"));
  const Request request = neuralRequest(*document.scene, params);
  auto& state = renderState(context);
  state.admit(request);
  validateWriter(context, Format::Png);
  if (Preferences::instance()->isSceneNumberingEnabled())
    throw Error("unsupported_render_settings", "Disable scene numbering before automated rendering.");
  validateAutomationScene(*document.scene);
  checkNeuralDeadline(deadline);
  const NeuralPreview preview = prepareNeuralPreview(context, document, params, deadline);
  auto job = std::make_shared<RenderJob>();
  job->id = newId("job_");
  job->documentId = document.id;
  job->revision = document.revisionText();
  job->request = request;
  job->workspace = context.workspace();
  job->directory = context.resolvePath(context.workspace() + "/renders/" + job->id, true);
  checkNeuralDeadline(deadline);
  context.markMutationStarted();
  if (QFileInfo::exists(job->directory) || !QDir().mkpath(job->directory + "/frames"))
    throw Error("render_output_failed", "Cannot create a private Neural preview output directory.");
  job->snapshot = createRenderSnapshot(context, document, job->directory + "/snapshot");
  checkNeuralDeadline(deadline);
  attachNeuralPreview(*job, preview);
  configureSnapshot(*job);
  checkNeuralDeadline(deadline);
  state.jobs.emplace(job->id, job);
  job->observeNeuralDeadline();
  job->scheduleBuild();
  return job->describe();
}

static void validateMovieArtifact(Context& context, const Artifact& artifact) {
  if (artifact.format != Format::Mp4) return;
  const QString path = context.resolvePath(artifact.path, false);
  const QFileInfo info(path);
  validateOutputFile(info, artifact.path, artifact.bytes);
  if (path != artifact.path || info.size() != artifact.bytes ||
      info.lastModified() != artifact.lastModified)
    throw Error("invalid_artifact",
                "The movie artifact changed after verified publication.");
}

static QJsonValue artifactList(Context& context, const RenderJob& job) {
  if (job.status != JobStatus::Completed)
    throw Error("artifacts_unavailable",
                "Artifacts are available only after successful finalization.",
                {{"status", statusText(job.status)}});
  QJsonArray artifacts;
  for (const Artifact& artifact : job.artifacts) {
    validateMovieArtifact(context, artifact);
    artifacts.append(artifact.metadata());
  }
  QJsonObject result = job.describe();
  result.insert("artifacts", artifacts);
  return result;
}

static QJsonValue readImageArtifact(Context& context, const RenderJob& job,
                                    const Artifact& artifact) {
  if (artifact.format != Format::Png)
    throw Error("unsupported_artifact_kind",
                "Movie artifacts are delivered as verified file metadata.");
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
  result.insert("image",
                QJsonObject{{"mimeType", "image/png"},
                            {"data", QString::fromLatin1(data.toBase64())}});
  return result;
}

static QJsonValue artifactRead(Context& context, const QString& id) {
  for (const auto& entry : renderState(context).jobs) {
    const RenderJob& job = *entry.second;
    if (job.status != JobStatus::Completed) continue;
    for (const Artifact& artifact : job.artifacts)
      if (artifact.id == id) return readImageArtifact(context, job, artifact);
  }
  throw Error("unknown_artifact",
              "The artifact is unknown or belongs to an expired worker.");
}

static QJsonObject renderCapabilities(const Context& context) {
  std::unique_ptr<Tiio::Writer> writer(Tiio::makeWriter("png"));
  const bool configured = !context.mediaToolsDirectory.isEmpty();
  const bool registered = movieWriterRegistered();
  QJsonArray profiles{"rgba8-png"};
  if (configured && registered) profiles.append("opaque8-mp4");
  const QJsonObject movie{{"media_tools_configured", configured},
                          {"writer_registered", registered},
                          {"codec_verified_per_artifact", true},
                          {"even_dimensions", true},
                          {"step", 1},
                          {"quality", 90},
                          {"scale", 100},
                          {"audio", false},
                          {"alpha", "opaque"},
                          {"matte", "black"}};
  return {{"profiles", profiles},
          {"mp4", movie},
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

static QJsonObject jobOperation(const QString& name, const QString& description,
                                QJsonObject properties, QJsonArray required) {
  QJsonObject descriptor =
      operation(name, description, properties, required, true);
  descriptor.insert("mutates_document", false);
  return descriptor;
}

static QJsonValue executeJob(Context& context, const QString& method,
                             const QJsonObject& params) {
  const auto job = renderState(context).job(requiredString(params, "job_id"));
  if (method == "render.status") return job->describe();
  if (method == "render.artifacts") return artifactList(context, *job);
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
  sequence.insert("format", QJsonObject{{"type", "string"},
                                        {"enum", QJsonArray{"png", "mp4"}},
                                        {"default", "png"}});
  sequence.insert("width", integerSchema(1, RenderDetail::kMaxDimension));
  sequence.insert("height", integerSchema(1, RenderDetail::kMaxDimension));
  return {
      neuralPreviewOperation(),
      operation("render.capabilities",
                "Inspect bounded native render profiles and configured media "
                "support.",
                {}, {}),
      jobOperation("render.preview",
                   "Start an immutable one-frame native PNG preview job.",
                   preview, {"document_id", "frame"}),
      jobOperation("render.submit",
                   "Start a bounded immutable PNG sequence or opaque MP4 job; "
                   "MP4 requires even dimensions and step 1.",
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
    return RenderDetail::renderCapabilities(context);
  if (method == "assistance.neural.preview") return RenderDetail::submitNeural(context, params);
  if (method == "render.preview")
    return RenderDetail::submit(context, params, true);
  if (method == "render.submit")
    return RenderDetail::submit(context, params, false);
  if (method == "artifact.read")
    return RenderDetail::artifactRead(context,
                                      requiredString(params, "artifact_id"));
  return RenderDetail::executeJob(context, method, params);
}

void requestRenderCancellation(Context& context) {
  Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
  if (!context.renderState) return;
  for (const auto& entry : context.renderState->jobs) entry.second->cancel();
}

void shutdownRender(Context& context) {
  requestRenderCancellation(context);
  if (!context.renderState) return;
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
