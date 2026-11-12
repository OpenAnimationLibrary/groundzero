#pragma once

#include <QDir>
#include <QDirIterator>
#include <QStringList>

namespace RasterBrush {
// Legacy PNG packs retain original source names in their decoded records;
// filesystem names additionally need a portable component and suffix budget.
inline QString legacyTipLibraryName(const QString &name) {
  QString result;
  for (QChar c : name)
    result += c.unicode() < 32 || QString("\\/:*?\"<>|").contains(c)
                  ? QChar('_') : c;
  result = result.trimmed();
  while (result.endsWith('.')) result.chop(1);
  if (result.startsWith('.')) result.prepend('_');
  const QString base = result.section('.', 0, 0).toUpper();
  if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
      (base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT")) &&
       base[3] >= QChar('1') && base[3] <= QChar('9')))
    result.prepend('_');
  // A UTF-16 unit cap can exceed Unix's byte component limit, or bisect a
  // surrogate pair. Consume complete code points within 120 UTF-8 bytes.
  int units = 0, bytes = 0;
  while (units < result.size()) {
    const int count = result.at(units).isHighSurrogate() && units + 1 < result.size() &&
                              result.at(units + 1).isLowSurrogate() ? 2 : 1;
    const int added = result.mid(units, count).toUtf8().size();
    if (bytes + added > 120) break;
    bytes += added;
    units += count;
  }
  result.truncate(units);
  while (result.endsWith('.') || result.endsWith(' ')) result.chop(1);
  return result.isEmpty() ? QString("brushes") : result;
}
inline QString legacyTipLibraryCollisionKey(const QString &name) {
  // Some supported filesystems compare canonical Unicode spellings and case.
  return name.normalized(QString::NormalizationForm_C).toCaseFolded()
      .normalized(QString::NormalizationForm_C);
}

// Staging lives beside committed entries so publication stays on one filesystem.
// A leading dot is not a portable hidden attribute: exclude these reserved
// transaction components explicitly in every library picker.
inline bool isLibraryStagingPath(const QString &relativePath) {
  const auto components = QDir::fromNativeSeparators(relativePath).split('/');
  for (const auto &component : components)
    if (component.startsWith(".brush-import-", Qt::CaseInsensitive) ||
        component.startsWith(".brush-install-", Qt::CaseInsensitive) ||
        component.startsWith(".abr-import-", Qt::CaseInsensitive))
      return true;
  return false;
}

inline QStringList publishedLibraryFiles(const QString &directory,
                                          const QStringList &filters,
                                          int maximumEntries = 0,
                                          bool allowFileLinks = false) {
  const QDir root(directory);
  QStringList result;
  QDir::Filters flags = QDir::Files;
  if (!allowFileLinks) flags |= QDir::NoSymLinks;
  QDirIterator iterator(root.path(), filters, flags,
                        QDirIterator::Subdirectories);
  while (iterator.hasNext()) {
    const auto relative = root.relativeFilePath(iterator.next());
    if (isLibraryStagingPath(relative)) continue;
    result << relative;
    if (maximumEntries > 0 && result.size() >= maximumEntries) break;
  }
  return result;
}
}  // namespace RasterBrush
