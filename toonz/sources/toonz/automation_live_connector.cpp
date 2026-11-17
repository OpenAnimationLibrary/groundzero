#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QLocalSocket>
#include <QMap>
#include <QtEndian>
#include <algorithm>
#include <cstdio>

#include "automation.h"
#include "automation_live_protocol.h"
#include "automation_live_security.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#endif

namespace GroundzeroAutomation {
namespace Live {
namespace {

constexpr int connectionTimeout = 3000;
constexpr int responseTimeout = 5000;

[[noreturn]] void failed(const QString& code) {
  throw Error(code, "The native live connector cannot continue.");
}

bool takeFrame(QByteArray& input, int maximum, QByteArray& body) {
  if (input.size() < 4) return false;
  const quint32 size = qFromBigEndian<quint32>(
      reinterpret_cast<const uchar*>(input.constData()));
  if (size == 0 || size > quint32(maximum)) failed("invalid_frame");
  if (input.size() < int(size) + 4) return false;
  body = input.mid(4, int(size));
  input.remove(0, int(size) + 4);
  return true;
}

#ifdef Q_OS_WIN
bool writeHandle(HANDLE handle, const QByteArray& bytes) {
  int position = 0;
  while (position < bytes.size()) {
    DWORD written = 0;
    if (!WriteFile(handle, bytes.constData() + position,
                   DWORD(bytes.size() - position), &written, nullptr) ||
        written == 0)
      return false;
    position += int(written);
  }
  return true;
}

void writeOutput(const QByteArray& bytes) {
  if (!writeHandle(GetStdHandle(STD_OUTPUT_HANDLE), bytes))
    failed("connector_io");
}

void reportFailure(const QByteArray& code) {
  writeHandle(GetStdHandle(STD_ERROR_HANDLE),
              "Groundzero live connector failed: " + code + ".\n");
}
#else
void writeOutput(const QByteArray& bytes) {
  if (std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) !=
          size_t(bytes.size()) ||
      std::fflush(stdout) != 0)
    failed("connector_io");
}

void reportFailure(const QByteArray& code) {
  std::fprintf(stderr, "Groundzero live connector failed: %s.\n",
               code.constData());
}
#endif

void send(QLocalSocket& socket, const QByteArray& frame) {
  if (socket.state() != QLocalSocket::ConnectedState ||
      socket.bytesToWrite() + frame.size() > maximumRequestBytes + 4 ||
      socket.write(frame) != frame.size())
    failed("live_disconnected");
  QElapsedTimer timer;
  timer.start();
  while (socket.bytesToWrite() > 0) {
    const int remaining = 1000 - int(timer.elapsed());
    if (remaining <= 0 || !socket.waitForBytesWritten(remaining))
      failed("connector_io");
  }
}

QByteArray receiveHello(QLocalSocket& socket) {
  QElapsedTimer timer;
  timer.start();
  QByteArray input;
  QByteArray body;
  while (timer.elapsed() < connectionTimeout) {
    input.append(socket.read(maximumResponseBytes + 4 - input.size()));
    if (takeFrame(input, maximumResponseBytes, body)) {
      if (!input.isEmpty()) failed("invalid_frame");
      return body;
    }
    if (socket.state() != QLocalSocket::ConnectedState)
      failed("authentication_failed");
    socket.waitForReadyRead(connectionTimeout - int(timer.elapsed()));
  }
  failed("authentication_failed");
}

void validateHello(const QJsonObject& hello, const SecuritySession& session,
                   const QString& client) {
  const QStringList keys{"client_id", "instance_id", "limits", "mode", "tools"};
  if (hello.keys() != keys || hello.value("client_id") != client ||
      hello.value("instance_id") != session.instanceId() ||
      hello.value("mode") != session.mode())
    failed("authentication_failed");
  if (hello.value("tools") != QJsonValue(operations()) ||
      hello.value("limits") != QJsonValue(limits()))
    failed("invalid_capabilities");
}

void authenticate(QLocalSocket& socket, const SecuritySession& session) {
  socket.setReadBufferSize(maximumResponseBytes + 4);
  socket.connectToServer(session.endpoint());
  if (!socket.waitForConnected(connectionTimeout)) failed("live_unavailable");
  session.verifyServer(socket.socketDescriptor());
  const QString client = secureUuid();
  const QJsonObject auth{{"protocol", "groundzero-live-v1"},
                         {"instance_id", session.instanceId()},
                         {"client_id", client},
                         {"token", QString::fromLatin1(session.token())}};
  send(socket, frameMessage(auth, maximumRequestBytes));
  const QJsonObject hello =
      parseMessage(receiveHello(socket), maximumResponseBytes);
  validateHello(hello, session, client);
  writeOutput(frameMessage(hello, maximumResponseBytes));
}

#ifdef Q_OS_WIN

void prepareInput() {
  if (GetFileType(GetStdHandle(STD_INPUT_HANDLE)) != FILE_TYPE_PIPE ||
      GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_PIPE)
    failed("connector_io");
}

bool readInput(QByteArray& input, int capacity) {
  const HANDLE pipe = GetStdHandle(STD_INPUT_HANDLE);
  DWORD available = 0;
  if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) return false;
    failed("connector_io");
  }
  if (!available || capacity <= 0) return true;
  QByteArray bytes(std::min(capacity, int(std::min<DWORD>(available, 65540))),
                   '\0');
  DWORD count = 0;
  if (!ReadFile(pipe, bytes.data(), DWORD(bytes.size()), &count, nullptr))
    failed("connector_io");
  input.append(bytes.constData(), int(count));
  return count != 0;
}

