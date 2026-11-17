#include "automation.h"
#include "automation_dispatch.h"
#include "automation_shared.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

const QString PaletteId = "1700000000_1234567890123456789";

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

QJsonObject call(Dispatcher& dispatcher, const QString& method,
                 const QJsonObject& params) {
  return dispatcher.dispatch(
      {{"id", "shared-test"}, {"method", method}, {"params", params}});
}

QJsonObject result(Dispatcher& dispatcher, const QString& method,
                   const QJsonObject& params) {
  const QJsonObject response = call(dispatcher, method, params);
  if (!response.contains("result"))
    throw std::runtime_error(
        QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString());
  return response.value("result").toObject();
}

QJsonObject failure(Dispatcher& dispatcher, const QString& method,
                    const QJsonObject& params, const QString& code) {
  const QJsonObject error =
      call(dispatcher, method, params).value("error").toObject();
  if (error.value("code").toString() != code)
    throw std::runtime_error(
        (method + " expected " + code + ": " +
         QJsonDocument(error).toJson(QJsonDocument::Compact))
            .toStdString());
  return error.value("details").toObject();
}

void write(const QString& path, const QByteArray& bytes) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
          "Cannot write a shared fixture");
}

QByteArray palette(const QString& id, const QStringList& styles,
                   const QByteArray& extra = {}) {
  QByteArray indices;
  for (int index = 0; index < styles.size(); ++index)
    indices += QByteArray::number(index) + ' ';
  QByteArray text =
      id.isNull() ? "<palette>\n" : "<palette name=\"" + id.toUtf8() + "\">\n";
  text += "<version>71 0</version><styles>";
  for (const QString& style : styles)
    text += "<style>" + style.toUtf8() + "</style>";
  text += "</styles><stylepages><page><name>colors</name><indices>" + indices +
          "</indices></page></stylepages>" + extra + "</palette>\n";
  return text;
}

QString canonical(const QTemporaryDir& directory) {
  return QFileInfo(directory.path()).canonicalFilePath();
}

class RootsScope final {
public:
  RootsScope(Context& context, QMap<QString, SharedRootGrant> roots)
      : m_context(context), m_saved(context.sharedRoots) {
    context.sharedRoots = std::move(roots);
  }
  ~RootsScope() { m_context.sharedRoots = m_saved; }

private:
  Context& m_context;
  QMap<QString, SharedRootGrant> m_saved;
};

void checkGrammar() {
  for (const QString& good : {QString("Global Palettes"), QString("é"),
                              QString("a.b"), QString(120, 'x')})
    require(safeSharedName(good), "A safe shared name was refused");
  const QString decomposed = QString("e") + QChar(0x0301);
  for (const QString& bad :
       {QString(), QString(".hidden"), QString("trail."), QString("trail "),
        QString(" lead"), QString("a:b"), QString("a\\b"), QString("CON"),
        QString("lpt1.tpl"), QString(121, 'x'), decomposed,
        QString("a") + QChar(0x7f)})
    require(!safeSharedName(bad), "An unsafe shared name was admitted");
  require(sharedSegments("", true).isEmpty(), "The root path was refused");
  QStringList deep;
  for (int index = 0; index < 17; ++index) deep.append("d");
  for (const QString& bad : {QString("a//b"), QString("../a"), QString("/abs"),
                             deep.join('/'), QString(1025, 'a')}) {
    bool refused = false;
    try {
      sharedSegments(bad, true);
    } catch (const Error& error) {
      refused = error.code == "invalid_path";
    }
    require(refused, "An unsafe shared path was admitted");
  }
}

