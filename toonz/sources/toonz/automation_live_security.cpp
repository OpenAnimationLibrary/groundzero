#include "automation_live_security.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <cmath>
#include <limits>

#include "automation.h"
#include "automation_live_protocol.h"
#include "automation_live_security_p.h"

namespace GroundzeroAutomation {
namespace Live {
namespace {

void requireMode(const QString& mode) {
  if (mode != "inspect" && mode != "edit")
    throw Error("live_security",
                "The live grant mode must be inspect or edit.");
}

void requireProcessId(const QJsonValue& value) {
  const double pid = value.toDouble(-1);
  if (!std::isfinite(pid) || pid <= 0 || pid != std::floor(pid) ||
      pid > std::numeric_limits<quint32>::max())
    throw Error("live_security",
                "The live grant process identifier is invalid.");
}

void requireDescriptor(const QJsonObject& object) {
  const QStringList keys = {"endpoint", "instance_id", "mode",
                            "pid",      "protocol",    "token"};
  if (object.keys() != keys ||
      object.value("protocol") != QJsonValue("groundzero-live-v1"))
    throw Error("live_security", "The live grant schema is invalid.");
  for (const QString& key : {QString("endpoint"), QString("instance_id"),
                             QString("mode"), QString("token")})
    if (!object.value(key).isString())
      throw Error("live_security", "The live grant requires string fields.");
  requireProcessId(object.value("pid"));
}

void requireIdentity(const QString& id, const QByteArray& token) {
  const QUuid uuid(id);
  if (uuid.isNull() || uuid.toString(QUuid::WithoutBraces) != id)
    throw Error("live_security", "The live instance identifier is invalid.");
  const QRegularExpression hex(
      QRegularExpression::anchoredPattern("[0-9a-f]{64}"));
  if (!hex.match(QString::fromLatin1(token)).hasMatch())
    throw Error("live_security", "The live grant token encoding is invalid.");
}

}  // namespace

struct SecuritySession::Impl final {
  std::unique_ptr<SecurityDetail::PrivateFiles> files;
  QString instance;
  QString endpoint;
  QString mode;
  QByteArray token;
  bool publisher = false;
  bool published = false;
};

SecuritySession::SecuritySession(const QString& path, const QString& mode)
    : m_impl(std::make_unique<Impl>()) {
  requireMode(mode);
  m_impl->files = SecurityDetail::openPrivateFiles(path, true);
  m_impl->instance = secureUuid();
  m_impl->endpoint = SecurityDetail::endpointName(
      m_impl->files->directory(),
      QString::fromLatin1(secureRandomBytes(16).toHex()));
  m_impl->mode = mode;
  m_impl->token = secureRandomBytes(32).toHex();
  m_impl->publisher = true;
}

SecuritySession::SecuritySession(const QString& path)
    : m_impl(std::make_unique<Impl>()) {
  m_impl->files = SecurityDetail::openPrivateFiles(path, false);
  const QJsonObject object =
      parseMessage(m_impl->files->read(), SecurityDetail::maximumSessionBytes);
  requireDescriptor(object);
  m_impl->instance = object.value("instance_id").toString();
  m_impl->endpoint = object.value("endpoint").toString();
  m_impl->mode = object.value("mode").toString();
  m_impl->token = object.value("token").toString().toLatin1();
  requireMode(m_impl->mode);
  requireIdentity(m_impl->instance, m_impl->token);
  m_impl->files->verifySocket(m_impl->endpoint);
}

SecuritySession::~SecuritySession() { revoke(); }

std::unique_ptr<SecuritySession> SecuritySession::read(const QString& path) {
  return std::unique_ptr<SecuritySession>(new SecuritySession(path));
}

QString SecuritySession::instanceId() const { return m_impl->instance; }
QString SecuritySession::endpoint() const { return m_impl->endpoint; }
QString SecuritySession::mode() const { return m_impl->mode; }
const QByteArray& SecuritySession::token() const { return m_impl->token; }

qintptr SecuritySession::listenDescriptor() {
  if (!m_impl->publisher || m_impl->published)
    throw Error("live_security", "Only a new GUI grant can create a listener.");
  return m_impl->files->listenDescriptor(m_impl->endpoint);
}

qintptr SecuritySession::acceptDescriptor() {
  return m_impl->files->acceptDescriptor();
}

void SecuritySession::publish() {
  if (!m_impl->publisher || m_impl->published || m_impl->token.isEmpty())
    throw Error("live_security", "The live grant cannot be published again.");
  m_impl->files->captureSocket(m_impl->endpoint);
  const QJsonObject grant{{"protocol", "groundzero-live-v1"},
                          {"instance_id", m_impl->instance},
                          {"endpoint", m_impl->endpoint},
                          {"pid", double(QCoreApplication::applicationPid())},
                          {"mode", m_impl->mode},
                          {"token", QString::fromLatin1(m_impl->token)}};
  const QByteArray bytes = QJsonDocument(grant).toJson(QJsonDocument::Compact);
  if (bytes.size() > SecurityDetail::maximumSessionBytes)
    throw Error("live_security", "The live grant exceeds its size limit.");
  m_impl->files->publish(bytes);
  m_impl->published = true;
}

void SecuritySession::verifyClient(qintptr descriptor) const {
  SecurityDetail::verifyPeer(descriptor, true);
}

void SecuritySession::verifyServer(qintptr descriptor) const {
  m_impl->files->verifySocket(m_impl->endpoint);
  SecurityDetail::verifyPeer(descriptor, false);
}

bool SecuritySession::revoke() noexcept {
  if (!m_impl) return true;
  m_impl->token.fill('\0');
  m_impl->token.clear();
  return m_impl->files ? m_impl->files->revoke() : true;
}

QByteArray secureRandomBytes(int count) {
  if (count < 1 || count > 64)
    throw Error("live_security", "The random request size is invalid.");
  return SecurityDetail::randomBytes(count);
}

QString secureUuid() {
  QByteArray bytes = secureRandomBytes(16);
  bytes[6] = char((uchar(bytes[6]) & 0x0f) | 0x40);
  bytes[8] = char((uchar(bytes[8]) & 0x3f) | 0x80);
  return QUuid::fromRfc4122(bytes).toString(QUuid::WithoutBraces);
}

void closeAcceptedDescriptor(qintptr descriptor) noexcept {
  SecurityDetail::closeAccepted(descriptor);
}

bool constantTimeTokenEquals(const QByteArray& first,
                             const QByteArray& second) {
  if (first.size() != 64 || second.size() != 64) return false;
  volatile unsigned int difference = 0;
  for (int index = 0; index < 64; ++index)
    difference = difference | (uchar(first[index]) ^ uchar(second[index]));
  return difference == 0;
}

}  // namespace Live
}  // namespace GroundzeroAutomation
