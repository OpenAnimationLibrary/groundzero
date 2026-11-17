#include "automation_production.h"

#include "tfx.h"
#include "tpalette.h"
#include "tstream.h"
#include "tvectorimage.h"
#include "toonz/fxdag.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/sceneresources.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct Fixture final {
  Context& context;
  Document& document;

  explicit Fixture(Context& owner)
      : context(owner)
      , document(owner.addDocument(std::make_unique<ToonzScene>())) {
    document.scene->setScenePath(context.project()->getScenesPath() +
                                 "inspection.tnz");
    document.revision = 7;
  }
  ~Fixture() { context.removeDocument(document.id); }

  TXshSimpleLevel* level(const std::wstring& name) {
    auto* result =
        document.scene->createNewLevel(PLI_XSHLEVEL, name)->getSimpleLevel();
    require(result != nullptr, "Cannot create production fixture level");
    result->setFrame(TFrameId(1), new TVectorImage);
    return result;
  }

  QJsonObject read(const QString& method, QJsonObject options = {}) {
    options.insert("document_id", document.id);
    if (!options.contains("expected_revision"))
      options.insert("expected_revision", document.revisionText());
    return executeProduction(context, method, options).toObject();
  }
};

Error rejected(Fixture& fixture, const QString& method,
               QJsonObject params = {}) {
  try {
    fixture.read(method, params);
  } catch (const Error& error) {
    return error;
  }
  throw std::runtime_error(
      "Production inspection unexpectedly accepted invalid state");
}

QJsonArray rows(const QJsonObject& report) {
  return report.value("items").toArray();
}

bool hasCode(const QJsonObject& report, const QString& code) {
  for (const auto& value : rows(report))
    if (value.toObject().value("code") == code) return true;
  return false;
}

QByteArray nativeState(Fixture& fixture) {
  const QString path =
      fixture.context.workspace() + "/production-native-state.xml";
  {
    TOStream output{TFilePath(path)};
    require(bool(output), "Cannot serialize native inspection fixture state");
    auto* levels = fixture.document.scene->getLevelSet();
    for (int index = 0; index < levels->getLevelCount(); ++index)
      output << levels->getLevel(index);
    output << fixture.document.scene->getTopXsheet();
  }
  QFile input(path);
  require(input.open(QIODevice::ReadOnly),
          "Cannot inspect serialized native state");
  return input.readAll();
}

void pagingAndMetadata(Context& context) {
  Fixture fixture(context);
  auto* first  = fixture.level(L"First");
  auto* second = fixture.level(L"Second");
  first->setCastTags({"hero", QString::fromUtf8("caf\xc3\xa9")});
  first->setCastCover(TFrameId(1));
  TXshLevel::LibraryProvenance origin;
  origin.relationship    = TXshLevel::LibraryProvenance::LocalCopy;
  origin.itemId          = "recorded-id";
  origin.kind            = "symbol";
  origin.itemPath        = "recorded/library/item";
  origin.sourceAssetPath = "recorded/source.pli";
  origin.revision        = 3;
  first->setLibraryProvenance(origin);
  const auto before = nativeState(fixture);
  const bool dirty  = first->getProperties()->getDirtyFlag();
  const auto a      = fixture.read("cast.list", {{"limit", 1}});
  const auto b      = fixture.read("cast.list", {{"offset", 1}, {"limit", 1}});
  require(rows(a).size() == 1 && rows(b).size() == 1,
          "Native cast pagination lost records");
  const auto item = rows(a).first().toObject();
  require(item.value("native_cast_id") == first->getCastId(),
          "Cast read invented native identity");
  require(item.value("tags").toArray() ==
              QJsonArray::fromStringList(first->getCastTags()),
          "Cast read lost native tags");
  require(
      item.value("library").toObject().value("relationship") == "local_copy",
      "Cast read lost actual stored Library provenance");
  require(rows(b).first().toObject().value("level_id") ==
              fixture.document.idForLevel(second),
          "Native cast pagination changed order");
  require(a.value("next_offset") == 1 && b.value("next_offset").isNull(),
          "Cast page boundary is incorrect");
  require(rows(fixture.read("cast.list", {{"offset", 2}})).isEmpty(),
          "Final empty page was rejected");
  require(
      rejected(fixture, "cast.list", {{"offset", 3}}).code == "invalid_params",
      "Past-end page accepted");
  require(nativeState(fixture) == before &&
              first->getProperties()->getDirtyFlag() == dirty,
          "Read changed native serialized contents or dirty state");
  require(fixture.document.revision == 7, "Read changed document revision");
}

