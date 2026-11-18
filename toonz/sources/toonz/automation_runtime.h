#pragma once

#include "automation.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace GroundzeroAutomation {

// One capability's layered state as runtime.inspect reports it.
//
// An empty optional reports null: the layer does not apply to the capability.
struct CapabilityState final {
  bool compiled = true;
  std::optional<bool> installed;
  std::optional<bool> configured;
  std::optional<bool> loaded;
  // passed, failed, not_run or not_applicable.
  QString verified = "not_run";
  // always, launch_option, toolset or unavailable; name only for the middle
  // two.
  QString activationKind = "always";
  QString activationName;
  QStringList reasons;

  // Every applicable layer holds and a probe passed or none applies.
  bool usable() const;
  QJsonObject toJson() const;
};

// The result of one graphics probe helper run.
struct GraphicsProbe final {
  // context_ready, no_context, no_framebuffer, readback_mismatch, crashed,
  // timeout or memory.
  QString result;
  QString vendor;
  QString renderer;
  QString version;
  qint64 elapsedMs = 0;

  QJsonObject toJson() const;
};

// Identity and version of one granted media executable.
struct MediaFile final {
  QString file;
  qint64 size = 0;
  QString sha256;
  quint64 inode  = 0;
  qint64 mtimeMs = 0;
  QString versionLine;
  QJsonArray libraries;

  QJsonObject toJson() const;
};

// The result of the media probe: identities, versions and the mp4 encoder.
struct MediaProbe final {
  MediaFile ffmpeg;
  MediaFile ffprobe;
  QString muxerCodec;
  QString encoder;
  // Empty when every command succeeded; else a reason code.
  QString failure;
  qint64 elapsedMs = 0;

  bool encoderAvailable() const { return !encoder.isEmpty(); }
};

// Per-worker cache of probe results; probes run lazily and are cached for
// the worker's life.
struct RuntimeState final {
  std::optional<GraphicsProbe> graphics;
  // Set when a render job failed for want of GL although the helper had GL.
  bool graphicsFailedInWorker = false;
  std::optional<MediaProbe> media;
  // Set when a re-stat before an mp4 job found a granted file replaced.
  bool mediaChanged = false;
};

// Version line and library versions from `-hide_banner -version` output.
struct VersionReport final {
  QString line;
  QJsonArray libraries;
};

VersionReport parseVersionOutput(const QByteArray& output);
// The codec named by "Default video codec:" in `-h muxer=mp4`, or empty.
QString parseMuxerDefaultVideoCodec(const QByteArray& output);
// The first non-experimental video encoder `-encoders` lists for codec.
QString parseEncoder(const QByteArray& output, const QString& codec);
// hardware, software or unknown from GL_VENDOR and GL_RENDERER.
QString classifyRenderer(const QString& vendor, const QString& renderer);

// Run the graphics probe unless cached (or always when forced).
//
// Args:
//   context: Worker owning the runtime cache and private runtime directory.
//   force: Run a new probe even when a result is cached.
// Returns:
//   Whether a probe ran now.
bool ensureGraphicsProbe(Context& context, bool force = false);
// Run the media probe unless cached; no process starts without --media-tools.
bool ensureMediaProbe(Context& context, bool force = false);
CapabilityState graphicsState(Context& context);
CapabilityState mediaState(Context& context);
// {state, qpa_platform, probe, required_by} as runtime.inspect reports it.
QJsonObject graphicsReport(Context& context);
// {writer_registered, muxer_default_video_codec, encoder, encoder_available}.
QJsonObject mp4Report(Context& context);
// Whether the native mp4 level writer is registered in this process.
bool mp4WriterRegistered();
// Re-stat both media files and re-hash on change; false when either changed.
bool mediaToolsUnchanged(Context& context);
// Record that a render job found no GL although the helper probe passed.
void markGraphicsFailedInWorker(Context& context);

// {os, os_version, cpu_architecture, build_cpu_architecture, ...}.
QJsonObject runtimePlatform();

QJsonArray runtimeOperations();
QJsonValue executeRuntime(Context& context, const QString& method,
                          const QJsonObject& params);

// Helper side of the graphics probe: build what QtOfflineGL builds, clear and
// read back a framebuffer and report the GL strings.
QJsonObject graphicsProbeReply();

#ifdef BUILD_UI_PORT_TESTS
void runAutomationRuntimeTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
