#pragma once

#include "tcommon.h"
#include <QObject>
#include <QString>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QStringView>
#endif
#include <functional>
#include <tuple>
#include <utility>
class QComboBox;
class QTabBar;

#undef DVAPI
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

// UI captions retain their original translation identity. No lookup by the
// displayed text is performed: unrelated/user-authored strings are never
// candidates merely because their spelling matches a translated caption.
namespace UiTranslations {
using Recipe = std::function<QString()>;

DVAPI QString record(const char *context, const char *source,
                     const char *disambiguation, int plural,
                     const QString &translated);
DVAPI Recipe recipe(const QString &text);
DVAPI QString remember(const QString &text, Recipe translate);
DVAPI void bind(QObject *object, const char *property, const char *context,
                const QString &source,
                const QString &disambiguation = QString(), int plural = -1);
DVAPI void onLanguageChanged(QObject *owner, std::function<void()> callback);
DVAPI void captureAll();
DVAPI void refresh();
DVAPI void initialize();
// Only call for a combo's private, presentation-only item model. Scene,
// project, preset and other data-backed selectors must refresh explicitly.
DVAPI void watchCombo(QComboBox *combo);
// Opt in native caption tabs only; document and room names are user data.
DVAPI void watchTabBar(QTabBar *tabs);
// Used by lifecycle diagnostics and regression coverage.
DVAPI std::size_t retainedCaptionCount();

inline QString ownArgument(const char *value) {
  return QString::fromUtf8(value ? value : "");
}
inline QString ownArgument(char *value) {
  return QString::fromUtf8(value ? value : "");
}
inline QString ownArgument(QLatin1String value) { return QString(value); }
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
inline QString ownArgument(QStringRef value) { return value.toString(); }
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
inline QString ownArgument(QStringView value) { return value.toString(); }
#endif
template <typename T> T ownArgument(T value) { return value; }

// Formatting retains the source identity and freezes the argument values at
// the time the caption is assigned. A later language change cannot evaluate a
// dangling local reference, repeat a command, or read mutable scene data.
class Text : public QString {
  Recipe m_translate;

public:
  Text(const QString &value) : QString(value), m_translate(recipe(value)) {}
  Text(const QString &value, Recipe translate)
      : QString(remember(value, translate)), m_translate(std::move(translate)) {}

  Text appended(const QString &suffix) const {
    QString value = QString(*this) + suffix;
    if (!m_translate) return Text(value);
    auto translate = m_translate;
    return Text(value, [translate, suffix]() { return translate() + suffix; });
  }

  template <typename... Args> Text arg(Args... args) const {
    QString value = QString::arg(args...);
    if (!m_translate) return Text(value);
    auto translate = m_translate;
    auto values = std::make_tuple(ownArgument(args)...);
    return Text(value, [translate, values]() {
      return std::apply([&](const auto &...args) {
        return translate().arg(args...);
      }, values);
    });
  }
};

inline Text text(const QString &value) { return Text(value); }
}  // namespace UiTranslations
