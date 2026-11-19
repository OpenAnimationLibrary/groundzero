#include "automation.h"
#include "automation_audio.h"
#include "automation_files.h"
#include "automation_fx.h"
#include "automation_jobs.h"
#include "automation_media.h"
#include "automation_neural.h"
#include "automation_ownership.h"
#include "automation_scene_host.h"
#include "automation_render_profile.h"
#include "automation_runtime.h"
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
#include "tmacrofx.h"
#include "trasterfx.h"
#include "tproperty.h"
#include "tiio.h"
#include "texception.h"
#include "tnzimage.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QStorageInfo>
#include <QJsonArray>
#include <QThread>
#include <QTimer>

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
constexpr qint64 kInlineBytes    = 4 * 1024 * 1024;
constexpr qint64 kFrameBytes     = 20 * 1024 * 1024;

static QJsonObject stringSchema() {
  return {{"type", "string"}, {"minLength", 1}};
}

static QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

static RenderProfile readProfile(const QJsonObject& params) {
  return params.contains("profile")
             ? profileFromText(requiredString(params, "profile"))
             : RenderProfile::Rgba8Png;
}

static QStringList readAccepted(const QJsonObject& params) {
  QStringList accepted;
  for (const QJsonValue& kind : params.value("accept_conversions").toArray())
    accepted.append(kind.toString());
  return accepted;
}

struct Request final {
  RenderProfile profile  = RenderProfile::Rgba8Png;
  QString pipelineSource = "profile_default";
  QStringList accepted;
  int width     = 0;
  int height    = 0;
  int start     = 0;
  int end       = 0;
  int step      = 1;
  // Explicit aspect export; the scene's stored value is never read.
  int aspectMode       = TOutputProperties::NoAspectExport;
  double aspectRatio   = 1.0;
  TPixel32 aspectColor = TPixel32::Black;

  int count() const { return (end - start) / step + 1; }
  // The written frame size: crop reduces the rendered size to the ratio.
  TDimension outputSize() const {
    TOutputProperties output;
    output.setAspectMode(aspectMode);
    output.setAspectRatio(aspectRatio);
    return output.getOutputSize(TDimension(width, height));
  }
  bool movie() const { return profileInfo(profile).movie; }
  qint64 reserveBytes() const {
    return (qint64(width) * height * profileInfo(profile).reserveBytesPerPixel +
            65536) *
           count();
  }
};

enum class Kind { Preview, Sequence };

// A render request as far as its parameters can be read; the checks decide
// whether it is admissible.
struct Plan final {
  Request request;
  Kind kind       = Kind::Sequence;
  double fitScale = 1;
  TDimension camera;
  TDimensionD cameraSize;
  TPointD dpi;
  double frameRate = 0;
  SceneOutputFacts scene;
  RenderPipeline pipeline;
  QJsonArray omissions;
  std::vector<Conversion> conversions;

  // Avoidable conversions the request did not accept.
  QStringList unaccepted() const {
    QStringList kinds;
    for (const Conversion& conversion : conversions)
      if (!conversion.accepted && !kinds.contains(conversion.kind))
        kinds.append(conversion.kind);
    return kinds;
  }

  int bound() const {
    return kind == Kind::Preview ? kPreviewDimension : kMaxDimension;
  }
  bool framesValid() const { return request.end >= request.start; }
  bool sizeValid() const {
    return request.width <= bound() && request.height <= bound();
  }
};

static void readCamera(Plan& plan, ToonzScene& scene) {
  TCamera* camera = scene.getCurrentCamera();
  plan.camera     = camera->getRes();
  plan.cameraSize = camera->getSize();
  plan.dpi        = camera->getDpi();
  plan.frameRate = scene.getProperties()->getOutputProperties()->getFrameRate();
  plan.scene      = readSceneOutputFacts(scene);
  if (plan.camera.lx <= 0 || plan.camera.ly <= 0)
    throw Error("invalid_camera",
                "The scene camera must have positive dimensions.");
}

static void readAspectExport(const QJsonObject& params, Request& request) {
  const QJsonObject aspect = params.value("aspect_export").toObject();
  const QString mode       = aspect.value("mode").toString("none");
  if (mode == "none") return;
  request.aspectMode      = mode == "crop" ? TOutputProperties::AspectCrop
                                           : TOutputProperties::AspectLetterbox;
  request.aspectRatio     = aspect.value("ratio").toDouble();
  const QJsonObject color = aspect.value("color").toObject();
  if (!color.isEmpty())
    request.aspectColor =
        TPixel32(color.value("r").toInt(), color.value("g").toInt(),
                 color.value("b").toInt(), color.value("a").toInt());
}

static QJsonObject aspectRecord(const Request& request) {
  static const char* modes[] = {"none", "letterbox", "crop"};
  const TDimension size      = request.outputSize();
  return {{"mode", modes[request.aspectMode]},
          {"ratio", request.aspectRatio},
          {"output_width", size.lx},
          {"output_height", size.ly}};
}

static Plan readPlan(const QJsonObject& params, ToonzScene& scene, Kind kind) {
  Plan plan;
  plan.kind = kind;
  readCamera(plan, scene);
  if (params.contains("width") != params.contains("height"))
    throw Error("invalid_params",
                "Specify both width and height when resizing the output.");
  if (kind == Kind::Preview && !params.contains("width"))
    plan.fitScale = std::min(1.0, double(kPreviewDimension) /
                                      std::max(plan.camera.lx, plan.camera.ly));
  Request& request = plan.request;
  request.width =
      integerParam(params, "width", 1, kMaxDimension,
                   std::max(1, int(plan.camera.lx * plan.fitScale)));
  request.height =
      integerParam(params, "height", 1, kMaxDimension,
                   std::max(1, int(plan.camera.ly * plan.fitScale)));
  readAspectExport(params, request);
  if (kind == Kind::Preview) {
    request.start = integerParam(params, "frame", 0, 1000000, 0);
    request.end   = request.start;
  } else {
    request.profile = readProfile(params);
    request.pipelineSource =
        params.value("pipeline").toString("profile_default");
    request.accepted = readAccepted(params);
    request.start    = integerParam(params, "start_frame", 0, 1000000, 0);
    request.end  = integerParam(params, "end_frame", 0, 1000000, request.start);
    request.step = integerParam(params, "step", 1, 1000000, 1);
  }
  plan.pipeline =
      resolvePipeline(request.profile, request.pipelineSource, plan.scene);
  plan.omissions =
      outputOmissions(plan.scene, request.aspectMode, request.aspectRatio);
  return plan;
}

static QJsonArray conversionRows(const Plan& plan) {
  QJsonArray rows;
  for (const Conversion& conversion : plan.conversions)
    rows.append(conversion.toJson());
  return rows;
}

static QJsonObject effectiveOutput(const Plan& plan) {
  const Request& request  = plan.request;
  const ProfileInfo& info = profileInfo(request.profile);
  QJsonObject result{
      {"width", request.width},
      {"height", request.height},
      {"fit_scale", plan.fitScale},
      {"frames", QJsonObject{{"start", request.start},
                             {"end", request.end},
                             {"step", request.step},
                             {"count", request.count()}}},
      {"frame_rate", plan.frameRate},
      {"profile", info.name},
      {"color", colorReport(request.profile, plan.pipeline, plan.scene)},
      {"conversions", conversionRows(plan)},
      {"omissions", plan.omissions},
      {"alpha", info.alpha},
      {"audio", false},
      {"camera",
       QJsonObject{{"resolution", QJsonObject{{"width", plan.camera.lx},
                                              {"height", plan.camera.ly}}},
                   {"size_inch", QJsonObject{{"width", plan.cameraSize.lx},
                                             {"height", plan.cameraSize.ly}}},
                   {"dpi", QJsonObject{{"x", plan.dpi.x}, {"y", plan.dpi.y}}}}},
      {"crop",
       QJsonObject{{"kind", "full_camera"},
                   {"camera_pixels", QJsonObject{{"x0", 0},
                                                 {"y0", 0},
                                                 {"x1", plan.camera.lx},
                                                 {"y1", plan.camera.ly}}}}}};
  // Reported only when asked for, which keeps job receipts within their
  // reserved size.
  if (request.aspectMode != TOutputProperties::NoAspectExport)
    result.insert("aspect_export", aspectRecord(request));
  return result;
}

