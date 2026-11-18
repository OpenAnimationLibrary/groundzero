#include "automation_runtime.h"
#include "automation_shared_parse.h"

#include "tenv.h"
#include "tiio.h"
#include "tlevel_io.h"
#include "tsound_io.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QProcess>
#include <QRegularExpression>
#include <QSysInfo>
#include <QUuid>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace GroundzeroAutomation {
namespace {

constexpr int MediaDeadlineMs      = 10000;
constexpr qint64 MediaOutputBytes  = 1024 * 1024;
constexpr qint64 ResponseBytes     = 256 * 1024;
constexpr int MaxNativeFormats     = 128;
constexpr int MaxSoundFormats      = 64;
constexpr int MaxLibraryVersions   = 16;
constexpr int GraphicsProbeSize    = 16;
constexpr int MaxGraphicsElapsedMs = 10000;
const QStringList GraphicsResults{
    "context_ready", "no_context", "no_framebuffer", "readback_mismatch",
    "crashed",       "timeout",    "memory"};

QJsonValue optionalBool(const std::optional<bool>& value) {
  return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}

QJsonValue optionalText(const QString& value) {
  return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

RuntimeState& runtimeState(Context& context) {
  if (!context.runtimeState)
    context.runtimeState = std::make_shared<RuntimeState>();
  return *context.runtimeState;
}

QString executableName(const QString& name) {
#ifdef _WIN32
  return name + ".exe";
#else
  return name;
#endif
}

}  // namespace

bool CapabilityState::usable() const {
  return compiled && installed.value_or(true) && configured.value_or(true) &&
         loaded.value_or(true) &&
         (verified == "passed" || verified == "not_applicable");
}

QJsonObject CapabilityState::toJson() const {
  QJsonArray codes;
  for (const QString& reason : reasons.mid(0, 16)) codes.append(reason);
  return {{"compiled", compiled},
          {"installed", optionalBool(installed)},
          {"configured", optionalBool(configured)},
          {"loaded", optionalBool(loaded)},
          {"verified", verified},
          {"usable", usable()},
          {"activation", QJsonObject{{"kind", activationKind},
                                     {"name", optionalText(activationName)}}},
          {"reasons", codes}};
}

QJsonObject GraphicsProbe::toJson() const {
  return {{"result", result},
          {"vendor", vendor.left(256)},
          {"renderer", renderer.left(256)},
          {"version", version.left(256)},
          {"renderer_class", classifyRenderer(vendor, renderer)},
          {"elapsed_ms",
           double(std::clamp<qint64>(elapsedMs, 0, MaxGraphicsElapsedMs))}};
}

QJsonObject MediaFile::toJson() const {
  return {{"file", file},
          {"size_bytes", double(size)},
          {"sha256", sha256},
          {"version_line", versionLine.left(256)},
          {"library_versions", libraries}};
}

VersionReport parseVersionOutput(const QByteArray& output) {
  static const QRegularExpression library(
      "^\\s*(lib[a-z0-9_]{1,29})\\s+([0-9]+)\\.\\s*([0-9]+)\\.\\s*([0-9]+)");
  VersionReport report;
  const QStringList lines = QString::fromUtf8(output).split('\n');
  if (!lines.isEmpty()) report.line = lines.first().trimmed().left(256);
  for (const QString& line : lines) {
    const auto match = library.match(line);
    if (!match.hasMatch() || report.libraries.size() >= MaxLibraryVersions)
      continue;
    report.libraries.append(
        QJsonObject{{"name", match.captured(1)},
                    {"version", match.captured(2) + "." + match.captured(3) +
                                    "." + match.captured(4)}});
  }
  return report;
}

QString parseMuxerDefaultVideoCodec(const QByteArray& output) {
  static const QRegularExpression codec(
      "^\\s*Default video codec:\\s*([A-Za-z0-9_]{1,32})\\.?\\s*$",
      QRegularExpression::MultilineOption);
  const auto match = codec.match(QString::fromUtf8(output));
  return match.hasMatch() ? match.captured(1) : QString();
}

QString parseEncoder(const QByteArray& output, const QString& codec) {
  static const QRegularExpression row(
      "^ V([.F])([.S])([.X])[.B][.D] ([A-Za-z0-9_-]{1,64}) +(.*)$");
  static const QRegularExpression named(
      "\\(codec ([A-Za-z0-9_]{1,32})\\)\\s*$");
  if (codec.isEmpty()) return {};
  for (const QString& line : QString::fromUtf8(output).split('\n')) {
    const auto match = row.match(QString(line).remove('\r'));
    if (!match.hasMatch() || match.captured(3) == "X") continue;
    const QString encoder = match.captured(4);
    const auto declared   = named.match(match.captured(5));
    const QString rowCodec =
        declared.hasMatch() ? declared.captured(1) : encoder;
    if (rowCodec == codec) return encoder;
  }
  return {};
}

QString classifyRenderer(const QString& vendor, const QString& renderer) {
  for (const char* marker :
       {"llvmpipe", "softpipe", "SwiftShader", "Software", "GDI Generic"})
    if (renderer.contains(QLatin1String(marker), Qt::CaseInsensitive))
      return "software";
  for (const char* known :
       {"Apple", "NVIDIA", "AMD", "ATI Technologies", "Intel", "Qualcomm",
        "ARM", "Imagination", "Broadcom"})
    if (vendor.contains(QLatin1String(known))) return "hardware";
  return "unknown";
}

bool mp4WriterRegistered() {
  QStringList formats;
  TLevelWriter::getSupportedFormats(formats, true);
  return formats.contains("mp4");
}

bool ensureGraphicsProbe(Context& context, bool force) {
  RuntimeState& state = runtimeState(context);
  if (state.graphics && !force) return false;
  const HelperProbeRun run = runProbeHelper(context, "gl_probe");
  GraphicsProbe probe;
  probe.elapsedMs = run.elapsedMs;
  probe.result =
      run.cause.isEmpty() ? run.reply.value("result").toString() : run.cause;
  if (!GraphicsResults.contains(probe.result)) probe.result = "crashed";
  probe.vendor                 = run.reply.value("vendor").toString();
  probe.renderer               = run.reply.value("renderer").toString();
  probe.version                = run.reply.value("version").toString();
  state.graphics               = probe;
  state.graphicsFailedInWorker = false;
  return true;
}

void markGraphicsFailedInWorker(Context& context) {
  runtimeState(context).graphicsFailedInWorker = true;
}

CapabilityState graphicsState(Context& context) {
  const RuntimeState& state = runtimeState(context);
  CapabilityState capability;
  if (!state.graphics) {
    capability.reasons = QStringList{"probe_not_run"};
    return capability;
  }
  if (state.graphicsFailedInWorker) {
    capability.verified = "failed";
    capability.reasons =
        QStringList{"gl_unavailable", "render_failed_without_gl"};
  } else if (state.graphics->result == "context_ready") {
    capability.verified = "passed";
  } else {
    capability.verified = "failed";
    capability.reasons  = QStringList{"gl_unavailable", state.graphics->result};
  }
  return capability;
}

QJsonObject graphicsReport(Context& context) {
  const RuntimeState& state = runtimeState(context);
  return {{"state", graphicsState(context).toJson()},
          {"qpa_platform", QGuiApplication::platformName().left(32)},
          {"probe", state.graphics ? QJsonValue(state.graphics->toJson())
                                   : QJsonValue(QJsonValue::Null)},
          {"required_by", "every_render"}};
}

namespace {

struct CommandRun final {
  QByteArray output;
  QString failure;
};

CommandRun runMediaCommand(const QString& program,
                           const QStringList& arguments) {
  QProcess process;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.remove("GROUNDZERO_AUTOMATION_CONTROL_FD");
  process.setProcessEnvironment(environment);
  process.setProgram(program);
  process.setArguments(arguments);
  process.setProcessChannelMode(QProcess::SeparateChannels);
  process.setStandardInputFile(QProcess::nullDevice());
  process.setStandardErrorFile(QProcess::nullDevice());
  QElapsedTimer timer;
  timer.start();
  process.start();
  CommandRun run;
  if (!process.waitForStarted(MediaDeadlineMs)) {
    run.failure = "probe_failed";
    return run;
  }
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(20);
    run.output += process.readAllStandardOutput();
    if (run.output.size() > MediaOutputBytes)
      run.failure = "probe_output_limit";
    else if (timer.elapsed() > MediaDeadlineMs)
      run.failure = "probe_timeout";
    if (run.failure.isEmpty()) continue;
    process.kill();
    process.waitForFinished(3000);
    return run;
  }
  run.output += process.readAllStandardOutput();
  if (run.output.size() > MediaOutputBytes)
    run.failure = "probe_output_limit";
  else if (process.exitStatus() != QProcess::NormalExit ||
           process.exitCode() != 0)
    run.failure = "probe_failed";
  return run;
}

struct FileStat final {
  bool exists   = false;
  qint64 size   = 0;
  quint64 inode = 0;
  qint64 mtime  = 0;
};

FileStat statFile(const QString& path) {
  FileStat result;
#ifdef _WIN32
  const QFileInfo info(path);
  result.exists = info.isFile();
  result.size   = info.size();
  result.mtime  = info.lastModified().toMSecsSinceEpoch();
#else
  struct stat status{};
  if (::stat(QFile::encodeName(path).constData(), &status) != 0) return result;
  result.exists = S_ISREG(status.st_mode);
  result.size   = qint64(status.st_size);
  result.inode  = quint64(status.st_ino);
#ifdef __APPLE__
  result.mtime = qint64(status.st_mtimespec.tv_sec) * 1000 +
                 status.st_mtimespec.tv_nsec / 1000000;
#else
  result.mtime =
      qint64(status.st_mtim.tv_sec) * 1000 + status.st_mtim.tv_nsec / 1000000;
#endif
#endif
  return result;
}

QString hashFile(const QString& path) {
  QFile file(path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
  return QString::fromLatin1(hash.result().toHex());
}

MediaFile identify(const QString& directory, const QString& name) {
  const QString path    = QDir(directory).filePath(executableName(name));
  const FileStat status = statFile(path);
  MediaFile file;
  file.file    = executableName(name);
  file.size    = status.size;
  file.inode   = status.inode;
  file.mtimeMs = status.mtime;
  file.sha256  = status.exists ? hashFile(path) : QString();
  return file;
}

QString probeVersion(MediaFile& file, const QString& directory) {
  const CommandRun run = runMediaCommand(QDir(directory).filePath(file.file),
                                         {"-hide_banner", "-version"});
  const VersionReport report = parseVersionOutput(run.output);
  file.versionLine           = report.line;
  file.libraries             = report.libraries;
  if (!run.failure.isEmpty()) return run.failure;
  return report.line.isEmpty() ? "probe_failed" : QString();
}

QString probeEncoder(MediaProbe& probe, const QString& directory) {
  const QString ffmpeg = QDir(directory).filePath(probe.ffmpeg.file);
  const CommandRun muxer =
      runMediaCommand(ffmpeg, {"-hide_banner", "-h", "muxer=mp4"});
  if (!muxer.failure.isEmpty()) return muxer.failure;
  probe.muxerCodec = parseMuxerDefaultVideoCodec(muxer.output);
  if (probe.muxerCodec.isEmpty()) return "probe_failed";
  const CommandRun encoders =
      runMediaCommand(ffmpeg, {"-hide_banner", "-encoders"});
  if (!encoders.failure.isEmpty()) return encoders.failure;
  probe.encoder = parseEncoder(encoders.output, probe.muxerCodec);
  return probe.encoder.isEmpty() ? "codec_unavailable" : QString();
}

QString firstFailure(const QStringList& failures) {
  for (const QString& failure : failures)
    if (!failure.isEmpty()) return failure;
  return {};
}

}  // namespace

bool ensureMediaProbe(Context& context, bool force) {
  RuntimeState& state     = runtimeState(context);
  const QString directory = context.mediaToolsDirectory;
  if (directory.isEmpty() || (state.media && !force)) return false;
  QElapsedTimer timer;
  timer.start();
  MediaProbe probe;
  probe.ffmpeg  = identify(directory, "ffmpeg");
  probe.ffprobe = identify(directory, "ffprobe");
  const QString missing =
      probe.ffmpeg.sha256.isEmpty() || probe.ffprobe.sha256.isEmpty()
          ? QString("media_tools_missing")
          : QString();
  const QString ffmpeg =
      missing.isEmpty() ? probeVersion(probe.ffmpeg, directory) : QString();
  const QString ffprobe =
      missing.isEmpty() ? probeVersion(probe.ffprobe, directory) : QString();
  const QString encoder = missing.isEmpty() && ffmpeg.isEmpty()
                              ? probeEncoder(probe, directory)
                              : QString();
  probe.failure         = firstFailure({missing, ffmpeg, ffprobe, encoder});
  probe.elapsedMs       = timer.elapsed();
  state.media           = probe;
  state.mediaChanged    = false;
  return true;
}

bool mediaToolsUnchanged(Context& context) {
  RuntimeState& state = runtimeState(context);
  if (!state.media || state.mediaChanged) return !state.mediaChanged;
  const QString directory = context.mediaToolsDirectory;
  for (MediaFile* file : {&state.media->ffmpeg, &state.media->ffprobe}) {
    const QString path    = QDir(directory).filePath(file->file);
    const FileStat status = statFile(path);
    if (status.exists && status.size == file->size &&
        status.inode == file->inode && status.mtime == file->mtimeMs)
      continue;
    if (!status.exists || hashFile(path) != file->sha256) {
      state.mediaChanged = true;
      return false;
    }
    file->inode   = status.inode;
    file->mtimeMs = status.mtime;
  }
  return true;
}

CapabilityState mediaState(Context& context) {
  const RuntimeState& state = runtimeState(context);
  CapabilityState capability;
  capability.activationKind = "launch_option";
  capability.activationName = "--media-tools";
  const bool configured     = !context.mediaToolsDirectory.isEmpty();
  capability.configured     = configured;
  capability.installed      = configured;
  capability.loaded         = mp4WriterRegistered();
  if (!configured) capability.reasons.append("not_configured");
  if (!*capability.loaded) capability.reasons.append("mp4_writer_unregistered");
  if (!configured) return capability;
  if (!state.media) {
    capability.reasons.append("probe_not_run");
    return capability;
  }
  capability.installed  = !state.media->ffmpeg.sha256.isEmpty() &&
                          !state.media->ffprobe.sha256.isEmpty();
  const QString failure = state.mediaChanged ? QString("media_tools_changed")
                                             : state.media->failure;
  capability.verified   = failure.isEmpty() ? "passed" : "failed";
  if (!failure.isEmpty()) capability.reasons.append(failure);
  return capability;
}

QJsonObject mp4Report(Context& context) {
  const RuntimeState& state = runtimeState(context);
  const bool probed         = bool(state.media);
  return {
      {"writer_registered", mp4WriterRegistered()},
      {"muxer_default_video_codec", probed
                                        ? optionalText(state.media->muxerCodec)
                                        : QJsonValue(QJsonValue::Null)},
      {"encoder", probed ? optionalText(state.media->encoder)
                         : QJsonValue(QJsonValue::Null)},
      {"encoder_available", probed && !state.media->muxerCodec.isEmpty()
                                ? QJsonValue(state.media->encoderAvailable())
                                : QJsonValue(QJsonValue::Null)}};
}

namespace {

QJsonObject mediaReport(Context& context) {
  const RuntimeState& state = runtimeState(context);
  const bool probed         = bool(state.media);
  return {{"state", mediaState(context).toJson()},
          {"ffmpeg", probed ? QJsonValue(state.media->ffmpeg.toJson())
                            : QJsonValue(QJsonValue::Null)},
          {"ffprobe", probed ? QJsonValue(state.media->ffprobe.toJson())
                             : QJsonValue(QJsonValue::Null)},
          {"mp4", mp4Report(context)},
          {"probed_ms", probed ? QJsonValue(double(state.media->elapsedMs))
                               : QJsonValue(QJsonValue::Null)}};
}

QJsonArray formatList(QStringList names, int limit) {
  for (QString& name : names) name = name.toLower();
  names.removeDuplicates();
  names.sort();
  QJsonArray result;
  for (const QString& name : names)
    if (!name.isEmpty() && name.size() <= 16 && result.size() < limit)
      result.append(name);
  return result;
}

QJsonObject nativeFormats() {
  QStringList imageRead, imageWrite, levelRead, levelWrite, soundRead,
      soundWrite;
  Tiio::Reader::getSupportedFormats(imageRead);
  Tiio::Writer::getSupportedFormats(imageWrite, true);
  TLevelReader::getSupportedFormats(levelRead);
  TLevelWriter::getSupportedFormats(levelWrite, false);
  TSoundTrackReader::getSupportedFormats(soundRead);
  TSoundTrackWriter::getSupportedFormats(soundWrite);
  return {{"image_read", formatList(imageRead, MaxNativeFormats)},
          {"image_write", formatList(imageWrite, MaxNativeFormats)},
          {"level_read", formatList(levelRead, MaxNativeFormats)},
          {"level_write", formatList(levelWrite, MaxNativeFormats)},
          {"sound_read", formatList(soundRead, MaxSoundFormats)},
          {"sound_write", formatList(soundWrite, MaxSoundFormats)}};
}

QJsonObject admission(const QString& tool, const QString& direction,
                      const QStringList& formats) {
  return {{"tool", tool},
          {"direction", direction},
          {"formats", QJsonArray::fromStringList(formats)}};
}

QJsonArray admittedFormats(Context& context) {
  QStringList rendered{"png"};
  if (mediaState(context).usable()) rendered.append("mp4");
  return {admission("level.import", "read", {"png"}),
          admission("raster.import_png", "read", {"png"}),
          admission("raster.export_png", "write", {"png"}),
          admission("audio.import", "read", {"wav", "aiff"}),
          admission("audio.mixdown", "write", {"wav"}),
          admission("lipsync.import", "read", {"dat", "tsv", "json"}),
          admission("studio_palette.inspect", "read", {"tpl"}),
          admission("library.inspect", "read", {"tnz", "tposes"}),
          admission("scene.open", "read", {"worker-package"}),
          admission("scene.save_copy", "write", {"worker-package"}),
          admission("render.preview", "render", {"png"}),
          admission("render.submit", "render", rendered)};
}

QJsonObject fontsReport() {
  const int families = QFontDatabase().families().size();
  CapabilityState capability;
  capability.installed = families > 0;
  capability.verified  = "not_applicable";
  if (families == 0) capability.reasons = QStringList{"no_font_families"};
  return {{"state", capability.toJson()},
          {"families", std::min(families, 65536)}};
}

QJsonArray toolsetsReport(const Context& context) {
  QJsonArray result;
  for (const QString& name : context.toolsets) {
    CapabilityState capability;
    capability.configured = true;
    capability.verified   = "not_applicable";
    if (name != "core") {
      capability.activationKind = "toolset";
      capability.activationName = name;
    }
    result.append(QJsonObject{{"name", name}, {"state", capability.toJson()}});
  }
  return result;
}

// Whether this process runs under binary translation (Rosetta).
bool translatedProcess() {
#ifdef __APPLE__
  int translated = 0;
  size_t size    = sizeof(translated);
  return sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr,
                      0) == 0 &&
         translated == 1;
#else
  return false;
#endif
}

// The machine's native architecture; a translated process's uname reports
// the emulated one.
QString runningArchitecture() {
  return translatedProcess() ? QString("arm64")
                             : QSysInfo::currentCpuArchitecture();
}

QString operatingSystem() {
#if defined(Q_OS_MACOS)
  return "macos";
#elif defined(Q_OS_WIN)
  return "windows";
#elif defined(Q_OS_LINUX)
  return "linux";
#else
  return "other";
#endif
}

QString boundedText(const QString& value, int limit) {
  const QString text = value.trimmed().left(limit);
  return text.isEmpty() ? QString("unknown") : text;
}

QString executableDigest() {
  static const QString digest =
      hashFile(QCoreApplication::applicationFilePath());
  return digest;
}

QJsonObject workerIdentity() {
  static const QString instance =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  return {{"instance_id", instance},
          {"build",
           QJsonObject{
               {"application", "groundzero"},
               {"version",
                boundedText(
                    QString::fromStdString(TEnv::getApplicationVersion()), 64)},
               {"executable_sha256", executableDigest()}}}};
}

QStringList requestedProbes(const QJsonObject& params) {
  QStringList probes;
  for (const QJsonValue& value : params.value("probes").toArray())
    if (!probes.contains(value.toString())) probes.append(value.toString());
  return probes;
}

QJsonValue inspect(Context& context, const QJsonObject& params) {
  const QStringList probes = requestedProbes(params);
  if (probes.contains("graphics")) ensureGraphicsProbe(context, true);
  if (probes.contains("media")) ensureMediaProbe(context, true);
  const QJsonObject result{
      {"worker", workerIdentity()},
      {"platform", runtimePlatform()},
      {"graphics", graphicsReport(context)},
      {"media", mediaReport(context)},
      {"formats", QJsonObject{{"native", nativeFormats()},
                              {"admitted", admittedFormats(context)}}},
      {"plugins",
       QJsonObject{
           {"policy", "builtin_only"}, {"plugin_fx", 0}, {"shader_fx", 0}}},
      {"stuff", QJsonObject{{"kind", "private_runtime"}, {"library", "empty"}}},
      {"fonts", fontsReport()},
      {"components", QJsonObject{{"see", "assistance.inspect"}}},
      {"toolsets", toolsetsReport(context)}};
  if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() >
      ResponseBytes)
    throw Error("response_too_large",
                "The runtime report exceeds its 256 KiB bound.");
  return result;
}