void checkRoots(Context& context, Dispatcher& dispatcher,
                const QString& studio) {
  {
    RootsScope none(context, {});
    const QJsonArray rows =
        result(dispatcher, "shared.roots", {}).value("roots").toArray();
    require(
        rows.size() == 3 && !rows[0].toObject().value("configured").toBool(),
        "Unconfigured roles must be reported as unconfigured");
    const QJsonObject details = failure(
        dispatcher, "studio_palette.list",
        {{"root", "studio_palettes"}, {"folder", ""}}, "root_unavailable");
    require(details.value("reason") == "not_configured",
            "A missing grant must name its reason");
  }
  RootsScope scope(context, {{"studio_palettes", {studio, false}}});
  const QJsonObject row = result(dispatcher, "shared.roots", {})
                              .value("roots")
                              .toArray()[0]
                              .toObject();
  require(row.value("available").toBool() && !row.value("writable").toBool() &&
              row.value("path") == studio &&
              row.value("layout").toObject().value("global_palettes_folder") ==
                  true,
          "A granted studio root is read-only and reports its layout");
}

void buildStudio(const QString& studio) {
  const QString global = studio + "/Global Palettes";
  write(global + "/Warm.tpl",
        palette(PaletteId, {"\"|-" + PaletteId + "-0\" color_0 3 255 255 255 0",
                            "\"|+Other_9-1\" \"@Sky\" color_1 3 10 20 30 255",
                            "\"|-Other_9-2\" \"@@Ink\" color_2 3 1 2 3 255",
                            "\"|bad\" color_3 3 4 5 6 255"}));
  write(global + "/Plain.tpl", palette(QString(), {"color_0 3 0 0 0 0"}));
  write(global + "/Odd.tpl", palette("bad id!", {"color_0 3 0 0 0 0"}));
  write(global + "/Notes.txt", "notes");
  write(global + "/.hidden.tpl", palette(PaletteId, {"color_0 3 0 0 0 0"}));
  QDir().mkpath(global + "/Sub");
  QFile large(global + "/Large.tpl");
  require(large.open(QIODevice::WriteOnly) && large.resize(4 * 1024 * 1024 + 1),
          "Cannot create the oversized palette fixture");
#ifndef _WIN32
  require(QFile::link(global + "/Warm.tpl", global + "/Alias.tpl"),
          "Cannot create the symlink fixture");
#endif
}

void checkListing(Dispatcher& dispatcher) {
  const QJsonObject params{{"root", "studio_palettes"},
                           {"folder", "Global Palettes"}};
  const QJsonObject listing = result(dispatcher, "studio_palette.list", params);
  const QJsonArray entries  = listing.value("entries").toArray();
  QStringList order;
  QHash<QString, QString> statuses;
  for (const QJsonValue& value : entries) {
    order.append(value.toObject().value("name").toString());
    statuses.insert(order.last(),
                    value.toObject().value("id_status").toString());
  }
  require(order == QStringList({"Sub", "Large.tpl", "Odd.tpl", "Plain.tpl",
                                "Warm.tpl"}),
          "Folders come first, then palettes by folded name");
  require(statuses.value("Warm.tpl") == "ok" &&
              statuses.value("Plain.tpl") == "missing" &&
              statuses.value("Odd.tpl") == "invalid" &&
              statuses.value("Large.tpl") == "too_large",
          "Palette id statuses follow the bounded root-tag read");
  const QJsonObject skipped = listing.value("skipped").toObject();
  require(skipped.value("hidden").toInt() == 1 &&
              skipped.value("other_files").toInt() == 1,
          "Hidden and non-palette entries are skipped and counted");
#ifndef _WIN32
  require(skipped.value("symlinks").toInt() == 1,
          "Symbolic links are skipped and counted, never followed");
  failure(dispatcher, "studio_palette.inspect",
          {{"root", "studio_palettes"}, {"path", "Global Palettes/Alias.tpl"}},
          "path_denied");
#endif
  QJsonObject paged = params;
  paged.insert("limit", 2);
  paged.insert("offset", 1);
  const QJsonObject page = result(dispatcher, "studio_palette.list", paged);
  require(page.value("entries").toArray().size() == 2 &&
              page.value("next_offset") == 3,
          "Listings page with offset and limit");
  QJsonObject stale = params;
  stale.insert("expected_listing_revision", "listing:" + QString(64, '0'));
  require(failure(dispatcher, "studio_palette.list", stale, "revision_conflict")
                  .value("scope") == "listing",
          "A stale listing revision is a listing conflict");
}

