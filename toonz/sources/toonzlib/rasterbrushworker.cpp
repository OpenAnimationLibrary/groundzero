#include "toonz/rasterbrushimport.h"
#include "rasterbrushresponse.h"
#include "rasterbrushtestobserver.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QImage>
#include <QDir>
#include <QElapsedTimer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include "rasterbrushmacsandbox.h"
#include <set>
#include <algorithm>
#include <cstdio>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include "rasterbrushwindowsandbox.h"
#endif
#ifdef __linux__
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace RasterBrush {
namespace {
constexpr qint64 packageBudget  = 256 * 1024 * 1024;
constexpr qint64 manifestBudget = 1024 * 1024;
bool writeFile(const QString &path, const QByteArray &bytes) {
  // Results are private staging files. The parent opens them only after a
  // successful worker exit and validates the complete manifest. NewOnly
  // prevents replacing any existing entry, including a symlink. An unnamed
  // QSaveFile temporary would need linkat outside the deliberately small
  // syscall interface; no per-file publication occurs inside this protocol.
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
         file.write(bytes) == bytes.size() && file.flush();
}
#ifdef _WIN32
bool limitWindowsImport() {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME |
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
      JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
      60LL * 10000000;
  limits.ProcessMemoryLimit = SIZE_T(2) * 1024 * 1024 * 1024;
  const bool ok =
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                              sizeof(limits)) &&
      AssignProcessToJobObject(job, GetCurrentProcess());
  // The unnamed job remains alive while this process is a member. Retaining
  // no handle prevents decoder code from changing its limits. No breakaway
  // flag is granted, and the limit rejects ordinary CreateProcess children.
  CloseHandle(job);
  return ok;
}
#endif
#ifdef __linux__
bool limitLinuxImportMemory() {
  // Bound the whole decoder process independently of decoded-resource budgets.
  // The allowance includes loaded application/Qt libraries and codec scratch.
  constexpr rlim_t addressLimit = rlim_t(2) * 1024 * 1024 * 1024;
  rlimit addressSpace{addressLimit, addressLimit};
  rlimit fileSize{rlim_t(packageBudget), rlim_t(packageBudget)};
  return setrlimit(RLIMIT_AS, &addressSpace) == 0 &&
         setrlimit(RLIMIT_FSIZE, &fileSize) == 0;
}
bool restrictLinuxImport(const QString &input, const QString &directory) {
#if defined(SYS_landlock_create_ruleset) && defined(SYS_landlock_add_rule) &&  \
    defined(SYS_landlock_restrict_self) &&                                     \
    (defined(__x86_64__) || defined(__aarch64__))
  auto failed = [](const char *stage) {
    std::fprintf(stderr, "Import isolation failed at %s (errno %d)\n", stage,
                 errno);
    return false;
  };
  // Landlock scopes the calling thread and its future children. Refuse a
  // process that already has another thread before applying the restrictions.
  if (QDir("/proc/self/task")
          .entryList(QDir::Dirs | QDir::NoDotAndDotDot)
          .size() != 1)
    return failed("single-thread precondition");
  // ABI 3 controls truncation as well as ordinary reads/writes. There is no
  // fallback to unrestricted decoding when the kernel lacks this boundary.
  const int abi = int(syscall(SYS_landlock_create_ruleset, nullptr, 0,
                              LANDLOCK_CREATE_RULESET_VERSION));
  if (abi < 3) return failed("Landlock ABI >= 3");
  constexpr uint64_t truncateAccess = uint64_t(1) << 14;
  constexpr uint64_t referAccess    = uint64_t(1) << 13;
  landlock_ruleset_attr attributes{};
  attributes.handled_access_fs = (uint64_t(1) << 15) - 1;
  const int rules              = int(
      syscall(SYS_landlock_create_ruleset, &attributes, sizeof(attributes), 0));
  if (rules < 0) return failed("Landlock ruleset creation");
  auto allow = [&](const QString &path, uint64_t access) {
    const QByteArray native = QFile::encodeName(path);
    const int fd            = open(native.constData(), O_PATH | O_CLOEXEC);
    if (fd < 0) return false;
    landlock_path_beneath_attr rule{};
    rule.parent_fd      = fd;
    rule.allowed_access = access;
    const bool ok       = syscall(SYS_landlock_add_rule, rules,
                                  LANDLOCK_RULE_PATH_BENEATH, &rule, 0) == 0;
    close(fd);
    return ok;
  };
  const bool paths =
      allow(input, LANDLOCK_ACCESS_FS_READ_FILE) &&
      allow(directory,
            LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_MAKE_REG |
                LANDLOCK_ACCESS_FS_REMOVE_FILE | truncateAccess | referAccess);
  const bool restricted = paths &&
                          prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
                          syscall(SYS_landlock_restrict_self, rules, 0) == 0;
  close(rules);
  if (!restricted) return failed("Landlock path restrictions");
    // A small syscall allowlist complements Landlock: no sockets, subprocesses,
    // executable loading, ptrace, cross-process memory access or mount changes.
    // All codec initialization happens before this boundary; the worker is a
    // QCoreApplication and decoding/encoding here is single-threaded.
#if defined(__x86_64__)
  constexpr unsigned architecture = AUDIT_ARCH_X86_64;
#else
  constexpr unsigned architecture = AUDIT_ARCH_AARCH64;
#endif
  std::vector<sock_filter> filter = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, architecture, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr))};
  const int permitted[] = {
      SYS_read,         SYS_write,        SYS_readv,          SYS_writev,
      SYS_close,        SYS_lseek,        SYS_openat,         SYS_fstat,
      SYS_newfstatat,   SYS_fcntl,        SYS_getdents64,     SYS_fsync,
      SYS_fdatasync,    SYS_ftruncate,    SYS_renameat,       SYS_unlinkat,
      SYS_mmap,         SYS_munmap,       SYS_mprotect,       SYS_mremap,
      SYS_brk,          SYS_madvise,      SYS_futex,          SYS_clock_gettime,
      SYS_gettimeofday, SYS_rt_sigaction, SYS_rt_sigprocmask, SYS_rt_sigreturn,
      SYS_sigaltstack,  SYS_getpid,       SYS_gettid,         SYS_getuid,
      SYS_geteuid,      SYS_getgid,       SYS_getegid,        SYS_sched_yield,
      SYS_exit,         SYS_exit_group,   SYS_getrandom,
#ifdef SYS_statx
      SYS_statx,
#endif
#ifdef SYS_renameat2
      SYS_renameat2,
#endif
#ifdef SYS_open
      SYS_open,
#endif
#ifdef SYS_stat
      SYS_stat,
#endif
#ifdef SYS_lstat
      SYS_lstat,
#endif
#ifdef SYS_rename
      SYS_rename,
#endif
#ifdef SYS_unlink
      SYS_unlink,
#endif
#ifdef SYS_access
      SYS_access,
#endif
#ifdef SYS_faccessat
      SYS_faccessat,
#endif
#ifdef SYS_faccessat2
      SYS_faccessat2,
#endif
#ifdef SYS_readlink
      SYS_readlink,
#endif
#ifdef SYS_readlinkat
      SYS_readlinkat,
#endif
  };
  for (int number : permitted) {
    filter.push_back(
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, unsigned(number), 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  }
  filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM));
  sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0 ||
         failed("seccomp installation");
