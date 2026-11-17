#pragma once

#include <QByteArray>
#include <QString>
#include <memory>

namespace GroundzeroAutomation {
namespace Live {

// Native-only grant ownership. Tokens never enter adapter arguments or logs.
class SecuritySession final {
 public:
  SecuritySession(const QString& sessionPath, const QString& mode);
  ~SecuritySession();
  SecuritySession(const SecuritySession&) = delete;
  SecuritySession& operator=(const SecuritySession&) = delete;

  static std::unique_ptr<SecuritySession> read(const QString& sessionPath);
  QString instanceId() const;
  QString endpoint() const;
  QString mode() const;
  const QByteArray& token() const;

  // POSIX listener ownership stays here; Qt must not unlink its pathname.
  qintptr listenDescriptor();
  qintptr acceptDescriptor();
  // Publish after listening. Windows uses QLocalServer::UserAccessOption.
  void publish();
  void verifyClient(qintptr descriptor) const;
  void verifyServer(qintptr descriptor) const;
  bool revoke() noexcept;

 private:
  explicit SecuritySession(const QString& sessionPath);
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

QByteArray secureRandomBytes(int count);
QString secureUuid();
void closeAcceptedDescriptor(qintptr descriptor) noexcept;
bool constantTimeTokenEquals(const QByteArray& first, const QByteArray& second);

// Creates QCoreApplication only; never initializes or owns an editor/worker.
// Stdout: authenticated hello first, then bounded length-framed responses.
int runConnector(int argc, char* argv[]);

}  // namespace Live
}  // namespace GroundzeroAutomation