void missingAndRuns(Context& context) {
  Fixture fixture(context);
  auto* level = fixture.level(L"Exposed");
  fixture.level(L"Unused");
  auto* sheet = fixture.document.scene->getTopXsheet();
  sheet->setCell(0, 0, TXshCell(level, TFrameId(1)));
  sheet->setCell(1, 0, TXshCell(level, TFrameId(8)));
  sheet->setCell(2, 0, TXshCell(level, TFrameId(8)));
  sheet->setCell(4, 0, TXshCell(level, TFrameId(1)));
  const auto before = nativeState(fixture);
  const auto uses   = rows(fixture.read("cast.usage"));
  require(uses.size() == 2, "Drawing changes split contiguous level usage");
  require(uses.first().toObject().value("last_frame") == 2 &&
              uses.last().toObject().value("first_frame") == 4,
          "Usage merged a gap or lost the inclusive exposure range");
  const auto checks = fixture.read("scene.validate");
  require(hasCode(checks, "missingDrawing") && hasCode(checks, "unusedLevel"),
          "Basic scene checks omitted missing or unused artwork");
  const auto first = rows(checks).first().toObject();
  require(first.value("first_frame") == 1 && first.value("last_frame") == 2,
          "Missing drawing did not preserve its exact local range");
  require(nativeState(fixture) == before,
          "Basic checks mutated native scene contents");
}

void cellAndRecordLimits(Context& context) {
  Fixture fixture(context);
  auto* level = fixture.level(L"Long hold");
  fixture.level(L"Unknown unused");
  std::vector<TXshCell> cells(100001, TXshCell(level, TFrameId(1)));
  auto* sheet = fixture.document.scene->getTopXsheet();
  require(sheet->setCells(0, 0, static_cast<int>(cells.size()), cells.data()),
          "Cannot build long hold");
  auto report = fixture.read("cast.usage");
  require(!report.value("traversal_complete").toBool() &&
              report.value("reasons").toArray().contains("cell_limit"),
          "Cell limit certified a complete traversal");
  require(report.value("scanned").toObject().value("scanned_cells") == 100000,
          "Cell counter exceeded the advertised ceiling");
  require(rows(report).first().toObject().value("last_frame") == 99999,
          "Partial hold did not stop at the last inspected frame");
  require(!hasCode(fixture.read("scene.validate"), "unusedLevel"),
          "Incomplete scan made an unused-level claim");
  sheet->removeCells(0, 0, 100001);
  for (int index = 0; index < 4097; ++index)
    sheet->setCell(index * 2, 0, TXshCell(level, TFrameId(1)));
  report = fixture.read("cast.usage", {{"offset", 4096}});
  require(report.value("discovered_records") == 4096 && rows(report).isEmpty(),
          "Result cap failed to retain an exact finite prefix");
  require(report.value("reasons").toArray().contains("record_limit") &&
              report.value("next_offset").isNull(),
          "Truncation was confused with another page");
}

void levelPrefixLimit(Context& context) {
  Fixture fixture(context);
  TXshSimpleLevel* last = nullptr;
  for (int index = 0; index < 257; ++index)
    last = fixture.level(L"Level " + std::to_wstring(index));
  fixture.document.scene->getTopXsheet()->setCell(0, 0,
                                                  TXshCell(last, TFrameId(1)));
  const auto checks = fixture.read("scene.validate");
  require(checks.value("reasons").toArray().contains("level_limit"),
          "Level limit was omitted");
  require(
      !hasCode(checks, "invalidExposure") && !hasCode(checks, "unusedLevel"),
      "Unscanned cast membership became a false invalid or unused issue");
  require(checks.value("scanned").toObject().value("levels") == 256,
          "Cast scan exceeded its level budget");
}

