#include "automation_shared.h"
#include "automation_shared_parse.h"

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cctype>

namespace GroundzeroAutomation {
namespace {

constexpr qint64 PaletteBytes     = 4 * 1024 * 1024;
constexpr int HeadBytes           = 64 * 1024;
constexpr int ScanEntries         = 20000;
constexpr int ScanDepth           = 16;
constexpr int ScanFiles           = 8192;
constexpr qint64 ScanBytes        = 256LL * 1024 * 1024;
constexpr qint64 ScanMilliseconds = 30000;
constexpr int FindMatches         = 16;
constexpr int StylePage           = 128;

const QRegularExpression& paletteIdGrammar() {
  static const QRegularExpression grammar(
      QRegularExpression::anchoredPattern("[0-9A-Za-z_]{1,64}"));
  return grammar;
}

// Mirrors TIStream's tag grammar for the root tag of an uncompressed stream.
class HeadScanner final {
public:
  explicit HeadScanner(const QByteArray& bytes) : m_bytes(bytes) {}

  bool rootTag(QByteArray& name, QHash<QByteArray, QByteArray>& attributes) {
    skipComments();
    if (!match('<')) return false;
    skipBlanks();
    if (!identifier(name)) return false;
    for (;;) {
      skipBlanks();
      if (match('>') || (match('/') && match('>'))) return true;
      QByteArray key, value;
      if (!identifier(key)) return false;
      skipBlanks();
      if (match('=')) {
        skipBlanks();
        if (!quoted(value)) return false;
      }
      attributes.insert(key, value);
    }
  }

private:
  int peek() const {
    return m_at < m_bytes.size() ? static_cast<unsigned char>(m_bytes[m_at])
                                 : -1;
  }
  bool match(char character) {
    if (peek() != static_cast<unsigned char>(character)) return false;
    ++m_at;
    return true;
  }
  void skipBlanks() {
    while (peek() >= 0 && std::isspace(peek())) ++m_at;
  }
  void skipComments() {
    skipBlanks();
    while (m_bytes.mid(m_at, 4) == "<!--") {
      const int end = m_bytes.indexOf("-->", m_at + 4);
      m_at          = end < 0 ? m_bytes.size() : end + 3;
      skipBlanks();
    }
  }
  bool identifier(QByteArray& value) {
    if (peek() < 0 || !std::isalnum(peek())) return false;
    while (peek() >= 0 && (std::isalnum(peek()) || peek() == '_' ||
                           peek() == '.' || peek() == '-'))
      value.append(char(m_bytes[m_at++]));
    return true;
  }
  bool quoted(QByteArray& value) {
    const int quote = peek();
    if (quote != '\'' && quote != '"') return false;
    for (++m_at; m_at < m_bytes.size(); ++m_at) {
      char character = m_bytes[m_at];
      if (character == quote) {
        ++m_at;
        return true;
      }
      if (character == '\\') {
        if (++m_at == m_bytes.size()) return false;
        character = m_bytes[m_at];
        if (character != '\'' && character != '"' && character != '\\')
          return false;
      }
      value.append(character);
    }
    return false;
  }
  const QByteArray& m_bytes;
  int m_at = 0;
};

struct PaletteHead final {
  QString status;
  QString id;
};

PaletteHead paletteHead(const QString& path, qint64 size) {
  if (size > PaletteBytes) return {"too_large", {}};
  QByteArray head;
  try {
    head = readSharedHead(path, HeadBytes);
  } catch (const Error&) {
    return {"unreadable", {}};
  }
  QByteArray tag;
  QHash<QByteArray, QByteArray> attributes;
  if (!HeadScanner(head).rootTag(tag, attributes) || tag != "palette")
    return {"invalid", {}};
  const QString id = QString::fromUtf8(attributes.value("name"));
  if (id.isEmpty()) return {"missing", {}};
  if (!paletteIdGrammar().match(id).hasMatch()) return {"invalid", {}};
  return {"ok", id};
}

bool paletteFileName(const QString& name) {
  return name.endsWith(".tpl") && safeSharedName(name.chopped(4));
}

QString paletteRole(const QJsonObject& params) {
  const QString role = params.value("root").toString();
  if (role != "studio_palettes" && role != "project_palettes")
    throw Error("invalid_params",
                "root must be studio_palettes or project_palettes.");
  return role;
}

QString childPath(const QString& folder, const QString& name) {
  return folder.isEmpty() ? name : folder + '/' + name;
}

struct ListedEntry final {
  EntryInfo info;
  bool folder = false;
  QString folded;
};

struct Skipped final {
  int symlinks = 0, hidden = 0, other = 0, unsafe = 0;
  QJsonObject json() const {
    return {{"symlinks", symlinks},
            {"hidden", hidden},
            {"other_files", other},
            {"unsafe_names", unsafe}};
  }
};

void classify(const EntryInfo& info, const QString& folder, int depth,
              QVector<ListedEntry>& admitted, Skipped& skipped) {
  switch (sharedEntryKind(info)) {
  case SharedEntryKind::Symlink:
    ++skipped.symlinks;
    return;
  case SharedEntryKind::Hidden:
    ++skipped.hidden;
    return;
  case SharedEntryKind::Unsafe:
    ++skipped.unsafe;
    return;
  case SharedEntryKind::Folder:
  case SharedEntryKind::File:
  case SharedEntryKind::Other:
    break;
  }
  const bool isFolder = info.isDir();
  if (!isFolder && !(info.isFile() && paletteFileName(info.fileName()))) {
    ++(info.fileName().endsWith(".tpl") ? skipped.unsafe : skipped.other);
    return;
  }
  if (depth >= 16 || folder.size() + 1 + info.fileName().size() > 1024) {
    ++skipped.unsafe;
    return;
  }
  admitted.append({info, isFolder, foldedName(info.fileName())});
}

QJsonObject entryRow(const ListedEntry& entry, const QString& folder,
                     bool collision) {
  const QString name = entry.info.fileName();
  QJsonObject row{{"name", name},
                  {"path", childPath(folder, name)},
                  {"kind", entry.folder ? "folder" : "palette"},
                  {"size_bytes", QJsonValue()},
                  {"read_only", !entry.info.isWritable()},
                  {"palette_id", QJsonValue()},
                  {"id_status", QJsonValue()},
                  {"case_fold_collision", collision}};
  if (entry.folder) return row;
  const PaletteHead head =
      paletteHead(entry.info.absoluteFilePath(), entry.info.size());
  row.insert("size_bytes", double(entry.info.size()));
  row.insert("palette_id", head.id.isEmpty() ? QJsonValue() : head.id);
  row.insert("id_status", head.status);
  return row;
}

QJsonObject listingPage(const SharedFolder& listing, const QString& folder,
                        int depth, const QJsonObject& params) {
  QVector<ListedEntry> admitted;
  Skipped skipped;
  for (const EntryInfo& info : listing.entries)
    classify(info, folder, depth, admitted, skipped);
  std::sort(admitted.begin(), admitted.end(),
            [](const ListedEntry& left, const ListedEntry& right) {
              if (left.folder != right.folder) return left.folder;
              if (left.folded != right.folded)
                return left.folded < right.folded;
              return left.info.fileName() < right.info.fileName();
            });
  QHash<QString, int> folds;
  for (const ListedEntry& entry : admitted) ++folds[entry.folded];
  const int offset = integerParam(params, "offset", 0, SharedFolderEntries, 0);
  const int limit  = integerParam(params, "limit", 1, 256, 128);
  const int end    = std::min<int>(admitted.size(), offset + limit);
  QJsonArray entries;
  for (int index = offset; index < end; ++index)
    entries.append(entryRow(admitted[index], folder,
                            folds.value(admitted[index].folded) > 1));
  return {
      {"listing_revision", listing.revision},
      {"total", admitted.size()},
      {"entries", entries},
      {"next_offset", end < admitted.size() ? QJsonValue(end) : QJsonValue()},
      {"skipped", skipped.json()}};
}

QJsonObject listPalettes(Context& context, const QJsonObject& params) {
  const QString role         = paletteRole(params);
  const QString folder       = params.value("folder").toString();
  const QStringList segments = sharedSegments(folder, true);
  const QString directory =
      resolveSharedPath(sharedRootPath(context, role), segments);
  if (!QFileInfo(directory).isDir())
    throw Error("not_found", "The studio palette folder does not exist.");
  QJsonObject result =
      stableSharedListing(directory, [&](const SharedFolder& listing) {
        requireListingRevision(params, listing);
        return listingPage(listing, folder, segments.size(), params);
      });
  result.insert("root", role);
  result.insert("folder", folder);
  return result;
}

QStringList palettePath(const QJsonObject& params) {
  const QStringList segments =
      sharedSegments(requiredString(params, "path"), false);
  if (!paletteFileName(segments.last()))
    throw Error("invalid_path", "Studio palette paths name a .tpl file.");
  return segments;
}

[[noreturn]] void outOfProfile(const QString& message) {
  throw Error("unsupported_palette", message, {{"cause", "out_of_bounds"}});
}

int bounded(const QJsonObject& object, const QString& key, int maximum) {
  const QJsonValue value = object.value(key);
  if (!value.isDouble() || value.toInt(-1) < 0 || value.toInt() > maximum)
    outOfProfile("The palette field " + key + " is outside its bound.");
  return value.toInt();
}

QString boundedText(const QJsonObject& object, const QString& key, int minimum,
                    int maximum) {
  const QString value = object.value(key).toString();
  if (value.size() < minimum || value.size() > maximum)
    outOfProfile("The palette text " + key + " is outside its bound.");
  return value;
}

QJsonValue styleLink(const QJsonObject& style, const QString& paletteId) {
  static const QRegularExpression grammar(QRegularExpression::anchoredPattern(
      "([+-])([0-9A-Za-z_]{1,64})-([0-9]{1,4})"));
  const QString global = boundedText(style, "global_name", 0, 256);
  if (global.isEmpty()) return QJsonValue();
  const QString origin = boundedText(style, "original_name", 0, 128);
  QJsonObject link{{"global_name", global},
                   {"state", "malformed"},
                   {"palette_id", QJsonValue()},
                   {"source_style_id", QJsonValue()},
                   {"original_name", origin.isEmpty() ? QJsonValue() : origin},
                   {"edited", style.value("edited").toBool()}};
  const auto match = grammar.match(global);
  if (!match.hasMatch() || match.captured(3).toInt() > 4095) return link;
  const QString source = match.captured(2);
  const bool own       = match.captured(1) == "-" && source == paletteId;
  link.insert("state", match.captured(1) == "+" ? "on"
                       : own                    ? "source"
                                                : "off");
  link.insert("palette_id", source);
  link.insert("source_style_id", match.captured(3).toInt());
  return link;
}

QJsonObject styleRow(const QJsonObject& style, const QString& paletteId) {
  QJsonObject row{
      {"style_id", bounded(style, "style_id", 4095)},
      {"name", boundedText(style, "name", 0, 128)},
      {"native_tag", bounded(style, "native_tag", 100000)},
      {"solid", style.value("solid").toBool()},
      {"color",
       style.value("color").isObject() ? style.value("color") : QJsonValue()},
      {"keyframe_count", bounded(style, "keyframe_count", 4096)},
      {"page_index", style.value("page_index").isNull()
                         ? QJsonValue()
                         : QJsonValue(bounded(style, "page_index", 127))},
      {"link", styleLink(style, paletteId)}};
  return row;
}

QJsonArray pageRows(const QJsonArray& pages) {
  if (pages.isEmpty() || pages.size() > 128)
    outOfProfile("Studio palettes need 1 to 128 pages.");
  QJsonArray rows;
  for (int index = 0; index < pages.size(); ++index) {
    const QJsonObject page = pages[index].toObject();
    QJsonArray ids;
    for (const QJsonValue& id : page.value("style_ids").toArray())
      ids.append(bounded({{"id", id}}, "id", 4095));
    rows.append(QJsonObject{{"page_index", index},
                            {"name", boundedText(page, "name", 1, 128)},
                            {"style_ids", ids}});
  }
  return rows;
}

QJsonObject paletteResult(const QJsonObject& summary, const QJsonObject& params,
                          QJsonObject result) {
  const QJsonArray styles = summary.value("styles").toArray();
  if (styles.isEmpty() || styles.size() > 4096)
    outOfProfile("Studio palettes need 1 to 4096 styles.");
  const QString global = summary.value("global_name").toString();
  const bool validId   = paletteIdGrammar().match(global).hasMatch();
  const int offset     = integerParam(params, "offset", 0, 4096, 0);
  const int limit      = integerParam(params, "limit", 1, StylePage, StylePage);
  const int end        = std::min<int>(styles.size(), offset + limit);
  bool solid           = true;
  QJsonArray page;
  for (int id = 0; id < styles.size(); ++id) {
    const QJsonObject row = styleRow(styles[id].toObject(), global);
    solid                 = solid && row.value("solid").toBool();
    if (id >= offset && id < end) page.append(row);
  }
  result.insert("palette_id", validId ? QJsonValue(global) : QJsonValue());
  result.insert("locked", summary.value("locked").toBool());
  result.insert("cleanup_palette", summary.value("cleanup_palette").toBool());
  result.insert("animated", summary.value("animated").toBool());
  result.insert("all_styles_solid", solid);
  result.insert(
      "color_model",
      QJsonObject{{"present", summary.value("color_model").toBool()}});
  result.insert("style_count", styles.size());
  result.insert("pages", pageRows(summary.value("pages").toArray()));
  result.insert("styles", page);
  result.insert("next_offset",
                end < styles.size() ? QJsonValue(end) : QJsonValue());
  return result;
}

void requireFileRevision(const QJsonObject& params, const SharedFile& file) {
  if (!params.contains("expected_file_revision")) return;
  const QString expected = params.value("expected_file_revision").toString();
  if (!validRevisionText(expected, "sha256"))
    throw Error("invalid_params",
                "expected_file_revision must be sha256:<64 hex>.");
  if (expected != file.revision)
    throw Error("revision_conflict", "The shared file changed.",
                {{"scope", "shared_file"}, {"file_revision", file.revision}});
}

QJsonObject inspectPalette(Context& context, const QJsonObject& params) {
  const QString role         = paletteRole(params);
  const QStringList segments = palettePath(params);
  const QString path =
      resolveSharedPath(sharedRootPath(context, role), segments);
  const SharedFile file = readSharedFile(path, PaletteBytes);
  requireFileRevision(params, file);
  const QJsonObject summary = parseSharedFile(
      context, "studio_palette", file.bytes, "unsupported_palette");
  return paletteResult(summary, params,
                       {{"root", role},
                        {"path", segments.join('/')},
                        {"file_revision", file.revision},
                        {"size_bytes", double(file.bytes.size())},
                        {"read_only", !QFileInfo(path).isWritable()}});
}

struct Scan final {
  QString target;
  QJsonArray matches;
  QSet<QString> reasons;
  int directories = 0, files = 0, entries = 0;
  qint64 bytes = 0;
  QElapsedTimer timer;
  bool stopped = false;

