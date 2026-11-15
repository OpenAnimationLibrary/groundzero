# Audio resource lifetime follow-up

The current source review found independent playback lifetime defects. These are
not attributed to the unresolved Windows text-layout crash. No audio-device
session, local build or memory measurement has been executed for this follow-up.
At head `376d2e6ce`, Linux Qt5.15.3 and macOS Qt5.15.19 reached the F54 ownership
sentinel before separate later native failures (CI34/CI38). Qt6 timer execution
and the subsequent Windows repair remain unqualified.

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

## F60 Windows callback ownership: approved repair prepared for CI

Windows selects `common/tsound/tsound_nt.cpp`; the Qt change does not affect it.
The following source paths motivated the coordinated repair below:

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

## Approved Windows repair boundary

The user approved retaining the WinMM backend and its public playback/scrubbing behavior,
but give each output implementation one explicitly owned callback thread and
serialize its buffer retirement. Replacing Windows output with Qt would remove
legacy code, but changes backend/device behavior and is a separate migration.

The approved invariants are:

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
4. Close retires all queued and pending buffers. Implementation shutdown wakes
   and joins its callback thread before destroying thread handles, queue state
   and the implementation. An ordinary public `close()` retains the owned idle
   callback thread for a later `open()` on that same player. Listener notifications
   must not outlive their registration.
5. All queue/device access follows one documented lock/order rule. The current
   device-to-queue and queue-to-device acquisition paths cannot remain inverted.

The thread and buffer ownership design was approved and integrated in commit
`91d8060d3` as F60. R21 remains an alias, not another distinct defect. This change
postdates head `376d2e6ce`; its CI results do not qualify F60. Approval and source
review do not establish runtime correctness. The prepared implementation uses one
owned header/byte buffer record for pending, submitted and retiring states;
successful unprepare precedes erasing that record. Completion resolves an address
against live ownership before reading it. Stop clears looping before reset and
waits for a callback barrier before a new playback generation can allocate headers.
This covers completion messages already queued by a completed reset, including
duplicates. It does not claim to distinguish arbitrary driver messages posted
after that drain with an address reused by a later generation.

One operation mutex serializes public playback and retirement operations. The
callback takes only the state mutex; waits and joins occur outside that mutex.
The retained thread handle and queue-ready event provide the startup boundary.
Posting a barrier and marking final thread exit share the state mutex; accepted
barrier requests retain their own event until delivery or final draining. Failed
posting returns failure rather than joining a worker that has not been woken.
The final join also explicitly excludes the callback thread itself.

Each listener attachment has a separate registration. Detach removes that
registration under the state mutex, then waits for its delivery mutex outside
the state and operation locks. Delivery holds the recursive registration mutex
while invoking the listener, allowing self-detach and rejecting queued messages
from a previous attachment of the same address. Shutdown invalidates listeners
before taking the operation mutex. `EndPlayMsg` is sent from a stack object, so
the dispatcher's clone is the only allocated message owner.

Failed reset/unprepare/close retains the affected storage and disables playback
with a diagnostic. Successful explicit stop/close can recover a live owner; a
later callback can finish retiring a shutdown owner. New output construction is
blocked while failed cleanup remains, and the failed controller accepts no new
buffers. Retention is limited to the buffers/devices already owned at failure,
not a claim of a fixed byte limit. Existing players created before the failure
may independently reach this state. If the worker has already exited, an
allocation-free self-reference retains unsafe-to-free storage. An existing
external owner may retry close; without that owner, process restart is the
recovery boundary. No further callback can recover an exited worker.

Two separate caller ownership gaps are repaired across platforms: Flipbook
detaches itself and deletes its player during destruction; sound-column stop
deletes the player before clearing its pointer, and its destructor retires an
idle player too. These changes use the same existing public playback API. Format
selection, supported rates including 32000 Hz, sample extraction, loop behavior,
and scrub fade/crossfade/pending eviction policy are preserved.

The Windows-only `runWindowsSoundOutputOwnershipChecks` fixture is connected to
the existing native application's repeated-ownership checks, with no new CI job.
It injects WinMM calls and Win32 thread/message boundaries per new implementation
instance, while exercising the real controller, callback thread and listener
dispatcher. Fake state survives every device and thread using it. Authored cases
cover:

- create/destroy without open; failed thread creation; delayed message-queue
  readiness; device-open failure; repeated open/close;
- normal completion, loop replay, stop, format changes, prepare/write failures,
  reset/unprepare/close failure retention and later recovery;
- exact ordinary/loop sample bytes, scrub fade and decay order, submitted
  crossfade bytes, pending eviction and FIFO sample order;
- reset-queued duplicate completions before the next playback generation;
- queued listener removal and reattachment, reentrant self-detach/stop, and a
  concurrent detach waiting for delivery to finish;
- failed shutdown message posting, unexpected worker exit, and retained-owner
  recovery after exit;
- a barrier accepted immediately before thread exit and a concurrent close
  started while the final drain holds the state lock;
- balanced successful prepare/unprepare and device open/close counts. These
  are boundary ownership counts, not heap-allocation or process-RAM measurements.

The fixture has not been built or executed locally, following the repository's
CI-only rule. CI execution, page-heap instrumentation and fault mutations remain
pending. Source review verifies the self-join guard, but the fixture does not
invoke public shutdown from the callback thread. Its bounded gate/notification
waits do not prove that real device calls cannot stall inside a driver.
Production barriers and fixture thread joins remain unbounded; hang detection
therefore relies on the existing native-process CI timeout, not solely on the
fixture's five-second polling deadlines.

Run Windows page heap or allocation-mismatch instrumentation on that fixture,
then qualify real mono/stereo playback, timeline scrubbing, cancellation and
device disconnect/reconnect on physical Windows hardware. Preserve original
failure output before diagnostics; do not add retries or loosen assertions.
