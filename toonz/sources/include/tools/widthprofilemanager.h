#pragma once

#include "tools/widthprofile.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QDoubleSpinBox>

namespace WidthProfile {

// The manager uses the same textual interchange as existing user profiles.
// A malformed import is rejected as a whole instead of silently dropping rows.
inline bool readLibrary(const QString &path, std::vector<Profile> &out,
                        QByteArray &bytes, QString &error) {
  out.clear();
  bytes.clear();
  QFile file(path);
  if (!file.exists()) return true;
  if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
  if (file.size() > 4 * 1024 * 1024) {
    error = QObject::tr("The profile file is larger than 4 MiB.");
    return false;
  }
  bytes = file.readAll();
  if (file.error() != QFile::NoError) { error = file.errorString(); return false; }
  int lineNumber = 0;
  for (const QByteArray &line : bytes.split('\n')) {
    ++lineNumber;
    if (line.trimmed().isEmpty() || line.trimmed().startsWith('#')) continue;
    Profile profile;
    if (!fromText(line.toStdString(), profile) || profile.points.size() > 4096 ||
        profile.name.size() > 1024) {
      error = QObject::tr("Invalid width profile on line %1.").arg(lineNumber);
      out.clear();
      return false;
    }
    const auto duplicate = std::find_if(out.begin(), out.end(),
        [&](const Profile &p) { return p.name == profile.name; });
    if (duplicate != out.end()) {
      error = QObject::tr("Duplicate profile name on line %1.").arg(lineNumber);
      out.clear();
      return false;
    }
    out.push_back(profile);
  }
  return true;
}

inline bool writeLibrary(const QString &path, const std::vector<Profile> &profiles,
                         QString &error) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    error = QObject::tr("The profile folder could not be created.");
    return false;
  }
  QSaveFile file(path);
  // Never fall back to truncating the previous library in place.
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) { error = file.errorString(); return false; }
  const std::string text = "# Width profiles: name TAB position:ratio pairs\n" +
                           formatProfiles(profiles);
  if (file.write(text.data(), qint64(text.size())) != qint64(text.size()) ||
      !file.commit()) { error = file.errorString(); return false; }
  return true;
}

class ProfilePreview final : public QWidget {
  Profile m_profile;
public:
  explicit ProfilePreview(QWidget *parent = nullptr) : QWidget(parent) {
    setMinimumSize(280, 140);
    setAccessibleName(tr("Width profile preview"));
  }
  void setProfile(const Profile &profile) { m_profile = profile; update(); }
protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), palette().base());
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_profile.empty()) return;
    const QRectF area = rect().adjusted(16, 16, -16, -16);
    QPainterPath shape;
    for (int side = 0; side < 2; ++side)
      for (int i = 0; i <= 128; ++i) {
        const double t = double(side ? 128 - i : i) / 128;
        const QPointF point(area.left() + t * area.width(),
            area.center().y() + (side ? 1 : -1) *
                evaluate(m_profile, t) * area.height() * .4);
        if (!side && !i) shape.moveTo(point); else shape.lineTo(point);
      }
    shape.closeSubpath();
    painter.fillPath(shape, palette().text());
  }
};

class Manager final : public QDialog {
  QString m_path;
  QByteArray m_original;
  std::vector<Profile> m_profiles;
  Profile m_captured;
  QListWidget *m_list;
  ProfilePreview *m_preview;
  QDoubleSpinBox *m_strength;
  QComboBox *m_mode;
  bool m_apply = false;
  bool m_loaded = false;
  bool m_dirty = false;

  void updatePreview() {
    if (index() < 0) { m_preview->setProfile(Profile()); return; }
    Profile preview = m_profiles[index()];
    {
      const Profile baseline = m_captured.empty() ? builtIn(UNIFORM) : m_captured;
      std::vector<double> positions, widths;
      for (int i = 0; i <= 128; ++i) {
        const double t = double(i) / 128;
        positions.push_back(t); widths.push_back(evaluate(baseline, t));
      }
      auto result = applyBlended(preview, positions, widths, strength(), multiply());
      preview.points.clear();
      for (size_t i = 0; i < result.size(); ++i)
        preview.points.emplace_back(positions[i], result[i]);
    }
    m_preview->setProfile(preview);
  }

