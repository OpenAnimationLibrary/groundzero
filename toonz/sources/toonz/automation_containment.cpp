#include "automation_containment.h"
#include "automation.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <qt_windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/ucred.h>
#include <sys/un.h>
#endif
#endif

namespace GroundzeroAutomation {
#if defined(AUTOMATION_CONTAINMENT_TESTING) && !defined(_WIN32)
int automationContainmentKill(pid_t group, int signal);
#endif
namespace {

[[noreturn]] void refuse(const char* message) {
  throw Error("helper_containment_failed", QString::fromUtf8(message));
}

#ifdef _WIN32
bool ownJob() {
  const HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) refuse("Cannot create the native helper job; relaunch the worker.");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  const bool configured =
      SetHandleInformation(job, HANDLE_FLAG_INHERIT, 0) &&
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                              sizeof(limits));
  if (!configured || !AssignProcessToJobObject(job, GetCurrentProcess())) {
    const DWORD error = GetLastError();
    CloseHandle(job);
    throw Error("helper_containment_failed",
                "Cannot assign the native worker and helpers to its owned job.",
                {{"windows_error", QString::number(error)}});
  }
  // This noninheritable handle belongs to process teardown: closing it here
  // would terminate this worker, which is itself a member of the job.
  return true;
}
#else
using Clock                     = std::chrono::steady_clock;
using Frame                     = std::array<std::uint8_t, 44>;
constexpr int ControlDescriptor = 3;

#ifdef __APPLE__
pid_t peerProcess() {
  pid_t peer     = -1;
  socklen_t size = sizeof(peer);
  if (getsockopt(ControlDescriptor, SOL_LOCAL, LOCAL_PEERPID, &peer, &size) ||
      size != sizeof(peer))
    refuse("Cannot identify the private supervisor socket peer.");
  return peer;
}

uid_t peerOwner() {
  xucred credentials{};
  socklen_t size = sizeof(credentials);
  if (getsockopt(ControlDescriptor, SOL_LOCAL, LOCAL_PEERCRED, &credentials,
                 &size) ||
      size != sizeof(credentials) || credentials.cr_version != XUCRED_VERSION)
    refuse("Cannot verify the private supervisor socket credentials.");
  return credentials.cr_uid;
}
#endif

std::pair<pid_t, uid_t> peerIdentity() {
#ifdef __APPLE__
  return {peerProcess(), peerOwner()};
#elif defined(__linux__)
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  if (getsockopt(ControlDescriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                 &size) ||
      size != sizeof(credentials))
    refuse("Cannot verify the private supervisor socket credentials.");
  return {credentials.pid, credentials.uid};
#else
  refuse("Private helper supervision is unsupported on this platform.");
#endif
}

void requirePeer() {
  const auto [peer, owner] = peerIdentity();
  if (peer <= 0 || peer != getppid() || owner != geteuid() ||
      getpgrp() != peer || getsid(0) != peer)
    refuse(
        "Private supervisor does not own this worker's parent, user and "
        "group.");
}

void waitReady(short events, Clock::time_point deadline) {
  for (;;) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              Clock::now());
    if (remaining.count() <= 0)
      refuse("Private supervisor handshake exceeded five seconds.");
    pollfd socket{ControlDescriptor, events, 0};
    const int result = poll(&socket, 1, static_cast<int>(remaining.count()));
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0 || socket.revents & (POLLERR | POLLNVAL))
      refuse("Private supervisor handshake socket failed or timed out.");
    return;
  }
}

void transfer(std::uint8_t* bytes, size_t size, bool writing,
              Clock::time_point deadline) {
  size_t offset = 0;
  while (offset < size) {
    waitReady(writing ? POLLOUT : POLLIN, deadline);
    const ssize_t count = writing
                              ? send(ControlDescriptor, bytes + offset,
                                     size - offset, MSG_DONTWAIT | MSG_NOSIGNAL)
                              : recv(ControlDescriptor, bytes + offset,
                                     size - offset, MSG_DONTWAIT);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (count <= 0) refuse("Private supervisor closed during its handshake.");
    offset += static_cast<size_t>(count);
  }
}

std::uint32_t word(const Frame& frame, size_t offset) {
  return std::uint32_t(frame[offset]) |
         (std::uint32_t(frame[offset + 1]) << 8) |
         (std::uint32_t(frame[offset + 2]) << 16) |
         (std::uint32_t(frame[offset + 3]) << 24);
}

void requireNoExtraBytes() {
  std::uint8_t extra;
  ssize_t count;
  do {
    count = recv(ControlDescriptor, &extra, 1, MSG_DONTWAIT | MSG_PEEK);
  } while (count < 0 && errno == EINTR);
  if (count >= 0 || errno != EAGAIN)
    refuse(
        "Private supervisor sent extra bytes or closed its startup channel.");
}

void exchange(Clock::time_point deadline) {
  Frame frame{};
  transfer(frame.data(), frame.size(), false, deadline);
  if (word(frame, 0) != 0x475A4153 || word(frame, 4) != 1 ||
      word(frame, 8) != static_cast<std::uint32_t>(getppid()))
    refuse("Private supervisor handshake has an invalid version or identity.");
  requireNoExtraBytes();
  transfer(frame.data(), frame.size(), true, deadline);
  std::uint8_t acknowledgement = 0;
  transfer(&acknowledgement, 1, false, deadline);
  if (acknowledgement != 1)
    refuse("Private supervisor did not acknowledge the verified handshake.");
  requireNoExtraBytes();
  if (Clock::now() >= deadline)
    refuse("Private supervisor handshake exceeded five seconds.");
}

[[noreturn]] void terminateOwnedGroup() {
#ifdef AUTOMATION_CONTAINMENT_TESTING
  automationContainmentKill(-getpgrp(), SIGKILL);
#else
  kill(-getpgrp(), SIGKILL);
#endif
  _exit(125);
}

void guardAnchor() {
  for (;;) {
    pollfd socket{ControlDescriptor, POLLIN, 0};
    const int ready = poll(&socket, 1, -1);
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) terminateOwnedGroup();
    std::uint8_t value;
    const ssize_t count = recv(ControlDescriptor, &value, 1, MSG_DONTWAIT);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    terminateOwnedGroup();
  }
}

bool verifyAnchor() {
  const char* marker = std::getenv("GROUNDZERO_AUTOMATION_CONTROL_FD");
  if (!marker) return false;
  if (std::strcmp(marker, "3") != 0)
    refuse("Private supervisor descriptor marker must identify descriptor 3.");
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  const int flags     = fcntl(ControlDescriptor, F_GETFD);
  if (flags < 0 || fcntl(ControlDescriptor, F_SETFD, flags | FD_CLOEXEC) < 0)
    refuse(
        "Cannot isolate the private supervisor descriptor from helper exec.");
  try {
    requirePeer();
    exchange(deadline);
    std::thread(guardAnchor).detach();
  } catch (...) {
    close(ControlDescriptor);
    throw;
  }
  return true;
}
#endif
}  // namespace

bool establishAutomationContainment() {
#ifdef _WIN32
  return ownJob();
#else
  return verifyAnchor();
#endif
}

}  // namespace GroundzeroAutomation