void unsupportedNativeContent(Context& context) {
  Fixture fixture(context);
  auto* child =
      fixture.document.scene->createNewLevel(CHILD_XSHLEVEL, L"Nested")
          ->getChildLevel();
  fixture.document.scene->getTopXsheet()->setCell(0, 0,
                                                  TXshCell(child, TFrameId(1)));
  TFxP cache = TFx::create("STD_particleCacheFx");
  require(bool(cache),
          "Native cache factory unavailable in production fixture");
  auto* resource = dynamic_cast<SceneFxResource*>(cache.getPointer());
  require(resource != nullptr, "Native cache lacks its resource interface");
  resource->setResourcePath(
      TFilePath("/forbidden-production-fixture/cache.otpc"));
  fixture.document.scene->getTopXsheet()->getFxDag()->getInternalFxs()->addFx(
      cache.getPointer());
  const auto checks = fixture.read("scene.validate");
  require(
      hasCode(checks, "unsupportedLevel") && hasCode(checks, "unsupportedFx"),
      "Native unsupported content was silently admitted");
  require(
      checks.value("reasons").toArray().contains("unsupported_nested_sheet"),
      "Nested coverage was falsely complete");
  const auto dependencies = fixture.read("scene.dependencies");
  const auto item         = rows(dependencies).last().toObject();
  require(item.value("role") == "fx_resource" &&
              item.value("availability") == "unsupported",
          "External FX dependency was omitted or probed as admitted");
  require(
      item.value("resolved_path").isNull() && item.value("size_bytes").isNull(),
      "Unsupported FX returned unauthorized file metadata");
}

void writeFile(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write("native") == 6,
          "Cannot create authorized metadata fixture");
}

void dependencyAuthority(Context& context) {
  Fixture fixture(context);
  auto* level        = fixture.level(L"Paths");
  const QString path = context.workspace() + "/observed.pli";
  writeFile(path);
  level->setPath(TFilePath(path), true);
  auto item = rows(fixture.read("scene.dependencies")).first().toObject();
  require(
      item.value("availability") == "present" && item.value("size_bytes") == 6,
      "Authorized file metadata was not observed");
  level->setPath(TFilePath(context.workspace() + "/missing.pli"), true);
  item = rows(fixture.read("scene.dependencies")).first().toObject();
  require(item.value("availability") == "missing",
          "Authorized missing leaf was not distinguished");
  level->setPath(TFilePath("+unknown/source.pli"), true);
  item = rows(fixture.read("scene.dependencies")).first().toObject();
  require(item.value("authority") == "unresolved",
          "Unknown project alias acquired authority");
  level->setPath(TFilePath("+drawings/sequence..png"), true);
  item = rows(fixture.read("scene.dependencies")).first().toObject();
  require(item.value("path_kind") == "sequence_pattern" &&
              item.value("availability") == "not_checked",
          "A sequence pattern was mistaken for a missing literal file");
  QTemporaryDir foreign;
  require(foreign.isValid(), "Cannot create private foreign fixture");
  const QString denied = foreign.filePath("foreign.pli");
  writeFile(denied);
  level->setPath(TFilePath(denied), true);
  item = rows(fixture.read("scene.dependencies")).first().toObject();
  require(item.value("availability") == "forbidden" &&
              item.value("resolved_path").isNull() &&
              item.value("size_bytes").isNull(),
          "Forbidden source leaked observed metadata");
  auto foreignProject = std::make_shared<TProject>();
  foreignProject->setFolder("drawings", TFilePath(foreign.path()));
  fixture.document.scene->setProject(foreignProject);
  level->setPath(TFilePath("+drawings/foreign.pli"), true);
  item = rows(fixture.read("scene.dependencies")).first().toObject();
  require(item.value("availability") == "forbidden" &&
              item.value("resolved_path").isNull(),
          "A foreign project alias borrowed the worker project's authority");
}

void linkedOriginsAndStaleReads(Context& context) {
  Fixture fixture(context);
  auto* level = fixture.level(L"Unadmitted linked origin");
  TXshLevel::LibraryProvenance origin;
  origin.relationship    = TXshLevel::LibraryProvenance::Linked;
  origin.sourceAssetPath = "/foreign-library-fixture/source.pli";
  level->setLibraryProvenance(origin);
  auto error = rejected(fixture, "cast.list", {{"expected_revision", "6"}});
  require(error.code == "stale_revision",
          "Stale revision did not precede native content inspection");
  error = rejected(fixture, "cast.list");
  require(
      error.code == "unsupported_level" &&
          error.details.value("level_id") == fixture.document.idForLevel(level),
      "Linked origin was fabricated or inspected instead of refused");
  require(
      level->getLibraryProvenance() == origin && fixture.document.revision == 7,
      "Linked-origin refusal mutated native provenance or revision");
}

