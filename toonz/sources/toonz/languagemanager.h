#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

// Owns the installed catalog set for the entire application lifetime. The
// settings owner persists languageSelected, emitted only after a complete
// successful switch chosen by the user; this class is also usable with an
// isolated catalog directory in CI.
class UiLanguageManager final : public QObject {
  Q_OBJECT
  class Imp;
  std::unique_ptr<Imp> m_imp;

public:
  explicit UiLanguageManager(QObject *parent = nullptr);
  ~UiLanguageManager() override;
  static UiLanguageManager *instance();

  void setCatalogDirectory(const QString &directory);
  QString catalogDirectory() const;
  QString currentLanguage() const;
  QStringList availableLanguages() const;
  // A choice of the user. On failure the current language is kept; on
  // success languageSelected is emitted, also when it was already current.
  bool setLanguage(const QString &language, QString *error = nullptr);
  // Applies the saved language at startup. If it cannot be loaded, English is
  // used for this session only and false is returned with the reason:
  // languageSelected is not emitted, so a missing or damaged catalog does not
  // replace the saved choice.
  bool startWithLanguage(const QString &saved, QString *error = nullptr);

signals:
  // Emitted after catalogs change, before persistent UI owners are refreshed.
  // Native property metadata is updated here.
  void languageChanged(const QString &language);
  // Emitted after a successful choice of the user, which the settings owner
  // saves. Never emitted for the startup fallback.
  void languageSelected(const QString &language);

private:
  bool apply(const QString &language, QString *error);
};
