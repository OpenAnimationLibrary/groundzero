#include "toonzqt/tmessageviewer.h"
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QListView>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QThread>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void events() { QCoreApplication::processEvents(QEventLoop::AllEvents, 30); }
void run() {
  auto *repository = TMessageRepository::instance();
  repository->clear();
  repository->setHistoryLimit(3);
  int records = 0;
  bool guiThread = true;
  QObject::connect(repository, &TMessageRepository::recordAdded, repository,
      [&](quint64, const QDateTime &, int, const QString &) {
        ++records;
        guiThread &= QThread::currentThread() == qApp->thread();
      });
  TMessageViewer first(nullptr), second(nullptr);
  first.resize(600, 240); second.resize(600, 240);
  first.show(); second.show(); events();
  for (int index = 0; index < 5; ++index)
    repository->messageReceived(DVGui::INFORMATION, QString("event-%1").arg(index));
  require(records == 5, "two views do not duplicate ingestion");
  require(repository->getModel()->rowCount() == 3 && repository->discardedCount() == 2,
          "history cap retains only newest events with discard count");
  const QModelIndex oldest = repository->getModel()->index(0, 0);
  require(oldest.data(TMessageRepository::MessageRole).toString() == "event-2", "eviction removes oldest rows");
  const QDateTime time = oldest.data(TMessageRepository::TimestampRole).toDateTime();
  require(time.isValid() && oldest.data(TMessageRepository::SequenceRole).toULongLong() > 0,
          "timestamp and sequence attached to record");
  auto *list = first.findChild<QListView *>("MessageHistory");
  require(list, "native history view exists");
  first.m_greenCheck->setChecked(false); events();
  require(list->model()->rowCount() == 0, "existing info filter works with metadata");
  repository->getModel()->item(0)->setIcon(QIcon());
  events();
  require(list->model()->rowCount() == 0, "icon replacement cannot change severity classification");
  first.m_greenCheck->setChecked(true);
  require(oldest.data(TMessageRepository::TimestampRole).toDateTime() == time,
          "filter changes preserve capture time");
  repository->messageReceived(DVGui::WARNING, "warning-visible");
  require(records == 6, "visible Message Center records warning once without popup");
  repository->clear();
  require(records == 6 && repository->discardedCount() == 0, "clear does not replay events or retain discard count");

  first.hide(); second.hide();
  repository->setHistoryLimit(10000);
  for (int index = 0; index < 160; ++index)
    repository->messageReceived(DVGui::INFORMATION, QString(100000, 'x'));
  require(repository->retainedTextBytes() <= 16 * 1024 * 1024,
          "text-volume bound applies before entry-count bound");
  require(repository->discardedCount() > 0, "text-volume overflow evicts old entries");
  require(repository->getModel()->item(0)->data(TMessageRepository::MessageRole).toString().size() < 40000,
          "individual oversized event has bounded history representation");
  repository->clear();
  repository->setHistoryLimit(1000);
  first.show(); second.show();
  for (int index = 0; index < 100; ++index)
    repository->messageReceived(DVGui::INFORMATION, QString("line-%1").arg(index));
  events();
  list->verticalScrollBar()->setValue(0);
  const auto anchor = list->indexAt(QPoint(1, 1)).data(TMessageRepository::SequenceRole);
  repository->messageReceived(DVGui::INFORMATION, "arrived-while-reading"); events();
  require(list->indexAt(QPoint(1, 1)).data(TMessageRepository::SequenceRole) == anchor,
          "reading older history preserves viewport anchor");
  list->scrollToBottom();
  repository->messageReceived(DVGui::INFORMATION, "follow-new-message"); events();
  require(list->verticalScrollBar()->value() == list->verticalScrollBar()->maximum(),
          "view follows when already at end");

  const int beforeWorker = records;
  std::thread worker([repository] {
    repository->messageReceived(DVGui::INFORMATION, "worker-event");
  });
  worker.join();
  QElapsedTimer timer; timer.start();
  while (records == beforeWorker && timer.elapsed() < 3000) events();
  require(records == beforeWorker + 1 && guiThread, "worker deliveries marshal to GUI thread");
  {
    auto *temporary = new TMessageViewer(nullptr);
    temporary->show(); events();
    delete temporary;
  }
  first.hide(); second.hide(); events();
  require(!TMessageViewer::isTMsgVisible(), "destroyed viewer removed from visibility registry");
  repository->clear();
  QObject::disconnect(repository, nullptr, repository, nullptr);
}
}  // namespace
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication app(argc, argv);
  try { run(); std::cout << "message history passed\n"; return 0; }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
