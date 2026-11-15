

#include "tsound.h"

#ifndef TNZCORE_LIGHT
#include "tthread.h"
#endif
#include "tsop.h"
#include <set>
#include <memory>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <limits>
#include "tsystem.h"

#include <mmsystem.h>

#ifdef BUILD_UI_PORT_TESTS
#include "tsoundwindowsownershiptest.h"
#endif

//=========================================================

// forward declarations
class TSoundOutputDeviceImp;
class TSoundInputDeviceImp;

//=========================================================

namespace {
void CALLBACK recordCB(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
                       DWORD_PTR dwParam1, DWORD dwParam2);

bool setRecordLine(TSoundInputDevice::Source typeInput);

MMRESULT getLineInfo(HMIXEROBJ hMixer, DWORD dwComponentType, MIXERLINE& mxl);

MMRESULT getLineControl(MIXERCONTROL& mxc, HMIXEROBJ hMixer, DWORD dwLineID,
                        DWORD dwControlType);

MMRESULT setControlDetails(HMIXEROBJ hMixer, DWORD dwSelectControlID,
                           DWORD dwMultipleItems,
                           MIXERCONTROLDETAILS_UNSIGNED* mxcdSelectValue);

MMRESULT getControlDetails(HMIXEROBJ hMixer, DWORD dwSelectControlID,
                           DWORD dwMultipleItems,
                           MIXERCONTROLDETAILS_UNSIGNED* mxcdSelectValue);

MMRESULT isaFormatSupported(int sampleRate, int channelCount, int bitPerSample,
                            int sampleType, bool input);

void getAmplitude(int& amplitude, const TSoundTrackP st, TINT32 sample);
}  // namespace

namespace {

struct WaveOutputApi {
  decltype(&waveOutOpen) open                 = waveOutOpen;
  decltype(&waveOutClose) close               = waveOutClose;
  decltype(&waveOutPrepareHeader) prepare     = waveOutPrepareHeader;
  decltype(&waveOutUnprepareHeader) unprepare = waveOutUnprepareHeader;
  decltype(&waveOutWrite) write               = waveOutWrite;
  decltype(&waveOutReset) reset               = waveOutReset;
  decltype(&CreateThread) createThread        = CreateThread;
  decltype(&PostThreadMessage) postMessage    = PostThreadMessage;
  decltype(&PeekMessage) peekMessage          = PeekMessage;
  decltype(&GetMessage) getMessage            = GetMessage;
};

#ifdef BUILD_UI_PORT_TESTS
thread_local const WaveOutputApi* testWaveOutputApi = nullptr;
#endif

WaveOutputApi outputApi() {
#ifdef BUILD_UI_PORT_TESTS
  if (testWaveOutputApi) return *testWaveOutputApi;
#endif
  return {};
}

std::atomic<int> failedWaveOutputs{0};
constexpr UINT waveBarrierMessage = WM_APP + 0x431;

struct WaveBarrier {
  HANDLE event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  ~WaveBarrier() {
    if (event) CloseHandle(event);
  }
};

struct SoundRegistration {
  std::recursive_mutex mutex;
  TSoundOutputDeviceListener* listener;
  explicit SoundRegistration(TSoundOutputDeviceListener* value)
      : listener(value) {}
};

#ifndef TNZCORE_LIGHT
class EndPlayMsg final : public TThread::Message {
  std::weak_ptr<SoundRegistration> m_registration;

public:
  explicit EndPlayMsg(const std::shared_ptr<SoundRegistration>& registration)
      : m_registration(registration) {}
  TThread::Message* clone() const override { return new EndPlayMsg(*this); }
  void onDeliver() override {
    if (auto registration = m_registration.lock()) {
      std::lock_guard<std::recursive_mutex> lock(registration->mutex);
      if (auto* listener = registration->listener) listener->onPlayCompleted();
    }
  }
};
#endif

void getAmplitude(int& amplitude, const TSoundTrackP track, TINT32 sample) {
  amplitude = 0;
  for (int channel = 0; channel < track->getChannelCount(); ++channel)
    amplitude += int(track->getPressure(sample, channel));
  amplitude /= track->getChannelCount();
}

}  // namespace

