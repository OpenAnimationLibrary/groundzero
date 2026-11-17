#include "automation_live_security.h"

#include <sys/stat.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>
#include <stdexcept>

#include "automation_live_protocol.h"

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
void connectorRoundTrip(SecuritySession& publisher, const QString& path,
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
  require(constantTimeTokenEquals(auth.value("token").toString().toLatin1(),
                                  publisher.token()),
          "Connector sent incorrect token");
  const QJsonObject hello{{"instance_id", publisher.instanceId()},
                          {"client_id", auth.value("client_id")},
                          {"mode", publisher.mode()},
                          {"tools", operations()},
                          {"limits", limits()}};
  sendFrame(peer, hello);
  require(parseMessage(readFrame(connector), maximumResponseBytes) == hello,
          "Connector did not expose authenticated hello");
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
void fileAdmission(const QString& path, const QString& directory) {
  const auto encoded = QFile::encodeName(path);
  require(chmod(encoded.constData(), 0644) == 0, "chmod failed");
  deniedRead(path);
  QProcess connector;
  connector.start(QCoreApplication::applicationFilePath(),
                  {"--automation-live-connect", path});
  connector.closeWriteChannel();
  require(connector.waitForFinished(3000), "Insecure connector admission hung");
  require(
      connector.exitCode() == 1 && connector.readAllStandardOutput().isEmpty(),
      "Insecure connector admission did not fail closed");
  require(connector.readAllStandardError() ==
              "Groundzero live connector failed: live_security.\n",
          "Connector leaked unexpected diagnostics");
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
    connectorRoundTrip(publisher, path, false);
    connectorRoundTrip(publisher, path, true);
    fileAdmission(path, root);
    replacementCleanup(publisher, path);
    ordinaryCleanup(root + "/ordinary.json");
    std::puts(
        "PASS: actual native security, connector round trip, private-file "
        "errors, EOF, cleanup");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
