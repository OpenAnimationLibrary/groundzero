#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

namespace RasterBrush {
namespace Detail {
// Version 1 preserves opaque source tag labels and their manifest association.
// Resource tags describe the source library, not a dependency or rights claim.
inline QString bundleMetadataError(const QJsonValue &value) {
  const auto fail = [] {
    return QString("Invalid or oversized bundle tag metadata");
  };
  if (!value.isObject()) return fail();
  const auto root = value.toObject();
  if (root.size() != 4 || root.value("version") != QJsonValue(1) ||
      root.value("manifestVersion") != QJsonValue("1.2") ||
      !root.contains("presetTags") || !root.contains("resourceTags") ||
      QJsonDocument(root).toJson(QJsonDocument::Compact).size() > 12288)
    return fail();
  auto tagsValid = [](const QJsonValue &v) {
    if (!v.isArray() || v.toArray().size() > 64) return false;
    for (const auto &tag : v.toArray())
      if (!tag.isString() || tag.toString().size() > 256 ||
          tag.toString().contains(QChar::Null))
        return false;
    return true;
  };
  if (!tagsValid(root.value("presetTags")) ||
      !root.value("resourceTags").isArray() ||
      root.value("resourceTags").toArray().size() > 512)
    return fail();
  QSet<QString> members;
  for (const auto &entry : root.value("resourceTags").toArray()) {
    if (!entry.isObject()) return fail();
    const auto resource = entry.toObject();
    const auto member   = resource.value("member").toString();
    const auto type     = resource.value("mediaType").toString();
    if (resource.size() != 3 || member.isEmpty() || member.size() > 1024 ||
        member.startsWith('/') || member.contains('\\') ||
        member.contains(':') || member.contains(QChar::Null) ||
        type.isEmpty() || type.size() > 128 || type.contains('/') ||
        type.contains('\\') || type.contains(':') ||
        type.contains(QChar::Null) || !member.startsWith(type + '/') ||
        members.contains(member) || !tagsValid(resource.value("tags")))
      return fail();
    for (const auto &part : member.split('/'))
      if (part.isEmpty() || part == "." || part == "..") return fail();
    members.insert(member);
  }
  return {};
}
}  // namespace Detail
}  // namespace RasterBrush
