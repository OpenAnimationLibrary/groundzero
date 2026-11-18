#include "automation.h"
#include "automation_runtime.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void capabilityDerivation() {
  CapabilityState state;
  require(!state.usable(), "A never-run probe made a capability usable");
  state.verified = "passed";
  require(state.usable(), "Null layers and a passed probe are usable");
  state.verified = "not_applicable";
  require(state.usable(), "A capability without a probe is usable");
  for (auto layer : {&CapabilityState::installed, &CapabilityState::configured,
                     &CapabilityState::loaded}) {
    CapabilityState missing = state;
    missing.*layer          = false;
    require(!missing.usable(), "A false layer left a capability usable");
  }
  CapabilityState uncompiled = state;
  uncompiled.compiled        = false;
  require(!uncompiled.usable(), "An uncompiled capability was usable");
  state.verified = "failed";
  require(!state.usable(), "A failed probe left a capability usable");
  state.activationKind   = "launch_option";
  state.activationName   = "--media-tools";
  state.configured       = false;
  const QJsonObject json = state.toJson();
  require(json.value("installed").isNull(), "An absent layer must be null");
  require(json.value("configured") == QJsonValue(false),
          "A false layer must be reported");
  require(json.value("activation").toObject().value("name") == "--media-tools",
          "The launch option is not reported");
  require(state.toJson().value("usable") == QJsonValue(false),
          "usable must be derived, never stored");
  CapabilityState always;
  require(always.toJson().value("activation").toObject().value("name").isNull(),
          "An always-active capability names no option");
}

const QByteArray VersionFixture =
    "ffmpeg version 8.1.2 Copyright (c) 2000-2026 the FFmpeg developers\n"
    "built with Apple clang version 21.0.0\n"
    "configuration: --enable-gpl\n"
    "libavutil      60. 26.102 / 60. 26.102\n"
    "libavcodec     62. 28.102 / 62. 28.102\n"
    "libswresample   6.  3.102 /  6.  3.102\n";

const QByteArray EncoderFixture =
    "Encoders:\n"
    " V..... = Video\n"
    " ------\n"
    " V..X.D h264_experimental    Test encoder (codec h264)\n"
    " A....D aac                  AAC (codec aac)\n"
    " V....D mpeg4                MPEG-4 part 2\n"
    " V....D libx264              libx264 H.264 (codec h264)\n"
    " V....D h264_videotoolbox    VideoToolbox H.264 Encoder (codec h264)\r\n";

void mediaOutputParsing() {
  const VersionReport version = parseVersionOutput(VersionFixture);
  require(version.line.startsWith("ffmpeg version 8.1.2"),
          "The ffmpeg version line was not parsed");
  require(version.libraries.size() == 3, "Library versions were not parsed");
  require(version.libraries[2].toObject().value("name") == "libswresample" &&
              version.libraries[2].toObject().value("version") == "6.3.102",
          "A padded library version was misread");
  require(parseVersionOutput({}).line.isEmpty(), "Empty output has a version");
  require(
      parseMuxerDefaultVideoCodec("Muxer mp4 [MP4]:\n"
                                  "    Default video codec: h264.\n"
                                  "    Default audio codec: aac.\n") == "h264",
      "The mp4 muxer default codec was not parsed");
  require(parseMuxerDefaultVideoCodec("Muxer mp4 [MP4]:\n").isEmpty(),
          "A missing default codec was invented");
  require(parseEncoder(EncoderFixture, "h264") == "libx264",
          "The first non-experimental h264 encoder was not chosen");
  require(parseEncoder(EncoderFixture, "mpeg4") == "mpeg4",
          "An encoder named after its codec was not matched");
  require(parseEncoder(EncoderFixture, "aac").isEmpty(),
          "An audio encoder satisfied a video codec");
  require(parseEncoder(EncoderFixture, "hevc").isEmpty(),
          "An unlisted codec was reported available");
  require(parseEncoder(EncoderFixture, "").isEmpty(),
          "An unknown codec was reported available");
}

void rendererClassification() {
  require(classifyRenderer("Apple", "Apple M2") == "hardware",
          "Apple GPU is not hardware");
  require(classifyRenderer("Mesa", "llvmpipe (LLVM 17.0.6, 256 bits)") ==
              "software",
          "llvmpipe is not software");
  require(classifyRenderer("Google Inc.", "Google SwiftShader") == "software",
          "SwiftShader is not software");
  require(
      classifyRenderer("Microsoft Corporation", "GDI Generic") == "software",
      "GDI Generic is not software");
  require(classifyRenderer("Intel", "Mesa Intel(R) UHD Graphics") == "hardware",
          "A Mesa hardware driver is not hardware");
  require(classifyRenderer("Unnamed", "Some GPU") == "unknown",
          "An unknown vendor was classified");
}

void graphicsProbeCaching(Context& context) {
  context.runtimeState.reset();
  require(graphicsState(context).verified == "not_run",
          "A fresh worker reports a graphics result");
  require(graphicsReport(context).value("probe").isNull(),
          "A fresh worker reports a probe");
  require(ensureGraphicsProbe(context), "The first graphics probe did not run");
  require(!ensureGraphicsProbe(context), "A cached graphics probe ran again");
  const QJsonObject probe = graphicsReport(context).value("probe").toObject();
  require(!probe.value("result").toString().isEmpty() &&
              probe.value("elapsed_ms").toDouble() <= 10000,
          "The graphics probe result is not bounded");
  const bool ready = probe.value("result") == "context_ready";
  require(graphicsState(context).usable() == ready,
          "Graphics usability differs from the probe result");
#ifdef BUILD_UI_PORT_TESTS
  const QString fault = context.childFault;
  context.childFault  = "crash";
  require(ensureGraphicsProbe(context, true), "A forced probe did not run");
  context.childFault = fault;
  require(graphicsReport(context).value("probe").toObject().value("result") ==
              "crashed",
          "A crashed probe helper was not reported as crashed");
  require(!graphicsState(context).usable(),
          "A crashed probe left graphics usable");
#endif
  ensureGraphicsProbe(context, true);
  if (ready) {
    markGraphicsFailedInWorker(context);
    require(!graphicsState(context).usable(),
            "A render without GL left graphics usable");
  }
  context.runtimeState.reset();
}

