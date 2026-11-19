#pragma once

#include "automation.h"

#include <QMap>
#include <QSet>
#include <QStringList>

namespace GroundzeroAutomation {

// Every tool's input schema is a plain object at its root: type "object" and no
// oneOf, anyOf, allOf, not or $ref there. Several clients and model APIs accept
// nothing else, so a native operation that breaks this cannot be registered.
void requirePlainInputSchema(const QJsonObject& descriptor);

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
  void add(const QJsonArray& operations, Handler handler,
           const QString& toolset);
  void addWorkerOperation(QJsonObject descriptor);
  QJsonArray toolsetCatalog() const;
  QJsonObject capabilities(const QJsonObject& params) const;
  void checkReceiptCapacity(bool control) const;
  void rememberReceipt(const QString& operationId, const Receipt& receipt,
                       const QString& controlJob);
  QJsonObject replayReceipt(const Receipt& receipt,
                            const QByteArray& fingerprint) const;
  quint64 checkRevision(const QJsonObject& params) const;
  QJsonValue execute(const QString& method, const QJsonObject& params);
  QJsonValue invoke(const Entry& entry, const QString& method,
                    const QJsonObject& params);
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
  QMap<QString, QStringList> m_toolsets;
  bool m_busy = false;
};

QJsonObject errorObject(const Error& error);
#ifdef BUILD_UI_PORT_TESTS
void runAutomationDispatcherTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
