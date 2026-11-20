#include "automation.h"
#include "automation_containment.h"

#include <QCoreApplication>
#include <QFile>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>

namespace {
std::atomic<bool> failGroupKill{false};

[[noreturn]] void descendant(int descriptor) {
  if (fcntl(3, F_GETFD) != -1 || errno != EBADF) _exit(11);
  struct sigaction action{};
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGTERM, &action, nullptr) != 0) _exit(12);
  const char ready = 1;
  if (write(descriptor, &ready, 1) != 1) _exit(13);
  close(descriptor);
  for (;;) pause();
}

pid_t startDescendant(const QByteArray& executable) {
  int readiness[2];
  if (pipe(readiness) != 0)
    throw std::runtime_error("Cannot create helper readiness pipe");
  const std::string descriptor = std::to_string(readiness[1]);
  const pid_t child            = fork();
  if (child < 0)
    throw std::runtime_error("Cannot fork the trusted helper fixture");
  if (child == 0) {
    close(readiness[0]);
    execl(executable.constData(), executable.constData(), "--descendant",
          descriptor.c_str(), nullptr);
    _exit(14);
  }
  close(readiness[1]);
  pollfd channel{readiness[0], POLLIN, 0};
  char value = 0;
  const bool ready =
      poll(&channel, 1, 5000) > 0 && read(readiness[0], &value, 1) == 1;
  close(readiness[0]);
  if (!ready || value != 1)
    throw std::runtime_error("Trusted helper did not become ready");
  return child;
}
pid_t requestedHelper(bool contained, const std::string& mode) {
  if (mode != "--block" && mode != "--kill-failure") return -1;
  if (!contained)
    throw std::runtime_error("Fixture helpers require verified containment");
  return startDescendant(
      QFile::encodeName(QCoreApplication::applicationFilePath()));
}

}  // namespace

namespace GroundzeroAutomation {
int automationContainmentKill(pid_t group, int signal) {
  if (failGroupKill.load()) {
    errno = EPERM;
    return -1;
  }
  return kill(group, signal);
}
}  // namespace GroundzeroAutomation

int main(int argc, char* argv[]) {
  if (argc == 3 && std::string(argv[1]) == "--descendant")
    descendant(std::atoi(argv[2]));
  QCoreApplication application(argc, argv);
  const std::string mode = argc > 1 ? argv[1] : "";
  failGroupKill          = mode == "--kill-failure";
  try {
    const bool contained =
        GroundzeroAutomation::establishAutomationContainment();
    const pid_t helper = requestedHelper(contained, mode);
    std::cout << "{\"contained\":" << (contained ? "true" : "false")
              << ",\"pid\":" << getpid() << ",\"group\":" << getpgrp()
              << ",\"session\":" << getsid(0) << ",\"helper\":" << helper
              << "}\n";
    std::cout.flush();
    if (helper > 0)
      for (;;) pause();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "containment-fixture: " << error.what() << '\n';
    return 2;
  }
}