#else

void prepareInput() {
  // A closed adapter pipe is reported through the connector error path.
  std::signal(SIGPIPE, SIG_IGN);
}

bool inputReady() {
  pollfd pipe{STDIN_FILENO, POLLIN | POLLHUP, 0};
  const int ready = ::poll(&pipe, 1, 0);
  if (ready < 0 && errno == EINTR) return false;
  if (ready < 0 || (pipe.revents & (POLLERR | POLLNVAL)))
    failed("connector_io");
  return ready > 0;
}

bool readInput(QByteArray& input, int capacity) {
  if (capacity <= 0 || !inputReady()) return true;
  QByteArray bytes(capacity, '\0');
  const ssize_t count = ::read(STDIN_FILENO, bytes.data(), size_t(capacity));
  if (count < 0 && errno == EINTR) return true;
  if (count < 0) failed("connector_io");
  input.append(bytes.constData(), int(count));
  return count != 0;
}

#endif

class Forwarder final {
 public:
  explicit Forwarder(QLocalSocket& socket) : m_socket(socket) {
    m_clock.start();
  }
  void run() {
    while (m_socket.state() == QLocalSocket::ConnectedState) {
      receive();
      if (!submit()) return;
      requireTimelyResponses();
      m_socket.waitForReadyRead(10);
    }
    receive();
    if (!m_pending.isEmpty()) failed("live_disconnected");
  }

 private:
  bool submit() {
    const int capacity = maximumRequestBytes + 4 - m_input.size();
    if (m_pending.size() < 4 && !readInput(m_input, capacity)) {
      if (!m_input.isEmpty()) failed("invalid_frame");
      return false;
    }
    QByteArray body;
    while (m_pending.size() < 4 &&
           takeFrame(m_input, maximumRequestBytes, body)) {
      const QJsonObject request = parseMessage(body, maximumRequestBytes);
      validateRequest(request);
      const QString id = request.value("id").toString();
      if (m_pending.contains(id)) failed("invalid_request_id");
      m_pending.insert(id, m_clock.elapsed());
      send(m_socket, frameMessage(request, maximumRequestBytes));
    }
    return true;
  }
  void receive() {
    m_output.append(m_socket.read(maximumResponseBytes + 4 - m_output.size()));
    QByteArray body;
    while (takeFrame(m_output, maximumResponseBytes, body)) {
      const QJsonObject response = parseMessage(body, maximumResponseBytes);
      validateResponse(response);
      m_pending.remove(response.value("id").toString());
      writeOutput(frameMessage(response, maximumResponseBytes));
    }
  }
  void validateResponse(const QJsonObject& response) const {
    const QStringList success{"id", "result"};
    const QStringList error{"error", "id"};
    if (response.keys() != success && response.keys() != error)
      failed("invalid_frame");
    if (!response.value("id").isString() ||
        !m_pending.contains(response.value("id").toString()))
      failed("invalid_request_id");
  }
  void requireTimelyResponses() const {
    for (qint64 submitted : m_pending)
      if (m_clock.elapsed() - submitted > responseTimeout)
        failed("live_timeout");
  }
  QLocalSocket& m_socket;
  QByteArray m_input;
  QByteArray m_output;
  QMap<QString, qint64> m_pending;
  QElapsedTimer m_clock;
};

}  // namespace

int runConnector(int argc, char* argv[]) {
  try {
    if (argc != 3) failed("invalid_arguments");
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() != 3 || arguments[1] != "--automation-live-connect")
      failed("invalid_arguments");
    prepareInput();
    const auto session = SecuritySession::read(arguments[2]);
    QLocalSocket socket;
    authenticate(socket, *session);
    Forwarder(socket).run();
    socket.abort();
    return 0;
  } catch (const Error& error) {
    reportFailure(error.code.toUtf8());
  } catch (...) {
    reportFailure("internal_error");
  }
  return 1;
}

}  // namespace Live
}  // namespace GroundzeroAutomation
