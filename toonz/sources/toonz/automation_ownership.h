#pragma once

#include <QString>
#include <QStringList>

#ifdef _WIN32
using AutomationOwnershipHandle = void*;
#endif

namespace GroundzeroAutomation {

// A claim on a directory that lasts as long as the process that took it. The
// operating system drops it when the process ends, so another process id that
// happens to match the owner's never keeps it alive.
class OwnershipClaim final {
public:
  // With keepMarker the marker stays after release, unheld, so a later process
  // finds the directory abandoned and may remove it.
  explicit OwnershipClaim(bool keepMarker = false) : m_keepMarker(keepMarker) {}
  ~OwnershipClaim() { release(); }
  OwnershipClaim(const OwnershipClaim&)            = delete;
  OwnershipClaim& operator=(const OwnershipClaim&) = delete;

  // Claims an existing directory; false when it cannot be claimed.
  bool acquire(const QString& directory);
  // Ends the claim; the marker file goes unless the claim keeps it.
  void release();

  // True when the directory carries a claim marker that no live process holds.
  static bool abandoned(const QString& directory);
#ifndef _WIN32
  // The same for the directory open at `directory`, reached without a path.
  bool acquireAt(int directory);
  static bool abandonedAt(int directory);
#endif
  // Removes every directory under parent matching a name filter that carries an
  // unheld claim marker, and returns how many it removed.
  static int removeAbandoned(const QString& parent, const QStringList& filters);

private:
  bool m_keepMarker = false;
  QString m_marker;
#ifdef _WIN32
  AutomationOwnershipHandle m_handle = nullptr;
#else
  int m_descriptor = -1;
  // The claimed directory, for a claim taken by descriptor.
  int m_directory = -1;
#endif
};

}  // namespace GroundzeroAutomation
