#pragma once

#include <QJsonObject>

namespace GroundzeroAutomation {

/// Identify the running main executable once per process.
/// Returns:
///   {application, version, executable_sha256}; the hash is the compared value
///   and covers the executable holding every automation and bridge source.
/// Raises:
///   Error: connector_io when the executable cannot be read.
QJsonObject buildIdentity();

/// Check one value against the build identity schema.
/// Args:
///   value: Untrusted JSON from a session file, handshake or hello.
/// Returns:
///   True when it is an object with exactly the three bounded identity fields.
bool validBuildIdentity(const QJsonValue& value);

}  // namespace GroundzeroAutomation