class TSoundOutputDeviceImp
    : public std::enable_shared_from_this<TSoundOutputDeviceImp> {
  enum class BufferState { Pending, Submitted, Retiring };
  enum class BarrierPost { Posted, ThreadExited, Failed };
  struct Buffer {
    WAVEHDR header{};
    std::vector<char> bytes;
    TSoundTrackP track;
    BufferState state      = BufferState::Pending;
    std::uint64_t sequence = 0;
    bool prepared          = false;
  };

  WaveOutputApi m_api;
  std::recursive_mutex m_operations;
  std::map<WAVEHDR*, std::unique_ptr<Buffer>> m_buffers;
  std::map<TSoundOutputDeviceListener*, std::shared_ptr<SoundRegistration>>
      m_listeners;
  TSoundTrackP m_lastTrack;
  std::uint64_t m_sequence = 0;
  int m_lastOffset         = 0;
  HWAVEOUT m_wout          = nullptr;
  HANDLE m_thread          = nullptr;
  HANDLE m_ready           = nullptr;
  DWORD m_threadId         = 0;
  bool m_stopped           = true;
  bool m_isPlaying         = false;
  bool m_looped            = false;
  bool m_scrubbing         = false;
  bool m_shutdown          = false;
  bool m_failedCleanup     = false;
  bool m_threadExiting     = false;
  std::shared_ptr<TSoundOutputDeviceImp> m_failedOwner;
  TSoundTrackFormat m_currentFormat;

  void failCleanup(const char* operation) {
    if (!m_failedCleanup) {
      m_failedCleanup = true;
      ++failedWaveOutputs;
      std::fprintf(stderr,
                   "Windows audio %s failed; retained device buffers safely. "
                   "Playback is disabled until cleanup succeeds; restart if "
                   "the device does not recover.\n",
                   operation);
    }
  }

  void cleanupRecovered() {
    if (m_failedCleanup) {
      m_failedCleanup = false;
      --failedWaveOutputs;
    }
    m_failedOwner.reset();
  }

  bool startThread() {
    if (m_thread) return WaitForSingleObject(m_thread, 0) == WAIT_TIMEOUT;
    if (m_ready) CloseHandle(m_ready);
    m_ready = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!m_ready) return false;
    auto owner = std::make_unique<std::shared_ptr<TSoundOutputDeviceImp>>(
        shared_from_this());
    m_thread =
        m_api.createThread(nullptr, 0, threadMain, owner.get(), 0, &m_threadId);
    if (!m_thread) return false;
    owner.release();
    const HANDLE handles[] = {m_ready, m_thread};
    return WaitForMultipleObjects(2, handles, FALSE, INFINITE) ==
               WAIT_OBJECT_0 &&
           WaitForSingleObject(m_thread, 0) == WAIT_TIMEOUT;
  }

  BarrierPost postBarrier(const std::shared_ptr<WaveBarrier>& request) {
    auto message = std::make_unique<std::shared_ptr<WaveBarrier>>(request);
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_threadExiting) return BarrierPost::ThreadExited;
    if (WaitForSingleObject(m_thread, 0) != WAIT_TIMEOUT)
      return BarrierPost::ThreadExited;
    if (!m_api.postMessage(m_threadId, waveBarrierMessage, 0,
                           LPARAM(message.get())))
      return BarrierPost::Failed;
    message.release();
    return BarrierPost::Posted;
  }

  bool barrier() try {
    if (!m_thread || GetCurrentThreadId() == m_threadId) return true;
    auto request = std::make_shared<WaveBarrier>();
    if (!request->event) return false;
    const auto posted = postBarrier(request);
    if (posted == BarrierPost::Failed) return false;
    if (posted == BarrierPost::ThreadExited)
      return WaitForSingleObject(m_thread, INFINITE) == WAIT_OBJECT_0;
    const HANDLE handles[] = {request->event, m_thread};
    const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    return result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1;
  } catch (...) {
    return false;
  }

  void processMessage(const MSG& message) {
    try {
      if (message.message == waveBarrierMessage) {
        std::unique_ptr<std::shared_ptr<WaveBarrier>> request(
            reinterpret_cast<std::shared_ptr<WaveBarrier>*>(message.lParam));
        {
          std::lock_guard<std::recursive_mutex> lock(m_mutex);
          retireReturned();
        }
        SetEvent((*request)->event);
      } else if (message.message == MM_WOM_DONE) {
        completed(reinterpret_cast<HWAVEOUT>(message.wParam),
                  reinterpret_cast<WAVEHDR*>(message.lParam));
      }
    } catch (...) {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      m_shutdown = true;
      resetBuffers();
      failCleanup("callback");
    }
  }

  void retireThread(const std::shared_ptr<TSoundOutputDeviceImp>& owner) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_threadExiting = true;
    MSG message{};
    // Posting and final draining use the same lock, so no request can be
    // accepted after this thread's last drain or target a recycled thread ID.
    while (m_api.peekMessage(&message, nullptr, waveBarrierMessage,
                             waveBarrierMessage, PM_REMOVE)) {
      std::unique_ptr<std::shared_ptr<WaveBarrier>> request(
          reinterpret_cast<std::shared_ptr<WaveBarrier>*>(message.lParam));
      SetEvent((*request)->event);
    }
    if (!m_buffers.empty() || m_wout) {
      resetBuffers();
      if (!closeReturned()) {
        failCleanup("callback thread exit");
        // The callback thread is gone, so no further completion can release
        // device-owned storage. Keep this disabled owner for process teardown.
        m_failedOwner = owner;
      }
    }
  }

  static DWORD WINAPI threadMain(void* argument) {
    std::unique_ptr<std::shared_ptr<TSoundOutputDeviceImp>> argumentOwner(
        static_cast<std::shared_ptr<TSoundOutputDeviceImp>*>(argument));
    auto owner = std::move(*argumentOwner);
    argumentOwner.reset();
    MSG message{};
    owner->m_api.peekMessage(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(owner->m_ready);
    while (owner->m_api.getMessage(&message, nullptr, 0, 0) > 0) {
      owner->processMessage(message);
      std::lock_guard<std::recursive_mutex> lock(owner->m_mutex);
      if (owner->m_shutdown && owner->closeReturned()) break;
    }
    owner->retireThread(owner);
    return 0;
  }

  size_t count(BufferState state) const {
    size_t result = 0;
    for (const auto& entry : m_buffers)
      if (entry.second->state == state) ++result;
    return result;
  }

  Buffer* oldestPending() {
    Buffer* result = nullptr;
    for (const auto& entry : m_buffers) {
      Buffer* buffer = entry.second.get();
      if (buffer->state == BufferState::Pending &&
          (!result || buffer->sequence < result->sequence))
        result = buffer;
    }
    return result;
  }

  bool retire(Buffer& buffer) {
    if (buffer.prepared &&
        m_api.unprepare(m_wout, &buffer.header, sizeof(WAVEHDR)) !=
            MMSYSERR_NOERROR) {
      buffer.state = BufferState::Retiring;
      return false;
    }
    m_buffers.erase(&buffer.header);
    return true;
  }

  void retireReturned() {
    for (auto it = m_buffers.begin(); it != m_buffers.end();) {
      Buffer& buffer = *it++->second;
      if (buffer.state == BufferState::Retiring &&
          !(buffer.header.dwFlags & WHDR_INQUEUE))
        retire(buffer);
    }
  }

  bool resetBuffers() {
    m_stopped   = true;
    m_looped    = false;
    m_isPlaying = false;
    for (auto& entry : m_buffers) entry.second->state = BufferState::Retiring;
    m_lastTrack  = TSoundTrackP();
    m_lastOffset = 0;
    if (m_wout && m_api.reset(m_wout) != MMSYSERR_NOERROR) {
      failCleanup("reset");
      return false;
    }
    return true;
  }

  bool closeReturned() {
    retireReturned();
    if (!m_buffers.empty()) return false;
    if (m_wout && m_api.close(m_wout) != MMSYSERR_NOERROR) return false;
    m_wout      = nullptr;
    m_isPlaying = false;
    cleanupRecovered();
    return true;
  }

  Buffer* prepare(const TSoundTrackP& track) {
    auto storage   = std::make_unique<Buffer>();
    storage->track = track;
    const size_t bytes =
        size_t(track->getSampleSize()) * track->getSampleCount();
    if (bytes > (std::numeric_limits<DWORD>::max)())
      throw TSoundDeviceException(
          TSoundDeviceException::UnablePrepare,
          "Windows audio playback buffer exceeds the device size limit");
    storage->bytes.resize(bytes);
    std::memcpy(storage->bytes.data(), track->getRawData(),
                storage->bytes.size());
    storage->header.lpData         = storage->bytes.data();
    storage->header.dwBufferLength = DWORD(storage->bytes.size());
    storage->sequence              = ++m_sequence;
    auto* buffer                   = storage.get();
    m_buffers.emplace(&buffer->header, std::move(storage));
    if (m_api.prepare(m_wout, &buffer->header, sizeof(WAVEHDR)) !=
        MMSYSERR_NOERROR) {
      m_buffers.erase(&buffer->header);
      throw TSoundDeviceException(
          TSoundDeviceException::UnablePrepare,
          "Windows audio could not prepare a playback buffer");
    }
    buffer->prepared = true;
    return buffer;
  }

  void submit(Buffer& buffer) {
    if (m_api.write(m_wout, &buffer.header, sizeof(WAVEHDR)) !=
        MMSYSERR_NOERROR) {
      if (!retire(buffer)) failCleanup("buffer release after failed write");
      throw TSoundDeviceException(
          TSoundDeviceException::UnablePrepare,
          "Windows audio could not queue a playback buffer");
    }
    buffer.state = BufferState::Submitted;
    m_lastTrack  = buffer.track;
    getAmplitude(m_lastOffset, buffer.track,
                 buffer.track->getSampleCount() - 1);
    m_isPlaying = true;
    m_stopped   = false;
  }

  void submit(const TSoundTrackP& track) { submit(*prepare(track)); }

  void finishOffset(const TSoundTrackP& track) {
    m_lastOffset = track->isSampleSigned() ? 0 : 127;
  }

  void submitScrub(const TSoundTrackP& track) {
    const bool roomForDecay = count(BufferState::Submitted) <= 7;
    Buffer* buffer          = prepare(track);
    if (m_lastOffset) {
      int offset = 0;
      getAmplitude(offset, track, 0);
      if (m_lastOffset != offset) {
        TSoundTrackP faded = TSop::crossFade(m_lastTrack, track, 0.3);
        const size_t bytes =
            size_t(faded->getSampleCount()) * faded->getSampleSize();
        std::memcpy(buffer->bytes.data(), faded->getRawData(),
                    std::min(bytes, buffer->bytes.size()));
      }
    } else {
      submit(TSop::fadeIn(track, 0.3));
    }
    submit(*buffer);
    if (roomForDecay && m_lastOffset) {
      submit(TSop::fadeOut(track, 0.3));
      finishOffset(track);
    }
  }

  void put(const TSoundTrackP& track) {
    if (!m_scrubbing) {
      submit(track);
      return;
    }
    if (count(BufferState::Submitted) == 0) {
      submit(TSop::fadeIn(track, 0.9));
      submit(track);
      if (m_lastOffset) {
        submit(TSop::fadeOut(track, 0.9));
        finishOffset(track);
      }
      return;
    }
    if (count(BufferState::Submitted) < 10) {
      submitScrub(track);
      return;
    }
    queuePending(track);
  }

  void queuePending(const TSoundTrackP& track) {
    if (count(BufferState::Pending) == 4) {
      if (!retire(*oldestPending())) {
        failCleanup("pending buffer release");
        throw TSoundDeviceException(
            TSoundDeviceException::UnablePrepare,
            "Windows audio could not retire a pending buffer");
      }
      if (Buffer* next = oldestPending()) {
        int offset = 0;
        getAmplitude(offset, next->track, 0);
        if (m_lastOffset != offset) {
          TSoundTrackP faded = TSop::crossFade(m_lastTrack, next->track, 0.3);
          const size_t bytes =
              size_t(faded->getSampleCount()) * faded->getSampleSize();
          std::memcpy(next->bytes.data(), faded->getRawData(),
                      std::min(bytes, next->bytes.size()));
        }
      }
    }
    prepare(track);
  }

  Buffer* returnedBuffer(HWAVEOUT device, WAVEHDR* header) {
    if (device != m_wout) return nullptr;
    const auto found = m_buffers.find(header);
    if (found == m_buffers.end()) return nullptr;
    if (!(header->dwFlags & WHDR_DONE)) return nullptr;
    if (header->dwFlags & WHDR_INQUEUE) return nullptr;
    return found->second.get();
  }

  bool repeat(Buffer& buffer) {
    if (buffer.state != BufferState::Submitted || !m_looped || m_stopped)
      return false;
    submit(buffer);
    return true;
  }

  void completed(HWAVEOUT device, WAVEHDR* header) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    Buffer* returned = returnedBuffer(device, header);
    if (!returned) return;
    Buffer& buffer = *returned;
    if (repeat(buffer)) return;
    const bool active = buffer.state == BufferState::Submitted;
    if (!retire(buffer)) {
      resetBuffers();
      failCleanup("completed buffer release");
      return;
    }
    if (!m_stopped) {
      if (Buffer* next = oldestPending()) submit(*next);
    }
    if (active) finishPlayback();
  }

  void finishPlayback() {
    if (count(BufferState::Submitted) || count(BufferState::Pending)) return;
    m_isPlaying = false;
    m_lastTrack = TSoundTrackP();
#ifndef TNZCORE_LIGHT
    if (!m_stopped) {
      for (const auto& entry : m_listeners) {
        EndPlayMsg message(entry.second);
        message.send();
      }
    }
#endif
    m_stopped = true;
    if (!closeReturned()) failCleanup("completed device close");
  }

  bool unavailable() const {
    return m_shutdown || m_failedCleanup || m_threadExiting;
  }

  bool openDeviceFormat(const TSoundTrackFormat& format) {
    WAVEFORMATEX wave{};
    wave.wFormatTag      = format.m_sampleType & TSound::WMASK;
    wave.nChannels       = format.m_channelCount;
    wave.nSamplesPerSec  = format.m_sampleRate;
    wave.wBitsPerSample  = format.m_bitPerSample;
    wave.nBlockAlign     = (wave.nChannels * wave.wBitsPerSample) >> 3;
    wave.nAvgBytesPerSec = wave.nSamplesPerSec * wave.nBlockAlign;
    const MMRESULT result =
        m_api.open(&m_wout, WAVE_MAPPER, &wave, m_threadId, 0, CALLBACK_THREAD);
    if (result != MMSYSERR_NOERROR) {
      m_wout = nullptr;
      return false;
    }
    m_currentFormat = format;
    return true;
  }

  void checkRetirement() {
    if (count(BufferState::Retiring))
      failCleanup("returned buffer release");
    else if (m_failedCleanup && !closeReturned())
      failCleanup("returned device close");
  }

