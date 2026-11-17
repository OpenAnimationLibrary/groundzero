#include "automation_text.h"
#include "typetoolvector.h"

#include "timage_io.h"
#include "tlevel_io.h"
#include "tlevel.h"
#include "tpalette.h"
#include "tregion.h"
#include "tstroke.h"
#include "toonz/toonzscene.h"
#include "toonz/txshsimplelevel.h"

#include <QDir>
#include <QFontDatabase>
#include <QJsonDocument>

#include <functional>
#include <memory>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

QJsonObject nativeFont() {
  const QFontDatabase database;
  for (const auto& family : {"Arial", "DejaVu Sans", "Liberation Sans"}) {
    if (!database.families().contains(family)) continue;
    const auto styles = database.styles(family);
    for (const auto& style : {"Regular", "Book"})
      if (styles.contains(style))
        return {{"family", family}, {"style", style}, {"size_points", 32}};
  }
  throw std::runtime_error(
      "Native text fixtures require Arial, DejaVu Sans or Liberation Sans");
}

QJsonObject target(Context& context, Document& document) {
  const auto level =
      executeScene(
          context, "level.create",
          {{"document_id", document.id},
           {"name", QString("Text fixture %1").arg(document.levels.size())},
           {"format", "vector"}})
          .toObject();
  return {{"document_id", document.id},
          {"expected_revision", document.revisionText()},
          {"operation_id", "native-text"},
          {"level_id", level.value("level_id")},
          {"drawing_id", QJsonObject{{"number", 1}, {"suffix", ""}}},
          {"text", "OB"},
          {"font", nativeFont()},
          {"baseline", QJsonObject{{"x", 0}, {"y", 0}}},
          {"style_id", 1}};
}

TXshSimpleLevel* levelFor(Document& document, const QJsonObject& params) {
  return document.level(params.value("level_id").toString())->getSimpleLevel();
}

void rejects(Context& context, Document& document, const QJsonObject& params,
             const QString& code) {
  auto* level         = levelFor(document, params);
  const int before    = level->getFrameCount();
  const auto revision = document.revisionText();
  const auto palette  = level->getPalette()->getStyle(1)->getMainColor();
  context.resetMutationStarted();
  try {
    executeText(context, "vector.text.create", params);
  } catch (const Error& error) {
    require(error.code == code,
            qPrintable("Wrong text error: " + error.code + " expected " + code +
                       " message " + error.message));
    require(!context.mutationStarted(),
            "Rejected text crossed the publication boundary");
    require(before == level->getFrameCount(),
            "Rejected text published a frame");
    require(revision == document.revisionText(),
            "Rejected text changed document revision");
    require(palette == level->getPalette()->getStyle(1)->getMainColor(),
            "Rejected text changed style");
    return;
  }
  throw std::runtime_error("Invalid text unexpectedly succeeded");
}

void fontDiscovery(Context& context) {
  const auto result =
      executeText(context, "text.fonts",
                  {{"family_prefix", ""}, {"offset", 0}, {"limit", 64}})
          .toObject();
  require(result.value("scope") == "worker_host",
          "Font discovery scope differs");
  const auto fonts = result.value("families").toArray();
  require(!fonts.isEmpty() && fonts.size() <= 64,
          "Font discovery page is empty or oversized");
  QString previous;
  for (const auto& value : fonts) {
    const auto family = value.toObject().value("family").toString();
    require(previous.isEmpty() || previous < family,
            "Fonts are not sorted uniquely");
    previous = family;
  }
  const auto empty = executeText(context, "text.fonts",
                                 {{"family_prefix", "MCP-no-such-font-012345"},
                                  {"offset", 0},
                                  {"limit", 1}})
                         .toObject();
  require(empty.value("families").toArray().isEmpty(),
          "Missing prefix returned fonts");
  require(empty.value("next_offset").isNull(),
          "Empty fonts page has a next offset");
}

