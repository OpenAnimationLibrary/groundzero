#pragma once

#include <QJsonArray>
#include <QString>

/// Restrict an empty disposable grant directory to the current native identity.
/// Args:
///   path: Absolute path to a new or existing empty directory without links.
void prepareMcpLivePrivateDirectory(const QString& path);

/// Create Windows ACL/reparse variants of an already working private grant.
/// Args:
///   sessionPath: The active fixture grant whose contents remain outside evidence.
/// Returns:
///   Variant paths and a protected positive baseline, or an empty POSIX array.
QJsonArray createMcpLiveInsecureFiles(const QString& sessionPath);

/// Remove only the token-bearing files and directories created by this fixture.
/// Raises:
///   std::runtime_error: A created path cannot be removed; cleanup is not certified.
void removeMcpLiveInsecureFiles();