struct Artifact final {
  RenderProfile profile = RenderProfile::Rgba8Png;
  QJsonObject movieMetadata;
  QString relativePath;
  QDateTime lastModified;
  QString id;
  QString path;
  QString sha256;
  int frame    = 0;
  int width    = 0;
  int height   = 0;
  qint64 bytes = 0;

  bool exr() const { return profileInfo(profile).linearOutput; }

  QJsonObject metadata() const {
    if (profileInfo(profile).movie) {
      QJsonObject result = movieMetadata;
      result.insert("artifact_id", id);
      result.insert("kind", "movie");
      result.insert("mime_type", "video/mp4");
      return result;
    }
    QJsonObject result{{"kind", exr() ? "exr" : "image"},
                       {"artifact_id", id},
                       {"frame", frame},
                       {"width", width},
                       {"height", height},
                       {"mime_type", exr() ? "image/x-exr" : "image/png"},
                       {"size_bytes", double(bytes)},
                       {"sha256", sha256},
                       {"alpha", profileInfo(profile).alpha}};
    if (exr()) {
      result.insert("profile", profileInfo(profile).name);
      result.insert("sample", profileInfo(profile).sample);
      result.insert("transfer", "linear");
      result.insert("workspace_relative_path", relativePath);
    }
    return result;
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
  QFile file;
  if (openApprovedFile(file, path) != OpenResult::Opened)
    throw Error("artifact_read_failed", "The rendered file cannot be opened.");
  const QByteArray data = file.read(limit + 1);
  if (file.error() != QFileDevice::NoError || data.size() != info.size())
    throw Error("artifact_read_failed",
                "The rendered file could not be read completely.");
  return data;
}

static QString sha256Hex(const QByteArray& data) {
  return QString::fromLatin1(
      QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

// An EXR frame: its header, offset table and chunk framing must describe one
// whole RGBA frame of the profile's sample type at the requested size.
static Artifact verifyExrOutput(const QString& path, int frame,
                                const Request& request, const QString& id) {
  const ProfileInfo& info = profileInfo(request.profile);
  const QByteArray data   = readOutput(path, info.frameFileLimit);
  const ExrFacts facts    = readExrFacts(data);
  const TDimension size   = request.outputSize();
  if (facts.width != size.lx || facts.height != size.ly)
    throw Error("invalid_artifact",
                "The rendered EXR dimensions differ from the request.");
  if (facts.sample != info.sample ||
      facts.channels != QStringList{"A", "B", "G", "R"} ||
      facts.compression != 3)
    throw Error("invalid_artifact",
                "The rendered EXR channels, sample type or compression "
                "differ from the profile.",
                {{"sample", facts.sample},
                 {"channels", QJsonArray::fromStringList(facts.channels)},
                 {"compression", facts.compression}});
  Artifact artifact;
  artifact.profile      = request.profile;
  artifact.id           = id;
  artifact.path         = path;
  artifact.frame        = frame;
  artifact.width        = facts.width;
  artifact.height       = facts.height;
  artifact.bytes        = data.size();
  artifact.sha256       = sha256Hex(data);
  artifact.lastModified = QFileInfo(path).lastModified();
  return artifact;
}

static Artifact verifyOutput(const QString& path, int frame,
                             const Request& request, const QString& id) {
  if (profileInfo(request.profile).linearOutput)
    return verifyExrOutput(path, frame, request, id);
  const QByteArray data = readOutput(path, kFrameBytes);
  QBuffer buffer;
  buffer.setData(data);
  if (!buffer.open(QIODevice::ReadOnly))
    throw Error("artifact_read_failed",
                "The PNG decoder buffer could not be opened.");
  QImageReader reader(&buffer, "png");
  const TDimension size = request.outputSize();
  if (reader.size() != QSize(size.lx, size.ly))
    throw Error("invalid_artifact",
                "The rendered PNG dimensions differ from the request.");
  const QImage decoded = reader.read();
  if (decoded.isNull() || !decoded.hasAlphaChannel())
    throw Error("invalid_artifact",
                "The rendered PNG cannot be decoded as an alpha image.");
  Artifact artifact;
  artifact.id     = id;
  artifact.path   = path;
  artifact.frame  = frame;
  artifact.width  = decoded.width();
  artifact.height = decoded.height();
  artifact.bytes  = data.size();
  artifact.sha256 = sha256Hex(data);
  return artifact;
}

enum class JobStatus {
  Preparing,
  Running,
  Canceling,
  Finalizing,
  Completed,
  Failed,
  Canceled,
  DeadlineExceeded
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
  case JobStatus::DeadlineExceeded:
    return "deadline_exceeded";
  }
  throw Error("internal_error", "Unknown render job state.");
}

// The contract's failure codes; any other native code is reported as one of
// these with the native message kept.
static QString failureCode(const QString& code, const QString& fallback) {
  static const QStringList codes{"render_failed",
                                 "writer_failed",
                                 "verification_failed",
                                 "render_limit",
                                 "neural_execution_changed",
                                 "revision_changed",
                                 "source_changed",
                                 "scene_host_failed",
                                 "gl_unavailable",
                                 "native_error"};
  return codes.contains(code) ? code : fallback;
}

// The native renderer's message when this process has no offscreen GL
// context for its frames.
static bool glUnavailable(const QString& message) {
  return message.contains("OpenGL is not available");
}

#ifdef BUILD_UI_PORT_TESTS
constexpr unsigned long BusyHostMs = 8000;
#endif

class RenderJob final : public Job,
                        public MovieRenderer::Listener,
                        public std::enable_shared_from_this<RenderJob> {
public:
  JobCommon common;
  RenderPipeline pipeline;
  QString directory;
  QString workspace;
  QString mediaToolsDirectory;
  QString frameError;
  Request request;
  QJsonObject assistance;
  QJsonObject effective;
  QJsonArray provenance;
  // Runs before artifacts publish; a nonempty diagnostic fails the job.
  std::function<QString()> publicationCheck;
  // The owning worker context, which outlives every job it retains.
  Context* context      = nullptr;
  bool deadlineExceeded = false;
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
  // Held while the job's output is unfinished, so a later worker can tell
  // abandoned output from a live worker's.
  OwnershipClaim ownership{true};
  QString fault;
  // Forced encoder exits counted before this job; one render runs at a time,
  // so later ones are this job's.
  int encoderExitsBefore = ffmpegForcedExits();
  qint64 stopRequestedMs = -1;
  bool stopping          = false;
  // When a poll answered off the application thread asked the job to stop.
  std::atomic<qint64> interruptMs{-1};

  QString id() const override { return common.id; }
  JobLane lane() const override { return JobLane::Render; }
  QString documentId() const override { return common.documentId; }
  qint64 reservedBytes() const override { return request.reserveBytes(); }
  QJsonValue result(Context& owner, const QJsonObject&) override;

  bool terminal() const override {
    return status == JobStatus::Completed || status == JobStatus::Failed ||
           status == JobStatus::Canceled ||
           status == JobStatus::DeadlineExceeded;
  }

  qint64 now() const { return jobRegistry(*context).elapsedMs(); }

  // Test hook: writers that stay open after a stop request until well past the
  // cancel grace period.
  bool holdingWriters() {
#ifdef BUILD_UI_PORT_TESTS
    if (fault != "hold-writers" ||
        (cancellation && now() - stopRequestedMs > CancelGraceMs + 5000))
      return false;
    QTimer::singleShot(10, QCoreApplication::instance(),
                       [self = shared_from_this()]() { self->buildFrame(); });
    return true;
#else
    return false;
#endif
  }

  void forced(const QString& process) {
    if (!common.forced.contains(process)) common.forced.append(process);
  }

  // Records the end time and every child process killed on the way there.
  void ended() {
    common.endedMs = now();
    if (ffmpegForcedExits() > encoderExitsBefore) forced("encoder");
    if (neuralExecution && neuralExecution->helperWasKilled())
      forced("neural_helper");
  }

  // Writers still open this long after a stop request mean native work does
  // not stop; the job stays non-terminal and a new worker is the recovery.
  bool stalledNow() const {
    return status == JobStatus::Canceling && !writersClosed &&
           stopRequestedMs >= 0 && now() - stopRequestedMs > CancelGraceMs;
  }

  bool requested(int frame) const {
    return frame >= request.start && frame <= request.end &&
           (frame - request.start) % request.step == 0;
  }

  void claimOutput() {
    if (!ownership.acquire(directory))
      throw Error("render_output_failed",
                  "A new private render output directory could not be "
                  "claimed.");
  }

  // Output that never completed is removed, so a canceled or failed job leaves
  // no partial files behind. A completed job keeps its claim, and so its
  // output, until the worker ends; the marker then stays unheld and a later
  // worker's sweep removes the folder.
  void releaseOutput(bool keep) {
    if (keep) return;
    ownership.release();
    try {
      removeApprovedTree(directory);
    } catch (const Error&) {
    }
  }

  bool holding(const char* point, std::function<void()> resume) {
    if (fault != point || cancellation) return false;
    QTimer::singleShot(10, QCoreApplication::instance(), std::move(resume));
    return true;
  }

  // Test hook: callbacks the native renderer would never send, before and
  // after the job ends.
  void injectStrayCallbacks(bool settled) {
    TException stray(L"Injected stray frame failure.");
    if (settled) {
      onFrameCompleted(request.start);
      onFrameFailed(request.start, stray);
      onSequenceFinished(outputPath(), MovieRenderer::CompletionStatus::Failed,
                         L"Injected late sequence failure.");
      return;
    }
    onFrameCompleted(request.end);
    onFrameCompleted(request.end + 1000);
    onFrameFailed(request.end + 7, stray);
  }

  // Test hook: what the native renderer reports when this process cannot
  // make the offscreen GL context its frames need.
  void injectGlContextFailure() {
    status = JobStatus::Running;
    TException missing(L"OpenGL is not available: no offscreen context");
    for (int frame = request.start; frame <= request.end; frame += request.step)
      onFrameFailed(frame, missing);
    onSequenceFinished(outputPath(), MovieRenderer::CompletionStatus::Failed,
                       L"OpenGL is not available: no offscreen context");
  }

  void settled() {
#ifdef BUILD_UI_PORT_TESTS
    if (fault != "stray-callbacks") return;
    const auto self = shared_from_this();
    QTimer::singleShot(30, QCoreApplication::instance(),
                       [self]() { self->injectStrayCallbacks(true); });
#endif
  }

  QJsonObject record() const override {
    const QJsonObject limits{
        {"deadline_ms", context->renderDeadlineMs},
        {"storage_reserved_bytes", double(request.reserveBytes())},
        {"cancel_grace_ms", CancelGraceMs}};
    const QJsonObject progress{{"unit", "frames"},
                               {"done", int(completed.size())},
                               {"total", request.count()}};
    const QJsonObject outputs{
        {"directory", QDir(workspace).relativeFilePath(directory)},
        {"artifact_count", int(artifacts.size())},
        {"result_rows", 0}};
    JobCommon current  = common;
    current.stalled    = stalledNow();
    QJsonObject result = commonRecord(*context, current, statusText(status),
                                      terminal(), limits, progress, outputs);
    const QJsonObject fields{{"frame_count", request.count()},
                             {"completed_frames", int(completed.size())},
                             {"failed_frames", int(failed.size())},
                             {"writers_closed", writersClosed},
                             {"width", request.width},
                             {"height", request.height},
                             {"start_frame", request.start},
                             {"end_frame", request.end},
                             {"step", request.step},
                             {"frame_rate", frameRate},
                             {"profile", profileInfo(request.profile).name},
                             {"alpha", profileInfo(request.profile).alpha},
                             {"fx_provenance", provenance},
                             {"effective", effective}};
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
      result.insert(it.key(), it.value());
    if (!assistance.isEmpty()) result.insert("assistance", assistance);
    return result;
  }

  TFilePath outputPath() const {
    const ProfileInfo& info = profileInfo(request.profile);
    const QString suffix    = info.movie
                                  ? QString("/movie.mp4")
                                  : QString("/frames/frame..") + info.extension;
    return TFilePath((directory + suffix).toStdWString());
  }

  void releaseNativeState() {
    Q_ASSERT(writersClosed);
    nativeCancel = {};
    renderer.reset();
    if (neuralExecution && !neuralExecution->helpersClosed())
      neuralExecution->closureFailed();
    snapshot.reset();
    removeSnapshotFiles();
  }

  // The snapshot is a private copy of the draft's scene and artwork that this
  // job renders from. Nothing reads it once the native scene is released:
  // artifacts are verified from the closed output files. The scene and the
  // artwork are therefore removed when the job ends, whatever the outcome and
  // whether or not the draft still exists, so a discarded draft leaves nothing
  // but its artifacts. The manifest stays, a few hundred bytes, because the job
  // record names its hash and that hash can then be checked; with the artifacts
  // it stays until the worker ends, so that job.result can still serve them.
  void removeSnapshotFiles() {
#ifdef BUILD_UI_PORT_TESTS
    // Test hook: keep the configured scene and its artwork for the native
    // batch renderer to render as the fidelity oracle.
    if (fault == "keep-oracle-scene") return;
#endif
    const QString folder = directory + "/snapshot";
    try {
      QVector<EntryInfo> entries;
      if (listApprovedDirectory(folder, entries) != ListResult::Listed) return;
      for (const EntryInfo& entry : entries) {
        if (entry.fileName() == "manifest.json") continue;
        if (entry.isDir())
          removeApprovedTree(entry.absoluteFilePath());
        else
          QFile::remove(entry.absoluteFilePath());
      }
    } catch (const Error&) {
    }
  }

  void fail(const QString& code, const QString& phase, const QString& message) {
    common.failure = QJsonObject{
        {"code", code}, {"phase", phase}, {"message", message.left(2048)}};
    artifacts.clear();
    status = JobStatus::Failed;
    ended();
    releaseNativeState();
    releaseOutput(false);
    settled();
  }

  // The terminal state of a job stopped before it published artifacts. The
  // native writers must already be closed.
  void stop() {
    artifacts.clear();
    status =
        deadlineExceeded ? JobStatus::DeadlineExceeded : JobStatus::Canceled;
    ended();
    releaseOutput(false);
    settled();
  }

  void complete() {
    status = JobStatus::Completed;
    ended();
    releaseOutput(true);
    settled();
  }

  // Stops the job as cancellation does when its wall-clock budget runs out,
  // unless it already ended or the caller canceled it first.
  void scheduleDeadline(int milliseconds) {
    const auto self = shared_from_this();
    QTimer::singleShot(milliseconds, QCoreApplication::instance(),
                       [self, milliseconds]() {
                         Q_UNUSED(milliseconds);
                         if (self->terminal() || self->cancellation) return;
                         self->deadlineExceeded = true;
                         self->requestStop();
                       });
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
      requestStop();
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
    if (holdingWriters()) return;
#ifdef BUILD_UI_PORT_TESTS
    if (holding("hold-launch",
                [self = shared_from_this()]() { self->buildFrame(); }))
      return;
#endif
    if (cancellation) {
      writersClosed = true;
      releaseNativeState();
      stop();
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
        launch();
      }
    } catch (const Error& exception) {
      writersClosed = true;
      fail(failureCode(exception.code, "render_failed"), "prepare",
           exception.message);
    } catch (const TException& exception) {
      writersClosed = true;
      fail("native_error", "prepare",
           QString::fromStdWString(exception.getMessage()));
    } catch (const std::exception& exception) {
      writersClosed = true;
      fail("native_error", "prepare", QString::fromUtf8(exception.what()));
    } catch (...) {
      writersClosed = true;
      fail("native_error", "prepare",
           "The native frame graph could not be prepared.");
    }
  }

  void launch() {
#ifdef BUILD_UI_PORT_TESTS
    if (holding("hold-dispatch",
                [self = shared_from_this()]() { self->launch(); }))
      return;
    if (fault == "stray-callbacks") injectStrayCallbacks(false);
    if (fault == "gl-context-failure") return injectGlContextFailure();
#endif
    status           = JobStatus::Running;
    common.startedMs = now();
    renderer->start();
#ifdef BUILD_UI_PORT_TESTS
    // Test hook: a scene host whose application thread stays busy while its
    // render runs, as a long decode or frame build keeps it.
    if (fault == "busy-host") QThread::msleep(BusyHostMs);
#endif
  }

  // A caller's request; a finished job records it and is unchanged.
  void cancel() override {
    if (!common.cancelRequested) {
      const qint64 asked     = interruptMs.load();
      common.cancelRequested = true;
      common.cancelMs        = asked >= 0 ? asked : now();
    }
    requestStop();
  }

  // Render threads read `cancellation` and stop at their next frame;
  // requestStop() completes the request on the application thread.
  void interrupt(qint64 requestedMs) override {
    qint64 unset = -1;
    interruptMs.compare_exchange_strong(unset, requestedMs);
    cancellation = true;
  }

  void requestStop() {
    if (terminal() || stopping) return;
    stopping           = true;
    cancellation       = true;
    const qint64 asked = interruptMs.load();
    stopRequestedMs    = asked >= 0 ? asked : now();
    status             = JobStatus::Canceling;
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
      if (self->terminal() || !self->requested(frame)) return;
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
                         if (self->terminal() || !self->requested(frame))
                           return;
                         self->failed.insert(frame);
                         self->frameError = message;
                         if (glUnavailable(message))
                           markGraphicsFailedInWorker(*self->context);
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
    if (terminal()) return;
    expireNeural();
    writersClosed = true;
    // The native terminal callback and render-end cleanup run on this thread.
    // This queued continuation runs after they return. Drop graphs before the
    // snapshot scene; artifact verification only needs the closed output files.
    releaseNativeState();
    if (cancellation || result == MovieRenderer::CompletionStatus::Canceled) {
      stop();
      return;
    }
#ifdef BUILD_UI_PORT_TESTS
    if (fault == "encoder-failure") {
      fail("writer_failed", "encode",
           "The encoder failed while finalizing the output (injected).");
      return;
    }
#endif
    if (result == MovieRenderer::CompletionStatus::Failed) {
      const QString message =
          diagnostic.isEmpty() ? "Native rendering failed." : diagnostic;
      if (glUnavailable(diagnostic) || glUnavailable(frameError))
        fail("gl_unavailable", "render", message);
      else if (request.movie())
        fail("writer_failed", "encode", message);
      else
        fail("render_failed", "render", message);
      return;
    }
    if (int(completed.size()) != request.count() || !failed.empty()) {
      fail(glUnavailable(frameError) ? "gl_unavailable" : "render_failed",
           "render",
           frameError.isEmpty()
               ? "Rendering ended without completing every requested frame."
               : frameError);
      return;
    }
#ifdef BUILD_UI_PORT_TESTS
    if (fault == "fail-after-final-frame") {
      fail("render_failed", "render",
           "The job failed right after its final frame (injected).");
      return;
    }
#endif
    status = JobStatus::Finalizing;
    publish();
  }

  void publish() {
#ifdef BUILD_UI_PORT_TESTS
    if (holding("hold-publication",
                [self = shared_from_this()]() { self->publish(); }))
      return;
#endif
    if (cancellation) {
      stop();
      return;
    }
    if (request.movie()) return verifyMovie();
    nextVerification = request.start;
    scheduleVerification();
  }

  void verifyMovie() {
    MovieVerificationConfig config;
    config.mediaToolsDirectory = mediaToolsDirectory;
    config.moviePath    = QString::fromStdWString(outputPath().getWideString());
    config.width        = request.outputSize().lx;
    config.height       = request.outputSize().ly;
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
    if (verification && verification->forcedExit()) forced("probe");
    verification.reset();
    if (cancellation) {
      stop();
      return;
    }
    if (!failure.isEmpty())
      return fail("verification_failed", "verify", failure);
    Artifact artifact;
    artifact.profile = request.profile;
    artifact.id     = jobRegistry(*context).newArtifactId();
    artifact.path   = QString::fromStdWString(outputPath().getWideString());
    artifact.lastModified = QFileInfo(artifact.path).lastModified();
    artifact.bytes        = qint64(metadata.value("size_bytes").toDouble());
    artifact.sha256       = metadata.value("sha256").toString();
    metadata.insert("workspace_relative_path",
                    QDir(workspace).relativeFilePath(artifact.path));
    metadata.insert("job_id", common.id);
    metadata.insert("document_id", common.documentId);
    metadata.insert("source_revision", common.revision);
    metadata.insert("source_frame_rate", frameRate);
    metadata.insert("start_frame", request.start);
    metadata.insert("end_frame", request.end);
    metadata.insert("step", request.step);
    artifact.movieMetadata = std::move(metadata);
    bytes                  = artifact.bytes;
    artifacts.push_back(std::move(artifact));
    complete();
  }

  void scheduleVerification() {
    const auto self = shared_from_this();
    QTimer::singleShot(0, QCoreApplication::instance(),
                       [self]() { self->verifyFrame(); });
  }

  void verifyFrame() {
    expireNeural();
    if (cancellation) {
      stop();
      return;
    }
    try {
      const TFilePath path =
          outputPath().withFrame(TFrameId(nextVerification + 1));
      Artifact artifact = verifyOutput(
          QString::fromStdWString(path.getWideString()), nextVerification,
          request, jobRegistry(*context).newArtifactId());
      artifact.relativePath = QDir(workspace).relativeFilePath(artifact.path);
      bytes += artifact.bytes;
      if (bytes > request.reserveBytes())
        throw Error("render_limit",
                    "The output exceeded the reserved storage budget.");
      checkNeuralPublication();
      artifacts.push_back(std::move(artifact));
      nextVerification += request.step;
      if (nextVerification <= request.end) {
        scheduleVerification();
        return;
      }
      const QString diagnostic =
          publicationCheck ? publicationCheck() : QString();
      if (!diagnostic.isEmpty())
        return fail("source_changed", "verify", diagnostic);
      complete();
    } catch (const Error& exception) {
      fail(failureCode(exception.code, "verification_failed"), "verify",
           exception.message);
    } catch (const std::exception& exception) {
      fail("verification_failed", "verify",
           QString::fromUtf8(exception.what()));
    } catch (...) {
      fail("verification_failed", "verify",
           "The rendered PNG could not be verified.");
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

template <class Property>
static Property* writerProperty(TPropertyGroup* properties, const char* name) {
  auto* property = properties
                       ? dynamic_cast<Property*>(properties->getProperty(name))
                       : nullptr;
  if (!property)
    throw Error("render_unavailable",
                "The native EXR writer lacks one of its fixed options.",
                {{"option", name}});
  return property;
}

// EXR frames: RGBA, the profile's sample type, scanlines, lossless ZIP, and
// linearized with the pipeline's gamma so no second adjustment happens.
static void configureExr(RenderJob& job, TOutputProperties& output) {
  TPropertyGroup* properties = output.getFileFormatProperties("exr");
  const bool half = job.request.profile == RenderProfile::RgbaHalfExr;
  writerProperty<TEnumProperty>(properties, "Bits Per Pixel")
      ->setValue(half ? L"128(RGBA)_HF" : L"128(RGBA)_F");
  writerProperty<TEnumProperty>(properties, "Compression Type")
      ->setValue(L"ZIP");
  writerProperty<TEnumProperty>(properties, "Storage Type")
      ->setValue(L"Store Image as Scanlines");
  writerProperty<TDoubleProperty>(properties, "Color Space Gamma")
      ->setValue(job.pipeline.gamma);
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
  // Renders carry no audio, so the native writer never builds a device mix.
  muteAutomationSound(*job.snapshot);
  job.snapshot->getCurrentCamera()->setRes(
      TDimension(job.request.width, job.request.height));
  TOutputProperties* output =
      job.snapshot->getProperties()->getOutputProperties();
  job.frameRate = output->getFrameRate();
  TRenderSettings settings;
  settings.m_bpp              = job.pipeline.bpp;
  settings.m_linearColorSpace = job.pipeline.linear;
  settings.m_colorSpaceGamma  = job.pipeline.gamma;
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
  output->setAspectMode(job.request.aspectMode);
  output->setAspectRatio(job.request.aspectRatio);
  output->setAspectColor(job.request.aspectColor);
  output->setMultimediaRendering(0);
  output->getBoardSettings()->setActive(false);
  output->setPath(job.outputPath());
  output->formatTemplateFId() = TFrameId(1);
  if (job.request.movie())
    configureMovie(job, *output);
  else if (profileInfo(job.request.profile).linearOutput)
    configureExr(job, *output);
  else
    configurePng(*output);
#ifdef BUILD_UI_PORT_TESTS
  if (job.fault == "keep-oracle-scene")
    job.snapshot->saveCopy(
        TFilePath((job.directory + "/snapshot/oracle.tnz").toStdWString()));
#endif
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

// Every check render.preflight reports, in the order render jobs apply them.
static const QStringList CheckIds{
    "frame_range", "dimensions",  "scene_numbering",  "fx_graph",
    "fx_extent",   "graphics",    "writer",           "media_tools",
    "encoder",     "conversions", "dependencies",     "storage",
    "lane",        "retention",   "render_admission", "source_pins"};

struct Check final {
  QString id;
  QString status = "pass";
  QString code;
  QString message;
  QJsonObject details;

  void fail(const QString& failure, const QString& text,
            QJsonObject extra = {}) {
    status  = "fail";
    code    = failure;
    message = text;
    details = std::move(extra);
  }
  void skip(const QString& state, const QString& reason) {
    status  = state;
    details = {{"reason", reason}};
  }
  void unavailable(const QString& cause, const QString& text,
                   QJsonObject extra = {}) {
    extra.insert("cause", cause);
    fail("render_unavailable", text, std::move(extra));
  }
  bool failed() const { return status == "fail"; }
  QJsonObject toJson() const {
    return {{"id", id},
            {"status", status},
            {"code",
             code.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(code)},
            {"details", details}};
  }
};

// The checks every render job passes before any job, directory or snapshot
// exists, also reported by render.preflight without starting one.
class RenderChecks final {
public:
  RenderChecks(Context& context, Document& document, Plan plan)
      : m_context(context), m_document(document), m_plan(std::move(plan)) {}

  QJsonArray provenance;

  // The plan with the conversions the conversions check found.
  const Plan& plan() const { return m_plan; }

  void run(const QStringList& ids, bool stopAtFailure) {
    for (const QString& id : ids) {
      m_checks.push_back(evaluate(id));
      if (stopAtFailure && m_checks.back().failed()) return;
    }
  }

  const Check* firstFailure() const {
    for (const Check& check : m_checks)
      if (check.failed()) return &check;
    return nullptr;
  }

  // Refuse with the first failing check's code; end_frame before
  // start_frame is an invalid parameter of the job tools.
  void requireReady() const {
    const Check* check = firstFailure();
    if (!check) return;
    QJsonObject details = check->details;
    details.insert("check", check->id);
    if (check->id == "frame_range")
      throw Error("invalid_params", check->message, details);
    throw Error(check->code, check->message, details);
  }

  QJsonObject report() const {
    QJsonArray checks;
    for (const Check& check : m_checks) checks.append(check.toJson());
    const Check* failure = firstFailure();
    const bool shaped    = passed("frame_range") && passed("dimensions");
    return {{"document_id", m_document.id},
            {"revision", m_document.revisionText()},
            {"ready", failure == nullptr},
            {"checks", checks},
            {"first_failure",
             failure ? QJsonValue(failure->id) : QJsonValue(QJsonValue::Null)},
            {"effective", shaped ? QJsonValue(effectiveOutput(m_plan))
                                 : QJsonValue(QJsonValue::Null)},
            {"probes_run", QJsonArray::fromStringList(m_probes)}};
  }

private:
  using Evaluator = void (RenderChecks::*)(Check&);

  Check evaluate(const QString& id) {
    static const QHash<QString, Evaluator> evaluators{
        {"frame_range", &RenderChecks::frameRange},
        {"dimensions", &RenderChecks::dimensions},
        {"scene_numbering", &RenderChecks::sceneNumbering},
        {"fx_graph", &RenderChecks::fxGraph},
        {"fx_extent", &RenderChecks::fxExtent},
        {"graphics", &RenderChecks::graphics},
        {"writer", &RenderChecks::writer},
        {"media_tools", &RenderChecks::mediaTools},
        {"encoder", &RenderChecks::encoder},
        {"conversions", &RenderChecks::conversions},
        {"dependencies", &RenderChecks::dependencies},
        {"storage", &RenderChecks::storage},
        {"lane", &RenderChecks::lane},
        {"retention", &RenderChecks::retention},
        {"render_admission", &RenderChecks::renderAdmission},
        {"source_pins", &RenderChecks::sourcePins}};
    Check check;
    check.id = id;
    (this->*evaluators.value(id))(check);
    return check;
  }

  bool passed(const QString& id) const {
    for (const Check& check : m_checks)
      if (check.id == id) return check.status == "pass";
    return false;
  }

  bool movie() const { return m_plan.request.movie(); }

  void frameRange(Check& check) {
    if (m_plan.framesValid()) return;
    check.fail("invalid_frame_range", "end_frame must not precede start_frame.",
               {{"reason", "end_before_start"},
                {"start_frame", m_plan.request.start},
                {"end_frame", m_plan.request.end}});
  }

  void dimensions(Check& check) {
    const Request& request = m_plan.request;
    if (!m_plan.sizeValid())
      return check.fail("render_limit",
                        "Specify width and height within the render limit.",
                        {{"reason", "dimensions"},
                         {"width", request.width},
                         {"height", request.height},
                         {"max_dimension", m_plan.bound()}});
    if (movie() && (request.step != 1 || request.outputSize().lx % 2 != 0 ||
                    request.outputSize().ly % 2 != 0))
      return check.fail("unsupported_render_settings",
                        "MP4 requires step 1 and even output width and height.",
                        {{"reason", "mp4_even_dimensions_step_1"}});
    if (!m_plan.framesValid())
      return check.skip("not_run", "frame_range_failed");
    if (request.count() > kMaxFrames || request.reserveBytes() > RenderJobBytes)
      check.fail("render_limit", "Reduce the frame count or output dimensions.",
                 {{"reason", "frames_or_reservation"},
                  {"frame_count", request.count()},
                  {"max_frames", kMaxFrames},
                  {"reserve_bytes", double(request.reserveBytes())},
                  {"max_bytes", double(RenderJobBytes)}});
  }

  void sceneNumbering(Check& check) {
    if (Preferences::instance()->isSceneNumberingEnabled())
      check.fail("unsupported_render_settings",
                 "Disable scene numbering before automated rendering.");
  }

  void fxGraph(Check& check) {
    if (m_document.readOnly)
      return check.skip("not_applicable", "read_only_render_profile");
    try {
      validateAutomationFxGraph(*m_document.scene);
    } catch (const Error& error) {
      check.fail(error.code, error.message, error.details);
    }
  }

  void fxExtent(Check& check) {
    if (!passed("fx_graph") && !m_document.readOnly)
      return check.skip("not_run", "fx_graph_failed");
    if (!m_plan.framesValid() || !m_plan.sizeValid())
      return check.skip("not_run", "request_invalid");
    const Request& request = m_plan.request;
    try {
      provenance =
          preflightAutomationRender(m_document, request.width, request.height,
                                    request.start, request.end, request.step);
    } catch (const Error& error) {
      check.fail(error.code, error.message, error.details);
    }
  }

  void graphics(Check& check) {
    if (ensureGraphicsProbe(m_context)) m_probes.append("graphics");
    if (graphicsState(m_context).usable()) return;
    const QJsonObject probe =
        graphicsReport(m_context).value("probe").toObject();
    check.unavailable("gl_unavailable",
                      "This worker has no usable OpenGL context; renders "
                      "need native OpenGL.",
                      {{"probe_result", probe.value("result")}});
  }

  void writer(Check& check) {
    if (movie()) {
      if (!mp4WriterRegistered())
        check.unavailable(
            "mp4_writer_unregistered",
            "The configured media tools did not register the native MP4 "
            "writer.");
      return;
    }
    const char* extension = profileInfo(m_plan.request.profile).extension;
    std::unique_ptr<Tiio::Writer> writer(Tiio::makeWriter(extension));
    if (!writer)
      check.unavailable(QString(extension) + "_writer_unavailable",
                        "The native image writer of this profile is "
                        "unavailable.");
  }

  void mediaTools(Check& check) {
    if (!movie()) return check.skip("not_applicable", "image_sequence");
    if (m_context.mediaToolsDirectory.isEmpty())
      return check.unavailable(
          "media_tools_not_configured",
          "Start the worker with an explicit --media-tools directory for MP4.");
    if (ensureMediaProbe(m_context)) m_probes.append("media");
    if (!mediaToolsUnchanged(m_context) ||
        m_context.runtimeState->media->failure == "media_tools_missing")
      check.unavailable("media_tools_changed",
                        "A granted media tool changed after launch; start a "
                        "new worker.");
  }

  void encoder(Check& check) {
    if (!movie()) return check.skip("not_applicable", "image_sequence");
    if (!passed("media_tools"))
      return check.skip("not_run", "media_tools_failed");
    const MediaProbe& probe = *m_context.runtimeState->media;
    if (probe.failure.isEmpty()) {
      check.details = {{"codec", probe.muxerCodec}, {"encoder", probe.encoder}};
      return;
    }
    check.unavailable(
        "codec_unavailable",
        "The configured ffmpeg offers no encoder for the mp4 muxer's default "
        "video codec.",
        {{"reason", probe.failure},
         {"codec", probe.muxerCodec.isEmpty() ? QJsonValue(QJsonValue::Null)
                                              : QJsonValue(probe.muxerCodec)}});
  }

  // Levels and palettes of an editable document render from memory; the
  // snapshot re-reads every sound source from its private copy. A read-only
  // document's loader reports the level files it could not load.
  void dependencies(Check& check) {
    QJsonArray missing;
    if (m_document.readOnly) {
      for (const ContentRow& row : readOnlyBlockers(true))
        if (missing.size() < BlockerRowLimit) missing.append(row.blockerRow());
    } else {
      missing = missingAutomationSounds(*m_document.scene);
    }
    if (missing.isEmpty()) return;
    check.fail("missing_dependency",
               "A level or sound the render needs is missing, unreadable or "
               "changed.",
               {{"dependencies", missing}, {"count", missing.size()}});
  }

  void storage(Check& check) {
    if (!m_plan.framesValid() || !m_plan.sizeValid())
      return check.skip("not_run", "request_invalid");
    const qint64 reserved =
        jobRegistry(m_context).reservedBytes(JobLane::Render) +
        m_plan.request.reserveBytes();
    if (reserved > RenderWorkerBytes)
      return check.fail(
          "render_limit",
          "This worker has reached its render output storage limit.",
          {{"reason", "worker_reservation"},
           {"reserved_bytes", double(reserved)},
           {"max_bytes", double(RenderWorkerBytes)}});
    const qint64 available =
        QStorageInfo(m_context.workspace()).bytesAvailable();
    if (available < m_plan.request.reserveBytes() + RenderFreeSpaceMargin)
      check.fail("render_limit",
                 "The workspace volume has too little free space for this "
                 "render.",
                 {{"reason", "workspace_free_space"},
                  {"available_bytes", double(available)},
                  {"reserve_bytes", double(m_plan.request.reserveBytes())},
                  {"margin_bytes", double(RenderFreeSpaceMargin)}});
  }

  void lane(Check& check) {
    const QString active = jobRegistry(m_context).active(JobLane::Render);
    if (!active.isEmpty())
      check.fail("render_busy",
                 "Wait for the active render job to finish or cancel it.",
                 {{"job_id", active}});
  }

  void retention(Check& check) {
    if (jobRegistry(m_context).retained(JobLane::Render) >= JobsPerLane)
      check.fail("render_limit",
                 "This worker has reached its render job limit.",
                 {{"max_retained_jobs", JobsPerLane}});
  }

  static bool dependencyReason(const ContentRow& row) {
    return row.reason == "missing_file" || row.reason == "level_load_failed" ||
           row.reason == "partial_level";
  }

  std::vector<ContentRow> readOnlyBlockers(bool dependencyRows) {
    std::vector<ContentRow> rows;
    for (const ContentRow& row : renderBlockers(m_document))
      if (dependencyReason(row) == dependencyRows) rows.push_back(row);
    return rows;
  }

  void renderAdmission(Check& check) {
    if (!m_document.readOnly)
      return check.skip("not_applicable", "editable_document");
    if (movie())
      return check.fail("render_unavailable",
                        "Read-only documents render PNG only.",
                        {{"reason", "read_only_png_only"}});
    const std::vector<ContentRow> blockers = readOnlyBlockers(false);
    if (blockers.empty()) return;
    QJsonArray rows;
    for (const ContentRow& row : blockers)
      if (rows.size() < BlockerRowLimit) rows.append(row.blockerRow());
    check.fail("render_blocked",
               "The scene contains content the read-only render profile "
               "does not run.",
               {{"blockers", rows}, {"blocker_count", int(blockers.size())}});
  }

  void sourcePins(Check& check) {
    if (!m_document.readOnly)
      return check.skip("not_applicable", "editable_document");
    try {
      requireUnchangedSources(m_document.readOnly->pins);
    } catch (const Error& error) {
      check.fail(error.code, error.message, error.details);
    }
  }

  static bool computesInFloat(TFx* fx) {
    auto* raster = dynamic_cast<TRasterFx*>(fx);
    return raster && raster->canComputeInFloat();
  }

  // The effect a provenance row names: a macro member, a generator column's
  // effect or the node itself.
  static TFx* evaluatedEffect(TFx* node, const QString& memberKey) {
    if (auto* macro = dynamic_cast<TMacroFx*>(node)) {
      for (const TFxP& member : macro->getFxs())
        if (QString::fromStdWString(member->getFxId()) == memberKey)
          return member.getPointer();
      return nullptr;
    }
    if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(node))
      return wrapper->getZeraryFx();
    return node;
  }

  // Whether each effect the render evaluates computes in float, as its node
  // and the effect itself both must for a float tile to pass unconverted.
  std::vector<EffectPrecision> effectPrecision() const {
    std::vector<EffectPrecision> rows;
    for (const QJsonValue& value : provenance) {
      const QJsonObject entry = value.toObject();
      EffectPrecision row;
      row.fxId      = entry.value("fx_id").toString();
      row.memberKey = entry.value("member_key").toString();
      row.type      = entry.value("type").toString();
      TFx* node     = m_document.fx(row.fxId);
      TFx* effect   = evaluatedEffect(node, row.memberKey);
      row.float32 = effect && computesInFloat(node) && computesInFloat(effect);
      rows.push_back(row);
    }
    return rows;
  }

  // Every conversion the request makes; avoidable ones it did not accept
  // refuse the job (contract w3-fidelity §4).
  void conversions(Check& check) {
    if (m_plan.pipeline.bpp == 128 && !passed("fx_extent"))
      return check.skip("not_run", "fx_extent_failed");
    m_plan.conversions = renderConversions(
        m_plan.request.profile, m_plan.pipeline, effectPrecision(),
        m_plan.omissions, m_plan.request.accepted);
    const QStringList unaccepted = m_plan.unaccepted();
    if (unaccepted.isEmpty()) return;
    check.fail("unaccepted_conversion",
               "The render makes avoidable conversions the request did not "
               "accept; list them in accept_conversions or change the profile "
               "or pipeline.",
               {{"conversions", QJsonArray::fromStringList(unaccepted)}});
  }

  Context& m_context;
  Document& m_document;
  Plan m_plan;
  std::vector<Check> m_checks;
  QStringList m_probes;
};

static QString kindText(Kind kind) {
  return kind == Kind::Preview ? "render_preview" : "render_sequence";
}

// The request as admitted: the caller's arguments with every default filled.
static QJsonObject admittedRequest(const QJsonObject& params,
                                   const Plan& plan) {
  const Request& request = plan.request;
  QJsonObject result     = params;
  result.remove("operation_id");
  result.remove("expected_revision");
  result.insert("width", request.width);
  result.insert("height", request.height);
  if (plan.kind == Kind::Preview) {
    result.insert("frame", request.start);
    return result;
  }
  result.insert("start_frame", request.start);
  result.insert("end_frame", request.end);
  result.insert("step", request.step);
  result.insert("profile", profileInfo(request.profile).name);
  result.insert("pipeline", request.pipelineSource);
  result.insert("accept_conversions",
                QJsonArray::fromStringList(request.accepted));
  return result;
}

static std::shared_ptr<RenderJob> newJob(Context& context, Document& document,
                                         const Plan& plan, const QString& kind,
                                         const QJsonObject& params) {
  JobRegistry& registry   = jobRegistry(context);
  auto job                = std::make_shared<RenderJob>();
  job->context            = &context;
  job->common.id          = registry.newJobId();
  job->common.kind        = kind;
  job->common.documentId  = document.id;
  job->common.revision    = document.revisionText();
  job->common.submittedMs = registry.elapsedMs();
  job->common.request     = admittedRequest(params, plan);
  job->request            = plan.request;
  job->pipeline           = plan.pipeline;
  job->effective          = effectiveOutput(plan);
  job->workspace          = context.workspace();
  try {
    job->directory = context.resolvePath(
        context.workspace() + "/renders/" + job->common.id, true);
  } catch (const Error&) {
    // The workspace is gone or replaced, for example by a removed volume.
    throw Error("render_output_failed",
                "The worker's private render output folder is unavailable.");
  }
  return job;
}

static void linkReadOnlySources(RenderJob& job, Document& document) {
  job.common.sourceFingerprint      = document.readOnly->fingerprint;
  const std::vector<SourcePin> pins = document.readOnly->pins;
  job.publicationCheck              = [pins]() {
    const QStringList changed = changedSourcePins(pins);
    if (changed.isEmpty()) return QString();
    return "source_changed: " + QString::number(changed.size()) +
           " pinned source file(s) changed during the render, including " +
           changed.first();
  };
}

static QJsonValue submit(Context& context, const QJsonObject& params,
                         bool preview) {
  Document& document = context.document(requiredString(params, "document_id"));
  const Plan plan    = readPlan(params, *document.scene,
                                preview ? Kind::Preview : Kind::Sequence);
  RenderChecks checks(context, document, plan);
  checks.run(CheckIds, true);
  checks.requireReady();
  auto job =
      newJob(context, document, checks.plan(), kindText(plan.kind), params);
  job->provenance          = checks.provenance;
  job->mediaToolsDirectory = context.mediaToolsDirectory;
  context.markMutationStarted();
#ifdef BUILD_UI_PORT_TESTS
  job->fault = context.renderFault;
  if (!context.sceneHostDocument.isEmpty() && context.childFault == "busy-host")
    job->fault = context.childFault;
#endif
  try {
    if (QFileInfo::exists(job->directory) ||
        !QDir().mkpath(job->directory + "/frames"))
      throw Error(
          "render_output_failed",
          "A new private render output directory could not be created.");
    job->claimOutput();
    if (document.readOnly) {
      linkReadOnlySources(*job, document);
      job->snapshot = loadReadOnlySnapshot(context, document);
    } else {
      job->snapshot =
          createRenderSnapshot(context, document, job->directory + "/snapshot",
                               &job->common.snapshotManifest);
    }
    configureSnapshot(*job);
  } catch (...) {
    job->releaseOutput(false);
    if (!QFileInfo::exists(job->directory)) context.resetMutationStarted();
    throw;
  }
  jobRegistry(context).add(job);
  job->scheduleDeadline(context.renderDeadlineMs);
  job->scheduleBuild();
  return job->record();
}

static void requirePreflightFields(const QJsonObject& params,
                                   const QString& kind) {
  const QStringList sequence{"start_frame", "end_frame",          "step",
                             "profile",     "accept_conversions", "pipeline"};
  const bool preview = kind == "preview";
  bool valid         = params.contains("frame") == preview;
  for (const QString& field : sequence)
    if (preview && params.contains(field)) valid = false;
  if (!preview &&
      (!params.contains("start_frame") || !params.contains("end_frame")))
    valid = false;
  if (!valid)
    throw Error("invalid_params",
                "kind preview takes frame only; kind sequence takes "
                "start_frame, end_frame and optional step, profile, "
                "pipeline and accept_conversions.");
}

static QJsonValue preflight(Context& context, const QJsonObject& params) {
  Document& document = context.document(requiredString(params, "document_id"));
  if (requiredString(params, "expected_revision") != document.revisionText())
    throw Error(
        "stale_revision", "The document changed; inspect it before preflight.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
  const QString kind = requiredString(params, "kind");
  requirePreflightFields(params, kind);
  RenderChecks checks(
      context, document,
      readPlan(params, *document.scene,
               kind == "preview" ? Kind::Preview : Kind::Sequence));
  checks.run(CheckIds, false);
  return checks.report();
}

static Plan neuralPlan(ToonzScene& scene, const QJsonObject& params) {
  Plan plan;
  plan.kind = Kind::Preview;
  readCamera(plan, scene);
  if (plan.camera.lx > 1024 || plan.camera.ly > 1024)
    throw Error("render_limit", "Neural preview requires a camera at most 1024 by 1024 pixels.");
  plan.request.width  = plan.camera.lx;
  plan.request.height = plan.camera.ly;
  plan.request.start  = integerParam(params, "frame", 0, 999999, -1);
  plan.request.end    = plan.request.start;
  return plan;
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
  const Plan plan = neuralPlan(*document.scene, params);
  RenderChecks checks(context, document, plan);
  checks.run({"graphics", "writer", "lane", "retention", "storage"}, true);
  checks.requireReady();
  if (Preferences::instance()->isSceneNumberingEnabled())
    throw Error("unsupported_render_settings", "Disable scene numbering before automated rendering.");
  validateAutomationScene(*document.scene);
  const QJsonArray provenance = preflightAutomationRender(
      document, plan.request.width, plan.request.height, plan.request.start,
      plan.request.end, 1);
  checkNeuralDeadline(deadline);
  const NeuralPreview preview = prepareNeuralPreview(context, document, params, deadline);
  auto job        = newJob(context, document, plan, "neural_preview", params);
  job->provenance = provenance;
  checkNeuralDeadline(deadline);
  context.markMutationStarted();
  try {
    if (QFileInfo::exists(job->directory) ||
        !QDir().mkpath(job->directory + "/frames"))
      throw Error("render_output_failed",
                  "Cannot create a private Neural preview output directory.");
    job->claimOutput();
    job->snapshot =
        createRenderSnapshot(context, document, job->directory + "/snapshot",
                             &job->common.snapshotManifest);
    checkNeuralDeadline(deadline);
    attachNeuralPreview(*job, preview);
    configureSnapshot(*job);
    checkNeuralDeadline(deadline);
  } catch (...) {
    job->releaseOutput(false);
    if (!QFileInfo::exists(job->directory)) context.resetMutationStarted();
    throw;
  }
  jobRegistry(context).add(job);
  job->observeNeuralDeadline();
  job->scheduleBuild();
  return job->record();
}

// Movies and EXR frames are served as verified file metadata, so their files
// must still be the ones verified at publication.
static void validateFileArtifact(Context& context, const Artifact& artifact) {
  if (!profileInfo(artifact.profile).movie && !artifact.exr()) return;
  const QString path = context.resolvePath(artifact.path, false);
  const QFileInfo info(path);
  validateOutputFile(info, artifact.path, artifact.bytes);
  if (path != artifact.path || info.size() != artifact.bytes ||
      info.lastModified() != artifact.lastModified)
    throw Error("invalid_artifact",
                "The artifact file changed after verified publication.");
}

static QJsonValue artifactList(Context& context, const RenderJob& job) {
  if (job.status != JobStatus::Completed)
    throw Error("artifacts_unavailable",
                "Artifacts are available only after successful finalization.",
                {{"status", statusText(job.status)}});
  QJsonArray artifacts;
  for (const Artifact& artifact : job.artifacts) {
    validateFileArtifact(context, artifact);
    artifacts.append(artifact.metadata());
  }
  return QJsonObject{{"job", job.record()}, {"artifacts", artifacts}};
}

QJsonValue RenderJob::result(Context& owner, const QJsonObject&) {
  return artifactList(owner, *this);
}

static QJsonValue readImageArtifact(Context& context, const RenderJob& job,
                                    const Artifact& artifact) {
  if (artifact.profile != RenderProfile::Rgba8Png)
    throw Error(
        "unsupported_artifact_kind",
        "Movie and EXR artifacts are served as verified file "
        "metadata by job.result, not inline.",
        {{"kind", artifact.metadata().value("kind")}, {"hint", "job.result"}});
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
  result.insert("job_id", job.common.id);
  result.insert("document_id", job.common.documentId);
  result.insert("source_revision", job.common.revision);
  result.insert("image",
                QJsonObject{{"mimeType", "image/png"},
                            {"data", QString::fromLatin1(data.toBase64())}});
  return result;
}

static QJsonValue artifactRead(Context& context, const QString& id) {
  JobRegistry& registry = jobRegistry(context);
  for (const auto& entry : registry.jobs()) {
    const auto* job = dynamic_cast<const RenderJob*>(entry.get());
    if (!job || job->status != JobStatus::Completed) continue;
    for (const Artifact& artifact : job->artifacts)
      if (artifact.id == id) return readImageArtifact(context, *job, artifact);
  }
  const bool ours =
      id.startsWith("artifact_" + registry.instance().left(8) + "_");
  throw Error("unknown_artifact", "This worker has no artifact with this id.",
              {{"reason", ours ? "not_issued" : "other_worker"},
               {"worker_instance_id", registry.instance()}});
}

static QJsonObject renderCapabilities(Context& context) {
  ensureGraphicsProbe(context);
  ensureMediaProbe(context);
  std::unique_ptr<Tiio::Writer> writer(Tiio::makeWriter("png"));
  std::unique_ptr<Tiio::Writer> exr(Tiio::makeWriter("exr"));
  QJsonArray profiles;
  if (graphicsState(context).usable()) {
    if (writer) profiles.append("rgba8-png");
    if (mediaState(context).usable()) profiles.append("opaque8-mp4");
    if (exr) profiles.append("rgba-half-exr");
    if (exr) profiles.append("rgba-float-exr");
  }
  QJsonArray table;
  for (const QString& name : profileNames()) {
    const ProfileInfo& info = profileInfo(profileFromText(name));
    table.append(
        QJsonObject{{"profile", info.name},
                    {"pipeline_bpp", info.defaultBpp},
                    {"sample", info.sample},
                    {"alpha", info.alpha},
                    {"transfer", info.linearOutput ? "linear" : "power"},
                    {"reserve_bytes_per_pixel", info.reserveBytesPerPixel},
                    {"frame_file_bytes", double(info.frameFileLimit)},
                    {"inline_read", name == "rgba8-png"}});
  }
  QJsonObject movie = mp4Report(context);
  movie.insert("media_tools_configured",
               !context.mediaToolsDirectory.isEmpty());
  movie.insert("codec_verified_per_artifact", true);
  movie.insert("even_dimensions", true);
  movie.insert("step", 1);
  movie.insert("quality", 90);
  movie.insert("scale", 100);
  movie.insert("audio", false);
  movie.insert("alpha", "opaque");
  movie.insert("matte", "black");
  return {{"profiles", profiles},
          {"profile_table", table},
          {"avoidable_conversions",
           QJsonArray::fromStringList(avoidableConversionKinds())},
          {"mp4", movie},
          {"png_writer_available", bool(writer)},
          {"graphics", graphicsReport(context)},
          {"max_frames", kMaxFrames},
          {"max_dimension", kMaxDimension},
          {"max_preview_dimension", kPreviewDimension},
          {"max_active_jobs", 1},
          {"max_retained_jobs", JobsPerLane},
          {"inline_image_bytes", double(kInlineBytes)},
          {"storage_budget_bytes", double(RenderWorkerBytes)},
          {"job_storage_bytes", double(RenderJobBytes)},
          {"free_space_margin_bytes", double(RenderFreeSpaceMargin)},
          {"frame_origin", 0},
          {"snapshot_inputs", "qualified-collected-native-scene"},
          {"read_only_snapshot_inputs", linkedSnapshotInputs()},
          {"job_lifetime", "worker-session"}};
}

static QJsonObject jobOperation(const QString& name, const QString& description,
                                QJsonObject properties, QJsonArray required) {
  return operation(name, description, properties, required, true);
}

}  // namespace RenderDetail

// An explicit aspect export for one render: none, or letterbox or crop to a
// ratio with an optional fill color.
static QJsonObject aspectExportSchema() {
  const auto channel = [] { return RenderDetail::integerSchema(0, 255); };
  const QJsonObject color{{"type", "object"},
                          {"additionalProperties", false},
                          {"properties", QJsonObject{{"r", channel()},
                                                     {"g", channel()},
                                                     {"b", channel()},
                                                     {"a", channel()}}},
                          {"required", QJsonArray{"r", "g", "b", "a"}}};
  const QJsonObject none{
      {"type", "object"},
      {"additionalProperties", false},
      {"properties", QJsonObject{{"mode", QJsonObject{{"const", "none"}}}}},
      {"required", QJsonArray{"mode"}}};
  const QJsonObject fitted{
      {"type", "object"},
      {"additionalProperties", false},
      {"properties",
       QJsonObject{
           {"mode", QJsonObject{{"type", "string"},
                                {"enum", QJsonArray{"letterbox", "crop"}}}},
           {"ratio",
            QJsonObject{{"type", "number"}, {"minimum", 0.1}, {"maximum", 10}}},
           {"color", color}}},
      {"required", QJsonArray{"mode", "ratio"}}};
  return {{"oneOf", QJsonArray{none, fitted}}};
}

// profile, pipeline and accept_conversions of render.submit and
// render.preflight.
static std::vector<std::pair<QString, QJsonObject>> fidelitySchemas() {
  return {
      {"profile",
       QJsonObject{{"type", "string"},
                   {"enum", QJsonArray::fromStringList(profileNames())},
                   {"default", "rgba8-png"}}},
      {"pipeline", QJsonObject{{"type", "string"},
                               {"enum", QJsonArray{"profile_default", "scene"}},
                               {"default", "profile_default"}}},
      {"accept_conversions",
       QJsonObject{
           {"type", "array"},
           {"items", QJsonObject{{"type", "string"},
                                 {"enum", QJsonArray::fromStringList(
                                              avoidableConversionKinds())}}},
           {"minItems", 1},
           {"maxItems", 5}}}};
}

QJsonArray renderOperations() {
  using RenderDetail::integerSchema;
  using RenderDetail::jobOperation;
  using RenderDetail::stringSchema;
  const QJsonObject document{{"document_id", stringSchema()}};
  QJsonObject preview = document;
  preview.insert("frame", integerSchema(0, 1000000));
  preview.insert("width", integerSchema(1, RenderDetail::kPreviewDimension));
  preview.insert("height", integerSchema(1, RenderDetail::kPreviewDimension));
  preview.insert("aspect_export", aspectExportSchema());
  QJsonObject sequence = document;
  sequence.insert("start_frame", integerSchema(0, 1000000));
  sequence.insert("end_frame", integerSchema(0, 1000000));
  sequence.insert("step", integerSchema(1, 1000000));
  for (const auto& [name, schema] : fidelitySchemas())
    sequence.insert(name, schema);
  sequence.insert("width", integerSchema(1, RenderDetail::kMaxDimension));
  sequence.insert("height", integerSchema(1, RenderDetail::kMaxDimension));
  sequence.insert("aspect_export", aspectExportSchema());
  QJsonObject check{
      {"document_id",
       QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}};
  check.insert(
      "expected_revision",
      QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 80}});
  check.insert("kind",
               QJsonObject{{"type", "string"},
                           {"enum", QJsonArray{"preview", "sequence"}}});
  check.insert("frame", integerSchema(0, 1000000));
  check.insert("start_frame", integerSchema(0, 1000000));
  check.insert("end_frame", integerSchema(0, 1000000));
  check.insert("step", integerSchema(1, 1000000));
  for (const auto& [name, schema] : fidelitySchemas())
    check.insert(name, schema);
  check.insert("width", integerSchema(1, RenderDetail::kMaxDimension));
  check.insert("height", integerSchema(1, RenderDetail::kMaxDimension));
  check.insert("aspect_export", aspectExportSchema());
  return {
      neuralPreviewOperation(),
      operation("render.capabilities",
                "Inspect bounded native render profiles, the graphics probe "
                "and configured media support; runs the graphics and media "
                "probes once per worker.",
                {}, {}),
      operation("render.preflight",
                "Run every check that render.preview or render.submit runs "
                "before a job exists, without starting one, and report the "
                "effective output: size, fit, frames, rate, profile, alpha, "
                "color pipeline and output, every conversion and every scene "
                "output setting the render does not apply, camera DPI and "
                "crop.",
                check, {"document_id", "expected_revision", "kind"}),
      jobOperation("render.preview",
                   "Start an immutable one-frame native PNG preview job.",
                   preview, {"document_id", "frame"}),
      jobOperation("render.submit",
                   "Start a bounded immutable image-sequence or movie job in "
                   "an output profile (8-bit PNG, opaque MP4, half or float "
                   "EXR); avoidable precision or setting losses must be "
                   "accepted by name. MP4 requires even dimensions and step "
                   "1.",
                   sequence, {"document_id", "start_frame", "end_frame"}),
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
  if (method == "render.preflight")
    return RenderDetail::preflight(context, params);
  if (method == "assistance.neural.preview") return RenderDetail::submitNeural(context, params);
  if (method == "render.preview")
    return RenderDetail::submit(context, params, true);
  if (method == "render.submit")
    return RenderDetail::submit(context, params, false);
  if (method == "artifact.read")
    return RenderDetail::artifactRead(context,
                                      requiredString(params, "artifact_id"));
  throw Error("unknown_operation", "Unknown native render operation.");
}

int renderJobCount(Context& context) {
  return context.jobs ? context.jobs->retained(JobLane::Render) : 0;
}

QString activeRenderJob(Context& context) {
  return context.jobs ? context.jobs->active(JobLane::Render) : QString();
}

void sweepAbandonedRenders(Context& context) {
  OwnershipClaim::removeAbandoned(context.workspace() + "/renders", {"job_*"});
}

}  // namespace GroundzeroAutomation