public:
  std::recursive_mutex m_mutex;
  std::set<int> m_supportedRate;

  TSoundOutputDeviceImp() : m_api(outputApi()) {
    if (failedWaveOutputs.load())
      throw TSoundDeviceException(
          TSoundDeviceException::FailedInit,
          "Windows audio is waiting for failed device cleanup");
    for (int rate :
         {8000, 11025, 16000, 22050, 32000, 44100, 48000, 96000, 192000}) {
      WAVEFORMATEX format{};
      format.wFormatTag      = WAVE_FORMAT_PCM;
      format.nChannels       = 1;
      format.nSamplesPerSec  = rate;
      format.wBitsPerSample  = 8;
      format.nBlockAlign     = 1;
      format.nAvgBytesPerSec = rate;
      const MMRESULT result =
          m_api.open(nullptr, WAVE_MAPPER, &format, 0, 0, WAVE_FORMAT_QUERY);
      if (result == MMSYSERR_NOERROR)
        m_supportedRate.insert(rate);
      else if (result != WAVERR_BADFORMAT)
        throw TSoundDeviceException(
            TSoundDeviceException::FailedInit,
            "Unable to query supported Windows audio rates");
    }
    if (m_supportedRate.empty())
      throw TSoundDeviceException(
          TSoundDeviceException::FailedInit,
          "Unable to verify supported Windows audio rates");
  }

  ~TSoundOutputDeviceImp() {
    assert(m_buffers.empty());
    if (m_thread) CloseHandle(m_thread);
    if (m_ready) CloseHandle(m_ready);
    cleanupRecovered();
  }

  bool doOpenDevice(const TSoundTrackFormat& format) {
    std::lock_guard<std::recursive_mutex> operation(m_operations);
    {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      if (unavailable()) return false;
      if (m_wout && m_currentFormat == format) return true;
    }
    if (!doCloseDevice() || !startThread()) return false;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (unavailable() || WaitForSingleObject(m_thread, 0) != WAIT_TIMEOUT)
      return false;
    return openDeviceFormat(format);
  }

  bool doCloseDevice() {
    std::lock_guard<std::recursive_mutex> operation(m_operations);
    {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      if (!resetBuffers()) return false;
    }
    if (!barrier()) {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      failCleanup("close completion barrier");
      return false;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (closeReturned()) return true;
    failCleanup("close");
    return false;
  }

  void stop() {
    std::lock_guard<std::recursive_mutex> operation(m_operations);
    {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      if (!resetBuffers()) return;
    }
    if (!barrier()) {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      failCleanup("completion barrier");
      return;
    }
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    checkRetirement();
  }

  void shutdown() {
    decltype(m_listeners) listeners;
    {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      m_shutdown = true;
      m_stopped  = true;
      m_looped   = false;
      listeners.swap(m_listeners);
    }
    for (const auto& entry : listeners) {
      std::lock_guard<std::recursive_mutex> lock(entry.second->mutex);
      entry.second->listener = nullptr;
    }
    std::lock_guard<std::recursive_mutex> operation(m_operations);
    const bool closed = doCloseDevice();
    {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      m_shutdown = true;
      if (!closed) failCleanup("shutdown");
    }
    if (!m_thread) return;
    const bool delivered = barrier();
    if (!delivered) {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      failCleanup("shutdown barrier");
      return;
    }
    if (closed && GetCurrentThreadId() != m_threadId)
      WaitForSingleObject(m_thread, INFINITE);
  }

  void play(const TSoundTrackP& track, bool loop, bool scrubbing) {
    std::lock_guard<std::recursive_mutex> operation(m_operations);
    bool pendingRetirement = false;
    {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      if (m_looped)
        throw TSoundDeviceException(
            TSoundDeviceException::Busy,
            "Unable to queue playback while the sound player is looping");
      pendingRetirement = count(BufferState::Retiring) != 0;
    }
    if (pendingRetirement) {
      if (!barrier())
        throw TSoundDeviceException(
            TSoundDeviceException::UnableCloseDevice,
            "Windows audio could not drain returned playback buffers");
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      checkRetirement();
    }
    if (!doOpenDevice(track->getFormat()))
      throw TSoundDeviceException(
          TSoundDeviceException::UnableOpenDevice,
          "Windows audio could not open the playback device");
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_looped    = loop;
    m_scrubbing = scrubbing;
    try {
      put(track);
    } catch (...) {
      resetBuffers();
      throw;
    }
  }

  void attach(TSoundOutputDeviceListener* listener) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_shutdown && !m_listeners.count(listener))
      m_listeners.emplace(listener,
                          std::make_shared<SoundRegistration>(listener));
  }

  void detach(TSoundOutputDeviceListener* listener) {
    std::shared_ptr<SoundRegistration> registration;
    {
      std::lock_guard<std::recursive_mutex> lock(m_mutex);
      const auto found = m_listeners.find(listener);
      if (found == m_listeners.end()) return;
      registration = found->second;
      m_listeners.erase(found);
    }
    std::lock_guard<std::recursive_mutex> lock(registration->mutex);
    registration->listener = nullptr;
  }

  bool isPlaying() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_isPlaying;
  }
  bool isLooping() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_looped;
  }
  void setLooping(bool value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_looped = value;
  }
  bool allPlayed() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& entry : m_buffers)
      if (entry.second->state == BufferState::Submitted &&
          !(entry.first->dwFlags & WHDR_DONE))
        return false;
    return true;
  }

#ifdef BUILD_UI_PORT_TESTS
  friend bool runWindowsSoundOutputOwnershipChecks(QString& failure);
#endif
};

TSoundOutputDevice::TSoundOutputDevice()
    : m_imp(std::make_shared<TSoundOutputDeviceImp>()) {}
TSoundOutputDevice::~TSoundOutputDevice() { m_imp->shutdown(); }
bool TSoundOutputDevice::installed() { return waveOutGetNumDevs() > 0; }
bool TSoundOutputDevice::open(const TSoundTrackP& track) {
  return m_imp->doOpenDevice(track->getFormat());
}
bool TSoundOutputDevice::close() { return m_imp->doCloseDevice(); }
void TSoundOutputDevice::stop() { m_imp->stop(); }
void TSoundOutputDevice::attach(TSoundOutputDeviceListener* listener) {
  m_imp->attach(listener);
}
void TSoundOutputDevice::detach(TSoundOutputDeviceListener* listener) {
  m_imp->detach(listener);
}
bool TSoundOutputDevice::isPlaying() const { return m_imp->isPlaying(); }
bool TSoundOutputDevice::isAllQueuedItemsPlayed() { return m_imp->allPlayed(); }
bool TSoundOutputDevice::isLooping() { return m_imp->isLooping(); }
void TSoundOutputDevice::setLooping(bool loop) { m_imp->setLooping(loop); }

void TSoundOutputDevice::play(const TSoundTrackP& track, TINT32 first,
                              TINT32 last, bool loop, bool scrubbing) {
  if (!track || !track->getSampleCount()) return;
  if (scrubbing && loop)
    throw TSoundDeviceException(TSoundDeviceException::Busy,
                                "Windows audio cannot loop a scrub sample");
  if (getPreferredFormat(track->getFormat()) != track->getFormat())
    throw TSoundDeviceException(TSoundDeviceException::UnsupportedFormat,
                                "Unsupported Format");
  TSoundTrackP part = track->extract(first, last);
  if (part && part->getSampleCount()) m_imp->play(part, loop, scrubbing);
}

TSoundTrackFormat TSoundOutputDevice::getPreferredFormat(TUINT32 sampleRate,
                                                         int channelCount,
                                                         int bitPerSample) {
  TSoundTrackFormat fmt;

  // avvvicinarsi al sample rate => dovrebbe esser OK avendo selezionato i piu'
  // vicini
  std::set<int>::iterator it = m_imp->m_supportedRate.lower_bound(sampleRate);
  if (it == m_imp->m_supportedRate.end()) {
    it = std::max_element(m_imp->m_supportedRate.begin(),
                          m_imp->m_supportedRate.end());
    if (it != m_imp->m_supportedRate.end())
      sampleRate = *(m_imp->m_supportedRate.rbegin());
    else
      throw TSoundDeviceException(TSoundDeviceException::UnsupportedFormat,
                                  "There isn't a supported format");
  } else
    sampleRate = *it;

  // Normalize bit sample and set preferred sample type
  if (bitPerSample <= 8) {
    bitPerSample     = 8;
    fmt.m_sampleType = TSound::UINT;
  } else if (bitPerSample <= 16) {
    bitPerSample     = 16;
    fmt.m_sampleType = TSound::INT;
  } else if (bitPerSample <= 24) {
    bitPerSample     = 24;
    fmt.m_sampleType = TSound::INT;
  } else if (bitPerSample <= 32) {
    bitPerSample     = 32;
    fmt.m_sampleType = TSound::FLOAT;
  }

  // switch mono/stereo
  if (channelCount <= 1)
    channelCount = 1;
  else
    channelCount = 2;

  fmt.m_bitPerSample = bitPerSample;
  fmt.m_channelCount = channelCount;
  fmt.m_sampleRate   = sampleRate;

  return fmt;
}

//------------------------------------------------------------------------------

TSoundTrackFormat TSoundOutputDevice::getPreferredFormat(
    const TSoundTrackFormat &format) {
  try {
    return getPreferredFormat(format.m_sampleRate, format.m_channelCount,
                              format.m_bitPerSample);
  } catch (TSoundDeviceException &e) {
    throw TSoundDeviceException(TSoundDeviceException::UnsupportedFormat,
                                e.getMessage());
  }
}

#ifdef BUILD_UI_PORT_TESTS
#include "tsoundwindowsownershiptest.inc"
#endif

//==============================================================================
//==============================================================================
// Classi per la gestione della registrazione
//==============================================================================
//==============================================================================

//==============================================================================

class WaveFormat final : public WAVEFORMATEX {
public:
  WaveFormat(){};
  WaveFormat(unsigned char channelCount, TUINT32 sampleRate,
             unsigned char bitPerSample, int sampleFormat);

  ~WaveFormat(){};
};

