#pragma once

#include <QJsonObject>

namespace GroundzeroAutomation {

/// Identify the running build once per process.
/// Returns:
///   {application, version, build_digest}. build_digest is the compared value:
///   the SHA-256 of a manifest listing the SHA-256 of the main executable and
///   of each library this project builds, read where the dynamic loader loaded
///   it from, in name order. Third-party and system libraries are not part of
///   it.
/// Raises:
///   Error: connector_io when a project library is not loaded or a covered
///   file cannot be read.
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

/// Print the build manifest as one JSON line: {build_digest, files: [{name,
/// path, sha256}]}, so a runner can verify the digest over the same files.
/// Returns:
///   The process exit code: 0 when printed, 1 on failure.
int printBuildManifest(int argc, char* argv[]);

}  // namespace GroundzeroAutomation
