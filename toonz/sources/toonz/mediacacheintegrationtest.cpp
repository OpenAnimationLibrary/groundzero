#include "mediacacheintegrationtest.h"
#include "menubarcommandids.h"
#include "toonz/mediacache.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/menubarcommand.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
}
bool runMediaCacheIntegrationTest(QMainWindow *, QString *error) {
  try {
    const QString root = QDir(ToonzFolder::getCacheRootFolder().getQString()).filePath("ffmpeg");
    const QString base = QDir(root).filePath("import-v2-" + QString(64, 'a'));
    require(QDir().mkpath(base + ".frames"), "Cannot create disposable cache data");
    QFile frame(QDir(base + ".frames").filePath("fixture.png"));
    require(frame.open(QIODevice::WriteOnly) && frame.write(QByteArray(4096, 'x')) == 4096,
            "Cannot write disposable cache data");
    frame.close();
    MediaCache::Lease active(base);
    auto *action = CommandManager::instance()->getAction(MI_ManageMediaCache);
    require(action && action->isEnabled(), "Media cache action is unavailable");
    for (int pass = 0; pass < 2; ++pass) {
      int stage = 0;
      QString failure;
      QElapsedTimer elapsed;
      elapsed.start();
      QTimer driver;
      driver.setInterval(10);
      QObject::connect(&driver, &QTimer::timeout, [&]() {
        try {
          require(elapsed.elapsed() < 15000, "Native cache dialog did not complete its operation");
          if (auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            require(stage == 2, "Unexpected cache confirmation");
            // Cancel must leave both cache data and policy untouched.
            confirm->done(QMessageBox::Cancel);
            stage = 3;
            return;
          }
          auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
          if (!dialog || dialog->objectName() != "mediaCacheManager") return;
          auto *refresh = dialog->findChild<QPushButton *>("mediaCacheRefresh");
          auto *apply = dialog->findChild<QPushButton *>("mediaCacheApply");
          auto *clear = dialog->findChild<QPushButton *>("mediaCacheClear");
          auto *budget = dialog->findChild<QSpinBox *>("mediaCacheBudgetMiB");
          auto *usage = dialog->findChild<QLabel *>("mediaCacheUsage");
          require(refresh && apply && clear && budget && usage,
                  "Native cache controls are missing");
          if (!refresh->isEnabled()) return;
          if (stage == 0) {
            require(!usage->text().isEmpty() && !budget->accessibleName().isEmpty(),
                    "Cache usage or accessible budget label is missing");
            if (pass == 1) {
              require(budget->value() == 1, "Cache budget was not restored on reopen");
              dialog->reject(); stage = 4; return;
            }
            budget->setValue(1);
            apply->click(); stage = 1;
          } else if (stage == 1) {
            require(MediaCache::budget(root) == 1024 * 1024,
                    "Native cache Apply did not persist the selected budget");
            stage = 2;
            QTimer::singleShot(0, [&]() {
              auto *confirm = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
              if (!confirm) {
                failure = "Native cache removal did not ask for confirmation";
                if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget())) modal->reject();
                return;
              }
              confirm->done(QMessageBox::Cancel);
              stage = 3;
            });
            clear->click();
          } else if (stage == 3) {
            require(QFileInfo::exists(base + ".frames"), "Canceled native cleanup removed data");
            dialog->reject(); stage = 4;
          }
        } catch (const std::exception &problem) {
          failure = QString::fromUtf8(problem.what());
          driver.stop();
          if (auto *modal = qobject_cast<QDialog *>(QApplication::activeModalWidget())) modal->reject();
          for (auto *widget : QApplication::topLevelWidgets())
            if (widget->objectName() == "mediaCacheManager")
              if (auto *dialog = qobject_cast<QDialog *>(widget)) dialog->reject();
        }
      });
      driver.start();
      action->trigger();
      driver.stop();
      if (!failure.isEmpty()) throw std::runtime_error(failure.toStdString());
      require(stage == 4, "Native cache dialog did not finish the expected workflow");
    }
    QString ignored;
    require(MediaCache::setBudget(root, 0, &ignored), "Cannot restore disposable cache policy");
    return true;
  } catch (const std::exception &problem) {
    if (error) *error = QString::fromUtf8(problem.what());
    return false;
  }
}
