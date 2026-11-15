// Included by the Qt playback implementation in BUILD_UI_PORT_TESTS builds.
#include <QPointer>
#include <atomic>
#include <cstdio>
#include <vector>

namespace {

std::atomic<QtMessageHandler> previousAudioTestHandler{nullptr};

void audioOwnershipMessage(QtMsgType type, const QMessageLogContext& context,
                           const QString& message) {
  // A headless CI host may have no sound device. The unopened sink's QObject
  // lifetime is still testable; other Qt diagnostics remain visible.
  if (type == QtWarningMsg && message == "No audio device detected") return;
  if (auto previous = previousAudioTestHandler.load())
    previous(type, context, message);
  else {
    const QByteArray text =
        qFormatLogMessage(type, context, message).toLocal8Bit();
    std::fprintf(stderr, "%s\n", text.constData());
  }
}

class AudioOwnershipMessages final {
public:
  AudioOwnershipMessages() {
    previousAudioTestHandler = qInstallMessageHandler(audioOwnershipMessage);
  }
  ~AudioOwnershipMessages() {
    qInstallMessageHandler(previousAudioTestHandler.load());
    previousAudioTestHandler = nullptr;
  }
};

}  // namespace

bool runSoundOutputOwnershipChecks(QString& failure) {
  AudioOwnershipMessages messages;
  auto owner = std::make_shared<TSoundOutputDeviceImp>();
  QAudioFormat format;
  format.setSampleRate(44100);
  format.setChannelCount(1);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  format.setSampleFormat(QAudioFormat::Int16);
#else
  format.setCodec("audio/pcm");
  format.setByteOrder(QAudioFormat::LittleEndian);
  format.setSampleType(QAudioFormat::SignedInt);
  format.setSampleSize(16);
#endif
  QPointer<TAudioOutput> output;
  std::vector<QPointer<QObject>> children;
  for (int cycle = 0; cycle < 16; ++cycle) {
    format.setSampleRate(cycle % 2 ? 48000 : 44100);
    owner->createAudioOutput(TAudioDevice(), format);
    if (output) {
      failure = "Replacing the audio format retained the previous output";
      return false;
    }
    for (const auto& child : children) {
      if (child) {
        failure = "Replacing audio output retained a child callback object";
        return false;
      }
    }
    output = owner->m_audioOutput.get();
    children.clear();
    for (QObject* child : output->findChildren<QObject*>())
      children.push_back(child);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (output->findChildren<QTimer*>().size() != 1) {
      failure = "Audio output did not own exactly one refill timer";
      return false;
    }
#endif
  }
  owner.reset();
  if (output) {
    failure = "Retiring the sound owner retained its audio output";
    return false;
  }
  for (const auto& child : children) {
    if (child) {
      failure = "Retiring the sound owner retained a child callback object";
      return false;
    }
  }
  failure.clear();
  return true;
}