WaveFormat::WaveFormat(unsigned char channelCount, TUINT32 sampleRate,
                       unsigned char bitPerSample, int sampleFormat) {
  wFormatTag      = sampleFormat & TSound::WMASK;
  nChannels       = channelCount;
  nSamplesPerSec  = sampleRate;
  wBitsPerSample  = bitPerSample;
  nBlockAlign     = (channelCount * bitPerSample) >> 3;
  nAvgBytesPerSec = nBlockAlign * sampleRate;
  cbSize          = 0;
}

//==============================================================================

class WinSoundInputDevice {
public:
  WinSoundInputDevice();
  ~WinSoundInputDevice();

  void open(const WaveFormat &wf);
  void close();

  void prepareHeader(char *sampleBuffer, TUINT32 sampleBufferSize,
                     WAVEHDR &whdr);

  void unprepareHeader(WAVEHDR &whdr);

  void addBlock(WAVEHDR &whdr);

  void start();

  void reset();
  void stop();

  HANDLE m_hBlockDone;

private:
  HWAVEIN m_hWaveIn;
};

//--------------------------------------------------------------------

WinSoundInputDevice::WinSoundInputDevice() : m_hWaveIn(0) {
  m_hBlockDone = CreateEvent(NULL,   // no security attributes
                             FALSE,  // auto-reset event
                             FALSE,  // initial state is not signaled
                             NULL);  // object not named
}

//--------------------------------------------------------------------

WinSoundInputDevice::~WinSoundInputDevice() { CloseHandle(m_hBlockDone); }

//--------------------------------------------------------------------

void WinSoundInputDevice::open(const WaveFormat &wf) {
  if (m_hWaveIn) close();

  MMRESULT ret = waveInOpen(&m_hWaveIn, WAVE_MAPPER, &wf, (DWORD_PTR)recordCB,
                            (DWORD_PTR)m_hBlockDone, CALLBACK_FUNCTION);

  if (ret != MMSYSERR_NOERROR) {
    throw TException("Error to open the input device");
  }
}

//--------------------------------------------------------------------

void WinSoundInputDevice::close() {
  if (!m_hWaveIn) return;

  MMRESULT ret = waveInClose(m_hWaveIn);

  if (ret != MMSYSERR_NOERROR) {
    throw TException("Error to close the input device");
  }
  m_hWaveIn = 0;
}

//--------------------------------------------------------------------

void WinSoundInputDevice::prepareHeader(char *sampleBuffer,
                                        TUINT32 sampleBufferSize,
                                        WAVEHDR &whdr) {
  whdr.lpData         = sampleBuffer;
  whdr.dwBufferLength = sampleBufferSize;  // numero di byte
  whdr.dwFlags        = 0;
  whdr.dwLoops        = 0;

  MMRESULT ret = waveInPrepareHeader(m_hWaveIn, &whdr, sizeof(WAVEHDR));
  if (ret != MMSYSERR_NOERROR) {
    throw TException("Unable to prepare a wave header");
  }
}

//--------------------------------------------------------------------

void WinSoundInputDevice::unprepareHeader(WAVEHDR &whdr) {
  MMRESULT ret = waveInUnprepareHeader(m_hWaveIn, &whdr, sizeof(WAVEHDR));
  if (ret != MMSYSERR_NOERROR) {
    throw TException("Unable to unprepare a wave header");
  }
}

//--------------------------------------------------------------------

void WinSoundInputDevice::addBlock(WAVEHDR &whdr) {
  MMRESULT ret = waveInAddBuffer(m_hWaveIn, &whdr, sizeof(WAVEHDR));
  if (ret != MMSYSERR_NOERROR) {
    throw TException("Unable to add a waveheader");
  }
}

//--------------------------------------------------------------------

void WinSoundInputDevice::start() {
  int ret = waveInStart(m_hWaveIn);
  if (ret != MMSYSERR_NOERROR) {
    throw TException("Unable to add a waveheader");
  }
}

//--------------------------------------------------------------------

void WinSoundInputDevice::reset() {
  if (!m_hWaveIn) return;

  MMRESULT ret = waveInReset(m_hWaveIn);
  if (ret != MMSYSERR_NOERROR) {
    throw TException("Unable to add a waveheader");
  }
}

//--------------------------------------------------------------------

void WinSoundInputDevice::stop() {
  MMRESULT ret = waveInStop(m_hWaveIn);
  if (ret != MMSYSERR_NOERROR) {
    throw TException("Unable to add a waveheader");
  }
}

//====================================================================

#ifndef TNZCORE_LIGHT
class RecordTask final : public TThread::Runnable {
public:
  RecordTask(std::shared_ptr<TSoundInputDeviceImp> dev)
      : Runnable(), m_dev(std::move(dev)) {}

  ~RecordTask() {}

  void run() override;

  std::shared_ptr<TSoundInputDeviceImp> m_dev;
};

#endif

//====================================================================

class TSoundInputDeviceImp final : public WinSoundInputDevice {
public:
  bool m_allocateBuff;
  bool m_isRecording;
  bool m_supportVolume;
  int m_index;
  TINT32 m_byteRecorded;

  TSoundTrackP m_st;
  TSoundTrackFormat m_format;

#ifndef TNZCORE_LIGHT
  TThread::Executor m_executor;
#endif
  std::vector<WAVEHDR> m_whdr;
  std::vector<char *> m_recordedBlocks;
  std::set<int> m_supportedRate;

  HANDLE m_hLastBlockDone;

  TSoundInputDeviceImp();
  ~TSoundInputDeviceImp();

  void insertAllRate();
  bool verifyRate();
};

//------------------------------------------------------------------------------

TSoundInputDeviceImp::TSoundInputDeviceImp()
    : m_allocateBuff(false)
    , m_isRecording(false)
    , m_supportVolume(false)
    , m_index(0)
    , m_byteRecorded(0)
    , m_format()
    , m_whdr(3)
    , m_recordedBlocks()
    , m_supportedRate() {
  m_hLastBlockDone = CreateEvent(NULL,   // no security attributes
                                 FALSE,  // is manual-reset event?
                                 FALSE,  // is signaled?
                                 NULL);  // object not named
}

//------------------------------------------------------------------------------

TSoundInputDeviceImp::~TSoundInputDeviceImp() {
  if (m_isRecording) {
    try {
      reset();
      WaitForSingleObject(m_hLastBlockDone, INFINITE);
      int i;
      for (i = 0; i < (int)m_recordedBlocks.size(); ++i)
        delete[] m_recordedBlocks[i];
      close();
    } catch (TException &) {
    }
  }
  CloseHandle(m_hLastBlockDone);
}

//----------------------------------------------------------------------------

void TSoundInputDeviceImp::insertAllRate() {
  m_supportedRate.insert(8000);
  m_supportedRate.insert(11025);
  m_supportedRate.insert(16000);
  m_supportedRate.insert(22050);
  m_supportedRate.insert(32000);
  m_supportedRate.insert(44100);
  m_supportedRate.insert(48000);
  m_supportedRate.insert(96000);
  m_supportedRate.insert(192000);
}

//----------------------------------------------------------------------------

bool TSoundInputDeviceImp::verifyRate() {
  std::set<int>::iterator it;

  for (it = m_supportedRate.begin(); it != m_supportedRate.end();) {
    MMRESULT ret;
    WAVEFORMATEX wf;

    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = *it;
    wf.wBitsPerSample  = 8;
    wf.nBlockAlign     = (wf.nChannels * wf.wBitsPerSample) >> 3;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize          = 0;

    ret = waveInOpen(NULL, WAVE_MAPPER, &wf, NULL, NULL, WAVE_FORMAT_QUERY);

    if (ret == MMSYSERR_NOERROR) {
      ++it;
      continue;
    }
    if (ret == WAVERR_BADFORMAT)
      it = m_supportedRate.erase(it);
    else
      return false;
  }
  if (m_supportedRate.end() == m_supportedRate.begin()) return false;

  return true;
}

//====================================================================
namespace {
void CALLBACK recordCB(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
                       DWORD_PTR dwParam1, DWORD dwParam2) {
  WAVEHDR *whdr     = (WAVEHDR *)dwParam1;
  HANDLE *blockDone = (HANDLE *)dwInstance;

  if (uMsg != MM_WIM_DATA) return;
  SetEvent(blockDone);
}
}  // namespace

//==============================================================================

TSoundInputDevice::TSoundInputDevice() : m_imp(new TSoundInputDeviceImp()) {
  m_imp->insertAllRate();
  if (!m_imp->verifyRate())
    throw TSoundDeviceException(TSoundDeviceException::FailedInit,
                                "Unable to verify supported rates");
  if (supportsVolume()) m_imp->m_supportVolume = true;
}

//------------------------------------------------------------------------------

TSoundInputDevice::~TSoundInputDevice() {}

//------------------------------------------------------------------------------

bool TSoundInputDevice::installed() {
  int ndev = waveInGetNumDevs();
  if (ndev <= 0) return false;
  return true;
}

//------------------------------------------------------------------------------

#ifndef TNZCORE_LIGHT

