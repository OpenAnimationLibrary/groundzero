#pragma once

#ifndef THEMENAMES_H
#define THEMENAMES_H

#include <QString>
#include <QStringList>

// The theme to use for a saved theme name. An installed theme is kept. A theme
// that was renamed selects its new name, and any other name selects Default.
inline QString installedThemeName(const QString &saved,
                                  const QStringList &installed) {
  if (installed.contains(saved)) return saved;
  static const struct {
    const char *from, *to;
  } renamed[] = {
      {"ToonBoom", "Slate"}, {"Animate", "Charcoal"}, {"Premiere", "Midnight"}};
  for (const auto &theme : renamed) {
    const QString to = QString::fromLatin1(theme.to);
    if (saved == QLatin1String(theme.from) && installed.contains(to)) return to;
  }
  return QStringLiteral("Default");
}

#endif  // THEMENAMES_H