#else
  return false;
#endif
}
#endif
bool prepareWorkerCodecs() {
  QImage probe(1, 1, QImage::Format_ARGB32);
  probe.fill(Qt::transparent);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  return buffer.open(QIODevice::WriteOnly) && probe.save(&buffer, "PNG") &&
         !QImage::fromData(bytes, "PNG").isNull();
}
}  // namespace

int runImportWorker(const QString &input, const QString &directory) {
  // Establish restrictions before any untrusted bytes are decoded.
#ifdef __APPLE__
  if (!macImportEntitlement()) return 2;
#endif
#ifndef _WIN32
  rlimit noCore{0, 0};
  if (setrlimit(RLIMIT_CORE, &noCore) != 0) return 2;
  rlimit cpu{60, 60};
  if (setrlimit(RLIMIT_CPU, &cpu) != 0) return 2;
#endif
  try {
    QDir output(directory);
    if (!output.exists() || QFileInfo(directory).isSymLink() ||
        !output
             .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
             .empty())
      return 2;
#ifdef __linux__
    if (!limitLinuxImportMemory()) return 2;
#endif
#ifdef _WIN32
    if (!isWindowsImportContainer() || !limitWindowsImport()) {
      writeFile(
          output.filePath("error.txt"),
          "Cannot establish Windows brush AppContainer and resource limits");
      return 2;
    }
#endif
    if (!prepareWorkerCodecs()) return 2;
#ifdef __linux__
    if (!restrictLinuxImport(input, directory)) {
      writeFile(
          output.filePath("error.txt"),
          "Brush import requires Linux Landlock ABI 3 and seccomp isolation");
      return 2;
    }
#endif
    QString error;
    auto imported = importBrushFile(input, error);
    if (!error.isEmpty() || !imported.complete || imported.records.empty() ||
        imported.records.size() > 256) {
      writeFile(output.filePath("error.txt"), error.left(4096).toUtf8());
      return 1;
    }
    qint64 total = 0;
    for (size_t i = 0; i < imported.records.size(); ++i) {
      auto bytes = encodeWorkerRecord(imported.records[i], directory, error);
      total += bytes.size();
      if (!error.isEmpty() || total > packageBudget ||
          !writeFile(output.filePath(QString::number(i) + ".gzbrush"), bytes))
        return 2;
    }
    QJsonArray diagnostics;
    for (const auto &message : imported.diagnostics)
      diagnostics.append(message);
    QByteArray manifest =
        QJsonDocument(QJsonObject{{"protocol", 2},
                                  {"count", int(imported.records.size())},
                                  {"diagnostics", diagnostics}})
            .toJson(QJsonDocument::Compact);
    if (manifest.size() > manifestBudget ||
        !writeFile(output.filePath("result.json"), manifest))
      return 2;
    return 0;
  } catch (...) {
    return 2;
  }
}

