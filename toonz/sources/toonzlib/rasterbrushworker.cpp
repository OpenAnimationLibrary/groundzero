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
#include <QSaveFile>
#include <QTemporaryDir>
#include <set>
#ifndef _WIN32
#include <sys/resource.h>
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
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace RasterBrush {
namespace {
constexpr qint64 packageBudget  = 256 * 1024 * 1024;
constexpr qint64 manifestBudget = 1024 * 1024;
bool writeFile(const QString &path, const QByteArray &bytes) {
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
         file.commit();
}
#ifdef __linux__
bool restrictLinuxImport(const QString &input, const QString &directory) {
#if defined(SYS_landlock_create_ruleset) && defined(SYS_landlock_add_rule) &&  \
    defined(SYS_landlock_restrict_self) &&                                     \
    (defined(__x86_64__) || defined(__aarch64__))
  // Landlock scopes the calling thread and its future children. Refuse a
  // process that already has another thread before applying the restrictions.
  if (QDir("/proc/self/task")
          .entryList(QDir::Dirs | QDir::NoDotAndDotDot)
          .size() != 1)
    return false;
  // ABI 3 controls truncation as well as ordinary reads/writes. There is no
  // fallback to unrestricted decoding when the kernel lacks this boundary.
  const int abi = int(syscall(SYS_landlock_create_ruleset, nullptr, 0,
                              LANDLOCK_CREATE_RULESET_VERSION));
  if (abi < 3) return false;
  constexpr uint64_t truncateAccess = uint64_t(1) << 14;
  constexpr uint64_t referAccess    = uint64_t(1) << 13;
  landlock_ruleset_attr attributes{};
  attributes.handled_access_fs = (uint64_t(1) << 15) - 1;
  const int rules              = int(
      syscall(SYS_landlock_create_ruleset, &attributes, sizeof(attributes), 0));
  if (rules < 0) return false;
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
  if (!restricted) return false;
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
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
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

#if defined(__linux__) && defined(RASTERBRUSH_SANDBOX_TESTING)
int probeImportSandbox(const QString &input, const QString &directory,
                       const QString &forbidden) {
  if (!prepareWorkerCodecs() || !restrictLinuxImport(input, directory))
    return 1;
  QFile allowed(input), denied(forbidden);
  if (!allowed.open(QIODevice::ReadOnly) || allowed.readAll() != "allowed" ||
      denied.open(QIODevice::ReadOnly) || denied.open(QIODevice::WriteOnly))
    return 2;
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
  return writeFile(QDir(directory).filePath("sandbox-passed"), "passed") ? 0
                                                                         : 7;
}
#endif

ImportResult importBrushProcess(const QString &executable, const QString &path,
                                QString &error,
                                const std::atomic_bool *cancelled) {
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
        load(package, error, size_t(packageBudget) - pixels * sizeof(Pixel));
    if (!error.isEmpty()) return {};
    std::set<const Image *> resources;
    auto account = [&](const std::shared_ptr<const Image> &image) {
      if (image && resources.insert(image.get()).second)
        pixels += image->pixels.size();
    };
    for (const auto &image : record.preset.tip.images) account(image);
    for (const auto &image : record.preset.secondary.tip.images) account(image);
    account(record.preset.texture.image);
    if (pixels > size_t(packageBudget) / sizeof(Pixel)) {
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
