#include "automation_journal.h"

#include "automation.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>

#include <functional>
#include <memory>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

// A journal of a worker whose runtime folder is `runtime` under the workspace;
// a missing folder makes the worker dead to every other journal.
std::shared_ptr<Journal> openJournal(const QString& workspace,
                                     const QString& instance,
                                     const QString& runtime) {
  Journal::Start start;
  start.workspace       = workspace;
  start.instance        = instance;
  start.runtimeName     = runtime;
  start.authorityDigest = "authority";
  return Journal::open(start);
}

QJsonObject admission(const QString& id, const QString& effectClass) {
  return {{"operation_id", id},
          {"arguments_sha256", QString(64, 'a')},
          {"authority_digest", "authority"},
          {"method", "scene.create"},
          {"document_id", QJsonValue()},
          {"expected_revision", QJsonValue()},
          {"effect_class", effectClass}};
}

QString segmentPath(const QString& workspace) {
  const QDir journal(workspace + "/.groundzero/journal");
  const QStringList segments = journal.entryList({"seg-*.jsonl"}, QDir::Files);
  require(!segments.isEmpty(), "the journal has no segment");
  return journal.filePath(segments.last());
}

Error refusal(const std::function<void()>& call) {
  try {
    call();
  } catch (const Error& error) {
    return error;
  }
  throw std::runtime_error("a journal call was not refused");
}

void twoWritersShareOneSequence(const QString& workspace) {
  const QString runtime = "runtime_" + uuid();
  require(QDir(workspace).mkdir(runtime), "cannot create a runtime folder");
  auto first  = openJournal(workspace, uuid(), runtime);
  auto second = openJournal(workspace, uuid(), runtime);
  require(first->available() && second->available(), "journals unavailable");
  require(first->admit(admission("op-a", "memory")), "first admission refused");
  require(!second->admit(admission("op-a", "memory")),
          "a second worker admitted an operation already admitted");
  require(second->admit(admission("op-b", "memory")), "op-b refused");
  first->refresh();
  const JournalOperation* a = first->find("op-a");
  const JournalOperation* b = first->find("op-b");
  require(a && b && b->admittedSeq > a->admittedSeq,
          "sequences do not increase across writers");
  require(first->outcome("op-a", {{"result", QJsonObject{{"done", true}}}},
                         {{"code", QJsonValue()}}),
          "outcome not recorded");
  second->refresh();
  require(second->response(*second->find("op-a")).value("result").toObject() ==
              QJsonObject{{"done", true}},
          "the other worker does not read the recorded result");
}

void largeResultsGoToBlobs(const QString& workspace) {
  auto journal = openJournal(workspace, uuid(), "runtime_" + uuid());
  require(journal->admit(admission("op-large", "memory")), "admission refused");
  const QJsonObject response{
      {"result", QJsonObject{{"text", QString(40000, 'x')}}}};
  require(journal->outcome("op-large", response, {}), "large outcome refused");
  const JournalOperation* operation = journal->find("op-large");
  const QString blob = operation->outcome.value("blob").toString();
  require(blob.size() == 64, "a 40 KB result was stored inline");
  require(journal->response(*operation) == response, "the blob differs");
  QFile file(workspace + "/.groundzero/journal/blobs/" + blob);
  require(file.open(QIODevice::ReadWrite) && file.write("y") == 1,
          "cannot alter the blob");
  file.close();
  require(refusal([&] { journal->response(*operation); }).code ==
              "journal_unavailable",
          "an altered blob was returned");
}

void tornTailIsQuarantined(const QString& workspace) {
  {
    auto journal = openJournal(workspace, uuid(), "runtime_" + uuid());
    require(journal->admit(admission("op-whole", "memory")), "refused");
  }
  QFile segment(segmentPath(workspace));
  require(segment.open(QIODevice::Append) && segment.write("abc {\"type\"") > 0,
          "cannot tear the tail");
  segment.close();
  auto journal = openJournal(workspace, uuid(), "runtime_" + uuid());
  const QJsonObject status = journal->status();
  require(status.value("state") == "ok", "a torn tail marked the journal");
  require(status.value("last_recovery")
                  .toObject()
                  .value("truncated_tail_bytes")
                  .toInt() == 11,
          "the torn tail was not cut off");
  require(journal->find("op-whole"), "a whole record was lost with the tail");
  require(!QDir(workspace + "/.groundzero/journal/quarantine")
               .entryList({"tail-*"}, QDir::Files)
               .isEmpty(),
          "the torn tail was not kept in quarantine");
}

