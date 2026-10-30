#pragma once
#ifdef __APPLE__
#include <Security/SecTask.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include <set>
#include <algorithm>
#include <QElapsedTimer>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <libproc.h>
#include <cstdio>

namespace RasterBrush {
namespace {
constexpr qint64 macWireLimit = 256LL * 1024 * 1024;
// Darwin includes trusted shared-cache mappings in virtual_size. Bound new
// mapping headroom rather than pretending Linux's absolute 2 GiB fits this ABI.
constexpr uint64_t macMappingHeadroom = uint64_t(1024) * 1024 * 1024;
constexpr uint64_t macMaximumTrustedMappings =
    uint64_t(64) * 1024 * 1024 * 1024;
constexpr uint64_t macFootprintWatchdog = uint64_t(1024) * 1024 * 1024;
bool limitMacImportMemory() {
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS ||
      info.virtual_size > macMaximumTrustedMappings)
    return false;
  const rlim_t cap = rlim_t(info.virtual_size + macMappingHeadroom);
  rlimit addressSpace{cap, cap};
  if (setrlimit(RLIMIT_AS, &addressSpace)) return false;
  rlimit raised{cap + 65536, cap + 65536};
  // A privileged host must not silently make this a reversible limit.
  if (setrlimit(RLIMIT_AS, &raised) == 0) return false;
  if (errno != EPERM && errno != EACCES) return false;
  std::fprintf(stderr, "Brush macOS virtual baseline=%llu cap=%llu bytes\n",
               static_cast<unsigned long long>(info.virtual_size),
               static_cast<unsigned long long>(cap));
  // Both POSIX and Mach allocation entry points must enforce the kernel map
  // limit. These reservations do not touch physical pages. Probe before bytes.
  errno = 0;
  void *excess =
      mmap(nullptr, size_t(cap), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  const int mmapError = errno;
  if (excess != MAP_FAILED) {
    munmap(excess, size_t(cap));
    return false;
  }
  if (mmapError != ENOMEM) return false;
  mach_vm_address_t address      = 0;
  const kern_return_t allocation = mach_vm_allocate(
      mach_task_self(), &address, mach_vm_size_t(cap), VM_FLAGS_ANYWHERE);
  if (allocation == KERN_SUCCESS) {
    mach_vm_deallocate(mach_task_self(), address, mach_vm_size_t(cap));
    return false;
  }
  if (allocation != KERN_NO_SPACE && allocation != KERN_RESOURCE_SHORTAGE)
    return false;
  // A normal small allocation must still work. A blanket allocator failure
  // does not qualify an operational importer boundary.
  void *ordinary = mmap(nullptr, 65536, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
  if (ordinary == MAP_FAILED) return false;
  static_cast<volatile unsigned char *>(ordinary)[0] = 1;
  return munmap(ordinary, 65536) == 0;
}
bool macImportFootprint(qint64 pid, uint64_t &bytes) {
  rusage_info_v2 usage{};
  if (pid <= 0 || proc_pid_rusage(pid_t(pid), RUSAGE_INFO_V2,
                                  reinterpret_cast<rusage_info_t *>(&usage)))
    return false;
  bytes = usage.ri_phys_footprint;
  return true;
}
bool macImportEntitlement() {
  SecTaskRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
  if (!task) return false;
  CFTypeRef value = SecTaskCopyValueForEntitlement(
      task, CFSTR("com.apple.security.app-sandbox"), nullptr);
  const bool enabled = value && CFEqual(value, kCFBooleanTrue);
  if (value) CFRelease(value);
  CFRelease(task);
  return enabled;
}
bool macWireName(const QString &name) {
  static const QRegularExpression pattern(
      "^(?:result\\.json|error\\.txt|[0-9]{1,3}\\.gzbrush|[a-f0-9]{64}\\.rgba8)"
      "$");
  return pattern.match(name).hasMatch();
}
// Streaming transport copies opaque private records, never decoding a foreign
// format in the parent. All output is recreated as regular files by the parent.
// The descriptor remains open throughout launch/read: no worker-controlled path
// is reopened. Limits are checked before allocation, including entry count.
bool unpackMacWorkerResponse(QFile &wire, const QString &directory,
                             QString &error,
                             const std::atomic_bool *cancelled = nullptr,
                             const QElapsedTimer *elapsed      = nullptr) {
  auto invalid = [&] {
    error = "Invalid or oversized sandboxed brush response";
    return false;
  };
  auto stopped = [&] {
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
      error.clear();
      return true;
    }
    if (elapsed && elapsed->elapsed() > 120000) {
      error = "Brush import exceeded the 120-second limit";
      return true;
    }
    return false;
  };
  if (stopped()) return false;
  if (wire.size() > macWireLimit || !wire.seek(0) || wire.read(8) != "GZBW0001")
    return invalid();
  auto number = wire.read(4);
  if (number.size() != 4) return invalid();
  const quint32 count = qFromBigEndian<quint32>(number.constData());
  if (!count || count > 66000) return invalid();
  std::set<QString> names;
  for (quint32 i = 0; i < count; ++i) {
    if (stopped()) return false;
    auto header = wire.read(8);
    if (header.size() != 8) return invalid();
    const quint32 nameSize = qFromBigEndian<quint32>(header.constData());
    quint32 remaining      = qFromBigEndian<quint32>(header.constData() + 4);
    if (!nameSize || nameSize > 80 || remaining > macWireLimit ||
        qint64(nameSize) + remaining > wire.size() - wire.pos())
      return invalid();
    const QByteArray encodedName = wire.read(nameSize);
    const QString name           = QString::fromLatin1(encodedName);
    if (!macWireName(name) || !names.insert(name).second) return invalid();
    QFile output(QDir(directory).filePath(name));
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
      return invalid();
    while (remaining) {
      if (stopped()) return false;
      auto bytes = wire.read(std::min(quint32(65536), remaining));
      if (bytes.isEmpty() || output.write(bytes) != bytes.size())
        return invalid();
      remaining -= quint32(bytes.size());
    }
    if (!output.flush()) return invalid();
  }
  return wire.pos() == wire.size() ? true : invalid();
}

bool macImportBoundaryProbe(const QString &forbidden) {
  if (!macImportEntitlement()) return false;
  QFile file(forbidden);
  if (file.open(QIODevice::ReadOnly) || file.open(QIODevice::WriteOnly))
    return false;
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return errno == EPERM || errno == EACCES;
  sockaddr_in address{};
  address.sin_len         = sizeof(address);
  address.sin_family      = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port        = htons(9);
  const int result =
      connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  const int failure = errno;
  close(fd);
  return result < 0 && (failure == EPERM || failure == EACCES);
}
}  // namespace

int runMacImportStream(const QString &name, const QString &forbidden,
                       int (*worker)(const QString &, const QString &)) {
  // The independently signed helper enters App Sandbox at exec, before Qt or
  // any decoder runs. These checks additionally fail closed if packaging lost
  // its entitlement or accidentally added broad file/network privileges.
  if (!macImportBoundaryProbe(forbidden) ||
      name != QFileInfo(name).fileName() || name.isEmpty() || name == "." ||
      name == "..")
    return 2;
  rlimit core{0, 0}, cpu{60, 60},
      outputLimit{rlim_t(macWireLimit), rlim_t(macWireLimit)}, children{0, 0};
  if (setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_CPU, &cpu) ||
      setrlimit(RLIMIT_FSIZE, &outputLimit) ||
      setrlimit(RLIMIT_NPROC, &children))
    return 2;
  // Ask Darwin for this sandbox's temporary directory instead of inheriting
  // the unrestricted drawing application's TMPDIR or falling back to /tmp.
  const size_t temporaryLength = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
  if (!temporaryLength || temporaryLength > 32768) return 2;
  QByteArray temporary(int(temporaryLength), char(0));
  if (confstr(_CS_DARWIN_USER_TEMP_DIR, temporary.data(), temporaryLength) !=
      temporaryLength)
    return 2;
  temporary.chop(1);
  qputenv("TMPDIR", temporary);
  if (!limitMacImportMemory()) return 2;
  QTemporaryDir inputDirectory, outputDirectory;
  QFile input, output;
  if (!inputDirectory.isValid() || !outputDirectory.isValid() ||
      !input.open(STDIN_FILENO, QIODevice::ReadOnly) ||
      !output.open(STDOUT_FILENO, QIODevice::WriteOnly))
    return 2;
  const QByteArray bytes = input.read(64LL * 1024 * 1024 + 1);
  if (bytes.size() > 64LL * 1024 * 1024 || !input.atEnd()) return 2;
  const QString path = inputDirectory.filePath(name);
  QFile staged(path);
  if (!staged.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      staged.write(bytes) != bytes.size() || !staged.flush())
    return 2;
  staged.close();
  const int status = worker(path, outputDirectory.path());
  const QStringList names =
      QDir(outputDirectory.path())
          .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
                     QDir::Name);
  if (names.isEmpty() || names.size() > 66000) return 2;
  QByteArray header("GZBW0001", 8);
  char number[4];
  qToBigEndian<quint32>(quint32(names.size()), number);
  header.append(number, 4);
  if (output.write(header) != header.size()) return 2;
  qint64 total = header.size();
  for (const QString &entry : names) {
    if (!macWireName(entry)) return 2;
    QString error;
    // Refuse special files even if a compromised decoder manufactured them.
    auto data = readBoundedRegularFile(outputDirectory.filePath(entry),
                                       macWireLimit - total, error);
    if (!error.isEmpty()) return 2;
    const QByteArray encoded = entry.toLatin1();
    total += 8 + encoded.size() + data.size();
    if (total > macWireLimit) return 2;
    char prefix[8];
    qToBigEndian<quint32>(quint32(encoded.size()), prefix);
    qToBigEndian<quint32>(quint32(data.size()), prefix + 4);
    if (output.write(prefix, 8) != 8 ||
        output.write(encoded) != encoded.size() ||
        output.write(data) != data.size())
      return 2;
  }
  return output.flush() ? status : 2;
}
}  // namespace RasterBrush
#endif