void checkInspect(Dispatcher& dispatcher, const QString& studio) {
  const QJsonObject params{{"root", "studio_palettes"},
                           {"path", "Global Palettes/Warm.tpl"}};
  const QJsonObject inspected =
      result(dispatcher, "studio_palette.inspect", params);
  require(inspected.value("palette_id") == PaletteId &&
              inspected.value("style_count") == 4 &&
              inspected.value("all_styles_solid").toBool(),
          "A studio palette reports its id, styles and solidity");
  QStringList states;
  for (const QJsonValue& style : inspected.value("styles").toArray())
    states.append(style.toObject().value("link").isNull() ? QString("none")
                                                          : style.toObject()
                                                                .value("link")
                                                                .toObject()
                                                                .value("state")
                                                                .toString());
  require(states == QStringList({"source", "on", "off", "malformed"}),
          "Style links parse as source, on, off and malformed");
  const QJsonObject edited = inspected.value("styles")
                                 .toArray()[2]
                                 .toObject()
                                 .value("link")
                                 .toObject();
  require(
      edited.value("original_name") == "Ink" && edited.value("edited").toBool(),
      "Original names and the edited flag are reported");
  QJsonObject stale = params;
  stale.insert("expected_file_revision", "sha256:" + QString(64, '0'));
  require(
      failure(dispatcher, "studio_palette.inspect", stale, "revision_conflict")
              .value("file_revision") == inspected.value("file_revision"),
      "A stale file revision is refused with the current one");
  failure(dispatcher, "studio_palette.inspect",
          {{"root", "studio_palettes"}, {"path", "Global Palettes/Large.tpl"}},
          "resource_limit");
  failure(dispatcher, "studio_palette.inspect",
          {{"root", "studio_palettes"}, {"path", "Global Palettes/warm.tpl"}},
          "not_found");
  write(studio + "/Global Palettes/Texture.tpl",
        palette(PaletteId, {"texture_x 4 \"/missing/t.bmp\" 255 0 0 255 0 0 "
                            "1 0 1 1 1"}));
  const QJsonObject crashed = failure(
      dispatcher, "studio_palette.inspect",
      {{"root", "studio_palettes"}, {"path", "Global Palettes/Texture.tpl"}},
      "unsupported_palette");
  require(crashed.value("cause") == "crashed",
          "A native parser crash ends only the helper");
  require(result(dispatcher, "studio_palette.list",
                 {{"root", "studio_palettes"}, {"folder", ""}})
              .contains("entries"),
          "The worker keeps serving after a helper crash");
}

void checkFind(Context& context, Dispatcher& dispatcher, const QString& studio,
               const QString& project) {
  write(project + "/Copy.tpl", palette(PaletteId, {"color_0 3 0 0 0 0"}));
  RootsScope scope(context, {{"studio_palettes", {studio, false}},
                             {"project_palettes", {project, false}}});
  const QJsonObject found =
      result(dispatcher, "studio_palette.find", {{"palette_id", PaletteId}});
  require(found.value("matches").toArray().size() == 3 &&
              !found.value("unique").toBool(),
          "Duplicate palette ids are reported, never resolved");
#ifndef _WIN32
  require(found.value("completeness")
              .toObject()
              .value("reasons")
              .toArray()
              .contains("symlinks_skipped"),
          "A skipped symbolic link makes the scan partial");
#endif
  failure(dispatcher, "studio_palette.find", {{"palette_id", "bad id"}},
          "invalid_params");
}

void checkStudio(Context& context, Dispatcher& dispatcher) {
  QTemporaryDir studioDirectory, projectDirectory;
  require(studioDirectory.isValid() && projectDirectory.isValid(),
          "Cannot create shared root fixtures");
  const QString studio = canonical(studioDirectory);
  buildStudio(studio);
  checkRoots(context, dispatcher, studio);
  {
    RootsScope scope(context, {{"studio_palettes", {studio, false}}});
    checkListing(dispatcher);
    checkInspect(dispatcher, studio);
    for (const QString& bad : {QString("../x"), QString("Global Palettes/"),
                               QString("CON"), QString("a\\b")})
      failure(dispatcher, "studio_palette.list",
              {{"root", "studio_palettes"}, {"folder", bad}}, "invalid_path");
  }
  checkFind(context, dispatcher, studio, canonical(projectDirectory));
}

