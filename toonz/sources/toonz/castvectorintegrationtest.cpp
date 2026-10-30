#include "castvectorintegrationtest.h"
#include "castbrowseractions.h"
#include "cellselection.h"
#include "filmstripcommand.h"
#include "iocommand.h"
#include "matchline.h"
#include "nativeuiintegrationtest.h"
#include "nativefiledialogtest.h"
#include "tapp.h"
#include "tdoubleparam.h"
#include "tools/strokeselection.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/morphhints.h"
#include "toonz/preferences.h"
#include "toonz/sceneproperties.h"
#include "toonz/scenechecker.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/functioncurveoperations.h"
#include "tlevel_io.h"
#include "toutputproperties.h"
#include "tpalette.h"
#include "tproperty.h"
#include "tstroke.h"
#include "tundo.h"
#include "tvectorimage.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QAbstractButton>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

void checkpoint(const char *phase) {
  std::cerr << "cast-vector-native: checkpoint: " << phase << std::endl;
}

void outputPropertiesOwnership() {
  TOutputProperties source;
  auto *sourceFormat = source.getFileFormatProperties("mov");
  const std::string propertyName = "Cast fixture output quality";
  sourceFormat->add(new TIntProperty(propertyName, 0, 100, 42));
  source.syncColorSettings(false);
  source.setNonlinearBpp(64);
  source.setFrameRate(30.);
  source.setAspectMode(TOutputProperties::AspectCrop);
  source.setAspectRatio(2.39);

  auto extensions = [](const TOutputProperties &properties) {
    std::vector<std::string> result;
    properties.getFileFormatPropertiesExtensions(result);
    return result;
  };
  auto quality = [&](TPropertyGroup *format) {
    auto *value = dynamic_cast<TIntProperty *>(format->getProperty(propertyName));
    require(value, "Output assignment lost its format property");
    return value;
  };
  auto expectCopy = [&](TOutputProperties &copy) {
    require(extensions(copy) == std::vector<std::string>{"mov"} &&
                !copy.isColorSettingsSynced() && copy.getNonlinearBpp() == 64 &&
                copy.getFrameRate() == 30. &&
                copy.getAspectMode() == TOutputProperties::AspectCrop &&
                copy.getAspectRatio() == 2.39,
            "Output assignment retained old formats or lost copied settings");
    auto *format = copy.getFileFormatProperties("mov");
    require(format != sourceFormat && quality(format)->getValue() == 42 &&
                copy.getBoardSettings() != source.getBoardSettings() &&
                &copy.getRenderSettings() != &source.getRenderSettings(),
            "Output copy shares owned settings with its source");
  };

  TOutputProperties constructed(source);
  expectCopy(constructed);
  quality(constructed.getFileFormatProperties("mov"))->setValue(17);
  require(quality(sourceFormat)->getValue() == 42,
          "Editing copied output properties changed the source");

  TOutputProperties assigned;
  assigned.getFileFormatProperties("png");
  assigned.getFileFormatProperties("tif");
  TOutputProperties disjoint;
  disjoint.getFileFormatProperties("jpg");
  for (int cycle = 0; cycle < 3; ++cycle) {
    assigned = source;
    expectCopy(assigned);
    auto *format = assigned.getFileFormatProperties("mov");
    const TOutputProperties &same = assigned;
    assigned = same;
    require(assigned.getFileFormatProperties("mov") == format &&
                quality(format)->getValue() == 42 &&
                !assigned.isColorSettingsSynced() && assigned.getNonlinearBpp() == 64,
            "Output self-assignment changed or released live settings");
    assigned = disjoint;
    require(extensions(assigned) == std::vector<std::string>{"jpg"},
            "Output assignment retained a format absent from the source");
    // Scene loading performs two consecutive assignments from defaults. A
    // format key left behind after the first reset used to be double-deleted.
    assigned = TOutputProperties();
    require(extensions(assigned).empty() && assigned.isColorSettingsSynced(),
            "Output reset retained old format ownership or color state");
    assigned = TOutputProperties();
    require(extensions(assigned).empty(), "Repeated output reset restored stale formats");
  }
}

class DialogDriver {
  QTimer m_timer, m_deadline;
public:
  bool expired = false;
  explicit DialogDriver(const std::function<void(QDialog *)> &action) {
    QObject::connect(&m_timer, &QTimer::timeout, [action]() {
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        action(dialog);
    });
    QObject::connect(&m_deadline, &QTimer::timeout, [this]() {
      expired = true;
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
        std::cerr << "cast-vector-native: modal deadline: "
                  << dialog->metaObject()->className() << " / "
                  << dialog->objectName().toStdString() << " / "
                  << dialog->windowTitle().toStdString() << std::endl;
        dialog->reject();
      }
    });
    m_timer.start(30);
    m_deadline.setSingleShot(true);
    m_deadline.start(15000);
  }
};

class NativeDialogsGuard {
  const bool m_previous = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
public:
  NativeDialogsGuard() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true); }
  ~NativeDialogsGuard() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, m_previous); }
};

void showProgressNextTurn(const QString &label) {
  QTimer::singleShot(0, [label]() {
    for (auto *widget : QApplication::topLevelWidgets())
      if (auto *progress = qobject_cast<QProgressDialog *>(widget))
        if (progress->labelText() == label) progress->show();
  });
}

const StrokeOpacity::Profile sourceProfile{
    {0., .15}, {.5, .3}, {.5, .85}, {1., .55}};
const StrokeOpacity::Profile replacementProfile{
    {0., 1.}, {.25, .5}, {1., 0.}};