void mediaWithoutConfiguration(Context& context) {
  const QString directory     = context.mediaToolsDirectory;
  context.mediaToolsDirectory = QString();
  context.runtimeState.reset();
  require(!ensureMediaProbe(context, true),
          "A media probe ran without --media-tools");
  const CapabilityState state = mediaState(context);
  require(state.configured == false &&
              state.activationKind == "launch_option" &&
              state.activationName == "--media-tools" && !state.usable() &&
              state.verified == "not_run",
          "Unconfigured media tools were not reported as a launch option");
  require(mp4Report(context).value("encoder_available").isNull(),
          "An unprobed encoder was reported");
  context.mediaToolsDirectory = directory;
}

#ifndef _WIN32
void writeStub(const QString& path, const QByteArray& body) {
  QFile file(path);
  require(
      file.open(QIODevice::WriteOnly) && file.write("#!/bin/sh\n" + body) > 0,
      "Cannot write a stub media tool");
  file.close();
  require(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner),
          "Cannot mark a stub media tool executable");
}

QByteArray stubFfmpeg(const QByteArray& encoders) {
  return "case \"$2\" in\n"
         "-version) echo 'ffmpeg version stub-1'; "
         "echo 'libavcodec     62. 28.102 / 62. 28.102';;\n"
         "-h) echo 'Muxer mp4:'; echo '    Default video codec: h264.';;\n"
         "-encoders) printf '%s\\n' 'Encoders:' ' ------' '" +
         encoders +
         "';;\n"
         "esac\n";
}

void mediaStubProbe(Context& context, const QString& directory,
                    const QByteArray& encoders, const QString& encoder) {
  writeStub(QDir(directory).filePath("ffmpeg"), stubFfmpeg(encoders));
  writeStub(QDir(directory).filePath("ffprobe"),
            "echo 'ffprobe version stub-1'\n");
  context.runtimeState.reset();
  require(ensureMediaProbe(context), "The configured media probe did not run");
  require(!ensureMediaProbe(context), "A cached media probe ran again");
  const QJsonObject mp4 = mp4Report(context);
  require(mp4.value("muxer_default_video_codec") == "h264",
          "The stub muxer codec was not read");
  require(
      mp4.value("encoder") == (encoder.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                 : QJsonValue(encoder)),
      "The stub encoder was misreported");
  const CapabilityState state = mediaState(context);
  require(state.verified == (encoder.isEmpty() ? "failed" : "passed"),
          "Encoder availability did not decide verification");
  require(encoder.isEmpty() == state.reasons.contains("codec_unavailable"),
          "A missing encoder was not named codec_unavailable");
}

void mediaIdentityAndChange(Context& context) {
  QTemporaryDir directory(
      QDir(context.workspace()).filePath("runtime-media-XXXXXX"));
  require(directory.isValid(), "Cannot create stub media storage");
  const QString canonical     = QFileInfo(directory.path()).canonicalFilePath();
  const QString previous      = context.mediaToolsDirectory;
  context.mediaToolsDirectory = canonical;
  mediaStubProbe(context, canonical, " V....D stubx264   Stub (codec h264)",
                 "stubx264");
  require(mediaToolsUnchanged(context), "Unchanged media tools were refused");
  QFile ffprobe(QDir(canonical).filePath("ffprobe"));
  require(ffprobe.open(QIODevice::Append) && ffprobe.write("exit 0\n") > 0,
          "Cannot replace the stub ffprobe");
  ffprobe.close();
  require(!mediaToolsUnchanged(context), "A replaced ffprobe was accepted");
  require(mediaState(context).reasons.contains("media_tools_changed"),
          "A replaced media tool was not reported");
  mediaStubProbe(context, canonical, " V....D mpeg4   MPEG-4 part 2", "");
  context.mediaToolsDirectory = previous;
  context.runtimeState.reset();
}
#endif

void platformReport() {
  const QJsonObject platform = runtimePlatform();
  require(QStringList({"macos", "windows", "linux", "other"})
              .contains(platform.value("os").toString()),
          "The operating system is outside its enumeration");
  for (const char* key :
       {"os_version", "cpu_architecture", "build_cpu_architecture",
        "qt_runtime_version", "qt_build_version"})
    require(!platform.value(key).toString().isEmpty(),
            "A platform field is empty");
#if defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
  require(platform.value("cpu_architecture") == "arm64" &&
              platform.value("build_cpu_architecture") == "arm64",
          "An arm64 build on Apple silicon misreports its architecture");
#endif
}

}  // namespace

void runAutomationRuntimeTests(Context& context) {
  capabilityDerivation();
  mediaOutputParsing();
  rendererClassification();
  platformReport();
  graphicsProbeCaching(context);
  mediaWithoutConfiguration(context);
#ifndef _WIN32
  mediaIdentityAndChange(context);
#endif
  std::cout << "automation-runtime-test: capability states, probes and "
               "parsers PASS\n";
}

}  // namespace GroundzeroAutomation