void inputRejections(Context& context, Document& document) {
  const auto params = target(context, document);
  auto changed      = params;
  changed.insert("text", QString(1025, 'A'));
  rejects(context, document, changed, "resource_limit");
  changed.insert("text", QString(32, '\n'));
  rejects(context, document, changed, "resource_limit");
  for (const auto& text :
       {QString(QChar(0xd800)), QString(QChar(0xdc00)), QString(QChar(0)),
        QString(QChar(0x2028)), QString("a\rb")}) {
    changed.insert("text", text);
    rejects(context, document, changed, "invalid_params");
  }
  changed.insert("text", " \t\n ");
  rejects(context, document, changed, "unsupported_outline");
  changed   = params;
  auto font = changed.value("font").toObject();
  font.insert("family", "MCP font must not silently substitute");
  changed.insert("font", font);
  rejects(context, document, changed, "font_not_found");
  font = params.value("font").toObject();
  font.insert("style", "MCP absent style");
  changed.insert("font", font);
  rejects(context, document, changed, "font_not_found");
}

void nativeRejections(Context& context, Document& document) {
  auto params = target(context, document);
  auto* level = levelFor(document, params);
  level->getPalette()->setIsLocked(true);
  rejects(context, document, params, "locked");
  level->getPalette()->setIsLocked(false);
  level->setIsReadOnly(true);
  rejects(context, document, params, "locked");
  level->setIsReadOnly(false);
  params.insert("style_id", 4095);
  rejects(context, document, params, "invalid_params");
  params.insert("style_id", 1);
  params.insert("baseline", QJsonObject{{"x", 32768}, {"y", 0}});
  rejects(context, document, params, "resource_limit");
  params.insert("baseline", QJsonObject{{"x", 0}, {"y", 0}});
  const uint unassigned[] = {0x10ffff};
  params.insert("text", QString::fromUcs4(unassigned, 1));
  rejects(context, document, params, "missing_glyph");
}

void shapingAndGeometryBounds(Context& context, Document& document) {
  const QStringList textCases{"OB",
                              "ffi",
                              QString::fromUtf8("Oé\nBΩ"),
                              QString::fromUtf8("é"),
                              "A B",
                              "A\tB",
                              QString("A") + QChar(0x200d) + "B",
                              QString(QChar(0x200e)) + "AB"};
  for (const auto& text : textCases) {
    auto params = target(context, document);
    params.insert("text", text);
    const auto result =
        executeText(context, "vector.text.create", params).toObject();
    require(!result.value("stroke_indices").toArray().isEmpty(),
            "Shaped text lost native geometry");
  }
  auto params = target(context, document);
  params.insert("text", QString(1024, 'O'));
  rejects(context, document, params, "resource_limit");
}

void classificationRejection(Context& context, Document& document) {
  const QFontDatabase database;
  if (database.families().contains("Apple Color Emoji")) {
    auto emoji = target(context, document);
    emoji.insert("font", QJsonObject{{"family", "Apple Color Emoji"},
                                     {"style", "Regular"},
                                     {"size_points", 32}});
    emoji.insert("text", QString::fromUtf8("🙂"));
    rejects(context, document, emoji, "unsupported_outline");
  }
  if (!database.families().contains("PingFang SC")) return;
  if (nativeFont().value("family") == "Arial") {
    auto fallback = target(context, document);
    fallback.insert("text", QString::fromUtf8("中"));
    rejects(context, document, fallback, "font_fallback");
  }
  auto params = target(context, document);
  params.insert("font", QJsonObject{{"family", "PingFang SC"},
                                    {"style", "Regular"},
                                    {"size_points", 32}});
  params.insert("text", QString::fromUtf8("中"));
  rejects(context, document, params, "unsupported_outline");
}

void assetAndFrameLimits(Context& context) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  auto params    = target(context, document);
  auto* level    = levelFor(document, params);
  for (int number = 1; number <= 4095; ++number) {
    TVectorImageP image = new TVectorImage;
    image->setPalette(level->getPalette());
    level->setFrame(TFrameId(number), image);
  }
  params.insert("drawing_id", QJsonObject{{"number", 4096}, {"suffix", ""}});
  rejects(context, document, params, "resource_limit");
  TVectorImageP image = new TVectorImage;
  image->setPalette(level->getPalette());
  level->setFrame(TFrameId(4096), image);
  params.insert("drawing_id", QJsonObject{{"number", 4097}, {"suffix", ""}});
  rejects(context, document, params, "resource_limit");
  context.removeDocument(document.id);
}