bool readbackMatches(const std::vector<unsigned char>& pixels) {
  for (size_t index = 0; index < pixels.size(); index += 4) {
    const int expected[] = {64, 128, 191, 255};
    for (int channel = 0; channel < 4; ++channel)
      if (std::abs(int(pixels[index + channel]) - expected[channel]) > 1)
        return false;
  }
  return true;
}

QString glString(QOpenGLFunctions* functions, unsigned int name) {
  const auto* value = functions->glGetString(name);
  return value ? QString::fromLatin1(reinterpret_cast<const char*>(value))
                     .left(256)
               : QString();
}

QString clearAndReadBack(QOpenGLFunctions* functions) {
  QOpenGLFramebufferObjectFormat format;
  format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
  QOpenGLFramebufferObject framebuffer(GraphicsProbeSize, GraphicsProbeSize,
                                       format);
  if (!framebuffer.isValid() || !framebuffer.bind()) return "no_framebuffer";
  functions->glViewport(0, 0, GraphicsProbeSize, GraphicsProbeSize);
  functions->glClearColor(64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f);
  functions->glClear(GL_COLOR_BUFFER_BIT);
  std::vector<unsigned char> pixels(GraphicsProbeSize * GraphicsProbeSize * 4);
  functions->glReadPixels(0, 0, GraphicsProbeSize, GraphicsProbeSize, GL_RGBA,
                          GL_UNSIGNED_BYTE, pixels.data());
  framebuffer.release();
  return readbackMatches(pixels) ? "context_ready" : "readback_mismatch";
}

}  // namespace

