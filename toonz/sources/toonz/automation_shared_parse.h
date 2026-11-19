#pragma once

#include "automation.h"

class QFile;

namespace GroundzeroAutomation {

// Wall-clock and memory bounds of one helper parse, start-up included.
constexpr int SharedParseDeadlineMs     = 10000;
// The graphics probe's bound: it starts a whole application and a GL context,
// which on a loaded machine can take far longer than the 0.5 s it usually does.
constexpr int GraphicsProbeDeadlineMs   = 20000;
constexpr qint64 SharedParseMemoryBytes = 1024LL * 1024 * 1024;

// Parse a user-authored shared file in a short-lived helper process.
//
// The helper is this executable in parse mode. It reads a private copy of the
// exact hashed bytes, so a native parser never sees the shared path, and its
// crash, hang or memory growth ends only the helper.
//
// Args:
//   context: Worker owning the private runtime directory for the copy.
//   kind: studio_palette, template, pose_set or pose_thumbnail:<index>.
//   bytes: The hashed file bytes.
//   failureCode: Code for an unparseable file: unsupported_palette or
//   invalid_library_item.
// Returns:
//   The helper's bounded native summary.
// Throws:
//   Error: failureCode with details.cause crashed, timeout, memory or
//   rejected; out-of-profile files raise unsupported_palette for palettes
//   and resource_limit otherwise, with details.cause out_of_bounds.
QJsonObject parseSharedFile(Context& context, const QString& kind,
                            const QByteArray& bytes,
                            const QString& failureCode);

// Run the helper side: parse <workspace>/input.<ext> and write one JSON line.
//
// Args:
//   context: Private helper context whose workspace holds the copy.
//   kind: The parse kind passed by the parent worker.
//   output: Reserved protocol output that receives the reply.
// Returns:
//   Process exit code; 0 when a reply was written.
int runSharedParseHelper(Context& context, const QString& kind, QFile& output);

// Bounded outcome of a probe helper, which reads no input file.
struct HelperProbeRun final {
  // The helper's JSON reply; empty when it wrote none.
  QJsonObject reply;
  // Empty, crashed, timeout or memory.
  QString cause;
  qint64 elapsedMs = 0;
};

// Run this executable in helper mode `kind` in a private directory, with the
// worker's environment and the parse helper bounds.
//
// Args:
//   context: Worker owning the private runtime directory.
//   kind: A probe kind the helper answers without input, such as gl_probe.
// Returns:
//   The reply and how the helper ended; a crash, hang or memory growth ends
//   only the helper.
HelperProbeRun runProbeHelper(Context& context, const QString& kind);

// Return a process's physical memory footprint in bytes, or -1 if unknown.
qint64 processMemoryBytes(qint64 processId);

}  // namespace GroundzeroAutomation