#if defined(__APPLE__) && defined(RASTERBRUSH_SANDBOX_TESTING)
int probeMacWorkerTransport() {
  auto accepts = [](const QByteArray &bytes) {
    QTemporaryDir directory;
    QTemporaryFile wire;
    QString error;
    return directory.isValid() && wire.open() &&
           wire.write(bytes) == bytes.size() && wire.flush() &&
           unpackMacWorkerResponse(wire, directory.path(), error);
  };
  auto number = [](quint32 n) {
    QByteArray bytes(4, char(0));
    qToBigEndian<quint32>(n, bytes.data());
    return bytes;
  };
  const QByteArray entry = number(11) + number(2) + "result.json" + "{}";
  const QByteArray valid = QByteArray("GZBW0001") + number(1) + entry;
  if (!accepts(valid) || accepts(valid + "trailing") ||
      accepts(valid.left(valid.size() - 1)) ||
      accepts(QByteArray("GZBW0001") + number(66001)) ||
      accepts(QByteArray("GZBW0001") + number(2) + entry + entry) ||
      accepts(QByteArray("GZBW0001") + number(1) + number(9) + number(1) +
              "../escape" + "x") ||
      accepts(QByteArray("GZBW0001") + number(1) + number(11) +
              number(0xffffffffu) + "result.json"))
    return 1;
  return 0;
}
#endif