TVectorImageP drawing(TPalette *palette, double offset,
                      const StrokeOpacity::Profile &profile, double opacity) {
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette);
  auto *stroke = new TStroke(std::vector<TThickPoint>{
      {offset, 0., 2.}, {offset + 5., 10., 2.}, {offset + 10., 0., 2.}});
  stroke->setStyle(1);
  stroke->setOpacity(opacity);
  require(stroke->setOpacityProfile(profile), "Cast/vector fixture rejected an opacity profile");
  image->addStroke(stroke);
  return image;
}

void writeLevel(const QString &path, double offset,
                const StrokeOpacity::Profile &profile, double opacity) {
  TPaletteP palette = new TPalette;
  TLevelWriterP writer{TFilePath(path)};
  for (int fid : {1, 3, 5})
    writer->getFrameWriter(TFrameId(fid))->save(
        drawing(palette.getPointer(), offset + (fid - 1) * 10., profile, opacity));
}

TVectorImageP image(TXshSimpleLevel *level, int fid) {
  TVectorImageP value = level->getFrame(TFrameId(fid), false);
  require(value && value->getStrokeCount() == 1, "Cast/vector drawing missing or changed");
  return value;
}

void expectDrawing(TXshSimpleLevel *level, int fid, double offset,
                   const StrokeOpacity::Profile &profile, double opacity) {
  const auto value = image(level, fid);
  const auto *stroke = value->getStroke(0);
  require(stroke->getControlPoint(0).x == offset &&
              stroke->getOpacityProfile() == profile && stroke->getOpacity() == opacity,
          "Cast/vector operation changed geometry, global opacity or envelope knots");
}

QJsonObject pair(TXshSimpleLevel *level, int first, int last) {
  return {{"kind", "pair"}, {"firstFrame", MorphHints::frame(TFrameId(first))},
          {"lastFrame", MorphHints::frame(TFrameId(last))},
          {"first", MorphHints::anchor(image(level, first), 0)},
          {"last", MorphHints::anchor(image(level, last), 0)}};
}

std::string hints(TXshSimpleLevel *level) {
  auto zone = pair(level, 1, 3);
  zone["kind"] = "zone";
  zone["firstMembers"] = QJsonArray{zone["first"]};
  zone["lastMembers"] = QJsonArray{zone["last"]};
  zone["firstRect"] = MorphHints::rect(image(level, 1)->getBBox());
  zone["lastRect"] = MorphHints::rect(image(level, 3)->getBBox());
  return MorphHints::encode(QJsonArray{pair(level, 1, 3), zone, pair(level, 1, 5)});
}

void expectHints(TXshSimpleLevel *level, int first, int last, bool active) {
  const auto pairs = StrokeSelection::getInbetweenStrokePairs(
      level, TFrameId(first), TFrameId(last));
  const auto zones = StrokeSelection::getInbetweenZoneHints(
      level, TFrameId(first), TFrameId(last));
  require(active ? (pairs == std::vector<std::pair<int, int>>{{0, 0}} && zones.size() == 1)
                 : (pairs.empty() && zones.empty()),
          "Cast/vector morph anchors resolved against the wrong drawings");
}

void expectMetadata(TXshSimpleLevel *level, const QStringList &tags,
                    const std::string &storedHints, int cover = 5) {
  require(level->getCastTags() == tags && level->hasCastCover() &&
              level->getCastCover() == TFrameId(cover) && level->getMorphHints() == storedHints,
          "Cast/vector operation lost tags, cover or scene-owned morph hints");
}

void clean(TXshSimpleLevel *level) {
  level->getProperties()->setDirtyFlag(false);
  if (level->getPalette()) level->getPalette()->setDirtyFlag(false);
}
}  // namespace

