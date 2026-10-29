#include "themeapplication.h"

#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QThread>
#include <QUrl>
#include <algorithm>
#include <utility>

namespace {
bool fail(QString *error, const QString &message) {
  if (error) *error = message;
  return false;
}

// QStyle has no public stylesheet-parse result. Check structural errors before
// touching the live application, including unterminated comments/quoted URLs.
bool completeStyleSheet(const QString &style, bool allowEmpty = false) {
  QString delimiters;
  QChar quote;
  bool comment = false;
  bool escaped = false;
  bool rule = false;
  bool code = false;
  for (int i = 0; i < style.size(); ++i) {
    const QChar c = style.at(i);
    const QChar next = i + 1 < style.size() ? style.at(i + 1) : QChar();
    if (comment) {
      if (c == '*' && next == '/') { comment = false; ++i; }
      continue;
    }
    if (!quote.isNull()) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == quote) quote = QChar();
      continue;
    }
    if (c == '/' && next == '*') { comment = true; ++i; continue; }
    if (!c.isSpace()) code = true;
    if (c == '\'' || c == '"') { quote = c; continue; }
    if (c == '{' || c == '(' || c == '[') {
      delimiters.append(c);
      if (c == '{') rule = true;
    } else if (c == '}' || c == ')' || c == ']') {
      if (delimiters.isEmpty()) return false;
      const QChar open = delimiters.at(delimiters.size() - 1);
      if ((c == '}' && open != '{') || (c == ')' && open != '(') ||
          (c == ']' && open != '[')) return false;
      delimiters.chop(1);
    }
  }
  return (rule || (allowEmpty && !code)) && !comment && quote.isNull() &&
         delimiters.isEmpty();
}

bool resolveAssets(QString &style, const QString &directory, QString *error) {
  const QRegularExpression urls(
      QStringLiteral("url\\(\\s*(['\"]?)([^)'\"]*)\\1\\s*\\)"),
      QRegularExpression::CaseInsensitiveOption);
  // Ignore examples and disabled rules inside comments, keeping offsets into
  // the original stylesheet so its comments and formatting stay intact.
  QString searchable = style;
  QChar quote;
  bool escaped = false;
  bool comment = false;
  for (int i = 0; i < searchable.size(); ++i) {
    const QChar c = style.at(i);
    const QChar next = i + 1 < style.size() ? style.at(i + 1) : QChar();
    if (comment) {
      searchable[i] = ' ';
      if (c == '*' && next == '/') {
        searchable[++i] = ' ';
        comment = false;
      }
    } else if (!quote.isNull()) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == quote) quote = QChar();
    } else if (c == '\'' || c == '"') quote = c;
    else if (c == '/' && next == '*') {
      searchable[i] = searchable[i + 1] = ' ';
      ++i;
      comment = true;
    }
  }
  auto matches = urls.globalMatch(searchable);
  QString resolved;
  int position = 0;
  while (matches.hasNext()) {
    const auto match = matches.next();
    const QString asset = match.captured(2).trimmed();
    resolved += style.mid(position, match.capturedStart() - position);
    position = match.capturedEnd();
    if (asset.isEmpty()) {
      resolved += match.captured();
      continue;
    }
    const QUrl url(asset);
    QString path;
    if (asset.startsWith(QLatin1String(":"))) path = asset;
    else if (url.scheme() == QLatin1String("qrc")) path = ':' + url.path();
    else if (url.isLocalFile()) path = url.toLocalFile();
    else if (QDir::isAbsolutePath(asset)) path = asset;
    else if (url.isRelative()) path = QDir(directory).filePath(asset);
    else {
      // Keep externally named resources as supplied. No network request is
      // made by the loader; Qt decides which resource schemes it supports.
      resolved += match.captured();
      continue;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
      return fail(error, ThemeApplication::tr(
          "A stylesheet image is missing or unreadable. The theme was not changed."));
    path = QDir::fromNativeSeparators(QDir::cleanPath(path));
    path.replace('"', QStringLiteral("\\\""));
    resolved += QStringLiteral("url(\"") + path + QStringLiteral("\")");
  }
  resolved += style.mid(position);
  style = resolved;
  return true;
}

class QuickThemeMenu final : public QMenu {
public:
  QuickThemeMenu(ThemeApplication &themes, QWidget *parent)
      : QMenu(parent), m_themes(themes) {
    setObjectName(QStringLiteral("QuickThemeMenu"));
    setTitle(ThemeApplication::tr("Theme"));
    connect(this, &QMenu::aboutToShow, this, [this]() { refresh(); });
    connect(&themes, &ThemeApplication::themeChanged, this,
            [this]() { refresh(); });
    refresh();
  }

protected:
  void changeEvent(QEvent *event) override {
    QMenu::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
      setTitle(ThemeApplication::tr("Theme"));
      refresh();
    }
  }

private:
  void refresh() {
    QStringList names = m_themes.availableThemes();
    const QString current = m_themes.currentTheme();
    if (!current.isEmpty() && !names.contains(current)) names.prepend(current);
    // Avoid Qt 5's QList equality dependency on removed MSVC stdext iterators.
    const bool unchanged =
        names.size() == m_names.size() &&
        std::equal(names.cbegin(), names.cend(), m_names.cbegin());
    if (!unchanged || actions().isEmpty()) {
      clear();
      delete m_group;
      m_group = new QActionGroup(this);
      m_group->setExclusive(true);
      m_names = names;
      for (const QString &name : names) {
        QAction *action = addAction(name);
        action->setData(name);
        action->setCheckable(true);
        m_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, name]() {
          QString error;
          if (!m_themes.applyTheme(name, &error)) {
            synchronize();
            QMessageBox::warning(this, ThemeApplication::tr("Theme"), error);
          }
        });
      }
      if (names.isEmpty()) {
        QAction *empty = addAction(ThemeApplication::tr("No installed themes"));
        empty->setEnabled(false);
      }
    }
    const QStringList installed = m_themes.availableThemes();
    for (QAction *action : actions())
      action->setEnabled(installed.contains(action->data().toString()));
    synchronize();
  }

  void synchronize() {
    for (QAction *action : actions()) {
      const QSignalBlocker blocker(action);
      action->setChecked(action->data().toString() == m_themes.currentTheme());
    }
  }

  ThemeApplication &m_themes;
  QStringList m_names;
  QActionGroup *m_group = nullptr;
};
}  // namespace

