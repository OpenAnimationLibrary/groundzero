#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QImage>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
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
#include <windows.h>
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
    if (!limitWindowsImport()) {
      writeFile(output.filePath("error.txt"),
                "Cannot establish Windows brush worker resource limits");
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
      auto bytes = encode(imported.records[i], error);
      total += bytes.size();
      if (!error.isEmpty() || total > packageBudget ||
          !writeFile(output.filePath(QString::number(i) + ".gzbrush"), bytes))
        return 2;
    }
    QJsonArray diagnostics;
    for (const auto &message : imported.diagnostics)
      diagnostics.append(message);
    QByteArray manifest =
        QJsonDocument(QJsonObject{{"protocol", 1},
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

#if defined(_WIN32) && defined(RASTERBRUSH_SANDBOX_TESTING)
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

ImportResult importBrushProcess(const QString &executable, const QString &path,
                                QString &error,
                                const std::atomic_bool *cancelled,
                                size_t maximumDecodedBytes) {
  maximumDecodedBytes = std::min(maximumDecodedBytes, size_t(packageBudget));
  error.clear();
  auto stopped = [&] {
    return cancelled && cancelled->load(std::memory_order_relaxed);
  };
  if (stopped()) return {};
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = "Cannot create isolated brush import directory";
    return {};
  }
  QProcess process;
  process.setProgram(executable);
  process.setArguments({"--raster-brush-import-worker",
                        QFileInfo(path).absoluteFilePath(), directory.path()});
  process.setWorkingDirectory(directory.path());
  // Decoder console output is not part of the result protocol. Redirect it
  // at process creation: draining QProcess later still permits its internal
  // buffers to grow while a noisy decoder is running. Bounded error.txt and
  // result.json remain the only parent-visible diagnostics.
  process.setStandardOutputFile(QProcess::nullDevice());
  process.setStandardErrorFile(QProcess::nullDevice());
  process.start();
  if (!process.waitForStarted(5000)) {
    error = "Cannot start brush import worker: " + process.errorString();
    return {};
  }
  QElapsedTimer elapsed;
  elapsed.start();
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(50);
    if (stopped() || elapsed.elapsed() > 120000) {
      process.kill();
      process.waitForFinished(5000);
      if (!stopped()) error = "Brush import exceeded the 120-second limit";
      return {};
    }
  }
  if (stopped()) return {};
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    QFile diagnostic(directory.filePath("error.txt"));
    if (diagnostic.open(QIODevice::ReadOnly))
      error = QString::fromUtf8(diagnostic.read(16384));
    if (error.isEmpty())
      error = "Brush import worker failed or exceeded its resource limits";
    return {};
  }
  QFile manifest(directory.filePath("result.json"));
  if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > manifestBudget) {
    error = "Invalid brush import result";
    return {};
  }
  auto json   = QJsonDocument::fromJson(manifest.read(manifestBudget + 1));
  auto object = json.object();
  int count   = object.value("count").toInt(-1);
  if (!json.isObject() || object.value("protocol").toInt() != 1 || count < 1 ||
      count > 256 || !object.value("diagnostics").isArray()) {
    error = "Invalid brush import result protocol";
    return {};
  }
  ImportResult result;
  qint64 total  = 0;
  size_t pixels = 0;
  for (int i = 0; i < count; ++i) {
    if (stopped()) return {};
    const QString package = directory.filePath(QString::number(i) + ".gzbrush");
    QFileInfo info(package);
    total += info.size();
    if (info.isSymLink() || !info.isFile() || total > packageBudget) {
      error = "Brush import result exceeded its package budget";
      return {};
    }
    auto record =
        load(package, error, maximumDecodedBytes - pixels * sizeof(Pixel));
    if (!error.isEmpty()) return {};
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
  for (const auto &message : object.value("diagnostics").toArray()) {
    if (!message.isString()) {
      error = "Invalid brush import diagnostic";
      return {};
    }
    result.diagnostics << message.toString();
  }
  result.complete = true;
  return result;
}
}  // namespace RasterBrush
