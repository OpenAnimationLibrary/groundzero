#include "automation_live_security.h"

#include <sys/stat.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <cstdio>
#include <stdexcept>

#include "automation_build_identity.h"
#include "automation_live_protocol.h"
#include "automation_live_transport.h"

using namespace GroundzeroAutomation;
using namespace GroundzeroAutomation::Live;
namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void deniedRead(const QString& path) {
  try {
    SecuritySession::read(path);
  } catch (const Error& error) {
    require(error.code == "live_security", "Wrong admission error code");
    return;
  }
  throw std::runtime_error("Insecure grant admitted");
}
QByteArray readFrame(QIODevice& stream) {
  QByteArray bytes;
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 3000) {
    bytes += stream.readAll();
    if (bytes.size() >= 4) {
      const quint32 size = qFromBigEndian<quint32>(
          reinterpret_cast<const uchar*>(bytes.constData()));
      require(size > 0 && size <= maximumResponseBytes, "Frame exceeded limit");
      if (bytes.size() >= int(size) + 4) {
        require(bytes.size() == int(size) + 4, "Unexpected extra frame");
        return bytes.mid(4);
      }
    }
    stream.waitForReadyRead(50);
  }
  throw std::runtime_error("Framed response timed out");
}
void sendFrame(QIODevice& stream, const QJsonObject& object) {
  const QByteArray bytes = frameMessage(object, maximumResponseBytes);
  require(stream.write(bytes) == bytes.size(), "Short frame write");
  while (stream.bytesToWrite())
    require(stream.waitForBytesWritten(1000), "Write timed out");
}
QByteArray rawFrame(const QJsonObject& object) {
  return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
void requireNoSecrets(const QByteArray& bytes, const SecuritySession& session) {
  require(!bytes.contains(session.token()) &&
              !bytes.contains(session.endpoint().toUtf8()),
          "A connector refusal leaked the token or endpoint");
}
void versionedHandshake(SecuritySession& publisher, const QString& path,
                        bool serverCloses) {
  QProcess connector;
  connector.start(QCoreApplication::applicationFilePath(),
                  {"--automation-live-connect", path});
  require(connector.waitForStarted(3000), "Connector failed to start");
  qintptr descriptor = -1;
  QElapsedTimer timer;
  timer.start();
  while (descriptor < 0 && timer.elapsed() < 3000) {
    descriptor = publisher.acceptDescriptor();
    if (descriptor < 0) connector.waitForReadyRead(10);
  }
  require(descriptor >= 0, "Connector never reached native listener");
  QLocalSocket peer;
  require(peer.setSocketDescriptor(descriptor),
          "Cannot adopt accepted descriptor");
  const auto auth = parseMessage(readFrame(peer), maximumRequestBytes);
  publisher.verifyClient(peer.socketDescriptor());
  require(auth.value("protocol") == QJsonValue(liveProtocol) &&
              auth.value("build") == QJsonValue(buildIdentity()),
          "Connector authentication omitted its protocol or build");
  require(constantTimeTokenEquals(auth.value("token").toString().toLatin1(),
                                  publisher.token()),
          "Connector sent incorrect token");
  const QJsonObject hello{{"protocol", liveProtocol},
                          {"build", buildIdentity()},
                          {"instance_id", publisher.instanceId()},
                          {"client_id", auth.value("client_id")},
                          {"mode", publisher.mode()},
                          {"tools", operations()},
                          {"limits", limits()}};
  sendFrame(peer, hello);
  require(readFrame(connector) == rawFrame(hello),
          "Connector did not forward the hello byte for byte");
  const QJsonObject request{
      {"id", "one"}, {"method", "live.status"}, {"params", QJsonObject{}}};
  sendFrame(connector, request);
  require(parseMessage(readFrame(peer), maximumRequestBytes) == request,
          "Connector changed forwarded request");
  const QJsonObject response{{"id", "one"},
                             {"result", QJsonObject{{"boundary", "verified"}}}};
  sendFrame(peer, response);
  if (serverCloses) peer.disconnectFromServer();
  require(parseMessage(readFrame(connector), maximumResponseBytes) == response,
          "Connector changed forwarded response");
  if (!serverCloses) connector.closeWriteChannel();
  require(connector.waitForFinished(3000), "Connector ignored EOF");
  require(connector.exitCode() == 0, "Connector failed natural shutdown");
  require(connector.readAllStandardError().isEmpty(),
          "Connector emitted diagnostics");
}
// Runs a connector expected to refuse before the hello; the listener must
// stay unconnected for three seconds when one is given.
QJsonObject refusal(const QString& path, const QString& code,
                    SecuritySession& publisher, bool watchListener) {
  QProcess connector;
  connector.start(QCoreApplication::applicationFilePath(),
                  {"--automation-live-connect", path});
  connector.closeWriteChannel();
  require(connector.waitForFinished(5000), "Refused connector hung");
  QElapsedTimer timer;
  timer.start();
  while (watchListener && timer.elapsed() < 3000) {
    const qintptr descriptor = publisher.acceptDescriptor();
    if (descriptor >= 0) closeAcceptedDescriptor(descriptor);
    require(descriptor < 0, "A refused connector reached the listener");
    QThread::msleep(20);
  }
  const QByteArray output = connector.readAllStandardOutput();
  require(connector.exitCode() == 2, "A refused connector did not exit 2");
  require(connector.readAllStandardError().isEmpty(),
          "A refused connector wrote diagnostics");
  requireNoSecrets(output, publisher);
  require(output.size() > 4 &&
              qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(
                  output.constData())) == quint32(output.size() - 4),
          "A refusal is not exactly one frame");
  const QJsonObject frame   = parseMessage(output.mid(4), maximumResponseBytes);
  const QJsonObject body    = frame.value("refusal").toObject();
  const QJsonObject details = body.value("details").toObject();
  require(frame.keys() == QStringList{"refusal"} &&
              body.keys() == QStringList({"code", "details", "message"}) &&
              body.value("code") == code &&
              !body.value("message").toString().isEmpty() &&
              details.keys() == QStringList({"connector", "editor"}) &&
              details.value("connector") == QJsonValue(liveIdentity()),
          "A connector refusal has the wrong code or schema");
  return details;
}
QJsonObject publishedGrant(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read the published grant");
  return parseMessage(file.readAll(), 4096);
}
QString privateGrant(const QString& directory, const QString& name,
                     const QJsonObject& grant) {
  const QString path = directory + "/" + name;
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly),
          "Cannot create a grant variant");
  require(file.write(rawFrame(grant)) > 0, "Cannot write a grant variant");
  file.close();
  require(chmod(QFile::encodeName(path).constData(), 0600) == 0,
          "Cannot restrict a grant variant");
  return path;
}
void buildMismatchBeforeConnect(SecuritySession& publisher, const QString& path,
                                const QString& directory) {
  QJsonObject grant = publishedGrant(path);
  QJsonObject build = grant.value("build").toObject();
  build.insert("executable_sha256", QString(64, 'a'));
  grant.insert("build", build);
  const QString variant = privateGrant(directory, "foreign-build.json", grant);
  const QJsonObject details =
      refusal(variant, "build_mismatch", publisher, true);
  require(details.value("editor") ==
              QJsonObject({{"protocol", liveProtocol}, {"build", build}}),
          "A build refusal did not name the editor's identity");
  require(QFile::remove(variant), "Cannot remove a grant variant");
}
void protocolMismatchBeforeConnect(SecuritySession& publisher,
                                   const QString& path,
                                   const QString& directory) {
  QJsonObject grant = publishedGrant(path);
  grant.remove("build");
  grant.insert("protocol", "groundzero-live-v1");
  const QString variant = privateGrant(directory, "previous.json", grant);
  const QJsonObject details =
      refusal(variant, "protocol_mismatch", publisher, true);
  require(details.value("editor") ==
              QJsonObject({{"protocol", "groundzero-live-v1"},
                           {"build", QJsonValue()}}),
          "A protocol refusal did not name the editor's label");
  require(QFile::remove(variant), "Cannot remove a grant variant");
}
void fileAdmission(SecuritySession& publisher, const QString& path,
                   const QString& directory) {
  const auto encoded = QFile::encodeName(path);
  require(chmod(encoded.constData(), 0644) == 0, "chmod failed");
  deniedRead(path);
  refusal(path, "live_security", publisher, false);
  require(chmod(encoded.constData(), 0600) == 0, "chmod restoration failed");
  const QString hardlink = directory + "/hard.json";
  require(
      link(encoded.constData(), QFile::encodeName(hardlink).constData()) == 0,
      "Cannot create native hardlink fixture");
  deniedRead(path);
  require(QFile::remove(hardlink), "Cannot remove native hardlink fixture");
  const QString symlink = directory + "/symbolic.json";
  require(::symlink(encoded.constData(),
                    QFile::encodeName(symlink).constData()) == 0,
          "Cannot create native symlink fixture");
  deniedRead(symlink);
  require(QFile::remove(symlink), "Cannot remove symlink fixture");
  require(chmod(QFile::encodeName(directory).constData(), 0755) == 0,
          "parent chmod failed");
  deniedRead(path);
  require(chmod(QFile::encodeName(directory).constData(), 0700) == 0,
          "parent chmod restoration failed");
}
// A real transport on its own thread, as the editor runs it.
class TransportHost final {
public:
  explicit TransportHost(const QString& path)
      : security(std::make_shared<SecuritySession>(path, "edit")) {
    auto state = std::make_shared<SharedState>(security->instanceId(), "edit");
    transport  = new Transport(state, security, []() {}, []() {});
    transport->moveToThread(&thread);
    thread.start();
    bool started = false;
    QMetaObject::invokeMethod(
        transport,
        [this, &started]() {
          try {
            transport->start();
            started = true;
          } catch (...) {
          }
        },
        Qt::BlockingQueuedConnection);
    require(started, "The native transport did not start");
  }
  ~TransportHost() {
    QMetaObject::invokeMethod(
        transport, [this]() { transport->stop(); },
        Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
    delete transport;
  }
  std::shared_ptr<SecuritySession> security;
  QThread thread;
  Transport* transport = nullptr;
};
QJsonObject authentication(const SecuritySession& session) {
  return {{"protocol", liveProtocol},
          {"build", buildIdentity()},
          {"instance_id", session.instanceId()},
          {"client_id", "00000000-0000-4000-8000-00000000000a"},
          {"token", QString::fromLatin1(session.token())}};
}
struct Answer final {
  QByteArray body;
  bool closed = false;
};
bool completeFrame(const QByteArray& bytes) {
  return bytes.size() > 4 &&
         qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(
             bytes.constData())) <= quint32(bytes.size() - 4);
}
// Reads the editor's one answer frame, then whether it closed afterwards.
Answer authenticate(const SecuritySession& session, const QJsonObject& message,
                    int closeWaitMs = 3000) {
  QLocalSocket client;
  client.connectToServer(session.endpoint());
  require(client.waitForConnected(3000), "Cannot reach the native transport");
  sendFrame(client, message);
  QByteArray bytes;
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 3000 && !completeFrame(bytes) &&
         client.state() == QLocalSocket::ConnectedState) {
    client.waitForReadyRead(50);
    bytes += client.readAll();
  }
  timer.restart();
  while (timer.elapsed() < closeWaitMs &&
         client.state() == QLocalSocket::ConnectedState)
    client.waitForReadyRead(50);
  bytes += client.readAll();
  Answer answer;
  answer.closed = client.state() != QLocalSocket::ConnectedState;
  if (bytes.isEmpty()) return answer;
  require(completeFrame(bytes) &&
              qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(
                  bytes.constData())) == quint32(bytes.size() - 4),
          "The native transport answered with more than one frame");
  answer.body = bytes.mid(4);
  return answer;
}
void requireMismatch(const Answer& answer, const QString& code) {
  const QJsonObject expected{
      {"error",
       QJsonObject{{"code", code},
                   {"details", QJsonObject{{"editor", liveIdentity()}}}}}};
  require(!answer.body.isEmpty() &&
              parseMessage(answer.body, maximumResponseBytes) == expected,
          "The editor did not refuse a foreign protocol or build");
  require(answer.closed, "The editor kept a refused connection open");
}
void transportAuthentication(const QString& directory) {
  TransportHost host(directory + "/transport.json");
  const SecuritySession& session = *host.security;
  QJsonObject foreignBuild       = authentication(session);
  QJsonObject build              = buildIdentity();
  build.insert("executable_sha256", QString(64, 'b'));
  foreignBuild.insert("build", build);
  requireMismatch(authenticate(session, foreignBuild), "build_mismatch");
  QJsonObject previous = authentication(session);
  previous.insert("protocol", "groundzero-live-v1");
  requireMismatch(authenticate(session, previous), "protocol_mismatch");
  QJsonObject wrongToken = foreignBuild;
  wrongToken.insert("token", QString(64, 'c'));
  requireMismatch(authenticate(session, wrongToken), "build_mismatch");
  QJsonObject tokenOnly = authentication(session);
  tokenOnly.insert("token", QString(64, 'c'));
  const Answer denied = authenticate(session, tokenOnly);
  require(denied.body.isEmpty() && denied.closed,
          "A wrong token was answered instead of refused by closing");
  const Answer accepted   = authenticate(session, authentication(session), 0);
  const QJsonObject hello = parseMessage(accepted.body, maximumResponseBytes);
  require(hello.value("protocol") == QJsonValue(liveProtocol) &&
              hello.value("build") == QJsonValue(buildIdentity()) &&
              hello.value("instance_id") == session.instanceId(),
          "The editor hello omitted its protocol, build or instance");
}
void replacementCleanup(SecuritySession& publisher, const QString& path) {
  const QString socket = publisher.endpoint();
  require(QFile::rename(path, path + ".owned"), "Cannot move owned grant");
  require(QFile::rename(socket, socket + ".owned"), "Cannot move owned socket");
  for (const QString& entry : {path, socket}) {
    QFile foreign(entry);
    require(foreign.open(QIODevice::WriteOnly | QIODevice::NewOnly),
            "Cannot substitute fixture");
    require(foreign.write("replacement") == 11,
            "Cannot write replacement fixture");
  }
  require(!publisher.revoke(),
          "Substitution was not reported during revocation");
  for (const QString& entry : {path, socket}) {
    QFile foreign(entry);
    require(
        foreign.open(QIODevice::ReadOnly) && foreign.readAll() == "replacement",
        "Revocation deleted or modified replacement");
  }
}

