#pragma once

#include <QObject>
#include <QStringList>
#include <functional>

class QComboBox;
class QMenu;
class QWidget;

// One application path for menu, Preferences, additional styles and Reload QSS.
// The native settings adapter is separate so this behavior can be tested with
// temporary installed themes and settings without starting an animation scene.
class ThemeApplication final : public QObject {
  Q_OBJECT
public:
  struct Settings {
    std::function<QString()> theme;
    std::function<QString()> additionalStyle;
    std::function<void(const QString &)> saveTheme;
    std::function<void(const QString &)> saveAdditionalStyle;
    std::function<void(const QString &)> refreshIcons;
  };

  ThemeApplication(const QString &directory, Settings settings,
                   QObject *parent = nullptr);
  QStringList availableThemes() const;
  QString currentTheme() const;
  bool applyTheme(const QString &name, QString *error = nullptr);
  bool applyAdditionalStyle(const QString &style, QString *error = nullptr);
  bool reload(QString *error = nullptr);
  // Applies the saved theme when nothing is styled yet. A theme or additional
  // style sheet that fails the checks is never applied; instead of leaving the
  // application unstyled, the saved theme without the additional sheet, or
  // the Default theme, is used for this session and the saved settings are
  // kept. Returns whether a style sheet was applied; error explains any
  // failure, including one recovered by a fallback. Once a theme is applied
  // this is reload(): interactive changes stay strict.
  bool startup(QString *error = nullptr);

  // Synchronizes presentation only; the Preferences owner handles user edits.
  void bindComboBox(QComboBox *combo);
  void synchronizeComboBox(QComboBox *combo) const;

signals:
  void themeChanged(const QString &name);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  bool apply(const QString &name, const QString &additional,
             bool saveAdditional, bool saveTheme, bool force, QString *error);
  QString m_directory;
  Settings m_settings;
  QString m_appliedTheme;
  QString m_appliedStyle;
  bool m_applying = false;
  bool m_initialized = false;
};

QMenu *createQuickThemeMenu(ThemeApplication &themes, QWidget *parent);

namespace NativeTheme {
ThemeApplication &instance();
}