ThemeApplication::ThemeApplication(const QString &directory, Settings settings,
                                   QObject *parent)
    : QObject(parent), m_directory(directory), m_settings(std::move(settings)) {}

QStringList ThemeApplication::availableThemes() const {
  QStringList names;
  const QDir directory(m_directory);
  for (const QString &name : directory.entryList(
           QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
    const QFileInfo file(directory.filePath(name + '/' + name + ".qss"));
    if (file.isFile() && file.isReadable()) names.append(name);
  }
  return names;
}

QString ThemeApplication::currentTheme() const {
  return m_settings.theme ? m_settings.theme() : QString();
}

bool ThemeApplication::applyTheme(const QString &name, QString *error) {
  return apply(name, m_settings.additionalStyle ? m_settings.additionalStyle()
                                              : QString(), false, false, error);
}

bool ThemeApplication::applyAdditionalStyle(const QString &style, QString *error) {
  return apply(currentTheme(), style, true, false, error);
}

bool ThemeApplication::reload(QString *error) {
  return apply(currentTheme(), m_settings.additionalStyle ? m_settings.additionalStyle()
                                                        : QString(), false, true, error);
}

bool ThemeApplication::apply(const QString &name, const QString &additional,
                             bool saveAdditional, bool force, QString *error) {
  if (error) error->clear();
  if (!qApp || QThread::currentThread() != qApp->thread() || m_applying)
    return fail(error, tr("The theme cannot be changed during another UI update."));
  if (name.isEmpty() || name == "." || name == ".." || name.contains('/') ||
      name.contains('\\') || !availableThemes().contains(name))
    return fail(error, tr("The selected theme is missing or unreadable."));
  const QDir directory(QDir(m_directory).filePath(name));
  QFile file(directory.filePath(name + ".qss"));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return fail(error, tr("The selected theme is missing or unreadable."));
  const QByteArray bytes = file.readAll();
  if (file.error() != QFile::NoError)
    return fail(error, tr("The selected theme could not be read."));
  QString style = QString::fromUtf8(bytes);
  if (!completeStyleSheet(style))
    return fail(error, tr("The selected theme contains an incomplete style rule."));
  if (!completeStyleSheet(additional, true))
    return fail(error, tr("The additional style sheet contains an incomplete style rule."));
  style += '\n' + additional;
  if (!resolveAssets(style, directory.absolutePath(), error)) return false;

  const QScopedValueRollback<bool> guard(m_applying, true);
  const bool repaint = force || !m_initialized || m_appliedTheme != name ||
                       m_appliedStyle != style;
  if (repaint) {
    if (m_settings.refreshIcons) m_settings.refreshIcons(style);
    qApp->setStyleSheet(style);
    // QWidget::setStyleSheet repolishes all children. Repaint every window too:
    // icon engines can have changed even if its stylesheet text is unchanged.
    for (QWidget *window : QApplication::topLevelWidgets()) {
      window->update();
      for (QWidget *child : window->findChildren<QWidget *>()) child->update();
    }
    m_appliedTheme = name;
    m_appliedStyle = style;
    m_initialized = true;
  }
  if (m_settings.saveTheme && currentTheme() != name) m_settings.saveTheme(name);
  if (saveAdditional && m_settings.saveAdditionalStyle &&
      (!m_settings.additionalStyle || m_settings.additionalStyle() != additional))
    m_settings.saveAdditionalStyle(additional);
  if (repaint) emit themeChanged(name);
  return true;
}

void ThemeApplication::synchronizeComboBox(QComboBox *combo) const {
  const QSignalBlocker blocker(combo);
  QStringList names = availableThemes();
  const QString current = currentTheme();
  if (!current.isEmpty() && !names.contains(current)) names.prepend(current);
  QStringList previous;
  for (int i = 0; i < combo->count(); ++i)
    previous.append(combo->itemData(i).toString());
  if (names != previous) {
    combo->clear();
    for (const QString &name : names) combo->addItem(name, name);
  }
  combo->setCurrentIndex(combo->findData(current));
}

void ThemeApplication::bindComboBox(QComboBox *combo) {
  if (!combo) return;
  if (!combo->property("quickThemeBound").toBool()) {
    combo->setProperty("quickThemeBound", true);
    combo->installEventFilter(this);
    connect(this, &ThemeApplication::themeChanged, combo,
            [this, combo]() { synchronizeComboBox(combo); });
  }
  synchronizeComboBox(combo);
}

bool ThemeApplication::eventFilter(QObject *object, QEvent *event) {
  if (event->type() == QEvent::Show || event->type() == QEvent::FocusIn)
    if (QComboBox *combo = qobject_cast<QComboBox *>(object)) synchronizeComboBox(combo);
  return QObject::eventFilter(object, event);
}

QMenu *createQuickThemeMenu(ThemeApplication &themes, QWidget *parent) {
  return new QuickThemeMenu(themes, parent);
}