#if defined(_WIN32) && defined(RASTERBRUSH_SANDBOX_TESTING)
int probeWindowsImportSandboxWorker(const QString &input, const QString &output,
                                    const QString &forbidden) {
  if (!isWindowsImportContainer() || !limitWindowsImport()) return 1;
  if (QFileInfo(input).fileName() == "overlap") {
    if (!writeFile(QDir(output).filePath("entered"),
                   QByteArray::number(QDateTime::currentMSecsSinceEpoch())))
      return 14;
    Sleep(1000);  // Test-only barrier: keep the first import alive while
                  // another waits.
  }
  QFile permitted(input), denied(forbidden);
  if (!permitted.open(QIODevice::ReadOnly) ||
      permitted.readAll() != "allowed" || denied.open(QIODevice::ReadOnly) ||
      denied.open(QIODevice::WriteOnly))
    return 2;
  permitted.close();
  if (permitted.open(QIODevice::WriteOnly)) return 3;
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 4;
  SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socketHandle == INVALID_SOCKET) return 5;
  sockaddr_in address{};
  address.sin_family      = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port        = htons(9);
  const int connected =
      connect(socketHandle, reinterpret_cast<const sockaddr *>(&address),
              sizeof(address));
  const int networkError = WSAGetLastError();
  closesocket(socketHandle);
  WSACleanup();
  // Connection refusal alone is not evidence of isolation. Windows must deny
  // the capability, independently of whether any service listens at the port.
  if (connected == 0 || networkError != WSAEACCES) {
    std::fprintf(stderr,
                 "AppContainer loopback denial probe: connect=%d Winsock=%d "
                 "(required WSAEACCES=%d)\n",
                 connected, networkError, WSAEACCES);
    return 6;
  }
  return writeFile(QDir(output).filePath("sandbox-passed"), "passed") ? 0 : 7;
}
int probeWindowsImportSandbox(const QString &executable, const QString &input,
                              const QString &output, const QString &forbidden) {
  QString error;
  WindowsImportSandbox sandbox;
  if (!prepareWorkerCodecs() ||
      !sandbox.prepare(executable, input, output, error)) {
    std::fprintf(stderr, "Windows sandbox preparation failed: %s (Win32 %lu)\n",
                 error.toUtf8().constData(),
                 static_cast<unsigned long>(GetLastError()));
    return 10;
  }
  QProcess process;
  sandbox.configure(process);
  process.setWorkingDirectory(output);
  process.setStandardOutputFile(QProcess::nullDevice());
  // This test-only helper prints bounded stage diagnostics. Forward them to
  // the probe parent, whose output the regression driver captures.
  process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
  process.start(sandbox.program, {"--test-windows-sandbox-worker",
                                  sandbox.input, output, forbidden});
  if (!process.waitForStarted(5000)) {
    std::fprintf(stderr, "Windows sandbox launch failed: %s; modifier=%s\n",
                 process.errorString().toUtf8().constData(),
                 sandbox.launchError.toUtf8().constData());
    return 15;
  }
  if (!process.waitForFinished(15000)) {
    process.kill();
    process.waitForFinished(5000);
    return 11;
  }
  const int status =
      process.exitStatus() == QProcess::NormalExit ? process.exitCode() : 12;
  if (!sandbox.close()) return 13;
  if (status)
    std::fprintf(stderr, "Windows sandbox child exit=%d status=%d\n",
                 process.exitCode(), int(process.exitStatus()));
  return status;
}
int probeWindowsImportLimits() {
  if (!limitWindowsImport()) return 1;
  void *excess = VirtualAlloc(nullptr, SIZE_T(2) * 1024 * 1024 * 1024,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (excess) {
    VirtualFree(excess, 0, MEM_RELEASE);
    return 2;
  }
  QProcess child;
  child.start(QCoreApplication::applicationFilePath(), {"--test-worker-ping"});
  if (child.waitForStarted(5000)) {
    child.kill();
    child.waitForFinished(5000);
    return 3;
  }
  return 0;
}
#endif
#if defined(__linux__) && defined(RASTERBRUSH_SANDBOX_TESTING)
int probeImportSandbox(const QString &input, const QString &directory,
                       const QString &forbidden) {
  if (!limitLinuxImportMemory() || !prepareWorkerCodecs() ||
      !restrictLinuxImport(input, directory))
    return 1;
  QFile allowed(input), denied(forbidden);
  if (!allowed.open(QIODevice::ReadOnly) || allowed.readAll() != "allowed" ||
      denied.open(QIODevice::ReadOnly) || denied.open(QIODevice::WriteOnly))
    return 2;
  allowed.close();
  if (allowed.open(QIODevice::WriteOnly)) return 8;
  const QByteArray inputPath = QFile::encodeName(input);
  const int readonly = open(inputPath.constData(), O_RDONLY | O_CLOEXEC);
  if (readonly < 0) return 9;
  const long changedMode = syscall(SYS_fchmod, readonly, 0600);
  const int modeError    = errno;
  close(readonly);
  if (changedMode == 0 || modeError != EPERM) return 10;
  // Reserving the complete address allowance must fail because the process
  // already maps its executable and libraries; this does not fault in pages.
  void *excess = mmap(nullptr, size_t(2) * 1024 * 1024 * 1024, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (excess != MAP_FAILED) {
    munmap(excess, size_t(2) * 1024 * 1024 * 1024);
    return 11;
  }
  if (errno != ENOMEM) return 12;
  const int network = socket(AF_INET, SOCK_STREAM, 0);
  if (network >= 0) {
    close(network);
    return 3;
  }
  if (errno != EPERM) return 4;
  const long child = syscall(SYS_clone, 0, nullptr, nullptr, nullptr, 0);
  if (child == 0) _exit(5);
  if (child >= 0 || errno != EPERM) return 5;
  const char *argv[] = {"true", nullptr};
  syscall(SYS_execve, "/bin/true", argv, nullptr);
  if (errno != EPERM) return 6;
  QFile marker(QDir(directory).filePath("sandbox-passed"));
  if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      marker.write("passed") != 6 || !marker.flush()) {
    const QByteArray reason = marker.errorString().toUtf8();
    write(STDERR_FILENO, reason.constData(), size_t(reason.size()));
    return 7;
  }
  return 0;
}
#endif

#ifdef RASTERBRUSH_SANDBOX_TESTING
namespace {
thread_local ImportCancellationObserver cancellationObserver;
}
void setImportCancellationObserver(ImportCancellationObserver observer) {
  cancellationObserver = std::move(observer);
}
void notifyImportCancellationObserver(const char *phase, const QString &path,
                                      qint64 processId) {
  if (cancellationObserver) cancellationObserver(phase, path, processId);
}
#endif

ImportResult importBrushProcess(const QString &executable, const QString &path,
                                QString &error,
                                const std::atomic_bool *cancelled,
                                size_t maximumDecodedBytes) {
  maximumDecodedBytes = std::min(maximumDecodedBytes, size_t(packageBudget));
  error.clear();
  QElapsedTimer elapsed;
  elapsed.start();
  auto stopped = [&] {
    return cancelled && cancelled->load(std::memory_order_relaxed);
  };
  if (stopped()) return {};
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = "Cannot create isolated brush import directory";
    return {};
  }
  Detail::RegularResponseDirectory responses;
  if (!responses.open(directory.path(), error)) return {};
#ifdef _WIN32
  // Declared before QProcess: on every early return the child is destroyed
  // before its ephemeral permissions and AppContainer profile are removed.
  WindowsImportSandbox sandbox;
  if (!prepareWorkerCodecs() ||
      !sandbox.prepare(executable, path, directory.path(), error, cancelled,
                       &elapsed)) {
    if (stopped()) return {};
    if (error.isEmpty()) error = "Cannot initialize trusted brush codecs";
    return {};
  }
  const QString workerInput = sandbox.input;
#else
  const QString workerInput = QFileInfo(path).absoluteFilePath();
#endif
#ifdef __APPLE__
  QTemporaryDir transport;
  QTemporaryFile staged(transport.filePath("input-XXXXXX"));
  QTemporaryFile wire(transport.filePath("response-XXXXXX"));
  QFile sentinel(transport.filePath("private-probe"));
  auto inputBytes = readBoundedRegularFile(path, 64LL * 1024 * 1024, error);
  if (!transport.isValid() || !error.isEmpty() || !staged.open() ||
      staged.write(inputBytes) != inputBytes.size() || !staged.flush() ||
      !wire.open() ||
      !sentinel.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      sentinel.write("private") != 7 || !sentinel.flush()) {
    if (error.isEmpty()) error = "Cannot stage sandboxed brush transport";
    return {};
  }
  sentinel.close();
  inputBytes.clear();
#endif
  QProcess process;
#ifdef _WIN32
  sandbox.configure(process);
#endif
#ifdef _WIN32
  process.setProgram(sandbox.program);
#else
  process.setProgram(executable);
#endif
  process.setArguments(
      {"--raster-brush-import-worker", workerInput, directory.path()});
  process.setWorkingDirectory(directory.path());
  // Decoder console output is not part of the result protocol. Redirect it
  // at process creation: draining QProcess later still permits its internal
  // buffers to grow while a noisy decoder is running. Bounded error.txt and
  // result.json remain the only parent-visible diagnostics.
  process.setStandardOutputFile(QProcess::nullDevice());
  process.setStandardErrorFile(QProcess::nullDevice());
#ifdef __APPLE__
  const QFileInfo requested(executable);
  if (!requested.isExecutable()) {
    error = "Brush worker application is missing or not executable";
    return {};
  }
  const QString helper =
      requested.fileName() == "rasterbrushworker_test"
          ? requested.absoluteFilePath()
          : requested.dir().filePath("rasterbrushimport-helper");
  process.setProgram(helper);
  process.setArguments({"--raster-brush-import-stream",
                        QFileInfo(path).fileName(), sentinel.fileName()});
  process.setStandardInputFile(staged.fileName());
  process.setStandardOutputFile(wire.fileName());
  process.setWorkingDirectory("/");
  // Loader paths are established by the helper's build/package rpaths. Do not
  // pass parent secrets or DYLD injection variables into the sandbox.
  QProcessEnvironment helperEnvironment;
  helperEnvironment.insert("MallocNanoZone", "0");
  process.setProcessEnvironment(helperEnvironment);
#endif
#ifdef RASTERBRUSH_SANDBOX_TESTING
  notifyImportCancellationObserver("prepared", {}, 0);
#endif
  if (stopped()) return {};
  if (elapsed.elapsed() >= 120000) {
    error = "Brush import exceeded the 120-second limit during preparation";
    return {};
  }
  process.start();
  if (!process.waitForStarted(int(std::max(
          qint64(0), std::min(qint64(5000), 120000 - elapsed.elapsed()))))) {
    error = "Cannot start brush import worker: " + process.errorString();
    return {};
  }
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(50);
#ifdef RASTERBRUSH_SANDBOX_TESTING
    notifyImportCancellationObserver("running",
#ifdef __APPLE__
                                     wire.fileName(),
#else
                                     directory.filePath("cancel-entered"),
#endif
                                     process.processId());
#endif
#ifdef __APPLE__
    if (process.state() != QProcess::NotRunning) {
      uint64_t footprint  = 0;
      const bool observed = macImportFootprint(process.processId(), footprint);
      if (!observed || footprint > macFootprintWatchdog) {
        // Exit can race proc_pid_rusage. Reap once before declaring observation
        // failure; a still-running child must remain observable or be killed.
        process.waitForFinished(0);
        if (process.state() != QProcess::NotRunning) {
          process.kill();
          process.waitForFinished(5000);
          error = observed
                      ? "Brush helper exceeded its 1 GiB footprint watchdog"
                      : "Cannot observe sandboxed brush helper memory";
          return {};
        }
      }
    }
#endif
    if (stopped() || elapsed.elapsed() > 120000) {
      process.kill();
      process.waitForFinished(5000);
      if (!stopped()) error = "Brush import exceeded the 120-second limit";
      return {};
    }
  }
  if (stopped()) return {};
