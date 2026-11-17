#pragma once

#include "automation_live_security.h"

namespace GroundzeroAutomation {
namespace Live {
namespace SecurityDetail {

constexpr int maximumSessionBytes = 4096;

// Holds opened ancestor directories while validating and using a grant.
class PrivateFiles {
 public:
  virtual ~PrivateFiles() = default;
  virtual QString directory() const = 0;
  virtual QByteArray read() = 0;
  virtual void publish(const QByteArray& bytes) = 0;
  virtual qintptr listenDescriptor(const QString& endpoint) = 0;
  virtual qintptr acceptDescriptor() = 0;
  virtual void captureSocket(const QString& endpoint) = 0;
  virtual void verifySocket(const QString& endpoint) const = 0;
  virtual bool revoke() noexcept = 0;
};

std::unique_ptr<PrivateFiles> openPrivateFiles(const QString& path,
                                               bool create);
QByteArray randomBytes(int count);
void verifyPeer(qintptr descriptor, bool client);
void closeAccepted(qintptr descriptor) noexcept;
QString endpointName(const QString& directory, const QString& randomName);

}  // namespace SecurityDetail
}  // namespace Live
}  // namespace GroundzeroAutomation
