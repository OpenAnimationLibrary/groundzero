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
#include <limits>

namespace RasterBrush {
namespace {
constexpr qint64 macWireLimit = 256LL * 1024 * 1024;
// Bound growth of the actual top-level VM map, not adjusted task-info totals.
// Existing reserved mappings can still be committed: this is not an RSS cap.
constexpr uint64_t macMappingHeadroom   = uint64_t(1024) * 1024 * 1024;
constexpr uint64_t macFootprintWatchdog = uint64_t(1024) * 1024 * 1024;
// The public nonrecursive BASIC_INFO_64 query returns whole top-level entries,
// including submap reservations. Sum each once, matching raw vm_map->size
// rather than task_info's adjusted metric, which excludes some Rosetta
// reservations. Stack-only enumeration runs before input is read; no region
// content is read.
bool macRawMappingBaseline(uint64_t &bytes, QString &error) {
  bytes                    = 0;
  mach_vm_address_t cursor = 0;
  QElapsedTimer deadline;
  deadline.start();
  for (unsigned int regions = 0; regions < 65536; ++regions) {
    if (deadline.elapsed() > 2000) {
      error = "Trusted VM map enumeration exceeded two seconds";
      return false;
    }
    mach_vm_address_t address = cursor;
    mach_vm_size_t size       = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object           = MACH_PORT_NULL;
    const kern_return_t status   = mach_vm_region(
        mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL)
      mach_port_deallocate(mach_task_self(), object);
    if (status == KERN_INVALID_ADDRESS && bytes != 0) return true;
    if (status != KERN_SUCCESS || count != VM_REGION_BASIC_INFO_COUNT_64 ||
        size == 0 || address < cursor ||
        size > std::numeric_limits<uint64_t>::max() - address ||
        size > uint64_t(RLIM_INFINITY) - macMappingHeadroom - 65536 - bytes) {
      error = QString("Cannot enumerate trusted VM map safely: Mach status %1")
                  .arg(status);
      return false;
    }
    bytes += size;
    cursor = address + size;
  }
  error = "Trusted VM map exceeds the 65536-region enumeration bound";
  return false;
}
bool limitMacImportMemory(QString &error) {
  auto failed = [&](const QString &reason) {
    error = reason;
    return false;
  };
  uint64_t baseline = 0;
  if (!macRawMappingBaseline(baseline, error)) return false;
  const rlim_t cap = rlim_t(baseline + macMappingHeadroom);
  rlimit addressSpace{cap, cap};
  if (setrlimit(RLIMIT_AS, &addressSpace))
    return failed(QString("Cannot set RLIMIT_AS: errno %1").arg(errno));
  rlimit raised{cap + 65536, cap + 65536};
  // A privileged host must not silently make this a reversible limit.
  if (setrlimit(RLIMIT_AS, &raised) == 0)
    return failed("RLIMIT_AS hard ceiling can be raised");
  if (errno != EPERM && errno != EACCES)
    return failed(QString("Unexpected hard-limit probe errno %1").arg(errno));
  std::fprintf(stderr, "Brush macOS virtual baseline=%llu cap=%llu bytes\n",
               static_cast<unsigned long long>(baseline),
               static_cast<unsigned long long>(cap));
  // Both POSIX and Mach allocation entry points must enforce the kernel map
  // limit. These reservations do not touch physical pages. Probe before bytes.
  errno = 0;
  void *excess =
      mmap(nullptr, size_t(cap), PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  const int mmapError = errno;
  if (excess != MAP_FAILED) {
    munmap(excess, size_t(cap));
    return failed("mmap exceeded RLIMIT_AS without rejection");
  }
  if (mmapError != ENOMEM)
    return failed(
        QString("Unexpected mmap ceiling probe errno %1").arg(mmapError));
  mach_vm_address_t address      = 0;
  const kern_return_t allocation = mach_vm_allocate(
      mach_task_self(), &address, mach_vm_size_t(cap), VM_FLAGS_ANYWHERE);
  if (allocation == KERN_SUCCESS) {
    mach_vm_deallocate(mach_task_self(), address, mach_vm_size_t(cap));
    return failed("Mach VM allocation exceeded RLIMIT_AS without rejection");
  }
  if (allocation != KERN_NO_SPACE && allocation != KERN_RESOURCE_SHORTAGE)
    return failed(
        QString("Unexpected Mach VM ceiling probe status %1").arg(allocation));
  // A normal small allocation must still work. A blanket allocator failure
  // does not qualify an operational importer boundary.
  void *ordinary = mmap(nullptr, 65536, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
  if (ordinary == MAP_FAILED)
    return failed(
        QString("Small mapping unavailable after memory limit: errno %1")
            .arg(errno));
  static_cast<volatile unsigned char *>(ordinary)[0] = 1;
  if (munmap(ordinary, 65536))
    return failed("Cannot release ordinary mapping probe");
  return true;
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

bool macImportBoundaryProbe(const QString &forbidden, QString &error) {
  auto failed = [&](const QString &reason) {
    error = reason;
    return false;
  };
  if (!macImportEntitlement())
    return failed("App Sandbox entitlement is absent");
  QFile file(forbidden);
  if (file.open(QIODevice::ReadOnly))
    return failed("App Sandbox allowed private sentinel read");
  if (file.open(QIODevice::WriteOnly))
    return failed("App Sandbox allowed private sentinel write");
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (errno == EPERM || errno == EACCES) return true;
    return failed(
        QString("Unexpected network probe socket errno %1").arg(errno));
  }
  sockaddr_in address{};
  address.sin_len         = sizeof(address);
  address.sin_family      = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port        = htons(9);
  const int result =
      connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  const int failure = errno;
  close(fd);
  if (result < 0 && (failure == EPERM || failure == EACCES)) return true;
  return failed(
      QString("Network capability probe was not denied: result %1 errno %2")
          .arg(result)
          .arg(failure));
}
int macImportStartupFailure(const QString &reason) {
  // Diagnostics use the same bounded private envelope, including failures
  // before any sandbox temporary directory can be created. No foreign bytes
  // have been read or decoded at these failure sites.
  const QByteArray message =
      ("macOS brush helper: " + reason).toUtf8().left(4096);
  QByteArray packet("GZBW0001", 8);
  char fields[12];
  qToBigEndian<quint32>(1, fields);
  qToBigEndian<quint32>(9, fields + 4);
  qToBigEndian<quint32>(quint32(message.size()), fields + 8);
  packet.append(fields, 12);
  packet.append("error.txt", 9);
  packet.append(message);
  QFile output;
  if (output.open(STDOUT_FILENO, QIODevice::WriteOnly)) {
    output.write(packet);
    output.flush();
  }
  return 2;
}
}  // namespace

int runMacImportStream(const QString &name, const QString &forbidden,
                       int (*worker)(const QString &, const QString &)) {
  // The independently signed helper enters App Sandbox at exec, before Qt or
  // any decoder runs. These checks additionally fail closed if packaging lost
  // its entitlement or accidentally added broad file/network privileges.
  QString startupError;
  if (!macImportBoundaryProbe(forbidden, startupError))
    return macImportStartupFailure(startupError);
  if (name != QFileInfo(name).fileName() || name.isEmpty() || name == "." ||
      name == "..")
    return macImportStartupFailure("Invalid input basename");
  rlimit core{0, 0}, cpu{60, 60},
      outputLimit{rlim_t(macWireLimit), rlim_t(macWireLimit)}, children{0, 0};
  if (setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_CPU, &cpu) ||
      setrlimit(RLIMIT_FSIZE, &outputLimit) ||
      setrlimit(RLIMIT_NPROC, &children))
    return macImportStartupFailure(
        QString("Cannot establish worker resource limits: errno %1")
            .arg(errno));
  // Ask Darwin for this sandbox's temporary directory instead of inheriting
  // the unrestricted drawing application's TMPDIR or falling back to /tmp.
  const size_t temporaryLength = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
  if (!temporaryLength || temporaryLength > 32768)
    return macImportStartupFailure("Cannot locate sandbox temporary directory");
  QByteArray temporary(int(temporaryLength), char(0));
  if (confstr(_CS_DARWIN_USER_TEMP_DIR, temporary.data(), temporaryLength) !=
      temporaryLength)
    return macImportStartupFailure("Cannot read sandbox temporary directory");
  temporary.chop(1);
  qputenv("TMPDIR", temporary);
  if (!limitMacImportMemory(startupError))
    return macImportStartupFailure(startupError);
  QTemporaryDir inputDirectory, outputDirectory;
  QFile input, output;
  if (!inputDirectory.isValid() || !outputDirectory.isValid() ||
      !input.open(STDIN_FILENO, QIODevice::ReadOnly) ||
      !output.open(STDOUT_FILENO, QIODevice::WriteOnly))
    return macImportStartupFailure(
        "Cannot open private staging or inherited transport descriptors");
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
