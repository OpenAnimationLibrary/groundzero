#include "mediacachepopup.h"
#include "toonz/mediacache.h"
#include "toonz/toonzfolders.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>
#include <memory>

namespace {
class MediaCacheDialog final : public QDialog {
  Q_DECLARE_TR_FUNCTIONS(MediaCacheDialog)
  QString m_root;
  QLabel *m_usage, *m_result;
  QSpinBox *m_budget;
  QPushButton *m_refresh, *m_apply, *m_clear, *m_cancel;
  QThread *m_worker = nullptr;
  std::atomic<bool> m_stop{false};
  bool m_closeWhenFinished = false;
  static QString size(qint64 bytes) {
    return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 1) + " MiB";
  }
  void busy(bool value) {
    QWidget *widgets[] = {m_budget, m_refresh, m_apply, m_clear};
    for (QWidget *widget : widgets) widget->setEnabled(!value);
    m_cancel->setEnabled(value);
  }
  void start(int operation) {
    if (m_worker) return;
    const qint64 target = qint64(m_budget->value()) * 1024 * 1024;
    m_stop.store(false);
    busy(true);
    m_result->setText(tr("Inspecting cached media…"));
    auto result = std::make_shared<MediaCache::Report>();
    m_worker = QThread::create([this, operation, target, result]() {
      if (operation == 1) {
        QString error;
        if (!MediaCache::setBudget(m_root, target, &error)) {
          result->errors.append(error);
          return;
        }
      }
      *result = operation == 2 ? MediaCache::trim(m_root, 0, &m_stop)
          : operation == 1 && target > 0 ? MediaCache::trim(m_root, target, &m_stop)
                                        : MediaCache::usage(m_root, &m_stop);
    });
    connect(m_worker, &QThread::finished, this, [this, result, operation]() {
      m_worker->deleteLater();
      m_worker = nullptr;
      busy(false);
      m_usage->setText(tr("Cached media: %1. In use: %2 (%3 generations).")
          .arg(size(result->bytes)).arg(size(result->activeBytes))
          .arg(result->activeGenerations));
      if (result->canceled)
        m_usage->setText(tr("Inspection canceled; totals are incomplete. Refresh to recalculate."));
      else if (!result->errors.isEmpty())
        m_usage->setText(tr("Cache inspection was incomplete. Review the errors and refresh."));
      QString message = result->canceled ? tr("Canceled. Completed removals are retained.")
          : operation == 0 ? tr("Cache inspection completed.")
          : tr("Removed %1 unused generations (%2).")
                .arg(result->removedGenerations).arg(size(result->removedBytes));
      if (!result->errors.isEmpty()) message += "\n" + result->errors.join('\n');
      if (operation == 1 && result->bytes > qint64(m_budget->value()) * 1024 * 1024 &&
          m_budget->value() > 0)
        message += "\n" + tr("The budget could not be reached. Media still in use is protected.");
      m_result->setText(message);
      if (m_closeWhenFinished) QDialog::reject();
    });
    m_worker->start();
  }
public:
  explicit MediaCacheDialog(QWidget *parent) : QDialog(parent) {
    setObjectName("mediaCacheManager");
    setWindowTitle(tr("Manage Media Cache"));
    m_root = QDir(ToonzFolder::getCacheRootFolder().getQString()).filePath("ffmpeg");
    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(tr("Cached video frames can be recreated from their source movies. "
        "Open readers in this or another current Groundzero session are protected."), this);
    description->setWordWrap(true);
    layout->addWidget(description);
    m_usage = new QLabel(this);
    m_usage->setObjectName("mediaCacheUsage");
    m_usage->setWordWrap(true);
    m_usage->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_usage);
    auto *form = new QFormLayout;
    m_budget = new QSpinBox(this);
    m_budget->setObjectName("mediaCacheBudgetMiB");
    m_budget->setRange(0, 1024 * 1024);
    m_budget->setSpecialValueText(tr("No automatic cleanup"));
    m_budget->setSuffix(tr(" MiB"));
    m_budget->setValue(int(qMin<qint64>(1024 * 1024,
                                     MediaCache::budget(m_root) / (1024 * 1024))));
    form->addRow(tr("Cache &budget:"), m_budget);
    m_budget->setAccessibleName(tr("Media cache budget in MiB"));
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(this);
    m_refresh = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    m_apply = buttons->addButton(tr("Save Budget and Clean"), QDialogButtonBox::ActionRole);
    m_clear = buttons->addButton(tr("Remove Unused Media"), QDialogButtonBox::ActionRole);
    m_cancel = buttons->addButton(tr("Cancel Operation"), QDialogButtonBox::ActionRole);
    m_refresh->setObjectName("mediaCacheRefresh");
    m_apply->setObjectName("mediaCacheApply");
    m_clear->setObjectName("mediaCacheClear");
    m_cancel->setObjectName("mediaCacheCancel");
    buttons->addButton(QDialogButtonBox::Close);
    connect(m_refresh, &QPushButton::clicked, this, [this]() { start(0); });
    connect(m_apply, &QPushButton::clicked, this, [this]() { start(1); });
    connect(m_clear, &QPushButton::clicked, this, [this]() {
      if (QMessageBox::question(this, tr("Remove Unused Media"),
          tr("Remove unused cached video generations? Source movies and open readers are retained."),
          QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Ok)
        start(2);
    });
    connect(m_cancel, &QPushButton::clicked, this, [this]() { m_stop.store(true); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    m_result = new QLabel(this);
    m_result->setObjectName("mediaCacheResult");
    m_result->setWordWrap(true);
    m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_result);
    resize(620, 240);
    start(0);
  }
  ~MediaCacheDialog() override {
    m_stop.store(true);
    if (m_worker) { m_worker->wait(); delete m_worker; }
  }
  void reject() override {
    if (m_worker) { m_closeWhenFinished = true; m_stop.store(true); }
    else QDialog::reject();
  }
};
}

void showMediaCacheManager(QWidget *parent) {
  MediaCacheDialog dialog(parent);
  dialog.exec();
}