  void error(const QString &message) {
    QMessageBox::warning(this, tr("Width Profiles"), message);
  }
  int index() const { return m_list->currentRow(); }
  QString askName(const QString &initial) {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Profile name"), tr("Name:"),
        QLineEdit::Normal, initial, &ok).trimmed();
    if (!ok) return QString();
    if (name.isEmpty() || name.startsWith('#') || name.size() > 64 ||
        name.contains('\t') || name.contains('\n') || name.contains('\r')) {
      error(tr("Use a name of 1–64 characters without tabs or line breaks, "
               "not starting with #."));
      return QString();
    }
    return name;
  }
  bool confirmReplace(const std::string &name) {
    return QMessageBox::question(this, tr("Replace profile"),
        tr("Replace the saved profile “%1”?").arg(QString::fromStdString(name)),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) ==
        QMessageBox::Yes;
  }
  void refresh(int selected) {
    m_list->clear();
    for (const auto &profile : m_profiles)
      m_list->addItem(QString::fromStdString(profile.name));
    m_list->setCurrentRow(std::min(selected, int(m_profiles.size()) - 1));
    updatePreview();
  }
  void add(Profile profile) {
    QString name = askName(QString::fromStdString(profile.name));
    if (name.isEmpty()) return;
    profile.name = name.toStdString();
    auto found = std::find_if(m_profiles.begin(), m_profiles.end(),
        [&](const Profile &p) { return p.name == profile.name; });
    if (found != m_profiles.end() && !confirmReplace(profile.name)) return;
    const int selected = found == m_profiles.end() ? int(m_profiles.size())
                                                  : int(found - m_profiles.begin());
    addOrReplace(m_profiles, profile);
    m_dirty = true;
    refresh(selected);
  }
  void renameProfile() {
    if (index() < 0) return;
    const int row = index();
    QString name = askName(QString::fromStdString(m_profiles[row].name));
    if (name.isEmpty() || name.toStdString() == m_profiles[row].name) return;
    for (int i = 0; i < int(m_profiles.size()); ++i)
      if (i != row && m_profiles[i].name == name.toStdString()) {
        error(tr("That name is already used. Choose another name.")); return;
      }
    m_profiles[row].name = name.toStdString();
    m_dirty = true;
    refresh(row);
  }
  void importProfiles() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Width Profiles"),
        QString(), tr("Width profiles (*.txt);;All files (*)"));
    if (path.isEmpty()) return;
    std::vector<Profile> incoming;
    QByteArray bytes; QString reason;
    if (!QFileInfo::exists(path) || !readLibrary(path, incoming, bytes, reason)) {
      error(reason.isEmpty() ? tr("The file no longer exists.") : reason); return;
    }
    if (incoming.empty()) { error(tr("The file contains no profiles.")); return; }
    auto draft = m_profiles;
    for (const auto &profile : incoming) {
      const auto found = std::find_if(draft.begin(), draft.end(),
          [&](const Profile &p) { return p.name == profile.name; });
      if (found != draft.end() && !confirmReplace(profile.name)) return;
      addOrReplace(draft, profile);
    }
    m_profiles.swap(draft);
    m_dirty = true;
    refresh(0);
  }
  void commit(bool apply) {
    if (!m_loaded || (apply && index() < 0)) return;
    if (m_dirty) {
      if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        error(tr("The profile folder could not be created.")); return;
      }
      QLockFile lock(m_path + ".lock");
      if (!lock.tryLock(0)) {
        error(tr("The profile library is being saved by another window. Retry after it finishes.")); return;
      }
      std::vector<Profile> current; QByteArray bytes; QString reason;
      if (!readLibrary(m_path, current, bytes, reason) || bytes != m_original) {
        error(reason.isEmpty() ? tr("The profile library changed in another window. "
                                    "Cancel and reopen before saving.") : reason);
        return;
      }
      if (!writeLibrary(m_path, m_profiles, reason)) { error(reason); return; }
    }
    m_apply = apply;
    accept();
  }