#ifdef __APPLE__
  if (process.exitStatus() != QProcess::NormalExit ||
      !unpackMacWorkerResponse(wire, directory.path(), error, cancelled,
                               &elapsed)) {
    if (stopped()) return {};
    if (error.isEmpty()) error = "Sandboxed brush helper failed";
    return {};
  }
#endif
  return readWorkerResponse(
      responses, process.exitStatus() == QProcess::NormalExit,
      process.exitCode(), error, maximumDecodedBytes, cancelled, &elapsed);
}

ImportResult readWorkerResponse(
    const Detail::RegularResponseDirectory &responses, bool normalExit,
    int exitCode, QString &error, size_t maximumDecodedBytes,
    const std::atomic_bool *cancelled, const QElapsedTimer *elapsed) {
  error.clear();
  maximumDecodedBytes  = std::min(maximumDecodedBytes, size_t(packageBudget));
  auto responseStopped = [&] {
    if (cancelled && cancelled->load(std::memory_order_relaxed)) return true;
    if (!elapsed || elapsed->elapsed() <= 120000) return false;
    error = "Brush import exceeded the 120-second limit";
    return true;
  };
  if (responseStopped()) return {};
  if (!normalExit || exitCode != 0) {
    QString readError;
    const auto diagnostic =
        Detail::readResponseFile(responses, "error.txt", 16384, readError);
    if (readError.isEmpty()) error = QString::fromUtf8(diagnostic);
    if (error.isEmpty())
      error = QString("Brush import worker failed (exit %1): %2")
                  .arg(exitCode)
                  .arg(readError.isEmpty()
                           ? QString("no diagnostic or resource limit exceeded")
                           : readError);
    return {};
  }
  const auto manifest =
      Detail::readResponseFile(responses, "result.json", manifestBudget, error);
  if (!error.isEmpty()) return {};
  auto json              = QJsonDocument::fromJson(manifest);
  auto object            = json.object();
  int count              = object.value("count").toInt(-1);
  const auto diagnostics = object.value("diagnostics").toArray();
  const bool knownFields = object.size() == 3 && object.contains("protocol") &&
                           object.contains("count") &&
                           object.contains("diagnostics");
  if (!json.isObject() || !knownFields ||
      !object.value("protocol").isDouble() ||
      object.value("protocol").toDouble() != 2 ||
      !object.value("count").isDouble() ||
      object.value("count").toDouble() != count || count < 1 || count > 256 ||
      !object.value("diagnostics").isArray() || diagnostics.size() > 1024) {
    error = "Invalid brush import result protocol";
    return {};
  }
  ImportResult result;
  qint64 total  = 0;
  size_t pixels = 0;
  for (int i = 0; i < count; ++i) {
    if (responseStopped()) return {};
    const auto metadata = Detail::readResponseFile(
        responses, QString::number(i) + ".gzbrush",
        std::min(qint64(64 * 1024 * 1024), packageBudget - total), error);
    if (!error.isEmpty()) return {};
    total += metadata.size();
    auto record = decodeWorkerRecordInDirectory(
        metadata, responses, error,
        maximumDecodedBytes - pixels * sizeof(Pixel));
    if (!error.isEmpty() || responseStopped()) return {};
    std::set<const Image *> resources;
    auto account = [&](const std::shared_ptr<const Image> &image) {
      if (image && resources.insert(image.get()).second)
        pixels += image->pixels.size();
    };
    for (const auto &image : record.preset.tip.images) account(image);
    for (const auto &image : record.preset.secondary.tip.images) account(image);
    account(record.preset.texture.image);
    if (pixels > maximumDecodedBytes / sizeof(Pixel)) {
      error = "Brush import result exceeded its decoded image budget";
      return {};
    }
    result.records.push_back(std::move(record));
  }
  for (const auto &message : diagnostics) {
    if (!message.isString() || message.toString().size() > 16384) {
      error = "Invalid brush import diagnostic";
      return {};
    }
    result.diagnostics << message.toString();
  }
  if (responseStopped()) return {};
  result.complete = true;
  return result;
}
}  // namespace RasterBrush
