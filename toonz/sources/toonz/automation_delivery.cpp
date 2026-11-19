#include "automation_delivery.h"

#include "automation_build_identity.h"
#include "automation_files.h"
#include "automation_jobs.h"
#include "automation_journal.h"
#include "automation_package.h"
#include "automation_root_publish.h"
#include "automation_runtime.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <memory>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxItems         = 8;
constexpr int MaxFileOffset    = 1100;
constexpr int MaxFilePage      = 256;
constexpr int DefaultFilePage  = 64;
constexpr int OperationIdChars = 128;
// The exact bytes of a delivery are known; the margin covers its folders.
constexpr qint64 SpaceMarginBytes = 4 * 1024 * 1024;
const char* const ManifestName    = "delivery.json";
const char* const FailureCode     = "delivery_write_failed";

const QStringList& checkNames() {
  static const QStringList names{"root",       "path",  "parent",
                                 "collision",  "jobs",  "artifacts",
                                 "filesystem", "space", "journal"};
  return names;
}

QJsonValue orNull(const QJsonValue& value) {
  return value.isUndefined() ? QJsonValue(QJsonValue::Null) : value;
}

QString sha256Text(const QByteArray& bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// One item of a delivery: a completed render job of this worker and the
// folder its files fill.
struct Item final {
  QString jobId;
  QString folder;
  RenderedOutput output;
};

// One file the delivery publishes, in plan order.
struct PlannedFile final {
  QString path;  // relative to the delivery directory
  int item                 = 0;
  const RenderedFile* file = nullptr;
};

// Everything a request names, resolved: the destination, the items and the
// first job refusal (kept for the jobs check, raised by a publication).
struct Plan final {
  PackageTarget target;
  std::vector<Item> items;
  std::vector<PlannedFile> files;
  std::unique_ptr<Error> jobFailure;
  qint64 bytes = 0;
};

// What a delivery never contains (amendment PB-11).
QJsonArray notIncluded() {
  static const QList<QPair<QString, QString>> rows{
      {"render_snapshot", "private_to_job"},
      {"scene_package", "publish_with_scene_save_copy"},
      {"audio", "renders_are_video_only"},
      {"job_records", "in_the_journal"}};
  QJsonArray result;
  for (const auto& row : rows)
    result.append(QJsonObject{{"class", row.first}, {"reason", row.second}});
  return result;
}

// The completed render job `id` of this process, or the refusal that names
// why it cannot be published.
RenderedOutput resolveJob(Context& context, const QString& id) {
  if (observedHostJobs(context).contains(id))
    throw Error("unsupported_target",
                "A read-only document's render job keeps its files in its "
                "scene host; publish a render of an editable document.",
                {{"reason", "scene_host_job"}, {"job_id", id}});
  const std::shared_ptr<Job> job       = retainedJobForResult(context, id);
  std::optional<RenderedOutput> output = renderedOutput(*job);
  if (!output)
    throw Error("unsupported_target", "Only render jobs have files to publish.",
                {{"reason", "not_a_render_job"},
                 {"job_id", id},
                 {"kind", job->record().value("kind")}});
  return *output;
}

void refuseItems(const QString& reason, const QString& value) {
  throw Error("invalid_params",
              "Name each job and each folder once; delivery.json is the "
              "manifest's own name.",
              {{"reason", reason}, {"value", value}});
}

Plan makePlan(Context& context, const QJsonObject& params) {
  Plan plan;
  plan.target = packageTarget(context, params.value("destination").toObject(),
                              "path", true);
  QSet<QString> folders;
  QSet<QString> jobs;
  for (const QJsonValue& value : params.value("items").toArray()) {
    const QJsonObject entry = value.toObject();
    Item item{
        entry.value("job_id").toString(), entry.value("folder").toString(), {}};
    if (item.folder == ManifestName)
      refuseItems("reserved_folder", item.folder);
    if (folders.contains(item.folder.toLower()))
      refuseItems("duplicate_folder", item.folder);
    if (jobs.contains(item.jobId)) refuseItems("duplicate_job", item.jobId);
    folders.insert(item.folder.toLower());
    jobs.insert(item.jobId);
    try {
      item.output = resolveJob(context, item.jobId);
    } catch (const Error& error) {
      if (!plan.jobFailure) plan.jobFailure = std::make_unique<Error>(error);
    }
    plan.items.push_back(std::move(item));
  }
  for (int index = 0; index < int(plan.items.size()); ++index)
    for (const RenderedFile& file : plan.items[index].output.files) {
      plan.files.push_back(
          {plan.items[index].folder + '/' + file.name, index, &file});
      plan.bytes += file.bytes;
    }
  return plan;
}

// Each planned file is still the regular file the render verified, at its
// verified size; the copy compares every byte with its digest.
void checkArtifacts(PreflightChecks& checks, const Plan& plan) {
  if (plan.jobFailure) {
    checks.fail("jobs", plan.jobFailure->code, plan.jobFailure->message);
    checks.unknown("artifacts", "The jobs check failed.");
    return;
  }
  checks.pass("jobs");
  for (const PlannedFile& planned : plan.files) {
    const QFileInfo info(planned.file->path);
    if (info.isFile() && !info.isSymLink() &&
        info.size() == planned.file->bytes)
      continue;
    checks.fail("artifacts", "invalid_artifact",
                "The job file for " + planned.path +
                    " is missing or changed since the render verified it.");
    return;
  }
  checks.pass("artifacts");
}

void checkPlace(PreflightChecks& checks, const DestinationFacts& facts) {
  if (!facts.rootCode.isEmpty())
    return checks.fail("root", facts.rootCode, facts.rootDetail);
  checks.pass("root");
  if (!facts.pathCode.isEmpty())
    return checks.fail("path", facts.pathCode, facts.pathDetail);
  checks.pass("path");
  if (!facts.parentFound)
    return checks.fail("parent", "missing_parent",
                       "Create the delivery's parent directory first.");
  if (facts.writable)
    checks.pass("parent");
  else
    checks.fail("parent", "destination_unwritable",
                "The delivery's parent directory is not writable.");
  if (facts.exists == 1)
    checks.fail("collision", "already_exists",
                "Choose a new delivery directory; existing paths are never "
                "replaced.");
  else if (facts.exists == 0)
    checks.pass("collision");
  else
    checks.unknown("collision", "The destination cannot be looked up.");
}

void checkVolume(PreflightChecks& checks, const DestinationFacts& facts,
                 qint64 bytes) {
  if (facts.rename == "failed_last_attempt")
    checks.fail("filesystem", "publication_unsupported",
                "A publication on this volume was refused: it cannot create "
                "a name without replacing one.");
  else if (facts.rename == "expected")
    checks.pass("filesystem");
  else
    checks.unknown("filesystem",
                   "Whether this file system renames without replacing is "
                   "not known until a publication tries it.");
  if (!facts.parentFound || facts.freeBytes < 0)
    checks.unknown("space", "The free space of the destination is unknown.");
  else if (facts.freeBytes < bytes + SpaceMarginBytes)
    checks.fail("space", "insufficient_space",
                QString("%1 bytes are free; the delivery needs %2 bytes and "
                        "a 4 MiB margin.")
                    .arg(facts.freeBytes)
                    .arg(bytes));
  else
    checks.pass("space");
}

void checkJournal(PreflightChecks& checks, Context& context) {
  Journal* journal = workspaceJournal(context);
  if (!journal)
    checks.fail("journal", "journal_unavailable",
                "This worker has no workspace journal; nothing can be "
                "published durably.");
  else if (!journal->available())
    checks.fail("journal", "journal_unavailable", journal->unavailableReason());
  else
    checks.pass("journal");
}

PreflightChecks runChecks(Context& context, const Plan& plan,
                          const DestinationFacts& facts, qint64 manifestBytes) {
  PreflightChecks checks(checkNames());
  checkPlace(checks, facts);
  checkArtifacts(checks, plan);
  checkVolume(checks, facts, plan.bytes + manifestBytes);
  checkJournal(checks, context);
  return checks;
}

QJsonObject folderRecord(const Item& item) {
  const QJsonObject record    = item.output.record;
  const QJsonObject inputs    = record.value("inputs").toObject();
  const QJsonObject effective = record.value("effective").toObject();
  QJsonObject movie           = item.output.movie;
  for (const char* key : {"artifact_id", "workspace_relative_path", "job_id"})
    movie.remove(QLatin1String(key));
  qint64 bytes = 0;
  for (const RenderedFile& file : item.output.files) bytes += file.bytes;
  return {{"folder", item.folder},
          {"job_id", item.jobId},
          {"kind", orNull(record.value("kind"))},
          {"profile", orNull(record.value("profile"))},
          {"document_id", orNull(record.value("document_id"))},
          {"source_revision", orNull(record.value("source_revision"))},
          {"snapshot_manifest_sha256",
           orNull(inputs.value("snapshot_manifest_sha256"))},
          {"source_fingerprint", orNull(inputs.value("source_fingerprint"))},
          {"frames", QJsonObject{{"start", orNull(record.value("start_frame"))},
                                 {"end", orNull(record.value("end_frame"))},
                                 {"step", orNull(record.value("step"))},
                                 {"count", orNull(record.value("frame_count"))},
                                 {"numbering", "scene_frame_plus_one"}}},
          {"frame_rate", orNull(record.value("frame_rate"))},
          {"width", orNull(record.value("width"))},
          {"height", orNull(record.value("height"))},
          {"alpha", orNull(record.value("alpha"))},
          {"color", orNull(effective.value("color"))},
          {"conversions", orNull(effective.value("conversions"))},
          {"omissions", orNull(effective.value("omissions"))},
          {"fx_provenance", orNull(record.value("fx_provenance"))},
          {"movie", movie.isEmpty() ? QJsonValue() : QJsonValue(movie)},
          {"files", int(item.output.files.size())},
          {"bytes", double(bytes)}};
}

QJsonArray fileRows(const Plan& plan) {
  QJsonArray rows;
  for (const PlannedFile& planned : plan.files)
    rows.append(QJsonObject{
        {"path", planned.path},
        {"folder", plan.items[planned.item].folder},
        {"role", planned.file->role},
        {"frame", planned.file->frame < 0 ? QJsonValue()
                                          : QJsonValue(planned.file->frame)},
        {"size_bytes", double(planned.file->bytes)},
        {"sha256", planned.file->sha256},
        {"media_type", planned.file->mediaType}});
  return rows;
}

// SHA-256 of the canonical request without its operation_id.
QString requestDigest(const QJsonObject& params) {
  QJsonObject request = params;
  request.remove("operation_id");
  return sha256Text(QJsonDocument(request).toJson(QJsonDocument::Compact));
}

QByteArray manifestBytes(Context& context, const Plan& plan,
                         const QJsonObject& params,
                         const QString& operationId) {
  QJsonArray folders;
  for (const Item& item : plan.items) folders.append(folderRecord(item));
  QJsonObject producer = buildIdentity();
  producer.insert("source_commit", QJsonValue());
  producer.insert("platform", runtimePlatform());
  producer.insert("media_tools", mediaToolIdentity(context));
  const QJsonObject manifest{
      {"format", "groundzero-delivery"},
      {"version", 1},
      {"publication",
       QJsonObject{{"operation_id", operationId},
                   {"request_sha256", requestDigest(params)},
                   {"published_utc",
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}},
      {"producer", producer},
      {"folders", folders},
      {"files", fileRows(plan)},
      {"not_included", notIncluded()},
      {"totals", QJsonObject{{"files", int(plan.files.size())},
                             {"bytes", double(plan.bytes)}}}};
  return QJsonDocument(manifest).toJson(QJsonDocument::Compact);
}

// The manifest's size before the publication's operation id is known: the
// longest id it may carry.
qint64 manifestBytesBound(Context& context, const Plan& plan,
                          const QJsonObject& params) {
  return manifestBytes(context, plan, params,
                       QString(OperationIdChars, QChar('x')))
      .size();
}

QJsonArray folderSummaries(const Plan& plan) {
  QJsonArray folders;
  for (const Item& item : plan.items) {
    qint64 bytes = 0;
    for (const RenderedFile& file : item.output.files) bytes += file.bytes;
    const QJsonObject record = item.output.record;
    folders.append(QJsonObject{{"folder", item.folder},
                               {"job_id", item.jobId},
                               {"kind", orNull(record.value("kind"))},
                               {"profile", orNull(record.value("profile"))},
                               {"files", int(item.output.files.size())},
                               {"bytes", double(bytes)}});
  }
  return folders;
}

QJsonObject planReport(Context& context, const Plan& plan,
                       const DestinationFacts& facts, qint64 manifest) {
  return {
      {"location",
       outputLocation(context, plan.target.root, plan.target.relative)},
      {"files", int(plan.files.size()) + 1},
      {"bytes", double(plan.bytes + manifest)},
      {"manifest_bytes", double(manifest)},
      {"free_bytes", facts.freeBytes < 0 ? QJsonValue()
                                         : QJsonValue(double(facts.freeBytes))},
      {"same_volume_as_workspace",
       facts.sameVolume < 0 ? QJsonValue() : QJsonValue(facts.sameVolume == 1)},
      {"folders", folderSummaries(plan)}};
}

QJsonObject preflight(Context& context, const QJsonObject& params) {
  const int offset = integerParam(params, "offset", 0, MaxFileOffset, 0);
  const int limit =
      integerParam(params, "limit", 1, MaxFilePage, DefaultFilePage);
  const Plan plan = makePlan(context, params);
  if (offset > int(plan.files.size()))
    throw Error("invalid_params", "offset exceeds files_total.",
                {{"files_total", int(plan.files.size())}});
  const DestinationFacts facts = observeOutputDestination(context, plan.target);
  const qint64 manifest        = manifestBytesBound(context, plan, params);
  const PreflightChecks checks = runChecks(context, plan, facts, manifest);
  const QJsonArray rows        = fileRows(plan);
  const int end                = std::min(offset + limit, int(rows.size()));
  QJsonArray page;
  for (int index = offset; index < end; ++index) page.append(rows.at(index));
  const QJsonValue failure = checks.firstFailure();
  return {{"ready", failure.isNull()},
          {"first_failure", failure},
          {"checks", checks.report()},
          {"plan", planReport(context, plan, facts, manifest)},
          {"files", page},
          {"files_total", int(rows.size())},
          {"next_offset", end < rows.size() ? QJsonValue(end) : QJsonValue()},
          {"not_included", notIncluded()}};
}

// A publication refuses on the first failed check, before anything is
// written; a job refusal keeps its own details.
void refuseFailedCheck(const PreflightChecks& checks, const Plan& plan) {
  const QJsonValue name = checks.firstFailure();
  if (name.isNull()) return;
  if (name == "jobs") throw *plan.jobFailure;
  const QJsonObject check = checks.failed(name.toString());
  throw Error(check.value("code").toString(), check.value("detail").toString(),
              {{"check", name}});
}

// The digest journalContentDigest computes of the published tree, from the
// verified digests instead of reading the files again.
QString treeDigest(const QVector<StagedEntry>& entries) {
  QStringList lines;
  for (const StagedEntry& entry : entries)
    lines.append(entry.path + '\0' + QString::number(entry.size) + '\0' +
                 QString::fromLatin1(entry.sha256));
  lines.sort();
  return sha256Text(lines.join('\n').toUtf8());
}

QVector<StagedEntry> stagedEntries(const Plan& plan,
                                   const QByteArray& manifest) {
  QVector<StagedEntry> entries;
  for (const PlannedFile& planned : plan.files)
    entries.append({planned.path, planned.file->bytes,
                    planned.file->sha256.toLatin1(), planned.file->path});
  entries.append({ManifestName, manifest.size(),
                  sha256Text(manifest).toLatin1(), QString()});
  return entries;
}

// The publication's intent and stage listing: the manifest stored in the
// journal, its name and the tree's digest.
QJsonObject publicationIntent(Context& context, const QByteArray& manifest,
                              const QString& content) {
  Journal* journal = workspaceJournal(context);
  const QString blob =
      journal && journal->available() && context.writeClass == "publication"
          ? journal->storeBlob(manifest)
          : QString();
  return {{"delivery",
           QJsonObject{{"manifest_blob",
                        blob.isEmpty() ? QJsonValue() : QJsonValue(blob)},
                       {"manifest_bytes", double(manifest.size())},
                       {"manifest_name", ManifestName}}},
          {"manifest_name", ManifestName},
          {"content_sha256", content}};
}

// What a failed publication had done, file by file in plan order: the files
// copied (then removed with the container) before the one that failed, and
// those never reached.
void describeProgress(Error& error, const QVector<StagedEntry>& entries,
                      const PackageTarget& target, const QString& container) {
  if (!error.details.contains("stage")) return;
  const QString stage = error.details.value("stage").toString();
  const QString file  = error.details.value("file").toString();
  int copied          = 0;
  if (stage == "publish" || (stage == "flush" && file.isEmpty())) {
    copied = entries.size();
  } else if (!file.isEmpty()) {
    while (copied < entries.size() && entries.at(copied).path != file) ++copied;
  }
  bool left = false;
  try {
    left = rootDirectoryExists(
        target.rootPath,
        target.parent.isEmpty() ? container : target.parent + '/' + container);
  } catch (const Error&) {
  }
  const bool failed = !file.isEmpty() && copied < entries.size();
  error.details.insert("files_total", entries.size());
  error.details.insert("files_copied", copied);
  error.details.insert("failed_file", failed ? QJsonValue(file) : QJsonValue());
  error.details.insert("not_reached",
                       int(entries.size()) - copied - (failed ? 1 : 0));
  error.details.insert("container_left", left);
}

QJsonObject publish(Context& context, const QJsonObject& params) {
  const Plan plan              = makePlan(context, params);
  const DestinationFacts facts = observeOutputDestination(context, plan.target);
  const QByteArray manifest =
      manifestBytes(context, plan, params, context.writeOperation);
  refuseFailedCheck(runChecks(context, plan, facts, manifest.size()), plan);
  PrivateDirectory staging(context.runtimeDirectory + "/delivery-XXXXXX");
  QFile file(staging.filePath(ManifestName));
  if (!staging.isValid() || !file.open(QIODevice::WriteOnly) ||
      file.write(manifest) != manifest.size() || !file.flush())
    throw Error("io_error", "Cannot write the delivery manifest privately.",
                {{"stage", "manifest"}});
  file.close();
  const QVector<StagedEntry> entries = stagedEntries(plan, manifest);
  const QString content              = treeDigest(entries);
  const RootTarget target{plan.target.root, plan.target.rootPath,
                          plan.target.parent, plan.target.name};
  QString stored;
  try {
    stored = publishToRoot(context, staging.path(), target, entries,
                           publicationIntent(context, manifest, content),
                           FailureCode);
  } catch (Error& error) {
    describeProgress(error, entries, plan.target,
                     stagingContainerName(context.writeOperation));
    throw;
  }
  qint64 bytes = 0;
  for (const StagedEntry& entry : entries) bytes += entry.size;
  return {{"published", true},
          {"location", outputLocation(context, plan.target.root, stored)},
          {"manifest_sha256", sha256Text(manifest)},
          {"content_sha256", content},
          {"files", entries.size()},
          {"bytes", double(bytes)},
          {"folders", folderSummaries(plan)},
          {"not_included", notIncluded()}};
}

QJsonObject destinationSchema() {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"required", QJsonArray{"root", "path"}},
          {"properties",
           QJsonObject{
               {"root", QJsonObject{{"type", "string"},
                                    {"minLength", 1},
                                    {"maxLength", 32},
                                    {"pattern", "^[a-z][a-z0-9_-]{0,31}$"}}},
               {"path", QJsonObject{{"type", "string"},
                                    {"minLength", 1},
                                    {"maxLength", 1024}}}}}};
}

