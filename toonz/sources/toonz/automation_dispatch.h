#pragma once

#include "automation.h"

#include <QSet>

namespace GroundzeroAutomation {

class Dispatcher final {
public:
  explicit Dispatcher(Context& context);
  QJsonObject dispatch(const QJsonObject& request);

private:
#ifdef BUILD_UI_PORT_TESTS
  friend void runAutomationDispatcherTests(Context& context);
#endif
  using Handler = QJsonValue (*)(Context&, const QString&, const QJsonObject&);
  struct Entry {
    QJsonObject descriptor;
    Handler handler;
  };
  struct Receipt {
    QByteArray fingerprint;
    QJsonObject response;
  };
  void add(const QJsonArray& operations, Handler handler);
  void checkReceiptCapacity(bool control) const;
  void rememberReceipt(const QString& operationId, const Receipt& receipt,
                       const QString& controlJob);
  QJsonObject replayReceipt(const Receipt& receipt,
                            const QByteArray& fingerprint) const;
  quint64 checkRevision(const QJsonObject& params) const;
  QJsonValue execute(const QString& method, const QJsonObject& params);
  QJsonObject executeWrite(const Entry& entry, const QString& method,
                           const QJsonObject& params);
  QJsonObject applyWrite(const Entry& entry, const QString& method,
                         const QJsonObject& params);
  void finishRevision(QJsonObject& result, const QString& documentId,
                      quint64 revision);
  Context& m_context;
  QHash<QString, Entry> m_entries;
  QHash<QString, Receipt> m_receipts;
  QSet<QString> m_controlJobs;
  qint64 m_receiptBytes = 0;
  QJsonArray m_catalog;
  bool m_busy = false;
};

QJsonObject errorObject(const Error& error);
#ifdef BUILD_UI_PORT_TESTS
void runAutomationDispatcherTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
