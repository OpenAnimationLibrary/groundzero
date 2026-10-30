#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

// Owns the installed catalog set for the entire application lifetime. The
// settings owner persists languageChanged only after a complete successful
// switch; this class is also usable with an isolated catalog directory in CI.
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
  bool setLanguage(const QString &language, QString *error = nullptr);

signals:
  // Emitted after catalogs change, before persistent UI owners are refreshed.
  // Native property metadata and the saved preference are updated here.
  void languageChanged(const QString &language);
};