void TSoundInputDevice::record(const TSoundTrackFormat &format,
                               Source devtype) {
  if (m_imp->m_isRecording)
    throw TSoundDeviceException(TSoundDeviceException::Busy,
                                "Just another recoding is in progress");

  /*if ((format.m_bitPerSample == 8 && format.m_signedSample) ||
(format.m_bitPerSample == 24))
throw TException("This format is not supported for recording");*/
  try {
    TSoundTrackFormat fmt = getPreferredFormat(format);
    if (fmt != format) {
      throw TSoundDeviceException(TSoundDeviceException::UnsupportedFormat,
                                  "Unsupported Format");
    }
  } catch (TSoundDeviceException &e) {
    throw TSoundDeviceException(TSoundDeviceException::UnsupportedFormat,
                                e.getMessage());
  }

  if (!setRecordLine(devtype))
    throw TSoundDeviceException(TSoundDeviceException::UnableSetDevice,
                                "Problems to set input source line to record");

  m_imp->m_format = format;
  m_imp->m_st     = 0;
  ResetEvent(m_imp->m_hLastBlockDone);
  ResetEvent(m_imp->m_hBlockDone);
  m_imp->m_allocateBuff = true;
  m_imp->m_isRecording  = true;
  m_imp->m_index        = 0;
  m_imp->m_recordedBlocks.clear();
  m_imp->m_byteRecorded = 0;
  TINT32 bytePerSec     = format.m_sampleRate *
                      ((format.m_bitPerSample * format.m_channelCount) >> 3);

  try {
    WaveFormat wf(m_imp->m_format.m_channelCount, m_imp->m_format.m_sampleRate,
                  m_imp->m_format.m_bitPerSample, m_imp->m_format.m_sampleType);

    m_imp->open(wf);
  } catch (TException &e) {
    m_imp->m_isRecording = false;
    throw TSoundDeviceException(TSoundDeviceException::UnableOpenDevice,
                                e.getMessage());
  }
  for (; m_imp->m_index < (int)(m_imp->m_whdr.size() - 1); ++m_imp->m_index) {
    try {
      m_imp->prepareHeader(new char[bytePerSec], bytePerSec,
                           m_imp->m_whdr[m_imp->m_index]);
      m_imp->addBlock(m_imp->m_whdr[m_imp->m_index]);
    } catch (TException &e) {
      m_imp->m_isRecording = false;
      for (int j = 0; j < (int)(m_imp->m_whdr.size() - 1); ++j) {
        if (m_imp->m_whdr[j].dwFlags & WHDR_PREPARED) {
          try {
            m_imp->unprepareHeader(m_imp->m_whdr[j]);
          } catch (TException &e) {
            throw TSoundDeviceException(
                TSoundDeviceException::UnableCloseDevice, e.getMessage());
          }
          delete[] m_imp->m_whdr[j].lpData;
        } else if (j == m_imp->m_index)
          delete[] m_imp->m_whdr[j].lpData;
      }
      throw TSoundDeviceException(TSoundDeviceException::UnablePrepare,
                                  e.getMessage());
    }
  }

  m_imp->m_executor.addTask(new RecordTask(m_imp));
}

//------------------------------------------------------------------------------

void TSoundInputDevice::record(const TSoundTrackP &st, Source devtype) {
  if (m_imp->m_isRecording)
    throw TSoundDeviceException(TSoundDeviceException::Busy,
                                "Just another recoding is in progress");

  m_imp->m_format = st->getFormat();
  /*if ((m_imp->m_format.m_bitPerSample == 8 && m_imp->m_format.m_signedSample)
||
(m_imp->m_format.m_bitPerSample == 24))
throw TException("This format is not supported for recording");*/
  try {
    TSoundTrackFormat fmt = getPreferredFormat(st->getFormat());
    if (fmt != st->getFormat()) {
      throw TSoundDeviceException(TSoundDeviceException::UnsupportedFormat,
                                  "Unsupported Format");
    }
  } catch (TSoundDeviceException &e) {
    throw TSoundDeviceException(TSoundDeviceException::UnsupportedFormat,
                                e.getMessage());
  }

  if (!setRecordLine(devtype))
    throw TSoundDeviceException(TSoundDeviceException::UnableSetDevice,
                                "Problems to set input source line to record");

  m_imp->m_st           = st;
  m_imp->m_allocateBuff = false;
  m_imp->m_isRecording  = true;
  ResetEvent(m_imp->m_hLastBlockDone);
  ResetEvent(m_imp->m_hBlockDone);
  m_imp->m_index = 0;
  m_imp->m_recordedBlocks.clear();
  m_imp->m_byteRecorded = 0;
  try {
    WaveFormat wf(m_imp->m_format.m_channelCount, m_imp->m_format.m_sampleRate,
                  m_imp->m_format.m_bitPerSample, m_imp->m_format.m_sampleType);

    m_imp->open(wf);
    m_imp->prepareHeader(
        (char *)st->getRawData(),
        st->getSampleCount() *
            ((st->getBitPerSample() * st->getChannelCount()) >> 3),
        m_imp->m_whdr[m_imp->m_index]);
    m_imp->addBlock(m_imp->m_whdr[m_imp->m_index]);
  } catch (TException &e) {
    m_imp->m_isRecording = false;
    if (m_imp->m_whdr[m_imp->m_index].dwFlags & WHDR_PREPARED)
      m_imp->unprepareHeader(m_imp->m_whdr[m_imp->m_index]);

    throw TSoundDeviceException(TSoundDeviceException::UnablePrepare,
                                e.getMessage());
  }

  m_imp->m_executor.addTask(new RecordTask(m_imp));
}
#endif

//------------------------------------------------------------------------------

TSoundTrackP TSoundInputDevice::stop() {
  if (!m_imp->m_isRecording) return 0;

  m_imp->m_isRecording = false;
  try {
    m_imp->reset();
  } catch (TException &e) {
    for (int j = 0; j < (int)m_imp->m_whdr.size(); ++j) {
      if (m_imp->m_whdr[j].dwFlags & WHDR_PREPARED) {
        try {
          m_imp->unprepareHeader(m_imp->m_whdr[j]);
        } catch (TException &e) {
          throw TSoundDeviceException(TSoundDeviceException::UnablePrepare,
                                      e.getMessage());
        }
        delete[] m_imp->m_whdr[j].lpData;
      }
    }
    throw TSoundDeviceException(TSoundDeviceException::UnableCloseDevice,
                                e.getMessage());
  }

  if (m_imp->m_allocateBuff) {
    WaitForSingleObject(m_imp->m_hLastBlockDone, INFINITE);

    TSoundTrackP st = TSoundTrack::create(
        m_imp->m_format,
        m_imp->m_byteRecorded / ((m_imp->m_format.m_bitPerSample *
                                  m_imp->m_format.m_channelCount) >>
                                 3));

    TINT32 bytePerSec =
        m_imp->m_format.m_sampleRate *
        ((m_imp->m_format.m_bitPerSample * m_imp->m_format.m_channelCount) >>
         3);

    int i;
    for (i = 0; i < (int)(m_imp->m_recordedBlocks.size() - 1); ++i) {
      memcpy((void *)(st->getRawData() + bytePerSec * i),
             m_imp->m_recordedBlocks[i], bytePerSec);
      delete[] m_imp->m_recordedBlocks[i];
    }

    TINT32 lastBlockSize = m_imp->m_byteRecorded - (bytePerSec * i);

    if (lastBlockSize != 0) {
      memcpy((void *)(st->getRawData() + bytePerSec * i),
             m_imp->m_recordedBlocks[i], lastBlockSize);
      delete[] m_imp->m_recordedBlocks[i];
    }

    try {
      m_imp->close();
    } catch (TException &e) {
      throw TSoundDeviceException(TSoundDeviceException::UnableCloseDevice,
                                  e.getMessage());
    }
    return st;
  } else {
    WaitForSingleObject(m_imp->m_hLastBlockDone, INFINITE);
    try {
      m_imp->close();
    } catch (TException &e) {
      throw TSoundDeviceException(TSoundDeviceException::UnableCloseDevice,
                                  e.getMessage());
    }
    return m_imp->m_st;
  }
}

//------------------------------------------------------------------------------
#ifndef TNZCORE_LIGHT

void RecordTask::run() {
  m_dev->start();

  if (m_dev->m_allocateBuff) {
    TINT32 bytePerSec =
        m_dev->m_format.m_sampleRate *
        ((m_dev->m_format.m_bitPerSample * m_dev->m_format.m_channelCount) >>
         3);

    while (m_dev->m_whdr[(m_dev->m_index + 1) % m_dev->m_whdr.size()].dwFlags &
           WHDR_PREPARED) {
      if (m_dev->m_isRecording)
        WaitForSingleObject(m_dev->m_hBlockDone, INFINITE);
      int indexToPrepare = m_dev->m_index;

      // calcolo l'indice successivo per far l'unprepare
      m_dev->m_index = (m_dev->m_index + 1) % m_dev->m_whdr.size();
      if (m_dev->m_whdr[m_dev->m_index].dwFlags & WHDR_DONE) {
        TINT32 byteRecorded = m_dev->m_whdr[m_dev->m_index].dwBytesRecorded;
        if (byteRecorded) {
          m_dev->m_recordedBlocks.push_back(
              m_dev->m_whdr[m_dev->m_index].lpData);
          m_dev->m_byteRecorded += byteRecorded;
        }

        try {
          m_dev->unprepareHeader(m_dev->m_whdr[m_dev->m_index]);
        } catch (TException &) {
          for (int i = 0; i < (int)m_dev->m_recordedBlocks.size(); ++i)
            delete[] m_dev->m_recordedBlocks[i];
          return;
        }

        if (byteRecorded == 0) {
          delete[] m_dev->m_whdr[m_dev->m_index].lpData;
        }

        // con questo controllo si evita che vengano accodati nuovi blocchi
        // dopo che e' stata chiamata la waveInReset
        if (m_dev->m_isRecording) {
          try {
            m_dev->prepareHeader(new char[bytePerSec], bytePerSec,
                                 m_dev->m_whdr[indexToPrepare]);

            m_dev->addBlock(m_dev->m_whdr[indexToPrepare]);
          } catch (TException &) {
            m_dev->m_isRecording = false;
            for (int i = 0; i < (int)m_dev->m_recordedBlocks.size(); ++i)
              delete[] m_dev->m_recordedBlocks[i];
            return;
          }
        }

      } else
        m_dev->m_index = indexToPrepare;
    }
  } else {
    if (m_dev->m_isRecording)
      WaitForSingleObject(m_dev->m_hBlockDone, INFINITE);
    try {
      m_dev->unprepareHeader(m_dev->m_whdr[m_dev->m_index]);
      m_dev->m_isRecording = false;
    } catch (TException &) {
      m_dev->m_isRecording = false;
      return;
    }
  }

  SetEvent(m_dev->m_hLastBlockDone);
  return;
}