QJsonObject runtimePlatform() {
  return {
      {"os", operatingSystem()},
      {"os_version", boundedText(QSysInfo::productVersion(), 64)},
      {"cpu_architecture", boundedText(runningArchitecture(), 32)},
      {"translated", translatedProcess()},
      {"build_cpu_architecture",
       boundedText(QSysInfo::buildCpuArchitecture(), 32)},
      {"qt_runtime_version", boundedText(QString::fromLatin1(qVersion()), 32)},
      {"qt_build_version", boundedText(QStringLiteral(QT_VERSION_STR), 32)}};
}

QJsonObject graphicsProbeReply() {
  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setProfile(QSurfaceFormat::CompatibilityProfile);
  format.setVersion(2, 1);
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  QOffscreenSurface surface;
  surface.setFormat(format);
  surface.create();
  QOpenGLContext context;
  context.setFormat(format);
  if (!surface.isValid() || !context.create() || !context.makeCurrent(&surface))
    return {{"result", "no_context"}};
  QOpenGLFunctions* functions = context.functions();
  const QJsonObject strings{{"vendor", glString(functions, GL_VENDOR)},
                            {"renderer", glString(functions, GL_RENDERER)},
                            {"version", glString(functions, GL_VERSION)}};
  QJsonObject reply = strings;
  reply.insert("result", clearAndReadBack(functions));
  context.doneCurrent();
  return reply;
}

QJsonArray runtimeOperations() {
  const QJsonObject probes{
      {"type", "array"},
      {"items", QJsonObject{{"type", "string"},
                            {"enum", QJsonArray{"graphics", "media"}}}},
      {"minItems", 1},
      {"maxItems", 2}};
  return {operation(
      "runtime.inspect",
      "Report what this worker can actually run: build and platform, a "
      "graphics readiness probe, media tool identity and mp4 encoder, native "
      "and admitted formats, plugin policy and toolset states, each with "
      "compiled, installed, configured, loaded, verified and usable states.",
      {{"probes", probes}}, {})};
}

QJsonValue executeRuntime(Context& context, const QString& method,
                          const QJsonObject& params) {
  if (method == "runtime.inspect") return inspect(context, params);
  throw Error("method_not_found", "Unknown runtime operation: " + method);
}

}  // namespace GroundzeroAutomation
