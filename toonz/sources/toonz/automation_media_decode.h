#pragma once

#include <QString>
#include <QStringList>

class QFile;

namespace GroundzeroAutomation {
class Context;

// Largest dependency file the media_decode check decodes.
constexpr qint64 MediaDecodeFileLimitBytes = 256LL * 1024 * 1024;
// Files one media helper decodes before the worker starts another.
constexpr int MediaFilesPerHelper = 64;

// The extensions media_decode decodes; every other one is unsupported_format.
const QStringList& mediaDecodeFormats();

// Run the media helper side: decode each file of <workspace>/manifest.json
// and write one JSON line per file as soon as it is decoded, so a crash on a
// later file leaves the earlier answers.
//
// Args:
//   context: Private helper context whose workspace holds the copies.
//   output: Reserved protocol output that receives the lines.
// Returns:
//   Process exit code; 0 when every file was answered.
int runMediaDecodeHelper(Context& context, QFile& output);

}  // namespace GroundzeroAutomation