//------------------------------------------------------------------------------
#endif

double TSoundInputDevice::getVolume() {
  DWORD dwVolumeControlID;

  UINT nNumMixers;
  MMRESULT ret;
  MIXERLINE mxl;

  nNumMixers = mixerGetNumDevs();

  if (nNumMixers == 0)
    throw TSoundDeviceException(TSoundDeviceException::NoMixer,
                                "Doesn't exist a audio mixer device");

  // get dwLineID
  ret = getLineInfo((HMIXEROBJ)0, MIXERLINE_COMPONENTTYPE_DST_WAVEIN, mxl);
  if (ret != MMSYSERR_NOERROR)
    throw TSoundDeviceException(TSoundDeviceException::UnableVolume,
                                "Error to obtain info by mixer");

  // get dwControlID
  MIXERCONTROL mxc;
  ret = getLineControl(mxc, (HMIXEROBJ)0, mxl.dwLineID,
                       MIXERCONTROL_CONTROLTYPE_VOLUME);
  if (ret == MIXERR_INVALCONTROL)
    throw TSoundDeviceException(
        TSoundDeviceException::UnableVolume,
        "Is not possible to obtain info of volume by mixer");

  if (ret != MMSYSERR_NOERROR)
    throw TSoundDeviceException(TSoundDeviceException::UnableVolume,
                                "Error to obtain info by mixer");

  dwVolumeControlID = mxc.dwControlID;

  MIXERCONTROLDETAILS_UNSIGNED mxcdVolume;

  ret = getControlDetails((HMIXEROBJ)0, dwVolumeControlID, mxc.cMultipleItems,
                          &mxcdVolume);
  if (ret != MMSYSERR_NOERROR)
    throw TSoundDeviceException(TSoundDeviceException::UnableVolume,
                                "Error to obtain info by mixer");

  DWORD dwVal = mxcdVolume.dwValue;

  return (double)dwVal * 10.0 / (double)mxc.Bounds.dwMaximum;
}

//------------------------------------------------------------------------------

bool TSoundInputDevice::setVolume(double value) {
  DWORD dwVolumeControlID, dwMaximum;

  UINT nNumMixers;
  MMRESULT ret;
  MIXERLINE mxl;

  nNumMixers = mixerGetNumDevs();

  if (nNumMixers == 0)
    throw TSoundDeviceException(TSoundDeviceException::NoMixer,
                                "Doesn't exist a audio mixer device");

  // get dwLineID
  ret = getLineInfo((HMIXEROBJ)0, MIXERLINE_COMPONENTTYPE_DST_WAVEIN, mxl);
  if (ret != MMSYSERR_NOERROR)
    throw TException("Error to obtain info by mixer");

  // get dwControlID
  MIXERCONTROL mxc;
  ret = getLineControl(mxc, (HMIXEROBJ)0, mxl.dwLineID,
                       MIXERCONTROL_CONTROLTYPE_VOLUME);
  if (ret != MMSYSERR_NOERROR)
    throw TSoundDeviceException(TSoundDeviceException::UnableVolume,
                                "Error to obtain info by mixer");

  dwMaximum         = mxc.Bounds.dwMaximum;
  dwVolumeControlID = mxc.dwControlID;

  int newValue;
  double fattProp = ((double)(mxc.Metrics.cSteps - 1) * value) / 10;
  double delta    = (double)(dwMaximum / (mxc.Metrics.cSteps - 1));
  newValue        = (int)(tround(fattProp) * delta);

  MIXERCONTROLDETAILS_UNSIGNED mxcdVolume = {(DWORD)newValue};
  ret = setControlDetails((HMIXEROBJ)0, dwVolumeControlID, mxc.cMultipleItems,
                          &mxcdVolume);
  if (ret != MMSYSERR_NOERROR)
    throw TSoundDeviceException(TSoundDeviceException::UnableVolume,
                                "Error to obtain info by mixer");
  return true;
}

//------------------------------------------------------------------------------

bool TSoundInputDevice::supportsVolume() {
  UINT nNumMixers;
  MMRESULT ret;
  MIXERLINE mxl;

  nNumMixers = mixerGetNumDevs();

  if (nNumMixers == 0)
    throw TSoundDeviceException(TSoundDeviceException::NoMixer,
                                "Doesn't exist a audio mixer device");

  // get dwLineID
  ret = getLineInfo((HMIXEROBJ)0, MIXERLINE_COMPONENTTYPE_DST_WAVEIN, mxl);
  if (ret != MMSYSERR_NOERROR)
    throw TSoundDeviceException(TSoundDeviceException::UnableVolume,
                                "Error to obtain info by mixer");

  // get dwControlID
  MIXERCONTROL mxc;
  ret = getLineControl(mxc, (HMIXEROBJ)0, mxl.dwLineID,
                       MIXERCONTROL_CONTROLTYPE_VOLUME);
  if (ret == MIXERR_INVALCONTROL) return false;

  if (ret != MMSYSERR_NOERROR)
    throw TSoundDeviceException(TSoundDeviceException::UnableVolume,
                                "Error to obtain info by mixer");

  return true;
}

//------------------------------------------------------------------------------

bool TSoundInputDevice::isRecording() { return m_imp->m_isRecording; }

//------------------------------------------------------------------------------

/*TSoundTrackFormat TSoundInputDevice::getPreferredFormat(
      ULONG sampleRate, int channelCount, int bitPerSample)
{
  MMRESULT ret;
  TSoundTrackFormat fmt;

  ret = isaFormatSupported(sampleRate, channelCount, bitPerSample, true);

  if (ret == MMSYSERR_NOERROR)
  {
    fmt.m_bitPerSample = bitPerSample;
    fmt.m_channelCount = channelCount;
    fmt.m_sampleRate = sampleRate;

    if (bitPerSample >= 16)
      fmt.m_signedSample = true;
    else
      fmt.m_signedSample = false;
    return fmt;
  }
  if (ret == WAVERR_BADFORMAT)
  {
    //avvvicinarsi al sample rate => dovrebbe esser OK avendo selezionato i piu'
vicini
    std::set<int>::iterator it = m_imp->m_supportedRate.lower_bound(sampleRate);
    if (it == m_imp->m_supportedRate.end())
    {
                  it = std::max_element(m_imp->m_supportedRate.begin(),
                                         m_imp->m_supportedRate.end());
                        if (it != m_imp->m_supportedRate.end())
                  sampleRate = *(m_imp->m_supportedRate.rbegin());
      else
        throw TSoundDeviceException(
          TSoundDeviceException::UnsupportedFormat,
          "There isn't a supported format");
    }
    else
      sampleRate = *it;
    ret = isaFormatSupported(sampleRate, channelCount, bitPerSample, true);
    if (ret == MMSYSERR_NOERROR)
    {
      fmt.m_bitPerSample = bitPerSample;
      fmt.m_channelCount = channelCount;
      fmt.m_sampleRate = sampleRate;

      if (bitPerSample >= 16)
        fmt.m_signedSample = true;
      else
        fmt.m_signedSample = false;
      return fmt;
    }
    if (ret == WAVERR_BADFORMAT)
    {
      //cambiare bps
      if (bitPerSample <= 8)
        bitPerSample = 8;
      else if ((bitPerSample > 8 && bitPerSample < 16) || bitPerSample >= 16)
        bitPerSample = 16;

      ret = isaFormatSupported(sampleRate, channelCount, bitPerSample, true);
      if (ret == MMSYSERR_NOERROR)
      {
        fmt.m_bitPerSample = bitPerSample;
        fmt.m_channelCount = channelCount;
        fmt.m_sampleRate = sampleRate;

        if (bitPerSample >= 16)
          fmt.m_signedSample = true;
        else
          fmt.m_signedSample = false;
        return fmt;
      }
      if (ret == WAVERR_BADFORMAT)
      {
        //switch mono/stereo
        if (channelCount <= 1)
          channelCount = 1;
        else
          channelCount = 2;

        ret = isaFormatSupported(sampleRate, channelCount, bitPerSample, true);
        if (ret == MMSYSERR_NOERROR)
        {
          fmt.m_bitPerSample = bitPerSample;
          fmt.m_channelCount = channelCount;
          fmt.m_sampleRate = sampleRate;

          if (bitPerSample >= 16)
            fmt.m_signedSample = true;
          else
            fmt.m_signedSample = false;
          return fmt;
        }
        if (ret == WAVERR_BADFORMAT)
        {
          throw TSoundDeviceException(
            TSoundDeviceException::UnsupportedFormat,
            "Doesn't exist a preferred format");
        }
      }
    }
  }
  throw TSoundDeviceException(
    TSoundDeviceException::UnsupportedFormat,
    "Error to query supported format");
}
*/
TSoundTrackFormat TSoundInputDevice::getPreferredFormat(TUINT32 sampleRate,
                                                        int channelCount,
                                                        int bitPerSample) {
  TSoundTrackFormat fmt;

  // avvvicinarsi al sample rate => dovrebbe esser OK avendo selezionato i piu'
  // vicini
  std::set<int>::iterator it = m_imp->m_supportedRate.lower_bound(sampleRate);
  if (it == m_imp->m_supportedRate.end()) {
    it = std::max_element(m_imp->m_supportedRate.begin(),
                          m_imp->m_supportedRate.end());
    if (it != m_imp->m_supportedRate.end())
      sampleRate = *(m_imp->m_supportedRate.rbegin());
    else
      throw TSoundDeviceException(TSoundDeviceException::UnsupportedFormat,
                                  "There isn't a supported format");
  } else
    sampleRate = *it;

  // Normalize bit sample and set preferred sample type
  if (bitPerSample <= 8) {
    bitPerSample     = 8;
    fmt.m_sampleType = TSound::UINT;
  } else if (bitPerSample <= 16) {
    bitPerSample     = 16;
    fmt.m_sampleType = TSound::INT;
  } else if (bitPerSample <= 24) {
    bitPerSample     = 24;
    fmt.m_sampleType = TSound::INT;
  } else if (bitPerSample <= 32) {
    bitPerSample     = 32;
    fmt.m_sampleType = TSound::FLOAT;
  }

  // switch mono/stereo
  if (channelCount <= 1)
    channelCount = 1;
  else
    channelCount = 2;

  fmt.m_bitPerSample = bitPerSample;
  fmt.m_channelCount = channelCount;
  fmt.m_sampleRate   = sampleRate;

  return fmt;
}

