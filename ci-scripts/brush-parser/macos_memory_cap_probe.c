/* CI-only macOS memory-limit capability experiment. No production launch path.
 * Private SPI declarations/constants are pinned to Apple's xnu-11215.81.4:
 * libsyscall/wrappers/spawn/spawn_private.h, bsd/sys/spawn_internal.h,
 * bsd/sys/proc_info_private.h, bsd/sys/proc_info.h, bsd/sys/reason.h,
 * bsd/sys/kern_memorystatus.h. See the accompanying research document.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecTask.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_purgable.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MIB (UINT64_C(1024) * 1024)
#define CHUNK_BYTES (4 * MIB)
#define ACTIVE_FATAL 0x04
#define INACTIVE_FATAL 0x08
#define EXIT_REASON_BASIC_INFO 25
#define REASON_JETSAM 1
#define REASON_PER_PROCESS_LIMIT 7

typedef int (*set_jetsam_fn)(posix_spawnattr_t *, short, int, int, int);
typedef kern_return_t (*set_footprint_fn)(task_t, int, int *);
typedef int (*memorystatus_fn)(uint32_t, int32_t, uint32_t, void *, size_t);
struct probe_exit_reason {
  uint32_t name_space;
  uint64_t code;
  uint64_t flags;
  uint32_t buffer_size;
} __attribute__((packed));
_Static_assert(sizeof(struct probe_exit_reason) == 24, "Pinned exit reason ABI");

static uint64_t parse_mib(const char *text, bool allow_zero) {
  char *end = NULL;
  errno = 0;
  unsigned long result = strtoul(text, &end, 10);
  if (errno || !*text || *end || result > 4096 || (!allow_zero && !result))
    return UINT64_MAX;
  return (uint64_t)result;
}

static bool entitled(const char *name) {
  SecTaskRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
  if (!task) return false;
  CFStringRef key = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
  CFTypeRef value = SecTaskCopyValueForEntitlement(task, key, NULL);
  bool result = value && CFEqual(value, kCFBooleanTrue);
  if (value) CFRelease(value);
  CFRelease(key);
  CFRelease(task);
  return result;
}

static int child_boundary(const char *sentinel) {
  if (getuid() == 0 || geteuid() == 0 ||
      !entitled("com.apple.security.app-sandbox") ||
      entitled("com.apple.security.inherit") ||
      entitled("com.apple.security.network.client") ||
      entitled("com.apple.security.network.server")) {
    fprintf(stderr, "Unprivileged independent App Sandbox identity required\n");
    return 70;
  }
  const int access_modes[] = {O_RDONLY, O_WRONLY};
  for (size_t i = 0; i < sizeof(access_modes) / sizeof(access_modes[0]); ++i) {
    errno = 0;
    int fd = open(sentinel, access_modes[i]);
    int failure = errno;
    if (fd >= 0) close(fd);
    if (fd >= 0 || (failure != EACCES && failure != EPERM)) {
      fprintf(stderr, "Private sentinel access not denied: %d\n", failure);
      return 71;
    }
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (errno != EPERM && errno != EACCES) return 72;
  } else {
    struct sockaddr_in address = {0};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(9);
    int status = connect(fd, (struct sockaddr *)&address, sizeof(address));
    int failure = errno;
    close(fd);
    if (status >= 0 || (failure != EPERM && failure != EACCES)) return 72;
  }
  puts("CHILD_BOUNDARY {\"unprivileged\":true,\"app_sandbox\":true,\"sentinel_denied\":true,\"network_denied\":true}");
  return 0;
}

static int report_memory(const char *mode, uint64_t touched) {
  task_vm_info_data_t info = {0};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  kern_return_t result = task_info(mach_task_self(), TASK_VM_INFO,
      (task_info_t)&info, &count);
  if (result != KERN_SUCCESS || count < TASK_VM_INFO_REV1_COUNT) return 73;
  printf("CHILD_MEMORY {\"mode\":\"%s\",\"touched_bytes\":%" PRIu64
         ",\"resident_bytes\":%" PRIu64 ",\"phys_footprint_bytes\":%" PRIu64 "}\n",
         mode, touched, (uint64_t)info.resident_size,
         (uint64_t)info.phys_footprint);
  return 0;
}

static int attempt_relaxation(void) {
  set_footprint_fn set_footprint =
      (set_footprint_fn)dlsym(RTLD_DEFAULT, "task_set_phys_footprint_limit");
  memorystatus_fn control =
      (memorystatus_fn)dlsym(RTLD_DEFAULT, "memorystatus_control");
  if (!set_footprint || !control) return 74;
  int previous = 0;
  kern_return_t mach_status = set_footprint(mach_task_self(), 4096, &previous);
  /* MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES=7, fatal attributes=1. */
  struct { int32_t active; uint32_t active_attr;
           int32_t inactive; uint32_t inactive_attr; } properties = {4096, 1, 4096, 1};
  errno = 0;
  int status = control(7, getpid(), 0, &properties, sizeof(properties));
  int failure = errno;
  printf("CHILD_RELAXATION {\"mach_status\":%d,\"memorystatus_status\":%d,\"memorystatus_errno\":%d}\n",
         mach_status, status, failure);
  return mach_status == KERN_NO_ACCESS && status == -1 &&
      (failure == EPERM || failure == EACCES) ? 0 : 75;
}