public:
  Manager(const QString &path, const Profile &captured, bool canApply,
          QWidget *parent = nullptr)
      : QDialog(parent), m_path(path), m_captured(captured) {
    setWindowTitle(tr("Width Profiles"));
    setObjectName("widthProfileManager");
    resize(620, 380);
    auto *layout = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;
    m_list = new QListWidget(this); m_list->setObjectName("widthProfileList");
    m_preview = new ProfilePreview(this);
    body->addWidget(m_list); body->addWidget(m_preview, 1); layout->addLayout(body);
    auto *settings = new QHBoxLayout;
    settings->addWidget(new QLabel(tr("Apply:"), this));
    m_mode = new QComboBox(this);
    m_mode->addItems({tr("Replace pressure"), tr("Multiply pressure")});
    m_mode->setObjectName("widthProfileBlendMode");
    settings->addWidget(m_mode);
    settings->addWidget(new QLabel(tr("Strength:"), this));
    m_strength = new QDoubleSpinBox(this);
    m_strength->setObjectName("widthProfileStrength");
    m_strength->setRange(0, 100); m_strength->setValue(100); m_strength->setSuffix("%");
    settings->addWidget(m_strength); layout->addLayout(settings);
    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] { updatePreview(); });
    connect(m_strength, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this] { updatePreview(); });
    auto *actions = new QHBoxLayout;
    auto button = [&](const QString &text, auto action) {
      auto *b = new QPushButton(text, this); actions->addWidget(b);
      connect(b, &QPushButton::clicked, this, action); return b;
    };
    button(tr("New…"), [this] {
      QStringList choices;
      if (!m_captured.empty()) choices << tr("Selected stroke");
      for (int i = 0; i < BUILTIN_COUNT; ++i) choices << tr(builtInName(i));
      bool ok = false;
      const QString choice = QInputDialog::getItem(this, tr("New Width Profile"),
          tr("Start from:"), choices, 0, false, &ok);
      if (!ok) return;
      const int row = choices.indexOf(choice);
      add(!m_captured.empty() && row == 0 ? m_captured
          : builtIn(row - (m_captured.empty() ? 0 : 1)));
    });
    button(tr("Rename…"), [this] { renameProfile(); });
    button(tr("Delete…"), [this] {
      const int row = index(); if (row < 0) return;
      if (QMessageBox::question(this, tr("Delete Width Profile"),
          tr("Delete “%1” from this library?").arg(m_list->currentItem()->text()),
          QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
      m_profiles.erase(m_profiles.begin() + row); m_dirty = true; refresh(row);
    });
    button(tr("Import…"), [this] { importProfiles(); });
    button(tr("Export…"), [this] {
      if (index() < 0) return;
      const QString path = QFileDialog::getSaveFileName(this, tr("Export Selected Width Profile"),
          QString(), tr("Width profiles (*.txt)"));
      if (path.isEmpty()) return;
      const QFileInfo destination(path), library(m_path);
      if (destination.absoluteFilePath() == library.absoluteFilePath() ||
          (!destination.canonicalFilePath().isEmpty() &&
           destination.canonicalFilePath() == library.canonicalFilePath())) {
        error(tr("Use Save Library to update the library itself.")); return;
      }
      QString reason;
      if (!writeLibrary(path, {m_profiles[index()]}, reason)) error(reason);
    });
    layout->addLayout(actions);
    layout->addWidget(new QLabel(m_captured.empty()
        ? tr("Preview uses a uniform reference stroke; each selected stroke keeps its own pressure when multiplied.")
        : tr("Preview uses the selected stroke's pressure."), this));
    layout->addWidget(new QLabel(tr("Preview changes no artwork. Library edits are saved "
        "only with Save Library or Save and Apply. Export writes the selected profile."), this));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto *save = buttons->addButton(tr("Save Library"), QDialogButtonBox::AcceptRole);
    auto *apply = buttons->addButton(tr("Save and Apply"), QDialogButtonBox::ActionRole);
    apply->setEnabled(canApply);
    connect(save, &QPushButton::clicked, this, [this] { commit(false); });
    connect(apply, &QPushButton::clicked, this, [this] { commit(true); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) { updatePreview(); });
    QString reason;
    m_loaded = readLibrary(path, m_profiles, m_original, reason);
    if (!m_loaded) {
      auto *warning = new QLabel(tr("Cannot read the library: %1").arg(reason), this);
      warning->setWordWrap(true); layout->addWidget(warning);
      save->setEnabled(false); apply->setEnabled(false);
    }
    refresh(0);
  }
  bool shouldApply() const { return m_apply; }
  double strength() const { return .01 * m_strength->value(); }
  bool multiply() const { return m_mode->currentIndex() == 1; }
  Profile selectedProfile() const { return index() < 0 ? Profile() : m_profiles[index()]; }
  void captureNewProfile() { if (!m_captured.empty()) add(m_captured); }
};
}  // namespace WidthProfile
