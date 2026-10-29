#pragma once

#include "cadmiumconsensus.h"
#include "cadmiumsession.h"
#include "tundo.h"

#include <algorithm>
#include <limits>

namespace Cadmium {

constexpr qint64 MaximumCacheBytes = 512LL * 1024 * 1024;

inline qint64 addMemoryBytes(qint64 left, qint64 right) {
  if (left < 0 || right < 0 || right > std::numeric_limits<qint64>::max() - left)
    return std::numeric_limits<qint64>::max();
  return left + right;
}

// Count owned payload conservatively: implicitly shared arrays/images may be
// counted more than once. Native image estimates do not include renderer/driver
// caches or arbitrary style internals, so this is not a process RSS limit.
inline qint64 frameMemoryBytes(const Frame &frame) {
  qint64 bytes = sizeof(Frame);
  for (qint64 payload : {
           qint64(TUndo::getImageMemorySize(frame.original.getPointer())),
           qint64(TUndo::getImageMemorySize(frame.lineSource.getPointer())),
           qint64(frame.lines.sizeInBytes()),
           qint64(frame.protectionMask.sizeInBytes()),
           qint64(frame.labels.capacity()) * qint64(sizeof(int)),
           qint64(frame.regionStyles.capacity()) * qint64(sizeof(int)),
           qint64(frame.nativeRegionIndices.capacity()) * qint64(sizeof(int)),
           qint64(frame.regionColors.capacity()) * qint64(sizeof(QColor)),
           qint64(frame.nativeRegionColors.capacity()) * qint64(sizeof(NativeColor)),
           qint64(frame.fingerprint.capacity())})
    bytes = addMemoryBytes(bytes, payload);
  for (const QString &warning : frame.warnings)
    bytes = addMemoryBytes(bytes, sizeof(QString) + qint64(warning.capacity()) * sizeof(QChar));
  return bytes;
}

inline bool hasRegionDecisions(const QVector<RegionProposal> &regions) {
  // Region zero is the automatically protected exterior, not an artist choice.
  for (int region = 1; region < regions.size(); ++region) {
    const RegionProposal &proposal = regions[region];
    if (proposal.selected != -1 || proposal.locked || proposal.protectedRegion ||
        !proposal.candidates.isEmpty() || !proposal.references.isEmpty())
      return true;
  }
  return false;
}

inline qint64 sessionDrawingMemoryBytes(const SessionDrawing &drawing) {
  qint64 bytes = sizeof(SessionDrawing) + drawing.targetFingerprint.capacity() +
      drawing.labelsFingerprint.capacity() +
      qint64(drawing.references.capacity()) * sizeof(SessionReference);
  bytes = addMemoryBytes(bytes, proposalMemoryBytes(drawing.regions));
  for (const SessionReference &reference : drawing.references)
    bytes = addMemoryBytes(bytes, reference.fingerprint.capacity());
  return bytes;
}

inline qint64 sessionMemoryBytes(const Session &session) {
  qint64 bytes = sizeof(Session) + qint64(session.levelId.capacity()) * sizeof(QChar) +
      qint64(session.options.lineLevelId.capacity()) * sizeof(QChar) +
      session.options.lineSourceKey.capacity() +
      qint64(session.drawings.capacity() - session.drawings.size()) * sizeof(SessionDrawing);
  for (const SessionDrawing &drawing : session.drawings)
    bytes = addMemoryBytes(bytes, sessionDrawingMemoryBytes(drawing));
  return bytes;
}

// Clone, labels, line image, connected-component work queue, display image and
// region metadata headroom. Admission is also checked against actual results.
inline qint64 analysisMemoryReservation(qint64 nativeBytes) {
  qint64 bytes = 32LL * 1024 * 1024;
  for (int i = 0; i < 6; ++i) bytes = addMemoryBytes(bytes, nativeBytes);
  return bytes;
}

struct CacheEntry {
  qint64 bytes;
  bool evictable;
};
struct CacheAdmission {
  bool allowed = false;
  QVector<int> evictions;
};

// Plan first, then evict: a request which cannot fit must preserve every entry.
// The caller marks current/in-flight entries and artist decisions non-evictable.
inline CacheAdmission planCacheAdmission(const QVector<CacheEntry> &entries,
                                        qint64 fixedBytes, qint64 requestedBytes,
                                        qint64 limit = MaximumCacheBytes) {
  CacheAdmission result;
  if (limit < 0 || fixedBytes < 0 || requestedBytes < 0 ||
      fixedBytes > limit || requestedBytes > limit - fixedBytes)
    return result;
  qint64 available = limit - fixedBytes - requestedBytes;
  qint64 retained = 0;
  for (const CacheEntry &entry : entries) {
    if (entry.bytes < 0) return result;
    if (!entry.evictable) retained = addMemoryBytes(retained, entry.bytes);
  }
  if (retained > available) return result;
  available -= retained;
  // Prefer retaining later drawings when inspection-only caches need eviction.
  for (int i = entries.size() - 1; i >= 0; --i) {
    if (!entries[i].evictable) continue;
    if (entries[i].bytes <= available) available -= entries[i].bytes;
    else result.evictions.append(i);
  }
  result.allowed = true;
  return result;
}

}  // namespace Cadmium
