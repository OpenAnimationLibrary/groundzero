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

class Journal;
struct JournalOperation;

class Dispatcher final {
public:
  explicit Dispatcher(Context& context);
  QJsonObject dispatch(const QJsonObject& request);
  // Appends the terminal record of every job of this worker that ended since
  // its last record. Runs on the application thread: before each request, on
  // a one-second timer, and at teardown.
  void recordJobOutcomes();
  // Between requests, once a second: job outcomes, then one bounded step of
  // journal retention.
  void idle();

private:
#ifdef BUILD_UI_PORT_TESTS
  friend void runAutomationDispatcherTests(Context& context);
  friend struct DispatcherProbe;
#endif
  using Handler = QJsonValue (*)(Context&, const QString&, const QJsonObject&);
  struct Entry {
    QJsonObject descriptor;
    Handler handler;
  };
  // fingerprint is the SHA-256 (hex) of the request and the launch
  // authority; request is that of {method, params} alone.
  struct Receipt {
    QByteArray fingerprint;
    QByteArray request;
    QJsonObject response;
  };
  Receipt receiptKey(const QString& method, const QJsonObject& params) const;
  void add(const QJsonArray& operations, Handler handler,
           const QString& toolset);
  void addWorkerOperation(QJsonObject descriptor);
  QJsonArray toolsetCatalog() const;
  QJsonObject capabilities(const QJsonObject& params) const;
  void checkReceiptCapacity(bool control) const;
  void rememberReceipt(const QString& operationId, const Receipt& receipt,
                       const QString& controlJob);
  QJsonObject replayReceipt(const Receipt& receipt, const Receipt& key) const;
  quint64 checkRevision(const QJsonObject& params) const;
  QJsonValue execute(const QString& method, const QJsonObject& params);
  QJsonValue invoke(const Entry& entry, const QString& method,
                    const QJsonObject& params);
  QJsonObject executeWrite(const Entry& entry, const QString& method,
                           const QJsonObject& params);
  QJsonObject applyWrite(const Entry& entry, const QString& method,
                         const QJsonObject& params, quint64 revision);
  bool admit(Journal& journal, const QString& method, const QJsonObject& params,
             const Receipt& key, const QString& writeClass);
  void recordOutcome(Journal* journal, const QString& operationId,
                     const QJsonObject& response);
  void recordJobStart(Journal* journal, const QString& operationId,
                      const QJsonObject& result);
  void recordJobEnd(Journal& journal, const QString& id,
                    const QJsonObject& record);
  QJsonObject replayJournal(Journal& journal, const JournalOperation& operation,
                            const Receipt& key);
  QJsonObject recordedResponse(Journal& journal,
                               const JournalOperation& operation);
  void finishRevision(QJsonObject& result, const QString& documentId,
                      quint64 revision);
  Context& m_context;
  QString m_authorityDigest;
  QString m_instance;
  QHash<QString, Entry> m_entries;
  QHash<QString, Receipt> m_receipts;
  QSet<QString> m_controlJobs;
  // Jobs whose terminal record is in the journal.
  QSet<QString> m_journaledJobs;
  qint64 m_receiptBytes = 0;
  QJsonArray m_catalog;
  QMap<QString, QStringList> m_toolsets;
  bool m_busy = false;
};

// The toolsets of this binary, sorted by name.
QStringList toolsetNames();
QJsonObject errorObject(const Error& error);
// Throw invalid_result when a result, without its image, is outside the
// success shape of the tool's outputSchema. Test builds check every result.
void checkResult(const QJsonObject& descriptor, const QJsonValue& result);
#ifdef BUILD_UI_PORT_TESTS
void runAutomationDispatcherTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