QByteArray sidecar(const QString& id, const QString& kind,
                   const QString& revision = "2") {
  return ("<?xml version=\"1.0\"?><libraryItem version=\"1\"><id>" + id +
          "</id><revision>" + revision + "</revision><kind>" + kind +
          "</kind><sourceScene>Shot</sourceScene><future>x</future>"
          "</libraryItem>")
      .toUtf8();
}

void buildLibrary(const QString& library) {
  const QString templates = library + "/templates";
  for (const QString& name :
       {QString("Rig"), QString("Copy"), QString("Legacy"), QString("Broken"),
        QString("Pose")})
    write(templates + '/' + name + ".tnz", "<tnz/>");
  write(templates + "/Rig.libinfo", sidecar("id-1", "rig"));
  write(templates + "/Copy.libinfo", sidecar("id-1", "symbol"));
  write(templates + "/Broken.libinfo", sidecar("id-2", "rig", "zero"));
  write(templates + "/Pose.libinfo", sidecar("id-3", "poseSet"));
  QDir().mkpath(templates + "/sceneIcons");
  QImage icon(4, 3, QImage::Format_ARGB32);
  icon.fill(Qt::red);
  require(icon.save(templates + "/sceneIcons/Rig .png", "PNG"),
          "Cannot write the icon fixture");
  write(templates + "/sceneIcons/Copy .png", "not a png");
  QDir().mkpath(library + "/poses");
}

void checkLibrary(Context& context, Dispatcher& dispatcher) {
  QTemporaryDir directory;
  require(directory.isValid(), "Cannot create the Library fixture");
  const QString library = canonical(directory);
  buildLibrary(library);
  RootsScope scope(context, {{"library", {library, false}}});
  const QJsonArray items =
      result(dispatcher, "library.list", {{"kind", "template"}})
          .value("items")
          .toArray();
  QHash<QString, QJsonObject> rows;
  for (const QJsonValue& item : items)
    rows.insert(item.toObject().value("name").toString(), item.toObject());
  const auto status = [&rows](const QString& name) {
    return rows.value(name).value("identity").toObject().value("status");
  };
  require(status("Rig") == "duplicate_id" && status("Copy") == "duplicate_id" &&
              status("Legacy") == "not_recorded" &&
              status("Broken") == "malformed" &&
              status("Pose") == "kind_mismatch",
          "Sidecar identities are reported without inventing any");
  require(rows.value("Legacy")
              .value("identity")
              .toObject()
              .value("item_id")
              .isNull(),
          "A legacy item has no invented id");
  require(rows.value("Rig").value("thumbnail") == "present" &&
              rows.value("Copy").value("thumbnail") == "invalid" &&
              rows.value("Legacy").value("thumbnail") == "missing",
          "Thumbnail states come from a header check");
  const QJsonObject thumbnail = result(dispatcher, "library.thumbnail",
                                       {{"kind", "template"}, {"name", "Rig"}});
  require(thumbnail.value("width") == 4 && thumbnail.value("height") == 3,
          "Template icons are re-encoded at their size");
  failure(dispatcher, "library.thumbnail",
          {{"kind", "template"}, {"name", "Legacy"}}, "asset_unavailable");
  failure(dispatcher, "library.thumbnail",
          {{"kind", "template"}, {"name", "Rig"}, {"pose_index", 0}},
          "invalid_params");
  require(result(dispatcher, "library.list",
                 {{"kind", "template"}, {"name_contains", "LEG"}})
                  .value("total") == 1,
          "Name filters match folded names");
}

}  // namespace

void runAutomationSharedTests(Context& context) {
  Dispatcher dispatcher(context);
  checkGrammar();
  checkStudio(context, dispatcher);
  checkLibrary(context, dispatcher);
  std::cerr << "automation-shared-test: PASS\n";
}

}  // namespace GroundzeroAutomation