constexpr int MetadataPadding = 4096 * 3 + 80 * 32;

QString paddingPart(int& remaining, int maximum) {
  const int size = std::min(remaining, maximum);
  remaining -= size;
  return QString(size, QChar('x'));
}

void paddedMetadata(TXshLevel* level, int padding) {
  TXshLevel::LibraryProvenance origin;
  origin.relationship    = TXshLevel::LibraryProvenance::LocalCopy;
  origin.kind            = "symbol";
  origin.revision        = 1;
  origin.itemId          = paddingPart(padding, 4096);
  origin.itemPath        = paddingPart(padding, 4096);
  origin.sourceAssetPath = paddingPart(padding, 4096);
  level->setLibraryProvenance(origin);
  QStringList tags;
  for (int index = 0; index < 32; ++index)
    tags.append(paddingPart(padding, 80));
  level->setCastTags(tags);
  require(padding == 0, "Byte fixture padding exceeds native field bounds");
}

bool pageFits(Fixture& fixture) {
  try {
    fixture.read("cast.list", {{"limit", 128}});
    return true;
  } catch (const Error& error) {
    require(error.code == "resource_limit" &&
                error.message.contains("response exceeds"),
            "Byte fixture failed for an unrelated limit");
    return false;
  }
}

void tailPadding(const std::vector<TXshSimpleLevel*>& levels, int padding) {
  paddedMetadata(levels.at(levels.size() - 2),
                 std::min(padding, MetadataPadding));
  paddedMetadata(levels.back(), std::max(0, padding - MetadataPadding));
}

void responseByteBoundary(Context& context) {
  Fixture fixture(context);
  std::vector<TXshSimpleLevel*> levels;
  while (levels.size() < 128) {
    auto* level = fixture.level(L"Large " + std::to_wstring(levels.size()));
    levels.push_back(level);
    paddedMetadata(level, MetadataPadding);
    if (!pageFits(fixture)) break;
  }
  require(levels.size() >= 2 && levels.size() < 128,
          "Byte boundary was not reached within record limits");
  int lower = 0, upper = 2 * MetadataPadding;
  tailPadding(levels, lower);
  require(pageFits(fixture), "Byte boundary has no valid lower fixture");
  while (lower < upper) {
    const int middle = lower + (upper - lower + 1) / 2;
    tailPadding(levels, middle);
    if (pageFits(fixture))
      lower = middle;
    else
      upper = middle - 1;
  }
  tailPadding(levels, lower);
  const auto result = fixture.read("cast.list", {{"limit", 128}});
  const QJsonObject envelope{{"id", QString(128, QChar(1))},
                             {"result", result}};
  require(QJsonDocument(envelope).toJson(QJsonDocument::Compact).size() + 1 ==
              1048576,
          "The accepted boundary did not include the worst escaped request ID "
          "and newline");
  tailPadding(levels, lower + 1);
  require(!pageFits(fixture),
          "One byte over the complete response limit was accepted");
  require(rows(fixture.read("cast.list", {{"limit", 1}})).size() == 1,
          "Reducing page size did not recover from the response byte bound");
  require(fixture.document.revision == 7,
          "Byte rejection changed native revision");
}

}  // namespace

void runAutomationProductionTests(Context& context) {
  const bool mutation = context.mutationStarted();
  pagingAndMetadata(context);
  missingAndRuns(context);
  cellAndRecordLimits(context);
  levelPrefixLimit(context);
  unsupportedNativeContent(context);
  dependencyAuthority(context);
  linkedOriginsAndStaleReads(context);
  responseByteBoundary(context);
  require(context.mutationStarted() == mutation,
          "Read inspection changed the worker mutation marker");
  std::cout << "PASS: native production inspection boundaries (8 groups)\n";
}

}  // namespace GroundzeroAutomation
