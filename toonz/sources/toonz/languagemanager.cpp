#include "languagemanager.h"
#include "toonzqt/uitranslations.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QMap>
#include <QPointer>
#include <QReadWriteLock>
#include <QThread>
#include <QTranslator>
#include <vector>

namespace {
using Catalogs = std::vector<std::unique_ptr<QTranslator>>;

QString localeForLanguage(const QString &language) {
  const QMap<QString, QString> names{
      {QString::fromUtf8("日本語"), "ja"}, {"Italiano", "it"},
      {QString::fromUtf8("Français"), "fr"},
      {QString::fromUtf8("Español"), "es"},
      {QString::fromUtf8("中文"), "zh_CN"}, {"Deutsch", "de"},
      {QString::fromUtf8("Русский"), "ru"},
      {QString::fromUtf8("한국어"), "ko"},
      {QString::fromUtf8("Čeština"), "cs"}, {"Portuguese", "pt_BR"}};
  return names.value(language, language);
}

class SourceTranslator final : public QTranslator {
  mutable QReadWriteLock m_lock;
  Catalogs m_catalogs;

public:
  bool isEmpty() const override { return false; }
  void replace(Catalogs catalogs) {
    QWriteLocker lock(&m_lock);
    m_catalogs.swap(catalogs);
  }
  QString translate(const char *context, const char *source,
                    const char *disambiguation = nullptr,
                    int plural = -1) const override {
    if (!source) return QString();
    QString value;
    {
      QReadLocker lock(&m_lock);
      for (auto it = m_catalogs.rbegin(); it != m_catalogs.rend(); ++it) {
        value = (*it)->translate(context, source, disambiguation, plural);
        if (!value.isNull()) break;
      }
      if (value.isNull()) value = QString::fromUtf8(source);
      value.detach();
    }
    // Resolve plural placeholders before recording the result's identity;
    // QCoreApplication otherwise makes a new buffer after translate returns.
    if (plural >= 0) {
      value.replace("%Ln", QLocale().toString(plural));
      value.replace("%n", QString::number(plural));
    }
    return UiTranslations::record(context, source, disambiguation, plural, value);
  }
};
}  // namespace

class UiLanguageManager::Imp {
public:
  SourceTranslator translator;
  QString directory;
  QString language;
  bool changing = false;
};

UiLanguageManager::UiLanguageManager(QObject *parent)
    : QObject(parent), m_imp(new Imp) {
  UiTranslations::initialize();
  QCoreApplication::installTranslator(&m_imp->translator);
}

UiLanguageManager::~UiLanguageManager() {
  QCoreApplication::removeTranslator(&m_imp->translator);
}

UiLanguageManager *UiLanguageManager::instance() {
  static QPointer<UiLanguageManager> manager;
  if (!manager) manager = new UiLanguageManager(qApp);
  return manager;
}

void UiLanguageManager::setCatalogDirectory(const QString &directory) {
  m_imp->directory = directory;
}
QString UiLanguageManager::catalogDirectory() const { return m_imp->directory; }
QString UiLanguageManager::currentLanguage() const { return m_imp->language; }

QStringList UiLanguageManager::availableLanguages() const {
  QStringList result{QStringLiteral("English")};
  const QDir directory(m_imp->directory);
  for (const QString &name : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                 QDir::Name)) {
    if (QFileInfo::exists(directory.filePath(name + "/toonz.qm")))
      result.push_back(name);
  }
  return result;
}

bool UiLanguageManager::setLanguage(const QString &language, QString *error) {
  if (error) error->clear();
  if (QThread::currentThread() != thread() || m_imp->changing) {
    if (error) *error = tr("The interface language is already being changed.");
    return false;
  }
  if (language == m_imp->language) return true;
  if (language.isEmpty() || language.contains('/') || language.contains('\\') ||
      language == "." || language == "..") {
    if (error) *error = tr("The selected interface language is unavailable.");
    return false;
  }

  Catalogs next;
  if (language != "English") {
    const QDir directory(QDir(m_imp->directory).filePath(language));
    for (const QString &module : {QString("toonz"), QString("toonzqt"),
         QString("tnzcore"), QString("toonzlib"), QString("colorfx"),
         QString("tnztools"), QString("image")}) {
      auto catalog = std::unique_ptr<QTranslator>(new QTranslator);
      const QString path = directory.filePath(module + ".qm");
      if (!QFileInfo::exists(path) || !catalog->load(path)) {
        if (error) *error = tr("The %1 interface catalog for %2 could not be "
                              "loaded. The current language was kept.")
                              .arg(module, language);
        return false;
      }
      next.push_back(std::move(catalog));
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QString qtPath = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
#else
    const QString qtPath = QLibraryInfo::location(QLibraryInfo::TranslationsPath);
#endif
    auto qt = std::unique_ptr<QTranslator>(new QTranslator);
    const QString locale = localeForLanguage(language);
    if (qt->load(directory.filePath("qtbase.qm")) ||
        qt->load("qt_" + locale, qtPath) ||
        qt->load("qtbase_" + locale, qtPath))
      next.push_back(std::move(qt));
  }

  UiTranslations::captureAll();
  m_imp->changing = true;
  m_imp->translator.replace(std::move(next));
  m_imp->language = language;
  emit languageChanged(language);
  UiTranslations::refresh();
  m_imp->changing = false;
  return true;
}
