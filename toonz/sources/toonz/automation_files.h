#pragma once

#include <QString>

class QFile;

namespace GroundzeroAutomation {

// Opening and publishing files at paths that Context::resolvePath or a shared
// root walk approved, without trusting those paths a second time. A path that
// was approved can be replaced by a link before it is used, so the directory
// that holds the file is opened by descriptor, proven to be the approved
// directory, and only then is the file opened or published relative to it.

enum class OpenResult { Opened, NotRegular, Failed };

// Open the regular file at an approved absolute path for reading.
//
// The directory is opened without following links and must still be the
// approved directory; the name is opened without following a link and without
// blocking, so a named pipe or a device is reported as NotRegular.
//
// Throws:
//   Error: path_denied when a component of the path was redirected.
OpenResult openApprovedFile(QFile& file, const QString& path);

// Move a staged file or directory to the approved, not yet existing path.
//
// The destination directory is pinned as for openApprovedFile and nothing is
// replaced. Returns false when the destination exists, is on another volume or
// cannot be written.
//
// Throws:
//   Error: path_denied when a component of the path was redirected.
bool publishApproved(const QString& staged, const QString& path);

#ifdef BUILD_UI_PORT_TESTS
// Test-only: the next time `stage` is reached with a path at or below `path`
// (stages: validated, after a path is approved; pinned, after its directory is
// pinned), replace `path` with a symbolic link to `target`, once, and leave a
// `.fired` witness beside it.
void armPathSwap(const QString& path, const QString& target,
                 const QString& stage);
void firePathSwap(const QString& stage, const QString& path);
#endif

}  // namespace GroundzeroAutomation