void ordinaryCleanup(const QString& path) {
  SecuritySession publisher(path, "inspect");
  publisher.listenDescriptor();
  publisher.publish();
  const QString socket = publisher.endpoint();
  require(publisher.revoke(), "Owned grant revocation failed");
  require(!QFile::exists(path) && !QFile::exists(socket),
          "Owned grant or socket remained after revocation");
}

#ifdef Q_OS_MACOS
void extendedAclAdmission(const QString& path) {
  QProcess chmod;
  chmod.start("/bin/chmod", {"+a", "everyone allow read", path});
  require(chmod.waitForFinished(3000) && chmod.exitCode() == 0,
          "Cannot create the native extended-ACL fixture");
  struct stat info{};
  require(stat(QFile::encodeName(path).constData(), &info) == 0 &&
              (info.st_mode & 07777) == 0600,
          "Extended-ACL fixture changed the POSIX mode oracle");
  deniedRead(path);
  chmod.start("/bin/chmod", {"-N", path});
  require(chmod.waitForFinished(3000) && chmod.exitCode() == 0,
          "Cannot remove the native extended-ACL fixture");
  require(!SecuritySession::read(path)->token().isEmpty(),
          "Private grant did not recover after ACL removal");
}
#endif
}  // namespace
int main(int argc, char* argv[]) {
  if (argc > 1) return runConnector(argc, argv);
  QCoreApplication application(argc, argv);
  try {
#ifdef Q_OS_MACOS
    QTemporaryDir directory("/private/tmp/gz-l-XXXXXX");
#else
    QTemporaryDir directory("/tmp/gz-l-XXXXXX");
#endif
    require(directory.isValid(), "Cannot create private native test directory");
    const QString root = QFileInfo(directory.path()).canonicalFilePath();
    const QString path = root + "/session.json";
    SecuritySession publisher(path, "edit");
    require(publisher.listenDescriptor() >= 0,
            "Native listener creation failed");
    publisher.publish();
    const auto reader = SecuritySession::read(path);
    require(reader->instanceId() == publisher.instanceId(),
            "Grant identity changed");
    require(reader->token() == publisher.token(), "Grant token changed");
    require(!constantTimeTokenEquals(publisher.token(), QByteArray(64, 'z')),
            "Wrong token accepted");
    versionedHandshake(publisher, path, false);
    versionedHandshake(publisher, path, true);
    buildMismatchBeforeConnect(publisher, path, root);
    protocolMismatchBeforeConnect(publisher, path, root);
    fileAdmission(publisher, path, root);
    transportAuthentication(root);
#ifdef Q_OS_MACOS
    extendedAclAdmission(path);
#endif
    replacementCleanup(publisher, path);
    ordinaryCleanup(root + "/ordinary.json");
    std::puts(
        "PASS: actual native security, versioned connector handshake, "
        "mismatch refusals, private-file errors, EOF, cleanup");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