//------------------------------------------------------------------------------

TSoundTrackFormat TSoundInputDevice::getPreferredFormat(
    const TSoundTrackFormat &format) {
  try {
    return getPreferredFormat(format.m_sampleRate, format.m_channelCount,
                              format.m_bitPerSample);
  } catch (TSoundDeviceException &e) {
    throw TSoundDeviceException(TSoundDeviceException::UnsupportedFormat,
                                e.getMessage());
  }
}

//==============================================================================
//==============================================================================
// Funzioni per l'interazione con il mixer device
//==============================================================================
//==============================================================================

namespace {

// restituisce dentro la struttura mxc le informazioni relative
// al controllo di tipo dwControlType associato alla linea
// identificata da dwLineID
MMRESULT getLineControl(MIXERCONTROL &mxc, HMIXEROBJ hMixer, DWORD dwLineID,
                        DWORD dwControlType) {
  MIXERLINECONTROLS mxlc;
  mxlc.cbStruct      = sizeof(MIXERLINECONTROLS);
  mxlc.dwLineID      = dwLineID;
  mxlc.dwControlType = dwControlType;
  mxlc.cControls     = 1;
  mxlc.cbmxctrl      = sizeof(MIXERCONTROL);
  mxlc.pamxctrl      = &mxc;
  MMRESULT ret       = mixerGetLineControls(
      (HMIXEROBJ)hMixer, &mxlc,
      MIXER_OBJECTF_HMIXER | MIXER_GETLINECONTROLSF_ONEBYTYPE);
  return ret;
}

//------------------------------------------------------------------------------

// restituisce nella struttura mxl le informazioni relative alla linea
// sorgente individuata dagli estremi destination e source
MMRESULT getLineInfo(HMIXEROBJ hMixer, MIXERLINE &mxl, DWORD destination,
                     DWORD source) {
  MMRESULT ret;

  mxl.cbStruct      = sizeof(mxl);
  mxl.dwDestination = destination;
  mxl.dwSource      = source;
  ret               = mixerGetLineInfo(0, &mxl,
                                       MIXER_OBJECTF_HMIXER | MIXER_GETLINEINFOF_SOURCE);
  return ret;
}

//------------------------------------------------------------------------------

// restituisce nella struttura mxl le informazioni relative alla linea
// individuata da dwLineID
MMRESULT getLineInfo(HMIXEROBJ hMixer, MIXERLINE &mxl, DWORD dwLineID) {
  MMRESULT ret;

  mxl.cbStruct = sizeof(mxl);
  mxl.dwLineID = dwLineID;
  ret          = mixerGetLineInfo((HMIXEROBJ)hMixer, &mxl,
                                  MIXER_OBJECTF_HMIXER | MIXER_GETLINEINFOF_LINEID);
  return ret;
}

//------------------------------------------------------------------------------

// restituisce nella struttura mxl le informazioni relative alla linea
// individuata dal tipo specificato in dwComponentType
MMRESULT getLineInfo(HMIXEROBJ hMixer, DWORD dwComponentType, MIXERLINE &mxl) {
  MMRESULT ret;

  mxl.cbStruct        = sizeof(MIXERLINE);
  mxl.dwComponentType = dwComponentType;
  ret =
      mixerGetLineInfo((HMIXEROBJ)hMixer, &mxl,
                       MIXER_OBJECTF_HMIXER | MIXER_GETLINEINFOF_COMPONENTTYPE);
  return ret;
}

//------------------------------------------------------------------------------

// consente di settare il valore booleano specificato in mxcdSelectValue
// relativo al controllo specificato in dwSelectControlID
MMRESULT setControlDetails(HMIXEROBJ hMixer, DWORD dwSelectControlID,
                           DWORD dwMultipleItems,
                           MIXERCONTROLDETAILS_BOOLEAN *mxcdSelectValue) {
  MMRESULT ret;
  MIXERCONTROLDETAILS mxcd;
  mxcd.cbStruct       = sizeof(MIXERCONTROLDETAILS);
  mxcd.dwControlID    = dwSelectControlID;
  mxcd.cChannels      = 1;
  mxcd.cMultipleItems = dwMultipleItems;
  mxcd.cbDetails      = sizeof(MIXERCONTROLDETAILS_BOOLEAN);
  mxcd.paDetails      = mxcdSelectValue;
  ret                 = mixerSetControlDetails(
      (HMIXEROBJ)hMixer, &mxcd,
      MIXER_OBJECTF_HMIXER | MIXER_SETCONTROLDETAILSF_VALUE);
  return ret;
}

//------------------------------------------------------------------------------

// consente di settare il valore UNSIGNED specificato in mxcdSelectValue
// relativo al controllo specificato in dwSelectControlID
MMRESULT setControlDetails(HMIXEROBJ hMixer, DWORD dwSelectControlID,
                           DWORD dwMultipleItems,
                           MIXERCONTROLDETAILS_UNSIGNED *mxcdSelectValue) {
  MMRESULT ret;
  MIXERCONTROLDETAILS mxcd;
  mxcd.cbStruct       = sizeof(MIXERCONTROLDETAILS);
  mxcd.dwControlID    = dwSelectControlID;
  mxcd.cChannels      = 1;
  mxcd.cMultipleItems = dwMultipleItems;
  mxcd.cbDetails      = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
  mxcd.paDetails      = mxcdSelectValue;
  ret                 = mixerSetControlDetails(
      (HMIXEROBJ)hMixer, &mxcd,
      MIXER_OBJECTF_HMIXER | MIXER_SETCONTROLDETAILSF_VALUE);
  return ret;
}

//------------------------------------------------------------------------------

// consente di ottenere il valore UNSIGNED specificato in mxcdSelectValue
// relativo al controllo specificato in dwSelectControlID
MMRESULT getControlDetails(HMIXEROBJ hMixer, DWORD dwSelectControlID,
                           DWORD dwMultipleItems,
                           MIXERCONTROLDETAILS_UNSIGNED *mxcdSelectValue) {
  MMRESULT ret;
  MIXERCONTROLDETAILS mxcd;
  mxcd.cbStruct       = sizeof(MIXERCONTROLDETAILS);
  mxcd.dwControlID    = dwSelectControlID;
  mxcd.cChannels      = 1;
  mxcd.cMultipleItems = dwMultipleItems;
  mxcd.cbDetails      = sizeof(MIXERCONTROLDETAILS_UNSIGNED);
  mxcd.paDetails      = mxcdSelectValue;
  ret                 = mixerGetControlDetails(
      (HMIXEROBJ)hMixer, &mxcd,
      MIXER_OBJECTF_HMIXER | MIXER_SETCONTROLDETAILSF_VALUE);
  return ret;
}
//------------------------------------------------------------------------------

// consente di ottenere la lista di informazioni in pmxcdSelectText
// relativo al controllo specificato in dwSelectControlID
MMRESULT getControlDetails(HMIXEROBJ hMixer, DWORD dwSelectControlID,
                           DWORD dwMultipleItems,
                           MIXERCONTROLDETAILS_LISTTEXT *pmxcdSelectText) {
  MMRESULT ret;
  MIXERCONTROLDETAILS mxcd;

  mxcd.cbStruct       = sizeof(MIXERCONTROLDETAILS);
  mxcd.dwControlID    = dwSelectControlID;
  mxcd.cChannels      = 1;
  mxcd.cMultipleItems = dwMultipleItems;
  mxcd.cbDetails      = sizeof(MIXERCONTROLDETAILS_LISTTEXT);
  mxcd.paDetails      = pmxcdSelectText;
  ret                 = mixerGetControlDetails((HMIXEROBJ)0, &mxcd,
                                               MIXER_GETCONTROLDETAILSF_LISTTEXT);
  return ret;
}

//------------------------------------------------------------------------------

// restituiscei l nome della linea identificata da lineID
std::string getMixerLineName(DWORD lineID) {
  MIXERLINE mxl;
  MMRESULT ret;

  ret = getLineInfo((HMIXEROBJ)0, mxl, lineID);
#ifdef TNZCORE_LIGHT
  assert(false);
  return "";
#else
  return QString::fromWCharArray(mxl.szName).toStdString();
#endif
}

//------------------------------------------------------------------------------

// restituisce la lista degli identificativi delle linee sorgente associate
// alla destinazione di tipo dstComponentType
std::list<DWORD> getMixerSrcLines(DWORD dstComponentType) {
  std::list<DWORD> srcList;
  MMRESULT ret;
  MIXERLINE mxl;

  ret = getLineInfo((HMIXEROBJ)0, MIXERLINE_COMPONENTTYPE_DST_WAVEIN, mxl);
  if (ret != MMSYSERR_NOERROR)
    // forse bisognerebbe lanciare un'eccezione
    return srcList;  // non ha linea di dst per la registrazione

  int v;
  for (v = 0; v < (int)mxl.cConnections; v++) {
    MIXERLINE mxl1;

    ret = getLineInfo((HMIXEROBJ)0, mxl1, mxl.dwDestination, v);
    if (ret == MMSYSERR_NOERROR) srcList.push_back(mxl1.dwLineID);
  }
  return srcList;
}

//------------------------------------------------------------------------------

// restituisce la lista degli identificativi delle linee sorgente di tipo
// srcComponentType associate alla destinazione di tipo dstComponentType
std::list<DWORD> getMixerSrcLines(DWORD dstComponentType,
                                  DWORD srcComponentType) {
  std::list<DWORD> srcList;
  MMRESULT ret;
  MIXERLINE mxl;

  ret = getLineInfo((HMIXEROBJ)0, MIXERLINE_COMPONENTTYPE_DST_WAVEIN, mxl);
  if (ret != MMSYSERR_NOERROR)
    // forse bisognerebbe lanciare un'eccezione
    return srcList;  // non ha linea di dst per la registrazione

  int v;
  for (v = 0; v < (int)mxl.cConnections; v++) {
    MIXERLINE mxl1;

    ret = getLineInfo((HMIXEROBJ)0, mxl1, mxl.dwDestination, v);
    if (ret == MMSYSERR_NOERROR)
      if (mxl1.dwComponentType == srcComponentType)
        srcList.push_back(mxl1.dwLineID);
  }
  return srcList;
}

//------------------------------------------------------------------------------

// restituisce true sse la linea destinazione di tipo dstComponentType
// supporta una linea sorgente di tipo srcComponentType
bool isSrcLineSupported(DWORD dstComponentType, DWORD srcComponentType) {
  // ci possono essere piu' linee sorgente dello stesso tipo in
  // corrispondenza di una data linea destinazione ?
  MMRESULT ret;
  MIXERLINE mxl;

  ret = getLineInfo((HMIXEROBJ)0, MIXERLINE_COMPONENTTYPE_DST_WAVEIN, mxl);
  if (ret != MMSYSERR_NOERROR)
    return false;  // non ha linea di dst per la registrazione

  int v;
  for (v = 0; v < (int)mxl.cConnections; v++) {
    MIXERLINE mxl1;

    ret = getLineInfo((HMIXEROBJ)0, mxl1, mxl.dwDestination, v);
    if (ret == MMSYSERR_NOERROR)
      if (mxl1.dwComponentType == srcComponentType) return true;
  }

  return false;
}

//------------------------------------------------------------------------------

bool activateSrcLine(const MIXERLINE &mxlDst, DWORD componentTypeSrc) {
  if (!isSrcLineSupported(mxlDst.dwComponentType, componentTypeSrc))
    return false;

  bool bRetVal = true;

  for (DWORD v = 0; v < mxlDst.cConnections; v++) {
    MIXERLINE mxlSrc;
    MMRESULT ret = getLineInfo((HMIXEROBJ)0, mxlSrc, mxlDst.dwDestination, v);

    if (ret == MMSYSERR_NOERROR) {
      // chiedo il controllo di tipo MUTE della linea sorgente
      MIXERCONTROL mxc;
      ret = getLineControl(mxc, (HMIXEROBJ)0, mxlSrc.dwLineID,
                           MIXERCONTROL_CONTROLTYPE_MUTE);

      if (ret == MMSYSERR_NOERROR) {
        MIXERCONTROLDETAILS_BOOLEAN mxcdSelectValue;
        mxcdSelectValue.fValue =
            mxlSrc.dwComponentType == componentTypeSrc ? 0L : 1L;

        ret = setControlDetails((HMIXEROBJ)0, mxc.dwControlID,
                                mxc.cMultipleItems, &mxcdSelectValue);
        if (ret != MMSYSERR_NOERROR) bRetVal = false;
      }
    }
  }
  return bRetVal;
}

//------------------------------------------------------------------------------

bool setSrcMixMuxControl(MIXERCONTROL mxc, DWORD componentTypeSrc) {
  MMRESULT ret;
  DWORD dwIndexLine;
  bool found = false;

  // mantengo nota del ID del controllo dsst individuato e
  // del numero di linee src ad esso associate
  DWORD dwSelectControlID = mxc.dwControlID;
  DWORD dwMultipleItems   = mxc.cMultipleItems;

  if (dwMultipleItems == 0) return false;

  // determino l'indice dell'item corrispondente alla linea sorgente
  // di tipo componentTypeSrc

  std::unique_ptr<MIXERCONTROLDETAILS_LISTTEXT[]> pmxcdSelectText(
      new MIXERCONTROLDETAILS_LISTTEXT[dwMultipleItems]);

  if (pmxcdSelectText) {
    // estraggo le info su tutte le linee associate al controllo
    ret = getControlDetails((HMIXEROBJ)0, dwSelectControlID, dwMultipleItems,
                            pmxcdSelectText.get());

    if (ret == MMSYSERR_NOERROR) {
      for (DWORD dwi = 0; dwi < dwMultipleItems; dwi++) {
        // prendo le info su ogni linea e verifico se e' del giusto tipo
        MIXERLINE mxl;
        ret = getLineInfo((HMIXEROBJ)0, mxl, pmxcdSelectText[dwi].dwParam1);
        if (ret == MMSYSERR_NOERROR &&
            mxl.dwComponentType == componentTypeSrc) {
          dwIndexLine = dwi;
          found       = true;
          break;
        }
      }
    }

    if (!found) return false;
  }

  if (dwIndexLine >= dwMultipleItems) return false;

  bool bRetVal = false;

  std::unique_ptr<MIXERCONTROLDETAILS_BOOLEAN[]> pmxcdSelectValue(
      new MIXERCONTROLDETAILS_BOOLEAN[dwMultipleItems]);

  if (pmxcdSelectValue) {
    ::ZeroMemory(pmxcdSelectValue.get(),
                 dwMultipleItems * sizeof(MIXERCONTROLDETAILS_BOOLEAN));

    // impostazione del valore
    pmxcdSelectValue[dwIndexLine].fValue =
        (TINT32)1;  // lVal; //dovrebbe esser uno

    ret = setControlDetails((HMIXEROBJ)0, dwSelectControlID, dwMultipleItems,
                            pmxcdSelectValue.get());
    if (ret == MMSYSERR_NOERROR) bRetVal = true;
  }
  return bRetVal;
}

//------------------------------------------------------------------------------

bool setRecordLine(TSoundInputDevice::Source typeInput) {
  DWORD dwComponentTypeSrc;

  UINT nNumMixers;
  MMRESULT ret;
  MIXERLINE mxl = {0};

  switch (typeInput) {
  case TSoundInputDevice::LineIn:
    dwComponentTypeSrc = MIXERLINE_COMPONENTTYPE_SRC_LINE /*|
                             MIXERLINE_COMPONENTTYPE_SRC_AUXILIARY |
                             MIXERLINE_COMPONENTTYPE_SRC_ANALOG*/
        ;
    break;
  case TSoundInputDevice::DigitalIn:
    dwComponentTypeSrc = MIXERLINE_COMPONENTTYPE_SRC_DIGITAL;
    break;
  case TSoundInputDevice::CdAudio:
    dwComponentTypeSrc = MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC;
    break;
  default:
    dwComponentTypeSrc = MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE;
  }

  nNumMixers = mixerGetNumDevs();

  if (nNumMixers == 0) return false;

  // utilizziamo il MIXER di default identificato dall'indice 0
  // vedo se il device ha una linea dst per il wave_input
  ret = getLineInfo((HMIXEROBJ)0, MIXERLINE_COMPONENTTYPE_DST_WAVEIN, mxl);
  if (ret != MMSYSERR_NOERROR)
    return false;  // non ha linea di dst per la registrazione

  // vediamo che tipo controllo ha questa linea dst

  // sara' un MIXER?
  MIXERCONTROL mxc = {0};

  ret = getLineControl(mxc, (HMIXEROBJ)0, mxl.dwLineID,
                       MIXERCONTROL_CONTROLTYPE_MIXER);

  if (ret != MMSYSERR_NOERROR) {
    // no mixer, try MUX
    ret = getLineControl(mxc, (HMIXEROBJ)0, mxl.dwLineID,
                         MIXERCONTROL_CONTROLTYPE_MUX);

    if (ret != MMSYSERR_NOERROR) {
      // vediamo se e' uno di quei device ne' MIXER ne' MUX
      return activateSrcLine(mxl, dwComponentTypeSrc);
    } else {
      // la linea ha un controllo di tipo MUX
      return setSrcMixMuxControl(mxc, dwComponentTypeSrc);
    }
  } else {
    // la linea ha un controllo di tipo MIXER
    return setSrcMixMuxControl(mxc, dwComponentTypeSrc);
  }
}

//------------------------------------------------------------------------------

MMRESULT isaFormatSupported(int sampleRate, int channelCount, int bitPerSample,
                            int sampleType, bool input) {
  WAVEFORMATEX wf;
  MMRESULT ret;

  wf.wFormatTag      = sampleType & TSound::WMASK;
  wf.nChannels       = channelCount;
  wf.nSamplesPerSec  = sampleRate;
  wf.wBitsPerSample  = bitPerSample;
  wf.nBlockAlign     = (wf.nChannels * wf.wBitsPerSample) >> 3;
  wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
  wf.cbSize          = 0;

  if (input)
    ret = waveInOpen(NULL, WAVE_MAPPER, &wf, NULL, NULL, WAVE_FORMAT_QUERY);
  else
    ret = waveOutOpen(NULL, WAVE_MAPPER, &wf, NULL, NULL, WAVE_FORMAT_QUERY);
  return ret;
}
}  // namespace
