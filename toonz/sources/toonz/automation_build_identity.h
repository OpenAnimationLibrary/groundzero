#pragma once

#include <QJsonObject>

namespace GroundzeroAutomation {

/// Identify the running build once per process.
/// Returns:
///   {application, version, executable_sha256}. The compared value,
///   executable_sha256, is the SHA-256 of a manifest listing the SHA-256 of the
///   main executable and of each library this project builds that is present
///   beside it, in name order. Third-party and system libraries are not part of
///   it.
/// Raises:
///   Error: connector_io when one of those files cannot be read.
QJsonObject buildIdentity();

/// Start computing the build identity on another thread, so a later
/// buildIdentity() call on the GUI thread does not wait for the whole read.
void prefetchBuildIdentity();

/// Check one value against the build identity schema.
/// Args:
///   value: Untrusted JSON from a session file, handshake or hello.
/// Returns:
///   True when it is an object with exactly the three bounded identity fields.
bool validBuildIdentity(const QJsonValue& value);

}  // namespace GroundzeroAutomation