QJsonObject itemsSchema() {
  const QJsonObject item{
      {"type", "object"},
      {"additionalProperties", false},
      {"required", QJsonArray{"job_id", "folder"}},
      {"properties",
       QJsonObject{
           {"job_id",
            QJsonObject{
                {"type", "string"}, {"minLength", 1}, {"maxLength", 80}}},
           {"folder",
            QJsonObject{{"type", "string"},
                        {"minLength", 1},
                        {"maxLength", 64},
                        {"pattern", "^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"}}}}}};
  return {{"type", "array"},
          {"minItems", 1},
          {"maxItems", MaxItems},
          {"items", item}};
}

}  // namespace

QJsonArray deliveryOperations() {
  return {
      operation(
          "delivery.preflight",
          "Plan publishing completed render jobs into an output root: every "
          "file with its size and digest (paged), the destination's checks "
          "(root, path, parent, collision, jobs, artifacts, file system, "
          "space, journal) and what a delivery never includes. Writes "
          "nothing; a failing check is a result (ready false), not an error.",
          {{"destination", destinationSchema()},
           {"items", itemsSchema()},
           {"offset", QJsonObject{{"type", "integer"},
                                  {"minimum", 0},
                                  {"maximum", MaxFileOffset},
                                  {"default", 0}}},
           {"limit", QJsonObject{{"type", "integer"},
                                 {"minimum", 1},
                                 {"maximum", MaxFilePage},
                                 {"default", DefaultFilePage}}}},
          {"destination", "items"}),
      operation(
          "delivery.publish",
          "Publish completed render jobs' verified files into a new "
          "directory below an output root: one folder per job and "
          "delivery.json listing every file's size and SHA-256. Files are "
          "copied and checked byte for byte against the render's digests in "
          "a hidden staging folder, then appear at once by one rename that "
          "never replaces; a failure leaves nothing under the final name and "
          "reports how far the copy got, file by file.",
          {{"destination", destinationSchema()}, {"items", itemsSchema()}},
          {"destination", "items"}, true)};
}

QJsonValue executeDelivery(Context& context, const QString& method,
                           const QJsonObject& params) {
  if (method == "delivery.preflight") return preflight(context, params);
  if (method == "delivery.publish") return publish(context, params);
  throw Error("method_not_found", "The delivery operation is not available.");
}

}  // namespace GroundzeroAutomation