void damageIsQuarantinedAndReported(const QString& workspace) {
  {
    auto journal = openJournal(workspace, uuid(), "runtime_" + uuid());
    require(journal->admit(admission("op-1", "memory")), "refused");
    require(journal->admit(admission("op-2", "memory")), "refused");
  }
  QFile segment(segmentPath(workspace));
  require(segment.open(QIODevice::ReadOnly), "cannot read the segment");
  QByteArray bytes = segment.readAll();
  segment.close();
  const int at  = bytes.indexOf("\"op-1\"");
  bytes[at + 2] = 'X';
  require(segment.open(QIODevice::WriteOnly) && segment.write(bytes) > 0,
          "cannot damage the segment");
  segment.close();
  auto journal = openJournal(workspace, uuid(), "runtime_" + uuid());
  require(journal->available() && journal->state() == "damaged",
          "damage was not reported");
  require(!journal->find("op-1") && journal->find("op-2"),
          "the damaged line was read or a good one lost");
  require(journal->status().value("damage").toArray().size() == 1,
          "no damage row");
  require(journal->admit(admission("op-3", "memory")),
          "writes stopped after damage");
}

void newerFormatIsReadOnly(const QString& workspace) {
  openJournal(workspace, uuid(), "runtime_" + uuid());
  QFile format(workspace + "/.groundzero/journal/FORMAT");
  require(format.open(QIODevice::WriteOnly) &&
              format.write(
                  "{\"format\":\"groundzero-journal\",\"version\":9}\n") > 0,
          "cannot write FORMAT");
  format.close();
  auto journal = openJournal(workspace, uuid(), "runtime_" + uuid());
  require(!journal->available() && journal->readable() &&
              journal->unavailableReason() == "newer_format",
          "a newer format was not refused");
  require(refusal([&] { journal->admit(admission("op-x", "memory")); }).code ==
              "journal_unavailable",
          "a newer journal admitted a write");
}

void verdictsFollowTheEffectClass(const QString& workspace) {
  const QString deadRuntime = "runtime_" + uuid();
  {
    auto dead = openJournal(workspace, uuid(), deadRuntime);
    for (const char* kind : {"memory", "job", "publication"})
      require(dead->admit(admission(QString("op-") + kind, kind)), "refused");
    require(dead->admit(admission("op-written", "publication")), "refused");
    QFile target(workspace + "/written.txt");
    require(target.open(QIODevice::WriteOnly) && target.write("data") == 4,
            "cannot write the target");
    target.close();
    const QString digest = journalContentDigest(workspace + "/written.txt");
    dead->intent({{"operation_id", "op-written"},
                  {"target", "written.txt"},
                  {"expected_sha256", digest},
                  {"content_sha256", digest}});
  }
  auto journal    = openJournal(workspace, uuid(), "runtime_" + uuid());
  const auto kind = [&](const char* id) {
    return journal->verdict(*journal->find(id)).value("kind").toString();
  };
  require(kind("op-memory") == "lost_with_worker", "memory verdict");
  require(kind("op-job") == "job_unknown", "job verdict");
  require(kind("op-publication") == "not_published", "publication verdict");
  require(kind("op-written") == "published", "published verdict");
  require(journal->verdict(*journal->find("op-written")).value("reconciled") ==
              true,
          "the start-up recovery did not reconcile a dead worker's operation");
}

void reservedPathsAreRefused(Context& context) {
  for (const QString& path : {QString(".groundzero"), QString(".groundzero/x"),
                              QString(".GROUNDZERO/journal/FORMAT")}) {
    const Error error = refusal([&] { context.resolvePath(path, false); });
    require(error.code == "path_denied" &&
                error.details.value("reason") == "reserved",
            "a reserved path was resolved");
  }
  context.resolvePath(".groundzeroish/x", true);
}

}  // namespace

void runAutomationJournalTests(Context& context) {
  const auto fresh = [&context]() {
    auto directory = std::make_unique<QTemporaryDir>(
        context.scratchDirectory() + "/journal-test-XXXXXX");
    require(directory->isValid(), "cannot create a journal test workspace");
    return directory;
  };
  twoWritersShareOneSequence(fresh()->path());
  largeResultsGoToBlobs(fresh()->path());
  tornTailIsQuarantined(fresh()->path());
  damageIsQuarantinedAndReported(fresh()->path());
  newerFormatIsReadOnly(fresh()->path());
  verdictsFollowTheEffectClass(fresh()->path());
  reservedPathsAreRefused(context);
}

}  // namespace GroundzeroAutomation