  void stop(const QString& reason) {
    reasons.insert(reason);
    stopped = true;
  }
};

void scanPalette(Scan& scan, const QString& role, const EntryInfo& info,
                 const QString& path) {
  if (info.size() > PaletteBytes) {
    scan.reasons.insert("oversized_files");
    return;
  }
  if (scan.files == ScanFiles) return scan.stop("file_limit");
  const qint64 head = std::min<qint64>(info.size(), HeadBytes);
  if (scan.bytes + head > ScanBytes) return scan.stop("byte_limit");
  ++scan.files;
  scan.bytes += head;
  const PaletteHead parsed = paletteHead(info.absoluteFilePath(), info.size());
  if (parsed.status == "unreadable") scan.reasons.insert("unreadable_entries");
  if (parsed.id != scan.target) return;
  if (scan.matches.size() == FindMatches) return scan.stop("row_limit");
  scan.matches.append(QJsonObject{
      {"root", role}, {"path", path}, {"size_bytes", double(info.size())}});
}

void scanEntry(Scan& scan, const QString& role, const EntryInfo& info,
               const QString& path, QVector<QPair<QString, int>>& pending,
               int depth) {
  switch (sharedEntryKind(info)) {
  case SharedEntryKind::Symlink:
    scan.reasons.insert("symlinks_skipped");
    return;
  case SharedEntryKind::Folder:
    if (depth + 1 >= ScanDepth)
      scan.reasons.insert("depth_limit");
    else
      pending.append({path, depth + 1});
    return;
  case SharedEntryKind::File:
    if (paletteFileName(info.fileName())) scanPalette(scan, role, info, path);
    return;
  case SharedEntryKind::Hidden:
  case SharedEntryKind::Unsafe:
  case SharedEntryKind::Other:
    return;
  }
}

void scanRoot(Scan& scan, const QString& role, const QString& root) {
  QVector<QPair<QString, int>> pending{{QString(), 0}};
  while (!pending.isEmpty() && !scan.stopped) {
    const auto [relative, depth] = pending.takeLast();
    if (scan.timer.elapsed() > ScanMilliseconds) return scan.stop("time_limit");
    const QString directory = relative.isEmpty() ? root : root + '/' + relative;
    QVector<EntryInfo> listed;
    const ListResult result =
        listApprovedDirectory(directory, listed, ScanEntries + 1);
    if (result == ListResult::Failed) {
      scan.reasons.insert("unreadable_entries");
      continue;
    }
    ++scan.directories;
    for (const EntryInfo& entry : listed) {
      if (scan.stopped) break;
      if (++scan.entries > ScanEntries) return scan.stop("entry_limit");
      scanEntry(scan, role, entry, childPath(relative, entry.fileName()),
                pending, depth);
    }
  }
}

QStringList paletteRoots(const Context& context) {
  QStringList roles;
  for (const QString& role :
       {QString("studio_palettes"), QString("project_palettes")})
    if (context.sharedRoots.contains(role)) roles.append(role);
  if (roles.isEmpty())
    throw Error("root_unavailable",
                "No studio palette root was granted at worker launch.",
                {{"root", "studio_palettes"}, {"reason", "not_configured"}});
  return roles;
}

QJsonObject findPalette(Context& context, const QJsonObject& params) {
  Scan scan;
  scan.target = requiredString(params, "palette_id");
  if (!paletteIdGrammar().match(scan.target).hasMatch())
    throw Error("invalid_params",
                "palette_id must match ^[0-9A-Za-z_]{1,64}$.");
  const QStringList roles = paletteRoots(context);
  QStringList roots;
  for (const QString& role : roles) roots.append(sharedRootPath(context, role));
  scan.timer.start();
  for (int index = 0; index < roles.size() && !scan.stopped; ++index)
    scanRoot(scan, roles[index], roots[index]);
  QStringList reasons = scan.reasons.values();
  std::sort(reasons.begin(), reasons.end());
  const bool complete = reasons.isEmpty();
  return {{"palette_id", scan.target},
          {"matches", scan.matches},
          {"unique", complete && scan.matches.size() == 1},
          {"completeness",
           QJsonObject{
               {"status", complete ? "complete" : "partial"},
               {"reasons", QJsonArray::fromStringList(reasons)},
               {"scanned", QJsonObject{{"directories", scan.directories},
                                       {"files_read", scan.files},
                                       {"bytes_read", double(scan.bytes)}}}}}};
}

QJsonObject rootSchema() {
  return {{"type", "string"},
          {"enum", QJsonArray{"studio_palettes", "project_palettes"}}};
}

QJsonObject folderSchema() {
  const QString segment = sharedSegmentPattern();
  return {{"type", "string"},
          {"minLength", 0},
          {"maxLength", 1024},
          {"pattern", "^(?:" + segment + "(?:/" + segment + "){0,15})?$"}};
}

QJsonObject palettePathSchema() {
  const QString segment = sharedSegmentPattern();
  return {{"type", "string"},
          {"minLength", 5},
          {"maxLength", 1024},
          {"pattern", "^(?:" + segment + "/){0,15}" + segment + "\\.tpl$"}};
}

}  // namespace

QJsonArray studioPaletteOperations() {
  return {
      sharedDescriptor(
          "studio_palette.list",
          "List one studio-palette folder: subfolders first, then .tpl "
          "palettes, each with its native palette id when readable. Bounded "
          "and paged; never imports anything.",
          {{"root", rootSchema()},
           {"folder", folderSchema()},
           {"offset", sharedIntegerSchema(0, 8192)},
           {"limit", sharedIntegerSchema(1, 256)},
           {"expected_listing_revision", sharedRevisionSchema("listing")}},
          {"root", "folder"}),
      sharedDescriptor(
          "studio_palette.inspect",
          "Read one studio palette: identity, file revision, lock/read-only "
          "state, pages and a page of styles with link metadata. Parses a "
          "private copy of the hashed bytes.",
          {{"root", rootSchema()},
           {"path", palettePathSchema()},
           {"offset", sharedIntegerSchema(0, 4096)},
           {"limit", sharedIntegerSchema(1, 128)},
           {"expected_file_revision", sharedRevisionSchema("sha256")}},
          {"root", "path"}),
      sharedDescriptor(
          "studio_palette.find",
          "Locate every .tpl whose native palette id equals palette_id under "
          "the configured palette roots. Reports duplicates and scan "
          "completeness; writes no path cache.",
          {{"palette_id", QJsonObject{{"type", "string"},
                                      {"minLength", 1},
                                      {"maxLength", 64},
                                      {"pattern", "^[0-9A-Za-z_]{1,64}$"}}}},
          {"palette_id"})};
}

QJsonValue executeStudioPalette(Context& context, const QString& method,
                                const QJsonObject& params) {
  if (method == "studio_palette.list") return listPalettes(context, params);
  if (method == "studio_palette.inspect")
    return inspectPalette(context, params);
  if (method == "studio_palette.find") return findPalette(context, params);
  throw Error("method_not_found",
              "The studio palette operation is not available.");
}

}  // namespace GroundzeroAutomation
