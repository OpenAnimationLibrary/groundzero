#include "castvectorintegrationtest.h"
#include "castbrowseractions.h"
#include "cellselection.h"
#include "filmstripcommand.h"
#include "iocommand.h"
#include "tapp.h"
#include "tools/strokeselection.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/morphhints.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "tlevel_io.h"
#include "tpalette.h"
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
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
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
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        dialog->reject();
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
  app->getCurrentScene()->setDirtyFlag(false);
  TUndoManager::manager()->reset();
  IoCmd::newScene();
  QApplication::processEvents();
  auto *scene = app->getCurrentScene()->getScene();
  QTemporaryDir temporary;
  require(temporary.isValid(), "Cast/vector temporary directory unavailable");
  const QString originalDir = temporary.path() + "/external";
  const QString replacementDir = temporary.path() + "/replacement";
  const QString localDir = temporary.path() + "/local";
  for (const auto &dir : {originalDir, replacementDir, localDir})
    require(QDir().mkpath(dir), "Cast/vector cannot create fixture folders");
  writeLevel(originalDir + "/figure.pli", 0., sourceProfile, .327);
  writeLevel(replacementDir + "/figure.pli", 100., replacementProfile, .7);
  scene->setScenePath(TFilePath(localDir + "/scene.tnz"));
  auto *level = scene->createNewLevel(PLI_XSHLEVEL, L"Cast vector fixture")->getSimpleLevel();
  level->setPath(TFilePath(originalDir + "/figure.pli"));
  level->load();
  clean(level);
  app->getCurrentLevel()->setLevel(level);
  const QStringList tags{QString::fromUtf8("hero λ"), "review \"ink\" & line"};
  const std::string storedHints = hints(level);
  level->setCastTags(tags);
  level->setCastCover(TFrameId(5));
  level->setMorphHints(storedHints);
  auto *sheet = scene->getXsheet();
  for (int row = 0; row < 3; ++row)
    sheet->setCell(row, 0, TXshCell(level, TFrameId(1 + row * 2)));
  expectDrawing(level, 1, 0., sourceProfile, .327);
  expectHints(level, 1, 3, true);

  // Native scene attributes and native PLI envelope data must survive together.
  const TFilePath saved(localDir + "/saved.tnz");
  scene->saveCopy(saved);
  {
    ToonzScene loaded;
    TProjectManager::instance()->initializeScene(&loaded);
    loaded.loadTnzFile(saved);
    loaded.loadResources(false);
    auto *asset = loaded.getLevelSet()->getLevel(level->getName());
    require(asset && asset->getSimpleLevel(), "Cast/vector scene reload lost level identity");
    auto *reopened = asset->getSimpleLevel();
    expectMetadata(reopened, tags, storedHints);
    expectHints(reopened, 1, 3, true);
    expectDrawing(reopened, 1, 0., sourceProfile, .327);
    expectDrawing(reopened, 5, 40., sourceProfile, .327);
  }

  NativeDialogsGuard dialogs;
  const TFilePath original = level->getPath();
  {
    QString failure;
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        files->setDirectory(replacementDir);
        files->selectFile(replacementDir);
        dialog->accept();
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
  expectMetadata(level, tags, storedHints);
  expectDrawing(level, 1, 100., replacementProfile, .7);
  expectHints(level, 1, 3, false);
  TUndoManager::manager()->undo();
  expectMetadata(level, tags, storedHints);
  expectDrawing(level, 1, 0., sourceProfile, .327);
  expectHints(level, 1, 3, true);
  TUndoManager::manager()->redo();
  level->invalidateFrames();
  expectDrawing(level, 5, 140., replacementProfile, .7);
  expectHints(level, 1, 3, false);
  TUndoManager::manager()->undo();
  level->invalidateFrames();
  expectDrawing(level, 5, 40., sourceProfile, .327);
  expectHints(level, 1, 3, true);

  // Copying external native files retains envelopes and valid authoring hints;
  // undo restores links while leaving the explicitly published files intact.
  require(CastBrowserActions::external(scene, level), "Cast/vector source is not external");
  {
    QString failure;
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        files->setDirectory(localDir);
        files->selectFile(localDir);
        dialog->accept();
        showProgressNextTurn("Preparing local asset copies…");
      } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        if (message->standardButtons().testFlag(QMessageBox::Yes))
          message->done(QMessageBox::Yes);
        else { failure = message->text(); message->reject(); }
      }
    });
    CastBrowserActions::makeLocal(window, {level});
    require(!driver.expired && failure.isEmpty() && !CastBrowserActions::external(scene, level),
            "Cast/vector native local copy did not apply");
  }
  const TFilePath copied = scene->decodeFilePath(level->getPath());
  require(QFileInfo::exists(copied.getQString()), "Cast/vector copied PLI does not exist");
  expectMetadata(level, tags, storedHints);
  expectHints(level, 1, 3, true);
  level->invalidateFrames();
  expectDrawing(level, 1, 0., sourceProfile, .327);
  TUndoManager::manager()->undo();
  require(level->getPath() == original && QFileInfo::exists(copied.getQString()),
          "Cast/vector local-copy undo lost original link or removed copied file");
  expectMetadata(level, tags, storedHints);
  TUndoManager::manager()->redo();
  require(scene->decodeFilePath(level->getPath()) == copied,
          "Cast/vector local-copy redo created a new destination");
  level->invalidateFrames();
  expectDrawing(level, 3, 20., sourceProfile, .327);
  expectHints(level, 1, 3, true);

  // Matching PLI bytes alone are insufficient: a different scene-owned morph
  // plan must block consolidation. Once plans match, the explicit reviewed
  // command preserves the chosen cover, envelopes and undoable exposures.
  auto *duplicate = scene->createNewLevel(PLI_XSHLEVEL, L"Cast vector duplicate")->getSimpleLevel();
  duplicate->setPath(copied);
  duplicate->load();
  duplicate->setPalette(level->getPalette());
  duplicate->clonePropertiesFrom(level);
  duplicate->setCastTags({"duplicate-tag"});
  duplicate->setCastCover(TFrameId(1));
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
          button->click();
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
          message->done(expectRejected ? QMessageBox::No : QMessageBox::Yes);
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
  consolidate(true);
  require(sheet->getCell(4, 2).getSimpleLevel() == duplicate &&
              scene->getLevelSet()->getLevel(duplicate->getName()) == duplicate,
          "Cast/vector incompatible consolidation changed asset identity or exposure");
  duplicate->setMorphHints(storedHints);
  consolidate(false);
  require(sheet->getCell(4, 2) == TXshCell(level, TFrameId(3)) &&
              !scene->getLevelSet()->getLevel(L"Cast vector duplicate"),
          "Cast/vector consolidation lost exposure frame IDs or retained duplicate");
  expectMetadata(level, QStringList(tags) << "duplicate-tag", storedHints);
  expectHints(level, 1, 3, true);
  expectDrawing(level, 3, 20., sourceProfile, .327);
  TUndoManager::manager()->undo();
  expectMetadata(level, tags, storedHints);
  require(sheet->getCell(4, 2).getSimpleLevel() == duplicate &&
              duplicate->getCastCover() == TFrameId(1) && duplicate->getMorphHints() == storedHints,
          "Cast/vector consolidation undo did not restore duplicate metadata");
  TUndoManager::manager()->redo();
  expectDrawing(level, 3, 20., sourceProfile, .327);
  TUndoManager::manager()->undo();

  // The real Clone Level command must preserve exact-copy metadata and prune
  // subset references only after all selected drawings have been cloned.
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
  expectMetadata(exact.getPointer(), tags, storedHints);
  expectHints(exact.getPointer(), 1, 3, true);
  expectDrawing(exact.getPointer(), 5, 40., sourceProfile, .327);
  TUndoManager::manager()->undo();
  require(!scene->getLevelSet()->getLevel(exact->getName()) && sheet->getCell(0, 1).isEmpty(),
          "Cast/vector clone undo retained the clone or exposures");
  TUndoManager::manager()->redo();
  require(sheet->getCell(0, 1).getSimpleLevel() == exact.getPointer(),
          "Cast/vector clone redo changed clone identity");
  expectMetadata(exact.getPointer(), tags, storedHints);
  TUndoManager::manager()->undo();
  auto subset = clone(1, "Cast_vector_subset");
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

  // Filmstrip renumber and native undo remap Cast covers, both morph endpoint
  // pairs and zones, and exposures without touching per-stroke envelopes.
  FilmstripCmd::renumber(level, {{TFrameId(1), TFrameId(11)},
                                {TFrameId(3), TFrameId(13)},
                                {TFrameId(5), TFrameId(15)}}, true);
  const auto renumberedHints = MorphHints::renumber(storedHints,
      {{TFrameId(1), TFrameId(11)}, {TFrameId(3), TFrameId(13)}, {TFrameId(5), TFrameId(15)}});
  expectMetadata(level, tags, renumberedHints, 15);
  expectHints(level, 11, 13, true);
  expectDrawing(level, 11, 0., sourceProfile, .327);
  require(sheet->getCell(2, 0) == TXshCell(level, TFrameId(15)),
          "Cast/vector renumber did not update exposures");
  TUndoManager::manager()->undo();
  expectMetadata(level, tags, storedHints);
  expectHints(level, 1, 3, true);
  expectDrawing(level, 5, 40., sourceProfile, .327);
  require(sheet->getCell(2, 0) == TXshCell(level, TFrameId(5)),
          "Cast/vector renumber undo lost exposure IDs");
  TUndoManager::manager()->redo();
  expectMetadata(level, tags, renumberedHints, 15);
  expectHints(level, 11, 13, true);
  TUndoManager::manager()->undo();

  // Leave no fixture referring to temporary files for the following workflows.
  app->getCurrentScene()->setDirtyFlag(false);
  TUndoManager::manager()->reset();
  IoCmd::newScene();
  QApplication::processEvents();
  std::cout << "cast-vector-native: PASS scene/PLI persistence, relink/local copy/consolidation, "
               "exact/subset validated clones, envelopes and renumber undo\n";
}