// Runs after the independent native fixtures, with a fresh scene and real PLI
// files. Every edit below uses the artist-facing native command/undo path.
void runCastVectorIntegrationTest(QMainWindow *window) {
  auto *app = TApp::instance();
  QString resetError;
  if (!resetNativeUiIntegrationScene(&resetError))
    throw std::runtime_error(resetError.toStdString());
  checkpoint("settling clean scene");
  QApplication::processEvents();
  checkpoint("checking output-setting ownership before scene reload");
  outputPropertiesOwnership();
  checkpoint("creating native PLI fixtures");
  auto *scene = app->getCurrentScene()->getScene();
  QTemporaryDir temporary;
  require(temporary.isValid(), "Cast/vector temporary directory unavailable");
  const QString originalDir = temporary.path() + "/external";
  const QString replacementDir = temporary.path() + "/replacement";
  const QString localDir = temporary.path() + "/local";
  for (const auto &dir : {originalDir, replacementDir, localDir})
    require(QDir().mkpath(dir), "Cast/vector cannot create fixture folders");
  writeLevel(originalDir + "/figure.pli", 0., sourceProfile, .327);
  checkpoint("original PLI written");
  writeLevel(replacementDir + "/figure.pli", 100., replacementProfile, .7);
  checkpoint("replacement PLI written; loading scene level");
  scene->setScenePath(TFilePath(localDir + "/scene.tnz"));
  auto *level = scene->createNewLevel(PLI_XSHLEVEL, L"Cast vector fixture")->getSimpleLevel();
  level->setPath(TFilePath(originalDir + "/figure.pli"));
  level->load();
  clean(level);
  app->getCurrentLevel()->setLevel(level);
  checkpoint("building authoring metadata and exposures");
  const QStringList tags{QString::fromUtf8("hero λ"), "review \"ink\" & line"};
  const std::string storedHints = hints(level);
  level->setCastTags(tags);
  level->setCastCover(TFrameId(5));
  level->setMorphHints(storedHints);
  const QString sourceId = level->getCastId();
  using Origin = TXshLevel::LibraryProvenance;
  // Actual Library insertion is covered by libraryprovenanceintegrationtest.
  // Seed its persisted output here to exercise the subsequent vector commands.
  Origin sourceOrigin;
  sourceOrigin.itemId = "combined-vector-origin";
  sourceOrigin.itemPath = originalDir + "/source-rig.tnz";
  sourceOrigin.kind = "rig";
  sourceOrigin.revision = 3;
  sourceOrigin.sourceAssetPath = originalDir + "/figure.pli";
  sourceOrigin.relationship = Origin::Linked;
  level->setLibraryProvenance(sourceOrigin);
  auto expectSource = [&](Origin::Relationship relation) {
    Origin expected = sourceOrigin;
    expected.relationship = relation;
    require(level->getCastId() == sourceId && level->getLibraryProvenance() == expected &&
                level->getLibraryRelationship() == relation && level->sourceExpectedOnDisk(),
            "Cast/vector operation changed asset identity or source ownership");
  };
  expectSource(Origin::Linked);
  auto *sheet = scene->getXsheet();
  for (int row = 0; row < 3; ++row)
    sheet->setCell(row, 0, TXshCell(level, TFrameId(1 + row * 2)));
  expectDrawing(level, 1, 0., sourceProfile, .327);
  expectHints(level, 1, 3, true);

  // Native scene attributes and native PLI envelope data must survive together.
  checkpoint("saving native scene metadata");
  const TFilePath saved(localDir + "/saved.tnz");
  scene->saveCopy(saved);
  {
    checkpoint("reloading native scene resources");
    ToonzScene loaded;
    TProjectManager::instance()->initializeScene(&loaded);
    loaded.loadTnzFile(saved);
    loaded.loadResources(false);
    auto *asset = loaded.getLevelSet()->getLevel(level->getName());
    require(asset && asset->getSimpleLevel(), "Cast/vector scene reload lost level identity");
    auto *reopened = asset->getSimpleLevel();
    require(reopened->getCastId() == sourceId && reopened->getLibraryProvenance() == sourceOrigin &&
                reopened->getLibraryRelationship() == Origin::Linked && reopened->sourceExpectedOnDisk(),
            "Cast/vector scene reload changed asset UUID or Library provenance");
    expectMetadata(reopened, tags, storedHints);
    expectHints(reopened, 1, 3, true);
    expectDrawing(reopened, 1, 0., sourceProfile, .327);
    expectDrawing(reopened, 5, 40., sourceProfile, .327);
  }

  checkpoint("relinking PLI drawings");
  NativeDialogsGuard dialogs;
  const TFilePath original = level->getPath();
  {
    QString failure;
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        NativeUiTest::acceptDirectory(files, replacementDir);
      } else if (dialog->objectName() == "CastRelinkReview") {
        dialog->accept();
        showProgressNextTurn("Validating replacement drawings…");
      } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        failure = message->text();
        message->reject();
      }
    });
    CastBrowserActions::relink(window, {level});
    require(!driver.expired && failure.isEmpty() &&
                scene->decodeFilePath(level->getPath()) == TFilePath(replacementDir + "/figure.pli"),
            "Cast/vector native relink did not apply");
  }
  checkpoint("checking relink undo, redo and cache eviction");
  expectSource(Origin::Replaced);
  expectMetadata(level, tags, storedHints);
  expectDrawing(level, 1, 100., replacementProfile, .7);
  expectHints(level, 1, 3, false);
  TUndoManager::manager()->undo();
  expectSource(Origin::Linked);
  expectMetadata(level, tags, storedHints);
  expectDrawing(level, 1, 0., sourceProfile, .327);
  expectHints(level, 1, 3, true);
  TUndoManager::manager()->redo();
  expectSource(Origin::Replaced);
  level->invalidateFrames();
  expectDrawing(level, 5, 140., replacementProfile, .7);
  expectHints(level, 1, 3, false);
  TUndoManager::manager()->undo();
  expectSource(Origin::Linked);
  level->invalidateFrames();
  expectDrawing(level, 5, 40., sourceProfile, .327);
  expectHints(level, 1, 3, true);

  // Copying external native files retains envelopes and valid authoring hints;
  // undo restores links while leaving the explicitly published files intact.
  checkpoint("making a local PLI copy");
  require(CastBrowserActions::external(scene, level), "Cast/vector source is not external");
  {
    QString failure;
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        NativeUiTest::acceptDirectory(files, localDir);
        showProgressNextTurn("Preparing local asset copies…");
      } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        if (message->standardButtons().testFlag(QMessageBox::Yes))
          message->button(QMessageBox::Yes)->click();
        else { failure = message->text(); message->reject(); }
      }
    });
    CastBrowserActions::makeLocal(window, {level});
    require(!driver.expired && failure.isEmpty() && !CastBrowserActions::external(scene, level),
            "Cast/vector native local copy did not apply");
  }
  checkpoint("checking local-copy undo, redo and cache eviction");
  expectSource(Origin::LocalCopy);
  const TFilePath copied = scene->decodeFilePath(level->getPath());
  require(QFileInfo::exists(copied.getQString()), "Cast/vector copied PLI does not exist");
  expectMetadata(level, tags, storedHints);
  expectHints(level, 1, 3, true);
  level->invalidateFrames();
  expectDrawing(level, 1, 0., sourceProfile, .327);
  TUndoManager::manager()->undo();
  expectSource(Origin::Linked);
  require(level->getPath() == original && QFileInfo::exists(copied.getQString()),
          "Cast/vector local-copy undo lost original link or removed copied file");
  expectMetadata(level, tags, storedHints);
  TUndoManager::manager()->redo();
  expectSource(Origin::LocalCopy);
  require(scene->decodeFilePath(level->getPath()) == copied,
          "Cast/vector local-copy redo created a new destination");
  level->invalidateFrames();
  expectDrawing(level, 3, 20., sourceProfile, .327);
  expectHints(level, 1, 3, true);

  // Matching PLI bytes alone are insufficient: a different scene-owned morph
  // plan must block consolidation. Once plans match, the explicit reviewed
  // command preserves the chosen cover, envelopes and undoable exposures.
  checkpoint("preparing duplicate metadata");
  auto *duplicate = scene->createNewLevel(PLI_XSHLEVEL, L"Cast vector duplicate")->getSimpleLevel();
  duplicate->setPath(copied);
  duplicate->load();
  duplicate->setPalette(level->getPalette());
  duplicate->clonePropertiesFrom(level);
  duplicate->setCastTags({"duplicate-tag"});
  duplicate->setCastCover(TFrameId(1));
  const QString duplicateId = duplicate->getCastId();
  require(duplicateId != sourceId, "Distinct vector asset reused source UUID");
  const QList<TSceneProperties::CastCollection> collections{
      {QUuid::createUuid().toString(QUuid::WithoutBraces), "Duplicate only", {duplicateId}},
      {QUuid::createUuid().toString(QUuid::WithoutBraces), "Both drawings", {sourceId, duplicateId}}};
  scene->getProperties()->setCastCollections(collections);
  {
    const bool dirty = app->getCurrentScene()->getDirtyFlag();
    const int history = TUndoManager::manager()->getCurrentHistoryIndex();
    int collectionWarnings = 0, provenanceNotes = 0, identityNotes = 0;
    for (const auto &issue : SceneChecker::compatibilityReport(scene)) {
      if (issue.severity == SceneChecker::Warning && issue.text.contains("castCollections"))
        ++collectionWarnings;
      if (issue.severity == SceneChecker::Info && issue.levelName == level->getName() &&
          issue.text.contains("recorded Library source attribution")) ++provenanceNotes;
      if (issue.severity == SceneChecker::Info && issue.text.contains("Scene Cast asset identities"))
        ++identityNotes;
    }
    require(collectionWarnings == 1 && provenanceNotes == 1 && identityNotes == 1 &&
                scene->getProperties()->getCastCollections() == collections &&
                app->getCurrentScene()->getDirtyFlag() == dirty &&
                TUndoManager::manager()->getCurrentHistoryIndex() == history,
            "Compatibility inventory omitted new Cast metadata or modified authoring state");
  }
  auto consolidatedCollections = collections;
  consolidatedCollections[0].members = QStringList{sourceId};
  consolidatedCollections[1].members = QStringList{sourceId};
  clean(duplicate);
  sheet->setCell(4, 2, TXshCell(duplicate, TFrameId(3)));
  auto consolidate = [&](bool expectRejected) {
    bool reviewed = false, confirmed = false, rejected = false;
    const int before = TUndoManager::manager()->getCurrentHistoryIndex();
    DialogDriver driver([&](QDialog *dialog) {
      if (dialog->windowTitle() == "Duplicate Asset Review") {
        if (reviewed) { dialog->reject(); return; }
        if (auto *button = dialog->findChild<QPushButton *>("CastReviewConsolidation")) {
          reviewed = true;
          // The click opens another modal event loop. A timer cannot reenter
          // its own timeout callback to drive that nested dialog.
          QMetaObject::invokeMethod(button, "click", Qt::QueuedConnection);
        }
      } else if (dialog->objectName() == "CastConsolidationReview") {
        if (auto *table = dialog->findChild<QTableWidget *>())
          for (int row = 0; row < table->rowCount(); ++row) {
            const bool match = table->item(row, 2)->text().contains("Cast vector duplicate");
            table->item(row, 0)->setCheckState(match ? Qt::Checked : Qt::Unchecked);
            if (match)
              if (auto *keep = qobject_cast<QComboBox *>(table->cellWidget(row, 1)))
                keep->setCurrentIndex(keep->findText("Cast vector fixture"));
          }
        dialog->accept();
      } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        if (message->standardButtons().testFlag(QMessageBox::Yes)) {
          confirmed = true;
          message->button(expectRejected ? QMessageBox::No : QMessageBox::Yes)->click();
        } else {
          rejected = message->text().contains("different drawing, layer, palette, rig, or level metadata");
          message->reject();
        }
      }
    });
    CastBrowserActions::duplicates(window);
    require(reviewed && !driver.expired &&
                (expectRejected ? (rejected && !confirmed &&
                    TUndoManager::manager()->getCurrentHistoryIndex() == before)
                                : (confirmed && !rejected &&
                    TUndoManager::manager()->getCurrentHistoryIndex() == before + 1)),
            "Cast/vector consolidation did not enforce morph compatibility and explicit confirmation");
  };
  checkpoint("rejecting incompatible consolidation");
  consolidate(true);
  require(scene->getProperties()->getCastCollections() == collections,
          "Rejected vector consolidation changed collection membership");
  require(sheet->getCell(4, 2).getSimpleLevel() == duplicate &&
              scene->getLevelSet()->getLevel(duplicate->getName()) == duplicate,
          "Cast/vector incompatible consolidation changed asset identity or exposure");
  duplicate->setMorphHints(storedHints);
  checkpoint("consolidating compatible duplicates");
  consolidate(false);
  require(scene->getProperties()->getCastCollections() == consolidatedCollections,
          "Vector consolidation did not migrate and deduplicate collection membership");
  expectSource(Origin::LocalCopy);
  require(sheet->getCell(4, 2) == TXshCell(level, TFrameId(3)) &&
              !scene->getLevelSet()->getLevel(L"Cast vector duplicate"),
          "Cast/vector consolidation lost exposure frame IDs or retained duplicate");
  expectMetadata(level, QStringList(tags) << "duplicate-tag", storedHints);
  expectHints(level, 1, 3, true);
  expectDrawing(level, 3, 20., sourceProfile, .327);
  {
    const TFilePath path(localDir + "/consolidated.tnz");
    scene->saveCopy(path);
    ToonzScene restored;
    TProjectManager::instance()->initializeScene(&restored);
    restored.loadTnzFile(path);
    restored.loadResources(false);
    auto *survivor = restored.getLevelSet()->getLevel(level->getName());
    require(survivor && survivor->getCastId() == sourceId &&
                survivor->getLibraryProvenance() == level->getLibraryProvenance() &&
                restored.getProperties()->getCastCollections() == consolidatedCollections &&
                !restored.getLevelSet()->getLevel(L"Cast vector duplicate"),
            "Consolidated vector scene reload changed ownership or collection membership");
  }
  TUndoManager::manager()->undo();
  expectMetadata(level, tags, storedHints);
  require(scene->getProperties()->getCastCollections() == collections &&
              duplicate->getCastId() == duplicateId,
          "Vector consolidation undo did not restore exact collection membership and identity");
  require(sheet->getCell(4, 2).getSimpleLevel() == duplicate &&
              duplicate->getCastCover() == TFrameId(1) && duplicate->getMorphHints() == storedHints,
          "Cast/vector consolidation undo did not restore duplicate metadata");
  TUndoManager::manager()->redo();
  require(scene->getProperties()->getCastCollections() == consolidatedCollections,
          "Vector consolidation redo changed membership mapping");
  expectDrawing(level, 3, 20., sourceProfile, .327);
  TUndoManager::manager()->undo();

  // The real Clone Level command must preserve exact-copy metadata and prune
  // subset references only after all selected drawings have been cloned.
  checkpoint("cloning exact and subset levels");
  const Origin beforeClones = level->getLibraryProvenance();
  Origin linkedForClones = beforeClones;
  linkedForClones.sourceAssetPath = copied.getQString();
  linkedForClones.relationship = Origin::Linked;
  level->setLibraryProvenance(linkedForClones);
  require(level->getLibraryRelationship() == Origin::Linked,
          "Clone fixture must start with a live recorded source link");
  auto clone = [&](int lastRow, const QString &name) {
    const int before = TUndoManager::manager()->getCurrentHistoryIndex();
    bool accepted = false;
    DialogDriver driver([&](QDialog *dialog) {
      if (dialog->windowTitle() == "Clone Level") {
        if (auto *edit = dialog->findChild<QLineEdit *>()) {
          edit->setText(name);
          accepted = true;
          dialog->accept();
        }
      } else if (qobject_cast<QMessageBox *>(dialog)) dialog->reject();
    });
    TCellSelection selection;
    selection.selectCells(0, 0, lastRow, 0);
    selection.cloneLevel();
    auto *result = sheet->getCell(0, 1).getSimpleLevel();
    require(accepted && !driver.expired && result && result != level &&
                TUndoManager::manager()->getCurrentHistoryIndex() == before + 1,
            "Cast/vector native Clone Level did not create one undoable clone");
    return TXshSimpleLevelP(result);
  };
  auto exact = clone(2, "Cast_vector_exact");
  const QString exactId = exact->getCastId();
  Origin copyOrigin = linkedForClones;
  copyOrigin.relationship = Origin::LocalCopy;
  require(exactId != sourceId && exactId != duplicateId &&
              exact->getLibraryProvenance() == copyOrigin &&
              exact->getLibraryRelationship() == Origin::LocalCopy &&
              scene->getProperties()->getCastCollections() == collections,
          "Vector clone reused source identity, claimed a live Library link or joined source collections");
  expectMetadata(exact.getPointer(), tags, storedHints);
  expectHints(exact.getPointer(), 1, 3, true);
  expectDrawing(exact.getPointer(), 5, 40., sourceProfile, .327);
  TUndoManager::manager()->undo();
  require(!scene->getLevelSet()->getLevel(exact->getName()) && sheet->getCell(0, 1).isEmpty(),
          "Cast/vector clone undo retained the clone or exposures");
  TUndoManager::manager()->redo();
  require(sheet->getCell(0, 1).getSimpleLevel() == exact.getPointer() && exact->getCastId() == exactId &&
              exact->getLibraryProvenance() == copyOrigin,
          "Cast/vector clone redo changed clone identity");
  expectMetadata(exact.getPointer(), tags, storedHints);
  TUndoManager::manager()->undo();
  auto subset = clone(1, "Cast_vector_subset");
  require(subset->getCastId() != sourceId && subset->getCastId() != exactId &&
              subset->getLibraryProvenance() == copyOrigin,
          "Vector subset clone lost distinct identity or source attribution");
  QJsonArray subsetHints;
  require(subset->getCastTags() == tags && !subset->hasCastCover() && !subset->isFid(TFrameId(5)) &&
              MorphHints::decode(subset->getMorphHints(), subsetHints) && subsetHints.size() == 2,
          "Cast/vector subset clone retained an absent cover or morph endpoint");
  expectHints(subset.getPointer(), 1, 3, true);
  expectDrawing(subset.getPointer(), 3, 20., sourceProfile, .327);
  TUndoManager::manager()->undo();
  TUndoManager::manager()->redo();
  require(sheet->getCell(0, 1).getSimpleLevel() == subset.getPointer() && !subset->hasCastCover(),
          "Cast/vector subset redo changed identity or restored an absent cover");
  TUndoManager::manager()->undo();

  // Syntactically valid but obsolete geometry anchors must not be copied into
  // an exact clone, including stale zone members beyond the main anchor.
  checkpoint("pruning stale clone anchors");
  QJsonArray valid;
  require(MorphHints::decode(storedHints, valid), "Cast/vector hint fixture invalid");
  auto stalePair = valid[0].toObject();
  auto staleAnchor = stalePair["first"].toObject();
  staleAnchor["style"] = 999;
  stalePair["first"] = staleAnchor;
  auto staleZone = valid[1].toObject();
  staleZone["firstMembers"] = QJsonArray{staleAnchor};
  valid.append(stalePair);
  valid.append(staleZone);
  level->setMorphHints(MorphHints::encode(valid));
  auto validated = clone(2, "Cast_vector_validated");
  expectMetadata(validated.getPointer(), tags, storedHints);
  expectHints(validated.getPointer(), 1, 3, true);
  TUndoManager::manager()->undo();
  level->setMorphHints(storedHints);

  // Merge TLV's no-save cloning shares this production operation. Work in a
  // nested xsheet, reuse an empty column with existing settings/keys, insert
  // beside an occupied column, and then undo while the parent sheet is active.
  checkpoint("cloning nested levels without saving");
  {
    TXshChildLevelP child = new TXshChildLevel(L"Cast vector nested clone");
    child->setScene(scene);
    scene->getLevelSet()->insertLevel(child.getPointer());
    sheet->setCell(8, 5, TXshCell(child.getPointer(), TFrameId(1)));
    auto *nested = child->getXsheet();
    for (int row = 0; row < 3; ++row) {
      nested->setCell(row, 0, TXshCell(level, TFrameId(row < 2 ? 1 : 3)));
      nested->setCell(row, 1, TXshCell(level, TFrameId(1 + row * 2)));
    }
    nested->insertColumn(2);
    auto *emptyColumn = nested->getColumn(2);
    emptyColumn->setOpacity(73);
    auto *emptyStage = nested->getStageObject(TStageObjectId::ColumnId(2));
    auto *emptyX = emptyStage->getParam(TStageObject::T_X);
    require(emptyX->setValue(6., 42.) && emptyX->isKeyframe(6.) &&
                emptyStage->getParam(TStageObject::T_X, 6.) == 42.,
            "Cast/vector destination key fixture was not created");
    emptyStage->setName("Existing destination keys");
    nested->setCell(0, 3, TXshCell(duplicate, TFrameId(3)));
    auto *guardColumn = nested->getColumn(3);
    const TXshCell parentCell = sheet->getCell(0, 0);
    const int levelCount = scene->getLevelSet()->getLevelCount();
    const int history = TUndoManager::manager()->getCurrentHistoryIndex();
    clean(level);
    TCellSelection::Range range;
    range.m_r0 = 0; range.m_r1 = 2;
    range.m_c0 = 0; range.m_c1 = 1;
    struct RestoreSheet {
      TXsheetHandle *handle;
      TXsheet *sheet;
      bool linkNames;
      ~RestoreSheet() {
        handle->setXsheet(sheet);
        Preferences::instance()->setValue(linkColumnNameWithLevel, linkNames);
      }
    } restore{app->getCurrentXsheet(), sheet,
              Preferences::instance()->isLinkColumnNameWithLevelEnabled()};
    Preferences::instance()->setValue(linkColumnNameWithLevel, true);
    app->getCurrentXsheet()->setXsheet(nested);
    emptyColumn->lock(true);
    require(!MatchlineCommands::cloneLevels(range, "Cast_vector_locked_no_save") &&
                scene->getLevelSet()->getLevelCount() == levelCount &&
                TUndoManager::manager()->getCurrentHistoryIndex() == history &&
                nested->getColumn(2) == emptyColumn && nested->getColumn(3) == guardColumn &&
                nested->getCell(0, 2).isEmpty() && emptyColumn->isLocked() &&
                emptyStage->getName() == "Existing destination keys",
            "Cast/vector locked no-save destination was not rejected before mutation");
    emptyColumn->lock(false);
    require(MatchlineCommands::cloneLevels(range, "Cast_vector_no_save"),
            "Cast/vector unlocked no-save clone was rejected");
    TXshSimpleLevelP noSave = nested->getCell(0, 2).getSimpleLevel();
    require(noSave && noSave.getPointer() != level &&
                TUndoManager::manager()->getCurrentHistoryIndex() == history + 1,
            "Cast/vector no-save clone did not create one undoable level");
    const auto cloneName = noSave->getName();
    const QString noSaveId = noSave->getCastId();
    require(noSaveId != sourceId && noSaveId != exactId &&
                noSave->getLibraryProvenance() == copyOrigin &&
                !noSave->sourceExpectedOnDisk(),
            "Unsaved vector clone reused identity, live linkage or saved-source state");
    const auto clonedColumnName = emptyStage->getName();
    app->getCurrentXsheet()->setXsheet(sheet);
    auto expectNoSave = [&]() {
      require(scene->getLevelSet()->getLevelCount() == levelCount + 1 &&
                  scene->getLevelSet()->getLevel(cloneName) == noSave.getPointer() &&
                  nested->getCell(0, 2) == TXshCell(noSave.getPointer(), TFrameId(1)) &&
                  nested->getCell(1, 2) == TXshCell(noSave.getPointer(), TFrameId(1)) &&
                  nested->getCell(2, 2) == TXshCell(noSave.getPointer(), TFrameId(3)) &&
                  nested->getCell(2, 3) == TXshCell(noSave.getPointer(), TFrameId(5)) &&
                  nested->getColumn(4) == guardColumn &&
                  nested->getCell(0, 4) == TXshCell(duplicate, TFrameId(3)) &&
                  emptyStage->getName() == clonedColumnName,
              "Cast/vector no-save redo changed clone identity, holds or neighboring column");
      expectMetadata(noSave.getPointer(), tags, storedHints);
      require(noSave->getCastId() == noSaveId && noSave->getLibraryProvenance() == copyOrigin &&
                  !noSave->sourceExpectedOnDisk(),
              "Repeated no-save clone redo lost attribution, identity or unsaved state");
      expectHints(noSave.getPointer(), 1, 3, true);
      expectDrawing(noSave.getPointer(), 5, 40., sourceProfile, .327);
    };
    auto expectSourceAndEmptyColumn = [&]() {
      require(sheet->getCell(0, 0) == parentCell,
              "Cast/vector no-save clone changed the parent-sheet exposure");
      require(nested->getCell(0, 0) == TXshCell(level, TFrameId(1)),
              "Cast/vector no-save clone changed the nested source exposure");
      require(nested->getColumn(2) == emptyColumn,
              "Cast/vector no-save clone replaced the reused destination column");
      require(emptyColumn->getOpacity() == 73,
              "Cast/vector no-save clone changed destination column opacity");
      require(nested->getStageObject(TStageObjectId::ColumnId(2)) == emptyStage,
              "Cast/vector no-save clone replaced the destination stage object");
      require(emptyStage->getParam(TStageObject::T_X) == emptyX &&
                  emptyX->getKeyframeCount() == 1 && emptyX->isKeyframe(6.),
              "Cast/vector no-save clone changed the destination X key identity or frame");
      require(emptyStage->getParam(TStageObject::T_X, 6.) == 42.,
              "Cast/vector no-save clone changed the destination X key value");
      require(!level->getProperties()->getDirtyFlag(),
              "Cast/vector no-save clone marked the source drawings dirty");
      expectMetadata(level, tags, storedHints);
      expectDrawing(level, 1, 0., sourceProfile, .327);
    };
    expectNoSave();
    emptyColumn->lock(true);
    for (int cycle = 0; cycle != 3; ++cycle) {
      require(TUndoManager::manager()->undo(), "Cast/vector no-save undo missing");
      require(scene->getLevelSet()->getLevelCount() == levelCount &&
                  !scene->getLevelSet()->getLevel(cloneName) &&
                  nested->getCell(0, 2).isEmpty() && nested->getCell(2, 2).isEmpty() &&
                  nested->getColumn(3) == guardColumn && emptyColumn->isLocked() &&
                  emptyStage->getName() == "Existing destination keys",
              "Cast/vector repeated no-save undo retained a generated clone or shifted neighbor");
      expectSourceAndEmptyColumn();
      require(TUndoManager::manager()->redo(), "Cast/vector no-save redo missing");
      expectNoSave();
      expectSourceAndEmptyColumn();
      require(emptyColumn->isLocked(), "Cast/vector no-save redo lost the current column lock");
    }
    require(TUndoManager::manager()->undo(), "Cast/vector final no-save undo missing");
    expectSourceAndEmptyColumn();
    app->getCurrentXsheet()->setXsheet(nested);
    range.m_c0 = range.m_c1 = 2;
    require(!MatchlineCommands::cloneLevels(range, "Cast_vector_empty_no_save"),
            "Cast/vector empty no-save clone reported an edit");
    require(TUndoManager::manager()->getCurrentHistoryIndex() == history &&
                scene->getLevelSet()->getLevelCount() == levelCount &&
                nested->getColumn(2) == emptyColumn && nested->getColumn(3) == guardColumn,
            "Cast/vector empty no-save clone changed history or columns");
  }

  checkpoint("checking missing clone destinations and preparation rollback");
  {
    TXshChildLevelP child = new TXshChildLevel(L"Cast vector missing destinations");
    child->setScene(scene);
    scene->getLevelSet()->insertLevel(child.getPointer());
    sheet->setCell(9, 5, TXshCell(child.getPointer(), TFrameId(1)));
    auto *nested = child->getXsheet();
    struct RestoreSheet {
      TXsheetHandle *handle; TXsheet *sheet;
      ~RestoreSheet() { handle->setXsheet(sheet); }
    } restore{app->getCurrentXsheet(), sheet};
    app->getCurrentXsheet()->setXsheet(nested);
    TCellSelection::Range range;
    range.m_r0 = 0; range.m_r1 = 2; range.m_c0 = 0; range.m_c1 = 2;
    const int emptyColumns = nested->getColumnCount();
    const int emptyStageObjects = nested->getStageObjectTree()->getStageObjectCount();
    const int levelCount = scene->getLevelSet()->getLevelCount();
    const int history = TUndoManager::manager()->getCurrentHistoryIndex();
    require(!MatchlineCommands::cloneLevels(range, "Cast_vector_absent_no_op") &&
                nested->getColumnCount() == emptyColumns &&
                nested->getStageObjectTree()->getStageObjectCount() == emptyStageObjects &&
                scene->getLevelSet()->getLevelCount() == levelCount &&
                TUndoManager::manager()->getCurrentHistoryIndex() == history,
            "Cast/vector empty clone created absent columns or stage objects");
    nested->setCell(0, 0, TXshCell(level, TFrameId(1)));
    nested->setCell(1, 0, TXshCell(level, TFrameId(999)));
    // setCell can leave valid source-column stage data lazy. Materialize and
    // author that existing node before comparing counts with newly added nodes.
    auto *sourceStage = nested->getStageObject(TStageObjectId::ColumnId(0));
    auto *sourceX = sourceStage->getParam(TStageObject::T_X);
    require(sourceX->setValue(6., 12.), "Cast/vector source key fixture failed");
    const int originalColumns = nested->getColumnCount();
    const int originalStageObjects = nested->getStageObjectTree()->getStageObjectCount();
    require(!MatchlineCommands::cloneLevels(range, "Cast_vector_missing_drawing") &&
                nested->getColumnCount() == originalColumns &&
                nested->getStageObjectTree()->getStageObjectCount() == originalStageObjects &&
                scene->getLevelSet()->getLevelCount() == levelCount &&
                TUndoManager::manager()->getCurrentHistoryIndex() == history &&
                nested->getCell(1, 0) == TXshCell(level, TFrameId(999)),
            "Cast/vector later missing drawing left a partial clone");
    nested->setCell(1, 0, TXshCell());
    require(MatchlineCommands::cloneLevels(range, "Cast_vector_new_columns"),
            "Cast/vector clone into missing destination failed");
    TXshSimpleLevelP generated = nested->getCell(0, 3).getSimpleLevel();
    auto *generatedColumn = nested->getColumn(3);
    auto *generatedStage = nested->getStageObject(TStageObjectId::ColumnId(3));
    TDoubleParamP generatedX = generatedStage->getParam(TStageObject::T_X);
    require(generated && nested->getColumnCount() == 4,
            "Cast/vector clone did not create required intermediate columns");
    // A later Function Editor undo entry owns the exact curve, not a column ID.
    // Recreating a default stage during clone redo would detach that later edit.
    TDoubleParamP authored(dynamic_cast<TDoubleParam *>(generatedX->clone()));
    require(bool(authored), "Cast/vector cloned-column parameter copy lost numeric type");
    require(authored->setValue(6., 23.), "Cast/vector cloned-column key fixture failed");
    QString curveError;
    require(FunctionCurveOperations::apply(
                FunctionCurveOperations::planRestore({{generatedX, authored}}),
                "Cloned column curve fixture", curveError),
            "Cast/vector cloned-column Function Editor edit failed");
    for (int cycle = 0; cycle != 2; ++cycle) {
      require(TUndoManager::manager()->undo() && generatedX->getKeyframeCount() == 0,
              "Cast/vector cloned-column curve undo failed");
      require(TUndoManager::manager()->undo() &&
                  nested->getColumnCount() == originalColumns &&
                  nested->getStageObjectTree()->getStageObjectCount() == originalStageObjects &&
                  scene->getLevelSet()->getLevelCount() == levelCount,
              "Cast/vector clone undo retained newly created columns or stage objects");
      for (int col = 1; col != 4; ++col)
        require(!nested->getStageObjectTree()->getStageObject(TStageObjectId::ColumnId(col), false),
                "Cast/vector clone undo retained an absent destination stage");
      require(nested->getStageObject(TStageObjectId::ColumnId(0)) == sourceStage &&
                  sourceStage->getParam(TStageObject::T_X) == sourceX &&
                  sourceX->isKeyframe(6.) && sourceX->getValue(6.) == 12.,
              "Cast/vector clone undo replaced or changed the authored source stage");
      require(TUndoManager::manager()->redo() &&
                  nested->getColumnCount() == 4 && nested->getColumn(3) == generatedColumn &&
                  nested->getCell(0, 3) == TXshCell(generated.getPointer(), TFrameId(1)) &&
                  scene->getLevelSet()->getLevelCount() == levelCount + 1,
              "Cast/vector missing-destination redo changed original clone identities");
      require(nested->getStageObject(TStageObjectId::ColumnId(3)) == generatedStage &&
                  generatedStage->getParam(TStageObject::T_X) == generatedX.getPointer() &&
                  generatedX->getKeyframeCount() == 0,
              "Cast/vector clone redo replaced the stage or its original curve");
      require(TUndoManager::manager()->redo() && generatedX->isKeyframe(6.) &&
                  nested->getStageObject(TStageObjectId::ColumnId(3))
                      ->getParam(TStageObject::T_X, 6.) == 23.,
              "Cast/vector Function Editor redo edited a detached clone curve");
      expectDrawing(generated.getPointer(), 1, 0., sourceProfile, .327);
    }
    require(TUndoManager::manager()->undo(), "Cast/vector final cloned-column key undo missing");
    require(TUndoManager::manager()->undo(), "Cast/vector missing-destination final undo missing");
  }

  // Filmstrip renumber and native undo remap Cast covers, both morph endpoint
  // pairs and zones, and exposures without touching per-stroke envelopes.
  require(level->getLibraryProvenance() == linkedForClones &&
              level->getLibraryRelationship() == Origin::Linked,
          "Cloning detached the original Library source instead of the new drawings");
  level->setLibraryProvenance(beforeClones);
  checkpoint("renumbering covers, morph hints and exposures");
  FilmstripCmd::renumber(level, {{TFrameId(1), TFrameId(11)},
                                {TFrameId(3), TFrameId(13)},
                                {TFrameId(5), TFrameId(15)}}, true);
  const auto renumberedHints = MorphHints::renumber(storedHints,
      {{TFrameId(1), TFrameId(11)}, {TFrameId(3), TFrameId(13)}, {TFrameId(5), TFrameId(15)}});
  expectMetadata(level, tags, renumberedHints, 15);
  expectSource(Origin::LocalCopy);
  expectHints(level, 11, 13, true);
  expectDrawing(level, 11, 0., sourceProfile, .327);
  require(sheet->getCell(2, 0) == TXshCell(level, TFrameId(15)),
          "Cast/vector renumber did not update exposures");
  TUndoManager::manager()->undo();
  expectMetadata(level, tags, storedHints);
  expectHints(level, 1, 3, true);
  expectDrawing(level, 5, 40., sourceProfile, .327);
  expectSource(Origin::LocalCopy);
  require(sheet->getCell(2, 0) == TXshCell(level, TFrameId(5)),
          "Cast/vector renumber undo lost exposure IDs");
  TUndoManager::manager()->redo();
  expectMetadata(level, tags, renumberedHints, 15);
  expectHints(level, 11, 13, true);
  TUndoManager::manager()->undo();
  require(scene->getProperties()->getCastCollections() == collections,
          "Clone or renumber workflows changed saved collection membership");

  // Leave no fixture referring to temporary files for the following workflows.
  checkpoint("releasing fixture resources");
  if (!resetNativeUiIntegrationScene(&resetError))
    throw std::runtime_error(resetError.toStdString());
  QApplication::processEvents();
  std::cout << "cast-vector-native: PASS scene/PLI persistence, relink/local copy/consolidation, "
               "exact/subset validated clones, nested no-save redo identity, envelopes and renumber undo\n";
}
