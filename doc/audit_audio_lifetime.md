# Audio resource lifetime follow-up

The current source review found independent playback lifetime defects. These are
not attributed to the unresolved Windows text-layout crash. No audio-device
session, local build, runtime regression, sanitizer run, or memory measurement
has been executed for this follow-up.

## Qt output ownership repair

Linux and macOS select `common/tsound/tsound_qt.cpp` in
`tnzcore/CMakeLists.txt`. `TSoundOutputDeviceImp::play` created an unparented
`QAudioSink`/`QAudioOutput` and retained only a `QPointer`. Replacing a format
stopped the old object but overwrote that observer without deleting the old
object. The final implementation also had no output cleanup. Each Qt 6 output
owned a live refill timer whose lambda captured the implementation's raw `this`.

`TXsheet::play` and the Cast inspector own their `TSoundOutputDevice`, so retiring
those owners can leave a timer referring to a destroyed implementation. Other
legacy callers have separate raw-player ownership gaps; those do not make the
implementation's missing ownership safe.

The prepared repair gives the implementation unique ownership of its output,
stops the old output before replacement/destruction, and connects refill callbacks
with the output as context and a weak implementation reference. The format
selection, playback range, sample conversion and buffering policy are unchanged.
The existing setup is a private helper so its actual Qt object ownership can be
observed without opening a physical audio stream.

The test-only `runSoundOutputOwnershipChecks` entry repeats output setup with
alternating formats sixteen times. It checks that the prior output and children
are destroyed at replacement and that the final output and children disappear
when the implementation owner retires. On Qt 6 it also requires one owned refill
timer. Only the exact expected missing-device warning from a headless constructor
is captured; other Qt messages retain their previous handler or are printed.

This fixture does not prove audible playback, backend shutdown under load,
sample-range correctness or overall RAM reduction. It must run through the
existing non-Windows native application driver. A narrow mutation can release
the old `unique_ptr` before replacement, and separately release the final output
before destruction: the corresponding lifetime assertion must fail. No mutation
result is claimed yet.

Qt documents that output destruction releases system resources and buffers;
its source also confirms the missing-device constructor warning and that the
internal write device becomes invalid after stop/restart:
[QAudioSink API](https://doc.qt.io/qt-6/qaudiosink.html),
[Qt 6.8.3 implementation](https://github.com/qt/qtmultimedia/blob/v6.8.3/src/multimedia/audio/qaudiosink.cpp).

## Windows callback ownership: confirmed, not repaired

Windows selects `common/tsound/tsound_nt.cpp`; the Qt change does not affect it.
The following source paths need one coordinated repair:

| Path | Existing behavior | Consequence |
| --- | --- | --- |
| `MyWaveOutCallbackThread`, `MM_WOM_DONE` | Calls `WavehdrQueue::popFront` with the returned header's counter, then waits for `m_closeDevice` before another unprepare/delete of the same header | `popFront` already frees the payload/header. The callback waits for an event signaled only after its own message loop exits. The normal first completion can block; if that wait is satisfied by another thread generation, later code uses/deletes released storage. |
| `WavehdrQueue::popFront` | The matched header is deleted without first calling `waveOutUnprepareHeader`; skipped headers are unprepared, but the loop can access `front()` after removing the last entry | Violates the WinMM buffer lifetime contract; an empty or unexpected completion sequence can access invalid queue state. |
| `TSoundOutputDevice::stop` and `WavehdrQueue::clear` | Reset returns device buffers, then clear frees queued headers while `MM_WOM_DONE` messages still carry their addresses | A late completion dereferences `pWaveHdr->dwUser` before checking whether the queue still owns it. The pending `m_items` list is simply cleared, losing its allocated headers. |
| Constructor/destructor | The close event starts unsignaled; the destructor waits on it even if no callback thread ever started | Constructing then destroying a usable device without playback can hang. |
| `doOpenDevice` | Discards the real thread handle immediately, does not handle `CreateThread` failure, and spins posting quit if device opening fails | No reliable join boundary; failed creation/queue readiness can hang. Format changes reuse one event across thread generations. |

These paths are reachable from ordinary soundtrack/Flipbook playback and from
owners that destroy their player, such as the Cast inspector. The exact observed
symptom on a particular device remains unqualified. Early construction can fail
when no audio hardware is installed, so a passing headless application fixture
does not exercise this protocol.

Microsoft requires successful unprepare before freeing playback storage. A reset
returns pending buffers and produces completion messages; it does not erase the
application's queued callbacks:
[unprepare contract](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutunprepareheader),
[reset contract](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutreset),
[completion message](https://learn.microsoft.com/en-us/windows/win32/multimedia/mm-wom-done).

## Proposed Windows repair boundary

Recommended: retain the WinMM backend and its public playback/scrubbing behavior,
but give each output implementation one explicitly owned callback thread and
serialize its buffer retirement. Replacing Windows output with Qt would remove
legacy code, but changes backend/device behavior and is a separate migration.

The proposed invariants are:

1. Keep the actual thread handle until it has exited; create the message queue
   before announcing readiness. An unopened or failed-start device has no thread
   to join. Joining happens outside locks needed by the callback and never from
   that same callback thread.
2. Each allocated header/payload has exactly one owner across prepared, queued,
   returned and retired states. Only that owner unprepares and deletes it. Failed
   prepare/write paths release their own storage; successful unprepare is required
   before release. `delete[]` always matches the audio byte allocation.
3. Completion resolves the returned address against live ownership before any
   dereference. Reset/stop marks playback as stopping before device reset, so
   returned buffers cannot be requeued for looping. Already-returned completion
   messages are drained before header addresses can be reused for new work.
4. Close retires all queued and pending buffers, posts the termination request,
   waits for actual callback retirement, and only then destroys thread handles,
   queue state and the implementation. Listener notifications must not outlive
   their listener registration; the current raw `EndPlayMsg` is another boundary
   requiring explicit treatment in the same review.
5. All queue/device access follows one documented lock/order rule. The current
   device-to-queue and queue-to-device acquisition paths cannot remain inverted.

Before implementation, review the thread protocol as a concrete design change.
The bounded CI fixture should fake only the WinMM/device boundary, exercise the
real queue and controller code, and retain diagnostics for these cases:

- create/destroy without open; failed thread creation; delayed message-queue
  readiness; device-open failure; repeated open/close;
- normal completion, multiple queued/pending buffers, looping then stop, format
  change, write/unprepare failure, and duplicate/late completion;
- stop/close while callbacks are pending, and listener removal before delivery;
- exact allocation/retirement counts, no header dereference after retirement,
  no callback self-join, and completion within the fixture's deadline.

Run Windows page heap or allocation-mismatch instrumentation on that fixture,
then qualify real mono/stereo playback, timeline scrubbing, cancellation and
device disconnect/reconnect on physical Windows hardware. Preserve original
failure output before diagnostics; do not add retries or loosen assertions.
