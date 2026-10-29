#pragma once

#ifndef LEVELSELECTION_H
#define LEVELSELECTION_H

#include "toonz/txshlevel.h"

#include <QSet>

// Keep selected levels alive while hashing their identity. Without this
// overload, the smart pointer's implicit bool conversion puts every non-null
// level in the same hash bucket.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
inline size_t qHash(const TXshLevelP &level, size_t seed = 0) noexcept {
#else
inline uint qHash(const TXshLevelP &level, uint seed = 0) noexcept {
#endif
  return qHash(level.getPointer(), seed);
}

#endif  // LEVELSELECTION_H