void sharedInspectionPrecedesPainting() {
  TypeToolText::TypeToolTextEntry entry;
  const auto font = nativeFont();
  entry.source    = "OB";
  entry.font = {font.value("family").toString(), font.value("style").toString(),
                "32"};
  const auto ordinary  = TypeToolText::buildOutlineLayout(entry);
  bool observed        = false;
  const auto inspected = TypeToolText::inspectOutlineLayout(
      entry, [&](const QTextDocument&) { observed = true; });
  require(observed, "Native layout inspection was not called");
  require(
      ordinary.paths == inspected.paths && ordinary.bounds == inspected.bounds,
      "Inspection changed the existing Type Tool layout");
  struct CancelInspection {};
  bool cancelled = false;
  try {
    TypeToolText::vectorizeOutlineLayout(
        inspected, 1, 1, [&](const TVectorImageP& image) {
          require(image->getRegionCount() > 0,
                  "Inspection precedes region discovery");
          for (UINT index = 0; index < image->getRegionCount(); ++index)
            require(image->getRegion(index)->getStyle() == 0,
                    "Native fill painting ran before bounded inspection");
          throw CancelInspection();
        });
  } catch (const CancelInspection&) {
    cancelled = true;
  }
  require(cancelled, "Converter ignored pre-paint inspection refusal");
}

void saveReopenAndEdit(Context& context, Document& document) {
  const auto params = target(context, document);
  context.resetMutationStarted();
  const auto result =
      executeText(context, "vector.text.create", params).toObject();
  require(context.mutationStarted(),
          "Text publication omitted its mutation marker");
  auto* level         = levelFor(document, params);
  TVectorImageP image = level->getFrame(TFrameId(1), false);
  require(image && image->getStrokeCount() > 1,
          "Text did not create editable contours");
  require(result.value("geometry_revision").isString(),
          "Geometry revision is not a string");
  require(result.value("group_index") == 0,
          "Text contours lack a flat native group");
  require(result.value("font_runs").toArray().size() > 0,
          "Text omitted resolved font evidence");
  const TFilePath path(
      QDir(context.workspace()).filePath("native-text.pli").toStdWString());
  {
    TLevelWriterP writer(path);
    writer->setPalette(level->getPalette());
    writer->getFrameWriter(TFrameId(1))->save(image);
  }
  TLevelReaderP reader(path);
  auto loaded            = reader->loadInfo();
  TVectorImageP reopened = reader->getFrameReader(TFrameId(1))->load();
  reopened->setPalette(loaded->getPalette());
  require(reopened->getStrokeCount() == image->getStrokeCount(),
          "PLI lost text contours");
  for (UINT index = 0; index < image->getStrokeCount(); ++index) {
    require(reopened->getStroke(index)->isSelfLoop(),
            "PLI text contour is not closed");
    require(reopened->getStroke(index)->getStyle() == 1,
            "PLI changed text contour style");
  }
  level->setFrame(TFrameId(1), reopened);
  auto edit = params;
  edit.insert("coordinate_space", "drawing_native");
  edit.insert("sharing", "all_exposures");
  edit.insert("edits",
              QJsonArray{QJsonObject{{"action", "delete"}, {"index", 0}}});
  const auto count = reopened->getStrokeCount();
  executeDrawing(context, "vector.edit", edit);
  TVectorImageP edited = level->getFrame(TFrameId(1), false);
  require(edited->getStrokeCount() == count - 1,
          "Reopened text is not editable vector geometry");
  rejects(context, document, params, "already_exists");
}

}  // namespace

void runAutomationTextTests(Context& context) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  assetAndFrameLimits(context);
  sharedInspectionPrecedesPainting();
  fontDiscovery(context);
  inputRejections(context, document);
  nativeRejections(context, document);
  shapingAndGeometryBounds(context, document);
  classificationRejection(context, document);
  saveReopenAndEdit(context, document);
  context.removeDocument(document.id);
}

}  // namespace GroundzeroAutomation
