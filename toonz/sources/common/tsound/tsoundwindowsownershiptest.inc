// Included only by the Windows backend in BUILD_UI_PORT_TESTS builds.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QString>
#include <functional>
#include <stdexcept>
#include <thread>
#include "tsound_t.h"

namespace {

struct FakeWaveState {
  std::mutex mutex;
  std::map<WAVEHDR*, bool> prepared;
  std::vector<std::vector<char>> writes;
  DWORD threadId            = 0;
  HWAVEOUT handle           = nullptr;
  int prepareCount          = 0;
  int releaseCount          = 0;
  int openCount             = 0;
  int closeCount            = 0;
  int unprepareFailures     = 0;
  int failPrepare           = 0;
  int failWrite             = 0;
  int failUnprepare         = 0;
  int failReset             = 0;
  int failClose             = 0;
  bool failOpen             = false;
  bool failThread           = false;
  bool failPost             = false;
  bool queueReady           = false;
  bool duplicateResetDone   = false;
  int barrierPosts          = 0;
  int postFailures          = 0;
  int deliveredBarriers     = 0;
  int idleBarrierGeneration = 0;
  HANDLE startupGate        = nullptr;
  HANDLE startupEntered     = nullptr;
  HANDLE exitGate           = nullptr;
  HANDLE exitEntered        = nullptr;
  HANDLE quitGate           = nullptr;
  HANDLE quitEntered        = nullptr;
  HANDLE barrierAccepted    = nullptr;
  std::atomic<bool> threadExited{false};
  ~FakeWaveState() {
    if (startupGate) CloseHandle(startupGate);
    if (startupEntered) CloseHandle(startupEntered);
    if (exitGate) CloseHandle(exitGate);
    if (exitEntered) CloseHandle(exitEntered);
    if (quitGate) CloseHandle(quitGate);
    if (quitEntered) CloseHandle(quitEntered);
    if (barrierAccepted) CloseHandle(barrierAccepted);
  }
};

struct FakeWaveHandle {
  std::shared_ptr<FakeWaveState> state;
};
thread_local std::shared_ptr<FakeWaveState> activeWaveState;

bool consumeFailure(int& count) {
  if (!count) return false;
  if (count > 0) --count;
  return true;
}

void audioRequire(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void audioWait(const std::function<bool()>& condition, const char* message) {
  QElapsedTimer deadline;
  deadline.start();
  while (!condition() && deadline.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    Sleep(1);
  }
  audioRequire(condition(), message);
}

void audioWaitWorker(const std::function<bool()>& condition,
                     const char* message) {
  QElapsedTimer deadline;
  deadline.start();
  while (!condition() && deadline.elapsed() < 5000) Sleep(1);
  audioRequire(condition(), message);
}

MMRESULT WINAPI fakeWaveOpen(LPHWAVEOUT output, UINT, LPCWAVEFORMATEX,
                             DWORD_PTR callback, DWORD_PTR, DWORD flags) {
  if (flags & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
  auto state = activeWaveState;
  std::lock_guard<std::mutex> lock(state->mutex);
  state->queueReady =
      PostThreadMessage(DWORD(callback), WM_NULL, 0, 0) != FALSE;
  if (state->failOpen) return MMSYSERR_ERROR;
  auto handle     = std::make_unique<FakeWaveHandle>();
  handle->state   = state;
  *output         = reinterpret_cast<HWAVEOUT>(handle.release());
  state->handle   = *output;
  state->threadId = DWORD(callback);
  ++state->openCount;
  return MMSYSERR_NOERROR;
}

MMRESULT WINAPI fakeWavePrepare(HWAVEOUT device, LPWAVEHDR header, UINT) {
  auto state = reinterpret_cast<FakeWaveHandle*>(device)->state;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (consumeFailure(state->failPrepare)) return MMSYSERR_NOMEM;
  header->dwFlags |= WHDR_PREPARED;
  state->prepared.emplace(header, false);
  ++state->prepareCount;
  return MMSYSERR_NOERROR;
}

MMRESULT WINAPI fakeWaveWrite(HWAVEOUT device, LPWAVEHDR header, UINT) {
  auto state = reinterpret_cast<FakeWaveHandle*>(device)->state;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (consumeFailure(state->failWrite)) return MMSYSERR_ERROR;
  const auto found = state->prepared.find(header);
  if (found == state->prepared.end() || found->second)
    return MMSYSERR_INVALPARAM;
  state->writes.emplace_back(header->lpData,
                             header->lpData + header->dwBufferLength);
  found->second   = true;
  header->dwFlags = (header->dwFlags | WHDR_INQUEUE) & ~WHDR_DONE;
  return MMSYSERR_NOERROR;
}

MMRESULT WINAPI fakeWaveUnprepare(HWAVEOUT device, LPWAVEHDR header, UINT) {
  auto state = reinterpret_cast<FakeWaveHandle*>(device)->state;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (consumeFailure(state->failUnprepare)) {
    ++state->unprepareFailures;
    return MMSYSERR_NOMEM;
  }
  const auto found = state->prepared.find(header);
  if (found == state->prepared.end()) return MMSYSERR_INVALPARAM;
  if (found->second) return WAVERR_STILLPLAYING;
  header->dwFlags &= ~WHDR_PREPARED;
  state->prepared.erase(found);
  ++state->releaseCount;
  return MMSYSERR_NOERROR;
}

void returnWaveBuffers(HWAVEOUT device, FakeWaveState& state, bool all) {
  for (auto& entry : state.prepared) {
    if (!entry.second) continue;
    entry.second         = false;
    entry.first->dwFlags = (entry.first->dwFlags | WHDR_DONE) & ~WHDR_INQUEUE;
    PostThreadMessage(state.threadId, MM_WOM_DONE, WPARAM(device),
                      LPARAM(entry.first));
    if (!all) break;
  }
}

MMRESULT WINAPI fakeWaveReset(HWAVEOUT device) {
  auto state = reinterpret_cast<FakeWaveHandle*>(device)->state;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (consumeFailure(state->failReset)) return MMSYSERR_ERROR;
  returnWaveBuffers(device, *state, true);
  if (state->duplicateResetDone) {
    for (const auto& entry : state->prepared)
      PostThreadMessage(state->threadId, MM_WOM_DONE, WPARAM(device),
                        LPARAM(entry.first));
  }
  return MMSYSERR_NOERROR;
}

MMRESULT WINAPI fakeWaveClose(HWAVEOUT device) {
  auto* handle = reinterpret_cast<FakeWaveHandle*>(device);
  auto state   = handle->state;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (consumeFailure(state->failClose)) return MMSYSERR_ERROR;
  if (!state->prepared.empty()) return WAVERR_STILLPLAYING;
  ++state->closeCount;
  state->handle = nullptr;
  delete handle;
  return MMSYSERR_NOERROR;
}

struct FakeAudioLaunch {
  LPTHREAD_START_ROUTINE start;
  LPVOID argument;
  std::shared_ptr<FakeWaveState> state;
};

DWORD WINAPI fakeAudioThreadMain(void* argument) {
  std::unique_ptr<FakeAudioLaunch> launch(
      static_cast<FakeAudioLaunch*>(argument));
  activeWaveState = launch->state;
  if (launch->state->startupGate) {
    SetEvent(launch->state->startupEntered);
    WaitForSingleObject(launch->state->startupGate, INFINITE);
  }
  const DWORD result          = launch->start(launch->argument);
  launch->state->threadExited = true;
  activeWaveState.reset();
  return result;
}

HANDLE WINAPI fakeAudioThread(LPSECURITY_ATTRIBUTES security, SIZE_T stack,
                              LPTHREAD_START_ROUTINE start, LPVOID argument,
                              DWORD flags, LPDWORD id) {
  if (activeWaveState->failThread) return nullptr;
  auto launch = std::make_unique<FakeAudioLaunch>(
      FakeAudioLaunch{start, argument, activeWaveState});
  HANDLE thread = CreateThread(security, stack, fakeAudioThreadMain,
                               launch.get(), flags, id);
  if (thread) launch.release();
  return thread;
}

BOOL WINAPI fakeAudioPost(DWORD id, UINT message, WPARAM value, LPARAM data) {
  if (activeWaveState) {
    std::lock_guard<std::mutex> lock(activeWaveState->mutex);
    if (activeWaveState->failPost) {
      ++activeWaveState->postFailures;
      return FALSE;
    }
    if (message == waveBarrierMessage) ++activeWaveState->barrierPosts;
  }
  const BOOL result = PostThreadMessage(id, message, value, data);
  if (result && message == waveBarrierMessage && activeWaveState &&
      activeWaveState->barrierAccepted)
    SetEvent(activeWaveState->barrierAccepted);
  return result;
}

BOOL WINAPI fakeAudioPeek(LPMSG message, HWND window, UINT first, UINT last,
                          UINT remove) {
  const auto state = activeWaveState;
  if (first == waveBarrierMessage && state && state->exitGate) {
    SetEvent(state->exitEntered);
    WaitForSingleObject(state->exitGate, INFINITE);
  }
  return PeekMessage(message, window, first, last, remove);
}

BOOL WINAPI fakeAudioGet(LPMSG message, HWND window, UINT first, UINT last) {
  const auto state = activeWaveState;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->idleBarrierGeneration = state->deliveredBarriers;
  }
  const BOOL result = GetMessage(message, window, first, last);
  if (result > 0 && message->message == waveBarrierMessage) {
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->deliveredBarriers;
  }
  if (result == 0 && state && state->quitGate) {
    SetEvent(state->quitEntered);
    WaitForSingleObject(state->quitGate, INFINITE);
  }
  return result;
}

class FakeAudioScope {
  WaveOutputApi m_api;
  const WaveOutputApi* m_previousApi;
  std::shared_ptr<FakeWaveState> m_previousState;

public:
  std::shared_ptr<FakeWaveState> state = std::make_shared<FakeWaveState>();
  FakeAudioScope()
      : m_previousApi(testWaveOutputApi), m_previousState(activeWaveState) {
    m_api.open         = fakeWaveOpen;
    m_api.close        = fakeWaveClose;
    m_api.prepare      = fakeWavePrepare;
    m_api.unprepare    = fakeWaveUnprepare;
    m_api.write        = fakeWaveWrite;
    m_api.reset        = fakeWaveReset;
    m_api.createThread = fakeAudioThread;
    m_api.postMessage  = fakeAudioPost;
    m_api.peekMessage  = fakeAudioPeek;
    m_api.getMessage   = fakeAudioGet;
    testWaveOutputApi  = &m_api;
    activeWaveState    = state;
  }
  ~FakeAudioScope() {
    testWaveOutputApi = m_previousApi;
    activeWaveState   = m_previousState;
  }
  void balanced() {
    std::lock_guard<std::mutex> lock(state->mutex);
    audioRequire(state->prepared.empty(),
                 "Windows audio retained prepared buffers");
    audioRequire(state->prepareCount == state->releaseCount,
                 "Windows audio released a buffer zero or multiple times");
    audioRequire(state->openCount == state->closeCount,
                 "Windows audio retained an open device");
  }
};

TSoundTrackP audioTrack(int rate = 32000, short value = 12000) {
  auto track = TSoundTrack::create(rate, 16, 1, 128, TSound::INT);
  auto* samples =
      static_cast<TSoundTrackT<TMono16Sample>*>(track.getPointer())->samples();
  std::fill(samples, samples + track->getSampleCount(), TMono16Sample(value));
  return track;
}

std::vector<char> audioBytes(const TSoundTrackP& track) {
  auto* begin = reinterpret_cast<const char*>(track->getRawData());
  return {begin, begin + track->getSampleCount() * track->getSampleSize()};
}

void checkAudioStartup() {
  FakeAudioScope scope;
  for (int cycle = 0; cycle < 8; ++cycle) {
    TSoundOutputDevice unused;
  }
  {
    TSoundOutputDevice device;
    auto track = audioTrack();
    audioRequire(
        device.getPreferredFormat(track->getFormat()) == track->getFormat(),
        "Windows audio lost the supported 32000 Hz format");
    scope.state->failThread = true;
    audioRequire(!device.open(track),
                 "Windows audio accepted failed thread creation");
    scope.state->failThread = false;
    scope.state->failOpen   = true;
    audioRequire(!device.open(track),
                 "Windows audio accepted failed device opening");
    scope.state->failOpen = false;
    audioRequire(device.open(track),
                 "Windows audio could not recover from startup failure");
    audioRequire(scope.state->queueReady,
                 "Windows audio opened before the callback queue was ready");
  }
  scope.balanced();
}

void checkAudioDelayedStartup() {
  FakeAudioScope scope;
  scope.state->startupGate    = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  scope.state->startupEntered = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  audioRequire(scope.state->startupGate && scope.state->startupEntered,
               "Windows audio fixture could not create readiness gates");
  TSoundOutputDevice device;
  std::atomic<bool> heldBeforeOpen{false};
  std::thread release([state = scope.state, &heldBeforeOpen]() {
    if (WaitForSingleObject(state->startupEntered, 5000) == WAIT_OBJECT_0) {
      std::lock_guard<std::mutex> lock(state->mutex);
      heldBeforeOpen = state->openCount == 0 && !state->queueReady;
    }
    SetEvent(state->startupGate);
  });
  bool opened = false;
  try {
    opened = device.open(audioTrack());
  } catch (...) {
    SetEvent(scope.state->startupGate);
    release.join();
    throw;
  }
  release.join();
  audioRequire(opened && heldBeforeOpen,
               "Windows audio bypassed callback queue readiness");
  device.close();
  scope.balanced();
}

struct AudioListener final : TSoundOutputDeviceListener {
  int calls = 0;
  std::function<void()> completed;
  void onPlayCompleted() override {
    ++calls;
    if (completed) completed();
  }
};

WAVEHDR* completeAudio(const std::shared_ptr<FakeWaveState>& state,
                       bool all = true) {
  std::lock_guard<std::mutex> lock(state->mutex);
  WAVEHDR* first = nullptr;
  for (const auto& entry : state->prepared) {
    if (entry.second) {
      first = entry.first;
      break;
    }
  }
  returnWaveBuffers(state->handle, *state, all);
  return first;
}

size_t audioWrites(const std::shared_ptr<FakeWaveState>& state) {
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->writes.size();
}

void checkAudioCompletion() {
  FakeAudioScope scope;
  auto track = audioTrack();
  AudioListener listener;
  {
    TSoundOutputDevice device;
    device.attach(&listener);
    device.play(track, TINT32(0), TINT32(127));
    completeAudio(scope.state);
    audioWait([&]() { return listener.calls == 1; },
              "Windows audio completion did not reach the listener");
    audioRequire(!device.isPlaying(),
                 "Completed Windows audio remained playing");
    device.play(track, TINT32(0), TINT32(127), true);
    const size_t before = audioWrites(scope.state);
    completeAudio(scope.state);
    audioWait([&]() { return audioWrites(scope.state) == before + 1; },
              "Looped Windows audio did not requeue its completed buffer");
    {
      std::lock_guard<std::mutex> lock(scope.state->mutex);
      audioRequire(scope.state->writes.back() == audioBytes(track),
                   "Looped Windows audio changed the replayed samples");
    }
    device.stop();
    device.detach(&listener);
  }
  scope.balanced();
  audioRequire(listener.calls == 1,
               "Stopping looped audio delivered extra completion");
}

void checkAudioRegistrationRetirement() {
  FakeAudioScope scope;
  auto track = audioTrack();
  AudioListener listener;
  {
    TSoundOutputDevice device;
    device.attach(&listener);
    device.play(track, TINT32(0), TINT32(127));
    completeAudio(scope.state);
    // Normal completion queues the GUI message before closing the device.
    audioWaitWorker(
        [&]() {
          std::lock_guard<std::mutex> lock(scope.state->mutex);
          return scope.state->closeCount == 1;
        },
        "Windows audio did not queue its completion before listener "
        "retirement");
    device.detach(&listener);
    device.attach(&listener);
    QCoreApplication::processEvents();
    audioRequire(
        listener.calls == 0,
        "A new audio listener registration received an old completion");
    listener.completed = [&]() {
      device.detach(&listener);
      device.stop();
    };
    device.play(track, TINT32(0), TINT32(127));
    completeAudio(scope.state);
    audioWait([&]() { return listener.calls == 1; },
              "Audio completion could not detach and stop reentrantly");
  }
  scope.balanced();
}

void checkAudioPlayback() {
  FakeAudioScope scope;
  auto track = audioTrack();
  AudioListener listener;
  {
    TSoundOutputDevice device;
    device.attach(&listener);
    device.play(track, TINT32(0), TINT32(127));
    {
      std::lock_guard<std::mutex> lock(scope.state->mutex);
      audioRequire(scope.state->writes.front() == audioBytes(track),
                   "Windows audio changed ordinary playback bytes");
    }
    // Stop returns only after callback messages have retired the old buffers.
    device.stop();
    device.play(audioTrack(44100), TINT32(0), TINT32(127), true);
    audioRequire(device.isLooping(), "Windows audio did not start looping");
    device.stop();
    audioRequire(!device.isLooping() && !device.isPlaying(),
                 "Windows audio stop retained looping or playing state");
    device.detach(&listener);
  }
  scope.balanced();
  audioRequire(listener.calls == 0,
               "Stopped playback delivered a completion notification");
}

void checkAudioFailureRecovery() {
  FakeAudioScope scope;
  auto track = audioTrack();
  {
    TSoundOutputDevice device;
    for (int kind = 0; kind < 2; ++kind) {
      {
        std::lock_guard<std::mutex> lock(scope.state->mutex);
        if (kind == 0)
          scope.state->failPrepare = 1;
        else
          scope.state->failWrite = 1;
      }
      bool rejected = false;
      try {
        device.play(track, TINT32(0), TINT32(127));
      } catch (const TSoundDeviceException&) {
        rejected = true;
      }
      audioRequire(rejected, "Windows audio accepted a failed prepare/write");
      device.play(track, TINT32(0), TINT32(127));
      device.stop();
    }
    device.play(track, TINT32(0), TINT32(127));
    {
      std::lock_guard<std::mutex> lock(scope.state->mutex);
      scope.state->failUnprepare = -1;
    }
    device.stop();
    audioRequire(failedWaveOutputs.load() == 1,
                 "Windows audio allowed playback after failed buffer cleanup");
    {
      std::lock_guard<std::mutex> lock(scope.state->mutex);
      scope.state->failUnprepare = 0;
    }
    device.stop();
    audioRequire(
        failedWaveOutputs.load() == 0,
        "Windows audio did not recover after successful stopped cleanup");
    device.play(track, TINT32(0), TINT32(127));
    device.stop();
  }
  scope.balanced();
}

void checkAudioScrubbing() {
  FakeAudioScope scope;
  auto track = audioTrack(32000, -32768);
  {
    TSoundOutputDevice device;
    device.play(track, TINT32(0), TINT32(127), false, true);
    std::vector<std::vector<char>> expected = {
        audioBytes(TSop::fadeIn(track, 0.9)), audioBytes(track),
        audioBytes(TSop::fadeOut(track, 0.9))};
    for (int cycle = 0; cycle < 3; ++cycle) {
      device.play(track, TINT32(0), TINT32(127), false, true);
      expected.push_back(audioBytes(TSop::fadeIn(track, 0.3)));
      expected.push_back(audioBytes(track));
      if (cycle < 2) expected.push_back(audioBytes(TSop::fadeOut(track, 0.3)));
    }
    {
      std::lock_guard<std::mutex> lock(scope.state->mutex);
      audioRequire(scope.state->writes == expected,
                   "Windows audio changed scrub fade, sample, or decay order");
    }
    device.stop();
  }
  scope.balanced();
}

std::vector<char> audioCrossFade(const TSoundTrackP& from,
                                 const TSoundTrackP& to) {
  auto expected = audioBytes(to);
  auto prefix   = audioBytes(TSop::crossFade(from, to, 0.3));
  std::copy_n(prefix.begin(), std::min(prefix.size(), expected.size()),
              expected.begin());
  return expected;
}

void completeOneAudio(const std::shared_ptr<FakeWaveState>& state) {
  int released = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    released = state->releaseCount;
  }
  completeAudio(state, false);
  audioWaitWorker(
      [&]() {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->releaseCount == released + 1;
      },
      "Windows audio did not retire a completed scrub buffer");
}

void checkAudioScrubPending() {
  FakeAudioScope scope;
  auto low = audioTrack(32000, -32768);
  auto mid = audioTrack(32000, 12000);
  {
    TSoundOutputDevice device;
    for (int cycle = 0; cycle < 4; ++cycle)
      device.play(low, TINT32(0), TINT32(127), false, true);
    completeOneAudio(scope.state);
    completeOneAudio(scope.state);
    device.play(mid, TINT32(0), TINT32(127), false, true);
    {
      std::lock_guard<std::mutex> lock(scope.state->mutex);
      audioRequire(scope.state->writes.size() == 12 &&
                       scope.state->writes.back() == audioCrossFade(low, mid),
                   "Windows audio changed the submitted scrub crossfade");
    }
    const std::vector<TSoundTrackP> pending = {mid, low, mid, low, mid};
    for (const auto& track : pending)
      device.play(track, TINT32(0), TINT32(127), false, true);
    audioRequire(audioWrites(scope.state) == 12,
                 "Windows audio submitted pending scrub samples too early");
    for (size_t index = 1; index < pending.size(); ++index) {
      completeOneAudio(scope.state);
      audioWaitWorker([&]() { return audioWrites(scope.state) == 12 + index; },
                      "Windows audio failed to submit pending scrub samples");
      const auto expected =
          index == 1 ? audioCrossFade(mid, low) : audioBytes(pending[index]);
      std::lock_guard<std::mutex> lock(scope.state->mutex);
      audioRequire(
          scope.state->writes.back() == expected,
          "Windows audio changed pending scrub eviction or FIFO sample order");
    }
    device.stop();
  }
  scope.balanced();
}

void checkAudioLateCompletion() {
  FakeAudioScope scope;
  scope.state->duplicateResetDone = true;
  {
    TSoundOutputDevice device;
    for (int cycle = 0; cycle < 16; ++cycle) {
      auto track = audioTrack(32000, short(10000 + cycle * 100));
      device.play(track, TINT32(0), TINT32(127));
      device.stop();
      std::lock_guard<std::mutex> lock(scope.state->mutex);
      audioRequire(scope.state->writes.size() == size_t(cycle + 1) &&
                       scope.state->writes.back() == audioBytes(track),
                   "Reset completion changed a later playback generation");
      audioRequire(
          scope.state->prepared.empty() &&
              scope.state->releaseCount == scope.state->prepareCount,
          "Stop returned before reset completions retired their buffers");
    }
  }
  scope.balanced();
}

void checkAudioRetainedFailure(int kind) {
  FakeAudioScope scope;
  auto owner = std::make_shared<TSoundOutputDeviceImp>();
  owner->play(audioTrack(), false, false);
  {
    std::lock_guard<std::mutex> lock(scope.state->mutex);
    if (kind == 0) scope.state->failUnprepare = -1;
    if (kind == 1) scope.state->failReset = -1;
    if (kind == 2) scope.state->failClose = -1;
  }
  std::weak_ptr<TSoundOutputDeviceImp> retained = owner;
  owner->shutdown();
  owner.reset();
  audioRequire(!retained.expired() && failedWaveOutputs.load() == 1,
               "Windows audio destroyed a controller with failed cleanup");
  bool blocked = false;
  try {
    TSoundOutputDevice another;
  } catch (const TSoundDeviceException&) {
    blocked = true;
  }
  audioRequire(blocked,
               "Failed Windows cleanup permitted another retained output");
  {
    std::lock_guard<std::mutex> lock(scope.state->mutex);
    scope.state->failUnprepare = 0;
    scope.state->failReset     = 0;
    scope.state->failClose     = 0;
    if (kind == 1) returnWaveBuffers(scope.state->handle, *scope.state, true);
    PostThreadMessage(scope.state->threadId, WM_NULL, 0, 0);
  }
  audioWaitWorker(
      [&]() { return retained.expired() && scope.state->threadExited; },
      "Recovered Windows audio controller did not retire its worker");
  audioRequire(failedWaveOutputs.load() == 0,
               "Recovered Windows audio remained disabled");
  scope.balanced();
}

void checkAudioPostFailure() {
  FakeAudioScope scope;
  auto owner = std::make_shared<TSoundOutputDeviceImp>();
  audioRequire(owner->doOpenDevice(audioTrack()->getFormat()),
               "Windows audio post-failure fixture could not open");
  owner->stop();
  audioWaitWorker(
      [&]() {
        std::lock_guard<std::mutex> lock(scope.state->mutex);
        return scope.state->idleBarrierGeneration > 0;
      },
      "Windows audio fixture did not reach an idle callback wait");
  {
    std::lock_guard<std::mutex> lock(scope.state->mutex);
    scope.state->failPost = true;
  }
  std::weak_ptr<TSoundOutputDeviceImp> retained = owner;
  owner->shutdown();
  owner.reset();
  {
    std::lock_guard<std::mutex> lock(scope.state->mutex);
    audioRequire(
        scope.state->postFailures > 0 && !retained.expired(),
        "Windows audio fixture did not exercise failed shutdown posting");
    scope.state->failPost = false;
  }
  PostThreadMessage(scope.state->threadId, WM_NULL, 0, 0);
  audioWaitWorker(
      [&]() { return retained.expired() && scope.state->threadExited; },
      "Windows audio blocked on a failed shutdown message");
  scope.balanced();
}

void checkAudioUnexpectedExit() {
  FakeAudioScope scope;
  auto owner = std::make_shared<TSoundOutputDeviceImp>();
  owner->play(audioTrack(), false, false);
  {
    std::lock_guard<std::mutex> lock(scope.state->mutex);
    scope.state->failUnprepare = -1;
  }
  PostThreadMessage(scope.state->threadId, WM_QUIT, 0, 0);
  audioWaitWorker([&]() { return scope.state->threadExited.load(); },
                  "Windows audio fixture worker did not exit");
  audioRequire(failedWaveOutputs.load() == 1,
               "An exited Windows audio worker freed retained device storage");
  audioRequire(!owner->doOpenDevice(audioTrack()->getFormat()),
               "Windows audio reopened a device using an exited thread ID");
  {
    std::lock_guard<std::mutex> lock(scope.state->mutex);
    scope.state->failUnprepare = 0;
  }
  audioRequire(owner->doCloseDevice(),
               "Windows audio terminal cleanup did not recover");
  std::weak_ptr<TSoundOutputDeviceImp> retired = owner;
  owner->shutdown();
  owner.reset();
  audioRequire(retired.expired(),
               "Windows audio retained a recovered terminal owner");
  scope.balanced();
}

void checkAudioExitBarrierRace() {
  FakeAudioScope scope;
  scope.state->exitGate    = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  scope.state->exitEntered = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  audioRequire(scope.state->exitGate && scope.state->exitEntered,
               "Windows audio fixture could not create exit gates");
  auto owner = std::make_shared<TSoundOutputDeviceImp>();
  audioRequire(owner->doOpenDevice(audioTrack()->getFormat()),
               "Windows audio exit fixture could not open");
  PostThreadMessage(scope.state->threadId, WM_QUIT, 0, 0);
  const bool entered =
      WaitForSingleObject(scope.state->exitEntered, 5000) == WAIT_OBJECT_0;
  if (!entered) SetEvent(scope.state->exitGate);
  audioRequire(entered,
               "Windows audio fixture did not reach final message draining");
  std::atomic<bool> closeStarted{false};
  std::atomic<bool> closed{false};
  std::thread closer([owner, state = scope.state, &closeStarted, &closed]() {
    activeWaveState = state;
    closeStarted    = true;
    closed          = owner->doCloseDevice();
    activeWaveState.reset();
  });
  while (!closeStarted) Sleep(1);
  SetEvent(scope.state->exitGate);
  closer.join();
  audioRequire(closed, "Windows audio close raced the callback thread exit");
  {
    std::lock_guard<std::mutex> lock(scope.state->mutex);
    audioRequire(
        scope.state->barrierPosts == 0,
        "Windows audio posted a barrier after final message draining began");
  }
  owner->shutdown();
  owner.reset();
  scope.balanced();
}

void checkAudioConcurrentDetach(
    const std::function<bool(const std::shared_ptr<TSoundOutputDeviceImp>&)>&
        removed) {
  FakeAudioScope scope;
  auto owner = std::make_shared<TSoundOutputDeviceImp>();
  AudioListener listener;
  std::atomic<bool> entered{false};
  std::atomic<bool> detached{false};
  std::atomic<bool> release{false};
  std::atomic<bool> observedRetirement{false};
  bool prematureDetach  = false;
  bool deliveryTimedOut = false;
  listener.completed    = [&]() {
    entered = true;
    QElapsedTimer deadline;
    deadline.start();
    while (!release && deadline.elapsed() < 5000) Sleep(1);
    deliveryTimedOut = !release;
    prematureDetach  = detached;
  };
  owner->attach(&listener);
  owner->play(audioTrack(), false, false);
  completeAudio(scope.state);
  std::thread detach([&]() {
    QElapsedTimer deadline;
    deadline.start();
    while (!entered && deadline.elapsed() < 5000) Sleep(1);
    owner->detach(&listener);
    detached = true;
  });
  std::thread retire([&]() {
    QElapsedTimer deadline;
    deadline.start();
    while (!removed(owner) && deadline.elapsed() < 5000) Sleep(1);
    observedRetirement = removed(owner);
    release            = true;
  });
  try {
    audioWait([&]() { return listener.calls == 1; },
              "Windows audio concurrent listener fixture did not deliver");
  } catch (...) {
    release = true;
    detach.join();
    retire.join();
    owner->shutdown();
    throw;
  }
  detach.join();
  retire.join();
  audioRequire(
      observedRetirement && detached && !deliveryTimedOut && !prematureDetach,
      "Windows audio did not synchronize listener retirement");
  owner->shutdown();
  scope.balanced();
}

void checkAudioAcceptedExitBarrier() {
  FakeAudioScope scope;
  scope.state->quitGate        = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  scope.state->quitEntered     = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  scope.state->barrierAccepted = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  audioRequire(scope.state->quitGate && scope.state->quitEntered &&
                   scope.state->barrierAccepted,
               "Windows audio fixture could not create accepted-message gates");
  auto owner = std::make_shared<TSoundOutputDeviceImp>();
  audioRequire(owner->doOpenDevice(audioTrack()->getFormat()),
               "Windows audio accepted-message fixture could not open");
  PostThreadMessage(scope.state->threadId, WM_QUIT, 0, 0);
  const bool quit =
      WaitForSingleObject(scope.state->quitEntered, 5000) == WAIT_OBJECT_0;
  if (!quit) SetEvent(scope.state->quitGate);
  audioRequire(quit,
               "Windows audio fixture did not reach the pending thread exit");
  std::atomic<bool> closed{false};
  std::thread closer([owner, state = scope.state, &closed]() {
    activeWaveState = state;
    closed          = owner->doCloseDevice();
    activeWaveState.reset();
  });
  const bool accepted =
      WaitForSingleObject(scope.state->barrierAccepted, 5000) == WAIT_OBJECT_0;
  SetEvent(scope.state->quitGate);
  closer.join();
  audioRequire(
      accepted && closed,
      "Windows audio lost a barrier accepted immediately before worker exit");
  owner->shutdown();
  owner.reset();
  scope.balanced();
}

}  // namespace

bool runWindowsSoundOutputOwnershipChecks(QString& failure) {
  try {
    checkAudioStartup();
    checkAudioDelayedStartup();
    checkAudioPlayback();
    checkAudioCompletion();
    checkAudioRegistrationRetirement();
    checkAudioFailureRecovery();
    checkAudioScrubbing();
    checkAudioScrubPending();
    checkAudioLateCompletion();
    for (int kind = 0; kind < 3; ++kind) checkAudioRetainedFailure(kind);
    checkAudioPostFailure();
    checkAudioUnexpectedExit();
    checkAudioExitBarrierRace();
    checkAudioAcceptedExitBarrier();
    checkAudioConcurrentDetach(
        [](const std::shared_ptr<TSoundOutputDeviceImp>& owner) {
          std::lock_guard<std::recursive_mutex> lock(owner->m_mutex);
          return owner->m_listeners.empty();
        });
    failure.clear();
    return true;
  } catch (const TException& error) {
    failure = QString::fromStdWString(error.getMessage());
  } catch (const std::exception& error) {
    failure = QString::fromUtf8(error.what());
  }
  return false;
}
