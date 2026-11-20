#include "automation_grants.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace GroundzeroAutomation {
namespace {

const QStringList& grantClasses() {
  static const QStringList classes{"inspect", "draft", "file", "job"};
  return classes;
}

// Distinct, non-empty comma-separated names, or invalid_argument naming the
// option.
QStringList distinctNames(const QString& option, const QString& value) {
  const QStringList names = value.split(',');
  if (QSet<QString>(names.begin(), names.end()).size() != names.size() ||
      names.contains(QString()))
    throw Error("invalid_argument",
                option + " takes distinct comma-separated names.",
                {{"option", option}});
  return names;
}

QStringList withImplied(QStringList names, const QString& implied) {
  if (!names.contains(implied)) names.append(implied);
  names.sort();
  return names;
}

QStringList grantedClasses(const Context& context) {
  return withImplied(context.authority.grants, "inspect");
}

QStringList grantedDomains(const Context& context) {
  return withImplied(context.authority.domains, "core");
}

bool discardsReadOnly(Context& context, const QJsonObject& effects,
                      const QJsonObject& params) {
  if (effects.value("document") != "discards" ||
      !params.value("document_id").isString())
    return false;
  try {
    return context.document(params.value("document_id").toString()).readOnly !=
           nullptr;
  } catch (const Error& error) {
    if (error.code != "document_not_found") throw;
    return false;
  }
}

[[noreturn]] void deny(const Context& context, const QString& tool,
                       const QStringList& missing, const QString& domain) {
  QStringList needs;
  if (!missing.isEmpty()) needs << "--grants " + missing.join(',');
  if (!grantedDomains(context).contains(domain)) needs << "--domains " + domain;
  throw Error("grant_denied",
              "This session was not granted what " + tool +
                  " needs; start the worker with " + needs.join(" and ") + ".",
              {{"tool", tool},
               {"missing", QJsonArray::fromStringList(missing)},
               {"domain", domain},
               {"granted", QJsonArray::fromStringList(grantedClasses(context))},
               {"granted_domains",
                QJsonArray::fromStringList(grantedDomains(context))}});
}

}  // namespace

QStringList parseGrants(const QString& value) {
  QStringList grants = distinctNames("--grants", value);
  for (const QString& grant : grants)
    if (!grantClasses().contains(grant))
      throw Error("invalid_argument",
                  "--grants names the unknown grant class " + grant +
                      "; use inspect, draft, file and job.",
                  {{"option", "--grants"}, {"value", grant}});
  if (!grants.contains("inspect"))
    throw Error("invalid_argument", "--grants must contain inspect.",
                {{"option", "--grants"}});
  grants.sort();
  return grants;
}

QStringList parseDomains(const QString& value, const QStringList& toolsets) {
  const QStringList names = distinctNames("--domains", value);
  if (names.contains("all")) {
    if (names.size() != 1)
      throw Error("invalid_argument",
                  "--domains takes all alone or a list of toolset names.",
                  {{"option", "--domains"}});
    return withImplied(toolsets, "core");
  }
  for (const QString& name : names)
    if (!toolsets.contains(name))
      throw Error("invalid_argument",
                  "--domains names the unknown toolset " + name + "; use " +
                      toolsets.join(", ") + " or all.",
                  {{"option", "--domains"}, {"value", name}});
  return withImplied(names, "core");
}

QJsonObject authorityReport(const Context& context) {
  QStringList inputs = context.inputRoots();
  inputs.removeDuplicates();
  inputs.sort();
  QJsonArray shared;
  for (auto it = context.sharedRoots.constBegin();
       it != context.sharedRoots.constEnd(); ++it)
    shared.append(QJsonObject{{"role", it.key()},
                              {"path", it.value().path},
                              {"writable", it.value().writable}});
  QJsonArray outputs;
  for (auto it = context.outputRoots.constBegin();
       it != context.outputRoots.constEnd(); ++it)
    outputs.append(QJsonObject{{"name", it.key()},
                               {"path", it.value().path},
                               {"modify", it.value().modify}});
  const QString media = context.mediaToolsDirectory;
  QJsonObject report{
      {"grants", QJsonArray::fromStringList(grantedClasses(context))},
      {"domains", QJsonArray::fromStringList(grantedDomains(context))},
      {"workspace", context.workspace()},
      {"input_roots", QJsonArray::fromStringList(inputs)},
      {"shared_roots", shared},
      {"output_roots", outputs},
      {"media_tools", media.isEmpty() ? QJsonValue() : QJsonValue(media)}};
  const QByteArray digest = QCryptographicHash::hash(
      QJsonDocument(report).toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256);
  report.insert("digest", QString::fromLatin1(digest.toHex()));
  return report;
}

void requireGrants(Context& context, const QJsonObject& descriptor,
                   const QJsonObject& params) {
  const QString tool        = descriptor.value("name").toString();
  const QJsonObject effects = descriptor.value("effects").toObject();
  if (!effects.value("grants").isArray())
    throw Error("internal_error",
                "Tool " + tool + " has no grants in its effect declaration.");
  const QStringList granted = grantedClasses(context);
  QStringList missing;
  for (const QJsonValue& value : effects.value("grants").toArray()) {
    const QString grant = value.toString();
    if (granted.contains(grant)) continue;
    if (grant == "draft" && discardsReadOnly(context, effects, params))
      continue;
    missing.append(grant);
  }
  const QString domain = descriptor.value("toolset").toString();
  if (!missing.isEmpty() || !grantedDomains(context).contains(domain))
    deny(context, tool, missing, domain);
}

}  // namespace GroundzeroAutomation