static volatile unsigned char read_sink;
static int child_main(const char *mode, const char *sentinel, uint64_t target_mib) {
  setvbuf(stdout, NULL, _IONBF, 0);
  struct rlimit no_core = {0, 0};
  if (setrlimit(RLIMIT_CORE, &no_core)) return 76;
  int result = child_boundary(sentinel);
  if (result) return result;
  if (!strcmp(mode, "ordinary")) {
    result = attempt_relaxation();
    if (result) return result;
  }
  uint64_t target = target_mib * MIB;
  result = report_memory("startup", 0);
  if (result) return result;
  if (!strcmp(mode, "private-file") || !strcmp(mode, "shared-file")) {
    struct stat info;
    if (fstat(3, &info) || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || (uint64_t)info.st_size != target) return 77;
    int flags = !strcmp(mode, "private-file") ? MAP_PRIVATE : MAP_SHARED;
    void *mapping = mmap(NULL, (size_t)target, PROT_READ, flags, 3, 0);
    if (mapping == MAP_FAILED) return 78;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return 79;
    volatile const unsigned char *bytes = mapping;
    unsigned char checksum = 0;
    for (uint64_t offset = 0; offset < target; offset += (uint64_t)page)
      checksum ^= bytes[offset];
    read_sink = checksum;
    result = report_memory(mode, target);
    if (munmap(mapping, (size_t)target)) return 80;
    return result;
  }
  bool reserved = !strcmp(mode, "pre-reserved");
  bool volatile_memory = !strcmp(mode, "purgeable-volatile");
  bool mach = !strcmp(mode, "mach") || volatile_memory;
  if (!reserved && !mach && strcmp(mode, "malloc") && strcmp(mode, "ordinary"))
    return 81;
  void *reservation = NULL;
  if (reserved) {
    reservation = mmap(NULL, (size_t)target, PROT_NONE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (reservation == MAP_FAILED ||
        mprotect(reservation, (size_t)target, PROT_READ | PROT_WRITE)) return 82;
    puts("CHILD_RESERVED {\"private_reservation_before_touch\":true}");
  }
  /* Retain every allocation. Incompressible page contents exercise dirty
   * commitment, rather than allocator reservations or compressed zero pages. */
  static void * volatile retained[1024];
  size_t allocation = 0;
  for (uint64_t offset = 0; offset < target;) {
    size_t amount = (size_t)((target - offset < CHUNK_BYTES)
        ? target - offset : CHUNK_BYTES);
    void *memory;
    if (reserved) memory = (unsigned char *)reservation + offset;
    else if (mach) {
      mach_vm_address_t address = 0;
      if (mach_vm_allocate(mach_task_self(), &address, amount,
                           VM_FLAGS_ANYWHERE | (volatile_memory ? VM_FLAGS_PURGABLE : 0)) != KERN_SUCCESS) return 83;
      memory = (void *)(uintptr_t)address;
    } else {
      memory = malloc(amount);
      if (!memory || allocation >= sizeof(retained) / sizeof(retained[0])) return 84;
      retained[allocation++] = memory;
    }
    arc4random_buf(memory, amount);
    if (volatile_memory) {
      int state = VM_PURGABLE_VOLATILE;
      if (mach_vm_purgable_control(mach_task_self(),
          (mach_vm_address_t)(uintptr_t)memory, VM_PURGABLE_SET_STATE,
          &state) != KERN_SUCCESS) return 92;
    }
    offset += amount;
  }
  /* A capped allocation case reaching here disproves fatal enforcement. */
  return report_memory(mode, target);
}

static double monotonic_seconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
  return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static int run_child(const char *helper, const char *mode, const char *sentinel,
                     const char *file, uint64_t limit, uint64_t target) {
  if (getuid() == 0 || geteuid() == 0 || entitled("com.apple.security.app-sandbox"))
    return 85;
  set_jetsam_fn set_jetsam =
      (set_jetsam_fn)dlsym(RTLD_DEFAULT, "posix_spawnattr_setjetsam_ext");
  if (limit && !set_jetsam) {
    fprintf(stderr, "macOS private spawn-memory SPI unavailable\n");
    return 86;
  }
  posix_spawnattr_t attributes;
  posix_spawn_file_actions_t actions;
  int error = posix_spawnattr_init(&attributes);
  if (error) return 87;
  error = posix_spawn_file_actions_init(&actions);
  if (error) { posix_spawnattr_destroy(&attributes); return 88; }
  if (limit) error = set_jetsam(&attributes,
      ACTIVE_FATAL | INACTIVE_FATAL, 100, (int)limit, (int)limit);
  int descriptor = -1;
  if (!strcmp(mode, "private-file") || !strcmp(mode, "shared-file")) {
    descriptor = open(file, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) error = errno;
    else if (!error) error = posix_spawn_file_actions_adddup2(&actions, descriptor, 3);
    if (descriptor > 3 && !error)
      error = posix_spawn_file_actions_addclose(&actions, descriptor);
  }
  char target_text[24];
  snprintf(target_text, sizeof(target_text), "%" PRIu64, target);
  char *arguments[] = {(char *)helper, "--child", (char *)mode,
                       (char *)sentinel, target_text, NULL};
  char *environment[] = {"MallocNanoZone=0", NULL};
  pid_t child = -1;
  if (!error) error = posix_spawn(&child, helper, &actions, &attributes,
                                 arguments, environment);
  if (descriptor >= 0) close(descriptor);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  if (error) {
    fprintf(stderr, "Probe spawn failed: %d\n", error);
    return 89;
  }
  /* There is deliberately no footprint/RSS watchdog. Only a wall deadline
   * prevents an unavailable mechanism from hanging CI. Deadline kills can
   * never qualify: the exact unreaped kernel exit identity is required. */
  double deadline = monotonic_seconds() + 20;
  bool timeout = false;
  int wait_error = 0;
  for (;;) {
    siginfo_t event = {0};
    if (waitid(P_PID, child, &event, WEXITED | WNOHANG | WNOWAIT)) {
      if (errno == EINTR) continue;
      wait_error = errno;
      break;
    }
    if (event.si_pid == child) break;
    double now = monotonic_seconds();
    if (now < 0 || now >= deadline) { timeout = true; break; }
    usleep(10000);
  }
  if (timeout || wait_error) {
    kill(child, SIGKILL);
    siginfo_t event = {0};
    while (waitid(P_PID, child, &event, WEXITED | WNOWAIT) && errno == EINTR) {}
  }
  struct probe_exit_reason reason = {0};
  errno = 0;
  int reason_size = proc_pidinfo(child, EXIT_REASON_BASIC_INFO, 0,
                                &reason, sizeof(reason));
  int reason_error = errno;
  int status = 0;
  struct rusage usage = {0};
  pid_t reaped;
  do { reaped = wait4(child, &status, 0, &usage); } while (reaped < 0 && errno == EINTR);
  if (reaped != child) return 90;
  bool killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL &&
      reason_size == (int)sizeof(reason) && reason.name_space == REASON_JETSAM &&
      reason.code == REASON_PER_PROCESS_LIMIT && !timeout && !wait_error;
  printf("PROBE_RESULT {\"mode\":\"%s\",\"limit_mib\":%" PRIu64
         ",\"target_mib\":%" PRIu64 ",\"timed_out\":%s,\"wait_errno\":%d,"
         "\"wait_status\":%d,\"peak_resident_bytes\":%" PRIu64 ",\"reason_size\":%d,\"reason_errno\":%d,"
         "\"reason_namespace\":%u,\"reason_code\":%" PRIu64
         ",\"kernel_per_process_limit_kill\":%s,\"spawn_memory_spi_available\":%s,\"parent_memory_watchdog\":false}\n",
         mode, limit, target, timeout ? "true" : "false", wait_error, status,
         (uint64_t)(usage.ru_maxrss > 0 ? usage.ru_maxrss : 0),
         reason_size, reason_error, reason.name_space, reason.code,
         killed ? "true" : "false", set_jetsam ? "true" : "false");
  return timeout || wait_error ? 91 : 0;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc == 5 && !strcmp(argv[1], "--child")) {
    uint64_t target = parse_mib(argv[4], false);
    if (target == UINT64_MAX) return 64;
    return child_main(argv[2], argv[3], target);
  }
  if (argc == 8 && !strcmp(argv[1], "--run")) {
    uint64_t limit = parse_mib(argv[6], true);
    uint64_t target = parse_mib(argv[7], false);
    if (limit == UINT64_MAX || target == UINT64_MAX) return 64;
    return run_child(argv[2], argv[3], argv[4], argv[5], limit, target);
  }
  fputs("CI-only memory probe: use the accompanying Python driver\n", stderr);
  return 64;
}
